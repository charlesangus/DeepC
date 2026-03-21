# S01 — Research

**Date:** 2026-03-20

## Summary

DeepCBlur is a direct `DeepFilterOp` subclass that applies a Gaussian blur to deep images with sample propagation into empty pixels and built-in per-pixel sample optimization. The codebase has well-established patterns for every piece of this implementation: `DeepCKeymix.cpp` is the canonical reference for direct `DeepFilterOp` subclasses (knobs, `_validate`, `getDeepRequests`, `doDeepEngine` with `DeepInPlaceOutputPlane`), `DeepThinner.cpp` demonstrates variable-sample-count output via `DeepOutputPlane` + `DeepOutPixel` + `addPixel`/`addHole`, and `DeepCAdjustBBox.cpp` shows bounding-box expansion in `_validate`. The sample optimization logic in DeepThinner (specifically its Pass 6 "Smart Merge" and Pass 7 "Max Samples") provides a direct reference for the `DeepSampleOptimizer.h` shared header.

The primary recommendation is a full 2D (non-separable) Gaussian kernel for v1. The separable approach (horizontal then vertical) is theoretically faster but requires materializing an intermediate deep image with variable sample counts per pixel between passes — adding significant complexity with unclear benefit given that max_samples will cap counts anyway. The full 2D kernel approach is simpler, correct, and matches the M001 research guidance that separable decomposition may not be worth the complexity for v1.

The riskiest aspect is sample-merging semantics: when a blur kernel gathers samples from multiple neighboring source pixels at different depths, the output pixel accumulates all contributed samples weighted by Gaussian kernel values. The shared `DeepSampleOptimizer` then merges nearby-depth samples and caps at max_samples. This two-phase approach (gather-all → optimize) keeps the blur logic clean and the optimization reusable. The approach is proven viable by DeepThinner's existing merge logic.

## Recommendation

Build the slice in three logical units in this order:

1. **DeepSampleOptimizer.h** — standalone header implementing `optimizeSamples()` that takes a vector of sample records (depth front/back + all channel values) and returns a compacted vector. Implements two passes: merge samples within a depth tolerance (using front-to-back over-compositing, same as DeepThinner Pass 6) and cap at max_samples (drop deepest, same as DeepThinner Pass 7). Build and verify this first because it is the highest-risk shared component and is independently testable.

2. **DeepCBlur.cpp** — direct `DeepFilterOp` subclass. `_validate()` expands `_deepInfo.box()` by blur radius. `getDeepRequests()` pads the requested Box by blur radius. `doDeepEngine()` fetches expanded input plane, applies 2D Gaussian kernel per output pixel by gathering weighted samples from all kernel-radius neighbors, runs `optimizeSamples()` per pixel, emits output via `DeepOutputPlane` + `DeepOutPixel` + `addPixel`/`addHole`.

3. **Verification** — local CMake build, manual Nuke 16 load test.

## Implementation Landscape

### Key Files

- `src/DeepCKeymix.cpp` — **Primary reference.** Direct `DeepFilterOp` subclass with multi-input handling, `getDeepRequests()` with `RequestData`, `doDeepEngine()` using `DeepInPlaceOutputPlane`. Follow its structure for class layout, `knobs()`, `_validate()`, Op::Description registration.
- `src/DeepThinner.cpp` — **Sample optimization reference.** `SampleRecord` struct, depth-sorted sample collection, `scratch.alive[]` pass system, Pass 6 "Smart Merge" (Z + color tolerances, front-to-back over-compositing for merged channels), Pass 7 "Max Samples" (truncate groups). Also demonstrates variable-count output via `DeepOutputPlane(channels, box, DeepPixel::eUnordered)` + `DeepOutPixel` + `addPixel()`/`addHole()`.
- `src/DeepCWrapper.cpp` — **Processing loop reference.** Shows the abort-check pattern (`if (Op::aborted()) return false;` inside the pixel loop), `foreach(z, channels)` iteration, `colourIndex(z)`, alpha handling, side mask pattern. DeepCBlur won't inherit from this but should replicate its abort-check discipline.
- `src/DeepCAdjustBBox.cpp` — **BBox expansion reference.** Shows `_deepInfo.box().set(...)` in `_validate()` to expand the output bounding box. DeepCBlur needs this to declare an expanded output bbox.
- `src/DeepCConstant.cpp` — **DeepInfo construction reference.** Shows creating `_deepInfo = DeepInfo(formats, box, new_channelset)` with expanded channel set including `Mask_Deep`.
- `src/CMakeLists.txt` — **Build integration.** DeepCBlur goes into the `PLUGINS` list (non-wrapped). `FILTER_NODES` list updated. `add_nuke_plugin()` function handles MODULE target creation.
- `python/menu.py.in` — **Menu template.** No changes needed — `@FILTER_NODES@` variable is already substituted from CMake. Adding to `FILTER_NODES` in CMakeLists.txt is sufficient.

### Files to Create

- `src/DeepSampleOptimizer.h` — New shared header. Header-only with `optimizeSamples()` function taking a `std::vector<SampleRecord>` (or similar), merge tolerance, color tolerance, and max_samples parameters. Returns optimized vector.
- `src/DeepCBlur.cpp` — New plugin source. Direct `DeepFilterOp` subclass.

### Build Order

**1. DeepSampleOptimizer.h first** — This is the shared reusable component (R005). Building it first:
- Lets us validate the merge semantics independently
- Unblocks DeepCBlur to just call `optimizeSamples()` 
- Makes the DeepCBlur code simpler and more focused
- The struct and merge logic are directly derivable from DeepThinner's `SampleRecord`, Pass 6, and Pass 7

**2. DeepCBlur.cpp second** — Depends on the optimizer header. Key implementation order within the file:
- Class skeleton with knobs (`blur_width`, `blur_height`, `max_samples`, `merge_tolerance`) 
- `_validate()` — expand `_deepInfo.box()` by `ceil(3*sigma)` in each direction (3-sigma Gaussian coverage)
- `getDeepRequests()` — pad input box by kernel radius
- `doDeepEngine()` — the core Gaussian blur with sample propagation:
  1. Fetch expanded `DeepPlane` from input
  2. For each output pixel: iterate kernel footprint, gather all samples from source pixels, weight channel values by 2D Gaussian, propagate depth front/back from source
  3. Run `optimizeSamples()` on gathered samples
  4. Emit via `DeepOutPixel` + `addPixel()`
- Op::Description registration

**3. CMake integration** — Add `DeepCBlur` to `PLUGINS` list and `FILTER_NODES` in `src/CMakeLists.txt`

### Verification Approach

1. **Compile test:** `cmake --build build/` succeeds with DeepCBlur.so produced
2. **Load test:** Nuke 16 loads DeepCBlur.so without errors; node appears in DeepC > Filter menu
3. **Functional test — sparse input:** Connect DeepCConstant (1 sample, small bbox like 10x10) → DeepCBlur (blur_width=5, blur_height=5) → DeepToImage. Verify:
   - Output bbox is larger than input bbox (propagation into empty pixels)
   - DeepToImage shows a blurred result (not a hard-edged rectangle)
   - Viewing deep data shows new samples exist in pixels outside the original constant's bbox
4. **Functional test — dense input:** Connect a rendered deep EXR with overlapping objects → DeepCBlur → DeepToImage. Verify:
   - Output visually shows blur
   - Deep sample counts are bounded by max_samples knob
   - Adjusting max_samples changes output sample counts
5. **Functional test — optimization:** Compare sample counts before/after with and without max_samples. Verify merge_tolerance collapses nearby-depth samples.

## Constraints

- **C++17 / GCC 11.2.1 / `_GLIBCXX_USE_CXX11_ABI=1`** — mandatory ABI for Nuke 16+. All code must compile under these constraints. Use `std::vector`, `<cmath>`, `<algorithm>` — no C++20 features.
- **DeepFilterOp base class only** — `DeepPixelOp` explicitly prohibits spatial operations ("each pixel on the input corresponds exactly to a pixel on the output"). `DeepCWrapper` fetches only the output box, can't expand input requests. Decision D001 confirms this.
- **Depth channels are NOT blurred** — `Chan_DeepFront` and `Chan_DeepBack` propagate from source samples with their original values. Blurring depth values would produce incorrect compositing. Only color/alpha channels receive Gaussian weighting.
- **All channels blurred uniformly** — Decision D004: no channel selector knob. Deep images carry all channels per-sample; blurring a subset creates inconsistency.
- **No thread_local assumption for DeepOutPixel** — DeepThinner uses `thread_local ScratchBuf`. DeepCBlur should follow the same pattern for its per-pixel accumulation buffers to ensure thread safety (Nuke calls `doDeepEngine` from multiple threads concurrently for different tiles).
- **DeepSampleOptimizer.h must be header-only** — to be `#include`-able by future plugins without adding object library dependencies.

## Common Pitfalls

- **`getDeepRequests()` Box must cover the full kernel radius** — If the expanded box is too small, edge pixels will have a truncated kernel and the blur will have visible banding at tile boundaries. Use `box.pad(kernelRadius)` where `kernelRadius = ceil(3 * sigma)`.
- **Channel set mismatch between `getDeepRequests()` and `doDeepEngine()`** — If `doDeepEngine()` tries to read channels not requested in `getDeepRequests()`, values will be zero. Extract a `neededChannels()` helper and call it from both methods. Must include `Chan_DeepFront`, `Chan_DeepBack`, `Chan_Alpha`, and all requested output channels.
- **Missing abort check in the pixel loop** — `doDeepEngine()` iterates W×H×kernelArea×samples. Without `if (Op::aborted()) return false;` checks inside the outer pixel loop, large blur operations cannot be cancelled by the user.
- **Gaussian kernel must be normalized** — The sum of all kernel weights must equal 1.0. Pre-compute the kernel once per doDeepEngine call (it doesn't change per pixel), store in a vector. For non-uniform (width ≠ height), compute a separable 2D kernel as `kernel[dx][dy] = gauss1d(dx, sigmaX) * gauss1d(dy, sigmaY)`, then normalize the full 2D kernel to sum to 1.0.
- **Empty source pixels in kernel footprint** — When the kernel covers pixels outside the input bbox, those pixels have zero samples. They should contribute nothing to the output (skip them), but the kernel normalization must still use the full kernel weight (not renormalize to only valid pixels) to avoid brightness changes at edges. Exception: pixels entirely outside the deep data should produce zero output, handled naturally by the addHole() path.
- **Heap allocation in the hot loop** — Allocating `std::vector` per pixel is expensive. Use `thread_local` scratch buffers (following DeepThinner's `ScratchBuf` pattern) and `.clear()` + reuse per pixel.
- **DeepOutPixel channel order** — When using `DeepOutPixel::push_back()`, values must be pushed in the exact order of the `ChannelSet` iteration (`foreach(z, channels)`). Mismatched order produces channel swaps.
- **Output bbox expansion in _validate()** — `_validate()` must expand `_deepInfo.box()` by the blur radius so downstream nodes know the output is larger than the input. Without this, Nuke will request only the input bbox region and blur won't be visible at the edges.

## Open Risks

- **Performance at large blur sizes** — O(W×H×S×kernel_area) is inherent. A blur_width=20 produces a kernel_area of ~41×41 = 1681 evaluations per output pixel per sample. Dense deep images (100+ samples/pixel) will be very slow. The max_samples cap mitigates output cost but not input reading cost. A tooltip warning for values > 10 is the pragmatic approach. No GPU path is in scope.
- **Separable decomposition for v2** — If performance is unacceptable, a future version could implement separable horizontal+vertical passes. This requires materializing an intermediate deep image between passes, which is complex but feasible. Flagged as v2 optimization, not v1 scope.
- **Merge tolerance default value** — The right depth tolerance for merging propagated samples is data-dependent. Too small = no merging (memory explosion). Too large = visible artifacts. Start with 0.001 and expose as a knob. May need per-shot tuning guidance in the tooltip.
