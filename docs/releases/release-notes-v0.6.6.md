# Release Notes — v0.6.6

## CLI help unified across all `mrtk` subcommands; long-option aliases added

**Release date:** 2026-05-10
**Type:** Refactor — user-visible CLI surface, no behavior change in any positioning engine
**Branch:** `release/v0.6.6`

---

### Overview

v0.6.6 is a CLI-ergonomics release. Every `mrtk` subcommand now opens its
help with `mrtk <subcommand>:` instead of the legacy binary name
(`rtkrcv`, `rnx2rtkp`, `str2str`, `convbin`, `ssr2osr`, `recvbias`,
`dumpcssr`), and every help screen follows the same Usage / Options /
Examples layout.

GNU-style long aliases — `--config`, `--output`, `--start`, `--end`,
`--interval`, `--trace`, `--input`, `--nav`, `--device`, `--port`,
`--freq`, `--help` — are now available alongside the existing short
flags. All existing scripts continue to work unchanged.

No positioning-engine numerics are modified; the CTest suite passes
unchanged relative to the v0.6.5 baseline (61/63; same two pre-existing
failures, same numerical signatures).

---

### Major changes

#### Unified subcommand help format

Every `mrtk` subcommand now shows a help screen of the form:

```
mrtk <subcommand>: <one-line description>

Usage: mrtk <subcommand> [OPTIONS] [args...]

Options:
  -k, --config FILE       Configuration file (TOML)        [none]
  -o, --output FILE       Output file                      [stdout]
  ...
  -h, --help              Show this help

Examples:
  mrtk <subcommand> --config conf/foo.toml --output out.pos input.obs
```

Defaults are shown in `[bracket]` notation, argument placeholders use
`UPPER CASE`, and every help block ends with at least one `Examples:`
section. The legacy headers (`usage: rtkrcv`, `usage: rnx2rtkp`,
`Synopsis convbin`, `NAME: recvbias`, etc.) are gone.

#### GNU-style long-option aliases

The most-used short flags now have long aliases:

| Short | Long | Subcommands |
|:------|:-----|:------------|
| `-k` | `--config` | `post`, `ssr2obs`, `ssr2osr`, `dump`, `cssr2rtcm3` |
| `-o` | `--output` | `post`, `ssr2obs`, `ssr2osr`, `dump`, `convert`, `bias`, `l6extract`, and `--output` for `cssr2rtcm3` (`-out`) |
| `-ts` | `--start` | `post`, `ssr2obs`, `ssr2osr`, `dump`, `convert` |
| `-te` | `--end` | `post`, `ssr2obs`, `ssr2osr`, `dump`, `convert` |
| `-ti` | `--interval` | `post`, `ssr2obs`, `ssr2osr`, `convert` |
| `-x` | `--trace` | `post`, `ssr2obs`, `ssr2osr`, `dump` |
| `-t` | `--trace` | `run`, `relay`, `bias` |
| `-d` | `--trace` | `cssr2rtcm3` |
| `-trace` | `--trace` | `convert` |
| `-in` | `--input` | `relay`, `cssr2rtcm3`, `l6extract` |
| `-out` | `--output` | `relay`, `cssr2rtcm3` |
| `-nav` | `--nav` | `cssr2rtcm3`, `bias`, `convert` (`-n`/`--nav`) |
| `-d` | `--device` | `run` |
| `-p` | `--port` | `run` |
| `-f` | `--freq` | `post`, `convert` |
| `-h` | `--help` | All except `post` and `convert` (see below) |

Aliases are implemented as a pre-pass that swaps `argv[i]` pointers to
their existing short forms before per-subcommand parsing runs, so
existing scripts that use the short flags are bit-identical in behavior.

#### `-h` exceptions (backward compatibility)

Two subcommands intentionally do **not** rebind `-h` to help, because
`-h` already means something else there:

- **`mrtk post`** — `-h` is the **fix-and-hold integer ambiguity
  resolution** flag (inherited from upstream `rnx2rtkp`). Help is
  available via `--help` or the legacy `-?`.
- **`mrtk convert`** — `-h FILE` is the **HNAV output file** flag
  (inherited from upstream `convbin`). Help is available via `--help`.

Both exceptions are documented at the top of `docs/guide/cli.md` and
inside each subcommand's `--help` output.

#### Documentation refresh

- [`docs/guide/cli.md`](../guide/cli.md) — option tables now show both
  `-short` and `--long` forms; `-h` exceptions called out explicitly.
- [`docs/reference/rtkrcv-clas-realtime.md`](../reference/rtkrcv-clas-realtime.md)
  — retitled to "via `mrtk run`"; all in-text references to legacy
  `rtkrcv` / `rnx2rtkp` binaries updated to the unified-CLI form.

#### Implementation: shared CLI helper

Two new files:

- [`include/mrtklib/mrtk_cli.h`](../../include/mrtklib/mrtk_cli.h)
- [`src/core/mrtk_cli.c`](../../src/core/mrtk_cli.c)

Expose two small helpers:

- `mrtk_normalize_args(argc, argv, optmap)` — long→short alias pre-pass
  (pointer swap only; no string mutation).
- `mrtk_is_help_flag(arg)` — recognises `-h` / `--help`. The two
  exception subcommands match `--help` (and `-?` in `mrtk post`)
  manually instead.

#### Other

- [`scripts/analysis/compare_rtcm3.py`](../../scripts/analysis/compare_rtcm3.py)
  — RTCM3 byte-level diff utility preserved from the cssr2rtcm3
  development workflow (PR #103).

---

### Files Changed

| File | Change |
|------|--------|
| `apps/convbin/convbin.c` | Unified help, long aliases, `--help` (kept `-h` = HNAV file) |
| `apps/cssr2rtcm3/cssr2rtcm3.c` | Long aliases (header was already unified) |
| `apps/dumpcssr/dumpcssr.c` | Unified help, long aliases, `-h`/`--help` |
| `apps/l6extract/l6extract.c` | Long aliases (header was already unified) |
| `apps/recvbias/recvbias.c` | Unified help, long aliases, `-h`/`--help` |
| `apps/rnx2rtkp/rnx2rtkp.c` | Unified help, long aliases, `--help` and `-?` (kept `-h` = fix-hold AR) |
| `apps/rtkrcv/rtkrcv.c` | Unified help, long aliases, `-h`/`--help` |
| `apps/ssr2obs/ssr2obs.c` | Unified help, long aliases, `-h`/`--help` |
| `apps/ssr2osr/ssr2osr.c` | Unified help, long aliases, `-h`/`--help` |
| `apps/str2str/str2str.c` | Unified help, long aliases, `-h`/`--help` |
| `include/mrtklib/mrtk_cli.h` | New — shared CLI helpers |
| `src/core/mrtk_cli.c` | New — implementation |
| `CMakeLists.txt` | Add `mrtk_cli.c`; version 0.6.5 → 0.6.6 |
| `docs/guide/cli.md` | Both short and long forms; `-h` exceptions |
| `docs/reference/rtkrcv-clas-realtime.md` | "via `mrtk run`"; legacy binary refs updated |
| `scripts/analysis/compare_rtcm3.py` | New — RTCM3 byte-level diff (PR #103) |
| `CHANGELOG.md` | v0.6.6 entry |
| `mkdocs.yml` | v0.6.6 in Releases navigation |
| `README.md`, `CLAUDE.md` | v0.6.6 roadmap entry |

---

### Upgrade notes

- **All existing short flags continue to work** in every subcommand;
  this is verified by CTest. Scripts that use `-k`, `-o`, `-ts`, `-x`
  etc. need no changes.
- **`mrtk post -h`** still triggers fix-and-hold AR (not help) —
  unchanged from v0.6.5.
- **`mrtk convert -h FILE`** still sets the HNAV output path —
  unchanged from v0.6.5.
- **No positioning-engine numerics changed.** The CLI refactor does not
  touch any `.c` file under `src/{pos,clas,madoca,models,rtcm,stream}/`.

### Test Results

61/63 tests pass — identical to the develop baseline (`6fcb496`). The
two pre-existing failures (`rtkrcv_rt`, `madocalib_pppar_ion_check`)
reproduce on baseline with the same numerical signatures (3D RMS
0.016186 m on baseline vs 0.016324 m on this release, well within
numerical noise). Verified by re-running the same two tests in a
worktree at the pre-refactor commit. **Zero regressions.**

---

### PRs

- [#103](https://github.com/h-shiono/MRTKLIB/pull/103) —
  `chore: preserve scripts/analysis/compare_rtcm3.py used during cssr2rtcm3 dev`
- [#104](https://github.com/h-shiono/MRTKLIB/pull/104) —
  `refactor(cli): unify subcommand help format and add long-option aliases`

### Closes

- [#77](https://github.com/h-shiono/MRTKLIB/issues/77) —
  `refactor: unify CLI help output format across all subcommands`
