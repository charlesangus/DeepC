# S01 UAT: Iop Scaffold, Poly Loader, and Architecture Decision

**Milestone:** M006  
**Slice:** S01  
**Written:** 2026-03-23  
**Tester:** developer / CI engineer  

## Preconditions

1. Working directory is the repo root (`/workspace/.gsd/worktrees/M006` or the cloned repo root).
2. `g++` (≥ C++14) is installed and on `PATH` (required for syntax check).
3. `grep`, `cmake` are available.
4. Docker is available for TC-7 (docker build gate). If Docker is absent, TC-7 must run in CI — all other test cases can run locally.
5. A lentil `.fit` polynomial lens file is available at a known path for TC-6 (runtime load test). If unavailable, TC-6 is deferred to a CI/Nuke environment.

---

## Test Cases

### TC-1: Syntax verifier passes for all three plugin files

**Purpose:** Confirm DeepCDefocusPO.cpp, DeepCBlur.cpp, and DeepCDepthBlur.cpp all compile cleanly under `g++ -fsyntax-only` with the mock DDImage headers.

**Steps:**
1. From the repo root, run: `bash scripts/verify-s01-syntax.sh`
2. Observe output.

**Expected:**
```
Syntax check passed: DeepCBlur.cpp
Syntax check passed: DeepCDepthBlur.cpp
Syntax check passed: DeepCDefocusPO.cpp
All syntax checks passed.
```
Exit code: 0.

**Failure signals:**
- `error:` line printed with `DeepCDefocusPO.cpp:NN:` — indicates a C++ syntax error in the plugin; fix the source file.
- `fatal error: DDImage/SomeHeader.h: No such file or directory` — a new DDImage include was added but no mock stub was created; add the stub to `scripts/verify-s01-syntax.sh`.
- Exit code non-zero but no error message — check that the script has execute permission (`chmod +x scripts/verify-s01-syntax.sh`).

---

### TC-2: All grep contracts pass

**Purpose:** Confirm all required symbols are present in the correct files.

**Steps:** Run each command; all must exit 0.

```bash
grep -q 'poly_system_read' src/DeepCDefocusPO.cpp && echo "PASS: poly_system_read in .cpp"
grep -q 'poly_system_destroy' src/DeepCDefocusPO.cpp && echo "PASS: poly_system_destroy in .cpp"
grep -q 'File_knob' src/DeepCDefocusPO.cpp && echo "PASS: File_knob in .cpp"
grep -q 'renderStripe' src/DeepCDefocusPO.cpp && echo "PASS: renderStripe in .cpp"
grep -q 'lt_newton_trace' src/deepc_po_math.h && echo "PASS: lt_newton_trace in math header"
grep -q 'mat2_inverse' src/deepc_po_math.h && echo "PASS: mat2_inverse in math header"
grep -q 'coc_radius' src/deepc_po_math.h && echo "PASS: coc_radius in math header"
grep -q 'poly_system_read' src/poly.h && echo "PASS: poly_system_read in poly.h"
```

**Expected:** All 8 lines print `PASS: ...`. Any missing symbol indicates an incomplete implementation.

---

### TC-3: CMakeLists.txt has both PLUGINS and FILTER_NODES entries

**Purpose:** Confirm DeepCDefocusPO will be compiled and appear in Nuke's Filter menu.

**Steps:**
```bash
count=$(grep -c 'DeepCDefocusPO' src/CMakeLists.txt)
echo "Count: $count"
grep -n 'DeepCDefocusPO' src/CMakeLists.txt
```

**Expected:**
- Count: 2 (exactly)
- Line 1: `DeepCDefocusPO` inside the `PLUGINS` list
- Line 2: `DeepCDefocusPO` inside the `set(FILTER_NODES ...)` call

**Failure signals:**
- Count 1: one registration missing; check that both PLUGINS and FILTER_NODES lines are present.
- Count 0: plugin was removed from CMakeLists.txt; re-add both entries.
- Count > 2: duplicate entry; remove the extra occurrence.

---

### TC-4: deepc_po_math.h does NOT include poly.h (ODR firewall)

**Purpose:** Confirm the ODR firewall is intact — poly.h's inline function bodies must not appear in more than one TU.

**Steps:**
```bash
grep -n '#include.*poly' src/deepc_po_math.h && echo "FAIL: poly.h included in math header" || echo "PASS: ODR firewall intact"
```

**Expected:** `PASS: ODR firewall intact` (grep exits non-zero = no match = firewall intact).

**Failure signal:** If the grep finds a match, `poly.h` will be compiled into every TU that includes `deepc_po_math.h`, causing linker duplicate-symbol errors in the docker build.

---

### TC-5: PlanarIop, ImagePlane, File_knob stubs are in the verifier script

**Purpose:** Confirm the mock header stubs for the new plugin type are present so the syntax check is valid.

**Steps:**
```bash
grep -q 'PlanarIop' scripts/verify-s01-syntax.sh && echo "PASS: PlanarIop stub present"
grep -q 'ImagePlane' scripts/verify-s01-syntax.sh && echo "PASS: ImagePlane stub present"
grep -q 'File_knob' scripts/verify-s01-syntax.sh && echo "PASS: File_knob stub present"
grep -q 'DeepCDefocusPO.cpp' scripts/verify-s01-syntax.sh && echo "PASS: DeepCDefocusPO.cpp in check loop"
```

**Expected:** All 4 lines print `PASS: ...`.

---

### TC-6: poly_system_read error path is reachable (failure-path diagnostic)

**Purpose:** Confirm the failure branch in `_validate` compiles — the `error()` call on poly load failure will fire in Nuke when the file path is wrong or unreadable.

**Steps:**
```bash
# Confirm poly_system_read is defined in poly.h (error branch compiles)
grep -q 'poly_system_read' src/poly.h && echo "PASS: error path compiles (poly_system_read present)"

# Confirm error() call is present in the .cpp
grep -q 'error("Cannot open lens file' src/DeepCDefocusPO.cpp && echo "PASS: error() call present in _validate"
```

**Expected:** Both lines print `PASS`.

**Runtime check (Nuke environment required):**
1. Load a comp with DeepCDefocusPO.
2. Set `lens file` to `/tmp/does-not-exist.fit`.
3. Force a cook.
4. **Expected:** Node tile turns red; Nuke error bar shows `"Cannot open lens file: /tmp/does-not-exist.fit"`.
5. Set `lens file` to a valid `.fit` binary file.
6. Force a cook.
7. **Expected:** Node tile turns green (no error).

---

### TC-7: Docker build exits 0 and produces DeepCDefocusPO.so (CI gate)

**Purpose:** Confirm the plugin compiles against the real Nuke SDK inside Docker and appears in the release zip.

**Precondition:** Docker daemon running; `docker-build.sh` is present and executable.

**Steps:**
```bash
bash docker-build.sh --linux --versions 16.0
echo "Exit code: $?"
unzip -l release/DeepC-Linux-Nuke16.0.zip | grep DeepCDefocusPO.so
```

**Expected:**
- Exit code: 0
- `unzip -l` output includes a line containing `DeepCDefocusPO.so`

**Failure signals:**
- Compiler error in Docker output: check the error location (filename:line); most likely cause is an API mismatch not caught by the mock-header syntax check.
- `DeepCDefocusPO.so` absent from zip: `DeepCDefocusPO` may not be in the `PLUGINS` list in `CMakeLists.txt` — re-run TC-3 to confirm.
- Docker daemon error: infrastructure issue; confirm Docker is running and the build image is pulled.

---

### TC-8: Node appears in Nuke's Filter menu (Nuke environment required)

**Purpose:** Confirm the `FILTER_NODES` CMake entry correctly places DeepCDefocusPO in the Filter menu category.

**Precondition:** DeepCDefocusPO.so installed in Nuke's plugin path; Nuke running.

**Steps:**
1. Launch Nuke.
2. Open the node creation menu or tab-create dialog.
3. Navigate to **Filter** category.
4. Look for `DeepCDefocusPO`.

**Expected:** `DeepCDefocusPO` is listed in the Filter category.

**Failure signal:** Node absent from Filter but present in Deep or Other — `FILTER_NODES` entry may be missing or misspelled in CMakeLists.txt.

---

### TC-9: renderStripe outputs flat black (S01 regression guard)

**Purpose:** Confirm S01's renderStripe writes zeros — a non-zero output at S01 stage indicates unintended code regression.

**Precondition:** Nuke environment with DeepCDefocusPO.so loaded.

**Steps:**
1. Connect a Deep source to DeepCDefocusPO input 0.
2. Attach a `Viewer` downstream of DeepCDefocusPO.
3. Set a valid `.fit` file in `lens file`.
4. Cook the node.
5. Check pixel values in the Viewer for any non-black pixel.

**Expected:** All output pixels are `(0, 0, 0, 0)`.

**Note:** This test becomes invalid after S02 (scatter math will produce non-zero output). It is a regression guard only for the S01 skeleton state. Remove or update this test case when S02 is integrated.

---

## Edge Cases

### EC-1: Empty lens file path
- Set `lens file` to empty string and cook.
- **Expected:** Node does not crash; `_validate` skips the poly load (the `_poly_file[0] != '\0'` guard prevents calling `poly_system_read` with an empty path); output is flat black.

### EC-2: Corrupt or truncated .fit file
- Create a file with random bytes (`dd if=/dev/urandom of=/tmp/bad.fit bs=64 count=1`) and set it as the lens file.
- **Expected:** Node tile turns red with `"Cannot open lens file: /tmp/bad.fit"`. No crash. `poly_system_destroy` not called (load never succeeded). No memory leak.

### EC-3: Hot-reload after valid lens is loaded
- Load a valid `.fit` file; cook successfully.
- Change the path to a different valid `.fit` file.
- **Expected:** `poly_system_destroy` called on the old system; `poly_system_read` called on the new path; node cooks without crash. Nuke tile remains green.

### EC-4: No Deep input connected
- Leave input 0 disconnected.
- **Expected:** `_validate` detects `input0() == nullptr`; sets `info_` to empty box with no channels; does not crash; does not call `poly_system_read`.

### EC-5: Holdout input connected (S01 scope)
- Connect a Deep source to input 1 (holdout).
- Cook.
- **Expected:** No crash; output is still flat black (holdout input is declared but not yet read in S01's renderStripe). The holdout wiring is structural only in S01.

---

## Notes for Tester

- TC-1 through TC-5 can be run locally without Nuke or Docker.
- TC-7 is the CI integration gate — must run in an environment with Docker.
- TC-6, TC-8, TC-9 require a Nuke installation with the plugin loaded.
- EC-2 through EC-5 are best verified in Nuke but EC-1/EC-4 can be inferred from the source code guard conditions.
- This UAT is scoped to S01 (scaffold + poly loader). Scatter output, bokeh shape, CoC size, chromatic aberration, and holdout depth-correctness are S02/S03 UAT concerns.
