---
id: T02
parent: S01
milestone: M003
provides:
  - Separable H→V blur engine in doDeepEngine
  - Intermediate per-pixel sample buffer between passes
  - Syntax verification script with mock DDImage headers
key_files:
  - src/DeepCBlur.cpp
  - scripts/verify-s01-syntax.sh
key_decisions:
  - Intermediate buffer indexed as [relY][relX] with width = output box width, height = input box height (padded Y range)
  - optimizeSamples called only after V pass to avoid premature merging of H-accumulated samples
patterns_established:
  - Helper lambdas intX/intY for coordinate translation to avoid off-by-one errors in buffer indexing
observability_surfaces:
  - computeKernel calls are wired to _kernelQuality knob — kernel tier selection is inspectable at runtime via Nuke knob panel
  - Separable pass structure is verifiable via syntax script and grep checks
duration: 20m
verification_result: passed
completed_at: 2026-03-21
blocker_discovered: false
---

# T02: Replace 2D blur with separable H→V passes

**Replaced O(r²) 2D Gaussian blur in doDeepEngine with two sequential 1D gather passes (H→V) using intermediate per-pixel sample buffer and kernel tier dispatcher**

## What Happened

Refactored `doDeepEngine` in `DeepCBlur.cpp` to use separable Gaussian blur:

1. **Removed old 2D kernel code:** Deleted the `sigmaX/sigmaY/kernelW/kernelH` computation block, the nested `dx/dy` kernel fill loop, `kernelSum` normalization, and `ScratchBuf::kernel` member.

2. **Horizontal pass:** Computes 1D half-kernel via `computeKernel(_blurWidth, _kernelQuality)`. Iterates each row in the padded input Y range and each column in the output X range. Gathers from the 1D H neighbourhood, weighting alpha and colour channels by kernel weight while propagating depth channels raw. Stores results in `intermediateBuffer[relY][relX]` — a `vector<vector<vector<SampleRecord>>>` with dimensions (inputBox height × output box width).

3. **Vertical pass:** Computes 1D half-kernel via `computeKernel(_blurHeight, _kernelQuality)`. For each output pixel, gathers from intermediate buffer rows in the 1D V neighbourhood. Applies V kernel weight on top of already-H-weighted samples (separable property: total = H × V). Calls `optimizeSamples` only after this final pass.

4. **Zero-blur fast path preserved unchanged** — the `radX == 0 && radY == 0` block remains at the top of doDeepEngine.

5. **Created `scripts/verify-s01-syntax.sh`** with mock DDImage headers providing minimal stubs for all types/macros used by DeepCBlur.cpp. Runs `g++ -std=c++17 -fsyntax-only` against the source.

## Verification

All task-level and slice-level checks pass. The syntax script compiles the refactored source without errors. Old 2D kernel variables are confirmed removed (remaining `kernelW`/`kernelH` matches are from the HQ kernel function's local `kernelWidth` and the new `kernelH` 1D half-kernel variable).

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `bash scripts/verify-s01-syntax.sh` | 0 | ✅ pass | <1s |
| 2 | `grep -q "intermediateBuffer\|intermediate" src/DeepCBlur.cpp` | 0 | ✅ pass | <1s |
| 3 | `grep -q "computeKernel" src/DeepCBlur.cpp` | 0 | ✅ pass | <1s |
| 4 | `grep -q "radX == 0 && radY == 0" src/DeepCBlur.cpp` | 0 | ✅ pass | <1s |
| 5 | `grep -c "Enumeration_knob" src/DeepCBlur.cpp` (returns 2) | 0 | ✅ pass | <1s |
| 6 | `grep -c "getLQ\|getMQ\|getHQ" src/DeepCBlur.cpp` (returns 7) | 0 | ✅ pass | <1s |
| 7 | `grep -q "computeKernel.*_kernelQuality" src/DeepCBlur.cpp` | 0 | ✅ pass | <1s |

## Diagnostics

- **Kernel quality wiring:** Both H and V passes call `computeKernel(blur, _kernelQuality)`, so the kernel quality knob affects both axes. Inspectable via `nuke.toNode("DeepCBlur1")["kernel_quality"].value()`.
- **Intermediate buffer:** Named `intermediateBuffer` — greppable in source for structural verification.
- **Pass structure:** Two clearly delimited code sections with `HORIZONTAL PASS` and `VERTICAL PASS` comments for navigability.

## Deviations

None.

## Known Issues

None.

## Files Created/Modified

- `src/DeepCBlur.cpp` — Replaced 2D blur loop with separable H→V passes, removed ScratchBuf::kernel, updated header/function comments
- `scripts/verify-s01-syntax.sh` — New syntax verification script with mock DDImage headers
- `.gsd/milestones/M003/slices/S01/S01-PLAN.md` — Marked T02 done, added diagnostic verification check
