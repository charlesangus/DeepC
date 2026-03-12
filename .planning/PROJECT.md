# DeepC

## What This Is

DeepC is an open-source suite of 22 deep-compositing Nuke NDK plugins targeting Foundry Nuke 16+. It provides color, matte, noise, shuffle, and utility operations on deep images, built on two shared C++ base classes (`DeepCWrapper`, `DeepCMWrapper`) that handle the common deep-pixel processing pipeline. Plugins are compiled as native shared libraries and registered into Nuke's toolbar via a generated Python menu.

## Core Value

Artists can use deep-compositing operations in Nuke that don't exist in the built-in toolset, with professional-quality UIs and reliable deep-pixel math.

## Requirements

### Validated

- ✓ 22 deep-compositing plugins (Add, AddChannels, AdjustBBox, Blink, Clamp, ColorLookup, Constant, CopyBBox, Gamma, Grade, HueShift, ID, Invert, Keymix, Matrix, Multiply, PMatte, PNoise, Posterize, RemoveChannels, Saturation, Shuffle, World) — existing
- ✓ Shared base class pipeline (DeepCWrapper, DeepCMWrapper) managing masking, unpremult/premult, mix, per-sample loop — existing
- ✓ CMake build system supporting Nuke 9.0–15.1 — existing
- ✓ Python toolbar menu registration via generated menu.py — existing
- ✓ FastNoise library integration (Simplex, Cellular, Cubic, Perlin, Value noise) in DeepCPNoise — existing
- ✓ Docker-documented build (manual, ASWF CI images) — existing

### Active

- [ ] DeepShuffle UI: full parity with Nuke's old Shuffle node — channel routing knobs + labeled input/output ports
- [ ] DeepShuffle UI: match Nuke 12+ Shuffle2 "noodle" routing style if NDK permits
- [ ] GL handles for DeepCPMatte: 3D viewport feedback showing what will be selected, interactive handle movement
- [ ] DeepDefocus node (v1): new node with 2D output, circular bokeh, CPU implementation first; GPU/Blink path as stretch goal
- [ ] DeepBlur node: new node with Deep output, CPU implementation first; GPU/Blink path as stretch goal
- [ ] Docker + GitHub Actions CI/CD: automated build triggered on push/PR, builds against Nuke 16
- [ ] 4D noise: expose 4D noise option in all noise types across relevant nodes (FastNoise already supports it internally)
- [ ] Vendor DeepThinner: integrate bratgot/DeepThinner into the build system and toolbar menu
- [ ] Codebase sweep: NDK API modernization for Nuke 16+, code quality (dead code, inconsistencies), performance (unnecessary copies), build system cleanup (CMake hygiene, warnings)

### Out of Scope

- DeepDefocus v2 (custom bokeh shapes) — deferred to next milestone
- DeepDefocus v3 (spatial/depth-varying bokeh: cat's eye, barn door) — deferred to future milestone
- GL handles for DeepCPNoise or DeepCWorld — not requested for this milestone
- Nuke < 16 support for new features — dropping legacy version constraints to simplify work
- macOS support — not actively maintained
- Real-time GPU path for DeepDefocus/DeepBlur v1 — CPU correctness first, GPU is stretch

## Context

- Codebase map is at `.planning/codebase/` — read ARCHITECTURE.md, CONCERNS.md, CONVENTIONS.md before implementation work
- Known bugs documented in CONCERNS.md — codebase sweep should address these (DeepCGrade reverse gamma, DeepCKeymix B-side containment check, DeepCSaturation/DeepCHueShift divide-by-zero, DeepCID unused loop variable, DeepCConstant comma operator)
- FastNoise already implements 4D variants internally; 4D noise task is UI exposure + hookup, not new algorithm work
- DeepCBlink is an unfinished proof of concept; the sweep should decide whether to complete or remove it
- `perSampleData` interface (single float) is a known design limitation — sweep can redesign to pointer+length

## Constraints

- **Tech stack**: C++17, Nuke 16+ NDK, new ABI (`_GLIBCXX_USE_CXX11_ABI=1`)
- **Platform**: Linux primary; Windows supported; macOS not maintained
- **Build**: CMake 3.15+; Docker (ASWF CI images); GitHub Actions
- **License**: GPL-3.0
- **Dependencies**: No new runtime dependencies beyond NDK unless strictly necessary

## Key Decisions

| Decision | Rationale | Outcome |
|----------|-----------|---------|
| Target Nuke 16+ only | Simplifies ABI/API constraints; drops legacy workarounds | — Pending |
| DeepDefocus v1 = CPU first | Correctness before optimization; Blink path as stretch goal | — Pending |
| GL handles for DeepCPMatte only | Scoped to the most-used P node; others deferred | — Pending |
| GitHub Actions for CI/CD | Native to repo, free for public repos | — Pending |

---
*Last updated: 2026-03-12 after initialization*
