# M011-qa62vv: Kernel-Level Holdout for DeepCOpenDefocus

**Gathered:** 2026-03-29
**Status:** Ready for planning

## Project Description

Fork the opendefocus convolution kernel to support holdout transmittance evaluation inside the gather loop. For each output pixel, every gathered input sample's contribution is multiplied by T(output_pixel, sample_depth) before accumulation — where T is the holdout transmittance at the output pixel evaluated at that specific input sample's depth.

## Why This Milestone

The holdout for DeepCOpenDefocus requires three simultaneous properties:
1. **Spatial sharpness** — holdout edges are pixel-sharp, not blurred by the defocus convolution
2. **Depth-correctness** — holdout at z=3 attenuates z=5 samples but not z=2 samples
3. **Full disc coverage** — the entire bokeh disc from a behind-holdout source is affected

Previous attempts proved none of these can be achieved outside the convolution:
- Pre-defocus attenuation (Q5, Q7): blurs the holdout edges because the convolution spreads the already-attenuated energy
- Post-defocus attenuation (Q6): either misses scattered energy (per-source-pixel) or loses depth information (per-output-pixel)
- Two-pass split (Q6 v2): only correct for trivial cases — fails when a bokeh disc from behind the holdout scatters to pixels without holdout
- Layer-peel post-render (Q8 attempt): per-output-pixel holdout skips pixels that received scattered energy but had no source sample (z=0 guard)

The only correct placement is **inside the gather loop** of the convolution kernel.

## User-Visible Outcome

### When this milestone is complete, the user can:

- Connect a holdout Deep input to DeepCOpenDefocus and see sharp holdout edges cutting through defocused bokeh discs
- Verify that a z=2 disc passes through a z=3 holdout uncut, while a z=5 disc is sharply masked

### Entry point / environment

- Entry point: Nuke node `DeepCOpenDefocus` with holdout on input 1
- Environment: local Nuke 16.0 with DeepCOpenDefocus.so
- Live dependencies: wgpu/Vulkan GPU backend, CPU fallback via rayon

## Completion Class

- Contract complete means: forked kernel compiles to SPIR-V and native Rust, holdout texture binding exists, T lookup logic is in the gather loop
- Integration complete means: HoldoutData flows from C++ through full FFI/Rust/kernel stack, nuke -x holdout test produces correct depth-selective sharp-edged output
- Operational complete means: docker-build.sh exits 0 producing a working DeepCOpenDefocus.so

## Final Integrated Acceptance

To call this milestone complete, we must prove:

- The holdout test EXR shows a z=5 red disc sharply cut at holdout z=3 pixels, while a z=2 green disc passes through uncut at the same pixels
- The holdout boundary in the output is pixel-sharp (not blurred) across the full extent of the bokeh disc
- Docker build exits 0 for Nuke 16.0 Linux

## Risks and Unknowns

- **rust-gpu SPIR-V constraints** — the kernel compiles with `#![no_std]` for the GPU path. Adding a holdout texture binding must work within spirv_std's Image/Sampler types. Variable-length per-pixel data (holdout breakpoints) may need to be encoded as a fixed-layout texture or storage buffer.
- **opendefocus fork coordination** — the kernel crate (`opendefocus-kernel`), the main crate (`opendefocus`), and our wrapper (`opendefocus-deep`) all need synchronized changes. The kernel is compiled to SPIR-V by a separate build step using nightly Rust.
- **Holdout data format for GPU** — the sorted (depth, T) breakpoints per pixel need a GPU-friendly representation. Options: (a) encode as a multi-channel texture where channels hold depth/T pairs, (b) use a storage buffer with per-pixel offsets. Storage buffer is more flexible but adds a binding.

## Existing Codebase / Prior Art

- `crates/opendefocus-deep/src/lib.rs` — layer-peel loop that calls render_stripe per layer. Currently has pre-defocus holdout attenuation (from Q7) that blurs the holdout edges.
- `crates/opendefocus-deep/include/opendefocus_deep.h` — HoldoutData FFI struct with per-pixel sorted (depth, cumulative_T) breakpoints. This data format is correct; it just needs to reach the kernel.
- `src/DeepCOpenDefocus.cpp` — C++ side builds HoldoutData from the holdout deep plane. This stays unchanged.
- opendefocus-kernel `global_entrypoint` in `lib.rs` — the convolution entry point. Takes input_image, inpaint, filter, depth as textures/images. Holdout would be a new binding.
- opendefocus-kernel `calculate_ring` in `stages/ring.rs` — the gather loop. Iterates over nearby pixels, computes `calculate_sample` for each. The holdout T multiply goes here, applied to each sample before `rings.background.add_sample()` and `rings.foreground.add_sample()`.
- opendefocus `worker/engine.rs` — `render_convolve` calls `runner.convolve()`. The holdout data needs to be threaded through here.
- opendefocus `runners/cpu.rs` — CPU runner calls `global_entrypoint` per pixel via rayon. Holdout CPUImage needs to be passed.
- opendefocus `runners/wgpu.rs` — GPU runner sets up descriptor set bindings. Holdout storage buffer binding needs to be added.

> See `.gsd/DECISIONS.md` for all architectural and pattern decisions.

## Relevant Requirements

- R048 — (re-opened) Holdout provides sharp depth-aware mask inside the convolution
- R067 — Kernel-level holdout: gather loop evaluates T at output pixel per sample depth
- R068 — opendefocus-kernel fork: holdout binding in global_entrypoint and calculate_ring
- R069 — HoldoutData flows through full FFI → opendefocus → kernel stack
- R070 — Integration test: sharp depth-selective holdout

## Scope

### In Scope

- Fork opendefocus-kernel with holdout texture/buffer binding
- Modify calculate_ring to attenuate gathered samples by T
- Modify opendefocus crate (engine, runners) to pass holdout through
- Wire HoldoutData from lib.rs layer-peel loop through render_stripe to kernel
- Update integration test to verify sharp depth-selective holdout

### Out of Scope / Non-Goals

- Modifying the bokeh shape or non-uniform artifact system
- Changing the layer-peel architecture itself
- Supporting holdout on the GPU path if SPIR-V constraints prove blocking (CPU-only holdout is acceptable as a first pass, with GPU holdout as a follow-up)

## Technical Constraints

- opendefocus-kernel requires nightly Rust for SPIR-V compilation via rust-gpu
- The kernel code must compile for both `target_arch = "spirv"` and native targets
- No dynamic allocation in the SPIR-V path (`#![no_std]`)
- The forked crates must be referenced as path dependencies in Cargo.toml, not as published crates

## Integration Points

- opendefocus-kernel (fork) — new holdout binding
- opendefocus crate (fork) — engine/runner changes to pass holdout
- opendefocus-deep — layer-peel loop passes holdout to render_stripe
- DeepCOpenDefocus.cpp — unchanged (already builds HoldoutData)

## Open Questions

- **GPU holdout data format** — storage buffer with per-pixel offset+count, or encode as a multi-channel texture? Storage buffer is more natural for variable-length data but adds complexity to the wgpu binding setup. Could start with CPU-only holdout and add GPU in a follow-up if the storage buffer setup proves complex.
- **Holdout in foreground vs background rings** — the opendefocus kernel separates foreground and background contributions. Holdout T should be applied to both, but the depth comparison direction may differ for foreground (inverse defocus) vs background (normal defocus).
