#!/usr/bin/env python3
"""Characterize the existing mrtk run telnet interface (Python 3.10+, stdlib only).

Run after regression_setup has extracted the MALIB data:
    python3 scripts/tests/check_rtkrcv_console.py --binary build/mrtk --root .
To deliberately regenerate the four interface goldens from the current build,
add --update-golden, then review the diff. Help retains whitespace and one LF per
line (excluding its version line). Each option line pins its name and optional
" # (<comment>)", without its value; status lists contain one label per line.
Numeric replay values are never compared with a reference.
"""

import argparse
import os
from pathlib import Path
import re
import shlex
import signal
import socket
import subprocess
import sys
import tempfile
import time
import traceback
from typing import Any

PROMPT = "rtkrcv> "
CLEAR = "\x1b[H\x1b[2J"
DEADLINE = 5.0
# Upper bound for a shell gate left behind by a failed test; generous so that a
# slow machine never releases a transition before the test does.
GATE_LIMIT = 60.0
SAT = r"[GREJCIS]\d{2}"
DATE = r"\d{4}/\d{2}/\d{2} \d{2}:\d{2}:\d{2}"
VERSION = r"rtkrcv \(MRTKLIB ver\.\S+ git \S+\)"

# Copied verbatim from h-shiono/mrtklib-docker-ui commit 8848daa,
# src/mrtklib_web_ui/services/mrtk_run_service.py. Keep these client regexes
# independent of the stricter printing-layout checks below.
_POS_LLH_PATTERN = re.compile(r"pos llh.*rover\s*:\s*([-\d.]+),([-\d.]+),([-\d.]+)")
_SOL_STATUS_PATTERN = re.compile(r"solution status\s*:\s*(\S+)")
_NSAT_PATTERN = re.compile(r"# of valid satellites\s*:\s*(\d+)")
_RATIO_PATTERN = re.compile(r"ratio for ar validation\s*:\s*([\d.]+)")
_AGE_PATTERN = re.compile(r"age of differential.*:\s*([\d.]+)")
_TIME_PATTERN = re.compile(r"time of receiver clock rover\s*:\s*(.+)")
_STATE_PATTERN = re.compile(r"rtk server state\s*:\s*(\S+)")
_STREAM_ROW_PATTERN = re.compile(
    r"^input([123])\s+(\S+)\s+(\S*)\s+[-CE]\s+(-?\d+)\s+(-?\d+)\s+-?\d+\s+-?\d+\s(.{24})\s?(.*)$"
)
_INPUT_DATA_PATTERN = re.compile(r"# of input([123]) data\s*:\s*(.*)")
_INPUT_COUNT_PATTERN = re.compile(r"(\w+)\((\d+)\)")


# Copied verbatim from h-shiono/mrtklib-docker-ui commit 8848daa,
# src/mrtklib_web_ui/services/mrtk_run_service.py, lines 53-192.
# License: MIT
# Copyright (c) H. SHIONO
# Preserve the client bodies and formatting so changes are easy to compare.
# fmt: off
SOL_QUALITY_MAP = {
    "-": 0, "fix": 1, "float": 2, "SBAS": 3,
    "DGPS": 4, "single": 5, "PPP": 6,
}

# Satellite system map from ID prefix
_SAT_SYSTEM_MAP = {
    "G": "GPS", "R": "GLONASS", "E": "Galileo",
    "J": "QZSS", "C": "BeiDou", "S": "SBAS", "I": "NavIC",
}


# Solution qualities (SOL_QUALITY_MAP values) computed by the RTK / PPP /
# VRS / PPP-RTK filter, whose ns counts the satellites valid on L1. The others
# (single, SBAS, none) come from pntpos, whose ns counts C1.
_FILTER_QUALITIES = {SOL_QUALITY_MAP["fix"], SOL_QUALITY_MAP["float"],
                     SOL_QUALITY_MAP["DGPS"], SOL_QUALITY_MAP["PPP"]}


def parse_satellite_output(text: str, solution_quality: int | None = None) -> list[dict[str, Any]]:
    """Parse mrtk run `satellite` command output into per-satellite data.

    Columns (rtkrcv prsatellite, nf frequencies, 2 by default):
    SAT C1 Az El L1..Lnf Fix1..Fixnf P1Res.. L1Res.. Sl.. Lock.. Rj..

    - C1 (`valid`): rtk.ssat.vs, valid in the single point (SPP) solution.
    - L1 (`vsat[0]`): valid in the RTK / PPP / VRS / PPP-RTK filter on L1.
      MRTKLIB counts the solution's ns as these satellites.

    `used` matches the solution's ns (#120): the L1 column when the current
    solution came from the filter, else C1. `solution_quality` is the
    `status` quality polled just before. It decides, because the L1 flags
    are not cleared when an epoch stops before the filter (no base data, age
    of differential too large): they still show the previous epoch while ns
    is the SPP count. Without it, any L1 flag is taken to mean the filter ran.
    """
    text = re.sub(r'\x1b\[[0-9;]*[a-zA-Z]', '', text)
    text = re.sub(r'[\xff\xfb\xfc\xfd\xfe][\x00-\xff]', '', text)
    satellites: list[dict[str, Any]] = []
    nf = 2  # rtkrcv's default; the header says how many L columns follow El

    for line in text.split('\n'):
        line = line.strip()
        if line.startswith('SAT'):
            n = sum(1 for t in line.split() if re.fullmatch(r'L\d', t))
            if n > 0:
                nf = n
            continue
        if not line or line.startswith('password'):
            continue
        parts = line.split()
        if len(parts) < 4:
            continue
        prn = parts[0]
        # Validate: first char should be satellite system letter
        if not prn or prn[0] not in _SAT_SYSTEM_MAP:
            continue
        try:
            valid = parts[1] == "OK"
            az = float(parts[2])
            el = float(parts[3])
        except (ValueError, IndexError):
            continue

        vsat_l1 = len(parts) > 4 and parts[4] == "OK"
        # Ambiguity state from the Fix columns (after the nf L columns)
        fix = "NONE"
        for p in parts[4 + nf:4 + 2 * nf]:
            if p in ("FIX", "FLOAT", "HOLD"):
                fix = p
                break

        system = _SAT_SYSTEM_MAP.get(prn[0], "Unknown")
        satellites.append({
            "prn": prn,
            "system": system,
            "azimuth": az,
            "elevation": el,
            "valid": valid,
            "vsat_l1": vsat_l1,
            "fix": fix,
            "snr": 0.0,  # Will be filled from observ command
        })

    if solution_quality is None:
        filter_active = any(s["vsat_l1"] for s in satellites)
    else:
        filter_active = solution_quality in _FILTER_QUALITIES
    for s in satellites:
        s["used"] = s.pop("vsat_l1") if filter_active else s["valid"]
    return satellites


def parse_observ_snr(text: str) -> dict[str, float]:
    """Parse mrtk run `observ` command output to extract SNR per satellite.
    Returns dict mapping PRN -> max SNR (dB-Hz) across frequencies.
    Only parses rover observations (rcv=1).
    """
    text = re.sub(r'\x1b\[[0-9;]*[a-zA-Z]', '', text)
    text = re.sub(r'[\xff\xfb\xfc\xfd\xfe][\x00-\xff]', '', text)
    snr_map: dict[str, float] = {}

    for line in text.split('\n'):
        line = line.strip()
        if not line or line.startswith('TIME') or line.startswith('password'):
            continue
        parts = line.split()
        if len(parts) < 6:
            continue
        # Format: YYYY/MM/DD HH:MM:SS.ss PRN RCV C1 C2 S1 S2 ...
        prn = parts[2]
        if not prn or prn[0] not in _SAT_SYSTEM_MAP:
            continue
        try:
            rcv = int(parts[3])
        except ValueError:
            continue
        if rcv != 1:  # rover only
            continue
        # Find SNR values (S1, S2, ... columns follow code columns)
        # Codes are 2-char, SNR are numeric
        snr_vals: list[float] = []
        for p in parts[4:]:
            try:
                v = float(p)
                # SNR values are typically 0-60, pseudoranges are large
                if 0 < v <= 60:
                    snr_vals.append(v)
                elif v > 1000:
                    break  # hit pseudorange columns
            except ValueError:
                continue
        if snr_vals:
            snr_map[prn] = max(snr_vals)

    return snr_map
# fmt: on


def clean(raw, ansi=True):
    """Remove telnet frames, CRLF, and the terminal's space/backspace echo refresh."""
    text = re.sub(rb"\xff[\xfb-\xfe][\x00-\xff]", b"", raw).decode("latin1")
    text = text.replace(" \b", "").replace("\r\n", "\n")
    if ansi:
        text = re.sub(r"\x1b\[[0-9;]*[a-zA-Z]", "", text)
    return text


class Console:
    def __init__(self, sock):
        self.sock = sock
        self.raw = b""
        self.command = "<connect>"
        self.received = ""

    def check(self, condition, description, text=None):
        if not condition:
            raise AssertionError(
                f"command={self.command!r}: {description}\n"
                f"received={self.received if text is None else text!r}\nraw={self.raw!r}"
            )

    def read_until(self, predicate, description, timeout=DEADLINE, eof=False):
        end = time.monotonic() + timeout
        while True:
            self.received = clean(self.raw)
            if predicate(self.received):
                return self.received
            remaining = end - time.monotonic()
            self.check(remaining > 0, f"deadline waiting for {description}")
            self.sock.settimeout(remaining)
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                self.check(False, f"deadline waiting for {description}")
            except ConnectionResetError:
                chunk = b""
            if not chunk:
                self.check(eof, f"unexpected connection close waiting for {description}")
                return self.received
            self.raw += chunk

    def begin(self, command):
        self.command = command
        self.raw = b""
        self.received = ""
        self.sock.sendall(command.encode("ascii") + b"\r")

    def response(self):
        text = self.read_until(lambda s: s.endswith(PROMPT), "command prompt")
        self.check(text.startswith(self.command + "\n"), "command echoed completely")
        return text[len(self.command) + 1 : -len(PROMPT)]

    def run(self, command):
        self.begin(command)
        return self.response()

    def expect(self, command, expected):
        text = self.run(command)
        self.check(text == expected + "\n", f"response equals {expected!r}")
        return text

    def interrupt(self):
        self.sock.sendall(b"\x03")
        text = self.response()
        self.check("^C\n" in text, "ctrl-C prints ^C and returns to prompt")
        return text

    def close_command(self, command):
        self.begin(command)
        text = self.read_until(lambda s: False, "EOF or reset", eof=True)
        self.check(text == command + "\n", "exit echoes command and closes connection")


class Server:
    """Own the process, sockets, stderr, and every file produced by one run."""

    def __init__(self, args, start=False, password=""):
        self.args = args
        self.start = start
        self.password = password
        self.sockets = []

    def __enter__(self):
        self.temp = tempfile.TemporaryDirectory(prefix="rtkrcv_console_")
        self.directory = Path(self.temp.name)
        config = (self.args.root / "conf/malib/rtkrcv.toml").read_text()
        config = config.replace(
            "./tests/data/malib/", (self.args.root / "tests/data/malib").as_posix() + "/"
        )
        config = re.sub(
            r'(\[streams.output.stream1\][^\[]*?path\s*=\s*)"[^"]*"',
            lambda m: m[1] + '"' + (self.directory / "solution.pos").as_posix() + '"',
            config,
            count=1,
        )
        config = config.replace('passwd = ""', f'passwd = "{self.password}"')
        (self.directory / "replay.toml").write_text(config)
        self.stderr = (self.directory / "stderr").open("w+b")
        command = [
            str(self.args.binary),
            "run",
            "-p",
            str(self.args.port),
            "-w",
            self.password,
            "-o",
            "replay.toml",
        ]
        if self.start:
            command.append("-s")
        try:
            self.process = subprocess.Popen(
                command,
                cwd=self.directory,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=self.stderr,
            )
        except BaseException:
            self.stderr.close()
            self.temp.cleanup()
            raise
        return self

    def __exit__(self, exc_type, exc, tb):
        for sock in self.sockets:
            sock.close()
        if self.process.poll() is None:
            # Kill directly on failures: a broken console must not block cleanup.
            self.process.kill()
        self.process.wait(timeout=DEADLINE)
        if exc_type:
            self.stderr.seek(0)
            print(
                f"mrtk stderr ({self.directory}):\n{self.stderr.read().decode(errors='replace')}",
                file=sys.stderr,
            )
        self.stderr.close()
        self.temp.cleanup()

    def connect(self):
        end = time.monotonic() + DEADLINE
        while True:
            try:
                # D-24: connect only to loopback; deliberately do not pin bind address.
                sock = socket.create_connection(("127.0.0.1", self.args.port), timeout=0.2)
                break
            except ConnectionRefusedError:
                if self.process.poll() is not None or time.monotonic() >= end:
                    raise AssertionError(
                        "command='<connect>': process exited or connection deadline; received=b''"
                    )
                # Connection refusal has no readiness notification; bounded retry backoff.
                time.sleep(0.01)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sockets.append(sock)
        console = Console(sock)
        suffix = "password: " if self.password else PROMPT
        text = console.read_until(lambda s: s.endswith(suffix), suffix)
        console.check(
            console.raw.startswith(b"\xff\xfb\x03\xff\xfb\x01"), "WILL SGA, WILL ECHO negotiation"
        )
        console.check(
            re.fullmatch(
                r"\n\*\* rtkrcv\(MRTKLIB ver\.\S+ git \S+\) console \(h:help\) \*\*\n"
                + re.escape(suffix),
                text,
            ),
            "welcome line and prompt layout",
        )
        if not self.password:
            console.check("password:" not in text, "empty -w/config password skips login")
        return console

    def shutdown(self, console):
        console.begin("shutdown")
        text = console.read_until(lambda s: False, "shutdown EOF/reset", eof=True)
        console.check(text == "shutdown\nrtk server shutdown ...\n", "shutdown response")
        try:
            result = self.process.wait(timeout=DEADLINE)
        except subprocess.TimeoutExpired:
            console.check(False, "shutdown process exit deadline")
        console.check(result == 0, f"shutdown exit status is 0 (received {result})")


def golden(args, console, name, text):
    path = args.root / "tests/data/rtkrcv_console" / name
    if args.update_golden:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
    else:
        console.check(path.read_text(encoding="utf-8") == text, f"golden {name} matches", text)


def lines(text):
    return [line for line in text.splitlines() if line]


class PrintfRow:
    """Check printed fields without relying on whitespace to separate them."""

    def __init__(self, console, row):
        self.console = console
        self.row = row
        self.offset = 0

    def literal(self, expected):
        field = self.row[self.offset : self.offset + len(expected)]
        self.console.check(
            field == expected, f"literal {expected!r} at column {self.offset}", self.row
        )
        self.offset += len(expected)

    def string(self, width, *, allowed=None, pattern=None, align=">", separator=" ", minimum=False):
        self.literal(separator)
        if minimum:
            candidates = [
                format(value, f"{align}{width}")
                for value in allowed
                if self.row[self.offset :].startswith(format(value, f"{align}{width}"))
            ]
            self.console.check(candidates, f"known string field %{width}s", self.row)
            field = max(candidates, key=len)
        else:
            field = self.row[self.offset : self.offset + width]
        value = field.strip()
        self.console.check(
            field == format(value, f"{align}{width}"),
            f"string field %{('-' if align == '<' else '')}{width}s at column {self.offset}",
            self.row,
        )
        if allowed is not None:
            self.console.check(value in allowed, f"string field in {allowed!r}", self.row)
        if pattern is not None:
            self.console.check(
                re.fullmatch(pattern, value), f"string field pattern {pattern}", self.row
            )
        self.offset += len(field)
        return value

    def number(self, spec, *, separator=" ", minimum=False):
        self.literal(separator)
        precision = re.search(r"\.(\d+)", spec)
        if spec.endswith("d") or (spec.endswith("f") and precision and precision[1] == "0"):
            pattern = r" *-?\d+"
        elif spec.endswith("X"):
            pattern = r"[0-9A-F]+"
        else:
            pattern = rf" *-?\d+\.\d{{{precision[1]}}}"
            if spec.endswith("E"):
                pattern += r"E[+-]\d{2,}"
        if minimum:
            # With an explicit separator, printf's width is a minimum. Preserve
            # legitimate overflow (e.g. a negative %6.4f SSR clock coefficient).
            match = re.match(pattern, self.row[self.offset :])
            self.console.check(match, f"numeric field %{spec} at column {self.offset}", self.row)
            field = match[0]
        else:
            width = int(re.match(r"\d+", spec)[0])
            field = self.row[self.offset : self.offset + width]
            self.console.check(
                re.fullmatch(pattern, field),
                f"fixed numeric field %{spec} at column {self.offset}",
                self.row,
            )
        value = (
            int(field, 16)
            if spec.endswith("X")
            else int(field)
            if spec.endswith("d")
            else float(field)
        )
        self.console.check(
            field == format(value, spec),
            f"numeric field equals value reformatted as %{spec} at column {self.offset}",
            self.row,
        )
        self.offset += len(field)
        return value

    def end(self):
        self.console.check(
            self.offset == len(self.row), "no missing or extra printed fields", self.row
        )


def wait_command(console, command, predicate, description="replay data appeared"):
    end = time.monotonic() + DEADLINE
    while True:
        text = console.run(command)
        if predicate(text):
            return text
        console.check(time.monotonic() < end, f"{description} before deadline")


def status(console, text, args=None):
    labels = []
    for line in lines(text):
        console.check(
            re.fullmatch(r".{28}: .*", line), "status label column is 28 characters", text
        )
        labels.append(line[:28].rstrip())
    if args:
        # Pin the list produced by this PPP MALIB replay, including any conditional labels.
        golden(args, console, "status-labels.txt", "\n".join(labels) + "\n")
    for pattern in (
        _POS_LLH_PATTERN,
        _SOL_STATUS_PATTERN,
        _NSAT_PATTERN,
        _RATIO_PATTERN,
        _AGE_PATTERN,
        _TIME_PATTERN,
        _STATE_PATTERN,
    ):
        console.check(pattern.search(text), f"docker-ui status parser {pattern.pattern}", text)
    counts = _INPUT_DATA_PATTERN.findall(text)
    console.check(
        [i for i, _ in counts] == ["1", "2", "3"], "docker-ui extracts all input count lines", text
    )
    for _, values in counts:
        console.check(
            "err" in dict(_INPUT_COUNT_PATTERN.findall(values)),
            "docker-ui extracts err input counter",
            text,
        )


def satellite(console, text, nf, solution_quality, system=None):
    table = lines(text)
    frequencies = [1, 2, 5][:nf]
    header = ["SAT", "C1", "Az", "El"]
    for template in ("L{}", "Fix{}", "P{}Res", "L{}Res", "Sl{}", "Lock{}", "Rj{}"):
        header += [template.format(f) for f in frequencies]
    console.check(table[0].split() == header, "satellite header", text)
    console.check(len(table) > 1, "at least one satellite row", text)
    structural_prns = set()
    for line in table[1:]:
        row = PrintfRow(console, line)
        prn = row.string(3, pattern=SAT, separator="")
        structural_prns.add(prn)
        row.string(2, allowed=("OK", "-"))
        row.number("5.1f")
        row.number("4.1f")
        for _ in range(nf):
            row.string(2, allowed=("OK", "-"))
        for _ in range(nf):
            row.string(5, allowed=("FIX", "FLOAT", "HOLD", "-"))
        # prsatellite has no separator before/between these residual fields.
        for spec in ("7.3f", "8.4f"):
            for _ in range(nf):
                row.number(spec, separator="")
        for spec in ("4d", "6d", "3d"):
            for _ in range(nf):
                row.number(spec, minimum=True)
        row.end()
        if system:
            console.check(prn.startswith(system), "satellite system filter", text)
    raw_decoded = console.raw.decode("utf-8", errors="replace")
    parsed = parse_satellite_output(raw_decoded, solution_quality)
    console.check(bool(parsed), "docker-ui extracts at least one satellite", raw_decoded)
    console.check(
        all(sat["prn"] in structural_prns for sat in parsed),
        "every docker-ui satellite PRN appears in the structural parse",
        raw_decoded,
    )
    # The verbatim parser keeps nf local. Check the raw header it derives nf from
    # separately rather than instrumenting or changing its body.
    raw_header = next(
        (
            line.strip()
            for line in re.sub(r"\x1b\[[0-9;]*[a-zA-Z]", "", raw_decoded).split("\n")
            if line.strip().startswith("SAT")
        ),
        "",
    )
    derived_nf = sum(1 for token in raw_header.split() if re.fullmatch(r"L\d", token))
    console.check(derived_nf == nf, f"docker-ui header-derived nf is {nf}", raw_decoded)


def observ(console, text, nf):
    table = lines(text)
    header = ["TIME(GPST)", "SAT", "R"]
    for template in ("C{}", "S{}", "P{}(m)", "L{}(cyc)", "D{}(Hz)"):
        header += [template.format(i) for i in range(1, nf + 1)]
    header += ["LLI"]
    console.check(table[0].split() == header, "observ header", text)
    console.check(len(table) > 1, "at least one observation row", text)
    for line in table[1:]:
        row = PrintfRow(console, line)
        row.string(22, pattern=DATE + r"\.\d{2}", separator="")
        row.string(3, pattern=SAT)
        row.string(1, allowed=("1", "2"))
        for _ in range(nf):
            row.string(3, pattern=r"(?:[1-9][A-Z])?", separator="")
        # Every numeric group is adjacent, including Doppler. Full-width fields
        # and blank codes must not be interpreted as missing/merged tokens.
        for spec in ("3.0f", "13.3f", "14.3f", "8.1f"):
            for _ in range(nf):
                row.number(spec, separator="")
        row.literal(" ")
        for _ in range(nf):
            row.number("1X", separator="")
        row.end()
    raw_decoded = console.raw.decode("utf-8", errors="replace")
    console.check(
        bool(parse_observ_snr(raw_decoded)),
        "docker-ui extracts at least one rover SNR value",
        raw_decoded,
    )


def navigation(console, text):
    table = lines(text)
    console.check(
        table[0].split() == "SAT S IOD IOC FRQ A/A SVH Toe Toc Ttr/Tof L2C L2P".split(),
        "navidata header",
        text,
    )
    rows = table[1:-2]
    for line in rows:
        row = PrintfRow(console, line)
        prn = row.string(3, pattern=rf"(?:{SAT}|\d{{3}})", separator="")
        row.string(3, allowed=("OK", "-"))
        for _ in range(4):
            row.number("3d", minimum=True)
        if prn.startswith("R"):
            row.number("02X", separator="  ", minimum=True)
        else:
            row.number("03X", minimum=True)
        # Date strings contain a space; '-' still occupies the full %19s field.
        for _ in range(3):
            row.string(19, pattern=rf"(?:{DATE}|-)")
        for _ in range(2):
            row.number("3d", minimum=True)
        row.end()
    ion = PrintfRow(console, table[-2])
    ion.literal("ION:")
    for _ in range(8):
        ion.number("9.2E", minimum=True)
    ion.end()
    utc = PrintfRow(console, table[-1])
    utc.literal("UTC:")
    for _ in range(4):
        utc.number("9.2E", minimum=True)
    utc.literal("  LEAPS: ")
    utc.number(".0f", separator="", minimum=True)
    utc.end()
    print(f"navidata: {len(rows)} rows")


def ssr(console, text):
    table = lines(text)
    console.check(
        table[0].split()
        == "SAT S UDI IOD URA T0 D0-A D0-C D0-R D1-A D1-C D1-R C0 C1 C2 C-HR".split(),
        "ssr header",
        text,
    )
    for line in table[1:]:
        row = PrintfRow(console, line)
        row.string(3, pattern=SAT, separator="")
        row.string(3, allowed=("OK", "-"))
        row.number("3.0f", minimum=True)
        row.number("3d", minimum=True)
        row.number("3d", minimum=True)
        row.string(19, pattern=DATE)
        for spec in ("6.3f",) * 6 + ("8.3f", "6.3f", "6.4f", "6.3f"):
            row.number(spec, minimum=True)
        row.end()
    print(f"ssr: {len(table) - 1} rows")


def stream(console, text):
    table = lines(text)
    expected = "input1 input2 input3 output1 output2 log1 log2 log3 monitor".split()
    console.check(
        table[0]
        == f"{'Stream':12} {'Type':8} {'Fmt':5} S {'In-byte':>10} {'In-bps':>7} {'Out-byte':>10} {'Out-bps':>7} {'Path':24} Message",
        "stream header fixed widths",
        text,
    )
    console.check(len(table) == 10, "nine stream rows", text)
    for name, line in zip(expected, table[1:]):
        row = PrintfRow(console, line)
        row.string(12, allowed=(name,), align="<", separator="")
        row.string(
            8,
            align="<",
            minimum=True,
            allowed="- serial file tcpsvr tcpcli ntripsvr ntripcli ftp http ntripcas udpsvr udpcli".split(),
        )
        row.string(
            5,
            align="<",
            minimum=True,
            allowed=[""]
            + "- rtcm2 rtcm3 oem4 oem3 ubx ss2 hemis skytreq javad nvs binex rt17 sbf sp3 stat l6e clas llh xyz enu nmea".split(),
        )
        row.string(1, allowed=("-", "C", "E"))
        for spec in ("10d", "7d", "10d", "7d"):
            row.number(spec, minimum=True)
        row.literal(" ")
        # %-24.24s is exactly 24 characters. Spaces inside Path and the trailing
        # %s Message are content, so neither is split into whitespace tokens.
        path = line[row.offset : row.offset + 24]
        console.check(len(path) == 24, "stream Path exactly 24 characters", text)
        row.offset += 24
        row.literal(" ")
        row.offset = len(line)  # Message may be empty or contain any number of spaces.
        row.end()
    parsed = [_STREAM_ROW_PATTERN.match(row.rstrip("\r")) for row in table[1:4]]
    console.check(
        all(parsed) and [m[1] for m in parsed] == ["1", "2", "3"],
        "docker-ui parses three input rows",
        text,
    )
    console.check(parsed[1][2] == "-", "docker-ui input2 type is -", text)


def options(console, text):
    entries = []
    for row in lines(text):
        match = re.fullmatch(r"([ *])([\w-]+)( *) =(.*)", row)
        console.check(match, "option marker/name/value layout", text)
        name = match[2]
        console.check(match[2] + match[3] == f"{name:18}", "option name minimum width 18", text)
        comment = ""
        if " # " in match[4]:
            comment_match = re.search(r" # \((.+)\)$", row)
            console.check(comment_match, "option comment parentheses", text)
            comment = comment_match[1]
            before_comment = row[: row.index(" # ")]
            console.check(
                before_comment == before_comment.rstrip().ljust(31),
                "option comment padding to column 31 (including marker)",
                text,
            )
        # Empty comments are recorded too: loss of a formerly present comment
        # changes this option's golden entry rather than bypassing the check.
        entries.append((name, comment))
    console.check(bool(entries), "option list nonempty", text)
    return entries


def help_checks(args, console):
    help_body = None
    for command in ("help", "?", "h"):
        text = console.run(command)
        version, _, body = text.partition("\n")
        console.check(re.fullmatch(VERSION, version), "help version line", text)
        if help_body is not None:
            console.check(body == help_body, "help aliases produce identical text", text)
        help_body = body
        golden(args, console, "help.txt", body)
    golden(args, console, "help-path.txt", console.run("help path"))
    console.expect("help xyz", "unknown help: xyz")


def periodic(console, command, header):
    console.begin(command)
    console.read_until(lambda s: s.count(header) >= 2, "two periodic displays")
    console.check(
        clean(console.raw, ansi=False).count(CLEAR) >= 2, "clear-screen sequence each cycle"
    )
    console.check(PROMPT not in console.received, "periodic display has no prompt before ctrl-C")
    console.interrupt()


def poll_solution_quality(console):
    console.run("status")
    raw_decoded = console.raw.decode("utf-8", errors="replace")
    match = _SOL_STATUS_PATTERN.search(raw_decoded)
    console.check(
        match and match[1] in SOL_QUALITY_MAP,
        "docker-ui status parse supplies the satellite solution quality",
        raw_decoded,
    )
    return SOL_QUALITY_MAP[match[1]]


def replay_checks(args):
    with Server(args, start=True) as server:
        console = server.connect()
        wait_command(console, "satellite", lambda s: len(lines(s)) > 1)
        for command, nf, system in (
            ("satellite", 2, None),
            ("satellite -3", 3, None),
            ("satellite -G", 2, "G"),
        ):
            quality = poll_solution_quality(console)
            satellite(console, console.run(command), nf, quality, system)
        status(console, console.run("status"), args)
        status(console, console.run("sta"))
        observ(console, console.run("observ"), 3)
        observ(console, console.run("observ -2"), 2)
        navigation(console, console.run("navidata"))
        ssr(console, console.run("ssr"))
        stream(console, console.run("stream"))
        help_checks(args, console)
        text = console.run("option")
        entries = options(console, text)
        golden(
            args,
            console,
            "option-names.txt",
            "\n".join(name + (f" # ({comment})" if comment else "") for name, comment in entries)
            + "\n",
        )
        filtered = options(console, console.run("option console-"))
        console.check(
            filtered == [entry for entry in entries if "console-" in entry[0]],
            "option substring filter including comments",
        )
        console.expect("foo", "unknown command: foo.")
        console.check(console.run("s") == "", "s abbreviation does nothing")
        console.check(console.run("shutd") == "", "shutd does not shut down")
        periodic(console, "status 0.05", "Parameter")
        periodic(console, "satellite 0.05", "Fix1")
        # D-20: error clears the shared buffer on entry and on each running read.
        # ctrl-C is sent right behind the command line, so error's first break check
        # (vt_chkbrk) reads it. Had error returned at once, its prompt would come
        # before ^C; the exact response shows that it ran until ctrl-C, without timing.
        console.begin("error")
        console.sock.sendall(b"\x03")
        text = console.response()
        console.check(text == "^C\n\n", "error runs until ctrl-C", text)
        other = server.connect()
        status(other, other.run("status"))
        status(console, console.run("status"))
        other.close_command("exit")
        console.check(server.process.poll() is None, "exit leaves server running")
        third = server.connect()
        third.command = "<ctrl-D>"
        third.raw = b""
        third.sock.sendall(b"\x04")
        third.read_until(lambda s: False, "ctrl-D EOF/reset", eof=True)
        third.check(third.received == "", "empty-line ctrl-D closes without response")
        third.check(server.process.poll() is None, "ctrl-D leaves process running")
        fourth = server.connect()
        status(fourth, fourth.run("status"))
        fourth.close_command("exit")
        if os.name == "nt":
            print("SKIP !command: POSIX shell escape unavailable on Windows")
        else:
            console.expect("!echo console-shell-marker", "console-shell-marker")
            text = console.run("!false")
            console.check(
                re.fullmatch(r"command exec error \([1-9]\d*\)\n", text),
                "failing shell exit status response",
                text,
            )
        server.shutdown(console)


def solution_row(console, text, soltype, timetype, flag):
    row = PrintfRow(console, text)
    if timetype == 3:
        row.literal("  ")
        row.number("04d", separator="", minimum=True)
        row.number("9.2f", minimum=True)
    else:
        decimals = 1 if timetype == 0 else 2
        row.string(20 + decimals, pattern=DATE + rf"\.\d{{{decimals}}}", separator="")
    row.literal(" (")
    row.string(
        6,
        allowed=("------", "FIX", "FLOAT", "SBAS", "DGPS", "SINGLE", "PPP", ""),
        align="<",
        separator="",
    )
    row.literal(")")
    if soltype in (0, 1):
        for hemisphere, spec in (("NS", "2.0f"), ("EW", "3.0f")):
            row.string(1, allowed=hemisphere)
            row.literal(":")
            if soltype == 0:
                row.number(spec, separator="", minimum=True)
                row.number("02.0f", minimum=True)
                row.number("07.4f", minimum=True)
            else:
                row.number("11.8f" if hemisphere == "NS" else "12.8f", separator="", minimum=True)
        row.literal(" H:")
        row.number("8.3f", separator="", minimum=True)
    else:
        for label in {2: "XYZ", 3: "ENU", 4: "PYL"}[soltype]:
            row.literal(f" {label}:")
            row.number("12.3f", separator="", minimum=True)
    if flag & 1:
        labels = "NEU" if soltype == 0 else "XYZ" if soltype == 2 else "ENU"
        row.literal(" (")
        for i, label in enumerate(labels):
            row.literal((" " if i else "") + label + ":")
            row.number("6.3f", separator="", minimum=True)
        row.literal("m)" if soltype == 1 else ")")
    if flag & 2:
        for label, spec in (("A", "4.1f"), ("R", "5.1f"), ("N", "2d")):
            row.literal(f" {label}:")
            row.number(spec, separator="", minimum=True)
    row.end()


def solution_checks(console):
    for soltype in range(5):
        console.expect(f"set console-soltype {soltype}", "option console-soltype changed.")
        for timetype in range(4):
            console.expect(f"set console-timetype {timetype}", "option console-timetype changed.")
            for flag in (0, 1, 2, 3) if timetype == 0 else (0,):
                console.expect(f"set console-solflag {flag}", "option console-solflag changed.")
                # D-17: each one-shot read drains the process-wide solution buffer.
                # Retry through complete prompts until a fresh replay solution exists.
                text = wait_command(console, "solution", lambda s: bool(s.strip()))
                for row in lines(text):
                    solution_row(console, row, soltype, timetype, flag)


def overwrite(console, command, path, answer):
    console.begin(command)
    console.read_until(lambda s: s.endswith("? (y/n): "), "overwrite question")
    console.check(
        console.received == command + "\n" + f"overwrite {path:16} ? (y/n): ",
        "overwrite prompt layout",
    )
    console.sock.sendall(answer.encode() + b"\r")
    return console.response()


def mutation_checks(args):
    with Server(args) as server:
        console = server.connect()
        text = console.run("status")
        console.check(_STATE_PATTERN.search(text)[1] == "stop", "initial server state stop", text)
        console.expect("set", "specify option type")
        console.expect("set no-such-option 1", "no option type: no-such-option")
        console.expect("set console-soltype bogus", "invalid option value: console-soltype bogus")
        console.expect("set console-soltype 1", "option console-soltype changed.")
        console.check(
            console.run("option console-soltype").startswith(" "),
            "console option has no modified mark",
        )
        # Stopped changes retain the existing restart notice and modified mark.
        console.expect("set pos1-elmask 15", "option pos1-elmask changed. restart to enable it")
        console.check(
            console.run("option pos1-elmask").startswith("*"), "non-console option modified mark"
        )
        console.begin("set console-solflag")
        console.read_until(lambda s: s.endswith(": "), "interactive option value prompt")
        console.check(
            console.received
            == "set console-solflag\nconsole-solflag (0:off,1:std+2:age/ratio/ns): ",
            "interactive set prompt",
        )
        console.sock.sendall(b"0\r")
        console.check(
            console.response().endswith("0\noption console-solflag changed.\n"),
            "interactive set value/response",
        )
        text = console.run("start")
        console.check(text.endswith("rtk server start\n"), "start response", text)
        wait_command(
            console,
            "status",
            lambda s: _STATE_PATTERN.search(s)[1] == "run",
            "state run after start",
        )
        before = console.run("option pos1-elmask")
        console.expect("set pos1-elmask 10", "option not changed: stop rtk server first")
        console.check(console.run("option pos1-elmask") == before, "running set preserves option")
        solution_checks(console)
        clock = _TIME_PATTERN.search(console.run("status"))[1]
        wait_command(console, "status", lambda s: _TIME_PATTERN.search(s)[1] != clock)
        text = console.run("stop")
        console.check(text.endswith("rtk server stop\n"), "stop response", text)
        console.check(
            _STATE_PATTERN.search(console.run("status"))[1] == "stop", "state stop after stop"
        )
        # D-17: stopping production makes shared-buffer draining deterministic.
        first = console.run("solution")
        console.check(bool(first.strip()), "first console consumes buffered solutions", first)
        second = server.connect()
        console.check(
            second.run("solution") == "", "another console sees drained shared solution buffer"
        )
        console.check(console.run("solution") == "", "same console also sees drained buffer")
        second.close_command("exit")
        print(f"shared solution buffer drained after stop: {len(lines(first))} buffered rows")
        output = (server.directory / "solution.pos").as_posix()
        text = overwrite(console, "start", output, "n")
        console.check(text.endswith("n\n"), "overwrite refusal releases start claim", text)
        console.expect("stop", "rtk server stop")
        text = overwrite(console, "restart", output, "y")
        console.check(text.endswith("rtk server restart\n"), "restart response", text)
        wait_command(
            console,
            "status",
            lambda s: _STATE_PATTERN.search(s)[1] == "run",
            "state run after restart",
        )
        console.run("stop")
        for path in ("saved.conf", "saved.toml"):
            # D-11/P5: TOML contents will change; don't pin issue #342's dropped options.
            console.expect(f"save {path}", f"options saved to {path}")
            console.check((server.directory / path).is_file(), "save creates file")
        saved = server.directory / "saved.conf"
        before = saved.read_bytes()
        text = overwrite(console, "save saved.conf", "saved.conf", "n")
        console.check(
            text.endswith("n\n") and saved.read_bytes() == before,
            "save overwrite n preserves file",
            text,
        )
        saved.write_text("overwrite sentinel\n")
        text = overwrite(console, "save saved.conf", "saved.conf", "y")
        console.check(
            text.endswith("y\noptions saved to saved.conf\n")
            and saved.read_text() != "overwrite sentinel\n",
            "save overwrite y replaces file",
            text,
        )
        console.expect("load saved.conf", "options loaded from saved.conf. restart to enable them")
        # D-37: failed load resets processing options today; pin only its response.
        console.expect("load absent.conf", "no options file: absent.conf")
        console.expect("log", "specify log file")
        console.expect("log console.log", "log on: console.log")
        console.expect("foo", "unknown command: foo.")
        console.expect("log off", "log off")
        log = server.directory / "console.log"
        console.check(
            b"unknown command: foo." in log.read_bytes(), "following console output is logged"
        )
        before = log.read_bytes()
        text = overwrite(console, "log console.log", "console.log", "n")
        console.check(
            text.endswith("n\n") and log.read_bytes() == before,
            "log overwrite n preserves file",
            text,
        )
        text = overwrite(console, "log console.log", "console.log", "y")
        console.check(text.endswith("y\nlog on: console.log\n"), "log overwrite y opens log", text)
        console.expect("log off", "log off")
        server.shutdown(console)


def transitional_displays(console):
    """Check refused snapshots keep the existing display structure."""
    text = console.run("status")
    status(console, text)
    fields = {line[:28].rstrip(): line[30:] for line in lines(text)}
    console.check(fields["rtk server state"] == "stop", "transitional status state", text)
    expected_labels = (
        Path(__file__).resolve().parents[2] / "tests/data/rtkrcv_console/status-labels.txt"
    )
    console.check(
        list(fields) == expected_labels.read_text().splitlines(), "status labels intact", text
    )
    for label in (
        "rtk server thread",
        "processing cycle (ms)",
        "cpu time for a cycle (ms)",
        "missing obs data count",
        "# of satellites rover",
        "# of satellites base",
        "# of valid satellites",
        "# of real estimated states",
        "# of all estimated states",
    ):
        console.check(fields[label] == "0", f"zero {label} during transition", text)
    for _, counts in _INPUT_DATA_PATTERN.findall(text):
        console.check(
            all(int(value) == 0 for _, value in _INPUT_COUNT_PATTERN.findall(counts)),
            "zero input counters during transition",
            text,
        )
    console.check(fields["bytes in input buffer"] == "0,0", "zero input buffers", text)
    console.check(fields["pos xyz float (m) rover"] == "0.000,0.000,0.000", "zero vector", text)
    console.check(fields["time of receiver clock rover"] == "-", "zero receiver time", text)
    for command, header in (
        ("satellite -1", "SAT C1 Az El L1 Fix1 P1Res L1Res Sl1 Lock1 Rj1"),
        ("stream", "Stream Type Fmt S In-byte In-bps Out-byte Out-bps Path Message"),
        ("observ -1", "TIME(GPST) SAT R C1 S1 P1(m) L1(cyc) D1(Hz) LLI"),
        ("ssr", "SAT S UDI IOD URA T0 D0-A D0-C D0-R D1-A D1-C D1-R C0 C1 C2 C-HR"),
    ):
        text = console.run(command)
        console.check(
            len(lines(text)) == 1 and lines(text)[0].split() == header.split(),
            f"{command} has only its header during transition",
            text,
        )
    console.check(console.run("solution") == "", "refused solution prints nothing")
    console.begin("error")
    console.sock.sendall(b"\x03")
    console.check(console.response() == "^C\n\n", "refused error has no error rows")


def configuration_refusals(console, path, before):
    """Check changes are refused while console settings remain available."""
    console.expect("set pos1-elmask 10", "option not changed: stop rtk server first")
    console.expect(f"load {path}", "options not loaded: stop rtk server first")
    console.expect("load absent.conf", "options not loaded: stop rtk server first")
    text = console.run("option pos1-elmask")
    console.check(text == before and text.startswith(" "), "refusal preserves value and mark", text)
    console.expect("set console-soltype 1", "option console-soltype changed.")
    text = console.run("option console-soltype")
    console.check(
        text.startswith(" ") and re.search(r"=\s*deg\b", text),
        "console change applies without a modified mark",
        text,
    )
    console.expect("set pos1-elmask", "option not changed: stop rtk server first")


def file_gate(entered, release, timeout=GATE_LIMIT):
    """Build a file gate that also exits on directory cleanup or bounded polling."""
    entered_path = shlex.quote(str(entered))
    release_path = shlex.quote(str(release))
    directory = shlex.quote(str(entered.parent))
    polls = max(1, int(timeout / 0.05))
    script = (
        f"touch {entered_path}; remaining={polls}; "
        f'while [ ! -e {release_path} ] && [ -d {directory} ] && [ "$remaining" -gt 0 ]; do '
        "remaining=$((remaining - 1)); sleep 0.05; done; "
        f"[ -e {release_path} ]"
    )
    return "sh -c " + shlex.quote(script)


def file_gate_checks():
    """Check release, directory removal and timeout each terminate a shell gate."""
    for outcome in ("release", "cleanup", "timeout"):
        with tempfile.TemporaryDirectory(prefix="rtkrcv_gate_") as directory:
            root = Path(directory)
            entered, release = root / "entered", root / "go"
            process = subprocess.Popen(
                file_gate(entered, release, timeout=0.1 if outcome == "timeout" else DEADLINE),
                shell=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            try:
                end = time.monotonic() + DEADLINE
                while not entered.exists():
                    if process.poll() is not None or time.monotonic() >= end:
                        raise AssertionError(f"{outcome} file gate did not create its marker")
                if outcome == "release":
                    release.touch()
                elif outcome == "cleanup":
                    entered.unlink()
                    root.rmdir()
                result = process.wait(timeout=DEADLINE / 2 if outcome == "cleanup" else DEADLINE)
                if result != (0 if outcome == "release" else 1):
                    raise AssertionError(f"{outcome} file gate returned {result}")
            finally:
                if root.exists():
                    release.touch()
                process.wait(timeout=DEADLINE)


def lifecycle_checks(args):
    """Hold transitions at file gates and check a second console's responses."""
    with Server(args) as server:
        first = server.connect()
        second = server.connect()
        first.expect("stop", "rtk server stop")
        first.expect("set outstr1-type 0", "option outstr1-type changed. restart to enable it")
        first.expect(
            "set pos1-correction bds-b2b", "option pos1-correction changed. restart to enable it"
        )
        first.expect("start", "error: correction source not implemented yet (correction=bds-b2b)")
        second.expect("stop", "rtk server stop")
        first.expect(
            "set pos1-correction qzs-madoca", "option pos1-correction changed. restart to enable it"
        )
        load_path = server.directory / "stage2.conf"
        load_path.write_text("pos1-elmask =10\nconsole-soltype =2\n")
        before = second.run("option pos1-elmask")
        gates = {}
        for phase in ("start", "stop"):
            entered = server.directory / f"entered-{phase}"
            release = server.directory / f"go-{phase}"
            command = file_gate(entered, release)
            first.begin(f"set misc-{phase}cmd")
            first.read_until(lambda s: s.endswith(": "), "interactive command value prompt")
            first.sock.sendall(command.encode("ascii") + b"\r")
            text = first.response()
            first.check(
                text.endswith(f"option misc-{phase}cmd changed. restart to enable it\n"),
                "file gate command configured",
                text,
            )
            gates[phase] = (entered, release)

        def reached(phase, observer=second):
            # Each complete status prompt yields to the server; the file proves arrival.
            wait_command(
                observer,
                "status",
                lambda _: gates[phase][0].exists(),
                f"{phase} command marker",
            )

        def busy(state):
            for command in ("start", "stop", "restart"):
                second.expect(command, f"rtk server busy ({state})")
            transitional_displays(second)
            configuration_refusals(second, load_path, before)

        try:
            first.begin("start")
            reached("start")
            busy("starting")
            options(second, second.run("option"))
            second.expect("save starting.conf", "options saved to starting.conf")
            second.check(
                (server.directory / "starting.conf").is_file(), "save during start completes"
            )
            login_console = server.connect()
            login_console.close_command("exit")
            gates["start"][1].touch()
            text = first.response()
            first.check(text.endswith("rtk server start\n"), "released start succeeds", text)
            gates["start"][0].unlink()
            second.expect("start", "rtk server start error (server already started)")
            second.check(not gates["start"][0].exists(), "double start skips start preparation")
            second.check(
                _STATE_PATTERN.search(second.run("status"))[1] == "run", "start published run"
            )
            configuration_refusals(second, load_path, before)

            first.begin("stop")
            reached("stop")
            busy("stopping")
            gates["stop"][1].touch()
            text = first.response()
            first.check(text == "stop rtk server\nrtk server stop\n", "released stop notices", text)
            second.expect("stop", "rtk server stop")
            second.check(
                _STATE_PATTERN.search(second.run("status"))[1] == "stop", "stop published stop"
            )

            # Recheck application when a start claims during an interactive prompt.
            second.begin("set pos1-elmask")
            second.read_until(lambda s: s.endswith(": "), "stopped interactive set prompt")
            gates["start"][1].unlink()
            first.begin("restart")
            observer = server.connect()
            reached("start", observer)
            second.sock.sendall(b"10\r")
            text = second.response()
            second.check(
                text.endswith("10\noption not changed: stop rtk server first\n"),
                "interactive set rechecks after a start claims",
                text,
            )
            busy("starting")
            gates["start"][1].touch()
            text = first.response()
            first.check(text.endswith("rtk server restart\n"), "stopped restart succeeds", text)
            # system() may inherit the observer socket until the shell exits.
            observer.close_command("exit")

            # A running restart owns both stopping and starting without a claim gap.
            for entered, release in gates.values():
                entered.unlink()
                release.unlink()
            first.begin("restart")
            reached("stop")
            busy("stopping")
            gates["stop"][1].touch()
            reached("start")
            busy("starting")
            gates["start"][1].touch()
            text = first.response()
            first.check(
                text.endswith("stop rtk server\nrtk server restart\n"),
                "running restart keeps stop and restart notices",
                text,
            )
            second.expect("start", "rtk server start error (server already started)")
            first.run("stop")
            second.expect("stop", "rtk server stop")
            server.shutdown(first)
        finally:
            for _, release in gates.values():
                release.touch()


def shutdown_checks(args):
    """Shutdown during a file-gated start through telnet and a signal."""
    for source in ("telnet", "signal"):
        with Server(args) as server:
            first = server.connect()
            second = server.connect()
            third = server.connect()
            first.expect("set outstr1-type 0", "option outstr1-type changed. restart to enable it")
            entered = server.directory / "entered-start"
            release = server.directory / "go-start"
            stopped = server.directory / "shutdown-stopped"
            for option, command in (
                (
                    "misc-startcmd",
                    file_gate(entered, release),
                ),
                ("misc-stopcmd", f"touch {stopped}"),
            ):
                first.begin(f"set {option}")
                first.read_until(lambda s: s.endswith(": "), "interactive command value prompt")
                first.sock.sendall(command.encode("ascii") + b"\r")
                text = first.response()
                first.check(
                    text.endswith(f"option {option} changed. restart to enable it\n"),
                    "shutdown marker command configured",
                    text,
                )
            try:
                first.begin("start")
                wait_command(third, "status", lambda _: entered.exists(), "start command marker")
                if source == "telnet":
                    second.begin("shutdown")
                    text = second.read_until(
                        lambda s: s.endswith("rtk server shutdown ...\n"),
                        "shutdown request notice",
                    )
                    second.check(
                        text == "shutdown\nrtk server shutdown ...\n",
                        "shutdown accepted during start",
                    )
                else:
                    server.process.send_signal(signal.SIGTERM)
                    wait_command(
                        third,
                        "start",
                        lambda s: s == "rtk server shutting down\n",
                        "signal shutdown request recorded",
                    )
                for command in ("start", "stop", "restart"):
                    third.expect(command, "rtk server shutting down")
                transitional_displays(third)
                third.check(
                    server.process.poll() is None, "shutdown waits for the start transition"
                )
                third.check(not stopped.exists(), "shutdown has not stopped the unfinished start")
                release.touch()
                try:
                    result = server.process.wait(timeout=DEADLINE)
                except subprocess.TimeoutExpired:
                    third.check(False, f"{source} shutdown process exit deadline")
                third.check(result == 0, f"{source} shutdown exit status is 0 (received {result})")
                third.check(stopped.is_file(), "final shutdown executed the stop command")
                if source == "telnet":
                    text = second.read_until(lambda s: False, "shutdown EOF/reset", eof=True)
                    second.check(text == "shutdown\nrtk server shutdown ...\n", "shutdown response")
            finally:
                release.touch()


def password_checks(args):
    # Match CLI and config passwords: don't pin the known startup precedence bug.
    with Server(args, password="console-secret") as server:
        console = server.connect()
        console.command = "<wrong password>"
        console.raw = b""
        console.sock.sendall(b"wrong-password\r")
        text = console.read_until(lambda s: s.endswith("password: "), "retry password prompt")
        console.check(
            text == "*" * len("wrong-password") + "\n\ninvalid password\npassword: ",
            "exact masked wrong password echo and retry response",
        )
        console.command = "<correct password>"
        console.raw = b""
        console.sock.sendall(b"console-secret\r")
        text = console.read_until(lambda s: s.endswith(PROMPT), "authenticated command prompt")
        console.check(
            text == "*" * len("console-secret") + "\n" + PROMPT,
            "exact masked correct password echo and command prompt",
        )
        other = server.connect()
        console.expect("set console-passwd updated-secret", "option console-passwd changed.")
        other.command = "<updated password>"
        other.raw = b""
        other.sock.sendall(b"updated-secret\r")
        text = other.read_until(lambda s: s.endswith(PROMPT), "updated password login prompt")
        other.check(
            text == "*" * len("updated-secret") + "\n" + PROMPT,
            "login copies the current password after input",
        )
        other.close_command("exit")
        server.shutdown(console)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--port", type=int, default=52008)
    parser.add_argument("--update-golden", action="store_true")
    args = parser.parse_args()
    args.binary = args.binary.resolve()
    args.root = args.root.resolve()
    started = time.monotonic()
    try:
        file_gate_checks()
        replay_checks(args)
        mutation_checks(args)
        lifecycle_checks(args)
        shutdown_checks(args)
        password_checks(args)
    except Exception:
        traceback.print_exc()
        return 1
    print(f"PASS rtkrcv console ({time.monotonic() - started:.3f}s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
