# Milestones

## v1.2 DeepThinner & Documentation (Shipped: 2026-03-19)

**Phases completed:** 2 phases, 2 plans, 5 tasks
**Files changed:** 17 | **Lines added:** +1,772 (net +1,632)
**Timeline:** 2026-03-18 → 2026-03-19 (~1 day)

**Key accomplishments:**

- Vendored DeepThinner.cpp (707 lines, MIT, Marten Blumen) into src/ with SPDX copyright header prepended for attribution compliance
- Wired DeepThinner into CMake: PLUGINS list, FILTER_NODES variable, string replacement, and Filter submenu in menu.py.in — 24th DeepC plugin
- Local Linux build verified: DeepThinner.so compiled cleanly with Nuke 16.0 SDK
- README.md overhauled: 23-plugin list with DeepThinner entry (Marten Blumen attribution + upstream link), docker-build.sh Nuke 16+ build workflow, all stale content (DeepCBlink, DeepCompress, CentOS, VS2017, batchInstall.sh) removed
- THIRD_PARTY_LICENSES.md created: full MIT license attribution for DeepThinner (Marten Blumen, 2025) and FastNoise (Jordan Peck, 2017)

---

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
