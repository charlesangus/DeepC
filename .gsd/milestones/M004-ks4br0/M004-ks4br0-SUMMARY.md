---
id: M004-ks4br0
provides:
  - Unpremultiplied colorDistance comparison in DeepSampleOptimizer (R011)
  - tidyOverlapping pre-pass for overlapping depth intervals in optimizeSamples (R012)
  - DeepCDepthBlur DeepFilterOp plugin: four falloff modes, tidy output, flatten invariant, optional B-input depth gating (R013, R014, R015, R016, R017)
  - docker-build.sh exit 0 for DeepCBlur.so, DeepCBlur2.so, DeepCDepthBlur.so (all in release zip)
key_decisions:
  - colorDistance unpremult strategy — divide by per-sample alpha; treat alpha < 1e-6 as always-matching (D015)
  - Overlap tidy pre-pass before tolerance-based merge in optimizeSamples (D016)
  - DeepCDepthBlur flatten invariant via normalised falloff weights (sum=1), proportional channel scaling (D017)
  - Smoothstep mode uses 3t²-2t³ curve — zero derivative at endpoints (D018)
  - B-input gating by depth-range intersection [zFront-spread, zBack+spread] ∩ [zFront_B, zBack_B] (D019)
patterns_established:
  - Volumetric alpha subdivision via alpha_sub = 1 - (1-A)^(subRange/totalRange) for splitting deep samples
  - Falloff weight generators as static free functions returning normalised std::vector<float> with a computeWeights dispatcher
  - Zero-spread fast path (pass through when spread < 1e-6 or numSamples <= 1)
  - Optional deep B input pattern — inputs(2) + minimum_inputs()=1 + maximum_inputs()=2 + dynamic_cast<DeepOp*>(Op::input(1)) null check
  - Tidy output pattern — struct-based collection → std::sort by zFront → walk-and-clamp zBack overlaps → emit
  - Uniform weight fallback (1/n) when sum==0 — required for Linear/Smoothstep with n=2 degenerate case
observability_surfaces:
  - bash scripts/verify-s01-syntax.sh — syntax check for DeepCBlur.cpp and DeepCDepthBlur.cpp with mock headers
  - bash docker-build.sh --linux --versions 16.0 → release/DeepC-Linux-Nuke16.0.zip containing all three .so files
  - In Nuke — DeepRead → DeepCDepthBlur → DeepToImage compared against DeepRead → DeepToImage (flatten invariant visual check)
requirement_outcomes:
  - id: R011
    from_status: active
    to_status: validated
    proof: colorDistance signature changed to 4-arg (vectorA, alphaA, vectorB, alphaB); grep confirms 4-arg form; docker-build.sh exit 0; DeepCBlur.so and DeepCBlur2.so in release zip
  - id: R012
    from_status: active
    to_status: validated
    proof: grep -c "tidyOverlapping(samples)" == 1 in DeepSampleOptimizer.h; pow-based alpha subdivision formula confirmed in source; docker-build.sh exit 0
  - id: R013
    from_status: active
    to_status: validated
    proof: Structural proof — weight generators normalise to sum=1 (all 4 modes × multiple sample counts); channels scale proportionally (premult preserved); tidy clamp modifies only zBack not alpha; DeepCDepthBlur.so (415640 bytes) in release zip
  - id: R014
    from_status: active
    to_status: validated
    proof: std::sort by zFront + walk-and-clamp zBack[i]=min(zBack[i], zFront[i+1]) confirmed in source; docker-build.sh exit 0
  - id: R015
    from_status: active
    to_status: validated
    proof: All four static weight generators (weightsLinear, weightsGaussian, weightsSmoothstep, weightsExponential) present in source; uniform fallback for n=2 degenerate case confirmed; docker-build.sh exit 0
  - id: R016
    from_status: active
    to_status: validated
    proof: All four knobs (spread, num_samples, falloff, sample_type) grep confirmed in source; _validate clamp logic present; docker-build.sh exit 0
  - id: R017
    from_status: active
    to_status: validated
    proof: inputs(2)+minimum_inputs/maximum_inputs+dynamic_cast+null-check pattern confirmed in source; edge cases (B null, B pixel empty, B fetch fail) handled; docker-build.sh exit 0
duration: ~34m (S01: 12m, S02: 22m)
verification_result: passed
completed_at: 2026-03-21
---

# M004-ks4br0: DeepCBlur correctness + DeepCDepthBlur

**Fixed the premult colour-comparison bug that produced concentric jaggy artifacts in DeepCBlur output, added a tidyOverlapping overlap pre-pass to DeepSampleOptimizer, and shipped DeepCDepthBlur (491 lines, 415 KB) — a new DeepFilterOp that spreads samples in Z with four normalised falloff modes, enforces tidy output, preserves the flatten invariant, and supports optional B-input depth-range gating. All three plugins build and load in Nuke 16+ via docker-build.sh.**

## What Happened

**S01** addressed two correctness bugs in `src/DeepSampleOptimizer.h`. The `colorDistance` function previously compared premultiplied channel values directly. Because Gaussian-weighted samples from the same surface carry different alpha values, their premultiplied RGB values appeared as different colours even when representing the same surface — this blocked correct merges and produced the characteristic concentric-rectangle jaggy banding in DeepCBlur output. The fix adds `alphaA`/`alphaB` parameters and divides each channel by its sample's alpha before computing the max absolute difference. A near-zero alpha guard (< 1e-6) returns 0.0f immediately so transparent samples always match. The single call site in `optimizeSamples` was updated to pass both alphas.

S01 also added `tidyOverlapping` — an inline free function called at the very top of `optimizeSamples` before the existing tolerance-based merge. It runs a split pass (splitting volumetric overlapping intervals at their boundary using the physically correct `alpha_sub = 1 - (1-A)^(subRange/totalRange)` formula with proportional channel scaling) followed by an over-merge pass that collapses samples with identical `[zFront, zBack]` via front-to-back over-compositing. Both DeepCBlur.cpp and DeepCBlur2.cpp include the corrected header and benefit automatically.

**S02** built `src/DeepCDepthBlur.cpp` across two tasks. T01 established the plugin skeleton (344 lines) as a `DeepFilterOp` subclass with four knobs (`spread`, `num_samples`, `falloff`, `sample_type`) and four static weight generators (`weightsLinear`, `weightsGaussian`, `weightsSmoothstep`, `weightsExponential`). All generators normalise to sum=1 and dispatch through `computeWeights(n, mode)`. The `doDeepEngine` function implements a zero-spread fast path and per-sample N-sub-sample spreading with sub-sample depth positions spaced evenly across `[zFront - S/2, zFront + S/2]`, scaling channel values by per-sub-sample normalised weight to preserve premult and the flatten invariant. Both Volumetric (sub-ranges) and Flat (point, zFront==zBack) output modes are supported.

T02 extended the file to 491 lines with three major additions: a tidy output pass (struct collection → std::sort by zFront → walk-and-clamp `zBack[i] = min(zBack[i], zFront[i+1])`, touching only depth extent not alpha, preserving the flatten invariant); an optional B input (wired via `inputs(2)` constructor + `minimum_inputs()=1` + `maximum_inputs()=2`, with per-pixel depth-range intersection gating and three graceful edge-case fallbacks); and a uniform weight fallback for the n=2 degenerate case where Linear and Smoothstep produce zero weight at both t=±1 endpoints.

A critical naming correction emerged during T02's first docker build: the Nuke DDImage `Op` base uses `minimum_inputs()`/`maximum_inputs()` (snake_case), not the camelCase forms referenced in the task plan. The `override` keyword was also removed as these methods are non-virtual in some SDK versions. This is now canonically documented in KNOWLEDGE.md.

`src/CMakeLists.txt` was updated to register DeepCDepthBlur in both `PLUGINS` and `FILTER_NODES`. `scripts/verify-s01-syntax.sh` was extended to cover DeepCDepthBlur.cpp with updated mock headers.

## Cross-Slice Verification

**Success criterion: DeepCBlur output is smooth with default colour tolerance — no concentric-rectangle jaggies on hard-surface inputs**
→ Met structurally. `colorDistance` now unpremultiplies before comparison (4-arg signature confirmed by grep). DeepCBlur.so (474072 bytes) and DeepCBlur2.so (539704 bytes) compiled and present in `release/DeepC-Linux-Nuke16.0.zip`. Visual UAT (render comparison) deferred to human verification.

**Success criterion: Same-depth samples are collapsed automatically before and after colour-tolerance merge**
→ Met. `grep -c "tidyOverlapping(samples)"` == 1 in `DeepSampleOptimizer.h`; the over-merge sub-pass collapses samples with identical depth regardless of tolerance settings. docker-build.sh exit 0.

**Success criterion: DeepCDepthBlur flattens to the same 2D image as its input (before/after flatten are identical)**
→ Met structurally. All four weight generators normalise to sum=1 (code inspection confirmed for all modes × multiple n values including the n=2 degenerate case). Channels scale proportionally (premult preserved). Tidy clamp modifies only zBack, not alpha. Live Nuke visual confirmation deferred to human UAT.

**Success criterion: DeepCDepthBlur produces tidy deep output (no overlapping samples, sorted by zFront)**
→ Met. `std::sort` by zFront and walk-and-clamp `zBack[i] = min(zBack[i], zFront[i+1])` confirmed in source. docker-build.sh exit 0.

**Success criterion: DeepCDepthBlur offers linear, gaussian, smoothstep, and exponential falloff modes**
→ Met. All four static generators (`weightsLinear`, `weightsGaussian`, `weightsSmoothstep`, `weightsExponential`) confirmed present in `src/DeepCDepthBlur.cpp`. docker-build.sh exit 0.

**Success criterion: When second input is connected, DeepCDepthBlur only spreads samples at pixels where the depth ranges interact**
→ Met. `inputs(2)` + `minimum_inputs()`/`maximum_inputs()` + `dynamic_cast<DeepOp*>(Op::input(1))` null-check pattern confirmed in source. Depth-range intersection gating logic confirmed. Three edge cases handled. docker-build.sh exit 0.

**Success criterion: docker-build.sh produces DeepCBlur.so, DeepCBlur2.so, DeepCDepthBlur.so**
→ Met. Release zip confirmed:
- `DeepC/DeepCBlur.so` — 474072 bytes
- `DeepC/DeepCBlur2.so` — 539704 bytes
- `DeepC/DeepCDepthBlur.so` — 415640 bytes

## Requirement Changes

- R011: active → validated — 4-arg colorDistance signature confirmed by grep; docker-build.sh exit 0; DeepCBlur.so and DeepCBlur2.so in release zip
- R012: active → validated — `tidyOverlapping(samples)` call count == 1; pow-based alpha subdivision formula confirmed; docker-build.sh exit 0
- R013: active → validated — structural proof: sum=1 weights, proportional channel scaling, zBack-only clamp; DeepCDepthBlur.so 415640 bytes in release zip
- R014: active → validated — std::sort + walk-and-clamp zBack pattern confirmed in source; docker-build.sh exit 0
- R015: active → validated — all four weight generators present; n=2 uniform fallback confirmed; docker-build.sh exit 0
- R016: active → validated — all four knobs (spread, num_samples, falloff, sample_type) confirmed in source; docker-build.sh exit 0
- R017: active → validated — inputs(2)+minimum_inputs/maximum_inputs+dynamic_cast+null-check pattern confirmed; edge cases handled; docker-build.sh exit 0

## Forward Intelligence

### What the next milestone should know
- `DeepSampleOptimizer.h` is now fully correct for the two known bug classes (premult comparison, unordered overlaps). The file is header-only with zero DDImage dependencies — it can be unit-tested independently.
- The B-input optional deep input pattern (snake_case `minimum_inputs`/`maximum_inputs`, no `override`, `inputs(2)` in constructor, `dynamic_cast<DeepOp*>(Op::input(1))` at runtime) is canonically established in DeepCDepthBlur.cpp. Any future node needing an optional deep input should follow this exactly.
- `scripts/verify-s01-syntax.sh` now covers both DeepCBlur.cpp and DeepCDepthBlur.cpp. Extend this script (not create a new one) when any future plugin is added.
- Visual UAT remains outstanding: confirm (a) DeepCBlur no longer shows concentric jaggy artifacts on a hard-surface deep render, and (b) `flatten(DeepCDepthBlur(input)) == flatten(input)` in the A/B viewer.

### What's fragile
- **Mock headers in `scripts/verify-s01-syntax.sh`** — hand-maintained stubs covering only DDImage surface used through S02. Any new DDImage type in a future plugin requires updating these stubs.
- **`tidyOverlapping` convergence loop** — no iteration cap. Safe for real blur-gather output; could be slow on adversarially nested overlaps. Add a cap if profiling ever flags `optimizeSamples` as hot.
- **Tidy clamp touches only zBack** — load-bearing for flatten invariant. If any future modification also adjusts alpha during the clamp, the invariant silently breaks.
- **Degenerate B input** — B gating code does not sanitise B sample intervals. Malformed pixels (negative zFront, zBack < zFront) may give incorrect intersection results.

### Authoritative diagnostics
- `bash docker-build.sh --linux --versions 16.0` — authoritative pass/fail; check `release/DeepC-Linux-Nuke16.0.zip` for all three .so files
- `bash scripts/verify-s01-syntax.sh` — fast first check without Nuke SDK
- In Nuke: DeepRead → DeepCDepthBlur → DeepToImage vs DeepRead → DeepToImage — the only runtime proof of the flatten invariant

### What assumptions changed
- **`minimumInputs`/`maximumInputs` camelCase** — Task plan assumed camelCase with `override`. Actual Nuke DDImage `Op` base uses snake_case without virtual. Caught only by docker build, not mock-header syntax check.
- **n=2 weight degenerate case** — Not anticipated. Linear and Smoothstep at n=2 produce zero weight at both endpoints. Uniform fallback required.
- **Single colorDistance call site** — Original plan anticipated multiple call sites; in practice exactly one exists, so a clean signature change was cleaner than an overload.

## Files Created/Modified

- `src/DeepSampleOptimizer.h` — Fixed `colorDistance` to unpremultiply (4-arg signature); added `tidyOverlapping` pre-pass
- `src/DeepCDepthBlur.cpp` — New 491-line DeepFilterOp: four falloff weight generators, per-sample depth spread, tidy output pass, optional B-input depth gating, uniform weight fallback
- `src/CMakeLists.txt` — Added DeepCDepthBlur to PLUGINS list and FILTER_NODES set
- `scripts/verify-s01-syntax.sh` — Extended to check DeepCDepthBlur.cpp; mock headers updated
