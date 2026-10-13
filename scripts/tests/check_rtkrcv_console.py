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
        console.begin("error")
        console.read_until(lambda s: s.startswith("error\n"), "error command echo")
        # There need not be an error to signal entry; a bounded socket observation
        # window checks the streaming command remains active before interruption.
        console.sock.settimeout(0.12)
        try:
            console.raw += console.sock.recv(65536)
        except socket.timeout:
            pass
        console.check(PROMPT not in clean(console.raw), "error stays active until ctrl-C")
        console.interrupt()
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
        # D-43: non-console set is accepted with restart notice both stopped/running.
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
        console.expect("set pos1-elmask 15", "option pos1-elmask changed. restart to enable it")
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
        replay_checks(args)
        mutation_checks(args)
        password_checks(args)
    except Exception:
        traceback.print_exc()
        return 1
    print(f"PASS rtkrcv console ({time.monotonic() - started:.3f}s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
