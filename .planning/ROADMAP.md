# Roadmap: DeepC

## Overview

The DeepC milestone adds interactive GL handles to DeepCPMatte, expands the DeepShuffle channel routing UI, exposes 4D noise in DeepCPNoise, vendors the DeepThinner optimizer plugin, and resolves all known codebase bugs and NDK API debt. Work begins with a mandatory codebase sweep that fixes confirmed bugs and resolves the DeepCBlink fate before any other files are touched. The remaining four feature tracks are architecturally independent of each other and execute in sequence after the sweep.

## Phases

**Phase Numbering:**
- Integer phases (1, 2, 3): Planned milestone work
- Decimal phases (2.1, 2.2): Urgent insertions (marked with INSERTED)

Decimal phases appear between their surrounding integers in numeric order.

- [x] **Phase 1: Codebase Sweep** - Fix all confirmed bugs, modernize NDK APIs, resolve DeepCBlink fate (completed 2026-03-13)
- [x] **Phase 2: DeepCPMatte GL Handles** - Interactive wireframe viewport overlay and drag-to-reposition handle for the selection volume (completed 2026-03-14)
- [x] **Phase 3: DeepShuffle UI** - Expand channel routing from 4 to 8 channels with labeled ports and layer-level routing (completed 2026-03-15)
- [ ] **Phase 4: DeepCPNoise 4D** - Expose 4D noise option in the UI wired to existing FastNoise 4D methods
- [ ] **Phase 5: DeepThinner Vendor** - Confirm license compatibility, integrate source into CMake build, register in toolbar menu

## Phase Details

### Phase 1: Codebase Sweep
**Goal**: The codebase is free of all confirmed bugs, DeepCWrapper/DeepCMWrapper are removed from the Nuke node menu, all deprecated Nuke 16+ NDK APIs are confirmed non-present and the one concrete modernization (DeepCWorld inverse matrix cache) is applied, and DeepCBlink is removed from the build. (Note: SWEEP-07 perSampleData redesign and SWEEP-08 grade coefficient extraction were dropped after analysis — see CONTEXT.md.)
**Depends on**: Nothing (first phase)
**Requirements**: SWEEP-01, SWEEP-02, SWEEP-03, SWEEP-04, SWEEP-05, SWEEP-06, SWEEP-09, SWEEP-10
**Success Criteria** (what must be TRUE):
  1. DeepCGrade in reverse mode produces correct output — gamma is applied to the remapped value, not discarded
  2. DeepCKeymix correctly uses `aPixel.channels()` for the A-side containment check, not `bPixel.channels()`
  3. DeepCSaturation and DeepCHueShift produce zero output (not NaN) for fully transparent samples during unpremult
  4. Loading any DeepC plugin in Nuke does not expose DeepCWrapper or DeepCMWrapper as selectable nodes in the node menu
  5. DeepCBlink is absent from the compiled build and toolbar menu
**Plans**: 5 plans

Plans:
- [ ] 01-01-PLAN.md — Fix DeepCGrade gamma, DeepCKeymix channel set, DeepCSaturation/HueShift zero-alpha guard
- [ ] 01-02-PLAN.md — Fix DeepCConstant comma expression, DeepCID loop variable
- [ ] 01-03-PLAN.md — Remove DeepCWrapper/DeepCMWrapper node registrations and Class(); clean dead code
- [ ] 01-04-PLAN.md — Delete DeepCBlink source and CMake references (with build verification checkpoint)
- [ ] 01-05-PLAN.md — Cache DeepCWorld inverse matrix; fix switch UB; correct batchInstall.sh comment

### Phase 2: DeepCPMatte GL Handles
**Goal**: DeepCPMatte displays a wireframe sphere or cube in the Nuke 3D viewport representing the current selection volume, the handle is draggable to reposition the volume, and the GL thread no longer calls `_validate()`.
**Depends on**: Phase 1
**Requirements**: GLPM-01, GLPM-02, GLPM-03
**Success Criteria** (what must be TRUE):
  1. Opening DeepCPMatte in Nuke with the 3D viewer active does not deadlock or hang
  2. A wireframe shape (sphere or cube) appears in the 3D viewport at the position and scale of the Axis knob transform
  3. Clicking and dragging the wireframe handle repositions the selection volume and the node reprocesses with the updated transform
**Plans**: 1 plan

Plans:
- [ ] 02-01-PLAN.md — Fix deadlock, add wireframe GL handles, expose Axis_knob at top level

### Phase 3: DeepShuffle UI
**Goal**: DeepCShuffle exposes per-channel routing for up to 8 channels with labeled input and output ports, clean row layout, and layer-level routing via a ChannelSet knob.
**Depends on**: Phase 1
**Requirements**: SHUF-01, SHUF-02, SHUF-03, SHUF-04
**Success Criteria** (what must be TRUE):
  1. DeepCShuffle shows per-channel routing knobs in an in→out row layout matching the legacy Shuffle node, for up to 8 channel pairs
  2. Input and output ports in the node panel are labeled with the channel names assigned to them
  3. Users can route a complete layer at once using the layer-level ChannelSet knob without configuring individual channels
**Plans**: 4 plans

Plans:
- [ ] 03-01-PLAN.md — Install Qt6 dev headers; add find_package(Qt6) + AUTOMOC to CMakeLists.txt; verify baseline build
- [ ] 03-02-PLAN.md — Write ShuffleMatrixKnob.h and ShuffleMatrixWidget.h interface contracts
- [ ] 03-03-PLAN.md — Implement ShuffleMatrixKnob.cpp and ShuffleMatrixWidget.cpp
- [ ] 03-04-PLAN.md — Rewrite DeepCShuffle.cpp with new data model and knob wiring; UAT checkpoint

### Phase 03.1: refine and fix deepcshuffle ui/behaviour (INSERTED)

**Goal:** DeepCShuffle2 has full visual and behavioral parity with the reference Shuffle2 node: colored X-mark channel buttons, embedded ChannelSet pickers, radio enforcement scoped to each output group, const:0/1 source columns, identity routing on first open, and disabled-not-hidden behavior for none ChannelSets.
**Requirements**: none (fix phase — all SHUF requirements already marked complete)
**Depends on:** Phase 3
**Plans:** 1/4 plans executed

Plans:
- [ ] 03.1-01-PLAN.md — Fix radio no-op guard, outGroup radio scope, add const:0/1 columns, identity routing init
- [ ] 03.1-02-PLAN.md — Disabled-not-hidden for none ChannelSets; fix make_widget timing for single-step ChannelSet update
- [ ] 03.1-03-PLAN.md — ChannelButton subclass with colored X-mark paintEvent; down/right arrow decorators
- [ ] 03.1-04-PLAN.md — Embed QComboBox pickers in widget; hide NDK knobs with INVISIBLE; UAT checkpoint

### Phase 4: DeepCPNoise 4D
**Goal**: DeepCPNoise exposes a 4D noise dimension option in the UI for all supported noise types, correctly enabling the evolution knob only for Simplex noise, with the noise type comparison replaced by a named constant.
**Depends on**: Phase 1
**Requirements**: NOIS-01
**Success Criteria** (what must be TRUE):
  1. A 4D noise option is visible and selectable in the DeepCPNoise UI for all five noise types (Simplex, Perlin, Cellular, Cubic, Value)
  2. The `noise_evolution` knob is enabled only when Simplex is selected and is grayed out for all other noise types
**Plans**: TBD

### Phase 5: DeepThinner Vendor
**Goal**: DeepThinner is confirmed license-compatible with GPL-3.0, its source is compiled as a Nuke plugin via CMake, and it appears in the DeepC toolbar submenu in Nuke.
**Depends on**: Phase 1
**Requirements**: THIN-01, THIN-02, THIN-03
**Success Criteria** (what must be TRUE):
  1. A written license compatibility confirmation exists before any DeepThinner source is added to the repository
  2. Running the CMake build produces a DeepThinner `.so` artifact with no name collisions against existing DeepC nodes
  3. DeepThinner is accessible from the DeepC submenu in the Nuke toolbar and can be placed on the node graph
**Plans**: TBD

## Progress

**Execution Order:**
Phases execute in numeric order: 1 → 2 → 3 → 3.1 → 4 → 5

| Phase | Plans Complete | Status | Completed |
|-------|----------------|--------|-----------|
| 1. Codebase Sweep | 5/5 | Complete   | 2026-03-13 |
| 2. DeepCPMatte GL Handles | 1/1 | Complete   | 2026-03-14 |
| 3. DeepShuffle UI | 4/4 | Complete   | 2026-03-15 |
| 3.1. Refine DeepCShuffle UI | 0/4 | Not started | - |
| 4. DeepCPNoise 4D | 0/TBD | Not started | - |
| 5. DeepThinner Vendor | 0/TBD | Not started | - |
