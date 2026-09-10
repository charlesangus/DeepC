# Milestone 4: Coverage renormalization — focal-line bands and FG-silhouette halos

Fixes the two measured coverage artifacts in the shipped `DeepCDefocus`: bands of reduced
opacity either side of the focal line when a plane is defocused at 45° to camera, and
reduced-opacity halos around objects in front of the focal plane. Bokeh/pgBokeh show neither.
This is a **deliberate, total contract change** to M1's shipped "honest dip" behaviour — there
is no legacy knob and no opt-out.

## Design reference (read before any task in this milestone)

The executable design is [REFERENCE/COVERAGE-FILL-PLAN-v2.md](../REFERENCE/COVERAGE-FILL-PLAN-v2.md)
— read it before any task here. Its companions are the independent review that shaped it
([COVERAGE-FILL-RECOMMENDATIONS.md](../REFERENCE/COVERAGE-FILL-RECOMMENDATIONS.md)), the cost
notes ([COVERAGE-FILL-PERFORMANCE.md](../REFERENCE/COVERAGE-FILL-PERFORMANCE.md)), and the
superseded v1 ([COVERAGE-FILL-PLAN.md](../REFERENCE/COVERAGE-FILL-PLAN.md), kept only so review
items can be traced). Line-level derivation lives in the planning-pass transcript at
`~/.claude/plans/there-s-a-fundamental-issue-squishy-tulip-agent-a9a1aa83e0f5f5bca.md`.
Untracked copies of all four also sit in the code repo root; those are scratch — the
`REFERENCE/` copies are canonical.

**Why the artifacts exist.** `DeepCDefocus` is a *scatter* algorithm. Each fragment's disc
kernel sums to 1, but the weight *arriving* at an output pixel does not sum to 1 when CoC
varies spatially — the sharp-path delta at r<0.5px vs. the r=0.5 disc, kernel-grid snapping,
and the true-gradient term (adjacent scanlines genuinely 0.5px apart in radius). Scene (g)
measures up to **7.4e-2 deficit near focus, K-invariant**; scene (l) l5 is the 45° case and is
worse. Deficits are only ever clamped DOWN (`saturateBucketPlanes`), never up. The same
mechanism also produces **over-reads of up to ~5.9%** near focus (g5's surplus cells) — which
deficit-only division cannot correct, and which is why kernel blending (piece 4) is in this
milestone rather than deferred. The halos are the "COVERAGE DEFICIT (specified behaviour)"
block's own subject: defocused FG scatters off its silhouette, and the vacated pixels have no
occluded-BG samples to fall back on, leaving an alpha dip ~one CoC wide. 2D nodes don't show
any of this because a flat image has a source pixel under every output pixel — their scatter
weight sums are ~1 everywhere, an implicit renormalization.

**User decisions, already taken (do not re-litigate):** fill like pgBokeh — renormalize so
opacity holds up, even where the renderer wrote no occluded samples — and make it the **only**
behaviour, with no legacy knob. The "honest dip" contract is retired; docs, validation scene
(i), and the XFAIL pins are re-specified accordingly in Phase 4.3.

### The four pieces (all always-on)

1. **Gather-share partition (flatten side).** In `flattenPixelToSoA()`'s front-to-back staging
   loop, partition each source pixel's unit area over its fragments: `share = t * alpha;
   t *= (1 - alpha)`. The residual `T_px = t` is the virtual background's claim, so shares +
   residual sum to exactly 1 by construction. Volumetric split parts each take `t * part.alpha`;
   a pre-merge sums its members' shares; **collision attenuation must NOT touch shares.**
   Also carry out `residualRadiusPx` — the scatter radius of the pixel's *deepest* sample (the
   last staged front-to-back, after any split/merge). A pixel with no samples returns the empty
   defaults (`T = 1`, radius = global background radius).
2. **Arrival plane + virtual background (scatter side).** `BucketPlanes` gains a fifth,
   **K-independent** buffer `arrival`. Deposits use the **raw** kernel row — *before* the
   holdout-visibility fold — so held-out alpha is never renormalized back up. **One arrival
   plane; do NOT add per-bucket arrival planes** (the coverage plane also carries holdout
   visibility and follows an area model, so a bucket can legitimately hold more than unit
   weight; per-bucket raw arrival would fix only the narrow same-surface surplus case at K×
   the memory cost — surpluses stay with the existing excess/attenuation machinery).
   The **virtual background** covers the **full fetch window**, not just `srcBox`: residual `T`
   defaults to 1.0 for every fetch-window pixel inside the output box regardless of `srcBox`,
   or a deep input with a tight bbox would turn every bloom pixel outside that box into a hard
   disc at alpha 1. Each pixel's residual scatters at **its own deepest sample's CoC**; only
   pixels with no samples at all use the global `background_depth` radius.
3. **Deficit-only division (composite side).** Immediately *before* the existing down-only
   clamp: if `D > kFillMinArrival (~1e-3)` **and** `D < 1 - kFillDeficitTol (1e-5)`, scale
   `accAlpha` and every output colour channel by `1/D` — the premultiplied pair moves together.
   The `1 - 1e-5` tolerance is **not optional**: at exact size-0 the share sums can land one ulp
   below 1, and dividing there would move pixels that must be untouched and break the bit-exact
   size-0 parity gate (a).
4. **Kernel blending for fractional diameters.** Replace the hard sharp/disc switch with a
   **minimum kernel diameter of 1 px** and, for fractional diameter `d`, a blend of the two
   bracketing odd-sized kernels at `f = (d - dA) / (dB - dA)`. The 1×1 kernel is the sharp
   path's delta, so `d ≤ 1` reproduces today's fast path exactly. Both kernels feed colour
   **and** arrival with the same `f`. This is the only route to correct alpha on
   semi-transparent surfaces near focus.

### Properties this must preserve (each pinned by a test or scene)

- Empty stays exactly black — numerator 0; scene (d).
- **Soft blooms over emptiness are unchanged *conditionally*, not as an invariant.** Arrival
  equals 1 around an isolated object only when that object's CoC equals the radius the
  surrounding empty pixels scatter their virtual background at. That holds for a single-depth
  scene under the auto default. In a scene with far geometry elsewhere, a near object's fringe
  over emptiness **can** be boosted where its kernel is smaller than the background kernel. The
  per-pixel residual radius does not remove this — truly empty pixels have no sample to borrow a
  depth from. Pinned by scene (e)/m2 under the single-depth condition; the multi-depth case is
  **documented, not pinned**.
- Fog identities unchanged where fog covers the frame (shares + residual make `D == 1` exactly;
  two 0.5 layers still read 0.75).
- Holdout regions never boosted, and more strongly: **fill and holdout commute** — the numerator
  scales linearly with visibility per fragment while the divisor ignores visibility, so a holdout
  applied before the fill gives the same ratio as one applied after. Pinned by m4.

### Execution constraints specific to this milestone

- **The validation suite will be red mid-milestone, by design.** The behaviour change and the
  re-spec/re-pin (Phase 4.3) cannot land in the same commit under per-task commits. Scene (i),
  g4, g5 and the honest-alpha doc blocks stay stale until P3.T3. **Do not "fix" a red scene
  before P3.T3** — its redness is the expected signal. The gate is the PR, not each task.
- **Preserve the T0 baseline `.so`.** Phase 4.3's visual sign-off and m2's bloom-profile pin are
  before/after comparisons against it; it cannot be reconstructed once the branch moves.
- **M1's standing lesson applies in full** (recorded eight times over): every wrong figure in the
  M1 plan was a measurement that never reached the phenomenon it claimed to bound. So —
  mutation-test every new check (one nobody has made fail proves nothing), **band** pins rather
  than bounding them on one side, validate every re-pin against an **independent oracle** rather
  than against the new output, and give every XFAIL a **hard outer bound** so it cannot swallow a
  later regression.
- **Colour:alpha ratio is a standing invariant.** The clamp-one-of-a-premultiplied-pair defect
  has appeared three times in this codebase. Every new alpha assertion gets a colour-ratio
  assertion beside it.
- Work continues on `claude/deep-defocus-node-plan-o0ld83` per the board's branch constraint.

> **Anchors below were verified against the working tree on 2026-09-10.** Where the design doc's
> cited line differs from the real one, the real one is used here and the drift noted.

## Phase 4.1: Fill mechanics

- [x] M4.P1.T1 — Baseline capture, and commit the pending harness changes
  - files: `tests/nuke/run_validation.sh` (currently modified, uncommitted), `scripts/hostguard.sh`
    and `scripts/hostguard-hook.py` (currently untracked)
  - approach: commit the pending harness work **first**, so the baseline is reproducible: the
    `run_validation.sh` diff adds a `--threads N` wrapper option (mapping to Nuke's `-m`) to cap
    core use on this 4-core host and re-points the "plugin not found" hint at
    `scripts/hostguard.sh`; the two `scripts/hostguard.sh*` files are untracked but every task's
    `verify` below invokes them. Then build:
    `cmake -S . -B build/local-17.0 -D Nuke_ROOT=/usr/local/Nuke17.0v3 -D DEEPC_BUILD_TESTS=ON`
    and `scripts/hostguard.sh -- cmake --build build/local-17.0 -j`. Run both doctest suites, then
    `DEEPC_PLUGIN_DIR=$PWD/build/local-17.0/src scripts/hostguard.sh -- tests/nuke/run_validation.sh`.
    **Use `build/local-17.0` explicitly and never `build/local-17.0-O3`** — the stale-mtime trap.
    Copy the built `DeepCDefocus.so` aside to a path **outside the repo tree** and record that path
    in this milestone's `## Decisions`; it cannot be reconstructed once the branch moves, and both
    m2's bloom pin and the P3.T5 sign-off are before/after comparisons against it.
  - verify: both unit suites green; the harness reproduces M1's closing tally
    (PASS=86 FAIL=2 XFAIL=9 SKIP=1) or every delta is explained in writing; every scene cell number
    and the `run_profile.sh` output recorded in `## Decisions`; the preserved `.so` loads in
    headless Nuke from its recorded path.
  - size: M

- [x] M4.P1.T2 — Gather-share partition and residual radius in the flatten
  - files: `src/DeepCDefocusScatter.cpp` (`flattenPixelToSoA()` spans 574–1058; its front-to-back
    staging loop, "4. sample -> fragments (THE COMPOSITION CONTRACT)", is 648–797),
    `src/DeepCDefocusScatter.h` (`FlattenScratch::Staged`, `FragmentRecord`, `SampleSoA`,
    `appendFragment`), `tests/test_defocus_scatter.cpp`
  - approach: add `share` to `FlattenScratch::Staged` and `FragmentRecord`, and `arrivalShare` to
    `SampleSoA` (+ `appendFragment`). Compute it inside the existing staging loop as
    `share = t * alpha; t *= (1 - alpha)`, so shares + residual sum to exactly 1 by construction.
    Volumetric split parts each take `t * part.alpha`; a pre-merge sums its members' shares;
    **collision attenuation must NOT touch `share`** (the same-pixel bucket-collision path from
    M1.P3.T13/T15 — leave it alone, or the partition stops summing to 1). Add out-params
    `float* residualT` and `float* residualRadiusPx`; the latter is the scatter radius of the
    **deepest** sample, i.e. the last one staged front-to-back, after any split or merge. An empty
    pixel returns `T = 1` and leaves the radius at the caller's global default. The function
    already mutates `samples` in place and takes `scratch`/`out`/`stats`; keep the two new
    out-params explicit rather than folding them into `FlattenStats`, per the file's convention.
  - verify: new doctest cases — shares + residual sum to 1 within 1e-6, fuzzed over point,
    volumetric-split, pre-merged and same-pixel-collision stacks; a collision stack's shares are
    bit-identical to the same stack without the collision; `residualRadiusPx` equals the deepest
    staged sample's scatter radius for each of the three stack shapes; an empty pixel returns
    `T = 1`. **Mutation-test each**: flip the `t` update and confirm every assertion fails.
  - size: M

- [x] M4.P1.T3 — Fifth arrival plane and raw-weight deposits
  - files: `src/DeepCDefocusScatter.cpp` (`BucketPlanes` `allocate`:1344, `zero`:1363,
    `release`:1373, `sizeBytes`:1386, `view`:1392), `src/DeepCDefocusScatter.h` (the struct at
    1144–1148 holding `color`/`alpha`/`weight`/`colocated`; `bytesForBand`:1188 with its memory
    commentary at 1140–1142 and 1170–1187; `scatterFragmentSpans`:1810;
    `scatterFragmentSharp`:1896), `tests/test_defocus_scatter.cpp:3802`
  - approach: add `arrival` as a fifth buffer that is **K-independent** — one float per band
    pixel, not per bucket. In `scatterFragmentSpans()` deposit once per fragment (group 0 only)
    `arrival[dst+i] += w[i] * share`, using the **raw** row `const float* w = kv.rowWeights(row) +
    skip;` at :1853 and depositing **before** the holdout-visibility fold at :1866–1872 — the
    denominator must exclude holdout attenuation, or held-out alpha gets renormalized back up.
    `scatterFragmentSharp()` (:1896) deposits `arrival[dst] += share`. Update `bytesForBand` and
    the `K*W*B*(C+3)*4` commentary for the extra non-K plane (on holdout-connected bands the
    existing LUT is already K+1 floats per pixel, so this is a small fraction of current memory).
    The :3802 test is named "clears ALL FOUR planes" — rename and extend it to five.
  - verify: doctest — `arrival` is bit-identical with and without a holdout LUT connected;
    `BucketPlanes::zero()` clears the fifth buffer; `bytesForBand` includes it and matches an
    independently hand-computed byte count; a single fragment's arrival deposits sum to its
    `share` within 1e-6.
  - size: M

- [x] M4.P1.T4 — Residual maps over the full fetch window, and the `background_depth` knob
  - files: `src/DeepCDefocus.cpp` (`computeBand()`:1685, its fetch loop 1706–1725;
    `frameSetup()`:1215 with the measured-radius block at 1328–1340 and `rMin = 0` at ~1353),
    `src/DeepCDefocusScatter.h` (`ScatterParams`), `tests/test_defocus_scatter.cpp`
  - approach: build a residual-T map **and** a residual-radius map sized to the **full fetch
    window**. Today the loop clips to `job.srcBox` (`fy0 = max(job.srcBox.y(), y0 - job.padY)`,
    `fy1 = min(job.srcBox.t(), y1 + job.padY)`, and X taken straight from `job.srcBox.x()/.r()`).
    The maps must instead default `T = 1.0` for every fetch-window pixel inside the **output box**,
    whether or not it lies in `srcBox`. Without that, a deep input with a bbox tight around its
    content gives every bloom pixel outside the box no virtual background: arrival equals the
    bloom's own weight, alpha divides to exactly 1, and a soft edge becomes a hard disc. Pixels
    with samples take `T_px` and `residualRadiusPx` from P1.T2. Add knob `background_depth`,
    default 0 = auto = the CoC at `buckets.depthMax()`, a manual value clamped to `rMax`; compute
    it in `frameSetup()` beside the existing measured-radius block and pass it through as an
    explicit parameter, the file's convention, rather than as a member read.
  - verify: doctest on the map builder — every fetch-window pixel inside the output box but
    outside `srcBox` has `T = 1`; pixels with samples carry the flatten's values; `background_depth
    = 0` resolves to the CoC at `depthMax()` and a manual value above `rMax` clamps to it. The knob
    appears in the node's properties under headless-Nuke script inspection.
  - size: M

- [x] M4.P1.T5 — `scatterBackgroundCPU()`
  - files: `src/DeepCDefocusScatter.cpp`, `src/DeepCDefocusScatter.h`,
    `tests/test_defocus_scatter.cpp`
  - approach: for each source pixel with `T > 1e-4`, deposit `w * T` into `arrival` using a kernel
    at **that pixel's own** residual radius — its deepest sample's CoC where it has samples, and
    the global `background_depth` radius **only** where it has none. A mismatched radius is
    visible: a true α=0.9 surface reads ~0.893 instead of 0.900, about 1.8 code values. The
    virtual background carries zero alpha and zero colour; it enters the denominator only. Use
    `DiscKernelLUT::radiusToIndex()` (`src/DeepCDefocusKernel.h:634`) for the per-pixel lookup.
    This is deliberately naive — π·r² per non-opaque source pixel. **Do not optimize here**;
    P3.T5 measures it, and the mitigation (bucket residuals by kernel bin, convolve per bin) is a
    new task only if the profile demands it.
  - verify: doctest — background deposits sum to `T` per source pixel within 1e-6; a pixel with
    samples uses its deepest sample's radius and one without uses the global radius (assert the
    **kernel index chosen**, not merely the sum); an α=0.9 flat field with a forced 0.93 arrival
    reads 0.900 to 1e-6, and reads ~0.893 when the radius is forced to a mismatched global value —
    that second assertion is the mutation test, and must fail if the per-pixel radius is dropped.
  - size: M

- [x] M4.P1.T6 — Deficit-only division in the composite
  - files: `src/DeepCDefocusScatter.cpp` (`resolveBandCPU()`:1605),
    `src/DeepCDefocusScatter.h` (`compositePixelCoveragePartition()`:2512; the down-only clamp
    block at 2947–2954), `tests/test_defocus_scatter.cpp`
  - approach: thread `arrival` through `resolveBandCPU()` into `compositePixelCoveragePartition()`.
    Immediately **before** the down-only clamp, with `D = arrival[pixel]`: if
    `D > kFillMinArrival` (~1e-3) **and** `D < 1.0f - kFillDeficitTol` (1e-5f), set `s = 1/D` and
    scale `accAlpha` **and every `outColor` channel together** — the premultiplied pair moves as
    one, the defect that has already appeared three times in this codebase. The existing clamp
    then absorbs any overshoot unchanged. The `1 - 1e-5` tolerance is load-bearing, not a nicety:
    at exact size-0 the floating-point share sums can land one ulp below 1, and dividing there
    moves pixels that must be untouched and breaks gate (a)'s bit-exact parity.
  - verify: doctest — flat opaque `D = 0.93` → alpha exactly 1 **and** unpremult colour ratio
    preserved; `D = 1.3` untouched; `D = 0` with a zero numerator → 0; the `kFillMinArrival`
    boundary from both sides; **a sharp-path-only pixel with `D = 1 - 1 ulp` is left bit-identical**
    while `D = 1 - 2e-5` does divide; the colour:alpha pair stays locked through the `accAlpha > 1`
    clamp; two 0.5 fog layers still read 0.75.
  - size: M

- [x] M4.P1.T7 — Pool a volumetric parent's arrival claim onto one radius
  - **Approach as originally written ("split the residual positively across each part's own
    radius") was REFUTED during execution and is superseded by the text below.** For an isolated
    source pixel in a uniform `T` field the surrounding kernels tile to exactly 1, so
    `arrival(x) = 1 - K_R(x-p) + own_p(x)`. Exactness demands `own_p == K_R`; both carry unit mass
    and `own_p >= 0`, so the pixel's whole claim must be a **single unit kernel at the residual
    radius**. Non-negative portions at the parts' radii cannot reach it — at 15 parts the claim at
    the deepest radius caps at 0.261 against the 1.0 required — and measured on the real pipeline
    that direction makes the artifact **worse** (isolated pixel +27.03% -> +34.13%, 40x40 patch
    +10.46% -> +11.99%). Expressing it residual-side would need **signed** per-part portions, which
    also breaks `scatterBackgroundCPU()`'s `T > kFillDeficitTol` skip.
  - what shipped: the inverse. A parent's split parts contribute their whole arrival share at the
    **deepest part's radius** (the others go to zero), pooled per-parent in `flattenPixelToSoA()`'s
    volumetric branch. The bucket split is an artefact of K and must not move where a parent claims
    arrival. `ResidualWindow` and `scatterBackgroundCPU()` were left untouched, which is why this
    needed neither of the two carrying designs the brief offered. `share` feeds only the two
    arrival deposits, so this moves the denominator and never the numerator; the partition
    invariant holds because only the distribution *within* one parent changes.
  - outcome: both deliberately-red cases from `f2e40d4` green on their original pins; 82/82 cases,
    238766 assertions; point sample bit-identical (it cannot reach the pooling block). Harness
    `PASS=82 FAIL=7` -> `PASS=83 FAIL=6`. **c4 returned to its exact pre-M4 reading (1.397e-05
    against a 1e-4 gate)** — pooling collapses a K-part parent's K arrival deposits into one.
  - size: L

## Phase 4.2: Kernel blending

> Deficit-only division cannot correct an **over-read**, and the kernel step produces up to ~5.9%
> of it near focus (g5's surplus cells). For opaque geometry the alpha clamp hides it; for
> semi-transparent surfaces it is visible in alpha, and blending is the only route to correct it.
> This phase is in the milestone, not deferred — see the milestone-shape decision below.

- [ ] M4.P2.T1 — Decide the bracketing-kernel scheme from rendered pixels
  - files: `src/DeepCDefocusKernel.h` (`kernelGridIndex()`:287 — a free function, not a member;
    `DiscKernelLUT::radiusToIndex()`:634) — prototype only, plus a throwaway render script
  - approach: the design says "odd integer diameters" are the bracketing set, but the grid today
    is **not** that: index 0 is a radius-0 delta entry, indices 1–993 are hyperbolic
    (`512/(1025-index)`, covering 0.5–16px), and indices >993 are uniform 0.5px steps. The LUT is
    built with `rMin = 0` and `radiusToIndex()` clamps anything below `entryRadius(0)` **up** to
    index 0. Reconcile by **prototyping both candidates behind a temporary compile-time switch and
    deciding from pixels, not from the derivation**: (A) re-cut the grid to the odd-integer-diameter
    set; (B) keep the existing grid and blend between adjacent existing nodes. Render the
    near-focus ramp at scene-g slope, at α=1 and α=0.9, under each. Either way the spec is
    "continuous in `d`, exact at the nodes, minimum diameter 1px", with index 0's delta serving as
    the 1×1 kernel.
  - verify: both variants render; the choice is recorded in this milestone's `## Decisions` with
    the rendered evidence (max |a−0.9| across the ramp for each, plus the visual call); the losing
    prototype and the compile-time switch are both removed — no production code keeps the switch.
  - size: L

- [ ] M4.P2.T2 — Minimum 1px diameter and the bracketing-kernel blend
  - files: `src/DeepCDefocusKernel.h`, `src/DeepCDefocusScatter.h` (`kSharpRadiusPx`:503 = `0.5f`
    and the rMin contract comment at 499–502; `scatterFragmentSpans`:1810;
    `scatterFragmentSharp`:1896), `tests/test_defocus_scatter.cpp`
  - approach: implement P2.T1's chosen scheme. Replace the hard switch
    (`!(radius >= kSharpRadiusPx)` → sharp path, else nearest grid node) with a **minimum kernel
    diameter of 1px** and, for fractional `d`, a blend of the two bracketing kernels at
    `f = (d - dA) / (dB - dA)`. Both kernels deposit into colour **and** into `arrival` with the
    same `f` — a blended fragment contributes `share * ((1-f)·wA_raw + f·wB_raw)`. Move
    `kSharpRadiusPx` and the rMin contract comment in lockstep: that comment exists precisely
    because "the flatten, the scatter and the LUT's rMin contract all read the same number".
  - verify: doctest — kernel weights are continuous in `d` (no jump above 1e-6 across a fine
    sweep) and exact at the nodes; **size-0 (all `d ≤ 1`) rasterises bit-identically to the old
    sharp path**, which gate (a) depends on; an α=0.9 surface at fractional diameters reads 0.900
    within 1/255 across the whole near-focus ramp — the case deficit-only division cannot fix.
  - size: L

- [ ] M4.P2.T3 — Tighten the identical-rasterisation predicate
  - files: `src/DeepCDefocusScatter.h` (`scatterKernelBin()`:546, `sameScatterKernel()`:555, and
    the g4/g5 commentary block at ~2320–2410), `tests/test_defocus_scatter.cpp`,
    `tests/nuke/scenes.py` (i7 at :2385)
  - approach: two radii now rasterise identically only if they share **both** bracketing kernels
    **and** the same `f`; make the predicate strict — same pair, `|Δf|` below an epsilon. This is
    the conservative direction the comment block there already demands. **It must be tightened,
    never loosened**, or the flatten's absorb of same-kernel fragments becomes lossy.
    `pre_merge`/`merge_tolerance` are unchanged in semantics (lossy by design at 0.25px), but i7's
    pinned number may move — re-measure it (its gate is `atDefault.maxAbs > 1.0e-02`, with the i7b
    control at :2397–2401).
  - verify: fuzzed doctest — `sameScatterKernel` is **never** true for two radii that rasterise
    differently (compare the full rasterised kernels, not the bins); the flatten absorb over a
    fuzzed fragment set is lossless within 1e-6; i7 re-measured and re-pinned with a hard outer
    bound.
  - size: M

- [ ] M4.P2.T4 — Re-confirm the share-side arrival deficit (c4 went green at M4.P1.T7)
  - files: investigation first — `src/DeepCDefocusScatter.h`
    (`scatterFragmentSpans()`/`scatterFragmentSharp()`'s arrival deposit,
    `compositePixelCoveragePartition()`), `tests/test_defocus_scatter.cpp`
  - approach: validation c4 regressed at M4.P1.T6 (1.397e-05 -> 5.261e-04 against a 1e-4 gate,
    ~0.13 code values) and it is **not** the residual-radius defect T7 fixes. The α=1.0 control
    settles that: where `T = 0` and `scatterBackgroundCPU()` deposits nothing at all, a
    fully-covered opaque uniform field still reads colour **+3.548e-4 above its input**, because
    arrival reads 0.99965 rather than 1. So it is a **share-side** deficit in the arrival
    accumulator. Known shape: uniform across the interior (spread 0.00e+00), erratic in sign across
    α (-5.4e-6 / +5.85e-4 / +1.11e-4 / -1.05e-5 at α = 0.99/0.90/0.50/0.20) and across K (+8.6e-5 /
    -2.1e-5 / +5.85e-4 at K = 4/8/16), vanishing at small CoC (size 4 -> 1.6e-7; span 20-30 ->
    9.3e-8) and appearing at large CoC. Sequenced **after** Phase 4.2 by the user's ruling: kernel
    blending rewrites how weights reach arrival and may move or explain this, and measuring it
    first risks measuring it twice. Find the root cause before proposing a fix; if the cause turns
    out to be float accumulation order in a single K-independent plane summing ~K× more terms than
    any per-bucket plane, say so explicitly — that is a different fix from a weight bug.
  - verify: the root cause is named and demonstrated by a test that fails for that reason and no
    other; c4 returns inside its 1e-4 gate, or the gate is re-specified against an **independent
    oracle** with a hard outer bound and the reason recorded. Mutation-test whatever pins it.
  - size: L

## Phase 4.3: Scenes, re-spec, sign-off

- [ ] M4.P3.T1 — Validation scene (m), cells m0–m3
  - files: `tests/nuke/scenes.py` (new scene builder; the `SCENES` dict at :2965 currently holds
    keys a–l only), `tests/nuke/generate_scene_scripts.py`,
    `tests/nuke/scene_m_coverage_halo.nk` (new — the convention is
    `scene_<letter>_<short_description>.nk`)
  - approach: **m0** occluded-samples-present control — interior alpha 1 within 1/255 **and**
    interior unpremult colour ratio within 1/255 of the source (near silhouettes, occluded samples
    can be scattered into the numerator but excluded from the denominator, alpha overshoots 1, and
    the clamp scales colour with alpha — both need checking). **m1** the halo — BG with a hole
    under the FG silhouette: dip depth ≤ 1/255, and the filled band's unpremult colour is **FG**
    colour (the fill must not invent BG colour). **m2** FG over nothing — bloom profile shipped as
    pinned numbers, cross-checked against P1.T1's preserved baseline `.so`; a single-depth scene,
    so the conditional bloom property applies. **m3** the reported artifact — 45° ramp at scene-g
    slope at α=1 and α=0.9, interior `|a−1| ≤ 1/255` and `|a−0.9| ≤ 1/255`, **including the
    near-focus rows scene (g) excludes**.
  - verify: all four cells run under `run_validation.sh` and pass on the new build; **each is
    mutation-tested** (perturb the fill by a known amount and confirm the cell fails — a check
    nobody has made fail proves nothing); the committed `.nk` reproduces each cell exactly.
  - size: L

- [ ] M4.P3.T2 — Validation scene (m), cells m4 and m5
  - files: `tests/nuke/scenes.py`, `tests/nuke/scene_m_coverage_halo.nk`
  - approach: **m4 holdout commutation** — render the scene with and without a 0.5-alpha holdout
    card in front of everything; the ratio of the two renders under the card must hold to float
    precision at every pixel, **including the rows the fill changes**. This is the direct test that
    the numerator scales linearly with visibility per fragment while the divisor ignores it, and it
    subsumes the weaker "held-out alpha is never boosted" guard. **m5** — over-checkerboard EXR
    writes of m1 and m3 for eyeballing, consumed by P3.T5.
  - verify: m4 passes, and is mutation-tested by deliberately folding visibility into the arrival
    deposit — it must fail; m5 writes readable EXRs at the expected paths.
  - size: M

- [ ] M4.P3.T3 — Re-spec and re-pin the retired honest-dip contract
  - files: `tests/nuke/scenes.py` (scene (i); g4 at :1805 `G4_PIN, G4_BAND = 0.0325, 0.004`; the
    g5 cells at :1933–1941; l6 `hardTol=1.5e-02` at :2953; f2 `F2_HARD = 1.00` at :633 and f3
    `F3C_HARD = 2.0e-02`/`F3D_HARD = 5.0e-02` at :681–682 if they move),
    `tests/nuke/scene_g_banding.nk` (StickyNote2 numbers), `src/DeepCDefocus.cpp` (the "COVERAGE
    DEFICIT (specified behaviour)" node_help block at 125–149 and limitation #1 at 199–211),
    `src/DeepCDefocusScatter.h` (commentary at ~2320–2410 and the honest-alpha sentence at
    ~2928–2943)
  - approach: scene (i) is re-specified — the deficit now fills, and **that IS the feature**.
    **CORRECTED 2026-09-10, after M4.P1.T6 measured it: g4 does NOT collapse under the fill, and
    neither does g5.** g4's residual is a bucket-composite artifact — `scenes.py`'s own commentary
    calls it "one bucket pooling a head and a rear at unequal per-unit opacity" — so `arrival` is
    already ~1 there and the deficit-only fill correctly never engages; it is not a coverage
    shortfall and this milestone does not address it. g5 is the **over-read** arm, which
    deficit-only division never touches by construction. If g5's four cells (`+0.0593`, `+0.0340`,
    `+0.0653`, `+0.0706`) collapse at all it is **Phase 4.2's kernel blending** that does it, so
    re-measure them after P2.T2 and not before; if they survive, they stay pinned with a hard outer
    bound and g4 keeps its own pin unchanged. The cells that DID move under the fill are scene (i)
    (i2/i3), c4, f3g/f3h and l6 — those are the re-spec targets. Re-measure every number against an **independent oracle** — never
    against the new output. node_help, stated plainly: the fill divides by **un-held-out** arrival,
    so holdout attenuation is preserved exactly and fill and holdout commute; the fill invents
    **foreground-coloured** coverage only where the renderer wrote none, like a 2D defocus, and
    never invents background colour; `background_depth` sets the defocus of invented coverage for
    pixels the deep image left empty (auto = farthest measured depth), while coverage behind a
    pixel's own samples always takes that pixel's deepest sample's defocus. Delete the
    "alpha-renormalise … deliberately not in this version" text, and update the `scatterKernelBin`
    comment per P2.T3.
  - verify: every re-pinned number is justified in the commit message by the independent oracle
    that produced it; every surviving XFAIL carries a hard outer bound so it cannot swallow a later
    regression; `grep -rn "honest" src/ tests/` turns up only intentional, re-specified prose — no
    source or scene file still asserts the retired contract.
  - size: L

- [ ] M4.P3.T4 — Full validation suite
  - files: none — verification only; any fix lands in the task that owns the code
  - approach: run both doctest suites and the full a–l+m harness on a clean rebuild, with
    `DEEPC_PLUGIN_DIR` pinned to the fresh `build/local-17.0/src` and **never** the stale `-O3`
    tree. Everything through `scripts/hostguard.sh` on this 4-core/15GB host.
  - verify: must-hold gates all green — (a) size-0 parity bit-exact, (c) energy conservation
    alpha == 1, (d) empty exactly black, (e) sharp edge ≤1px + bloom extent + no BG bleed,
    (h) overlap normalization, (b)/(f) holdout parity, (m4) holdout commutation. Every XFAIL
    re-pinned with a hard outer bound; the PASS/FAIL/XFAIL/SKIP tally recorded and every delta
    from P1.T1's baseline explained in writing.
  - size: M

- [ ] M4.P3.T5 — Visual and performance sign-off
  - files: `tests/nuke/run_profile.sh` (run, not edited); scratch renders outside the repo tree
  - approach: render m5's checkerboard outputs from P1.T1's preserved baseline `.so` and from the
    new one, and compare them by eye — **this pixel judgment is what the whole milestone is for**,
    so put both in front of the user rather than deciding from the numbers. Then `run_profile.sh`
    before/after. Costs to watch, in expected order of size: the per-radius virtual-background
    scatter on sparse and semi-transparent frames (mitigation: bucket residuals by kernel bin and
    convolve per bin), the doubled raster work near integer kernel sizes from Phase 4.2, then the
    arrival deposit loop (expected to be lost in the noise — it costs less than the visibility
    interpolation already running in the same loop). **None of these gate correctness**; optimize
    only if the profile demands it, and if it does, that is a new task, not a widening of this one.
    Finally delete the untracked `COVERAGE-FILL-*.md` scratch copies from the repo root — the
    `PLAN/REFERENCE/` copies are canonical.
  - verify: before/after renders of m1 and m3 put in front of the user for the visual call; profile
    numbers recorded in `## Decisions` beside M1's 2K/20spp figures; any regression beyond ~2×
    flagged with a named mitigation rather than silently accepted.
  - size: M

**Verification gate:** both doctest suites green; the full a–l+m harness green under
`scripts/hostguard.sh` with `DEEPC_PLUGIN_DIR` pinned to a fresh `build/local-17.0/src`, with
must-hold gates (a) size-0 bit-exact parity, (c), (d), (e), (h), (b)/(f) holdout parity and (m4)
holdout commutation all passing, and every XFAIL carrying a hard outer bound; **m3 — the 45° ramp
at α=1 and α=0.9 including the near-focus rows — is the acceptance check for the reported
artifact**, and m1's dip ≤ 1/255 with FG-coloured fill the acceptance check for the halo; the user
has signed off on the before/after checkerboard renders; `./docker-build.sh --linux` green (the
release gate — local-build binaries need `GLIBC_2.29` and cannot load on RHEL 8); then PR to
`master` from `claude/deep-defocus-node-plan-o0ld83`.

## Decisions

- 2026-09-10 — **Kernel blending ships inside this milestone, not as a follow-up**: the v2 design
  allows splitting the fill (T0–T4) from the blending (T5), but the user chose one milestone. One
  contract change means scene (i)/g4/g5/m cells are re-pinned exactly **once** instead of twice,
  and semi-transparent alpha is correct the moment it lands. The cost is a larger PR.
- 2026-09-10 — **M4 runs before M2 (aberrations) and M3 (CUDA)**: this fixes a measured,
  user-reported defect in shipped behaviour, and Phase 4.2 rewrites the kernel step that M2's
  kernel-field node builds directly on top of. Doing M2 first would mean writing the aberration
  path against a kernel contract that M4 then changes.
- 2026-09-10 — **The bracketing-kernel scheme is decided from rendered pixels (P2.T1)**, by
  prototyping both candidates rather than deriving the answer, because the existing LUT grid is
  hyperbolic-below-16px with a radius-0 delta at index 0 and does not match the design's
  "odd integer diameters" phrasing.

### 2026-09-10 — M4.P1.T1 baseline (T0)

**Preserved artifacts** — `~/deepc-baselines/M4-T0/` (outside the repo tree, survives every
branch move): `DeepCDefocus.so`, `PROVENANCE.txt`, `validation-baseline.log` (the full a–l
harness output, every cell number), `profile-baseline.log`. Built from `bb07e57` on
`claude/deep-defocus-node-plan-o0ld83` against Nuke 17.0v3 out of `build/local-17.0`.
Verified loading in headless Nuke from that path (49 knobs). P3.T4 explains its deltas against
`validation-baseline.log`; P3.T5's sign-off and m2's bloom pin render from this `.so`.

**Unit suites:** `test_defocus_math` 27/27 cases, 140887 assertions; `test_defocus_scatter`
64/64 cases, 212725 assertions. Both green.

**Harness tally: `PASS=86 FAIL=2 XFAIL=9 SKIP=1`** — reproduces M1's closing tally exactly, no
deltas to explain. The non-PASS roster, which is what P3.T4 compares against:

| Cell | Reading | Gate | Status |
|------|---------|------|--------|
| e4 escape mid-cook cancel | — | — | SKIP |
| f2 opaque holdout erases unoccluded FG | 5 strips; 0.975 depth units (78% of bracket) | 0 strips (xfail < 100%) | XFAIL |
| f3c fog density, defocused, overlapping slabs | 0.7491561 (−0.113%) | 0.75 ± 1e-05 (xfail < 2.0%) | XFAIL |
| f3d fog density, span + point, shared bucket | 0.7374337 (−1.676%) | 0.75 ± 1e-05 (xfail < 5.0%) | XFAIL |
| f3e unequal density, overlapping spans | +77.411% high / −2.011% low | ≤ 0.5% per px | FAIL |
| f3f unequal density swept, worst cell | +83.648% high / −2.240% low (K 64) | ≤ 0.5% per px, every cell | FAIL |
| g4 K=16 flat-field alpha at α=0.90 | 0.0325 low (0.870724 vs 0.90) | 0 (xfail 0.0325 ± 0.0040) | XFAIL |
| g5 K=16 α=0.10 over-read | +0.0593 (0.105926) | 0 (xfail +0.0593 ± 0.0040) | XFAIL |
| g5 K=16 α=0.30 over-read | +0.0340 (0.310197) | 0 (xfail +0.0340 ± 0.0040) | XFAIL |
| g5 K=4 α=0.10 over-read | +0.0653 (0.106526) | 0 (xfail +0.0653 ± 0.0040) | XFAIL |
| g5 K=16 α=0.01 over-read | +0.0706 (0.010706) | 0 (xfail +0.0706 ± 0.0040) | XFAIL |
| l6 CoC field's own extremum | 9.937e-03 | ≤ 3.9e-03 (xfail < 1.5e-02) | XFAIL |

g4 is the deficit arm and the five g5 rows the over-read arm — Phase 4.1 collapses the former,
Phase 4.2 the latter, and P3.T3 re-pins both. l1/l5 (2.084e-03 / 3.667e-03 against a 3.9e-03
gate) are the closest PASS rows to a boundary and are the ones most likely to move.

**Profile (`run_profile.sh`, variant=defocus, 5 reps):** `min=59.282 median=70.353 mean=68.915
max=79.334` s wall, `cpuMedian=63.390` s, `coresBusyMedian=0.90`, `rssPeakGB=1.770`. Read the
wall figures as a *ratio* only against an after-run taken the same way: this ran under
`scripts/hostguard.sh --mem-gb 6` at nice 19 with a concurrency cap of 2, so ~0.9 cores busy is
the guard, not the node. `cpuMedian` is the stabler comparand.

**Harness memory:** the default hostguard budget (MemTotal/4 = 3.84 GB) kills the run at scene
(i) — it peaks at 4.58 GB. Every harness invocation in this milestone needs
`scripts/hostguard.sh --mem-gb 6`, which the milestone's later `verify` steps should be read as
including.

- 2026-09-10 — **`residualRadiusPx` is captured before pre-merge, not after** (M4.P1.T2). The
  milestone's "after any split/merge" was read as "after step 4's same-bucket split-merge", not
  "after the pre-merge/collision regrouping": a pre-merge group's radius is a union midpoint, so
  taking it would report a radius shallower than the pixel's genuinely deepest surface. The
  residual scatters at the CoC of the deepest surface actually present, so the pre-regroup
  staged radius is the right one. The two differ by at most `merge_tolerance` (0.25 px default)
  when pre-merge is on, and not at all when it is off.
- 2026-09-10 — **The `T = 1` / untouched-radius convention covers the `staged == 0` path too**
  (M4.P1.T2), not just `samples.empty()`. A pixel whose every sample failed the zero-alpha
  early-out has nothing to scatter and no deepest sample either; treating it differently from a
  genuinely empty pixel would be arbitrary.

- 2026-09-10 — **The "GCC -O3 miscompile" reported during M4.P1.T3 was not real**, and the
  `target_compile_options(test_defocus_scatter PRIVATE -O2)` workaround proposed with it was
  dropped, never committed. A differential harness compiling `DeepCDefocusScatter.cpp` at both
  levels with otherwise identical flags and hashing all five planes over 4000 randomized trials
  through the reused-vs-fresh threading pattern gives a **bit-identical** result
  (`c9a7be7c184b54e4`) at `-O2` and `-O3`. Re-introducing the likely intermediate defect —
  `arrival` in `allocate()` but not in `zero()` — reproduces the reported symptom exactly, at
  **both** levels and with the same hash, so `-O2` never cleared it. The implementer hit a real
  bug of their own, fixed it in passing, and did not retest whether the workaround was still
  needed. Independently: the suite is green 12+ consecutive runs at `-O3 -mavx`, and clean under
  ASan+UBSan. **The scoping was also wrong** — the code in question ships at `-O3` in the plugin,
  so lowering only the test binary would have hidden the detector and shipped the defect. Any
  future "lower the optimization level" proposal in this repo gets this treatment before it lands.
- 2026-09-10 — **`allocate()`, not `zero()`, is the production per-band clear** (found at
  M4.P1.T3). `DeepCDefocus.cpp` calls `allocate()` for every band and relies on its unconditional
  re-fill; `zero()` is only ever called by tests. Every pre-existing `allocate()` test ran on a
  fresh object or was immediately followed by `zero()`, so a buffer omitted from `allocate()`'s
  fill on **reuse** was invisible to the suite. Now pinned by a test that dirties the planes and
  re-allocates at the same geometry and at a smaller one. No `src/` defect existed; the gap was
  in coverage.
- 2026-09-10 — **TSan is unusable on this host.** The sandbox blocks the `personality` syscall,
  so `setarch -R` fails and TSan aborts with `unexpected memory mapping` under this kernel's ASLR
  entropy, PIE or not. ASan+UBSan work and are the substitute. The race question this was raised
  for is answered structurally in any case: `BucketPlanes` is a `BandJob` member handed out under
  `_jobLock`, so it is exclusive per thread and no two threads can write the same plane element.

- 2026-09-10 — **`buildResidualWindow()` is extracted as a shared driver, not just a container**
  (M4.P1.T4). The doctest binary has no Nuke/NDK dependency, so a fetch loop living only in
  `DeepCDefocus.cpp` is unreachable from any test — and the output-box-vs-`srcBox` bounds are the
  one thing this task exists to get right. Production and the suite now run the same loop through
  row-fetch/flatten callbacks. Wider than the brief's "if that needs a small extraction, do it",
  and the right call.
- 2026-09-10 — **`ResidualWindow` is counted in `bandBudgetBytes()`, not named as an omission**
  (M4.P1.T4). It costs `2*W*(B + 2*padY)*4`, K-independent — 8.72 MB per band at the 4K/K=16
  defaults with `max_radius=100`/`edge_softness=1.0` (`padY=101`), measured by running the
  compiled function rather than by hand. `BandJob` owns it and comes from `_jobPool`, so it
  scales with `maxInFlight` exactly as the bucket planes and the holdout LUT do; that lifetime is
  what makes counting correct rather than naming it the way the band output planes are named.
  Consequence: `maxInFlight` drops 9 → 8 in the doc block's 4 GB/2160-row case, and the two
  peak-RSS measurements recorded there predate the term and are now undercounts by its size —
  both noted in the block. `bandBudgetBytes()`/`planBands()` gained a trailing `padY` parameter.

- 2026-09-10 — **`DiscKernelLUT::radiusToIndex()` is private; the brief was wrong to point at it**
  (M4.P1.T5). `private:` is at `DeepCDefocusKernel.h:474`, the method at :634 — verified.
  `scatterBackgroundCPU()` takes `const KernelSampler&` and reaches the per-pixel lookup through
  the public `kernel()`, the same seam `scatterBandCPU()` uses, which also keeps the CUDA seam
  uniform rather than special-casing a concrete `DiscKernelLUT&` parameter. **P2.T1's brief cites
  the same private method — read it as "through `KernelSampler::kernel()`" there too.**
- 2026-09-10 — **A sharp-path mutation test was initially vacuous, and the fix is a realistic LUT**
  (M4.P1.T5). Bypassing `scatterBackgroundCPU()`'s sharp fast path broke nothing at first: with a
  LUT built from `minRadius = 0` at `edge_softness = 1.0`, a sub-`kSharpRadiusPx` radius
  degenerates to an identical single-pixel disc, so the two paths agree by construction. Production
  builds its LUT over the frame's **measured** radius range, which rarely reaches 0. Rebuilding the
  fixture at `minRadius = 2.0` makes the mutation clamp to a much larger entry and the test then
  catches it. Any future kernel-path test in this milestone should use a measured-range LUT, not a
  zero-floor one, or it risks pinning nothing — this is M1's standing lesson in its exact form.

- 2026-09-10 — **The claimed "fill flattens an isolated object's bloom into a plateau" was a rig
  artifact, not a defect** (adjudicated at M4.P1.T6). The rig had been "fixed" to a **1×1** residual
  window, which deletes the virtual-background contribution of every surrounding empty pixel and
  reproduces verbatim the failure mode M4.P1.T4 was built to prevent. End-to-end settles it: an
  isolated α=0.8 card over emptiness renders **bit-identical to the pre-M4 baseline** in alpha and
  colour, at full-frame bbox and at a `DeepCrop`-tightened one, falloff intact (0.458700 centre →
  0.003039 → 0.000000). The plugin is provably live — a BG-with-a-hole halo reads 0.446–0.757 on
  baseline and exactly 1.000000 on the new build. **Lesson for the rest of this milestone: a
  residual window in any test rig must be shaped as production shapes it** (full window, `T = 1`
  defaults, background radius from the content's own farthest depth — `frameSetup()` builds buckets
  from the frame's *measured* range, so for a frame holding one object the auto radius IS its
  deepest sample's CoC). A 1×1 window models nothing.
- 2026-09-10 — **`scatterBackgroundCPU()`'s skip floor is `kFillDeficitTol`, not a bare `1e-4`**
  (M4.P1.T6). The two constants were set by different tasks and never reconciled: a pixel whose
  true residual landed in `(1e-5, 1e-4]` had it withheld from the denominator, so `D` undershot 1
  by more than the fill's own tolerance and a genuinely non-opaque pixel was pulled to alpha 1
  (measured error equalled `1 - trueAlpha` to 6 significant figures). They are **not independently
  settable**: the mass a skip withholds is bounded by `tol * Σw = tol`, so the fill's tolerance is
  the loosest floor that provably cannot trigger a false divide.
- 2026-09-10 — **Arrival accuracy is now a correctness requirement, not a cosmetic one.** Anything
  that leaves arrival systematically short of 1 is a **colour and alpha error** after M4.P1.T6, and
  arrival is the least-accurate accumulator in the pipeline: one K-independent float summing every
  fragment's deposit, i.e. ~K× more terms than any per-bucket plane. Weigh that in every later
  change that touches a deposit path — Phase 4.2's blending especially, which doubles the raster
  work feeding it.
- 2026-09-10 — **`kFillMinArrival = 1e-3` is an unbounded cliff, flagged not fixed.** Just above it
  the fill multiplies by ~1000 and the down-clamp caps alpha at 1; a doctest pins that as intended.
  It is correct wherever numerator and arrival stay proportional, which is the normal case, but
  nothing bounds the case where they do not. No scene reaches it today. Revisit if one ever does.
- 2026-09-10 — **User rulings on the two M4.P1.T6 defects**: the residual-radius convention is
  **fixed now**, as new task M4.P1.T7, because shipping a 0.3-alpha silhouette band would trade one
  visible artifact for another in a milestone whose whole purpose is removing them. The c4
  share-side deficit is **investigated after Phase 4.2**, as new task M4.P2.T4, since kernel
  blending rewrites how weights reach arrival and may move or explain it.

- 2026-09-10 — **The T7 brief was wrong and execution refuted it with a proof, not a preference.**
  "Split the residual positively across each part's own radius" is unreachable: for an isolated
  source pixel in a uniform `T` field the neighbours' kernels tile to 1, so
  `arrival(x) = 1 - K_R(x-p) + own_p(x)`, and exactness forces `own_p == K_R` — a single unit
  kernel at the residual radius. Non-negative per-part portions cap at 0.261 of the required 1.0 at
  15 parts, and measured, that direction makes the artifact worse. What shipped is the inverse:
  pool the parent's whole arrival claim onto the deepest part's radius. **`share` feeds only the
  two arrival deposits**, so this moves the denominator and never the numerator — that is what
  makes it safe, and it is worth re-checking before anything else touches `share`.
- 2026-09-10 — **c4 was the same defect after all, and T7 fixed it.** The adjudication correctly
  located it on the **share** side (at α=1.0 the residual deposits nothing, yet arrival still read
  0.99965) but concluded it was independent of the residual-radius convention. It was not: pooling
  is a share-side change, and c4 returned to 1.397e-05 exactly. M4.P2.T4 is re-scoped from
  investigation to confirmation.
- 2026-09-10 — **f3g and f3h do NOT return to baseline, and f3g provably cannot from within T7's
  scope.** Both are additivity controls (`merged == soloA + soloB`) that were exact pre-M4. Traced
  with a `background_depth` probe (since reverted): f3g's cards are one bucket, one part, radius
  **27.895 px**, while the auto background radius is **12.25 px** (the CoC at the range anchor's
  z=16); forcing `background_depth = 28` returns f3g to its exact pre-M4 reading. So f3g measures
  the **object-vs-background radius mismatch** the design documents as a conditional, not the
  split-part defect. f3h (11 parts, radii 37.67 -> 21.538) *is* the T7 defect — with the fix plus
  `background_depth = 21.53846` it reads its exact pre-M4 numbers — but the auto radius still
  cannot match it. **P3.T3 owns re-specifying both**, with hard outer bounds, and must not re-pin
  them against new output. The open question underneath — what the auto background radius should be
  when frame content sits at a very different CoC from `depthMax()` — is real and is NOT in this
  milestone; raise it at the gate.
- 2026-09-10 — **f3e/f3f moved in the wrong direction at T7** (+75.686/-3.470 -> +87.379/-1.585, and
  worst cell +91.008% -> +104.788%). Both were already FAIL pre-M4 and belong to M1.P3.T23, not to
  this milestone, but the movement is real and is recorded here so it is not mistaken for noise
  later.
