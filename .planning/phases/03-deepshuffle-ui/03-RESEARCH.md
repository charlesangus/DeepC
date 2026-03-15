# Phase 3: DeepShuffle UI — Research

**Researched:** 2026-03-14
**Domain:** Nuke NDK custom Knob subclass, Qt widget embedding, DeepFilterOp channel routing
**Confidence:** HIGH (primary findings verified against installed NDK 16.0v6 headers and official examples)

---

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- Single Op input in the DAG (one connection point, unlabeled, no custom port label)
- Two ChannelSet pickers (in1 required defaults to rgba, in2 optional defaults to none); both read from the same connected input node
- Routing UI: C++ custom `DD::Image::Knob` subclass with embedded Qt widget — visual matrix grid matching the legacy Shuffle node layout (rows = output channels, columns = source channels from in1/in2)
- Toggle buttons at each cell; click to route source channel → destination channel
- Matrix state serialized to a hidden string knob for script/render persistence (no Python dependency)
- `knob_changed()` in the Op updates ChannelSet picker visibility and matrix column headers when in1/in2 ChannelSet changes
- Passthrough: channels not covered by in1 or in2 selection pass through unchanged; nothing is zeroed
- SHUF-02 satisfied by in-panel row/column labels on the matrix widget; no DAG port label changes

### Claude's Discretion
- None identified; all gray areas were resolved in discuss-phase

### Deferred Ideas (OUT OF SCOPE)
- Shuffle2 noodle/wire routing UI (NDK does not expose this API)
</user_constraints>

---

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| SHUF-01 | DeepCShuffle exposes per-channel input/output routing knobs matching the layout of the legacy Nuke Shuffle node | Custom Knob subclass with `make_widget()` returning a Qt grid widget; `CustomKnob2` macro registers it |
| SHUF-02 | DeepCShuffle input and output ports are labeled with their assigned channel names | In-panel matrix: column headers = source channel names from in1/in2 ChannelSet; row labels = output channel names |
| SHUF-03 | DeepCShuffle supports routing up to 8 channels (expanded from current 4) | Replace 4 fixed `_inChannelN`/`_outChannelN` member pairs with a `std::array<Channel, 8>` per direction; matrix has up to 8 rows |
| SHUF-04 | DeepCShuffle provides layer-level routing via `Input_ChannelSet_knob` in addition to per-channel selection | `Input_ChannelSet_knob(f, &_in1ChannelSet, 0, "in1")` and `Input_ChannelSet_knob(f, &_in2ChannelSet, 0, "in2")` with `KNOB_CHANGED_ALWAYS` flag |
</phase_requirements>

---

## Summary

The existing `DeepCShuffle.cpp` is a minimal 160-line `DeepFilterOp` with four hard-coded `Input_Channel_knob`/`Channel_knob` pairs (in/out for channels 0–3). This phase replaces that UI entirely with a custom `DD::Image::Knob` subclass that embeds a Qt grid widget (toggle-button matrix), plus two `Input_ChannelSet_knob` pickers for layer-level selection.

The NDK 16.0v6 install at `/usr/local/Nuke16.0v6` ships Qt6 runtime libraries (`libQt6Widgets.so.6.5.3` etc.) but does **not** ship Qt6 development headers or `moc`. The official NDK example `AddCustomQt.cpp` requires `find_package(Qt6)` — meaning Qt6 headers must be installed separately (e.g., `apt install qt6-base-dev` on Debian Bookworm). The apt package cache on this machine does not currently have Qt6 packages indexed, so **installing Qt6 dev headers is a Wave 0 prerequisite** for this phase.

Serialization uses the standard NDK pattern: the custom knob implements `to_script()` / `from_script()` directly (no separate hidden String_knob is needed, though one could be used). The matrix state should be stored inside the custom knob's own member data and persisted via `to_script`/`from_script`. The `knob_changed()` override in the Op responds to ChannelSet changes by calling `knob("matrix_knob")->updateWidgets()` to rebuild column headers.

**Primary recommendation:** Implement `ShuffleMatrixKnob : public DD::Image::Knob` following the `AddCustomQt.cpp` and `CryptomatteLayerKnob.cpp` patterns verbatim; add Qt6 dev headers as a CMake build prerequisite; use `CustomKnob2` macro for registration.

---

## Standard Stack

### Core
| Library / API | Version | Purpose | Why Standard |
|---------------|---------|---------|--------------|
| `DD::Image::Knob` | NDK 16.0v6 | Base class for custom knob | Only NDK-supported way to embed custom UI widget in node panel |
| `DD::Image::DeepFilterOp` | NDK 16.0v6 | Op base class (existing) | Already in use; no change |
| `Input_ChannelSet_knob` | NDK 16.0v6 | Layer-level picker bound to input 0 | Standard NDK function; filters menu to channels present in input stream |
| `CustomKnob2` macro | NDK 16.0v6 | Register custom knob in `knobs()` | Required macro for `Knob` subclasses with pointer + label arg |
| Qt6 Widgets | 6.5.3 (Nuke-bundled) | `QWidget`, `QGridLayout`, `QPushButton`, `QLabel` | Nuke 16 uses Qt6.5; must match runtime version |

### Supporting
| Library / API | Version | Purpose | When to Use |
|---------------|---------|---------|-------------|
| `Knob::to_script` / `from_script` | NDK 16.0v6 | Serialize matrix state to Nuke script | Always — required for script save/load and undo |
| `Knob::changed()` | NDK 16.0v6 | Notify Nuke that knob value changed | Call from widget slot after toggle to trigger hash recook |
| `Knob::new_undo()` | NDK 16.0v6 | Register undo checkpoint | Call before mutating `_matrixState` in setValue path |
| `Knob::updateWidgets()` | NDK 16.0v6 | Repaint widget without triggering a recook | Call from `Op::knob_changed()` after ChannelSet changes columns |
| `Knob::addCallback` / `removeCallback` | NDK 16.0v6 | Widget lifecycle (update/destroy) | Required in every custom Qt widget's constructor/destructor |
| `SetFlags(f, Knob::KNOB_CHANGED_ALWAYS)` | NDK 16.0v6 | Force `knob_changed()` call on ChannelSet change | Apply to `in1` and `in2` ChannelSet knobs |

### Alternatives Considered
| Instead of | Could Use | Tradeoff |
|------------|-----------|----------|
| `CustomKnob2` (C++ subclass) | Python `nuke.PyCustom_Knob` | Python not allowed (must survive headless render); ruled out by constraint |
| `to_script`/`from_script` on custom knob | Hidden `String_knob` + custom knob reading it | Hidden String_knob approach adds an extra knob and an extra `knob_changed` notification round-trip; direct serialization is simpler and is what the official examples use |
| `QGridLayout` + `QPushButton` toggle buttons | `QTableWidget` | `QTableWidget` is heavier and harder to style; `QPushButton` with `setCheckable(true)` matches the legacy Shuffle visual most directly |

**Installation (build prerequisite — Wave 0):**
```bash
# On the build machine — required before CMake can find Qt6
sudo apt install qt6-base-dev
# Then add to src/CMakeLists.txt:
find_package(Qt6 COMPONENTS Core Gui Widgets REQUIRED)
set(CMAKE_AUTOMOC ON)
```

---

## Architecture Patterns

### Recommended File Layout
```
src/
├── DeepCShuffle.cpp          # Op class + knob registration (modified)
├── DeepCShuffle.h            # (new) Op class declaration; ShuffleMatrixKnob forward decl
├── ShuffleMatrixKnob.h       # (new) Knob subclass + moc.h include guard
├── ShuffleMatrixKnob.cpp     # (new) Knob implementation: to_script, from_script, make_widget
├── ShuffleMatrixWidget.h     # (new) QWidget subclass — Q_OBJECT, moc-able
└── ShuffleMatrixWidget.cpp   # (new) Qt widget implementation: layout, toggle slots
```

The moc header pattern (`.moc.h` or `.h` with `Q_OBJECT`) follows the NDK examples exactly. `CMAKE_AUTOMOC ON` tells CMake to run moc automatically on any file containing `Q_OBJECT`.

### Pattern 1: Custom Knob Subclass (AddCustomQt.cpp pattern)
**What:** Subclass `DD::Image::Knob`; override `Class()`, `not_default()`, `to_script()`, `from_script()`, `store()`, `make_widget()`; register with `CustomKnob2` macro.
**When to use:** Any time a custom Qt widget must live inside a Nuke node panel and persist its state.

```cpp
// Source: /usr/local/Nuke16.0v6/Documentation/NDKExamples/examples/AddCustomQt.cpp
class ShuffleMatrixKnob : public DD::Image::Knob
{
public:
  ShuffleMatrixKnob(DD::Image::Knob_Closure* kc, std::string* data, const char* n, const char* label)
    : Knob(kc, n, label)
  {
    if (data) _matrixState = *data;
  }

  const char* Class() const override { return "ShuffleMatrixKnob"; }
  bool not_default() const override { return !_matrixState.empty(); }

  void to_script(std::ostream& os, const DD::Image::OutputContext*, bool quote) const override
  {
    if (quote) os << cstring(_matrixState.c_str());
    else       os << _matrixState;
  }

  bool from_script(const char* v) override
  {
    _matrixState = v ? v : "";
    changed();
    return true;
  }

  void store(DD::Image::StoreType, void* dest, DD::Image::Hash& hash,
             const DD::Image::OutputContext&) override
  {
    if (dest) *static_cast<std::string*>(dest) = _matrixState;
    hash.append(_matrixState);
  }

  DD::Image::WidgetPointer make_widget(const DD::Image::WidgetContext&) override
  {
    auto* widget = new ShuffleMatrixWidget(this);
    return widget;
  }

  // Called by widget when user clicks a cell toggle
  void setValue(const std::string& newState)
  {
    new_undo("ShuffleMatrix");
    _matrixState = newState;
    changed();   // triggers hash recook + widget update
  }

private:
  std::string _matrixState;  // serialized routing: "out0:in1.r,out1:in1.g,out2:in1.b,out3:in1.a"
};
```

### Pattern 2: Widget Callback Lifecycle (CryptomatteLayerWidget.cpp pattern)
**What:** Qt widget registers `Knob::Callback` in constructor, removes it in destructor; handles `kUpdateWidgets`, `kDestroying`, `kIsVisible`.
**When to use:** Required for every custom widget — Nuke uses these callbacks to keep widget in sync with knob state and to clean up on panel close.

```cpp
// Source: /usr/local/Nuke16.0v6/Documentation/NDKExamples/examples/CryptomatteLayerWidget.cpp
class ShuffleMatrixWidget : public QWidget
{
  Q_OBJECT
public:
  ShuffleMatrixWidget(ShuffleMatrixKnob* knob) : _knob(knob)
  {
    _knob->addCallback(WidgetCallback, this);
    buildLayout();
  }
  ~ShuffleMatrixWidget()
  {
    if (_knob) _knob->removeCallback(WidgetCallback, this);
  }

  static int WidgetCallback(void* closure, DD::Image::Knob::CallbackReason reason)
  {
    auto* widget = static_cast<ShuffleMatrixWidget*>(closure);
    switch (reason) {
      case DD::Image::Knob::kIsVisible:
        for (QWidget* w = widget->parentWidget(); w; w = w->parentWidget())
          if (qobject_cast<QTabWidget*>(w))
            return widget->isVisibleTo(w);
        return widget->isVisible();
      case DD::Image::Knob::kUpdateWidgets:
        widget->syncFromKnob();
        return 0;
      case DD::Image::Knob::kDestroying:
        widget->_knob = nullptr;
        return 0;
      default: return 0;
    }
  }
private:
  ShuffleMatrixKnob* _knob;
};
```

### Pattern 3: ChannelSet Knob + knob_changed Rebuild
**What:** Register `Input_ChannelSet_knob` with `KNOB_CHANGED_ALWAYS`; in `knob_changed()`, detect change and call `updateWidgets()` on the matrix knob to rebuild column headers.
**When to use:** Any time UI column/row headers must reflect the live ChannelSet selection.

```cpp
// Op::knobs() — source verified against NDK Knobs.h lines 810-814
void DeepCShuffle::knobs(Knob_Callback f)
{
  Input_ChannelSet_knob(f, &_in1ChannelSet, 0, "in1", "in1 channels");
  SetFlags(f, Knob::KNOB_CHANGED_ALWAYS);

  Input_ChannelSet_knob(f, &_in2ChannelSet, 0, "in2", "in2 channels");
  SetFlags(f, Knob::KNOB_CHANGED_ALWAYS);

  CustomKnob2(ShuffleMatrixKnob, f, &_matrixState, "matrix", "routing");
}

// Op::knob_changed() — pattern from DynamicKnobs.cpp line 52-59 + DeepCPMatte.cpp line 209
int DeepCShuffle::knob_changed(Knob* k)
{
  if (k->is("in1") || k->is("in2") || k == &Knob::inputChange || k == &Knob::showPanel)
  {
    if (Knob* matrixKnob = knob("matrix"))
      matrixKnob->updateWidgets();  // rebuild column headers without recook
    return 1;
  }
  return DeepFilterOp::knob_changed(k);
}
```

### Pattern 4: Matrix State Serialization Format
**What:** Flat string encoding the full routing table. Each entry maps an output slot index to a source channel name string. Slots with no mapping (passthrough) are omitted.
**When to use:** This format is the only state that `to_script`/`from_script` need to exchange. Parseable in headless render with no Qt dependency.

```
// Example serialized state (8 output slots, in1=rgba, in2=depth.Z)
"0:rgba.red,1:rgba.green,2:rgba.blue,3:rgba.alpha,4:depth.Z"
// Slot 5,6,7 omitted → those output channels pass through unchanged
// Parsing: split by comma, split each entry by ":", map slot index → Channel
```

### Op Data Model Changes
```cpp
// Replace the 4-pair fixed layout:
//   Channel _inChannel0..3; Channel _outChannel0..3;
// With:
ChannelSet _in1ChannelSet;   // defaults to Mask_RGBA
ChannelSet _in2ChannelSet;   // defaults to Chan_Black (none)
std::string _matrixState;    // serialized routing — owned by Op, mirrored by knob store()
// Routing map built from _matrixState at _validate() time:
std::array<Channel, 8> _outputChannels;  // which output channels are active
std::array<Channel, 8> _sourceChannels;  // which input channel feeds each output slot
```

### Anti-Patterns to Avoid
- **Calling `op()->validate()` from inside the widget or knob:** Deadlocks the render thread (this was the GLPM-01 bug). Never call validate from UI callbacks.
- **Storing Qt object pointers as Op members:** Qt objects live only on the GUI thread; the Op lives on render threads too. Keep Qt strictly inside `ShuffleMatrixWidget`.
- **Using a hidden `String_knob` instead of direct `to_script`:** Adds a superfluous knob to the script file and complicates the callback chain with no benefit.
- **Calling `changed()` from `from_script`... with caution:** `changed()` must be called in `from_script` to inform Nuke the value was set — this is the pattern in both NDK examples (AddCustomQt line 52, CryptomatteLayerKnob line 181). Do not omit it.
- **Forgetting `Q_OBJECT` in the widget class:** Causes Qt SIGNAL/SLOT connections to silently fail; `AUTOMOC` requires `Q_OBJECT` to detect that moc needs to run on a file.

---

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Knob script persistence | Custom binary serializer | `to_script()` / `from_script()` virtuals | NDK handles all script read/write orchestration; custom binary format breaks copy/paste |
| Widget-knob sync | Manual polling loop | `Knob::addCallback` + `kUpdateWidgets` | Nuke's callback system is the only thread-safe way to push state to widgets |
| Channel name display | Manual `Channel` int-to-string | `getName(channel)` (from `DDImage/Channel.h`) | NDK provides `const char* getName(Channel)` — handles all layer.channel name formatting |
| ChannelSet iteration | Manual bitmask iteration | `foreach(z, channelSet)` NDK macro | NDK `foreach` iterates `ChannelSet` correctly across sparse channel numbers |
| Undo support | Custom undo stack | `new_undo("name")` before mutation | NDK undo is required; omitting it makes changes non-undoable in Nuke |

**Key insight:** The NDK knob serialization and widget lifecycle infrastructure handles all the hard coordination. The custom class only needs to store state internally and implement the ~6 virtual methods.

---

## Common Pitfalls

### Pitfall 1: Qt Headers Not Installed
**What goes wrong:** CMake `find_package(Qt6)` fails; build fails with "Qt6 not found"; or moc is not run, causing linker errors like `undefined reference to vtable for ShuffleMatrixWidget`.
**Why it happens:** Nuke ships Qt6 runtime `.so` files but no development headers or moc tool. The apt cache on this machine has no Qt6 packages indexed.
**How to avoid:** Wave 0 task must install `qt6-base-dev` (or equivalent) before any DeepCShuffle code that includes Qt headers is compiled. Add `find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)` and `set(CMAKE_AUTOMOC ON)` to `src/CMakeLists.txt`.
**Warning signs:** CMake output says "Qt6 not found" or linker reports "undefined reference to vtable".

### Pitfall 2: Python.h Include Order with Qt
**What goes wrong:** `slots` macro from Qt is redefined by Python headers, causing compile errors in `.moc.h` files.
**Why it happens:** Qt `#define slots` to empty string; Python headers declare a struct member named `slots`. Include order matters.
**How to avoid:** If Python.h is needed, include it before any Qt headers (exactly as `CryptomatteLayerKnob.cpp` line 9 documents). In DeepCShuffle there is no Python dependency so this should not arise — but note it if Python interaction is ever added.
**Warning signs:** Compiler error mentioning `PyType_Slot *slots` or "`slots` is not a valid identifier".

### Pitfall 3: Matrix State Out of Sync on Script Load
**What goes wrong:** After loading a saved script, the matrix widget shows the wrong (blank or default) state even though the serialized string was loaded correctly.
**Why it happens:** `from_script()` is called before the widget exists. The knob must call `changed()` in `from_script()` to schedule a later `kUpdateWidgets` callback, so the widget syncs when it is eventually created.
**How to avoid:** Always call `changed()` at the end of `from_script()` (verified in AddCustomQt.cpp line 52 and CryptomatteLayerKnob.cpp line 181).
**Warning signs:** Matrix looks empty after File > Open; correct after user clicks a cell.

### Pitfall 4: ChannelSet knob_changed Not Firing
**What goes wrong:** Changing in1 or in2 ChannelSet does not rebuild matrix column headers.
**Why it happens:** `knob_changed()` is only called when the knob has `KNOB_CHANGED_ALWAYS` flag. Without it, Nuke may suppress the call if it previously returned 0.
**How to avoid:** Apply `SetFlags(f, Knob::KNOB_CHANGED_ALWAYS)` immediately after each `Input_ChannelSet_knob(...)` call in `knobs()`. Also handle `k == &Knob::showPanel` in `knob_changed()` to force initial column population when the panel first opens.
**Warning signs:** Column headers show stale layer names after changing ChannelSet.

### Pitfall 5: Channel Names After ChannelSet Change Are Stale
**What goes wrong:** Matrix widget builds column headers in `make_widget()` from the ChannelSet state at panel-open time, then never updates.
**Why it happens:** Qt widget only builds its layout once in `buildLayout()`. Column headers must be rebuilt when `kUpdateWidgets` fires.
**How to avoid:** In `ShuffleMatrixWidget::syncFromKnob()` (called from `kUpdateWidgets`), enumerate the current ChannelSet values from the knob's Op, clear and rebuild the column headers, then re-sync toggle states. The knob must expose a reference to its parent Op's ChannelSets (via `knob->op()` cast).
**Warning signs:** Column headers show old layer name after changing in1 ChannelSet.

### Pitfall 6: Passthrough Zeroing Output Channels
**What goes wrong:** Output channels not covered by any matrix routing are written as 0.0f instead of being passed through.
**Why it happens:** Current implementation checks `z == _outChannelN` and falls back to 0.0f via `Chan_Black`. New implementation must default `inChannel = z` (passthrough) before applying any matrix overrides.
**How to avoid:** In `doDeepEngine()`, initialize `inChannel = z` for each output channel, then override only for channels that have a routing entry. The copy-then-override approach (documented in CONTEXT.md) is the correct idiom.
**Warning signs:** Output channels not in the routing table become black.

---

## Code Examples

### Verified: CustomKnob2 Registration Macro
```cpp
// Source: /usr/local/Nuke16.0v6/include/DDImage/Knobs.h lines 1483-1485
// Constructor signature required: (Knob_Closure*, pointer*, name, label)
#define CustomKnob2(knobclass, cb, pointer, name, label)                     \
  (knobclass*)cb(DD::Image::CUSTOM_KNOB, DD::Image::Custom, pointer, name, 0, \
    (cb.makeKnobs() && cb.filter(name))                                       \
      ? new knobclass(&cb, pointer, name, label)                               \
      : (cb.setLastMadeKnob(nullptr), (knobclass*)nullptr))
```

### Verified: Input_ChannelSet_knob Signature
```cpp
// Source: /usr/local/Nuke16.0v6/include/DDImage/Knobs.h lines 810-813
// input=0 means "filter menu to channels from Op input 0"
inline Knob* Input_ChannelSet_knob(Knob_Callback f, ChannelSet* p, int input,
                                    NAME n, LABEL l = nullptr,
                                    ndk::ChannelManager* chanmgr = nullptr)
{
  ChannelKnobData data{0/*count*/, input, chanmgr};
  return f(CHANNEL_MASK_KNOB, ChannelSetPtr, p, n, l, (void*)&data);
}
```

### Verified: Knob Flags for ChannelSet knob_changed
```cpp
// Source: /usr/local/Nuke16.0v6/include/DDImage/Knobs.h lines 438-447 + Knob.h line 458
// Apply immediately after knob creation in knobs()
SetFlags(f, Knob::KNOB_CHANGED_ALWAYS);
```

### Verified: knob_changed Special Knob References
```cpp
// Source: /usr/local/Nuke16.0v6/include/DDImage/Knob.h lines 1582-1584
static Knob& showPanel;   // k == &Knob::showPanel when node panel opens
static Knob& hidePanel;   // k == &Knob::hidePanel when panel closes
static Knob& inputChange; // k == &Knob::inputChange when upstream connections change
```

### Verified: knob() Lookup + updateWidgets
```cpp
// Source: pattern from DeepCPMatte.cpp line 231 + Knob.h line 1317
// Safe null-check pattern:
if (Knob* matrixKnob = knob("matrix"))
  matrixKnob->updateWidgets();   // sends kUpdateWidgets to all widgets; does NOT trigger recook
```

### Verified: CMake Qt6 + AUTOMOC Integration
```cmake
# Source: /usr/local/Nuke16.0v6/Documentation/NDKExamples/examples/CMakeLists.txt lines 131-136
find_package(Qt6 COMPONENTS Core Gui Widgets)
if (Qt6_FOUND)
  set(CMAKE_AUTOMOC ON)
  target_sources(DeepCShuffle PRIVATE ShuffleMatrixWidget.h)  # moc needs to see Q_OBJECT headers
  target_link_libraries(DeepCShuffle PRIVATE Qt::Core Qt::Gui Qt::Widgets)
endif()
```

---

## State of the Art

| Old Approach (current DeepCShuffle) | New Approach (this phase) | Impact |
|-------------------------------------|---------------------------|--------|
| 4 fixed `Input_Channel_knob` + `Channel_knob` pairs (dropdown rows) | Qt matrix widget with toggle buttons | Matches legacy Shuffle visual; supports up to 8 channels |
| No ChannelSet pickers | Two `Input_ChannelSet_knob` pickers for layer-level routing | SHUF-04 satisfied |
| State stored directly in `Channel` member variables | State in `std::string _matrixState`, serialized via `to_script`/`from_script` | Survives script save/load; accessible headlessly |
| No `knob_changed` override | `knob_changed` rebuilds matrix columns on ChannelSet change | Live column header updates |

**Deprecated/outdated in current source:**
- `_inChannel0..3` / `_outChannel0..3` fixed member pairs: replaced by `_in1ChannelSet`, `_in2ChannelSet`, `_matrixState`
- `Input_Channel_knob` + arrow `Text_knob` + `Channel_knob` triplet rows: replaced by `CustomKnob2` matrix widget
- The current `knobs()` body (lines 134–147 in DeepCShuffle.cpp): entirely replaced

---

## Open Questions

1. **Qt6 dev headers availability on build machine**
   - What we know: apt cache has no Qt6 packages indexed; Nuke ships Qt6 runtime but no headers or moc; Debian Bookworm (the OS) does ship `qt6-base-dev` in its official repos.
   - What's unclear: Whether `apt update && apt install qt6-base-dev` will succeed in the actual CI/build environment (network access required).
   - Recommendation: Make Wave 0 the first task; have it verify Qt6 is installable and add the `find_package(Qt6)` block to `src/CMakeLists.txt`; if Qt6 headers are unavailable from apt, the fallback is to download a Qt6 source tarball and build headers, but this is highly unlikely to be needed on standard Debian Bookworm.

2. **Serialization format: slot-indexed vs channel-name-indexed**
   - What we know: The matrix has up to 8 output rows; input columns are dynamic (depend on ChannelSet). Channel names can be arbitrary strings.
   - What's unclear: Whether to key the serialized state by output slot index (e.g., `"0:rgba.red"`) or by output channel name (e.g., `"rgba.red:rgba.red"`). Slot-indexed is simpler to parse; channel-name-indexed survives reordering of the output ChannelSet.
   - Recommendation: Key by output channel name string (not slot index) so that routing assignments survive a script reload where the output ChannelSet order may differ. Format: `"outChanName:srcChanName"` pairs, comma-separated.

3. **Matrix widget height for 8 rows**
   - What we know: Up to 8 output rows + up to ~8 in1 columns + ~8 in2 columns = up to 16 toggle buttons per row. Legacy Shuffle shows 4 rows.
   - What's unclear: Whether the Nuke panel will allocate enough vertical space for 8 rows automatically, or whether a `setMinimumHeight` hint is needed on the widget.
   - Recommendation: Set `setMinimumHeight(8 * rowHeight)` in `buildLayout()` where `rowHeight` is ~24px. Nuke panels will expand to accommodate.

---

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Build verification only (no unit test framework in project) |
| Config file | `build/` (CMake build directory) |
| Quick run command | `cmake --build /workspace/build --target DeepCShuffle` |
| Full suite command | `cmake --build /workspace/build` |

### Phase Requirements → Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| SHUF-01 | Plugin builds and loads into Nuke; matrix grid widget appears in panel | build + manual | `cmake --build /workspace/build --target DeepCShuffle` | ✅ existing CMakeLists |
| SHUF-02 | In-panel column/row labels show channel names matching in1/in2 selections | manual visual | N/A — requires Nuke GUI | manual only |
| SHUF-03 | Up to 8 output channel rows render without crash | build + manual | `cmake --build /workspace/build --target DeepCShuffle` | ✅ |
| SHUF-04 | Changing in1 ChannelSet to a non-rgba layer rebuilds matrix columns | manual | N/A — requires Nuke GUI | manual only |

### Sampling Rate
- **Per task commit:** `cmake --build /workspace/build --target DeepCShuffle`
- **Per wave merge:** `cmake --build /workspace/build`
- **Phase gate:** Full suite green + manual UAT in Nuke before `/gsd:verify-work`

### Wave 0 Gaps
- [ ] `sudo apt install qt6-base-dev` (or confirm Qt6 headers are already present) — required before ShuffleMatrixWidget.h can compile
- [ ] Add to `src/CMakeLists.txt`: `find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)`, `set(CMAKE_AUTOMOC ON)`, `target_link_libraries(DeepCShuffle PRIVATE Qt::Core Qt::Gui Qt::Widgets)`
- [ ] Re-run `cmake /workspace -B /workspace/build` after Qt6 install to regenerate build system with MOC support

*(No automated test framework gaps — project relies on build verification + Nuke manual UAT)*

---

## Sources

### Primary (HIGH confidence)
- `/usr/local/Nuke16.0v6/include/DDImage/Knob.h` — `make_widget`, `to_script`, `from_script`, `changed`, `updateWidgets`, `new_undo`, `addCallback`, flag constants, `showPanel`/`hidePanel`/`inputChange` statics
- `/usr/local/Nuke16.0v6/include/DDImage/Knobs.h` — `CustomKnob2` macro, `Input_ChannelSet_knob`, `String_knob`, `SetFlags`, `KNOB_CHANGED_ALWAYS`, `ALWAYS_SAVE`, `INVISIBLE`
- `/usr/local/Nuke16.0v6/include/DDImage/Channel_KnobI.h` — `Channel_KnobI` interface; confirmed `Input_ChannelSet_knob` uses `SelectionHandling::Default` (input channels promoted, others in "Other Layers" submenu)
- `/usr/local/Nuke16.0v6/Documentation/NDKExamples/examples/AddCustomQt.cpp` + `AddCustomQt.moc.h` — complete `Knob` subclass example with `QDial` widget, `to_script`/`from_script`, callback lifecycle
- `/usr/local/Nuke16.0v6/Documentation/NDKExamples/examples/CryptomatteLayerKnob.cpp` + `CryptomatteLayerKnob.h` — production-quality `Knob` subclass with `updateUI`, dynamic menu, `store`, Python integration
- `/usr/local/Nuke16.0v6/Documentation/NDKExamples/examples/CryptomatteLayerWidget.cpp` — full `QWidget` subclass with callback dispatch (kIsVisible, kUpdateWidgets, kDestroying)
- `/usr/local/Nuke16.0v6/Documentation/NDKExamples/examples/CMakeLists.txt` lines 131–167 — confirmed `find_package(Qt6)` + `CMAKE_AUTOMOC ON` + `target_link_libraries(... Qt::Widgets)` pattern
- `/usr/local/Nuke16.0v6/Documentation/NDKExamples/examples/DynamicKnobs.cpp` — `KNOB_CHANGED_ALWAYS` + `knob_changed` + `showPanel` pattern
- `/workspace/src/DeepCShuffle.cpp` — current 159-line implementation; confirmed 4-channel fixed layout to be replaced
- `/workspace/src/CMakeLists.txt` — confirmed `add_nuke_plugin` function; no Qt currently configured
- `/workspace/src/DeepCPMatte.cpp` lines 209–236 — project's own `knob_changed` pattern for reference

### Secondary (MEDIUM confidence)
- Debian Bookworm package database (inferred): `qt6-base-dev` is available in standard Bookworm repos (Qt 6.4.x on Bookworm); Nuke 16 ships Qt 6.5.3. Minor version mismatch (6.4 headers vs 6.5 runtime) is generally binary-compatible for `QWidget`/`QPushButton`/`QGridLayout` usage, but should be verified during Wave 0 install.

### Tertiary (LOW confidence)
- Qt 6.4 vs 6.5 ABI compatibility for `QWidget` plugin use: Multiple NDK community reports confirm Qt minor-version header/runtime mismatches work for basic widget usage, but this has not been verified against this specific machine's available packages.

---

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH — verified against installed NDK headers and official NDK examples
- Architecture: HIGH — derived directly from official NDK example code (`AddCustomQt.cpp`, `CryptomatteLayerKnob.cpp`)
- Pitfalls: HIGH for NDK-specific ones (verified in code); MEDIUM for Qt version mismatch (inferred)
- Qt availability: LOW — apt cache is empty; Debian Bookworm normally has qt6-base-dev but this was not confirmed on this machine

**Research date:** 2026-03-14
**Valid until:** 2026-09-14 (stable NDK API; 6 months)
