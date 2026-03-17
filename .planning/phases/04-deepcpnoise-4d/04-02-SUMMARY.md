---
phase: 04-deepcpnoise-4d
plan: 02
subsystem: noise
tags: [fastnoise, cpp, 4d-noise, cubic-noise, cellular-noise, catmull-rom]

# Dependency graph
requires:
  - phase: 04-01
    provides: "ValCoord4DFast helper, Index4D_256 hash, 4D method declarations in FastNoise.h"
provides:
  - FastNoise.cpp: CELL_4D_X/Y/Z/W static arrays (256 normalized 4D unit vectors each)
  - FastNoise.cpp: CUBIC_4D_BOUNDING = 1/(1.5^4) normalization constant
  - FastNoise.cpp: SingleCubic(4D) — 256-sample Catmull-Rom cascade over 4 w-slabs
  - FastNoise.cpp: SingleCubicFractalFBM/Billow/RigidMulti(4D) — fractal octave wrappers
  - FastNoise.cpp: SingleCellular(4D) — 81-cell neighborhood search with full return type support
  - FastNoise.cpp: SingleCellular2Edge(4D) — 81-cell 2Edge search with distance array pattern
affects: [04-03]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "4D Catmull-Rom cubic: 4 w-slabs x 64 samples = 256 ValCoord4DFast lookups, normalized by CUBIC_4D_BOUNDING = 1/(1.5^4)"
    - "4D Cellular neighborhood: 4 nested loops (wi,zi,yi,xi) over 3^4=81 cells using CELL_4D jitter tables"
    - "CELL_4D tables: 256 pre-normalized 4D unit vectors (discrete canonical set, same design philosophy as CELL_3D)"
    - "4D Cellular2Edge: uses fmax/fmin distance array with m_cellularDistanceIndex0/1, matching 3D pattern exactly"
    - "4D Cellular switch-on-distance-function structure: separate loop per Euclidean/Manhattan/Natural for cache efficiency"

key-files:
  created: []
  modified:
    - FastNoise/FastNoise.cpp

key-decisions:
  - "Modeled SingleCellular 4D on the actual 3D switch-on-distance-function structure (separate loops per type), not the simplified plan pseudocode — ensures correctness and matches codebase style"
  - "Modeled SingleCellular2Edge 4D on the actual fmax/fmin distance array pattern with m_cellularDistanceIndex0/1, not the simple distance/distance2 pair — supports multiple distance indices"
  - "CELL_4D jitter tables placed immediately after CELL_3D_Z for locality, matching CELL_3D table placement convention"

patterns-established:
  - "4D Cubic normalization: CUBIC_4D_BOUNDING = 1/(1.5^4) applied as the final multiplier"
  - "4D Cellular hashing: Index4D_256(0, xi, yi, zi, wi) for all 4D cellular loops"

requirements-completed: [NOIS-01]

# Metrics
duration: 5min
completed: 2026-03-17
---

# Phase 4 Plan 02: Add 4D Cubic and Cellular Noise Summary

**4D Cubic noise (256-sample Catmull-Rom + 3 fractal variants) and 4D Cellular noise (SingleCellular + SingleCellular2Edge with 81-cell neighborhood) added to FastNoise.cpp, completing all declared 4D Single* implementations**

## Performance

- **Duration:** 5 min
- **Started:** 2026-03-17T10:17:22Z
- **Completed:** 2026-03-17T10:23:02Z
- **Tasks:** 2
- **Files modified:** 1

## Accomplishments

- Added CELL_4D_X/Y/Z/W static arrays (4 x 256 = 1024 values) as normalized 4D unit vector jitter tables
- Added CUBIC_4D_BOUNDING = 1/(1.5^4) constant and implemented SingleCubic(4D) using a full 256-sample Catmull-Rom cascade across 4 w-slabs
- Implemented all three SingleCubicFractal wrappers (FBM, Billow, RigidMulti) in 4D with correct pre-scaled convention and w-axis lacunarity scaling
- Implemented SingleCellular(4D) searching 3^4=81 cells with Euclidean/Manhattan/Natural distance support and CellValue/NoiseLookup/Distance return types
- Implemented SingleCellular2Edge(4D) with the same 81-cell structure, using the fmax/fmin distance array pattern for multi-index Distance2 returns
- `make FastNoise` compiles with zero errors; all declared 4D methods in FastNoise.h now have implementations

## Task Commits

Each task was committed atomically:

1. **Task 1: Add CELL_4D tables and implement 4D Cubic noise** - `c07762b` (feat)
2. **Task 2: Implement 4D Cellular noise in FastNoise.cpp** - `8f354e2` (feat)

## Files Created/Modified

- `/workspace/FastNoise/FastNoise.cpp` - 669 lines added: CELL_4D tables (436 lines) + SingleCubic 4D (233 lines)

## Decisions Made

- Modeled the 4D Cellular functions on the actual 3D implementations rather than the simplified plan pseudocode. The 3D `SingleCellular` uses a switch on distance function with separate loops per type (not the unified-loop pattern shown in the plan), and the 3D `SingleCellular2Edge` uses `fmax`/`fmin` with a `distance[]` array indexed by `m_cellularDistanceIndex0/1` (not the simple `distance`/`distance2` pair). Matching the actual 3D structure ensures consistency and correctness.
- CELL_4D tables placed immediately after CELL_3D_Z (before the helper functions section), consistent with the positioning of CELL_3D tables in the file.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Implemented SingleCellular(4D) and SingleCellular2Edge(4D) with correct 3D-matching structure**
- **Found during:** Task 2 (Implement 4D Cellular noise)
- **Issue:** The plan's pseudocode for SingleCellular used a unified loop with an inline switch, while the actual 3D codebase uses a switch-on-distance-function with separate loops per type. The plan's SingleCellular2Edge used a simple `distance`/`distance2` pair, while the actual 3D version uses a `distance[]` array with `m_cellularDistanceIndex0/1` and `fmax`/`fmin`. Using the plan's simplified version would have been inconsistent with the codebase and silently broken multi-index distance support.
- **Fix:** Implemented both functions following the actual 3D pattern exactly, adding the `wi` outer loop and `vecW` component at each layer.
- **Files modified:** FastNoise/FastNoise.cpp
- **Verification:** Build passes with zero errors; all expected symbols present.
- **Committed in:** `8f354e2` (Task 2 commit)

---

**Total deviations:** 1 auto-fixed (Rule 1 - structural correctness)
**Impact on plan:** The deviation was necessary to match the actual codebase structure. No scope creep — the output functions are identical in behavior to what the plan specified, just implemented correctly.

## Issues Encountered

None — the build was clean before and remained clean after all additions.

## Next Phase Readiness

- FastNoise.cpp now has complete 4D implementations for all declared Single* methods: Value (04-01), Perlin (04-01), Cubic (this plan), and Cellular (this plan)
- Plan 04-03 (DeepCPNoise wiring) can proceed: the 4D GetNoise dispatch currently only routes Simplex/SimplexFractal in 4D; Plan 03 will extend the switch to include Value, Perlin, Cubic, and Cellular 4D noise types

---
*Phase: 04-deepcpnoise-4d*
*Completed: 2026-03-17*
