#!/usr/bin/env python3
"""Compare RTCM3 output: mosaic-CLAS (from SBF DiffCorrIn) vs MRTKLIB cssr2rtcm3.

Usage:
    python3 compare_rtcm3.py --sbf G5P3082b.sbf --mrtklib /tmp/mrtklib_out.rtcm3
"""

import argparse
import struct
import math
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from typing import Optional


# ---------------------------------------------------------------------------
# RTCM3 constants
# ---------------------------------------------------------------------------
SPEED_OF_LIGHT = 299792458.0

# GPS MSM signal table (DF395) - index 1-based
GPS_SIGNALS = {
    2: "1C",
    3: "1P",
    4: "1W",
    8: "2C",
    9: "2P",
    10: "2W",
    15: "2S",
    16: "2L",
    17: "2X",
    22: "5I",
    23: "5Q",
    24: "5X",
}

# Galileo MSM signal table (RTCM 3.3 table 3.5-99, 1-indexed)
GAL_SIGNALS = {
    2: "1C",
    3: "1A",
    4: "1B",
    5: "1X",
    6: "1Z",
    8: "6C",
    9: "6A",
    10: "6B",
    11: "6X",
    12: "6Z",
    14: "7I",
    15: "7Q",
    16: "7X",
    18: "8I",
    19: "8Q",
    20: "8X",
    22: "5I",
    23: "5Q",
    24: "5X",
}

# QZSS MSM signal table (RTCM 3.3 table 3.5-105, 1-indexed)
QZS_SIGNALS = {
    2: "1C",
    9: "6S",
    10: "6L",
    11: "6X",
    15: "2S",
    16: "2L",
    17: "2X",
    22: "5I",
    23: "5Q",
    24: "5X",
    30: "1S",
    31: "1L",
    32: "1X",
}

SYS_SIGNAL_TABLES = {
    "GPS": GPS_SIGNALS,
    "GAL": GAL_SIGNALS,
    "QZS": QZS_SIGNALS,
}

# Satellite system prefixes
SYS_PREFIX = {"GPS": "G", "GAL": "E", "QZS": "J"}

# Wavelengths (m) for common signals
GPS_L1_FREQ = 1575.42e6
GPS_L2_FREQ = 1227.60e6
GAL_E1_FREQ = 1575.42e6
GAL_E5A_FREQ = 1176.45e6
GAL_E5B_FREQ = 1207.140e6
GAL_E5_FREQ = 1191.795e6  # AltBOC (E5a+E5b)


# ---------------------------------------------------------------------------
# Bit reader
# ---------------------------------------------------------------------------
class BitReader:
    def __init__(self, data: bytes):
        self.data = data
        self.pos = 0  # bit position

    def read_uint(self, nbits: int) -> int:
        val = 0
        for _ in range(nbits):
            byte_idx = self.pos >> 3
            bit_idx = 7 - (self.pos & 7)
            if byte_idx < len(self.data):
                val = (val << 1) | ((self.data[byte_idx] >> bit_idx) & 1)
            else:
                val = val << 1
            self.pos += 1
        return val

    def read_int(self, nbits: int) -> int:
        val = self.read_uint(nbits)
        if val >= (1 << (nbits - 1)):
            val -= 1 << nbits
        return val

    def read_int64(self, nbits: int) -> int:
        val = self.read_uint(nbits)
        if val >= (1 << (nbits - 1)):
            val -= 1 << nbits
        return val


# ---------------------------------------------------------------------------
# Data structures
# ---------------------------------------------------------------------------
@dataclass
class StationRef:
    station_id: int = 0
    itrf_year: int = 0
    gps_ind: int = 0
    glo_ind: int = 0
    gal_ind: int = 0
    ref_sta_ind: int = 0
    x: float = 0.0
    y: float = 0.0
    z: float = 0.0
    ant_height: float = 0.0
    msg_type: int = 0  # 1005 or 1006


@dataclass
class MSM7Obs:
    sat_id: str = ""
    signal: str = ""
    pseudorange: float = 0.0  # meters
    phase: float = 0.0  # cycles
    phase_m: float = 0.0  # meters (for comparison)
    snr: float = 0.0  # dB-Hz
    doppler: float = 0.0  # Hz
    lock_time: float = 0.0  # seconds
    half_cycle: int = 0


@dataclass
class MSM7Epoch:
    tow: float = 0.0
    sys: str = ""
    msg_type: int = 0
    station_id: int = 0
    obs: list = field(default_factory=list)


# ---------------------------------------------------------------------------
# RTCM3 parsers
# ---------------------------------------------------------------------------
def parse_1005_1006(payload: bytes) -> Optional[StationRef]:
    br = BitReader(payload)
    s = StationRef()
    s.msg_type = br.read_uint(12)
    s.station_id = br.read_uint(12)
    br.read_uint(6)  # ITRF realization year
    s.itrf_year = 0
    s.gps_ind = br.read_uint(1)
    s.glo_ind = br.read_uint(1)
    s.gal_ind = br.read_uint(1)
    s.ref_sta_ind = br.read_uint(1)
    s.x = br.read_int64(38) * 0.0001  # meters
    br.read_uint(1)  # single receiver osc
    br.read_uint(1)  # reserved
    s.y = br.read_int64(38) * 0.0001
    br.read_uint(2)  # quarter cycle indicator
    s.z = br.read_int64(38) * 0.0001
    if s.msg_type == 1006:
        s.ant_height = br.read_uint(16) * 0.0001
    return s


def parse_msm7(payload: bytes, sys_name: str) -> Optional[MSM7Epoch]:
    """Parse MSM7 message (1077/1097/1117)."""
    br = BitReader(payload)
    ep = MSM7Epoch()
    ep.msg_type = br.read_uint(12)
    ep.station_id = br.read_uint(12)
    ep.sys = sys_name

    if sys_name == "GPS" or sys_name == "QZS":
        ep.tow = br.read_uint(30) / 1000.0  # GPS TOW ms -> s
    elif sys_name == "GAL":
        ep.tow = br.read_uint(30) / 1000.0

    br.read_uint(1)  # multiple message
    br.read_uint(3)  # IODS
    br.read_uint(7)  # reserved
    br.read_uint(2)  # clock steering
    br.read_uint(2)  # external clock
    br.read_uint(1)  # smoothing indicator
    br.read_uint(3)  # smoothing interval

    # Satellite mask (64 bits)
    sat_mask = br.read_uint(64)
    # Signal mask (32 bits)
    sig_mask = br.read_uint(32)

    sat_ids = []
    for i in range(64):
        if sat_mask & (1 << (63 - i)):
            sat_ids.append(i + 1)

    sig_ids = []
    for i in range(32):
        if sig_mask & (1 << (31 - i)):
            sig_ids.append(i + 1)

    nsat = len(sat_ids)
    nsig = len(sig_ids)

    if nsat == 0 or nsig == 0:
        return ep

    # Cell mask (nsat * nsig bits)
    cell_mask = []
    for _ in range(nsat):
        row = []
        for _ in range(nsig):
            row.append(br.read_uint(1))
        cell_mask.append(row)

    ncell = sum(sum(row) for row in cell_mask)

    # Satellite data (per satellite)
    sat_rough_ranges_ms = []
    for _ in range(nsat):
        val = br.read_uint(8)  # rough range integer ms
        sat_rough_ranges_ms.append(val)

    sat_ext_info = []
    for _ in range(nsat):
        sat_ext_info.append(br.read_uint(4))  # extended info

    sat_rough_ranges_mod = []
    for _ in range(nsat):
        val = br.read_uint(10)  # rough range mod 1ms
        sat_rough_ranges_mod.append(val)

    sat_rough_phase_rate = []
    for _ in range(nsat):
        val = br.read_int(14)  # rough phase range rate
        sat_rough_phase_rate.append(val)

    # Signal data (per cell)
    fine_pr = []
    fine_cp = []
    lock_time_ind = []
    half_cycle = []
    fine_snr = []
    fine_phrate = []
    for _ in range(ncell):
        fine_pr.append(br.read_int(20))  # fine pseudorange
    for _ in range(ncell):
        fine_cp.append(br.read_int64(24))  # fine phase range
    for _ in range(ncell):
        lock_time_ind.append(br.read_uint(10))  # lock time indicator
    for _ in range(ncell):
        half_cycle.append(br.read_uint(1))
    for _ in range(ncell):
        fine_snr.append(br.read_uint(10))  # fine CNR
    for _ in range(ncell):
        fine_phrate.append(br.read_int(15))  # fine phase range rate

    # Build observations
    sig_table = SYS_SIGNAL_TABLES.get(sys_name, {})
    prefix = SYS_PREFIX.get(sys_name, "?")
    cell_idx = 0
    for isat in range(nsat):
        for isig in range(nsig):
            if not cell_mask[isat][isig]:
                continue

            rr_ms = sat_rough_ranges_ms[isat]
            rr_mod = sat_rough_ranges_mod[isat]
            if rr_ms == 255:  # invalid
                cell_idx += 1
                continue

            rough_range_ms = rr_ms + rr_mod / 1024.0  # milliseconds
            rough_range_m = rough_range_ms * SPEED_OF_LIGHT / 1000.0

            pr_fine = fine_pr[cell_idx] * SPEED_OF_LIGHT / 1000.0 / (1 << 29)
            cp_fine = fine_cp[cell_idx] * SPEED_OF_LIGHT / 1000.0 / (1 << 31)
            snr = fine_snr[cell_idx] * 0.0625  # dB-Hz

            rough_rate = sat_rough_phase_rate[isat]  # m/s
            rate_fine = fine_phrate[cell_idx] * 0.0001  # m/s
            doppler_ms = rough_rate + rate_fine  # m/s

            obs = MSM7Obs()
            obs.sat_id = f"{prefix}{sat_ids[isat]:02d}"
            obs.signal = sig_table.get(sig_ids[isig], f"?{sig_ids[isig]}")
            obs.pseudorange = rough_range_m + pr_fine
            obs.phase_m = rough_range_m + cp_fine
            obs.snr = snr
            obs.doppler = doppler_ms  # store as m/s for now
            obs.lock_time = lock_time_ind[cell_idx]
            obs.half_cycle = half_cycle[cell_idx]
            ep.obs.append(obs)
            cell_idx += 1

    return ep


# ---------------------------------------------------------------------------
# SBF DiffCorrIn RTCM3 extractor
# ---------------------------------------------------------------------------
def extract_rtcm3_from_sbf(sbf_path: str) -> list:
    """Extract raw RTCM3 messages from SBF DiffCorrIn blocks."""
    with open(sbf_path, "rb") as f:
        data = f.read()

    messages = []
    pos = 0
    while pos < len(data) - 8:
        if data[pos] != 0x24 or data[pos + 1] != 0x40:
            pos += 1
            continue
        block_id = struct.unpack_from("<H", data, pos + 4)[0]
        block_len = struct.unpack_from("<H", data, pos + 6)[0]
        block_num = block_id & 0x1FFF

        if block_num == 5919 and block_len >= 16:
            payload = data[pos + 16 : pos + block_len]
            # Find RTCM3 preamble
            for i in range(min(20, len(payload) - 5)):
                if payload[i] == 0xD3:
                    msg_len = ((payload[i + 1] & 0x03) << 8) | payload[i + 2]
                    if msg_len >= 2 and i + 3 + msg_len + 3 <= len(payload):
                        rtcm3_payload = payload[i + 3 : i + 3 + msg_len]
                        msg_type = (rtcm3_payload[0] << 4) | (rtcm3_payload[1] >> 4)
                        messages.append((msg_type, rtcm3_payload))
                    break

        if block_len >= 8:
            pos += block_len
        else:
            pos += 1

    return messages


def extract_rtcm3_from_file(path: str) -> list:
    """Extract RTCM3 messages from a raw RTCM3 binary file."""
    with open(path, "rb") as f:
        data = f.read()

    messages = []
    pos = 0
    while pos < len(data) - 5:
        if data[pos] != 0xD3:
            pos += 1
            continue
        msg_len = ((data[pos + 1] & 0x03) << 8) | data[pos + 2]
        if msg_len < 2 or pos + 3 + msg_len + 3 > len(data):
            pos += 1
            continue
        # CRC check could go here; skip for now
        rtcm3_payload = data[pos + 3 : pos + 3 + msg_len]
        msg_type = (rtcm3_payload[0] << 4) | (rtcm3_payload[1] >> 4)
        messages.append((msg_type, rtcm3_payload))
        pos += 3 + msg_len + 3

    return messages


# ---------------------------------------------------------------------------
# Grouping and comparison
# ---------------------------------------------------------------------------
def group_by_epoch(messages: list) -> dict:
    """Group parsed MSM7 epochs by TOW."""
    epochs = defaultdict(dict)  # tow -> {sys -> MSM7Epoch}
    station_refs = []

    for msg_type, payload in messages:
        if msg_type in (1005, 1006):
            ref = parse_1005_1006(payload)
            if ref:
                station_refs.append(ref)
        elif msg_type == 1077:
            ep = parse_msm7(payload, "GPS")
            if ep and ep.obs:
                epochs[ep.tow]["GPS"] = ep
        elif msg_type == 1097:
            ep = parse_msm7(payload, "GAL")
            if ep and ep.obs:
                epochs[ep.tow]["GAL"] = ep
        elif msg_type == 1117:
            ep = parse_msm7(payload, "QZS")
            if ep and ep.obs:
                epochs[ep.tow]["QZS"] = ep

    return epochs, station_refs


def compare_station_ref(refs_a: list, refs_b: list, label_a: str, label_b: str):
    """Compare station reference positions."""
    print("=" * 70)
    print("1. Station Reference Position (1005/1006)")
    print("=" * 70)

    if not refs_a:
        print(f"  {label_a}: no 1005/1006 messages")
        return
    if not refs_b:
        print(f"  {label_b}: no 1005/1006 messages")
        return

    ra = refs_a[0]
    rb = refs_b[0]

    print(f"\n  {'Field':<20s} {label_a:>22s} {label_b:>22s} {'Diff':>12s}")
    print(f"  {'-' * 20} {'-' * 22} {'-' * 22} {'-' * 12}")
    print(f"  {'Msg type':<20s} {ra.msg_type:>22d} {rb.msg_type:>22d}")
    print(f"  {'Station ID':<20s} {ra.station_id:>22d} {rb.station_id:>22d}")
    print(f"  {'GPS indicator':<20s} {ra.gps_ind:>22d} {rb.gps_ind:>22d}")
    print(f"  {'GLO indicator':<20s} {ra.glo_ind:>22d} {rb.glo_ind:>22d}")
    print(f"  {'GAL indicator':<20s} {ra.gal_ind:>22d} {rb.gal_ind:>22d}")
    print(f"  {'Ref sta ind':<20s} {ra.ref_sta_ind:>22d} {rb.ref_sta_ind:>22d}")

    dx = ra.x - rb.x
    dy = ra.y - rb.y
    dz = ra.z - rb.z
    dist = math.sqrt(dx**2 + dy**2 + dz**2)

    print(f"  {'X (m)':<20s} {ra.x:>22.4f} {rb.x:>22.4f} {dx:>+12.4f}")
    print(f"  {'Y (m)':<20s} {ra.y:>22.4f} {rb.y:>22.4f} {dy:>+12.4f}")
    print(f"  {'Z (m)':<20s} {ra.z:>22.4f} {rb.z:>22.4f} {dz:>+12.4f}")
    print(f"  {'3D dist (m)':<20s} {'':>22s} {'':>22s} {dist:>12.4f}")

    if ra.msg_type == 1006:
        print(f"  {'Ant height (m)':<20s} {ra.ant_height:>22.4f}", end="")
        if rb.msg_type == 1006:
            print(f" {rb.ant_height:>22.4f}")
        else:
            print(f" {'N/A (1005)':>22s}")

    # Position stability check (first vs last)
    if len(refs_a) > 1:
        rl = refs_a[-1]
        ddist = math.sqrt((ra.x - rl.x) ** 2 + (ra.y - rl.y) ** 2 + (ra.z - rl.z) ** 2)
        print(f"\n  {label_a} position drift (first-last): {ddist:.4f} m ({len(refs_a)} msgs)")
    if len(refs_b) > 1:
        rl = refs_b[-1]
        ddist = math.sqrt((rb.x - rl.x) ** 2 + (rb.y - rl.y) ** 2 + (rb.z - rl.z) ** 2)
        print(f"  {label_b} position drift (first-last): {ddist:.4f} m ({len(refs_b)} msgs)")


def compare_message_stats(msgs_a: list, msgs_b: list, label_a: str, label_b: str):
    """Compare message type counts."""
    print("\n" + "=" * 70)
    print("2. Message Type Summary")
    print("=" * 70)

    ca = Counter(t for t, _ in msgs_a)
    cb = Counter(t for t, _ in msgs_b)
    all_types = sorted(set(ca.keys()) | set(cb.keys()))

    print(f"\n  {'Type':>6s} {label_a:>15s} {label_b:>15s}")
    print(f"  {'-' * 6} {'-' * 15} {'-' * 15}")
    for t in all_types:
        print(f"  {t:>6d} {ca.get(t, 0):>15d} {cb.get(t, 0):>15d}")


def compare_signal_types(epochs_a: dict, epochs_b: dict, label_a: str, label_b: str):
    """Compare signal types per system."""
    print("\n" + "=" * 70)
    print("3. Signal Types")
    print("=" * 70)

    for src_label, epochs in [(label_a, epochs_a), (label_b, epochs_b)]:
        sigs_by_sys = defaultdict(set)
        for tow, sys_eps in epochs.items():
            for sys_name, ep in sys_eps.items():
                for obs in ep.obs:
                    sigs_by_sys[sys_name].add(obs.signal)
        print(f"\n  {src_label}:")
        for sys_name in sorted(sigs_by_sys.keys()):
            sigs = sorted(sigs_by_sys[sys_name])
            print(f"    {sys_name}: {', '.join(sigs)}")


def compare_observations(epochs_a: dict, epochs_b: dict, label_a: str, label_b: str):
    """Compare observation values at common epochs/satellites."""
    print("\n" + "=" * 70)
    print("4. Observation Value Comparison (common epochs & satellites)")
    print("=" * 70)

    common_tows = sorted(set(epochs_a.keys()) & set(epochs_b.keys()))
    print(
        f"\n  Total epochs: {label_a}={len(epochs_a)}, {label_b}={len(epochs_b)}, common={len(common_tows)}"
    )

    if not common_tows:
        # Try to find overlap with tolerance
        tows_a = sorted(epochs_a.keys())
        tows_b = sorted(epochs_b.keys())
        if tows_a and tows_b:
            print(f"  {label_a} TOW range: {tows_a[0]:.1f} - {tows_a[-1]:.1f}")
            print(f"  {label_b} TOW range: {tows_b[0]:.1f} - {tows_b[-1]:.1f}")
            # Try rounding to nearest second
            tows_a_rounded = {round(t): t for t in tows_a}
            tows_b_rounded = {round(t): t for t in tows_b}
            common_rounded = sorted(set(tows_a_rounded.keys()) & set(tows_b_rounded.keys()))
            if common_rounded:
                print(f"  Common epochs (rounded to 1s): {len(common_rounded)}")
                common_tows = [(tows_a_rounded[t], tows_b_rounded[t]) for t in common_rounded]
            else:
                print("  No overlapping epochs found!")
                return
        else:
            return

    # Collect per-satellite-signal differences
    pr_diffs = defaultdict(list)  # (sat, sig_a, sig_b) -> [diff_m]
    cp_diffs = defaultdict(list)
    snr_a_vals = defaultdict(list)
    snr_b_vals = defaultdict(list)
    dop_diffs = defaultdict(list)

    if isinstance(common_tows[0], tuple):
        tow_pairs = common_tows
    else:
        tow_pairs = [(t, t) for t in common_tows]

    for tow_a, tow_b in tow_pairs:
        ea = epochs_a.get(tow_a, epochs_a.get(round(tow_a), {}))
        eb = epochs_b.get(tow_b, epochs_b.get(round(tow_b), {}))

        for sys_name in set(ea.keys()) & set(eb.keys()):
            # Group by (sat_id, signal) to handle multiple signals per sat
            obs_a = {}
            for o in ea[sys_name].obs:
                obs_a[(o.sat_id, o.signal)] = o
            obs_b = {}
            for o in eb[sys_name].obs:
                obs_b[(o.sat_id, o.signal)] = o

            for sat_sig in sorted(set(obs_a.keys()) & set(obs_b.keys())):
                oa = obs_a[sat_sig]
                ob = obs_b[sat_sig]
                key = (sat_sig[0], oa.signal, ob.signal)

                if oa.pseudorange > 0 and ob.pseudorange > 0:
                    pr_diffs[key].append(oa.pseudorange - ob.pseudorange)
                if oa.phase_m != 0 and ob.phase_m != 0:
                    cp_diffs[key].append(oa.phase_m - ob.phase_m)
                if oa.snr > 0:
                    snr_a_vals[key].append(oa.snr)
                if ob.snr > 0:
                    snr_b_vals[key].append(ob.snr)
                if oa.doppler != 0 and ob.doppler != 0:
                    dop_diffs[key].append(oa.doppler - ob.doppler)

    if not pr_diffs:
        print("  No common satellite observations found for comparison.")
        return

    # Print per-satellite summary
    print(
        f"\n  {'Sat':>5s} {'Sig_A':>6s} {'Sig_B':>6s} {'N':>5s} "
        f"{'PR_mean(m)':>12s} {'PR_std(m)':>10s} "
        f"{'CP_mean(m)':>12s} {'CP_std(m)':>10s} "
        f"{'SNR_A(dB)':>10s} {'SNR_B(dB)':>10s} "
        f"{'Dop_mean':>10s}"
    )
    print(
        f"  {'-' * 5} {'-' * 6} {'-' * 6} {'-' * 5} "
        f"{'-' * 12} {'-' * 10} "
        f"{'-' * 12} {'-' * 10} "
        f"{'-' * 10} {'-' * 10} "
        f"{'-' * 10}"
    )

    for key in sorted(pr_diffs.keys()):
        sat_id, sig_a, sig_b = key
        prd = pr_diffs[key]
        cpd = cp_diffs.get(key, [])
        sa = snr_a_vals.get(key, [])
        sb = snr_b_vals.get(key, [])
        dd = dop_diffs.get(key, [])

        pr_mean = sum(prd) / len(prd) if prd else 0
        pr_std = (sum((x - pr_mean) ** 2 for x in prd) / len(prd)) ** 0.5 if len(prd) > 1 else 0
        cp_mean = sum(cpd) / len(cpd) if cpd else 0
        cp_std = (sum((x - cp_mean) ** 2 for x in cpd) / len(cpd)) ** 0.5 if len(cpd) > 1 else 0
        snr_a_mean = sum(sa) / len(sa) if sa else 0
        snr_b_mean = sum(sb) / len(sb) if sb else 0
        dop_mean = sum(dd) / len(dd) if dd else 0

        print(
            f"  {sat_id:>5s} {sig_a:>6s} {sig_b:>6s} {len(prd):>5d} "
            f"{pr_mean:>+12.3f} {pr_std:>10.3f} "
            f"{cp_mean:>+12.3f} {cp_std:>10.3f} "
            f"{snr_a_mean:>10.2f} {snr_b_mean:>10.2f} "
            f"{dop_mean:>+10.3f}"
        )

    # Overall statistics
    all_pr = [v for diffs in pr_diffs.values() for v in diffs]
    all_cp = [v for diffs in cp_diffs.values() for v in diffs]
    if all_pr:
        mean_pr = sum(all_pr) / len(all_pr)
        std_pr = (sum((x - mean_pr) ** 2 for x in all_pr) / len(all_pr)) ** 0.5
        print(f"\n  Overall PR diff: mean={mean_pr:+.3f}m, std={std_pr:.3f}m, N={len(all_pr)}")
    if all_cp:
        mean_cp = sum(all_cp) / len(all_cp)
        std_cp = (sum((x - mean_cp) ** 2 for x in all_cp) / len(all_cp)) ** 0.5
        print(f"  Overall CP diff: mean={mean_cp:+.3f}m, std={std_cp:.3f}m, N={len(all_cp)}")


def compare_satellite_coverage(epochs_a: dict, epochs_b: dict, label_a: str, label_b: str):
    """Compare satellite coverage per system."""
    print("\n" + "=" * 70)
    print("5. Satellite Coverage")
    print("=" * 70)

    for src_label, epochs in [(label_a, epochs_a), (label_b, epochs_b)]:
        sats_by_sys = defaultdict(set)
        count_by_sys = defaultdict(list)
        for tow, sys_eps in epochs.items():
            for sys_name, ep in sys_eps.items():
                sats = set(o.sat_id for o in ep.obs)
                sats_by_sys[sys_name] |= sats
                count_by_sys[sys_name].append(len(sats))

        print(f"\n  {src_label}:")
        for sys_name in sorted(sats_by_sys.keys()):
            sats = sorted(sats_by_sys[sys_name])
            counts = count_by_sys[sys_name]
            avg_count = sum(counts) / len(counts) if counts else 0
            print(f"    {sys_name}: {', '.join(sats)} (avg {avg_count:.1f}/epoch)")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(
        description="Compare RTCM3: mosaic-CLAS (SBF DiffCorrIn) vs MRTKLIB cssr2rtcm3"
    )
    parser.add_argument("--sbf", required=True, help="SBF file with DiffCorrIn (mosaic-CLAS RTCM3)")
    parser.add_argument("--mrtklib", required=True, help="MRTKLIB cssr2rtcm3 output (raw RTCM3)")
    parser.add_argument("--label-a", default="mosaic-CLAS", help="Label for SBF source")
    parser.add_argument("--label-b", default="MRTKLIB", help="Label for MRTKLIB source")
    args = parser.parse_args()

    print(f"Loading {args.label_a} RTCM3 from SBF: {args.sbf}")
    msgs_a = extract_rtcm3_from_sbf(args.sbf)
    print(f"  -> {len(msgs_a)} messages extracted")

    print(f"Loading {args.label_b} RTCM3: {args.mrtklib}")
    msgs_b = extract_rtcm3_from_file(args.mrtklib)
    print(f"  -> {len(msgs_b)} messages extracted")

    epochs_a, refs_a = group_by_epoch(msgs_a)
    epochs_b, refs_b = group_by_epoch(msgs_b)

    compare_station_ref(refs_a, refs_b, args.label_a, args.label_b)
    compare_message_stats(msgs_a, msgs_b, args.label_a, args.label_b)
    compare_signal_types(epochs_a, epochs_b, args.label_a, args.label_b)
    compare_satellite_coverage(epochs_a, epochs_b, args.label_a, args.label_b)
    compare_observations(epochs_a, epochs_b, args.label_a, args.label_b)

    print("\n" + "=" * 70)
    print("Done.")


if __name__ == "__main__":
    main()
