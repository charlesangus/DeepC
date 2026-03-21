---
id: T02
parent: S02
milestone: M004-ks4br0
provides:
  - Tidy output pass (sort by zFront + overlap clamp) for DeepCDepthBlur
  - Optional B input with depth-range gating for selective spreading
  - Docker-verified DeepCDepthBlur.so linked against real Nuke 16.0 SDK
  - Uniform fallback fix for degenerate n=2 weight edge case
key_files:
  - src/DeepCDepthBlur.cpp
  - scripts/verify-s01-syntax.sh
key_decisions:
  - Used inputs(2) constructor + minimum_inputs/maximum_inputs pattern (matching DeepCCopyBBox) instead of override keywords which don't exist on the real Nuke SDK Op base class
  - B plane fetch failure treated as "no B samples" (graceful degradation, not error)
  - Tidy overlap clamp modifies zBack only (not alpha), preserving flatten invariant
patterns_established:
  - Optional deep B input pattern: inputs(2) + minimum_inputs()=1 + dynamic_cast<DeepOp*>(Op::input(1)) with null check
  - Tidy output pattern: struct-based sample collection → sort by zFront → clamp zBack overlaps → emit
observability_surfaces:
  - Docker build confirms DeepCDepthBlur.so compiles and links against real Nuke SDK
  - g++ -fsyntax-only with mock headers validates syntax without SDK
  - B input gating visible via untouched pass-through samples in deep pixel viewer
duration: 10m
verification_result: passed
completed_at: 2026-03-21
blocker_discovered: false
---

# T02: Tidy output + optional B input + docker verification

**Added tidy output pass (sort + overlap clamp), optional B input with depth-range gating, fixed weight normalisation edge case, and verified docker build produces DeepCDepthBlur.so for Nuke 16.0.**

## What Happened

Extended `src/DeepCDepthBlur.cpp` (344→491 lines) with three major additions:

1. **Tidy output pass:** After spreading, output samples are collected into a struct vector, sorted by zFront ascending, then walked to clamp overlapping zBack values (`zBack[i] = min(zBack[i], zFront[i+1])`). The clamp only modifies depth extent, not alpha, so the flatten invariant is preserved.

2. **Optional B input:** Wired as input 1 using the `inputs(2)` + `minimum_inputs()=1` + `maximum_inputs()=2` pattern. When B is connected, `getDeepRequests` fetches B's depth channels, and `doDeepEngine` collects B sample depth intervals per pixel. Each source sample's expanded depth range `[zFront-S/2, zBack+S/2]` is tested against B intervals — only overlapping samples are spread; others pass through unchanged. Edge cases: B disconnected → all samples spread; B pixel empty → all pass through; B fetch fails → graceful fallback to no-B mode.

3. **Weight normalisation fix:** All four weight generators now have a uniform fallback (`1.0f/n`) for the degenerate case where sum=0 (occurs with n=2 for Linear and Smoothstep, where endpoints t=±1 produce zero weight).

Updated `scripts/verify-s01-syntax.sh` to check both DeepCBlur.cpp and DeepCDepthBlur.cpp, with mock headers extended to include `inputs()`, `input()`, `minimum_inputs`, `maximum_inputs`, `input_label`, and `test_input`.

## Verification

- `g++ -fsyntax-only` passes on DeepCDepthBlur.cpp with mock DDImage headers
- `grep -c DeepCDepthBlur src/CMakeLists.txt` returns 2 (PLUGINS + FILTER_NODES)
- `wc -l < src/DeepCDepthBlur.cpp` = 491 (≥ 150)
- `docker-build.sh --linux --versions 16.0` exits 0 with DeepCDepthBlur.so (415640 bytes) in archive
- All four weight generators produce normalised weights (sum ≈ 1.0) for n ∈ {1,2,3,5,10,32,64}
- Zero-spread fast path present and tested via code inspection

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `bash scripts/verify-s01-syntax.sh` | 0 | ✅ pass | <1s |
| 2 | `grep -c DeepCDepthBlur src/CMakeLists.txt` (=2) | 0 | ✅ pass | <1s |
| 3 | `wc -l < src/DeepCDepthBlur.cpp` (=491, ≥150) | 0 | ✅ pass | <1s |
| 4 | `bash docker-build.sh --linux --versions 16.0` | 0 | ✅ pass | 33s |
| 5 | `unzip -l release/DeepC-Linux-Nuke16.0.zip \| grep DeepCDepthBlur` | 0 | ✅ pass | <1s |
| 6 | Weight normalisation test (all 4 modes × 7 sample counts) | 0 | ✅ pass | <1s |

## Diagnostics

- **Docker build:** `bash docker-build.sh --linux --versions 16.0` — confirms compile+link. Check `release/DeepC-Linux-Nuke16.0.zip` for `DeepCDepthBlur.so`.
- **Syntax validation:** `bash scripts/verify-s01-syntax.sh` — mock-header syntax check without Nuke SDK.
- **Weight correctness:** Extract weight functions into a standalone `.cpp` and verify sum ≈ 1.0 for all modes and sample counts.
- **B input inspection:** In Nuke, connect a deep stream to B and verify that only depth-overlapping samples are spread. Disconnect B to confirm all samples spread.

## Deviations

- **`minimumInputs`/`maximumInputs` → `minimum_inputs`/`maximum_inputs`:** The task plan used camelCase names, but the real Nuke SDK uses snake_case. Also removed `override` keywords since these are not virtual on the Nuke base class. Discovered during first docker build attempt.
- **Uniform weight fallback:** Added `else for (auto& v : w) v = 1.0f / n` to all four weight generators to fix the n=2 degenerate case. Not in the original plan but required for the "weights normalised for all modes" verification check.

## Known Issues

None. All slice verification checks pass.

## Files Created/Modified

- `src/DeepCDepthBlur.cpp` — Extended from 344→491 lines with tidy pass, B input, and weight fix
- `scripts/verify-s01-syntax.sh` — Extended to check DeepCDepthBlur.cpp with updated mock headers
- `.gsd/milestones/M004-ks4br0/slices/S02/S02-PLAN.md` — Added failure-path verification checks; marked T02 done
- `.gsd/milestones/M004-ks4br0/slices/S02/tasks/T02-PLAN.md` — Added Expected Output and Observability Impact sections
