# M003: DeepCBlur v2

**Vision:** Upgrade DeepCBlur with separable blur for performance, kernel accuracy tiers for quality control, alpha darkening correction for compositing correctness, and UI polish to match Nuke conventions — all within the existing single-file plugin.

## Success Criteria

- Blur at radius 20 completes in roughly 1/10th the time of the current non-separable implementation
- All three kernel tiers produce valid, visually distinct output
- Alpha correction visibly fixes the darkening artifact when compositing blurred deep images
- Blur size uses a single WH_knob with lock toggle
- Sample optimization knobs are hidden in a twirldown by default
- docker-build.sh produces DeepCBlur.so in the release archive

## Key Risks / Unknowns

- **Separable intermediate buffer** — deep pixels have variable sample counts, so the H→V intermediate must store sample lists per pixel, not flat arrays. This is the main structural challenge.
- **Alpha correction pass ordering** — correction must run after both blur passes complete. Getting this wrong produces incorrect output that's hard to diagnose.

## Proof Strategy

- Separable intermediate buffer → retire in S01 by proving the two-pass engine compiles and the H→V intermediate correctly propagates variable-length sample arrays
- Alpha correction ordering → retire in S02 by proving correction produces visibly different (brighter) output compared to uncorrected

## Verification Classes

- Contract verification: docker-build.sh compiles DeepCBlur.so; g++ -fsyntax-only passes
- Integration verification: none (no cross-plugin wiring changes)
- Operational verification: none
- UAT / human verification: visual comparison of blur output in Nuke (separable vs non-separable parity, correction on vs off)

## Milestone Definition of Done

This milestone is complete only when all are true:

- Separable blur engine replaces non-separable in doDeepEngine
- All three kernel tiers are selectable via enum knob
- Alpha correction toggle exists and changes output when enabled
- WH_knob replaces separate Float_knobs for blur size
- Sample optimization knobs are in a closed group
- Zero-blur fast path still works
- docker-build.sh exit 0 with DeepCBlur.so in archive

## Requirement Coverage

- Covers: R001, R002, R003, R004, R005, R006, R007, R008
- Partially covers: none
- Leaves for later: none
- Orphan risks: none

## Slices

- [x] **S01: Separable blur + kernel tiers** `risk:high` `depends:[]`
  > After this: DeepCBlur uses two-pass separable Gaussian blur with three kernel accuracy tiers selectable via enum knob. Compiles via docker-build.sh.

- [ ] **S02: Alpha correction + UI polish** `risk:medium` `depends:[S01]`
  > After this: WH_knob controls blur size, sample optimization knobs are in a twirldown, alpha correction toggle fixes compositing darkening. Full docker build verified.

## Boundary Map

### S01 → S02

Produces:
- `doDeepEngine()` refactored to two-pass separable blur (H then V) with per-pixel intermediate sample buffer
- 1D kernel generation functions: `computeKernelLQ()`, `computeKernelMQ()`, `computeKernelHQ()` (static methods or free functions in DeepCBlur.cpp)
- `_kernelQuality` enum knob (int, 0/1/2) controlling kernel tier selection
- `_blurWidth` / `_blurHeight` member variables still exist as floats derived from whatever knob feeds them
- Zero-blur fast path preserved

Consumes:
- nothing (first slice)

### S02 consumes from S01

Consumes:
- Separable doDeepEngine() with clean per-pixel output loop (alpha correction inserts as a post-pass before emit)
- `_blurWidth` / `_blurHeight` floats (will be sourced from WH_knob double[2] instead of separate Float_knobs)
- Kernel quality enum knob (already in knobs() layout — S02 arranges UI around it)

Produces:
- `_blurSize[2]` double array with WH_knob replacing separate Float_knobs
- `BeginClosedGroup("Sample Optimization")` wrapping merge/color tolerance + max samples
- `_alphaCorrection` bool knob + post-blur correction pass in doDeepEngine
- Updated help text reflecting new capabilities
