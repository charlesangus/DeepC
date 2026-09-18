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

- [ ] M6.P1.T1 — Recapture the T0 `.so` and reference renders from HEAD
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

- [ ] M6.P1.T2 — `makeBokeh()`: the Bokeh oracle in the harness, CoC-calibrated to `DeepCDefocus`
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

- [ ] M6.P1.T3 — Scene (o): slanted plane + small objects, complete deep, dips pinned as XFAILs
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

- [ ] M6.P2.T1 — Per-pixel decomposition probe (`DEEPC_DEFOCUS_DEBUG_PROBE`)
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

- [ ] M6.P2.T2 — Run the H1–H6 mutation experiments and write the mechanism ruling
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
  - verify: doctests green; tally `PASS=135+N FAIL=2 XFAIL=11+X SKIP=1` with N/X scene (o)'s
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
