# Phase 7: DeepThinner Integration - Research

**Researched:** 2026-03-18
**Domain:** Nuke C++ plugin vendoring, CMake build integration, Python menu registration
**Confidence:** HIGH

## Summary

DeepThinner is a single-file MIT-licensed Nuke plugin by Marten Blumen that reduces deep-pixel sample counts through seven configurable optimization passes. It inherits from Nuke's `DeepFilterOp` base class — the same base used by simpler non-wrapped plugins already in this project (e.g., `DeepCConstant`, `DeepCAddChannels`). The source file is a self-contained `.cpp` with no third-party dependencies beyond DDImage.

The vendoring task is architecturally straightforward: copy the upstream `.cpp` into `src/`, add `DeepThinner` to the `PLUGINS` list in `src/CMakeLists.txt` (the non-wrapped list, since it uses `DeepFilterOp` directly), add a new `FILTER_NODES` CMake variable (or reuse an existing category), wire it into `menu.py.in`, and verify both build targets. The `docker-build.sh` requires no changes — it builds everything CMake produces.

The one mandatory constraint is that the upstream MIT license comment header inside `DeepThinner.cpp` must remain intact and unmodified. The project decision log explicitly forbids adapting DeepThinner to the `DeepCWrapper`/`DeepCMWrapper` base classes.

**Primary recommendation:** Copy `DeepThinner.cpp` verbatim into `src/`, add it to `PLUGINS` in `src/CMakeLists.txt`, add a new `FILTER_NODES` CMake variable for menu categorization, update `menu.py.in` to include a "Filter" submenu, install the icon, and run `docker-build.sh --linux` to confirm before the full dual-platform build.

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| THIN-01 | DeepThinner.cpp vendored into `src/` with upstream MIT license header comment preserved | Upstream file is a single self-contained `.cpp`; copy verbatim, no adaptation needed |
| THIN-02 | DeepThinner added to CMake `PLUGINS` list and builds as `.so`/`.dll` | `DeepFilterOp`-based plugins use the `PLUGINS` list and the existing `add_nuke_plugin` function; no additional link deps required |
| THIN-03 | DeepThinner wired into `menu.py.in` for Deep toolbar registration | Upstream registers into `Deep/DeepThinner` via `Op::Description`; project menu uses CMake-generated lists in `menu.py.in` — requires a new or existing menu category variable |
| THIN-04 | DeepThinner builds successfully via `docker-build.sh` for Linux and Windows | `docker-build.sh` invokes CMake with no plugin-specific flags; once THIN-02 is done, THIN-04 follows automatically |
</phase_requirements>

---

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| DDImage (Nuke SDK) | 16.x | Base classes `DeepFilterOp`, `Op::Description`, deep pixel API | The only dependency for all existing DeepC plugins |
| CMake | 3.15+ | Build system already in use project-wide | Matches existing `cmake_minimum_required` in root `CMakeLists.txt` |

### Supporting
| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| None | — | DeepThinner has no third-party dependencies | N/A |

**Installation:** No new packages required. DeepThinner depends only on DDImage which is already provided by the NukeDockerBuild containers.

---

## Architecture Patterns

### How the Existing Plugin Build Works

The project has three plugin categories in `src/CMakeLists.txt`:

| CMake List | Base Class | Example |
|------------|-----------|---------|
| `PLUGINS` | `DeepFilterOp` or direct Op | `DeepCConstant`, `DeepCAddChannels` |
| `PLUGINS_WRAPPED` | `DeepCWrapper` | `DeepCGrade`, `DeepCAdd` |
| `PLUGINS_MWRAPPED` | `DeepCMWrapper` | `DeepCPNoise`, `DeepCID` |

`add_nuke_plugin` is the CMake function used for all three categories. It creates a MODULE library, sets no prefix, and links DDImage. DeepThinner belongs in `PLUGINS` (the non-wrapped category) because it inherits directly from `DeepFilterOp`.

### Menu Registration Pattern

`menu.py.in` is a CMake template. CMake variables (e.g., `@DRAW_NODES@`) are lists of plugin names, joined into Python string literals by CMake's `string(REPLACE ";" ...)` command at configure time.

The existing categories in `menu.py.in`:

| Variable | Menu Label | Current members |
|----------|-----------|-----------------|
| `DRAW_NODES` | Draw | DeepCConstant, DeepCID, DeepCPMatte, DeepCPNoise |
| `CHANNEL_NODES` | Channel | DeepCAddChannels, DeepCRemoveChannels, DeepCShuffle2 |
| `COLOR_NODES` | Color | DeepCAdd, DeepCClamp, DeepCColorLookup, etc. |
| `3D_NODES` | 3D | DeepCWorld |
| `MERGE_NODES` | Merge | DeepCKeymix |
| `Util_NODES` | Util | DeepCAdjustBBox, DeepCCopyBBox |

DeepThinner is a sample-reduction/filtering utility. It does not belong in any existing category cleanly. The best fit is a new `FILTER_NODES` list with a "Filter" submenu in the DeepC menu. This follows the same pattern used for every other category.

### Pattern: Adding a New Category

**Step 1 — `src/CMakeLists.txt`:**
```cmake
# Add to PLUGINS list
set(PLUGINS
    ...existing entries...
    DeepThinner
)

# New category variable for menu generation
set(FILTER_NODES DeepThinner)
```

**Step 2 — CMake string substitution and configure_file call (already exists, just add the new variable):**
```cmake
string(REPLACE ";" "\", \"" FILTER_NODES "\"${FILTER_NODES}\"")
configure_file(../python/menu.py.in menu.py)
```

**Step 3 — `menu.py.in`:**
```python
menus = {
    ...existing entries...
    "Filter": {"icon":"ToolbarFilter.png",
               "nodes": [@FILTER_NODES@]},
}
```

Note: `ToolbarFilter.png` is a standard Nuke bundled icon. Any Nuke toolbar icon name works here since it references the Nuke install, not the DeepC icons folder.

### Icon Pattern

Every plugin in `icons/` has a `DeepC<PluginName>.png` file. The icon is referenced in `menu.py` as `"{}.png".format(node)`. DeepThinner does not follow the `DeepC` prefix convention — its icon file should be `DeepThinner.png`. The root `CMakeLists.txt` installs icons with `file(GLOB ICONS "icons/DeepC*.png")` — this glob will **not** pick up `DeepThinner.png` if it is not prefixed `DeepC`.

**Resolution:** Either rename the icon to `DeepThinner.png` and add a separate install rule for non-DeepC icons, or install it via an explicit `install(FILES ...)` statement. The simpler solution is an explicit install in the root `CMakeLists.txt`.

If no icon is provided, `menu.py` will still work — Nuke silently falls back to no icon. An icon is nice-to-have, not required for functional registration.

### Anti-Patterns to Avoid

- **Adapting DeepThinner to DeepCWrapper/DeepCMWrapper:** Explicitly out of scope per project decisions. DeepThinner is a standalone `DeepFilterOp` implementation — use `PLUGINS`, not `PLUGINS_WRAPPED` or `PLUGINS_MWRAPPED`.
- **Modifying `DeepThinner.cpp` content:** The MIT license header must be preserved intact. Any modification risks breaking the THIN-01 success criterion.
- **Modifying `docker-build.sh`:** The script requires no changes. It passes CMake the source tree; CMake drives what gets built.
- **Adding DeepThinner to the existing glob for icons:** `file(GLOB ICONS "icons/DeepC*.png")` only picks up the `DeepC` prefix. A `DeepThinner.png` icon needs an explicit install rule or the glob pattern updated.

---

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Deep sample optimization | Custom sample-thinning algorithm | DeepThinner.cpp vendored as-is | Upstream is stable and well-tested; the whole point of this phase is vendoring |
| Plugin build boilerplate | Custom CMake function | Existing `add_nuke_plugin()` | Already handles MODULE, prefix stripping, DDImage link, Windows flags |
| Menu registration | Custom Python loader | Existing `menu.py.in` + CMake configure pattern | Consistent with all 22 other plugins |

---

## Common Pitfalls

### Pitfall 1: Icon Glob Miss
**What goes wrong:** `DeepThinner.png` is placed in `icons/` but not installed into the release archive because `file(GLOB ICONS "icons/DeepC*.png")` only matches files starting with `DeepC`.
**Why it happens:** The glob pattern was written before any non-`DeepC`-prefixed plugins existed.
**How to avoid:** Add an explicit `install(FILES icons/DeepThinner.png DESTINATION icons)` in the root `CMakeLists.txt`, or update the glob to `icons/Deep*.png`. Check the installed archive contents after the build.
**Warning signs:** Icon appears during local testing but is absent from the release zip.

### Pitfall 2: CMake Variable Not Passed to configure_file
**What goes wrong:** `FILTER_NODES` is defined in `src/CMakeLists.txt` but the `configure_file` call does not see it because CMake variable scope is the current directory.
**Why it happens:** `configure_file` in `src/CMakeLists.txt` already has access to variables defined in the same file. This is a non-issue as long as `FILTER_NODES` is defined in the same `src/CMakeLists.txt` that calls `configure_file`.
**How to avoid:** Define `FILTER_NODES` in `src/CMakeLists.txt`, not in the root `CMakeLists.txt`.

### Pitfall 3: License Header Corruption
**What goes wrong:** An editor, formatter, or copy-paste operation strips or rewrites the MIT copyright comment at the top of `DeepThinner.cpp`.
**Why it happens:** Automatic formatters, IDE settings, or a manual edit during "cleanup".
**How to avoid:** Copy the file with `cp` or equivalent — do not open-and-save in an editor that auto-formats. Verify with a diff against the upstream raw file after copying.
**Warning signs:** The copyright comment block no longer matches `https://raw.githubusercontent.com/bratgot/DeepThinner/main/DeepThinner.cpp`.

### Pitfall 4: DeepThinner Registered in Wrong Nuke Menu
**What goes wrong:** DeepThinner is wired into the global Nuke "Deep" built-in menu instead of the project's "DeepC" custom menu.
**Why it happens:** Upstream `menu.py` registers into `nuke.menu("Nodes").findItem("Deep")`. The project's `menu.py.in` registers into a custom `DeepCMenu`. If upstream menu.py is copied verbatim instead of following the project's menu template, DeepThinner appears in the wrong menu.
**How to avoid:** Do not copy the upstream `menu.py`. Add DeepThinner to `menu.py.in` using the project's existing CMake variable pattern.

### Pitfall 5: Windows Build Missing Due to `NOMINMAX`/`_USE_MATH_DEFINES`
**What goes wrong:** Windows build fails with compile errors related to `min`/`max` macros or missing math constants.
**Why it happens:** DeepThinner's upstream `CMakeLists.txt` adds `NOMINMAX` and `_USE_MATH_DEFINES` for Windows. The project's `add_nuke_plugin()` function already adds these for every plugin in the `PLUGINS` list.
**How to avoid:** Using `add_nuke_plugin()` via the `PLUGINS` list automatically applies these flags. No manual per-plugin flag is needed.

---

## Code Examples

### How `add_nuke_plugin` Works (from `src/CMakeLists.txt`)
```cmake
# Source: /workspace/src/CMakeLists.txt lines 50-61
function(add_nuke_plugin PLUGIN_NAME)
    add_library(${PLUGIN_NAME} MODULE ${ARGN})
    add_library(NukePlugins::${PLUGIN_NAME} ALIAS ${PLUGIN_NAME})
    target_link_libraries(${PLUGIN_NAME} PRIVATE ${NUKE_DDIMAGE_LIBRARY})
    set_target_properties(${PLUGIN_NAME} PROPERTIES PREFIX "")
    if (APPLE)
        set_target_properties(${PLUGIN_NAME} PROPERTIES SUFFIX ".dylib")
    endif()
    if (WIN32)
        target_compile_definitions(${PLUGIN_NAME} PRIVATE NOMINMAX _USE_MATH_DEFINES)
    endif()
endfunction()
```

### How the PLUGINS Loop Builds Each Plugin
```cmake
# Source: /workspace/src/CMakeLists.txt lines 97-99
foreach(PLUGIN_NAME ${PLUGINS})
    add_nuke_plugin(${PLUGIN_NAME} ${PLUGIN_NAME}.cpp)
endforeach()
```

Adding `DeepThinner` to `PLUGINS` is sufficient to invoke `add_nuke_plugin(DeepThinner DeepThinner.cpp)` and produce `DeepThinner.so` / `DeepThinner.dll`.

### How CMake Variable Lists Become Python Lists in menu.py.in
```cmake
# Source: /workspace/src/CMakeLists.txt lines 129-135
string(REPLACE ";" "\", \"" DRAW_NODES "\"${DRAW_NODES}\"")
string(REPLACE ";" "\", \"" FILTER_NODES "\"${FILTER_NODES}\"")
configure_file(../python/menu.py.in menu.py)
```

```python
# Result in generated menu.py for a single-member list:
"Filter": {"icon":"ToolbarFilter.png",
           "nodes": ["DeepThinner"]},
```

### How `install(TARGETS ...)` Picks Up the New Plugin
```cmake
# Source: /workspace/src/CMakeLists.txt lines 140-144
install(TARGETS
        ${PLUGINS}        # DeepThinner is now in this list
        ${PLUGINS_WRAPPED}
        ${PLUGINS_MWRAPPED}
        DESTINATION .)
```

No separate install rule is needed for the binary — it is covered by the existing `install(TARGETS ${PLUGINS} ...)` block.

---

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| Manual `menu.py` maintained by hand | CMake-generated `menu.py` from `menu.py.in` | Phase 6 (v1.1) | New plugins only need a CMake variable; no Python editing |
| Per-plugin Dockerfile builds | `docker-build.sh` automated dual-platform loop | Phase 6 (v1.1) | THIN-04 is free once CMake is correct |

---

## Open Questions

1. **What toolbar icon should DeepThinner use?**
   - What we know: No `DeepThinner.png` exists in `icons/`; upstream has no bundled icon.
   - What's unclear: Whether to create a placeholder icon, use an existing one, or ship without an icon.
   - Recommendation: Ship without an icon initially (Nuke handles missing icons gracefully); document as a known limitation. Alternatively, create a simple `DeepThinner.png` matching the project's icon style.

2. **Which menu category fits DeepThinner?**
   - What we know: Upstream registers into Nuke's built-in "Deep" menu. The DeepC project has no "Filter" category yet.
   - What's unclear: User preference for category name ("Filter", "Optimize", "Util").
   - Recommendation: Create a new "Filter" category. "Util" is also reasonable — it currently holds `DeepCAdjustBBox` and `DeepCCopyBBox`. Adding DeepThinner to `Util_NODES` would avoid a new category.

---

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Manual build verification (no automated test suite in this project) |
| Config file | none |
| Quick run command | `docker-build.sh --linux --versions 16.0` |
| Full suite command | `docker-build.sh --all --versions 16.0,16.1,17.0` |

### Phase Requirements → Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| THIN-01 | `src/DeepThinner.cpp` exists with MIT license header intact | manual | `diff <(head -20 src/DeepThinner.cpp) <(curl -s https://raw.githubusercontent.com/bratgot/DeepThinner/main/DeepThinner.cpp | head -20)` | ❌ Wave 0 |
| THIN-02 | CMake produces `DeepThinner.so` / `DeepThinner.dll` | smoke | `docker-build.sh --linux --versions 16.0` then `ls install/16.0-linux/DeepC/DeepThinner.so` | ❌ Wave 0 |
| THIN-03 | DeepThinner appears in menu.py after build | smoke | `grep DeepThinner build/16.0-linux/src/menu.py` | ❌ Wave 0 |
| THIN-04 | Both platform archives contain DeepThinner binary | smoke | `unzip -l release/DeepC-Linux-Nuke16.0.zip | grep DeepThinner` and Windows equivalent | ❌ Wave 0 |

### Sampling Rate
- **Per task commit:** Check that `src/DeepThinner.cpp` exists and `src/CMakeLists.txt` contains `DeepThinner`
- **Per wave merge:** `docker-build.sh --linux --versions 16.0` completes without error
- **Phase gate:** Full `docker-build.sh --all` green before `/gsd:verify-work`

### Wave 0 Gaps
- [ ] No automated test suite exists in this project — all verification is via build smoke tests
- [ ] No CI configuration — verification is manual `docker-build.sh` runs

*(The project has no test infrastructure to scaffold. Verification is build-based.)*

---

## Sources

### Primary (HIGH confidence)
- Direct file read: `/workspace/src/CMakeLists.txt` — complete plugin build system examined
- Direct file read: `/workspace/python/menu.py.in` — menu template examined
- Direct file read: `/workspace/docker-build.sh` — build automation examined
- Direct file read: `/workspace/CMakeLists.txt` — root build and icon install examined
- WebFetch: `https://raw.githubusercontent.com/bratgot/DeepThinner/main/DeepThinner.cpp` — upstream source structure confirmed
- WebFetch: `https://raw.githubusercontent.com/bratgot/DeepThinner/main/CMakeLists.txt` — upstream build dependencies confirmed (DDImage only)
- WebFetch: `https://raw.githubusercontent.com/bratgot/DeepThinner/main/LICENSE` — MIT license, copyright 2025 Marten Blumen confirmed

### Secondary (MEDIUM confidence)
- WebFetch: `https://github.com/bratgot/DeepThinner` — repository file list and toolbar category confirmed

---

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH — examined directly from project source and upstream repo
- Architecture: HIGH — CMake patterns read directly from `src/CMakeLists.txt`; all integration points are well understood
- Pitfalls: HIGH — derived from direct source inspection; icon glob and menu scope issues are concrete, not speculative

**Research date:** 2026-03-18
**Valid until:** 2026-06-18 (stable domain — CMake and Nuke plugin API are not fast-moving)