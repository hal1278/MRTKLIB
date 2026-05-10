# Release Notes — v0.6.5

## First official release of `mrtk cssr2rtcm3` — real-time CSSR→RTCM3 converter

**Release date:** 2026-05-10
**Type:** Feature — first official release of a new subcommand and a new hardware integration path
**Branch:** `release/v0.6.5`

---

### Overview

v0.6.5 marks the first official release of the **`mrtk cssr2rtcm3`**
subcommand, previously available only on the `feat/integrate-cssr2rtcm3`
development branch. It converts QZSS CLAS L6D corrections into standard
RTCM3 MSM7 messages on the fly, so any RTCM3-capable receiver (including
those that cannot decode CLAS L6D natively) can take advantage of CLAS as
a Virtual Reference Station (VRS) source.

The release also ships a Septentrio mosaic-G5 P3 hardware integration
guide, a 24-hour static endurance test, and a smaller utility,
**`mrtk l6extract`**, for pulling L6D / L6E frames out of SBF and UBX logs.

The 62-test CTest suite passes unchanged; this release does not modify
any existing positioning-engine numerics.

---

### Major additions

#### `mrtk cssr2rtcm3` — CSSR-to-RTCM3 converter (public release)

Real-time converter from QZSS CLAS L6D (Compact SSR) to RTCM3 MSM7,
designed to be inserted between a CLAS-capable receiver (e.g. Septentrio
mosaic-G5 P3) and an arbitrary RTK rover. Functional highlights:

- **MSM7** output for GPS / Galileo / QZSS (configurable via TOML).
- **`1005` / `1006`** RTCM3 base-station messages with auto-detected GNSS
  indicators and configurable mosaic-CLAS-compatible station ID.
- **Septentrio SBF single-stream input** (multiplexed L6D + decoded NAV +
  PVT) plus UBX and BINEX paths.
- **Elevation-based L6D PRN auto-selection** with configurable threshold
  (`l6d_elmin`, default `10°`), 30-second timeout, and 5° hysteresis;
  `l6d_prn_fixed` available to lock the source PRN.
- **Configurable signal remap** (e.g. `G2X→2W`, `E1X→1C`, `E5X→5Q`,
  `J2X→2L`) for receivers that prefer specific signal codes.
- **Same-frequency fallback** in `clas_osr_corrmeas()` so CLAS bias
  matching does not strand satellites that broadcast a different code on
  the same frequency.
- **PVT-time-based RTCM3 pacing** (1 s grid) for stable downstream RTK
  consumption.
- **Selectable SNR model** (fixed 50 dBHz or elevation-dependent).
- **Default config** in [`conf/cssr2rtcm3.toml`](../../conf/cssr2rtcm3.toml).

#### `mrtk l6extract` — L6D / L6E frame extractor

Small companion utility that reads an SBF or UBX log and writes per-PRN
L6D / L6E frame files. Useful for recovering raw CLAS / MADOCA streams
from receiver-side recordings.

#### Hardware integration: Septentrio mosaic-G5 P3

New documentation page,
[`docs/hardware/cssr2rtcm3-mosaic-g5.md`](../hardware/cssr2rtcm3-mosaic-g5.md),
walks through the full end-to-end setup of `cssr2rtcm3` with a mosaic-G5
P3 evaluation kit:

- RxTools configuration (USB ports, SBF Output `Support` block set,
  QZSS L6 tracking, RTCM3 input port, Solution Sensitivity).
- Wiring of `mrtk relay` + `mrtk cssr2rtcm3` for VRS-RTK operation.
- Alternative path: native MRTKLIB engine via `mrtk run`.
- Real-world setup gotchas captured from field testing
  (macOS `cu.*` vs `tty.*`, explicit `RTCMv3` input type required).

#### 24-hour endurance test

The hardware guide ships with the first published 24-hour static
endurance result of the `cssr2rtcm3 → mosaic-G5` chain. Headline numbers
(2026-05-08 → 2026-05-09 GPST, 87,839 epochs):

| Mode  | Share    |
| ----- | -------: |
| Fix   | 72.05 %  |
| Float | 27.94 %  |
| DGPS  | 0.00 %   |
| SPP   | 0.02 %   |

A reference 24-hour run of a mosaic-CLAS receiver at a separate Kantō
site on the same day reached 97.96 % Fix, indicating roughly 20 % of
remaining headroom — see Known Issues below.

---

### Robustness fixes that landed during the v0.6.5 development cycle

These fixes shipped progressively on `feat/integrate-cssr2rtcm3` and are
all bundled into this release.

- **Long-running daemon stability** — `actualdist()` now caps satellite
  enumeration at `MAXOBS` and skips non-CLAS systems
  (`d028b0c`, `7fb497f`), eliminating the `MAXSAT` overflow / GLONASS
  RK4 hangs that would silently stall the converter on multi-day runs.
- **No more 10-minute DGPS dips** — broadcast-ephemeris IODE rollover
  used to leave CLAS SSR pointing at a vanished IODE for ~55 s, causing
  RTK to fall back to DGPS. A new `nav_t.eph_prev` slot keeps the prior
  IODE alive long enough to bridge the gap (`3a8cc51`).
- **L5 / E5a corrections** — the OSR engine now applies CLAS L5/E5a
  corrections instead of leaving them invalid (`361b6df`).
- **Galileo / QZSS in MSM** — Galileo and QZSS satellites are now
  emitted in the RTCM3 stream (`21f7b1f`, `c2ba3b6`, `f5f2748`,
  `985fc34`).
- **VRS base position latched on first SPP fix** (`4fa4d65`) — eliminates
  base-coordinate drift that was the dominant cause of Float-only RTK on
  the converter side.
- **`osr[]` per-epoch clearing** in `clas_ssr2osr()` (`0145c30`) —
  prevents previous-epoch P / Φ values from being carried forward when
  no new correction arrived for a satellite.
- **δBIAS discontinuity compensation** (IS-QZSS-L6-005 §5.5.3.2,
  `2b7580e`).
- **Ephemeris lookup via `seleph()`** with a 30 s rebroadcast cycle
  (`d64893b`, `c2ba3b6`).
- **`-prn` deprecated in favour of `l6d_elmin` auto-selection**
  (`2c6e251`, `64617b5`, `ef369a3`, `16db68f`).
- **Same-frequency code fallback** so CLAS bias matching does not
  strand satellites that broadcast a different code on the same
  frequency (`d8d2f4b`).
- **VRS PP improvements** — base position init enabled for VRS-RTK in
  post-processing, and an `selfreqpair` fallback in `zdres_sat` for E5a
  observations (`b034d48`, `ea296db`). Post-processing VRS now reaches
  98.5 % Fix on the regression dataset.
- **PVT-based RTCM3 pacing** at the 1 s grid (`e729983`, `95dn58ca`,
  `03a798d`) — RTCM3 delivery rate up to ~99.5 %.
- **mosaic-G5 RTCMv3 input gotcha** documented after a 24h SPP-stuck
  diagnosis: `auto` does not reliably detect RTCMv3 on firmware
  `20250611b`; the receive-side port must be set to `RTCMv3` explicitly
  (`0ea0147`).

### Other changes worth highlighting

- **`l6extract` subcommand** for offline L6D / L6E frame extraction.
- **`module:cssr2rtcm3` GitHub label** added now that the tool is
  publicly released (PR #99).
- **Plotting utilities** under `scripts/plotting/`:
  - `parse_pvt.py` — NMEA/SBF parser with rigorous ECEF→ENU.
  - `plot_pos.py` — ENU comparison plotter with CSV save mode.
  - `sbf_plot.py` — real-time SBF position monitor.
- **Positioning-engines reference** (`docs/guide/positioning-engines.md`):
  RTK vs VRS-RTK vs PPP-RTK overview.

---

### Known limitations

#### Vertical-component dispersion (~30 s sawtooth) — [#97](https://github.com/h-shiono/MRTKLIB/issues/97)

The current `cssr2rtcm3` SSR extrapolation holds rate terms at zero, so
each ~30 s SSR snapshot introduces a small step in the corrected
satellite position/clock that propagates into the vertical component as
a few-cm peak-to-peak sawtooth. Horizontal accuracy is unaffected.
Estimating SSR rate by numerical differentiation between consecutive
snapshots is being investigated.

#### Fix-rate gap vs. mosaic-CLAS reference (~20 %) — [#98](https://github.com/h-shiono/MRTKLIB/issues/98)

In the 24-hour endurance test summarised above, the
`cssr2rtcm3 → mosaic-G5` chain achieved 72 % Fix, versus 98 % on a
mosaic-CLAS reference at a different Kantō site on the same day. The
gap is concentrated in two windows (h ≈ 04–05 and h ≈ 14–18 GPST), in
which the tracked-satellite count and the differential-correction age
are both healthy — so the cause is neither observation starvation nor a
correction-link outage. Investigation continues; candidates include
PRN-specific cycle-slip cascades, ionospheric activity, and
CLAS-network-region transitions ([#66](https://github.com/h-shiono/MRTKLIB/issues/66)).

---

### Files Changed

| File | Change |
|------|--------|
| `apps/cssr2rtcm3/cssr2rtcm3.c` | New — public CSSR→RTCM3 converter |
| `apps/l6extract/l6extract.c` | New — L6D/L6E frame extractor |
| `apps/mrtk/main.c` | New `cssr2rtcm3` and `l6extract` subcommands |
| `src/clas/mrtk_clas_osr.c` | δBIAS compensation, `osr[]` clearing, code fallback, L5/E5a corrections |
| `src/clas/mrtk_clas.c` | `nav_t.eph_prev` slot, `seleph()` IODE-prev fallback |
| `src/rtcm/...` | MSM7 output for GAL/QZS, `1006` station message, station-ID config |
| `conf/cssr2rtcm3.toml` | New — default cssr2rtcm3 config (with `l6d_elmin`, `l6d_prn_fixed`, signal remap, MSM systems) |
| `docs/hardware/cssr2rtcm3-mosaic-g5.md` | New — mosaic-G5 P3 setup, two-approach (VRS / engine), 24h test result, mosaic-CLAS comparison, known issues |
| `docs/hardware/images/mosaic-g5/*.png` | New — RxTools screenshots, 24h ENU plot, mosaic-CLAS reference plot |
| `docs/guide/positioning-engines.md` | New — RTK / VRS-RTK / PPP-RTK reference |
| `scripts/plotting/{parse_pvt,plot_pos,sbf_plot}.py` | New — analysis & live plot helpers |
| `CMakeLists.txt` | Version 0.6.4 → 0.6.5 |
| `CHANGELOG.md` | v0.6.5 entry |
| `mkdocs.yml` | v0.6.5 in Releases navigation |
| `README.md` | v0.6.5 roadmap entry |

---

### Upgrade notes

- **No positioning-engine numerics changed.** Existing `mrtk run`,
  `mrtk post`, `rnx2rtkp` invocations behave bit-identically. The 62-test
  CTest suite passes unchanged.
- **No TOML-schema break for existing modes.** The new `[cssr2rtcm3]`
  options (`l6d_elmin`, `l6d_prn_fixed`, signal remap, MSM systems) are
  scoped to the new subcommand.
- The legacy `-prn` flag on `cssr2rtcm3` is **deprecated** but still
  accepted; new usage should rely on `l6d_elmin` / `l6d_prn_fixed`.

---

### Test Results

62/62 tests pass (no regressions).
