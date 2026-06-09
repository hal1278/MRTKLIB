#!/usr/bin/env python3
"""Carrier-phase self-consistency analyzer for cssr2rtcm3 VRS/OSR output.

Decodes the RTCM3 (MSM7 + 1005) produced by ``mrtk cssr2rtcm3`` and looks for
discontinuities and excess noise in the *synthesized* base carrier phase — the
"phase residual large and unstable" failure mode flagged for the VRS path
(#97 / #98). It needs no broadcast ephemeris: the diagnostics are formed from
combinations in which the geometric range and satellite clock cancel.

Per satellite, two combinations are tracked over time (both in metres):

  GF  (geometry-free phase)   = phase_m(bandA) - phase_m(bandB)
        -> ionosphere + inter-frequency bias + carrier ambiguity.
           Varies smoothly with the ionosphere (mm/s). A step means a carrier
           discontinuity on one band (cycle slip, phase-bias wrap, compL/SIS
           glitch) — exactly what breaks the rover's DD ambiguity.

  CMC (code minus carrier)    = pseudorange(band) - phase_m(band)
        -> 2*ionosphere + (code bias - phase bias) + ambiguity.
           Also smooth; a step flags a slip on that signal.

The MSM rough range (shared per satellite) cancels in both, so geometry,
satellite orbit and clock all drop out and only the correction/ambiguity
content remains.

Usage:
    python3 osr_residual.py BRANCH.rtcm3 [--compare OTHER.rtcm3]
                            [--jump-floor 0.05]

The optional --compare runs the same analysis on a second file (e.g. a
develop build vs a fix branch) and prints the jump counts side by side.
"""

import argparse
import math
import os
import statistics
import sys

# Reuse the RTCM3 MSM7/1005 decoder that already ships in this directory.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import compare_rtcm3 as rtcm  # noqa: E402


def _band(signal: str) -> int:
    """Frequency band from an RTCM signal code ('1C'->1, '2L'->2, '5Q'->5)."""
    return int(signal[0]) if signal and signal[0].isdigit() else 0


def build_series(epochs: dict):
    """Reshape decoded epochs into per-(sat, signal) time series.

    Returns {sat_id: {signal: [(tow, pseudorange, phase_m, lock_time), ...]}}
    with each list sorted by tow.
    """
    series = {}
    for tow in sorted(epochs):
        for _sys, ep in epochs[tow].items():
            for o in ep.obs:
                if o.phase_m == 0.0 and o.pseudorange == 0.0:
                    continue
                series.setdefault(o.sat_id, {}).setdefault(o.signal, []).append(
                    (tow, o.pseudorange, o.phase_m, o.lock_time)
                )
    return series


def _consecutive_diffs(series, value_fn, dt=1.0, dt_tol=0.05):
    """First differences between epochs exactly ``dt`` apart.

    Returns (diffs, events) where events carries (tow, diff) for each gap so a
    flagged jump can be reported with its timestamp.
    """
    diffs, events = [], []
    for prev, cur in zip(series, series[1:]):
        if abs((cur[0] - prev[0]) - dt) > dt_tol:
            continue  # epoch gap — not a same-rate step, skip
        d = value_fn(cur) - value_fn(prev)
        diffs.append(d)
        events.append((cur[0], d))
    return diffs, events


def _mad(values):
    if not values:
        return 0.0
    med = statistics.median(values)
    return statistics.median([abs(v - med) for v in values]) or 0.0


def _jumps(events, diffs, jump_floor):
    """Robust jump detection: |d - median| > max(jump_floor, 6*MAD)."""
    if len(diffs) < 5:
        return []
    med = statistics.median(diffs)
    thr = max(jump_floor, 6.0 * _mad(diffs))
    return [(tow, d) for tow, d in events if abs(d - med) > thr]


def _detrend_rms(diffs):
    """RMS of the epoch-to-epoch change after removing its median rate.

    For a clean carrier this is the per-second jitter of the combination
    (should be small); a noisy/unstable correction inflates it.
    """
    if not diffs:
        return 0.0
    med = statistics.median(diffs)
    return math.sqrt(sum((d - med) ** 2 for d in diffs) / len(diffs))


def analyze(series, jump_floor):
    """Per-satellite GF / CMC discontinuity statistics."""
    rows = []
    for sat in sorted(series):
        sigs = series[sat]
        # primary dual-frequency pair: band 1 (L1/E1) + the next band present
        by_band = {}
        for sig, pts in sigs.items():
            by_band.setdefault(_band(sig), []).append((sig, len(pts)))
        if 1 not in by_band:
            continue
        sigA = max(by_band[1], key=lambda t: t[1])[0]
        upper = sorted(b for b in by_band if b > 1)
        if not upper:
            continue
        sigB = max(by_band[upper[0]], key=lambda t: t[1])[0]

        # align bandA / bandB phase by tow for the geometry-free combination
        pa = {t: pm for t, _p, pm, _l in sigs[sigA]}
        pb = {t: pm for t, _p, pm, _l in sigs[sigB]}
        common = sorted(set(pa) & set(pb))
        gf = [(t, pa[t] - pb[t]) for t in common]
        gf_d, gf_ev = _consecutive_diffs(gf, lambda x: x[1])
        gf_jumps = _jumps(gf_ev, gf_d, jump_floor)

        # code-minus-carrier on band A
        cmc = [(t, p - pm) for t, p, pm, _l in sigs[sigA]]
        cmc_d, cmc_ev = _consecutive_diffs(cmc, lambda x: x[1])
        cmc_jumps = _jumps(cmc_ev, cmc_d, jump_floor)

        # lock-time decreases (encoder slip flag, if populated)
        lt = [(t, lk) for t, _p, _pm, lk in sigs[sigA]]
        lt_resets = sum(1 for a, b in zip(lt, lt[1:]) if b[1] < a[1])

        rows.append(
            {
                "sat": sat,
                "pair": f"{sigA}-{sigB}",
                "n": len(common),
                "gf_jumps": gf_jumps,
                "gf_rms_mm": 1000.0 * _detrend_rms(gf_d),
                "cmc_jumps": cmc_jumps,
                "cmc_rms_mm": 1000.0 * _detrend_rms(cmc_d),
                "lock_resets": lt_resets,
            }
        )
    return rows


# GLONASS (1087) is intentionally omitted: cssr2rtcm3 does not emit it and the
# compare_rtcm3 obs decoder does not handle it, so the rest of this tool is
# GPS/GAL/QZS only — keep the dead-obs scan to the same scope.
_MSM7_SYS = {1077: "GPS", 1097: "GAL", 1117: "QZS"}
_MSM7_PREFIX = {"GPS": "G", "GAL": "E", "QZS": "J"}


def scan_dead_obs(msgs):
    """Count satellites emitted with an *invalid* MSM rough range (255).

    A satellite encoded with rough-range = 255 carries no usable pseudorange
    (and, sharing the rough range, no usable carrier either): the rover sees
    it in the mask but cannot position with it. These are silently dropped by
    the obs decoder, so they never reach the residual analysis above — yet
    they represent real geometry that cssr2rtcm3 advertised but did not fill
    (e.g. QZS satellites with no valid OSR pseudorange).

    Returns {(sys, sat_id): [n_epochs, n_invalid]}.
    """
    tally = {}
    for mtype, payload in msgs:
        sys = _MSM7_SYS.get(mtype)
        if sys is None:
            continue
        br = rtcm.BitReader(payload)
        for nbits in (12, 12, 30, 1, 3, 7, 2, 2, 1, 3):  # type..smoothing intv
            br.read_uint(nbits)
        sat_mask = br.read_uint(64)
        sig_mask = br.read_uint(32)
        sat_ids = [i + 1 for i in range(64) if sat_mask & (1 << (63 - i))]
        nsig = bin(sig_mask).count("1")
        if not sat_ids or nsig == 0:
            continue  # empty MSM (matches compare_rtcm3.parse_msm7's early return)
        for _ in range(len(sat_ids) * nsig):  # cell mask
            br.read_uint(1)
        prefix = _MSM7_PREFIX.get(sys, "?")
        for sid in sat_ids:
            rr_ms = br.read_uint(8)
            key = (sys, f"{prefix}{sid:02d}")
            t = tally.setdefault(key, [0, 0])
            t[0] += 1
            if rr_ms == 255:
                t[1] += 1
    return tally


def report_dead(tally):
    dead = {k: v for k, v in tally.items() if v[1] > 0}
    if not dead:
        return
    print("\n=== satellites emitted with invalid rough range (255 = no usable obs) ===")
    print(f"{'sys':<5}{'sat':<5}{'epochs':>8}{'invalid':>9}{'%':>7}")
    for (sys, sat), (n, bad) in sorted(dead.items(), key=lambda x: -x[1][1]):
        print(f"{sys:<5}{sat:<5}{n:>8}{bad:>9}{100.0 * bad / n:>6.0f}%")
    fully = [k for k, v in dead.items() if v[1] == v[0]]
    if fully:
        print(
            "  ALWAYS invalid (advertised but never usable): "
            + ", ".join(f"{s}" for _sys, s in sorted(fully))
        )


def report(rows, label):
    print(f"\n=== {label} — carrier-phase self-consistency ===")
    print(
        f"{'sat':<4}{'pair':<9}{'n':>5}{'GFjmp':>6}{'GFrms[mm]':>10}"
        f"{'CMCjmp':>7}{'CMCrms[mm]':>11}{'lockRst':>8}"
    )
    tot_gf = tot_cmc = 0
    for r in sorted(rows, key=lambda r: -len(r["gf_jumps"])):
        tot_gf += len(r["gf_jumps"])
        tot_cmc += len(r["cmc_jumps"])
        print(
            f"{r['sat']:<4}{r['pair']:<9}{r['n']:>5}{len(r['gf_jumps']):>6}"
            f"{r['gf_rms_mm']:>10.1f}{len(r['cmc_jumps']):>7}"
            f"{r['cmc_rms_mm']:>11.1f}{r['lock_resets']:>8}"
        )
    gf_rms = statistics.median([r["gf_rms_mm"] for r in rows]) if rows else 0.0
    print(
        f"\n  satellites={len(rows)}  total GF jumps={tot_gf}  "
        f"total CMC jumps={tot_cmc}  median GF jitter={gf_rms:.1f} mm/epoch"
    )
    # surface the biggest discontinuities for follow-up
    big = sorted(
        ((r["sat"], t, d) for r in rows for t, d in r["gf_jumps"]),
        key=lambda x: -abs(x[2]),
    )[:12]
    if big:
        print("  largest GF discontinuities (tow, sat, step[m]):")
        for sat, t, d in big:
            print(f"    tow={t:.0f}  {sat}  {d:+.3f} m  ({d / 0.190:+.1f} L1-cyc)")
    return tot_gf, tot_cmc


def run(path, jump_floor):
    msgs = rtcm.extract_rtcm3_from_file(path)
    epochs, refs = rtcm.group_by_epoch(msgs)
    base = refs[0] if refs else None
    if base:
        print(
            f"{os.path.basename(path)}: {len(epochs)} epochs, "
            f"base XYZ=({base.x:.3f}, {base.y:.3f}, {base.z:.3f})"
        )
    rows = analyze(build_series(epochs), jump_floor)
    report_dead(scan_dead_obs(msgs))
    return rows


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("rtcm3", help="cssr2rtcm3 RTCM3 output (MSM7 + 1005)")
    ap.add_argument("--compare", help="second RTCM3 file for side-by-side jump counts")
    ap.add_argument(
        "--jump-floor", type=float, default=0.05, help="min step [m] to flag (default 0.05)"
    )
    args = ap.parse_args()

    rows = run(args.rtcm3, args.jump_floor)
    gf, cmc = report(rows, os.path.basename(args.rtcm3))

    if args.compare:
        rows2 = run(args.compare, args.jump_floor)
        gf2, cmc2 = report(rows2, os.path.basename(args.compare))
        print("\n=== A/B summary (GF jumps / CMC jumps) ===")
        print(f"  {os.path.basename(args.rtcm3):<30} GF={gf:<5} CMC={cmc}")
        print(f"  {os.path.basename(args.compare):<30} GF={gf2:<5} CMC={cmc2}")


if __name__ == "__main__":
    main()
