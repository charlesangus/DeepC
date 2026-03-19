# Milestones

## v1.1 Local Build System (Shipped: 2026-03-18)

**Phases completed:** 1 phase, 1 plan
**Files changed:** 14 | **Lines added:** +1,452 (net +1,431)
**Timeline:** 2026-03-17 → 2026-03-18 (~1 day)

**Key accomplishments:**
- Created `docker-build.sh` — 186-line NukeDockerBuild orchestration script for Linux (.so) and Windows (.dll) builds from a single Linux host, no local Nuke SDK required
- Integrated `ghcr.io/gillesvink/nukedockerbuild` images for both Linux slim and Windows cross-compilation containers
- Per-platform isolated build/install directories prevent CMake cache conflicts when building multiple platforms in one run
- Distributable zip archives produced at `release/DeepC-{Platform}-Nuke{version}.zip` — matches `batchInstall.sh` convention
- Added Windows Qt6 support and bootstrapping of NukeDockerBuild images from source when Docker Hub images are unavailable

---

## v1.0 DeepC (Shipped: 2026-03-17)

**Phases completed:** 6 phases (Phases 1–5, plus Phase 3.1 inserted), 18 plans
**Timeline:** 2026-03-13 → 2026-03-17 (~5 days)

**Key accomplishments:**
- Codebase sweep: all deep-pixel math bugs fixed, NDK APIs modernized for Nuke 16+, DeepCBlink removed, Op::Description removal pattern established for silent binaries
- DeepCPMatte GL handles: deadlock-free 3D wireframe viewport handle with interactive drag-to-reposition
- DeepCShuffle2: full parity with Nuke Shuffle2 — colored channel routing buttons, embedded ChannelSet pickers, radio enforcement, const:0/1 columns, identity routing on first open
- DeepCPNoise 4D: `noise_evolution` wired unconditionally as the w dimension across all 9 noise types via 4D GetNoise dispatch
- CMake build system with Qt6 AUTOMOC, Python toolbar registration via `menu.py.in` with display-name rename support
- Release cleanup: 22 plugins shipped, silent backward-compat `.so` pattern for DeepCShuffle, SWEEP-07/08 scoped out

---
