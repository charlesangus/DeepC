---
phase: 5
slug: release-cleanup
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-03-17
---

# Phase 5 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | None — Nuke C++ plugin; no automated test suite exists |
| **Config file** | N/A |
| **Quick run command** | `ls /workspace/icons/DeepCShuffle2.png` (SC-1); `grep -E "SWEEP-0[78]" /workspace/.planning/REQUIREMENTS.md` (SC-4) |
| **Full suite command** | Manual UAT: load both .so files in Nuke, verify icon, label, node creation, no duplicates |
| **Estimated runtime** | ~2 minutes (manual) |

---

## Sampling Rate

- **After every task commit:** Run quick smoke check relevant to that task (see Per-Task map below)
- **After every plan wave:** Run all automated smoke checks above
- **Before `/gsd:verify-work`:** Full manual Nuke UAT must be completed
- **Max feedback latency:** Immediate for smoke checks; manual UAT at wave end

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 5-01-01 | 01 | 1 | SC-1 | smoke | `ls /workspace/icons/DeepCShuffle2.png` | ❌ W0 | ⬜ pending |
| 5-01-02 | 01 | 1 | SC-2, SC-3 | manual-only | N/A — requires Nuke runtime | N/A | ⬜ pending |
| 5-01-03 | 01 | 1 | SC-2, SC-3 | manual-only | N/A — requires Nuke runtime | N/A | ⬜ pending |
| 5-01-04 | 01 | 1 | SC-4 | smoke | `grep -E "SWEEP-0[78]" /workspace/.planning/REQUIREMENTS.md` | ✅ | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

Existing infrastructure covers all phase requirements — no test framework setup needed. Smoke checks use standard shell commands (`ls`, `grep`) against files that will exist after tasks execute.

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| One "DeepCShuffle" menu entry in Nuke Channel submenu that creates a DeepCShuffle2 node | SC-2 | Requires Nuke runtime to inspect menu | Load DeepCShuffle2.so via Nuke; open Nodes > DeepC > Channel; confirm one "DeepCShuffle" entry exists, creates a DeepCShuffle2 node |
| No duplicate or broken entries when both .so files are loaded | SC-3 | Requires Nuke runtime; depends on DeepCShuffle.so self-registration behavior | Load both DeepCShuffle.so and DeepCShuffle2.so; inspect Channel submenu for duplicates or errors |
| DeepCShuffle2 node shows correct icon in Nuke toolbar | SC-1 (install verification) | Icon display requires Nuke runtime | After install, open Nuke and confirm the DeepCShuffle menu entry shows the DeepCShuffle2.png icon |

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 120s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
