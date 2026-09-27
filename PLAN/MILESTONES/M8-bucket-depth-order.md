# Milestone 8: Depth-ordered colour within a bucket, the silhouette-edge oracle, and nested footprints

> **Scope extended 2026-09-26 (user ruling):** M9 (nested footprints — correlated occlusion in the coverage
> partition) is folded into this milestone as **Phase 8.5**, run before the Phase 8.4 gate. M8 no longer ships the
> silhouette band or the volumetric rim as M9 XFAILs; it fixes the nesting term itself. See `## Decisions`.

Scene (o)'s o6c (M7.P3.T5) pins a colour defect that predates M7. On the mixed-opacity rig, colour:alpha
is up to 0.28 off Bokeh while alpha is exact. There are two mechanisms (see the M7 file's
`## Decisions`, M7.P3.T5 entry; evidence in `~/deepc-validation/M7-P3T5/` and its `o6c-mechanism/`
subdirectory):

- **(1) A near surface and the surface it hides share one depth bucket.** At K=4 they share it
  outright. At K=16/64 they share it through the near card's fractional depth-split share. The
  bucket's alpha is summed and saturated as one, so the two surfaces mix by alpha (0.89 : 1.0) instead
  of in depth order. This is a node defect. **The fix is the depth-ordered streaming composite
  designed in P2.T7** (2026-09-24): the band's fragments are sorted by depth and composited per pixel
  as they arrive, so buckets, and everything that quantises depth, go away. Two earlier candidates are
  rejected and closed: per-tile adaptive bucket boundaries (P2.T1's design; rejected by the user for its
  tile-aligned seams) and per-bucket depth-moment planes (rejected for their two-depths-per-bucket
  limit). See `## Decisions`.
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

- [x] M8.P1.T1 — Capture M8-T0: the pre-M8 plugin set and a full a–o reference at `2ac3550`
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

- [x] M8.P1.T2 — Spike: an independent brute-force thin-lens reference of the o6 rig, compared with the node and Bokeh
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

- [x] M8.P1.T3 — USER RULING: which renderer is the oracle for defocused see-around and silhouette weighting, and at what tolerance
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

    Phase 8.2's design and implementation (T1–T4) do not wait on this ruling. Its acceptance does:
    P2.T5 checks the fix against whatever oracle this ruling picks (user ruling 2026-09-24).
  - verify: the answers are recorded verbatim in `## Decisions`; Phase 8.3's tasks are rewritten
    from the stub below to match (IDs kept, new ones appended); the board shows any new milestone
    stub the ruling creates.
  - size: S
  - depends: M8.P1.T2

## Phase 8.2: Mechanism (1) — the depth-ordered streaming composite

> **Redirected 2026-09-24, designed 2026-09-25.** P2.T1's per-tile tables were rejected (seams). P2.T7 designed and
> prototyped the replacement: a depth-ordered streaming composite with **no buckets**, whose per-deposit rule is the
> **amended two-recency-chunk rule** of `~/deepc-validation/M8-P2T7/DESIGN.md` §2.4 (the rule as first briefed fails
> the two-fog-layers identity under defocus, §2.2). Verdict: **go**. P2.T2–T6 below are rewritten from that note;
> P2.T8–T10 are appended. **Every implementer reads DESIGN.md's relevant sections before touching code**, and may
> consult the prototype patch `~/deepc-validation/M8-P2T7/proto-stream.patch` (against `2ac3550`) — a scratch
> prototype, not a drop-in: production code is written to the briefs, with tests, and to house style.
>
> **Execution order:** P2.T2 → P2.T9 → P2.T3 → P2.T4 → P2.T8 → P2.T5 → P2.T6 (→ P2.T10 only if ruled in).
> P2.T5, P2.T6 and P2.T8's V1 arm wait on user rulings (Q2 tolerance class; the silhouette band; the volumetric rim —
> see the board's `# Open questions`). P2.T2/T9/T3/T4 do not.

Design and implementation run in parallel with P1.T3. **Acceptance (T5) uses the thin-lens reference** (user ruling
2026-09-24) at the tolerance class Q2 rules. The **layer-ordered partition** (single-layer node renders composed in
true depth order, "LO(P)") stays as a sanity bar and a secondary row.

- [x] M8.P2.T1 — Design and derivation: per-tile depth tables, the gap-aware split, and a go/no-go
  - files: design note `~/deepc-validation/M8-P2T1/DESIGN.md` plus scratch prototype patches (never committed).
  - approach: (rejected design — see `## Decisions` 2026-09-24; §1 and §4 of its note remain the reference for
    what the flatten depends on and the bucket table's other users.)
  - verify: done; superseded by P2.T7.
  - size: L
  - depends: M8.P1.T1

- [x] M8.P2.T7 — Design and derivation v2: the depth-ordered streaming composite (no buckets), with a go/no-go prototype
  - files: `~/deepc-validation/M8-P2T7/DESIGN.md` (items 1–11 + brief corrections), `proto-stream.patch`,
    `plugins/DeepCDefocus.so` (prototype build), `o6/`, `harness/`, `vol/`, `determinism/`, `profile/`, `parity/`.
  - approach: as briefed (items 1–11); outcome in `## Decisions` 2026-09-25.
  - verify: done — go on the amended rule; every item-4 identity holds; 0 ulps across band plans and threads;
    profile −32.5 % wall vs M8-T0.
  - size: L
  - depends: M8.P1.T1

- [x] M8.P2.T2 — The per-deposit body and the stream primitives (math/kernel side), with doctests against analytic oracles
  - files: `src/DeepCDefocusScatter.h` (new: `StreamPlanes`/`StreamPlaneView`, `orderedDepthKey`,
    `depositStreamSpanRecency` — the amended rule of DESIGN §2.4 — `volumetricPieceStepPx`,
    `volumetricPieceBounds`), `src/DeepCDefocusMath.h` (a `FrameDepthRange {depthMin, depthMax, K}` replaces
    `DepthBuckets` everywhere except as the holdout set's source; `bucketOf`, `locateBoundary`,
    `bucketOfContaining`, `fragmentDeposit`, `splitSpanAtBoundaries`, `splitPartCount`, `saturationScale`,
    `BucketWeight`/`BucketDeposit` deleted; `partitionAlpha`/`partitionColorScale` kept for pieces),
    `tests/test_defocus_math.cpp`, `tests/test_defocus_scatter.cpp`
  - approach: read DESIGN §1, §2 (all of it — §2.2 is why the naive rule is wrong, §2.4 is the rule to implement),
    §5.1 and §7 first. The deposit body is a `DEEPC_HD` span function over per-pixel state `(Q, A, uO, sO, cLast,
    C[c])` with two divisions, selected (branch-free per pixel), never branched per pixel; rotation is lazy plus the
    1 px CoC-jump rule exactly as §2.4 derives it. No κ, no previous-radius field (DESIGN §5.2, decision (i)).
    Doctests drive the body on hand-built deposit sequences, never through the old composite. This task may leave
    the old bucket callers compiling against shims only if deleting them outright would break the build before P2.T3;
    say which in the report. Every alpha assertion gets a colour:alpha twin.
  - verify: builds clean; doctests, each against an analytic oracle written in the test, each with a colour:alpha
    twin: (1) a tiling of n deposits with `Σw = 1`, α = 1 → `A == 1.0f` exactly, n ∈ {1, 7, 1000}; (2) two
    full-coverage 0.5 layers each split into n tiling deposits → 0.75 within `4n·2⁻²⁴`, and the briefed (naive)
    rule kept in the test as a **mutation** fails by `exp(−0.5)` vs 0.5 (the test can see DESIGN §2.2); (3) an
    opaque tiled layer behind a partly covering 0.8 layer → `A == 1` within term count (the CoC-jump rotation's
    case; **mutation:** drop the jump → fails); (4) straddle: `wA 0.5 αA 1` then `wB 1.0 αB 0.5` → 0.75 exactly
    (today +8.3 %); (5) a single sharp deposit → `A == α`, `C == c` bit-exact; (6) `orderedDepthKey` monotone over
    ±0, subnormals, negatives, 1e12; (7) `volumetricPieceBounds`: transmittance shares sum to 1 within `n·2⁻²⁴`,
    the focal plane is a cut, count ≤ K+1, ΔCoC per piece ≤ step. Math 26/26 → the rewritten count stated; the
    scatter suite rebuilt and green (count stated). nvcc-cleanliness grep of every `DEEPC_HD` body (no `std::`,
    heap, `getenv`).
  - size: M
  - depends: M8.P2.T7

- [x] M8.P2.T9 — Holdout boundary decoupling: pass the frame's `HoldoutBoundaries` explicitly
  - files: `src/DeepCDefocusScatter.h`, `src/DeepCDefocusScatter.cpp`, `src/DeepCDefocus.cpp`
  - approach: `FlattenParams` carries the frame's `HoldoutBoundaries`, built once in `frameSetup()` from
    `FrameDepthRange` (uniform in z, count from `depth_layers`; DESIGN §6); `holdoutBracketOf` stops deriving it
    from a bucket object. `makeUniformHoldoutBoundaries` keeps its contract. No behaviour change intended.
  - verify: builds clean; both doctest suites green; harness `--scenes f` and `--scenes a`: f1/f2 and a4 rows
    byte-identical to M8-T0 (if P2.T2 already moved a row, say which and why).
  - size: S
  - depends: M8.P2.T2

- [x] M8.P2.T3 — Flatten, sort, scatter, resolve: the streaming pipeline replaces the bucket composite, with doctests
  - files: `src/DeepCDefocusScatter.cpp`, `src/DeepCDefocusScatter.h`, `tests/test_defocus_scatter.cpp`
  - approach: read DESIGN §1, §3, §5, §6 (pre-merge and `checkCompositionContract`), §7 and §8 first. The SoA drops
    the six bucket fields (41 B/fragment at C = 3). Flatten: tidy + sort unchanged; points are one fragment; spans
    cut by `volumetricPieceBounds` (step `max(2·merge_tolerance, frameCocVariation/K)`), shares pooled on the deepest
    piece; pre-merge keyed on radius tolerance + holdout bracket only; collision merge on kernel bin + side of focus
    + bracket; **both merges composite back to front** (`C = c + (1 − α)C`, DeepToImage's own order — DESIGN §4,
    size-0 parity). `sortFragmentsByDepth`: stable LSD radix on the ordered key, constant bytes skipped.
    `scatterStreamCPU`: blend the two bracketing kernel rows into one scratch row **before** the rule (two passes
    are not equivalent under a non-linear rule), fold `vis` in, deposit `arrival` from the raw row, call the body.
    `resolveStreamCPU` = fill + premultiplied clamp. `scatterBackgroundCPU` writes an arrival pointer. **Delete**
    `scatterBandCPU`, `resolveBandCPU`, `compositePixelCoveragePartition*`, the trace structs, `BucketPlanes`,
    `visitBucket`/`claimNewArea`/frontier, and every shim P2.T2 left. `checkCompositionContract` rewritten (finite,
    alpha in [0,1], radius ≥ 0) plus `checkStreamOrder`. Update the scatter header's contract block ("THE BUCKET
    COMPOSITE" → the stream) following the comment policy. The node (`DeepCDefocus.cpp`) may be minimally adapted
    to compile; its real wiring is P2.T4.
  - verify: builds clean; doctests (each with a colour:alpha twin where alpha is asserted): (1) band-plan invariance
    — one SoA scattered as 1, 2, 7 and 37-row bands gives bitwise the same pixels; (2) size-0 corpus (900 px ×
    2..20 spp, points + spans, pre_merge on/off) bit-exact against a back-to-front flatten oracle in the test; (3) a
    full-coverage volumetric parent reconstructs its alpha and colour within term count at any K; (4) holdout law
    `A == vis` for an opaque fragment; (5) `checkStreamOrder` accepts the sort and rejects a swapped pair
    (**mutation**); (6) the scratch-row blend equals a single pass at the blended radius within `2·2⁻²⁴` per weight.
    The existing deposit-invariant and independent-rasterisation tests are kept or replaced with a stated reason.
    A bench on the 4K/20 spp corpus reports sort time and deposit time separately. Suite counts stated. nvcc grep
    clean. `grep -n DepthBuckets src/ tests/` lists only the holdout source (or nothing).
  - size: L
  - depends: M8.P2.T9

- [x] M8.P2.T4 — Node wiring, probe, memory budget, determinism, and the profile
  - files: `src/DeepCDefocus.cpp`, `src/DeepCDefocusFill.h`, `src/DeepCDefocusScatter.h` (`bandBudgetBytes`)
  - approach: read DESIGN §6, §7, §8 first. `frameSetup()` builds `FrameDepthRange`, the frame's
    `HoldoutBoundaries` (P2.T9) and `pieceStepPx`; the `depth_layers` tooltip is rewritten (holdout depth resolution
    and the cap on pieces per volumetric span; no role in point colour). `BandJob` owns `StreamPlanes`, the order
    array and the sort scratch. `computeBand()`: flatten → holdout LUT → sort → scatter → background → resolve.
    Budget: `W·B·(C+6)·4` + SoA at the measured resident bytes per fragment (re-measure; 41 logical + 20 sort);
    `planBands` is free to pick any band height and job size must never depend on anything that changes output.
    The probe (`DEEPC_DEFOCUS_DEBUG_PROBE`) prints the pixel's deposit stream in order (depth, w, α, fit/excess,
    chunk state; DESIGN §6), zero-cost when unset. `stagedRadiusPx` re-derived from the new cut without buckets.
    Pipeline comments (`DeepCDefocus.cpp` ~15–24 and the frameSetup block) updated per the comment policy.
  - verify: build clean (`-Wall -Wextra`; `-fopt-info-vec` shows the deposit loop vectorised, or the miss and the
    escalation-ladder step taken); harness `--scenes a`: a1/a2/a3 **0.000e+00** with the gate tightened to 0, a4
    ≤ 2e-7, a5 PASS (its expectedFailure dropped); **determinism:** o6 fog 0.2 K = 16 and a volumetric rig at forced
    band heights {1, 7, 32, 96} (via `memory_limit` values that actually move `planBands()`, or a scratch-only
    switch that is never committed) and at `--threads 1` vs `2`: **0 differing channel-pixels**, logged under
    `~/deepc-validation/M8-P2T4/`; probe at `125,130` K=4 fog 0.2 shows the near card deposited before the stack
    with `F` exhausted; `--scenes o` readings recorded (o6c expected to move); **profile** interleaved against M8-T0
    (T0, M8, T0, M8; 2048×1080, 20 spp, K=16, 5 reps, 2 threads, `run_profile.sh --stats` under hostguard
    `--mem-gb 6`): within noise or the delta stated beside the prototype's wall −32.5 % / CPU −36 % / RSS −0.29 GB.
  - size: M
  - depends: M8.P2.T3

- [x] M8.P2.T8 — Thin-lens volumetric oracle: the reference extended to spans, with harness rigs V1–V3
  - files: `tests/reference/vref.cpp` (promoted from `~/deepc-validation/M8-P2T7/vol/vref.cpp`; standalone C++17,
    built by CMake under `DEEPC_BUILD_TESTS`, **no `src/` header included**), the top-level `CMakeLists.txt` (the doctest
    targets live there, ~51–65; there is no `tests/CMakeLists.txt`),
    `tests/nuke/scenes.py` (rigs V1–V3 as scene (f) or (o) rows — state which), `tests/nuke/harness.py` (loader)
  - approach: exact per-ray path length through a uniform box (image position affine in 1/z), 16 × 1024 rays per
    pixel, standard error stated; V1 a lone 0.8 fog card (CoC 5–20 px), V2 the same over an opaque card, V3
    straddling focus. The V2/V3 rows gate alpha and colour:alpha against the oracle; the V1 rim row is written as a
    reading (PASS/XFAIL decided by the user's rim ruling — leave it XFAIL with a hard outer bound from the oracle
    until ruled). Mutation: flip the tool's CoC sign → V2 fails.
  - verify: the tool builds locally and in docker; V2/V3 alpha within 2e-3 of the oracle (prototype 1.8e-3, T0
    0.094); V1 reading stated (prototype +0.148 / +10.9 %, T0 −0.125 / −4.3 %); rows mutation-tested against
    `~/deepc-baselines/M8-T0/plugins` (V2/V3 must FAIL there); `.nk` regenerated only for the touched scene and
    loads headless.
  - size: M
  - depends: M8.P2.T4 (V1's PASS/XFAIL status additionally on the user's rim ruling)

- [x] M8.P2.T5 — Scene (o): o6d against the layer-ordered partition, K-invariance, interior vs the thin-lens reference, o6c re-oracled
  - files: `tests/nuke/scenes.py` (`MIX_*`, `O6_PIN_COLOUR`, the o6 block, a single-layer render helper),
    `tests/nuke/harness.py` (reference-map loader if needed), `tests/nuke/generate_scene_scripts.py` (StickyNote),
    `tests/nuke/scene_o_solid_alpha.nk` (regenerated)
  - approach: **o6d**: node vs its own single-layer renders composited near → stack → plane (LO(P)) in double in the
    harness, all six K × fog cells over `MIX_BOX`, no pixel excluded (the prototype meets (104,104) too); bound is
    the sum of the renders' `oTolerance` terms. **o6dα** beside it. **o7 (K-invariance):** the o6 cell at K = 4, 16,
    64 bit-identical (0 ulps). **o8 (determinism):** P2.T4's band-plan/thread bit-identity as harness rows where the
    harness can set them; record which arm is available. **o6e (interior vs reference):** over `MIX_NEAR_BOX` inset
    2 against the P1.T2 reference map, at **Q2(i) as ruled 2026-09-26: bound = the kernel's calibrated rim residual +
    3 × the reference's Monte Carlo SE** (P1.T2: residual ≤ 8.6e-4, SE ≤ 2.2e-4 → ≈ 1.5e-3), plus the row's
    term-count `N·2⁻²⁴`. Prototype max 6e-4 / mean 1e-4 at fog 0.2, 3e-4 at 0.5 — about 2.5× margin. If the committed node
    exceeds it, **do not widen the bound**: first cut the reference's SE (more samples, fixed seed), and if the node
    still exceeds the bound, stop and report to the user. **o6f (silhouette band) moves to Phase 8.5** (the nesting fix):
    it is gated there against the reference at the same bound, not XFAILed here. **o6c** re-oracled to the reference
    (Bokeh comparison kept as a reported reading); pixels in the silhouette band are left to Phase 8.5.
    Mutation-test every row: o6d/o6dα must FAIL on `~/deepc-baselines/M8-T0/plugins`; o7 must FAIL under a scratch
    build with a K-dependent term; o6e under the scratch naive-rule build. No vacuous gates. Regenerate only the o
    `.nk`.
  - verify: `--scenes o` reads `PASS=36+R FAIL=0 XFAIL=X SKIP=0` with R and X stated; the pre-o6 rows byte-identical
    to M8-T0 or each move listed for P2.T6; mutation runs logged under `~/deepc-validation/M8-P2T5/`; the `.nk`
    loads headless.
  - size: M
  - depends: M8.P2.T4, M8.P1.T3 (Q2 ruled 2026-09-26)

- [x] M8.P2.T6 — Full a–o on the streaming build: every moved row explained and re-pinned from an independent oracle
  - files: `tests/nuke/scenes.py`, `tests/nuke/harness.py`, `tests/nuke/generate_scene_scripts.py` / `.nk` only
    where a StickyNote carries a moved number
  - approach: run a–f, g–j, k–n, o in batches (`--threads=2`, renders kept). Diff every row against M8-T0
    (whitespace-normalised). Re-pin every row whose mechanism was a bucket term, each from an independent oracle:
    g4/g5 from the kernel's adjoint sum computed from `DiscKernelLUT` directly (`α + (S−1)α(1−α)`, S = 1.0712 on
    the g rig; prototype 0.90647 vs predicted 0.90641); f3c/f3d back to plain PASS where they meet 1e-5 or a
    term-count arrival bound (f3c is 4.9e-5 off from arrival accumulation: derive its taps like f3b's); a5 PASS;
    f3e/f3f/f3h/f3h2/f3i (the volumetric rim, DESIGN §5): **not re-pinned here** — the user deferred the rim to
    Phase 8.5 (2026-09-26); record their readings in the table as "→ P5" and re-read them after Phase 8.5 lands.
    If the nesting fix does not bring them inside P2.T8/P2.T12's oracle bounds, they go back to the user — no XFAIL
    without a ruling. Scene (i)/(m)/(n) rows re-read (fill interplay) before any is
    re-pinned. A regression with no bucket-term explanation **stops the task** and goes back to the PM.
    `exrdiff.py` scene (m)'s EXRs against M8-T0.
  - verify: tally stated per batch and in total as `PASS=… FAIL=… XFAIL=… SKIP=2`; every FAIL either fixed or a
    user-ruled XFAIL with a hard bound; f3e/f3f accounted for; a bit-exact; d empties black; m4a/m4b/n1/n7 PASS;
    every changed row in a table (row, T0 reading, M8 reading, gate, probed cause) under
    `~/deepc-validation/M8-P2T6/`; re-pinned rows PASS on the new build and FAIL under a recorded mutation; no pin
    derived from the new output.
  - size: L
  - depends: M8.P2.T5, M8.P2.T8, M8.P2.T12

- [ ] ~~M8.P2.T10 — (only if the user rules the volumetric rim over-read out) Per-parent chain for volumetric pieces~~
  **Cancelled 2026-09-26:** the user rejected the premise — a deep image stores no "one fog body" identity, so a
  chain keyed on adjacency cannot know two pieces belong together. The rim is deferred to Phase 8.5's general
  nesting fix. Kept for history.
  - files: `src/DeepCDefocusScatter.h`, `src/DeepCDefocusScatter.cpp`, `tests/test_defocus_scatter.cpp`
  - approach: DESIGN §5.4 — an open-chain slot per pixel keyed by (source pixel, sample), 12 B/pixel/slot, so
    consecutive pieces of one body from neighbouring source pixels stop being treated as disjoint. Doctests on a
    hand-built volumetric deposit sequence against the analytic path-length oracle.
  - verify: V1 within P2.T8's oracle SE band; f3h/f3h2 back to PASS; V2/V3 unchanged; profile delta vs P2.T4 stated.
  - size: L
  - depends: M8.P2.T8, user ruling

- [x] M8.P2.T11 — Pre-merge must not group pieces from opposite sides of focus
  - files: `src/DeepCDefocusScatter.cpp` (pre-merge grouping ~386–430, `setDepthDerived(sampleMidDepth(...))`),
    `src/DeepCDefocusScatter.h` (pre-merge contract comment ~442–445), `tests/test_defocus_scatter.cpp`
  - approach: the pre-merge keys on radius tolerance + holdout bracket; across focus |CoC| folds, so a piece at z 9.49–10
    (r 0.522) and one at 10–10.61 (r 0.588) fall within `merge_tolerance` and are drawn at the union's depth midpoint 10.049,
    r 0.097 — the sharp path, a delta with α 0.226 at focus (consultant model vs node −1.073e-2 / −1.028e-2 at (95,128) on V3;
    `pre_merge` off halves the ring deficit). Add **side of focus** to the pre-merge key, matching the collision merge's
    `sameLensPatch` key, so a group never spans the focal cut; the group's derived depth then maps to a radius between its
    members'. Fix the two comments that claim this cannot happen (pieces "only regroup on the max_radius plateau"; the group
    "rasterises at its front member's radius"). No other behaviour change.
  - verify: builds clean; doctest: a span straddling focus, cut by `volumetricPieceBounds`, pre-merged with tolerance 0.25 →
    the pieces either side of the focal cut are never in one group and the group radius equals a member's (or lies between
    members') radius; **mutation:** drop the sign key → the straddling pair merges and the test fails; both suites green
    (counts); harness `--scenes a` bit-exact (a1–a3 0, a4 ≤ 2e-7, a5 PASS); `--scenes f` with `vref`: V3 (f4c/f4cr) ring
    deficit at (95,128) moves from −1.075e-2 toward ≈ −4.4e-3 and every other f row byte-identical to `d927a07` or listed.
  - size: M
  - depends: M8.P2.T8

- [x] M8.P2.T12 — f4 rows: a radius-dependent kernel term replaces ε·(1+τ); the provisional V3 figure goes
  - files: `tests/reference/vref.cpp` (a kernel-spec coverage mode), `tests/nuke/scenes.py` (f4a–f4cr bounds,
    `VOL_V3_NEAR_FOCUS_DEFICIT` removed), `tests/nuke/harness.py` if the loader needs a second output
  - approach: P1.T2's ε = 8.6e-4 is the kernel's error at r = 14–17 px; the consultant's table (`eps_table.txt` in its scratch,
    reproduced in `## Decisions`) shows ε_K(r) = 2.0e-1 at r ≤ 0.5, 4.4e-2 at (1,2], 9.3e-3 at (4,6], 3.1e-3 at (8,10], 3.8e-4
    at 14 — V1/V2 pieces run r 5–20 and V3's near-focus slices are sub-pixel, so a constant ε is wrong on every f4 row.
    **Preferred:** a per-pixel `K(p) = Σᵢ αᵢ·|c_spec,i(p) − c̄_true,i(p)|` in share units, with the pieces cut as the node
    documents (step `max(2·merge_tolerance, CoC variation/K)`, each at its midpoint radius), `c_spec` the kernel **spec**
    reimplemented in `vref` independently of `src/` (anti-aliased disc with a 1 px ramp, normalised, delta at r ≤ 0.5, bracket
    blend on the documented grid) and `c̄_true` exact ray coverage; gate low = shareRef − SE term − K(p), high = ceiling + K(p).
    **Fallback** if the mode is disproportionate: the rigorous scalar `Σᵢ αᵢ·sup_p|Δcᵢ|` (5.33e-2 on V3; the geometry-only
    `∫σ·ε_K(r(z))dz + ε_K(r_card)` = 3.18e-2 under-counts midpoint quantisation by ~60 % and is not enough). Derivation to cite:
    `|Π(1−αᵢaᵢ) − Π(1−αᵢbᵢ)| ≤ Σαᵢ|aᵢ−bᵢ|`, and the free-area-first rule reads ≥ the product model. State in the row note the
    one unproven assumption (truth ≤ F, the free-area composite of exact piece coverages; measured ≥ +1.6e-3 at every probed
    pixel; per-ray slack ≤ τ_max²/4 = 1.4e-2). The `min(1, E[τ])` ceiling stays (legitimate once the kernel term is right).
    The tolerance is no longer divided by the 0.3 colour contrast in a way that hides share-unit deficits — gate in share units.
  - verify: `vref` builds; the K(p) mode reproduces the consultant's 3.29e-2 at (96,96) and 1.89e-2 at (95,128) on V3 (or the
    scalar is stated); f4a–f4cr re-run on the P2.T11 build: readings vs the new bounds per rig (V3 low side within bound with no
    provisional figure), V1/V2/V3 colour XFAIL ceilings unchanged; mutation: halved-CoC → V1/V2c/V3c FAIL, M8-T0 → V1/V2/V3 FAIL;
    every other f row identical; `.nk` loads if regenerated.
  - size: L
  - depends: M8.P2.T11

## Phase 8.3: Mechanism (2) — re-oracle o6c as ruled (stub, finalised after M8.P1.T3)

> **P1.T3 fully ruled (2026-09-26):** Q1 — the thin-lens reference is the oracle; Q2 — **(i)**, a derived bound of
> the kernel's rim residual + 3 × the reference's Monte Carlo SE; Q3 — nested footprints are fixed **in this milestone** (Phase 8.5), not in M9.
> So P3.T1 is the Monte Carlo tool (the Q2(ii) analytic model is not built), and P3.T2 takes the "reference ruled
> oracle" branch, with nested-footprint pixels gated after Phase 8.5 rather than XFAILed. The PM still finalises
> these briefs in place (IDs kept) before starting.

- [x] M8.P3.T1 — Land the P1.T2 thin-lens reference as a harness oracle (Q2(i): Monte Carlo, fixed seed)
  - files: `tests/reference/thinlens_ref.cpp` (new: promoted from `~/deepc-validation/M8-P1T2/tool/thinlens_ref.cpp`,
    cleaned to house style and the comment policy, still including no `src/` header), the top-level `CMakeLists.txt`
    (a `thinlens_ref` target beside `vref` under `DEEPC_BUILD_TESTS`, same OpenMP handling), `tests/nuke/harness.py`
    (a `findThinlensRef()` / loader mirroring `findVref()`/`_vrefLines`, SKIP with reason if absent, plus a deep-dump
    writer from a Nuke deep node in the tool's input format — port `deepexr_dump.py`'s logic if needed)
  - approach: keep the tool's `mc` mode (stratified sub-pixel × concentric-map lens strata, `reps` replicates, SE =
    sd(replicate means)/√reps, per-pixel seeded so output is thread-count independent); keep `analytic` mode as the
    cross-check. The harness writes the node's own deep input as a dump, runs the tool on a pixel set, and returns
    per-pixel RGBA + SE. Settle the risers default (P1.T2 caveat: riser walls between plane rows moved G/A ≤ 0.012 at
    two pixels) — state which the harness uses and why. No new scene rows here (P2.T5 part 2 adds o6e; P3.T2 re-oracles
    o6c) — but add one calibration row in scene (o) (an isolated opaque card at z 7.90 over nothing, node alpha vs
    reference off the rim, bound = rim residual 8.6e-4 + 3·SE + term count per the 2026-09-26 ruling) so the oracle is
    exercised in CI of the harness. **Mutation:** scale the tool's CoC by (1 + 1e-3) or flip the pixel-centre
    convention → the calibration row FAILs.
  - verify: `thinlens_ref` builds via CMake locally (and compiles in isolation with plain `g++ -std=c++17`); on the
    P1.T2 dumps it reproduces `~/deepc-validation/M8-P1T2/out/*_ref_five.txt` within stated SE at the five pixels and
    the calibration map's rim residual; the harness loader runs it on a freshly dumped o6 fog-0.2 rig and matches
    P1.T2's map within SE; the calibration row PASSes on HEAD and FAILs under the mutation; runtime per invocation
    stated; `--scenes o` tally stated (prior rows identical).
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
  - depends: M8.P1.T3, M8.P2.T6, M8.P3.T1, and M8.P5 for the nested-footprint pixels

## Phase 8.5: Mechanism (3) — nested footprints (folded in from M9, 2026-09-26; stub — elaborate before starting)

> Runs after Phase 8.2/8.3's reference oracle is in the harness and before Phase 8.4. The PM elaborates it in
> place at phase start (scout + consultant), appending tasks after P5.T1. Intent carried over from the retired
> M9 stub (`M9-nested-footprints.md`, kept for history):
>
> When a near surface's lens set lies inside a farther surface's, the disjoint-area coverage partition
> over-weights the farther surface by the near surface's lens area. Known instances, each with its oracle:
> - **o6 silhouette band (o6f):** whole-map max |ΔG/A| 0.1148 / mean 0.0368 vs the thin-lens reference (T0 0.0934 /
>   0.0396); P1.T2 points (155,128) +0.105, (104,104) +0.041, (150,150) +0.059.
> - **Same-source-pixel fog + opaque stack past the pre-merge tolerance (o6g/o6h):** 1 − a up to 9.07e-2.
> - **Volumetric rim (f4a/f4br/f4cr, f3e/f3f/f3h/f3h2/f3i):** a lone 0.8 fog card over-reads +0.150 alpha / +10.9 %
>   at its defocused rim; colour over an opaque card +4.0e-2 / +1.43e-2 c:a (the latter mostly sub-pixel kernel
>   error per the P2.T8 consultant — not expected to move).
>
> - **Order-dependent fill at a defocused edge (n8b symmetry / thin-lens rows, added by P2.T6's follow-up):** equal-depth
>   background samples take the free area in raster/sort order rather than by lens position — the same root cause as the
>   nesting term (the scalar coverage state does not know *which* part of the lens is covered), but it gets the *identity* of
>   what shows through wrong rather than the amount. x↔y transpose asymmetry 0.163 (T0 5.4e-7); band mean |dR| vs thin-lens
>   0.042 (T0 0.0305). **Design requirement:** the fix must be independent of emission/sort order among samples that do not
>   occlude one another (a depth tie-break alone is insufficient — a 0.01·y tilt restores the error in full). Evidence
>   `~/deepc-validation/M8-P2T6-consult/n8b/`.
>
> **User constraint (2026-09-26):** a deep image does not record that two samples belong to "one fog body", so
> no fix may assume that identity (this is why P2.T10's per-parent chain was cancelled). The fix works from what a
> deep sample actually carries — depth range, alpha, position — and the rim is expected, not promised, to
> follow from the general fix. Whatever the rim rows read afterwards goes back to the user if outside its oracle.

- [ ] M8.P5.T1 — Design the nested-footprint correction and predict every affected row
  - files: none in the repo; design note and prototype under `~/deepc-validation/M8-P5T1/`
  - approach: consultant-led (opus). Start from P1.T2's REPORT and, if it survived, P2.T7's `DESIGN.md` §5 (see
    the 2026-09-26 evidence decision). Characterise the over-weight as a function of the two lens sets; propose a
    correction to the streaming composite's per-deposit rule that needs no body identity, stays K-invariant and
    deterministic (o7/o8), and fits the memory budget. Prototype it as a scratch patch and read o6f, o6g/o6h,
    f4a/f4b/f4c and f3e–f3i against their oracles.
  - verify: note states the rule, its identities (the two-fog-layers identity and o6d must still hold), predicted
    vs prototype readings for every row above, memory/profile delta, and a go/no-go. A no-go goes to the user.
  - size: L
  - depends: M8.P2.T5, M8.P3.T1

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
  - depends: M8.P2.T6, Phase 8.5's last task

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
  - depends: M8.P3.T2, M8.P4.T1, Phase 8.5

**Verification gate:** all of the following, then the PR to `master` from
`claude/deep-defocus-node-plan-o0ld83`.
- **Reference and ruling:** P1.T2's independent thin-lens reference exists with its calibration,
  analytic cross-check and report. The user's P1.T3 ruling is recorded.
- **Doctests:** both suites green, including the depth-ordered stream's cases (the identities of
  P2.T7 item 4) and the scatter cases against the layer-ordered partition oracle. Each is
  mutation-tested.
- **Determinism:** output is bit-identical across `memory_limit` and thread count (o7).
- **Scene (o):**
  - o6d (node vs the layer-ordered partition of single-layer renders) PASS at term-count bounds on
    all six K × fog cells, and FAIL on M8-T0;
  - o6c re-oracled or re-pinned exactly as ruled;
  - o6e (interior) PASS at rim residual + 3 × the reference SE; o6f (silhouette band) and o6g/o6h PASS against the reference after
    Phase 8.5, or a user ruling recorded.
- **Nested footprints (Phase 8.5):** every row in Phase 8.5's list read against its oracle; the volumetric rim rows
  either inside their bounds or explicitly ruled by the user.
- **Full a–o suite:** tally as restated in P4.T2, with every moved row explained against M8-T0.
  FAILs are f3e/f3f only. Surviving XFAILs keep their hard bounds. Scene (a) is bit-exact, (d) is
  black, and m4a/m4b/n1/n7 hold.
- **Profile:** within noise of M8-T0 on this host, or the cost is measured and accepted by the user.
- **Docker:** `./docker-build.sh --linux --nuke-sdk /usr/local/Nuke16.0v9` is green.

## Decisions

- 2026-09-24 — **User ruling: mechanism (1) acceptance waits for the independent reference.** Offered o6d (node vs its own single-layer renders composited in depth order) as a ruling-independent oracle, since the fix is predicted to move the node away from Bokeh (G/A ≈ 0.598 vs Bokeh 0.552 at (125,130) K=4). The user chose "Wait for the reference" over accepting o6d and over keeping Bokeh. Consequence: P2.T1–T4 still run in parallel with the spike; P2.T5 now depends on P1.T3, and o6d's oracle is whatever P1.T3 picks (o6d may still be kept as a secondary self-consistency row if the ruling agrees).
- 2026-09-24 — **M8.P1.T1 done (evidence only):** M8-T0 at `2ac3550` in `~/deepc-baselines/M8-T0/` (PROVENANCE.txt sha256 `8ca9142b…`, DeepCDefocus.so `282130b6…`). Doctests 26/26 + 121/121; a–o `PASS=172 FAIL=2 XFAIL=12 SKIP=2`, every row whitespace-identical to M7-P4T2 (a–n) and M7-review-fix (o). The a–f batch and the build needed `--floor-gb 1.0` and a retry because of memory pressure from elsewhere on the shared host.
- 2026-09-24 — **M8.P1.T2 done (evidence only)**, `~/deepc-validation/M8-P1T2/REPORT.md`. Standalone thin-lens reference (no `src/` include). Calibration passes: node alpha vs the exact analytic value is ≤7e-4 off the rim; rim residual max 8.6e-4 (z=7.90) / 7.3e-4 (z=8.20). Bokeh is off by up to 0.029 (its fStop mapping blurs too wide). Fog-0.2 G/A, reference / node K=16 / Bokeh: (128,128) 0.567/0.559/0.550; (155,128) 0.734/0.741/0.918; (125,130) 0.598/0.575/0.552; (104,104) 0.743/0.722/0.767; (150,150) 0.760/0.764/0.806. c1 silhouette: the reference (0.5226) matches the node (0.523); Bokeh reads 1.0, with no see-around. Verdicts: mech (1) — neither (the fix moves the node toward the reference); mech (2) — the node's silhouette weighting is right; new mech (3), nested footprints — the disjoint partition over-weights the stack by the near card's lens area (+0.105 at (155,128)), so fixing (1) alone moves that pixel away from the reference. Caveats: the reference adds riser walls between plane rows (moves G/A ≤0.012 at two pixels); blur sign is untested (both cards sit in front of focus).
- 2026-09-24 — **User ruling (M8.P1.T3, partial).** Q1: the **thin-lens reference** is the oracle for o6c-class colour weighting and for the mechanism-(1) gate (P2.T5). Q3: nested footprints (mechanism 3) become a **new milestone, M9** — M8 records them as a known limit and keeps them out of its gate. Q2 (tolerance class against the reference) is **still open**: the user asked for more information before ruling. P1.T3 stays open until Q2 is answered; P2.T5 and Phase 8.3 wait on it.
- 2026-09-24 — **M8.P2.T1 done (design; evidence `~/deepc-validation/M8-P2T1/DESIGN.md`, `proto-X.patch`).** Recommendation: **X, sparse**. Only the 32 px tiles whose reachable-fragment depth histogram shows a gap get their own table and rerun the table-dependent half of the flatten; every other tile takes today's path, bit-identical to T0. Go on items 3 and 5. Prototype: worst |ΔG/A| vs layer-ordered ≤7.8e-4 at (128,128), (155,128), (125,130) and (150,150), down from 2e-1 at T0. (104,104) fails in every variant: a ground-plane surface between the depths makes layer-ordered invalid there. g1–g5 read identical to T0. Profile +5.6% sparse (all of it in the unoptimised serial histogram pass), +10.6% dense. Brief corrections still to apply to P2.T2–T5: drop Y; split the flatten at the tidy/sort line; pass the frame's holdout boundary set explicitly (`holdoutBracketOf` derives it from `buckets`); ramp-tile bit-identity holds only for sparse; job size must never depend on `memory_limit`, and determinism means 0 ulps across `memory_limit` and 1 vs 2 threads; P2.T2 adds a test that a 0.2 px cluster keeps its own bucket beside a wide one at K=64; o6d excludes pixels like (104,104). **Open for the user:** tile-aligned seams as large as the correction (0.17 G/A at K=4, 0.08 at K=16/64 on o6) wherever a floor-like surface reaches only part of a tile row. Accept them, or go to moment planes?
- 2026-09-24 — **User ruling: the per-tile design is rejected, and so is the moment-plane fallback.**
  Given P2.T1's seam table (steps of 0.18 G/A at K=4 and 0.08 at K=16/64 along tile edges, wherever a
  floor bridges two surfaces across part of a tile row), the user ruled the design "not moving in the
  right direction" and asked for a replacement with **no seams and no limit on depths per bucket**.
  Moment planes fail the second test. P2.T1 is closed as a rejected design; its §1 and §4 (what the
  flatten depends on; the bucket table's other users) remain the reference for the coupling. P2.T2–T6
  are void as written and are rewritten from P2.T7. Sparse-vs-dense is moot.
- 2026-09-24 — **Replacement direction: the depth-ordered streaming composite (P2.T7).** Sort each band's
  fragments by depth and apply the coverage partition's fit/excess rule per fragment on per-pixel
  running state, deleting buckets outright. Rationale: the per-pixel state is continuous across the
  frame (no seams by construction), the ordering is exact for any number of surfaces (no depth limit),
  size-0 `DeepToImage` parity is exact by construction, and it removes the fractional split, the
  saturation and the head-tile stack that exist only to compensate for bucket pooling. M1 rejected
  full sorting because holdout visibility made it unnecessary, not because it was infeasible: sorting a
  band's SoA is a permutation of a list that already exists, not the per-destination fragment lists M1
  ruled out on memory. Risks carried into P2.T7: the per-deposit arithmetic is roughly three times
  today's flop count on a memory-bound loop (measured, not assumed, against M8-T0); volumetric parents
  need a co-location rule derived from lens geometry; M3's CUDA seam becomes tile-parallel with
  per-tile sorted lists rather than fragment-parallel with atomics. Nested footprints (M9) are not
  addressed by depth order and stay in M9.
- 2026-09-25 — **M8.P2.T7 done (design + prototype; `~/deepc-validation/M8-P2T7/DESIGN.md`, `proto-stream.patch`). GO on
  the amended rule; NO-GO on the rule as briefed.** The briefed per-deposit rule attenuates each later deposit by the
  *pooled* claimed transmittance, which already holds earlier deposits of the same surface, so a defocused layer of
  alpha `a` transmits ≈`e^−a` instead of `1−a` (f3b −2.79 %, opaque-behind-fog alpha 0.712 vs 1.0); exact only at
  size 0. The amendment (§2.4) splits the claimed share into two recency chunks (least / most recently covered),
  covers free area → older → newer, and rotates when the older chunk is exhausted or a deposit arrives > 1 px of CoC
  behind the previous one; +3 floats per pixel, no K. Two cheaper fixes measured and rejected (coverage-count classes;
  recency without the CoC-jump rule). Under it every item-4 identity holds: a1–a3 bit-exact 0.000e+00 (needs the
  pre-merge to composite back to front, DeepToImage's order, matched on 65 536/65 536 px); g1 Σ = 1 exactly; f3
  0.7500000, f3b +0.005 %, f3c +0.006 % (T0 −0.111 %), f3d +0.001 % (T0 −1.670 %); holdout f1 4.4e-8, a4 1.19e-7
  (gate 2e-7); colour:alpha exact. o6: bit-identical across K; the four mechanism-(1) pixels within 7.7e-4 of the
  layer-ordered values; interior vs reference 6e-4 (T0 0.0756). Whole-map |ΔG/A| fog 0.2 max/mean 0.1148/0.0368
  (T0 K=16 0.0934/0.0396), the higher max being M9's nesting term now undiluted. g1–g3 0.000e+00; g4 0.906473 and
  g5 +5–7 % now flat in K (explained by the kernel's own over-delivery S = 1.0712 to 1.5e-4; re-pin from the LUT).
  Determinism 0/262 144 channel-pixels at forced band heights 1/7/96 and `-m 1` vs `2`. Profile: wall 31.67 s vs
  46.93 s (−32.5 %), CPU −36 %, RSS 1.20–1.25 vs 1.44–1.54 GB (pre-merge groups more once the bucket key is gone:
  14.4 M vs 23.1 M fragments). Memory per band 2048×32, C=3: 2.4 MB vs 25.4 MB (K=16) / 101 MB (K=64). Volumetric
  parents: cut every `max(2·merge_tolerance, frame CoC range / depth_layers)` px of CoC, independent pieces (i);
  the exact overlap κ(d) was implemented and is indistinguishable (within 0.1 point), forced full overlap breaks o6.
  **Two user rulings requested, neither blocking the go** (board `# Open questions`): the silhouette band now carries
  M9's nesting term undiluted (max 0.1148); isolated fog volumes over-read at their defocused rim (+0.148 alpha /
  +10.9 % on a lone 0.8 fog card, where T0 under-reads −0.125 / −4.3 %) — this drives f3h/f3h2 to −23.4 % (T0 f3h2
  PASS) and f3i to 1.2e-5 vs its 1e-5 gate; accept as an M9-class bounded XFAIL, or add a per-parent chain (P2.T10).
  M3 consequence: a device build bins the sorted stream into per-destination-tile lists (CUB sort by tile then depth)
  and runs one block per tile walking its list in order, one thread per pixel; the `DEEPC_HD` body stays a pure
  function of (state, deposit); M3's brief must replace "one thread per fragment with atomics" and budget for
  fragments duplicated across the tiles their disc reaches. **PM decision:** the amended rule is an engineering
  correction inside the user's direction (no seams, no depth limit, buckets gone), so implementation proceeds on it
  without a further ruling; P2.T2–T6 rewritten in place from the note, P2.T8–T10 appended.
- 2026-09-25 — **M8.P2.T2 done (code `1d6924f`).** `depositStreamSpanRecency` (§2.4 rule, branch-free selects, lazy +
  1 px CoC-jump rotation), `orderedDepthKey`, `StreamPlanes` (W·B·(C+6)·4, arrival included), `FrameDepthRange`,
  `volumetricPieceStepPx`/`volumetricPieceBounds` (cuts uniform in clamped CoC; a max-radius plateau is one piece;
  step floor 2·max(tol, 0.125) px; writes `VolumetricPiece` records so P2.T3 swaps it for `splitSpanAtBoundaries`).
  Bucket math removed from Math.h; the old lookups moved verbatim into `src/DeepCDefocusBucketShim.h` (bit-identical)
  with call sites in Scatter.cpp/Fill.h/DeepCDefocus.cpp/test_defocus_scatter.cpp switched to free-function form —
  **P2.T3 deletes the shim and those call sites.** Doctests math 26→21 (7 bucket tests gone, 2 added), scatter 121→129.
  Mutations: drop the jump → test 3 fails by 0.048; wrong key → test 6 fails; no focal cut → test 7 fails; drop the
  lazy rotation → **equivalent mutant** (with uO = 0 the pN branch reproduces rotate-then-cover; kept because §2.4
  specifies it). The body takes a whole-fragment channel count; chromatic channel groups are P2.T3's. First hostguard
  build was killed at 137 by outside memory pressure; `--floor-gb 1.0` rerun passed.
- 2026-09-25 — **M8.P2.T9 done (code `f7e127c`), scene f verification outstanding.** `FlattenParams::holdoutBoundaries`
  is built once in `frameSetup()` (`makeUniformHoldoutBoundaries(buckets)`, contract unchanged) and is the single object
  the flatten and the holdout LUT read; `holdoutBracketOf(params, depth)` no longer takes `DepthBuckets` or the scratch
  cache (removed from `FlattenScratch`); `FrameShared::holdoutBoundaries` consolidated into it. One scatter SUBCASE
  rewritten to assert the new invariant (bracket follows the explicit input, not the `DepthBuckets` also handed in).
  Doctests 21/21, 129/129. Harness a: `PASS=5`, a1–a5 identical to M8-T0 (a4 0.000e+00 ≤ 2e-7). **Harness f could not
  be run**: the host (outside this container) sits at ~21/23 GiB with swap full; hostguard's pressure watchdog killed
  20 implementer attempts and Claude Code stopped the PM's own attempt for critical memory. The change is data plumbing
  of a per-frame value, so f1/f2 bit-identity is expected but **unmeasured**; P2.T4's determinism runs and P2.T6's
  full a–o cover it. The shim include in Scatter.h remains only for `assignBucket` (P2.T3 deletes it).
- 2026-09-25 — **M8.P2.T3 done (code `6d83f25`).** `sortFragmentsByDepth` (stable LSD radix), `checkStreamOrder`,
  `scatterStreamCPU` (bracket rows blended into one scratch row before the rule), `resolveStreamCPU`, `sameLensPatch`
  collision key; flatten takes no `DepthBuckets`, cuts spans with `volumetricPieceBounds`, pre-merge keyed on radius
  tolerance + holdout bracket, every merged run composited once back to front. Deleted: `scatterBandCPU`,
  `resolveBandCPU`, `compositePixelCoveragePartition*`, `BucketPlanes`, the head-tile/frontier/claim machinery and
  `DeepCDefocusBucketShim.h` with every call site. `DepthBuckets` survives only as the holdout source
  (`Math.h`, `computeDepthRange`'s `sanitizeDepth`). SoA 37 B/fragment (the brief's 41 included the dropped
  previous-radius field). Doctests math 21/21; scatter 129 → 101 + 1 skipped bench (≈50 bucket-composite cases deleted,
  the rest adapted); all seven verify items mutation-tested and caught (`~/deepc-validation/M8-P2T3/mutations.log`).
  Bench (4096×64 band, 20 spp, 1.33 M fragments, 22.5 M deposits): flatten 1825 ms, sort 71 ms, deposit 2736 ms.
  Harness a/c/d all PASS: a1/a3 → 0 (bit-exact), a4 0 → 1.192e-7 (gate 2e-7, the bracket-split case DESIGN §4
  predicts), c1 → 0, c2b/c3/c4 improved, d unchanged. Decisions: all-sharp spans are one piece (else size-0 parity
  breaks); chromatic channel groups deposit at group 0's radius through one alpha state (documented in code).
  **Flagged for a consultant:** verify item 3 (full-coverage volumetric parent) cannot hold at pure term count — when
  pieces are < 1 px CoC apart the jump rotation never fires and a layer can exhaust the older chunk one deposit early
  in float, leaving ≤ one kernel peak weight read pooled by the next layer: 1.96e-4 at K=8, 3.26e-4 at K=16/64 at
  α 0.8; the test pins alpha at term count plus an analytic bound from the LUT (Σ a_{k+1}·w_max,k·T_{k−1}·a_k below
  the jump threshold), colour:alpha at pure term count. Also flagged: pre-existing `1/255` figures in
  `test_defocus_scatter.cpp` (~4736–4748, ~7614–7635) contradict the 2026-09-18 no-8-bit rule. Left for P2.T4:
  tooltips, the probe stream printout, the budget doc/`kSoAResidentBytesPerFragment`/sort scratch, holdout set from
  `FrameDepthRange`, determinism, profile, vectorisation check.
- 2026-09-25 — **Consultant ruling on P2.T3's volumetric residual: a flaw in the rule, not rounding; fix the body.**
  (Scratch evidence `~/deepc-validation/M8-P2T3-consult/`.) At a layer boundary the §2.4 rule is discontinuous in the
  layer's weight sum S: exactly O's area → 0 error; slightly below → O(ε); slightly above by any amount → a fixed
  −3.28e-4 (K=16) / −4.29e-4 (K=8, α 0.8), because a deposit that spills into N files its whole footprint as "recent"
  (`O = uN − pN`) and the next piece, with no CoC jump, re-reads that area pooled instead of tiling it; it cascades to
  2.9× the peak weight and reaches 177× term count (2.7e-3) on a size-3 / zb-9 / α-0.8 rig. Not f3b's term (a 0.5/0.5
  flat field reads ≤1.5e-6 either way). Seven candidates measured; **adopted: relabel on reach-N and drop the lazy
  rotation term** — `uO/sO` rotate on the jump only, and on a deposit that reaches N the state becomes
  `uO = qNew − pN`, `sO = (qNew − aNew) − pN·tN·(1−α)`. Residual 6.6e-7 / 1.43e-6, continuous (0.27·ε), ≤0.05 of
  term count over an 85-rig sweep; every P2.T2 identity, band-plan invariance, threading and size-0 parity still hold;
  receding ramps bit-identical to the current body; selects only, no new state, vectorises as before. Dropping the
  lazy term is safe because with uO == 0 it produced the same x and O′ as the reach-N path. **Test rulings:** the
  LUT-derived `flipBound` is not legitimate (assumes one peak weight per boundary where the trace shows 2.9×; passes
  only because it is loose, 1.1e-3 vs 3.3e-4) — the volumetric test asserts pure `deposits·2⁻²⁴` on alpha and
  colour:alpha (the unfixed body fails it at α 0.8; add a size-3/zb-9 rig). The two pre-existing `1/255` sites are
  replaced: adjacent-kernel-bin trough vs a double-normalised oracle from the same float taps at `(Sa+Sb)·2⁻²⁴` plus an
  exact pin of `kernelGridRadius` (Δ(1/r) = 1/512 by construction); the α-0.9 ramp vs a per-pixel oracle
  `A = min(fill ? Araw/S : Araw, 1)`, `Araw = a·min(S,1) + a(1−a)·max(S−1,0)`, fill decided on the measured float
  arrival, at `2·N_p·2⁻²⁴`, arrival vs S likewise, and the snap-vs-blend control as `maxS_snap−1 > 2·(maxS_blend−1)`.
  **Also found:** the deposit loop does **not** auto-vectorise under the shipped flags (GCC 12.2: "control flow in
  loop" with `-ffp-contract=off`); it does with `-fno-trapping-math` — P2.T4's vectorisation check must settle this
  (per the board's escalation ladder, a flag on the scatter TU or `#pragma omp simd` before any library). V1–V3 and
  f3e–f3i readings may shift slightly; P2.T6 re-reads them. DESIGN §2.4 and the header comment are updated with the fix.
- 2026-09-25 — **Recency-rule fix landed (code `5f63919`, follow-up to P2.T3 per the consultant ruling above).** `uO/sO` rotate on
  the CoC jump only; a deposit that reaches N relabels `uO = qNew − pN`, `sO = (qNew − aNew) − pN·tN·(1−α)`; selects only, no new
  state. Volumetric test asserts pure `deposits·2⁻²⁴` on alpha and colour:alpha with a size-3/zb-9/α-0.8 rig added (fixed body
  reaches 0.013 / 0.045 of the bound; the unfixed body fails by 3.0–3.07× on size 6 and 116–177× on size 3, mutation logged in
  the task report). Adjacent-kernel-bin site: trough vs a double-normalised oracle (`exactCentreRowSum`) at `(Sa+Sb)·2⁻²⁴`
  (worst 0.572 of bound) plus exact `kernelGridRadius` pins over i ∈ [1, 1400]. α-0.9 ramp site: per-pixel weight-sum oracle at
  `2·N_p·2⁻²⁴` (alpha ≤ 0.23, arrival ≤ 0.27 of bound); snap-vs-blend control `maxS_snap−1 > 2·(maxS_blend−1)` for r0 ≥ 6.
  `grep 255` in the scatter tests now hits only an unrelated design-doc comment (line ~6613). Doctests 21/21, 101 + 1 skipped.
  DESIGN §2.4 carries the amended rule as implemented.
- 2026-09-25 — **M8.P2.T4 done (code `ae9b734`; evidence `~/deepc-validation/M8-P2T4/`).** `frameSetup()` builds the holdout set
  from `FrameDepthRange` (K+1 uniform in z); `DepthBuckets` survives only in `Math.h` constants/sanitiser and tests. `pieceStepPx`
  and `stagedRadiusPx` already cut like the flatten (no change). Tooltip, node help and pipeline comments rewritten for the stream.
  Probe rewritten as an in-node replay with a bit-for-bit self-check (no κ/T columns). `kSoAResidentBytesPerFragment` 100 → 70
  (measured median 65.98 B: SoA 49.98 + sort 16; the design note's 20 B sort estimate was high). **Vectorisation: ladder step (b)** —
  `#pragma omp simd` still missed ("control flow in loop"); `-fno-trapping-math` on `DeepCDefocusScatter.cpp` only vectorises the
  deposit loop (32-byte vectors), output bit-identical (o6 0/262 144, V2/V3 dumps identical, doctests same assertion count).
  Scene (a): a1–a3 0 at gate 0, a4 1.788e-7 (≤ 2e-7), a5 PASS. Determinism 0 differing channel-pixels at forced band heights
  1/7/32/96 (scratch-only switch, diff kept in evidence) and `-m 1` vs `-m 2`, on o6 and V2/V3. Probe (125,130) K=4: near card
  867 free-area deposits (Q 0.8909), stack rotates on the jump and exhausts F at its deposit #948, plane lands on the older chunk;
  G/A 0.59801 = reference. Scene (o) 36/0/1 both builds; o6c worst 0.176 (fog 0.2) / 0.111 (0.5), K-flat, vs T0 0.283/0.177.
  Scene (f) side check: f1/f2 unchanged, f3i now PASS 2.3e-6, f3e/f3f/f3h/f3h2 FAIL as predicted (await the rim ruling, P2.T6).
  Profile (interleaved, 5 reps): wall 27.3/27.6 s vs T0 40.4/40.4 (**−32.1 %**), CPU −36.2 %, RSS 1.219 vs 1.787 GB (−0.57 GB);
  spreads T0 0.02 s, M8 0.32 s. Left for P2.T6: node help "KNOWN LIMITATIONS" 1–2 still quote T0-era numbers.
- 2026-09-25 — **M8.P2.T8 done (code `d927a07`; evidence `~/deepc-validation/M8-P2T8/`), with two findings that change the
  rim question.** `tests/reference/vref.cpp` (standalone, exact per-ray path length, 4×4 px × 16×16 lens strata × 8 jittered
  replicates, fixed per-pixel seed, prints RGBA + SE + E[τ]); CMake target `vref` under `DEEPC_BUILD_TESTS`; harness `findVref()`
  (`DEEPC_VREF`, then beside the plugins, then `build/local-16.0/vref`; SKIP with reason if absent); six scene (f) rows f4a/f4ar
  (V1), f4b/f4br (V2), f4c/f4cr (V3) at K=16, R/G/B all gated (G contrasts only 0.50 vs 0.55 and hides the over-read). Bound:
  alpha `5·SE_max + ε·(1+τ_box)` with ε = 8.6e-4 (P1.T2's worst class, no rim exclusion) → 2.24e-3 (V2/V3), 5.55e-3 (V1);
  colour:alpha propagated per pixel for a ≥ 0.05; XFAIL hard ceiling `min(1, E[τ])` from the oracle (never crossed; closest
  1.25e-2 on V1). **Readings:** V1 alpha +0.150 / +10.87 % XFAIL, colour:alpha 4e-7 PASS; V2 alpha −8.8e-6 PASS, **colour:alpha
  +4.0e-2 XFAIL** (11 374 of 34 992 channel-px); V3 alpha −8.1e-6 PASS, **colour:alpha 1.43e-2 XFAIL**. (1) The over-card rigs'
  alpha is a leak test only (truth is 1 across the window) — the rim over-read shows in their colour, so the user's rim ruling now
  covers V2/V3 colour as well as V1. (2) The prototype's "V2/V3 worst 1.8e-3" was the old oracle's own aliasing at the card corner
  (true 0.570881 by 1-D quadrature; node 0.570910, 2.9e-5 off). (3) **V3 near-focus ring:** one pixel outside the box the fog share
  reads up to 1.08e-2 low (matching high just inside), where slices have sub-pixel radii the P1.T2 calibration (r 14/17 px) never
  covered; the implementer could not derive a bound and left a **provisional** 2.2e-2 (2× the reading, flagged in code and note,
  `VOL_V3_NEAR_FOCUS_DEFICIT`) — a fitted figure the board's rule forbids; sent to a consultant for derivation. (4) The CoC-sign
  mutation is undetectable by construction (lens integral even in u, and the card covers every ray); the halved-CoC mutation is
  the detectable one (V1, V2c, V3c FAIL); M8-T0 plugins: V1/V2/V3 all FAIL. Scene (f): PASS 8→11, FAIL 4 (f3e/f3f/f3h/f3h2, not
  re-pinned), XFAIL 3→6; all 15 prior rows identical. `.nk` header now 16.0v9 (written by Nuke 16). Oracle cost ≈ 13–19 s per rig.
  Docker does not set `DEEPC_BUILD_TESTS`, so `vref` is not built there today. Doctests 21/21, 101 + 1 skipped.
- 2026-09-25 — **P2.T5 split into two halves (execution deviation).** The user rulings (Q2 tolerance class, the silhouette
  band, the volumetric rim) are unanswered and the run is autonomous, so the ruling-independent rows — o6d/o6dα (node vs the
  layer-ordered partition of its own single-layer renders, term-count bounds), o7 (K-invariance, 0 ulps), o8 (determinism arm
  the harness can set) — run now as **P2.T5 part 1**; o6e, o6f and the o6c re-oracle stay **part 2**, after the rulings. The
  checkbox flips only when part 2 lands.
- 2026-09-25 — **Consultant ruling on P2.T8's provisional V3 bound (scratch `…/consult-v3/`, node model reproducing the node to
  ~1e-3): remove the 2.2e-2; the ring deficit is two node error classes, one a bug.** At (95,128) on V3: **pre-merge bug −6.4e-3**
  (pieces either side of focus, r 0.522/0.588, are within tolerance and drawn at the union's depth midpoint → r 0.097, sharp
  delta at focus; `pre_merge` off moves the ring from −1.075e-2 to −4.40e-3, model −4.37e-3) → **P2.T11**; **sub-pixel kernel
  error ≈ −1.0e-2 gross** (the kernel is a point-sampled disc, a delta at r ≤ 0.5; exact ring-1 coverage of a straight edge is
  2r/(3π) = 0.106 at r 0.5 where the node spills 0; a corner at r 0.5 reads 1.0 vs 0.7995); nesting +5.9e-3 partly cancels.
  ε_K(r) table (sup over edge/corner pixels): (0,0.5] 2.01e-1; (0.5,1] 1.85e-1; (1,2] 4.4e-2; (2,3] 2.3e-2; (3,4] 1.4e-2;
  (4,6] 9.3e-3; (6,8] 5.4e-3; (8,10] 3.1e-3; 14 → 3.8e-4 (agrees with P1.T2's 8.6e-4) — the constant ε is valid only from
  r ≈ 12 px, so every f4 row needs a radius-dependent term → **P2.T12**. **Q2:** V2's colour over-read is nesting (+0.130 of
  +0.133 share at (130,95); kernel −7.5e-4; +2.4e-3 free-area-first placement) and so is V1's (+0.140; V1 also exceeds the
  product model by ≤ +2e-2, the recency rule on 17 pieces, not the fill — `fill: background` is bit-identical); **V3's is mostly
  not nesting** (+4.76e-2 share at (96,96) = nesting 1.77e-2 + sub-pixel kernel 1.70e-2 + pre-merge 1.19e-2), so a per-parent
  chain (P2.T10) would leave V3's corner at ≈ +1.7e-2 after P2.T11. The `min(1, E[τ])` ceiling is legitimate only with the
  radius-dependent kernel term. Also: `sameLensPatch` treats every sharp radius as one lens patch, so the folded r 0.097 group
  joined its sharp neighbours. **PM decision:** P2.T11 (bug fix, no ruling needed) and P2.T12 (bound rework) appended; P2.T6 now
  also depends on P2.T12.
- 2026-09-25 — **P2.T5 part 1 implemented but NOT committed (working tree, `tests/nuke/scenes.py`; evidence
  `~/deepc-validation/M8-P2T5/`): o6d/o6da/o7/o8 added, o6d FAILs at fog 0.2.** o6d composes the node's single-layer renders by
  the partition rule (LO(P); `over` reads up to 0.107 off because the node fills free area first) at twice o6's term-count
  tolerance over all 3136 px; alpha exactly 1 everywhere; colour: 112 px around MIX_NEAR_BOX's corners read |ΔG/A| 1.473e-3
  (bound 1.297e-3) / |ΔB/A| 1.873e-3 (1.308e-3) at fog 0.2, PASS at 0.5 — the same **stack-weight error −3.35e-3** at both fogs
  and all K, so a coverage-weight difference between solo and full renders, not rounding. o7 0 ulps over the whole frame (K 16/64
  vs 4, both fogs). o8 SKIP: thread count is only Nuke's `-m` flag (no knob, no `nuke.execute` argument). Mutations: M8-T0 →
  o6d FAIL 0.261 / o7 FAIL (4.5 M ulp); K-term → o7 FAIL; free-area ceiling 1−2⁻⁸ → o6d/o6da FAIL (o6da cannot fail on M8-T0,
  whose alpha is also exact here). On this rig the pre-merge folds fog + card into one opaque fragment (radii 0.19 px apart).
  **New at HEAD: with `pre_merge` off, o6 alpha reads 0.9093 at (100,100), fog 0.2** — must be 1. Both sent to a consultant
  (diagnose; patch for the alpha defect delivered as a file, applied after P2.T11 lands).
- 2026-09-25 — **M8.P2.T11 done (code `376bc88`; evidence `~/deepc-validation/M8-P2T11/`).** `focusSideOf(signedRadius)` (−1 / +1,
  0 for any r ≤ `kSharpRadiusPx` incl. NaN, matching `sameLensPatch`: sharp fragments see the whole lens and stay one class)
  joins the pre-merge key, compared against the group head like the tolerance and bracket checks; both stale comments rewritten;
  the test's `refFlatten` reference carries the same key. New doctest on a manual rig (size 20, focus 10, slab 7–14, step 1.15)
  reproducing V3's pieces 7/8 (r −0.522 / +0.588): no group spans the cut, derived radius within [min, max] of members, colour:alpha
  twin. Mutation (key dropped): 2 failures (`0.0971 ≥ 0.5217`; reference test 51 vs 52 fragments). Doctests 21/21, 102 + 1 skipped.
  Scene (a) unchanged (a4 1.788e-7). Scene (f): only f4c/f4cr moved — V3 fog share at (95,128) −1.028e-2 → −3.92e-3, worst low
  (160,139) −1.075e-2 → −4.40e-3 (model −4.37e-3), worst corner (96,96) +4.756e-2 → +3.563e-2; f4cr channel-px past gate 568 → 179,
  the unprovisional low excursion 2.185e-3 → 0 (the provisional 2.2e-2 is no longer needed; P2.T12 removes it).
- 2026-09-25 — **Consultant ruling on P2.T5 part 1's two failures (scratch `…/consult-o6/`).** **(A) o6d's fog-0.2 FAIL is an
  oracle artefact:** the stack-alone render's virtual background uses the auto radius (14.049 px, z 8.20) while the merged stack
  fragment sits at 14.144 px (mid-depth 8.19); the kernels don't tile at the silhouette, arrival 0.99589, and the deficit fill
  scales the stack 0.81096 → 0.81431 = the +3.35e-3. Fix in the row: single-layer renders take `background_depth` at the
  layer's own merged radius so the fill is inert (arrival 1.0000023); o6d then reads |ΔG/A| 2.1e-6 / |ΔB/A| 5.0e-6 over all
  3136 px (0.4 % of the bound). Bound, population and o6da kept. **(B) `pre_merge`-off alpha 0.9093 is a genuine node limit of
  the M9 class, not a rule flaw and not the knob:** at the stack's silhouette the fog's and card's lens sets coincide, the rule
  files the card as disjoint (free area), and the plane's overflow never rotates (its CoC runs continuously through the stack's)
  so it covers only the pooled mean; deficit ÷ (1 − fog α) = 0.1134 at both fogs. Gap sweep with `pre_merge` on: 0.19 px
  (folded) → 1; 0.29 / 0.48 / 0.95 / 1.9 px → 0.9093–0.9096; 4.8 px → 0.999995 (a 2 px-jump coincidence). So o6's alpha row
  really tests the pre-merge, 0.06 px below its tolerance. A collision-merge mitigation (fold same-pixel point groups within
  1 px on one side) was measured and rejected (moves the boundary, changes the knob's meaning). **Rulings applied:** B → M9
  (with f4a's rim, the same mechanism: consecutive pieces 0.5 px apart at an edge-matching silhouette); two o6 variants
  (`pre_merge` off; gap 0.03 with it on) added as XFAIL with hard bound `1 − A ≤ (1 − fog α)·w_fog` (0.218 vs 0.0907 at fog
  0.2; 0.136 vs 0.0567 at 0.5); the `pre_merge` tooltip must say the pre-merge is the *more* accurate setting for same-pixel
  stacks under the stream (→ P2.T6's node-text clean-up); o6c and scene (i) i6 would show B with the knob off or a wider gap.
- 2026-09-25 — **M8.P2.T5 part 1 done (code `e2912e9`; evidence `~/deepc-validation/M8-P2T5/final*`).** Scene (o) `PASS=41
  FAIL=0 XFAIL=3 SKIP=1` (was 36/0/1/0); all 36 prior rows identical. o6d/o6da: stack-alone render's `background_depth` at its
  merged radius (`sampleMid` = a Python copy of `sampleMidDepth`, 14.144078 px), worst |Δ(G/A, B/A)| 4.97e-6 (fog 0.2) /
  2.17e-6 (0.5) vs ~1.2e-3 bounds, alpha exactly 1 on both sides, identical at K 4/16/64. o7 0 ulps whole frame. o8 SKIP (thread
  count is only Nuke's `-m`). **o6g** (`pre_merge` off) / **o6h** (gap `MIX_DELTA_UNFOLDED` 0.03): XFAIL, worst 1 − a 9.07e-2 (fog
  0.2) / 5.67e-2 (0.5) at 0.414 of the hard bound `(1 − fog α)·w_fog + N·2⁻²⁴` (w_fog from the solo fog render), two-sided; twins
  o6gr/o6hr gate the fog share ≥ 0 and G/B agreement (≤ 5.8e-5 vs ~6e-4) — the upper c/a side is not gateable because the fill
  scale (1/arrival, up to 1.0231) is not an output. Mutations: M8-T0 → o6d FAIL 0.261, o7 FAIL; ceiling 1−2⁻⁸ → o6d/o6da FAIL;
  ceiling 0.75 → o6g/o6h FAIL (1.14×/1.82× bound); B×(1−2⁻⁸) → o6gr/o6hr FAIL; doubled transmitted mass shrinks the deficit
  (0.171 of bound, still XFAIL — the band is wide by construction). `.nk` not regenerated (its StickyNote covers only o6c's nodes).
  Part 2 (o6e, o6f, o6c re-oracle) waits on the rulings.
- 2026-09-25 — **M8.P2.T12 done (code `c38c85f`; evidence `~/deepc-validation/M8-P2T12/`).** `vref kernel` mode: pieces cut as
  the node documents (uniform in CoC per side, focus a cut, step `max(2·max(tol, 1/8), variation/layers)` over the frame range
  incl. the card, ≤ layers+1 pieces at midpoint radius, α = 1−(1−a)^t, the card one more opaque layer); `c_spec` = the kernel
  spec reimplemented without `src/` (point-sampled disc, 1 px ramp, normalised, delta at r ≤ 0.5, bracket blend); `c_true`
  analytic slice coverage (Gauss–Legendre; 25× refinement moves K ≤ 1e-10); prints per-pixel K, plus a float32 term
  Φ = 2⁻²⁴·(2Σαᵢnᵢ + 2L) for the leak rows; 7–33 s per rig, byte-identical with/without OpenMP. Cross-check V3: K(96,96)
  3.2955e-2 (consultant 3.29e-2), K(95,128) 1.9008e-2 (1.89e-2). Gates `tol = 5·SE_max + K(p) + Φ(p)`, low `ref − tol`, high
  `max(ref, min(1, E[τ])) + 5·SE_τ + tol`; V2/V3 colour gated per channel as the fog's share (SE ÷ contrast, K not); a
  PREMISE-BROKEN guard FAILs the row if two same-side pieces ever fall within the merge tolerance (smallest gap today 0.94 px).
  Readings: f4a XFAIL (low 0.05 / ceiling 0.71 of bound), f4ar PASS 3.97e-7; f4b PASS 0.02, f4br XFAIL (0.08 / 0.70); f4c PASS
  0.01, f4cr XFAIL (low 0.20 at (160,139), ceiling 0.36, 153 of 22 188 channel-px past). No provisional figure remains.
  Mutations: halved CoC → f4a/f4br/f4cr FAIL (0.179/0.166/0.119); M8-T0 → f4a/f4b/f4br/f4c/f4cr FAIL; spec without the ramp →
  K max V3 3.30e-2 → 6.51e-2, V1 1.91e-3 → 6.23e-3. Scene (f) 11/4/6 before and after, f0–f3i identical. `scene_f` `.nk`
  regenerated only. Doctests 21/21, 102 + 1 skipped.
- 2026-09-25 — **Run paused: every remaining task depends on the user's rulings.** Landed this session: recency-rule fix
  `5f63919`, P2.T4 `ae9b734`, P2.T8 `d927a07`, P2.T11 `376bc88`, P2.T5 part 1 `e2912e9`, P2.T12 `c38c85f`. Waiting: P2.T5 part 2
  (o6e at Q2's tolerance class; o6f after the silhouette-band ruling; o6c re-oracle), P2.T6 (rim ruling: f3e/f3f/f3h/f3h2 stay
  FAIL until ruled XFAIL-with-bound or fixed by P2.T10), P2.T10 (only if ruled in), Phase 8.3, Phase 8.4.
- 2026-09-26 — **User rulings on the three blocking questions (via `/cat-discuss`).** (1) **Q2 → (i)**, a derived bound of
  the kernel's calibrated rim residual + 3 × the reference's Monte Carlo SE, ≈ 1.5e-3 (user corrected 3σ-only the same day;
  project-wide: `PLAN/DECISIONS/2026-09-26-reference-bound-3-sigma.md`); P1.T3 is now fully
  ruled. (2) **Silhouette band → fold M9 into M8** as Phase 8.5 rather than XFAIL it
  (`PLAN/DECISIONS/2026-09-26-m9-folded-into-m8.md`); o6f moves from P2.T5 to Phase 8.5. (3) **Volumetric rim → deferred, not
  ruled:** the user rejects (b)'s premise — "one fog body" is not information a deep image stores — and expects the general
  nesting fix may address it; P2.T10 cancelled, P2.T6 leaves the rim rows to Phase 8.5 and returns them to the user if still out.
  Consequence: Phase 8.5 is new scope ahead of the gate, and M8.P1.T3 can be checked off.
- 2026-09-26 — **Evidence directories may be lost.** `~/deepc-validation/` and `~/deepc-baselines/` do not exist on the host the
  rulings were given from; they were probably in an ephemeral container and may survive only in the PM's container. On resume,
  the PM checks for them first. If `~/deepc-baselines/M8-T0/` is missing, rebuild it per M8.P1.T1 from `2ac3550` before any
  mutation run that uses it. Reports that cannot be rebuilt (P1.T2's REPORT, P2.T7's `DESIGN.md` and prototype patch) — if
  missing, say so to the user and work from the committed code, the tests and this file's decisions; the P1.T2 reference tool
  must be re-derived for P3.T1 in that case.
- 2026-09-26 — **Execution order on resume (PM):** P3.T1 runs before P2.T5 part 2, because o6e gates against the reference
  and the reference must be in the harness to be an oracle (not a pasted map). Then P2.T5 part 2 → P2.T6 → P5.T1 → the rest
  of Phase 8.5 → P3.T2 → Phase 8.4. P3.T1 finalised in place (Monte Carlo tool promoted from P1.T2, `analytic` kept as the
  cross-check). Evidence directories confirmed present on the PM's host (the 2026-09-26 loss note does not apply).
- 2026-09-26 — **M8.P3.T1 done (code `5e5f633`; evidence `~/deepc-validation/M8-P3T1/`).** `tests/reference/thinlens_ref.cpp`
  (the P1.T2 tool in house style, arithmetic unchanged, `mc` + `analytic`), CMake target `thinlens_ref` beside `vref`;
  harness `findThinlensRef()` (`DEEPC_THINLENS_REF`), `runThinlensRef()` (RGBA + SE, colour ratios, per-layer weights),
  `dumpDeep()` via DeepWrite at 32-bit float (`deepSample()` reads stale data headless), `exrio.readDeepExr()`. **Risers on**
  (without them rays slip between plane rows: opaque o6 alpha 0.945 at (150,150) where the flatten is 1). Reproduction: all six
  P1.T2 dumps' five-pixel outputs and the cal790 map **identical** (0 difference), o6 fog-0.2 fresh dump → 4 096/4 096 map px
  identical. New rows **o0c** (alpha, 289 px incl. rim, bound 8.6e-4 + 3·SE + N·2⁻²⁴: worst 7.9e-4 / 0.60 of bound) and **o0cr**
  (c/a, 3·SE + N·2⁻²⁴: 0.03 of bound). Mutations: half-pixel centre shift → FAIL 16.6×; CoC ×(1+1e-2) → FAIL 4.0×; **CoC ×(1+1e-3)
  is undetectable under the ruled bound by construction** (straight-edge δa ≤ δr/(πr) = 3.2e-4 < 8.6e-4 rim allowance) — the
  brief's suggested mutation was too small; stated in the row note. Runtime 12.6 s per calibration call; scene (o) 90.8 → 104.3 s;
  a 4 096-px o6 map costs 325 s. Scene (o) `PASS=43 FAIL=0 XFAIL=3 SKIP=1`, prior rows identical.
- 2026-09-26 — **M8.P2.T5 done — part 2 (code `0a0a709`; evidence `~/deepc-validation/M8-P2T5/part2/`).** Scene (o) `PASS=46
  FAIL=0 XFAIL=3 SKIP=1`; 45/47 prior rows byte-identical (o5b differs only in its printed out-dir; old o6c renamed **o6cb**, the
  Bokeh reading, unchanged two-sided XFAIL). **o6c** now node vs thin-lens reference (G/A, B/A, R/A; K 4/16/64 × fog 0.2/0.5) over
  MIX_BOX minus the **silhouette band** — geometric: pixels whose footprint the stack's larger disc (fog, r 14.2396) sees across
  MIX_BOX's edge AND the near card's disc (r 17.0127) reaches; leaves the saturated core (115,115,141,141), 676 px, band 2 460 px.
  **o6e** (alpha, K=16) / **o6er** (c/a) over the same 676 px (deviation: the band clips one ring off "MIX_NEAR_BOX inset 2", 784
  px). Bound 8.6e-4 + 3·SE + term count; B/A SE bounded as (SE_B + (B/A)·SE_A)/A. Readings: c/a worst 6.56e-4 / mean 1.0e-4 / 0.378
  of bound (fog 0.2), 4.13e-4 / 0.254 (0.5); alpha exactly 1 both sides. Bokeh vs reference over the core: 7.2e-2 / 4.5e-2. Band
  readings (node − ref, fog 0.2): (155,128) +0.046, (104,104) +0.019, (150,150) +0.027 — Phase 8.5's targets. Mutations: M8-T0 →
  o6c 163×, o6er 63× FAIL (o6e PASS: T0 alpha was already exact here); fitshrink / free-area ceiling → o6c, o6er FAIL; output ×(1−2⁻⁸)
  → o6e FAIL 2.7×. **The naive-rule mutant is undetectable on these cells by construction** (pre-merge folds fog + card into one
  opaque fragment; on an all-opaque stream the naive rule equals the amended one) — substituted fitshrink, as the brief allowed.
  Reference 3×64 strata × 8 reps, seed 818: scene (o) now 365 s wall (+250 s). P3.T2 is left with only the band pixels, after
  Phase 8.5.
- 2026-09-26 — **M8.P2.T6 landed except n8b (code `57aae0c`; evidence `~/deepc-validation/M8-P2T6/`, `moved-rows.md` lists 120
  changed rows with causes).** Total `PASS=199 FAIL=6 XFAIL=8 SKIP=3` (a–f 38/4/5/1, g–j 53/0/0/0, k–n 62/2/0/1, o 46/0/3/1). FAILs:
  f3e/f3f/f3h/f3h2 (→ P5, status untouched) and **n8b ×2**. a1–a3 0, d black, m4a/m4b/n1/n7 PASS, doctests 21/21 + 102/1 skip.
  Re-pins, each FAILing on T0 or a recorded scratch mutation (`~/deepc-scratch/M8-P2T6`): **g4/g5** vs α(1+(S−1)(1−α)) with S from
  the shipped LUT via new tool `tests/reference/kernel_sums.cpp` (CMake target; S = 1.0719210, the brief's 1.0712 was back-derived),
  node within 5.4e-7, gate (N+8)·2⁻²⁴, N 4 843; **m3c** same law per row, XFAIL dropped; **f3c/f3d** at arrival tap counts (5 520 /
  4 240), XFAILs dropped; **f3b** gate re-derived to 3 039 taps; **i7/i7d** redesigned on a 128² card edge (full-frame pair no longer
  shows pre-merge: 2.4e-7); new colour:alpha twins f3br/f3cr/f3dr, g4r, g5r×4, m3cr. Node text: KNOWN LIMITATIONS 1–2 restated
  for the stream; `pre_merge` tooltip says pre-merge is the more accurate setting for same-pixel stacks. h3c now 1.4e-6 vs 2e-6
  gate (passes narrowly). exrdiff scene (m) vs T0: halo 1 ulp, α 0.9 ramp ≤ 0.034, α 1 ramp 1.4e-5. **Open, sent to a consultant:**
  (1) n8b moved by the stream (equal-depth background samples take free area in raster/sort order at the halo edge — rows 124–127
  show, 128–132 nothing; T0 averaged the disc; foreground reading 0.0648 → 0.0689) and its pins were bake-off readings, so there
  is no oracle; (2) f3c's arrival-count gate does not bound the composite's own rounding (comment says "empirical") — check it is
  derived, not fitted. P2.T6 stays open until both are settled.
- 2026-09-26 — **Consultant ruling on P2.T6's two leftovers (`~/deepc-validation/M8-P2T6-consult/`); PM applies it.** (1) **n8b:**
  a real stream-rule defect of the correlated-occlusion class (card and checker on opposite sides of focus: the open lens sees
  background from x ∈ (85,88] under the card, the stream takes rows 124–127; coverage amounts match the reference to 1e-4, the
  identity is wrong); raster-order dependent (bottom strip 0.0637 vs top 0.0185; transpose asymmetry 0.163 vs T0 5.4e-7); a
  tie-break does not fix it. The "foreground reading moved" claim was wrong — the foreground render is unchanged, its twin moved.
  Old n8b pins retired (bake-off readings, no oracle); a transpose-symmetry row (derived `N·2⁻²⁴`) and a thin-lens row added, both
  **left FAIL** pending Phase 8.5 (no XFAIL without a ruling), and added to Phase 8.5's target list with order independence as a
  design requirement. (2) **f3c's arrival-count gate was fitted in disguise** (it bounds only the fill's +1.7e-5; the composite's
  rounding over 35 574 deposits is unbounded; f3b/f3d same pattern). No derived bound on the current rig catches T0; fix the rig
  instead: a size-6/K-4 (or 12/4) row gated at `(κ·n_every + n_arrival)·2⁻²⁴·0.75` with κ derived, f3b/f3c/f3d re-gated at their
  derived worst case, arrival gated directly. Both sent to one follow-up implementer; P2.T6 closes when it lands.
- 2026-09-26 — **M8.P2.T6 done — follow-up (code `53fc335`; evidence `~/deepc-validation/M8-P2T6-followup/`, κ argument
  `kappa.md`).** New `densityBound()` gates at `(κ·n_every + n_arrival)·2⁻²⁴·value`, κ derived per rig from the update's forward
  error (light conserved by construction; per deposit 1 + α·(4 + 2w_A + 2w_M), R = run of boundaries not certain to rotate; assumes
  glibc ≤ 1 ulp `log1pf`/`expm1f`). **f3j** (size 6, K 4; κ 4.40, bound 7.09e-4): HEAD −4.1e-6 PASS, T0 −5.72e-3 FAIL — the
  discriminator; f3jr twin. f3b (κ 2.23, 5.22e-3; T0 passes too), **f3c sanity-only** (R = 17, κ 17.0, 2.72e-2; no longer catches
  T0), f3d (κ 1.98, 6.11e-3; catches T0). **f3a** gates |1 − arrival| ≤ n_arrival·2⁻²⁴ from the probe (0.093 of bound; catches the
  arrival mutation; SKIP on T0, no probe). n8b / n8b#2 retired; **n8c/n8cr** transpose symmetry (2·(11·922 + 992)·2⁻²⁴ = 1.33e-3):
  HEAD FAIL 0.174, T0 PASS 5.4e-7; **n8d/n8dr** vs thin-lens over half the band (2 926 px, ~2.5 min): alpha PASS 0 diff, colour FAIL
  mean 0.042 (T0 also fails, 0.031). n8c/n8cr/n8dr left FAIL → Phase 8.5. Mutations: deposit weight ×0.99 → f3j FAIL; resolve
  alpha ×0.99 → n8d FAIL (per-deposit mutations heal in the opaque stack). Tallies f 18/4/5/0, n 24/3/0/1; all other rows
  identical. **Suite total now `PASS=203 FAIL=7 XFAIL=8 SKIP=3`**; FAILs f3e/f3f/f3h/f3h2, n8c/n8cr/n8dr — all Phase 8.5 targets.
- 2026-09-26 — **Evidence directories lost on resume (PM, ~09:00).** A new PM session found `~/deepc-validation/` and
  `~/deepc-baselines/` absent on this host (the earlier loss note now applies): P1.T2's REPORT, P2.T7's `DESIGN.md` and
  prototype patch, every task's evidence logs, and the M8-T0 plugin set are gone. Committed code, tests, the harness oracles
  (`thinlens_ref`, `vref`, `kernel_sums`) and this file's decisions survive. Actions: M8-T0 is rebuilt per M8.P1.T1 from
  `2ac3550` in a separate worktree; P5.T1 works from the committed code and this file's recorded figures. The user is told.
- 2026-09-26 — **M8-T0 partially rebuilt; Nuke license server down (PM).** Worktree at `~/DeepC-T0` (`2ac3550`; `~/git/` is not
  writable for new siblings). `~/deepc-baselines/M8-T0/plugins/` holds all 28 `.so` + `PROVENANCE.txt`/`plugins.sha256`; doctests
  26/26 + 121/121. The headless-load check and the a–o harness are **not run**: the RLM server `5053@172.20.0.1` refuses connections
  (Nuke exits 100, `ENT_STATUS_RLM_LICENSE_COMM_ERROR`). Every Nuke-dependent step (T0 a–o, P5.T1's harness readings, P3.T2, P4.T*)
  waits for it. P5.T1 proceeds on doctests / standalone drivers vs `thinlens_ref`, harness rows flagged as predicted.
- 2026-09-26 — **M8-T0 rebuilt a second time, into `.evidence/` (PM, ~18:30).** Worktree `.evidence/DeepC-T0` at `2ac3550`;
  `~/deepc-baselines/M8-T0/plugins/` (→ `.evidence/baselines/M8-T0/plugins/`) holds 28 `.so` + `menu.py`, `PROVENANCE.txt`,
  `plugins.sha256`; `DeepCDefocus.so` `fa351898…` (differs from P1.T1's `282130b6…` — that build used a different SDK/flags;
  the a–o readings, not the hash, are the baseline). Doctests 26/26 + 121/121. Headless load and T0 a–o still wait on the
  licence server (down at 18:25).
- 2026-09-27 — **PM resumed (~01:30); licence server still down** (RLM `5053@172.20.0.1` COMM_ERROR, no login tokens). The
  previous P5.T1 consultant was cut off mid-experiment: `~/deepc-validation/M8-P5T1/` holds a standalone driver
  (`tools/p5drv.cpp`), rule runs r2–r6 on an o6 fog-0.2 dump, a V1 reference map, and no design note. P5.T1 is relaunched to
  resume from those files, harness rows still flagged as predicted.
- 2026-09-27 — **Licence server back (02:00, headless Nuke 16.0v9 exits 0).** The P5.T1 consultant was told to take harness
  readings where they are cheap. The pending M8-T0 checks (headless load + a–o on `~/deepc-baselines/M8-T0/plugins`) run in parallel,
  with evidence in `~/deepc-validation/M8-T0-rerun/`.
