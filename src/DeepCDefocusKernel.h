// SPDX-License-Identifier: MIT
//
// ============================================================================
//
//  DeepCDefocusKernel — Header-only disc-bokeh kernel LUT for DeepCDefocus
//
//  Provides the v2-ready kernel seam described in the M1 DeepCDefocus design
//  doc ("Kernel seam for v2"):
//
//    - KernelView    : a non-owning view of one radius's precomputed disc
//                      kernel, laid out as contiguous per-row weight spans so
//                      the scatter inner loop is a flat, auto-vectorizable
//                      `dst[i] += w[i]*c` over one row with no per-pixel disc
//                      test.
//    - KernelSampler : the abstract v2 seam. `destX/destY/depth/channelGroup`
//                      are deliberately unused in v1 -- a future milestone
//                      fills them in for spatially-varying / chromatic
//                      kernels. That unused-ness IS the seam.
//    - DiscKernelLUT : the v1 implementation. Radius-indexed at 0.5px steps
//                      over a measured radius RANGE [minRadius, maxRadius]
//                      (see the constructor comment for why it is a range and
//                      not [0, max_radius]), anti-aliased edge (edgeSoftness
//                      knob), per-entry exact normalization (sum(w) == 1),
//                      precomputed row spans, Y extent pre-scaled by pixel
//                      aspect (anamorphic -> ellipse).
//
//  Zero NDK/DDImage dependencies -- standard library only. Compiles with
//  plain `g++ -std=c++17`. Per the CUDA seam (M3), hot per-span accessors are
//  marked DEEPC_HD so the same source can later compile under nvcc without
//  change; only the loop driver + allocator get swapped in that milestone.
//
//  DEEPC_HD itself is owned by DeepCDefocusMath.h (milestone Decisions log,
//  2026-07-26), which is why that header is included below: defining the
//  macro independently here would make its expansion depend on include order
//  in a .cu translation unit. No other symbol from the math header is used.
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
// aspect) -- see the milestone's bbox-pad task before relying on either.
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
    // loop, so they are DEEPC_HD inline for the M3 CUDA seam.
    DEEPC_HD inline int rowY(int rowIndex) const { return rowIndex - radiusY; }
    DEEPC_HD inline const RowSpan& row(int rowIndex) const { return rowSpans[rowIndex]; }
    DEEPC_HD inline const float* rowWeights(int rowIndex) const
    {
        return weights + rowSpans[rowIndex].weightOffset;
    }
};

// ---------------------------------------------------------------------------
// KernelSampler -- abstract v2 seam.
//
// `destX`, `destY`, `depth`, `channelGroup` are deliberately unused in v1;
// a future milestone fills them in for spatially-varying / chromatic
// kernels. They are kept as real named parameters in the interface -- do not
// drop them from the signature. Implementations that ignore them should
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
// DiscKernelLUT -- v1 KernelSampler implementation.
//
// Radius-indexed at 0.5px steps across [minRadius, maxRadius] (nearest-entry
// lookup, clamped at both ends -- see the constructor for why the LUT covers
// a measured range rather than [0, max_radius]). Entries sit on the global
// 0.5px grid: the first is the largest 0.5px multiple <= minRadius and the
// last the smallest 0.5px multiple >= maxRadius, so the requested range is
// always fully covered, never clipped short.
//
// CALLER CONTRACT for the clamp: a query below minRadius silently returns the
// minRadius kernel, so a LUT built over a measured [2, 40] answers a 0.25px
// query with a 2px disc -- a visible error, not a rounding one. That is fine
// only because the caller both (a) measures the range from the frame's own CoC
// range, and (b) never reaches the sampler for radius < 0.5px, which takes the
// sharp fast path instead. A caller that cannot guarantee (b) must pass
// minRadius = 0. The high-end clamp is benign by comparison: radii above
// maxRadius are already bounded by the `max_radius` knob before they get here.
//
// Each entry is an anti-aliased disc built via discEdgeWeight(),
// exactly normalized so sum(weights) == 1, with row spans computed against
// the elliptical (pixel-aspect-scaled) boundary. All entries share one flat
// std::vector<float> weight buffer and one flat std::vector<RowSpan> span
// buffer (per-entry offsets into each) so a later CUDA milestone can upload
// the whole LUT as a single contiguous device buffer.
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
//   - the milestone's node bbox pad is `ceil(max_radius)` in X and
//     `ceil(max_radius * aspect)` in Y (M1.P2.T2) -- same factor, same way up;
//   - DeepCDefocusMath.h's filmbackRadiusMm() uses
//     `mmPerPxY = mmPerPxX / pixelAspect`, i.e. it assumes exactly the same
//     pixel geometry (pixelHeight = pixelWidth / PAR). The two headers must
//     not disagree about this, and they do not.
// Correspondingly, a row's integer Y offset is converted back into X-pixel
// units by DIVIDING by the aspect (yEff = y / PAR) in buildEntry().
// ---------------------------------------------------------------------------
class DiscKernelLUT : public KernelSampler {
public:
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
    // steps holds ~2*pi*R^3/3 floats in total, so it costs ~8.4MB at R=100
    // but ~1.0GB at R=500 -- and 500 is exactly what the `max_radius` knob
    // permits. That knob is a *bound on the worst case*, not an allocation
    // request: a user who raises it defensively must not pay a gigabyte. The
    // caller instead sizes the LUT from the frame's measured CoC range, which
    // the alpha-weighted depth-range pass already discovers once per cook
    // before any scatter runs (milestone Decisions, 2026-07-26), so only the
    // radii the frame actually contains get built. A realistic measured range
    // such as [2, 40] costs well under a megabyte. Building stays eager and
    // lock-free, and the 0.5px quantisation is unchanged.
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
        // v1: destX/destY/depth/channelGroup are unused -- see KernelSampler
        // doc. Kept as real parameters for the M2 seam.
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
    // the global 0.5px grid, so this is exact.
    float entryRadius(int index) const
    {
        return static_cast<float>(_baseIndex + index) * kStepPx;
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

    // Allocate and fill every entry on the 0.5px grid covering
    // [_minRadius, _maxRadius]. Both bounds are sanitised finite values in
    // [0, kMaxSupportedRadius] with _minRadius <= _maxRadius, so the index
    // arithmetic below cannot overflow and always yields n >= 1.
    void build()
    {
        _baseIndex = static_cast<int>(std::floor(_minRadius / kStepPx));
        const int lastIndex = static_cast<int>(std::ceil(_maxRadius / kStepPx));
        const int n = lastIndex - _baseIndex + 1;

        // Sizing pass. Growing the flat buffers with bare push_back costs a
        // transient 2-3x the final footprint (old + new buffer live at once
        // during each reallocation) -- on the multi-hundred-megabyte LUTs this
        // class exists to keep honest, that transient peak is exactly the cost
        // the measured-range decision was taken to avoid, so it is not enough
        // to shrink afterwards. The counting loop reuses geometryFor() /
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
        // (-> first entry) and +inf the second (-> last entry); std::lround
        // is therefore only ever reached with a finite, in-range value, and
        // the result is clamped again regardless.
        if (!(radiusPx > entryRadius(0)))
            return 0;
        if (!(radiusPx < entryRadius(last)))
            return last;

        long idx = std::lround(radiusPx / kStepPx) - static_cast<long>(_baseIndex);
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
    // float leaves a residual; measured over every entry of a [0, 100] LUT at
    // 0.5px steps the worst |sum - 1| is ~5e-8, i.e. at float resolution.
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

    // Grid index of entry 0: entry i covers radius (_baseIndex + i) * kStepPx.
    int _baseIndex = 0;

    std::vector<Entry> _entries;
    std::vector<float> _weights;
    std::vector<RowSpan> _rowSpans;
};

} // namespace deepc

#endif // DEEPC_DEFOCUS_KERNEL_H
