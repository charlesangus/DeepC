# Milestone 8: Depth-ordered colour within a bucket, and the silhouette-edge oracle

Scene (o)'s o6c (M7.P3.T5) pins a colour defect that predates M7. On the mixed-opacity rig, colour:alpha
is up to 0.28 off Bokeh while alpha is exact. There are two mechanisms (see the M7 file's
`## Decisions`, M7.P3.T5 entry; evidence in `~/deepc-validation/M7-P3T5/` and its `o6c-mechanism/`
subdirectory):

- **(1) A near surface and the surface it hides share one depth bucket.** At K=4 they share it
  outright. At K=16/64 they share it through the near card's fractional depth-split share. The
  bucket's alpha is summed and saturated as one, so the two surfaces mix by alpha (0.89 : 1.0) instead
  of in depth order. This is a node defect. The fix is recommended and was accepted by the user:
  per-tile adaptive bucket boundaries. It keeps the additive planes and the atomic splat. (Rejected:
  per-fragment order within a bucket. Fallback: per-bucket depth-moment planes, at roughly 2× plane
  memory.)
- **(2) Weighting on a defocused silhouette.** The node weights the stack by thin-lens disc coverage
  (0.52 half a pixel inside the edge), where Bokeh reads 0.84. Nobody has decided which is right. The
  user asked for an independent reference render before choosing.

M8 builds that reference first and asks the user for a ruling. In parallel it fixes mechanism (1) with
an oracle that does not depend on the ruling. Then it re-oracles o6c as the ruling says. The milestone
runs after M7 (shipped 2026-09-24 as `2ac3550`, PR #109) and before M2.

## What we know (read before any task)

**Baseline.** Master is `2ac3550`, the M7 head plus PR #109's review fixes (`91de156`).
- M7.P4.T2's a–o tally at `ed98e68` was **`PASS=172 FAIL=2 XFAIL=12 SKIP=2`**:
  - a–f 31/2/5/1, g–j 43/0/5/0, k–n 62/0/1/1, o 36/0/1/0;
  - the FAILs are f3e and f3f (both pre-existing);
  - the XFAILs are f's five, g4, g5×4, m3c and o6c.
- After the review fixes, only g/i/m (43/0/6) and o (36/0/1) were re-run
  (`~/deepc-validation/M7-review-fix/`).
- Doctests: math 26/26, scatter 121/121.
- Profile (2048×1080, 20 spp, K=16, 2 threads, 5 reps, interleaved): M7 wall median 42.57 s, CPU
  75.55 s, peak RSS 1.478 GB, with noise of 0.77 s / 1.78 s / 65 MB (`~/deepc-validation/M7-P4T1/`).
- No M8-T0 plugin set exists yet. P1.T1 captures it.

**The o6 rig** (`tests/nuke/scenes.py`: `MIX_*` ~5439–5474, `mixRig()` ~5512, o6/o6b/o6c
~6218–6275) contains:
- an opaque card at z=8.20 (R 0.90) and a fog card at z=8.18 (R 0.10), both over `MIX_BOX`
  (100,100,156,156);
- an opaque near card at z=7.90 (R 0.55) over `MIX_NEAR_BOX` (112,112,144,144);
- the receding ground plane.

Settings: complete deep, `fill: foreground`, manual CoC, size 64, focus 10, fog α ∈ {0.2, 0.5}, K ∈
{4, 16, 64}. Every layer shares the plane's G and B. The fog's colour is not premultiplied by its
alpha, so the stack alone flattens to G/A 0.99 (fog 0.2) or 0.825 (fog 0.5), and everything else
reads G/A 0.55. A pixel's **stack weight** is therefore `w = (G/A − 0.55) / (G/A_stack − 0.55)`. This
is the quantity mechanism (2) is about.

**Measured values (`o6c-mechanism/vals.log`, `probe_k4.log`, `probe_k16.log`, fog 0.2).**

| pixel | node K=4 | node K=16/64 | Bokeh | flat |
|---|---|---|---|---|
| (155,128), stack silhouette | 0.822 | 0.741 | 0.918 | 0.99 |
| (125,130), inside the near card | 0.775 | 0.575 | 0.552 | 0.55 |
| (128,128) | — | 0.5585 | 0.5500 | — |

At (155,128), K=16, the probe reads:
- near card bucket k=4: coverage 0.1052;
- stack bucket k=5: coverage 0.5225, with the near card's split share 0.105 co-located in it;
- plane: fit 0.372.

The stack's effective colour weight from G/A is **≈0.435**, not the 0.52 coverage the stub quotes.

**Consultant estimates, to be confirmed or refuted by P1.T2.** These are continuous thin-lens values
at the pixel centre. CoC is `64·|1 − 10/z|`: near card 17.01 px, stack 14.05 px (fog 14.24 px).
- **(125,130):** the near card covers 0.891 (the node's k=4 coverage reads 0.8909). A depth-ordered
  composite therefore gives G/A ≈ 0.891·0.55 + 0.109·0.99 ≈ **0.598**, where Bokeh reads 0.552.
  Bokeh shows no see-around past a defocused near card.
- **(128,128):** the near card covers ≈0.963 (the node reads 0.9622), so the depth-ordered G/A is
  ≈0.566.

  **Consequence:** fixing mechanism (1) correctly moves interior pixels **away** from Bokeh. For
  that reason Bokeh cannot be the acceptance oracle for mechanism (1).
- **(155,128):** the near card's lens set lies inside the stack's (both are in front of focus, so the
  offsets have the same sign). Thin lens then gives near ≈0.105, stack ≈0.418, plane ≈0.477. The
  node's coverage partition treats the two footprints as disjoint, which gives the stack its full
  0.5225 once mechanism (1) is fixed. So the reference may agree with **neither** the node nor Bokeh
  (0.84). That would be a third mechanism: correlated occlusion, which the coverage partition's
  new-area model does not represent.
- **Control:** a single card half a pixel inside its edge should read about 0.52 under both thin lens
  and the node.

**Why "per-tile boundaries" is more than a table swap (the stub understated this).**
- `DepthBuckets` is built once per frame in `frameSetup()` (`src/DeepCDefocus.cpp` ~1336–1360,
  `src/DeepCDefocusMath.h` ~900–1250). The design is that "bucket boundaries are global".
- Bucket assignment happens at **flatten time, per fragment**. `flattenPixelToSoA()` writes
  `bucketIndex0/1`, `bucketAlpha0/1` and `colorScale0/1` into the SoA
  (`src/DeepCDefocusScatter.cpp` ~239–245, ~589), and `scatterBandCPU()` (~1519) deposits wherever
  they point.
- Bands are full-width strips. Their height is `clamp(2·maxRadius, 32, 256)`, shrunk by
  `planBands()` under `memory_limit` (`src/DeepCDefocusScatter.h` ~3819–3900). A table per band
  would therefore make the output depend on the memory knob, and a full-width strip is not local
  enough to separate surfaces anyway.
- `bucketOf()` splits a point fragment between bucket **centres**. That split is what leaks the
  near card into the stack's bucket at K=16/64. New boundaries alone do not stop it: the split rule
  has to stop at gaps between surfaces.
- The split is also what prevents scene (g)'s banding ("mandatory for point fragments"), so it has
  to stay inside continuous depth ranges.
- Other users of the table: the pre-merge grouping key (`locateBoundary`), the volumetric span split
  (`splitSpanAtBoundaries` + `bucketOfContaining`), the fill's run bucketing
  (`src/DeepCDefocusFill.h` ~61), `makeUniformHoldoutBoundaries` (which takes its count and range
  from the buckets), the claim bins, and the probe's `zCentre` printout.

**Constraints from the board apply in full:**
- Nuke 16.0v9 only, built in `build/local-16.0`, configured with `-D Nuke_ROOT=/usr/local/Nuke16.0v9
  -D DEEPC_BUILD_TESTS=ON -D "DEEPC_DEFOCUS_ISA_FLAGS=-mavx;-mfma"`.
- Every build and Nuke run goes through `scripts/hostguard.sh` (`--mem-gb 5.5`/`6` for the harness;
  a–f needed `--floor-gb 1.0` at M7). Run the harness in batches: a–f, g–j, k–n, o.
- No 8-bit tolerances. Pins are float-exact or `N·2⁻²⁴` term-count bounds against an independent
  oracle, never against the new output.
- No hand-written SIMD. `DEEPC_HD` kernels must stay nvcc-clean. Planes use `PodBuffer<T>`. Tests
  use doctest.
- House style: 4-space indentation, `_` member prefix, lowerCamelCase. Follow the comment policy (no
  narrative, no plan or task IDs in code).
- Every new alpha assertion gets a colour:alpha assertion beside it. Every surviving XFAIL keeps a
  hard outer bound. Every new check is mutation-tested.
- Scratch mutation builds are never committed. Work happens on
  `claude/deep-defocus-node-plan-o0ld83`.

## Phase 8.1: Baseline, the independent reference, and the ruling

- [ ] M8.P1.T1 — Capture M8-T0: the pre-M8 plugin set and a full a–o reference at `2ac3550`
  - files: `~/deepc-baselines/M8-T0/` (new, outside the repo)
  - approach: from the clean tree at `2ac3550`, configure `build/local-16.0` with the board's line
    and build with `scripts/hostguard.sh -- cmake --build build/local-16.0 -j2`. Copy **every**
    plugin `.so` into `~/deepc-baselines/M8-T0/plugins/`. Write `PROVENANCE.txt` (commit, date, SDK,
    configure line, sha256 of each `.so`). Run both doctest binaries and keep their logs. Run the
    a–o harness in four batches with `DEEPC_PLUGIN_DIR=~/deepc-baselines/M8-T0/plugins`,
    `--threads=2` and `--out-dir ~/deepc-baselines/M8-T0/renders`, keeping scene (o)'s and (m)'s
    EXRs. This becomes M8's row reference, because PR #109's review fixes were only re-verified on
    g/i/m/o.
  - verify:
    - doctests 26/26 + 121/121;
    - a–o `PASS=172 FAIL=2 XFAIL=12 SKIP=2`, every row whitespace-equal to `~/deepc-validation/M7-P4T2/`
      (a–n) and `M7-review-fix/` (o), or else each difference reported and explained;
    - `DeepCDefocus.so` loads headless;
    - paths and sha256 reported for `## Decisions`.
  - size: M
  - depends: none

- [ ] M8.P1.T2 — Spike: an independent brute-force thin-lens reference of the o6 rig, compared with the node and Bokeh
  - files: none in the repo. The tool, its inputs, outputs and report go under
    `~/deepc-validation/M8-P1T2/`. The tool is a standalone C++17 source built with plain
    `g++ -O2 -fopenmp`: no NDK, and **no `src/` header included**. Deep dumps and the node/Bokeh EXRs
    come from headless Nuke.
  - approach:
    - **Input is the node's own input.** A Nuke script (run with
      `NUKE_PATH=build/local-16.0/src /usr/local/Nuke16.0v9/Nuke16.0 -t`, through hostguard) builds
      `mixRig(fogAlpha)` and the control rigs from `tests/nuke/scenes.py`'s helpers. It dumps every
      deep sample (x, y, zFront, zBack, rgba) with `node.deepSampleCount` / `node.deepSample` over
      `MIX_BOX` outset by the largest CoC + 2. The same script renders `DeepCDefocus` (K=4/16/64,
      from `~/deepc-baselines/M8-T0/plugins`) and `makeBokeh()` on the identical deep input.
    - **Reference model.** Treat each deep sample as its source pixel's footprint `[x,x+1)×[y,y+1)`
      at its depth. The signed CoC is `c(z) = size·(1 − focus/z)` in px. Take the lens model from
      the knob documentation / M1 design reference (Manual mode; `max_radius` inactive here) —
      re-derived, not copied from code.
    - **Rendering.** Output pixel `q`, sub-pixel `s` and lens point `u` (uniform on the unit disc)
      give the ray that meets depth z at image position `q + s − c(z)·u`. Walk the source pixels
      along that segment. Collect the samples whose own footprint contains the ray's position at
      their depth, and composite them front to back with `over`, box-filtered over the pixel.
      Stratify at ≥ 4×4 sub-pixel × ≥ 32×32 lens samples. Report the Monte Carlo standard error per
      pixel. It must be ≤ 2e-3 on G/A.
    - **Analytic cross-check.** At the five `vals.log` pixels, compute each card's continuous
      lens-set area in double (disc ∩ shifted rectangle, including the near ⊂ stack nesting). The
      Monte Carlo result must agree within its error.
    - **Calibration first.** An isolated opaque card over nothing, at the near-card and stack
      depths: compare node and reference alpha per pixel across its silhouette. This catches CoC
      sign, pixel-centre and kernel-convention mismatches before any o6 comparison. Stop and fix the
      reference if the curves differ by more than the Monte Carlo error away from the rim. **Report
      the node's residual at the anti-aliased rim** (that is the kernel's approximation class, and
      the ruling needs it).
    - **Rigs:**
      - (c1) a single opaque card with a distinct G/A over the plane (pure silhouette, no near
        card, no fog);
      - (c2) the o6 rig at fog 0.2 and 0.5;
      - (c3) the o6 rig without the fog card, as a control.
    - **Report:**
      - stack-weight (and near-card weight) profiles along y=128, x∈[136,172], and along x=128
        across `MIX_BOX`'s top edge, for reference / node K=4 / K=16 / K=64 / Bokeh;
      - a per-pixel table at the five `vals.log` pixels;
      - verdicts on the "What we know" estimates (0.598 at (125,130); 0.105/0.418/0.477 at
        (155,128); about 0.52 for c1);
      - for each mechanism, which renderer the reference sides with, or "neither".
  - verify:
    - the calibration curves overlay within the Monte Carlo error away from the rim, with the rim
      residual stated;
    - the analytic cross-check agrees within error at all five pixels;
    - `REPORT.md` holds the tables and profiles, plus the exact commands and the tool source;
    - the report answers three questions:
      - (a) c1's reference weight vs node (0.52) vs Bokeh (0.84);
      - (b) the o6 interior pixels' reference vs node vs Bokeh;
      - (c) whether nested footprints (near ⊂ stack) make the reference differ from the node's
        disjoint-area partition, and by how much.
  - size: L
  - depends: M8.P1.T1

- [ ] M8.P1.T3 — USER RULING: which renderer is the oracle for defocused see-around and silhouette weighting, and at what tolerance
  - files: none. The PM records the ruling in `## Decisions`, then finalises Phase 8.3 in place.
  - approach: the PM puts P1.T2's report to the user (profiles and the table, not prose) with these
    questions:
    - **Q1 — oracle.** Which is the oracle for o6c-class colour weighting:
      - (a) the thin-lens reference;
      - (b) Bokeh;
      - (c) per mechanism. For example: reference for c1-style silhouettes, while the
        nested-footprint case is recorded as a model limit.
    - **Q2 — tolerance class, if the reference is chosen.** The reference is Monte Carlo /
      continuous and the node rasterises an anti-aliased kernel, so a pure `N·2⁻²⁴` bound may be
      unattainable. Choose between:
      - (i) a derived bound: kernel rim residual from the calibration + Monte Carlo error at k σ,
        derived from geometry and never fitted — this needs an explicit exception to the
        no-8-bit/term-count rule;
      - (ii) an analytic double-precision reference restricted to axis-aligned cards, with the rim
        excluded, then `N·2⁻²⁴`;
      - (iii) report only: o6c stays XFAIL against Bokeh with the reference's verdict in its note.
    - **Q3 — node change, if the reference disagrees with the node's model.** Examples are nested
      footprints, or any see-around the coverage partition cannot represent. Choose:
      - fix inside M8 (new tasks);
      - a new milestone stub (e.g. M9, correlated occlusion in the coverage partition);
      - accept and document it.

    Phase 8.2 does not wait on this ruling.
  - verify: the answers are recorded verbatim in `## Decisions`; Phase 8.3's tasks are rewritten
    from the stub below to match (IDs kept, new ones appended); the board shows any new milestone
    stub the ruling creates.
  - size: S
  - depends: M8.P1.T2

## Phase 8.2: Mechanism (1) — per-tile adaptive depth buckets

Runs in parallel with P1.T2/T3. Its acceptance oracle does not depend on the ruling: the
**layer-ordered partition** composes single-layer renders of the node in true depth order. The
thin-lens and Bokeh comparisons are Phase 8.3's.

- [ ] M8.P2.T1 — Design and derivation: per-tile depth tables, the gap-aware split, and a go/no-go
  - files: design note `~/deepc-validation/M8-P2T1/DESIGN.md` plus scratch prototype patches (never
    committed). The PM copies the decision into `## Decisions` and rewrites P2.T2–T4 in place if the
    design departs from their briefs.
  - approach: a consultant reads the M1 design reference's depth-bucketing sections,
    `DepthBuckets` (`src/DeepCDefocusMath.h` ~880–1440), `flattenPixelToSoA` / `assignBucket` /
    `emitFragment`, `scatterBandCPU`, `planBands`, `frameSetup` / `computeBand`, the fill's bucket
    use, and the composite's contract block. They choose and derive:
    1. **Architecture.** Two options:
       - (X) **Per-tile flatten.** Job tiles on a fixed output grid, each with its own table built
         from its fetch window. The flatten and scatter kernels stay unchanged, and only the table
         source changes. Cost: x/y-pad re-flatten overhead, plus the engine moving from full-width
         bands to 2-D tiles.
       - (Y) **Per-destination-tile assignment at scatter time.** The flatten runs once per band;
         `bucketOf` and the deposit alphas are evaluated per (fragment, destination tile), with
         span splits at tile x-edges. The kernel source changes.

       Compare flatten/scatter cost on the profile rig, the effect on the M3 CUDA seam, and code
       churn.
    2. **Table construction.** One candidate: generalise `buildBoundedDeltaCoc`'s per-side
       proportional budget split to per-cluster. Clusters are occupied intervals of the tile's
       alpha-weighted histogram, separated by gaps measured in CoC px. Each cluster gets ≥ 1 bucket
       when K allows, with uniform ΔCoC inside a cluster and no buckets spent on gaps. For a
       continuous tile (scene (g)) this reduces to today's table over the tile's range. Derive the
       gap threshold. Plane rows step about 0.37 CoC px per scanline; o6's near card and stack are
       2.8 px apart.
    3. **A gap-aware split rule.** `bucketOf` interpolates between two centres only when both lie
       in the same cluster; across a gap the fragment is assigned whole. This removes the
       depth-split leak without bringing back scene (g)'s banding. Prove the partition of unity and
       the `over` reconstruction still hold.
    4. The volumetric span split and `bucketOfContaining` composition contract (e.g. tile boundaries
       ⊂ one global fine boundary set), the pre-merge grouping key, the fill's run bucketing, and the
       holdout boundary count and range.
    5. **Determinism.** Output must be bit-identical across `memory_limit`, band plan and thread
       count. The tile grid and tables must be pure functions of the frame and knobs, and every
       fetch window must cover every fragment that reaches its tile.
    6. **Seams** at tile edges, with a derived expectation. **Memory** (tables × tiles) and **cost**
       (the histogram pre-pass).
    7. **Prototype.** A scratch patch on the M8-T0 tree shows o6 K=4 (125,130) moving to the
       layer-ordered value, and scene (g) g1–g3 not regressing.

    **Go/no-go:** if no design meets items 3 and 5 with cost within about 10% on the profile rig, the
    note says so and recommends depth-moment planes (≈2× plane memory) instead. The PM takes that to
    the user before P2.T2.
  - verify: `DESIGN.md` gives a derivation for each of items 1–6. The prototype's o6 K=4/16/64
    readings at the five `vals.log` pixels are within 1e-3 of the layer-ordered values computed by
    hand from single-layer renders (a prototype sanity bar, not a gate). g1–g3 prototype readings sit
    inside their current bounds. The prototype profile Δ is stated. The recommendation is explicit
    (X, Y, or moment planes).
  - size: L
  - depends: M8.P1.T1

- [ ] M8.P2.T2 — Per-tile table builder and the gap-aware split in `DepthBuckets`, with math doctests
  - files: `src/DeepCDefocusMath.h` (`DepthBuckets`: new host-side per-tile builder, cluster and gap
    metadata in the fixed-size POD, gap-aware `bucketOf` / `locateBoundary` / `bucketOfContaining`),
    `tests/test_defocus_math.cpp`
  - approach: implement P2.T1's items 2–4 exactly as the note derives them:
    - Host-side builder (double internally, as `buildBoundedDeltaCoc`) from a tile's alpha-weighted
      depth histogram. Storage stays fixed-size and trivially copyable. Lookups stay `DEEPC_HD`: no
      `std::`, no heap.
    - `buildBoundedDeltaCoc` is kept unchanged for the frame-level uses the note keeps (e.g. holdout
      count and range).
    - Doctests against independent oracles:
      - (a) cluster detection on synthetic histograms (two spikes, spike + ramp, pure ramp, one
        depth, K < clusters), checked against a brute-force enumeration written in the test;
      - (b) the pure-ramp case reproduces `buildBoundedDeltaCoc` over the same range (bit-exact, or
        within the note's stated ulps);
      - (c) the gap-aware split's partition of unity is exact (`a0 ⊕ a1 == a` under `over`, as the
        existing test at ~964) and never crosses a gap;
      - (d) the bounded-ΔCoC property holds inside each cluster;
      - (e) the post-conditions of the existing `buildBoundedDeltaCoc` tests (~865–1065) hold per
        cluster;
      - (f) degenerate inputs: empty tile, NaN/inf depths, K=4 with more clusters than buckets.
    - **Mutation:** on a scratch copy, make `bucketOf` split across gaps. (c) must fail; nothing
      else may.
  - verify: builds clean; math doctests `26+n` all pass; the scratch mutation fails exactly (c);
    `nvcc`-cleanliness grep (no `std::` / heap / host calls in the `DEEPC_HD` lookups); the scatter
    doctests are still 121/121 (no caller changed yet).
  - size: M
  - depends: M8.P2.T1

- [ ] M8.P2.T3 — Feed per-tile tables through flatten and scatter, with scatter doctests against the layer-ordered oracle
  - files: `src/DeepCDefocusScatter.h`, `src/DeepCDefocusScatter.cpp` (`flattenPixelToSoA` /
    `assignBucket` ~239–600, `scatterBandCPU` ~1519, `checkCompositionContract` ~1168),
    `src/DeepCDefocusFill.h` (~42–81), `tests/test_defocus_scatter.cpp`
  - approach: follow P2.T1's chosen architecture:
    - (X): the SoA flatten takes the job tile's table; nothing else in the kernel changes.
    - (Y): the deposit setup moves per (fragment, destination tile); spans split at tile x-edges;
      the SoA keeps depth and alpha.

    The additive planes and atomic splat are unchanged. The composite is untouched.
    `checkCompositionContract` checks against the table actually used. Doctests, each against an
    independent oracle:
    - (a) two opaque point fragments in one source pixel, at depths the global K=4 table puts in one
      bucket: the per-tile path composites them in depth order. Alpha is `== 1.0f` and colour:alpha
      equals the front fragment's, bit-exactly where only one term contributes.
    - (b) an o6-shaped miniature (near disc partly over an opaque stack, over a far field) through
      `scatterBandCPU` → `resolveBandCPU` at K ∈ {4, 16, 64}. Colour:alpha per pixel equals the
      layer-ordered partition computed in double from each layer rasterised **alone** (reuse the
      independent-rasterisation helper at ~6439). The bound is `N·2⁻²⁴` with `N` counted from the
      contributing taps, as `oTolerance` does. Before the change this reads the mixed value (record
      it).
    - (c) a continuous ramp tile gives output bit-identical to the global table built over the same
      range, or within the note's derived ulps.
    - (d) a volumetric span crossing a cluster gap still reconstructs its parent exactly (the
      composition-contract test at ~1249 extended).
    - (e) the existing deposit-invariant (~6576) and independent-rasterisation (~6439) tests pass
      unchanged.

    **Mutation:** force every tile onto the global table. (a) and (b) must fail. Never committed.
  - verify: builds clean; scatter doctests `121+n` pass and math still passes; the mutation fails
    (a)/(b); a grep of the touched `DEEPC_HD` bodies finds no `std::`/heap/`getenv`; scenes a, c
    and d run on this build are byte-identical to M8-T0 (size-0 parity, energy, empties black).
  - size: L
  - depends: M8.P2.T2

- [ ] M8.P2.T4 — Node wiring: tile grid and per-tile table pass in the engine, probe printout, determinism
  - files: `src/DeepCDefocus.cpp` (`frameSetup` ~1336, `computeBand` / `BandJob`, `planBands` call
    ~1651–1700, `computeDepthRange` ~1760, `printProbe` ~1979–2030), `src/DeepCDefocusScatter.h`
    (`planBands` / `BandPlan` ~3819–3900, only if the job grid changes)
  - approach:
    - Build the per-tile tables where P2.T1 put them: per job tile under (X), or per band for its
      destination tiles under (Y).
    - Size the fetch windows so every fragment that can reach a tile is in the tile's histogram.
    - Make the tile grid independent of `memory_limit` and thread count. `memory_limit` may then cap
      only concurrency, or (X) job size in a way that cannot change which tables are built. State
      the policy if a single tile exceeds the limit.
    - The probe prints the destination tile, its cluster list and per-bucket `zCentre` from the
      tile's table. It stays zero-cost when unset.
    - Update the pipeline comments (`DeepCDefocus.cpp` ~15–24, ~1336–1360), following the comment
      policy.
  - verify:
    - builds clean and both doctest suites pass;
    - **determinism:** o6's fog 0.2 K=16 render and scene (g)'s K=16 ramp are bit-identical across
      `memory_limit` 4 GB vs the smallest value that still renders (forcing a different band plan,
      confirmed in the probe or log), and across `--threads 1` vs `2`;
    - `DEEPC_DEFOCUS_DEBUG_PROBE="125,130"` at K=4 fog 0.2 shows the near card and the stack in
      different buckets with no near share in the stack's bucket; saved to
      `~/deepc-validation/M8-P2T4/`;
    - `--scenes o` readings recorded. o6c's pins are expected to move and read "moved/DIP GONE"
      (re-pinned in P2.T5, the precedent being M7.P2.T1).
  - size: M
  - depends: M8.P2.T3

- [ ] M8.P2.T5 — Scene (o): an o6d gate against the layer-ordered oracle, determinism rows, o6c re-pinned
  - files: `tests/nuke/scenes.py` (`MIX_*`, `O6_PIN_COLOUR` ~5474, the o6 block ~6218–6275, a new
    helper that renders single layers), `tests/nuke/generate_scene_scripts.py` (StickyNote),
    `tests/nuke/scene_o_solid_alpha.nk` (regenerated)
  - approach:
    - **o6d** (the mechanism-(1) gate, all six K × fog cells, over `MIX_BOX`):
      - render the near card, the stack (fog + opaque together) and the plane each **alone** through
        the node at the same K;
      - compose them in true depth order with the coverage partition's opaque-layer rule, in double
        in the harness: `fit_j = min(α_j, free)`, colour `c_j/α_j · fit_j`;
      - first validate that algebra on pixels where no two layers reach each other (it must match
        the full render within bound there), then gate G/A and B/A per pixel;
      - the bound is the sum of the four renders' `oTolerance` terms (a difference carries every
        render's rounding, as i7's reachability arm).
    - **o6dα** beside it: an alpha arm.
    - **o7** (determinism): the P2.T4 bit-identity checks as harness rows (0 ulps across two
      `memory_limit` values and across 1 vs 2 threads) on the o6 rig and scene (g)'s ramp. Only the
      thread-count arm is available if the harness can't set `--threads` per render. Record which.
    - **o6c** (still against Bokeh): re-pin the six cells at their new readings, with the note
      rewritten. Mechanism (1) is gone. The residual is see-around/silhouette weighting, pending
      P1.T3. Keep the two-sided XFAIL with a hard outer bound of 2× pin.
    - **Mutation-test every new row:**
      - o6d/o6dα must FAIL on `~/deepc-baselines/M8-T0/plugins` (the K=4 cell at about 0.2 from the
        mixing);
      - o7 must FAIL under a scratch build whose tile grid follows the band plan;
      - record every reading. No vacuous gates: a row no mutation moves is dropped and the drop
        noted.
    - Regenerate only the o `.nk` (revert the other 14 if regeneration touches them, as at M7).
  - verify:
    - `--scenes o` reads `PASS=36+R FAIL=0 XFAIL=1 SKIP=0`, where R is the new rows (stated);
    - the 34 pre-o6 rows are byte-identical to M8-T0, or else each move is listed for P2.T6;
    - o6/o6b are still PASS;
    - every mutation run is logged under `~/deepc-validation/M8-P2T5/`;
    - the regenerated `.nk` loads headless.
  - size: M
  - depends: M8.P2.T4

- [ ] M8.P2.T6 — Full a–o run on the mechanism-(1) build: every moved row explained, re-pinned where M8 moves it
  - files: `tests/nuke/scenes.py` (only rows that move past their gate or whose pinned value moves;
    expect g1–g5 and i7-family candidates), `tests/nuke/generate_scene_scripts.py` / `.nk` only if a
    scene's StickyNote carries a moved number
  - approach:
    - Run the a–o harness in the four batches with `--threads=2` and renders kept.
    - Diff every row against M8-T0 (whitespace-normalised).
    - Explain each changed row by probing its worst pixel. Expected causes: the per-tile table over a
      tile's narrower range (finer ΔCoC on ramps), or the gap-aware split.
    - A row that moves but stays within its gate keeps its gate; the reading goes in the report.
    - A row gated at a pinned value, or failing, is re-pinned per the project rule: a term-count bound
      against an independent oracle, never the new output.
    - Any XFAIL that moves is re-pinned in this commit.
    - A row that regresses (e.g. scene (g) banding) **stops the task** and goes back to the PM. Do
      not re-pin a regression.
    - `exrdiff.py` scene (m)'s kept EXRs against `~/deepc-baselines/M8-T0/renders`.
  - verify:
    - a–o tally stated as `PASS=172+R FAIL=2 XFAIL=12 SKIP=2` (R from P2.T5), or with every
      difference named;
    - FAILs are f3e/f3f only;
    - a bit-exact; d empties black; m4a/m4b/n1/n7 PASS;
    - every changed row is in a table (row, T0 reading, M8 reading, gate, probed cause), saved to
      `~/deepc-validation/M8-P2T6/`;
    - re-pinned rows PASS on the new build and are shown to FAIL under a recorded mutation;
    - EXR ulp figures explained.
  - size: M
  - depends: M8.P2.T5

## Phase 8.3: Mechanism (2) — re-oracle o6c as ruled (stub, finalised after M8.P1.T3)

> The PM rewrites this phase in place from the P1.T3 ruling (IDs kept, new ones appended) before
> starting it. The branches below are the expected shapes, not briefs to execute as written.

- [ ] M8.P3.T1 — (only if Q1 names the reference, in whole or in part) Land the reference as a harness oracle
  - files: `tests/reference/` (new: the P1.T2 tool, cleaned up and built by CMake under
    `DEEPC_BUILD_TESTS`, still including no `src/` header) or a pure-Python analytic model in
    `tests/nuke/scenes.py`, per Q2; `CMakeLists.txt` / `tests/CMakeLists.txt`;
    `tests/nuke/harness.py` (invocation)
  - approach: Q2(i) is the Monte Carlo tool with a fixed seed and stratification, whose bound is
    derived from its error and the calibration rim residual. Q2(ii) is the analytic
    double-precision disc∩rectangle model for axis-aligned cards, rim excluded, bound `N·2⁻²⁴`.
    Either way the oracle consumes the same deep dump as the node. It is checked on the calibration
    card and c1 against the P1.T2 numbers. **Mutation:** perturb its CoC sign or scale by one ulp
    of the CoC formula, and the calibration check must fail.
  - verify: the tool builds in the local build and in docker; it reproduces P1.T2's tables to its
    stated error; its runtime per scene (o) run is stated.
  - size: L
  - depends: M8.P1.T3

- [ ] M8.P3.T2 — Re-oracle or re-pin o6c per the ruling
  - files: `tests/nuke/scenes.py` (o6c, `O6_PIN_COLOUR`, possibly new o6e/o6f rows for c1-style
    silhouettes), `tests/nuke/generate_scene_scripts.py`, `tests/nuke/scene_o_solid_alpha.nk`
  - approach, by ruling:
    - **Reference ruled oracle, and it agrees with the node:** o6c becomes node vs reference
      (PASS at the ruled bound). A Bokeh-comparison row is kept as a reported reading, documenting
      Bokeh's divergence.
    - **The reference disagrees with the node** (e.g. nested footprints): cells where they agree
      PASS against the reference. The rest stay a two-sided XFAIL against the reference, pinned
      with a hard bound and naming the model limit. Any node change goes to the milestone Q3 names.
    - **Bokeh ruled oracle:** o6c stays XFAIL against Bokeh (re-pinned in P2.T5). Its note records
      the reference's verdict. A node change is scoped per Q3.

    Mutation-test every PASS row: it must fail on `~/deepc-baselines/M8-T0/plugins` or under a
    named scratch mutation.
  - verify: `--scenes o` tally stated; every other o row is identical to P2.T6's log; mutation runs
    are logged under `~/deepc-validation/M8-P3T2/`; the `.nk` loads headless.
  - size: M
  - depends: M8.P1.T3, M8.P2.T6, M8.P3.T1 (if it exists)

## Phase 8.4: Gate

- [ ] M8.P4.T1 — Profile against M8-T0 under the same conditions as M7.P4.T1
  - files: none. Evidence goes under `~/deepc-validation/M8-P4T1/`.
  - approach: interleave T0, M8, T0, M8. Each run is
    `DEEPC_NUKE=/usr/local/Nuke16.0v9/Nuke16.0 DEEPC_PLUGIN_DIR=<dir> scripts/hostguard.sh --mem-gb 6 -- tests/nuke/run_profile.sh --reps 5 --threads 2 --stats`
    (2048×1080, 20 spp, K=16), with `<dir>` = `~/deepc-baselines/M8-T0/plugins` then
    `build/local-16.0/src`. "Within noise" means:
    - |median(M8) − median(T0)| ≤ the larger same-`.so` median spread, or M8 is faster;
    - RSS is no more than T0's plus the same spread.

    Report the histogram/table pass and the re-flatten overhead separately if the probe or timers
    allow.
  - verify: four runs logged; wall/CPU medians, RSS and the noise figure reported for `## Decisions`
    beside M7's 42.57 s. If not within noise, the measured cost goes to the user before P4.T2
    (accept or optimise).
  - size: S
  - depends: M8.P2.T6

- [ ] M8.P4.T2 — Clean rebuild, doctests, full a–o, the docker gate, and the PR
  - files: none. This task only verifies; any fix lands in the task that owns the code.
  - approach:
    - Remove `build/local-16.0` and reconfigure with the board's line.
    - Run both doctest binaries.
    - Run the a–o harness in four batches (`--mem-gb 5.5`, `--floor-gb 1.0` for a–f if the host
      needs it, `--threads=2`, renders kept under `~/deepc-validation/M8-P4T2/renders`).
    - Diff every row against M8-T0. The moved rows must all be on P2.T6's and P3.T2's explained
      lists.
    - `exrdiff.py` scene (m)'s EXRs against M8-T0.
    - Run `./docker-build.sh --linux --nuke-sdk /usr/local/Nuke16.0v9` with nothing heavy alongside.
    - Open the PR from `claude/deep-defocus-node-plan-o0ld83` to `master`.
  - verify:
    - doctests `26+n` + `121+m` (restate the counts from the task reports);
    - a–o tally `PASS=172+R+Q FAIL=2 XFAIL=12+X SKIP=2`, with R from P2.T5, and Q/X from P3.T2
      (restate the exact numbers before checking);
    - FAILs are f3e/f3f only; surviving a–n XFAILs are byte-identical with hard bounds;
    - a bit-exact; d black; m4a/m4b/n1/n7 PASS;
    - o6/o6b/o6d/o7 PASS, and o6c as P3.T2 left it;
    - docker: EXIT=0, 28 plugins including `DeepCDefocus.so`, 0 errors, 0 `DeepCDefocus*` warnings,
      SKIP for 16.1/17.0, and the release `.so` loads headless (no render — AVX2 build).
  - size: M
  - depends: M8.P3.T2, M8.P4.T1

**Verification gate:** all of the following, then the PR to `master` from
`claude/deep-defocus-node-plan-o0ld83`.
- **Reference and ruling:** P1.T2's independent thin-lens reference exists with its calibration,
  analytic cross-check and report. The user's P1.T3 ruling is recorded.
- **Doctests:** both suites green, including the per-tile table and gap-aware split cases and the
  scatter cases against the layer-ordered partition oracle. Each is mutation-tested.
- **Determinism:** output is bit-identical across `memory_limit` and thread count (o7).
- **Scene (o):**
  - o6d (node vs the layer-ordered partition of single-layer renders) PASS at term-count bounds on
    all six K × fog cells, and FAIL on M8-T0;
  - o6c re-oracled or re-pinned exactly as ruled.
- **Full a–o suite:** tally as restated in P4.T2, with every moved row explained against M8-T0.
  FAILs are f3e/f3f only. Surviving XFAILs keep their hard bounds. Scene (a) is bit-exact, (d) is
  black, and m4a/m4b/n1/n7 hold.
- **Profile:** within noise of M8-T0 on this host, or the cost is measured and accepted by the user.
- **Docker:** `./docker-build.sh --linux --nuke-sdk /usr/local/Nuke16.0v9` is green.

## Decisions
