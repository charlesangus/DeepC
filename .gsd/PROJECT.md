# DeepC

## What This Is

DeepC is an open-source suite of deep-compositing Nuke NDK plugins targeting Foundry Nuke 16+. Ships 24 plugins covering color, matte, noise, shuffle, utility, blur, and deep-sample optimisation operations on deep images. Core plugins are built on two shared C++ base classes (`DeepCWrapper`, `DeepCMWrapper`); DeepThinner and DeepCBlur use `DeepFilterOp` directly. Highlights: DeepCShuffle2 features a full Qt6 custom knob UI with colored channel routing, DeepCPMatte provides an interactive 3D wireframe GL handle, DeepCPNoise supports true 4D noise evolution across all 9 noise types, DeepThinner reduces deep sample counts via seven independent artist-controllable passes, DeepCBlur provides Gaussian blur for deep images with sample propagation and per-pixel optimization, and developers can build for all target Nuke versions on Linux and Windows via a single Docker-based script.

## Core Value

Artists can use deep-compositing operations in Nuke that don't exist in the built-in toolset, with professional-quality UIs and reliable deep-pixel math.

## Current State

**v1.2 + DeepCBlur v2 + DeepCDepthBlur correctness shipped.** 24 plugins. DeepCDepthBlur alpha decomposition corrected from additive (`α * w`) to multiplicative (`1 - pow(1-α, w)`), ensuring the flatten invariant holds under Nuke's deep compositing model. Zero-alpha guards added (double 1e-6f pattern). Second input renamed "B" → "ref". Docker build confirmed clean (DeepCDepthBlur.so, 415,640 bytes, Nuke 16.0v2 / GCC 11.2.1). R013, R017, R018 validated.

## Architecture / Key Patterns

- **Three plugin tiers**: `PLUGINS` (direct `DeepFilterOp`), `PLUGINS_WRAPPED` (`DeepCWrapper` base), `PLUGINS_MWRAPPED` (`DeepCMWrapper` base)
- **Build**: CMake 3.15+, C++17, Qt 6.5.3 for DeepCShuffle2 custom knob, NukeDockerBuild Docker images for Linux + Windows
- **Menu**: `python/menu.py.in` CMake template with category variables (`DRAW_NODES`, `FILTER_NODES`, etc.)
- **Shared headers**: `DeepSampleOptimizer.h` — header-only sample merge/cap utility in `deepc` namespace, zero DDImage deps
- **Platform**: Linux primary, Windows supported, macOS not maintained
- **Constraints**: Nuke 16+ only, `_GLIBCXX_USE_CXX11_ABI=1`, GCC 11.2.1, GPL-3.0

## Capability Contract

See `.gsd/REQUIREMENTS.md` for the explicit capability contract, requirement status, and coverage mapping.

## Milestone Sequence

- [x] M001: DeepC v1.0–v1.2 — 23 plugins shipped, codebase sweep, GL handles, Shuffle2 UI, 4D noise, build system, DeepThinner, docs
- [x] M002: DeepCBlur — Gaussian blur on deep images with sample propagation and built-in optimization
- [x] M003: DeepCBlur v2 — Separable blur, kernel accuracy tiers, alpha darkening correction, UI polish *(complete)*
- [x] M004-ks4br0: DeepCBlur correctness + DeepCDepthBlur — Fix premult colour comparison and overlap tidy in optimizer; new Z-axis depth spread node
- [x] M005-9j5er8: DeepCDepthBlur correctness — Fix multiplicative alpha decomposition, zero-alpha filtering, input rename
