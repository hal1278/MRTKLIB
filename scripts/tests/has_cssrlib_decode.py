#!/usr/bin/env python3
"""Independent "golden" decode of Galileo HAS corrections via cssrlib.

Produces a JSONL reference dump for cross-validating the MRTKLIB C HAS
decoder. Read-only with respect to the MRTKLIB repo except for the output
files. Drives the cssrlib reference implementation (cssr_has / cnav_msg)
directly so the page-collection + RS(255,32) decode and the per-block
CSSR decode are all cssrlib's own code.

cssrlib is an external run-time dependency of this script only (not bundled
with, linked into, or copied into MRTKLIB): MIT License, Copyright (c) 2021
Rui Hirokawa, https://github.com/hirokawa/cssrlib.

Input format (G5P3162a.has): fixed 64-byte little-endian records
    tow_ms  u4   GPS TOW of reception (ms)
    wnc     u2   GPS week
    prn     u1   Galileo PRN
    flags   u1
    page    u8[56]   448-bit HAS page (24-bit header at bit 0 + 53-byte body)
The 14 reserved C/NAV bits are already stripped: the HAS header starts at
bit 0 of the page (HASS u2 | Res u2 | MT u2 | MID u5 | MS u5 | PID u8,
MS stored minus 1, PID 1-based).

Sign / scaling conventions of the JSONL dump are RAW ICD values:
  dr,dit,dct  orbit corrections [m]   (radial/along/cross, ICD sign, scaled)
  dclk        clock correction  [m]   (= raw_int * 0.0025 * delta_clock_mult)
  cbias       code bias         [m]
  pbias       phase bias        [cycles]
cssrlib internally negates the HAS orbit correction (dorb *= -1.0 for
GAL_HAS_SIS); this script UNDOES that negation so dr/dit/dct are raw ICD.
The clock delta-clock-multiplier is an ICD-broadcast scaling, kept as-is.
"data not available" sentinels decode to NaN and are emitted as null.

Usage:
    python3 scripts/tests/has_cssrlib_decode.py \
        --has G5P3162a.has --gmat /tmp/has_gmat.csv \
        --out G5P3162a_has_cssrlib.jsonl
If --gmat does not exist it is generated per the HAS ICD (GF(256),
primitive polynomial 0x11D, g(x)=prod_{i=1..223}(x-alpha^i)).
"""

import argparse
import json
import math
import os
import struct
import sys
from collections import Counter, defaultdict

import numpy as np


def _import_cssrlib():
    """Import cssrlib, returning the symbols this script needs.

    Resolution order: (1) an already-installed/already-on-path cssrlib;
    (2) the directory in the CSSRLIB_SRC environment variable, if set.
    No developer-specific path is baked into the source. Deferred into a
    function so ``--help`` works without cssrlib present.
    """
    try:
        import cssrlib  # noqa: F401
    except ImportError:
        src = os.environ.get("CSSRLIB_SRC")
        if src and src not in sys.path:
            sys.path.insert(0, src)
        try:
            import cssrlib  # noqa: F401
        except ImportError:
            sys.exit(
                "error: cssrlib is required but not importable.\n"
                "  Install it (pip install cssrlib) or point CSSRLIB_SRC at a\n"
                "  cssrlib source checkout, e.g.:\n"
                "    CSSRLIB_SRC=/path/to/cssrlib/src python3 "
                "scripts/tests/has_cssrlib_decode.py ...\n"
                "  See https://github.com/hirokawa/cssrlib"
            )

    import bitstruct as bs
    import galois

    from cssrlib.cssr_has import cssr_has
    from cssrlib.cssrlib import sCType
    from cssrlib.gnss import sat2id, sat2prn, uGNSS

    return bs, galois, cssr_has, sCType, sat2id, sat2prn, uGNSS


# HAS RINEX-3 signal -> ICD signal-name mapping, per GNSS.
# Keyed by GNSS short name ("GPS"/"GAL") rather than the cssrlib uGNSS enum so
# this module-level table does not depend on cssrlib being importable (the
# import is deferred so --help works without it). The enum is resolved to a
# name in sig_label() at call time.
# Inner keys are the rSigRnx.str() short codes cssrlib produces (e.g. "L1B").
# Values are the Galileo HAS ICD signal names used in the dump.
SIG_NAME = {
    "GPS": {
        "1C": "L1-C/A",
        "1P": "L1-P",
        "1W": "L1-Z",
        "1L": "L1C(P)",
        "1S": "L1C(D)",
        "1X": "L1C(D+P)",
        "2S": "L2-CM",
        "2L": "L2-CL",
        "2X": "L2-CM+CL",
        "2P": "L2-P",
        "2W": "L2-Z",
        "5I": "L5-I",
        "5Q": "L5-Q",
        "5X": "L5-I+Q",
    },
    "GAL": {
        "1B": "E1-B",
        "1C": "E1-C",
        "1X": "E1-B+C",
        "5I": "E5a-I",
        "5Q": "E5a-Q",
        "5X": "E5a-I+Q",
        "7I": "E5b-I",
        "7Q": "E5b-Q",
        "7X": "E5b-I+Q",
        "8I": "E5-I",
        "8Q": "E5-Q",
        "8X": "E5-I+Q",
        "6B": "E6-B",
        "6C": "E6-C",
        "6X": "E6-B+C",
    },
}


def _jval(x):
    """NaN -> None, numpy scalar -> python scalar, for JSON."""
    if x is None:
        return None
    if isinstance(x, (np.floating, np.integer)):
        x = x.item()
    if isinstance(x, float) and math.isnan(x):
        return None
    return x


def sig_label(rsig, uGNSS):
    """rSigRnx -> ICD signal name (falls back to RINEX-3 code)."""
    code = rsig.str().strip()  # e.g. "L1B" -> band+attr "1B"
    key = code[1:]  # drop the type char (L/C)
    gname = {uGNSS.GPS: "GPS", uGNSS.GAL: "GAL"}.get(rsig.sys)
    tbl = SIG_NAME.get(gname, {})
    return tbl.get(key, code)


def build_gmat(path, galois):
    """Generate the 255x32 systematic RS(255,32) generator matrix per ICD."""
    GF = galois.GF(256, irreducible_poly=0x11D, primitive_element=2)
    alpha = GF(2)
    g = galois.Poly([1], field=GF)
    for i in range(1, 224):
        g = g * galois.Poly([GF(1), alpha**i], field=GF)
    gc = g.coeffs
    assert len(gc) == 224 and int(gc[0]) == 1

    # ICD Table 42 sanity check (low-first indexing, g_0 = constant term).
    g_low = gc[::-1]
    checks = {
        0: 88,
        1: 216,
        2: 195,
        3: 23,
        4: 111,
        32: 118,
        64: 81,
        96: 255,
        128: 39,
        160: 224,
        192: 241,
        223: 1,
    }
    for j, v in checks.items():
        assert int(g_low[j]) == v, f"g(x) mismatch at j={j}: {int(g_low[j])}!={v}"

    I32 = np.eye(32, dtype=int)
    P = np.zeros((223, 32), dtype=int)
    xshift = galois.Poly.Degrees([223], coeffs=[GF(1)], field=GF)
    for j in range(32):
        m = galois.Poly.Degrees([31 - j], coeffs=[GF(1)], field=GF)
        r = (m * xshift) % g
        rc = r.coeffs
        full = np.zeros(223, dtype=int)
        L = len(rc)
        for t in range(L):
            deg_t = L - 1 - t
            full[222 - deg_t] = int(rc[t])
        P[:, j] = full

    G = np.vstack([I32, P]).astype(np.uint8)
    np.savetxt(path, G, fmt="%d", delimiter=",")
    return G


def load_records(path):
    """Yield (tow_ms, wnc, prn, page_bytes) for each 64-byte record."""
    data = open(path, "rb").read()
    n = len(data) // 64
    out = []
    for r in range(n):
        rec = data[r * 64 : (r + 1) * 64]
        tow_ms, wnc, prn, _flags = struct.unpack_from("<IHBB", rec, 0)
        page = rec[8:64]  # 56 bytes (24-bit header + 53-byte body, then pad)
        out.append((tow_ms, wnc, prn, page))
    return out


def main():
    ap = argparse.ArgumentParser(description="Golden HAS MT1 decode via cssrlib.")
    ap.add_argument("--has", required=True, help="input .has record file (64-byte LE records)")
    ap.add_argument(
        "--gmat",
        default=None,
        help="RS(255,32) generator matrix CSV (default: <stem>_gmat.csv in CWD)",
    )
    ap.add_argument(
        "--out", default=None, help="output JSONL (default: <input-stem>_cssrlib.jsonl in CWD)"
    )
    args = ap.parse_args()

    stem = os.path.splitext(os.path.basename(args.has))[0]
    if args.out is None:
        args.out = f"{stem}_cssrlib.jsonl"
    if args.gmat is None:
        args.gmat = f"{stem}_gmat.csv"

    # Deferred so --help does not require cssrlib to be installed.
    bs, galois, cssr_has, sCType, sat2id, sat2prn, uGNSS = _import_cssrlib()

    if os.path.exists(args.gmat):
        gMat = np.genfromtxt(args.gmat, dtype="u1", delimiter=",")
    else:
        gMat = build_gmat(args.gmat, galois)

    GF = galois.GF(256, irreducible_poly=0x11D, primitive_element=2)

    records = load_records(args.has)
    # group by reception second, preserving order
    groups = defaultdict(list)
    order = []
    week = None
    for tow_ms, wnc, prn, page in records:
        if tow_ms not in groups:
            order.append(tow_ms)
        groups[tow_ms].append((prn, page))
        week = wnc

    cs = cssr_has()
    cs.monlevel = 0
    cs.week = week
    cs.msgtype = 1

    # Per-mid page accumulator (mirrors cnav_msg state but keyed by mid so
    # interleaved messages are collected independently).
    acc = {}  # mid -> {"ms":, "pages": {pid0: body}, "first_tow": }

    out_f = open(args.out, "w")
    header = {
        "format": "golden HAS MT1 decode via cssrlib",
        "values": "RAW ICD convention",
        "dr_dit_dct": "orbit radial/along/cross [m], ICD sign (cssrlib -1 undone), "
        "scale 0.0025/0.0080/0.0080",
        "dclk": "clock correction [m] = raw_int*0.0025*delta_clock_multiplier",
        "cbias": "code bias [m], scale 0.02",
        "pbias": "phase bias [cycles], scale 0.01",
        "di": "phase-bias discontinuity indicator (2-bit)",
        "iodref": "issue-of-data (IODE/IODnav) for the orbit set",
        "null": "data-not-available sentinel (NaN)",
        "signal_names": "Galileo HAS ICD signal names; see SIG_NAME in script",
        "week": week,
    }
    out_f.write("# " + json.dumps(header) + "\n")

    n_messages = 0
    ms_counter = Counter()
    mid_counter = Counter()
    flag_counter = Counter()
    sats_gps = set()
    sats_gal = set()
    sig_per_gnss = defaultdict(set)
    pbias_blocks = 0
    cbias_blocks = 0
    orbit_blocks = 0
    clock_blocks = 0
    mask_blocks = 0
    clksub_blocks = 0
    dr_vals, dit_vals, dct_vals, dclk_vals = [], [], [], []
    cb_vals, pb_vals = [], []
    k_max = 0
    stats = defaultdict(int)
    stats["decode_error_examples"] = []
    seen_msgs = set()

    def try_decode(mid):
        nonlocal n_messages, pbias_blocks, cbias_blocks, orbit_blocks
        nonlocal clock_blocks, mask_blocks, clksub_blocks, k_max
        ent = acc[mid]
        pids = sorted(ent["pages"].keys())
        if len(pids) < ent["ms"]:
            return
        idx = pids
        k = len(idx)
        k_max = max(k_max, k)
        has_pages = np.zeros((255, 53), dtype=int)
        for p in pids:
            has_pages[p, :] = ent["pages"][p]
        Wd = GF(has_pages[idx, :])
        try:
            Dinv = np.linalg.inv(GF(gMat[idx, :k]))
        except Exception:
            del acc[mid]
            return
        Md = Dinv @ Wd
        msg = np.array(Md).tobytes()

        # Each HAS message is re-broadcast continuously (same MID, climbing
        # PIDs) until its content changes, so the same bytes are recovered
        # many times per second. Emit each distinct message once. The MID
        # space (0..31) is reused for later messages, so dedup on the full
        # recovered byte content, not on MID.
        digest = hash(msg)
        if digest in seen_msgs:
            del acc[mid]
            return

        # peek header to record flag distribution before decode mutates state
        toh, flags, _res, mask_id, iodssr = bs.unpack_from("u12u6u4u5u5", msg, 0)
        if toh >= 3600:
            del acc[mid]
            return

        # Validity gate against RS decodes built from a mixed page set (two
        # message generations sharing one MID at a transition). A genuine HAS
        # MT1 message has a time-of-hour close behind the reception second:
        # latency = (rec%3600 - toh) mod 3600 is a small non-negative value.
        # Corrupt mixes show random toh (hundreds/thousands of seconds off).
        rec_tow_s = ent["first_tow"] // 1000
        latency = (rec_tow_s % 3600 - toh) % 3600
        if latency > 120:
            stats["rejected_toh"] += 1
            del acc[mid]
            return
        seen_msgs.add(digest)

        # A message whose blocks depend on a mask (orbit/clock/bias) but that
        # carries no mask block itself needs the mask state of the matching
        # iodssr to already be loaded. If iodssr does not match the current
        # mask, the sat list is stale and the variable-length blocks desync;
        # skip such a message rather than emit garbage.
        has_mask = bool((flags >> 5) & 1)
        if not has_mask:
            mask_loaded = (cs.lc[0].cstat & (1 << sCType.MASK)) != 0
            if not mask_loaded or cs.iodssr != iodssr:
                stats["skipped_no_mask"] += 1
                del acc[mid]
                return

        # set the hour base for this message's TOH
        cs.tow0 = (rec_tow_s // 3600) * 3600

        try:
            cs.decode_cssr(msg, 0)
        except Exception as e:
            stats["decode_error"] += 1
            stats["decode_error_examples"].append((mid, format(flags, "06b"), str(e)))
            del acc[mid]
            return

        flag_counter[flags] += 1
        if (flags >> 5) & 1:
            mask_blocks += 1
        if (flags >> 4) & 1:
            orbit_blocks += 1
        if (flags >> 3) & 1:
            clock_blocks += 1
        if (flags >> 2) & 1:
            clksub_blocks += 1
        if (flags >> 1) & 1:
            cbias_blocks += 1
        if (flags >> 0) & 1:
            pbias_blocks += 1
        ms_counter[ent["ms"]] += 1
        mid_counter[mid] += 1

        rec = build_record(toh, flags, mask_id, iodssr, rec_tow_s)
        if rec is not None:
            out_f.write(json.dumps(rec) + "\n")
            n_messages += 1
        del acc[mid]

    def build_record(toh, flags, mask_id, iodssr, rec_tow_s):
        lc = cs.lc[0]
        has_orbit = bool((flags >> 4) & 1)
        has_clock = bool((flags >> 3) & 1)
        has_cbias = bool((flags >> 1) & 1)
        has_pbias = bool((flags >> 0) & 1)

        # iterate the masked satellite list established by the mask block
        sats = {}
        for k_, sat in enumerate(cs.sat_n):
            sid = sat2id(sat)
            sys, _prn = sat2prn(sat)
            if sys == uGNSS.GPS:
                sats_gps.add(sid)
            elif sys == uGNSS.GAL:
                sats_gal.add(sid)
            d = {}
            if has_orbit and sat in lc.dorb:
                dorb = lc.dorb[sat]
                # cssrlib stored -ICD; undo the negation for raw ICD values
                dr = -dorb[0]
                dit = -dorb[1]
                dct = -dorb[2]
                d["iodref"] = _jval(lc.iode.get(sat))
                d["dr"] = _jval(dr)
                d["dit"] = _jval(dit)
                d["dct"] = _jval(dct)
                if not (isinstance(dr, float) and math.isnan(dr)):
                    dr_vals.append(dr)
                    dit_vals.append(dit)
                    dct_vals.append(dct)
            if has_clock and sat in lc.dclk:
                dclk = lc.dclk[sat]
                d["dclk"] = _jval(dclk)
                if not (isinstance(dclk, float) and math.isnan(dclk)):
                    dclk_vals.append(dclk)
            if has_cbias and sat in lc.cbias:
                cb = {}
                for rsig, val in lc.cbias[sat].items():
                    name = sig_label(rsig, uGNSS)
                    cb[name] = _jval(val)
                    sig_per_gnss[sat2id(sat)[0]].add(name)
                    if not (isinstance(val, float) and math.isnan(val)):
                        cb_vals.append(val)
                if cb:
                    d["cbias"] = cb
            if has_pbias and sat in lc.pbias:
                pb = {}
                di = {}
                for rsig, val in lc.pbias[sat].items():
                    name = sig_label(rsig, uGNSS)
                    pb[name] = _jval(val)
                    if sat in lc.di and rsig in lc.di[sat]:
                        di[name] = _jval(lc.di[sat][rsig])
                    if not (isinstance(val, float) and math.isnan(val)):
                        pb_vals.append(val)
                if pb:
                    d["pbias"] = pb
                    d["di"] = di
            if d:
                sats[sid] = d

        return {
            "tow_rec": rec_tow_s,
            "toh": toh,
            "mask_id": mask_id,
            "iod_set_id": iodssr,
            "flags_bits": format(flags, "06b"),
            "sats": sats,
        }

    for tow_ms in order:
        for prn, page in groups[tow_ms]:
            hass, _res, mt, mid, ms, pid = bs.unpack_from("u2u2u2u5u5u8", page, 0)
            if hass != 1 or mt != 1:
                continue
            ms += 1
            pid0 = pid - 1
            body = bs.unpack_from("u8" * 53, page, 24)
            if mid not in acc:
                acc[mid] = {"ms": ms, "pages": {}, "first_tow": tow_ms}
            acc[mid]["pages"][pid0] = body
            if len(acc[mid]["pages"]) >= acc[mid]["ms"]:
                try_decode(mid)

    out_f.close()

    # ---- report ----
    print(f"records read: {len(records)}")
    print(f"complete MT1 messages recovered: {n_messages}")
    print(f"max pages per decode (k): {k_max}")
    print(f"MS distribution: {dict(ms_counter)}")
    print(f"MID count (distinct): {len(mid_counter)}")
    print(f"  MID histogram (top): {mid_counter.most_common(12)}")
    print("flag-pattern distribution (u6, bit5=mask..bit0=pbias):")
    for f, c in sorted(flag_counter.items()):
        print(f"  {format(f, '06b')}: {c}")
    print(
        f"blocks: mask={mask_blocks} orbit={orbit_blocks} clock={clock_blocks} "
        f"clksub={clksub_blocks} cbias={cbias_blocks} pbias={pbias_blocks}"
    )
    print(f"GPS sats seen ({len(sats_gps)}): {sorted(sats_gps)}")
    print(f"GAL sats seen ({len(sats_gal)}): {sorted(sats_gal)}")
    for g_, sigs in sorted(sig_per_gnss.items()):
        print(f"  signals[{g_}]: {sorted(sigs)}")

    def rng(name, v):
        if v:
            print(
                f"  {name}: n={len(v)} min={min(v):.4f} max={max(v):.4f} mean={sum(v) / len(v):.4f}"
            )
        else:
            print(f"  {name}: (none)")

    print("value ranges (raw ICD):")
    rng("dr [m]", dr_vals)
    rng("dit [m]", dit_vals)
    rng("dct [m]", dct_vals)
    rng("dclk [m]", dclk_vals)
    rng("cbias [m]", cb_vals)
    rng("pbias [cyc]", pb_vals)
    print(
        f"skipped (no mask / iodssr mismatch): {stats['skipped_no_mask']}; "
        f"rejected (toh inconsistent w/ rec time): {stats['rejected_toh']}; "
        f"decode errors: {stats['decode_error']}"
    )
    for ex in stats["decode_error_examples"][:5]:
        print(f"  err mid={ex[0]} flags={ex[1]}: {ex[2]}")
    print(f"output: {args.out}")


if __name__ == "__main__":
    main()
