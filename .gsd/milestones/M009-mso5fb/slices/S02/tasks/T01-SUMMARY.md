---
id: T01
parent: S02
milestone: M009-mso5fb
provides: []
requires: []
affects: []
key_files: ["crates/opendefocus-deep/src/lib.rs", "crates/opendefocus-deep/include/opendefocus_deep.h", "src/DeepCOpenDefocus.cpp"]
key_decisions: ["LensParams placed before DeepSampleData in lib.rs so it is declared before use in the extern fn signature", "All fields use f32/float matching natural knob storage types with no conversion needed"]
patterns_established: []
drill_down_paths: []
observability_surfaces: []
duration: ""
verification_result: "Ran scripts/verify-s01-syntax.sh which syntax-checks all three src/Deep*.cpp files using mock DDImage headers via g++ -std=c++17 -fsyntax-only. All three (DeepCBlur.cpp, DeepCDepthBlur.cpp, DeepCOpenDefocus.cpp) passed with exit code 0."
completed_at: 2026-03-28T10:53:49.400Z
blocker_discovered: false
---

# T01: Added LensParams #[repr(C)] struct and updated opendefocus_deep_render FFI signature to carry lens parameters (focal_length_mm, fstop, focus_distance, sensor_size_mm) across the C++/Rust boundary

> Added LensParams #[repr(C)] struct and updated opendefocus_deep_render FFI signature to carry lens parameters (focal_length_mm, fstop, focus_distance, sensor_size_mm) across the C++/Rust boundary

## What Happened
---
id: T01
parent: S02
milestone: M009-mso5fb
key_files:
  - crates/opendefocus-deep/src/lib.rs
  - crates/opendefocus-deep/include/opendefocus_deep.h
  - src/DeepCOpenDefocus.cpp
key_decisions:
  - LensParams placed before DeepSampleData in lib.rs so it is declared before use in the extern fn signature
  - All fields use f32/float matching natural knob storage types with no conversion needed
duration: ""
verification_result: passed
completed_at: 2026-03-28T10:53:49.401Z
blocker_discovered: false
---

# T01: Added LensParams #[repr(C)] struct and updated opendefocus_deep_render FFI signature to carry lens parameters (focal_length_mm, fstop, focus_distance, sensor_size_mm) across the C++/Rust boundary

**Added LensParams #[repr(C)] struct and updated opendefocus_deep_render FFI signature to carry lens parameters (focal_length_mm, fstop, focus_distance, sensor_size_mm) across the C++/Rust boundary**

## What Happened

The S01 opendefocus_deep_render() had no way to pass lens parameters needed for CoC computation in S02. Three coordinated changes wired the full lens parameter surface through the FFI boundary: (1) LensParams #[repr(C)] struct added to lib.rs before DeepSampleData, with the function signature extended to accept *const LensParams as a 5th arg; (2) matching LensParams typedef and updated prototype added to opendefocus_deep.h; (3) DeepCOpenDefocus.cpp renderStripe now constructs a stack LensParams from the four knob members and passes &lensParams to the FFI call. The stub body remains empty — no behavioral change — but lens values are now wired end-to-end for S02 CoC implementation.

## Verification

Ran scripts/verify-s01-syntax.sh which syntax-checks all three src/Deep*.cpp files using mock DDImage headers via g++ -std=c++17 -fsyntax-only. All three (DeepCBlur.cpp, DeepCDepthBlur.cpp, DeepCOpenDefocus.cpp) passed with exit code 0.

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `bash scripts/verify-s01-syntax.sh` | 0 | ✅ pass | 1100ms |


## Deviations

None.

## Known Issues

None.

## Files Created/Modified

- `crates/opendefocus-deep/src/lib.rs`
- `crates/opendefocus-deep/include/opendefocus_deep.h`
- `src/DeepCOpenDefocus.cpp`


## Deviations
None.

## Known Issues
None.
