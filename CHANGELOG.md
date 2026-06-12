# Changelog

All notable changes to MRTKLIB are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [v0.7.0] - 2026-06-12

**Galileo HAS (High Accuracy Service) float PPP.** Opens the v0.7.x series
themed around global SSR correction services. MRTKLIB now decodes Galileo HAS
corrections broadcast on the E6-B C/NAV signal and applies them in float PPP for
GPS+Galileo, in both post-processing (`mrtk post`) and real-time (`mrtk run`).
HAS is a free, global high-accuracy SSR service (orbit / clock / code bias) in
the GTRF/ITRF2020 frame; the HAS Initial Service broadcasts no phase biases or
ionosphere/troposphere corrections, so this release targets float PPP. The
decoder was cross-validated bit-exact against the cssrlib reference
implementation. (BDS PPP-B2b is planned next in the series.)

### Added

- **Galileo HAS E6-B C/NAV decoder** (`src/has/`,
  [PR #214](https://github.com/h-shiono/MRTKLIB/pull/214)) — a new decoder
  collects HAS pages (keyed by Message ID / page ID), runs the
  **RS(255,32,224)** erasure decoder over GF(256) to recover each HAS message,
  and decodes Mask + Orbit + Clock + Code-bias + Phase-bias (MT1) blocks into
  `ssr_t`, feeding the existing `satpos_ssr()` / `corr_meas()` SSR application
  path. The RS decoder builds its systematic generator matrix at init (no
  embedded tables) and was validated against ICD Annex C; the MT1 decoder was
  cross-validated **bit-exact against cssrlib** over 428 MT1 messages on a live
  mosaic-G5 capture.
- **`correction = "gal-has"`** for PPP modes — wires HAS corrections through the
  SSR branch (orbit, clock, code bias) with a HAS-specific bias-code selector
  (same-frequency fallback). Supported in post-processing and real-time, with
  `satellite_ephemeris = "brdc+ssrapc"`. Sample configs `conf/has/ppp_gal_has.toml`
  (post) and `conf/has/run_gal_has.toml` (real-time).
- **`mrtk l6extract` HAS extraction** — extracts Galileo HAS pages from SBF
  **GALRawCNAV** (block 4024) into a `.has` file consumed by `mrtk post`. The
  same release also adds mosaic-G5 QZSS **L6E** extraction from SBF block
  **4271** (byte-identical to L6D block 4270 apart from the Source field),
  written per-PRN +10 above the L6D PRN.
- **Bundled HAS regression tests** — `tests/data/has/has_testdata.tar.gz`
  (~2.1 MB) ships a block-filtered 15-minute mosaic-G5 slice
  (`G5P3162a_15m.sbf`) plus a `has_*` ctest pipeline (extract → convert → float
  PPP → accuracy gate). A trim-equivalence proof shows the trimmed slice is
  byte-identical to the corresponding full-hour window (`.has` records and OBS
  records identical; PPP solutions bit-identical over all common epochs).

### Known limitations

- **Float PPP only** — the HAS Initial Service broadcasts no phase biases, so
  integer PPP-AR is not yet possible; it is planned for when HAS reaches Full
  Operational Capability (FOC) with phase biases.
- **No ionosphere / troposphere corrections** — HAS carries none; troposphere is
  estimated (`est-ztd`) and the iono-free combination is used, as in MADOCA-PPP.
- **Absolute accuracy under investigation** — over 1 h of live data the HAS float
  solution sits ~0.5–0.6 m horizontal from a cm-grade ITRF reference, larger than
  mature PPP would give; the accuracy investigation is open
  ([#215](https://github.com/h-shiono/MRTKLIB/issues/215)). Day-to-day
  repeatability is good (RMS E 7.5 / N 7.3 / U 38 cm).
- **GPS + Galileo only** — the constellations HAS corrects.

### References

- Galileo HAS SIS ICD, Issue 1.0 (May 2022).
- cssrlib (MIT License, © 2021 Rui Hirokawa,
  <https://github.com/hirokawa/cssrlib>), used as an independent development-time
  cross-reference only — not bundled with or linked into MRTKLIB.

## [v0.6.14] - 2026-06-10

**Real-time CLAS handover robustness + cssr2rtcm3 / VRS carrier-phase quality.**
Two real-time reliability fixes. (1) Real-time CLAS PPP-RTK (`mrtk run`) no
longer drops permanently to Single after a QZSS L6 satellite handover — the
single-stream L6D demux now keeps each channel locked to one coherent PRN and
re-locks across a handover for every rover format (the re-lock clock previously
stalled for RTCM2/RTCM3 rovers). (2) The `mrtk cssr2rtcm3` VRS/OSR carrier phase
no longer leaks phase-bias-wrap jumps or stale-slot carrier glitches, removing
the cycle slips behind the downstream RTK fix-rate gap and vertical sawtooth.

### Fixed

- **Real-time CLAS no longer freezes on a QZSS satellite handover**
  ([#197](https://github.com/h-shiono/MRTKLIB/issues/197),
  [#205](https://github.com/h-shiono/MRTKLIB/issues/205);
  PRs [#200](https://github.com/h-shiono/MRTKLIB/pull/200),
  [#201](https://github.com/h-shiono/MRTKLIB/pull/201),
  [#209](https://github.com/h-shiono/MRTKLIB/pull/209)) — in real-time PPP-RTK
  the UBX/SBF single-stream L6D demux locked channel 0 to the first PRN it saw
  and never released it, so when that QZS set below the horizon corrections
  froze and the solution dropped to Single (`age=1e4`) permanently until a
  restart. The demux now (a) keeps each CLAS channel locked to a single
  coherent active-source PRN (no multi-PRN subframe-interleave corruption on a
  D9C-class 2-channel receiver) and (b) re-locks to a live PRN after the locked
  one goes silent (handover). The re-lock timeout clock now advances for
  **RTCM2/RTCM3 rovers** too (it was driven by `raw[0].time`, which a
  decoded-RTCM rover never updates, so the timeout never fired — the root cause
  of the "drops to Single and never recovers" reports). Verified by replaying a
  captured stream that froze on a real handover: the channel now re-locks within
  ~14 s and stays Fix instead of freezing.
- **cssr2rtcm3 / VRS phase-bias-wrap repair applied to the output carrier**
  ([#97](https://github.com/h-shiono/MRTKLIB/issues/97),
  [#98](https://github.com/h-shiono/MRTKLIB/issues/98);
  PR [#207](https://github.com/h-shiono/MRTKLIB/pull/207)) — the ±100-cycle
  CLAS phase-bias-wrap repair (`clas_osr_zdres()`) was computed but never applied
  to the synthesized VRS/OSR carrier phase (the port kept only the PPP-RTK
  ambiguity-state half of upstream's two output variants). On a CLAS phase-bias
  (ST5) wrap, ~100 cycles (~19 m at L1) leaked into the base carrier, forcing a
  cycle slip on the downstream RTK rover. Now applied to `obs[].L` in the VRS
  fill — prime suspect for the cssr2rtcm3 fix-rate gap vs mosaic-CLAS (#98) and
  vertical sawtooth (#97).
- **cssr2rtcm3 / VRS signal-slot desync and dead-satellite emission**
  ([#98](https://github.com/h-shiono/MRTKLIB/issues/98);
  PR [#208](https://github.com/h-shiono/MRTKLIB/pull/208)) — the reused VRS dummy
  obs buffer could carry a stale per-signal code from a previous epoch,
  desyncing the OSR signal-slot order from `smode` and leaking a stale carrier
  value (observed as single-epoch ~1153 m carrier-phase glitches on Galileo E5a
  when the receiver-tracked signal set differed from the CLAS-corrected set).
  VRS obs codes are now set unconditionally from `smode`, unused slots cleared,
  and satellites with no usable observation (invalid MSM rough range 255) are no
  longer advertised. CLAS RTCM3 output is byte-identical for `nf=2`.
- **`mrtk relay -t` now writes a trace file**
  ([#79](https://github.com/h-shiono/MRTKLIB/issues/79);
  PR [#204](https://github.com/h-shiono/MRTKLIB/pull/204)) — `relay` did not
  create the runtime context, so `trace*()` no-opped and `-t <level>` produced
  no file. The context is now created when a trace level is requested and torn
  down on both exit paths (including the `strsvrstart` failure path, which also
  now frees its stream converters).

### Added

- **cssr2rtcm3 OSR carrier-phase self-consistency analyzer**
  ([#206](https://github.com/h-shiono/MRTKLIB/pull/206)) —
  `scripts/analysis/osr_residual.py`, a no-ephemeris diagnostic that decodes the
  RTCM3 (MSM7 + 1005) emitted by `mrtk cssr2rtcm3` and, per satellite, tracks
  geometry-free phase, code-minus-carrier and lock-time to surface carrier
  discontinuities (slips, bias-wrap, glitches) and satellites advertised with an
  invalid rough range. Used to root-cause the #97/#98 OSR-quality defects above.

### Changed

- **Documentation**: single-port relay-back VRS setup guide for cssr2rtcm3
  ([#117](https://github.com/h-shiono/MRTKLIB/issues/117),
  PR [#202](https://github.com/h-shiono/MRTKLIB/pull/202)); CI retries transient
  `taplo` download failures ([#203](https://github.com/h-shiono/MRTKLIB/pull/203)).

## [v0.6.13] - 2026-06-07

**Enhanced PPP-RTK SPP seed; RINEX converter frequency unification.** The CLAS
PPP-RTK / VRS-RTK per-epoch single-point seed now uses the v0.6.10 SPP error
model (C/N0 weighting + TDCP jump-reject) by default, improving kinematic fix
rate and the fixed-epoch error tail while leaving static positioning unchanged.
Separately, `mrtk convert` (RINEX converter) unifies its frequency indexing on
the obsdef tables, reordering some observation-type columns (no positioning
impact).

### Added

- **Enhanced a-priori SPP seed for PPP-RTK/VRS**
  ([#196](https://github.com/h-shiono/MRTKLIB/pull/196)) — the single-point fix
  that seeds (and, on a filter reset, re-seeds) the CLAS/VRS precise filter can
  apply the proven v0.6.10 SPP error model. New `[positioning.clas]
  enhanced_spp_seed` profile (`off` / `cn0+tdcp` / `cn0+tdcp+robust`), applied to
  a **private** SPP option copy so the CLAS measurement model is untouched
  (`err[5]/err[6]` mean C/N0 in SPP but iono/trop in the CLAS engine). On the PPC
  urban benchmark: mean fix rate +1.0 pp, fixed-epoch p95 3.23 m → 3.02 m. IGG-III
  robust is an explicit open-sky opt-in (it can trip fix-and-hold mis-fixes in
  deep urban canyons). Applies to real-time (`mrtk run`) too.

### Changed

- **Enhanced SPP seed on by default** (`cn0+tdcp`)
  ([#196](https://github.com/h-shiono/MRTKLIB/pull/196)) — `prcopt_default` now
  enables the C/N0 + TDCP seed for PPP-RTK/VRS. **Positioning change** for those
  modes (kinematic improves; the static regression suite is byte-close and within
  tolerances). Set `enhanced_spp_seed = "off"` to restore prior behaviour. Plain
  PPP / PPP-AR are unaffected (the seed coupling there was investigated and found
  inert).
- Removed 31 deprecated commented `# constellations` / `# frequency` keys from the
  shipped configs (superseded by the active `systems` / `signals` keys); no value
  change.
- **RINEX converter unified on obsdef frequency indexing**
  ([#71](https://github.com/h-shiono/MRTKLIB/issues/71)) — `mrtk convert`
  (convrnx) now derives observation-type frequency indices from
  `code2freq_idx()`, the obsdef-based mapping already used by every receiver
  decoder, the RINEX parser, RTCM3 and CLAS. The legacy fixed per-band
  `code2idx()` / `band2idx_fixed()` are removed. **Output change (converter
  only, no positioning impact):** RINEX obs-type columns now follow obsdef band
  order, so Galileo lists **E5a before E5b** and QZSS lists **L5 before L2**
  (BeiDou columns reorder similarly). The `-f` / `--freq N` band count keeps
  its meaning but now selects the same N bands the positioning engines use.
  Positioning output is byte-identical (the obsdef tables are untouched).

### Known limitations

`mrtk convert` does not emit two rare signals that have no slot in the obsdef
frequency tables. Adding slots for them would perturb GLONASS / BeiDou
positioning, so the tables are intentionally left as-is
([#71](https://github.com/h-shiono/MRTKLIB/issues/71)):

- **GLONASS G3 (CDMA, GLONASS-K2 only)** — no `obsdef_GLO` slot.
- **BeiDou B2a+b (AltBOC)** — maps to the 6th obsdef slot, beyond the
  converter's 5-band `--freq` mask.

### Tests

- `utest_t_freqidx` — pins the `code2freq_idx()` (sys, code) → frequency-index
  contract for GPS/GLONASS/Galileo/QZSS/BeiDou, including the GLONASS G3
  regression guard.

## [v0.6.12] - 2026-06-03

**Real-time MADOCA-PPP multi-GNSS signal selection.** Brings the real-time
(`mrtk run`) path up to parity with post-processing for non-GPS constellations:
Galileo and QZSS are now used, GLONASS L2C/A can be selected, and the obsdef
signal tables survive in-process restarts. **This is a positioning change** for
real-time MADOCA-PPP (more constellations enter the solution); post-processing
outputs are unchanged.

### Added

- **Non-GPS signals in real-time MADOCA-PPP** ([#184](https://github.com/h-shiono/MRTKLIB/issues/184),
  PR #185) — `rtkrcv` now applies the configured PPP signal selection
  (`apply_pppsig()`) at server start, mirroring the post-processing path. Without
  it the obsdef tables kept their defaults and the engine could only form the
  iono-free pair for GPS, silently dropping Galileo/QZSS/BeiDou. IGS-product PPP
  still skips it (#135) so the receiver's actual 2nd band survives.
- **`[positioning].signals` drives raw-decoder code selection**
  ([#187](https://github.com/h-shiono/MRTKLIB/issues/187), PR #190) — the
  signal list is now authoritative for which observation code occupies each
  band's slot during decoding, so e.g. **GLONASS L2C/A** can be used as the
  iono-free 2nd frequency on an L2C/A-only receiver (Septentrio mosaic-CLAS)
  without the legacy `-RL2C` option. New `mrtk_sigcfg_freq_idx()` helper;
  `raw_t` carries the resolved `sigcfg`. **Phase 1 covers the Septentrio (SBF)
  decoder**; other decoders, the RTCM3 obs path and `convbin` follow in
  [#189](https://github.com/h-shiono/MRTKLIB/issues/189). The `-R/-G` receiver
  options are retained as the fallback when `signals` is unset.
- **Unit tests** — `utest_t_obsdef` (obsdef idempotency/reset) and
  `utest_t_sigcfg_decode` (sigcfg-driven per-code slotting), both env-independent.

### Changed

- **obsdef signal selection is idempotent across `rtkrcv` restarts**
  ([#186](https://github.com/h-shiono/MRTKLIB/issues/186), PR #188) —
  `set_obsdef()` now rebuilds from a pristine default snapshot instead of mutating
  in place, and a new `reset_obsdef()` is applied before re-selecting signals at
  server start. A `load`+`restart` that changes the correction source or signal
  selection (including switching to `correction = "igs"`, which skips
  `apply_pppsig()`) can no longer inherit a trimmed obsdef. Bit-identical on the
  first/single configuration.
- **Docs** — `[positioning].signals` is documented as the recommended,
  authoritative signal-selection surface (overrides `frequency` / the `[signals]`
  presets, derives the frequency count, and drives decoder code selection). The
  `[signals]` preset section is marked **legacy** (coarse, no per-code control,
  no GLONASS entry). List every band you want — `signals = ["R2C"]` alone derives
  a single frequency.

### Fixed

- **`init_raw()` initializes the new `raw_t.sigcfg`/`sigcfg_set` fields**
  (PR #190 review) — `init_raw()` sets members individually, so the
  `malloc()`+`init_raw()` paths (stream converter, `convbin`) could otherwise
  evaluate the sigcfg-driven branch on uninitialized state. Defaults to the
  legacy path.

### Known limitations

- Decoder code-selection from `[positioning].signals` is **Septentrio/SBF only**
  in this release; the remaining decoders, RTCM3 observations and `convbin` are
  tracked in [#189](https://github.com/h-shiono/MRTKLIB/issues/189) (Phase 2).
- GLONASS in MADOCA-PPP requires dual-frequency observations (G1 + G2). On a
  GLONASS-L2C/A receiver, list `"R2C"` in `[positioning].signals`; an L2P-capable
  receiver works out-of-the-box.

## [v0.6.11] - 2026-05-25

**Tooling / developer experience** — no positioning change; all binaries and
solution outputs are **bit-identical** to v0.6.10. Establishes an enforced
formatter baseline, adds a smartphone SPP benchmark, and speeds up CI.

### Added

- **GSDC-2023 smartphone SPP benchmark** ([#165](https://github.com/h-shiono/MRTKLIB/issues/165))
  — a harness to evaluate SPP on Google's GSDC-2023 smartphone dataset, where the
  v0.6.10 SPP accuracy work (#116) is meant to pay off. `run_gsdc_benchmark.py`
  runs `mrtk` SPP (`gsdc_to_rinex.py` converts `device_gnss.csv` → RINEX,
  `download_brdc.py` fetches broadcast ephemeris); `compare_gsdc.py` scores NMEA
  vs the GSDC `ground_truth.csv`, reporting the official GSDC score (mean of p50 &
  p95) and a coverage (`Cov%`) column. Config `conf/benchmark/gsdc_p0.toml`; docs
  [`docs/reference/benchmark-gsdc.md`](docs/reference/benchmark-gsdc.md).
- **CI formatter gate** ([#166](https://github.com/h-shiono/MRTKLIB/issues/166),
  PR #179) — a build-free `.github/workflows/format.yaml` runs
  `clang-format --dry-run --Werror`, `taplo fmt --check`, and `ruff format --check`
  on every push / PR. Check-only (no auto-apply push-back); pinned tool versions
  (clang-format 21.1.6 + ruff 0.15.2 via pip, taplo 0.10.0 release binary verified
  by SHA-256); reuses `.clang-format-ignore`. `ruff check` (lint) is not gated.

### Changed

- **Repo-wide format sweep** (#166, PR #177) — `clang-format` (48 files),
  `taplo fmt` (3), and `ruff format` (23) applied across the tree with **no
  functional change**. A new `.clang-format-ignore` excludes vendored
  `src/core/tomlc99` and RTKLIB-derived `util/`; `.git-blame-ignore-revs` records
  the three sweep commits so `git blame` skips them.
- **Faster, network-free regression CI** (#175) — `ctest` runs in parallel
  (`-j4`, BLAS pinned to one thread/process for deterministic PPP-AR), timeout
  raised to 20 min; the IONEX TEC fixture is vendored
  (`tests/cmake/download_tec.cmake`) with an integrity check, removing a per-run
  network download.

### Docs

- `CONTRIBUTING.md` and `CLAUDE.md` document the three pinned formatters, their
  config files, the vendored-code exclusions, and local fix commands.

## [v0.6.10] - 2026-05-24

**Feature** — single-point positioning (SPP) accuracy enhancements: C/N0
weighting, IGG-III robust estimation with a pre-robust acceptance gate, and
time-differenced carrier phase (TDCP) velocity + jump rejection. All opt-in and
**default-off**, so existing behaviour is bit-identical.

### Added

- **SPP C/N0 (Sigma-ε) pseudorange weighting** (#116) — `varerr()` gains an
  optional SNR-dependent variance term, identical in form to the RTK engine's.
  Exposed as `[kalman_filter.measurement_error] snr_max / snr_error` (legacy
  `stats-snrmax` / `stats-errsnr`); this also makes the RTK engine's
  previously-unreachable SNR term configurable. `snr_error = 0` (default) → off.
- **SPP IGG-III robust re-weighting** (#116) — `estpos()` down-weights/rejects
  pseudorange outliers via a three-segment equivalent-weight function on the
  MAD-standardised residual. Gated by `[positioning] robust = "igg3"`
  (`robust_k0` / `robust_k1`); `robust = "off"` (default) → bit-identical.
- **SPP pre-robust acceptance gate** (#116) — when robust is active, the
  chi-square gate runs on the pre-robust all-satellite residuals so the
  weighting cannot defeat it (the piece that turns C/N0 + robust into a clean
  win rather than a tail-inflating one).
- **SPP TDCP velocity, slip detection and jump rejection** (#116) — `resdop()`
  uses the time-differenced carrier-phase rate (mm/s-class) when locked and
  slip-free, falling back to Doppler; a SPP-local cycle-slip detector and a
  TDCP-vs-code jump-rejection QC remove position spikes. Gated by
  `[positioning] tdcp` / `tdcp_jump`; `tdcp = false` (default) → bit-identical.
- **Benchmark `single` mode** — `scripts/benchmark/run_benchmark.py` gains an
  SPP mode (and prefers the unified `mrtk post`, with `rnx2rtkp` fallback);
  `conf/benchmark/single.toml` enables the features for evaluation.
- **Design record** [`docs/design/spp-accuracy.md`](docs/design/spp-accuracy.md)
  — rationale, per-step before/after benchmarks, and the deferred P5/P6.

### Performance

- On the PPC-Dataset (six urban-driving runs, mean), enabling the features
  improves SPP vs the baseline: **<2 m fix rate 41.2 → 61.2 %**, median (p68)
  **5.61 → 2.83 m**, p95 **17.71 → 12.42 m**, RMS **17.07 → 7.54 m**.

### Notes

- P5 (common-mode clock-jump) and P6 (position EKF) are deferred to a smartphone
  benchmark follow-up ([#165](https://github.com/h-shiono/MRTKLIB/issues/165)),
  where clock jumps and large jitter actually occur.

## [v0.6.9] - 2026-05-24

**Feature** — IGS-product **integer PPP-AR**: resolve integer ambiguities for
`correction = "igs"` using satellite phase biases from a Bias-SINEX OSB product
(post-processing).

### Added

- **Integer PPP-AR for `correction = "igs"`** (#142) — in the uncombined
  measurement model (`ionosphere = "est-stec"`), `corr_meas()` applies the
  per-signal satellite **code bias to `P`** and **phase bias to `L`** from the
  Bias-SINEX OSB (e.g. COD MGEX IAR `OSB.BIA`), making the float ambiguity
  integer-recoverable so `ppp_amb_ILS` can fix wide-/narrow-lane. Gated on
  uncombined mode, so the float iono-free path stays bit-identical.
- **Bias-SINEX phase-bias source** in `udsatpb()` (`pppsatpb` gains `2:bia`) —
  previously only SSR/FCB fed `nav->osb.spb`, so file-based OSB phase biases were
  loaded then dropped. Only reached when no SSR/FCB phase bias exists, so
  MADOCA / CLAS / VRS are unaffected.
- **Configs** `conf/igs/rnx2rtkp_igspppar.toml` (GPS+Galileo) and
  `conf/igs/rnx2rtkp_igspppar_gps.toml` (GPS-only).
- **Regression tests** `igs_pppar_ge` (GPS+GAL, asserts integer-fix rate via the
  new `compare_pos_abs.py --min-fix-rate`) and `igs_pppar_gps` (GPS-only,
  accuracy + dual-frequency AR path). Validated on GEONET FUJISAWA vs the GSI F5
  coordinate; the IGS float PPP is cross-checked against upstream RTKLIB 2.4.3
  (≈8.4 cm 3D, equivalent).
- **IGS-RTS PPP quick-start guide** `docs/guide/quickstart-igs-rts.md` and
  real-time sample `conf/igs/rtkrcv_igsrts.toml` (#148).

### Fixed

- **PPP-AR zero-ambiguity Kalman update** — the extra-wide-lane step called the
  filter with `na = 0` on a dual-frequency setup, invoking LAPACK `DGETRF` with
  `n = 0` and aborting the whole AR before wide-/narrow-lane. Now guarded
  (`na <= 0` is a no-op). Latent bug; benefits any dual-frequency PPP-AR.
  Multi-frequency AR (MADOCA / CLAS) is unaffected.

### Changed

- `resolve_correction()` — `correction = "igs"` with ambiguity resolution now
  requires `ionosphere = "est-stec"` (the iono-free combination has one ambiguity
  per satellite, so AR would otherwise be a silent no-op).

### Files Changed

| File | Change |
|------|--------|
| `src/pos/mrtk_ppp.c` | apply OSB per-signal code/phase bias in `corr_meas` for uncombined `igs` |
| `src/pos/mrtk_rtkpos.c` | Bias-SINEX phase-bias source in `udsatpb` (+ stale-bias clearing) |
| `src/pos/mrtk_ppp_ar.c` | guard zero-ambiguity Kalman update (DGETRF n=0) |
| `src/pos/mrtk_opt.c`, `src/pos/mrtk_options.c`, `include/mrtklib/mrtk_opt.h` | `igs`+AR validity; `pppsatpb` `2:bia` |
| `conf/igs/rnx2rtkp_igspppar*.toml`, `tests/data/igs/igs_testdata.tar.gz`, `CMakeLists.txt` | PPP-AR configs + regression tests + trimmed OSB |
| `scripts/tests/compare_pos_abs.py` | `--min-fix-rate` integer-fix-rate assertion |
| `docs/guide/configuration.md`, `docs/guide/quickstart-igs-rts.md`, `conf/igs/rtkrcv_igsrts.toml` | PPP-AR + RTKLIB cross-val docs; IGS-RTS guide (#148) |

## [v0.6.8] - 2026-05-23

**Feature** — real-time **IGS-RTS float PPP** via the `correction = "igs-rts"`
axis (RTCM-SSR / IGS-SSR), and the fix that routes it through the correct
measurement model.

### Added

- **`correction = "igs-rts"`** (#138) — conventional float PPP driven by a
  real-time RTCM-SSR / IGS-SSR (MT4076) correction stream (IGS01/03, CNES
  `CLK9x`, BKG, etc.). The decode-and-apply pipeline was already inherited from
  the MADOCA-via-RTCM integration; this unlocks the axis. `resolve_correction()`
  removes `igs-rts` from the reserved-reject (`gal-has` / `bds-b2b` stay
  reserved), adds it to the PPP validity matrix, and requires
  `satellite_ephemeris = "brdc+ssrapc"` / `"brdc+ssrcom"`. It is **not**
  auto-inferred (it shares `brdc+ssrapc` with `qzs-madoca`), so it must be set
  explicitly.
- **`conf/igs/rnx2rtkp_igsrts.toml`** sample config.
- **`igsrts` regression test** — AIRA00JPN (Aira, Japan, IGS) GPS+GAL, IGS
  combined RTCM-SSR + broadcast eph, validated against an IGS final SINEX
  coordinate (3D 1σ ≈ 0.18 m float; cross-checked against upstream RTKLIB 2.4.3
  `rnx2rtkp` on identical inputs).
- **`docs/releases/release-notes-v0.6.7.md`** — backfilled (was missing).

### Changed

- **`corr_meas()`** routes `igs-rts` through the **RTKLIB float-PPP measurement
  model** (shared with `correction = igs`), not the MADOCA SSR-bias path. The
  satellite orbit/clock still come from `satpos_ssr` (`EPHOPT_SSRAPC`); only the
  receiver measurement model changes. The per-signal RTCM-SSR code/phase bias is
  not applied — RTCM-SSR uses a different bias convention than MADOCA-CSSR (the
  code bias has the opposite sign), and applying it via the MADOCA path biases
  the solution by metres. The MADOCA / `gal-has` / `bds-b2b` SSR path is
  unchanged (bit-identical).

### Files Changed

| File | Change |
|------|--------|
| `src/pos/mrtk_opt.c` | unlock `CORR_IGS_RTS` in `resolve_correction()` (validity matrix + sateph guard) |
| `src/pos/mrtk_ppp.c` | route `igs-rts` through the RTKLIB float-PPP measurement model in `corr_meas` |
| `conf/igs/rnx2rtkp_igsrts.toml`, `conf/igs/rnx2rtkp_igsrts_test.toml` | sample + regression-test configs |
| `tests/data/igs/igsrts_testdata.tar.gz`, `CMakeLists.txt` | `igsrts` regression test |
| `docs/design/configuration.md`, `docs/reference/config-options.md` | `igs-rts` documented as implemented |

## [v0.6.7] - 2026-05-21

**Feature** — conventional **IGS precise-products float PPP** via a new
`correction` configuration axis, plus a fix that lets **F9P-class multi-GNSS
receivers** (GAL E5b / BDS B2I) actually use Galileo and BeiDou in iono-free PPP.

### Added

- **`correction` configuration axis** (#130, #136) — decouples the correction
  source from `mode`. Implemented values `none | igs | qzs-madoca | qzs-clas`
  (reserved, enum-only: `igs-rts | gal-has | bds-b2b`). `resolve_correction()`
  infers the source from `mode` + `satellite_ephemeris` when `correction` is
  omitted, and validates the (mode, correction) pair.
- **IGS precise-products float PPP path** — `corr_meas()` IGS branch applies
  optional P1-C1/P2-C2 DCB from `nav->cbias`, lets the float phase-ambiguity
  absorb the satellite phase bias, and never discards a measurement for a
  missing SSR bias. Reads SP3 / CLK / Bias-SINEX / ERP files.
- **IGS-products PPP regression test** (#137) — GSI GEONET station 3034
  (FUJISAWA), GPS+GAL, validated against the published GSI daily coordinate
  (`igs_setup`/`igs_ppp`/`igs_ppp_check`/`igs_cleanup`).
- **IFLC frequency-fallback regression test** (#135) — ECJ02 u-blox F9P,
  1 h window, exercising the GAL E5b / BDS B2I iono-free fallback against a
  full-day static-PPP reference coordinate (lightweight 0.9 MB data archive).
- **`compare_pos_abs.py`** — `--llh` and `--ecef` fixed-reference modes.

### Changed

- **`corr_meas()`** now branches by correction source (IGS files / SSR / none),
  replacing the previous SSR-only measurement path.
- For `correction = igs`, the iono-free pair is selected from the **available**
  observations (data-driven), and the legacy `pos2-sig` obsdef reshaping is
  skipped so a receiver's actual 2nd band survives. A one-line-per-system stderr
  notice reports any non-nominal 2nd-band auto-selection.

### Fixed

- **#135** — GAL E5b (`C7Q`) / BDS B2I (`C7I`) satellites are no longer silently
  dropped from iono-free IGS-product PPP on receivers without E5a/B3I (e.g.
  u-blox ZED-F9P). The fix is gated on `correction == igs`, is a no-op when the
  conventional slot-1 band is present (IGS geodetic regression `.pos` is
  bit-identical), and leaves MADOCA / CLAS / RTK paths unchanged.

### Files Changed

| File | Change |
|------|--------|
| `src/pos/mrtk_opt.c`, `include/mrtklib/mrtk_opt.h` | `correction` enum + `resolve_correction()` |
| `src/pos/mrtk_ppp.c` | `corr_meas` IGS/SSR/none branches; data-driven IFLC pair; auto-select notice |
| `src/pos/mrtk_spp.c` | data-driven IFLC 2nd-frequency for `correction=igs` |
| `apps/rnx2rtkp/rnx2rtkp.c` | `resolve_correction()` wiring; gate `apply_pppsig` off for IGS PPP |
| `conf/igs/*.toml` | IGS PPP sample configs |
| `tests/data/igs/*`, `CMakeLists.txt` | `igs_ppp` and `igs_iflc` regression tests |
| `scripts/tests/compare_pos_abs.py` | `--llh` / `--ecef` reference modes |
| `docs/design/configuration.md` | correction-axis design and validity matrix |

## [v0.6.6] - 2026-05-10

**Refactor** — unified `mrtk` subcommand help format and added GNU-style
long-option aliases (`--config`, `--output`, `--start`, `--end`, `--interval`,
`--trace`, `--input`, `--nav`, `--device`, `--port`, `--freq`, `--help`)
across all 10 subcommands. No behavior change for existing scripts; no
positioning-engine numerics modified.

### Added

- **Long-option aliases** for the most-used short flags in every `mrtk`
  subcommand. Implemented as a pre-pass that rewrites `argv[i]` pointers
  to existing short forms, so per-subcommand argument-parsing loops are
  untouched. Discoverable via `mrtk <subcommand> --help`.
- **`-h` / `--help` flag** added to eight subcommands that previously had
  no help flag at all (`run`, `ssr2obs`, `ssr2osr`, `bias`, `dump`).
  `relay`, `cssr2rtcm3`, and `l6extract` already supported `-h` / `--help`.
- **Shared CLI helpers** in `include/mrtklib/mrtk_cli.h` and
  `src/core/mrtk_cli.c`:
  - `mrtk_normalize_args()` — long→short alias pre-pass.
  - `mrtk_is_help_flag()` — recognises `-h` / `--help`.
- **`scripts/analysis/compare_rtcm3.py`** — RTCM3 byte-level diff utility
  preserved from the cssr2rtcm3 development workflow (PR #103).

### Changed

- **All subcommand help text** rewritten to a consistent
  `mrtk <subcommand>: <description>` header followed by a unified
  Usage / Options / Examples layout, replacing the legacy binary-name
  headers (`usage: rtkrcv`, `usage: rnx2rtkp`, `Synopsis convbin`,
  `NAME: recvbias`, etc.).
- **Help flags within subcommands**: nine subcommands now use `-h` /
  `--help`. Two intentional exceptions are preserved for backward
  compatibility:
  - `mrtk post`: `-h` remains the fix-and-hold AR flag; help is `-?` /
    `--help`.
  - `mrtk convert`: `-h FILE` remains the HNAV-output flag; help is
    `--help`.
- **`docs/guide/cli.md`** — option tables now show both `-short` and
  `--long` forms and call out the `-h` exceptions.
- **`docs/reference/rtkrcv-clas-realtime.md`** — retitled to "via
  `mrtk run`"; all in-text references to legacy `rtkrcv` / `rnx2rtkp`
  binaries updated to the unified-CLI form.

### Fixed

- N/A (refactor only, no functional bug fixes).

### Files Changed

| File | Change |
|------|--------|
| `apps/{convbin,cssr2rtcm3,dumpcssr,l6extract,recvbias,rnx2rtkp,rtkrcv,ssr2obs,ssr2osr,str2str}.c` | Unified help text, long-option aliases, help-flag handling |
| `include/mrtklib/mrtk_cli.h` | New — shared CLI helpers (long-alias map, help-flag recogniser) |
| `src/core/mrtk_cli.c` | New — implementation |
| `CMakeLists.txt` | Add `src/core/mrtk_cli.c`, version 0.6.5 → 0.6.6 |
| `docs/guide/cli.md` | Show both short and long forms; `-h` exceptions documented |
| `docs/reference/rtkrcv-clas-realtime.md` | "via `mrtk run`"; legacy binary refs updated |
| `scripts/analysis/compare_rtcm3.py` | New — RTCM3 byte-level diff (PR #103) |
| `CHANGELOG.md` | v0.6.6 entry |
| `mkdocs.yml` | v0.6.6 in Releases navigation |
| `README.md`, `CLAUDE.md` | v0.6.6 roadmap entry |

### Test Results

61/63 tests pass — identical to develop baseline. The two pre-existing
failures (`rtkrcv_rt`, `madocalib_pppar_ion_check`) reproduce on the
baseline with the same numerical signatures (3D RMS 0.016186 m on
baseline vs 0.016324 m here, well within numerical noise). Verified by
re-running the same two tests in a worktree at `origin/develop`'s
pre-refactor commit. **Zero regressions introduced.**

### PRs

- [#104](https://github.com/h-shiono/MRTKLIB/pull/104) —
  `refactor(cli): unify subcommand help format and add long-option aliases`

## [v0.6.5] - 2026-05-10

**Feature** — first official release of `mrtk cssr2rtcm3` (real-time CSSR→RTCM3 converter) and `mrtk l6extract`, with a Septentrio mosaic-G5 P3 hardware integration guide and a 24-hour endurance test.

### Added

- **`mrtk cssr2rtcm3` — public release** of the real-time QZSS CLAS L6D
  (CSSR) → RTCM3 MSM7 converter. Lets RTCM3-capable receivers consume
  CLAS as a Virtual Reference Station source. Outputs `1005` / `1006`
  base-station messages and MSM7 for GPS / Galileo / QZSS, with
  configurable signal remap (`G2X→2W`, `E1X→1C`, `E5X→5Q`, `J2X→2L`),
  selectable SNR model (fixed or elevation-based), and
  mosaic-CLAS-compatible station ID. Default config in
  `conf/cssr2rtcm3.toml`.
- **Elevation-based L6D PRN auto-selection** in `cssr2rtcm3` with
  configurable threshold (`l6d_elmin`, default `10°`), 30-second
  timeout, and 5° hysteresis. `l6d_prn_fixed` available to lock the
  source PRN. The legacy `-prn` flag is deprecated.
- **`mrtk l6extract` subcommand** — offline L6D / L6E frame extractor
  for SBF and UBX logs.
- **Septentrio mosaic-G5 P3 hardware integration guide**
  (`docs/hardware/cssr2rtcm3-mosaic-g5.md`) covering RxTools setup,
  `cssr2rtcm3 → mosaic-G5` VRS-RTK wiring, the alternative `mrtk run`
  PPP-RTK path, and a published 24-hour static endurance result
  (Fix 72.05 % / Float 27.94 %, 87,839 epochs).
- **`module:cssr2rtcm3` GitHub label** for issue/PR triage along the
  module axis (PR #99).
- **Plotting helpers** under `scripts/plotting/`:
  `parse_pvt.py` (NMEA / SBF parser with rigorous ECEF→ENU),
  `plot_pos.py` (ENU comparison plotter, supports CSV save),
  `sbf_plot.py` (real-time SBF position monitor).
- **Positioning-engines reference**
  (`docs/guide/positioning-engines.md`) explaining RTK vs. VRS-RTK
  vs. PPP-RTK.

### Fixed

- **Long-running daemon stability** — `actualdist()` now caps satellite
  enumeration at `MAXOBS` and skips non-CLAS systems
  (`d028b0c`, `7fb497f`), eliminating the `MAXSAT` overflow / GLONASS
  RK4 hangs that would otherwise stall multi-day runs.
- **10-minute DGPS dips eliminated** — broadcast-ephemeris IODE
  rollover used to leave CLAS SSR pointing at a vanished IODE for
  ~55 s, causing RTK to fall back to DGPS. New `nav_t.eph_prev` slot
  bridges the gap (`3a8cc51`).
- **L5 / E5a OSR corrections** — `clas_ssr2osr()` now applies CLAS
  L5/E5a corrections instead of leaving them invalid (`361b6df`).
- **Galileo / QZSS in MSM output** — Galileo and QZSS are now emitted
  in the RTCM3 stream (`21f7b1f`, `985fc34`); Galileo ephemeris (1045)
  broadcast added (`f5f2748`); broadcast ephemeris resent every 30 s
  (`c2ba3b6`).
- **VRS base position latched on first SPP fix** (`4fa4d65`) —
  removes the dominant cause of Float-only RTK on the converter side.
- **`osr[]` per-epoch clearing** in `clas_ssr2osr()` (`0145c30`) —
  prevents stale P / Φ being carried forward when no new correction
  arrived for a satellite.
- **δBIAS discontinuity compensation** per IS-QZSS-L6-005 §5.5.3.2
  (`2b7580e`).
- **Ephemeris lookup via `seleph()`** with IODE matching (`d64893b`).
- **Same-frequency code fallback** in `clas_osr_corrmeas()` so CLAS
  bias matching does not strand satellites that broadcast a different
  code on the same frequency (`d8d2f4b`).
- **VRS PP**: base position init enabled for VRS-RTK in
  post-processing, plus `selfreqpair` fallback in `zdres_sat` for E5a
  observations (`b034d48`, `ea296db`, `5295229` Galileo-only scope);
  regression dataset reaches 98.5 % Fix.
- **PVT-time-based RTCM3 pacing** at the 1 s grid (`e729983`) —
  RTCM3 delivery rate up to ~99.5 %.
- **SBF Type-2 sub-blocks** processed when the Type-1 signal is not
  in `obsdef` (#69 / `a3a1203`).

### Changed

- **`-prn` deprecated** in `cssr2rtcm3` in favour of `l6d_elmin`
  auto-selection. The flag is still accepted but emits a deprecation
  warning.

### Documentation

- **mosaic-G5 RTCMv3 input gotcha** documented after a 24h
  SPP-stuck diagnosis: on firmware `20250611b`, the receiver does
  not reliably auto-detect RTCMv3 — the receive-side port must be
  set to `RTCMv3` explicitly (`0ea0147`).
- **macOS serial setup**: use `/dev/cu.*` not `/dev/tty.*` for
  serial output (DCD blocking).

### Known limitations

- **Vertical-component dispersion (~30 s sawtooth)** — [#97](https://github.com/h-shiono/MRTKLIB/issues/97).
- **Fix-rate gap vs. mosaic-CLAS reference (~20 %)** — [#98](https://github.com/h-shiono/MRTKLIB/issues/98). Concentrated in two windows (h ≈ 04–05 and h ≈ 14–18 GPST); satellite count and correction age are healthy in both.

### Test Results

62/62 tests pass (no regressions).

## [v0.6.4] - 2026-04-16

**Patch** — rtkrcv stability fixes and GitHub Community Profile completion.

### Fixed

- **rtkrcv status-poll SIGSEGV** (#74) — `prstatus()` `mode[]` array had 8 entries but `PMODE_PPP_RTK`/`PMODE_VRS_RTK` etc. index beyond that. Expanded to 13 entries with bounds checks on both `mode[]` and `freq[]`.
- **rtkrcv status-path data race** (#74) — `prstatus()` shallow-copied `rtk_t` under lock, leaving `x`/`P`/`xa`/`Pa` aliased to heap buffers the processing thread keeps mutating via `rtkpos()`. Now extracts position + covariance diagonal into local variables under the lock and nulls the shared pointers after unlock.
- **rtkrcv SIGSEGV handler safety** (#82, #85) — Crash handler is now async-signal-safe (`write(2)` instead of `fprintf()`) and re-raises the signal after restoring the default handler, so OS core-dump capture still fires.

### Added

- **GitHub issue and PR templates** (#88) — Five issue templates (`bug_report`, `positioning_issue`, `feature_request`, `documentation`, `question`) plus a PR template with `ctest` slot and positioning-regression check.
- **Declarative label scheme** (#88, #90) — `.github/labels.yml` with 34 labels across six axes (type/module/mode/gnss/priority/status), synced to GitHub by `EndBug/label-sync@v2` on push to `main`.
- **CONTRIBUTING.md** (#92) — Issue reporting, fork + upstream-remote workflow, branch/PR conventions targeting `develop`, coding standards, positioning-regression guard, label reference, BSD 2-clause inbound=outbound.
- **SECURITY.md** (#92) — Private Vulnerability Reporting flow; scope explicitly also covers Code of Conduct reports via the same advisory channel.
- **CODE_OF_CONDUCT.md** (#92) — Contributor Covenant 2.1 verbatim.
- **Crash-diagnostic build flags** — `-rdynamic` on Linux for SIGSEGV backtrace symbolization.
- **CLAS real-time Grafana dashboard link** in README for users monitoring `mrtk run`.

### Test Results

62/62 tests pass (no regressions).

## [v0.6.3] - 2026-03-31

**Feature** — NTRIP v2 (HTTP/1.1) protocol support with auto-negotiation.

### Added

- **NTRIP v2 client** — HTTP/1.1 GET requests with `Host:` and `Ntrip-Version: Ntrip/2.0` headers; automatic `HTTP/1.1 200 OK` response parsing.
- **NTRIP v2 server** — HTTP/1.1 POST requests with `Transfer-Encoding: chunked` (replaces legacy `SOURCE` command).
- **Chunked transfer encoding** — Incremental, non-blocking decoder and stateless encoder in header-only `ntrip_chunk.h`.
- **Version auto-detection** — Tries v2 first, falls back to v1 transparently; per-stream `?ver=N` override.
- **`strsetntripver()`** — Public API for setting the global default NTRIP version.
- **URL percent-decoding** — `%XX` sequences in NTRIP user/password fields are now decoded (e.g., `%40` -> `@`).
- **NTRIP streams guide** — New documentation page (`docs/guide/ntrip.md`) with path format, version selection, TLS tunnel setup (stunnel/socat), and troubleshooting.
- **`t_ntrip` unit test** — 17 tests for chunked codec and HTTP helpers.

### Changed

- `ntrip_t` struct extended with version negotiation, chunked state, and host fields.
- `ntripc_con_t` struct extended with per-client NTRIP version tracking.
- NTRIP caster sends chunked encoding to v2 clients, raw data to v1 clients.

### Test Results

63/63 tests pass (62 existing + 1 new; no regressions).

## [v0.6.2] - 2026-03-13

**Enhancement** — Documentation site with MkDocs Material + Doxygen + GitHub Pages.

### Added

- **MkDocs Material documentation site** — Modern, responsive docs with dark mode toggle, search, and navigation tabs.
- **Documentation pages** — Landing page, installation guide, first-run tutorial, CLI reference, TOML configuration guide.
- **Configuration options reference** — Auto-generated from `conf2toml.py` MAPPING table via `scripts/docs/gen_config_ref.py`.
- **Doxygen API reference integration** — Separate HTML build linked from MkDocs site navigation.
- **GitHub Actions deployment** — `.github/workflows/docs.yaml` deploys to GitHub Pages on push to `main`.

### Changed

- `Doxyfile.in`: `HTML_OUTPUT` changed from `html` to `api` for MkDocs site integration.
- Existing technical docs (`benchmark.md`, `rtkrcv-clas-realtime.md`, `test-accuracy-methodology.md`) included in site navigation.

### Test Results

62/62 tests pass (no regressions).

## [v0.6.1] - 2026-03-12

**Enhancement** — TOML configuration UX improvements.

### Added

- **`positioning.systems` string list** — Human-readable constellation selection
  (e.g., `["GPS", "Galileo", "QZSS"]`) as alternative to numeric `constellations` bitmask.
- **`excluded_sats` string list** — Satellite exclusion as TOML array
  (e.g., `["G01", "G02", "+E05"]`) alongside legacy space-separated string.
- **`taplo` TOML formatter** — Project-wide formatter config (`taplo.toml`) with
  VSCode format-on-save integration via Even Better TOML extension.

### Changed

- `tidal_correction` moved from `[positioning.atmosphere]` to `[positioning.corrections]`.
- All 20 TOML config files formatted with `taplo` and migrated to `systems` string list.
- `conf2toml.py` updated: outputs `systems` list, adds `siglist` type, taplo-compatible output.

### Removed

- Obsolete `[unknown]` sections from `conf/malib/*.toml` (per-satellite-type signal
  options tracked in [#59](https://github.com/h-shiono/MRTKLIB/issues/59)).

### Test Results

62/62 tests pass (no regressions).

## [v0.6.0] - 2026-03-12

**Feature** — Unified `mrtk` single CLI binary.

### Added

- **Unified `mrtk` CLI** (`apps/mrtk/mrtk_main.c`) — BusyBox/Git-style single
  binary with subcommand routing: `run`, `post`, `relay`, `convert`, `ssr2obs`,
  `ssr2osr`, `bias`, `dump`.

### Changed

- All 8 app `main()` functions renamed to `mrtk_*()` entry points.
- `showmsg`/`settspan`/`settime` consolidated into single shared implementation.
- Large static variables (`rtksvr_t`, `rtcm_t`) converted to heap allocation,
  reducing `__DATA` segment from 3,032 MB to 34 MB.
- CMake: 8 individual build targets replaced by single `mrtk` target.
- Version bumped to 0.6.0.

### Removed

- Individual executables (`rnx2rtkp`, `rtkrcv`, `str2str`, `convbin`, etc.)
  replaced by `mrtk <subcommand>`.

### Test Results

62/62 tests pass (no regressions).

## [v0.5.7] - 2026-03-12

**Feature** — Port RTKLIB `convbin` and `str2str` CLI applications.

### Added

- **`convbin` CLI app** (`apps/convbin/convbin.c`) — Converts receiver binary
  log files (RTCM 2/3, NovAtel, u-blox, Septentrio, etc.) to RINEX obs/nav and
  SBAS message files.
- **`str2str` CLI app** (`apps/str2str/str2str.c`) — Stream-to-stream data relay
  with optional format conversion (serial, TCP, NTRIP, file).
- **`mrtk_convrnx.c`** — Core RINEX translation logic (library module), ported
  from upstream `convrnx.c`.
- **`mrtk_streamsvr.c`** — Stream server functions (library module), ported from
  upstream `streamsvr.c`.

### Removed

- All WIN32-specific code (`#ifdef WIN32`, `winsock2.h`, `WSAStartup`, etc.)
  removed from ported files; POSIX-only paths retained.
- Constellation enable macros (`ENAGLO`, `ENAGAL`, `ENACMP`, etc.) removed;
  all constellations unconditionally enabled.

### Test Results

59/59 tests pass (no regressions).

## [v0.5.6] - 2026-03-12

**Feature** — RINEX 4.00 CNAV/CNV2 navigation message support.

### Added

- **GPS/QZSS CNAV decoder** (`decode_eph_cnav()`) — Parses 8-orbit CNAV and
  9-orbit CNV2 ephemeris records, including Adot, Delta_n0_dot, URAI indices,
  and ISC corrections (ISC_L1CA, ISC_L2C, ISC_L5I5, ISC_L5Q5, ISC_L1Cd).
- **BDS CNAV decoder** (`decode_eph_bds_cnav()`) — Parses CNV1 (B1C, 9 orbits),
  CNV2 (B2a, 9 orbits), and CNV3 (B2b, 8 orbits) ephemeris records with
  SISAI/SISMAI accuracy indices and BDS-specific TGD/ISC fields.
- **STO/ION CNVX support** — CNAV-source system time offset and ionosphere
  records (`STO Gxx CNVX`, `ION Cxx CNVX`, etc.) are now parsed alongside
  their LNAV/D1D2 equivalents.
- **RINEX 4 unit tests** — `t_rinex4.c` with 4 test cases verifying record
  counts and field sanity against DLR BRD400 merged broadcast data.

### Changed

- **EPH decode trigger** — Replaced the fixed `i >= 31` (7-orbit) threshold
  with v4type-aware logic: 35 fields for 8-orbit types (GPS/QZSS CNAV, BDS
  CNV3) and 39 fields for 9-orbit types (GPS/QZSS CNV2, BDS CNV1/CNV2).

### Test Results

62/62 non-realtime tests pass (59 existing + 3 new RINEX 4 tests).

## [v0.5.5] - 2026-03-11

**Bug fix** — CLAS real-time positioning via UBX does not work
([#31](https://github.com/h-shiono/MRTKLIB/issues/31)).

### Fixed

- **`decode_rxmqzssl6()`** — L6D frames (msg=0) were unconditionally sent to
  the MADOCA decoder which silently dropped them (vendor_id≠2).  Now branches on
  the `msg` field: L6D returns `ret=10` directly, routing to the CLAS decoder
  via the redirect block.
- **UBX 2-channel L6D demux** — UBX streams interleave L6D frames from two QZS
  satellites.  The redirect block now extracts the PRN from the L6 frame header
  and routes frames to separate CLAS channels (ch0/ch1), preventing subframe
  assembly corruption in `clas_input_cssr()`.
- **`l6delivery[]` initial value** — Channel assignment checked `== 0` but the
  field is initialized to `-1`.  Fixed to `< 0`.

### Added

- **`conf/claslib/rtkrcv_ubx_clas.toml`** — Template config for real-time CLAS
  PPP-RTK via u-blox UBX TCP streams (F9P obs + D9C L6D).
- **CSSR decode trace** — Diagnostic trace output for L6D redirect and CSSR
  epoch decode events.

## [v0.5.4] - 2026-03-11

**Signals architecture redesign** — Introduces `mrtk_band_t` enum (26 physical
frequency bands), structured signal priority table (`SIG_PRIORITY_TABLE`), and
explicit `signals = ["G1C", "G2W", ...]` TOML configuration.  Replaces the
legacy string-based `codepris[]` system with a single, structured source of truth
for signal priority.  No algorithmic changes; all positioning output is identical.

### Added

- **`mrtk_band_t` enum** (`mrtk_foundation.h`) — 26 per-constellation physical
  frequency bands (GPS L1/L2/L5, GLO G1/G2/G3, GAL E1/E5a/E5b/E6/E5ab, QZS
  L1/L2/L5/L6, SBS L1/L5, BDS B1I/B1C/B2a/B2b/B2ab/B3, IRN L5/S) plus
  `MRTK_BAND_UNKNOWN` and `MRTK_BAND_MAX` sentinels.
- **`SIG_PRIORITY_TABLE`** (`mrtk_obs.c`) — 26-entry structured priority table
  using C99 designated initializers and `CODE_*` constants, replacing the opaque
  `codepris[7][MAXFREQ][16]` string arrays.
- **`mrtk_rinex_freq_to_band()`** — RINEX frequency digit → `mrtk_band_t`
  conversion, including GLONASS CDMA aliases (freq 4→G1, freq 6→G2).
- **`mrtk_get_signal_priority()`** — Structured (sys, band) → priority-ordered
  code array lookup.
- **`mrtk_band2freq_hz()`** — Physical band → base carrier frequency (Hz).
- **`mrtk_band_to_freq_num()`** — Reverse mapping: band → RINEX frequency number.
- **`mrtk_parse_signal_str()`** — Parse RINEX3-style signal strings (e.g.,
  "G1C" → `SYS_GPS`, `MRTK_BAND_GPS_L1`, `CODE_L1C`).
- **`mrtk_sigcfg_from_signals()`** — Build per-system signal config from a string
  array, auto-deriving `nf` (number of frequencies).
- **`mrtk_sigcfg_to_obsdef()`** — Bridge signal config to existing obsdef tables
  via `set_obsdef()`.
- **`mrtk_signal_t` / `mrtk_sigcfg_t` types** (`mrtk_obs.h`) — Signal
  configuration structures for per-constellation band + preferred code settings.
- **`prcopt_t.sigcfg[7]` / `sigcfg_set`** (`mrtk_opt.h`) — Processing option
  fields for explicit signal configuration.
- **TOML `signals` array support** (`mrtk_toml.c`) — `[positioning] signals`
  key accepts TOML string arrays (e.g., `["G1C", "G2W", "E1C", "E5Q"]`),
  converted to CSV for the existing opt_t pipeline.  When present, overrides
  `frequency` setting and auto-derives `nf`.

### Changed

- **`getcodepri()`** — Rewritten to use `mrtk_rinex_freq_to_band()` +
  `mrtk_get_signal_priority()` instead of the legacy `codepris[]` string scan.
- **`code2idx()`** — Rewritten as `code2freq_num()` → `mrtk_rinex_freq_to_band()`
  → `band2idx_fixed()` pipeline, replacing 7 per-system `code2freq_*()` static
  functions (~140 lines removed).
- **obsdef tables** (`mrtk_nav.c`) — Annotated with `mrtk_band_t` names in
  comments for readability (e.g., `/* 0:L1 (MRTK_BAND_GPS_L1) */`).

### Removed

- **`codepris[7][MAXFREQ][16]`** — Legacy string-based signal priority array.
- **`setcodepri()`** — Legacy function to modify string priority table.
- **`get_codepris()`** — Legacy function to read obsdef codepris strings.
- **`obsdef_t.codepris` field** — Removed from struct and all 8 obsdef table
  initializers; signal priority now served exclusively from `SIG_PRIORITY_TABLE`.
- **7 × `code2freq_*()` functions** — `code2freq_GPS/GLO/GAL/QZS/SBS/BDS/IRN`
  replaced by band-based lookup pipeline.

### Known Limitations

- **`positioning.signals` and `[signals]` section conflict** — Configs that use
  the MADOCALIB per-system signal selection (`[signals] gps`, `galileo`, `bds2`,
  `bds3`, etc., mapped to `pos2-sig*`) cannot use `positioning.signals` at the
  same time.  Both mechanisms call `set_obsdef()`, and the `[signals]` section
  overrides the obsdef ordering set by `positioning.signals`, causing incorrect
  frequency assignment.  Affected configs (MADOCALIB, MALIB, benchmark/madoca)
  retain `frequency` for now.  A future release will unify these two mechanisms.

### Test Results

56/56 non-realtime tests pass — unchanged from v0.5.3.

---

## [v0.5.3] - 2026-03-11

**Full clang-format release** — Applies `clang-format` (Google style, 4-space
indent, 120-column limit) to the entire codebase.  No functional or algorithmic
changes.

### Changed

- **Full clang-format application** — All 116 source and header files under
  `src/`, `apps/`, and `include/` formatted with `clang-format` using the
  project `.clang-format` configuration.  Excludes vendored `src/core/tomlc99/`.
- **Idempotency verified** — Re-running `clang-format --dry-run --Werror`
  produces zero violations.

### Scope

116 files, +48,027 / -46,636 lines.

### Test Results

56/56 non-realtime tests pass — unchanged from v0.5.2.

---

## [v0.5.2] - 2026-03-10

**Code quality release** — Enforces mandatory braces on all control flow
statements and eliminates nested/complex ternary operators across 67 source
files.  No functional or algorithmic changes.

### Changed

- **Mandatory braces on control flow** — 4,053 single-statement `if`/`for`/
  `while`/`else` blocks wrapped in explicit `{}` braces, enforced by Clang-Tidy
  `readability-braces-around-statements`.
- **Nested ternary elimination** — 19 nested ternary operators (`a ? b ? c : d : e`)
  converted to `if`-`else` chains or `switch`-`case` statements.  Three
  `#define NT()` / `NT_RTK()` macros replaced by `static inline` functions.
- **Complex ternary cleanup** — 8 ternary operators with side-effect assignments
  or function-call branches refactored to explicit `if`-`else`.
- **Changed-line formatting** — All modified lines formatted with
  `git clang-format` (Google style, 4-space indent, 120-column limit).

### Scope

67 files, +12,727 / -5,085 lines.  Excludes vendored `src/core/tomlc99/`.

### Test Results

56/56 non-realtime tests pass — unchanged from v0.5.1.

---

## [v0.5.1] - 2026-03-10

**Dual-channel CLAS fix rate bug fix** — Resolves a significant performance
degradation in dual-channel CLAS real-time PPP-RTK positioning.  RT fix rate
improves from 67% to 93%; PP fix rate improves from 87% to 99%.

### Fixed

- **2ch CLAS frequency configuration** — `rtkrcv_2ch.toml` used
  `frequency = "l1+2+3"` (nf=3), but CLAS does not provide Galileo E5a bias
  corrections.  This caused false L1-L5 geometry-free cycle slip detection at
  every epoch, destroying Galileo ambiguities and preventing AR convergence.
  Changed to `frequency = "l1+2"` (nf=2).

### Improved

- **`gen_l6_tag.py` tick_scale calculation** — L6 tag file timing now always
  matches the master tag's scale, not only when the master appears compressed.
- **`gen_l6_tag.py` UTC→GPST time basis** — L6 tag `time_time` now includes
  GPS-UTC leap seconds to match the GPST basis used by `gen_bnx_tag.py` and
  RTKLIB's `strsync()`.  Previously ~18 s mismatch caused L6 data to replay
  slightly early relative to observations.

### 2ch CLAS performance (2025/157 dataset)

| Metric | v0.4.4 (nf=3) | v0.5.1 (nf=2) |
|--------|:---:|:---:|
| RT fix rate | 67.4% (2428/3600) | **92.6% (3335/3600)** |
| PP fix rate | 87.1% | **99.4%** |

### Test Results

59 tests — unchanged from v0.5.0.

---

## [v0.5.0] - 2026-03-10

**TOML configuration migration** — Replaces the legacy RTKLIB `key=value` `.conf`
format with TOML v1.0.  All 19 configuration files converted to semantic TOML
sections.  Zero-regression: all 59 tests pass with bit-identical positioning output.

### Added

- **TOML v1.0 parser** — [tomlc99](https://github.com/cktan/tomlc99) (MIT, C99)
  vendored at `src/core/tomlc99/`.
- **`mrtk_toml.c` / `mrtk_toml.h`** — C TOML loader with 230-entry mapping table.
  Navigates TOML tree, converts values to strings, and feeds them through the
  existing `searchopt()` + `str2opt()` infrastructure.
- **`scripts/tools/conf2toml.py`** — Python converter: legacy `.conf` → `.toml`
  with full enum resolution (bare integers → named strings), rtkrcv stream/console
  key support, and batch conversion mode.
- **19 TOML config files** — claslib (9), madocalib (3), malib (2), benchmark (5).
  Options grouped semantically: `[positioning]`, `[ambiguity_resolution]`,
  `[kalman_filter]`, `[streams.*]`, etc.

### Changed

- **`loadopts()` auto-detection** — Checks file extension; `.toml` dispatches to
  `loadopts_toml()`, other extensions use the legacy parser.
- **CTest** — All 59 test commands switched from `-k *.conf` to `-k *.toml`.
- **`run_rtkrcv_test.sh`** — TOML-aware config patching (Python regex for output
  path and playback speed).
- **`run_benchmark.py`** — References updated to `.toml`.
- **Reference generation scripts** — Updated to use `.toml` configs.

### Removed

- **19 legacy `.conf` files** — All configuration files under `conf/` replaced by
  `.toml` equivalents.
- **`toml11` vcpkg dependency** — Removed (C++17, incompatible with C11 codebase);
  replaced by vendored tomlc99.

### Test Results

59 tests — unchanged from v0.4.4.  Bit-identical output verified on CLAS PPP-RTK
(7,160-line NMEA, 0 diff between `.conf` and `.toml`).

---

## [v0.4.4] - 2026-03-09

**Dual-channel CLAS real-time PPP-RTK** — Extends `rtkrcv` to process two independent
CLAS L6D correction streams by repurposing the unused base-station stream slot for L6
ch2.  Achieves 67.4% fix rate on the 2025/157 dual-channel dataset (PP baseline: 88%).

### Added

- **2ch CLAS real-time via `rtkrcv`** — `inpstr2` (internal index 1, base slot, unused
  in PPP-RTK) carries L6 ch2; `inpstr3` (internal index 2) carries L6 ch1.  CLAS channel
  is derived from the 0-based stream index: `ch = (index == 1) ? 1 : 0`.
- **`rtkrcv_2ch.conf`** — Configuration for dual-channel BINEX+L6 file replay.
- **`rtkrcv_rt_clas_2ch` CTest** — Regression test replaying 1 hour of 2ch data at 10x
  speed (~372 s wall time).  Uses `RESOURCE_LOCK rtkrcv_port`.

### PP vs RT performance (2025/157 dual-channel dataset)

| Metric | PP (rnx2rtkp) | RT (rtkrcv) |
|--------|:---:|:---:|
| Fix (Q=4) | ~3,168 (88%) | 2,428 (67.4%) |
| Float (Q=5) | ~432 (12%) | 1,155 (32.1%) |
| SPP (Q=1) | 0 (0%) | 17 (0.5%) |

RT fix rate gap resolved in v0.5.1 (root cause: nf=3 config, not stream sync).

### Test Results

59 tests (58 from v0.4.3 + 1 new `rtkrcv_rt_clas_2ch`).

---

## [v0.4.3] - 2026-03-09

**Real-time CLAS PPP-RTK** — Enables `rtkrcv` to perform CLAS PPP-RTK positioning
using BINEX/SBF observations and CLAS L6D corrections via file-stream replay.
Achieves 97.7% fix rate on the 2019/239 reference dataset, matching post-processing
steady-state accuracy after convergence.

### Added

- **Real-time CLAS PPP-RTK via `rtkrcv`** — The CLAS PPP-RTK engine now runs inside
  the rtksvr real-time pipeline.  Supported input stream combinations: BINEX+L6,
  SBF+L6, RTCM3+UBX.
- **L6 rate limiter** (`mrtk_rtksvr.c`) — Pauses L6 correction processing when it
  runs >60 s ahead of observations, protecting the CLAS bank ring buffer (32 entries)
  from overwrite during file replay.
- **`gen_bnx_tag.py`** — BINEX time-tag generator; parses 0x7F-05 observation records
  to extract epoch timestamps and create `.tag` files for `::T::xN` replay.
- **`gen_l6_tag.py`** — L6 time-tag generator with `--sync-tag` master synchronisation;
  auto-detects compressed master tags and adjusts tick_n scaling.
- **`rtkrcv_rt_clas` CTest** — Automated regression test replaying 1 hour of BINEX+L6
  data at 10x speed (~370 s wall time).  Uses `RESOURCE_LOCK rtkrcv_port` to prevent
  parallel conflicts with the MADOCA `rtkrcv_rt` test.
- **rtkrcv configurations** — Three configs for different stream combinations:
  `rtkrcv.conf` (BINEX+L6), `rtkrcv_sbf_l6d.conf` (SBF+L6),
  `rtkrcv_rtcm3_ubx.conf` (RTCM3+UBX).
- **Documentation** ([docs/rtkrcv-clas-realtime.md](docs/rtkrcv-clas-realtime.md)) —
  Setup guide, PP vs RT comparison, architecture overview, troubleshooting.

### Changed

- **`run_rtkrcv_test.sh`** — Parameterised to accept config file path, port, and
  max timeout; previously hardcoded to the MADOCA test configuration.
- **Debug fprintf cleanup** — Removed temporary debug print statements from
  `mrtk_clas.c`, `mrtk_clas_grid.c`, `mrtk_clas_osr.c`, and `mrtk_ppp_rtk.c`.

### PP vs RT performance (2019/239 dataset)

| Metric | PP (rnx2rtkp) | RT (rtkrcv) |
|--------|:---:|:---:|
| Fix (Q=4) | 3,575 (99.86%) | 3,517 (97.72%) |
| Float (Q=5) | 5 (0.14%) | 5 (0.14%) |
| SPP (Q=1) | 0 (0.00%) | 77 (2.14%) |

77 Q=1 epochs = initial convergence period (~77 s).
Steady-state fix rate identical to post-processing.

### Test Results

58 tests (57 from v0.4.2 + 1 new `rtkrcv_rt_clas`).

---

## [v0.4.2] - 2026-03-08

**PPP-RTK / PPP accuracy release** — Extends the [demo5](https://github.com/rtklibexplorer/RTKLIB)
kinematic algorithm improvements from the RTK engine (v0.4.1) to the CLAS PPP-RTK
and MADOCA PPP engines.  RTK and VRS-RTK engines are unchanged.

### Added

- **`detslp_dop()` for PPP-RTK and PPP** — Doppler-based cycle-slip detection with
  clock-jump mean removal, identical to the RTK implementation ported in v0.4.1.
  Activated by `pos2-thresdop > 0.0` (library default: 0 = disabled).
- **`detslp_code()` for PPP-RTK and PPP** — Observation-code-change cycle-slip
  detector; flags slips whenever a satellite's tracked signal code changes between
  epochs, indicating a receiver re-acquisition that shifts the integer ambiguity.
- **`ph[0][f]` / `pt[0][f]` update in PPP `update_stat()`** — PPP did not previously
  record phase observations needed by `detslp_dop()`; the update was added to enable
  Doppler slip detection on the next epoch.

### Changed

- **`ephpos()` GLONASS clock rejection** — Added `if (fabs(geph->taun) > 1.0) return 0`
  guard, matching the protection already present in `ephclk()`.  Corrupted GLONASS
  clock entries (`|taun| > 1 s`) could propagate incorrect `dts[0]` into PPP-RTK and
  PPP via `satpos_ssr()`.
- **PPP-RTK PAR — position variance gate (B2)** — `ppp_rtk_pos()` now skips AR when
  the mean diagonal of the 3 × 3 position covariance block exceeds `thresar[1]`,
  preventing premature fixing before filter convergence.
- **PPP-RTK PAR — arfilter (B3)** — After PAR fails, newly-locked satellites
  (lock count = 0) are backed off and LAMBDA is retried, matching the RTK `arfilter`
  behaviour (activated by `pos2-arfilter`).
- **Full per-constellation EFACT in `varerr()`** — Both `mrtk_ppp_rtk.c` and
  `mrtk_ppp.c` now expand the system error factor to all seven constellations
  (GAL/QZS/CMP/IRN) via `mrtk_const.h` constants; previously GPS/GLO/SBS only.
- **Adaptive outlier threshold in `residual_test()` (PPP-RTK only)** — The
  sigma-normalised rejection threshold is inflated 10× for phase residuals whose
  corresponding bias state was just initialised (`P[IB,IB] == SQR(std[0])`).

### Fixed

- **PPP adaptive outlier threshold regression** — An equivalent D2 threshold inflation
  applied to PPP's absolute-metres check (`|v| > maxinno`) caused a severe accuracy
  regression (tokyo_run1 MADOCA RMS: 1.8 m → 199 m).  Undifferenced PPP residuals can
  legitimately exceed tens of metres at initialisation; inflating the threshold by 10×
  admitted extreme outliers.  The change was reverted; PPP retains the original
  fixed-threshold check.

### CLAS benchmark summary (6-run PPC-Dataset, FIX tier, `--skip-epochs 60`)

| Run | Fix% (v0.3.3) | Fix% (v0.4.2) | RMS_2D fix (v0.3.3) | RMS_2D fix (v0.4.2) | 1σ (v0.4.2) |
|-----|:---:|:---:|:---:|:---:|:---:|
| nagoya_run1 | 17.0% | 17.0% | 1.105 m | 1.105 m | 0.402 m |
| nagoya_run2 | 26.9% | 23.4% | 1.088 m | 1.119 m | **0.461 m** |
| nagoya_run3 |  6.3% |  6.3% | 0.318 m | 0.318 m | 0.339 m |
| tokyo_run1  |  5.2% |  4.9% | 0.868 m | **0.747 m** | 0.244 m |
| tokyo_run2  | 21.7% | 21.7% | 0.590 m | **0.514 m** | 0.120 m |
| tokyo_run3  |  7.4% |  7.4% | 0.801 m | 0.801 m | 0.075 m |

2/6 Tokyo runs: FIX RMS_2D improved (−13–14%).
MADOCA results unchanged across all 6 runs.

### Test Results

All 57 tests pass (unchanged from v0.4.1).

---

## [v0.4.1] - 2026-03-07

**RTK accuracy release** — Ports the [demo5](https://github.com/rtklibexplorer/RTKLIB)
kinematic RTK algorithm improvements into MRTKLIB and resolves a false-fix persistence
defect that caused 1σ accuracy to degrade to 1.4 m in urban-canyon scenarios.

### Added

- **`pos2-arminfixsats`** — Minimum satellites required for AR (`nb >= minfixsats-1`
  DD pairs); guards against rank-deficient LAMBDA decompositions (library default: 0 = disabled;
  benchmark conf: 4).
- **`pos2-arfilter`** — AR candidate filter: newly-locked satellites that degrade
  the ambiguity ratio are temporarily excluded and re-introduced epoch by epoch
  (library default: off; benchmark conf: on).
- **`pos2-thresdop`** — Doppler-based cycle-slip threshold in cycles/s; 0 = disabled
  (library default: 0; benchmark conf: 1.0).
- **`stats-eratio3`** — Phase/code variance ratio for L5 frequency, completing
  independent eratio configuration for triple-frequency processing.
- **Partial AR (`manage_amb_LAMBDA`)** — Multi-attempt LAMBDA with PAR satellite
  exclusion loop and polynomial ratio threshold scaled to number of satellite pairs.
- **`detslp_code()`** — Observation-code-change-based cycle-slip detector for
  receiver signal-tracking transitions.
- **`detslp_dop()`** restored — Doppler-based slip detection with clock-jump
  mean-removal fix that prevented false triggers.
- **RTK benchmark results** — Full six-run PPC-Dataset results (Phase 4A–4F) added
  to `docs/benchmark.md` with Fix%, RMS_2D, 1σ, 95%, and TTFF.

### Changed

- **`varerr()`** — Rewritten with per-constellation elevation factors (GAL/QZS/CMP/IRN),
  SNR-weighted noise term, and correct IFLC variance scaling.
- **Outlier rejection threshold** — Adaptive 10× inflation for first-epoch or
  recently-reset biases; added `rejc<2` guard to prevent premature bias reset.
- **`seliflc()`** — Carrier-smoothed pseudo-range selection included in observation
  pre-processing.
- **Acceleration coupling gate** — State-transition acceleration term (`F[pos,acc]`)
  is disabled when position variance exceeds `thresar[1]`, preventing premature
  dynamics coupling before the filter has converged.
- **Half-cycle variance inflation** — Adds +0.01 m² to phase measurement variance
  when the LLI half-cycle ambiguity flag is set.
- **`seph2clk()` parenthesis fix** — Corrected iteration guard for SBAS ephemeris
  clock computation.
- **GF slip detector** — Added `thresslip==0` early-out guard; replaced early
  `return` with `continue` to prevent skipping subsequent satellite pairs.

### Fixed

- **False-fix persistence in urban canyons** (Phase 4F) — `holdamb()` constrains
  the Kalman ambiguity covariance to VAR_HOLDAMB = 0.001 cy², making wrong integer
  solutions effectively permanent until a cycle-slip or satellite-loss event.  A
  Phase 4D change had made the lock counter conditional (`lock++` only when
  `nfix>0 && fix[f]≥2`), which froze lock counts during `nfix=0` epochs and
  forced re-selection of the same wrong integers immediately after every false-fix
  break.  Reverting to unconditional `lock++` allows the eligible satellite set to
  diversify between fix attempts, enabling escape from the false-fix cycle.
  **Effect**: nagoya_run3 1σ restored from 1.373 m → 0.135 m (baseline: 0.128 m).

- **GLONASS clock rejection** — Ephemeris entries with `|taun| > 1 s` are now
  discarded; corrupted GLO clock data was propagating into positioning.

- **GLONASS health check** — Switched from generic `svh!=0` to ICD-specific bit
  mask `(svh&9)!=0 || (svh&6)==4`; GPS/Galileo/QZSS retain the `svh!=0` check.

- **vsat set by phase DD** (Phase 4E revert) — A demo5 change that set `vsat=1`
  only from phase DD residuals caused AR bootstrap deadlock in urban canyons where
  satellites have valid code DD but noisy phase.  Reverted to code-DD-based
  `vsat` (original behaviour).

- **Conditional lock increment** (Phase 4E revert) — A demo5 change that gated
  `lock++` on `nfix>0` prevented bootstrap from `nfix=0` at session start, blocking
  AR entirely until the first integer fix was achieved.

### RTK benchmark summary (6-run PPC-Dataset, FIX tier)

All results use city conf (`nagoya.conf` / `tokyo.conf`) for precise base-station
coordinates.  `--skip-epochs 60`.

| Run | Fix% (v0.3.3) | Fix% (v0.4.1) | 1σ (v0.3.3) | 1σ (v0.4.1) | TTFF (v0.4.1) |
|-----|:---:|:---:|:---:|:---:|---:|
| nagoya_run1 | 29.7% | 27.4% | 0.112 m | 0.112 m | 797 s |
| nagoya_run2 | 16.1% | **28.3%** | 0.154 m | 0.175 m | 0 s |
| nagoya_run3 |  8.2% | **10.1%** | 0.128 m | **0.135 m** | 74 s |
| tokyo_run1  |  3.4% |  2.9% | 0.027 m | **0.026 m** | 1841 s |
| tokyo_run2  | 18.3% | **22.1%** | 0.010 m | 0.020 m | 803 s |
| tokyo_run3  | 25.1% | **27.7%** | 0.013 m | 0.013 m | 658 s |

4/6 runs show improved fix rate vs v0.3.3; all runs maintain Phase-3-level 1σ accuracy.

### Test Results

All 57 tests pass (unchanged from v0.3.3):

| Test Suite | Tests |
|------------|-------|
| Unit tests | 12 |
| SPP / receiver bias / rtkrcv | 5 |
| MADOCA PPP / PPP-AR / PPP-AR+iono | 10 |
| CLAS PPP-RTK + VRS-RTK | 19 |
| ssr2obs / ssr2osr / BINEX | 4 |
| Tier 2 absolute accuracy | 2 |
| Tier 3 position scatter | 2 |
| Fixtures | 3 |

---

## [v0.3.3] - 2026-03-07

Minor release — kinematic positioning benchmark for urban driving evaluation.
No functional changes to the library.

### Added

- **Kinematic benchmark infrastructure** (`scripts/benchmark/`) — End-to-end
  pipeline evaluating CLAS PPP-RTK, MADOCA PPP, and kinematic RTK against the
  PPC-Dataset urban driving data:
  - `cases.py` — Metadata for 6 PPC-Dataset runs (GPS week/TOW, city/run IDs)
  - `download_l6.py` — Auto-download QZSS L6D (CLAS) and L6E (MADOCA) archive
    files; MADOCA PRN auto-probed from candidates `[209, 193, 194, 195, 196, 199]`
  - `compare_ppc.py` — NMEA vs `reference.csv` comparison; three-tier accuracy
    breakdown (FIX/FF/ALL for CLAS & RTK; PPP for MADOCA); computes 2D/3D RMS,
    1σ, 95%, TTFF, mean satellite count; optional PNG plots
  - `run_benchmark.py` — Orchestrator with result caching, layered `-k` conf
    support, and summary table; `--mode clas|madoca|rtk|both|all`
    (`both`=clas+madoca, `all`=clas+madoca+rtk, default `all`)
- **Benchmark configurations** (`conf/benchmark/`):
  - `clas.conf` — CLAS PPP-RTK: `ant1-anttype=*`, `pos2-isb=off`, NMEA output
  - `madoca.conf` — MADOCA PPP: `pos1-dynamics=on`, `ant1-postype=single`, NMEA
  - `rtk.conf` — Kinematic RTK: `pos1-frequency=l1+2+3`, `pos1-ionoopt=off`,
    `pos1-snrmask_r=on` (enables genuine triple-frequency AR)
  - `nagoya.conf` — City overrides: precise base LLH, antenna types, `ant2-antdelu`
  - `tokyo.conf` — City overrides: precise base LLH, antenna types, `ant2-antdelu`
- **Benchmark documentation** ([docs/benchmark.md](docs/benchmark.md)) —
  Dataset download instructions, L6 auto-download, three-mode execution walkthrough,
  three-tier metric definitions, result tables, and known limitations.

### Changed

- `.gitignore` — Added `data/benchmark/` exclusion (large L6/NMEA files).
- `ruff.toml` — Added `scripts/benchmark/*.py` to `D103` per-file-ignores.
- **PPC-Dataset attribution** — Corrected to credit the contest organiser:
  Precise Positioning Challenge 2024 (高精度測位チャレンジ2024) by the
  Institute of Navigation Japan (測位航法学会); data published by Prof. Taro
  Suzuki (Chiba Institute of Technology).
- **Benchmark disclaimer** — Added note that parameters are not tuned and
  results are for reference only.

### Fixed

- **`rnx2rtkp` multi-`-k` conf loading** — `resetsysopts()` was called inside
  the `-k` processing loop, resetting values already loaded by earlier conf files.
  Moved to before the loop so layered city overrides (`nagoya.conf`, `tokyo.conf`)
  take effect correctly.
- **`loadopts()` leading whitespace** — Values after the `=` separator now have
  leading whitespace stripped, preventing key lookup failures for entries like
  `ant1-anttype       = *`.
- **MADOCA `misc-timeinterp`** — Was inadvertently `off` in the benchmark conf;
  restored to `on`, matching upstream MADOCALIB behaviour.

### Dataset

The benchmark uses the **PPC-Dataset** from the Precise Positioning Challenge 2024
(高精度測位チャレンジ2024), organised by the Institute of Navigation Japan
(測位航法学会). Data published by Prof. Taro Suzuki (Chiba Institute of Technology):
<https://github.com/taroz/PPC-Dataset>

Six urban vehicle runs (Nagoya × 3, Tokyo × 3) with 5 Hz triple-frequency
multi-GNSS, 100 Hz IMU, and sub-centimetre Applanix POS LV 220 ground truth.

---

## [v0.3.2] - 2026-03-06

Patch release — three-tier test methodology with absolute accuracy and position
scatter checks. No functional changes to the library.

### Added

- **Three-tier test methodology** ([docs/test-accuracy-methodology.md](docs/test-accuracy-methodology.md))
  — Formalises Tier 1 (relative porting correctness), Tier 2 (absolute geodetic
  accuracy vs SINEX / GSI F5), and Tier 3 (position scatter / precision) layers.
- **Tier 2 absolute accuracy tests** (2 new CTest entries):
  - `madocalib_pppar_abs_check` — MADOCA PPP-AR vs IGS SINEX MIZU (GPS week 2383), 2D horizontal ≤ 0.100 m
  - `claslib_ppp_rtk_2ch_abs_check` — CLAS 2CH vs GSI F5 TSUKUBA3 2025/06/06, 2D horizontal ≤ 0.300 m
- **Tier 3 position scatter tests** (2 new CTest entries):
  - `madocalib_ppp_scatter` — MADOCA-PPP 2D scatter ≤ 0.150 m (1σ=7.4 cm, 95%=11.2 cm, skip=30)
  - `claslib_ppp_rtk_2ch_scatter` — CLAS 2CH scatter ≤ 0.400 m (1σ=5.8 cm, 95%=25.2 cm, skip=20)
- **New comparison scripts**: `compare_pos_abs.py`, `compare_nmea_abs.py` (Tier 2);
  `check_pos_scatter.py` (Tier 3).
- **`scripts/tests/_geo.py`** — Shared geodetic utilities (`blh2xyz`, `xyz2blh`,
  `xyz2enu`, `nmea_to_deg`) replacing 4× inline duplication across test scripts.
- **`ruff.toml`** at project root — Ruff linting for all Python scripts
  (`E`, `F`, `W`, `I`, `D` rules, Google docstring convention).
- **`.vscode/settings.json`** — Ruff format-on-save and `scripts/.venv` interpreter.

### Changed

- `scripts/pyproject.toml` — Reduced to package metadata only; ruff config moved
  to root `ruff.toml`.
- Three debug plot scripts moved from `scripts/tests/` to `scripts/plotting/`
  (`plot_gloeph.py`, `plot_igp.py`, `plot_peph.py`).

### Removed

- `scripts/fix_trace_fwd_decls.py`, `scripts/migrate_trace_ctx.py` — One-time
  migration tools no longer needed.
- `scripts/plotting/plot_lex_{eph,ion,ure}.py` — LEX service retired 2020.

### Fixed

- **NMEA ellipsoidal height** — `compare_nmea_abs.py` now correctly computes
  `h_ell = MSL + geoid_separation` from GGA fields [9] and [11]; previously
  field[11] was not summed, causing ~40 m Up errors.
- **Absolute check 2D mode** — `--use-2d` flag in `compare_pos_abs.py` now
  consistently governs both the reported metric and the pass/fail criterion.

### Test Results

All 57 tests pass (53 carried from v0.3.1 + 4 new):

| Test Suite | Tests |
|------------|-------|
| Unit tests | 12 |
| SPP / receiver bias / rtkrcv | 5 |
| MADOCA PPP / PPP-AR / PPP-AR+iono | 10 |
| CLAS PPP-RTK + VRS-RTK | 19 |
| ssr2obs / ssr2osr / BINEX | 4 |
| Tier 2 absolute accuracy | 2 ← new |
| Tier 3 position scatter | 2 ← new |
| Fixtures | 3 |

---

## [v0.3.1] - 2026-03-06

Patch release — MADOCALIB PPP-AR regression test quality improvement.
No functional changes.

### Changed

- **MADOCALIB PPP-AR reference data** — Replaced `pppar` and `pppar_ion`
  reference `.pos` files with outputs from upstream MADOCALIB built with
  `-DLAPACK` (Accelerate framework). The previous LU-solver reference caused
  artificial 1.5–3.8 cm offsets unrelated to porting correctness.
- **Test tolerances tightened** (when `LAPACK_FOUND` is true):
  - `madocalib_pppar_check`: 0.020 m → **0.008 m**
  - `madocalib_pppar_ion_check`: 0.040 m → **0.005 m**
  - Fallback to original thresholds when LAPACK is unavailable (internal LU vs LAPACK reference diverges ~1.5–3.8 cm)

### Background

Investigation confirmed that upstream MADOCALIB already supports LAPACK via
`#ifdef LAPACK` in `rtkcmn.c`. Building upstream with `-DLAPACK -framework
Accelerate` reveals the true implementation difference: MRTKLIB vs upstream
LAPACK is **0.41 cm** (pppar) and **0.25 cm** (pppar_ion), confirming the
porting is correct. The 1.5–3.8 cm difference seen in v0.3.0 was entirely
attributable to LU vs LAPACK numerical divergence within the upstream itself.

### Test Results

All 53 tests pass. Tolerances when built with LAPACK (tight) vs without (wide):

| Test | 3D RMS | Tolerance (LAPACK) | Tolerance (no LAPACK) |
|------|--------|-------------------|----------------------|
| madocalib_pppar | 0.41 cm | 0.008 m (~50% margin) | 0.020 m |
| madocalib_pppar_ion | 0.25 cm | 0.005 m (~50% margin) | 0.040 m |

---

## [v0.3.0] - 2026-03-06

CLASLIB integration — adds QZSS CLAS L6D augmentation (PPP-RTK and VRS-RTK)
via [CLASLIB](https://github.com/QZSS-Strategy-Office/claslib) ver.0.8.2 (`9e714b9`).
Includes the complete CSSR decoder, grid correction engine, PPP-RTK positioning,
VRS double-differenced RTK, dual-channel L6 support, and SSR→OSR utilities.

### Added

- **CLAS CSSR decoder** (`mrtk_clas.c`) — Full ST1–ST12 Compact SSR message
  decoder supporting IS-QZSS-L6-007 two-channel L6 input.
- **CLAS grid correction engine** (`mrtk_clas_grid.c`) — Tropospheric and STEC
  grid interpolation derived from GSILIB v1.0.3 algorithms.
- **PPP-RTK positioning engine** (`mrtk_ppp_rtk.c`) — CLAS centimeter-level
  augmentation using double-differenced carrier-phase OSR corrections.
  Accuracy: **5.9 cm 3D RMS, 99.86% fix** (single-channel, DOY 239/2019).
- **VRS DD-RTK positioning engine** (`mrtk_vrs.c`, ~2050 lines) — Virtual
  Reference Station double-differenced RTK using CLAS OSR corrections.
  Accuracy: **3.3 cm 3D RMS, 99.86% fix** (DOY 239/2019).
- **Multi-L6E dual-channel support** — `nav->ssr_ch[SSR_CH_NUM][MAXSAT]` and
  `_mcssr[SSR_CH_NUM]` decoder array support two independent L6 channels
  (ST12 mode, IS-QZSS-L6-007). ST12 accuracy: **10.8 cm 3D RMS, 98.75% fix**.
- **BINEX file reading** — `readbnxt()` enables post-processing from Septentrio
  BINEX raw observation files.
- **Per-system frequency selection** (`posopt[10–12]`) — Independent L1/L2/L5
  signal selection for GPS, GLONASS, Galileo, and QZSS.
- **`ssr2obs` utility** — Standalone tool converting CLAS L6 corrections to
  RINEX3 VRS pseudo-observations; also outputs OSR CSV and RTCM3 MSM4 format.
- **`ssr2osr` utility** — Post-processing OSR verification tool (SSR→OSR via
  `postpos()` with `PMODE_SSR2OSR`).
- **`dumpcssr` utility** — CSSR message dump tool for L6 stream inspection.
- **`clas_ssr2osr()` wrapper** — Public API for SSR→OSR conversion callable
  from within `rtkpos()`.
- **23 new regression tests** — PPP-RTK (10), VRS-RTK (9), ssr2obs/ssr2osr (3),
  BINEX (1); total test count raised from 30 to 53.
- **NMEA comparison infrastructure** (`scripts/tests/compare_nmea.py`) —
  GGA-based accuracy comparison for VRS-RTK regression testing.

### Changed

- **`rtkpos()` dispatch** — Added `PMODE_PPP_RTK`, `PMODE_VRS_RTK`, and
  `PMODE_SSR2OSR` branches; CLAS context initialised in `postpos()`.
- **`prcopt_t`** — Extended `err[5]→err[8]` (CLAS measurement noise model) and
  `posopt[6]→posopt[13]` (per-system frequency and PPP-RTK knobs).
- **`sat_lambda()` in PPP-RTK** — Switched from obsdef table lookup to direct
  obs-code→frequency mapping, fixing QZS L2 wavelength (0 → correct value)
  and raising fix rate from 97% → 99.86%.
- **`clas_osr_zdres()`** — When `y==NULL` (VRS mode), writes OSR directly to
  the caller's buffer without an intermediate `osrtmp` redirect.
- **`tidedisp()` call** — Corrected bitmask routing (1=solid, 2=OTL, 4=pole)
  for grid OTL displacement.
- **Unified copyright headers** — All source files (lib + apps, ~112 files)
  updated with full 9-party contributor lineage, SPDX identifier, and
  Doxygen `@file` blocks separated from license comments.
- **`LICENSE.txt`** — Added Mitsubishi Electric Corp. (2015–) and Geospatial
  Information Authority of Japan (2014–) copyright lines.
- **`README.md`** — CLASLIB marked Integrated (was Planned); License &
  Attributions table added.

### Fixed

- **`set_ssr_ch_idx()` / `set_mcssr_ch()`** — Added `[0, SSR_CH_NUM)` bounds
  guard to prevent out-of-bounds access on `nav->ssr_ch` and `_mcssr` arrays.
- **`memset` sizeof targets** — `biaosb->spb` and `biaosb->vspb` now use
  `sizeof` of their own member (previously used sibling member's size).
- **SIS/IODE boundary detection** — `clas_osr_satcorr_update()` uses
  `prev_orb_tow` to detect orbit-epoch advancement; removed incorrect
  `orb_tow == clk_tow` requirement that broke ST12 (orbit always 5 s ahead
  of clock in ST12 streams).
- **`clas_mode` gate in `postpos()`** — `PMODE_SSR2OSR` included in CLAS
  context initialisation check.
- **`compensatedisp()` SSR crash** — Fixed null pointer dereference when SSR
  corrections are not yet available.

### Known Limitations

- **Real-time CLAS**: `rtkrcv` supports a single CLAS stream (channel 0).
  Real-time dual-L6 channel switching is not yet implemented.
- **Real-time L6D**: Ionospheric STEC correction (L6D) is not available in
  real-time mode. PPP-AR+iono requires post-processing (`rnx2rtkp`).
- **Static mode**: PPP-RTK in static receiver mode is not yet validated
  (test data unavailable).

### Test Results

All 53 tests pass:

| Test Suite | Tests | Key Accuracy |
|------------|-------|-------------|
| Unit tests | 12 | — |
| SPP regression | 2 | — |
| Receiver bias | 2 | — |
| rtkrcv real-time | 1 | — |
| MADOCA PPP / PPP-AR / PPP-AR+iono | 10 | <0.5 cm / ~1.6 cm / ~3.8 cm 3D RMS |
| CLAS PPP-RTK (single-ch / dual-ch / ST12 / L1CL5) | 10 | 5.9 cm / — / 10.8 cm / — |
| CLAS VRS-RTK (single-ch / ST12 / dual-ch) | 9 | 3.3 cm / — / — |
| ssr2obs / ssr2osr | 3 | — |
| BINEX reading | 1 | — |
| Fixtures (setup/cleanup/download) | 3 | — |

### Upstream References

- **CLASLIB**: [claslib](https://github.com/QZSS-Strategy-Office/claslib) ver.0.8.2 (`9e714b9`)

---

## [v0.2.0] - 2026-03-02

MADOCALIB PPP engine integration — replaces the MALIB PPP engine with
[MADOCALIB](https://github.com/QZSS-Strategy-Office/madocalib) ver.2.0 (`8091004`)
for higher-accuracy PPP/PPP-AR processing with L6E and L6D correction support.

### Added

- **MADOCALIB PPP/PPP-AR engine** — Full integration of the MADOCALIB positioning
  algorithms (PPP, PPP-AR, PPP-AR+iono) from upstream ver.2.0, replacing the
  previous MALIB PPP stub.
- **L6E SSR decoder** (`mrtk_madoca.c`) — QZSS L6E orbit/clock/bias correction
  stream decoder (MCSSR format).
- **L6D ionospheric decoder** (`mrtk_madoca_iono.c`) — QZSS L6D wide-area STEC
  ionospheric correction decoder for PPP-AR+iono post-processing.
- **Real-time PPP** — `rtkrcv` now runs MADOCALIB PPP engine with L6E corrections
  via SBF file stream replay; verified with `rtkrcv_rt` regression test.
- **MADOCALIB regression tests** (8 tests) — PPP, PPP-AR (mdc-004/mdc-003),
  PPP-AR+iono accuracy checks against upstream reference data (MIZU station).
- **Python-based `.pos` comparison tool** (`tests/cmake/compare_pos.py`) —
  Configurable tolerance comparison for regression testing.
- **Unified copyright headers** — All 104 source files updated with full
  7-party contributor lineage and SPDX-License-Identifier.

### Changed

- **PPP engine** — `pppos()` in `mrtk_ppp.c` is now the MADOCALIB implementation
  (previously MALIB). Includes PPP-AR (`mrtk_ppp_ar.c`) and ionospheric
  correction (`mrtk_ppp_iono.c`) modules.
- **Signal selection** — Unified to obsdef tables aligned with upstream MADOCALIB
  (`code2idx()` frequency index mapping).
- **SPP ISB model** — Aligned with upstream MADOCALIB observation selection.
- **BLAS guard** — Added zero-dimension guard in `matmul()` to prevent BLAS
  `dgemm` LDC=0 errors when filter state count is zero.
- **`prev_qr[]` state management** — Zero-initialized in `rtkinit()` and updated
  after `rtkpos()` in both `procpos()` and `rtksvrthread()` to ensure correct
  `const_iono_corr()` convergence detection.
- **File naming** — Renamed for consistency with `mrtk_madoca_*` convention:
  - `mrtk_rtcm3lcl.c` → `mrtk_rtcm3_local_corr.c`
  - `mrtk_mdciono.c` → `mrtk_madoca_iono.c`
- **CI workflow** — Updated accuracy analysis to use MADOCALIB PPP output
  (`out_madocalib_ppp.pos`) with MIZU station reference coordinates.
- **`prn[6]` documentation** — Corrected field comment from `dcb` to `ifb`
  (Inter-Frequency Bias); `stats-prndcb` retained as legacy alias.
- **`prcopt_t` cleanup** — Removed unused `ppp_engine` field and related
  constants/configuration (`pos1-ppp-engine`).

### Removed

- **`mrtk_ppp_madoca.c`** — Dead stub file (SOLQ_NONE output only); the actual
  MADOCALIB engine is integrated directly into `mrtk_ppp.c`.
- **`MRTK_PPP_ENGINE_*` constants** — Engine selection mechanism removed as
  MADOCALIB is now the sole PPP engine.

### Known Limitations

- **Real-time L6D**: Ionospheric STEC correction input is not available in
  real-time mode (`rtkrcv`). PPP-AR+iono requires post-processing (`rnx2rtkp`).
- **Single correction stream**: `rtkrcv` supports one correction input
  (`inpstr3`). Multiple L6E channels require receiver-side multiplexing.
- **LAPACK numerical difference**: MRTKLIB uses system LAPACK while upstream
  MADOCALIB uses an embedded LU solver, causing ~1.5–3.8 cm 3D RMS differences
  in PPP-AR solutions. Test tolerances are adjusted accordingly.

### Test Results

All 30 tests pass (12 unit + 18 regression):

| Test Suite | Tests | Status |
|------------|-------|--------|
| Unit tests | 12 | PASS |
| SPP regression | 2 | PASS |
| Receiver bias | 2 | PASS |
| rtkrcv real-time | 1 | PASS |
| MADOCA PPP | 2 | PASS |
| MADOCA PPP-AR (mdc-004) | 2 | PASS |
| MADOCA PPP-AR (mdc-003) | 2 | PASS |
| MADOCA PPP-AR+iono | 2 | PASS |
| Fixtures (setup/cleanup) | 5 | PASS |

### Upstream References

- **MALIB**: [JAXA-SNU/MALIB](https://github.com/JAXA-SNU/MALIB) feature/1.2.0 (`f006a34`)
- **MADOCALIB**: [madocalib](https://github.com/QZSS-Strategy-Office/madocalib) ver.2.0 (`8091004`)

## [v0.1.0] - 2026-02-23

Initial release — MALIB structural migration complete.

### Added

- **MRTKLIB core architecture** — Thread-safe `mrtk_ctx_t` context, POSIX/C11
  pure implementation, CMake build system.
- **Domain-driven directory structure** — `src/core/`, `src/pos/`, `src/data/`,
  `src/rtcm/`, `src/models/`, `src/stream/`, `src/madoca/`.
- **Applications** — `rnx2rtkp` (post-processing), `rtkrcv` (real-time),
  `recvbias` (receiver bias estimation).
- **Regression test framework** — CTest-based SPP, receiver bias, and real-time
  PPP tests with reference data comparison.
- **MALIB integration** — Structural base from JAXA MALIB feature/1.2.0
  (directory layout, threading, stream I/O).

[v0.5.4]: https://github.com/h-shiono/MRTKLIB/compare/v0.5.3...v0.5.4
[v0.5.3]: https://github.com/h-shiono/MRTKLIB/compare/v0.5.2...v0.5.3
[v0.5.2]: https://github.com/h-shiono/MRTKLIB/compare/v0.5.1...v0.5.2
[v0.5.1]: https://github.com/h-shiono/MRTKLIB/compare/v0.5.0...v0.5.1
[v0.5.0]: https://github.com/h-shiono/MRTKLIB/compare/v0.4.4...v0.5.0
[v0.4.4]: https://github.com/h-shiono/MRTKLIB/compare/v0.4.3...v0.4.4
[v0.4.3]: https://github.com/h-shiono/MRTKLIB/compare/v0.4.2...v0.4.3
[v0.4.2]: https://github.com/h-shiono/MRTKLIB/compare/v0.4.1...v0.4.2
[v0.4.1]: https://github.com/h-shiono/MRTKLIB/compare/v0.3.3...v0.4.1
[v0.3.3]: https://github.com/h-shiono/MRTKLIB/compare/v0.3.2...v0.3.3
[v0.3.2]: https://github.com/h-shiono/MRTKLIB/compare/v0.3.1...v0.3.2
[v0.3.1]: https://github.com/h-shiono/MRTKLIB/compare/v0.3.0...v0.3.1
[v0.3.0]: https://github.com/h-shiono/MRTKLIB/compare/v0.2.0...v0.3.0
[v0.2.0]: https://github.com/h-shiono/MRTKLIB/compare/v0.1.0...v0.2.0
[v0.1.0]: https://github.com/h-shiono/MRTKLIB/releases/tag/v0.1.0
