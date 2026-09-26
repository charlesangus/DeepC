# Milestone 10: Holdout in front of a deep stack — parity with DeepHoldout2

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Found by M5.P3.T1 (2026-09-11), pre-existing and not the fill's: a 0.5-alpha holdout placed *in front of* a
two-layer deep stack renders alpha 1.0 / R/A 0.50 inside the silhouette, where stock `DeepHoldout2` gives 0.5 / 0.80
(`DeepMerge2`'s holdout op gives 0.75 / 0.60). Reproduced on the M5-T0 `.so`, in `fill: foreground`, at size 0,
with `pre_merge` off. Scene (b) never sees it because its holdout sits *between* its layers, where all three agree.
The user ruled it a defect (2026-09-26): depth-correct holdouts are the node's headline feature, so a holdout in
front must hold out. This milestone diagnoses why the in-front holdout's transmittance is lost, fixes it, and
extends scene (b) so the case is gated.

Blocked on: M8 shipping — M8 replaces the bucket planes with the depth-ordered streaming composite, which rewrites
the path the holdout transmittance goes through, so the diagnosis must run on post-M8 code (M8 may even change the
reading).

Acceptance sketch:
- Scene (b) gains holdout-in-front cells (0.5-alpha holdout in front of a two-layer stack, at size 0 and defocused,
  `pre_merge` on and off, both `fill` modes); at size 0 they match `DeepHoldout2` (alpha 0.5, R/A 0.80) at a
  term-count bound, and defocused cells are gated against an independent oracle, not the new output.
- The existing n7 note is updated and the row gated; the holdout stays pixel-sharp.
- Full a–o suite green with every moved row explained; profile within noise or the cost accepted; docker gate green.
