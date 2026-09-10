# DeepCDefocus: coverage renormalization — fix focal-line bands and FG-silhouette halos (v2)

Supersedes `COVERAGE-FILL-PLAN.md`. This revision folds in the independent review's
findings (`COVERAGE-FILL-RECOMMENDATIONS.md`) and its cost notes
(`COVERAGE-FILL-PERFORMANCE.md`). What changed from v1, in review order:

| # | Change | Kind | Where in this plan |
|---|--------|------|--------------------|
| 1 | Virtual background covers the full fetch window, not just `srcBox` | correctness | Design §2, T3 |
| 2 | Residual behind a pixel's stack scatters at that pixel's own deepest-sample CoC; the global `background_depth` radius is only for pixels with no samples | correctness (semi-transparent) | Design §2, T1, T3 |
| 3 | Deficit division fires only for `D < 1 - 1e-5`, not `D < 1` | correctness (size-0 parity) | Design §3, T4 |
| 4 | Kernel blending for fractional diameters is promoted from "deferred polish" to an in-plan task, with an explicit split rule | correctness (semi-transparent near focus) | Design §4, T5 |
| — | Per-bucket arrival planes explicitly rejected | design decision held | Design §2 |
| — | m4 becomes a holdout *commutation* test; m0 also asserts colour ratio | tests | T6 |
| — | "Blooms over emptiness unchanged" restated as conditional | properties | Design §3 |
| — | node_help wording: divides by un-held-out arrival; invents FG-coloured coverage only where the renderer wrote none | docs | T7 |

## Context

Defocusing a plane at 45° to camera (focal plane mid-frame) produces bands of
reduced opacity either side of the focal line, and reduced-opacity halos around
objects in front of the focal plane. Bokeh/pgBokeh show neither. Both artifacts
are real, measured, and already characterized in this repo:

1. **Bands**: DeepCDefocus is a scatter algorithm; each fragment's disc kernel
   sums to 1, but the weight *arriving* at an output pixel does not sum to 1
   when CoC varies spatially (sharp-path delta at r<0.5px vs r=0.5 disc,
   kernel-grid snapping, and the true-gradient term — adjacent scanlines
   genuinely 0.5px apart in radius). Scene (g) measures up to **7.4e-2 deficit
   near focus, K-invariant**; scene (l) l5 is the 45° case and is worse.
   Deficits are only ever clamped DOWN (`saturateBucketPlanes`), never up.
   The same mechanism also produces **over-reads** of up to ~5.9% near focus
   (g5's surplus cells); deficit-only division cannot correct those, which is
   why kernel blending (§4) is in this plan rather than deferred.
2. **Halos**: the "COVERAGE DEFICIT (specified behaviour)" block
   (`src/DeepCDefocus.cpp:125-149`) — defocused FG scatters off its silhouette;
   vacated pixels lack occluded-BG samples, leaving an alpha dip ~one CoC wide.
3. 2D nodes don't show this because a flat image has a source pixel (with Z)
   under every output pixel — their scatter weight sums are ~1 everywhere, an
   implicit renormalization.

**User decisions (asked and answered):** fill like pgBokeh (renormalize so
opacity holds up, even where the renderer wrote no occluded samples), and make
it the **only behavior — no legacy knob**. The "honest dip" contract is
retired; docs, scene (i), and XFAIL pins are re-specified accordingly.

A full design was worked out by a planning pass (verified against source):
`~/.claude/plans/there-s-a-fundamental-issue-squishy-tulip-agent-a9a1aa83e0f5f5bca.md`
— refer to it for line-level detail. This file is the executable summary,
adjusted for the user's decisions and the review.

## Design: gather-consistent renormalization with virtual background coverage

Four pieces, all always-on. Pieces 1–3 are the fill; piece 4 is the kernel
step that the fill cannot correct on its own.

### 1. Gather-share partition (flatten side) — `src/DeepCDefocusScatter.cpp`
In `flattenPixelToSoA()`'s existing front-to-back staging loop (step 4,
~:668-797), partition each source pixel's unit area over its fragments:
`share = t * alpha; t *= (1-alpha)`; residual `T_px = t` is the virtual
background's claim. Shares + residual sum to exactly 1 by construction
(volumetric split parts each take `t * part.alpha`; pre-merge sums member
shares; collision attenuation must NOT touch shares). Plumbing:
`FlattenScratch::Staged.share`, `FragmentRecord.share`,
`SampleSoA.arrivalShare` (+`appendFragment`), out-params `float* residualT`
**and `float* residualRadiusPx`** — the scatter radius of the pixel's deepest
sample (the last one staged front-to-back, after any split/merge), which is
the radius the residual scatters at (§2). For a pixel with no samples both
out-params are left at their "empty" defaults (`T = 1`, radius = global bg).

### 2. Arrival plane + virtual background (scatter side)
- `BucketPlanes` gains a fifth, K-independent buffer `arrival`
  (`src/DeepCDefocusScatter.cpp:1344-1405` alloc/zero/release/view/sizeBytes;
  `bytesForBand` `.h:1188` + memory_limit comment — one float per band pixel;
  on holdout-connected bands the existing LUT is already K+1 floats per pixel,
  so this is a small fraction of current memory). Planes are per-BandJob and
  thread-private — no BandLedger changes needed.
- **One arrival plane, kernel-size-independent, deficit-only division. Do NOT
  add per-bucket arrival planes.** The coverage plane also carries holdout
  visibility and follows an area model (one full deposit per parent sample),
  so a bucket can legitimately hold more than unit weight; per-bucket raw
  arrival would only fix the narrow same-surface surplus case at K× the memory
  cost. Surpluses stay with the existing excess/attenuation machinery.
- `scatterFragmentSpans()` (`.h:1810`): once per fragment (group 0), deposit
  `arrival[dst+i] += wRaw[i] * share` using the **raw** kernel row
  (`kv.rowWeights(row)+skip`, before the holdout vis fold at `.h:1866-1872`) —
  the denominator must exclude holdout attenuation so held-out alpha is never
  renormalized back up. Same for `scatterFragmentSharp()` (`.h:1896`):
  `arrival[dst] += share`. (After §4 lands, a blended fragment deposits
  `share * ((1-f)·wA_raw + f·wB_raw)`, i.e. both bracketing kernels feed
  arrival with the same blend weight the colour deposit uses.) This extra
  per-span loop over raw weights costs less than the visibility interpolation
  already running alongside it.
- **Virtual background**: in `computeBand()`'s fetch loop
  (`src/DeepCDefocus.cpp:1706-1725`), build a residual-T map **and a residual-
  radius map** over the fetch window. **Residual `T` defaults to 1.0 for every
  pixel of the fetch window that lies inside the output box, whether or not it
  lies in `srcBox`.** (v1 restricted the virtual background to `srcBox`. For a
  deep input whose bbox is tight around its content, every bloom pixel outside
  that box would get no virtual background: arrival equals the bloom's own
  weight, alpha divides to exactly 1, and a soft edge becomes a hard disc.)
  For pixels with samples, `T_px` and `residualRadiusPx` come from flatten.
- New `scatterBackgroundCPU()`: for each source pixel with `T > 1e-4`, deposit
  `w * T` into `arrival` using a kernel at that pixel's **residual radius**:
  - pixel has samples → the CoC of its own deepest sample (so the residual
    shares the surface's defocus; with a mismatched radius a true alpha-0.9
    surface reads back ~0.893 instead of 0.900 — ~1.8 code values);
  - pixel has no samples → the global `backgroundRadiusPx` (new knob
    `background_depth`, default 0 = auto = CoC at `buckets.depthMax()`; manual
    value clamped to rMax; computed in `frameSetup()` near :1333-1340).
  Perf: this is no longer a single convolution of the T map with one kernel.
  Naive cost is π·r² per non-opaque source pixel. Measure with
  `tests/nuke/run_profile.sh` before optimizing; the obvious mitigation is to
  bucket residuals by kernel bin and convolve per bin.

### 3. Deficit-only division (composite side)
Thread `arrival` through `resolveBandCPU()` (`.cpp:1605`) into
`compositePixelCoveragePartition()`. Immediately **before** the existing
down-only clamp block at `src/DeepCDefocusScatter.h:2947-2953`:

```
D = arrival[pixel]                       // real shares + virtual bg, raw w
if (D > kFillMinArrival(~1e-3) && D < 1.0f - kFillDeficitTol(1e-5f)) {
    s = 1/D; accAlpha *= s; outColor[all c] *= s;   // premult pair together
}
// existing accAlpha>1 clamp block then absorbs any overshoot, pair locked
```

The `1 - 1e-5` tolerance is not optional: at exact size-0 (no blur) the
floating-point share sums can land one ulp below 1 and would otherwise trigger
the division, moving pixels that must be untouched by one ulp and breaking the
bit-exact size-0 parity gate (a).

Deficit-only (`D < 1 - tol`): surpluses stay with the existing
excess/attenuation machinery. Properties this preserves (each pinned by a unit
test or scene):
- empty stays exactly black (numerator 0) — scene (d);
- **soft blooms over emptiness are unchanged *conditionally*, not as an
  invariant**: arrival equals 1 around an isolated object only when that
  object's CoC equals the radius the surrounding empty pixels scatter their
  virtual background at. That holds for a single-depth scene under the auto
  default (bg radius = CoC at `depthMax()` = the object's own CoC). In a scene
  with far geometry elsewhere, a near object's fringe over emptiness can be
  boosted where its kernel is smaller than the background kernel. The per-pixel
  residual radius (§2) does not remove this — truly empty pixels have no sample
  to take a radius from. Pinned by scene (e)/m2 under the single-depth
  condition; the multi-depth case is documented, not pinned;
- fog identities unchanged where fog covers the frame (shares+residual make
  D == 1 exactly; e.g. two 0.5 layers still read 0.75);
- holdout regions never boosted (numerator carries vis, D doesn't) — and more
  strongly, fill and holdout **commute** (T6 m4): the numerator scales linearly
  with visibility per fragment while the divisor ignores visibility, so a
  holdout applied before the fill gives the same ratio as one applied after.

### 4. Kernel blending for fractional diameters (was v1's deferred "Option B")
The current kernel step is a hard switch: sharp one-pixel path for
`!(radius >= kSharpRadiusPx)` (0.5px, `.h:503`), else the disc whose radius
rounds to the nearest node of the non-uniform `kernelGridIndex()` grid. That
step is what produces the ~5.9% over-read near focus, and deficit-only
division has no mechanism to correct an over-read. For opaque geometry the
existing alpha>1 clamp hides it; for semi-transparent surfaces near focus it is
visible in alpha, and blending is the only route to correct alpha there.

Fix: replace the sharp/disc step with a **minimum kernel diameter of 1 px**
and, for any fractional diameter `d`, **blend the two odd-sized kernels that
bracket `d`**, with weight `f = (d - dA) / (dB - dA)`. The 1×1 kernel is the
sharp path's delta, so `d ≤ 1` reproduces today's sharp fast path exactly
(size-0 parity, gate (a), still holds). Both kernels deposit into colour and
into arrival with the same `f` (§2).

Things that encode "these two radii rasterise identically" and must move in
lockstep or the flatten absorb becomes lossy:
- `scatterKernelBin()` / `sameScatterKernel()` (`.h:548-560`): two radii now
  rasterise identically only if they share both bracketing kernels *and* the
  same `f`; make the predicate strict (same pair, `|Δf|` below an epsilon) —
  the conservative direction, as the comment block there requires.
- `DiscKernelLUT::radiusToIndex()` and the `kernelGridIndex()` grid
  (`DeepCDefocusKernel.h`): the review specifies odd integer diameters as the
  bracketing set. Reconcile with the existing radius grid — either the grid
  becomes the integer-diameter set, or the blend interpolates between adjacent
  existing nodes. Decide at T5 after reading the LUT builder; the spec is
  "continuous in `d`, exact at the nodes, minimum 1px".
- `pre_merge` / `merge_tolerance` (lossy by design at 0.25px; harness i7) —
  unchanged in semantics, but re-measure i7's pinned number.
- `kSharpRadiusPx` and the LUT `rMin` contract (`.h:499-503`).

Cost: near integer kernel sizes a fragment rasterises two kernels instead of
one — roughly double the per-fragment raster work there. Measure in T9.

## Ordered tasks

- **T0 Baseline**: `cmake -S . -B build/local-17.0 -D Nuke_ROOT=/usr/local/Nuke17.0v3 -D DEEPC_BUILD_TESTS=ON`;
  `scripts/hostguard.sh -- cmake --build build/local-17.0 -j`; run doctest
  unit tests; `DEEPC_PLUGIN_DIR=$PWD/build/local-17.0/src scripts/hostguard.sh
  -- tests/nuke/run_validation.sh` (guards against the stale
  `build/local-17.0-O3` mtime trap). Record all scene numbers; **preserve this
  baseline .so** (copy aside) for before/after renders.
- **T1 Flatten shares + residual** (piece 1) + unit tests: shares+residual == 1
  to 1e-6, fuzzed over point/volumetric/pre-merge/collision stacks; collisions
  leave shares untouched; `residualRadiusPx` equals the deepest staged
  sample's scatter radius for point, volumetric-split and pre-merged stacks;
  empty pixel returns the "empty" defaults.
- **T2 Arrival plane + deposits** (piece 2, minus virtual bg) + unit tests:
  arrival is vis-independent (bit-identical with/without holdout LUT);
  `BucketPlanes::zero()` clears the fifth buffer (extend the four-plane test
  ~`tests/test_defocus_scatter.cpp:3802`); `bytesForBand` includes it.
- **T3 Virtual background**: residual-T and residual-radius maps over the full
  fetch window (default `T = 1` for every fetch-window pixel inside the output
  box, independent of `srcBox`), `background_depth` knob + `frameSetup()`
  wiring (explicit params, the file's convention), `scatterBackgroundCPU()`
  with per-pixel residual radius. Unit tests:
  - bg deposits sum to T per source pixel;
  - isolated opaque fragment over an empty field keeps its bloom profile
    unchanged by the division (single-depth condition);
  - **tight-bbox bloom**: an isolated *blurred* fragment whose deep bbox is
    tight around it keeps its bloom profile — bloom pixels outside `srcBox`
    must not divide to alpha 1 (review item 1);
  - **per-pixel residual radius**: alpha-0.9 flat field with a forced 0.93
    arrival reads 0.900 to 1e-6 (review item 2; with a mismatched global
    radius it reads ~0.893);
  - pixels with no samples use the global radius; pixels with samples use
    their deepest sample's radius.
- **T4 Composite division** + unit tests: flat opaque D=0.93 → alpha exactly
  1, colour ratio preserved; D=1.3 → untouched; D=0 with zero numerator → 0;
  `kFillMinArrival` boundary; **deficit tolerance: a sharp-path-only pixel
  with `D = 1 - 1 ulp` is left bit-identical** (review item 3), and
  `D = 1 - 2e-5` does divide; interaction with the accAlpha>1 clamp (pair
  stays locked); two-fog-layers 0.75 identity.
- **T5 Kernel blending** (piece 4): minimum 1px diameter, bracketing-kernel
  blend, arrival deposit from both kernels, `scatterKernelBin`/LUT/`rMin`
  reconciliation as listed in §4. Unit tests: kernel weights are continuous in
  `d` and exact at the nodes; size-0 (all `d ≤ 1`) rasterises bit-identically
  to the old sharp path; a semi-transparent (α=0.9) surface at a fractional
  diameter reads 0.900 within 1/255 across the whole near-focus ramp (the
  case deficit-only division cannot fix); `sameScatterKernel` is never true
  for radii that rasterise differently (fuzzed).
  **Split rule** (review item 4): if schedule forces a split, ship T0–T4 and
  T6–T9 first, re-pinning the affected over-read cells (g5 +5.9% etc.) only as
  an interim measure, then land T5 as the *immediate* next change and re-pin
  those cells again. Opaque geometry is acceptable without T5 (the alpha
  clamp hides the over-read); semi-transparent geometry is not.
- **T6 Scene (m)** in `tests/nuke/scenes.py` (+ `SCENES` dict ~:2965, and a
  documentation `scene_m_coverage_halo.nk` per repo convention). Cells:
  m0 occluded-samples-present control — interior alpha 1 within 1/255 **and
  interior unpremult colour ratio within 1/255 of the source**: near
  silhouettes, occluded samples can be scattered into the numerator but
  excluded from the denominator, alpha overshoots 1, and the clamp scales
  colour with alpha, so both need checking;
  m1 the halo — BG with a hole under the FG silhouette: dip depth ≤ 1/255,
  and filled-band colour is FG colour (unpremult ratio — fill must not invent
  BG colour);
  m2 FG over nothing — bloom profile matches baseline render (dev check vs.
  preserved T0 .so; shipped as pinned profile numbers; single-depth scene so
  the conditional bloom property applies);
  m3 the user's artifact — 45° ramp at scene-g slope, α=1 and α=0.9, interior
  |a−1| ≤ 1/255 (α=1) and |a−0.9| ≤ 1/255 (α=0.9) INCLUDING the near-focus
  rows scene (g) excludes (acceptance check for this bug; the α=0.9 rows are
  the T5 acceptance check and are XFAIL-pinned if T5 is split out);
  **m4 holdout commutation** — render the scene with and without a 0.5-alpha
  holdout card in front of everything; the ratio of the two renders under the
  card must hold to float precision at every pixel, including rows the fill
  changes. This subsumes v1's "held-out alpha never boosted" guard;
  m5 over-checkerboard EXR writes of m1/m3 for eyeballing.
- **T7 Re-spec + re-pin (same commit as the behavior change)**:
  scene (i) re-specified — the deficit now fills (that IS the feature);
  scene (g) g4 `0.0325±0.004` (scenes.py:1805) and g5 cells (:1933-1941) —
  deficit cells collapse; over-read cells collapse too **if T5 is in the same
  change**, otherwise they survive deficit-only division and are re-pinned
  as interim (see T5 split rule); re-measure and re-pin all; l6
  `hardTol 1.5e-02` (:2953); f2/f3 hard bounds (:622, :668) if moved; i7's
  pre-merge number if T5 moved it; `scene_g_banding.nk` StickyNote2 numbers;
  node_help rewrite (`src/DeepCDefocus.cpp:125-149` COVERAGE DEFICIT block,
  :199-211 limitation numbers, the "alpha-renormalise … deliberately not in
  this version" text). New semantics, stated plainly:
  - the fill divides by **un-held-out arrival**, so holdout attenuation is
    preserved exactly (fill and holdout commute);
  - the fill invents **foreground-coloured** coverage only where the renderer
    wrote none, like a 2D defocus; it never invents background colour;
  - `background_depth` sets the defocus of invented coverage for pixels the
    deep image left empty (auto = farthest measured depth); coverage behind a
    pixel's own samples always takes that pixel's deepest sample's defocus.
  Also update the composite header's honest-alpha commentary
  (`.h:2351-2399`, :2941-2944) and the `scatterKernelBin` comment block if T5
  changed the predicate.
- **T8 Full validation**: entire a–l+m suite green (XFAILs re-pinned);
  must-hold gates: (a) size-0 parity bit-exact (depends on the T4 tolerance
  and T5's 1px minimum), (c) energy conservation alpha==1, (d) empty exactly
  black, (e) sharp edge ≤1px + bloom extent + no BG bleed, (h) overlap
  normalization, (b)/(f) holdout parity, (m4) holdout commutation.
- **T9 Visual + perf sign-off**: baseline-.so vs new-.so renders of m1/m3
  over checkerboard (m5 outputs) for pixel judgment; `tests/nuke/run_profile.sh`
  before/after. Costs to watch, in expected order of size: the per-radius
  virtual-bg scatter on sparse and semi-transparent frames (bucket by kernel
  bin if it shows), the doubled raster work near integer kernel sizes from
  T5, then the arrival deposit loop (expected to be lost in the noise). None
  of these gate correctness; optimize only if the profile demands.

## Files touched
- `src/DeepCDefocusScatter.cpp` — flatten shares/residual/residual radius,
  BucketPlanes arrival buffer, per-radius virtual-bg scatter, resolveBandCPU
  threading
- `src/DeepCDefocusScatter.h` — ScatterFragment.share, deposit bodies (both
  kernels after T5), composite division with tolerance, `scatterKernelBin`
  predicate, doc-block updates
- `src/DeepCDefocusKernel.h` — kernel grid / blend reconciliation (T5)
- `src/DeepCDefocus.cpp` — `background_depth` knob, frameSetup wiring,
  computeBand residual-T and residual-radius maps over the full fetch window,
  node_help rewrite
- `src/DeepCDefocusMath.h` — only if share helpers land there
- `tests/test_defocus_scatter.cpp` — unit tests T1-T5
- `tests/nuke/scenes.py`, `tests/nuke/generate_scene_scripts.py`,
  `scene_m_coverage_halo.nk`, scene_g/i re-pins

## Verification
Unit tests (doctest, no Nuke) per task; then the full hostguard-wrapped
headless-Nuke suite with `DEEPC_PLUGIN_DIR` pinned to the fresh build; scene
(m) m3 is the acceptance check for the reported artifact (its α=0.9 rows are
the acceptance check for T5); m4 is the acceptance check that fill and holdout
commute; m5 checkerboards + a baseline-.so comparison render are the visual
sign-off. Everything runs through `scripts/hostguard.sh` on this shared
4-core/15GB host.

## Risks
- Contract change is deliberate and total (no legacy path): scene (i) and all
  honest-alpha doc blocks must be re-specified in the same commit or the suite
  goes red.
- Virtual-bg perf: per-pixel residual radii turn a single convolution into one
  per distinct radius. Mitigation identified (bucket by kernel bin), deferred
  until `run_profile.sh` shows it.
- Global bg radius now applies ONLY to pixels with no samples at all. It
  carries zero alpha/colour and only enters the denominator, so it shapes how
  smoothly the fill varies across truly empty regions, never per-sample blur.
  Known residual effect: a near object's fringe over emptiness in a scene with
  far geometry elsewhere can be boosted where its kernel is smaller than the
  background kernel (see the conditional bloom property in §3). Documented,
  not fixed — an empty pixel has no depth to borrow.
- T5 touches the "identical rasterisation" predicate that makes the flatten
  absorb lossless. The predicate must be tightened, never loosened; the fuzzed
  `sameScatterKernel` test in T5 is the guard.
- If T5 is split out, g5's over-read cells and m3's α=0.9 rows persist as
  interim pins. They are the record of what T5 owes; do not chase them in the
  fill change.
