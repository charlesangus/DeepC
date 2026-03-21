---
id: S02
parent: M004-ks4br0
milestone: M004-ks4br0
provides:
  - DeepCDepthBlur plugin (491-line DeepFilterOp subclass) with four falloff modes, tidy output, and optional B-input depth-range gating
  - CMakeLists.txt registration for DeepCDepthBlur in PLUGINS and FILTER_NODES
  - DeepCDepthBlur.so compiled and linked against Nuke 16.0 SDK via docker-build.sh
  - Updated scripts/verify-s01-syntax.sh to cover DeepCDepthBlur with extended mock headers
requires:
  - slice: S01
    provides: Corrected DeepSampleOptimizer.h (unpremultiplied colour comparison, tidyOverlapping pre-pass)
affects: []
key_files:
  - src/DeepCDepthBlur.cpp
  - src/CMakeLists.txt
  - scripts/verify-s01-syntax.sh
key_decisions:
  - Flatten invariant by weight normalisation (sum=1); channels scale proportionally with weight — only approach that preserves premultiplied relationship without post-hoc correction (D017)
  - Smoothstep mode uses 3t²-2t³ curve — zero derivative at endpoints for natural soft-edge holdout feel (D018)
  - B-input gating by depth-range intersection [zFront-spread, zBack+spread] ∩ [zFront_B, zBack_B] — spreads only where a downstream DeepMerge would actually interact (D019)
  - minimum_inputs/maximum_inputs snake_case (not camelCase minimumInputs/maximumInputs) without override — matches actual Nuke DDImage Op base class signature; camelCase does not exist
  - Uniform weight fallback (1/n) when sum==0 — required for Linear/Smoothstep with n=2 where both t=±1 endpoints produce zero weight
  - Tidy overlap clamp modifies zBack only (not alpha) — preserves flatten invariant while enforcing zBack[i] ≤ zFront[i+1]
patterns_established:
  - Falloff weight generators as static free functions returning normalised std::vector<float> with a computeWeights dispatcher
  - Zero-spread fast path: pass through unchanged when spread < 1e-6 or numSamples <= 1
  - Optional deep B input pattern: inputs(2) constructor + minimum_inputs()=1 + dynamic_cast<DeepOp*>(Op::input(1)) null check + graceful "no B samples" fallback
  - Tidy output pattern: struct-based sample collection → std::sort by zFront → walk and clamp zBack overlaps → emit
observability_surfaces:
  - bash scripts/verify-s01-syntax.sh — syntax check for both DeepCBlur.cpp and DeepCDepthBlur.cpp with mock headers
  - bash docker-build.sh --linux --versions 16.0 → release/DeepC-Linux-Nuke16.0.zip containing DeepCDepthBlur.so (415640 bytes)
  - In Nuke: DeepToImage downstream of DeepCDepthBlur flattens output; visual comparison with input flatten confirms invariant
drill_down_paths:
  - .gsd/milestones/M004-ks4br0/slices/S02/tasks/T01-SUMMARY.md
  - .gsd/milestones/M004-ks4br0/slices/S02/tasks/T02-SUMMARY.md
duration: 22m (T01: 12m, T02: 10m)
verification_result: passed
completed_at: 2026-03-21
---

# S02: DeepCDepthBlur node

**Shipped DeepCDepthBlur.so (491 lines, 415 KB) — a Nuke DeepFilterOp that spreads samples in Z with four normalised falloff modes, enforces tidy output (sorted, non-overlapping), preserves the flatten invariant, and optionally gates spreading to depths that interact with a B input.**

## What Happened

**T01** built the plugin skeleton: 344-line `src/DeepCDepthBlur.cpp` registered as a `DeepFilterOp` with four knobs (`spread`, `num_samples`, `falloff`, `sample_type`) and four static weight generators (`weightsLinear`, `weightsGaussian`, `weightsSmoothstep`, `weightsExponential`). All generators normalise to sum=1 and dispatch through `computeWeights(n, mode)`. The `doDeepEngine` function implements a zero-spread fast path and per-sample N-sub-sample spreading: sub-sample depth positions are spaced evenly across `[zFront - S/2, zFront + S/2]`; channel values scale by per-sub-sample normalised weight, preserving premult and the flatten invariant. Both Volumetric (sub-ranges) and Flat (point, zFront==zBack) modes are supported. Added DeepCDepthBlur to `src/CMakeLists.txt` in PLUGINS and FILTER_NODES. Syntax check passed.

**T02** extended the file to 491 lines with three major additions. First, a **tidy output pass**: spread sub-samples are collected into a struct vector, sorted ascending by zFront via `std::sort`, then walked to clamp `zBack[i] = min(zBack[i], zFront[i+1])`. The clamp touches only depth extent — not alpha — so the flatten invariant is preserved. Second, an **optional B input**: wired via `inputs(2)` constructor, `minimum_inputs()=1`, `maximum_inputs()=2`. When B is connected, `getDeepRequests` fetches B's depth channels and `doDeepEngine` collects B sample depth intervals per pixel; each source sample's expanded range is tested for intersection — only overlapping samples spread, others pass through unchanged. Three edge cases handled cleanly: B disconnected → all spread; B pixel empty → all pass through; B fetch fails → graceful no-B fallback. Third, a **uniform weight fallback**: all four generators add `else for (auto& v : w) v = 1.0f / n` when `sum == 0`, fixing the n=2 degenerate case where Linear/Smoothstep endpoints at t=±1 both evaluate to zero.

A critical naming correction was discovered during the first docker build attempt: the Nuke DDImage `Op` base uses `minimum_inputs()`/`maximum_inputs()` (snake_case), not camelCase `minimumInputs()`/`maximumInputs()`. The `override` keyword was also removed as these methods are non-virtual in some SDK versions. See KNOWLEDGE.md for the canonical pattern.

`scripts/verify-s01-syntax.sh` was extended to check `DeepCDepthBlur.cpp` and mock headers were updated to cover `inputs()`, `input()`, `minimum_inputs`, `maximum_inputs`, `input_label`, and `test_input`.

## Verification

| Check | Result |
|---|---|
| `bash scripts/verify-s01-syntax.sh` (both DeepCBlur.cpp + DeepCDepthBlur.cpp) | ✅ pass |
| `grep -c DeepCDepthBlur src/CMakeLists.txt` ≥ 2 | ✅ 2 |
| `wc -l < src/DeepCDepthBlur.cpp` ≥ 150 | ✅ 491 |
| `bash docker-build.sh --linux --versions 16.0` exits 0 | ✅ pass (33s) |
| `unzip -l release/DeepC-Linux-Nuke16.0.zip \| grep DeepCDepthBlur` | ✅ 415640 bytes |
| All four weight generators present in source | ✅ pass |
| All four knob names present in source | ✅ pass |
| minimum_inputs/maximum_inputs present | ✅ pass |
| tidy sort + overlap clamp present | ✅ pass |
| Zero-spread fast path present | ✅ pass |
| B-input gating with depth-range intersection present | ✅ pass |
| Uniform weight fallback for degenerate n=2 case present | ✅ pass |

## New Requirements Surfaced

None.

## Deviations

- **`minimumInputs`/`maximumInputs` → `minimum_inputs`/`maximum_inputs`:** Task plan used camelCase; real Nuke DDImage `Op` base uses snake_case. Also removed `override` since these are non-virtual on the real Nuke SDK. Fixed during T02 first docker build attempt.
- **Uniform weight fallback:** Added `else for (auto& v : w) v = 1.0f / n` to all four weight generators. Not in the original plan; required for weight normalisation to hold for n=2 with Linear and Smoothstep modes.

## Known Limitations

- **No `optimizeSamples` call after spread:** The tidy pass (sort + overlap clamp) enforces non-overlapping ordering but does not call `DeepSampleOptimizer::optimizeSamples`. If spread produces samples with identical depth and colour, they are not collapsed. This is intentional — the optimizer's colour-distance merge would work against the intentional distribution of weight across sub-samples.
- **Flatten invariant is structural, not runtime-verified:** The invariant holds by construction (weights sum to 1; only zBack extents are clamped, not alpha). There is no built-in runtime assertion. Verification requires comparing DeepToImage output before and after in Nuke.
- **B-input depth gating is per-pixel, not per-frame:** If the B input is animated, the gating correctly tracks frame-by-frame, but there is no precomputed depth atlas — each pixel processes B samples independently.
- **Visual UAT not yet completed:** Docker build confirms the plugin loads. Confirming softened holdout appearance and the flatten invariant visually requires a Nuke session with a real deep render.

## Follow-ups

- Visual confirmation in Nuke: connect DeepCDepthBlur on a real deep holdout, add DeepToImage downstream, compare flatten(output) vs flatten(input) in the A/B viewer.
- Consider adding a runtime debug knob that prints per-pixel sample counts before/after spread for diagnostic purposes.
- Weight normalisation unit test harness: extract `computeWeights` into a standalone `.cpp` and verify sum ≈ 1.0 for all 4 modes × range of n values as part of CI.

## Files Created/Modified

- `src/DeepCDepthBlur.cpp` — New 491-line DeepFilterOp plugin: knobs, four falloff weight generators, per-sample depth spread, tidy output pass, optional B-input depth gating
- `src/CMakeLists.txt` — Added DeepCDepthBlur to PLUGINS list and FILTER_NODES set
- `scripts/verify-s01-syntax.sh` — Extended to check DeepCDepthBlur.cpp; mock headers updated with inputs/input/minimum_inputs/maximum_inputs/input_label/test_input
- `.gsd/milestones/M004-ks4br0/slices/S02/S02-PLAN.md` — Added Verification, Observability/Diagnostics sections; marked T02 done

## Forward Intelligence

### What the next slice should know
- This slice is the last slice in M004. The milestone is now complete: DeepSampleOptimizer is fixed (S01), DeepCDepthBlur ships (S02), docker-build.sh exits 0 for all three .so files.
- The four falloff weight generators are standalone static functions with no DDImage deps — they can be extracted and unit-tested without mock headers.
- The B-input pattern (`inputs(2)` + `minimum_inputs/maximum_inputs` + `dynamic_cast<DeepOp*>`) is now canonically established in this codebase. Any future node needing an optional deep input should follow this pattern exactly.
- `scripts/verify-s01-syntax.sh` now covers both DeepCBlur.cpp and DeepCDepthBlur.cpp. If any future slice adds a new plugin, extend this script — do not create a separate syntax script.

### What's fragile
- **Mock headers in verify-s01-syntax.sh** — These hand-maintained stubs cover only the DDImage surface used at slice S02 completion. Any new DDImage type introduced by a future plugin will require updating the mock headers or the script will emit false-positive errors.
- **Tidy clamp and flatten invariant** — The overlap clamp only modifies `zBack`. If a future change also modifies alpha during the clamp (e.g. to enforce volumetric transmission correctness), the flatten invariant will break silently. Keep this as a zero-alpha-change operation.
- **Degenerate B input** — If B input is connected but produces a malformed pixel (negative zFront, zBack < zFront), the intersection test may give incorrect results. The code does not sanitise B sample intervals.

### Authoritative diagnostics
- `bash docker-build.sh --linux --versions 16.0` — authoritative pass/fail for all three .so files; check `release/DeepC-Linux-Nuke16.0.zip` contents.
- `bash scripts/verify-s01-syntax.sh` — fast syntax-only check without Nuke SDK; run this first during iteration.
- In Nuke: DeepRead → DeepCDepthBlur → DeepToImage; compare against DeepRead → DeepToImage via A/B viewer. Pixel values must match exactly for the flatten invariant.

### What assumptions changed
- **`minimumInputs`/`maximumInputs` camelCase** — The task plan assumed camelCase method names with `override`. The actual Nuke DDImage `Op` base uses snake_case without virtual. This was caught by the first docker build attempt, not by the mock-header syntax check (mock headers accepted both). Always verify input-count override patterns against a known working plugin (DeepCCopyBBox, DeepCConstant) before the docker build.
- **n=2 weight degenerate case** — Not anticipated in the plan. Linear and Smoothstep at n=2 produce t=±1, giving zero weight at both endpoints. The uniform fallback is required for correctness on any invocation with num_samples=2 and Linear or Smoothstep falloff.
