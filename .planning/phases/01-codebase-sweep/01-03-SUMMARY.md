---
phase: 01-codebase-sweep
plan: 03
subsystem: api
tags: [nuke, deep-compositing, c++, plugin-registration]

# Dependency graph
requires: []
provides:
  - DeepCWrapper base class has no Op::Description registration (not selectable in Nuke node menu)
  - DeepCMWrapper base class has no Op::Description registration (not selectable in Nuke node menu)
  - test_input switch in DeepCWrapper has default case (no undefined behavior on unknown input index)
affects: [all DeepCWrapper subclasses, all DeepCMWrapper subclasses]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Base wrapper classes declare no Op::Description or Class() — only concrete plugin subclasses register themselves"

key-files:
  created: []
  modified:
    - src/DeepCWrapper.cpp
    - src/DeepCWrapper.h
    - src/DeepCMWrapper.cpp
    - src/DeepCMWrapper.h

key-decisions:
  - "Base class plugin registrations removed: DeepCWrapper and DeepCMWrapper should never be user-instantiable; only concrete subclasses register themselves"
  - "Class() returning d.name removed from both base headers — dangling references eliminated alongside their matching definitions"

patterns-established:
  - "Plugin registration pattern: only concrete leaf classes define static Op::Description d and a build() factory; abstract base classes omit both"

requirements-completed: [SWEEP-06]

# Metrics
duration: 8min
completed: 2026-03-12
---

# Phase 01 Plan 03: Deregister DeepCWrapper and DeepCMWrapper Abstract Base Classes Summary

**Removed Op::Description plugin registrations and Class() methods from DeepCWrapper and DeepCMWrapper so abstract base classes can no longer be instantiated as Nuke nodes, plus dead-comment cleanup and switch UB fix**

## Performance

- **Duration:** 8 min
- **Started:** 2026-03-12T00:00:00Z
- **Completed:** 2026-03-12T00:08:00Z
- **Tasks:** 2
- **Files modified:** 4

## Accomplishments
- DeepCWrapper and DeepCMWrapper are no longer selectable nodes in the Nuke node menu
- Four dead commented-out pointer increment lines (`//++inData;`, `//++outData;`) removed from DeepCWrapper.cpp
- `test_input` switch in DeepCWrapper has a `default: return false;` branch preventing undefined behavior on unknown input index
- All 23 individual plugin Op::Description registrations verified untouched

## Task Commits

Each task was committed atomically:

1. **Task 1: Remove DeepCWrapper registration and Class()** - `167c929` (fix)
2. **Task 2: Remove DeepCMWrapper registration and Class()** - `8d7dda6` (fix)

## Files Created/Modified
- `src/DeepCWrapper.cpp` - Removed build() factory and DeepCWrapper::d definition; removed 4 dead comment lines; added default case to test_input switch
- `src/DeepCWrapper.h` - Removed `static const Iop::Description d;` and `const char* Class() const { return d.name; }` from class body
- `src/DeepCMWrapper.cpp` - Removed build() factory and DeepCMWrapper::d definition
- `src/DeepCMWrapper.h` - Removed `static const Iop::Description d;` and `const char* Class() const { return d.name; }` from class body

## Decisions Made
- Removed Class() from both base headers because it returned d.name, which would become a dangling reference once d was removed — the two changes are inseparable.
- Kept subclass Op::Description definitions (DeepCGrade::d, DeepCPMatte::d, etc.) entirely untouched as planned.

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered
None.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- DeepCWrapper and DeepCMWrapper are now clean abstract base classes; any future refactoring or subclassing starts from a correct base
- Plan 04 and Plan 05 can proceed independently — no dependencies on this plan's changes

---
*Phase: 01-codebase-sweep*
*Completed: 2026-03-12*
