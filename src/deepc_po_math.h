// SPDX-License-Identifier: MIT
//
// deepc_po_math.h — DeepCDefocusPO math primitives
//
// Standalone math header: no DDImage dependency, no poly.h include.
// Receives poly_system_t by pointer via a forward declaration only — this
// is the ODR firewall: poly.h (which defines functions inline) is included
// in exactly one TU (DeepCDefocusPO.cpp) and must NOT be included here.
//
// Contents:
//   Mat2            — 2×2 double matrix
//   mat2_inverse    — analytic 2×2 inverse (replaces Eigen — see D023)
//   Vec2            — 2D double vector
//   halton          — Halton low-discrepancy sequence
//   map_to_disk     — Shirley concentric square-to-disk mapping
//   lt_newton_trace — polynomial-optics Newton trace
//   coc_radius      — thin-lens circle-of-confusion radius
//
// ============================================================================

#pragma once

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>

// Forward-declare poly_system_t so lt_newton_trace can accept a pointer
// without pulling in the full poly.h definition (which has inline function
// bodies that would cause ODR violations if included in multiple TUs).
//
// When poly.h is included BEFORE this header in the same TU (as in
// DeepCDefocusPO.cpp), the typedef from poly.h is already visible and we
// must not redeclare it.  poly.h sets DEEPC_POLY_H when included, so we
// skip the forward declaration in that case.
#ifndef DEEPC_POLY_H
typedef struct poly_system_s poly_system_t;
#endif

// forward-declare poly_system_evaluate so lt_newton_trace can call it.
// When poly.h is included before this header the definition is already
// visible; when not included we need at least a declaration.
#ifndef DEEPC_POLY_H
inline void poly_system_evaluate(const poly_system_t* sys,
                                 const float* input,
                                 float* output,
                                 int num_out,
                                 int max_degree = -1);
#endif

// ---------------------------------------------------------------------------
// Mat2 — 2×2 column-major double matrix
// ---------------------------------------------------------------------------
struct Mat2 {
    double m[2][2];  // m[row][col]
};

// mat2_inverse — analytic inverse of a 2×2 matrix
//
// Uses the standard formula:
//   inv(J) = (1/det) * [[ J11, -J01 ],
//                       [-J10,  J00 ]]
// where det = J00*J11 - J01*J10.
//
// No singularity check — callers must ensure det != 0 (the Newton iteration
// checks the Jacobian condition before calling this).
inline Mat2 mat2_inverse(const Mat2& J)
{
    const double invdet = 1.0 / (J.m[0][0] * J.m[1][1] - J.m[0][1] * J.m[1][0]);
    Mat2 inv;
    inv.m[0][0] =  J.m[1][1] * invdet;
    inv.m[0][1] = -J.m[0][1] * invdet;
    inv.m[1][0] = -J.m[1][0] * invdet;
    inv.m[1][1] =  J.m[0][0] * invdet;
    return inv;
}

// ---------------------------------------------------------------------------
// Vec2 — 2D double vector
// ---------------------------------------------------------------------------
struct Vec2 {
    double x, y;
};

// ---------------------------------------------------------------------------
// halton — Halton low-discrepancy sequence
//
// Returns the index-th value in the Halton sequence for the given base.
// index should start from 1 (index 0 gives 0.0 for all bases).
// Use base 2 and base 3 together for a well-distributed 2D sample set.
// ---------------------------------------------------------------------------
inline float halton(int index, int base)
{
    float result = 0.0f;
    float f = 1.0f;
    int i = index + 1;  // skip index 0 (maps to 0 for all bases)
    while (i > 0) {
        f /= static_cast<float>(base);
        result += f * static_cast<float>(i % base);
        i /= base;
    }
    return result;
}

// ---------------------------------------------------------------------------
// map_to_disk — Shirley concentric square-to-disk mapping
//
// Maps (u, v) in [0, 1) uniformly to a point (x, y) on the unit disk with
// no area distortion (Peter Shirley & Kenneth Chiu, 1997).
// ---------------------------------------------------------------------------
inline void map_to_disk(float u, float v, float& x, float& y)
{
    const float a = 2.0f * u - 1.0f;
    const float b = 2.0f * v - 1.0f;
    if (a == 0.0f && b == 0.0f) { x = y = 0.0f; return; }
    float r, phi;
    if (std::fabs(a) > std::fabs(b)) {
        r   = a;
        phi = (static_cast<float>(M_PI) / 4.0f) * (b / a);
    } else {
        r   = b;
        phi = (static_cast<float>(M_PI) / 2.0f)
            - (static_cast<float>(M_PI) / 4.0f) * (a / b);
    }
    x = r * std::cos(phi);
    y = r * std::sin(phi);
}

// ---------------------------------------------------------------------------
// lt_newton_trace — polynomial-optics Newton trace
//
// Solves the inverse lens mapping: given a target sensor position and an
// aperture sample, iterates Newton's method through the polynomial system
// to find where the aperture sample actually lands on the sensor at the
// given wavelength.
//
// Returns: Vec2{sensor_x, sensor_y} of the landing position in normalised
//          sensor coordinates (same units as sensor_target).
//
// poly_sys must be non-null and loaded (caller's responsibility).
// ---------------------------------------------------------------------------
inline Vec2 lt_newton_trace(const float sensor_target[2],
                            const float aperture_pos[2],
                            float       lambda_um,
                            const poly_system_t* poly_sys)
{
    float pos[2] = { sensor_target[0], sensor_target[1] };

    const int   MAX_ITER = 20;
    const float TOL      = 1e-5f;
    const float EPS      = 1e-4f;

    for (int iter = 0; iter < MAX_ITER; ++iter) {
        float in5[5]  = { pos[0], pos[1],
                           aperture_pos[0], aperture_pos[1], lambda_um };
        float out5[5] = {};
        poly_system_evaluate(poly_sys, in5, out5, 5);

        // out5[2], out5[3] = back-projected sensor position at (pos, aperture)
        const float res0 = out5[2] - sensor_target[0];
        const float res1 = out5[3] - sensor_target[1];

        if (res0 * res0 + res1 * res1 < TOL * TOL)
            break;

        // Finite-difference Jacobian
        float in_dx[5] = { pos[0]+EPS, pos[1],     aperture_pos[0], aperture_pos[1], lambda_um };
        float in_dy[5] = { pos[0],     pos[1]+EPS, aperture_pos[0], aperture_pos[1], lambda_um };
        float out_dx[5] = {}, out_dy[5] = {};
        poly_system_evaluate(poly_sys, in_dx, out_dx, 5);
        poly_system_evaluate(poly_sys, in_dy, out_dy, 5);

        Mat2 J;
        J.m[0][0] = static_cast<double>((out_dx[2] - out5[2]) / EPS);
        J.m[0][1] = static_cast<double>((out_dy[2] - out5[2]) / EPS);
        J.m[1][0] = static_cast<double>((out_dx[3] - out5[3]) / EPS);
        J.m[1][1] = static_cast<double>((out_dy[3] - out5[3]) / EPS);

        const double det = J.m[0][0] * J.m[1][1] - J.m[0][1] * J.m[1][0];
        if (std::fabs(det) < 1e-12)
            break;  // singular Jacobian — no update, return current estimate

        const Mat2 Jinv = mat2_inverse(J);
        pos[0] -= static_cast<float>(Jinv.m[0][0] * res0 + Jinv.m[0][1] * res1);
        pos[1] -= static_cast<float>(Jinv.m[1][0] * res0 + Jinv.m[1][1] * res1);
    }

    return Vec2{ static_cast<double>(pos[0]), static_cast<double>(pos[1]) };
}

// ---------------------------------------------------------------------------
// coc_radius — thin-lens circle-of-confusion radius (mm)
//
// Computes the geometric CoC radius for a sample at sample_depth_mm given a
// lens focused at focus_dist_mm, using the thin-lens approximation:
//
//   CoC = (aperture_diameter) * |sample_depth - focus_dist| / sample_depth
//
// where aperture_diameter = focal_length / fstop.
//
// Parameters:
//   focal_length_mm  — lens focal length (mm)
//   fstop            — lens f-stop (dimensionless)
//   focus_dist_mm    — distance to the focus plane from the lens (mm)
//   sample_depth_mm  — distance from the lens to the sample (mm)
//
// Returns 0 for samples at or behind the lens (sample_depth_mm < 1e-6).
// ---------------------------------------------------------------------------
inline float coc_radius(float focal_length_mm,
                        float fstop,
                        float focus_dist_mm,
                        float sample_depth_mm)
{
    if (sample_depth_mm < 1e-6f) return 0.0f;
    const float aperture_diam = focal_length_mm / fstop;
    const float delta = std::fabs(sample_depth_mm - focus_dist_mm);
    return aperture_diam * delta / sample_depth_mm;
}

// ---------------------------------------------------------------------------
// sphereToCs_full — full lentil-style tangent-frame sphere-to-camera-space
//
// Converts a point (x, y) on a spherical lens element with radius R and
// center offset along z into a 3D ray origin + direction in camera space.
// The tangent frame is constructed from the surface normal at (x, y).
//
// This is the full version of sphereToCs that returns both the origin on the
// sphere surface and the direction transformed by the tangent frame.
// The existing codebase sphereToCs only returns direction; this version is
// needed by the path-trace engine (S02) for ray-scene intersection.
//
// Parameters:
//   x, y        — position on the sphere (lens element coordinates, mm)
//   dx, dy      — input direction to be transformed (normalised 2D)
//   center      — z-offset of the sphere center (mm)
//   R           — sphere radius (mm, can be negative for concave surfaces)
//   ox,oy,oz    — [out] ray origin on sphere surface
//   odx,ody,odz — [out] ray direction in camera space
//
// Reference: lentil pota/src/lens.h:99 (sphereToCs)
// ---------------------------------------------------------------------------
// Direction-only sphere-to-camera-space conversion.
// Computes normalized direction from sphere surface point (x, y) on sphere
// of radius R to the sphere center. Used by the ray-trace loop.
inline void sphereToCs(float x, float y, float R,
                       float& out_x, float& out_y, float& out_z)
{
    const float r2 = x*x + y*y;
    const float disc = R*R - r2;
    if (disc < 0.0f) {
        out_x = x; out_y = y; out_z = 0.0f;
        return;
    }
    out_z = R - std::copysign(std::sqrt(disc), R);
    const float len = std::sqrt(x*x + y*y + out_z*out_z);
    if (len < 1e-9f) { out_x = 0; out_y = 0; out_z = 1; return; }
    out_x = x / len;
    out_y = y / len;
    out_z = out_z / len;
}

inline void sphereToCs_full(float x, float y, float dx, float dy,
                             float center, float R,
                             float& ox, float& oy, float& oz,
                             float& odx, float& ody, float& odz)
{
    // Normal on sphere surface
    const float nx = x / R;
    const float ny = y / R;
    const float nz_raw = R * R - x * x - y * y;
    const float nz = std::sqrt(std::max(0.0f, nz_raw)) / std::fabs(R);

    // Tangent frame: ex perpendicular to normal in xz-plane
    const float ex_len = std::sqrt(nz * nz + nx * nx);
    float ex_x, ex_y, ex_z;
    if (ex_len > 1e-12f) {
        ex_x =  nz / ex_len;
        ex_y =  0.0f;
        ex_z = -nx / ex_len;
    } else {
        ex_x = 1.0f; ex_y = 0.0f; ex_z = 0.0f;
    }

    // ey = cross(normal, ex)
    const float ey_x = ny * ex_z - nz * ex_y;
    const float ey_y = nz * ex_x - nx * ex_z;
    const float ey_z = nx * ex_y - ny * ex_x;

    // Input direction in local tangent space → camera space
    const float dz_local = std::sqrt(std::max(0.0f, 1.0f - dx * dx - dy * dy));
    odx = ex_x * dx + ey_x * dy + nx * dz_local;
    ody = ex_y * dx + ey_y * dy + ny * dz_local;
    odz = ex_z * dx + ey_z * dy + nz * dz_local;

    // Origin on sphere surface
    ox = x;
    oy = y;
    oz = nz * R + center;
}

// ---------------------------------------------------------------------------
// pt_sample_aperture — Newton aperture match for path-trace engine
//
// Given a sensor position and a target aperture point, finds the sensor
// direction (dx, dy) such that tracing through the polynomial system lands
// at (target_ax, target_ay) on the aperture plane.
//
// Uses 5 Newton iterations with finite-difference Jacobians, 1e-4 tolerance,
// 1e-4 FD epsilon, and 0.72 damping factor.
//
// The polynomial input convention is:
//   in5 = {sensor_x + dx*sensor_shift, sensor_y + dy*sensor_shift, dx, dy, lambda}
//   out5[0:1] = predicted aperture position
//
// Parameters:
//   sensor_x, sensor_y — sensor position (mm)
//   dx, dy             — [in/out] direction; initialised to 0 on entry,
//                         updated to converged direction on success
//   target_ax, target_ay — target aperture position (mm)
//   lambda             — wavelength (µm)
//   sensor_shift       — sensor shift for focus (mm)
//   poly_sys           — polynomial system (loaded .fit file)
//   max_deg            — max polynomial degree (-1 = use all terms)
//
// Returns: true if converged (residual < 1e-4), false if vignetted/singular
//
// Reference: lentil pota/src/lens.h:1272 (lens_pt_sample_aperture)
// ---------------------------------------------------------------------------
inline bool pt_sample_aperture(float sensor_x, float sensor_y,
                                float& dx, float& dy,
                                float target_ax, float target_ay,
                                float lambda,
                                float sensor_shift,
                                const poly_system_t* poly_sys,
                                int max_deg = -1)
{
    const int   MAX_ITER = 5;
    const float TOL      = 1e-4f;
    const float EPS      = 1e-4f;
    const float DAMP     = 0.72f;

    dx = 0.0f;
    dy = 0.0f;

    for (int k = 0; k < MAX_ITER; ++k) {
        // Shifted sensor position
        const float sx = sensor_x + dx * sensor_shift;
        const float sy = sensor_y + dy * sensor_shift;

        // Evaluate polynomial: sensor → aperture
        float in5[5]  = { sx, sy, dx, dy, lambda };
        float out5[5] = {};
        poly_system_evaluate(poly_sys, in5, out5, 5, max_deg);

        // Residual: predicted aperture - target aperture
        const float r0 = out5[0] - target_ax;
        const float r1 = out5[1] - target_ay;

        if (r0 * r0 + r1 * r1 < TOL * TOL)
            return true;

        // Finite-difference Jacobian d(out[0:1]) / d(dx, dy)
        // Perturb dx
        float in_pdx[5] = { sensor_x + (dx + EPS) * sensor_shift, sy,
                             dx + EPS, dy, lambda };
        float out_pdx[5] = {};
        poly_system_evaluate(poly_sys, in_pdx, out_pdx, 5, max_deg);

        // Perturb dy
        float in_pdy[5] = { sx, sensor_y + (dy + EPS) * sensor_shift,
                             dx, dy + EPS, lambda };
        float out_pdy[5] = {};
        poly_system_evaluate(poly_sys, in_pdy, out_pdy, 5, max_deg);

        Mat2 J;
        J.m[0][0] = static_cast<double>((out_pdx[0] - out5[0]) / EPS);
        J.m[0][1] = static_cast<double>((out_pdy[0] - out5[0]) / EPS);
        J.m[1][0] = static_cast<double>((out_pdx[1] - out5[1]) / EPS);
        J.m[1][1] = static_cast<double>((out_pdy[1] - out5[1]) / EPS);

        const double det = J.m[0][0] * J.m[1][1] - J.m[0][1] * J.m[1][0];
        if (std::fabs(det) < 1e-12)
            return false;  // singular Jacobian — vignetted ray

        const Mat2 Jinv = mat2_inverse(J);

        // Newton step with damping
        dx -= DAMP * static_cast<float>(Jinv.m[0][0] * r0 + Jinv.m[0][1] * r1);
        dy -= DAMP * static_cast<float>(Jinv.m[1][0] * r0 + Jinv.m[1][1] * r1);
    }

    return false;  // did not converge in MAX_ITER iterations
}

// ---------------------------------------------------------------------------
// logarithmic_focus_search — find sensor_shift for a target focus distance
//
// Brute-force searches 20001 logarithmically-spaced sensor_shift values in
// [-45mm, +45mm] (quadratic spacing: dense near 0, sparse at extremes).
// For each shift, traces a center ray through pt_sample_aperture → poly eval
// → sphereToCs_full, intersects the optical axis, and picks the shift whose
// resulting focus distance best matches focal_distance_mm.
//
// Parameters:
//   focal_distance_mm            — desired focus distance (mm)
//   sensor_x, sensor_y           — center pixel sensor position (typically 0)
//   lambda                       — wavelength (µm)
//   poly_sys                     — polynomial system
//   outer_pupil_curvature_radius — curvature radius of outer pupil (mm)
//   outer_pupil_center           — z-offset of outer pupil center (mm)
//   aperture_housing_radius      — aperture housing radius (mm, for bounds)
//   max_deg                      — max polynomial degree (-1 = all)
//
// Returns: best sensor_shift (mm) that focuses at focal_distance_mm
//
// Reference: lentil pota/src/lens.h:395 (logarithmic_focus_search)
// ---------------------------------------------------------------------------
inline float logarithmic_focus_search(float focal_distance_mm,
                                       float sensor_x, float sensor_y,
                                       float lambda,
                                       const poly_system_t* poly_sys,
                                       float outer_pupil_curvature_radius,
                                       float outer_pupil_center,
                                       float aperture_housing_radius,
                                       int max_deg = -1)
{
    float best_shift = 0.0f;
    float best_error = 1e30f;

    for (int i = -10000; i <= 10000; ++i) {
        const float t = static_cast<float>(i) / 10000.0f;
        // Quadratic spacing: dense near 0, sparse at ±45mm
        const float sensor_shift = (t < 0.0f ? -1.0f : 1.0f) * t * t * 45.0f;

        // Try to trace a center ray to aperture center (0, 0)
        float dx = 0.0f, dy = 0.0f;
        if (!pt_sample_aperture(sensor_x, sensor_y, dx, dy,
                                0.0f, 0.0f, lambda, sensor_shift,
                                poly_sys, max_deg))
            continue;  // didn't converge — skip this shift

        // Forward evaluate to get outer pupil position + direction
        const float sx = sensor_x + dx * sensor_shift;
        const float sy = sensor_y + dy * sensor_shift;
        float in5[5]  = { sx, sy, dx, dy, lambda };
        float out5[5] = {};
        poly_system_evaluate(poly_sys, in5, out5, 5, max_deg);

        // Transform through outer pupil sphere
        float ray_ox, ray_oy, ray_oz, ray_dx, ray_dy, ray_dz;
        sphereToCs_full(out5[0], out5[1], out5[2], out5[3],
                        outer_pupil_center, outer_pupil_curvature_radius,
                        ray_ox, ray_oy, ray_oz,
                        ray_dx, ray_dy, ray_dz);

        // Focus distance: axial intersection for near-center ray
        if (std::fabs(ray_dz) < 1e-10f)
            continue;

        const float dist = std::fabs(ray_oz / ray_dz);
        const float error = std::fabs(dist - focal_distance_mm);

        if (error < best_error) {
            best_error = error;
            best_shift = sensor_shift;
        }
    }

    return best_shift;
}
