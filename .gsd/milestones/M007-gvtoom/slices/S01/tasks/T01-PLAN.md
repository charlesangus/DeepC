---
estimated_steps: 4
estimated_files: 2
skills_used: []
---

# T01: Add max_degree to poly_system_evaluate and sphereToCs to deepc_po_math.h

**Slice:** S01 — Shared infrastructure, max_degree knob, two-plugin scaffold
**Milestone:** M007-gvtoom

## Description

Modify the shared headers that both new plugins depend on. Add a `max_degree` parameter to `poly_system_evaluate` in `poly.h` with backward-compatible default, and add the `sphereToCs` utility function plus updated forward declaration to `deepc_po_math.h`.

## Steps

1. **Add `max_degree` parameter to `poly_system_evaluate` in `src/poly.h`:**
   - Change signature to: `inline void poly_system_evaluate(const poly_system_t* sys, const float* input, float* output, int num_out, int max_degree = -1)`
   - Inside the inner loop over terms (`for (int k = 0; k < p->num_terms; ++k)`), compute total degree as `int deg = 0; for (int d = 0; d < 5; ++d) deg += std::abs(p->term[k].exp[d]);` then `if (max_degree >= 0 && deg > max_degree) break;` (terms are sorted ascending by degree in .fit files, so break is correct)
   - Add the degree computation and break BEFORE the `poly_term_evaluate` call

2. **Update the forward declaration of `poly_system_evaluate` in `src/deepc_po_math.h`:**
   - Find the `#ifndef DEEPC_POLY_H` block that forward-declares `poly_system_evaluate`
   - Change the signature to include `int max_degree = -1` to match the new poly.h signature
   - This is the declaration inside the `#ifndef DEEPC_POLY_H` guard (around line 47)

3. **Add `sphereToCs` inline function to `src/deepc_po_math.h`:**
   - Add after the `coc_radius` function, before the closing of the file
   - Signature: `inline void sphereToCs(float x, float y, float R, float& out_x, float& out_y, float& out_z)`
   - Implementation:
     ```cpp
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
     ```
   - This converts a 2D point on a sphere of radius R to a unit 3D direction. Only needs to compile in S01; correctness validated in S03.

4. **Verify both files compile together** by checking that `grep -q 'max_degree' src/poly.h` and `grep -q 'sphereToCs' src/deepc_po_math.h` pass.

## Must-Haves

- [ ] `poly_system_evaluate` has `int max_degree = -1` parameter
- [ ] Inner loop breaks early when term degree exceeds max_degree (only when max_degree >= 0)
- [ ] Forward declaration in deepc_po_math.h matches the new signature
- [ ] `sphereToCs` function present in deepc_po_math.h with correct signature
- [ ] Existing `lt_newton_trace` calls continue to work (they omit max_degree, getting the default -1 = all terms)

## Verification

- `grep -q 'max_degree' src/poly.h` — parameter exists
- `grep -q 'max_degree.*=.*-1' src/poly.h` — default is -1
- `grep -q 'sphereToCs' src/deepc_po_math.h` — function exists
- `grep -q 'max_degree' src/deepc_po_math.h` — forward declaration updated

## Inputs

- `src/poly.h` — existing polynomial evaluator (add max_degree parameter)
- `src/deepc_po_math.h` — existing math header (update forward declaration, add sphereToCs)

## Expected Output

- `src/poly.h` — modified with max_degree parameter and early-exit logic
- `src/deepc_po_math.h` — modified with updated forward declaration and sphereToCs function
