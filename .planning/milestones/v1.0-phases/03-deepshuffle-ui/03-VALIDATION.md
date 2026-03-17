---
phase: 03
slug: deepshuffle-ui
status: draft
nyquist_compliant: false
wave_0_complete: false
created: 2026-03-14
---

# Phase 03 — Validation Strategy

> Per-phase validation contract for feedback sampling during execution.

---

## Test Infrastructure

| Property | Value |
|----------|-------|
| **Framework** | Build verification only (no unit test framework in project) |
| **Config file** | `build/` (CMake build directory) |
| **Quick run command** | `cmake --build /workspace/build --target DeepCShuffle` |
| **Full suite command** | `cmake --build /workspace/build` |
| **Estimated runtime** | ~30 seconds (quick), ~90 seconds (full) |

---

## Sampling Rate

- **After every task commit:** Run `cmake --build /workspace/build --target DeepCShuffle`
- **After every plan wave:** Run `cmake --build /workspace/build`
- **Before `/gsd:verify-work`:** Full suite must be green + manual UAT in Nuke
- **Max feedback latency:** ~30 seconds

---

## Per-Task Verification Map

| Task ID | Plan | Wave | Requirement | Test Type | Automated Command | File Exists | Status |
|---------|------|------|-------------|-----------|-------------------|-------------|--------|
| Wave 0 Qt setup | 01 | 0 | SHUF-01 | build | `cmake --build /workspace/build --target DeepCShuffle` | ❌ W0 | ⬜ pending |
| ShuffleMatrixKnob class | 01 | 1 | SHUF-01 | build | `cmake --build /workspace/build --target DeepCShuffle` | ✅ | ⬜ pending |
| ChannelSet knobs | 01 | 1 | SHUF-04 | build | `cmake --build /workspace/build --target DeepCShuffle` | ✅ | ⬜ pending |
| 8-channel routing | 01 | 1 | SHUF-03 | build | `cmake --build /workspace/build --target DeepCShuffle` | ✅ | ⬜ pending |
| knob_changed / column rebuild | 01 | 1 | SHUF-02 | build | `cmake --build /workspace/build --target DeepCShuffle` | ✅ | ⬜ pending |
| processSample passthrough | 01 | 1 | SHUF-01 | build | `cmake --build /workspace/build --target DeepCShuffle` | ✅ | ⬜ pending |

*Status: ⬜ pending · ✅ green · ❌ red · ⚠️ flaky*

---

## Wave 0 Requirements

- [ ] `sudo apt install qt6-base-dev` (or confirm Qt6 headers present) — required before ShuffleMatrixWidget compiles
- [ ] Add to `src/CMakeLists.txt`: `find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)`, `set(CMAKE_AUTOMOC ON)`, `target_link_libraries(DeepCShuffle PRIVATE Qt::Core Qt::Gui Qt::Widgets)`
- [ ] Re-run `cmake /workspace -B /workspace/build` after Qt6 install to regenerate build system with MOC support
- [ ] Verify `cmake --build /workspace/build --target DeepCShuffle` passes after CMake reconfiguration (baseline build green before new code is added)

---

## Manual-Only Verifications

| Behavior | Requirement | Why Manual | Test Instructions |
|----------|-------------|------------|-------------------|
| Matrix grid widget appears in node panel | SHUF-01 | Requires Nuke GUI | Open DeepCShuffle in Nuke; verify matrix widget renders in panel |
| Column headers update when ChannelSet changes | SHUF-02/04 | Requires Nuke GUI | Change in1 ChannelSet from rgba to another layer; verify matrix columns rebuild |
| Routing assignment persists across script save/load | SHUF-01 | Requires Nuke GUI | Set routing, save .nk script, reload; verify routing is restored |
| Unselected channels pass through unchanged | SHUF-01 | Requires Nuke GUI | Connect node, route only rgba, verify other layers (e.g. depth) pass through |
| in2 optional — setting to none hides in2 columns | SHUF-04 | Requires Nuke GUI | Leave in2 as none; verify no in2 columns in matrix |

---

## Validation Sign-Off

- [ ] All tasks have `<automated>` verify or Wave 0 dependencies
- [ ] Sampling continuity: no 3 consecutive tasks without automated verify
- [ ] Wave 0 covers all MISSING references
- [ ] No watch-mode flags
- [ ] Feedback latency < 30s
- [ ] `nyquist_compliant: true` set in frontmatter

**Approval:** pending
