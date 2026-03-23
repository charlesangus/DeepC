# Architecture

**Analysis Date:** 2026-03-12

## Pattern Overview

**Overall:** Nuke NDK Plugin Suite with Inheritance-Based Template Method Pattern

**Key Characteristics:**
- Each plugin is a self-contained C++ shared library (`.so`/`.dylib`) loaded by Foundry's Nuke
- Shared behavior is factored into two reusable base class hierarchies (`DeepCWrapper` and `DeepCMWrapper`)
- The "template method" pattern governs processing: base classes own the pipeline loop; subclasses override specific virtual hooks
- Plugins are registered with Nuke via a static `Op::Description` object and a factory function
- A generated Python file (`menu.py`) registers all plugins into Nuke's toolbar at runtime

## Layers

**NDK Base Layer (Foundry DDImage):**
- Purpose: Provides Nuke's node graph, deep pixel, channel, and knob abstractions
- Location: External — Nuke installation headers under `DDImage/`
- Contains: `DeepFilterOp`, `DeepPixelOp`, `Iop`, `ChannelSet`, `Channel`, `Knob_Callback`, `Box`, `DeepPixel`, etc.
- Depends on: Nuke runtime
- Used by: All plugin source files

**Shared Wrapper Library Layer:**
- Purpose: Provides reusable base classes that manage the common deep-image processing pipeline (channel selection, masking, unpremult/premult, mix, per-sample loop)
- Location: `src/DeepCWrapper.h`, `src/DeepCWrapper.cpp`, `src/DeepCMWrapper.h`, `src/DeepCMWrapper.cpp`
- Contains: `DeepCWrapper` (color-effect base) and `DeepCMWrapper` (matte-generation base, extends `DeepCWrapper`)
- Depends on: `DDImage/DeepFilterOp.h` and related Nuke headers
- Used by: All PLUGINS_WRAPPED and PLUGINS_MWRAPPED plugin targets

**Plugin Layer:**
- Purpose: Implements each individual deep-compositing tool; only contains the unique math and knob definitions
- Location: `src/DeepC*.cpp` (one file per plugin, 22 plugins total)
- Contains: A single C++ class per file that inherits from `DeepCWrapper`, `DeepCMWrapper`, or directly from `DeepFilterOp`/`DeepPixelOp`
- Depends on: Wrapper library layer and/or Nuke NDK
- Used by: Nuke at runtime when nodes are instantiated

**Third-Party Library Layer:**
- Purpose: Provides 4D noise generation for `DeepCPNoise`
- Location: `FastNoise/FastNoise.cpp`, `FastNoise/FastNoise.h`
- Contains: `FastNoise` class with simplex, cellular, cubic, Perlin, and value noise types
- Depends on: Nothing (standalone)
- Used by: `src/DeepCPNoise.cpp` only

**Build System Layer:**
- Purpose: Discovers Nuke installation, configures compiler flags, builds all plugins as modules, generates the Python menu file, and produces an install layout
- Location: `CMakeLists.txt` (root), `src/CMakeLists.txt`, `cmake/FindNuke.cmake`
- Contains: Custom `FindNuke.cmake` module; categorized plugin lists; `add_nuke_plugin()` helper function
- Depends on: CMake 3.15+, Nuke installation, optional OpenGL and Blink
- Used by: Developers and CI build processes

**Python Integration Layer:**
- Purpose: Registers plugins into Nuke's node toolbar at startup
- Location: `python/init.py`, `python/menu.py.in` (template, filled by CMake at build time)
- Contains: `create_deepc_menu()` function that builds the DeepC submenu with categorized entries
- Depends on: `nuke` Python module (Nuke runtime)
- Used by: Nuke at startup via `init.py`

## Data Flow

**Per-Node Execution Flow (Wrapper Plugins):**

1. Nuke calls `_validate(bool)` on the node — base class validates masking inputs, calls `findNeededDeepChannels()`, stores all required channel sets
2. Nuke calls `getDeepRequests()` — base class adds needed channels to the upstream request
3. Nuke calls `doDeepEngine(Box, ChannelSet, DeepOutputPlane&)` on the base class — this is the main processing loop
4. Base class fetches the input `DeepPlane` from `input0()` and optionally fetches a mask row from `input1()` (the 2D mask `Iop`)
5. For each pixel in the bounding box, the base class iterates samples
6. Per sample: reads alpha, evaluates deep mask and side mask values, calls `wrappedPerSample()` (subclass hook for per-sample, channel-agnostic data such as a position-based matte)
7. Per channel: calls `wrappedPerChannel()` (subclass hook for the actual color math), then applies unpremult → math → mix/mask → repremult
8. Output samples are written to `DeepOutputPlane`

**Non-Wrapper Plugin Execution Flow:**

For plugins that inherit directly from `DeepFilterOp` or `DeepPixelOp` (`DeepCAddChannels`, `DeepCKeymix`, `DeepCConstant`, `DeepCWorld`, etc.), each plugin implements `_validate()`, `getDeepRequests()`, `doDeepEngine()` (or `doDeepPixelEngine()`) independently.

**Plugin Registration:**

Each plugin file ends with:
```cpp
static Op* build(Node* node) { return new PluginClassName(node); }
const Op::Description PluginClassName::d("PluginClassName", 0, build);
```
This is how Nuke discovers and instantiates the plugin from its shared library.

**Python Menu Registration:**

1. CMake configures `python/menu.py.in` using the categorized node lists defined in `src/CMakeLists.txt`
2. The generated `menu.py` is installed alongside the plugin binaries
3. Nuke's `init.py` runs `create_deepc_menu()` at startup, inserting the DeepC submenu into Nuke's Nodes toolbar

## Key Abstractions

**`DeepCWrapper` (color-effect base class):**
- Purpose: Manages the complete deep-pixel processing loop including masking, unpremult/premult, mix, and abort checking; exposes two virtual hooks for subclasses
- Examples: `src/DeepCWrapper.h`, `src/DeepCWrapper.cpp`
- Pattern: Template Method — `doDeepEngine()` is the invariant algorithm; `wrappedPerSample()` and `wrappedPerChannel()` are the variant hooks
- Knobs provided: `channels` (ChannelSet), `unpremult` (bool), `deep_mask` (Channel), `invert_deep_mask`, `unpremult_deep_mask`, `side_mask` (Channel), `invert_mask`, `mix` (float)
- Inputs: input 0 = deep image, input 1 = 2D mask `Iop`

**`DeepCMWrapper` (matte-generation base class, extends `DeepCWrapper`):**
- Purpose: Extends the wrapper pattern for plugins that generate per-sample matte data from an auxiliary channel (e.g., position or ID); adds an `operation` knob (replace/union/mask/stencil/out/min/max) controlling how the computed matte is composited into the output channel
- Examples: `src/DeepCMWrapper.h`, `src/DeepCMWrapper.cpp`
- Pattern: Template Method — overrides `wrappedPerSample()` and `wrappedPerChannel()`, which subclasses override again
- Also adds channels to `_deepInfo` in `_validate()` to extend the stream with new output channels

**`add_nuke_plugin()` CMake function:**
- Purpose: Defines the standard module build target for a Nuke plugin (shared library, no prefix, linked to `DDImage`)
- Location: `src/CMakeLists.txt` lines 50–58
- Pattern: Reusable CMake function called in loops over `PLUGINS`, `PLUGINS_WRAPPED`, and `PLUGINS_MWRAPPED` lists

## Entry Points

**`src/DeepCWrapper.cpp` / `src/DeepCMWrapper.cpp`:**
- Compiled as OBJECT libraries (`DeepCWrapper`, `DeepCMWrapper`) and linked into each wrapped plugin module
- Not standalone plugins; provide shared implementation

**Individual plugin `.cpp` files (`src/DeepC*.cpp`):**
- Each compiled as a separate shared module by `add_nuke_plugin()`
- Nuke's plugin loader discovers each `.so`/`.dylib` by name and calls the registered factory function
- Triggers: Nuke loading the plugin directory at startup, or when the user creates a node

**`python/init.py`:**
- Triggers: Nuke startup (`init.py` is executed automatically by Nuke when found in the plugin path)
- Responsibilities: Sources the generated `menu.py` (which calls `create_deepc_menu()`)

**`CMakeLists.txt` (root):**
- Triggers: Developer runs `cmake` followed by `make install`
- Responsibilities: Configures the entire build, discovers Nuke, sets C++ standard based on Nuke version (C++14 for < Nuke 15, C++17 for >= Nuke 15), generates `menu.py`, produces installable plugin binaries and icons

**`batchInstall.sh`:**
- Triggers: Developer runs manually on Linux or Windows (MINGW)
- Responsibilities: Iterates over all Nuke installations in a given directory, builds and installs for each version, creates zip archives for release

## Error Handling

**Strategy:** Minimal explicit error handling; relies on Nuke's framework and C++ assertions

**Patterns:**
- `mFnAssert(inPlaceOutPlane.isComplete())` — assertion that the output plane is complete before returning (in `DeepCWrapper::doDeepEngine()`, `src/DeepCWrapper.cpp`)
- `Op::aborted()` checked inside the pixel loop to bail fast on user-interrupt (`src/DeepCWrapper.cpp` line 148)
- `if (!input0()) return true;` — silent no-op when no deep input is connected
- Channel availability checks: `if (available.contains(Chan_Alpha))` before reading alpha
- Deep mask channel set to zero when channel is unavailable rather than crashing

## Cross-Cutting Concerns

**Logging:** None — no logging framework used; NDK does not expose a logging API at the plugin level

**Validation:** Each plugin implements `_validate(bool for_real)` called by Nuke before processing; base class validates masking state and channel sets; subclasses call `DeepCWrapper::_validate(for_real)` first and then add their own validation

**Authentication:** Not applicable — this is a Nuke plugin suite with no networking or access control

---

*Architecture analysis: 2026-03-12*
