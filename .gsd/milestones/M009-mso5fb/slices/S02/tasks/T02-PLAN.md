---
estimated_steps: 20
estimated_files: 3
skills_used: []
---

# T02: Add opendefocus Cargo dep + implement layer-peel defocus engine in lib.rs

Implement the full opendefocus_deep_render() body in Rust.

1. Update `crates/opendefocus-deep/Cargo.toml`: add `opendefocus = { version = "0.1.10", features = ["std", "wgpu"] }`, `circle-of-confusion = "0.2"`, `futures = "0.3"`, `once_cell = "1"`.
2. Add workspace-level dep versions to root `Cargo.toml` if needed.
3. In `lib.rs` implement `opendefocus_deep_render()`:
   a. Log crate init: eprintln! backend TBD (actual logging from renderer init in step c).
   b. Reconstruct per-pixel sample arrays from flat FFI arrays (sample_counts, rgba, depth) into a Vec<Vec<(f32,f32,f32,f32,f32)>> (pixel → samples of (r,g,b,a,z)).
   c. Init renderer via `once_cell::sync::Lazy<Mutex<OpenDefocusRenderer>>` (block_on the async new()). Log 'GPU backend: Vulkan' or 'CPU fallback: rayon' based on renderer backend field.
   d. Sort each pixel's samples by depth front-to-back.
   e. Determine max_layers = max sample count across all pixels.
   f. Allocate accum buffer (width×height×4 f32, zeroed).
   g. For layer i in 0..max_layers:
      - Build flat RGBA array (width×height×4): for pixels with sample i, use that sample's rgba; others use 0,0,0,0.
      - Build depth map (width×height): per-pixel use sample[i].z if it exists, else 0.0.
      - Compute per-pixel CoC from depth map using circle_of_confusion::compute() with lens params.
      - Construct opendefocus Settings with sane defaults + CoC values.
      - Call renderer.render_stripe() on the flat RGBA + CoC.
      - Front-to-back composite rendered layer into accum buffer: out = over(layer, accum).
   h. Write accum buffer into output_rgba.
4. Add `use` imports and handle all unsafe pointer dereferences safely.
5. Note: `cargo build` will fail locally (rustc 1.75) — this is expected. Verification is in T04.

## Inputs

- `crates/opendefocus-deep/src/lib.rs`
- `crates/opendefocus-deep/Cargo.toml`
- `Cargo.toml`
- `.gsd/milestones/M009-mso5fb/M009-mso5fb-RESEARCH.md`
- `.gsd/DECISIONS.md`

## Expected Output

- `crates/opendefocus-deep/Cargo.toml with opendefocus dep`
- `crates/opendefocus-deep/src/lib.rs with full layer-peel implementation`

## Verification

cargo metadata --no-deps (inspect Cargo.toml is parseable). Rust source file well-formed (rustfmt --check if available). Full cargo build verified in T04.
