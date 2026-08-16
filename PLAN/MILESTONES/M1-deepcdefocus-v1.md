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
  **READ THAT JUSTIFICATION AS `over`-SPECIFIC SINCE M1.P3.T17.** "Cannot survive the front-to-back
  bucket composite" was written when the composite *was* plain `over`; T17 deleted `over` and the
  shipped composite is `compositePixelCoveragePartition()`, which reconstructs a split fragment from
  the area planes rather than from the split's `over` identity. The transmittance form is therefore
  no longer *required* by the composite. **M1.P3.T20 (2026-08-16) settled what to do about that: the
  split is UNCHANGED and the composite was fixed instead.** The α<1 ramp deficit was never the
  split's — it was the composite carrying one pooled transmittance for a claimed area a depth ramp
  had made a mosaic. The linear split lead was built and **disproved**: it is exact on a mosaic but
  breaks the dense/sharp `over` identities (two same-pixel layers read 0.875 against a true 1.0, i.e.
  it would fail validation scene (a)'s parity gate outright). See Decisions, 2026-08-16, M1.P3.T20.
- **Normalization**: each bucket accumulates `(Σ color·w·vis, Σ alpha·w·vis, Σ w·vis)` — a
  coverage/weight plane alongside color. Since M1.P3.T9 that `Σ w·vis` is **two** planes, not one:
  *new area* (deposits that claim area a pixel didn't have) and *co-located area* (deposits carrying
  alpha at area already claimed — a fractional split's farther-bucket deposit, and every non-head part
  of a split parent). Their sum is the original single plane; splitting them is what lets the composite
  resolve a residual as `aRes/D_k` exactly at any radius spread. See Decisions. Additive premult accumulation is energy-conserving in
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
- Memory-limit knob caps **concurrent in-flight bands**, not workers. NOTE the formula below counts
  only the bucket planes; M1.P3.T1 measured the SoA fragment buffers at 69 B/fragment (~1.49GB for a
  4K band at 20spp) — **61 B/fragment (~1.32GB) since M1.P3.T10 removed the boundary pair** — which
  dominates them — budget on the combined total, see Decisions:
  `scratch/band = K·W·B·(C+3)·4 bytes` (color + alpha + the two area planes — new area and,
  since M1.P3.T9, co-located area; ~117MB at K=16,C=4,W=4096,B=64; ~940MB at K=128). Cap floors at 1
  concurrent band, then shrinks B — never deadlocks at 0.
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
instead, per band, precompute a per-dest-pixel transmittance LUT at **the holdout's own K+1
boundaries — `HoldoutBoundaries`, uniform in Z over the frame's measured depth range, NOT the
ΔCoC bucket boundaries** (in-span exponential folded in at build time); a fragment's vis is
interpolation between its two boundary values in log-transmittance space — O(1) per
fragment-pixel, off a closed-form index. The two sets share only the count K+1 (so LUT memory is
`(K+1)·W·B·4`); ΔCoC spacing bounds *banding*, a CoC criterion, and sampling this LUT at it made
an opaque card at z=50 start occluding at z=10.9 (see Decisions, 2026-07-27, M1.P3.T10). The
log chord is exact only where no holdout span edge falls strictly inside a bracket; elsewhere it
is the monotone chord, whose worst case `(T0−T1)/2` is irreducible with two boundary values, so
*placement* was the fix — but the shipped chord does **not** attain that bound on an opaque step,
and the leftover is only partly irreducible: it floors `log T` at `kMinTransmittance`, so vis
collapses to ~0 across almost the whole bracket, always toward camera. **A holdout card still
fully erases genuinely-unoccluded source geometry for one bracket in front of it** — 5.07 of the
6.19-unit bracket at K=16 on the default rig, 2.28 at K=32, 0.89 at K=64, i.e. ≈`(depthRange)/K`.
**Understated — corrected 2026-08-16 at M1.P3.T12's review.** Measured end to end on the harness,
the erasure begins at the bracket's *lower boundary itself*, not partway in: vis is 1.0000 at −2% of
the bracket and already 0.2512 at +2%, 0.0010 at +10%, 0 beyond, decaying as `10^(−30·frac)` — i.e.
exactly `kMinTransmittance = 1e-30` (`src/DeepCDefocusMath.h:61`) flooring `log T`. So it is ≈100%
of the bracket, not the 82% the figures above imply, and it scales as `depthRange/K` exactly
(bracket 2.500/1.250/0.625/0.3125 at K=8/16/32/64, same fraction erased at every K). M1.P3.T18 must
judge from these numbers, not the older ones. Harness comparison across the three interpolants:
LogChord erases 78% of the bracket, MidpointStep 30%, LinearInT none outright but ramps 0.95→0.25
across it.
That is the standard holdout setup (an element sitting just in front of the held-out object), it
is a visible artefact, and the interpolant half of it is cheaply reducible — an open follow-up,
see Decisions. Fragment contribution at scatter time: `V·w·vis`, `alpha·w·vis` — no separate
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
| Perf | `pre_merge` + `merge_tolerance` | Bool + Float | true / 0.25px | 0–2px, in **CoC-radius pixels** (tidy pass itself always on) |
| Perf | `memory_limit` | Float GB | 4.0 | 1–64; caps concurrent in-flight bands (floor 1, then shrink B) |

Holdout itself has no enable knob — connection presence enables it.

### Validation scenes (`tests/nuke/*.nk`, committed alongside the node)

a. size=0 / all-in-focus ⇒ pixel-identical to stock `DeepToImage`, scoped as follows (measured at
   M1.P2.T2 and re-measured at its review):
   - **Point-sample input: ≤2e-07 absolute (a couple of ULP) — NOT 0 ULP.** ~~bit-exact (0 ULP)~~ was
     the gate from M1.P2.T2 until M1.P3.T14's review, and it held across 20 samples/pixel, alphas from
     1e-7 to 1.0, opaque-in-front, sparse, 16-thread cooks, downstream crops, channel subsets, pixel
     aspect 2 and proxy 0.5 — **but only of the `flattenPixel()` path, which M1.P3.T5 retired.** The
     shipping architecture cannot be bit-exact by design: every point fragment goes through
     `bucketOf()`'s **mandatory fractional two-bucket split**, which is the design's own required fix
     for layer-transition banding, and whose reconstruction is mathematically exact but not bit-exact
     unless a sample's depth lands exactly on a bucket centre. M1.P1.T2 already measured that
     reconstruction at **8.3e-08 worst across the α×f grid**; the end-to-end figure is consistent
     (max 1–2 ULP, ~3% of samples at 1 spp). Verified flag-independent — bit-identical across
     unoptimised, `-g`, and `-O3` builds. Removing the split is not available: M1.P3.T13's review
     measured that whole-weight assignment on the sharp path **defeats the K knob**, which is the
     design's stated mitigation for within-bucket ordering loss. So the tolerance is the honest gate.
   - **Coincident-depth samples: assert ≤2e-07 absolute, NOT a ULP bound.** The mandated tidy pass
     over-composites samples sharing an exact `[zFront, zBack]` before the flatten while
     `DeepToImage` composites them individually, and `over` is associative in exact arithmetic but
     not in float. The ULP residual *grows with coincident-sample count* (1 ULP at 2 samples, 3 at
     5, 6 at 20), so a "≤N ULP" gate is only meaningful against a named scene with a stated sample
     count; the absolute tolerance held everywhere.
   - **Overlapping volumetric spans: a real parity gate, ≤2.4e-07 absolute** — settled at M1.P3.T0,
     which fixed `tidyOverlapping()`'s merge to the OpenEXR mixture model. **The gate must pin
     `volumetric_composition` ON** on the `DeepToImage` it compares against (that is Nuke's
     default); with the knob off, Nuke selects the plain-`over` form this node deliberately moved
     away from, and the node disagrees at ~1e-02 by design.
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
| Lock/abort UX during full-frame precompute | Med | `Op::aborted()` per row; abort invalidates hash-keyed cache, erased rows stay black. Verified at M1.P2.T2: an aborted cook leaves the frame fully black, publishes nothing, and the next cook is bit-exact |
| Upstream deep failure cached as a valid black frame | Med | The NDK does **not** propagate an upstream `doDeepEngine()` failure — `DeepOp::deepEngine()` returns *true* with an empty plane, so the defensive bool check can never fire and a silently-empty upstream is published as a valid all-black cache until the hash changes (stock `DeepToImage` has the same blind spot but no cache, so it recovers next cook). Only `Op::aborted()` is a real signal. Found at M1.P2.T2's review; revisit when M1.P4.T1 rebuilds the cache |
| `tidyOverlapping()` split pass is superlinear | High | Measured ≈O(n³·⁷) — 9.96ms/pixel at 32 mutually overlapping spans, which makes a fog frame unrenderable rather than merely slow. Rewritten single-pass at M1.P3.T6, before T5's scenes need it |
| SoA fragment memory outside the `memory_limit` formula | Med | 113 B/fragment resident ⇒ ~2.4GB for one 4K band at 20spp, dwarfing the bucket planes (≈100 B / ~2.1GB since M1.P3.T10 dropped the boundary pair). Formula extended at M1.P3.T1 (see Decisions); `reserveFragments()` collapses the capacity slack |
| Non-terminating sample tidying on volumetric input | High | `deepc::tidyOverlapping()` looped forever on *any* overlapping volumetric pair — fixed during M1.P2.T2 (see Decisions); termination fuzz test added at M1.P3.T4 |
| Holdout LUT erases fragments far in front of the holdout | High | The design's K+1 ΔCoC bucket boundaries are the wrong basis for depth occlusion — a solid card at z=50 occluded from z=10.9, 98% erasing a fragment at z=15. Found at M1.P3.T3's review; decoupled boundary set at M1.P3.T10 |
| Optimisation flags never present in the built plugin | High | `CMAKE_BUILD_TYPE` unset since the project began, so the CMake build emits no `-O` at all: 0 vectorized loops and 0 FMA instructions in the shipped `.so` against 396 and 97 at `-O3 -mavx2 -mfma`. Every `-fopt-info-vec` result on record describes a hand-compiled object, not the module. Found at M1.P3.T5's review; fixed at M1.P3.T14, which must precede M1.P4.T2's perf gate |
| Same-pixel fragments colliding in one bucket | High | Additive within-bucket accumulation lets `newArea` exceed 1 per pixel; the composite then clamps `cov` and `a` together and loses both coverage and ordering — 1.000 against a true 0.781 on two samples. Breaks size-0 `DeepToImage` parity outright (up to 2.4e-01, 12% of pixels). Third instance of the double-count class. M1.P3.T13 |
| Coverage plane double-counted, inflating alpha and colour | High | Fractional-split half found and fixed at M1.P3.T2's review (2.0 vs honest 1.0; deposit once, into the nearer bucket). Volumetric-split half still open — M1.P3.T8, which must land before M1.P3.T5's bake-off |
| Build gate not actually building the node | Med | `src/CMakeLists.txt` never listed `DeepCDefocus`; every Phase 1.2/1.3 "local build clean" was compiled by hand instead. Registration pulled forward to M1.P3.T7 |
| Kernel-bin quantisation trough on small CoC | ~~High~~ **closed** | `radiusToIndex()`'s 0.5px grid made adjacent scanlines straddling a bin edge rasterise different discs: a one-scanline 20% dark trough at r=0.762px (35% radial), colour tracking alpha so it read as a visible dark line. Energy loss, not ripple. Found at M1.P3.T16, **fixed at M1.P3.T19** by an adaptive `h(r) = r²/512` grid that bounds the deficit uniformly across radius: 2.009e-01 → 2.176e-03 against a 3.9e-03 gate |
| CoC field's own interior extremum (`l6`) | Low | Where the radius field has an interior extremum, normalised-kernel scatter under-delivers ~2/3 of the field's local slope at the apex (0.990 at slope 0.0195, 0.971 at 0.0391). **Inherent, not quantisation** — it survives an exact per-pixel kernel with no grid at all, and is invariant to K and to the bucket-composite candidate. Exposed (not caused) by M1.P3.T19, which stopped the coarse grid flattening the extremum neighbourhood onto one disc. Accepted for v1 as a bounded XFAIL; a v2 note |
| Correct number, wrong explanation | Med | Twice at M1.P3.T19 a re-pinned constant was right while its stated mechanism was fabricated; only an independent grid-free oracle caught it. Validate re-pins against an oracle, never against the new output, and **band** pins rather than bounding them on one side — a one-sided pin there would have passed a full revert of the fix |
| `pre_merge` lossy at its shipping default | Med | Losslessness holds only when grouped radii share a kernel bin; at the 0.25px default two layers 0.20px apart straddling a bin edge move 9.0e-02 on 100% of pixels. Two separate probes concluded "unreachable"/"exactly lossless" because they happened to group nothing. `merge_tolerance`'s default needs a review at Phase 1.4 |
| Alpha<1 receding content loses up to 16% of its alpha | ~~High~~ **closed** | **FIXED at M1.P3.T20** by giving the composite a second transmittance scalar — a co-located deposit is attenuated by the tile its own head claimed, not by the pooled mean over everything claimed, and a residual's occlusion is *subtracted* from that mean in proportion to the area it covers instead of multiplying the whole of it. On the isolated rig the deficit goes to **exact** at every N, α and split fraction (was −17.4% at α=0.90/N=16, −38.9% at split fraction **0.75** — T17 and T20 both mislabelled that cell as 0.25, which reads −5.49%); harness `g4` goes **0.1607 → 0.0543**; the K-divergence is **gone** (α=0.50 swept +0.33…−26.62% at K=2…128, now +1.37…−1.01%). The *opaque* twin improved five to six decades on the same scene — saturation pushes part of a bucket's alpha into the residual term even at α=1 — which retired the `g1`/`g2`/`g3` XFAILs and closed scene (g)'s own stated seam criterion. What remains at `g4` (−5.4%) is a **different mechanism**: one bucket pooling a head and a rear at unequal per-unit opacity — `f3c`/`f3d`'s term, information lost at accumulation and not recoverable by any per-bucket composite rule. (T20 first attributed this to fragments carrying *differing* split fractions; its review showed a constant fraction ≠ 0.5 reproduces it, so the trigger is `partitionAlpha(α,1−frac) ≠ partitionAlpha(α,frac)`, not fraction mixing.) **T20 also introduced a NEW error in the opposite, forbidden direction on staggered multi-part parents — see the row below.** Original entry, kept for the record: Found at M1.P3.T17's bake-off. An ordinary semi-transparent surface receding through focus reads −12.5/−16.1/−9.2% at α=0.99/0.90/0.50 (K=16) and **diverges** in K (−5.0 → −26.6% at K=8 → 128 at α=0.5), where the same content at α=1 reads −0.4% and converges. Controls exclude the kernel, the sharp path and depth quantisation (constant depth is exact under both candidates at every K and radius). **Mechanism isolated at T17's review**: the composite's single scalar `tClaimed` cannot represent a claimed area that a depth ramp has made a mosaic of differently-transmissive sub-areas — reproduced on hand-built planes with no kernel at all (−17.4% at α=0.9/N=16, exact at α=1, exact when the fragments share a bucket pair, −38.9% at split fraction 0.25). The deleted candidate was also wrong here but **better at every K and every α<1 tested** (−1.0/−6.6/−15.3/−18.3% at K=8/16/64/128 against −11.4/−16.1/−23.0/−27.5%), by 1.5–3×; it is not a reason to reopen T17 because it diverges in K too and fails identities partition satisfies exactly, but it is not merely "less bad by a hair" either. Bounded XFAIL `g4`; owed a fix or an accepted-residual ruling at Phase 1.4/1.5, and the check must be RE-PINNED by whatever commit fixes it |
| Staggered multi-part parents over-report alpha (up to +18.3%) | ~~High~~ **closed** | Found at M1.P3.T20's **review**, and **introduced** by T20: the composite carried ONE head tile, so a bucket that both claimed new area and continued a residual chain had to discard one of the two, and the dropped parent's later parts were attenuated by an unrelated tile — 153/525 swept cells >+0.5% high, worst **+12.3%** on this task's own sweep and **+21.1%** on the widened one (the review measured +18.3%), saturating alpha to 1 at α=0.90, where pre-T20 they read 4–16% **low**. **FIXED at M1.P3.T21** by carrying a STACK of 16 tiles and allocating a bucket's co-located residual across it by area from the newest end: the sweep goes to **zero cells over +0.5%, worst +0.000%** (exact, not merely under the gate), and randomised multi-parent pixels are EXACT wherever the parents' per-unit opacities agree. It cost **128 bytes per thread** — no plane, no per-bucket state, `memory_limit` unchanged — and roughly doubled an O(K)-per-pixel composite (319 → 581 ns/pixel at K=16). `g4` improved 0.0543 → **0.0325** in the same change; `a3` and every other pinned constant are bit-identical. What remains is one bucket pooling two per-unit opacities (`f3c`/`f3d`'s accumulation-time term, worst +83.8% on an adversarial 17× opacity ratio), which no per-bucket rule can undo |
| **Two defocused parents at unequal alpha and overlapping depths invent up to +71% alpha — RENDERED, and gated by nothing** | **High** | **Found at M1.P3.T21's review.** T21 records the multi-parent upward residual as an "adversarial 17× opacity ratio" on hand-built planes. It is not adversarial and it is not confined to hand-built planes: **two DeepCConstant cards side by side (a gap of 20 px), at α 0.99 and α 0.10, depth spans 8–12 and 9–13, `size` 14, K=16, defocused so their discs overlap — the ordinary "dense fog card near a thin one" shape — reads +71.2% high at its worst pixel and over +0.5% high on 41 of 186 probed pixels.** Oracle is independent of the composite and needs no ordering assumption: below saturation the area model is ADDITIVE, so the merged render must equal the sum of the two cards rendered SEPARATELY (each alone is one open chain, which the composite handles exactly). Worst pixel: solo 0.21431 + 0.03817 = 0.25248, merged **0.43220**. Other cells: α 0.10/0.99 **+63.3%**, α 0.90/0.20 **+29.3%**, and even the EQUAL-density control **+1.5%**. **T21 improved every one of these** (T20 read +94.8 / +65.1 / +44.9 / +9.6%) — this is a pre-existing defect that T21 reduced, not a regression — but it is far larger than any number in T21's record, it is in the honest-alpha contract's forbidden direction, and **no check in the harness or the unit suite bounds it**: `f3c`/`f3d` are the only overlapping-depth rendered checks and both use two slabs at the SAME density (grey 0.5), which is the one case that is nearly exact. Mechanism is tile mis-assignment across open chains (see the corrected Decisions entry), which no plane count fixes. **Owed at Phase 1.4/1.5: an unequal-density `f3c`/`f3d` variant with a hard bound, and a v1 node-help note that overlapping defocused volumes of differing density over-report alpha.** | 
| Scene (g)'s α<1 ramp reads HIGH — up to **+6.5%**, at **every** K including the default | Med | **RE-MEASURED AND WIDENED AT M1.P3.T21's REVIEW**, which swept the g4 rig over α ∈ {0.99, 0.90, 0.50, 0.30, 0.10} × K ∈ {2,4,8,16,32,64} on BOTH the T21 tree and a worktree build of T20 (`ac46700`). T21's self-report below reproduces exactly for the cells it covers — but it stopped at α=0.50, and the excursion grows as α falls: **α=0.30 reads +5.17% (K=2/4) and +3.40% at the default K=16; α=0.10 reads +6.53% (K=2/4) and +5.93% at K=16.** So it is neither bounded by +3.8% nor confined to coarse K. Most of that is **pre-existing, not T21's**: T20 read +3.73% and +6.05% at α=0.30/0.10 K=2/4, so T21 added +1.4 and +0.5 points there against the +2.4 it added at α=0.50. The honest statement is that this rig has carried an UNGATED upward error at low α since before T20, T21 made it modestly worse, and **no check anywhere bounds it** — `g4` is α=0.90/K=16, where T21 reads −3.25% (a deficit). A bounded check owed at Phase 1.4/1.5 must cover α≤0.30, not only α=0.50. Original T21 entry follows. **New at M1.P3.T21**, and the price of its fix: removing the composite's spurious occlusion lifts scene (g)'s whole K curve by ~2 points, which closes the deficit at fine K and pushes the already-positive coarse-K readings further up. α=0.50 reads **+3.76% at K=2/4** (T20: +1.37%) and **+0.90% at the shipping default K=16** (T20: −0.13%); α=0.90 reads +0.82% at K=2/4. Bounded and roughly K-flat — no divergence — but upward, which the honest-alpha contract forbids. **Not** the mosaic term and **not** the `excess` regime (a hand-built dense ramp at α=0.50 is exact at coverage 0.5 and 1.0 and reads *lower* than T20 at coverage 1.5); it is the accumulation-time pooling of unequal per-unit opacities seen from its positive side, i.e. `f3c`/`f3d`'s term, which no per-bucket composite rule can undo. Nothing gates it — the only α<1 ramp check, `g4`, is α=0.90/K=16 — so a bounded check at α=0.50 is owed at Phase 1.4/1.5. **RE-MEASURED AND CONFIRMED A THIRD TIME at M1.P3.T23's review**, on the shipped plugin, through a fresh reimplementation of this rig (α=0.30 **+5.166%** K=2/4, **+3.399%** K=16; α=0.10 **+6.526%** K=2/4, **+5.926%** K=16; α=0.50 +3.757%/+0.897%; α=0.90 +0.819%/**−3.253%**, the last being `g4`'s own 0.0325 pin, which is what validates the probe). **These are `g4`-RIG figures and belong to THIS row.** M1.P3.T23 read them against the `f3e`/`f3f` two-card unequal-density oracle, got +0.370%/+1.619%, and reported this row as unreproducible and transposed; that report is **retracted** (Decisions, 2026-08-16). Anyone re-checking these numbers must render scene (g)'s ground ramp, not `unequalDensityCell()` |
| Upward alpha error in the `excess` regime (pre-T20, unbounded) | Med | A fragment whose head lands entirely in already-claimed area registers no tile, so its own co-located rear is attenuated by whatever tile the pixel was carrying: behind a full-coverage α→0 foreground a defocused opaque fragment of coverage 0.05 reads 0.0976 against a true 0.0501 (**+94.8%**). A fragment straddling the free/claimed boundary has its excess attenuated by a mean including the tile it just claimed (**+8.3%**). Both bit-identical before and after T20 — not T20's regression — but no check bounds either, and both err in the honest-alpha contract's forbidden direction. Registering the excess as its own tile fixes the first exactly on hand-built planes and is **refuted by pixels** (g1 1.082e-02, g4 0.0825), so the fix is not known |
| Documented residual masking a later regression | Med | An unbounded XFAIL swallows anything that lands on top of it. Every harness XFAIL now carries a hard outer bound that flips it to FAIL on drift; new XFAILs must too. **M1.P3.T20's review found the converse too**: `l3` was described as retired while still carrying `expectedFailure=True, hardTol=8.0e-03`, so a full revert of T20 reported XFAIL, not FAIL. A check whose residual is gone must lose its `expectedFailure`, not just its reading |
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

- [x] M1.P2.T2 — `_validate`/`_request`/`engine` flatten path + NDK compile gate
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

- [x] M1.P3.T0 — Adjudicate volumetric tidying against the deep spec (run FIRST in this phase)
  - files: none expected (investigation); if it concludes a change is needed, that change lands as
    an amendment to this phase's tasks, not here
  - approach: M1.P2.T2's review measured that for **overlapping volumetric spans** this node
    disagrees with stock `DeepToImage` by ~8.9e-03 (partial overlap) and ~9.5e-02 (perfectly
    coincident) — not a rounding difference but two different algorithms: the design mandates
    `deepc::tidyOverlapping()` (split + over-merge) while Nuke runs its own
    `CombineOverlappingSamples`. Decide which is right *on the merits* rather than by preferring
    either implementation: derive the correct result for a hand-worked overlapping-fog pixel from
    the OpenEXR "Interpreting Deep Pixels" tidying rules (the same exponential in-span model this
    node already uses for holdout transmittance and for the volumetric bucket split), then compare
    both implementations against it. Recommend one of: keep `tidyOverlapping()` and scope the
    parity gate to point samples; adopt Nuke's combine for the flatten; or fix `tidyOverlapping()`
    if it is the one that diverges from the spec.
  - verify: a written recommendation with the hand-derived reference numbers, folded into this
    file's Decisions, and scene (a)'s volumetric clause settled. This must land before M1.P3.T1
    builds SoA flattening on top of the tidy pass, and before M1.P3.T5's correctness gate.
  - size: M

- [x] M1.P3.T1 — `PodBuffer<T>`, `DEEPC_HD`, and SoA flattening
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
    (~2.6KB at K=128) — stack-sized in the per-sample path, no heap. Note `tidyOverlapping()`
    restarts its scan (and re-sorts) after every split, so a pixel with many overlapping spans costs
    roughly O(splits · n log n) — tolerable at M1.P2.T2's scale but this task puts it on the real
    hot path, so measure it. Also: `max_radius` is not proxy-scaled (M1.P2.T2 used it only for the
    bbox pad, where over-padding is harmless); the radius clamp here must handle proxy.
  - verify: covered by the M1.P3.T4 unit test task (synthetic `SampleRecord` vectors in, SoA
    buffers out).
  - size: L

- [x] M1.P3.T2 — `scatterBandCPU`
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

- [x] M1.P3.T7 — Wire `DeepCDefocus` into `src/CMakeLists.txt` (run NEXT — every later verify depends on it)
  - files: `src/CMakeLists.txt`
  - approach: found at M1.P3.T2's review — **neither `DeepCDefocus.cpp` nor `DeepCDefocusScatter.cpp`
    is in CMake**, so the milestone's "the local build compiles clean" verify step has been vacuous
    for every Phase 1.2/1.3 task; both were compiled by hand ad hoc, and the tests target only builds
    the header-only math. Pull the *registration* half of M1.P5.T1 forward: add `DeepCDefocus` to
    `PLUGINS` and `FILTER_NODES` inside the `if (UNIX)` guard (verified pattern: top-level
    `CMakeLists.txt:11` already opens a UNIX block), plus
    `target_sources(DeepCDefocus PRIVATE DeepCDefocusScatter.cpp)` following the
    `DeepCShuffle2`/`ShuffleMatrixKnob.cpp` precedent at `src/CMakeLists.txt:128-129` (not the
    FastNoise object-library pattern — different shape). **Do NOT add `-mavx2 -mfma` here** — those
    compile options stay at M1.P5.T1, where the FMA/`fp-contract` parity hazard is documented, and
    they are only wanted once M1.P4.T2 has inspected vectorization.
  - verify: `cmake -S . -B build/local-17.0 -D Nuke_ROOT=/usr/local/Nuke17.0v3 && cmake --build
    build/local-17.0 -j"$(nproc)"` builds `DeepCDefocus.so` (confirm the `.so` appears under
    `build/local-17.0/src`, i.e. the gate is no longer vacuous), warnings reviewed; the headless-Nuke
    `DeepToImage` parity check from M1.P2.T2 still passes against the CMake-built plugin.
  - size: S

- [x] M1.P3.T8 — Coverage-head flag for split volumetric parents
  - files: `src/DeepCDefocusScatter.h`/`.cpp`
  - approach: found at M1.P3.T2's review and deliberately deferred out of it (it changes M1.P3.T1's
    committed SoA contract). Every part of a split volumetric parent currently deposits its own
    `w·vis` into the coverage plane, so one slab is counted once **per bucket it spans**: a 4-part
    α=0.9 slab measures band alpha sum **1.7506 (CoveragePartition) / 1.7372 (over) against an honest
    0.9000** — ~1.94×, growing toward K× as α→1. It is exact only at full kernel coverage, so it is
    wrong for every bokeh, every edge and every isolated fog element — i.e. exactly the scenes (f),
    (g) and (i) that M1.P3.T5's bake-off turns on. Fix per the review's verified recipe: the flatten
    marks the **first part of a split parent** (one bool through `FragmentRecord`/`SampleSoA`,
    surviving pre-merge as head-of-group), and the scatter passes
    `depositWeight = coverageHead && group == 0`; the composite already handles alpha-without-coverage
    via M1.P3.T2's residual term. Verify analytically that this reconstructs the parent exactly at any
    coverage, not just at full coverage. Note this re-measures M1.P3.T1's 113 B/fragment resident
    (≈100 B since M1.P3.T10)
    figure — update the Decisions entry if it moves materially.
  - verify: a driver over random (α, split-part-count, kernel radius, coverage fraction) showing band
    alpha/premult-colour sum reconstructs the parent to ≤1e-6 at partial coverage, plus the existing
    single-fragment energy identity unregressed; local build and `ctest` green. The permanent test
    lands at M1.P3.T4.
  - size: M

- [x] M1.P3.T9 — Fourth accumulation plane: co-located area (run BEFORE T5's bake-off)
  - files: `src/DeepCDefocusScatter.h`/`.cpp`
  - approach: **user's call at the M1.P3.T8 boundary** — build the exact fix rather than judging the
    approximation from pixels first, because the coverage-partition candidate is the only one that
    reads the coverage plane (plain `over` cannot use it at all), so a bake-off with candidate 2
    knowingly crippled would not mean much. Add a fourth per-bucket plane holding **co-located area**
    beside the existing "new area", so the composite's residual resolves as `resLocal = aRes/D_k = a_p`
    exactly at any radius spread and the telescope closes per pixel rather than only when a parent's
    parts share a CoC radius. This retires the −92.1% (α=0.9) / −24.1% (α=0.1) full-range-span error
    and the `claimed == 0` back-field over-count recorded in Decisions; verify both directions, since
    the current form errs high behind focus and low in front of it. Memory formula becomes
    `K·W·B·(C+3)·4` — **update every place the plan and the source state `(C+2)`**: the Design
    reference's Parallelism bullet, `BucketPlanes::bytesForBand()`, and M1.P4.T1's budgeting text.
    Cost is ~+17% of the bucket planes (~+17MB at 4K defaults), negligible against the SoA's ~2.4GB, so
    do not trade accuracy for it. Keep the plane out of `FrontToBackOver`'s path — that candidate must
    stay exactly as it is so T5 compares like for like, and the loser's path is deleted at T5 anyway.
    `DEEPC_HD` bodies stay in the header, `.cpp` stays loop drivers only.
  - verify: the equal-radius identities all stay bit-unchanged (two 50% fog layers 0.750000, receding
    opaque 1.000000, scene (i) 0.600000, the 4-part opaque slab 1.000000, M1.P3.T2's single-fragment
    energy identity); the differing-radius cases now reconstruct the parent to ≤1e-6 — re-measure the
    exact rig the Decisions entry names (front of focus at 4/8/12 buckets and full range, behind focus
    at 2/3/4/8, at α=0.9 and α=0.1) and show each is now exact; premultiplied colour:alpha ratio holds
    at the input's true value on a span reaching focus (the 0.8748-vs-0.5 case); local build and
    `ctest` green; `DeepToImage` parity still 0 ULP.
  - size: L

- [x] M1.P3.T3 — Holdout SoA and per-pixel boundary-LUT
  - files: `src/DeepCDefocusScatter.h`/`.cpp`
  - approach: build the holdout sample SoA (`DeepFront/DeepBack/Alpha` only) and, per band, the
    per-dest-pixel transmittance LUT at the K+1 bucket boundaries (in-span exponential folded in
    at build time per the Design reference's Holdout mechanics). Unconnected holdout / outside
    holdout bbox short-circuits to `vis ≡ 1` at zero cost.
  - verify: covered by the M1.P3.T4 unit test task (synthetic holdout SoA in, checking LUT
    values against the exact exponential eval).
  - size: M

- [x] M1.P3.T10 — Decouple the holdout LUT's boundary set from the ΔCoC buckets (run BEFORE T4)
  - files: `src/DeepCDefocusScatter.h`/`.cpp`, `src/DeepCDefocusMath.h` if the boundary-set helper
    belongs beside `HoldoutVisibility`
  - approach: found at M1.P3.T3's review. **The Design reference's instruction to build the holdout
    transmittance LUT "at the K+1 bucket boundaries" is wrong, and it breaks the node's differentiator.**
    ΔCoC spacing bounds *banding* — a CoC criterion — and deliberately spends 15 of 16 buckets in front
    of focus, so on the default rig (K=16, focus 10, range [1,100]) the entire back side collapses into
    one bucket spanning `[10, 100]`. An **opaque point-sample holdout** — a solid card, the commonest
    holdout there is — at z=50 then starts occluding at **z=10.9**: a source fragment at z=15, 35 units
    *in front* of the card, comes out **98% erased** (vis 0.0215), and fragments at z=30/40/49 are
    erased completely. Mean |vis error| 0.391, max 1.000. **K does not rescue it** (bites at 25.8 / 40.6
    / 40.2 for K=32 / 64 / 128), and neither does a better interpolant (linear-in-T moves the mean only
    0.476 → 0.450) — the dominant term is boundary *placement*, not interpolation. Two tells that this
    is not a principled approximation: the log chord collapses the step onto the bracket's *near*
    boundary (systematically toward camera), and the error's magnitude tracks `kMinTransmittance`
    (moving the floor 1e-30 → 1e-3 moves the bite 10.9 → 19.0).
    Fix: give the holdout LUT **its own boundary set**, decoupled from the ΔCoC buckets. With the same
    17 entries/pixel, uniform-in-z measures mean error **0.057 vs 0.391** and bites at **44.4 vs 10.9**
    against a true 50 — 7× better at identical memory, with a closed-form O(1) index. Prefer that as
    the baseline and measure a holdout-depth-histogram-derived set against it; **uniform-in-1/z is NOT
    the answer** (0.352, barely better than shipped). Sub-refining the ΔCoC set instead needs S=16
    (257 entries/px, 16× memory and build time) to reach the same place — reject it. A worst case of
    `(T0−T1)/2` is irreducible with two boundary values, so the goal is the right *placement*, not
    exactness.
    This changes `HoldoutSoA`'s contract — `boundaryCount` stops being K+1, and the fragment carries a
    different index/frac pair than `locateBoundary()`'s — which is why it runs **before** M1.P3.T4
    rather than at T5: T4 should write its permanent tests once, against the final contract.
  - verify: re-measure the exact rig above — opaque point holdout at z=50 on the default K=16 rig must
    occlude at ~50, not 10.9, and a fragment at z=15/30/40 must be essentially unattenuated; report
    mean/max |vis error| against the shipped 0.391/1.000 and the randomised sweep (K 4–128, 1–4
    samples) against 0.464 (point) / 0.431 (sub-bucket span) / 0.234 (wide span). Boundary-set build
    stays O(1)-indexable and adds no per-band memory over `(K+1)·W·B·4`. All of M1.P3.T3's identities
    still hold: LUT vs exact 0.000e+00 at boundaries, all-ones LUT bit-identical to the disabled path,
    fully-behind ⇒ exactly 0, fully-in-front ⇒ bit-identical to no-holdout, and the hard-edge
    transition exactly one pixel wide in both directions. Local build and `ctest` green; `DeepToImage`
    parity still 0 ULP.
  - size: L

- [x] M1.P3.T11 — Both `interpAtBucket` variants for the opaque-step degeneracy (run BEFORE T4)
  - files: `src/DeepCDefocusMath.h`, plus whatever carries the selection flag through
    `ScatterParams`/`HoldoutSoA`
  - approach: M1.P3.T10 fixed boundary *placement*; this is the interpolant half of the same defect,
    which its review measured as **not irreducible** after all. The shipped log chord floors `log T`
    at `kMinTransmittance`, so an opaque step collapses vis to ~0 across almost the whole bracket
    instead of the `(T0−T1)/2` bound's 0.5 — and always toward camera. Net effect: **a holdout card
    still fully erases genuinely-unoccluded source geometry for one bracket in front of it** — 5.07 of
    the 6.19-unit bracket at K=16 on the default rig (5.57 worst case over card position), 2.28 at
    K=32, 0.89 at K=64, scaling as ≈`(depthRange)/K`. An element sitting just in front of the
    held-out object *is* the standard holdout setup, so this is an ordinary case, not a corner.
    Follow the M1.P3.T2 precedent: implement **both** candidates behind a runtime flag (not a
    preprocessor one — T5 must switch at render time) and let T5 decide from pixels. Both are ~4 lines,
    zero memory, zero per-fragment cost, and fire **only** when `T1 == 0` — reachable only from
    fully-opaque content, where "log T is linear in z" is not the model at all — so neither moves any
    α<1 case at all. Measured: midpoint-step on `T1==0` gives headline mean 0.0262, 2.59u erased in
    front, ≤half a bracket leaked behind, opaque sweep 0.0074; linear-in-T on `T1==0` gives 0.0266,
    **0.00u erased**, 0.49u leaked behind (6.16u worst), opaque sweep 0.0097; the shipped chord is
    0.0565 / 5.07u / 0.00u / 0.0134. The trade is **erasing FG in front vs leaking BG behind**, and
    scene (e) can fail either way — which is exactly why it is judged from pixels rather than derived.
    Do not pursue "one extra entry" (17→18→19→21 gives 5.07 → 1.83 → 4.45 → 3.96 units: phase noise,
    not convergence) or snapping a boundary to a detected opaque step (exact, but needs the eager
    full-frame holdout pass the histogram set was rejected for).
  - verify: both variants selectable at runtime and measured against the table above on the same rig;
    every α<1 case bit-unchanged under all three (that is the property that makes this safe); all of
    M1.P3.T3/T10's identities still hold — LUT vs exact 0.000e+00 at boundaries, all-ones LUT
    bit-identical to the disabled path, fully-behind ⇒ exactly 0, fully-in-front ⇒ bit-identical to
    no-holdout, hard edge exactly one pixel wide in both directions. Local build and `ctest` green;
    `DeepToImage` parity 0 ULP.
  - size: M

- [x] M1.P3.T4 — Unit tests for the scatter core (POD-level)
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
    **Add a parent-reconstruction test for the composition contract**: accumulate a volumetric
    sample's deposits, composite them front-to-back, and compare against the parent sample. M1.P3.T1's
    review showed `checkCompositionContract()` catches a wrong *branch* but NOT a wrong *label* — the
    branch and the audit read the same `kind` field, so mislabelling a split part as `Point`
    reproduces the +9% double-count and still passes the audit. Parent reconstruction is the only
    check that catches it. (Note the +9% figure no longer reproduces from a wrong *label* alone since
    the fourth plane landed — measured at T4, a Point label with the head kept on part 0 gives exactly
    0.900000, because the centre re-split of a whole-weight assignment is compensated. The whole
    over-count is now coverage-head duplication, pinned at **+40.09% / +70.70%** for 2/4 parts at
    α=0.9. `checkCompositionContract()` still accepts both labellings.)
    ~~Also consider extracting an `overCompositeGroup()` helper into `DeepSampleOptimizer.h` here~~ —
    **deferred deliberately at T4**: the safety condition ("once these tests protect the shipped
    `DeepCBlur`/`DeepCBlur2` callers") is NOT met, because those plugins reach the arithmetic through
    `optimizeSamples()`, which `DeepCDefocus` never calls and this suite therefore never exercises. The
    three copies (`DeepSampleOptimizer.h:486` and `:577`, `DeepCDefocusScatter.cpp:582`) also differ
    materially — span union present/absent, channel-count clamp present/absent, different output
    target — so it is a real refactor of shipped shared code needing the same "bit-identical on
    previously-working input" differential evidence M1.P3.T6 carried. Follow-up task, size S/M: add a
    differential harness over `optimizeSamples()` first, then extract and point all three at it.
    **From M1.P3.T2's review** (these are the mutation-resistant gates for the whole coverage-plane
    defect class, which that review found live and fixed): the **single-fragment energy identity** over
    random `(α, split fraction, radius)` — band alpha and premult-colour sums reconstruct the fragment
    (measured 1.37e-07 / 1.24e-07 under `CoveragePartition`; plain `over` is up to **+93.8%**, so
    `over` inflates as badly as it deflates and both directions need a case); the
    **volumetric-slab-at-partial-coverage identity** (currently failing — gate it on M1.P3.T8's
    head-flag fix); scene (c)'s flat-opaque field asserted at **|α−1| ≤ 1e-6, NOT equality** (measured
    0.9999992 — the disc LUT's ~5e-8 per-entry normalisation residual over ~113 contributing
    fragments; the earlier "exactly 1" reading was an artifact of the over-count then being clamped);
    and every case must **set `ScatterParams::combine` explicitly** rather than relying on the
    provisional default.
    **From M1.P3.T8/T9's reviews**: head count == 1 per **post-tidy** parent, fuzzed (T8's review ran
    200,000 single-parent and 60,000 multi-parent cases); `pre_merge` on/off identical for a single
    parent in alpha *and* coverage; the 3000-point-fragment bit-exactness corpus and the four
    hand-built identities asserted with the fourth plane both zero and populated; front-of-focus
    exactness at 4/8/12 buckets and full-side range; and a **pinned** behind-focus regression gate at
    the measured +45.2/+51.2/+59.1% (3/4/8 buckets, α=0.9) so the structural residue is
    documentation-with-teeth rather than something that drifts silently. Assert the **colour:alpha
    ratio as a standing invariant** (and `aCov ≤ cov`): clamping one of a premultiplied pair and not
    the other has now been the defect three times — M1.P3.T8's residual, T9's area split, and T2's
    original saturation — so it wants one invariant, not three cases. Note `checkCompositionContract()`
    does **not** audit the area planes; a test summing `weight + colocated` per bucket against the
    deposits would catch a future break of the deposit invariant. `tests/test_defocus_scatter.cpp`
    does not exist yet.
    **From M1.P3.T3's review**: a **point-sample-holdout accuracy case** — the one holdout shape the
    suite has no coverage of, and the shape that exposed M1.P3.T10's boundary-set defect; a
    `build()`-vs-`evalBoundaries()` equivalence fuzz on **overlapping and unsorted** input (both are
    supported, neither is assumed); the NaN-depth drop; and a `depthScale` round-trip. Write these
    against M1.P3.T10's boundary set, not T3's. **T10's review already pinned `HoldoutBoundaries`'
    documented post-conditions, its closed-form-index-vs-binary-search agreement, its NaN/±inf/
    degenerate-range behaviour, the `locate()+interpAtBucket` == `interp` identity and the
    bit-exactness at the boundaries in `tests/test_defocus_math.cpp`** (T10 shipped the struct
    before this task exists, so they could not be left as a promise). Move them into the
    scatter-core suite if that reads better, but do not drop or weaken them.
    **Standing warning from M1.P3.T13's review**: its `"coverage head: exactly one per POST-TIDY
    parent"` re-pin was claimed "strictly stronger" and was not — "head `bucketIndex0` pairwise
    distinct" and `!preMerge ⇒ heads == liveParents` are *different* invariants, not ordered, and a
    mutation silently dropping one head of three passed the new clause while failing the old. When any
    pinned invariant is replaced, prove the replacement subsumes the original or keep both.
    **From M1.P3.T11's
    review**: a scatter-core-level (not just math-level) test that `ScatterParams::holdoutInterp`
    threads correctly through `scatterBandCPU` on **both** the sharp and span paths, and the dense
    volumetric holdout fixture (46 samples at α=0.9 packed in one bracket) at the scatter level, so the
    variants' divergence is pinned in deposited pixels rather than only in the LUT math.
    **Add a termination fuzz test for `deepc::tidyOverlapping()`**: randomised sample vectors with
    depths drawn from a small discrete set so exact ties are common, asserting termination and a
    bounded output size. The non-termination bug fixed during M1.P2.T2 hung Nuke unkillably on
    ordinary fog input and would have been caught in milliseconds by this.
  - verify: `cmake -DDEEPC_BUILD_TESTS=ON && make && ctest` — all cases pass.
  - size: M

- [x] M1.P3.T6 — Make `tidyOverlapping()`'s split pass single-pass (run BEFORE T5)
  - files: `src/DeepSampleOptimizer.h`
  - approach: the split pass restarts its scan and re-sorts after every single split, which measured
    at M1.P3.T1 as **≈O(n³·⁷) time for O(n) output** on mutually overlapping spans — per pixel:
    0.06ms at 8 spans, 0.59ms at 16, 2.92ms at 24, **9.96ms at 32**. At ~10ms/pixel a 2K frame of fog
    would take days, so T5's validation scenes (f) and (g) are not runnable until this is fixed, and
    it is not shippable regardless. It also costs one malloc/free per multi-sample pixel (~12.7M per
    4K frame) from its result vector, and it dominated the review's sanitizer runs by ~1000×, so it
    is a dev-loop cost as well as a render cost. Replace with the equivalent single-pass form: collect the distinct
    endpoint set once, sort it, then cut every span against it in one sweep — O(n log n), identical
    output. The transmittance-preserving split arithmetic and the mixture merge (M1.P3.T0) both stay
    exactly as they are; only the loop structure changes. Note this is shared code reached by shipped
    `DeepCBlur`/`DeepCBlur2`, so it needs the same "bit-identical on previously-working input"
    evidence the two earlier fixes carried.
  - verify: a differential harness over a large randomised corpus showing bit-identical output
    against the current implementation on every input it terminates on, plus the timing curve
    re-measured; full local build and `ctest` green; `DeepToImage` parity unchanged in headless
    Nuke. (Outcome: 9.96ms → 0.28ms at n=32, 583× at n=128. "Single-digit µs" was not attainable —
    the intermediate is Θ(n²) for mutually overlapping spans even though the output is O(n).)
  - size: M

- [ ] M1.P3.T5 — Wire scatter into `engine()` (serial, single frame-wide lock)
  - files: `src/DeepCDefocus.cpp`, `src/DeepCDefocusScatter.h`/`.cpp`
  - approach: `computeDepthRange()` — a separate cheap full-frame `DeepFront/DeepBack/Alpha`
    pass, alpha-weighted, producing the ΔCoC bucket boundaries. **It must apply the same
    ray-distance→Z correction the flatten applies**, or every corner-pixel sample lands below
    `depthMin` (the correction always shrinks depth) and out-of-range spans pile into the edge
    bucket; the alpha-weighted range clipping can do the same to low-alpha fog. M1.P3.T1's review
    made out-of-range parts harmless by over-compositing same-bucket parts of one parent, but the
    two passes disagreeing is still a bug worth not writing. Build the `DiscKernelLUT` here,
    immediately after that pass, proxy-scaling `edge_softness` as well as the radii (M1.P3.T1's
    `applyProxyScale()` deliberately does not touch `edge_softness`, since `CocParams` doesn't carry
    it — the LUT build is where it lands), over the frame's **measured** radius range — `rMax` measured
    (clamped by `max_radius`), but **`rMin` passed as 0** — rather than eagerly over
    `[0, max_radius]`; see Decisions for both halves of this.
    `computeBand(b)` — given the
    global boundaries, fetch source rows for `band ± maxRadius`, run T1's SoA flatten, T3's
    holdout LUT, T2's scatter, saturate, the bucket composite (`compositeBucketsFrontToBack()` when
    this was written; `compositePixelCoveragePartition()` since M1.P3.T17 deleted the other
    candidate), write.
    **Holdout obligations from M1.P3.T3's review**: skip the fetch/append loop **entirely** when the
    holdout is unconnected or the band doesn't intersect its bbox (`begin(N)` with no appends is
    well-defined and lands on the same disabled view) — discovering emptiness by running the per-pixel
    loop costs ~1.98 ms/band, ~67 ms per 4K frame of pure bookkeeping; pass the **same per-pixel
    ray-distance factor as the flatten** via the holdout SoA's `depthScale` (an uncorrected holdout
    sits 47.3% too far back in Z at the corner of a 20mm frame); and do **not** compute the holdout
    matte AOV as `1 − boundaryT` in float (see Decisions — the deficit's relative error is 100% at
    α=1e-7). Build the holdout boundary set **once per frame** via
    `makeUniformHoldoutBoundaries(buckets)`, never per band: a fragment near a band edge scatters into
    two bands, and per-band sets put a seam along every boundary (measured: the same fragment reads vis
    0.0448 in one band and 1.0000 in the next). Both run under
    a single frame-wide lock in this phase (no per-band concurrency yet — that's Phase 1.4) so
    correctness lands before concurrency is introduced. Set `holdoutInterp` and `pre_merge` from the
    knobs explicitly — never rely on a default (M1.P3.T18 must be able to render each candidate).
    This clause named `ScatterParams::combine` first; M1.P3.T17 decided that bake-off and deleted the
    field, so there is nothing left to select there.
  - verify: the local build compiles and `DeepCDefocus.so` is produced; both unit suites stay green;
    in headless Nuke the node renders a defocused frame end to end on a simple deep scene (not black,
    not garbage, bbox padded, sparse regions still exactly black), **scene (a)'s size-0 flatten parity
    is still 0 ULP for point samples** now that the scatter rather than the old flatten path produces
    it, and an aborted mid-cook still recovers cleanly. **STATUS: the wiring is landed and correct, but
    this task is NOT closed** — its parity clause fails (M1.P3.T13 owns the defect, which is in the
    bucket accumulation T5 merely put on the cook path, not in the wiring), and abort recovery could
    not be exercised at all, since headless Nuke cannot trigger a recoverable mid-cook cancel
    (`nuke.cancel()` from a timer thread does nothing; `SIGINT` kills the process). Re-run this gate
    after T13; the abort clause needs an interactive or Viewer-driven check and must not be recorded as
    verified until it gets one. The full validation sweep (a)–(l) and both
    bake-off decisions are **M1.P3.T12**, split out because they are a distinct body of work with
    their own gate — this task's job is to make the node actually render correctly, T12's is to prove
    it across the scene list and pick the two candidates.
  - size: L

- [x] M1.P3.T14 — Default `CMAKE_BUILD_TYPE` to Release (run FIRST — everything downstream measures it)
  - files: `CMakeLists.txt`
  - approach: found at M1.P3.T5's review. **`CMAKE_BUILD_TYPE` is unset, so the milestone's own build
    command has never passed an `-O` flag at all.** The compile line for both the plugin and the test
    targets is `-D_GLIBCXX_USE_CXX11_ABI=1 -fPIC -DUSE_GLEW -msse … -mavx -std=gnu++17` and nothing
    else. Consequences, measured: the shipped `DeepCDefocus.so` contains **0** `vfmadd`/`vmulps`/
    `vaddps` where the same TU at `-O3 -mavx2 -mfma` contains **97**, and `-fopt-info-vec` reports **0**
    vectorized loops against **396**. So the row-span auto-vectorization the entire performance design
    rests on **has never existed in any binary the CMake build produced**, and every earlier task's
    `-fopt-info-vec` evidence describes hand-compiled objects rather than the shipped module. Cost is
    3.2× on the scatter suite (10.02s → 3.10s) and ~4.7× on a 1K cook. Smallest correct form: three
    lines after `project(DeepC)` defaulting `CMAKE_BUILD_TYPE` to `Release` when the user hasn't set
    it (never overriding an explicit choice, and leaving multi-config generators alone).
    **Must run before M1.P4.T2**, not at M1.P5.T1 — P5.T1 lands after P4.T2's perf gate, so the gate
    would otherwise profile the unoptimised module. Verified safe at T5's review: at `-O3` with the
    *existing* flag set both suites are green and **zero `vfmadd`** is emitted, so the `fp-contract`
    parity hazard stays untouched (it needs `-mfma`, which is deliberately absent until M1.P5.T1).
  - verify: `cmake -S . -B build/local-17.0 -D Nuke_ROOT=/usr/local/Nuke17.0v3` with no build type
    given now compiles at `-O3`; confirm from the actual compile line, not from the cache alone.
    `-fopt-info-vec` on the scatter TU reports vectorized loops where it reported none. Both suites
    green. **`DeepToImage` parity still 0 ULP** — that is the whole risk of turning on optimisation,
    and the `#pragma GCC optimize("fp-contract=off")` guard is what should hold it. An explicit
    `-DCMAKE_BUILD_TYPE=Debug` must still win.
  - size: S

- [x] M1.P3.T13 — Same-pixel bucket collisions break the size-0 flatten (run BEFORE T12)
  - files: `src/DeepCDefocusScatter.h`/`.cpp`, `tests/test_defocus_scatter.cpp`
  - approach: found at M1.P3.T5's review, which measured it far past what T5 itself reported. **Within-
    bucket accumulation is additive and the `newArea` plane can exceed 1 per pixel; the composite then
    clamps both `cov` and `a` into [0,1] and loses the coverage and the ordering together.** Plane dump
    from the worst 2-sample case (z=9.063 α=0.4667, z=11.039 α=0.5899, K=8): `bucket[6] alpha=1.0127
    newArea=2.0 colocated=0.0` — *both* fragments' heads claimed new area in the same bucket, `cov`
    clamps 2.0→1.0, `a` clamps 1.0127→1.0, `local = a/cov = 1.0`, and the bucket reads fully opaque:
    output **1.000 against a true 0.781** (+0.219 α, +0.269 colour). This is the third instance of the
    "Coverage plane double-counted" High risk.
    **The trigger**: pre-merge groups by `bucketOfContaining()` (`DeepCDefocusScatter.cpp:401`) while a
    Point fragment deposits via `bucketOf()` (`:255`). Two same-pixel fragments in *different*
    containing buckets can share a `bucketOf` index, so they are never merged and both deposit `w=1` of
    new area into the same bucket. Note T5's implementer diagnosed this as the `C_k : D_k` area-ratio
    split being depth-order-blind; that is a real but *sub-dominant* case — in the dump above
    `colocated == 0`, so that branch isn't even engaged.
    Magnitude (scene (a) config, K=16, `pre_merge` ON, 2000 random pixels per count) — note it is
    **bimodal**, which is why spot checks read "3–5 ULP": median |dα| ~2e-08 at every count, but max
    **1.73e-01 at 2 spp / 2.05e-01 at 3 / 2.38e-01 at 5 / 2.32e-01 at 20**, with **5.7% / 9.1% / 11.8%
    / 3.0%** of pixels wrong by >1e-3. Even **1 spp is not bit-exact today** (247–266 of 300).
    **Three fixes are already disproved and must not be re-tried**: whole-weight (`bucketOfContaining`)
    on the sharp path alone — fixes 1 spp exactly (300/300) and gets n≥2 inside the ≤2e-07 gate, but
    only with `pre_merge` ON (OFF gives 0.946 → 1.000), it steps **0.700 → 0.900 at radius 0.5**, and
    decisively it **defeats the K knob**, which is the design's own stated mitigation for within-bucket
    ordering loss (RMS over 27 configs at K=8/16/32/64 — baseline 1.51e-01 / 7.96e-02 / 2.01e-02 /
    5.69e-03 converges; whole-weight 9.06e-02 / 1.35e-01 / 5.94e-02 / 3.03e-02 does not); merge-key =
    deposit bucket alone (worst 2.2e-01); both together (2.4e-01 — the key and deposit disagree again in
    the opposite direction). Depositing coverage into both buckets is also closed off (M1.P3.T2's
    review, +8.29% double-count). Candidates worth exploring: merge by the deposit **pair**, so no two
    unmerged same-pixel point fragments share either bucket; pre-composite a pixel's sharp fragments
    per bucket before deposit; or a per-pixel-per-bucket "already claimed" marker so a second same-pixel
    deposit lands as co-located rather than new area.
    **Also note validation scene (l) already fails on current code** independently of any threshold
    crossing: a two-layer flat field (truth 0.70) wanders 0.683–0.768 across the ramp, max step 9.19e-02.
  - verify: (1) scene (a) at size 0, K ∈ {4,8,16,32,64,128}, 1–20 spp, ≥2000 random pixels per count —
    report **median and tail**, and require max |dα| ≤ 2e-07 with **0% of pixels >1e-3**, at `pre_merge`
    both ON and OFF. (2) The two-layer flat field stays **K-convergent**: RMS must decrease monotonically
    in K and beat the baseline row above at every K. (3) No step at the sharp threshold on a 0–2px ramp:
    max step across a threshold crossing ≤ max step elsewhere. (4) Two existing tests move and must be
    re-pinned **with an explicit argument, not a tolerance bump** — `"flattenPixelToSoA reproduces an
    independent tidy + split + merge reference"` and `"two distinct co-located point parents (PINNED)"`;
    the latter (`tests/test_defocus_scatter.cpp:2594`) currently pins `newArea = 2.0` and alpha
    `0.694518` against a true `0.58`, i.e. **it pins the bug** and must be corrected rather than
    preserved. Local build and both suites green.
  - size: L

- [x] M1.P3.T15 — Per-bucket transmittance attenuation at the flatten (run BEFORE T12)
  - files: `src/DeepCDefocusScatter.h`/`.cpp`, `tests/test_defocus_scatter.cpp`
  - approach: identified at M1.P3.T13's review as the change that closes **three** open holes at once and
    lets T13's one-slot merge and its gates be **deleted**. T13 fixed same-pixel bucket collisions by
    merging colliding groups and by claiming area per (bucket, kernel); that works, but leaves: (1) the
    **cross-kind hole** — a Point and a span piece sharing a bucket cannot merge without mislabelling one
    against the COMPOSITION CONTRACT, so mixed point+volumetric interiors still carry a ~2.7e-01
    residual; (2) the **same-kernel unmergeable residual**; and (3) the **holdout-connected gap** —
    connecting input 1 switches T13's fix off, so size-0 parity is met only with the holdout
    disconnected (worst |dα| 2.35e-01, up to 99.0% of pixels, and validation scene (b) will fail the way
    scene (a) did).
    Fix: in `FlattenScratch`, beside `claimStamp`/`claimBin`, keep a **per-bucket running alpha** for the
    current source pixel. When a fragment's deposit lands in a bucket already written by a fragment with
    the same `scatterKernelBin` at that pixel, scale that deposit's `alpha_k` and `colorScale_k` by
    `(1 − running_k)` — **per bucket independently**, never by the leading fragment's total alpha — then
    update `running_k`. Areas revert to the pre-T13 rule (both claim; the coverage clamp handles it),
    because with alpha composited rather than added, `aCov = min(a, cov) = a` and `local = a_true`, which
    is exact. Exact by construction: `1 − Π_k(1−A_k) = 1 − Π_k Π_i (1−a_{i,k}) = 1 − Π_i(1−a_i)`.
    Measured at T13's review on hand-built planes: ≤**4.2e-08** on alpha *and* premultiplied colour in
    every row where two fragments share `index0`, and never worse than additive elsewhere.
    **It is label-neutral**, which is exactly why it closes the cross-kind hole — no `FragmentKind`
    decision is needed. No new plane, no composite change. **It also needs no holdout gate for the
    catastrophic case**: holdout transmittance is monotone in z and the front member is in front, so
    `vis_front ≥ vis_back` and a sample can never be carried from behind a card to in front of it — each
    fragment keeps its own depth and its own `vis`. Only the attenuation *factor* is stale when
    `vis_front < 1`, which is bounded and soft.
    **The two alternatives are already disproved** and must not be re-tried: merge-as-Volumetric loses
    the point's mandatory fractional split (banding; the K-knob failure already on record), and
    merge-as-Point gives a span piece a second fractional split on top of its boundary split (the +8.3%
    double-count). Whole-**fragment** attenuation is also disproved — systematically under, −9.8e-02 at
    truth 0.963, worse than plain additive on 8 of 12 rows; it is the *per-bucket* form that is exact.
    Cost: one float per bucket per thread (~512 B at K=128), no per-fragment scatter cost.
  - verify: (i) the size-0 **mixed** point+volumetric corpus reaches the same ≤2e-07 / 0%-over-1e-3 gate
    the pure-point and pure-span rows already meet, at every K ∈ {4,8,16,32,64,128} and `pre_merge` both
    ways; (ii) `SpanSplitPart` reconstruction identities and the four hand-built plane identities
    **bitwise** unchanged; (iii) `FrontToBackOver` untouched (M1.P3.T12 must still render it);
    (iv) **with a holdout connected**, size-0 parity matches the disconnected case to ≤2e-07 — that is
    the gate that fails today; (v) two opaque layers at one pixel read **exactly 1.000000** at any alpha
    pair and any kernel; (vi) the two-layer K-convergence table beats the current one at every K.
    Delete T13's one-slot merge and its gates if this supersedes them, and say so explicitly rather than
    leaving both mechanisms in. Local build and both suites green; mutation-test any new cases to the
    suite's established bar.
  - size: L

> **T12 was split at 2026-08-16** (sizing rule, §6): as written it was a harness build, a
> twelve-scene sweep, two independent bake-off decisions and two source deletions in one task —
> far past "one coherent change". It is now **T12** (harness + scenes a–f), **M1.P3.T16**
> (scenes g–l), **M1.P3.T17** (bucket-composite bake-off) and **M1.P3.T18** (holdout-interpolant
> bake-off). See this file's Decisions.
>
> **Execution order: T12 → T16 → T19 → T17 → T18.** T19 (the kernel-bin trough M1.P3.T16 found)
> is sequenced *before* the two bake-offs on purpose: it changes `DiscKernelLUT`, so it moves every
> rendered pixel, and T17/T18 decide from rendered pixels. Landing it afterwards would mean both
> decisions were taken on pre-fix imagery and the inherited comparison tables would be stale.

- [x] M1.P3.T12 — Headless validation harness + scenes (a)–(f)
  - files: `tests/nuke/` (new — harness + scene builders, Python)
  - approach: with M1.P3.T5's serial wiring in place and T13/T15 landed, build the **scripted
    headless harness** every remaining validation task reuses, then run **scenes (a)–(f)** of the
    Design reference's list through it: `NUKE_PATH=<build dir> /usr/local/Nuke17.0v3/Nuke17.0 -t
    <script.py>`. The harness must build each scene from Python nodes (no committed `.nk` — that is
    M1.P5.T3's job), render, and report **numeric** pass/fail against each scene's stated check,
    not eyeballed pixels. It must expose `combine`, `holdoutInterp`, `K` and `pre_merge` as harness
    parameters, since T16/T17/T18 drive the same scenes at different settings.
    Carry these findings in:
    **scene (b) must be built with `DeepHoldout2`, not `DeepHoldout`** — the latter's input 1 is a 2D
    depth image and cannot take a deep input, which is what made it look unbuildable headless; pick
    the reference deliberately, since `DeepHoldout2`'s flatten differs from volumetric `DeepToImage`
    by |dc| 3.8e-03 on 11.9% of pixels on volumetric spans. Scene (a)'s parity tolerances are the
    three scoped ones in the Design reference's scene list (≤2e-07 absolute, **not** 0 ULP), and its
    volumetric row **must pin `volumetric_composition` ON** on the `DeepToImage` it compares against.
    **Expect scene (f) to show the M1.P3.T10 log-chord erasure** — that is the documented residual,
    not a T12 defect, and T18 is where it is judged; report it, do not "fix" it here. Connecting even
    a *non-occluding* holdout moves defocused pixels by up to 1.78e-01 through merge regrouping, so
    **set `pre_merge` and `combine` explicitly in every render** — never rely on a default.
    **Scene (e)'s `Escape` mid-cook cancel clause cannot be exercised headless** (same limitation
    M1.P3.T5 hit: `nuke.cancel()` from a timer thread does nothing, `SIGINT` kills the process) —
    run the rest of (e), and leave the cancel clause to the interactive pass M1.P3.T5 already owes.
    Report band-alpha and flat-field readings **separately**: the same input reads two orders of
    magnitude apart between them (a full-range α=0.9 fog slab is +6.65% as an isolated band-alpha sum
    and 0.8999999 as a flat field). Report scene (f) fog density against M1.P3.T8's coverage-head
    fix, and expect a residual ~4%/layer loss where fragments with *different* split fractions share
    a bucket (measured: two fully-covering 50% fog layers give 0.7297 vs the exact 0.75).
    This task changes **no** source under `src/` — if a scene fails, report it with numbers; the fix
    is a new task, not a silent edit.
  - verify: scenes (a)–(f) each pass their stated check with reported numbers, or are reported as
    failing with a measured magnitude and the pixel population affected. The harness re-runs from a
    single command and is committed. Local build and both unit suites stay green.
  - size: L

- [x] M1.P3.T16 — Validation scenes (g)–(l) on T12's harness
  - files: `tests/nuke/` (extending T12's harness)
  - approach: run the remaining six Design-reference scenes through T12's harness: (g) banding on a
    ground plane receding through focus, (h) overlap normalization, (i) sparse reveal / coverage
    deficit, (j) anamorphic, (k) proxy + ray-distance, (l) small-CoC transition.
    **Scene (g) needs a *steep* ramp**: the bucket deficit scales with how many buckets a destination
    pixel's CoC neighbourhood straddles, not with K alone (a gentle ramp lost only 4.8% end-to-end
    versus 35.6% in the synthetic K=16 worst case), so a shallow ramp understates the effect T17 must
    judge — and note from M1.P3.T4 that the `over`-vs-partition discriminator is a field whose
    fragments land in *different* buckets with `frac == 0`, which is exactly what this ramp must
    produce. Compare K=8 vs K=16 vs K=64 as the scene specifies. **Scene (i) must NOT be built via
    `DeepMerge`** — those scenes carry full occluded information and cannot show the deficit; the
    expected result is the *documented* honest alpha dip, so it passes by matching the spec, not by
    the dip's absence, and `pre_merge` moves the coverage plane that diagnoses it, so set it
    explicitly both ways. Scene (l) was closed by T13/T15 — re-confirm it here rather than assuming.
    Same discipline as T12: `combine`/`holdoutInterp`/`pre_merge` explicit in every render, and
    **no `src/` changes** — a failure is reported with numbers, not patched here.
    **Carried from M1.P3.T12's review — `pre_merge`'s plumbing is unproven.** Toggling it produced
    **0 pixel difference** on every configuration tried (overlapping slabs, three adjacent thin slabs,
    a 40-sample fog, textured point layers). That is consistent with the design ("lossless when radii
    are equal") and `fp.preMerge` is genuinely read at `src/DeepCDefocusScatter.cpp:961`, but unlike
    `combine`/`holdoutInterp`/`K` there is no positive proof it reaches the render. Scene (i), which
    this task already runs both ways, is the natural place to settle it — **if there is still no
    delta there, chase reachability and report it**, since scene (i)'s diagnosis depends on the knob
    actually moving the coverage plane.
  - verify: scenes (g)–(l) each pass their stated check with reported numbers, or are reported as
    failing with a measured magnitude and affected pixel population. Scene (g) is reported at K=8/16/64
    under **both** `combine` candidates, so T17 inherits the comparison rather than re-rendering it.
    `pre_merge` either demonstrably moves pixels somewhere, or is reported as unreachable with the
    evidence. Local build and both unit suites stay green.
  - size: L

- [x] M1.P3.T19 — Kernel-bin quantisation trough on small CoC (closes validation scene (l))
  - files: `src/DeepCDefocusKernel.h`, `tests/test_defocus_scatter.cpp`, `tests/nuke/`
  - approach: found at M1.P3.T16, whose review reproduced it **from first principles, independent of
    the harness**. `DiscKernelLUT::radiusToIndex()` is `lround(radius / 0.5)`
    (`src/DeepCDefocusKernel.h:503`), so kernel radius is quantised onto a 0.5 px grid with bin edges at
    `radius = n·0.5 + 0.25`. Adjacent scanlines that straddle a bin edge rasterise *different* discs, and
    the adjacent-row deficit is exactly `(f_r(0) − f_{r+0.5}(0))/2` — predicted from the LUT alone and
    matched in Nuke to six decimals at every crossing: 0.5→1.0 **0.799119**, 1.0→1.5 0.905153, 1.5→2.0
    0.948266, 2.0→2.5 0.973739. On a 0–2.5 px ramp that is a **one-scanline 20.09% dark trough** at CoC
    radius 0.762 px, on 14 of 238 interior rows, every one within 1.8 scanlines of a bin edge (measured,
    not asserted). **It is energy loss, not ripple**: the matching +5.2%/+7.8% surplus on the other side
    of the edge is destroyed by the saturation clamp (profile max is exactly 1.0000000; net deficit 1.070
    alpha-rows over 238). **Colour tracks alpha exactly** (R/A = 0.400000 everywhere), so it reads as a
    visible one-pixel dark line across an opaque surface, not an alpha-only artefact. **The 1D ramp
    understates it** — same slope turned diagonal reads 0.675220 (−32.5%) and radial 0.649560 (−35.0%).
    **Four causes are already excluded and must not be re-investigated**: not the sharp-path threshold
    (the r=0.5 LUT entry's centre weight is 1.000000, identical to the sharp path, so the 0.5 px crossing
    produces no step at all — the worst is at 0.75 px); not LUT normalisation (every entry sums to
    1 ± 2.2e-08); not bucketing (bit-identical at K=8/16/64); not a harness artefact (the same radius
    held constant over the frame is flat to 0.0).
    This is the "watch the 0.5–1.5px transition band for chatter; if it chatters, add a sharp↔defocused
    blend zone rather than lowering the threshold" case the Design reference anticipated. Two shapes
    worth trying: **interpolate between adjacent LUT entries**, or make the radius step **adaptive below
    ~3 px**. Do not simply lower the threshold — the Design reference rules that out, and the trough is
    at 0.762 px where the sharp path is not even engaged.
  - verify: harness checks `l1`, `l2` and `l5` go from FAIL to PASS (gate 1/255 ≈ 3.9e-03), including
    the diagonal and radial 2D ramps, which are the worst cases. Scene (g)'s readings, which currently
    exclude rows within 2.5 px of focus to keep this artefact out of the bucket-composite numbers,
    should be re-run **without** that exclusion and reported. No regression anywhere else in the harness
    (scenes (a)–(f) are currently PASS=28 FAIL=0 XFAIL=3, and (g)–(k) must not move). Add a POD-level
    unit test pinning the adjacent-bin deficit so this cannot silently return. Local build and both unit
    suites green.
  - size: M

- [x] M1.P3.T17 — Decide the bucket composite, delete the loser (needs T12 + T16)
  - files: `src/DeepCDefocusScatter.h`/`.cpp`, `src/DeepCDefocus.cpp`, `tests/`, this file's
    `## Decisions`
  - approach: decide between `BucketCombine::FrontToBackOver` and `BucketCombine::CoveragePartition`
    (`src/DeepCDefocusScatter.h:1079`) **from rendered pixels**, per the 2026-07-26 answer to the
    bucket-composite alpha deficit. Judge on scenes **(c), (f), (g), (i)** rendered through both
    candidates on T12/T16's harness — `ScatterParams::combine` set explicitly for each render, never
    the provisional default. **Re-render the table rather than quoting it**: M1.P3.T19 has landed and
    changed `DiscKernelLUT`, so every scene-(g) figure below was measured on the *old* kernel and has
    moved. What you inherit from T16 is *where to look*, not the figures. **Scene (c) no longer
    discriminates** — `over` passes it at K=8, so the design
    reference's "plain `over` is known to fail scene (c)" is stale on constant-depth content. **Scene
    (g)'s ramp is the decisive one**: pre-T19 it read 0.997176 / 0.978461 / 0.922105 at K=8/16/64 under
    `over` against 0.989520 / 0.995558 / 1.000000 under `CoveragePartition` — opposite directions in K,
    partition converging to exact, `over` diverging to −7.8%. Note T16 excluded rows within 2.5 px of
    focus from scene (g) to keep T19's trough out of these numbers; once T19 has landed, re-run
    **without** that exclusion. Weigh scenes (f)/(g) *interiors* separately from scene (i)-style sparse
    content (band-alpha vs flat-field readings differ by two orders of magnitude on the same input).
    In front of focus candidate 2 is now exact, so the comparison is fair; **behind focus, expect the
    structural +45.2/+51.2/+59.1% at 3/4/8 buckets under *both* candidates** (plain `over` tracks the
    same numbers because it ignores the area planes entirely) — that residue is settled and is not a
    reason to prefer either — **but M1.P3.T19 moved this column and the old figures are wrong.** The
    behind-focus partition residue is now **36.86/50.25/56.31/61.00%** at 2/3/4/8 buckets (validated
    against a grid-free oracle at 36.91/50.25/56.40/61.02) while `over` barely moved
    (48.77/75.03/90.96/117.10), so **the gap between the candidates at 2 buckets narrows from 20.9 to
    11.9 points**. Read those numbers, and know the mechanism is **disc mis-sizing at small radii**, not
    collision suppression — the fabricated version of that story is recorded in Decisions precisely so
    it is not reasoned from again.
    **Two new discriminators M1.P3.T19 handed you**, both small but real and both candidate-dependent:
    harness check `l3` (5.226e-03 under partition against 2.186e-04 under `over`) and scene (l)
    generally (`l1` 2.176e-03 vs 4.296e-04, `l5` 3.667e-03 vs 2.935e-03). Small-CoC content now
    discriminates where it previously could not — the old kernel grid masked it.
    **The different-split-fraction residual, by contrast, IS
    candidate-discriminating** — the 2026-07-26 Decisions entry claiming it is identical under both
    was corrected at M1.P3.T12's review: it is signed and the sign flips (harness f3c/f3d at K=4/8/16,
    partition −0.763/−0.610/−0.113% and −1.539/−1.395/−1.675%, `over` +2.194/+0.212/+0.074% and
    +4.799/+1.956/+0.967%). Weigh it as evidence.
    **Start from M1.P3.T12's largest discriminator**: under `FrontToBackOver` the holdout fog reading
    (harness check `f1`, single opaque point fragments against the analytic `(1−α)^t` — the simplest
    possible content) is off by **2.500e-01** worst case, against **4.367e-08** under
    `CoveragePartition`. That is the strongest signal the harness produces and it must be explained,
    not skipped. Then **delete the losing path, its enum value, its flag,
    and the accumulation plane the winner does not read**, and prune the tests that only existed to
    pin the loser. The behind-focus residue is pinned by tests, so a change that moves it fails the
    suite and needs adjudication here rather than a silent tolerance bump.
  - verify: the decision is recorded in this file's `## Decisions` with the side-by-side numbers that
    drove it. The losing path, flag and unread plane are gone from `src/` (grep clean). Local build
    and both unit suites green. Scenes (c), (f), (g), (i) re-run on the surviving path and still meet
    their checks.
  - size: L
  - **DONE 2026-08-16. `CoveragePartition` wins; `FrontToBackOver` is deleted.** Full numbers in
    `## Decisions`. Harness **PASS=76 FAIL=0 XFAIL=12 SKIP=1, exit 0** (from 77/0/18/1): the eight
    scene-(g) rows that rendered the losing candidate are gone (7 XFAIL + 1 PASS) and one new bounded
    XFAIL `g4` is added; **every other reading in the suite is bit-identical to the pre-deletion run**,
    verified by diffing the two tables. Both unit suites green (27 cases / 141,034 assertions and
    53 / 175,179). Nothing in `src/` mentions the deleted rule except the two comment blocks that
    record why it went. **There was no unread accumulation plane to delete** — the surviving composite
    reads all four, and the two area planes are exactly what the loser ignored.
    **It found a new residual on the winner** (`g4`, see Decisions): the same scene-(g) ramp at
    **α < 1** reads −16.1% at K=16/α=0.90 and diverges in K to −26.6% at K=128, against −0.4% on the
    opaque twin. Three controls attribute it to the transmittance split's recombination and exclude
    the kernel, the sharp path and depth quantisation; the exact pooling term is **not** isolated and
    is recorded as unproven rather than guessed. Pinned as a **banded** XFAIL (0.1607 ± 0.025) at a
    fixed K=16, mutation-tested against three separate mutations of the composite (0.1020 / 0.0777 /
    0.1925 — all three FAIL the band). **It is a Phase 1.4/1.5 follow-up, and it is the largest
    known error in the shipped node.**
  - **REVIEWED 2026-08-16 (independent). The decision stands; five things were corrected.** The
    reviewer re-derived it from its own renders rather than auditing the numbers in place: it built
    the pre-deletion tree at `6cbf28b` in a worktree and re-ran the harness under `--combine over`,
    reproducing `f1` = 2.500e-01 (and confirming `2·vis − vis²` at all six strips whose depth splits,
    while the two strips at the depth range's endpoints, which have no split partner, read EXACT —
    so the error tracks the depth split itself and is not an artefact of how scene (f) is built),
    scene (l)'s five readings, scene (c)'s c4, and scene (g)'s whole K
    sweep at α = 0.99 / 0.90 / 0.50 to the sixth decimal. The bit-identical claim was verified by
    diffing the two full report tables: the ONLY differences are the eight deleted rows, the g1/g2/g3
    label renames, two note-text edits and the new `g4` — every numeric reading is unchanged. Both
    unit suites reproduce at 27/141,034 and 53/175,179. Corrections: **(1)** `g4`'s mechanism is no
    longer unproven — it is isolated above; **(2)** scene (i) is not literally candidate-blind (i7 /
    i7d differ); **(3)** partition's errors are not all deficits at row granularity; **(4)** scene
    (l)'s cost is 1.33 code values, not ~1, and `l5` ships at 6% of gate where the deleted candidate
    was at 25%; **(5)** `G1_HARD` was left at 1.5e-01, sized across BOTH candidates, against a
    surviving worst reading of 1.048e-02 — 14x, wide enough to swallow an order of magnitude of new
    defect. Re-sized to 3.5e-02 (g2 and g3 were already ~3x and are unchanged). The three tests that
    lost `compositePixelFrontToBack()` as an oracle were mutation-tested with a linear
    `partitionAlpha()` and all three still FAIL, so none went circular. `g4`'s band was
    mutation-tested with a fourth, independent mutation (area-weighting the residual's occlusion,
    which is algebraically the pre-T9 divisor): 0.0777, FAIL.

- [x] M1.P3.T20 — Rule on the α<1 receding-content residual (`g4`) — run BEFORE T18
  - **RULED 2026-08-16: FIXED, in the composite.** Lead (b) (the linear split) was built and
    **disproved** — exact on a mosaic, but it makes a surface self-occlude in the `excess` regime and
    reads 0.875 against a true 1.0 on two same-pixel layers, i.e. it fails scene (a)'s parity gate by
    five decades. `partitionAlpha()` is untouched. The mechanism T17's review isolated was fixed
    directly instead: a co-located deposit is attenuated by the tile *its own head* claimed
    (`tHead`), not by the pooled `tClaimed`, and a residual's occlusion is subtracted from that mean
    in proportion to the area it covers. Isolated rig: **exact** at every N, α and split fraction
    (was −17.4% / −38.9%). Harness `g4` **0.1607 → 0.0543**, re-pinned in the same change; the
    K-divergence is **gone**; the `g1`/`g2`/`g3` XFAILs **retired** (five to six decades better) and
    `l3` went to 0.000e+00; harness **PASS=83 FAIL=0 XFAIL=5 SKIP=1, exit 0**; both unit suites green.
    The remaining 5.4% is `f3c`/`f3d`'s pooling term, a different mechanism, and is *not* claimed
    fixed. **REVIEWED 2026-08-16: ruling upheld, one regression found and pinned, four claims
    corrected in place** — staggered multi-part parents now read up to **+18.3% HIGH** (was 4–16%
    low), `l3` was still an `expectedFailure` rather than a plain check, the remaining g4 term is
    triggered by any split fraction ≠ 0.5 rather than by *differing* fractions, `claimA = cov` is
    caught by g1/g2/g3 as well as g4, and `a3`/`l1` moved besides `g4`. Full record in
    `## Decisions` (the review entry precedes the task's own).
  - files: `src/DeepCDefocusScatter.h`/`.cpp`, `src/DeepCDefocusMath.h`, `tests/test_defocus_scatter.cpp`,
    `tests/nuke/scenes.py`, this file's `## Decisions`
  - approach: **this is the largest known error in the shipped node** and it is not acceptable to
    carry it to a milestone gate as a floating obligation. An ordinary semi-transparent surface
    receding through focus loses **−12.5 / −16.1 / −9.2%** of its alpha at α=0.99/0.90/0.50 (K=16) and
    **diverges in K** (−5.0 → −26.6% at K=8 → 128 at α=0.5), worst case **−38.9%** at split fraction
    0.25. The mechanism was isolated at M1.P3.T17's review on hand-built planes with no kernel at all,
    so it is not a scene or kernel artefact: the composite's single scalar `tClaimed` cannot represent
    a claimed area that a depth ramp has made a mosaic of differently-transmissive sub-areas.
    **It must run before M1.P3.T18** — the fix changes the split or the composite, which moves every
    rendered pixel, and T18 decides the holdout interpolant from rendered pixels. Same reasoning that
    put T19 before T17.
    **Lead (b) is the one to try first**, and M1.P3.T17 is what made it available: the transmittance
    split (`α_i = 1 − (1−α)^{w_i}`) exists *only* so two deposits reconstruct the parent under the
    front-to-back `over` that T17 has now **deleted**. With that composite gone its rationale is gone
    with it, and on the isolated rig a **linear** split (`α_i = α·w_i`, each half claiming its own new
    area rather than arriving co-located) is **exact** — 0.900000 / 0.500000 at every N and both split
    fractions, where the transmittance split reads −17.4% / −22.4%. But that is arithmetic on one
    synthetic pixel: it says nothing yet about volumetric parents, holdouts, within-bucket ordering, or
    the depth-interpolation continuity the transmittance form was *also* chosen for. **Establish those
    four before adopting it**, and note the Design reference's own justification for the transmittance
    split is `over`-specific and must not be re-derived from (it is already flagged in place).
    **Lead (a) is disproved as a free fix and must not be re-tried on its own**: area-weighting the
    second consequence is algebraically identical to reverting the residual divisor to the pre-M1.P3.T9
    `claimedArea`; it takes `g4` from 0.1607 to 0.0777 and improves g1/g2/g3, but moves T9's pinned
    behind-focus residue from 61.00% to 70.53% and fails the unit suite. T9 already adjudicated that
    trade.
    If neither lead survives contact, **an accepted-residual ruling is a legitimate outcome** — but it
    must be an explicit ruling with the node-help text to match, not a deferral.
  - verify: either `g4` is fixed — in which case **re-pin it in the same commit**, since it is banded
    and can never PASS as written — or the residual is explicitly accepted, documented in node help
    alongside the coverage-deficit spec, and the band re-stated as intended behaviour. Either way:
    the K-divergence at α<1 is characterised across K ∈ {8,16,64,128} and both split fractions; T9's
    pinned behind-focus residue and every other pinned constant either hold or are re-pinned against
    an **independent oracle** with the mechanism stated; the harness stays green with no XFAIL bound
    raised; local build and both unit suites green.
  - size: L

- [x] M1.P3.T21 — Staggered multi-part parents read HIGH (M1.P3.T20's regression) — run BEFORE T18
  - files: `src/DeepCDefocusScatter.h`/`.cpp`, `tests/test_defocus_scatter.cpp`, `tests/nuke/scenes.py`,
    this file's `## Decisions`
  - approach: found at M1.P3.T20's review. **T20 flipped this content's error into the direction the
    honest-alpha contract forbids.** Two multi-part parents at *overlapping* depth ranges — two fog
    slabs, or a fog slab and a point fragment, whose kernel weights tile one destination pixel — now
    read **up to +18.3% HIGH**, where before T20 they read 5–11% low: 3 parts/offset 1/w=0.75/α=0.90
    goes −5.42% → **+11.11%**; 4/2/0.50/0.90 −9.73% → **+11.11%** (saturating to alpha 1.000000);
    4/1/0.50/0.90 −11.24% → **+9.35%**; volumetric+point −10.59% → **+4.56%**. Swept, **377 of 525
    cells now read >+0.5% high**. Truth needs no ordering assumption here: the coverages sum to 1 and
    both fit, so the parts tile the pixel as disjoint sub-areas at the same α and the answer is exactly
    α. Two fog slabs at overlapping depths is ordinary comp content, not a corner case.
    **Cause**: the composite carries **one** `(tHead, headArea)` pair, so when a bucket both claims area
    and continues a chain the merge rule must discard one tile. That is free only while the discarded
    chain has no deposits left — and here it has. **T20's own volumetric check could not see this**: it
    indexed parts as `k = j*(P+1)+i`, i.e. **non-overlapping** runs, which are exact and stay exact.
    Another instance of the standing pattern — a check that never reached the phenomenon.
    **Both cheap alternatives are already disproved and must not be re-tried**: always carrying the
    chain reads −9.3% on the dense ramp, and merging by area reads −5.9% there, takes `g4` to 0.0913
    and fails 40 unit assertions. The fix is **more state** — carry more than one `(tHead, headArea)`
    tile — so the question is how many are needed and what that costs per pixel per bucket. Note the
    composite currently costs one float per bucket per thread; state the new figure.
    **Do not fix this by clamping**, which would hide it rather than correct it, and do not reopen
    M1.P3.T17: `over` was worse on this content too and fails identities partition satisfies exactly.
    Two **pre-existing** upward errors are pinned nearby and are NOT this task's (both are bit-identical
    across T20): the excess-regime rear double-count (up to +94.8%) and the +8.3% free/claimed straddle.
    Do not fold them in silently — if your change moves them, say so and adjudicate separately. An
    excess-tile fix for the first is exact on hand-built planes but **renders decisively worse**
    (`g4` 0.0825, g1/g2/g3 all worse); it was measured and withdrawn, so do not re-propose it untested.
  - verify: the staggered-parent sweep reads within the honest-alpha contract — **no cell above +0.5%**
    — across parts ∈ {2,3,4}, offsets, w and α, with the pinned band in `tests/test_defocus_scatter.cpp`
    re-pinned to the new behaviour **against an independent oracle**, not re-fit. The α<1 ramp gains
    T20 bought are kept: `g4` ≤ 0.0543+band, and g1/g2/g3 stay at their post-T20 ~1e-06..1e-08 readings
    rather than regressing toward 1e-02. K sweep at α ∈ {0.5, 0.9, 0.99} across K ∈ {2..128} shows no
    divergence reintroduced. Scene (a)'s size-0 parity still ≤2e-07 (note `a3`'s margin already halved
    at T20 to 1.192e-07 against a 2.4e-07 gate — do not spend the rest of it silently). Per-pixel and
    per-bucket cost stated. Harness green with no XFAIL bound raised; local build and both unit suites
    green.
  - size: L

- [x] M1.P3.T22 — Gate the unequal-density over-read (run FIRST — T23 cannot iterate without it)
  - files: `tests/nuke/scenes.py`, `tests/test_defocus_scatter.cpp`
  - approach: found at M1.P3.T21's review. **The node's largest rendered error is currently ungated.**
    Two cards side by side at α 0.99 and α 0.10 with overlapping depth spans, `size` 14, K=16 — a dense
    fog card beside a thin one, ordinary comp content — read **+71.2%** at the worst pixel, with 41 of
    186 probed pixels over +0.5% high. It is in the direction the honest-alpha contract forbids.
    **Nothing sees it**: `f3c`/`f3d` are the only overlapping-depth rendered checks and **both pin equal
    density**, which is the one case that is nearly exact. That is the fifth instance in this milestone
    of a check structurally unable to reach the phenomenon it guards, and it is why this task must
    precede any further composite work — M1.P3.T23 would otherwise be iterating blind.
    Add an **unequal-density** variant of the `f3c`/`f3d` family with a **hard bound**, plus the
    matching POD-level pin. The oracle needs no ordering assumption: below saturation the area model is
    additive, so the merged render must equal the sum of the two solo renders, and each solo is a single
    open chain, which the composite does exactly. Worst pixel on record: solo 0.21431 + 0.03817 =
    0.25248 against a merged **0.43220**.
    Sweep density ratio, depth overlap, `size` and K rather than pinning one configuration — the point
    is a gate that *tracks* the defect as T23 moves it, not a single number.
  - verify: the new check FAILs on current `HEAD` at the recorded magnitude and is **mutation-tested**
    in both directions — it must also fail if the composite is perturbed toward *under*-reporting, so it
    cannot be satisfied by trading the error's sign. Equal-density `f3c`/`f3d` readings stay
    bit-identical. No existing bound raised. Harness stays green apart from the new expected failure,
    which is recorded as a **FAIL, not an XFAIL** — it is the target T23 must close. Local build and
    both unit suites green.
  - size: M

- [x] M1.P3.T23 — Composite iteration against the unequal-density over-read (needs T22)
  - **CLOSED 2026-08-16 as a MEASURED NEGATIVE RESULT — no code changed.** Five candidates built,
    three rendered through T22's gate behind a runtime switch (control off = every reading reproduced
    row for row). None beats the trade. The decisive finding is that `f3f`'s `overlap 100%` cell
    (+80.428%, and bit-unchanged by the fifth plane) is a bucket-level pooling that NO composite rule
    can reach at any plane count, and the part that IS reachable costs the fifth plane plus a
    −38.6% deficit arm and turns the exact `f3h` control into a FAIL. The fifth plane, measured end to
    end, retires `f3c`/`f3d` exactly but moves the correctness target 2.7 points — which confirms the
    user's "polish, not correctness" ruling on it rather than overturning it. Full numbers, both axes
    per candidate, the K/α sweeps and the costs are in `## Decisions`, 2026-08-16.
  - **INDEPENDENT REVIEW, same day — negative result ACCEPTED, with three amendments.** The baseline,
    both unit suites, `a3`, the whole `f3e`–`f3i` family and a from-scratch fifth-plane build were
    reproduced independently; the diff is comment-and-documentation only and no experiment tree can be
    reached by `run_validation.sh`'s newest-`.so` search. (a) The impossibility argument holds **for
    the four planes** but its "at any plane count" clause is withdrawn: a second-moment plane plus the
    plan's own untried *opacity-band tile split* takes the coincident cell to +0.000% on POD while
    leaving T21's staggered exactness and the dense ramp bit-identical — it still does not beat the
    trade, so the ruling is unchanged. (b) Correction 1 is confirmed and its unexplained 88% is now
    bounded on two sides. (c) **Correction 2 is retracted**: the brief's +6.53%/+5.17% are `g4`-rig
    figures, they reproduce to three digits, and low α IS part of the target — that arm of the verify
    clause is still open. See `## Decisions`, 2026-08-16.
  - files: `src/DeepCDefocusScatter.h`/`.cpp`, `tests/test_defocus_scatter.cpp`, `tests/nuke/scenes.py`,
    this file's `## Decisions`
  - approach: **the user directed continued composite iteration on 2026-08-16, having been shown the
    evidence that it has reached a Pareto point** (see Decisions). This task carries that direction, and
    is scoped by the user's second instruction — **fix correctness, defer polish**:
    - **In scope: the +71.2% unequal-density over-read.** Invented alpha, forbidden direction,
      ordinary content. This is the correctness target.
    - **Out of scope: `f3c`/`f3d` and `g4`.** Both are precision-only residuals that cost accuracy, not
      correctness. They stay pinned and documented. **Do not add the fifth (co-located alpha) plane** —
      it closes exactly those two, does nothing for the over-read, and costs +14% bucket memory at C=4
      against a standing High memory risk. If you find it is unavoidable for the correctness target,
      that is a finding to report, not a licence to add it.
    **What is already established and must not be re-derived**: there are two independent losses. The
    `C_k : D_k` split is recoverable (a fifth plane closes it exactly — measured). **Parent identity is
    not recoverable by any bounded plane count**, because parents-per-bucket is unbounded, and that is
    the term reading +71%. Newest-first and oldest-first ordering **trade**: newest gives +64.6% on the
    unequal-alpha case and −4.107% on the dense ramp; oldest gives +22.4% and −27.4%. Both were measured
    at T21's review. So a rule that merely reorders the stack is not progress — **a candidate must beat
    that trade, not sit somewhere on it**, and the report must show where it lands on both axes.
    Ideas worth considering, none endorsed: bounding the *sign* rather than the magnitude (accept a
    deficit, forbid invented alpha — the contract only forbids one direction, and cap overflow already
    degrades downward); an opacity-aware tile-selection rule rather than a positional one; splitting the
    tile stack by opacity band; or detecting the unequal-density case and falling back to a conservative
    rule there. **If measurement says none of them beats the trade, that is a legitimate reportable
    outcome** — say so with the numbers, and the ruling then becomes documentation.
    **Three corrections from M1.P3.T22, which changed this task's scope — read them before starting:**
    1. **"Unequal density" is the wrong name for the target.** The over-read needs a **multi-part
       parent**, not a density ratio: two *identical* α=0.99 fog cards **staggered by one depth unit**
       read **+8.373% high** on 641/1076 px (α=0.50 twins read +2.931%). That was first deferred as
       `f3c`/`f3d`'s accumulation-time term and **that attribution was wrong** — mutation shows it goes
       to +0.0001% under `tHeadIn=1` while `f3c`/`f3d` go to +33.333%, so it is the **same
       composite-side tile allocation** as the +77% case and it **is in scope**. It is also maximally
       ordinary content, which makes it the more important of the two.
    2. **`f3e` has two arms and they are two different terms.** Closing the over-read alone leaves the
       low arm red: depth-**disjoint** parents read +0.000% high but **−3.278% low**, because a
       residual whose disc overhangs its own head tile spills onto a foreign parent's tile. Permitted
       direction, but do not read `f3e`'s persistent FAIL as "the target is still open" without
       checking which arm.
    3. **The conservative "occlude by the densest tile" idea listed above is already measured and does
       not work as stated**: it turns +77.4% into **−52.3% on 244/738 px** and `f3e` still FAILs.
       Bounding the sign is still a legitimate direction, but that naive form is spent.
    Also note the gate's own limits, stated in its header: the oracle is a ratio of two renders of the
    same node, so it is **invariant to any uniform scaling** of the composite's output — `f3i` and
    `f3c`/`f3d` are what catch that class, so do not let them regress. And read `f3f`'s `Cells:` detail
    rather than its headline, which reports only the worst cell's own two arms.
  - verify: M1.P3.T22's gate passes, or its magnitude is materially reduced with the remainder bounded
    and no cell in the forbidden direction beyond the stated bound. **The T21 gains hold**: the
    staggered sweep stays at zero cells >+0.5%, `g4` ≤ 0.0325+band, g1/g2/g3 stay at their post-T21
    readings. Scene (a) parity ≤2e-07 with `a3`'s remaining margin (1.192e-07 of 2.4e-07) not spent.
    K sweep α ∈ {0.1, 0.3, 0.5, 0.9, 0.99} × K ∈ {2..128} — note α=0.10 and 0.30 currently read +6.53%
    and +5.17% and stay above +3.4% at the default K=16, so **low α is part of the target, not a
    footnote**. Every pin that moves is re-pinned against an independent oracle with the mechanism
    stated. Cost stated per pixel, per bucket, per thread and at K=128. Harness green with no XFAIL
    bound raised; local build and both unit suites green.
  - size: L

- [ ] M1.P3.T24 — Gate and rule on the low-α ramp over-read (the target M1.P3.T23 never measured)
  - files: `tests/nuke/scenes.py`, `src/DeepCDefocusScatter.h`, `tests/test_defocus_scatter.cpp`,
    this file's `## Decisions`
  - approach: **M1.P3.T23 discharged the user's "keep iterating" mandate on the `f3e`/`f3f` over-read
    and returned a measured negative result. This is a *different* target that its search never
    touched**, and it is the more ordinary content of the two.
    On **scene (g)'s α<1 ramp — the `g4` rig, NOT the `f3e`/`f3f` family** — the over-read scales
    **inversely** with α: **+6.526% at α=0.10 (K=2/4), +5.926% at α=0.10 (K=16)**, +5.166%/+3.399% at
    α=0.30, +3.757%/+0.897% at α=0.50, and −3.253% at α=0.90 (which is `g4`'s own 0.0325 pin, and is
    what validates the probe). It is **invented alpha in the forbidden direction, on ordinary content,
    at the shipping default K, and it is ungated at α ≤ 0.30.**
    **Read this history before starting.** T23 reported these figures "did not reproduce, by a factor
    of ~18" and proposed the plan had transposed them. **That was wrong — T23 measured the wrong rig**,
    the `f3e`/`f3f` family instead of scene (g)'s ramp, where the α scaling genuinely runs the other
    way (twins read +0.370% at α=0.10). Its own table then self-contradicted the claim, α=0.30 at 10:1
    reading +6.49%. The review rebuilt the correct rig and reproduced the plan's figures to three
    digits. This is the **seventh** instance of the milestone's standing lesson and the first committed
    by a *correction* rather than an original claim — so **render scene (g)'s ramp, not
    `unequalDensityCell()`**, and say which rig every number came from.
    **Gate first, then rule** — the same discipline M1.P3.T22 established. `g4` currently pins one α;
    extend it to the α arm with a hard bound so the defect is visible before anything is changed.
    **What T23 established, which applies here too**: ordering alone is zero-sum; the four-plane layout
    cannot reach the coincident-span degeneracy; the dropped term is a **covariance** (the composite
    forms `E[a_res]·E[1−a_head]` where truth wants `E[a_res·(1−a_head)]`), and a **second-moment plane**
    is the cheapest thing that sees it — the review's opacity-band prototype (`Σ wᵢaᵢ²`) reaches the
    cell T23 called unreachable at zero cost on both trade axes, but doubles `f3c`/`f3d` and leaves the
    staggered half untouched. **That is the direction if a fix is attempted; it is not endorsed**, and
    its memory must be weighed against the standing High memory risk.
    **A negative result is again a legitimate outcome.** If no rule beats the trade here either, the
    ruling is: bound it, document it in node help alongside the coverage-deficit spec, and stop — but
    it must be an explicit ruling with the numbers, not a deferral, and **not** the "low α is mild"
    reading that T23's retracted correction would have shipped.
  - verify: the low-α arm is gated with a hard bound and the gate is **mutation-tested in both
    directions**, so it cannot be satisfied by trading the error's sign. Then either the over-read is
    materially reduced with the remainder bounded, or it is explicitly accepted and documented. Either
    way: α ∈ {0.10, 0.30, 0.50, 0.90, 0.99} × K ∈ {4..128} reported **from the `g4` rig**, with the rig
    named in every figure. T21's gains hold (staggered sweep zero cells >+0.5%), `f3c`/`f3d`/`f3i`/
    `g1`/`g2`/`g3` do not regress, and scene (a) parity stays ≤2e-07 with `a3`'s remaining margin
    (1.192e-07 of 2.4e-07) not spent. Harness green with no XFAIL bound raised; local build and both
    unit suites green.
  - size: L

- [ ] M1.P3.T18 — Decide the holdout interpolant, delete the losers (needs T12 + T16)
  - files: `src/DeepCDefocusMath.h`, `src/DeepCDefocusScatter.h`/`.cpp`, `src/DeepCDefocus.cpp`,
    `tests/`, this file's `## Decisions`
  - approach: decide among `HoldoutInterp::{LogChord, MidpointStep, LinearInT}`
    (`src/DeepCDefocusMath.h:462`) — M1.P3.T11's erase-FG-in-front vs leak-BG-behind trade — judged
    from scenes **(e)** and **(f)** on T12's harness, plus **a genuinely dense volumetric holdout**
    (many samples in depth, not a single fog slab). That last case is mandatory: T11's review
    disproved the assumption that these variants only affect fully-opaque content, since a run of α<1
    samples underflows the transmittance product to bitwise zero at ordinary counts (46 at α=0.9) and
    the three then diverge hard — so do **not** write the comparison up as risk-free on α<1 content.
    The M1.P3.T10 log-chord erasure is the thing being judged, not a defect to route around (Midpoint
    step read 16 wrong pixels near scene (f) against Log chord's 157). Then **delete the losing
    variants and the flag**, same discipline as T17, and prune the tests that only pinned them. The
    interpolant divergence is pinned by tests; a change that moves it fails the suite and is
    adjudicated here. **Re-render rather than quoting M1.P3.T12's figures** — T19 changes
    `DiscKernelLUT` and moves every pixel. What T12 established and T19 will not change is the
    *shape*: LogChord erases 78% of a `depthRange/K` bracket, MidpointStep 30%, LinearInT none
    outright but ramps 0.95→0.25 across it, and the LogChord decay is `10^(−30·frac)` — i.e. it is
    `kMinTransmittance = 1e-30` (`src/DeepCDefocusMath.h:61`) flooring `log T`, not a chord-accuracy
    limit. That distinction matters: a floor is cheap to raise, an irreducible chord bound is not.
  - verify: the decision is recorded in this file's `## Decisions` with the side-by-side numbers,
    including the dense-volumetric-holdout case. Losing variants and the flag are gone from `src/`
    (grep clean). Local build and both unit suites green. Scenes (b), (e), (f) re-run on the
    surviving variant and still meet their checks.
  - size: L

## Phase 1.4: Concurrency + performance

- [ ] M1.P4.T1 — Per-band lazy-claim concurrency
  - files: `src/DeepCDefocus.cpp`, `src/DeepCDefocusScatter.h`/`.cpp`
  - approach: replace Phase 1.3's frame-wide lock with per-band atomic state
    (`Dirty → InProgress → Done`) plus `DDImage/Thread.h` `Lock`/`Guard`/`Condition`; a claiming
    render thread computes its band into private bucket planes and writes a disjoint region of
    the shared flat frame; other threads block on that band until `Done`. `_validate` marks all
    bands `Dirty` on an `Op::hash()` change. Memory-limit knob caps concurrent in-flight bands
    per the formula in the Design reference (floor 1 band, then shrink B — never deadlock at 0),
    budgeting on the **combined** bucket-plane + SoA-fragment total per the Decisions entry: the SoA
    is the larger term at 4K (~1.49GB vs ~117MB), so a cap counting only the planes under-budgets by
    an order of magnitude. **`bytesForBand()` must also gain the holdout term**, which M1.P3.T3 left
    out: measured at 17.0 MB per 4096×64 band at K=16 / 2 samples per pixel (~578 MB per 4K frame),
    plus ~15.3 ms append and ~24.7 ms build per band (~1.4 s per 4K frame), both scaling with K. The
    exact size depends on M1.P3.T10's boundary set — `(K+1)·W·B·4` as T3 built it, unchanged if T10
    decouples the set at the same entry count.
    `Op::aborted()` checked per source row; an aborted band resets to `Dirty`, wakes waiters,
    leaves erased/black rows. Every `deepEngine()` bool return checked. Also **assert the planes'
    geometry against `params.bandWidth/bandHeight` caller-side**: `scatterBandCPU` silently returns
    when they disagree (found at M1.P3.T2's review), which under per-band claiming would surface as
    black bands rather than an error. **Budget this as rework,
    not extension**: M1.P2.T2's cache is a `shared_ptr<const FrameCache>` published by copy, which
    is the wrong primitive for per-band claims (they need a mutable shared frame plus per-band
    atomics), and its `engine()` takes the frame-wide lock on *every row* just to snapshot the
    pointer — ~2160 acquisitions per thread per 4K frame. Both are correct for the serial phase and
    both must go here.
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
    **Also review `merge_tolerance`'s default here** (carried from M1.P3.T16's review). The knob is
    documented as "lossless when radii are equal", but losslessness actually holds only when the
    grouped radii land in the *same* `DiscKernelLUT` bin: at the 0.25px default, two same-pixel layers
    0.20px apart that straddle a bin edge move **9.0e-02 on 100% of pixels**. So the shipping default
    trades correctness for speed, silently. Decide from the perf numbers this task produces whether
    0.25px is worth what it costs, and either re-document the knob honestly or lower the default —
    note M1.P3.T19 may change the bin grid underneath this, so run it after T19.
  - verify: `-fopt-info-vec` output shows the scatter loop vectorized (or the omp-simd fallback
    does); the 2K/20spp synthetic scene completes in a time you record in this file's Decisions
    section as the perf baseline for future regressions. `merge_tolerance`'s default is either
    changed or its docs corrected, with the speed-vs-accuracy numbers recorded.
  - size: M

## Phase 1.5: Integration polish

- [ ] M1.P5.T1 — CMake wiring
  - files: `src/CMakeLists.txt`
  - approach: **the plugin registration and `target_sources` half of this task moved forward to
    M1.P3.T7** (the build gate was vacuous without it); what remains here is the compile options and
    the cross-platform check. For reference, T7 added `DeepCDefocus` to `PLUGINS` and `FILTER_NODES`
    inside an `if (UNIX)` guard
    (verified pattern: top-level `CMakeLists.txt:11` already opens a UNIX block) and
    `target_sources(DeepCDefocus PRIVATE DeepCDefocusScatter.cpp)` following the
    `DeepCShuffle2`/`ShuffleMatrixKnob.cpp` precedent at `src/CMakeLists.txt:128-129` (not the
    FastNoise object-library pattern — different shape). This task adds `target_compile_options(DeepCDefocus
    PRIVATE -mavx2 -mfma)` per-target, so the existing global `-mavx` floor is unchanged for
    every other node. **Move or duplicate the `fp-contract=off` guard onto the scatter TU when those
    flags land** — `flattenPixel()`, which currently carries it, has had zero call sites since
    M1.P3.T5, so the guard protects nothing reachable while the arithmetic that produces shipped pixels
    sits unguarded in `DeepCDefocusScatter.{h,cpp}` (see Decisions). **FMA hazard**: `-mfma` under GCC's default `-ffp-contract=fast` fuses the
    flatten loop's multiply-add and destroys the `DeepToImage` bit-parity M1.P2.T2 established
    (the 1.19e-07 divergence returns). That file guards its composite loop with a
    `#pragma GCC optimize("fp-contract=off")`; if the pragma is ever removed, `-ffp-contract=off`
    must go on the target instead.
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
    stock `DeepToImage` rather than as a plain 2D box. The help text should also state that,
    unlike `DeepToImage`, this node does not synthesise a `depth.Z` AOV — a selected `Z` flattens
    as an ordinary data channel (M1.P2.T2 deliberately did not special-case `Chan_Z`).
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

- 2026-08-16 — **M1.P3.T23: a MEASURED NEGATIVE RESULT. No composite-side rule beats the trade, and
  most of the over-read is not a composite term at all. NOTHING IN `src/` CHANGED.** The task carried
  the user's direction to keep iterating; five candidate rules were built and the three that survived
  POD screening were **rendered** through M1.P3.T22's gate (each in its own build tree, behind a
  runtime switch, with the switch OFF as a control — the control reproduced all 86 PASS / 2 FAIL /
  5 XFAIL / 1 SKIP readings row for row).
  **WHERE EACH CANDIDATE LANDS ON BOTH AXES** (`f3e` high / low arm; the dense ramp is the POD
  `alpha 0.90, frac 0.25` row the newest/oldest trade was stated in, shipped value −4.107%):

  | candidate | `f3e` high | `f3e` low | dense ramp | staggered POD sweep | other |
  |---|---|---|---|---|---|
  | shipped (M1.P3.T21) | +77.411% | −2.011% | −4.107% | exact | `f3h` +0.078% |
  | (1) residual allocated by tile OPACITY, no new plane | +75.935% | **−49.613%** | −4.107% | exact | `f3h` **+6.510% FAIL** |
  | (2) opacity-MATCHED tile selection (POD only) | +47.15%¹ | — | −4.107% | **−25.03% low** | 32×32 overflow −5.13 → −15.29% |
  | (6) allocation weighted by each tile's own `1−T` (POD only) | +42.04%¹ | — | −4.107% | **−1.66% low** | 32×32 overflow −5.13 → **−25.09%** |
  | (8) the FIFTH PLANE, allocation unchanged | +74.702% | −3.132% | **exact** | exact | `f3c`/`f3d` **PASS**, `g4` 0.0325 → 0.0287 |
  | (9) fifth plane + opacity allocation | +34.664% | **−49.613%** | **exact** | exact | `f3h` **+6.510% FAIL** |

  ¹ POD-modelled `f3e` shape, not the render; (2) and (6) were rejected before rendering because they
  break M1.P3.T21's staggered exactness, which the unit suite pins as `|a − alpha| <= 3e-06`.
  **The naive conservative rule and both orderings stay rejected** on M1.P3.T21/T22's own readings.

  **THE FINDING THAT DECIDES IT — most of `f3e`'s high arm is NOT a tile-allocation term and no
  composite rule of any kind can reach it.** `f3f`'s `overlap 100% (coincident spans)` cell reads
  +80.428% and is **bit-identical** under the fifth plane (+80.428% → +80.428%) and moves 2.9 points
  under the best candidate there is. The reason is structural, not empirical: when two parents' spans
  coincide, both heads land in the SAME bucket and every later part likewise, so the planes for
  {A, B} are **numerically identical** to those of one parent at the pooled density — one
  `(C_k, D_k, A_k)` triple, one tile. A rule reading only **those four planes** cannot distinguish the
  two. It is `f3g`'s argument one level up, and `f3g` is exact only because a single-bucket
  pair has no residual to mis-attribute. What IS composite-reachable is the cells whose two heads land
  in DIFFERENT buckets, and candidate (9) does move those (`overlap 25%` +52.251 → +5.464%,
  `overlap 0%` +20.906 → +0.206%) — but it needs the fifth plane, it takes the permitted-direction
  deficit arm from −3.278% to **−38.636%** on the disjoint-span cell, and it turns `f3h`, an
  arrangement the composite is currently EXACT on, into a +6.510% FAIL. That is a trade, not a win,
  and the verify clause's "no cell in the forbidden direction beyond a stated bound" is not met by it.
  **So the ruling M1.P3.T21's review recommended stands, now on rendered evidence rather than on POD
  reasoning: gate the residual and document it.**

  **THE IMPOSSIBILITY ARGUMENT, AS NARROWED BY THIS TASK'S INDEPENDENT REVIEW.** The conclusion above
  is accepted, and the bit-identity is reproduced on the reviewer's own fifth-plane build
  (`overlap 100%` +80.428% → +80.428%, `overlap: spans disjoint` and `K 4` likewise bit-identical).
  Two clauses were too strong and are withdrawn:
  1. **"At any plane count" is false; "from these four planes" is true.** The term the composite drops
     on that cell is a **covariance**: it forms `E[a_res]·E[1−a_head]` where the truth wants
     `E[a_res·(1−a_head)]`. A plane carrying the second moment `Σ w_i·a_i²` supplies the within-bucket
     opacity SPREAD, and subtracting `s_res·s_head` from the residual's per-tile transmittance — the
     plan's own listed-but-untried **"splitting the tile stack by opacity band"** — is a **strict no-op
     wherever either spread is zero**, so M1.P3.T21's staggered exactness (4/8/16/32 parents) and the
     dense ramp (N=2/16/64 × α=0.50/0.90/0.99) stay **BIT-IDENTICAL**, while the coincident two-parent
     shape goes from **+17.6…+86.3% to −5.8…+15.6%, and to exactly +0.000% at two parts**. Measured on
     hand-built planes at the review; **not shipped and not a fix either** — it leaves the
     staggered/offset cells unmoved (+51.7% → +51.7%), it **doubles** `f3c`/`f3d`'s deficit (−0.753% →
     −1.505% on their POD shape), and it costs one or two planes ON TOP of the fifth. So it does not
     beat the trade; but it does mean the stopping argument is **"the four-plane layout cannot reach
     this"**, not **"arithmetic cannot"**. The permanent statement is the weaker one already in the
     header: parent count per bucket is unbounded, so no FIXED plane count recovers parent identity.
  2. **The bit-identity has a narrower cause than "the planes look like one parent's".** In that cell
     every bucket carries EITHER new area OR co-located area and **never both** — both cards occupy the
     same z span, so both heads claim in the first bucket and every later part is co-located — so the
     `C_k : D_k` split is **degenerate** (`aRes = A_k` where `C_k = 0`, `aCov = A_k` where `D_k = 0`)
     and a plane that only refines that split is a strict no-op *by construction*. The bit-identity
     therefore does **not** generalise to a coincident pair whose heads land in different buckets, and
     it is not evidence about plane count as such.

  **THE FIFTH PLANE, MEASURED END TO END — and it is NOT the over-read's fix.** Built and rendered
  (candidate 8): `f3c` −0.113% → **+0.000%** and `f3d` −1.676% → **−0.000%**, i.e. both deferred
  precision XFAILs go EXACT and retire, harness PASS=88 FAIL=2 XFAIL=3. It moves the correctness
  target by 2.7 points (+77.411% → +74.702%). So the user's ruling — fifth plane deferred, it is
  polish not correctness — is **confirmed by measurement**, and this is the finding the task was told
  to report rather than act on. Cost: `K·W·B·(C+3)·4` → `(C+4)`, i.e. **+14.3% at C=4, +25% at C=1**
  (112 → 128 MiB at K=16/C=4/4096×64; **896 → 1024 MiB at K=128**), one accumulator write per
  co-located deposit and one more full-plane zero per band, against a standing High memory risk.

  **ONE RECORDED CLAIM IS WRONG, CORRECTED HERE — AND THE SECOND "CORRECTION" WAS ITSELF WRONG AND IS
  RETRACTED BY THIS TASK'S REVIEW.**
  1. **CONFIRMED. `g4`'s remainder is NOT `f3c`/`f3d`'s term.** `DeepCDefocusScatter.h` has said since
     M1.P3.T20 that the accumulation-time pooling "is the whole of g4's remaining 5.4%". The fifth
     plane closes `f3c`/`f3d` **exactly** and moves `g4` only **0.0325 → 0.0287** — 12% of it. So ≈88%
     of `g4`'s remainder is a different mechanism, and **which one is unexplained**; it is stated as
     unexplained rather than re-attributed. Sixth instance of a correct number carrying a wrong
     mechanism. **REPRODUCED INDEPENDENTLY** at the review on its own fifth-plane build (`f3c`
     0.7500004, `f3d` 0.7499995, `g4` 0.874128 = 0.0287, `f3e` +74.702%, `f3f` worst +82.963%), and the
     "unexplained" 88% is now **bounded on two sides**: it is NOT the tile-stack cap
     (`kCompositeHeadTiles` 16 → 64 leaves `g4` bit-identical at 0.874128) and it IS inside the
     residual-occlusion path (`tHeadIn = 1` drives `g4` to 1.000000, i.e. +11.1%), so it is
     `compositePixelCoveragePartition()`'s term and not the flatten's or the scatter's.
  2. **RETRACTED — THE BRIEF WAS RIGHT AND THIS TASK MEASURED THE WRONG RIG.** M1.P3.T23 reported that
     the verify clause's "α=0.10 and 0.30 currently read +6.53% and +5.17% and stay above +3.4% at the
     default K=16" did not reproduce, by a factor of ~18, and proposed that the pair was transposed.
     It measured those figures through the **`f3e`/`f3f` two-card unequal-density oracle**. They are
     not `f3e`/`f3f` figures: they come verbatim from this file's own **Med risk row on scene (g)'s
     α<1 ramp** — the **`g4` rig** — where M1.P3.T21's review swept α ∈ {0.99, 0.90, 0.50, 0.30, 0.10}
     × K ∈ {2…64}. **Re-measured at this task's review on that rig, they reproduce to three digits:**
     α=0.30 **+5.166%** at K=2/4 and **+3.399%** at K=16; α=0.10 **+6.526%** at K=2/4 and **+5.926%**
     at K=16; α=0.50 +3.757% / +0.897%; α=0.90 +0.819% / **−3.253%** (the last of which is `g4`'s own
     0.0325 pin, which is what validates the probe). So on the `g4` rig the over-read scales
     **INVERSELY** with α, **low α IS part of the target exactly as the brief said**, and it is still
     ungated at α ≤ 0.30. What M1.P3.T23 actually measured is a true and useful fact about a
     *different* rig — through the `f3e`/`f3f` oracle, equal-density staggered twins read +0.370% at
     α=0.10 and +1.619% at α=0.30 (both reproduced), and that family's over-read does scale *with* α.
     Its own table also contradicts its "nothing above +3.4% at α ≤ 0.30": α=0.30 at the 10:1 ratio
     reads **+6.49%**, thirteen times the gate. **This is the SEVENTH instance of the standing lesson
     in this milestone — a measurement that never reached the phenomenon it claimed to bound — and the
     first one committed by a correction rather than by a claim.** Nothing was acted on it, so no code
     or pin moved; the retraction is documentation only. The low-α arm of M1.P3.T23's verify clause is
     therefore **NOT met and NOT measured on the right rig by that task**; it is measured here, and it
     remains an open, ungated upward error owed a bounded check at Phase 1.4/1.5.
  **THE K SWEEP** (equal-density staggered twins, high arm, K = 4/8/16/32/64/128) **CONVERGES**, plateau
  by K=32 — it does not grow without bound as the unequal-density `f3f` K row suggested:
  α 0.10 +0.301/+0.266/+0.370/+0.415/+0.408/+0.409 (0 cells over the gate at every K);
  α 0.30 +1.066/+1.284/+1.619/+1.708/+1.716/+1.708;  α 0.50 +2.030/+2.437/+2.931/+3.070/+3.083/+3.070;
  α 0.90 +5.781/+5.638/+6.658/+7.005/+6.990/+6.963;  α 0.99 +8.422/+8.446/+8.373/+8.506/+8.657/+9.423.
  At the 10:1 ratio (K=4/16/64): α 0.10 +1.357/+1.831/+1.936; α 0.30 +4.856/+6.461/+6.853;
  α 0.50 +9.573/+12.808/+13.606; α 0.90 +31.481/+43.182/+46.177; α 0.99 +53.827/+77.411/+83.648.

  **COST OF THE REJECTED CANDIDATES, for whoever reads this before trying again**: the opacity
  allocation costs one float per tile (+64 B/thread at depth 16, 128 → 192 B, independent of K, format
  and thread count) and **+38–41% of the composite** — 507 → 717 ns/pixel at K=16, 2848 → 3948 at
  K=64, 5975 → 8222 at K=128 (worst-case synthetic, C=4, every bucket carrying both kinds of area,
  20 000 pixels, best of 7, same body as the shipped function). Nothing was spent: no pin moved, no
  XFAIL bound moved, `a3` is unmoved at 1.192e-07 of its 2.4e-07 gate, harness is PASS=86 FAIL=2
  XFAIL=5 SKIP=1 exactly as before, and both unit suites are green at 27/141 034 and 54/175 468.

- 2026-08-16 — **The user directed continued composite iteration, and scoped what "done" means.** The
  PM put the state to the user after M1.P3.T21: that iteration had reached a **Pareto point** rather
  than converging (newest-first vs oldest-first tile ordering trade by comparable margins — +64.6% /
  −4.107% against +22.4% / −27.4%), that there are **two independent losses** and not the one the
  record named, and that the reviewer's recommendation was to stop, gate the residual and document it.
  Options offered: (1) gate + document only, (2) that plus a fifth accumulation plane, (3) keep
  iterating. **The user chose (3), keep iterating** — and separately chose **"fix correctness, defer
  polish"** for how new findings are handled.
  Read together those two answers scope the work precisely, and that reading is what M1.P3.T23
  implements: the **+71.2% unequal-density over-read is the target**, because invented alpha on ordinary
  content is a correctness defect in the direction the honest-alpha contract forbids; while **`f3c`/`f3d`
  and `g4` are precision-only and are deferred**, pinned and documented rather than fixed. That also
  settles the fifth plane against adding it — it closes exactly the two deferred residuals, does nothing
  for the over-read, and costs +14% bucket memory at C=4 against a standing High memory risk.
  **M1.P3.T22 must land first**: the over-read is currently ungated, so iterating on it would be
  iterating blind. A candidate rule must **beat** the newest/oldest trade rather than sit somewhere on
  it, and reporting that none does is a legitimate outcome.

- 2026-08-16 — **M1.P3.T21's review established the root cause, and it is not what the record said.**
  There are **two** losses in the bucket planes, not one:
  - **(a) The `C_k : D_k` split** — how a bucket's pooled alpha divides between its new-area and
    co-located deposits. This is `f3c`/`f3d`'s term and `g4`'s remainder. **It is recoverable**: a fifth
    plane carrying the co-located alpha closes it **exactly** (−4.107% → ±0.0001% at every N and both
    split fractions), verified by building it. Cost `(C+3) → (C+4)`: +14% bucket memory at C=4, +25% at
    C=1, plus one accumulator write per deposit.
  - **(b) Parent identity** — which open chain a bucket's co-located area belongs to. **This is not
    recoverable by any bounded plane count**, because parents-per-bucket is unbounded and the planes
    hold only sums. It is an information problem, not a rule problem. This is the term reading **+71.2%**
    rendered, and adding the fifth plane leaves it **bit-unchanged**.
  So **M1.P3.T20 and M1.P3.T21 were patching different things, and neither was patching the term the
  record named** (both had attributed the residual to `f3c`/`f3d`'s accumulation-time pooling). Proof
  that iteration on ordering alone is zero-sum: newest-first reads +64.6% on the unequal-alpha case and
  −4.107% on the dense ramp; oldest-first reads +22.4% and −27.4%.
  **The user was shown this and directed continued iteration anyway** (see the entry above); M1.P3.T23
  carries it, with the requirement that a candidate beat the trade rather than move along it.

- 2026-08-16 — **M1.P3.T21: the upward error M1.P3.T20 traded for the α<1 ramp is FIXED, and the ramp
  got BETTER rather than worse. The composite's one head tile became a STACK of sixteen; nothing else
  moved.** The regression's cause was stated exactly right at T20's review: one `(tHead, headArea)`
  pair, so a bucket that both claims new area and continues a residual chain must discard one tile,
  which is free only while the discarded chain has no deposits left. **The fix is the state the review
  said it would take, and it is per-thread, not per-bucket.**
  **WHAT SHIPS.** `compositePixelCoveragePartition()` carries `kCompositeHeadTiles = 16` tiles —
  `(area, transmittance)` pairs, oldest first — instead of one. A bucket's co-located residual is
  **allocated across them by area from the NEWEST end**, each tile is attenuated by the share of the
  residual that landed on it, and only what does not fit lands on the tile this bucket itself just
  claimed. Both of T20's merge branches survive as special cases of that allocation and read
  ~~bit-identically~~ **identically to within float reassociation — 107 of 8 000 scalars differ over a
  4 000-pixel single-chain corpus, worst 2 ULP / 1.6e-07 relative (measured at T21's review against a
  worktree build of `ac46700`)**: the dense depth ramp (the residual is the newest tile's own rear — T20's
  `else`) and the M1.P3.T13/T15 same-pixel collision (it overflows onto this bucket's claim — T20's
  `> claimedArea` branch). A partly-covered tile **splits** into a covered core and an uncovered ring
  rather than averaging — averaging is algebraically M1.P3.T9's rejected `claimedArea` divisor and
  moves T9's pinned behind-focus residue 61.00% → 70.53%, measured here, matching what T9 and T17
  recorded. No plane, no signature, no `KernelSampler`/`DEEPC_HD`/`PodBuffer` seam, no
  `partitionAlpha()`, no scatter change.
  **THE COST, stated as the brief asked.** Per **pixel**: nothing — the stack is a stack frame, alive
  only inside one call. Per **bucket**: nothing — no plane and no per-bucket state is added, so
  `memory_limit`'s `K·W·B·(C+3)·4` is **unchanged at every K**, 117 MB at K=16 and 940 MB at K=128 as
  before. Per **thread**: `2 × 16` floats = **128 bytes**, independent of K, of the band size, of the
  format and of the thread count (at K=128 it is still 128 bytes, i.e. 0.00001% of that band's
  planes). In time, on a worst-case synthetic where *every* bucket carries both new and co-located
  area (4 channels, 20 000 pixels, best of 7): **319 → 581 ns/pixel at K=16**, 1379 → 2900 at K=64,
  3009 → 6901 at K=128 — the composite roughly doubles, on an O(K)-per-pixel pass against the
  scatter's O(Σπr²)-per-fragment one; the harness's own render totals do not separate it from
  run-to-run variance (110 s vs 66 s across two runs of the *same* code path shape). Depth 8 and depth
  16 are within 1% of each other at the shipped default K=16 (577 vs 581 ns), so **the depth was
  chosen on accuracy, not on the clock**.
  **HOW DEEP, MEASURED.** Worst UPWARD reading / mean |error| over 20 000 randomised pixels of N
  equal-alpha parents (random weights summing to 1, random part counts, random overlapping start
  buckets), judged against the disjoint-tiling oracle `Σ w_j·α_j`:
  | parents | 4 | 8 | 16 | 32 |
  |---|---|---|---|---|
  | M1.P3.T20 (one tile) | +21.86% / 2.32 | +21.22% / 2.76 | +22.75% / 3.10 | +18.95% / 3.34 |
  | depth 2 | **+0.000%** / 2.28 | **+0.000%** / 5.34 | −0.357% / 9.18 | −0.769% / 11.83 |
  | depth 4 | +0.000% / 1.29 | +0.000% / 2.27 | −0.106% / 2.90 | −0.160% / 3.00 |
  | depth 8 | +0.000% / 1.29 | +0.000% / 2.25 | −0.106% / 2.85 | −0.149% / 2.96 |
  | depth 16 | identical to depth 8, and so is depth 32, out to 128 overlapping parents |
  **TWO tiles already remove the upward error entirely**, which is the property that matters: past the
  cap the stack folds its two oldest tiles together *and* a partly-covered frontier tile can no longer
  split (the split needs a free slot and must not make one by merging, which renumbers the stack), and
  both of those OVER-occlude — the direction the contract permits. What more depth buys is the size of
  the remaining **deficit**; on the random corpus the knee is at 4, but on the worst case the unit
  suite pins — 32 equal parents of 32 parts each, i.e. 32 chains open at once — depth 8 reads −20.84%
  and depth 16 −5.13%. **16 ships** because it costs nothing at the default K (581 vs 577 ns/pixel,
  below the benchmark's own noise) and only 14% at K=128. A tile is retired only by capacity, since
  nothing in the planes records that a chain has ENDED.
  **THE SWEEP THE BRIEF ASKED FOR.** parts ∈ {2,3,4} × offsets 1..5 × 7 weights × 5 alphas = 525
  cells: **153 cells over +0.5% and worst +12.340% → ZERO cells and worst +0.000%**, with no cell
  below −0.5% either and no cell saturating alpha (8 did). Widened to parts ≤ 8 and offsets ≤ 7
  (2268 cells) it goes from 1024 cells / **+21.105%** to zero — i.e. this sweep reaches *worse* cells
  than the +18.3% the review reported, and closes them. All eight of the review's pinned cells and the
  volumetric+point cell reproduce their T20 readings exactly before the fix. **Every cell is now
  EXACT, not merely under the gate**, and the re-pin is against a hand-derived oracle (the coverages
  sum to 1 and both parents fit, so they tile the pixel as disjoint sub-areas at the same α, and the
  answer is α with no ordering assumption) rather than against the new output.
  **WHAT IS EXACT NOW, GENERALLY.** Over 40 000 randomised multi-parent pixels the corpus splits
  cleanly: **every pixel whose parents' per-unit opacities agree reads EXACT** (equal alpha *and*
  equal part count: 0 of 100 000 cells off by more than 3e-06, at any overlap; equal alpha with mixed
  part counts: 0 cells above +0.5%, worst high **+0.0000%**), and **every** error that remains has two
  parents' deposits in ONE bucket at different per-unit opacities. The classification is the evidence:
  of 40 000 mixed-alpha pixels, the 15 936 with no shared bucket are exact and all 13 632 cells over
  +0.5% are among the 24 064 that share one. That is `f3c`/`f3d`'s term — information lost at
  ACCUMULATION, not at composition — and it is **not fixed, not claimed fixed, and not fixable by any
  per-bucket composite rule**. Its worst reading on that corpus is **+83.8%** (α 0.058 and α 0.979
  pooled in one bucket, a 17× per-unit opacity ratio); under T20 the same corpus read +91.9% worst and
  44.9% of cells over +0.5% against 34.1% now.
  **THE MECHANISM ATTRIBUTION IS WRONG — CORRECTED AT T21's REVIEW.** The classification above is
  sound and reproduces (an independent 320 000-pixel corpus gives worst high **+91.4%** volumetric /
  **+99.2%** deep-mixed under T21, against **+105.5%** / **+113.0%** under T20 — so T21 improves it
  substantially and regresses nothing, but "worst +0.000% — exact" describes only the EQUAL-α family
  the sweep varies). What is NOT sound is naming `f3c`/`f3d`'s term as the cause. That term is the
  `C_k : D_k` area split guessing how bucket k's pooled alpha divides between the new-area deposit
  and the co-located one. **It was tested directly**: the composite was rebuilt taking a FIFTH plane
  carrying the co-located alpha, so the split is read rather than guessed. That closes the `f3c`/`f3d`
  dense-ramp term **exactly** (−4.107% → ±0.0001% at every N and both split fractions) and leaves the
  +64.6% two-parent case **bit-unchanged**. So they are two different mechanisms, and the big upward
  error is not the one named.
  **WHAT ACTUALLY CAUSES IT: TILE MIS-ASSIGNMENT ACROSS OPEN CHAINS.** Traced by hand on the minimal
  case (two volumetric parents, w 0.5/0.5, parts 5 and 1, α 0.99 and 0.10, offset 2). At the bucket
  carrying parent 1's fourth part, the stack holds parent 1's tile (T = 0.0631) and parent 2's fresh
  claim (T = 0.90); newest-first hands parent 1's residual parent 2's tile, so it is attenuated by
  0.90 instead of 0.0631 and contributes 0.271 where truth is 0.019. **The planes carry no parent
  identity**, so which open chain a bucket's co-located area belongs to is undecidable from them:
  swapping to oldest-first moves this case to +22.4% but takes the dense ramp from −4.11% to −27.4%.
  Neither order is right; the information is absent. Unlike the `C_k : D_k` split, **no fixed number
  of planes recovers it** — parent count per bucket is unbounded — so this one really is a permanent
  residual, for a reason different from the one recorded.
  **T20's GAINS ARE KEPT, AND TWO OF THEM IMPROVED.** Harness `g4` **0.0543 → 0.0325** (re-pinned in
  the same change, band unchanged at 0.004); `g2` K=16 7.153e-07 → **1.192e-07**; `g3` K=16 2.980e-07
  → **1.192e-07**; `f3b` −0.001% → −0.000%; `f3c`/`f3d` move in the 7th decimal. `g1` reads
  3.757e-07 / 1.277e-08 / 1.809e-08 (K=8/16/64). **CORRECTED AT T21's REVIEW: `g1` is NOT unmoved.**
  T20's commit `ac46700` was rebuilt in a worktree and scene (g) re-rendered from it: it reads
  1.703e-08 / 5.801e-08 / 2.182e-08, i.e. exactly the figures T20 recorded. T21 moved K=8 by 22×
  (1.703e-08 → 3.757e-07) and improved K=16/64. The same re-render shows `g2`/`g3` at K=8 also moved
  the wrong way — `g2` 1.848e-06 → 3.862e-05, `g3` 1.907e-06 → 3.862e-05, ~20× each — against the
  K=16 improvements reported above. All six readings stay 100× or more under their 1.0e-03 / 3.9e-03
  gates, so nothing here is a defect; what was wrong was the claim that T20's number did not
  reproduce, and the selective reporting of the K=16 column only.
  **Every other check in the suite is bit-identical**, including scene (a)'s size-0 parity — `a3`
  **1.192e-07 against its 2.4e-07 gate, unmoved**, so the margin T20 halved is not spent further, and
  a1/a2/a4 likewise — T9's pinned behind-focus residue (36.86/50.25/56.31/61.00%), scene (i)'s
  coverage-deficit identity, `h3c`, `l1`, `l3`, `l5`, `b1`. Harness **PASS=83 FAIL=0 XFAIL=5 SKIP=1,
  exit 0** — the same counts as before the change, `g4` still the banded XFAIL, re-pinned in this
  commit. Unit suites 54 cases / **175 429** assertions and 27 / 141 034, green.
  **THE K SWEEP: NO DIVERGENCE REINTRODUCED, AND ONE COST, REPORTED RATHER THAN SMOOTHED.** Scene
  (g)'s ramp, interior flat-field mean, rendered at K = 2…128 (the knob's own range is 4–128):
  | α | K=2 | K=4 | K=8 | K=16 | K=32 | K=64 | K=128 |
  |---|-----|-----|-----|------|------|------|-------|
  | 1.00 | −0.000 | −0.000 | −0.000 | −0.000 | −0.000 | −0.000 | −0.000% |
  | 0.99 | −0.943 | −0.943 | −1.855 | −2.536 | −2.970 | −3.714 | −5.497% |
  | 0.90 | +0.819 | +0.819 | −1.698 | −3.253 | −4.084 | −4.935 | −6.576% |
  | 0.50 | +3.757 | +3.757 | +1.987 | +0.897 | +0.319 | −0.287 | −1.409% |
  against T20's −4.363…−5.036 / −3.290…−6.055 / +1.373…−1.007. Still bounded and roughly K-flat —
  nothing like T17's +0.33 → −26.62% — and better at every K ≤ 32; slightly worse at K=64/128 (α=0.90
  −4.665 → −4.935 and −6.055 → −6.576, the only two columns where this scene fills the tile stack at
  all — the whole harness renders bit-identically at depth 8 and depth 16). **THE COST IS THE UPWARD END**: the fix removes spurious
  occlusion, so it lifts this whole curve by roughly two points, which improves it wherever the
  reading was negative and *worsens* it wherever it was already positive. T20's worst positive
  excursion of **+1.37%** (α=0.50/K=2) becomes **+3.76%**, and α=0.50 now reads **+0.90% at the
  shipping default K=16** where T20 read −0.13%. That is the honest-alpha contract's forbidden
  direction on a rendered scene, it is not the mosaic term (a hand-built dense ramp at α=0.50 is
  exact at coverage 0.5, 1.0 and — through the saturation pass — reads *lower* than T20 at coverage
  1.5, so the `excess` regime is not the source), and it is left as a recorded cost rather than
  clamped away. It is the same accumulation-time pooling that `g4`'s residue is made of, seen from
  its positive side.
  **THE DENSE RAMP'S REMAINING DEFICIT NOW HAS A CLOSED FORM**, which is the strongest evidence that
  what is left is the pooling term and not a mosaic one. A dense ramp at split fraction `frac` reads
  `1 − (1−m)²` with `m = (a0+a1)/2`, against a truth of `1 − (1−a0)(1−a1) = α`; AM-GM makes that a
  deficit for every fraction but 0.5, and it is **independent of N** — **−4.107% at α=0.90 for frac
  0.25 AND 0.75, at N=4, 16 and 64**, agreeing with the hand-derived closed form to 5 decimals. Under
  T20 the same cells read −2.42/−3.69/−4.00% and −5.79/−4.53/−4.21%: N-dependent and asymmetric in the
  fraction, because the single tile mixed the pooling term with the mosaic error. The unit suite's
  cells are re-pinned **against that closed form, computed in the test**, not against a re-run.
  **THE TWO PRE-EXISTING UPWARD ERRORS DID NOT MOVE**, and they were checked rather than assumed: the
  `excess`-regime rear double-count reads **0.097590 against a true 0.050095 (+94.810%)** and the
  free/claimed straddle **0.812500 against 0.750000 (+8.333%)**, both bit-identical to their pre-T21
  values, both still pinned, and neither folded into this change. The excess-tile fix for the first
  was **not** re-proposed.
  **MUTATION-TESTED, seven ways.** Reverting to T20's single tile (via the cap) fails 4 unit
  assertions and takes `g4` to 0.0543; allocating oldest-tile-first (FIFO) fails **204** assertions
  and 9 harness checks (`g4` 0.0718); spreading the residual over every tile — the disproved "merge by
  area" — fails 17; refusing the overflow onto this bucket's own claim fails 9; not splitting the
  partly-covered frontier tile fails 1; folding the overflow by area instead of by minimum fails 1;
  never pushing the claim tile fails the M1.P3.T13 sharp-path identity. **Two of those are NOT caught
  by any rendered check** (`g4` reads 0.0325 under both), and that is recorded next to the pin rather
  than left unstated.
  **THE OVERFLOW FOLD TAKES THE MINIMUM, NOT THE AREA MEAN, and that is an argument, not a
  measurement** — `min ≤ mean`, so a lower tile transmittance can only reduce the alpha a later
  residual adds, i.e. whatever the cap costs it costs in the direction the honest-alpha contract
  permits. Over 80 000 randomised overflow pixels the two forms were **indistinguishable** (identical
  worst readings in every row); the unit suite's 32-parent overflow row is the one place that
  separates them. Said plainly so nobody re-derives a mechanism from a number that did not move.
  **A BUG FOUND IN REVIEW OF THIS OWN CHANGE, and what it changed.** The first draft made room for the
  frontier split by calling the merge — which renumbers the stack under the already-resolved frontier
  index. It never crashed and the harness never reached it (the rendered scenes never fill the stack:
  depth 8 and depth 16 render bit-identically), but the 32-parent overflow row read +0.44% under it,
  i.e. the cap could still err upward. With the split simply skipped when the stack is full, **every
  corpus measured reads ≤ 0 at overflow** (worst upward −0.106% at 16 parents, −0.149% at 32, −0.304%
  at 128) and the pinned overflow row moved +0.44% → −5.13%. That is a bigger deficit and a strictly
  safer direction, and it is why the depth table above is not the one an earlier draft measured.
  **STILL UNEXPLAINED, said rather than papered over.** (i) Why the harness's total render time came
  out *lower* after the change (110 s → 66 s across the two runs) when the composite got slower — most
  likely machine variance or filesystem caching, but it was not isolated, and the microbenchmark above
  is the figure to trust. (ii) Why the two eviction rules (fold the oldest two by MINIMUM vs by area)
  are indistinguishable on 80 000 randomised overflow pixels while the unit suite's 32-parent row
  separates them cleanly: the minimum is kept on the derivation (`min ≤ mean`, so it can only
  over-occlude), not on a measured advantage. (iii) ~~The `g1` figures M1.P3.T20 recorded (1.703e-08
  at K=8) do not reproduce.~~ **RESOLVED AND WRONG (T21's review).** T20's commit `ac46700` was
  rebuilt in a `git worktree` and scene (g) re-rendered from it: `g1` reads 1.703e-08 at K=8, exactly
  as T20 recorded. 3.757e-07 is T21's own reading — T21 moved that row, and the "does not reproduce"
  claim was a fabricated mechanism attached to a real movement. See the corrected paragraph above.

- 2026-08-16 — **M1.P3.T20's REVIEW: the ruling stands — the fix is real and root-caused — but it
  bought the α<1 ramp with a NEW upward error on staggered multi-part parents, and five of its claims
  were overstated.** Verified independently, with an area-model oracle written from the design
  reference rather than from the composite, and with every mutation re-rendered.
  **CONFIRMED.** The isolated rig goes to exact at every N, α and split fraction (reproduced:
  −2.05/−7.02/−8.36, −4.11/−17.44/−21.63, −3.03/−22.39/−33.65% before → ≤1e-06 after). Lead (b) is
  genuinely disproved: the closed form `wα(1 − w₀w₁wα)` is exact to the bit (0.4375 at w=1/α=0.5/
  frac 0.5), and on the natural arrangement it is *worse* than T20 reported — fog-over-opaque reads
  **0.859375**, not 0.875, and two 50% fogs **0.68359375**, not 0.71875 (T20's figures correspond to
  an arrangement in which only one of the two layers self-occludes). Lead (b) on the ramp reads
  −25.6…−27.8%. Harness reproduced at **PASS=83 FAIL=0 XFAIL=5 SKIP=1**, both unit suites green
  (54/175 329 and 27/141 034), and all seven g4 mutation readings reproduce **exactly**
  (0.1607/0.1315/0.1196/0.0913/0.0703/0.0598/0.0491), so the 0.004 band is sound.
  **THE ONE REGRESSION — NOT FIXED, NOW PINNED.** Only ONE `(tHead, headArea)` pair is carried, so
  the merge rule must DISCARD one tile when a bucket both claims new area and continues a chain. That
  is free only while the discarded chain has no deposits left. **Two multi-part parents at
  OVERLAPPING depth ranges** — two fog slabs, or a fog slab and a point fragment, whose kernel
  weights tile one destination pixel, so truth is α with no ordering assumption — both have deposits
  left: **377/525 swept cells now read >+0.5% HIGH, worst +18.3%**, and at α=0.90 several saturate the
  output alpha to exactly 1. The pre-T20 composite read those same cells 4–16% **LOW**. The sign is
  the honest-alpha contract's forbidden one. T20's required "volumetric parents" check tested only the
  **non-overlapping** mosaic (`k = j*(P+1)+i`), which is exact and stays exact. Neither alternative is
  a trade worth making (`always carry the chain` −9.3% on the dense ramp; `merge by area` −5.9%,
  g4 0.0913, 40 unit assertions), so the fix is more state — a Phase 1.4/M2 question. Pinned as a
  band in `tests/test_defocus_scatter.cpp` ("staggered multi-part parents …").
  **"+1.37% IS THE WORST POSITIVE EXCURSION" IS TRUE OF THE K SWEEP, NOT OF THE COMPOSITE.** Two
  larger upward errors are **pre-existing and bit-identical before and after T20**, so they are not
  this task's regression, but nothing bounds them: (i) in the `excess` regime a fragment's head
  registers no tile, so its own co-located rear is attenuated by an unrelated tile — behind a
  full-coverage α→0 foreground, a defocused opaque fragment of coverage 0.05 reads **0.0976 against a
  true 0.0501 (+94.8%)**; (ii) a fragment straddling the free/claimed boundary has its excess
  attenuated by a mean that includes the tile it just claimed and does not overlap — **+8.3%**
  (0.8125 against 0.75). Both are recorded in the source. An eighth mutation tried at this review —
  registering the excess as its own tile, which fixes (i) exactly on hand-built planes — is
  **refuted by pixels**: g1 1.082e-02, g2 4.954e-02, g3 3.200e-02, g4 0.0825. The fit-only rule is
  right; the residual is real.
  **FOUR CLAIMS CORRECTED IN PLACE.** (1) **Three XFAILs were retired, not four** — `l3` still
  carried `expectedFailure=True, hardTol=8.0e-03`, so a full pre-T20 revert reported it XFAIL rather
  than FAIL; it is now a plain check. (2) **The remaining g4 term is not "fragments at different
  split fractions"** — T20's own unit cells hold the fraction CONSTANT and still read −2.42…−5.79%;
  the trigger is that a dense ramp's bucket k pools fragment k's head at `partitionAlpha(α, 1−frac)`
  and fragment k−1's rear at `partitionAlpha(α, frac)`, which differ for every frac but 0.5. (3)
  **`claimA = cov` is caught by four rendered checks, not one** — g1 9.980e-03, g2 4.906e-02, g3
  3.191e-02 as well as g4 0.0703 (it does still survive the unit suite). (4) **`−38.9% at split
  fraction 0.25` is the frac 0.75 cell** under this repo's own convention; frac 0.25 reads −5.49%,
  which the entry's own table already says.
  **AND TWO CONSTANTS DID MOVE BESIDES `g4`.** Re-rendering the current checks against the pre-T20
  plugin moves exactly 15 of 89 rows: `g1`×3, `g2`×2, `g3`×3, `g4`, `l3`, `f3b`/`f3c`/`f3d` (7th
  decimal, as reported) — **plus `a3` 5.960e-08 → 1.192e-07** and **`l1` 2.176e-03 → 2.084e-03**,
  neither disclosed. `a3` still passes scene (a)'s 2.4e-07 parity gate but its margin has halved;
  `l1` improved. Everything else — T9's behind-focus 36.86/50.25/56.31/61.00%, `h3c`, `l5`, scene
  (i)'s coverage-deficit identity, scene (b) — is bit-identical, so "no other pinned constant moved"
  is very nearly, but not exactly, true.

- 2026-08-16 — **M1.P3.T20: the α<1 receding-content residual is FIXED, in the COMPOSITE, not in the
  split. Lead (b) was built and disproved; the mechanism T17's review isolated was fixed directly.**
  **The ruling is a fix, not an accepted residual.** Harness `g4` 0.1607 → **0.0543**; on the isolated
  rig the deficit goes to **exact**; the K-divergence is **gone**. Full harness
  **PASS=83 FAIL=0 XFAIL=5 SKIP=1, exit 0** (from 76/0/12/1), both unit suites green, **no pinned
  constant moved** except `g4`'s own, which is re-pinned here. **[CORRECTED at review: `a3`
  5.960e-08 → 1.192e-07 and `l1` 2.176e-03 → 2.084e-03 moved too; both still pass.]**
  **LEAD (b) — the linear split — IS DISPROVED, and the reason is worth carrying.** On the isolated
  mosaic rig `α_i = α·w_i` with each half claiming its own new area *is* exact, as T17's review said.
  But it is exact only in the `fit` regime. In the `excess` regime — where a fragment's coverage lands
  on area already claimed, which is every dense pixel and every sharp-path pixel — splitting one
  surface into two independently-distributed sub-areas makes it occlude itself: the pair delivers
  `wα(1 − w₀w₁wα)` instead of `wα`, a relative deficit of `w₀w₁·w·α` that is **negligible for a large
  disc and maximal at w = 1**. Built and measured: an α=0.5 fog layer over an opaque card at one pixel
  reads **0.875 against a true 1.0** at split fraction 0.5, and the two-50%-fog-layers identity reads
  0.71875 against 0.75. Validation scene (a)'s size-0 parity gate is 2e-07; the linear split misses it
  by five decades on any pixel with two samples. **The transmittance split is therefore KEPT
  unchanged**, and the Design reference's flagged justification is superseded not by "the linear form
  is fine now" but by "the composite, not the split, was wrong". `partitionAlpha()` is untouched.
  **THE FIX.** `compositePixelCoveragePartition()` gains ONE scalar (plus the area it describes) and
  loses nothing: no new plane, no new memory, no signature change, no `KernelSampler`/`DEEPC_HD`/
  `PodBuffer` seam touched. Both halves of the mechanism T17's review named are addressed:
  - a co-located deposit is attenuated by **`tHead`**, the transmittance of the tile *its own head*
    claimed, instead of by the pooled `tClaimed`. Its head is the only thing in front of it at that
    pixel by construction (a fractional split's rear is one bucket behind its head; a split parent's
    parts are consecutive), so the pooled mean was letting every *other* fragment's head and rear
    occlude it;
  - `tClaimed *= (1 − resLocal)` becomes **subtractive and area-weighted**:
    `tClaimed -= aRes·tHead/claimedArea`, i.e. only `resArea` of the claimed share loses
    transmittance, and it loses `resLocal` of its own. The two terms then telescope exactly — the
    alpha added equals the transmittance removed.
  - **A bucket registers a new tile only for its `fit` share.** The `excess` share lands on area the
    mosaic already has, so it *attenuates* the existing tile rather than adding one. Registering it
    too reads **+5.169%** on two full-coverage layers sharing a bucket pair (against the pre-T20
    composite's +0.141%) — it double-counts one physical area as two tiles at two stages of the same
    composite.
  - **When a bucket leaves two candidate tiles, the pixel's own area decides.** If
    `claimA + chainA > claimedArea` they cannot be disjoint, so the residual sat on the very tile this
    bucket claimed and the chain already carries its whole occlusion — carry the chain. Otherwise they
    fit side by side and the next co-located deposit is the rear of the head just claimed — carry the
    claim. **Merging them by area instead is wrong in both directions at once** (measured: −5.9% on a
    dense ramp AND +5.2% on the collision shape), because the question is not "what is the mean" but
    "which tile does the next deposit land on". The `> claimedArea` branch is what keeps the
    M1.P3.T13/T15 same-pixel collision shape and the two-layer flat field bit-identical to pre-T20.
  **THE EVIDENCE, on the same rig T17's review used** (hand-built planes, one pixel, no kernel, no
  holdout, no flatten, no depth quantisation; truth is α because the weights sum to 1):
  | α | N=2 | N=16 | N=64 | frac 0.25, N=16 |
  |---|-----|------|------|-----------------|
  | 0.99 | −2.05% → **exact** | −7.02% → **exact** | −8.36% → **exact** | −1.59% → **exact** |
  | 0.90 | −4.11% → **exact** | −17.44% → **exact** | −21.63% → **exact** | −5.49% → **exact** |
  | 0.50 | −3.03% → **exact** | −22.39% → **exact** | −33.65% → **exact** | −7.34% → **exact** |
  ("exact" = ≤1.19e-06 absolute, float accumulation over up to 64 fragments.) Over 2000 randomised
  multi-fragment pixels judged against an independently-written area-model oracle, mean |error| goes
  **6.91% → 0.38%** (point fragments, no pooling) and **5.13% → 0.61%** (volumetric parents); with
  pooling allowed, 9.13% → 3.33%, the rest being the pooling term below.
  **THE FOUR THINGS THE BRIEF REQUIRED BEFORE ADOPTING ANYTHING, each measured.**
  - **Volumetric parents.** Unchanged where they were already exact (single parent, any part count,
    any alpha, in front of focus) and **fixed** where they were not: a *mosaic* of volumetric parents
    read −24.5% at 8 parents × 3 parts / α=0.90 and is now exact. `splitSpanAtBoundaries()` is
    untouched — the volumetric transmittance split is physically exact and was never the issue.
  - **Holdouts.** `vis` multiplies into both the alpha and the area planes, so `local = A_k/C_k` is
    invariant to it and the fix commutes with holdout visibility by construction. **[CORRECTED at
    review: only `local` and `resLocal` are invariant. `fit = min(cov, freeArea)`, `claimedArea` and
    the `claimA + chainA > claimedArea` test all move with `vis`, which is the intended semantic (a
    held-out fragment claims less area) but is not "by construction". The no-regression conclusion
    is carried by the rendered scenes below, not by the invariance argument.]** Rendered: scenes (b)
    and (f) are unmoved to the digit — `f1` 4.367e-08, `f2`'s erasure profile, `f3c` −0.113%, `f3d`
    −1.676%, `b1` 0.000e+00 against `DeepHoldout2` — and `f3c`/`f3d` move only in the 7th decimal.
  - **Within-bucket ordering, and the K knob.** Nothing is assigned whole-weight and nothing changes
    at scatter time, so the K knob's mitigation is intact; the **K sweep is the proof it did not trade
    one divergence for another** (below). Where within-bucket ordering *is* genuinely lost — two
    unrelated layers pooled in one bucket — the readings are bit-identical to pre-T20 by the
    disjointness branch above.
  - **Depth-interpolation continuity.** The split is unchanged, so the interpolant is unchanged;
    `partitionAlpha()`'s documented α→1 quantisation-at-bucket-centres cost is neither improved nor
    worsened. Scene (l)'s small-CoC readings are unmoved (`l1` 2.084e-03, `l2` 1.950e-03,
    `l5` 3.667e-03) and **`l3` went from XFAIL 5.226e-03 to 0.000e+00** — the K-dependence at the
    sharp↔disc threshold that T19 attributed to the bucket composite was exactly this defect.
  **THE K SWEEP** (scene (g)'s ramp, interior flat-field mean, rendered):
  | α | K=2 | K=4 | K=8 | K=16 | K=32 | K=64 | K=128 |
  |---|-----|-----|-----|------|------|------|-------|
  | 1.00 | −0.000 | −0.000 | −0.000 | −0.000 | −0.000 | −0.000 | −0.000% |
  | 0.99 | −4.363 | −4.363 | −5.552 | −4.886 | −3.923 | −3.539 | −5.036% |
  | 0.90 | −3.290 | −3.290 | −5.583 | −5.426 | −4.784 | −4.665 | −6.055% |
  | 0.50 | +1.373 | +1.373 | −0.090 | −0.128 | +0.104 | −0.022 | −1.007% |
  against T17's α=0.90 column of −11.39 / −16.07 / −22.99 / −27.49% at K=8/16/64/128 and α=0.50's
  +0.33 → −26.62% over the whole sweep. **Bounded and roughly K-flat, where it used to diverge.** The
  positive excursions are small and are reported rather than smoothed: the worst is **+1.37%** at
  α=0.50/K=2 (was +0.33%), so the fix does move the honest-alpha contract's forbidden direction by
  about one point in one corner of the sweep, which is a cost, not a wash.
  **WHY IT IS NOT THE DISPROVED "COVERAGE INTO BOTH BUCKETS".** That defect (M1.P3.T2's review,
  +8.29%) deposits `w` into the area planes **twice**, so a pixel with an honest 60% coverage deficit
  reports 120% and an isolated bokeh renders at double energy. Nothing here changes what is deposited:
  the total area written per fragment is still exactly `w`, once, and `scatterSpanBothBuckets()` is
  untouched. The change is entirely in how the composite *reads* the two area planes it already had.
  The scene-(i) coverage-deficit identity is bit-unchanged (dip min 0.513260, width 24 px, fabricated
  blue 0.000e+00) and the unit suite's "honest 60% hole stays 0.6" still holds exactly.
  **LEAD (a) WAS NOT RE-TRIED ALONE, and is now moot.** Area-weighting the residual's occlusion
  *multiplicatively* — algebraically the pre-T9 `claimedArea` divisor — reads 0.0777 on `g4` and fails
  the unit suite, exactly as T9 and T17 recorded. The subtractive form shipped here is **not** that
  expression: it is `tClaimed − aRes·tHead/claimedArea`, which reduces to the multiplicative one only
  when `tHead == tClaimed`, and the unit suite pins the difference on an identity that needs no
  arithmetic (an opaque surface covering the whole pixel reads alpha exactly 1 whatever is in front of
  it; the multiplicative form punches a 12.5% hole through it). T9's pinned behind-focus residue
  (36.86 / 50.25 / 56.31 / **61.00%**) is **unmoved**, and so is every other pinned constant in both
  suites — 54 cases / 175,329 assertions and 27 / 141,034, green.
  **WHAT IS LEFT AT `g4`, AND WHY IT IS A DIFFERENT MECHANISM.** −5.43%, and it is **not** the
  `tClaimed` mosaic term. The control is in the unit suite: a ramp whose fragments all carry the SAME
  split fraction is now exact at every bucket count, every N and every α; give the same fragments
  DIFFERENT split fractions and −2.4 to −5.8% comes straight back, because a bucket's pooled alpha
  then carries two per-unit opacities and the `C_k : D_k` area split cannot separate them.
  **[CORRECTED at review: the control is wrong as stated. The exact case is fragments in their OWN
  bucket pairs, at ANY split fraction; the −2.4…−5.8% cells hold the fraction CONSTANT across every
  fragment and pack the pairs adjacently, so the trigger is one bucket pooling a head at
  `partitionAlpha(α,1−frac)` with a rear at `partitionAlpha(α,frac)` — unequal for every frac but
  0.5 — not fraction mixing between fragments.]** That is
  **`f3c`/`f3d`'s mechanism** — information lost at *accumulation*, not at composition — and no
  per-bucket composite rule can undo it. A real ramp gives every scanline its own split fraction,
  which is why this scene cannot reach zero. Reducing it would mean changing what the scatter
  accumulates (a per-bucket opacity moment, or narrower buckets), which is a Phase 1.4/M2 question and
  is **not** claimed here.
  **HARNESS EFFECTS.** PASS 76 → **83**, XFAIL 12 → **5**, FAIL 0 → 0, exit 0.
  `g1` K=8/16 1.048e-02 / 4.446e-03 → **1.703e-08 / 5.801e-08**; `g2` K=8/16 4.909e-02 / 4.657e-02 →
  **1.848e-06 / 7.153e-07**; `g3` K=8/16 3.191e-02 / 2.883e-02 → **1.907e-06 / 2.980e-07**;
  `l3` 5.226e-03 → **0.000e+00**. **[CORRECTED at review: only THREE were retired — `l3` kept
  `expectedFailure=True, hardTol=8.0e-03`, so a full pre-T20 revert reported it XFAIL rather than
  FAIL. It is a plain check as of the review.]** Those XFAILs are **retired to plain checks**, deliberately: an
  `expectedFailure` that no longer describes a residual is an unbounded licence to fail. **Scene (g)'s
  own stated criterion — "no visible seams at bucket boundaries at K=16" — is now met outright, at
  every K, by four decades.** `l5` (3.667e-03 against 3.9e-03) and `h3c` (1.335e-06 against 2.0e-06)
  are **bit-unchanged**, so neither margin was spent.
  **WHY AN OPAQUE PLANE SHOWED AN α<1 DEFECT AT ALL** — the one thing here that was surprising, and it
  is derived, not guessed: at α=1 the transmittance split is a no-op, but **saturation** is not. Where
  a destination pixel's new area and co-located area sum past 1 the bucket's alpha clamps to 1, so
  `local = aCov/cov` comes out at `1/(C_k + D_k) < 1` and the remainder becomes a residual — which the
  pooled `tClaimed` then over-occluded exactly as it did at α<1. That is why `g1`/`g2`/`g3` moved by
  five to six decades on a fully opaque input, and why they barely move at K=64 (few fragments share a
  bucket there), which is the same convergence `g4` now shows.
  **`g4` IS RE-PINNED IN THIS SAME CHANGE**, 0.1607 ± 0.025 → **0.0543 ± 0.004**, still a band and
  still unable to PASS by construction. **Mutation-tested by re-rendering the scene through seven
  separate mutations**: full pre-T20 revert 0.1607, always-carry-the-chain 0.1315, residual alpha ×0.75
  0.1196, merge-tiles-by-area 0.0913, `claimA = cov` 0.0703, multiplicative update 0.0598, no-excess-
  attenuation-of-`tHead` 0.0491. The two nearest sit 0.0052 and 0.0055 outside the pin, hence the
  0.004 band. **The independent oracle for the new value** is the isolated rig above, which is
  arithmetic on hand-built planes with no Nuke in it: it predicts −5.47% where the render reads
  −5.43%, and its fixed-split-fraction control is exact — so the number is validated *and* attributed,
  not re-fitted to the new output.
  **ONE THING NO TEST PINS, said rather than hidden**: `claimA = fit` rather than `cov`. It is right by
  the same argument as `claimT` (the excess share is not a new tile), but the mutation **survives the
  entire unit suite** and is caught only by `g4`'s band (0.0703). Recorded in the source next to the
  line. **[CORRECTED at review: it is also caught by `g1` 9.980e-03, `g2` 4.906e-02 and `g3`
  3.191e-02 — four rendered checks, not one. It does still survive the unit suite.]**

- 2026-08-16 — **M1.P3.T17: the bucket composite is `CoveragePartition`. `FrontToBackOver` is deleted.**
  Decided from rendered pixels, every render setting `ScatterParams::combine` explicitly, on the
  post-T19 plugin (`6cbf28b`). It is **not** a clean sweep — `over` wins several readings and they are
  reported below rather than dropped — but the two candidates fail in different *classes*, and only one
  of them breaks identities on the simplest content the node has.
  **The four scenes T17 was told to judge on.**
  - **(c) does not discriminate**, confirming T16: every check PASSes under both. c1 `|α−1|` is 0.000e+00
    under both; c3 band-alpha ratio 8.984e-06 under both; c4 (α=0.9 full-range fog flat field) is the one
    row that moves and it favours `over` — 2.742e-07 against partition's 1.397e-05, both four decades
    inside the gate.
  - **(f) is where `over` dies.** `f1` — eight opaque point strips at size 0 behind an α=0.9 fog holdout,
    against the analytic `(1−α)^t`, i.e. the simplest content the node renders — reads **4.367e-08 under
    partition and 2.500e-01 under `over`, a hard FAIL**. The mechanism is now *derived and confirmed at
    every strip, not guessed*: an opaque fragment's transmittance split is a no-op (`a₀ = a₁ = 1`), so
    both bucket deposits carry the full `1·vis`, and `over` composites them as two independent layers,
    giving `2·vis − vis²` instead of `vis`. Measured against predicted, per strip: 0.6683→**0.8900**
    (predicted 0.8899), 0.5012→**0.7512** (0.7512), 0.3758→**0.6104** (0.6104), 0.1585→**0.2919**
    (0.2919). The error is `vis(1−vis)`, maximal **0.25 at vis = 0.5**, i.e. a **+50% relative** error on
    the node's differentiating feature, K-independent and unreachable by any knob. `f2`'s erasure
    profile is the same law applied again (partition 0.2512 at the onset probe, `over` 0.4393 = 2v−v²).
    `f3c`/`f3d` at K=16 favour `over` (+0.074% / +0.967% against partition's −0.113% / −1.675%) and are
    reported as such; at K=4 the ordering reverses (`over` +2.194% / +4.799%).
  - **(g) is decisive, and reproduces post-T19 in both framings.** Interior flat-field alpha on the
    opaque receding plane, K=8/16/64, **with** the ±2.5 px focus-row exclusion: partition
    0.989516 / 0.995554 / **1.000000** (`|α−1|` 1.048e-02 / 4.446e-03 / **2.799e-07**) against `over`
    0.997177 / 0.978471 / **0.922108** (2.823e-03 / 2.153e-02 / **7.789e-02**). **Without** the exclusion
    (T19 having removed its original justification): partition 0.990084 / 0.994916 / 0.998101
    (9.916e-03 / 5.084e-03 / 1.899e-03) against `over` 0.997437 / 0.980542 / 0.929502 (2.563e-03 /
    1.946e-02 / 7.050e-02). Same verdict either way — partition converges in K, `over` diverges — and
    the exclusion only caps partition's K=64 reading at the six focus rows' content-driven term (worst
    row y=127, radius 0.50 px, 0.925927). **Population is the sharper reading than magnitude**: g2 puts
    `over` over 1/255 on **112/112 and 112/112** interior rows at K=8/16 against partition's 51/112 and
    23/112, i.e. `over`'s banding is the whole field and partition's is localised. g3 (worst row-to-row
    seam) favours `over` at K=8/16 (3.012e-03 / 5.183e-03 against 3.191e-02 / 2.883e-02) and partition
    at K=64 (3.576e-07 against 1.582e-02); read with the medians it is one spike against a flat field
    (partition's median step at K=16 is 1.788e-07) versus a permanently stepping one (`over`'s 7.492e-04).
  - **(i) does not discriminate on any of its spec checks** — i1–i5 are identical to the digit under
    both candidates (dip min 0.513260, width 24 px, i3b 1.472e-05, fabricated blue 0.000e+00,
    DeepMerge twin 1.000000). Expected: the silhouette has one sample per pixel and total coverage < 1
    through the dip, so every bucket is pure `fit` and the two rules coincide there. **CORRECTED at
    T17's review: "every reading" was too strong** — i7 and i7d, the `pre_merge`-reachability probes,
    read 9.0e-02 under the shipped composite against 1.6e-01 under the deleted one, on the
    bin-straddling pair that is *not* pure-`fit` content. Both PASS either way and neither is a
    bake-off criterion, but the scene is not literally candidate-blind. Scene (i) is a spec check,
    not a bake-off scene.
  **The contrary evidence, explained rather than dropped.** Scene (l) favours `over` across the board —
  `l1` 4.296e-04 against 2.176e-03, `l2` 4.175e-04 against 1.950e-03, `l3` 2.186e-04 against 5.226e-03,
  `l5` 2.935e-03 against 3.667e-03 (`l6` is 9.937e-03 under both, confirming it is the field's extremum
  and not the composite). **The first hypothesis — that an opaque field's `max` is pinned at 1.000000 by
  the saturation clamp, so `|α−1|` one-sidedly rewards `over`'s bias — was PROPOSED AND REFUTED**: the
  same three ramps re-rendered at α=0.5 and α=0.25 still favour `over` (worst-pixel 4.3e-03 / 2.2e-03
  against partition's 5.7e-02 / 1.5e-02), so the advantage is real accuracy at small CoC, not an artefact
  of the metric. The explanation that does survive is that **both of `over`'s failure modes are quenched
  together below ~2.5 px**: its across-bucket deficit needs a destination pixel's coverage spread over
  many buckets (a 0.5–2.5 px disc gathers from a handful of source pixels), and its split-fragment
  inflation needs kernel weights well below 1 (they are near 1 at that radius). Partition's own residual
  does *not* vanish with radius. Consistent with that, `over`'s advantage shrinks monotonically as the
  CoC grows: 13× at scene (l)'s 0–2.5 px ramp at α=0.5, 1.8× at scene (g)'s 0–64 px ramp at the same
  alpha. In absolute terms partition's whole scene-(l) disadvantage is about one 8-bit code value —
  worst is `l3` at 5.226e-03, i.e. **1.33** code values (the review's arithmetic; "never more than
  ~1" was a shade generous). **This is a recorded COST of the decision, not a wash:** `l5` sits at
  3.667e-03 against a 3.9e-03 gate (6% of margin) under the shipped composite where the deleted one
  read 2.935e-03 (25% of margin), so small-CoC content is the one regime where T17 shipped the worse
  of the two and left less room for the next regression.
  **A NEW residual on the WINNER, found by this bake-off and larger than anything scene (l) shows.**
  Scene (g)'s ramp at **α < 1** — an ordinary semi-transparent receding surface, one sample per pixel —
  breaks partition's K-convergence outright. At K=16: partition −12.527% at α=0.99, −16.065% at α=0.90,
  −9.194% at α=0.50, against `over`'s −5.703% / −6.551% / −1.308%. In K at α=0.5 partition **diverges**
  (+0.331 / +0.331 / −5.000 / −9.194 / −13.713 / −19.645 / −26.622% at K=2/4/8/16/32/64/128) while `over`
  goes +4.983 → −9.322% over the same sweep. Three controls, run before believing it: the source
  flattens through stock `DeepToImage` to exactly 0.5000000 min/max/mean; the **same alpha at the same
  16 px CoC radius with the depth ramp removed is exact under BOTH candidates at K=8 and K=64
  (0.5000017)**, so neither the kernel nor the sharp path is involved; and the deficit vanishes at α=1,
  which is exactly where the transmittance split `α_i = 1−(1−α)^{w_i}` degenerates to a no-op. So the
  mechanism is **the transmittance split's recombination, not the kernel and not depth quantisation** —
  that much is established. T17 recorded the *pooling* term itself as consistent with the
  different-split-fraction residual (f3c/f3d) but **not isolated**, and said so rather than asserting
  it.
  **T17's REVIEW ISOLATED IT (2026-08-16), outside Nuke and with no kernel, no holdout, no flatten and
  no depth quantisation in the rig.** Hand-built bucket planes, one destination pixel, N equal-weight
  α-fragments each split 50/50 across a bucket pair, fed straight to
  `compositePixelCoveragePartition()`; truth is α because the weights sum to 1:
  | α | N=2 | N=16 | N=64 | all N in ONE bucket pair |
  |---|-----|------|------|--------------------------|
  | 1.00 | 0.000% | 0.000% | 0.000% | exact |
  | 0.99 | −2.05% | −7.02% | −8.36% | exact |
  | 0.90 | −4.11% | **−17.44%** | −21.63% | exact |
  | 0.50 | −3.03% | −22.39% | −33.65% | exact |
  That reproduces the sign, the α-dependence, the divergence in bucket count (≙ K), the exact
  vanishing at α=1 and the exact constant-depth control, at the scene's own magnitude (−17.4% against
  the rendered −16.1% at α=0.90/K=16). **The term is `tClaimed`.** The composite carries ONE scalar
  transmittance for the whole claimed area, but a depth ramp makes every destination pixel's claimed
  area a mosaic of disjoint sub-areas at different depths, each with its own transmittance. Two
  consequences, both deficits: a fragment's co-located rear deposit is attenuated by the pooled
  `tClaimed` instead of by its own head's `1−a₀`, and `tClaimed *= (1 − resLocal)` applies a
  residual's occlusion to the *whole* claimed area rather than to the `resArea/claimedArea` share it
  actually covers. At frac 0.25 rather than 0.50 the same rig reads **−38.9%** at N=16, so −16.1% is
  not the worst case the mechanism admits.
  **Two leads for the Phase 1.4/1.5 ruling, each with its evidence and neither a ruling.**
  (a) Area-weighting that second consequence is *algebraically identical* to reverting the residual
  divisor to the pre-M1.P3.T9 `claimedArea`: built and rendered, it takes `g4` from 0.1607 to 0.0777
  and improves g1/g2/g3 as well, but it moves T9's pinned behind-focus residue from 61.00% to 70.53%
  and FAILs `tests/test_defocus_scatter.cpp`. T9 already adjudicated that trade; it is not a free fix.
  (b) The transmittance split exists *only* so the two deposits reconstruct the parent under the
  `over` that T17 deleted. On the same isolated rig, a **linear** split (`αᵢ = α·wᵢ`, each half
  claiming its own new area instead of one arriving co-located) is exact — 0.900000 / 0.500000 at
  every N and at both split fractions, where the transmittance split reads −17.4% / −22.4%. That is
  arithmetic on one synthetic pixel and says nothing yet about volumetric parents, holdouts,
  within-bucket ordering or the depth-interpolation continuity the transmittance form was *also*
  chosen for. It is a lead, not a mechanism claim.
  Pinned as bounded XFAIL `g4`. **Promoted from a Phase 1.4/1.5 obligation to its own task,
  M1.P3.T20, sequenced before M1.P3.T18** — it is the largest known error in the shipped node, a fix
  moves every rendered pixel, and T18 decides from rendered pixels.
  **Why partition still wins, given that.** `over` is **better** on this one class of content, and
  the review's own K sweep says so more strongly than the entry above did: at α=0.90 `over` reads
  −1.045 / −6.551 / −15.308 / −18.305% at K=8/16/64/128 against partition's −11.390 / −16.065 /
  −22.994 / −27.494%, and it wins at every K and every α<1 tested, by 1.5–3×. It is still only *less*
  bad (it diverges in K too), and it additionally fails identities partition satisfies exactly: the holdout law above (+50%
  relative), the flat-opaque-across-buckets deficit (25.0/31.6/34.4/35.6% at 2/4/8/16 buckets, pinned in
  `tests/test_defocus_scatter.cpp`), the isolated-bokeh inflation (up to **+93.8%**, i.e. a bokeh
  highlight at double energy — the most visible artefact a defocus node can produce), and the
  behind-focus structural residue (48.77/75.03/90.96/117.10% against partition's
  36.86/50.25/56.31/61.00%). It also errs in **both** directions, which the node's honest-alpha contract
  forbids in the upward one — `over` reads **+2.362%** on scene (g) at α=0.5/K=8 and +4.983% at K=2,
  and inflates an isolated bokeh by +93.8%. (Corrected at T17's review: "every partition error
  measured here is a deficit" holds for the *means* but not per row — partition's own row means run
  as high as 0.937923 against α=0.90 at K=8. The asymmetry is one of degree, not of kind.) And `over` gets
  monotonically worse as K rises, so the design's own stated mitigation for within-bucket ordering loss
  is an anti-mitigation for it. A residual on the winner is a follow-up; an identity violation on the
  loser is not fixable — `FrontToBackOver` reads neither area plane and has nothing to correct with.
  **Deleted**: `BucketCombine`, `ScatterParams::combine`, the `bucket_combine` knob and
  `clampedBucketCombine()`, `resolveBandCPU`'s switch, `compositePixelFrontToBack()` /
  `compositeBucketsFrontToBack()`, the harness `--combine` flag and its scene-(g) candidate loop, and
  the unit tests that existed only to pin `over`. **Deliberately KEPT**: all four accumulation planes —
  T17's brief anticipated deleting "the accumulation plane the winner does not read", and there is
  **none**: `compositePixelCoveragePartition()` reads colour, alpha, new area AND co-located area, and
  the two area planes are precisely what the losing candidate ignored. Also kept: every test pinning
  behaviour partition still has, including the behind-focus residue and the interpolant divergence.

- 2026-08-16 — **M1.P3.T19 replaced the kernel's uniform radius grid with an adaptive one; the harness
  is green end to end for the first time** (PASS=77 FAIL=0 XFAIL=18 SKIP=1, exit 0). The deficit at a
  bin edge is `h·|S′_r(0)|/2 ≈ h/(πr²)`, so a step `h(r) = c·r²` makes it **uniform at `c/π` across the
  whole radius range** — a bound, rather than a bound at one radius. `c = 1/512` puts the step at 0.5 px
  at r=16 and makes every constant exact in binary. Below 16 px the grid is hyperbolic
  (`radius = 512/(1025−index)`), above it the old uniform 0.5 px grid is kept. Closed-form inverse, so
  lookup stays O(1). Costs, all measured, none assumed: **+4.8 ns per fragment** (8.1 ns vs 3.3 ns for
  the lookup — under 1% of a fragment at r≥4 px, invisible end to end), **+201,544 B flat** (the same at
  `max_radius` 500 as at 40, because the refinement only exists below 16 px), vectorization unchanged at
  441 loops, weights still summing to 1 within 8.94e-08.
  **Interpolating between adjacent LUT entries was prototyped and lost on both axes** — accuracy
  (3.354e-03 / 3.041e-03 / 1.087e-02 on the y/diagonal/radial ramps against the shipped 2.084e-03 /
  1.819e-03 / 9.937e-03) and cost (+18.5% / +34.9% / +68.2% per fragment) — and would have had to blend
  into per-thread scratch and hand back a `KernelView` of it, breaking the documented "safe to hold and
  share across render threads" lifetime contract. Do not re-try it.
  **Two residuals were exposed, not introduced**, and each now carries a bounded XFAIL:
  - **`l3`** — the bucket composite showing through at the sharp↔disc threshold. It is
    **candidate-dependent** (K=64 reads 0.992598 under `CoveragePartition` against 0.999666 under
    `FrontToBackOver`), which the kernel cannot cause and the composite can. **No pixel got worse**: the
    worst row was 0.799119 at *every* K before and is ≥0.9926 at every K after. The old grid hid it by
    rasterising every radius in [0.25, 0.75] as the same delta.
  - **`l6`** — the CoC *field's* own interior extremum, not quantisation at all. It survives an **exact
    per-pixel kernel with no grid whatsoever** (0.989234 against the shipped grid's 0.990063), matches
    the analytic cone prediction `1 − 2a/3`, and is invariant to both K and the composite candidate. It
    is inherent to normalised-kernel scatter and will recur wherever the radius field has an interior
    extremum; a v2 note, not a v1 defect.
  **Note `l5` passes with only 6.5% margin and only because of `l6`'s apex exclusion** (it reads
  9.937e-03 and FAILs without it), and **`h3c` now sits at 67% of its gate**, up from 25%. T17 and T18
  should expect to touch both.

- 2026-08-16 — **Re-pinned constants must be validated against an independent oracle, not against the
  new output.** M1.P3.T19 moved four pinned test constants. Its review built a **grid-free
  `KernelSampler`** and drove the real scatter through it to get a truth column: checkerboard
  flat-field 0.99927 → **0.995718** (truth 0.996160), mislabel over-count 40.09/70.70% →
  **45.30/76.70%** (truth identical), behind-focus partition 28.22/45.21/51.23/59.05% →
  **36.86/50.25/56.31/61.00%** (truth 36.91/50.25/56.40/61.02), behind-focus `over` barely moving. Every
  new pin lands within 0.1 of truth where the old ones sat **up to 8.6 points below it** — so the fix
  moved them toward correctness rather than re-fitting them.
  **But two of the mechanism stories attached to those numbers were false**, and only the oracle caught
  it: both claimed the same-kernel collision rule had been suppressing area claims, when the fragments'
  bins were **always distinct under both grids** (behind-focus parts at 0.800/2.353/3.905/5.458 px →
  old bins 2/5/8/11; mislabel at 4.956/3.503/2.040/0.552 → old bins 10/7/4/1), and the mislabel test
  builds its SoA by hand and never reaches the flatten path at all. The real mechanism is **disc
  mis-sizing at small radii** — the old grid snapped 0.800 → 1.000 (+25% radius, +56% area) and
  0.552 → 0.500, which *is* the single-pixel delta. **A correct number with a wrong explanation is a
  trap**: the next task to reason from it will reason from the explanation.
  Two related process findings, both now fixed: the new POD test only checked
  `kernelGridIndex(kernelGridRadius(i)) == i` — **on node radii, where `floor`/`ceil`/`round` all
  agree** — so a `lround → floor` mutation survived the entire suite; and the checkerboard pin was
  one-sided (`> 0.9956`) and would have **passed a full revert of the fix**. Pins must be **banded**,
  and mutation-tested **off** the nodes, not just on them.

- 2026-08-16 — **M1.P3.T16 completed the scene sweep; the harness is now the milestone's gate, and it
  is red on one real defect.** Full run: **PASS=74 FAIL=3 XFAIL=16 SKIP=1**, exit 1. The three FAILs
  are all validation scene (l) and are one genuine node defect, now **M1.P3.T19**. Consequences:
  - **The 2026-07-27 entry claiming T13 closed scene (l) is retracted** (struck through in place). The
    bucket-collision wander T13 fixed was real; the scene's own 0–2 px criterion nonetheless fails at
    **2.009e-01**, 70× T13's recorded 2.906e-03, on a mechanism T13's synthetic corpus never reached.
    A synthetic corpus that does not cross the artefact's trigger is not evidence that the scene passes.
  - **`pre_merge`'s record is corrected twice over.** It *is* reachable at the **shipping default**
    0.25 px — two same-pixel layers at CoC radius 1.2/1.4 px (0.20 apart, inside the default tolerance,
    straddling the 1.25 px bin edge) move **9.0e-02 on 100% of pixels**. And the knob is therefore
    **lossy at its default**, not "lossless when radii are equal": losslessness holds only when the
    grouped radii land in the *same* kernel bin. M1.P3.T12's "0 pixel difference everywhere" and
    M1.P3.T16's first explanation ("at 0.25 px anything grouped rasterises the same disc") were both
    wrong for the same reason — the probes happened to group nothing. **This deserves a
    `merge_tolerance` default review at Phase 1.4**, since the default is currently trading correctness
    for speed silently.
  - **Scene (c) no longer discriminates the bucket composite.** `FrontToBackOver` now *passes* scene (c)
    at K=8, so the design reference's "plain `over` is known to fail scene (c)" no longer reproduces on
    constant-depth content. Scene **(g)'s ramp** is where `over` breaks, and it breaks decisively:
    interior flat-field alpha on an opaque receding plane reads 0.997176 / 0.978461 / **0.922105** at
    K=8/16/64 under `over` against 0.989520 / 0.995558 / **1.000000** under `CoveragePartition`. The two
    candidates move in **opposite directions in K** — partition converges to exact (2.8e-07 at K=64),
    `over` diverges monotonically to −7.8%, which is precisely the 2026-07-26 prediction that "it
    worsens as K rises, so the K knob is not a mitigation". M1.P3.T17 inherits this table.
  - **Unbounded XFAILs are a false-pass class.** An `expectedFailure` with no outer bound silently
    swallows a regression that lands on top of a documented residual. Every XFAIL in the harness now
    carries a hard bound that flips it to FAIL if the reading drifts past it. Apply this to any future
    XFAIL as a matter of course.
  The review also fixed four checks that had no discriminating power (`i4` measured the deep interior
  rather than the dip band and survived its own mutation; `j0` read the root format rather than the
  source, so it passed the very bug it guards; scene (h) was **vacuous** — two opaque cards at exactly
  the same depth are collapsed by the tidy pre-pass into a bit-identical single-card render, so the
  saturation rule was never exercised; scene (g) never gated its own stated seam criterion). **Every
  check in the harness must now be mutation-tested** — a check nobody has made fail proves nothing.

- 2026-08-16 — **M1.P3.T12 landed the harness; scenes (a)–(f) are green with three documented
  XFAILs.** `tests/nuke/` (one command, numeric gates, non-zero exit on failure, `combine` /
  `holdoutInterp` / `K` / `pre_merge` all parameters) reads **PASS=28 FAIL=0 XFAIL=3 SKIP=1** at
  K=16 / CoveragePartition / LogChord / `pre_merge` on. Four things worth carrying:
  - **Nuke 17's Python has neither numpy nor OpenImageIO**, and `nuke.sample()` proved unreliable, so
    every render round-trips through a 32-bit-float uncompressed EXR read by `tests/nuke/exrio.py`
    (validated against an analytic `Expression` pattern to 1.11e-08, including a negative-origin data
    window). Any future in-Nuke measurement should reuse it rather than re-derive this.
  - **`DeepHoldout2` outputs a flat 2D image — it must NOT be followed by `DeepToImage`.** The design
    reference's "`DeepHoldout` → `DeepToImage`" phrasing (scene (b)) is wrong for it; with a
    `DeepToImage` attached the reference renders a uniform (0,0,0,1) frame, which reads as a 0.95 /
    100%-of-pixels failure. Corrected, scene (b) is bit-exact — and non-vacuously so: `DeepHoldout2`
    moves the plain flatten by 9.023e-01 on 39.06% of pixels.
  - **`DeepCConstant` emits NaN when `front == back`** (`weight = depth / (back − front)` = 0/0). Any
    scene built with it needs a nonzero span thickness.
  - **Parity gates are only meaningful on premultiplied source data.** With unpremultiplied layers
    scene (a)'s composite reaches 1.56 and the same 3-ULP residual reads 3.576e-07 — over the 2e-07
    absolute gate — for no fault of the node.
  The three XFAILs were each adjudicated as pre-existing, not new: **f2** is the M1.P3.T10 log-chord
  erasure (decay shape matches `kMinTransmittance` exactly, scales as `depthRange/K`); **f3c** is the
  different-split-fraction residual and is K-convergent (exact at K≥64); **f3d** is the same
  mechanism, non-monotone in K (exact at K=128), at ~0.9%/layer rather than the recorded ~4%.
  **A documented behaviour no longer reproduces**: "connecting even a non-occluding holdout moves
  defocused pixels by up to 1.78e-01 through merge regrouping" now measures **exactly 0.0** at both
  size 0 and size 6 — presumably closed by T13/T15. Note also that `computeDepthRange()` is
  source-only (`src/DeepCDefocus.cpp:954`), so that row's z=40 holdout sits beyond the LUT's last
  boundary: it validly tests merge regrouping but not the LUT.

- 2026-08-16 — **M1.P3.T12 split four ways** (T12 harness + scenes a–f, T16 scenes g–l, T17
  bucket-composite bake-off, T18 holdout-interpolant bake-off): as written it bundled a harness
  build, a twelve-scene sweep, two independent decisions and two source deletions, well past the
  format's one-coherent-change sizing rule, and its verify depended on work inside itself. The two
  bake-offs are genuinely independent of each other (different flags, different scenes, different
  files), so they parallelise conceptually even though both need the harness first. Ordering is
  T12 → T16 → T17 → T18. No scope was dropped in the split; the scene list, both bake-offs and the
  delete-the-loser discipline all survive verbatim in the new tasks.

- 2026-07-27 — **M1.P3.T15 closed the last three same-pixel holes, and needed THREE mechanisms where the
  plan prescribed one.** Both deviations were forced by measurement, and both were independently
  re-derived at review: (a) the plan's "areas revert to the pre-T13 rule, both claim, the coverage clamp
  handles it" **does not work** — with both claiming, the composite's `C_k : D_k` split still reads
  `a − a²/4` and alpha stays at **2.50e-01**; the repeat deposit must write **no area at all** (two new
  bits in the existing flag byte). (b) Per-bucket attenuation alone is exact on **alpha but not colour**
  — it redistributes premultiplied colour whenever a trailing fragment lands in a leading fragment's
  *front* bucket: alpha 2.4e-07 but **|dc| 7.97e-01 over 79–95% of pixels**. The closure is a third
  mechanism, a **monotone bucket frontier** (a deposit may not land in front of a bucket an earlier
  same-kernel fragment at that pixel already reached; clamped forward at whole weight if it would).
  Note this means T13's review's ≤4.2e-08 colour figure for attenuation holds only when both layers
  carry the same unpremultiplied colour.
  **The frontier rule is sound and holds K-convergence** — the property two earlier candidate fixes died
  on — verified rather than argued: it moves at most **one** bucket (maxMove = 1 over ~90k fragments),
  never moves depth or radius, its firing rate falls monotonically with K (34.5% → 3.0% at size 0), the
  two-layer defocused field is **bit-identical to pre-T15 at every K** and converges to 0 by K=64, and a
  4500-config worst-case sweep shows it adds no new structural error. It **must** be gated on kernel bin.
  Results — six corpora (point/span/mixed × holdout off/on), 900px × K{4…128} × spp{2…20} × `pre_merge`
  both: **point-on 2.25e-01 → 2.38e-07, span-on 3.07e-01 → 5.82e-07, mixed-off 2.49e-01 → 5.24e-07,
  mixed-on 2.67e-01 → 5.24e-07**, every rate to **0.00%**; point-off and span-off bit-identical to
  shipped, fragment counts included. In headless Nuke, scene (a) goes 2.38e-07 / 5 ULP / 0.00% with the
  holdout **connected or disconnected**. The span rows' >2e-07 is the span-split float chain against a
  double reference, not T15 — verified identical base-vs-T15 at every count.
- 2026-07-27 — **T13's collision merge is RETAINED, but not for the reason first given, and the numbers
  behind it were wrong.** It *is* superseded on accuracy: compiled out, every T15 criterion still passes
  and the field table is identical. It was kept as a fragment-count optimisation claimed at −30%/−20%,
  but review measured that this holds only with `pre_merge` **off**; **at the shipping default it
  collapses to −1.1%/−4.2%/−4.8% with no measurable wall time.** So the "every extra fragment rasterises
  a disc" argument is **not supported at default knobs** — M1.P4.T2 must measure it there and delete the
  merge if it doesn't pay. Also recorded: the merge's holdout-bracket gate is why connecting a
  *non-occluding* holdout still moves defocused pixels by up to 1.78e-01 on 442/4096 px; with the merge
  out and `pre_merge` off it is bitwise 0/4096.
- 2026-07-27 — **`DeepHoldout`'s input 1 is a 2D depth image, not a deep input — validation scene (b)
  must use `DeepHoldout2`.** M1.P3.T15 reported `setInput(1, …)` returning False as a headless-Nuke
  blocker and substituted "an opaque black card merged into the deep stream"; review found that
  substitution **vacuous** (the card evaluated to r=g=b=a=0, front=inf, from a `channel0`/`expr0..3`
  pairing mistake, and it executes zero lines of holdout code since `holdoutConnected` gates on
  `input1()`). The blocker claim is literally true but its conclusion is wrong: `DeepHoldout2` takes a
  deep second input, `setInput(1, …)` returns True, and it verifiably holds out. **M1.P3.T12 can run
  scene (b) as specified** — but pick the reference deliberately: `DeepHoldout2`'s flatten differs from
  volumetric `DeepToImage` by |dc| 3.8e-03 on 11.9% of pixels on volumetric spans.
- 2026-07-27 — **Two "unreachable by construction" claims in this phase turned out to be reachable**, and
  the pattern is worth naming. M1.P3.T15 argued the `depositArea1` guard could never be exercised
  because the monotone staging order puts a rear deposit's bucket beyond the frontier; three mutants
  rested on that and all survived. Review disproved it: **CoC radius is V-shaped about focus**, so three
  same-pixel samples can bin A,B,A — the middle leaves `frontierBin` on B, the third's clamp doesn't
  fire, and both its deposits land on buckets the first already touched (25 such fragments at K=4 over
  900px at size 6). Now covered by four subcases. The same shape left a **residual T15 does not close**:
  `frontierBin`/`runBin` are single slots, so a negligible (α=1e-6) different-kernel sample interleaved
  between two same-kernel ones reopens the collision — 0.750000 → 0.737391 (1.26e-02) where pre-T15 read
  0.973309 (2.23e-01), an 18× improvement rather than a closure. Unreachable at size 0 (one kernel bin),
  so no milestone gate touches it; pinned by test. The obvious fix (`>=`→`>` on the frontier update)
  **regresses** it to 2.96e-02 and was dropped.
- 2026-07-27 — The flatten scratch costs **20 B/bucket/thread (2.5 KB at K=128)**, not the 512 B the
  plan budgeted: T15's touch record is 12 B/bucket (`runStamp`/`runBin`/`runAlpha`) and T13's claim pair
  a further 8 B. They must stay **separate records stamped on different events** — folding them measured
  a regression. **M1.P4.T1 budgets on 20 B/bucket.** Fragment memory and the SoA layout are unchanged;
  the two new area flags are bits 2/3 of the existing byte.
- 2026-07-27 — **M1.P3.T13 fixed the same-pixel bucket collision** with two passes in
  `flattenPixelToSoA` and **no plane, flag or composite change**: a one-slot-delayed collision merge
  (`over`-composite when same `FragmentKind` + `sameScatterKernel()` + same holdout bracket +
  intersecting deposit ranges, unconditional rather than behind `pre_merge` since it is a correctness
  pass, and the group's depth/radius always its front-most member's), plus `claimNewArea()` so a second
  same-pixel deposit into a bucket arrives as co-located area. Scene (a) at size 0 over 60 rows
  (K ∈ {4…128} × 1–20 spp × `pre_merge` both, 2000 pixels each): worst |dα| **2.65e-01 → 2.38e-07**,
  worst |dcolour| 5.89e-01 → 3.44e-07, **100% → 0.00%** of pixels over 1e-3. In real headless Nuke
  against `DeepToImage`: worst |dα| **1.394e-01 → 2.980e-07**, 96.3% → 0.00%. ~~**Scene (l) is closed**
  (wander 0.780–0.880 / max step 9.97e-02 → 0.6947–0.7010 / 3.90e-03 against a truth of 0.700), and the
  0–2px ramp criterion is met (max step across a threshold crossing 2.906e-03 ≤ 3.899e-03 elsewhere).~~
  **RETRACTED 2026-08-16 at M1.P3.T16's review: scene (l) is NOT closed.** On a real headless-Nuke
  0–2.5 px ramp the max step is **2.009e-01**, 70× the figure above. T13's measurement was taken on a
  synthetic corpus that never crossed the 0.75 px kernel-bin edge, which is where the trough lives.
  What T13 *did* close on scene (l) is real and stands — the bucket-collision wander — but the scene's
  own criterion fails on a different mechanism (`DiscKernelLUT` radius quantisation, **M1.P3.T19**).
  **K-convergence survives** — the property the previously-disproved whole-weight fix died on — because
  the pass never touches `bucketOf()`'s assignment: the sharp row is flat at ~3e-08 at every K (six
  orders better than baseline) and the defocused row beats baseline at every K. Note the defocused row
  is **not** strictly monotone in an independent 200-config corpus (4.39e-02 at K=8 → 4.53e-02 at K=16);
  the 3% wobble is the milestone's own occlusion-before-blur residual, not T13, so verify clause (2)'s
  "monotone" should read "beats baseline at every K, monotone within corpus noise".
- 2026-07-27 — **The area claim is per (bucket, KERNEL), not per (bucket, pixel)** — T13's review found
  the as-submitted form punched a **25% hole in in-focus opaque geometry that had previously been
  exact**. Re-labelling the second same-kernel deposit co-located makes the composite split that
  bucket's alpha `C_k : D_k = 1 : 1` and read `a − a²/4`: exact only when the two alphas are equal,
  short by `((a1−a2)/2)²` in general, and short by a **flat 0.25 once the additive alpha saturates**
  (two opaque samples read 0.750 against a true 1.000). Over mixed corpora it moved mean |dα|
  2.27e-02 → 6.00e-02 and the rate 38.9% → 98.0%. Root cause: the area planes model "C_k claimed by one
  kernel, D_k co-located on it" — which describes two *different-sized* discs and describes nothing at
  all when the two deposits share a kernel and therefore cover the identical area. Fixed by yielding an
  existing claim only to a **different** kernel; the different-kernel win T13 was built for is retained
  (a 23.3px/8.5px pair still reads band alpha 1.000000 against 2.000000 before), pure rows are
  byte-identical, and mixed rows now beat both baseline and the as-submitted form on mean, RMS, worst
  and rate in every row.
- 2026-07-27 — **`pre_merge` merging across holdout brackets was erasing unoccluded foreground outright,
  at default knobs.** Found at T13's review: with `pre_merge` on and tolerance 0.25px, an opaque card at
  z=50 and samples at z=40 and z=62 all land in the default rig's single `[10, 100]` ΔCoC bucket — 90
  units wide against ~6.2-unit holdout brackets — and inside the radius tolerance, so the group's union
  midpoint lands *behind* the card and the pixel reads **alpha 0.000000 against an exact 0.500000**.
  Fixed by applying T13's own `holdoutBracketOf()` gate to the pre-merge predicate (same cache, free
  when no holdout is connected). Pre-existing, default-on, and pinned by a new test.
- 2026-07-27 — **The `pre_merge` focus-straddle risk is REFUTED, not merely unaddressed.**
  `buildBoundedDeltaCoc` places a boundary *exactly on* the focal plane (`boundary(focusBoundary()) ==
  focus` to 5 dp at focus 2/10/50 and K 4/16/128), so no ΔCoC bucket contains focus in its interior and
  a `pre_merge` group — keyed on one containing bucket — can never straddle it. T13's own collision
  merge *can* span focus, and anchoring the group's radius to its front-most member is exactly the right
  guard (measured 0.52632 for an equal-radius straddling pair, not 0).
- 2026-07-27 — **Size-0 parity currently holds only with input 1 DISCONNECTED, and validation scene (b)
  will fail until M1.P3.T15 lands.** Connecting a holdout that occludes nothing switches T13's fix off:
  the same 60-row corpus reads worst |dα| **2.35e-01 with up to 99.0% of pixels over 1e-3**, and two
  sharp samples at z=20/40 behind a card at z=95 read 0.816118 against a true 0.750000. Not a regression
  — the baseline is equally bad — but it is a gate the milestone does not yet have. **M1.P3.T12 must not
  read scene (b)'s failure as a T12 defect.**
- 2026-07-27 — Scene (a)'s ≤2e-07 gate needs the **same "grows with sample count" qualifier the
  coincident-sample clause already carries**: the residual is a float `over` chain against a double
  reference, ≤2.0e-07 at 1–5 spp but **2.38e-07 offline and 2.98e-07 in Nuke at 12–20 spp / K=128**.
  State the sample count with the tolerance, as the coincident-sample clause does.
- 2026-07-27 — `merge_tolerance` no longer governs the M1.P3.T13 collision pass, which ignores it
  entirely (bin equality on the 0.5px grid already implies an *identical* `KernelView`, which is the
  stronger and provable condition — an explicit `|Δr| ≤ 0.25px` limb was tried, survived mutation, and
  was deleted as strictly tighter than necessary). The knob's documented meaning now covers only the
  pre-merge. **M1.P5.T2's node help must say so.**
- 2026-07-27 — **`CMAKE_BUILD_TYPE` had been unset since the project began, so the CMake build never
  passed an `-O` flag at all** — every plugin in this repo, not just the new node. The scatter TU went
  from **0 vectorized loops to 402** once M1.P3.T14 defaulted it to Release, and `vmulps`/`vaddps`
  across the linked `.so` from 0 to 83 (that 83 is a whole-`.so` count; the 402 is per-TU). `ctest`
  4.8–6× faster. So the row-span auto-vectorization the entire performance design rests on had never
  existed in a binary the CMake build produced, and every earlier `-fopt-info-vec` result on record
  described a hand-compiled object. **`vfmadd` stays at 0** — `-mfma` remains absent until M1.P5.T1, so
  the `fp-contract` parity hazard is still closed. `-DNDEBUG` was audited for reach and has none: there
  is not a single `<cassert>` include or bare `assert()` in `src/` or `tests/`. No pinned figure moved.
  Landed before M1.P4.T2 deliberately, so that gate profiles the module that actually ships.
- 2026-07-27 — **Validation scene (a)'s "point samples are bit-exact (0 ULP)" clause is retired, and
  the gate is now ≤2e-07 absolute.** Not a relaxation of standards — the 0-ULP result was a true
  property of the `flattenPixel()` path, which M1.P3.T5 retired from the cook path, and the shipping
  bucketed-scatter architecture cannot reproduce it by construction: every point fragment goes through
  `bucketOf()`'s **mandatory** fractional two-bucket split (the design's own required fix for
  layer-transition banding), whose reconstruction is mathematically exact but not bit-exact unless the
  depth lands exactly on a bucket centre. M1.P1.T2 had already measured that reconstruction at 8.3e-08
  worst; end to end it reads max 1–2 ULP over ~3% of samples at 1 spp, and it is **flag-independent**
  (bit-identical across unoptimised, `-g` and `-O3` builds, same diffs at the same coordinates).
  Removing the split is not on the table — M1.P3.T13's review measured that whole-weight assignment on
  the sharp path defeats the K knob, the design's stated mitigation for within-bucket ordering loss.
  **This is a distinct mechanism from M1.P3.T13's**, which needs ≥2 fragments per destination pixel to
  collide and is four orders of magnitude larger (0.238 vs ~1e-07); T13's brief is NOT extended to
  cover it, and T13's own ≤2e-07 criterion already accommodates it.
- 2026-07-27 — `flattenPixel()` has **zero call sites** since M1.P3.T5 — genuinely dead, and the
  `#pragma GCC optimize("fp-contract=off")` guard on it therefore protects nothing reachable. It is
  inert rather than harmful today (no `-mfma` anywhere yet), but **M1.P5.T1 must move or duplicate that
  guard onto the scatter TU** when `-mavx2 -mfma` land, since that is where the arithmetic producing
  shipped pixels now lives.
- 2026-07-27 — **M1.P3.T5 wired the scatter into `engine()` and the node defocuses — but the task is
  NOT closed.** Two of its four gate clauses did not hold. (i) **Scene (a)'s size-0 parity fails**, and
  the defect is in the bucket accumulation rather than in T5's wiring, which merely put it on the cook
  path — recorded and specified as **M1.P3.T13**, which must land before M1.P3.T12. (ii) **Abort
  recovery was never actually exercised**: headless Nuke cannot trigger a recoverable mid-cook cancel
  (`nuke.cancel()` from a timer thread has no effect; `SIGINT` kills the process), so T5's implementer's
  "next cook 0/24576 samples different" was not reproducible. What *was* confirmed instead: bitwise
  determinism across four independent processes (identical sha256), a forced full re-cook bitwise
  identical, correct disconnect/reconnect, and — usefully — that an **upstream failure mid-cook neither
  published nor corrupted the cache** (a missing-file `DeepRead` raised, the cache came back unchanged,
  the forced recompute was bitwise identical). **Do not record abort recovery as verified**; it needs an
  interactive or Viewer-driven pass.
  What *did* verify: the node renders end to end (32×32 card at z=3, Manual size 12 ⇒ radius 28.0px,
  support exactly 88px both axes, plateau alpha 0.4156817 against the analytic 0.4157517 — 1.68e-4
  relative); sparse regions are **exactly** black (48,640 pixels bitwise zero on all four channels, no
  denormals, the disc rim a hard cut-in rather than a decaying tail); and the holdout works exactly as
  designed — alpha exactly 0.0 and exactly 1.0 with no other value present, the transition **one pixel**
  wide, a foreground in front of the holdout surviving with its own colour while the background at the
  same x erases to exact 0, and the matte AOV exact and computed as `−expm1(Σ log1p(−a))` rather than
  `1 − boundaryT`. Enabling the AOV leaves RGBA bitwise identical.
  Two figures T5's implementer reported are wrong as recorded: colour:alpha is **0.80001**, not "exactly
  0.8" (the sharp path does give exactly `float(0.8)`; the drift enters through the scatter/LUT
  normalisation), and the bbox pad of 41 was measured at `max_radius=40` rather than the default 100.
  The **formula** is right and verified across seven settings: `ceil(max_radius + 0.5·edge_softness)`.
- 2026-07-27 — **Plain front-to-back `over` — bucket-composite candidate 1 — is unshippable on any
  split parent, and M1.P3.T12 can decide that half of the bake-off on energy alone.** On a card carrying
  a thin volumetric span it gives centre alpha exactly `1−(1−0.41575)^K` (0.8834 / 0.9864 / 0.99982 /
  0.99999994 at K=4/8/16/32) and total `Σ alpha` of **2794.8 at K=4 and 4779.1 at K=16 against an input
  total of 1024.0** — validation scene (c) failed by **367%**, and not `depth_layers`-invariant.
  Coverage-partition reads **1023.998** (2.0e-6 relative) with **identical bits at every K**. This is
  what M1.P3.T9's fourth plane exists to prevent — `splitSpanAtBoundaries()` cuts the span into K parts,
  coverage-partition telescopes them back to the parent through the coverage-head/co-located machinery,
  and plain `over` ignores the area planes and composites each part as an independent layer — but the
  magnitude is worth pinning. T12 still owes the **point-fragment** cases from pixels.
- 2026-07-27 — **The bbox pad is driven by `max_radius`, never by the measured CoC**, so at `size = 0`
  the output is still padded by 101: `(0,-1,64,66)` → `(−101,−102,266,268)`. `bboxPadX()` is now the
  only radius consumer that uses neither the measured `rMax` nor `_proxyScale`. At 4K that is ~202
  extra rows and columns of band compute whenever the actual radius is small — one for M1.P4.T2 to
  weigh, since the pad must stay conservative enough not to clip scattered energy at the frame edge.
- 2026-07-27 — **The fourth "clamp one of a premultiplied pair and not the other" was the first one
  reachable from production, and it was shipping fog interiors 66% too bright.**
  `compositePixelCoveragePartition`'s final statement clamped `accAlpha` to [0,1] with no matching
  scale on the colour. M1.P3.T4 found it on hand-built planes, probed the real path (0 of 60
  randomised fields, 0 of 30 dense-fog fields), concluded it was unreachable, and left it — reasoning
  that changing a composite candidate right before M1.P3.T5 judges it from pixels was the greater
  risk. **T4's review probed harder and refuted that**: 300 randomised fields through the real
  `flattenPixelToSoA → scatterBandCPU → saturateBucketPlanes → resolveBandCPU` path (24×24 band,
  K∈[2,24], focus∈[0.8,60]) put **970 of 172,800 pixels above alpha 1, worst 1.6598**, shipping
  premultiplied colour **0.9959 against an honest 0.5975 — +66.0%**. The trigger is ordinary
  *overlapping volumetric fog*: several co-located residuals each attenuate `tClaimed` by `aRes/D_k`,
  which is weaker than the alpha each adds whenever `D_k > aRes`, so the sum over buckets isn't bounded
  the way the per-bucket terms are. Fixed by rescaling the colour by the same factor — the same
  down-only move `saturateBucketPixel()` already makes one pass earlier, fabricating no coverage and
  leaving scene (i)'s honest deficit untouched, and a no-op wherever `accAlpha ≤ 1` (every pinned
  number bit-unchanged; worst ratio error 65.98% → 0.0003%). **The right call was to fix it**: leaving
  it would have had M1.P3.T5 choosing between a correct candidate and one that renders fog 66% too
  bright. **The pattern is now 4-for-4 — every clamp applied to one half of a premultiplied pair in
  this codebase has been a bug.** Note the invariant that should have caught it was scoped
  `if (outAlpha > 1e-04f && outAlpha < 1.0f)`, and that `< 1.0f` excluded exactly the region where it
  breaks — the "dead `if`" failure mode already on record from Phase 1.1. Scoping removed.
- 2026-07-27 — **`HoldoutVisibility::build()` is order-dependent for overlapping spans**, so
  `appendPixel()`'s ascending `zFront` sort is a **correctness** requirement, not the fast-path
  precondition M1.P3.T3 recorded it as. Presenting the same overlapping spans descending instead of
  ascending changes the result by up to **2.7e-02 absolute** (0.1117 vs 0.0851, 24% relative) over
  200k trials. The `build()`-vs-`evalBoundaries()` equivalence fuzz cannot see this — both walk in
  whatever order they're given, so they agree with each other while both differ from the sorted
  answer. A pixel's LUT is a function of its sample *set* only because of that sort. Now pinned.
- 2026-07-27 — **M1.P3.T4's suite is mutation-verified at a higher bar than Phase 1.1's**: the
  implementer's own 35 mutations all died, but the review's independent 46-mutation list found **16
  survivors** against the as-submitted suite — among them an unclamped fourth plane, `resolveBandCPU`'s
  default branch silently becoming plain `over`, the ray-distance→Z correction dropped wholesale,
  `FragmentKind` dropped from the pre-merge predicate, a descending holdout sort, and
  `BucketPlanes::zero()` leaving the co-located plane dirty across band reuse. All closed; final state
  **40 killed / 6 verified-equivalent**. Two equivalence claims were checked rather than accepted: the
  disc kernel is **exactly** Y-symmetric including anamorphic pixel aspect (66,692 row pairs across
  PAR 0.5–2.0, zero asymmetries), and `tidyOverlapping()`'s disjoint-or-identical postcondition makes
  `zBack` monotone within a staged pre-merge run (3.75M groups, zero differences). Two of the six
  (`g == 0` in the deposit) are equivalent **only because v1 has one channel group** — they become live
  mutants at M2. One tolerance was fudged and got tightened: scene (c) asserted at 1e-05 against its own
  measured 8e-07 where the plan says ~1e-6, now 2e-06.
- 2026-07-27 — **M1.P3.T11 shipped all three holdout interpolants behind
  `ScatterParams::holdoutInterp` (`LogChord` = shipped default, `MidpointStep`, `LinearInT`) — and its
  review FALSIFIED the safety argument they were commissioned under.** T11's brief (and T10's
  Decisions entry) said the new variants fire "only when `T1 == 0` — reachable only from fully-opaque
  content", so they "cannot move any α<1 case, bit-for-bit". **That is false.** The branch keys on a
  *bitwise-zero* far-boundary transmittance, and a dense run of α<1 samples inside one bracket
  underflows the cumulative product to `0.0f` in plain float with no sample anywhere near α=1:
  measured thresholds **150 samples at α=0.5, 87 at α=0.7, 46 at α=0.9, 23 at α=0.99**. A sweep of
  20–200-sample α<1 stacks hit a bitwise-zero boundary in **52% of trials**. On the clean
  counterexample — 46 point samples at α=0.9 packed inside one K=16 bracket — the three variants
  diverge hard at z=48: LogChord **1.32e-18**, MidpointStep **0.0**, LinearInT **0.404**. Those are
  ordinary sample counts for a dense volumetric holdout column, i.e. precisely validation scene (f)'s
  content class. The earlier verification missed it because 1–4-sample stacks cannot underflow (a
  200,000-trial small-n sweep reproduces the original "0 hits" exactly).
  **No cheap gate exists**: `(T0, T1)` alone cannot tell "one truly-opaque sample" from "many α<1
  samples whose product underflowed" — both present as a non-degenerate `T0` and a bitwise-zero `T1`.
  This is an inherent limit of a two-values-per-bracket LUT rather than new debt, so the variants land
  as built, with the true gating condition documented in-source and pinned by tests covering both the
  safe small-n regime and the divergent deep-stack one. **Consequence for M1.P3.T5: the interpolant
  bake-off must render a genuine DENSE volumetric holdout — many samples, not a single fog slab
  sample — under all three variants, and the write-up must not imply the choice is risk-free on all
  content.**
  Two smaller corrections from the same review: `sizeof(ScatterFragment)` had grown 64 → 72 bytes
  because the new 1-byte field was placed ahead of an 8-byte-aligned pointer, forcing 7 bytes of pad —
  moved to the tail, back to 64 (it is a per-iteration stack local, never part of the 61 B/fragment SoA
  budget, so M1.P4.T1's formula was never affected, but the "costs nothing" comment was literally
  untrue). And MidpointStep's measured **0.00u leak is rig-position dependent, not a broken metric** —
  at this rig the card sits past the bracket midpoint so that variant only erases; swept across card
  position the worst-case leak is **3.06u against a 3.09u half-bracket**, matching the plan's
  "≤ half bracket" prediction exactly.
- 2026-07-27 — **M1.P3.T10 shipped the decoupled holdout boundary set: `HoldoutBoundaries`,
  uniform in Z over the frame's measured depth range, at the same K+1 entries/pixel.** The
  headline rig (opaque point holdout at z=50, K=16, range [1,100]) goes mean |vis error|
  **0.391 → 0.057**, bite **10.9 → 44.4** against a true 50; fragments at z=15/30/40 come back
  **1.000000** from 0.0215/0.000000/0.000000. Randomised sweep (K 4–128, 1–4 samples, 3000
  trials/shape, same harness before and after) — opaque samples: point **0.1113 → 0.0140**,
  sub-bucket spans **0.1113 → 0.0140**, wide spans **0.1117 → 0.0127** (7.9–8.8×); with
  α~U(0,1): **0.0826 → 0.0072**, **0.0806 → 0.0064**, **0.0780 → 0.0016** (11–49×). NOTE the
  absolute shipped baselines differ from the 0.464/0.431/0.234 quoted in T10's brief because the
  corpus generator differs (this one draws K uniformly in [4,128], which is what most of the gap
  is); the headline rig reproduces the brief's numbers to four figures, so the harness is
  calibrated and the before/after pairs above are the ones to compare.
  - **A holdout-depth-histogram-derived set was measured and REJECTED.** Boundaries at
    equal-occlusion-mass (equal optical depth, `−ln(1−α)` spread along each span) quantiles are
    *exact* for isolated opaque cards — 0.0000 on the headline, 0.0058 on a two-card frame — but
    they are **20× worse than uniform-in-z on volumetric holdouts** (opaque sub-bucket spans
    0.270 vs 0.014, wide spans 0.318 vs 0.013), because an opaque slab carries essentially all
    the frame's mass, every quantile crossing lands inside its front edge, and the rest of the
    range collapses into one bracket — the ΔCoC failure reproduced through a different door, and
    precisely validation scene (f). It also loses on a continuum of holdout depths (0.0266 vs
    0.0168 over 64 pixels), needs an **eager full-frame holdout pass** the design does not
    otherwise have (the depth pass reads the source only), and has no closed-form index. A
    one-sided variant is worse still: pinning only the front of each step leaves a log ramp
    across the whole gap to the next boundary (0.0817, i.e. no better than the ΔCoC set).
  - **Uniform-in-1/z confirmed not the answer**: 0.352 / bite 14.8 on the headline, 0.116 on the
    opaque sweep — it reproduces almost exactly the front-loaded bias that broke the ΔCoC set.
  - **`HoldoutSoA`'s contract changed**: it now carries `HoldoutBoundaries` **by value** next to
    `boundaryT` (the set the LUT was built at travels with the LUT, so a build and a lookup
    cannot drift onto different arrays), `boundaryCount` became a method, and
    `HoldoutLut::build()` takes `HoldoutBoundaries` instead of `DepthBuckets`. The boundary set
    must be **frame-global** — a fragment near a band edge scatters into two bands, and per-band
    sets would put a seam along every band boundary; `makeUniformHoldoutBoundaries(buckets)`
    derives it from the already-global measured range at no extra pass.
  - **`SampleSoA::boundaryIndex`/`boundaryFrac` are GONE.** They held
    `DepthBuckets::locateBoundary()`'s pair, which now indexes the wrong array;
    `scatterBandCPU()` derives the right pair from the fragment's own `depth` via
    `HoldoutSoA::locate()`. Per-fragment cost: the plan budgeted *two binary searches per
    fragment*; it is now **one** (`bucketOf`'s O(log K)) plus an **O(1) closed-form locate**,
    skipped entirely when no holdout is connected — and the SoA is **69 → 61 B/fragment**
    (~173 MB off a 4K/20spp band). Per-band LUT memory is unchanged at exactly `(K+1)·W·B·4`
    (17,825,792 B measured at K=16/4096×64).
  - Every M1.P3.T3 identity re-verified against the real scatter: LUT vs exact at the boundaries
    **0.000e+00** over 2000 random holdouts; `locate(boundary(i)) == {i, 0}` exactly at
    K∈{4,16,33,64,128}; all-ones LUT **bit-identical** to the disabled path; fully behind an
    opaque holdout **exactly 0**; fully in front **bit-identical** to no-holdout; the hard edge is
    **exactly one pixel wide in both directions** (1 transition, at the holdout's own pixel);
    α=1e-7 precision unregressed (1 ulp of 1.0, identical before and after) and bit-exact at the
    boundaries for a 200-sample 1e-7 stack. ASAN+UBSAN clean (empty holdout, single sample,
    zero-thickness spans, band outside the bbox, NaN/±inf depths and alphas, K=4/16/128).
    `DeepToImage` parity still **0 ULP** (12 depth-separated layers, alphas 1e-7…1.0, all four
    channels) — engine() is still the M1.P2.T2 flatten path, so it was never at risk.
  - The Design reference's **Holdout mechanics** paragraph has been corrected in place; do not
    read the pre-correction "at the K+1 bucket boundaries" wording from git history as binding.
  - **THE RESIDUAL, STATED PLAINLY (added at T10's independent review, which reproduced every
    number above with its own drivers — headline 0.3909/0.3519/0.0000/0.0565 at bites
    10.90/14.78/50.00/44.38, and the histogram's volumetric failure at 0.256/0.300 against
    uniform-in-z's 0.013).** The card at z=50 sits in bracket [44.31, 50.50], and **fragments from
    44.93 to 50 come back vis 0** — 5.07 world units, 82% of the bracket, of genuinely-unoccluded
    geometry fully erased. 2.28 units at K=32, 0.89 at K=64, 0.196 at K=128; worst case over card
    position at K=16 is 5.57 units. It scales as ≈`(measured depth range)/K`, so it is mild on a
    tight scene and material on a wide one. **This is NOT all "the irreducible `(T0−T1)/2`
    placement error"**, and T10's first draft of this entry called it that. Half a bracket of
    placement uncertainty is irreducible with K+1 values; the *other* half is the log chord's own
    degeneracy — it floors `log T` at `kMinTransmittance`, so an opaque step collapses vis to ~0
    across the whole bracket instead of the bound's 0.5, and always toward camera. Measured
    alternatives, all zero memory / zero per-fragment cost, firing only when `T1 == 0` (reachable
    only from fully-opaque content, where "log T is linear in z" is not the model at all):
    | interpAtBucket variant | headline mean | erased in front | leaks behind | opaque sweep | α<1 sweep |
    |---|---|---|---|---|---|
    | log chord (shipped) | 0.0565 | 5.07 u | 0.00 u | 0.0134 | 0.0075/0.0058/0.0018 |
    | midpoint step on `T1==0` | 0.0262 | 2.59 u | ≤ half bracket | 0.0074 | unchanged |
    | linear-in-T on `T1==0` | 0.0266 | 0.00 u | 0.49 u (to 6.16 worst) | 0.0097 | unchanged |
    Both cut the mean ~2× and neither moves any α<1 case. The trade is *erasing FG in front* vs
    *leaking BG behind* — scene (e) can fail either way, so **judge it from pixels at M1.P3.T5,
    do not derive it**. "One extra entry" is NOT a fix (17→18→19→21 entries gives 5.07 → 1.83 →
    4.45 → 3.96 units: phase noise, not convergence); snapping a boundary to a detected opaque
    step would fix it exactly but needs the eager full-frame holdout pass the histogram set was
    rejected for. **Land T10; open a follow-up on the interpolant.**
- 2026-07-26 — **The coverage plane `Σ w·vis` is deposited ONCE per fragment, into the nearer
  bucket** — not into both buckets of a fractional split. M1.P3.T2 shipped the full-deposit form on
  the argument that `A_k ≤ C_k` is a precondition of the coverage-partition candidate; its review
  refuted that (`A_k > C_k` is legitimate and simply means "co-located rear deposit") and found the
  full deposit was a **2× energy error on every partially-covered region**: one opaque fragment split
  across two bucket centres gave band alpha sum **2.0000 against an honest 1.0000**, with colour
  inflated identically; fog α=0.5 gave 0.5858 vs 0.5000 (so fog is *not* spared — it is a `Σa_k/α`
  effect, not an α² one); a defocused opaque half-plane edge went 0.5632 → 1.0000. That last case
  destroys scene (i)'s whole point: a pixel with an honest 0.6 coverage reported 1.2, which is
  precisely the discrimination the plane exists to provide. Fixed in the review, with a residual term
  added to `compositePixelCoveragePartition` so alpha beyond a bucket's own coverage claims no new
  area and is `over`-attenuated by `tClaimed`. Post-fix, over 3000 random `(α, fraction, radius)`
  single fragments, CoveragePartition conserves energy to **1.37e-07** alpha / 1.24e-07 premult
  colour. **Plain `over` is up to +93.8% on the same corpus** — so `over` now has two failure modes,
  inflating as badly as it deflates, which M1.P3.T5's bake-off should weigh.
- 2026-07-26 — **Volumetric span splits still carry that over-count, and it is M1.P3.T8's job.**
  Each part of a split parent deposits its own coverage into its own bucket, so a slab is counted once
  per bucket it spans: a 4-part α=0.9 slab measures **1.7506 / 1.7372** (partition / over) against an
  honest **0.9000**, growing toward K× as α→1, exact only at full kernel coverage. It was deliberately
  not fixed inside M1.P3.T2 because the fix changes M1.P3.T1's committed SoA contract (a
  head-of-group bool through `FragmentRecord`/`SampleSoA`) and needs its own test coverage — the
  milestone's sizing rule says that is a new task, not an "and then also". **M1.P3.T5's bake-off is
  not meaningful on scenes (f)/(g)/(i) until T8 lands.**
- 2026-07-26 — **SUPERSEDED 2026-08-16 by M1.P3.T17** (which decided the bake-off from rendered
  pixels and deleted `ScatterParams::combine` outright — there is no default left to be provisional
  about, and no explicit-setting discipline left to keep). Kept for the history of how the default
  was chosen. ~~`ScatterParams::combine` defaults to `CoveragePartition`, but the default is
  **provisional and non-authoritative**: M1.P3.T5 still decides from rendered pixels per the
  bucket-composite entry below. The default is not neutral (plain `over` is *known* to fail scene (c),
  so defaulting to it would ship a known-failing default while T5 runs), and post-fix
  CoveragePartition is the only candidate satisfying its own identities. To stop the default biasing
  anything, M1.P3.T4 and M1.P3.T5 must set `combine` explicitly in every case and every render. The
  review also fixed an unreachable `default:` branch that routed to `FrontToBackOver` while claiming
  it was "the safer of the two" — no longer true.~~
- 2026-07-26 — A flat opaque field resolves to **0.9999992, not exactly 1.0**: the disc LUT's
  per-entry normalisation residual (~5e-8 each, over ~113 contributing fragments). The design
  reference's scene (c) says "alpha ≡ 1 exactly" and M1.P3.T2 initially reported exactly 1 — that
  reading was an artifact of the coverage over-count above being clamped back down by the saturation
  pass. **Scene (c) and M1.P3.T4 must assert `|α−1| ≤ ~1e-6`, not equality**; the α=0.5 flat fog case
  lands at 0.5000010 on the same basis. Saturation itself is confirmed down-only and firing on the
  ordinary path (2 coincident opaque sharp fragments 2.0 → 1.0; 3 same-bucket fog 1.5 → 1.0 — a clamp,
  not a restoration of the true 0.875, as designed).
- 2026-07-26 — Where fragments carrying **different split fractions** share a bucket, the planes lose
  the pairing and alpha comes in slightly low: two fully-covering 50% fog layers with random fractions
  give **0.7297 against the exact 0.75**, ~4% per layer. ~~Identical under both bucket-combine
  candidates, so it does not bias M1.P3.T5's comparison~~ — **CORRECTED 2026-08-16 at M1.P3.T12's
  review: it is NOT candidate-independent. It is signed, and the sign flips with the candidate.**
  Measured end to end on the harness (fog-density scenes f3c/f3d at K=4/8/16). CoveragePartition:
  **−0.763% / −0.610% / −0.113%** (f3c) and **−1.539% / −1.395% / −1.675%** (f3d).
  FrontToBackOver: **+2.194% / +0.212% / +0.074%** (f3c) and **+4.799% / +1.956% / +0.967%** (f3d).
  So `over` errs *high* exactly where `partition` errs low, and M1.P3.T17 must not
  treat this residual as a common-mode term that cancels out of the comparison. Magnitude on the
  harness's arrangements is ~0.9%/layer rather than ~4%; f3c is K-convergent (exact at K≥64) and
  f3d is non-monotone in K (exact at K=128), which is what the random-split-fraction mechanism
  predicts. Expect it in scenes (f)/(g) — but read it as candidate-discriminating evidence, not as
  noise to be subtracted.
- 2026-07-26 — `scatterBandCPU`'s signature deviates from the design reference's four-argument sketch
  and the deviation is **accepted**: `KernelSampler`, a per-thread `ScatterScratch` and an optional
  `ScatterStats*` are added, and the bucket composite is a separate `resolveBandCPU` so a band can be
  scattered from several SoA chunks and so M1.P3.T4/T5 can drive the passes independently. The four
  design-named parameters keep their names, order and positions. The M3 CUDA seam survives: the
  virtual `KernelSampler::kernel()` call happens once per (fragment, channel group) in the `.cpp`
  driver, never inside a `DEEPC_HD` body and never per pixel — every per-span body takes a POD
  `KernelView`. Verified: the TU compiles with plain `g++ -std=c++17` (no NDK), and
  `-fopt-info-vec` at `-O3 -mavx2 -mfma` vectorizes all three deposit loops and the composite channel
  loops at 16- and 32-byte vectors.
- 2026-07-26 — **The milestone's "the local build compiles clean" verify step was vacuous for every
  Phase 1.2 and 1.3 task**: neither `DeepCDefocus.cpp` nor `DeepCDefocusScatter.cpp` was ever added to
  `src/CMakeLists.txt` (confirmed by the PM), so the local build built the other 27 plugins and none of
  this node, and the tests target builds only the header-only math. Both files were in fact compiled by
  hand, ad hoc, per task — which is why nothing was missed — but the gate as written proved nothing.
  Registration is pulled forward from M1.P5.T1 into **M1.P3.T7**, which runs next, so that every
  subsequent task's build gate is real. The `-mavx2 -mfma` compile options stay at M1.P5.T1, where the
  `-ffp-contract` parity hazard is documented and where M1.P4.T2 will have inspected vectorization first.
- 2026-07-27 — **The Design reference's "transmittance LUT at the K+1 bucket boundaries" is wrong and
  is being replaced at M1.P3.T10.** Found at M1.P3.T3's review, which built the case the task itself
  had not: an opaque *point-sample* holdout — a solid card, the commonest holdout shape — rather than a
  volumetric span. ΔCoC bucket spacing bounds banding, a CoC criterion, and spends 15 of 16 buckets in
  front of focus, so the default rig collapses the whole back side into one `[10, 100]` bucket and a
  card at z=50 starts occluding at **z=10.9**: a fragment 35 units in front of it is 98% erased, and
  fragments at z=30/40/49 vanish entirely (mean |vis error| 0.391, max 1.000). That is the node's
  differentiator inverted — the thing that is supposed to stay pixel-sharp instead eats everything in
  front of it. Neither K (bites at 25.8/40.6/40.2 for K=32/64/128) nor a better interpolant
  (linear-in-T: mean 0.476 → 0.450) helps, because the dominant term is boundary *placement*. The
  Design reference's "exact for the exponential model" claim holds only when a holdout span covers the
  whole bracket. Fix is a **decoupled boundary set**: uniform-in-z gives mean 0.057 and bites at 44.4
  against a true 50, at identical memory and still O(1)-indexable. Sequenced before M1.P3.T4 rather
  than at T5 because it changes `HoldoutSoA`'s contract, and T4's permanent tests should be written
  once against the final one.
- 2026-07-27 — Two smaller holdout-path defects fixed at M1.P3.T3's review. **NaN holdout depths are
  dropped**, deliberately diverging from the source flatten's `sanitizeSampleDepth()` NaN→0: on the
  source side z=0 is harmless (radius 0, composites in place), but on the holdout side z=0 sits in
  front of `boundary(0)`, so one NaN-`DeepFront` opaque sample drove the whole pixel's LUT to ≡0 at
  every boundary — a black hole in the plate. ±inf is kept (a legitimate far-field holdout). And the
  holdout SoA now carries a **mandatory `depthScale`** matching the flatten's ray-distance→Z factor;
  without it an uncorrected holdout sits systematically too far back off-axis — **47.3% too far in Z
  at the corner of a 20mm / 36×24 frame**. That is the same failure class the plan already names for
  `computeDepthRange()`, reached through a different door, and it would have shipped silently at T5.
- 2026-07-27 — The **holdout matte AOV must not be computed as `1 − boundaryT` in float.** `vis` is
  only ever consumed as a multiplicative weight, so its ~4e-08 absolute error at α=1e-7 is harmless
  and `expm1`/`log1p` genuinely do not apply on that path — but the AOV writes `1 − vis(∞)`, a
  subtraction, and the relative error in that deficit is **100% at α=1e-7 and 19.2% at 200 compounded
  samples of α=1e-7**. The AOV is knob-only today, so this lands with M1.P3.T5's wiring.
- 2026-07-27 — Holdout samples are deliberately **not** run through `tidyOverlapping()`: the model
  multiplies independent per-sample transmittances (Beer's law), so overlapping spans need no merge,
  unlike the scatter's additive bucket planes. Verified analytically rather than argued — two
  overlapping spans against a hand-derived Beer solution (densities add in the overlap) give worst
  error **1.24e-08** for the raw product versus 2.98e-08 for the tidied 3-span mixture, i.e. the same
  answer with no double-attenuation. `HoldoutVisibility::build()` does not assume tidy *or* sorted
  input: 20,000 randomised overlapping cases and 17,500 unsorted ones all match `evalBoundaries()` to
  0.000e+00; the `zFront` sort is a fast-path precondition only.
- 2026-07-27 — **M1.P3.T9 landed the fourth plane. In front of focus the coverage-partition composite
  is now exact** (≤1e-6) at any part count, any alpha and any radius spread — a span covering that
  whole side went **−68.94% → −0.0000%**. `D_k` accumulates `Σ w·vis` over exactly the deposits that
  carry alpha but claim no new area, so `resLocal = (w_p·a_p)/w_p = a_p` with `w_p` cancelling part by
  part; `aRes ≤ D_k` holds by construction rather than by clamp. Memory formula is now
  `K·W·B·(C+3)·4`, verified byte-for-byte against a live `sizeBytes()`: **100.66 → 117.44 MB** at 4K
  defaults, **805.31 → 939.52 MB** at K=128. Everything that must not move didn't: 0 differing floats
  over 3000 random point fragments and over equal-radius volumetric slabs, and all four hand-built
  identities (two 50% fog 0.7500000, receding opaque 1.0000000, scene (i) 0.6000000, 4-part opaque
  slab 1.0000000) bitwise identical. `FrontToBackOver` is untouched and reads neither area plane, so
  T5 still compares like for like — it does now pay the plane's memory and deposit loop, which the
  losing path's deletion at T5 reclaims.
- 2026-07-27 — **Behind focus the residue is structurally irreducible by any per-bucket plane. This is
  settled, not open.** The head is the part nearest focus, so its disc is the *smallest*; where a rear
  part reaches a pixel the head never touched, that bucket holds four zeros — colour, alpha, new area
  and co-located area — and a per-pixel per-bucket reduction has no channel through which to learn
  that `a₀` occludes what follows. Within-parent occlusion happens along the ray *before* the blur.
  Both alternatives were tested and fail: depositing the head's area at the largest part radius leaves
  `local = 0` (no occlusion) unless its alpha and colour move too, which is "rasterise the parent at
  one radius" and deletes the depth-graded split the span split exists for; a per-parent occlusion
  term needs per-parent composite state, i.e. the fragment lists the design rejected as
  memory-infeasible. Deeper: the error follows from **one global front-to-back visit order plus
  per-bucket planes** — behind focus the part whose disc contains the others is the rearmost, visited
  last, and a back-to-front composite just mirrors the problem onto the front field. Measured residue:
  **+45.2 / +51.2 / +59.1%** at 3/4/8 buckets at α=0.9 (from +56.2 / +76.5 / +113.3% pre-T9), +7.69%
  for a full-range span, +6.65% for a focus-crossing span — that last number is *entirely* its
  behind-focus half, not a new front-of-focus error.
- 2026-07-27 — **The dominant error in any multi-sample deep pixel is occlusion-before-blur, and it
  belongs to neither bucket-composite candidate** — it is unchanged by T8 and T9 and cannot be fixed
  by picking a winner at T5. Two opaque point samples at one pixel band-sum to 2.0000 against a true
  1.0000 under CoveragePartition and 3.9726 under plain `over`; four receding opaque, 4.0000 vs
  6.6573. **But it is an isolated/sparse-content effect: the identical content as a flat field is
  exact under both candidates** — 4 receding opaque → 1.0000000, two 50% fog layers → 0.7500000, a
  full-range α=0.9 fog slab → 0.8999999. The two readings of the same input differ by two orders of
  magnitude, so M1.P3.T5 must report band-alpha and flat-field figures separately and must not read a
  sparse-content number as a fog-interior one.
- 2026-07-27 — A **colour:alpha desync** was found and fixed in T9's new area split before it landed:
  `local = clampf(aCov/cov, 0, 1)` capped the alpha the fit/excess branches emit while the colour
  beside it was scaled by the *unclamped* `aCov/a`, so hand-built planes `(A=1.0, C=0.3, D=0.2)` gave
  alpha 0.3 with premultiplied colour 0.6 — a ratio of 2.0. This is character-for-character the defect
  M1.P3.T8's review fixed on the residual term (0.8748 against a true 0.5); it recurred in the new
  clamp within one task. It was unreachable in production (`aCov ≤ cov` follows from the deposit
  invariant) but is now guarded by construction. **Third occurrence of the same failure shape — clamp
  one of a premultiplied pair and not the other.** M1.P3.T4 should assert the colour:alpha ratio as a
  standing invariant rather than case by case.
- 2026-07-26 — **The fourth accumulation plane gets built before M1.P3.T5's bake-off, not deferred to
  it** (user's call, asked at the M1.P3.T8 boundary). The alternative — render scenes (f)/(g) through
  both candidates as-is and add the plane only if the radius-spread error shows in pixels — matches how
  the bucket-composite question itself was decided, but does not apply here: the coverage-partition
  candidate is the only one that reads the coverage plane, so comparing it while it carries a
  known-exactly-fixable −92% error would not tell us which candidate is better, only that candidate 2
  was crippled. Cost is ~+17% of the bucket planes (~+17MB at 4K defaults) against a ~2.4GB SoA, so
  memory is not the deciding term. Landed as M1.P3.T9, which runs before T3/T4/T5.
- 2026-07-26 — M1.P3.T8 fixed the volumetric-split coverage over-count with a **coverage-head bit**:
  the flatten marks the front-most emitted part of a split parent, the scatter deposits coverage only
  from it (`depositWeight = coverageHead && group == 0`), and the composite's residual term carries the
  rest as alpha-without-coverage. Stored as **bit 1 of the existing `kind` byte**, which was renamed
  `flags` so any un-migrated `static_cast<FragmentKind>` fails to compile; `SampleSoA::sizeBytes()` is
  byte-identical, so M1.P3.T1's 69 B/fragment logical / 113 B resident figures and the `memory_limit`
  entry were unchanged **by T8** (M1.P3.T10 later moved both to 61 / ≈100 — see the 2026-07-27
  entries). Pre-merge survival is an **OR over the group**, not the group head's flag — a
  group is a maximal run of adjacent staged fragments and can mix a non-head part of parent A with a
  head point sample B, so taking the first flag would drop B's coverage. Fuzz-verified rather than
  argued: head count is exactly 1 per parent over 200,000 randomised single-parent cases, and never
  exceeds the parent count over 60,000 multi-parent cases. **"Parent" means a POST-TIDY sample** —
  `tidyOverlapping()` cuts overlapping spans into disjoint segments first, so N depth-disjoint samples
  at one pixel still deposit N coverages. Measured: a 4-part opaque slab's band alpha 4.000000 →
  1.000000; a hand-built 60%-coverage opaque slab 1.0000 → 0.6000 (scene (i)'s honest hole stays
  honest); the whole point-sample path is bit-unchanged.
- 2026-07-26 — **The coverage-head fix is exact only when a parent's parts share a CoC radius, and is
  inexact in BOTH directions when they don't** — the open item M1.P3.T5 must weigh. Each part is CoC'd
  at its own midpoint, so a slab spanning N buckets rasterizes N different-sized discs, and the
  governing quantity is the **radius ratio inside one parent, not the bucket count**. It is unbounded
  whenever a span reaches the focal plane: that part takes the sharp path (w=1 into one pixel) while
  the head is spread over a disc. Measured on the standard rig at α=0.9, front of focus: −0.0% at 1–2
  buckets, −6.4% at 4, −24.8% at 8, −46.5% at 12, **−92.1% for a full-range span**; at α=0.1 full
  range, −24.1% (against +0.9% for the old over-counting form, so at fog alphas with a wide spread the
  new form can be *worse* — the old over-count was first-order-small at low α). Behind focus the flag
  clearly helps: +14.5%/+36.2%/+76.5%/+113.3% at 2/3/4/8 buckets against the old +51.7%/+77.9%/
  +93.5%/+118.1%. Net over 219 randomised single-parent cases: mean |band-alpha error| **6.7% vs
  10.2%**, worst **89.7% vs 202.1%** — landed as a clear net improvement, exact where the design
  intends (parents inside one or two buckets). **The exact fix is a fourth accumulation plane**
  (per bucket, "co-located area" beside "new area"), which makes `resLocal = aRes/D_k = a_p` exactly at
  any spread; memory formula would become `K·W·B·(C+3)·4`. Choosing a different head does NOT fix it —
  a largest-radius head is identical in front of focus (the front-most part already *is* the largest)
  and unusable behind it, since the head must be the bucket the composite visits first or every part in
  front of it falls into the `claimed == 0` branch. Note the real reason is that the discs' normalised
  **densities** cross, not that their supports fail to nest.
- 2026-07-26 — **The composite's residual alpha is no longer clamped to the claimed area.** Found at
  M1.P3.T8's review: `accAlpha += min(aRes, claimedArea) · tClaimed` was unreachable for the fractional
  split it was written for (`aRes = w·a₁ ≤ w = claimedArea` always) but fires constantly once non-head
  parts arrive at a different kernel radius, and **it clamped only the alpha — the colour term beside it
  has none**. A 4-part span reaching focus therefore produced a premultiplied colour:alpha ratio of
  0.8748 against the input's true 0.5: a 75%-too-bright pixel, not a dim one. Now `accAlpha += aRes ·
  tClaimed`; the clamp is retained for the *transmittance*, where it is a genuine bound. Post-fix the
  ratio is 0.5000 on every case, every documented identity is bit-unchanged (two 50% fog layers
  0.750000, receding opaque 1.000000, scene (i) 0.600000), and it cannot over-count — per bucket the
  three terms still sum to at most `A_k`.
- 2026-07-26 — `compositePixelCoveragePartition` accumulates `claimedArea` directly instead of deriving
  it as `1 − freeArea`, which cancels catastrophically at small per-pixel coverage — exactly the case
  the residual term divides by. Measured on 11 co-located layers: −8.1e-06 relative at coverage 1e-3,
  +1.04e-04 at 1e-4, +8.6e-04 at 1e-5, **+11.6% at 1e-7**; ≤1.4e-07 everywhere after. This is recorded
  separately from the coverage-head fix on purpose: it was reachable **before** M1.P3.T8 (a fractional
  split's rear deposit at a 21px radius sits at coverage ~7e-4), so it changes numbers on a path that
  M1.P3.T2 had already landed, and it was folded into T8 silently rather than recorded.
- 2026-07-26 — **`pre_merge` now moves the coverage plane, and ON (the default) is the accurate
  branch.** Two distinct co-located point parents: OFF gives alpha 0.701926 with coverage 2.0; ON gives
  0.580000 with coverage 1.0, and 0.58 is the exact sequential `over` — so the merged reading is right
  and the unmerged one is the over-count. Acceptable, but it must be **documented in node help**
  (M1.P5.T2) because the coverage plane is what discriminates scene (i)'s honest coverage deficit from
  bucketing loss, and a perf knob silently changing that would make a deficit diagnosis knob-dependent.
  M1.P3.T5 must set `pre_merge` explicitly in every render for the same reason.
- 2026-07-26 — M1.P3.T7 wired the node in with `list(APPEND PLUGINS/FILTER_NODES DeepCDefocus)` inside
  a new `if (UNIX)` block (the `if (OpenGL_FOUND) list(APPEND PLUGINS_MWRAPPED DeepCPMatte)` idiom),
  not by editing the unconditional base `set(...)` lines. `PLUGINS` is the correct list — `DeepCDefocus`
  is a plain `Iop` like `DeepCShuffle2`, so it must NOT go in `PLUGINS_MWRAPPED`, which links the
  `DeepCWrapper`/`DeepCMWrapper` object libraries it neither uses nor needs. **`if (UNIX)` is provably
  the right guard for keeping the node out of the Windows build**, verified rather than assumed at T7's
  review: `docker-build.sh:245` passes `-DCMAKE_SYSTEM_NAME=Windows`, and CMake's `UNIX`/`WIN32` track
  the *target* (the host-only forms are `CMAKE_HOST_UNIX`/`CMAKE_HOST_WIN32`), so on the Linux docker
  host the Windows cross-compile still evaluates `UNIX` empty / `WIN32` 1 — reproduced locally. That is
  the same predicate the top-level `CMakeLists.txt:11` Linux-only flag block already depends on in
  production. `FILTER_NODES` is live, not dead: it substitutes into `python/menu.py.in` via
  `configure_file` at `src/CMakeLists.txt:153`, placing the node in the Filter toolbar submenu.
  `DeepToImage` parity re-verified 0 ULP against the CMake-built `.so` on a harder scene than before
  (two `DeepMerge`d layers, per-pixel varying depth, 2 samples/pixel, alpha < 1 on both, 6144 samples).
- 2026-07-26 — The FMA/`fp-contract` parity guard has **two independent layers**, so M1.P5.T1 is not
  the sole defence: `DeepCDefocus.cpp:847-943` already wraps the `flattenPixel` composite loop in
  `#pragma GCC push_options` / `#pragma GCC optimize("fp-contract=off")`, which holds even once
  `-mavx2 -mfma` land on the target. The CMake-level omission is the belt; the pragma is the braces.
  M1.P5.T1 should still verify parity after adding the flags rather than trusting either layer alone.
- 2026-07-26 — Alpha and coverage are deposited by **channel group 0 only** (they are not per-channel
  quantities). Unobservable in v1, where every `channelRadiusScale` is 1.0 — but **M2 must decide which
  group owns alpha** once chromatic-aberration scales diverge. Noted in-source at the deposit site.
- 2026-07-26 — Coverage is **clamped to [0,1] at use, not in the plane**. That is what keeps the local
  opacity `A_k/C_k ≤ 1` after the saturation pass has pulled an over-covered bucket's alpha down to 1,
  and it makes an over-covered bucket degrade gracefully to plain `over`.
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
- 2026-07-26 — M1.P3.T6 rewrote `tidyOverlapping()`'s split pass as a single front-to-back sweep,
  35–583× faster (9.96ms → 0.28ms per pixel at 32 overlapping spans; a 30-scene render set 424s →
  17.1s), unblocking T5's fog scenes. Geometry, sample counts and all volumetric records are
  bit-identical, and point-only input — all a released build could render — is bit-identical over
  292k cases and 65 rendered scenes. The `over` order of *coincident point samples* does move, by up
  to 3.1e-03 through the shipped blur nodes and up to 0.89 offline where coincident samples carry
  different colours; alpha is unaffected. Full reasoning, the corrected root-cause analysis, and the
  decisions to keep the merge pass's second `std::sort` and NOT to adopt `stable_sort` are in
  `PLAN/DECISIONS/2026-07-26-tidyoverlapping-single-pass.md`. That file's "release note must say"
  paragraph is the one to copy into the PR.
- 2026-07-26 — The `memory_limit` formula **extends to cover the SoA fragment buffers**, not just the
  bucket planes: measured at M1.P3.T1 as 69 B/fragment at C=4 *logically*, but **113 B/fragment
  actually resident** (139 at C=8) once geometric capacity slack across the 15 independent buffers is
  counted — so a 4096-wide band at B=64 / `max_radius`=100 / 20spp is ~2.4GB, not the ~1.49GB the
  logical figure suggests, and either way it dwarfs the ~100MB of bucket planes the original formula
  counted. **Superseded in part by M1.P3.T10, which removed two of the fifteen buffers: 61
  B/fragment logical at C=4 (77 at C=8), ≈100 B resident at C=4 and ≈126 at C=8 — the
  resident/logical ratio is unchanged, so re-measure exactly at M1.P4.T1. The rejection below no
  longer applies to the boundary pair: T10 dropped it and paid NO search, because
  `HoldoutBoundaries::locate()` is O(1) closed form. It still stands for the `deposit` arrays,
  whose locator (`bucketOf`) is not.** Budget on the resident figure, and call `reserveFragments()` (which already exists) to
  collapse the slack wherever the caller can estimate the count. Rejected the
  alternative of dropping the precomputed deposit/boundary arrays (−32 B/fragment) and recomputing
  them in the scatter: that trades a real memory win for a binary search per fragment on the hottest
  loop in the node, and the `memory_limit` knob already exists precisely to cap concurrent in-flight
  bands (floor 1, then shrink B). M1.P4.T1 must budget on the combined figure.
- 2026-07-26 — Within-bucket additive accumulation over-counts **same-pixel** fragments, not only the
  different-pixel case the design reference anticipated, and **`pre_merge` does not mitigate it**.
  Measured at M1.P3.T1's review on disjoint fog spans sharing a bucket: **+33.3% alpha at 2 spans,
  +71.4% at 3, +113.3% at 4** — reconstructed alpha reaching 1.0 / 1.5 / 2.0 — and *identical with
  `pre_merge` on or off*, because its 0.25px radius tolerance is far below one bucket's ΔCoC step
  (~1.43px on the standard rig) and so never fires. **2,519 of 4,000 randomised multi-sample pixels
  exceeded alpha 1**, so this is the ordinary case for fog, not a corner. Consequences: M1.P3.T2's
  saturate-down pass is fully load-bearing for ordinary volumetric input rather than a safety net for
  overlapping surfaces, and this bears directly on M1.P3.T5's bucket-composite comparison — record
  the behaviour at K=8 and K=64 when the scenes run, since K trades this against banding in the
  opposite direction to what the design assumed. (The design accepted "within-bucket loss of ordering
  between *different-pixel* fragments" as the residual approximation; this is the same mechanism at
  the same pixel, and much larger.)
- 2026-07-26 — `merge_tolerance` is in **CoC-radius pixels**, not z-distance. The knob table says
  "0.25px / 0–2px" while the `optimizeSamples()` merge this task reuses groups by z-distance; the
  CoC-radius reading is the only one under which both the knob's stated unit and the design's
  "lossless when radii are equal" claim are true. Grouping predicate: same fragment kind AND same
  containing bucket AND |Δradius| ≤ tolerance. Verified lossless against true sequential `over`
  (two same-bucket points at α 0.3/0.4 merge to exactly 0.58; unmerged additive gives 0.69999). Knob
  table amended to state the unit.
- 2026-07-26 — M1.P2.T2's `FrameCache` was ported from `std::vector<float>` to `PodBuffer<float>` at
  M1.P3.T1, honouring the in-source note M1.P2.T2 left asking for exactly that once the POD buffer
  existed. `DeepToImage` parity re-verified bit-exact after the port. `DeepCDefocus.cpp` now includes
  `DeepCDefocusScatter.h` but uses only the header-only template, so no link dependency is added
  ahead of M1.P5.T1's CMake wiring.
- 2026-07-26 — M1.P3.T0 adjudicated volumetric tidying in favour of the **OpenEXR mixture model**
  and fixed `tidyOverlapping()`'s merge accordingly; a second, independent precision defect in the
  same function's split pass was fixed alongside, and that one **visibly changes shipped
  `DeepCBlur`/`DeepCBlur2` output** (2.32e-02 abs / 12.4% rel in alpha on point-inside-span input) —
  landed deliberately, on the user's explicit call, and needing a release note. Full reasoning, the
  ray-march validation, the measured errors and the shipped-node impact analysis are in
  `PLAN/DECISIONS/2026-07-26-volumetric-tidying-semantics.md`. Consequences for this milestone:
  scene (a)'s volumetric clause is now a real parity gate at ≤2.4e-07 with `volumetric_composition`
  pinned ON; M1.P3.T1 inherits a correct tidy pass and needs no rework, but should measure the
  `log1p`-per-sample cost the merge adds on the hot path.
- 2026-07-26 — **`deepc::tidyOverlapping()` never terminated on overlapping volumetric samples**, a
  pre-existing bug in committed shared code, fixed during M1.P2.T2's review. The split loop always
  split `samples[i]` at `samples[i+1].zFront`; when the two shared a `zFront` that *is*
  `samples[i]`'s own front, so it made no progress — and the loop created that configuration itself,
  since splitting `[1,5]` at 3 yields `[3,5]`, which then shares a front with `[3,9]`. Every
  overlapping volumetric pair hit it: the vector grew without bound and Nuke hung unkillably, with
  the frame lock held and no `aborted()` check to escape through. **Blast radius is wider than the
  new node**: `optimizeSamples()` calls `tidyOverlapping()` (`DeepSampleOptimizer.h:235`), and both
  shipped `DeepCBlur` and `DeepCBlur2` call `optimizeSamples()` — so this was live in released
  plugins, not just here. Fix: pick which of the pair to split so the split point is strictly
  interior — fronts differ → split the earlier at the later's front (as before); fronts equal →
  split the longer at the shorter's back, or skip if identical (the over-merge pass already handles
  that). Every split point is an endpoint that already existed, so the endpoint set never grows and
  termination is provable. This is why the milestone's "reuse `tidyOverlapping()` rather than
  reimplementing" constraint was still the right call — the reuse is what surfaced the bug.
- 2026-07-26 — Flatten parity with stock `DeepToImage` is **bit-exact for point samples** and is
  scoped, not universal — see validation scene (a), rewritten at M1.P2.T2. Getting to 0 ULP needed
  two specific choices, both of which later phases must preserve: back-to-front accumulation, and
  the `acc = acc*(1−a) + c` form of `over` as a separate multiply and add. Front-to-back
  `acc += c·T` was off by exactly 1 ULP. The composite loop is guarded with
  `#pragma GCC optimize("fp-contract=off")` because `-mfma` under GCC's default
  `-ffp-contract=fast` fuses that multiply-add and costs the parity (verified: 5 ULP without the
  pragma, 0 with it, at `-O3 -mavx2 -mfma`). The pragma is the authority rather than a build flag,
  since a flag can be dropped in a refactor and the failure mode is a silent 1e-7 pixel drift. Note
  the pragma covers `flattenPixel` only — `tidyOverlapping()` is compiled with contraction on and
  contains the remaining FMAs; harmless today (measured identical under `-mavx` and `-mavx2 -mfma`)
  but the "no FMA on the parity path" guarantee is narrower than it reads.
- 2026-07-26 — The frame cache is published under a lock as a `shared_ptr<const FrameCache>` and
  readers snapshot it **under that same lock**, so a bare `shared_ptr` is sufficient and
  `atomic_load`/`atomic_store` are not needed. Two threads cannot both compute (verified: 16 threads
  on 2048×1152 with a 200ms stall injected to widen the race window → exactly 2 computes for 2
  distinct hashes, 0 concurrent entries, output 0 ULP; and 24 renders across 12 hashes → 24 computes,
  0 concurrent, all bit-exact). One real bug was found and fixed here: `Op::hash()` was sampled
  *before* taking the guard, so a re-validate in between could publish a cache computed from the new
  `info_` under the old key — a permanent recompute-every-cook livelock. `DD::Image::Lock` is a
  non-recursive pthread mutex, so the "upstream cooks can't re-enter" invariant is load-bearing: a
  violation deadlocks rather than corrupting.
- 2026-07-26 — Serial-phase performance baseline, for M1.P4.T2 to beat: the frame-wide precompute is
  ~2–3× slower than stock `DeepToImage` on a cold cook (4096×3112 / 12 layers: 3.99s vs 1.81s;
  2048×1556: 6.5s vs 2.2s on the pre-optimisation build), and free on a warm one (0.39s at 4K).
  Expected — Nuke cooks `DeepToImage` per row across all threads while this phase deliberately
  serialises on one lock until M1.P4.T1. Frame cache at 4K with default knobs is ~155MiB, ~310MiB
  transient during a recompute (old + fresh coexist), and ~799MB at the `kMaxRadiusCap` ceiling.
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
  to be able to exceed them, but the design's memory formulas (`K·W·B·(C+3)·4` per band since M1.P3.T9, LUT
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
