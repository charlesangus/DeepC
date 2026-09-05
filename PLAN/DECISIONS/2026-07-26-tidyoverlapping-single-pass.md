# tidyOverlapping split pass rewritten single-pass — with a documented ordering caveat

`tidyOverlapping()`'s split pass resolved one overlapping pair per iteration, then restarted the
whole scan including a re-sort. Measured at M1.P3.T1, once the function reached `DeepCDefocus`'s hot
path: ≈O(n³·⁷), **9.96 ms per pixel at 32 mutually overlapping spans** — days per fog frame, so
validation scenes (f) and (g) were unrunnable and the node unshippable regardless.

**Replaced with a single front-to-back sweep.** A group is every record sharing the current smallest
unemitted front `f`, resolved in at most two rounds: cut everything reaching past `bmin` (the nearest
back in the group) at `bmin`; then, if `bmin` still reaches past the next front, cut there too. Far
pieces wait in a min-heap over an append-only pool. Round 1 must precede round 2 — not for
correctness but to reproduce the old code's exact float rounding chain, since the old scan always
resolved the *leftmost* conflicting adjacent pair and a shared-front conflict sits one index earlier
than a crossing-front one. An ascending variant gives identical geometry but diverges from the old
output in 36,752/60,000 cases against 3,402 for the implemented order.

Termination is now structural: every cut point is an endpoint that already existed, each iteration
emits its whole group, every queued piece starts strictly beyond `f`, so `f` strictly increases and
the sweep runs at most once per distinct endpoint. Nothing restarts and nothing is re-sorted inside
the split pass. M1.P3.T0's `log1p`/`expm1` split arithmetic and the volumetric mixture merge are
carried over verbatim (verified: a normalised diff of the split arithmetic against the previous
version is a single line, and the transmittance identity holds to 2.3e-09…5.4e-08 relative across α
from 1e-7 to 0.99).

**Measured speedup:** n=8 3×, n=16 12×, n=32 **35–41×** (9.96ms → 0.28ms), n=128 583×
(4581ms → 7.9ms). On a 30-scene render set, 424 s → 17.1 s; worst single scene 59 s → 2.2 s.

## The ordering caveat, and what it means for shipped nodes

Geometry, sample counts and every volumetric/mixture-merge record are **bit-identical** to the
previous implementation across ~1.29M randomised cases. **Point-only input — the only class a
released `DeepCBlur`/`DeepCBlur2` build could actually render, since anything volumetric hung before
the termination fix — is bit-identical** over 292k independent cases and 65 rendered scenes. That is
the safety argument, and it holds.

What differs is the `over` composite *order* of coincident point samples and inverted spans. The
cause is that libstdc++'s `std::sort` is a stable insertion sort at ≤16 elements and an unstable
introsort above, and the old code's second sort saw an array order produced by its own
pair-at-a-time splitting that no different algorithm can reproduce. Two corrections to how this was
first characterised, both from the review:

- **The implementer's proof does not hold as stated.** Substituting `std::stable_sort` into the *old*
  implementation does NOT give 0 differences — it gives 48%, worse than the unmodified old code.
  The experiment that actually isolates the cause is making *both* implementations stable: 1 case in
  20,029, and that one is a ragged-channel-count artefact unreachable from any current caller.
- **The reachable class is wider, and the magnitude larger, than "degenerate inverted spans".** The
  "16 records" framing is wrong — what matters is the array size at the *second* sort, which
  splitting grows past 16 from inputs of any size. An ordinary comp (a volumetric layer plus surfaces
  at a shared depth) moves by up to **3.1e-03 through both shipped nodes**; offline, where coincident
  samples carry genuinely different colours, the difference reaches **0.89 absolute in a colour
  channel**. Alpha is essentially unaffected (≤1.2e-07), since `over`'s alpha is `1−Π(1−a)` in any
  order. **The release note must say this**, not the narrower version.

**Decisions taken:**
- **Keep the merge pass's second `std::sort`.** Dropping it is 4–12% faster but changes 41,281/91,032
  cases and breaks bit-exactness for the >16-point-sample regime, which is exactly the safety
  argument above.
- **Do not switch to `std::stable_sort` now.** It would make coincident-point output reproducible
  across toolchains, but at the cost of Linux bit-exactness against the previous build — trading a
  known-good status quo for portability the codebase has never had. Worth revisiting deliberately,
  with the note that this output *already* differs between the Linux and Windows builds today, so
  what is being preserved is one libstdc++ permutation, not determinism.
- **Do not record the new Θ(n²) as a lower bound.** The intermediate is Θ(n²) for mutually
  overlapping spans while the output is O(n), which is the price of the split-then-merge formulation
  that buys bit-exactness — but a boundary-event sweep accumulating optical depth per elementary
  interval would be O(n log n + K). Relevant if fog performance resurfaces.

**Follow-up, unreachable today:** the merge pass takes `nChan` from a group's first member, so a
group with ragged channel counts depends on group order. All three callers set a uniform channel
count (`DeepCBlur.cpp:279`, `DeepCBlur2.cpp:367,429`, `DeepCDefocus.cpp:889`).
