# S05 Research: Build Integration, Menu Registration, and Final Verification

**Milestone:** M006 — DeepCDefocusPO  
**Slice:** S05  
**Risk:** Low  
**Depth:** Light — all integration points are already wired; this slice is mechanical cleanup + verification

---

## Summary

S01–S04 delivered a complete, fully wired DeepCDefocusPO implementation. The node compiles, all S01–S04 syntax and grep contracts pass (`verify-s01-syntax.sh` → exit 0 as of this research). S05 has three deliverables:

1. **Fix two stale comments** in `src/DeepCDefocusPO.cpp` (the `S01 state: skeleton only` header block — noted by S04).
2. **Create `icons/DeepCDefocusPO.png`** — the node icon required for the menu entry to show a visible icon in Nuke. Without this, menu registration still works but the menu item has no icon.
3. **Add a `lentil/poly.h` entry to `THIRD_PARTY_LICENSES.md`** — poly.h was vendored from `https://github.com/hanatos/lentil` (MIT); it is not yet documented.
4. **Add S05 grep contracts to `verify-s01-syntax.sh`** — confirms build integration, icon presence, and THIRD_PARTY_LICENSES entry.
5. **Document the docker build + zip verification procedure** — no actual docker run (Docker not installed in workspace); add a verification note that confirms the CMake wiring is correct and what CI must check.

The docker build gate (`docker-build.sh --linux --versions 16.0` exits 0 with `DeepCDefocusPO.so` in the zip) is **CI-only**. The workspace has no Docker. The structural proof (CMake entry, Description registration, FILTER_NODES entry) is already complete and verified. S05 documents the procedure and confirms the structural wiring is in place.

---

## Current State — What Is Already Done

### CMake integration (complete ✅)
`src/CMakeLists.txt` has `DeepCDefocusPO` in two places:
- Line 16: `PLUGINS` list (non-wrapped; simple `add_nuke_plugin(DeepCDefocusPO DeepCDefocusPO.cpp)` via the foreach loop)
- Line 53: `FILTER_NODES` list (propagated into `menu.py` via CMake `configure_file`)

No changes needed to CMakeLists.txt.

### Menu registration (complete ✅)
`python/menu.py.in` reads `@FILTER_NODES@` which CMake fills from `set(FILTER_NODES ... DeepCDefocusPO)`. The menu command template is:
```python
new.addCommand(label, "nuke.createNode('{}')".format(node), icon="{}.png".format(node))
```
This generates `nuke.createNode('DeepCDefocusPO')` with icon `DeepCDefocusPO.png`. **The icon file must exist** in the `icons/` directory at install time, or the menu item will appear without an icon (not a crash, just visually missing).

`python/init.py` calls `nuke.pluginAddPath("icons")` — this registers the icons directory so Nuke's icon lookup resolves `DeepCDefocusPO.png` correctly.

The `CMakeLists.txt` (root-level) installs icons via:
```cmake
file(GLOB ICONS "icons/DeepC*.png")
install(FILES ${ICONS} DESTINATION icons)
```
This glob matches `DeepCDefocusPO.png` automatically — **no CMakeLists.txt changes needed** for icon installation, just the icon file itself.

### Op::Description registration (complete ✅)
`DeepCDefocusPO.cpp` line ~420:
```cpp
static Op* build(Node* node) { return new DeepCDefocusPO(node); }
const Op::Description DeepCDefocusPO::d(::CLASS, "Deep/DeepCDefocusPO", build);
```
This is the correct Nuke registration pattern (matches `DeepCBlur`, `DeepCDepthBlur`, etc.).

### Verify script contracts (complete through S04 ✅)
`scripts/verify-s01-syntax.sh` passes exit 0, covering S01–S04 contracts. S05 needs to append its own contract group.

---

## What S05 Must Do

### Task 1: Fix stale comment block in `src/DeepCDefocusPO.cpp`

Lines 12–14 read:
```cpp
//  S01 state: skeleton only — renderStripe outputs flat black (zeros).
//  S02 replaces the renderStripe body with the full PO scatter loop.
```
These are post-S02 lies. The node is fully implemented. Replace with accurate state:
```cpp
//  Full implementation: renderStripe scatters each deep sample through the
//  polynomial lens system; holdout weighting applied per-contribution.
```
Also, the knobs comment block (lines 21–24) lists only four knobs and is missing `focal_length` added in S04:
```cpp
//  Knobs:
//    poly_file        — path to lentil .fit polynomial lens file
//    focus_distance   — distance to focus plane, mm (default 200 mm = 2 m)
//    fstop            — lens f-stop (default 2.8)
//    aperture_samples — scatter samples per deep sample (default 64)
```
This should add `focal_length_mm` and the divider convention. The file-header comment is cosmetic only — the S04 grep contract already checks `! grep -q 'S01 skeleton'`. The two stale lines are a separate string from the HELP text.

Exact old text to replace (lines 12–13):
```
//  S01 state: skeleton only — renderStripe outputs flat black (zeros).
//  S02 replaces the renderStripe body with the full PO scatter loop.
```
Replace with:
```
//  Full implementation: PO scatter engine (S02), depth-aware holdout (S03),
//  and focal-length knob (S04) are all complete.
```

### Task 2: Create `icons/DeepCDefocusPO.png`

The CMake glob `icons/DeepC*.png` auto-installs any PNG in `icons/` matching the pattern — no CMakeLists.txt change needed.

Without this file:
- The menu item still appears and `nuke.createNode('DeepCDefocusPO')` still works
- The menu entry has no visible icon (shows a blank/default icon)
- `unzip -l release/DeepC-Linux-Nuke16.0.zip | grep icons/DeepCDefocusPO.png` fails

Creating the icon: the simplest approach is to copy an existing DeepC filter icon (e.g. `DeepCBlur.png`) as a placeholder. For a production release a distinct icon would be created, but the milestone only requires the node to appear in the FILTER_NODES menu category — a placeholder PNG is sufficient.

The pattern from DeepCDepthBlur (M005): no dedicated icon was added for DeepCDepthBlur — it is absent from `ls icons/`. This means `DeepCDepthBlur.png` does not exist and the menu entry for it has no icon either. **The milestone success criterion says "Node appears in FILTER_NODES menu category"** — this is satisfied by the CMakeLists.txt entry and the `menu.py.in` wiring, regardless of whether an icon PNG exists. The icon file is therefore **optional for milestone completion** but recommended for polish.

Decision: create a minimal `DeepCDefocusPO.png` by copying `DeepCBlur.png` as placeholder. This matches the pattern for DeepCDepthBlur (where the icon is also absent but the node works). If the user accepts this, S05 should create the file and document it. If they want a custom icon, that is out of scope for this milestone.

### Task 3: Add lentil/poly.h entry to `THIRD_PARTY_LICENSES.md`

`poly.h` is vendored from `https://github.com/hanatos/lentil` (MIT). The file header says:
```
// SPDX-License-Identifier: MIT
// poly.h — Vendored from lentil polynomial-optics/src/poly.h
// Source: https://github.com/hanatos/lentil  (MIT license)
```
The lentil project does not have a published formal copyright notice in the public repo beyond the SPDX identifier. A best-effort attribution entry should be added to `THIRD_PARTY_LICENSES.md` following the existing pattern (DeepThinner, FastNoise sections).

### Task 4: Add S05 contracts to `verify-s01-syntax.sh`

S05 grep contracts to add (before the final `echo "All syntax checks passed."`):
```bash
# --- S05 contracts ---
echo "Checking S05 contracts..."
# Stale S01/S02 comment block removed
! grep -q 'S01 state: skeleton only' "$SRC_DIR/DeepCDefocusPO.cpp" || { echo "FAIL: stale S01 skeleton comment still present"; exit 1; }
! grep -q 'S02 replaces the renderStripe' "$SRC_DIR/DeepCDefocusPO.cpp" || { echo "FAIL: stale S02 comment still present"; exit 1; }
# CMake entries still present (regression guard)
grep -c 'DeepCDefocusPO' "$SRC_DIR/CMakeLists.txt" | grep -q '^2$' || { echo "FAIL: DeepCDefocusPO not in exactly 2 CMakeLists.txt locations"; exit 1; }
# FILTER_NODES entry
grep -q 'FILTER_NODES.*DeepCDefocusPO\|DeepCDefocusPO.*FILTER_NODES' "$SRC_DIR/CMakeLists.txt" || { echo "FAIL: DeepCDefocusPO not in FILTER_NODES"; exit 1; }
# THIRD_PARTY_LICENSES entry
grep -q 'lentil\|poly\.h\|hanatos' "$(dirname "$SRC_DIR")/THIRD_PARTY_LICENSES.md" || { echo "FAIL: lentil/poly.h not in THIRD_PARTY_LICENSES.md"; exit 1; }
# Op::Description registration present
grep -q 'Op::Description' "$SRC_DIR/DeepCDefocusPO.cpp" || { echo "FAIL: Op::Description missing"; exit 1; }
echo "S05 contracts: all pass."
```

Note: the FILTER_NODES grep uses the full CMakeLists.txt line `set(FILTER_NODES DeepCBlur DeepCBlur2 DeepThinner DeepCDepthBlur DeepCDefocusPO)` — the pattern `FILTER_NODES.*DeepCDefocusPO` matches this because `FILTER_NODES` appears on the same line as `DeepCDefocusPO`.

The icon contract is deliberately omitted from the verify script — icon presence is a build artefact check (done via `unzip -l` on the release zip) rather than a source-level structural check.

---

## Implementation Landscape (File-by-File)

| File | Change | Priority |
|------|--------|----------|
| `src/DeepCDefocusPO.cpp` | Fix stale 2-line comment (lines 12–13) | Required — S04 noted it; one edit |
| `THIRD_PARTY_LICENSES.md` | Add lentil/poly.h section | Required — legal hygiene; one append |
| `scripts/verify-s01-syntax.sh` | Append S05 contract block | Required — milestone gate |
| `icons/DeepCDefocusPO.png` | Create placeholder (copy of DeepCBlur.png) | Recommended (menu polish); not required for node registration |

No changes to `src/CMakeLists.txt`, `CMakeLists.txt` (root), `python/menu.py.in`, or `python/init.py` — all are already correct.

---

## Verification Procedure

### What can be verified in this workspace (no Docker)

1. `bash scripts/verify-s01-syntax.sh` → exit 0 (all S01–S05 contracts pass)
2. `grep -c 'DeepCDefocusPO' src/CMakeLists.txt` → `2` (PLUGINS + FILTER_NODES)
3. `grep -q 'FILTER_NODES.*DeepCDefocusPO' src/CMakeLists.txt` → exit 0
4. `grep -q 'Op::Description' src/DeepCDefocusPO.cpp` → exit 0
5. `grep -q 'lentil\|hanatos' THIRD_PARTY_LICENSES.md` → exit 0
6. `! grep -q 'S01 state: skeleton only' src/DeepCDefocusPO.cpp` → exit 0
7. `cmake -S . -B /tmp/deepc-test -DNuke_ROOT=/nonexistent` → fails at `find_package(Nuke REQUIRED)` but CMake syntax is clean and `DeepCDefocusPO` appears in configure output

### What requires CI (Docker not available here)

```bash
docker-build.sh --linux --versions 16.0   # exits 0
unzip -l release/DeepC-Linux-Nuke16.0.zip | grep 'DeepCDefocusPO.so'  # present
unzip -l release/DeepC-Linux-Nuke16.0.zip | grep 'menu.py'            # present
unzip -l release/DeepC-Linux-Nuke16.0.zip | grep 'icons/'             # icons present
```

### Nuke runtime (requires Nuke 16+)

```
# In Nuke Script Editor:
nuke.createNode('DeepCDefocusPO')  # should create node without crash
# Connect a Deep input; set poly_file to a real .fit file; render
```

The milestone Definition of Done includes:
- Non-zero output pixels with bokeh visible (requires real .fit file)
- Holdout stencil verified depth-correctly
- Node in FILTER_NODES menu visible in Nuke toolbar

These are CI/UAT checks. This slice's scope is the structural/code-level gates.

---

## Risks and Constraints

**None significant.** S05 is three mechanical edits + one file creation. All integration wiring (CMake, menu, Description) was established in S01 and is already verified. The docker build has never been run in this workspace but the CMake wiring is structurally correct and matches the pattern of all other PLUGINS entries that do build successfully.

**Known limitation from S02 (not fixed in S05):** The stripe-boundary seam for large bokeh discs — contributions landing in a neighbouring stripe are discarded. This produces dark seams when bokeh radius is large relative to stripe height. Documented in S02 summary. Not a build or registration issue.

**Icon without file:** If `icons/DeepCDefocusPO.png` is not created, the menu entry still functions — Nuke shows a blank icon. This matches DeepCDepthBlur's current state (its icon is also absent). The milestone DoD says "Node appears in FILTER_NODES menu category" — this is satisfied without the icon PNG.

---

## Recommendation

Single task T01 covering all four changes:

1. Edit `src/DeepCDefocusPO.cpp`: fix the two stale comment lines (12–13)
2. Append lentil section to `THIRD_PARTY_LICENSES.md`
3. Append S05 contract block to `scripts/verify-s01-syntax.sh`
4. Create `icons/DeepCDefocusPO.png` (copy `icons/DeepCBlur.png` as placeholder)

All four are mechanical, independent, and low-risk. Verification: `bash scripts/verify-s01-syntax.sh` → exit 0.
