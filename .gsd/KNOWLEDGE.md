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
