# S02: DeepCDefocusPOThin — UAT Script

**Milestone:** M007-gvtoom  
**Slice:** S02 — DeepCDefocusPOThin scatter engine  
**Written:** 2026-03-24  
**Runtime required:** Yes — docker build + Nuke session

---

## Preconditions

1. `docker-build.sh --linux --versions 16.0` has completed successfully and produced `DeepCDefocusPOThin.so`
2. The plugin is loadable by Nuke (either via docker test container or local batchInstall.sh)
3. The Angenieux 55mm exitpupil.fit file is present at the path referenced in `test/test_thin.nk`:  
   `/home/latuser/git/lentil/polynomial-optics/database/lenses/1953-angenieux-double-gauss/55/fitted/exitpupil.fit`
4. `scripts/verify-s01-syntax.sh` exits 0 (syntax gate, must pass before any runtime test)

---

## Test Cases

### TC-01: Syntax gate passes

**Precondition:** Working directory is `/home/latuser/git/DeepC` (or worktree)

**Steps:**
1. Run: `bash scripts/verify-s01-syntax.sh`
2. Confirm exit code is 0
3. Confirm output includes "Syntax check passed: DeepCDefocusPOThin.cpp"
4. Confirm "All syntax checks passed." appears at end

**Expected:** Exit 0, all 4 source files pass, S05 contracts pass.  
**Failure signal:** Any "Syntax check FAILED" line means a regression was introduced. Check the mock headers if the error is about `chanNo`, `writableAt`, or `contains`.

---

### TC-02: Structural contract checks

**Steps:** Run each grep in sequence:
```bash
grep -q 'coc_radius' src/DeepCDefocusPOThin.cpp && echo PASS || echo FAIL
grep -q 'halton' src/DeepCDefocusPOThin.cpp && echo PASS || echo FAIL
grep -q 'map_to_disk' src/DeepCDefocusPOThin.cpp && echo PASS || echo FAIL
grep -q 'chanNo' src/DeepCDefocusPOThin.cpp && echo PASS || echo FAIL
grep -q 'transmittance_at' src/DeepCDefocusPOThin.cpp && echo PASS || echo FAIL
grep -q '0.45f' src/DeepCDefocusPOThin.cpp && echo PASS || echo FAIL
grep -q '_max_degree' src/DeepCDefocusPOThin.cpp && echo PASS || echo FAIL
grep -q 'chans.contains' src/DeepCDefocusPOThin.cpp && echo PASS || echo FAIL
grep -q 'error.*Cannot open lens file' src/DeepCDefocusPOThin.cpp && echo PASS || echo FAIL
```

**Expected:** All 9 lines print PASS.  
**Failure signal:** Any FAIL indicates a required feature was removed. Each grep corresponds to a specific requirement: CoC physics (R030), aperture sampling (R025), channel safety, holdout (R035), CA wavelengths (R035), max_degree (R032), and error reporting.

---

### TC-03: Test script well-formedness

**Steps:**
```bash
test -f test/test_thin.nk && echo PASS || echo FAIL
grep -q 'DeepCDefocusPOThin' test/test_thin.nk && echo PASS || echo FAIL
grep -q 'exitpupil.fit' test/test_thin.nk && echo PASS || echo FAIL
grep -q 'test_thin.exr' test/test_thin.nk && echo PASS || echo FAIL
grep -q 'focal_length 55' test/test_thin.nk && echo PASS || echo FAIL
grep -q 'focus_distance 10000' test/test_thin.nk && echo PASS || echo FAIL
grep -q 'max_degree 11' test/test_thin.nk && echo PASS || echo FAIL
```

**Expected:** All 7 lines print PASS.  
**Failure signal:** Missing file or missing knob means the test script cannot reproduce the UAT scene.

---

### TC-04: Plugin loads in Nuke without errors

**Precondition:** Plugin compiled and on Nuke's plugin path.

**Steps:**
1. Launch Nuke: `nuke`
2. Open the Script Editor
3. Run: `nuke.load('DeepCDefocusPOThin')`  
   — OR just open a new node graph and Tab → type "DeepCDefocusPOThin"
4. Verify the node appears in the node menu under the DeepC group

**Expected:** Node creates without error; all knobs visible: `poly_file`, `focal_length`, `focus_distance`, `fstop`, `aperture_samples`, `max_degree`, holdout group.  
**Failure signal:** "Plugin not found" = build failed or symlink broken. "Unknown knob" = S01 knob registration regressed.

---

### TC-05: `nuke -x` command-line render exits 0

**Steps:**
```bash
nuke -x test/test_thin.nk
echo "Exit: $?"
```

**Expected:** Exit code 0. Nuke should print frame 1 render complete.  
**Failure signal:**  
- Exit 1 + "Cannot open lens file" → exitpupil.fit path is wrong for this machine. Update the `poly_file` knob in the test script or create a symlink.  
- Exit 1 + "Unknown node: DeepCDefocusPOThin" → plugin not loaded. Check `NUKE_PATH` or `init.py`.  
- Exit 0 but no output file → Write node path wrong (check working directory).

---

### TC-06: Output EXR is non-black (primary correctness gate)

**Precondition:** TC-05 exit 0, `test_thin.exr` written to working directory.

**Steps:**
1. Open `test_thin.exr` in Nuke or use `exrinfo`:
   ```bash
   exrinfo test_thin.exr | grep -i "channels\|resolution"
   ```
2. Load in Nuke viewer: confirm at least some pixels have non-zero R, G, or B values

**Expected:** The output is a visibly defocused version of the input checkerboard — blurred squares, not a sharp checkerboard. The bokeh shape should be roughly circular (thin-lens CoC) with subtle CA fringing on high-contrast edges.

**Failure signals:**
- **All black:** renderStripe early-returned. Check: `_poly_loaded` (fix: verify exitpupil.fit path), `_aperture_samples` knob (fix: set to ≥1), scatter samples all land outside tile bounds.
- **Sharp checkerboard (no blur):** CoC radius is near-zero. Check focus_distance vs z=5000 (with focus_distance=10000 and focal_length=55, CoC at z=5000 should be several pixels at 128×72).
- **Aperture ring / hollow disk:** Poly warp is being used as a direct position instead of an offset. This would indicate an Option B regression — the warp clamping/scaling logic in renderStripe.

---

### TC-07: Defocus moves with depth (depth-correctness check)

**Precondition:** TC-06 passes. This is a manual edit test.

**Steps:**
1. In `test/test_thin.nk`, change `z 5000` (in DeepFromImage1) to `z 2000`
2. Re-render: `nuke -x test/test_thin.nk`
3. Compare output against TC-06 output

**Expected:** At z=2000 (closer, more out of focus at focus_distance=10000), blur radius should be **larger** than at z=5000. The checkerboard squares should be blurrier.

**Failure signal:** Same blur radius at both depths → `coc_radius()` is not receiving the per-sample z value, or focus_distance knob is not wired to the function call.

---

### TC-08: max_degree affects output (R032 runtime proof)

**Precondition:** TC-06 passes.

**Steps:**
1. In `test/test_thin.nk`, change `max_degree 11` to `max_degree 3`
2. Re-render: `nuke -x test/test_thin.nk` → save as `test_thin_deg3.exr`
3. Restore `max_degree 11`, re-render → `test_thin_deg11.exr`
4. Open both in Nuke viewer side-by-side

**Expected:** Both renders should show defocused output. At degree 3 (fewer polynomial terms), aberration detail should be visibly reduced — bokeh shapes may be more symmetric/clean. At degree 11, subtle asymmetries from higher-order lens aberrations should be visible. The difference may be subtle on this synthetic scene but should exist.

**Failure signal:** Identical output at max_degree 3 and 11 → `_max_degree` is not being passed to `poly_system_evaluate`, or poly_system_evaluate ignores the limit.

---

### TC-09: Holdout path — no input

**Steps:**
1. Create a minimal Nuke script with `DeepCDefocusPOThin` with no input connected, then render one frame (or just call `node.setInput(0, None)` in script editor)
2. Check for crash vs graceful zero output

**Expected:** No crash. Plugin produces a black output (zero-output early return path). No error console spam beyond potentially a Nuke warning about missing input.

**Failure signal:** Nuke crash or assertion failure → the `!input0()` guard is missing or incorrectly placed in renderStripe.

---

### TC-10: Missing poly file produces parseable error

**Steps:**
1. In `test/test_thin.nk`, set `poly_file` to a non-existent path (e.g. `/tmp/does_not_exist.fit`)
2. Render with `nuke -x`

**Expected:** Nuke prints `error("Cannot open lens file: /tmp/does_not_exist.fit")` to stderr/console. Plugin does NOT crash. Output is black (zero output from early-return path).

**Failure signal:** Crash or silent hang → error handling in poly load path is missing.

---

## Edge Cases

### EC-01: aperture_samples = 1

Set `aperture_samples 1` in the test node. Render should produce a valid (non-black) but noisier defocused image. No crash. Verifies the `max(1, _aperture_samples)` guard.

### EC-02: fstop extremes

- `fstop 1.4` → large CoC, heavily blurred output
- `fstop 22` → very small CoC, output should approach the sharp input

Both should produce non-black, non-crashing output. Confirms the `coc_radius()` formula handles the full fstop range without divide-by-zero or NaN.

### EC-03: focus_distance equals z (in-focus check)

Set `focus_distance 5000` (matching the scene depth z=5000). CoC radius should approach zero; output should look very close to the sharp input. Confirms the CoC formula correctly goes to zero at exact focus.

---

## Failure Triage Guide

| Symptom | Most Likely Cause | Fix |
|---------|------------------|-----|
| Black output | exitpupil.fit not found | Fix poly_file path; check error console |
| Black output | Plugin not loaded | Check NUKE_PATH / plugin path |
| Aperture ring (hollow disk) | Option B warp used as absolute position | Check warp magnitude clamping in renderStripe |
| Sharp output (no blur) | CoC radius zero | Check focus_distance vs scene z; check coc_radius() call |
| Crash at startup | Channel enum mismatch | Rebuild with current sources; check mock header change didn't regress |
| Identical output at different max_degree | max_degree not wired | Check poly_system_evaluate call site in renderStripe |
| Per-channel fringing absent | CA loop not executing | Check 0.45/0.55/0.65 μm loop in renderStripe |

---

## Notes for Tester

- The synthetic scene (checkerboard at z=5000, focus_distance=10000) is intentionally out of focus. With Angenieux 55mm at f/2.8 (default), the CoC at 5000mm with focus at 10000mm is substantial — visible blurring should be immediately obvious at 128×72.
- CA fringing will be subtle at 128×72 but should be visible on the sharp checkerboard edges in the blurred output.
- The visual UAT (TC-06 through TC-08) requires human judgment — "does it look defocused?" is not automatable. This is the acceptance gate for the entire M007 thin variant.
