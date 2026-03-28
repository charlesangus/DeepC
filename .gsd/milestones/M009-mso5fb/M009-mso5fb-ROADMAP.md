# M009-mso5fb: 

## Vision
A new DeepCOpenDefocus Nuke plugin that takes Deep image input and produces flat 2D RGBA output with physically-based, GPU-accelerated defocus using a forked opendefocus library. Per-deep-sample circle of confusion, holdout attenuation, all non-uniform bokeh artifacts, camera node link.

## Slice Overview
| ID | Slice | Risk | Depends | Done | After this |
|----|-------|------|---------|------|------------|
| S01 | Fork + build integration + FFI scaffold | high | — | ✅ | S01 complete. opendefocus-deep Rust static lib exists (55.9 MB), extern C FFI defined, CMake links it, docker-build.sh installs Rust stable before cmake, DeepCOpenDefocus.so loads in Nuke as a no-op PlanarIop. scripts/verify-s01-syntax.sh passes. |
| S02 | Deep defocus engine — layer peeling + GPU dispatch | high | S01 | ✅ | Deep input → visibly defocused flat RGBA output. Per-sample CoC from real depth. GPU-accelerated convolution with all non-uniform bokeh artifacts. Front-to-back depth-correct compositing. nuke -x test/test_opendefocus.nk produces non-black 128×72 EXR. |
| S03 | Holdout input + camera node link + polish | medium | S02 | ✅ | Holdout Deep input attenuates behind its samples without being defocused. Camera node link drives lens parameters. Menu registration, icon, test .nk, THIRD_PARTY_LICENSES.md attribution. Full milestone DoD satisfied. |
