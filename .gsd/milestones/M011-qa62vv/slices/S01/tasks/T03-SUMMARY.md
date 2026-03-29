---
id: T03
parent: S01
milestone: M011-qa62vv
provides: []
requires: []
affects: []
key_files: ["crates/opendefocus/src/runners/runner.rs", "crates/opendefocus/src/worker/engine.rs", "crates/opendefocus/src/lib.rs", "crates/opendefocus-deep/src/lib.rs", "crates/opendefocus-deep/include/opendefocus_deep.h"]
key_decisions: ["Threaded holdout through convolve() rather than leaving &[] stub in runner.rs — required by T03 contract to let opendefocus-deep pass real holdout data to render_stripe", "render_impl() refactor: extracted full layer-peel body so both opendefocus_deep_render (&[] holdout) and opendefocus_deep_render_holdout (real holdout) share one code path with zero duplication", "public render() in opendefocus/src/lib.rs keeps no holdout param — passes &[] internally; render_stripe() gets explicit holdout param to support the deep render call site"]
patterns_established: []
drill_down_paths: []
observability_surfaces: []
duration: ""
verification_result: "Ran PROTOC=/tmp/protoc_dir/bin/protoc /home/latuser/.cargo/bin/cargo check -p opendefocus-deep 2>&1 | grep -c '^error' | grep -q '^0$' && echo PASS — output: PASS. Also ran cargo test -p opendefocus-kernel: 19/19 tests pass (holdout unit tests from T02 unaffected)."
completed_at: 2026-03-29T04:18:43.422Z
blocker_discovered: false
---

# T03: Added HoldoutData to C FFI and Rust, extracted render_impl(), added opendefocus_deep_render_holdout, and threaded holdout through the full runner stack from render_stripe() to execute_kernel_pass()

> Added HoldoutData to C FFI and Rust, extracted render_impl(), added opendefocus_deep_render_holdout, and threaded holdout through the full runner stack from render_stripe() to execute_kernel_pass()

## What Happened
---
id: T03
parent: S01
milestone: M011-qa62vv
key_files:
  - crates/opendefocus/src/runners/runner.rs
  - crates/opendefocus/src/worker/engine.rs
  - crates/opendefocus/src/lib.rs
  - crates/opendefocus-deep/src/lib.rs
  - crates/opendefocus-deep/include/opendefocus_deep.h
key_decisions:
  - Threaded holdout through convolve() rather than leaving &[] stub in runner.rs — required by T03 contract to let opendefocus-deep pass real holdout data to render_stripe
  - render_impl() refactor: extracted full layer-peel body so both opendefocus_deep_render (&[] holdout) and opendefocus_deep_render_holdout (real holdout) share one code path with zero duplication
  - public render() in opendefocus/src/lib.rs keeps no holdout param — passes &[] internally; render_stripe() gets explicit holdout param to support the deep render call site
duration: ""
verification_result: passed
completed_at: 2026-03-29T04:18:43.423Z
blocker_discovered: false
---

# T03: Added HoldoutData to C FFI and Rust, extracted render_impl(), added opendefocus_deep_render_holdout, and threaded holdout through the full runner stack from render_stripe() to execute_kernel_pass()

**Added HoldoutData to C FFI and Rust, extracted render_impl(), added opendefocus_deep_render_holdout, and threaded holdout through the full runner stack from render_stripe() to execute_kernel_pass()**

## What Happened

T02 had already added holdout: &[f32] to execute_kernel_pass and runner impls, but left convolve() hardcoded to &[]. T03 completed the chain: added holdout param to ConvolveRunner::convolve(), RenderEngine::render_convolve/render(), and OpenDefocusRenderer::render_stripe(). Extracted the full layer-peel body of opendefocus_deep_render into private render_impl(... holdout: &[f32]); the existing FFI entry point calls render_impl with &[]; new opendefocus_deep_render_holdout builds a safe slice from HoldoutData (null-guarded) and calls render_impl with real holdout. Added HoldoutData repr(C) struct in Rust and matching C struct + function declaration in the header. Updated doc-tests for render_stripe (two rustdoc examples). cargo check -p opendefocus-deep: zero errors. cargo test -p opendefocus-kernel: 19/19 pass.

## Verification

Ran PROTOC=/tmp/protoc_dir/bin/protoc /home/latuser/.cargo/bin/cargo check -p opendefocus-deep 2>&1 | grep -c '^error' | grep -q '^0$' && echo PASS — output: PASS. Also ran cargo test -p opendefocus-kernel: 19/19 tests pass (holdout unit tests from T02 unaffected).

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `PROTOC=/tmp/protoc_dir/bin/protoc /home/latuser/.cargo/bin/cargo check -p opendefocus-deep 2>&1 | grep -c '^error' | grep -q '^0$' && echo PASS` | 0 | ✅ pass | 2700ms |
| 2 | `PROTOC=/tmp/protoc_dir/bin/protoc /home/latuser/.cargo/bin/cargo test -p opendefocus-kernel 2>&1 | tail -5` | 0 | ✅ pass | 4200ms |


## Deviations

Also updated opendefocus/src/lib.rs doc-tests for render_stripe (two call sites in rustdoc examples) since the signature change broke them at compile time. render_impl is fn not pub fn — private to the crate, consistent with task plan intent.

## Known Issues

None.

## Files Created/Modified

- `crates/opendefocus/src/runners/runner.rs`
- `crates/opendefocus/src/worker/engine.rs`
- `crates/opendefocus/src/lib.rs`
- `crates/opendefocus-deep/src/lib.rs`
- `crates/opendefocus-deep/include/opendefocus_deep.h`


## Deviations
Also updated opendefocus/src/lib.rs doc-tests for render_stripe (two call sites in rustdoc examples) since the signature change broke them at compile time. render_impl is fn not pub fn — private to the crate, consistent with task plan intent.

## Known Issues
None.
