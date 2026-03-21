---
id: T02
parent: S01
milestone: M002
provides:
  - DeepCBlur.cpp DeepFilterOp plugin with 2D Gaussian blur, sample propagation, per-pixel optimization
  - CMakeLists.txt integration for DeepCBlur build and menu registration
key_files:
  - src/DeepCBlur.cpp
  - src/CMakeLists.txt
key_decisions:
  - Full 2D (non-separable) Gaussian kernel computed per doDeepEngine call
  - Depth channels (Chan_DeepFront/Chan_DeepBack) propagated raw from source, not weighted
  - Zero-blur passthrough fast path avoids unnecessary kernel computation
  - Kernel weight indexed as (dy+radY)*kernelW+(dx+radX) flat vector
patterns_established:
  - DeepFilterOp subclass with bbox expansion in _validate and padded input in getDeepRequests
  - thread_local ScratchBuf pattern reused from DeepThinner for zero-allocation hot path
  - deepc::SampleRecord channels vector stores all channel data including depth positions for emit phase
observability_surfaces:
  - Compile-time: cmake --build build/ surfaces missing includes, type mismatches, linker failures
  - Runtime: Nuke red error badge if doDeepEngine returns false; Op::aborted() cancellation feedback
  - Inspection: blur_width, blur_height, max_samples, merge_tolerance, color_tolerance visible in node properties
  - Failure path: return false on abort or input engine failure
duration: 8min
verification_result: passed
completed_at: 2025-03-20
blocker_discovered: false
---

# T02: Implement DeepCBlur.cpp plugin and wire into CMake

**Created DeepCBlur DeepFilterOp with 2D Gaussian blur, depth propagation, per-pixel sample optimization, and CMake build integration**

## What Happened

Implemented `src/DeepCBlur.cpp` as a direct `DeepFilterOp` subclass following the patterns from DeepCKeymix (class layout, knobs, getDeepRequests with RequestData), DeepThinner (DeepOutputPlane + DeepOutPixel + addPixel/addHole, thread_local ScratchBuf), and DeepCAdjustBBox (bbox expansion in _validate).

The plugin computes a full 2D Gaussian kernel from blur_width/blur_height parameters (sigma = radius/3), iterates over the kernel neighbourhood for each output pixel, collects weighted samples (depth channels propagated raw, colour/alpha weighted by kernel value), runs deepc::optimizeSamples() for per-pixel merge and cap, and emits the surviving samples via DeepOutPixel. Empty source pixels in the kernel footprint are skipped. A zero-blur fast path passes through input unchanged.

Updated `src/CMakeLists.txt` to add DeepCBlur to both the PLUGINS list (for compilation) and FILTER_NODES list (for Nuke's Deep menu).

## Verification

All 12 task-level and 8 slice-level grep checks pass, confirming: class exists as DeepFilterOp subclass, includes shared optimizer header, has separate blur_width/blur_height knobs, contains Op::aborted() check, uses thread_local scratch buffers, has Op::Description registration, is wired into CMakeLists PLUGINS and FILTER_NODES, kernel is normalized, failure propagation present, and optimizer header remains DDImage-free.

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `grep -q "class DeepCBlur" src/DeepCBlur.cpp` | 0 | ✅ pass | <1s |
| 2 | `grep -q '#include "DeepSampleOptimizer.h"' src/DeepCBlur.cpp` | 0 | ✅ pass | <1s |
| 3 | `grep -q "blur_width" && grep -q "blur_height" src/DeepCBlur.cpp` | 0 | ✅ pass | <1s |
| 4 | `grep -q "Op::aborted()" src/DeepCBlur.cpp` | 0 | ✅ pass | <1s |
| 5 | `grep -q "thread_local" src/DeepCBlur.cpp` | 0 | ✅ pass | <1s |
| 6 | `grep -q "DeepCBlur" src/CMakeLists.txt` | 0 | ✅ pass | <1s |
| 7 | `grep "FILTER_NODES" src/CMakeLists.txt \| grep -q "DeepCBlur"` | 0 | ✅ pass | <1s |
| 8 | `grep -q "optimizeSamples" src/DeepSampleOptimizer.h` | 0 | ✅ pass | <1s |
| 9 | `! grep -q "DDImage" src/DeepSampleOptimizer.h` | 0 | ✅ pass | <1s |
| 10 | `grep -q "return false" src/DeepCBlur.cpp` | 0 | ✅ pass | <1s |
| 11 | `grep -q "Op::Description" src/DeepCBlur.cpp` | 0 | ✅ pass | <1s |
| 12 | `grep -q "kernelSum" src/DeepCBlur.cpp` | 0 | ✅ pass | <1s |

## Diagnostics

- **Compile check:** `cmake --build build/` — will surface missing includes, type errors, linker issues
- **Standalone syntax check:** `g++ -std=c++17 -fsyntax-only -I/path/to/nuke/include src/DeepCBlur.cpp` (requires Nuke SDK headers)
- **Runtime inspection:** In Nuke, create DeepCBlur node — blur_width, blur_height, max_samples, merge_tolerance, color_tolerance knobs visible in properties panel
- **Failure state:** doDeepEngine returns false on input failure or Op::aborted(), producing red error badge on node

## Deviations

- Added `_colorTolerance` member and `color_tolerance` knob — the plan mentioned it in step 1 but didn't list it explicitly as a must-have. Added for parity with DeepSampleOptimizer.h's colorTolerance parameter.
- Added zero-blur passthrough fast path (radX==0 && radY==0) — not in the plan but essential to avoid empty kernel computation when blur parameters are zero.

## Known Issues

- Full 2D (non-separable) kernel is O(radius²) per source sample — large radii (>10) will be slow. Plan notes this is intentional for v1.
- Cannot verify actual compilation without Nuke SDK headers present on this machine — cmake --build requires DDImage headers.

## Files Created/Modified

- `src/DeepCBlur.cpp` — Complete DeepFilterOp subclass with 2D Gaussian blur, sample propagation, per-pixel optimization
- `src/CMakeLists.txt` — Added DeepCBlur to PLUGINS and FILTER_NODES lists
