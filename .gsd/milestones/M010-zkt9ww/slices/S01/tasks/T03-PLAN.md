---
estimated_steps: 1
estimated_files: 1
skills_used: []
---

# T03: Extend verify-s01-syntax.sh for all 7 .nk files and run final verification

The current verify-s01-syntax.sh S02 section only checks for test/test_opendefocus.nk. This task extends it to check all 7 .nk files exist, then runs the script as the slice's objective stopping condition. All 7 files must exist from T02 before this task.

## Inputs

- ``scripts/verify-s01-syntax.sh` — existing script; extend S02 section to check 6 additional .nk files`
- ``test/test_opendefocus_multidepth.nk` — must exist (from T02)`
- ``test/test_opendefocus_holdout.nk` — must exist (from T02)`
- ``test/test_opendefocus_camera.nk` — must exist (from T02)`
- ``test/test_opendefocus_cpu.nk` — must exist (from T02)`
- ``test/test_opendefocus_empty.nk` — must exist (from T02)`
- ``test/test_opendefocus_bokeh.nk` — must exist (from T02)`

## Expected Output

- ``scripts/verify-s01-syntax.sh` — S02 section extended with existence checks for all 6 new .nk files: test_opendefocus_multidepth.nk, test_opendefocus_holdout.nk, test_opendefocus_camera.nk, test_opendefocus_cpu.nk, test_opendefocus_empty.nk, test_opendefocus_bokeh.nk`

## Verification

sh scripts/verify-s01-syntax.sh
