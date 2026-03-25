---
sliceId: S01
uatType: artifact-driven
verdict: PASS
date: 2026-03-25T08:10:50-04:00
---

# UAT Result — S01

## Checks

| Check | Mode | Result | Notes |
|-------|------|--------|-------|
| TC-01: Full S01 verify script (gate test) | runtime | PASS | `bash scripts/verify-s01-syntax.sh` exits 0; all 4 source files compile ("Syntax check passed"); all 13 S01 contracts pass, 0 FAILs. Final line: "All S01 contracts passed. All syntax checks passed." |
| TC-02: _validate format fix in Thin | artifact | PASS | `grep -n 'info_\.format(' src/DeepCDefocusPOThin.cpp` → line 229: `info_.format(*di.format())`. Ordering check: comes after `info_.set(di.box())`. |
| TC-02: _validate format fix in Ray | artifact | PASS | `grep -n 'info_\.format(' src/DeepCDefocusPORay.cpp` → line 325: `info_.format(*di.format())`. Ordering confirmed via `grep -A3 'info_\.set(di\.box())'`. |
| TC-03: Five lens constant knobs on Ray (count) | artifact | PASS | `grep -c '_sensor_width\|_back_focal_length\|...'` → 15 (≥5). |
| TC-03: _sensor_width ≥3 lines | artifact | PASS | Lines 106 (member decl), 146 (ctor init 36.0f), 265 (Float_knob call). |
| TC-03: Angenieux 55mm defaults (4 non-sensor values) | artifact | PASS | Found `30.829003f`, `29.865562f`, `19.357308f`, `35.445997f` in ctor initialiser list. |
| TC-04: Three path-trace math functions present | artifact | PASS | `grep -n 'inline.*sphereToCs_full\|inline.*pt_sample_aperture\|inline.*logarithmic_focus_search'` returns 3 lines at deepc_po_math.h:265, 332, 421. |
| TC-04: pt_sample_aperture Newton parameters | artifact | PASS | Found `DAMP = 0.72f`, `TOL = 1e-4f`, comment "5 Newton iterations … 1e-4 tolerance … 0.72 damping factor". |
| TC-04: logarithmic_focus_search step count | artifact | PASS | Comment "Brute-force searches 20001 logarithmically-spaced sensor_shift values" confirmed in deepc_po_math.h. |
| TC-05: sphereToCs direction-only retained | artifact | PASS | `grep -n 'inline.*sphereToCs[^_]'` → line 248. `grep -c 'sphereToCs' src/DeepCDefocusPORay.cpp` → 2. |
| TC-06: File structure (old gone, new exist, test .nk files) | artifact | PASS | Ray PASS, Thin PASS, old DeepCDefocusPO.cpp PASS (gone), test_thin.nk PASS, test_ray.nk PASS. |
| TC-07: poly.h max_degree parameter | artifact | PASS | Lines 98 (parameter default -1), 108-113 (early-exit logic). |
| TC-08: CMakeLists.txt targets both split plugins | artifact | PASS | DeepCDefocusPOThin: 2 hits; DeepCDefocusPORay: 2 hits; DeepCDefocusPO[^TR]: 0 hits. |
| TC-09: Syntax compilation in isolation (via gate script) | runtime | PASS | Both DeepCDefocusPOThin.cpp and DeepCDefocusPORay.cpp compile cleanly inside verify script (separate g++ invocations). |
| TC-10: deepc_po_math.h + poly.h compile together | runtime | PASS | Inline compilation `echo '#include "poly.h"\n#include "deepc_po_math.h"\nint main(){}'` via g++ -std=c++17 exits 0, zero errors. |

## Overall Verdict

PASS — All 15 checks across 10 test cases passed; 13/13 S01 structural contracts pass; all 4 source files compile cleanly; 0 FAILs.

## Notes

- TC-01 gate script (`verify-s01-syntax.sh`) is the single authoritative gate and passed cleanly.
- The `grep -c 'DeepCDefocusPO[^TR]' src/CMakeLists.txt` command returns exit code 1 (grep returns 1 when no matches found), which is the correct/expected result (old monolithic entry absent). Count confirmed as 0.
- TC-09 leveraged the verify script's isolated g++ invocations rather than the manual `/tmp/mock_headers` path — both prove the same property.
- No runtime Nuke execution required at S01 proof level (contract only); runtime proof is S02's gate.
