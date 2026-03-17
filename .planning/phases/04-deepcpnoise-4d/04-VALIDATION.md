---
phase: 4
slug: deepcpnoise-4d
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-03-16
---

# Phase 4 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | None detected — no pytest/jest/vitest config, no test/ directory |
| **Config file** | None — Wave 0 installs compilation check |
| **Quick run command** | `cd /workspace/build && make DeepCPNoise 2>&1 \| tail -10` |
| **Full suite command** | Compilation clean + manual Nuke UAT |
| **Estimated runtime** | ~30 seconds (compilation) |

---

## Sampling Rate

- **After every task commit:** Run `cd /workspace/build && make DeepCPNoise 2>&1 | tail -10`
- **After every plan wave:** Run compilation clean + grep for removed dead code
- **Before `/gsd:verify-work`:** Full suite must be green + manual Nuke UAT completed
- **Max feedback latency:** 30 seconds (compilation)

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 4-01-01 | 01 | 1 | NOIS-01 | smoke | `cd /workspace/build && make DeepCPNoise 2>&1 \| grep -E 'error\|warning'` | ❌ W0 | ⬜ pending |
| 4-01-02 | 01 | 1 | NOIS-01 | static | `grep -n "only 4D" /workspace/src/DeepCPNoise.cpp` returns empty | ❌ W0 | ⬜ pending |
| 4-01-03 | 01 | 1 | NOIS-01 | manual | Load DeepCPNoise in Nuke, verify all 5 noise types produce output | N/A | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] Compilation check — `cd /workspace/build && make DeepCPNoise` succeeds without errors
- [ ] Tooltip regression grep — `grep -n "only 4D" /workspace/src/DeepCPNoise.cpp` returns empty after changes

*Existing infrastructure covers compilation; no test framework installed. Manual Nuke UAT is the acceptance gate.*

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| All 5 noise types produce non-zero output in 4D mode | NOIS-01 | No automated UI test framework | Load DeepCPNoise in Nuke, set each noise type (Simplex, Perlin, Cellular, Cubic, Value), set noise_evolution=0.5, verify non-black output |
| noise_evolution=0 produces same result as 3D | NOIS-01 | Requires visual comparison in Nuke | Set noise_evolution=0, compare output to 3D equivalent |
| noise_evolution enabled only for Simplex | NOIS-01 | Requires Nuke UI inspection | Switch between noise types, verify knob is enabled only for Simplex |

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 30s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
