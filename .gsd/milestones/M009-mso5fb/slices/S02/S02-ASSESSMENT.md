---
sliceId: S02
uatType: artifact-driven
verdict: PASS
date: 2026-03-28T10:35:00Z
---

# UAT Result — S02

## Checks

| Check | Mode | Result | Notes |
|-------|------|--------|-------|
| TC-01: docker-build.sh artifacts exist (DeepCOpenDefocus.so + zip) | artifact | PASS | `build/16.0-linux/src/DeepCOpenDefocus.so` — 21 MB; `release/DeepC-Linux-Nuke16.0.zip` — 10 MB; zip contains `DeepC/DeepCOpenDefocus.so` (21466896 bytes). Full docker re-run not repeated (incremental artifacts confirmed fresh from prior T04 run — 2026-03-28 07:18). |
| TC-02: libopendefocus_deep.a exists ≥55 MB; Cargo.lock opendefocus 0.1.10 | artifact | PASS | `target/release/libopendefocus_deep.a` — 58M (mtime 2026-03-28 07:16). Cargo.lock: `name = "opendefocus"` / `version = "0.1.10"` confirmed. |
| TC-03: opendefocus_deep_render exported as T symbol | artifact | PASS | `nm build/16.0-linux/src/DeepCOpenDefocus.so \| grep opendefocus_deep_render` → `00000000001ebcd0 T opendefocus_deep_render` — T = defined text symbol, not U (undefined). |
| TC-04: verify-s01-syntax.sh exits 0, all expected lines present | runtime | PASS | Output (exact): `Syntax check passed: DeepCBlur.cpp` / `Syntax check passed: DeepCDepthBlur.cpp` / `Syntax check passed: DeepCOpenDefocus.cpp` / `All syntax checks passed.` / `test/test_opendefocus.nk exists — OK` / `All S02 checks passed.` Exit code 0. |
| TC-05: LensParams typedef with 4 float fields in C header | artifact | PASS | `grep -A8 "LensParams" opendefocus_deep.h` shows `typedef struct LensParams { float focal_length_mm; float fstop; float focus_distance; float sensor_size_mm; } LensParams;`. Prototype line: `void opendefocus_deep_render(... const LensParams* lens);` — 5th parameter confirmed. |
| TC-06: test/test_opendefocus.nk has DeepConstant + DeepCOpenDefocus + Write nodes | artifact | PASS | DeepConstant: format `128 72`, color `{0.5 0.5 0.5 1.0}`, `deep_front 5.0`. DeepCOpenDefocus: `focal_length 50`, `fstop 2.8`, `focus_distance 5.0`, `sensor_size_mm 36`. Write: `file /tmp/test_opendefocus.exr`. All node types present. |
| TC-07: Cargo.toml has opendefocus wgpu, ndarray, futures, once_cell, staticlib | artifact | PASS | `opendefocus = { version = "0.1.10", features = ["std", "wgpu"] }`, `ndarray = "0.16"`, `futures = "0.3"`, `once_cell = "1"`, `crate-type = ["staticlib"]` — all present. |
| TC-08: lib.rs implements layer-peel algorithm with block_on, render_stripe, accum, LensParams | artifact | PASS | `block_on` present (line 19, 45, 56, 317). `render_stripe` call at line 317: `block_on(renderer.render_stripe(...))`. Accumulator buffer at lines 209-343: front-to-back "over" composite. `LensParams` with `#[repr(C)]` at line 71/87. Layer-peel architecture comment at lines 6-15. |
| TC-09: nuke -x test/test_opendefocus.nk EXR output | human-follow-up | NEEDS-HUMAN | Nuke is not installed in the CI/worktree environment. Cannot automate. Requires a Nuke 16.0 installation with DeepCOpenDefocus.so in the plugin path. Steps: (1) copy `.so` to Nuke plugin path, (2) `nuke -x test/test_opendefocus.nk`, (3) verify `/tmp/test_opendefocus.exr` non-black. Deferred to S03 or manual UAT. |

## Overall Verdict

**PASS** — All 8 automatable checks passed. TC-09 (Nuke runtime EXR output) is correctly marked NEEDS-HUMAN as Nuke is not available in the CI environment; this is a known S02 limitation explicitly documented in the slice summary.

## Notes

- TC-01 reuses artifacts from the prior T04 docker build (2026-03-28 07:18) rather than re-running docker. The docker re-run is not required to validate artifact presence — the files exist, sizes are correct, and the zip contains the expected payload.
- TC-03 confirms the critical FFI contract: `opendefocus_deep_render` is a defined (T) symbol, not undefined (U), proving the Rust staticlib was successfully linked into the .so.
- TC-06 confirmed all .nk knob values match the expected values exactly: `focal_length 50`, `fstop 2.8`, `focus_distance 5.0`, `sensor_size_mm 36`, `deep_front 5.0`.
- TC-08 confirmed both `#[repr(C)]` annotations (lines 71 and 87 in lib.rs) covering LensParams and the input data struct. The front-to-back composite formula at lines 340-343 matches R058: `out = layer + (1 - layer_alpha) * accum`.
- TC-09 (NEEDS-HUMAN) instructions: Copy `build/16.0-linux/src/DeepCOpenDefocus.so` to Nuke 16.0 plugin directory, run `nuke -x test/test_opendefocus.nk`, confirm `/tmp/test_opendefocus.exr` exists and is non-black. This is a S03 gate item.
