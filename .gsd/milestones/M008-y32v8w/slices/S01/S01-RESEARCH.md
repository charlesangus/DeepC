# S01 Research: Path-trace infrastructure + _validate format fix

**Slice:** S01 — Path-trace infrastructure + _validate format fix
**Milestone:** M008-y32v8w
**Researched:** 2026-03-25

---

## Summary

S01 is a **targeted infrastructure slice**. The worktree branches from `master` (commit `a9fe480`, the M006 baseline), which has only `DeepCDefocusPO.cpp`. The completed M007 split (Thin + Ray variants) exists on a separate branch (`89de100`) that was never merged into this worktree's base. S01 must therefore port the M007 files into the worktree, apply the `_validate` format fix, add new lens constant knobs to Ray, extend `deepc_po_math.h` with three new functions, update poly.h, CMakeLists.txt, and verify-s01-syntax.sh, and create the `test/` directory with both `.nk` files.

Work is **medium complexity**: the code templates exist in the git history; the primary risks are (1) the `_validate` format fix getting the right NDK API calls, and (2) implementing `pt_sample_aperture` with FD Jacobians correctly.

---

## Codebase State (worktree baseline)

### What exists in the worktree (M006 base):
- `src/DeepCDefocusPO.cpp` — M006 PO scatter node (to be replaced)
- `src/deepc_po_math.h` — has: `Mat2`, `Vec2`, `halton`, `map_to_disk`, `lt_newton_trace`, `coc_radius`, plus a **partial** `sphereToCs` (direction-only, no origin)
- `src/poly.h` — **NO** `max_degree` parameter on `poly_system_evaluate`
- `src/CMakeLists.txt` — references `DeepCDefocusPO` (not Thin/Ray)
- `scripts/verify-s01-syntax.sh` — checks `DeepCDefocusPO.cpp` only; mock headers have old `DeepInfo` (no `format()` getter, no `fullSizeFormat()`)
- `THIRD_PARTY_LICENSES.md` — **already contains** the lentil/poly.h entry (no action needed)
- `test/` — **does not exist** yet

### What the M007 branch (`89de100`) has (available via `git show`):
- `src/DeepCDefocusPOThin.cpp` (529 lines) — full thin-lens scatter engine
- `src/DeepCDefocusPORay.cpp` (660 lines) — gather + scatter + sphereToCs call
- `src/poly.h` — updated with `max_degree` default param on `poly_system_evaluate`
- `src/deepc_po_math.h` — partial `sphereToCs` already present
- `scripts/verify-s01-syntax.sh` — updated mock (chanNo, fullSizeFormat, int-indexed writableAt)
- `test/test_thin.nk` and `test/test_ray.nk` — 128×72 Nuke scripts

---

## The `_validate` Format Bug

**Symptom:** Both `DeepCDefocusPOThin` and `DeepCDefocusPORay` output the wrong frame size (not matching the Deep input's format). Artists see a 1×1 or default-format output instead of the Deep input's resolution.

**Root cause confirmed from NDK headers:**

`DeepInfo` extends `Info2D` which stores two separate format pointers:
- `_format` — used by `PlanarIop` for render allocation (the "what size do I output" format)
- `_fullSizeFormat` — used for proxy scaling

`IopInfo::info_` has two setters:
- `info_.format(const Format& v)` — sets `_format` 
- `info_.full_size_format(const Format& v)` — sets `_fullSizeFormat`

`DeepInfo` exposes:
- `di.format()` → `const Format*` (returns `&Info2D::format()`)
- `di.fullSizeFormat()` → `const Format*` (returns `_fullSizeFormat`)

**Both M006 and M007 `_validate` only call `info_.full_size_format(...)` — they never call `info_.format(...)`**. This leaves `_format` pointing at whatever `IopInfo`'s default is (likely null or a stale pointer), causing `PlanarIop` to allocate output at a wrong size.

**Fix:**
```cpp
const DeepInfo& di = in->deepInfo();
info_.set(di.box());
info_.format(*di.format());               // ← ADD THIS
info_.full_size_format(*di.fullSizeFormat());
info_.channels(Mask_RGBA);
set_out_channels(Mask_RGBA);
```

Both `Thin` and `Ray` `_validate` methods need this one-line addition.

**Mock update needed:** The verify-s01-syntax.sh mock `DeepInfo` must gain `const Format* format() const` alongside the existing `fullSizeFormat()`.

---

## Implementation Landscape

### File-by-file plan:

#### 1. `src/poly.h` — add `max_degree` parameter
The M007 version adds `int max_degree = -1` as a defaulted 5th parameter to `poly_system_evaluate`, with an inner degree-computation loop that breaks early when `deg > max_degree`. This is a purely additive change — default `-1` means uncapped (backward compatible).

**Key detail:** Degree is computed as `sum of abs(exp[i])` for all 5 input dimensions per term.

#### 2. `src/deepc_po_math.h` — three new functions

**A. `sphereToCs_full()` — full lentil-style sphereToCs**

The existing `sphereToCs()` in M007 computes only direction (not origin). The full lentil version from `/home/latuser/git/lentil/pota/src/lens.h:99` also outputs 3D origin and uses a proper tangent-frame construction. The M008 path-trace flow needs both origin and direction.

Lentil algorithm (Eigen-free translation):
```
normal = (x/R, y/R, sqrt(max(0, R²-x²-y²))/|R|)
tempDir = (dx, dy, sqrt(max(0, 1-dx²-dy²)))
ex = normalize(normal.z, 0, -normal.x)   // tangent
ey = cross(normal, ex)                   // bitangent
outdir = tempDir[0]*ex + tempDir[1]*ey + tempDir[2]*normal
outpos = (x, y, normal.z*R + center)
```
Signature: `sphereToCs_full(float x, float y, float dx, float dy, float center, float R, float& ox, float& oy, float& oz, float& odx, float& ody, float& odz)`

**B. `pt_sample_aperture()` — Newton aperture match with FD Jacobians**

Lentil's `lens_pt_sample_aperture` (line 1272) uses per-lens analytic Jacobians from generated code (loaded via `load_pt_sample_aperture.h`). Our version uses FD Jacobians with the generic `poly_system_evaluate`.

The function finds sensor direction `(dx, dy)` such that when the sensor is shifted by `sensor_shift` and the polynomial is evaluated, the aperture output `out[0:1]` matches the target aperture position `(ax, ay)`.

Algorithm (from lentil trace_ray_fw_po pattern):
```
Input: sensor_x, sensor_y, target_ax, target_ay, lambda, sensor_shift, poly_sys
Search for: dx, dy (sensor ray direction components)

For each Newton iteration (max 5, tolerance 1e-4):
  shifted_sx = sensor_x + dx * sensor_shift
  shifted_sy = sensor_y + dy * sensor_shift
  in5 = { shifted_sx, shifted_sy, target_ax, target_ay, lambda }
  poly_evaluate(in5, out5)
  residual = (out5[0] - target_ax, out5[1] - target_ay)
  if |residual| < tol: break
  FD Jacobian: perturb dx, dy by EPS, recompute
  Newton step: (dx, dy) -= J^-1 * residual * 0.72  (lentil's 0.72 damping)
Output: dx, dy updated in-place
```

Note: The 0.72 damping factor is taken directly from lentil's Newton iteration for stability.

**C. `logarithmic_focus_search()` — sensor_shift for focus distance**

From `/home/latuser/git/lentil/pota/src/lens.h:395`, the logarithmic values range from `[-45mm, +45mm]` using `(i < 0 ? -1 : 1) * i² * 45` stepping `i` from -1 to 1 in increments of 0.0001. This gives ~20001 candidate sensor shifts, densely sampling near 0 and sparsely at extremes.

For each candidate `sensor_shift`, call `camera_get_y0_intersection_distance()` equivalent: trace a center ray through `pt_sample_aperture`, shift sensor, forward evaluate, `sphereToCs_full`, intersect with the `y=0` plane to get focus distance. Pick the shift that minimizes `|focal_distance - test_distance|`.

Signature: `float logarithmic_focus_search(float focal_distance_mm, float sensor_x, float sensor_y, float lambda, const poly_system_t* poly_sys, float outer_pupil_curvature_radius, float aperture_housing_radius)`

The function is used once at render time (in _validate or renderStripe) to compute `_sensor_shift` from `_focus_distance`.

#### 3. `src/DeepCDefocusPORay.cpp` — new lens constant knobs + _validate fix

Port from M007 `DeepCDefocusPORay.cpp` (`git show 89de100:src/DeepCDefocusPORay.cpp`) and apply:

**New member variables** (add alongside existing geometry knobs):
```cpp
float _sensor_width;         // 36.0f default
float _back_focal_length;    // 30.829003f (Angenieux 55mm)
float _outer_pupil_radius;   // 29.865562f (Angenieux 55mm)
float _inner_pupil_radius;   // 19.357308f (Angenieux 55mm)
float _aperture_pos;         // 35.445997f (Angenieux 55mm)
```

**New knobs** in `knobs()` (in the "Lens geometry" group):
```cpp
Float_knob(f, &_sensor_width, "sensor_width", "sensor width (mm)");
SetRange(f, 1.0f, 100.0f);
Float_knob(f, &_back_focal_length, "back_focal_length", "back focal length (mm)");
SetRange(f, 0.0f, 200.0f);
Float_knob(f, &_outer_pupil_radius, "outer_pupil_radius", "outer pupil radius (mm)");
SetRange(f, 0.1f, 100.0f);
Float_knob(f, &_inner_pupil_radius, "inner_pupil_radius", "inner pupil radius (mm)");
SetRange(f, 0.1f, 100.0f);
Float_knob(f, &_aperture_pos, "aperture_pos", "aperture pos (mm)");
SetRange(f, 0.0f, 200.0f);
```

**_validate format fix:** add `info_.format(*di.format())` line.

The renderStripe in S01 stays as the M007 scatter engine (not yet replaced with path-trace). S02 will replace it.

#### 4. `src/DeepCDefocusPOThin.cpp` — _validate fix only

Port from M007 (`git show 89de100:src/DeepCDefocusPOThin.cpp`). Apply `info_.format(*di.format())` fix. No other changes.

#### 5. `src/CMakeLists.txt` — replace DeepCDefocusPO with Thin + Ray

Changes:
- PLUGINS: remove `DeepCDefocusPO`, add `DeepCDefocusPOThin` and `DeepCDefocusPORay`
- FILTER_NODES: same replacement

#### 6. `scripts/verify-s01-syntax.sh` — update mock + checked files

Changes from M007 version (`git show 89de100:scripts/verify-s01-syntax.sh`):
- `DeepInfo` mock: add `const Format* format() const { static Format f; return &f; }` 
- `ImagePlane` mock: add `writableAt(int,int,int)` overload and `chanNo()` method
- `Info` class: update `full_size_format() const` to return `const Format&` (M007 changed this)
- Syntax-check loop: `DeepCDefocusPOThin.cpp`, `DeepCDefocusPORay.cpp` (not `DeepCDefocusPO.cpp`)
- S05 contracts: check Thin/Ray in 2 CMakeLists.txt locations each; old `DeepCDefocusPO.cpp` deleted; `lentil` in THIRD_PARTY_LICENSES.md

**New S01 contracts to add:**
```bash
# _validate format fix in both files
grep -q 'info_\.format(' "$SRC_DIR/DeepCDefocusPOThin.cpp"  || { echo "FAIL: _validate format fix missing from Thin"; exit 1; }
grep -q 'info_\.format(' "$SRC_DIR/DeepCDefocusPORay.cpp"   || { echo "FAIL: _validate format fix missing from Ray"; exit 1; }
# New lens constant knobs on Ray
grep -q '_sensor_width'        "$SRC_DIR/DeepCDefocusPORay.cpp" || { echo "FAIL: _sensor_width knob missing"; exit 1; }
grep -q '_back_focal_length'   "$SRC_DIR/DeepCDefocusPORay.cpp" || { echo "FAIL: _back_focal_length knob missing"; exit 1; }
grep -q '_outer_pupil_radius'  "$SRC_DIR/DeepCDefocusPORay.cpp" || { echo "FAIL: _outer_pupil_radius knob missing"; exit 1; }
grep -q '_inner_pupil_radius'  "$SRC_DIR/DeepCDefocusPORay.cpp" || { echo "FAIL: _inner_pupil_radius knob missing"; exit 1; }
grep -q '_aperture_pos'        "$SRC_DIR/DeepCDefocusPORay.cpp" || { echo "FAIL: _aperture_pos knob missing"; exit 1; }
# New math functions in deepc_po_math.h
grep -q 'pt_sample_aperture'       "$SRC_DIR/deepc_po_math.h" || { echo "FAIL: pt_sample_aperture missing"; exit 1; }
grep -q 'sphereToCs_full'          "$SRC_DIR/deepc_po_math.h" || { echo "FAIL: sphereToCs_full missing"; exit 1; }
grep -q 'logarithmic_focus_search' "$SRC_DIR/deepc_po_math.h" || { echo "FAIL: logarithmic_focus_search missing"; exit 1; }
# poly.h max_degree
grep -q 'max_degree'               "$SRC_DIR/poly.h"          || { echo "FAIL: max_degree missing from poly.h"; exit 1; }
```

#### 7. `test/` directory — create test .nk scripts

Both `test_thin.nk` and `test_ray.nk` from M007 branch are usable via `git show 89de100:test/test_ray.nk` and `test/test_thin.nk`. They already reference 128×72 format and Angenieux 55mm .fit paths.

---

## Sequence of Tasks (recommended for planner)

**T01: Port M007 code into worktree** (infrastructure, no logic changes yet)
- Update `src/poly.h` with max_degree parameter
- Create `src/DeepCDefocusPOThin.cpp` from M007 (port verbatim, same as M007)
- Create `src/DeepCDefocusPORay.cpp` from M007 (port verbatim, same as M007)
- Remove `src/DeepCDefocusPO.cpp`
- Update `src/CMakeLists.txt` (Thin + Ray in PLUGINS and FILTER_NODES)
- Create `test/test_thin.nk` and `test/test_ray.nk` from M007

**T02: Apply _validate format fix to both Thin and Ray**
- Add `info_.format(*di.format())` to `_validate` in both files
- Update verify-s01-syntax.sh mock to add `DeepInfo::format()` and update for chanNo/writableAt

**T03: Add new lens constant knobs to Ray**
- Add 5 new Float_knob members + ctor defaults to `DeepCDefocusPORay`
- Add knobs in the lens geometry group
- Angenieux 55mm defaults from lens_constants.h

**T04: Add new math functions to deepc_po_math.h**
- `sphereToCs_full()` — full lentil sphereToCs with tangent-frame, origin+direction output
- `pt_sample_aperture()` — Newton aperture match with FD Jacobians (5 iterations, tol 1e-4, 0.72 damping)
- `logarithmic_focus_search()` — iterate log-spaced sensor_shifts from [-45mm, +45mm], pick best match

**T05: Update verify-s01-syntax.sh + contracts**
- Update mock headers for Thin/Ray API (already partially done in T02)
- Add S01 grep contracts for the new deliverables
- Remove references to old DeepCDefocusPO file
- Verify `bash scripts/verify-s01-syntax.sh` passes

---

## Key Risks and Constraints

### Risk 1: `pt_sample_aperture` FD Jacobian convergence
The per-lens analytic Jacobians in `pt_sample_aperture.h` (e.g., for Angenieux 55mm) include all cross-terms. FD Jacobians lose this precision. At field edge (large `sx`, `sy`), convergence may require more iterations or smaller EPS. **Mitigated by:**
- Using lentil's 0.72 damping factor
- 5 iteration maximum (same as analytic version's `k<5`)
- Vignetting retry loop in S02 handles ray failures

### Risk 2: `logarithmic_focus_search` range for Angenieux 55mm
Lentil's range is `[-45mm, +45mm]`. For Angenieux 55mm with `back_focal_length = 30.829003mm`, this range is sufficient — the sensor shift for focus at infinity is on the order of a few mm. Test with `focus_distance = 200mm` (the default knob value).

### Risk 3: Coordinate convention in `pt_sample_aperture`
The polynomial input for aperture prediction is:
```
in5 = { sensor_x + dx*sensor_shift, sensor_y + dy*sensor_shift, ax, ay, lambda }
```
where `sensor_x, sensor_y` are in **physical mm** (per D037 for Ray: `sx = pixel * sensor_width / (2 * half_res)`). The aperture target `ax, ay` is also in mm (`unit_disk * aperture_radius_mm`).

This is different from the M007 Ray scatter engine which uses normalised coords. S01 adds the knobs but does not change the renderStripe — the path-trace flow using mm coords is S02's job. The new knobs just need to be present and compilable.

### Risk 4: Mock header completeness for new knob types
All new knobs use `Float_knob` + `SetRange` + `Tooltip` — already stubbed in the mock. `BeginClosedGroup` / `EndGroup` are also in the mock. No new DDImage types needed.

### Risk 5: The old `DeepCDefocusPO.cpp` includes poly.h
When `DeepCDefocusPO.cpp` is deleted, `poly.h` ODR constraint is relaxed — Thin and Ray each include it, but since they compile to separate `.so` files, there's no ODR violation. The build system compiles each `.cpp` independently via `add_nuke_plugin`.

---

## What S02 Consumes from S01

S02 relies on these S01 deliverables from `deepc_po_math.h`:
- `pt_sample_aperture()` — for Newton aperture match at each ray
- `sphereToCs_full()` — for 3D origin + direction from polynomial output
- `logarithmic_focus_search()` — to compute `_sensor_shift` from `_focus_distance` at render time
- New knob members on Ray: `_sensor_width`, `_back_focal_length`, `_outer_pupil_radius`, `_inner_pupil_radius`, `_aperture_pos` — used to configure the path-trace coordinate system and vignetting guards

S02 also requires that both nodes compile cleanly with the updated `poly.h` (max_degree support).

---

## Verification

The primary verification for S01 is: `bash scripts/verify-s01-syntax.sh` exits 0.

This covers:
- Syntax compilation of `DeepCDefocusPOThin.cpp` and `DeepCDefocusPORay.cpp`
- Structural grep contracts for all S01 deliverables
- CMakeLists.txt registration (2 entries each for Thin and Ray)
- THIRD_PARTY_LICENSES.md check (already present)
- Old `DeepCDefocusPO.cpp` removal confirmation

No runtime verification in S01 — the renderStripe in Ray is still the M007 scatter engine (not path-trace). S02 completes the engine swap.
