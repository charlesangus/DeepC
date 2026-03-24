# Quick Task 7: DeepCDefocusPO incorrect output — wrong frame size, all black

**Date:** 2026-03-24
**Branch:** gsd/quick/7-deepcdefocus-isn-t-crashing-anymore-but
**Commit:** f5f1bd1

## What Changed

### 1. Frame size mismatch (displayWindow)
`_validate` only set `info_.full_size_format()` but not `info_.format()`. The output displayWindow defaulted to Nuke's default (640×480) instead of matching the Deep input (1280×720). Fixed by adding `info_.format(*di.format())`.

### 2. All-black output — inverse vs forward poly evaluation
`lt_newton_trace` solves the **inverse** lens mapping (gather direction): "find the input position that maps to the target through the lens." But scatter-based defocus needs the **forward** direction: "given a sample position and aperture, where does the light land?" The Newton trace converged to landing coordinates 10–70× outside the normalised sensor range, causing every contribution to fail the bounds check. Replaced with direct forward evaluation: `poly(sensor, aperture, lambda)[2:3]` gives the correct landing position.

### 3. Performance: poly_term evaluation and degree truncation
- Added coefficient epsilon skip (`|coeff| < 1e-35`) and zero-input early-exit to `poly_term_evaluate`, matching lentil's `poly_coeff_eps`.
- Added `max_degree` parameter to `poly_system_evaluate` for degree-based term truncation. Terms in .fit files are sorted by ascending degree, enabling early loop exit. At degree 3: 56 terms evaluated instead of 4368 (78× reduction). At degree 5: 252 terms (17× reduction).
- Removing the Newton iteration (which called poly_system_evaluate ~60× per pixel per wavelength) reduces per-pixel cost by another ~60×.
- Net: a render that timed out at 120s now completes in 0.59s.

### 4. Test script fixes
- `DeepFromImage` now has `set_z true` and `z 5000` so deep samples actually exist.
- Added `Reformat` to 128×72 for fast CI rendering.
- 4 aperture samples, f/2.8 — produces 18 non-zero pixels per channel in the scatter output.

## Files Modified
- `src/DeepCDefocusPO.cpp` — format propagation, forward eval replacing Newton trace
- `src/poly.h` — poly_term_evaluate epsilon/zero optimisations, max_degree in poly_system_evaluate
- `scripts/verify-s01-syntax.sh` — mock Info/DeepInfo format methods
- `test/test_deepcdefocus_po.nk` — fixed deep data generation, reduced resolution

## Verification
- `nuke -t test_format_final.py`: Input 1280×720, Output 1280×720 — PASS
- `nuke -x test/test_deepcdefocus_po.nk`: Renders in 0.59s, exit 0, produces 128×72 EXR with 18 non-zero pixels per channel
