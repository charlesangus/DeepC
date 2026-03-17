# Phase 5: Release Cleanup - Context

**Gathered:** 2026-03-17
**Status:** Ready for planning

<domain>
## Phase Boundary

Add a DeepCShuffle2 icon, promote DeepCShuffle2 as the single "DeepCShuffle" menu entry (DeepCShuffle.so remains compiled and loadable for backward compatibility but is hidden from the menu), and update REQUIREMENTS.md traceability to mark SWEEP-07 and SWEEP-08 as Dropped.

</domain>

<decisions>
## Implementation Decisions

### Icon
- `icons/DeepCShuffle2.png` is a copy of `icons/DeepCShuffle.png` — identical icon, no visual distinction needed since only one entry appears in the menu
- CMake install: no CMakeLists change needed — the root `CMakeLists.txt` already globs `icons/DeepC*.png` and installs them to `icons/`; adding the file is sufficient

### Menu wiring
- The Nuke toolbar menu entry is labeled **"DeepCShuffle"** (no version suffix visible to artists)
- The entry calls `nuke.createNode('DeepCShuffle2')` and uses icon `DeepCShuffle2.png`
- Approach: inline rename dict inside `menu.py.in` (e.g. `{"DeepCShuffle2": "DeepCShuffle"}`) — a small special-case in the loop, no new CMake variables
- `DeepCShuffle` is removed from `CHANNEL_NODES` in `src/CMakeLists.txt` so it produces no menu entry
- `DeepCShuffle` stays in the `PLUGINS` build list so it compiles to a `.so` and ships in the install directory

### REQUIREMENTS.md traceability
- SWEEP-07 and SWEEP-08 status updated from "Pending" to "Dropped" in the traceability table
- Rationale already documented in Phase 1 CONTEXT.md — no need to repeat inline

### Claude's Discretion
- Exact Python implementation of the rename dict / special-case in `menu.py.in`
- Whether REQUIREMENTS.md "Dropped" rows get a note column or just the status change

</decisions>

<specifics>
## Specific Ideas

No specific requirements — open to standard approaches.

</specifics>

<code_context>
## Existing Code Insights

### Reusable Assets
- `icons/DeepCShuffle.png`: source file to copy as `DeepCShuffle2.png`
- `python/menu.py.in`: the template to modify — currently iterates `entry.get("nodes")` and uses `node` as both label and class name

### Established Patterns
- Icon install: root `CMakeLists.txt` line 41-42 globs `icons/DeepC*.png` → no CMake edit needed, just drop the file
- Plugin build: `src/CMakeLists.txt` has `PLUGINS` list (compiles) and `CHANNEL_NODES` list (menu) as separate concerns — split already cleanly supports hiding a plugin from the menu

### Integration Points
- `src/CMakeLists.txt` line 42: `set(CHANNEL_NODES ...)` — remove `DeepCShuffle`, keep `DeepCShuffle2`
- `python/menu.py.in` `addCommand` loop: add a rename dict so `DeepCShuffle2` displays as `DeepCShuffle`
- `REQUIREMENTS.md` traceability table: SWEEP-07 and SWEEP-08 rows

</code_context>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope.

</deferred>

---

*Phase: 05-release-cleanup*
*Context gathered: 2026-03-17*
