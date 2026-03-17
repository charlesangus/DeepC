# DeepC

## What This Is

DeepC is an open-source suite of deep-compositing Nuke NDK plugins targeting Foundry Nuke 16+. v1.0 ships 21 plugins covering color, matte, noise, shuffle, and utility operations on deep images, built on two shared C++ base classes (`DeepCWrapper`, `DeepCMWrapper`). Highlights: DeepCShuffle2 features a full Qt6 custom knob UI with colored channel routing, DeepCPMatte provides an interactive 3D wireframe GL handle, and DeepCPNoise supports true 4D noise evolution across all 9 noise types.

## Core Value

Artists can use deep-compositing operations in Nuke that don't exist in the built-in toolset, with professional-quality UIs and reliable deep-pixel math.

## Requirements

### Validated

- ✓ 22 deep-compositing plugins (Add, AddChannels, AdjustBBox, Clamp, ColorLookup, Constant, CopyBBox, Gamma, Grade, HueShift, ID, Invert, Keymix, Matrix, Multiply, PMatte, PNoise, Posterize, RemoveChannels, Saturation, Shuffle2, World) — existing + cleaned
- ✓ Shared base class pipeline (DeepCWrapper, DeepCMWrapper) managing masking, unpremult/premult, mix, per-sample loop — existing + Op::Description removed
- ✓ CMake build system with Qt6 AUTOMOC support — v1.0
- ✓ Python toolbar menu registration via generated menu.py with display-name rename support — v1.0
- ✓ FastNoise library integration with full 4D support across all 9 noise types — v1.0
- ✓ Codebase sweep: all confirmed deep-pixel math bugs fixed, NDK APIs modernized for Nuke 16+, DeepCBlink removed — v1.0 (SWEEP-01 through SWEEP-06, SWEEP-09, SWEEP-10)
- ✓ DeepCPMatte GL handles: deadlock-free 3D wireframe viewport handle, interactive drag-to-reposition — v1.0 (GLPM-01, GLPM-02, GLPM-03)
- ✓ DeepCShuffle2 UI: full parity with Nuke Shuffle2 — colored channel buttons, embedded ChannelSet pickers, radio enforcement, const:0/1 columns, identity routing on first open — v1.0 (SHUF-01, SHUF-02, SHUF-03, SHUF-04)
- ✓ DeepCPNoise 4D: noise_evolution wired as w dimension unconditionally for all noise types — v1.0 (NOIS-01)

### Active (v1.1)

- [ ] DeepThinner: license confirmed compatible with GPL-3.0, source integrated into CMake build, registered in toolbar menu (THIN-01, THIN-02, THIN-03)
- [ ] Docker + GitHub Actions CI/CD: automated build triggered on push/PR, builds against Nuke 16 (CI-01, CI-02, CI-03, CI-04)

### Out of Scope

- Shuffle2 noodle/wire routing UI — NDK does not expose this API; internal to Foundry's built-in node only
- DeepDefocus (all versions) — deferred to future milestone
- DeepBlur — deferred to future milestone
- GL handles for DeepCPNoise or DeepCWorld — not requested
- Nuke < 16 support for new/modified features — dropping legacy constraints
- macOS support — not actively maintained
- SWEEP-07 (perSampleData redesign) — dropped after analysis; refactor risk outweighed benefit at this scale
- SWEEP-08 (grade coefficient extraction) — dropped after analysis; shared utility adds complexity for marginal gain

## Context

**v1.0 shipped 2026-03-17.**

- Codebase map at `.planning/codebase/` — ARCHITECTURE.md, CONCERNS.md, CONVENTIONS.md
- Qt 6.5.3 pre-installed at `/opt/Qt/6.5.3/gcc_64` — exact version match with Nuke 16 runtime; no apt install needed
- DeepCShuffle2 uses Qt6 custom knob (`ShuffleMatrixKnob` + `ShuffleMatrixWidget`) registered via NDK `CustomKnob2`
- Display-name rename dict in `menu.py.in` decouples class names from menu labels — enables backward-compat .so files with clean UX
- Silent binary pattern established: include in `PLUGINS`, exclude from `CHANNEL_NODES`, remove `Op::Description`
- Known pre-existing TODOs in `src/DeepCWrapper.cpp`, `src/DeepCMWrapper.cpp`, `src/DeepCWorld.cpp`, `src/DeepCPNoise.cpp` — low priority, not blocking
- Nyquist validation partially adopted (Phase 1 fully compliant; Phases 2–4 partial; no test framework exists for Nuke C++ plugins — manual UAT is the gate)

## Constraints

- **Tech stack**: C++17, Nuke 16+ NDK, new ABI (`_GLIBCXX_USE_CXX11_ABI=1`), Qt 6.5.3
- **Platform**: Linux primary; Windows supported; macOS not maintained
- **Build**: CMake 3.15+; Docker (ASWF CI images); GitHub Actions
- **License**: GPL-3.0
- **Dependencies**: No new runtime dependencies beyond NDK + Qt6 (already bundled by Nuke)

## Key Decisions

| Decision | Rationale | Outcome |
|----------|-----------|---------|
| Target Nuke 16+ only | Simplifies ABI/API constraints; drops legacy workarounds | ✓ Good — no legacy issues encountered |
| Qt6 via /opt/Qt/6.5.3/gcc_64 not apt | Exact ABI match with Nuke 16 runtime; no sudo required | ✓ Good — eliminated version mismatch risk |
| DeepCShuffle2 as new class, DeepCShuffle.so kept silent | Backward compat without duplicate menu entries | ✓ Good — clean UX via display-name rename dict |
| Op::Description removal pattern (SWEEP-06 + Phase 5) | Prevents self-registration when .so auto-loaded | ✓ Good — consistent pattern across all silent binaries |
| NDK ChannelSet knobs over QComboBox pickers | QComboBox segfaulted during UAT; NDK knobs are Nuke-integrated | ✓ Good — reverted in Phase 3.1 after UAT failure |
| SWEEP-07/08 dropped (not deferred) | Refactor risk outweighed benefit; no user-visible value | ✓ Good — avoided scope creep |
| DeepCBlink removed from build | Unfinished proof-of-concept; no clear path to completion | ✓ Good — cleaner build, no misleading placeholder |
| GitHub Actions for CI/CD | Native to repo, free for public repos | — Deferred to v1.1 |
| DeepDefocus v1 = CPU first | Correctness before optimization | — Deferred to future milestone |

---
*Last updated: 2026-03-17 after v1.0 milestone*
