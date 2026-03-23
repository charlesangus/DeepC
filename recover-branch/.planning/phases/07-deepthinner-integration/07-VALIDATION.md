---
phase: 7
slug: deepthinner-integration
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-03-18
---

# Phase 7 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | cmake --build (build verification) + manual Nuke toolbar check |
| **Config file** | CMakeLists.txt (src/ and root) |
| **Quick run command** | `cmake --build build/ --target DeepThinner 2>&1 | tail -5` |
| **Full suite command** | `bash docker-build.sh linux 2>&1 | tail -20` |
| **Estimated runtime** | ~120 seconds (docker build) |

---

## Sampling Rate

- **After every task commit:** Run `cmake --build build/ --target DeepThinner 2>&1 | tail -5`
- **After every plan wave:** Run `bash docker-build.sh linux 2>&1 | tail -20`
- **Before `/gsd:verify-work`:** Full suite (both linux and windows docker builds) must be green
- **Max feedback latency:** 120 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| 7-01-01 | 01 | 1 | THIN-01 | file-check | `test -f src/DeepThinner.cpp && head -5 src/DeepThinner.cpp` | ❌ W0 | ⬜ pending |
| 7-01-02 | 01 | 1 | THIN-02 | build | `cmake --build build/ --target DeepThinner 2>&1 \| grep -E "error\|DeepThinner"` | ❌ W0 | ⬜ pending |
| 7-01-03 | 01 | 2 | THIN-03 | file-check | `grep -n "DeepThinner" nuke/menu.py.in` | ❌ W0 | ⬜ pending |
| 7-01-04 | 01 | 3 | THIN-04 | build | `bash docker-build.sh linux 2>&1 \| grep -E "DeepThinner\|error"` | ❌ W0 | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] Verify `cmake` is configured and a build directory exists before modifications begin
- [ ] Confirm existing `PLUGINS` list format in `src/CMakeLists.txt`
- [ ] Confirm existing `FILTER_NODES` or equivalent variable exists (or plan to add)
- [ ] Confirm `menu.py.in` template location and `string(REPLACE ...)` pattern

*If build infrastructure is already in place from prior phases: "Existing infrastructure covers all phase requirements."*

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| DeepThinner node appears in Deep toolbar | THIN-03 | Requires running Nuke with the generated menu.py | Source menu.py in Nuke, open Deep toolbar, verify DeepThinner is listed |
| License header intact | THIN-01 | Requires visual inspection of MIT header comment | `head -20 src/DeepThinner.cpp` — confirm MIT license header is present and unmodified |

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 120s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
