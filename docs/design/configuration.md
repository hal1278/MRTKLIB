# MRTKLIB — Positioning Configuration Model

> **Status:** Proposed (design) · **Tracking:** [#130](https://github.com/h-shiono/MRTKLIB/issues/130) · **Phase:** 1
>
> This document defines how MRTKLIB *should* express a positioning run in
> configuration, and why. It is a design rationale for maintainers and
> contributors — MRTKLIB is a reference implementation, so the *why* is part
> of the deliverable. Where the current code diverges from this model, the
> divergence and its migration path are stated explicitly.

---

## 1. Background

Historically, the `mode` key (RTKLIB heritage, inherited through MALIB /
MADOCALIB / CLASLIB) encodes several independent decisions at once. As a
result there is no clean way to express combinations that are perfectly valid
in principle — most concretely, **"run the PPP engine, fed by IGS precise
products."** Attempting conventional IGS-product PPP today produces an empty
solution file; the root cause and full investigation are in
[#130](https://github.com/h-shiono/MRTKLIB/issues/130).

The fix is not a one-off patch but a small clarification of the configuration
model: separate the *correction source* from the *positioning mode*.

## 2. The four axes

A positioning run is the product of four mostly-orthogonal choices. Naming
them explicitly is the core of this design.

| Axis | Question it answers | Value domain |
|------|--------------------|--------------|
| **engine** | Which estimator? | `single` · `dgps` · `rtk` · `ppp` · `ppp-rtk` · `vrs-rtk` |
| **dynamics** | How does the receiver move? | `static` · `kinematic` · `fixed` |
| **armode** | Integer ambiguity resolution? | `off` · `continuous` · `instantaneous` |
| **correction** | Where do corrections come from? | `none` · `igs` · `igs-rts` · `qzs-madoca` · `gal-has` · `bds-b2b` · `qzs-clas` |

The **engine** is the genuine dispatch decision — each value maps to a
distinct estimator in the code (`pntpos` / `relpos` / `pppos` /
`ppp_rtk_pos` / `relposvrs`). The other three axes parameterize the chosen
engine; they do not change which estimator runs.

### 2.1 Orthogonality, and the one real coupling

`dynamics` and `armode` are orthogonal to everything: any engine can run
static or kinematic, with or without AR (subject to whether the engine
implements AR at all).

`correction` is *almost* orthogonal to `engine`, with one structural
coupling: **a correction source implies a compatible engine class.**

| `correction` | implied engine class | content delivered |
|--------------|---------------------|-------------------|
| `none` | `single` / `dgps` / `rtk` | broadcast only, or a real base station |
| `igs` | `ppp` | precise orbit/clock/bias **files** (SP3 / CLK / Bias-SINEX / DCB) |
| `igs-rts` | `ppp` | IGS Real-Time Service SSR as RTCM-SSR over NTRIP (e.g. `SSRC00CAS0`) |
| `qzs-madoca` | `ppp` | MADOCA-PPP SSR via QZSS L6E or RTCM-SSR |
| `gal-has` | `ppp` | Galileo HAS via E6-B SSR |
| `bds-b2b` | `ppp` | BeiDou PPP-B2b via B2b-signal SSR (orbit/clock/code bias) |
| `qzs-clas` | `ppp-rtk` / `vrs-rtk` | QZSS CLAS L6D CSSR (orbit/clock/bias **+ gridded atmosphere**); consumed undifferenced (`ppp-rtk`) or as CLAS-derived OSR by double-differencing (`vrs-rtk`) |

This is why `mode` and `correction` were conflated in the first place: the
source nearly determines the engine. The design keeps both axes explicit
anyway — making the engine implicit in `correction` reintroduces ambiguity
(`none` could mean SPP *or* RTK) and hides a real dispatch decision, which is
unacceptable for a reference implementation.

### 2.2 Mapping the axes onto today's `mode`

The current `mode` enum fuses **engine** and **dynamics** — including the
position-fixed value `PMODE_PPP_FIXED`, which constrains the receiver to known
coordinates (dynamics = `fixed`), *not* an AR setting. Concretely, the literal
`mode` values combine the two axes: the `ppp` engine appears as `ppp-static` /
`ppp-kine` / `ppp-fixed`, the `rtk` engine as `static` / `kinematic` / `fixed`
/ `movingbase` (no `rtk-` prefix), while `ppp-rtk` and `vrs-rtk` are currently
kinematic-only. **armode** is already a separate key
(`[ambiguity_resolution].mode`); it is not part of `mode`.
**correction** is the only axis left implicit (inferred from the input stream
format). This document pulls `correction` out as its own key; the `dynamics`
double-encoding and the missing `ppp-rtk` / `vrs-rtk` dynamics variants are
addressed in §6.3.

### 2.3 Naming convention for `correction` values

Values follow a deliberate rule so the namespace stays unambiguous as more
sources are added:

- **`<system>-<service>`** when a single constellation operates and broadcasts
  the augmentation — `qzs-madoca`, `qzs-clas`, `gal-has`, `bds-b2b`. The prefix
  is the **broadcasting / operating system**, not the constellations being
  corrected (MADOCA and CLAS correct multiple GNSS but are broadcast by QZSS).
- **bare name** for cross-constellation products with no single operating
  constellation — `igs`, `igs-rts`.

The prefix matters because service *names* are not unique: "HAS" (High
Accuracy Service) is a generic label other systems may reuse, so `gal-has`
pins the provider. `none` is the absence of an augmentation source.

## 3. Configuration surface

```toml
[positioning]
mode       = "ppp-static"   # engine + dynamics (unchanged in Phase 1)
correction = "igs"          # NEW: correction source (case-insensitive)
```

- `correction` is the **single source of truth** for the correction source.
  It selects, in one place: (a) which decoder/loader runs, (b) the bias
  branch inside `corr_meas` (§5), and (c) the default value of
  `satellite_ephemeris`.
- The value is **case-insensitive** (`"IGS"`, `"igs"`, `"Igs"` all accepted),
  consistent with the existing `systems` / `excluded_sats` handling.
- `correction` is **optional but recommended**. Setting it explicitly is the
  canonical form; when omitted, it is inferred for backward compatibility
  (§6.1), and that inference is a compatibility shim, not the intended way to
  write a new config.

## 4. Validity matrix

Each `mode` engine accepts a subset of `correction` values. An invalid
combination is a **hard error at load time** (fail fast, with a message that
names both values and lists the accepted set) — it is never silently
"corrected" to something else.

The left column uses the **current literal `mode` values** (Phase 1 leaves
`mode` unchanged): PPP modes carry a `ppp-` prefix, while the RTK / relative
modes are bare words (no `rtk-` prefix).

| `mode` value | accepted `correction` |
|--------------|-----------------------|
| `ppp-static` / `ppp-kine` / `ppp-fixed` | `igs`, `qzs-madoca`, `gal-has`†, `bds-b2b`†, `igs-rts`† |
| `ppp-rtk` | `qzs-clas` |
| `vrs-rtk` | `qzs-clas` |
| `kinematic` / `static` / `fixed` / `movingbase` | `none` |
| `dgps` | `none` |
| `single` | `none` |

† Reserved value — accepted by the matrix but rejected with an explicit
"not implemented" error until the corresponding decoder lands (§7).

Dynamics (static / kinematic / fixed) is independent of `correction`: all three
accept the same sources. The fixed variant (`ppp-fixed`) constrains the receiver
to known coordinates and removes position from the estimator — valuable for
residual analysis (so e.g. `ppp-fixed × igs` is fully valid). This is **not**
integer
ambiguity resolution; AR is the separate `armode` setting. IGS *integer*
PPP-AR (`armode` ≠ off with `igs`) additionally needs a phase-bias product and
is deferred to Phase 2 (§5, §6.3); Phase-1 `igs` is float.

`ppp-rtk` and `vrs-rtk` are **two engines for the same source** (`qzs-clas`):
`ppp-rtk` (`ppp_rtk_pos`) positions undifferenced; `vrs-rtk` (`relposvrs`,
ported from CLASLIB `rtkvrs.c`) converts the CLAS corrections to virtual-
reference OSR — via `ssr2osr` / `cssr2rtcm3` — and positions by
double-differencing. MRTKLIB's `vrs-rtk` is therefore **CLAS-specific**, not a
generic network-VRS client. A commercial network VRS (RTCM-OSR streamed from a
caster) is *not* this engine: it is the standard RTK / relative engine with
that stream as its base, i.e. `mode = kinematic` (or `static` / `fixed`),
`correction = none`.

## 5. `corr_meas` branch specification (minimal)

`corr_meas()` ([`src/pos/mrtk_ppp.c`](https://github.com/h-shiono/MRTKLIB/blob/main/src/pos/mrtk_ppp.c)) is the
bias-correction chokepoint **of the `ppp` engine** (`pppos`): it turns raw
observations into bias-corrected phase/code, handling **satellite-side bias
only** (phase / code). It is `static` to `mrtk_ppp.c` — the CLAS engines
`ppp-rtk` (`ppp_rtk_pos`) and `vrs-rtk` (`relposvrs`) have their own
measurement models (and their own atmosphere-grid / OSR handling) and do **not**
route through it. This spec therefore covers only the `ppp`-engine sources.

Today `corr_meas` has exactly one behavior (SSR-mandatory), which is why the
IGS path fails. Phase 1 introduces a branch on `correction`:

**SSR path** — `correction ∈ { qzs-madoca, gal-has, bds-b2b, igs-rts }`
: Current behavior, unchanged. Satellite code/phase bias is read from
  `nav->ssr_ch` (filled by the L6E / RTCM-SSR decoders); an observation lacking
  a valid SSR bias (`vcbias` / `vpbias`) is dropped. This is required for PPP-AR
  and is the MADOCALIB design. Note `igs-rts` (RTCM-SSR stream) takes this path,
  whereas `igs` (precise files) takes the IGS-files path below — the two IGS
  values are deliberately distinct (§7).

**IGS-files path** — `correction = igs`
: RTKLIB-style float PPP, ported from
  [`upstream/RTKLIB/src/ppp.c`](https://github.com/h-shiono/MRTKLIB/blob/main/upstream/RTKLIB/src/ppp.c):

    - **Never discard** an observation for missing bias.
    - Apply receiver/satellite antenna PCV and phase windup (as today).
    - Apply P1-C1 / P2-C2 DCB from `nav->cbias` **if present**, otherwise
      leave the code measurement as-is (the precise clock already absorbs the
      ionosphere-free code bias convention).
    - Do **not** apply or require a satellite phase bias; the **float phase
      ambiguity** state (already present in `pppos`) absorbs it. Integer PPP-AR
      with `igs` (i.e. `armode` ≠ off) needs a phase-bias product and the
      `nav->osb` wiring — deferred to Phase 2 (§6.3); Phase-1 `igs` is float.
      Dynamics static / kinematic / fixed all work — `ppp-fixed` is the
      known-coordinate residual-analysis case.

**No-correction path** — `correction = none`
: No satellite bias applied. Used by `single` / `dgps` / the RTK relative modes (these engines
  do not route through `corr_meas`, but the branch is defined for completeness
  and to make the matrix total).

Notes:

- The float-ambiguity machinery, EKF state layout, and `udbias_ppp` /
  `udiono_ppp` seeding in `pppos` are unchanged — the IGS path only stops the
  premature zeroing. No new engine is introduced.
- `nav->osb` (the unified OSB/DCB/FCB store populated by `udbiass`) remains
  consumed only by the `recvbias` utility in Phase 1. Wiring `nav->osb` into
  `corr_meas` as a unified bias source is a larger change, deliberately out of
  scope (§6).

## 6. Backward compatibility and migration

### 6.1 `correction` omitted → inferred

Existing TOML configs do not set `correction`. When the key is absent, the
source is inferred so those configs keep working unchanged:

| Inferred from | `correction` |
|---------------|--------------|
| input stream format `STRFMT_L6E` | `qzs-madoca` |
| input stream format `STRFMT_CLAS`, or `mode` is `ppp-rtk` / `vrs-rtk` | `qzs-clas` |
| `mode` is `single` / `dgps` / `kinematic` / `static` / `fixed` / `movingbase` | `none` |
| otherwise (PPP mode, no SSR stream) | `igs` |

The inference is a compatibility shim, not the recommended form. New configs
should set `correction` explicitly.

### 6.2 OSB / Bias-SINEX file routing (existing footgun)

Independent of this design, note the current input-routing trap that this
document's IGS path depends on getting right:

- `[files] dcb` → `file-dcbfile` → `readdcb` (legacy **CODE DCB** text format)
- `[files] bias_sinex` → `file-biafile` → `readbsnx` (**Bias-SINEX / OSB**)

An OSB Bias-SINEX file (e.g. `COD…OSB.BIA`) passed to `dcb=` is silently
ignored by the DCB parser. The IGS-files loader must consume the
Bias-SINEX/DCB products through the correct readers and route them into the
`nav` fields the IGS `corr_meas` path reads (`nav->cbias`, and—if/when
unified—`nav->osb`).

### 6.3 Dynamics axis: cleanup and symmetry (Phase 2)

**Not touched in Phase 1** (decision (a)); recorded here so the migration is
designed, not discovered.

First, a clarification that scopes these items: **position-fixed mode
(`*-fixed`) is the third value of the dynamics axis, not ambiguity
resolution.** `PMODE_PPP_FIXED` ([`mrtk_ppp.c`](https://github.com/h-shiono/MRTKLIB/blob/main/src/pos/mrtk_ppp.c),
`udpos_ppp`) constrains the receiver position to the known coordinates
`opt.ru` with near-zero variance and removes it from the estimator. It is a
first-class research feature (residual analysis at known stations: correction-
quality assessment, bias identification, atmosphere-model validation,
integrity reference runs) and is **not** deprecated. AR is the independent
`armode` setting.

Phase-2 items:

1. **`dynamics` double-encoding.** The dynamics value baked into `mode`
   (`ppp-static` / `ppp-kine` / `ppp-fixed`; bare `static` / `kinematic` /
   `fixed` for RTK) and the separate `dynamics` boolean both encode the
   dynamics axis (the boolean adds velocity/acceleration states). Consolidate
   to one representation — likely keep the `mode` value and derive the boolean
   — for RTKLIB compatibility.

2. **`ppp-rtk` / `vrs-rtk` dynamics symmetry.** The CLAS engines are currently
   kinematic-only, while the PPP engine exposes static/kinematic/fixed. Extend
   them to the full dynamics axis — i.e. add `PMODE_PPPRTK_FIXED` (and the
   static variant). `ppp-rtk-fixed` (`engine = ppp-rtk`, `dynamics = fixed`,
   `correction = qzs-clas`) lets CLAS SIS and gridded-atmosphere quality be
   observed directly at a known-coordinate station, separated from position
   error — the PPP-RTK analogue of `PMODE_PPP_FIXED`. This restores
   orthogonality (dynamics independent of engine), which the current
   kinematic-only CLAS engines break.

3. **IGS PPP-AR.** Wire `nav->osb` into the IGS path so integer AR works with
   `igs` products (a phase-consistent Bias-SINEX, e.g. CNES GRG or CODE). This
   is `armode`-driven and orthogonal to the `-fixed` dynamics value.

## 7. Reserved-but-unimplemented values

`gal-has`, `bds-b2b`, and `igs-rts` are **reserved**: accepted by the validity
matrix and the enum, but rejected at load time with an explicit "not
implemented" message. The reservation policy is deliberately narrow — **only
sources we intend to support are reserved**, and all three are open, published
specifications. No commercial brand names appear as `correction` values; a
brand (e.g. PointPerfect) is a *delivery service* for a format (SPARTN), not a
dispatch key.

- **`gal-has`** (Galileo HAS, E6-B SSR) and **`bds-b2b`** (BeiDou PPP-B2b,
  B2b-signal SSR) are both open global SSR services. They fit the existing
  `ppp` engine once a decoder fills the same internal SSR structures — no new
  engine expected. Names follow the `<system>-<service>` convention (§2.3).
- **`igs-rts`** (IGS Real-Time Service) is an open, real-time **RTCM-SSR**
  *stream* (e.g. `SSRC00CAS0`); it routes through the **SSR path** (§5), like
  `qzs-madoca`. It is deliberately distinct from `igs`, which is post-processing
  precise *files* on the IGS-files path. Keeping them separate avoids overloading
  one value with two unrelated transport mechanisms.

## 8. Phasing

**Phase 1 (this design's deliverable):**

- Add the `correction` TOML key and enum. Implemented: `none`, `igs`,
  `qzs-madoca`, `qzs-clas`. Reserved: `gal-has`, `bds-b2b`, `igs-rts`.
- Branch `corr_meas` (SSR / IGS-files / none) per §5.
- IGS-files loader: SP3 / CLK / Bias-SINEX / DCB into the `nav` fields the
  IGS path reads.
- Validity-matrix check at load time (§4), hard error on invalid combos.
- Backward-compat inference when `correction` is omitted (§6.1).

**Phase 2 and later (out of scope here):**

- `dynamics` double-encoding cleanup (§6.3).
- `ppp-rtk` / `vrs-rtk` dynamics symmetry, incl. `PMODE_PPPRTK_FIXED` (§6.3).
- `igs-rts` (real-time IGS SSR stream).
- `gal-has` / `bds-b2b` decoder implementations.
- `nav->osb` wiring into the IGS path → integer IGS PPP-AR (`armode` with
  `igs`; needs a phase-bias product) (§6.3).

## 9. Open questions

- `dynamics` consolidation (§6.3): keep the `mode` suffix as the source of
  truth and derive the `dynamics` boolean, or invert? — Phase 2.
- Whether `armode` (already `[ambiguity_resolution].mode`) should be lifted to
  a top-level positioning key for parity with the other three axes — cosmetic,
  Phase 2.

---

This document is published under the **Design** section of the MkDocs site;
future architecture / roadmap design notes are collected there alongside it.
