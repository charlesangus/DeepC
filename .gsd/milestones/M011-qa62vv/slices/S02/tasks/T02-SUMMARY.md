---
id: T02
parent: S02
milestone: M011-qa62vv
provides: []
requires: []
affects: []
key_files: ["test/test_opendefocus_holdout.nk"]
key_decisions: ["LIFO Nuke stack: holdout pushed first, back second, front third, then DeepMerge consumes front+back leaving [merge, holdout] for DeepCOpenDefocus inputs 2", "Output renamed to holdout_depth_selective.exr to distinguish from the old single-subject test"]
patterns_established: []
drill_down_paths: []
observability_surfaces: []
duration: ""
verification_result: "Ran task-plan verification greps — all four patterns (DeepMerge, holdout_depth_selective.exr, 2.0, 5.0) found in the file (exit 0). Ran bash scripts/verify-s01-syntax.sh — all cpp syntax checks passed and all .nk test files present (exit 0)."
completed_at: 2026-03-29T11:50:40.538Z
blocker_discovered: false
---

# T02: Rewrote test/test_opendefocus_holdout.nk with two subjects at z=2 (green, passes through) and z=5 (red, attenuated), DeepMerge node, and Write to holdout_depth_selective.exr

> Rewrote test/test_opendefocus_holdout.nk with two subjects at z=2 (green, passes through) and z=5 (red, attenuated), DeepMerge node, and Write to holdout_depth_selective.exr

## What Happened
---
id: T02
parent: S02
milestone: M011-qa62vv
key_files:
  - test/test_opendefocus_holdout.nk
key_decisions:
  - LIFO Nuke stack: holdout pushed first, back second, front third, then DeepMerge consumes front+back leaving [merge, holdout] for DeepCOpenDefocus inputs 2
  - Output renamed to holdout_depth_selective.exr to distinguish from the old single-subject test
duration: ""
verification_result: passed
completed_at: 2026-03-29T11:50:40.539Z
blocker_discovered: false
---

# T02: Rewrote test/test_opendefocus_holdout.nk with two subjects at z=2 (green, passes through) and z=5 (red, attenuated), DeepMerge node, and Write to holdout_depth_selective.exr

**Rewrote test/test_opendefocus_holdout.nk with two subjects at z=2 (green, passes through) and z=5 (red, attenuated), DeepMerge node, and Write to holdout_depth_selective.exr**

## What Happened

The existing test had a single grey subject at z=5 with no DeepMerge node, which could not demonstrate depth-selective holdout behaviour. Rewrote the file entirely following LIFO Nuke stack ordering: holdout (alpha=0.5, front=3.0) pushed first, back (red, front=5.0) second, front (green, front=2.0) third, then DeepMerge {inputs 2} consumes front+back leaving [merge, holdout] for DeepCOpenDefocus {inputs 2}. Write node outputs to test/output/holdout_depth_selective.exr. This stack guarantees the z=5 red subject sits behind the holdout at z=3 (attenuated by T≈0.5) while the z=2 green subject is in front (passes through uncut).

## Verification

Ran task-plan verification greps — all four patterns (DeepMerge, holdout_depth_selective.exr, 2.0, 5.0) found in the file (exit 0). Ran bash scripts/verify-s01-syntax.sh — all cpp syntax checks passed and all .nk test files present (exit 0).

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `grep -q 'DeepMerge' test/test_opendefocus_holdout.nk && grep -q 'holdout_depth_selective.exr' test/test_opendefocus_holdout.nk && grep -q '2.0' test/test_opendefocus_holdout.nk && grep -q '5.0' test/test_opendefocus_holdout.nk` | 0 | ✅ pass | 100ms |
| 2 | `bash scripts/verify-s01-syntax.sh` | 0 | ✅ pass | 3000ms |


## Deviations

None.

## Known Issues

None.

## Files Created/Modified

- `test/test_opendefocus_holdout.nk`


## Deviations
None.

## Known Issues
None.
