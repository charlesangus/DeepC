# Architecture Research

**Domain:** Nuke NDK C++ deep-compositing plugin suite (DeepC milestone additions)
**Researched:** 2026-03-12
**Confidence:** HIGH (NDK patterns verified against Foundry official docs + direct codebase inspection)

---

## System Overview

The existing architecture has three layers a new component can land in. Every new feature must
choose the right layer and the right NDK base class before any code is written.

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        NDK BASE LAYER (Foundry DDImage)                     │
│  DeepFilterOp   DeepPixelOp   Iop   Op   ViewerContext   DeepOp interface   │
└──────────────────────────────┬──────────────────────────────────────────────┘
                               │ inherits / wraps
┌──────────────────────────────▼──────────────────────────────────────────────┐
│                     SHARED WRAPPER LIBRARY (DeepC-owned)                    │
│                                                                             │
│   DeepCWrapper (DeepFilterOp subclass)                                      │
│   └─ DeepCMWrapper (DeepCWrapper subclass, adds matte-op semantics)         │
│                                                                             │
│   [Optional planned addition: GradeCoefficients utility struct/mixin]       │
└────────────┬──────────────────────────────┬────────────────────────────────┘
             │ 11 color-effect plugins       │ 3 matte-gen plugins
┌────────────▼──────────┐        ┌───────────▼─────────────────────────────┐
│   PLUGINS_WRAPPED     │        │          PLUGINS_MWRAPPED               │
│   (DeepCWrapper base) │        │          (DeepCMWrapper base)           │
│   DeepCAdd            │        │   DeepCID  DeepCPNoise  DeepCPMatte     │
│   DeepCGrade          │        │   [GL handles live in DeepCPMatte only] │
│   DeepCSaturation     │        └─────────────────────────────────────────┘
│   DeepCHueShift etc.  │
└───────────────────────┘
┌──────────────────────────────────────────────────────────────────────────────┐
│   PLUGINS (direct NDK base, no wrapper)                                      │
│   DeepCKeymix  DeepCAddChannels  DeepCShuffle  DeepCWorld  DeepCConstant ... │
│                                                                              │
│   NEW: DeepBlur (DeepFilterOp direct)                                        │
│   NEW: DeepDefocus (Iop with DeepOp input)                                   │
│   NEW: [DeepThinner — same layer, same category if integrated]               │
└──────────────────────────────────────────────────────────────────────────────┘
┌──────────────────────────────────────────────────────────────────────────────┐
│   THIRD-PARTY LAYER                                                          │
│   FastNoise (vendored in-tree)   DeepThinner (to be vendored)                │
└──────────────────────────────────────────────────────────────────────────────┘
```

---

## Component Responsibilities and Boundaries

| Component | Responsibility | Communicates With |
|-----------|---------------|-------------------|
| `DeepCWrapper` | Per-channel color pipeline: masking, unpremult/premult, mix, abort | Nuke runtime (DDImage), subclasses via virtual hooks |
| `DeepCMWrapper` | Extends wrapper for matte generation; adds operation knob (replace/union/mask/stencil/min/max) | `DeepCWrapper` (parent), subclasses |
| `DeepCPMatte` | Position-based spherical/cubic matte; owns GL handle drawing | `DeepCMWrapper` (parent), Nuke viewer (ViewerContext, OpenGL) |
| `DeepCShuffle` | Per-sample channel remapping; direct `DeepFilterOp` subclass | Nuke runtime only |
| `DeepDefocus` (new) | Depth-sorted circle-of-confusion scatter; deep-in → 2D-out | Nuke runtime via `Iop::engine()` + `DeepOp` input |
| `DeepBlur` (new) | Spatial gaussian/box blur on deep samples; deep-in → deep-out | Nuke runtime via `DeepFilterOp::doDeepEngine()` |
| `DeepThinner` (vendored) | External thin-deep algorithm; standalone plugin | Nuke runtime, CMake build only |
| FastNoise | Noise generation algorithms | `DeepCPNoise` only |
| CMake build system | Compiles plugins as MODULE targets; generates menu.py | Developer / CI |
| Python menu | Registers plugins into Nuke toolbar at startup | Nuke Python runtime |

---

## Architectural Patterns

### Pattern 1: Template Method via DeepCWrapper (existing, proven)

**What:** Base class owns the entire `doDeepEngine()` loop. Subclasses inject behavior at two points:
`wrappedPerSample()` (once per deep sample, channel-agnostic) and `wrappedPerChannel()` (once per
channel per sample).

**When to use:** Any plugin that applies a color transformation uniformly across the requested
channel set, with optional masking and mix. This covers all 11 existing color-effect plugins.

**Trade-offs:**
- Pro: masking, unpremult, mix, abort-check for free
- Pro: consistent knob layout across all wrapper plugins
- Con: `perSampleData` is a single `float` — passing multi-value state from per-sample to
  per-channel requires workarounds (see DeepCHueShift's `Vector3& sampleColor` parameter reuse)
- Con: not suitable for spatial operations (needs the full input plane before per-pixel work)

**Example skeleton:**
```cpp
class DeepCFoo : public DeepCWrapper {
    void wrappedPerChannel(float inputVal, float perSampleData,
                           Channel z, float& outData, Vector3& sampleColor) override {
        outData = /* math on inputVal */;
    }
};
```

### Pattern 2: Direct DeepFilterOp Subclass (existing, for structural ops)

**What:** Plugin implements `_validate()`, `getDeepRequests()`, and `doDeepEngine()` directly,
without the wrapper pipeline. Full control over the processing loop.

**When to use:** Any plugin that is not a uniform color effect — structural changes (channel
routing, bbox ops, merge ops, spatial operations).

**New application — DeepBlur:** A spatial blur must read neighboring pixels' deep data before
writing the output plane. This requires expanding the requested bounding box in `getDeepRequests()`
(inflate the input Box by the blur radius in x and y), then using the full `DeepPlane` across the
expanded region. `DeepFilterOp` allows this; `DeepCWrapper` does not because its loop iterates
only over the output box. `DeepPixelOp` explicitly prohibits spatial operations. Therefore
`DeepBlur` must be a direct `DeepFilterOp` subclass.

**Trade-offs:**
- Pro: complete control, no wrapper overhead
- Con: must implement masking, mix, abort-check manually if wanted
- Con: more boilerplate

### Pattern 3: Iop with DeepOp Input (new for DeepDefocus)

**What:** A standard 2D `Iop` that accepts a `DeepOp` connection on input 0. The `engine()`
function (not `doDeepEngine()`) fetches a `DeepPlane` row-by-row via `deepIn->deepEngine(y, x, r,
channels, deepRow)`, then scatters or flattens samples to produce 2D output pixels.

**Why Iop and not DeepFilterOp:** `DeepFilterOp` always produces deep output. `DeepDefocus` must
produce 2D output (the scattered bokeh discs need to accumulate into a flat image, not remain as
discrete deep samples). The NDK explicitly supports this pattern: "It is possible to write a
regular 2D Iop that takes deep inputs."

**Required method overrides:**
```
test_input(int n, Op* op)   → dynamic_cast<DeepOp*>(op) != nullptr for input 0
default_input(int n)         → nullptr (deep input required)
_validate(bool)              → copy deepInfo from input; set info_ (output 2D info)
_request(int x, y, r, ...)  → call deepIn->deepRequest() on the deep input
engine(int y, x, r, cs, row)→ fetch DeepPlane via deepIn->deepEngine(y,x,r,...),
                               iterate samples, scatter/accumulate to row
```

**Trade-offs:**
- Pro: correct output type (2D), integrates naturally into Nuke's 2D node graph downstream
- Con: engine() is called per-scanline; accumulation across scanlines (e.g. for a disc that
  spans multiple rows) requires a full-frame intermediate buffer allocated in `_validate()` or
  computed row-by-row with a radius-constrained kernel

### Pattern 4: GL Handles for Viewport Interaction (existing in DeepCPMatte, to be improved)

**What:** Three virtual methods on `Op` enable custom OpenGL drawing in the Nuke viewer:
- `build_handles(ViewerContext* ctx)` — decides whether to add handles; calls
  `build_knob_handles(ctx)` for knob-driven handles and `add_draw_handle(ctx)` to register the
  custom draw callback
- `draw_handle(ViewerContext* ctx)` — executes OpenGL calls; anything drawn between
  `begin_handle()` / `end_handle()` is interactive (responds to mouse events)
- `knob_changed(Knob* k)` — reacts to knob changes that should move/update the handle

**Current state in DeepCPMatte:** Stubs exist (`build_handles`, `draw_handle`, `knob_changed`),
but the `draw_handle` implementation only draws a `GL_LINES` point at `_center` — a minimal
placeholder. The center XY knob's `knob_changed` already queries the deep input at the picked
pixel and sets the axis translate. The architecture is correct; it needs richer drawing (sphere
outline, radius indicator, falloff shell).

**Integration rules:**
- Must include `DDImage/ViewerContext.h` and `DDImage/gl.h`
- Must link the plugin module against `OpenGL::GL` in CMake (already done for DeepCPMatte via the
  `if (OpenGL_FOUND)` guard in `src/CMakeLists.txt`)
- `build_handles` should guard on `ctx->transform_mode() != VIEWER_2D` for 3D handles or the
  inverse for 2D handles — the existing guard is correct for 2D center picking
- Interactive dragging: wrap drawn geometry in `begin_handle(ctx, ...)`/`end_handle(ctx)` and
  respond to `ctx->event() == PUSH` / `DRAG` / `RELEASE` events in `draw_handle` or a registered
  static callback

---

## Data Flow

### Deep-in → Deep-out (Wrapper and Direct DeepFilterOp plugins)

```
Nuke scheduler
    │
    ▼ _validate(bool)
DeepCWrapper / DeepFilterOp
    │  validates channels, mask inputs, bbox
    │
    ▼ getDeepRequests(Box, ChannelSet, count, requests)
    │  pushes RequestData to upstream (expands Box for spatial ops)
    │
    ▼ doDeepEngine(Box, ChannelSet, DeepOutputPlane&)
    │
    ├─ input0()->deepEngine(box, neededChannels, deepInPlane)
    │     [fetches upstream DeepPlane into memory]
    │
    ├─ for each pixel in box:
    │   ├─ deepInPlane.getPixel(it) → DeepPixel
    │   ├─ for each sample:
    │   │   ├─ wrappedPerSample() [hook: matte/position math → perSampleData]
    │   │   └─ for each channel:
    │   │       └─ wrappedPerChannel() [hook: color math → outData]
    │   └─ write to DeepOutputPlane
    │
    ▼ DeepOutputPlane delivered to Nuke
```

For **DeepBlur**: `getDeepRequests()` expands the requested `Box` by ±blurRadius in x and y.
`doDeepEngine()` fetches the expanded `DeepPlane`, then for each output pixel samples the
surrounding deep pixels to compute weighted output samples.

### Deep-in → 2D-out (DeepDefocus — Iop pattern)

```
Nuke scheduler
    │
    ▼ _validate(bool)
DeepDefocus (Iop subclass)
    │  copies format/bbox from deepIn->deepInfo()
    │  sets info_.channels to output channel set (e.g. RGBA)
    │
    ▼ _request(int x, y, r, ChannelSet, count)
    │  deepIn->deepRequest(Box, channels, count)
    │
    ▼ engine(int y, int x, int r, ChannelSet, Row& row)
    │
    ├─ deepIn->deepEngine(y, x, r, neededChannels, deepRow)
    │     [fetches one scanline's worth of DeepPlane]
    │
    ├─ for each pixel x..r in the scanline:
    │   ├─ deepRow.getPixel(y, x) → DeepPixel
    │   ├─ for each sample (front-to-back, sorted by depth):
    │   │   ├─ compute circle-of-confusion radius from sample depth + focus params
    │   │   └─ splat sample's color into row (or accumulation buffer)
    │   └─ write accumulated result to row[channel][x]
    │
    ▼ Row delivered to Nuke (2D pixels)
```

Note: Row-by-row processing means a sample's CoC disc that spans multiple scanlines cannot be
trivially accumulated using only the current scanline's data. For v1 (CPU, correctness first),
the recommended approach is a full-frame pre-pass: in `_validate()`, allocate an RGBA buffer
sized to the output format; in `engine()` for y==bbox.y() (first scanline), scatter all samples
into the buffer; for subsequent scanlines, copy from the buffer. This is the standard approach
used by Nuke's own DeepToImage-style nodes.

### Plugin Registration Flow

```
CMake build
    │ compiles each .cpp as MODULE .so
    ├─ add_nuke_plugin(DeepCFoo DeepCFoo.cpp)
    ├─ [if wrapped] target_sources(... $<TARGET_OBJECTS:DeepCWrapper>)
    └─ [if gl] target_link_libraries(... OpenGL::GL)
    │
    ▼ Nuke startup
    init.py → menu.py → create_deepc_menu()
    │  nuke.menu("Nodes").addCommand("DeepC/DeepCFoo", ...)
    │
    ▼ User creates node
    Nuke dlopen("DeepCFoo.so")
    → finds Op::Description::d registered via static initializer
    → calls build(node) → new DeepCFoo(node)
```

---

## Recommended Project Structure (additions only)

The existing `src/` flat layout is correct. New files follow the same pattern:

```
src/
├── DeepCWrapper.h / .cpp       # existing — perSampleData interface redesign here
├── DeepCMWrapper.h / .cpp      # existing — remove Op::Description here
├── DeepCPMatte.cpp             # existing — GL handle improvements here
├── DeepCShuffle.cpp            # existing — channel routing UI improvements here
│
├── DeepCDefocus.cpp            # NEW — Iop subclass, deep-in → 2D-out
├── DeepCBlur.cpp               # NEW — DeepFilterOp direct subclass, spatial
│
└── [DeepThinner vendored dir]  # NEW — see vendoring section
    DeepThinner/
    ├── CMakeLists.txt          # minimal, wraps upstream source
    └── [upstream .cpp/.h]
```

---

## Integration Architecture for Each New Component

### 1. DeepDefocus — Iop with DeepOp Input

**Base class:** `DD::Image::Iop`
**CMake category:** `PLUGINS` (non-wrapped, direct NDK)
**NDK headers:** `DDImage/Iop.h`, `DDImage/Row.h`, `DDImage/DeepOp.h`, `DDImage/DeepFilterOp.h`

Key design choices:
- `test_input(0, op)` → `dynamic_cast<DeepOp*>(op) != nullptr`
- `default_input(0)` → `nullptr` (deep connection required)
- `_validate()` derives output `format()` and `bbox()` from `deepIn->deepInfo()`, sets
  `info_.set_channels(outputChannelSet)` for RGBA or user-selected channels
- v1 approach: allocate a per-validate frame buffer (`std::vector<float>` per channel) in
  `_validate()`, scatter all samples into it in the first `engine()` call, serve subsequent rows
  from the buffer — avoids multi-pass complexity while remaining correct
- Depth sort: deep samples must be processed front-to-back; use `getOrderedSample()` (which sorts
  by `Chan_DeepFront`) or sort manually if the input plane is unordered
- CoC radius per sample: `coc = abs(sampleDepth - focusDistance) * cocScale`; clamp to max radius
- Alpha compositing: over-composite samples front-to-back into the accumulation buffer using the
  standard deep compositing formula

**Confidence:** HIGH — pattern confirmed in Foundry official NDK docs (Deep to 2D Ops page)

### 2. DeepBlur — Direct DeepFilterOp Subclass

**Base class:** `DD::Image::DeepFilterOp`
**CMake category:** `PLUGINS` (non-wrapped, direct NDK)
**NDK headers:** `DDImage/DeepFilterOp.h`, `DDImage/Knobs.h`

Key design choices:
- Cannot use `DeepCWrapper` — the wrapper's `doDeepEngine()` fetches the input plane for exactly
  the output box; a blur needs an expanded input region
- `getDeepRequests()` must inflate the requested `Box` by the blur radius in both x and y:
  ```cpp
  Box expandedBox = box;
  expandedBox.pad(_blurRadius);
  requests.push_back(RequestData(input0(), expandedBox, neededChannels, count));
  ```
- `doDeepEngine()` fetches the expanded `DeepPlane`, then for each output pixel gathers the
  surrounding deep pixels, weights each neighbor sample by a Gaussian kernel based on XY distance,
  and writes weighted output samples. Output sample count equals input sample count at the center
  pixel (samples are not merged across pixels — this is a deep blur, not a flatten-then-blur).
- Deep blur semantics: apply kernel weights to per-channel values while preserving the sample
  structure of the center pixel. Neighboring samples are blended per-channel by their kernel
  weight contribution.
- Cannot use `DeepPixelOp` — NDK docs explicitly state DeepPixelOp "provide[s] no support for
  spatial operations: each pixel on the input corresponds exactly to a pixel on the output."

**Confidence:** HIGH for base class choice. MEDIUM for exact deep blur sample-merging semantics
(no official NDK guidance found; approach matches how 2D blur works but adapted to deep stacks).

### 3. GL Handles for DeepCPMatte — Improving Existing Plugin

**No base class change required** — `DeepCPMatte` already inherits `DeepCMWrapper` and already
has the correct method stubs (`build_handles`, `draw_handle`, `knob_changed`) plus the OpenGL
CMake linkage. This is a pure implementation improvement.

**Current state:**
- `build_handles()` — correct: calls `build_knob_handles(ctx)` + `add_draw_handle(ctx)`, guards
  on `VIEWER_2D` mode
- `draw_handle()` — placeholder: draws a `GL_LINES` single point at `_center`; no sphere, no
  radius indicator, no interactivity
- `knob_changed("center")` — functional: queries deep input at picked pixel, sets axis translate

**Target state (what to add):**
- Draw sphere outline (lat/lon lines) in `draw_handle()` using `glBegin(GL_LINE_LOOP)`
- Draw falloff shell at `1.0 / _falloff` scaled radius
- Wrap geometry in `begin_handle()` / `end_handle()` to make it mouse-interactive
- In `draw_handle()`, detect `ctx->event() == PUSH` to begin a drag; `DRAG` to update `_center`
  or axis knob values; `RELEASE` to commit
- For 3D viewer support: add a branch for `ctx->transform_mode() != VIEWER_2D` to draw the same
  geometry in 3D space using the axis matrix

**Confidence:** HIGH — pattern fully confirmed in existing DeepCPMatte code + Foundry NDK docs +
community tutorial (Erwan LeRoy's Part 4)

### 4. DeepCShuffle Improvements

**No base class change required** — `DeepCShuffle` is a direct `DeepFilterOp` subclass with
correct architecture. The improvements are UI-only (knob changes).

**Current limitation:** Four fixed `Input_Channel_knob` / `Channel_knob` pairs for in0→out0
through in3→out3. This does not match Nuke's built-in Shuffle node's UX (labeled input ports,
dynamic routing rows, Shuffle2 noodle style).

**Recommended approach:**
- For Nuke 12+ Shuffle2 parity: investigate `ShuffleLayer_knob()` or equivalent NDK knob type if
  it exists in the public API. If not public, use `Script_knob` to generate dynamic Python-driven
  UI or stick with the existing fixed 4-pair layout expanded to support more channels.
- Label the two inputs clearly via `input_label()` (already standard in DeepFilterOp subclasses)
- The processing logic in `doDeepEngine()` is correct and does not need changes for basic UI parity

**Confidence:** MEDIUM — NDK does not publicly document a "Shuffle2 noodle" style knob; needs
direct NDK header inspection or Nukepedia community research before committing to approach.

### 5. DeepThinner Vendor Integration

**Search result:** No public repository found for "bratgot/DeepThinner" or any "DeepThinner" Nuke
plugin. The project may be private, renamed, or not yet published.

**Assuming source is obtained, recommended CMake integration pattern:**

Option A (preferred for small, compile-once source): Copy source files into `src/DeepThinner/` or
directly into `src/`. Add to the `PLUGINS` list in `src/CMakeLists.txt`. Use `add_nuke_plugin()`.
This is exactly how FastNoise is handled — sources live in the tree, no separate CMake project.

Option B (for larger, complex dependency): Use `FetchContent` (CMake 3.14+, already within the
project's CMake 3.15+ floor) to pull from a remote URL at configure time:
```cmake
include(FetchContent)
FetchContent_Declare(DeepThinner
    GIT_REPOSITORY https://github.com/bratgot/DeepThinner.git
    GIT_TAG        main
)
FetchContent_MakeAvailable(DeepThinner)
```
Then link the DeepThinner target into the plugin module.

Avoid `ExternalProject_Add` — it builds at build time, not configure time, making target linking
complex. `FetchContent` is simpler and is the modern CMake idiom for vendoring.

**CMake list category:** Add to `PLUGINS` in `src/CMakeLists.txt` (non-wrapped, direct NDK).
**Menu category:** Likely `Util_NODES` or a new "Depth" category alongside DeepDefocus/DeepBlur.

**Confidence:** LOW — source availability unknown. Architecture recommendation is HIGH confidence
given the existing FastNoise/DeepCBlink vendoring patterns in the codebase.

---

## Codebase Sweep: Recommended Order of Concern Resolution

The five tech-debt items in `CONCERNS.md` have ordering dependencies. Address them in this order:

**Step 1 — `perSampleData` interface redesign (DeepCWrapper.h, DeepCWrapper.cpp, DeepCMWrapper)**

Do this first because it is a breaking change to the virtual interface. Every subclass that
overrides `wrappedPerSample()` or `wrappedPerChannel()` must be updated. Doing this before
adding DeepBlur or any other new wrapper-based plugin avoids a second round of updates.

Recommended signature change:
```cpp
// Before:
virtual void wrappedPerSample(..., float& perSampleData, Vector3& sampleColor);
virtual void wrappedPerChannel(float inputVal, float perSampleData, Channel z,
                               float& outData, Vector3& sampleColor);
// After:
virtual void wrappedPerSample(..., float* perSampleData, int perSampleDataLen,
                               Vector3& sampleColor);
virtual void wrappedPerChannel(float inputVal, const float* perSampleData,
                               int perSampleDataLen, Channel z,
                               float& outData, Vector3& sampleColor);
```
Default length = 1, keeping existing behavior for plugins that use the single float.

**Step 2 — Remove `Op::Description` from DeepCWrapper and DeepCMWrapper**

Remove the `build()` functions and `Op::Description` static members from `src/DeepCWrapper.cpp`
and `src/DeepCMWrapper.cpp`. These are abstract base classes that should never appear in Nuke's
node menu. This change is non-breaking to subclasses and has zero runtime risk.

**Step 3 — Extract grade coefficient utility**

After the interface is stabilised (Step 1), extract the duplicated `A[]`, `B[]`, `G[]` arrays
and `_validate` precompute block from `DeepCGrade` and `DeepCPNoise` into a shared utility struct
(e.g. `DeepCGradeCoefficients`) in a new header included by both. This is purely a code quality
improvement with no API impact.

**Step 4 — Fix known bugs**

In dependency order:
1. `DeepCGrade` reverse gamma (one-line fix, isolated)
2. `DeepCKeymix` A/B containment bug (one-line fix, isolated)
3. `DeepCSaturation` / `DeepCHueShift` divide-by-zero (add alpha guard in `wrappedPerSample`)
4. `DeepCID` unused loop variable (rename or restructure loop)
5. `DeepCConstant` comma operator (replace with explicit lerp call)

**Step 5 — DeepCBlink decision**

Evaluate whether to complete or remove `DeepCBlink`. If removing: delete the file, remove from
`PLUGINS` list, remove from `DRAW_NODES`, remove `target_link_libraries(DeepCBlink ...)`. If
keeping: fix GPU path, fix memory leaks (replace `calloc`/`free` with `std::vector`), handle
all channel counts.

---

## Anti-Patterns

### Anti-Pattern 1: Using DeepPixelOp for DeepBlur

**What people do:** Inherit from `DeepPixelOp` because `processSample()` looks like the right
per-sample hook.
**Why it's wrong:** NDK docs state explicitly that `DeepPixelOp` "provide[s] no support for
spatial operations: each pixel on the input corresponds exactly to a pixel on the output." A blur
that reads neighboring pixels will simply not have access to them.
**Do this instead:** Direct `DeepFilterOp` subclass with expanded `getDeepRequests()` bbox.

### Anti-Pattern 2: Using DeepFilterOp for DeepDefocus

**What people do:** Try to produce 2D scattered output from a `DeepFilterOp` by using a "flat"
output channel.
**Why it's wrong:** `DeepFilterOp` always outputs a `DeepOutputPlane`. There is no way to produce
a true 2D composited result (accumulated over depth) from this base class.
**Do this instead:** `Iop` subclass with `test_input()` accepting `DeepOp*`; output via
`engine()`'s `Row&` parameter.

### Anti-Pattern 3: Using DeepCWrapper for DeepBlur

**What people do:** Attempt to reuse the wrapper pipeline (free masking, mix) by inheriting
`DeepCWrapper` and doing kernel math in `wrappedPerChannel()`.
**Why it's wrong:** The wrapper's `doDeepEngine()` fetches the input plane for exactly the output
box with no expansion. Neighboring pixels outside the output box are not available.
**Do this instead:** Direct `DeepFilterOp` subclass. Implement masking and mix manually if needed,
or leave them out for v1.

### Anti-Pattern 4: Registering Base Classes as Nuke Nodes

**What currently exists:** `DeepCWrapper` and `DeepCMWrapper` have `Op::Description` objects that
make them appear as instantiatable nodes in Nuke.
**Why it's wrong:** These are abstract base classes. Instantiating them applies a no-op gain of
1.0 with no meaningful operation, confusing artists.
**Do this instead:** Remove `Op::Description` and `build()` from both files. See codebase sweep
Step 2 above.

### Anti-Pattern 5: ExternalProject_Add for small vendored NDK plugins

**What people do:** Use `ExternalProject_Add` to pull and build DeepThinner as a separate CMake
build at build time.
**Why it's wrong:** `ExternalProject_Add` targets are not available at configure time, so you
cannot link directly against them in other `add_nuke_plugin()` targets without complex import
chains. The build system becomes hard to understand.
**Do this instead:** `FetchContent_Declare` / `FetchContent_MakeAvailable` (configure-time) for
remote source, or simply copy source files into the tree (as done for FastNoise).

---

## Integration Points

### Internal Boundaries

| Boundary | Communication | Notes |
|----------|---------------|-------|
| Plugin → DeepCWrapper | C++ inheritance + virtual function calls | Interface change (perSampleData) is breaking — do first |
| DeepCWrapper → DeepCMWrapper | C++ inheritance | DeepCMWrapper overrides `wrappedPerSample` / `wrappedPerChannel` |
| DeepCPMatte → Nuke Viewer | OpenGL calls via `draw_handle()` + `ViewerContext` | Requires `OpenGL::GL` CMake link; already present |
| DeepDefocus → DeepOp input | `dynamic_cast<DeepOp*>(Op::input(0))` + `deepIn->deepEngine()` | Standard NDK deep-to-2D pattern |
| DeepBlur → deep input | `input0()->deepEngine(expandedBox, ...)` | Box must be expanded in `getDeepRequests()` |
| CMake → DeepThinner | `FetchContent` or in-tree sources | Mirrors existing FastNoise integration |
| menu.py ↔ CMake | CMake configures `menu.py.in`, injecting plugin name lists | Add new plugins to correct category lists in `src/CMakeLists.txt` |

### External Dependencies

| Dependency | Integration Pattern | Notes |
|------------|---------------------|-------|
| Foundry Nuke NDK | Headers + DDImage link at build time | `FindNuke.cmake` already handles this |
| OpenGL | `find_package(OpenGL)` + `target_link_libraries(... OpenGL::GL)` | Already gated on `OpenGL_FOUND`; DeepPMatte already linked |
| FastNoise | In-tree OBJECT library | Existing pattern for DeepCPNoise |
| DeepThinner | In-tree (preferred) or `FetchContent` | Source availability TBD |

---

## Build Order Implications for Roadmap Phases

The dependency graph between the five feature areas determines safe phase ordering:

```
Phase N: Codebase Sweep (must come first)
    └─ perSampleData interface redesign         [blocks: any new wrapper-based plugin]
    └─ Remove base class Op::Description        [no blockers]
    └─ Grade coefficient extraction             [no blockers beyond perSampleData being stable]
    └─ Known bug fixes                          [no blockers]
    └─ DeepCBlink decision                      [no blockers]

Phase N+1: DeepCShuffle UI improvements         [no dependencies on sweep]
Phase N+1: GL handles for DeepCPMatte           [no dependencies on sweep; isolated to one file]

Phase N+2: DeepBlur                             [depends on: perSampleData if wrapper approach needed
                                                  (it doesn't — direct DeepFilterOp, so independent)]
Phase N+2: DeepDefocus                          [fully independent of sweep and wrappers]

Phase N+3: DeepThinner vendor integration       [fully independent; blocked only by source availability]
```

All Phase N+2 and N+3 items can be developed in parallel once Phase N is complete. The sweep
is the only true dependency gate because the `perSampleData` interface change touches all
`PLUGINS_WRAPPED` and `PLUGINS_MWRAPPED` source files.

---

## Sources

- [NDK Developers Guide: Deep to 2D Ops](https://learn.foundry.com/nuke/developers/11.2/ndkdevguide/deep/deepto2d.html) — HIGH confidence (official Foundry docs)
- [NDK Developers Guide: Simple DeepPixelOp](https://learn.foundry.com/nuke/developers/11.2/ndkdevguide/deep/deepsimple.html) — HIGH confidence (confirms spatial op prohibition)
- [NDK Developers Guide: Basic DeepOps](https://learn.foundry.com/nuke/developers/140/ndkdevguide/deep/deep.html) — HIGH confidence (Nuke 14 current)
- [Writing Nuke C++ Plugins Part 4 — GL Handles](https://erwanleroy.com/writing-nuke-c-plugins-ndk-part-4-custom-knobs-gl-handles-and-the-table-knob/) — MEDIUM confidence (community tutorial, confirmed against DeepCPMatte source)
- [DeepC codebase: `src/DeepCPMatte.cpp`](../codebase/ARCHITECTURE.md) — HIGH confidence (direct inspection)
- [DeepC codebase: `src/DeepCWrapper.h`, `src/DeepCWrapper.cpp`](../codebase/ARCHITECTURE.md) — HIGH confidence (direct inspection)
- [DeepC codebase: `src/DeepCShuffle.cpp`](../codebase/ARCHITECTURE.md) — HIGH confidence (direct inspection)
- [CMake ExternalProject docs](https://cmake.org/cmake/help/latest/module/ExternalProject.html) — HIGH confidence (official CMake docs)
- DeepThinner source availability — NOT FOUND (search returned nothing; treat as LOW confidence unknown)

---

*Architecture research for: DeepC Nuke NDK Plugin Suite — milestone feature additions*
*Researched: 2026-03-12*
