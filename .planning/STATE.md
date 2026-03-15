---
gsd_state_version: 1.0
milestone: v1.0
milestone_name: milestone
status: "Phase 3 context gathered — ready for /gsd:plan-phase 3"
stopped_at: Completed 03-02-PLAN.md
last_updated: "2026-03-15T14:44:44.583Z"
last_activity: 2026-03-14 — Phase 3 discuss complete; 03-CONTEXT.md written
progress:
  total_phases: 5
  completed_phases: 2
  total_plans: 10
  completed_plans: 8
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

## Accumulated Context

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

### Pending Todos

None yet.

### Blockers/Concerns

- Phase 5 (DeepThinner): `bratgot/DeepThinner` repository was not found during research (LOW confidence). Source availability must be confirmed before Phase 5 can be planned. If unavailable, options are: locate alternative, implement from scratch, or defer from this milestone.

## Session Continuity

Last session: 2026-03-15T14:44:44.580Z
Stopped at: Completed 03-02-PLAN.md
Resume file: None
