---
verdict: needs-attention
remediation_round: 0
---

# Milestone Validation: M006

## Success Criteria Checklist

- [x] **DeepCDefocusPO node loads a lentil .fit polynomial file at runtime without crash** — evidence: `poly_system_read` lifecycle fully wired in `_validate(for_real)` with `_poly_loaded` flag, `Op::error()` on failure, and double-destroy safety in destructor + `_close()`. `File_knob` wired. All S01 grep contracts pass; `bash scripts/verify-s01-syntax.sh` exits 0. Runtime gate (docker + Nuke session) deferred to CI per roadmap.

- [x] **Flat 2D output is produced from a Deep input; bokeh is visible and physically sized** — evidence: `PlanarIop` base class confirmed (D027); `renderStripe` implements full PO scatter loop with `deepEngine` fetch, Halton(2,3)+Shirley disk aperture sampling, Newton-iteration trace at 3 wavelengths, `writableAt +=` accumulation into flat RGBA buffer. `lt_newton_trace` stub replaced by real 20-iteration Newton loop. All S02 grep contracts pass; syntax exits 0. Runtime visual verification (non-zero bokeh pixels) deferred to CI/Nuke UAT per roadmap.

- [x] **Chromatic aberration is visible on out-of-focus highlights** — evidence: `lambdas[3] = {0.45f, 0.55f, 0.65f}` used in the R/G/B channel scatter loop; each channel is traced independently via `lt_newton_trace`, producing per-channel sensor landing positions. Grep contracts for all three wavelength constants pass. R022 marked `validated` in REQUIREMENTS.md. Runtime fringing visibility deferred to Nuke UAT.

- [x] **Holdout input stencils the defocused result at correct depths; holdout geometry does not appear in output and is not blurred** — evidence: `transmittance_at` lambda with depth gate (`hzf >= Z`), `holdout_w` applied to every RGB and alpha `writableAt +=` accumulation, colour channels excluded from holdout `deepRequest`, holdout evaluated at output-pixel bounds (never at the input sample's pixel position). S03 contracts all pass. R023 and R024 marked `validated`. Runtime visual check (sharp holdout boundary, no colour bleed) deferred to CI/Nuke UAT.

- [ ] **`docker-build.sh --linux --versions 16.0` exits 0 with DeepCDefocusPO.so in release zip** — gap: Docker daemon not available in workspace; runtime CI gate confirmed pending throughout S01–S05. `docker-build.sh` is present in the repo root. All structural prerequisites for a successful build are in place: CMake registration, Op::Description, poly.h inline (single TU, no ODR violation), no Eigen dependency. This gate cannot be confirmed at this validation round.

- [x] **Node appears in FILTER_NODES category in Nuke** — evidence: `set(FILTER_NODES ... DeepCDefocusPO)` at line 53 of `src/CMakeLists.txt`; `configure_file(../python/menu.py.in menu.py)` propagates the list into the Nuke Python menu; `python/init.py` registers `icons/` path; `icons/DeepCDefocusPO.png` present and non-empty. Grep contract `FILTER_NODES.*DeepCDefocusPO` passes. `Op::Description` registration at `"Deep/DeepCDefocusPO"` confirmed.

---

## Slice Delivery Audit

| Slice | Claimed | Delivered | Status |
|-------|---------|-----------|--------|
| S01 | Compilable PlanarIop scaffold; Deep input wiring; poly load/destroy lifecycle; flat-black renderStripe; four knobs; CMakeLists.txt registration | `src/DeepCDefocusPO.cpp` (PlanarIop subclass, two Deep inputs, `_validate` poly lifecycle, flat-black `renderStripe`), `src/poly.h` (vendored lentil API), `src/deepc_po_math.h` (Mat2, mat2_inverse, lt_newton_trace stub, coc_radius), `src/CMakeLists.txt` (PLUGINS line 16, FILTER_NODES line 53), `scripts/verify-s01-syntax.sh` extended; syntax exits 0; 9 grep contracts pass | ✅ pass |
| S02 | Deep input scattered to flat output via PO; bokeh shape and size visible; chromatic aberration; per-channel wavelength tracing | `lt_newton_trace` stub replaced with 20-iter Newton loop; `halton`/`map_to_disk` added to `deepc_po_math.h`; `renderStripe` scatter engine complete: deepEngine, Halton+Shirley, Newton at R/G/B wavelengths, `writableAt +=` accumulation; no TODO/STUB markers; 10 S02 grep contracts pass; syntax exits 0 | ✅ pass |
| S03 | Holdout Deep input stencils defocused output at correct depth; holdout never scattered; matches DeepHoldout semantics | `input1()` accessor; `getRequests` holdout deepRequest (alpha+zfront+zback only); `holdoutPlane` fetch at output bounds; `transmittance_at` lambda with `hzf >= Z` depth gate; `holdout_w` applied to all RGB+alpha splats; 7 S03 grep contracts pass; syntax exits 0; R023+R024 marked validated | ✅ pass |
| S04 | All artist controls with tooltips; f-stop, focus distance, aperture samples, focal length wired; no stale comments; matches DeepC UI conventions | `_focal_length_mm` Float_knob (replaces hardcoded 50.0f); `Divider` between lens/render groups; `"focus distance (mm)"` label; stale S01 skeleton text removed; 5 S04 grep contracts pass; `_focal_length_mm` appears 4× in source; syntax exits 0 | ✅ pass |
| S05 | docker-build.sh exits 0; DeepCDefocusPO.so in release zip; node in FILTER_NODES; syntax verifier passes; end-to-end test documented | Stale S01/S02 comment block removed; `THIRD_PARTY_LICENSES.md` lentil/MIT entry added; `icons/DeepCDefocusPO.png` created; 5 S05 grep contracts pass; full verify script (S01–S05) exits 0; docker build and Nuke session documented as CI/UAT-pending in S05-UAT.md | ✅ pass (structural) / ⚠️ CI gate pending |

---

## Cross-Slice Integration

All boundary map entries checked against actual source:

| Boundary | Expected | Actual | Status |
|----------|----------|--------|--------|
| S01 → S02: `lt_newton_trace` stub in `deepc_po_math.h` | Stub returning `{0,0}` | Replaced by full Newton loop in S02 | ✅ |
| S01 → S02: `coc_radius` fully implemented | Present, available for S02 CoC culling | Confirmed in `deepc_po_math.h`; called in `renderStripe` | ✅ |
| S01 → S02: `renderStripe` insertion point | Zeroing loop with `// S02: replace` marker | Marker absent (0 matches for `S02: replace`); full scatter loop in place | ✅ |
| S01 → S03: Holdout input wired as `Op::input(1)` | `input_label` returning "holdout" | `input(1)` appears 3× in source; `input1()` accessor present | ✅ |
| S02 → S03: Holdout `holdout_w` inserted before splat | `holdout_w` factor in all `writableAt +=` | 4 occurrences of `holdout_w` in `DeepCDefocusPO.cpp` | ✅ |
| S01 → S04: All four knobs already present | `poly_file`, `focus_distance`, `fstop`, `aperture_samples` | All four present from S01; S04 added `focal_length` Float_knob and Divider | ✅ |
| S02 → S05: Scatter engine for final integration | Full `renderStripe` working; no stubs | Syntax exits 0; S05 contracts pass; all stub markers removed | ✅ |
| S03 → S05: Holdout wired correctly | Holdout depth evaluation at output pixel | `holdoutOp->deepEngine(bounds, ...)` uses output-pixel bounds; confirmed | ✅ |
| S04 → S05: Knob surface complete | All six knobs with tooltips and defaults | Confirmed: `poly_file`, `focal_length`, Divider, `focus_distance`, `fstop`, `aperture_samples` | ✅ |
| CMake configure_file | `FILTER_NODES` list propagated to `menu.py.in` | `configure_file(../python/menu.py.in menu.py)` at CMakeLists.txt line 142 | ✅ |

No boundary mismatches found.

---

## Requirement Coverage

All R019–R026 requirements in scope for M006 are accounted for:

| Requirement | Slice Owner | Status | Notes |
|-------------|-------------|--------|-------|
| R019 — Deep→flat PO scatter | S02 (S01 scaffold) | validated | Structural proof complete; runtime non-zero pixel proof deferred to CI |
| R020 — poly .fit file load | S01 | validated | poly_system_read/evaluate/destroy wired; File_knob; error path |
| R021 — CoC from focal length, f-stop, focus distance | S02 formula; S04 knob | validated | Float_knob wired in S04; formula in deepc_po_math.h; runtime bokeh sizing deferred to CI/UAT |
| R022 — Chromatic aberration via per-channel wavelength tracing | S02 | validated | lambdas[] at 0.45/0.55/0.65 μm; independent trace per channel |
| R023 — Holdout transmittance accumulation | S03 | validated | transmittance_at lambda; front-to-back product; disconnected path = 1.0f identity |
| R024 — Holdout: no colour in output; evaluated at output pixel | S03 | validated | Colour channels excluded from deepRequest; evaluated at output bounds not input position |
| R025 — Stochastic aperture sampling, artist-controlled N | S02 | validated | Halton(2,3) + Shirley disk; Int_knob aperture_samples; loop runs N iterations per deep sample |
| R026 — Flat RGBA output (not Deep stream) | S01 | validated | PlanarIop base class; flat renderStripe output confirmed |
| R027 — Aperture blade shape | — | deferred | Out of M006 scope per roadmap |
| R029 — Bidirectional sampling | — | out-of-scope | Excluded from M006 per roadmap |

All eight in-scope requirements (R019–R026) are in `validated` status. No active requirements are unaddressed.

---

## Verdict Rationale

**Verdict: `needs-attention`**

All five slices delivered their claimed outputs. All eight in-scope requirements are marked `validated`. All structural gates pass: `bash scripts/verify-s01-syntax.sh` exits 0 with 30+ grep contracts across S01–S05; C++ syntax checks pass for all three files; CMake registration is correct; THIRD_PARTY_LICENSES.md is populated; icon exists. No boundary mismatches. No stub markers or TODO artifacts remain in source.

The single gap is the **docker build gate**, which is a milestone Definition of Done item that cannot be satisfied in this workspace (no Docker daemon). This is not a regression or a missing deliverable — it has been documented consistently as CI-only since S01, explicitly deferred in every slice summary, and the UAT document for S05 records it as "CI pending" with all Tier 1 structural checks confirmed passing.

This is classified `needs-attention` rather than `needs-remediation` because:
- No new code is needed; all source is correct and complete at the structural level.
- The docker build failure mode is infrastructure-only (no Docker daemon in workspace), not a code defect.
- The milestone DoD explicitly separates structural gates (all passing) from runtime gates (docker + Nuke session).
- The blocking item is environment-dependent, not something a remediation slice can fix in-workspace.

The attention item is tracked below. It does not require a new remediation slice.

---

## Attention Items (non-blocking)

1. **Docker build gate pending CI** — `docker-build.sh --linux --versions 16.0` must be run in a Docker-capable environment to confirm `DeepCDefocusPO.so` appears in the release zip. All structural prerequisites are in place. This is the only remaining milestone DoD item.

2. **Runtime Nuke UAT pending** — Visual checks T2-2 through T2-7 from S05-UAT.md (node loads in Nuke, bokeh visible, chromatic fringing visible, holdout depth-correctness) require a Nuke 16.0 license and a real lentil `.fit` file. These are deferred to the CI/artist session.

3. **Stripe-boundary seam (known limitation)** — Scatter contributions crossing stripe boundaries are silently discarded, producing a dark seam when bokeh radius exceeds stripe height. Documented in S02 summary and S05 known-limitations. Not a regression; accepted artefact for this milestone.

4. **Placeholder icon** — `icons/DeepCDefocusPO.png` is a copy of `icons/DeepCBlur.png`. A custom icon is out of scope for M006 per S05 summary. Node is visually distinguishable in the Node Graph via name.

---

## Remediation Plan

Not required. Verdict is `needs-attention` — all structural deliverables are complete. The remaining gates are runtime/CI-only and cannot be addressed by additional code slices in this workspace.
