#!/usr/bin/env python3
"""Exercise mrtk run startup, authenticated WebSockets, and JSON-RPC envelopes.

Python 3.10+ and its standard library suffice. When jsonschema is importable,
results and errors are also checked against docs/rpc/openrpc.json.

Usage: check_rpc_transport.py --mrtk build/mrtk --source-dir .
"""

import argparse
import base64
import hashlib
import ipaddress
import json
import os
import re
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from urllib.parse import urlsplit

try:
    from jsonschema import Draft7Validator
    from referencing import Registry, Resource
    from referencing.jsonschema import DRAFT7
except ImportError:
    Draft7Validator = None

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
        self, server, protocols=OFFER, origin=None, path="/rpc", upgrade=True, method="GET"
    ):
        """Connect and finish one raw HTTP handshake."""
        self.sock = socket.create_connection((server.host, server.port), timeout=DEADLINE)
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
        self.sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode("ascii"))
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

    def send(self, payload, opcode=1, final=True):
        """Send one client frame, including a mask even for empty payloads."""
        if isinstance(payload, str):
            payload = payload.encode("utf-8")
        length = len(payload)
        header = bytes([(0x80 if final else 0) | opcode])
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
            require(length <= 65536, "unbounded server frame")
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
        (self.directory / "run.toml").write_text(self.config, encoding="utf-8")
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


class TransportTests(unittest.TestCase):
    """Public process and transport contract checks."""

    args = None
    contract = None

    def validate(self, schema, instance):
        """Use the same draft and local reference registry as the schema self-test."""
        if Draft7Validator is None:
            return
        root = dict(self.contract, __transport_check=schema)
        registry = Registry().with_resource(
            "urn:mrtk-transport", Resource(contents=root, specification=DRAFT7)
        )
        Draft7Validator(
            {"$ref": "urn:mrtk-transport#/__transport_check"}, registry=registry
        ).validate(instance)

    def version(self, ws, authentication="token"):
        """Check getVersion values, independently of optional schema validation."""
        response = ws.request()
        self.assertEqual(response["id"], 1)
        result = response["result"]
        self.assertEqual(set(result), {"protocolVersion", "mrtklibVersion", "authentication"})
        self.assertEqual(result["protocolVersion"], "0.1")
        self.assertEqual(result["authentication"], authentication)
        version = re.search(
            r"project\(MRTKLIB VERSION ([0-9.]+)",
            (self.args.source_dir / "CMakeLists.txt").read_text(),
        )[1]
        self.assertEqual(result["mrtklibVersion"], version)
        self.validate({"$ref": "#/components/schemas/VersionResult"}, result)

    def error(self, ws, payload, code, request_id=None):
        """Check standard error fields and the contract's invalidParams data."""
        ws.send(payload)
        response, raw = ws.response()
        self.assertEqual(response["id"], request_id)
        self.validate_error(response["error"], code)
        return raw

    def validate_error(self, error, code):
        """Check every error against its envelope and the shared contract data."""
        self.assertEqual(error["code"], code)
        self.assertIsInstance(error["message"], str)
        self.assertTrue(error["message"])
        self.assertTrue(error["message"].isascii())
        self.validate(
            {
                "type": "object",
                "required": ["code", "message"],
                "additionalProperties": False,
                "properties": {
                    "code": {"type": "integer", "const": code},
                    "message": {"type": "string", "minLength": 1},
                    "data": {"$ref": "#/components/schemas/ErrorData"},
                },
            },
            error,
        )
        if code == -32602:
            self.assertEqual(
                error["data"], self.contract["components"]["errors"]["InvalidParams"]["data"]
            )
        else:
            self.assertNotIn("data", error)

    def test_bind_addresses(self):
        """Port zero, IPv4/IPv6, and wildcard URLs are usable."""
        for bind, host in [
            ("127.0.0.1", "127.0.0.1"),
            ("::1", "::1"),
            ("0.0.0.0", "127.0.0.1"),
            ("::", "::1"),
            ("0:0:0:0:0:0:0:0", "::1"),
        ]:
            with (
                self.subTest(bind=bind),
                Server(
                    self.args, ["--rpc-port", "0", "--rpc-bind", bind, "--no-console"]
                ) as server,
            ):
                self.assertEqual(server.host, host)
                bracketed = f"[{host}]" if ":" in host else host
                self.assertEqual(server.url, f"ws://{bracketed}:{server.port}/rpc")
                self.version(server.connect())
                server.process.send_signal(signal.SIGTERM)
                server.wait()

    def test_token_bounds_and_default_origin(self):
        """Both token length bounds work, while browser origins default to refused."""
        for token in ("x", "X" * 256):
            with self.subTest(length=len(token)), Server(self.args, token=token) as server:
                offer = f"mrtk-rpc, mrtk-token.{token}"
                ws = server.connect(protocols=offer)
                self.version(ws)
                ws.close()
                browser = server.connect(protocols=offer, origin="http://localhost")
                self.assertEqual(browser.status, 403)
                browser.close()

    def test_bind_failure(self):
        """A listener already occupying the selected port prevents readiness."""
        with socket.socket() as occupied:
            occupied.bind(("127.0.0.1", 0))
            occupied.listen(1)
            port = occupied.getsockname()[1]
            with Server(self.args, ["--rpc-port", str(port), "--no-console"], ready=False) as s:
                s.refuse("bind")

    def test_startup_validation(self):
        """Invalid token and incompatible options always fail visibly."""
        cases = [
            (["--rpc-port", "0", "--no-console"], None, "MRTK_RPC_TOKEN"),
            (["--rpc-port", "0", "--no-console"], "", "MRTK_RPC_TOKEN"),
            (["--rpc-port", "0", "--no-console"], "bad.token", "MRTK_RPC_TOKEN"),
            (["--rpc-port", "0", "--no-console"], "x" * 257, "MRTK_RPC_TOKEN"),
            (["--no-console"], TOKEN, "--rpc-port"),
            (["--rpc-port", "-1"], TOKEN, "--rpc-port"),
            (["--rpc-port", "65536"], TOKEN, "--rpc-port"),
            (["--rpc-port", "invalid"], TOKEN, "--rpc-port"),
            (["--rpc-port", "0", "--rpc-bind", "127.0.0.1:123,0.0.0.0"], TOKEN, "--rpc-bind"),
            (["--rpc-port", "0", "--exit-on-stdin-eof"], TOKEN, "--no-console"),
        ]
        for index, (options, token, diagnostic) in enumerate(cases):
            with (
                self.subTest(case=index),
                Server(self.args, options, token=token, ready=False) as server,
            ):
                server.refuse(diagnostic)

    def test_no_auth(self):
        """No-auth is explicit and restricted to every loopback family."""
        for bind in ("127.0.0.1", "127.23.4.5", "::1"):
            with (
                self.subTest(bind=bind),
                Server(
                    self.args,
                    ["--rpc-port", "0", "--rpc-bind", bind, "--no-console", "--rpc-no-auth"],
                    token=None,
                ) as server,
            ):
                # Ready URLs preserve non-default loopback binds too.
                self.version(server.connect(protocols="mrtk-rpc"), "none")
                self.assertIn(b"warning", server.output().lower())
                server.process.send_signal(signal.SIGTERM)
                server.wait()
        for bind in ("0.0.0.0", "::", "192.0.2.1"):
            with (
                self.subTest(bind=bind),
                Server(
                    self.args,
                    ["--rpc-port", "0", "--rpc-bind", bind, "--no-console", "--rpc-no-auth"],
                    token=None,
                    ready=False,
                ) as server,
            ):
                server.refuse("loopback")

    def test_handshake(self):
        """Authentication ordering, all header entries, origins, and paths are fixed."""
        options = [
            "--rpc-port",
            "0",
            "--no-console",
            "--rpc-origin",
            "http://allowed",
            "--rpc-origin",
            "https://second:8443",
        ]
        cases = [
            ("default", {}, 101),
            ("token first", {"protocols": f"mrtk-token.{TOKEN}, mrtk-rpc"}, 101),
            ("no spaces", {"protocols": f"mrtk-rpc,mrtk-token.{TOKEN}"}, 101),
            ("spaces after comma", {"protocols": f"mrtk-rpc,  mrtk-token.{TOKEN}"}, 101),
            ("space before comma", {"protocols": f"mrtk-rpc , mrtk-token.{TOKEN}"}, 400),
            ("tabs", {"protocols": f"\tmrtk-rpc\t,\tmrtk-token.{TOKEN}\t"}, 400),
            ("multiple headers", {"protocols": ["mrtk-rpc", f"mrtk-token.{TOKEN}"]}, 101),
            ("allowed origin", {"origin": "HTTP://ALLOWED"}, 101),
            ("second origin", {"origin": "https://second:8443"}, 101),
            ("no protocols", {"protocols": None}, 400),
            ("no rpc", {"protocols": f"mrtk-token.{TOKEN}"}, 400),
            ("rpc first check", {"protocols": "wrong", "origin": "http://bad"}, 400),
            ("no token", {"protocols": "mrtk-rpc"}, 401),
            ("wrong token", {"protocols": "mrtk-rpc, mrtk-token.wrong"}, 401),
            ("prefix token", {"protocols": f"mrtk-rpc, mrtk-token.{TOKEN}suffix"}, 401),
            ("origin first check", {"protocols": "mrtk-rpc", "origin": "http://bad"}, 403),
            ("bad origin", {"origin": "http://bad"}, 403),
            ("origin suffix", {"origin": "http://allowed.evil"}, 403),
            ("rpc child", {"path": "/rpc/child"}, 404),
            ("other websocket", {"path": "/other"}, 404),
            ("plain rpc", {"upgrade": False}, 426),
            ("plain other", {"path": "/other", "upgrade": False}, 404),
            ("plain root", {"path": "/", "upgrade": False}, 404),
            ("PUT other", {"method": "PUT", "path": "/other", "upgrade": False}, 405),
            ("DELETE other", {"method": "DELETE", "path": "/other", "upgrade": False}, 405),
        ]
        with Server(self.args, options) as server:
            for name, kwargs, expected in cases:
                with self.subTest(case=name):
                    ws = server.connect(**kwargs)
                    self.assertEqual(ws.status, expected)
                    if expected == 101:
                        self.version(ws)
                    else:
                        self.assertEqual(ws.headers.get("connection"), ["close"])
                    ws.close()

    def test_frames(self):
        """Invalid message types, UTF-8, and size receive their specified closes."""
        with Server(self.args) as server:
            for name, payload, opcode, expected in [
                ("binary", b"binary", 2, 1003),
                ("invalid UTF-8", b"\xff", 1, 1007),
                ("overlong UTF-8", b"\xc0\xaf", 1, 1007),
                ("surrogate", b"\xed\xa0\x80", 1, 1007),
                ("truncated UTF-8", b"\xe2\x82", 1, 1007),
                ("too large", b"x" * 65537, 1, 1009),
            ]:
                with self.subTest(case=name):
                    ws = server.connect()
                    ws.send(payload, opcode)
                    ws.close_code(expected)
                    ws.close()
            ws = server.connect()
            ws.send(b"x" * 32768, final=False)
            ws.send(b"x" * 32769, opcode=0)
            ws.close_code(1009)
            ws.close()

    def test_fragmentation_ping_and_limit(self):
        """Fragment reassembly permits control frames and a 64 KiB text message."""
        with Server(self.args) as server:
            ws = server.connect()
            request = '{"jsonrpc":"2.0","method":"getVersion","id":"fragment-\u20ac"}'.encode()
            split = request.index("\u20ac".encode()) + 1
            ws.send(request[:split], final=False)
            ws.send(b"fragment-ping", opcode=9)
            self.assertEqual(ws.receive(), (10, b"fragment-ping"))
            ws.send(request[split:], opcode=0)
            response, _ = ws.response()
            self.assertEqual(response["id"], "fragment-\u20ac")
            self.assertIn("result", response)
            ws.no_response()
            payload = b'{"jsonrpc":"2.0","method":"getVersion","id":2}'
            ws.send(payload + b" " * (65536 - len(payload)))
            self.assertEqual(ws.response()[0]["id"], 2)

    def test_jsonrpc_errors(self):
        """Strict JSON parsing and request-envelope validation have stable errors."""
        with Server(self.args) as server:
            ws = server.connect()
            for payload in (b"{", b'{"x":NaN}', b'{"x":1,}', b"/*comment*/{}", b"{}{}"):
                with self.subTest(payload=payload):
                    self.error(ws, payload, -32700)
            invalid = [
                [],
                [dict(jsonrpc="2.0", method="getVersion", id=1)],
                None,
                1,
                True,
                "text",
                {},
                {"method": "getVersion", "id": 1},
                {"jsonrpc": "1.0", "method": "getVersion", "id": 1},
                {"jsonrpc": "2.0", "id": 1},
                {"jsonrpc": "2.0", "method": 1, "id": 1},
            ]
            # Invalid envelopes use a null id; valid id fields may still be echoed.
            for index, payload in enumerate(invalid):
                with self.subTest(invalid=index):
                    ws.send(json.dumps(payload))
                    result = ws.response()[0]
                    self.validate_error(result["error"], -32600)
            for request_id in (None, True, False, [], {}):
                with self.subTest(id_type=type(request_id).__name__):
                    self.error(
                        ws,
                        json.dumps(dict(jsonrpc="2.0", method="getVersion", id=request_id)),
                        -32600,
                    )
            for params in (None, [], 1, True, "text", {"unknown": 1}):
                with self.subTest(params=params):
                    self.error(
                        ws,
                        json.dumps(
                            dict(jsonrpc="2.0", method="getVersion", id="params", params=params)
                        ),
                        -32602,
                        "params",
                    )
            for method in ("missing", "getStatus", "sync", "getEvents", "start", "stop", "restart"):
                with self.subTest(method=method):
                    self.error(ws, json.dumps(dict(jsonrpc="2.0", method=method, id=7)), -32601, 7)
            self.assertIn("result", ws.request(params={}))

    def test_notification_and_exact_id(self):
        """Notifications do not execute and number ids preserve their input spelling."""
        with Server(self.args) as server:
            ws = server.connect()
            ws.send('{"jsonrpc":"2.0","method":"shutdown"}')
            ws.no_response()
            self.assertIsNone(server.process.poll())
            self.version(ws)
            for spelling in ("9007199254740993", "12345678901234567890"):
                with self.subTest(id=spelling):
                    ws.send('{"jsonrpc":"2.0","method":"getVersion","id":' + spelling + "}")
                    response, raw = ws.response()
                    self.assertIn("result", response)
                    match = re.search(rb'"id"\s*:\s*([^,}\s]+)', raw)
                    self.assertIsNotNone(match)
                    self.assertEqual(match[1].decode(), spelling)
            for request_id in ("text-\u20ac", 0, -1):
                self.assertEqual(ws.request(request_id=request_id)["id"], request_id)

    def test_shutdown(self):
        """Shutdown replies before closing every open connection with 1001."""
        with Server(self.args) as server:
            first, second = server.connect(), server.connect()
            response = first.request("shutdown", "shutdown")
            self.assertEqual(response["id"], "shutdown")
            self.assertEqual(set(response["result"]), {"seq"})
            self.validate({"$ref": "#/components/schemas/Seq"}, response["result"]["seq"])
            self.assertIsInstance(response["result"]["seq"], int)
            first.close_code(1001)
            second.close_code(1001)
            server.wait()

    def test_stdin_eof(self):
        """Discarded stdin data keeps serving; EOF shuts down normally and saves nav."""
        with Server(self.args, ["--rpc-port", "0", "--no-console", "--exit-on-stdin-eof"]) as s:
            ws = s.connect()
            s.process.stdin.write(b"discard this input\n")
            s.process.stdin.flush()
            self.version(ws)
            s.process.stdin.close()
            ws.close_code(1001)
            s.wait()
            self.assertTrue((s.directory / "rtkrcv.nav").is_file(), "normal exit must save nav")

    def test_token_environment_removed(self):
        """A startup child process cannot inherit the token."""
        config = CONFIG + '\n[server]\nstart_cmd = "env > child-environment"\n'
        with Server(self.args, ["--rpc-port", "0", "--no-console", "-s"], config=config) as s:
            marker = s.directory / "child-environment"
            wait_for(marker.is_file, "misc-startcmd child environment")
            environment = marker.read_bytes()
            self.assertNotIn(b"MRTK_RPC_TOKEN=", environment)
            self.assertNotIn(TOKEN.encode(), environment)
            ws = s.connect()
            ws.request("shutdown")
            ws.close_code(1001)
            s.wait()

    def test_invalid_token_without_endpoint(self):
        """Legacy runs ignore invalid tokens but still remove them before children."""
        config = CONFIG + '\n[server]\nstart_cmd = "env > child-environment"\n'
        for token in ("bad.token", "", "x" * 257):
            with self.subTest(length=len(token)):
                with socket.socket() as probe:
                    probe.bind(("127.0.0.1", 0))
                    port = probe.getsockname()[1]
                with Server(
                    self.args,
                    ["-p", str(port), "-s"],
                    token=token,
                    ready=False,
                    config=config,
                ) as s:
                    marker = s.directory / "child-environment"
                    wait_for(marker.is_file, "misc-startcmd child environment")
                    sock = socket.create_connection(("127.0.0.1", port), timeout=DEADLINE)
                    s.sockets.append(sock)
                    raw = bytearray()
                    while not raw.endswith(b"rtkrcv> "):
                        chunk = sock.recv(4096)
                        require(bool(chunk), "telnet closed before prompt")
                        raw.extend(chunk)
                    sock.sendall(b"shutdown\r\n")
                    s.wait()
                    self.assertNotIn(READY, s.output())
                    self.assertNotIn(b"MRTK_RPC_TOKEN", s.output())
                    self.assertNotIn(b"MRTK_RPC_TOKEN=", marker.read_bytes())

    def test_stdin_configuration_refused(self):
        """The telnet console preserves its failure line when stdin is reserved."""
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        config = CONFIG.replace(
            '[streams.input.rover]\ntype = "off"',
            '[streams.input.rover]\ntype = "file"\npath = "::T"',
        )
        with Server(
            self.args,
            ["--rpc-port", "0", "--no-console", "--exit-on-stdin-eof", "-p", str(port), "-w", ""],
            config=config,
        ) as s:
            sock = socket.create_connection(("127.0.0.1", port), timeout=DEADLINE)
            s.sockets.append(sock)

            def prompt():
                raw = bytearray()
                while not raw.endswith(b"rtkrcv> "):
                    chunk = sock.recv(4096)
                    require(bool(chunk), "telnet closed before prompt")
                    raw.extend(chunk)
                return re.sub(rb"\x1b\[[0-9;]*[A-Za-z]", b"", raw).replace(b"\r\n", b"\n")

            prompt()
            sock.sendall(b"start\r\n")
            output = prompt()
            self.assertIn(b"rtk server start error (", output)
            self.assertNotIn(b"rtk server start\n", output)
            ws = s.connect()
            self.version(ws)
            s.process.stdin.close()
            ws.close_code(1001)
            s.wait()


def main():
    """Parse test paths and run every transport test."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mrtk", required=True, type=Path)
    parser.add_argument("--source-dir", required=True, type=Path)
    args = parser.parse_args()
    args.mrtk = args.mrtk.resolve()
    args.source_dir = args.source_dir.resolve()
    TransportTests.args = args
    TransportTests.contract = json.loads((args.source_dir / "docs/rpc/openrpc.json").read_text())
    if Draft7Validator is None:
        print("jsonschema unavailable: skipping schema checks; transport checks still run")
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(TransportTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
