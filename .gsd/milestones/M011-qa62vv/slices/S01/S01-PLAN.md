# S01: Fork opendefocus-kernel with holdout gather

**Goal:** Fork the four opendefocus crates as path dependencies, add a holdout transmittance binding to the kernel's gather loop, thread it through the cpu runner, and prove depth-correct T attenuation in a unit test.
**Demo:** After this: Forked kernel compiles to SPIR-V and native Rust with holdout binding. CPU path unit test shows T lookup attenuates gathered samples correctly at output pixel per sample depth.

## Tasks
- [x] **T01: Extracted four opendefocus 0.1.10 crates to crates/ as workspace path deps via [patch.crates-io], installed Rust 1.94.1 and protoc, deleted v4 lockfile, cargo check -p opendefocus-deep passes with zero errors** — Download the four opendefocus 0.1.10 crates from static.crates.io, extract them to the crates/ directory, update workspace and crate Cargo.toml files to use path dependencies, delete the v4 Cargo.lock so cargo 1.75 regenerates a v3-compatible one, and confirm `cargo check -p opendefocus-deep` compiles.

The Cargo.lock in the worktree is version 4 which cargo 1.75 cannot parse. Deleting it and letting cargo regenerate it on the first `cargo check` produces a v3 lockfile, which is fine for this project.

The four forked crates already reference each other via internal path deps (`opendefocus-shared = { path = "../opendefocus-shared" }` etc.). After extracting them to `crates/`, these relative paths will be correct. The only change needed is in `crates/opendefocus-deep/Cargo.toml` (switch from registry dep to `path = "../opendefocus"`) and in the workspace `Cargo.toml` (add four new members).

Steps:
1. Download and extract the four crates:
   ```bash
   for crate in opendefocus-kernel opendefocus opendefocus-shared opendefocus-datastructure; do
     curl -L "https://static.crates.io/crates/$crate/$crate-0.1.10.crate" -o /tmp/$crate.crate
     tar xzf /tmp/$crate.crate -C crates/ && mv crates/$crate-0.1.10 crates/$crate
   done
   ```
2. In workspace `Cargo.toml`, add four members to the `[workspace]` members array: `"crates/opendefocus-kernel"`, `"crates/opendefocus"`, `"crates/opendefocus-shared"`, `"crates/opendefocus-datastructure"`.
3. In `crates/opendefocus-deep/Cargo.toml`, replace `opendefocus = { version = "0.1.10", features = ["std", "wgpu"] }` with `opendefocus = { path = "../opendefocus", features = ["std", "wgpu"] }`.
4. In each extracted crate's `Cargo.toml`, verify the inter-crate path references (e.g. `opendefocus-shared = { path = "../opendefocus-shared", version = "=0.1.10" }`) — they should already be correct if the crate used path deps upstream. If any still reference the registry, patch them to use `path = "../"` equivalents.
5. Delete the v4 `Cargo.lock`: `rm Cargo.lock`.
6. Run `cargo check -p opendefocus-deep 2>&1 | tail -5` — must show no errors.
  - Estimate: 45m
  - Files: Cargo.toml, Cargo.lock, crates/opendefocus-deep/Cargo.toml, crates/opendefocus-kernel/Cargo.toml, crates/opendefocus/Cargo.toml, crates/opendefocus-shared/Cargo.toml, crates/opendefocus-datastructure/Cargo.toml
  - Verify: cd /home/latuser/git/DeepC/.gsd/worktrees/M011-qa62vv && cargo check -p opendefocus-deep 2>&1 | grep -c '^error' | grep -q '^0$' && echo PASS
- [x] **T02: Added holdout transmittance parameter to CPU kernel path and implemented depth-correct T attenuation in calculate_ring gather loop, verified by 19/19 passing tests** — Modify the forked `opendefocus-kernel` to accept a flat holdout slice under the CPU (non-SPIR-V) compilation path and implement depth-correct T attenuation in the `calculate_ring` gather loop.

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
  - Estimate: 1h
  - Files: crates/opendefocus-kernel/src/lib.rs, crates/opendefocus-kernel/src/stages/ring.rs, crates/opendefocus-kernel/src/stages/sample.rs
  - Verify: cd /home/latuser/git/DeepC/.gsd/worktrees/M011-qa62vv && cargo test -p opendefocus-kernel 2>&1 | tail -10 && cargo check -p opendefocus-kernel 2>&1 | grep -c '^error' | grep -q '^0$' && echo PASS
- [x] **T03: Added HoldoutData to C FFI and Rust, extracted render_impl(), added opendefocus_deep_render_holdout, and threaded holdout through the full runner stack from render_stripe() to execute_kernel_pass()** — Extend the `ConvolveRunner` trait and both runner implementations (cpu, wgpu) to accept a holdout slice, add `HoldoutData` to the C FFI boundary, and update the layer-peel loop in `opendefocus-deep/src/lib.rs` to construct and pass holdout data.

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
  - Estimate: 1.5h
  - Files: crates/opendefocus/src/runners/runner.rs, crates/opendefocus/src/runners/cpu.rs, crates/opendefocus/src/runners/wgpu.rs, crates/opendefocus-deep/src/lib.rs, crates/opendefocus-deep/include/opendefocus_deep.h
  - Verify: cd /home/latuser/git/DeepC/.gsd/worktrees/M011-qa62vv && cargo check -p opendefocus-deep 2>&1 | grep -c '^error' | grep -q '^0$' && echo PASS
- [x] **T04: Added CPU unit test proving depth-correct holdout transmittance attenuation, fixed scene-depth vs CoC-space mismatch** — Add a `#[test]` in `crates/opendefocus-deep/src/lib.rs` (in a `#[cfg(test)] mod tests` block) that directly exercises the holdout path through `opendefocus_deep_render_holdout` on a minimal synthetic image.

The test constructs a 3×1 deep image:
- Pixel 0 (left): red sample at depth z=5.0 (behind holdout)
- Pixel 1 (center): green sample at depth z=2.0 (in front of holdout)
- Pixel 2 (right): blue sample at depth z=5.0 (behind holdout)

Holdout: all three pixels have a holdout surface at depth z=3.0 with cumulative T=0.1 for samples behind it. Encode as `[3.0, 0.1, 3.0, 0.1]` per pixel (d0=3.0, T0=0.1, d1=3.0, T1=0.1 — same breakpoint repeated for simplicity).

Expected behavior after defocus (using CPU path, minimal blur, quality=Low):
- Output pixel 0: red channel attenuated toward 0 by ~T=0.1
- Output pixel 1: green channel largely unattenuated (T=1.0, sample is at z=2.0 < z=3.0 holdout)
- Output pixel 2: blue channel attenuated toward 0 by ~T=0.1

Note: opendefocus convolves the image, so exact values depend on CoC size. Use a large focus distance (e.g. focus_distance=100.0) to minimize blur and make the test more deterministic. The key assertion is: output_pixel_1.green > output_pixel_0.red (green passes, red is attenuated), not exact float equality.

Steps:
1. Add `#[cfg(test)] mod tests { use super::*; ... }` at the bottom of `lib.rs`.
2. Write `fn test_holdout_attenuates_background_samples()`. Construct the data:
   - `sample_counts = [1u32, 1, 1]` (one sample per pixel)
   - `rgba = [1,0,0,1, 0,1,0,1, 0,0,1,1]` (R, G, B premultiplied)
   - `depth = [5.0f32, 2.0, 5.0]`
   - `lens = LensParams { focal_length_mm: 50.0, fstop: 8.0, focus_distance: 100.0, sensor_size_mm: 36.0 }`
   - `holdout_data = [3.0f32, 0.1, 3.0, 0.1, 3.0, 0.1, 3.0, 0.1, 3.0, 0.1, 3.0, 0.1]` (4 floats × 3 pixels)
   - `HoldoutData { data: holdout_data.as_ptr(), len: 12 }`
3. Allocate `output_rgba = [0f32; 12]` (3 pixels × 4 channels). Call `opendefocus_deep_render_holdout` with use_gpu=0.
4. Assert: `output_rgba[4] > output_rgba[0]` (pixel 1 green > pixel 0 red — holdout attenuated pixel 0's red).
5. Assert: `output_rgba[4] > output_rgba[8]` (pixel 1 green > pixel 2 blue — holdout attenuated pixel 2's blue).
6. Run: `cargo test -p opendefocus-deep -- holdout 2>&1 | grep -E 'ok|FAILED'`.
  - Estimate: 45m
  - Files: crates/opendefocus-deep/src/lib.rs
  - Verify: cd /home/latuser/git/DeepC/.gsd/worktrees/M011-qa62vv && cargo test -p opendefocus-deep -- holdout 2>&1 | grep -q 'test.*ok' && echo PASS
