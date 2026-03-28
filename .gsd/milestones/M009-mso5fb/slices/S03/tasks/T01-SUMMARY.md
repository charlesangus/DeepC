---
id: T01
parent: S03
milestone: M009-mso5fb
provides: []
requires: []
affects: []
key_files: ["src/DeepCOpenDefocus.cpp"]
key_decisions: ["Multiply all four RGBA channels by T so holdout attenuates both colour and coverage uniformly", "computeHoldoutTransmittance is a free static function outside the class for unit-testability", "Request Chan_DeepFront alongside Chan_Alpha from holdout deepEngine to properly initialise the deep plane", "fprintf(stderr) INFO log when holdout path active for observability"]
patterns_established: []
drill_down_paths: []
observability_surfaces: []
duration: ""
verification_result: "bash scripts/verify-s01-syntax.sh exits 0 (all three Deep*.cpp pass g++ -std=c++17 -fsyntax-only with mock DDImage headers, test_opendefocus.nk exists check passes). grep confirms computeHoldoutTransmittance at lines 66 and 279. grep confirms dynamic_cast<DeepOp*>(input(1)) at line 266."
completed_at: 2026-03-28T11:30:55.921Z
blocker_discovered: false
---

# T01: Added per-pixel holdout transmittance attenuation (T = product of 1−alpha_s) applied to defocused output_buf after FFI call, gated by dynamic_cast<DeepOp*>(input(1)), plus output_buf→ImagePlane copy replacing old zeros scaffold

> Added per-pixel holdout transmittance attenuation (T = product of 1−alpha_s) applied to defocused output_buf after FFI call, gated by dynamic_cast<DeepOp*>(input(1)), plus output_buf→ImagePlane copy replacing old zeros scaffold

## What Happened
---
id: T01
parent: S03
milestone: M009-mso5fb
key_files:
  - src/DeepCOpenDefocus.cpp
key_decisions:
  - Multiply all four RGBA channels by T so holdout attenuates both colour and coverage uniformly
  - computeHoldoutTransmittance is a free static function outside the class for unit-testability
  - Request Chan_DeepFront alongside Chan_Alpha from holdout deepEngine to properly initialise the deep plane
  - fprintf(stderr) INFO log when holdout path active for observability
duration: ""
verification_result: passed
completed_at: 2026-03-28T11:30:55.921Z
blocker_discovered: false
---

# T01: Added per-pixel holdout transmittance attenuation (T = product of 1−alpha_s) applied to defocused output_buf after FFI call, gated by dynamic_cast<DeepOp*>(input(1)), plus output_buf→ImagePlane copy replacing old zeros scaffold

**Added per-pixel holdout transmittance attenuation (T = product of 1−alpha_s) applied to defocused output_buf after FFI call, gated by dynamic_cast<DeepOp*>(input(1)), plus output_buf→ImagePlane copy replacing old zeros scaffold**

## What Happened

Implemented holdout attenuation entirely in C++ renderStripe, post-FFI, with no changes to the Rust FFI boundary. Added a static free function computeHoldoutTransmittance(const DeepPixel&)->float that multiplies (1-alpha_s) for each holdout sample with alpha clamped to [0,1]. In renderStripe, after opendefocus_deep_render() returns, a dynamic_cast<DeepOp*>(input(1)) guard checks whether the holdout input is connected. If connected, deepEngine() fetches the holdout DeepPlane (Chan_Alpha + Chan_DeepFront), and for each pixel computeHoldoutTransmittance() is called; all four output_buf RGBA channels are multiplied by T. A fprintf(stderr) INFO log records the pixel count when the holdout path fires. Also replaced the old memset-zeros at the end of renderStripe with a memcpy of output_buf into output.writable() so the defocused result actually reaches the output plane. Extended includes with algorithm, utility, and cstdio.

## Verification

bash scripts/verify-s01-syntax.sh exits 0 (all three Deep*.cpp pass g++ -std=c++17 -fsyntax-only with mock DDImage headers, test_opendefocus.nk exists check passes). grep confirms computeHoldoutTransmittance at lines 66 and 279. grep confirms dynamic_cast<DeepOp*>(input(1)) at line 266.

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `bash scripts/verify-s01-syntax.sh` | 0 | ✅ pass | 2000ms |
| 2 | `grep -n "computeHoldoutTransmittance" src/DeepCOpenDefocus.cpp` | 0 | ✅ pass | 5ms |
| 3 | `grep -n "dynamic_cast<DeepOp\*>(input(1))" src/DeepCOpenDefocus.cpp` | 0 | ✅ pass | 5ms |


## Deviations

Added memcpy(output_buf → output.writable()) to replace old memset(zeros) scaffold — this is the correct S02 output-copy step that was left as zeros in the scaffold and needed to be fixed here since this task owns renderStripe.

## Known Issues

None.

## Files Created/Modified

- `src/DeepCOpenDefocus.cpp`


## Deviations
Added memcpy(output_buf → output.writable()) to replace old memset(zeros) scaffold — this is the correct S02 output-copy step that was left as zeros in the scaffold and needed to be fixed here since this task owns renderStripe.

## Known Issues
None.
