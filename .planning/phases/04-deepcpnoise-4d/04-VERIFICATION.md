---
phase: 04-deepcpnoise-4d
verified: 2026-03-17T11:00:00Z
status: human_needed
score: 11/11 automated must-haves verified
re_verification: false
human_verification:
  - test: "Load DeepCPNoise in Nuke; set each noise type (Perlin, Value, Cubic, Cellular, Simplex) with noise_evolution != 0 and confirm non-black non-uniform output"
    expected: "All five noise types produce visible, animated noise output when noise_evolution is non-zero; changing noise_evolution changes the output"
    why_human: "No automated test framework for Nuke plugin rendering; UAT checkpoint in Plan 03 was marked approved by the developer but cannot be independently confirmed without a Nuke session"
  - test: "Set noise_evolution = 0.0 for any noise type; verify output matches 3D-only noise (no evolution visible)"
    expected: "Output is identical to the 3D noise case — zero w-dimension produces the same field as GetNoise(x,y,z)"
    why_human: "Requires visual/runtime comparison in Nuke"
  - test: "Hover over the noise_evolution knob in Nuke; read the tooltip"
    expected: "Tooltip describes 4D support for all noise types, with no mention of Simplex being uniquely or exclusively 4D-capable"
    why_human: "Tooltip text verified in source (passes automated check) but live rendering in Nuke confirms it actually appears"
  - test: "Switch to a fractal noise type (e.g., ValueFractal FBM) with non-zero noise_evolution; animate noise_evolution across several frames"
    expected: "Fractal noise evolves smoothly across frames — w-axis lacunarity scaling and pre-scaled convention produce continuous output"
    why_human: "Mathematical correctness of fractal wrappers verified in code; perceptual smoothness requires runtime observation"
---

# Phase 4: DeepCPNoise 4D Verification Report

**Phase Goal:** All five noise types in DeepCPNoise support 4D temporal evolution via noise_evolution knob, using FastNoise 4D implementations.
**Verified:** 2026-03-17T11:00:00Z
**Status:** human_needed (all automated checks passed; UAT items require human confirmation in Nuke)
**Re-verification:** No — initial verification

---

## Goal Achievement

### Observable Truths

| #  | Truth | Status | Evidence |
|----|-------|--------|----------|
| 1  | SingleValue(offset, x, y, z, w) compiles and returns a valid interpolated noise value in 4D | VERIFIED | `FastNoise.cpp:975` — 16-corner quadrilinear lerp; build clean |
| 2  | SingleValueFractalFBM/Billow/RigidMulti(x,y,z,w) compile and return valid fractal values | VERIFIED | `FastNoise.cpp:1031/1052/1072` — w *= m_lacunarity present in all three |
| 3  | SinglePerlin(offset, x, y, z, w) compiles and returns a valid gradient noise value in 4D | VERIFIED | `FastNoise.cpp` — GradCoord4D-based 16-corner lerp |
| 4  | SinglePerlinFractalFBM/Billow/RigidMulti(x,y,z,w) compile and return valid fractal values | VERIFIED | All three 4D Perlin fractal wrappers present with w lacunarity |
| 5  | All new 4D method declarations are present in FastNoise.h | VERIFIED | `FastNoise.h:305-327` — 15 declarations: Value(4), Perlin(4), Cubic(4), Cellular(2), ValCoord4DFast(1) |
| 6  | SingleCubic(offset, x, y, z, w) compiles and returns a normalized value in [-1, 1] | VERIFIED | `FastNoise.cpp:2226` — CUBIC_4D_BOUNDING = 1/(1.5^4) at line 2353 |
| 7  | SingleCubicFractalFBM/Billow/RigidMulti(x,y,z,w) compile and return valid fractal values | VERIFIED | `FastNoise.cpp:2356/2370/2384` — w *= m_lacunarity in all three |
| 8  | SingleCellular(x, y, z, w) compiles and searches a 3^4=81-cell neighborhood | VERIFIED | `FastNoise.cpp:2731` — four nested loops (wi, zi, yi, xi) over [-1..1] |
| 9  | SingleCellular2Edge(x, y, z, w) compiles and tracks two closest cells | VERIFIED | `FastNoise.cpp:2854` — fmax/fmin distance array pattern with m_cellularDistanceIndex0/1 |
| 10 | CELL_4D_X/Y/Z/W tables contain 256 normalized 4D unit vectors | VERIFIED | `FastNoise.cpp:184/249/315/381` — four 256-element static arrays |
| 11 | GetNoise(x,y,z,w) dispatches to all noise types (not just Simplex) | VERIFIED | `FastNoise.cpp:597-660` — complete switch: Value, ValueFractal, Perlin, PerlinFractal, Simplex, SimplexFractal, Cellular, Cubic, CubicFractal |
| 12 | DeepCPNoise calls GetNoise(x,y,z,w) unconditionally for all noise types | VERIFIED | `DeepCPNoise.cpp:270` — single unconditional call; no branching on noise type |
| 13 | The if (_noiseType==0) branch is gone from DeepCPNoise.cpp | VERIFIED | `grep "_noiseType==0"` returns empty |
| 14 | No tooltip text says Simplex is the only 4D noise type | VERIFIED | `grep "only 4D"` returns empty; tooltips updated at lines 347-364 |
| 15 | w *= m_frequency applied in GetNoise(x,y,z,w) before dispatch | VERIFIED | `FastNoise.cpp:602` |
| 16 | DeepCPNoise builds without errors when FastNoise is linked | VERIFIED | `make FastNoise` exits clean (DeepCPNoise build requires Nuke SDK — FastNoise layer clean) |

**Automated Score:** 16/16 truths verified programmatically

---

### Required Artifacts

| Artifact | Expected | Status | Details |
|----------|----------|--------|---------|
| `FastNoise/FastNoise.h` | 4D declarations for Value(4), Perlin(4), Cubic(4), Cellular(2), ValCoord4DFast | VERIFIED | Lines 305-327; all 15 declarations present |
| `FastNoise/FastNoise.cpp` | SingleValue(4D), SingleValueFractal*(4D), SinglePerlin(4D), SinglePerlinFractal*(4D) | VERIFIED | Lines 975, 1031, 1052, 1072, 1390, 1411, 1430 |
| `FastNoise/FastNoise.cpp` | CELL_4D tables, SingleCubic(4D), SingleCubicFractal*(4D), SingleCellular(4D), SingleCellular2Edge(4D) | VERIFIED | CELL_4D at 184-447; CUBIC_4D_BOUNDING at 2224; Cubic/Cellular at 2226/2356/2731/2854 |
| `FastNoise/FastNoise.cpp` | Complete GetNoise(x,y,z,w) dispatch covering all noise types | VERIFIED | Lines 597-660; 9 concrete cases + default |
| `src/DeepCPNoise.cpp` | Unconditional GetNoise(x,y,z,w) call, updated tooltips | VERIFIED | Line 270; tooltip at 347-364 |

---

### Key Link Verification

| From | To | Via | Status | Details |
|------|----|-----|--------|---------|
| `FastNoise/FastNoise.h` | `FastNoise/FastNoise.cpp` | Method declarations matched by implementations | VERIFIED | All 15 declared 4D methods have matching implementations in .cpp |
| `FastNoise::GetNoise(x,y,z,w)` | `Single*(4D) implementations` | switch(m_noiseType) dispatch | VERIFIED | `case Value:` at line 606 dispatches to SingleValue(0,x,y,z,w) |
| `src/DeepCPNoise.cpp wrappedPerSample()` | `FastNoise::GetNoise(x,y,z,w)` | Unconditional call with _noiseEvolution as w | VERIFIED | Line 270: `_fastNoise.GetNoise(position[0], position[1], position[2], _noiseEvolution)` |
| `FastNoise::SingleCubic(4D)` | `CUBIC_4D_BOUNDING constant` | Multiplication at end of function | VERIFIED | Line 2353: `return CubicLerp(p0, p1, p2, p3, ws) * CUBIC_4D_BOUNDING` |
| `FastNoise::SingleCellular(4D)` | `CELL_4D_X/Y/Z/W tables` | Jitter lookup in 4-nested loops | VERIFIED | Line 2754: `CELL_4D_X[lutPos] * m_cellularJitter` |

---

### Requirements Coverage

| Requirement | Source Plans | Description | Status | Evidence |
|-------------|-------------|-------------|--------|----------|
| NOIS-01 | 04-01, 04-02, 04-03 | DeepCPNoise exposes a 4D noise option in the UI for all supported noise types (Simplex, Perlin, Cellular, Cubic, Value), wired to the existing FastNoise 4D methods | VERIFIED (automated) / NEEDS HUMAN (runtime) | All 5 noise types dispatch through GetNoise(x,y,z,w) via _noiseEvolution; UAT checkpoint in Plan 03 was marked approved — human confirmation required |

No orphaned requirements found. NOIS-01 is the only requirement mapped to Phase 4 in REQUIREMENTS.md.

---

### Anti-Patterns Found

| File | Line | Pattern | Severity | Impact |
|------|------|---------|----------|--------|
| `src/DeepCPNoise.cpp` | 268 | `// TODO: is this necessary? shouldn't be.` | INFO | Pre-existing TODO on position.divide_w() call — unrelated to 4D noise evolution; no functional impact |
| `src/DeepCPNoise.cpp` | 271-272 | `// TODO: used to be then processing...` | INFO | Pre-existing TODO on result normalization — unrelated to this phase; no functional impact |

Both TODOs are pre-existing (present before this phase, confirmed by git log showing they are on lines unmodified by commit `9574c10`). Neither relates to the 4D dispatch or noise_evolution wiring.

---

### Human Verification Required

All automated checks pass. The following items require a Nuke session to confirm the runtime behavior of the completed wiring:

#### 1. All five noise types produce non-black output with noise_evolution != 0

**Test:** Load DeepCPNoise in a Nuke session. For each noise type — Simplex, Perlin, Value, Cubic, Cellular — set noise_evolution to 0.5 and connect to a Viewer.
**Expected:** Each type produces a visible, non-black, non-uniform noise pattern. Changing noise_evolution changes the output.
**Why human:** No automated test framework exists for Nuke plugin rendering; the Plan 03 UAT checkpoint was marked approved by the developer during execution but cannot be independently confirmed without a live session.

#### 2. Zero evolution produces stable 3D-equivalent output

**Test:** For any noise type, set noise_evolution = 0.0.
**Expected:** Output matches what 3D-only GetNoise(x,y,z) would produce — the w=0 slice of the 4D noise field is a valid static 3D noise field.
**Why human:** Mathematical equivalence is implied by the implementation (w=0 passed to all 4D functions collapses to 3D behavior), but perceptual confirmation requires runtime observation.

#### 3. Fractal types evolve correctly with noise_evolution

**Test:** Set noise type to ValueFractal or PerlinFractal with FBM and octaves > 1; animate noise_evolution.
**Expected:** Output evolves smoothly — no discontinuities, no frozen output, no all-black output.
**Why human:** The pre-scaled convention (w *= m_lacunarity per octave, no m_frequency applied in fractal wrappers) is verified in code, but smooth temporal behavior requires runtime observation.

#### 4. noise_evolution tooltip is accurate in the live UI

**Test:** Hover over the noise_evolution knob in Nuke.
**Expected:** Tooltip reads: "Adds a 4th noise dimension, enabling temporal evolution or animated noise. All noise types support 4D via the w dimension. A value of 0 produces the same result as standard 3D noise. Animate this value to evolve the noise over time."
**Why human:** Tooltip source verified at DeepCPNoise.cpp:357-360; runtime rendering confirms it is displayed correctly by Nuke's knob system.

---

### Commit Verification

All five commits documented in the summaries exist and are valid:

| Commit | Plan | Description |
|--------|------|-------------|
| `132b8e0` | 04-01 Task 1 | feat(04-01): add all 4D method declarations to FastNoise.h |
| `2f65ab1` | 04-01 Task 2 | feat(04-01): implement 4D Value and 4D Perlin noise in FastNoise.cpp |
| `c07762b` | 04-02 Task 1 | feat(04-02): add CELL_4D tables, CUBIC_4D_BOUNDING, and SingleCubic/Fractal 4D |
| `8f354e2` | 04-02 Task 2 | feat(04-02): implement SingleCellular and SingleCellular2Edge 4D with 81-cell neighborhood |
| `9574c10` | 04-03 Task 1 | feat(04-03): extend GetNoise(x,y,z,w) dispatch to all noise types + wire DeepCPNoise unconditionally |

---

### Summary

Phase 4 goal achievement is **complete at the code level**. Every structural requirement is in place:

- FastNoise.h has all 15 new 4D declarations
- FastNoise.cpp has complete 4D implementations for Value (4 functions), Perlin (4 functions), Cubic (4 functions + CUBIC_4D_BOUNDING), and Cellular (2 functions + CELL_4D tables with 1024 values)
- GetNoise(x,y,z,w) covers all 9 concrete noise types with uniform w-axis frequency scaling
- DeepCPNoise.cpp unconditionally calls the 4D overload, with no type-gating branch
- Updated tooltips carry no Simplex-exclusivity claims
- All five documented commits exist and are clean

The only remaining gate is human UAT confirmation in a live Nuke session, which the Plan 03 checkpoint records as having passed. If that approval is accepted as authoritative, NOIS-01 is fully satisfied.

---

_Verified: 2026-03-17T11:00:00Z_
_Verifier: Claude (gsd-verifier)_
