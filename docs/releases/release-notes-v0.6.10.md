# Release Notes — v0.6.10

## SPP accuracy: C/N0 weighting, IGG-III robust estimation, and TDCP

**Release date:** 2026-05-24
**Type:** Feature — single-point positioning (SPP) accuracy, opt-in / default-off
**Branch:** `release/v0.6.10`

---

### Overview

v0.6.10 modernises the **single-point positioning** engine (`PMODE_SINGLE`,
`mrtk post -p 0` / `mrtk run` with `mode = "single"`), which until now was a
near-verbatim RTKLIB 2.4.3 snapshot least-squares solver. It adds four staged,
independently-gated improvements (P1–P4 of the plan in
[`docs/design/spp-accuracy.md`](../design/spp-accuracy.md)):

1. **C/N0 (Sigma-ε) pseudorange weighting** — down-weight low-C/N0 signals.
2. **IGG-III robust re-weighting** — down-weight / reject pseudorange outliers.
3. **Pre-robust acceptance gate** — keep the chi-square gate effective under
   robust weighting (the piece that makes 1+2 a *clean* win).
4. **TDCP velocity + jump rejection** — mm/s-class velocity from
   time-differenced carrier phase, plus rejection of code position spikes that
   disagree with the TDCP displacement.

Every feature is **TOML-gated and off by default** (`prcopt_default` unchanged),
so any existing configuration produces **bit-identical** results; the gains
appear only when the new keys are set (see `conf/benchmark/single.toml`).

This is the SPP counterpart to the v0.6.7–v0.6.9 PPP/PPP-AR work on the
`correction` axis — a substantial new capability shipped as an opt-in patch.

---

### Major changes (#116)

#### 1. C/N0 (Sigma-ε) weighting

`varerr()` ([`src/pos/mrtk_spp.c`](https://github.com/h-shiono/MRTKLIB/blob/main/src/pos/mrtk_spp.c))
adds `σ² += (fact·snr_error)² · 10^(0.1·max(0, snr_max − C/N0))`, identical in
form to the RTK engine's `varerr()`. The parameters `snr_max` / `snr_error`
(`err[5]` / `err[6]`) were previously zero-initialised and unreachable from any
config; exposing them also makes the RTK engine's dormant SNR term configurable.
Gated by `snr_error > 0` (default 0 → off).

#### 2. IGG-III robust re-weighting + 3. pre-robust gate

`estpos()` applies an IGG-III three-segment equivalent-weight function to the
standardised pseudorange residuals, scaled by a **MAD robust scale**
(`σ̂ = max(1, 1.4826·median|r̃|)`; Rousseeuw & Croux 1993). Robust weighting
alone defeats the chi-square gate (it shrinks the residuals the gate inspects),
which inflates the error tail; so when robust is active, `valsol()` is fed the
**pre-robust, all-satellite** residuals — the outliers the robust pass suppresses
still trigger rejection of inconsistent epochs. Gated by `robust = "igg3"`
(default `"off"` → bit-identical).

#### 4. TDCP velocity, slip detection, jump rejection

- `pntpos()` stores per-satellite carrier-phase history (`ssat.ph/pt`) each epoch.
- `spp_detslp()` flags cycle slips (LLI + Doppler-vs-phase-rate consistency).
- `resdop()` uses the TDCP phase rate (mm/s-class) when locked and slip-free,
  else falls back to Doppler (preserving the Doppler-absence behaviour).
- A jump-rejection QC drops epochs whose code position change disagrees with the
  TDCP displacement (`velocity·Δt`) by more than `tdcp_jump`.

Gated by `tdcp = true` (default `false` → bit-identical).

---

### Performance

Measured with `scripts/benchmark/run_benchmark.py --mode single` on the
PPC-Dataset (six Nagoya/Tokyo urban-driving runs), mean across runs, baseline
(features off) → all four enabled:

| Metric | Baseline | v0.6.10 (enabled) | change |
|--------|---------:|------------------:|-------:|
| <2 m fix rate | 41.2 % | **61.2 %** | **+20.0 pp** |
| median (p68) | 5.61 m | **2.83 m** | **−50 %** |
| 95th pct (p95) | 17.71 m | **12.42 m** | **−30 %** |
| RMS 2D | 17.07 m | **7.54 m** | **−56 %** |

The jump-rejection QC in particular collapses the catastrophic tail (e.g. one
run's RMS 36.85 → 4.84 m).

---

### Configuration

Enable the features under `[positioning]` / `[kalman_filter.measurement_error]`,
e.g. (see `conf/benchmark/single.toml`):

```toml
[positioning]
mode = "single"
robust = "igg3"     # IGG-III robust re-weighting + pre-robust gate
robust_k0 = 1.5
robust_k1 = 4.0
tdcp = true         # TDCP velocity + jump rejection
tdcp_jump = 5.0     # m

[slip_detection]
doppler = 1.0       # Doppler-vs-phase slip threshold (cyc/s)

[kalman_filter.measurement_error]
snr_max = 50.0      # dB-Hz
snr_error = 0.5     # m  (0 = C/N0 weighting off)
```

---

### Tests

`ctest`: **zero regression**. The SPP regression (`rnx2rtkp_spp` /
`rnx2rtkp_spp_check`) and the PPP/IGS/RTK suites pass unchanged; the two
perennial failures (`rtkrcv_rt` — a headless-terminal environmental issue;
`madocalib_pppar_ion_check` — a LAPACK-vs-embedded-LU numerical-noise difference)
are pre-existing and unrelated. Default-off keeps all existing outputs
bit-identical.

---

### Deferred to the smartphone benchmark ([#165](https://github.com/h-shiono/MRTKLIB/issues/165))

- **P5 — common-mode clock-jump correction**: only relevant to receivers that
  step their clock (smartphones / low-cost), not the geodetic PPC set.
- **P6 — position EKF (track smoothing / coasting)**: an untuned first cut gave
  no gain and diverged on the geodetic kinematic data (the post-P1–P4 WLS is
  already good there); it belongs on a smartphone/static benchmark where jitter
  is large. See [`docs/design/spp-accuracy.md`](../design/spp-accuracy.md) §4.6.

A GSDC-2023 smartphone benchmark plus tuned P5/P6 re-attempt is tracked in #165.
