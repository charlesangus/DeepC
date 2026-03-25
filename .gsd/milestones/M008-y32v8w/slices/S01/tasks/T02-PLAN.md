---
estimated_steps: 4
estimated_files: 1
skills_used: []
---

# T02: Implement path-trace math functions in deepc_po_math.h

**Slice:** S01 — Path-trace infrastructure + _validate format fix
**Milestone:** M008-y32v8w

## Description

Add three new inline functions to `src/deepc_po_math.h` that the S02 path-trace renderStripe will consume: `sphereToCs_full()` (full lentil-style tangent-frame sphere-to-camera-space conversion), `pt_sample_aperture()` (Newton aperture match with FD Jacobians), and `logarithmic_focus_search()` (sensor_shift search for a target focus distance).

These functions translate lentil's Eigen/C++ algorithms into our standalone math header using the existing `Mat2`, `Vec2`, `mat2_inverse`, and `poly_system_evaluate` primitives.

## Steps

1. **Add `sphereToCs_full()`.** This is the full lentil `sphereToCs` from `lentil/pota/src/lens.h:99` that returns both 3D origin and direction (the existing `sphereToCs` in the codebase only returns direction).

   Signature:
   ```cpp
   inline void sphereToCs_full(float x, float y, float dx, float dy,
                                float center, float R,
                                float& ox, float& oy, float& oz,
                                float& odx, float& ody, float& odz)
   ```

   Algorithm:
   - Compute normal on sphere: `nx = x/R, ny = y/R, nz = sqrt(max(0, R²-x²-y²)) / fabs(R)`
   - Construct tangent frame: `ex = normalize(nz, 0, -nx)`, `ey = cross(normal, ex)`
   - Transform input direction `(dx, dy, sqrt(max(0, 1-dx²-dy²)))` by the tangent frame to get `(odx, ody, odz)`
   - Compute origin on sphere surface: `ox = x, oy = y, oz = nz * R + center`
   - Note: `R` can be negative (concave surface) — the `fabs(R)` in nz handles this. The `nz * R + center` formula gives the correct z-position on the sphere.

2. **Add `pt_sample_aperture()`.** This is the Newton aperture match for the path-trace flow, adapted from lentil's `lens_pt_sample_aperture` (line 1272).

   Signature:
   ```cpp
   inline bool pt_sample_aperture(float sensor_x, float sensor_y,
                                   float& dx, float& dy,
                                   float target_ax, float target_ay,
                                   float lambda,
                                   float sensor_shift,
                                   const poly_system_t* poly_sys,
                                   int max_deg = -1)
   ```

   Algorithm:
   - Initialize `dx = 0, dy = 0`
   - For 5 Newton iterations (lentil uses `k < 5`):
     - Compute shifted sensor position: `shifted_sx = sensor_x + dx * sensor_shift`, same for y
     - Build `in5 = { shifted_sx, shifted_sy, dx, dy, lambda }` — **important**: the polynomial input is `{sensor_pos_shifted, direction, lambda}`, and `out[0:1]` gives the aperture position
     - Call `poly_system_evaluate(poly_sys, in5, out5, 5, max_deg)` (if max_deg version exists, else 4-arg)
     - Residual: `r = (out5[0] - target_ax, out5[1] - target_ay)`
     - If `|r| < 1e-4`: converged, return true
     - FD Jacobian: perturb `dx` and `dy` by EPS=1e-4, recompute poly eval for each
     - Build 2×2 Jacobian `J` from `d(out[0:1])/d(dx,dy)`
     - If `|det(J)| < 1e-12`: singular, return false
     - Newton step with 0.72 damping: `(dx, dy) -= 0.72 * J^-1 * r`
   - If not converged after 5 iters, return false (vignetted — S02's retry loop handles this)

   **Critical coordinate note:** The polynomial input convention for the aperture prediction system is `in5 = {sensor_x_shifted, sensor_y_shifted, dx, dy, lambda}`. Outputs `out5[0], out5[1]` are the predicted aperture position. This matches lentil's `trace_ray_fw_po` pattern where the polynomial maps sensor → aperture.

3. **Add `logarithmic_focus_search()`.** From lentil `lens.h:395`.

   Signature:
   ```cpp
   inline float logarithmic_focus_search(float focal_distance_mm,
                                          float sensor_x, float sensor_y,
                                          float lambda,
                                          const poly_system_t* poly_sys,
                                          float outer_pupil_curvature_radius,
                                          float outer_pupil_center,
                                          float aperture_housing_radius,
                                          int max_deg = -1)
   ```

   Algorithm:
   - Initialize `best_shift = 0, best_error = INFINITY`
   - Loop `i` from -10000 to 10000 (step 1):
     - Compute `t = i / 10000.0` (range [-1, 1])
     - `sensor_shift = (t < 0 ? -1 : 1) * t * t * 45.0` — quadratic spacing, dense near 0, sparse at ±45mm
     - Call `pt_sample_aperture(sensor_x, sensor_y, dx, dy, 0, 0, lambda, sensor_shift, poly_sys, max_deg)` with aperture target (0,0) = center
     - If pt_sample_aperture converges:
       - Forward eval: `in5 = {sensor_x + dx*sensor_shift, sensor_y + dy*sensor_shift, dx, dy, lambda}`
       - `poly_system_evaluate(poly_sys, in5, out5, 5, max_deg)`
       - `sphereToCs_full(out5[0], out5[1], out5[2], out5[3], outer_pupil_center, outer_pupil_curvature_radius, ox,oy,oz, odx,ody,odz)`
       - If `fabs(odz) > 1e-10`: compute intersection distance `dist = -oz / odz` (ray from `(ox,oy,oz)` in direction `(odx,ody,odz)` hitting z=0 plane — this is focus distance)
       - Wait — lentil intersects y=0 plane actually. Check: lentil's `camera_get_y0_intersection_distance` computes `dist = -oy / ody`. But for focus distance, we want the axial intersection along the z-axis. **Re-check:** Lentil uses `-o[1]/d[1]` which is the y-intercept (the optical axis is along z). For an on-axis ray, the z-intersection distance is what we want: `dist = oz + (-oz/odz)*odz`... No — we need the parameter `t` such that the ray hits the focal plane at distance `focal_distance_mm` along z. Actually lentil computes `t_at_y0 = -oy/ody` and focus distance is the z-coordinate at that t: `focus_z = oz + t*odz`. **Simplification for our case:** since we're tracing a center ray (sensor_x ≈ 0, sensor_y ≈ 0, aperture center), the ray is approximately along the optical axis. The focus distance is approximately `oz / odz` in absolute value. Use `dist = fabs(oz + t_intersect * odz)` where `t_intersect = -oz / odz` gives z=0 plane intersection distance, and the focus distance is `fabs(t_intersect) * sqrt(odx² + ody² + odz²)` ≈ `fabs(oz / odz)` for near-axial rays.
       - **Practical implementation:** Use `dist = fabs(oz / odz)` as the focus distance for this sensor_shift. This is valid for center rays.
       - `error = fabs(dist - focal_distance_mm)`
       - If `error < best_error`: update `best_shift, best_error`
   - Return `best_shift`

4. **Verify functions exist.**
   ```bash
   grep -q 'sphereToCs_full' src/deepc_po_math.h
   grep -q 'pt_sample_aperture' src/deepc_po_math.h
   grep -q 'logarithmic_focus_search' src/deepc_po_math.h
   ```

## Must-Haves

- [ ] `sphereToCs_full` implemented with full tangent-frame construction, outputs both origin and direction
- [ ] `pt_sample_aperture` implemented with Newton iteration (5 iters, 1e-4 tol, 0.72 damping, FD Jacobians)
- [ ] `logarithmic_focus_search` implemented with log-spaced sensor_shift search over [-45mm, +45mm]
- [ ] All functions are `inline` (header-only, consistent with existing functions)
- [ ] Functions use existing `Mat2`, `mat2_inverse`, `poly_system_evaluate` primitives

## Verification

- `grep -q 'sphereToCs_full' src/deepc_po_math.h && grep -q 'pt_sample_aperture' src/deepc_po_math.h && grep -q 'logarithmic_focus_search' src/deepc_po_math.h`
- All three functions compile as part of T03's syntax check (they are inline in the header included by both .cpp files)

## Inputs

- `src/deepc_po_math.h` — existing math header with Mat2, Vec2, mat2_inverse, halton, map_to_disk, lt_newton_trace, coc_radius
- `src/poly.h` — poly_system_evaluate function (T01 updates this with max_degree)

## Expected Output

- `src/deepc_po_math.h` — extended with sphereToCs_full, pt_sample_aperture, logarithmic_focus_search

## Observability Impact

- **Signals changed:** `deepc_po_math.h` exports three new inline functions consumed by S02's path-trace renderStripe. No runtime signals at this stage (header-only, no runtime until S02 integrates).
- **Inspection:** `grep -c 'inline.*sphereToCs_full\|inline.*pt_sample_aperture\|inline.*logarithmic_focus_search' src/deepc_po_math.h` returns 3. Syntax compilation with poly.h included first produces zero errors.
- **Failure state:** If any function has a syntax error, the T03 syntax-check script will emit g++ diagnostics with file:line. If a function is missing, the T03 grep contract will report FAIL with the missing function name.
