# Phase 4: DeepCPNoise 4D - Research

**Researched:** 2026-03-16
**Domain:** FastNoise 4D algorithm extension + DeepCPNoise call-site cleanup
**Confidence:** HIGH

---

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions

#### 4D scope — all types, all variants
- All five noise types get full 4D support: Simplex, Perlin, Value, Cubic, Cellular
- All fractal variants (FBM, Billow, RigidMulti) also get 4D implementations
- 4D algorithms authored directly in `FastNoise.h` / `FastNoise.cpp` — no external dependency

#### 4D Cellular
- Implement 4D Cellular (Voronoi) in FastNoise — not skipped, not a fallback
- Requires 4D cell jitter tables and distance calculations in 4D hyperspace

#### UI model — no toggle, always-on
- `noise_evolution` knob stays as a plain float, always visible, always enabled for all noise types
- Non-zero evolution = 4D noise active; zero evolution = effectively 3D behavior (w=0 is a valid neutral value)
- No "Enable 4D" checkbox; no conditional knob gating

#### w dimension source
- `_noiseEvolution` float knob is the sole source for the w coordinate — static value set by the user
- Per-sample channel-driven w is explicitly deferred (see Deferred Ideas)

#### Magic index fix (deferred from Phase 1)
- Replace `if (_noiseType == 0)` with `if (noiseTypes[_noiseType] == FastNoise::SimplexFractal)` or equivalent named comparison
- Once all types support 4D, this branch is removed entirely — `GetNoise(x,y,z,w)` is called unconditionally for all types

#### Tooltips
- Claude's discretion — update to accurately reflect that all types now support 4D evolution
- Remove the "Simplex is the only 4D noise" text from the existing tooltips

### Claude's Discretion
- Tooltip wording: update to accurately reflect that all types now support 4D evolution

### Deferred Ideas (OUT OF SCOPE)
- **Per-sample w from deep channel**: source the 4th noise dimension from a deep channel (e.g., a time pass or custom channel) rather than a static knob
</user_constraints>

---

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| NOIS-01 | DeepCPNoise exposes a 4D noise option in the UI for all supported noise types (Simplex, Perlin, Cellular, Cubic, Value), wired to the existing FastNoise 4D methods | The existing `GetNoise(x,y,z,w)` dispatch is the entry point to extend; all 3D Single* methods provide direct templates; the existing 4D Simplex implementation is a complete reference. The `_noiseType==0` branch in `wrappedPerSample()` is the only DeepCPNoise call-site change needed. |
</phase_requirements>

---

## Summary

Phase 4 is predominantly a FastNoise algorithm authoring task with a small DeepCPNoise call-site cleanup. The project already contains a complete 4D Simplex implementation (`SingleSimplex(offset, x, y, z, w)` and three fractal variants) with all required support infrastructure (F4/G4 constants, `GRAD_4D[32*4]` table, `Index4D_32`, `Index4D_256`, `GradCoord4D`, `ValCoord4D`). That infrastructure is sufficient to implement 4D Value and 4D Perlin without any new tables. 4D Cellular is the only type requiring a new jitter table (`CELL_4D_X/Y/Z/W` — 256 unit-vectors in 4-space). 4D Cubic requires a 4D Catmull-Rom grid walk (4^4 = 256 `CubicLerp` calls), which is large but mechanical.

The DeepCPNoise call-site change is minimal: remove the `if (_noiseType==0)` branch and call `GetNoise(x,y,z,w)` unconditionally, passing `_noiseEvolution` as `w`. The `GetNoise(x,y,z,w)` dispatch in FastNoise must be extended to handle all noise type cases instead of returning 0 for non-Simplex types. No UI changes are needed beyond tooltip text updates.

**Primary recommendation:** Author 4D Single* algorithms bottom-up (Value → Perlin → Cubic → Cellular), extend `GetNoise(x,y,z,w)` dispatch, then collapse the DeepCPNoise call-site to a single unconditional call.

---

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| FastNoise (vendored) | 0.4.1 (local copy) | All noise generation | Already integrated; phase extends it in-place |
| Nuke NDK | 16+ | Plugin host API | Project-wide requirement |

### Supporting
No additional dependencies. All 4D math infrastructure is present in FastNoise.cpp.

---

## Architecture Patterns

### Recommended Project Structure

No structural changes needed. All new code lands in two existing files:

```
FastNoise/
├── FastNoise.h      # Add 4D method signatures (Value, Perlin, Cubic, Cellular)
└── FastNoise.cpp    # Implement 4D algorithm bodies; extend GetNoise(x,y,z,w) dispatch
src/
└── DeepCPNoise.cpp  # Remove if (_noiseType==0) branch; call GetNoise(x,y,z,w) unconditionally
                     # Update tooltip strings
```

### Pattern 1: Fractal wrapper pattern (established in codebase)

**What:** Each noise type has three fractal wrapper functions (FBM, Billow, RigidMulti) that loop over octaves calling the Single* base function with lacunarity/gain scaling. The Single* base function does the actual spatial hashing and interpolation.

**When to use:** All fractal variants for all new 4D types follow this exact pattern.

**Example (reference — existing 4D Simplex FBM):**
```cpp
// Source: FastNoise.cpp line 1059
FN_DECIMAL FastNoise::SingleSimplexFractalFBM(FN_DECIMAL x, FN_DECIMAL y, FN_DECIMAL z, FN_DECIMAL w) const
{
    FN_DECIMAL sum = SingleSimplex(0, x * m_frequency, y * m_frequency, z * m_frequency, w * m_frequency);
    FN_DECIMAL amp = 1;
    int i = 0;
    while (++i < m_octaves)
    {
        x *= m_lacunarity;
        y *= m_lacunarity;
        z *= m_lacunarity;
        w *= m_lacunarity;
        amp *= m_gain;
        sum += SingleSimplex(m_perm[i], x, y, z, w) * amp;
    }
    return sum * m_fractalBounding;
}
```

**Key detail:** The FBM 4D variant applies `m_frequency` scaling only in the initial call (first octave uses pre-scaled coords); note `SingleSimplexFractalFBM` receives pre-scaled x,y,z from `GetNoise`, but the 4D FBM does its own `x * m_frequency` on the first octave call — this is a quirk specific to 4D Simplex FBM compared to 3D. The 3D fractal variants receive already-scaled coords from `GetNoise`. Follow the same pattern per noise type.

### Pattern 2: Single* base function pattern — Value/Perlin 4D

**What:** Grid-cell integer floor, compute fractional offsets for all four axes, apply interpolation function, trilinearly-interpolated → quadrilinearly-interpolated for 4D (2^4 = 16 corner samples, lerped down through 4 dimensions).

**Existing 3D Value as template (FastNoise.cpp line 631):**
```cpp
// 3D Value: floor → xs/ys/zs interp weights → 8 corner lerp cascade
// 4D Value: add w dimension → ws interp weight → 16 corner lerp cascade (4 dimensions)
// Uses: Index4D_256 for hashing, VAL_LUT for corner values
// Bounding: no explicit bounding constant — 3D value returns Lerp result directly
```

**Existing 3D Perlin as template (FastNoise.cpp line 859):**
```cpp
// 3D Perlin: floor → xs/ys/zs interp weights → GradCoord3D for 8 corners
// 4D Perlin: add w dimension → GradCoord4D for 16 corners
// GradCoord4D already exists (line 324) — uses Index4D_32 + GRAD_4D[32*4]
```

### Pattern 3: Single* base function pattern — Cubic 4D

**What:** Catmull-Rom cubic interpolation. 3D Cubic uses a 4^3 = 64-sample neighborhood (four points per axis), nested `CubicLerp` calls. 4D Cubic extends to 4^4 = 256 samples.

**Scale constant:** 3D Cubic uses `CUBIC_3D_BOUNDING = 1 / (1.5^3)`. 4D Cubic needs `CUBIC_4D_BOUNDING = 1 / (1.5^4)`.

**Existing 3D Cubic Single (FastNoise.cpp line 1629):**
```cpp
// Pattern: w0=floor-1, w1=floor, w2=floor+1, w3=floor+2 for each axis
// Nested CubicLerp(CubicLerp(...), ...) cascade
// 4D extension: add w axis → outermost CubicLerp over 4 z-slabs,
//   each z-slab is the full 3D Cubic structure, producing 4 values
//   that are combined with CubicLerp(v0, v1, v2, v3, ws)
// Uses: ValCoord3DFast → needs ValCoord4DFast (new inline helper using Index4D_256)
```

**Note:** `ValCoord4DFast` is a new private helper needed in both `.h` declaration and `.cpp` implementation. It follows the same pattern as `ValCoord3DFast`:
```cpp
inline FN_DECIMAL ValCoord4DFast(unsigned char offset, int x, int y, int z, int w) const;
// Implementation: return VAL_LUT[Index4D_256(offset, x, y, z, w)];
```

### Pattern 4: Single* base function pattern — Cellular 4D

**What:** 4D Voronoi. Search over a 3^4 = 81-cell neighborhood of integer grid points around the input point, find minimum distance cell, return value based on `m_cellularReturnType`.

**Requires new data table:** The 3D cellular uses `CELL_3D_X/Y/Z[256]` — pre-randomized unit-vector components for cell jitter. 4D needs `CELL_4D_X/Y/Z/W[256]` — 256 4D unit vectors (or at minimum 256 random values for each of 4 components, normalized to unit length in 4-space).

**Distance functions extend directly:**
- Euclidean: `vecX*vecX + vecY*vecY + vecZ*vecZ + vecW*vecW`
- Manhattan: `|vecX| + |vecY| + |vecZ| + |vecW|`
- Natural: Manhattan + Euclidean

**CellValue return:** Uses `ValCoord4D(m_seed, xc, yc, zc, wc)` — already exists (line 292).

**NoiseLookup return:** Calls `m_cellularNoiseLookup->GetNoise(...)` on the nearest cell point — extend to pass 4D coordinates or leave as 3D lookup (jittered x/y/z only). The CONTEXT.md says "full 4D cellular" so pass all four jittered coordinates to a `GetNoise(x,y,z,w)` call.

**SingleCellular2Edge 4D:** Same structure as SingleCellular but tracks the two closest cells. Add `wc` tracking alongside `xc/yc/zc`.

### Pattern 5: GetNoise(x,y,z,w) dispatch extension

**Current state (FastNoise.cpp line 331):** The 4D dispatch handles only Simplex and SimplexFractal, returning 0 for all other types.

**After extension:** Add all noise type cases matching the 3D dispatch structure:
```cpp
// In GetNoise(x, y, z, w):
// Frequency scaling: x *= m_frequency; y *= m_frequency; z *= m_frequency; w *= m_frequency;
// Then dispatch to:
case Value:        return SingleValue(0, x, y, z, w);
case ValueFractal: // FBM/Billow/RigidMulti
case Perlin:       return SinglePerlin(0, x, y, z, w);
case PerlinFractal: // FBM/Billow/RigidMulti
case Cellular:     // SingleCellular / SingleCellular2Edge
case Cubic:        return SingleCubic(0, x, y, z, w);
case CubicFractal: // FBM/Billow/RigidMulti
// Simplex/SimplexFractal — already present, keep as-is
// WhiteNoise — GetWhiteNoise already has 4D overload, can add here
```

**Note:** The 4D Simplex FBM wrapper does not pre-scale x/y/z/w before the first octave call (it does `x * m_frequency` inside the fractal function). Verify this pattern is consistent for new types — the 3D fractal wrappers receive pre-scaled coords from `GetNoise`. For consistency, the new 4D fractal wrappers should receive pre-scaled coords too (i.e., `GetNoise` scales first, then passes to `SingleXxxFractalFBM`).

### Pattern 6: DeepCPNoise call-site simplification

**Current state (DeepCPNoise.cpp line 271-278):**
```cpp
// TODO: handle this better; fragile now - depends on order of noise list
if (_noiseType==0)
{
    // GetNoise(x, y, z, w) only implemented for simplex so far
    perSampleData = _fastNoise.GetNoise(position[0], position[1], position[2], _noiseEvolution);
} else
{
    perSampleData = _fastNoise.GetNoise(position[0], position[1], position[2]);
}
```

**After extension:** Replace entirely with:
```cpp
perSampleData = _fastNoise.GetNoise(position[0], position[1], position[2], _noiseEvolution);
```

The w=0 case (zero evolution) is correctly handled by all 4D algorithms because a zero w coordinate yields valid, stable noise output identical to the 3D slice at w=0.

### Anti-Patterns to Avoid

- **Returning 0 for unimplemented cases:** The existing `GetNoise(x,y,z,w)` dispatch's `default: return 0` silently produces black output for non-Simplex types. This is the current bug being fixed. Never silently return 0 for unimplemented noise variants after this phase.
- **Skipping `w *= m_lacunarity` in fractal loops:** Every axis must scale in the fractal loop. Missing `w` scaling makes w effectively freeze at its initial value across octaves.
- **Applying frequency scaling twice:** Some fractal functions receive pre-scaled coords; if `GetNoise` scales before calling the fractal wrapper, the wrapper must NOT apply `m_frequency` again. Verify the convention matches 3D behavior for each new type.
- **Forgetting `CUBIC_4D_BOUNDING`:** Without the normalization constant the Cubic 4D output will be approximately 1.5× too large in amplitude.

---

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| 4D gradient table | Custom gradient set | Use existing `GRAD_4D[128]` + `GradCoord4D` | Already correct for 4D simplex; reuse for Perlin 4D |
| 4D hash | New hash function | `Index4D_256` (line 264), `Index4D_32` (line 252) | Both already exist and are correctly seeded |
| 4D value coordinate | New integer hash | `ValCoord4D(seed, x, y, z, w)` (line 292) | Already exists with W_PRIME constant |
| Fractal bounding | Recalculate per-type | `m_fractalBounding` (set by `CalculateFractalBounding`) | Shared across all fractal types; already correct |

---

## Common Pitfalls

### Pitfall 1: Frequency double-scaling in 4D fractal variants
**What goes wrong:** 4D Simplex FBM (line 1059) applies `* m_frequency` inside its first octave call, but `GetNoise(x,y,z,w)` ALSO scales by `m_frequency` before calling the fractal function (line 333-336). The 4D Simplex FBM applies frequency to its own copy of coordinates — so `GetNoise` scales w but NOT the fractal wrapper's internal `w * m_frequency`. This is inconsistent with 3D: the 3D fractal wrappers receive pre-scaled coords and do NOT apply frequency internally.
**Why it happens:** The 4D Simplex FBM was written differently from the 3D fractal wrappers (line 1061 does `x * m_frequency` even though GetNoise already scaled). Verify the final frequency applied for new 4D fractal types matches the intent — new types should follow the 3D convention (GetNoise scales, fractal wrapper does not re-apply frequency).
**How to avoid:** New 4D fractal wrappers (Value, Perlin, Cubic, Cellular) should follow the 3D pattern: receive pre-scaled coordinates from `GetNoise`, do NOT multiply by `m_frequency` internally.
**Warning signs:** Noise appears finer/coarser than the frequency knob suggests.

### Pitfall 2: Missing w in fractal octave loop
**What goes wrong:** New fractal wrappers compile and run but the w dimension is frozen — the noise doesn't evolve correctly with `_noiseEvolution` at non-default lacunarity.
**Why it happens:** Developer adds x/y/z scaling but forgets `w *= m_lacunarity` in the octave loop.
**How to avoid:** Every axis (`x`, `y`, `z`, `w`) must be multiplied by `m_lacunarity` inside the fractal loop.

### Pitfall 3: 4D Cubic bounding constant omitted or wrong
**What goes wrong:** Cubic 4D output is outside [-1, 1], causing the grade to be extreme or clipped.
**Why it happens:** Forgetting `CUBIC_4D_BOUNDING` or using the 3D value `1/(1.5^3)`.
**How to avoid:** Define `const FN_DECIMAL CUBIC_4D_BOUNDING = 1 / (FN_DECIMAL(1.5) * FN_DECIMAL(1.5) * FN_DECIMAL(1.5) * FN_DECIMAL(1.5));` and apply it at the end of `SingleCubic` 4D.

### Pitfall 4: 4D Cellular neighborhood too small
**What goes wrong:** Cellular noise has visible seaming artifacts at grid boundaries.
**Why it happens:** Using a 3^3 = 27 neighborhood (±1 in each axis) for 4D instead of a 3^4 = 81 neighborhood (±1 in each of four axes).
**How to avoid:** The 4D cellular search must add an outer loop over `wi = wr - 1; wi <= wr + 1` to the 3D triple loop, making four nested loops total.

### Pitfall 5: CELL_4D jitter table generation
**What goes wrong:** 4D cellular produces obviously non-random or periodic patterns.
**Why it happens:** Using 3D unit vectors for 4D jitter (copying CELL_3D with a zero W component) produces degenerate Voronoi cells.
**How to avoid:** Generate 256 random 4D unit vectors (normalize random 4-component vectors to unit length). The existing `CELL_3D_X/Y/Z` arrays provide the pattern — new `CELL_4D_X/Y/Z/W` arrays follow the same structure with proper 4D normalization.

### Pitfall 6: noiseTypeNames array ordering vs FastNoise enum
**What goes wrong:** Selecting "Perlin" in the UI evaluates `noiseTypes[_noiseType]` to the wrong `FastNoise::NoiseType`, producing the wrong algorithm.
**Why it happens:** `noiseTypeNames[]` in DeepCPNoise.cpp has a custom order (Simplex=0, Cellular=1, Cubic=2, Perlin=3, Value=4) that does NOT match the `FastNoise::NoiseType` enum order. `noiseTypes[]` provides the mapping and is correct. The magic index `_noiseType==0` was correctly identifying Simplex (index 0 in the names array = SimplexFractal in noiseTypes array). After removing the branch, this mapping must remain intact.
**How to avoid:** Do not change `noiseTypeNames[]` or `noiseTypes[]` order. The unconditional `GetNoise(x,y,z,w)` call is safe because `_fastNoise.SetNoiseType(noiseTypes[_noiseType])` already sets the correct FastNoise enum on the instance.

---

## Code Examples

### GetNoise(x,y,z,w) extended dispatch skeleton
```cpp
// Source: FastNoise.cpp line 331 — extend this function
FN_DECIMAL FastNoise::GetNoise(FN_DECIMAL x, FN_DECIMAL y, FN_DECIMAL z, FN_DECIMAL w) const
{
    x *= m_frequency;
    y *= m_frequency;
    z *= m_frequency;
    w *= m_frequency;

    switch (m_noiseType)
    {
    case Value:
        return SingleValue(0, x, y, z, w);
    case ValueFractal:
        switch (m_fractalType)
        {
        case FBM:        return SingleValueFractalFBM(x, y, z, w);
        case Billow:     return SingleValueFractalBillow(x, y, z, w);
        case RigidMulti: return SingleValueFractalRigidMulti(x, y, z, w);
        default: return 0;
        }
    case Perlin:
        return SinglePerlin(0, x, y, z, w);
    case PerlinFractal:
        // ... same structure
    case Simplex:
        return SingleSimplex(0, x, y, z, w);
    case SimplexFractal:
        // ... existing, keep as-is
    case Cellular:
        switch (m_cellularReturnType)
        {
        case CellValue:
        case NoiseLookup:
        case Distance:
            return SingleCellular(x, y, z, w);
        default:
            return SingleCellular2Edge(x, y, z, w);
        }
    case Cubic:
        return SingleCubic(0, x, y, z, w);
    case CubicFractal:
        // ...
    default:
        return 0;
    }
}
```

### DeepCPNoise::wrappedPerSample simplified call
```cpp
// Source: DeepCPNoise.cpp line 271 — replace the conditional block
perSampleData = _fastNoise.GetNoise(position[0], position[1], position[2], _noiseEvolution);
perSampleData = perSampleData * .5f + .5f;
```

### New header declarations needed in FastNoise.h (//4D section)
```cpp
// Add to FastNoise.h alongside existing GetSimplex(x,y,z,w) declarations:
FN_DECIMAL GetValue(FN_DECIMAL x, FN_DECIMAL y, FN_DECIMAL z, FN_DECIMAL w) const;
FN_DECIMAL GetValueFractal(FN_DECIMAL x, FN_DECIMAL y, FN_DECIMAL z, FN_DECIMAL w) const;
FN_DECIMAL GetPerlin(FN_DECIMAL x, FN_DECIMAL y, FN_DECIMAL z, FN_DECIMAL w) const;
FN_DECIMAL GetPerlinFractal(FN_DECIMAL x, FN_DECIMAL y, FN_DECIMAL z, FN_DECIMAL w) const;
FN_DECIMAL GetCubic(FN_DECIMAL x, FN_DECIMAL y, FN_DECIMAL z, FN_DECIMAL w) const;
FN_DECIMAL GetCubicFractal(FN_DECIMAL x, FN_DECIMAL y, FN_DECIMAL z, FN_DECIMAL w) const;
FN_DECIMAL GetCellular(FN_DECIMAL x, FN_DECIMAL y, FN_DECIMAL z, FN_DECIMAL w) const;

// Private method declarations (// 4D section):
FN_DECIMAL SingleValueFractalFBM(FN_DECIMAL x, FN_DECIMAL y, FN_DECIMAL z, FN_DECIMAL w) const;
FN_DECIMAL SingleValueFractalBillow(FN_DECIMAL x, FN_DECIMAL y, FN_DECIMAL z, FN_DECIMAL w) const;
FN_DECIMAL SingleValueFractalRigidMulti(FN_DECIMAL x, FN_DECIMAL y, FN_DECIMAL z, FN_DECIMAL w) const;
FN_DECIMAL SingleValue(unsigned char offset, FN_DECIMAL x, FN_DECIMAL y, FN_DECIMAL z, FN_DECIMAL w) const;

FN_DECIMAL SinglePerlinFractalFBM(FN_DECIMAL x, FN_DECIMAL y, FN_DECIMAL z, FN_DECIMAL w) const;
FN_DECIMAL SinglePerlinFractalBillow(FN_DECIMAL x, FN_DECIMAL y, FN_DECIMAL z, FN_DECIMAL w) const;
FN_DECIMAL SinglePerlinFractalRigidMulti(FN_DECIMAL x, FN_DECIMAL y, FN_DECIMAL z, FN_DECIMAL w) const;
FN_DECIMAL SinglePerlin(unsigned char offset, FN_DECIMAL x, FN_DECIMAL y, FN_DECIMAL z, FN_DECIMAL w) const;

FN_DECIMAL SingleCubicFractalFBM(FN_DECIMAL x, FN_DECIMAL y, FN_DECIMAL z, FN_DECIMAL w) const;
FN_DECIMAL SingleCubicFractalBillow(FN_DECIMAL x, FN_DECIMAL y, FN_DECIMAL z, FN_DECIMAL w) const;
FN_DECIMAL SingleCubicFractalRigidMulti(FN_DECIMAL x, FN_DECIMAL y, FN_DECIMAL z, FN_DECIMAL w) const;
FN_DECIMAL SingleCubic(unsigned char offset, FN_DECIMAL x, FN_DECIMAL y, FN_DECIMAL z, FN_DECIMAL w) const;

FN_DECIMAL SingleCellular(FN_DECIMAL x, FN_DECIMAL y, FN_DECIMAL z, FN_DECIMAL w) const;
FN_DECIMAL SingleCellular2Edge(FN_DECIMAL x, FN_DECIMAL y, FN_DECIMAL z, FN_DECIMAL w) const;

inline FN_DECIMAL ValCoord4DFast(unsigned char offset, int x, int y, int z, int w) const;
```

### CELL_4D table structure (new data to add in FastNoise.cpp)
```cpp
// Pattern: 256 4D unit vectors, each component stored in a separate array
// These need to be pre-computed (or hard-coded) normalized 4D vectors.
// Convention matches CELL_3D_X/Y/Z pattern.
const FN_DECIMAL CELL_4D_X[256] = { /* 256 values */ };
const FN_DECIMAL CELL_4D_Y[256] = { /* 256 values */ };
const FN_DECIMAL CELL_4D_Z[256] = { /* 256 values */ };
const FN_DECIMAL CELL_4D_W[256] = { /* 256 values */ };
// Each (X[i], Y[i], Z[i], W[i]) is a unit vector in 4-space:
// sqrt(X[i]^2 + Y[i]^2 + Z[i]^2 + W[i]^2) == 1
```

---

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| Simplex-only 4D noise in GetNoise(x,y,z,w) | All types 4D-capable | This phase | All noise types gain temporal/4D evolution |
| `if (_noiseType==0)` magic index | Unconditional `GetNoise(x,y,z,w)` | This phase | Call-site is clean and type-safe |
| Tooltip says "Simplex: 4D noise" implying others are not | All tooltips describe uniform 4D support | This phase | Artist expectation matches reality |

---

## Open Questions

1. **CELL_4D jitter table values**
   - What we know: Need 256 normalized 4D vectors; the pattern is clear from `CELL_3D_X/Y/Z`.
   - What's unclear: Whether to generate these algorithmically at compile time (constexpr) or hard-code them as a literal array (as `CELL_3D_X/Y/Z` are hard-coded).
   - Recommendation: Hard-code a precomputed literal array following the same style as `CELL_3D_X/Y/Z`. Consistency with existing code is more important than compile-time elegance. Use a simple offline Python script to generate 256 normalized random 4D vectors and paste the values.

2. **Frequency scaling consistency in 4D fractal wrappers**
   - What we know: The existing 4D SimplexFractalFBM applies `m_frequency` internally (line 1061), which is inconsistent with 3D variants where `GetNoise` pre-scales. The 4D Simplex wrapper actually double-applies frequency if `GetNoise` also scales — reviewing lines 331-336 shows `GetNoise` scales x/y/z but NOT w, then calls the fractal wrapper which scales all four with `m_frequency`. This means w is NOT pre-scaled by `GetNoise` before the 4D Simplex call.
   - What's unclear: Whether new 4D fractal wrappers should follow the 3D pattern (pre-scaled by GetNoise) or the existing 4D Simplex pattern (wrapper applies frequency). The 4D GetNoise currently applies frequency to x,y,z but NOT to w. New types should match this to keep the call-site behavior uniform.
   - Recommendation: Follow the 3D pattern for new 4D types: `GetNoise(x,y,z,w)` scales all four coordinates by `m_frequency` before calling the Single* dispatch, and fractal wrappers do NOT re-apply frequency. This requires careful review of the 4D Simplex dispatch to confirm it isn't double-scaling in practice (line 341: `SingleSimplex(0, x, y, z, w)` — the non-fractal simplex uses already-scaled coords correctly; line 1061 in the fractal wrapper is a bug/inconsistency to be aware of but not introduce in new code).

---

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | None detected — no pytest/jest/vitest config, no test/ directory |
| Config file | None — Wave 0 gap |
| Quick run command | `cd /workspace/build && make DeepCPNoise 2>&1 | tail -5` (compilation check) |
| Full suite command | Manual: load DeepCPNoise in Nuke, set each noise type, adjust noise_evolution, verify non-black output |

### Phase Requirements → Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| NOIS-01 | All 5 noise types produce non-zero output when GetNoise(x,y,z,w) is called | smoke | `cd /workspace/build && make DeepCPNoise 2>&1 | grep -E 'error|warning'` — compilation success is the automated gate | ❌ Wave 0 |
| NOIS-01 | noise_evolution=0 produces same result as 3D call for all types | manual | Manual Nuke load test | N/A |
| NOIS-01 | noise_evolution != 0 changes output for all noise types | manual | Manual Nuke load test | N/A |
| NOIS-01 | Tooltip text no longer says "Simplex is the only 4D noise" | static | `grep -n "only 4D" /workspace/src/DeepCPNoise.cpp` returns empty | ❌ Wave 0 |

### Sampling Rate
- **Per task commit:** `cd /workspace/build && make DeepCPNoise 2>&1 | tail -10` (compilation clean)
- **Per wave merge:** Compilation clean + grep for removed dead code
- **Phase gate:** Full Nuke manual UAT before `/gsd:verify-work`

### Wave 0 Gaps
- [ ] No automated test framework exists — manual Nuke UAT is the acceptance gate for this phase
- [ ] `grep -n "only 4D" /workspace/src/DeepCPNoise.cpp` — tooltip regression check (trivial, no framework needed)

*(No existing test infrastructure. Compilation + manual UAT is the established project pattern.)*

---

## Sources

### Primary (HIGH confidence)
- `/workspace/FastNoise/FastNoise.h` — Full class interface, all existing 4D method signatures
- `/workspace/FastNoise/FastNoise.cpp` — All existing implementations: 4D Simplex (lines 1059-1118, 1449-1543), `GetNoise(x,y,z,w)` dispatch (lines 331-357), helper functions (lines 186-329)
- `/workspace/src/DeepCPNoise.cpp` — Full call-site, knob registration, noise type mapping arrays

### Secondary (MEDIUM confidence)
- `/workspace/.planning/phases/04-deepcpnoise-4d/04-CONTEXT.md` — User decisions locking scope and design choices

### Tertiary (LOW confidence)
- None — all findings are directly verified from source code.

---

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH — directly verified from source
- Architecture: HIGH — all patterns extracted from existing working code
- Pitfalls: HIGH — identified from direct inspection of existing dispatch and wrapper code
- CELL_4D table generation: MEDIUM — pattern is clear, specific values require offline generation

**Research date:** 2026-03-16
**Valid until:** Stable (FastNoise is vendored; no external dependency churn)
