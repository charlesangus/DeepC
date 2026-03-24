# S02: DeepCDefocusPOThin Scatter Engine

**Goal:** DeepCDefocusPOThin renders a defocused checkerboard with visible bokeh using thin-lens CoC scatter + polynomial aberration modulation.
**Demo:** `bash scripts/verify-s01-syntax.sh` exits 0; grep contracts confirm CoC, Halton, holdout, CA, max_degree, chanNo all present; `test/test_thin.nk` exists and is well-formed. Runtime proof (`nuke -x test/test_thin.nk` producing non-black EXR) deferred to docker build.

## Must-Haves

- Thin-lens CoC controls scatter radius (not polynomial output positions)
- Polynomial warps aperture sample positions within the CoC disk (Option B from D032)
- Per-channel CA wavelength tracing at 0.45/0.55/0.65 μm
- Halton+Shirley aperture sampling with `_aperture_samples` loop count
- `_max_degree` passed to `poly_system_evaluate` (R032 runtime proof)
- Holdout transmittance applied per-sample (R035 runtime proof)
- `chanNo()` used for all `writableAt` calls (heap corruption guard)
- `chans.contains()` guard on every channel write
- Poly load/reload in `renderStripe` (not relying on `_validate` — M006 thread-safety lesson)
- Test script `test/test_thin.nk` at 128×72 with Angenieux 55mm lens

## Proof Level

- This slice proves: integration (scatter engine + poly evaluation + deep fetch compose into flat output)
- Real runtime required: yes (docker build + `nuke -x`)
- Human/UAT required: yes (visual comparison — does it look defocused, not like an aperture ring?)

## Verification

```bash
# Syntax gate
bash scripts/verify-s01-syntax.sh  # must exit 0

# Scatter engine structural contracts
grep -q 'coc_radius' src/DeepCDefocusPOThin.cpp
grep -q 'halton' src/DeepCDefocusPOThin.cpp
grep -q 'map_to_disk' src/DeepCDefocusPOThin.cpp
grep -q 'chanNo' src/DeepCDefocusPOThin.cpp
grep -q 'transmittance_at' src/DeepCDefocusPOThin.cpp
grep -q '0.45f' src/DeepCDefocusPOThin.cpp
grep -q '_max_degree' src/DeepCDefocusPOThin.cpp
grep -q 'chans.contains' src/DeepCDefocusPOThin.cpp

# Test script exists
test -f test/test_thin.nk
grep -q 'DeepCDefocusPOThin' test/test_thin.nk
grep -q 'exitpupil.fit' test/test_thin.nk
```

## Observability / Diagnostics

- **Runtime signals:** `error()` call when poly file cannot be loaded (visible in Nuke error console). Zero-output early return when `!_poly_loaded || !input0()` — diagnosable by black output tile.
- **Inspection surfaces:** Poly load/reload state (`_poly_loaded`, `_reload_poly`) toggles are observable via Nuke knob_changed flow. The `_max_degree` knob controls polynomial truncation — lower values produce visibly degraded but faster output, confirming the code path is active.
- **Failure visibility:** Missing/corrupt `.fit` file → `error("Cannot open lens file: %s")` message. Missing deep input → silent zero output (black tile). Out-of-bounds scatter samples are bounds-checked and silently clipped (no crash, just missing energy).
- **Redaction:** No secrets or PII in this slice; `.fit` file paths may appear in error messages but are user-provided scene paths.

## Verification (Diagnostic)

```bash
# Failure-path: verify that missing poly produces a parseable error string in the source
grep -q 'error.*Cannot open lens file' src/DeepCDefocusPOThin.cpp
```

## Integration Closure

- Upstream surfaces consumed: `src/DeepCDefocusPOThin.cpp` scaffold (S01), `src/deepc_po_math.h`, `src/poly.h`
- New wiring introduced in this slice: `renderStripe` body wires deep fetch → scatter loop → flat output accumulation
- What remains before the milestone is truly usable end-to-end: S03 (Ray variant), docker build verification, visual UAT

## Tasks

- [x] **T01: Implement thin-lens scatter engine in renderStripe + fix mock header** `est:1h`
  - Why: The S01 scaffold has a zero-output stub in `renderStripe`. This task replaces it with the full thin-lens CoC scatter engine that produces correct defocused output. Also fixes the `chanNo` mock header gap.
  - Files: `src/DeepCDefocusPOThin.cpp`, `scripts/verify-s01-syntax.sh`
  - Do: Replace `renderStripe` body with: poly load/reload check, deep fetch via `deepEngine`, per-pixel/per-sample/per-aperture scatter loop using `coc_radius` for scatter radius, Option B poly-warped aperture positions, per-channel CA wavelengths, holdout `transmittance_at` lambda, `chanNo`+`chans.contains` guards. Add `chanNo(Channel)` and `writableAt(int,int,int)` signatures to `ImagePlane` mock in verify script.
  - Verify: `bash scripts/verify-s01-syntax.sh` exits 0; all grep contracts pass
  - Done when: Syntax check passes and all 8 grep contracts return 0

- [x] **T02: Write test/test_thin.nk test script** `est:20m`
  - Why: The UAT gate requires `nuke -x test/test_thin.nk` to produce non-black 128×72 EXR. This task creates the Nuke script adapted from the reference test.
  - Files: `test/test_thin.nk`
  - Do: Adapt `/home/latuser/git/DeepC/test/test_deepcdefocus_po.nk` — change node class to `DeepCDefocusPOThin`, add `max_degree 11`, set `focal_length 55`, `focus_distance 10000`, output to `./test_thin.exr` at 128×72.
  - Verify: `test -f test/test_thin.nk && grep -q 'DeepCDefocusPOThin' test/test_thin.nk && grep -q 'exitpupil.fit' test/test_thin.nk`
  - Done when: Test script file exists with correct node class, lens file path, and output path

## Files Likely Touched

- `src/DeepCDefocusPOThin.cpp`
- `scripts/verify-s01-syntax.sh`
- `test/test_thin.nk`
