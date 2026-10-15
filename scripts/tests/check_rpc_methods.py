#!/usr/bin/env python3
"""Exercise protocol 0.1 lifecycle, status, event pages and overwrite agreements.

Uses only the standard library; jsonschema validation is optional.
Run through CTest to prepare and clean the MALIB replay fixtures:
    ctest --test-dir build -R '^rpc_methods$' --output-on-failure
"""

import argparse
import json
import re
import socket
import time
import unittest
from pathlib import Path

from rpc_test_support import CONFIG, Server, wait_for
from check_rtkrcv_console import file_gate

try:
    from jsonschema import Draft7Validator
except ImportError:
    Draft7Validator = None


def make_validator(doc, schema):
    """Keep local component references rooted in the full contract document."""
    return Draft7Validator(dict(doc, **schema))


class MethodsTests(unittest.TestCase):
    """Check results against both protocol behavior and its closed schemas."""

    args = None
    contract = None

    def validate(self, schema, value):
        if Draft7Validator is not None:
            make_validator(self.contract, {"$ref": f"#/components/schemas/{schema}"}).validate(
                value
            )

    def call(self, ws, method, params=None, error=None):
        response = ws.request(method, params=params if params is not None else {})
        if error:
            self.assertEqual(response["error"]["code"], error)
            self.assertIsInstance(response["error"]["message"], str)
            self.validate("ErrorData", response["error"]["data"])
            self.assertEqual(set(response["error"]), {"code", "message", "data"})
            if error == -32602:
                self.assertEqual(response["error"]["data"], {"reason": "invalidParams"})
            return response["error"]
        result = response["result"]
        method_schema = next(m for m in self.contract["methods"] if m["name"] == method)["result"][
            "schema"
        ]
        if Draft7Validator is not None:
            make_validator(self.contract, method_schema).validate(result)
        if method == "getEvents":
            self.assertFalse(result["lost"], "test must keep up with the ring")
            for event in result["events"]:
                self.validate("Event", event)
            seqs = [e["seq"] for e in result["events"]]
            self.assertEqual(seqs, sorted(set(seqs)))
            if seqs:
                self.assertGreater(seqs[0], params["after"])
                self.assertEqual(result["next"], seqs[-1])
            else:
                self.assertEqual(result["next"], params["after"])
        return result

    def page(self, ws, after, **params):
        return self.call(ws, "getEvents", {"after": after, **params})

    def until(self, ws, after, predicate):
        events = []
        cursor = after

        def collect():
            nonlocal cursor
            result = self.page(ws, cursor)
            events.extend(result["events"])
            cursor = result["next"]
            return predicate(events)

        wait_for(collect, "expected RPC events")
        return events

    def state(self, ws, after, state):
        return self.until(
            ws,
            after,
            lambda events: any(
                e["kind"] == "status" and e["data"]["state"] == state for e in events
            ),
        )

    def accepted(self, ws, method, params=None):
        result = self.call(ws, method, params)
        self.assertEqual(set(result), {"seq"})
        self.assertIsInstance(result["seq"], int)
        return result["seq"]

    def replay(self, directory, output="solution.pos", tagged=True):
        config = (self.args.source_dir / "conf/malib/rtkrcv.toml").read_text()
        config = config.replace(
            "./tests/data/malib/", (self.args.source_dir / "tests/data/malib").as_posix() + "/"
        )
        config = re.sub(
            r'(\[streams.output.stream1\][^\[]*?path\s*=\s*)"[^"]*"',
            lambda m: m[1] + json.dumps(str(directory / output)),
            config,
            count=1,
        )
        if not tagged:
            config = config.replace("::T::x10", "")
        return config

    def finish(self, server, ws):
        self.call(ws, "shutdown")
        ws.close_code(1001)
        server.wait()

    def test_lifecycle_replay(self):
        with Server(self.args, config=lambda d: self.replay(d)) as server:
            ws = server.connect()
            sync = self.call(ws, "sync")
            self.assertEqual(sync["status"], {"lifecycle": {"state": "stopped"}})
            self.assertEqual(self.page(ws, sync["seq"])["events"], [])
            self.assertEqual(self.page(ws, sync["seq"])["events"], [])
            seq = self.accepted(ws, "start")
            self.assertGreater(seq, sync["seq"])
            events = self.state(ws, sync["seq"], "running")
            self.assertEqual(
                events[0], {"seq": seq, "kind": "status", "data": {"state": "starting"}}
            )
            self.until(
                ws,
                seq,
                lambda ev: any(
                    e["kind"] == "solution" and e["data"]["status"] != "none" for e in ev
                ),
            )
            status = self.call(ws, "getStatus")
            self.assertEqual(status["status"]["mode"], "ppp-kine")
            self.assertIn("solution", status["status"])
            self.assertEqual(
                self.call(ws, "start", error=1002)["data"], {"reason": "alreadyRunning"}
            )
            seq = self.accepted(
                ws, "restart", {"overwrite": [str(server.directory / "solution.pos")]}
            )
            events = self.state(ws, seq - 1, "running")
            states = [e["data"]["state"] for e in events if e["kind"] == "status"]
            self.assertEqual(states, ["stopping", "stopped", "starting", "running"])
            self.assertEqual(events[0]["seq"], seq)
            seq = self.accepted(ws, "stop")
            events = self.state(ws, seq - 1, "stopped")
            self.assertEqual(events[0]["data"], {"state": "stopping", "stopReason": "request"})
            self.assertEqual(events[-1]["data"], {"state": "stopped", "stopReason": "request"})
            boundary = events[-1]["seq"]
            self.assertEqual(self.accepted(ws, "stop"), boundary)
            self.assertEqual(self.page(ws, boundary)["events"], [])
            # Read all runs together to check no solution crosses a final stopped event.
            all_events = self.page(ws, sync["seq"])["events"]
            running = False
            for event in all_events:
                if event["kind"] == "status":
                    state = event["data"]["state"]
                    if state == "running":
                        running = True
                    if state in ("starting", "stopped"):
                        running = False
                if event["kind"] == "solution":
                    self.assertTrue(running)
            first = self.page(ws, sync["seq"], limit=1)
            self.assertEqual(len(first["events"]), 1)
            self.assertTrue(first["more"])
            self.assertLessEqual(len(self.page(ws, 0, limit=100000)["events"]), 1000)
            self.finish(server, ws)

    def test_held_start(self):
        def config(directory):
            gate = file_gate(directory / "entered", directory / "release")
            return CONFIG + "\n[server]\nstart_cmd = " + json.dumps(gate) + "\n"

        with Server(self.args, config=config) as server:
            ws = server.connect()
            started = time.monotonic()
            seq = self.accepted(ws, "start")
            self.assertLess(time.monotonic() - started, 1.0)
            wait_for((server.directory / "entered").exists, "start gate entered")
            for method in ("start", "stop", "restart"):
                self.assertEqual(
                    self.call(ws, method, error=1001)["data"],
                    {"reason": "busy", "state": "starting"},
                )
            for method, params in (("getStatus", {}), ("getEvents", {"after": seq - 1})):
                started = time.monotonic()
                self.call(ws, method, params)
                self.assertLess(time.monotonic() - started, 1.0)
            # Independent connections remain available during the same claim.
            peer = server.connect()
            self.call(peer, "sync")
            started = time.monotonic()
            self.call(ws, "shutdown")
            self.assertLess(time.monotonic() - started, 1.0)
            self.assertIsNone(server.process.poll())
            for method in ("start", "stop", "restart"):
                self.assertEqual(
                    self.call(ws, method, error=1003)["data"], {"reason": "shuttingDown"}
                )
            (server.directory / "release").touch()
            ws.close_code(1001)
            peer.close_code(1001)
            server.wait()

    def test_overwrite_and_retry(self):
        def config(directory):
            (directory / "solution.pos").write_text("existing")
            return self.replay(directory)

        with Server(self.args, config=config) as server:
            ws = server.connect()
            seq = self.accepted(ws, "start")
            events = self.state(ws, seq - 1, "stopped")
            error = events[-1]["data"]["lastError"]
            self.assertEqual(
                error,
                {
                    "reason": "outputExists",
                    "message": "Output files already exist.",
                    "paths": [str(server.directory / "solution.pos")],
                },
            )
            self.assertEqual(self.call(ws, "getStatus")["status"]["lifecycle"], events[-1]["data"])
            seq = self.accepted(ws, "start", {"overwrite": error["paths"]})
            events = self.state(ws, seq - 1, "running")
            self.assertEqual(events[0]["data"], {"state": "starting", "lastError": error})
            self.assertEqual(events[-1]["data"], {"state": "running"})
            self.finish(server, ws)

    def test_keyword_output(self):
        for tagged in (False, True):
            with self.subTest(tagged=tagged):

                def config(directory):
                    # Create yesterday, today and tomorrow to avoid midnight flakes.
                    for offset in (-86400, 0, 86400):
                        (
                            directory
                            / time.strftime("out_%Y%m%d.pos", time.gmtime(time.time() + offset))
                        ).touch()
                    return self.replay(directory, "out_%Y%m%d.pos", tagged=tagged)

                with Server(self.args, config=config) as server:
                    ws = server.connect()
                    seq = self.accepted(ws, "start")
                    events = self.state(ws, seq - 1, "running" if tagged else "stopped")
                    if not tagged:
                        paths = events[-1]["data"]["lastError"]["paths"]
                        self.assertEqual(len(paths), 1)
                        self.assertNotIn("%", paths[0])
                        self.assertTrue(Path(paths[0]).exists())
                    self.finish(server, ws)

    def test_start_failure_redaction(self):
        # A TCP client path carries a password; the invalid port makes the open
        # fail at once. File paths are not secrets, so a file input would not test this.
        config = CONFIG.replace(
            '[streams.input.rover]\ntype = "off"',
            '[streams.input.rover]\ntype = "tcpcli"\npath = "user:secret@127.0.0.1:notaport"',
        )
        with Server(self.args, config=config) as server:
            ws = server.connect()
            seq = self.accepted(ws, "start")
            events = self.state(ws, seq - 1, "stopped")
            self.assertEqual(
                events[-1]["data"]["lastError"],
                {"reason": "startFailed", "message": "The server could not start."},
            )
            self.assertTrue(
                any(e["kind"] == "logMessage" and "user:***@" in e["data"]["text"] for e in events)
            )
            status = self.call(ws, "getStatus")
            self.assertNotIn("secret", json.dumps([events, status]))
            self.finish(server, ws)
            self.assertNotIn(b"secret", server.output())
            self.assertIn(b"rtk server start error: ", server.output())

    def test_slow_reader_isolation(self):
        """A blocked response write cannot hold the registry mutex."""
        path = "missing/" + "x" * 900
        config = CONFIG.replace(
            '[streams.input.rover]\ntype = "off"',
            '[streams.input.rover]\ntype = "file"\npath = ' + json.dumps(path),
        )
        with Server(self.args, config=config) as server:
            ws = server.connect()
            # Each failure contributes two long log events plus lifecycle events.
            for _ in range(300):
                seq = self.accepted(ws, "start")
                self.state(ws, seq - 1, "stopped")
            page = self.page(ws, 0)
            self.assertEqual(len(page["events"]), 1000)
            self.assertTrue(page["more"])
            self.assertGreater(len(json.dumps(page)), 200000)
            slow = server.connect()
            slow.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
            for request_id in range(100):
                slow.send(
                    json.dumps(
                        {
                            "jsonrpc": "2.0",
                            "method": "getEvents",
                            "id": request_id,
                            "params": {"after": 0},
                        }
                    )
                )
            time.sleep(0.2)
            started = time.monotonic()
            self.call(ws, "getStatus")
            self.assertLess(time.monotonic() - started, 1.0)
            slow.sock.close()
            self.finish(server, ws)

    def test_invalid_parameters(self):
        with Server(self.args) as server:
            ws = server.connect()
            for method in (
                "getVersion",
                "getStatus",
                "sync",
                "getEvents",
                "start",
                "stop",
                "restart",
                "shutdown",
            ):
                self.call(ws, method, {"unknown": 1}, error=-32602)
            for method in ("start", "restart"):
                for value in (None, 1, True, "path", {}, [1], [False], [None]):
                    self.call(ws, method, {"overwrite": value}, error=-32602)
            for params in (
                {},
                {"after": 0, "limit": 0},
                {"after": 1},
                {"after": -1},
                {"after": 2**53},
                {"after": 0.1},
                {"after": True},
                {"after": "0"},
                {"after": 0, "limit": 1.1},
                {"after": 0, "limit": False},
            ):
                self.call(ws, "getEvents", params, error=-32602)
            for params in (
                {"after": 0},
                {"after": 0.0, "limit": 1e99},
                {"after": 0, "limit": 1001},
            ):
                self.assertEqual(self.call(ws, "getEvents", params)["events"], [])
            self.finish(server, ws)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mrtk", type=Path, required=True)
    parser.add_argument("--source-dir", type=Path, required=True)
    args = parser.parse_args()
    args.mrtk = args.mrtk.resolve()
    args.source_dir = args.source_dir.resolve()
    fixture = args.source_dir / "tests/data/malib/MALIB_OSS_data_obsnav_240822-1100.sbf"
    if not fixture.is_file():
        parser.error("MALIB replay fixtures are missing; run through CTest with -R '^rpc_methods$'")
    MethodsTests.args = args
    MethodsTests.contract = json.loads((args.source_dir / "docs/rpc/openrpc.json").read_text())
    if Draft7Validator is None:
        print("jsonschema unavailable: schema validation skipped; behavioral tests still run")
    result = unittest.TextTestRunner(verbosity=2).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(MethodsTests)
    )
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
