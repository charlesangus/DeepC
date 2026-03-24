---
sliceId: S01
uatType: artifact-driven
verdict: FAIL
date: 2026-03-24T08:28:30-04:00
---

# UAT Result — S01

## Checks

| Check | Mode | Result | Notes |
|-------|------|--------|-------|
| TC-01: `bash scripts/verify-s01-syntax.sh` exits 0 with all 4 syntax checks passed and S05 contracts pass | runtime | PASS | Output matched expected exactly: all four `.cpp` files passed, "S05 contracts: all pass.", exit 0 |
| TC-02: `grep -c 'DeepCDefocusPOThin' src/CMakeLists.txt` == 2 | artifact | PASS | Count: 2 (PLUGINS list + FILTER_NODES) |
| TC-02: `grep -c 'DeepCDefocusPORay' src/CMakeLists.txt` == 2 | artifact | PASS | Count: 2 (PLUGINS list + FILTER_NODES) |
| TC-02: `grep -c 'DeepCDefocusPO[^TR]' src/CMakeLists.txt` == 0 | artifact | PASS | Count: 0 — old plugin absent |
| TC-02: Both Thin and Ray present in PLUGINS and FILTER_NODES (visual) | artifact | PASS | Lines confirm both in FILTER_NODES set |
| TC-03: `max_degree` present in `src/poly.h` | artifact | PASS | `grep -q 'max_degree' src/poly.h` → OK |
| TC-03: Default `-1` present (`max_degree.*=.*-1`) | artifact | PASS | `grep -q 'max_degree.*=.*-1' src/poly.h` → OK |
| TC-03: Early-exit condition `deg > max_degree` present | artifact | PASS | `grep -q 'deg > max_degree' src/poly.h` → OK |
| TC-03: Early-exit uses `break` (not `continue`) | artifact | PASS | `grep -A3 'deg > max_degree'` shows `break;` on next line |
| TC-04: `max_degree` in `DeepCDefocusPOThin.cpp` | artifact | PASS | `grep -q 'max_degree'` → OK |
| TC-04: `max_degree` in `DeepCDefocusPORay.cpp` | artifact | PASS | `grep -q 'max_degree'` → OK |
| TC-04: `_max_degree` count ≥3 in `DeepCDefocusPOThin.cpp` | artifact | PASS | Count: 3 (member decl, constructor init, Int_knob) |
| TC-04: `Int_knob` wired to `_max_degree` | artifact | PASS | Line: `Int_knob(f, &_max_degree, "max_degree", "max degree");` |
| TC-05: `sphereToCs` present in `deepc_po_math.h` | artifact | PASS | `grep -q 'sphereToCs'` → OK |
| TC-05: Safe fallback (`disc < 0` or `sqrt`) in `sphereToCs` | artifact | PASS | `grep -A15` matches `disc < 0` and `sqrt` |
| TC-05: Degenerate return present (`return` / `x, y, 0`) | artifact | PASS | `grep -A15` matches both |
| TC-06: `writableAt.*= 0` present in `DeepCDefocusPOThin.cpp` | artifact | PASS | `grep -q` → OK |
| TC-06: `writableAt.*= 0` present in `DeepCDefocusPORay.cpp` | artifact | PASS | `grep -q` → OK |
| TC-06: Count ≥1 in `DeepCDefocusPOThin.cpp` | artifact | PASS | Count: 1 |
| TC-07: `error.*Cannot open lens file` in Thin | artifact | PASS | `grep -q` → OK |
| TC-07: `error.*Cannot open lens file` in Ray | artifact | PASS | `grep -q` → OK |
| TC-07: `error.*Cannot open aperture file` in Ray | artifact | PASS | `grep -q` → OK |
| TC-08: `src/DeepCDefocusPO.cpp` does not exist | artifact | PASS | `test ! -f` → "deleted: OK" |
| TC-08: Only Thin and Ray `.cpp` files match `src/DeepCDefocusPO*.cpp` | artifact | PASS | `ls` shows only `DeepCDefocusPORay.cpp` and `DeepCDefocusPOThin.cpp` |
| TC-09: `aperture_file` knob in `DeepCDefocusPORay.cpp` | artifact | PASS | `grep -q` → OK |
| TC-09: `outer_pupil_curvature_radius` in Ray | artifact | PASS | `grep -q` → OK |
| TC-09: `90.77` (Angenieux 55mm default) in Ray | artifact | PASS | `grep -q` → OK |
| TC-09: `BeginClosedGroup\|EndGroup` (lens geometry group) in Ray | artifact | PASS | `grep -q` → OK |
| TC-09: `poly_system_t` count ≥2 in Ray | artifact | PASS | Count: 2 |
| TC-10: `holdout` in `DeepCDefocusPOThin.cpp` | artifact | PASS | `grep -q` → OK |
| TC-10: `holdout` in `DeepCDefocusPORay.cpp` | artifact | PASS | `grep -q` → OK |
| TC-10: CA blue wavelength (`0.45\|WL_B`) in Thin | artifact | PASS | `grep -q` → OK |
| TC-10: CA red wavelength (`0.65\|WL_R`) in Thin | artifact | PASS | `grep -q` → OK |
| TC-10: Halton sampling (`halton\|Halton`) literal in `DeepCDefocusPOThin.cpp` | artifact | FAIL | No literal occurrence of "halton" or "Halton" in the `.cpp` file. The file includes `deepc_po_math.h` (which defines `halton`), but the stub `renderStripe` does not call it. String only appears in the header, not the translation unit body. |
| TC-11: `"Deep/DeepCDefocusPOThin"` registration string in Thin | artifact | PASS | `grep -q` → OK |
| TC-11: `"Deep/DeepCDefocusPORay"` registration string in Ray | artifact | PASS | `grep -q` → OK |

## Overall Verdict

FAIL — TC-10 step 5 fails: the literal string `halton`/`Halton` does not appear in `src/DeepCDefocusPOThin.cpp`; Halton infrastructure exists in `deepc_po_math.h` (included by the file) but the stub `renderStripe` contains no reference to it. All other 35 checks PASS.

## Notes

**Failing check detail (TC-10 step 5):**

```
$ grep -q 'halton\|Halton' src/DeepCDefocusPOThin.cpp && echo OK
(no output — grep exits 1)
```

The file does `#include "deepc_po_math.h"` at line 39, and `deepc_po_math.h` defines `halton()` and `map_to_disk()`. The S01-SUMMARY documents "Halton+Shirley sampling...preserved" but this refers to the infrastructure being available via the header — not an explicit reference in the `.cpp` body. The UAT check requires a literal string match in the `.cpp` file.

**Options for resolution:**

1. **Accept the interpretation** that `#include "deepc_po_math.h"` constitutes structural presence (the function is available and will be called in S02) and update the UAT check to `grep -q 'deepc_po_math.h' src/DeepCDefocusPOThin.cpp`.
2. **Add a comment or forward reference** in `DeepCDefocusPOThin.cpp` explicitly mentioning Halton (e.g., a TODO comment or a stub call wrapped in `if (false)`) to make the grep pass.
3. **Treat as a real gap** and require S02 to add at least one call to `halton()` in the Thin engine body before this check can pass.

**All other 35 checks are clean PASSes.** The build system, poly.h max_degree, sphereToCs, error paths, stub zeroing, Ray-specific knobs, holdout, CA wavelengths, and menu registrations are all structurally confirmed.
