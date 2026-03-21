# S01 — Research

**Date:** 2026-03-21

## Summary

This slice replaces the O(r²) non-separable 2D Gaussian blur in `doDeepEngine` with two sequential 1D passes (horizontal then vertical), and adds three kernel accuracy tiers selectable via an `Enumeration_knob`. The current implementation (336 lines) already has the right structure — a single `doDeepEngine` that iterates output pixels, gathers weighted samples from a kernel neighbourhood, runs `optimizeSamples`, and emits. The separable refactor keeps that structure but splits the gather into two passes with an intermediate per-pixel sample buffer.

The main structural challenge is the intermediate representation between H and V passes. Unlike flat images (one value per channel per pixel), deep pixels have variable-length sample lists. The intermediate must store a `std::vector<SampleRecord>` per pixel, indexed by (x, y). CMG99 solved this by chaining separate `DeepOp` subclasses via an OpTree framework — we avoid that complexity (D008) and instead allocate the intermediate buffer as a 2D array of sample vectors within the single `doDeepEngine` call.

The kernel tier algorithms are directly available from CMG99's `BlurKernels.cpp` (commit `9551704`). All three produce half-kernels (symmetric, stored as `radius+1` elements). LQ is raw unnormalized Gaussian, MQ normalizes to sum=1, HQ uses CDF-based sub-pixel integration then normalizes. These can be ported as static methods with minimal adaptation.

## Recommendation

Implement in three logical stages within `DeepCBlur.cpp`:

1. **Kernel generation functions** — Port `getLQGaussianKernel`, `getMQGaussianKernel`, `getHQGaussianKernel` from CMG99 as static free functions. Use half-kernel (symmetric) storage. Add `Enumeration_knob` for tier selection. This is independent work with no risk.

2. **Horizontal pass** — Iterate each row, gather weighted samples from the 1D H kernel neighbourhood, store results in an intermediate `std::vector<std::vector<SampleRecord>>` indexed by pixel position.

3. **Vertical pass** — Read from the intermediate buffer, gather weighted samples from the 1D V kernel neighbourhood, run `optimizeSamples`, emit to `DeepOutputPlane`.

Use the MQ (normalized) kernel as default — it matches the current behaviour. Preserve sigma = blur / 3 convention rather than CMG99's `blur * 0.42466` to maintain backward compatibility with existing M002 scenes.

## Implementation Landscape

### Key Files

- `src/DeepCBlur.cpp` — **only file modified**. Current 336 lines. Contains class definition, `knobs()`, `_validate()`, `getDeepRequests()`, `doDeepEngine()`.
- `src/DeepSampleOptimizer.h` — **unchanged, consumed**. `SampleRecord` struct (zFront, zBack, alpha, channels vector) and `optimizeSamples()`. Both passes produce `SampleRecord` vectors.
- `src/DeepCPNoise.cpp` — **reference only**. Lines 58-62 show `Enumeration_knob` pattern: `static const char* const names[] = { "A", "B", "C", nullptr };` then `Enumeration_knob(f, &_intVar, names, "script_name");`.
- Commit `9551704:DeepCBlur/src/BlurKernels.cpp` — **source for kernel algorithms**. Three functions: `getLQGaussianKernel`, `getMQGaussianKernel`, `getHQGaussianKernel`. All take `(float sd, int blurRadius)` and return `std::vector<float>` of size `blurRadius + 1` (half-kernel).

### Build Order

**Stage 1: Kernel functions + enum knob** (independent, no risk)
- Add `static const char* const kernelQualityNames[] = { "Low", "Medium", "High", nullptr };`
- Add `int _kernelQuality` member (default 1 = Medium)
- Add `Enumeration_knob(f, &_kernelQuality, kernelQualityNames, "kernel_quality", "kernel quality");` in `knobs()`
- Port three kernel generation functions as static free functions returning `std::vector<float>` (half-kernel)
- Add a dispatcher: `computeKernel(float blur, int quality)` that calls the appropriate tier

**Stage 2: Separable blur engine** (main risk — intermediate buffer)
- Replace the 2D kernel computation block (lines ~210-230) with two 1D kernel computations (H and V)
- Replace the single nested loop (lines ~250-300) with:
  - **H pass**: For each pixel in `inputBox`, gather from H neighbours → store in `intermediateBuffer[y][x]` as `std::vector<SampleRecord>`
  - **V pass**: For each pixel in output `box`, gather from V neighbours in `intermediateBuffer` → `optimizeSamples` → emit
- The intermediate buffer covers the output box expanded by `radX` in H (which is `inputBox.y()..inputBox.t()` × `box.x()..box.r()`)
- Wait — the intermediate needs height = full input height (padded by radY) but width = output width only, since H pass collapses the X padding. Actually: H pass processes rows across the full padded X range, producing results for output X range. V pass then reads from intermediate across padded Y range, producing output Y range. So intermediate dimensions are: width = `box.r() - box.x()`, height = `inputBox.t() - inputBox.y()` (padded Y range).

**Stage 3: Verification** — compile check via `docker-build.sh` or `g++ -fsyntax-only`

### Verification Approach

1. **Syntax check**: `g++ -std=c++17 -fsyntax-only -I<nuke_include_path> src/DeepCBlur.cpp` — requires Nuke SDK headers. If not available locally, use `docker-build.sh` which handles the SDK.
2. **Docker build**: `./docker-build.sh --linux --versions 16.0` — must exit 0 and produce `DeepCBlur.so` in the archive.
3. **Zero-blur fast path**: Verify the early-return path (radX == 0 && radY == 0) is preserved unchanged.
4. **Code review**: Confirm `_kernelQuality` enum has 3 values, `doDeepEngine` has two sequential 1D gather loops, intermediate buffer is `vector<vector<SampleRecord>>`.

## Constraints

- Must remain a single `DeepFilterOp` subclass — no new files, no new classes (D008).
- GCC 11.2.1, C++17, DDImage API (Nuke 16+).
- `Enumeration_knob` requires a `const char* const[]` terminated by `nullptr` and an `int` member.
- Half-kernel storage: element 0 = center weight, element i = weight at distance i (symmetric). When applying, use `kernel[abs(distance)]`.
- `getDeepRequests` padding must remain unchanged — both radX and radY still needed because V pass reads from intermediate which was built from the padded input.

## Common Pitfalls

- **Intermediate buffer indexing** — The intermediate is indexed relative to a sub-region of the input box. Off-by-one errors on coordinate translation between inputBox, box, and intermediate indices are the most likely bug. Use helper lambdas: `intIdx(x, y)` that maps world coords to buffer indices.
- **Sigma convention mismatch** — CMG99 uses `sd = blur * 0.42466` while current code uses `sigma = blur / 3.0`. These are very close (~0.4247 vs ~0.3333) but not identical. Stick with current `blur / 3.0` for backward compatibility; document the difference in a comment.
- **H pass must weight alpha too** — Current code weights alpha by kernel weight (`rec.alpha = ... * weight`). Both H and V passes must do this, and both must weight colour channels. CMG99's `RawGaussianStrategy` confirms this pattern — RGBA all get multiplied by kernel weight, depth channels propagated raw.
- **DeepOutPixel from intermediate** — The V pass reads from the intermediate `SampleRecord` vectors, not from `DeepPixel` objects. The loop structure differs from the current code which calls `inPlane.getPixel()`. The V pass iterates `intermediateBuffer[srcY][outX].samples` directly.

## Open Risks

- **Memory pressure at large radii** — The intermediate buffer stores `SampleRecord` vectors for `(outputWidth) × (inputHeight)` pixels. Each SampleRecord contains a `std::vector<float>`. At radius 50 with dense deep images (100+ samples/pixel), this could be significant. Mitigation: this matches what the current non-separable approach already touches (it reads all those pixels anyway), and the SampleRecord vectors are temporary within one `doDeepEngine` call.
- **Thread safety of intermediate buffer** — `doDeepEngine` is called per-tile by Nuke's threading. Each tile call gets its own `box` and must allocate its own intermediate buffer. The current `thread_local ScratchBuf` pattern won't work for the 2D intermediate — it must be a local allocation per call. This is fine for correctness but means no buffer reuse across tiles.
