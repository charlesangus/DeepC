# Milestone 9: Nested footprints — correlated occlusion in the coverage partition

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

M8.P1.T2's thin-lens reference (`~/deepc-validation/M8-P1T2/REPORT.md`) found a third mechanism behind
o6c. When a near surface's lens set lies inside a farther surface's (the near card's defocused
footprint nested in the stack's), the node's disjoint-area coverage partition over-weights the farther
surface by the near surface's lens area. Measured: +0.105 G/A at (155,128), +0.041 at (104,104),
+0.059 at (150,150). There is no effect in the interior. Fixing M8's mechanism (1) alone moves (155,128)
from 0.435 to 0.5225, further from the reference's 0.417. The user ruled this a separate milestone
(2026-09-24).

Blocked on: M8 shipping (per-tile buckets and the reference oracle are the base).

Acceptance sketch: o6c's nested-footprint pixels read colour:alpha against the thin-lens reference
within the tolerance class the user rules at M8.P1.T3 Q2; the full suite is green with every moved row
explained; the profile is within noise or the cost is accepted; the docker gate is green.

## Decisions

> **Added 2026-09-25 (from M8.P2.T5/P2.T8 consultants):** two more instances of the nesting term to cover. (1) A same-source-pixel
> fog + opaque stack whose CoC gap exceeds the pre-merge tolerance (or `pre_merge` off): at the stack's silhouette both lens sets
> coincide, the card is filed as disjoint, and the plane's overflow never rotates — o6 alpha 0.9093 at (100,100) (M8 rows: o6's
> `pre_merge`-off and gap-0.03 XFAIL variants, hard bound `1 − A ≤ (1 − fog α)·w_fog`). (2) A lone volumetric card's defocused
> rim (f4a: +0.150 alpha / +10.9 %) and its colour over an opaque card (f4b +4.0e-2 c:a): consecutive pieces of one body from
> neighbouring source pixels nest in lens space (DESIGN §5.4's per-parent chain is the candidate fix; M8.P2.T10 if ruled in).
> V3's residual after M8.P2.T11 is mostly sub-pixel kernel error, not nesting.
