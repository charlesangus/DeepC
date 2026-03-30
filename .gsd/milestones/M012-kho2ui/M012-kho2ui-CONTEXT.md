# M012-kho2ui: DeepCOpenDefocus Performance

**Gathered:** 2026-03-30
**Status:** Ready for planning

## Project Description

DeepCOpenDefocus is functionally correct but unusably slow for interactive work. Even trivial test scenes (256×256, single deep layer) take 20+ seconds on CPU. The user's target is under 10 seconds for these cases — fast enough to scrub parameters and see results.

## Why This Milestone

The node ships and produces correct output, but no artist will use it at current render times. Performance is the blocker to adoption.

## User-Visible Outcome

### When this milestone is complete, the user can:

- Render a 256×256 single-layer deep scene through DeepCOpenDefocus in under 10 seconds on CPU
- Adjust a quality knob to trade speed for quality during interactive preview
- See the defocused result without re-rendering when the Nuke viewer re-requests the same region

### Entry point / environment

- Entry point: Nuke node graph → DeepCOpenDefocus node → Viewer or Write node
- Environment: Nuke 16.0+ on Linux workstation
- Live dependencies involved: none

## Completion Class

- Contract complete means: Rust unit tests pass; cargo check clean; scripts/verify-s01-syntax.sh passes
- Integration complete means: Docker build exits 0; DeepCOpenDefocus.so produced; nuke -x renders correct output
- Operational complete means: 256×256 single-layer CPU render under 10 seconds measured via nuke -x -m 1

## Final Integrated Acceptance

To call this milestone complete, we must prove:

- 256×256 single-layer DeepCConstant → DeepCOpenDefocus → Write renders under 10 seconds (nuke -x -m 1)
- Multi-layer test (3 DeepCConstant merged) renders without regression in correctness
- Quality knob visible in Nuke and affects render time (Low faster than High)
- Docker build exits 0

## Risks and Unknowns

- opendefocus engine's `render_stripe`/`RenderEngine::render` owns the full prep pipeline (filter, mipmaps, inpaint) — hoisting prep out requires modifying the forked crate's internal API, which could break assumptions the engine makes about state
- PlanarIop cache invalidation — Nuke's plane cache has its own invalidation logic tied to `hash()` and knob changes; custom caching on top must not fight it
- Singleton renderer lifetime — the `OpenDefocusRenderer` holds a `SharedRunner` with a `CpuRunner` (rayon thread pool); the singleton must be safe to call from Nuke's multi-threaded engine() calls

## Existing Codebase / Prior Art

- `crates/opendefocus-deep/src/lib.rs` — render_impl with layer-peel loop, FFI entry points, HoldoutData
- `crates/opendefocus/src/lib.rs` — OpenDefocusRenderer, render_stripe
- `crates/opendefocus/src/worker/engine.rs` — RenderEngine::render (filter prep, mipmap creation, inpaint, chunk dispatch)
- `crates/opendefocus/src/runners/cpu.rs` — CpuRunner::execute_kernel_pass (rayon parallel per-pixel)
- `crates/opendefocus/src/runners/runner.rs` — ConvolveRunner trait, convolve method
- `src/DeepCOpenDefocus.cpp` — PlanarIop subclass, renderStripe, deep sample flattening

> See `.gsd/DECISIONS.md` for all architectural and pattern decisions — it is an append-only register; read it during planning, append to it during execution.

## Relevant Requirements

- R063 — Singleton renderer (eliminate per-call init overhead)
- R064 — Per-layer dedup (hoist filter/mipmap/inpaint prep out of layer loop)
- R065 — Full-image PlanarIop cache (render once, serve stripes from cache)
- R066 — Quality knob (user control over speed/quality tradeoff)
- R067 — 256×256 single-layer under 10s CPU (acceptance criterion)

## Scope

### In Scope

- CPU render path performance
- Rust-side render_impl restructuring
- Forked opendefocus crate API modifications for prep/dispatch separation
- C++ PlanarIop caching in DeepCOpenDefocus.cpp
- Quality knob (Enumeration_knob → FFI → opendefocus Quality enum)

### Out of Scope / Non-Goals

- GPU/wgpu path improvements (explicitly deferred per user)
- GPU device loss handling (addressed in quick tasks 2-4, not revisited)
- New visual features or holdout changes
- Windows build changes

## Technical Constraints

- Forked opendefocus crates are workspace path deps under crates/ — full source control
- PlanarIop::renderStripe receives sub-region bounds from Nuke; the deep input must be fetched for the full bbox
- Singleton must handle concurrent renderStripe calls from Nuke's thread pool (Arc<Mutex> or similar)
- opendefocus Quality enum is proto-generated i32 — use .into() for assignment

## Integration Points

- opendefocus-deep FFI boundary — new `quality` parameter added to opendefocus_deep_render / opendefocus_deep_render_holdout
- C++ DeepCOpenDefocus knobs — new Enumeration_knob for quality level
- PlanarIop plane cache — custom render cache layered on top

## Open Questions

- How much speedup comes from the singleton alone vs. hoisting prep? Profiling in S01 will answer.
- Whether inpaint preprocessing can be skipped entirely for the deep-defocus use case (it's designed for flat images with gaps, but layer-peel images have transparent regions, not gaps) — investigate in S01.
