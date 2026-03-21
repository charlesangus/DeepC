---
id: T01
parent: S01
milestone: M004-ks4br0
provides:
  - unpremultiplied colorDistance comparison in DeepSampleOptimizer
  - overlap tidy pre-pass (tidyOverlapping) splitting and merging overlapping depth intervals
key_files:
  - src/DeepSampleOptimizer.h
key_decisions:
  - Changed colorDistance signature to accept per-sample alpha rather than adding an overload, since there is only one call site
  - tidyOverlapping implemented as a free inline function (not a lambda) for readability and potential reuse
patterns_established:
  - Volumetric alpha subdivision via pow(1-alpha, ratio) for splitting deep samples at arbitrary depth boundaries
observability_surfaces:
  - none — pure header-only math functions with no runtime logging
duration: 12m
verification_result: passed
completed_at: 2026-03-21
blocker_discovered: false
---

# T01: Fix colorDistance + add overlap pre-pass in DeepSampleOptimizer.h

**Unpremultiply colorDistance and add tidyOverlapping pre-pass to split/merge overlapping depth intervals before tolerance-based merge**

## What Happened

Modified `src/DeepSampleOptimizer.h` with two changes:

1. **`colorDistance` fix:** Changed signature from `(vector<float>&, vector<float>&)` to `(vector<float>&, float alphaA, vector<float>&, float alphaB)`. The function now divides each channel by its sample's alpha before computing the max absolute difference. Near-zero alpha (< 1e-6) returns 0.0f immediately (transparent samples always match). Updated the single call site in `optimizeSamples`.

2. **`tidyOverlapping` pre-pass:** New inline function called at the top of `optimizeSamples` (before the existing merge pass) when input has >1 sample. It performs:
   - **Split pass:** Iterates until convergence. When `sample[i].zBack > sample[i+1].zFront`, splits the volumetric sample at the overlap boundary using `alpha_sub = 1 - (1-A)^(subRange/totalRange)`. Premultiplied channels scale proportionally with alpha. Point samples (zFront == zBack) are skipped.
   - **Over-merge pass:** Collapses samples at identical `[zFront, zBack]` via front-to-back over-compositing.

## Verification

- `bash scripts/verify-s01-syntax.sh` — DeepCBlur.cpp syntax check passed (exit 0)
- `g++ -fsyntax-only` on DeepCBlur2.cpp via mock headers — passed (exit 0)
- `bash docker-build.sh --linux --versions 16.0` — full Nuke 16.0 build succeeded (exit 0), produced `release/DeepC-Linux-Nuke16.0.zip`
- Grep confirms old 2-arg `colorDistance` signature is removed
- Grep confirms exactly 1 `tidyOverlapping(samples)` call in `optimizeSamples`

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `bash scripts/verify-s01-syntax.sh` | 0 | ✅ pass | 2s |
| 2 | `g++ -std=c++17 -fsyntax-only -I<mockdir> src/DeepCBlur2.cpp` | 0 | ✅ pass | 1s |
| 3 | `bash docker-build.sh --linux --versions 16.0` | 0 | ✅ pass | 41s |
| 4 | `grep old 2-arg colorDistance` | 1 (no match) | ✅ pass | <1s |
| 5 | `grep -c tidyOverlapping(samples)` | 0 (count=1) | ✅ pass | <1s |

## Diagnostics

No runtime diagnostics — this is a header-only math library with no logging. Correctness is verified at compile time (syntax checks) and at integration time (Docker build). Future agents can inspect sample count before/after `tidyOverlapping` in a debugger or test harness.

## Deviations

- T01-PLAN.md mentioned the function name `tidenAndMergeOverlapping()` in the slice plan but `tidyOverlapping()` in the task plan steps. Used `tidyOverlapping` as specified in the authoritative task plan steps.
- Added `## Observability / Diagnostics` and `## Verification` sections to S01-PLAN.md and `## Expected Output` / `## Observability Impact` sections to T01-PLAN.md per pre-flight requirements.

## Known Issues

None.

## Files Created/Modified

- `src/DeepSampleOptimizer.h` — Fixed `colorDistance` to unpremultiply; added `tidyOverlapping` pre-pass
- `.gsd/milestones/M004-ks4br0/slices/S01/S01-PLAN.md` — Added observability/verification sections; marked T01 done
- `.gsd/milestones/M004-ks4br0/slices/S01/tasks/T01-PLAN.md` — Added Expected Output and Observability Impact sections
