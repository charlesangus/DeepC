# S03: Holdout input + camera node link + polish — UAT

**Milestone:** M009-mso5fb
**Written:** 2026-03-28T11:44:08.678Z


## UAT: S03 — Holdout input + camera node link + polish

### Preconditions
- Nuke 16.0+ with a valid license installed
- DeepCOpenDefocus.so built at `build/16.0-linux/src/DeepCOpenDefocus.so` (21 MB)
- test/test_opendefocus.nk present with correct knob names
- icons/DeepCOpenDefocus.png present (64x64 RGBA PNG)
- A Deep EXR test sequence available (or use the included test scene)

---

### Test Case 1: Node appears in Nuke Filter menu with icon

**Precondition:** DeepCOpenDefocus.so in NUKE_PATH, icons/ dir in NUKE_PATH  
**Steps:**
1. Launch Nuke 16.0 with the plugin loaded
2. Open the node Graph and press Tab or use the Nodes menu
3. Filter by "DeepCOpenDefocus"

**Expected:**  
- Node appears under the Filter category  
- 64×64 aperture-iris icon (blue/cyan 6-blade) shown next to the node name  
- Node can be instantiated and shows 3 inputs: [0] Deep (main), [1] Deep (holdout), [2] Camera

---

### Test Case 2: Basic defocus — no holdout, no camera link

**Precondition:** A deep EXR with depth variation, no holdout connected  
**Steps:**
1. Place a DeepReader → DeepCOpenDefocus → DeepToImage chain
2. Set focal_length=50, fstop=2.0, focus_distance=100, sensor_size_mm=36
3. Render a single frame

**Expected:**
- Non-black RGBA output with visible defocus blur on objects outside focus distance
- No crash or NaN in output
- Stderr contains: `DeepCOpenDefocus: camera link inactive, using manual knobs`

---

### Test Case 3: Holdout attenuation — connected Deep input on input(1)

**Precondition:** A second Deep source (roto/paint element) available  
**Steps:**
1. Connect a DeepCConstant or second DeepReader to input(1) of DeepCOpenDefocus
2. Set holdout source to have alpha=1.0 at known pixels (e.g. full frame solid)
3. Render and compare output with/without holdout connected

**Expected:**
- With holdout alpha=1.0 (fully opaque): output pixels behind the holdout are fully attenuated (black/transparent)
- With holdout alpha=0.5: output pixels attenuated to 50% (T = 1−0.5 = 0.5)
- With holdout alpha=0.0 (fully transparent): output unchanged vs. no holdout
- Stderr contains: `DeepCOpenDefocus: holdout active, attenuated N pixels` (N > 0)

---

### Test Case 4: Holdout depth-dependence

**Precondition:** Deep scene with foreground and background objects at different depths  
**Steps:**
1. Create a holdout Deep source with samples only at near depth (e.g. Z=50 cm)
2. Connect to input(1)
3. Render and observe which pixels are attenuated

**Expected:**
- Pixels where the main input has samples *behind* the holdout are attenuated
- Pixels where the main input has samples *in front of* the holdout are not attenuated (T ≈ 1.0 since holdout sample is further)
- Net result: near objects pass through; far objects are masked

---

### Test Case 5: CameraOp link — camera overrides manual knobs

**Precondition:** A Camera node in the scene with known focal_length/fstop/focus_distance  
**Steps:**
1. Create a Camera node, set focal_length=35, fstop=1.4, projection_distance=200
2. Connect the Camera to input(2) of DeepCOpenDefocus
3. Render; observe defocus vs. manual knob values

**Expected:**
- Defocus matches Camera node parameters (35mm, f/1.4, 200-unit focus), NOT manual knob values
- Manual knob values can be changed freely — they have no effect while camera is connected
- Stderr contains: `DeepCOpenDefocus: camera link active (focal_length=35 fstop=1.4 focus=200 sensor=...)`
- Knob tooltips note "(overridden by Camera input when connected)"

---

### Test Case 6: CameraOp disconnect reverts to manual knobs

**Precondition:** Continuation of Test Case 5  
**Steps:**
1. Disconnect the Camera from input(2)
2. Re-render with manual knob focal_length=85, fstop=5.6

**Expected:**
- Defocus changes to match manual values (85mm, f/5.6)
- Stderr contains: `DeepCOpenDefocus: camera link inactive, using manual knobs`

---

### Test Case 7: test/test_opendefocus.nk loads and renders

**Steps:**
1. `nuke -x test/test_opendefocus.nk`
2. Check output EXR exists at expected path

**Expected:**
- nuke -x exits 0
- Output EXR is non-black (at least some pixels have R, G, B, A > 0)
- No Python errors or DDImage warnings about unknown knobs

---

### Test Case 8: THIRD_PARTY_LICENSES.md attribution

**Steps:**
1. `grep -A6 '## opendefocus' THIRD_PARTY_LICENSES.md`

**Expected:**
- Entry present with Author: Gilles Vink, License: EUPL-1.2
- Full EUPL-1.2 licence text included

---

### Edge Cases

| Scenario | Expected Behaviour |
|---|---|
| Holdout input connected but has zero deep samples at a pixel | T = 1.0 (no attenuation), output unchanged |
| Holdout alpha > 1.0 on a sample | Clamped to [0,1] before transmittance calc |
| Camera input connected but not validated | cam->validate(false) called internally — safe |
| Camera film_width returns 0 | sensor_size_mm = 0 mm; opendefocus FFI receives 0 (degenerate, not a crash) |
| Main Deep input empty (no samples) | FFI call with empty buffer — output is black; no crash |

