---
estimated_steps: 5
estimated_files: 2
---

# T02: Implement DeepCBlur.cpp plugin and wire into CMake

**Slice:** S01 — DeepCBlur core implementation
**Milestone:** M002

## Description

Create `src/DeepCBlur.cpp` — a direct `DeepFilterOp` subclass that applies a 2D Gaussian blur to deep images with sample propagation into empty pixels and per-pixel sample optimization. Wire it into `src/CMakeLists.txt` so it compiles as part of the build.

This is the core plugin file implementing requirements R001 (Gaussian blur on deep images), R002 (separate width/height knobs), R003 (sample propagation), and R004 (per-pixel optimization).

**Key reference files the executor must read:**
- `src/DeepCKeymix.cpp` — canonical DeepFilterOp subclass pattern (class layout, knobs(), _validate(), getDeepRequests() with RequestData, doDeepEngine())
- `src/DeepThinner.cpp` — variable-sample output via DeepOutputPlane + DeepOutPixel + addPixel/addHole, thread_local ScratchBuf pattern, channel iteration with `foreach(z, channels)`
- `src/DeepCAdjustBBox.cpp` — _deepInfo.box().set() for bbox expansion in _validate()
- `src/DeepSampleOptimizer.h` — the shared header from T01, include and call `deepc::optimizeSamples()`
- `src/CMakeLists.txt` — where to add DeepCBlur to PLUGINS and FILTER_NODES lists

**Architecture decisions (non-negotiable):**
- Direct DeepFilterOp subclass (D001) — NOT DeepPixelOp or DeepCWrapper
- Gaussian only (D002), separate width/height floats (D003)
- All channels blurred uniformly, no channel selector (D004)
- Depth channels propagate from source, NOT blurred (D005)
- Per-pixel optimization inside doDeepEngine (D006)
- Full 2D (non-separable) Gaussian kernel for v1

## Steps

1. **Create class skeleton** in `src/DeepCBlur.cpp`:
   - Includes: `DDImage/DeepFilterOp.h`, `DDImage/DeepPixel.h`, `DDImage/DeepPlane.h`, `DDImage/Knobs.h`, `"DeepSampleOptimizer.h"`, `<cmath>`, `<vector>`, `<algorithm>`
   - Class `DeepCBlur : public DeepFilterOp` with member variables:
     - `float _blurWidth` (default 1.0), `float _blurHeight` (default 1.0)
     - `int _maxSamples` (default 100), `float _mergeTolerance` (default 0.001f), `float _colorTolerance` (default 0.01f)
   - Constructor, `Class()` returns `"DeepCBlur"`, `node_help()` with description
   - `knobs()`: `Float_knob` for blur_width and blur_height with range 0–100, tooltip warning for values > 10. `Int_knob` for max_samples. `Float_knob` for merge_tolerance and color_tolerance.
   - `Op::Description` registration: `"DeepCBlur"`, `"Deep/DeepCBlur"`, build function
   - Thread-local scratch struct similar to DeepThinner's ScratchBuf

2. **Implement `_validate()`:**
   - Call `DeepFilterOp::_validate(for_real)`
   - Compute `sigmaX = _blurWidth / 3.0` and `sigmaY = _blurHeight / 3.0` (blur_width/height = 3-sigma radius)
   - Compute `kernelRadiusX = (int)ceil(_blurWidth)` and `kernelRadiusY = (int)ceil(_blurHeight)` — or use `ceil(3 * sigma)` if sigma-based
   - NOTE: Use the convention that blur_width IS the pixel radius directly: `sigmaX = _blurWidth / 3.0`, `kernelRadiusX = (int)ceil(_blurWidth)`. This means blur_width=5 gives a 11×11 kernel (±5 pixels)
   - Expand `_deepInfo.box()` by kernelRadius in each direction using `.set(x-radX, y-radY, r+radX, t+radY)`

3. **Implement `getDeepRequests()`:**
   - Compute kernel radius same as _validate
   - Pad the incoming box: `box.pad(kernelRadiusX, kernelRadiusY)` or manually expand
   - Request all channels including `Chan_DeepFront`, `Chan_DeepBack`, `Chan_Alpha`
   - Push `RequestData(input0(), paddedBox, requestChannels, count)`

4. **Implement `doDeepEngine()`:**
   - Fetch expanded input plane from input0 — request `box.pad(kernelRadius)` region
   - Pre-compute normalized 2D Gaussian kernel: `kernel[dx][dy] = gauss(dx, sigmaX) * gauss(dy, sigmaY)`, normalize to sum=1.0. Store as `std::vector<float>` indexed by `(dy + radiusY) * kernelWidth + (dx + radiusX)`. Compute once per doDeepEngine call.
   - Create output: `plane = DeepOutputPlane(channels, box, DeepPixel::eUnordered)`
   - Use `static thread_local` scratch buffers (vector of SampleRecord, DeepOutPixel)
   - For each output pixel (y, x) in box:
     - Check `Op::aborted()` — return false if aborted
     - Clear scratch sample accumulator
     - For each kernel offset (dy in -radiusY..+radiusY, dx in -radiusX..+radiusX):
       - Source pixel = (x+dx, y+dy). Skip if outside input plane's bbox.
       - Get `DeepPixel` from input plane. Skip if sample count = 0.
       - `float weight = kernel[(dy+radiusY)*kernelWidth + (dx+radiusX)]`
       - For each sample s in source pixel:
         - Create a `deepc::SampleRecord`: zFront/zBack from source (propagated as-is), alpha = source_alpha * weight, channels = each source channel value * weight (except DeepFront/DeepBack which are copied raw)
         - Push to scratch accumulator
     - If no samples accumulated → `plane.addHole(); continue;`
     - Call `deepc::optimizeSamples(scratchSamples, _mergeTolerance, _colorTolerance, _maxSamples)`
     - Emit: build `DeepOutPixel`, for each surviving sample push channel values in `foreach(z, channels)` order — DeepFront→zFront, DeepBack→zBack, other channels from the sample's channels vector
     - `plane.addPixel(outPixel)`
   - Return true

5. **Update `src/CMakeLists.txt`:**
   - Add `DeepCBlur` to the `PLUGINS` list (it's a non-wrapped plugin, direct DeepFilterOp)
   - Add `DeepCBlur` to the `FILTER_NODES` list (alongside DeepThinner)

## Must-Haves

- [ ] DeepCBlur is a direct DeepFilterOp subclass (not DeepPixelOp, not DeepCWrapper)
- [ ] Separate blur_width and blur_height float knobs
- [ ] _validate() expands _deepInfo.box() by kernel radius
- [ ] getDeepRequests() pads input box by kernel radius
- [ ] doDeepEngine() applies full 2D Gaussian kernel with sample propagation
- [ ] Depth channels (Chan_DeepFront, Chan_DeepBack) propagate from source — NOT multiplied by kernel weight
- [ ] Color/alpha channels weighted by Gaussian kernel value
- [ ] Empty pixels in kernel footprint are skipped (no contribution)
- [ ] Per-pixel optimizeSamples() called before output emission
- [ ] thread_local scratch buffers for thread safety
- [ ] Op::aborted() check in pixel loop
- [ ] Op::Description registration
- [ ] DeepCBlur added to PLUGINS and FILTER_NODES in CMakeLists.txt
- [ ] Gaussian kernel normalized to sum=1.0

## Verification

- `grep -q "class DeepCBlur" src/DeepCBlur.cpp` — plugin class exists
- `grep -q '#include "DeepSampleOptimizer.h"' src/DeepCBlur.cpp` — uses shared optimizer
- `grep -q "blur_width" src/DeepCBlur.cpp && grep -q "blur_height" src/DeepCBlur.cpp` — separate knobs
- `grep -q "Op::aborted" src/DeepCBlur.cpp` — abort check
- `grep -q "thread_local" src/DeepCBlur.cpp` — thread safety
- `grep -q "DeepCBlur" src/CMakeLists.txt` — build integration
- `grep "FILTER_NODES" src/CMakeLists.txt | grep -q "DeepCBlur"` — menu integration

## Inputs

- `src/DeepSampleOptimizer.h` — shared header from T01 providing deepc::SampleRecord and deepc::optimizeSamples()
- `src/DeepCKeymix.cpp` — reference for DeepFilterOp subclass pattern
- `src/DeepThinner.cpp` — reference for DeepOutputPlane + DeepOutPixel output, thread_local scratch, channel iteration
- `src/DeepCAdjustBBox.cpp` — reference for bbox expansion in _validate()
- `src/CMakeLists.txt` — build file to modify

## Expected Output

- `src/DeepCBlur.cpp` — complete DeepFilterOp subclass with Gaussian blur, sample propagation, per-pixel optimization
- `src/CMakeLists.txt` — modified with DeepCBlur in PLUGINS and FILTER_NODES lists

## Observability Impact

- **New compile target:** `cmake --build build/` now compiles DeepCBlur.so — compiler warnings/errors are the first signal of breakage.
- **Runtime failure surface:** doDeepEngine() returns false on input failure or Op::aborted(), producing Nuke's red error badge on the node.
- **Knob inspection:** blur_width, blur_height, max_samples, merge_tolerance, color_tolerance exposed in Nuke's node properties panel.
- **Diagnostic grep:** `grep -q "return false" src/DeepCBlur.cpp` confirms failure propagation paths exist.
- **Menu visibility:** DeepCBlur appears under Deep > Filter in Nuke's menu after FILTER_NODES registration.
