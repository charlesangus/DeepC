# Rows gated against the thin-lens reference use a 3σ Monte Carlo bound

**Decision (user ruling, 2026-09-26, answers M8.P1.T3 Q2):** option (i). Where a harness row is gated against the
Monte Carlo thin-lens reference (M8.P1.T2's tool, landed as a harness oracle by M8.P3.T1), the bound is **3 × the
reference's standard error** at the gated pixels (reference SE ≤ 2.2e-4 on the o6 rig → ≈ 6.6e-4), plus the row's
float term count `N·2⁻²⁴`. The kernel's calibrated rim residual is *not* added; rows that use this bound exclude
the rim (e.g. `MIX_NEAR_BOX` inset 2). Rejected: (i) with the rim residual added (≈ 1.5e-3), (ii) an analytic
double-precision reference for axis-aligned cards, (iii) report-only.

**Rationale:** the reference is the ruled oracle; its sampling error is the only honest slack against it, and 3σ
keeps the gate tight enough to catch the interior error M8 removed (T0 read 0.0756).

**Applies to / consequences:** o6e, o6c's re-oracle, Phase 8.3/8.5 rows, and any later reference-gated row. It is
the one exception to `2026-09-18-no-8bit-tolerances.md`'s term-count-only rule. The margin is thin (M8 prototype
max 6e-4): if a row exceeds it, reduce the reference's SE (more samples, fixed seed) — never widen the bound — and
if it still fails, it goes to the user.
