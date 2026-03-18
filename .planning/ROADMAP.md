# Roadmap: DeepC

## Milestones

- ✅ **v1.0 DeepC** — Phases 1–5 (shipped 2026-03-17)
- 📋 **v1.1** — Phases 6+ (planned)

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

### 📋 v1.1 (Planned)

- [ ] Phase 6: Local Build System — Integrate NukeDockerBuild for multi-version local builds (Linux + Windows) without manual Nuke SDK installation

### Phase 6: Local Build System
**Goal**: A developer can run a single script to build DeepC for all target Nuke versions on both Linux and Windows, using NukeDockerBuild Docker images — no manual Nuke SDK installation required.
**Depends on**: Phase 5
**Plans:** 1 plan

Plans:
- [ ] 06-01-PLAN.md — Create docker-build.sh with NukeDockerBuild orchestration (Linux + Windows builds, zip archives)

## Progress

| Phase | Milestone | Plans Complete | Status | Completed |
|-------|-----------|----------------|--------|-----------|
| 1. Codebase Sweep | v1.0 | 5/5 | Complete | 2026-03-13 |
| 2. DeepCPMatte GL Handles | v1.0 | 1/1 | Complete | 2026-03-14 |
| 3. DeepShuffle UI | v1.0 | 4/4 | Complete | 2026-03-15 |
| 3.1. Refine DeepCShuffle UI (INSERTED) | v1.0 | 5/5 | Complete | 2026-03-16 |
| 4. DeepCPNoise 4D | v1.0 | 3/3 | Complete | 2026-03-17 |
| 5. Release Cleanup | v1.0 | 1/1 | Complete | 2026-03-17 |
| 6. Local Build System | v1.1 | 0/1 | Not started | - |
