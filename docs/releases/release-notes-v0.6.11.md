# Release Notes — v0.6.11

## Developer experience: formatter CI gate, GSDC smartphone benchmark, faster CI

**Release date:** 2026-05-25
**Type:** Tooling / developer experience — **no positioning change** (all binaries and outputs are bit-identical to v0.6.10)
**Branch:** `release/v0.6.11`

---

### Overview

v0.6.11 is a **tooling and developer-experience** release. It contains no
changes to the positioning engines, decoders, or numerical code — every binary
and every solution output is bit-identical to v0.6.10. It bundles three streams
of work merged to `develop` since the last release:

1. **Formatter CI gate + a one-time repo-wide format baseline** ([#166](https://github.com/h-shiono/MRTKLIB/issues/166))
2. **GSDC-2023 smartphone SPP benchmark** ([#165](https://github.com/h-shiono/MRTKLIB/issues/165))
3. **Faster, network-free regression CI** (#175)

---

### 1. Formatter CI gate + format baseline (#166)

The project already documented `clang-format` / `taplo` / `ruff` as the style
authorities, but enforcement was local-only and files had drifted. v0.6.11
establishes a reproducible baseline and gates it in CI.

**Repo-wide format sweep (PR #177)** — a one-time, **no-functional-change** sweep
applied the pinned formatters across the tree:

| Formatter | Version | Files |
|-----------|---------|------:|
| `clang-format` (Google / 4-space / 120-col) | 21.1.6 | 48 |
| `taplo fmt` | 0.10.0 | 3 |
| `ruff format` | 0.15.2 | 23 |

- Vendored / upstream-derived code is **excluded** via a new `.clang-format-ignore`
  (`src/core/tomlc99`, `util/`), so reformatting never pollutes upstream-sync diffs.
- The three sweep commits are recorded in **`.git-blame-ignore-revs`** so
  `git blame` skips them (GitHub honors this automatically; locally
  `git config blame.ignoreRevsFile .git-blame-ignore-revs`).

**Build-free CI gate (PR #179)** — a new lightweight workflow
[`.github/workflows/format.yaml`](https://github.com/h-shiono/MRTKLIB/blob/main/.github/workflows/format.yaml)
runs `clang-format --dry-run --Werror`, `taplo fmt --check`, and
`ruff format --check` on every push / PR. It is kept separate from the ~10-minute
regression job so style feedback is fast, and is **check-only** — it never pushes
formatting commits back (signing policy + fork-PR token constraints). Tool
versions are pinned (clang-format / ruff via pip wheels; the taplo release binary
is verified by SHA-256), and the same `.clang-format-ignore` exclude is reused, so
a local run matches CI exactly. `ruff check` (lint) is intentionally **not** part
of the gate.

`CONTRIBUTING.md` and `CLAUDE.md` now document all three formatters, their pinned
versions, config files, exclusions, and local fix commands.

### 2. GSDC-2023 smartphone SPP benchmark (#165)

A new benchmark harness evaluates SPP on Google's **GSDC-2023** smartphone
dataset — the environment where the SPP accuracy work in v0.6.10 (#116) is
actually meant to pay off (clock jumps, large jitter), unlike the geodetic
PPC set.

- **`scripts/benchmark/run_gsdc_benchmark.py`** — runs MRTKLIB SPP on GSDC-2023
  traces, with `scripts/benchmark/gsdc_to_rinex.py` converting `device_gnss.csv`
  to RINEX and `scripts/benchmark/download_brdc.py` fetching broadcast ephemeris.
- **`scripts/benchmark/compare_gsdc.py`** — scores `mrtk` NMEA output against the
  GSDC `ground_truth.csv`, reporting the **official GSDC score** (mean of the
  p50 and p95 horizontal errors) and a **coverage (`Cov%`) column**.
- **`conf/benchmark/gsdc_p0.toml`** and
  [`docs/reference/benchmark-gsdc.md`](../reference/benchmark-gsdc.md) document
  the workflow.
- A position-EKF (P6) re-attempt on this data was evaluated and **reverted** (no
  gain); the negative result is recorded in `docs/design/` for future reference.

### 3. Faster, network-free regression CI (#175)

- `ctest` now runs in parallel (`-j4`, matching the runner) with BLAS pinned to
  one thread per process for deterministic PPP-AR results; the timeout was raised
  to 20 minutes.
- The IONEX TEC fixture is **vendored** (`tests/cmake/download_tec.cmake`),
  removing a per-run network download, with an integrity check on the file.

---

### Tests

`ctest`: **77 / 79** on the maintainer's machine. The two failures —
`rtkrcv_rt` (a headless-terminal / RT file-replay environmental issue; excluded
from CI via `-LE realtime`) and `madocalib_pppar_ion_check` (a LAPACK-vs-reference
numerical-noise difference, ~1.6 cm vs a 0.5 cm tolerance) — **reproduce
identically on a clean `develop` build** and are pre-existing and unrelated to
this release. Because v0.6.11 changes no numerical code, all positioning outputs
remain bit-identical to v0.6.10.

The new `Formatter Check` workflow is green across the repository.

---

### Upgrade notes

- No configuration, API, or behavioural changes — drop-in for v0.6.10.
- Contributors: install the pinned formatters (see `CONTRIBUTING.md`) and,
  optionally, enable blame-ignore once with
  `git config blame.ignoreRevsFile .git-blame-ignore-revs`.
