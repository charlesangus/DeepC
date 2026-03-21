---
estimated_steps: 5
estimated_files: 1
---

# T01: Add kernel tier functions and enum knob

**Slice:** S01 — Separable blur + kernel tiers
**Milestone:** M003

## Description

Port three Gaussian kernel generation functions from CMG99's BlurKernels.cpp (commit `9551704`) into `src/DeepCBlur.cpp` as static free functions, add a `_kernelQuality` enum knob for tier selection (Low/Medium/High, default Medium), and add a `computeKernel()` dispatcher. This is independent of the separable blur refactor — the existing 2D engine is not modified in this task.

**Relevant skills:** None needed — this is straightforward C++ porting.

## Steps

1. **Add kernel quality names and member variable.** Above the `DeepCBlur` class definition, add:
   ```cpp
   static const char* const kernelQualityNames[] = { "Low", "Medium", "High", nullptr };
   ```
   Inside the class, add `int _kernelQuality;` member. Initialize to `1` (Medium) in the constructor.

2. **Port the three kernel functions as static free functions.** Place them above the class definition (or as static file-scope functions). Each takes `(float sigma, int radius)` and returns `std::vector<float>` of size `radius + 1` (half-kernel, symmetric). Port from CMG99 commit `9551704:DeepCBlur/src/BlurKernels.cpp`:
   - `getLQGaussianKernel(float sigma, int radius)` — raw unnormalized Gaussian: `kernel[i] = exp(-i*i / (2*sigma*sigma)) / (sigma * sqrt(2*pi))`. No normalization.
   - `getMQGaussianKernel(float sigma, int radius)` — same as LQ but normalizes the half-kernel so the full symmetric kernel sums to 1.0. Full sum = `kernel[0] + 2 * sum(kernel[1..radius])`.
   - `getHQGaussianKernel(float sigma, int radius)` — CDF-based sub-pixel integration. Uses `erff()` for the CDF. Covers -3.5σ to +3.5σ. Step = `7*sigma / (2*radius+1)`. Integrates area between CDF steps. Then normalizes like MQ. Note: CMG99 uses `M_SQRT2_F` — use `static_cast<float>(M_SQRT2)` or `sqrtf(2.0f)` for portability.

3. **Add computeKernel dispatcher.** Static or free function:
   ```cpp
   static std::vector<float> computeKernel(float blur, int quality)
   {
       const float sigma = std::max(blur / 3.0f, 0.001f);
       const int radius = std::max(0, static_cast<int>(std::ceil(blur)));
       if (radius == 0)
           return {1.0f};
       switch (quality) {
           case 0: return getLQGaussianKernel(sigma, radius);
           case 2: return getHQGaussianKernel(sigma, radius);
           default: return getMQGaussianKernel(sigma, radius);
       }
   }
   ```
   Uses `sigma = blur / 3.0` (NOT CMG99's `blur * 0.42466`) for backward compatibility with M002.

4. **Add Enumeration_knob in knobs().** After the blur height knob, before the optimization knobs:
   ```cpp
   Enumeration_knob(f, &_kernelQuality, kernelQualityNames, "kernel_quality", "kernel quality");
   Tooltip(f, "Gaussian kernel accuracy tier. Low = fast/approximate, "
               "Medium = normalized (default), High = CDF sub-pixel integration.");
   ```
   Follow the pattern from `DeepCPNoise.cpp` lines 8, 346.

5. **Verify compilation.** The file should still compile. The new functions are defined but not yet called from `doDeepEngine` — T02 wires them in. Verify with a quick syntax scan.

## Must-Haves

- [ ] `getLQGaussianKernel`, `getMQGaussianKernel`, `getHQGaussianKernel` exist as static free functions returning `std::vector<float>` half-kernels
- [ ] `_kernelQuality` int member initialized to 1 in constructor
- [ ] `Enumeration_knob` with `kernelQualityNames` array `{"Low", "Medium", "High", nullptr}`
- [ ] `computeKernel()` dispatcher using sigma = blur / 3.0
- [ ] Existing `doDeepEngine` not modified — kernel functions are added but not wired

## Verification

- `grep -c "getLQGaussianKernel\|getMQGaussianKernel\|getHQGaussianKernel" src/DeepCBlur.cpp` returns >= 3
- `grep -q "Enumeration_knob" src/DeepCBlur.cpp` exits 0
- `grep -q "_kernelQuality" src/DeepCBlur.cpp` exits 0
- `grep -q "computeKernel" src/DeepCBlur.cpp` exits 0

## Inputs

- `src/DeepCBlur.cpp` — current 336-line implementation to extend with kernel functions and enum knob
- `src/DeepCPNoise.cpp` — reference for `Enumeration_knob` pattern (lines 8, 346)

## Expected Output

- `src/DeepCBlur.cpp` — extended with three kernel functions, computeKernel dispatcher, _kernelQuality member, and Enumeration_knob in knobs()
