# Volumetric tidying follows the OpenEXR mixture model — with visible impact on shipped nodes

Adjudicated at M1.P3.T0, after M1.P2.T2 measured `DeepCDefocus`'s size-0 flatten disagreeing with
stock `DeepToImage` by ~8.9e-03 (partially overlapping volumetric spans) and ~9.5e-02 (perfectly
coincident ones) — far too large to be rounding, so two different algorithms, one of them more
nearly right.

**The governing model**, derived from the radiative transfer equation for a homogeneous
emitting/absorbing slab and not from either implementation: a volumetric sample is a medium of
optical depth `u = −ln(1−α)`. Two samples on the same interval are two independent media
co-occupying that volume — neither is in front of the other — so densities add:

```
u = Σ u_s,   u_s = −ln(1−α_s)
α = 1 − e^−u          (identical to 1 − Π(1−α_s): alpha matches `over`; only colour differs)
C = (Σ C_s·u_s/α_s) · α/u
```

This is order-independent, which `over` is not. Both the implementer and the reviewer confirmed it
against **independently written numerical ray marches** of the raw media (40M steps; agreement to
1.7e-10 and to ≤4.9e-07 respectively) — so the rule is validated against direct integration, not
merely against Nuke.

**Findings.** `tidyOverlapping()`'s *split* was already spec-correct in form. Its *merge* was plain
`over`, wrong by 2.1e-03…2.3e-01 against ground truth and order-dependent (swapping two coincident
samples flipped the answer), with worst-case error approaching 0.499 per channel as α→1. Nuke's
`DeepToImage` with `volumetric_composition` **on** (its default) implements the mixture rule
exactly. A notable side finding: on the flatten path the old tidy pass was an *algebraic no-op* for
volumetric input — its split is exactly invertible under `over` and its merge was `over` — so it
only ever mattered by stopping the scatter path from adding coincident samples.

**Resolution:** fix `tidyOverlapping()`. The merge now branches — `zBack > zFront` uses the mixture
merge (double accumulation, `log1p`/`expm1`), `zBack == zFront` keeps the previous `over` loop
verbatim for point samples. Post-fix, `DeepCDefocus` size-0 vs `DeepToImage` (volumetric composition
on) is ≤2.4e-07 — ordinary re-association noise — so validation scene (a)'s volumetric clause
becomes a real parity gate rather than being scoped out. **That gate must pin
`volumetric_composition` ON**; with it off, Nuke selects the `over` form this change moved away
from and the node now deliberately disagrees at ~1e-02.

**A second, independent defect** was found in the same function's split pass during review and
fixed alongside: it zeroed a span's colour outright below `alpha > 1e-6f`, and its
`1 − pow(1−a, ratio)` form cancelled catastrophically against 1.0 — 7.3e-02 relative error at
α=1e-6 and 1.9e-01 at α=1e-7, against the identity a split must satisfy
(`1 − (1−a_front)(1−a_back) == a`). Reformulated in optical depth, which also removes the magic
epsilon; measured relative error now 1e-10…1e-7 across α from 1e-7 to 0.99.

**Impact on shipped `DeepCBlur` / `DeepCBlur2`** (the only other callers, via `optimizeSamples()`):

- The **merge** change is provably unreachable on anything a released build ever rendered — that
  input class hung unkillably before the termination fix, confirmed by building released `master`
  and watching it wedge. Verified bit-identical on point-only, disjoint-volumetric and mixed input
  (200k randomised trials each, by two independent harnesses; 0 differences).
- The **split** change *does* alter released behaviour, on point samples lying inside a volumetric
  span: **2.32e-02 absolute / 12.4% relative in alpha**. These are not exotic alphas — `DeepCBlur`
  multiplies every gathered sample's alpha by its kernel weight before tidying, so low alpha is the
  normal case. The user was asked and chose to land it now, with the change called out explicitly
  rather than slipped in, on the grounds that the branch is unreleased and this is the cheapest
  moment to correct it. **It needs a release note.**

**Follow-ups noted, not actioned here:** `DeepCBlur.cpp:275` and `DeepCBlur2.cpp:364` read
`Chan_DeepBack` without the `std::max(zf, zb)` clamp `DeepCDefocus.cpp` applies, so they can build
inverted spans (harmless today — inverted spans take the point path — but an unguarded assumption).
And the mixture merge costs a `log1p` per sample in a coincident group, which `DeepCBlur`'s
neighbourhood gather makes the *common* case for volumetric input, so it lands on the hot path when
M1.P3.T1 moves tidying there.
