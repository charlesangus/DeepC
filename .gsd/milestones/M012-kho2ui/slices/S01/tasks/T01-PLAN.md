---
estimated_steps: 1
estimated_files: 1
skills_used: []
---

# T01: Replace per-call renderer init with static singleton

In `crates/opendefocus-deep/src/lib.rs`, replace the local `let renderer = if use_gpu != 0 { … }` block in `render_impl` with a `std::sync::OnceLock<Arc<Mutex<OpenDefocusRenderer>>>` static. The lock is initialised on first call using the caller's `use_gpu` flag; subsequent calls lock the Mutex, call `renderer.render_stripe(…)`, then release. Add a `test_singleton_reuse` unit test that calls `opendefocus_deep_render` (CPU path) twice on a trivially small image and asserts both calls succeed without panicking.

## Inputs

- `crates/opendefocus-deep/src/lib.rs`

## Expected Output

- `crates/opendefocus-deep/src/lib.rs`

## Verification

grep -q 'OnceLock' crates/opendefocus-deep/src/lib.rs && grep -q 'test_singleton_reuse' crates/opendefocus-deep/src/lib.rs
