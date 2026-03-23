---
estimated_steps: 4
estimated_files: 2
skills_used:
  - review
---

# T01: Vendor poly.h and write deepc_po_math.h stubs

**Slice:** S01 — Iop Scaffold, Poly Loader, and Architecture Decision
**Milestone:** M006

## Description

Two header files must exist before `DeepCDefocusPO.cpp` can be written:

1. `src/poly.h` — an exact verbatim copy of the lentil `polynomial-optics/src/poly.h` (MIT licensed). This is the runtime polynomial evaluation engine. It is pure C, header-only, uses `malloc`/`fread`, and defines `poly_system_t`, `poly_system_read`, `poly_system_evaluate`, and `poly_system_destroy`. It is included only in `DeepCDefocusPO.cpp` (one TU only) to avoid ODR violations from the defined-in-header functions.

2. `src/deepc_po_math.h` — a standalone C++ header (no include of `poly.h`) that defines:
   - `Mat2` struct and `mat2_inverse` inline function (hand-rolled 2×2 inverse — replaces Eigen)
   - `Vec2` struct
   - `lt_newton_trace` function (stub body only — returns `{0.0, 0.0}`; full Newton iteration in S02)
   - `coc_radius` inline formula (thin-lens circle-of-confusion radius)

`deepc_po_math.h` receives `const poly_system_t*` by pointer only — it must NOT include `poly.h` to avoid the ODR problem.

## Steps

1. Write `src/poly.h` with `#pragma once` guard. Define `poly_term_t` (struct with `float coeff` and `int32_t exp[5]`), `poly_t` (struct with `int32_t num_terms` and `poly_term_t* term`), `poly_system_t` (struct with `poly_t poly[5]`). Implement `poly_system_read(poly_system_t*, const char*)` using `fopen`/`fread`/`malloc` — reads 5 polynomials sequentially: for each, reads `int32_t num_terms`, `malloc`s `term` array, reads `num_terms` × `poly_term_t` entries. Returns 0 on success, 1 on error. Implement `poly_system_evaluate(const poly_system_t*, const float*, float*, int)` — evaluates each of 5 poly outputs from a 5-element input vector using nested loops over terms and exponents (raise each input element to its exponent power using `pow`, multiply by coeff, accumulate). Implement `poly_pow(float, int)` as an inline helper. Implement `poly_term_evaluate(const poly_term_t*, const float*)`. Implement `poly_system_destroy(poly_system_t*)` — frees each `poly[i].term`. All implemented inline in the header (matching lentil style — this is what causes the ODR concern if included in multiple TUs).

2. Write `src/deepc_po_math.h` with `#pragma once` guard. Include `<cmath>` and `<cstdint>`. Forward-declare `struct poly_system_t;` (do not include `poly.h`). Define:
   ```cpp
   struct Mat2 { double m[2][2]; };
   inline Mat2 mat2_inverse(const Mat2& J) {
       const double invdet = 1.0 / (J.m[0][0]*J.m[1][1] - J.m[0][1]*J.m[1][0]);
       return { {{ J.m[1][1]*invdet, -J.m[0][1]*invdet },
                  {-J.m[1][0]*invdet,  J.m[0][0]*invdet }} };
   }
   struct Vec2 { double x, y; };
   Vec2 lt_newton_trace(const float sensor_pos[2], const float aperture_pos[2],
                        float lambda_um, const poly_system_t* poly_sys_outer);
   // inline stub body: return Vec2{0.0, 0.0};
   inline float coc_radius(float focal_length_mm, float fstop,
                            float focus_dist_mm, float sample_depth_mm) {
       const float aperture_diam = focal_length_mm / fstop;
       const float delta = std::fabs(sample_depth_mm - focus_dist_mm);
       if (sample_depth_mm < 1e-6f) return 0.0f;
       return aperture_diam * delta / sample_depth_mm;
   }
   ```

3. Verify grep contracts pass (see Verification section).

4. No CMake changes in this task — `poly.h` and `deepc_po_math.h` are headers; they are not compiled separately.

## Must-Haves

- [ ] `src/poly.h` exists with `#pragma once`; contains `poly_system_t`, `poly_system_read`, `poly_system_evaluate`, `poly_system_destroy`
- [ ] `src/deepc_po_math.h` exists with `#pragma once`; contains `mat2_inverse`, `lt_newton_trace`, `coc_radius`
- [ ] `deepc_po_math.h` does NOT include `poly.h` — it only forward-declares `struct poly_system_t`
- [ ] `poly_pow` and `poly_term_evaluate` defined in `poly.h` (matching lentil style)
- [ ] `lt_newton_trace` has a stub body that compiles (returns `Vec2{0.0, 0.0}`)

## Verification

```bash
grep -q 'poly_system_read'   src/poly.h
grep -q 'poly_system_destroy' src/poly.h
grep -q 'poly_system_evaluate' src/poly.h
grep -q 'poly_system_t'      src/poly.h
grep -q 'lt_newton_trace'    src/deepc_po_math.h
grep -q 'mat2_inverse'       src/deepc_po_math.h
grep -q 'coc_radius'         src/deepc_po_math.h
# confirm no poly.h include in deepc_po_math.h:
! grep -q '#include.*poly' src/deepc_po_math.h
```

## Observability Impact

- **Compile-time signal:** `poly.h` and `deepc_po_math.h` are included in `DeepCDefocusPO.cpp` (T02). Any ODR violation, missing type, or broken forward-declaration surfaces as a compiler error at the `g++ -fsyntax-only` step in `scripts/verify-s01-syntax.sh` — the exact error line is printed to stdout.
- **Failure state visible:** If `poly.h` is missing or malformed, T02's `#include "poly.h"` fails with `fatal error: poly.h: No such file or directory` — immediately pinpointing this task as the defect source.
- **Forward-declaration isolation:** `deepc_po_math.h`'s `struct poly_system_t;` forward-declaration is inspectable with `grep -n 'poly_system_t' src/deepc_po_math.h` — agents can verify the ODR firewall without compiling.
- **No runtime signals in this task** — both files are headers; runtime behavior (poly load/destroy, Newton trace) surfaces in T02's `_validate` and T04's docker build.

## Inputs

- `src/DeepCDepthBlur.cpp` — reference for Deep op C++ style conventions in this codebase (do not modify)
- `src/DeepSampleOptimizer.h` — reference for header-only style with no DDImage deps (do not modify)

## Expected Output

- `src/poly.h` — vendored lentil polynomial-optics header (MIT); defines the poly_system_t API
- `src/deepc_po_math.h` — standalone math header; Mat2, Vec2, lt_newton_trace stub, coc_radius
