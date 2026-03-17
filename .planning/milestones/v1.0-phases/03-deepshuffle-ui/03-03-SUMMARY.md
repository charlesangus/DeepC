---
phase: 03-deepshuffle-ui
plan: "03"
subsystem: ui
tags: [ndk, qt6, custom-knob, qt-widget, serialization, channel-routing, deep-shuffle]

# Dependency graph
requires:
  - phase: 03-02
    provides: ShuffleMatrixKnob.h and ShuffleMatrixWidget.h header declarations with all method signatures
  - phase: 03-01
    provides: Qt6 CMake build infrastructure (AUTOMOC, Qt6::Core/Gui/Widgets linkage)

provides:
  - ShuffleMatrixKnob.cpp — full NDK Knob virtual method implementations (to_script, from_script, store, make_widget, setValue, setChannelSets, accessors)
  - ShuffleMatrixWidget.cpp — Qt toggle-button routing matrix with WidgetCallback lifecycle, buildLayout, clearLayout, syncFromKnob, onCellToggled
  - Both files compiled into DeepCShuffle.so via cmake target

affects:
  - 03-04 (DeepCShuffle Op integration — will wire knob_changed to call setChannelSets + updateWidgets)
  - phase-04 onwards (any phase that depends on DeepCShuffle being functional)

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "NDK custom Knob virtual method chain: to_script (cstring quoting), from_script (changed() call), store (hash.append + dest copy), make_widget (new Widget(this))"
    - "WidgetCallback static dispatch: kIsVisible with QTabWidget parent walk, kUpdateWidgets -> syncFromKnob, kDestroying -> null knob pointer"
    - "syncFromKnob rebuild pattern: clearLayout + buildLayout + parse matrixState + blockSignals during checked-state sync"
    - "onCellToggled single-source-per-row enforcement: erase sibling row entries from map, restore current cell entry after loop"
    - "Button objectName as outName|srcName identity token — avoids storing raw pointers in lambda captures"
    - "QSignalBlocker RAII guard preferred over manual blockSignals(true/false) pairs"
    - "Lambda capture by value for outputName/sourceName strings in QPushButton::toggled connection"

key-files:
  created:
    - src/ShuffleMatrixKnob.cpp
    - src/ShuffleMatrixWidget.cpp
  modified:
    - src/CMakeLists.txt

key-decisions:
  - "syncFromKnob rebuilds layout fully (clearLayout + buildLayout) rather than just updating button states — ensures column headers stay consistent with current ChannelSets at sync time"
  - "QSignalBlocker used in syncFromKnob instead of manual blockSignals pairs — RAII ensures unblocking even if code throws"
  - "onCellToggled erases all sibling entries first then re-inserts current entry — handles edge case where map already had the same output key"
  - "setMinimumHeight((outputCount + 1) * 28) provides a sensible panel height hint matching the 28px-per-row recommendation from RESEARCH.md"

patterns-established:
  - "NDK widget lifecycle pattern: addCallback in constructor, removeCallback in destructor, null-guard all _knob accesses"
  - "State serialization round-trip: parse CSV string to std::map, mutate map, serialize back with std::ostringstream — deterministic sorted output via std::map ordering"

requirements-completed: [SHUF-01, SHUF-02, SHUF-03]

# Metrics
duration: 2min
completed: 2026-03-15
---

# Phase 3 Plan 03: ShuffleMatrixKnob and ShuffleMatrixWidget Implementation Summary

**Custom NDK Knob with embedded Qt6 toggle-button routing matrix: serialization, WidgetCallback lifecycle, single-source-per-row enforcement, and clean DeepCShuffle.so build.**

## Performance

- **Duration:** ~2 min
- **Started:** 2026-03-15T14:45:52Z
- **Completed:** 2026-03-15T14:48:00Z
- **Tasks:** 2
- **Files modified:** 3

## Accomplishments

- Implemented all six NDK Knob virtual methods in `ShuffleMatrixKnob.cpp`: `Class()`, `not_default()`, `to_script()` (with `cstring()` quoting), `from_script()` (calls `changed()` for deferred widget sync), `store()` (hash contribution + dest copy), and `make_widget()` (returns `new ShuffleMatrixWidget(this)`)
- Implemented full `ShuffleMatrixWidget.cpp`: constructor/destructor callback registration, static `WidgetCallback` dispatch for all three `CallbackReason` cases, `buildLayout()` with up to 8 output rows and dynamic source columns from in1/in2 ChannelSets, `clearLayout()`, `syncFromKnob()` with signal blocking, and `onCellToggled()` with single-source-per-row constraint
- Both files compile cleanly as part of `DeepCShuffle.so`; `cmake --build /workspace/build --target DeepCShuffle` exits 0 with MOC running on `ShuffleMatrixWidget.h`

## Task Commits

Each task was committed atomically:

1. **Task 1: Implement ShuffleMatrixKnob.cpp** - `3ac5f74` (feat)
2. **Task 2: Implement ShuffleMatrixWidget.cpp** - `428dfac` (feat)

**Plan metadata:** (this commit)

## Files Created/Modified

- `src/ShuffleMatrixKnob.cpp` — NDK Knob virtual methods: serialization, widget factory, undo-aware setValue, ChannelSet storage
- `src/ShuffleMatrixWidget.cpp` — Qt widget: grid layout builder, callback lifecycle, state sync, user interaction handler
- `src/CMakeLists.txt` — Added `target_sources(DeepCShuffle PRIVATE ShuffleMatrixKnob.cpp ShuffleMatrixWidget.cpp)` inside `if(Qt6_FOUND)` block

## Decisions Made

- `syncFromKnob` does a full `clearLayout()` + `buildLayout()` rebuild rather than updating button states in-place. This ensures column headers always reflect the live ChannelSets that may have changed since the widget was first opened, at the cost of slightly more work per kUpdateWidgets event.
- `QSignalBlocker` used for signal blocking in `syncFromKnob` — RAII pattern preferred over manual `blockSignals(true/false)` pairs to ensure unblocking even if an exception occurs.
- `onCellToggled` erases all sibling row entries from the map and then re-inserts the current entry after the loop. This handles the edge case where the previous map state already had the same output key mapped to a different source.
- Button `objectName` stores `outName|srcName` as a string token rather than keeping lambda captures of both values. This avoids the need for a `_buttonMetadata` side table and makes button identity self-describing.

## Deviations from Plan

None — plan executed exactly as written.

## Issues Encountered

None. The build was clean after both files were created. The CMake reconfiguration error on the intermediate build (before `ShuffleMatrixWidget.cpp` existed) was expected and resolved by creating the file before the final build invocation.

## User Setup Required

None — no external service configuration required.

## Next Phase Readiness

- `ShuffleMatrixKnob` and `ShuffleMatrixWidget` are fully implemented and compiling
- Plan 03-04 can now wire `DeepCShuffle::knob_changed()` to call `setChannelSets()` and `updateWidgets()` on the matrix knob, and update `doDeepEngine()` to parse `_matrixState` for actual channel routing

## Self-Check: PASSED

- src/ShuffleMatrixKnob.cpp: FOUND
- src/ShuffleMatrixWidget.cpp: FOUND
- 03-03-SUMMARY.md: FOUND
- Commit 3ac5f74: FOUND
- Commit 428dfac: FOUND
- cmake --build /workspace/build --target DeepCShuffle: exits 0

---
*Phase: 03-deepshuffle-ui*
*Completed: 2026-03-15*
