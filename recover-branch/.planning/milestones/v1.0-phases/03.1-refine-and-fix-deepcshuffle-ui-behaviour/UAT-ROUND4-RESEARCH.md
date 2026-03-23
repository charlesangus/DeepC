# UAT Round 4 — Bug Analysis & Agreed Approaches

**Session date:** 2026-03-16
**Status:** Research complete, ready to implement. Context cleared before implementation.

---

## Context

Phase 03.1 has gone through 3+ rounds of UAT fixes. The following bugs survive. Root causes
are now understood from direct code review of the current source. This document is the
authoritative handoff for the next implementation session.

Current source files reviewed:
- `src/ShuffleMatrixWidget.cpp` (854 lines)
- `src/ShuffleMatrixWidget.h` (165 lines)
- `src/ShuffleMatrixKnob.cpp` (173 lines)
- `src/ShuffleMatrixKnob.h` (175 lines)
- `src/DeepCShuffle2.cpp` (338 lines)

---

## Bug 1 — Arrow Labels Too Small

**Root cause:** `makeArrowLabel` creates a QLabel with `setFixedHeight(22)` and `setPointSize(18)`.
At 96dpi, 18pt ≈ 24px — larger than the 22px height constraint, so the glyph is clipped.
Additionally, there is no `setFixedWidth`, so the label may be wider than 22px with a bold font,
potentially causing column-width inconsistency (may contribute to Bug 4).

**Agreed fix:** Replace `makeArrowLabel` QLabel approach with a custom `ArrowLabel` widget
that uses `setFixedSize(22, 22)` (matching ChannelButton exactly) and draws the arrow via
`QPainter` in a `paintEvent` override. Both the down-arrow (column headers) and right-arrow
(row labels) variants should use the same custom class with a direction parameter.
The painted arrow should be centered and sized to fill the cell.

**Note:** The right-arrow on output row labels (the `⇒` before the channel name in the
output QLabel) is a separate widget — also review whether that needs the same treatment.

---

## Bug 2 — Routing State Cleared When ChannelSet Changes

**Root cause:** State is stored by full NDK channel name:
```
"rgba.red:in1:rgba.red,rgba.green:in1:rgba.green,..."
```
When in1 changes from "rgba" to "diffuse", the new column names are "diffuse.red" etc.
The state restore loop looks for `routingMap["rgba.red"]` and compares against
`"in1:diffuse.red"` — no match — all buttons unchecked.

**User decision:** **Store by position (row index, column index), not channel names.**

**New state format:**
```
"out1:0:in1:2,out1:1:in1:1,out2:0:const:0,..."
```
Where:
- First field: output group id ("out1" or "out2")
- Second field: row index within that output group (0-based)
- Third field: source group ("in1", "in2", or "const")
- Fourth field: column index within that source group (0-based), OR "0"/"1" for const

**Implications for the engine (`_validate`):**
The engine currently parses state strings into channel names directly. With positional state,
`_validate` must resolve positions to channel names by looking up current ChannelSets:
- out1 row N → `out1ChannelSet` channel at index N
- in1 col N → `in1ChannelSet` channel at index N
- const 0 → 0.0f, const 1 → 1.0f

`_validate` already has access to `_in1ChannelSet`, `_out1ChannelSet`, etc. (the Op members).
Add a helper: `channelAtIndex(ChannelSet, int index) → Channel`.

**State restore in widget:** Button objectName format should change to include row/col indices,
OR the restore loop should match by computing the button's row/col position from its
position in `_toggleButtons` or from its grid layout position.

Simplest approach for objectName: keep current 4-part format but replace channel names with indices:
```
"out1|0|in1|2"   (output group | output row idx | input group | source col idx)
```

**Backward compat:** Old scripts with name-based state will not restore correctly. This is
acceptable — it's an unreleased plugin. Add a migration path in `from_script` that detects
old format (contains "rgba." or similar) and silently clears state.

---

## Bug 3 — ChannelSet Picker Requires Two Interactions

**Root cause:** UNCONFIRMED. Hypothesis: `set_text()` on a ChannelSet knob does not
immediately update the Op's C++ backing member (`_in1ChannelSet`). When the picker's
deferred lambda calls `op->knob_changed(knob)`, `DeepCShuffle2::knob_changed` then calls
`setChannelSets(_in1ChannelSet, ...)` — but `_in1ChannelSet` still has the OLD value.
The widget rebuilds with stale channel names. Only after Nuke's own update cycle (triggered
by the next user interaction) does `_in1ChannelSet` get the new value.

**Agreed diagnostic approach:** Write a minimal test node `DeepCTest` with:
- One `ChannelSet_knob` named "layer"
- One `String_knob` named "result"
- One `Bool_knob` named "flag"

In `knob_changed`: if `k->is("layer")`, flip the bool and write the resolved ChannelSet
channel names to the string knob.

This will definitively show whether the ChannelSet knob backing variable is updated
synchronously or requires a Nuke update cycle.

**Implementation note:** `DeepCTest` should live in `src/DeepCTest.cpp`, registered as
`"DeepCTest"`, added to `CMakeLists.txt` as a separate target. Remove after diagnosis.

---

## Bug 4 — Gap Between Green and Blue Columns

**Observed:** User sees: `red  green  [small gap]  blue  alpha  [large gap]  0  1  [small gap]  red  green  [small gap]  blue  alpha`

**Analysis:**
- The [large gap] between `alpha` and `0` is the intended `sepBeforeConstCol` separator (8px). ✓
- The [small gap] between `1` and in2's `red` — this should ALSO be a large gap (sepAfterConstCol).
  The separator column IS present at `in1Count+3`, but may not be visible because the in2Picker
  QComboBox spans from `sepBeforeConstCol` through all in2 columns, potentially absorbing the
  separator visually.
- The [small gap] between `green` and `blue` WITHIN the in1 group — this should NOT exist.
  Columns 0,1,2,3 are placed with `setColumnStretch(col, 0)` and ChannelButton fixed at 22×22.
  **Most likely cause:** `makeArrowLabel` (Bug 1) creates QLabels with NO fixed width and a bold
  18pt font. The ⇓ glyph at 18pt bold may be wider in some columns than others depending on
  font rendering, OR one of the arrow labels has a text that's wider. Alternatively, the
  `in1Header` label ("in 1") spanning in1Count columns may be forcing a minimum width that
  distributes unevenly. **Fixing Bug 1 (ArrowLabel with setFixedSize(22,22)) will likely
  also fix this gap.**

**Additional fix needed:** The separator after const:1 (`sepAfterConstCol`) should be clearly
visible as a gap. Currently the in2Picker spans `sepBeforeConstCol` through the in2 columns,
which visually absorbs the separator. Consider starting the in2Picker at `in2StartCol` (not
`sepBeforeConstCol`) so the separator columns remain visually between the const group and in2.

---

## Bug 5 — Disabled Buttons Invisible (Not Greyed)

**Root cause:** `paintEvent` renders disabled buttons as `QColor(55, 55, 55)` — near-black.
Nuke's panel background is approximately `#2b2b2b` (also very dark). The buttons render but
are essentially invisible against the background.

**Fix:** Change disabled fill color to `QColor(85, 85, 85)` and disabled border to
`QColor(110, 110, 110)`. This makes the button visibly present (as a faintly-visible square)
without appearing active.

---

## Implementation Order

1. **Bug 1 + Bug 4 together:** Replace `makeArrowLabel` with `ArrowLabel` custom widget
   using `setFixedSize(22, 22)`. This may also fix the green/blue gap. Also start in2Picker
   at `in2StartCol` instead of `sepBeforeConstCol`.

2. **Bug 5:** Change disabled fill color in `ChannelButton::paintEvent`.

3. **Bug 3:** Build `DeepCTest` diagnostic node. Run in Nuke to confirm or refute the
   `set_text()` hypothesis. Fix based on findings.

4. **Bug 2:** Redesign state format to positional indexing. This is the largest change —
   touches `ShuffleMatrixWidget.cpp` (buildLayout objectNames, state restore),
   `ShuffleMatrixKnob.cpp` (from_script migration), and `DeepCShuffle2.cpp` (_validate parsing).

---

## Current Git State

All prior fix attempts are committed. The code compiles. Last round of commits:
- `4146538`: fix(03.1): UAT round 3 — six ShuffleMatrixWidget bugs fixed
- `2fd381c`: docs(03.1): complete UAT round 3 bug fixes

The next commit series should start from a clean state on top of these.

---

## Key Files

| File | Purpose |
|------|---------|
| `src/ShuffleMatrixWidget.cpp` | All layout, state restore, button creation, pickers |
| `src/ShuffleMatrixWidget.h` | ChannelButton, widget class decl |
| `src/ShuffleMatrixKnob.cpp` | make_widget, setValue, syncWidgetNow, from_script |
| `src/ShuffleMatrixKnob.h` | Knob class decl, private state |
| `src/DeepCShuffle2.cpp` | knobs(), knob_changed(), _validate() engine |
| `CMakeLists.txt` | Add DeepCTest target (temporary) |
