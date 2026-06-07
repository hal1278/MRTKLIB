# Release Notes — v0.6.13

## Enhanced PPP-RTK SPP seed + RINEX converter frequency unification

**Release date:** 2026-06-07
**Type:** Positioning improvement (PPP-RTK/VRS-RTK kinematic) + converter refactor
**Branch:** `release/v0.6.13`

---

### Overview

v0.6.13 bundles two independent changes:

1. **Enhanced a-priori SPP seed for PPP-RTK / VRS-RTK**
   ([#196](https://github.com/h-shiono/MRTKLIB/pull/196)) — the per-epoch
   single-point fix that seeds the CLAS/VRS precise filter now uses the proven
   v0.6.10 SPP error model (C/N0 weighting + TDCP jump-reject) **by default**,
   improving kinematic fix rate and the fixed-epoch error tail. **This is a
   positioning change** for PPP-RTK/VRS-RTK; static positioning is unchanged.

2. **RINEX converter unified on obsdef frequency indexing**
   ([#71](https://github.com/h-shiono/MRTKLIB/issues/71),
   PR [#195](https://github.com/h-shiono/MRTKLIB/pull/195)) — `mrtk convert`
   derives observation-type frequency indices from the same obsdef tables every
   decoder uses. **Converter output change only** (some obs-type columns reorder);
   positioning output is byte-identical.

---

### 1. Enhanced a-priori SPP seed for PPP-RTK / VRS-RTK (#196)

#### Mechanism

Every epoch, `pntpos` computes a single-point fix into `rtk->sol.rr`. On a filter
reset (observation-loss gap, persistent-FLOAT, large PPP-RTK↔SPP divergence) the
CLAS/VRS filter re-initialises its position from that seed, and `resamb_LAMBDA`
builds the ambiguity-resolution float vector from it. A better seed therefore
helps the kinematic, reset-prone case.

#### The key enabler — no CLAS corruption

The seed runs on a **private option copy** (`prcopt_t sppopt = rtk->opt`). The
C/N0 coefficient slots `err[5]/err[6]` mean `snr_max`/`snr_error` in the SPP
`varerr`, but the **iono/trop estimation-error terms** in the CLAS `varerr`. They
are therefore set on the copy only, leaving the CLAS measurement model untouched.

#### Configuration

`[positioning.clas] enhanced_spp_seed` (option key `pos1-seedenh`):

| Value | Seed gets |
|-------|-----------|
| `off` | nothing — bit-identical to prior behaviour |
| `cn0+tdcp` | C/N0 weighting + TDCP jump-reject (**default**) |
| `cn0+tdcp+robust` | + IGG-III robust (open-sky opt-in) |

#### Results (PPC-Dataset, 6 urban kinematic runs)

`cn0+tdcp` vs baseline: mean **fix rate 23.7 → 24.7 %** (+1.0 pp), mean
fixed-epoch **p95 3.23 → 3.02 m**; largest single gain on tokyo_run1
(p95 5.44 → 3.52 m). No large-misfix regression.

#### Why IGG-III robust is opt-in, not default

Robust lifts the mean fix rate a further +1.0 pp and helps open-sky runs, but in a
deep urban canyon (tokyo_run3) it shifts the seed enough to trip the filter into
fix-and-hold mis-fixes (mis-fixes 5 → 71, worst fixed error 4.4 m → 12 m). The
harm is a filter-coupling effect invisible to seed-local diagnostics and could not
be auto-gated reliably (self-gate and filter-health gate were both prototyped and
rejected), so robust is left to the operator — use it where the environment is
open-sky, omit it in dense urban canyons. Full rationale in
[`docs/design/spp-accuracy.md`](../design/spp-accuracy.md) §11.

#### Safety / scope

- **Static (みなしローバー) positioning unaffected.** All six claslib static
  regression cases run byte-close to the off baseline — deltas sub-cm RMS, inside
  the existing test tolerances; one VRS case improves (fix 98.9 → 99.9 %). On
  clean continuously-tracked data the filter rarely falls back to SPP, so the seed
  is effectively inert.
- **Plain PPP / PPP-AR are excluded.** Investigated and confirmed the lever yields
  no benefit there: PPP couples to the seed far more weakly (loose `VAR_POS` prior,
  no reset-to-SPP snap, AR resolves from the converged float). An empirical probe
  on madocalib PPP-AR showed identical TTFF and post-convergence accuracy
  (2D RMS 4.172 → 4.170 cm). See `spp-accuracy.md` §11.
- **Real-time too.** `mrtk run` (real-time PPP-RTK/VRS-RTK) drives the same
  `rtkpos()` seed path, so the default profile is active in real time as well —
  arguably where it matters most (stream gaps and slips make real-time the
  reset-prone regime the seed targets). The benchmark numbers above are
  post-processing; the real-time effect has not yet been separately measured.

#### Config cleanup

This change ships with a config tidy-up: 31 deprecated commented
`# constellations` / `# frequency` lines (19 files), superseded by the active
`systems` / `signals` keys, were removed. Active `frequency` keys are untouched;
no option value changed.

---

### 2. RINEX converter unified on obsdef frequency indexing (#71, PR #195)

`mrtk convert` (convrnx) now derives observation-type frequency indices from
`code2freq_idx()` — the obsdef-based mapping already used by every receiver
decoder, the RINEX parser, RTCM3 and CLAS — and the legacy fixed per-band
`code2idx()` / `band2idx_fixed()` are removed.

> **Output change (converter only, no positioning impact).** RINEX obs-type
> columns now follow obsdef band order, so Galileo lists **E5a before E5b** and
> QZSS lists **L5 before L2** (BeiDou columns reorder similarly). The `-f` /
> `--freq N` band count keeps its meaning but now selects the same N bands the
> positioning engines use. Positioning output is byte-identical (the obsdef
> tables are untouched).

**Known limitations** (intentional — adding slots would perturb GLONASS/BeiDou
positioning):

- **GLONASS G3 (CDMA, GLONASS-K2 only)** — no `obsdef_GLO` slot.
- **BeiDou B2a+b (AltBOC)** — maps to the 6th obsdef slot, beyond the converter's
  5-band `--freq` mask.

Guarded by a new unit test `utest_t_freqidx`, which pins the `code2freq_idx()`
(sys, code) → frequency-index contract for GPS/GLONASS/Galileo/QZSS/BeiDou,
including the GLONASS G3 regression guard.

---

### Compatibility & migration

- **PPP-RTK / VRS-RTK users**: no config change required to gain the enhanced
  seed; it is on by default. Set `[positioning.clas] enhanced_spp_seed = "off"` to
  restore the prior seed exactly. `cn0+tdcp+robust` is available for open-sky
  deployments.
- **RINEX converter users**: scripts that key on a fixed obs-type column order
  must follow the obsdef band order (E5a-before-E5b, L5-before-L2). The signal set
  and `--freq N` count are otherwise unchanged.
- **Everyone else**: post-processing positioning output is unchanged by #71, and
  unchanged by #196 outside PPP-RTK/VRS-RTK.

### Tests

Full `ctest` regression: only the two documented pre-existing environment/numerical
failures (`rtkrcv_rt` headless replay, `madocalib_pppar_ion_check` LAPACK
tolerance); no new failures. The enhanced-seed default-off path is byte-identical;
all `claslib_*` / `vrs` static cases pass. New unit test `utest_t_freqidx` added
for #71. clang-format 21.1.6 / taplo 0.10.0 clean; no test tolerances changed.

### Issues / PRs

- [#196](https://github.com/h-shiono/MRTKLIB/pull/196) — enhanced a-priori SPP
  seed for PPP-RTK/VRS (default `cn0+tdcp`)
- [#71](https://github.com/h-shiono/MRTKLIB/issues/71) /
  PR [#195](https://github.com/h-shiono/MRTKLIB/pull/195) — RINEX converter
  unified on obsdef frequency indexing
