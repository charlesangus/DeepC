---
estimated_steps: 25
estimated_files: 5
skills_used: []
---

# T03: Thread holdout through opendefocus runners, add HoldoutData to FFI, wire in lib.rs

Extend the `ConvolveRunner` trait and both runner implementations (cpu, wgpu) to accept a holdout slice, add `HoldoutData` to the C FFI boundary, and update the layer-peel loop in `opendefocus-deep/src/lib.rs` to construct and pass holdout data.

The wgpu runner is a no-op for this slice — accept the `holdout: &[f32]` parameter but do not wire it to any GPU buffer. Full GPU holdout (storage buffer binding 9) is deferred to S02.

Holdout encoding passed to kernel: `width * height * 4` f32 flat array. Each pixel `i` occupies `holdout[i*4..i*4+4]` = `[depth0, T0, depth1, T1]`. Pixels with no holdout encode `[0.0, 1.0, 0.0, 1.0]` (T=1 at all depths). An empty slice (len=0) is also valid and means no holdout.

Steps:
1. Open `crates/opendefocus/src/runners/runner.rs`. Find the `ConvolveRunner` trait (or equivalent). Add `holdout: &[f32]` parameter to `execute_kernel_pass` (after `cached_samples` if it exists in the trait signature, otherwise after the last parameter).
2. Open `crates/opendefocus/src/runners/cpu.rs`. Update `execute_kernel_pass` to match the new signature. In the rayon parallel loop where `global_entrypoint` is called, pass `holdout` (a shared `&[f32]` reference — safe because rayon closures need `&[f32]: Sync` which holds for immutable slices).
3. Open `crates/opendefocus/src/runners/wgpu.rs`. Update `execute_kernel_pass` signature to accept `holdout: &[f32]`; ignore it in the body (add `let _ = holdout;` or just drop the binding).
4. Find any other files in `crates/opendefocus/src/` that call `execute_kernel_pass` or `runner.convolve()` (likely `engine.rs` or `renderer.rs`) and thread `holdout: &[f32]` through those call sites as well. Pass `&[]` (empty slice) at the top-level public `render_stripe` API so the existing opendefocus-deep call site keeps working unchanged.
5. In `crates/opendefocus-deep/include/opendefocus_deep.h`: Add the `HoldoutData` C struct:
   ```c
   typedef struct HoldoutData {
       /** Flat float array: width*height*4 values encoding (d0,T0,d1,T1) per pixel. May be null. */
       const float* data;
       /** Number of floats in data; must be width*height*4 or 0. */
       uint32_t len;
   } HoldoutData;
   ```
   Add a new FFI function declaration:
   ```c
   void opendefocus_deep_render_holdout(const DeepSampleData* data, float* output_rgba,
       uint32_t width, uint32_t height, const LensParams* lens, int use_gpu,
       const HoldoutData* holdout);
   ```
6. In `crates/opendefocus-deep/src/lib.rs`: Add the matching Rust `HoldoutData` repr(C) struct and `opendefocus_deep_render_holdout` extern "C" function. The new function delegates to an internal helper `render_impl` that accepts `holdout: &[f32]`. Extract the body of `opendefocus_deep_render` into `render_impl(... holdout: &[f32])` and have both FFI entry points call it (existing `opendefocus_deep_render` passes `&[]` for holdout, new `opendefocus_deep_render_holdout` passes the actual data). In the layer-peel loop, pass `holdout` to `renderer.render_stripe()` — but since `render_stripe` is the public API, add a `holdout` parameter or use the new runner path. Check the actual signature of `render_stripe` in the extracted source and thread accordingly.
7. Verify: `cargo check -p opendefocus-deep 2>&1 | grep -c '^error'` must be 0.

## Inputs

- ``crates/opendefocus/src/runners/runner.rs``
- ``crates/opendefocus/src/runners/cpu.rs``
- ``crates/opendefocus/src/runners/wgpu.rs``
- ``crates/opendefocus-deep/src/lib.rs``
- ``crates/opendefocus-deep/include/opendefocus_deep.h``

## Expected Output

- ``crates/opendefocus/src/runners/runner.rs``
- ``crates/opendefocus/src/runners/cpu.rs``
- ``crates/opendefocus/src/runners/wgpu.rs``
- ``crates/opendefocus-deep/src/lib.rs``
- ``crates/opendefocus-deep/include/opendefocus_deep.h``

## Verification

cd /home/latuser/git/DeepC/.gsd/worktrees/M011-qa62vv && cargo check -p opendefocus-deep 2>&1 | grep -c '^error' | grep -q '^0$' && echo PASS
