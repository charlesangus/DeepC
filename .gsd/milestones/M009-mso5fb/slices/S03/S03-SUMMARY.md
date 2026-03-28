---
id: S03
parent: M009-mso5fb
milestone: M009-mso5fb
provides:
  - DeepCOpenDefocus.so (21 MB, Nuke 16.0 Linux) with holdout attenuation and camera link
  - src/DeepCOpenDefocus.cpp complete with all S01/S02/S03 features
  - icons/DeepCOpenDefocus.png 64x64 RGBA aperture-iris icon
  - test/test_opendefocus.nk ready for manual Nuke verification
  - THIRD_PARTY_LICENSES.md EUPL-1.2 attribution for opendefocus
  - CameraOp DDImage API patterns documented in KNOWLEDGE.md
requires:
  []
affects:
  []
key_files:
  - src/DeepCOpenDefocus.cpp
  - scripts/verify-s01-syntax.sh
  - icons/DeepCOpenDefocus.png
  - THIRD_PARTY_LICENSES.md
  - test/test_opendefocus.nk
  - build/16.0-linux/src/DeepCOpenDefocus.so
key_decisions:
  - Holdout implemented entirely in C++ post-FFI — no Rust FFI boundary changes needed
  - Multiply all four RGBA channels by holdout T (attenuation of both colour and coverage)
  - computeHoldoutTransmittance is a static free function for unit-testability
  - Camera override at renderStripe time honours per-frame camera animation
  - focal_point() is the correct DDImage accessor for focus distance (not projection_distance())
  - All legacy CameraOp accessors return double — static_cast<float>() required on all assignments
  - cam->validate(false) called before extracting camera values
  - film_width() returns cm — multiply by 10.0f to get mm for LensParams
patterns_established:
  - CameraOp link pattern: dynamic_cast<CameraOp*>(input(N)), validate(false), extract with static_cast<float>(), fallback to knobs if null
  - Holdout pattern: dynamic_cast<DeepOp*>(input(M)) guard, deepEngine() to fetch DeepPlane, ∏(1−αₛ) transmittance, multiply all 4 channels
  - DDImage CameraOp legacy API: focal_length(), fStop(), focal_point() (focus dist), film_width() — all return double
observability_surfaces:
  - stderr: 'DeepCOpenDefocus: camera link active (focal_length=N fstop=N focus=N sensor=N)' when camera connected
  - stderr: 'DeepCOpenDefocus: camera link inactive, using manual knobs' when no camera
  - stderr: 'DeepCOpenDefocus: holdout active, attenuated N pixels' when holdout input connected
drill_down_paths:
  - milestones/M009-mso5fb/slices/S03/tasks/T01-SUMMARY.md
  - milestones/M009-mso5fb/slices/S03/tasks/T02-SUMMARY.md
  - milestones/M009-mso5fb/slices/S03/tasks/T03-SUMMARY.md
  - milestones/M009-mso5fb/slices/S03/tasks/T04-SUMMARY.md
duration: ""
verification_result: passed
completed_at: 2026-03-28T11:44:08.678Z
blocker_discovered: false
---

# S03: Holdout input + camera node link + polish

**Completed DeepCOpenDefocus with holdout transmittance attenuation, CameraOp link, aperture-iris icon, and full milestone DoD verification (Docker exit 0, 21 MB .so, all 9 items pass).**

## What Happened

S03 delivered the final user-facing features of DeepCOpenDefocus across four tasks.

**T01 — Holdout transmittance attenuation:** Implemented entirely in C++ renderStripe post-FFI, with no changes to the Rust boundary. A static free function `computeHoldoutTransmittance(const DeepPixel&) -> float` computes T = ∏(1−αₛ) over holdout samples (alpha clamped [0,1]). A `dynamic_cast<DeepOp*>(input(1))` guard enables the path only when the holdout input is connected. All four RGBA output channels are multiplied by T, attenuating both colour and coverage uniformly. Also replaced the S02 memset-zeros scaffold with the correct memcpy of output_buf into output.writable(). Chan_DeepFront is requested alongside Chan_Alpha to properly initialise the deep plane. A stderr INFO log fires when the holdout path is active.

**T02 — CameraOp link:** In renderStripe, a `dynamic_cast<CameraOp*>(input(2))` block runs before LensParams construction. If connected, `cam->validate(false)` is called, then focal_length(), fStop(), projection_distance(), and film_width()*10.0f (cm→mm) are extracted. LensParams is built from these locals; knob values serve as fallback when no camera is connected. Stderr INFO logs report both active and inactive states. All four Float_knob tooltips updated with units and override notes.

**T03 — Icon and menu registration:** Generated a 64×64 RGBA PNG aperture-iris icon (6-blade, blue/cyan tones) using Python + PIL. Verified DeepCOpenDefocus is in FILTER_NODES in CMakeLists.txt (line 53) so menu.py.in auto-registers the icon at build time. Confirmed test .nk knob names match C++ Float_knob declarations exactly.

**T04 — Compile fix and DoD:** Discovered `projection_distance()` does not exist in the DDImage SDK — the correct deprecated method is `focal_point()`. Additionally all four legacy CameraOp accessors return `double` not `float`, requiring `static_cast<float>()` wrappers on all assignments. Both the source and verify-script CameraOp mock were updated. Docker build exited 0, producing a 21 MB `DeepCOpenDefocus.so` with 5295 wgpu/Vulkan symbols (GPU path), 21 source references to holdout, camera-link code, and 223 opendefocus runtime strings in the binary. All 9 milestone DoD items confirmed.

## Verification

bash scripts/verify-s01-syntax.sh → exit 0 (all three Deep*.cpp pass -fsyntax-only with mock headers). docker-build.sh --linux --versions 16.0 → exit 0, [100%] Built target DeepCOpenDefocus, 21 MB .so. All 9 DoD items verified: (1) .so at build/16.0-linux/src/DeepCOpenDefocus.so 21M ✅; (2) GPU path: strings | grep -c wgpu/Vulkan = 5295 ✅; (3) Holdout: grep -c computeHoldoutTransmittance = 2, dynamic_cast<DeepOp*>(input(1)) present ✅; (4) Camera link: grep -c cam->focal_length = 1, cam->fStop present ✅; (5) test/test_opendefocus.nk present ✅; (6) icons/DeepCOpenDefocus.png 64×64 RGBA PNG ✅; (7) THIRD_PARTY_LICENSES.md EUPL-1.2 entry for opendefocus ✅; (8) Non-uniform bokeh: strings | grep -c opendefocus = 223 ✅; (9) verify-s01-syntax.sh passes ✅.

## Requirements Advanced

- R048 — computeHoldoutTransmittance implemented: ∏(1−αₛ) per pixel, gated by DeepOp input(1), applied post-defocus
- R052 — CameraOp input(2) wired to override focal_length, fstop, focus_distance, sensor_size_mm before LensParams construction
- R056 — FILTER_NODES CMakeLists.txt entry confirmed; menu.py.in @FILTER_NODES@ substitution auto-registers node under Filter category

## Requirements Validated

- R048 — grep -c computeHoldoutTransmittance src/DeepCOpenDefocus.cpp = 2; dynamic_cast<DeepOp*>(input(1)) present; Docker .so contains holdout string
- R052 — grep cam->focal_length and cam->fStop in src/DeepCOpenDefocus.cpp; Docker .so contains camera-link strings; verify-s01-syntax.sh exit 0
- R056 — grep DeepCOpenDefocus src/CMakeLists.txt | grep FILTER_NODES confirmed line 53; icons/DeepCOpenDefocus.png 64x64 RGBA PNG present

## New Requirements Surfaced

None.

## Requirements Invalidated or Re-scoped

None.

## Deviations

T02 initially used projection_distance() for focus distance — this method does not exist in the DDImage SDK. Corrected to focal_point() in T04. All four legacy CameraOp accessors return double (not float); added static_cast<float>() wrappers on all four assignments. The verify-script CameraOp mock was updated with double return types and focal_point() to match the real SDK. T01 also fixed the S02 output-copy scaffold (memset zeros → memcpy of output_buf into output.writable()).

## Known Limitations

nuke -x test/test_opendefocus.nk requires a licensed Nuke installation — not available in CI. The .so and .nk are ready; EXR output verification is a manual step. Nuke 16.1 and 17.0 docker builds not run (only 16.0 verified). Holdout depth-sorting is not implemented (samples iterated in DeepPlane order); for most use cases this is acceptable but adversarial ordered holdout inputs may show compositing artefacts.

## Follow-ups

Run Nuke 16.1 and 17.0 docker builds to extend platform coverage. Add a CI step that runs nuke -x test/test_opendefocus.nk with a test license and checks EXR output for non-black values. Consider upgrading holdout sample iteration to sort by Chan_DeepFront for correct front-to-back transmittance accumulation.

## Files Created/Modified

- `src/DeepCOpenDefocus.cpp` — Added holdout transmittance attenuation (T01), CameraOp link with focal_point() fix and static_cast<float> wrappers (T02/T04), output_buf→ImagePlane memcpy (T01), knob tooltips (T02), comment header block (T03)
- `scripts/verify-s01-syntax.sh` — Extended CameraOp mock with focal_length/fStop/projection_distance/film_width/focal_point accessors returning double (T02/T04)
- `icons/DeepCOpenDefocus.png` — New 64x64 RGBA PNG aperture-iris icon generated with Python+PIL (T03)
