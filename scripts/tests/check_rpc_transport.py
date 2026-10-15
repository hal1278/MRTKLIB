#!/usr/bin/env python3
"""Exercise mrtk run startup, authenticated WebSockets, and JSON-RPC envelopes.

Python 3.10+ and its standard library suffice. When jsonschema is importable,
results and errors are also checked against docs/rpc/openrpc.json.
With --compiler, the C preprocessor also checks the CivetWeb minimum-version guard.

Usage: check_rpc_transport.py --mrtk build/mrtk --source-dir .
"""

import argparse
import json
import re
import signal
import socket
import struct
import subprocess
import sys
import time
import unittest
from pathlib import Path

try:
    from jsonschema import Draft7Validator
    from referencing import Registry, Resource
    from referencing.jsonschema import DRAFT7
except ImportError:
    Draft7Validator = None

from rpc_test_support import (
    CONFIG,
    DEADLINE,
    READY,
    TOKEN,
    Server,
    require,
    wait_for,
)


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

    def test_civetweb_minimum_version(self):
        """The actual source guard rejects old versions and accepts newer releases."""
        if self.args.compiler is None:
            self.skipTest("CivetWeb version guard checks require --compiler")
        source = (self.args.source_dir / "apps/rtkrcv/rcvrpc.c").read_text()
        guard = re.search(
            r"^#if CIVETWEB_VERSION_MAJOR.*?^#endif", source, re.MULTILINE | re.DOTALL
        )
        self.assertIsNotNone(guard, "CivetWeb minimum-version guard not found")
        for major, minor, patch, accepted in [
            (0, 99, 99, False),
            (1, 15, 99, False),
            (1, 16, 0, True),
            (1, 16, 1, True),
            (1, 17, 0, True),
            (2, 0, 0, True),
        ]:
            with self.subTest(version=f"{major}.{minor}.{patch}"):
                program = (
                    f"#define CIVETWEB_VERSION_MAJOR {major}\n"
                    f"#define CIVETWEB_VERSION_MINOR {minor}\n"
                    f"#define CIVETWEB_VERSION_PATCH {patch}\n" + guard[0] + "\n"
                )
                result = subprocess.run(
                    [str(self.args.compiler), "-E", "-x", "c", "-"],
                    input=program,
                    text=True,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.PIPE,
                    timeout=DEADLINE,
                    check=False,
                )
                self.assertEqual(result.returncode == 0, accepted, result.stderr)
                if not accepted:
                    self.assertIn("MRTK RPC requires CivetWeb 1.16 or newer", result.stderr)

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
            ("space after token", {"protocols": f"mrtk-token.{TOKEN} , mrtk-rpc"}, 101),
            ("no spaces", {"protocols": f"mrtk-rpc,mrtk-token.{TOKEN}"}, 101),
            ("spaces after comma", {"protocols": f"mrtk-rpc,  mrtk-token.{TOKEN}"}, 101),
            ("space before comma", {"protocols": f"mrtk-rpc , mrtk-token.{TOKEN}"}, 400),
            ("tabs", {"protocols": f"\tmrtk-rpc\t,\tmrtk-token.{TOKEN}\t"}, 400),
            ("tab after comma", {"protocols": f"mrtk-rpc,\tmrtk-token.{TOKEN}"}, 400),
            ("tab in other header", {"headers": ["X-Client: native\tclient"]}, 400),
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

    def test_http_body_rejection(self):
        """Rejected requests close promptly without waiting for their bodies."""
        cases = [
            ("no body length", "/rpc", False, [], b"", 426),
            ("zero body length", "/rpc", False, ["Content-Length: 0"], b"", 426),
            ("incomplete body", "/rpc", False, ["Content-Length: 1000000"], b"few", 426),
            (
                "incomplete chunk",
                "/rpc",
                False,
                ["Transfer-Encoding: chunked"],
                b"F4240\r\nfew",
                426,
            ),
            (
                "identity transfer encoding",
                "/rpc",
                False,
                ["Transfer-Encoding: identity"],
                b"",
                426,
            ),
            ("other upgrade body", "/other", True, ["Content-Length: 1000000"], b"few", 404),
            ("child upgrade body", "/rpc/child", True, ["Content-Length: 1000000"], b"few", 404),
        ]
        with Server(self.args) as server:
            for name, path, upgrade, headers, body, expected in cases:
                with self.subTest(case=name):
                    started = time.monotonic()
                    ws = server.connect(
                        method="GET" if upgrade else "POST",
                        path=path,
                        upgrade=upgrade,
                        headers=headers,
                        body=body,
                        timeout=1.0,
                    )
                    try:
                        self.assertEqual(ws.status, expected)
                        self.assertEqual(ws.headers.get("connection"), ["close"])
                        if expected == 426:
                            self.assertEqual(ws.headers.get("upgrade"), ["websocket"])
                        received = 0
                        while chunk := ws.sock.recv(4096):
                            received += len(chunk)
                            self.assertLess(received, 65536, "unbounded HTTP rejection body")
                        self.assertLess(time.monotonic() - started, 1.0)
                    finally:
                        ws.sock.close()
                    normal = server.connect()
                    self.version(normal)
                    normal.close()

    def test_client_close(self):
        """Client Close receives one reply before TCP EOF, including empty Close."""
        with Server(self.args) as server:
            for payload in (b"", struct.pack("!H", 1000), struct.pack("!H", 1000) + b"finished"):
                with self.subTest(payload_length=len(payload)):
                    ws = server.connect()
                    try:
                        ws.send(payload, opcode=8)
                        self.assertEqual(ws.receive(), (8, payload[:2]))
                        self.assertEqual(
                            ws.sock.recv(1), b"", "Close reply must be followed by TCP EOF"
                        )
                    finally:
                        ws.sock.close()

    def test_frame_validation(self):
        """RSV bits, invalid control frames, and reserved opcodes close with 1002."""
        with Server(self.args) as server:
            cases = [
                (f"RSV{index + 1}", b"{}", dict(rsv=bit))
                for index, bit in enumerate((0x40, 0x20, 0x10))
            ]
            for opcode in (8, 9, 10):
                cases += [
                    (f"long control {opcode}", b"x" * 126, dict(opcode=opcode)),
                    (f"fragmented control {opcode}", b"", dict(opcode=opcode, final=False)),
                ]
            cases += [
                (f"reserved opcode {opcode}", b"", dict(opcode=opcode))
                for opcode in (*range(3, 8), *range(11, 16))
            ]
            for name, payload, flags in cases:
                with self.subTest(case=name):
                    ws = server.connect()
                    try:
                        ws.send(payload, **flags)
                        ws.close_code(1002)
                        self.assertEqual(
                            ws.sock.recv(1), b"", "server must not send a second Close"
                        )
                    finally:
                        ws.sock.close()

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
            for method in ("missing",):
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

    def test_responses_not_delayed(self):
        """Sequential requests are answered without a delayed-ACK wait.

        A response frame is written as a header and a payload. Without TCP_NODELAY,
        Nagle's algorithm can hold the payload until the client acknowledges the
        header, and the client may delay that acknowledgement (40 ms observed on
        Linux). The median tolerates up to four slow round trips; the test assumes
        that the client and the server get a CPU promptly most of the time (on
        Linux with 8 busy loops per CPU the median stayed below 0.1 ms). A platform
        that acknowledges at once passes without TCP_NODELAY, so this checks for
        the delay, not for the socket option.
        """
        with Server(self.args) as server:
            ws = server.connect()
            self.version(ws)
            times = []
            for request_id in range(10):
                started = time.monotonic()
                self.assertIn("result", ws.request(request_id=request_id))
                times.append(time.monotonic() - started)
            times.sort()
            self.assertLess(times[len(times) // 2], 0.02, f"round trips (s): {times}")

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
            self.assertEqual(first.sock.recv(1), b"", "shutdown Close must not be sent twice")
            self.assertEqual(second.sock.recv(1), b"", "shutdown Close must not be sent twice")
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
    parser.add_argument("--compiler", type=Path, help="C compiler for minimum-version guard checks")
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
