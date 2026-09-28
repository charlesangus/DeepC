# Milestone 9: Nested footprints — correlated occlusion in the coverage partition (research)

> **Revived 2026-09-27 as research** (`PLAN/DECISIONS/2026-09-27-m9-revived-as-research.md`). It was cancelled on 2026-09-26
> when its scope was folded into M8 Phase 8.5. M8.P5.T1 then found no per-deposit rule that works
> (`~/deepc-validation/M8-P5T1/DESIGN.md`), so the fix moves back here. M8 stays `blocked` until this lands, on the same
> branch. M8's P3.T2 and gate run after this milestone, and M8's PR carries both.

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

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

Blocked on: M8.P5.T1's no-go and the 2026-09-27 rulings (resolved), so the next step is elaboration. Being research, this
milestone most likely opens with a feasibility spike and a go/no-go back to the user.

Acceptance sketch:
- every target row PASSes at its ruled bound (thin-lens reference: rim residual + 3·SE + term count; vref/derived bounds
  elsewhere);
- n8c transpose symmetry is within its derived bound;
- no row that passes today regresses;
- profile within noise, or the cost is accepted by the user.
