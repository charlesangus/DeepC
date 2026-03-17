---
phase: 2
slug: deepcpmatte-gl-handles
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-03-14
---

# Phase 2 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | None — compiled Nuke plugin (.so); no unit test framework in repo |
| **Config file** | none — build infrastructure exists in `/workspace/build` |
| **Quick run command** | `cd /workspace/build && make DeepCPMatte 2>&1 \| tail -20` |
| **Full suite command** | `cd /workspace/build && make 2>&1 \| tail -30` |
| **Estimated runtime** | ~10 seconds |

---

## Sampling Rate

- **After every task commit:** Run `cd /workspace/build && make DeepCPMatte 2>&1 | tail -5`
- **After every plan wave:** Run `cd /workspace/build && make 2>&1 | tail -10`
- **Before `/gsd:verify-work`:** Clean build + manual UAT in Nuke 16.0v6
- **Max feedback latency:** ~10 seconds (compile check)

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 2-01-01 | 01 | 1 | GLPM-01 | static/grep | `grep -n "_validate\|->validate" /workspace/src/DeepCPMatte.cpp` — expect zero matches in `draw_handle` body | N/A (grep) | ⬜ pending |
| 2-01-02 | 01 | 1 | GLPM-02 | static/grep | `grep -n "gl_sphere\|gl_cubef\|glPushMatrix\|glMultMatrixf" /workspace/src/DeepCPMatte.cpp` — expect all four present | N/A (grep) | ⬜ pending |
| 2-01-03 | 01 | 1 | GLPM-03 | static/grep | `grep -n "BeginGroup\|build_knob_handles" /workspace/src/DeepCPMatte.cpp` — expect no BeginGroup, one build_knob_handles | N/A (grep) | ⬜ pending |
| 2-01-xx | 01 | 1 | All | build | `cd /workspace/build && make DeepCPMatte 2>&1 \| grep -E "error:\|warning:"` | ❌ Wave 0 | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

Existing infrastructure covers all phase requirements. No test file creation needed — build infrastructure already exists in `/workspace/build`. Validation is:
1. Grep checks for absence/presence of specific patterns in `DeepCPMatte.cpp`
2. Successful compilation (`make DeepCPMatte`)
3. Manual UAT in Nuke 16.0v6 instance

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| Opening DeepCPMatte with 3D viewer active does not deadlock/hang | GLPM-01 | Requires running Nuke 16.0v6 instance | Load DeepCPMatte node, open 3D viewer, confirm no hang |
| Wireframe sphere or cube appears in 3D viewport at Axis knob position/scale | GLPM-02 | Requires running Nuke 3D viewport | Inspect viewport after loading node with Axis knob set |
| Clicking and dragging wireframe handle repositions volume and triggers reprocess | GLPM-03 | Requires interactive Nuke 3D viewport | Drag handle in 3D viewer, confirm position updates and node recooks |

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 10s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
