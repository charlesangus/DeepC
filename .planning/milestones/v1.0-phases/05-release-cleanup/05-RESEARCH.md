# Phase 5: Release Cleanup - Research

**Researched:** 2026-03-17
**Domain:** Nuke plugin menu wiring (CMake + Python template), icon asset management, documentation traceability
**Confidence:** HIGH

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions

#### Icon
- `icons/DeepCShuffle2.png` is a copy of `icons/DeepCShuffle.png` — identical icon, no visual distinction needed since only one entry appears in the menu
- CMake install: no CMakeLists change needed — the root `CMakeLists.txt` already globs `icons/DeepC*.png` and installs them to `icons/`; adding the file is sufficient

#### Menu wiring
- The Nuke toolbar menu entry is labeled **"DeepCShuffle"** (no version suffix visible to artists)
- The entry calls `nuke.createNode('DeepCShuffle2')` and uses icon `DeepCShuffle2.png`
- Approach: inline rename dict inside `menu.py.in` (e.g. `{"DeepCShuffle2": "DeepCShuffle"}`) — a small special-case in the loop, no new CMake variables
- `DeepCShuffle` is removed from `CHANNEL_NODES` in `src/CMakeLists.txt` so it produces no menu entry
- `DeepCShuffle` stays in the `PLUGINS` build list so it compiles to a `.so` and ships in the install directory

#### REQUIREMENTS.md traceability
- SWEEP-07 and SWEEP-08 status updated from "Pending" to "Dropped" in the traceability table
- Rationale already documented in Phase 1 CONTEXT.md — no need to repeat inline

### Claude's Discretion
- Exact Python implementation of the rename dict / special-case in `menu.py.in`
- Whether REQUIREMENTS.md "Dropped" rows get a note column or just the status change

### Deferred Ideas (OUT OF SCOPE)
None — discussion stayed within phase scope.
</user_constraints>

---

## Summary

Phase 5 is a focused release-cleanup phase with three independent, low-risk tasks: (1) add a `DeepCShuffle2.png` icon file, (2) wire the Nuke toolbar menu so that one "DeepCShuffle" entry creates a `DeepCShuffle2` node while `DeepCShuffle.so` remains compiled but invisible to artists, and (3) update REQUIREMENTS.md traceability to mark SWEEP-07 and SWEEP-08 as Dropped.

All implementation points are pre-identified with exact file locations. The CMake build system already supports the icon glob pattern and the PLUGINS/CHANNEL_NODES split that makes a plugin compilable but menu-hidden. The only novel work is a Python rename-dict in `menu.py.in` and a one-line CMake list edit.

No new libraries, no new build dependencies, and no automated test infrastructure exist in this project. Validation is manual: load both `.so` files in Nuke and confirm menu state.

**Primary recommendation:** Implement the three tasks in order — icon first (file copy, no risk), then menu wiring (CMake + Python template edits), then REQUIREMENTS.md update (pure documentation).

---

## Standard Stack

### Core (already in use — no new installs)

| Component | Version / Location | Purpose | Why Standard |
|-----------|-------------------|---------|--------------|
| CMake `file(GLOB ...)` + `install(FILES ...)` | root CMakeLists.txt lines 41-42 | Auto-installs all `icons/DeepC*.png` | Already wired; adding a file is sufficient |
| `src/CMakeLists.txt` `set(CHANNEL_NODES ...)` | line 42 | Controls which plugins receive menu entries | Established separation of PLUGINS (compile) vs CHANNEL_NODES (menu) |
| `python/menu.py.in` CMake template | `configure_file` in src/CMakeLists.txt line 121 | Generates `menu.py` from CMake list variables | Already used for all menu entries |
| `nuke.addCommand(label, command, icon=...)` | `menu.py.in` line 27 | Registers a named entry in the Nuke toolbar | Standard Nuke Python API |

### No New Libraries

This phase adds no new dependencies. All tooling (CMake, Python, Nuke NDK) is already present.

---

## Architecture Patterns

### Existing Project Structure (relevant files)

```
DeepC/
├── icons/
│   ├── DeepCShuffle.png        # source to copy
│   └── DeepCShuffle2.png       # NEW — copy of above
├── python/
│   └── menu.py.in              # CMake template — modify loop
├── src/
│   └── CMakeLists.txt          # CHANNEL_NODES list — remove DeepCShuffle
├── CMakeLists.txt              # icon glob install — no change needed
└── .planning/
    └── REQUIREMENTS.md         # traceability table — update SWEEP-07/08
```

### Pattern 1: CMake PLUGINS vs CHANNEL_NODES Separation

**What:** Two separate CMake lists govern (a) which plugins compile to `.so` and (b) which plugins appear in the Nuke menu.

**Current state (src/CMakeLists.txt):**
```cmake
# Line 2-12: compiles both DeepCShuffle and DeepCShuffle2 to .so
set(PLUGINS
    ...
    DeepCShuffle
    DeepCShuffle2
    ...)

# Line 42: both currently appear in menu
set(CHANNEL_NODES DeepCAddChannels DeepCRemoveChannels DeepCShuffle DeepCShuffle2)
```

**After change:** Remove `DeepCShuffle` from `CHANNEL_NODES` only. `PLUGINS` stays unchanged.

```cmake
set(CHANNEL_NODES DeepCAddChannels DeepCRemoveChannels DeepCShuffle2)
```

This is the complete and only CMake edit required for menu hiding.

### Pattern 2: menu.py.in Rename Dict

**What:** The menu loop in `menu.py.in` currently uses `node` as both the menu label and the `createNode` class name. A rename dict makes these independently configurable for specific entries.

**Current loop (menu.py.in lines 26-27):**
```python
for node in entry.get("nodes"):
    new.addCommand(node, "nuke.createNode('{}')".format(node), icon="{}.png".format(node))
```

**Proposed pattern (Claude's discretion to choose exact form):**
```python
# Rename dict: maps internal class name to display label
_display_name = {"DeepCShuffle2": "DeepCShuffle"}

for node in entry.get("nodes"):
    label = _display_name.get(node, node)
    new.addCommand(label, "nuke.createNode('{}')".format(node), icon="{}.png".format(node))
```

This keeps `icon=DeepCShuffle2.png` (by using `node` not `label` for the icon path) which is correct since the icon filename matches the class name, not the display label.

**Alternative (also valid):** Keep the icon name derived from label by using `icon="{}.png".format(label)` — but that would require the icon to be named `DeepCShuffle.png`, which conflicts with the existing file for the old node. Using `node` for icon path avoids this collision.

### Pattern 3: Icon File Install via Glob

**What:** Root `CMakeLists.txt` line 41-42 uses `file(GLOB ICONS "icons/DeepC*.png")` at configure time.

**Important caveat:** CMake `file(GLOB ...)` is evaluated at configure time, not build time. If the file is added to the `icons/` directory after a configure has already run, a re-configure is required before CMake will include the new file in the install set. This is expected behavior and not a bug — the build/install step will pick up the new file after a clean configure.

**No CMakeLists.txt edit required.** Dropping `icons/DeepCShuffle2.png` is the complete action.

### Pattern 4: REQUIREMENTS.md Traceability Update

**Current state (REQUIREMENTS.md lines 92-93 and 104-106):**
```markdown
| SWEEP-07 | Phase 1 | Pending |
| SWEEP-08 | Phase 1 | Pending |
...
| THIN-01 | Phase 5 | Pending |
| THIN-02 | Phase 5 | Pending |
| THIN-03 | Phase 5 | Pending |
```

**Target state:** SWEEP-07 and SWEEP-08 rows change `Pending` to `Dropped`. THIN-01/02/03 are also `Pending` but belong to a deferred milestone — leave them unchanged (they are not in scope for this phase).

Note: The traceability table currently maps THIN-01/02/03 to "Phase 5" — this mapping was set before Phase 5 was redefined as release-cleanup. Those rows should NOT be touched in this phase. The CONTEXT.md is explicit: only SWEEP-07 and SWEEP-08 change status.

### Anti-Patterns to Avoid

- **Removing DeepCShuffle from PLUGINS:** Do not remove `DeepCShuffle` from the `PLUGINS` list. Backward compatibility requires the `.so` to ship. Only remove it from `CHANNEL_NODES`.
- **Renaming the icon based on display label:** Using `icon="{}.png".format(label)` would look for `DeepCShuffle.png` as the icon for the DeepCShuffle2 node — this works accidentally today because the files are identical, but the architecture decision specifies `DeepCShuffle2.png` as the correct icon name for the DeepCShuffle2 node.
- **Adding a new CMake variable:** The CONTEXT.md decision locks "inline rename dict, no new CMake variables." Do not introduce a `CHANNEL_NODE_LABELS` or equivalent CMake variable.
- **Updating THIN-01/02/03:** These are deferred, not dropped. Do not change their status.

---

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Icon discovery at install time | Custom CMake install rule | Existing `file(GLOB)` in root CMakeLists.txt | Already handles all `icons/DeepC*.png` |
| Per-node label override | New CMake variable or second list | Inline Python dict in `menu.py.in` | Simpler, no CMake changes, established Python pattern |

**Key insight:** The split between PLUGINS (compile) and CHANNEL_NODES (menu) was intentionally designed in the project (as confirmed by SWEEP-06 which removed `Op::Description` from wrapper classes). This phase uses that existing design rather than inventing new mechanisms.

---

## Common Pitfalls

### Pitfall 1: CMake Glob Stale Cache

**What goes wrong:** `icons/DeepCShuffle2.png` is added to disk but `cmake --install` does not include it.
**Why it happens:** `file(GLOB ...)` runs at configure time. If the build directory was configured before the file was added, the glob result is cached.
**How to avoid:** After adding the icon file, run a fresh CMake configure (or `cmake .` in the build dir) before `cmake --install`. This is standard CMake behavior.
**Warning signs:** `cmake --install` completes without copying `DeepCShuffle2.png` to the install prefix's `icons/` directory.

### Pitfall 2: Both DeepCShuffle and DeepCShuffle2 Appear in Menu

**What goes wrong:** After editing CHANNEL_NODES, the generated `menu.py` still registers both nodes.
**Why it happens:** `configure_file` was not re-run after the CMake edit, so the old `menu.py` is still present in the build directory.
**How to avoid:** After editing `src/CMakeLists.txt`, run CMake configure to regenerate `menu.py` from the updated template. Then verify `menu.py` content before loading in Nuke.
**Warning signs:** `cat build/menu.py` shows `"DeepCShuffle"` and `"DeepCShuffle2"` both in the Channel section.

### Pitfall 3: Duplicate "DeepCShuffle" Labels from Two Sources

**What goes wrong:** If `DeepCShuffle.so` is loaded by Nuke (via `nuke.load()` or auto-discovery) AND `menu.py` also registers a "DeepCShuffle" label, you may get two menu entries or a conflict.
**Why it happens:** Nuke auto-registers nodes from `.so` files that contain `Op::Description`; however, `DeepCShuffle` uses a manual `build()` factory (consistent with SWEEP-06's pattern for non-wrapper plugins). As a non-wrapper plugin, it relies on `Op::Description` for self-registration — verify whether DeepCShuffle.so self-registers in the menu when loaded.
**How to avoid:** Load both `.so` files in Nuke and inspect the Channel submenu before and after. If `DeepCShuffle.so` self-registers, its `Op::Description` may need to be removed (similar to SWEEP-06 treatment for DeepCWrapper/DeepCMWrapper).
**Warning signs:** Two "DeepCShuffle" entries in the Channel submenu after loading both `.so` files.

### Pitfall 4: Icon Path Mismatch in Nuke

**What goes wrong:** The node icon does not appear in Nuke — broken icon placeholder shown.
**Why it happens:** Nuke looks for the icon by name in its `icons/` path. If the icon is installed with a different name than what `addCommand` specifies, it silently falls back to no icon.
**How to avoid:** Confirm `addCommand` uses `icon="DeepCShuffle2.png"` and the installed file is named exactly `DeepCShuffle2.png` (case-sensitive on Linux).
**Warning signs:** No icon displayed next to the DeepCShuffle menu entry in Nuke.

---

## Code Examples

### menu.py.in After Edit

```python
# Source: python/menu.py.in (modified for Phase 5)
import nuke

def create_deepc_menu():
    menus = {
            "Draw": {"icon":"ToolbarDraw.png",
                    "nodes": [@DRAW_NODES@]},
            "Channel": {"icon":"ToolbarChannel.png",
                        "nodes": [@CHANNEL_NODES@]},
            "Color": {"icon":"ToolbarColor.png",
                      "nodes": [@COLOR_NODES@]},
            "3D": {"icon":"ToolbarChannel.png",
                    "nodes": [@3D_NODES@]},
            "Merge": {"icon":"ToolbarMerge.png",
                      "nodes": [@MERGE_NODES@]},
            "Util": {"icon":"ToolbarToolsets.png",
                    "nodes": [@Util_NODES@]},
    }

    # Display-name overrides: maps class name → menu label
    _display_name = {"DeepCShuffle2": "DeepCShuffle"}

    toolbar = nuke.menu("Nodes")
    DeepCMenu = toolbar.addMenu("DeepC", icon="DeepC.png")

    for menu, entry in menus.items():
        new = DeepCMenu.addMenu(menu, icon=entry["icon"])

        for node in entry.get("nodes"):
            label = _display_name.get(node, node)
            new.addCommand(label, "nuke.createNode('{}')".format(node), icon="{}.png".format(node))

create_deepc_menu()
```

### src/CMakeLists.txt CHANNEL_NODES Line (after edit)

```cmake
# Source: src/CMakeLists.txt line 42 (modified for Phase 5)
# DeepCShuffle removed from menu; DeepCShuffle2 shown as "DeepCShuffle" via rename dict
set(CHANNEL_NODES DeepCAddChannels DeepCRemoveChannels DeepCShuffle2)
```

### REQUIREMENTS.md Traceability Rows (after edit)

```markdown
| SWEEP-07 | Phase 1 | Dropped |
| SWEEP-08 | Phase 1 | Dropped |
```

---

## State of the Art

| Old Approach | Current Approach | Introduced | Impact |
|--------------|-----------------|------------|--------|
| Both DeepCShuffle and DeepCShuffle2 in CHANNEL_NODES | Only DeepCShuffle2, displayed as "DeepCShuffle" | Phase 5 | Artists see one canonical entry; old .so stays loadable for compat |

**Pending / stale in REQUIREMENTS.md:**
- THIN-01/02/03 are mapped to "Phase 5" in the current traceability table but DeepThinner was deferred. The table mapping is stale but fixing that mapping is out of scope for this phase — only SWEEP-07/08 status is in scope.

---

## Open Questions

1. **Does DeepCShuffle.so self-register via Op::Description?**
   - What we know: Non-wrapper plugins in this project use `static Iop::Description d(...)` for self-registration. SWEEP-06 removed `Op::Description` from the wrapper base classes only.
   - What's unclear: Whether `src/DeepCShuffle.cpp` contains its own `Op::Description` that would auto-register it in the Nuke menu when the `.so` is loaded.
   - Recommendation: Check `src/DeepCShuffle.cpp` for `Op::Description` or `DD::Image::Op::Description` during plan execution. If present and the `.so` is auto-loaded, a second "DeepCShuffle" entry could appear. The fix would be to remove/comment out that Description from DeepCShuffle.cpp (not from DeepCShuffle2.cpp).

2. **THIN-01/02/03 mapping in REQUIREMENTS.md**
   - What we know: These rows currently say `Phase 5 | Pending` but DeepThinner was deferred before this milestone.
   - What's unclear: Whether the planner should correct the Phase column (e.g., change to "Deferred" or a future phase number) or leave it.
   - Recommendation: Leave THIN rows unchanged — CONTEXT.md scopes only SWEEP-07 and SWEEP-08 for this phase. Flag as a post-milestone documentation cleanup if desired.

---

## Validation Architecture

### Test Framework

| Property | Value |
|----------|-------|
| Framework | None — Nuke C++ plugin; no automated test suite exists |
| Config file | N/A |
| Quick run command | Manual: load `DeepCShuffle2.so` + `DeepCShuffle.so` in Nuke, inspect Channel submenu |
| Full suite command | Manual UAT: verify icon, label, node creation, no duplicates |

### Phase Requirements → Test Map

This phase has no formal requirement IDs. Validation maps to the four success criteria:

| Success Criterion | Behavior | Test Type | Automated Command | File Exists? |
|------------------|----------|-----------|-------------------|-------------|
| SC-1: icon file exists | `icons/DeepCShuffle2.png` present and installed | smoke | `ls icons/DeepCShuffle2.png` (source); `ls <install>/icons/DeepCShuffle2.png` (after install) | ❌ file created in this phase |
| SC-2: one menu entry | Nuke Channel submenu shows "DeepCShuffle" (creates DeepCShuffle2 node), no separate "DeepCShuffle2" entry | manual-only | N/A — requires Nuke runtime | N/A |
| SC-3: no duplicates | Loading both .so files produces no duplicate or broken entries | manual-only | N/A — requires Nuke runtime | N/A |
| SC-4: REQUIREMENTS.md updated | SWEEP-07 and SWEEP-08 rows show "Dropped" | smoke | `grep -E "SWEEP-0[78]" .planning/REQUIREMENTS.md` | ✅ file exists |

### Sampling Rate

- **Per task commit:** `ls /workspace/icons/DeepCShuffle2.png` (SC-1) or `grep -E "SWEEP-0[78]" /workspace/.planning/REQUIREMENTS.md` (SC-4) — where applicable
- **Per wave merge:** All automated smoke checks above
- **Phase gate:** Full manual UAT in Nuke before `/gsd:verify-work`

### Wave 0 Gaps

None — no test framework setup needed. Smoke checks use standard shell commands against files that will exist after tasks execute.

---

## Sources

### Primary (HIGH confidence)

- Direct file inspection: `/workspace/python/menu.py.in` — current menu loop implementation
- Direct file inspection: `/workspace/src/CMakeLists.txt` — PLUGINS, CHANNEL_NODES, configure_file
- Direct file inspection: `/workspace/CMakeLists.txt` — icon glob pattern lines 41-42
- Direct file inspection: `/workspace/icons/` — confirms `DeepCShuffle.png` exists, `DeepCShuffle2.png` does not yet exist
- Direct file inspection: `/workspace/.planning/REQUIREMENTS.md` — current traceability table state
- Direct file inspection: `/workspace/.planning/phases/05-release-cleanup/05-CONTEXT.md` — locked decisions

### Secondary (MEDIUM confidence)

- CMake `file(GLOB)` configure-time evaluation behavior: well-documented CMake limitation, consistent with observed project structure

### Tertiary (LOW confidence — for open question only)

- DeepCShuffle.cpp self-registration behavior: not verified by reading the source file; inferred from project patterns

---

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH — all components verified by direct file inspection
- Architecture: HIGH — implementation points are precisely identified with line numbers
- Pitfalls: HIGH (SC-1/SC-2/SC-4), MEDIUM (SC-3 duplicate menu risk — depends on DeepCShuffle.cpp content not yet read)

**Research date:** 2026-03-17
**Valid until:** Stable — no external dependencies; valid until project files change
