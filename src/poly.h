// SPDX-License-Identifier: MIT
//
// poly.h — Vendored from lentil polynomial-optics/src/poly.h
// Source: https://github.com/hanatos/lentil  (MIT license)
//
// Polynomial lens system: binary-format reader, evaluator, and destructor.
// All functions are defined inline in this header — include it from exactly
// ONE translation unit to avoid ODR violations (the function bodies would be
// multiply defined).  In DeepC, only DeepCDefocusPO.cpp includes this header.
//
// Binary .fit file layout (little-endian, native float/int32):
//   For each of 5 polynomial outputs:
//     int32_t  num_terms          — number of terms in this polynomial
//     poly_term_t[num_terms]      — packed array of terms
//
// poly_term_t layout:
//     float    coeff              — term coefficient
//     int32_t  exp[5]             — exponents for the 5 input dimensions
//
// Evaluation:
//   input  : float[5]  (sensor_x, sensor_y, aperture_x, aperture_y, lambda)
//   output : float[5]  (aperture_x', aperture_y', sensor_x', sensor_y', lambda')
//
// ============================================================================

#pragma once

// Sentinel used by deepc_po_math.h to detect that poly_system_t is already
// defined via this header (so the forward declaration is suppressed).
#define DEEPC_POLY_H

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

// ---------------------------------------------------------------------------
// Data structures
// ---------------------------------------------------------------------------

typedef struct {
    float    coeff;
    int32_t  exp[5];
} poly_term_t;

typedef struct {
    int32_t     num_terms;
    poly_term_t* term;
} poly_t;

typedef struct poly_system_s {
    poly_t poly[5];
} poly_system_t;

// ---------------------------------------------------------------------------
// poly_pow — integer-exponent power helper
//
// Handles negative exponents (returns 1/x^|n|) and the zero-exponent case
// (returns 1.0 regardless of base, matching lentil behaviour).
// ---------------------------------------------------------------------------
inline float poly_pow(float x, int n)
{
    if (n == 0) return 1.0f;
    if (n < 0) {
        x = 1.0f / x;
        n = -n;
    }
    float result = 1.0f;
    for (int i = 0; i < n; ++i)
        result *= x;
    return result;
}

// ---------------------------------------------------------------------------
// poly_term_evaluate — evaluate a single polynomial term
//
// term(input) = coeff * product_i( input[i]^exp[i] )
// ---------------------------------------------------------------------------
inline float poly_term_evaluate(const poly_term_t* t, const float* input)
{
    float v = t->coeff;
    for (int i = 0; i < 5; ++i)
        v *= poly_pow(input[i], t->exp[i]);
    return v;
}

// ---------------------------------------------------------------------------
// poly_system_evaluate — evaluate all 5 polynomial outputs
//
// output[j] = sum_k( poly[j].term[k](input) )   for j in [0,num_out)
//
// num_out is clamped to [0,5]; callers typically pass 5.
// ---------------------------------------------------------------------------
inline void poly_system_evaluate(const poly_system_t* sys,
                                 const float* input,
                                 float* output,
                                 int num_out,
                                 int max_degree = -1)
{
    if (num_out < 0) num_out = 0;
    if (num_out > 5) num_out = 5;

    for (int j = 0; j < num_out; ++j) {
        const poly_t* p = &sys->poly[j];
        float acc = 0.0f;
        for (int k = 0; k < p->num_terms; ++k) {
            // Early exit: terms are sorted ascending by total degree in .fit
            // files, so once we exceed max_degree we can break.
            if (max_degree >= 0) {
                int deg = 0;
                for (int d = 0; d < 5; ++d)
                    deg += std::abs(p->term[k].exp[d]);
                if (deg > max_degree)
                    break;
            }
            acc += poly_term_evaluate(&p->term[k], input);
        }
        output[j] = acc;
    }
}

// ---------------------------------------------------------------------------
// poly_system_read — load a .fit binary file into a poly_system_t
//
// Returns 0 on success, 1 on any error (file not found, short read, alloc
// failure).  On error, any partial allocations are freed before returning.
// The caller is responsible for calling poly_system_destroy on a successful
// return value (== 0).
// ---------------------------------------------------------------------------
inline int poly_system_read(poly_system_t* sys, const char* filename)
{
    FILE* f = fopen(filename, "rb");
    if (!f) return 1;

    for (int j = 0; j < 5; ++j)
        sys->poly[j].term = nullptr;

    for (int j = 0; j < 5; ++j) {
        poly_t* p = &sys->poly[j];

        // Read number of terms
        if (fread(&p->num_terms, sizeof(int32_t), 1, f) != 1) {
            fclose(f);
            // Free already-allocated arrays
            for (int k = 0; k < j; ++k)
                free(sys->poly[k].term);
            return 1;
        }

        if (p->num_terms <= 0) {
            // Zero-term polynomial is valid; leave term as nullptr
            p->term = nullptr;
            continue;
        }

        p->term = static_cast<poly_term_t*>(
            malloc(static_cast<size_t>(p->num_terms) * sizeof(poly_term_t)));
        if (!p->term) {
            fclose(f);
            for (int k = 0; k < j; ++k)
                free(sys->poly[k].term);
            return 1;
        }

        const size_t n = static_cast<size_t>(p->num_terms);
        if (fread(p->term, sizeof(poly_term_t), n, f) != n) {
            free(p->term);
            p->term = nullptr;
            fclose(f);
            for (int k = 0; k < j; ++k)
                free(sys->poly[k].term);
            return 1;
        }
    }

    fclose(f);
    return 0;
}

// ---------------------------------------------------------------------------
// poly_system_destroy — release all term arrays
//
// Safe to call if poly_system_read returned non-zero (nullptr term pointers
// are skipped).  Safe to call multiple times if all term pointers are set to
// nullptr after the first call (the caller's responsibility).
// ---------------------------------------------------------------------------
inline void poly_system_destroy(poly_system_t* sys)
{
    for (int j = 0; j < 5; ++j) {
        free(sys->poly[j].term);
        sys->poly[j].term = nullptr;
    }
}
