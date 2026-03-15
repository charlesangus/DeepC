---
phase: 03-deepshuffle-ui
type: context
created: 2026-03-14
status: complete
---

# Phase 3: DeepShuffle UI — Context

## Phase Goal

DeepCShuffle exposes per-channel routing for up to 8 channels with a visual matrix UI matching the legacy Nuke Shuffle node, single-input two-ChannelSet routing, and unselected channels pass through unchanged.

## What We Discussed

Four gray areas were identified and resolved through discussion:

### 1. 8-Channel Layout & Routing UI

**Decision**: C++ custom `DD::Image::Knob` subclass with embedded Qt widget — a visual matrix grid matching the legacy Shuffle node layout.

- Rows = output channels
- Columns = source channels from in1 and/or in2 ChannelSets
- Toggle buttons at each cell (click to route source → destination)
- Selected state serialized to a hidden string knob for script/render persistence
- This reproduces the visual matrix from the legacy Shuffle node rather than falling back to dropdown rows

### 2. Op Inputs

**Decision**: Single Op input in the DAG (one connection point, unlabeled, no custom port label).

Within the panel, the user selects one or two `ChannelSet`s from that single input stream (e.g. `rgba` as in1, `depth` as in2 — both from the same upstream node). The matrix then routes between the selected source channels and output channel destinations.

- `in2` ChannelSet picker is optional (can be left as `none`)
- Both in1 and in2 draw from the same connected input node

### 3. Port Labeling (SHUF-02)

**Decision**: No custom port labels. Single unlabeled input port on the DAG node tile — standard Nuke default behavior. No changes to port label display.

Note: SHUF-02 as written refers to "input and output ports labeled with channel names" — based on discussion this requirement is satisfied by the in-panel row labels on the matrix (output channel names shown on the right side of the matrix), not by customizing DAG noodle port labels.

### 4. Passthrough Behavior

**Decision**: Channels from the input that are not selected in in1 or in2 pass through to the output unchanged. Nothing is zeroed.

## Architecture Notes

### Custom Knob Implementation

The matrix widget is a `DD::Image::Knob` subclass:
- `make_widget(QWidget* parent)` returns a custom `QWidget` containing the matrix grid
- State stored as a flat string (e.g. `"0:in1.r,1:in1.g,2:in1.b,3:in1.a"`) in `to_script()` / `from_script()`
- `knob_changed()` in the Op updates the ChannelSet picker visibility and matrix column headers when in1/in2 ChannelSet changes
- Matrix columns rebuild dynamically when ChannelSet selection changes

### ChannelSet Pickers

- `in1`: required `Input_ChannelSet_knob` (defaults to `rgba`)
- `in2`: optional `Input_ChannelSet_knob` (defaults to `none`; when `none`, in2 columns hidden from matrix)

### Channel Count

Up to 8 output channels (SHUF-03). Matrix has 8 rows maximum; number of active rows matches the number of channels in the selected output ChannelSet.

### Passthrough

`processSample()` copies all input channels to output first, then applies matrix routing on top. Channels not covered by in1 or in2 selection are untouched.

## Requirements Addressed

| Requirement | How |
|-------------|-----|
| SHUF-01 | C++ Qt custom knob matrix grid matching legacy Shuffle layout |
| SHUF-02 | In-panel row/column labels on matrix; no DAG port label changes |
| SHUF-03 | Matrix supports up to 8 output channel rows |
| SHUF-04 | Per-input `Input_ChannelSet_knob` for layer-level routing |

## Key Constraints

- Matrix widget state **must** survive script save/load and headless renders — no Python dependency
- `in2` ChannelSet is optional; UI must handle `none` gracefully (collapse in2 columns)
- Single Op input — do not add a second `Op*` input
- Unselected channels pass through; never zero output channels that weren't routed
