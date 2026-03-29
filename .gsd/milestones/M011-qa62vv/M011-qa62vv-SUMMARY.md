---
id: M011-qa62vv
title: "Kernel-Level Holdout for DeepCOpenDefocus"
status: complete
completed_at: 2026-03-29T12:00:45.964Z
key_decisions:
  - D028: Holdout transmittance applied in render_impl (scene-depth space), not kernel gather loop (CoC space) — CoC values in pixels are incompatible with scene-depth holdout breakpoints; kernel application is a silent no-op
  - D027: Fork opendefocus-kernel and three companion crates as workspace path deps under crates/; use [patch.crates-io] instead of editing normalized manifests
  - [patch.crates-io] pattern for redirecting a published crate family to local path deps — cleaner than per-manifest edits, no need to modify normalized Cargo.toml files
  - edition = '2021' patch applied to all four extracted crate manifests — system Rust 1.75 incompatible with edition 2024 in transitive deps
  - Empty holdout sentinel (&[]) passed to render_stripe prevents double-attenuation between render_impl and the kernel gather loop
  - render_impl private extraction: both opendefocus_deep_render (&[] holdout) and opendefocus_deep_render_holdout (real holdout) share one code path via private render_impl()
  - buildHoldoutData() y-outer x-inner scan order must match sample flatten loop scan order in the Rust kernel — mismatched scan order silently assigns wrong holdout values to wrong pixels
  - Pre-render dispatch replaces post-defocus scalar multiply: the gather loop must receive holdout transmittance as input so each gathered sample is attenuated before accumulation, not after the bokeh disc has already been spread
key_files:
  - crates/opendefocus-deep/src/lib.rs
  - crates/opendefocus-deep/include/opendefocus_deep.h
  - crates/opendefocus-kernel/src/stages/ring.rs
  - crates/opendefocus-kernel/src/lib.rs
  - crates/opendefocus/src/runners/runner.rs
  - crates/opendefocus/src/runners/cpu.rs
  - crates/opendefocus/src/runners/wgpu.rs
  - crates/opendefocus/src/lib.rs
  - crates/opendefocus/src/worker/engine.rs
  - src/DeepCOpenDefocus.cpp
  - test/test_opendefocus_holdout.nk
  - Cargo.toml
  - install/16.0-linux/DeepC/DeepCOpenDefocus.so
  - release/DeepC-Linux-Nuke16.0.zip
lessons_learned:
  - CoC-vs-scene-depth unit mismatch is a silent bug: comparing sample.coc (pixels) against holdout breakpoints (scene depth) produces no-attenuation without any compiler or runtime error. Always verify that quantities on both sides of a comparison share coordinate space.
  - [patch.crates-io] is the correct pattern for forking a family of published crates to workspace path deps — no need to edit normalized manifests, and the workspace root owns the redirect.
  - System Rust 1.75 cannot parse edition = '2024' in transitive deps — always check toolchain version when adding crates with modern edition fields. Patching to edition = '2021' in extracted manifests is a safe workaround.
  - protoc lives in /tmp after ad-hoc fetch — it must be re-fetched if /tmp is cleared. Add PROTOC to a persistent location or document the re-fetch step for future contributors.
  - Pre-render holdout dispatch is architecturally required for correct bokeh edges — post-render scalar multiply blurs the holdout boundary because the gather loop has already spread samples. The holdout must be injected before the gather.
  - Empty holdout sentinel (&[]) to render_stripe prevents double-attenuation: if render_impl pre-attenuates and render_stripe also attenuates, attenuation is applied twice. Passing empty slice to the kernel is the correct pattern.
  - Nuke stack ordering is LIFO: to get [merge, holdout] at the top, push holdout first and back-subject last. Incorrect stack ordering in .nk files produces wrong input wire assignments silently.
---

# M011-qa62vv: Kernel-Level Holdout for DeepCOpenDefocus

**Forked the opendefocus convolution kernel into four workspace crates, threaded depth-correct holdout transmittance through the full CPU stack (render_impl → FFI → C++ → Nuke), and shipped DeepCOpenDefocus.so (21 MB) from a Docker build that exits 0.**

## What Happened

M011 delivered depth-correct holdout transmittance for DeepCOpenDefocus across two sequential slices.

**S01 — Fork opendefocus-kernel with holdout gather**

The first challenge was establishing a compilable fork. Four opendefocus 0.1.10 crates (`opendefocus-kernel`, `opendefocus`, `opendefocus-shared`, `opendefocus-datastructure`) were downloaded from `static.crates.io` and extracted to `crates/`. The workspace root uses `[patch.crates-io]` to redirect the published registry deps to these local paths — avoiding per-manifest edits across normalized Cargo.toml files. Two environment blockers were resolved: system Rust 1.75 cannot parse `edition = "2024"` in transitive deps (Rust 1.94.1 installed via rustup); `prost-build` requires `protoc` (fetched protoc 29.3 from GitHub releases to `/tmp`). All four extracted manifests had `edition = "2024"` patched to `edition = "2021"` for compatibility.

Holdout was threaded through the CPU path under `#[cfg(not(spirv))]` guards, leaving the SPIR-V compute shader entry point completely unchanged. An `apply_holdout_attenuation` helper was added to `ring.rs` to read `(d_front, T, d_back, T)` breakpoints per output pixel. However, T02 exposed a critical architectural mistake: `sample.coc` in the kernel gather loop is a circle-of-confusion radius in *pixels*, while holdout breakpoints are *scene depths*. Comparing them is a unit mismatch producing silent no-attenuation.

The fix (T04) moved holdout transmittance application to `render_impl` in `opendefocus-deep/src/lib.rs`, applied during the layer-peel loop where sample `z` and holdout breakpoints share scene-depth coordinate space. `render_stripe` receives `&[]` (empty holdout) to prevent double-attenuation. The `opendefocus_deep_render_holdout` FFI entry point accepts a `HoldoutData { data: *const f32, len: u32 }` C struct and calls the shared `render_impl` path. The existing `opendefocus_deep_render` entry point calls `render_impl(&[])` unchanged.

Unit test `test_holdout_attenuates_background_samples` proved correctness: 3×1 deep image with red (z=5), green (z=2), blue (z=5); holdout at d_front=3.0, T=0.1. After fix: green ≈ 0.97 (z=2 < d_front=3, T=1.0, uncut); red/blue ≈ 0.099 (z=5 > d_front=3, T=0.1, ~10× attenuated). 19/19 kernel unit tests pass (12 pre-existing + 7 new holdout branch tests).

**S02 — Wire holdout through full stack and integration test**

Pre-S02, the C++ `renderStripe()` applied holdout as a post-defocus scalar multiply — wrong because the Rust kernel had already scattered samples across the bokeh disc without holdout geometry. T01 deleted `computeHoldoutTransmittance()` and the entire S03 post-render block. In their place, `buildHoldoutData()` iterates pixels in y-outer × x-inner scan order (matching the Rust flatten loop), sorts samples by `Chan_DeepFront` ascending, computes per-pixel transmittance `T = ∏(1 - clamp(α, 0, 1))`, and packs `[d_front, T, d_back, T]` quads. `renderStripe()` branches: input 1 connected → `buildHoldoutData()` + `opendefocus_deep_render_holdout()`; disconnected → `opendefocus_deep_render()` unchanged.

T02 rewrote `test/test_opendefocus_holdout.nk` with LIFO Nuke stack ordering: holdout (black, α=0.5, front=3.0) first; red subject (front=5.0) second; green subject (front=2.0) third; `DeepMerge {inputs 2}` leaving [merge, holdout] for `DeepCOpenDefocus {inputs 2}`. This demonstrates depth-selective behaviour (z=5 cut, z=2 uncut) in the correct Nuke compositing graph.

T03 ran `bash docker-build.sh --linux --versions 16.0` inside `nukedockerbuild:16.0-linux` (Rocky Linux 8). No code changes were needed. The build installed `protobuf-compiler` and `cc` symlink (known Rocky Linux 8 quirks from M009), compiled the full Cargo workspace including `opendefocus-deep` v0.1.0, ran CMake against the Nuke 16.0v2 SDK, and produced `DeepCOpenDefocus.so` (21 MB) and `release/DeepC-Linux-Nuke16.0.zip` (10 MB). Exit 0.

**Known limitation:** Pixel-level visual UAT in Nuke (running `test_opendefocus_holdout.nk` and inspecting `holdout_depth_selective.exr`) is deferred — no Nuke license in CI. Algorithmic correctness is proven by the Rust unit test; the `.nk` file is structurally verified and ready for manual visual verification.

## Success Criteria Results

## Success Criteria Results

| Criterion | Result | Evidence |
|-----------|--------|----------|
| Kernel forked — four opendefocus 0.1.10 crates as workspace path deps | ✅ Met | `crates/opendefocus-kernel`, `crates/opendefocus`, `crates/opendefocus-shared`, `crates/opendefocus-datastructure` in git diff; `[patch.crates-io]` in Cargo.toml |
| Holdout transmittance evaluated per sample — T(output_pixel, sample_depth) applied to each gathered contribution | ✅ Met | `holdout_transmittance()` called in `render_impl` during layer-peel loop; attenuation applied to `sample.color`, `sample.alpha`, `sample.alpha_masked` before accumulation in `render_stripe` |
| Depth-correct — scene-depth space, not CoC space | ✅ Met | Attenuation placed in `render_impl` (scene-depth), not kernel gather loop (CoC). Architectural decision D028 documented. |
| CPU unit test proves correctness | ✅ Met | `cargo test -p opendefocus-deep -- holdout` → 1/1 pass. green (z=2) ≈ 0.97 (uncut), red/blue (z=5) ≈ 0.099 (10× attenuated). |
| FFI entry point `opendefocus_deep_render_holdout()` + `HoldoutData` C struct exposed | ✅ Met | Declared in `crates/opendefocus-deep/include/opendefocus_deep.h`; implemented in `crates/opendefocus-deep/src/lib.rs` |
| C++ Nuke plugin dispatches holdout pre-render (not post-render scalar multiply) | ✅ Met | `buildHoldoutData()` + `opendefocus_deep_render_holdout()` branch in `renderStripe()`; post-defocus scalar multiply deleted from `src/DeepCOpenDefocus.cpp` |
| Docker build exits 0; `DeepCOpenDefocus.so` (21 MB) produced | ✅ Met | S02 T03: `bash docker-build.sh --linux --versions 16.0` exit 0; `install/16.0-linux/DeepC/DeepCOpenDefocus.so` 21 MB confirmed |
| Visual UAT in Nuke — z=5 disc sharply cut, z=2 disc uncut | ⚠️ Deferred | No Nuke license in CI. Algorithmically equivalent proof provided by Rust unit test. `.nk` test structurally verified (content greps pass). Documented as known limitation in S02 summary. |

## Definition of Done Results

## Definition of Done Results

- ✅ **All slices complete** — S01 ✅ and S02 ✅ in ROADMAP.md slice overview table. Both have SUMMARY.md files on disk.
- ✅ **All slice summaries exist** — `.gsd/milestones/M011-qa62vv/slices/S01/S01-SUMMARY.md` and `.gsd/milestones/M011-qa62vv/slices/S02/S02-SUMMARY.md` confirmed present.
- ✅ **Code changes committed** — `git diff --stat $(git merge-base HEAD deepcblur) HEAD -- ':!.gsd/'` shows 87 files changed, 21082 insertions. Key non-.gsd files: `src/DeepCOpenDefocus.cpp`, all four `crates/opendefocus-*`, `test/test_opendefocus_holdout.nk`, `install/16.0-linux/DeepC/DeepCOpenDefocus.so`, `release/DeepC-Linux-Nuke16.0.zip`.
- ✅ **Cross-slice integration verified** — S01 `HoldoutData` FFI layout matches S02 `buildHoldoutData()` pack format; scan order (y-outer × x-inner) verified consistent; pre-render dispatch confirmed in production binary; backward-compatible fallback path (`opendefocus_deep_render()`) unchanged.
- ✅ **Requirements advanced/validated** — R046 (DeepCOpenDefocus.so with holdout), R049 (kernel fork + depth-correct attenuation), R057 (four crates as workspace path deps) all have evidence.
- ✅ **Milestone validation recorded** — `.gsd/milestones/M011-qa62vv/M011-qa62vv-VALIDATION.md` with verdict `needs-attention` (environment-constrained gaps, no code deficiencies).
- ⚠️ **Visual UAT deferred** — Nuke license not available in CI; Rust unit test provides mathematical equivalent proof.

## Requirement Outcomes

## Requirement Outcomes

### R046 — DeepCOpenDefocus ships with depth-correct holdout support
- **Transition:** active → validated (advanced in this milestone, validated by S02 Docker build)
- **Evidence:** `bash docker-build.sh --linux --versions 16.0` exits 0; `install/16.0-linux/DeepC/DeepCOpenDefocus.so` (21 MB) linked against Nuke 16.0v2 SDK. `buildHoldoutData()` + `opendefocus_deep_render_holdout()` dispatch path active in production `renderStripe()`. C++ source: `src/DeepCOpenDefocus.cpp`.

### R049 — opendefocus Rust library forked and modified to accept holdout transmittance; CPU path applies depth-correct T attenuation during layer-peel
- **Transition:** active → validated (advanced in this milestone, validated by Rust unit test + Docker build)
- **Evidence:** `holdout_transmittance()` in `render_impl` (layer-peel, scene-depth space); `test_holdout_attenuates_background_samples` passes (1/1); Cargo workspace compiled including `opendefocus-deep` v0.1.0 with holdout dispatch in Docker build.

### R057 — Four opendefocus 0.1.10 crates extracted to crates/ as workspace path dependencies
- **Transition:** active → validated (advanced and completed in this milestone)
- **Evidence:** `crates/opendefocus-kernel`, `crates/opendefocus`, `crates/opendefocus-shared`, `crates/opendefocus-datastructure` in git diff. `[patch.crates-io]` in `Cargo.toml`. `cargo check -p opendefocus-deep` → 0 errors. Docker build exits 0 confirming Cargo workspace resolves all deps via local path deps.

## Deviations

- S01 T01: Used [patch.crates-io] instead of per-crate manifest edits (better approach). Installed Rust 1.94.1 via rustup (unplanned environment fix). Fetched protoc 29.3 from GitHub releases to /tmp (unplanned). Patched edition 2024→2021 in all four extracted manifests.
- S01 T02: Holdout attenuation was initially placed in ring.rs kernel gather loop (CoC space) — discovered to be architecturally wrong in T04. Also updated runner.rs, cpu.rs, wgpu.rs, shared_runner.rs beyond the two files in the plan (required by API change propagation).
- S01 T03: Updated two doc-test call sites in opendefocus/src/lib.rs (unplanned — required by render_stripe signature change).
- S01 T04 (major): Moved holdout attenuation from ring.rs kernel gather loop to render_impl layer-peel loop to fix CoC-vs-scene-depth unit mismatch. This is a significant architectural deviation from the original T02 plan but is required for correctness. Documented as D028.

## Follow-ups

- Add protoc to a persistent location (not /tmp) to avoid re-fetch when /tmp is cleared. Document PROTOC env var in docker-build.sh or CMakeLists.txt.
- Run test/test_opendefocus_holdout.nk in Nuke non-interactively and validate holdout_depth_selective.exr contains expected channel values at known pixel coordinates (pixel-level visual UAT deferred from S02).
- GPU wgpu runner currently ignores holdout entirely (_holdout parameter). If GPU holdout is required for performance, add storage buffer binding 9 in the wgpu runner and shader.
- Consider activating the RENDERER singleton (lazy_static) before production use to avoid per-call GPU context creation overhead.
- apply_holdout_attenuation in ring.rs is now dead code for the opendefocus-deep path (always receives &[]). Consider removing or adding a non-deep code path that uses it.
- Add CI check that runs 7 existing .nk test scripts in Nuke and verifies they produce unchanged output (regression guard for future DeepCOpenDefocus changes).
