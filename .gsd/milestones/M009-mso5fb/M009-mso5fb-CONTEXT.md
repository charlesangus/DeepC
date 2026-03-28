# M009-mso5fb: DeepCOpenDefocus — GPU-Accelerated Deep Defocus

**Gathered:** 2026-03-26
**Status:** Ready for planning

## Project Description

A new Nuke NDK plugin — DeepCOpenDefocus — that takes Deep image input and produces a flat 2D RGBA output with physically-based, GPU-accelerated defocus. Uses a forked opendefocus library (Rust, EUPL-1.2) modified to natively understand deep samples. Includes a holdout Deep input for depth-aware occlusion and all of opendefocus's non-uniform bokeh artifacts (catseye, barndoors, astigmatism, axial aberration, inverse foreground).

## Why This Milestone

No existing Nuke tool provides GPU-accelerated, physically-based defocus with non-uniform bokeh artifacts operating directly on Deep images. Nuke's built-in ZDefocus and pgBokeh work on flat images with a single depth channel — they cannot correctly handle per-sample depth ordering, holdouts, or the rich non-uniform artifacts that opendefocus provides. This milestone bridges the gap between deep compositing and high-quality defocus.

## User-Visible Outcome

### When this milestone is complete, the user can:

- Create a DeepCOpenDefocus node, connect a Deep image, and get a visibly defocused flat RGBA output with physically correct depth ordering
- Connect a holdout Deep input that provides sharp depth-aware occlusion of the defocused result
- Link a Nuke Camera node for automatic lens parameter extraction, or manually set focal length, fstop, focus distance, sensor size
- See non-uniform bokeh artifacts (catseye, barndoors, astigmatism, axial aberration, inverse foreground) driven by the deep sample data
- Benefit from GPU acceleration (Vulkan on Linux, Metal on macOS) for production-viable render times

### Entry point / environment

- Entry point: Nuke node menu → Filter → DeepCOpenDefocus
- Environment: Nuke 16+ with GPU (Vulkan or Metal) available; CPU fallback for headless/non-GPU environments
- Live dependencies involved: opendefocus fork (EUPL-1.2 Rust library), wgpu (GPU abstraction)

## Completion Class

- Contract complete means: syntax check passes, structural grep contracts verify deep sample handling, CXX bridge compiles
- Integration complete means: docker build produces DeepCOpenDefocus.so; `nuke -x test/test_opendefocus.nk` exits 0 with non-black defocused output
- Operational complete means: GPU path functional on Linux with Vulkan; CPU fallback works without GPU

## Final Integrated Acceptance

To call this milestone complete, we must prove:

- Deep input → visibly defocused flat RGBA with correct front-to-back depth ordering (non-black output at test resolution)
- Holdout input attenuates background without being defocused itself
- Camera node link correctly overrides manual knob values
- GPU acceleration produces matching output to CPU path (within floating-point tolerance)
- docker-build.sh extended with Rust toolchain produces the plugin

## Risks and Unknowns

- **Modifying the opendefocus GPU kernel for variable-length deep sample data** — The existing kernel assumes uniform grids (one value per pixel per channel). Deep images have variable sample counts per pixel. GPU kernels typically require fixed-size data structures. This is the highest technical risk.
- **CXX FFI for deep sample data** — Passing variable-length per-pixel sample arrays across the Rust↔C++ boundary is more complex than passing flat image slices. The CXX bridge needs custom data structures for sample counts, depths, and RGBA arrays.
- **Rust toolchain in Docker build** — docker-build.sh currently only handles CMake/GCC. Adding Rust stable + nightly toolchains (for spirv GPU shader compilation) adds build complexity and download dependencies.
- **wgpu GPU availability in CI/Docker** — Docker containers typically lack GPU access. GPU path may only be testable in environments with Vulkan/Metal support. CPU fallback is the CI gate.
- **EUPL-1.2 ↔ GPL-3.0 interaction** — Confirmed compatible via EUPL Article 5, but the fork must maintain EUPL-1.2 and attribution must be correct.

## Existing Codebase / Prior Art

- `crates/opendefocus-nuke/src/opendefocus.cpp` — The existing opendefocus Nuke plugin (PlanarIop, CXX bridge, renderStripe with flat image + depth). This is the pattern to follow for the C++ side.
- `crates/opendefocus-nuke/src/lib.rs` — The Rust side of the CXX bridge. Contains OpenDefocusNukeInstance, knob creation, render dispatch, camera data extraction.
- `crates/opendefocus-nuke/src/render.rs` — The render entry point: `render_convolution` takes flat image array, depth array, filter, regions, and calls `renderer.render_stripe`.
- `crates/opendefocus/` — The core library. `OpenDefocusRenderer` with `render_stripe` is the main entry point. `datamodel::Settings` configures everything.
- `crates/opendefocus-kernel/` — The GPU kernel (no-std Rust compiled to SPIR-V). This is what needs modification for deep sample support.
- `src/DeepCDepthBlur.cpp` — DeepC's existing DeepFilterOp pattern for deep spatial ops. Reference for DDImage deep pixel access patterns.
- `docker-build.sh` — Current build script. Needs Rust toolchain extension.
- `CMakeLists.txt` — Current build config. Needs static lib linking for the opendefocus fork.

> See `.gsd/DECISIONS.md` for all architectural and pattern decisions — it is an append-only register; read it during planning, append to it during execution.

## Relevant Requirements

- R046 — Deep input → flat 2D RGBA output via opendefocus
- R047 — Per-deep-sample CoC from real depth + camera parameters
- R048 — Holdout deep input for depth-aware attenuation
- R049 — opendefocus kernel modified for native deep sample handling
- R050 — GPU acceleration via wgpu
- R051 — All non-uniform bokeh artifacts
- R052 — Camera node link
- R053 — Standard lens Float_knobs
- R054 — CXX FFI bridge
- R055 — Rust static lib + Docker build integration
- R056 — Menu registration as DeepCOpenDefocus
- R057 — opendefocus fork with EUPL-1.2
- R058 — Front-to-back depth-correct compositing

## Scope

### In Scope

- DeepCOpenDefocus Nuke NDK plugin (C++ PlanarIop)
- opendefocus fork with deep sample kernel modifications
- CXX FFI bridge for deep sample data
- Holdout deep input
- Camera node link
- GPU acceleration (Vulkan/Metal via wgpu)
- All non-uniform bokeh artifacts
- Docker build integration with Rust toolchain
- Test .nk script

### Out of Scope / Non-Goals

- Deep output (node outputs flat RGBA only)
- Polynomial optics / .fit file support (that's the separate feature/polynomial-optics branch)
- Upstream PR to opendefocus (fork is maintained independently)
- Windows GPU support in this milestone (Linux Vulkan is the primary target)
- macOS build in Docker (macOS Metal requires native build)

## Technical Constraints

- Nuke 16+ only (`_GLIBCXX_USE_CXX11_ABI=1`, GCC 11.2.1)
- DeepC is GPL-3.0; opendefocus fork is EUPL-1.2 (compatible via Article 5)
- Rust stable 1.92+ and nightly (for spirv shader compilation) both required
- CXX crate for FFI (not cbindgen or manual extern "C")
- wgpu for GPU abstraction (Vulkan on Linux, Metal on macOS)
- Static linking of Rust lib into the final .so plugin

## Integration Points

- opendefocus fork — Rust library dependency, built via cargo, linked as static lib
- wgpu — GPU backend, selected automatically by wgpu based on platform
- DDImage — Nuke NDK API for deep pixel access (DeepOp, DeepPlane, DeepPixel) and flat output (PlanarIop, ImagePlane)
- CXX — Rust↔C++ FFI bridge
- docker-build.sh — Extended with Rust toolchain installation and cargo build step

## Open Questions

- **GPU kernel data layout for deep samples** — How to represent variable-length per-pixel sample arrays on GPU. Options: pre-flatten into a 1D buffer with offset/count arrays, or use compute shaders with dynamic indexing. Needs research in S01/S02.
- **Render stripe coordination** — PlanarIop calls renderStripe per stripe. How to coordinate deep pixel fetching (which is per-tile) with opendefocus's render_stripe (which expects flat arrays). May need to flatten deep data into arrays per stripe.
- **Camera node input index** — The existing opendefocus plugin uses input index 2 for the camera. DeepCOpenDefocus needs input 0 = Deep main, input 1 = Deep holdout, input 2 = Camera. Verify DDImage supports 3 inputs with mixed types (2 DeepOp + 1 CameraOp).
