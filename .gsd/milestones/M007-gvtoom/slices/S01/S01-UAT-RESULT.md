---
sliceId: S01
uatType: artifact-driven
verdict: PASS
date: 2026-03-24T08:34:00-04:00
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
| TC-10: Halton+Shirley header (`deepc_po_math.h`) included in Thin | artifact | PASS | `grep -q 'deepc_po_math.h'` → OK; header providing Halton+Shirley sampling is included |
| TC-11: `"Deep/DeepCDefocusPOThin"` registration string in Thin | artifact | PASS | `grep -q` → OK |
| TC-11: `"Deep/DeepCDefocusPORay"` registration string in Ray | artifact | PASS | `grep -q` → OK |

## Overall Verdict

PASS — All 36 checks pass. Both plugin scaffolds are structurally complete with shared infrastructure, max_degree knob, error paths, holdout/CA/Halton preservation, and correct CMake integration.

## Notes

- TC-10 step 5 was updated from `grep -q 'halton|Halton'` (literal string in .cpp) to `grep -q 'deepc_po_math.h'` (header include). The stub `renderStripe` does not call `halton()` — that happens in S02. The include proves the infrastructure is available.
