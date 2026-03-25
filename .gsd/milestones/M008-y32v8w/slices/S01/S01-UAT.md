# S01 UAT: Path-trace infrastructure + _validate format fix

**Proof level:** Contract (syntax compilation + structural grep)  
**Runtime required:** No — S01 is infrastructure only; runtime proof is S02's gate  
**Prerequisites:** Working directory is `/home/latuser/git/DeepC/.gsd/worktrees/M008-y32v8w`. `g++` available with C++17 support.

---

## Test Suite

### TC-01: Full S01 verify script (gate test)

**What it proves:** All S01 deliverables at once — syntax compilation of both PO nodes and all 13 structural contracts.

**Preconditions:** None beyond working directory.

**Steps:**
1. `cd /home/latuser/git/DeepC/.gsd/worktrees/M008-y32v8w`
2. `bash scripts/verify-s01-syntax.sh`
3. Observe output lines

**Expected outcome:**
- Exit code 0
- All 4 source files print "Syntax check passed: <filename>"
- "--- S01 contracts ---" section follows, with 13 PASS lines (no FAIL lines)
- Final line: "All S01 contracts passed." then "All syntax checks passed."

**Edge case — diagnose which contract failed:**
```
bash scripts/verify-s01-syntax.sh 2>&1 | grep -c FAIL
# Must return 0
```

---

### TC-02: _validate format fix in both nodes

**What it proves:** R045 — both PO nodes propagate Deep input resolution to PlanarIop output.

**Steps:**
1. `grep -n 'info_\.format(' src/DeepCDefocusPOThin.cpp`
2. `grep -n 'info_\.format(' src/DeepCDefocusPORay.cpp`

**Expected outcome:**
- Each command prints at least one line containing `info_.format(*di.format())` inside the `_validate` method body (after the `info_.set(di.box())` call).
- Exit code 0 for both.

**Edge case — verify ordering:**
```
grep -A3 'info_\.set(di\.box())' src/DeepCDefocusPORay.cpp | grep 'info_\.format'
# Must print the format line — confirming it comes after info_.set(di.box())
```

---

### TC-03: Five new lens constant knobs on Ray

**What it proves:** R043 — all 5 Angenieux 55mm lens constants are exposed as Float_knobs.

**Steps:**
1. `grep -c '_sensor_width\|_back_focal_length\|_outer_pupil_radius\|_inner_pupil_radius\|_aperture_pos' src/DeepCDefocusPORay.cpp`

**Expected outcome:** Returns ≥ 5 (one declaration + knob wiring per member minimum; likely more due to constructor init and SetRange calls).

2. `grep -n '_sensor_width' src/DeepCDefocusPORay.cpp`

**Expected outcome:** At least 3 lines — class member declaration, constructor initialization (36.0f), and Float_knob call in `knobs()`.

**Edge case — Angenieux 55mm default values:**
```
grep '30.829\|29.865\|19.357\|35.445' src/DeepCDefocusPORay.cpp
# Must return 4 lines — the 4 non-sensor-width Angenieux defaults
```

---

### TC-04: Three path-trace math functions in deepc_po_math.h

**What it proves:** R039 (pt_sample_aperture), R040 (sphereToCs_full), R042 (logarithmic_focus_search).

**Steps:**
1. `grep -n 'inline.*sphereToCs_full\|inline.*pt_sample_aperture\|inline.*logarithmic_focus_search' src/deepc_po_math.h`

**Expected outcome:** 3 lines — one per function definition.

2. Verify signatures individually:
```
grep -A2 'inline.*pt_sample_aperture' src/deepc_po_math.h
# Must show: sensor_x, sensor_y, &dx, &dy, target aperture params, lambda, sensor_shift, poly_sys, max_degree

grep -A2 'inline.*sphereToCs_full' src/deepc_po_math.h
# Must show: x, y, dx, dy, center, R, output params &ox &oy &oz &odx &ody &odz

grep -A2 'inline.*logarithmic_focus_search' src/deepc_po_math.h
# Must show: focal_distance_mm, outer_pupil_radius, back_focal_length, aperture_housing_radius, poly_sys
```

**Edge case — pt_sample_aperture Newton parameters:**
```
grep '0\.72\|1e-4\|5.*iter\|iter.*5' src/deepc_po_math.h
# Must confirm: 0.72 damping factor and 1e-4 tolerance present
```

**Edge case — logarithmic_focus_search step count:**
```
grep '20001\|20000' src/deepc_po_math.h
# Must confirm search over 20001 steps (or equivalent constant)
```

---

### TC-05: sphereToCs direction-only function retained (backward compat)

**What it proves:** The M007 `sphereToCs` (returns direction only) is still present for Ray's existing renderStripe.

**Steps:**
1. `grep -n 'inline.*sphereToCs[^_]' src/deepc_po_math.h`

**Expected outcome:** At least one line — the original direction-only sphereToCs definition.

2. `grep -c 'sphereToCs' src/DeepCDefocusPORay.cpp`

**Expected outcome:** ≥ 1 — Ray's renderStripe calls it.

---

### TC-06: File structure contracts

**What it proves:** Old monolithic DeepCDefocusPO.cpp is gone; split files and test scripts exist.

**Steps:**
```
test -f src/DeepCDefocusPORay.cpp  && echo "PASS Ray" || echo "FAIL Ray"
test -f src/DeepCDefocusPOThin.cpp && echo "PASS Thin" || echo "FAIL Thin"
! test -f src/DeepCDefocusPO.cpp   && echo "PASS old gone" || echo "FAIL old still present"
test -f test/test_thin.nk          && echo "PASS test_thin" || echo "FAIL test_thin"
test -f test/test_ray.nk           && echo "PASS test_ray" || echo "FAIL test_ray"
```

**Expected outcome:** All 5 lines print PASS.

---

### TC-07: poly.h max_degree parameter

**What it proves:** R032 — `poly_system_evaluate` has max_degree early-exit for quality/speed tradeoff.

**Steps:**
```
grep -n 'max_degree' src/poly.h
```

**Expected outcome:** At least 1 line — the parameter declaration/default and/or early-exit logic.

---

### TC-08: CMakeLists.txt targets both split plugins

**What it proves:** R036 — both Thin and Ray are registered for build and Nuke menu.

**Steps:**
```
grep -c 'DeepCDefocusPOThin' src/CMakeLists.txt
grep -c 'DeepCDefocusPORay' src/CMakeLists.txt
grep -c 'DeepCDefocusPO[^TR]' src/CMakeLists.txt
```

**Expected outcome:**
- First command: ≥ 2 (PLUGINS list + FILTER_NODES list)
- Second command: ≥ 2
- Third command: 0 (old monolithic entry gone)

---

### TC-09: Syntax compilation in isolation (without script)

**What it proves:** Both PO source files compile cleanly with C++17 outside the verify script (no hidden dependency on script-provided flags).

**Steps:**
```
g++ -std=c++17 -fsyntax-only \
  -I/tmp/mock_headers \
  -I src \
  src/DeepCDefocusPOThin.cpp 2>&1 | head -20
```
(Create minimal mock headers if /tmp/mock_headers doesn't exist — or just run via verify script which sets them up.)

**Expected outcome:** Zero error lines (warnings for unused variables acceptable). Exit code 0.

---

### TC-10: deepc_po_math.h compiles with poly.h

**What it proves:** The new math functions correctly call poly_system_evaluate with the 5-arg signature.

**Steps:**
```
echo '#include "poly.h"
#include "deepc_po_math.h"
int main() { return 0; }' | g++ -std=c++17 -fsyntax-only -I src -x c++ - 2>&1
```

**Expected outcome:** Zero error lines. Exit code 0. (This confirms the forward declaration in deepc_po_math.h matches the actual signature in poly.h.)

---

## Summary Table

| Test | Checks | Proof Level |
|------|--------|-------------|
| TC-01 | All S01 deliverables (gate) | Structural |
| TC-02 | _validate format fix in both nodes | Structural |
| TC-03 | 5 lens constant knobs on Ray | Structural |
| TC-04 | 3 path-trace math functions | Structural |
| TC-05 | sphereToCs backward compat | Structural |
| TC-06 | File structure (old gone, new exist) | Structural |
| TC-07 | poly.h max_degree | Structural |
| TC-08 | CMakeLists.txt Thin+Ray | Structural |
| TC-09 | Thin/Ray syntax in isolation | Compilation |
| TC-10 | deepc_po_math.h + poly.h compile | Compilation |

**All tests must pass before S02 begins. TC-01 is the single authoritative gate.**
