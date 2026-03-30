---
estimated_steps: 7
estimated_files: 1
skills_used: []
---

# T04: Add timing unit test documenting singleton + prep speedup

Add a `test_render_timing` unit test in `crates/opendefocus-deep/src/lib.rs` that:
1. Calls `opendefocus_deep_render` on a minimal 4×4 single-sample image, recording elapsed time with `std::time::Instant` for the first call (cold, singleton init) and a second call (warm, reuses singleton).
2. Prints both elapsed times with `eprintln!` (visible with `--nocapture`).
3. Asserts that the warm call is not pathologically slow (assert `warm_ms < 5000` — this is not a tight SLA, just guards against regression).
4. Add a `test_holdout_timing` variant that calls `opendefocus_deep_render_holdout` twice on a 4×4 image with a minimal holdout slice, similarly printing warm/cold times.

These tests run with `cargo test -p opendefocus-deep -- --nocapture` in Docker. They don't assert a specific speedup ratio (difficult to measure in a unit test environment) but produce timing output that documents the before/after intent.

Also verify the full test suite (`test_holdout_attenuates_background_samples`, `test_singleton_reuse`, `test_render_timing`, `test_holdout_timing`) are syntactically correct by running `bash scripts/verify-s01-syntax.sh` (which only checks C++ syntax, but the Rust code must at least be structurally consistent with prior tasks).

## Inputs

- `crates/opendefocus-deep/src/lib.rs`

## Expected Output

- `crates/opendefocus-deep/src/lib.rs`

## Verification

grep -q 'test_render_timing' crates/opendefocus-deep/src/lib.rs && grep -q 'test_holdout_timing' crates/opendefocus-deep/src/lib.rs && bash scripts/verify-s01-syntax.sh
