---
id: T01
parent: S02
milestone: M006
provides:
  - halton and map_to_disk inline helpers in deepc_po_math.h
  - lt_newton_trace full Newton iteration (replaces stub)
  - renderStripe full PO scatter loop in DeepCDefocusPO.cpp
  - S02 grep contracts appended to verify-s01-syntax.sh
key_files:
  - src/deepc_po_math.h
  - src/DeepCDefocusPO.cpp
  - scripts/verify-s01-syntax.sh
key_decisions:
  - M_PI fallback define placed before includes so cmath on some platforms does not suppress it
  - poly_system_evaluate forward declaration guarded by DEEPC_POLY_H — only needed in header-only contexts; in the actual TU poly.h is already included first
  - Format mock width()/height() stubs added to verify-s01-syntax.sh so renderStripe compiles under the mock DDImage environment
patterns_established:
  - Newton iteration with finite-difference Jacobian, singular-det guard (|det|<1e-12), 20-iter max, TOL=1e-5
  - Halton(2,3) + Shirley concentric-disk sampling for aperture points
  - Normalised [-1,1] sensor coordinates throughout — avoids sensor-size metadata dependency
  - _poly_loaded fast-path zero guard before any Deep fetch
observability_surfaces:
  - _poly_loaded flag → black output when poly absent (inspectable in Nuke viewer)
  - deepEngine return-value check → silently skips stripe; upstream Deep error surfaces normally
  - Op::aborted() poll inside scatter loop → honours Nuke cancel without threads
  - Newton non-convergence: best estimate returned, no NaN/inf
duration: ~1h
verification_result: passed
completed_at: 2026-03-23
blocker_discovered: false
---

# T01: Implement Newton Trace Helpers and PO Scatter Loop

**Replaced lt_newton_trace stub and renderStripe zeroing loop with full polynomial-optics scatter engine; all S02 grep contracts pass and syntax check exits 0.**

## What Happened

S01 left two stubs: `lt_newton_trace` returning `{0,0}` in `deepc_po_math.h`, and `renderStripe` writing zeroes in `DeepCDefocusPO.cpp`. This task replaced both with production implementations.

**`src/deepc_po_math.h`:**
- Added `#ifndef M_PI / #define M_PI` fallback before includes.
- Added `halton(int index, int base)` — Halton low-discrepancy sequence using Van der Corput construction. Index is offset by +1 to skip the degenerate index-0 = 0 case.
- Added `map_to_disk(float u, float v, float& x, float& y)` — Peter Shirley concentric square-to-disk mapping with no area distortion. Handles the `a==b==0` degenerate case explicitly.
- Added a guarded forward declaration of `poly_system_evaluate` (suppressed when `DEEPC_POLY_H` is defined, i.e. when `poly.h` is already included).
- Replaced the `lt_newton_trace` stub with a 20-iteration Newton loop: evaluates the polynomial at `(pos, aperture, λ)` to get residuals `out5[2..3] - sensor_target`, builds a finite-difference Jacobian with step `EPS=1e-4`, guards against singular Jacobians (`|det|<1e-12`), and applies the `mat2_inverse` update. Convergence check uses `TOL=1e-5`.

**`src/DeepCDefocusPO.cpp`:**
- Added `#include <algorithm>` for `std::max`/`std::min`.
- Replaced the S01 zeroing `renderStripe` with the full scatter loop:
  - Fast-path zero branch when `!_poly_loaded || !input0()`.
  - Deep input fetch via `input0()->deepEngine(bounds, needed, deepPlane)`.
  - Buffer zero-fill before accumulation (correct for `+=` splat).
  - Normalised `[-1,1]` coordinate system: `half_w = fmt.width()*0.5`, `ap_radius = 1/fstop`.
  - Per-pixel → per-deep-sample → per-aperture-sample loop.
  - CoC-based early cull (performance guard only).
  - Halton(2,3) + Shirley disk sampling for aperture positions.
  - Per-channel Newton trace at `{0.45f, 0.55f, 0.65f}` μm (R, G, B).
  - Transmittance read from `poly_system_evaluate` output[4], clamped `[0,1]`.
  - Bounds-clamped `+=` splat with `1/N` weight × transmittance × alpha.
  - Alpha uses G-channel (index 1) landing position.
  - Out-of-stripe contributions discarded with seam-limitation comment.
  - `Op::aborted()` poll at top of pixel loop.

**`scripts/verify-s01-syntax.sh`:**
- Added `width()`/`height()` stub methods to the mock `Format` class (required now that `renderStripe` calls `fmt.width()`/`fmt.height()`).
- Appended S02 grep contracts block checking all 10 structural invariants.

## Verification

Ran `bash scripts/verify-s01-syntax.sh` — all three `.cpp` files pass syntax-only compilation and all 10 S02 grep contracts pass. Individual grep commands also confirmed clean.

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `bash scripts/verify-s01-syntax.sh` | 0 | ✅ pass | ~2s |
| 2 | `grep -q 'halton' src/deepc_po_math.h` | 0 | ✅ pass | <1s |
| 3 | `grep -q 'map_to_disk' src/deepc_po_math.h` | 0 | ✅ pass | <1s |
| 4 | `grep -q '0\.45f' src/DeepCDefocusPO.cpp` | 0 | ✅ pass | <1s |
| 5 | `grep -q '0\.55f' src/DeepCDefocusPO.cpp` | 0 | ✅ pass | <1s |
| 6 | `grep -q '0\.65f' src/DeepCDefocusPO.cpp` | 0 | ✅ pass | <1s |
| 7 | `grep -q 'deepEngine' src/DeepCDefocusPO.cpp` | 0 | ✅ pass | <1s |
| 8 | `grep -q 'lt_newton_trace' src/DeepCDefocusPO.cpp` | 0 | ✅ pass | <1s |
| 9 | `! grep -q 'S02: replace' src/DeepCDefocusPO.cpp` | 0 | ✅ pass | <1s |
| 10 | `grep -c 'TODO\|STUB\|S02: replace' src/DeepCDefocusPO.cpp` → 0 | 0 | ✅ pass | <1s |
| 11 | `grep -q 'mat2_inverse' src/deepc_po_math.h` | 0 | ✅ pass | <1s |

## Diagnostics

- **No poly loaded:** `_poly_loaded == false` → `renderStripe` immediately zeros the stripe and returns. Output is uniformly black in Nuke viewer.
- **Wrong poly path:** `poly_system_read` returns non-zero → `_validate` calls `error(...)` → red error node in Nuke DAG.
- **deepEngine failure:** stripe stays at zeroed accumulation buffer. Upstream error surfaces through the upstream Deep node's normal error mechanism.
- **Newton non-convergence or singular Jacobian:** best estimate returned; no NaN/inf. Bokeh disc may be slightly misplaced but render completes cleanly.
- **Seam artefacts:** contributions landing outside the current stripe are discarded. Visible as dark seams for large bokeh at stripe boundaries — documented with a comment in the scatter loop.
- **Inspect hooks:** `grep -c 'aborted\|_poly_loaded\|deepEngine' src/DeepCDefocusPO.cpp` should return ≥3.

## Deviations

- **Mock `Format` class needed `width()`/`height()` stubs.** The S01 mock `Format` was empty; `renderStripe` now calls `fmt.width()`/`fmt.height()` so the mock needed these added. Not mentioned in the task plan (which focused on source files), but necessary for the syntax check to pass.
- **`poly_system_evaluate` forward declaration added to `deepc_po_math.h`.** The task plan stated `<cmath>` is already present but didn't explicitly call out needing a forward declaration for `poly_system_evaluate`. Added a guarded declaration so the header compiles standalone (in contexts without `poly.h`). In `DeepCDefocusPO.cpp`, the `DEEPC_POLY_H` guard suppresses this declaration and the inline definition from `poly.h` is used directly.

## Known Issues

- Stripe-boundary seams for large bokeh discs: contributions landing outside the current stripe are discarded. This is an accepted limitation documented in both the code and the S02 research notes.

## Files Created/Modified

- `src/deepc_po_math.h` — Added M_PI guard, halton, map_to_disk, poly_system_evaluate forward decl, full Newton iteration in lt_newton_trace
- `src/DeepCDefocusPO.cpp` — Added #include <algorithm>, replaced renderStripe stub with full PO scatter loop
- `scripts/verify-s01-syntax.sh` — Added Format::width()/height() stubs to mock; appended S02 grep contract block
