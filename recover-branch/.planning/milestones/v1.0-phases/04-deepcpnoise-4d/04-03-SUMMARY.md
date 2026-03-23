---
phase: 04-deepcpnoise-4d
plan: 03
subsystem: noise
tags: [fastnoise, cpp, 4d-noise, deepcpnoise, nuke, wiring, dispatch]

# Dependency graph
requires:
  - phase: 04-01
    provides: "4D Value and Perlin Single* implementations + FastNoise.h declarations"
  - phase: 04-02
    provides: "4D Cubic and Cellular Single* implementations"
provides:
  - FastNoise.cpp: Complete GetNoise(x,y,z,w) dispatch covering all 9 noise types (+ default)
  - FastNoise.cpp: w *= m_frequency applied before dispatch so all 4 axes are uniformly scaled
  - src/DeepCPNoise.cpp: Unconditional GetNoise(x,y,z,w) call using _noiseEvolution as w dimension
  - src/DeepCPNoise.cpp: Removed if(_noiseType==0) branch — all noise types now support 4D evolution
  - src/DeepCPNoise.cpp: Updated tooltips — no tooltip implies Simplex is the only 4D-capable type
affects: []

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "4D dispatch pattern: GetNoise(x,y,z,w) switch covers all NoiseType enum values; WhiteNoise falls to default: return 0"
    - "Uniform w-axis frequency scaling: w *= m_frequency applied in GetNoise before dispatch (consistent with x/y/z); Simplex FBM quirk (internal w scaling) left intact for backward compatibility"
    - "Call-site simplification: DeepCPNoise wrappedPerSample always calls the 4D overload; _noiseEvolution=0 produces identical output to 3D noise"

key-files:
  created: []
  modified:
    - FastNoise/FastNoise.cpp
    - src/DeepCPNoise.cpp

key-decisions:
  - "WhiteNoise left as default: return 0 in 4D dispatch — consistent with the 3D GetNoise dispatch which also delegates WhiteNoise separately"
  - "Simplex FBM internal w-scaling quirk left unchanged — plan specified explicitly to not fix this legacy inconsistency"
  - "Removed TODO comment above the former if(_noiseType==0) block along with the branch itself"
  - "UAT checkpoint passed: all 5 noise types (Simplex, Perlin, Value, Cubic, Cellular) produce non-black non-uniform output with non-zero noise_evolution in Nuke"

patterns-established:
  - "Complete 4D dispatch: any new NoiseType added to FastNoise in future must also be added to the GetNoise(x,y,z,w) switch"
  - "Unconditional 4D call site: DeepCPNoise does not gate on noise type; the FastNoise dispatch owns the type-specific routing"

requirements-completed: [NOIS-01]

# Metrics
duration: ~5min
completed: 2026-03-17
---

# Phase 4 Plan 03: Wire DeepCPNoise 4D Dispatch Summary

**GetNoise(x,y,z,w) extended to all 9 noise types in FastNoise.cpp; DeepCPNoise.cpp unconditionally calls the 4D overload with _noiseEvolution as w — completing NOIS-01 across all noise types, UAT approved**

## Performance

- **Duration:** ~5 min
- **Started:** 2026-03-17T10:25:00Z
- **Completed:** 2026-03-17T10:30:00Z
- **Tasks:** 2 (1 auto + 1 UAT checkpoint)
- **Files modified:** 2

## Accomplishments

- Extended `GetNoise(x,y,z,w)` switch to cover all 9 concrete noise types: Value, ValueFractal, Perlin, PerlinFractal, Simplex, SimplexFractal, Cellular, Cubic, CubicFractal
- Added `w *= m_frequency` before the dispatch so the w axis is uniformly scaled in line with x/y/z
- Removed the `if (_noiseType==0)` branch from `DeepCPNoise::wrappedPerSample()` — all noise types now unconditionally call the 4D overload
- Updated `noise_evolution` tooltip to describe universal 4D support; removed any implication that Simplex is the only 4D-capable type
- UAT confirmed in Nuke: all 5 noise types (Simplex, Perlin, Value, Cubic, Cellular) produce non-black non-uniform output with `noise_evolution != 0`; zero evolution produces stable output matching 3D noise

## Task Commits

Each task was committed atomically:

1. **Task 1: Extend GetNoise(x,y,z,w) dispatch + wire DeepCPNoise unconditionally** - `9574c10` (feat)
2. **Task 2: UAT - Verify all noise types support 4D evolution in Nuke** - UAT approved (no code commit — verification only)

## Files Created/Modified

- `/workspace/FastNoise/FastNoise.cpp` - Extended `GetNoise(x,y,z,w)` switch from 2 cases (Simplex/SimplexFractal) to 9 cases; added `w *= m_frequency` frequency scaling
- `/workspace/src/DeepCPNoise.cpp` - Removed `if (_noiseType==0)` branch; unconditional `_fastNoise.GetNoise(position[0], position[1], position[2], _noiseEvolution)`; updated `noise_evolution` tooltip

## Decisions Made

- WhiteNoise left as `default: return 0` in the 4D dispatch. The 3D `GetNoise` dispatch also does not have an explicit WhiteNoise case (WhiteNoise has its own `GetWhiteNoise` family), so returning 0 in the 4D path is consistent with the existing design.
- The Simplex FBM internal w-scaling quirk was left unchanged per the plan's explicit instruction. The new dispatch cases (Value, Perlin, Cubic, Cellular) all follow the pre-scaled convention correctly.
- The `noise_evolution` tooltip was rewritten to be type-neutral, describing the w-dimension behavior for all noise types. The `noiseType` tooltip for Simplex was updated to remove the "4D noise" qualifier.

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered

None — build was clean, UAT passed on all tested noise types with no failures.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness

- NOIS-01 is complete: all five noise types accessible in the Nuke UI support 4D evolution via the `noise_evolution` knob
- Phase 4 (04-deepcpnoise-4d) is fully complete — all three plans executed and verified
- No follow-up work required in FastNoise or DeepCPNoise for 4D evolution support

---
*Phase: 04-deepcpnoise-4d*
*Completed: 2026-03-17*
