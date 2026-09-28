# M9 revived as a research milestone; M8 held until it lands

**Decision (user ruling, 2026-09-27, answers the M8.P5.T1 no-go):** option (C). The nested-footprint fix leaves M8's Phase 8.5
and becomes M9 again, now scoped as research: a per-pixel lens-coverage representation plus a kernel that conserves area on
receding surfaces. Two further rulings bind it:
- **No per-sample depth gradient.** Estimating a sample's depth slope from neighbouring pixels' samples is not admissible.
  A fix may use only what a sample carries on its own: depth range, alpha, position. This extends the 2026-09-26 "no body
  identity" constraint.
- **The n8 rows (n8c/n8cr/n8dr, order dependence at a defocused edge) must be fixed**, not XFAILed.

**M8 is held (board `blocked`) until M9 lands.** Nothing merges to master before then. The target rows stay FAIL on HEAD
meanwhile, and none is XFAILed as an interim: o6f, o6g/o6h, f4a/f4b/f4c (+r), f3e/f3f/f3h/f3h2/f3i, n8c/n8cr/n8dr.
M9's code rides the same branch, `claude/deep-defocus-node-plan-o0ld83`. M8's P3.T2 and Phase 8.4 gate run after M9, and M8's
PR carries both milestones.

**Rationale:** P5.T1's candidate C missed every bound and regressed f4b/f3e/f3f. Even an ideal claimed-region oracle read 8×
the bound on the o6f band, because receding surfaces break area conservation in the kernel's weights. Plain M8 tuning cannot
close that gap. The user prefers holding the merge to shipping XFAILs or a partial fix.
