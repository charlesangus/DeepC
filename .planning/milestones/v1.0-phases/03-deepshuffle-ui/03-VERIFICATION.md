---
phase: 03-deepshuffle-ui
verified: 2026-03-15T00:00:00Z
status: human_needed
score: 10/10 must-haves verified (automated)
re_verification: false
human_verification:
  - test: "Open DeepCShuffle2 node panel in Nuke and confirm the ShuffleMatrixKnob matrix grid widget renders"
    expected: "Two ChannelSet pickers (in1 defaulting to rgba, in2 defaulting to none) appear above a toggle-button grid"
    why_human: "Widget rendering inside Nuke's Qt panel cannot be verified by static analysis or build output"
  - test: "Change the in1 ChannelSet picker to a different layer (e.g. depth)"
    expected: "Matrix column headers rebuild to show depth channel names"
    why_human: "Dynamic knob_changed -> setChannelSets -> updateWidgets -> syncFromKnob chain requires a live Nuke session"
  - test: "Toggle a routing cell (e.g. rgba.red -> depth.Z), then save script and reload"
    expected: "The routing assignment is restored after reload — toggle button is checked"
    why_human: "Script serialization round-trip requires a running Nuke instance to write and reload a .nk file"
  - test: "Connect a node with multiple layers upstream; route only rgba channels; inspect output"
    expected: "Depth and other non-routed layers are present and unmodified in the output (passthrough, not zeroed)"
    why_human: "Passthrough-first behavior in doDeepEngine requires live deep-pixel data to confirm"
  - test: "Leave in2 set to 'none'; confirm no in2 source columns appear in the matrix"
    expected: "Matrix shows only in1 source channel columns"
    why_human: "Visual layout of the Qt grid is not verifiable via static code inspection"
---

# Phase 3: DeepShuffle UI Verification Report

**Phase Goal:** DeepCShuffle exposes per-channel routing for up to 8 channels with labeled input and output ports, clean row layout, and layer-level routing via a ChannelSet knob.
**Implementation note:** Goal is satisfied by DeepCShuffle2 (new node). Original DeepCShuffle was preserved unchanged for backward compatibility.
**Verified:** 2026-03-15
**Status:** human_needed — all automated checks pass; 5 live-Nuke behaviors require human confirmation
**Re-verification:** No — initial verification


## Goal Achievement

### Observable Truths

| # | Truth | Status | Evidence |
|---|-------|--------|---------|
| 1 | DeepCShuffle2 exposes per-channel routing knob (ShuffleMatrixKnob) in the node panel | VERIFIED | `CustomKnob2(ShuffleMatrixKnob, f, &_matrixState, "matrix", "routing")` at DeepCShuffle2.cpp:204 |
| 2 | Input and output port labels use NDK channel names | VERIFIED | `buildLayout()` calls `DD::Image::getName(channelIterator)` for both row and column labels (ShuffleMatrixWidget.cpp:80,101) |
| 3 | Matrix supports up to 8 output channels | VERIFIED | `if (_outputChannelNames.size() >= 8) break;` at ShuffleMatrixWidget.cpp:78; `std::array<Channel, 8>` in DeepCShuffle2.cpp:19-20 |
| 4 | Layer-level routing via Input_ChannelSet_knob (in1 and in2) | VERIFIED | `Input_ChannelSet_knob(f, &_in1ChannelSet, ...)` and `Input_ChannelSet_knob(f, &_in2ChannelSet, ...)` at DeepCShuffle2.cpp:198,201 |
| 5 | Unrouted channels pass through unchanged (never zeroed) | VERIFIED | `Channel inChannel = z;` default before override loop at DeepCShuffle2.cpp:165 |
| 6 | Routing state serializes and deserializes across Nuke script save/load | VERIFIED | `to_script()/from_script()` implemented in ShuffleMatrixKnob.cpp:25-40; `from_script()` calls `changed()` to trigger widget sync |
| 7 | Column headers rebuild when ChannelSet changes | VERIFIED | `knob_changed()` calls `setChannelSets()` + `matrixKnob->updateWidgets()` at DeepCShuffle2.cpp:221-222; `updateWidgets` triggers `syncFromKnob` which calls `clearLayout() + buildLayout()` |
| 8 | DeepCShuffle2.so builds cleanly | VERIFIED | `cmake --build /workspace/build --target DeepCShuffle2` exits 0; both .so files present at /workspace/build/src/ |
| 9 | Single-source-per-output-row constraint enforced | VERIFIED | `onCellToggled` erases sibling row entries and restores current; QSignalBlocker used for visual uncheck at ShuffleMatrixWidget.cpp:258-277 |
| 10 | Original DeepCShuffle preserved (backward compatibility) | VERIFIED | `DeepCShuffle.cpp` restored (commit f5161b6); both targets in PLUGINS list; DeepCShuffle.so confirmed built |

**Score:** 10/10 truths verified (automated)


### Required Artifacts

| Artifact | Expected | Status | Details |
|----------|----------|--------|---------|
| `src/CMakeLists.txt` | Qt6 find_package, AUTOMOC, DeepCShuffle2 Qt6 link | VERIFIED | find_package(Qt6) line 66; CMAKE_AUTOMOC ON line 68; Qt6::Core/Gui/Widgets link line 111 |
| `src/ShuffleMatrixKnob.h` | ShuffleMatrixKnob Knob subclass declaration | VERIFIED | 135 lines; `class ShuffleMatrixKnob : public DD::Image::Knob`; all 6 virtual overrides declared; serialization format comment present |
| `src/ShuffleMatrixWidget.h` | ShuffleMatrixWidget QWidget subclass with Q_OBJECT | VERIFIED | 130 lines; `class ShuffleMatrixWidget : public QWidget`; `Q_OBJECT` at line 41; WidgetCallback, syncFromKnob, onCellToggled declared |
| `src/ShuffleMatrixKnob.cpp` | Knob virtual method implementations | VERIFIED | 96 lines; `ShuffleMatrixKnob::to_script` at line 25; all 6 NDK virtuals + accessors implemented |
| `src/ShuffleMatrixWidget.cpp` | Qt widget implementation | VERIFIED | 299 lines; `ShuffleMatrixWidget::buildLayout` at line 69; WidgetCallback, syncFromKnob, onCellToggled, clearLayout all implemented |
| `src/DeepCShuffle2.cpp` | Rewritten Op with ShuffleMatrixKnob, ChannelSet pickers, new data model | VERIFIED | 245 lines; `CustomKnob2(ShuffleMatrixKnob` at line 204; `Input_ChannelSet_knob` x2; passthrough routing at line 165 |


### Key Link Verification

| From | To | Via | Status | Details |
|------|----|-----|--------|---------|
| `src/CMakeLists.txt` | `Qt6::Widgets` | `target_link_libraries(DeepCShuffle2 PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)` | VERIFIED | Line 111 of CMakeLists.txt |
| `src/ShuffleMatrixKnob.h` | `src/ShuffleMatrixWidget.h` | forward declaration + `make_widget()` return type | VERIFIED | `class ShuffleMatrixWidget;` at line 13; `new ShuffleMatrixWidget(this)` in ShuffleMatrixKnob.cpp:58 |
| `src/ShuffleMatrixWidget.h` | `src/ShuffleMatrixKnob.h` | `ShuffleMatrixKnob* _knob` pointer member | VERIFIED | `ShuffleMatrixKnob* _knob;` at ShuffleMatrixWidget.h:106 |
| `src/ShuffleMatrixWidget.cpp` | `src/ShuffleMatrixKnob.h` | `_knob->setValue()` called in onCellToggled | VERIFIED | `_knob->setValue(serializedStream.str())` at ShuffleMatrixWidget.cpp:297 |
| `src/ShuffleMatrixKnob.cpp` | `src/ShuffleMatrixWidget.h` | `make_widget()` instantiates ShuffleMatrixWidget | VERIFIED | `return new ShuffleMatrixWidget(this)` at ShuffleMatrixKnob.cpp:58 |
| `src/DeepCShuffle2.cpp` | `src/ShuffleMatrixKnob.h` | `#include ShuffleMatrixKnob.h` + `CustomKnob2` macro | VERIFIED | `#include "ShuffleMatrixKnob.h"` at line 1; `CustomKnob2(ShuffleMatrixKnob, ...)` at line 204 |
| `src/DeepCShuffle2.cpp` | `_matrixState` string | `store()` copies knob state to Op; `doDeepEngine` reads it | VERIFIED | `_matrixState` parsed in `_validate()` at lines 71-94; routing used at lines 165-176 |
| `doDeepEngine` | passthrough logic | `inChannel = z` default before routing overrides | VERIFIED | `Channel inChannel = z;` at line 165; loop overrides only on match |


### Requirements Coverage

| Requirement | Source Plan(s) | Description | Status | Evidence |
|-------------|---------------|-------------|--------|---------|
| SHUF-01 | 03-01, 03-02, 03-03, 03-04 | Per-channel input/output routing knobs matching Shuffle node layout | SATISFIED | ShuffleMatrixKnob grid widget in DeepCShuffle2 panel; buildLayout creates row/col grid with channel-name labels |
| SHUF-02 | 03-02, 03-03, 03-04 | Input and output ports labeled with assigned channel names | SATISFIED | `DD::Image::getName(channelIterator)` used for all row and column labels; NDK channel names ("rgba.red", "depth.Z") shown in widget |
| SHUF-03 | 03-01, 03-02, 03-03 | Supports routing up to 8 channels | SATISFIED | `std::array<Channel, 8>` output/source arrays; `size() >= 8` guard in buildLayout |
| SHUF-04 | 03-01, 03-04 | Layer-level routing via Input_ChannelSet_knob | SATISFIED | Two `Input_ChannelSet_knob` calls in DeepCShuffle2::knobs(); `KNOB_CHANGED_ALWAYS` flag ensures knob_changed fires on every change |

All four SHUF requirements satisfied. No orphaned requirements found.


### Anti-Patterns Found

No anti-pattern comments (TODO, FIXME, PLACEHOLDER, etc.) found in any phase-created files.
No empty implementations, stub returns, or console-log-only handlers found.
No references to old `_inChannel0..3` data model remain in DeepCShuffle2.cpp.


### Human Verification Required

#### 1. Matrix Widget Renders in Nuke Panel

**Test:** Load DeepCShuffle2.so in Nuke, place a DeepCShuffle2 node, open properties panel.
**Expected:** Two ChannelSet pickers (in1 defaulting to rgba, in2 defaulting to none) appear above a toggle-button grid with channel name labels.
**Why human:** Qt widget rendering inside Nuke's panel host cannot be verified by static file inspection or build success alone.

#### 2. Column Headers Rebuild on ChannelSet Change

**Test:** With DeepCShuffle2 node open, change the in1 ChannelSet picker to a different layer (e.g. depth).
**Expected:** Matrix column headers update to the new layer's channel names without reopening the panel.
**Why human:** The `knob_changed -> setChannelSets -> updateWidgets -> syncFromKnob` live event chain requires a running Nuke session.

#### 3. Routing Survives Script Save/Reload

**Test:** Toggle a cell (e.g. route rgba.red to depth.Z), save the Nuke script (File > Save), close and reopen Nuke, reload the script.
**Expected:** The matrix routing assignment is restored — the toggled button is checked.
**Why human:** `to_script()/from_script()` round-trip requires writing an actual .nk file and reloading it in Nuke.

#### 4. Passthrough for Unrouted Channels

**Test:** Connect a node with multiple layers (rgba + depth) upstream; route only rgba channels; inspect the output in a viewer or downstream node.
**Expected:** Depth and other non-routed layers are present and unmodified in the output — they are not zeroed.
**Why human:** Passthrough-first behavior in `doDeepEngine()` requires live deep-pixel data flowing through the node to confirm correct channel routing at runtime.

#### 5. in2 "none" Hides In2 Columns

**Test:** Leave in2 set to "none" (Chan_Black). Inspect the matrix grid.
**Expected:** No in2 source channel columns appear — the matrix shows only in1 source channels.
**Why human:** The conditional `if (in2Set != ChannelSet(Chan_Black))` at ShuffleMatrixWidget.cpp:96 suppresses in2 columns, but the actual rendered column count requires visual inspection in a live Nuke panel.


### Build Artifact Verification

| Artifact | Path | Status |
|----------|------|--------|
| DeepCShuffle2.so | /workspace/build/src/DeepCShuffle2.so | EXISTS |
| DeepCShuffle.so (original preserved) | /workspace/build/src/DeepCShuffle.so | EXISTS |
| moc_ShuffleMatrixWidget.cpp | Generated by AUTOMOC at build time | GENERATED (confirmed by build output showing "Automatic MOC for target DeepCShuffle2") |

### Commit Verification

All commits documented in summaries exist in git history:

| Commit | Description |
|--------|-------------|
| a2dc62b | chore(03-01): add Qt6 find_package, AUTOMOC, and DeepCShuffle link |
| 4ef58a7 | feat(03-02): add ShuffleMatrixKnob.h |
| bcfe276 | feat(03-02): add ShuffleMatrixWidget.h |
| 3ac5f74 | feat(03-03): implement ShuffleMatrixKnob.cpp |
| 428dfac | feat(03-03): implement ShuffleMatrixWidget.cpp |
| f5161b6 | fix(03-04): restore original DeepCShuffle.cpp for backward compatibility |
| ae1c7c6 | feat(03-04): add DeepCShuffle2 with ShuffleMatrixKnob matrix UI |

---

_Verified: 2026-03-15_
_Verifier: Claude (gsd-verifier)_
