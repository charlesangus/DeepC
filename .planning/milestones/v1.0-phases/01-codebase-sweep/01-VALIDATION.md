---
phase: 1
slug: codebase-sweep
status: draft
nyquist_compliant: true
wave_0_complete: true
created: 2026-03-12
---

# Phase 1 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | None — no automated test infrastructure exists |
| **Config file** | None |
| **Quick run command** | `cmake --build build/VERSION --config Release` |
| **Full suite command** | Build + manual Nuke load (no automated equivalent) |
| **Estimated runtime** | ~30 seconds (build only) |

---

## Sampling Rate

- **After every task commit:** Run `cmake --build build/VERSION --config Release`
- **After every plan wave:** Run full build + manual load test in Nuke confirms no crashes on plugin load
- **Before `/gsd:verify-work`:** All bugs verified fixed manually
- **Max feedback latency:** 30 seconds (build)

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 1-01-01 | 01 | 1 | SWEEP-01 | manual | `cmake --build build/VERSION --config Release` | N/A | ⬜ pending |
| 1-01-02 | 01 | 1 | SWEEP-02 | manual | `cmake --build build/VERSION --config Release` | N/A | ⬜ pending |
| 1-01-03 | 01 | 1 | SWEEP-03 | manual | `cmake --build build/VERSION --config Release` | N/A | ⬜ pending |
| 1-01-04 | 01 | 1 | SWEEP-04 | manual | `cmake --build build/VERSION --config Release` | N/A | ⬜ pending |
| 1-01-05 | 01 | 1 | SWEEP-05 | code-review | `cmake --build build/VERSION --config Release` | N/A | ⬜ pending |
| 1-02-01 | 02 | 1 | SWEEP-06 | manual | `cmake --build build/VERSION --config Release` | N/A | ⬜ pending |
| 1-02-02 | 02 | 1 | SWEEP-09 | build | `cmake --build build/VERSION --config Release` | N/A | ⬜ pending |
| 1-02-03 | 02 | 1 | SWEEP-10 | code-review + build | `cmake --build build/VERSION --config Release` | N/A | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

None — Existing infrastructure covers all phase requirements. All verification is manual or build-based. No test framework setup required for this phase.

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| Grade reverse gamma applied to ramp result | SWEEP-01 | No automated test infra | Load DeepCGrade in Nuke, enable reverse, set gamma != 1.0, verify output changes correctly |
| Keymix A-side uses aPixel channels | SWEEP-02 | No automated test infra | Connect A/B with different channel sets, partial mix, inspect output |
| Saturation/HueShift produce 0 (not NaN) for alpha==0 | SWEEP-03 | No automated test infra | Route fully-transparent samples through DeepCSaturation/DeepCHueShift, verify zero output |
| DeepCConstant front/back interpolation is correct | SWEEP-04 | No automated test infra | Create multi-sample constant, verify color gradient |
| DeepCWrapper/DeepCMWrapper absent from node menu | SWEEP-06 | No automated test infra | Load plugins in Nuke, verify node menu has no DeepCWrapper or DeepCMWrapper entry |
| DeepCBlink absent from build and menu | SWEEP-09 | No automated test infra | Confirm build succeeds without Blink source; load plugins in Nuke |

---

## Validation Sign-Off

- [x] All tasks have `<automated>` verify or Wave 0 dependencies
- [x] Sampling continuity: no 3 consecutive tasks without automated verify
- [x] Wave 0 covers all MISSING references
- [x] No watch-mode flags
- [x] Feedback latency < 30s
- [x] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
