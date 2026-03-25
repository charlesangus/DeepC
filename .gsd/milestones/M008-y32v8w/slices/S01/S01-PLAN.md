# S01: Path-trace infrastructure + _validate format fix

**Goal:** Both DeepCDefocusPOThin and DeepCDefocusPORay compile (syntax check), output correct frame size via the _validate format fix, Ray has new lens constant knobs, and deepc_po_math.h exposes pt_sample_aperture, sphereToCs_full, and logarithmic_focus_search for S02 to consume.
**Demo:** `bash scripts/verify-s01-syntax.sh` exits 0 — syntax compilation passes for both source files, all structural grep contracts pass (format fix, lens knobs, math functions, poly.h max_degree, CMakeLists.txt registration).

## Must-Haves

- DeepCDefocusPOThin.cpp and DeepCDefocusPORay.cpp exist and pass syntax check
- Old DeepCDefocusPO.cpp removed
- `info_.format(*di.format())` present in both Thin and Ray `_validate` methods (R045)
- 5 new Float_knob members on Ray: `_sensor_width`, `_back_focal_length`, `_outer_pupil_radius`, `_inner_pupil_radius`, `_aperture_pos` with Angenieux 55mm defaults (R038, R043)
- `pt_sample_aperture()` in deepc_po_math.h — Newton aperture match with FD Jacobians, 5 iterations, 1e-4 tolerance, 0.72 damping (R039)
- `sphereToCs_full()` in deepc_po_math.h — full lentil-style tangent-frame sphereToCs returning origin + direction (R040)
- `logarithmic_focus_search()` in deepc_po_math.h — log-spaced sensor_shift search over [-45mm, +45mm] (R042)
- `poly.h` updated with `max_degree` defaulted parameter on `poly_system_evaluate`
- `CMakeLists.txt` references Thin + Ray (not old DeepCDefocusPO)
- `test/test_thin.nk` and `test/test_ray.nk` exist
- `scripts/verify-s01-syntax.sh` passes with all S01 contracts

## Proof Level

- This slice proves: contract (syntax compilation + structural grep)
- Real runtime required: no (syntax check only; runtime verification is S02)
- Human/UAT required: no

## Verification

- `bash scripts/verify-s01-syntax.sh` exits 0
- `bash scripts/verify-s01-syntax.sh 2>&1 | grep -c FAIL` returns 0 (no failed contracts — diagnostic for identifying which specific contract broke)

## Observability / Diagnostics

- **Runtime signals:** The `_validate` format fix ensures `info_.format()` propagates Deep input resolution to the PlanarIop output — observable at runtime when the output frame matches the Deep input resolution instead of a stale default.
- **Inspection surfaces:** `grep -q 'info_\.format(' src/DeepCDefocusPO*.cpp` confirms the fix is present in both nodes. `bash scripts/verify-s01-syntax.sh` provides a single structural contract gate for all S01 deliverables.
- **Failure visibility:** The verify script prints per-contract PASS/FAIL lines and exits non-zero on first failure, making the exact missing contract immediately identifiable. Syntax compilation errors include full g++ diagnostics with line numbers.
- **Failure-path verification:** `bash scripts/verify-s01-syntax.sh` includes grep contracts that fail with descriptive messages when expected patterns are absent (e.g. "FAIL: _sensor_width knob missing in Ray").

## Integration Closure

- Upstream surfaces consumed: M007 branch code (`git show 89de100:src/...`), lentil algorithm reference
- New wiring introduced in this slice: deepc_po_math.h exports 3 new functions consumed by S02's renderStripe; Ray gains lens constant knob members consumed by S02
- What remains before the milestone is truly usable end-to-end: S02 replaces Ray's renderStripe with the path-trace engine using these infrastructure functions

## Tasks

- [x] **T01: Port M007 split files, apply _validate format fix, and add lens constant knobs** `est:45m`
  - Why: The worktree starts from M006 (single DeepCDefocusPO.cpp). Must port the M007 Thin/Ray split, fix the _validate format bug in both, add new lens knobs to Ray, update poly.h/CMakeLists.txt, and create test .nk files. Delivers R043, R045, and the file structure S02 needs.
  - Files: `src/DeepCDefocusPORay.cpp`, `src/DeepCDefocusPOThin.cpp`, `src/poly.h`, `src/CMakeLists.txt`, `test/test_thin.nk`, `test/test_ray.nk`, `src/DeepCDefocusPO.cpp`
  - Do: (1) Extract M007 versions of Ray, Thin, poly.h, CMakeLists.txt, test/*.nk from git commit 89de100 via `git show`. (2) Apply `info_.format(*di.format())` one-line addition to `_validate` in both Thin and Ray (line after `info_.set(di.box())`). (3) Add 5 new Float_knob member variables to Ray with Angenieux 55mm defaults: `_sensor_width=36.0`, `_back_focal_length=30.829003`, `_outer_pupil_radius=29.865562`, `_inner_pupil_radius=19.357308`, `_aperture_pos=35.445997`. Add corresponding `Float_knob`/`SetRange` calls in `knobs()`. (4) Delete `src/DeepCDefocusPO.cpp`.
  - Verify: `test -f src/DeepCDefocusPORay.cpp && test -f src/DeepCDefocusPOThin.cpp && ! test -f src/DeepCDefocusPO.cpp && grep -q 'info_\.format(' src/DeepCDefocusPORay.cpp && grep -q '_sensor_width' src/DeepCDefocusPORay.cpp`
  - Done when: Both Thin and Ray source files exist with _validate format fix, Ray has 5 new lens knobs, poly.h has max_degree, CMakeLists.txt targets Thin+Ray, test .nk files exist, old DeepCDefocusPO.cpp deleted.

- [x] **T02: Implement path-trace math functions in deepc_po_math.h** `est:45m`
  - Why: S02's path-trace renderStripe depends on three new functions: Newton aperture match, full sphereToCs, and logarithmic focus search. These must be implemented now so S02 can consume them. Delivers R039, R040, R042.
  - Files: `src/deepc_po_math.h`
  - Do: (1) Add `sphereToCs_full(x, y, dx, dy, center, R, &ox, &oy, &oz, &odx, &ody, &odz)` — lentil-style tangent-frame construction: normal from sphere, ex/ey from cross products, transform tempDir into camera space, compute origin on sphere surface. (2) Add `pt_sample_aperture(sensor_x, sensor_y, &dx, &dy, target_ax, target_ay, lambda, sensor_shift, poly_sys, max_degree)` — Newton iteration (5 iters, tol 1e-4, EPS 1e-4, 0.72 damping) finding sensor direction that maps to target aperture point. Uses `poly_system_evaluate` with the shifted sensor position as input. (3) Add `logarithmic_focus_search(focal_distance_mm, ...)` — iterate ~20001 log-spaced sensor_shifts from [-45mm, +45mm], trace center ray through pt_sample_aperture + forward eval + sphereToCs_full, intersect y=0 plane, pick shift minimizing |distance - target|.
  - Verify: `grep -q 'sphereToCs_full' src/deepc_po_math.h && grep -q 'pt_sample_aperture' src/deepc_po_math.h && grep -q 'logarithmic_focus_search' src/deepc_po_math.h`
  - Done when: All three functions are implemented inline in deepc_po_math.h with correct signatures and algorithms matching the lentil reference.

- [x] **T03: Update verify-s01-syntax.sh with new mocks and contracts, verify all pass** `est:30m`
  - Why: The syntax-check script must compile both new source files and verify all S01 deliverables via grep contracts. This is the slice's objective stopping condition.
  - Files: `scripts/verify-s01-syntax.sh`
  - Do: (1) Update `DeepInfo` mock to add `const Format* format() const`. (2) Update `ImagePlane` mock to add `writableAt(int,int,int)` overload and `chanNo()` method. (3) Change syntax-check loop to compile `DeepCDefocusPOThin.cpp` and `DeepCDefocusPORay.cpp` (not old `DeepCDefocusPO.cpp`). (4) Add S01 grep contracts: _validate format fix in both files, 5 lens knob members, 3 math functions, poly.h max_degree, CMakeLists.txt Thin/Ray entries. (5) Remove old DeepCDefocusPO references. (6) Run `bash scripts/verify-s01-syntax.sh` and confirm exit 0.
  - Verify: `bash scripts/verify-s01-syntax.sh`
  - Done when: `bash scripts/verify-s01-syntax.sh` exits 0 with all syntax checks and grep contracts passing.

## Files Likely Touched

- `src/DeepCDefocusPORay.cpp`
- `src/DeepCDefocusPOThin.cpp`
- `src/deepc_po_math.h`
- `src/poly.h`
- `src/CMakeLists.txt`
- `src/DeepCDefocusPO.cpp` (deleted)
- `test/test_thin.nk`
- `test/test_ray.nk`
- `scripts/verify-s01-syntax.sh`
