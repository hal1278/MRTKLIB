# Release Notes — v0.6.9

## IGS-product integer PPP-AR via Bias-SINEX OSB phase biases

**Release date:** 2026-05-24
**Type:** Feature — integer ambiguity resolution for `correction = "igs"` (post-processing)
**Branch:** `release/v0.6.9`

---

### Overview

v0.6.9 extends the **`correction = "igs"`** axis from **float-only** PPP
(v0.6.7) to **integer PPP-AR**, using satellite **phase biases** from a
Bias-SINEX Observable-Specific Bias (OSB) product — e.g. the CODE MGEX IAR
`OSB.BIA`. Integer resolution gives faster convergence and cm-level fixed
solutions.

This is the "Phase 2" item of the `correction` axis
([`docs/design/configuration.md`](../design/configuration.md)), following the
v0.6.7 IGS-files float PPP (#130/#135/#137) and v0.6.8 IGS-RTS float PPP (#138).

---

### Major changes

#### Integer PPP-AR for `correction = "igs"` (#142)

Integer resolution from IGS precise products needs the **uncombined**
measurement model plus a satellite **phase-bias** product:

- `corr_meas()` ([`src/pos/mrtk_ppp.c`](https://github.com/h-shiono/MRTKLIB/blob/main/src/pos/mrtk_ppp.c)),
  when uncombined (`ionosphere = "est-stec"`), applies the per-signal satellite
  **code bias to the pseudorange** and **phase bias to the carrier** from
  `nav->osb` (the Bias-SINEX OSB). Removing the satellite phase bias makes the
  float ambiguity integer-recoverable, so `ppp_amb_ILS` can fix the
  wide-/narrow-lane. It is **gated on uncombined mode**, so the float iono-free
  path (`ionosphere = "dual-freq"`) stays **bit-identical** — float results are
  unchanged with AR off (before/after guard).
- `udsatpb()` ([`src/pos/mrtk_rtkpos.c`](https://github.com/h-shiono/MRTKLIB/blob/main/src/pos/mrtk_rtkpos.c))
  gains a **Bias-SINEX phase-bias source** (`pppsatpb` enum `2:bia`). Previously
  only SSR/FCB fed `nav->osb.spb`, so file-based OSB phase biases were loaded
  then dropped. The new source is reached **only when no SSR/FCB phase bias
  exists**, so MADOCA / CLAS / VRS are unaffected.
- `resolve_correction()` requires `ionosphere = "est-stec"` when
  `correction = "igs"` is combined with ambiguity resolution — the iono-free
  combination has a single ambiguity per satellite, so AR would otherwise be a
  silent no-op.

#### PPP-AR zero-ambiguity guard (dual-frequency)

The extra-wide-lane step called the Kalman update with `na = 0` on a
dual-frequency setup, invoking LAPACK `DGETRF` with `n = 0`
(`XERBLA "parameter 4 illegal"`) and **aborting the whole AR** before the
wide-/narrow-lane steps. `update_states()`
([`src/pos/mrtk_ppp_ar.c`](https://github.com/h-shiono/MRTKLIB/blob/main/src/pos/mrtk_ppp_ar.c))
now treats `na <= 0` as a no-op. This is a latent bug that affected any
dual-frequency PPP-AR; multi-frequency AR (MADOCA / CLAS, `nf ≥ 3`) is
unaffected (bit-identical).

#### Validation

- **GEONET FUJISAWA** (GSI 93034, Trimble geodetic), 2026-01-21, GPS+Galileo,
  uncombined PPP-AR with COD MGEX `OSB.BIA`, against the GSI F5 coordinate:
  **~31 % integer-fix** over a 2 h window (**100 % once converged**), 3D ≈ 6.7 cm
  (dominated by the data's ~7 cm Up bias, which the float carries too).
- The IGS **float** PPP is cross-checked against **upstream RTKLIB 2.4.3**
  `rnx2rtkp` on identical inputs: both ≈ **8.4 cm 3D** vs the GSI coordinate —
  i.e. no meaningful degradation. (RTKLIB has no integer PPP-AR — its `ppp_ar()`
  is a stub — so the fixed solution has no upstream counterpart.)
- New `igs_pppar_ge` (GPS+GAL, asserts the integer-fix rate via
  `compare_pos_abs.py --min-fix-rate`) and `igs_pppar_gps` (GPS-only) regression
  groups.

#### Constellations

The AR engine is multi-GNSS (GPS / Galileo / QZSS / BeiDou via
`[ambiguity_resolution].systems`; GLONASS is float-only). With the COD MGEX OSB,
**GPS + Galileo is the dependable combination**: that product's QZSS phase
biases are L1/L5 only (no L2, so the `l1+2` pair can't fix QZSS) and it carries
no BeiDou phase biases. See [`docs/guide/configuration.md`](../guide/configuration.md).

#### IGS-RTS quick-start guide (#148)

Adds [`docs/guide/quickstart-igs-rts.md`](../guide/quickstart-igs-rts.md) and a
real-time NTRIP sample `conf/igs/rtkrcv_igsrts.toml` for the v0.6.8 IGS-RTS
float PPP feature.

---

### Files Changed

| File | Change |
|------|--------|
| `src/pos/mrtk_ppp.c` | apply OSB per-signal code/phase bias in `corr_meas` for uncombined `igs` |
| `src/pos/mrtk_rtkpos.c` | Bias-SINEX phase-bias source in `udsatpb` (+ stale-bias clearing) |
| `src/pos/mrtk_ppp_ar.c` | guard zero-ambiguity Kalman update (DGETRF `n=0`) |
| `src/pos/mrtk_opt.c`, `src/pos/mrtk_options.c`, `include/mrtklib/mrtk_opt.h` | `igs`+AR validity; `pppsatpb` `2:bia` |
| `conf/igs/rnx2rtkp_igspppar.toml`, `conf/igs/rnx2rtkp_igspppar_gps.toml` | PPP-AR sample/test configs |
| `tests/data/igs/igs_testdata.tar.gz`, `CMakeLists.txt` | trimmed OSB + `igs_pppar_gps` / `igs_pppar_ge` regression tests |
| `scripts/tests/compare_pos_abs.py` | `--min-fix-rate` integer-fix-rate assertion |
| `docs/guide/configuration.md` | IGS PPP-AR requirements + RTKLIB float cross-validation note |
| `docs/guide/quickstart-igs-rts.md`, `conf/igs/rtkrcv_igsrts.toml` | IGS-RTS quick-start guide (#148) |

---

### Upgrade notes

- **Existing configs need no change.** Float `correction = "igs"`
  (`ionosphere = "dual-freq"`) is unchanged and bit-identical.
- To run integer PPP-AR, use the uncombined model and supply an OSB phase-bias
  product:
  ```toml
  [positioning]
  correction = "igs"
  [positioning.atmosphere]
  ionosphere = "est-stec"      # uncombined — required for AR
  [ambiguity_resolution]
  mode = "continuous"
  [files]
  bias_sinex = "COD0MGXFIN_<yyyyddd>0000_01D_01D_OSB.BIA"   # NOT dcb=
  ```
  See `conf/igs/rnx2rtkp_igspppar.toml`. GPS-only often has too few satellites
  for reliable narrow-lane fixing on short sessions (the wide-lane still fixes);
  GPS + Galileo is recommended.
- **Real-time / live-stream** integer PPP-AR (`correction = "igs-rts"` with CNES
  SSR phase biases, MT1265-1267) is a tracked follow-up
  ([#151](https://github.com/h-shiono/MRTKLIB/issues/151)).

### Test Results

The `igs_pppar_gps` / `igs_pppar_ge` groups pass, along with the existing IGS
float and MADOCA / CLAS regression tests (bit-identical where guarded). The only
failures are the two known pre-existing ones on the maintainer host —
`rtkrcv_rt` (real-time timing) and `madocalib_pppar_ion_check` (LAPACK vs
embedded-LU numerical difference, CLAUDE.md §7.2) — both unrelated to this
release.

---

### PRs

- [#150](https://github.com/h-shiono/MRTKLIB/pull/150) — `feat(ppp): IGS-product integer PPP-AR via OSB phase biases (#142)`
- [#148](https://github.com/h-shiono/MRTKLIB/pull/148) — IGS-RTS PPP quick-start guide

### Closes

- [#142](https://github.com/h-shiono/MRTKLIB/issues/142) — `feat(ppp): IGS-product PPP-AR (integer ambiguity resolution with OSB phase biases)`
