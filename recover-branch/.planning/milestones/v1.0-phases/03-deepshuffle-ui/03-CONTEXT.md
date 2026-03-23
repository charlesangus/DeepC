---
phase: 03-deepshuffle-ui
type: context
created: 2026-03-14
updated: 2026-03-15
status: complete
---

# Phase 3: DeepShuffle UI — Context

**Gathered:** 2026-03-14
**Updated:** 2026-03-15
**Status:** Ready for planning

<domain>
## Phase Boundary

DeepCShuffle exposes per-channel routing for up to 8 channels with a visual matrix UI matching the legacy Nuke Shuffle node, single-input two-ChannelSet routing, and unselected channels pass through unchanged.

</domain>

<decisions>
## Implementation Decisions

### Dual-Node Approach
- New implementation registered as `DeepCShuffle2`; original `DeepCShuffle` preserved unchanged
- Backward compatibility: existing Nuke scripts using the `DeepCShuffle` node must not break
- Two independent `.so` targets in CMake: `DeepCShuffle` and `DeepCShuffle2`

### Matrix Routing Behavior
- **Radio-button semantics per row**: each output channel row always has exactly one source cell checked
- Clicking an unchecked cell in a row moves the selection to that cell
- Clicking an already-checked cell is a no-op — rows cannot be left unchecked
- **Default state**: identity routing — out1.R→in1.R, out1.G→in1.G, etc.
- **Source columns span in1 AND in2**: a single logical row for out1.R spans all in1 columns + const columns + all in2 columns. Enforcement must apply within this entire span.
- **Known bug**: current `onCellToggled` incorrectly enforces single-source across out1 and out2 groups (matching row indices across both matrix sections) instead of within a single output channel's row. Fix needed: use output channel name as the enforcement key, not row index.

### Layout — Match Reference
- **Reference**: `reference/shuffle.png` (Nuke's built-in Shuffle node panel)
- **Column order**: in1 channels | const:0 | const:1 | in2 channels (unified single column set)
- **Row order**: out1 output channels (grouped) then out2 output channels (grouped below)
- Const values (0 and 1) are source columns, visually grouped/distinct between in1 and in2 sections
- **Checked cell visual**: plain highlight (lighter background) — no channel-specific color coding
- Down-arrows below column headers; right-arrows on each row (matching reference)

### ChannelSet Pickers (4 total)
- `in1` ChannelSet: required, defaults to `rgba`
- `in2` ChannelSet: optional, defaults to `none`; when `none`, in2 columns hidden
- `out1` ChannelSet: required, defaults to `rgba`
- `out2` ChannelSet: optional, defaults to `none`; when `none`, out2 row group hidden

### Op Inputs
- Single Op input in the DAG (one connection point, no custom port label)
- in1 and in2 ChannelSets draw from the same connected input node
- `in2` and `out2` are optional — UI collapses their sections when set to `none`

### Passthrough Behavior
- Channels from the input not selected in out1 or out2 pass through to the output unchanged
- Nothing is zeroed

### State Serialization
- `_matrixState` string: CSV of `outName:srcName` pairs (e.g. `"rgba.red:rgba.red,rgba.green:rgba.green"`)
- Stored in `ShuffleMatrixKnob` via `to_script()`/`from_script()` — survives save/load and headless renders
- `_matrixState` parsed in `_validate()` into `std::array<Channel, 8>` output/source pairs

### Claude's Discretion
- Exact separator style between in1/const/in2 column groups
- Row height and button sizing
- Error state handling for invalid `_matrixState` strings

</decisions>

<code_context>
## Existing Code Insights

### Reusable Assets
- `ShuffleMatrixKnob.cpp/.h`: NDK Knob subclass with `to_script`, `from_script`, `store`, `make_widget`, `setChannelSets`, `setValue`
- `ShuffleMatrixWidget.cpp/.h`: Qt6 QWidget with `buildLayout`, `clearLayout`, `syncFromKnob`, `onCellToggled`, WidgetCallback lifecycle
- `DeepCShuffle2.cpp`: DeepFilterOp subclass with 4 ChannelSet knobs, `_matrixState` string, `knob_changed()` push, passthrough-first `doDeepEngine()`
- `DeepCShuffle.cpp`: original 4-channel fixed-pair implementation, preserved for backward compat

### Established Patterns
- `CustomKnob2` macro: embeds a custom Knob subclass in an Op panel
- `WidgetCallback` static dispatch: `kIsVisible` → QTabWidget parent walk, `kUpdateWidgets` → `syncFromKnob`, `kDestroying` → null knob pointer
- `syncFromKnob` rebuild: `clearLayout()` + `buildLayout()` + parse `_matrixState` + `QSignalBlocker` during state sync
- Button `objectName` stores `outName|srcName` token — avoids raw pointer captures in lambda connections
- `findChannel()` (not `getChannel()`) in `_validate()` for channel lookup before channels are requested
- Passthrough-first routing in `doDeepEngine()`: `inChannel = z` default before override lookup

### Integration Points
- `CMakeLists.txt`: `DeepCShuffle2` target compiles `ShuffleMatrixKnob.cpp` and `ShuffleMatrixWidget.cpp`; Qt6 linked only to `DeepCShuffle2`, not `DeepCShuffle`
- `knob_changed()` calls `setChannelSets(in1, in2, out1, out2)` and `updateWidgets()` on the matrix knob when any ChannelSet picker changes

</code_context>

<specifics>
## Specific Ideas

- Reference image at `reference/shuffle.png` — Nuke's built-in Shuffle node panel is the visual target
- Current screenshot at `reference/Screenshot_20260315_140709.png` shows actual implementation state
- Key difference from reference not yet fixed: single-source-per-row enforcement bug (cross-matrix row index confusion)

</specifics>

<deferred>
## Deferred Ideas

- None — discussion stayed within phase scope

</deferred>

---

*Phase: 03-deepshuffle-ui*
*Context gathered: 2026-03-14 | Updated: 2026-03-15*
