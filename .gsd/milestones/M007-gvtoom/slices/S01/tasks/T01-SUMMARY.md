---
id: T01
parent: S01
milestone: M007-gvtoom
provides:
  - max_degree parameter on poly_system_evaluate with early-exit logic
  - sphereToCs utility function in deepc_po_math.h
  - updated forward declaration of poly_system_evaluate in deepc_po_math.h
key_files:
  - src/poly.h
  - src/deepc_po_math.h
key_decisions:
  - max_degree early-exit uses break (not continue) since .fit terms are sorted ascending by degree
  - sphereToCs returns degenerate (x,y,0) for out-of-sphere inputs instead of NaN
patterns_established:
  - backward-compatible default parameters (max_degree = -1 means all terms)
observability_surfaces:
  - max_degree truncation observable by comparing poly output with max_degree=-1 vs truncated value
  - sphereToCs safe fallback for out-of-range inputs (no NaN)
duration: 10m
verification_result: passed
completed_at: 2026-03-24
blocker_discovered: false
---

# T01: Add max_degree to poly_system_evaluate and sphereToCs to deepc_po_math.h

**Added max_degree early-exit parameter to poly_system_evaluate and sphereToCs sphere-to-direction utility to deepc_po_math.h**

## What Happened

Modified two shared headers that both new plugins (DeepCDefocusPOThin, DeepCDefocusPORay) depend on:

1. **poly.h**: Added `int max_degree = -1` parameter to `poly_system_evaluate`. When `max_degree >= 0`, the inner term loop computes total degree (sum of absolute exponents) and breaks early once it exceeds the limit. The default `-1` preserves existing behavior (all terms evaluated), so existing callers like `lt_newton_trace` are unaffected.

2. **deepc_po_math.h**: Updated the forward declaration of `poly_system_evaluate` inside the `#ifndef DEEPC_POLY_H` guard to include the new `max_degree = -1` parameter, matching the definition in poly.h.

3. **deepc_po_math.h**: Added `sphereToCs` inline function after `coc_radius`. It maps a 2D point on a sphere of radius R to a normalised 3D direction vector, with safe fallbacks for out-of-range and near-zero inputs.

Also added an Observability / Diagnostics section to the S01 slice plan per pre-flight requirements.

## Verification

All six task-level and two applicable slice-level checks pass:

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `grep -q 'max_degree' src/poly.h` | 0 | ✅ pass | <1s |
| 2 | `grep -q 'max_degree.*=.*-1' src/poly.h` | 0 | ✅ pass | <1s |
| 3 | `grep -q 'sphereToCs' src/deepc_po_math.h` | 0 | ✅ pass | <1s |
| 4 | `grep -q 'max_degree' src/deepc_po_math.h` | 0 | ✅ pass | <1s |
| 5 | `grep -q 'deg > max_degree' src/poly.h` | 0 | ✅ pass | <1s |
| 6 | `grep -q 'std::abs(p->term\[k\].exp\[d\])' src/poly.h` | 0 | ✅ pass | <1s |

Slice-level checks that depend on T02/T03 (new .cpp files, CMake, syntax script) are not yet applicable.

## Diagnostics

- `grep 'max_degree' src/poly.h` — verify parameter and early-exit logic
- `grep 'sphereToCs' src/deepc_po_math.h` — verify function presence
- Compile-time: both headers are syntax-checked as part of plugin compilation in T03's verify script

## Deviations

None.

## Known Issues

None.

## Files Created/Modified

- `src/poly.h` — added `max_degree` parameter with early-exit logic to `poly_system_evaluate`
- `src/deepc_po_math.h` — updated forward declaration with `max_degree`, added `sphereToCs` function
