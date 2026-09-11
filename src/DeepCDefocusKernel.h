// SPDX-License-Identifier: MIT
//
// ============================================================================
//
//  DeepCDefocusKernel — Header-only disc-bokeh kernel LUT for DeepCDefocus
//
//    - KernelView    : a non-owning view of one radius's precomputed disc
//                      kernel, laid out as contiguous per-row weight spans so
//                      the scatter inner loop is a flat, auto-vectorizable
//                      `dst[i] += w[i]*c` over one row with no per-pixel disc
//                      test.
//    - KernelSampler : the abstract sampler seam. `destX/destY/depth/
//                      channelGroup` are unused by the disc implementation
//                      but stay in the signature, so a spatially-varying or
//                      chromatic kernel can be dropped in without touching
//                      any call site.
//    - DiscKernelLUT : the disc implementation. Radius-indexed on the global
//                      kernel-radius grid (hyperbolic below 16px, uniform
//                      0.5px above -- see the grid comment for why) over a
//                      measured radius RANGE [minRadius, maxRadius] (see the
//                      constructor comment for why it is a range and not
//                      [0, max_radius]), anti-aliased edge (edgeSoftness
//                      knob), per-entry exact normalization (sum(w) == 1),
//                      precomputed row spans, Y extent pre-scaled by pixel
//                      aspect (anamorphic -> ellipse).
//
//  Zero NDK/DDImage dependencies -- standard library only. Compiles with
//  plain `g++ -std=c++17`. Hot per-span accessors are marked DEEPC_HD so the
//  same source compiles under nvcc unchanged; only the loop driver and the
//  allocator would need swapping for a device build.
//
//  DEEPC_HD itself is owned by DeepCDefocusMath.h, which is why that header
//  is included below: defining the macro independently here would make its
//  expansion depend on include order in a .cu translation unit. No other
//  symbol from the math header is used.
//
// ============================================================================

#ifndef DEEPC_DEFOCUS_KERNEL_H
#define DEEPC_DEFOCUS_KERNEL_H

#include <cmath>
#include <cstddef>
#include <vector>

// Owns the guarded DEEPC_HD definition (a .cu TU pre-defines it as
// `__host__ __device__`; on the CPU it vanishes). Included for that macro
// alone, so the two headers can never disagree about what DEEPC_HD means.
#include "DeepCDefocusMath.h"

namespace deepc {

// ---------------------------------------------------------------------------
// RowSpan -- per-row metadata for one kernel entry's disc.
//
// [xStart, xEnd] is the inclusive pixel-offset range (relative to the
// kernel's center pixel) that the disc covers on this row; `weightOffset` is
// the offset into the owning KernelView::weights pointer where this row's
// (xEnd - xStart + 1) contiguous weights begin. An empty row (no coverage,
// e.g. the extreme top/bottom rows of a near-circular disc) is represented
// by xEnd < xStart; count()/empty() below are the hot per-span accessors
// used by the scatter inner loop, so they carry DEEPC_HD.
// ---------------------------------------------------------------------------
struct RowSpan {
    int xStart = 0;        // inclusive, pixel offset from kernel center
    int xEnd = -1;          // inclusive, pixel offset from kernel center
    int weightOffset = 0;   // offset from KernelView::weights for this row

    DEEPC_HD inline bool empty() const { return xEnd < xStart; }
    DEEPC_HD inline int count() const { return empty() ? 0 : (xEnd - xStart + 1); }
};

// ---------------------------------------------------------------------------
// KernelView -- non-owning view of one radius's disc kernel.
//
// `radiusX`/`radiusY` are the kernel's pixel half-extents (rows run
// y = -radiusY .. +radiusY, i.e. rowCount == 2*radiusY + 1); they differ
// under non-1.0 pixel aspect (see DiscKernelLUT). `weights` points at the
// entry's contiguous weight storage; `rowSpans` points at its rowCount
// RowSpan entries, row 0 corresponding to y == -radiusY.
//
// The half-extents are a safe OUTER bound, not the tight nonzero footprint:
// they are ceil()ed and include the anti-aliased edge band, so the outermost
// row or two can legitimately be empty and the outermost column of a span can
// legitimately weigh 0. Anything sizing a buffer or padding a bbox from them
// is therefore conservative in the safe direction; anything wanting the exact
// nonzero footprint must read the row spans. Note in particular that the
// extents cover `radius + edgeSoftness/2`, which is up to edgeSoftness/2 px
// WIDER than the node-level bbox pad of ceil(max_radius) / ceil(max_radius *
// aspect); check which of the two a caller actually needs before relying on
// either.
//
// LIFETIME: purely non-owning. The pointers alias the sampler's internal
// storage and stay valid only while that sampler is alive and unmodified;
// DiscKernelLUT never mutates its buffers after construction, so views taken
// from a live LUT are safe to hold and to share across render threads.
//
// Intended scatter inner loop (illustrative; not compiled in this header):
//
//   const KernelView kv = sampler.kernel(radiusPx, destX, destY, depth, grp);
//   for (int row = 0; row < kv.rowCount; ++row) {
//       const RowSpan& span = kv.row(row);
//       if (span.empty()) continue;
//       const int y = kv.rowY(row);
//       const float* __restrict w = kv.rowWeights(row);
//       float* __restrict dst = destRow(destY + y) + (destX + span.xStart);
//       const int n = span.count();
//       for (int i = 0; i < n; ++i)
//           dst[i] += w[i] * c;         // flat, contiguous, no disc test
//   }
// ---------------------------------------------------------------------------
struct KernelView {
    int radiusX = 0;
    int radiusY = 0;
    const float* weights = nullptr;
    const RowSpan* rowSpans = nullptr;
    int rowCount = 0;

    DEEPC_HD inline bool valid() const
    {
        return weights != nullptr && rowSpans != nullptr && rowCount > 0;
    }

    // Hot per-span accessors -- called once per row inside the scatter inner
    // loop, so they are DEEPC_HD inline for the CUDA seam.
    DEEPC_HD inline int rowY(int rowIndex) const { return rowIndex - radiusY; }
    DEEPC_HD inline const RowSpan& row(int rowIndex) const { return rowSpans[rowIndex]; }
    DEEPC_HD inline const float* rowWeights(int rowIndex) const
    {
        return weights + rowSpans[rowIndex].weightOffset;
    }
};

// ---------------------------------------------------------------------------
// KernelSampler -- the abstract sampler seam.
//
// `destX`, `destY`, `depth`, `channelGroup` are unused by the disc
// implementation; they exist so a spatially-varying or chromatic kernel can
// be dropped in without touching any call site. They are kept as real named
// parameters in the interface -- do not drop them from the signature.
// Implementations that ignore them should
// suppress the unused-parameter warning at the definition site (e.g.
// `(void)destX;`), never by removing the parameter from the interface.
// ---------------------------------------------------------------------------
class KernelSampler {
public:
    virtual ~KernelSampler() = default;

    virtual KernelView kernel(float radiusPx,
                               int destX,
                               int destY,
                               float depth,
                               int channelGroup) const = 0;
};

// ---------------------------------------------------------------------------
// discEdgeWeight -- anti-aliased disc edge ramp.
//
// A clamped LINEAR ramp (not smoothstep) in radial distance `r`, centered on
// `radius` with total band width `edgeSoftness`:
//   - r <= radius - edgeSoftness/2  -> 1.0 (fully inside)
//   - r >= radius + edgeSoftness/2  -> 0.0 (fully outside)
//   - otherwise                     -> linear interpolation across the band
// `edgeSoftness == 0` collapses inner==outer==radius, which the r<=inner /
// r>=outer branches already resolve to a hard-edged disc (r<=radius -> 1,
// else 0) without ever dividing by zero; the `band <= eps` branch is a
// defensive fallback for floating-point degeneracy of the same case.
// ---------------------------------------------------------------------------
DEEPC_HD inline float discEdgeWeight(float r, float radius, float edgeSoftness)
{
    const float half = edgeSoftness * 0.5f;
    const float innerR = radius - half > 0.0f ? radius - half : 0.0f;
    const float outerR = radius + half;

    if (r <= innerR)
        return 1.0f;
    if (r >= outerR)
        return 0.0f;

    const float band = outerR - innerR;
    if (band <= 1e-8f)
        return (r <= radius) ? 1.0f : 0.0f;

    return (outerR - r) / band;
}

// ---------------------------------------------------------------------------
// THE GLOBAL KERNEL-RADIUS GRID
//
// Every kernel entry sits on one global, frame-independent grid of radii, and
// both the LUT (radiusToIndex()) and the scatter's "would these two radii
// rasterise the same disc?" predicate (scatterKernelBin(), in
// DeepCDefocusScatter.h) read it from HERE so they cannot drift apart.
//
// WHY IT IS NOT A UNIFORM 0.5px GRID.  Quantising radius onto a uniform step
// h makes two adjacent scanlines that straddle a bin edge rasterise DIFFERENT
// discs, and a flat opaque surface then loses exactly
// `(S_r(0) - S_{r+h}(0))/2` of its alpha on the crossing row, where S_r(0) is
// the entry's centre-ROW weight sum. On a uniform 0.5px grid that is
// 2.0088e-01 at r=0.5 -- a one-scanline 20% dark line across an opaque
// surface, 51x the 1/255 visibility gate, measured in Nuke (validation scene
// (l)) and predicted from the LUT alone to six decimals.
//
// The deficit is `h * |S_r'(0)| / 2` and `S_r(0) ~ 2/(pi*r)`, so it is
// `~ h / (pi*r^2)`: a uniform step is wrong at BOTH ends -- ruinous at small
// radii, wasteful at large ones. Making the step `h(r) = c*r^2` instead makes
// the deficit UNIFORM at `c/pi` across the whole radius range, which is the
// only spacing law that buys a bound rather than a bound-at-one-radius.
//
// Integrating dr/di = c*r^2 gives r(i) = 1/(A - c*i) -- a hyperbolic grid,
// closed-form in both directions, so the lookup stays O(1) with no search and
// no per-fragment cost. With c chosen so the step reaches 0.5px at
// r = 16, every constant below falls out EXACTLY in binary (c = 1/512):
//
//   index 0          -> radius 0                     (the delta entry)
//   index 1..993     -> radius 512 / (1025 - index)  (0.5 .. 16, hyperbolic)
//   index >993       -> radius 16 + (index-993)*0.5  (uniform)
//
// Properties, all measured (see tests/test_defocus_scatter.cpp's
// "adjacent kernel bins never lose a visible amount of alpha"):
//   - worst adjacent-bin deficit over r in [0.5, 20]: 1.2566e-03 (at
//     r = 1.7415), against 2.0088e-01 on a uniform 0.5px grid -- a 160x
//     reduction, and 3.1x inside the 1/255 gate;
//   - r < 0.5 costs nothing to leave coarse: with edgeSoftness 1.0 every disc
//     of radius <= 0.5 IS the single-pixel delta (the nearest neighbours sit
//     at r = 1.0 >= radius + softness/2), so entry 0 and entry 1 are the same
//     kernel and no query below 0.5 can be wrong. The scatter never asks --
//     it takes the sharp path there -- but nothing depends on that here;
//   - the refinement is BOUNDED: it only exists below 16px, so it adds a
//     FIXED 197KB (993 small entries) no matter how large max_radius is.
//     Measured: 0.776MB at the [0, 40] range a frame typically measures,
//     8.606MB at [0, 100]; build time 0.891ms and 10.37ms respectively, once
//     per cook.
//
// The alternative -- interpolating between two adjacent 0.5px entries --
// loses on both accuracy and cost. Flat-field |a-1| on validation scene (l)'s
// three ramps (y / diagonal / radial): interpolation 3.354e-03 / 3.041e-03 /
// 1.087e-02, this grid 2.084e-03 / 1.819e-03 / 9.937e-03 -- and BOTH converge
// on the same floor, which is not the grid at all but the disc family's own C1
// kink at r=0.5 plus the CoC field's extremum. Interpolation also needs an
// O(kernel area) blend into per-thread scratch on EVERY fragment (+18.5% /
// +34.9% / +68.2% on the scatter's inner loop at r = 2 / 8 / 24px, 7 planes),
// and it would have to hand back a view of that scratch, breaking
// KernelView's "safe to hold and share across render threads" contract. The
// grid's own lookup costs 8.1ns per call against 3.3ns for a plain
// `lround(r/0.5)`, i.e. +4.8ns per fragment against the O(pi*r^2 * (C+3))
// FMAs that fragment then costs -- under 1% of a fragment at r >= 4px, and
// not visible end to end.
// ---------------------------------------------------------------------------

// Radius, in X pixels, at and above which the grid reverts to uniform 0.5px
// steps -- i.e. where `c*r^2` first reaches 0.5 with c = 1/512.
constexpr float kKernelCoarseFromPx = 16.0f;

// Last index of the hyperbolic (fine) region; index 0 is the radius-0 entry.
constexpr int kKernelFineLastIndex = 993;

// r = kKernelFineScale / (kKernelFineOrigin - index) over the fine region.
constexpr float kKernelFineScale  = 512.0f;
constexpr int   kKernelFineOrigin = 1025;

// Radius of grid node `index`. Exact for every index (the fine region's
// constants are powers of two), monotonically increasing, and the inverse of
// kernelGridIndex() on every node. Negative indices clamp to node 0.
DEEPC_HD inline float kernelGridRadius(int index)
{
    if (index <= 0)
        return 0.0f;
    if (index <= kKernelFineLastIndex)
        return kKernelFineScale / static_cast<float>(kKernelFineOrigin - index);
    return kKernelCoarseFromPx
         + static_cast<float>(index - kKernelFineLastIndex) * 0.5f;
}

// Nearest grid node to `radiusPx`, nearest IN RADIUS (the same rule a plain
// `lround(radius / 0.5)` on a uniform grid implements, so nothing downstream
// has to learn a new convention). Written so NaN takes the first branch (-> node
// 0) and +inf the +inf branch; the arithmetic below is therefore only ever
// reached with a finite, bounded value.
//
// The fine region's nodes are `kKernelFineScale / n` for integer
// n = kKernelFineOrigin - index, so "nearest in radius" is decided on n rather
// than on the index: r lies between nodes n = k and n = k+1 (radii 512/k and
// 512/(k+1)), whose midpoint in RADIUS is 512*(2k+1)/(2k(k+1)), i.e. the
// crossing is at u = 512/r == 2k(k+1)/(2k+1) -- the harmonic mean of k and
// k+1. One division, no search, no table.
DEEPC_HD inline int kernelGridIndex(float radiusPx)
{
    if (!(radiusPx > 0.25f))                    // NaN, negative, sub-quarter-px
        return 0;                               // (nearest of node 0 and node 1)
    if (!(radiusPx > 0.5f))
        return 1;
    if (!(radiusPx < kKernelCoarseFromPx)) {    // +inf lands here, then clamps
        if (!(radiusPx < 1.0e6f))
            return kKernelFineLastIndex + 2000000;
        return kKernelFineLastIndex
             + static_cast<int>(std::lround((radiusPx - kKernelCoarseFromPx) * 2.0f));
    }

    const double u = static_cast<double>(kKernelFineScale)
                   / static_cast<double>(radiusPx);
    long k = static_cast<long>(std::floor(u));
    // u is in (32, 1024) for radiusPx in (0.5, 16); clamp anyway so a rounding
    // wobble at either end cannot index off the grid.
    const long kMin = static_cast<long>(kKernelFineScale) / 16;      // 32
    const long kMax = static_cast<long>(kKernelFineOrigin) - 2;      // 1023
    if (k < kMin) k = kMin;
    if (k > kMax) k = kMax;

    const double thresh = 2.0 * static_cast<double>(k) * static_cast<double>(k + 1)
                        / (2.0 * static_cast<double>(k) + 1.0);
    const long n = (u <= thresh) ? k : (k + 1);
    return kKernelFineOrigin - static_cast<int>(n);
}

// The two grid nodes that bracket `radiusPx`, and the blend weight between
// them: a caller rasterises node A at (1 - frac) and node B at frac, which is
// continuous in radius and reproduces the node's own kernel exactly on a node.
// `frac` is stated on the DIAMETER, (d - dA)/(dB - dA); diameter is twice the
// radius, so the factors of two cancel and it is computed on radii.
//
// Derived FROM kernelGridIndex() rather than by re-inverting the grid, so the
// two can never drift: nearest-in-radius is at most one node from the floor,
// and the step back is decided by the same float comparison the caller makes.
// Every degenerate case -- exactly on a node, NaN, non-positive, or the
// saturation clamp kernelGridIndex() applies above 1e6 px -- returns
// indexB == indexA with frac 0, i.e. a single kernel and no second pass.
struct KernelGridBracket {
    int   indexA = 0;
    int   indexB = 0;
    float frac   = 0.0f;
};

DEEPC_HD inline KernelGridBracket kernelGridBracket(float radiusPx)
{
    KernelGridBracket b;
    if (!(radiusPx > 0.0f))
        return b;

    int i = kernelGridIndex(radiusPx);
    if (i < 0)
        i = 0;
    if (kernelGridRadius(i) > radiusPx && i > 0)
        --i;

    b.indexA = i;
    b.indexB = i;

    const float rA = kernelGridRadius(i);
    const float rB = kernelGridRadius(i + 1);
    if (!(radiusPx > rA) || !(radiusPx < rB))
        return b;

    b.indexB = i + 1;
    b.frac   = (radiusPx - rA) / (rB - rA);
    return b;
}

// ---------------------------------------------------------------------------
// DiscKernelLUT -- the disc KernelSampler implementation.
//
// Radius-indexed on the global kernel-radius grid above across
// [minRadius, maxRadius] (nearest-entry lookup, clamped at both ends -- see
// the constructor for why the LUT covers a measured range rather than
// [0, max_radius]). Entries sit on that global grid: the first is the largest
// grid node <= minRadius and the last the smallest grid node >= maxRadius, so
// the requested range is always fully covered, never clipped short.
//
// CALLER CONTRACT for the clamp: a query below minRadius silently returns the
// minRadius kernel, so a LUT built over a measured [2, 40] answers a 0.25px
// query with a 2px disc -- a visible error, not a rounding one. That is fine
// only because the caller both (a) measures the range from the frame's own CoC
// range, and (b) never reaches the sampler for radius < 0.5px -- the minimum
// kernel DIAMETER is 1px (`kSharpRadiusPx`), and everything at or below it is
// served by the sharp single-pixel path instead. A caller that cannot
// guarantee (b) must pass minRadius = 0. Above that floor a caller asks for
// two adjacent grid nodes (kernelGridBracket()) and blends them, so the
// radii reaching here are grid-node radii and the clamp is the only rounding
// left. The high-end clamp is benign by comparison: radii above
// maxRadius are already bounded by the `max_radius` knob before they get here.
//
// Each entry is an anti-aliased disc built via discEdgeWeight(),
// exactly normalized so sum(weights) == 1, with row spans computed against
// the elliptical (pixel-aspect-scaled) boundary. All entries share one flat
// std::vector<float> weight buffer and one flat std::vector<RowSpan> span
// buffer (per-entry offsets into each) so the whole LUT can be uploaded as a
// single contiguous device buffer.
//
// Pixel-aspect convention: Nuke's pixel aspect ratio (PAR) is pixel WIDTH /
// pixel HEIGHT. `radiusPx` (the argument to kernel()) is defined in X-pixel
// units (this is the space CoC math already works in: coc_px = coc_mm /
// filmbackWidth_mm * format.width()). A physically circular blur of
// physical radius R therefore spans R / pixelWidth pixels horizontally and
// R / pixelHeight pixels vertically. Since PAR = pixelWidth / pixelHeight,
// pixelHeight = pixelWidth / PAR, so:
//   verticalPixelSpan = R / pixelHeight = R / (pixelWidth / PAR)
//                      = (R / pixelWidth) * PAR = horizontalPixelSpan * PAR
// i.e. radiusY = radiusX * pixelAspect. When PAR > 1 (pixels physically
// wider than tall), a fixed physical circle needs FEWER horizontal pixels
// to cover the same physical width, but its horizontal pixel radius here is
// the independent variable (radiusX == radiusPx), so the scaling instead
// shows up as MORE vertical pixels for the same physical circle: radiusY
// grows with PAR.
//
// Two independent cross-checks on that direction:
//   - the node's output bbox pad is `ceil(max_radius)` in X and
//     `ceil(max_radius * aspect)` in Y -- same factor, same way up;
//   - DeepCDefocusMath.h's filmbackRadiusMm() uses
//     `mmPerPxY = mmPerPxX / pixelAspect`, i.e. it assumes exactly the same
//     pixel geometry (pixelHeight = pixelWidth / PAR). The two headers must
//     not disagree about this, and they do not.
// Correspondingly, a row's integer Y offset is converted back into X-pixel
// units by DIVIDING by the aspect (yEff = y / PAR) in buildEntry().
// ---------------------------------------------------------------------------
class DiscKernelLUT : public KernelSampler {
public:
    // The grid's COARSE step -- the spacing at and above kKernelCoarseFromPx,
    // and the widest step the grid ever takes. Below that radius the spacing
    // is the hyperbolic `c*r^2` law documented above, so this is an upper
    // bound on the step, not the step. Kept public (and kept at 0.5) because
    // callers budgeting worst-case entry counts read it.
    static constexpr float kStepPx = 0.5f;

    // Absolute safety caps on the constructor's float inputs. They are NOT
    // knob ranges -- they sit above every documented one (`max_radius` 1-500,
    // `edge_softness` 0-4, pixel aspect ~0.5-2) and exist only so a NaN, an
    // infinity or an uninitialised 1e30 cannot turn into an undefined
    // float->int conversion or an unbounded resize(). A sanitised garbage
    // input can therefore never cost more than the documented worst case.
    static constexpr float kMaxSupportedRadius = 512.0f;
    static constexpr float kMaxSupportedSoftness = 16.0f;
    static constexpr float kMinSupportedAspect = 0.125f;
    static constexpr float kMaxSupportedAspect = 8.0f;

    // Primary form -- build the LUT over a *measured* radius range.
    //
    // WHY a range instead of [0, max_radius]: a radius-indexed LUT at 0.5px
    // steps holds ~2*pi*R^3/3 floats in its coarse region, so it costs ~8.4MB
    // at R=100 but ~1.0GB at R=500 -- and 500 is exactly what the
    // `max_radius` knob permits. That knob is a *bound on the worst case*,
    // not an allocation request: a user who raises it defensively must not
    // pay a gigabyte. The
    // caller instead sizes the LUT from the frame's measured CoC range, which
    // the alpha-weighted depth-range pass already discovers once per cook
    // before any scatter runs, so only the radii the frame actually contains
    // get built. A realistic measured range such as [2, 40] costs 0.68MB
    // (measured). Building stays eager and lock-free. The grid's own
    // refinement below 16px is bounded and independent of this range: it adds
    // a fixed ~200KB, so the cubic term is entirely the measured range's.
    DiscKernelLUT(float minRadius, float maxRadius, float edgeSoftness, float pixelAspect)
        : _minRadius(sanitizeMinRadius(minRadius, maxRadius))
        , _maxRadius(sanitizeRadius(maxRadius))
        , _edgeSoftness(sanitizeSoftness(edgeSoftness))
        , _pixelAspect(sanitizeAspect(pixelAspect))
    {
        build();
    }

    // Convenience form -- minRadius = 0, i.e. the whole [0, maxRadius] range.
    // Kept so existing call sites (and callers that genuinely have no
    // measured lower bound) keep working unchanged.
    DiscKernelLUT(float maxRadius, float edgeSoftness, float pixelAspect)
        : DiscKernelLUT(0.0f, maxRadius, edgeSoftness, pixelAspect)
    {
    }

    KernelView kernel(float radiusPx,
                       int destX,
                       int destY,
                       float depth,
                       int channelGroup) const override
    {
        // destX/destY/depth/channelGroup are unused here -- see the
        // KernelSampler doc; they are kept as real parameters for the seam.
        (void)destX;
        (void)destY;
        (void)depth;
        (void)channelGroup;

        if (_entries.empty())
            return KernelView{};

        const Entry& e = _entries[static_cast<std::size_t>(radiusToIndex(radiusPx))];

        KernelView view;
        view.radiusX = e.radiusX;
        view.radiusY = e.radiusY;
        view.weights = _weights.data() + e.weightOffset;
        view.rowSpans = _rowSpans.data() + e.rowSpanOffset;
        view.rowCount = e.rowCount;
        return view;
    }

    // Sanitised copies of the constructor arguments (see the k*Supported*
    // caps): what the LUT was actually built for, not what was asked for.
    float minRadius() const { return _minRadius; }
    float maxRadius() const { return _maxRadius; }
    float edgeSoftness() const { return _edgeSoftness; }
    float pixelAspect() const { return _pixelAspect; }
    int entryCount() const { return static_cast<int>(_entries.size()); }

    // Radius, in X pixels, that entry `index` was built for. Entries are on
    // the global kernel-radius grid, so this is exact.
    float entryRadius(int index) const
    {
        return kernelGridRadius(_baseIndex + index);
    }

    // Total bytes owned by the LUT's flat storage (capacity, not just size --
    // this is the real allocation). Cost grows as ~R^3, so callers doing
    // memory budgeting must be able to query it: see the constructor comment
    // for why the range, and hence this number, is measured rather than
    // derived from the `max_radius` knob.
    std::size_t sizeBytes() const
    {
        return _weights.capacity() * sizeof(float)
             + _rowSpans.capacity() * sizeof(RowSpan)
             + _entries.capacity() * sizeof(Entry);
    }

private:
    struct Entry {
        int radiusX = 0;
        int radiusY = 0;
        int rowCount = 0;
        std::size_t weightOffset = 0;
        std::size_t weightCount = 0;
        std::size_t rowSpanOffset = 0;
    };

    // --- input sanitisation -------------------------------------------------
    // Every predicate is written so that NaN takes the "reject" branch: NaN
    // fails both `> lo` and `< hi`, and `!(x < hi)` is therefore true for it,
    // so the order below matters. Nothing here can return a non-finite value.

    static float sanitizeRadius(float r)
    {
        if (!(r > 0.0f))                    // NaN, -0, negative
            return 0.0f;
        if (!(r < kMaxSupportedRadius))     // +inf, absurdly large
            return kMaxSupportedRadius;
        return r;
    }

    static float sanitizeMinRadius(float minRadius, float maxRadius)
    {
        const float lo = sanitizeRadius(minRadius);
        const float hi = sanitizeRadius(maxRadius);
        return (lo < hi) ? lo : hi;         // also handles minRadius > maxRadius
    }

    static float sanitizeSoftness(float s)
    {
        if (!(s > 0.0f))                    // NaN, negative -> hard edge
            return 0.0f;
        if (!(s < kMaxSupportedSoftness))   // +inf, absurdly large
            return kMaxSupportedSoftness;
        return s;
    }

    static float sanitizeAspect(float a)
    {
        if (!(a > 0.0f))                    // NaN, 0, negative -> square pixels
            return 1.0f;
        if (a < kMinSupportedAspect)
            return kMinSupportedAspect;
        if (!(a < kMaxSupportedAspect))     // +inf, absurdly large
            return kMaxSupportedAspect;
        return a;
    }

    // Absorbs float round-off at exact radius/extent boundaries. Deliberately
    // one constant for both the half-extent ceil() and the squared-distance
    // span test, so the search bound can never be tighter than the test.
    static constexpr float kGeomEps = 1e-4f;

    // Footprint of one entry, derived from its radius alone. Shared by the
    // sizing pass and the fill pass so the two can never disagree about how
    // many weights an entry occupies.
    struct EntryGeometry {
        int   radiusX   = 0;
        int   radiusY   = 0;
        int   rowCount  = 1;
        float invAspect = 1.0f;
        float outerR2   = 0.0f;
    };

    EntryGeometry geometryFor(float radius) const
    {
        const float outerR = radius + _edgeSoftness * 0.5f;

        EntryGeometry g;
        // y (integer pixel offset) is converted to an X-pixel-equivalent
        // physical offset via division by pixel aspect -- see the class-level
        // comment for the full derivation.
        g.invAspect = 1.0f / _pixelAspect;
        g.outerR2   = outerR * outerR;
        g.radiusX   = static_cast<int>(std::ceil(outerR + kGeomEps));
        g.radiusY   = static_cast<int>(std::ceil(outerR * _pixelAspect + kGeomEps));
        if (g.radiusX < 0) g.radiusX = 0;
        if (g.radiusY < 0) g.radiusY = 0;
        g.rowCount = 2 * g.radiusY + 1;
        return g;
    }

    // Largest |x| offset the disc covers on row `y`, or -1 for an empty row
    // (no coverage at all, e.g. the extreme top/bottom rows). The span is
    // symmetric, so the row occupies 2*xMax + 1 weights.
    static int rowExtent(const EntryGeometry& g, int y)
    {
        const float yEff = static_cast<float>(y) * g.invAspect;
        const float rem2 = g.outerR2 - yEff * yEff;
        if (!(rem2 >= 0.0f))        // also rejects NaN
            return -1;

        // Squared-distance search (no sqrt) avoids floating-point precision
        // pitfalls right at the disc boundary. x^2 is monotone in x, so the
        // first failure ends the row.
        int xMax = -1;
        for (int x = 0; x <= g.radiusX; ++x) {
            const float x2 = static_cast<float>(x) * static_cast<float>(x);
            if (x2 <= rem2 + kGeomEps)
                xMax = x;
            else
                break;
        }
        return xMax;
    }

    // Allocate and fill every entry on the global kernel-radius grid covering
    // [_minRadius, _maxRadius]. Both bounds are sanitised finite values in
    // [0, kMaxSupportedRadius] with _minRadius <= _maxRadius, so the index
    // arithmetic below cannot overflow and always yields n >= 1.
    void build()
    {
        // The grid nodes bracketing the requested range from OUTSIDE, so the
        // range is always fully covered: kernelGridIndex() rounds to nearest,
        // so step one node back/forward when it rounded the wrong way.
        _baseIndex = kernelGridIndex(_minRadius);
        if (kernelGridRadius(_baseIndex) > _minRadius && _baseIndex > 0)
            --_baseIndex;
        int lastIndex = kernelGridIndex(_maxRadius);
        if (kernelGridRadius(lastIndex) < _maxRadius)
            ++lastIndex;
        if (lastIndex < _baseIndex)
            lastIndex = _baseIndex;
        const int n = lastIndex - _baseIndex + 1;

        // Sizing pass. Growing the flat buffers with bare push_back costs a
        // transient 2-3x the final footprint (old + new buffer live at once
        // during each reallocation) -- on a multi-hundred-megabyte LUT that
        // peak is the very cost the measured range exists to avoid, so
        // shrinking afterwards is not enough. The counting loop reuses geometryFor() /
        // rowExtent(), so the reserve is exact; were it ever not, the vectors
        // would still grow correctly and only the peak would regress.
        std::size_t totalWeights = 0;
        std::size_t totalRowSpans = 0;
        for (int i = 0; i < n; ++i) {
            const EntryGeometry g = geometryFor(entryRadius(i));
            totalRowSpans += static_cast<std::size_t>(g.rowCount);
            for (int rowI = 0; rowI < g.rowCount; ++rowI) {
                const int xMax = rowExtent(g, rowI - g.radiusY);
                if (xMax >= 0)
                    totalWeights += static_cast<std::size_t>(2 * xMax + 1);
            }
        }
        _weights.reserve(totalWeights);
        _rowSpans.reserve(totalRowSpans);

        _entries.resize(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i)
            buildEntry(static_cast<std::size_t>(i), entryRadius(i));

        // No-ops when the reserve above was exact (the normal case); kept so
        // sizeBytes() stays an accurate statement of what the LUT costs even
        // if the two passes ever drift apart.
        _weights.shrink_to_fit();
        _rowSpans.shrink_to_fit();
    }

    int radiusToIndex(float radiusPx) const
    {
        const int last = static_cast<int>(_entries.size()) - 1;
        if (last <= 0)
            return 0;

        // Both bounds tests are negated so that NaN takes the first branch
        // (-> first entry) and +inf the second (-> last entry); the grid
        // lookup is therefore only ever reached with a finite, in-range
        // value, and the result is clamped again regardless.
        if (!(radiusPx > entryRadius(0)))
            return 0;
        if (!(radiusPx < entryRadius(last)))
            return last;

        long idx = static_cast<long>(kernelGridIndex(radiusPx))
                 - static_cast<long>(_baseIndex);
        if (idx < 0) idx = 0;
        if (idx > last) idx = last;
        return static_cast<int>(idx);
    }

    void buildEntry(std::size_t index, float radius)
    {
        Entry& e = _entries[index];
        const EntryGeometry g = geometryFor(radius);

        e.radiusX = g.radiusX;
        e.radiusY = g.radiusY;
        e.rowCount = g.rowCount;
        e.rowSpanOffset = _rowSpans.size();
        e.weightOffset = _weights.size();

        for (int rowI = 0; rowI < e.rowCount; ++rowI) {
            const int y = rowI - g.radiusY;
            const float yEff = static_cast<float>(y) * g.invAspect;
            const float yEff2 = yEff * yEff;
            const int xMax = rowExtent(g, y);

            // Recorded before this row's weights are appended, and relative to
            // the entry's base, so it stays valid for KernelView::rowWeights()
            // (which adds it to the entry-relative `weights` pointer).
            RowSpan span;
            span.weightOffset = static_cast<int>(_weights.size() - e.weightOffset);

            if (xMax < 0) {
                span.xStart = 0;
                span.xEnd = -1; // empty row -- outside the disc entirely
            } else {
                span.xStart = -xMax;
                span.xEnd = xMax;
                for (int x = -xMax; x <= xMax; ++x) {
                    const float xf = static_cast<float>(x);
                    const float r = std::sqrt(xf * xf + yEff2);
                    _weights.push_back(discEdgeWeight(r, radius, _edgeSoftness));
                }
            }
            _rowSpans.push_back(span);
        }

        e.weightCount = _weights.size() - e.weightOffset;
        normalizeEntry(e);
    }

    // Per-entry exact normalization: divide by the entry's weight sum so
    // sum(w) == 1 for every radius (load-bearing for energy conservation).
    // "Exact" means the divisor is the entry's own summed weight (accumulated
    // in double), not an analytic disc area -- it is not a claim that the
    // float weights re-sum to a bit-exact 1.0. Rounding each scaled weight to
    // float leaves a residual; measured over every entry of a [0, 100] LUT the
    // worst |sum - 1| is ~5e-8, i.e. at float resolution.
    // Tests should assert that tolerance, not equality.
    //
    // The center pixel (x=0, y=0) is always inside the disc (outerR >= 0),
    // so `sum` is provably > 0 for every entry; the else-branch below is an
    // unreachable-in-practice defensive fallback that guarantees we never
    // divide by zero / propagate NaN even if that invariant were violated.
    void normalizeEntry(Entry& e)
    {
        double sum = 0.0;
        for (std::size_t i = 0; i < e.weightCount; ++i)
            sum += static_cast<double>(_weights[e.weightOffset + i]);

        if (sum > 1e-12) {
            const float invSum = static_cast<float>(1.0 / sum);
            for (std::size_t i = 0; i < e.weightCount; ++i)
                _weights[e.weightOffset + i] *= invSum;
            return;
        }

        for (std::size_t i = 0; i < e.weightCount; ++i)
            _weights[e.weightOffset + i] = 0.0f;

        const RowSpan& centerSpan = _rowSpans[e.rowSpanOffset + static_cast<std::size_t>(e.radiusY)];
        if (!centerSpan.empty()) {
            const int centerIdx = 0 - centerSpan.xStart;
            _weights[e.weightOffset + static_cast<std::size_t>(centerSpan.weightOffset) +
                      static_cast<std::size_t>(centerIdx)] = 1.0f;
        }
    }

    float _minRadius;
    float _maxRadius;
    float _edgeSoftness;
    float _pixelAspect;

    // Grid index of entry 0: entry i covers kernelGridRadius(_baseIndex + i).
    int _baseIndex = 0;

    std::vector<Entry> _entries;
    std::vector<float> _weights;
    std::vector<RowSpan> _rowSpans;
};

} // namespace deepc

#endif // DEEPC_DEFOCUS_KERNEL_H
