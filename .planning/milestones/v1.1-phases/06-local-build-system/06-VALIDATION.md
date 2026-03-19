---
phase: 6
slug: local-build-system
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-03-17
---

# Phase 6 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | bash / shell script integration tests |
| **Config file** | none — scripts are self-contained |
| **Quick run command** | `bash docker-build.sh --dry-run 2>&1` (if dry-run supported) or `bash -n docker-build.sh` |
| **Full suite command** | `bash docker-build.sh 16.0 linux 2>&1` (requires Docker daemon) |
| **Estimated runtime** | ~5–30 minutes (full Docker build); ~1 second (syntax check) |

---

## Sampling Rate

- **After every task commit:** Run `bash -n docker-build.sh` (syntax check)
- **After every plan wave:** Run `bash docker-build.sh --help 2>&1 || bash -n docker-build.sh`
- **Before `/gsd:verify-work`:** Full Docker build suite must be green
- **Max feedback latency:** 5 seconds (syntax checks); Docker builds are manual-only

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 6-01-01 | 01 | 1 | BUILD-01 | integration | `bash -n docker-build.sh` | ✅ / ❌ W0 | ⬜ pending |
| 6-01-02 | 01 | 1 | BUILD-02 | integration | `bash -n docker-build.sh` | ✅ / ❌ W0 | ⬜ pending |
| 6-01-03 | 01 | 2 | BUILD-03 | manual | docker run test | ✅ / ❌ W0 | ⬜ pending |
| 6-01-04 | 01 | 2 | BUILD-04 | manual | docker run test | ✅ / ❌ W0 | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] `docker-build.sh` — main build script (created in Wave 1)
- [ ] Script must pass `bash -n docker-build.sh` (syntax check) before Docker testing

*If framework install needed: Docker daemon must be running for full integration tests.*

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| Linux .so build artifact produced | BUILD-LINUX | Requires running Docker daemon with full image pull | `docker run ... cmake --build` and verify `.so` exists |
| Windows .dll build artifact produced | BUILD-WINDOWS | Requires running Docker daemon with wine-msvc image | `docker run ... cmake --build` and verify `.dll` exists |
| Cross-platform archive creation | BUILD-ARCHIVE | Requires completed build artifacts | Verify `dist/DeepC-<version>.zip` contains both `.so` and `.dll` |

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 5s (syntax checks)
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
