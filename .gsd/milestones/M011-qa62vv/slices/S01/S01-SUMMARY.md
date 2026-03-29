---
id: S01
parent: M011-qa62vv
milestone: M011-qa62vv
provides:
  - Four forked opendefocus 0.1.10 crates as workspace path deps under crates/
  - HoldoutData C struct and opendefocus_deep_render_holdout FFI entry point in opendefocus_deep.h
  - holdout: &[f32] parameter threaded through entire CPU runner stack (kernel → runner → engine → renderer → FFI)
  - CPU unit test test_holdout_attenuates_background_samples proving depth-correct T attenuation
  - 19 kernel unit tests proving all holdout branch conditions
  - Knowledge: scene-depth vs CoC-space mismatch — never compare sample.coc against holdout depth breakpoints
requires:
  []
affects:
  - S02
key_files:
  - crates/opendefocus-kernel/src/lib.rs
  - crates/opendefocus-kernel/src/stages/ring.rs
  - crates/opendefocus/src/runners/runner.rs
  - crates/opendefocus/src/runners/cpu.rs
  - crates/opendefocus/src/runners/wgpu.rs
  - crates/opendefocus/src/runners/shared_runner.rs
  - crates/opendefocus/src/worker/engine.rs
  - crates/opendefocus/src/lib.rs
  - crates/opendefocus-deep/src/lib.rs
  - crates/opendefocus-deep/include/opendefocus_deep.h
  - Cargo.toml
  - Cargo.lock
key_decisions:
  - Used [patch.crates-io] to redirect four opendefocus registry deps to local path deps — cleaner than editing normalized manifests
  - Installed Rust 1.94.1 via rustup; system 1.75 cannot parse edition 2024 transitive deps
  - Patched edition=2024 to edition=2021 in all four extracted crate manifests for compatibility
  - Holdout attenuation applied in render_impl (scene-depth space), NOT in kernel gather loop (CoC space) — CoC vs scene-depth unit mismatch makes kernel application a silent no-op
  - Pass empty holdout to render_stripe to prevent double-attenuation between render_impl and kernel
  - render_impl private helper shares code path for both opendefocus_deep_render (&[] holdout) and opendefocus_deep_render_holdout (real holdout)
  - wgpu runner accepts holdout param as _holdout (ignored) — GPU holdout deferred to S02
patterns_established:
  - [patch.crates-io] pattern for forking a family of published crates to workspace path deps
  - Holdout attenuation in render_impl (scene-depth) not kernel (CoC) — the correct placement for depth-correct holdout in the opendefocus-deep pipeline
  - Empty holdout sentinel (&[]) to render_stripe prevents double-attenuation when render_impl has already applied T
  - render_impl private extraction for multiple FFI entry points sharing one implementation path
observability_surfaces:
  - none
drill_down_paths:
  - .gsd/milestones/M011-qa62vv/slices/S01/tasks/T01-SUMMARY.md
  - .gsd/milestones/M011-qa62vv/slices/S01/tasks/T02-SUMMARY.md
  - .gsd/milestones/M011-qa62vv/slices/S01/tasks/T03-SUMMARY.md
  - .gsd/milestones/M011-qa62vv/slices/S01/tasks/T04-SUMMARY.md
duration: ""
verification_result: passed
completed_at: 2026-03-29T11:32:21.138Z
blocker_discovered: false
---

# S01: Fork opendefocus-kernel with holdout gather

**Forked four opendefocus 0.1.10 crates as path deps, threaded holdout transmittance through the full CPU stack (kernel → runner → FFI), and proved depth-correct T attenuation with a passing unit test (green z=2 uncut, red/blue z=5 attenuated 10×).**

## What Happened

## S01: Fork opendefocus-kernel with holdout gather

### Overview

S01 delivered a complete CPU-path holdout transmittance pipeline for DeepCOpenDefocus, from forked workspace crates through kernel attenuation logic to a passing integration unit test. Four tasks progressed sequentially, with T04 surfacing and fixing a critical CoC-vs-scene-depth bug that originated in T02.

### T01 — Extract crates and establish workspace

Downloaded opendefocus 0.1.10 tarballs for all four crates (`opendefocus-kernel`, `opendefocus`, `opendefocus-shared`, `opendefocus-datastructure`) from `static.crates.io` and extracted them to `crates/`. Used `[patch.crates-io]` in the workspace root rather than editing each normalized manifest — cleaner and more maintainable. Two unplanned environment issues were resolved: system Rust 1.75 cannot parse `edition = "2024"` manifests in transitive deps (installed Rust 1.94.1 via rustup); `prost-build` requires `protoc` which needs root to install via apt (fetched protoc 29.3 from GitHub releases to `/tmp`). Patched `edition = "2024"` → `edition = "2021"` in extracted manifests for compatibility. Deleted the v4 `Cargo.lock` and let cargo regenerate a v3 lockfile. Result: `cargo check -p opendefocus-deep` passes with zero errors.

### T02 — Add holdout parameter to kernel CPU path

Modified `opendefocus-kernel` to thread `holdout: &[f32]` through the CPU execution path under `#[cfg(not(spirv))]` guards, leaving the SPIR-V entry point completely unchanged. Added `apply_holdout_attenuation` helper that reads `(d0, T0, d1, T1)` breakpoints per output pixel and attenuates `sample.color`, `sample.alpha`, `sample.alpha_masked` (not `kernel` or `weight`). Also updated `ConvolveRunner` trait and all runner implementations (cpu, wgpu, shared_runner) to carry the parameter — necessary because the API change would break compilation without it. The wgpu runner accepts the parameter as `_holdout` (ignored). Added 7 unit tests covering all branches: below d0 (T=1.0), between d0/d1 (T=T0), beyond d1 (T=T1), empty holdout, out-of-bounds pixel, multi-pixel lookup, and kernel/weight unchanged invariant. All 19 kernel tests pass.

**Key bug noted for T04:** `sample.coc` is a CoC radius in pixels, not a scene depth. The T02 implementation compared it against holdout breakpoints encoded in scene-depth units — a unit mismatch that would cause silent no-attenuation at runtime.

### T03 — Thread holdout through full runner stack and C FFI

Completed the end-to-end chain that T02 left incomplete at `convolve()`. Added `holdout: &[f32]` to `ConvolveRunner::convolve()`, `RenderEngine::render_convolve()/render()`, and `OpenDefocusRenderer::render_stripe()`. Extracted the full layer-peel body of `opendefocus_deep_render` into private `render_impl(... holdout: &[f32])`. The existing `opendefocus_deep_render` FFI entry point calls `render_impl` with `&[]`; the new `opendefocus_deep_render_holdout` builds a safe slice from a null-guarded `HoldoutData` C struct and calls `render_impl` with real holdout data. Added `HoldoutData { data: *const f32, len: u32 }` as `repr(C)` struct in Rust and matching C struct + function declaration in the header. Also updated doc-test examples for `render_stripe` (two rustdoc call sites) since the signature change broke them at compile time. Result: `cargo check -p opendefocus-deep` passes with zero errors.

### T04 — CPU unit test and scene-depth fix

Added `test_holdout_attenuates_background_samples` in `opendefocus-deep/src/lib.rs`. The test builds a 3×1 deep image: pixel 0 (red, z=5.0), pixel 1 (green, z=2.0), pixel 2 (blue, z=5.0); holdout at z=3.0 with T=0.1 for all three pixels. First run exposed the T02 bug: `apply_holdout_attenuation` in `ring.rs` compared `sample.coc` (pixels) against scene-depth breakpoints → no attenuation → test failed.

**Fix:** Moved holdout transmittance application to `render_impl` in `opendefocus-deep/src/lib.rs`, computed during the layer-peel loop where sample depths and breakpoints share the same scene-depth coordinate space. `render_stripe` receives `&[]` (empty holdout) to avoid double-attenuation. After fix: green pixel reads 0.97 (z=2.0 < d0=3.0 → T=1.0, unattenuated); red and blue pixels read 0.099 (z=5.0 > d0=3.0 → T=0.1, ~10× attenuation). Both test assertions pass.

### Final Verification

- `cargo check -p opendefocus-deep`: 0 errors  
- `cargo test -p opendefocus-kernel --lib`: 19/19 pass (12 pre-existing + 7 new holdout branch tests)  
- `cargo test -p opendefocus-deep -- holdout`: 1/1 pass (`test_holdout_attenuates_background_samples`)

### Key Architecture Decisions

1. **`[patch.crates-io]`** over per-manifest edits — cleaner for redirecting a family of published crates to local paths.
2. **`edition = "2021"` patch** applied to all four extracted manifests — system Rust 1.75 incompatible with edition 2024 transitive deps.
3. **Holdout attenuation in `render_impl`, not in kernel gather loop** — CoC-vs-scene-depth unit mismatch makes kernel-level application a silent no-op. Scene-depth correct placement is `render_impl` during layer extraction.
4. **Empty holdout to `render_stripe`** — prevents double-attenuation between `render_impl` and the kernel.
5. **`render_impl` private helper** — both old and new FFI entry points share one code path, zero duplication.
6. **wgpu GPU runner ignores holdout** — full GPU holdout (storage buffer binding 9) deferred to S02.


## Verification

All slice-level verification commands passed:

1. `PATH=$HOME/.cargo/bin:$PATH PROTOC=/tmp/protoc_dir/bin/protoc cargo check -p opendefocus-deep 2>&1 | grep -c '^error'` → 0 (exit 1 from grep means zero matches = zero errors)
2. `PATH=$HOME/.cargo/bin:$PATH PROTOC=/tmp/protoc_dir/bin/protoc cargo test -p opendefocus-kernel --lib` → 19/19 pass
3. `PATH=$HOME/.cargo/bin:$PATH PROTOC=/tmp/protoc_dir/bin/protoc cargo test -p opendefocus-deep -- holdout` → 1/1 pass (`test_holdout_attenuates_background_samples ... ok`)

## Requirements Advanced

- R049 — Kernel is forked and modified to accept holdout transmittance data; CPU path applies depth-correct T attenuation during layer-peel, not just sample accumulation
- R057 — Fork is established: four opendefocus 0.1.10 crates extracted to crates/ as workspace path dependencies

## Requirements Validated

None.

## New Requirements Surfaced

None.

## Requirements Invalidated or Re-scoped

None.

## Deviations

- T01: Used `[patch.crates-io]` instead of per-crate manifest edits. Installed Rust 1.94.1 via rustup (unplanned). Fetched protoc from GitHub (unplanned). Patched edition 2024→2021 in extracted manifests.
- T02: Also updated runner.rs, cpu.rs, wgpu.rs, shared_runner.rs beyond the two kernel files named in the plan — required by API change. Used `sample.coc` as depth proxy (later found to be wrong).
- T03: Updated two doc-test call sites in opendefocus/src/lib.rs (unplanned — required by signature change).
- T04: Moved holdout attenuation from ring.rs kernel gather loop to render_impl layer-peel loop to fix scene-depth vs CoC-space mismatch. This is a significant architectural deviation from the original T02 plan but is required for correctness.

## Known Limitations

- `apply_holdout_attenuation` in `ring.rs` is now dead code when called from opendefocus-deep (always receives empty holdout). Useful for non-deep paths.
- GPU wgpu runner ignores holdout entirely — full GPU holdout deferred to S02.
- protoc binary lives in `/tmp` and must be re-fetched if `/tmp` is cleared. Future cargo commands require `PROTOC=/tmp/protoc_dir/bin/protoc`.
- `sample.coc` in the kernel represents CoC radius in pixels — cannot be used directly for holdout depth comparison without conversion to scene depth.

## Follow-ups

- S02: Wire holdout through GPU pipeline (wgpu storage buffer binding 9) and run integration test with Docker build.
- Consider adding protoc to a persistent location to avoid re-fetch on /tmp clear.
- Consider activating the RENDERER singleton (lazy static) before production use to avoid per-call GPU context creation overhead.

## Files Created/Modified

- `Cargo.toml` — Added four opendefocus crate workspace members and [patch.crates-io] section
- `Cargo.lock` — Regenerated as v3 (deleted v4 lockfile)
- `crates/opendefocus-kernel/src/lib.rs` — Added holdout/output_width params to global_entrypoint under cfg(not(spirv)) guard
- `crates/opendefocus-kernel/src/stages/ring.rs` — Added apply_holdout_attenuation helper; added holdout/output_width params to calculate_ring; added 7 unit tests
- `crates/opendefocus/src/runners/runner.rs` — Added holdout param to ConvolveRunner trait (execute_kernel_pass and convolve)
- `crates/opendefocus/src/runners/cpu.rs` — Updated execute_kernel_pass to thread holdout into rayon parallel loop
- `crates/opendefocus/src/runners/wgpu.rs` — Updated execute_kernel_pass signature; ignores holdout (_holdout)
- `crates/opendefocus/src/runners/shared_runner.rs` — Updated to carry holdout parameter
- `crates/opendefocus/src/worker/engine.rs` — Added holdout param to render_convolve/render methods
- `crates/opendefocus/src/lib.rs` — Added holdout param to render_stripe; updated doc-test call sites
- `crates/opendefocus-deep/src/lib.rs` — Added HoldoutData repr(C) struct; extracted render_impl(); added opendefocus_deep_render_holdout FFI; added holdout_transmittance() in scene-depth space; added test_holdout_attenuates_background_samples unit test
- `crates/opendefocus-deep/include/opendefocus_deep.h` — Added HoldoutData C struct and opendefocus_deep_render_holdout function declaration
