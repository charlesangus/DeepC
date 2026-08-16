---
title: DeepCDefocus — deep-input, flat-output defocus node
status: running
current: M1.P3.T21
pm_heartbeat: 2026-08-16T12:15:00-04:00
ship: pr-per-milestone
---

# Goal

Ship `DeepCDefocus`: a Nuke deep-input, flat-output defocus/DOF node for the DeepC suite,
whose key differentiator vs. pgBokeh/Bokeh is depth-correct holdouts that stay pixel-sharp
(never defocused) because holdout visibility is evaluated per destination pixel, per fragment
depth, after scatter. v1 (Milestone 1) ships plain anti-aliased circular bokeh, CPU-only,
proving correctness end to end; v2 (Milestone 2) adds aberrations via a separate kernel-field
node; v3 (Milestone 3) adds a CUDA backend behind the seams v1/v2 leave in place.

# Context and constraints

- **New node type for the repo**: `DeepCDefocus` is a `DD::Image::Iop` subclass (not
  `DeepFilterOp` — deep output would be wrong here), the first `Iop` in `src/` that consumes a
  deep input and produces flat 2D output. Model on Foundry's "Deep to 2D Ops" NDK docs; no
  existing in-repo precedent to copy.
- **Design source of truth**: the full architecture (CoC model, depth-bucketed scatter
  algorithm, holdout transmittance mechanics, coverage-deficit spec, multichannel handling,
  performance mitigations, knob list, verification scene list, risk register) lives in
  `PLAN/MILESTONES/M1-deepcdefocus-v1.md` — read it before touching any M1/M2 task. M2 and M3
  build on that same design; their files reference back to it rather than repeating it.
- **Background reading**: `PLAN/REFERENCE/ARCHITECTURE.md` "Pattern 3", `PITFALLS.md`,
  `FEATURES.md` sketched this node originally (tracked as `PLUG-01` in
  `PLAN/REFERENCE/v1.2-REQUIREMENTS.md:35`) but the current design supersedes that
  sketch. `PITFALLS.md` has partially aged (its #2 was fixed in `d89b516`) — re-verify its
  claims against current code before relying on them in any task. These four files are the
  only survivors of the retired GSD `.planning/`/`.gsd/` trees (deleted 2026-08-16; recover any
  of them with `git show d547559:<path>`, the last commit that still had them). Cross-references
  *inside* these four to other `.planning/` paths
  (e.g. `PITFALLS.md` → `codebase/CONCERNS.md`) are dangling by design.
- **Linux-only for this node.** Gated `if (UNIX)` in `src/CMakeLists.txt` (verified: the
  top-level `CMakeLists.txt:11` already has a UNIX block ending at `-mavx`). The Windows
  docker build (`docker/windows.Dockerfile` — not `src/`, as this line said until M1.P3.T7 —
  `docker-build.sh --windows`) must keep building every
  other node unchanged; `DeepCDefocus` is deliberately absent there.
- **No hand-written SIMD, no SIMD library.** The scatter inner loop is a plain
  `dst[i] += w[i]*c` multiply-add over a row span, written so GCC auto-vectorizes it at `-O3`
  with `__restrict__`/FMA. A per-target `-mavx2 -mfma` (via `target_compile_options`, not a
  global bump) applies only to the scatter TU. Escalation ladder if `-fopt-info-vec` shows a
  miss: (a) `#pragma omp simd` first, (b) only then a portable-SIMD library confined to the one
  scatter function, scalar version kept as the CUDA source.
- **No CPU/GPU portability layer** (Kokkos, SYCL, Alpaka) despite the CUDA goal (M3) — too
  heavy for a `dlopen`'d Nuke plugin. Substitute: a `DEEPC_HD` macro (`__host__ __device__`
  under nvcc, empty otherwise) on header-only per-fragment/per-span kernel functions, plus a
  `PodBuffer<T>` owning-allocation wrapper used for every SoA/plane buffer from day 1 so M3
  swaps only the allocator and launcher, not the kernel source. Thrust/CUB are M3-only (ship
  with the CUDA toolkit) and must never leak into CPU translation units.
- **Library choices**: `doctest` (single vendored header) for `tests/`; `Imath` (already an NDK
  dependency) for incidental vector math — do not add Eigen or glm. Reuse
  `deepc::tidyOverlapping()` / `SampleRecord` from `src/DeepSampleOptimizer.h` (verified
  present) rather than reimplementing sample tidying/merging.
- **Local build is the dev-loop compile gate in this environment; docker is not available
  here.** `docker` is installed but its daemon isn't running in this environment (no
  `/var/run/docker.sock`), so `./docker-build.sh` cannot be used for iterative compiles here.
  However, **licensed Nuke SDK installs are present locally** at `/usr/local/Nuke16.0v9`,
  `/usr/local/Nuke16.1v3`, and `/usr/local/Nuke17.0v3` — full NDK headers plus `libDDImage.so`
  et al. — so NDK-facing code compiles and links directly via
  `cmake -S . -B build/local -D Nuke_ROOT=/usr/local/Nuke17.0v3 && cmake --build build/local`
  (verified: builds the existing repo clean, warnings only). Milestone 1's Phase 1.0 sets this
  up formally; every later task's "docker compile gate" language should be read as "this local
  build" for day-to-day iteration in this environment.
  Headless Nuke also works here (verified at M1.P2.T1):
  `NUKE_PATH=<dir> /usr/local/Nuke17.0v3/Nuke17.0 -t <script.py>` loads a locally-built plugin with
  no GUI and no licensing obstacle, so every in-Nuke verification in this plan can be scripted
  rather than run by hand. `./docker-build.sh --linux`/`--windows`
  remain the pre-merge/release gate (exact production toolchain across all three Nuke minor
  versions, plus the Windows cross-compile that only NukeDockerBuild can do) — run it wherever
  docker is available (e.g. the user's machine or CI) before a milestone's PR merges; don't
  block milestone progress on it being runnable from this session.
- **House style**: 4-space indentation, `_` member prefix, lowerCamelCase (per README
  conventions).
- **Branch**: all work happens on the existing branch `claude/deep-defocus-node-plan-o0ld83`
  (not a fresh `milestone/<id>-<slug>` branch per milestone — this was locked in with the user
  and deliberately overrides the default per-milestone branch convention). Still gate each
  milestone with a PR at its verification gate per `ship: pr-per-milestone`.

# Board

| ID | Milestone                                          | Status | File |
|----|-----------------------------------------------------|--------|------|
| M1 | DeepCDefocus v1 (CPU, round bokeh, holdout)          | doing  | [M1-deepcdefocus-v1.md](PLAN/MILESTONES/M1-deepcdefocus-v1.md) |
| M2 | DeepCKernelField + aberrations                       | todo   | [M2-kernelfield-aberrations.md](PLAN/MILESTONES/M2-kernelfield-aberrations.md) |
| M3 | CUDA backend                                         | todo   | [M3-cuda-backend.md](PLAN/MILESTONES/M3-cuda-backend.md) |

# Open questions

(none awaiting a human answer — the bucket-composite alpha deficit found at M1.P1.T2 was answered
2026-07-26 by building both candidates behind an internal flag and deciding from rendered pixels;
**M1.P3.T17 did that on 2026-08-16 and kept `CoveragePartition`**. See that milestone file's Decisions.)

**Where things stand (2026-08-16T12:00-04:00).** Phases 1.0, 1.1 and 1.2 are complete and committed;
Phase 1.3 has T0, T1, T6, T2, T7, T8, T9, T3, T10, T11 and T4 done. **M1.P3.T5's wiring is landed and
the node now genuinely defocuses**, but T5 is deliberately left OPEN: its review found that scene (a)'s
size-0 parity gate fails (up to 2.4e-01, ~12% of pixels) because same-pixel fragments collide in one
bucket and the composite clamps coverage and ordering away together — a defect in the bucket
accumulation that T5 merely put on the cook path — and that abort recovery cannot be exercised headless
at all. T14 has since fixed something wrong since the project began — `CMAKE_BUILD_TYPE` was unset, so the
CMake build had never passed an `-O` flag and the shipped plugin contained **zero** vectorized loops
against 402 at `-O3`. T13 and T15 have since closed every same-pixel bucket-collision hole: size-0 parity goes from 2.6e-01
with ~100% of pixels wrong to ≤5.8e-07 with none, across point, volumetric and mixed content, with the
holdout connected as well as disconnected. ~~Validation scene (l) is closed with them.~~ — retracted at
T16; scene (l) fails on a *different* mechanism T13 never reached (see below).
**T12 was split four ways on 2026-08-16** (it bundled a harness build, a twelve-scene sweep, two
independent decisions and two source deletions — past the sizing rule): **T12** = headless harness +
validation scenes (a)–(f), **T16** = scenes (g)–(l), **T17** = the bucket-composite bake-off + delete
the loser, **T18** = the holdout-interpolant bake-off + delete the losers. They run in that order.
**T12 and T16 are both done and committed, and the full scene sweep (a)–(l) is now in place.**
`tests/nuke/` is the milestone's validation gate — one command, numeric per-check gates, non-zero exit
on failure, with `combine`/`holdoutInterp`/`K`/`pre_merge` as parameters so T17/T18 drive the same
scenes at different settings. Full run: **PASS=74 FAIL=3 XFAIL=16 SKIP=1**.

**T17 is done: the bucket composite is decided and the loser is deleted.** `CoveragePartition` wins
on scenes (c)/(f)/(g)/(i) rendered through both candidates post-T19, every render setting `combine`
explicitly. The deciding readings: harness `f1` — opaque point strips behind a fog holdout at size 0,
the simplest content the node has — is 4.367e-08 under the winner and a hard-FAILing **2.500e-01**
under plain `over`, whose mechanism is now *derived and confirmed at every strip* (an opaque
fragment's transmittance split is a no-op, so `over` composites its two bucket deposits as
independent layers and renders `2·vis − vis²` instead of `vis`, +50% relative at vis=0.5); and scene
(g)'s ramp, where the winner converges in K (−1.05/−0.44/−2.8e-05% at K=8/16/64) while `over` diverges
(−0.28/−2.15/−7.79%) and is over 1/255 on **112 of 112** interior rows against 51 and 23. Scene (c)
does not discriminate and neither does scene (i). **Scene (l) favoured the loser at every check** and
the first explanation offered for that (one-sided saturation flattering an opaque field) was
**proposed and refuted** by re-rendering at α=0.5 and α=0.25; the surviving explanation is that both of
`over`'s failure modes are quenched together below ~2.5 px, and its advantage never exceeded ~1 8-bit
code value. The bake-off also found **a new, larger residual on the winner**: the same ramp at α<1
loses **16.1%** of its alpha at K=16/α=0.90 and diverges in K to 26.6% at K=128, where the opaque twin
loses 0.4%. Three controls attribute that to the transmittance split's recombination and exclude the
kernel, the sharp path and depth quantisation; the exact pooling term is unproven and said to be so.
It is pinned as a banded, mutation-tested XFAIL (`g4`) and owed a ruling at Phase 1.4/1.5 — it is now
the largest known error in the shipped node. Harness **PASS=76 FAIL=0 XFAIL=12 SKIP=1, exit 0**, with
every surviving reading bit-identical to the pre-deletion run.

**T20 is done: the largest known error in the shipped node is FIXED, and it was the composite, not the
split.** The α<1 receding-content residual T17 found is gone at its root. Lead (b) — the linear alpha
split — was built and **disproved**: exact on a mosaic, but in the `excess` regime it makes one surface
occlude itself, reading 0.875 against a true 1.0 on two same-pixel layers, which fails validation scene
(a)'s 2e-07 parity gate by five decades. `partitionAlpha()` is untouched. What was wrong is that
`compositePixelCoveragePartition()` carried ONE pooled transmittance for a claimed area that a depth
ramp makes a mosaic of tiles: a co-located deposit is now attenuated by the tile *its own head* claimed,
and a residual's occlusion is *subtracted* from the claimed mean in proportion to the area it covers
rather than multiplying the whole of it. One extra scalar, no new plane, no seam touched. On T17's own
isolated rig the deficit goes to **exact** at every N, α and split fraction (was −17.4% at α=0.90/N=16,
−38.9% at split fraction 0.25); harness `g4` **0.1607 → 0.0543**, re-pinned in the same change against
the rig as an independent oracle; and the **K-divergence is gone** (α=0.50 swept +0.33 → −26.62% at
K=2…128, now +1.37 → −1.01%). The surprise, derived rather than guessed: the *opaque* twin improved five
to six decades too — saturation pushes part of a bucket's alpha into the residual term even at α=1 —
which **retired the `g1`/`g2`/`g3` XFAILs** and closed scene (g)'s own stated seam criterion at
every K. Harness **PASS=83 FAIL=0 XFAIL=5 SKIP=1, exit 0**; both unit suites green. The remaining 5.4%
at `g4` is a *different* mechanism — one bucket pooling a head and a rear at unequal per-unit opacity,
i.e. `f3c`/`f3d`'s term, information lost at accumulation and not recoverable by any per-bucket
composite rule — and is not claimed fixed.

**T20 has been independently reviewed (2026-08-16): the ruling is upheld and the fix is genuine, with
one regression found and pinned.** Reproduced against an independently written area-model oracle: the
isolated rig is exact, lead (b) is disproved (and is worse than T20 reported — 0.859375, not 0.875),
all seven `g4` mutation readings reproduce exactly, and re-rendering the current checks against the
pre-T20 plugin FAILs `g1`/`g2`/`g3`, so the retired checks have teeth. **What the fix cost:** the
composite carries only ONE head tile, so a bucket that both claims area and continues a residual chain
must discard one — and **two multi-part parents at overlapping depth ranges now read up to +18.3%
HIGH** (377/525 swept cells over +0.5%, saturating to alpha 1 at α=0.90) where pre-T20 they read 4–16%
low. That is the honest-alpha contract's forbidden direction; T20's volumetric check covered only the
*non-overlapping* mosaic, which is exact. Not fixed — both alternatives are worse trades — but pinned
as a band in the unit suite and carried in the milestone's risk register. Four claims were also
corrected in place: only **three** XFAILs were retired (`l3` was still an `expectedFailure`, now a
plain check); the remaining `g4` term is triggered by any split fraction ≠ 0.5, not by *differing*
fractions; `claimA = cov` is caught by `g1`/`g2`/`g3` as well as `g4`; and `a3` (5.960e-08 →
1.192e-07) and `l1` moved besides `g4`. Two large **pre-existing** upward errors are also now on the
record (+94.8% in the `excess` regime, +8.3% on a free/claimed straddle), so "+1.37% is the worst
positive excursion" describes the K sweep, not the composite.

**T19 is done: the harness is now green end to end** — PASS=77 FAIL=0 XFAIL=18 SKIP=1, exit 0, for the
first time in this milestone. T16's three FAILs were one real node defect: `DiscKernelLUT` quantised
radius onto a uniform 0.5 px grid, so adjacent scanlines straddling a bin edge rasterised different
discs, leaving a one-scanline **20% dark trough** at CoC radius 0.762 px (35% radial). T19 replaced the
grid with an adaptive `h(r) = r²/512` one, which bounds the deficit *uniformly across radius* rather
than at one radius — 2.009e-01 → 2.176e-03 against a 3.9e-03 gate, at +4.8 ns/fragment and a flat
+197 KB, with vectorization unchanged. Interpolating between LUT entries was prototyped and lost on
both accuracy and cost. Two residuals it *exposed* (the bucket composite showing through at the
sharp↔disc threshold, and the CoC field's own interior extremum) are bounded XFAILs, each shown
pre-existing rather than introduced.

The two reviews also corrected four plan errors, all of the same shape — a figure recorded from a
measurement that never reached the phenomenon it claimed to bound: the different-split-fraction
residual is *not* candidate-independent (it is signed and flips between the two bucket-composite
candidates, so it is evidence for T17 rather than noise); the M1.P3.T10 erasure was understated (it
starts at the bracket's lower boundary, ≈100% of it, not 82%); **T13's "scene (l) is closed" is
retracted** (its synthetic corpus never crossed the 0.75 px bin edge); and `pre_merge` is both
reachable at its shipping default *and* lossy there, not "lossless when radii are equal" — which
earns `merge_tolerance`'s default a review at Phase 1.4.
Note T5's size-0 parity clause now passes; its abort-recovery clause remains unverified and needs an
interactive pass, since headless Nuke cannot trigger a recoverable mid-cook cancel — validation scene
(e)'s `Escape` clause (harness check `e4`, SKIPped) is blocked on the same thing and is owed by the
same interactive pass.
**Standing lesson, now recorded three times over:** every wrong figure in this plan has been a
measurement that never reached the phenomenon it claimed to bound — T13's synthetic corpus never
crossed the kernel-bin edge, two `pre_merge` probes each happened to group nothing, and at T19 two
re-pinned constants were *correct* while their stated mechanisms were fabricated. So: mutation-test
every check (one nobody has made fail proves nothing), band pins rather than bounding them on one side,
validate re-pins against an independent oracle rather than against the new output, and give every XFAIL
a hard outer bound so it cannot swallow a later regression.

Remaining in this milestone: P3 T21, T18, then Phase 1.4 and Phase 1.5. Eighteen tasks
have now been added by execution findings (M1.P3.T0, T6, T7, T8, T9, T10, T11, T12, T13, T14, T15, T16,
T17, T18, T19, T20, T21, plus the enlarged M1.P3.T4 test list).
No PR yet — `ship: pr-per-milestone` puts that at M1's verification gate. The branch
`claude/deep-defocus-node-plan-o0ld83` is committed but NOT pushed.

**Carried obligations for whoever resumes:** M1.P3.T4 and M1.P3.T5
must set `pre_merge` explicitly rather than relying on defaults (the same clause used to name
`ScatterParams::combine`; M1.P3.T17 deleted that field, so only `pre_merge` and `holdoutInterp`
remain), and T4
owes a parent-reconstruction test, a `tidyOverlapping()` termination fuzz test, the single-fragment
energy identity, a pinned behind-focus regression gate, and the colour:alpha ratio as a standing
invariant (that same clamp-one-of-a-premultiplied-pair defect has now appeared three times); M1.P3.T5
must apply the ray-distance correction in `computeDepthRange()` *and* pass the matching `depthScale` to
the holdout SoA, skip the holdout append entirely when unconnected, avoid computing the matte AOV as
`1 − boundaryT` in float, use a steep ramp for scene (g), report band-alpha and flat-field figures
separately, build the holdout boundary set once per frame rather than per band, and decide
M1.P3.T11's interpolant variant from rendered scenes (the bucket-composite half of that clause was
discharged by M1.P3.T17); M1.P4.T1 must budget on
`(C+3)` plus the holdout term, at the revised 61 B/fragment logical / ≈100 B resident. **M1.P3.T17's
α<1 receding-content residual is closed by M1.P3.T20** (fixed in the composite; `g4` re-pinned at
0.0543 ± 0.004). It ran before T18 because the fix moved every rendered pixel and T18 decides from
rendered pixels — **T18 must re-render rather than quoting any figure from before `d06c4da`**.
And the milestone PR body must carry the shipped-node release note from
`PLAN/DECISIONS/2026-07-26-tidyoverlapping-single-pass.md` and
`PLAN/DECISIONS/2026-07-26-volumetric-tidying-semantics.md`.
