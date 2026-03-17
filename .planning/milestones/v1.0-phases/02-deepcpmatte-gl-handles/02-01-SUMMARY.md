---
phase: 02-deepcpmatte-gl-handles
plan: 01
subsystem: ui
tags: [opengl, nuke-ndk, gl-handles, wireframe, deepcpmatte]

# Dependency graph
requires:
  - phase: 01-codebase-sweep
    provides: Cleaned, building codebase with correct member variables (_axisKnob, _shape, _falloff) in place
provides:
  - Deadlock-free draw_handle() — no _validate() call on GL thread
  - Wireframe outer + inner boundary geometry in 3D viewport using gl_sphere / gl_cubef
  - Correct build_handles() 2D/3D guard — wireframe only shown in 3D viewer mode
  - Axis_knob exposed at top level of properties panel (BeginGroup wrapper removed)
affects: [03-deepcpmatte-interaction, any phase adding further GL handles to DeepC nodes]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "GL draw reads only from cached member variables (_axisKnob, _shape, _falloff) — never calls _validate() on GL thread"
    - "glPushMatrix / glMultMatrixf(_axisKnob.array()) pattern to place wireframe in Axis local space"
    - "innerScale > 0.001f guard prevents degenerate zero-scale draw when _falloff == 1.0f"
    - "ctx->node_color() for wire color per Nuke convention"

key-files:
  created: []
  modified:
    - src/DeepCPMatte.cpp

key-decisions:
  - "Removed _validate() from draw_handle() entirely — cached members are already populated before draw fires"
  - "Single glPushMatrix/glPopMatrix pair wraps both outer and inner wireframe draws"
  - "gl_sphere(0.5f) paired with gl_cubef(0,0,0,1.0f) so both shapes have matching unit-scale diameter"
  - "innerScale computed as (1.0f - _falloff); draw skipped when <= 0.001f to avoid degenerate zero-scale call"
  - "ctx->node_color() used for wire color — matches Nuke node chip color convention"

patterns-established:
  - "GL thread safety: draw_handle() reads only from validated member cache, never calls _validate()"
  - "Axis-space wireframe: glMultMatrixf(_axisKnob.array()) before geometry calls places handles in local space"

requirements-completed: [GLPM-01, GLPM-02, GLPM-03]

# Metrics
duration: 12min
completed: 2026-03-14
---

# Phase 2 Plan 01: DeepCPMatte GL Handles Summary

**Deadlock-free 3D wireframe handle for DeepCPMatte selection volume using gl_sphere/gl_cubef in Axis local space, with corrected 2D/3D guard and Axis_knob promoted to top-level properties panel**

## Performance

- **Duration:** 12 min
- **Started:** 2026-03-14T13:10:00Z
- **Completed:** 2026-03-14T13:22:00Z
- **Tasks:** 2
- **Files modified:** 1

## Accomplishments

- Removed `DeepCPMatte::_validate(false)` from `draw_handle()` — eliminates the GL-thread deadlock when a 3D viewer is open
- Added proper wireframe visualization: outer boundary at unit scale + inner boundary scaled by `(1 - _falloff)`, both matching the active `_shape` (sphere or cube)
- Fixed inverted `transform_mode()` guard in `build_handles()` so `add_draw_handle` fires only in 3D viewer mode
- Removed `BeginGroup("Position")` / `EndGroup` wrapper — Axis_knob now appears at the top level of the properties panel

## Task Commits

Each task was committed atomically:

1. **Task 1: Fix build_handles() guard and remove Axis_knob BeginGroup wrapper** - `314c925` (fix)
2. **Task 2: Replace draw_handle() — remove _validate() deadlock, add wireframe geometry** - `d89b516` (fix)

## Files Created/Modified

- `/workspace/src/DeepCPMatte.cpp` - Fixed build_handles() guard, removed BeginGroup/EndGroup, replaced draw_handle() body with proper wireframe geometry

## Decisions Made

- Used `ctx->node_color()` for wire color — consistent with Nuke node chip color convention, no hardcoded color needed
- Used a single `glPushMatrix`/`glPopMatrix` pair to wrap both outer and inner wireframe draws — cleaner than per-draw push/pop
- `gl_sphere(0.5f)` radius chosen to match `gl_cubef(0,0,0,1.0f)` unit-diameter cube at the same Axis scale
- Inner wireframe draw guarded at `innerScale > 0.001f` (not the plan's `>= 0.999f` on `_falloff`) — functionally equivalent, more readable as a direct scale check

## Deviations from Plan

None — plan executed exactly as written. The `innerScale > 0.001f` threshold is equivalent to the plan's `_falloff >= 0.999f` skip condition and was specified in the task action body.

## Issues Encountered

None — both edits were surgical and the build was clean on the first attempt.

## User Setup Required

None — no external service configuration required.

## Next Phase Readiness

- All three GLPM requirements satisfied: deadlock fixed (GLPM-01), wireframe geometry added (GLPM-02), guard and knob layout corrected (GLPM-03)
- UAT required before phase close: open DeepCPMatte in Nuke 16.0v6 with a 3D viewer, confirm no hang, wireframe visible at Axis position, dragging handle activates Axis gizmo
- No blockers for subsequent phases

## Self-Check: PASSED

- src/DeepCPMatte.cpp: FOUND
- 02-01-SUMMARY.md: FOUND
- Commit 314c925: FOUND
- Commit d89b516: FOUND

---
*Phase: 02-deepcpmatte-gl-handles*
*Completed: 2026-03-14*
