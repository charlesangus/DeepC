# DeepC

## What This Is

DeepC is an open-source suite of deep-compositing Nuke NDK plugins targeting Foundry Nuke 16+. Ships 24 plugins covering color, matte, noise, shuffle, utility, blur, and deep-sample optimisation operations on deep images. Core plugins are built on two shared C++ base classes (`DeepCWrapper`, `DeepCMWrapper`); DeepThinner and DeepCBlur use `DeepFilterOp` directly. Highlights: DeepCShuffle2 features a full Qt6 custom knob UI with colored channel routing, DeepCPMatte provides an interactive 3D wireframe GL handle, DeepCPNoise supports true 4D noise evolution across all 9 noise types, DeepThinner reduces deep sample counts via seven independent artist-controllable passes, DeepCBlur provides Gaussian blur for deep images with sample propagation and per-pixel optimization, DeepCDepthBlur spreads each deep sample across sub-samples in Z with physically correct alpha decomposition, and developers can build for all target Nuke versions on Linux and Windows via a single Docker-based script.

## Core Value

Artists can use deep-compositing operations in Nuke that don't exist in the built-in toolset, with professional-quality UIs and reliable deep-pixel math.

## Current State

**v1.2 + DeepCBlur v2 + DeepCDepthBlur correctness + DeepCOpenDefocus shipped + Kernel-level holdout complete + Performance M012 complete.** 25 plugins. DeepCOpenDefocus accepts Deep image input, applies layer-peel algorithm with per-sample CoC from depth, outputs physically-based defocused flat RGBA. Features: holdout Deep input with depth-correct per-sample T attenuation in render_impl (scene-depth space), CameraOp input link, 4 manual lens knobs, all non-uniform bokeh artifacts (catseye, barndoor, astigmatism, axial aberration, inverse foreground), aperture-iris icon, EUPL-1.2 attribution. M011 holdout: opendefocus_deep_render_holdout() FFI + HoldoutData C struct; buildHoldoutData() pre-render C++ dispatch; CPU unit test: z=2 uncut (T=1.0), z=5 attenuated 10x (T=0.1). M012 performance: OnceLock singleton renderer eliminates per-call async init; prepare_filter_mipmaps + render_stripe_prepped hoists filter/mipmap prep outside layer loop; render_convolve_prepped skips Telea inpaint for layer-peel layers; full-image PlanarIop cache (mutex-guarded, 6-field cache key, invalidated in _validate()) eliminates redundant FFI renders across Nuke stripes; quality Enumeration_knob (Low/Medium/High) wired C++ → FFI → Rust; 256×256 SLA test fixture test/test_opendefocus_256.nk. Docker-verified: 21 MB DeepCOpenDefocus.so (Nuke 16.0 Linux). R046, R049, R057 validated.

## Architecture / Key Patterns

- **Three plugin tiers**: `PLUGINS` (direct `DeepFilterOp`), `PLUGINS_WRAPPED` (`DeepCWrapper` base), `PLUGINS_MWRAPPED` (`DeepCMWrapper` base)
- **Build**: CMake 3.15+, C++17, Qt 6.5.3 for DeepCShuffle2 custom knob, NukeDockerBuild Docker images for Linux + Windows. Local dev: symlink from build output into Nuke's plugin path — rebuilding in-place is sufficient.
- **Menu**: `python/menu.py.in` CMake template with category variables (`DRAW_NODES`, `FILTER_NODES`, etc.)
- **Shared headers**: `DeepSampleOptimizer.h` — header-only sample merge/cap utility in `deepc` namespace, zero DDImage deps
- **Platform**: Linux primary, Windows supported, macOS not maintained
- **Constraints**: Nuke 16+ only, `_GLIBCXX_USE_CXX11_ABI=1`, GCC 11.2.1, GPL-3.0

## Capability Contract

See `.gsd/REQUIREMENTS.md` for the explicit capability contract, requirement status, and coverage mapping.

## Milestone Sequence

- [x] M001: DeepC v1.0–v1.2 — 23 plugins shipped, codebase sweep, GL handles, Shuffle2 UI, 4D noise, build system, DeepThinner, docs
- [x] M002: DeepCBlur — Gaussian blur on deep images with sample propagation and built-in optimization
- [x] M003: DeepCBlur v2 — Separable blur, kernel accuracy tiers, alpha darkening correction, UI polish
- [x] M004-ks4br0: DeepCBlur correctness + DeepCDepthBlur — Fix premult colour comparison and overlap tidy in optimizer; new Z-axis depth spread node
- [x] M005-9j5er8: DeepCDepthBlur correctness — Fix multiplicative alpha decomposition, zero-alpha filtering, input rename
- [x] M009-mso5fb: DeepCOpenDefocus — GPU-accelerated deep defocus using forked opendefocus library, with holdout input and non-uniform bokeh artifacts
- [x] M010-zkt9ww: DeepCOpenDefocus integration tests — 7 syntax-verified .nk test scripts covering multi-depth, holdout, camera link, CPU-only, empty input, and bokeh disc; use_gpu Bool_knob wired through full C++/FFI/Rust stack
- [x] M011-qa62vv: Kernel-level holdout — forked four opendefocus 0.1.10 crates as workspace path deps; holdout transmittance evaluated per-sample in render_impl (scene-depth space) during layer-peel; opendefocus_deep_render_holdout() FFI wired through C++ buildHoldoutData() pre-render dispatch; Docker build exits 0; DeepCOpenDefocus.so (21 MB) ships with depth-correct holdout. R046, R049, R057 validated.
- [x] M012-kho2ui: DeepCOpenDefocus performance — OnceLock singleton renderer ✅, layer-peel dedup (prepare_filter_mipmaps hoisted, Telea inpaint skipped) ✅, quality Enumeration_knob ✅ (S01); full-image PlanarIop cache (mutex-guarded, 6-field key, _validate() invalidation, fullBox deepEngine fetch) ✅, 256×256 SLA test fixture ✅ (S02). All slices complete. Docker build + live Nuke 10s SLA deferred to release environment.
