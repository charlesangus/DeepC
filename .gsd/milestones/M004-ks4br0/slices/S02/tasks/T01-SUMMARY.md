---
id: T01
parent: S02
milestone: M004-ks4br0
provides:
  - DeepCDepthBlur plugin skeleton with knobs and falloff weight engine
  - CMakeLists.txt registration for DeepCDepthBlur
key_files:
  - src/DeepCDepthBlur.cpp
  - src/CMakeLists.txt
key_decisions:
  - Weight generators implemented as static free functions with a dispatcher, matching the plan's architecture
  - Premult-preserving channel scaling: all non-depth channels multiplied by weight per sub-sample
patterns_established:
  - Falloff weight generators as static functions returning normalised std::vector<float>
  - Zero-spread fast path pattern for pass-through when spread < 1e-6 or numSamples <= 1
observability_surfaces:
  - g++ -fsyntax-only with mock DDImage headers validates syntax without Nuke SDK
  - Knob tooltips document each parameter's range and behaviour
duration: 12m
verification_result: passed
completed_at: 2026-03-21
blocker_discovered: false
---

# T01: DeepCDepthBlur skeleton + knobs + falloff engine

**Created DeepCDepthBlur.cpp (344 lines) with four falloff weight generators, all knobs, and per-sample depth-spread logic; registered in CMakeLists.txt PLUGINS and FILTER_NODES.**

## What Happened

Created `src/DeepCDepthBlur.cpp` as a `DeepFilterOp` subclass implementing:
- Plugin registration as `"DeepCDepthBlur"` with menu path `"Deep/DeepCDepthBlur"`
- Four knobs: `spread` (Float, default 1.0), `num_samples` (Int, default 5), `falloff` (Enum: Linear/Gaussian/Smoothstep/Exponential), `sample_type` (Enum: Volumetric/Flat)
- Four weight generators (`weightsLinear`, `weightsGaussian`, `weightsSmoothstep`, `weightsExponential`) all normalised to sum=1, with a `computeWeights` dispatcher
- `_validate` clamping num_samples ≥ 1 and spread ≥ 0
- `getDeepRequests` as pass-through (no spatial bbox expansion)
- `doDeepEngine` with zero-spread fast path and per-sample N-sub-sample spreading: depth positions evenly spaced in [zFront-S/2, zFront+S/2], channel values scaled by normalised weights (preserving premult and flatten invariant), volumetric vs flat depth representation

Added `DeepCDepthBlur` to `src/CMakeLists.txt` in both the PLUGINS list and FILTER_NODES set.

Also added missing Observability/Diagnostics and Verification sections to S02-PLAN.md, and Expected Output + Observability Impact sections to T01-PLAN.md per pre-flight requirements.

## Verification

- `g++ -std=c++17 -fsyntax-only` with mock DDImage headers passes cleanly
- DeepCDepthBlur appears 2 times in CMakeLists.txt (PLUGINS + FILTER_NODES)
- File is 344 lines (above 150 minimum)
- All four knob names present in source
- All five weight functions present in source
- Plugin CLASS and menu path strings verified

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `g++ -std=c++17 -fsyntax-only -I$TMPDIR src/DeepCDepthBlur.cpp` | 0 | ✅ pass | <1s |
| 2 | `grep -c DeepCDepthBlur src/CMakeLists.txt` (≥2) | 0 | ✅ pass | <1s |
| 3 | `wc -l < src/DeepCDepthBlur.cpp` (≥150) | 0 | ✅ pass | <1s |
| 4 | `grep '"DeepCDepthBlur"' src/DeepCDepthBlur.cpp` | 0 | ✅ pass | <1s |
| 5 | `grep '"Deep/DeepCDepthBlur"' src/DeepCDepthBlur.cpp` | 0 | ✅ pass | <1s |
| 6 | Knob names: spread, num_samples, falloff, sample_type | 0 | ✅ pass | <1s |
| 7 | Falloff functions: weightsLinear/Gaussian/Smoothstep/Exponential/computeWeights | 0 | ✅ pass | <1s |

## Diagnostics

- **Syntax validation:** Run `g++ -fsyntax-only` with mock DDImage headers (see `scripts/verify-s01-syntax.sh` for the mock header pattern)
- **Knob inspection:** Grep for knob script-names (`spread`, `num_samples`, `falloff`, `sample_type`) in source
- **Weight correctness:** The static `computeWeights(n, mode)` function can be called from a standalone test harness to verify normalisation

## Deviations

None. Implementation follows the task plan exactly.

## Known Issues

- Output is not yet tidy (not sorted by zFront, potential overlaps) — deferred to T02
- B input not wired — deferred to T02
- Docker build not yet verified — deferred to T02

## Files Created/Modified

- `src/DeepCDepthBlur.cpp` — New 344-line DeepFilterOp plugin with knobs, weight generators, and depth-spread engine
- `src/CMakeLists.txt` — Added DeepCDepthBlur to PLUGINS list and FILTER_NODES set
- `.gsd/milestones/M004-ks4br0/slices/S02/S02-PLAN.md` — Added Verification, Observability/Diagnostics sections
- `.gsd/milestones/M004-ks4br0/slices/S02/tasks/T01-PLAN.md` — Added Expected Output, Observability Impact sections
