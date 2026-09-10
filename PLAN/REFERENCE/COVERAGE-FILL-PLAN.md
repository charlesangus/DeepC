# DeepCDefocus: coverage renormalization — fix focal-line bands and FG-silhouette halos

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
`/home/bosley/.claude/plans/there-s-a-fundamental-issue-squishy-tulip-agent-a9a1aa83e0f5f5bca.md`
— refer to it for line-level detail. This file is the executable summary,
adjusted for the user's decisions (fill unconditional; the "Option B"
discretization polish deferred to a follow-up).

## Design: gather-consistent renormalization with virtual background coverage

Three pieces, all always-on:

### 1. Gather-share partition (flatten side) — `src/DeepCDefocusScatter.cpp`
In `flattenPixelToSoA()`'s existing front-to-back staging loop (step 4,
~:668-797), partition each source pixel's unit area over its fragments:
`share = t * alpha; t *= (1-alpha)`; residual `T_px = t` is the virtual
background's claim. Shares + residual sum to exactly 1 by construction
(volumetric split parts each take `t * part.alpha`; pre-merge sums member
shares; collision attenuation must NOT touch shares). Plumbing:
`FlattenScratch::Staged.share`, `FragmentRecord.share`,
`SampleSoA.arrivalShare` (+`appendFragment`), out-param `float* residualT`.

### 2. Arrival plane + virtual background (scatter side)
- `BucketPlanes` gains a fifth, K-independent buffer `arrival`
  (`src/DeepCDefocusScatter.cpp:1344-1405` alloc/zero/release/view/sizeBytes;
  `bytesForBand` `.h:1188` + memory_limit comment). Planes are per-BandJob and
  thread-private — no BandLedger changes needed.
- `scatterFragmentSpans()` (`.h:1810`): once per fragment (group 0), deposit
  `arrival[dst+i] += wRaw[i] * share` using the **raw** kernel row
  (`kv.rowWeights(row)+skip`, before the holdout vis fold at `.h:1866-1872`) —
  the denominator must exclude holdout attenuation so held-out alpha is never
  renormalized back up. Same for `scatterFragmentSharp()` (`.h:1896`):
  `arrival[dst] += share`.
- **Virtual background**: in `computeBand()`'s fetch loop
  (`src/DeepCDefocus.cpp:1706-1725`), build a residual-T map over the fetch
  window (default 1.0 = fully background for empty pixels; `T_px` from flatten
  otherwise). New `scatterBackgroundCPU()`: for each source pixel with
  `T > 1e-4`, deposit `w * T` into `arrival` with ONE shared kernel at
  `backgroundRadiusPx` (new knob `background_depth`, default 0 = auto = CoC at
  `buckets.depthMax()`; manual value clamped to rMax; computed in
  `frameSetup()` near :1333-1340). Pixels outside `srcBox` get no virtual
  background — the background exists only where the deep image is defined.
  Perf risk noted: naive π·r² per non-opaque pixel; it is a convolution of the
  T map with one kernel — optimize only if `run_profile.sh` demands.

### 3. Deficit-only division (composite side)
Thread `arrival` through `resolveBandCPU()` (`.cpp:1605`) into
`compositePixelCoveragePartition()`. Immediately **before** the existing
down-only clamp block at `src/DeepCDefocusScatter.h:2947-2953`:

```
D = arrival[pixel]                       // real shares + virtual bg, raw w
if (D > kFillMinArrival(~1e-3) && D < 1.0f) {
    s = 1/D; accAlpha *= s; outColor[all c] *= s;   // premult pair together
}
// existing accAlpha>1 clamp block then absorbs any overshoot, pair locked
```

Deficit-only (`D < 1`): surpluses stay with the existing excess/attenuation
machinery. Properties this preserves (each pinned by a unit test or scene):
- empty stays exactly black (numerator 0) — scene (d);
- soft blooms over emptiness unchanged (surrounding empty pixels contribute
  T=1 virtual-bg weight, so D≈1 there; division ≈ no-op) — scene (e)/(m2);
- fog identities unchanged where fog covers the frame (shares+residual make
  D == 1 exactly; e.g. two 0.5 layers still read 0.75);
- holdout regions never boosted (numerator carries vis, D doesn't).

## Deferred follow-up (separate change, not this plan)
Kernel-grid interpolation + sharp↔disc blend ("Option B" in the agent plan):
pure discretization polish (~2e-3 scale, plus RGB texture). Fill fixes the
reported artifacts without it. Revisit if T8's checkerboards still show
texture near focus.

## Ordered tasks

- **T0 Baseline**: `cmake -S . -B build/local-17.0 -D Nuke_ROOT=/usr/local/Nuke17.0v3 -D DEEPC_BUILD_TESTS=ON`;
  `scripts/hostguard.sh -- cmake --build build/local-17.0 -j`; run doctest
  unit tests; `DEEPC_PLUGIN_DIR=$PWD/build/local-17.0/src scripts/hostguard.sh
  -- tests/nuke/run_validation.sh` (guards against the stale
  `build/local-17.0-O3` mtime trap). Record all scene numbers; **preserve this
  baseline .so** (copy aside) for before/after renders.
- **T1 Flatten shares** (piece 1) + unit test: shares+residual == 1 to 1e-6,
  fuzzed over point/volumetric/pre-merge/collision stacks; collisions leave
  shares untouched.
- **T2 Arrival plane + deposits** (piece 2, minus virtual bg) + unit tests:
  arrival is vis-independent (bit-identical with/without holdout LUT);
  `BucketPlanes::zero()` clears the fifth buffer (extend the four-plane test
  ~`tests/test_defocus_scatter.cpp:3802`); `bytesForBand` includes it.
- **T3 Virtual background**: residual-T map, `background_depth` knob +
  `frameSetup()` wiring (explicit params, the file's convention),
  `scatterBackgroundCPU()`. Unit test: bg deposits sum to T per source pixel;
  isolated opaque fragment over an empty field keeps its bloom profile
  unchanged by the division.
- **T4 Composite division** + unit tests: flat opaque D=0.93 → alpha exactly
  1, colour ratio preserved; D=1.3 → untouched; D=0 with zero numerator → 0;
  epsilon boundary; interaction with the accAlpha>1 clamp (pair stays locked);
  two-fog-layers 0.75 identity.
- **T5 Scene (m)** in `tests/nuke/scenes.py` (+ `SCENES` dict ~:2965, and a
  documentation `scene_m_coverage_halo.nk` per repo convention). Cells:
  m0 occluded-samples-present control (interior alpha 1 within 1/255);
  m1 the halo — BG with a hole under the FG silhouette: dip depth ≤ 1/255,
  and filled-band colour is FG colour (unpremult ratio — fill must not invent
  BG colour); m2 FG over nothing — bloom profile matches baseline render
  (dev check vs. preserved T0 .so; shipped as pinned profile numbers);
  m3 the user's artifact — 45° ramp at scene-g slope, α=1 and α=0.9, interior
  |a−1| ≤ 1/255 INCLUDING the near-focus rows scene (g) excludes (acceptance
  check for this bug); m4 holdout guard — held-out alpha never boosted;
  m5 over-checkerboard EXR writes of m1/m3 for eyeballing.
- **T6 Re-spec + re-pin (same commit as the behavior change)**:
  scene (i) re-specified — the deficit now fills (that IS the feature);
  scene (g) g4 `0.0325±0.004` (scenes.py:1805) and g5 cells (:1933-1941) —
  deficit cells should collapse, over-read cells (+5.9% etc.) survive
  deficit-only division: re-measure and re-pin all; l6 `hardTol 1.5e-02`
  (:2953); f2/f3 hard bounds (:622, :668) if moved;
  `scene_g_banding.nk` StickyNote2 numbers; node_help rewrite
  (`src/DeepCDefocus.cpp:125-149` COVERAGE DEFICIT block, :199-211 limitation
  numbers, the "alpha-renormalise … deliberately not in this version" text) —
  new semantics stated plainly: coverage fill invents FG-coloured coverage
  where the renderer wrote none, like a 2D defocus. Also update the composite
  header's honest-alpha commentary (`.h:2351-2399`, :2941-2944).
- **T7 Full validation**: entire a–l+m suite green (XFAILs re-pinned);
  must-hold gates: (a) size-0 parity bit-exact, (c) energy conservation
  alpha==1, (d) empty exactly black, (e) sharp edge ≤1px + bloom extent + no
  BG bleed, (h) overlap normalization, (b)/(f) holdout parity.
- **T8 Visual + perf sign-off**: baseline-.so vs new-.so renders of m1/m3
  over checkerboard (m5 outputs) for pixel judgment; `tests/nuke/run_profile.sh`
  before/after (arrival deposits + virtual-bg pass on sparse frames are the
  costs to watch).

## Files touched
- `src/DeepCDefocusScatter.cpp` — flatten shares/residual, BucketPlanes
  arrival buffer, virtual-bg scatter, resolveBandCPU threading
- `src/DeepCDefocusScatter.h` — ScatterFragment.share, deposit bodies,
  composite division, doc-block updates
- `src/DeepCDefocus.cpp` — `background_depth` knob, frameSetup wiring,
  computeBand residual-T map, node_help rewrite
- `src/DeepCDefocusMath.h` — only if share helpers land there
- `tests/test_defocus_scatter.cpp` — unit tests T1-T4
- `tests/nuke/scenes.py`, `tests/nuke/generate_scene_scripts.py`,
  `scene_m_coverage_halo.nk`, scene_g/i re-pins

## Verification
Unit tests (doctest, no Nuke) per task; then the full hostguard-wrapped
headless-Nuke suite with `DEEPC_PLUGIN_DIR` pinned to the fresh build; scene
(m) m3 is the acceptance check for the reported artifact; m5 checkerboards +
a baseline-.so comparison render are the visual sign-off. Everything runs
through `scripts/hostguard.sh` on this shared 4-core/15GB host.

## Risks
- Contract change is deliberate and total (no legacy path): scene (i) and all
  honest-alpha doc blocks must be re-specified in the same commit or the suite
  goes red.
- Virtual-bg perf on sparse frames (optimization path identified, deferred).
- Single bg radius: applies ONLY to the virtual-background weight — invented
  coverage for data the renderer never wrote (empty pixels, residual behind a
  stack). It carries zero alpha/color and only enters the denominator, so it
  shapes how smoothly the fill amount varies spatially, never per-sample blur
  (every real sample keeps its own exact per-depth CoC). Auto default = far
  end of measured range, which matches the typical missing-BG depth. If scenes
  with multiple background depths ever show it, the refinement is a per-pixel
  bg radius from each pixel's own deepest sample (residual is at least that
  deep), knob/auto for fully empty pixels.
- g5's over-read cells are NOT fixed by deficit-only division (different
  mechanism, tracked in the risk register) — expect them to persist; re-pin,
  don't chase here.
