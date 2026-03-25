---
id: T02
parent: S01
milestone: M008-y32v8w
provides:
  - sphereToCs_full — full tangent-frame sphere-to-camera-space conversion (origin + direction)
  - pt_sample_aperture — Newton aperture match with FD Jacobians (5 iters, 1e-4 tol, 0.72 damping)
  - logarithmic_focus_search — brute-force sensor_shift search over [-45mm, +45mm] quadratic spacing
key_files:
  - src/deepc_po_math.h
key_decisions:
  - Forward declaration of poly_system_evaluate updated to 5-arg signature with max_degree default
  - Added <algorithm> include for std::max (needed by sphereToCs_full tangent-frame clamp)
patterns_established:
  - Path-trace math functions follow same inline header-only pattern as existing lt_newton_trace
  - pt_sample_aperture perturbs both sensor position and direction simultaneously when computing FD Jacobian (sensor_shift couples dx/dy to shifted sensor pos)
observability_surfaces:
  - grep -c 'inline.*sphereToCs_full\|inline.*pt_sample_aperture\|inline.*logarithmic_focus_search' src/deepc_po_math.h returns 3
  - Syntax compilation with poly.h + deepc_po_math.h produces zero errors
duration: 8m
verification_result: passed
completed_at: 2026-03-25
blocker_discovered: false
---

# T02: Implement path-trace math functions in deepc_po_math.h

**Added sphereToCs_full, pt_sample_aperture, and logarithmic_focus_search inline functions to deepc_po_math.h for S02 path-trace engine consumption.**

## What Happened

Implemented three new inline functions in `src/deepc_po_math.h` translating lentil's Eigen/C++ algorithms into our standalone math primitives:

1. **sphereToCs_full** — Full tangent-frame construction from a sphere surface point. Computes surface normal, builds orthonormal ex/ey frame via cross products, transforms input direction into camera space, and returns both the 3D origin on the sphere and the transformed direction. Handles negative R (concave surfaces) via fabs(R) in the normal z-component.

2. **pt_sample_aperture** — Newton solver finding the sensor direction (dx, dy) that maps to a target aperture point through the polynomial system. Uses 5 iterations, 1e-4 convergence tolerance, 1e-4 FD epsilon for Jacobian computation, and 0.72 damping. The FD Jacobian correctly accounts for sensor_shift coupling (perturbing dx also shifts the sensor position).

3. **logarithmic_focus_search** — Brute-force search over 20001 quadratically-spaced sensor_shift values in [-45mm, +45mm]. For each shift, traces a center ray through pt_sample_aperture → poly_system_evaluate → sphereToCs_full, computes axial focus distance as |oz/odz|, and picks the shift minimizing error against the target focal distance.

Also updated the forward declaration of `poly_system_evaluate` to match the 5-arg signature with `max_degree` default parameter, and added `<algorithm>` include for `std::max`.

## Verification

All three functions confirmed present via grep. Header compiles cleanly both standalone (forward-decl path) and with poly.h included first (actual usage pattern). The slice-level verify script still fails on unrelated T03 contracts (expected — T03 updates the script).

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `grep -q 'sphereToCs_full' src/deepc_po_math.h` | 0 | ✅ pass | <1s |
| 2 | `grep -q 'pt_sample_aperture' src/deepc_po_math.h` | 0 | ✅ pass | <1s |
| 3 | `grep -q 'logarithmic_focus_search' src/deepc_po_math.h` | 0 | ✅ pass | <1s |
| 4 | `g++ -std=c++14 -fsyntax-only -I src src/deepc_po_math.h` | 0 | ✅ pass (warnings only: #pragma once, undefined forward-decl) | <1s |
| 5 | `echo '#include "poly.h"\n#include "deepc_po_math.h"\nint main(){}' \| g++ -std=c++14 -fsyntax-only -I src -x c++ -` | 0 | ✅ pass (zero diagnostics) | <1s |
| 6 | `bash scripts/verify-s01-syntax.sh` | 1 | ⏳ partial (T03 updates script) | 3s |

## Diagnostics

- Confirm all three functions: `grep -n 'inline.*sphereToCs_full\|inline.*pt_sample_aperture\|inline.*logarithmic_focus_search' src/deepc_po_math.h`
- Syntax check with poly.h: `echo '#include "poly.h"' '#include "deepc_po_math.h"' 'int main(){}' | g++ -std=c++14 -fsyntax-only -I src -x c++ -`
- Check function count: `grep -c '^inline' src/deepc_po_math.h` (should be 11 including forward-decl)

## Deviations

- Added `#include <algorithm>` — not mentioned in plan but required for `std::max` used in sphereToCs_full's normal clamping.
- Updated forward declaration of `poly_system_evaluate` from 4-arg to 5-arg with `max_degree = -1` default — necessary since the new functions pass max_deg through to poly_system_evaluate.

## Known Issues

- `scripts/verify-s01-syntax.sh` still references old `DeepCDefocusPO.cpp` — T03 will update it.
- `logarithmic_focus_search` accepts `aperture_housing_radius` parameter for API compatibility with lentil but does not currently use it (the brute-force search doesn't clip to housing bounds). S02 may add bounds checking if needed.

## Files Created/Modified

- `src/deepc_po_math.h` — Added sphereToCs_full, pt_sample_aperture, logarithmic_focus_search; updated forward declaration to 5-arg poly_system_evaluate; added `<algorithm>` include
- `.gsd/milestones/M008-y32v8w/slices/S01/tasks/T02-PLAN.md` — Added Observability Impact section (pre-flight fix)
- `.gsd/milestones/M008-y32v8w/slices/S01/S01-PLAN.md` — Marked T02 done; added diagnostic failure-path verification step
