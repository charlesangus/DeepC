---
title: DeepCDefocus — deep-input, flat-output defocus node
status: running
current: null
pm_heartbeat: 2026-09-06T14:25:20-04:00
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
- **Local build is the dev-loop compile gate; the docker build is the RELEASE gate, and it runs
  here.** As of 2026-09-06 `./docker-build.sh --linux` runs green on this machine — Docker Hub's
  blob CDN is unroutable, but `mirror.gcr.io` serves the same images, AlmaLinux 8 stands in for the
  unroutable Rocky 8 package mirrors, and the local Nuke SDK is fed in as a BuildKit named context
  in place of the unroutable Foundry installer download
  (`PLAN/DECISIONS/2026-09-06-docker-linux-gate-runs-here.md`). Two limits stand: NukeDockerBuild
  ships no Dockerfile past Nuke 16.0, so the gate covers **16.0 only**; and `--windows` is
  impossible here, needing the Windows Nuke SDK from that same unroutable host.
  **The local build's binaries are NOT shippable** — they require `GLIBC_2.29` and cannot load on
  RHEL 8, which Nuke 16/17 is supported on; four plugins are affected, `DeepCDefocus` among them
  (`PLAN/DECISIONS/2026-09-06-local-build-is-not-shippable.md`). Cut release artifacts from the
  container build only.
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
  remain the pre-merge/release gate (the release toolchain, plus the Windows cross-compile that
  only NukeDockerBuild can do) — the user runs it before a milestone's PR merges; don't block
  milestone progress on it. The local SDKs cover all three minor versions, which is more version
  coverage than the docker path currently offers.
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

(none — all resolved; the bucket-composite question's ruling — "the bucket composite is
`CoveragePartition`", 2026-08-16 — is in the archived M1 decisions log,
`PLAN/ARCHIVE/M1-history.md`. Project-wide decisions: `PLAN/DECISIONS/INDEX.md`. Current
state and carried obligations: the M1 file's `## Status log`.)
