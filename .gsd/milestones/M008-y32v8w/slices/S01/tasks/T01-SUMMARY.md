---
id: T01
parent: S01
milestone: M008-y32v8w
provides:
  - DeepCDefocusPORay.cpp with M007 content, _validate format fix, and 5 new lens constant knobs
  - DeepCDefocusPOThin.cpp with M007 content and _validate format fix
  - Updated poly.h with max_degree parameter
  - Updated CMakeLists.txt targeting Thin + Ray
  - Test .nk files for both nodes
key_files:
  - src/DeepCDefocusPORay.cpp
  - src/DeepCDefocusPOThin.cpp
  - src/poly.h
  - src/CMakeLists.txt
  - test/test_thin.nk
  - test/test_ray.nk
key_decisions:
  - Lens constant knobs placed inside Lens geometry BeginClosedGroup alongside existing curvature/housing knobs
patterns_established:
  - _validate format fix pattern: info_.format(*di.format()) inserted between info_.set(di.box()) and info_.full_size_format()
observability_surfaces:
  - grep 'info_.format(' src/DeepCDefocusPO{Thin,Ray}.cpp confirms format fix presence
  - grep '_sensor_width|_aperture_pos' src/DeepCDefocusPORay.cpp confirms lens knobs
duration: 10m
verification_result: passed
completed_at: 2026-03-25
blocker_discovered: false
---

# T01: Port M007 split files, apply _validate format fix, and add lens constant knobs

**Ported M007 Thin/Ray split from commit 89de100, applied `info_.format()` fix to both nodes' `_validate`, and added 5 Angenieux 55mm lens constant knobs to Ray.**

## What Happened

Extracted 6 files from M007 commit `89de100` via `git show`: DeepCDefocusPORay.cpp, DeepCDefocusPOThin.cpp, poly.h, CMakeLists.txt, test/test_thin.nk, and test/test_ray.nk. Deleted the old monolithic DeepCDefocusPO.cpp. Applied the `_validate` format fix (`info_.format(*di.format())`) to both Thin and Ray — this ensures PlanarIop allocates output at the correct Deep input resolution. Added 5 new Float_knob members to Ray for the S02 path-trace engine: `_sensor_width` (36.0), `_back_focal_length` (30.829003), `_outer_pupil_radius` (29.865562), `_inner_pupil_radius` (19.357308), `_aperture_pos` (35.445997) — all with Angenieux 55mm defaults, proper SetRange/Tooltip, and constructor initialization. Added Observability sections to S01-PLAN.md and T01-PLAN.md per pre-flight requirements.

## Verification

All 4 task-level verification commands pass. All 14 structural grep checks pass (file existence, format fix in both files, all 5 knobs, max_degree in poly.h, CMake targets). The slice-level `verify-s01-syntax.sh` still references old DeepCDefocusPO.cpp and will be updated in T03.

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `grep -q 'info_\.format(' src/DeepCDefocusPORay.cpp && grep -q 'info_\.format(' src/DeepCDefocusPOThin.cpp` | 0 | ✅ pass | <1s |
| 2 | `grep -q '_sensor_width' src/DeepCDefocusPORay.cpp && grep -q '_aperture_pos' src/DeepCDefocusPORay.cpp` | 0 | ✅ pass | <1s |
| 3 | `! test -f src/DeepCDefocusPO.cpp` | 0 | ✅ pass | <1s |
| 4 | `test -f test/test_thin.nk && test -f test/test_ray.nk` | 0 | ✅ pass | <1s |
| 5 | `bash scripts/verify-s01-syntax.sh` | 1 | ⏳ partial (T03 updates script) | 5s |

## Diagnostics

- Confirm format fix: `grep -n 'info_\.format(' src/DeepCDefocusPORay.cpp src/DeepCDefocusPOThin.cpp`
- Confirm lens knobs: `grep -n '_sensor_width\|_back_focal_length\|_outer_pupil_radius\|_inner_pupil_radius\|_aperture_pos' src/DeepCDefocusPORay.cpp`
- Confirm file structure: `ls src/DeepCDefocusPO{Thin,Ray}.cpp src/poly.h test/test_{thin,ray}.nk`

## Deviations

None — all steps executed as planned.

## Known Issues

- `scripts/verify-s01-syntax.sh` still references old `DeepCDefocusPO.cpp` — will be updated in T03.

## Files Created/Modified

- `src/DeepCDefocusPORay.cpp` — M007 port with _validate format fix and 5 new lens constant knobs
- `src/DeepCDefocusPOThin.cpp` — M007 port with _validate format fix
- `src/poly.h` — M007 version with max_degree parameter (extracted from 89de100)
- `src/CMakeLists.txt` — Updated to target Thin + Ray instead of old DeepCDefocusPO
- `test/test_thin.nk` — Nuke test script for Thin node (extracted from 89de100)
- `test/test_ray.nk` — Nuke test script for Ray node (extracted from 89de100)
- `src/DeepCDefocusPO.cpp` — Deleted (replaced by Thin/Ray split)
- `.gsd/milestones/M008-y32v8w/slices/S01/S01-PLAN.md` — Added Observability/Diagnostics section
- `.gsd/milestones/M008-y32v8w/slices/S01/tasks/T01-PLAN.md` — Added Observability Impact section
