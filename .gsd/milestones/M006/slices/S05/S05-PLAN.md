# S05: Build Integration, Menu Registration, and Final Verification

**Goal:** Close the milestone by fixing the two stale comment lines left by S01/S02, adding the lentil/poly.h third-party license entry, creating the node icon, and extending the verify script with S05 contracts — leaving every structural gate passing and the node ready for CI docker build.
**Demo:** `bash scripts/verify-s01-syntax.sh` exits 0 with S05 contracts included; `grep -q 'lentil\|hanatos' THIRD_PARTY_LICENSES.md` exits 0; `test -f icons/DeepCDefocusPO.png` exits 0; no stale S01/S02 comment lines in `src/DeepCDefocusPO.cpp`.

## Must-Haves

- Stale two-line comment block (`S01 state: skeleton only` / `S02 replaces the renderStripe`) removed from `src/DeepCDefocusPO.cpp`
- lentil/poly.h attribution section added to `THIRD_PARTY_LICENSES.md`
- S05 grep contracts appended to `scripts/verify-s01-syntax.sh` and passing
- `icons/DeepCDefocusPO.png` created (placeholder copy of `icons/DeepCBlur.png`)

## Proof Level

- This slice proves: contract — all structural integration wiring (CMake PLUGINS + FILTER_NODES, Op::Description, icon install glob, menu.py.in) confirmed in place via grep contracts; S01–S05 verify script exits 0
- Real runtime required: no — docker build and Nuke runtime are CI/UAT only; Docker not installed in workspace
- Human/UAT required: no — visual bokeh check and holdout depth-correctness are documented as CI/UAT steps in M006-CONTEXT.md; not part of this slice's gate

## Verification

```bash
# Primary gate — all S01–S05 contracts pass
bash scripts/verify-s01-syntax.sh

# Individual spot checks
grep -c 'DeepCDefocusPO' src/CMakeLists.txt | grep -q '^2$'
grep -q 'FILTER_NODES.*DeepCDefocusPO' src/CMakeLists.txt
grep -q 'Op::Description' src/DeepCDefocusPO.cpp
grep -q 'lentil\|hanatos' THIRD_PARTY_LICENSES.md
! grep -q 'S01 state: skeleton only' src/DeepCDefocusPO.cpp
test -f icons/DeepCDefocusPO.png
```

Slice is done when `bash scripts/verify-s01-syntax.sh` exits 0 and all six spot checks above pass.

## Integration Closure

- Upstream surfaces consumed: `src/DeepCDefocusPO.cpp` (S01–S04), `src/CMakeLists.txt` (S01), `scripts/verify-s01-syntax.sh` (S01–S04), `icons/DeepCBlur.png` (existing)
- New wiring introduced in this slice: none — all CMake/menu/Description wiring was established in S01 and is already in place; this slice only removes stale comments, adds attribution, creates the icon file, and extends the verify script
- What remains before the milestone is truly usable end-to-end: CI docker build (`docker-build.sh --linux --versions 16.0` exits 0, `DeepCDefocusPO.so` in release zip) and Nuke runtime visual check (bokeh visible, holdout depth-correct)

## Observability / Diagnostics

The primary inspection surface for this slice is the verify script itself: `bash scripts/verify-s01-syntax.sh` exits 0 and emits "S05 contracts: all pass." when all structural wiring is correct. Individual failure signals:

- **Stale-comment check fails:** `grep -q 'S01 state: skeleton only' src/DeepCDefocusPO.cpp` exits 0 — means the comment edit was not applied or was reverted.
- **CMake count fails:** `grep -c 'DeepCDefocusPO' src/CMakeLists.txt` ≠ 2 — means either the PLUGINS list or FILTER_NODES entry is missing/duplicated.
- **License missing:** `grep -q 'lentil\|hanatos' THIRD_PARTY_LICENSES.md` exits 1 — means the append did not land.
- **Icon missing:** `test -f icons/DeepCDefocusPO.png` exits 1 — means the copy was not executed; `file icons/DeepCDefocusPO.png` shows whether the file is a valid PNG.
- **Op::Description missing:** `grep -q 'Op::Description' src/DeepCDefocusPO.cpp` exits 1 — means the static description block was removed from the node source.

No runtime logs are emitted by these changes; all observability is via the verify script and direct grep commands listed above.

## Tasks

- [x] **T01: Fix stale comments, add license entry, create icon, extend verify script** `est:30m`
  - Why: Closes all four remaining mechanical S05 deliverables in one coherent pass; the verify script extension is the gate that proves all structural wiring is in place
  - Files: `src/DeepCDefocusPO.cpp`, `THIRD_PARTY_LICENSES.md`, `scripts/verify-s01-syntax.sh`, `icons/DeepCDefocusPO.png`
  - Do: (1) Edit `src/DeepCDefocusPO.cpp` lines 13–14 — replace the two stale comment lines with the accurate implementation state comment; (2) Append lentil/poly.h section to `THIRD_PARTY_LICENSES.md`; (3) Copy `icons/DeepCBlur.png` to `icons/DeepCDefocusPO.png` as placeholder; (4) Append S05 contract block to `scripts/verify-s01-syntax.sh` before the final success echo
  - Verify: `bash scripts/verify-s01-syntax.sh` exits 0; `test -f icons/DeepCDefocusPO.png`; `grep -q 'lentil\|hanatos' THIRD_PARTY_LICENSES.md`; `! grep -q 'S01 state: skeleton only' src/DeepCDefocusPO.cpp`
  - Done when: All four spot checks above pass and `bash scripts/verify-s01-syntax.sh` exits 0 with S05 contracts included

## Files Likely Touched

- `src/DeepCDefocusPO.cpp`
- `THIRD_PARTY_LICENSES.md`
- `scripts/verify-s01-syntax.sh`
- `icons/DeepCDefocusPO.png`
