---
id: T01
parent: S02
milestone: M011-qa62vv
provides: []
requires: []
affects: []
key_files: ["src/DeepCOpenDefocus.cpp"]
key_decisions: ["buildHoldoutData() encodes [d_front, T, d_back, T] matching the HoldoutData FFI contract exactly", "Holdout dispatch is pre-render (not post-render), so the Rust kernel receives holdout geometry during the scatter/gather pass", "Moved holdout log line into new holdout branch only — fallback path emits nothing extra"]
patterns_established: []
drill_down_paths: []
observability_surfaces: []
duration: ""
verification_result: "Ran cargo test -p opendefocus-deep -- holdout: 1 test passed (test_holdout_attenuates_background_samples). Ran bash scripts/verify-s01-syntax.sh: g++ -std=c++17 -fsyntax-only passed for DeepCOpenDefocus.cpp and all other .cpp files, all .nk test files present, script exited 0."
completed_at: 2026-03-29T11:49:40.666Z
blocker_discovered: false
---

# T01: Replaced incorrect post-defocus scalar holdout multiply with depth-correct opendefocus_deep_render_holdout dispatch via new buildHoldoutData() helper in DeepCOpenDefocus.cpp

> Replaced incorrect post-defocus scalar holdout multiply with depth-correct opendefocus_deep_render_holdout dispatch via new buildHoldoutData() helper in DeepCOpenDefocus.cpp

## What Happened
---
id: T01
parent: S02
milestone: M011-qa62vv
key_files:
  - src/DeepCOpenDefocus.cpp
key_decisions:
  - buildHoldoutData() encodes [d_front, T, d_back, T] matching the HoldoutData FFI contract exactly
  - Holdout dispatch is pre-render (not post-render), so the Rust kernel receives holdout geometry during the scatter/gather pass
  - Moved holdout log line into new holdout branch only — fallback path emits nothing extra
duration: ""
verification_result: passed
completed_at: 2026-03-29T11:49:40.666Z
blocker_discovered: false
---

# T01: Replaced incorrect post-defocus scalar holdout multiply with depth-correct opendefocus_deep_render_holdout dispatch via new buildHoldoutData() helper in DeepCOpenDefocus.cpp

**Replaced incorrect post-defocus scalar holdout multiply with depth-correct opendefocus_deep_render_holdout dispatch via new buildHoldoutData() helper in DeepCOpenDefocus.cpp**

## What Happened

The existing src/DeepCOpenDefocus.cpp had computeHoldoutTransmittance() applying transmittance as a post-defocus scalar multiply (after the Rust kernel ran), which blurred holdout edges. Removed that function and the entire S03 post-render block. Added buildHoldoutData() that iterates pixels in y-outer × x-inner scan order, sorts samples front-to-back by Chan_DeepFront, computes T = product(1 - clamp(alpha,0,1)), and encodes [d_front, T, d_back, T] per pixel into the HoldoutData FFI buffer. Updated renderStripe() to conditionally dispatch: when input 1 (holdout) is connected, calls buildHoldoutData() + opendefocus_deep_render_holdout(); when not connected, calls opendefocus_deep_render() unchanged. Moved the holdout log line into the new holdout branch to preserve observability.

## Verification

Ran cargo test -p opendefocus-deep -- holdout: 1 test passed (test_holdout_attenuates_background_samples). Ran bash scripts/verify-s01-syntax.sh: g++ -std=c++17 -fsyntax-only passed for DeepCOpenDefocus.cpp and all other .cpp files, all .nk test files present, script exited 0.

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `PATH=$HOME/.cargo/bin:$PATH PROTOC=/tmp/protoc_dir/bin/protoc cargo test -p opendefocus-deep -- holdout` | 0 | ✅ pass | 7000ms |
| 2 | `bash scripts/verify-s01-syntax.sh` | 0 | ✅ pass | 2000ms |


## Deviations

None.

## Known Issues

None.

## Files Created/Modified

- `src/DeepCOpenDefocus.cpp`


## Deviations
None.

## Known Issues
None.
