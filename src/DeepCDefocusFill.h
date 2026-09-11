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
#include <cstdint>
#include <limits>
#include <vector>

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

// ---------------------------------------------------------------------------
// The background predicate: Q's surface may stand in behind P's iff
//
//   zFront_Q > zBack_P + max(kFillDepthTol * zBack_P,
//                            kFillSlope * step_P * d(P, Q))
//
// The relative term rejects coplanar noise.  The slope term is what stops a
// receding CONTINUOUS surface borrowing from a deeper part of itself: with a
// relative tolerance alone an alpha-0.9 ramp over nothing would fill from its
// own far rows and read 0.99.  step_P is P's own local depth step (see
// localDepthStep), so a surface never qualifies as its own background, while
// a real background behind a flat card (no local step) qualifies at the
// first term.
//
// kFillSlope 2 is not enough: a ground plane's depth is convex in y, and over
// a 100 px reach its far rows outrun a 2x-step line.  Larger than 4 only
// trades away fill behind tilted surfaces.
// ---------------------------------------------------------------------------
constexpr float kFillDepthTol = 0.02f;
constexpr float kFillSlope    = 4.0f;

struct FillPredicate {
    float depthTol = kFillDepthTol;
    float slope    = kFillSlope;
};

// Monotone non-decreasing in distancePx, which is what lets the pyramid
// evaluate it at a tile's nearest point and prune conservatively.
DEEPC_HD inline float fillDepthThreshold(const FillPredicate& pred,
                                         float zBackP, float stepP, float distancePx)
{
    const float relative = pred.depthTol * zBackP;
    const float sloped   = pred.slope * stepP * distancePx;
    return zBackP + ((relative > sloped) ? relative : sloped);
}

DEEPC_HD inline bool qualifiesAsBackground(const FillPredicate& pred,
                                           float zBackP, float stepP, float distancePx,
                                           float zFrontQ)
{
    return zFrontQ > fillDepthThreshold(pred, zBackP, stepP, distancePx);
}

DEEPC_HD inline bool neighbourZBack(const SurfaceMap& map, int nx, int ny, float& zBack)
{
    if (!map.contains(nx, ny))
        return false;
    const std::ptrdiff_t i = map.index(nx, ny);
    if (map.empty(i))
        return false;
    zBack = map.plane(SurfaceMap::kZBack)[i];
    return true;
}

// The SMALLER of the two opposite-neighbour steps per axis, so a silhouette
// pixel reads its own surface's step rather than the jump to whatever lies
// beside it; a missing or empty neighbour is no information, not a step.
DEEPC_HD inline float axisDepthStep(float zP, bool haveLo, float zLo, bool haveHi, float zHi)
{
    const float stepLo = haveLo ? std::fabs(zP - zLo) : 0.0f;
    const float stepHi = haveHi ? std::fabs(zP - zHi) : 0.0f;
    if (haveLo && haveHi)
        return (stepLo < stepHi) ? stepLo : stepHi;
    return haveLo ? stepLo : stepHi;
}

DEEPC_HD inline float localDepthStep(const SurfaceMap& map, int x, int y)
{
    const float zP = map.plane(SurfaceMap::kZBack)[map.index(x, y)];
    float zL = 0.0f, zR = 0.0f, zD = 0.0f, zU = 0.0f;
    const bool haveL = neighbourZBack(map, x - 1, y, zL);
    const bool haveR = neighbourZBack(map, x + 1, y, zR);
    const bool haveD = neighbourZBack(map, x, y - 1, zD);
    const bool haveU = neighbourZBack(map, x, y + 1, zU);
    const float gx = axisDepthStep(zP, haveL, zL, haveR, zR);
    const float gy = axisDepthStep(zP, haveD, zD, haveU, zU);
    return (gx > gy) ? gx : gy;
}

// ---------------------------------------------------------------------------
// pruneSynthesis — the post-check on a found source.
//
// residualTP is P's residual transmittance as the flatten reports it (for a
// single-surface P that is 1 - alpha_P); the caller passes it because the map
// holds only the deepest surface's alpha.  An opaque P whose background's disc
// is at least as wide as its own gets that background from outside the
// silhouette anyway, so synthesising it would double-count; a semi-
// transparent P always synthesises.
// ---------------------------------------------------------------------------
DEEPC_HD inline bool pruneSynthesis(float residualTP, float radiusPxP, float radiusPxQ)
{
    return residualTP <= kFillDeficitTol && radiusPxQ >= radiusPxP;
}

// ---------------------------------------------------------------------------
// MaxDepthPyramid — max zFront over 4x4 tiles, level on level, above a
// SurfaceMap.  Level 0 is the map's own zFront plane and is not stored; level
// l holds one float per 4^l x 4^l block of map pixels, up to the root level
// whose single tile covers the whole map.  kEmpty is -inf, so an empty cell
// never lifts a tile's max and an all-empty tile stays -inf.
// ---------------------------------------------------------------------------
struct MaxDepthPyramid {
    static constexpr int kTileShift = kMaxDepthPyramidTileShift;
    static constexpr int kTile      = 1 << kTileShift;
    static constexpr int kMaxLevels = kMaxDepthPyramidMaxLevels;

    PodBuffer<float> _tiles;
    std::ptrdiff_t   _offset[kMaxLevels + 1] = {};
    int              _width[kMaxLevels + 1]  = {};
    int              _height[kMaxLevels + 1] = {};
    int              _levelCount = 0;

    int levelCount() const { return _levelCount; }
    int width(int level) const  { return _width[level]; }
    int height(int level) const { return _height[level]; }

    const float* level(int l) const { return _tiles.data() + _offset[l]; }
    float*       level(int l)       { return _tiles.data() + _offset[l]; }

    void clear()
    {
        _levelCount = 0;
        _tiles.clear();
    }

    static int levelsForWindow(int width, int height)
    {
        return maxDepthPyramidLevels(width, height);
    }

    static std::size_t bytesForWindow(int width, int height)
    {
        return maxDepthPyramidBytesForWindow(width, height);
    }

    void build(const SurfaceMap& map)
    {
        _levelCount = levelsForWindow(map.width, map.height);
        _width[0]   = map.width;
        _height[0]  = map.height;
        _offset[0]  = 0;

        std::size_t total = 0;
        for (int l = 1; l <= _levelCount; ++l) {
            _width[l]  = (_width[l - 1] + kTile - 1) / kTile;
            _height[l] = (_height[l - 1] + kTile - 1) / kTile;
            _offset[l] = static_cast<std::ptrdiff_t>(total);
            total += static_cast<std::size_t>(_width[l]) * static_cast<std::size_t>(_height[l]);
        }
        _tiles.resizeUninitialized(total);

        for (int l = 1; l <= _levelCount; ++l) {
            const float* src  = (l == 1) ? map.plane(SurfaceMap::kZFront) : level(l - 1);
            const int    srcW = _width[l - 1];
            const int    srcH = _height[l - 1];
            float*       dst  = level(l);
            const int    dstW = _width[l];
            const int    dstH = _height[l];
            for (int ty = 0; ty < dstH; ++ty) {
                const int y0 = ty * kTile;
                const int y1 = (y0 + kTile < srcH) ? y0 + kTile : srcH;
                for (int tx = 0; tx < dstW; ++tx) {
                    const int x0 = tx * kTile;
                    const int x1 = (x0 + kTile < srcW) ? x0 + kTile : srcW;
                    float m = SurfaceMap::kEmpty;
                    for (int sy = y0; sy < y1; ++sy) {
                        const float* row = src + static_cast<std::ptrdiff_t>(sy) * srcW;
                        for (int sx = x0; sx < x1; ++sx)
                            m = (row[sx] > m) ? row[sx] : m;
                    }
                    dst[static_cast<std::ptrdiff_t>(ty) * dstW + tx] = m;
                }
            }
        }
    }
};

struct FillSearchStats {
    int tilesVisited  = 0;
    int leavesVisited = 0;
};

struct BackgroundSource {
    bool  found    = false;
    int   qx       = 0;
    int   qy       = 0;
    float distance = 0.0f;
};

struct FillTileRef {
    int level;
    int tx;
    int ty;
};

DEEPC_HD inline std::int64_t tileMinDistanceSq(int px, int py,
                                               int x0, int x1, int y0, int y1)
{
    const int dx = (px < x0) ? (x0 - px) : ((px >= x1) ? (px - (x1 - 1)) : 0);
    const int dy = (py < y0) ? (y0 - py) : ((py >= y1) ? (py - (y1 - 1)) : 0);
    return static_cast<std::int64_t>(dx) * dx + static_cast<std::int64_t>(dy) * dy;
}

// Ties in distance resolve to the lowest y, then the lowest x, whatever
// order the tiles are walked in.
DEEPC_HD inline bool precedesInScan(std::int64_t d2, int y, int x,
                                    std::int64_t bestD2, int bestY, int bestX)
{
    if (d2 != bestD2)
        return d2 < bestD2;
    if (y != bestY)
        return y < bestY;
    return x < bestX;
}

// ---------------------------------------------------------------------------
// nearestBackground — the nearest qualifying Q within reachPx of P, by
// branch-and-bound over the pyramid: a tile is skipped when its max zFront
// cannot beat the threshold at the tile's nearest point, or when that point
// is already farther than the best found.  Explicit stack, children ordered
// nearest-first, no allocation.  P is (px, py) in map-local coordinates.
// ---------------------------------------------------------------------------
DEEPC_HD inline BackgroundSource nearestBackground(const SurfaceMap&      map,
                                                   const MaxDepthPyramid& pyramid,
                                                   const FillPredicate&   pred,
                                                   int px, int py,
                                                   float zBackP, float stepP,
                                                   int reachPx,
                                                   FillSearchStats* stats)
{
    BackgroundSource out;
    const int levels = pyramid.levelCount();
    if (levels <= 0 || reachPx < 0)
        return out;

    const std::int64_t reach2 = static_cast<std::int64_t>(reachPx) * reachPx;
    std::int64_t bestD2 = reach2 + 1;
    int bestX = 0, bestY = 0;

    const float* zFront = map.plane(SurfaceMap::kZFront);

    FillTileRef stack[MaxDepthPyramid::kMaxLevels * MaxDepthPyramid::kTile * MaxDepthPyramid::kTile + 1];
    int sp = 0;
    stack[sp++] = FillTileRef{levels, 0, 0};

    while (sp > 0) {
        const FillTileRef t = stack[--sp];
        const int shift = MaxDepthPyramid::kTileShift * t.level;
        const int x0 = t.tx << shift;
        const int y0 = t.ty << shift;
        const int x1 = ((x0 + (1 << shift)) < map.width)  ? (x0 + (1 << shift)) : map.width;
        const int y1 = ((y0 + (1 << shift)) < map.height) ? (y0 + (1 << shift)) : map.height;

        const std::int64_t d2 = tileMinDistanceSq(px, py, x0, x1, y0, y1);
        if (d2 > reach2 || d2 > bestD2)
            continue;
        if (stats)
            ++stats->tilesVisited;

        const float tileMax = pyramid.level(t.level)[static_cast<std::ptrdiff_t>(t.ty) * pyramid.width(t.level) + t.tx];
        if (!qualifiesAsBackground(pred, zBackP, stepP, std::sqrt(static_cast<float>(d2)), tileMax))
            continue;

        if (t.level == 1) {
            for (int y = y0; y < y1; ++y) {
                const float* row = zFront + static_cast<std::ptrdiff_t>(y) * map.width;
                for (int x = x0; x < x1; ++x) {
                    if (stats)
                        ++stats->leavesVisited;
                    const std::int64_t dx = x - px;
                    const std::int64_t dy = y - py;
                    const std::int64_t q2 = dx * dx + dy * dy;
                    if (q2 > reach2 || !precedesInScan(q2, y, x, bestD2, bestY, bestX))
                        continue;
                    if (!qualifiesAsBackground(pred, zBackP, stepP, std::sqrt(static_cast<float>(q2)), row[x]))
                        continue;
                    bestD2 = q2;
                    bestX  = x;
                    bestY  = y;
                }
            }
            continue;
        }

        // Children go on the stack farthest-first so the nearest pops first
        // and the distance bound tightens before the far ones are examined;
        // the exact tie rule makes the answer independent of this order.
        const int childLevel = t.level - 1;
        const int childShift = MaxDepthPyramid::kTileShift * childLevel;
        const int childW = pyramid.width(childLevel);
        const int childH = pyramid.height(childLevel);
        FillTileRef  child[MaxDepthPyramid::kTile * MaxDepthPyramid::kTile];
        std::int64_t childD2[MaxDepthPyramid::kTile * MaxDepthPyramid::kTile];
        int n = 0;
        for (int j = 0; j < MaxDepthPyramid::kTile; ++j) {
            const int cy = (t.ty << MaxDepthPyramid::kTileShift) + j;
            if (cy >= childH)
                break;
            for (int i = 0; i < MaxDepthPyramid::kTile; ++i) {
                const int cx = (t.tx << MaxDepthPyramid::kTileShift) + i;
                if (cx >= childW)
                    break;
                const int cx0 = cx << childShift;
                const int cy0 = cy << childShift;
                const int cx1 = ((cx0 + (1 << childShift)) < map.width)  ? (cx0 + (1 << childShift)) : map.width;
                const int cy1 = ((cy0 + (1 << childShift)) < map.height) ? (cy0 + (1 << childShift)) : map.height;
                const std::int64_t cd2 = tileMinDistanceSq(px, py, cx0, cx1, cy0, cy1);
                int k = n++;
                while (k > 0 && childD2[k - 1] < cd2) {
                    child[k]   = child[k - 1];
                    childD2[k] = childD2[k - 1];
                    --k;
                }
                child[k]   = FillTileRef{childLevel, cx, cy};
                childD2[k] = cd2;
            }
        }
        for (int k = 0; k < n; ++k)
            stack[sp++] = child[k];
    }

    if (bestD2 > reach2)
        return out;
    out.found    = true;
    out.qx       = map.x + bestX;
    out.qy       = map.y + bestY;
    out.distance = std::sqrt(static_cast<float>(bestD2));
    return out;
}

// ---------------------------------------------------------------------------
// findBackgroundSource — the two-tier query for absolute pixel (x, y): the
// nearest qualifying source within primaryReachPx, else within
// fallbackReachPx, else none.  Both reaches must not exceed the extension the
// map was built with, so every query sees its whole disc whatever band the
// map was windowed for.
// ---------------------------------------------------------------------------
DEEPC_HD inline BackgroundSource findBackgroundSource(const SurfaceMap&      map,
                                                      const MaxDepthPyramid& pyramid,
                                                      int x, int y,
                                                      int primaryReachPx,
                                                      int fallbackReachPx,
                                                      const FillPredicate& pred = FillPredicate(),
                                                      FillSearchStats*     stats = nullptr)
{
    if (!map.contains(x, y) || map.empty(map.index(x, y)))
        return BackgroundSource();

    const float zBackP = map.plane(SurfaceMap::kZBack)[map.index(x, y)];
    const float stepP  = localDepthStep(map, x, y);
    const int   px     = x - map.x;
    const int   py     = y - map.y;

    BackgroundSource r = nearestBackground(map, pyramid, pred, px, py, zBackP, stepP,
                                           primaryReachPx, stats);
    if (!r.found && fallbackReachPx > primaryReachPx)
        r = nearestBackground(map, pyramid, pred, px, py, zBackP, stepP,
                              fallbackReachPx, stats);
    return r;
}

// ---------------------------------------------------------------------------
// residualTransmittance — the pixel's virtual-background claim BEFORE any
// synthesis, from its raw samples: the product of (1 - alpha) over the
// samples the flatten stages, with its alpha sanitising.  The flatten's own
// residualT is the same product taken over the tidied, bucket-split parts,
// which agrees with this to rounding (the split's parts multiply back to
// their parent's transmittance); it is only compared against
// kFillDeficitTol, so a flatten is not spent on it.
// ---------------------------------------------------------------------------
inline float residualTransmittance(const std::vector<SampleRecord>& samples)
{
    float t = 1.0f;
    for (const SampleRecord& s : samples) {
        const float a = clampf(s.alpha, 0.0f, 1.0f);
        if (a > 0.0f)
            t *= (1.0f - a);
    }
    return t;
}

// ---------------------------------------------------------------------------
// synthesizeHiddenSample — Q's surface as a raw SampleRecord for pixel P
//
// The map holds sanitised CAMERA-SPACE depths; flattenPixelToSoA() will
// multiply P's samples by P's own ray-distance factor, so the depths are
// written pre-divided by it and land back on Q's camera depth (bit-exact at
// factor 1, within 1 ulp otherwise).  Channels are the map's, premultiplied,
// in the SoA's channel order.
// ---------------------------------------------------------------------------
inline void synthesizeHiddenSample(const SurfaceMap& map, int qx, int qy,
                                   float rayScaleP, SampleRecord& out)
{
    const std::ptrdiff_t q = map.index(qx, qy);
    out.zFront = map.plane(SurfaceMap::kZFront)[q] / rayScaleP;
    out.zBack  = map.plane(SurfaceMap::kZBack)[q]  / rayScaleP;
    out.alpha  = map.plane(SurfaceMap::kAlpha)[q];
    out.channels.resize(static_cast<std::size_t>(map.channelCount));
    for (int c = 0; c < map.channelCount; ++c)
        out.channels[static_cast<std::size_t>(c)] = map.plane(SurfaceMap::kChannel0 + c)[q];
}

// ---------------------------------------------------------------------------
// averageBackground — the smear estimator: Q's alpha/channels replaced by the
// mean over every qualifying pixel on a stride-capped lattice within reachPx.
// Depth and radius stay the nearest Q's.  The stride caps the reads per pixel
// at kFillAverageBudget whatever the reach.
// ---------------------------------------------------------------------------
constexpr int kFillAverageBudget = 800;

DEEPC_HD inline int fillAverageStride(int reachPx)
{
    const float area   = 3.14159265f * static_cast<float>(reachPx) * static_cast<float>(reachPx);
    const int   stride = static_cast<int>(std::ceil(std::sqrt(area / static_cast<float>(kFillAverageBudget))));
    return (stride > 1) ? stride : 1;
}

inline bool averageBackground(const SurfaceMap& map, const FillPredicate& pred,
                              int x, int y, float zBackP, float stepP, int reachPx,
                              SampleRecord& out)
{
    const int          stride = fillAverageStride(reachPx);
    const int          steps  = reachPx / stride;
    const std::int64_t reach2 = static_cast<std::int64_t>(reachPx) * reachPx;
    const int          C      = map.channelCount;

    // Accumulated in double so a uniform disc's mean is bit-exact to its value.
    std::vector<double> channelSum(static_cast<std::size_t>(C), 0.0);
    double alphaSum = 0.0;
    int    n        = 0;
    for (int j = -steps; j <= steps; ++j) {
        const int dy = j * stride;
        const int ny = y + dy;
        for (int i = -steps; i <= steps; ++i) {
            const int dx = i * stride;
            const int nx = x + dx;
            const std::int64_t d2 = static_cast<std::int64_t>(dx) * dx + static_cast<std::int64_t>(dy) * dy;
            if (d2 > reach2 || !map.contains(nx, ny))
                continue;
            const std::ptrdiff_t q = map.index(nx, ny);
            if (map.empty(q))
                continue;
            if (!qualifiesAsBackground(pred, zBackP, stepP, std::sqrt(static_cast<float>(d2)),
                                       map.plane(SurfaceMap::kZFront)[q]))
                continue;
            alphaSum += map.plane(SurfaceMap::kAlpha)[q];
            for (int c = 0; c < C; ++c)
                channelSum[static_cast<std::size_t>(c)] += map.plane(SurfaceMap::kChannel0 + c)[q];
            ++n;
        }
    }
    if (n == 0)
        return false;
    out.alpha = static_cast<float>(alphaSum / n);
    for (int c = 0; c < C; ++c)
        out.channels[static_cast<std::size_t>(c)] = static_cast<float>(channelSum[static_cast<std::size_t>(c)] / n);
    return true;
}

// ---------------------------------------------------------------------------
// appendHiddenBackground — the whole per-pixel synthesis for pixel P, run on
// P's real samples just before they are flattened: search, prune, and append
// ONE synthetic sample (Q's surface) to `samples`.  Returns whether one was
// appended.  `fillSearchPx` is the knob's clamped value (<= 0 = auto, resolved
// per pixel against P's own staged radius); `maxRadiusPx` is both the
// fallback reach and the reach the map was extended by.  `fillSmear` swaps
// the nearest Q's colour for the disc average when the PRIMARY tier found Q;
// a fallback-tier Q is always copied verbatim.
// ---------------------------------------------------------------------------
inline bool appendHiddenBackground(const SurfaceMap&      map,
                                   const MaxDepthPyramid& pyramid,
                                   const FlattenParams&   params,
                                   int x, int y,
                                   float fillSearchPx, float maxRadiusPx,
                                   bool fillSmear,
                                   std::vector<SampleRecord>& samples,
                                   FillSearchStats* stats = nullptr)
{
    if (!map.contains(x, y))
        return false;
    const std::ptrdiff_t iP = map.index(x, y);
    if (map.empty(iP))
        return false;

    const float radiusP  = map.plane(SurfaceMap::kRadius)[iP];
    const int   primary  = fillReachPx(resolveFillSearchPx(fillSearchPx, radiusP, maxRadiusPx));
    const int   fallback = fillReachPx(maxRadiusPx);

    const BackgroundSource q = findBackgroundSource(map, pyramid, x, y, primary, fallback,
                                                    FillPredicate(), stats);
    if (!q.found)
        return false;

    const float radiusQ = map.plane(SurfaceMap::kRadius)[map.index(q.qx, q.qy)];
    if (pruneSynthesis(residualTransmittance(samples), radiusP, radiusQ))
        return false;

    samples.resize(samples.size() + 1);
    synthesizeHiddenSample(map, q.qx, q.qy, rayDepthScaleAt(params, x, y), samples.back());
    if (fillSmear && q.distance <= static_cast<float>(primary)) {
        const float zBackP = map.plane(SurfaceMap::kZBack)[iP];
        const float stepP  = localDepthStep(map, x, y);
        averageBackground(map, FillPredicate(), x, y, zBackP, stepP, primary, samples.back());
    }
    return true;
}

} // namespace deepc

#endif // DEEPC_DEFOCUS_FILL_H
