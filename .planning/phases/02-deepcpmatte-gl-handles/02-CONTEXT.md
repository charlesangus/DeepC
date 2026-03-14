# Phase 2: DeepCPMatte GL Handles - Context

**Gathered:** 2026-03-14
**Status:** Ready for planning

<domain>
## Phase Boundary

Add a 3D wireframe GL handle to DeepCPMatte that visualizes the selection volume in the Nuke 3D viewport, fix the deadlock caused by calling `_validate()` on the GL thread, and make the handle interactive via the existing Axis_knob gizmo. Creating/modifying the selection math, adding handles to other nodes, and any new knobs beyond UI layout adjustments are out of scope.

</domain>

<decisions>
## Implementation Decisions

### Deadlock fix (GLPM-01)
- Remove the `_validate(false)` call from `draw_handle()` entirely
- Read from cached member variables (`_axisKnob`, `_shape`, `_falloff`, `_falloffGamma`) that are already populated by the time `draw_handle()` fires — no re-validation needed

### Wireframe geometry
- Dynamic shape: wireframe matches the `_shape` knob — sphere when sphere mode, cube when cube mode
- Two wireframe primitives drawn per frame:
  - **Outer boundary**: unit scale in the Axis_knob's local space — where selection falloff reaches zero
  - **Inner boundary**: scaled by `(1.0 - _falloff)` — where full selection ends and falloff begins
- Both drawn in world space after applying `_axisKnob` transform
- Sphere subdivision: simple, ~8 latitude rings × 8 longitude segments (fast, readable, standard for VFX handles)
- Single wire color, Claude's discretion (e.g., standard Nuke handle yellow); no active/idle color change

### Interaction model
- Clicking or dragging the wireframe activates the existing **Axis_knob gizmo** (translate + rotate + scale), not a custom drag handler
- Standard Nuke `ViewerContext` handle selection feedback used automatically via `add_draw_handle()` / `build_knob_handles()`
- No custom pick-and-drag code required

### Knob layout changes
- **Axis_knob exposed at top level**: remove the `BeginGroup("Position")` / `EndGroup` wrapper around `Axis_knob(f, &_axisKnob, "selection")` — the Axis is now the primary positioning control and should be immediately visible
- **center XY_knob kept at top**: 2D pixel picker stays as a convenience tool (click a pixel → Axis translate auto-populated from P channel at that point); no reorder

### Viewer mode
- `build_handles()` changed to activate the wireframe draw handle in **3D mode only** (invert the current `VIEWER_2D` bail-out guard to a `VIEWER_2D` check that skips `add_draw_handle`)
- `build_knob_handles(ctx)` retained to keep all knob handles (Axis gizmo XYZ arrows, center XY) active in the viewer

### Claude's Discretion
- Exact GL color values for the wireframe lines
- Whether to use `glPushMatrix`/`glPopMatrix` or load the Axis matrix directly
- Sphere tessellation implementation detail (parametric or precomputed points)
- Whether outer and inner wireframes are drawn with the same color or subtly distinguished (e.g., inner slightly transparent)

</decisions>

<specifics>
## Specific Ideas

- Two wireframes showing the falloff zone is the key visual requirement: the artist should be able to see both where full selection ends and where influence drops to zero — not just the outer boundary
- The interaction model deliberately reuses the existing Axis_knob gizmo rather than inventing custom drag logic — keeps the UX consistent with standard Nuke 3D nodes

</specifics>

<code_context>
## Existing Code Insights

### Reusable Assets
- `_axisKnob` (Matrix4 member): already stores the full Axis transform; already used in `wrappedPerSample` for the inverse-transform position test — same matrix drives the wireframe
- `_shape` (int member, sphere=0/cube=1): already present, drives wireframe shape choice
- `_falloff` (float member): already present, drives inner wireframe scale (`1 - _falloff`)
- `build_handles()` / `draw_handle()`: stubs already exist and are wired into the Op; only content needs changing
- `Axis_knob(f, &_axisKnob, "selection")`: already registered — removing its BeginGroup wrapper is a one-line UI change

### Established Patterns
- `_validate()` is the right place to precompute cached values (established in Phase 1, DeepCWorld inverse matrix)
- Plugin registration via `static Op::Description` + `build()` at bottom — no changes needed here
- `build_knob_handles(ctx)` is the standard first call in `build_handles()` — keep it

### Integration Points
- `DDImage/ViewerContext.h` already included at top of `DeepCPMatte.cpp`
- `DDImage/gl.h` already included — GL draw primitives available directly
- `build_handles()` currently guards `ctx->transform_mode() != VIEWER_2D` to bail out; change guard to only add the wireframe draw handle in 3D mode

</code_context>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope.

</deferred>

---

*Phase: 02-deepcpmatte-gl-handles*
*Context gathered: 2026-03-14*
