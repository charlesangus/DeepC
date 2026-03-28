---
id: T03
parent: S01
milestone: M009-mso5fb
provides: []
requires: []
affects: []
key_files: ["src/DeepCOpenDefocus.cpp", "scripts/verify-s01-syntax.sh"]
key_decisions: ["Used using namespace DD::Image to match DeepCDepthBlur.cpp house style", "renderStripe uses makeWritable()+memset(writable()) for zero-fill per task plan note", "Extended verify-s01-syntax.sh in T03 (not T04) because mock headers were needed for T03 verification gate"]
patterns_established: []
drill_down_paths: []
observability_surfaces: []
duration: ""
verification_result: "Ran bash scripts/verify-s01-syntax.sh — all three files (DeepCBlur.cpp, DeepCDepthBlur.cpp, DeepCOpenDefocus.cpp) passed syntax-only g++ compilation with exit 0. Ran the plan's grep-filtered command: 4 'passed' lines, 0 'error' lines."
completed_at: 2026-03-26T13:18:24.064Z
blocker_discovered: false
---

# T03: Created src/DeepCOpenDefocus.cpp PlanarIop no-op scaffold with lens knobs, DeepSampleData FFI wiring, zero-fill output, and extended verify-s01-syntax.sh with PlanarIop/DeepOp/CameraOp mock headers; all three DeepC*.cpp files pass syntax gate

> Created src/DeepCOpenDefocus.cpp PlanarIop no-op scaffold with lens knobs, DeepSampleData FFI wiring, zero-fill output, and extended verify-s01-syntax.sh with PlanarIop/DeepOp/CameraOp mock headers; all three DeepC*.cpp files pass syntax gate

## What Happened
---
id: T03
parent: S01
milestone: M009-mso5fb
key_files:
  - src/DeepCOpenDefocus.cpp
  - scripts/verify-s01-syntax.sh
key_decisions:
  - Used using namespace DD::Image to match DeepCDepthBlur.cpp house style
  - renderStripe uses makeWritable()+memset(writable()) for zero-fill per task plan note
  - Extended verify-s01-syntax.sh in T03 (not T04) because mock headers were needed for T03 verification gate
duration: ""
verification_result: passed
completed_at: 2026-03-26T13:18:24.069Z
blocker_discovered: false
---

# T03: Created src/DeepCOpenDefocus.cpp PlanarIop no-op scaffold with lens knobs, DeepSampleData FFI wiring, zero-fill output, and extended verify-s01-syntax.sh with PlanarIop/DeepOp/CameraOp mock headers; all three DeepC*.cpp files pass syntax gate

**Created src/DeepCOpenDefocus.cpp PlanarIop no-op scaffold with lens knobs, DeepSampleData FFI wiring, zero-fill output, and extended verify-s01-syntax.sh with PlanarIop/DeepOp/CameraOp mock headers; all three DeepC*.cpp files pass syntax gate**

## What Happened

Created src/DeepCOpenDefocus.cpp: PlanarIop subclass with four lens float knobs (focal_length/fstop/focus_distance/sensor_size_mm), three inputs (deep required, holdout deep optional, camera optional), test_input with dynamic_cast guards, renderStripe that flattens deep samples into DeepSampleData FFI arrays, calls the S01 Rust no-op stub, then zero-fills output via makeWritable()+memset. Extended scripts/verify-s01-syntax.sh with six new mock headers (PlanarIop.h, DeepOp.h, CameraOp.h, Row.h, ImagePlane.h, Descriptions.h), added w()/h() to Box mock, and added DeepCOpenDefocus.cpp to the syntax-check loop.

## Verification

Ran bash scripts/verify-s01-syntax.sh — all three files (DeepCBlur.cpp, DeepCDepthBlur.cpp, DeepCOpenDefocus.cpp) passed syntax-only g++ compilation with exit 0. Ran the plan's grep-filtered command: 4 'passed' lines, 0 'error' lines.

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `bash scripts/verify-s01-syntax.sh 2>&1 | grep -E 'passed|error' | head -20` | 0 | ✅ pass | 3200ms |
| 2 | `grep -n 'class DeepCOpenDefocus.*PlanarIop' src/DeepCOpenDefocus.cpp` | 0 | ✅ pass | 5ms |
| 3 | `grep -n 'opendefocus_deep_render' src/DeepCOpenDefocus.cpp` | 0 | ✅ pass | 5ms |
| 4 | `grep -n 'DeepCOpenDefocus.cpp' scripts/verify-s01-syntax.sh` | 0 | ✅ pass | 5ms |


## Deviations

Extended scripts/verify-s01-syntax.sh with mock headers in this task rather than deferring to T04, because T03's verification gate required them. T04 retains THIRD_PARTY_LICENSES.md attribution and any final script polish.

## Known Issues

None. CMake compile requires real Nuke SDK (Docker build only). T04 adds THIRD_PARTY_LICENSES. S02 replaces stub with GPU dispatch.

## Files Created/Modified

- `src/DeepCOpenDefocus.cpp`
- `scripts/verify-s01-syntax.sh`


## Deviations
Extended scripts/verify-s01-syntax.sh with mock headers in this task rather than deferring to T04, because T03's verification gate required them. T04 retains THIRD_PARTY_LICENSES.md attribution and any final script polish.

## Known Issues
None. CMake compile requires real Nuke SDK (Docker build only). T04 adds THIRD_PARTY_LICENSES. S02 replaces stub with GPU dispatch.
