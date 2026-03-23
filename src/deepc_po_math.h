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
                                 int num_out);
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
