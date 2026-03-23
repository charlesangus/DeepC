# Knowledge Base

<!-- Append-only. Add entries that would save future agents from repeating investigation or hitting the same issues.
     Only add entries that are genuinely useful — don't pad with obvious observations. -->

## Deep Spatial Ops — Intermediate Buffer Patterns

### Half-kernel convention: center weight at index 0
**Context:** DeepCBlur separable kernel (S01, M003)

All three kernel tiers (LQ/MQ/HQ) store a half-kernel where index 0 is the center weight and subsequent indices extend outward. The application loop must apply `kernel[0]` to the source pixel and `kernel[k]` symmetrically at both `+k` and `-k` offsets. Any new code touching kernel application (e.g. adding a new tier or a new spatial pass) must respect this convention — violating it silently produces blur that's too narrow and incorrectly weighted.

### optimizeSamples placement in multi-pass ops
**Context:** DeepCBlur H→V separable blur (S01, M003)

`optimizeSamples` must only be called after the final pass (V pass). Calling it after the H pass collapses samples that still need to accumulate vertical weights from the V pass, producing permanently incorrect output with no error. The rule: **optimize only after all spatial accumulation is complete.**

### Mock DDImage headers in syntax scripts
**Context:** `scripts/verify-s01-syntax.sh` (S01, M003)

The syntax verification script creates minimal mock DDImage header stubs in a temp directory. These stubs cover only the types and macros used at the time of writing. If new DDImage types are introduced in later slices, the mock headers must be updated or `verify-s01-syntax.sh` will emit false-positive errors. Check the mock headers first if the syntax script starts failing after a refactor that shouldn't have broken anything.

### Intermediate buffer coordinate translation
**Context:** DeepCBlur separable blur (S01, M003)

The intermediate buffer is indexed as `[relY][relX]` where both indices are *output-box-relative*, not absolute pixel coordinates. The helper lambdas `intX(x)` and `intY(y)` translate absolute pixel coords to buffer indices. Without these lambdas, the two coordinate systems (absolute vs. relative) are easy to confuse and produce silent off-by-one errors where the blur appears shifted or corrupted. Future deep spatial ops with intermediate buffers should adopt the same lambda pattern.

### V pass applies weight multiplicatively on H-accumulated samples
**Context:** DeepCBlur separable blur (S01, M003)

In the V pass, kernel weight is applied to samples that already carry H-pass weight baked in. The separable property means the total 2D weight equals H_weight × V_weight, so the V pass simply multiplies by its 1D kernel weight. This is correct and intentional — do not re-normalize or reset weights between passes.

## DDImage Knob Patterns

### WH_knob requires double[2] member and SetRange(double,double) overload
**Context:** DeepCBlur UI refactor (S02, M003)

`WH_knob` binds to a `double[2]` member (not `float[2]`). All call sites that pass the values to Nuke API functions expecting `float` must cast explicitly: `static_cast<float>(_blurSize[0])`. The `SetRange` call for `WH_knob` uses double literals — if your mock headers only have a `SetRange(Knob_Callback, float, float)` overload, the syntax check will fail. Add a `SetRange(Knob_Callback, double, double)` overload.

### grep -c contract for single-use macros
**Context:** `scripts/verify-s01-syntax.sh` (S02, M003)

If slice verification uses `grep -c "SomeMacro"` == 1 to prove a knob is present, remove the literal string from comments, HELP text, and any other non-code context to keep the count at exactly 1. The count contract is deliberately tight to catch accidental duplication.

### Alpha correction pass ordering in doDeepEngine
**Context:** DeepCBlur alpha correction (S02, M003)

The alpha correction pass must sit between `optimizeSamples` and the emit loop. Placing it before `optimizeSamples` means correction runs on un-optimized samples that may be collapsed later (changing the per-sample channel values correction computed). Placing it after the emit loop means samples have already been written — no effect. The correct order is: blur H → blur V → optimizeSamples → alpha correction → emit.

## DDImage Build Patterns

### docker-build.sh is the only reliable compilation proof
**Context:** M003 final verification

`scripts/verify-s01-syntax.sh` (g++ -fsyntax-only with mock headers) catches C++ syntax errors quickly but cannot catch DDImage API mismatches — it uses hand-maintained stubs. `docker-build.sh --linux --versions 16.0` compiles against the real Nuke SDK inside Docker and is the authoritative pass/fail signal. Always run both in sequence: syntax check first for fast iteration, docker build last for final proof.

### Knob grep contracts require literal-free comments and HELP strings
**Context:** M003/S02 WH_knob verification

When a slice defines a `grep -c "SomeMacro" == N` contract to prove a knob is used exactly once, that count includes occurrences in comments, HELP text, and string literals — not just code. Remove the literal from all non-code contexts before committing. This is especially relevant for macros like `WH_knob`, `BeginClosedGroup`, `Bool_knob` where documentation might naturally mention the macro name.

## DeepCBlur-Specific Patterns

### _blurSize[2] replaces _blurWidth / _blurHeight (M003+)
**Context:** M003/S02 WH_knob refactor

As of M003, `_blurWidth` and `_blurHeight` no longer exist. The authoritative blur size is `double _blurSize[2]` (index 0 = width, 1 = height). All call sites cast to float: `static_cast<float>(_blurSize[0])`. Any future feature touching blur size must read from `_blurSize`, not create new float members.

### Alpha correction must survive any future emit loop restructure
**Context:** M003/S02 alpha correction placement

The correction pass sits between `optimizeSamples` and the emit loop. This ordering is load-bearing: correction before `optimizeSamples` operates on un-optimized samples that may later be merged (invalidating corrections); correction after emit is a no-op. If the emit loop is ever refactored for a new feature (Z-blur, etc.), verify this ordering is preserved before shipping.

## DeepSampleOptimizer — S01/M004 Patterns

### colorDistance now requires both alphas — 4-arg signature
**Context:** DeepSampleOptimizer colour comparison fix (S01, M004)

`colorDistance` was changed from a 2-arg `(vectorA, vectorB)` to a 4-arg `(vectorA, alphaA, vectorB, alphaB)` signature. There is exactly one call site in `optimizeSamples`. Any future call site must pass both sample alphas. The old signature no longer exists — code that compiles against mock headers but uses the old 2-arg form will fail at Docker build time.

### tidyOverlapping is the first thing optimizeSamples does — do not move it
**Context:** DeepSampleOptimizer overlap tidy pre-pass (S01, M004)

`tidyOverlapping(samples)` is called at the very top of `optimizeSamples`, before the tolerance-based merge loop. Moving it after the merge loop would mean overlapping samples escape the tidy pass. Moving it out of `optimizeSamples` entirely would require all call sites to call it manually — easy to miss. The current placement is load-bearing; do not relocate it without updating all affected logic.

### Volumetric alpha subdivision: alpha_sub = 1 - (1-A)^(subRange/totalRange)
**Context:** DeepSampleOptimizer tidyOverlapping split pass (S01, M004)

When a volumetric deep sample spanning [zFront, zBack] is split at an interior depth boundary, the sub-sample's alpha is computed as `1 - pow(1-alpha, subRange/totalRange)`. Premultiplied channels scale proportionally: `channel_sub = channel * (alpha_sub / alpha)`. This is the physically correct formula for partial transmission through a homogeneous volume. Do NOT use a linear approximation — it produces incorrect compositing when sub-ranges are large fractions of the total range.

### tidyOverlapping convergence loop has no iteration cap
**Context:** DeepSampleOptimizer tidyOverlapping split pass (S01, M004)

The split pass iterates until no overlaps remain. On real blur-gather output this converges in one or two passes. On adversarial input (many nested overlaps), it could be slow. If performance profiling ever flags `optimizeSamples` as hot, adding an iteration cap (e.g. max 16 passes with a warning) is the right fix — not removing the loop.

## Nuke SDK Naming Conventions

### minimum_inputs/maximum_inputs are snake_case, not camelCase
**Context:** DeepCDepthBlur optional B input (S02, M004)

The Nuke DDImage `Op` base class uses `minimum_inputs()` and `maximum_inputs()` (snake_case). The task plan referenced `minimumInputs`/`maximumInputs` (camelCase) with `override` — neither exists. Do NOT use `override` on these methods; they are non-virtual in some SDK versions. The `inputs(N)` constructor call sets the initial count, while `minimum_inputs`/`maximum_inputs` control the valid range. See `DeepCCopyBBox.cpp` and `DeepCConstant.cpp` for canonical patterns.

### Weight generators must handle n=2 degenerate case
**Context:** DeepCDepthBlur falloff weights (S02, M004)

For Linear and Smoothstep falloffs with n=2, the two sample positions are t=-1 and t=1. Linear gives weight 0 at both endpoints (1-|t|=0); Smoothstep similarly. The sum is 0, and without a fallback the division produces NaN. All weight generators should include `else for (auto& v : w) v = 1.0f / n;` as a uniform fallback when `sum == 0`.

## DeepCDepthBlur — S02/M004 Patterns

### Optional B input: snake_case minimum_inputs/maximum_inputs, no override, inputs(2) constructor
**Context:** DeepCDepthBlur optional B input (S02, M004)

The Nuke DDImage `Op` base uses `minimum_inputs()` and `maximum_inputs()` (snake_case). The task plan referenced camelCase `minimumInputs`/`maximumInputs` with `override` — neither exists. This was caught only during the docker build, NOT by the mock-header syntax check (mock headers accepted both forms). The correct pattern, matching `DeepCCopyBBox.cpp` and `DeepCConstant.cpp`:
- Constructor: `inputs(2)` to set the initial input count
- `int minimum_inputs() const { return 1; }` — no override
- `int maximum_inputs() const { return 2; }` — no override
- Runtime fetch: `dynamic_cast<DeepOp*>(Op::input(1))` with null check

### Tidy output clamp must not touch alpha — only zBack
**Context:** DeepCDepthBlur tidy pass (S02, M004)

The overlap clamp (`zBack[i] = min(zBack[i], zFront[i+1])`) must modify only `zBack`. Modifying alpha to compensate for the trimmed Z extent would break the flatten invariant. The flatten invariant is maintained because alpha was already set by the normalised weight during spreading — trimming the depth interval's extent doesn't change the compositing weight. Future modifications to the tidy pass must preserve this: touch only Z extent, never alpha.

### Weight normalisation must handle sum==0 (n=2 degenerate case)
**Context:** DeepCDepthBlur falloff weights (S02, M004)

For Linear and Smoothstep falloffs with n=2, sample positions are at t=−1 and t=+1. Linear weight = 1−|t| = 0 at both; Smoothstep similarly. Sum = 0, causing division-by-zero (NaN). All weight generators must include a uniform fallback: `if (sum > 0.0f) for (auto& v : w) v /= sum; else for (auto& v : w) v = 1.0f / n;`. This was not in the original plan but is required for correct output at num_samples=2 with Linear or Smoothstep.

### Flatten invariant is structural, not runtime-asserted
**Context:** DeepCDepthBlur correctness guarantee (S02, M004)

`flatten(output) == flatten(input)` holds by construction: weights sum to 1 so total alpha is preserved; channels scale proportionally so premult is preserved; the tidy clamp touches only zBack not alpha. There is no runtime assertion. The only way to catch a regression is a visual A/B comparison in Nuke (DeepToImage before vs after DeepCDepthBlur). Consider adding a unit test harness that exercises `computeWeights` directly if CI is ever added.

## DeepCDepthBlur — M005-9j5er8 Patterns

### Multiplicative alpha decomposition requires double-precision intermediate
**Context:** DeepCDepthBlur doDeepEngine spreading (S01, M005)

`alphaSub = static_cast<float>(1.0 - std::pow(1.0 - static_cast<double>(srcAlpha), static_cast<double>(w)))` — the `double` intermediate is intentional. `float` arithmetic for `pow(1-α, w)` loses precision at extreme alpha values (near 0 or near 1) in ways that are visually detectable. The `static_cast<float>` on the result satisfies `-Wconversion` without changing the precision where it matters. Do not simplify to all-`float` arithmetic.

### Double 1e-6f guard pattern eliminates all zero-alpha deep image output
**Context:** DeepCDepthBlur zero-alpha guards (S01, M005)

Two separate guards are required, not one:
1. **Input-level guard** (`srcAlpha < 1e-6f`): Pass the sample through unchanged, skip spreading entirely. Prevents division-by-zero in `alphaSub / srcAlpha` premult scaling.
2. **Sub-sample-level guard** (`alphaSub < 1e-6f`): Drop the sub-sample silently. Prevents NaN propagation from degenerate `pow` results at pathological alpha values.

Both are necessary — the input guard does not protect against degenerate pow outputs, and the sub-sample guard alone doesn't protect against division-by-zero. `grep -c '1e-6f' src/DeepCDepthBlur.cpp` → ≥2 is the diagnostic command to verify both are present.

### Flatten invariant is provable at source level, not runtime-asserted
**Context:** DeepCDepthBlur correctness guarantee (S01, M005)

The invariant `flatten(DeepCDepthBlur(input)) == flatten(input)` is guaranteed by construction — the multiplicative formula `1 - pow(1-α, w)` is the exact inverse of Nuke's `1 - ∏(1-αᵢ)` flatten model when weights sum to 1. There is no runtime assertion. The only machine-executable check is `grep -q 'pow(1' src/DeepCDepthBlur.cpp`. Visual regression detection requires a Nuke A/B comparison (`DeepToImage` before vs after). If CI is ever added, test `computeWeights` for sum=1 directly.

### "ref" input label does not imply depth-range gating is implemented
**Context:** DeepCDepthBlur R017 partial coverage (S01, M005)

R017 describes two things: (1) input label "ref" with main input unlabelled — **implemented and validated in M005**. (2) Depth-range gating: only spread samples whose Z range intersects a ref sample's range — **not implemented**, explicitly deferred. Future milestones implementing this feature need non-trivial additions to `doDeepEngine` to iterate ref samples per input sample.

### C-style typedef structs need a tag name when forward-declared in C++
**Context:** poly.h / deepc_po_math.h ODR firewall (M006/S01/T01)

`typedef struct { ... } foo_t;` creates an anonymous struct with a typedef. You cannot forward-declare it in another header as `typedef struct foo_t;` — the names don't match. Fix: give the struct a tag: `typedef struct foo_s { ... } foo_t;` then forward-declare as `typedef struct foo_s foo_t;`. Also, the header that defines the full type must emit a preprocessor sentinel (`#define DEEPC_POLY_H`) so any co-included header can skip the forward declaration with `#ifndef DEEPC_POLY_H`.

### poly.h ODR pattern: single-TU inclusion via sentinel guard
**Context:** poly.h vendored from lentil (M006/S01/T01)

poly.h defines all its functions inline in the header (lentil style). To avoid ODR violations when multiple .cpp files are compiled into the same .so, include poly.h in exactly ONE translation unit (`DeepCDefocusPO.cpp`). deepc_po_math.h uses a forward declaration + `#ifndef DEEPC_POLY_H` guard so it compiles both standalone and when poly.h is included first.

### CMake syntax validation without Docker/Nuke SDK
**Context:** docker-build.sh unavailable in workspace (M006/S01/T04)

When Docker is absent, CMake syntax validity can be confirmed with: `cmake -S . -B /tmp/cmake-check-build -DNuke_ROOT=/nonexistent`. CMake parses all `CMakeLists.txt` syntax before calling `find_package(Nuke REQUIRED)`, which fails (expected). Any CMake syntax error in the plugin list, target definitions, or install rules surfaces before the Nuke SDK check. This is a useful intermediate gate between `g++ -fsyntax-only` and the full docker build.

### docker-build.sh is CI-only; syntax-only gate is the local fast path
**Context:** docker-build.sh unavailable in workspace (M006/S01/T04)

`docker-build.sh` requires a live Docker daemon and pulls/builds a NukeDockerBuild container image. It will never run in a workspace-only environment. The correct fast-feedback loop is: `bash scripts/verify-s01-syntax.sh` (mock headers, <5s) → `cmake -S . -B /tmp/cmake-check-build -DNuke_ROOT=/nonexistent` (CMake syntax, ~5s) → docker build in CI. Do not block slice completion on the docker gate if the workspace has no Docker.

## DeepCDefocusPO — S02/M006 Patterns

### Normalised [-1,1] sensor coordinates — use these, not mm
**Context:** DeepCDefocusPO PO scatter engine (S02, M006)

The lentil polynomial was fitted in a normalised coordinate space, but the exact physical sensor size assumed varies by .fit file. Using normalised `[-1,1]` sensor coordinates throughout avoids any sensor-size dependency. Pixel-to-norm: `sx = (px + 0.5 - half_w) / half_w`. Norm-to-pixel: `out_px = landing_x * half_w + half_w - 0.5`. Aperture radius in normalised units = `1 / fstop`. The research proposed `pix_per_mm` with a 36mm sensor, but the implementation (and the research's own recommended approach) use normalised coords. Do not add a sensor-size constant unless a specific .fit file requires it.

### Focal length hardcoded at 50mm in S02 — only for CoC culling
**Context:** DeepCDefocusPO focal length default (S02, M006)

`focal_length_mm = 50.0f` is used only for the CoC-based early cull (performance guard) in `renderStripe`. The actual bokeh size and shape come from the polynomial itself via the Newton iteration — the focal length constant does not affect scatter correctness. A wrong `focal_length_mm` produces suboptimal culling (too aggressive or too lenient) but no visual error. S04 adds a Float_knob; until then, 50mm is the default.

### Stripe-boundary seam: out-of-stripe scatter contributions are silently discarded
**Context:** DeepCDefocusPO PlanarIop stripe model (S02, M006)

`renderStripe` is called once per stripe. Scatter contributions from a deep sample in the current stripe that land in a neighbouring stripe are discarded (not wrapped). This produces a dark seam at stripe boundaries when bokeh radius is large relative to stripe height. This is an accepted limitation documented in a code comment. Mitigation: request a single full-height stripe by setting the output box to the full format in `_validate` — deferred to S04. Do not attempt to "fix" this by requesting broader deepEngine bounds, as that would over-fetch the Deep input without solving the output-write problem.

### PlanarIop's single-threaded renderStripe eliminates scatter buffer races
**Context:** DeepCDefocusPO base class selection (S01/S02, M006)

Stochastic aperture scatter writes to random output pixels (`imagePlane.writableAt(...) +=`). This is safe only because `PlanarIop::renderStripe` is single-threaded — Nuke calls `renderStripe` once per stripe, sequentially, never from two threads simultaneously for the same ImagePlane. If this were multi-threaded (e.g. via a Thread/Engine approach), the `+=` accumulation would be a data race requiring atomics or a mutex. D027 confirms PlanarIop was chosen specifically for this property. Do not change the base class without revisiting the scatter loop.

### Format::width()/height() must be stubbed in verify-s01-syntax.sh when renderStripe uses them
**Context:** verify-s01-syntax.sh mock headers (S02, M006)

When `renderStripe` calls `fmt.width()` and `fmt.height()` on a `const Format&`, the mock `Format` class in `verify-s01-syntax.sh` must expose these methods. S01's mock `Format` was empty; S02 extended it with `int width() const { return 2048; }` and `int height() const { return 1556; }`. Any future `renderStripe` refactor that calls new Format methods must extend the mock accordingly — the syntax check is the first gate and will fail otherwise.

## DeepCDefocusPO — S03/M006 Patterns

### Holdout transmittance lambda: unordered iteration is correct; hzf >= Z is the depth gate
**Context:** DeepCDefocusPO depth-aware holdout (S03, M006)

The `transmittance_at(ox, oy, Z)` lambda accumulates `T *= (1 - alpha_i)` over holdout samples using `getUnorderedSample`. This is correct and intentional — the product `∏(1-αᵢ)` is commutative, so sample order does not matter. The depth gate `if (hzf >= Z) continue` skips samples at or behind the scatter depth Z; only samples in front of (shallower than) the scatter depth contribute to blocking. Future nodes implementing DeepHoldout semantics should copy this exact pattern: unordered iteration, front-to-back transmittance product, `hzf >= Z` guard, `std::max(0.0f, T)` clamp, `holdoutConnected` boolean identity short-circuit.

### Holdout must be fetched at output pixel bounds, not input sample position
**Context:** DeepCDefocusPO depth-aware holdout (S03, M006)

`holdoutOp->deepEngine(bounds, ...)` is called at the **output-pixel bounds** (the same `bounds` passed to `renderStripe`), not at the position of the input Deep sample being scattered. This is the load-bearing guarantee that the holdout is never defocused through the lens: evaluating at the output pixel means the holdout mask is always sharp, regardless of how far out-of-focus the input sample is. Using input-pixel coordinates would scatter the holdout through the Newton iteration, blurring the holdout boundary — the double-defocus problem. The deepRequest in `getRequests` must also use the output-pixel box for the same reason.

### DeepPlane::getPixel takes (y, x) — row-major DDImage convention
**Context:** DeepCDefocusPO holdout plane access (S03, M006)

`DeepPlane::getPixel(row, col)` takes `(y, x)` in DDImage convention — row first, column second. The transmittance_at lambda calls `holdoutPlane.getPixel(oy, ox)`. This is the opposite of the common `(x, y)` expectation. Getting this wrong produces a transposed holdout mask that appears correct for square formats but fails for non-square images. The mock header stubs `DeepPlane::getPixel` with `(int y, int x)` — check the signature there when debugging.

## DeepCDefocusPO — S04/M006 Patterns

### Verify-script Knobs.h heredoc must be extended for every new knob helper
**Context:** DeepCDefocusPO knob polish (S04, M006)

The `scripts/verify-s01-syntax.sh` mock `Knobs.h` heredoc must be extended whenever a new DDImage knob helper is used in `knobs()`. Any undeclared helper will fail the syntax check with "undeclared identifier". This is not a Nuke SDK change — it is a maintenance requirement of the mock-header approach. Common helpers requiring stubs: `Divider`, `BeginClosedGroup`, `EndGroup`, `Bool_knob`, `WH_knob`. Check the mock first when the syntax script starts failing after adding a new knob.

### Unit-labelled knob names are the DeepCDefocusPO convention
**Context:** DeepCDefocusPO knob polish (S04, M006)

All knobs with physical units follow the `"name (unit)"` pattern: `"focus distance (mm)"`, `"focal length (mm)"`. This makes unit expectations unambiguous to artists. Apply the same pattern to any future knobs on this node (or sibling nodes) that carry physical units.

### Divider knob usage: insert between logical groups, stub needed in mock
**Context:** DeepCDefocusPO knob layout (S04, M006)

`Divider(f, "")` creates a horizontal rule between knob groups. The two DDImage arguments are the callback handle and an (optional) empty label. The verify-script mock must provide `inline void Divider(Knob_Callback, const char*) {}` or the syntax check fails. A Divider stub does not need any body — it is a pure UI layout hint.

## DeepCDefocusPO — S05/M006 Patterns

### CMake icon glob is automatic: just create the file in icons/
**Context:** DeepCDefocusPO icon registration (S05, M006)

The root `CMakeLists.txt` uses `file(GLOB ICONS "icons/DeepC*.png")` to auto-install all matching PNGs at build time. Creating `icons/DeepCMyNewNode.png` is sufficient — no CMakeLists.txt edit is needed. This applies to all existing and future DeepC nodes. Without the file, the menu entry loads but shows no icon; the icon is not required for functionality.

### Stale comment blocks from early slices must be removed before milestone close
**Context:** DeepCDefocusPO header cleanup (S05, M006)

Development across multiple slices often leaves phase comments like `S01 state: skeleton only` or `S02 replaces the renderStripe body`. These must be removed in the final integration slice (S05) so the merged code describes the implemented state, not the development history. The S05 verify contract `! grep -q 'S01 state'` enforces this. Future milestones should add a stale-comment-removal contract to their S05 equivalent.

### Proof separation: structural gates in workspace, runtime gates in CI
**Context:** DeepCDefocusPO final verification model (S05, M006)

M006's proof strategy cleanly separates what can be proven in the workspace from what requires CI/Nuke. Structural gates (syntax check, cmake-parse, grep contracts, icon presence) run locally in <10s. Runtime gates (docker build → .so, Nuke session → bokeh/holdout/CA visibility) require infrastructure unavailable in the workspace. Document this separation explicitly in the slice plan so slice-completion decisions are not blocked by missing runtime infrastructure.

### Third-party vendored sources require THIRD_PARTY_LICENSES.md entry before milestone close
**Context:** lentil/poly.h attribution (S05, M006)

`src/poly.h` was vendored from `lentil` (Johannes Hanika / hanatos, MIT) in S01 without a license entry. S05 added the entry. Any future vendor of a header-only or snippet library must add the entry in the same slice it is vendored — not deferred to the final integration slice. The S05 contract `grep -q 'lentil|hanatos' THIRD_PARTY_LICENSES.md` enforces presence but not timeliness.

## DeepCDefocusPO — PlanarIop Scatter Write Correctness

### imagePlane.writableAt requires chans.contains() guard — missing channel = heap corruption
**Context:** DeepCDefocusPO malloc crash fix (Q3)

`ImagePlane::writableAt(x, y, channel)` computes a stride-based offset into a buffer sized for the ChannelSet returned by `imagePlane.channels()`. Writing a channel *not* in that ChannelSet produces an invalid offset and silently corrupts adjacent heap. The crash symptom is `malloc(): invalid size (unsorted)` at the *next* allocation after `renderStripe` returns — not at the write site, making it easy to misdiagnose. Nuke can and does call `renderStripe` requesting only a subset of RGBA. Every `writableAt` in `renderStripe` must be guarded with `chans.contains(chan)` before writing.

### ChannelSet::contains() mock stub was missing from verify-s01-syntax.sh
**Context:** DeepCDefocusPO malloc crash fix (Q3)

`ChannelSet::contains(Channel)` is used throughout the codebase (DeepCWrapper, DeepCKeymix, etc.) but the mock `ChannelSet` in `verify-s01-syntax.sh` was missing the method. Any new use of `ChannelSet` methods in a plugin's `renderStripe` or `doDeepEngine` must be checked against the mock stub list — add missing methods as `bool contains(Channel) const { return true; }` or equivalent no-op stub.
