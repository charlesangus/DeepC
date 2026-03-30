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

## DeepCOpenDefocus — S01/M009 Patterns

### Root Cargo workspace changes staticlib artifact path
**Context:** opendefocus-deep Rust staticlib build integration (T02, S01, M009)

When a root `Cargo.toml` with `[workspace]` is present, `cargo build --release` run from the repo root produces the artifact at `target/release/libopendefocus_deep.a` (workspace root), NOT `crates/opendefocus-deep/target/release/libopendefocus_deep.a` (crate-local). CMake's `OPENDEFOCUS_DEEP_LIB` variable must point at `${CMAKE_SOURCE_DIR}/target/release/libopendefocus_deep.a`. If you run `cargo build --release` from the crate directory directly, the artifact appears in the crate-local `target/`. This discrepancy will cause CMake to fail to find the library at link time.

### extern "C" over CXX crate is correct for S01 stub; upgrade to CXX in S02 if needed
**Context:** opendefocus-deep FFI boundary (S01, M009, D026)

The milestone architecture (D022) specified the CXX crate, but for S01's no-op stub the CXX crate adds non-trivial CMake complexity (build.rs, generated cxxbridge headers, compiling generated .cc). Plain `extern "C"` with a hand-written C header achieves the identical boundary with zero CMake friction. S02 should evaluate whether type-safe bidirectional CXX calls are genuinely needed for the GPU kernel integration before adding CXX.

### PlanarIop mock headers for verify-s01-syntax.sh require six new headers
**Context:** `scripts/verify-s01-syntax.sh` (T03/T04, S01, M009)

Adding a `PlanarIop`-based plugin to the syntax script requires six new mock headers beyond the existing set: `DDImage/PlanarIop.h` (with `renderStripe`, `getRequirements`, `Descriptions`), `DDImage/CameraOp.h` (stub focalLength/fStop/focusDistance/aperture), `DDImage/DeepOp.h` (standalone class distinct from DeepFilterOp), `DDImage/Row.h` (minimal stub), `DDImage/ImagePlane.h` (bounds, makeWritable, writable, channels, stride, nChans), and `DDImage/Descriptions.h`. The existing `DDImage/Box.h` mock also needs `w()` and `h()` methods if not already present. Add `OPENDEFOCUS_INCLUDE=${SCRIPT_DIR}/../crates/opendefocus-deep/include` as an explicit script variable so the FFI header path is canonical and survives path refactors.

### nukedockerbuild:16.0-linux needs cc symlink and protobuf-compiler for Rust builds
**Context:** Docker cargo build verification (T04, S02, M009)

The Rocky Linux 8 NukeDockerBuild image has `gcc` at `/usr/bin/gcc` but no `/usr/bin/cc` symlink. Rust's linker looks for `cc`, not `gcc`, so `cargo build` fails with "linker `cc` not found". Fix: `ln -sf /usr/bin/gcc /usr/local/bin/cc && export PATH="/usr/local/bin:$PATH"`.

Additionally, the `circle-of-confusion` crate has a protobuf build script that requires `protoc`. The image doesn't include it by default but it's available via `dnf install -y protobuf-compiler`. Both fixes must be applied before `cargo build --release` can succeed inside the container.

### opendefocus 0.1.10 uses proto-generated i32 fields for Quality/ResultMode/FilterMode/DefocusMode
**Context:** opendefocus Settings API (T04, S02, M009)

The `Settings` struct and its nested types are generated from Protocol Buffer `.proto` files. Enum fields like `quality`, `result_mode`, `filter.mode`, and `defocus.defocus_mode` are stored as `i32` in the generated Rust structs, NOT as the enum type directly. Assignments like `settings.render.quality = Quality::Low` will fail with type mismatch errors at compile time. Use `.into()`: `settings.render.quality = Quality::Low.into()`. The same applies to all proto-generated enum fields.

### opendefocus CameraData has no focal_point field — use near_field/far_field + focal_plane
**Context:** opendefocus CameraData struct (T04, S02, M009)

The planner assumed `CameraData` had a `focal_point` field. The actual proto-generated struct has: `focal_length`, `f_stop`, `filmback: Filmback { width, height }`, `near_field`, `far_field`, `world_unit: WorldUnit`, `resolution: Resolution { width, height }`. The focus distance maps to the `focal_plane` field on the CoC Settings: `settings.defocus.circle_of_confusion.focal_plane = focus_distance`. The `camera_data` field itself is under `settings.defocus.circle_of_confusion.camera_data`, not directly on `settings.defocus`.

### DD::Image::Descriptions does not exist in Nuke 16.0 SDK — do not override getRequirements
**Context:** PlanarIop subclass compilation (T04, S02, M009)

The planner's stub included a `void getRequirements(DD::Image::Descriptions& desc)` override. `DD::Image::Descriptions` does not exist in the real Nuke 16.0 DDImage SDK. The mock headers had a stub for it, but the real SDK's `PlanarIop.h` uses a different mechanism. Removing the `getRequirements` override entirely is the correct fix — it was a no-op pass-through anyway and the base class default suffices.

### docker-build.sh must use `command -v` and dnf/apt-get detection for Rocky Linux 8 containers
**Context:** docker-build.sh Linux build loop (S02, M009)

The nukedockerbuild:16.0-linux container is Rocky Linux 8 which lacks the `which` command and uses `dnf`, not `apt-get`. The original docker-build.sh used `which curl || apt-get install -y curl` which fails with "bash: which: command not found" (exit 127). The correct pattern for cross-distro scripts in Docker containers:
```
if command -v dnf >/dev/null 2>&1; then
    dnf install -y curl protobuf-compiler 2>/dev/null || true
elif command -v apt-get >/dev/null 2>&1; then
    apt-get install -y curl protobuf-compiler 2>/dev/null || true
fi &&
if ! command -v cc >/dev/null 2>&1; then
    ln -sf $(command -v gcc) /usr/local/bin/cc
fi
```
`command -v` is the POSIX-correct alternative to `which` and works on all distros. Also note that `which` is not installed in the Rocky Linux 8 container but IS available via the `which` RPM package if needed.

## DDImage CameraOp API — Deprecated Method Names vs Real SDK

## DeepCOpenDefocus — S03/M009 Patterns

### Holdout DeepOp integration: request Chan_DeepFront + Chan_Alpha; post-FFI in renderStripe
**Context:** DeepCOpenDefocus holdout attenuation (T01, S03, M009)

The holdout attenuation must be applied *after* the FFI call returns the defocused flat RGBA buffer. Architecture: (1) `dynamic_cast<DeepOp*>(input(1))` guards the path — no holdout input means no-op. (2) Call `input1->deepEngine(output.bounds(), ..., {Chan_Alpha, Chan_DeepFront})` to get the `DeepPlane`. Requesting `Chan_DeepFront` alongside `Chan_Alpha` is required to properly initialise the deep plane — requesting only `Chan_Alpha` produces a malformed plane on some Deep source types. (3) The free function `computeHoldoutTransmittance(const DeepPixel&) -> float` computes T = ∏(1−clamp(αₛ, 0, 1)) — clamp is necessary because deep samples can have out-of-range alpha. (4) Multiply all four RGBA output channels by T (not just RGB) — coverage (alpha) must be attenuated alongside colour for correct compositing.

### GSD verification gate: plan.verify strings are prose, not shell commands — avoid natural-language descriptions that look like commands
**Context:** S03 T04 auto-fix failure (M009)

The GSD verification gate attempts to execute the plan's `verify` field as a shell command. If that field is written as a prose description (e.g. `docker-build.sh exits 0. All 9 DoD items checked. scripts/verify-s01-syntax.sh passes.`), the gate tries to run it literally and fails with `command not found`. Write verify fields as actual runnable shell commands: `bash docker-build.sh --linux --versions 16.0 && bash scripts/verify-s01-syntax.sh`.

## DDImage CameraOp API — Deprecated Method Names vs Real SDK

### `projection_distance()` does not exist; use `focal_point()`
**Context:** DeepCOpenDefocus camera link (M009 S03 T04)
**Finding:** The Nuke DDImage SDK's `CameraOp` does NOT have a `projection_distance()` method. The correct deprecated accessor for focus distance is `focal_point()` (returns `double`). All legacy camera accessors (`focal_length()`, `fStop()`, `focal_point()`, `film_width()`) return `double`, not `float` — always `static_cast<float>(...)` when assigning to a `float` local to avoid narrowing-conversion compile errors.
**Modern equivalents:** `focal_length()` → `focalLength()`, `focal_point()` → `focusDistance()`, `fstop()` → `fStop()` (from `ndk::MultiProjectionCamera`), `film_width()` → `horizontalAperture()`.
**Rule:** Always test camera accessor names inside the real Docker build container — mock stubs won't catch wrong method names until the Docker build runs.

## DeepCOpenDefocus — M009 Cross-Cutting Lessons

### Never plan opendefocus API calls without a real cargo build to verify against the actual types
**Context:** M009 S02 T04 — six API mismatches discovered at Docker build time

opendefocus Settings types are generated from Protocol Buffer `.proto` files. Without running `cargo build` against the real crate, you cannot know: enum fields are `i32` (require `.into()`), nesting paths like `camera_data` (under `settings.defocus.circle_of_confusion.camera_data`, NOT `settings.defocus`), the difference between `CameraData` fields and `Settings` fields for focus distance (`focal_plane` on CoC Settings vs nonexistent `focal_point` on CameraData). Plan any opendefocus integration with explicit cargo-test tasks early, not at the end of the slice.

### Static lib Rust+CMake+Docker pipeline: three-layer verification gate
**Context:** M009 overall build strategy

The three verification layers for a Rust staticlib linked by CMake in a Docker build:
1. **`bash scripts/verify-s01-syntax.sh`** — runs in <5s, catches C++ syntax errors and FFI header inclusion with mock DDImage headers. Run after every .cpp change.
2. **`cargo build --release` at repo root** — produces the .a; catches all Rust compilation and API errors. Can run locally if Rust is installed (avoid needing Docker for every Rust iteration).
3. **`bash docker-build.sh --linux --versions 16.0`** — authoritative gate: real Nuke SDK, real linker, real container OS. Run after significant API changes, before marking any slice done.

Skipping level 2 (local cargo build) forces level 3 to catch API errors inside Docker, which is slow and creates long feedback loops. Install Rust locally (`rustup`) for any future milestones involving Rust staticlibs.

### Layer-peel GPU defocus: RENDERER singleton should be activated before production use
**Context:** M009 S02/S03 opendefocus renderer lifecycle

`lib.rs` creates a new opendefocus `Renderer` per FFI call (`opendefocus_deep_render`). The `RENDERER` static (using `once_cell::sync::Lazy`) is scaffolded but not activated — activating it requires calling `Renderer::new()` which is async (`block_on`). A new renderer per call works correctly for correctness testing, but creates a new GPU context every frame, which is ~100 ms overhead per render on first frame and may trigger GPU resource exhaustion on long renders. Before production use, activate the singleton: call `block_on(Renderer::new())` inside `once_cell::sync::Lazy::new(|| ...)` and store the result.

## DeepCOpenDefocus Integration Test Patterns — M010-zkt9ww

### verify-s01-syntax.sh for-loop pattern is the canonical way to extend .nk existence checks
**Context:** `scripts/verify-s01-syntax.sh` S02 section (T03, S01, M010)

The S02 section uses a `for`-loop over a whitespace-separated list of `.nk` filenames. To add a new integration test script, append its filename to the list — no new check block required. Do not regress to per-file hardcoded blocks; the loop form is intentionally extensible.

```sh
for nk_file in test_opendefocus.nk test_opendefocus_multidepth.nk ...; do
    [ -f "test/$nk_file" ] || { echo "MISSING: test/$nk_file"; FAILED=1; }
    echo "test/$nk_file exists — OK"
done
```

### use_gpu Bool_knob name is canonical — future .nk scripts must use `use_gpu false` exactly
**Context:** DeepCOpenDefocus CPU-only path (T01, S01, M010)

The `Bool_knob` is registered in C++ as `Bool_knob(f, &_use_gpu, "use_gpu", "Use GPU")`. The knob name string `"use_gpu"` is what Nuke serialises to .nk format. Future .nk scripts that want to force CPU-only execution must write `use_gpu false` — any other capitalisation or spelling will silently use the default (GPU).

### push 0 is the Nuke stack idiom for a null/unconnected input in a multi-input node
**Context:** `test_opendefocus_camera.nk` holdout slot (T02, S01, M010)

When a Nuke node has multiple inputs and only some are connected, `push 0` writes a null entry to the node's input stack. In `test_opendefocus_camera.nk`, the holdout input (slot 1) is null and the camera input (slot 2) is Camera2 — so the stack reads `push 0` (null holdout), then `Camera2` (camera). This is the canonical Nuke .nk convention; do not omit the `push 0` or slot ordering breaks.

### DeepFromImage set_z true / z 1.0 converts a flat image to a single deep sample per pixel
**Context:** `test_opendefocus_bokeh.nk` (T02, S01, M010)

To create a minimal Deep input from a flat Constant node for bokeh disc testing: feed Constant → Crop → `DeepFromImage` with knobs `set_z true` and `z 1.0`. This places a single deep sample at depth 1.0 per pixel. Without `set_z true`, `DeepFromImage` produces deep samples with no depth information, which may result in no CoC being computed by `DeepCOpenDefocus`. The `z 1.0` value is arbitrary for a test — use a value within the focus distance range for visible defocus.

### test/output/.gitignore co-located with test data is cleaner than root .gitignore
**Context:** EXR output gitignore strategy (T02, S01, M010)

Placing `*.exr\n*.tmp` in `test/output/.gitignore` (not the root `.gitignore`) keeps the ignore rule co-located with the directory it governs. This is preferable because: the root `.gitignore` doesn't need to know about test output details; the scope is explicit; removing `test/output/` removes the ignore rule too. Apply the same pattern in future milestones that add new output directories.

### DeepMerge operation plus provides additive compositing for discrete-depth multi-layer tests
**Context:** `test_opendefocus_multidepth.nk` (T02, S01, M010)

For multi-depth layer peel testing, `DeepMerge` with `operation plus` (additive) is the correct operation to combine discrete depth layers that don't overlap. Using the default `over` operation would composite the layers by alpha, collapsing depth ordering information. For testing the layer-peel algorithm specifically, additive compositing preserves each layer's full RGBA at its discrete depth.

## opendefocus Fork Setup — M011-qa62vv Patterns

### [patch.crates-io] is the cleanest way to redirect published crates to local paths
**Context:** T01, S01, M011-qa62vv

When forking a family of published crates to workspace path dependencies, use `[patch.crates-io]` in the workspace root `Cargo.toml` rather than editing each crate's normalized manifest. This preserves the extracted manifests exactly as published and keeps the workspace redirect in one place. The `[patch]` section redirects all consumers of the registry dep to the local path automatically — no per-dep `path = ".."` changes needed in each manifest.

### Rust 1.94.1 required; system Rust 1.75 fails on edition 2024 manifests in transitive deps
**Context:** T01, S01, M011-qa62vv

System Rust 1.75 in this environment cannot parse `edition = "2024"` manifests that appear in transitive dependencies downloaded at `cargo check` time. Installing Rust 1.94.1 via rustup is required before running any cargo command against the opendefocus crate family. Install with: `curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --default-toolchain 1.94.1`. The PATH must include `$HOME/.cargo/bin` on every subsequent cargo invocation.

### PROTOC env var is required for every cargo command touching opendefocus-datastructure
**Context:** T01, S01, M011-qa62vv

`opendefocus-datastructure` uses `prost-build` which requires a `protoc` binary. The system package manager cannot install it without root. Fetch from GitHub releases: `curl -L https://github.com/protocolbuffers/protobuf/releases/download/v29.3/protoc-29.3-linux-x86_64.zip -o /tmp/protoc.zip && unzip -q /tmp/protoc.zip -d /tmp/protoc_dir`. Then prefix every cargo command: `PROTOC=/tmp/protoc_dir/bin/protoc cargo ...`. The binary lives in `/tmp` and must be re-fetched if `/tmp` is cleared. Consider adding it to a persistent location.

### Holdout breakpoints are scene depths; kernel gather loop uses CoC pixels — never compare them directly
**Context:** T04, S01, M011-qa62vv — critical bug discovered and fixed

The initial T02 implementation placed holdout attenuation inside `calculate_ring` (the kernel gather loop) using `sample.coc` as the depth proxy. This is wrong: `sample.coc` is a circle-of-confusion radius in pixels, but holdout breakpoints are encoded as real scene depths (e.g. `d0 = 3.0` units). Comparing them produces no attenuation whatsoever — the bug is silent. The fix: apply holdout transmittance in `render_impl` during the layer-peel loop (before samples are handed to the renderer), where both sample `z` values and holdout breakpoints are in scene-depth space. The kernel's `apply_holdout_attenuation` code in `ring.rs` is now effectively dead when called from `opendefocus-deep` (it always receives an empty holdout slice). Do NOT resurrect it for depth-correct holdout without first resolving the CoC-vs-scene-depth unit mismatch.

### Double-attenuation hazard: pass empty holdout to render_stripe when already applying in render_impl
**Context:** T04, S01, M011-qa62vv

`render_impl` applies holdout transmittance during layer extraction, then calls `render_stripe` with `holdout: &[]` (empty). If you ever change `render_stripe` to accept and use a non-empty holdout, both paths will attenuate the same samples twice, silently halving or squaring the transmittance. The architecture is: exactly one layer applies holdout — currently `render_impl`. Document this invariant in code comments when modifying either site.

## opendefocus Holdout C++ Integration — M011-qa62vv Patterns

### buildHoldoutData() scan order must match the Rust kernel flatten loop scan order
**Context:** T01, S02, M011-qa62vv

`buildHoldoutData()` in `DeepCOpenDefocus.cpp` iterates pixels in y-outer × x-inner scan order to pack per-pixel `[d_front, T, d_back, T]` quads into the `HoldoutData` array. This order **must** match the flatten loop scan order used inside the Rust `render_impl` to index into the holdout slice. If the scan orders diverge (e.g. one iterates x-outer and the other y-outer), the wrong holdout values are silently assigned to wrong pixels — no crash, incorrect attenuation. Always keep both sides in sync when refactoring either loop.

### HoldoutData FFI layout: T is scalar transmittance duplicated at indices 1 and 3
**Context:** T01, S02, M011-qa62vv

`HoldoutData` packs `[d_front, T, d_back, T]` quads per pixel. T at index 1 and T at index 3 are identical (duplicated). The Rust `holdout_transmittance()` function reads index 1 as the scalar transmittance for the whole pixel. The duplication is an implementation detail of the FFI layout — do not assign different T values to indices 1 and 3 unless you change the Rust reader to use them separately.

### Post-defocus scalar multiply is wrong for holdout; pre-render dispatch is required
**Context:** T01, S02, M011-qa62vv — replaced an architecturally incorrect pattern

The pre-M011 `renderStripe()` applied holdout as a post-defocus scalar multiply on the output RGBA. This is wrong: the opendefocus gather loop had already scattered samples across the full bokeh disc without knowledge of holdout geometry. The result: holdout edges are blurred by the bokeh spread. Correct approach: call `buildHoldoutData()` + `opendefocus_deep_render_holdout()` **before** the gather loop runs, so each gathered sample is attenuated by T(output_pixel, sample_depth) at collection time. The Rust kernel then accumulates already-attenuated samples — producing sharp holdout edges even for large CoC discs.

### Nuke stack ordering is LIFO: push holdout first for [merge, holdout] DeepCOpenDefocus inputs
**Context:** T02, S02, M011-qa62vv

In Nuke `.nk` files, node inputs are assigned in LIFO (last-pushed = input 0). To produce `DeepCOpenDefocus` with input 0 = deep merge result and input 1 = holdout, push nodes in this order: (1) holdout node, (2) red/back subject, (3) green/front subject. `DeepMerge {inputs 2}` then consumes the top two (back and front), leaving `[merge_result, holdout]` on the stack for the subsequent `DeepCOpenDefocus {inputs 2}`. Wrong stack ordering silently produces wrong input wire assignments.

## DeepCOpenDefocus Performance — M012-kho2ui/S01 Patterns

### OnceLock singleton: lock once per render_impl, hold for full layer loop
**Context:** opendefocus-deep renderer singleton (T01, S01, M012)

`RENDERER: OnceLock<Arc<Mutex<OpenDefocusRenderer>>>` is initialised on first call via `get_or_init`. The `MutexGuard` is acquired once at the top of `render_impl` and held for the entire layer loop — not per-layer. This is correct because `render_stripe` / `render_stripe_prepped` take `&self` (immutable), so the guard deref chain composes cleanly. Acquiring per-layer would be wasteful and could allow interleaving on a future multi-threaded call path. Do NOT move the lock acquisition inside the layer loop.

### prepare_filter_mipmaps: filter shape is constant across layers, settings differ
**Context:** opendefocus-deep layer-peel hoisting (T02, S01, M012)

Filter/mipmap prep depends on the filter shape (bokeh disc, aperture mask, blur radius) from `Settings`, but NOT on per-layer depth values (`focal_plane`, `camera_data`). Hoisting `prepare_filter_mipmaps` above the layer loop uses the first layer's settings. Per-layer `render_stripe_prepped` still receives a fresh `Settings` built per layer (focal_plane, camera_data differ). This is the key split: filter shape is a rendering constant; depth is a per-layer variable.

### render_convolve_prepped skips Telea inpaint — correct for layer-peel
**Context:** opendefocus RenderEngine (T02, S01, M012)

The standard `render_convolve` path runs Telea inpaint on the input image to fill unknown regions before convolution. In the layer-peel approach, each extracted layer already has a valid alpha channel with no unknown regions — the layer is exactly the set of pixels at that depth. `render_convolve_prepped` replaces the inpaint step with a zero `inpaint_image` and duplicate-channel depth, which matches the existing 2-D flatten code path. Do NOT re-introduce Telea inpaint in the prepped path without revisiting layer validity guarantees.

### quality.into() works directly on i32 for proto-generated enum fields
**Context:** opendefocus Settings quality knob wiring (T03, S01, M012)

`settings.render.quality = quality.into()` where `quality: i32` maps directly to the proto-generated field. No named `Quality` enum constant is needed at the call site. The `use opendefocus::Quality` import can be dropped entirely when this pattern is adopted. (Prior code used `Quality::Low.into()` which required the import — the new pattern is cleaner and parameterisable.)

### Enumeration_knob kQualityEntries must be file-scope static, null-terminated
**Context:** DeepCOpenDefocus quality knob (T03, S01, M012)

`Enumeration_knob(f, &_quality, kQualityEntries, "quality", "Quality")` requires a null-terminated `const char* const[]`. Define `kQualityEntries` as a file-scope static BEFORE the class definition to keep the null-terminator convention visible and avoid accidental double-registration if the knobs() method is refactored. The `grep -c 'Enumeration_knob'` contract requires exactly 1 occurrence (code only, not comments).

### Timing tests: loose 5000 ms warm assertion guards regression, not speedup ratio
**Context:** opendefocus-deep unit test timing (T04, S01, M012)

`test_render_timing` and `test_holdout_timing` assert only `warm_ms < 5000` on a 4×4 CPU image. They do NOT assert a speedup ratio because the singleton + mipmap-hoisting optimisation is not measurable at 4×4. The tests serve two purposes: (1) document that warm calls exist and are fast, (2) guard against catastrophic regression (e.g. re-introducing per-call renderer init that would cause seconds of overhead even at 4×4). Any tighter SLA should come from a Docker timing benchmark at 256×256.

## DeepCOpenDefocus — S02/M012 PlanarIop Cache Patterns

### Full-image cache: render outside mutex, write inside mutex
**Context:** DeepCOpenDefocus PlanarIop render cache (S02, M012)

The Rust FFI render runs **outside** `_cacheMutex` to avoid serializing all of Nuke's concurrent tile threads for the duration of the defocus computation. Only the cache metadata read (hit check) and cache write are locked. Two concurrent cache misses will each perform the full render; the second write is harmless because same inputs always produce identical outputs. Pattern: lock → check hit → unlock → render (slow) → lock → write cache → unlock → copy stripe.

### PlanarIop cache invalidation: _validate() resets _cacheValid, renderStripe() checks key
**Context:** DeepCOpenDefocus PlanarIop render cache (S02, M012)

Two complementary guards prevent stale cache reads:
1. `_validate()` sets `_cacheValid = false` unconditionally — fires on every upstream change, knob edit, or frame change.
2. `renderStripe()` checks a 6-field cache key (fl, fs, fd, ssz, quality, use_gpu) before serving from cache.

The `_validate()` invalidation is conservative: it always marks the cache dirty regardless of whether the change actually affected lens params. This is correct because `_validate()` is cheap. Future code that adds new render-affecting parameters must add those fields to the cache key in `renderStripe()`.

### holdout branch and deepEngine must both use fullBox (info_.box())
**Context:** DeepCOpenDefocus PlanarIop render cache (S02, M012)

Both the normal and holdout branches call `deepIn->deepEngine(fullBox, ...)` / `holdoutIn->deepEngine(fullBox, ...)` where `fullBox = info_.box()`. Using the stripe bounds here would fetch incomplete deep data for a non-local convolution — defocus needs to gather samples from the entire image, not just the current stripe. `info_.box()` is safe because it is always set by `_validate()` before `renderStripe()` is called.

### Stripe copy index arithmetic: fullBox-relative row-major
**Context:** DeepCOpenDefocus stripe copy from cache (S02, M012)

The cache buffer is indexed as `_cachedRGBA[(y - fullBox.y()) * fullW * 4 + (x - fullBox.x()) * 4 + c]`. Both `y` and `x` are absolute pixel coordinates; subtracting `fullBox.y()` and `fullBox.x()` respectively gives the zero-based row/col index into the cache. After the full render, the stripe copy loop iterates over `output.bounds()` (the stripe region), not the full box — this is what makes the PlanarIop approach correct.
