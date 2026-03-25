# DeepC

## What This Is

DeepC is an open-source suite of deep-compositing Nuke NDK plugins targeting Foundry Nuke 16+. Ships 24 plugins covering color, matte, noise, shuffle, utility, blur, and deep-sample optimisation operations on deep images. Core plugins are built on two shared C++ base classes (`DeepCWrapper`, `DeepCMWrapper`); DeepThinner and DeepCBlur use `DeepFilterOp` directly. Highlights: DeepCShuffle2 features a full Qt6 custom knob UI with colored channel routing, DeepCPMatte provides an interactive 3D wireframe GL handle, DeepCPNoise supports true 4D noise evolution across all 9 noise types, DeepThinner reduces deep sample counts via seven independent artist-controllable passes, DeepCBlur provides Gaussian blur for deep images with sample propagation and per-pixel optimization, DeepCDepthBlur spreads each deep sample across sub-samples in Z with physically correct alpha decomposition, and developers can build for all target Nuke versions on Linux and Windows via a single Docker-based script.

## Core Value

Artists can use deep-compositing operations in Nuke that don't exist in the built-in toolset, with professional-quality UIs and reliable deep-pixel math.

## Current State

**M008-y32v8w S01 complete.** Path-trace infrastructure delivered: `_validate` format fix in both PO nodes (R045); 5 new lens constant Float_knobs on Ray with Angenieux 55mm defaults (R043, R038); `pt_sample_aperture`, `sphereToCs_full`, and `logarithmic_focus_search` implemented inline in `deepc_po_math.h` (R039, R040, R042); `verify-s01-syntax.sh` with 13 structural contracts all passing. Ray's renderStripe still uses the M007 Option B scatter engine — S02 replaces it with the lentil-style path tracer. Requirements R038–R043, R045 validated at contract level.

## Architecture / Key Patterns

- **Three plugin tiers**: `PLUGINS` (direct `DeepFilterOp`), `PLUGINS_WRAPPED` (`DeepCWrapper` base), `PLUGINS_MWRAPPED` (`DeepCMWrapper` base)
- **Build**: CMake 3.15+, C++17, Qt 6.5.3 for DeepCShuffle2 custom knob, NukeDockerBuild Docker images for Linux + Windows. Local dev: run the project build script — a symlink from the build output directory into Nuke's plugin path means no separate install step is needed; rebuilding in-place is sufficient.
- **Menu**: `python/menu.py.in` CMake template with category variables (`DRAW_NODES`, `FILTER_NODES`, etc.)
- **Shared headers**: `DeepSampleOptimizer.h` — header-only sample merge/cap utility in `deepc` namespace, zero DDImage deps
- **PO shared infrastructure**: `poly.h` (MIT, vendored from lentil) — polynomial lens reader/evaluator; `deepc_po_math.h` — Halton, Shirley disk, CoC, Newton trace helpers
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
- [x] M006: DeepCDefocusPO — Polynomial optics defocus scaffold: Deep→flat, holdout, CA, .fit loader, knobs
- [x] M007-gvtoom: DeepCDefocusPO correctness — Replace broken scatter with two correct variants: thin-lens + raytraced
- [ ] M008-y32v8w: DeepCDefocusPORay path tracer — S01 complete (infrastructure + format fix); S02 replaces Ray's scatter engine with lentil-style path tracer
