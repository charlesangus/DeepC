---
id: S01
parent: M002
milestone: M002
provides:
  - DeepCBlur.cpp — complete DeepFilterOp subclass with 2D Gaussian blur, sample propagation, per-pixel optimization
  - DeepSampleOptimizer.h — standalone header-only merge+cap utility in deepc namespace
  - CMakeLists.txt entries for PLUGINS and FILTER_NODES
requires:
  - slice: none
    provides: first slice in M002
affects:
  - S02
key_files:
  - src/DeepSampleOptimizer.h
  - src/DeepCBlur.cpp
  - src/CMakeLists.txt
key_decisions:
  - Full 2D non-separable Gaussian kernel (not separable passes) — simpler for variable-length sample arrays per pixel
  - Depth channels propagated raw from source, never Gaussian-weighted
  - Generalized DeepThinner's fixed rgb[3] to std::vector<float> channels for arbitrary channel sets in optimizer
  - Zero-blur passthrough fast path when both radii are 0
  - Added color_tolerance knob for parity with optimizer's colorTolerance parameter
patterns_established:
  - deepc namespace for shared non-DDImage utilities
  - Header-only inline pattern for cross-plugin reuse (DeepSampleOptimizer.h)
  - DeepFilterOp subclass with bbox expansion in _validate and padded input in getDeepRequests
  - thread_local ScratchBuf pattern reused from DeepThinner for zero-allocation hot path
  - SampleRecord channels vector stores all channel data including depth positions for emit phase
observability_surfaces:
  - Compile-time: cmake --build build/ surfaces missing includes, type mismatches, linker failures
  - Runtime: Nuke red error badge if doDeepEngine returns false; Op::aborted() cancellation feedback
  - Inspection: blur_width, blur_height, max_samples, merge_tolerance, color_tolerance visible in node properties
  - Standalone compile: g++ -std=c++17 -fsyntax-only -I. src/DeepSampleOptimizer.h verifies no DDImage leakage
drill_down_paths:
  - .gsd/milestones/M002/slices/S01/tasks/T01-SUMMARY.md
  - .gsd/milestones/M002/slices/S01/tasks/T02-SUMMARY.md
duration: 23min
verification_result: passed
completed_at: 2026-03-20
---

# S01: DeepCBlur core implementation

**Standalone DeepSampleOptimizer.h shared header and complete DeepCBlur DeepFilterOp plugin with 2D Gaussian blur, sample propagation, per-pixel optimization, and CMake build integration**

## What Happened

Built the core DeepCBlur plugin in two tasks. T01 extracted and generalized the merge logic from DeepThinner.cpp (Pass 6 Smart Merge + Pass 7 Max Samples) into a standalone, DDImage-free header `DeepSampleOptimizer.h`. The key generalization replaced DeepThinner's fixed `float rgb[3]` with `std::vector<float> channels` so any channel set can be optimized. The header lives in `namespace deepc` with all functions `inline` for ODR safety. A standalone compile test + unit test harness validated merge math (front-to-back over-compositing: `alpha_acc += alpha * (1 - alpha_acc)`).

T02 then built `DeepCBlur.cpp` as a direct `DeepFilterOp` subclass following patterns from DeepCKeymix (class layout, knobs, getDeepRequests with RequestData), DeepThinner (DeepOutputPlane + DeepOutPixel + addPixel/addHole, thread_local ScratchBuf), and DeepCAdjustBBox (bbox expansion in _validate). The plugin computes a full 2D Gaussian kernel from blur_width/blur_height (sigma = radius/3), gathers weighted samples from the kernel footprint, propagates depth channels raw from source, weights color/alpha by kernel value, runs `deepc::optimizeSamples()` per pixel, and emits survivors. A zero-blur fast path passes through input unchanged. CMakeLists.txt was updated with DeepCBlur in both PLUGINS and FILTER_NODES lists.

## Verification

All 12 slice-level verification checks passed:
1. `optimizeSamples` present in DeepSampleOptimizer.h
2. `class DeepCBlur` defined in DeepCBlur.cpp
3. DeepCBlur in CMakeLists.txt
4. DeepCBlur.cpp includes DeepSampleOptimizer.h
5. Separate blur_width/blur_height knobs present
6. Op::aborted() check present
7. thread_local scratch buffers present
8. No DDImage dependency in optimizer header
9. FILTER_NODES contains DeepCBlur
10. Op::Description registration present
11. `return false` failure propagation present
12. kernelSum normalization present

T01 additionally ran a standalone g++ compile + unit test harness exercising empty input, single sample passthrough, non-merge, merge with over-compositing math verification, and max_samples cap. All passed.

Compilation against Nuke SDK headers cannot be verified in this environment (requires DDImage headers). Deferred to S02's docker-build.sh verification.

## New Requirements Surfaced

- none

## Deviations

- Added `color_tolerance` knob not explicitly listed in the plan's must-haves, but required for parity with DeepSampleOptimizer.h's colorTolerance parameter.
- Added zero-blur passthrough fast path (radX==0 && radY==0) — not in the plan but essential to avoid empty kernel computation.
- Simplified merge implementation vs DeepThinner's alive-marking approach — used fresh `merged` vector with move-assign instead of in-place alive/dead tracking.

## Known Limitations

- Full 2D non-separable kernel is O(radius²) per source sample — large radii (>10) will be slow. Intentional for v1; separable decomposition deferred.
- Cannot verify actual Nuke SDK compilation in this environment — cmake --build requires DDImage headers on the build machine. Docker build in S02 will prove this.
- Runtime behavior (correct visual output, sample propagation into empty pixels) requires Nuke runtime — deferred to UAT with human verification.

## Follow-ups

- S02 must verify docker-build.sh produces DeepCBlur.so/.dll for Linux and Windows
- S02 must add DeepCBlur.png icon (placeholder or proper)
- S02 must update README.md to reflect 24 plugins
- UAT should verify: sparse deep image blurs visibly into empty pixels, dense deep image composites correctly after blur+flatten, max_samples cap reduces output counts

## Files Created/Modified

- `src/DeepSampleOptimizer.h` — new header-only utility with SampleRecord struct, colorDistance helper, optimizeSamples function (deepc namespace)
- `src/DeepCBlur.cpp` — complete DeepFilterOp subclass with 2D Gaussian blur, sample propagation, per-pixel optimization
- `src/CMakeLists.txt` — DeepCBlur added to PLUGINS and FILTER_NODES lists

## Forward Intelligence

### What the next slice should know
- DeepCBlur is already in both PLUGINS and FILTER_NODES in CMakeLists.txt. S02 only needs to verify docker-build.sh picks it up — no CMakeLists changes needed unless the string(REPLACE) pattern for FILTER_NODES doesn't match.
- The Op::Description class string determines toolbar placement. Check the first arg to `Op::Description(...)` in DeepCBlur.cpp to confirm it matches the menu category expected by menu.py.in.

### What's fragile
- The kernel normalization (`kernelSum`) — if the Gaussian kernel weights don't sum to 1.0 due to floating-point or edge clamping, color will shift. Verify with a uniform deep image (all pixels same color) that blur produces the same color in the center.
- DDImage API surface area — DeepCBlur uses `DeepPlane::getPixel()`, `DeepPixel::getSample()`, `DeepOutPixel`, `addPixel`/`addHole`. If any of these differ between Nuke versions, the docker build will catch it.

### Authoritative diagnostics
- `g++ -std=c++17 -fsyntax-only -I. src/DeepSampleOptimizer.h` — proves optimizer header compiles standalone with zero DDImage deps
- `cmake --build build/` in docker — the definitive compilation proof for DeepCBlur.cpp against actual Nuke SDK headers

### What assumptions changed
- Original plan considered separable Gaussian as a possibility — confirmed full 2D kernel is the right choice for v1 given variable-length sample arrays per pixel make horizontal/vertical decomposition complex.
