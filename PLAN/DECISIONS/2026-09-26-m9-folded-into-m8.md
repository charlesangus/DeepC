# Nested footprints are fixed inside M8, not a separate M9

**Decision (user ruling, 2026-09-26, answers M8.P2.T7 ruling 1):** option B. Rather than ship M8 with the o6
silhouette band as a bounded XFAIL for M9, M9's scope (nested footprints — correlated occlusion in the coverage
partition) is folded into M8 as Phase 8.5, ahead of M8's gate. M9 is cancelled (row and file kept for history).
The volumetric-rim over-read (M8.P2.T7 ruling 2) is deferred into the same phase: the user rejected the per-parent
chain (M8.P2.T10, cancelled) because a deep image does not record that two samples are "one fog body", and expects
the general nesting fix may address the rim; whatever it reads afterwards goes back to the user.

**Rationale:** removing the buckets made the nesting term's worst case larger (max |ΔG/A| 0.1148 vs 0.0934); the
user would rather M8 merge without that regression than carry it on master until a later milestone.

**Consequences:** M8 grows by a design-first phase (L); the execution order is now M8 → M10 → M2 → M3; M2/M3's
freshness checks read the post-Phase-8.5 composite.
