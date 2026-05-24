"""download_brdc.py - Download daily merged multi-GNSS broadcast ephemeris.

The GSDC ``device_gnss.csv`` carries no broadcast ephemeris, so the SPP
benchmark needs an external broadcast-navigation file per UTC day.  This module
fetches the daily *merged* multi-GNSS (GPS+GLO+GAL+BDS+QZS) RINEX 3 navigation
file from BKG, which serves it without authentication (CDDIS requires an
Earthdata login; GSSC returned 403 from CI).

Archive URL (BKG)
-----------------
    https://igs.bkg.bund.de/root_ftp/IGS/BRDC/{year}/{doy}/{name}_{year}{doy}0000_01D_MN.rnx.gz

``name`` candidates are tried in order; the first that exists is used:
    BRDM00DLR_S  - DLR merged multi-GNSS (most complete)
    BRDC00WRD_S  - BKG combined broadcast
    BRDC00IGS_R  - IGS combined broadcast

Requires only the Python standard library.

Usage
-----
    python download_brdc.py --date 2020-06-25 [--brdc-dir DIR] [--dry-run]
    python download_brdc.py --year 2020 --doy 177
"""

import argparse
import gzip
import shutil
import sys
import urllib.error
import urllib.request
from datetime import date, datetime
from pathlib import Path

_BKG_URL = (
    "https://igs.bkg.bund.de/root_ftp/IGS/BRDC/{year}/{doy:03d}/"
    "{name}_{year}{doy:03d}0000_01D_MN.rnx.gz"
)
# Tried in priority order; first existing wins.
_BRDC_NAMES = ["BRDM00DLR_S", "BRDC00WRD_S", "BRDC00IGS_R"]


def ymd_to_doy(d: date) -> int:
    """Return the day-of-year for a date."""
    return d.timetuple().tm_yday


def _local_path(brdc_dir: Path, name: str, year: int, doy: int) -> Path:
    """Local (uncompressed) path for a given product name/day."""
    return brdc_dir / f"{name}_{year}{doy:03d}0000_01D_MN.rnx"


def find_cached(brdc_dir: Path, year: int, doy: int) -> Path | None:
    """Return a cached broadcast-nav file for the day, or None."""
    for name in _BRDC_NAMES:
        p = _local_path(brdc_dir, name, year, doy)
        if p.is_file() and p.stat().st_size > 0:
            return p
    return None


def ensure_brdc(year: int, doy: int, brdc_dir: Path, dry_run: bool = False) -> Path | None:
    """Ensure a broadcast-nav file for ``(year, doy)`` exists locally.

    Args:
        year: Four-digit year.
        doy: Day of year (1-366).
        brdc_dir: Cache directory.
        dry_run: Print URLs without downloading.

    Returns:
        Path to the local uncompressed RINEX nav file, or None if unavailable.
    """
    cached = find_cached(brdc_dir, year, doy)
    if cached is not None:
        return cached
    brdc_dir.mkdir(parents=True, exist_ok=True)

    for name in _BRDC_NAMES:
        url = _BKG_URL.format(year=year, doy=doy, name=name)
        if dry_run:
            print(f"  [dry-run] {url}")
            return None
        dest = _local_path(brdc_dir, name, year, doy)
        gz = dest.with_suffix(dest.suffix + ".gz")
        try:
            urllib.request.urlretrieve(url, gz)
            with gzip.open(gz, "rb") as fin, open(dest, "wb") as fout:
                shutil.copyfileobj(fin, fout)
            gz.unlink(missing_ok=True)
            if dest.stat().st_size > 0:
                print(f"  downloaded {dest.name}")
                return dest
        except (urllib.error.HTTPError, urllib.error.URLError, OSError, EOFError):
            gz.unlink(missing_ok=True)
            dest.unlink(missing_ok=True)
            continue

    print(f"  FAIL: no broadcast nav found for {year} doy {doy}", file=sys.stderr)
    return None


def _parse_args_to_day(args: argparse.Namespace) -> tuple[int, int]:
    """Resolve CLI args to a (year, doy) pair."""
    if args.date:
        d = datetime.strptime(args.date, "%Y-%m-%d").date()
        return d.year, ymd_to_doy(d)
    if args.year and args.doy:
        return args.year, args.doy
    sys.exit("FAIL: provide --date YYYY-MM-DD, or both --year and --doy")


def main() -> int:
    """Download one day's merged broadcast navigation file."""
    p = argparse.ArgumentParser(description="Download daily merged multi-GNSS broadcast nav")
    p.add_argument("--date", default="", help="UTC date YYYY-MM-DD")
    p.add_argument("--year", type=int, default=0, help="four-digit year")
    p.add_argument("--doy", type=int, default=0, help="day of year (1-366)")
    p.add_argument("--brdc-dir", default="data/gsdc/brdc", help="cache directory")
    p.add_argument("--dry-run", action="store_true", help="print URL without downloading")
    args = p.parse_args()

    year, doy = _parse_args_to_day(args)
    brdc_dir = Path(args.brdc_dir)
    if not brdc_dir.is_absolute():
        brdc_dir = Path(__file__).resolve().parent.parent.parent / brdc_dir

    print(f"Broadcast nav for {year} doy {doy:03d} -> {brdc_dir}")
    path = ensure_brdc(year, doy, brdc_dir, dry_run=args.dry_run)
    if path:
        print(f"  ready: {path.name}")
    return 0 if (path or args.dry_run) else 1


if __name__ == "__main__":
    sys.exit(main())
