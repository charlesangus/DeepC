# S01 UAT: Shared Infrastructure, max_degree Knob, Two-Plugin Scaffold

**Slice:** S01  
**Milestone:** M007-gvtoom  
**Date:** 2026-03-24  

## Preconditions

- Working directory: `/home/latuser/git/DeepC/.gsd/worktrees/M007-gvtoom`
- `g++` with `-std=c++17` available (Ubuntu or macOS dev environment)
- No Nuke installation required for any check in this slice (all checks are syntax/grep/file-system)

---

## Test Cases

### TC-01: Syntax check gate passes for both new plugins

**What it proves:** Both `DeepCDefocusPOThin.cpp` and `DeepCDefocusPORay.cpp` compile without errors using the mock DDImage headers. This is the primary S01 gate.

**Steps:**

1. `bash scripts/verify-s01-syntax.sh`

**Expected output:**
```
Running syntax check: g++ ... src/DeepCBlur.cpp
Syntax check passed: DeepCBlur.cpp
Running syntax check: g++ ... src/DeepCDepthBlur.cpp
Syntax check passed: DeepCDepthBlur.cpp
Running syntax check: g++ ... src/DeepCDefocusPOThin.cpp
Syntax check passed: DeepCDefocusPOThin.cpp
Running syntax check: g++ ... src/DeepCDefocusPORay.cpp
Syntax check passed: DeepCDefocusPORay.cpp
Checking S05 contracts...
S05 contracts: all pass.
All syntax checks passed.
```

**Expected exit code:** 0

**Fail criteria:** Any "error:" line from g++, or "FAILED" from a contract check.

---

### TC-02: Both new plugins present in CMake PLUGINS and FILTER_NODES; old plugin absent

**What it proves:** The build system will produce `DeepCDefocusPOThin.so` and `DeepCDefocusPORay.so` and will not attempt to build the deleted `DeepCDefocusPO.so`.

**Steps:**

1. `grep -c 'DeepCDefocusPOThin' src/CMakeLists.txt`  
   Expected: `2`

2. `grep -c 'DeepCDefocusPORay' src/CMakeLists.txt`  
   Expected: `2`

3. `grep -c 'DeepCDefocusPO[^TR]' src/CMakeLists.txt`  
   Expected: `0`

4. `grep 'DeepCDefocusPOThin\|DeepCDefocusPORay' src/CMakeLists.txt`  
   Expected: lines containing both Thin and Ray in `PLUGINS` list and `FILTER_NODES` list (verify visually that both lists are updated).

**Fail criteria:** Any count differs from expected, or old `DeepCDefocusPO` appears.

---

### TC-03: max_degree parameter present in poly.h with correct early-exit logic

**What it proves:** The shared evaluation primitive truncates to `max_degree` using `break` (correct for ascending-degree-sorted terms).

**Steps:**

1. `grep -q 'max_degree' src/poly.h && echo OK`  
   Expected: `OK`

2. `grep -q 'max_degree.*=.*-1' src/poly.h && echo OK`  
   Expected: `OK` (default -1 means all terms)

3. `grep -q 'deg > max_degree' src/poly.h && echo OK`  
   Expected: `OK`

4. Inspect the early-exit block: `grep -A3 'deg > max_degree' src/poly.h`  
   Expected: `break` on the following line (not `continue`)

**Fail criteria:** Any check fails, or `continue` appears instead of `break`.

---

### TC-04: max_degree knob wired in both plugin scaffolds

**What it proves:** Artists will see the `max_degree` quality/speed knob in Nuke's properties panel for both nodes.

**Steps:**

1. `grep -q 'max_degree' src/DeepCDefocusPOThin.cpp && echo OK`  
   Expected: `OK`

2. `grep -q 'max_degree' src/DeepCDefocusPORay.cpp && echo OK`  
   Expected: `OK`

3. `grep -c '_max_degree' src/DeepCDefocusPOThin.cpp`  
   Expected: ≥3 (member declaration, constructor initialisation, Int_knob call)

4. `grep 'Int_knob.*max_degree\|max_degree.*Int_knob' src/DeepCDefocusPOThin.cpp`  
   Expected: a line wiring `_max_degree` to `Int_knob`

**Fail criteria:** Any check fails.

---

### TC-05: sphereToCs present in deepc_po_math.h with safe fallback

**What it proves:** S03 can call `sphereToCs` to convert polynomial output to a 3D ray direction without NaN risk.

**Steps:**

1. `grep -q 'sphereToCs' src/deepc_po_math.h && echo OK`  
   Expected: `OK`

2. `grep -A15 'sphereToCs' src/deepc_po_math.h | grep -q 'disc < 0\|sqrt' && echo OK`  
   Expected: `OK` (fallback for out-of-sphere inputs)

3. `grep -A15 'sphereToCs' src/deepc_po_math.h | grep -q 'return\|x, y, 0' && echo OK`  
   Expected: `OK` (degenerate (x,y,0) return for out-of-sphere)

**Fail criteria:** Function absent or no safe fallback present.

---

### TC-06: Stub renderStripe zeros all output channels in both plugins

**What it proves:** Before S02/S03 implement the real engines, both nodes produce black (zero) output rather than uninitialised memory or a crash.

**Steps:**

1. `grep -q 'writableAt.*= 0' src/DeepCDefocusPOThin.cpp && echo OK`  
   Expected: `OK`

2. `grep -q 'writableAt.*= 0' src/DeepCDefocusPORay.cpp && echo OK`  
   Expected: `OK`

3. `grep -c 'writableAt.*= 0' src/DeepCDefocusPOThin.cpp`  
   Expected: ≥1 (at least the stub zero assignment)

**Fail criteria:** Any check fails.

---

### TC-07: _validate error paths present for both plugins

**What it proves:** Artists get a clear error message in Nuke's script editor when a .fit file path is wrong, rather than a crash or silent failure.

**Steps:**

1. `grep -q 'error.*Cannot open lens file' src/DeepCDefocusPOThin.cpp && echo OK`  
   Expected: `OK`

2. `grep -q 'error.*Cannot open lens file' src/DeepCDefocusPORay.cpp && echo OK`  
   Expected: `OK`

3. `grep -q 'error.*Cannot open aperture file' src/DeepCDefocusPORay.cpp && echo OK`  
   Expected: `OK`

**Fail criteria:** Any check fails.

---

### TC-08: Old DeepCDefocusPO.cpp is deleted

**What it proves:** The superseded single-plugin source is gone and cannot accidentally be compiled.

**Steps:**

1. `test ! -f src/DeepCDefocusPO.cpp && echo "deleted: OK"`  
   Expected: `deleted: OK`

2. `ls src/DeepCDefocusPO*.cpp 2>/dev/null`  
   Expected: only `DeepCDefocusPOThin.cpp` and `DeepCDefocusPORay.cpp` listed; nothing else.

**Fail criteria:** `src/DeepCDefocusPO.cpp` still exists.

---

### TC-09: Ray variant has aperture_file knob and lens geometry group

**What it proves:** DeepCDefocusPORay exposes the additional inputs S03 needs — aperture polynomial file and Angenieux 55mm lens geometry constants.

**Steps:**

1. `grep -q 'aperture_file' src/DeepCDefocusPORay.cpp && echo OK`  
   Expected: `OK`

2. `grep -q 'outer_pupil_curvature_radius' src/DeepCDefocusPORay.cpp && echo OK`  
   Expected: `OK`

3. `grep -q '90.77' src/DeepCDefocusPORay.cpp && echo OK`  
   Expected: `OK` (Angenieux 55mm default for outer pupil curvature radius)

4. `grep -q 'BeginClosedGroup\|EndGroup' src/DeepCDefocusPORay.cpp && echo OK`  
   Expected: `OK` (lens geometry group in knobs panel)

5. `grep -c 'poly_system_t' src/DeepCDefocusPORay.cpp`  
   Expected: ≥2 (one for lens poly, one for aperture poly)

**Fail criteria:** Any check fails.

---

### TC-10: Holdout and CA wavelengths preserved in both scaffolds

**What it proves:** R035 — the M006 validated features (holdout, CA, Halton) are structurally present in both new plugins.

**Steps:**

1. `grep -q 'holdout' src/DeepCDefocusPOThin.cpp && echo OK`  
   Expected: `OK`

2. `grep -q 'holdout' src/DeepCDefocusPORay.cpp && echo OK`  
   Expected: `OK`

3. `grep -q '0\.45\|WL_B' src/DeepCDefocusPOThin.cpp && echo OK`  
   Expected: `OK` (CA blue wavelength present)

4. `grep -q '0\.65\|WL_R' src/DeepCDefocusPOThin.cpp && echo OK`  
   Expected: `OK` (CA red wavelength present)

5. `grep -q 'halton\|Halton' src/DeepCDefocusPOThin.cpp && echo OK`  
   Expected: `OK`

**Fail criteria:** Any check fails.

---

### TC-11: Both plugins register under "Deep/" menu category

**What it proves:** R036 — artists find both nodes under the Filter > Deep category in Nuke's node menu.

**Steps:**

1. `grep -q '"Deep/DeepCDefocusPOThin"' src/DeepCDefocusPOThin.cpp && echo OK`  
   Expected: `OK`

2. `grep -q '"Deep/DeepCDefocusPORay"' src/DeepCDefocusPORay.cpp && echo OK`  
   Expected: `OK`

**Fail criteria:** Either registration string absent.

---

## Edge Cases

- **TC-03 break vs continue**: Incorrect use of `continue` instead of `break` in the max_degree early-exit would cause correct syntax-check pass but silently evaluate all terms regardless of max_degree. The grep for `break` after the `deg > max_degree` condition catches this.
- **TC-01 mock header staleness**: If new DDImage types are introduced in S02/S03, the mock headers in `scripts/verify-s01-syntax.sh` must be extended. A false-positive syntax error in an otherwise correct file is the symptom.
- **TC-08 residual references**: Even though `DeepCDefocusPO.cpp` is deleted, CMakeLists.txt or verify-s01-syntax.sh might still reference it. TC-02 step 3 and TC-01 together cover this.
- **TC-09 dual poly_system_t**: Ray variant has two `poly_system_t` members. If only one is initialised in `_validate` or destroyed in `_close`, S03 will encounter a use-after-free or missing-load bug. The grep count check in TC-09 step 5 flags this early.
