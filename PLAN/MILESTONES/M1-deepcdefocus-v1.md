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

> Complete. Full task briefs archived in [ARCHIVE/M1-history.md](../ARCHIVE/M1-history.md).

- [x] M1.P0.T1 — Confirm the local Nuke SDK builds the existing repo clean
- [x] M1.P0.T2 — Document the local build as this environment's dev-loop compile gate

## Phase 1.1: Pure math foundation (no NDK, unit-testable)

> Complete. Full task briefs archived in [ARCHIVE/M1-history.md](../ARCHIVE/M1-history.md).

- [x] M1.P1.T1 — CoC and holdout-visibility math
- [x] M1.P1.T2 — Depth-bucket and compositing math
- [x] M1.P1.T3 — Disc kernel LUT
- [x] M1.P1.T4 — Unit test suite for the math foundation

## Phase 1.2: Node skeleton + flatten path (first NDK compile gate)

> Complete. Full task briefs archived in [ARCHIVE/M1-history.md](../ARCHIVE/M1-history.md).

- [x] M1.P2.T1 — Iop skeleton, inputs, knobs
- [x] M1.P2.T2 — `_validate`/`_request`/`engine` flatten path + NDK compile gate

## Phase 1.3: Scatter core, buckets, holdout (serial band compute)

> Complete except M1.P3.T5's abort-recovery clause (its live brief is kept below). All other
> task briefs and this phase's design intro are archived in
> [ARCHIVE/M1-history.md](../ARCHIVE/M1-history.md); execution detail per task is in the
> archived `## Decisions` log there.

- [x] M1.P3.T0 — Adjudicate volumetric tidying against the deep spec (run FIRST in this phase)
- [x] M1.P3.T1 — `PodBuffer<T>`, `DEEPC_HD`, and SoA flattening
- [x] M1.P3.T2 — `scatterBandCPU`
- [x] M1.P3.T7 — Wire `DeepCDefocus` into `src/CMakeLists.txt` (run NEXT — every later verify depends on it)
- [x] M1.P3.T8 — Coverage-head flag for split volumetric parents
- [x] M1.P3.T9 — Fourth accumulation plane: co-located area (run BEFORE T5's bake-off)
- [x] M1.P3.T3 — Holdout SoA and per-pixel boundary-LUT
- [x] M1.P3.T10 — Decouple the holdout LUT's boundary set from the ΔCoC buckets (run BEFORE T4)
- [x] M1.P3.T11 — Both `interpAtBucket` variants for the opaque-step degeneracy (run BEFORE T4)
- [x] M1.P3.T4 — Unit tests for the scatter core (POD-level)
- [x] M1.P3.T6 — Make `tidyOverlapping()`'s split pass single-pass (run BEFORE T5)
- [x] M1.P3.T14 — Default `CMAKE_BUILD_TYPE` to Release (run FIRST — everything downstream measures it)
- [x] M1.P3.T13 — Same-pixel bucket collisions break the size-0 flatten (run BEFORE T12)
- [x] M1.P3.T15 — Per-bucket transmittance attenuation at the flatten (run BEFORE T12)
- [x] M1.P3.T12 — Headless validation harness + scenes (a)–(f)
- [x] M1.P3.T16 — Validation scenes (g)–(l) on T12's harness
- [x] M1.P3.T19 — Kernel-bin quantisation trough on small CoC (closes validation scene (l))
- [x] M1.P3.T17 — Decide the bucket composite, delete the loser (needs T12 + T16)
- [x] M1.P3.T20 — Rule on the α<1 receding-content residual (`g4`) — run BEFORE T18
- [x] M1.P3.T21 — Staggered multi-part parents read HIGH (M1.P3.T20's regression) — run BEFORE T18
- [x] M1.P3.T22 — Gate the unequal-density over-read (run FIRST — T23 cannot iterate without it)
- [x] M1.P3.T23 — Composite iteration against the unequal-density over-read (needs T22)
- [x] M1.P3.T24 — Gate and rule on the low-α ramp over-read (the target M1.P3.T23 never measured)
- [x] M1.P3.T18 — Decide the holdout interpolant, delete the losers (needs T12 + T16)

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

> The dated log through 2026-08-23 (every ruling, bake-off table, review amendment and
> retraction from Phases 1.0–1.3) is archived verbatim in
> [ARCHIVE/M1-history.md](../ARCHIVE/M1-history.md). Task briefs saying "this file's
> `## Decisions`" resolve there. Append NEW decisions below this note.

**Verification gate:** the Phase 1.0 local build (`-D Nuke_ROOT=/usr/local/Nuke17.0v3`) green
throughout, plus `./docker-build.sh --linux` (and once, at M1.P5.T1, `--windows`) both green
wherever docker is available before merge; all unit tests in `tests/test_defocus_math.cpp` and
`tests/test_defocus_scatter.cpp` pass; all validation scenes (a)–(l) in `tests/nuke/` pass in
Nuke; `-fopt-info-vec` confirms the scatter loop vectorized (or the omp-simd fallback does); the
2K/20spp perf profile is recorded. PR merges via `/address-pr-review --auto`.

## Status log

> The full "Where things stand" narrative through 2026-08-23 is archived verbatim in
> [ARCHIVE/M1-history.md](../ARCHIVE/M1-history.md). Kept live below: the current state, the
> standing lesson's operative rule, and the carried obligations.

**Current state (2026-09-05).** Phases 1.0–1.3 are COMPLETE (M1.P3.T18 closed 1.3 on
2026-08-23) except M1.P3.T5's abort-recovery clause, which needs an interactive Nuke pass
(headless cannot trigger a recoverable mid-cook cancel; harness check `e4` SKIPs on the same
blocker). Harness baseline: **PASS=86 FAIL=2 XFAIL=9 SKIP=1, exit 1 by design** (`f3e`/`f3f`
are deliberate plain FAILs gating the unequal-density over-read); both unit suites green.
Branch `claude/deep-defocus-node-plan-o0ld83` committed, NOT pushed; no PR yet. M1.P4.T1's
implementation (BandLedger, budget formula, band planner + unit suite) is in flight,
uncommitted in the working tree, building and green as of 2026-09-05.

**Standing lesson (recorded eight times over — full history in the archive):** every wrong
figure in this plan was a measurement that never reached the phenomenon it claimed to bound.
So: mutation-test every check (one nobody has made fail proves nothing), band pins rather
than bounding them on one side, validate re-pins against an independent oracle rather than
against the new output, and give every XFAIL a hard outer bound so it cannot swallow a later
regression.

Remaining in this milestone: Phase 1.4 (T1 concurrency, T2 vectorization/perf gate — plus the
`merge_tolerance` default review owed from T16), then Phase 1.5 (T1 CMake, T2 help/README — owed
the coverage-deficit spec and T24's low-α bound documentation, T3 scene scripts), then the
verification gate and PR. Twenty-one tasks
have now been added by execution findings (M1.P3.T0, T6, T7, T8, T9, T10, T11, T12, T13, T14, T15, T16,
T17, T18, T19, T20, T21, T22, T23, T24, plus the enlarged M1.P3.T4 test list).
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
α<1 receding-content residual is closed by M1.P3.T20** (fixed in the composite) **and M1.P3.T21**
(which fixed T20's own regression and improved it further; `g4` re-pinned at 0.0325 ± 0.004). They ran before T18 because the fixes moved every rendered pixel and T18 decides from
rendered pixels — **T18 must re-render rather than quoting any figure from before M1.P3.T21's
commit**.
And the milestone PR body must carry the shipped-node release note from
`PLAN/DECISIONS/2026-07-26-tidyoverlapping-single-pass.md` and
`PLAN/DECISIONS/2026-07-26-volumetric-tidying-semantics.md`.
