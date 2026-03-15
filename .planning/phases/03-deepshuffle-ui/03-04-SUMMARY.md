---
phase: 03-deepshuffle-ui
plan: "04"
subsystem: DeepCShuffle2
tags: [deep-compositing, shuffle, matrix-ui, qt6, channel-routing]
dependency_graph:
  requires:
    - 03-03: ShuffleMatrixKnob and ShuffleMatrixWidget implementation
  provides:
    - DeepCShuffle2.so: new Op with matrix routing UI registered in Nuke
    - DeepCShuffle.so: original implementation preserved for backward compatibility
  affects:
    - src/CMakeLists.txt: DeepCShuffle2 added as separate build target
tech_stack:
  added:
    - DeepCShuffle2.cpp: new Op class wiring ShuffleMatrixKnob into DeepFilterOp
  patterns:
    - CustomKnob2 macro to embed custom Knob subclass in Op panel
    - knob_changed() pushing ChannelSet state into ShuffleMatrixKnob via setChannelSets()
    - _matrixState string as shared data contract between Op and widget
    - Passthrough-first routing: inChannel = z default before override lookup
key_files:
  created:
    - src/DeepCShuffle2.cpp
  modified:
    - src/DeepCShuffle.cpp (restored to pre-rewrite original)
    - src/CMakeLists.txt (added DeepCShuffle2 target, Qt6 link moved)
decisions:
  - "New implementation registered as DeepCShuffle2; original DeepCShuffle preserved unchanged for backward compatibility with existing Nuke scripts"
  - "Linker error (_ZN19ShuffleMatrixWidgetD1Ev undefined) was caused by ShuffleMatrixWidget.cpp not being compiled into the DeepCShuffle target — fixed by moving Qt6 source linking to the new DeepCShuffle2 CMake target"
  - "Separate CMake targets (DeepCShuffle and DeepCShuffle2) produce independent .so files; both registered in Nuke via Op::Description"
metrics:
  duration_minutes: 35
  completed_date: "2026-03-15"
  tasks_completed: 2
  files_changed: 3
---

# Phase 3 Plan 4: DeepCShuffle2 Op Wiring Summary

**One-liner:** DeepCShuffle2 Op wired with ShuffleMatrixKnob, ChannelSet pickers, passthrough routing, and _matrixState serialization; original DeepCShuffle preserved for backward compatibility.

## What Was Built

`src/DeepCShuffle2.cpp` is a complete `DeepFilterOp` subclass that:

- Declares `ChannelSet _in1ChannelSet` (default `Mask_RGBA`) and `_in2ChannelSet` (default `Chan_Black`)
- Holds `std::string _matrixState` as the canonical routing state, mirrored by `ShuffleMatrixKnob::store()`
- Parses `_matrixState` in `_validate()` into `std::array<Channel, 8>` output/source pairs
- Implements passthrough-first routing in `doDeepEngine()`: `Channel inChannel = z` before checking the routing table, so unrouted channels are never zeroed
- Pushes `ChannelSet` state into the knob via `knob_changed()` so the widget's column headers rebuild live when `in1`/`in2` change
- Registers as `"DeepCShuffle2"` via `Op::Description`

`src/DeepCShuffle.cpp` was restored to the original pre-rewrite 4-channel fixed-pair implementation for backward compatibility with existing Nuke scripts.

`src/CMakeLists.txt` was updated to:
- Add `DeepCShuffle2` to `PLUGINS` and `CHANNEL_NODES`
- Compile `ShuffleMatrixKnob.cpp` and `ShuffleMatrixWidget.cpp` into the `DeepCShuffle2` target
- Link `Qt6::Core Qt6::Gui Qt6::Widgets` into `DeepCShuffle2` (not `DeepCShuffle`)

## Commits

| Hash | Description |
|------|-------------|
| 9a4b5e4 | feat(03-04): rewrite DeepCShuffle with ShuffleMatrixKnob (Task 1 — superseded by rename) |
| f5161b6 | fix(03-04): restore original DeepCShuffle.cpp for backward compatibility |
| ae1c7c6 | feat(03-04): add DeepCShuffle2 with ShuffleMatrixKnob matrix UI |

## UAT Results

- `DeepCShuffle.so` loads in Nuke 16: **PASS**
- `DeepCShuffle2.so` loads in Nuke 16: **PASS** (no undefined symbol errors)
- All destructor symbols (`_ZN19ShuffleMatrixWidgetD1Ev`, `D0Ev`, `D2Ev`) are `T` (defined) in the `.so`

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Undefined destructor symbol in DeepCShuffle.so**
- **Found during:** Task 2 (UAT)
- **Issue:** The original task 1 rewrite put the ShuffleMatrixKnob code into `DeepCShuffle.cpp`, but the CMake build system was not reconfigured after `target_sources()` was added for `ShuffleMatrixWidget.cpp`. The stale `link.txt` omitted both widget `.cpp` files, leaving `_ZN19ShuffleMatrixWidgetD1Ev` and related symbols as `U` (undefined) in the `.so`.
- **Fix:** Created a new CMake target `DeepCShuffle2` that properly compiles and links `ShuffleMatrixKnob.cpp` and `ShuffleMatrixWidget.cpp`. Regenerated cmake configuration.
- **Files modified:** `src/CMakeLists.txt`
- **Commit:** ae1c7c6

**2. [User Request] Rename new implementation to DeepCShuffle2, preserve original DeepCShuffle**
- **Found during:** Task 2 (UAT — user rename request)
- **Issue:** The Task 1 rewrite replaced `DeepCShuffle.cpp` entirely, breaking backward compatibility with existing Nuke scripts that use the `DeepCShuffle` node.
- **Fix:** Restored `src/DeepCShuffle.cpp` to the pre-rewrite 4-channel fixed-pair implementation. Created `src/DeepCShuffle2.cpp` with the new ShuffleMatrixKnob implementation, class name `DeepCShuffle2`, node string `"DeepCShuffle2"`.
- **Files modified:** `src/DeepCShuffle.cpp` (restored), `src/DeepCShuffle2.cpp` (created), `src/CMakeLists.txt`
- **Commits:** f5161b6, ae1c7c6

## Self-Check: PASSED

- [x] `src/DeepCShuffle2.cpp` exists: FOUND
- [x] `src/DeepCShuffle.cpp` restored: FOUND (original 4-channel implementation)
- [x] `/workspace/build/src/DeepCShuffle2.so` exists: FOUND
- [x] `/workspace/build/src/DeepCShuffle.so` exists: FOUND
- [x] Commits f5161b6 and ae1c7c6 exist in git log
- [x] `_ZN19ShuffleMatrixWidgetD1Ev` is `T` (defined) in DeepCShuffle2.so
- [x] Both .so files load in Nuke 16 without errors
