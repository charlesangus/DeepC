# S01: DeepCBlur core implementation

**Goal:** DeepCBlur.cpp compiles with local CMake, loads in Nuke 16+, and produces Gaussian-blurred deep output with sample propagation and per-pixel optimization. DeepSampleOptimizer.h exists as a shared header.
**Demo:** `cmake --build build/` succeeds producing DeepCBlur.so. The plugin registers as "DeepCBlur" with blur_width, blur_height, max_samples, and merge_tolerance knobs. DeepSampleOptimizer.h is a standalone header-only utility included by DeepCBlur.cpp.

## Must-Haves

- DeepSampleOptimizer.h: standalone header-only with `optimizeSamples()` — merges nearby-depth samples (front-to-back over-compositing) and caps at max_samples (R005)
- DeepCBlur.cpp: direct DeepFilterOp subclass with separate blur_width/blur_height float knobs (R001, R002)
- Full 2D Gaussian kernel blur in doDeepEngine() — gathers weighted samples from all kernel-radius neighbors (R001)
- Sample propagation creates new samples in previously-empty neighboring pixels (R003)
- Per-pixel optimization via DeepSampleOptimizer runs inside doDeepEngine() before output (R004)
- Depth channels (Chan_DeepFront, Chan_DeepBack) propagate from source — NOT blurred (D005)
- All color/alpha channels blurred uniformly — no channel selector (D004)
- Output bbox expanded by blur radius in _validate() so downstream nodes see the larger output
- getDeepRequests() pads input box by kernel radius
- Op::Description registration so the node appears in Nuke
- CMakeLists.txt updated: DeepCBlur in PLUGINS list and FILTER_NODES list
- thread_local scratch buffers for per-pixel accumulation (thread safety)
- Abort check in pixel loop (`Op::aborted()`)

## Proof Level

- This slice proves: contract (compiles, links, registers correctly; functional verification requires Nuke runtime)
- Real runtime required: yes (for functional verification beyond compilation — deferred to UAT)
- Human/UAT required: yes (visual inspection of blurred output in Nuke viewer)

## Verification

- `cd /home/latuser/git/DeepC/.gsd/worktrees/M002 && grep -q "optimizeSamples" src/DeepSampleOptimizer.h` — shared header exists with the core function
- `cd /home/latuser/git/DeepC/.gsd/worktrees/M002 && grep -q "class DeepCBlur" src/DeepCBlur.cpp` — plugin source exists
- `cd /home/latuser/git/DeepC/.gsd/worktrees/M002 && grep -q "DeepCBlur" src/CMakeLists.txt` — build integration done
- `cd /home/latuser/git/DeepC/.gsd/worktrees/M002 && grep -q '#include "DeepSampleOptimizer.h"' src/DeepCBlur.cpp` — blur uses shared optimizer
- `cd /home/latuser/git/DeepC/.gsd/worktrees/M002 && grep -q "blur_width" src/DeepCBlur.cpp && grep -q "blur_height" src/DeepCBlur.cpp` — separate size knobs
- `cd /home/latuser/git/DeepC/.gsd/worktrees/M002 && grep -q "Op::aborted()" src/DeepCBlur.cpp` — abort check present
- `cd /home/latuser/git/DeepC/.gsd/worktrees/M002 && grep -q "thread_local" src/DeepCBlur.cpp` — thread-safe scratch buffers
- `cd /home/latuser/git/DeepC/.gsd/worktrees/M002 && ! grep -q "DDImage" src/DeepSampleOptimizer.h` — optimizer header has zero DDImage dependencies (failure-path: if DDImage leaks in, standalone testability breaks)

## Observability / Diagnostics

- **Compile-time diagnostics:** `cmake --build build/` emits compiler warnings/errors — first signal of breakage. Build logs capture missing includes, type mismatches, and linker failures.
- **Runtime failure visibility:** DeepCBlur inherits DeepFilterOp error propagation — if doDeepEngine() returns false, Nuke surfaces a red error badge on the node. Op::aborted() checks produce immediate cancellation feedback in Nuke's progress bar.
- **Inspection surfaces:** Nuke's node properties panel exposes blur_width, blur_height, max_samples, merge_tolerance knobs. Deep sample counts before/after optimization can be inspected via DeepExpression or Python `nuke.toNode('DeepCBlur1').deepSampleCount()`.
- **Header-only testability:** DeepSampleOptimizer.h can be compiled and tested in isolation with a trivial test harness (no Nuke required) to verify merge/cap logic correctness.
- **Failure-path verification:** `grep -q "return false" src/DeepCBlur.cpp` — confirms deepEngine failure propagation is present.

## Integration Closure

- Upstream surfaces consumed: Nuke 16 DDImage SDK (`DeepFilterOp.h`, `DeepPixel.h`, `DeepPlane.h`, `Knobs.h`)
- New wiring introduced in this slice: `src/DeepCBlur.cpp` (new plugin), `src/DeepSampleOptimizer.h` (new shared header), CMakeLists.txt entries
- What remains before the milestone is truly usable end-to-end: S02 — docker-build.sh integration, menu registration via FILTER_NODES substitution, README update, icon

## Tasks

- [x] **T01: Implement DeepSampleOptimizer.h shared header** `est:1h`
  - Why: The sample optimization logic (merge nearby depths + max_samples cap) is the highest-risk shared component (R005). Building it first validates merge semantics independently and unblocks the blur plugin to simply call it.
  - Files: `src/DeepSampleOptimizer.h`
  - Do: Create a header-only utility with `SampleRecord` struct and `optimizeSamples()` function. Extract merge logic from DeepThinner.cpp Pass 6 (Smart Merge) and Pass 7 (Max Samples). Must handle front-to-back over-compositing for merged channels. Parameters: merge_tolerance, color_tolerance, max_samples. Use `inline` functions for ODR safety. Include depth-sort, alive-marking, group formation, and merged channel emission. C++17, no external deps beyond `<vector>`, `<algorithm>`, `<cmath>`.
  - Verify: `test -f src/DeepSampleOptimizer.h && grep -q "optimizeSamples" src/DeepSampleOptimizer.h && grep -q "SampleRecord" src/DeepSampleOptimizer.h`
  - Done when: `src/DeepSampleOptimizer.h` exists as a self-contained header with complete merge + cap logic, compilable standalone (no DDImage dependency in the header itself)

- [x] **T02: Implement DeepCBlur.cpp plugin and wire into CMake** `est:2h`
  - Why: This is the core plugin implementing the Gaussian blur, sample propagation, and per-pixel optimization (R001–R004). CMake integration ensures it compiles as part of the build.
  - Files: `src/DeepCBlur.cpp`, `src/CMakeLists.txt`
  - Do: Create DeepCBlur as a direct DeepFilterOp subclass. Knobs: blur_width (float, default 1), blur_height (float, default 1), max_samples (int, default 100), merge_tolerance (float, default 0.001). _validate(): call base, expand _deepInfo.box() by ceil(3*sigma) in each direction. getDeepRequests(): pad box by kernel radius, request all channels + Mask_Deep. doDeepEngine(): fetch expanded DeepPlane, pre-compute normalized 2D Gaussian kernel, iterate output pixels — for each pixel gather all samples from kernel footprint neighbors, weight color/alpha channels by kernel value, propagate depth front/back from source, run optimizeSamples() per pixel, emit via DeepOutPixel+addPixel/addHole. Use thread_local scratch buffers. Add Op::aborted() check. Register with Op::Description. Add DeepCBlur to PLUGINS list and FILTER_NODES in src/CMakeLists.txt.
  - Verify: `grep -q "DeepCBlur" src/CMakeLists.txt && grep -q "class DeepCBlur" src/DeepCBlur.cpp && grep -q "thread_local" src/DeepCBlur.cpp && grep -q "Op::aborted" src/DeepCBlur.cpp`
  - Done when: `src/DeepCBlur.cpp` is a complete DeepFilterOp subclass with Gaussian blur + sample propagation + per-pixel optimization, and `src/CMakeLists.txt` has DeepCBlur in both PLUGINS and FILTER_NODES lists

## Files Likely Touched

- `src/DeepSampleOptimizer.h` (new)
- `src/DeepCBlur.cpp` (new)
- `src/CMakeLists.txt` (modified)
