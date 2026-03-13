---
phase: 01-codebase-sweep
plan: 01
subsystem: deep-pixel-math
tags: [deepcgrade, deepckeymix, deepcsaturation, deepchueshift, cpp, nuke]

# Dependency graph
requires: []
provides:
  - "DeepCGrade reverse-mode correct gamma application before linear ramp"
  - "DeepCKeymix A-side containment check queries aPixel not bPixel"
  - "DeepCSaturation zero-alpha guard preventing NaN from transparent samples"
  - "DeepCHueShift zero-alpha guard preventing NaN from transparent samples"
affects: [02-gizmo-library-port, 03-new-operators]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Zero-alpha guard pattern: if (alpha != 0.0f) before unpremult division; array stays zero-initialized so else branch is implicit"
    - "Reverse-grade pattern: apply gamma to outData first, then use outData in linear ramp"

key-files:
  created: []
  modified:
    - src/DeepCGrade.cpp
    - src/DeepCKeymix.cpp
    - src/DeepCSaturation.cpp
    - src/DeepCHueShift.cpp

key-decisions:
  - "Fixed three confirmed deep-pixel math bugs with minimal surgical one-line changes each; no behavior change for non-edge-case inputs"
  - "Zero-alpha guard uses implicit else (zero-initialized array) rather than explicit else branch, matching DeepCWrapper.cpp established convention"

patterns-established:
  - "Zero-alpha guard: if (alpha != 0.0f) wrapping unpremult division; rely on pre-initialized storage for the else case"

requirements-completed: [SWEEP-01, SWEEP-02, SWEEP-03]

# Metrics
duration: 1min
completed: 2026-03-13
---

# Phase 1 Plan 1: Codebase Sweep — Bug Fixes Summary

**Three confirmed deep-pixel math bugs fixed: DeepCGrade reverse gamma discard, DeepCKeymix A-side copy-paste error, and zero-alpha NaN in DeepCSaturation and DeepCHueShift**

## Performance

- **Duration:** ~1 min
- **Started:** 2026-03-13T06:40:07Z
- **Completed:** 2026-03-13T06:41:18Z
- **Tasks:** 3
- **Files modified:** 4

## Accomplishments

- DeepCGrade reverse path: gamma result (`outData`) now correctly flows into the linear ramp instead of being discarded and `inputVal` reused
- DeepCKeymix: `aInPixelChannels` now queries `aPixel.channels()` — fixes blending logic for mixed channel sets on the A side
- DeepCSaturation and DeepCHueShift: unpremult division guarded by `if (alpha != 0.0f)`, eliminating NaN propagation from fully transparent samples

## Task Commits

Each task was committed atomically:

1. **Task 1: Fix DeepCGrade reverse-mode gamma discard** - `ba4f1f5` (fix)
2. **Task 2: Fix DeepCKeymix A-side channel set** - `f6512b6` (fix)
3. **Task 3: Fix zero-alpha divide-by-zero in DeepCSaturation and DeepCHueShift** - `0129f3e` (fix)

## Files Created/Modified

- `src/DeepCGrade.cpp` - Reverse path linear ramp changed from `inputVal` to `outData` on line 113
- `src/DeepCKeymix.cpp` - `aInPixelChannels` assignment changed from `bPixel.channels()` to `aPixel.channels()` on line 265
- `src/DeepCSaturation.cpp` - Unpremult division wrapped in `if (alpha != 0.0f)` guard on line 112
- `src/DeepCHueShift.cpp` - Unpremult division wrapped in `if (alpha != 0.0f)` guard on line 106

## Decisions Made

- Each fix was a single-line surgical change; no logic restructuring needed
- Zero-alpha guard uses implicit else (zero-initialized array stays at 0.0f) rather than an explicit else branch, matching the established pattern in DeepCWrapper.cpp
- No build environment available; syntactic correctness verified by code review of minimal one-line changes

## Deviations from Plan

None — plan executed exactly as written.

## Issues Encountered

None. All three fixes were straightforward single-line changes precisely as documented in the plan's `<interfaces>` section.

## User Setup Required

None — no external service configuration required.

## Next Phase Readiness

- All four files are patched and ready for use
- DeepCSaturation and DeepCHueShift zero-alpha guard establishes a pattern all future `wrappedPerSample` implementations should follow
- Remaining Phase 1 plans (02 through 05) can proceed; no blockers introduced

## Self-Check: PASSED

All files found. All task commits verified.

---
*Phase: 01-codebase-sweep*
*Completed: 2026-03-13*
