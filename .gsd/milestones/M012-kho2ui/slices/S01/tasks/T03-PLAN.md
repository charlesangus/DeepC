---
estimated_steps: 14
estimated_files: 3
skills_used: []
---

# T03: Add quality FFI parameter and C++ Enumeration_knob

Wire a `quality: i32` parameter through the full stack: FFI header → Rust entry points → Settings → C++ knob.

Steps:
1. In `crates/opendefocus-deep/include/opendefocus_deep.h`, add `int quality` as the last parameter to both `opendefocus_deep_render` and `opendefocus_deep_render_holdout`.
2. In `crates/opendefocus-deep/src/lib.rs`, add `quality: i32` to both `#[no_mangle] pub extern "C"` functions and to `render_impl`. In `render_impl`, replace `settings.render.quality = Quality::Low.into();` with `settings.render.quality = quality.into();`.
3. In `src/DeepCOpenDefocus.cpp`:
   a. Add `static const char* const kQualityEntries[] = {"Low", "Medium", "High", nullptr};` near the top of the class or in the translation unit.
   b. Add `int _quality;` member initialised to `0` in the constructor.
   c. In `knobs(Knob_Callback f)`, add `Enumeration_knob(f, &_quality, kQualityEntries, "quality", "Quality");`.
   d. At both call sites, append `(int)_quality` as the last argument to `opendefocus_deep_render` and `opendefocus_deep_render_holdout`.

Key constraints:
- `quality.into()` maps a raw i32 directly to the proto-generated enum — confirmed pattern per KNOWLEDGE.md and existing use at `Quality::Low.into()`
- `kQualityEntries` must be null-terminated (Nuke convention)
- No existing test needs updating (tests call `render_impl` indirectly through the FFI functions; adding `quality` to those requires updating the test call sites too — update `test_holdout_attenuates_background_samples` and `test_singleton_reuse` to pass `0` for quality)
- `grep -c 'Enumeration_knob' src/DeepCOpenDefocus.cpp` must equal exactly 1 (one knob, no mention in comments)

## Inputs

- `crates/opendefocus-deep/include/opendefocus_deep.h`
- `crates/opendefocus-deep/src/lib.rs`
- `src/DeepCOpenDefocus.cpp`
- `scripts/verify-s01-syntax.sh`

## Expected Output

- `crates/opendefocus-deep/include/opendefocus_deep.h`
- `crates/opendefocus-deep/src/lib.rs`
- `src/DeepCOpenDefocus.cpp`

## Verification

bash scripts/verify-s01-syntax.sh && grep -c 'Enumeration_knob' src/DeepCOpenDefocus.cpp | grep -q '^1$' && grep -c 'quality' crates/opendefocus-deep/include/opendefocus_deep.h | awk '{exit ($1 < 2)}'
