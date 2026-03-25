---
id: S01
milestone: M008-y32v8w
title: "Path-trace infrastructure + _validate format fix"
status: complete
completed_at: 2026-03-25
tasks_completed: [T01, T02, T03]
verification_result: passed
---

# S01 Summary: Path-trace infrastructure + _validate format fix

## What This Slice Delivered

S01 built all the infrastructure S02 needs to replace Ray's scatter engine with a real lentil-style path tracer. Three parallel deliverables:

1. **_validate format fix** — Both `DeepCDefocusPOThin.cpp` and `DeepCDefocusPORay.cpp` now call `info_.format(*di.format())` in `_validate`, propagating the Deep input's resolution to PlanarIop. Without this, the output frame size defaulted to a stale value and downstream compositing failed.

2. **New lens constant knobs on Ray** — Five `Float_knob` members added to `DeepCDefocusPORay` with Angenieux 55mm defaults: `_sensor_width` (36.0mm), `_back_focal_length` (30.829003mm), `_outer_pupil_radius` (29.865562mm), `_inner_pupil_radius` (19.357308mm), `_aperture_pos` (35.445997mm). These expose the physical lens geometry the path-trace engine needs for sensor_shift computation, pupil culling, and Newton aperture matching. Knobs are grouped inside the existing "Lens geometry" BeginClosedGroup.

3. **Three path-trace math functions** — Added inline to `src/deepc_po_math.h`:
   - `sphereToCs_full(x, y, dx, dy, center, R, &ox, &oy, &oz, &odx, &ody, &odz)` — full tangent-frame construction. Computes surface normal, builds orthonormal ex/ey frame via cross products, transforms input direction into camera space, returns 3D origin on sphere surface + transformed direction. This is the lentil-equivalent that returns origin + direction rather than direction only.
   - `pt_sample_aperture(sensor_x, sensor_y, &dx, &dy, target_ax, target_ay, lambda, sensor_shift, poly_sys, max_degree)` — Newton solver (5 iters, 1e-4 tol, 1e-4 FD epsilon, 0.72 damping) that finds the sensor direction mapping to a target aperture point. FD Jacobian correctly couples dx/dy perturbation with sensor_shift.
   - `logarithmic_focus_search(focal_distance_mm, ...)` — 20001-step quadratically-spaced brute-force over [-45mm, +45mm], traces center ray through pt_sample_aperture + forward eval + sphereToCs_full, picks sensor_shift minimising focus error.
   - `sphereToCs` (direction-only) — the M007 original, restored for Ray's current renderStripe. This is still consumed by the existing renderStripe during S02 transition; S02 will migrate to sphereToCs_full.

4. **Updated verify-s01-syntax.sh** — Replaced old S05 contracts with 13 S01 structural contracts. Added `DeepInfo::format()` and `Info::format(const Format&)` mocks required for the _validate format fix to compile. Script now targets both `DeepCDefocusPOThin.cpp` and `DeepCDefocusPORay.cpp`.

5. **File structure** — `src/DeepCDefocusPO.cpp` deleted. Both split files ported from M007 commit `89de100`. `poly.h` has `max_degree` default parameter. CMakeLists.txt targets Thin + Ray. Test .nk files for both nodes present at 128×72.

## Verification

`bash scripts/verify-s01-syntax.sh` exits 0 with all 13 S01 contracts passing and all 4 source files compiling (DeepCBlur, DeepCDepthBlur, DeepCDefocusPOThin, DeepCDefocusPORay). Zero FAILs.

```
PASS: _validate format fix in Thin
PASS: _validate format fix in Ray
PASS: _sensor_width knob in Ray
PASS: _back_focal_length knob in Ray
PASS: _outer_pupil_radius knob in Ray
PASS: _inner_pupil_radius knob in Ray
PASS: _aperture_pos knob in Ray
PASS: pt_sample_aperture in deepc_po_math.h
PASS: sphereToCs_full in deepc_po_math.h
PASS: logarithmic_focus_search in deepc_po_math.h
PASS: max_degree in poly.h
PASS: DeepCDefocusPOThin in CMakeLists.txt
PASS: DeepCDefocusPORay in CMakeLists.txt
```

## Patterns Established

- **_validate format fix pattern** — `info_.format(*di.format())` inserted after `info_.set(di.box())` and before `info_.full_size_format()` in both PO node `_validate` methods. Apply this pattern to any future PlanarIop that must match a Deep input's resolution.
- **Sensor mm coordinate system for Ray** — Sensor positions are physical mm (not normalised [-1,1]): `sx = pixel_x * sensor_width / (2 * half_width_pixels)`. The Thin variant keeps normalised coords. Do not apply mm scaling to Thin.
- **Mock header maintenance** — When the `_validate` format fix or any new DDImage API is used, the mock stubs in `verify-s01-syntax.sh` must be updated to add the corresponding methods. Check mocks first if the syntax script fails after a refactor.
- **sphereToCs_full vs sphereToCs** — Two variants now coexist in deepc_po_math.h. The direction-only `sphereToCs` is the M007 original (for current Ray renderStripe compatibility). `sphereToCs_full` returns origin + direction and is what S02's path-trace engine must use. S02 should migrate all renderStripe sphereToCs calls to the full variant.
- **logarithmic_focus_search: quadratic not logarithmic** — Despite the name (matching lentil's API), the implementation uses quadratic spacing (not log). The housing radius parameter is accepted but not used as a bound — the search covers the full [-45, +45] range. S02 may add housing-radius clipping if vignetting from pupil edges becomes an issue.

## What S02 Should Know

S02 can consume these three functions directly. The function signatures are:
```cpp
// Full sphere surface → 3D ray (use this, not the direction-only sphereToCs)
inline void sphereToCs_full(float x, float y, float dx, float dy,
    float center, float R,
    float& ox, float& oy, float& oz, float& odx, float& ody, float& odz);

// Newton aperture match: find sensor direction that maps to (target_ax, target_ay)
// Returns false on non-convergence
inline bool pt_sample_aperture(float sensor_x, float sensor_y,
    float& dx, float& dy,
    float target_ax, float target_ay, float lambda, float sensor_shift,
    const poly_system* poly_sys, int max_degree = -1);

// Find sensor_shift that focuses lens at focal_distance_mm
inline float logarithmic_focus_search(float focal_distance_mm,
    float outer_pupil_radius, float back_focal_length, float aperture_housing_radius,
    const poly_system* poly_sys, int max_degree = -1);
```

The five new knob members on Ray are already declared in the class. The path-trace engine should read `_sensor_width`, `_back_focal_length`, `_outer_pupil_radius`, `_inner_pupil_radius`, `_aperture_pos` directly. `logarithmic_focus_search` should be called once per renderStripe (or in `_validate`) to compute `sensor_shift` from `_focus_distance` (existing knob) and `_back_focal_length` / `_outer_pupil_radius`.

The existing Ray renderStripe (Option B scatter) is the code S02 replaces. The new engine starts from sensor position → `pt_sample_aperture` → forward `poly_system_evaluate` → `sphereToCs_full` → 3D ray direction → project to input pixel → read and flatten deep column → accumulate flat RGBA.

## Requirements Coverage

- R038 ✅ Validated — sensor_width Float_knob (36mm default) added to Ray
- R039 ✅ Validated — pt_sample_aperture implemented in deepc_po_math.h
- R040 ✅ Validated — sphereToCs_full implemented in deepc_po_math.h
- R042 ✅ Validated — logarithmic_focus_search implemented in deepc_po_math.h
- R043 ✅ Validated — all 5 lens constant knobs on Ray with Angenieux 55mm defaults
- R045 ✅ Validated — info_.format(*di.format()) in both Thin and Ray _validate
- R037 — Active, deferred to S02 (requires renderStripe swap)

## Known Gaps / S02 Start Conditions

- Ray's renderStripe still uses the old Option B scatter engine — S02 replaces it.
- `logarithmic_focus_search` housing radius parameter is accepted but unused; the full [-45,+45] range is searched regardless.
- The `sphereToCs` direction-only function remains in deepc_po_math.h for Ray's current (S01) renderStripe. Once S02 migrates to sphereToCs_full, the old variant can be removed or kept for backward compatibility.
- No docker build / `nuke -x` run in S01 (proof level: contract only). Runtime proof is S02's gate.
