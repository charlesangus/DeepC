# Milestone 8: Depth-ordered colour within a bucket, and the silhouette-edge oracle

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Scene (o)'s o6c (M7.P3.T5) pins a pre-existing colour defect, colour:alpha up to 0.28 off Bokeh, with
alpha exact. It has two mechanisms (M7 file `## Decisions`, M7.P3.T5 entry; evidence
`~/deepc-validation/M7-P3T5/`):
(1) A near surface and the surface it hides land in one depth bucket (K=4 outright; K=16/64 via the
near card's depth-split share). The bucket's summed alpha is saturated as one, mixing them by alpha
(0.89 : 1.0) instead of in depth order. This is a node defect. The recommended fix is per-tile adaptive
bucket boundaries that keep separate surfaces out of one bucket: it keeps the additive planes and the
atomic splat, so only the bucket-centre table changes for M3. The alternative is extra per-bucket
depth-moment planes (about 2× plane memory). Per-fragment order within a bucket was rejected.
(2) On a defocused silhouette the node weights the surface by thin-lens disc coverage (0.52 half a
pixel inside the edge), where Bokeh reads 0.84. Which is right is undecided: the user asked for an
independent reference before choosing.

Blocked on: M7 shipping (its composite and scene (o) rows are the base). Mechanism (2) needs a spike
that builds an independent reference render of the o6 rig (e.g. brute-force ray-traced thin-lens
integration over the deep samples) before its acceptance can be written.

Acceptance sketch:
- a spike reports the reference's silhouette-edge weighting against the node (0.52) and Bokeh (0.84),
  and the user rules which is the oracle;
- o6c's mechanism-(1) cells read colour:alpha within their term-count bound against the chosen oracle,
  flipping XFAIL → PASS; mechanism-(2) cells are re-pinned or re-oracled per the ruling;
- the full suite is green with every moved row explained; the profile stays within noise of M7 (or the
  cost is measured and accepted); the docker gate is green.
