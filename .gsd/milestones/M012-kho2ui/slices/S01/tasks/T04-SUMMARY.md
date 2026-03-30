---
id: T04
parent: S01
milestone: M012-kho2ui
provides: []
requires: []
affects: []
key_files: ["crates/opendefocus-deep/src/lib.rs"]
key_decisions: ["Both timing tests use 4×4 single-sample image on CPU path matching prior test_singleton_reuse pattern; only assertion is warm_ms < 5000 as a loose regression guard", "HoldoutData in test_holdout_timing uses T0=T1=1.0 to isolate timing from correctness concerns"]
patterns_established: []
drill_down_paths: []
observability_surfaces: []
duration: ""
verification_result: "grep confirmed both test names present in lib.rs (exit 0); bash scripts/verify-s01-syntax.sh passed all three C++ files and seven .nk test artifacts (exit 0, 0.658s)."
completed_at: 2026-03-30T23:08:28.891Z
blocker_discovered: false
---

# T04: Added test_render_timing and test_holdout_timing unit tests to opendefocus-deep/src/lib.rs, printing cold/warm elapsed times and asserting warm < 5000 ms

> Added test_render_timing and test_holdout_timing unit tests to opendefocus-deep/src/lib.rs, printing cold/warm elapsed times and asserting warm < 5000 ms

## What Happened
---
id: T04
parent: S01
milestone: M012-kho2ui
key_files:
  - crates/opendefocus-deep/src/lib.rs
key_decisions:
  - Both timing tests use 4×4 single-sample image on CPU path matching prior test_singleton_reuse pattern; only assertion is warm_ms < 5000 as a loose regression guard
  - HoldoutData in test_holdout_timing uses T0=T1=1.0 to isolate timing from correctness concerns
duration: ""
verification_result: passed
completed_at: 2026-03-30T23:08:28.891Z
blocker_discovered: false
---

# T04: Added test_render_timing and test_holdout_timing unit tests to opendefocus-deep/src/lib.rs, printing cold/warm elapsed times and asserting warm < 5000 ms

**Added test_render_timing and test_holdout_timing unit tests to opendefocus-deep/src/lib.rs, printing cold/warm elapsed times and asserting warm < 5000 ms**

## What Happened

Inserted two new #[test] functions immediately before the existing test_singleton_reuse test in the #[cfg(test)] module. test_render_timing calls opendefocus_deep_render twice on a 4×4 single-sample image (cold then warm), captures std::time::Instant elapsed for each, prints both with eprintln!, and asserts warm_ms < 5000. test_holdout_timing does the same via opendefocus_deep_render_holdout with a trivial T=1.0 holdout slice (no attenuation). Both use CPU path (use_gpu=0, quality=0) to exercise full render_impl without GPU or Docker.

## Verification

grep confirmed both test names present in lib.rs (exit 0); bash scripts/verify-s01-syntax.sh passed all three C++ files and seven .nk test artifacts (exit 0, 0.658s).

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `grep -q 'test_render_timing' crates/opendefocus-deep/src/lib.rs` | 0 | ✅ pass | 10ms |
| 2 | `grep -q 'test_holdout_timing' crates/opendefocus-deep/src/lib.rs` | 0 | ✅ pass | 5ms |
| 3 | `bash scripts/verify-s01-syntax.sh` | 0 | ✅ pass | 658ms |


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
