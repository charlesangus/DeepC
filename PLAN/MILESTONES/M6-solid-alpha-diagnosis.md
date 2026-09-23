# Milestone 6: Solid alpha on opaque geometry — Bokeh oracle, scene (o), and the mechanism ruling

Opaque geometry must come out of `DeepCDefocus` with alpha **exactly 1** wherever it covers the
output pixel. On a slanted opaque plane with several small opaque objects in front of it —
**complete deep** (plane samples present behind every object, via `DeepMerge`), `fill: foreground`
— the node still shows slight alpha gaps/dips: around the objects, across the plane itself, and
where object discs overlap. Bokeh (Foundry's bundled build of pgBokeh, a full deep defocus)
renders the identical deep scene with solid alpha. This milestone does **not** change the
algorithm: it builds the rig and the Bokeh oracle into the harness as scene (o), pins every dip
as an XFAIL with a hard outer bound, adds a per-pixel decomposition probe, and runs the mutation
experiments that say **which mechanism** produces each dip class. Its output is a written ruling
that unblocks M7 (the fix). Runs before M2.

## What we know (read before any task)

**Ground truth.** With a source sample under every ray and opaque geometry everywhere in view,
the only correct alpha is `1.0`. Not "close": the acceptance bar throughout this milestone and M7
is **alpha `== 1.0`**, with any slack stated as an ulp bound derived from the accumulation's term
count (`N·2⁻²⁴`, the m1/l6 idiom), never a fixed decimal. The user has ruled that 8-bit-derived
tolerances are never cited in this project.

**The pipeline the dips come out of** (all in `src/DeepCDefocusScatter.h`/`.cpp`; M4's four pieces
are always-on): `flattenPixelToSoA()` partitions each source pixel's unit area over its fragments
(`share = t·α`, `t *= 1−α`; an opaque sample takes everything behind it to share 0) →
`scatterBandCPU()` deposits `w·share·α`, colour, and coverage `w·vis` into K bucket planes, and the
raw `w·share` into the K-independent `arrival` plane; a fragment whose depth falls between bucket
centres is **split** — coverage into the nearer bucket only, alpha `w·f` / `w·(1−f)` across the
pair, the rear part arriving with no coverage of its own (`scatterSpanBothBuckets()`); fractional
kernel diameters blend two bracketing odd kernels with the same `f` into colour and arrival →
`resolveBandCPU()`: `saturateBucketPlanes()` (down-only), then `compositePixelCoveragePartition()`
(the area model: `fit`/`excess` per bucket, the co-located residual `aRes` attenuated by
`tHead`), then the deficit-only division by `arrival` where `kFillMinArrival < D < 1 −
kFillDeficitTol`, then the down-only clamp. Every piece has a written identity that claims opaque
geometry sums to exactly 1 (`compositePixelCoveragePartition`'s contract block, ~2221–2330) — the
dips are where at least one of those identities does not hold on this rig.

**What is already pinned and must be respected.** Scene (m): m3a pins the α=1 45° ramp at 2.09e-6
(so a plane *alone* at scene (m)'s size/K may not reproduce the user's plane dips — scene (o) sweeps
size and K); m1 pins the flat-BG halo at 5.96e-8 with a **BG on the focal plane (r=0)** — the
object-over-plane case where the plane has its own CoC is *not* pinned anywhere. Scene (g) records
g4's bucket-pooling deficit and g5's over-read at α<1. M4 rejected symmetric `1/D` and named a
depth-gated ("same-surface") arrival plane as the structural follow-up for α<1 ramps — that is a
**candidate** for M7, not a decision; the α=1 case may have a cheaper cause.

**Hypotheses the diagnosis must accept or reject, each with a named mutation** (numbers, not
reasoning, decide — M1's standing lesson):

- **H1 — fractional bucket split.** On a ramp every fragment sits between two bucket centres; the
  rear part arrives as alpha with no coverage and is attenuated by `tHead`. If the per-pixel sum
  `w·f + aRes·tHead` is not `w` for opaque fragments at some `f`, the plane dips **periodically in
  y** with the bucket spacing. Mutation: sweep `depth_layers` (K = 4, 16, 64, 128) — dip period and
  depth should track K; and place bucket centres exactly on a flat card's depth (dip must vanish)
  vs. midway (dip maximal).
- **H2 — kernel blend at fractional diameters.** Coverage and arrival take the same blend `f`, but
  adjacent scanlines on the ramp bracket different kernel pairs. Mutation: pick `size` so every
  row's diameter is an odd integer (no blend) vs. half-integer (maximal blend).
- **H3 — object over a plane with its own CoC.** Under the object's silhouette the plane's own
  samples have share 0, so the plane's coverage there arrives only from neighbouring plane
  pixels at the plane's radius, while the object's coverage arrives at the object's radius; the
  two sums are complementary only when the kernels are. m1 covers `r_plane = 0` only. Mutation:
  `r_plane = 0` (must reproduce m1's 5.96e-8) vs. `r_plane ∈ {2, 8, 16}` px with `r_obj = 16`, and
  `r_obj < r_plane`; complete vs. sparse deep (the user reports complete deep dips too).
- **H4 — overlapping object discs at different depths.** Two objects in different buckets whose
  blooms cross: the second bucket's coverage is `excess` over the first's claimed area and is
  attenuated by `tClaimed`, which is only 0 if the first bucket's `a_k` is exactly 1. Mutation:
  same object depth (one bucket, pure `fit`) vs. different depths; plane present vs. absent.
- **H5 — the fill's own gates.** `kFillMinArrival = 1e-3`, `D < 1 − kFillDeficitTol (1e-5)`: pixels
  where `D ≥ 1 − 1e-5` but `accAlpha < D` are never scaled, and pixels with `D > 1` but
  `accAlpha < 1` are never touched at all (deficit-only by design). The probe shows this directly.
  Mutation: `kFillDeficitTol` → 0 and, separately, symmetric `1/D` (known to break gate (c) —
  measurement only).
- **H6 — saturate-down before the composite.** A bucket with `C_k > 1` from pooled kernels has
  `A_k` pulled to 1 before the area split; if `fit = min(C_k, freeArea)` then reads a clamped `C_k`
  the accounting can under-claim. Probe: per-bucket `C_k`, `A_k` before and after saturation at a
  dip pixel.

**Oracle.** Bokeh runs headless here: `nuke.nodes.Bokeh()` accepts the deep stack on **input 3**
(the only input that takes a deep node — verified 2026-09-18, `setInput(3, deep) → True`), needs a
2D image on input 0 for channels (a `DeepToImage` of the same stack), and rendered a card-over-plane
rig (`focalPlane=20, fStop=1.4, max_kernelsize=64`) with alpha `1.00000` across the halo band
where `DeepCDefocus`'s M4 halo fix is pinned. Its CoC is thin-lens (∝ `|1 − F/z|`, the same shape
as `coc_mode: manual`'s `size·|1 − focus/z|`), so one scale constant matches the two; calibrate it
by measuring a single-row bokeh's extent in both (the `groundPlaneRow()` non-vacuity idiom).

**Baselines.** `~/deepc-baselines/` and `~/deepc-validation/` **no longer exist on this host**
(M4/M5's evidence directories are gone). Nothing in this milestone needs them — the T0 `.so` is
recaptured from HEAD (`46421fa`, the merged M5) in P1.T1 — but any brief that says "compare to
M5-T0" means that recaptured binary, and M4's `~/deepc-baselines/M4-P3T1/` probe evidence is
unavailable.

**Constraints from the board apply in full:** no SIMD, `DEEPC_HD` header-only kernels, `PodBuffer<T>`
planes, doctest, 4-space/`_`member/lowerCamelCase, comment policy (no narrative comments, no
plan/task references in code), every build and Nuke run through `scripts/hostguard.sh` (`--mem-gb 6`
for the harness), work on `claude/deep-defocus-node-plan-o0ld83`. **Colour:alpha ratio is a standing
invariant**: every new alpha assertion gets a colour-ratio assertion beside it. Every XFAIL carries a
hard outer bound; every new check is mutation-tested; oracles are never the new output.

## Phase 6.1: Oracle and scene

- [x] M6.P1.T1 — Recapture the T0 `.so` and reference renders from HEAD
  - files: `~/deepc-baselines/M6-T0/` (new, outside the repo)
  - approach: the previous baseline directories are gone. Configure and build
    `cmake -S . -B build/local-17.0 -D Nuke_ROOT=/usr/local/Nuke17.0v3 -D DEEPC_BUILD_TESTS=ON` and
    `scripts/hostguard.sh -- cmake --build build/local-17.0 -j` (never `build/local-17.0-O3`) from
    the clean tree at `46421fa`; copy `DeepCDefocus.so` to `~/deepc-baselines/M6-T0/` with a
    `PROVENANCE.txt` (commit, date, SDK, build dir, `sha256sum`); render scene (m)'s kept EXRs
    there (`--scenes a,m --out-dir ~/deepc-baselines/M6-T0/renders`). Run both doctest suites and
    the full a–n harness in two halves (`a–f`, `g–n`) under `scripts/hostguard.sh --mem-gb 5.5`
    with `DEEPC_PLUGIN_DIR` pinned to the fresh build, and keep the logs beside the renders.
  - verify: the preserved `.so` loads in headless Nuke from its path; the harness reproduces M5's
    closing tally `PASS=135 FAIL=2 XFAIL=11 SKIP=1` or every delta is explained in writing;
    `tests/nuke/exrdiff.py` reports 0 ulps on a kept EXR against itself; tally and paths recorded
    in `## Decisions`.
  - size: S

- [x] M6.P1.T2 — `makeBokeh()`: the Bokeh oracle in the harness, CoC-calibrated to `DeepCDefocus`
  - files: `tests/nuke/harness.py` (new `makeBokeh(settings, source, focusDistance, size, ...)`
    beside `makeDefocus()` at ~251 and `deepToImage()` at ~286), `tests/nuke/scenes.py` (a
    calibration cell inside `sceneO()`'s file, see P1.T3 — the helper's own check lives with the
    scene)
  - approach: build `Bokeh` with the deep stack on input 3 and `deepToImage(source)` on input 0,
    `depthStyle = Real`, circular kernel, bloom off, `max_kernelsize ≥ 2·settings.maxRadius`,
    `focalPlane = focusDistance`; set `fStop`/`focalLength`/`filmFormat`/`worldScale` (whichever
    of Bokeh's lens knobs are honoured in `realWorldLens` mode — probe, don't assume) to a fixed
    lens whose CoC scale is then **measured**: render one `groundPlaneRow(y)` through both nodes at
    the same `focusDistance` and read each bokeh's half-extent along the row; derive the single
    multiplier that maps `size` to Bokeh's lens and expose it as a harness constant with the
    calibration numbers in its docstring. Fail loudly if Bokeh is unavailable (`nuke.nodes.Bokeh`
    raises or the deep input refuses) — the scene then SKIPs, never silently passes.
  - verify: a calibration cell (o0b in P1.T3) reads the two half-extents within 1 px of each other
    at two different `size` values; `makeBokeh()` on the M4 halo rig (`haloForeground()` +
    `haloBackground()` merged) reads alpha exactly `1.0` across the halo band — the oracle's own
    non-vacuity guard, recorded in the docstring; headless run under `scripts/hostguard.sh`.
  - size: M

- [x] M6.P1.T3 — Scene (o): slanted plane + small objects, complete deep, dips pinned as XFAILs
  - files: `tests/nuke/scenes.py` (new `sceneO()` reusing `groundPlane()`/`groundPlaneRow()`
    ~1597–1612, `pointLayer`/`rectangle2d`/`deepMerge` and the `_worstUnpremult()` idiom; register
    in `SCENES` at ~5268), `tests/nuke/generate_scene_scripts.py` (register at ~947 as
    `("o", "scene_o_solid_alpha.nk", buildSceneO)`), `tests/nuke/scene_o_solid_alpha.nk` (generated)
  - approach: the rig is `deepMerge([cards…, groundPlane()])` — the opaque ground ramp
    (`GROUND_Z_EXPR`, focus at `GROUND_FOCUS`) plus **four** small opaque cards (≈24–40 px) in
    front of it at two depths: two whose blooms overlap each other, one isolated over the
    near-focus rows, one over the far field; all cells `fill: foreground`, `coc_mode: manual`,
    complete deep. Cells: **o0** non-vacuity — a single `groundPlaneRow` renders with the expected
    extent, and each card's bloom extent matches `radiusPixels`; **o0b** the Bokeh calibration
    (P1.T2); **o1** plane alone at the run's K and size, plus a K sweep (4, 16, 64) and a
    half-integer-diameter size — per-row minimum alpha over the plane's interior; **o2** one card
    over the plane at `r_plane ∈ {0, 2, 8, 16}` under it (move the card in y) — minimum alpha in
    the band under the silhouette; **o3** the full rig — minimum alpha over the whole covered box,
    and separately over the two-card overlap region; **o4** the sparse twin of o3 (`_cropToDepth`
    idiom, no plane behind the cards) — same readings; **o5** Bokeh comparison — `makeBokeh()` on
    o3's stack, alpha over the same box must read exactly `1.0` (the oracle arm), and the
    DeepCDefocus−Bokeh alpha difference map is written to `--out-dir` for the user. Every dip
    reading is a **two-sided XFAIL**: expected `1 − α == 0` within an ulp bound derived from the
    term count, currently reads the measured value, hard outer bound at 2× the measured dip; a
    reading of exactly 1 where a dip was expected is reported so the cell is re-examined, not
    silently green. A colour-ratio arm (unpremultiplied plane/card colour within the same ulp
    bound of the source's own `deepToImage` flatten) sits beside every alpha arm.
  - verify: `scripts/hostguard.sh --mem-gb 6 -- tests/nuke/run_validation.sh --scenes o` runs with
    o0/o0b/o5-oracle PASS and each dip cell XFAIL with its measured value in the note; each dip
    cell demonstrated to move under at least one of the H1–H6 mutations (recorded in the note);
    the committed `.nk` reproduces every cell; the difference-map EXR opens and shows the dips
    where the user reported them (around the objects, across the plane, in the overlap).
  - size: L

## Phase 6.2: Decompose and rule

- [x] M6.P2.T1 — Per-pixel decomposition probe (`DEEPC_DEFOCUS_DEBUG_PROBE`)
  - files: `src/DeepCDefocus.cpp` (env-var read beside `_debugBands`/`_debugStats` at ~522–570; the
    band resolve call), `src/DeepCDefocusScatter.h` (`resolveBandCPU()` ~3382 and
    `compositePixelCoveragePartition()` ~2832: an optional per-pixel trace sink), `tests/test_defocus_scatter.cpp`
  - approach: `DEEPC_DEFOCUS_DEBUG_PROBE="x,y;x,y;…"` (output-pixel coordinates) prints, for each
    listed pixel, one line per bucket — `k, zCentre, C_k (raw and saturated), A_k (raw and
    saturated), D_k (fourth plane), colour` — then `arrival D`, `freeArea`/`tClaimed` after each
    bucket, `accAlpha` before the fill, after the fill, after the clamp. Zero cost when unset (one
    `getenv` at op construction, a null sink pointer in the resolve). Do not change any output.
    Implement as a small trace struct passed by pointer so the doctest can capture it without
    stdout parsing.
  - verify: doctest on a two-bucket fixture: the trace's per-bucket numbers equal the planes'
    values and the composite's arithmetic reproduces the returned alpha bit-exactly; with the
    variable unset the scene (m) kept EXRs are 0 ulps against `~/deepc-baselines/M6-T0/renders`;
    a headless run with the variable set on scene (o)'s o3 prints one block per listed pixel.
  - size: M

- [x] M6.P2.T2 — Run the H1–H6 mutation experiments and write the mechanism ruling
  - files: none in the repo tree — throwaway scripts and the evidence under
    `~/deepc-validation/M6-P2T2/`; the ruling is the PM's edit to this file's `## Decisions` and
    to `M7-solid-alpha-fix.md`'s `Blocked on:` line
  - approach: for each dip class scene (o) pinned (plane interior, under-silhouette band, disc
    overlap), probe the worst pixel with P2.T1, then run the mutation named for each hypothesis in
    "What we know" and record the reading before/after. A hypothesis is **accepted** for a class
    only if its mutation moves that class's dip by an amount its mechanism predicts, and
    **rejected** if the dip survives the mutation unchanged. Separate the classes: a mechanism
    that explains the plane dips need not explain the overlap dips. Where a mutation needs a
    source change (H5's tolerances, H6's saturate-down), make it on a scratch build, never commit
    it. End with a per-class table (class → accepted mechanism → the term in the composite/fill
    that mis-accounts → the smallest change that would make it exact → what existing pin it
    would move) and a one-paragraph recommendation for M7's approach, stating explicitly whether
    the fix is local (a term in `compositePixelCoveragePartition()`/the fill gates) or structural
    (the depth-gated arrival plane M4 named).
  - verify: every hypothesis has a before/after number for every class (no "not tested"); every
    accepted mechanism has a mutation that removes the dip on at least one pixel (to within the
    ulp bound); the ruling is recorded under `## Decisions` and M7's `Blocked on:` line is
    rewritten to name the chosen approach; evidence paths recorded.
  - size: L

## Phase 6.3: Gate

- [ ] M6.P3.T1 — Full suite, cross-`.so` identity, and the PR
  - files: none — verification only; any fix lands in the task that owns the code
  - approach: clean rebuild of `build/local-17.0`; both doctest suites; the full a–o harness in two
    halves under `scripts/hostguard.sh --mem-gb 5.5` with `DEEPC_PLUGIN_DIR` pinned to the fresh
    build; `exrdiff.py` on scene (m)'s kept EXRs against `~/deepc-baselines/M6-T0/renders`
    (the probe must be output-neutral); `./docker-build.sh --linux`.
  - verify: doctests green; tally `PASS=134+N FAIL=2 XFAIL=11+X SKIP=2` (the M6-T0 16.0v9 baseline) with N/X scene (o)'s
    PASS/XFAIL counts and **no a–n row moving**; 0 ulps on all three (m) EXRs; docker gate green.
  - size: M

**Verification gate:** scene (o) registered and green in the XFAIL sense (o0/o0b/o5-oracle PASS,
every dip cell XFAIL with a measured value and a hard outer bound, every cell mutation-tested); the
Bokeh oracle renders headless and its calibration is recorded; the probe is output-neutral (0 ulps
on scene (m) against M6-T0); the full a–o suite has no a–n row moved from M5's closing tally; the
H1–H6 table and the M7 recommendation are in `## Decisions` and M7's `Blocked on:` names the chosen
approach; `./docker-build.sh --linux` green; then PR to `master` from
`claude/deep-defocus-node-plan-o0ld83`.

## Decisions

- 2026-09-18 — **User rulings (planning interview):** the acceptance bar is alpha **`== 1.0`** where
  geometry is opaque — 8-bit-derived tolerances (`1/255` and kin) are never cited in this project
  again; Bokeh is the oracle and is a full deep defocus (Foundry's bundled pgBokeh, renamed) that
  rendered the identical deep scene; the rig is built in the harness with **complete** deep (the
  dips reproduce without hidden-sample loss, so this is not M5's fill); the dips were seen in
  `fill: foreground` and appear around the objects, across the plane, and where discs overlap.
- 2026-09-18 — **Two milestones, not one:** the fix cannot be planned before the mechanism is
  known — the candidates range from a one-term correction in the composite to M4's named
  structural follow-up (a depth-gated arrival plane), and choosing between them from a derivation
  would repeat M1's lesson. M6 ships the scene, the oracle, and the ruling; M7 is a stub blocked on
  M6.P2.T2 and is elaborated from the ruling.
- 2026-09-18 — **Bokeh works headless here** (`Nuke17.0 -t`): deep on input 3, `DeepToImage` on
  input 0; a card-over-plane rig read alpha `1.00000` across the halo band. Probe scripts were
  throwaway (`/tmp/bokeh_probe*.py`); P1.T2 makes the helper real.
- 2026-09-18 — **The M4/M5 baseline directories are gone from this host** (`~/deepc-baselines/`,
  `~/deepc-validation/`); M6 recaptures T0 from `46421fa` and no brief may rely on the old paths.
- 2026-09-22 — **M6-T0 recaptured on the reprovisioned host** (Nuke 16.0v9, no AVX2 — see
  `PLAN/DECISIONS/2026-09-22-sdk-is-16.0v9.md` and `…-no-avx2-on-dev-host.md`). Built from `8da4f9d`
  (tree = `46421fa` + the `DEEPC_DEFOCUS_ISA_FLAGS` option) in `build/local-16.0` with
  `-D "DEEPC_DEFOCUS_ISA_FLAGS=-mavx;-mfma"`; `~/deepc-baselines/M6-T0/DeepCDefocus.so` sha256
  `61d63aab…316a9d`, `PROVENANCE.txt`, scene (m)'s three kept EXRs in `renders/`, all logs in `logs/`.
  Doctests 27/27 + 116/116. Harness (a–f, g–n under `hostguard --mem-gb 5.5`, `DEEPC_NUKE` =
  16.0v9): **`PASS=134 FAIL=2 XFAIL=11 SKIP=2`** vs M5's 135/2/11/1 — the one delta is **n4b**
  PASS→SKIP because `~/deepc-baselines/M5-T0/renders` no longer exists (environment, not code). FAILs
  f3e/f3f and all 11 XFAILs unchanged. The `.so` loads headless from its baseline path; `exrdiff.py`
  self-diff 0 ulps. **This is the reference tally for P3.T1** (edited there to 134/2/11/2).
- 2026-09-22 — **M6.P1.T2: Bokeh oracle calibrated** (`2780056`). Bokeh behaves on 16.0v9 as on 17.0
  (deep on input 3). `makeBokeh()` fixes `realWorldLens`, `focalLength=300mm`, `filmFormat=35mm`,
  `worldScale=m`, `depthStyle=Real`, circular, bloom off, and sets `fStop = BOKEH_FSTOP_CAL/size`
  with `BOKEH_FSTOP_CAL = 64.0`; unavailable → `BokehUnavailable` (scene SKIPs). Calibration on
  `groundPlaneRow(200)`, focus 10: size 86 → 36.000 px both nodes, size 43 → 18.000 px both. **Caveat
  for P1.T3:** Bokeh's `fStop·radius` drifted 26.25–27.00 across the sweep (only approximately
  thin-lens), and the two verify sizes are the calibration points themselves — o0b must check the
  1 px agreement at scene (o)'s own sizes, not only these two. Halo non-vacuity: Bokeh alpha exactly
  1.0 over `(61,61)-(195,195)` on the M4 halo rig.
- 2026-09-22 — **M6.P1.T3: scene (o) landed** (`52c02d5`), `PASS=18 FAIL=0 XFAIL=6 SKIP=0`; evidence
  `~/deepc-validation/M6-P1T3/` (`logs/run2.log`, `renders/o5_alpha_diff_defocus_minus_bokeh.exr`).
  Rig: size 64, focus 10, four 28×28 opaque cards at z 8.20 (r 14.05) / 8.55 (r 10.85), pinned cells
  at K=16. Per-pixel bound `4·taps·2⁻²⁴` from the geometry. Pinned dips (XFAIL, outer bound 2×):
  **o1b** plane alone at K=4 1.777e-3 (K=16/64 clean); **o2b** ring round a card, r_plane 8 only,
  5.138e-4; **o3** full rig 3.067e-2 at (171,103), under the isolated card's lower edge; **o3b**
  overlap 2.493e-3; **o4/o4b** sparse twin 4.389e-3 / 2.199e-3. Findings that steer P2.T2:
  (1) **the plane alone does not dip at K=16** (any size 43/64/86, integer or half-integer
  diameter — H2 looks rejected at this rig); (2) **K=64 removes every K=16 dip**, K=4 moves them —
  strong first evidence for H1 (bucket split); (3) complete deep is *worse* than sparse (3.07e-2 vs
  4.39e-3); (4) same vs different pair depth barely moves the overlap (2.49e-3 → 2.39e-3 — H4 weak);
  (5) dips sit in horizontal row bands (96–105, 118–120) on the near-focus side of card blooms;
  colour ratio holds everywhere (worst 1.4e-5), so colour and alpha dip together.
  **Deviations:** Bokeh reads 1 ± 1 ulp (0.999999881..1.000000119 on 339/24336 px), not
  bit-exactly 1.0 — o5's oracle arm is gated on the same term-count ulp bound, consistent with the
  no-8-bit ruling; o0b's size-64 calibration sits exactly at the 1 px limit on row 200 (26 vs 27),
  `BOKEH_FSTOP_CAL` left at 64. Scenes.py gained a two-sided XFAIL helper (FAIL above 2× pin *and*
  FAIL with "DIP GONE" if the dip vanishes). Default scene list is now a–o.
- 2026-09-22 — **M6.P2.T1: probe landed** (`71a3ee3`). `compositePixelCoveragePartitionImpl<kTrace>`;
  `resolveBandCPU(..., CompositeProbe*)` re-composites probed pixels traced after the untraced loop.
  Coordinates are output pixels at render resolution (proxy pixels under proxy; `proxyScale` printed).
  Doctests 27/27 + 117/117; scene (m) 0 ulps vs M6-T0; scene (o) unchanged 18/0/6/0. Evidence
  `~/deepc-validation/M6-P2T1/probe_o3.txt` (+ `probe_o3.py`, the reusable single-cell runner).
  **First reading at o3's worst pixel (171,103), α 0.969:** buckets 5–7 (the card at 8.12, the plane
  at 8.79/9.57) are *saturated* (raw `A_k` 1.33, 1.65) with large co-located residuals (`aRes` 0.25,
  0.75, 0.33) attenuated by `tHead` 0.24 / 0.14 / 0.012; `arrival D = 1.0037 > 1` so the fill never
  runs. Points at H6 (saturate-down) + the residual/`tHead` path, with H5's deficit-only gate
  unable to recover it — a lead for P2.T2 to confirm or reject, not a ruling.
- 2026-09-23 — **M6.P2.T2 MECHANISM RULING: one term, local fix (H6, corrected).** Evidence
  `~/deepc-validation/M6-P2T2/` (`patches/m6-mut-all.patch` + `INDEX.txt`, runtime bitmask
  `DEEPC_M6_MUT`; `logs/sum-*.txt`, `logs/cmp-*.txt`, `probes/`, `scripts/m6exp.py`).
  **Mechanism.** A bucket pools opaque *new* area `C_k` with opaque *co-located* area `D_k` (raw
  `A_k = C_k + D_k > 1`); saturate pulls `A_k` to 1; the composite then splits the saturated 1
  across C:D (`Scatter.h` ~3041 `aRes = a*(colo/(cov+colo))`, `a` clamped at ~2992), so the fit
  term's per-unit opacity `local` drops below 1 (0.753 at (171,103)) and `accAlpha += fit*local`
  under-adds. `D_k` is fed by (i) the rear half of an opaque fragment's bucket split
  (`partitionAlpha(1,f) = 1`, both halves full alpha) and (ii) in complete deep, hidden share-0
  samples, which still deposit full alpha/colour/area (`Scatter.cpp` ~546–549). Every dip pixel
  has arrival `D > 1` (1.0037–1.61), so the deficit-only fill never engages.
  **Classes** (worst `1−α` past bound, K=16): P plane interior (o1b, K=4) 1.777e-3; U1 ring o2b
  5.138e-4; U2 under-edge o3 3.067e-2 (sparse 4.389e-3); O1/O2 overlap 2.493e-3 / 2.199e-3.
  **Grid** (P / U1 / U2 / O1 / O2): H1 K 4/16/64/128 — dips move, 0 at 64/128 all classes; H1
  card-on-centre — U2 *worse* (5.115e-2), O1/O2 → 0; H2 size 86/85/43/42 — tracks size, not
  integer diameter (85 ≈ 86); H3 sparse — U2 3.067e-2 → 2.790e-3, P unchanged; H3 r_plane 0/2/8/16
  — U1 0/0/5.138e-4/0, r_obj<r_plane 8.2e-4/8.9e-4; H4 same depth — unchanged (O1 2.49e-3 →
  2.39e-3); H5a `kFillDeficitTol→0` — unchanged all; H5b symmetric 1/D — 10–100× worse all; H6
  skip-saturate-only — bit-identical (composite clamps at use); **H6' split from raw `A_k` (mut64)
  — 0 / 0 / 0 / 0 / 0**, and (171,103) reads α exactly 1.000000000. (H3 r_plane/r_obj and H4 have
  no reading in classes whose geometry they don't vary — n/a by construction.)
  **Verdicts:** H6 accepted for every class with the corrected term; H1 accepted only as a
  *source* of `D_k` (P, O), rejected as stated; hidden samples accepted as a `D_k` source for U2;
  H2, H4, H5 rejected; H3's premise false.
  **Smallest exact change (mut64):** when `colo > 0` and raw `A_k > 1`, `u = min(A_raw/(C_raw+D_raw),
  1)`, `aCov = u·C`, `aRes = u·D`, colour scaled by `1/A_raw` as saturation does — no new plane,
  no memory. **Measured pin moves under mut64:** a, c, d, n, f, b, e, h, j, k, l bit-identical;
  g1 3.03e-8→1.38e-8, g2 2.09e-6→1.79e-7, g3 2.09e-6→1.19e-7, m3a 2.09e-6→1.19e-7 (all
  improvements); g4/g5, m0b, m1 unchanged; **i7 0.0900→2.96e-3 and i7d 0.0900→9.53e-4 FAIL** —
  their "a²/4 collision residual" signal *is* this defect and must be re-pinned; all six o XFAILs
  read DIP GONE. **Rejected alternatives:** opaque fragment drops its rear deposit (mut16) — U2
  worsens to 1.41e-1; plus culling hidden samples (mut48) — o clean but m0b edge R/A 0.5119→0.8000,
  n1b/n8/n8b/n9 FAIL (8 FAILs in a, c, n).
  **Recommendation for M7: LOCAL** — the corrected C:D split in `compositePixelCoveragePartition()`
  with saturation's colour scaling folded into it; not the depth-gated arrival plane (the fill
  cannot be the lever: arrival > 1 at every dip). M7 plans: re-pin i7/i7d against an independent
  oracle, flip o's six XFAILs to PASS, doctests for the saturated two-area case.
  **Corrections to "What we know" above:** hidden samples are *not* share-0 in their deposits
  (share feeds arrival only); an opaque split's rear carries alpha `w`, not `w·(1−f)`; H6's term
  is the aCov/aRes split, not `fit = min(C_k, freeArea)`; H2's axis is size/CoC slope, not
  integer diameter. The scene (o) o2 cell note in `scenes.py` repeats the share-0 premise and
  should be corrected when M7 touches scene (o).
