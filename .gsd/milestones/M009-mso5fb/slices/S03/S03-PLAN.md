# S03: Holdout input + camera node link + polish

**Goal:** Complete the user-facing capability: implement C++ holdout transmittance attenuation (post-defocus, no FFI changes), wire camera node link to override lens knobs, add the test .nk script and node icon, and verify the full milestone DoD.
**Demo:** After this: Holdout Deep input attenuates behind its samples without being defocused. Camera node link drives lens parameters. Menu registration, icon, test .nk, THIRD_PARTY_LICENSES.md attribution. Full milestone DoD satisfied.

## Tasks
- [x] **T01: Added per-pixel holdout transmittance attenuation (T = product of 1−alpha_s) applied to defocused output_buf after FFI call, gated by dynamic_cast<DeepOp*>(input(1)), plus output_buf→ImagePlane copy replacing old zeros scaffold** — Implement holdout attenuation entirely in C++ renderStripe after the FFI call returns the defocused flat RGBA buffer.

Approach (C++ only, no FFI changes):
1. After `opendefocus_deep_render()` returns, check whether input(1) (holdout) is connected and is a DeepOp.
2. If connected: fetch the holdout DeepPlane for the output bounds.
3. For each output pixel (x, y):
   a. Retrieve the holdout deep samples at that pixel.
   b. Sort samples by depth front-to-back.
   c. Compute transmittance T = product of (1 - alpha_s) for each holdout sample.
   d. Multiply the defocused output_rgba[pixel] channels by T (premultiplied: multiply all 4 channels including alpha by T, then re-premultiply if needed — or simply multiply rgb*T and alpha*T since holdout attenuates coverage).
4. If input(1) not connected: no-op, output unchanged.
5. Add helper function `computeHoldoutTransmittance(const DeepPixel& dp) -> float` for testability.
6. Run scripts/verify-s01-syntax.sh to confirm C++ compiles.
  - Estimate: 2h
  - Files: src/DeepCOpenDefocus.cpp
  - Verify: scripts/verify-s01-syntax.sh passes. Grep confirms computeHoldoutTransmittance in DeepCOpenDefocus.cpp. Grep confirms input(1) gated by dynamic_cast check.
- [x] **T02: Wired CameraOp input(2) to override lens knob values (focal_length, fstop, focus_distance, sensor_size_mm) before the LensParams FFI struct is built, with active/inactive stderr logs and updated knob tooltips** — Wire the CameraOp input (input 2) to override the lens knob values before the LensParams struct is constructed for the FFI call.

Following the existing opendefocus-nuke pattern:
1. In renderStripe (or _validate), retrieve `CameraOp* cam = dynamic_cast<CameraOp*>(Op::input(2))`.
2. If cam is not null: call `cam->validate()` then extract:
   - focal_length = cam->focal_length() (mm)
   - fstop = cam->fStop()
   - focus_distance = cam->projection_distance()
   - sensor_size_mm = cam->film_width() * 10.0f (filmback cm -> mm)
3. If cam is null: use the manual knob values (already in place).
4. Add a fprintf(stderr, ...) INFO log: 'DeepCOpenDefocus: camera link %s' (active/inactive).
5. Add brief tooltip comments to the Float_knob declarations noting units and camera override behavior.
6. Run verify script.
  - Estimate: 1h
  - Files: src/DeepCOpenDefocus.cpp
  - Verify: scripts/verify-s01-syntax.sh passes. Grep confirms cam->focal_length() and cam->fStop() usage in DeepCOpenDefocus.cpp.
- [x] **T03: Created icons/DeepCOpenDefocus.png (64x64 aperture-iris PNG) and verified FILTER_NODES/menu.py.in registration and .nk knob names** — Add the node icon and verify menu registration.

1. Create `icons/DeepCOpenDefocus.png` — a 48x48 or 64x64 PNG. Generate a simple placeholder icon (circular aperture shape in blue/white tones) using Python + PIL or a base64-embedded minimal PNG. If no image tool is available, copy and recolor an existing icon from the icons/ directory.
2. Verify that `src/CMakeLists.txt` already lists DeepCOpenDefocus in FILTER_NODES (it does per S01) — no CMake changes needed.
3. Check that `python/menu.py.in` (or generated menu.py) includes the DeepCOpenDefocus entry — confirm with grep.
4. Update `test/test_opendefocus.nk` if needed to match any knob name changes from S02/S03.
5. Add a comprehensive comment block at the top of DeepCOpenDefocus.cpp documenting the node's inputs, knobs, behavior, and the layer-peel approach.
  - Estimate: 1h
  - Files: icons/DeepCOpenDefocus.png, src/DeepCOpenDefocus.cpp
  - Verify: ls icons/DeepCOpenDefocus.png returns file. file icons/DeepCOpenDefocus.png shows PNG data. grep -r DeepCOpenDefocus src/CMakeLists.txt confirms FILTER_NODES entry.
- [x] **T04: Fixed CameraOp projection_distance()→focal_point() compile error, added static_cast&lt;float&gt; for double returns, rebuilt DeepCOpenDefocus.so via Docker exit 0, and confirmed all 9 milestone DoD items pass** — Run the full milestone DoD checklist and confirm all items pass.

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
  - Estimate: 1h
  - Files: docker-build.sh
  - Verify: docker-build.sh exits 0. All 9 DoD items checked. scripts/verify-s01-syntax.sh passes. THIRD_PARTY_LICENSES.md grep confirms opendefocus EUPL-1.2 entry.
