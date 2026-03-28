---
estimated_steps: 17
estimated_files: 1
skills_used: []
---

# T04: Full milestone DoD verification: docker build + nuke test + DoD checklist

Run the full milestone DoD checklist and confirm all items pass.

1. Run docker-build.sh (or the relevant docker run command) to produce a fresh DeepCOpenDefocus.so incorporating all S03 changes.
2. Run nuke -x test/test_opendefocus.nk (if Nuke available in env); otherwise confirm the .so was produced and document the manual test requirement.
3. If Nuke available: verify EXR output has non-black values (iinfo or exrinfo).
4. Run scripts/verify-s01-syntax.sh (or verify-s02-syntax.sh) to confirm all C++ compiles.
5. Check all DoD items:
   - DeepCOpenDefocus.so built ✓
   - nuke -x test passes ✓
   - GPU path compiles ✓
   - Holdout attenuation implemented ✓
   - Camera link implemented ✓
   - test/test_opendefocus.nk present ✓
   - icons/DeepCOpenDefocus.png present ✓
   - THIRD_PARTY_LICENSES.md EUPL-1.2 attribution present ✓
   - Non-uniform bokeh artifacts via opendefocus Settings defaults ✓
6. Record any deviations from the DoD.
7. Capture build log excerpts as verification evidence.

## Inputs

- `docker-build.sh`
- `test/test_opendefocus.nk`
- `icons/DeepCOpenDefocus.png`
- `THIRD_PARTY_LICENSES.md`
- `.gsd/milestones/M009-mso5fb/M009-mso5fb-ROADMAP.md`

## Expected Output

- `Docker build log showing S03 artifacts`
- `DoD checklist result`

## Verification

docker-build.sh exits 0. All 9 DoD items checked. scripts/verify-s01-syntax.sh passes. THIRD_PARTY_LICENSES.md grep confirms opendefocus EUPL-1.2 entry.
