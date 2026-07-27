---
title: DeepCDefocus — deep-input, flat-output defocus node
status: running
current: M1.P3.T5
pm_heartbeat: 2026-07-27T04:10:00-04:00
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
- **Background reading**: `.planning/research/ARCHITECTURE.md` "Pattern 3", `PITFALLS.md`,
  `FEATURES.md` sketched this node originally (tracked as `PLUG-01` in
  `.planning/milestones/v1.2-REQUIREMENTS.md:35`) but the current design supersedes that
  sketch. `PITFALLS.md` has partially aged (its #2 was fixed in `d89b516`) — re-verify its
  claims against current code before relying on them in any task.
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
2026-07-26: build both candidate composites behind an internal flag at M1.P3.T2 and decide from
rendered pixels at M1.P3.T5's gate. See that milestone file's Decisions.)

**Where things stand (2026-07-27T04:10-04:00).** Phases 1.0, 1.1 and 1.2 are complete and committed;
Phase 1.3 is complete except its last task: T0, T1, T6, T2, T7, T8, T9, T3, T10, T11 and T4 are all
done. `current: M1.P3.T5` — wiring the scatter into `engine()` and running validation scenes (a)–(l),
the milestone's core correctness gate, which also decides the bucket composite AND the holdout
interpolant from rendered pixels. `tests/test_defocus_scatter.cpp` now exists (26 cases, mutation-
verified at 40 killed / 6 verified-equivalent over the review's independent 46-mutation list), so every
contract T5 builds on is pinned.
Remaining in this milestone: P3 T5, then Phase 1.4 and Phase 1.5. Eight tasks
have now been added by execution findings (M1.P3.T0, T6, T7, T8, T9, T10, T11, plus the enlarged
M1.P3.T4 test list).
No PR yet — `ship: pr-per-milestone` puts that at M1's verification gate. The branch
`claude/deep-defocus-node-plan-o0ld83` is committed but NOT pushed.

**Carried obligations for whoever resumes:** M1.P3.T4 and M1.P3.T5
must set `ScatterParams::combine` and `pre_merge` explicitly rather than relying on defaults, and T4
owes a parent-reconstruction test, a `tidyOverlapping()` termination fuzz test, the single-fragment
energy identity, a pinned behind-focus regression gate, and the colour:alpha ratio as a standing
invariant (that same clamp-one-of-a-premultiplied-pair defect has now appeared three times); M1.P3.T5
must apply the ray-distance correction in `computeDepthRange()` *and* pass the matching `depthScale` to
the holdout SoA, skip the holdout append entirely when unconnected, avoid computing the matte AOV as
`1 − boundaryT` in float, use a steep ramp for scene (g), report band-alpha and flat-field figures
separately, build the holdout boundary set once per frame rather than per band, and decide both the
bucket composite AND M1.P3.T11's interpolant variant from rendered scenes; M1.P4.T1 must budget on
`(C+3)` plus the holdout term, at the revised 61 B/fragment logical / ≈100 B resident; and the milestone PR body must carry the shipped-node release note from
`PLAN/DECISIONS/2026-07-26-tidyoverlapping-single-pass.md` and
`PLAN/DECISIONS/2026-07-26-volumetric-tidying-semantics.md`.
