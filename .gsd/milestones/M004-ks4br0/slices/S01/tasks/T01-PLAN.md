# T01: Fix colorDistance + add overlap tidy pre-pass

**Slice:** S01
**Milestone:** M004-ks4br0

## Goal

Correct `DeepSampleOptimizer.h` so colour comparison is unpremultiplied and overlapping depth intervals are split and merged before the tolerance-based merge pass.

## Must-Haves

### Truths
- `colorDistance` returns 0 when either sample has alpha < 1e-6
- `colorDistance` divides channels[i] by alpha before computing max absolute difference
- Overlapping samples [1,5] and [3,7] are split into [1,3],[3,5] and [3,5],[5,7], then over-merged into [1,3] and [3,7]
- Point samples at identical zFront are collapsed by over-composite
- `g++ -fsyntax-only` on DeepCBlur.cpp, DeepCBlur2.cpp passes
- `docker-build.sh --linux --versions 16.0` exits 0

### Artifacts
- `src/DeepSampleOptimizer.h` — modified in place; no new files

### Key Links
- `DeepCBlur.cpp` → `DeepSampleOptimizer.h` via `#include` — benefits automatically
- `DeepCBlur2.cpp` → `DeepSampleOptimizer.h` via `#include` — benefits automatically

## Steps

1. Read `src/DeepSampleOptimizer.h` in full — understand current structure
2. Fix `colorDistance`: add alpha guard (return 0.0f if alpha < 1e-6), then divide each channel by alpha before the max-abs-diff loop. Note: `colorDistance` currently takes two `vector<float>` args — it doesn't have alpha access. Change signature or add an overload that takes alpha values, then update `optimizeSamples` call site to pass per-sample alpha.
3. Write `tidyOverlapping()` free function (or inline lambda inside `optimizeSamples`):
   - Input: sorted vector<SampleRecord>
   - Walk pairs; when sample[i].zBack > sample[i+1].zFront (overlap detected):
     - Compute split point z = sample[i+1].zFront
     - If sample[i] is volumetric (zBack > zFront): split it at z using `alpha_front = 1 - (1-A)^((z-zFront)/(zBack-zFront))`; channel values scale by `alpha_front / alpha_original`
     - Replace sample[i] with the front portion; insert a new back portion
     - Re-sort, repeat until no overlaps remain (or iterate to fixed point)
   - After split pass: over-merge any samples that are now at the same exact depth (zFront==zFront && zBack==zBack) using front-to-back over-composite
4. Insert call to `tidyOverlapping()` at the top of `optimizeSamples`, before the existing merge pass. Only run if input has > 1 sample.
5. Run `bash scripts/verify-s01-syntax.sh` — confirm exit 0 on DeepCBlur.cpp
6. Run same check on DeepCBlur2.cpp (modify script temporarily or inline the g++ call)
7. Run `bash docker-build.sh --linux --versions 16.0` — confirm exit 0

## Context

- `SampleRecord` has: zFront, zBack, alpha, channels (premultiplied, Gaussian-weighted)
- Volumetric alpha subdivision formula: `alpha_sub = 1 - (1 - alpha)^(subRange / totalRange)` where subRange is the split sub-interval length and totalRange is the original zBack-zFront
- For point samples (zFront == zBack), totalRange = 0 — treat as hard surface; no subdivision needed, just over-composite exact-match pairs
- The `colorDistance` signature change needs to propagate to the call in the merge pass inside `optimizeSamples` — check both call sites
- Keep `DeepSampleOptimizer.h` zero-DDImage-dependency (no Nuke headers)

## Expected Output

- `src/DeepSampleOptimizer.h` — modified with fixed `colorDistance` and new `tidyOverlapping`

## Observability Impact

- `colorDistance` now returns 0.0f for near-zero alpha samples instead of comparing near-zero premultiplied values — this makes merge decisions stable for transparent samples.
- `tidyOverlapping` runs before the merge pass; sample count changes are observable by comparing `samples.size()` before/after the call. No new logging — the function is pure (input→output, no side effects).
- Failure state: degenerate inputs (NaN depths, zero-alpha) hit guard clauses and produce well-defined output rather than NaN propagation.
