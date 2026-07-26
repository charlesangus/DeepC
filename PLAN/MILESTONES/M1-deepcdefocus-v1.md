# Milestone 1: DeepCDefocus v1 (CPU, round bokeh, holdout)

Ships a CPU-only `DeepCDefocus` Iop: deep source input, optional deep holdout input, flat 2D
defocused output with plain anti-aliased circular bokeh. This is the whole node's foundation —
M2 (aberrations) and M3 (CUDA) build on the scatter core, kernel seam, and POD-buffer seam this
milestone establishes; do not diverge from the seams described below even though M1 doesn't use
them yet.

## Design reference (read before any task in this milestone)

**Node shape.** `Iop` subclass, `inputs(2)`: 0 = deep source (required, `test_input` →
`dynamic_cast<DeepOp*>`), 1 = deep holdout (optional, `default_input` → `nullptr`,
`input_label` → `""` / `"holdout"`). All switches get `default:` cases and trailing returns.
Output is flat 2D; precompute is lazy per horizontal band, driven off Nuke's own render
threads (see Parallelism below), cache-valid via `Op::hash()` comparison (`_cachedHash`) — not
a `_computed` flag cleared in `_validate`, since `_validate` runs far more often than inputs
actually change. `row.erase(channels)` must be the first statement of `engine()` (sparse deep
pixels otherwise emit garbage).

**Compositing algorithm — depth-bucketed scatter.** Per-destination-pixel fragment lists are
memory-infeasible (~125B fragments at 4K/20spp/r̄=15px). Holdout visibility depends only on
(dest pixel, fragment z), so it is multiplied in *at scatter time*, before a fragment enters any
accumulation structure — this is why full fragment sorting isn't needed. K depth buckets
(default 16, knob 4–128) of premultiplied accumulation planes per band, composited
front-to-back at the end:
- **Boundary spacing**: bounded ΔCoC (not equal population) — CoC-radius step between adjacent
  buckets is uniform on each side of the focal plane. A depth-range histogram pass (alpha-
  weighted, so dense low-alpha fog doesn't skew it) finds the frame's depth/CoC range and clips
  empty ranges; banding visibility is governed by ΔCoC, not sample population.
- **Fractional assignment**: each fragment scatters into its two adjacent buckets via linear
  interpolation weights (partition of unity) by its position between bucket centers —
  `bucketOf()` returns `(index, fraction)`. Mandatory fix for layer-transition banding.
- **Normalization**: each bucket accumulates `(Σ color·w·vis, Σ alpha·w·vis, Σ w·α_frag·vis)` —
  a coverage/weight plane alongside color. Additive premult accumulation is energy-conserving in
  flat regions (kernels are per-radius normalized to Σw=1). Where same-bucket surfaces overlap
  in screen space, bucket alpha can exceed 1 — after scatter, rescale color+alpha by `1/alpha`
  wherever `alpha > 1` (**saturate down, never scale up** — scaling up would hide honest
  coverage deficit). Prerequisite: a per-pixel tidy pre-pass (`deepc::tidyOverlapping()` from
  `src/DeepSampleOptimizer.h`) so coincident same-pixel samples are over-composited before
  scatter, not added — without it even the flat case over-counts, and size-0 = `DeepToImage`
  parity is unachievable.
- Residual approximation: within-bucket loss of ordering between *different-pixel* fragments,
  mitigated by the K knob + ΔCoC spacing.

**Parallelism — ride Nuke's thread pool, no bespoke worker pool.** Frame splits into horizontal
bands (`B = clamp(2·maxRadius, 32, 256)`), computed lazily and claimed by whichever render
thread asks for a row in them first via an atomic band state (`Dirty → InProgress → Done`) plus
a `DD::Image::Lock`/`Condition`. The claiming thread computes the whole band into private bucket
planes and writes a disjoint region of the shared flat frame; other threads waiting on that band
block until `Done`. `_validate` marks all bands `Dirty` on an `Op::hash()` change (cheap — no
compute). `scatterBandCPU` stays thread-agnostic so unit tests can drive it directly with
`std::thread`.
- Band overlap fetch is explicit: a band's dest rows need source samples from
  `band ± ceil(maxRadius·aspect)` rows (up to ~2× duplicated fetches across bands; accepted —
  a whole-frame SoA would cost ~4GB at 4K/20spp/4ch).
- The depth-range pass stays eager (cheap full-frame read, `DeepFront`/`DeepBack`/`Alpha` only),
  run once under the same lock on first `engine()` — bucket boundaries are global.
- Memory-limit knob caps **concurrent in-flight bands**, not workers:
  `scratch/band = K·W·B·(C+2)·4 bytes` (color + alpha + weight plane; ~100MB at K=16,C=4,W=4096,
  B=64; ~800MB at K=128). Cap floors at 1 concurrent band, then shrinks B — never deadlocks at 0.
- `Op::aborted()` checked per source row; an aborted band resets to `Dirty` (never `Done`),
  wakes its waiters, leaves erased/black rows. Every `deepEngine()` bool return must be checked.
- **Hedge (only if profiling shows serialization at Phase 1.4's perf gate)**: Nuke's row
  scheduling isn't guaranteed to spread threads across distinct bands. If it collapses to
  near-serial, add a small `Thread::spawn` prefetch pool that claims `Dirty` bands ahead of the
  render, reusing the same claim/wait primitives — additive, not a rewrite.

**CoC model** (inverse-depth form, robust at d→∞; all physical math in mm):
```
S_mm      = focus_distance · unitScale       // world_units: mm=1 cm=10 dm=100 m=1000 in=25.4 ft=304.8
d_mm      = d · unitScale
cocScale  = (f/N)·f/(S_mm−f)                 // precomputed in _validate; clamp S_mm−f ≥ ε
coc_mm(d) = cocScale · |1 − S_mm/d_mm|       // via invD = 1/d_mm; d=∞ → invD=0
coc_px    = coc_mm / filmbackWidth_mm · format.width()
radius    = (coc_px/2) · (d<S ? frontMult : backMult), clamped to max_radius
```
- `world_units` is required for physical-mode correctness (mixing mm/scene-units is wrong by
  orders of magnitude), default Meters. Near-field CoC is unbounded as d→0 — `max_radius` is
  the guard; `d ≤ 0`/NaN → radius 0.
- Sample depth = midpoint `(zFront+zBack)/2` for bucket/CoC purposes; volumetric samples
  spanning multiple buckets are split at bucket boundaries (transmittance splits analytically as
  `(1−α)^t`) so a deep fog slab doesn't collapse to one hard layer.
- Ray-distance toggle: `z = rayDist · f/√(f² + r_mm²)`, `r_mm` = pixel's radial filmback offset.
- Manual mode: `coc_px = size · |1 − S/d|` (unitless ratio, no conversion needed).
- radius < 0.5px → sharp fast path: fragments composite directly into their own pixel's bucket,
  depth-ordered within the pixel (with the tidy pre-pass, size-0 output is bit-exact with a
  `DeepToImage` flatten). Watch the 0.5–1.5px transition band for chatter; if it chatters, add a
  sharp↔defocused blend zone rather than lowering the threshold.
- Proxy scaling is free via `info_.format().width()`; mm knobs are resolution-independent.

**Holdout mechanics.** Holdout channels requested: only `DeepFront, DeepBack, Alpha`, mirrored
identically in `_request` and fetch. Per dest pixel: samples sorted front-to-back, cumulative
transmittance `T_i = Π_{j<i}(1−α_j)`; `vis(z)` for a containing volumetric sample uses
exponential in-span attenuation `(1−α)^((z−zf)/(zb−zf))` within `[zf, zb]`. Per-fragment binary
search over holdout samples at every covered dest pixel is too slow (O(Σπr²·log H) total) —
instead, per band, precompute a per-dest-pixel transmittance LUT at the K+1 bucket boundaries
(in-span exponential folded in at build time); a fragment's vis is interpolation between its two
boundary values in log-transmittance space (exact for the exponential model) — O(1) per
fragment-pixel. Fragment contribution at scatter time: `V·w·vis`, `alpha·w·vis` — no separate
punch pass needed (fragments behind the holdout attenuate to zero; fragments in front survive,
so a defocused FG blooms over the held-out element with pixel-sharp edges, since visibility is
never blurred). Unconnected holdout / outside holdout bbox ⇒ vis ≡ 1, zero cost. Holdout matte
AOV (optional bool + `Channel_knob`): writes `1 − vis(∞)` per pixel.

**Coverage deficit (specified behavior, not a bug).** Deep input removes depth-edge (AA'd-Z
fringing) ambiguity unconditionally, but only removes disocclusion artifacts if the renderer
emitted hidden samples — not guaranteed (opaque hits normally terminate the ray). A defocused FG
scattering outward with nothing behind it at a dest pixel produces an honest alpha dip ~CoC wide
inside the silhouette. **Default: leave alpha honest** — correct for the comp-over-plate/holdout
workflow this node targets; the saturation rule never scales alpha up to hide this. Document in
node help, plus the renderer-side fix (enable hidden-surface/all-hits deep output). v2
candidates (not built in v1): pull-push fill, or an alpha-renormalize toggle.

**Multichannel.** `Input_ChannelSet_knob` (input 0), default rgba, NOT deprecated
`ChannelMask_knob`. Output channels = selection ∪ `Chan_Alpha` (+ holdout matte channel if
enabled). `Chan_DeepFront/Back` consumed internally, never output. A single
`ChannelSet neededDeepChannels()` (= selection ∪ {DeepFront, DeepBack, Alpha}) is the one source
of truth used by both `_request` and the precompute fetches — request/engine channel divergence
is a known failure class. Selected non-color channels scatter identically to color (documented).
SoA channel-group layout carries an `channelRadiusScale[]` hook (all 1.0 in v1) for M2's
chromatic aberration.

**Kernel seam for v2 (must exist, even though only one implementation ships in v1).** All disc
weights flow through a `KernelSampler` interface:
`kernel(radiusPx, destX, destY, depth, channelGroup) → KernelView{radiusX, radiusY, weights, rowSpans}`.
v1 implementation `DiscKernelLUT`: radius-indexed (0.5px steps to max_radius), anti-aliased 1px
edge (knob), per-entry exact normalization, precomputed row spans for contiguous scatter
(precedent: Blender 2.63 scanline-span disc fill), Y extent pre-scaled by pixel aspect
(anamorphic → ellipse). `destX/destY/depth/channelGroup` are deliberately-unused parameters in
v1 — that unused-ness IS the seam M2 fills in.

**CUDA seam for v3 (must exist, even though only CPU ships in v1).** Scatter core is
`scatterBandCPU(ScatterParams, SampleSoA, HoldoutSoA, BucketPlanes&)` — a free function over POD
SoA buffers (allocated through `PodBuffer<T>`) in its own translation unit; no `DDImage` type
crosses the boundary. Per-fragment/per-span bodies live in a header marked `DEEPC_HD`, so M3
compiles the same source under nvcc and only replaces the loop driver + allocator. Scalar,
`__restrict__`-annotated, auto-vectorization-friendly — no intrinsics (see board-level Context).

**Performance mitigations (all v1)**: row-span scatter (contiguous `__restrict__`
multiply-add, auto-vectorized under per-target `-mavx2 -mfma`, verified with `-fopt-info-vec`);
sharp fast path for in-focus regions; zero-alpha/zero-visibility early-outs before rasterization;
tidy + pre-merge (tidy is correctness-required and always on; pre-merge is a knob, default on,
0.25px tolerance, merges adjacent-depth samples via `DeepSampleOptimizer.h`'s over-composite
merge — lossless when radii are equal); `max_radius` bounds worst case, the LUT, and bbox pad.

**Explicitly out of scope for v1** (document in node help): highlight/bloom controls (node
works in scene-linear premultiplied light; a per-sample pre-gain is the v2-compatible shape if
later demanded — without it, clipped sources produce duller bokeh than photographic reference);
disocclusion fill; aberrations (M2); GPU (M3).

### Knob list (grouped)

| Group | Knob | Type | Default | Range / notes |
|---|---|---|---|---|
| Focus | `coc_mode` | Enum {Physical, Manual} | Physical | |
| Focus | `focus_distance` | Float | 10.0 | 0.001–1e6, log slider, scene Z units |
| Focus | `world_units` | Enum {mm, cm, dm, m, in, ft} | m | scene-unit → mm scale; required for physical-mode correctness |
| Focus | `depth_is_ray_distance` | Bool | false | corrects ray length → Z via focal+filmback |
| Lens | `focal_length` | Float mm | 50 | 8–300 (Physical) |
| Lens | `fstop` | Float | 2.8 | 0.7–32 |
| Lens | `filmback_width` | Float mm | 36.0 | 4–70 |
| Focus | `size` | Float px | 10 | 0–100 (Manual; radius at ∞; unit-agnostic) |
| Bokeh | `front_coc_mult` / `back_coc_mult` | Float | 1.0 | 0–4 |
| Bokeh | `edge_softness` | Float px | 1.0 | 0–4, disc AA falloff |
| Output | `channels` | Input_ChannelSet | rgba | alpha always processed |
| Output | `output_holdout_matte` | Bool + Channel | false | flattened holdout coverage AOV |
| Perf | `max_radius` | Int px | 100 | 1–500; bounds bbox pad + LUT |
| Perf | `depth_layers` | Int | 16 | 4–128 (K buckets; memory scales with K) |
| Perf | `pre_merge` + `merge_tolerance` | Bool + Float | true / 0.25px | 0–2px (tidy pass itself always on) |
| Perf | `memory_limit` | Float GB | 4.0 | 1–64; caps concurrent in-flight bands (floor 1, then shrink B) |

Holdout itself has no enable knob — connection presence enables it.

### Validation scenes (`tests/nuke/*.nk`, committed alongside the node)

a. size=0 / all-in-focus ⇒ pixel-identical to stock `DeepToImage` (needs tidy pre-pass).
b. holdout with everything in focus ⇒ matches `DeepHoldout → DeepToImage`.
c. energy conservation: constant-color constant-depth `DeepCConstant` field ⇒ flat field out at
   any CoC (bbox interior); alpha ≡ 1 exactly.
d. sparse deep input ⇒ zero-sample regions exactly black.
e. occlusion scene (3 depth-separated cards via `DeepMerge` of `DeepCConstant`s): defocused FG
   blooms over sharp mid; BG doesn't bleed through opaque FG; `Escape` mid-cook cancels cleanly.
f. volumetric holdout: fog slab holdout ⇒ partial, depth-graded attenuation.
g. banding: ground plane receding continuously through the focal plane — no visible seams at
   bucket boundaries at K=16; compare K=8 vs K=64.
h. overlap normalization: two opaque same-depth cards overlapping in screen space ⇒ no
   alpha>1, no brightened seam in the overlap band.
i. sparse reveal / coverage deficit: opaque near card over distant card WITHOUT hidden samples
   (not via `DeepMerge` — those scenes carry full occluded information); defocus the near card
   ⇒ documented alpha dip inside the silhouette, no fabricated color.
j. anamorphic: pixel aspect 2 format ⇒ bokeh elliptical by exactly the aspect; bbox pad correct
   in Y.
k. proxy + ray-distance: proxy mode halves radii consistently; `depth_is_ray_distance` on a
   wide-FOV corner matches ground-truth Z within tolerance.
l. small-CoC transition: shallow depth ramp crossing 0–2px CoC ⇒ no chatter/banding at the
   sharp-path / LUT-step transitions.

### Risk register

| Risk | Sev | Mitigation |
|---|---|---|
| Scratch memory at 4K / large radius / large K | High | Band decomposition; B tied to radius; memory-limit caps concurrent bands (floor 1, shrink B); formula incl. weight+vis planes documented in code |
| O(Σπr²) render time | High | Span scatter, tidy+pre-merge, sharp fast path, max_radius clamp, O(1) holdout vis LUT; hard perf gate at Phase 1.4 |
| Compiler fails to vectorize the span loop | Med | `-fopt-info-vec` check is part of the 1.4 gate, not an assumption; escalate per Context |
| Nuke row scheduling serializes band claims | Med | Measured at the 1.4 perf gate; additive `Thread::spawn` prefetch hedge, not a rewrite |
| Bucket quantization artifacts | Med | Fractional two-bucket assignment + ΔCoC-bounded spacing; K knob; banding scene (g) |
| Coverage deficit misread as bug | Med | Specified honest-alpha behavior; node help + renderer-settings docs; validation scene (i) |
| NDK signature drift | Low | Licensed Nuke SDKs are installed locally (`/usr/local/Nuke{16.0v9,16.1v3,17.0v3}`, Phase 1.0) with full NDK headers, so every NDK-facing task compiles locally as it's written, not just at a periodic gate; `docker-build.sh` remains the pre-merge parity check against the release toolchain |
| Lock/abort UX during full-frame precompute | Med | `Op::aborted()` per row; abort invalidates hash-keyed cache, erased rows stay black |
| Request/engine channel divergence | Low | Single `neededDeepChannels()` helper |
| Edge darkening at bbox borders | Low | Output bbox padded by `max_radius` so scattered energy is retained |

## Phase 1.0: Local build setup (no docker in this environment)

`docker` is installed but its daemon isn't reachable here (no `/var/run/docker.sock`), so
`./docker-build.sh` — the repo's only *documented* build path — cannot run in this environment.
This phase gets a working, docker-free compile/link gate in place before any node code is
written, since every later phase needs to build to verify. It does not touch node code.

- [x] M1.P0.T1 — Confirm the local Nuke SDK builds the existing repo clean
  - files: none (verification-only; no source edits)
  - approach: licensed Nuke SDK installs already exist at `/usr/local/Nuke16.0v9`,
    `/usr/local/Nuke16.1v3`, `/usr/local/Nuke17.0v3` (NDK headers under `include/ndk/nuke`,
    `libDDImage.so` and friends at the SDK root). Run
    `cmake -S . -B build/local-17.0 -D Nuke_ROOT=/usr/local/Nuke17.0v3` then
    `cmake --build build/local-17.0 -j"$(nproc)"` against the current (pre-`DeepCDefocus`)
    codebase to confirm the local SDK configures and links every existing plugin. If 17.0v3
    fails for any reason, fall back to `Nuke_ROOT=/usr/local/Nuke16.1v3`.
  - verify: `cmake --build` exits 0 and produces `.so` modules under `build/local-17.0/src`
    (already confirmed working during planning — this task is about locking the command in as
    the milestone's baseline, not discovering whether it works).
  - size: S

- [x] M1.P0.T2 — Document the local build as this environment's dev-loop compile gate
  - files: `README.md` (new short "Local development build" section)
  - approach: document
    `cmake -S . -B build/local -D Nuke_ROOT=/usr/local/Nuke17.0v3 && cmake --build build/local`
    as the fast local iteration loop, distinct from `docker-build.sh` (release packaging,
    Windows cross-compile, and exact multi-version toolchain parity), which requires a docker
    daemon not available in this environment. Note that every task in this milestone that says
    "docker compile gate" means: use the local build day-to-day; run `docker-build.sh` for the
    pre-merge/release check wherever docker is actually available.
  - verify: README section present and its command matches M1.P0.T1's verified invocation.
  - size: S

## Phase 1.1: Pure math foundation (no NDK, unit-testable)

- [ ] M1.P1.T1 — CoC and holdout-visibility math
  - files: `src/DeepCDefocusMath.h` (new, header-only, `namespace deepc`)
  - approach: implement `CocParams` + `signedCocPixels()` (incl. `unitScale`) and the
    ray-distance→Z correction per the CoC model above; implement
    `HoldoutVisibility{build, evalBoundaries, interp}` with in-span exponential attenuation.
    Header-only, no NDK/DDImage includes, so it's compilable with plain g++.
  - verify: covered by the M1.P1.T4 unit test task (this task alone has no independent gate).
  - size: L

- [ ] M1.P1.T2 — Depth-bucket and compositing math
  - files: `src/DeepCDefocusMath.h` (extends T1's file)
  - approach: implement `DepthBuckets{buildBoundedDeltaCoc, bucketOf → (index, fraction)}`
    (ΔCoC-bounded boundary spacing, monotonic on each side of focus), fractional two-bucket
    assignment, `compositeBucketsFrontToBack()`, the alpha-saturation renormalize (saturate
    down only), and volumetric bucket-boundary transmittance split. Depends on T1's types.
  - verify: covered by the M1.P1.T4 unit test task.
  - size: L

- [ ] M1.P1.T3 — Disc kernel LUT
  - files: `src/DeepCDefocusKernel.h` (new, header-only)
  - approach: define `KernelView`, the `KernelSampler` interface
    (`kernel(radiusPx, destX, destY, depth, channelGroup) → KernelView`), and `DiscKernelLUT`:
    radius-indexed at 0.5px steps to `max_radius`, anti-aliased 1px edge, per-entry exact
    normalization (Σw=1), precomputed row spans, Y extent pre-scaled by pixel aspect.
    `destX/destY/depth/channelGroup` are unused in v1 — leave them as real parameters (the M2
    seam), don't drop them.
  - verify: covered by the M1.P1.T4 unit test task.
  - size: M

- [ ] M1.P1.T4 — Unit test suite for the math foundation
  - files: `tests/test_defocus_math.cpp` (new), `tests/doctest.h` (new, vendored single-header
    MIT doctest), top-level `CMakeLists.txt` (add `option(DEEPC_BUILD_TESTS OFF)`)
  - approach: doctest-based tests, buildable with plain g++ (no docker, no NDK types anywhere
    in T1–T3's headers). Cover: CoC vs. hand-derived lens table (50mm f/2.8 S=2m d=4m ⇒
    0.2289mm) plus one non-meter `world_units` case; d=∞/d=S/d≤0 edges; LUT Σw=1 ∀ radii; AA
    monotonicity; visibility step/product/in-span identities + boundary-LUT interpolation vs.
    exact eval; ΔCoC boundary spacing (monotone depth, bounded step, both sides of focus);
    fractional-assignment partition-of-unity; flat-field alpha ≡ 1 under additive scatter;
    saturation renormalize (alpha>1 in, ≤1 out, color/alpha ratio preserved); volumetric split
    transmittance product identity; composite identities; tidy+sharp-path = sequential over.
  - verify: `cmake -DDEEPC_BUILD_TESTS=ON && make && ctest` (or direct
    `g++ tests/test_defocus_math.cpp -o test && ./test`) — all cases pass.
  - size: M

## Phase 1.2: Node skeleton + flatten path (first NDK compile gate)

- [ ] M1.P2.T1 — Iop skeleton, inputs, knobs
  - files: `src/DeepCDefocus.cpp` (new), `src/DeepCDefocus.h` if the repo's node convention
    splits declaration/definition (check an existing deep node for the local pattern)
  - approach: `Iop` subclass, ctor `inputs(2)`; `test_input` → `dynamic_cast<DeepOp*>` for
    input 0, `default_input` → `nullptr` for input 1, `input_label` → `""` / `"holdout"`;
    switches get `default:` cases + trailing returns. Declare the full knob set from the Design
    reference's Knob list table (values unused until later phases wire them up).
  - verify: builds as part of M1.P2.T2's compile gate, using the Phase 1.0 local build
    (`-D Nuke_ROOT=/usr/local/Nuke17.0v3`).
  - size: M

- [ ] M1.P2.T2 — `_validate`/`_request`/`engine` flatten path + NDK compile gate
  - files: `src/DeepCDefocus.cpp`/`.h`
  - approach: `_validate` computes `deepInfo → info_`, pads bbox by
    `ceil(max_radius)`/`ceil(max_radius·aspect)`, caches `CocParams` and rebuilds the kernel
    LUT; `_request` pulls the full padded deep box with `neededDeepChannels()` plus the holdout
    box (depth+alpha only); `engine` does `row.erase(channels)` FIRST, then
    `ensureComputed()` (hash-keyed on `Op::hash()`), then copies rows from the cached planar
    frame. Radius forced to 0 must produce a correct `DeepToImage`-equivalent flatten (tidy
    pre-pass included), sparse input stays black — no scatter code exists yet, so this proves
    validate/request/engine/channel bookkeeping/frame-cache plumbing in isolation.
  - verify: the Phase 1.0 local build (`-D Nuke_ROOT=/usr/local/Nuke17.0v3`) compiles clean;
    load the built node in the local Nuke install, force radius 0, and confirm pixel match
    against stock `DeepToImage` on a simple deep scene (validation scene (a) partially — full
    scene set lands at Phase 1.5). Run `./docker-build.sh --linux` too wherever docker is
    available, for release-toolchain parity.
  - size: L

## Phase 1.3: Scatter core, buckets, holdout (serial band compute)

All four components below are POD-only (no `DDImage`/NDK types cross into
`DeepCDefocusScatter.h`/`.cpp` — that's the M3 CUDA seam), so each is independently unit-
testable with synthetic inputs, the same way Phase 1.1's math was — `scatterBandCPU` in
particular is deliberately "thread-agnostic so unit tests can drive it directly with
`std::thread`." M1.P3.T4 below covers that; T1–T3 don't need to wait for engine wiring to be
verified.

- [ ] M1.P3.T1 — `PodBuffer<T>`, `DEEPC_HD`, and SoA flattening
  - files: `src/DeepCDefocusScatter.h` (new), `src/DeepCDefocusScatter.cpp` (new)
  - approach: define `PodBuffer<T>` (thin owning wrapper over aligned host allocation) and the
    `DEEPC_HD` macro in this header (the POD boundary that becomes the M3 CUDA seam). Implement
    SoA flattening of deep samples: tidy pre-pass (`deepc::tidyOverlapping()` /
    `deepc::SampleRecord` from `src/DeepSampleOptimizer.h`), CoC evaluation via
    `DeepCDefocusMath.h`, volumetric bucket-boundary split, and pre-merge (adjacent-depth
    samples within `merge_tolerance`, via `DeepSampleOptimizer.h`'s over-composite merge). Input
    is a `std::vector<deepc::SampleRecord>` (already NDK-agnostic), not a live deep pixel fetch,
    so this is testable standalone.
  - verify: covered by the M1.P3.T4 unit test task (synthetic `SampleRecord` vectors in, SoA
    buffers out).
  - size: L

- [ ] M1.P3.T2 — `scatterBandCPU`
  - files: `src/DeepCDefocusScatter.h`/`.cpp`
  - approach: fractional two-bucket deposit per fragment (`w`, `vis` from the LUT), accumulating
    `(Σ color·w·vis, Σ alpha·w·vis, Σ w·α·vis)` per bucket; weight-plane saturation pass
    (`alpha>1` → rescale down, never up). Per-fragment/per-span bodies marked `DEEPC_HD` so
    they're header-includable from a future `.cu` file without modification. `__restrict__`,
    scalar, auto-vectorization-friendly — no intrinsics. Signature takes only POD SoA buffers,
    so it's callable from a plain `std::thread` in a test binary.
  - verify: covered by the M1.P3.T4 unit test task (synthetic SoA + kernel LUT in, checking the
    energy-conservation and saturation identities directly, without a live Nuke session).
  - size: L

- [ ] M1.P3.T3 — Holdout SoA and per-pixel boundary-LUT
  - files: `src/DeepCDefocusScatter.h`/`.cpp`
  - approach: build the holdout sample SoA (`DeepFront/DeepBack/Alpha` only) and, per band, the
    per-dest-pixel transmittance LUT at the K+1 bucket boundaries (in-span exponential folded in
    at build time per the Design reference's Holdout mechanics). Unconnected holdout / outside
    holdout bbox short-circuits to `vis ≡ 1` at zero cost.
  - verify: covered by the M1.P3.T4 unit test task (synthetic holdout SoA in, checking LUT
    values against the exact exponential eval).
  - size: M

- [ ] M1.P3.T4 — Unit tests for the scatter core (POD-level)
  - files: `tests/test_defocus_scatter.cpp` (new, doctest, uses the same `DEEPC_BUILD_TESTS`
    option as Phase 1.1)
  - approach: drive T1–T3 directly with synthetic POD inputs (no NDK, no live Nuke session).
    Cover: SoA flatten matches manual tidy+split+merge on hand-built `SampleRecord` fixtures;
    `scatterBandCPU` driven via `std::thread` on a synthetic single-band SoA + `DiscKernelLUT`
    reproduces the energy-conservation identity (flat opaque field ⇒ alpha ≡ 1) and the
    saturation rule (overlapping same-bucket fragments ⇒ alpha rescaled down, ratio preserved);
    holdout LUT values match exact in-span exponential eval at several depths.
  - verify: `cmake -DDEEPC_BUILD_TESTS=ON && make && ctest` — all cases pass.
  - size: M

- [ ] M1.P3.T5 — Wire scatter into `engine()` (serial, single frame-wide lock)
  - files: `src/DeepCDefocus.cpp`, `src/DeepCDefocusScatter.h`/`.cpp`
  - approach: `computeDepthRange()` — a separate cheap full-frame `DeepFront/DeepBack/Alpha`
    pass, alpha-weighted, producing the ΔCoC bucket boundaries. `computeBand(b)` — given the
    global boundaries, fetch source rows for `band ± maxRadius`, run T1's SoA flatten, T3's
    holdout LUT, T2's scatter, saturate, `compositeBucketsFrontToBack()`, write. Both run under
    a single frame-wide lock in this phase (no per-band concurrency yet — that's Phase 1.4) so
    correctness lands before concurrency is introduced.
  - verify: the local build compiles; run validation scenes (a)–(l) from the Design reference
    against this serial implementation in the local Nuke install — all must pass before Phase
    1.4 changes the execution model. This is the milestone's core correctness gate.
  - size: L

## Phase 1.4: Concurrency + performance

- [ ] M1.P4.T1 — Per-band lazy-claim concurrency
  - files: `src/DeepCDefocus.cpp`, `src/DeepCDefocusScatter.h`/`.cpp`
  - approach: replace Phase 1.3's frame-wide lock with per-band atomic state
    (`Dirty → InProgress → Done`) plus `DDImage/Thread.h` `Lock`/`Guard`/`Condition`; a claiming
    render thread computes its band into private bucket planes and writes a disjoint region of
    the shared flat frame; other threads block on that band until `Done`. `_validate` marks all
    bands `Dirty` on an `Op::hash()` change. Memory-limit knob caps concurrent in-flight bands
    per the formula in the Design reference (floor 1 band, then shrink B — never deadlock at 0).
    `Op::aborted()` checked per source row; an aborted band resets to `Dirty`, wakes waiters,
    leaves erased/black rows. Every `deepEngine()` bool return checked.
  - verify: re-run validation scenes (a)–(l) — results must be identical to Phase 1.3's serial
    output (this phase changes execution order only, not results); manually abort a cook
    mid-render and confirm clean recovery (no crash, no stuck lock, next cook succeeds).
  - size: L

- [ ] M1.P4.T2 — Vectorization check and perf gate
  - files: `src/CMakeLists.txt` (add per-target `-mavx2 -mfma` and a `-fopt-info-vec` build
    variant for the scatter TU — final wiring lands in Phase 1.5's T1, this task just needs the
    flag to inspect vectorization), `src/DeepCDefocusScatter.cpp`
  - approach: compile the scatter TU with `-fopt-info-vec` and confirm the row-span multiply-add
    loop actually vectorized; if it didn't, try `#pragma omp simd` before considering any SIMD
    library. Profile a synthetic 2K/20spp scene in the locally-built Nuke. If band concurrency
    collapses to near-serial under Nuke's row scheduling (check via thread activity during the
    profile run), implement the `Thread::spawn` prefetch-pool hedge described in the Design
    reference, reusing M1.P4.T1's claim/wait primitives.
  - verify: `-fopt-info-vec` output shows the scatter loop vectorized (or the omp-simd fallback
    does); the 2K/20spp synthetic scene completes in a time you record in this file's Decisions
    section as the perf baseline for future regressions.
  - size: M

## Phase 1.5: Integration polish

- [ ] M1.P5.T1 — CMake wiring
  - files: `src/CMakeLists.txt`
  - approach: add `DeepCDefocus` to `PLUGINS` and `FILTER_NODES` inside an `if (UNIX)` guard
    (verified pattern: top-level `CMakeLists.txt:11` already opens a UNIX block);
    `target_sources(DeepCDefocus PRIVATE DeepCDefocusScatter.cpp)` following the
    `DeepCShuffle2`/`ShuffleMatrixKnob.cpp` precedent at `src/CMakeLists.txt:128-129` (not the
    FastNoise object-library pattern — different shape); `target_compile_options(DeepCDefocus
    PRIVATE -mavx2 -mfma)` per-target, so the existing global `-mavx` floor is unchanged for
    every other node.
  - verify: the local build (`-D Nuke_ROOT=/usr/local/Nuke17.0v3`) builds `DeepCDefocus` clean
    first; then, wherever docker is available, `./docker-build.sh --linux` builds it clean via
    the release toolchain and `./docker-build.sh --windows` confirms every other node still
    builds with `DeepCDefocus` correctly absent.
  - size: S

- [ ] M1.P5.T2 — Node help, icon, README entry
  - files: `src/DeepCDefocus.cpp`/`.h` (help text knob), `icons/DeepCDefocus.png` (new),
    `README.md`
  - approach: node help text covering holdout semantics, the coverage-deficit/renderer-settings
    note, the linear-light/no-bloom note, and the volumetric-split/midpoint limitation; icon;
    README plugin-list entry following house style (4-space indent, `_` prefix, lowerCamelCase).
  - verify: help text renders in Nuke's node properties panel; icon shows in the node graph;
    README entry present and follows the existing plugin-list format.
  - size: S

- [ ] M1.P5.T3 — Validation scene scripts
  - files: `tests/nuke/*.nk` (new, one per validation scene a–l from the Design reference)
  - approach: commit one `.nk` script per validation scene (a)–(l) so each is reproducible, not
    just run-once-by-hand. Each script should isolate its scenario (e.g. scene (c) is a
    `DeepCConstant` at constant depth through `DeepCDefocus`; scene (e) is three
    depth-separated `DeepCConstant` cards merged via `DeepMerge`).
  - verify: open each `tests/nuke/*.nk` script in Nuke and confirm it reproduces the pass/fail
    check described in the Design reference's Validation scenes list.
  - size: M

## Decisions

- 2026-07-26 — Baseline local build command locked in as
  `cmake -S . -B build/local-17.0 -D Nuke_ROOT=/usr/local/Nuke17.0v3 && cmake --build
  build/local-17.0 -j"$(nproc)"`: verified at M1.P0.T1 against the pre-`DeepCDefocus` tree —
  configure found `libDDImage.so` for Nuke 17.0v3, build exited 0, and produced 27 `.so` modules
  under `build/local-17.0/src`. No fallback to 16.1v3 was needed. `build` is already gitignored,
  so the local build dir never dirties the tree.
- 2026-07-26 — Work branch is `claude/deep-defocus-node-plan-o0ld83`, created off `master` at
  `74ee2d0` at milestone start: the board's Context names it as "the existing branch", but no such
  branch existed locally or on the remote, so the PM created it rather than falling back to the
  default `milestone/<id>-<slug>` convention the user deliberately overrode.

**Verification gate:** the Phase 1.0 local build (`-D Nuke_ROOT=/usr/local/Nuke17.0v3`) green
throughout, plus `./docker-build.sh --linux` (and once, at M1.P5.T1, `--windows`) both green
wherever docker is available before merge; all unit tests in `tests/test_defocus_math.cpp` and
`tests/test_defocus_scatter.cpp` pass; all validation scenes (a)–(l) in `tests/nuke/` pass in
Nuke; `-fopt-info-vec` confirms the scatter loop vectorized (or the omp-simd fallback does); the
2K/20spp perf profile is recorded. PR merges via `/address-pr-review --auto`.
