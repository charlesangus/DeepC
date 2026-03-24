---
id: T01
parent: S03
milestone: M007-gvtoom
provides:
  - Full gather renderStripe in DeepCDefocusPORay.cpp replacing zero-output stub
key_files:
  - src/DeepCDefocusPORay.cpp
key_decisions:
  - DDImage::Box lacks pad/intersect — used manual Box constructor with std::max/std::min for expanded bounds
patterns_established:
  - Gather loop pattern: outer output-pixel loop with inner CoC-bounded neighbourhood search and selectivity guard
  - Aperture poly reload guard mirrors exitpupil reload guard pattern
observability_surfaces:
  - error() calls on lens file load failure (both exitpupil and aperture)
  - Zero output (all-black) when poly not loaded — detectable via pixel count
duration: 15m
verification_result: passed
completed_at: 2026-03-24
blocker_discovered: false
---

# T01: Implement gather renderStripe in DeepCDefocusPORay.cpp

**Replaced zero-output stub renderStripe with full raytraced gather engine featuring CoC-bounded neighbourhood search, aperture vignetting, sphereToCs ray direction, and gather selectivity guard.**

## What Happened

Implemented the complete gather renderStripe in `src/DeepCDefocusPORay.cpp`, following the Thin scatter pattern for boilerplate (zero_output lambda, poly reload guard, holdout fetch, transmittance_at, CA per-channel evaluation, alpha at green landing) and adding gather-specific logic:

1. **Aperture poly reload guard** — same pattern as exitpupil, using `_aperture_file`, `_aperture_loaded`, `_reload_aperture`, `_aperture_sys`.
2. **CoC neighbourhood bound** — computed `max_coc_px` from `coc_radius()` at depth 1.0, used to define expanded input fetch region.
3. **Expanded bounds** — manual Box construction with `std::max`/`std::min` (DDImage::Box lacks `pad()`/`intersect()`).
4. **Gather loop** — outer loop over output pixels (ox, oy), inner loop over input neighbourhood (ix, iy) within ±max_coc_px, then deep samples, aperture samples (Halton + Shirley disk), and CA channels.
5. **Aperture vignetting** — `poly_system_evaluate(&_aperture_sys, in5, apt_out, 2, _max_degree)`; skip if magnitude exceeds `_aperture_housing_radius`.
6. **sphereToCs** — called with `out5[2], out5[3], _outer_pupil_curvature_radius` for R033 physical completeness.
7. **Option B landing** — CoC warp from `out5[0:1]` consistent with Thin, using `_aperture_housing_radius` (physical mm) instead of `1/fstop`.
8. **Gather selectivity** — `if (ox_land != ox || oy_land != oy) continue` ensures only matching contributions accumulate.

## Verification

All structural grep checks pass. Syntax check passes. test_ray.nk deferred to T02.

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `grep -q '_aperture_sys' src/DeepCDefocusPORay.cpp` | 0 | ✅ pass | <1s |
| 2 | `grep -q 'sphereToCs' src/DeepCDefocusPORay.cpp` | 0 | ✅ pass | <1s |
| 3 | `test $(grep -c '_max_degree' src/DeepCDefocusPORay.cpp) -ge 2` | 0 | ✅ pass | <1s |
| 4 | `grep -q 'transmittance_at' src/DeepCDefocusPORay.cpp` | 0 | ✅ pass | <1s |
| 5 | `grep -q 'halton' src/DeepCDefocusPORay.cpp` | 0 | ✅ pass | <1s |
| 6 | `grep -q '0.45f' src/DeepCDefocusPORay.cpp` | 0 | ✅ pass | <1s |
| 7 | `grep -qE 'ox_land\|oy_land' src/DeepCDefocusPORay.cpp` | 0 | ✅ pass | <1s |
| 8 | `bash scripts/verify-s01-syntax.sh` | 0 | ✅ pass | 3s |
| 9 | `test -f test/test_ray.nk` | 1 | ⏳ T02 | <1s |

## Diagnostics

- **Lens file errors:** `error()` calls surface in Nuke's message panel with the failing file path for both exitpupil and aperture poly files.
- **Zero output diagnosis:** If render produces all-black, check: (1) poly_file knob points to valid .fit, (2) aperture_file knob is valid if set, (3) input deep stream has samples in the requested region.
- **Structural verification:** Run `grep -q '_aperture_sys' src/DeepCDefocusPORay.cpp && grep -q 'sphereToCs' src/DeepCDefocusPORay.cpp` to confirm subsystems are present.

## Deviations

- DDImage::Box has no `pad()` or `intersect()` methods (planner assumed they existed). Fixed by constructing expanded Box manually with `std::max`/`std::min` against input format box bounds.

## Known Issues

None.

## Files Created/Modified

- `src/DeepCDefocusPORay.cpp` — Replaced stub renderStripe with full gather engine (~180 lines)
- `.gsd/milestones/M007-gvtoom/slices/S03/S03-PLAN.md` — Added Observability / Diagnostics section
