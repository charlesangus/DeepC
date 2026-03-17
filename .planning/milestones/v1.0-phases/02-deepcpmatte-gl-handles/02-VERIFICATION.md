---
phase: 02-deepcpmatte-gl-handles
verified: 2026-03-14T14:00:00Z
status: human_needed
score: 6/6 must-haves verified (automated); 1 truth requires human UAT
re_verification: false
human_verification:
  - test: "Open DeepCPMatte in Nuke 16.0v6 with an active 3D viewer. Connect a deep input carrying position data. Observe the 3D viewport."
    expected: "A wireframe sphere or cube appears at the Axis knob position and scale. No hang or deadlock occurs when the viewer opens."
    why_human: "GL rendering output cannot be verified by grep or build check — requires a running Nuke instance."
  - test: "In the same 3D viewer session, click and drag the wireframe handle."
    expected: "Dragging activates the Axis gizmo (translate/rotate/scale arrows) and repositions the selection volume in 3D space."
    why_human: "Interactive handle drag behavior requires a live Nuke session."
  - test: "Switch the viewer to 2D mode (F1 or viewer mode toggle). Confirm no wireframe appears."
    expected: "Wireframe is invisible in 2D viewer mode."
    why_human: "Viewer mode rendering cannot be observed without a running Nuke instance."
  - test: "Open the DeepCPMatte properties panel. Look for the Axis knob."
    expected: "Axis_knob appears at the top level of the panel with no collapsible group wrapping it."
    why_human: "Knob layout in the properties panel requires visual inspection in Nuke UI."
---

# Phase 02: DeepCPMatte GL Handles — Verification Report

**Phase Goal:** DeepCPMatte displays a wireframe sphere or cube in the Nuke 3D viewport representing the current selection volume, the handle is draggable to reposition the volume, and the GL thread no longer calls `_validate()`.
**Verified:** 2026-03-14T14:00:00Z
**Status:** human_needed — all automated checks passed; 4 items require UAT in a live Nuke session
**Re-verification:** No — initial verification

---

## Goal Achievement

### Observable Truths

| # | Truth | Status | Evidence |
|---|-------|--------|----------|
| 1 | Opening DeepCPMatte with a 3D viewer active does not hang or deadlock | ? HUMAN NEEDED | No `_validate()` call exists in `draw_handle()` (line 190 `validate` is in `knob_changed()`, cook thread — safe). Deadlock is structurally eliminated but requires live Nuke session to confirm no hang. |
| 2 | A wireframe outer boundary (sphere or cube) appears in the 3D viewport at the Axis knob position and scale | ? HUMAN NEEDED | `gl_sphere(0.5f)` and `gl_cubef(0,0,0,1.0f)` calls present in `draw_handle()` (lines 155, 157). `glMultMatrixf(_axisKnob.array())` places geometry in Axis local space (line 151). Code is correct; actual GL output requires Nuke. |
| 3 | A wireframe inner boundary scaled by (1 - _falloff) appears inside the outer boundary (skipped when falloff >= 0.999) | ✓ VERIFIED | `innerScale = 1.0f - _falloff` computed; `glScalef(innerScale, innerScale, innerScale)` applied before inner draw; `if (innerScale > 0.001f)` guard present (lines 160–168). Functionally equivalent to `_falloff >= 0.999f` skip condition. |
| 4 | Wireframe only appears in 3D viewer mode, not in the 2D viewer | ? HUMAN NEEDED | `build_handles()` guard is `if (ctx->transform_mode() == VIEWER_2D) return;` (lines 138–139) — correct direction confirmed by grep. Verified statically; runtime behavior requires Nuke. |
| 5 | Dragging the wireframe activates the Axis gizmo and repositions the selection volume | ? HUMAN NEEDED | `build_knob_handles(ctx)` is called unconditionally (line 137), registering Axis gizmo arrows. `add_draw_handle(ctx)` is called in 3D mode (line 140). Wiring is present; interactive behavior requires live Nuke. |
| 6 | The Axis_knob is visible at the top level of the properties panel without a BeginGroup/EndGroup wrapper | ✓ VERIFIED | `grep BeginGroup` → 0 matches. `grep EndGroup` → 0 matches. `Axis_knob(f, &_axisKnob, "selection")` registered bare at line 177 in `custom_knobs()`. |

**Score:** 6/6 automated structural checks pass. 4 truths additionally require human UAT to confirm runtime behavior.

---

### Required Artifacts

| Artifact | Expected | Status | Details |
|----------|----------|--------|---------|
| `src/DeepCPMatte.cpp` | All GL handle changes — deadlock fix, wireframe geometry, knob layout | ✓ VERIFIED | File exists, 223 lines, substantive. Contains all required patterns: `gl_sphere`, `gl_cubef`, `glPushMatrix`, `glMultMatrixf`, correct `== VIEWER_2D` guard, bare `Axis_knob` registration. Clean build confirmed. |

---

### Key Link Verification

| From | To | Via | Status | Details |
|------|----|-----|--------|---------|
| `build_handles()` | `add_draw_handle(ctx)` | `== VIEWER_2D` guard — skip in 2D, fire in 3D | ✓ WIRED | Line 138: `if (ctx->transform_mode() == VIEWER_2D)` correctly guards `add_draw_handle(ctx)` at line 140. Guard direction matches spec. |
| `draw_handle()` | `gl_sphere` / `gl_cubef` | `glPushMatrix` / `glMultMatrixf(_axisKnob.array())` — Axis local space | ✓ WIRED | Lines 150–170: `glPushMatrix()` → `glMultMatrixf(_axisKnob.array())` → shape calls → `glPopMatrix()`. All four required calls present. |
| `custom_knobs()` | `Axis_knob` | Direct registration without BeginGroup/EndGroup wrapper | ✓ WIRED | Line 177: bare `Axis_knob(f, &_axisKnob, "selection")`. No `BeginGroup` or `EndGroup` anywhere in file. |

---

### Requirements Coverage

| Requirement | Source Plan | Description | Status | Evidence |
|-------------|-------------|-------------|--------|---------|
| GLPM-01 | 02-01-PLAN.md | `draw_handle()` no longer calls `_validate()` on the GL thread | ✓ SATISFIED | `grep "_validate\|->validate" src/DeepCPMatte.cpp` returns only line 190 (`input0()->validate(true)` in `knob_changed()`) — the cook-thread call. No `_validate` or `->validate` in `draw_handle()` body (lines 143–171). |
| GLPM-02 | 02-01-PLAN.md | DeepCPMatte draws a wireframe sphere or cube in the 3D viewport | ✓ SATISFIED (code); ? HUMAN (runtime) | `gl_sphere(0.5f)` at lines 155 and 165; `gl_cubef(0,0,0,1.0f)` at lines 157 and 167. Geometry correctly placed via `glMultMatrixf(_axisKnob.array())`. Clean build confirms no compile-time issues. |
| GLPM-03 | 02-01-PLAN.md | Wireframe handle is interactive — click and drag to reposition | ✓ SATISFIED (code); ? HUMAN (runtime) | `build_knob_handles(ctx)` unconditional (Axis gizmo registered), `add_draw_handle(ctx)` fires in 3D mode. Axis matrix used for draw. Interactive wiring is structurally complete; requires UAT. |

No orphaned requirements found. All three Phase 2 requirements (GLPM-01, GLPM-02, GLPM-03) are claimed by 02-01-PLAN.md and covered by implementation. REQUIREMENTS.md traceability table marks all three Complete.

---

### Anti-Patterns Found

| File | Line | Pattern | Severity | Impact |
|------|------|---------|----------|--------|
| `src/DeepCPMatte.cpp` | 66 | `TODO: probably better to work with a pointer and length` in `wrappedPerSample` docblock | ℹ️ Info | Pre-existing comment describing SWEEP-07 (a separate pending requirement). Not introduced by this phase. Does not affect GL handle goal. |

No blocker or warning anti-patterns found. No stub returns, no placeholder GL draws, no hardcoded `glVertex` stubs.

---

### Commit Verification

Both commits documented in SUMMARY exist and are correctly scoped:

| Commit | Description | Files | Lines +/- |
|--------|-------------|-------|-----------|
| `314c925` | Fix `build_handles()` 2D guard and remove Axis_knob BeginGroup wrapper | `src/DeepCPMatte.cpp` | +1 / -3 |
| `d89b516` | Replace `draw_handle()` — remove `_validate()` deadlock, add wireframe geometry | `src/DeepCPMatte.cpp` | +23 / -4 |

---

### Human Verification Required

**All automated structural checks pass.** The following require UAT in a live Nuke 16.0v6 session:

#### 1. No Deadlock / Hang on 3D Viewer Open

**Test:** Open DeepCPMatte with a 3D viewer active and a connected deep input. Switch to the 3D viewer.
**Expected:** Nuke remains responsive — no hang, no deadlock, no spinning cursor.
**Why human:** GL thread deadlock can only be observed at runtime. The structural cause (the `_validate()` call) has been eliminated, but confirmation requires a live session.

#### 2. Wireframe Visible in 3D Viewport

**Test:** In the 3D viewer with DeepCPMatte selected, observe the viewport.
**Expected:** A wireframe sphere or cube appears centered at the Axis knob position. If falloff < 1.0, a smaller inner wireframe is also visible inside the outer one. Wire color matches the node chip color.
**Why human:** GL rendering output cannot be verified by source analysis alone.

#### 3. Wireframe Absent in 2D Viewer

**Test:** Switch the viewer to 2D mode (press F1 or toggle viewer mode).
**Expected:** No wireframe shape appears in the 2D viewer.
**Why human:** Viewer mode rendering requires runtime observation.

#### 4. Interactive Handle — Drag to Reposition

**Test:** In the 3D viewer, click on the wireframe and drag.
**Expected:** The Axis gizmo (translate/rotate/scale arrows) activates. Dragging moves or rotates the selection volume. The wireframe follows the new position.
**Why human:** Handle interactivity requires a live Nuke session with mouse input.

#### 5. Axis Knob Visible at Top Level of Properties Panel

**Test:** Open the DeepCPMatte properties panel in Nuke.
**Expected:** The Axis knob appears at the top level of the panel, not nested inside a collapsible "Position" group.
**Why human:** Knob panel layout requires visual inspection in the Nuke UI.

---

### Gaps Summary

No functional gaps found. All three phase requirements are structurally implemented and connected:

- GLPM-01: `_validate()` removed from `draw_handle()` — deadlock source eliminated
- GLPM-02: Wireframe geometry (`gl_sphere`/`gl_cubef`) placed in Axis local space via `glMultMatrixf(_axisKnob.array())` — inner/outer boundary both implemented with correct guard
- GLPM-03: `build_handles()` guard corrected (`== VIEWER_2D`), `build_knob_handles` unconditional, `Axis_knob` at top level — interactive handle wiring complete

Status is `human_needed` — not `passed` — because GL rendering, handle interactivity, and properties panel layout cannot be verified without running Nuke. The code is correct and complete by static analysis.

---

_Verified: 2026-03-14T14:00:00Z_
_Verifier: Claude (gsd-verifier)_
