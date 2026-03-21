---
id: S01
parent: M004-ks4br0
milestone: M004-ks4br0
provides:
  - unpremultiplied colorDistance comparison in DeepSampleOptimizer (R011)
  - tidyOverlapping pre-pass splitting and merging overlapping depth intervals (R012)
requires: []
affects:
  - S02
key_files:
  - src/DeepSampleOptimizer.h
key_decisions:
  - colorDistance signature changed to accept per-sample alpha explicitly (single call site made this safe)
  - tidyOverlapping implemented as a free inline function, not a lambda, for readability and potential reuse
patterns_established:
  - Volumetric alpha subdivision via alpha_sub = 1 - (1-A)^(subRange/totalRange) for splitting deep samples at arbitrary depth boundaries
observability_surfaces:
  - none — pure header-only math functions with no runtime logging
drill_down_paths:
  - .gsd/milestones/M004-ks4br0/slices/S01/tasks/T01-SUMMARY.md
duration: 12m
verification_result: passed
completed_at: 2026-03-21
---

# S01: Fix DeepSampleOptimizer colour comparison

**Corrected `colorDistance` to unpremultiply before comparison and added `tidyOverlapping` pre-pass to split/merge overlapping depth intervals — eliminating the premult bug that produced concentric jaggy artifacts in DeepCBlur output.**

## What Happened

Single task (T01) delivered both changes to `src/DeepSampleOptimizer.h`:

**`colorDistance` fix:** The function previously compared premultiplied channel values directly. Because Gaussian-weighted samples from the same surface carry different alpha values, their premultiplied RGB values appear as different colours even when they represent the same surface — blocking merges and producing jaggy banding. The fix adds `alphaA` / `alphaB` parameters and divides each channel by its sample's alpha before computing the max absolute difference. Near-zero alpha (< 1e-6) returns 0.0f immediately so transparent samples always match. The single call site in `optimizeSamples` was updated to pass both alphas.

**`tidyOverlapping` pre-pass:** New inline function called at the top of `optimizeSamples` (before the existing tolerance-based merge). It performs two sub-passes until convergence:
1. **Split pass** — detects any `sample[i].zBack > sample[i+1].zFront` overlap, splits the volumetric sample at the boundary using `alpha_sub = 1 - (1-A)^(subRange/totalRange)`, scaling premultiplied channels proportionally. Point samples (zFront == zBack) are skipped.
2. **Over-merge pass** — collapses samples sharing identical `[zFront, zBack]` via front-to-back over-compositing, eliminating same-depth duplicates regardless of colour tolerance.

Both DeepCBlur.cpp and DeepCBlur2.cpp include the corrected header and were verified at compile time and via Docker build against the real Nuke 16.0 SDK.

## Verification

| # | Check | Result |
|---|-------|--------|
| 1 | `bash scripts/verify-s01-syntax.sh` (DeepCBlur.cpp g++ syntax) | ✅ exit 0 |
| 2 | `g++ -fsyntax-only` on DeepCBlur2.cpp via mock headers | ✅ exit 0 |
| 3 | `bash docker-build.sh --linux --versions 16.0` | ✅ exit 0, produced release zip |
| 4 | grep for old 2-arg `colorDistance` signature | ✅ no match (removed) |
| 5 | `grep -c "tidyOverlapping(samples)"` in header | ✅ count = 1 |

## New Requirements Surfaced

- none

## Deviations

- Slice plan mentioned `tidenAndMergeOverlapping()` as the function name; the task plan and implementation used `tidyOverlapping()`. The task plan name was treated as authoritative.

## Known Limitations

- No runtime diagnostics. Correctness of the unpremult fix and tidy pre-pass is verified at compile time (syntax checks) and Docker build. A test harness or visual comparison against a reference render is needed to confirm jaggies are gone at runtime — that visual UAT step is deferred to human verification.
- `tidyOverlapping` iterates until convergence with no iteration cap. Pathological inputs with many nested overlaps could be slow, but this is not expected with real deep images.

## Follow-ups

- S02 should consume the corrected `DeepSampleOptimizer.h` for its tidy-up pass after depth spread. No interface changes are needed.
- Visual confirmation (render comparison with/without fix) is recommended before M004 sign-off but is not a blocker for S02.

## Files Created/Modified

- `src/DeepSampleOptimizer.h` — Fixed `colorDistance` to unpremultiply; added `tidyOverlapping` pre-pass
- `.gsd/milestones/M004-ks4br0/slices/S01/S01-PLAN.md` — Added observability/verification sections; marked T01 done
- `.gsd/milestones/M004-ks4br0/slices/S01/tasks/T01-PLAN.md` — Added Expected Output and Observability Impact sections

## Forward Intelligence

### What the next slice should know
- `tidyOverlapping` is called at the top of `optimizeSamples`. If S02 calls `optimizeSamples` after depth spread, overlapping samples from the spread will be automatically tidied. No extra call site needed unless S02 wants to tidy before calling `optimizeSamples`.
- `colorDistance` now takes four args: `(const vector<float>& a, float alphaA, const vector<float>& b, float alphaB)`. Any future call site must pass both alphas — the old two-arg overload is gone.

### What's fragile
- `tidyOverlapping` convergence loop — if future code inserts samples in a way that creates new overlaps after each split, the loop could run many iterations. The current implementation handles real-world blur gather output safely, but synthetic tests with adversarial depth layouts should be checked.
- The near-zero alpha guard in `colorDistance` (< 1e-6 → return 0) means two fully transparent samples are always treated as colour-matching and will be merged. This is the intended behavior, but worth noting if future features need to distinguish transparent samples by colour.

### Authoritative diagnostics
- `bash scripts/verify-s01-syntax.sh` — fast first check for C++ syntax errors in DeepCBlur.cpp
- `bash docker-build.sh --linux --versions 16.0` — authoritative build proof; mock headers cannot catch DDImage API mismatches

### What assumptions changed
- Original plan assumed the function might be called from multiple sites, motivating an overload approach. In practice there is exactly one call site in `optimizeSamples`, so a signature change was cleaner and was adopted instead.
