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
- **Fractional assignment**: each fragment scatters into its two adjacent buckets by its position
  between bucket centers — `bucketOf()` returns `(index, fraction)`, the two fractions summing to
  exactly 1. The fragment's *alpha* is split in transmittance-preserving form
  (`α_i = 1 − (1−α)^{w_i}`, premultiplied color scaled by `α_i/α`), NOT linearly: a linear alpha
  split cannot survive the front-to-back bucket composite (an opaque fragment split 50/50 returns
  0.75), so linear weights + over-compositing + flat-field `alpha ≡ 1` are mutually exclusive.
  See Decisions. Mandatory fix for layer-transition banding, except at α→1 where the
  transmittance form is necessarily a no-op (also in Decisions).
- **Normalization**: each bucket accumulates `(Σ color·w·vis, Σ alpha·w·vis, Σ w·vis)` —
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
- Manual mode: `radius_px = size · |1 − S/d| · (d<S ? frontMult : backMult)`, clamped to
  `max_radius` — unitless ratio, no mm conversion, and **no halving**: `size` is the blur
  *radius* in pixels at d=∞, matching the knob table's description (see Decisions).
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
| Output | `output_holdout_matte` | Bool + Channel | false / none | flattened holdout coverage AOV; the channel deliberately defaults to none (`Chan_Black`) so ticking the bool can't silently overwrite the node's own alpha — the user picks or creates one |
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

- [x] M1.P1.T1 — CoC and holdout-visibility math
  - files: `src/DeepCDefocusMath.h` (new, header-only, `namespace deepc`)
  - approach: implement `CocParams` + `signedCocPixels()` (incl. `unitScale`) and the
    ray-distance→Z correction per the CoC model above; implement
    `HoldoutVisibility{build, evalBoundaries, interp}` with in-span exponential attenuation.
    Header-only, no NDK/DDImage includes, so it's compilable with plain g++.
  - verify: covered by the M1.P1.T4 unit test task (this task alone has no independent gate).
  - size: L

- [x] M1.P1.T2 — Depth-bucket and compositing math
  - files: `src/DeepCDefocusMath.h` (extends T1's file)
  - approach: implement `DepthBuckets{buildBoundedDeltaCoc, bucketOf → (index, fraction)}`
    (ΔCoC-bounded boundary spacing, monotonic on each side of focus), fractional two-bucket
    assignment, `compositeBucketsFrontToBack()`, the alpha-saturation renormalize (saturate
    down only), and volumetric bucket-boundary transmittance split. Depends on T1's types.
  - verify: covered by the M1.P1.T4 unit test task.
  - size: L

- [x] M1.P1.T3 — Disc kernel LUT
  - files: `src/DeepCDefocusKernel.h` (new, header-only)
  - approach: define `KernelView`, the `KernelSampler` interface
    (`kernel(radiusPx, destX, destY, depth, channelGroup) → KernelView`), and `DiscKernelLUT`:
    radius-indexed at 0.5px steps to `max_radius`, anti-aliased 1px edge, per-entry exact
    normalization (Σw=1), precomputed row spans, Y extent pre-scaled by pixel aspect.
    `destX/destY/depth/channelGroup` are unused in v1 — leave them as real parameters (the M2
    seam), don't drop them.
  - verify: covered by the M1.P1.T4 unit test task.
  - size: M

- [x] M1.P1.T4 — Unit test suite for the math foundation
  - files: `tests/test_defocus_math.cpp` (new), `tests/doctest.h` (new, vendored single-header
    MIT doctest), top-level `CMakeLists.txt` (add `option(DEEPC_BUILD_TESTS OFF)`)
  - approach: doctest-based tests, buildable with plain g++ (no docker, no NDK types anywhere
    in T1–T3's headers). Cover: CoC vs. hand-derived lens table (50mm f/2.8 S=2m d=4m ⇒
    exactly 0.22893772893772894mm — do not re-derive it as 0.228912275, which circulated during
    T4 and is wrong in the 5th digit) plus one non-meter `world_units` case; d=∞/d=S/d≤0 edges; LUT Σw=1 ∀ radii
    (assert `|Σw − 1| < 1e-6`, NOT float equality — measured worst case is 5.1e-08, since each
    entry is normalized by its own double-accumulated sum, not an analytic disc area); AA
    monotonicity; visibility step/product/in-span identities + boundary-LUT interpolation vs.
    exact eval; ΔCoC boundary spacing (monotone depth, bounded step, both sides of focus);
    fractional-assignment partition-of-unity; flat-field alpha ≡ 1 under additive scatter;
    saturation renormalize (alpha>1 in, ≤1 out, color/alpha ratio preserved); volumetric split
    transmittance product identity; composite identities; tidy+sharp-path = sequential over.
    Plus, from M1.P1.T2's review: transmittance-split reconstruction of both alpha and premult
    color at the α=0 and α=1 endpoints; the span-split × bucket-assignment composition identity
    (guards the +8.3% double-count regression); `bucketOfContaining()`'s contract; continuity as
    a volumetric slab slides across a bucket boundary; and — once the bucket-composite alpha
    deficit question is settled — the flat-opaque-across-buckets identity.
  - verify: `cmake -DDEEPC_BUILD_TESTS=ON && make && ctest` (or direct
    `g++ tests/test_defocus_math.cpp -o test && ./test`) — all cases pass.
  - size: M

## Phase 1.2: Node skeleton + flatten path (first NDK compile gate)

- [x] M1.P2.T1 — Iop skeleton, inputs, knobs
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
    `ceil(max_radius + edge_softness/2)`/`ceil((max_radius + edge_softness/2)·aspect)` — the AA
    edge band genuinely reaches half a softness beyond `max_radius`, see Decisions — and caches
    `CocParams`. It does **not** build the
    kernel LUT — that is deferred until the frame's actual CoC range is known (see Decisions);
    this task has no scatter, so it needs no LUT at all. `_request` pulls the full padded deep box with `neededDeepChannels()` plus the holdout
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
  - approach: define `PodBuffer<T>` (thin owning wrapper over aligned host allocation) in this
    header (the POD boundary that becomes the M3 CUDA seam); the `DEEPC_HD` macro is NOT defined
    here — include it from `DeepCDefocusMath.h`, which owns it (see Decisions). Implement
    SoA flattening of deep samples: tidy pre-pass (`deepc::tidyOverlapping()` /
    `deepc::SampleRecord` from `src/DeepSampleOptimizer.h`), CoC evaluation via
    `DeepCDefocusMath.h`, volumetric bucket-boundary split, and pre-merge (adjacent-depth
    samples within `merge_tolerance`, via `DeepSampleOptimizer.h`'s over-composite merge). Input
    is a `std::vector<deepc::SampleRecord>` (already NDK-agnostic), not a live deep pixel fetch,
    so this is testable standalone. Honour `DeepCDefocusMath.h`'s COMPOSITION CONTRACT: a
    volumetric sample goes through `splitSpanAtBoundaries()` + `bucketOfContaining()`, a point
    sample through `bucketOf()` + `fragmentDeposit()` — never both splits, which double-counts
    (measured +8.3%). `splitSpanAtBoundaries()` needs a caller-owned `SpanSplitPart[K+2]`
    (~2.6KB at K=128) — stack-sized in the per-sample path, no heap.
  - verify: covered by the M1.P3.T4 unit test task (synthetic `SampleRecord` vectors in, SoA
    buffers out).
  - size: L

- [ ] M1.P3.T2 — `scatterBandCPU`
  - files: `src/DeepCDefocusScatter.h`/`.cpp`
  - approach: fractional two-bucket deposit per fragment (`w`, `vis` from the LUT), accumulating
    `(Σ color·w·vis, Σ alpha·w·vis, Σ w·vis)` per bucket; weight-plane saturation pass
    (`alpha>1` → rescale down, never up). Implement **both** bucket-combine candidates behind an
    internal flag — plain front-to-back `over`, and coverage-partition driven off the `Σ w·vis`
    plane — per the Decisions entry on the bucket-composite alpha deficit; M1.P3.T5 picks one from
    rendered scenes and the loser is deleted. Per-fragment/per-span bodies marked `DEEPC_HD` so
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
    holdout LUT values match exact in-span exponential eval at several depths. Both bucket-combine
    candidates get their own cases (plain `over` and coverage-partition), since M1.P3.T5 has to
    compare them. **Carried over from M1.P1.T4**, which could not close them: the
    "tidy + sharp-path = sequential over" identity (only the `over` half is testable at Phase 1.1 —
    `tidyOverlapping()` lives in `src/DeepSampleOptimizer.h`, which T1–T3 don't touch), and the
    flat-opaque-across-buckets identity (conditional on the bucket-composite decision, so it is
    written here once M1.P3.T2 has both candidates). Mutation-test any new cases the way T4's
    review did — a suite that survives a deliberately broken header is the defect to avoid.
  - verify: `cmake -DDEEPC_BUILD_TESTS=ON && make && ctest` — all cases pass.
  - size: M

- [ ] M1.P3.T5 — Wire scatter into `engine()` (serial, single frame-wide lock)
  - files: `src/DeepCDefocus.cpp`, `src/DeepCDefocusScatter.h`/`.cpp`
  - approach: `computeDepthRange()` — a separate cheap full-frame `DeepFront/DeepBack/Alpha`
    pass, alpha-weighted, producing the ΔCoC bucket boundaries. Build the `DiscKernelLUT` here,
    immediately after that pass, over the frame's **measured** radius range — `rMax` measured
    (clamped by `max_radius`), but **`rMin` passed as 0** — rather than eagerly over
    `[0, max_radius]`; see Decisions for both halves of this.
    `computeBand(b)` — given the
    global boundaries, fetch source rows for `band ± maxRadius`, run T1's SoA flatten, T3's
    holdout LUT, T2's scatter, saturate, `compositeBucketsFrontToBack()`, write. Both run under
    a single frame-wide lock in this phase (no per-band concurrency yet — that's Phase 1.4) so
    correctness lands before concurrency is introduced.
  - verify: the local build compiles; run validation scenes (a)–(l) from the Design reference
    against this serial implementation in the local Nuke install — all must pass before Phase
    1.4 changes the execution model. This is the milestone's core correctness gate. It also
    **decides the bucket-composite question**: render scenes (c), (f), (g) and (i) through both of
    M1.P3.T2's candidates, pick the one whose pixels are right, record the outcome and the
    comparison in this file's Decisions, and delete the losing path plus its flag before the
    milestone gate.
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
    Also strip M1.P2.T1's placeholder "this is the Phase 1.2 skeleton" line from `node_help()`, and
    override `node_shape()` to `DeepOp::DeepNodeShape()` so this deep-consuming node draws like
    stock `DeepToImage` rather than as a plain 2D box.
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
- 2026-07-26 — The disc LUT is built over the frame's **measured** CoC radius range, not eagerly
  over `[0, max_radius]`. Measured at M1.P1.T3: a full radius-indexed LUT at 0.5px steps costs
  ~8.4MB at `max_radius`=100 (fine) but **~1.0GB at `max_radius`=500** — the knob's documented
  maximum — because total size grows as ~2πR³/3. `max_radius` is meant to be a *bound* on the
  worst case, not an upfront allocation, and a user raising it defensively should not pay a
  gigabyte. Rejected alternatives: radius-proportional step sizes above a threshold (changes the
  specced 0.5px quantization and risks banding); storing only the AA edge band with a constant
  interior (would be smaller *and* faster, but the constant-interior assumption is exactly what
  M2's aberrated/textured kernels break, so it would destroy the v2 seam); lazy per-entry
  construction (needs a lock on the multithreaded band path). Chosen instead: `DiscKernelLUT`
  takes a `[rMin, rMax]` range, and construction moves out of `_validate` to just after
  `computeDepthRange()` — the alpha-weighted depth pass the design already runs per cook — which
  yields the frame's true CoC range for free, keeps the build eager and lock-free, and leaves the
  0.5px steps, exact per-entry normalization, row spans, and the `KernelSampler` seam untouched.
  M1.P2.T2 and M1.P3.T5 approach text amended accordingly.
- 2026-07-26 — **`DeepCDefocus` must never fall through to `Iop::_validate` or `Iop::_request`** —
  a standing invariant for every later phase, now documented in the source. Found at M1.P2.T1's
  review: `Iop::_validate` merges info from all inputs and `Iop::_request` forwards to them, both
  reaching inputs via `Iop::asIop()`, which is a bare `static_cast<Iop*>` (its `dynamic_cast` is
  only an `mFnAssert`, so debug-only). This node's inputs are `DeepOp`s, which are not `Iop`s, so
  the skeleton **core-dumped Nuke 17.0v3** on a plain `n.bbox()` with a `DeepFromImage` attached —
  it compiled, linked and registered perfectly and still killed the host. Minimum-safe `_validate`
  (empty box, `Mask_None`) and empty `_request` bodies added for M1.P2.T2 to replace. The same trap
  applies to the inherited `Iop::input0()`/`Iop::input(int)` accessors: the node defines its own
  `input0()`/`input1()` returning `DeepOp*`, and later phases must use those. Also pinned
  `getViewableModes()` to `eViewableMode2D` (the default offers 3D when an Op's inputs are of a
  different type than itself, i.e. exactly this node).
- 2026-07-26 — Knob ranges stay **soft** (`IRange`'s `force` defaults false, so a user can type
  `max_radius = 5000`); values are clamped at their use sites instead, which M1.P3/M1.P4 must
  actually do. Rationale: the documented ranges are ergonomic slider bounds, and Nuke users expect
  to be able to exceed them, but the design's memory formulas (`K·W·B·(C+2)·4` per band, LUT
  ~2πR³/3) are only bounded if the *use sites* clamp. Evaluate those formulas on clamped values,
  never on raw knob values.
- 2026-07-26 — **Headless Nuke works in this environment**, which was not assumed when the plan was
  written: `/usr/local/Nuke17.0v3/Nuke17.0 -t <script.py>` runs with no GUI and no licensing
  obstacle, and a plugin built into a scratch dir loads via `NUKE_PATH`. Verified at M1.P2.T1 by
  creating the node, reading back all 19 knobs, confirming `test_input` rejects a 2D `Constant` on
  input 0 while accepting a `DeepFromImage`, and validating without crashing. So M1.P2.T2's
  `DeepToImage` parity check, M1.P3.T5's scenes (a)–(l), and M1.P5.T3's committed `.nk` scripts can
  all be scripted and run non-interactively here rather than needing an interactive session.
- 2026-07-26 — Phase 1.1's test suite is **mutation-verified**, and that bar carries to M1.P3.T4.
  T4's review ran 21 deliberate header mutations against the as-written suite and **6 survived** —
  spacing on the unclamped rather than clamped CoC, a 2% error in `partitionColorScale`, a 1e-5
  colour:alpha ratio drift under saturation, wrong channel stride in `saturateBucketPlanes`, wrong
  pixel offset in `compositeBucketsFrontToBack`, and `d=∞` no longer being the far-field limit. The
  causes were the recurring ones: tolerances widened to whatever the implementation emitted rather
  than to a measured error, reference values re-derived with the code's own expression (a
  tautology), a whole-band driver never exercised, and a dead `if` that made the entire back-side
  ΔCoC check unreachable. All fixed; 0 of 21 now survive, at 24 cases / 2578 assertions. Note that
  three bucket-allocation numbers are now pinned as contract (the 8/8 and 15/1 K splits and
  `focusBoundary() == 11` on the saturating rig) — deliberate documentation-with-teeth, so a change
  to the bucket-budget split rule is meant to fail them.
- 2026-07-26 — **Bucket-composite alpha deficit: both candidates get built, and the choice is made
  from rendered pixels at M1.P3.T5, not from identities** (user's call, asked at the Phase 1.1
  boundary). The problem, found at M1.P1.T2's review: distinct opaque fragments whose disc weights
  sum to 1 at a destination pixel but land in *different* buckets front-to-back over-composite to
  `1 − Π_k(1−W_k) < 1` — measured 25.0% alpha deficit across 2 buckets, 31.6% / 4, 34.4% / 8,
  35.6% / 16. It worsens as K rises, so the K knob is not a mitigation, and it hits any receding
  opaque surface (scene (g)) or opaque card interior whose CoC neighbourhood straddles a boundary.
  It is *not* the specified coverage deficit of scene (i): there the coverage plane `Σ w·vis` is
  honestly < 1, here it sums to exactly 1 and the hole is fabricated by the bucketing. The two
  candidates: (1) **plain `over`** as originally specced — conventional, what classical layered DOF
  does, but scenes (c) and (g) fail as written; (2) **coverage-partition** — front-to-back with
  occlusion driven off the `Σ w·vis` plane, where coverage fitting inside the pixel's remaining
  unoccluded area adds and only the excess is `over`-attenuated, which reduces to additive when
  total coverage ≤ 1 (receding plane → alpha 1), to plain `over` when dense (two 50% fog layers →
  0.75), and leaves scene (i)'s honest hole untouched — but has no direct published precedent, so
  it needs empirical proof rather than a derivation. Both go in behind an internal flag at
  M1.P3.T2; M1.P3.T5 renders scenes (c), (f), (g), (i) through each and the winner is recorded
  here, with the losing path deleted before the milestone gate.
- 2026-07-26 — The fractional two-bucket **alpha** split is transmittance-preserving
  (`α_i = 1 − (1−α)^{w_i}`, premult color scaled by `α_i/α`), not linear. Found at M1.P1.T2: the
  design reference specified linear partition-of-unity weights, front-to-back bucket
  over-compositing, AND `alpha ≡ 1` on a flat opaque field (scene (c)) — all three cannot hold,
  since an opaque fragment split 50/50 over-composites to `1 − 0.5·0.5 = 0.75` (measured: 0.910 at
  f=0.1, 0.8125 at 0.25, 0.750 at 0.5). The transmittance form reconstructs both alpha and premult
  color exactly under `over` (worst error 8.3e-08 across the α×f grid), and is the same primitive
  the volumetric span split already needed. The interpolation *weights* stay linear and still sum
  to exactly 1, and the two forms agree to first order at fog alphas (2.5e-3 relative divergence at
  α=0.01), so fog is unaffected; they diverge only as α→1, which is where linear is wrong. Disc
  kernel weights stay linear — those are genuine partial coverage of distinct pixels, not one
  surface duplicated across layers. `expm1`/`log1p` are load-bearing: naive `1−powf(1−a,t)` in
  float is 19% off at α=1e-7. **Known cost:** at α=1 the split degenerates to a no-op (exact
  reconstruction under `over` forces at least one deposit fully opaque), so opaque fragments get no
  banding smoothing and an opaque fragment just past a bucket centre occludes up to one bucket in
  front of it. Mathematically unavoidable — "exact" and "smooth" are incompatible at α=1;
  documented in the header, watch scene (g).
- 2026-07-26 — Volumetric span splitting and fractional bucket assignment are **mutually
  exclusive**, enforced by a COMPOSITION CONTRACT in `DeepCDefocusMath.h`: a sample that went
  through `splitSpanAtBoundaries()` is assigned with `bucketOfContaining()` (whole weight, no
  fraction); only unsplit point samples use `bucketOf()` + `fragmentDeposit()`. Found at M1.P1.T2's
  review: applying both re-buckets split parts by centre into overlapping pairs where accumulation
  is additive rather than `over`, and the deliberately super-linear transmittance alphas then
  over-count — measured **+8.29% on alpha and premult color** at parent α=0.9 for a span straddling
  one boundary. With the contract the error is 0.00% and the result stays continuous (max alpha
  step 6e-08) as a slab slides across a boundary, so nothing is lost by dropping the second split.
- 2026-07-26 — The design reference's per-bucket accumulation triple listed
  `Σ w·α_frag·vis` as its third plane, which is character-for-character the alpha plane. Corrected
  to `Σ w·vis` — pure kernel coverage, independent of alpha. That is the only reading consistent
  with the `K·W·B·(C+2)` memory formula (color + alpha + one more plane) and it is the quantity
  that distinguishes bucketing-induced alpha loss from honest coverage deficit (see Open questions).
- 2026-07-26 — `bucketOf()` measures position between bucket **centres** (for fragment
  assignment); the holdout transmittance LUT needs position between **boundaries**, so M1.P1.T2
  added `locateBoundary()` and M1.P1.T1's `interpAtBucket` comment — which named `bucketOf` — was
  corrected. Verified: fed from `locateBoundary`, `interpAtBucket` reproduces the direct `interp()`
  bit-for-bit and max |vis − exact| is 0.680 vs 0.869 when fed from `bucketOf`. M1.P3.T2/T3 must
  wire the right one, and budget two binary searches per fragment (assignment + holdout vis).
- 2026-07-26 — ΔCoC bucket spacing is uniform in the **clamped** CoC (`min(coc, max_radius)`), a
  case the design reference left open. Near-field CoC is unbounded as d→0, so spacing on the raw
  value lets a saturated plateau consume the whole bucket budget; spacing on the clamped value
  spends exactly one bucket there. Sound because every fragment in the plateau has the identical
  radius and so cannot band. Cost: within-bucket ordering loss applies across the whole plateau,
  and holdout depth resolution is coarse inside it. K is split between the two sides of focus in
  proportion to their CoC spans, so the front and back steps are equal only up to integer bucket
  allocation (measured 1.5× apart at S=10, range [1,100], K=16).
- 2026-07-26 — Output bbox pad becomes `ceil(max_radius + edge_softness/2)` (X) and that value
  times pixel aspect (Y), not `ceil(max_radius)`. Found at M1.P1.T3's review: the anti-aliased
  edge band is centred on the disc rim, so the kernel's true nonzero extent is
  `max_radius + edge_softness/2` — up to 2px beyond `max_radius` at the knob's maximum softness of
  4. The old formula would drop that outermost scattered energy at the frame edge, which is
  exactly the "edge darkening at bbox borders" risk in the register. M1.P2.T2 amended.
- 2026-07-26 — M1.P3.T5 passes `rMin = 0` to `DiscKernelLUT`, using the measured range for `rMax`
  only. The range constructor exists to bound `rMax` (that's where the ~1.0GB worst case lives —
  size grows as ~2πR³/3); the low end saves almost nothing (a `[0,40]` LUT is only ~0.05MB bigger
  than `[2,40]`) while introducing a silent correctness trap: a query below `rMin` is clamped up
  to the `rMin` kernel, so any radius reaching the sampler between the sharp-path threshold and a
  measured `rMin` would be visibly over-blurred. Rather than couple the sharp-path threshold and
  `rMin` as provably-equal numbers, pin `rMin = 0` and keep the parameter as a seam. The header
  documents this as an explicit caller contract.
- 2026-07-26 — `DiscKernelLUT` sizes its buffers exactly before filling rather than growing them
  with `push_back` + `shrink_to_fit`. Found at M1.P1.T3's review: `shrink_to_fit` reclaims memory
  only *after* the peak, and the peak is what OOMs a render thread. Measured on a `[0,200]` LUT:
  peak RSS 134.0MB → 68.1MB and build time 189ms → 97ms (the reallocation copies were half the
  build cost), with byte-identical LUT output. A shared `geometryFor()`/`rowExtent()` pair backs
  both the counting pass and the fill so the reservation is exact by construction.
- 2026-07-26 — Manual-mode `size` is the blur **radius** in pixels at d=∞, not a diameter. The
  design reference's CoC-model block said `coc_px = size·|1−S/d|` and then halved `coc_px` on the
  shared code path (giving radius = size/2), while the knob table described `size` as "radius at
  ∞" — a genuine contradiction found by the M1.P1.T1 implementer. Resolved in favour of the knob
  table, since that string is the user-facing contract and `size=10` producing a 5px blur would
  be the surprising reading. Manual mode therefore skips the `/2` that the physical branch
  applies (physical genuinely computes a CoC *diameter*, so its halving is correct). The CoC
  model block was amended to state this.
- 2026-07-26 — `DEEPC_HD` is defined in `src/DeepCDefocusMath.h`, not in
  `src/DeepCDefocusScatter.h` as M1.P3.T1 originally said. The math header is the lowest-level
  header and is written first (Phase 1.1), and its per-fragment functions are exactly the ones
  the M3 CUDA seam needs annotated — defining the macro in a header written two phases later
  would leave Phase 1.1 either unannotated or dependent on a file that doesn't exist yet. It is
  guarded with `#ifndef DEEPC_HD` so a `.cu` TU can pre-define it; `DeepCDefocusScatter.h`
  inherits it by including the math header. M1.P3.T1's approach text was amended to match.
- 2026-07-26 — C++17 for all `DeepCDefocus` sources: `CMakeLists.txt:22-28` selects C++17 for
  Nuke ≥ 15.0 and C++14 below, and `docker-build.sh:24` targets 16.0/16.1/17.0 only — every
  supported build is ≥ 15, so no C++14 fallback path needs to be maintained in this node.
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
