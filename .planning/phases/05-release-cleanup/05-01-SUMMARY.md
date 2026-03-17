---
phase: 05-release-cleanup
plan: 01
subsystem: ui
tags: [nuke, menu, icon, cmake, requirements-traceability]

# Dependency graph
requires:
  - phase: 04-deepcpnoise-4d
    provides: DeepCShuffle2 node implementation that this plan promotes to the canonical menu entry
provides:
  - DeepCShuffle2 icon asset at icons/DeepCShuffle2.png
  - Single "DeepCShuffle" Nuke menu entry that creates DeepCShuffle2 node
  - DeepCShuffle.so kept as silent backward-compat binary (Op::Description removed)
  - REQUIREMENTS.md traceability with SWEEP-07/08 marked Dropped
affects: []

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Display-name rename dict in menu.py.in: _display_name maps class name to menu label, enabling backward-compat binaries with clean UX"
    - "Op::Description removal pattern for silent backward-compat .so files (established in Phase 1 for DeepCWrapper/DeepCMWrapper, now applied to DeepCShuffle)"

key-files:
  created:
    - icons/DeepCShuffle2.png
  modified:
    - src/CMakeLists.txt
    - python/menu.py.in
    - src/DeepCShuffle.cpp
    - .planning/REQUIREMENTS.md

key-decisions:
  - "DeepCShuffle.so kept in PLUGINS (ships as .so) but removed from CHANNEL_NODES (no menu entry generated) — backward compat without duplicate menu entries"
  - "Icon uses node name (DeepCShuffle2.png), not label, so icon resolution is correct regardless of display-name rename"
  - "SWEEP-07 and SWEEP-08 marked Dropped (not Pending) — requirements were scoped out, not deferred"

patterns-established:
  - "Silent binary pattern: include in PLUGINS, exclude from CHANNEL_NODES, remove Op::Description to prevent self-registration"
  - "Display-name override pattern: _display_name dict in menu.py.in applied before addCommand to decouple class name from menu label"

requirements-completed: []

# Metrics
duration: ~15min
completed: 2026-03-17
---

# Phase 5 Plan 01: Release Cleanup Summary

**DeepCShuffle2 icon, single-entry menu wiring via display-name rename dict, and Op::Description removal to eliminate duplicate registration**

## Performance

- **Duration:** ~15 min
- **Started:** 2026-03-17T11:30:00Z
- **Completed:** 2026-03-17T11:56:30Z
- **Tasks:** 4 (3 auto + 1 UAT checkpoint, user-approved)
- **Files modified:** 5

## Accomplishments

- Added `icons/DeepCShuffle2.png` (copy of DeepCShuffle.png) — CMake glob covers it automatically
- Wired Nuke Channel submenu: one "DeepCShuffle" entry creates a DeepCShuffle2 node via `_display_name` rename dict in menu.py.in
- Removed `Op::Description DeepCShuffle::d` from DeepCShuffle.cpp so DeepCShuffle.so loads silently without self-registering a duplicate menu entry
- Updated REQUIREMENTS.md traceability: SWEEP-07 and SWEEP-08 changed from Pending to Dropped

## Task Commits

Each task was committed atomically:

1. **Task 1: Add DeepCShuffle2 icon asset** - `2f877da` (feat)
2. **Task 2: Wire menu — remove DeepCShuffle from CHANNEL_NODES, add rename dict, remove Op::Description** - `1303f6c` (feat)
3. **Task 3: Update REQUIREMENTS.md traceability — mark SWEEP-07/08 as Dropped** - `f5f47e0` (chore)
4. **Task 4: UAT checkpoint** — user approved

**Plan metadata:** (docs commit — see below)

## Files Created/Modified

- `icons/DeepCShuffle2.png` - Icon asset for DeepCShuffle2 in Nuke toolbar (copy of DeepCShuffle.png)
- `src/CMakeLists.txt` - Removed DeepCShuffle from CHANNEL_NODES; DeepCShuffle2 is now the sole Channel entry
- `python/menu.py.in` - Added `_display_name` rename dict and `label = _display_name.get(node, node)` in inner loop
- `src/DeepCShuffle.cpp` - Removed `static const Iop::Description d;` member declaration, factory function, and `Op::Description DeepCShuffle::d` line
- `.planning/REQUIREMENTS.md` - SWEEP-07 and SWEEP-08 rows updated from Pending to Dropped

## Decisions Made

- **DeepCShuffle.so as silent binary:** kept in CMake PLUGINS list (compiles and ships) but removed from CHANNEL_NODES so no menu entry is generated from it. Op::Description also removed to prevent self-registration if the .so is loaded explicitly. This matches the SWEEP-06 pattern applied to DeepCWrapper/DeepCMWrapper in Phase 1.
- **Icon uses node name not label:** `icon="{}.png".format(node)` uses `node` (DeepCShuffle2) not `label` (DeepCShuffle), ensuring the icon resolves to the correct file.
- **SWEEP-07/08 Dropped:** These requirements were scoped out during Phase 5 planning, not deferred for a future milestone. Dropped status is accurate.

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered

None.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness

- Phase 5 plan 01 complete. This is the final plan of Phase 5 and the v1.0 milestone.
- All 16 requirements satisfied (as confirmed by the v1.0 audit in commit 5244e70).
- SWEEP-07 and SWEEP-08 formally marked Dropped in REQUIREMENTS.md.

---
*Phase: 05-release-cleanup*
*Completed: 2026-03-17*
