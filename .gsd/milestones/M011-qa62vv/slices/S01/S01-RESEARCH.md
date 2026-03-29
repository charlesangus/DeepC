# S01 Research: Fork opendefocus-kernel with holdout gather

**Milestone:** M011-qa62vv  
**Slice:** S01 — Fork opendefocus-kernel with holdout gather  
**Gathered:** 2026-03-29

---

## Summary

This slice adds a holdout transmittance texture to the forked opendefocus-kernel and evaluates T(output_pixel, sample_depth) inside the gather loop (`calculate_ring`). The work touches five layers:
1. **opendefocus-kernel fork** — new holdout binding in `global_entrypoint`, T lookup in `calculate_ring`
2. **opendefocus-shared fork** — `ConvolveSettings` size is alignment-critical; a new flag field is the safest extension point
3. **opendefocus (main crate) fork** — `runners/cpu.rs` and `runners/wgpu.rs` pass holdout data through `execute_kernel_pass`
4. **opendefocus-deep** — `lib.rs` constructs holdout CPUImage/buffer from `HoldoutData` and passes it to `render_stripe`
5. **Cargo.toml / workspace** — convert `opendefocus` and `opendefocus-kernel` from registry deps to path deps pointing at local forks

The CPU-only holdout path (no SPIR-V changes) is the confirmed correct first pass per the milestone context. GPU holdout (SPIR-V storage buffer binding) is deferred.

---

## Active Requirements Targeted

- **R048** — re-opened: holdout provides sharp depth-aware mask inside the convolution
- **R067** — kernel-level holdout: gather loop evaluates T at output pixel per sample depth
- **R068** — opendefocus-kernel fork: holdout binding in `global_entrypoint` and `calculate_ring`
- **R069** — HoldoutData flows through full FFI → opendefocus → kernel stack (FFI struct already exists in `include/opendefocus_deep.h`)
- **R070** — integration test: sharp depth-selective holdout (unit test component of S01)

---

## Implementation Landscape

### Existing Fork Infrastructure (already in place)

The worktree at `/home/latuser/git/DeepC/.gsd/worktrees/M011-qa62vv` already has:
- `crates/opendefocus-deep/src/lib.rs` — complete layer-peel loop calling `OpenDefocusRenderer`
- `crates/opendefocus-deep/include/opendefocus_deep.h` — `HoldoutData` FFI struct NOT YET in this file; only `DeepSampleData` and `LensParams` are present
- `src/DeepCOpenDefocus.cpp` — C++ side currently does post-defocus holdout multiply (S03 comment); pre-holds the HoldoutData construction
- `src/CMakeLists.txt` — already builds `libopendefocus_deep.a` via `cargo build --release` and links it
- `Cargo.toml` (workspace) — currently lists `opendefocus = { version = "0.1.10" }` from registry

**Key observation:** `HoldoutData` is referenced in the milestone context and `include/opendefocus_deep.h` header comment but is NOT yet defined in the header. The struct must be added.

### opendefocus 0.1.10 Source Structure (fetched from codeberg.org)

#### `crates/opendefocus-kernel/src/lib.rs`
```
global_entrypoint(
    thread_id, output_image, settings, cached_samples,
    input_image, inpaint, filter, depth,
    bilinear_sampler, nearest_sampler
)
```
Uses `#[cfg(not(any(target_arch = "spirv")))]` / `#[cfg(target_arch = "spirv")]` conditional compilation for each texture/sampler parameter. GPU entry point (`convolve_kernel_f32`) is a `#[spirv(compute(threads(16, 16)))]` function with explicit `#[spirv(descriptor_set = 0, binding = N)]` attributes, currently bindings 0–8.

#### `crates/opendefocus-kernel/src/stages/ring.rs`
`calculate_ring` has the exact gather loop. The insertion point for holdout T attenuation:
```rust
let sample = calculate_sample(...);
rings.background.add_sample(sample);  // ← multiply by T before this
```
And for foreground:
```rust
rings.foreground.add_sample(calculate_sample(...));  // ← multiply by T before this
```
`Sample::new` in `sample.rs` multiplies `color * weight`, so we want `sample.color *= T` and `sample.alpha *= T` etc. before `add_sample`, OR pass T into `calculate_sample`. Simpler: attenuation after `calculate_sample` returns, before `add_sample`.

#### `crates/opendefocus-kernel/src/stages/sample.rs`
`Sample` struct has: `color: Vec4, kernel: Vec4, weight: f32, alpha: f32, deep: f32, alpha_masked: f32, coc: f32`. T attenuation should multiply `color`, `alpha`, `alpha_masked` (not `kernel` or `weight` — those are filter/disc properties). The depth field of the sample (`coc`) is the depth of the **source pixel**, which is what we need to look up T.

#### `crates/opendefocus-shared/src/internal_settings.rs`
`ConvolveSettings` is `#[repr(C, align(16))]` with `bytemuck::Pod`. The struct has `_padding: u32` at the end. **CRITICAL:** A test asserts `size_of::<ConvolveSettings>() == 160`. Adding fields would break this test and the GPU uniform buffer layout. **We must NOT add fields to `ConvolveSettings`** for the CPU-only holdout path.

The holdout data for the CPU path should be passed as an additional parameter to `global_entrypoint` and `calculate_ring` — not through `ConvolveSettings`. This is consistent with how `cached_samples: &[f32]` is passed as a separate slice argument.

#### `crates/opendefocus/src/runners/cpu.rs`
`execute_kernel_pass` signature:
```rust
async fn execute_kernel_pass(
    &self, output_image: &mut [f32], input_image: Array3<f32>,
    inpaint: Array3<f32>, filters: &MipmapBuffer<f32>,
    depth: Array3<f32>, convolve_settings: ConvolveSettings,
    cached_samples: &[f32],
) -> Result<()>
```
Calls `global_entrypoint` per pixel via `rayon::par_chunks_exact_mut`. The holdout `CPUImage` needs to be passed through here and into `global_entrypoint`.

#### `crates/opendefocus/src/runners/wgpu.rs`
`execute_compute_shader` takes the same parameter set plus wgpu internals. Bindings 0–8 are already used. A holdout storage buffer would be binding 9. **For S01 (CPU-only holdout), no wgpu changes are needed.** The `WgpuRunner::execute_kernel_pass` can receive the holdout parameter but use it as no-op (pass an empty/ones slice to the CPU path; the GPU path ignores it).

#### `crates/opendefocus/src/runners/runner.rs`
The `ConvolveRunner` trait defines `execute_kernel_pass` — both CPU and GPU runners implement it. Adding `holdout: &HoldoutCPUImage` or `holdout: &[f32]` to the trait signature requires both implementations to accept it.

---

## What Needs to Be Built

### Fork Structure

Create local fork directories:
- `crates/opendefocus-kernel/` — fork of `opendefocus-kernel 0.1.10`
- `crates/opendefocus/` — fork of `opendefocus 0.1.10`
- `crates/opendefocus-shared/` — fork of `opendefocus-shared 0.1.10` (needed if ConvolveSettings changes; needed anyway because kernel and main crate reference it as a path dep)
- `crates/opendefocus-datastructure/` — likely needed as path dep of kernel/shared

Update workspace `Cargo.toml` to add these crates as workspace members and update `opendefocus-deep/Cargo.toml` to use path deps.

**Alternative simpler approach (preferred):** Instead of forking all crates, extract only what we need:
- Only fork `opendefocus-kernel` and `opendefocus` as path deps
- Keep `opendefocus-shared` and `opendefocus-datastructure` from registry — BUT the forked kernel's `Cargo.toml` uses `opendefocus-shared = { path = "../opendefocus-shared", version = "=0.1.10" }`. This means we MUST fork shared too.
- Total forks: `opendefocus-kernel`, `opendefocus`, `opendefocus-shared`, `opendefocus-datastructure`

**Recommended: download the 4 crate sources via `cargo vendor` or manual extraction.**

### Task Decomposition (natural seams)

**T01: Download and vendor the four opendefocus crates as local forks**
- Run `cargo vendor` in the worktree to pull registry sources into `vendor/`
- Copy the four crates to `crates/opendefocus-kernel/`, `crates/opendefocus/`, `crates/opendefocus-shared/`, `crates/opendefocus-datastructure/`
- Update workspace `Cargo.toml` (add members), `crates/opendefocus-deep/Cargo.toml` (path deps), cross-reference path deps inside the forked crates
- Verify: `cargo check -p opendefocus-deep` compiles with path deps

**T02: Add HoldoutData to FFI and construct per-pixel T array in opendefocus-deep**
- Add `HoldoutData` struct (per-pixel `(f32 depth, f32 T)` sorted breakpoints or simpler: per-pixel cumulative T flat) to `include/opendefocus_deep.h` and `crates/opendefocus-deep/src/lib.rs`
- Add `opendefocus_deep_render_holdout` FFI entry point (or extend existing) that accepts `HoldoutData*`
- In `lib.rs`, convert per-pixel holdout T values into a `CPUImage<LumaA<f32>>` (single channel T, or two-channel depth+T per pixel for depth-lookup)
- Unit test: given a 3-pixel mock holdout image, verify T array is constructed correctly

**T03: Add holdout parameter to kernel `global_entrypoint` and `calculate_ring`; implement T lookup and sample attenuation**
- In `crates/opendefocus-kernel/src/lib.rs`: add `holdout: &CPUImage<Rgba<f32>>` (or custom type) parameter under `#[cfg(not(any(target_arch = "spirv")))]` guards
- In `crates/opendefocus-kernel/src/stages/ring.rs`: thread `holdout` through `calculate_ring`; after `calculate_sample` for both fg and bg, look up T from holdout image at `position` (output pixel coords); multiply `sample.color`, `sample.alpha`, `sample.alpha_masked` by T
- The lookup uses `position` (the output pixel's coordinates) and the depth of the sample (available via `sample.coc` / the CoC that encodes depth) — but actually `position` is already known in `calculate_ring` and depth comparisons use the `depth` image. The simplest encoding for S01: pass holdout T as a flat `[f32]` array indexed by pixel index, pre-evaluated at the output pixel. Per-sample-depth T lookup requires passing breakpoints.

**Holdout data format decision:** For the CPU-only S01 path, the simplest correct implementation is:
- In `opendefocus-deep/lib.rs`, for each output pixel, pre-compute `T_at_depth[pixel_idx]` as a flat `Vec<f32>` — but this loses depth selectivity.
- For depth-correct S01: pass holdout as a `CPUImage<Rgba<f32>>` where each pixel stores `(depth_breakpoint_0, T_0, depth_breakpoint_1, T_1)` or use a storage buffer pattern. Given 4 channels per pixel, the simplest encoding is: channel 0 = T accumulated through z=sample_depth. With sorted breakpoints available in `HoldoutData`, we can bilinearly interpolate T at the sample's depth.

**Recommended S01 holdout encoding:** encode as a 4-channel float image where each pixel's RGBA encodes depth and T pairs: `R = depth0, G = T0, B = depth1, A = T1`. This covers the typical case of up to 2 holdout objects. For ≥3 objects, truncate or use the maximum-coverage pair. This avoids storage buffers and works with the existing `CPUImage` pattern.

**T04: Thread holdout through opendefocus runners (cpu.rs, runner.rs, engine.rs)**
- Extend `ConvolveRunner::execute_kernel_pass` trait to accept `holdout: &[f32]` (flat slice representing the pre-encoded holdout image data)
- In `cpu.rs`: build `CPUImage` from holdout slice and pass to `global_entrypoint`
- In `wgpu.rs`: accept holdout slice, ignore it for GPU path (no-op for S01)
- In `engine.rs` / `runner.rs`: thread holdout through `render_convolve` → `runner.convolve()` → `execute_kernel_pass`

**T05: CPU unit test proving T attenuation**
- Write a `#[test]` in `opendefocus-deep` or as a standalone test binary
- Construct a minimal 3×1 image: pixel 0 = red z=5 (behind holdout), pixel 1 = green z=2 (in front of holdout), pixel 2 = blue z=5 (behind holdout)
- Holdout: all pixels have z=3 holdout with alpha=0.9 → T ≈ 0.1
- Call `opendefocus_deep_render_holdout` with use_gpu=0
- Assert: pixels 0 and 2 in output are attenuated by T≈0.1, pixel 1 is unattenuated

---

## What's Riskiest (Build First)

1. **Getting cargo to compile with path deps for a 4-crate fork** — this is the gating task. If the crate sources can't be extracted from the registry or if edition/toolchain mismatches arise, everything else is blocked.
2. **ConvolveSettings must not grow** — adding holdout data to `ConvolveSettings` would break the SPIR-V uniform buffer layout. The approach of passing holdout as a separate `&[f32]` parallel to `cached_samples` avoids this entirely.
3. **Rayon parallelism + per-pixel holdout lookup** — in `cpu.rs`, `par_chunks_exact_mut` processes pixels in parallel. The holdout slice must be immutable (shared reference) — this is safe as long as we pass `holdout: &[f32]` (read-only). The closure captures `holdout` by reference which is `Send + Sync` for `&[f32]`.

---

## How to Extract the Fork Sources

`cargo vendor` will pull all deps into `vendor/`. Then copy the relevant crate directories:
```bash
# From the worktree root
cargo vendor vendor/ 2>&1 | tail -5
# Then copy the 4 crates:
cp -r vendor/opendefocus-0.1.10       crates/opendefocus
cp -r vendor/opendefocus-kernel-0.1.10  crates/opendefocus-kernel
cp -r vendor/opendefocus-shared-0.1.10  crates/opendefocus-shared
cp -r vendor/opendefocus-datastructure-0.1.10 crates/opendefocus-datastructure
```
Then update `Cargo.toml` paths. The vendor approach works even without network because `cargo vendor` caches to disk.

**Caveat:** `cargo vendor` with Cargo 1.75 (system cargo) and `edition2024` in the workspace may fail. Use `cargo +nightly vendor` if nightly is available. Alternatively: use the `cargo-download` approach, or (best) download the crate `.tar.gz` from static.crates.io directly:
```bash
curl -L "https://static.crates.io/crates/opendefocus-kernel/opendefocus-kernel-0.1.10.crate" -o /tmp/ok.crate
```
The static.crates.io URL does not require a User-Agent header unlike the API endpoint.

**Critical check:** The workspace `Cargo.toml` uses `version = 4` in `Cargo.lock` which requires Cargo ≥ 1.78. The system cargo is 1.75. The Rust crate source itself uses `edition = "2021"` (kernel Cargo.toml uses `edition.workspace = true`). The opendefocus workspace `rust-toolchain.toml` uses stable. This environment may need a newer cargo — check if `cargo` in PATH is a worktree-specific version or if there's a newer one available.

---

## Verification Plan

**T01 verify:** `cargo check -p opendefocus-deep 2>&1 | grep -c "^error"` → 0  
**T03 verify:** In the kernel unit tests (ring.rs `#[cfg(test)]`), add a test that directly calls a modified `calculate_ring` with a mock holdout and asserts the returned `rings.background.color` is attenuated.  
**T05 verify (slice success criterion):** 
```bash
cd crates/opendefocus-deep
cargo test -- holdout 2>&1
```
Expected: test passes, T lookup attenuates samples at depth > holdout depth, passes samples at depth < holdout depth.

**Also verify SPIR-V path is unaffected:** The SPIR-V `convolve_kernel_f32` entry point in `lib.rs` uses `#[cfg(target_arch = "spirv")]` guards. The holdout parameter must only be added under `#[cfg(not(any(target_arch = "spirv")))]` — the GPU entry point stays unchanged. The compiled `.wgsl` shader is pre-built and shipped as `src/shaders/opendefocus-kernel.wgsl`; we do not need to recompile the SPIR-V for S01.

---

## Forward Intelligence (for executor agents)

1. **Do NOT modify `ConvolveSettings`** — it's `Pod + align(16)` with a hard-coded size assertion. Pass holdout as an additional function argument alongside `cached_samples`.

2. **SPIR-V entry point is safe to ignore for S01** — the `convolve_kernel_f32` function and all its `#[spirv(...)]` parameters are under `#[cfg(target_arch = "spirv")]`. Adding a `#[cfg(not(spirv))]` parameter to `global_entrypoint` will NOT affect the SPIR-V compilation path. The GPU runner will pass a no-op empty slice for holdout.

3. **`cargo` version constraint** — system cargo is 1.75 which may not parse `version = 4` lockfiles. Test compilation with `cargo build` and check for version errors early. If needed, the `Cargo.lock` can be regenerated after adding path deps.

4. **The T lookup depth logic** — in `calculate_ring`, the source sample's depth is in scene-space (same units as the `depth` texture). The holdout image stores `(depth_breakpoint, cumulative_T)`. A sample at depth `d_sample` behind holdout (d_sample > d_holdout) should be attenuated. The depth comparison: `T = 1.0` if `d_sample <= d_holdout`, else `T = evaluated_cumulative_T`. The `depth` texture value for the background sample is accessible as `sample_result.background_sample` (the CoC-encoded depth from `get_coc_sample`). However for S01 with the simplified per-pixel T array, T is pre-evaluated at the output pixel location (not per-sample-depth) — which is acceptable as a first pass but is not the full depth-correct solution.

   **For the depth-correct S01 solution:** encode holdout as a `CPUImage` where each pixel contains the sorted breakpoints as 4-channel float (R=d0, G=T0, B=d1, A=T1). In `calculate_ring`, after computing `coordinates` for each gathered sample, load the holdout value at the OUTPUT pixel `position` (not `coordinates`), and compare the sample's depth to the breakpoints to select the correct T. This is the kernel-level holdout the milestone requires.

5. **`position` vs `coordinates` in `calculate_ring`** — `position: UVec2` is the output pixel (the convolution center); `coordinates: Vec2` is the normalized position of the source sample being gathered. The holdout T lookup must use `position` (output pixel) with the **sample's depth** to evaluate T. This is precisely what makes it depth-correct and spatially sharp.

6. **`add_sample` mutation** — `Ring::add_sample` accumulates. To attenuate by T, scale the returned `Sample` before calling `add_sample`:
   ```rust
   let mut sample = calculate_sample(...);
   sample.color *= T;
   sample.alpha *= T;
   sample.alpha_masked *= T;
   // Do NOT scale sample.kernel or sample.weight — those are filter disc properties
   rings.background.add_sample(sample);
   ```

7. **HoldoutData in include/opendefocus_deep.h** — the header currently only has `LensParams` and `DeepSampleData`. We need to add `HoldoutData` (per-pixel sorted depth-T breakpoints) and extend `opendefocus_deep_render` (or add `opendefocus_deep_render_holdout`) to accept it. Extend the existing function rather than adding a new one to keep the C++ side simple.

8. **`cargo vendor` static.crates.io URL** — if the API rate-limit blocks downloads, use:
   ```bash
   curl -L "https://static.crates.io/crates/opendefocus-kernel/opendefocus-kernel-0.1.10.crate" -o /tmp/opendefocus-kernel.crate
   tar xzf /tmp/opendefocus-kernel.crate -C /tmp/
   cp -r /tmp/opendefocus-kernel-0.1.10 crates/opendefocus-kernel
   ```
   (The `.crate` file is a `.tar.gz` archive.)

---

## Skills Discovered

No additional skills installed. The work is pure Rust crate modification with no novel external frameworks.
