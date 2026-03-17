# Phase 4: DeepCPNoise 4D - Context

**Gathered:** 2026-03-16
**Status:** Ready for planning

<domain>
## Phase Boundary

Extend FastNoise with full 4D implementations for all noise types (Perlin, Value, Cubic, Cellular, Simplex — plus all fractal variants), then wire the `noise_evolution` knob in DeepCPNoise to always pass the w dimension to all GetNoise calls. Fix the `_noiseType == 0` magic index comparison deferred from Phase 1. No new UI controls needed beyond the existing knob.

</domain>

<decisions>
## Implementation Decisions

### 4D scope — all types, all variants
- All five noise types get full 4D support: Simplex, Perlin, Value, Cubic, Cellular
- All fractal variants (FBM, Billow, RigidMulti) also get 4D implementations
- 4D algorithms authored directly in `FastNoise.h` / `FastNoise.cpp` — no external dependency

### 4D Cellular
- Implement 4D Cellular (Voronoi) in FastNoise — not skipped, not a fallback
- Requires 4D cell jitter tables and distance calculations in 4D hyperspace

### UI model — no toggle, always-on
- `noise_evolution` knob stays as a plain float, always visible, always enabled for all noise types
- Non-zero evolution = 4D noise active; zero evolution = effectively 3D behavior (w=0 is a valid neutral value)
- No "Enable 4D" checkbox; no conditional knob gating

### w dimension source
- `_noiseEvolution` float knob is the sole source for the w coordinate — static value set by the user
- Per-sample channel-driven w is explicitly deferred (see Deferred Ideas)

### Magic index fix (deferred from Phase 1)
- Replace `if (_noiseType == 0)` with `if (noiseTypes[_noiseType] == FastNoise::SimplexFractal)` or equivalent named comparison
- Once all types support 4D, this branch is removed entirely — `GetNoise(x,y,z,w)` is called unconditionally for all types

### Tooltips
- Claude's discretion — update to accurately reflect that all types now support 4D evolution
- Remove the "Simplex is the only 4D noise" text from the existing tooltips

</decisions>

<specifics>
## Specific Ideas

- The core design intent: once the FastNoise extension is done, 4D becomes a uniform feature of all noise types — there is no "Simplex is special" path remaining in DeepCPNoise
- Future idea (not this phase): source the w coordinate from a deep channel (e.g., a time pass or custom 4th channel) per-sample rather than from a static knob

</specifics>

<code_context>
## Existing Code Insights

### Reusable Assets
- `FastNoise::GetNoise(x,y,z,w)`: existing 4D dispatch method — currently only handles Simplex/SimplexFractal, returns 0 for all other types. This is the dispatch entry point to extend.
- `FastNoise::SingleSimplexFractalFBM(x,y,z,w)` and siblings: existing 4D Simplex fractal methods — reference implementations for how fractal 4D is structured in FastNoise
- `DeepCPNoise::wrappedPerSample()`: the call site where `_noiseEvolution` is used as w — once FastNoise supports all types in 4D, the Simplex branch collapses to a single unconditional `GetNoise(x,y,z,w)` call
- `_noiseEvolution` float member + `Float_knob`: already registered and persisted — no changes needed to the knob itself

### Established Patterns
- FastNoise internal naming convention: `SinglePerlinFractalFBM`, `SingleValue`, etc. — new 4D methods follow the same naming with a `w` parameter added
- `_validate()` precomputes cached values; no changes needed there for this phase
- Grade coefficients (A, B, G arrays) left as-is — SWEEP-08 was dropped

### Integration Points
- `FastNoise.h`: add 4D method signatures (`GetPerlin4D`, `GetValue4D`, `GetCubic4D`, `GetCellular4D` plus fractal variants, or extend the existing `GetNoise(x,y,z,w)` dispatch)
- `FastNoise.cpp`: implement the 4D algorithm bodies
- `DeepCPNoise::wrappedPerSample()`: remove the `if (_noiseType==0)` branch; call `GetNoise(x,y,z,w)` unconditionally with `_noiseEvolution` as w
- Tooltip strings in `custom_knobs()`: update to remove Simplex-only 4D references

</code_context>

<deferred>
## Deferred Ideas

- **Per-sample w from deep channel**: source the 4th noise dimension from a deep channel (e.g., a time pass or custom channel) rather than a static knob. Interesting for per-sample variation, but out of scope for this phase.

</deferred>

---

*Phase: 04-deepcpnoise-4d*
*Context gathered: 2026-03-16*
