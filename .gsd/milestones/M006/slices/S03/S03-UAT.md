# S03 UAT: Depth-Aware Holdout

**Milestone:** M006  
**Slice:** S03  
**Written:** 2026-03-23  
**Status:** Ready for human verification in Nuke

---

## Preconditions

- DeepCDefocusPO.so is built and loaded in Nuke (requires S05 docker build; until then, UAT tests 1–3 are executable in the workspace via syntax/grep gates)
- A test Deep image with at least one out-of-focus object is available
- A second Deep image (or DeepConstant) suitable for use as a holdout mask is available
- `bash scripts/verify-s01-syntax.sh` exits 0 (workspace gate — run first)

---

## Test Cases

### TC-1: Syntax and contract gate (workspace-executable, no Nuke required)

**Purpose:** Confirm all S03 implementation contracts are met before any Nuke test.

**Steps:**
1. `cd` to the project root.
2. Run: `bash scripts/verify-s01-syntax.sh`
3. Run each individual contract check:
   ```bash
   grep -q 'holdoutConnected' src/DeepCDefocusPO.cpp && echo PASS || echo FAIL
   grep -q 'transmittance_at' src/DeepCDefocusPO.cpp && echo PASS || echo FAIL
   grep -q 'holdout_w'        src/DeepCDefocusPO.cpp && echo PASS || echo FAIL
   grep -q 'hzf >= Z'         src/DeepCDefocusPO.cpp && echo PASS || echo FAIL
   grep -q 'holdoutOp->deepRequest' src/DeepCDefocusPO.cpp && echo PASS || echo FAIL
   test "$(grep -c 'input(1)' src/DeepCDefocusPO.cpp)" -ge 3 && echo PASS || echo FAIL
   ! grep -q 'TODO\|STUB' src/DeepCDefocusPO.cpp && echo PASS || echo FAIL
   ```

**Expected outcome:**  
`verify-s01-syntax.sh` exits 0 and prints "All syntax checks passed." All seven individual checks print PASS.

**Failure signal:** Any FAIL or non-zero exit → the implementation has regressed. Check which token is missing.

---

### TC-2: Holdout input not connected — identity behaviour

**Purpose:** Confirm that omitting the holdout input leaves the defocused output unchanged (holdout_w = 1.0f throughout).

**Setup in Nuke:**
1. Create a DeepCDefocusPO node.
2. Connect a Deep render to input 0. Leave input 1 (holdout) **disconnected**.
3. Load a valid `.fit` lens file. Set focus_distance to place a highlight out of focus.

**Steps:**
1. Render the output and examine the defocused image.
2. Verify bokeh is visible (non-zero pixels in defocused regions).
3. Note the pixel values at several defocused highlight positions.
4. Now connect a DeepConstant to input 1 with alpha = 0 (fully transparent) and repeat.

**Expected outcome:**  
- Disconnected holdout and zero-alpha holdout produce **identical** pixel values.
- No NaN, black-frame, or crash.
- Bokeh shape and size are unchanged from S02 output.

**Failure signal:** Blank output with no holdout connected → `holdoutConnected` default path is not returning 1.0f correctly.

---

### TC-3: Holdout at depth shallower than sample — scatter contribution zeroed

**Purpose:** Confirm that a fully opaque holdout sample placed in front of a scatter contribution blocks it completely.

**Setup in Nuke:**
1. Create a Deep render with a bright highlight at depth Z = 5.0 (out of focus).
2. Create a DeepConstant with alpha = 1.0 and depth range [1.0, 2.0] (in front of the highlight).
3. Connect the DeepConstant to DeepCDefocusPO input 1 (holdout).

**Steps:**
1. Render DeepCDefocusPO. Examine the output region where the out-of-focus highlight would appear.

**Expected outcome:**  
The bokeh contribution from the highlight at Z=5.0 is **zeroed out** (or near zero) at output pixels where the holdout sample (Z=1.0–2.0) was present.  
The holdout itself does **not** appear as a coloured shape in the output — no colour bleed from the DeepConstant.

**Failure signal:**  
- Holdout object appears as a coloured blurred shape → holdout colour channels are leaking into the scatter.  
- Highlight bokeh is visible through the holdout → transmittance_at is not blocking contributions correctly.

---

### TC-4: Holdout at depth deeper than sample — scatter contribution unaffected

**Purpose:** Confirm that a holdout sample placed **behind** a scatter contribution does NOT attenuate it.

**Setup in Nuke:**
1. Use the same bright highlight at depth Z = 2.0 (out of focus).
2. Create a DeepConstant with alpha = 1.0 and depth range [8.0, 10.0] (behind the highlight).
3. Connect to DeepCDefocusPO input 1 (holdout).

**Steps:**
1. Render DeepCDefocusPO. Examine the bokeh from the highlight.

**Expected outcome:**  
The bokeh output is **identical** to the no-holdout case (TC-2 baseline). The holdout sample at Z=8–10 does not affect scatter contributions from Z=2.0.

**Failure signal:** Bokeh is attenuated even though the holdout is behind the main sample → the `hzf >= Z` guard is inverted or missing.

---

### TC-5: Partial-depth holdout — graduated attenuation front-to-back

**Purpose:** Confirm transmittance accumulates front-to-back correctly across multiple holdout samples, matching Nuke DeepHoldout semantics.

**Setup in Nuke:**
1. Use a bright highlight at depth Z = 10.0.
2. Create a stack of 3 holdout samples at the same output pixel:
   - Sample A: depth [1.0, 2.0], alpha = 0.5
   - Sample B: depth [3.0, 4.0], alpha = 0.5
   - Sample C: depth [5.0, 6.0], alpha = 0.5
3. Connect the holdout stack to DeepCDefocusPO input 1.

**Steps:**
1. Render and read the pixel value of the bokeh output at the holdout location.
2. Compare to the no-holdout baseline from TC-2.

**Expected outcome:**  
The output value should be approximately `baseline × (1-0.5) × (1-0.5) × (1-0.5)` = `baseline × 0.125`.  
The `transmittance_at` function multiplies `(1 - alpha_i)` for each holdout sample where `hzf < Z`.  
This matches Nuke's DeepHoldout compositing formula.

**Failure signal:** The attenuated value differs significantly from 0.125× the baseline → the product accumulation is iterating incorrectly (e.g. summing instead of multiplying, or including samples behind Z).

---

### TC-6: Holdout mask is sharp — not defocused through the lens

**Purpose:** Confirm R024: the holdout acts as a crisp depth-resolved mask, not as a blurred shape.

**Setup in Nuke:**
1. Create a background Deep render with a large bright highlight at Z = 5.0 (significantly out of focus).
2. Create a DeepConstant with a hard-edge geometric shape (e.g. a square), alpha=1.0, at depth Z = 2.0.
3. Connect the DeepConstant as the holdout.
4. Also connect the same DeepConstant to a standalone DeepCDefocusPO (no holdout) to see its defocused appearance.

**Steps:**
1. Render the main DeepCDefocusPO with the holdout.
2. Compare the holdout boundary in the output to the same boundary rendered with no holdout.

**Expected outcome:**  
The holdout boundary is **sharp** (same pixel-level crisp edge as the DeepConstant itself). The holdout does **not** appear blurred or bokeh-shaped. Only the background bokeh is attenuated behind the holdout boundary.

**Failure signal:** The holdout edge appears fuzzy or follows the bokeh disc shape → holdout is being scattered through the lens rather than evaluated at the output pixel.

---

### TC-7: No double-defocus — holdout node itself not blurred

**Purpose:** Confirm that elements composited via the holdout input are never defocused a second time (the core design goal of R024).

**Setup:**
1. Create a scene where a foreground element (roto or flat plate) at Z = 0.5 would normally interfere with a background Deep bokeh at Z = 8.0.
2. Connect the foreground as the holdout.

**Steps:**
1. Render DeepCDefocusPO with and without the holdout.
2. In the holdout output, the foreground holdout geometry should appear sharp (as if composited over the defocused background with no additional blur).
3. The holdout-masked background bokeh should still be visible where the foreground holdout is absent.

**Expected outcome:**  
The foreground element retains its original sharpness. Background bokeh is correctly masked where the foreground is present. There is no "bokeh halo" around the foreground edge from holdout leakage.

**Failure signal:** Foreground appears blurred, or a bokeh-disc shaped halo is visible around the foreground edge.

---

## Edge Cases

### EC-1: Holdout with zero samples at a pixel

If the holdout has no samples at an output pixel (empty pixel), `transmittance_at` must return **1.0f** (fully transparent — no masking). The scatter contribution at that pixel must be unaffected.

### EC-2: Holdout with very high sample count

With many semi-transparent holdout samples, the product `∏(1 - alpha_i)` approaches zero. Verify no underflow below 0.0f — `std::max(0.0f, T)` in the implementation clamps this. Output should be near-black but not negative.

### EC-3: Holdout alpha exactly 1.0 (fully opaque single sample)

`T *= (1 - 1.0f) = 0.0f`. The `std::max(0.0f, 0.0f)` returns 0.0f. Scatter contribution is fully zeroed. No NaN or negative output.

### EC-4: Holdout connected but deepEngine returns false

When `holdoutOp->deepEngine(...)` returns false, `holdoutConnected` stays false. The scatter engine treats this as no holdout — identity path. Should not crash.

---

## Failure Signals Summary

| Symptom | Likely Cause |
|---------|-------------|
| Holdout disconnected → blank output | `holdoutConnected` default path not returning 1.0f |
| Holdout colour appearing in output | Colour channels requested in `getRequests` for holdout |
| Background visible through opaque foreground holdout | `hzf >= Z` guard inverted |
| Background masked even when holdout is behind it | `hzf >= Z` guard missing (treating all samples as blockers) |
| Holdout boundary blurred/bokeh-shaped | holdout evaluated at input pixel position, not output pixel |
| Double-defocus halo around holdout edge | holdout scattered through lens rather than post-evaluated |
| Partial holdout product wrong (not 0.125×) | Summing alphas instead of multiplying transmittances |

---

## Notes for Tester

- S03 verification is primarily by visual inspection in Nuke — the workspace gate (TC-1) is the only machine-executable check.
- The final integration test (TC-7 double-defocus check) is the primary design goal and most important visual check.
- Compare against pgBokeh or Nuke's built-in Bokeh node to demonstrate the holdout advantage.
