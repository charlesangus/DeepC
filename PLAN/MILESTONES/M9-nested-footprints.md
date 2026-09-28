# Milestone 9: Nested footprints — correlated occlusion in the coverage partition (research)

> **Revived 2026-09-27 as research** (`PLAN/DECISIONS/2026-09-27-m9-revived-as-research.md`). It was cancelled on 2026-09-26
> when its scope was folded into M8 Phase 8.5. M8.P5.T1 then found no per-deposit rule that works
> (`~/deepc-validation/M8-P5T1/DESIGN.md`), so the fix moves back here. M8 stays `blocked` until this lands, on the same
> branch. M8's P3.T2 and gate run after this milestone, and M8's PR carries both.

Fix the nesting term: when a near surface's lens set lies inside a farther surface's, the stream's scalar coverage state
over-weights the farther surface and takes free area in emission order. The work starts from P5.T1's findings:
- a claimed-region model per destination pixel (candidate C, two disc segments + bank) is too coarse at corners;
- receding surfaces break area conservation in the kernel's weights, so position-blocking leaks alpha, and a bank refill
  re-adds the over-read;
- even the ideal region raster read 1.33e-2 on the o6f band (8× the bound).
So the research has to go after area conservation on receding surfaces as well as the region representation.

**Constraints (user rulings):**
- No body identity (2026-09-26).
- **No per-sample depth gradient from neighbouring pixels** (2026-09-27): only a sample's own depth range, alpha and position.
- The n8 rows' order dependence **must be fixed**, not XFAILed.
- K-invariance and determinism (o7/o8) hold. The two-fog-layers identity holds. Memory/profile cost goes to the user if
  material.

Targets, each against its oracle (HEAD readings in the M8 file's `## Decisions`, 2026-09-27 P5.T1 entry):
- o6f band;
- o6g/o6h;
- f4a/f4b/f4c (+r);
- f3e/f3f/f3h/f3h2/f3i;
- n8c/n8cr/n8dr.

Elaborated 2026-09-27 by a planning consultant (scratch runs in `.evidence/scratch/M9-plan/`).

Acceptance sketch:
- every target row PASSes at its ruled bound (thin-lens reference: rim residual + 3·SE + term count; vref/derived bounds
  elsewhere);
- n8c transpose symmetry is within its derived bound;
- no row that passes today regresses;
- profile within noise, or the cost is accepted by the user.

## What we know (read before any task)

- **M8.P5.T1** (`.evidence/validation/M8-P5T1/DESIGN.md`) — candidate C:
  - two disc-segment slots + bank;
  - cuts the o6f band from 0.115 to 4.7e-3 and n8c from 0.174 to 5.9e-3, but misses every bound;
  - regresses f4b (PASS → FAIL) and f3e/f3f (45 %/88 %);
  - memory 36 → 88 B/px; scalar driver 5.5× slower.
- **P5.T1's "ideal raster, 8×" was a hybrid**, not a per-cell transmittance compositor: kernel weights × geometric fraction,
  still behind the F cap and the scalar recency chunks.
- **The o6f floor with no continuity is 10× the bound** (`.evidence/scratch/M9-plan/`, `o6box_r0.txt`, `floor.py`).
  - `thinlens_ref`'s risers join edge-adjacent same-layer samples: layer identity + adjacency.
  - o6's plane is point samples (zFront == zBack), so its own depth range carries no slope.
  - risers 0 vs 1 on the 2 460 band px: max |ΔG/A| 1.49e-2, mean 4.2e-3, 1 459 px past, |Δα| to 6.5e-2.
- **C reads closer to risers-on truth (4.7e-3) than the pure-geometric floor (1.49e-2).** Within-surface scalar area (the
  kernel's adjoint sum S≈1) is the only gradient-free stand-in for riser area.
- **Every lens tile is an axis-aligned square** (pixel footprint ÷ r). A rectilinear region model is therefore exact at card and
  box corners (C's failure 2) and on straight edges.
- **n8 is fronto-parallel** (z 640), so order is the whole n8c problem; commutative per-generation delivery is the remedy.
  **The board alone already fails n8dr's bound** (5.8e-3, 2.9×, 72 of 732 px): the kernel at r≈3.9 is outside the rim
  allowance calibrated at r 14/17, so n8dr needs a derived per-pixel K(p) like vref's kernel mode.
- **The generation key is the crux.**
  - HEAD's `lastCoc` chain (kCocJumpRotatePx) pools the near card, plane and stack at (130,101).
  - Keying on any CoC change makes every plane row its own region: alpha leaks, and g4/g5/m3c are at risk.
- **o6d** (node vs LO(P)) cannot hold on the nested set by construction; it needs re-scoping to non-nesting pixels.
- **Baseline:** M8 HEAD `53fc335` reads `PASS=203 FAIL=7 XFAIL=8 SKIP=3`; the FAILs are f3e/f3f/f3h/f3h2, n8c/n8cr/n8dr.
- **Consultant's feasibility read (2026-09-27):** under the strict reading there is no path to the whole target list; there is a
  realistic one if the user admits destination-side continuity (a kCocJump-class heuristic) and a derived n8dr K(p).
  - n8c/n8cr: a real path.
  - o6g/o6h, f4, f3: plausible (exact rectilinear geometry).
  - o6f: blocked on a strict reading.
  - Cost: ≥2× plane memory and a material deposit slowdown.
  - Phase 9.2 puts this to the user with measurements.

## Phase 9.1: Feasibility spikes

- [ ] M9.P1.T1 — Rebuild the P5.T1 driver at HEAD and extend it with the rigs, orderings and ideal rule the spikes need
  - files: `.evidence/validation/M9-P1/tools/{p9drv.cpp, rules.inc, build.sh, suite.sh, gates.py}` (evidence only; no repo files)
  - approach:
    - Copy p5drv2 + proto_seg.inc onto a fresh scratch tree at `53fc335`.
    - New rigs: g4/g5/m3c planes, each gated against its `kernel_sums` S-law oracle; n8 with `fill: background`.
    - Emission-order switch: reverse tie order, seeded shuffle of equal-depth ties, transpose; plus an n8 variant with a
      0.01·y board tilt.
    - Rule `geom`: per ROI pixel, a 512² lens raster with float transmittance per cell, compositing the node's own
      deposits (post pre-merge and volumetric split) as exact square tiles ∩ disc, with no kernel weights.
    - A `thinlens_ref` wrapper running risers 0 and 1 on the same dump.
  - verify:
    - `head` is bit-identical to production on o6, V1–V3, n8 and uneq.
    - Every recorded HEAD reading in P5.T1 §0 is reproduced.
    - `geom` matches `thinlens_ref` risers=0 on an isolated fronto-parallel card within 3·SE + raster quantisation (derived
      from cell size × edge length).
  - size: M
  - depends: —

- [ ] M9.P1.T2 — Spike A (area conservation): measure each target's floor for position-exact and continuity-closing rules
  - files: `.evidence/validation/M9-P1/spikeA/` (driver rule variants + note)
  - approach: for every target row, plus g4/g5/m3c, the o6c/o6e core, f3b/f3c/f3d/f3j and fog2, measure:
    - (a) `thinlens_ref` risers 0 vs 1 — the continuity term;
    - (b) `geom` vs risers 0 — the preprocessing + kernel residual;
    - (c) `geom` with destination-side closing (merge tiles abutting on the lens within one tile width when
      |ΔCoC| < kCocJumpRotatePx) vs risers 1;
    - (d) hybrid `geom`: position-exact across generations, HEAD's scalar recency within, with generation keys lastCoc
      chain / exact-CoC equality / lens-adjacency + CoC.
  - verify:
    - Note tabulates, per row and variant, worst ×bound, px past and mean.
    - A row is reachable only if some admissible variant is ≤1× its bound.
    - o6f must be ≤1× under (c) or (d), or it is unreachable without an oracle ruling.
    - g4/g5/m3c stay within their S-law gate under the chosen key.
  - size: L
  - depends: M9.P1.T1

- [ ] M9.P1.T3 — Spike B (region representation): prototype a rectilinear AABB-slot region model and measure it against the Spike A ideal
  - files: `.evidence/validation/M9-P1/spikeB/{proto_aabb.inc, note}`
  - approach:
    - Each slot holds one lens AABB (4 floats) + transmittance (or area and mass); a tile's claimed fraction is an exact box ∩ box.
    - Slot assignment: overlap or abut within one tile width; on overflow, a deterministic merge of the least-union-growth pair.
    - Sweep N ∈ {2,3,4,6}, both merge policies, and the generation keys that survived Spike A.
    - Record memory B/px, scalar cost and `-fopt-info-vec` status; the body must be select-only.
  - verify: full row suite against the Spike A ideal and the oracles. Go needs:
    - every target within 1.2× of its Spike A reachable floor;
    - f4b, f3b/f3c/f3d/f3j, g4/g5/m3c and o6c/o6e still PASS;
    - fog2 interior within term count;
    - f3e/f3f no worse than HEAD;
    - K 4/16/64 byte-identical;
    - memory and predicted deposit cost stated.
  - size: L
  - depends: M9.P1.T2

- [ ] M9.P1.T4 — Spike C (order independence): prototype commutative per-generation delivery and derive n8dr's kernel term
  - files: `.evidence/validation/M9-P1/spikeC/{proto_gen.inc, n8kernel.py, note}`
  - approach:
    - Replace the per-deposit `min(w(1−g), F)` cap with a pending generation buffer (demand D, α·w and c·w masses),
      delivered at generation close or resolve, scaled by `min(1, F/D)` or by geometric new area / D. Equal-depth ties then
      commute exactly.
    - Test on the `geom` ideal and the Spike B model, under every T1 emission order and the 0.01·y tilt.
    - Derive n8dr's K(p) from a board-only node render vs board-only thin-lens, as vref's kernel mode does. Board-only
      reads 5.8e-3, 72 of 732 px past.
  - verify:
    - n8c/n8cr ≤ 1.33e-3 under every ordering and both fill modes.
    - n8dr within rim + 3·SE + terms + K(p), with K(p) derived, not fitted.
    - n8c's colour arm is bit-symmetric where the fill is foreground.
  - size: M
  - depends: M9.P1.T1

- [ ] M9.P1.T5 — Design note: the chosen rule, predicted readings, cost and the user questions
  - files: `.evidence/validation/M9-P1/DESIGN.md`
  - approach:
    - Combine A–C into one rule.
    - Predicted readings for every target and every at-risk row: g, m, i6/i7, o0c, a1–a3, f3 density.
    - Identities: two-fog, K-invariance, size-0 parity, determinism by construction.
    - Memory (B/px, `bytesForBand`); scalar and estimated vectorised deposit cost.
    - Go/no-go per target.
  - verify: every figure traces to a run in `spikeA`–`spikeC`; any row predicted outside its bound is named, with the reason.
  - size: S
  - depends: M9.P1.T2, M9.P1.T3, M9.P1.T4

## Phase 9.2: User ruling

- [ ] M9.P2.T1 — USER RULING: go/no-go on M9.P1.T5, plus the oracle and cost questions it raises
  - files: `PLAN/DECISIONS/<date>-m9-go-nogo.md`, this file
  - approach: the PM puts the note to the user with these decisions:
    1. go / no-go, overall and per target row;
    2. is destination-side continuity (the lastCoc chain or lens-adjacency closing, the same heuristic class as HEAD's
       kCocJumpRotatePx) admissible under "no body identity / no gradient"? Needed if Spike A shows o6f reachable only
       with it;
    3. the o6f oracle for point-sampled receding surfaces:
       - keep risers-on;
       - risers-on + a derived |ref(r1) − ref(r0)| term in the bound;
       - risers-off;
    4. n8dr's derived K(p);
    5. o6d re-scoped to non-nesting pixels;
    6. memory and profile cost acceptance.
  - verify: the ruling is recorded in `DECISIONS/` and on the board. On a no-go, M9 closes and M8's plan is revisited.
  - size: S
  - depends: M9.P1.T5

## Phase 9.3: Implementation (finalised after M9.P2.T1)

- [ ] M9.P3.T1 — Region and generation state in the stream deposit
  - files: `src/DeepCDefocusScatter.h`, `src/DeepCDefocusScatter.cpp`, `src/DeepCDefocusMath.h` (only if a LUT survives),
    `tests/test_defocus_scatter.cpp`
  - approach:
    - Ruled slot and pending-generation planes go in `StreamPlanes`/`StreamPlaneView`, counted in `bytesForBand`.
    - `depositStreamSpanRecency` gains tile geometry (srcX, srcY, dstX0, dstY) and the ruled region + generation logic.
    - The body stays select-only and `DEEPC_HD`/nvcc-clean.
  - verify: doctests, each new assertion mutation-tested:
    - straight-edge and box-corner nesting against an analytic lens-set oracle at a term-count bound;
    - two fog layers at term count;
    - equal-depth deposits bit-identical under permutation;
    - size-0 bit-identical to `53fc335`;
    - the α-0.8 volumetric `deposits·2⁻²⁴` rig unchanged;
    - vectorisation report under the shipped flags.
  - size: L
  - depends: M9.P2.T1

- [ ] M9.P3.T2 — Resolve-time generation close, plane budgeting and the probe
  - files: `src/DeepCDefocusScatter.h`, `src/DeepCDefocusScatter.cpp`, `src/DeepCDefocus.cpp`, `tests/test_defocus_scatter.cpp`
  - approach:
    - `resolveStreamPixel` commits the pending generation before the fill and clamp.
    - `planBands`/`bandBudgetBytes` count the new planes.
    - Probe columns expose slot and generation state.
  - verify:
    - Doctests: resolve with a pending generation; fill with a deficit.
    - Byte-identical output across forced band heights 1/7/96, `-m 1` vs `-m 2`, and memory_limit.
    - Headless load OK.
  - size: M
  - depends: M9.P3.T1

- [ ] M9.P3.T3 — Ruled oracle support: continuity term and n8dr kernel term
  - files: `tests/reference/thinlens_ref.cpp`, `tests/nuke/scenes.py`, `tests/CMakeLists.txt` (if a mode is added)
  - approach, per M9.P2.T1:
    - risers-0/1 output in one pass and the per-pixel continuity term;
    - a thin-lens kernel mode giving K(p) for the n8 board;
    - harness helpers that read both.
  - verify:
    - existing o0c/o6c/o6e/n8d readings bit-unchanged;
    - the new terms reproduce the spike figures;
    - halved CoC and a half-pixel shift both FAIL.
  - size: M
  - depends: M9.P2.T1

- [ ] M9.P3.T4 — Re-read and re-pin scene (o) against the new rule
  - files: `tests/nuke/scenes.py`
  - approach:
    - o6f band rows (alpha + c/a) at the ruled bound.
    - o6g/o6h at their derived bound; the XFAIL comes off if they pass.
    - o6d re-scoped as ruled.
    - Every moved o row recorded with its cause.
  - verify:
    - scene (o) tally;
    - each new or changed row FAILs on `53fc335` or a scratch mutation;
    - every alpha row has a c:a twin.
  - size: L
  - depends: M9.P3.T2, M9.P3.T3

- [ ] M9.P3.T5 — Re-read and re-pin scenes f, g and n
  - files: `tests/nuke/scenes.py`
  - approach:
    - Re-read f4a/f4b/f4c (+r), f3e/f3f/f3h/f3h2/f3i, f3b/f3c/f3d/f3j, n8c/n8cr/n8dr (with K(p)) and g4/g5/m3c.
    - Stale pins are re-derived, never re-fitted.
  - verify:
    - every target at its bound;
    - `moved-rows.md` in `.evidence/validation/M9-P3T5/`;
    - mutations recorded.
  - size: L
  - depends: M9.P3.T2, M9.P3.T3

- [ ] M9.P3.T6 — Node text and design comments for the new rule
  - files: `src/DeepCDefocus.cpp`, `src/DeepCDefocusScatter.h`
  - approach:
    - KNOWN LIMITATIONS drops the nesting and order items and states any ruled continuity assumption.
    - The pre_merge tooltip is updated if o6g changed its advice.
    - Comment policy applies.
  - verify: the comment checker is clean; the help text shows in headless Nuke.
  - size: S
  - depends: M9.P3.T4, M9.P3.T5

## Phase 9.4: Gate

- [ ] M9.P4.T1 — M9 gate
  - files: none (evidence `.evidence/validation/M9-gate/`)
  - approach:
    - Full a–o in batches under `scripts/hostguard.sh`.
    - Doctests.
    - Determinism across band heights 1/7/96, `-m 1`/`2` and thread count.
    - Profile at 2048×1080, 20 spp, K=16, 2 threads: 5 reps interleaved with M8 HEAD `53fc335`.
  - verify: as the gate line below.
  - size: M
  - depends: M9.P3.T6

**Verification gate:**
- Every target row (o6f band, o6g/o6h, f4a/f4b/f4c (+r), f3e/f3f/f3h/f3h2/f3i, n8c/n8cr/n8dr) PASSes at its ruled bound;
  n8c's transpose symmetry holds within its derived bound.
- No regression vs M8 HEAD `PASS=203 FAIL=7 XFAIL=8 SKIP=3`: the seven FAILs become PASS, and every row passing today
  still passes.
- Doctests green.
- Output byte-identical across band plan, memory_limit and threads; K 4/16/64 byte-identical.
- The two-fog identity holds at term count.
- Profile within noise of `53fc335`, or the cost accepted by the user.
- **No PR here**: M8's P3.T2 and gate follow, and M8's PR carries both milestones.

## Decisions
