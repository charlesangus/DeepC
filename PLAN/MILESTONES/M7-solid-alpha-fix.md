# Milestone 7: Solid alpha on opaque geometry — the fix

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Makes `DeepCDefocus` read alpha **exactly `1.0`** wherever opaque geometry covers the output
pixel — on a slanted opaque plane with small opaque objects in front of it, around the objects,
across the plane, and where object discs overlap — matching Bokeh on the identical deep scene.
The change lands in the scatter/composite/fill chain (`src/DeepCDefocusScatter.h`/`.cpp`) at the
term M6's ruling names: either a local correction in `compositePixelCoveragePartition()`'s
accounting or the fill's gates, or — if the ruling says so — the depth-gated ("same-surface")
arrival plane M4 recorded as its structural follow-up. Scene (o)'s XFAILs are the acceptance
check: they retire to PASS at `1 − α == 0` within a term-count ulp bound, with the colour:alpha
ratio pinned beside every alpha arm. Not a fill option and not a knob: this is a correctness
change to the default behaviour, like M4. Runs after M6 and before M2.

Blocked on: M6.P2.T2's mechanism ruling — which of H1–H6 (or what else) produces each dip class,
and whether the fix is local or structural. The task list, the pins that will legitimately move
(m3c's surplus band, g4/g5, c1's accumulation bound are the candidates), and the T0 for the
before/after comparison all follow from that ruling.

Acceptance sketch:
- scene (o): every dip cell's alpha arm reads `1 − α == 0` within its ulp bound and flips XFAIL → PASS;
  o5's DeepCDefocus−Bokeh alpha difference is zero over the covered box; colour-ratio arms PASS.
- the full a–o suite is green with every moved a–n reading explained against M6-T0 and re-pinned
  against an independent oracle (never against the new output), every surviving XFAIL keeping a
  hard outer bound.
- size-0 parity (a) stays bit-exact; empties stay exactly black (d); fog identities and holdout
  commutation (m4a/m4b, n7) hold; `fill: background`'s twin identity (n1) holds.
- profile within noise of M5's foreground figure (28.0 s median at 2K/20spp, 2 threads) unless
  the ruling's approach is structural, in which case the cost is measured and reported.
- `./docker-build.sh --linux` green; PR to `master`.
