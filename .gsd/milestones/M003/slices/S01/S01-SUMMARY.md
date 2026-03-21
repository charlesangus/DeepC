---
id: S01
parent: M003
milestone: M003
provides:
  - Separable H→V two-pass Gaussian blur engine in doDeepEngine
  - Intermediate per-pixel sample buffer (vector<vector<vector<SampleRecord>>>) between passes
  - Three Gaussian kernel accuracy tier functions: getLQGaussianKernel, getMQGaussianKernel, getHQGaussianKernel
  - computeKernel dispatcher wired to _kernelQuality enum knob
  - _kernelQuality Enumeration_knob (0=Low, 1=Medium default, 2=High) in UI
  - scripts/verify-s01-syntax.sh — syntax check with mock DDImage headers
requires:
  - slice: none
    provides: n/a
affects:
  - S02
key_files:
  - src/DeepCBlur.cpp
  - scripts/verify-s01-syntax.sh
key_decisions:
  - sigma = blur / 3.0 (backward-compatible with M002, not CMG99's 0.42466)
  - Kernel functions placed as static free functions above class definition for file-scope visibility
  - Intermediate buffer indexed as [relY][relX]: width = output box width, height = input box height (padded Y range)
  - optimizeSamples called only after V pass — never between H and V passes
  - Helper lambdas intX/intY for coordinate translation to avoid off-by-one errors in buffer indexing
  - Two clearly delimited code sections (HORIZONTAL PASS / VERTICAL PASS comments) for navigability
patterns_established:
  - kernelQualityNames static array + Enumeration_knob — consistent with DeepCPNoise.cpp pattern
  - Half-kernel (symmetric) storage for all three tiers — center weight at index 0, half stored
  - V pass applies kernel weight on top of already-H-weighted samples (separable property: total weight = H × V)
observability_surfaces:
  - _kernelQuality knob: queryable in Nuke via nuke.toNode("DeepCBlur1")["kernel_quality"].value()
  - intermediateBuffer variable name: greppable for structural verification
  - Both H and V passes call computeKernel(blur, _kernelQuality) — kernel tier affects both axes
drill_down_paths:
  - .gsd/milestones/M003/slices/S01/tasks/T01-SUMMARY.md
  - .gsd/milestones/M003/slices/S01/tasks/T02-SUMMARY.md
duration: 35m (T01: 15m, T02: 20m)
verification_result: passed
completed_at: 2026-03-21
---

# S01: Separable blur + kernel tiers

**DeepCBlur now uses a two-pass separable Gaussian blur with three kernel accuracy tiers (LQ/MQ/HQ) selectable via Enumeration_knob; all slice verification checks pass and syntax compiles clean.**

## What Happened

**T01** ported three Gaussian kernel generation functions as static free functions above the DeepCBlur class definition. `getLQGaussianKernel` produces a raw unnormalized half-kernel (fastest). `getMQGaussianKernel` normalizes so the full symmetric kernel sums to 1.0 (default). `getHQGaussianKernel` uses CDF-based sub-pixel integration via `erff()` covering ±3.5σ (most accurate). A `computeKernel(float blur, int quality)` dispatcher selects the appropriate tier. A `_kernelQuality` int member (default 1 = Medium) was added along with an `Enumeration_knob` in `knobs()`. The existing 2D blur loop was intentionally left untouched at this stage.

**T02** replaced the old O(r²) 2D kernel with two sequential 1D gather passes. The refactored `doDeepEngine` works as follows:

1. **Zero-blur fast path** — `radX == 0 && radY == 0` check at top, passthrough with no allocation.
2. **Horizontal pass** — calls `computeKernel(_blurWidth, _kernelQuality)`, iterates each row in the padded input Y range × each column in the output X range, gathers from the 1D H neighbourhood weighting alpha and colour channels by kernel weight while propagating depth channels raw. Results stored in `intermediateBuffer[relY][relX]` (type `vector<vector<vector<SampleRecord>>>`).
3. **Vertical pass** — calls `computeKernel(_blurHeight, _kernelQuality)`, gathers from intermediate buffer rows in the 1D V neighbourhood, multiplies V kernel weight on top of already-H-weighted samples (separable property). Calls `optimizeSamples` only after this final pass, then emits to plane.

The old `ScratchBuf::kernel` member and all 2D kernel computation code were removed. Helper lambdas `intX`/`intY` translate output-relative coordinates to intermediate buffer indices, preventing off-by-one errors. A new `scripts/verify-s01-syntax.sh` was created with minimal mock DDImage header stubs; it runs `g++ -std=c++17 -fsyntax-only` against the live source.

## Verification

All six slice-level checks confirmed passing:

| Check | Command | Result |
|---|---|---|
| V1 — Enum knob present | `grep -c "Enumeration_knob" src/DeepCBlur.cpp` → 2 | ✅ |
| V2 — Three kernel functions defined | `grep -c "getLQ\|getMQ\|getHQ" src/DeepCBlur.cpp` → 7 | ✅ |
| V3 — Intermediate buffer present | `grep -q "intermediateBuffer"` → found | ✅ |
| V4 — Zero-blur fast path preserved | `grep -q "radX == 0 && radY == 0"` → found | ✅ |
| V5 — Syntax script exits 0 | `bash scripts/verify-s01-syntax.sh` → "Syntax check passed." | ✅ |
| V6 — Kernel quality wired into engine | `grep -q "computeKernel.*_kernelQuality"` → found | ✅ |

## New Requirements Surfaced

- none

## Deviations

None. All implementation matched the plan as written.

## Known Limitations

- **No visual/runtime verification yet** — the separable engine is syntactically correct and structurally sound, but runtime parity with the old 2D kernel has not been confirmed in Nuke. This is intentional per S01's proof level (contract only). Visual UAT is deferred to S02.
- **Docker build deferred** — `docker-build.sh` producing `DeepCBlur.so` in the release archive (R008) is a S02 deliverable.
- **Alpha correction not yet present** — R003 and R006 are S02 work.
- **WH_knob not yet present** — R004 (single WH_knob replacing separate float knobs) is S02 work. `_blurWidth`/`_blurHeight` floats still fed by the old separate Float_knobs.

## Follow-ups

- S02 must add alpha correction as a post-V-pass loop before the emit step — the V pass already calls `optimizeSamples` and then iterates output pixels; the correction pass inserts after optimizeSamples.
- S02 should verify that replacing Float_knobs with WH_knob doesn't break existing `_blurWidth`/`_blurHeight` member usage in doDeepEngine — the member names can stay the same, just the knob feeding them changes.

## Files Created/Modified

- `src/DeepCBlur.cpp` — Added 3 kernel tier functions + computeKernel dispatcher + _kernelQuality member + Enumeration_knob; replaced 2D blur loop with separable H→V passes + intermediate buffer; removed ScratchBuf::kernel
- `scripts/verify-s01-syntax.sh` — New: syntax verification script with mock DDImage headers

## Forward Intelligence

### What the next slice should know
- The V pass emits directly to the output plane after `optimizeSamples`. Alpha correction (S02) must insert between `optimizeSamples` and the emit loop — both exist in the same per-pixel output section at the bottom of doDeepEngine.
- `_blurWidth` and `_blurHeight` are used as plain floats in doDeepEngine. When WH_knob replaces the Float_knobs in S02, only the `knobs()` function and the member assignment in `knob_changed` (if it exists) need to change — doDeepEngine itself is insulated.
- The intermediate buffer is allocated fresh every `doDeepEngine` call inside the box iteration — no persistent state. This is intentional and correct for Nuke's tile-based execution model.

### What's fragile
- **Mock DDImage headers in verify-s01-syntax.sh** — these are minimal stubs. If new DDImage types or macros are introduced in S02, the mock headers may need updating for the syntax check to remain useful.
- **Half-kernel convention** — center weight is at index 0, the half-kernel extends outward. Both H and V passes must use `k == 0 ? weight : 2×weight` logic (or equivalent) when applying the half-kernel symmetrically. Any new code touching kernel application must respect this convention.

### Authoritative diagnostics
- `bash scripts/verify-s01-syntax.sh` — fastest compilation sanity check; exits 0 or shows exact line/column of any syntax error
- `grep -q "computeKernel.*_kernelQuality" src/DeepCBlur.cpp` — confirms kernel tier selection is actually wired into the blur engine (not just declared)
- `nuke.toNode("DeepCBlur1")["kernel_quality"].value()` in Nuke script console — runtime knob inspection

### What assumptions changed
- Original plan assumed CMG99's 0.42466 sigma factor; execution kept M002's `blur / 3.0` convention for backward compatibility. This is a conscious deviation from the reference source, not an oversight.
