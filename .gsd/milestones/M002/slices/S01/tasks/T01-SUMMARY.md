---
id: T01
parent: S01
milestone: M002
provides:
  - DeepSampleOptimizer.h header-only utility with optimizeSamples() and SampleRecord
key_files:
  - src/DeepSampleOptimizer.h
key_decisions:
  - Generalized DeepThinner's fixed rgb[3] to std::vector<float> channels for arbitrary channel sets
  - Used in-place merge with separate merged vector then move-assign (avoids alive-marking complexity)
patterns_established:
  - deepc namespace for shared non-DDImage utilities
  - Header-only inline pattern for cross-plugin reuse
observability_surfaces:
  - Standalone compile test validates header independence from DDImage
duration: 15m
verification_result: passed
completed_at: 2026-03-20
blocker_discovered: false
---

# T01: Implement DeepSampleOptimizer.h shared header

**Created standalone header-only DeepSampleOptimizer.h with SampleRecord struct and optimizeSamples() implementing front-to-back over-compositing merge and max_samples cap**

## What Happened

Extracted and generalized the merge logic from DeepThinner.cpp Pass 6 (Smart Merge) and Pass 7 (Max Samples) into a reusable, DDImage-free header. The key generalization: replaced DeepThinner's fixed `float rgb[3]` with `std::vector<float> channels` so any channel set can be optimized. The `optimizeSamples()` function sorts by zFront, forms groups by Z-proximity and color-proximity, merges each group via front-to-back over-compositing (`alpha_acc += alpha * (1 - alpha_acc)`), and truncates to maxSamples. All functions are `inline` for ODR safety. Everything lives in `namespace deepc`.

## Verification

Ran all task-level checks (file exists, struct defined, function defined, inline present, only std includes, no DDImage). Also compiled a standalone test harness with `g++ -std=c++17 -Wall -Wextra` that exercises: empty input, single sample passthrough, non-merge (far apart), merge with over-compositing math verification (alpha 0.5 + 0.3*(1-0.5) = 0.65), max_samples cap, and colorDistance helper. All tests passed.

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `test -f src/DeepSampleOptimizer.h` | 0 | ✅ pass | <1s |
| 2 | `grep -q "struct SampleRecord" src/DeepSampleOptimizer.h` | 0 | ✅ pass | <1s |
| 3 | `grep -q "optimizeSamples" src/DeepSampleOptimizer.h` | 0 | ✅ pass | <1s |
| 4 | `grep -q "inline" src/DeepSampleOptimizer.h` | 0 | ✅ pass | <1s |
| 5 | `grep -cP '#include\s+<' src/DeepSampleOptimizer.h` (result: 3) | 0 | ✅ pass | <1s |
| 6 | `grep -q "DDImage" src/DeepSampleOptimizer.h` (not found) | 1 | ✅ pass | <1s |
| 7 | `g++ -std=c++17 -Wall -Wextra -I. -o /tmp/test_optimizer /tmp/test_optimizer.cpp && /tmp/test_optimizer` | 0 | ✅ pass | <1s |

## Diagnostics

- Compile the header standalone with `g++ -std=c++17 -fsyntax-only -I. src/DeepSampleOptimizer.h` to verify no dependency leaks.
- Grep for `inline` count: `grep -c "inline" src/DeepSampleOptimizer.h` — should be 2 (colorDistance + optimizeSamples).
- The header is in `namespace deepc` — callers use `deepc::optimizeSamples()` and `deepc::SampleRecord`.

## Deviations

- Simplified the merge implementation compared to DeepThinner's alive-marking approach. Instead of marking samples alive/dead and compacting, used a fresh `merged` vector built during the group walk and move-assigned back. This is cleaner and avoids the complexity of tracking alive state across passes, since this header only does merge + cap (not the 7-pass pipeline).

## Known Issues

None.

## Files Created/Modified

- `src/DeepSampleOptimizer.h` — new header-only utility with SampleRecord struct, colorDistance helper, and optimizeSamples function
- `.gsd/milestones/M002/slices/S01/S01-PLAN.md` — added Observability / Diagnostics section, failure-path verification check, marked T01 done
- `.gsd/milestones/M002/slices/S01/tasks/T01-PLAN.md` — added Observability Impact section
