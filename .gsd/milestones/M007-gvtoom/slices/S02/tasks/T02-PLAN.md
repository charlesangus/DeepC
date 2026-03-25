---
estimated_steps: 3
estimated_files: 1
skills_used: []
---

# T02: Write test/test_thin.nk test script

**Slice:** S02 — DeepCDefocusPOThin scatter engine
**Milestone:** M007-gvtoom

## Description

Create `test/test_thin.nk` — a Nuke script that renders DeepCDefocusPOThin at 128×72 to verify non-black output. Adapted from the existing reference test script at `/home/latuser/git/DeepC/test/test_deepcdefocus_po.nk`.

The script generates a checkerboard grid pattern via `Reformat → Grid → Dilate → Merge → DeepFromImage` (at depth 5000mm), pipes it through `DeepCDefocusPOThin` with the Angenieux 55mm lens, and writes `./test_thin.exr`. The `nuke -x test/test_thin.nk` command is the UAT gate for S02.

## Steps

1. **Read the reference test script** at `/home/latuser/git/DeepC/test/test_deepcdefocus_po.nk` to understand the exact Nuke node graph structure.

2. **Create `test/test_thin.nk`** by adapting the reference with these changes:
   - Root `name` → `/home/latuser/git/DeepC/test/test_thin.nk`
   - Root format stays `"128 72"` (the `Reformat1` node already forces 128×72)
   - Node class `DeepCDefocusPO` → `DeepCDefocusPOThin`
   - Add `max_degree 11` knob value to the node
   - Set `focal_length 55` (Angenieux 55mm)
   - Set `focus_distance 10000` (10m in mm)
   - Keep `fstop` at default (2.8) — not set in reference, uses knob default
   - Write node output path → `./test_thin.exr`
   - Write node `file_type exr`
   - Keep the entire grid generation chain (Reformat1 → Grid1 → Dilate → Merge → DeepFromImage) identical
   - `poly_file` → `/home/latuser/git/lentil/polynomial-optics/database/lenses/1953-angenieux-double-gauss/55/fitted/exitpupil.fit`

3. **Verify the file** exists, contains `DeepCDefocusPOThin`, and references `exitpupil.fit`.

## Must-Haves

- [ ] `test/test_thin.nk` exists and is valid Nuke script format
- [ ] Uses `DeepCDefocusPOThin` node class (not `DeepCDefocusPO`)
- [ ] Output resolution is 128×72
- [ ] `poly_file` points to Angenieux 55mm exitpupil.fit
- [ ] `focal_length 55` and `focus_distance 10000` set
- [ ] `max_degree 11` included
- [ ] Write node outputs to `./test_thin.exr`

## Verification

- `test -f test/test_thin.nk`
- `grep -q 'DeepCDefocusPOThin' test/test_thin.nk`
- `grep -q 'exitpupil.fit' test/test_thin.nk`
- `grep -q 'test_thin.exr' test/test_thin.nk`
- `grep -q 'focal_length 55' test/test_thin.nk`
- `grep -q 'focus_distance 10000' test/test_thin.nk`
- `grep -q 'max_degree 11' test/test_thin.nk`

## Inputs

- `src/DeepCDefocusPOThin.cpp` — T01's completed scatter engine (defines the node class and knobs the test script references)
- `/home/latuser/git/DeepC/test/test_deepcdefocus_po.nk` — reference test script to adapt

## Expected Output

- `test/test_thin.nk` — Nuke test script for DeepCDefocusPOThin at 128×72
