# Codebase Structure

**Analysis Date:** 2026-03-12

## Directory Layout

```
DeepC/                          # Project root
├── CMakeLists.txt              # Root CMake build entry point
├── README.md                   # Project documentation and build instructions
├── LICENSE                     # GPL-3.0 license
├── batchInstall.sh             # Multi-version batch build/install script (Linux + Windows)
├── .gitignore                  # Git ignore rules
│
├── src/                        # All C++ plugin source code
│   ├── CMakeLists.txt          # Plugin build targets and install configuration
│   ├── DeepCWrapper.h          # Base class header: color-effect wrapper
│   ├── DeepCWrapper.cpp        # Base class implementation: core processing loop
│   ├── DeepCMWrapper.h         # Base class header: matte-generation wrapper (extends DeepCWrapper)
│   ├── DeepCMWrapper.cpp       # Base class implementation
│   └── DeepC*.cpp              # One file per plugin (22 plugin implementations)
│
├── FastNoise/                  # Vendored third-party noise library
│   ├── FastNoise.h             # FastNoise class header
│   ├── FastNoise.cpp           # FastNoise implementation (~100KB)
│   ├── Makefile                # Standalone makefile (not used by main build)
│   ├── LICENSE                 # FastNoise license
│   └── README.md               # FastNoise documentation
│
├── cmake/                      # CMake helper modules
│   └── FindNuke.cmake          # Custom CMake find module for Nuke installation
│
├── python/                     # Python integration files
│   ├── init.py                 # Nuke startup script (loaded by Nuke automatically)
│   └── menu.py.in              # CMake template for toolbar menu registration
│
├── icons/                      # Plugin toolbar icons
│   └── DeepC*.png              # One PNG icon per plugin (plus DeepC.png for menu root)
│
└── examples/                   # Example usage materials
    └── images/                 # Example images
```

## Directory Purposes

**`src/`:**
- Purpose: All C++ source code for the plugin suite
- Contains: Two shared base class pairs (`DeepCWrapper`, `DeepCMWrapper`) and 22 individual plugin `.cpp` files
- Key files:
  - `src/DeepCWrapper.h` / `src/DeepCWrapper.cpp` — core wrapper base class
  - `src/DeepCMWrapper.h` / `src/DeepCMWrapper.cpp` — matte-generation base class
  - `src/CMakeLists.txt` — defines `PLUGINS`, `PLUGINS_WRAPPED`, `PLUGINS_MWRAPPED` lists and all build targets

**`FastNoise/`:**
- Purpose: Vendored copy of the FastNoise library used exclusively by `DeepCPNoise`
- Contains: A single header/implementation pair providing simplex, cellular, cubic, Perlin, and value noise
- Generated: No — committed source
- Do not modify: Treat as a third-party vendored dependency

**`cmake/`:**
- Purpose: Custom CMake find modules
- Contains: `FindNuke.cmake` — locates Nuke installation, extracts version numbers, sets `NUKE_DDIMAGE_LIBRARY`, `NUKE_INCLUDE_DIRS`, etc.
- Key file: `cmake/FindNuke.cmake`

**`python/`:**
- Purpose: Nuke Python integration for toolbar menu registration
- Contains: `init.py` (Nuke startup hook) and `menu.py.in` (CMake template)
- At build time: CMake substitutes `@DRAW_NODES@`, `@COLOR_NODES@`, etc. into `menu.py.in` to produce `menu.py`
- At runtime: Nuke sources `init.py`, which calls `create_deepc_menu()` from the generated `menu.py`

**`icons/`:**
- Purpose: PNG icons for each plugin displayed in Nuke's toolbar
- Naming: `DeepC<PluginName>.png` for each plugin; `DeepC.png` for the top-level menu
- Installed to: `icons/` subdirectory of the install prefix

**`examples/`:**
- Purpose: Example materials for testing and demonstrating plugins
- Contains: Reference images

## Key File Locations

**Entry Points:**
- `CMakeLists.txt`: Root build configuration; start here for build system changes
- `src/CMakeLists.txt`: Plugin registration, category assignments, build target definitions
- `python/init.py`: Nuke startup hook
- `python/menu.py.in`: Toolbar menu template (CMake-generated)

**Core Abstractions:**
- `src/DeepCWrapper.h`: Interface for all color-effect plugins
- `src/DeepCWrapper.cpp`: Template method loop — all masking, unpremult, mix logic lives here
- `src/DeepCMWrapper.h`: Interface for matte-generation plugins
- `src/DeepCMWrapper.cpp`: Matte blend operation logic

**Build Infrastructure:**
- `cmake/FindNuke.cmake`: Nuke discovery logic; edit here to add new Nuke version support
- `batchInstall.sh`: Multi-version release builder

## Naming Conventions

**Files:**
- Plugin implementation files: `DeepC<NodeName>.cpp` — PascalCase after the `DeepC` prefix (e.g., `DeepCGrade.cpp`, `DeepCPNoise.cpp`)
- Base class pairs: header and implementation share the same stem (e.g., `DeepCWrapper.h` / `DeepCWrapper.cpp`)
- Icons: `DeepC<NodeName>.png` — matches the plugin class name exactly

**C++ Classes:**
- Plugin classes: named identically to the file stem and the Nuke node name (e.g., class `DeepCGrade` in `DeepCGrade.cpp`)
- Base classes: `DeepCWrapper`, `DeepCMWrapper`

**C++ Members:**
- Private/protected member variables: prefixed with `_` (single underscore), e.g., `_processChannelSet`, `_doDeepMask`
- Function and variable names: lowerCamelCase, e.g., `wrappedPerChannel`, `perSampleData`

**Nuke Node Names:**
- Match the C++ class name exactly — this is the string passed to `Op::Description` and used in `nuke.createNode('DeepCGrade')`

**CMake Variables:**
- Plugin category lists: UPPER_SNAKE_CASE with `_NODES` suffix (e.g., `COLOR_NODES`, `DRAW_NODES`, `Util_NODES` — note `Util_NODES` uses mixed case, an inconsistency in the existing code)

## Where to Add New Code

**New Color-Effect Plugin (wraps per-channel math):**
1. Create `src/DeepC<NodeName>.cpp` — inherit from `DeepCWrapper`
2. Override `wrappedPerChannel()` with the color math
3. Override `custom_knobs()` to add plugin-specific knobs
4. Add the class name to `PLUGINS_WRAPPED` list in `src/CMakeLists.txt`
5. Add the node name to the appropriate category list (`COLOR_NODES`, etc.) in `src/CMakeLists.txt`
6. Add `DeepC<NodeName>.png` to `icons/`

**New Matte-Generation Plugin (generates per-sample mask from auxiliary channel data):**
1. Create `src/DeepC<NodeName>.cpp` — inherit from `DeepCMWrapper`
2. Override `wrappedPerSample()` to compute matte value from auxiliary channel data
3. Override `custom_knobs()` for plugin-specific knobs; optionally override `top_knobs()`
4. Add the class name to `PLUGINS_MWRAPPED` list in `src/CMakeLists.txt`
5. Add the node name to the appropriate category list in `src/CMakeLists.txt`
6. Add icon to `icons/`

**New Standalone Plugin (direct NDK subclass, no wrapper):**
1. Create `src/DeepC<NodeName>.cpp` — inherit directly from `DeepFilterOp` or `DeepPixelOp`
2. Implement `_validate()`, `getDeepRequests()`, and `doDeepEngine()` / `doDeepPixelEngine()` in full
3. Add to `PLUGINS` list in `src/CMakeLists.txt`
4. Add to category list; add icon

**Utilities / Shared Helpers:**
- Shared C++ utilities should be added to `src/DeepCWrapper.cpp` or `src/DeepCMWrapper.cpp` if they are general enough, or to a new `src/DeepCUtils.h` header if standalone
- There is currently no dedicated utilities header; do not put shared logic in individual plugin files

## Special Directories

**`FastNoise/`:**
- Purpose: Vendored third-party noise library
- Generated: No
- Committed: Yes — treat as read-only vendor code; do not modify

**`.planning/`:**
- Purpose: GSD planning documents (codebase analysis, phase plans)
- Generated: Yes (by GSD tooling)
- Committed: Yes

**`build/` (created at build time):**
- Purpose: Out-of-source CMake build directory
- Generated: Yes
- Committed: No (in `.gitignore`)

**`install/` (created at build time):**
- Purpose: CMake install output — contains compiled `.so`/`.dylib` files, `init.py`, `menu.py`, and icons ready to deploy into a Nuke plugin path
- Generated: Yes
- Committed: No

**`release/` (created by `batchInstall.sh`):**
- Purpose: Zip archives of per-Nuke-version installs for distribution
- Generated: Yes
- Committed: No

---

*Structure analysis: 2026-03-12*
