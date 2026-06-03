# Release Notes — v0.6.12

## Real-time MADOCA-PPP multi-GNSS signal selection

**Release date:** 2026-06-03
**Type:** Positioning feature — real-time MADOCA-PPP now uses non-GPS constellations (post-processing outputs unchanged)
**Branch:** `release/v0.6.12`

---

### Overview

v0.6.12 brings the **real-time** (`mrtk run`) MADOCA-PPP path up to parity with
post-processing for non-GPS constellations. Before this release, real-time
MADOCA-PPP effectively produced a GPS-only solution; this release makes
Galileo and QZSS usable, lets GLONASS L2C/A be selected as the iono-free 2nd
frequency, and hardens the signal-selection tables against in-process restarts.

It bundles four issues, originally surfaced by an external bug report
([#183](https://github.com/h-shiono/MRTKLIB/pull/183), thanks @hitoshimukai):

1. **Non-GPS signals in real time** — Galileo / QZSS ([#184](https://github.com/h-shiono/MRTKLIB/issues/184), PR #185)
2. **obsdef idempotency across restarts** ([#186](https://github.com/h-shiono/MRTKLIB/issues/186), PR #188)
3. **GLONASS L2C/A via `[positioning].signals`** ([#187](https://github.com/h-shiono/MRTKLIB/issues/187), PR #190)
4. **Docs**: `signals` positioned as the authoritative signal-selection surface

> **Positioning impact.** Real-time MADOCA-PPP solutions now include more
> constellations, so outputs differ from v0.6.11 (more satellites in the filter).
> Post-processing (`mrtk post`) is unchanged.

---

### 1. Non-GPS signals in real-time MADOCA-PPP (#184, PR #185)

The post-processing entry point calls `apply_pppsig()` after resolving the
correction source, but the real-time server (`startsvr()`) never did. As a
result the obsdef tables — consulted by every receiver decoder via
`code2freq_idx()` — kept their defaults, the non-GPS 2nd band was not mapped into
a frequency slot, and the madocalib PPP engine could only form the iono-free pair
for GPS.

`rtkrcv` now applies the configured PPP signal selection at server start,
mirroring `mrtk post`. IGS precise-product PPP keeps skipping it (#135) so a
receiver's actual 2nd band (e.g. GAL E5b / BDS B2I on a u-blox F9P) survives.
Galileo and QZSS are now used in real-time MADOCA-PPP, confirmed by the reporter.

### 2. obsdef idempotency across rtkrcv restarts (#186, PR #188)

`set_obsdef()` rewrote the global obsdef tables in place (zeroing bands not in
the selected pair), with no way to restore them — so a band trimmed by one
selection could not be recovered by the next. In the `rtkrcv` shell a
`load`+`restart` that changed the correction source or signal selection inherited
a corrupted obsdef, mis-mapping frequency slots in later runs.

`set_obsdef()` now rebuilds from a pristine default snapshot (captured before any
mutation), and a new `reset_obsdef()` is applied before re-selecting signals at
server start. The result is **bit-identical** to the previous behaviour on the
first/single configuration; only repeated/changed selections within one process
differ (the bug case). Guarded by `utest_t_obsdef`.

### 3. GLONASS L2C/A via `[positioning].signals` (#187, PR #190)

GLONASS was unused on a Septentrio **mosaic-CLAS** rover because the observations
came out single-frequency (L1 C/A only) — no iono-free pair. mosaic-CLAS outputs
GLONASS **L2C/A** (not L2P); the SBF decoder routes L2C/A to an extra observation
slot unless the legacy `-RL2C` option is set, and `rtkrcv` had no way to pass it.

`obsdef` is band-level and cannot distinguish L2C from L2P, but the signal config
parsed from `[positioning].signals` already carries a per-band **preferred code**.
This release consumes it: a new `mrtk_sigcfg_freq_idx()` helper resolves the
observation slot from sigcfg, and `raw_t` carries the configured `sigcfg`. With

```toml
[positioning]
signals = ["G1C", "G2W", "R1C", "R2C", "E1C", "E7Q", "J1C", "J2L"]  # nf=2; GLONASS 2nd band = L2C/A
```

GLONASS L2C/A is placed in the iono-free 2nd frequency with no `-RL2C`. Guarded by
`utest_t_sigcfg_decode`.

**Phase 1 covers the Septentrio (SBF) decoder** — which is the mosaic-CLAS case.
The remaining decoders (NovAtel / JAVAD / u-blox / BINEX), the RTCM3 observation
path and `convbin` are tracked in
[#189](https://github.com/h-shiono/MRTKLIB/issues/189) (Phase 2). The `-R/-G`
receiver options remain as the fallback when `signals` is unset, so existing
configurations are unaffected.

### 4. `signals` is now the recommended signal-selection surface

`[positioning].signals` (RINEX3-style tokens, e.g. `"R2C"`) is the authoritative
way to select signals: it overrides `frequency` and the `[signals]` presets,
derives the number of frequencies, **and** drives decoder code selection. List
every band you want — `signals = ["R2C"]` alone derives a single frequency. The
`[signals]` preset section (`gps`/`qzs`/`galileo`/`bds2`/`bds3`) is now documented
as **legacy**: coarse, no per-code control, and **no GLONASS entry** (which is why
GLONASS L2 code selection requires `signals`). See
[Configuration Options](../reference/config-options.md).

---

### Compatibility & migration

- **Real-time MADOCA-PPP users**: no config change is required to gain Galileo /
  QZSS. To use GLONASS on an L2C/A-only receiver, switch from the `[signals]`
  presets / `frequency` to a full `[positioning].signals` list that includes
  `"R2C"` (see §3). An L2P-capable receiver works without any change.
- **Everyone else**: when `[positioning].signals` is unset, signal selection and
  raw decoding are unchanged (legacy `-R/-G` path); post-processing is unchanged.

### Tests

Full `ctest` regression: only the two documented pre-existing environment/numerical
failures (`rtkrcv_rt` headless replay, `madocalib_pppar_ion_check` LAPACK
tolerance); no new failures. Two new env-independent unit tests added
(`utest_t_obsdef`, `utest_t_sigcfg_decode`). clang-format 21.1.6 clean; no test
tolerances changed.

### Issues / PRs

- [#184](https://github.com/h-shiono/MRTKLIB/issues/184) / PR #185 — non-GPS signals in real-time MADOCA-PPP
- [#186](https://github.com/h-shiono/MRTKLIB/issues/186) / PR #188 — obsdef idempotency across restarts
- [#187](https://github.com/h-shiono/MRTKLIB/issues/187) / PR #190 — GLONASS L2C/A via `[positioning].signals`
- [#189](https://github.com/h-shiono/MRTKLIB/issues/189) — Phase 2 (remaining decoders / RTCM3 / convbin), open
- [#183](https://github.com/h-shiono/MRTKLIB/pull/183) — originating external report (thanks @hitoshimukai)
