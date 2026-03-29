---
estimated_steps: 16
estimated_files: 3
skills_used: []
---

# T02: Add holdout parameter to kernel global_entrypoint and implement T-attenuation in calculate_ring

Modify the forked `opendefocus-kernel` to accept a flat holdout slice under the CPU (non-SPIR-V) compilation path and implement depth-correct T attenuation in the `calculate_ring` gather loop.

The holdout slice encodes the output image's holdout data as `width * height * 4` f32 values where each group of 4 floats is `(depth0, T0, depth1, T1)` — two depth/T breakpoints per output pixel. T is cumulative transmittance through the holdout at that depth. If the gathered sample's depth exceeds `depth0`, apply `T0` as attenuation; if it also exceeds `depth1`, apply `T1`. Default T = 1.0 (no attenuation) when the holdout slice is empty or the pixel is out of bounds.

Do NOT modify `ConvolveSettings` — it has a hard-coded size assertion (`size_of::<ConvolveSettings>() == 160`) and is a GPU uniform buffer. Pass holdout as an additional function parameter alongside `cached_samples`.

The SPIR-V entry point (`convolve_kernel_f32` with `#[spirv(compute)]`) must not be modified — only add parameters under `#[cfg(not(any(target_arch = "spirv")))]` guards.

Steps:
1. Open `crates/opendefocus-kernel/src/lib.rs`. Find `global_entrypoint` (the CPU-only fn). Add parameter `holdout: &[f32]` after `cached_samples`.
2. Thread `holdout` into every call to functions that ultimately call `calculate_ring`. Find `calculate_ring` call sites in `lib.rs` (likely inside `execute_kernel` or similar) and add the `holdout` parameter.
3. Open `crates/opendefocus-kernel/src/stages/ring.rs`. Add `holdout: &[f32]` and `output_width: u32` parameters to `calculate_ring` (or whichever function calls into the per-sample gather). The `position: UVec2` (output pixel) is already available here.
4. Inside the gather loop, after each `calculate_sample(...)` call for both foreground and background samples:
   a. Compute `pixel_idx = position.y * output_width + position.x`.
   b. If `holdout.len() >= (pixel_idx as usize + 1) * 4`, read `(d0, t0, d1, t1)` from `holdout[pixel_idx*4..pixel_idx*4+4]`.
   c. Determine sample depth from `sample.coc` (or the source depth — check the `Sample` struct fields: `coc` encodes CoC magnitude, `deep` may carry raw depth). Use whichever field represents scene depth.
   d. Compute T: `if sample_depth > d1 { t1 } else if sample_depth > d0 { t0 } else { 1.0 }` (assumes d0 < d1, both are near-plane holdout depths).
   e. Attenuate: `sample.color *= T; sample.alpha *= T; sample.alpha_masked *= T;` — do NOT modify `sample.kernel` or `sample.weight`.
5. Run `cargo check -p opendefocus-kernel` (it should compile; the kernel crate is now a workspace member).
6. Check if `crates/opendefocus-kernel` has existing tests under `#[cfg(test)]`. If yes, run them: `cargo test -p opendefocus-kernel` and ensure they still pass (especially the `ConvolveSettings` size test).

## Inputs

- ``crates/opendefocus-kernel/src/lib.rs``
- ``crates/opendefocus-kernel/src/stages/ring.rs``
- ``crates/opendefocus-kernel/src/stages/sample.rs``

## Expected Output

- ``crates/opendefocus-kernel/src/lib.rs``
- ``crates/opendefocus-kernel/src/stages/ring.rs``

## Verification

cd /home/latuser/git/DeepC/.gsd/worktrees/M011-qa62vv && cargo test -p opendefocus-kernel 2>&1 | tail -10 && cargo check -p opendefocus-kernel 2>&1 | grep -c '^error' | grep -q '^0$' && echo PASS
