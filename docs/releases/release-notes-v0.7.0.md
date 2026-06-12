# Release Notes — v0.7.0

## Galileo HAS (High Accuracy Service) float PPP

**Release date:** 2026-06-12
**Type:** New feature (global SSR correction service: Galileo HAS)
**Branch:** `release/v0.7.0`

---

### Overview

v0.7.0 adds support for the **Galileo High Accuracy Service (HAS)** and opens the
v0.7.x series themed around global SSR correction services (BeiDou PPP-B2b is
planned next).

Galileo HAS is a **free, global high-accuracy service** that broadcasts SSR
corrections (satellite orbit, clock, and code bias) for **GPS and Galileo** on
the **E6-B C/NAV** signal — and, in parallel, over the internet. Its corrections
are referenced to the **GTRF/ITRF2020** frame. Combined with broadcast
ephemeris, HAS enables decimeter-level global PPP without a regional service
area.

MRTKLIB now decodes HAS corrections and applies them in **float PPP**, in both
**post-processing** (`mrtk post`) and **real-time** (`mrtk run`). The HAS
Initial Service broadcasts no phase biases and no ionosphere/troposphere
corrections, so this release targets **float** PPP (analogous to the MADOCA-PPP
float path); integer PPP-AR is a later phase contingent on the HAS phase-bias
broadcast.

There are **no positioning-algorithm or numerical changes** to the existing
PPP / PPP-AR / PPP-RTK engines; HAS feeds the same `satpos_ssr()` / `corr_meas()`
SSR application path already used by the other correction services.

---

### 1. What's new

#### A new HAS decoder (`src/has/`)

A self-contained decoder collects HAS pages, recovers each message with a
Reed–Solomon erasure decoder, and decodes the corrections:

- **Page collection** — HAS pages (24-bit header + encoded payload) are grouped
  by Message ID and page ID; once enough distinct pages of a message are
  received, the message is reconstructed.
- **RS(255,32,224) erasure decoder** — GF(256) Reed–Solomon decode recovers the
  message from the received pages. The systematic generator matrix is built at
  initialization by polynomial encoding (no embedded coefficient tables), and was
  validated against the worked example in **ICD Annex C**.
- **MT1 message decode** — Mask, Orbit, Clock (full-set / subset), Code-bias and
  Phase-bias blocks are decoded into `ssr_t` (`nav->ssr_ch[0]`), with HAS masks
  and IOD sets cached so corrections that omit a mask reference the previous one.

The decoded corrections flow through the existing SSR machinery: `satpos_ssr()`
applies the orbit/clock corrections to the selected broadcast ephemeris, and
`corr_meas()` applies the code biases (via a HAS-specific bias-code selector with
same-frequency fallback).

#### `correction = "gal-has"`

A new value of the `correction` configuration axis (introduced in v0.6.x)
selects HAS for PPP modes:

```toml
[positioning]
mode = "ppp-kine"                   # or ppp-static
correction = "gal-has"
satellite_ephemeris = "brdc+ssrapc" # SSR applied to broadcast ephemeris
systems = ["GPS", "Galileo"]
```

#### `.has` extraction and L6E support in `mrtk l6extract`

- **`.has` extraction** — `mrtk l6extract` extracts Galileo HAS pages from SBF
  **GALRawCNAV** (block 4024) into a fixed-record `.has` file, which `mrtk post`
  consumes directly (detected by extension).
- **mosaic-G5 L6E** — the same tool now also extracts QZSS **L6E** frames from
  SBF block **4271** (byte-identical to the L6D block 4270 except the Source
  field), written per-PRN +10 above the L6D PRN to match the MADOCA `.l6`
  convention.

---

### 2. Quick start

#### Capture requirements

- A receiver that tracks and outputs **Galileo E6-B C/NAV**, e.g. a **Septentrio
  mosaic-G5**, configured to log SBF **GALRawCNAV** (and the usual MeasEpoch /
  raw ephemeris blocks for RINEX conversion).
- For real-time, the same SBF stream feeds both observations/ephemeris and the
  HAS pages — a single stream is sufficient.

#### Post-processing

```bash
# 1. Extract HAS pages from the SBF capture
mrtk l6extract -i file.sbf -o file.has

# 2. Convert the SBF to RINEX obs/nav (same capture)
mrtk convert ...

# 3. Float PPP with HAS corrections
mrtk post -k conf/has/ppp_gal_has.toml obs nav file.has
```

`mrtk post` detects the `.has` infile by extension, feeds its records to the HAS
decoder, and applies the decoded corrections through the SSR branch.

#### Real-time

```bash
mrtk run -o conf/has/run_gal_has.toml
```

The real-time path takes a **single SBF stream**: the Septentrio decoder gains a
GALRawCNAV handler that routes HAS pages to the decoder while the same stream
supplies observations and broadcast ephemeris.

#### Key TOML keys

```toml
[positioning]
correction = "gal-has"
satellite_ephemeris = "brdc+ssrapc"
```

Both `conf/has/ppp_gal_has.toml` (post) and `conf/has/run_gal_has.toml`
(real-time) ship as ready-to-use starting points.

---

### 3. Validation

- **Decoder cross-validation** — the MT1 decoder was cross-validated
  **bit-exact against cssrlib** over **428 MT1 messages** decoded from a live
  mosaic-G5 capture (10 Galileo satellites, ~1 h). cssrlib (MIT License,
  © 2021 Rui Hirokawa, <https://github.com/hirokawa/cssrlib>) was used as an
  **independent development-time cross-reference only** — it is not bundled with
  or linked into MRTKLIB.
- **RS decoder** — validated against the worked decoding example in **ICD Annex
  C**.
- **Live float PPP repeatability** — over 1 h of live data the float PPP solution
  has a self-RMS of **E 7.5 / N 7.3 / U 38 cm**.
- **Real-time replay** — a real-time replay of the same data produces PPP float
  solutions on all epochs.
- **Bundled regression dataset** — `tests/data/has/has_testdata.tar.gz`
  (~2.1 MB) ships a block-filtered, 15-minute mosaic-G5 slice
  (`G5P3162a_15m.sbf`) and a `has_*` ctest pipeline (extract → convert → float
  PPP → accuracy gate). A **trim-equivalence proof** shows the trimmed slice is
  byte-identical to the corresponding full-hour window: the `.has` records are
  byte-identical (4834 records), the converted OBS records are identical (900
  epochs), and the PPP solutions are bit-identical over all common epochs.

---

### 4. Known limitations

- **Float PPP only** — the HAS Initial Service broadcasts **no phase biases**, so
  integer **PPP-AR** is not yet possible. PPP-AR is planned for when HAS reaches
  Full Operational Capability (FOC) with phase biases.
- **No ionosphere / troposphere corrections** — HAS carries none; the iono-free
  combination is used and the troposphere is estimated (`est-ztd`), as in the
  MADOCA-PPP float path.
- **Absolute accuracy under investigation** — the HAS float solution currently
  sits **~0.5–0.6 m horizontal** from a cm-grade ITRF reference, larger than
  mature PPP would give. Candidate causes (code-bias sign/selection,
  orbit/clock reference-point handling, troposphere estimation over a short
  window) are under investigation
  ([#215](https://github.com/h-shiono/MRTKLIB/issues/215)); published HAS
  Asia-region performance suggests 10–20 cm is attainable.
- **GPS + Galileo only** — the constellations HAS corrects.

---

### Compatibility & migration

- **No change for existing users.** HAS is opt-in via `correction = "gal-has"`;
  all existing PPP / PPP-AR / PPP-RTK / VRS configurations and outputs are
  unchanged.
- **New HAS users**: capture SBF GALRawCNAV from an E6-B-capable receiver and
  start from the bundled `conf/has/ppp_gal_has.toml` (post) or
  `conf/has/run_gal_has.toml` (real-time).

### Tests

Full `ctest` regression: the new `has_*` pipeline (extract / convert / float PPP
/ accuracy gate) passes, and the existing suite shows only the two documented
pre-existing environment/numerical failures (`rtkrcv_rt` headless replay,
`madocalib_pppar_ion_check` LAPACK tolerance); no new failures. clang-format
21.1.6 / taplo 0.10.0 clean; no test tolerances changed.

### Issues / PRs

- [PR #214](https://github.com/h-shiono/MRTKLIB/pull/214) — Galileo HAS support
  (decoder, `correction = "gal-has"`, `mrtk l6extract` `.has` / L6E extraction,
  bundled regression tests).
- [#215](https://github.com/h-shiono/MRTKLIB/issues/215) — HAS absolute-accuracy
  investigation (open).

### References

- Galileo HAS SIS ICD, Issue 1.0 (May 2022).
- cssrlib (MIT License, © 2021 Rui Hirokawa,
  <https://github.com/hirokawa/cssrlib>).
