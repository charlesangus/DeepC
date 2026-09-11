// SPDX-License-Identifier: MIT
//
// ============================================================================
//
//  DeepCDefocusFill — the background fill's per-band surface map
//
//  NDK-free like DeepCDefocusScatter.h, and the same CUDA seam: per-pixel
//  bodies are DEEPC_HD, buildSurfaceMap() is the loop driver.
//
//  SampleView contract (an adapter over one deep pixel, cheap to copy):
//
//    int   count() const;
//    float zFront(int i) const;   float zBack(int i) const;
//    float alpha(int i) const;    float channel(int i, int c) const;
//
// ============================================================================

#ifndef DEEPC_DEFOCUS_FILL_H
#define DEEPC_DEFOCUS_FILL_H

#include <cmath>
#include <cstddef>
#include <limits>

#include "DeepCDefocusMath.h"
#include "DeepCDefocusScatter.h"

namespace deepc {

DEEPC_HD inline int fillReachPx(float fillSearchPx)
{
    if (!(fillSearchPx > 0.0f) || !std::isfinite(fillSearchPx))
        return 0;
    return static_cast<int>(std::ceil(fillSearchPx));
}

struct Surface {
    float zFront   = 0.0f;
    float zBack    = 0.0f;
    float alpha    = 0.0f;
    float radiusPx = 0.0f;
};

// ---------------------------------------------------------------------------
// stagedRadiusPx — the scatter radius flattenPixelToSoA() stages a sample at
//
// A volumetric span is staged as its bucket-split parts, and the radius the
// residual borrows is the LAST part's, after consecutive same-bucket parts
// have been folded together — so the radius depends on the buckets, not on
// the span's own midpoint.  Must stay step-for-step with the flatten's split
// loop or the map's radius drifts from residualRadiusPx.
// ---------------------------------------------------------------------------
DEEPC_HD inline float stagedRadiusPx(const FlattenParams& params,
                                     const DepthBuckets&  buckets,
                                     float zFront, float zBack, float alpha)
{
    if (!(zBack > zFront))
        return radiusPixels(params.coc, sampleMidDepth(zFront, zBack));

    SpanSplitPart parts[kMaxSpanSplitParts];
    const int nParts = splitSpanAtBoundaries(buckets, zFront, zBack, alpha,
                                             parts, kMaxSpanSplitParts);

    bool  have      = false;
    float runFront  = zFront;
    float runBack   = zBack;
    int   runBucket = 0;
    for (int p = 0; p < nParts; ++p) {
        const SpanSplitPart& part = parts[p];
        if (!(part.t > 0.0f))
            continue;
        const float partDepth  = sampleMidDepth(part.zFront, part.zBack);
        const int   partBucket = buckets.bucketOfContaining(partDepth).index;
        if (have && partBucket == runBucket) {
            runBack   = part.zBack;
            runBucket = buckets.bucketOfContaining(sampleMidDepth(runFront, runBack)).index;
            continue;
        }
        runFront  = part.zFront;
        runBack   = part.zBack;
        runBucket = partBucket;
        have      = true;
    }
    return radiusPixels(params.coc, sampleMidDepth(runFront, runBack));
}

// ---------------------------------------------------------------------------
// deepestSurface — the sample flattenPixelToSoA() would stage last
//
// Applies the flatten's own sanitising (depth, ray-distance scale, back-
// before-front, alpha clamp) and its alpha > 0 early-out, then keeps the
// greatest sanitised zBack, later index on ties.  Returns the winning sample
// index, or -1 for a pixel the flatten stages nothing from.  Channels are the
// caller's to copy from that index: they are not read here so a pixel of many
// samples pays one channel read, not one per sample.
//
// Reads the RAW stack, so it agrees with the flatten exactly when the stack is
// tidy-disjoint; tidyOverlapping() would first cut overlapping spans, and the
// cut pieces are not reconstructed here.
// ---------------------------------------------------------------------------
template <typename SampleView>
DEEPC_HD inline int deepestSurface(const FlattenParams& params,
                                   const DepthBuckets&  buckets,
                                   int x, int y,
                                   const SampleView& samples,
                                   Surface& out)
{
    const int   n        = samples.count();
    const float rayScale = rayDepthScaleAt(params, x, y);

    int   best      = -1;
    float bestFront = 0.0f;
    float bestBack  = 0.0f;
    float bestAlpha = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float zf = sanitizeFragmentDepth(samples.zFront(i)) * rayScale;
        float       zb = sanitizeFragmentDepth(samples.zBack(i)) * rayScale;
        if (!(zb > zf))
            zb = zf;
        const float a = clampf(samples.alpha(i), 0.0f, 1.0f);
        if (!(a > 0.0f))
            continue;
        if (best < 0 || zb >= bestBack) {
            best      = i;
            bestFront = zf;
            bestBack  = zb;
            bestAlpha = a;
        }
    }
    if (best < 0)
        return -1;

    out.zFront   = bestFront;
    out.zBack    = bestBack;
    out.alpha    = bestAlpha;
    out.radiusPx = stagedRadiusPx(params, buckets, bestFront, bestBack, bestAlpha);
    return best;
}

// ---------------------------------------------------------------------------
// SurfaceMap — one pixel's deepest staged surface, over the fetch window
// extended by the fill's search reach (band +/- (padY + reach) rows, clipped
// to the output box like ResidualWindow, never to srcBox).
//
// Planar: plane p of pixel i is planes[p * pixels() + i], p in
// {zFront, zBack, alpha, radiusPx, channel 0 .. C-1}.  The search reads zBack
// alone across many neighbours, so the depths are kept contiguous.
//
// An empty cell (no samples, none with alpha > 0, or never visited because it
// lies outside srcBox) has zFront == kEmpty; every other field of an empty
// cell is unspecified.
// ---------------------------------------------------------------------------
struct SurfaceMap {
    enum Plane : int {
        kZFront   = 0,
        kZBack    = 1,
        kAlpha    = 2,
        kRadius   = 3,
        kChannel0 = 4
    };

    static constexpr float kEmpty = -std::numeric_limits<float>::infinity();

    PodBuffer<float> planes;

    int x            = 0;
    int y            = 0;
    int width        = 0;
    int height       = 0;
    int channelCount = 0;

    std::ptrdiff_t pixels() const
    {
        return static_cast<std::ptrdiff_t>(width) * height;
    }

    int planeCount() const { return kChannel0 + channelCount; }

    void allocate(int xIn, int yIn, int widthIn, int heightIn, int channelCountIn)
    {
        x            = xIn;
        y            = yIn;
        width        = (widthIn > 0) ? widthIn : 0;
        height       = (heightIn > 0) ? heightIn : 0;
        channelCount = (channelCountIn > 0) ? channelCountIn : 0;

        const std::size_t n = static_cast<std::size_t>(pixels());
        planes.resizeUninitialized(n * static_cast<std::size_t>(planeCount()));
        float* zf = plane(kZFront);
        for (std::size_t i = 0; i < n; ++i)
            zf[i] = kEmpty;
    }

    // Size 0, capacity kept: a pooled BandJob reused by a foreground cook
    // must read as "no map" without freeing or allocating anything.
    void clear()
    {
        width  = 0;
        height = 0;
        planes.clear();
    }

    std::ptrdiff_t index(int px, int py) const
    {
        return static_cast<std::ptrdiff_t>(py - y) * width + (px - x);
    }

    bool contains(int px, int py) const
    {
        return px >= x && px < x + width && py >= y && py < y + height;
    }

    float*       plane(int p)       { return planes.data() + static_cast<std::ptrdiff_t>(p) * pixels(); }
    const float* plane(int p) const { return planes.data() + static_cast<std::ptrdiff_t>(p) * pixels(); }

    bool empty(std::ptrdiff_t i) const
    {
        return plane(kZFront)[i] == kEmpty;
    }

    template <typename SampleView>
    void setPixel(int px, int py, const Surface& s,
                  const SampleView& samples, int sampleIndex)
    {
        const std::ptrdiff_t i = index(px, py);
        plane(kZFront)[i] = s.zFront;
        plane(kZBack)[i]  = s.zBack;
        plane(kAlpha)[i]  = s.alpha;
        plane(kRadius)[i] = s.radiusPx;
        for (int c = 0; c < channelCount; ++c)
            plane(kChannel0 + c)[i] = samples.channel(sampleIndex, c);
    }

    static std::size_t bytesForWindow(int width, int height, int channelCount)
    {
        return surfaceMapBytesForWindow(width, height, channelCount);
    }
};

// ---------------------------------------------------------------------------
// buildSurfaceMap — the ONE place that sizes and fills a SurfaceMap, shared by
// production and the doctests, with buildResidualWindow()'s callback shape:
// `fetchRow(y)` once per visited row, `pixelSamples(x, y)` returning a
// SampleView once per visited column.  Rows visited are clipped to srcBox;
// the map's extent is clipped to the output box.
// ---------------------------------------------------------------------------
template <typename FetchRowFn, typename PixelSamplesFn>
bool buildSurfaceMap(SurfaceMap& map,
                     const FlattenParams& params,
                     const DepthBuckets&  buckets,
                     int outputBoxX0, int outputBoxX1,
                     int outputBoxY0, int outputBoxY1,
                     int srcBoxX0, int srcBoxX1,
                     int srcBoxY0, int srcBoxY1,
                     int bandY0, int bandY1, int padY, int reachPx,
                     int channelCount,
                     FetchRowFn&&     fetchRow,
                     PixelSamplesFn&& pixelSamples)
{
    const int extent = padY + reachPx;

    int wy0 = 0, wy1 = 0;
    residualWindowYRange(outputBoxY0, outputBoxY1, bandY0, bandY1, extent, wy0, wy1);
    map.allocate(outputBoxX0, wy0, outputBoxX1 - outputBoxX0, wy1 - wy0, channelCount);

    const int fy0raw = bandY0 - extent;
    const int fy1raw = bandY1 + extent;
    const int fy0 = (srcBoxY0 > fy0raw) ? srcBoxY0 : fy0raw;
    const int fy1 = (srcBoxY1 < fy1raw) ? srcBoxY1 : fy1raw;

    for (int py = fy0; py < fy1; ++py) {
        if (!fetchRow(py))
            return false;
        for (int px = srcBoxX0; px < srcBoxX1; ++px) {
            if (!map.contains(px, py))
                continue;
            const auto samples = pixelSamples(px, py);
            Surface s;
            const int deepest = deepestSurface(params, buckets, px, py, samples, s);
            if (deepest < 0)
                continue;
            map.setPixel(px, py, s, samples, deepest);
        }
    }
    return true;
}

} // namespace deepc

#endif // DEEPC_DEFOCUS_FILL_H
