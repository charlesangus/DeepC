# Coverage fill replaces the honest-alpha dip, with no legacy knob

**2026-09-10.** `DeepCDefocus` v1 shipped an "honest coverage dip": where the scatter's arriving
weight summed to less than 1 — either side of the focal line when CoC varies spatially, and around
the silhouettes of foreground objects where the renderer wrote no occluded background samples — the
node reported the reduced opacity rather than inventing coverage. Deficits were only ever clamped
down, never up. This was a deliberate v1 contract, documented in the node help's "COVERAGE DEFICIT
(specified behaviour)" block and pinned by validation scene (i) and several XFAILs.

**The contract is retired.** The node now renormalizes, like pgBokeh and like every 2D defocus:
each output pixel is divided by the coverage that arrived at it, so opacity holds up even where the
renderer wrote no occluded samples. This is the **only** behaviour — the user explicitly ruled out a
legacy knob or opt-out, so there is no path back to the v1 output.

Rationale: the artifacts are real, measured, and visible in ordinary work — a plane defocused at 45°
to camera shows opacity bands up to 7.4e-2 deep, and foreground objects show alpha halos about one
CoC wide. Bokeh and pgBokeh show neither. 2D nodes avoid the problem implicitly, because a flat image
has a source pixel under every output pixel and their scatter weight sums are ~1 everywhere. Honesty
about a deficit is worth less than matching the tools compositors already trust.

Two properties bound what the fill is allowed to invent, and both are pinned by tests:
- It divides by **un-held-out** arrival — the divisor ignores holdout visibility while the numerator
  carries it — so holdout attenuation survives exactly and fill and holdout **commute**.
- It invents **foreground-coloured** coverage only where the renderer wrote none. It never invents
  background colour.

Because there is no legacy path, the behaviour change and the re-specification of scene (i), the
g4/g5 pins and every honest-alpha doc block must land in the same PR or the validation suite is red.
Implemented by Milestone 4; the design is `PLAN/REFERENCE/COVERAGE-FILL-PLAN-v2.md`.
