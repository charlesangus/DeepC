---
phase: 01-codebase-sweep
plan: 05
subsystem: ndk-api
tags: [deep-compositing, nuke-ndk, cpp, performance, matrix, shell]

# Dependency graph
requires: []
provides:
  - DeepCWorld inverse matrix cached in _validate(), used via member in processSample()
  - Switch UB eliminated in default_input() and input_label() with explicit default cases
  - batchInstall.sh Linux branch comment corrected
  - SWEEP-10 documented: no deprecated Nuke 16 NDK APIs remain in use
affects: [phase 2 - any code touching DeepCWorld or NDK API patterns]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Cache expensive per-sample computations in _validate() as class members with leading underscore naming"
    - "Switch statements in Op override methods always have a default case to avoid undefined behavior"

key-files:
  created: []
  modified:
    - src/DeepCWorld.cpp
    - batchInstall.sh

key-decisions:
  - "DeepCWorld inverse matrix cached as Matrix4 _inverse_window_matrix member: computed once in _validate(), referenced via const ref in processSample() — eliminates per-sample matrix inversion"
  - "SWEEP-10 NDK API audit result: DeepPixelOp and DeepFilterOp are not deprecated in Nuke 16; OutputContext two-arg constructor is deprecated but not used in this codebase — no API migration required"
  - "Switch UB fixed with nullptr (not 0) for Op* return and empty string for const char* return, consistent with C++17 target"

patterns-established:
  - "Inverse matrix cache pattern: compute in _validate(), store as _camelCase member, use via const ref in processSample()"

requirements-completed: [SWEEP-10]

# Metrics
duration: 5min
completed: 2026-03-13
---

# Phase 1 Plan 05: NDK API Modernization (SWEEP-10) Summary

**DeepCWorld per-sample matrix inversion eliminated by caching inverse in _validate(), switch UB fixed with explicit default cases, and batchInstall.sh Linux comment corrected**

## Performance

- **Duration:** ~5 min
- **Started:** 2026-03-13T06:49:00Z
- **Completed:** 2026-03-13T06:49:14Z
- **Tasks:** 2
- **Files modified:** 2

## Accomplishments

- Added `Matrix4 _inverse_window_matrix` class member to DeepCWorld, computed once per validate cycle in `_validate()` instead of every call to `processSample()`
- Eliminated undefined behavior in `default_input()` and `input_label()` switch statements by adding `default: return nullptr` and `default: return ""` respectively
- Corrected copy-paste comment on Linux branch of batchInstall.sh (was "Mac OS X", now "Linux platform")
- SWEEP-10 NDK API audit complete: confirmed no deprecated Nuke 16 NDK APIs in use (DeepPixelOp, DeepFilterOp non-deprecated; OutputContext two-arg constructor deprecated but unused)

## Task Commits

Each task was committed atomically:

1. **Task 1: Cache inverse_window_matrix in DeepCWorld and fix switch UB** - `f89054a` (feat)
2. **Task 2: Fix batchInstall.sh Linux comment** - `acb7a5d` (fix)

## Files Created/Modified

- `src/DeepCWorld.cpp` - Added `_inverse_window_matrix` member, computed in `_validate()`, used via const ref in `processSample()`; default cases added to both switch statements
- `batchInstall.sh` - Comment-only fix: Linux branch comment corrected from "Mac OS X" to "Linux platform"

## Decisions Made

- Used `const Matrix4& inverse_window = _inverse_window_matrix;` local reference in `processSample()` to avoid renaming all downstream uses of `inverse_window` — minimal change, same correctness
- Used `nullptr` (not `0`) for `Op*` default return, consistent with C++17 target
- Cached member uses `_inverse_window_matrix` naming (leading underscore + camelCase), consistent with established convention in CONVENTIONS.md

## Deviations from Plan

None — plan executed exactly as written.

## Issues Encountered

None.

## User Setup Required

None — no external service configuration required.

## Next Phase Readiness

- All SWEEP-10 requirements satisfied: inverse matrix cached, switch UB eliminated, comment corrected, NDK API audit documented
- Phase 1 (Codebase Sweep) now complete — all 5 plans executed
- Ready for Phase 2 planning; GLPM-01 deadlock fix in `draw_handle()` must be first action per established decision

---
*Phase: 01-codebase-sweep*
*Completed: 2026-03-13*
