"""Shared raw WebSocket client and isolated process for RPC contract tests."""

import base64
import hashlib
import ipaddress
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from urllib.parse import urlsplit

DEADLINE = 10.0
TOKEN = "Transport_Test-0123456789"
READY = b"MRTK-RPC-READY "
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
KEY = "dGhlIHNhbXBsZSBub25jZQ=="
OFFER = f"mrtk-rpc, mrtk-token.{TOKEN}"
CONFIG = """[positioning]
mode = "single"
[console]
passwd = ""
[streams.input.rover]
type = "off"
[streams.input.base]
type = "off"
[streams.input.correction]
type = "off"
[streams.output.stream1]
type = "off"
[streams.output.stream2]
type = "off"
[streams.log.stream1]
type = "off"
[streams.log.stream2]
type = "off"
[streams.log.stream3]
type = "off"
"""


def require(condition, description):
    """Raise a useful assertion without exposing request headers or tokens."""
    if not condition:
        raise AssertionError(description)


def wait_for(predicate, description):
    """Wait within the common deadline for a process or file transition."""
    end = time.monotonic() + DEADLINE
    while time.monotonic() < end:
        value = predicate()
        if value:
            return value
        time.sleep(0.01)
    raise AssertionError(f"deadline waiting for {description}")


class WebSocket:
    """Minimal masked RFC 6455 client with independently verified handshake."""

    def __init__(
        self,
        server,
        protocols=OFFER,
        origin=None,
        path="/rpc",
        upgrade=True,
        method="GET",
        headers=(),
        body=b"",
        timeout=DEADLINE,
    ):
        """Connect and finish one raw HTTP handshake."""
        self.sock = socket.create_connection((server.host, server.port), timeout=timeout)
        server.sockets.append(self.sock)
        lines = [f"{method} {path} HTTP/1.1", "Host: localhost"]
        if upgrade:
            lines += [
                "Upgrade: websocket",
                "Connection: Upgrade",
                f"Sec-WebSocket-Key: {KEY}",
                "Sec-WebSocket-Version: 13",
            ]
        if isinstance(protocols, str):
            protocols = [protocols]
        for value in protocols or []:
            lines.append(f"Sec-WebSocket-Protocol: {value}")
        if origin is not None:
            lines.append(f"Origin: {origin}")
        if method == "PUT":
            lines.append("Content-Length: 0")
        lines.extend(headers)
        self.sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode("ascii") + body)
        raw = bytearray()
        while not raw.endswith(b"\r\n\r\n"):
            byte = self.sock.recv(1)
            require(bool(byte), "HTTP connection closed before response headers")
            raw.extend(byte)
            require(len(raw) < 65536, "unbounded HTTP response headers")
        lines = raw.decode("latin1").split("\r\n")
        self.status = int(lines[0].split()[1])
        self.headers = {}
        for line in lines[1:]:
            if ":" in line:
                name, value = line.split(":", 1)
                self.headers.setdefault(name.lower(), []).append(value.strip())
        if self.status == 101:
            selected = self.headers.get("sec-websocket-protocol")
            require(selected == ["mrtk-rpc"], "upgrade must select only mrtk-rpc")
            accept = base64.b64encode(hashlib.sha1((KEY + GUID).encode()).digest()).decode()
            require(
                self.headers.get("sec-websocket-accept") == [accept],
                "incorrect WebSocket accept key",
            )

    def exact(self, length):
        """Read exactly one field without consuming the following frame."""
        raw = bytearray()
        while len(raw) < length:
            chunk = self.sock.recv(length - len(raw))
            require(bool(chunk), "WebSocket EOF without expected frame")
            raw.extend(chunk)
        return bytes(raw)

    def send(self, payload, opcode=1, final=True, rsv=0):
        """Send one client frame, including a mask even for empty payloads."""
        if isinstance(payload, str):
            payload = payload.encode("utf-8")
        length = len(payload)
        header = bytes([(0x80 if final else 0) | rsv | opcode])
        if length < 126:
            header += bytes([0x80 | length])
        elif length < 65536:
            header += bytes([0xFE]) + struct.pack("!H", length)
        else:
            header += bytes([0xFF]) + struct.pack("!Q", length)
        mask = b"\x12\x34\x56\x78"
        self.sock.sendall(
            header + mask + bytes(value ^ mask[i % 4] for i, value in enumerate(payload))
        )

    def receive(self):
        """Return the opcode and payload, answering any server ping first."""
        while True:
            header = self.exact(2)
            require(header[0] & 0x70 == 0, "unexpected reserved frame bits")
            require(header[0] & 0x80, "unexpected fragmented server response")
            require(not header[1] & 0x80, "server frames must be unmasked")
            length = header[1] & 0x7F
            if length == 126:
                length = struct.unpack("!H", self.exact(2))[0]
            elif length == 127:
                length = struct.unpack("!Q", self.exact(8))[0]
            require(length <= 4 * 1024 * 1024, "unbounded server frame")
            payload = self.exact(length)
            opcode = header[0] & 0x0F
            if opcode != 9:
                return opcode, payload
            self.send(payload, opcode=10)

    def response(self):
        """Read one JSON-RPC text response."""
        opcode, raw = self.receive()
        require(opcode == 1, f"expected JSON response, received opcode {opcode}")
        result = json.loads(raw)
        require(result.get("jsonrpc") == "2.0", "response must identify JSON-RPC 2.0")
        require("id" in result, "response must contain id")
        require(
            set(result) in ({"jsonrpc", "id", "result"}, {"jsonrpc", "id", "error"}),
            "response envelope has unexpected members",
        )
        return result, raw

    def request(self, method="getVersion", request_id=1, **members):
        """Send a normal request and read its response."""
        self.send(json.dumps(dict(jsonrpc="2.0", method=method, id=request_id, **members)))
        return self.response()[0]

    def close_code(self, expected):
        """Check the server close status and acknowledge its close frame."""
        opcode, payload = self.receive()
        require(opcode == 8 and len(payload) >= 2, "expected close frame with status")
        actual = struct.unpack("!H", payload[:2])[0]
        require(actual == expected, f"expected close {expected}, received {actual}")
        try:
            self.send(payload, opcode=8)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def no_response(self):
        """Check a quiet interval and restore the regular read deadline."""
        self.sock.settimeout(0.2)
        try:
            with unittest.TestCase().assertRaises(socket.timeout):
                self.sock.recv(1)
        finally:
            self.sock.settimeout(DEADLINE)

    def close(self):
        """Release the worker occupied by this client."""
        if self.status == 101:
            try:
                self.send(struct.pack("!H", 1000), opcode=8)
            except (BrokenPipeError, ConnectionResetError, OSError):
                pass
        self.sock.close()


class Server:
    """Own an isolated process, sockets, standard streams, and temporary files."""

    def __init__(self, args, options=None, token=TOKEN, ready=True, config=CONFIG):
        """Record the startup scenario without launching a child yet."""
        self.args = args
        self.options = options if options is not None else ["--rpc-port", "0", "--no-console"]
        self.token = token
        self.expect_ready = ready
        self.config = config
        self.sockets = []

    def __enter__(self):
        """Launch the isolated child and optionally await readiness."""
        self.temp = tempfile.TemporaryDirectory(prefix="rpc_transport_")
        self.directory = Path(self.temp.name)
        config = self.config(self.directory) if callable(self.config) else self.config
        (self.directory / "run.toml").write_text(config, encoding="utf-8")
        self.stderr = (self.directory / "stderr").open("w+b")
        self.stdout = (self.directory / "stdout").open("w+b")
        env = os.environ.copy()
        env.pop("MRTK_RPC_TOKEN", None)
        if self.token is not None:
            env["MRTK_RPC_TOKEN"] = self.token
        try:
            self.process = subprocess.Popen(
                [str(self.args.mrtk), "run", "-o", "run.toml", *self.options],
                cwd=self.directory,
                env=env,
                stdin=subprocess.PIPE,
                stdout=self.stdout,
                stderr=self.stderr,
            )
            if self.expect_ready:
                wait_for(self.read_ready, "RPC ready line")
            return self
        except BaseException:
            self.__exit__(*sys.exc_info())
            raise

    def output(self, name="stderr"):
        """Read captured output independently of the child's file position."""
        return (self.directory / name).read_bytes()

    def read_ready(self):
        """Parse a complete ready line and validate its URL."""
        lines = [line for line in self.output().splitlines(keepends=True) if line.startswith(READY)]
        require(len(lines) <= 1, "more than one RPC ready line")
        if not lines or not lines[0].endswith(b"\n"):
            require(self.process.poll() is None, "process exited before RPC ready line")
            return False
        self.ready_line = lines[0]
        obj = json.loads(self.ready_line[len(READY) :])
        require(set(obj) == {"url"}, "ready line must contain the URL object")
        self.url = obj["url"]
        parsed = urlsplit(self.url)
        require(parsed.scheme == "ws" and parsed.path == "/rpc", "incorrect ready URL")
        require(not parsed.query and not parsed.fragment, "ready URL contains query or fragment")
        require(
            ipaddress.ip_address(parsed.hostname).is_loopback, "incorrect ready loopback address"
        )
        require(parsed.port and 0 < parsed.port < 65536, "port zero was not resolved")
        self.host, self.port = parsed.hostname, parsed.port
        return True

    def wait(self, expected=0):
        """Require the process to finish through its normal exit path."""
        actual = self.process.wait(timeout=DEADLINE)
        require(actual == expected, f"expected exit {expected}, received {actual}")
        if self.expect_ready:
            require(self.output().count(READY) == 1, "ready line count changed at shutdown")
        return actual

    def refuse(self, message):
        """Require a failed startup with no ready line and a useful diagnostic."""
        status = self.process.wait(timeout=DEADLINE)
        require(status != 0, "invalid startup unexpectedly succeeded")
        output = self.output()
        require(READY not in output, "failed startup printed ready line")
        require(message.lower().encode() in output.lower(), f"missing diagnostic naming {message}")

    def connect(self, **kwargs):
        """Open an authenticated connection, or return a rejected handshake."""
        return WebSocket(self, **kwargs)

    def __exit__(self, exc_type, exc, tb):
        """Reap the child on every failure and remove its private files."""
        for sock in self.sockets:
            sock.close()
        if hasattr(self, "process"):
            if self.process.poll() is None:
                self.process.kill()
            self.process.wait(timeout=DEADLINE)
            self.process.stdin.close()
        if exc_type:
            # Diagnostics from a conforming server cannot include the secret. Redact
            # here too so the test failure remains safe when checking that invariant.
            output = self.output().decode(errors="replace")
            if self.token:
                output = output.replace(self.token, "<redacted>")
            print(f"mrtk stderr:\n{output}", file=sys.stderr)
        output = self.output() + self.output("stdout")
        self.stderr.close()
        self.stdout.close()
        self.temp.cleanup()
        if exc_type is None and self.token:
            require(self.token.encode() not in output, "server output disclosed the RPC token")
