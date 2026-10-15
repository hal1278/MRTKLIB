#!/usr/bin/env python3
"""check_openrpc.py - Self-check of the JSON-RPC contract docs/rpc/openrpc.json.

The contract encodes rules of docs/rpc/README.md as JSON Schema conditions
(for example: a running server reports no stopReason; a `none` solution has
no position). Mocks and contract tests validate their messages against the
file, so a rule that is dropped from the schema by mistake would go unnoticed.
This script fails when:

  - a component schema is not a valid JSON Schema (draft-07, as OpenRPC 1.3),
  - a `$ref` does not resolve,
  - a method example or an error example does not match its schema,
  - a valid case is rejected or a counter-example of a rule is accepted.

Each rule has at least one counter-example in CASES. When a rule changes,
change README.md, openrpc.json and CASES in the same commit.

Requires the Python package jsonschema (with referencing).

Usage: check_openrpc.py docs/rpc/openrpc.json
"""

import copy
import json
import sys

from jsonschema import Draft7Validator
from referencing import Registry, Resource
from referencing.jsonschema import DRAFT7

URI = "urn:mrtk-rpc"

LAST_ERROR = {"reason": "startFailed", "message": "input stream 1 could not be opened"}
TIME = {"week": 2440, "tow": 345600.25}
NONE = {"time": TIME, "leapSeconds": 18, "status": "none"}
POSITION = {
    "ecef": {"x": -3961904.912, "y": 3348993.77, "z": 3698211.831},
    "llh": {"lat": 35.66589, "lon": 139.79201, "height": 59.873},
}
COVARIANCE = {
    "ecef": {"xx": 1e-4, "yy": 1e-4, "zz": 2e-4, "xy": 1e-5, "yz": 2e-5, "zx": -1e-5},
    "enu": {"ee": 8e-5, "nn": 9e-5, "uu": 3e-4, "en": 1e-5, "nu": -2e-5, "ue": 1e-5},
}
FIX = {
    "time": TIME,
    "leapSeconds": 18,
    "status": "fix",
    "position": POSITION,
    "covariance": COVARIANCE,
    "geoid": {"model": "internal", "separation": 36.7},
    "validSatellites": 18,
    "age": 0.6,
    "ratio": 12.4,
}


def without(obj, key):
    """Return a copy of `obj` without member `key`."""
    obj = dict(obj)
    del obj[key]
    return obj


# (component schema, instance, expected valid, label)
CASES = [
    (
        "LastError",
        {
            "reason": "outputExists",
            "message": "Output files already exist.",
            "paths": ["/tmp/out.pos"],
        },
        True,
        "overwrite refused",
    ),
    (
        "LastError",
        {"reason": "outputExists", "message": "Output files already exist."},
        False,
        "overwrite without paths",
    ),
    (
        "LastError",
        {"reason": "outputExists", "message": "Output files already exist.", "paths": []},
        False,
        "overwrite empty paths",
    ),
    (
        "LastError",
        {"reason": "outputExists", "message": "Output files already exist.", "paths": [1]},
        False,
        "overwrite non-string path",
    ),
    ("LastError", {**LAST_ERROR, "paths": ["/tmp/out.pos"]}, False, "startFailed with paths"),
    ("OverwritePaths", [], True, "empty overwrite permission"),
    ("OverwritePaths", ["/tmp/out.pos"], True, "overwrite permission"),
    ("OverwritePaths", "/tmp/out.pos", False, "overwrite must be an array"),
    ("OverwritePaths", [1], False, "overwrite members must be strings"),
    # lifecycle
    ("Lifecycle", {"state": "stopped"}, True, "stopped before the first start"),
    ("Lifecycle", {"state": "stopped", "stopReason": "inputEnd"}, True, "stopped at input end"),
    (
        "Lifecycle",
        {"state": "stopped", "stopReason": "error", "lastError": LAST_ERROR},
        True,
        "start failed",
    ),
    (
        "Lifecycle",
        {"state": "starting", "lastError": LAST_ERROR},
        True,
        "starting again after a failure",
    ),
    ("Lifecycle", {"state": "stopping", "stopReason": "request"}, True, "stopping"),
    ("Lifecycle", {"state": "running", "stopReason": "request"}, False, "running with stopReason"),
    (
        "Lifecycle",
        {"state": "starting", "stopReason": "error", "lastError": LAST_ERROR},
        False,
        "starting with stopReason",
    ),
    ("Lifecycle", {"state": "stopping"}, False, "stopping without stopReason"),
    ("Lifecycle", {"state": "running", "lastError": LAST_ERROR}, False, "running with lastError"),
    ("Lifecycle", {"state": "stopped", "stopReason": "error"}, False, "error without lastError"),
    (
        "Lifecycle",
        {"state": "stopped", "stopReason": "request", "lastError": LAST_ERROR},
        False,
        "request with lastError",
    ),
    (
        "Lifecycle",
        {"state": "stopped", "lastError": LAST_ERROR},
        False,
        "first stopped with lastError",
    ),
    # status: mode and solution only while running
    (
        "Status",
        {"lifecycle": {"state": "running"}, "mode": "kinematic"},
        True,
        "running before the first solution",
    ),
    (
        "Status",
        {"lifecycle": {"state": "running"}, "mode": "ppp-kine", "solution": FIX},
        True,
        "running with a solution",
    ),
    ("Status", {"lifecycle": {"state": "running"}}, False, "running without mode"),
    (
        "Status",
        {"lifecycle": {"state": "stopped"}, "mode": "kinematic"},
        False,
        "stopped with mode",
    ),
    (
        "Status",
        {"lifecycle": {"state": "stopping", "stopReason": "request"}, "solution": NONE},
        False,
        "stopping with solution",
    ),
    # solution
    ("Solution", NONE, True, "none solution"),
    ("Solution", FIX, True, "fix solution"),
    ("Solution", {**FIX, "geoid": {"model": "egm96"}}, True, "geoid model without a value"),
    ("Solution", {**NONE, "position": POSITION}, False, "none with position"),
    ("Solution", {**NONE, "geoid": {"model": "internal"}}, False, "none with geoid"),
    ("Solution", without(FIX, "position"), False, "fix without position"),
    ("Solution", without(FIX, "geoid"), False, "fix without geoid"),
    (
        "Solution",
        {**FIX, "geoid": {"model": "internal", "separation": 0}},
        False,
        "geoid separation 0",
    ),
    ("Solution", {**NONE, "time": {"week": 2440, "tow": 604800}}, False, "tow out of range"),
    ("Solution", {**NONE, "time": {"week": -522, "tow": -259200}}, False, "unset time"),
    # events
    ("Event", {"seq": 5, "kind": "status", "data": {"state": "running"}}, True, "status event"),
    ("Event", {"seq": 6, "kind": "solution", "data": NONE}, True, "solution event"),
    (
        "Event",
        {"seq": 7, "kind": "logMessage", "data": {"level": 1, "text": "x"}},
        True,
        "log event",
    ),
    ("Event", {"seq": 5, "kind": "status", "data": NONE}, False, "kind and data mismatch"),
    (
        "Event",
        {"seq": 7, "kind": "logMessage", "data": {"level": 3, "text": "x"}},
        False,
        "log level 3",
    ),
    ("Event", {"seq": 2**53, "kind": "status", "data": {"state": "running"}}, False, "seq 2^53"),
    ("Event", {"seq": 8, "kind": "solutionGap", "data": {}}, False, "unknown kind"),
    # getEvents page
    (
        "EventsResult",
        {"events": [], "next": 1, "more": False, "lost": True, "oldestSeq": 5},
        True,
        "lost page",
    ),
    (
        "EventsResult",
        {
            "events": [{"seq": 2, "kind": "status", "data": {"state": "running"}}],
            "next": 2,
            "more": False,
            "lost": True,
            "oldestSeq": 5,
        },
        False,
        "lost page with events",
    ),
    (
        "EventsResult",
        {"events": [], "next": 1, "more": False, "lost": False, "oldestSeq": 5},
        False,
        "oldestSeq without lost",
    ),
]


def make_validator(doc, schema):
    """Validator for `schema`, resolving '#/components/...' against `doc`."""
    root = copy.deepcopy(doc)
    root["__check"] = schema
    registry = Registry().with_resource(URI, Resource(contents=root, specification=DRAFT7))
    return Draft7Validator({"$ref": URI + "#/__check"}, registry=registry)


def refs(node):
    """Yield every `$ref` value in `node`."""
    if isinstance(node, dict):
        for key, value in node.items():
            if key == "$ref":
                yield value
            else:
                yield from refs(value)
    elif isinstance(node, list):
        for value in node:
            yield from refs(value)


def main():
    """Run all checks; return the process exit code."""
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    with open(sys.argv[1], encoding="utf-8") as f:
        doc = json.load(f)
    failures = []

    for name, schema in doc["components"]["schemas"].items():
        try:
            Draft7Validator.check_schema(schema)
        except Exception as e:  # noqa: BLE001 - report every invalid schema
            failures.append(f"schema {name}: {e}")

    for ref in refs(doc):
        node = doc
        try:
            for part in ref.removeprefix("#/").split("/"):
                node = node[part]
        except (KeyError, TypeError):
            failures.append(f"unresolved $ref {ref}")

    names = [m["name"] for m in doc["methods"]]
    if len(names) != len(set(names)):
        failures.append("duplicate method names")
    for method in doc["methods"]:
        params = {p["name"]: p["schema"] for p in method["params"]}
        for example in method.get("examples", []):
            label = f"{method['name']} example '{example['name']}'"
            for param in example["params"]:
                for e in make_validator(doc, params[param["name"]]).iter_errors(param["value"]):
                    failures.append(f"{label} param {param['name']}: {e.message}")
            for e in make_validator(doc, method["result"]["schema"]).iter_errors(
                example["result"]["value"]
            ):
                failures.append(f"{label} result: {e.message}")

    error_data = {"$ref": "#/components/schemas/ErrorData"}
    for name, error in doc["components"]["errors"].items():
        for e in make_validator(doc, error_data).iter_errors(error["data"]):
            failures.append(f"error {name} data: {e.message}")

    for schema_name, instance, expected, label in CASES:
        valid = make_validator(doc, {"$ref": f"#/components/schemas/{schema_name}"}).is_valid(
            instance
        )
        if valid != expected:
            failures.append(
                f"{schema_name}: {label}: expected {'valid' if expected else 'invalid'}"
            )

    for failure in failures:
        print("FAIL", failure)
    print(f"{len(names)} methods, {len(CASES)} cases, {len(failures)} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
