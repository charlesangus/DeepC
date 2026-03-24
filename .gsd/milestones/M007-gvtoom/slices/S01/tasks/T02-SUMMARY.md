---
id: T02
parent: S01
milestone: M007-gvtoom
provides:
  - DeepCDefocusPOThin.cpp scaffold with all thin-lens knobs including max_degree
  - DeepCDefocusPORay.cpp scaffold with aperture_file, lens geometry group, second poly_system_t
  - Stub renderStripe (zeros output) in both plugins
  - Holdout input wiring, CA wavelengths, _validate poly loading preserved in both
key_files:
  - src/DeepCDefocusPOThin.cpp
  - src/DeepCDefocusPORay.cpp
key_decisions:
  - CA wavelengths stored as static constexpr class members rather than local variables for reuse in S02
  - Ray variant error() messages distinguish "lens file" from "aperture file" for clear diagnostics
patterns_established:
  - Two-plugin scaffold pattern: Thin is base, Ray extends with additional knobs and second poly_system_t
observability_surfaces:
  - _validate error() on poly_system_read failure with file path in message
  - Ray variant surfaces separate error for aperture file load failure
  - Stub renderStripe zeros all channels — observable as black output when wired in Nuke
duration: 12m
verification_result: passed
completed_at: 2026-03-24
blocker_discovered: false
---

# T02: Create DeepCDefocusPOThin.cpp and DeepCDefocusPORay.cpp scaffolds

**Created thin-lens and raytraced-gather plugin scaffolds with all knobs, holdout wiring, poly loading, and zero-output stub renderStripe**

## What Happened

Created two new Nuke plugin source files derived from the existing `src/DeepCDefocusPO.cpp` template:

1. **DeepCDefocusPOThin.cpp** — Thin-lens variant with all base knobs (poly_file, focal_length, focus_distance, fstop, aperture_samples) plus new `max_degree` Int_knob (default 11, range 1–11). renderStripe replaced with zero-output stub. All infrastructure preserved: holdout input wiring (input 1, "holdout" label, test_input with DeepOp cast), CA wavelength constants (0.45f/0.55f/0.65f as static constexpr), knob_changed poly reload trigger, _validate with poly loading and error() on failure, _close and destructor cleanup, getRequests with holdout deepRequest.

2. **DeepCDefocusPORay.cpp** — Raytraced-gather variant extending Thin with: second `const char* _aperture_file` File_knob, second `poly_system_t _aperture_sys` with independent load/reload tracking, 4 lens geometry Float_knobs in "Lens geometry" closed group (outer_pupil_curvature_radius=90.77, lens_length=100.89, aperture_housing_radius=14.10, inner_pupil_curvature_radius=-112.58 — Angenieux 55mm defaults), aperture_file knob_changed handler, aperture poly loading in _validate with separate error message, and dual poly_system_destroy in _close/destructor.

Also added failure-path verification checks to the S01 slice plan per pre-flight requirement.

## Verification

All task-plan and slice-applicable checks pass:

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `grep -q 'DeepCDefocusPOThin' src/DeepCDefocusPOThin.cpp` | 0 | ✅ pass | <1s |
| 2 | `grep -q 'DeepCDefocusPORay' src/DeepCDefocusPORay.cpp` | 0 | ✅ pass | <1s |
| 3 | `grep -q '_max_degree' src/DeepCDefocusPOThin.cpp` | 0 | ✅ pass | <1s |
| 4 | `grep -q '_max_degree' src/DeepCDefocusPORay.cpp` | 0 | ✅ pass | <1s |
| 5 | `grep -q 'aperture_file' src/DeepCDefocusPORay.cpp` | 0 | ✅ pass | <1s |
| 6 | `grep -q '90.77' src/DeepCDefocusPORay.cpp` | 0 | ✅ pass | <1s |
| 7 | `grep -q 'outer_pupil_curvature_radius' src/DeepCDefocusPORay.cpp` | 0 | ✅ pass | <1s |
| 8 | `grep -q 'writableAt.*= 0' src/DeepCDefocusPOThin.cpp` | 0 | ✅ pass | <1s |
| 9 | `grep -q 'writableAt.*= 0' src/DeepCDefocusPORay.cpp` | 0 | ✅ pass | <1s |
| 10 | `grep -q 'holdout' src/DeepCDefocusPOThin.cpp` | 0 | ✅ pass | <1s |
| 11 | `grep -q 'holdout' src/DeepCDefocusPORay.cpp` | 0 | ✅ pass | <1s |
| 12 | `grep -q 'error.*Cannot open lens file' src/DeepCDefocusPOThin.cpp` | 0 | ✅ pass | <1s |
| 13 | `grep -q 'error.*Cannot open aperture file' src/DeepCDefocusPORay.cpp` | 0 | ✅ pass | <1s |
| 14 | `grep -q 'Deep/DeepCDefocusPOThin' src/DeepCDefocusPOThin.cpp` | 0 | ✅ pass | <1s |
| 15 | `grep -q 'Deep/DeepCDefocusPORay' src/DeepCDefocusPORay.cpp` | 0 | ✅ pass | <1s |

Slice-level checks requiring CMake updates and syntax-check script (T03) are not yet applicable.

## Diagnostics

- `grep -c '_max_degree' src/DeepCDefocusPOThin.cpp` — verify max_degree knob presence (expect ≥3: member, constructor init, Int_knob)
- `grep -c 'poly_system_t' src/DeepCDefocusPORay.cpp` — verify dual poly systems (expect 2)
- `grep 'error(' src/DeepCDefocusPOThin.cpp src/DeepCDefocusPORay.cpp` — inspect error messages in _validate
- `grep 'BeginClosedGroup\|EndGroup' src/DeepCDefocusPORay.cpp` — verify lens geometry group structure

## Deviations

- CA wavelengths stored as `static constexpr` class members instead of local `const float` arrays — cleaner for S02 reuse.
- The full renderStripe scatter engine from DeepCDefocusPO.cpp was not preserved as dead code; only the stub is present. The original remains in DeepCDefocusPO.cpp as reference for S02.

## Known Issues

None.

## Files Created/Modified

- `src/DeepCDefocusPOThin.cpp` — new thin-lens scaffold plugin (all knobs, stub renderStripe, holdout, poly loading)
- `src/DeepCDefocusPORay.cpp` — new raytraced-gather scaffold plugin (Thin base + aperture_file, lens geometry, dual poly_system_t)
- `.gsd/milestones/M007-gvtoom/slices/S01/S01-PLAN.md` — marked T02 done, added failure-path verification checks
