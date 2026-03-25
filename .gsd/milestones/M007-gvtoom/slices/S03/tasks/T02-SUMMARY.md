---
id: T02
parent: S03
milestone: M007-gvtoom
provides:
  - test/test_ray.nk Nuke test script exercising DeepCDefocusPORay at 128×72 with Angenieux 55mm lens
key_files:
  - test/test_ray.nk
key_decisions:
  - Cloned test_thin.nk structure exactly, adding only aperture_file knob and swapping node class/names
patterns_established:
  - Test .nk files follow identical DAG structure (Reformat→Grid→Dilate→Merge→DeepFromImage→DefocusNode→Write) differing only in node class, knobs, and output filename
observability_surfaces:
  - test_ray.nk exercises full pipeline — runtime failures surface via Nuke error() on lens file load or all-black output on missing poly
duration: 5m
verification_result: passed
completed_at: 2026-03-24
blocker_discovered: false
---

# T02: Create test/test_ray.nk and run verification contracts

**Created test/test_ray.nk from test_thin.nk template with DeepCDefocusPORay node, aperture_file knob, and verified all 12 S03 structural contracts pass.**

## What Happened

Cloned `test/test_thin.nk` to `test/test_ray.nk` with these changes:
- Node class `DeepCDefocusPOThin` → `DeepCDefocusPORay`
- Added `aperture_file` knob pointing to Angenieux 55mm `aperture.fit`
- Write target `./test_thin.exr` → `./test_ray.exr`
- Node name `DeepCDefocusPOThin1` → `DeepCDefocusPORay1`
- Root name path updated to `test_ray.nk`
- write_info comment updated to reference `test_ray.exr`
- Viewer NDI sender name updated to `test_ray`

Also added the missing Observability / Diagnostics section to S03-PLAN.md and marked both T01 and T02 done.

## Verification

Ran all 12 structural verification contracts from the S03 slice plan. All pass — 8 renderStripe content checks + syntax check + 3 test script checks + 1 exr filename check.

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `grep -q '_aperture_sys' src/DeepCDefocusPORay.cpp` | 0 | ✅ pass | <1s |
| 2 | `grep -q 'sphereToCs' src/DeepCDefocusPORay.cpp` | 0 | ✅ pass | <1s |
| 3 | `test $(grep -c '_max_degree' src/DeepCDefocusPORay.cpp) -ge 2` | 0 | ✅ pass | <1s |
| 4 | `grep -q 'transmittance_at' src/DeepCDefocusPORay.cpp` | 0 | ✅ pass | <1s |
| 5 | `grep -q 'halton' src/DeepCDefocusPORay.cpp` | 0 | ✅ pass | <1s |
| 6 | `grep -q '0.45f' src/DeepCDefocusPORay.cpp` | 0 | ✅ pass | <1s |
| 7 | `grep -qE 'ox_land\|oy_land' src/DeepCDefocusPORay.cpp` | 0 | ✅ pass | <1s |
| 8 | `bash scripts/verify-s01-syntax.sh` | 0 | ✅ pass | 3s |
| 9 | `test -f test/test_ray.nk` | 0 | ✅ pass | <1s |
| 10 | `grep -q 'DeepCDefocusPORay' test/test_ray.nk` | 0 | ✅ pass | <1s |
| 11 | `grep -q 'aperture_file' test/test_ray.nk` | 0 | ✅ pass | <1s |
| 12 | `grep -q 'test_ray.exr' test/test_ray.nk` | 0 | ✅ pass | <1s |

## Diagnostics

- **Runtime testing:** `nuke -x test/test_ray.nk` exercises the full gather pipeline. Non-zero exit or all-black output indicates lens file or poly loading issue.
- **Structural re-check:** Re-run the 12 grep contracts above to confirm no regression.

## Deviations

- S03-PLAN.md was missing its Observability / Diagnostics section despite T01 summary claiming it was added. Added it in this task.

## Known Issues

- Runtime contracts (actual Nuke render + non-black pixel count) require docker build and are deferred to CI/assembly slice.

## Files Created/Modified

- `test/test_ray.nk` — Nuke test script exercising DeepCDefocusPORay at 128×72 with Angenieux 55mm lens files
- `.gsd/milestones/M007-gvtoom/slices/S03/S03-PLAN.md` — Added Observability / Diagnostics section, marked T01 and T02 done
