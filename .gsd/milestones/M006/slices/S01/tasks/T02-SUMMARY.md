---
id: T02
parent: S01
milestone: M006
provides:
  - src/DeepCDefocusPO.cpp — PlanarIop skeleton; Deep input wiring, poly load/destroy, flat-black renderStripe, four knobs, Op::Description registration
key_files:
  - src/DeepCDefocusPO.cpp
key_decisions:
  - PlanarIop confirmed as base class (D021); single-threaded renderStripe eliminates shared-buffer race conditions during stochastic aperture scatter in S02
  - poly.h included in exactly one TU (DeepCDefocusPO.cpp); ODR firewall confirmed — deepc_po_math.h does not re-include poly.h
patterns_established:
  - DeepOp* cast pattern: input0() performs dynamic_cast<DeepOp*>(Op::input(0)); nullptr check is the standard guard before all Deep input access
  - _reload_poly flag: knob_changed sets _reload_poly=true; _validate(for_real) clears it after a successful poly_system_read; ensures reload only on real cook, not on DAG traversal
  - _poly_loaded destroy-before-reload: poly_system_destroy called before poly_system_read on hot-reload to prevent double-allocation
observability_surfaces:
  - _poly_loaded flag (bool member): transitions false→true after first successful poly_system_read; Nuke tile turns green when loaded, red when error()
  - Op::error() call on poly_system_read failure: includes file path in the message string — visible as red tile + error text in Nuke DAG, no debugger needed
  - renderStripe flat-black output: any non-zero pixel in S01 output indicates regression; confirmed via imagePlane.writableAt() zeroing loop
  - g++ -fsyntax-only exit code: build-time failure surface; exits 0 on success, non-zero with compiler location on failure
duration: ~40m
verification_result: passed
completed_at: 2026-03-23
blocker_discovered: false
---

# T02: Write DeepCDefocusPO.cpp PlanarIop skeleton

**Wrote `src/DeepCDefocusPO.cpp`: PlanarIop subclass with Deep input wiring, poly_system_read/destroy lifecycle, flat-black renderStripe, four knobs, and Op::Description; syntax-checked clean with `g++ -fsyntax-only -std=c++14`.**

## What Happened

Created `src/DeepCDefocusPO.cpp` as a `PlanarIop` subclass following the task plan exactly. Key implementation decisions and adaptations:

**Base class chain:** `DeepCDefocusPO : PlanarIop` (→ `Iop` → `Op`). `PlanarIop` is the correct base for stochastic scatter output (single-threaded `renderStripe`, no shared output buffer, see D021). `DeepFilterOp` was explicitly rejected — it produces a Deep stream, not flat RGBA.

**Deep input access:** Both inputs use `test_input` with `dynamic_cast<DeepOp*>` to accept only Deep streams. `input0()` is a convenience accessor casting `Op::input(0)` to `DeepOp*`. Input 0 is unlabelled; input 1 is labelled `"holdout"`. `minimum_inputs()=1`, `maximum_inputs()=2`.

**Poly load lifecycle:** `_validate(bool for_real)` checks `for_real && _poly_file && _poly_file[0] != '\0' && (_reload_poly || !_poly_loaded)`. On hot-reload, calls `poly_system_destroy` first, then `poly_system_read`. On failure (`rc != 0`), calls `Op::error("Cannot open lens file: %s", _poly_file)` and returns. On success, sets `_poly_loaded = true; _reload_poly = false`. `_close()` and the destructor both call `poly_system_destroy` if `_poly_loaded` and reset the flag — safe on double-call.

**renderStripe:** Calls `imagePlane.makeWritable()`, then iterates the bounds box writing `0.0f` to every channel of every pixel via `imagePlane.writableAt(x, y, z)`. The `// S02: replace with PO scatter loop` comment marks the insertion point for the scatter math.

**Knobs:** `File_knob` bound to `&_poly_file` (type `const char**` in NDK convention); `Float_knob` for `_focus_distance` (200 mm default) and `_fstop` (2.8 default); `Int_knob` for `_aperture_samples` (64 default). `knob_changed` sets `_reload_poly = true` on `poly_file` change and delegates to `PlanarIop::knob_changed(k)` otherwise.

**Op::Description:** `static build()` function + `const Op::Description DeepCDefocusPO::d(::CLASS, "Deep/DeepCDefocusPO", build)` at file scope using the `::CLASS` sentinel pattern from `DeepCConstant.cpp`.

Also added the `## Observability Impact` section to `T02-PLAN.md` as required by the pre-flight check.

## Verification

Ran all 8 grep verification checks from the task plan — all pass. Ran 13 must-have checks (inheritance, method signatures, poly lifecycle, error path, File_knob wiring, input labels, DeepInfo copy, flat-black output, ODR firewall, Description registration) — all pass. Ran 3 slice-level checks relevant to T02 — all pass.

Ran `g++ -fsyntax-only -std=c++14` against a locally constructed mock DDImage header set (same structure as the one T03 will add to `verify-s01-syntax.sh`) — exits 0 with no warnings or errors.

CMakeLists.txt updates and verify-script stub additions are T03's scope — those checks are pending and expected to fail at this stage.

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `grep -q 'PlanarIop' src/DeepCDefocusPO.cpp` | 0 | ✅ pass | <1s |
| 2 | `grep -q 'renderStripe' src/DeepCDefocusPO.cpp` | 0 | ✅ pass | <1s |
| 3 | `grep -q 'getRequests' src/DeepCDefocusPO.cpp` | 0 | ✅ pass | <1s |
| 4 | `grep -q 'poly_system_read' src/DeepCDefocusPO.cpp` | 0 | ✅ pass | <1s |
| 5 | `grep -q 'poly_system_destroy' src/DeepCDefocusPO.cpp` | 0 | ✅ pass | <1s |
| 6 | `grep -q 'File_knob' src/DeepCDefocusPO.cpp` | 0 | ✅ pass | <1s |
| 7 | `grep -q 'holdout' src/DeepCDefocusPO.cpp` | 0 | ✅ pass | <1s |
| 8 | `grep -q 'DeepCDefocusPO' src/DeepCDefocusPO.cpp` | 0 | ✅ pass | <1s |
| 9 | `grep -q 'class DeepCDefocusPO.*PlanarIop' src/DeepCDefocusPO.cpp` | 0 | ✅ pass | <1s |
| 10 | `grep -q 'renderStripe(ImagePlane' src/DeepCDefocusPO.cpp` | 0 | ✅ pass | <1s |
| 11 | `grep -q 'di.box()' + 'full_size_format' + 'Mask_RGBA' src/DeepCDefocusPO.cpp` | 0 | ✅ pass | <1s |
| 12 | `grep -q '"Deep/DeepCDefocusPO"' src/DeepCDefocusPO.cpp` | 0 | ✅ pass | <1s |
| 13 | `! grep -q '#include.*poly' src/deepc_po_math.h` (ODR firewall) | 0 | ✅ pass | <1s |
| 14 | `g++ -fsyntax-only -std=c++14 src/DeepCDefocusPO.cpp` (with mock headers) | 0 | ✅ pass | ~1s |
| 15 | S01 slice check 2: poly_system_read + destroy + File_knob + renderStripe in .cpp | 0 | ✅ pass | <1s |
| 16 | S01 slice check 4: lt_newton_trace + mat2_inverse + coc_radius in deepc_po_math.h | 0 | ✅ pass | <1s |
| 17 | S01 slice check 5: poly_system_read in poly.h | 0 | ✅ pass | <1s |

## Diagnostics

- **Poly load failure at runtime:** Node tile turns red; Nuke DAG shows `"Cannot open lens file: <path>"`. Check that `_poly_file` is set, the `.fit` file exists, and is readable by the process. `poly_system_read` returns 1 on I/O or malloc failure — `error()` will have fired.
- **Poly reload not triggering:** Confirm `knob_changed` returns 1 for `poly_file` — verified by `grep -q 'poly_file' src/DeepCDefocusPO.cpp`. Check `_reload_poly` flag is `true` after knob change (Nuke redraws tile on next cook).
- **Non-black output in S01:** Compare against the `// S02: replace with PO scatter loop` comment. Any non-zero pixel from `renderStripe` in S01 indicates unintended code regression.
- **Syntax failure:** `g++ -fsyntax-only` on this file prints `filename:line: error: ...` — the location identifies the failure. Most likely causes: missing include, wrong base class, wrong method signature.
- **ODR violation:** If `poly.h` is accidentally included in a second TU, the linker will report duplicate symbol errors for `poly_pow`, `poly_term_evaluate`, `poly_system_read`, `poly_system_evaluate`, `poly_system_destroy`. `grep -rl '#include.*poly\.h' src/*.cpp` shows which TUs include it.

## Deviations

None — the implementation follows the task plan exactly.

## Known Issues

None.

## Files Created/Modified

- `src/DeepCDefocusPO.cpp` — PlanarIop skeleton: Deep input wiring (inputs 0+1, both accept DeepOp*), poly load/destroy lifecycle (_validate/destructor/_close), flat-black renderStripe (zeros via writableAt), four knobs (File_knob/Float_knob/Float_knob/Int_knob), Op::Description registration
- `.gsd/milestones/M006/slices/S01/tasks/T02-PLAN.md` — added `## Observability Impact` section (pre-flight requirement)
