"""compare_gsdc.py - Compare mrtk NMEA output against a GSDC ground_truth.csv.

The GSDC-2023 reference track is ``ground_truth.csv`` (NovAtel SPAN, post-
processed).  This module parses it into the same timed-position record shape the
PPC comparator uses, then reuses ``compare_ppc``'s epoch matching and metric
computation so the two benchmarks report identical statistics.

ground_truth.csv columns (subset used)
--------------------------------------
    LatitudeDegrees, LongitudeDegrees, AltitudeMeters (WGS84 ellipsoidal),
    SpeedMps, BearingDegrees, UnixTimeMillis (UTC)

Usage
-----
    compare_gsdc.py --ref <ground_truth.csv> <result.nmea>
                    [--skip-epochs N] [--threshold M] [--plot]
"""

import argparse
import csv
import math
import os
import sys
from pathlib import Path

# Reuse the PPC comparator's NMEA parser, epoch matcher and metric computation.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare_ppc import _match_epochs, compute_metrics, parse_nmea, plot_results  # noqa: E402


def parse_ground_truth(csv_path: str) -> list[tuple[float, float, float, float]]:
    """Parse a GSDC ground_truth.csv into timed position records.

    Args:
        csv_path: Path to ``ground_truth.csv``.

    Returns:
        List of ``(utc_sod, lat_deg, lon_deg, h_ell)`` tuples sorted by
        ``utc_sod`` (UTC seconds-of-day from ``UnixTimeMillis``).
    """
    rows = []
    with open(csv_path, newline="") as fh:
        for r in csv.DictReader(fh):
            try:
                t = int(r["UnixTimeMillis"])
                lat = float(r["LatitudeDegrees"])
                lon = float(r["LongitudeDegrees"])
                h = float(r["AltitudeMeters"])
            except (KeyError, ValueError):
                continue
            rows.append(((t / 1000.0) % 86400.0, lat, lon, h))
    rows.sort(key=lambda x: x[0])
    return rows


def parse_ground_truth_velocity(csv_path: str) -> dict[float, tuple[float, float]]:
    """Parse ground-truth horizontal velocity, keyed by UTC seconds-of-day.

    Provided for the deferred TDCP velocity validation (#165 task 6); the
    position comparison does not use it.

    Returns:
        ``{utc_sod: (speed_mps, bearing_deg)}``.
    """
    out = {}
    with open(csv_path, newline="") as fh:
        for r in csv.DictReader(fh):
            try:
                t = int(r["UnixTimeMillis"])
                speed = float(r["SpeedMps"])
                bearing = float(r["BearingDegrees"])
            except (KeyError, ValueError):
                continue
            out[(t / 1000.0) % 86400.0] = (speed, bearing)
    return out


def _fmt(v: float, unit: str = "m", digits: int = 3) -> str:
    """Format a metric value, showing 'nan' for math.nan."""
    return "nan" if math.isnan(v) else f"{v:.{digits}f} {unit}"


def main() -> int:
    """Compare one NMEA result against one GSDC ground_truth.csv."""
    p = argparse.ArgumentParser(
        description="Compare mrtk NMEA output against a GSDC ground_truth.csv"
    )
    p.add_argument("--ref", required=True, metavar="CSV", help="GSDC ground_truth.csv")
    p.add_argument("result", metavar="NMEA", help="mrtk NMEA output file")
    p.add_argument("--skip-epochs", type=int, default=0,
                   help="initial epochs to skip (convergence exclusion, default 0)")
    p.add_argument("--threshold", type=float, default=2.0,
                   help="2D horizontal error threshold in metres (default 2.0)")
    p.add_argument("--plot", action="store_true", help="generate ENU time-series PNG")
    p.add_argument("--plot-out", default="", help="output path for plot")
    args = p.parse_args()

    if not os.path.isfile(args.ref):
        print(f"FAIL: ground truth not found: {args.ref}", file=sys.stderr)
        return 1
    if not os.path.isfile(args.result):
        print(f"FAIL: result not found: {args.result}", file=sys.stderr)
        return 1

    ref_rows = parse_ground_truth(args.ref)
    nmea_rows = parse_nmea(args.result)
    if not ref_rows:
        print("FAIL: no data in ground_truth.csv", file=sys.stderr)
        return 1
    if not nmea_rows:
        print("FAIL: no GGA sentences in result", file=sys.stderr)
        return 1

    pairs = _match_epochs(ref_rows, nmea_rows)
    if not pairs:
        print("FAIL: no matching epochs (check date / time range)", file=sys.stderr)
        return 1

    m = compute_metrics(pairs, args.skip_epochs, threshold_2d=args.threshold)
    if m is None:
        print("FAIL: no usable epochs after skip", file=sys.stderr)
        return 1

    thr = args.threshold
    thr_label = f"<{thr:.1f}m" if thr >= 1.0 else f"<{thr * 100:.0f}cm"
    score = (m["p50_2d_all"] + m["p95_2d_all"]) / 2.0
    ref_usable = max(0, len(ref_rows) - args.skip_epochs)
    coverage = m["n_matched"] / ref_usable * 100.0 if ref_usable else math.nan
    print(f"Reference : {args.ref}")
    print(f"Result    : {args.result}")
    print()
    print(f"Reference epochs : {ref_usable}")
    print(f"Matched epochs : {m['n_matched']}")
    print(f"Coverage       : {coverage:.1f}%")
    print(f"{thr_label} rate    : {m['thr_rate']:.1f}%")
    print(f"mean nSV       : {m['mean_sv_all']:.1f}")
    print()
    print("  2D horizontal error (all epochs):")
    print(f"    p50    : {_fmt(m['p50_2d_all'])}")
    print(f"    p95    : {_fmt(m['p95_2d_all'])}")
    print(f"    RMS    : {_fmt(m['rms_2d_all'])}")
    print(f"    Max    : {_fmt(m['max_2d_all'])}")
    print()
    print(f"  GSDC score (mean of p50 & p95) : {_fmt(score)}")
    print()
    print("  ENU RMS (all epochs):")
    print(f"    East/North/Up : {m['rms_e']:.3f} / {m['rms_n']:.3f} / {m['rms_u']:.3f} m")

    if args.plot:
        out = args.plot_out or (os.path.splitext(args.result)[0] + ".png")
        plot_results(m, title=os.path.basename(args.result), output_path=out)

    return 0


if __name__ == "__main__":
    sys.exit(main())
