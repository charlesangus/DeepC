# S01: Fix DeepSampleOptimizer colour comparison

**Goal:** Correct `colorDistance` to unpremultiply before comparing, and add an overlap tidy pre-pass that splits and merges overlapping depth intervals before the existing tolerance-based merge.

**Demo:** DeepCBlur output on the reference rectangles input is smooth — no concentric jaggy artifacts — with default colour tolerance settings.

## Must-Haves

- `colorDistance` divides channels by alpha before comparison; near-zero alpha (< 1e-6) treated as always-matching (returns 0)
- Overlap tidy pre-pass in `optimizeSamples` runs before the existing merge pass: detects overlapping [zFront, zBack] intervals, splits at boundaries, over-merges front-to-back
- Volumetric alpha subdivision used when splitting: `alpha_sub = 1 - (1-A)^(subRange/totalRange)`
- Point samples at identical zFront are collapsed in the same pass
- `g++ -fsyntax-only` passes on DeepCBlur.cpp, DeepCBlur2.cpp, and DeepSampleOptimizer.h
- `docker-build.sh --linux --versions 16.0` exits 0

## Tasks

- [x] **T01: Fix colorDistance + add overlap pre-pass in DeepSampleOptimizer.h**
  Modify `colorDistance` to unpremultiply. Add `tidenAndMergeOverlapping()` private helper called at the top of `optimizeSamples`. Write and run syntax checks.

## Files Likely Touched

- `src/DeepSampleOptimizer.h`
- `scripts/verify-s01-syntax.sh` (may need stub updates if any new includes)

## Observability / Diagnostics

- **Compile-time signals:** `g++ -fsyntax-only` on DeepCBlur.cpp and DeepCBlur2.cpp confirms the header parses correctly with both consumers.
- **Runtime inspection:** Callers of `optimizeSamples` can inspect the sample count before/after the tidy pre-pass via `samples.size()` — a count decrease confirms overlaps were merged.
- **Failure visibility:** If `tidyOverlapping` encounters degenerate input (zero-length volumetric, NaN depths), the alpha-guard in `colorDistance` returns 0 rather than producing NaN/inf. Splitting a zero-range sample is a no-op (point sample guard).
- **No redaction constraints** — no secrets or PII involved.

## Verification

1. `bash scripts/verify-s01-syntax.sh` — exits 0 (DeepCBlur.cpp syntax)
2. `g++ -std=c++17 -fsyntax-only -I<mockdir> src/DeepCBlur2.cpp` — exits 0
3. `bash docker-build.sh --linux --versions 16.0` — exits 0
4. Grep for old `colorDistance` two-arg signature — must return no matches in DeepSampleOptimizer.h
5. Grep for `tidyOverlapping` call inside `optimizeSamples` — must find exactly one call site
