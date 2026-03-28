---
id: T02
parent: S03
milestone: M009-mso5fb
provides: []
requires: []
affects: []
key_files: ["src/DeepCOpenDefocus.cpp", "scripts/verify-s01-syntax.sh"]
key_decisions: ["Camera override applied at renderStripe time so per-frame camera animation is honoured", "cam->validate(false) called before extracting values", "film_width() returns cm; multiply by 10.0f for mm", "fprintf(stderr) logs both active and inactive camera link states"]
patterns_established: []
drill_down_paths: []
observability_surfaces: []
duration: ""
verification_result: "bash scripts/verify-s01-syntax.sh exits 0 (all three Deep*.cpp pass g++ -std=c++17 -fsyntax-only). grep confirms cam->focal_length() at line 268 and cam->fStop() at line 269. T01 signals (computeHoldoutTransmittance, dynamic_cast<DeepOp*>(input(1))) remain intact."
completed_at: 2026-03-28T11:33:06.968Z
blocker_discovered: false
---

# T02: Wired CameraOp input(2) to override lens knob values (focal_length, fstop, focus_distance, sensor_size_mm) before the LensParams FFI struct is built, with active/inactive stderr logs and updated knob tooltips

> Wired CameraOp input(2) to override lens knob values (focal_length, fstop, focus_distance, sensor_size_mm) before the LensParams FFI struct is built, with active/inactive stderr logs and updated knob tooltips

## What Happened
---
id: T02
parent: S03
milestone: M009-mso5fb
key_files:
  - src/DeepCOpenDefocus.cpp
  - scripts/verify-s01-syntax.sh
key_decisions:
  - Camera override applied at renderStripe time so per-frame camera animation is honoured
  - cam->validate(false) called before extracting values
  - film_width() returns cm; multiply by 10.0f for mm
  - fprintf(stderr) logs both active and inactive camera link states
duration: ""
verification_result: passed
completed_at: 2026-03-28T11:33:06.969Z
blocker_discovered: false
---

# T02: Wired CameraOp input(2) to override lens knob values (focal_length, fstop, focus_distance, sensor_size_mm) before the LensParams FFI struct is built, with active/inactive stderr logs and updated knob tooltips

**Wired CameraOp input(2) to override lens knob values (focal_length, fstop, focus_distance, sensor_size_mm) before the LensParams FFI struct is built, with active/inactive stderr logs and updated knob tooltips**

## What Happened

Added a camera-link override block in renderStripe immediately before LensParams construction. The block dynamic_cast<CameraOp*>(input(2)) — if non-null, calls cam->validate(false) then extracts focal_length(), fStop(), projection_distance(), and film_width()*10.0f (cm→mm) into locals; if null, locals fall back to knob values. LensParams is built from the locals. Added fprintf(stderr) INFO logs for both active and inactive camera states. Updated all four Float_knob tooltip strings to note units and camera override behaviour. Extended the verify script CameraOp mock to add the four accessor methods so the syntax check compiles the new code.

## Verification

bash scripts/verify-s01-syntax.sh exits 0 (all three Deep*.cpp pass g++ -std=c++17 -fsyntax-only). grep confirms cam->focal_length() at line 268 and cam->fStop() at line 269. T01 signals (computeHoldoutTransmittance, dynamic_cast<DeepOp*>(input(1))) remain intact.

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `bash scripts/verify-s01-syntax.sh` | 0 | ✅ pass | 1800ms |
| 2 | `grep -n "cam->focal_length" src/DeepCOpenDefocus.cpp` | 0 | ✅ pass | 5ms |
| 3 | `grep -n "cam->fStop" src/DeepCOpenDefocus.cpp` | 0 | ✅ pass | 5ms |
| 4 | `grep -n "computeHoldoutTransmittance" src/DeepCOpenDefocus.cpp` | 0 | ✅ pass | 5ms |
| 5 | `grep -n "dynamic_cast<DeepOp" src/DeepCOpenDefocus.cpp` | 0 | ✅ pass | 5ms |


## Deviations

Extended scripts/verify-s01-syntax.sh CameraOp mock with focal_length/fStop/projection_distance/film_width accessor stubs — required for the syntax check to compile the new camera-link code.

## Known Issues

None.

## Files Created/Modified

- `src/DeepCOpenDefocus.cpp`
- `scripts/verify-s01-syntax.sh`


## Deviations
Extended scripts/verify-s01-syntax.sh CameraOp mock with focal_length/fStop/projection_distance/film_width accessor stubs — required for the syntax check to compile the new camera-link code.

## Known Issues
None.
