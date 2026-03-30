---
estimated_steps: 16
estimated_files: 3
skills_used: []
---

# T02: Hoist filter/mipmap prep and skip per-layer inpaint

Extend the `opendefocus` crate with a `prepare_filter_mipmaps` method and a `render_stripe_prepped` method on `OpenDefocusRenderer`, then restructure the layer loop in `render_impl` to call them.

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

## Inputs

- `crates/opendefocus/src/lib.rs`
- `crates/opendefocus/src/worker/engine.rs`
- `crates/opendefocus/src/renders/resize.rs`
- `crates/opendefocus/src/renders/preprocessor.rs`
- `crates/opendefocus-deep/src/lib.rs`

## Expected Output

- `crates/opendefocus/src/lib.rs`
- `crates/opendefocus/src/worker/engine.rs`
- `crates/opendefocus-deep/src/lib.rs`

## Verification

grep -q 'prepare_filter_mipmaps' crates/opendefocus/src/lib.rs && grep -q 'render_stripe_prepped' crates/opendefocus/src/lib.rs && grep -q 'render_with_prebuilt_mipmaps' crates/opendefocus/src/worker/engine.rs && grep -q 'prepare_filter_mipmaps' crates/opendefocus-deep/src/lib.rs
