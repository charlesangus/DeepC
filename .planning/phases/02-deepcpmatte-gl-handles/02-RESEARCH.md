# Phase 2: DeepCPMatte GL Handles - Research

**Researched:** 2026-03-14
**Domain:** Nuke NDK OpenGL handle drawing, DDImage ViewerContext API, Matrix4 transform pipeline
**Confidence:** HIGH

---

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions

**Deadlock fix (GLPM-01)**
- Remove the `_validate(false)` call from `draw_handle()` entirely
- Read from cached member variables (`_axisKnob`, `_shape`, `_falloff`, `_falloffGamma`) that are already populated by the time `draw_handle()` fires — no re-validation needed

**Wireframe geometry**
- Dynamic shape: wireframe matches the `_shape` knob — sphere when sphere mode, cube when cube mode
- Two wireframe primitives drawn per frame:
  - Outer boundary: unit scale in the Axis_knob's local space — where selection falloff reaches zero
  - Inner boundary: scaled by `(1.0 - _falloff)` — where full selection ends and falloff begins
- Both drawn in world space after applying `_axisKnob` transform
- Sphere subdivision: simple, ~8 latitude rings x 8 longitude segments (fast, readable, standard for VFX handles)
- Single wire color, Claude's discretion (e.g., standard Nuke handle yellow); no active/idle color change

**Interaction model**
- Clicking or dragging the wireframe activates the existing Axis_knob gizmo (translate + rotate + scale), not a custom drag handler
- Standard Nuke ViewerContext handle selection feedback used automatically via `add_draw_handle()` / `build_knob_handles()`
- No custom pick-and-drag code required

**Knob layout changes**
- Axis_knob exposed at top level: remove the `BeginGroup("Position")` / `EndGroup` wrapper around `Axis_knob(f, &_axisKnob, "selection")` — the Axis is now the primary positioning control and should be immediately visible
- center XY_knob kept at top: 2D pixel picker stays as a convenience tool (click a pixel to auto-populate Axis translate from P channel); no reorder

**Viewer mode**
- `build_handles()` changed to activate the wireframe draw handle in 3D mode only (invert the current `VIEWER_2D` bail-out guard to a `VIEWER_2D` check that skips `add_draw_handle`)
- `build_knob_handles(ctx)` retained to keep all knob handles (Axis gizmo XYZ arrows, center XY) active in the viewer

### Claude's Discretion
- Exact GL color values for the wireframe lines
- Whether to use `glPushMatrix`/`glPopMatrix` or load the Axis matrix directly
- Sphere tessellation implementation detail (parametric or precomputed points)
- Whether outer and inner wireframes are drawn with the same color or subtly distinguished (e.g., inner slightly transparent)

### Deferred Ideas (OUT OF SCOPE)
None — discussion stayed within phase scope.
</user_constraints>

---

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| GLPM-01 | `draw_handle()` in DeepCPMatte no longer calls `_validate()` on the GL thread (deadlock fix) | Confirmed: `_validate()` call chain reaches `DeepFilterOp::_validate()` which calls `input0()->validate()`, a thread-unsafe operation on the GL thread. Cached member variables (`_axisKnob`, `_shape`, `_falloff`, `_falloffGamma`) are already populated before `draw_handle()` fires and are safe to read directly. |
| GLPM-02 | DeepCPMatte draws a wireframe sphere or cube in the 3D viewport representing the current selection volume | Confirmed: NDK provides `gl_sphere(float radius)` and `gl_cubef(float x, float y, float z, float width)` in `DDImage/gl.h`. Matrix transform via `glPushMatrix` / `glMultMatrixf(_axisKnob.array())` / `glPopMatrix` places geometry in world space. Two draw calls per frame (outer + inner) via `glScalef`. |
| GLPM-03 | The wireframe handle in DeepCPMatte is interactive — user can click and drag to reposition the selection volume | Confirmed: `build_knob_handles(ctx)` registers all Axis_knob gizmo arrows automatically. Removing `BeginGroup`/`EndGroup` wrapper makes the Axis_knob directly accessible. No custom drag code needed. |
</phase_requirements>

---

## Summary

Phase 2 is a focused NDK/OpenGL task with three tightly coupled changes to a single file (`DeepCPMatte.cpp`). All required NDK primitives exist in the Nuke 16.0v6 headers installed at `/usr/local/Nuke16.0v6/include/DDImage/`.

The deadlock (GLPM-01) is caused by `draw_handle()` calling `DeepCPMatte::_validate(false)`, which chains through `DeepCMWrapper::_validate` -> `DeepCWrapper::_validate` -> `DeepFilterOp::_validate` -> `input0()->validate()`. Calling `validate()` on an upstream op from the GL draw thread causes a mutex deadlock against Nuke's cook thread. The fix is a one-line deletion; all data `draw_handle()` needs is in member variables already written by the time the GL callback fires.

The wireframe (GLPM-02) uses two confirmed NDK helper functions: `gl_sphere(float radius)` draws a wireframe sphere centered at the GL origin, and `gl_cubef(float x, float y, float z, float width)` draws a wireframe cube. Both are in `DDImage/gl.h` (confirmed from Nuke 16.0v6 header, HIGH confidence). Applying the Axis_knob's Matrix4 via `glPushMatrix` / `glMultMatrixf` / `glPopMatrix` places both shapes in world space without hand-computing any vertices. The inner wireframe is produced by a `glScalef(s, s, s)` where `s = 1.0f - _falloff` after multiplying the Axis matrix.

Interactivity (GLPM-03) is automatically provided: `build_knob_handles(ctx)` registers all knob draw handles including the Axis gizmo (translate/rotate/scale arrows). The Axis gizmo is already wired to `_axisKnob`. Removing the `BeginGroup("Position")` / `EndGroup` wrapper makes the Axis control visible at the top of the panel. No custom `PUSH`/`DRAG` handler is required.

**Primary recommendation:** Three localized changes — (1) delete `DeepCPMatte::_validate(false)` from `draw_handle()`, (2) replace the 2D dummy draw in `draw_handle()` with `glPushMatrix`/`glMultMatrixf`/`gl_sphere` or `gl_cubef` + inner scaled repeat + `glPopMatrix`, (3) flip the `build_handles()` 2D guard logic and remove the BeginGroup wrapper in `custom_knobs()`.

---

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| DDImage/gl.h | Nuke 16.0v6 NDK | Wireframe primitives (`gl_sphere`, `gl_cubef`, `glPushMatrix` etc.) | Only supported GL interface for Nuke plugins; wraps platform GL and FoundryGL (macOS) uniformly |
| DDImage/ViewerContext.h | Nuke 16.0v6 NDK | Handle registration, draw pass gating, color accessors | Required for all Nuke 3D viewport interactions |
| DDImage/Matrix4.h | Nuke 16.0v6 NDK | Axis transform, `array()` accessor for `glMultMatrixf` | Already in use in `wrappedPerSample`; same matrix drives both math and visualization |

### Supporting
| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| Standard `<cmath>` | C++17 (project standard for Nuke 16+) | `cosf`/`sinf` for parametric sphere tessellation if hand-rolling | Only if `gl_sphere` doesn't match required subdivision count. `gl_sphere` is confirmed available so this is discretionary. |

### Alternatives Considered
| Instead of | Could Use | Tradeoff |
|------------|-----------|----------|
| `gl_sphere(radius)` | Hand-rolled parametric sphere with 8x8 lat/lon GL_LINES | `gl_sphere` uses ~51-point precomputed cosine table, not configurable subdivision — acceptable for handles; hand-roll only if exact 8x8 ring count is required |
| `glMultMatrixf(_axisKnob.array())` | Decompose Matrix4 into translate/rotate/scale GL calls | Direct matrix multiply is one call, numerically exact, no decomposition risk |
| `ctx->node_color()` for wire color | Hardcoded yellow (0xffff00ff) | `node_color()` auto-uses the node's chip color; matches Nuke convention. `ctx->selected_color()` is available for the selected state but CONTEXT.md locks to single wire color. |

**Installation:** No new libraries needed — all headers are already included in `DeepCPMatte.cpp`.

---

## Architecture Patterns

### Recommended Project Structure
```
src/
└── DeepCPMatte.cpp    # All changes confined here — no new files required
```

### Pattern 1: 3D Wireframe Draw Handle
**What:** Register a draw callback only in 3D viewer mode; gate drawing on `ctx->draw_lines()`; push/mult/pop matrix with Axis transform; call NDK wireframe helpers.
**When to use:** Any Nuke filter Op needing a 3D shape overlay in the viewport.
**Example:**
```cpp
// Source: NDK Draw3D.cpp example + NDK gl.h
void build_handles(ViewerContext* ctx)
{
    build_knob_handles(ctx);              // always — registers Axis gizmo arrows
    if (ctx->transform_mode() == VIEWER_2D)
        return;                           // skip wireframe in 2D mode
    add_draw_handle(ctx);                 // request draw_handle() callback in 3D mode
}

void draw_handle(ViewerContext* ctx)
{
    if (!ctx->draw_lines())              // only draw during the lines pass
        return;

    glColor(ctx->node_color());          // use node chip color

    // Outer wireframe — unit scale in Axis local space
    glPushMatrix();
    glMultMatrixf(_axisKnob.array());    // Matrix4::array() returns float* for OpenGL
    if (_shape == 0)
        gl_sphere(0.5f);                 // radius 0.5 = diameter 1 = unit cube equiv
    else
        gl_cubef(0.0f, 0.0f, 0.0f, 1.0f);

    // Inner wireframe — scaled by (1 - _falloff)
    float innerScale = 1.0f - _falloff;
    glScalef(innerScale, innerScale, innerScale);
    if (_shape == 0)
        gl_sphere(0.5f);
    else
        gl_cubef(0.0f, 0.0f, 0.0f, 1.0f);
    glPopMatrix();
}
```

### Pattern 2: Axis Knob at Top Level (No Group Wrapper)
**What:** Register `Axis_knob` directly without `BeginGroup`/`EndGroup` so it is immediately visible in the control panel.
**When to use:** When the Axis_knob is the primary positioning control, not a hidden sub-transform.
**Example:**
```cpp
// Before (Phase 1 state):
BeginGroup(f, "Position");
Axis_knob(f, &_axisKnob, "selection");
EndGroup(f);

// After (Phase 2):
Axis_knob(f, &_axisKnob, "selection");
```

### Anti-Patterns to Avoid
- **Calling `_validate()` from `draw_handle()`:** The GL draw thread runs concurrently with Nuke's cook thread. Calling `validate()` on any op from the draw thread will deadlock against Nuke's internal lock. Never call `_validate()`, `validate()`, or any method that calls `input0()->validate()` from `draw_handle()`.
- **Drawing during the wrong pass:** Always gate 3D wireframe drawing on `ctx->draw_lines()`. Drawing during `DRAW_OPAQUE` without testing the pass produces z-fighting and unpredictable compositing with geo in the scene.
- **Leaving `add_draw_handle` in 2D mode:** The existing code calls `add_draw_handle(ctx)` when `transform_mode() != VIEWER_2D`, which means it fires in 2D mode. The guard must be inverted: skip `add_draw_handle` when in 2D mode.
- **Using `glPopMatrix` without matching `glPushMatrix`:** Always wrap matrix changes in push/pop pairs. Missing pop corrupts the GL matrix stack for subsequent draws in the frame.

---

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Wireframe sphere | Parametric vertex loop with `glBegin(GL_LINES)` | `gl_sphere(float radius)` in DDImage/gl.h | NDK provides a precomputed function; avoids trigonometry at draw time |
| Wireframe cube | 12 `glVertex3f` pairs for 12 edges | `gl_cubef(float x, float y, float z, float width)` in DDImage/gl.h | One call, no manual vertex arithmetic |
| Applying Matrix4 to GL | Decompose into glTranslate / glRotate / glScale | `glMultMatrixf(_axisKnob.array())` | `Matrix4::array()` returns a float pointer in the correct column-major order for `glMultMatrixf` |
| Axis gizmo interactivity | Custom PUSH/DRAG handler | `build_knob_handles(ctx)` + `Axis_knob` | The Axis_knob already owns the 3D translate/rotate/scale arrows; `build_knob_handles` wires them into the viewer automatically |

**Key insight:** The Nuke NDK ships every primitive this phase needs. The only novel code is the two-draw outer/inner structure and the `_shape` branch — the geometry itself is delegated entirely to NDK helpers.

---

## Common Pitfalls

### Pitfall 1: Deadlock from `_validate()` on GL Thread
**What goes wrong:** Nuke hangs when the 3D viewer is active; spinning cursor, no recovery without force-quit.
**Why it happens:** `DeepCPMatte::_validate(false)` eventually calls `DeepFilterOp::_validate` which calls `input0()->validate()`. Nuke's cook lock is held by the cook thread; the GL thread cannot acquire it.
**How to avoid:** Delete the `DeepCPMatte::_validate(false)` line from `draw_handle()`. All member variables needed for drawing (`_axisKnob`, `_shape`, `_falloff`) are written in `_validate()` on the cook thread before any draw occurs.
**Warning signs:** Node deadlocks only when a 3D viewer is open; works fine in 2D. Debug output from `draw_handle()` never appears, or appears once and then hangs.

### Pitfall 2: Inverted 2D/3D Guard in `build_handles()`
**What goes wrong:** The wireframe appears in the 2D viewer (where it draws garbage), or never appears in the 3D viewer.
**Why it happens:** The existing guard is `if (ctx->transform_mode() != VIEWER_2D) return;` — this returns (skips) when NOT in 2D, i.e., it only runs in 3D. This is the inverted logic from what we want. The corrected version should skip `add_draw_handle` when in 2D.
**How to avoid:** Use `if (ctx->transform_mode() == VIEWER_2D) return;` before `add_draw_handle(ctx)`, matching the Draw3D NDK example pattern.
**Warning signs:** Either wireframe appears in 2D viewer overlaid on footage, or 3D viewport shows no wireframe at all.

### Pitfall 3: `gl_sphere` Draws at Origin, Not at Axis Position
**What goes wrong:** Sphere appears at world origin (0,0,0) regardless of Axis_knob position.
**Why it happens:** `gl_sphere()` draws relative to the current GL matrix. Without pushing the Axis matrix first, it draws at the GL identity origin.
**How to avoid:** Always wrap `gl_sphere()` / `gl_cubef()` with `glPushMatrix()` / `glMultMatrixf(_axisKnob.array())` before the draw call and `glPopMatrix()` after.

### Pitfall 4: Inner Wireframe Scale Order
**What goes wrong:** Inner boundary is drawn at wrong size or in wrong coordinate space.
**Why it happens:** `glScalef` accumulates with whatever matrix is already on the stack. Calling `glScalef` before `glMultMatrixf` will scale in world space, not local Axis space.
**How to avoid:** The inner wireframe must be drawn while the Axis matrix is still active. Approach: push once, multiply Axis matrix, draw outer, then `glScalef(s, s, s)`, draw inner, pop once. The scale is then applied in Axis local space (correct).

### Pitfall 5: `gl_cubef` `width` Parameter Semantics
**What goes wrong:** Cube is half the expected size, or dimensions are wrong.
**Why it happens:** `gl_cubef(x, y, z, width)` draws a cube of the given `width` (edge length) centered at `(x, y, z)`. For a unit cube, pass `width = 1.0f`. Because the Axis matrix already encodes scale, after `glMultMatrixf` the cube just needs `gl_cubef(0,0,0, 1.0f)`.
**Warning signs:** Cube appears but doesn't match sphere scale intuitively.

---

## Code Examples

Verified patterns from official NDK sources:

### Correct `build_handles()` for 3D-only wireframe
```cpp
// Source: NDK Draw3D.cpp example (matches SimpleAxis.cpp pattern)
void DeepCPMatte::build_handles(ViewerContext* ctx)
{
    build_knob_handles(ctx);           // registers Axis gizmo arrows in both 2D and 3D
    if (ctx->transform_mode() == VIEWER_2D)
        return;                        // wireframe only in 3D mode
    add_draw_handle(ctx);              // request draw_handle() callback
}
```

### Correct `draw_handle()` with push/pop matrix
```cpp
// Source: NDK SimpleAxis.cpp (glPushMatrix/glMultMatrixf pattern)
// Source: NDK gl.h (gl_sphere, gl_cubef signatures)
void DeepCPMatte::draw_handle(ViewerContext* ctx)
{
    if (!ctx->draw_lines())
        return;

    glColor(ctx->node_color());

    glPushMatrix();
    glMultMatrixf(_axisKnob.array());  // place in Axis local space

    // Outer wireframe (unit scale = falloff-zero boundary)
    if (_shape == 0)
        gl_sphere(0.5f);
    else
        gl_cubef(0.0f, 0.0f, 0.0f, 1.0f);

    // Inner wireframe (full-selection boundary)
    float innerScale = 1.0f - _falloff;
    glScalef(innerScale, innerScale, innerScale);
    if (_shape == 0)
        gl_sphere(0.5f);
    else
        gl_cubef(0.0f, 0.0f, 0.0f, 1.0f);

    glPopMatrix();
}
```

### NDK color accessor usage (confirmed from ViewerContext.h)
```cpp
// Source: /usr/local/Nuke16.0v6/include/DDImage/ViewerContext.h lines 906-926
// node_color() — the node's chip color (good default for lines)
// selected_color() — Nuke highlight color when node is selected
// fg_color() — foreground; recommended by NDK for filled geometry
glColor(ctx->node_color());              // unsigned, passed to DDImage::glColor(unsigned)
```

### Matrix4::array() for OpenGL (confirmed from Matrix4.h)
```cpp
// Source: /usr/local/Nuke16.0v6/include/DDImage/Matrix4.h line 97
// "Return a pointer to a00. This array is in the correct order to pass to OpenGL"
const float* array() const { return &a00; }
// Usage:
glMultMatrixf(_axisKnob.array());
```

### VIEWER_2D constant (confirmed from ViewerContext.h)
```cpp
// Source: /usr/local/Nuke16.0v6/include/DDImage/ViewerContext.h line 143
// ViewerMode enum — VIEWER_2D = 0
// transform_mode() returns int; VIEWER_2D is in the same namespace (DD::Image::VIEWER_2D)
if (ctx->transform_mode() == VIEWER_2D) { ... }
```

---

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| Calling `_validate()` from `draw_handle()` | Read cached member variables only | This phase | Eliminates GL-thread deadlock |
| Axis_knob hidden in BeginGroup | Axis_knob at top level | This phase | Makes primary control immediately visible |
| `build_handles()` guard: skip in 3D mode | Guard: skip `add_draw_handle` in 2D mode | This phase | Wireframe appears in 3D viewport as intended |

**Deprecated/outdated:**
- The `glVertex2d(_center.x, _center.y)` stub draw: replaced by proper 3D wireframe via `gl_sphere`/`gl_cubef`. The `_center` member remains (used by `knob_changed`); only the GL draw changes.

---

## Open Questions

1. **`gl_sphere` radius convention vs. `gl_cubef` width convention**
   - What we know: `gl_sphere(float radius = 0.5)` — default radius 0.5 = diameter 1 unit. `gl_cubef(x, y, z, float width)` — `width` is the edge length, centered at (x,y,z).
   - What's unclear: For the sphere case, passing `radius = 0.5` after `glMultMatrixf` produces a sphere inscribed in a 1-unit cube, which matches the cube falloff volume. This is geometrically consistent. However if the Axis_knob has non-uniform scale, `gl_sphere` will be sheared into an ellipsoid, which is correct visual behavior. No gap here; documenting for planner awareness.
   - Recommendation: Use `gl_sphere(0.5f)` and `gl_cubef(0.0f, 0.0f, 0.0f, 1.0f)` for unit-scale primitives; Axis matrix handles all positioning and scaling.

2. **Inner wireframe when `_falloff == 1.0`**
   - What we know: `innerScale = 1.0f - _falloff`. At `_falloff = 1.0` (default), `innerScale = 0.0` — the inner wireframe collapses to a point.
   - What's unclear: Whether this degenerate state (zero-scale draw call) causes GL errors or visual noise.
   - Recommendation: Add a guard `if (innerScale > 0.001f)` before the inner draw block to skip it when `_falloff` is at or near 1.0. This is a safe, cheap guard.

---

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | None detected — this is a compiled Nuke plugin (.so); no unit test framework exists in the repo |
| Config file | None |
| Quick run command | `cd /workspace/build && make DeepCPMatte 2>&1 \| tail -20` |
| Full suite command | `cd /workspace/build && make 2>&1 \| tail -30` |

### Phase Requirements -> Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| GLPM-01 | `draw_handle()` contains no call to `_validate()` or `validate()` | static / grep | `grep -n "_validate\|->validate" /workspace/src/DeepCPMatte.cpp` — expect zero matches in `draw_handle` body | N/A (grep check) |
| GLPM-02 | Wireframe geometry draw calls present in `draw_handle()` | static / grep | `grep -n "gl_sphere\|gl_cubef\|glPushMatrix\|glMultMatrixf" /workspace/src/DeepCPMatte.cpp` — expect all four present | N/A (grep check) |
| GLPM-03 | Axis_knob not wrapped in BeginGroup; `build_knob_handles` retained | static / grep | `grep -n "BeginGroup\|build_knob_handles" /workspace/src/DeepCPMatte.cpp` — expect no BeginGroup, one build_knob_handles | N/A (grep check) |
| All | Compiled .so without errors | build | `cd /workspace/build && make DeepCPMatte 2>&1 \| grep -E "error:\|warning:"` | ❌ Wave 0: build must succeed cleanly |

**Note on test automation:** There is no unit test framework for Nuke plugins in this repo. All functional verification (deadlock absence, wireframe appearance, handle interaction) requires manual testing in a running Nuke 16.0v6 instance. The automated checks above are static analysis (grep) plus compilation success. Manual UAT steps should be defined in the plan's verify section.

### Sampling Rate
- **Per task commit:** `cd /workspace/build && make DeepCPMatte 2>&1 | tail -5`
- **Per wave merge:** `cd /workspace/build && make 2>&1 | tail -10`
- **Phase gate:** Clean build + manual UAT in Nuke 16.0v6 before `/gsd:verify-work`

### Wave 0 Gaps
- None — no test file creation needed. Build infrastructure already exists in `/workspace/build`. Validation is: (1) grep checks for absence/presence of specific patterns, (2) successful compilation, (3) manual UAT in Nuke.

---

## Sources

### Primary (HIGH confidence)
- `/usr/local/Nuke16.0v6/include/DDImage/gl.h` — `gl_sphere`, `gl_cubef`, `gl_boxf` signatures; `glColor(unsigned)` helper
- `/usr/local/Nuke16.0v6/include/DDImage/ViewerContext.h` — `VIEWER_2D` enum value, `transform_mode()`, `add_draw_handle()`, `build_knob_handles()`, `node_color()`, `selected_color()`, `draw_lines()`
- `/usr/local/Nuke16.0v6/include/DDImage/Matrix4.h` — `array()` method documented as "correct order to pass to OpenGL"
- `/usr/local/Nuke16.0v6/include/DDImage/Op.h` — `doAnyHandles()`, `HandlesMode`, `build_handles()`, `draw_handle()`, `add_draw_handle(ViewerContext*)`
- `/usr/local/Nuke16.0v6/Documentation/NDKExamples/examples/Draw3D.cpp` — canonical `build_handles`/`draw_handle` pattern; `glPushMatrix`/`glPopMatrix`; 2D guard; `ctx->draw_lines()` pattern
- `/usr/local/Nuke16.0v6/Documentation/NDKExamples/examples/SimpleAxis.cpp` — `glMultMatrixf(local().array())` pattern; `node_color()` / `selected_color()` color switching; `build_knob_handles` before `add_draw_handle`
- `/workspace/src/DeepCPMatte.cpp` — existing code structure; confirms `_axisKnob`, `_shape`, `_falloff`, `_falloffGamma` are populated in `_validate()`; confirms `build_handles`/`draw_handle` stubs exist
- `/workspace/src/DeepCMWrapper.cpp`, `/workspace/src/DeepCWrapper.cpp` — confirms `_validate()` call chain reaches `DeepFilterOp::_validate` -> `input0()->validate()`

### Secondary (MEDIUM confidence)
- None required — primary NDK sources cover all claims.

### Tertiary (LOW confidence)
- None.

---

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH — all libraries confirmed present in Nuke 16.0v6 NDK headers
- Architecture: HIGH — patterns taken directly from NDK example files (Draw3D.cpp, SimpleAxis.cpp)
- Pitfalls: HIGH — deadlock confirmed by call chain trace through actual source files; matrix/scale pitfalls confirmed by API reading
- Validation: HIGH — build command confirmed working (DeepCPMatte.so exists in /workspace/build/src/)

**Research date:** 2026-03-14
**Valid until:** 2026-04-14 (NDK API is stable; Nuke 16.0v6 is pinned in the build system)
