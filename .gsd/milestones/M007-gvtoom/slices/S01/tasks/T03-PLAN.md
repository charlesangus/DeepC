---
estimated_steps: 4
estimated_files: 3
skills_used: []
---

# T03: Update CMake, verify script, and remove old DeepCDefocusPO.cpp

**Slice:** S01 — Shared infrastructure, max_degree knob, two-plugin scaffold
**Milestone:** M007-gvtoom

## Description

Wire both new plugins into the CMake build system, update the syntax-check script for the new file set, and remove the superseded `DeepCDefocusPO.cpp`.

## Steps

1. **Update `src/CMakeLists.txt`:**
   - In the `PLUGINS` list (around line 14): replace `DeepCDefocusPO` with `DeepCDefocusPOThin` and `DeepCDefocusPORay` (two separate entries)
   - In the `FILTER_NODES` list (around line 53): replace `DeepCDefocusPO` with `DeepCDefocusPOThin DeepCDefocusPORay`
   - After edit: `grep -c 'DeepCDefocusPOThin' src/CMakeLists.txt` should return 2; same for Ray; `grep 'DeepCDefocusPO[^TR]' src/CMakeLists.txt` should return nothing

2. **Update `scripts/verify-s01-syntax.sh`:**
   - Change the for-loop (around line near the bottom, `for src_file in DeepCBlur.cpp DeepCDepthBlur.cpp DeepCDefocusPO.cpp; do`) to: `for src_file in DeepCBlur.cpp DeepCDepthBlur.cpp DeepCDefocusPOThin.cpp DeepCDefocusPORay.cpp; do`
   - **Remove the S02 grep contracts block** (lines starting with `# --- S02 grep contracts ---` through `echo "S02 contracts: all pass."`). These check `DeepCDefocusPO.cpp` which is being deleted. S02 will need new contracts for the Thin variant — that's S02's responsibility.
   - **Remove the S03 grep contracts block** (lines starting with `# --- S03 grep contracts ---` through `echo "S03 contracts: all pass."`). Same reason.
   - **Remove the S04 grep contracts block** (lines starting with `# --- S04 grep contracts ---` through `echo "S04 contracts: all pass."`). Same reason.
   - **Update the S05 contracts block:**
     - Remove the two `grep -q` checks that look for stale S01/S02 comments in DeepCDefocusPO.cpp (the file is gone)
     - Replace the CMake count check `grep -c 'DeepCDefocusPO' "$SRC_DIR/CMakeLists.txt" | grep -q '^2$'` with TWO checks:
       ```bash
       grep -c 'DeepCDefocusPOThin' "$SRC_DIR/CMakeLists.txt" | grep -q '^2$' || { echo "FAIL: DeepCDefocusPOThin not in exactly 2 CMakeLists.txt locations"; exit 1; }
       grep -c 'DeepCDefocusPORay' "$SRC_DIR/CMakeLists.txt" | grep -q '^2$' || { echo "FAIL: DeepCDefocusPORay not in exactly 2 CMakeLists.txt locations"; exit 1; }
       ```
     - Replace the FILTER_NODES check with two checks for each new plugin name
     - Remove the `Op::Description` check that references DeepCDefocusPO.cpp
     - Keep the THIRD_PARTY_LICENSES check (it doesn't reference the old file)

3. **Delete `src/DeepCDefocusPO.cpp`:**
   - `rm src/DeepCDefocusPO.cpp`
   - Verify: `test ! -f src/DeepCDefocusPO.cpp`

4. **Run the full verification:**
   - `bash scripts/verify-s01-syntax.sh` — must exit 0

## Must-Haves

- [ ] CMake PLUGINS list has DeepCDefocusPOThin and DeepCDefocusPORay, not DeepCDefocusPO
- [ ] CMake FILTER_NODES list has both new plugins, not the old one
- [ ] verify-s01-syntax.sh for-loop checks both new .cpp files, not the old one
- [ ] Old S02/S03/S04 grep contracts removed (they referenced the deleted file)
- [ ] S05 CMake contracts updated for the new plugin names
- [ ] src/DeepCDefocusPO.cpp deleted
- [ ] `bash scripts/verify-s01-syntax.sh` exits 0

## Verification

- `bash scripts/verify-s01-syntax.sh` — primary gate, exits 0
- `test "$(grep -c 'DeepCDefocusPOThin' src/CMakeLists.txt)" -eq 2`
- `test "$(grep -c 'DeepCDefocusPORay' src/CMakeLists.txt)" -eq 2`
- `test ! -f src/DeepCDefocusPO.cpp`

## Inputs

- `src/CMakeLists.txt` — existing build file (replace old plugin entries)
- `scripts/verify-s01-syntax.sh` — existing syntax check script (update loop and contracts)
- `src/DeepCDefocusPOThin.cpp` — T02 output (must exist for syntax check)
- `src/DeepCDefocusPORay.cpp` — T02 output (must exist for syntax check)
- `src/DeepCDefocusPO.cpp` — to be deleted

## Expected Output

- `src/CMakeLists.txt` — modified with both new plugins
- `scripts/verify-s01-syntax.sh` — modified for new file set and contracts
