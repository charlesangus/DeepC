# M005-9j5er8: DeepCDepthBlur Correctness

**Gathered:** 2026-03-21
**Status:** Ready for planning

## Project Description

Fix DeepCDepthBlur's alpha decomposition so the flatten invariant actually holds under Nuke's multiplicative deep compositing model. The current code linearly scales alpha by weight (`α * w`), but `flatten()` computes `1 - ∏(1-αᵢ)`, not `Σαᵢ`. This produces three observable bugs: flattened output doesn't match input, zero-alpha samples appear in output, and solid images become semi-transparent.

Also rename the second input from "B" to "ref" (Nuke convention reserves "B" for main/background input), and make the main input unlabelled.

## Why This Milestone

M004-ks4br0/S02 shipped DeepCDepthBlur with structurally incorrect alpha math. The flatten invariant (R013) was claimed but never actually held. User testing confirmed all three symptoms. This is a correctness fix, not a feature addition.

## User-Visible Outcome

### When this milestone is complete, the user can:

- Connect DeepCDepthBlur in Nuke and verify that `DeepToImage(DeepCDepthBlur(input)) == DeepToImage(input)` — the flattened image is identical
- Use any falloff mode, spread value, and sample count without producing zero-alpha samples
- See the second input pipe labeled "ref" (not "B"), with the main input unlabelled

### Entry point / environment

- Entry point: Nuke node graph — DeepRead → DeepCDepthBlur → DeepToImage
- Environment: Nuke 16+ on Linux (Docker build for .so)
- Live dependencies involved: none

## Completion Class

- Contract complete means: docker-build.sh exits 0, syntax check passes, alpha math uses `1-(1-α)^w` formula
- Integration complete means: .so loads in Nuke 16+ and processes deep images correctly
- Operational complete means: none (single-shot image processing node)

## Final Integrated Acceptance

To call this milestone complete, we must prove:

- `docker-build.sh --linux --versions 16.0` exits 0 with DeepCDepthBlur.so in the output zip
- The spreading code uses `α_sub = 1 - pow(1-α, w)` for each sub-sample, not `α * w`
- Premultiplied channels scale as `c * (α_sub / α)` to maintain premult correctness
- Zero-alpha input samples (α < 1e-6) pass through without spreading
- No sub-sample with α ≈ 0 is emitted
- Second input labeled "ref", main input unlabelled

## Risks and Unknowns

- **Numerical precision at extreme alpha values** — `pow(1-α, w)` for α very close to 1.0 and small w may produce sub-samples with α ≈ 0. Need a clamp or skip threshold.

## Existing Codebase / Prior Art

- `src/DeepCDepthBlur.cpp` — the 491-line plugin to be modified. Weight generators, B-input gating, tidy pass all stay; only the spreading section of `doDeepEngine` and `input_label` change.
- `src/DeepSampleOptimizer.h` — uses `alpha_sub = 1 - pow(1-alpha, subRange/totalRange)` for volumetric subdivision in `tidyOverlapping`. Same mathematical family as what we need here.
- `scripts/verify-s01-syntax.sh` — syntax check covering DeepCDepthBlur.cpp with mock headers.

> See `.gsd/DECISIONS.md` for all architectural and pattern decisions — it is an append-only register; read it during planning, append to it during execution.

## Relevant Requirements

- R013 — Flatten invariant: `flatten(output) == flatten(input)` via multiplicative alpha decomposition
- R017 — Second input renamed to "ref", main unlabelled, always visible, optional
- R018 — No zero-alpha samples in output

## Scope

### In Scope

- Fix alpha decomposition math in `doDeepEngine` spreading section
- Filter/skip zero-alpha sub-samples and near-zero-alpha input samples
- Rename second input from "B" to "ref", main input unlabelled
- Adjust premultiplied channel scaling to match new alpha formula
- Verify via docker build and syntax check

### Out of Scope / Non-Goals

- No new knobs or falloff modes
- No changes to weight generators (they stay normalised to sum=1 — weights become exponents, not multipliers)
- No changes to tidy pass logic
- No changes to DeepSampleOptimizer.h

## Technical Constraints

- Nuke 16+ only, C++17, GCC 11.2.1
- DDImage DeepFilterOp subclass pattern
- Must build via `docker-build.sh --linux --versions 16.0`

## Integration Points

- None external — self-contained Nuke plugin

## Open Questions

- None — the math is well-understood from the volumetric subdivision pattern in `DeepSampleOptimizer.h`
