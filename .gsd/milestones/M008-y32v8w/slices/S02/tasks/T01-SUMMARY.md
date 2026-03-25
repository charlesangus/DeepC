---
id: T01
parent: S02
milestone: M008-y32v8w
provides:
  - Gather path-trace engine in DeepCDefocusPORay renderStripe
  - Expanded deep request bounds in getRequests
  - S02 structural contracts in verify script
key_files:
  - src/DeepCDefocusPORay.cpp
  - scripts/verify-s01-syntax.sh
key_decisions:
  - Vignetting retry breaks on any channel failure (all_channels_ok flag) rather than continuing partial channels
patterns_established:
  - Gather loop pattern: output pixel → sensor mm → aperture sample → pt_sample_aperture → poly eval → pupil culls → sphereToCs_full → pinhole projection → deep column flatten
observability_surfaces:
  - S02 grep contracts in verify-s01-syntax.sh verify structural presence of gather engine functions and absence of scatter vestiges
duration: 12m
verification_result: passed
completed_at: 2026-03-25
blocker_discovered: false
---

# T01: Replace Ray scatter engine with path-trace gather loop + add S02 contracts

**Replaced scatter renderStripe with gather path-trace engine using pt_sample_aperture, sphereToCs_full, and per-ray deep column flatten with vignetting retry**

## What Happened

Replaced the entire scatter loop in DeepCDefocusPORay::renderStripe with a gather path-trace engine. The old scatter loop iterated input pixels, computed thin-lens CoC, and splatted to output pixels using an Option B warp. The new gather loop iterates output pixels, converts to sensor mm coordinates (y divided by half_w for aspect ratio), samples aperture points via Halton sequence with vignetting retry (VIGNETTING_RETRIES=10), traces each CA channel through pt_sample_aperture for Newton aperture matching, forward poly_system_evaluate, outer then inner pupil culls, sphereToCs_full with center=-R convention for 3D ray direction, pinhole projects back to input pixel, and flattens the deep column front-to-back with holdout transmittance evaluated at the output pixel.

Also expanded getRequests to pad the deep request box by max CoC pixels (aperture_housing_radius / sensor_width * image_width * 2 + 2), and the renderStripe deep fetch uses the same expanded bounds. Removed all scatter vestiges: coc_norm, coc_radius usage, ap_radius normalised, Option B warp code, and the old alpha scatter section.

Added S02 contracts section to verify-s01-syntax.sh: positive greps for pt_sample_aperture, sphereToCs_full, logarithmic_focus_search, VIGNETTING_RETRIES, getPixel; negative grep for coc_norm.

## Verification

- `bash scripts/verify-s01-syntax.sh` exits 0 — all 4 source files compile, all S01 contracts pass, all S02 contracts pass
- `grep -q 'coc_norm' src/DeepCDefocusPORay.cpp` returns exit code 1 — scatter vestige confirmed removed

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `bash scripts/verify-s01-syntax.sh` | 0 | ✅ pass | ~3s |
| 2 | `grep -q 'coc_norm' src/DeepCDefocusPORay.cpp` | 1 | ✅ pass | <1s |

## Diagnostics

- Run `bash scripts/verify-s01-syntax.sh` to verify all S01+S02 contracts and compilation
- Check for scatter vestiges: `grep -n 'coc_norm\|Option B\|ap_radius' src/DeepCDefocusPORay.cpp` should return nothing
- Verify gather engine structure: `grep -n 'pt_sample_aperture\|sphereToCs_full\|VIGNETTING_RETRIES\|getPixel\|logarithmic_focus_search' src/DeepCDefocusPORay.cpp`

## Deviations

None.

## Known Issues

None.

## Files Created/Modified

- `src/DeepCDefocusPORay.cpp` — Replaced scatter renderStripe with gather path-trace engine; expanded getRequests deep bounds
- `scripts/verify-s01-syntax.sh` — Added S02 contracts section (6 structural grep checks)
- `.gsd/milestones/M008-y32v8w/slices/S02/S02-PLAN.md` — Added Observability/Diagnostics and failure-path verification sections
- `.gsd/milestones/M008-y32v8w/slices/S02/tasks/T01-PLAN.md` — Added Observability Impact section
