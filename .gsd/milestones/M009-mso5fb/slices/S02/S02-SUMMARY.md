---
id: S02
parent: M009-mso5fb
milestone: M009-mso5fb
provides:
  - DeepCOpenDefocus.so (21 MB) — Nuke plugin with full opendefocus 0.1.10 GPU/CPU defocus engine linked in
  - libopendefocus_deep.a (58 MB) — Rust staticlib with layer-peel opendefocus_deep_render implementation
  - LensParams FFI struct — focal_length_mm, fstop, focus_distance, sensor_size_mm carried across C++/Rust boundary
  - test/test_opendefocus.nk — ready-to-run Nuke test script (DeepConstant → DeepCOpenDefocus → Write)
  - docker-build.sh — fixed to work with Rocky Linux 8 nukedockerbuild containers (cc symlink + protoc + dnf support)
  - Layer-peel algorithm pattern established for future deep spatial ops
requires:
  []
affects:
  - S03 — depends on DeepCOpenDefocus.so, LensParams FFI struct, and layer-peel render pattern; must activate RENDERER singleton, add holdout input, camera node link, menu registration, and THIRD_PARTY_LICENSES attribution
key_files:
  - crates/opendefocus-deep/src/lib.rs
  - crates/opendefocus-deep/Cargo.toml
  - crates/opendefocus-deep/include/opendefocus_deep.h
  - src/DeepCOpenDefocus.cpp
  - test/test_opendefocus.nk
  - scripts/verify-s01-syntax.sh
  - docker-build.sh
  - Cargo.lock
key_decisions:
  - docker-build.sh: replaced which/apt-get with command -v + dnf/apt-get detection for Rocky Linux 8 compatibility
  - CoC driven internally by opendefocus via CameraData + DefocusMode::Depth — no circle-of-confusion direct dep
  - ndarray = 0.16 added to Cargo.toml (lib.rs uses Array2/Array3 directly)
  - proto-generated enum fields require .into() for i32 assignment
  - camera_data nested at settings.defocus.circle_of_confusion.camera_data
  - focus_distance maps to focal_plane on CoC Settings; CameraData uses near_field/far_field
  - DD::Image::Descriptions absent from Nuke 16.0 — getRequirements override removed
  - Renderer created per-call in T04; singleton scaffolded for S03
  - verify-s01-syntax.sh S02 checks appended to existing script (one entry-point)
  - POSIX sh compat: BASH_SOURCE[0] → $0, set -euo pipefail → set -eu
patterns_established:
  - Layer-peel deep defocus: sort per-pixel samples front-to-back, iterate max_layers calling opendefocus render_stripe per layer with per-pixel depth array, front-to-back alpha composite into accumulator
  - opendefocus Settings configuration for deep: set CameraData (focal_length, f_stop, filmback, near/far_field) under settings.defocus.circle_of_confusion.camera_data; set focal_plane on settings.defocus.circle_of_confusion; set defocus_mode to DefocusMode::Depth.into()
  - docker-build.sh prerequisites pattern for Rocky Linux 8: command -v dnf/apt-get detection, cc symlink creation, protobuf-compiler install before cargo build
observability_surfaces:
  - nm build/16.0-linux/src/DeepCOpenDefocus.so | grep opendefocus_deep_render — confirms T symbol exported
  - scripts/verify-s01-syntax.sh — combined S01+S02 syntax + file existence checks
  - Cargo.lock — opendefocus 0.1.10 dep resolution record
  - docker-build.sh --linux --versions 16.0 — authoritative build pass/fail
drill_down_paths:
  - milestones/M009-mso5fb/slices/S02/tasks/T01-SUMMARY.md
  - milestones/M009-mso5fb/slices/S02/tasks/T02-SUMMARY.md
  - milestones/M009-mso5fb/slices/S02/tasks/T03-SUMMARY.md
  - milestones/M009-mso5fb/slices/S02/tasks/T04-SUMMARY.md
duration: ""
verification_result: passed
completed_at: 2026-03-28T11:26:09.374Z
blocker_discovered: false
---

# S02: Deep defocus engine — layer peeling + GPU dispatch

**Full layer-peel deep defocus engine implemented in Rust, linked via extern-C FFI, building to DeepCOpenDefocus.so (21 MB) with opendefocus 0.1.10 (248 deps, wgpu GPU path) resolved and confirmed by docker build exit 0.**

## What Happened

S02 delivered the full deep defocus engine across four tasks, each building on the previous.

**T01 — LensParams FFI extension:** Added a `LensParams #[repr(C)]` struct to `crates/opendefocus-deep/src/lib.rs` carrying focal_length_mm, fstop, focus_distance, and sensor_size_mm. Updated `include/opendefocus_deep.h` with the matching C typedef and function prototype. Updated `src/DeepCOpenDefocus.cpp` renderStripe call site to construct a LensParams on the stack from the knob values. Verified with scripts/verify-s01-syntax.sh (all three .cpp files pass g++ -fsyntax-only). Decision: all fields use f32/float avoiding any cross-boundary conversion.

**T02 — Full layer-peel engine implementation:** Implemented the complete `opendefocus_deep_render()` body using the layer-peel approach. Added opendefocus = "0.1.10" (std+wgpu), futures = "0.3", once_cell = "1" to Cargo.toml. The algorithm: (1) reconstruct per-pixel deep sample arrays from flat FFI input, (2) sort each pixel's samples front-to-back by depth, (3) for each layer 0..max_layers, build flat RGBA + depth arrays and configure opendefocus Settings with CameraData (focal_length, f_stop, filmback, near/far field) driving internal CoC via DefocusMode::Depth, (4) call renderer.render_stripe via futures::executor::block_on, (5) alpha-composite the defocused layer front-to-back into the accumulator buffer. Key decision: CoC computation is handled internally by opendefocus when CameraData + DefocusMode::Depth is configured — no direct circle-of-confusion dep needed. Also fixed verify-s01-syntax.sh for POSIX sh compatibility (replaced bash-isms: BASH_SOURCE[0] → $0, set -euo pipefail → set -eu).

**T03 — Test .nk and S02 checks:** Created `test/test_opendefocus.nk` as a Nuke script containing a DeepConstant node (128×72, non-zero RGBA, depth 5.0), connected to DeepCOpenDefocus (focal_length 50, fstop 2.8, focus_distance 5.0, sensor_size_mm 36), connected to a Write node outputting /tmp/test_opendefocus.exr. Extended verify-s01-syntax.sh to check that test/test_opendefocus.nk exists ("All S02 checks passed."). Decision: appended S02 checks to the existing script rather than a separate script to keep one entry-point.

**T04 — Docker build verification and API fixes:** Running the full cargo+cmake pipeline inside nukedockerbuild:16.0-linux (Rocky Linux 8) revealed three pre-build blockers and six API mismatches:
- Pre-build: (1) Rocky Linux 8 has no `cc` symlink — fixed with `ln -sf /usr/bin/gcc /usr/local/bin/cc`; (2) Rocky Linux 8 lacks `which` — `docker-build.sh` used `which curl` which exits 127 — fixed by switching to `command -v` with dnf/apt-get detection; (3) circle-of-confusion prost build requires `protoc` — fixed with `dnf install -y protobuf-compiler`.
- API mismatches in lib.rs: (1) ndarray dependency missing from Cargo.toml — lib.rs uses `Array2`/`Array3` directly; (2) proto-generated enum fields require `.into()` for i32 assignment; (3) `camera_data` is nested under `settings.defocus.circle_of_confusion.camera_data`, not directly on `settings.defocus`; (4) `CameraData` has no `focal_point` field — use `focal_plane` on CoC Settings + `near_field`/`far_field` on CameraData; (5) type annotation needed for `Array3`; (6) `DD::Image::Descriptions` does not exist in Nuke 16.0 SDK — removed `getRequirements` override from DeepCOpenDefocus.cpp entirely.
- After all fixes: cargo produced `libopendefocus_deep.a` (58 MB, 248 transitive deps resolved), cmake linked `DeepCOpenDefocus.so` (21 MB), `nm` confirms `opendefocus_deep_render` exported as T symbol, docker run exits 0 in 32s (incremental, 54s clean).
- Note: Nuke is not in the docker image — `nuke -x test/test_opendefocus.nk` must run in a Nuke-installed environment (S03 concern).

## Verification

All slice verification checks passed:

1. `bash docker-build.sh --linux --versions 16.0` — exits 0 (32s incremental / 54s clean). Builds DeepCOpenDefocus.so and packages release/DeepC-Linux-Nuke16.0.zip.
2. `ls -lh build/16.0-linux/src/DeepCOpenDefocus.so` — 21 MB present.
3. `ls -lh target/release/libopendefocus_deep.a` — 58 MB present. Cargo.lock confirms opendefocus = "0.1.10".
4. `nm build/16.0-linux/src/DeepCOpenDefocus.so | grep opendefocus_deep_render` — `00000000001ebcd0 T opendefocus_deep_render` (T = defined text symbol, exported).
5. `bash scripts/verify-s01-syntax.sh` — all three .cpp syntax checks pass + "test/test_opendefocus.nk exists — OK" + "All S02 checks passed."

## Requirements Advanced

- R046 — DeepCOpenDefocus.so built and linked: accepts Deep image, produces flat 2D RGBA via opendefocus convolution; confirmed by docker build exit 0 and nm symbol check
- R047 — Per-sample CoC driven by CameraData + DefocusMode::Depth in opendefocus Settings; LensParams (focal_length_mm, fstop, focus_distance, sensor_size_mm) passed from C++ knobs via FFI
- R049 — Layer-peel algorithm implemented: sort samples front-to-back, iterate layers calling render_stripe per layer with per-pixel depth, front-to-back composite
- R050 — opendefocus 0.1.10 with wgpu feature linked; GPU Vulkan path available in built .so (CPU fallback included)
- R051 — All opendefocus non-uniform bokeh artifacts (catseye, barndoor, astigmatism, axial aberration, inverse foreground) available via opendefocus 0.1.10 — same engine as upstream Nuke plugin
- R053 — Manual Float_knobs for focal_length_mm, fstop, focus_distance, sensor_size_mm present in DeepCOpenDefocus.cpp; values passed to LensParams FFI struct
- R058 — Front-to-back alpha composite implemented in Rust: out_rgba[i] = layer[i] + (1 - layer_alpha) * accum[i]; depth order from per-pixel sample sorting

## Requirements Validated

- R049 — Layer-peel algorithm: sort → iterate → render_stripe → front-to-back composite. docker build exit 0 confirms code compiles. Cargo.lock confirms opendefocus 0.1.10 resolved. nm confirms opendefocus_deep_render T symbol exported from DeepCOpenDefocus.so.

## New Requirements Surfaced

- nuke -x EXR non-black output test requires a Nuke-installed environment not present in nukedockerbuild — needs separate CI or manual UAT step

## Requirements Invalidated or Re-scoped

None.

## Deviations

docker-build.sh required a fix to the Linux build loop (replaced `which curl || apt-get install -y curl` with `command -v`-based dnf/apt-get detection + cc symlink setup) to work with Rocky Linux 8 nukedockerbuild container. Six API mismatches in lib.rs were discovered and fixed during T04 docker build (not anticipated in T02 plan — no local rustc was available to compile against real opendefocus 0.1.10 API). ndarray was added to Cargo.toml (implicit dep in T02 plan but lib.rs uses it directly). getRequirements override removed from DeepCOpenDefocus.cpp (DD::Image::Descriptions absent from Nuke 16.0 SDK). circle-of-confusion direct dep was omitted — CoC driven internally by opendefocus via CameraData + DefocusMode::Depth, which is simpler and correct.

## Known Limitations

nuke -x test/test_opendefocus.nk cannot be verified inside the nukedockerbuild docker image (Nuke not installed in container). The nuke -x EXR output test must run in a separate Nuke-installed environment (deferred to S03 or manual UAT). The RENDERER static in lib.rs currently creates a new renderer per-call; the singleton optimisation is scaffolded (RENDERER static with dead_code warning) and should be activated in S03. GPU backend selection (Vulkan vs CPU fallback) is not logged at this stage — the log line will be emitted once the renderer is properly initialized in a running Nuke process.

## Follow-ups

S03 must: (1) activate the RENDERER static singleton to avoid per-call GPU context creation overhead; (2) add holdout Deep input and C++ post-defocus attenuation (per D026); (3) add camera node link driving LensParams knobs; (4) register DeepCOpenDefocus in menu.py with icon; (5) add THIRD_PARTY_LICENSES.md attribution for opendefocus EUPL-1.2 fork; (6) run nuke -x test/test_opendefocus.nk in a Nuke environment to confirm non-black EXR output.

## Files Created/Modified

- `crates/opendefocus-deep/src/lib.rs` — Full layer-peel opendefocus_deep_render() implementation; LensParams #[repr(C)] struct; opendefocus Settings configuration; front-to-back alpha compositing
- `crates/opendefocus-deep/Cargo.toml` — Added opendefocus = {version=0.1.10, features=[std,wgpu]}, ndarray=0.16, futures=0.3, once_cell=1
- `crates/opendefocus-deep/include/opendefocus_deep.h` — Added LensParams typedef; updated opendefocus_deep_render prototype to accept const LensParams*
- `src/DeepCOpenDefocus.cpp` — renderStripe call site updated to construct LensParams from knob values; getRequirements override removed (DD::Image::Descriptions absent from Nuke 16.0 SDK)
- `test/test_opendefocus.nk` — Created: DeepConstant 128x72 depth 5.0 → DeepCOpenDefocus default lens params → Write /tmp/test_opendefocus.exr
- `scripts/verify-s01-syntax.sh` — Fixed POSIX sh compat (BASH_SOURCE→$0, set -euo→set -eu); appended S02 checks (test .nk existence check)
- `docker-build.sh` — Linux build loop: replaced which/apt-get with command -v + dnf/apt-get detection; added cc symlink creation; added protobuf-compiler install
- `Cargo.lock` — Updated to reflect opendefocus 0.1.10 + 248 transitive deps resolved
