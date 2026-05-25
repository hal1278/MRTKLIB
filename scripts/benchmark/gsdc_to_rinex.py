"""gsdc_to_rinex.py - Convert a GSDC-2023 ``device_gnss.csv`` to RINEX 3.04 OBS.

The Google Smartphone Decimeter Challenge (GSDC) 2023 dataset ships its raw
measurements as ``device_gnss.csv`` - a *derived* CSV in which Google has already
reconstructed the pseudorange (``RawPseudorangeMeters``), classified the signal
(``SignalType``), and provided carrier phase (``AccumulatedDeltaRangeMeters``),
Doppler (``PseudorangeRateMetersPerSecond``) and C/N0 (``Cn0DbHz``).  Because the
pseudorange is already reconstructed (clock, week-rollover and inter-signal-bias
handling done), a direct CSV -> RINEX mapping is simpler and less error-prone than
re-deriving it from the raw Android clock fields.

This converter performs that mapping only; broadcast ephemeris (NAV) is acquired
separately (see ``download_brdc.py``) because the CSV carries no ephemeris.

Field mapping (per measurement row)
-----------------------------------
    C (pseudorange) = RawPseudorangeMeters                          [m]
    L (carrier)     = AccumulatedDeltaRangeMeters / lambda          [cycles]
    D (Doppler)     = -PseudorangeRateMetersPerSecond / lambda      [Hz]
    S (C/N0)        = Cn0DbHz                                        [dB-Hz]
    lambda          = c / CarrierFrequencyHz

The carrier phase is emitted only when ``AccumulatedDeltaRangeState`` has the
VALID bit set; the RINEX LLI flag is raised on a RESET or CYCLE_SLIP bit so the
downstream TDCP/cycle-slip logic (#116 P4) sees the discontinuity.

Epoch time
----------
GPS time (continuous) = utcTimeMillis/1000 - 315964800 + LeapSecond.  This was
cross-checked against the CSV's own ``ArrivalTimeNanosSinceGpsEpoch`` column and
agrees to sub-millisecond.

Usage
-----
    python gsdc_to_rinex.py <device_gnss.csv> -o <out.obs> [--marker NAME]
    python gsdc_to_rinex.py <device_gnss.csv> --self-check   # validate parsing
"""

import argparse
import csv
import math
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------
CLIGHT = 299792458.0
GPS_UNIX_EPOCH = 315964800.0  # Unix time (s) of 1980-01-06 00:00:00 UTC
GPS_EPOCH = datetime(1980, 1, 6)  # naive datetime, interpreted as GPST

# ConstellationType -> (RINEX system char, svid offset).  PRN = Svid - offset.
#   1 GPS, 2 SBAS, 3 GLONASS, 4 QZSS, 5 BeiDou, 6 Galileo, 7 IRNSS
# QZSS Android svid is 193-202 -> RINEX J01-J10 (offset 192).  SBAS/IRNSS are
# not used by the SPP benchmark and are dropped.
CONSTELLATION = {
    1: ("G", 0),
    3: ("R", 0),
    4: ("J", 192),
    5: ("C", 0),
    6: ("E", 0),
}

# Android SignalType -> (RINEX band char, RINEX attribute char).  Galileo E1 OS
# (pilot) maps to 1C and BeiDou B1I to 2I, per RINEX 3.04.  The list mirrors the
# signal universe observed across the GSDC-2023 archive (GPS/GAL/GLO/QZS/BDS).
SIGNAL_MAP = {
    "GPS_L1_CA": ("1", "C"),
    "GPS_L5_Q": ("5", "Q"),
    "GPS_L5_I": ("5", "I"),
    "GAL_E1_C_P": ("1", "C"),
    "GAL_E5A_Q": ("5", "Q"),
    "GAL_E5A_I": ("5", "I"),
    "GLO_G1_CA": ("1", "C"),
    "QZS_L1_CA": ("1", "C"),
    "QZS_L5_Q": ("5", "Q"),
    "QZS_L5_I": ("5", "I"),
    "BDS_B1_I": ("2", "I"),
}

# AccumulatedDeltaRangeState bits (metadata/accumulated_delta_range_state_bit_map.json)
ADR_VALID = 0x1
ADR_RESET = 0x2
ADR_CYCLE_SLIP = 0x4

OBS_KINDS = ("C", "L", "D", "S")  # observable order within a band


# ---------------------------------------------------------------------------
# CSV parsing
# ---------------------------------------------------------------------------
def _f(row: dict, key: str):
    """Parse a CSV cell as float, returning None when blank/invalid."""
    v = row.get(key, "")
    if v is None or v == "":
        return None
    try:
        return float(v)
    except ValueError:
        return None


def sat_id(const_type: int, svid: int) -> str | None:
    """Map (ConstellationType, Svid) to a RINEX 3 satellite id, or None."""
    if const_type not in CONSTELLATION:
        return None
    sys_char, offset = CONSTELLATION[const_type]
    prn = svid - offset
    if prn < 1 or prn > 99:
        return None
    return f"{sys_char}{prn:02d}"


def parse_csv(path: str) -> tuple[dict, dict]:
    """Parse a device_gnss.csv into per-epoch observation records.

    Args:
        path: Path to ``device_gnss.csv``.

    Returns:
        ``(epochs, sys_bands)`` where ``epochs`` is an ordered dict keyed by the
        integer ``utcTimeMillis`` mapping to ``{sat_id: {(band, attr): obs}}``
        (``obs`` is ``{"C","L","D","S","lli"}``), and ``sys_bands`` maps each
        RINEX system char to the sorted set of ``(band, attr)`` tuples seen.
    """
    epochs: dict[int, dict] = {}
    sys_bands: dict[str, set] = {}

    with open(path, newline="") as fh:
        reader = csv.DictReader(fh)
        for row in reader:
            sig = row.get("SignalType", "")
            if sig not in SIGNAL_MAP:
                continue
            pr = _f(row, "RawPseudorangeMeters")
            if pr is None or pr <= 0.0:
                continue
            try:
                const_type = int(float(row["ConstellationType"]))
                svid = int(float(row["Svid"]))
                tms = int(row["utcTimeMillis"])
            except (KeyError, ValueError):
                continue
            sid = sat_id(const_type, svid)
            if sid is None:
                continue

            band, attr = SIGNAL_MAP[sig]
            sysc = sid[0]
            sys_bands.setdefault(sysc, set()).add((band, attr))

            fc = _f(row, "CarrierFrequencyHz")
            lam = CLIGHT / fc if fc and fc > 0 else None

            # Carrier phase: only when ADR is valid; LLI on reset / cycle slip.
            ph = None
            lli = 0
            adr = _f(row, "AccumulatedDeltaRangeMeters")
            try:
                adr_state = int(float(row.get("AccumulatedDeltaRangeState", "0") or 0))
            except ValueError:
                adr_state = 0
            if lam and adr is not None and (adr_state & ADR_VALID):
                ph = adr / lam
                if adr_state & (ADR_RESET | ADR_CYCLE_SLIP):
                    lli = 1

            # Doppler [Hz] from pseudorange rate [m/s].
            dop = None
            prr = _f(row, "PseudorangeRateMetersPerSecond")
            if lam and prr is not None:
                dop = -prr / lam

            snr = _f(row, "Cn0DbHz")

            sat_map = epochs.setdefault(tms, {})
            obs = sat_map.setdefault(sid, {})
            obs[(band, attr)] = {"C": pr, "L": ph, "D": dop, "S": snr, "lli": lli}

    return dict(sorted(epochs.items())), sys_bands


# ---------------------------------------------------------------------------
# RINEX writing
# ---------------------------------------------------------------------------
def _gpst_calendar(utc_millis: int, leap: int) -> datetime:
    """Convert ``utcTimeMillis`` (UTC) to a GPST calendar datetime."""
    gps_total = utc_millis / 1000.0 - GPS_UNIX_EPOCH + leap
    return GPS_EPOCH + timedelta(seconds=gps_total)


def _snr2ssi(snr: float | None) -> int:
    """Map C/N0 [dB-Hz] to a RINEX signal-strength indicator (1-9), 0 if none."""
    if snr is None or snr <= 0:
        return 0
    return min(9, max(1, int(snr / 6.0)))


def _obs_field(val: float | None, lli: int, ssi: int) -> str:
    """Format one RINEX 3 observable: F14.3 value + LLI + SSI (16 chars)."""
    if val is None:
        return " " * 16
    return f"{val:14.3f}{lli if lli else ' '}{ssi if ssi else ' '}"


def _obs_types(sys_bands: dict[str, set]) -> dict[str, list[tuple[str, str, str]]]:
    """Build the ordered (kind, band, attr) obs-type list for each system."""
    out: dict[str, list] = {}
    for sysc, bands in sys_bands.items():
        codes = []
        for band, attr in sorted(bands):
            for kind in OBS_KINDS:
                codes.append((kind, band, attr))
        out[sysc] = codes
    return out


def _approx_xyz(csv_path: str) -> tuple[float, float, float]:
    """Median Google WLS position (ECEF) from the CSV, for APPROX POSITION XYZ."""
    xs, ys, zs = [], [], []
    with open(csv_path, newline="") as fh:
        for row in csv.DictReader(fh):
            x = _f(row, "WlsPositionXEcefMeters")
            y = _f(row, "WlsPositionYEcefMeters")
            z = _f(row, "WlsPositionZEcefMeters")
            if None not in (x, y, z) and (x or y or z):
                xs.append(x)
                ys.append(y)
                zs.append(z)
    if not xs:
        return (0.0, 0.0, 0.0)
    xs.sort()
    ys.sort()
    zs.sort()
    m = len(xs) // 2
    return (xs[m], ys[m], zs[m])


def write_rinex(
    epochs: dict,
    sys_bands: dict,
    out_path: str,
    marker: str,
    approx: tuple[float, float, float],
    leap: int = 18,
    interval: float = 1.0,
) -> int:
    """Write the parsed epochs to a RINEX 3.04 OBS file.

    Returns:
        Number of epoch records written.
    """
    obs_types = _obs_types(sys_bands)
    systems = sorted(obs_types)

    first_tms = next(iter(epochs))
    t0 = _gpst_calendar(first_tms, leap)
    now = datetime.now(timezone.utc).strftime("%Y%m%d %H%M%S UTC")

    lines = []

    def hdr(body: str, label: str) -> None:
        lines.append(f"{body:<60}{label}")

    hdr(f"{3.04:9.2f}{'':<11}OBSERVATION DATA{'':<4}M", "RINEX VERSION / TYPE")
    hdr(f"{'gsdc_to_rinex':<20}{'MRTKLIB':<20}{now:<20}", "PGM / RUN BY / DATE")
    hdr(f"{marker:<60}", "MARKER NAME")
    hdr(f"{'NON_GEODETIC':<20}", "MARKER TYPE")
    hdr(f"{'MRTKLIB':<20}{'MRTKLIB':<40}", "OBSERVER / AGENCY")
    hdr(f"{'0000':<20}{'Android':<20}{'1':<20}", "REC # / TYPE / VERS")
    hdr(f"{'0000':<20}{'NONE':<20}", "ANT # / TYPE")
    hdr(f"{approx[0]:14.4f}{approx[1]:14.4f}{approx[2]:14.4f}", "APPROX POSITION XYZ")
    hdr(f"{0.0:14.4f}{0.0:14.4f}{0.0:14.4f}", "ANTENNA: DELTA H/E/N")

    for sysc in systems:
        codes = obs_types[sysc]
        names = [f"{k}{b}{a}" for (k, b, a) in codes]
        # First line holds up to 13 codes; continuation lines are padded.
        first = names[:13]
        body = f"{sysc}  {len(names):3d}" + "".join(f" {n}" for n in first)
        hdr(body, "SYS / # / OBS TYPES")
        for i in range(13, len(names), 13):
            chunk = names[i : i + 13]
            body = " " * 6 + "".join(f" {n}" for n in chunk)
            hdr(body, "SYS / # / OBS TYPES")

    hdr(f"{interval:10.3f}", "INTERVAL")
    sec = t0.second + t0.microsecond / 1e6
    hdr(
        f"{t0.year:6d}{t0.month:6d}{t0.day:6d}{t0.hour:6d}{t0.minute:6d}{sec:13.7f}{'':5}GPS",
        "TIME OF FIRST OBS",
    )
    hdr("", "END OF HEADER")

    n_epoch = 0
    for tms, sat_map in epochs.items():
        t = _gpst_calendar(tms, leap)
        sec = t.second + t.microsecond / 1e6
        sats = sorted(sat_map)
        lines.append(
            f"> {t.year:4d} {t.month:02d} {t.day:02d} "
            f"{t.hour:02d} {t.minute:02d}{sec:11.7f}  0{len(sats):3d}"
        )
        for sid in sats:
            obs = sat_map[sid]
            row = sid
            for kind, band, attr in obs_types[sid[0]]:
                cell = obs.get((band, attr))
                if cell is None:
                    row += " " * 16
                    continue
                val = cell[kind]
                ssi = _snr2ssi(cell["S"])
                lli = cell["lli"] if kind == "L" else 0
                row += _obs_field(val, lli, ssi)
            lines.append(row.rstrip())
        n_epoch += 1

    Path(out_path).parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "w") as fh:
        fh.write("\n".join(lines) + "\n")
    return n_epoch


# ---------------------------------------------------------------------------
# Self-check: independent WLS from the CSV's own derived columns
# ---------------------------------------------------------------------------
def self_check(csv_path: str) -> int:
    """Reproduce Google's WLS from the CSV's pre-computed columns.

    Solves a per-epoch single-clock WLS using ``RawPseudorangeMeters`` plus the
    CSV's own ``SvPosition*``/``SvClockBias``/iono/tropo/Isrb corrections, and
    compares the solution to the ``WlsPosition*`` columns.  Agreement to ~metre
    level confirms the pseudorange / satellite-id / epoch-grouping reading that
    the converter relies on.  Pure validation; writes nothing.
    """
    OMGE = 7.2921151467e-5  # Earth rotation rate [rad/s]
    # One signal per satellite (prefer the lowest band) so a dual-frequency sat
    # is not double-counted; elevation-weighted to mirror a basic SPP estimator.
    _BAND_RANK = {"1": 0, "2": 1, "5": 2}
    by_epoch: dict[int, dict] = {}
    wls: dict[int, tuple] = {}

    with open(csv_path, newline="") as fh:
        for row in csv.DictReader(fh):
            sig = row.get("SignalType", "")
            if sig not in SIGNAL_MAP:
                continue
            pr = _f(row, "RawPseudorangeMeters")
            sx = _f(row, "SvPositionXEcefMeters")
            sy = _f(row, "SvPositionYEcefMeters")
            sz = _f(row, "SvPositionZEcefMeters")
            dts = _f(row, "SvClockBiasMeters")
            if None in (pr, sx, sy, sz, dts):
                continue
            try:
                tms = int(row["utcTimeMillis"])
                sid = sat_id(int(float(row["ConstellationType"])), int(float(row["Svid"])))
            except (KeyError, ValueError):
                continue
            if sid is None:
                continue
            iono = _f(row, "IonosphericDelayMeters") or 0.0
            trop = _f(row, "TroposphericDelayMeters") or 0.0
            isrb = _f(row, "IsrbMeters") or 0.0
            el = _f(row, "SvElevationDegrees")
            w = math.sin(math.radians(el)) ** 2 if el and el > 0 else 0.25
            # Corrected pseudorange referred to a single (GPS) receiver clock.
            prc = pr + dts - iono - trop - isrb
            band = SIGNAL_MAP[sig][0]
            sats = by_epoch.setdefault(tms, {})
            prev = sats.get(sid)
            if prev is None or _BAND_RANK.get(band, 9) < prev[4]:
                sats[sid] = (sx, sy, sz, prc, _BAND_RANK.get(band, 9), w)
            wx = _f(row, "WlsPositionXEcefMeters")
            wy = _f(row, "WlsPositionYEcefMeters")
            wz = _f(row, "WlsPositionZEcefMeters")
            if None not in (wx, wy, wz):
                wls[tms] = (wx, wy, wz)

    errs = []
    for tms, sats in by_epoch.items():
        meas = list(sats.values())
        if len(meas) < 5 or tms not in wls:
            continue
        x = list(wls[tms]) + [0.0]  # initial guess at Google WLS + zero clock
        reject: set = set()
        for it in range(12):
            H, r, w = [], [], []
            for k, (sx, sy, sz, prc, _rank, wt) in enumerate(meas):
                if k in reject:
                    continue
                dx, dy, dz = x[0] - sx, x[1] - sy, x[2] - sz
                rng = math.sqrt(dx * dx + dy * dy + dz * dz)
                sag = OMGE * (sx * x[1] - sy * x[0]) / CLIGHT  # Sagnac
                r.append(prc - (rng + sag + x[3]))
                H.append([dx / rng, dy / rng, dz / rng, 1.0])
                w.append(wt)
            dxv = _lsq(H, r, w)
            if dxv is None:
                break
            x = [x[i] + dxv[i] for i in range(4)]
            converged = max(abs(v) for v in dxv[:3]) < 1e-3
            if converged and it >= 1:
                # Single 3-sigma residual rejection pass, then resolve once more.
                res = []
                for k, (sx, sy, sz, prc, _rank, wt) in enumerate(meas):
                    if k in reject:
                        continue
                    dx, dy, dz = x[0] - sx, x[1] - sy, x[2] - sz
                    rng = math.sqrt(dx * dx + dy * dy + dz * dz)
                    sag = OMGE * (sx * x[1] - sy * x[0]) / CLIGHT
                    res.append((k, abs(prc - (rng + sag + x[3]))))
                rr = [v for _, v in res]
                sig_r = 1.4826 * sorted(rr)[len(rr) // 2] if rr else 0.0
                new = {k for k, v in res if sig_r > 0 and v > max(30.0, 3 * sig_r)}
                if not new:
                    break
                reject |= new
        wx, wy, wz = wls[tms]
        errs.append(math.sqrt((x[0] - wx) ** 2 + (x[1] - wy) ** 2 + (x[2] - wz) ** 2))

    if not errs:
        print("self-check: no usable epochs", file=sys.stderr)
        return 1
    errs.sort()
    n = len(errs)
    rms = math.sqrt(sum(e * e for e in errs) / n)
    p50 = errs[n // 2]
    print(f"self-check: {n} epochs, independent elev-weighted WLS vs Google WLS (3D)")
    print(f"  RMS  = {rms:8.3f} m")
    print(f"  p50  = {p50:8.3f} m")
    print(f"  p95  = {errs[min(n - 1, int(n * 0.95))]:8.3f} m")
    print(f"  max  = {errs[-1]:8.3f} m")
    # Judge on the median: a reading bug (wrong unit/sat-id/epoch) would make the
    # solution diverge from Google's by orders of magnitude, not a few metres.
    ok = p50 < 5.0
    print(f"  verdict: {'OK (CSV reading validated)' if ok else 'SUSPECT (median > 5 m)'}")
    return 0 if ok else 2


def _lsq(H: list[list[float]], r: list[float], w: list[float] | None = None) -> list[float] | None:
    """Tiny (optionally weighted) normal-equations least squares.

    ``H`` is m x 4, ``r`` length m, ``w`` optional per-row weights; returns the
    4-vector solution of ``min sum w*(H dx - r)^2``.
    """
    n = len(H[0])
    ata = [[0.0] * n for _ in range(n)]
    atb = [0.0] * n
    for i in range(len(H)):
        wi = w[i] if w else 1.0
        for a in range(n):
            atb[a] += wi * H[i][a] * r[i]
            for b in range(n):
                ata[a][b] += wi * H[i][a] * H[i][b]
    # Gauss-Jordan solve.
    for c in range(n):
        piv = ata[c][c]
        if abs(piv) < 1e-12:
            return None
        for b in range(n):
            ata[c][b] /= piv
        atb[c] /= piv
        for rr in range(n):
            if rr == c:
                continue
            f = ata[rr][c]
            for b in range(n):
                ata[rr][b] -= f * ata[c][b]
            atb[rr] -= f * atb[c]
    return atb


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def main() -> int:
    """Entry point for the GSDC device_gnss.csv -> RINEX converter."""
    p = argparse.ArgumentParser(description="Convert a GSDC-2023 device_gnss.csv to RINEX 3.04 OBS")
    p.add_argument("csv", help="path to device_gnss.csv")
    p.add_argument("-o", "--out", default="", help="output RINEX OBS path")
    p.add_argument("--marker", default="", help="MARKER NAME (default: derived from path)")
    p.add_argument("--leap", type=int, default=18, help="GPS-UTC leap seconds (default 18)")
    p.add_argument(
        "--self-check",
        action="store_true",
        help="validate CSV reading via an independent WLS vs the "
        "WlsPosition columns (writes no output)",
    )
    args = p.parse_args()

    if not Path(args.csv).is_file():
        sys.exit(f"FAIL: device_gnss.csv not found: {args.csv}")

    if args.self_check:
        return self_check(args.csv)

    out = args.out
    if not out:
        out = str(Path(args.csv).with_suffix(".obs"))
    marker = args.marker
    if not marker:
        # .../train/<trip>/<device>/device_gnss.csv -> <trip>_<device>
        parts = Path(args.csv).resolve().parts
        marker = "_".join(parts[-3:-1]) if len(parts) >= 3 else "GSDC"

    epochs, sys_bands = parse_csv(args.csv)
    if not epochs:
        sys.exit("FAIL: no usable measurements in CSV")
    approx = _approx_xyz(args.csv)
    n = write_rinex(epochs, sys_bands, out, marker[:60], approx, leap=args.leap)
    n_sat = sum(len(s) for s in epochs.values())
    print(f"wrote {out}")
    print(f"  epochs   : {n}")
    print(f"  sat-obs  : {n_sat}")
    print(f"  systems  : {' '.join(f'{s}({len(b)} bands)' for s, b in sorted(sys_bands.items()))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
