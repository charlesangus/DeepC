---
id: T03
parent: S02
milestone: M009-mso5fb
provides: []
requires: []
affects: []
key_files: ["test/test_opendefocus.nk", "scripts/verify-s01-syntax.sh"]
key_decisions: ["Appended S02 checks to verify-s01-syntax.sh rather than a separate script to keep one entry-point", "clang header check made optional to avoid CI failures in minimal environments"]
patterns_established: []
drill_down_paths: []
observability_surfaces: []
duration: ""
verification_result: "bash scripts/verify-s01-syntax.sh exits 0 with all three .cpp syntax checks passing plus 'test/test_opendefocus.nk exists — OK' and 'All S02 checks passed.' grep confirms DeepConstant, DeepCOpenDefocus, and Write nodes are present in the .nk file."
completed_at: 2026-03-28T11:01:52.373Z
blocker_discovered: false
---

# T03: Created test/test_opendefocus.nk with DeepConstant/DeepCOpenDefocus/Write nodes and extended verify-s01-syntax.sh to enforce .nk existence

> Created test/test_opendefocus.nk with DeepConstant/DeepCOpenDefocus/Write nodes and extended verify-s01-syntax.sh to enforce .nk existence

## What Happened
---
id: T03
parent: S02
milestone: M009-mso5fb
key_files:
  - test/test_opendefocus.nk
  - scripts/verify-s01-syntax.sh
key_decisions:
  - Appended S02 checks to verify-s01-syntax.sh rather than a separate script to keep one entry-point
  - clang header check made optional to avoid CI failures in minimal environments
duration: ""
verification_result: passed
completed_at: 2026-03-28T11:01:52.373Z
blocker_discovered: false
---

# T03: Created test/test_opendefocus.nk with DeepConstant/DeepCOpenDefocus/Write nodes and extended verify-s01-syntax.sh to enforce .nk existence

**Created test/test_opendefocus.nk with DeepConstant/DeepCOpenDefocus/Write nodes and extended verify-s01-syntax.sh to enforce .nk existence**

## What Happened

Created test/ directory and wrote test/test_opendefocus.nk as a plain-text Nuke script containing a DeepConstant node (128×72, RGBA 0.5/depth 5.0), a DeepCOpenDefocus node with default lens params (focal_length=50, fstop=2.8, focus_distance=5.0, sensor_size_mm=36), and a Write node targeting /tmp/test_opendefocus.exr. Extended scripts/verify-s01-syntax.sh by appending an S02 block that checks the .nk file exists and optionally lints the C header with clang if available. The prior verification failure was the gate runner interpreting description text as a shell command, not a code defect.

## Verification

bash scripts/verify-s01-syntax.sh exits 0 with all three .cpp syntax checks passing plus 'test/test_opendefocus.nk exists — OK' and 'All S02 checks passed.' grep confirms DeepConstant, DeepCOpenDefocus, and Write nodes are present in the .nk file.

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `bash scripts/verify-s01-syntax.sh` | 0 | ✅ pass | 2000ms |
| 2 | `grep -E "DeepConstant|DeepCOpenDefocus|Write" test/test_opendefocus.nk` | 0 | ✅ pass | 50ms |


## Deviations

None.

## Known Issues

Nuke runtime is not available in this environment so nuke -x cannot be run; structural verification only. Render verification deferred to T04 docker build step.

## Files Created/Modified

- `test/test_opendefocus.nk`
- `scripts/verify-s01-syntax.sh`


## Deviations
None.

## Known Issues
Nuke runtime is not available in this environment so nuke -x cannot be run; structural verification only. Render verification deferred to T04 docker build step.
