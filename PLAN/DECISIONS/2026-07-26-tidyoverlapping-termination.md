# tidyOverlapping non-termination — fixed in shared code, affecting shipped nodes

`deepc::tidyOverlapping()` in `src/DeepSampleOptimizer.h` never terminated when a deep pixel
contained overlapping volumetric samples. The split loop always split `samples[i]` at
`samples[i+1].zFront`; when the two shared a `zFront`, that split point *is* `samples[i]`'s own
front, so the split produced a zero-extent piece plus an unchanged copy of the original and the
scan restarted on the same overlap forever, growing the vector without bound. The loop created
that configuration itself: splitting `[1,5]` at 3 yields `[3,5]`, which then shares a front with
`[3,9]`. Two *exactly identical* spans hit it too, since the split was attempted before the
over-merge pass could collapse them.

This was recorded as a project-wide decision rather than a milestone-scoped one because the blast
radius reaches outside `DeepCDefocus`: `optimizeSamples()` calls `tidyOverlapping()`
(`DeepSampleOptimizer.h:235`), and both shipped nodes `DeepCBlur` (`DeepCBlur.cpp:303`) and
`DeepCBlur2` (`DeepCBlur2.cpp:449`) call `optimizeSamples()`. Verified end to end in headless Nuke
17.0v3 against a pristine build of the pre-fix tree: **both nodes hang (unkillable, unbounded
memory growth) on volumetric deep input**, and not only on contrived overlaps — a scene of
*disjoint* volumetric layers also hangs, because the blur's neighbour-gather feeds several
identical copies of one z-range into a single output pixel's sample list before tidying. Gathering
the same depth range from more than one neighbouring pixel is the normal case, so in practice any
`DeepCBlur`/`DeepCBlur2` use with blur > 0 over non-point deep input was at risk. The bug dates
from `2b77fab`, which introduced the tidy pre-pass.

**Fix:** choose which of the overlapping pair to split so the split point is always strictly
interior to the span being split — fronts differ → split the earlier at the later's front (the
original intent); fronts equal → split the longer at the shorter's back, or skip when they are
perfectly coincident and let the existing over-merge pass handle them. A guard enforces the
strictness the termination argument depends on. Every split point is an endpoint that already
existed in the list, so the set of distinct endpoints never grows and termination is provable.

**Evidence it is a no-op on previously-working input:** a differential harness over 65,043 cases
(structured coverage of point/disjoint/touching/partial/nested/coincident/mixed configurations,
plus 60,000 randomised multi-sample pixels with tie-heavy discrete depths, fed unsorted, each also
run through `optimizeSamples` under five parameter sets) found the old version hung on 41,068 of
them — 100% of every volumetric-volumetric overlap category — and among the 23,975 it did
terminate on, **0 differences, bit-exact on every float field**. In Nuke, a point-sample scene
through both shipped nodes is pixel-identical between the pre- and post-fix builds (max abs diff
0.0 on every channel).

**Follow-up, out of scope here:** with the fix in place, `DeepCBlur2` renders volumetric input with
alpha ≈ 1.066, i.e. above 1.0. That is in `DeepCBlur2`'s own alpha-correction path, not in the
optimizer — it was simply unobservable before, because the pre-fix build could not render
volumetric input at all. It deserves a separate look; a regression baseline cannot be taken from
the pre-fix tree.
