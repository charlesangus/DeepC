---
gsd_state_version: 1.0
milestone: v1.0
milestone_name: milestone
status: "Phase 3 context gathered — ready for /gsd:plan-phase 3"
stopped_at: Completed 03.1-04 second UAT round — all 6 UAT failures + arrow size fixed
last_updated: "2026-03-16T10:16:20Z"
last_activity: 2026-03-14 — Phase 3 discuss complete; 03-CONTEXT.md written
progress:
  total_phases: 6
  completed_phases: 4
  total_plans: 14
  completed_plans: 14
  percent: 100
---

# Project State

## Project Reference

See: .planning/PROJECT.md (updated 2026-03-12)

**Core value:** Artists can use deep-compositing operations in Nuke that don't exist in the built-in toolset, with professional-quality UIs and reliable deep-pixel math.
**Current focus:** Phase 3 — DeepShuffle UI

## Current Position

Phase: 3 of 5 (DeepShuffle UI)
Plan: 0 of TBD — context complete, ready to plan
Status: Phase 3 context gathered — ready for /gsd:plan-phase 3
Last activity: 2026-03-14 — Phase 3 discuss complete; 03-CONTEXT.md written

Progress: [██████████] 100%

## Performance Metrics

**Velocity:**
- Total plans completed: 0
- Average duration: -
- Total execution time: 0 hours

**By Phase:**

| Phase | Plans | Total | Avg/Plan |
|-------|-------|-------|----------|
| - | - | - | - |

**Recent Trend:**
- Last 5 plans: -
- Trend: -

*Updated after each plan completion*
| Phase 01-codebase-sweep P01 | 1 | 3 tasks | 4 files |
| Phase 01-codebase-sweep P02 | 2 | 2 tasks | 2 files |
| Phase 01-codebase-sweep P03 | 8 | 2 tasks | 4 files |
| Phase 01-codebase-sweep P05 | 5 | 2 tasks | 2 files |
| Phase 01-codebase-sweep P04 | 2 | 1 tasks | 2 files |
| Phase 01-codebase-sweep P04 | 15 | 2 tasks | 2 files |
| Phase 02-deepcpmatte-gl-handles P01 | 12 | 2 tasks | 1 files |
| Phase 03-deepshuffle-ui P01 | 3 | 3 tasks | 1 files |
| Phase 03-deepshuffle-ui P02 | 9 | 2 tasks | 3 files |
| Phase 03-deepshuffle-ui P03 | 2 | 2 tasks | 3 files |
| Phase 03-deepshuffle-ui P04 | 35 | 2 tasks | 3 files |
| Phase 03.1-refine-and-fix-deepcshuffle-ui-behaviour P01 | 4 | 2 tasks | 4 files |
| Phase 03.1-refine-and-fix-deepcshuffle-ui-behaviour P02 | 1 | 2 tasks | 2 files |
| Phase 03.1-refine-and-fix-deepcshuffle-ui-behaviour P03 | 2 | 2 tasks | 2 files |
| Phase 03.1-refine-and-fix-deepcshuffle-ui-behaviour P04 | 3 | 1 tasks | 3 files |
| Phase 03.1-refine-and-fix-deepcshuffle-ui-behaviour P04 | 25 | 2 tasks | 3 files |
| Phase 03.1-refine-and-fix-deepcshuffle-ui-behaviour P04 UAT2 | 11 | 7 fixes | 5 files |

## Accumulated Context

### Roadmap Evolution

- Phase 03.1 inserted after Phase 3: refine and fix deepcshuffle ui/behaviour (URGENT)

### Decisions

Decisions are logged in PROJECT.md Key Decisions table.
Recent decisions affecting current work:

- Roadmap: SWEEP-07 (`perSampleData` redesign) must be executed before any new wrapper subclass is written — it is a breaking virtual interface change
- Roadmap: GLPM-01 (deadlock fix in `draw_handle()`) must be the first action in Phase 2 before any handle drawing code is added
- Roadmap: THIN-01 (license confirmation) must be completed before THIN-02 (CMake integration) — no source added to repo before GPL-3.0 compatibility is confirmed
- [Phase 01-codebase-sweep]: Three confirmed deep-pixel math bugs fixed with minimal surgical one-line changes: DeepCGrade reverse gamma discard, DeepCKeymix A-side copy-paste error, zero-alpha NaN in DeepCSaturation and DeepCHueShift
- [Phase 01-codebase-sweep]: Zero-alpha guard pattern established: if (alpha != 0.0f) wrapping unpremult division with implicit else via zero-initialized storage
- [Phase 01-codebase-sweep]: DeepCConstant weight fix: comma expression removed, behavior unchanged (was already yielding depth/_overallDepth)
- [Phase 01-codebase-sweep]: DeepCID loop body fix: _auxChannel replaced with z inside foreach loop; no-op for single-member set but correct for multi-member sets
- [Phase 01-codebase-sweep]: DeepCWrapper and DeepCMWrapper base class plugin registrations removed — only concrete plugin subclasses register themselves in Nuke node menu
- [Phase 01-codebase-sweep]: Class() returning d.name removed from both base headers alongside their Op::Description definitions — inseparable pair to avoid dangling references
- [Phase 01-codebase-sweep]: DeepCWorld inverse matrix cached as Matrix4 _inverse_window_matrix member in _validate(), eliminating per-sample matrix inversion in processSample()
- [Phase 01-codebase-sweep]: SWEEP-10 NDK API audit: DeepPixelOp and DeepFilterOp are not deprecated in Nuke 16; OutputContext two-arg constructor is deprecated but not used — no API migration required
- [Phase 01-codebase-sweep]: DeepCBlink removed entirely: broken GPU knob (hardcoded CPU), >4 channel bail-out, calloc leaks — removal cleaner than retention
- [Phase 01-codebase-sweep]: NUKE_RIPFRAMEWORK_LIBRARY in cmake/FindNuke.cmake retained — shared Nuke package detection infrastructure, not DeepCBlink-specific
- [Phase 01-codebase-sweep]: DeepCBlink removed entirely: broken GPU knob (hardcoded CPU), >4 channel bail-out, calloc leaks — removal cleaner than retention
- [Phase 01-codebase-sweep]: NUKE_RIPFRAMEWORK_LIBRARY in cmake/FindNuke.cmake retained — shared Nuke package detection infrastructure, not DeepCBlink-specific
- [Phase 02-deepcpmatte-gl-handles]: Removed _validate() from draw_handle() entirely — cached members populated before GL draw fires, no re-validation needed on GL thread
- [Phase 02-deepcpmatte-gl-handles]: gl_sphere(0.5f) + gl_cubef(0,0,0,1.0f) at unit scale in Axis local space — matching diameter for sphere and cube wireframe shapes
- [Phase 02-deepcpmatte-gl-handles]: innerScale > 0.001f guard prevents degenerate zero-scale inner wireframe draw when _falloff == 1.0f (default)
- [Phase 03-deepshuffle-ui]: Single Op input (not two); in1 + optional in2 ChannelSet pickers both read from same input stream
- [Phase 03-deepshuffle-ui]: Routing UI is a C++ custom Knob with embedded Qt matrix widget — reproduces legacy Shuffle visual matrix, serialized to string knob
- [Phase 03-deepshuffle-ui]: Unselected channels pass through unchanged; no DAG port label changes
- [Phase 03-deepshuffle-ui]: Used pre-installed Qt 6.5.3 at /opt/Qt/6.5.3/gcc_64 instead of apt install qt6-base-dev — exact version match with Nuke 16 runtime, no sudo required
- [Phase 03-deepshuffle-ui]: CMAKE_PREFIX_PATH append approach for Qt6 discovery; Qt6 link uses versioned targets Qt6::Core/Gui/Widgets; AUTOMOC enabled conditionally inside if(Qt6_FOUND)
- [Phase 03-deepshuffle-ui]: WidgetPointer is a global typedef (not DD::Image::WidgetPointer) — use bare WidgetPointer in make_widget() override return type
- [Phase 03-deepshuffle-ui]: ShuffleMatrixKnob.h and ShuffleMatrixWidget.h use mutual forward declarations to avoid circular includes; .cpp files include both
- [Phase 03-deepshuffle-ui]: Qt6 discovery uses list(APPEND CMAKE_PREFIX_PATH /opt/Qt/6.5.3/gcc_64) before find_package — HINTS keyword did not set Qt6_FOUND reliably
- [Phase 03-deepshuffle-ui]: syncFromKnob does full clearLayout+buildLayout rebuild to keep column headers consistent with live ChannelSets
- [Phase 03-deepshuffle-ui]: QSignalBlocker RAII used in syncFromKnob instead of manual blockSignals pairs
- [Phase 03-deepshuffle-ui]: Button objectName stores outName|srcName as self-describing identity token, avoiding a separate metadata side table
- [Phase 03-deepshuffle-ui]: New implementation registered as DeepCShuffle2; original DeepCShuffle preserved unchanged for backward compatibility
- [Phase 03-deepshuffle-ui]: ShuffleMatrixWidget.cpp linker error fixed by creating separate DeepCShuffle2 CMake target that properly compiles widget sources
- [Phase 03.1-refine-and-fix-deepcshuffle-ui-behaviour]: Radio no-op guard in onCellToggled — if (!checked) return; prevents rows from being left with no source
- [Phase 03.1-refine-and-fix-deepcshuffle-ui-behaviour]: outputGroup scopes radio enforcement — out1 and out2 rows with identical channel names stay independent
- [Phase 03.1-refine-and-fix-deepcshuffle-ui-behaviour]: const buttons serialized with inputGroup 'in1' — const:0 and const:1 are virtual in1 sources
- [Phase 03.1-refine-and-fix-deepcshuffle-ui-behaviour]: initializeState() does not call changed() — avoids recursive rebuild when writing identity routing on first open
- [Phase 03.1-refine-and-fix-deepcshuffle-ui-behaviour]: buildGroup accepts buttonsDisabled param — row-level disable propagates to in1/const/outLabel; in2 buttons use separate in2Disabled flag captured from outer scope
- [Phase 03.1-refine-and-fix-deepcshuffle-ui-behaviour]: make_widget calls syncFromKnob() after construction — compensates for showPanel timing gap that caused first-open stale headers
- [Phase 03.1-refine-and-fix-deepcshuffle-ui-behaviour]: ChannelButton uses direct QPainter rendering in paintEvent — no Qt stylesheet — for precise colored fill, border, and X-mark overlay
- [Phase 03.1-refine-and-fix-deepcshuffle-ui-behaviour]: const:0 button near-black (30,30,30) and const:1 near-white (220,220,220) visually indicate 0.0 and 1.0 constant source values
- [Phase 03.1-refine-and-fix-deepcshuffle-ui-behaviour]: Knob::INVISIBLE + KNOB_CHANGED_ALWAYS: hides panel row while keeping knob_changed() firing and .nk serialization intact
- [Phase 03.1-refine-and-fix-deepcshuffle-ui-behaviour]: QComboBox pickers recreated each buildLayout() call (nulled in clearLayout) — simpler than re-parenting approach
- [Phase 03.1-refine-and-fix-deepcshuffle-ui-behaviour]: onPickerChanged calls set_text() then explicit knob_changed() to guarantee matrix rebuild fires on any NDK impl
- [Phase 03.1-refine-and-fix-deepcshuffle-ui-behaviour]: QComboBox pickers reverted: segfault at pick time and incorrect channel display; native NDK ChannelSet knobs used instead
- [Phase 03.1-refine-and-fix-deepcshuffle-ui-behaviour]: NDK knob ordering: in1/in2 before CustomKnob2 appear above matrix widget, out1/out2 after appear below — top-to-bottom NDK panel layout
- [Phase 03.1-refine-and-fix-deepcshuffle-ui-behaviour]: Removed spacerCol (12px gap column between const and in2 groups) — QGridLayout 2px spacing is sufficient, dedicated column created visible gap
- [Phase 03.1-refine-and-fix-deepcshuffle-ui-behaviour]: Radio scope must use stable groupId ("out1"/"out2") not the human-readable layer name — layer names can collide when out1 and out2 select the same layer
- [Phase 03.1-refine-and-fix-deepcshuffle-ui-behaviour]: syncWidgetNow() pattern: knob stores _widget pointer from make_widget(); knob_changed calls syncWidgetNow() synchronously after setChannelSets() to bypass async updateWidgets() lag
- [Phase 03.1-refine-and-fix-deepcshuffle-ui-behaviour]: QComboBox re-embedded with QTimer::singleShot(0) deferral — breaks re-entrant signal dispatch that caused segfault; NDK ChannelSet knobs marked INVISIBLE
- [Phase 03.1-refine-and-fix-deepcshuffle-ui-behaviour]: in2=Chan_Black placeholder columns: insert fixed rgba.red/green/blue/alpha names when in2Columns is empty after forEach iteration
- [Phase 03.1-refine-and-fix-deepcshuffle-ui-behaviour]: X-mark color: check fillColor.lightnessF() > 0.5 and use Qt::black on light backgrounds (const:1 near-white fill)
- [Phase 03.1-refine-and-fix-deepcshuffle-ui-behaviour]: Column stretch: set all button columns to stretch=0, only outLabelCol to stretch=1 — prevents QGridLayout from expanding button columns
- [Phase 03.1-refine-and-fix-deepcshuffle-ui-behaviour]: Arrow size: makeArrowLabel() with pointSize+4; use double-stroke glyphs ⇓/⇒ (U+21D3/U+21D2) for wider visual weight

### Pending Todos

None yet.

### Blockers/Concerns

- Phase 5 (DeepThinner): `bratgot/DeepThinner` repository was not found during research (LOW confidence). Source availability must be confirmed before Phase 5 can be planned. If unavailable, options are: locate alternative, implement from scratch, or defer from this milestone.

## Session Continuity

Last session: 2026-03-16T08:42:56.761Z
Stopped at: Completed 03.1-04-PLAN.md — Phase 03.1 complete
Resume file: None
