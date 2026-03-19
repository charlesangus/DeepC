# Roadmap: DeepC

## Milestones

- ✅ **v1.0 DeepC** — Phases 1–5 (shipped 2026-03-17)
- ✅ **v1.1 Local Build System** — Phase 6 (shipped 2026-03-18)
- 🚧 **v1.2 DeepThinner & Documentation** — Phases 7–8 (in progress)

## Phases

<details>
<summary>✅ v1.0 DeepC (Phases 1–5) — SHIPPED 2026-03-17</summary>

- [x] Phase 1: Codebase Sweep (5/5 plans) — completed 2026-03-13
- [x] Phase 2: DeepCPMatte GL Handles (1/1 plan) — completed 2026-03-14
- [x] Phase 3: DeepShuffle UI (4/4 plans) — completed 2026-03-15
- [x] Phase 3.1: Refine and Fix DeepCShuffle UI/Behaviour — INSERTED (5/5 plans) — completed 2026-03-16
- [x] Phase 4: DeepCPNoise 4D (3/3 plans) — completed 2026-03-17
- [x] Phase 5: Release Cleanup (1/1 plan) — completed 2026-03-17

Full details: `.planning/milestones/v1.0-ROADMAP.md`

</details>

<details>
<summary>✅ v1.1 Local Build System (Phase 6) — SHIPPED 2026-03-18</summary>

- [x] Phase 6: Local Build System (1/1 plan) — completed 2026-03-18

Full details: `.planning/milestones/v1.1-ROADMAP.md`

</details>

### 🚧 v1.2 DeepThinner & Documentation (In Progress)

**Milestone Goal:** Vendor DeepThinner into DeepC and overhaul project documentation to reflect current state.

- [x] **Phase 7: DeepThinner Integration** - Vendor DeepThinner into src/, wire into CMake and menu.py.in, verify build on Linux and Windows (completed 2026-03-19)
- [ ] **Phase 8: Documentation Overhaul** - Full README rewrite reflecting current plugin list, build workflow, and DeepThinner attribution; create THIRD_PARTY_LICENSES.md

## Phase Details

### Phase 7: DeepThinner Integration
**Goal**: DeepThinner is a fully integrated plugin — vendored in source, built by CMake, registered in the Deep toolbar, and shipping in release archives for both platforms
**Depends on**: Phase 6
**Requirements**: THIN-01, THIN-02, THIN-03, THIN-04
**Success Criteria** (what must be TRUE):
  1. `src/DeepThinner.cpp` exists with the upstream MIT license header comment intact and unmodified
  2. `cmake` includes DeepThinner in the `PLUGINS` list and produces `DeepThinner.so` (Linux) and `DeepThinner.dll` (Windows) without build errors
  3. DeepThinner appears in the Nuke Deep toolbar after sourcing the generated `menu.py`
  4. `docker-build.sh` completes successfully for both Linux and Windows targets, producing release archives that contain the DeepThinner binary
**Plans**: 1 plan
Plans:
- [ ] 07-01-PLAN.md — Vendor DeepThinner source, wire CMake + menu, verify dual-platform docker builds

### Phase 8: Documentation Overhaul
**Goal**: README.md and THIRD_PARTY_LICENSES.md accurately describe the current project — 23 plugins, docker-build.sh workflow, Nuke 16+ target, DeepThinner attribution — with no stale or dead content
**Depends on**: Phase 7
**Requirements**: DOCS-01, DOCS-02, DOCS-03, DOCS-04
**Success Criteria** (what must be TRUE):
  1. README.md lists all 23 plugins including DeepThinner, with a link to https://github.com/bratgot/DeepThinner and attribution to Marten Blumen
  2. README.md build section describes `docker-build.sh` usage and Nuke 16+ as the target; CentOS, VS2017, and `batchInstall.sh` references are removed
  3. README.md contains no references to DeepCBlink or "coming soon" DeepCompress
  4. `THIRD_PARTY_LICENSES.md` exists and documents DeepThinner's MIT license with full attribution to Marten Blumen
**Plans**: TBD

## Progress

| Phase | Milestone | Plans Complete | Status | Completed |
|-------|-----------|----------------|--------|-----------|
| 1. Codebase Sweep | v1.0 | 5/5 | Complete | 2026-03-13 |
| 2. DeepCPMatte GL Handles | v1.0 | 1/1 | Complete | 2026-03-14 |
| 3. DeepShuffle UI | v1.0 | 4/4 | Complete | 2026-03-15 |
| 3.1. Refine DeepCShuffle UI (INSERTED) | v1.0 | 5/5 | Complete | 2026-03-16 |
| 4. DeepCPNoise 4D | v1.0 | 3/3 | Complete | 2026-03-17 |
| 5. Release Cleanup | v1.0 | 1/1 | Complete | 2026-03-17 |
| 6. Local Build System | v1.1 | 1/1 | Complete | 2026-03-18 |
| 7. DeepThinner Integration | 1/1 | Complete    | 2026-03-19 | - |
| 8. Documentation Overhaul | v1.2 | 0/? | Not started | - |
