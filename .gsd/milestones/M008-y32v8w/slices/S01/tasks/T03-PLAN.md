---
estimated_steps: 4
estimated_files: 1
skills_used: []
---

# T03: Update verify-s01-syntax.sh with new mocks and contracts, verify all pass

**Slice:** S01 — Path-trace infrastructure + _validate format fix
**Milestone:** M008-y32v8w

## Description

Update `scripts/verify-s01-syntax.sh` so it compiles both `DeepCDefocusPOThin.cpp` and `DeepCDefocusPORay.cpp` (not the old `DeepCDefocusPO.cpp`), has complete mock headers for the new API surface (DeepInfo::format(), ImagePlane::writableAt(int,int,int), chanNo()), and includes grep contracts verifying all S01 deliverables. Run the script and confirm exit 0.

## Steps

1. **Start from the M007 version of verify-s01-syntax.sh** (which already handles Thin/Ray compilation):
   ```bash
   git show 89de100:scripts/verify-s01-syntax.sh > scripts/verify-s01-syntax.sh
   chmod +x scripts/verify-s01-syntax.sh
   ```

2. **Update mock headers for new API surface.** In the `DeepInfo` mock class, add:
   ```cpp
   const Format* format() const { static Format f; return &f; }
   ```
   This is needed because T01 added `info_.format(*di.format())` which calls `di.format()` on `DeepInfo`. The M007 mock may already have `fullSizeFormat()` but is missing `format()`.

   Verify `ImagePlane` mock has `writableAt(int x, int y, int chan)` overload and `chanNo()` method — the M007 version should have these but confirm.

   Also ensure the `Info` / `IopInfo` mock has `format(const Format&)` setter method — needed for `info_.format(...)`.

3. **Add S01 structural grep contracts.** After the syntax compilation loop, add contract checks:
   ```bash
   echo "--- S01 contracts ---"
   
   # _validate format fix in both files
   grep -q 'info_\.format(' "$SRC_DIR/DeepCDefocusPOThin.cpp" || { echo "FAIL: _validate format fix missing from Thin"; exit 1; }
   grep -q 'info_\.format(' "$SRC_DIR/DeepCDefocusPORay.cpp"  || { echo "FAIL: _validate format fix missing from Ray"; exit 1; }
   
   # New lens constant knobs on Ray
   grep -q '_sensor_width'        "$SRC_DIR/DeepCDefocusPORay.cpp" || { echo "FAIL: _sensor_width knob missing"; exit 1; }
   grep -q '_back_focal_length'   "$SRC_DIR/DeepCDefocusPORay.cpp" || { echo "FAIL: _back_focal_length knob missing"; exit 1; }
   grep -q '_outer_pupil_radius'  "$SRC_DIR/DeepCDefocusPORay.cpp" || { echo "FAIL: _outer_pupil_radius knob missing"; exit 1; }
   grep -q '_inner_pupil_radius'  "$SRC_DIR/DeepCDefocusPORay.cpp" || { echo "FAIL: _inner_pupil_radius knob missing"; exit 1; }
   grep -q '_aperture_pos'        "$SRC_DIR/DeepCDefocusPORay.cpp" || { echo "FAIL: _aperture_pos knob missing"; exit 1; }
   
   # New math functions in deepc_po_math.h
   grep -q 'pt_sample_aperture'       "$SRC_DIR/deepc_po_math.h" || { echo "FAIL: pt_sample_aperture missing"; exit 1; }
   grep -q 'sphereToCs_full'          "$SRC_DIR/deepc_po_math.h" || { echo "FAIL: sphereToCs_full missing"; exit 1; }
   grep -q 'logarithmic_focus_search' "$SRC_DIR/deepc_po_math.h" || { echo "FAIL: logarithmic_focus_search missing"; exit 1; }
   
   # poly.h max_degree
   grep -q 'max_degree' "$SRC_DIR/poly.h" || { echo "FAIL: max_degree missing from poly.h"; exit 1; }
   
   echo "All S01 contracts passed."
   ```

4. **Run the full verification script.**
   ```bash
   bash scripts/verify-s01-syntax.sh
   ```
   If any compilation or contract fails, debug and fix the issue. The most common failures will be:
   - Missing mock methods (fix by adding stubs to the mock headers in the script)
   - Missing `#include` or forward declarations (fix in the source files)
   - Grep contract not matching (fix the source or adjust the pattern)

## Must-Haves

- [ ] Script compiles `DeepCDefocusPOThin.cpp` and `DeepCDefocusPORay.cpp` successfully
- [ ] `DeepInfo` mock has `format()` method
- [ ] All S01 grep contracts present and passing
- [ ] `bash scripts/verify-s01-syntax.sh` exits 0

## Verification

- `bash scripts/verify-s01-syntax.sh` — must exit 0

## Inputs

- `scripts/verify-s01-syntax.sh` — M007 version extracted in this task as starting point
- `src/DeepCDefocusPOThin.cpp` — from T01
- `src/DeepCDefocusPORay.cpp` — from T01
- `src/deepc_po_math.h` — from T02
- `src/poly.h` — from T01

## Expected Output

- `scripts/verify-s01-syntax.sh` — updated with new mocks and S01 contracts, passing exit 0
