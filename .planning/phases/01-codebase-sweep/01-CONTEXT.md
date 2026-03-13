# Phase 1: Codebase Sweep - Context

**Gathered:** 2026-03-12
**Status:** Ready for planning

<domain>
## Phase Boundary

Fix all confirmed bugs (SWEEP-01 through SWEEP-06, SWEEP-10), resolve the DeepCBlink fate (SWEEP-09), and opportunistically clean up adjacent issues found while touching files. SWEEP-07 (perSampleData redesign) and SWEEP-08 (grade coefficient extraction) are explicitly out of scope — both were dropped as over-engineering after analysis.

</domain>

<decisions>
## Implementation Decisions

### DeepCBlink fate (SWEEP-09)
- Remove entirely: delete `src/DeepCBlink.cpp`, remove CMake target, remove toolbar entry from `python/menu.py.in`
- No stub, no deprecation comment, no backward-compatibility shim — clean deletion
- Rationale: ships a broken UX (GPU knob that never activates GPU, silent bail-out on >4 channels, two calloc leaks). Not worth completing.

### SWEEP-07 — perSampleData redesign
- **Dropped.** Every current subclass passes exactly one scalar. The one exception (DeepCHueShift) already uses the existing `Vector3& sampleColor` parameter. Changing to `float* + int len` would be pure churn with no real benefit. Leave the interface as-is.

### SWEEP-08 — Grade coefficient extraction
- **Dropped.** Two copies of well-understood grade math (DeepCGrade and DeepCPNoise) is a fine tradeoff versus introducing a shared header/utility for a single reuse. Leave both files unchanged on this point.

### Sweep breadth
- Grab opportunistic fixes in files we're already touching:
  - **DeepCWorld**: cache `inverse_window_matrix` in `_validate()` instead of recomputing per sample (one-liner)
  - **DeepCWrapper**: delete the two commented-out `++inData; ++outData;` lines (dead code)
  - **batchInstall.sh**: fix the copy-paste comment error (Linux branch labelled "Mac OS X")
- **DeepCPNoise magic index** (`_noiseType == 0`): leave for Phase 4 — that phase is specifically about DeepCPNoise and will need the fix anyway

### Claude's Discretion
- Commit granularity within the phase (per-bug vs. grouped)
- Order of bug fixes within the phase
- Exact wording of any error messages added

</decisions>

<specifics>
## Specific Ideas

No specific requirements — open to standard approaches.

</specifics>

<code_context>
## Existing Code Insights

### Reusable Assets
- `DeepCWrapper::doDeepEngine()`: the loop where most bug fixes land (masking, unpremult, mix pipeline)
- `FastNoise::SimplexFractal` enum value: the correct way to identify Simplex in DeepCPNoise (used in noiseTypes[] mapping)

### Established Patterns
- Plugin registration via `static Op::Description` + `build()` at bottom of each `.cpp` — removing DeepCBlink means removing these lines too
- `_validate()` is the right place to precompute cached values (e.g. inverse matrix)
- Each plugin is one `.cpp` file; no shared headers between plugins except the wrapper base classes

### Integration Points
- `src/CMakeLists.txt`: contains `PLUGINS_BLINK` list — DeepCBlink target removed here
- `python/menu.py.in`: CMake template with categorized node lists — DeepCBlink entry removed here
- `DeepCWrapper.h` / `DeepCMWrapper.h`: virtual hook signatures — left unchanged (SWEEP-07 dropped)

</code_context>

<deferred>
## Deferred Ideas

- SWEEP-07 (perSampleData pointer+length redesign) — dropped, not deferred; rationale: no subclass needs more than one scalar, existing Vector3 sampleColor parameter handles the only multi-value case
- SWEEP-08 (grade coefficient shared utility) — dropped, not deferred; rationale: two copies is acceptable for a single reuse
- DeepCPNoise magic index fix (`_noiseType == 0`) — deferred to Phase 4 (DeepCPNoise 4D), where the file will be touched intentionally and the fix is required anyway

</deferred>

---

*Phase: 01-codebase-sweep*
*Context gathered: 2026-03-12*
