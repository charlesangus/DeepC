# Milestone 3: CUDA backend

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5). Do not start until
> Milestone 1 (and ideally Milestone 2) are `done` — this milestone only compiles because M1
> deliberately built the `DEEPC_HD`/`PodBuffer<T>` seam described in
> `PLAN/MILESTONES/M1-deepcdefocus-v1.md`.

Add an opt-in CUDA execution backend for the scatter core, dispatched at runtime with silent
CPU fallback (no "Use GPU" knob until GPU output is validated against CPU output). The CPU path
must remain fully functional and remain the default; this milestone adds a parallel launcher, it
does not replace anything.

Known shape, from the M1 design work (elaborate against current code before committing to it —
this predates any M1/M2 implementation and may need adjustment):
- `src/DeepCDefocusScatter.cu` (new): `scatterBandCUDA(...)` with the same signature as
  `scatterBandCPU` — upload SoA, run per-block privatized bucket tiles or `atomicAdd`, download
  planes. The kernel bodies are `#include`d from the existing `DEEPC_HD`-marked header, not
  reimplemented; the `.cu` file is a launcher only. `PodBuffer<T>` switches its allocator to
  pinned `cudaHostAlloc` for staging buffers used on this path.
- Thrust/CUB (bundled with the CUDA toolkit, so no new dependency) for the depth histogram, scan,
  and any sort work — must stay out of CPU translation units.
- Top-level `CMakeLists.txt`: `option(DEEPC_ENABLE_CUDA OFF)` guarding `enable_language(CUDA)` +
  `find_package(CUDAToolkit)`. A CUDA-toolkit Dockerfile layer in `docker/` extending the
  NukeDockerBuild **Linux** image (mirrors the existing Windows/Qt extension pattern; Linux-only
  is already this node's baseline from M1). `--cuda` flag in `docker-build.sh`.
- Runtime `cudaGetDeviceCount` dispatch, CPU fallback on any failure; a validation mode that
  asserts GPU output matches CPU output within 1e-4 before the path is trusted.

Acceptance sketch:
- A GPU-equipped machine running `DeepCDefocus` with CUDA enabled produces output matching the
  CPU path within 1e-4, materially faster on large-radius/high-K scenes.
- A machine without a usable CUDA device (or `DEEPC_ENABLE_CUDA=OFF`) is unaffected — same
  binary, same CPU behavior as Milestone 1/2.
- `docker-build.sh --linux` (no `--cuda`) still builds the node with CUDA code entirely absent.

## Decisions

(none yet)
