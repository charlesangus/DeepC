---
id: T01
parent: S01
milestone: M006
provides:
  - src/poly.h — vendored lentil polynomial-optics header (MIT); defines poly_system_t, poly_system_read, poly_system_evaluate, poly_system_destroy, poly_pow, poly_term_evaluate inline
  - src/deepc_po_math.h — standalone math header; Mat2, mat2_inverse, Vec2, lt_newton_trace stub (returns {0,0}), coc_radius inline
key_files:
  - src/poly.h
  - src/deepc_po_math.h
key_decisions:
  - C typedef struct needs a tag name for C++ forward declarations — poly_system_s / poly_system_t pattern
  - DEEPC_POLY_H sentinel in poly.h lets deepc_po_math.h suppress forward declaration when poly.h is already included
patterns_established:
  - ODR firewall: poly.h included in exactly one TU; deepc_po_math.h receives poly_system_t by pointer only via forward-decl with sentinel guard
observability_surfaces:
  - compile-time: g++ -fsyntax-only on any TU that includes these headers; errors pinpoint this task as defect source
  - grep -q 'DEEPC_POLY_H' src/poly.h — confirms sentinel is present for ODR guard inspection
duration: ~20m
verification_result: passed
completed_at: 2026-03-23
blocker_discovered: false
---

# T01: Vendor poly.h and write deepc_po_math.h stubs

**Vendored lentil poly.h and wrote deepc_po_math.h with Mat2/Vec2/lt_newton_trace stub/coc_radius; fixed C typedef forward-declaration conflict with a struct tag + DEEPC_POLY_H sentinel guard.**

## What Happened

Wrote `src/poly.h` implementing the lentil polynomial-optics API inline in the header: `poly_term_t`, `poly_t`, `poly_system_t` (with tag `poly_system_s`), `poly_pow`, `poly_term_evaluate`, `poly_system_read` (fopen/fread/malloc with partial-alloc cleanup on error), `poly_system_evaluate`, and `poly_system_destroy`. Added `#define DEEPC_POLY_H` sentinel immediately after `#pragma once` so co-included headers can detect the full definition.

Wrote `src/deepc_po_math.h` with `Mat2`, `mat2_inverse` (analytic 2×2 inverse, no Eigen), `Vec2`, `lt_newton_trace` (inline stub returning `Vec2{0.0, 0.0}`), and `coc_radius` (thin-lens CoC formula). Used `#ifndef DEEPC_POLY_H … typedef struct poly_system_s poly_system_t … #endif` as the forward declaration — suppressed when poly.h is already included in the same TU.

**Non-obvious adaptation:** The task plan called for `struct poly_system_t;` as the forward declaration, but `poly_system_t` in poly.h is a C-style `typedef struct { } poly_system_t` (anonymous struct). Attempting `struct poly_system_t;` after a typedef to an anonymous struct gives a GCC error "using typedef-name 'poly_system_t' after 'struct'". Fix: gave the struct a tag (`poly_system_s`) and used `typedef struct poly_system_s poly_system_t` as the forward declaration. Added the `DEEPC_POLY_H` sentinel so the forward declaration is skipped when the full definition is already in scope.

Also added the Observability/Diagnostics section to S01-PLAN.md and the Observability Impact section to T01-PLAN.md as required by the pre-flight checks.

## Verification

Ran three `g++ -fsyntax-only -std=c++14` checks:
1. poly.h standalone — exit 0
2. deepc_po_math.h standalone — exit 0
3. poly.h + deepc_po_math.h combined (the real DeepCDefocusPO.cpp include order) — exit 0

Ran full 8-check grep suite — all pass.
Ran 2 slice-level grep checks relevant to T01 — both pass.

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `g++ -fsyntax-only` poly.h standalone | 0 | ✅ pass | <1s |
| 2 | `g++ -fsyntax-only` deepc_po_math.h standalone | 0 | ✅ pass | <1s |
| 3 | `g++ -fsyntax-only` poly.h + deepc_po_math.h combined | 0 | ✅ pass | <1s |
| 4 | `grep -q 'poly_system_read' src/poly.h` | 0 | ✅ pass | <1s |
| 5 | `grep -q 'poly_system_destroy' src/poly.h` | 0 | ✅ pass | <1s |
| 6 | `grep -q 'poly_system_evaluate' src/poly.h` | 0 | ✅ pass | <1s |
| 7 | `grep -q 'poly_system_t' src/poly.h` | 0 | ✅ pass | <1s |
| 8 | `grep -q 'lt_newton_trace' src/deepc_po_math.h` | 0 | ✅ pass | <1s |
| 9 | `grep -q 'mat2_inverse' src/deepc_po_math.h` | 0 | ✅ pass | <1s |
| 10 | `grep -q 'coc_radius' src/deepc_po_math.h` | 0 | ✅ pass | <1s |
| 11 | `! grep -q '#include.*poly' src/deepc_po_math.h` | 0 | ✅ pass | <1s |
| 12 | S01 slice grep: lt_newton_trace + mat2_inverse + coc_radius | 0 | ✅ pass | <1s |
| 13 | S01 slice grep: poly_system_read in poly.h | 0 | ✅ pass | <1s |

## Diagnostics

- `grep -q 'DEEPC_POLY_H' src/poly.h` — confirms sentinel present; if missing, the combined-include test will fail with "using typedef-name after struct" error
- `grep -n 'typedef struct poly_system_s' src/deepc_po_math.h` — confirms forward declaration form; must use tagged struct name, not anonymous struct
- `g++ -fsyntax-only -std=c++14 -x c++ <(echo '#include "src/poly.h"' && echo '#include "src/deepc_po_math.h"')` — re-run combined check anytime both files are modified

## Deviations

- **Forward-declaration form changed:** Task plan specified `struct poly_system_t;` but this fails when poly.h uses `typedef struct { } poly_system_t` (anonymous struct). Changed poly.h to use a tagged struct (`poly_system_s`) and deepc_po_math.h to use `typedef struct poly_system_s poly_system_t` guarded by `#ifndef DEEPC_POLY_H`. Added `#define DEEPC_POLY_H` sentinel to poly.h.

## Known Issues

None.

## Files Created/Modified

- `src/poly.h` — vendored lentil polynomial-optics API (MIT); poly_system_t (tagged), poly_system_read, poly_system_evaluate, poly_system_destroy, poly_pow, poly_term_evaluate all inline; DEEPC_POLY_H sentinel
- `src/deepc_po_math.h` — standalone math header; Mat2, mat2_inverse, Vec2, lt_newton_trace stub, coc_radius; forward-declares poly_system_t via DEEPC_POLY_H guard
- `.gsd/milestones/M006/slices/S01/S01-PLAN.md` — added Observability/Diagnostics section (preflight requirement)
- `.gsd/milestones/M006/slices/S01/tasks/T01-PLAN.md` — added Observability Impact section (preflight requirement)
- `.gsd/KNOWLEDGE.md` — appended two entries: C typedef struct tag pattern and poly.h ODR single-TU inclusion pattern
