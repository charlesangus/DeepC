# S02: Ray path-trace engine

**Goal:** DeepCDefocusPORay traces real polynomial-optics rays for each output pixel, producing visibly defocused output from Deep input with vignetting retry and per-ray deep flatten.
**Demo:** `bash scripts/verify-s01-syntax.sh` exits 0 with all S01 + S02 contracts passing. The scatter loop is fully replaced by a gather path-trace engine.

## Must-Haves

- Gather loop replacing scatter loop: output pixel → sensor mm → pt_sample_aperture → poly eval → sphereToCs_full → ray-to-pixel projection → deep column read → front-to-back flatten
- Expanded deep fetch in both getRequests and renderStripe to cover rays landing outside stripe bounds
- Vignetting retry loop (VIGNETTING_RETRIES constant, Halton re-index) for field-edge convergence
- Per-ray deep column flatten with holdout transmittance at output pixel
- CA wavelength tracing preserved (R/G/B at 0.45/0.55/0.65μm)
- sensor_shift computed via logarithmic_focus_search once per renderStripe
- Sensor coordinates in physical mm using _sensor_width (y divided by half_w, not half_h)
- S02 structural contracts added to verify-s01-syntax.sh
- All S01 contracts still passing
- Scatter vestiges removed (coc_norm, coc_radius usage, Option B warp)

## Proof Level

- This slice proves: contract (structural grep contracts + syntax compilation)
- Real runtime required: no (docker build + nuke -x is milestone DoD, not S02 gate)
- Human/UAT required: no

## Verification

- `bash scripts/verify-s01-syntax.sh` exits 0 — all S01 contracts pass, all S02 contracts pass, all 4 source files compile

## Integration Closure

- Upstream surfaces consumed: `src/deepc_po_math.h` (pt_sample_aperture, sphereToCs_full, logarithmic_focus_search), lens constant knobs on Ray class
- New wiring introduced in this slice: renderStripe gather engine calls S01 math functions with lens constant knobs; getRequests expanded bounds
- What remains before the milestone is truly usable end-to-end: docker build + `nuke -x test/test_ray.nk` runtime verification (milestone DoD)

## Tasks

- [ ] **T01: Replace Ray scatter engine with path-trace gather loop + add S02 contracts** `est:2h`
  - Why: This is the core S02 deliverable — R037 (renderStripe swap). The scatter loop is incorrect for polynomial optics (it uses thin-lens CoC warp); the gather loop traces real rays through the lens polynomial.
  - Files: `src/DeepCDefocusPORay.cpp`, `scripts/verify-s01-syntax.sh`
  - Do: (1) Replace getRequests to expand deep request by max CoC pixels using _aperture_housing_radius. (2) Replace renderStripe scatter loop with gather engine: compute sensor_shift via logarithmic_focus_search, compute aperture_radius_mm and f_px, iterate output pixels in mm coords (y uses half_w not half_h), sample aperture disk with Halton, call pt_sample_aperture for Newton match, forward poly_system_evaluate, outer/inner pupil culls, sphereToCs_full for 3D ray, pinhole project to input pixel, deep column flatten front-to-back with holdout, accumulate. Wrap aperture sampling in vignetting retry loop (VIGNETTING_RETRIES=10). (3) Remove scatter vestiges (coc_norm, coc_radius usage, Option B warp code). (4) Add S02 contracts section to verify-s01-syntax.sh: grep for pt_sample_aperture/sphereToCs_full/logarithmic_focus_search/VIGNETTING_RETRIES/getPixel in Ray.cpp, negative grep for coc_norm. (5) Run verify-s01-syntax.sh to confirm all contracts pass.
  - Verify: `bash scripts/verify-s01-syntax.sh` exits 0 with all S01 + S02 contracts passing
  - Done when: Ray's renderStripe is a gather path-trace engine using all S01 math functions, verify script passes with S02 contracts, no scatter vestiges remain

## Files Likely Touched

- `src/DeepCDefocusPORay.cpp`
- `scripts/verify-s01-syntax.sh`
