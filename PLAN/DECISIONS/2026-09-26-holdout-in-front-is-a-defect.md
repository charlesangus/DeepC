# A holdout in front of a deep stack must match DeepHoldout2 — scheduled as M10

**Decision (user ruling, 2026-09-26, answers the 2026-09-11 open question from M5.P3.T1):** a 0.5-alpha holdout in
front of a two-layer deep stack rendering alpha 1.0 / R/A 0.50 (where `DeepHoldout2` gives 0.5 / 0.80) is a
defect, not intended behaviour. It is scheduled as a new milestone, M10 (stub, blocked on M8 shipping because M8
rewrites the composite the holdout goes through), executed right after M8 and before M2.

**Rationale:** depth-correct holdouts are the node's headline differentiator; a holdout that stops holding out
when it sits in front is exactly the failure the node exists to avoid.
