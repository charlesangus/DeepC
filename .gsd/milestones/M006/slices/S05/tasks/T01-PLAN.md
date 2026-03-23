---
estimated_steps: 4
estimated_files: 4
skills_used:
  - best-practices
---

# T01: Fix stale comments, add license entry, create icon, extend verify script

**Slice:** S05 — Build Integration, Menu Registration, and Final Verification
**Milestone:** M006

## Description

S01–S04 left four loose ends that S05 must close before the milestone is done:

1. Lines 13–14 of `src/DeepCDefocusPO.cpp` still read "S01 state: skeleton only" and "S02 replaces the renderStripe body" — these are stale lies after S02–S04 fully implemented the scatter engine. They must be replaced with an accurate description.

2. `THIRD_PARTY_LICENSES.md` has no entry for `src/poly.h`, which was vendored from `https://github.com/hanatos/lentil` (MIT). Legal hygiene requires documenting this.

3. `icons/DeepCDefocusPO.png` does not exist. The CMake root-level glob `file(GLOB ICONS "icons/DeepC*.png")` auto-installs any matching PNG at build time — so the icon file only needs to exist in the `icons/` directory. Without it the menu item appears without an icon. A placeholder copy of `icons/DeepCBlur.png` is sufficient for milestone completion.

4. `scripts/verify-s01-syntax.sh` has no S05 contract block. Adding the S05 contracts provides the structural gate proving all integration wiring (CMake PLUGINS+FILTER_NODES, Op::Description, lentil license, stale comment removal) is in place.

All four changes are independent and mechanical. The verify script is the single gate for all of them.

## Steps

1. **Fix stale comment lines in `src/DeepCDefocusPO.cpp`**
   - Find lines 13–14: `//  S01 state: skeleton only — renderStripe outputs flat black (zeros).` and `//  S02 replaces the renderStripe body with the full PO scatter loop.`
   - Replace with:
     ```
     //  Full implementation: PO scatter engine (S02), depth-aware holdout (S03),
     //  and focal-length knob (S04) are all complete.
     ```
   - Also update the Knobs comment block (lines ~21–24) to add `focal_length_mm` — add one line: `//    focal_length_mm  — lens focal length in mm (default 50.0)` after the `fstop` line.
   - Verify with: `! grep -q 'S01 state: skeleton only' src/DeepCDefocusPO.cpp`

2. **Add lentil/poly.h section to `THIRD_PARTY_LICENSES.md`**
   - Append a new section following the existing FastNoise section pattern:
     ```markdown
     ---

     ## lentil / poly.h

     - **Project:** lentil — polynomial-optics lens simulation
     - **File vendored:** `src/poly.h` (polynomial-optics/src/poly.h)
     - **Author:** Johannes Hanika (hanatos)
     - **Source:** https://github.com/hanatos/lentil
     - **License:** MIT

     SPDX-License-Identifier: MIT

     Permission is hereby granted, free of charge, to any person obtaining a copy
     of this software and associated documentation files (the "Software"), to deal
     in the Software without restriction, including without limitation the rights
     to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
     copies of the Software, and to permit persons to whom the Software is
     furnished to do so, subject to the following conditions:

     The above copyright notice and this permission notice shall be included in all
     copies or substantial portions of the Software.

     THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
     IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
     FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
     AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
     LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
     OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
     SOFTWARE.
     ```
   - Verify with: `grep -q 'lentil\|hanatos' THIRD_PARTY_LICENSES.md`

3. **Create `icons/DeepCDefocusPO.png`**
   - Copy `icons/DeepCBlur.png` to `icons/DeepCDefocusPO.png`:
     ```bash
     cp icons/DeepCBlur.png icons/DeepCDefocusPO.png
     ```
   - The CMake root-level install glob `file(GLOB ICONS "icons/DeepC*.png")` will pick this up automatically — no CMakeLists.txt changes needed.
   - Verify with: `test -f icons/DeepCDefocusPO.png`

4. **Append S05 contract block to `scripts/verify-s01-syntax.sh`**
   - Insert the following block immediately before the final `echo "All syntax checks passed."` line:
     ```bash
     # --- S05 contracts ---
     echo "Checking S05 contracts..."
     # Stale S01/S02 comment lines removed
     ! grep -q 'S01 state: skeleton only' "$SRC_DIR/DeepCDefocusPO.cpp" || { echo "FAIL: stale S01 skeleton comment still present"; exit 1; }
     ! grep -q 'S02 replaces the renderStripe' "$SRC_DIR/DeepCDefocusPO.cpp" || { echo "FAIL: stale S02 comment still present"; exit 1; }
     # CMake entries present (regression guard)
     grep -c 'DeepCDefocusPO' "$SRC_DIR/CMakeLists.txt" | grep -q '^2$' || { echo "FAIL: DeepCDefocusPO not in exactly 2 CMakeLists.txt locations"; exit 1; }
     # FILTER_NODES entry
     grep -q 'FILTER_NODES.*DeepCDefocusPO\|DeepCDefocusPO.*FILTER_NODES' "$SRC_DIR/CMakeLists.txt" || { echo "FAIL: DeepCDefocusPO not in FILTER_NODES"; exit 1; }
     # THIRD_PARTY_LICENSES entry
     grep -q 'lentil\|hanatos' "$(dirname "$SRC_DIR")/THIRD_PARTY_LICENSES.md" || { echo "FAIL: lentil/poly.h not in THIRD_PARTY_LICENSES.md"; exit 1; }
     # Op::Description registration present
     grep -q 'Op::Description' "$SRC_DIR/DeepCDefocusPO.cpp" || { echo "FAIL: Op::Description missing"; exit 1; }
     echo "S05 contracts: all pass."
     ```
   - Verify by running `bash scripts/verify-s01-syntax.sh` — must exit 0 with "S05 contracts: all pass." in output.

## Observability Impact

This task produces no new runtime logs. The inspection surfaces are:

- `bash scripts/verify-s01-syntax.sh` — primary gate; exits 0 and prints "S05 contracts: all pass." when all structural checks hold. Each contract emits a distinct FAIL message to stderr and exits 1 on failure, making failures unambiguous.
- `grep -q 'S01 state: skeleton only' src/DeepCDefocusPO.cpp` — exits 1 (success = comment gone) to verify stale-comment removal.
- `grep -q 'lentil\|hanatos' THIRD_PARTY_LICENSES.md` — exits 0 to verify license entry.
- `test -s icons/DeepCDefocusPO.png` — exits 0 to verify icon file is non-empty.
- `file icons/DeepCDefocusPO.png` — confirms PNG MIME type if icon copy integrity needs checking.

No redaction constraints — all verification commands operate on source files, not secrets.

## Must-Haves

- [ ] Lines 13–14 of `src/DeepCDefocusPO.cpp` no longer contain `S01 state: skeleton only` or `S02 replaces the renderStripe`
- [ ] `THIRD_PARTY_LICENSES.md` contains `lentil` or `hanatos` (the attribution section)
- [ ] `icons/DeepCDefocusPO.png` exists and is a valid PNG (non-zero byte size)
- [ ] `scripts/verify-s01-syntax.sh` contains an S05 contracts block and exits 0 end-to-end

## Verification

- `bash scripts/verify-s01-syntax.sh` → exits 0; output includes "S05 contracts: all pass."
- `! grep -q 'S01 state: skeleton only' src/DeepCDefocusPO.cpp` → exits 0
- `grep -q 'lentil\|hanatos' THIRD_PARTY_LICENSES.md` → exits 0
- `test -f icons/DeepCDefocusPO.png` → exits 0
- `test -s icons/DeepCDefocusPO.png` → exits 0 (non-empty file)

## Inputs

- `src/DeepCDefocusPO.cpp` — fully implemented node from S01–S04; lines 13–14 contain the stale comments to fix
- `THIRD_PARTY_LICENSES.md` — existing file with DeepThinner and FastNoise sections; lentil section to be appended
- `scripts/verify-s01-syntax.sh` — existing verify script with S01–S04 contracts; S05 block to be appended
- `icons/DeepCBlur.png` — source icon to copy as placeholder for DeepCDefocusPO

## Expected Output

- `src/DeepCDefocusPO.cpp` — stale comment lines 13–14 replaced with accurate implementation-state comment; Knobs comment block updated to include `focal_length_mm`
- `THIRD_PARTY_LICENSES.md` — new `lentil / poly.h` section appended with MIT attribution
- `scripts/verify-s01-syntax.sh` — S05 contract block appended; script exits 0 end-to-end
- `icons/DeepCDefocusPO.png` — new file (copy of `icons/DeepCBlur.png`); picked up by CMake install glob automatically
