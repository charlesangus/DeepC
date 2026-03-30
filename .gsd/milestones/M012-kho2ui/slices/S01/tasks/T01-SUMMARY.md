---
id: T01
parent: S01
milestone: M012-kho2ui
provides: []
requires: []
affects: []
key_files: ["crates/opendefocus-deep/src/lib.rs"]
key_decisions: ["Used Arc<Mutex<OpenDefocusRenderer>> inside OnceLock as specified in task plan; Mutex provides forward safety for future mutable operations", "Mutex is acquired once per render_impl call and held for the full layer loop — not per-layer — because render_stripe takes &self and the MutexGuard deref chain is composable"]
patterns_established: []
drill_down_paths: []
observability_surfaces: []
duration: ""
verification_result: "grep verification passed (OnceLock and test_singleton_reuse both present). cargo check passed (no type errors). cargo test ran 2 tests — both passed: test_holdout_attenuates_background_samples and test_singleton_reuse. C++ syntax check (bash scripts/verify-s01-syntax.sh) also passed."
completed_at: 2026-03-30T22:54:18.442Z
blocker_discovered: false
---

# T01: Replace per-call OpenDefocusRenderer init with static OnceLock singleton in opendefocus-deep, eliminating async init overhead on every FFI call after the first

> Replace per-call OpenDefocusRenderer init with static OnceLock singleton in opendefocus-deep, eliminating async init overhead on every FFI call after the first

## What Happened
---
id: T01
parent: S01
milestone: M012-kho2ui
key_files:
  - crates/opendefocus-deep/src/lib.rs
key_decisions:
  - Used Arc<Mutex<OpenDefocusRenderer>> inside OnceLock as specified in task plan; Mutex provides forward safety for future mutable operations
  - Mutex is acquired once per render_impl call and held for the full layer loop — not per-layer — because render_stripe takes &self and the MutexGuard deref chain is composable
duration: ""
verification_result: passed
completed_at: 2026-03-30T22:54:18.448Z
blocker_discovered: false
---

# T01: Replace per-call OpenDefocusRenderer init with static OnceLock singleton in opendefocus-deep, eliminating async init overhead on every FFI call after the first

**Replace per-call OpenDefocusRenderer init with static OnceLock singleton in opendefocus-deep, eliminating async init overhead on every FFI call after the first**

## What Happened

The render_impl function in crates/opendefocus-deep/src/lib.rs previously called block_on(OpenDefocusRenderer::new(…)) on every single FFI invocation. This full async init (device enumeration, pipeline setup) was the primary CPU bottleneck for repeated render calls. The fix introduces a static RENDERER: OnceLock<Arc<Mutex<OpenDefocusRenderer>>> and a get_or_init_renderer(use_gpu) helper that initialises the renderer exactly once on first call and returns a cloned Arc to all subsequent callers. The Mutex is locked once at the start of render_impl and held for the full layer loop, providing safe shared access to the immutable render_stripe method. The new test_singleton_reuse test calls opendefocus_deep_render twice on a 2×2 CPU image and asserts both succeed without panic and produce non-NaN output.

## Verification

grep verification passed (OnceLock and test_singleton_reuse both present). cargo check passed (no type errors). cargo test ran 2 tests — both passed: test_holdout_attenuates_background_samples and test_singleton_reuse. C++ syntax check (bash scripts/verify-s01-syntax.sh) also passed.

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `grep -q 'OnceLock' crates/opendefocus-deep/src/lib.rs && grep -q 'test_singleton_reuse' crates/opendefocus-deep/src/lib.rs` | 0 | ✅ pass | 50ms |
| 2 | `cargo check -p opendefocus-deep --features 'opendefocus/protobuf-vendored'` | 0 | ✅ pass | 123000ms |
| 3 | `cargo test -p opendefocus-deep --features 'opendefocus/protobuf-vendored' -- --nocapture` | 0 | ✅ pass | 42500ms |
| 4 | `bash scripts/verify-s01-syntax.sh` | 0 | ✅ pass | 300ms |


## Deviations

None.

## Known Issues

None.

## Files Created/Modified

- `crates/opendefocus-deep/src/lib.rs`


## Deviations
None.

## Known Issues
None.
