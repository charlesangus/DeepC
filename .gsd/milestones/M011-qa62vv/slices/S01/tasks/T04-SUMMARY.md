---
id: T04
parent: S01
milestone: M011-qa62vv
provides: []
requires: []
affects: []
key_files: ["crates/opendefocus-deep/src/lib.rs"]
key_decisions: ["Moved holdout attenuation from kernel CoC-space to render_impl scene-depth space to fix scene-depth vs pixel-space mismatch", "Pass empty holdout to render_stripe to avoid double-attenuation"]
patterns_established: []
drill_down_paths: []
observability_surfaces: []
duration: ""
verification_result: "cargo test -p opendefocus-deep -- holdout: 1 test passed. cargo test -p opendefocus-deep -p opendefocus-kernel: 20 tests passed. Slice verification grep confirms PASS."
completed_at: 2026-03-29T11:28:12.382Z
blocker_discovered: false
---

# T04: Added CPU unit test proving depth-correct holdout transmittance attenuation, fixed scene-depth vs CoC-space mismatch

> Added CPU unit test proving depth-correct holdout transmittance attenuation, fixed scene-depth vs CoC-space mismatch

## What Happened
---
id: T04
parent: S01
milestone: M011-qa62vv
key_files:
  - crates/opendefocus-deep/src/lib.rs
key_decisions:
  - Moved holdout attenuation from kernel CoC-space to render_impl scene-depth space to fix scene-depth vs pixel-space mismatch
  - Pass empty holdout to render_stripe to avoid double-attenuation
duration: ""
verification_result: passed
completed_at: 2026-03-29T11:28:12.386Z
blocker_discovered: false
---

# T04: Added CPU unit test proving depth-correct holdout transmittance attenuation, fixed scene-depth vs CoC-space mismatch

**Added CPU unit test proving depth-correct holdout transmittance attenuation, fixed scene-depth vs CoC-space mismatch**

## What Happened

Wrote test_holdout_attenuates_background_samples in opendefocus-deep/src/lib.rs. Initial run exposed a critical bug: kernel's apply_holdout_attenuation compared sample.coc (pixels) against holdout scene-depth breakpoints, causing no attenuation. Fixed by adding holdout_transmittance() in scene-depth space during layer-peel extraction, passing empty holdout to render_stripe to avoid double-attenuation. After fix: green (z=2.0, in front of holdout) reads 0.97, red/blue (z=5.0, behind holdout with T=0.1) read 0.099 — correct 10× attenuation.

## Verification

cargo test -p opendefocus-deep -- holdout: 1 test passed. cargo test -p opendefocus-deep -p opendefocus-kernel: 20 tests passed. Slice verification grep confirms PASS.

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `PATH=$HOME/.cargo/bin:$PATH PROTOC=/tmp/protoc_dir/bin/protoc cargo test -p opendefocus-deep -- holdout 2>&1 | grep -q 'test.*ok' && echo PASS` | 0 | ✅ pass | 3700ms |
| 2 | `PATH=$HOME/.cargo/bin:$PATH PROTOC=/tmp/protoc_dir/bin/protoc cargo test -p opendefocus-deep -p opendefocus-kernel` | 0 | ✅ pass | 4000ms |


## Deviations

Moved holdout attenuation from kernel gather loop (ring.rs) to render_impl layer-peel loop (lib.rs) because kernel operates in CoC-pixel space while holdout breakpoints use scene depths.

## Known Issues

Kernel apply_holdout_attenuation in ring.rs is now effectively dead code when called from opendefocus-deep (always receives empty holdout). May be useful for non-deep paths.

## Files Created/Modified

- `crates/opendefocus-deep/src/lib.rs`


## Deviations
Moved holdout attenuation from kernel gather loop (ring.rs) to render_impl layer-peel loop (lib.rs) because kernel operates in CoC-pixel space while holdout breakpoints use scene depths.

## Known Issues
Kernel apply_holdout_attenuation in ring.rs is now effectively dead code when called from opendefocus-deep (always receives empty holdout). May be useful for non-deep paths.
