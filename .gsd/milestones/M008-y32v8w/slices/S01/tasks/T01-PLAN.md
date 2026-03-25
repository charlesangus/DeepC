---
estimated_steps: 5
estimated_files: 8
skills_used: []
---

# T01: Port M007 split files, apply _validate format fix, and add lens constant knobs

**Slice:** S01 — Path-trace infrastructure + _validate format fix
**Milestone:** M008-y32v8w

## Description

The worktree starts from the M006 baseline (commit `a9fe480`) which has a single `src/DeepCDefocusPO.cpp`. The completed M007 split into Thin and Ray variants exists on commit `89de100` but was never merged here. This task ports all M007 files into the worktree, applies the `_validate` format fix to both nodes, adds 5 new lens constant knobs to Ray, and creates the test directory.

## Steps

1. **Extract M007 files from git history.** Use `git show 89de100:src/<file>` to extract:
   - `src/DeepCDefocusPORay.cpp` (660 lines)
   - `src/DeepCDefocusPOThin.cpp` (529 lines)
   - `src/poly.h` (193 lines — has `max_degree` parameter already)
   - `src/CMakeLists.txt` (updated with Thin + Ray targets)
   - `test/test_thin.nk` (140 lines)
   - `test/test_ray.nk` (144 lines)

   Commands:
   ```bash
   git show 89de100:src/DeepCDefocusPORay.cpp > src/DeepCDefocusPORay.cpp
   git show 89de100:src/DeepCDefocusPOThin.cpp > src/DeepCDefocusPOThin.cpp
   git show 89de100:src/poly.h > src/poly.h
   git show 89de100:src/CMakeLists.txt > src/CMakeLists.txt
   mkdir -p test
   git show 89de100:test/test_thin.nk > test/test_thin.nk
   git show 89de100:test/test_ray.nk > test/test_ray.nk
   ```

2. **Delete old `src/DeepCDefocusPO.cpp`.**
   ```bash
   rm src/DeepCDefocusPO.cpp
   ```

3. **Apply `_validate` format fix to both Thin and Ray.** In each file's `_validate` method, find the block:
   ```cpp
   info_.set(di.box());
   info_.full_size_format(*di.fullSizeFormat());
   ```
   Insert `info_.format(*di.format());` between those two lines. The M007 code at line ~293 of Ray has:
   ```cpp
   info_.full_size_format(*di.fullSizeFormat());
   ```
   but is MISSING `info_.format(...)`. Add it. Same pattern in Thin.

   The fix ensures PlanarIop allocates output at the correct Deep input resolution instead of a default/stale format.

4. **Add 5 new lens constant Float_knob members to DeepCDefocusPORay.cpp.** These are additional knobs beyond what M007 had, needed by the S02 path-trace engine:

   **Member variables** — add in the private section alongside existing geometry knobs:
   ```cpp
   float _sensor_width;          // 36.0f (full-frame 35mm)
   float _back_focal_length;     // 30.829003f (Angenieux 55mm)
   float _outer_pupil_radius;    // 29.865562f
   float _inner_pupil_radius;    // 19.357308f
   float _aperture_pos;          // 35.445997f
   ```

   **Constructor defaults** — in the constructor initializer list or body:
   ```cpp
   _sensor_width(36.0f),
   _back_focal_length(30.829003f),
   _outer_pupil_radius(29.865562f),
   _inner_pupil_radius(19.357308f),
   _aperture_pos(35.445997f),
   ```

   **Knob declarations** — in `knobs()`, inside or after the "Lens geometry" `BeginClosedGroup`:
   ```cpp
   Float_knob(f, &_sensor_width, "sensor_width", "sensor width (mm)");
     SetRange(f, 1.0f, 100.0f);
     Tooltip(f, "Sensor width in mm. 36mm for full-frame 35mm.");
   Float_knob(f, &_back_focal_length, "back_focal_length", "back focal length (mm)");
     SetRange(f, 0.0f, 200.0f);
     Tooltip(f, "Back focal length from lentil lens_constants.h.");
   Float_knob(f, &_outer_pupil_radius, "outer_pupil_radius", "outer pupil radius (mm)");
     SetRange(f, 0.1f, 100.0f);
     Tooltip(f, "Outer (exit) pupil radius in mm.");
   Float_knob(f, &_inner_pupil_radius, "inner_pupil_radius", "inner pupil radius (mm)");
     SetRange(f, 0.1f, 100.0f);
     Tooltip(f, "Inner (entrance) pupil radius in mm.");
   Float_knob(f, &_aperture_pos, "aperture_pos", "aperture pos (mm)");
     SetRange(f, 0.0f, 200.0f);
     Tooltip(f, "Aperture position along optical axis from lentil lens_constants.h.");
   ```

5. **Verify the file state.**
   ```bash
   test -f src/DeepCDefocusPORay.cpp && echo "Ray exists"
   test -f src/DeepCDefocusPOThin.cpp && echo "Thin exists"
   ! test -f src/DeepCDefocusPO.cpp && echo "Old PO deleted"
   test -f test/test_thin.nk && test -f test/test_ray.nk && echo "Test nk files exist"
   grep -q 'info_\.format(' src/DeepCDefocusPORay.cpp && echo "Ray format fix present"
   grep -q 'info_\.format(' src/DeepCDefocusPOThin.cpp && echo "Thin format fix present"
   grep -q '_sensor_width' src/DeepCDefocusPORay.cpp && echo "sensor_width knob present"
   grep -q '_back_focal_length' src/DeepCDefocusPORay.cpp && echo "back_focal_length knob present"
   grep -q '_outer_pupil_radius' src/DeepCDefocusPORay.cpp && echo "outer_pupil_radius knob present"
   grep -q '_inner_pupil_radius' src/DeepCDefocusPORay.cpp && echo "inner_pupil_radius knob present"
   grep -q '_aperture_pos' src/DeepCDefocusPORay.cpp && echo "aperture_pos knob present"
   grep -q 'max_degree' src/poly.h && echo "poly.h max_degree present"
   grep -q 'DeepCDefocusPOThin' src/CMakeLists.txt && echo "CMake has Thin"
   grep -q 'DeepCDefocusPORay' src/CMakeLists.txt && echo "CMake has Ray"
   ```

## Must-Haves

- [ ] `src/DeepCDefocusPORay.cpp` exists with M007 content + _validate format fix + 5 new lens knobs
- [ ] `src/DeepCDefocusPOThin.cpp` exists with M007 content + _validate format fix
- [ ] `src/DeepCDefocusPO.cpp` deleted
- [ ] `src/poly.h` has `max_degree` parameter on `poly_system_evaluate`
- [ ] `src/CMakeLists.txt` targets Thin + Ray (not old DeepCDefocusPO)
- [ ] `test/test_thin.nk` and `test/test_ray.nk` exist
- [ ] `info_.format(*di.format())` present in both Thin and Ray `_validate`
- [ ] All 5 new Float_knob members declared, initialized, and exposed in knobs()

## Verification

- `grep -q 'info_\.format(' src/DeepCDefocusPORay.cpp && grep -q 'info_\.format(' src/DeepCDefocusPOThin.cpp`
- `grep -q '_sensor_width' src/DeepCDefocusPORay.cpp && grep -q '_aperture_pos' src/DeepCDefocusPORay.cpp`
- `! test -f src/DeepCDefocusPO.cpp`
- `test -f test/test_thin.nk && test -f test/test_ray.nk`

## Inputs

- `src/DeepCDefocusPO.cpp` — M006 baseline file to be replaced
- `src/poly.h` — existing M006 version to be replaced with M007 version
- `src/CMakeLists.txt` — existing M006 version to be replaced with M007 version
- `src/deepc_po_math.h` — not modified in this task, but Ray includes it

## Expected Output

- `src/DeepCDefocusPORay.cpp` — M007 port with _validate fix and 5 new lens knobs
- `src/DeepCDefocusPOThin.cpp` — M007 port with _validate fix
- `src/poly.h` — M007 version with max_degree parameter
- `src/CMakeLists.txt` — updated to target Thin + Ray
- `test/test_thin.nk` — Nuke test script from M007
- `test/test_ray.nk` — Nuke test script from M007

## Observability Impact

- **Signals changed:** The `_validate` format fix adds `info_.format(*di.format())` ensuring PlanarIop output resolution matches the Deep input. The 5 new lens constant knobs expose sensor/pupil/aperture parameters needed by S02's path-trace engine.
- **Inspection:** `grep -q 'info_\.format(' src/DeepCDefocusPO{Thin,Ray}.cpp` confirms the format fix. `grep '_sensor_width\|_aperture_pos' src/DeepCDefocusPORay.cpp` confirms lens knobs.
- **Failure visibility:** Missing format fix causes wrong output resolution at runtime. Missing lens knobs cause S02 compilation errors when the path-trace engine references them.
