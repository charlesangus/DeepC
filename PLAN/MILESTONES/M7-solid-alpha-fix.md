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

Blocked on: nothing — M6.P2.T2 ruled (2026-09-23, M6 file `## Decisions`): the fix is **local** —
split a saturated bucket's alpha over new vs co-located area from the *raw* `A_k`
(`u = min(A_raw/(C_raw+D_raw), 1)`, `aCov = u·C`, `aRes = u·D`, colour by `1/A_raw`) inside
`compositePixelCoveragePartition()`, not the depth-gated arrival plane. Pins that move: i7/i7d
(re-pin — their signal is this defect), g1/g2/g3/m3a (improve), scene (o)'s six XFAILs (→ PASS).
Reference patch: `~/deepc-validation/M6-P2T2/patches/m6-mut-all.patch` bit 64. T0: M6-T0 plus
M6's closing a–o tally. Elaborate from that ruling at promotion.

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
- scene (o)'s mutation runs (K=64, same-depth, sparse) are gated rather than note-only (PR #108
  review finding, deferred here): unpinned `oCheck` rows requiring 0 px past the bound.
- `./docker-build.sh --linux` green; PR to `master`. The hand-built AlmaLinux image and `zip`
  shim from `PLAN/DECISIONS/2026-09-06-docker-linux-gate-runs-here.md` are gone (no local images
  as of 2026-09-23) — rebuild them per that recipe first; M6 waived this gate, M7 cannot.
