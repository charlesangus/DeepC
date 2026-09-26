# Rows gated against the thin-lens reference: rim residual + 3σ bound

**Decision (user ruling, 2026-09-26, answers M8.P1.T3 Q2):** option (i), a derived bound. Where a harness row is
gated against the Monte Carlo thin-lens reference (M8.P1.T2's tool, landed as a harness oracle by M8.P3.T1), the
bound is **the kernel's calibrated rim residual + 3 × the reference's standard error** at the gated pixels, plus
the row's float term count `N·2⁻²⁴`. On the o6 rig: residual ≤ 8.6e-4 (P1.T2 calibration) + 3 × SE ≤ 2.2e-4 →
≈ 1.5e-3. The user first read as 3σ alone (≈ 6.6e-4), then corrected it the same day to include the rim residual.
Rejected: 3σ alone, (ii) an analytic double-precision reference for axis-aligned cards, (iii) report-only.

**Rationale:** the reference is the ruled oracle; its sampling error and the kernel's calibrated discretisation
error are the two measured, independent sources of slack against it. The bound keeps about 2.5× margin over the M8
prototype's interior max (6e-4) while still catching the interior error M8 removed (T0 read 0.0756).

**Applies to / consequences:** o6e, o6c's re-oracle, Phase 8.3/8.5 rows, and any later reference-gated row. It is
the one exception to `2026-09-18-no-8bit-tolerances.md`'s term-count-only rule. Both terms are re-derived per rig
from the calibration and the reference run, never from the node's new output. If a row exceeds it, reduce the
reference's SE (more samples, fixed seed) — never widen the bound — and if it still fails, it goes to the user.
