---
id: T01
parent: S01
milestone: M003
provides:
  - Three Gaussian kernel accuracy tier functions (LQ/MQ/HQ)
  - computeKernel dispatcher
  - _kernelQuality enum knob in UI
key_files:
  - src/DeepCBlur.cpp
key_decisions:
  - Used sigma = blur / 3.0 (backward-compatible with M002), not CMG99's 0.42466 factor
  - Placed kernel functions as static free functions above class definition for file-scope visibility
patterns_established:
  - kernelQualityNames static array + Enumeration_knob pattern consistent with DeepCPNoise.cpp
observability_surfaces:
  - Kernel quality tier visible via Nuke knob panel and script query
duration: 15m
verification_result: passed
completed_at: 2026-03-21
blocker_discovered: false
---

# T01: Add kernel tier functions and enum knob

**Added three Gaussian kernel accuracy tier functions (LQ/MQ/HQ), computeKernel dispatcher, and kernel_quality Enumeration_knob to DeepCBlur.cpp**

## What Happened

Ported three kernel generation functions as static free functions above the DeepCBlur class definition:

- `getLQGaussianKernel` — raw unnormalized Gaussian (fastest, least accurate)
- `getMQGaussianKernel` — normalized so full symmetric kernel sums to 1.0
- `getHQGaussianKernel` — CDF-based sub-pixel integration using `erff()`, covers ±3.5σ

Added `computeKernel(float blur, int quality)` dispatcher that maps quality enum (0/1/2) to the appropriate tier, using `sigma = blur / 3.0` for backward compatibility.

Added `_kernelQuality` int member (default 1 = Medium) and `Enumeration_knob` in `knobs()` after blur height, with tooltip describing the three tiers.

The existing `doDeepEngine` 2D blur loop was intentionally left untouched — T02 will wire the kernel functions into the separable blur refactor.

## Verification

All four task-level grep checks pass. Syntax-only compilation with mock DDImage headers confirms no C++ errors in the new code. Slice-level checks V1 (Enumeration_knob ≥1), V2 (kernel funcs ≥3), and V4 (zero-blur fast path) pass. V3 (intermediate buffer) and V5 (syntax script) are T02 deliverables.

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `grep -c "Enumeration_knob" src/DeepCBlur.cpp` (returns 2) | 0 | ✅ pass | <1s |
| 2 | `grep -c "getLQ\|getMQ\|getHQ" src/DeepCBlur.cpp` (returns 7) | 0 | ✅ pass | <1s |
| 3 | `grep -q "_kernelQuality" src/DeepCBlur.cpp` | 0 | ✅ pass | <1s |
| 4 | `grep -q "computeKernel" src/DeepCBlur.cpp` | 0 | ✅ pass | <1s |
| 5 | `grep -q "radX == 0 && radY == 0" src/DeepCBlur.cpp` | 0 | ✅ pass | <1s |
| 6 | `g++ -std=c++17 -fsyntax-only -I/tmp/mock_dd src/DeepCBlur.cpp` | 0 | ✅ pass | <1s |

## Diagnostics

- Kernel quality tier is queryable via `nuke.toNode("DeepCBlur1")["kernel_quality"].value()` in Nuke script console
- New functions are defined but not called from doDeepEngine until T02 wires them

## Deviations

None.

## Known Issues

None.

## Files Created/Modified

- `src/DeepCBlur.cpp` — Added 3 kernel tier functions, computeKernel dispatcher, _kernelQuality member + constructor init, Enumeration_knob in knobs()
- `.gsd/milestones/M003/slices/S01/S01-PLAN.md` — Added Observability/Diagnostics section, marked T01 done
