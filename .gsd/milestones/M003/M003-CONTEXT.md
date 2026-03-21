# M003: DeepCBlur v2

**Gathered:** 2026-03-21
**Status:** Ready for planning

## Project Description

Improve DeepCBlur with algorithmic upgrades (separable blur, kernel accuracy tiers, alpha darkening correction) drawn from the earlier CMG99 effort at commit `9551704`, while maintaining the single-file simplicity of the current implementation. Also polish the UI to match Nuke conventions.

## Why This Milestone

DeepCBlur v1 (M002) works but has two problems: it's O(r²) per pixel making it unusably slow at larger radii, and it produces darkened output when composited because it doesn't account for how the over-operation interacts with blurred deep samples. CMG99 solved both of these in a 20-file framework that was never integrated. This milestone brings those ideas into our single-file plugin.

## User-Visible Outcome

### When this milestone is complete, the user can:

- Blur deep images at large radii without prohibitive render times (separable blur)
- Choose kernel quality level to trade accuracy for speed
- Enable alpha correction to fix compositing darkening artifacts
- Control blur size with a single WH_knob matching Nuke's built-in Blur convention

### Entry point / environment

- Entry point: Nuke node graph → DeepCBlur node
- Environment: Nuke 16+ on Linux (Docker build verified)
- Live dependencies involved: none

## Completion Class

- Contract complete means: DeepCBlur.cpp compiles via docker-build.sh, all knobs present, separable engine executes both passes
- Integration complete means: Plugin loads in Nuke, knobs render correctly, blur produces visible output
- Operational complete means: none

## Final Integrated Acceptance

To call this milestone complete, we must prove:

- docker-build.sh produces DeepCBlur.so in the release archive
- Separable blur is exercised (the 2D kernel loop is replaced by two 1D passes)
- All three kernel tiers generate valid kernels
- Alpha correction toggle changes output when enabled
- WH_knob and twirldown group are present in the knob layout

## Risks and Unknowns

- **Separable intermediate buffer** — deep pixels have variable sample counts per pixel, so the intermediate representation between H and V passes must store gathered sample lists, not flat channel arrays. This is the main architectural challenge.
- **Alpha correction ordering** — the correction must happen after both blur passes complete, on the fully blurred output. Getting the pass ordering wrong will produce incorrect results.

## Existing Codebase / Prior Art

- `src/DeepCBlur.cpp` — current non-separable implementation (337 lines), DeepFilterOp subclass
- `src/DeepSampleOptimizer.h` — header-only sample merge/cap utility, used by DeepCBlur
- `src/DeepCAdjustBBox.cpp` — reference for WH_knob usage with double[2] array
- `src/DeepThinner.cpp` — reference for BeginClosedGroup twirldown pattern
- `src/DeepCPNoise.cpp` — reference for BeginClosedGroup and Enumeration_knob patterns
- Commit `9551704` — CMG99's DeepCBlur v1.0 with separable blur, 3 kernel tiers, 3 blur modes (20 files, 1704 lines)
  - `DeepCBlur/src/BlurKernels.cpp` — kernel generation algorithms (LQ/MQ/HQ)
  - `DeepCBlur/src/BlurStrategy.cpp` — modified gaussian alpha correction logic
  - `DeepCBlur/src/DeepCBlur.cpp` — post-blur alpha correction in doDeepEngine

> See `.gsd/DECISIONS.md` for all architectural and pattern decisions — it is an append-only register; read it during planning, append to it during execution.

## Relevant Requirements

- R001 — separable blur (primary deliverable of S01)
- R002 — kernel tiers (S01)
- R003 — alpha correction (S02)
- R004 — WH_knob (S02)
- R005 — twirldown group (S02)
- R006 — correction toggle knob (S02)
- R007 — zero-blur fast path (S01, preservation)
- R008 — docker build (S02, verification)

## Scope

### In Scope

- Separable 2D Gaussian blur (H pass then V pass)
- Three kernel accuracy tiers (low/medium/high)
- Alpha darkening correction with enable/disable toggle
- WH_knob for blur size
- BeginClosedGroup for sample optimization knobs
- All changes within `src/DeepCBlur.cpp` (single-file plugin, no new framework)

### Out of Scope / Non-Goals

- Z-blur (depth-direction blur)
- Transparent modified gaussian mode
- New shared base classes or op-tree framework
- Channel selection knob

## Technical Constraints

- Must remain a single `DeepFilterOp` subclass in `src/DeepCBlur.cpp`
- Must compile with GCC 11.2.1, C++17, Nuke 16+ DDImage API
- `WH_knob` takes `double[2]` — internal blur params currently `float`, will need conversion
- Kernel computation uses 1D half-kernel (symmetric) for separable approach

## Integration Points

- `DeepSampleOptimizer.h` — per-pixel optimization continues to be called after blur, before emit
- `src/CMakeLists.txt` — no changes needed (DeepCBlur already listed)
- `docker-build.sh` — no changes needed (already builds DeepCBlur)

## Open Questions

- **Kernel normalization for LQ tier** — CMG99's low quality kernel is unnormalized. Should we normalize it anyway for consistency, or preserve the raw behavior? Current thinking: normalize all tiers for safety, label LQ as "fast" instead.
