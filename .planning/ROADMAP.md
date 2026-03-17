# Roadmap: DeepC

## Overview

The DeepC milestone adds interactive GL handles to DeepCPMatte, expands the DeepShuffle channel routing UI, exposes 4D noise in DeepCPNoise, vendors the DeepThinner optimizer plugin, and resolves all known codebase bugs and NDK API debt. Work begins with a mandatory codebase sweep that fixes confirmed bugs and resolves the DeepCBlink fate before any other files are touched. The remaining four feature tracks are architecturally independent of each other and execute in sequence after the sweep.

## Phases

**Phase Numbering:**
- Integer phases (1, 2, 3): Planned milestone work
- Decimal phases (2.1, 2.2): Urgent insertions (marked with INSERTED)

Decimal phases appear between their surrounding integers in numeric order.

- [x] **Phase 1: Codebase Sweep** - Fix all confirmed bugs, modernize NDK APIs, resolve DeepCBlink fate (completed 2005-03-13)
- [x] **Phase 2: DeepCPMatte GL Handles** - Interactive wireframe viewport overlay and drag-to-reposition handle for the selection volume (completed 2005-03-14)
- [x] **Phase 3: DeepShuffle UI** - Expand channel routing from 4 to 8 channels with labeled ports and layer-level routing (completed 2005-03-15)
- [x] **Phase 4: DeepCPNoise 4D** - Expose 4D noise option in the UI wired to existing FastNoise 4D methods (completed 2005-03-17)
- [ ] **Phase 5: Release Cleanup** - Add DeepCShuffle2 icon, promote DeepCShuffle2 as the menu entry (DeepCShuffle.so kept for compat, hidden from menu), update REQUIREMENTS.md traceability for dropped SWEEP-07/08

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
**Plans:** 4/4 plans complete

Plans:
- [x] 03.1-01-PLAN.md — Fix radio no-op guard, outGroup radio scope, add const:0/1 columns, identity routing init
- [x] 03.1-02-PLAN.md — Disabled-not-hidden for none ChannelSets; fix make_widget timing for single-step ChannelSet update
- [x] 03.1-03-PLAN.md — ChannelButton subclass with colored X-mark paintEvent; down/right arrow decorators
- [x] 03.1-04-PLAN.md — Embed QComboBox pickers in widget (QTimer deferred); INVISIBLE NDK knobs; UAT all fixed

### Phase 4: DeepCPNoise 4D
**Goal**: DeepCPNoise wires the `noise_evolution` float knob as the w dimension to `GetNoise(x,y,z,w)` unconditionally for all five noise types, with FastNoise fully implementing 4D algorithms for Value, Perlin, Cubic, and Cellular (Simplex already had 4D support). The `if (_noiseType==0)` magic index branch is removed.
**Depends on**: Phase 1
**Requirements**: NOIS-01
**Success Criteria** (what must be TRUE):
  1. All five noise types (Simplex, Perlin, Cellular, Cubic, Value) produce non-black output when noise_evolution is non-zero
  2. `noise_evolution = 0` produces stable output for all types (w=0 is a valid neutral value)
  3. The `if (_noiseType==0)` conditional is absent from DeepCPNoise.cpp
  4. No tooltip in DeepCPNoise.cpp implies Simplex is the only noise type with 4D support
**Plans**: 3 plans

Plans:
- [ ] 04-01-PLAN.md — Add all 4D declarations to FastNoise.h; implement 4D Value and Perlin (base + fractals)
- [ ] 04-02-PLAN.md — Implement 4D Cubic and Cellular (CELL_4D tables, CUBIC_4D_BOUNDING, algorithm bodies)
- [ ] 04-03-PLAN.md — Extend GetNoise(x,y,z,w) dispatch; update DeepCPNoise call site + tooltips; UAT checkpoint

### Phase 5: Release Cleanup
**Goal**: DeepCShuffle2 has an icon in the Nuke toolbar menu, the "DeepCShuffle" menu entry now creates a DeepCShuffle2 node (DeepCShuffle.so is still compiled and loadable for backward compatibility but does not appear in the menu), and REQUIREMENTS.md traceability is updated to reflect that SWEEP-07 and SWEEP-08 were dropped (not deferred).
**Depends on**: Phase 4
**Requirements**: none (cleanup — no new feature requirements)
**Success Criteria** (what must be TRUE):
  1. `icons/DeepCShuffle2.png` exists and is installed by CMake
  2. The Nuke toolbar menu shows one DeepCShuffle entry that creates a DeepCShuffle2 node; no separate DeepCShuffle entry appears
  3. Loading both .so files in Nuke does not produce duplicate or broken menu entries
  4. REQUIREMENTS.md traceability table shows SWEEP-07 and SWEEP-08 as Dropped, not Pending
**Plans**: 1 plan
