# S01: Singleton renderer + layer-peel dedup

**Goal:** Eliminate the two primary CPU render bottlenecks in DeepCOpenDefocus: (A) per-FFI-call renderer init, replaced by a static singleton; (B) per-layer filter/mipmap prep and Telea inpaint, hoisted out of the layer loop. Also adds a `quality: i32` FFI parameter wired to an Enumeration_knob in C++.
**Demo:** After this: Same 256×256 test renders significantly faster — renderer created once, filter/mipmap/inpaint prep hoisted out of layer loop. cargo test passes. Timed comparison before/after.

## Tasks
- [x] **T01: Replace per-call OpenDefocusRenderer init with static OnceLock singleton in opendefocus-deep, eliminating async init overhead on every FFI call after the first** — In `crates/opendefocus-deep/src/lib.rs`, replace the local `let renderer = if use_gpu != 0 { … }` block in `render_impl` with a `std::sync::OnceLock<Arc<Mutex<OpenDefocusRenderer>>>` static. The lock is initialised on first call using the caller's `use_gpu` flag; subsequent calls lock the Mutex, call `renderer.render_stripe(…)`, then release. Add a `test_singleton_reuse` unit test that calls `opendefocus_deep_render` (CPU path) twice on a trivially small image and asserts both calls succeed without panicking.
  - Estimate: 45m
  - Files: crates/opendefocus-deep/src/lib.rs
  - Verify: grep -q 'OnceLock' crates/opendefocus-deep/src/lib.rs && grep -q 'test_singleton_reuse' crates/opendefocus-deep/src/lib.rs
- [x] **T02: Added prepare_filter_mipmaps + render_stripe_prepped to opendefocus and hoisted filter/mipmap prep outside the layer loop in opendefocus-deep, eliminating per-layer filter construction and Telea inpaint** — Extend the `opendefocus` crate with a `prepare_filter_mipmaps` method and a `render_stripe_prepped` method on `OpenDefocusRenderer`, then restructure the layer loop in `render_impl` to call them.

Steps:
1. In `crates/opendefocus/src/renders/resize.rs`, `MipmapBuffer` is already `pub struct`. No visibility change needed there.
2. In `crates/opendefocus/src/lib.rs`, add `pub use renders::resize::MipmapBuffer;` so downstream crates can name the type.
3. In `crates/opendefocus/src/worker/engine.rs`, add a new `pub(crate) async fn render_with_prebuilt_mipmaps` method to `RenderEngine` that:
   - Accepts `runner`, `image`, `depth`, `filter_mipmaps: &MipmapBuffer<f32>`, `holdout`
   - Calls `prepare_depth_map` (still needed per-layer)
   - For the Depth defocus mode, constructs `inpaint_image = Array3::zeros(original_image.dim())` and `depth_array` with duplicate-depth channels `[(d, d), …]` — skipping Telea inpaint
   - Iterates chunks and calls `render_convolve` exactly as `render` does, but with the prebuilt mipmaps
4. In `crates/opendefocus/src/lib.rs`, add `pub async fn prepare_filter_mipmaps(&self, settings: &Settings, channels: usize) -> Result<MipmapBuffer<f32>>` that replicates the filter-prep + mipmap chain + f32 normalisation from `engine.rs::render()`.
5. In `crates/opendefocus/src/lib.rs`, add `pub async fn render_stripe_prepped<T: TraitBounds>(&self, render_specs, settings, image: ArrayViewMut3<T>, depth: Array2<T>, filter_mipmaps: &MipmapBuffer<f32>, holdout: &[f32]) -> Result<()>` that calls `self.validate(…)` then `RenderEngine::new(settings, render_specs).render_with_prebuilt_mipmaps(…)`.
6. In `crates/opendefocus-deep/src/lib.rs`, hoist the `prepare_filter_mipmaps` call above the layer loop using the first layer's settings (filter shape is constant across layers), and replace `renderer.render_stripe(…)` calls inside the loop with `renderer.render_stripe_prepped(…, &filter_mipmaps, &[])`. Import `MipmapBuffer` from `opendefocus::MipmapBuffer`.

Key constraints:
- `opendefocus::TraitBounds` is already used by `render_stripe`; `render_stripe_prepped` must have the same `T: TraitBounds` bound
- Depth settings (focal_plane, camera_data) still differ per layer — only filter/mipmap prep is hoisted; `settings` is still built fresh per layer for `render_stripe_prepped`
- `test_holdout_attenuates_background_samples` must still pass — it calls `render_impl` which now uses the prepped path
  - Estimate: 2h
  - Files: crates/opendefocus/src/lib.rs, crates/opendefocus/src/worker/engine.rs, crates/opendefocus-deep/src/lib.rs
  - Verify: grep -q 'prepare_filter_mipmaps' crates/opendefocus/src/lib.rs && grep -q 'render_stripe_prepped' crates/opendefocus/src/lib.rs && grep -q 'render_with_prebuilt_mipmaps' crates/opendefocus/src/worker/engine.rs && grep -q 'prepare_filter_mipmaps' crates/opendefocus-deep/src/lib.rs
- [x] **T03: Wired quality: i32 end-to-end from C++ Enumeration_knob through both FFI entry points and render_impl to Settings.render.quality, replacing hardcoded Quality::Low** — Wire a `quality: i32` parameter through the full stack: FFI header → Rust entry points → Settings → C++ knob.

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
  - Estimate: 45m
  - Files: crates/opendefocus-deep/include/opendefocus_deep.h, crates/opendefocus-deep/src/lib.rs, src/DeepCOpenDefocus.cpp
  - Verify: bash scripts/verify-s01-syntax.sh && grep -c 'Enumeration_knob' src/DeepCOpenDefocus.cpp | grep -q '^1$' && grep -c 'quality' crates/opendefocus-deep/include/opendefocus_deep.h | awk '{exit ($1 < 2)}'
- [x] **T04: Added test_render_timing and test_holdout_timing unit tests to opendefocus-deep/src/lib.rs, printing cold/warm elapsed times and asserting warm < 5000 ms** — Add a `test_render_timing` unit test in `crates/opendefocus-deep/src/lib.rs` that:
1. Calls `opendefocus_deep_render` on a minimal 4×4 single-sample image, recording elapsed time with `std::time::Instant` for the first call (cold, singleton init) and a second call (warm, reuses singleton).
2. Prints both elapsed times with `eprintln!` (visible with `--nocapture`).
3. Asserts that the warm call is not pathologically slow (assert `warm_ms < 5000` — this is not a tight SLA, just guards against regression).
4. Add a `test_holdout_timing` variant that calls `opendefocus_deep_render_holdout` twice on a 4×4 image with a minimal holdout slice, similarly printing warm/cold times.

These tests run with `cargo test -p opendefocus-deep -- --nocapture` in Docker. They don't assert a specific speedup ratio (difficult to measure in a unit test environment) but produce timing output that documents the before/after intent.

Also verify the full test suite (`test_holdout_attenuates_background_samples`, `test_singleton_reuse`, `test_render_timing`, `test_holdout_timing`) are syntactically correct by running `bash scripts/verify-s01-syntax.sh` (which only checks C++ syntax, but the Rust code must at least be structurally consistent with prior tasks).
  - Estimate: 30m
  - Files: crates/opendefocus-deep/src/lib.rs
  - Verify: grep -q 'test_render_timing' crates/opendefocus-deep/src/lib.rs && grep -q 'test_holdout_timing' crates/opendefocus-deep/src/lib.rs && bash scripts/verify-s01-syntax.sh
