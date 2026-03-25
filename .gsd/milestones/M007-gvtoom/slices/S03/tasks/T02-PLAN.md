---
estimated_steps: 3
estimated_files: 1
skills_used: []
---

# T02: Create test/test_ray.nk and run verification contracts

**Slice:** S03 — DeepCDefocusPORay gather engine
**Milestone:** M007-gvtoom

## Description

Create the Nuke test script `test/test_ray.nk` parallel to `test/test_thin.nk`. This script exercises `DeepCDefocusPORay` at 128×72 with the Angenieux 55mm lens files (both exitpupil.fit and aperture.fit). Then run all structural verification contracts from the slice plan to confirm S03 is complete.

## Steps

1. **Read `test/test_thin.nk`** as the template. Clone it and make these changes:
   - Replace `DeepCDefocusPOThin` node class with `DeepCDefocusPORay`
   - Add `aperture_file /home/latuser/git/lentil/polynomial-optics/database/lenses/1953-angenieux-double-gauss/55/fitted/aperture.fit` knob to the node
   - Change Write file target from `./test_thin.exr` to `./test_ray.exr`
   - Update node name from `DeepCDefocusPOThin1` to `DeepCDefocusPORay1`
   - Update Root name path to reference `test_ray.nk`
   - Update write_info comment to reference `test_ray.exr`

2. **Run all structural verification contracts**:
   ```bash
   # Structural checks on renderStripe
   grep -q '_aperture_sys' src/DeepCDefocusPORay.cpp
   grep -q 'sphereToCs' src/DeepCDefocusPORay.cpp
   test $(grep -c '_max_degree' src/DeepCDefocusPORay.cpp) -ge 2
   grep -q 'transmittance_at' src/DeepCDefocusPORay.cpp
   grep -q 'halton' src/DeepCDefocusPORay.cpp
   grep -q '0.45f' src/DeepCDefocusPORay.cpp
   grep -qE 'ox_land|oy_land' src/DeepCDefocusPORay.cpp
   bash scripts/verify-s01-syntax.sh
   
   # Test script checks
   test -f test/test_ray.nk
   grep -q 'DeepCDefocusPORay' test/test_ray.nk
   grep -q 'aperture_file' test/test_ray.nk
   grep -q 'test_ray.exr' test/test_ray.nk
   ```

3. **Commit** all S03 changes (both `src/DeepCDefocusPORay.cpp` and `test/test_ray.nk`).

## Must-Haves

- [ ] `test/test_ray.nk` exists and contains `DeepCDefocusPORay` node
- [ ] `aperture_file` knob present pointing to Angenieux 55mm aperture.fit
- [ ] Write target is `./test_ray.exr`
- [ ] All structural grep contracts pass
- [ ] `verify-s01-syntax.sh` still passes

## Verification

- `test -f test/test_ray.nk`
- `grep -q 'DeepCDefocusPORay' test/test_ray.nk`
- `grep -q 'aperture_file' test/test_ray.nk`
- `grep -q 'test_ray.exr' test/test_ray.nk`
- `bash scripts/verify-s01-syntax.sh` exits 0

## Inputs

- `test/test_thin.nk` — template to clone from
- `src/DeepCDefocusPORay.cpp` — T01 output (gather renderStripe implemented)

## Expected Output

- `test/test_ray.nk` — Nuke test script exercising DeepCDefocusPORay at 128×72
