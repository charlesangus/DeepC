---
id: T02
parent: S01
milestone: M010-zkt9ww
provides: []
requires: []
affects: []
key_files: ["test/output/.gitignore", "test/test_opendefocus.nk", "test/test_opendefocus_multidepth.nk", "test/test_opendefocus_holdout.nk", "test/test_opendefocus_camera.nk", "test/test_opendefocus_cpu.nk", "test/test_opendefocus_empty.nk", "test/test_opendefocus_bokeh.nk"]
key_decisions: ["Used push 0 for null holdout input in camera.nk per Nuke stack convention", "DeepMerge operation plus for multi-depth additive compositing of discrete depth layers", "bokeh.nk uses DeepFromImage with set_z true / z 1.0 to convert single bright pixel to deep sample"]
patterns_established: []
drill_down_paths: []
observability_surfaces: []
duration: ""
verification_result: "ls verified all 7 .nk files exist (exit 0). grep confirmed test/output/ path in updated test_opendefocus.nk (exit 0). cat test/output/.gitignore confirmed *.exr and *.tmp entries (exit 0). sh scripts/verify-s01-syntax.sh re-run as regression — all C++ syntax checks and .nk existence check still pass (exit 0, 618ms)."
completed_at: 2026-03-28T16:19:55.144Z
blocker_discovered: false
---

# T02: Created test/output/.gitignore, updated test_opendefocus.nk Write path to test/output/, and authored 6 new .nk integration test scripts (multi-depth, holdout, camera, CPU-only, empty, bokeh)

> Created test/output/.gitignore, updated test_opendefocus.nk Write path to test/output/, and authored 6 new .nk integration test scripts (multi-depth, holdout, camera, CPU-only, empty, bokeh)

## What Happened
---
id: T02
parent: S01
milestone: M010-zkt9ww
key_files:
  - test/output/.gitignore
  - test/test_opendefocus.nk
  - test/test_opendefocus_multidepth.nk
  - test/test_opendefocus_holdout.nk
  - test/test_opendefocus_camera.nk
  - test/test_opendefocus_cpu.nk
  - test/test_opendefocus_empty.nk
  - test/test_opendefocus_bokeh.nk
key_decisions:
  - Used push 0 for null holdout input in camera.nk per Nuke stack convention
  - DeepMerge operation plus for multi-depth additive compositing of discrete depth layers
  - bokeh.nk uses DeepFromImage with set_z true / z 1.0 to convert single bright pixel to deep sample
duration: ""
verification_result: passed
completed_at: 2026-03-28T16:19:55.144Z
blocker_discovered: false
---

# T02: Created test/output/.gitignore, updated test_opendefocus.nk Write path to test/output/, and authored 6 new .nk integration test scripts (multi-depth, holdout, camera, CPU-only, empty, bokeh)

**Created test/output/.gitignore, updated test_opendefocus.nk Write path to test/output/, and authored 6 new .nk integration test scripts (multi-depth, holdout, camera, CPU-only, empty, bokeh)**

## What Happened

Pure file-authoring task. Created test/output/ directory with .gitignore (*.exr, *.tmp). Updated the Write file knob in test_opendefocus.nk from /tmp/ to test/output/. Authored 6 new .nk scripts using Nuke stack-based node graph format: multidepth (3 DeepConstants via DeepMerge operation plus), holdout (alpha=0.5 layer + subject, inputs 2), camera (Camera2 + push 0 null holdout + DeepConstant, inputs 3), cpu (use_gpu false knob), empty (color {0 0 0 0} transparent deep layer), and bokeh (Constant→Crop→DeepFromImage→DeepCOpenDefocus with focus_distance 10). All Write nodes use relative test/output/<name>.exr paths.

## Verification

ls verified all 7 .nk files exist (exit 0). grep confirmed test/output/ path in updated test_opendefocus.nk (exit 0). cat test/output/.gitignore confirmed *.exr and *.tmp entries (exit 0). sh scripts/verify-s01-syntax.sh re-run as regression — all C++ syntax checks and .nk existence check still pass (exit 0, 618ms).

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `ls test/test_opendefocus.nk test/test_opendefocus_multidepth.nk test/test_opendefocus_holdout.nk test/test_opendefocus_camera.nk test/test_opendefocus_cpu.nk test/test_opendefocus_empty.nk test/test_opendefocus_bokeh.nk` | 0 | ✅ pass | 12ms |
| 2 | `grep 'test/output/' test/test_opendefocus.nk` | 0 | ✅ pass | 5ms |
| 3 | `cat test/output/.gitignore` | 0 | ✅ pass | 3ms |
| 4 | `sh scripts/verify-s01-syntax.sh` | 0 | ✅ pass | 618ms |


## Deviations

None.

## Known Issues

None.

## Files Created/Modified

- `test/output/.gitignore`
- `test/test_opendefocus.nk`
- `test/test_opendefocus_multidepth.nk`
- `test/test_opendefocus_holdout.nk`
- `test/test_opendefocus_camera.nk`
- `test/test_opendefocus_cpu.nk`
- `test/test_opendefocus_empty.nk`
- `test/test_opendefocus_bokeh.nk`


## Deviations
None.

## Known Issues
None.
