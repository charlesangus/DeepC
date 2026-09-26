// SPDX-License-Identifier: MIT
//
// ============================================================================
//
//  test_defocus_scatter — unit tests for the POD scatter core
//
//  Covers DeepCDefocusScatter.h / .cpp: the SoA flatten, the depth sort, the
//  streaming scatter and resolve, the holdout SoA / boundary LUT, the fill's
//  surface map, the band plan and the band ledger.  POD level only: no NDK,
//  no DDImage type, no live Nuke session.
//
//  DETERMINISTIC.  The suite has no RNG: the fuzz/corpus cases use the
//  fixed-seed 64-bit LCG below, so every run — and every mutation-test run —
//  is bit-reproducible.  <random>'s distributions are not required to be
//  reproducible across implementations; a seeded in-file engine is.
//
//  REFERENCE VALUES ARE DERIVED INDEPENDENTLY, never by calling the function
//  under test to produce its own expectation:
//    * refRadiusPx()/refPartitionAlpha()/refFlatten() re-derive the flatten
//      from the documented formulae in double precision;
//    * refBlendedWeight()/refRasterize() re-derive the scatter's weights and
//      deposits from the KernelView seam and the documented blend;
//    * every band-level identity is asserted against a hand-derived closed
//      form, never against a re-run of the code.
//
//  Every tolerance is either exact (`==`) or a term-count bound N * 2^-24
//  stated beside it.
//
//  Builds standalone with plain
//  `g++ -std=c++17 tests/test_defocus_scatter.cpp src/DeepCDefocusScatter.cpp`
//  as well as through the DEEPC_BUILD_TESTS CMake option — no NDK anywhere.
//
// ============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "../src/DeepCDefocusFill.h"
#include "../src/DeepCDefocusScatter.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

using namespace deepc;

namespace {

// ===========================================================================
// Deterministic generator
// ===========================================================================

// Fixed-seed 64-bit LCG (Knuth's MMIX constants).
class Lcg {
public:
    explicit Lcg(std::uint64_t seed) : _s(seed) {}

    std::uint32_t next()
    {
        _s = _s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<std::uint32_t>(_s >> 33);
    }

    // [0, 1], inclusive at both ends so the degenerate endpoints get hit.
    float unit() { return static_cast<float>(next() % 1000001u) / 1000000.0f; }

    float range(float lo, float hi) { return lo + (hi - lo) * unit(); }

    int intRange(int lo, int hi)
    {
        return lo + static_cast<int>(next() % static_cast<std::uint32_t>(hi - lo + 1));
    }

private:
    std::uint64_t _s;
};

// ===========================================================================
// Rigs
// ===========================================================================

// THE STANDARD RIG: Physical, f=50, N=2.8, filmback 36mm at 1920px, metres,
// over a measured depth range of [1, 100].
CocParams makeStandardRig(float focusDistance, float maxRadiusPx = 100.0f)
{
    return makeCocParams(CocMode::Physical,
                          /*focalLengthMm*/   50.0f,
                          /*fStop*/           2.8f,
                          /*filmbackWidthMm*/ 36.0f,
                          /*focusDistance*/   focusDistance,
                          /*unitScaleValue*/  unitScale(WorldUnits::Meters),
                          /*formatWidthPx*/   1920.0f,
                          /*pixelAspect*/     1.0f,
                          /*frontMult*/       1.0f,
                          /*backMult*/        1.0f,
                          /*maxRadiusPx*/     maxRadiusPx,
                          /*sizePx*/          10.0f);
}

// Manual mode: radius = size * |1 - focus/d|.
CocParams makeManualRig(float sizePx, float focusDistance, float maxRadiusPx = 100.0f)
{
    return makeCocParams(CocMode::Manual,
                          50.0f, 2.8f, 36.0f,
                          focusDistance,
                          unitScale(WorldUnits::Meters),
                          1920.0f, 1.0f, 1.0f, 1.0f,
                          maxRadiusPx,
                          sizePx);
}

// The holdout boundary set the node builds for a frame range and a
// depth_layers count.
HoldoutBoundaries makeHoldoutBoundaries(const CocParams& p, float depthMin, float depthMax,
                                        int K)
{
    return makeUniformHoldoutBoundaries(makeBoundedDeltaCocBuckets(p, depthMin, depthMax, K));
}

HoldoutBoundaries makeStandardHoldoutBoundaries(const CocParams& p, int K = 16)
{
    return makeHoldoutBoundaries(p, 1.0f, 100.0f, K);
}

// ===========================================================================
// Independent reference implementations, in double, from the documented
// formulae.  Nothing here calls the function it is the reference for.
// ===========================================================================

// radius = clamp(0.5 * coc_mm * (formatWidth/filmbackWidth) * sideMult, 0, maxR)
// with coc_mm = (f/N)*f/(S_mm - f) * |1 - S_mm/d_mm|.  Physical mode only.
double refRadiusPx(const CocParams& p, double depth)
{
    if (!(depth > 0.0))
        return 0.0;

    const double sMm = static_cast<double>(p._focusDistance) * p._unitScale;
    const double dMm = depth * p._unitScale;
    if (dMm == sMm)
        return 0.0;

    double denom = sMm - p._focalLengthMm;
    if (!(denom > 1e-4))
        denom = 1e-4;

    const double cocScale = (p._focalLengthMm / p._fStop) * p._focalLengthMm / denom;
    const double cocMm    = cocScale * std::fabs(1.0 - sMm / dMm);
    const double pxPerMm  = p._formatWidthPx / p._filmbackWidthMm;
    const double mult     = (depth < p._focusDistance) ? p._frontMult : p._backMult;

    double r = 0.5 * cocMm * pxPerMm * mult;
    if (!(r > 0.0))
        return 0.0;
    return (r > p._maxRadiusPx) ? p._maxRadiusPx : r;
}

// 1 - (1-a)^t, and its premultiplied-colour partner a(t)/a, via std::pow.
double refPartitionAlpha(double a, double t)
{
    a = std::min(std::max(a, 0.0), 1.0);
    t = std::min(std::max(t, 0.0), 1.0);
    if (!(a > 0.0) || !(t > 0.0))
        return 0.0;
    if (t >= 1.0)
        return a;
    if (a >= 1.0)
        return 1.0;
    return 1.0 - std::pow(1.0 - a, t);
}

double refPartitionColorScale(double a, double t)
{
    a = std::min(std::max(a, 0.0), 1.0);
    t = std::min(std::max(t, 0.0), 1.0);
    if (!(a > 0.0))
        return t;
    if (a >= 1.0)
        return (t > 0.0) ? 1.0 : 0.0;
    return refPartitionAlpha(a, t) / a;
}

// "Which kernel would the scatter rasterise for this radius?", from the
// documented rule: below the sharp threshold one weight of 1.0 (bin -1),
// above it the pair (lower node, f) with f on the diameter, two radii sharing
// a bin when their f agree to 2^-20.  Found by SEARCHING the node radii, not
// by the closed-form inversion this reference exists to disagree with.
constexpr double kRefKernelBlendCells = 1048576.0;      // 2^20

std::int64_t refKernelBin(double radiusPx)
{
    if (!(radiusPx > static_cast<double>(kSharpRadiusPx)))
        return -1;

    int lo = 0, hi = 1;
    while (static_cast<double>(kernelGridRadius(hi)) < radiusPx) {
        lo = hi;
        hi *= 2;
        if (hi > (1 << 26))
            return static_cast<std::int64_t>(hi) * static_cast<std::int64_t>(kRefKernelBlendCells);
    }
    while (hi - lo > 1) {
        const int mid = lo + (hi - lo) / 2;
        if (static_cast<double>(kernelGridRadius(mid)) < radiusPx)
            lo = mid;
        else
            hi = mid;
    }
    const double rLo = static_cast<double>(kernelGridRadius(lo));
    const double rHi = static_cast<double>(kernelGridRadius(hi));
    if (radiusPx >= rHi)
        return static_cast<std::int64_t>(hi) * static_cast<std::int64_t>(kRefKernelBlendCells);
    const double f = (2.0 * radiusPx - 2.0 * rLo) / (2.0 * rHi - 2.0 * rLo);
    return static_cast<std::int64_t>(lo) * static_cast<std::int64_t>(kRefKernelBlendCells)
         + static_cast<std::int64_t>(std::floor(f * kRefKernelBlendCells));
}

double refMidDepth(double zFront, double zBack)
{
    if (!(zBack > zFront))
        return zFront;
    return zFront + 0.5 * (zBack - zFront);
}

double refSanitizeDepth(float v)
{
    if (std::isfinite(v))
        return static_cast<double>(v);
    return (v > 0.0f) ? static_cast<double>(FrameDepthRange::kMaxDepth) : 0.0;
}

// One expected SoA fragment.
struct RefFragment {
    double radius = 0.0;
    double signedRadius = 0.0;
    double depth  = 0.0;
    double alpha  = 0.0;
    double share  = 0.0;
    bool   volumetric = false;
    std::vector<double> channels;
};

// The flatten, re-derived: sanitise -> tidy -> (point | volumetric pieces) ->
// pre-merge groups -> collision runs -> back-to-front composite, in double.
// tidyOverlapping() and volumetricPieceBounds() are shared code with their own
// coverage (the latter's cut rule is pinned in its own test case below); what
// is re-derived is how the flatten stages, partitions and groups them.
std::vector<RefFragment> refFlatten(const FlattenParams& fp,
                                    std::vector<SampleRecord> samples,
                                    double* residualT = nullptr)
{
    const CocParams& p = fp.coc;
    const int C = fp.channelCount;
    for (SampleRecord& s : samples) {
        const double zf = refSanitizeDepth(s.zFront);
        double       zb = refSanitizeDepth(s.zBack);
        if (!(zb > zf))
            zb = zf;
        s.zFront = static_cast<float>(zf);
        s.zBack  = static_cast<float>(zb);
        s.alpha  = (s.alpha > 0.0f) ? ((s.alpha < 1.0f) ? s.alpha : 1.0f) : 0.0f;
        s.channels.resize(static_cast<std::size_t>(C), 0.0f);
    }
    if (samples.size() > 1)
        tidyOverlapping(samples);
    std::sort(samples.begin(), samples.end(),
        [](const SampleRecord& a, const SampleRecord& c) {
            return (a.zFront != c.zFront) ? a.zFront < c.zFront : a.zBack < c.zBack;
        });

    struct Staged {
        double zFront, zBack, alpha, depth, radius, signedRadius, share;
        bool   volumetric;
        std::vector<double> channels;
    };
    std::vector<Staged> staged;
    double t = 1.0;
    std::vector<VolumetricPiece> pieces(static_cast<std::size_t>(kMaxVolumetricPieces));
    for (const SampleRecord& s : samples) {
        if (!(s.alpha > 0.0f))
            continue;
        if (!(s.zBack > s.zFront)) {
            Staged st;
            st.zFront = s.zFront;
            st.zBack  = s.zBack;
            st.alpha  = s.alpha;
            st.depth  = s.zFront;
            st.signedRadius = signedCocPixels(p, s.zFront);
            st.radius = std::fabs(st.signedRadius);
            st.share  = t * s.alpha;
            st.volumetric = false;
            t *= 1.0 - s.alpha;
            for (float c : s.channels)
                st.channels.push_back(c);
            staged.push_back(st);
            continue;
        }
        const int n = volumetricPieceBounds(p, s.zFront, s.zBack, s.alpha, fp.pieceStepPx,
                                            pieces.data(),
                                            std::min(std::max(fp.maxVolumetricPieces, 1),
                                                     kMaxVolumetricPieces));
        const std::size_t first = staged.size();
        double parentShare = 0.0;
        for (int k = 0; k < n; ++k) {
            const VolumetricPiece& pc = pieces[static_cast<std::size_t>(k)];
            if (!(pc.t > 0.0f))
                continue;
            const double pa = refPartitionAlpha(s.alpha, pc.t);
            const double ps = refPartitionColorScale(s.alpha, pc.t);
            Staged st;
            st.zFront = pc.zFront;
            st.zBack  = pc.zBack;
            st.alpha  = pa;
            st.depth  = refMidDepth(pc.zFront, pc.zBack);
            st.signedRadius = signedCocPixels(p, static_cast<float>(st.depth));
            st.radius = std::fabs(st.signedRadius);
            st.share  = t * pa;
            parentShare += st.share;
            t *= 1.0 - pa;
            st.volumetric = true;
            for (float c : s.channels)
                st.channels.push_back(static_cast<double>(c) * ps);
            staged.push_back(st);
        }
        if (staged.size() > first) {
            for (std::size_t k = first; k + 1 < staged.size(); ++k)
                staged[k].share = 0.0;
            staged.back().share = parentShare;
        }
    }
    if (residualT != nullptr)
        *residualT = t;

    const auto bracket = [&](double depth) {
        return fp.holdoutConnected ? fp.holdoutBoundaries.locate(static_cast<float>(depth)).index
                                   : 0;
    };
    const auto sameLens = [&](double a, double b) {
        if (refKernelBin(std::fabs(a)) != refKernelBin(std::fabs(b)))
            return false;
        if (!(std::fabs(a) > kSharpRadiusPx))
            return true;
        return (a < 0.0) == (b < 0.0);
    };

    struct Run { std::size_t first, end; double depth, signedRadius; int bracket; bool vol; };
    std::vector<Run> runs;
    const bool merging = fp.preMerge && fp.mergeTolerancePx > 0.0f;
    std::size_t i = 0;
    while (i < staged.size()) {
        std::size_t j = i + 1;
        if (merging) {
            while (j < staged.size()
                   && std::fabs(staged[j].radius - staged[i].radius) <= fp.mergeTolerancePx
                   && bracket(staged[j].depth) == bracket(staged[i].depth))
                ++j;
        }
        double zf = staged[i].zFront, zb = staged[i].zBack;
        for (std::size_t k = i; k < j; ++k) {
            zf = std::min(zf, staged[k].zFront);
            zb = std::max(zb, staged[k].zBack);
        }
        Run g;
        g.first = i;
        g.end   = j;
        g.depth = static_cast<double>(sampleMidDepth(static_cast<float>(zf), static_cast<float>(zb)));
        g.signedRadius = signedCocPixels(p, static_cast<float>(g.depth));
        g.bracket = bracket(g.depth);
        g.vol = staged[i].volumetric;
        if (!runs.empty() && sameLens(runs.back().signedRadius, g.signedRadius)
            && runs.back().bracket == g.bracket)
            runs.back().end = j;
        else
            runs.push_back(g);
        i = j;
    }

    std::vector<RefFragment> out;
    for (const Run& r : runs) {
        RefFragment f;
        f.depth = r.depth;
        f.signedRadius = r.signedRadius;
        f.radius = std::fabs(r.signedRadius);
        f.volumetric = r.vol;
        f.channels.assign(static_cast<std::size_t>(C), 0.0);
        for (std::size_t k = r.end; k-- > r.first;) {
            const double tk = 1.0 - staged[k].alpha;
            for (int c = 0; c < C; ++c)
                f.channels[static_cast<std::size_t>(c)] =
                    staged[k].channels[static_cast<std::size_t>(c)]
                    + tk * f.channels[static_cast<std::size_t>(c)];
            f.alpha = staged[k].alpha + tk * f.alpha;
        }
        for (std::size_t k = r.first; k < r.end; ++k)
            f.share += staged[k].share;
        out.push_back(f);
    }
    return out;
}

// ===========================================================================
// Band plumbing
// ===========================================================================

struct Band {
    int C = 0, W = 0, H = 0;
    StreamPlanes             planes;
    PodBuffer<std::uint32_t> order;
    std::vector<float>       color;   // C planes of W*H, band-relative row-major
    std::vector<float>       alpha;   // W*H

    std::ptrdiff_t pixels() const { return static_cast<std::ptrdiff_t>(W) * H; }

    std::size_t at(int x, int y) const { return static_cast<std::size_t>(y) * W + x; }

    float outAlpha(int x, int y) const { return alpha[at(x, y)]; }

    float outColor(int c, int x, int y) const
    { return color[static_cast<std::size_t>(c) * pixels() + at(x, y)]; }
};

// Full band in the production order: allocate, sort, scatter (on a
// std::thread unless told otherwise — its thread-agnostic contract,
// exercised literally), virtual background when `residual` is supplied,
// resolve.  Without `residual` arrival carries only the fragments' own raw
// weight, as a scatter with no virtual background would.
void runBand(Band& band, const ScatterParams& sp, const SampleSoA& soa,
             const HoldoutSoA& holdout, const KernelSampler& kernel,
             bool useThread = true, const ResidualWindow* residual = nullptr,
             ScatterStats* stats = nullptr)
{
    band.planes.allocate(band.C, band.W, band.H);

    StreamSortScratch sortScratch;
    sortFragmentsByDepth(soa, band.order, sortScratch);

    ScatterScratch scratch;
    if (useThread) {
        std::thread worker([&] {
            scatterStreamCPU(sp, soa, band.order, holdout, kernel, band.planes, scratch, stats);
        });
        worker.join();
    } else {
        scatterStreamCPU(sp, soa, band.order, holdout, kernel, band.planes, scratch, stats);
    }

    if (residual != nullptr)
        scatterBackgroundCPU(sp, *residual, kernel, band.planes.arrival.data());

    // Poisoned, so a resolve that fails to overwrite is visible.
    band.color.assign(static_cast<std::size_t>(band.C) * band.pixels(), -777.0f);
    band.alpha.assign(static_cast<std::size_t>(band.pixels()), -777.0f);
    resolveStreamCPU(band.planes, band.color.data(), band.alpha.data());
}

double vecSum(const std::vector<float>& v)
{
    double s = 0.0;
    for (float x : v)
        s += static_cast<double>(x);
    return s;
}

double bandAlphaSum(const Band& b)
{
    double s = 0.0;
    for (float v : b.alpha)
        s += static_cast<double>(v);
    return s;
}

double bandColorSum(const Band& b, int c)
{
    double s = 0.0;
    const std::size_t px = static_cast<std::size_t>(b.pixels());
    for (std::size_t i = 0; i < px; ++i)
        s += static_cast<double>(b.color[static_cast<std::size_t>(c) * px + i]);
    return s;
}

SampleSoA flattenOnePixel(const FlattenParams& fp,
                          int x, int y, std::vector<SampleRecord> samples,
                          float* residualT = nullptr, float* residualRadiusPx = nullptr)
{
    SampleSoA soa;
    soa.begin(fp.channelCount, fp.groups);
    FlattenScratch scratch;
    flattenPixelToSoA(fp, x, y, samples, scratch, soa, nullptr,
                      residualT, residualRadiusPx);
    return soa;
}

// The auto `background_depth`: the CoC at the frame's farthest measured depth.
float autoBackgroundRadiusPx(const CocParams& p, float depthMax)
{
    return radiusPixels(p, depthMax);
}

// A rig with ONE real source pixel, windowed the way computeBand() windows it:
// over the whole output box, every other cell at T = 1 and the source
// pixel's own residual radius.  Sizing the window to the source pixel instead
// makes arrival equal the object's own kernel weight wherever it lands, and
// the fill then divides an anti-aliased bloom into a hard disc.
void oneSourcePixelWindow(ResidualWindow& window, int W, int H,
                          int px, int py,
                          float residualT, float residualRadiusPx)
{
    window.allocate(0, 0, W, H, residualRadiusPx);
    window.setPixel(px, py, residualT, residualRadiusPx);
}

// Flattens samplesFn(x, y) at every pixel of [x0,x1) x [y0,y1) into `soa` and
// records that pixel's residual into `window` — buildResidualWindow() without
// a live fetch loop.
template <typename SamplesFn>
void flattenIntoWithResidual(const FlattenParams& fp,
                             int x0, int y0, int x1, int y1,
                             SampleSoA& soa, FlattenScratch& scratch,
                             ResidualWindow& window, SamplesFn&& samplesFn)
{
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            std::vector<SampleRecord> v = samplesFn(x, y);
            const std::size_t i = static_cast<std::size_t>(window.index(x, y));
            float residualT = 1.0f;
            float residualR = window.radiusPx[i];
            flattenPixelToSoA(fp, x, y, v, scratch, soa, nullptr,
                              &residualT, &residualR);
            window.setPixel(x, y, residualT, residualR);
        }
    }
}

FlattenParams makeFlattenParams(const CocParams& p, int channelCount,
                                bool preMerge, float tolerancePx = 0.25f)
{
    FlattenParams fp;
    fp.coc = p;
    fp.preMerge = preMerge;
    fp.mergeTolerancePx = tolerancePx;
    fp.depthIsRayDistance = false;
    fp.channelCount = channelCount;
    fp.groups = makeSingleChannelGroup(channelCount);
    return fp;
}

ScatterParams makeScatterParams(int w, int h)
{
    ScatterParams sp;
    sp.bandX = 0;
    sp.bandY = 0;
    sp.bandWidth = w;
    sp.bandHeight = h;
    sp.sharpRadiusPx = kSharpRadiusPx;
    return sp;
}

// ===========================================================================
// Independent rasteriser
// ===========================================================================

// The bracketing-kernel blend from the stated rule — node A at (1-f), node B
// at f, f on the DIAMETER — with the floor node found by a linear walk of
// kernelGridRadius(), not by kernelGridBracket()'s closed form.
struct RefBracket {
    int    node[2]  = {0, 0};
    double blend[2] = {1.0, 0.0};
    int    passes   = 1;
};

RefBracket refBracket(float radius)
{
    int lo = 0;
    while (lo < 8000 && kernelGridRadius(lo + 1) <= radius)
        ++lo;
    const double dA = 2.0 * kernelGridRadius(lo);
    const double dB = 2.0 * kernelGridRadius(lo + 1);
    const double d  = 2.0 * static_cast<double>(radius);
    const double f  = (d > dA && d < dB) ? (d - dA) / (dB - dA) : 0.0;

    RefBracket b;
    b.node[0]  = lo;
    b.node[1]  = (f > 0.0) ? lo + 1 : lo;
    b.blend[0] = 1.0 - f;
    b.blend[1] = f;
    b.passes   = (f > 0.0) ? 2 : 1;
    return b;
}

// The blended kernel's weight at offset (dx, dy) from its centre.
double refBlendedWeight(const DiscKernelLUT& lut, float radius, int dx, int dy)
{
    const RefBracket b = refBracket(radius);
    double w = 0.0;
    for (int p = 0; p < b.passes; ++p) {
        const KernelView kv = lut.kernel(kernelGridRadius(b.node[p]), 0, 0, 0.0f, 0);
        REQUIRE(kv.valid());
        const int row = dy + kv.radiusY;
        if (row < 0 || row >= kv.rowCount)
            continue;
        const RowSpan& span = kv.row(row);
        if (span.empty() || dx < span.xStart || dx > span.xEnd)
            continue;
        w += static_cast<double>(kv.rowWeights(row)[dx - span.xStart]) * b.blend[p];
    }
    return w;
}

// The expected stream state for fragments whose effective weights sum to at
// most 1 at every pixel: then every deposit lands on free area, so
// Q = sum w*vis, A = sum a*w*vis, C = sum c*w*vis and arrival = sum share*w,
// with w the blended raw weight and vis the holdout's transmittance located
// by a binary search on depth (HoldoutVisibility::interp) — a different path
// from the scatter's O(1) locate.
struct ExpectedState {
    int C = 0, W = 0, H = 0;
    std::vector<double> claimed, alpha, color, arrival;

    void allocate(int c, int w, int h)
    {
        C = c; W = w; H = h;
        const std::size_t px = static_cast<std::size_t>(w) * h;
        claimed.assign(px, 0.0);
        alpha.assign(px, 0.0);
        color.assign(static_cast<std::size_t>(c) * px, 0.0);
        arrival.assign(px, 0.0);
    }
};

void refRasterize(ExpectedState& out, const ScatterParams& sp, const SampleSoA& soa,
                  const DiscKernelLUT& lut, const HoldoutSoA* holdout)
{
    const std::size_t px = static_cast<std::size_t>(out.W) * out.H;
    for (std::size_t f = 0; f < soa.fragmentCount(); ++f) {
        const float  radius = soa.radius[f];
        const float  depth  = soa.depth[f];
        const double a      = soa.alpha[f];
        const double share  = soa.arrivalShare[f];
        const float* col    = soa.colorOf(f);
        const int destX = static_cast<int>(soa.x[f]) - sp.bandX;
        const int destY = static_cast<int>(soa.y[f]) - sp.bandY;

        std::vector<std::pair<std::size_t, double>> touched;
        if (!(radius > sp.sharpRadiusPx)) {
            if (destX >= 0 && destX < out.W && destY >= 0 && destY < out.H)
                touched.emplace_back(static_cast<std::size_t>(destY) * out.W + destX, 1.0);
        } else {
            const int reach = static_cast<int>(std::ceil(radius)) + 3;
            for (int dy = -reach; dy <= reach; ++dy) {
                for (int dx = -reach; dx <= reach; ++dx) {
                    const int x = destX + dx, y = destY + dy;
                    if (x < 0 || x >= out.W || y < 0 || y >= out.H)
                        continue;
                    const double w = refBlendedWeight(lut, radius, dx, dy);
                    if (w != 0.0)
                        touched.emplace_back(static_cast<std::size_t>(y) * out.W + x, w);
                }
            }
        }

        for (const auto& tp : touched) {
            double w = tp.second;
            out.arrival[tp.first] += w * share;
            if (holdout != nullptr && holdout->enabled())
                w *= static_cast<double>(HoldoutVisibility::interp(
                        holdout->boundaries.boundaries(),
                        holdout->pixelLut(static_cast<std::ptrdiff_t>(tp.first)),
                        holdout->boundaryCount(), depth));
            out.claimed[tp.first] += w;
            out.alpha[tp.first]   += w * a;
            for (int c = 0; c < out.C; ++c)
                out.color[static_cast<std::size_t>(c) * px + tp.first] +=
                    w * static_cast<double>(col[c]);
        }
    }
}

// Every stored value is within `ulps` units of 2^-24 of the reference: each
// pixel's sums have a handful of terms, every one at most 1.
void checkState(const StreamPlanes& got, const ExpectedState& want, double ulps)
{
    REQUIRE(got.channelCount == want.C);
    REQUIRE(got.pixelCount == static_cast<std::ptrdiff_t>(want.W) * want.H);
    const double tol = ulps / 16777216.0;
    std::size_t bad = 0;
    for (std::size_t i = 0; i < want.claimed.size(); ++i) {
        if (std::fabs(static_cast<double>(got.claimed[i]) - want.claimed[i]) > tol) ++bad;
        if (std::fabs(static_cast<double>(got.alpha[i]) - want.alpha[i]) > tol) ++bad;
        if (std::fabs(static_cast<double>(got.arrival[i]) - want.arrival[i]) > tol) ++bad;
    }
    for (std::size_t i = 0; i < want.color.size(); ++i)
        if (std::fabs(static_cast<double>(got.color[i]) - want.color[i]) > tol) ++bad;
    CHECK(bad == 0);
}

// ===========================================================================
// Holdout plumbing
// ===========================================================================

// The supplier must fill `out` for EVERY band pixel in row-major order —
// appendPixel()'s documented CSR contract.
template <typename Fn>
void buildHoldout(HoldoutSampleSoA& samples, HoldoutLut& lut,
                  const HoldoutBoundaries& boundaries,
                  int w, int h, Fn perPixel, float depthScale = 1.0f)
{
    const std::ptrdiff_t px = static_cast<std::ptrdiff_t>(w) * h;
    samples.begin(px);
    std::vector<SampleRecord> scratch;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            scratch.clear();
            perPixel(x, y, scratch);
            samples.appendPixel(scratch, depthScale);
        }
    }
    lut.build(samples, boundaries);
}

// Appends one hand-built fragment; the radius is |signedRadius| and its sign
// goes to the flag byte, as the flatten stores it.
void appendFragmentAt(SampleSoA& soa, int x, int y, float signedRadius, float depth,
                      float alpha, float share, const std::vector<float>& channels,
                      FragmentKind kind = FragmentKind::Point)
{
    FragmentRecord f;
    f.x           = x;
    f.y           = y;
    f.radius      = std::fabs(signedRadius);
    f.depth       = depth;
    f.alpha       = alpha;
    f.share       = share;
    f.kind        = kind;
    f.cocNegative = signedRadius < 0.0f;
    REQUIRE(static_cast<int>(channels.size()) == soa.channelCount);
    soa.appendFragment(f, channels.data());
}

SampleRecord makeSample(float zFront, float zBack, float alpha,
                        std::vector<float> channels = {})
{
    SampleRecord s;
    s.zFront = zFront;
    s.zBack  = zBack;
    s.alpha  = alpha;
    s.channels = std::move(channels);
    return s;
}

} // namespace

// ===========================================================================
// The flatten
// ===========================================================================

namespace {

constexpr double kUlp = 1.0 / 16777216.0;      // 2^-24

} // namespace

TEST_CASE("flattenPixelToSoA reproduces an independent tidy + cut + merge reference")
{
    const CocParams p = makeStandardRig(10.0f);
    const int       C = 3;

    struct Fixture {
        const char* name;
        std::vector<SampleRecord> samples;
    };
    std::vector<Fixture> fixtures;
    fixtures.push_back({"one point sample",
        {makeSample(3.0f, 3.0f, 0.7f, {0.14f, 0.35f, 0.63f})}});
    fixtures.push_back({"two disjoint point samples at different kernels",
        {makeSample(2.0f, 2.0f, 0.5f, {0.1f, 0.2f, 0.3f}),
         makeSample(6.0f, 6.0f, 0.25f, {0.05f, 0.1f, 0.15f})}});
    fixtures.push_back({"two coincident point samples (tidy mixes them)",
        {makeSample(9.0f, 9.0f, 0.3f, {0.12f, 0.15f, 0.18f}),
         makeSample(9.0f, 9.0f, 0.4f, {0.04f, 0.08f, 0.12f})}});
    fixtures.push_back({"a volumetric span in front of focus (many pieces)",
        {makeSample(3.0f, 6.0f, 0.9f, {0.18f, 0.36f, 0.72f})}});
    fixtures.push_back({"a volumetric span straddling focus (the focal plane is a cut)",
        {makeSample(8.0f, 14.0f, 0.6f, {0.18f, 0.18f, 0.18f})}});
    fixtures.push_back({"a volumetric span reaching far outside [1, 100]",
        {makeSample(0.2f, 400.0f, 0.6f, {0.18f, 0.18f, 0.18f})}});
    fixtures.push_back({"a point in front of a volumetric span",
        {makeSample(1.5f, 1.5f, 0.8f, {0.2f, 0.3f, 0.4f}),
         makeSample(4.0f, 7.0f, 0.45f, {0.1f, 0.1f, 0.1f})}});
    fixtures.push_back({"overlapping volumetric spans (tidy cuts and mixes them)",
        {makeSample(2.0f, 6.0f, 0.5f, {0.2f, 0.2f, 0.2f}),
         makeSample(4.0f, 8.0f, 0.35f, {0.1f, 0.15f, 0.2f})}});
    fixtures.push_back({"three near-identical sharp samples (both merges take them)",
        {makeSample(9.0f, 9.0f, 0.2f, {0.02f, 0.04f, 0.06f}),
         makeSample(9.001f, 9.001f, 0.3f, {0.03f, 0.06f, 0.09f}),
         makeSample(9.002f, 9.002f, 0.25f, {0.025f, 0.05f, 0.075f})}});
    fixtures.push_back({"sharp samples either side of focus (one lens patch)",
        {makeSample(9.5f, 9.5f, 0.4f, {0.1f, 0.1f, 0.1f}),
         makeSample(10.5f, 10.5f, 0.6f, {0.2f, 0.2f, 0.2f})}});
    fixtures.push_back({"two point samples at DIFFERENT disc sizes behind focus",
        {makeSample(15.0f, 15.0f, 0.55f, {0.11f, 0.22f, 0.33f}),
         makeSample(50.0f, 50.0f, 0.65f, {0.13f, 0.26f, 0.39f})}});
    fixtures.push_back({"a point and a span of one disc size behind focus",
        {makeSample(60.0f, 60.0f, 0.6f, {0.12f, 0.24f, 0.36f}),
         makeSample(61.0f, 70.0f, 0.4f, {0.08f, 0.16f, 0.24f})}});

    const HoldoutBoundaries hb = makeStandardHoldoutBoundaries(p);
    for (bool holdoutConnected : {false, true})
    for (bool preMerge : {false, true})
    for (float stepPx : {0.5f, 2.0f}) {
        for (const Fixture& fx : fixtures) {
            CAPTURE(fx.name);
            CAPTURE(preMerge);
            CAPTURE(holdoutConnected);
            CAPTURE(stepPx);

            FlattenParams fp = makeFlattenParams(p, C, preMerge);
            fp.holdoutConnected  = holdoutConnected;
            fp.holdoutBoundaries = hb;
            fp.pieceStepPx       = stepPx;
            float residualT = -1.0f;
            const SampleSoA soa = flattenOnePixel(fp, 11, 23, fx.samples, &residualT);
            double refT = -1.0;
            const std::vector<RefFragment> want = refFlatten(fp, fx.samples, &refT);

            REQUIRE(soa.fragmentCount() == want.size());
            CHECK(std::fabs(static_cast<double>(residualT) - refT) <= 64.0 * kUlp);

            for (std::size_t i = 0; i < want.size(); ++i) {
                CAPTURE(i);
                const RefFragment& w = want[i];
                CHECK(soa.x[i] == 11);
                CHECK(soa.y[i] == 23);
                CHECK((fragmentKindOf(soa.flags[i]) == FragmentKind::Volumetric) == w.volumetric);
                CHECK(fragmentCocNegativeOf(soa.flags[i]) == (w.signedRadius < 0.0));

                // The reference splits with std::pow in double where the
                // flatten uses expm1/log1p in float, and composites a run of
                // up to a few dozen pieces: 64 units of 2^-24 on quantities
                // at most 1.
                CHECK(std::fabs(static_cast<double>(soa.alpha[i]) - w.alpha) <= 64.0 * kUlp);
                CHECK(std::fabs(static_cast<double>(soa.arrivalShare[i]) - w.share) <= 64.0 * kUlp);
                CHECK(static_cast<double>(soa.depth[i]) == w.depth);
                CHECK(std::fabs(static_cast<double>(soa.radius[i]) - refRadiusPx(p, w.depth))
                      <= 2e-5 * (1.0 + refRadiusPx(p, w.depth)));
                const float* got = soa.colorOf(i);
                for (int c = 0; c < C; ++c)
                    CHECK(std::fabs(static_cast<double>(got[c])
                                    - w.channels[static_cast<std::size_t>(c)]) <= 64.0 * kUlp);
            }
            CHECK(checkCompositionContract(soa));
        }
    }
}

TEST_CASE("checkCompositionContract accepts every flattened fragment and rejects each bad field")
{
    const CocParams p = makeStandardRig(10.0f);
    const FlattenParams fp = makeFlattenParams(p, 2, /*preMerge*/ true);
    Lcg rng(0xC0DEu);
    for (int iter = 0; iter < 500; ++iter) {
        std::vector<SampleRecord> v;
        float z = rng.range(0.5f, 20.0f);
        const int n = rng.intRange(1, 6);
        for (int s = 0; s < n; ++s) {
            const float a = rng.range(0.0f, 1.0f);
            const float th = (rng.unit() < 0.5f) ? 0.0f : rng.range(0.1f, 20.0f);
            v.push_back(makeSample(z, z + th, a, {a * rng.unit(), a * rng.unit()}));
            z += th + rng.range(0.0f, 3.0f);
        }
        const SampleSoA soa = flattenOnePixel(fp, 0, 0, v);
        std::size_t bad = 999;
        CHECK(checkCompositionContract(soa, &bad));
    }

    const float nan = std::numeric_limits<float>::quiet_NaN();
    struct Bad { const char* name; float radius, depth, alpha, share, c0; };
    const Bad bads[] = {
        {"alpha above 1",      1.0f, 5.0f, 1.0001f, 0.5f, 0.1f},
        {"negative alpha",     1.0f, 5.0f, -0.1f,   0.5f, 0.1f},
        {"NaN alpha",          1.0f, 5.0f, nan,     0.5f, 0.1f},
        {"negative radius",   -1.0f, 5.0f, 0.5f,    0.5f, 0.1f},
        {"NaN depth",          1.0f, nan,  0.5f,    0.5f, 0.1f},
        {"NaN share",          1.0f, 5.0f, 0.5f,    nan,  0.1f},
        {"NaN colour",         1.0f, 5.0f, 0.5f,    0.5f, nan},
    };
    for (const Bad& b : bads) {
        CAPTURE(b.name);
        SampleSoA soa;
        soa.begin(1, makeSingleChannelGroup(1));
        appendFragmentAt(soa, 0, 0, 1.0f, 4.0f, 0.5f, 0.5f, {0.25f});
        FragmentRecord f;
        f.radius = b.radius;
        f.depth  = b.depth;
        f.alpha  = b.alpha;
        f.share  = b.share;
        const float ch[1] = {b.c0};
        soa.appendFragment(f, ch);
        std::size_t first = 999;
        CHECK_FALSE(checkCompositionContract(soa, &first));
        CHECK(first == 1u);
    }
}

TEST_CASE("sameLensPatch: one kernel bin on one side of focus, or both sharp")
{
    const float r = 4.25f;
    CHECK(sameLensPatch(r, r));
    CHECK(sameLensPatch(-r, -r));
    CHECK_FALSE(sameLensPatch(-r, r));                    // mirrored patches
    CHECK_FALSE(sameLensPatch(r, r + 0.25f));             // different kernels
    CHECK(sameLensPatch(-0.3f, 0.45f));                   // sharp: the whole lens
    CHECK(sameLensPatch(0.0f, -kSharpRadiusPx));
    CHECK_FALSE(sameLensPatch(0.3f, 0.6f));               // one sharp, one not
    for (float a : {0.7f, 1.5f, 9.0f, 30.5f}) {
        CAPTURE(a);
        CHECK(sameLensPatch(a, a) == (refKernelBin(a) == refKernelBin(a)));
        CHECK(sameLensPatch(a, std::nextafter(a, 100.0f))
              == (refKernelBin(a) == refKernelBin(std::nextafter(a, 100.0f))));
    }

    // Through the flatten: pre_merge off, so only the collision merge can
    // join.  Opposite sides of focus at one disc size stay two fragments; the
    // same side joins; two sharp samples either side of focus join.
    const CocParams p = makeManualRig(8.0f, 10.0f);
    const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ false);
    const float zFront = 10.0f / (1.0f + 0.5f);          // radius 4, in front
    const float zBack  = 10.0f / (1.0f - 0.5f);          // radius 4, behind
    REQUIRE(radiusPixels(p, zFront) == doctest::Approx(radiusPixels(p, zBack)));
    CHECK(flattenOnePixel(fp, 0, 0, {makeSample(zFront, zFront, 0.5f, {0.1f}),
                                     makeSample(zBack, zBack, 0.5f, {0.1f})}).fragmentCount() == 2u);
    CHECK(flattenOnePixel(fp, 0, 0, {makeSample(zBack, zBack, 0.5f, {0.1f}),
                                     makeSample(zBack * 1.0000001f, zBack * 1.0000001f, 0.5f,
                                                {0.1f})}).fragmentCount() == 1u);
    CHECK(flattenOnePixel(fp, 0, 0, {makeSample(9.99f, 9.99f, 0.5f, {0.1f}),
                                     makeSample(10.01f, 10.01f, 0.5f, {0.1f})}).fragmentCount() == 1u);
}

// ===========================================================================
// The stream order
// ===========================================================================

TEST_CASE("sortFragmentsByDepth is the stable depth order, and checkStreamOrder accepts it "
          "and rejects a swapped pair")
{
    Lcg rng(0x50F7u);
    const float levels[] = {-3.0f, -0.0f, 0.0f, 1e-40f, 0.5f, 1.0f, 1.0f, 2.5f, 7.0f, 1e12f};
    for (int trial = 0; trial < 40; ++trial) {
        CAPTURE(trial);
        SampleSoA soa;
        soa.begin(1, makeSingleChannelGroup(1));
        const int n = rng.intRange(1, 3000);
        for (int i = 0; i < n; ++i) {
            const float d = (trial % 2 == 0)
                          ? levels[rng.intRange(0, 9)]
                          : 4.0f + rng.range(0.0f, 0.001f);   // shared top bytes
            appendFragmentAt(soa, i % 7, i / 7, 1.0f, d, 0.5f, 0.5f, {0.25f});
        }

        PodBuffer<std::uint32_t> order;
        StreamSortScratch scratch;
        sortFragmentsByDepth(soa, order, scratch);
        CHECK(checkStreamOrder(soa, order));

        std::vector<std::uint32_t> want(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i)
            want[static_cast<std::size_t>(i)] = static_cast<std::uint32_t>(i);
        std::stable_sort(want.begin(), want.end(), [&](std::uint32_t a, std::uint32_t b) {
            const float da = soa.depth[a], db = soa.depth[b];
            if (da < db) return true;
            if (db < da) return false;
            return std::signbit(da) && !std::signbit(db);      // -0 before +0
        });
        std::size_t mismatched = 0;
        for (int i = 0; i < n; ++i)
            if (order[static_cast<std::size_t>(i)] != want[static_cast<std::size_t>(i)])
                ++mismatched;
        CHECK(mismatched == 0u);

        // A swapped adjacent pair of distinct keys, or of equal keys (ties
        // must stay in emission order), is rejected; so is a repeated index
        // and a short permutation.
        for (int i = 0; i + 1 < n; ++i) {
            PodBuffer<std::uint32_t> bad;
            bad.assign(order.size(), 0u);
            for (std::size_t k = 0; k < order.size(); ++k)
                bad[k] = order[k];
            std::swap(bad[static_cast<std::size_t>(i)], bad[static_cast<std::size_t>(i) + 1]);
            if (!checkStreamOrder(soa, bad))
                continue;
            FAIL("a swapped pair was accepted at ", i);
        }
        if (n > 1) {
            PodBuffer<std::uint32_t> dup;
            dup.assign(order.size(), 0u);
            for (std::size_t k = 0; k < order.size(); ++k)
                dup[k] = order[k];
            dup[1] = dup[0];
            CHECK_FALSE(checkStreamOrder(soa, dup));
        }
        PodBuffer<std::uint32_t> shortOrder;
        shortOrder.assign(order.size() - 1, 0u);
        CHECK_FALSE(checkStreamOrder(soa, shortOrder));
    }
}

// ===========================================================================
// The streaming composite
// ===========================================================================

TEST_CASE("band-plan invariance: one frame scattered as 1, 2, 7 and 37-row bands gives "
          "bitwise the same pixels, with and without a holdout")
{
    const int W = 36, H = 44, C = 2;
    const CocParams p = makeManualRig(3.0f, 10.0f);
    DiscKernelLUT lut(0.0f, 16.0f, 1.0f, 1.0f);

    Lcg rng(0xBA4Du);
    std::vector<std::vector<SampleRecord>> pixels(static_cast<std::size_t>(W) * H);
    float rMax = 0.0f;
    for (auto& v : pixels) {
        if (rng.unit() < 0.1f)
            continue;
        float z = rng.range(3.0f, 12.0f);
        const int n = rng.intRange(1, 5);
        for (int s = 0; s < n; ++s) {
            const float a = rng.range(0.05f, 1.0f);
            const float th = (rng.unit() < 0.4f) ? rng.range(0.2f, 6.0f) : 0.0f;
            v.push_back(makeSample(z, z + th, a, {a * rng.unit(), a * rng.unit()}));
            rMax = std::max({rMax, radiusPixels(p, z), radiusPixels(p, z + th)});
            z += th + rng.range(0.05f, 3.0f);
        }
    }
    const int padY = static_cast<int>(std::ceil(rMax + 0.5f)) + 1;
    const HoldoutBoundaries hb = makeHoldoutBoundaries(p, 3.0f, 40.0f, 8);

    auto render = [&](int bandHeight, bool holdout, std::vector<float>& color,
                      std::vector<float>& alpha) {
        color.assign(static_cast<std::size_t>(C) * W * H, -1.0f);
        alpha.assign(static_cast<std::size_t>(W) * H, -1.0f);
        FlattenParams fp = makeFlattenParams(p, C, /*preMerge*/ true);
        fp.holdoutConnected  = holdout;
        fp.holdoutBoundaries = hb;
        for (int y0 = 0; y0 < H; y0 += bandHeight) {
            const int y1 = std::min(H, y0 + bandHeight);
            const int h  = y1 - y0;
            SampleSoA soa;
            soa.begin(C, fp.groups);
            FlattenScratch scratch;
            ResidualWindow window;
            std::vector<SampleRecord> v;
            REQUIRE(buildResidualWindow(window, 0, W, 0, H, 0, W, 0, H, y0, y1, padY,
                                        radiusPixels(p, 40.0f),
                [](int) { return true; },
                [&](int x, int y, float& t, float& r) {
                    v = pixels[static_cast<std::size_t>(y) * W + x];
                    flattenPixelToSoA(fp, x, y, v, scratch, soa, nullptr, &t, &r);
                    return true;
                }));

            HoldoutSampleSoA hs;
            HoldoutLut       hl;
            if (holdout) {
                hs.begin(static_cast<std::ptrdiff_t>(W) * h);
                for (int y = y0; y < y1; ++y)
                    for (int x = 0; x < W; ++x) {
                        std::vector<SampleRecord> hv;
                        if ((x / 5 + y / 3) % 2 == 0)
                            hv.push_back(makeSample(6.0f + 0.1f * x, 6.0f + 0.1f * x, 0.7f));
                        hs.appendPixel(hv, 1.0f);
                    }
                hl.build(hs, hb);
            }

            ScatterParams sp = makeScatterParams(W, h);
            sp.bandY = y0;
            Band band;
            band.C = C; band.W = W; band.H = h;
            runBand(band, sp, soa, hl.view(), lut, false, &window);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < W; ++x) {
                    const std::size_t o = static_cast<std::size_t>(y0 + y) * W + x;
                    alpha[o] = band.outAlpha(x, y);
                    for (int c = 0; c < C; ++c)
                        color[static_cast<std::size_t>(c) * W * H + o] = band.outColor(c, x, y);
                }
        }
    };

    for (bool holdout : {false, true}) {
        CAPTURE(holdout);
        std::vector<float> refColor, refAlpha;
        render(H, holdout, refColor, refAlpha);
        double alphaSum = 0.0;
        for (float a : refAlpha)
            alphaSum += a;
        REQUIRE(alphaSum > 0.25 * W * H);
        for (int bandHeight : {1, 2, 7, 37}) {
            CAPTURE(bandHeight);
            std::vector<float> color, alpha;
            render(bandHeight, holdout, color, alpha);
            std::size_t alphaDiffs = 0, colorDiffs = 0;
            for (std::size_t i = 0; i < alpha.size(); ++i)
                if (std::memcmp(&alpha[i], &refAlpha[i], sizeof(float)) != 0)
                    ++alphaDiffs;
            for (std::size_t i = 0; i < color.size(); ++i)
                if (std::memcmp(&color[i], &refColor[i], sizeof(float)) != 0)
                    ++colorDiffs;
            CHECK(alphaDiffs == 0u);
            CHECK(colorDiffs == 0u);
        }
    }
}

TEST_CASE("size-0 corpus: 900 pixels x 2..20 spp, points and spans, pre_merge on and off, "
          "bit-exact against a back-to-front flatten")
{
    // DeepToImage composites a pixel back to front, C = c + (1 - a) * C, in
    // float.  At size 0 every fragment is the sharp delta, both merges take a
    // whole pixel into one fragment, and that fragment's deposit is x = 1 on
    // empty state, so the output must be that float recurrence to the bit.
    const int C = 3, W = 30, H = 30;
    const CocParams p = makeManualRig(0.0f, 10.0f);
    DiscKernelLUT kernel(0.0f, 1.0f, 1.0f, 1.0f);

    for (int content = 0; content < 3; ++content)          // 0 point, 1 span, 2 mixed
    for (int spp : {2, 3, 5, 12, 20})
    for (bool preMerge : {false, true}) {
        CAPTURE(content);
        CAPTURE(spp);
        CAPTURE(preMerge);
        Lcg rng(0x7131u + static_cast<std::uint32_t>(spp * 7 + content * 977 + (preMerge ? 1 : 0)));
        std::vector<std::vector<SampleRecord>> pixels(static_cast<std::size_t>(W) * H);
        for (auto& v : pixels) {
            float z = rng.range(1.05f, 20.0f);
            for (int s = 0; s < spp; ++s) {
                const float a   = rng.range(0.02f, 1.0f);
                const bool  vol = (content == 1) || (content == 2 && (rng.next() & 1u));
                const float th  = vol ? rng.range(0.05f, 3.0f) : 0.0f;
                v.push_back(makeSample(z, z + th, a, {a * rng.unit(), a * rng.unit(), a * rng.unit()}));
                z += th + rng.range(0.05f, 6.0f);
            }
        }

        const FlattenParams fp = makeFlattenParams(p, C, preMerge);
        SampleSoA soa;
        soa.begin(C, fp.groups);
        FlattenScratch scratch;
        ResidualWindow window;
        window.allocate(0, 0, W, H, 0.0f);
        flattenIntoWithResidual(fp, 0, 0, W, H, soa, scratch, window,
            [&](int x, int y) { return pixels[static_cast<std::size_t>(y) * W + x]; });
        CHECK(soa.fragmentCount() == static_cast<std::size_t>(W * H));

        Band band;
        band.C = C; band.W = W; band.H = H;
        HoldoutSoA none;
        runBand(band, makeScatterParams(W, H), soa, none, kernel, false, &window);

        std::size_t alphaDiffs = 0, colorDiffs = 0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                std::vector<SampleRecord> s = pixels[static_cast<std::size_t>(y) * W + x];
                std::sort(s.begin(), s.end(), [](const SampleRecord& a, const SampleRecord& b) {
                    return a.zFront < b.zFront;
                });
                float a = 0.0f;
                float c[3] = {0.0f, 0.0f, 0.0f};
                for (std::size_t k = s.size(); k-- > 0;) {
                    const float t = 1.0f - s[k].alpha;
                    for (int ch = 0; ch < C; ++ch)
                        c[ch] = s[k].channels[static_cast<std::size_t>(ch)] + c[ch] * t;
                    a = s[k].alpha + a * t;
                }
                if (band.outAlpha(x, y) != a)
                    ++alphaDiffs;
                for (int ch = 0; ch < C; ++ch)
                    if (band.outColor(ch, x, y) != c[ch])
                        ++colorDiffs;
            }
        CHECK(alphaDiffs == 0u);
        CHECK(colorDiffs == 0u);
    }
}

TEST_CASE("a full-coverage volumetric parent reconstructs its alpha and its colour:alpha "
          "within term count, at any depth_layers")
{
    // A flat field of one slab, defocused: every piece is a full-coverage
    // layer, the layers compose exactly under the recency rule, so the
    // interior reads the parent's own alpha, with colour:alpha its own.  The
    // bound is one unit of 2^-24 per deposit reaching the pixel.
    struct Rig { float sizePx, zb; std::vector<float> alphas; };
    const Rig rigs[] = {{6.0f, 7.0f, {0.3f, 0.8f, 1.0f}},
                        {3.0f, 9.0f, {0.8f}}};
    const int W = 24, H = 24, C = 1, pad = 14;
    DiscKernelLUT lut(0.0f, 14.0f, 1.0f, 1.0f);
    const float zf = 4.0f, unpremult = 0.6f;

    for (const Rig& rig : rigs)
    for (int K : {4, 8, 16, 64})
    for (float alpha : rig.alphas) {
        CAPTURE(rig.sizePx);
        CAPTURE(rig.zb);
        CAPTURE(K);
        CAPTURE(alpha);
        const CocParams p = makeManualRig(rig.sizePx, 10.0f);
        const float zb = rig.zb;
        FlattenParams fp = makeFlattenParams(p, C, /*preMerge*/ true);
        fp.pieceStepPx         = volumetricPieceStepPx(p, makeFrameDepthRange(zf, zb, K), 0.25f);
        fp.maxVolumetricPieces = K + 1;

        SampleSoA soa;
        soa.begin(C, fp.groups);
        FlattenScratch scratch;
        ResidualWindow window;
        window.allocate(-pad, -pad, W + 2 * pad, H + 2 * pad, radiusPixels(p, zb));
        flattenIntoWithResidual(fp, -pad, -pad, W + pad, H + pad, soa, scratch, window,
            [&](int, int) { return std::vector<SampleRecord>{makeSample(zf, zb, alpha, {alpha * unpremult})}; });
        const std::size_t perPixel = soa.fragmentCount() / static_cast<std::size_t>((W + 2 * pad) * (H + 2 * pad));
        REQUIRE(perPixel >= 2u);

        long deposits = 0;
        for (std::size_t f = 0; f < perPixel; ++f) {
            const float r = soa.radius[f];
            const int reach = static_cast<int>(std::ceil(r)) + 2;
            for (int dy = -reach; dy <= reach; ++dy)
                for (int dx = -reach; dx <= reach; ++dx)
                    if (refBlendedWeight(lut, r, dx, dy) != 0.0)
                        ++deposits;
        }

        Band band;
        band.C = C; band.W = W; band.H = H;
        HoldoutSoA none;
        runBand(band, makeScatterParams(W, H), soa, none, lut, false, &window);

        const double bound = static_cast<double>(deposits) * kUlp;
        double worstAlpha = 0.0, worstRatio = 0.0;
        for (int y = 8; y < H - 8; ++y)
            for (int x = 8; x < W - 8; ++x) {
                const double a = band.outAlpha(x, y);
                worstAlpha = std::max(worstAlpha, std::fabs(a - alpha));
                worstRatio = std::max(worstRatio,
                    std::fabs(static_cast<double>(band.outColor(0, x, y)) / a - unpremult));
            }
        CAPTURE(perPixel);
        CAPTURE(deposits);
        CAPTURE(worstAlpha);
        CAPTURE(worstRatio);
        CHECK(worstAlpha <= bound);
        CHECK(worstRatio <= bound);
    }
}

TEST_CASE("holdout law: an opaque fragment's alpha is exactly the holdout visibility, "
          "sharp and defocused")
{
    const CocParams p = makeStandardRig(10.0f);
    const HoldoutBoundaries hb = makeStandardHoldoutBoundaries(p);
    const int W = 32, H = 20;
    DiscKernelLUT lut(0.0f, 30.0f, 1.0f, 1.0f);
    const float depth = 30.0f, unpremult = 0.7f;

    // A card of varying alpha in front of the fragment over the left part of
    // the band, so vis differs pixel to pixel.
    HoldoutSampleSoA hs;
    HoldoutLut       hl;
    buildHoldout(hs, hl, hb, W, H, [](int x, int, std::vector<SampleRecord>& out) {
        if (x < 20)
            out.push_back(makeSample(20.0f, 20.0f, 0.05f * static_cast<float>(x % 17), {}));
    });
    const HoldoutSoA view = hl.view();
    REQUIRE(view.enabled());

    auto visAt = [&](std::size_t i) {
        return static_cast<double>(HoldoutVisibility::interp(
            view.boundaries.boundaries(), view.pixelLut(static_cast<std::ptrdiff_t>(i)),
            view.boundaryCount(), depth));
    };

    SUBCASE("sharp: A == vis, end to end")
    {
        for (int x = 0; x < W; x += 3) {
            CAPTURE(x);
            SampleSoA soa;
            soa.begin(1, makeSingleChannelGroup(1));
            appendFragmentAt(soa, x, 7, 0.0f, depth, 1.0f, 1.0f, {unpremult});
            Band band;
            band.C = 1; band.W = W; band.H = H;
            runBand(band, makeScatterParams(W, H), soa, view, lut);
            const double vis = visAt(band.at(x, 7));
            CHECK(std::fabs(static_cast<double>(band.outAlpha(x, 7)) - vis) <= 2.0 * kUlp);
            CHECK(band.outColor(0, x, 7) == doctest::Approx(unpremult * band.outAlpha(x, 7)).epsilon(2.0 * kUlp));
        }
    }

    SUBCASE("defocused: A == w * vis at every pixel the disc reaches")
    {
        const float r = 6.3f;
        SampleSoA soa;
        soa.begin(1, makeSingleChannelGroup(1));
        appendFragmentAt(soa, 18, 10, r, depth, 1.0f, 1.0f, {unpremult});
        Band band;
        band.C = 1; band.W = W; band.H = H;
        runBand(band, makeScatterParams(W, H), soa, view, lut);
        int reached = 0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const double w = refBlendedWeight(lut, r, x - 18, y - 10);
                const std::size_t i = band.at(x, y);
                CHECK(std::fabs(static_cast<double>(band.planes.alpha[i]) - w * visAt(i)) <= 3.0 * kUlp);
                CHECK(std::fabs(static_cast<double>(band.planes.color[i])
                                - unpremult * w * visAt(i)) <= 3.0 * kUlp);
                if (w > 0.0 && visAt(i) < 1.0)
                    ++reached;
            }
        CHECK(reached > 20);
    }
}

TEST_CASE("the scratch-row blend equals a single pass at the blended radius, within 2 ulps "
          "per weight, and is deposited as one row")
{
    const int W = 90, H = 90;
    DiscKernelLUT lut(0.0f, 40.0f, 1.0f, 1.0f);
    const ScatterParams sp = makeScatterParams(W, H);

    int blended = 0;
    for (float radius : {0.83f, 1.37f, 2.71f, 5.55f, 12.3f, 17.25f, 33.3f}) {
        CAPTURE(radius);
        if (refBracket(radius).passes != 2)
            continue;
        ++blended;

        // blendBracketRow against the double blend, row by row.
        const KernelGridBracket br = kernelGridBracket(radius);
        const KernelView kvA = lut.kernel(kernelGridRadius(br.indexA), 0, 0, 0.0f, 0);
        const KernelView kvB = lut.kernel(kernelGridRadius(br.indexB), 0, 0, 0.0f, 0);
        std::vector<float> row(static_cast<std::size_t>(2 * std::max(kvA.radiusX, kvB.radiusX) + 1));
        double worst = 0.0;
        const int ry = std::max(kvA.radiusY, kvB.radiusY);
        for (int dy = -ry; dy <= ry; ++dy) {
            int xs = 0;
            const int n = blendBracketRow(kvA, 1.0f - br.frac, kvB, br.frac, dy, row.data(), xs);
            for (int k = 0; k < n; ++k)
                worst = std::max(worst, std::fabs(static_cast<double>(row[static_cast<std::size_t>(k)])
                                                  - refBlendedWeight(lut, radius, xs + k, dy)));
        }
        CHECK(worst <= 2.0 * kUlp);

        // Through the scatter: one unit fragment on empty state deposits the
        // blended row, into claimed area and arrival alike.
        SampleSoA soa;
        soa.begin(1, makeSingleChannelGroup(1));
        appendFragmentAt(soa, W / 2, H / 2, radius, 5.0f, 1.0f, 1.0f, {1.0f});
        Band band;
        band.C = 1; band.W = W; band.H = H;
        HoldoutSoA none;
        runBand(band, sp, soa, none, lut);
        double worstQ = 0.0, worstArrival = 0.0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const double w = refBlendedWeight(lut, radius, x - W / 2, y - H / 2);
                worstQ = std::max(worstQ, std::fabs(static_cast<double>(band.planes.claimed[band.at(x, y)]) - w));
                worstArrival = std::max(worstArrival, std::fabs(static_cast<double>(band.planes.arrival[band.at(x, y)]) - w));
            }
        CHECK(worstQ <= 2.0 * kUlp);
        CHECK(worstArrival <= 2.0 * kUlp);
    }
    CHECK(blended >= 5);

    SUBCASE("over claimed state the rule sees the blended row once, not two passes")
    {
        // A sharp opaque-ish layer claims every pixel first; then a blended
        // fog fragment lands on claimed area.  The expected state is the
        // deposit body applied to the reference blended row in one call.
        const float radius = 5.55f;
        SampleSoA soa;
        soa.begin(1, makeSingleChannelGroup(1));
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                appendFragmentAt(soa, x, y, 0.0f, 1.0f, 0.6f, 0.6f, {0.3f});
        appendFragmentAt(soa, W / 2, H / 2, radius, 5.0f, 0.5f, 0.5f, {0.25f});
        Band band;
        band.C = 1; band.W = W; band.H = H;
        HoldoutSoA none;
        runBand(band, sp, soa, none, lut);

        StreamPlanes want;
        want.allocate(1, W, H);
        const StreamPlaneView v = want.view();
        std::vector<float> xRow(static_cast<std::size_t>(W));
        const float one = 1.0f, sharpColor = 0.3f, fogColor = 0.25f;
        for (std::ptrdiff_t i = 0; i < v.pixelCount; ++i)
            depositStreamSpanRecency(v, i, &one, xRow.data(), 1, 0.6f, 0.0f, &sharpColor, 1);
        const KernelGridBracket br = kernelGridBracket(radius);
        const KernelView kvA = lut.kernel(kernelGridRadius(br.indexA), 0, 0, 0.0f, 0);
        const KernelView kvB = lut.kernel(kernelGridRadius(br.indexB), 0, 0, 0.0f, 0);
        std::vector<float> row(static_cast<std::size_t>(2 * std::max(kvA.radiusX, kvB.radiusX) + 1));
        for (int dy = -std::max(kvA.radiusY, kvB.radiusY); dy <= std::max(kvA.radiusY, kvB.radiusY); ++dy) {
            int xs = 0;
            const int n = blendBracketRow(kvA, 1.0f - br.frac, kvB, br.frac, dy, row.data(), xs);
            if (n > 0)
                depositStreamSpanRecency(v, static_cast<std::ptrdiff_t>(H / 2 + dy) * W + (W / 2 + xs),
                                         row.data(), xRow.data(), n, 0.5f, radius, &fogColor, 1);
        }
        std::size_t diffs = 0;
        for (std::size_t i = 0; i < static_cast<std::size_t>(W * H); ++i) {
            if (band.planes.alpha[i] != want.alpha[i]) ++diffs;
            if (band.planes.claimed[i] != want.claimed[i]) ++diffs;
            if (band.planes.color[i] != want.color[i]) ++diffs;
        }
        CHECK(diffs == 0u);
    }
}

TEST_CASE("scatterStreamCPU's deposits match an independent rasterisation where every deposit "
          "lands on free area")
{
    // Fragments spread so their weights sum to at most 1 at every pixel:
    // then Q, A, C and arrival are plain sums of the blended weights, which
    // refRasterize() derives from the KernelView seam in double.
    const int W = 64, H = 40, C = 2;
    DiscKernelLUT lut(0.0f, 20.0f, 1.0f, 1.0f);
    const ScatterParams sp = makeScatterParams(W, H);

    SampleSoA soa;
    soa.begin(C, makeSingleChannelGroup(C));
    appendFragmentAt(soa, 10, 10, 0.0f, 3.0f, 0.9f, 0.9f, {0.45f, 0.2f});
    appendFragmentAt(soa, 30, 12, 4.3f, 4.0f, 0.8f, 0.5f, {0.4f, 0.1f});
    appendFragmentAt(soa, 33, 14, -7.9f, 2.0f, 0.6f, 0.6f, {0.3f, 0.3f});
    appendFragmentAt(soa, 50, 25, 12.25f, 9.0f, 1.0f, 0.2f, {0.5f, 0.9f});
    appendFragmentAt(soa, 62, 39, 5.0f, 5.0f, 0.7f, 0.7f, {0.35f, 0.05f});   // clipped by the band
    appendFragmentAt(soa, 5, 30, 1.5f, 1.0f, 0.0f, 0.0f, {0.2f, 0.0f});      // emissive

    Band band;
    band.C = C; band.W = W; band.H = H;
    HoldoutSoA none;
    runBand(band, sp, soa, none, lut);

    ExpectedState want;
    want.allocate(C, W, H);
    refRasterize(want, sp, soa, lut, nullptr);
    double maxClaimed = 0.0;
    for (double q : want.claimed)
        maxClaimed = std::max(maxClaimed, q);
    REQUIRE(maxClaimed <= 1.0);
    checkState(band.planes, want, 4.0);
}

TEST_CASE("claimed area is min(sum of w*vis, 1) and alpha never exceeds it, over dense "
          "overlapping fragments")
{
    // The invariant the recency rule keeps whatever the order: a deposit
    // covers free area first, so Q is the clamped running sum of the
    // effective weights, and A = Q * (1 - T) <= Q.
    const int W = 40, H = 40;
    DiscKernelLUT lut(0.0f, 12.0f, 1.0f, 1.0f);
    const ScatterParams sp = makeScatterParams(W, H);
    const CocParams p = makeStandardRig(10.0f);
    const HoldoutBoundaries hb = makeStandardHoldoutBoundaries(p);
    HoldoutSampleSoA hs;
    HoldoutLut       hl;
    buildHoldout(hs, hl, hb, W, H, [](int x, int y, std::vector<SampleRecord>& out) {
        if ((x + y) % 3 == 0)
            out.push_back(makeSample(4.0f, 4.0f, 0.5f, {}));
    });

    Lcg rng(0xDE05u);
    for (bool holdout : {false, true}) {
        CAPTURE(holdout);
        SampleSoA soa;
        soa.begin(1, makeSingleChannelGroup(1));
        for (int i = 0; i < 400; ++i) {
            const float r = (rng.unit() < 0.2f) ? 0.0f : rng.range(0.6f, 9.0f);
            appendFragmentAt(soa, rng.intRange(0, W - 1), rng.intRange(0, H - 1),
                             (rng.unit() < 0.5f) ? -r : r, rng.range(1.0f, 20.0f),
                             rng.range(0.0f, 1.0f), 0.1f, {0.2f});
        }
        const HoldoutSoA view = holdout ? hl.view() : HoldoutSoA{};
        Band band;
        band.C = 1; band.W = W; band.H = H;
        runBand(band, sp, soa, view, lut);

        ExpectedState sums;
        sums.allocate(1, W, H);
        refRasterize(sums, sp, soa, lut, holdout ? &view : nullptr);
        double worstQ = 0.0, worstExcess = 0.0;
        for (std::size_t i = 0; i < sums.claimed.size(); ++i) {
            worstQ = std::max(worstQ, std::fabs(static_cast<double>(band.planes.claimed[i])
                                                - std::min(sums.claimed[i], 1.0)));
            worstExcess = std::max(worstExcess, static_cast<double>(band.planes.alpha[i])
                                                - static_cast<double>(band.planes.claimed[i]));
        }
        // 400 fragments, at most a few dozen reaching one pixel.
        CHECK(worstQ <= 64.0 * kUlp);
        CHECK(worstExcess <= 64.0 * kUlp);
    }
}

TEST_CASE("StreamPlanes: allocate() and zero() leave every plane zero, keep the allocation, "
          "and bytesForBand is W*B*(C+6)*4 with no depth_layers factor")
{
    StreamPlanes planes;
    planes.allocate(3, 7, 5);
    CHECK(planes.sizeBytes() >= StreamPlanes::bytesForBand(3, 7, 5));
    CHECK(StreamPlanes::bytesForBand(3, 7, 5) == 7u * 5u * 9u * 4u);
    CHECK(StreamPlanes::bytesForBand(4, 4096, 64) == 4096u * 64u * 10u * 4u);
    CHECK(StreamPlanes::bytesForBand(-1, 10, 10) == 10u * 10u * 6u * 4u);

    auto dirty = [&]() {
        for (PodBuffer<float>* b : {&planes.claimed, &planes.alpha, &planes.oldArea,
                                    &planes.oldMass, &planes.lastCoc, &planes.color,
                                    &planes.arrival})
            for (std::size_t i = 0; i < b->size(); ++i)
                (*b)[i] = 3.0f;
    };
    auto allZero = [&]() {
        std::size_t n = 0;
        for (const PodBuffer<float>* b : {&planes.claimed, &planes.alpha, &planes.oldArea,
                                          &planes.oldMass, &planes.lastCoc, &planes.color,
                                          &planes.arrival})
            for (std::size_t i = 0; i < b->size(); ++i)
                if ((*b)[i] != 0.0f)
                    ++n;
        return n;
    };
    const float* before = planes.alpha.data();
    dirty();
    planes.zero();
    CHECK(allZero() == 0u);
    CHECK(planes.alpha.data() == before);
    dirty();
    planes.allocate(3, 7, 5);
    CHECK(allZero() == 0u);
    CHECK(planes.color.size() == 3u * 7u * 5u);
    planes.allocate(1, 4, 2);
    CHECK(allZero() == 0u);
    CHECK(planes.view().valid());
    planes.release();
    CHECK(planes.sizeBytes() == 0u);
}

TEST_CASE("bench: one 4K band at 20 spp -- flatten, sort, deposit, background, resolve"
          * doctest::skip())
{
    // Run with `--no-skip -tc='bench*'`.  A 4096-wide band of 64 rows with its
    // fetch window, 20 samples per pixel in depth clusters (a mix of points
    // and slabs), radii up to ~8 px: the band shape the node plans at 4K.
    const int W = 4096, B = 64, C = 3;
    const CocParams p = makeManualRig(4.0f, 10.0f);
    DiscKernelLUT lut(0.0f, 12.0f, 1.0f, 1.0f);
    const int padY = 9;
    FlattenParams fp = makeFlattenParams(p, C, /*preMerge*/ true);
    fp.pieceStepPx = volumetricPieceStepPx(p, makeFrameDepthRange(3.0f, 40.0f, 16), 0.25f);
    fp.maxVolumetricPieces = 17;

    using Clock = std::chrono::steady_clock;
    auto ms = [](Clock::time_point a, Clock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };

    SampleSoA soa;
    soa.begin(C, fp.groups);
    FlattenScratch scratch;
    ResidualWindow window;
    Lcg rng(0xB16Bu);
    std::vector<SampleRecord> v;
    const auto t0 = Clock::now();
    REQUIRE(buildResidualWindow(window, 0, W, 0, B, 0, W, -padY, B + padY, 0, B, padY,
                                radiusPixels(p, 40.0f),
        [](int) { return true; },
        [&](int x, int y, float& t, float& r) {
            v.clear();
            float z = rng.range(3.0f, 30.0f);
            for (int s = 0; s < 20; ++s) {
                const float a  = rng.range(0.05f, 0.6f);
                const float th = (s % 5 == 4) ? rng.range(0.2f, 2.0f) : 0.0f;
                v.push_back(makeSample(z, z + th, a, {a * 0.5f, a * 0.4f, a * 0.3f}));
                z += th + ((s % 4 == 3) ? rng.range(0.5f, 3.0f) : 0.002f);
            }
            flattenPixelToSoA(fp, x, y, v, scratch, soa, nullptr, &t, &r);
            return true;
        }));
    const auto t1 = Clock::now();

    PodBuffer<std::uint32_t> order;
    StreamSortScratch sortScratch;
    sortFragmentsByDepth(soa, order, sortScratch);
    const auto t2 = Clock::now();

    StreamPlanes planes;
    planes.allocate(C, W, B);
    ScatterScratch sc;
    ScatterStats stats;
    scatterStreamCPU(makeScatterParams(W, B), soa, order, HoldoutSoA{}, lut, planes, sc, &stats);
    const auto t3 = Clock::now();
    scatterBackgroundCPU(makeScatterParams(W, B), window, lut, planes.arrival.data());
    const auto t4 = Clock::now();
    std::vector<float> color(static_cast<std::size_t>(C) * W * B), alpha(static_cast<std::size_t>(W) * B);
    resolveStreamCPU(planes, color.data(), alpha.data());
    const auto t5 = Clock::now();

    CHECK(checkStreamOrder(soa, order));
    std::printf("bench 4096x%d band (+/-%d rows), 20 spp: fragments %zu, pixel deposits %zu\n"
                "  flatten %.1f ms  sort %.1f ms  deposit %.1f ms  background %.1f ms  "
                "resolve %.1f ms\n",
                B, padY, soa.fragmentCount(), stats.pixelDeposits,
                ms(t0, t1), ms(t1, t2), ms(t2, t3), ms(t3, t4), ms(t4, t5));
}


TEST_CASE("the tidy pre-pass is correctness-required: coincident samples over-composite, "
          "and the sharp path reproduces a sequential `over` exactly")
{
    // "tidy + sharp-path = sequential over".  Two coincident point samples at
    // alpha 0.3 and 0.4 are ONE surface pair, so
    // the answer is the sequential over 0.3 + 0.4*0.7 = 0.58 -- not the 0.7
    // an additive accumulation of the two would give.
    const CocParams    p  = makeStandardRig(10.0f);
    const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ false);

    // depth 9.0 is 0.266px of CoC on this rig, i.e. the SHARP fast path.
    REQUIRE(radiusPixels(p, 9.0f) < kSharpRadiusPx);

    float residualT = 1.0f, residualR = 0.0f;
    const SampleSoA soa = flattenOnePixel(fp, 16, 16,
        {makeSample(9.0f, 9.0f, 0.3f, {0.3f * 0.8f}),
         makeSample(9.0f, 9.0f, 0.4f, {0.4f * 0.8f})},
        &residualT, &residualR);

    // Tidy collapsed the coincident pair into ONE sample before staging.
    REQUIRE(soa.fragmentCount() == 1);
    const double expectedAlpha = 0.3 + 0.4 * (1.0 - 0.3);          // 0.58, exact
    const double expectedColor = 0.3 * 0.8 + 0.4 * 0.8 * (1.0 - 0.3);
    CHECK(std::fabs(static_cast<double>(soa.alpha[0]) - expectedAlpha) <= 1e-6);

    const int W = 32, H = 32;
    DiscKernelLUT lut(0.0f, 4.0f, 1.0f, 1.0f);
    Band band;
    band.C = 1; band.W = W; band.H = H;
    HoldoutSoA noHoldout;

    // The production pipeline's middle step: without it, arrival carries only
    // the fragment's own raw weight (here exactly 1, the sharp path), so its
    // residual T=0.42 is never restored and this pixel's own deposit divides
    // by less than the numerator it was building -- inflating alpha past 1.
    ResidualWindow window;
    oneSourcePixelWindow(window, W, H, 16, 16, residualT, residualR);
    runBand(band, makeScatterParams(W, H), soa, noHoldout, lut, true, &window);

    // A sharp fragment deposits weight 1 into its own pixel, so the band
    // integral IS that pixel and it must be the sequential `over`.
    CHECK(std::fabs(bandAlphaSum(band) - expectedAlpha) <= 1e-6);
    CHECK(std::fabs(bandColorSum(band, 0) - expectedColor) <= 1e-6);
    CHECK(std::fabs(static_cast<double>(band.outAlpha(16, 16)) - expectedAlpha) <= 1e-6);
}

TEST_CASE("flatten sanitisation: non-finite depths, inverted spans and zero-alpha samples")
{
    const CocParams    p  = makeStandardRig(10.0f);
    const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ false);

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    SUBCASE("alpha-0 samples are dropped outright (DeepToImage parity)")
    {
        const SampleSoA soa = flattenOnePixel(fp, 0, 0,
            {makeSample(3.0f, 3.0f, 0.0f, {0.9f}),
             makeSample(4.0f, 4.0f, 0.5f, {0.5f})});
        REQUIRE(soa.fragmentCount() == 1);
        CHECK(soa.alpha[0] == doctest::Approx(0.5f));
    }

    SUBCASE("a NaN depth becomes 0 (sharp) rather than poisoning the sort")
    {
        const SampleSoA soa = flattenOnePixel(fp, 0, 0,
            {makeSample(nan, nan, 0.5f, {0.5f})});
        REQUIRE(soa.fragmentCount() == 1);
        CHECK(soa.depth[0] == 0.0f);
        CHECK(soa.radius[0] == 0.0f);          // d <= 0 -> radius 0
        CHECK(std::isfinite(soa.alpha[0]));
    }

    SUBCASE("+inf becomes the far-field limit, and stays finite")
    {
        const SampleSoA soa = flattenOnePixel(fp, 0, 0,
            {makeSample(inf, inf, 0.5f, {0.5f})});
        REQUIRE(soa.fragmentCount() == 1);
        CHECK(soa.depth[0] == FrameDepthRange::kMaxDepth);
        CHECK(std::isfinite(soa.radius[0]));
    }

    SUBCASE("an inverted span (zBack < zFront) collapses to a point sample")
    {
        const SampleSoA soa = flattenOnePixel(fp, 0, 0,
            {makeSample(5.0f, 2.0f, 0.5f, {0.5f})});
        REQUIRE(soa.fragmentCount() == 1);
        CHECK(fragmentKindOf(soa.flags[0]) == FragmentKind::Point);
        CHECK(soa.depth[0] == doctest::Approx(5.0f));
    }

    SUBCASE("a NaN alpha clamps to 0 and the sample is dropped")
    {
        const SampleSoA soa = flattenOnePixel(fp, 0, 0,
            {makeSample(3.0f, 3.0f, nan, {0.5f})});
        CHECK(soa.fragmentCount() == 0);
    }
}

TEST_CASE("depthIsRayDistance applies the per-pixel ray-distance -> Z correction")
{
    // The node's depth-range pass has to apply this correction IDENTICALLY:
    // the two passes disagreeing puts every corner-pixel sample below
    // depthMin.  It is per PIXEL, always shrinks the depth, and is the
    // identity on the optical axis; unpinned, dropping it entirely is
    // invisible.
    const CocParams    p  = makeStandardRig(10.0f);

    FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ false);
    fp.depthIsRayDistance = true;
    fp.formatHeightPx     = 1080.0f;

    // Independent derivation, in double, from the geometry: the pixel's radial
    // filmback offset r, then z = ray * f / sqrt(f^2 + r^2).
    auto refZ = [&](int x, int y, double ray) {
        const double mmPerPxX = 36.0 / 1920.0;
        const double mmPerPxY = mmPerPxX;                 // pixelAspect 1
        const double dx = ((x + 0.5) - 0.5 * 1920.0) * mmPerPxX;
        const double dy = ((y + 0.5) - 0.5 * 1080.0) * mmPerPxY;
        const double r  = std::sqrt(dx * dx + dy * dy);
        return ray * 50.0 / std::sqrt(50.0 * 50.0 + r * r);
    };

    const float ray = 20.0f;

    // Far off-axis: the correction bites (measured factor 0.9281 at this corner).
    {
        const SampleSoA soa = flattenOnePixel(fp, 1900, 1050,
            {makeSample(ray, ray, 0.5f, {0.25f})});
        REQUIRE(soa.fragmentCount() == 1);
        const double want = refZ(1900, 1050, ray);
        CHECK(want < 19.0);                                  // it really is a big correction
        CHECK(std::fabs(static_cast<double>(soa.depth[0]) - want) <= 2e-5 * want);
    }

    // On axis: the identity to within half a pixel of offset.
    {
        const SampleSoA soa = flattenOnePixel(fp, 960, 540,
            {makeSample(ray, ray, 0.5f, {0.25f})});
        REQUIRE(soa.fragmentCount() == 1);
        CHECK(std::fabs(static_cast<double>(soa.depth[0]) - refZ(960, 540, ray)) <= 2e-5 * ray);
        CHECK(std::fabs(static_cast<double>(soa.depth[0]) - ray) <= 1e-4);
    }

    // Knob OFF at the same pixel: the raw depth, untouched.
    {
        FlattenParams off = fp;
        off.depthIsRayDistance = false;
        const SampleSoA soa = flattenOnePixel(off, 1900, 1050,
            {makeSample(ray, ray, 0.5f, {0.25f})});
        REQUIRE(soa.fragmentCount() == 1);
        CHECK(soa.depth[0] == doctest::Approx(ray));
    }

    // Both ENDPOINTS of a span are scaled by the one factor, so the span stays
    // a span and the split still sees a monotone range.
    {
        const SampleSoA soa = flattenOnePixel(fp, 1900, 1050,
            {makeSample(6.0f, 30.0f, 0.5f, {0.25f})});
        REQUIRE(soa.fragmentCount() >= 1);
        CHECK(static_cast<double>(soa.depth[0]) < refZ(1900, 1050, 30.0));
        CHECK(static_cast<double>(soa.depth[0]) > refZ(1900, 1050, 6.0));
    }
}


TEST_CASE("channel counts: the flatten sizes its staging from the SoA, the scatter from "
          "min(SoA, planes) -- neither runs off the smaller of the two")
{
    // Both of these are safety nets against a caller whose FlattenParams and
    // SampleSoA::begin() (or whose SampleSoA and StreamPlanes) disagree; the
    // first was a heap-buffer-overflow under ASAN before it was added.  Nothing
    // pinned either of them.
    const CocParams    p  = makeStandardRig(10.0f);

    SUBCASE("flatten: params.channelCount SMALLER than the SoA's is a dropped channel, "
            "never a short read")
    {
        const int soaChan = 6;
        FlattenParams fp = makeFlattenParams(p, /*channelCount*/ 2, /*preMerge*/ false);
        fp.groups = makeSingleChannelGroup(soaChan);

        SampleSoA soa;
        soa.begin(soaChan, fp.groups);
        FlattenScratch scratch;
        // A VOLUMETRIC sample: the span path sizes its staging buffer with
        // resize(nChan), which is where a params-sized buffer reads short.
        std::vector<float> ch(static_cast<std::size_t>(soaChan));
        for (int c = 0; c < soaChan; ++c)
            ch[static_cast<std::size_t>(c)] = 0.9f * (0.1f + 0.13f * c);
        std::vector<SampleRecord> v{makeSample(2.0f, 3.0f, 0.9f, ch)};
        flattenPixelToSoA(fp, 0, 0, v, scratch, soa, nullptr, nullptr, nullptr);
        REQUIRE(soa.fragmentCount() >= 1u);

        // Every channel the SoA declared carries its scaled value; the pieces'
        // colour scales rebuild the parent under `over`, so the simplest
        // exact statement is that the RATIOS between channels survive.
        for (std::size_t i = 0; i < soa.fragmentCount(); ++i) {
            const float* got = soa.colorOf(i);
            REQUIRE(got[0] > 0.0f);
            for (int c = 1; c < soaChan; ++c) {
                CAPTURE(i);
                CAPTURE(c);
                CHECK(std::fabs(got[c] / got[0]
                                - ch[static_cast<std::size_t>(c)] / ch[0]) <= 1e-05);
            }
        }
    }

    SUBCASE("scatter: an SoA with MORE channels than the planes writes only the planes' own")
    {
        const int W = 12, H = 12;
        DiscKernelLUT lut(0.0f, 8.0f, 1.0f, 1.0f);

        SampleSoA soa;
        soa.begin(3, makeSingleChannelGroup(3));            // three channels...
        appendFragmentAt(soa, W / 2, H / 2, 0.0f, 5.0f, 1.0f, 1.0f, {0.3f, 0.5f, 0.7f});

        Band band;
        band.C = 1; band.W = W; band.H = H;                  // ...ONE plane
        HoldoutSoA none;
        runBand(band, makeScatterParams(W, H), soa, none, lut);

        REQUIRE(band.planes.color.size() == static_cast<std::size_t>(W * H));
        double sum = 0.0;
        for (float v : band.planes.color)
            sum += static_cast<double>(v);
        CHECK(sum == doctest::Approx(0.3));
        CHECK(band.planes.color[band.at(W / 2, H / 2)] == 0.3f);
        CHECK(band.planes.alpha[band.at(W / 2, H / 2)] == 1.0f);
    }
}


namespace {

// Sequential front-to-back `over` in double — a DeepToImage flatten of one
// pixel, which is what the size-0 gate is written against.  Nothing here calls
// the flatten, the scatter or the composite.
struct RefOver {
    double alpha = 0.0;
    std::vector<double> color;
};

RefOver refSequentialOver(std::vector<SampleRecord> s, int channelCount)
{
    std::sort(s.begin(), s.end(), [](const SampleRecord& a, const SampleRecord& b) {
        return (a.zFront != b.zFront) ? a.zFront < b.zFront : a.zBack < b.zBack;
    });

    RefOver r;
    r.color.assign(static_cast<std::size_t>(channelCount), 0.0);
    double t = 1.0;
    for (const SampleRecord& x : s) {
        const double a = std::min(std::max<double>(x.alpha, 0.0), 1.0);
        for (int c = 0; c < channelCount; ++c)
            r.color[static_cast<std::size_t>(c)] +=
                t * static_cast<double>(x.channels[static_cast<std::size_t>(c)]);
        r.alpha += t * a;
        t *= (1.0 - a);
    }
    return r;
}

// A Manual-mode rig whose `size` sets the radius directly: size 0 is validation
// scene (a)'s all-in-focus case, where every fragment is on the sharp path.

} // namespace

// ===========================================================================
// The gather-share partition — flattenPixelToSoA's `share`/residual out-params
// ===========================================================================

namespace {

// Sums one freshly-flattened pixel's arrivalShare.  Every case below flattens
// exactly one pixel into a fresh SoA, so every appended fragment belongs to it
// and this is the pixel's whole "shares" side of the partition.
double shareSum(const SampleSoA& soa)
{
    double sum = 0.0;
    for (std::size_t i = 0; i < soa.fragmentCount(); ++i)
        sum += static_cast<double>(soa.arrivalShare[i]);
    return sum;
}

} // namespace


TEST_CASE("the gather-share partition sums to exactly 1: shares + residual, fuzzed over "
          "point, volumetric-split, pre-merged and same-pixel-collision stacks")
{
    // THE MUTATION-TESTED PROPERTY.  Every SUBCASE below fuzzes a different
    // stack SHAPE; all of them must hold shareSum(soa) + residualT == 1 to
    // 1e-6, because the partition (share = t * alpha; t *= (1 - alpha)) makes
    // it true by construction regardless of how downstream pre-merge or the
    // deposit-collision merge later regroup the staged fragments — both only
    // ever SUM shares, never rescale them.
    Lcg rng(0xA57Eu);
    const CocParams    p  = makeStandardRig(10.0f);

    SUBCASE("point stacks")
    {
        for (int iter = 0; iter < 400; ++iter) {
            CAPTURE(iter);
            const bool preMerge = (rng.unit() < 0.5f);
            const FlattenParams fp = makeFlattenParams(p, 1, preMerge, rng.range(0.0f, 2.0f));
            const int n = rng.intRange(1, 8);
            std::vector<SampleRecord> v;
            for (int s = 0; s < n; ++s) {
                const float z = rng.range(1.05f, 99.0f);
                const float a = rng.range(0.001f, 1.0f);
                v.push_back(makeSample(z, z, a, {a * 0.5f}));
            }
            float residualT = -1.0f, residualR = -1.0f;
            const SampleSoA soa = flattenOnePixel(fp, 0, 0, v, &residualT, &residualR);
            REQUIRE(residualT >= 0.0f);
            CHECK(std::fabs(shareSum(soa) + static_cast<double>(residualT) - 1.0) <= 1e-6);
        }
    }

    SUBCASE("volumetric-split stacks")
    {
        for (int iter = 0; iter < 400; ++iter) {
            CAPTURE(iter);
            const bool preMerge = (rng.unit() < 0.5f);
            const FlattenParams fp = makeFlattenParams(p, 1, preMerge, rng.range(0.0f, 2.0f));
            const int n = rng.intRange(1, 4);
            std::vector<SampleRecord> v;
            float z = rng.range(1.05f, 30.0f);
            for (int s = 0; s < n; ++s) {
                const float thickness = rng.range(2.0f, 25.0f);
                const float a = rng.range(0.001f, 1.0f);
                v.push_back(makeSample(z, z + thickness, a, {a * 0.5f}));
                z += thickness + rng.range(0.5f, 8.0f);
                if (z > 95.0f)
                    z = rng.range(1.05f, 10.0f);
            }
            float residualT = -1.0f, residualR = -1.0f;
            const SampleSoA soa = flattenOnePixel(fp, 0, 0, v, &residualT, &residualR);
            REQUIRE(residualT >= 0.0f);
            CHECK(std::fabs(shareSum(soa) + static_cast<double>(residualT) - 1.0) <= 1e-6);
        }
    }

    SUBCASE("pre-merged stacks")
    {
        // A generous tolerance and a tight depth cluster: pre-merge groups
        // these aggressively.  The whole subcase is checked to have exercised
        // the merge at least once, so the fuzz is not vacuous.
        bool mergedAtLeastOnce = false;
        for (int iter = 0; iter < 400; ++iter) {
            CAPTURE(iter);
            const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ true, /*tol*/ 5.0f);
            const int n = rng.intRange(2, 6);
            std::vector<SampleRecord> v;
            const float base = rng.range(1.05f, 90.0f);
            for (int s = 0; s < n; ++s) {
                const float z = base + rng.range(0.0f, 0.2f) * static_cast<float>(s);
                const float a = rng.range(0.001f, 1.0f);
                v.push_back(makeSample(z, z, a, {a * 0.5f}));
            }
            float residualT = -1.0f, residualR = -1.0f;
            const SampleSoA soa = flattenOnePixel(fp, 0, 0, v, &residualT, &residualR);
            REQUIRE(residualT >= 0.0f);
            if (soa.fragmentCount() < static_cast<std::size_t>(n))
                mergedAtLeastOnce = true;
            CHECK(std::fabs(shareSum(soa) + static_cast<double>(residualT) - 1.0) <= 1e-6);
        }
        CHECK(mergedAtLeastOnce);
    }

    SUBCASE("same-pixel-collision stacks")
    {
        // Straddle the focal plane: every sample stays on the sharp path, so
        // every pair sees the whole lens and the collision merge joins them
        // whether or not pre-merge grouped them first.
        bool collidedAtLeastOnce = false;
        for (int iter = 0; iter < 400; ++iter) {
            CAPTURE(iter);
            const bool preMerge = (rng.unit() < 0.5f);
            const FlattenParams fp = makeFlattenParams(p, 1, preMerge);
            const int n = rng.intRange(2, 4);
            std::vector<SampleRecord> v;
            for (int s = 0; s < n; ++s) {
                const bool  front = (s % 2 == 0);
                const float z     = front ? rng.range(8.5f, 9.9f) : rng.range(10.1f, 11.5f);
                const float a     = rng.range(0.001f, 1.0f);
                REQUIRE(radiusPixels(p, z) < kSharpRadiusPx);
                v.push_back(makeSample(z, z, a, {a * 0.5f}));
            }
            float residualT = -1.0f, residualR = -1.0f;
            const SampleSoA soa = flattenOnePixel(fp, 0, 0, v, &residualT, &residualR);
            REQUIRE(residualT >= 0.0f);
            if (soa.fragmentCount() < static_cast<std::size_t>(n))
                collidedAtLeastOnce = true;
            CHECK(std::fabs(shareSum(soa) + static_cast<double>(residualT) - 1.0) <= 1e-6);
        }
        CHECK(collidedAtLeastOnce);
    }
}

TEST_CASE("the deposit-collision merge sums shares, never attenuates them: bit-identical to "
          "the raw staged shares it merged")
{
    // z = 9.063 / 11.039 straddle the focal plane on the sharp path, so the
    // two see the whole lens and merge into ONE emitted fragment, whichever
    // merge takes them.
    const CocParams    p  = makeStandardRig(10.0f);

    for (bool preMerge : {false, true}) {
        CAPTURE(preMerge);
        const FlattenParams fp = makeFlattenParams(p, 1, preMerge);
        std::vector<SampleRecord> v{makeSample(9.063f, 9.063f, 0.4667f, {0.4667f * 0.25f}),
                                    makeSample(11.039f, 11.039f, 0.5899f, {0.5899f * 0.75f})};
        SampleSoA soa;
        soa.begin(1, fp.groups);
        FlattenScratch scratch;
        flattenPixelToSoA(fp, 4, 4, v, scratch, soa, nullptr, nullptr, nullptr);

        REQUIRE(scratch.stagedCount == 2u);      // two fragments were staged...
        REQUIRE(soa.fragmentCount() == 1u);      // ...and the collision merged them

        // The staged shares, fixed before either merge runs, summed as the
        // merge sums them (one float addition).
        const float expected = scratch.staged[0].share + scratch.staged[1].share;
        CHECK(soa.arrivalShare[0] == expected);   // bit-exact, not approximate
    }
}

TEST_CASE("residualRadiusPx is the deepest STAGED fragment's own radius, for point, "
          "volumetric-split and pre-merged stacks")
{
    const CocParams    p  = makeStandardRig(10.0f);

    SUBCASE("point stack")
    {
        const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ false);
        std::vector<SampleRecord> v{makeSample(3.0f, 3.0f, 0.4f, {0.2f}),
                                    makeSample(20.0f, 20.0f, 0.6f, {0.3f})};   // deepest
        SampleSoA soa;
        soa.begin(1, fp.groups);
        FlattenScratch scratch;
        float residualT = -1.0f, residualR = -1.0f;
        flattenPixelToSoA(fp, 0, 0, v, scratch, soa, nullptr, &residualT, &residualR);

        REQUIRE(scratch.stagedCount >= 1u);
        CHECK(residualR == scratch.staged[scratch.stagedCount - 1].radius);

        // Independent cross-check, in double precision, off the design
        // reference's own CoC formula -- this stack never merges, so the
        // deepest sample's own radius at its own mid-depth is unambiguous.
        CHECK(residualR == doctest::Approx(refRadiusPx(p, 20.0)).epsilon(1e-4));
    }

    SUBCASE("volumetric-split stack")
    {
        const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ false);
        // One span in front of focus, where its CoC varies by many piece
        // steps.
        std::vector<SampleRecord> v{makeSample(2.0f, 8.0f, 0.7f, {0.35f})};
        SampleSoA soa;
        soa.begin(1, fp.groups);
        FlattenScratch scratch;
        float residualT = -1.0f, residualR = -1.0f;
        flattenPixelToSoA(fp, 0, 0, v, scratch, soa, nullptr, &residualT, &residualR);

        REQUIRE(scratch.stagedCount >= 2u);      // the span really did split
        CHECK(residualR == scratch.staged[scratch.stagedCount - 1].radius);

        // Cross-check: the tail piece's own [zFront, zBack], at its own
        // mid-depth, off the design reference's CoC formula.
        const FlattenScratch::Staged& tail = scratch.staged[scratch.stagedCount - 1];
        CHECK(tail.zBack == 8.0f);
        const double tailMid = refMidDepth(tail.zFront, tail.zBack);
        CHECK(residualR == doctest::Approx(refRadiusPx(p, tailMid)).epsilon(1e-4));
    }

    SUBCASE("pre-merged stack")
    {
        // Two point samples inside a generous merge tolerance, so pre-merge
        // folds them into ONE emitted fragment -- whose own radius
        // (the union midpoint) residualRadiusPx must NOT report.
        const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ true, /*tol*/ 5.0f);
        std::vector<SampleRecord> v{makeSample(50.0f, 50.0f, 0.4f, {0.2f}),
                                    makeSample(50.3f, 50.3f, 0.5f, {0.25f})};  // deepest
        SampleSoA soa;
        soa.begin(1, fp.groups);
        FlattenScratch scratch;
        float residualT = -1.0f, residualR = -1.0f;
        flattenPixelToSoA(fp, 0, 0, v, scratch, soa, nullptr, &residualT, &residualR);

        REQUIRE(scratch.stagedCount == 2u);      // both staged separately...
        REQUIRE(soa.fragmentCount() == 1u);      // ...then pre-merged into one

        CHECK(residualR == scratch.staged[scratch.stagedCount - 1].radius);
        // And it is NOT the merged fragment's own (union-midpoint) radius --
        // getting that distinction right is the reason this is captured
        // before pre-merge runs, not read back off the SoA.
        CHECK(residualR != soa.radius[0]);
        CHECK(residualR == doctest::Approx(refRadiusPx(p, 50.3)).epsilon(1e-4));
    }
}

TEST_CASE("one volumetric parent claims arrival at ONE radius: its pieces pool their "
          "shares onto the deepest piece")
{
    // The cut is an artefact of the step, so it must not move where a parent
    // claims arrival -- otherwise the deficit division fires on a parent whose
    // parts span a wide radius range.  Pooling is per PARENT, never per pixel:
    // two genuinely different surfaces keep two claims at two radii, which is
    // the signal the coverage fill exists to read.
    const CocParams     p  = makeStandardRig(10.0f);
    const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ false);

    SUBCASE("a split parent's whole share sits on its deepest part")
    {
        float residualT = -1.0f, residualR = -1.0f;
        const SampleSoA soa = flattenOnePixel(fp, 0, 0,
            {makeSample(2.0f, 8.0f, 0.7f, {0.35f})}, &residualT, &residualR);
        const std::size_t n = soa.fragmentCount();
        REQUIRE(n >= 2u);                        // the span really did split

        std::size_t claimants = 0;
        for (std::size_t i = 0; i < n; ++i)
            if (soa.arrivalShare[i] != 0.0f) {
                ++claimants;
                CHECK(soa.radius[i] == residualR);
                CHECK(soa.arrivalShare[i] == doctest::Approx(0.7).epsilon(1e-6));
            }
        CHECK(claimants == 1u);
        CHECK(std::fabs(shareSum(soa) + static_cast<double>(residualT) - 1.0) <= 1e-6);
    }

    SUBCASE("a point sample behind it keeps its OWN claim")
    {
        float residualT = -1.0f, residualR = -1.0f;
        const SampleSoA soa = flattenOnePixel(fp, 0, 0,
            {makeSample(2.0f, 8.0f, 0.7f, {0.35f}),
             makeSample(20.0f, 20.0f, 0.6f, {0.3f})}, &residualT, &residualR);
        const std::size_t n = soa.fragmentCount();
        REQUIRE(n >= 3u);

        std::size_t claimants = 0;
        for (std::size_t i = 0; i < n; ++i)
            if (soa.arrivalShare[i] != 0.0f)
                ++claimants;
        CHECK(claimants == 2u);                  // one per parent, not one per part
        CHECK(soa.arrivalShare[n - 1] == doctest::Approx(0.3 * 0.6).epsilon(1e-6));
        CHECK(std::fabs(shareSum(soa) + static_cast<double>(residualT) - 1.0) <= 1e-6);
    }

    SUBCASE("a lone point sample is bit-identical to the unpooled partition")
    {
        float residualT = -1.0f, residualR = -1.0f;
        const SampleSoA soa = flattenOnePixel(fp, 0, 0,
            {makeSample(3.0f, 3.0f, 0.4f, {0.2f})}, &residualT, &residualR);
        REQUIRE(soa.fragmentCount() == 1u);
        CHECK(soa.arrivalShare[0] == 0.4f);      // share = 1 * alpha, exactly
        CHECK(residualT == 0.6f);
    }
}

TEST_CASE("an empty pixel leaves residualT at 1 and does not touch residualRadiusPx")
{
    const CocParams    p  = makeStandardRig(10.0f);
    const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ true);

    SUBCASE("no samples at all")
    {
        SampleSoA soa;
        soa.begin(1, fp.groups);
        FlattenScratch scratch;
        std::vector<SampleRecord> v;
        float residualT = -1.0f, residualR = 12345.0f;
        flattenPixelToSoA(fp, 0, 0, v, scratch, soa, nullptr, &residualT, &residualR);
        CHECK(residualT == 1.0f);
        CHECK(residualR == 12345.0f);            // untouched, per the caller's own default
        CHECK(soa.fragmentCount() == 0u);
    }

    SUBCASE("samples present but every one fails the zero-alpha early-out")
    {
        // Same convention as the empty-list case: nothing was staged, so
        // there is no "deepest sample" to take a residual radius from.
        SampleSoA soa;
        soa.begin(1, fp.groups);
        FlattenScratch scratch;
        std::vector<SampleRecord> v{makeSample(5.0f, 5.0f, 0.0f, {0.0f}),
                                    makeSample(9.0f, 9.0f, 0.0f, {0.0f})};
        float residualT = -1.0f, residualR = 12345.0f;
        flattenPixelToSoA(fp, 0, 0, v, scratch, soa, nullptr, &residualT, &residualR);
        CHECK(residualT == 1.0f);
        CHECK(residualR == 12345.0f);
        CHECK(soa.fragmentCount() == 0u);
    }
}

TEST_CASE("a zero-alpha sample is not a surface: it sets neither a share nor the residual "
          "radius, whether it is alone in the pixel or behind real content")
{
    // The residual is the pixel's virtual background, scattered at the CoC of
    // the deepest surface ACTUALLY PRESENT so that the surface's own kernel
    // and its residual's kernel tile and alpha / arrival recovers the true
    // alpha.  An alpha-0 sample is invisible to the composite (the zero-alpha
    // early-out, DeepToImage parity), carries no share (t * 0), and so is not
    // a surface the residual could sit behind.  One rule covers both
    // shapes: a pixel of nothing but alpha-0 samples IS an empty pixel, and an
    // alpha-0 sample behind real content changes nothing about that content's
    // flatten.  The alternative -- letting the alpha-0 sample's depth pick
    // the residual radius -- is the mismatched-radius defect the per-pixel
    // radius exists to avoid, and the last block measures it.
    const CocParams    p  = makeStandardRig(10.0f);
    const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ true);
    const float alpha = 0.9f, unpremult = 0.5f;
    const float zNear = 5.0f, zFar = 60.0f;
    const float rNear = radiusPixels(p, zNear);
    const float rFar  = radiusPixels(p, zFar);
    REQUIRE(rNear > 1.0f);
    REQUIRE(rFar > 1.0f);
    REQUIRE(std::fabs(rFar - rNear) > 0.3f);     // two genuinely different kernels

    float tAlone = -1.0f, rAlone = -1.0f;
    const SampleSoA alone = flattenOnePixel(fp, 7, 7,
        {makeSample(zNear, zNear, alpha, {alpha * unpremult})}, &tAlone, &rAlone);

    float tBehind = -1.0f, rBehind = -1.0f;
    const SampleSoA behind = flattenOnePixel(fp, 7, 7,
        {makeSample(zNear, zNear, alpha, {alpha * unpremult}),
         makeSample(zFar, zFar, 0.0f, {0.3f})}, &tBehind, &rBehind);

    // Same fragments, same partition, same residual radius: the alpha-0
    // sample left no trace.
    REQUIRE(alone.fragmentCount() == 1u);
    REQUIRE(behind.fragmentCount() == 1u);
    CHECK(behind.radius[0] == alone.radius[0]);
    CHECK(behind.arrivalShare[0] == alone.arrivalShare[0]);
    CHECK(behind.alpha[0] == alone.alpha[0]);
    CHECK(behind.color[0] == alone.color[0]);
    CHECK(tBehind == tAlone);
    CHECK(rBehind == rAlone);
    CHECK(rAlone == rNear);
    CHECK(tAlone == 1.0f - alpha);

    // ...and alone it is an empty pixel: T = 1, the caller's default radius
    // kept, nothing staged.
    float tOnly = -1.0f, rOnly = 12345.0f;
    const SampleSoA only = flattenOnePixel(fp, 7, 7,
        {makeSample(zFar, zFar, 0.0f, {0.3f})}, &tOnly, &rOnly);
    CHECK(only.fragmentCount() == 0u);
    CHECK(tOnly == 1.0f);
    CHECK(rOnly == 12345.0f);

    // THE CONSEQUENCE, end to end.  With no field around it (every other
    // pixel's claim zeroed, so the centre's arrival is the sample's own
    // kernel plus its own residual's), the pixel with the alpha-0 sample
    // behind it resolves to alpha 0.9 exactly, because its residual scatters
    // at the surface's own radius.  Forcing that residual onto the alpha-0
    // sample's radius instead is the mismatch: alpha / arrival drifts off 0.9
    // by the difference in the two kernels' centre weights.
    const int W = 60, H = 60;
    DiscKernelLUT lut(0.0f, 30.0f, 1.0f, 1.0f);
    auto resolveAt = [&](float residualRadius) {
        Band band;
        band.C = 1; band.W = W; band.H = H;
        SampleSoA soa;
        soa.begin(1, fp.groups);
        FlattenScratch scratch;
        ResidualWindow window;
        window.allocate(0, 0, W, H, residualRadius);
        flattenIntoWithResidual(fp, 0, 0, W, H, soa, scratch, window,
            [&](int x, int y) {
                std::vector<SampleRecord> v;
                if (x == W / 2 && y == H / 2) {
                    v.push_back(makeSample(zNear, zNear, alpha, {alpha * unpremult}));
                    v.push_back(makeSample(zFar, zFar, 0.0f, {0.3f}));
                }
                return v;
            });
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                if (x != W / 2 || y != H / 2)
                    window.setPixel(x, y, 0.0f, residualRadius);
        const std::size_t centre = static_cast<std::size_t>(window.index(W / 2, H / 2));
        REQUIRE(window.t[centre] == 1.0f - alpha);
        REQUIRE(window.radiusPx[centre] == rNear);      // the flatten's own choice
        window.radiusPx[centre] = residualRadius;
        HoldoutSoA none;
        runBand(band, makeScatterParams(W, H), soa, none, lut, false, &window);
        return band.outAlpha(W / 2, H / 2);
    };
    const float own      = resolveAt(rNear);
    const float deeper   = resolveAt(rFar);
    const double wcNear  = refBlendedWeight(lut, rNear, 0, 0);
    const double wcFar   = refBlendedWeight(lut, rFar, 0, 0);
    const double wantDeeper = alpha * wcNear / (alpha * wcNear + (1.0 - alpha) * wcFar);
    CAPTURE(rNear); CAPTURE(rFar); CAPTURE(own); CAPTURE(deeper);
    CHECK(own == doctest::Approx(alpha).epsilon(1e-6));
    CHECK(deeper == doctest::Approx(wantDeeper).epsilon(1e-4));
    CHECK(std::fabs(deeper - alpha) > 0.005f);
}


TEST_CASE("residualWindowYRange clips the virtual-background window to the OUTPUT box, "
          "never to anything narrower")
{
    SUBCASE("interior band: padY reach stays well inside the output box")
    {
        int y0 = -1, y1 = -1;
        residualWindowYRange(/*outputBoxY0*/ 0, /*outputBoxY1*/ 1000,
                             /*bandY0*/ 400, /*bandY1*/ 460, /*padY*/ 10, y0, y1);
        CHECK(y0 == 390);
        CHECK(y1 == 470);
    }

    SUBCASE("band at the TOP of the output box: padY reach is clamped up to the box, "
            "not down to some tighter bound (the srcBox-clip bug this replaces)")
    {
        int y0 = -1, y1 = -1;
        residualWindowYRange(0, 1000, /*bandY0*/ 0, /*bandY1*/ 60, /*padY*/ 25, y0, y1);
        CHECK(y0 == 0);      // clamped to the output box top, not -25
        CHECK(y1 == 85);
    }

    SUBCASE("band at the BOTTOM of the output box")
    {
        int y0 = -1, y1 = -1;
        residualWindowYRange(0, 1000, /*bandY0*/ 940, /*bandY1*/ 1000, /*padY*/ 25, y0, y1);
        CHECK(y0 == 915);
        CHECK(y1 == 1000);   // clamped to the output box bottom, not 1025
    }

    SUBCASE("a band exactly spanning the whole box, with padY: both ends clamp")
    {
        int y0 = -1, y1 = -1;
        residualWindowYRange(0, 100, 0, 100, 50, y0, y1);
        CHECK(y0 == 0);
        CHECK(y1 == 100);
    }
}

// A hand-built stand-in for computeBand()'s fetch loop, over a deep source
// whose bbox (`srcBox`) is DELIBERATELY TIGHTER than the output box on every
// side -- the "bloom over emptiness" case this fix exists for.  Every
// (x, y) inside `srcBox` reports a real, non-default flatten result; nothing
// outside it is ever visited, exactly like deepEngine() has no data there.
struct TightSrcBoxRig {
    int outX0 = 0, outX1 = 40, outY0 = 0, outY1 = 40;   // the output box
    int srcX0 = 10, srcX1 = 30, srcY0 = 10, srcY1 = 30;  // strictly inside it
    int bandY0 = 0, bandY1 = 40, padY = 5;
    float backgroundRadiusPx = 8.0f;

    // The value every SRCBOX pixel's flatten reports -- picked far from both
    // defaults (T=1, radius=backgroundRadiusPx) so a cell that accidentally
    // keeps its default is unambiguous.
    static constexpr float kSampleT      = 0.25f;
    static constexpr float kSampleRadius = 2.5f;

    ResidualWindow run() const
    {
        ResidualWindow window;
        std::size_t rowFetches = 0;
        const bool ok = buildResidualWindow(
            window, outX0, outX1, outY0, outY1, srcX0, srcX1, srcY0, srcY1,
            bandY0, bandY1, padY, backgroundRadiusPx,
            [&](int) -> bool { ++rowFetches; return true; },
            [&](int, int, float& t, float& r) -> bool {
                t = kSampleT;
                r = kSampleRadius;
                return true;
            });
        REQUIRE(ok);
        CHECK(rowFetches == static_cast<std::size_t>(srcY1 - srcY0));
        return window;
    }
};

TEST_CASE("buildResidualWindow: fetch-window pixels inside the output box but outside a "
          "TIGHT srcBox stay at T=1 and the background radius; srcBox pixels take the "
          "flatten's values")
{
    const TightSrcBoxRig rig;
    const ResidualWindow window = rig.run();

    // The window itself spans the OUTPUT box (clipped by padY at the edges,
    // which don't bind here), not srcBox -- this is the size half of the fix.
    CHECK(window.x == rig.outX0);
    CHECK(window.width == rig.outX1 - rig.outX0);
    REQUIRE(window.contains(rig.outX0, rig.outY0));
    REQUIRE(window.contains(rig.outX1 - 1, rig.outY1 - 1));

    int outsideChecked = 0, insideChecked = 0;
    for (int y = window.y; y < window.y + window.height; ++y) {
        for (int x = window.x; x < window.x + window.width; ++x) {
            const bool insideSrcBox = x >= rig.srcX0 && x < rig.srcX1
                                    && y >= rig.srcY0 && y < rig.srcY1;
            const std::ptrdiff_t i = window.index(x, y);
            if (insideSrcBox) {
                CHECK(window.t[static_cast<std::size_t>(i)] == TightSrcBoxRig::kSampleT);
                CHECK(window.radiusPx[static_cast<std::size_t>(i)]
                      == TightSrcBoxRig::kSampleRadius);
                ++insideChecked;
            } else {
                // THE ASSERTION THAT MATTERS: every fetch-window pixel inside
                // the output box but outside srcBox is a fully open virtual
                // background, not a hard edge.
                CHECK(window.t[static_cast<std::size_t>(i)] == 1.0f);
                CHECK(window.radiusPx[static_cast<std::size_t>(i)] == rig.backgroundRadiusPx);
                ++outsideChecked;
            }
        }
    }
    CHECK(insideChecked == (rig.srcX1 - rig.srcX0) * (rig.srcY1 - rig.srcY0));
    CHECK(outsideChecked > 0);   // the srcBox-outside region is non-empty in this rig
}

TEST_CASE("resolveBackgroundRadiusPx: 0 (and anything <= 0, and NaN) is auto -- the CoC at "
          "depthMax(); a manual value above rMax clamps to rMax")
{
    const float cocAtDepthMax = 6.0f;
    const float rMax          = 20.0f;

    SUBCASE("0 is auto")
    {
        CHECK(resolveBackgroundRadiusPx(0.0f, cocAtDepthMax, rMax) == cocAtDepthMax);
    }
    SUBCASE("negative is auto, same convention as clampf's NaN handling")
    {
        CHECK(resolveBackgroundRadiusPx(-3.0f, cocAtDepthMax, rMax) == cocAtDepthMax);
    }
    SUBCASE("NaN is auto")
    {
        CHECK(resolveBackgroundRadiusPx(std::numeric_limits<float>::quiet_NaN(),
                                        cocAtDepthMax, rMax) == cocAtDepthMax);
    }
    SUBCASE("a manual value within range passes through unchanged")
    {
        CHECK(resolveBackgroundRadiusPx(12.0f, cocAtDepthMax, rMax) == 12.0f);
    }
    SUBCASE("a manual value above rMax clamps to rMax")
    {
        CHECK(resolveBackgroundRadiusPx(500.0f, cocAtDepthMax, rMax) == rMax);
    }
    SUBCASE("a manual value exactly at rMax is unchanged")
    {
        CHECK(resolveBackgroundRadiusPx(rMax, cocAtDepthMax, rMax) == rMax);
    }
}

TEST_CASE("resolveFillSearchPx: 0 (and NaN) is auto -- 2*radiusPx + 1; a manual value clamps to "
          "maxRadiusPx")
{
    const float radiusPx    = 6.0f;
    const float maxRadiusPx = 20.0f;

    SUBCASE("0 is auto: 2*radiusPx + 1")
    {
        CHECK(resolveFillSearchPx(0.0f, radiusPx, maxRadiusPx) == 13.0f);
    }
    SUBCASE("NaN is auto")
    {
        CHECK(resolveFillSearchPx(std::numeric_limits<float>::quiet_NaN(),
                                  radiusPx, maxRadiusPx) == 13.0f);
    }
    SUBCASE("auto clamps to maxRadiusPx when 2*radiusPx + 1 would exceed it")
    {
        CHECK(resolveFillSearchPx(0.0f, 15.0f, maxRadiusPx) == maxRadiusPx);
    }
    SUBCASE("a manual value within range passes through unchanged")
    {
        CHECK(resolveFillSearchPx(9.0f, radiusPx, maxRadiusPx) == 9.0f);
    }
    SUBCASE("a manual value above maxRadiusPx clamps to maxRadiusPx")
    {
        CHECK(resolveFillSearchPx(500.0f, radiusPx, maxRadiusPx) == maxRadiusPx);
    }
}

// ===========================================================================
// The background fill's surface map
// ===========================================================================

namespace {

// The SampleView deepestSurface() reads, over a hand-built SampleRecord stack
// -- the doctest-side stand-in for the node's DeepPixel adapter.
struct VectorSamples {
    const std::vector<SampleRecord>* v = nullptr;

    int   count() const            { return v ? static_cast<int>(v->size()) : 0; }
    float zFront(int i) const      { return (*v)[static_cast<std::size_t>(i)].zFront; }
    float zBack(int i) const       { return (*v)[static_cast<std::size_t>(i)].zBack; }
    float alpha(int i) const       { return (*v)[static_cast<std::size_t>(i)].alpha; }
    float channel(int i, int c) const
    {
        return (*v)[static_cast<std::size_t>(i)].channels[static_cast<std::size_t>(c)];
    }
};

// A tidy-disjoint stack: point samples and volumetric spans laid out front to
// back with gaps, so tidyOverlapping() has nothing to cut and the flatten's
// last-staged fragment is unambiguously the deepest sample's.  Optionally
// followed by alpha-0 samples deeper than everything else, and shuffled so
// the map's scan order is never the flatten's sorted order.
std::vector<SampleRecord> fuzzDisjointStack(Lcg& rng, int n, int zeroAlphaTail,
                                            int channelCount)
{
    std::vector<SampleRecord> v;
    float z = rng.range(1.05f, 20.0f);
    for (int s = 0; s < n; ++s) {
        const bool  span      = (rng.unit() < 0.5f);
        const float thickness = span ? rng.range(0.5f, 12.0f) : 0.0f;
        const float a         = rng.range(0.001f, 1.0f);
        std::vector<float> ch;
        for (int c = 0; c < channelCount; ++c)
            ch.push_back(a * rng.range(0.0f, 1.0f));
        v.push_back(makeSample(z, z + thickness, a, ch));
        z += thickness + rng.range(0.25f, 6.0f);
    }
    for (int s = 0; s < zeroAlphaTail; ++s) {
        const float thickness = (rng.unit() < 0.5f) ? rng.range(0.5f, 3.0f) : 0.0f;
        std::vector<float> ch(static_cast<std::size_t>(channelCount), 0.0f);
        v.push_back(makeSample(z, z + thickness, 0.0f, ch));
        z += thickness + rng.range(0.25f, 2.0f);
    }
    for (std::size_t i = v.size(); i > 1; --i) {
        const std::size_t j = static_cast<std::size_t>(rng.intRange(0, static_cast<int>(i) - 1));
        std::swap(v[i - 1], v[j]);
    }
    return v;
}

// The flatten's own answer for a stack: residualRadiusPx and the last staged
// fragment's zBack, read off the scratch it leaves behind.
struct LastStaged {
    bool  any      = false;
    float residualT = 1.0f;
    float radiusPx = -1.0f;
    float zBack    = -1.0f;
};

LastStaged flattenLastStaged(const FlattenParams& fp,
                             int x, int y, std::vector<SampleRecord> v)
{
    SampleSoA soa;
    soa.begin(fp.channelCount, fp.groups);
    FlattenScratch scratch;
    LastStaged r;
    flattenPixelToSoA(fp, x, y, v, scratch, soa, nullptr,
                      &r.residualT, &r.radiusPx);
    r.any = scratch.stagedCount > 0u;
    if (r.any)
        r.zBack = scratch.staged[scratch.stagedCount - 1].zBack;
    return r;
}

} // namespace

TEST_CASE("deepestSurface: depth and radius equal flattenPixelToSoA()'s last-staged "
          "fragment and residualRadiusPx bit-exactly, fuzzed over tidy-disjoint stacks of "
          "point samples, volumetric spans and alpha-0 tails")
{
    Lcg rng(0x5EAFu);
    const CocParams    p  = makeStandardRig(10.0f);
    const int C = 3;

    SUBCASE("on the optical axis, no ray-distance correction")
    {
        const FlattenParams fp = makeFlattenParams(p, C, /*preMerge*/ true);
        int volumetricWinners = 0;
        for (int iter = 0; iter < 600; ++iter) {
            CAPTURE(iter);
            const std::vector<SampleRecord> v =
                fuzzDisjointStack(rng, rng.intRange(1, 6), rng.intRange(0, 2), C);

            const LastStaged staged = flattenLastStaged(fp, 0, 0, v);
            REQUIRE(staged.any);

            Surface s;
            const VectorSamples view{&v};
            const int i = deepestSurface(fp, 0, 0, view, s);
            REQUIRE(i >= 0);
            CHECK(s.radiusPx == staged.radiusPx);
            CHECK(s.zBack    == staged.zBack);

            // The winner is the raw stack's deepest alpha > 0 sample, carried
            // whole (its own span and alpha, not the staged tail part's).
            const SampleRecord& w = v[static_cast<std::size_t>(i)];
            CHECK(w.alpha > 0.0f);
            CHECK(s.alpha  == w.alpha);
            CHECK(s.zFront == w.zFront);
            CHECK(s.zBack  == w.zBack);
            for (std::size_t k = 0; k < v.size(); ++k)
                if (v[k].alpha > 0.0f)
                    CHECK(v[k].zBack <= w.zBack);
            if (w.zBack > w.zFront)
                ++volumetricWinners;
        }
        CHECK(volumetricWinners > 100);      // the span branch is exercised
    }

    SUBCASE("off-axis with depth_is_ray_distance: the per-pixel scale is applied")
    {
        FlattenParams fp = makeFlattenParams(p, C, /*preMerge*/ true);
        fp.depthIsRayDistance = true;
        fp.formatHeightPx     = 1080.0f;
        for (int iter = 0; iter < 200; ++iter) {
            CAPTURE(iter);
            const int x = rng.intRange(0, 1919);
            const int y = rng.intRange(0, 1079);
            const std::vector<SampleRecord> v =
                fuzzDisjointStack(rng, rng.intRange(1, 5), rng.intRange(0, 1), C);

            const LastStaged staged = flattenLastStaged(fp, x, y, v);
            REQUIRE(staged.any);

            Surface s;
            const VectorSamples view{&v};
            const int i = deepestSurface(fp, x, y, view, s);
            REQUIRE(i >= 0);
            CHECK(s.radiusPx == staged.radiusPx);
            CHECK(s.zBack    == staged.zBack);
            // The correction always shrinks depth off-axis, so an unscaled
            // map would read the raw zBack here and fail.
            CHECK(s.zBack < v[static_cast<std::size_t>(i)].zBack);
        }
    }

    SUBCASE("a span reaching past depthMax folds its tail parts like the flatten does")
    {
        const FlattenParams fp = makeFlattenParams(p, C, /*preMerge*/ false);
        const std::vector<SampleRecord> v{makeSample(0.2f, 400.0f, 0.6f, {0.1f, 0.2f, 0.3f})};
        const LastStaged staged = flattenLastStaged(fp, 0, 0, v);
        REQUIRE(staged.any);
        Surface s;
        const VectorSamples view{&v};
        REQUIRE(deepestSurface(fp, 0, 0, view, s) == 0);
        CHECK(s.radiusPx == staged.radiusPx);
        CHECK(s.zBack    == staged.zBack);
    }
}

TEST_CASE("deepestSurface: an all-alpha-0 pixel and an empty pixel both read empty; NaN "
          "and non-finite depths take the flatten's sanitising")
{
    const CocParams     p  = makeStandardRig(10.0f);
    const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ true);

    SUBCASE("empty")
    {
        const std::vector<SampleRecord> v;
        Surface s;
        CHECK(deepestSurface(fp, 0, 0, VectorSamples{&v}, s) == -1);
    }
    SUBCASE("all alpha 0, including a NaN alpha")
    {
        const std::vector<SampleRecord> v{
            makeSample(3.0f, 3.0f, 0.0f, {0.0f}),
            makeSample(5.0f, 9.0f, -0.5f, {0.0f}),
            makeSample(20.0f, 20.0f, std::numeric_limits<float>::quiet_NaN(), {0.0f})};
        Surface s;
        CHECK(deepestSurface(fp, 0, 0, VectorSamples{&v}, s) == -1);
        const LastStaged staged = flattenLastStaged(fp, 0, 0, v);
        CHECK(!staged.any);
    }
    SUBCASE("+inf zBack is kMaxDepth, a NaN front is 0, back-before-front collapses")
    {
        const std::vector<SampleRecord> v{
            makeSample(std::numeric_limits<float>::quiet_NaN(), 2.0f, 0.5f, {0.1f}),
            makeSample(9.0f, 4.0f, 0.5f, {0.1f}),
            makeSample(30.0f, std::numeric_limits<float>::infinity(), 0.5f, {0.1f})};
        Surface s;
        CHECK(deepestSurface(fp, 0, 0, VectorSamples{&v}, s) == 2);
        CHECK(s.zFront == 30.0f);
        CHECK(s.zBack  == FrameDepthRange::kMaxDepth);
        const LastStaged staged = flattenLastStaged(fp, 0, 0, v);
        REQUIRE(staged.any);
        CHECK(s.radiusPx == staged.radiusPx);
        CHECK(s.zBack    == staged.zBack);

        const std::vector<SampleRecord> collapsed{makeSample(9.0f, 4.0f, 0.5f, {0.1f})};
        CHECK(deepestSurface(fp, 0, 0, VectorSamples{&collapsed}, s) == 0);
        CHECK(s.zFront == 9.0f);
        CHECK(s.zBack  == 9.0f);
    }
    SUBCASE("alpha above 1 clamps, ties on zBack go to the later index")
    {
        const std::vector<SampleRecord> v{
            makeSample(7.0f, 7.0f, 3.0f, {0.1f}),
            makeSample(7.0f, 7.0f, 0.25f, {0.2f})};
        Surface s;
        CHECK(deepestSurface(fp, 0, 0, VectorSamples{&v}, s) == 1);
        CHECK(s.alpha == 0.25f);
        const std::vector<SampleRecord> one{makeSample(7.0f, 7.0f, 3.0f, {0.1f})};
        CHECK(deepestSurface(fp, 0, 0, VectorSamples{&one}, s) == 0);
        CHECK(s.alpha == 1.0f);
    }
}

TEST_CASE("SurfaceMap: bytesForWindow is (C+4) floats per pixel; surfaceMapFrameBytes adds the "
          "pyramid, and bandBudgetBytes carries neither -- the map is per frame, not per band")
{
    CHECK(SurfaceMap::bytesForWindow(4096, 64 + 2 * (101 + 33), 4) == 8u * 4096u * 332u * 4u);
    CHECK(SurfaceMap::bytesForWindow(4096, 64 + 2 * (101 + 33), 4) == 43515904u);
    CHECK(SurfaceMap::bytesForWindow(-1, 10, 4) == 0u);
    CHECK(SurfaceMap::bytesForWindow(10, 0, 4) == 0u);
    CHECK(SurfaceMap::bytesForWindow(10, 10, -3) == 4u * 100u * 4u);
    CHECK(SurfaceMap::bytesForWindow(10, 10, 4) == surfaceMapBytesForWindow(10, 10, 4));

    const double pyramid332 = static_cast<double>(MaxDepthPyramid::bytesForWindow(4096, 332));
    CHECK(pyramid332 == (1024.0 * 83.0 + 256.0 * 21.0 + 64.0 * 6.0 + 16.0 * 2.0 + 4.0 + 1.0) * 4.0);
    CHECK(pyramid332 == static_cast<double>(maxDepthPyramidBytesForWindow(4096, 332)));
    CHECK(pyramid332 < 43515904.0 / 8.0 / 14.0);

    // 2K rgba: the frame-wide map is ~70 MB; 4K ~280 MB.
    const std::size_t frame2k = surfaceMapFrameBytes(2048, 1080, 4);
    CHECK(frame2k == 8u * 2048u * 1080u * 4u + MaxDepthPyramid::bytesForWindow(2048, 1080));
    CHECK(frame2k > 70u * 1000u * 1000u);
    CHECK(frame2k < 76u * 1000u * 1000u);
    CHECK(surfaceMapFrameBytes(4096, 2160, 4) > 280u * 1000u * 1000u);
    CHECK(surfaceMapFrameBytes(4096, 2160, 4) < 300u * 1000u * 1000u);
    CHECK(surfaceMapFrameBytes(0, 2160, 4) == 0u);

    // The per-band budget is fill-mode blind: the map is not in it.
    const double fg = bandBudgetBytes(16, 4, 4096, 64, false, 0.0, 101);
    CHECK(fg == static_cast<double>(StreamPlanes::bytesForBand(4, 4096, 64))
                + static_cast<double>(ResidualWindow::bytesForWindow(4096, 64 + 2 * 101)));

    CHECK(fillReachPx(0.0f) == 0);
    CHECK(fillReachPx(-2.0f) == 0);
    CHECK(fillReachPx(std::numeric_limits<float>::quiet_NaN()) == 0);
    CHECK(fillReachPx(std::numeric_limits<float>::infinity()) == 0);
    CHECK(fillReachPx(16.0f) == 16);
    CHECK(fillReachPx(16.01f) == 17);
}

TEST_CASE("SurfaceMap: a fresh map has size 0 and release() returns it there, freeing the "
          "planes; the pyramid likewise")
{
    SurfaceMap map;
    CHECK(map.pixels() == 0);
    CHECK(map.planes.size() == 0u);
    map.allocate(0, 0, 4, 3, 2);
    CHECK(map.pixels() == 12);
    CHECK(map.planes.size() == 12u * 6u);
    for (std::ptrdiff_t i = 0; i < map.pixels(); ++i)
        CHECK(map.empty(i));

    MaxDepthPyramid pyramid;
    pyramid.build(map);
    CHECK(pyramid.levelCount() == 1);
    CHECK(pyramid._tiles.size() == 1u);
    pyramid.release();
    CHECK(pyramid.levelCount() == 0);
    CHECK(pyramid._tiles.size() == 0u);
    CHECK(pyramid._tiles.capacity() == 0u);

    map.release();
    CHECK(map.pixels() == 0);
    CHECK(map.planes.size() == 0u);
    CHECK(map.planes.capacity() == 0u);
    CHECK(!map.contains(0, 0));
}

TEST_CASE("SurfaceMap/MaxDepthPyramid bytes(): a map rebuilt smaller without release() keeps "
          "the larger capacity, so the frame budget must charge bytes(), after a release()")
{
    SurfaceMap map;
    MaxDepthPyramid pyramid;
    map.allocate(0, 0, 64, 64, 4);
    pyramid.build(map);
    CHECK(map.bytes() == SurfaceMap::bytesForWindow(64, 64, 4));
    CHECK(pyramid.bytes() == MaxDepthPyramid::bytesForWindow(64, 64));
    CHECK(map.bytes() + pyramid.bytes() == surfaceMapFrameBytes(64, 64, 4));

    map.allocate(0, 0, 8, 8, 4);
    pyramid.build(map);
    CHECK(map.bytes() == SurfaceMap::bytesForWindow(64, 64, 4));
    CHECK(map.bytes() > SurfaceMap::bytesForWindow(8, 8, 4));
    CHECK(pyramid.bytes() == MaxDepthPyramid::bytesForWindow(64, 64));

    map.release();
    pyramid.release();
    CHECK(map.bytes() == 0u);
    CHECK(pyramid.bytes() == 0u);
    map.allocate(0, 0, 8, 8, 4);
    pyramid.build(map);
    CHECK(map.bytes() + pyramid.bytes() == surfaceMapFrameBytes(8, 8, 4));
}

TEST_CASE("buildSurfaceMap: the extended window is band +/- (padY + reach) clipped to the "
          "output box; only srcBox rows are fetched; cells outside srcBox read empty")
{
    const CocParams     p  = makeStandardRig(10.0f);
    const int C = 2;
    const FlattenParams fp = makeFlattenParams(p, C, /*preMerge*/ true);

    // Output box 40x40; srcBox strictly inside; a band in the middle whose
    // padY + reach reaches past srcBox on both sides but stays inside the
    // output box at the bottom and clips at the top.
    const int outX0 = 0, outX1 = 40, outY0 = 0, outY1 = 40;
    const int srcX0 = 10, srcX1 = 30, srcY0 = 8, srcY1 = 34;
    const int bandY0 = 16, bandY1 = 24, padY = 5, reach = 12;

    // Every srcBox pixel carries one point sample whose depth encodes (x, y)
    // and an alpha-0 sample deeper than it; pixels on one column carry
    // nothing at all.
    const auto stackAt = [&](int x, int y) {
        std::vector<SampleRecord> v;
        if (x == 17)
            return v;
        const float z = 2.0f + 0.01f * static_cast<float>(x) + 0.5f * static_cast<float>(y);
        v.push_back(makeSample(z + 50.0f, z + 50.0f, 0.0f, {0.0f, 0.0f}));
        v.push_back(makeSample(z, z, 0.5f, {0.1f * static_cast<float>(x), 0.2f * static_cast<float>(y)}));
        return v;
    };

    SurfaceMap map;
    std::vector<int> rowsFetched;
    std::vector<SampleRecord> row;    // the "fetched" pixel, rebuilt per column
    const bool ok = buildSurfaceMap(
        map, fp, outX0, outX1, outY0, outY1, srcX0, srcX1, srcY0, srcY1,
        bandY0, bandY1, padY, reach, C,
        [&](int y) -> bool { rowsFetched.push_back(y); return true; },
        [&](int x, int y) -> VectorSamples {
            row = stackAt(x, y);
            return VectorSamples{&row};
        });
    REQUIRE(ok);

    // Extent: rows [16-17, 24+17) = [-1, 41) clipped to [0, 40); full X.
    CHECK(map.x == outX0);
    CHECK(map.width == outX1 - outX0);
    CHECK(map.y == 0);
    CHECK(map.height == 40);
    CHECK(map.channelCount == C);

    // Visiting: srcBox rows only, [8, 34), in order.
    REQUIRE(rowsFetched.size() == static_cast<std::size_t>(srcY1 - srcY0));
    for (std::size_t i = 0; i < rowsFetched.size(); ++i)
        CHECK(rowsFetched[i] == srcY0 + static_cast<int>(i));

    int filled = 0, emptyInside = 0, emptyOutside = 0;
    for (int y = map.y; y < map.y + map.height; ++y) {
        for (int x = map.x; x < map.x + map.width; ++x) {
            const std::ptrdiff_t i = map.index(x, y);
            const bool inSrc = x >= srcX0 && x < srcX1 && y >= srcY0 && y < srcY1;
            if (!inSrc) {
                CHECK(map.empty(i));
                ++emptyOutside;
                continue;
            }
            if (x == 17) {
                CHECK(map.empty(i));
                ++emptyInside;
                continue;
            }
            const std::vector<SampleRecord> v = stackAt(x, y);
            const LastStaged staged = flattenLastStaged(fp, x, y, v);
            REQUIRE(staged.any);
            CHECK(!map.empty(i));
            CHECK(map.plane(SurfaceMap::kZFront)[i] == v[1].zFront);
            CHECK(map.plane(SurfaceMap::kZBack)[i]  == staged.zBack);
            CHECK(map.plane(SurfaceMap::kAlpha)[i]  == 0.5f);
            CHECK(map.plane(SurfaceMap::kRadius)[i] == staged.radiusPx);
            CHECK(map.plane(SurfaceMap::kChannel0)[i]     == v[1].channels[0]);
            CHECK(map.plane(SurfaceMap::kChannel0 + 1)[i] == v[1].channels[1]);
            ++filled;
        }
    }
    CHECK(filled == (srcX1 - srcX0 - 1) * (srcY1 - srcY0));
    CHECK(emptyInside == (srcY1 - srcY0));
    CHECK(emptyOutside == 40 * 40 - (srcX1 - srcX0) * (srcY1 - srcY0));

    SUBCASE("a band near the bottom: the extent runs past srcBox to row 39 while "
            "visiting stops at srcBox's last row")
    {
        rowsFetched.clear();
        const bool ok2 = buildSurfaceMap(
            map, fp, outX0, outX1, outY0, outY1, srcX0, srcX1, srcY0, srcY1,
            /*bandY0*/ 30, /*bandY1*/ 36, /*padY*/ 2, /*reach*/ 1, C,
            [&](int y) -> bool { rowsFetched.push_back(y); return true; },
            [&](int x, int y) -> VectorSamples {
                row = stackAt(x, y);
                return VectorSamples{&row};
            });
        REQUIRE(ok2);
        CHECK(map.y == 27);
        CHECK(map.height == 39 - 27);
        REQUIRE(rowsFetched.size() == 7u);       // [27, 34)
        CHECK(rowsFetched.front() == 27);
        CHECK(rowsFetched.back() == 33);
    }

    SUBCASE("a failing fetch aborts the build")
    {
        const bool ok3 = buildSurfaceMap(
            map, fp, outX0, outX1, outY0, outY1, srcX0, srcX1, srcY0, srcY1,
            bandY0, bandY1, padY, reach, C,
            [&](int y) -> bool { return y < 12; },
            [&](int x, int y) -> VectorSamples {
                row = stackAt(x, y);
                return VectorSamples{&row};
            });
        CHECK(!ok3);
    }
}

// ===========================================================================
// Background fill: the depth-aware nearest-source search
// ===========================================================================

namespace {

// Scene (m)'s rigs, rebuilt from their constants rather than rendered: the
// halo card (m1) and the receding ground plane (m3), on the plugin's own CoC
// law so the map's radii are the ones the flatten would stage.
constexpr int   kFillRigSize   = 256;
constexpr int   kHaloX0        = 80;
constexpr int   kHaloX1        = 176;
constexpr float kHaloNearZ     = 4.0f;
constexpr float kHaloFarZ      = 20.0f;
constexpr float kNearCardZ     = 3.0f;
constexpr int   kNearCardX1    = 192;
constexpr float kGroundC       = 1720.0f;
constexpr float kGroundHorizon = 300.0f;
constexpr int   kRampInterior0 = 66;
constexpr int   kRampInterior1 = 190;

float groundDepth(int y)
{
    return kGroundC / (kGroundHorizon - static_cast<float>(y));
}

bool inHaloCard(int x, int y)
{
    return x >= kHaloX0 && x < kHaloX1 && y >= kHaloX0 && y < kHaloX1;
}

bool inNearCard(int x, int y)
{
    return x >= kHaloX1 && x < kNearCardX1 && y >= kHaloX0 && y < kHaloX1;
}

struct FillRig {
    CocParams     coc;
    HoldoutBoundaries holdoutSet;
    float         depthMax = 0.0f;
    FlattenParams fp;
};

FillRig makeHaloFillRig()
{
    FillRig r;
    r.coc     = makeManualRig(4.0f, kHaloFarZ);
    r.holdoutSet = makeHoldoutBoundaries(r.coc, kNearCardZ, kHaloFarZ, 16);
    r.depthMax   = kHaloFarZ;
    r.fp      = makeFlattenParams(r.coc, 1, true);
    return r;
}

FillRig makeRampFillRig()
{
    FillRig r;
    r.coc     = makeManualRig(86.0f, 10.0f);
    r.holdoutSet = makeHoldoutBoundaries(r.coc, groundDepth(0), groundDepth(kFillRigSize - 1), 16);
    r.depthMax   = groundDepth(kFillRigSize - 1);
    r.fp      = makeFlattenParams(r.coc, 1, true);
    return r;
}

std::vector<SampleRecord> haloStack(int x, int y, bool nearCard)
{
    if (inHaloCard(x, y))
        return {makeSample(kHaloNearZ, kHaloNearZ, 1.0f, {0.8f})};
    if (nearCard && inNearCard(x, y))
        return {makeSample(kNearCardZ, kNearCardZ, 1.0f, {0.5f})};
    return {makeSample(kHaloFarZ, kHaloFarZ, 1.0f, {0.2f})};
}

std::vector<SampleRecord> rampStack(int, int y, float alpha)
{
    const float z = groundDepth(y);
    return {makeSample(z, z, alpha, {0.4f * alpha})};
}

template <typename StackFn>
void buildRigMap(SurfaceMap& map, const FillRig& rig,
                 int bandY0, int bandY1, int padY, int reach, StackFn&& stackAt)
{
    std::vector<SampleRecord> row;
    const bool ok = buildSurfaceMap(
        map, rig.fp,
        0, kFillRigSize, 0, kFillRigSize,
        0, kFillRigSize, 0, kFillRigSize,
        bandY0, bandY1, padY, reach, 1,
        [](int) { return true; },
        [&](int x, int y) -> VectorSamples {
            row = stackAt(x, y);
            return VectorSamples{&row};
        });
    REQUIRE(ok);
}

void buildFullFrameMap(SurfaceMap& map, MaxDepthPyramid& pyramid, const FillRig& rig,
                       const std::function<std::vector<SampleRecord>(int, int)>& stackAt)
{
    buildRigMap(map, rig, 0, kFillRigSize, 0, 0, stackAt);
    pyramid.build(map);
}

// Independent restatement of the step rule: per axis the smaller of the two
// opposite-neighbour |dz|, an absent neighbour deferring to the other side.
float refLocalStep(const SurfaceMap& map, int x, int y)
{
    const float* zb = map.plane(SurfaceMap::kZBack);
    const float  zP = zb[map.index(x, y)];
    float g = 0.0f;
    const int dirs[2][2] = {{1, 0}, {0, 1}};
    for (const auto& d : dirs) {
        std::vector<float> steps;
        for (int s = -1; s <= 1; s += 2) {
            const int nx = x + s * d[0], ny = y + s * d[1];
            if (map.contains(nx, ny) && !map.empty(map.index(nx, ny)))
                steps.push_back(std::fabs(zP - zb[map.index(nx, ny)]));
        }
        float axis = 0.0f;
        if (!steps.empty())
            axis = *std::min_element(steps.begin(), steps.end());
        g = std::max(g, axis);
    }
    return g;
}

float refThreshold(const FillPredicate& pred, float zP, float g, float d)
{
    return zP + std::max(pred.depthTol * zP, pred.slope * g * d);
}

BackgroundSource bruteForceNearest(const SurfaceMap& map, int x, int y, int reach,
                                   const FillPredicate& pred)
{
    BackgroundSource best;
    const std::ptrdiff_t iP = map.index(x, y);
    if (map.empty(iP))
        return best;
    const float zP = map.plane(SurfaceMap::kZBack)[iP];
    const float g  = refLocalStep(map, x, y);
    std::int64_t bestD2 = static_cast<std::int64_t>(reach) * reach + 1;
    for (int qy = y - reach; qy <= y + reach; ++qy) {
        for (int qx = x - reach; qx <= x + reach; ++qx) {
            if (!map.contains(qx, qy))
                continue;
            const std::int64_t dx = qx - x, dy = qy - y;
            const std::int64_t d2 = dx * dx + dy * dy;
            if (d2 > bestD2 || d2 > static_cast<std::int64_t>(reach) * reach)
                continue;
            if (d2 == bestD2 && (qy > best.qy || (qy == best.qy && qx > best.qx)))
                continue;
            const std::ptrdiff_t iQ = map.index(qx, qy);
            if (map.empty(iQ))
                continue;
            const float zQ = map.plane(SurfaceMap::kZFront)[iQ];
            if (!(zQ > refThreshold(pred, zP, g, std::sqrt(static_cast<float>(d2)))))
                continue;
            bestD2 = d2;
            best.found = true;
            best.qx = qx;
            best.qy = qy;
            best.distance = std::sqrt(static_cast<float>(d2));
        }
    }
    return best;
}

BackgroundSource bruteForceTiered(const SurfaceMap& map, int x, int y,
                                  int primary, int fallback, const FillPredicate& pred)
{
    BackgroundSource r = bruteForceNearest(map, x, y, primary, pred);
    if (!r.found && fallback > primary)
        r = bruteForceNearest(map, x, y, fallback, pred);
    return r;
}

bool sameSource(const BackgroundSource& a, const BackgroundSource& b)
{
    return a.found == b.found && (!a.found || (a.qx == b.qx && a.qy == b.qy && a.distance == b.distance));
}

// The margins each rig's intended decision clears the predicate by.
struct RigMargins {
    float haloAcceptBg  = 0.0f;
    float rampReject    = 0.0f;
    float nearRejectFg  = 0.0f;
    float nearAcceptBg  = 0.0f;
};

float haloAcceptMargin(const SurfaceMap& map, const MaxDepthPyramid& pyr,
                       const FillPredicate& pred, int reach)
{
    float worst = std::numeric_limits<float>::infinity();
    for (int y = kHaloX0; y < kHaloX1; ++y) {
        for (int x = kHaloX0; x < kHaloX1; ++x) {
            const BackgroundSource s = findBackgroundSource(map, pyr, x, y, reach, reach, pred);
            REQUIRE(s.found);
            const float zP = map.plane(SurfaceMap::kZBack)[map.index(x, y)];
            worst = std::min(worst, kHaloFarZ - refThreshold(pred, zP, refLocalStep(map, x, y), s.distance));
        }
    }
    return worst;
}

float rampRejectMargin(const SurfaceMap& map, const FillPredicate& pred, int reach)
{
    // The ramp is x-invariant and the threshold grows with distance, so the
    // closest-to-qualifying Q for any P lies in P's own column.
    float worst = std::numeric_limits<float>::infinity();
    const int x = kFillRigSize / 2;
    for (int y = kRampInterior0; y < kRampInterior1; ++y) {
        const float zP = map.plane(SurfaceMap::kZBack)[map.index(x, y)];
        const float g  = refLocalStep(map, x, y);
        for (int qy = std::max(0, y - reach); qy < std::min(kFillRigSize, y + reach + 1); ++qy) {
            if (qy == y)
                continue;
            const float zQ = map.plane(SurfaceMap::kZFront)[map.index(x, qy)];
            worst = std::min(worst, refThreshold(pred, zP, g, static_cast<float>(std::abs(qy - y))) - zQ);
        }
    }
    return worst;
}

float nearCardRejectMargin(const SurfaceMap& map, const FillPredicate& pred, int reach)
{
    float worst = std::numeric_limits<float>::infinity();
    for (int y = kHaloX0; y < kHaloX1; ++y) {
        for (int x = kHaloX0; x < kHaloX1; ++x) {
            const float zP = map.plane(SurfaceMap::kZBack)[map.index(x, y)];
            const float g  = refLocalStep(map, x, y);
            for (int qy = kHaloX0; qy < kHaloX1; ++qy) {
                for (int qx = kHaloX1; qx < kNearCardX1; ++qx) {
                    const float dx = static_cast<float>(qx - x), dy = static_cast<float>(qy - y);
                    const float d  = std::sqrt(dx * dx + dy * dy);
                    if (d > static_cast<float>(reach))
                        continue;
                    worst = std::min(worst, refThreshold(pred, zP, g, d) - kNearCardZ);
                }
            }
        }
    }
    return worst;
}

} // namespace

TEST_CASE("fill predicate constants: margins on the halo card, the ramp and the near-card "
          "control, for the shipped pair and the sweep around it")
{
    const FillRig halo = makeHaloFillRig();
    const FillRig ramp = makeRampFillRig();

    SurfaceMap haloMap, nearMap, rampMap;
    MaxDepthPyramid haloPyr, nearPyr, rampPyr;
    buildFullFrameMap(haloMap, haloPyr, halo, [](int x, int y) { return haloStack(x, y, false); });
    buildFullFrameMap(nearMap, nearPyr, halo, [](int x, int y) { return haloStack(x, y, true); });
    buildFullFrameMap(rampMap, rampPyr, ramp, [](int x, int y) { return rampStack(x, y, 1.0f); });

    CHECK(haloMap.plane(SurfaceMap::kRadius)[haloMap.index(100, 100)] == 16.0f);
    CHECK(haloMap.plane(SurfaceMap::kRadius)[haloMap.index(10, 10)]   == 0.0f);
    CHECK(rampMap.plane(SurfaceMap::kRadius)[rampMap.index(10, 128)]  == 0.0f);
    CHECK(rampMap.plane(SurfaceMap::kRadius)[rampMap.index(10, 0)]    == doctest::Approx(64.0f).epsilon(1e-4));

    for (int y = 0; y < kFillRigSize; y += 7)
        for (int x = 0; x < kFillRigSize; x += 5) {
            CHECK(localDepthStep(haloMap, x, y) == refLocalStep(haloMap, x, y));
            CHECK(localDepthStep(nearMap, x, y) == refLocalStep(nearMap, x, y));
            CHECK(localDepthStep(rampMap, x, y) == refLocalStep(rampMap, x, y));
        }
    CHECK(localDepthStep(haloMap, kHaloX0, 100) == 0.0f);
    CHECK(localDepthStep(haloMap, kHaloX1 - 1, kHaloX1 - 1) == 0.0f);
    CHECK(localDepthStep(nearMap, kHaloX1, 100) == 0.0f);
    CHECK(localDepthStep(rampMap, 100, 100) == doctest::Approx(groundDepth(100) - groundDepth(99)));

    const int reach = 100;
    const float tols[]   = {0.01f, 0.02f, 0.05f};
    const float slopes[] = {2.0f, 4.0f, 8.0f};
    std::printf("\nfill predicate margins (reach %d px): tol slope | halo-accept-bg  ramp-reject  "
                "near-reject-fg  near-accept-bg\n", reach);
    for (float tol : tols) {
        for (float slope : slopes) {
            const FillPredicate pred{tol, slope};
            RigMargins m;
            m.haloAcceptBg = haloAcceptMargin(haloMap, haloPyr, pred, reach);
            m.rampReject   = rampRejectMargin(rampMap, pred, reach);
            m.nearRejectFg = nearCardRejectMargin(nearMap, pred, reach);
            m.nearAcceptBg = haloAcceptMargin(nearMap, nearPyr, pred, reach);
            std::printf("  %.2f  %4.1f  | %14.3f  %11.3f  %14.3f  %14.3f\n",
                        tol, slope, m.haloAcceptBg, m.rampReject, m.nearRejectFg, m.nearAcceptBg);
            if (slope == 2.0f)
                CHECK(m.rampReject < 0.0f);
        }
    }

    const FillPredicate shipped;
    CHECK(shipped.depthTol == kFillDepthTol);
    CHECK(shipped.slope == kFillSlope);
    CHECK(haloAcceptMargin(haloMap, haloPyr, shipped, reach) > 15.0f);
    CHECK(rampRejectMargin(rampMap, shipped, reach) > 0.1f);
    CHECK(nearCardRejectMargin(nearMap, shipped, reach) > 1.0f);
    CHECK(haloAcceptMargin(nearMap, nearPyr, shipped, reach) > 15.0f);
}

TEST_CASE("findBackgroundSource: every halo-card pixel finds the nearest background pixel, "
          "matching a brute-force scan of the same predicate")
{
    const FillRig halo = makeHaloFillRig();
    SurfaceMap map;
    MaxDepthPyramid pyr;
    buildFullFrameMap(map, pyr, halo, [](int x, int y) { return haloStack(x, y, false); });
    CHECK(pyr.levelCount() == 4);

    const int primary = 33, fallback = 100;
    FillSearchStats stats;
    int queries = 0;
    for (int y = kHaloX0; y < kHaloX1; ++y) {
        for (int x = kHaloX0; x < kHaloX1; ++x) {
            const BackgroundSource s = findBackgroundSource(map, pyr, x, y, primary, fallback,
                                                            FillPredicate(), &stats);
            ++queries;
            REQUIRE(s.found);
            const int toEdge = std::min(std::min(x - kHaloX0 + 1, kHaloX1 - x),
                                        std::min(y - kHaloX0 + 1, kHaloX1 - y));
            CHECK(s.distance == static_cast<float>(toEdge));
            CHECK(map.plane(SurfaceMap::kZFront)[map.index(s.qx, s.qy)] == kHaloFarZ);
            const bool onRing = (x == kHaloX0 || x == kHaloX1 - 1 || y == kHaloX0 || y == kHaloX1 - 1);
            if (onRing || ((x - kHaloX0) % 4 == 0 && (y - kHaloX0) % 4 == 0)) {
                const BackgroundSource ref = bruteForceTiered(map, x, y, primary, fallback, FillPredicate());
                CHECK(sameSource(s, ref));
            }
        }
    }
    std::printf("\nhalo card search cost: %d queries, %.2f tiles and %.2f leaves per query\n",
                queries, static_cast<double>(stats.tilesVisited) / queries,
                static_cast<double>(stats.leavesVisited) / queries);
    CHECK(stats.leavesVisited < queries * 48);

    SUBCASE("background pixels find nothing: the root tile's max is their own depth")
    {
        FillSearchStats bg;
        for (int y = 0; y < kFillRigSize; y += 3) {
            for (int x = 0; x < kFillRigSize; x += 3) {
                if (inHaloCard(x, y))
                    continue;
                CHECK(!findBackgroundSource(map, pyr, x, y, primary, fallback, FillPredicate(), &bg).found);
            }
        }
        CHECK(bg.leavesVisited == 0);
    }

    SUBCASE("an empty or out-of-map query is not found")
    {
        CHECK(!findBackgroundSource(map, pyr, -1, 10, primary, fallback).found);
        CHECK(!findBackgroundSource(map, pyr, 10, kFillRigSize, primary, fallback).found);
        SurfaceMap sparse;
        MaxDepthPyramid sparsePyr;
        buildFullFrameMap(sparse, sparsePyr, halo, [](int x, int y) {
            return inHaloCard(x, y) ? std::vector<SampleRecord>() : haloStack(x, y, false);
        });
        CHECK(!findBackgroundSource(sparse, sparsePyr, 100, 100, primary, fallback).found);
        CHECK(!findBackgroundSource(sparse, sparsePyr, 10, 100, primary, fallback).found);
    }
}

TEST_CASE("findBackgroundSource: a nearer card beside the hole is never chosen, and the "
          "background is still found around it")
{
    const FillRig halo = makeHaloFillRig();
    SurfaceMap map;
    MaxDepthPyramid pyr;
    buildFullFrameMap(map, pyr, halo, [](int x, int y) { return haloStack(x, y, true); });

    const int primary = 33, fallback = 100;
    for (int y = kHaloX0; y < kHaloX1; ++y) {
        for (int x = kHaloX0; x < kHaloX1; ++x) {
            const BackgroundSource s = findBackgroundSource(map, pyr, x, y, primary, fallback);
            REQUIRE(s.found);
            CHECK(!inHaloCard(s.qx, s.qy));
            CHECK(!inNearCard(s.qx, s.qy));
            CHECK(map.plane(SurfaceMap::kZFront)[map.index(s.qx, s.qy)] == kHaloFarZ);
            if ((x % 4 == 0 && y % 4 == 0) || x == kHaloX1 - 1) {
                const BackgroundSource ref = bruteForceTiered(map, x, y, primary, fallback, FillPredicate());
                CHECK(sameSource(s, ref));
            }
        }
    }
    const BackgroundSource edge = findBackgroundSource(map, pyr, kHaloX1 - 1, 128, primary, fallback);
    CHECK(edge.distance == 17.0f);
    CHECK(edge.qx == kNearCardX1);

    SUBCASE("the near card itself sees the halo card as ITS background: the rule is depth "
            "order, not identity")
    {
        const BackgroundSource s = findBackgroundSource(map, pyr, kHaloX1, 128, primary, fallback);
        REQUIRE(s.found);
        CHECK(s.distance == 1.0f);
        CHECK(s.qx == kHaloX1 - 1);
        CHECK(inHaloCard(s.qx, s.qy));
        CHECK(sameSource(s, bruteForceTiered(map, kHaloX1, 128, primary, fallback, FillPredicate())));
    }
}

TEST_CASE("findBackgroundSource: a receding plane finds nothing from any interior pixel, "
          "opaque and at alpha 0.9; with the slope rule off it self-fills")
{
    const FillRig ramp = makeRampFillRig();
    const int reach = 100;
    for (float alpha : {1.0f, 0.9f}) {
        CAPTURE(alpha);
        SurfaceMap map;
        MaxDepthPyramid pyr;
        buildFullFrameMap(map, pyr, ramp, [alpha](int x, int y) { return rampStack(x, y, alpha); });

        int found = 0;
        for (int y = kRampInterior0; y < kRampInterior1; ++y)
            for (int x = kRampInterior0; x < kRampInterior1; ++x)
                found += findBackgroundSource(map, pyr, x, y, reach, reach).found ? 1 : 0;
        CHECK(found == 0);

        const FillPredicate noSlope{kFillDepthTol, 0.0f};
        int selfFilled = 0, deeperRow = 0;
        for (int y = kRampInterior0; y < kRampInterior1; ++y) {
            for (int x = kRampInterior0; x < kRampInterior1; ++x) {
                const BackgroundSource s = findBackgroundSource(map, pyr, x, y, reach, reach, noSlope);
                selfFilled += s.found ? 1 : 0;
                deeperRow  += (s.found && s.qy > y) ? 1 : 0;
                if (x == 128 && y % 16 == 0)
                    CHECK(sameSource(s, bruteForceTiered(map, x, y, reach, reach, noSlope)));
            }
        }
        const int interior = (kRampInterior1 - kRampInterior0) * (kRampInterior1 - kRampInterior0);
        CHECK(selfFilled == interior);
        CHECK(deeperRow == interior);
    }
}

TEST_CASE("findBackgroundSource: a uniform-depth field answers none at the root, visiting "
          "no leaf")
{
    const FillRig halo = makeHaloFillRig();
    SurfaceMap map;
    MaxDepthPyramid pyr;
    buildFullFrameMap(map, pyr, halo, [](int, int) {
        return std::vector<SampleRecord>{makeSample(kHaloFarZ, kHaloFarZ, 1.0f, {0.2f})};
    });
    REQUIRE(pyr.levelCount() == 4);
    CHECK(pyr.width(4) == 1);
    CHECK(pyr.height(4) == 1);
    CHECK(pyr.level(4)[0] == kHaloFarZ);
    CHECK(pyr.width(1) == 64);
    CHECK(MaxDepthPyramid::bytesForWindow(kFillRigSize, kFillRigSize)
          == (64u * 64u + 16u * 16u + 4u * 4u + 1u) * sizeof(float));

    FillSearchStats stats;
    int queries = 0;
    for (int y = 0; y < kFillRigSize; y += 5) {
        for (int x = 0; x < kFillRigSize; x += 5) {
            CHECK(!findBackgroundSource(map, pyr, x, y, 100, 100, FillPredicate(), &stats).found);
            ++queries;
        }
    }
    CHECK(stats.tilesVisited == queries);
    CHECK(stats.leavesVisited == 0);
    std::printf("\nuniform field search cost: %d queries, %d tiles, %d leaves\n",
                queries, stats.tilesVisited, stats.leavesVisited);

    SUBCASE("a two-tier query on the same field costs one root visit per tier")
    {
        FillSearchStats two;
        CHECK(!findBackgroundSource(map, pyr, 40, 40, 2, 100, FillPredicate(), &two).found);
        CHECK(two.tilesVisited == 2);
        CHECK(two.leavesVisited == 0);
    }
}

TEST_CASE("findBackgroundSource: the fallback tier reaches what the primary cannot")
{
    const FillRig halo = makeHaloFillRig();
    SurfaceMap map;
    MaxDepthPyramid pyr;
    buildFullFrameMap(map, pyr, halo, [](int x, int y) { return haloStack(x, y, false); });

    const int x = kHaloX0 + 10, y = 128;
    const BackgroundSource far = findBackgroundSource(map, pyr, x, y, 2, 100);
    REQUIRE(far.found);
    CHECK(far.distance == 11.0f);
    CHECK(far.qx == kHaloX0 - 1);
    CHECK(far.qy == y);

    CHECK(!findBackgroundSource(map, pyr, x, y, 2, 2).found);
    CHECK(!findBackgroundSource(map, pyr, x, y, 2, 10).found);
    CHECK(findBackgroundSource(map, pyr, x, y, 2, 11).found);
    CHECK(findBackgroundSource(map, pyr, x, y, 11, 11).found);
    CHECK(findBackgroundSource(map, pyr, x, y, 100, 2).found);
}

TEST_CASE("findBackgroundSource: band invariance -- two maps windowed differently around a "
          "pixel's full reach disc answer identically, and the frame-wide map the node builds "
          "answers as both of them do, from every band")
{
    const FillRig halo = makeHaloFillRig();
    FillScratch fillScratch;
    const int primary = 33, fallback = 60;

    SurfaceMap wide, narrow, frame;
    MaxDepthPyramid widePyr, narrowPyr, framePyr;
    buildRigMap(wide, halo, 100, 140, 5, fallback, [](int x, int y) { return haloStack(x, y, false); });
    buildRigMap(narrow, halo, 120, 124, 2, fallback, [](int x, int y) { return haloStack(x, y, false); });
    buildFullFrameMap(frame, framePyr, halo, [](int x, int y) { return haloStack(x, y, false); });
    widePyr.build(wide);
    narrowPyr.build(narrow);
    REQUIRE(wide.y == 35);
    REQUIRE(wide.height == 170);
    REQUIRE(narrow.y == 58);
    REQUIRE(narrow.height == 128);
    REQUIRE(frame.y == 0);
    REQUIRE(frame.height == kFillRigSize);
    CHECK(widePyr.levelCount() == 4);
    CHECK(narrowPyr.levelCount() == 4);
    CHECK(framePyr.levelCount() == 4);

    for (int y = 120; y < 124; ++y) {
        for (int x = kHaloX0; x < kHaloX1; ++x) {
            const BackgroundSource a = findBackgroundSource(wide, widePyr, x, y, primary, fallback);
            const BackgroundSource b = findBackgroundSource(narrow, narrowPyr, x, y, primary, fallback);
            const BackgroundSource c = findBackgroundSource(frame, framePyr, x, y, primary, fallback);
            REQUIRE(a.found);
            CHECK(sameSource(a, b));
            CHECK(a.qx == b.qx);
            CHECK(a.qy == b.qy);
            CHECK(a.distance == b.distance);
            CHECK(sameSource(a, c));
            CHECK(a.qx == c.qx);
            CHECK(a.qy == c.qy);
            CHECK(a.distance == c.distance);
        }
    }

    const BackgroundSource a = findBackgroundSource(wide, widePyr, 120, 122, primary, fallback);
    CHECK(a.distance == 41.0f);
    CHECK(a.qx == kHaloX0 - 1);
    CHECK(a.qy == 122);

    // Two disjoint bands, each with the band-windowed map the node once
    // built per band (band +/- (padY + fallback)), against the one
    // frame-wide map: the whole synthesis -- search, prune, smear -- lands
    // the same bits whichever band asks and whichever map it reads.
    const int padY = 17;
    const int bandY[2][2] = {{96, 128}, {160, 192}};
    int appended = 0;
    for (int band = 0; band < 2; ++band) {
        SurfaceMap windowed;
        MaxDepthPyramid windowedPyr;
        buildRigMap(windowed, halo, bandY[band][0], bandY[band][1], padY, fallback,
                    [](int x, int y) { return haloStack(x, y, false); });
        windowedPyr.build(windowed);
        REQUIRE(windowed.y != frame.y);
        for (bool smear : {false, true}) {
            for (int y = bandY[band][0] - padY; y < bandY[band][1] + padY; ++y) {
                for (int x = 0; x < kFillRigSize; ++x) {
                    std::vector<SampleRecord> fromWindow = haloStack(x, y, false);
                    std::vector<SampleRecord> fromFrame  = haloStack(x, y, false);
                    const bool w = appendHiddenBackground(windowed, windowedPyr, halo.fp, x, y,
                                                          0.0f, static_cast<float>(fallback),
                                                          smear, fromWindow, fillScratch);
                    const bool f = appendHiddenBackground(frame, framePyr, halo.fp, x, y,
                                                          0.0f, static_cast<float>(fallback),
                                                          smear, fromFrame, fillScratch);
                    REQUIRE(w == f);
                    REQUIRE(fromWindow.size() == fromFrame.size());
                    for (std::size_t i = 0; i < fromWindow.size(); ++i) {
                        REQUIRE(fromWindow[i].zFront == fromFrame[i].zFront);
                        REQUIRE(fromWindow[i].zBack  == fromFrame[i].zBack);
                        REQUIRE(fromWindow[i].alpha  == fromFrame[i].alpha);
                        REQUIRE(fromWindow[i].channels == fromFrame[i].channels);
                    }
                    appended += w ? 1 : 0;
                }
            }
        }
    }
    CHECK(appended > 0);
}

TEST_CASE("pruneSynthesis: an opaque P is pruned iff the source's disc is at least its own; "
          "a semi-transparent P never is")
{
    const float rP = 16.0f;
    CHECK(pruneSynthesis(0.0f, rP, rP));
    CHECK(pruneSynthesis(0.0f, rP, rP + 0.001f));
    CHECK(!pruneSynthesis(0.0f, rP, rP - 0.001f));
    CHECK(!pruneSynthesis(0.0f, rP, 0.0f));
    CHECK(pruneSynthesis(kFillDeficitTol, rP, rP));
    CHECK(!pruneSynthesis(kFillDeficitTol * 2.0f, rP, rP));

    for (float t : {0.1f, 0.5f, 1.0f}) {
        CHECK(!pruneSynthesis(t, rP, rP));
        CHECK(!pruneSynthesis(t, rP, rP + 100.0f));
        CHECK(!pruneSynthesis(t, rP, 0.0f));
    }
}

// ===========================================================================
// The synthesised hidden sample
// ===========================================================================

namespace {

// Scene (m)'s halo card at r = 8: a manual rig focused on the background
// plane, so the card at kSynthCardZ blurs by exactly 8 px and the plane by 0.
constexpr float kSynthCardZ  = 4.0f;
constexpr float kSynthPlaneZ = 20.0f;
constexpr float kSynthCardC  = 0.8f;
constexpr float kSynthPlaneC = 0.2f;

struct SynthRig {
    CocParams     coc;
    HoldoutBoundaries holdoutSet;
    float         depthMax = 0.0f;
    FlattenParams fp;
};

SynthRig makeSynthRig(bool rayDistance = false)
{
    SynthRig r;
    r.coc     = makeManualRig(2.0f, kSynthPlaneZ);
    r.holdoutSet = makeHoldoutBoundaries(r.coc, 1.0f, kSynthPlaneZ, 16);
    r.depthMax   = kSynthPlaneZ;
    r.fp      = makeFlattenParams(r.coc, 1, true);
    r.fp.depthIsRayDistance = rayDistance;
    r.fp.formatHeightPx     = 1080.0f;
    return r;
}

std::vector<SampleRecord> cardSample(float alpha)
{
    return {makeSample(kSynthCardZ, kSynthCardZ, alpha, {kSynthCardC * alpha})};
}

std::vector<SampleRecord> planeSample(float alpha = 1.0f, float rawScale = 1.0f)
{
    return {makeSample(kSynthPlaneZ / rawScale, kSynthPlaneZ / rawScale, alpha,
                       {kSynthPlaneC * alpha})};
}

// A map over [x0, x1) x [y0, y1) from a per-pixel stack supplier, with its
// pyramid -- one call per rig so every case below drives buildSurfaceMap().
template <typename StackFn>
void buildSynthMap(SurfaceMap& map, MaxDepthPyramid& pyramid, const SynthRig& rig,
                   int x0, int x1, int y0, int y1, StackFn&& stackAt)
{
    std::vector<SampleRecord> row;
    const bool ok = buildSurfaceMap(
        map, rig.fp,
        x0, x1, y0, y1,
        x0, x1, y0, y1,
        y0, y1, 0, 0, 1,
        [](int) { return true; },
        [&](int x, int y) -> VectorSamples {
            row = stackAt(x, y);
            return VectorSamples{&row};
        });
    REQUIRE(ok);
    pyramid.build(map);
}

int floatUlps(float a, float b)
{
    if (a == b)
        return 0;
    std::int32_t ia, ib;
    std::memcpy(&ia, &a, sizeof ia);
    std::memcpy(&ib, &b, sizeof ib);
    if (ia < 0) ia = std::numeric_limits<std::int32_t>::min() - ia;
    if (ib < 0) ib = std::numeric_limits<std::int32_t>::min() - ib;
    const std::int64_t d = static_cast<std::int64_t>(ia) - ib;
    return static_cast<int>(d < 0 ? -d : d);
}

// Field-by-field comparison of two SoAs, in ulps: the worst over every float
// field, with the integer and flag fields required to match outright.
struct SoADiff {
    bool sameShape = true;
    int  worstUlps = 0;
};

SoADiff compareSoA(const SampleSoA& a, const SampleSoA& b)
{
    SoADiff d;
    if (a.fragmentCount() != b.fragmentCount() || a.channelCount != b.channelCount) {
        d.sameShape = false;
        return d;
    }
    auto ints = [&](const PodBuffer<std::int32_t>& x, const PodBuffer<std::int32_t>& y) {
        for (std::size_t i = 0; i < x.size(); ++i)
            if (x[i] != y[i]) d.sameShape = false;
    };
    auto floats = [&](const PodBuffer<float>& x, const PodBuffer<float>& y) {
        for (std::size_t i = 0; i < x.size(); ++i)
            d.worstUlps = std::max(d.worstUlps, floatUlps(x[i], y[i]));
    };
    ints(a.x, b.x);
    ints(a.y, b.y);
    for (std::size_t i = 0; i < a.flags.size(); ++i)
        if (a.flags[i] != b.flags[i]) d.sameShape = false;
    floats(a.radius, b.radius);
    floats(a.depth, b.depth);
    floats(a.alpha, b.alpha);
    floats(a.arrivalShare, b.arrivalShare);
    floats(a.color, b.color);
    return d;
}

// The halo rig end to end -- flatten (with or without the synthesis, with or
// without the real hidden sample), scatter, virtual background, resolve --
// the doctest-side computeBand() for a W x W frame holding an opaque card
// [c0, c1)^2 over the plane.
constexpr int kSynthW    = 64;
constexpr int kSynthCard0 = 20;
constexpr int kSynthCard1 = 44;

bool inSynthCard(int x, int y)
{
    return x >= kSynthCard0 && x < kSynthCard1 && y >= kSynthCard0 && y < kSynthCard1;
}

struct SynthRender {
    Band      band;
    SampleSoA soa;
    int       appended = 0;
    ResidualWindow window;
};

void renderSynthRig(SynthRender& out, const SynthRig& rig, bool synthesize, bool twin,
                    bool smear, float cardAlpha, const HoldoutSoA& holdout)
{
    const int W = kSynthW, pad = 10;
    FillScratch fillScratch;
    auto stackAt = [&](int x, int y) -> std::vector<SampleRecord> {
        if (!inSynthCard(x, y))
            return planeSample();
        std::vector<SampleRecord> v = cardSample(cardAlpha);
        if (twin)
            v.push_back(planeSample()[0]);
        return v;
    };

    SurfaceMap map;
    MaxDepthPyramid pyramid;
    buildSynthMap(map, pyramid, rig, -pad, W + pad, -pad, W + pad, stackAt);

    out.soa.begin(1, rig.fp.groups);
    FlattenScratch scratch;
    out.window.allocate(-pad, -pad, W + 2 * pad, W + 2 * pad,
                        autoBackgroundRadiusPx(rig.coc, rig.depthMax));
    out.appended = 0;
    for (int y = -pad; y < W + pad; ++y) {
        for (int x = -pad; x < W + pad; ++x) {
            std::vector<SampleRecord> v = stackAt(x, y);
            if (synthesize
                && appendHiddenBackground(map, pyramid, rig.fp, x, y, 0.0f, 100.0f, smear, v, fillScratch))
                ++out.appended;
            const std::size_t i = static_cast<std::size_t>(out.window.index(x, y));
            float residualT = 1.0f;
            float residualR = out.window.radiusPx[i];
            flattenPixelToSoA(rig.fp, x, y, v, scratch, out.soa, nullptr,
                              &residualT, &residualR);
            out.window.setPixel(x, y, residualT, residualR);
        }
    }

    out.band.C = 1;
    out.band.W = W;
    out.band.H = W;
    DiscKernelLUT kernel(0.0f, 8.0f, 1.0f, 1.0f);
    runBand(out.band, makeScatterParams(W, W), out.soa, holdout, kernel, false, &out.window);
}

} // namespace

TEST_CASE("the synthesised hidden sample flattens bit-identically to the real one a DeepMerge "
          "would have supplied: fragments, residualT and residualRadiusPx")
{
    const SynthRig rig = makeSynthRig();
    FillScratch fillScratch;
    const int x0 = 0, x1 = 24, y0 = 0, y1 = 24;

    // A 24 x 24 field: card over [8, 16)^2, plane elsewhere.  Every card pixel
    // is within 8 px of the plane, so the primary reach (2 * 8 + 1) finds it.
    auto sparse = [](int x, int y) -> std::vector<SampleRecord> {
        const bool card = (x >= 8 && x < 16 && y >= 8 && y < 16);
        return card ? cardSample(1.0f) : planeSample();
    };

    SUBCASE("opaque card, on axis: T_P is 0, the synthetic's share is 0, residualT stays 0")
    {
        SurfaceMap map;
        MaxDepthPyramid pyramid;
        buildSynthMap(map, pyramid, rig, x0, x1, y0, y1, sparse);

        int checked = 0;
        for (bool smear : {false, true}) {
        for (int y = 8; y < 16; ++y) {
            for (int x = 8; x < 16; ++x) {
                CAPTURE(smear);
                CAPTURE(x);
                CAPTURE(y);
                std::vector<SampleRecord> synth = cardSample(1.0f);
                REQUIRE(appendHiddenBackground(map, pyramid, rig.fp, x, y, 0.0f, 100.0f, smear, synth, fillScratch));
                REQUIRE(synth.size() == 2u);
                CHECK(synth[1].zFront == kSynthPlaneZ);
                CHECK(synth[1].zBack  == kSynthPlaneZ);
                CHECK(synth[1].alpha  == 1.0f);
                CHECK(synth[1].channels.size() == 1u);
                CHECK(synth[1].channels[0] == kSynthPlaneC);

                std::vector<SampleRecord> twin = cardSample(1.0f);
                twin.push_back(planeSample()[0]);

                float tS = -1.0f, rS = -1.0f, tT = -1.0f, rT = -1.0f;
                const SampleSoA a = flattenOnePixel(rig.fp, x, y, synth, &tS, &rS);
                const SampleSoA b = flattenOnePixel(rig.fp, x, y, twin,  &tT, &rT);
                const SoADiff d = compareSoA(a, b);
                CHECK(d.sameShape);
                CHECK(d.worstUlps == 0);
                CHECK(tS == tT);
                CHECK(rS == rT);
                CHECK(tS == 0.0f);
                CHECK(rS == 0.0f);
                REQUIRE(a.fragmentCount() == 2u);
                CHECK(a.arrivalShare[0] == 1.0f);
                CHECK(a.arrivalShare[1] == 0.0f);
                CHECK(a.radius[0] == 8.0f);
                CHECK(a.radius[1] == 0.0f);
                CHECK(a.color[1] == kSynthPlaneC);
                ++checked;
            }
        }
        }
        CHECK(checked == 128);
    }

    SUBCASE("alpha 0.5 card: the synthetic's share is 0.5 * alpha_Q and residualT becomes "
            "0.5 * (1 - alpha_Q) at Q's radius")
    {
        const float alphaQ = 0.8f;
        auto field = [&](int x, int y) -> std::vector<SampleRecord> {
            const bool card = (x >= 8 && x < 16 && y >= 8 && y < 16);
            return card ? cardSample(0.5f) : planeSample(alphaQ);
        };
        SurfaceMap map;
        MaxDepthPyramid pyramid;
        buildSynthMap(map, pyramid, rig, x0, x1, y0, y1, field);

        for (bool smear : {false, true}) {
            CAPTURE(smear);
            std::vector<SampleRecord> synth = cardSample(0.5f);
            REQUIRE(appendHiddenBackground(map, pyramid, rig.fp, 12, 12, 0.0f, 100.0f, smear, synth, fillScratch));
            std::vector<SampleRecord> twin = cardSample(0.5f);
            twin.push_back(planeSample(alphaQ)[0]);

            float tS = -1.0f, rS = -1.0f, tT = -1.0f, rT = -1.0f;
            const SampleSoA a = flattenOnePixel(rig.fp, 12, 12, synth, &tS, &rS);
            const SampleSoA b = flattenOnePixel(rig.fp, 12, 12, twin,  &tT, &rT);
            const SoADiff d = compareSoA(a, b);
            CHECK(d.sameShape);
            CHECK(d.worstUlps == 0);
            CHECK(tS == tT);
            CHECK(rS == rT);
            REQUIRE(a.fragmentCount() == 2u);
            CHECK(a.arrivalShare[0] == 0.5f);
            CHECK(a.arrivalShare[1] == 0.5f * alphaQ);
            CHECK(tS == doctest::Approx(0.5f * (1.0f - alphaQ)).epsilon(1e-6));
            CHECK(rS == radiusPixels(rig.coc, kSynthPlaneZ));
            CHECK(rS == 0.0f);
            CHECK(a.color[1] == kSynthPlaneC * alphaQ);
        }
    }

    SUBCASE("off axis with depth_is_ray_distance: the raw depth is written so P's own "
            "factor lands it back on Q's camera depth at 0 ulp")
    {
        const SynthRig ray = makeSynthRig(true);
        const int ox = 1800, oy = 1000;
        // The plane's own ray-distance sample at each pixel: camera depth
        // kSynthPlaneZ, raw depth kSynthPlaneZ / scale(pixel).
        auto field = [&](int x, int y) -> std::vector<SampleRecord> {
            const bool card = (x >= ox + 8 && x < ox + 16 && y >= oy + 8 && y < oy + 16);
            if (card)
                return cardSample(1.0f);
            return planeSample(1.0f, rayDepthScaleAt(ray.fp, x, y));
        };
        SurfaceMap map;
        MaxDepthPyramid pyramid;
        buildSynthMap(map, pyramid, ray, ox, ox + 24, oy, oy + 24, field);

        int worstRoundTrip = 0, worstSoA = 0, exact = 0, checked = 0;
        for (bool smear : {false, true}) {
        for (int y = oy + 8; y < oy + 16; ++y) {
            for (int x = ox + 8; x < ox + 16; ++x) {
                CAPTURE(smear);
                CAPTURE(x);
                CAPTURE(y);
                const float sP = rayDepthScaleAt(ray.fp, x, y);
                REQUIRE(sP < 1.0f);

                std::vector<SampleRecord> synth = cardSample(1.0f);
                REQUIRE(appendHiddenBackground(map, pyramid, ray.fp, x, y, 0.0f, 100.0f, smear, synth, fillScratch));
                REQUIRE(synth.size() == 2u);
                // Q's camera depth as the map holds it, after P's own
                // correction is applied to the synthetic.
                const BackgroundSource q = findBackgroundSource(map, pyramid, x, y, 17, 100);
                REQUIRE(q.found);
                const float zCamQ = map.plane(SurfaceMap::kZFront)[map.index(q.qx, q.qy)];
                const int rt = floatUlps(synth[1].zFront * sP, zCamQ);
                worstRoundTrip = std::max(worstRoundTrip, rt);
                CHECK(rt == 0);
                CHECK(synth[1].zBack * sP == zCamQ);

                std::vector<SampleRecord> twin = cardSample(1.0f);
                twin.push_back(planeSample(1.0f, sP)[0]);
                float tS = -1.0f, rS = -1.0f, tT = -1.0f, rT = -1.0f;
                const SampleSoA a = flattenOnePixel(ray.fp, x, y, synth, &tS, &rS);
                const SampleSoA b = flattenOnePixel(ray.fp, x, y, twin,  &tT, &rT);
                const SoADiff d = compareSoA(a, b);
                CHECK(d.sameShape);
                // Q's camera depth (raw_Q * sQ) against the twin's (raw_P *
                // sP): on this rig both land on kSynthPlaneZ exactly, so the
                // synthetic and the twin flatten to the same bits.
                CHECK(d.worstUlps == 0);
                worstSoA = std::max(worstSoA, d.worstUlps);
                if (d.worstUlps == 0)
                    ++exact;
                CHECK(tS == tT);
                CHECK(rS == rT);
                CHECK(tS == 0.0f);
                ++checked;
            }
        }
        }
        std::printf("\noff-axis synthesis: %d pixels (both fill_smear states), worst round-trip "
                    "%d ulp, worst SoA field %d ulp, %d bit-exact\n",
                    checked, worstRoundTrip, worstSoA, exact);
        CHECK(checked == 128);
    }

    SUBCASE("Q's surface is an overlapping-span stack: the synthetic is Q's deepest RAW "
            "sample, and the map's radius sits within the span's own radius range of "
            "the flatten's residual radius")
    {
        // Two overlapping spans behind the card's depth.  The map carries the
        // raw span with the greatest zBack; the flatten at Q first cuts the
        // overlap and stages the tail piece, whose midpoint is deeper.
        const SampleRecord spanA = makeSample(kSynthPlaneZ - 2.0f, kSynthPlaneZ + 2.0f, 0.5f, {0.1f});
        const SampleRecord spanB = makeSample(kSynthPlaneZ,        kSynthPlaneZ + 6.0f, 0.4f, {0.08f});
        auto field = [&](int x, int y) -> std::vector<SampleRecord> {
            const bool card = (x >= 8 && x < 16 && y >= 8 && y < 16);
            return card ? cardSample(1.0f) : std::vector<SampleRecord>{spanA, spanB};
        };
        SynthRig wide = rig;
        wide.holdoutSet = makeHoldoutBoundaries(wide.coc, 1.0f, kSynthPlaneZ + 6.0f, 16);
    wide.depthMax   = kSynthPlaneZ + 6.0f;
        SurfaceMap map;
        MaxDepthPyramid pyramid;
        buildSynthMap(map, pyramid, wide, x0, x1, y0, y1, field);

        for (bool smear : {false, true}) {
            CAPTURE(smear);
            std::vector<SampleRecord> synth = cardSample(1.0f);
            REQUIRE(appendHiddenBackground(map, pyramid, wide.fp, 12, 12, 0.0f, 100.0f, smear, synth, fillScratch));
            REQUIRE(synth.size() == 2u);
            CHECK(synth[1].zFront == spanB.zFront);
            CHECK(synth[1].zBack  == spanB.zBack);
            CHECK(synth[1].alpha  == spanB.alpha);
            CHECK(synth[1].channels[0] == spanB.channels[0]);

            std::vector<SampleRecord> twin = cardSample(1.0f);
            twin.push_back(spanB);
            float tS = -1.0f, rS = -1.0f, tT = -1.0f, rT = -1.0f;
            const SampleSoA a = flattenOnePixel(wide.fp, 12, 12, synth, &tS, &rS);
            const SampleSoA b = flattenOnePixel(wide.fp, 12, 12, twin,  &tT, &rT);
            const SoADiff d = compareSoA(a, b);
            CHECK(d.sameShape);
            CHECK(d.worstUlps == 0);
            CHECK(tS == tT);
            CHECK(rS == rT);
        }

        const BackgroundSource q = findBackgroundSource(map, pyramid, 12, 12, 17, 100);
        REQUIRE(q.found);
        const float mapRadiusQ = map.plane(SurfaceMap::kRadius)[map.index(q.qx, q.qy)];
        float tQ = -1.0f, rQ = -1.0f;
        flattenOnePixel(wide.fp, q.qx, q.qy, {spanA, spanB}, &tQ, &rQ);
        const float rangeLo = radiusPixels(wide.coc, spanB.zFront);
        const float rangeHi = radiusPixels(wide.coc, spanB.zBack);
        CHECK(mapRadiusQ != rQ);
        CHECK(std::fabs(mapRadiusQ - rQ) <= std::fabs(rangeHi - rangeLo));
        CHECK(mapRadiusQ >= std::min(rangeLo, rangeHi));
        CHECK(mapRadiusQ <= std::max(rangeLo, rangeHi));
        CHECK(rQ >= std::min(rangeLo, rangeHi));
        CHECK(rQ <= std::max(rangeLo, rangeHi));
        std::printf("\noverlapping-span Q: map radius %.5f, flatten residual radius %.5f, "
                    "span radius range [%.5f, %.5f]\n", mapRadiusQ, rQ, rangeLo, rangeHi);
    }
}

TEST_CASE("rawDepthForCameraDepth: raw * s reproduces the camera depth exactly wherever a "
          "float raw exists, else lands within 1 ulp and no neighbour does better; the "
          "residual rate over 1e5 pairs")
{
    // s in the ray-scale range the node produces: f / sqrt(f^2 + r^2) runs
    // from 1 on axis to ~0.3 at the corner of a very wide lens.
    Lcg rng(0x5CA1Eu);
    int exact = 0, residual = 0, worstResidualUlps = 0;
    for (int iter = 0; iter < 100000; ++iter) {
        const float s   = rng.range(0.3f, 1.0f);
        const float cam = std::exp(rng.range(std::log(1e-3f), std::log(1e6f)));
        const float raw = rawDepthForCameraDepth(cam, s);
        REQUIRE(std::isfinite(raw));
        REQUIRE(raw > 0.0f);
        if (raw * s == cam) {
            ++exact;
            continue;
        }
        ++residual;
        const int u = floatUlps(raw * s, cam);
        worstResidualUlps = std::max(worstResidualUlps, u);
        REQUIRE(u <= 1);
        for (int k = 1; k <= 2; ++k) {
            float lo = raw, hi = raw;
            for (int i = 0; i < k; ++i) {
                lo = std::nextafter(lo, 0.0f);
                hi = std::nextafter(hi, std::numeric_limits<float>::infinity());
            }
            CHECK(!(lo * s == cam));
            CHECK(!(hi * s == cam));
        }
    }
    std::printf("\nrawDepthForCameraDepth: 1e5 pairs, exact %d, no float raw %d (worst %d ulp)\n",
                exact, residual, worstResidualUlps);
    CHECK(exact > 85000);
    CHECK(residual < 15000);
    CHECK(worstResidualUlps == 1);

    // s == 1 (the toggle off, or the on-axis pixel) and cam == 0 are the
    // identity at zero steps.
    CHECK(rawDepthForCameraDepth(7.25f, 1.0f) == 7.25f);
    CHECK(rawDepthForCameraDepth(0.0f, 0.7f) == 0.0f);
    CHECK(floatUlps(rawDepthForCameraDepth(FrameDepthRange::kMaxDepth, 0.3f) * 0.3f,
                    FrameDepthRange::kMaxDepth) <= 1);
}

TEST_CASE("residualTransmittance agrees with flattenPixelToSoA's residualT over fuzzed "
          "stacks of points, spans and overlaps, far inside kFillDeficitTol")
{
    Lcg rng(0xF111u);
    const CocParams     p  = makeStandardRig(10.0f);
    const FlattenParams fp = makeFlattenParams(p, 2, true);

    float worst = 0.0f;
    for (int iter = 0; iter < 800; ++iter) {
        CAPTURE(iter);
        std::vector<SampleRecord> v = fuzzDisjointStack(rng, rng.intRange(1, 6), rng.intRange(0, 2), 2);
        if (rng.unit() < 0.5f) {
            const SampleRecord& base = v[0];
            v.push_back(makeSample(base.zFront + 0.1f, base.zBack + 2.0f, rng.range(0.01f, 0.9f),
                                   {0.0f, 0.0f}));
        }
        if (rng.unit() < 0.2f)
            v.push_back(makeSample(3.0f, 3.0f, std::numeric_limits<float>::quiet_NaN(), {0.0f, 0.0f}));
        const float helper = static_cast<float>(residualTransmittance(v));
        float tF = -1.0f;
        flattenOnePixel(fp, 0, 0, v, &tF, nullptr);
        worst = std::max(worst, std::fabs(helper - tF));
        CHECK(helper == doctest::Approx(tF).epsilon(2e-6));
    }
    CHECK(worst < kFillDeficitTol * 0.1f);
    CHECK(residualTransmittance({}) == 1.0);
    CHECK(residualTransmittance(cardSample(1.0f)) == 0.0);
    CHECK(residualTransmittance(cardSample(0.5f)) == 0.5);
}

TEST_CASE("residualTransmittance and the prune decision are invariant to sample order, "
          "including stacks whose product sits within an ulp of kFillDeficitTol")
{
    Lcg rng(0x0DDEu);
    const float rP = 4.0f;
    int nearThreshold = 0, pruned = 0, floatOrderFlips = 0;
    for (int iter = 0; iter < 20000; ++iter) {
        CAPTURE(iter);
        const int n = rng.intRange(2, 8);
        std::vector<SampleRecord> v;
        // Half the stacks are built so the product of (1 - a) lands on
        // kFillDeficitTol to float rounding: n - 1 random factors, the last
        // chosen to hit the tolerance, then jittered by a few float ulps.
        const bool aimed = (iter % 2) == 0;
        double product = 1.0;
        for (int i = 0; i < n - 1; ++i) {
            const float a = aimed ? rng.range(0.05f, 0.6f) : rng.range(0.0f, 1.0f);
            v.push_back(makeSample(1.0f + i, 1.0f + i, a, {0.1f}));
            product *= 1.0 - static_cast<double>(a);
        }
        float last = aimed ? static_cast<float>(1.0 - static_cast<double>(kFillDeficitTol) / product)
                           : rng.range(0.0f, 1.0f);
        if (aimed) {
            for (int k = rng.intRange(-3, 3); k < 0; ++k) last = std::nextafter(last, 0.0f);
            for (int k = rng.intRange(-3, 3); k > 0; --k) last = std::nextafter(last, 1.0f);
        }
        v.push_back(makeSample(static_cast<float>(n), static_cast<float>(n), last, {0.1f}));

        const double t0 = residualTransmittance(v);
        const bool   p0 = pruneSynthesis(t0, rP, rP);
        if (std::fabs(t0 - static_cast<double>(kFillDeficitTol)) < 1e-6)
            ++nearThreshold;
        if (p0)
            ++pruned;

        // Every rotation and the reverse, plus a few random shuffles.
        float tFloatFirst = 1.0f;
        for (const SampleRecord& s : v) tFloatFirst *= (1.0f - s.alpha);
        std::vector<std::vector<SampleRecord>> orders;
        for (int r = 1; r < n; ++r) {
            std::vector<SampleRecord> w(v.begin() + r, v.end());
            w.insert(w.end(), v.begin(), v.begin() + r);
            orders.push_back(w);
        }
        orders.push_back(std::vector<SampleRecord>(v.rbegin(), v.rend()));
        for (int k = 0; k < 3; ++k) {
            std::vector<SampleRecord> w = v;
            for (int i = n - 1; i > 0; --i)
                std::swap(w[static_cast<std::size_t>(i)],
                          w[static_cast<std::size_t>(rng.intRange(0, i))]);
            orders.push_back(w);
        }
        for (const std::vector<SampleRecord>& w : orders) {
            const double t = residualTransmittance(w);
            CHECK(pruneSynthesis(t, rP, rP) == p0);
            CHECK(std::fabs(t - t0) <= 2e-15 * t0);
            float tFloat = 1.0f;
            for (const SampleRecord& s : w) tFloat *= (1.0f - s.alpha);
            if ((tFloat <= kFillDeficitTol) != (tFloatFirst <= kFillDeficitTol))
                ++floatOrderFlips;
        }
    }
    std::printf("\nresidual order invariance: %d near-threshold stacks, %d pruned; a float "
                "product in raw order would have flipped the prune %d times\n",
                nearThreshold, pruned, floatOrderFlips);
    CHECK(nearThreshold > 1000);
    CHECK(pruned > 1000);
}

TEST_CASE("appendHiddenBackground: the per-pixel auto reach is 2 * r_P + 1 at P's own "
          "radius; the prune drops an opaque P whose source disc is at least its own; an "
          "empty pixel or one with no source appends nothing")
{
    const SynthRig rig = makeSynthRig();
    FillScratch fillScratch;

    SUBCASE("prune: opaque P at r = 0 over a source at r = 0 appends nothing; alpha 0.5 P does")
    {
        // A near plane at the focus depth (r = 0) with a hole where a deeper
        // plane shows, so radius_Q >= radius_P for every hole-adjacent P.
        auto field = [](int x, int y) -> std::vector<SampleRecord> {
            const bool hole = (x >= 8 && x < 16 && y >= 8 && y < 16);
            if (hole)
                return {makeSample(kSynthPlaneZ + 10.0f, kSynthPlaneZ + 10.0f, 1.0f, {0.3f})};
            return planeSample();
        };
        SynthRig wide = rig;
        wide.holdoutSet = makeHoldoutBoundaries(wide.coc, 1.0f, kSynthPlaneZ + 10.0f, 16);
    wide.depthMax   = kSynthPlaneZ + 10.0f;
        SurfaceMap map;
        MaxDepthPyramid pyramid;
        buildSynthMap(map, pyramid, wide, 0, 24, 0, 24, field);

        std::vector<SampleRecord> opaque = planeSample(1.0f);
        CHECK(findBackgroundSource(map, pyramid, 7, 12, 1, 100).found);
        CHECK(!appendHiddenBackground(map, pyramid, wide.fp, 7, 12, 0.0f, 100.0f, true, opaque, fillScratch));
        CHECK(opaque.size() == 1u);

        std::vector<SampleRecord> half = planeSample(0.5f);
        CHECK(appendHiddenBackground(map, pyramid, wide.fp, 7, 12, 0.0f, 100.0f, true, half, fillScratch));
        CHECK(half.size() == 2u);
        CHECK(half[1].zFront == kSynthPlaneZ + 10.0f);
    }

    SUBCASE("auto reach: a card pixel 20 px from the plane is found at r_P = 8 (reach 17) "
            "only through the fallback, which max_radius bounds")
    {
        auto field = [](int x, int y) -> std::vector<SampleRecord> {
            const bool card = (x >= 4 && x < 44 && y >= 4 && y < 44);
            return card ? cardSample(1.0f) : planeSample();
        };
        SurfaceMap map;
        MaxDepthPyramid pyramid;
        buildSynthMap(map, pyramid, rig, 0, 48, 0, 48, field);
        REQUIRE(map.plane(SurfaceMap::kRadius)[map.index(24, 24)] == 8.0f);

        FillSearchStats withFallback, primaryOnly, manual;
        std::vector<SampleRecord> a = cardSample(1.0f);
        CHECK(appendHiddenBackground(map, pyramid, rig.fp, 24, 24, 0.0f, 100.0f, true, a, fillScratch, &withFallback));
        std::vector<SampleRecord> b = cardSample(1.0f);
        CHECK(!appendHiddenBackground(map, pyramid, rig.fp, 24, 24, 0.0f, 17.0f, true, b, fillScratch, &primaryOnly));
        CHECK(b.size() == 1u);
        std::vector<SampleRecord> c = cardSample(1.0f);
        CHECK(appendHiddenBackground(map, pyramid, rig.fp, 24, 24, 20.0f, 20.0f, true, c, fillScratch, &manual));
        CHECK(c.size() == 2u);
        std::vector<SampleRecord> e = cardSample(1.0f);
        CHECK(!appendHiddenBackground(map, pyramid, rig.fp, 24, 24, 19.0f, 19.0f, true, e, fillScratch));
        CHECK(!appendHiddenBackground(map, pyramid, rig.fp, 24, 24, 100.0f, 19.0f, true, e, fillScratch));
        CHECK(e.size() == 1u);
        CHECK(appendHiddenBackground(map, pyramid, rig.fp, 12, 24, 0.0f, 17.0f, true, e, fillScratch));
        CHECK(e.size() == 2u);
        CHECK(withFallback.tilesVisited > primaryOnly.tilesVisited);
    }

    SUBCASE("an empty pixel, an alpha-0 pixel and a plane pixel append nothing")
    {
        auto field = [](int x, int y) -> std::vector<SampleRecord> {
            const bool card = (x >= 8 && x < 16 && y >= 8 && y < 16);
            return card ? cardSample(1.0f) : planeSample();
        };
        SurfaceMap map;
        MaxDepthPyramid pyramid;
        buildSynthMap(map, pyramid, rig, 0, 24, 0, 24, field);
        std::vector<SampleRecord> none;
        CHECK(!appendHiddenBackground(map, pyramid, rig.fp, 30, 30, 0.0f, 100.0f, true, none, fillScratch));
        CHECK(!appendHiddenBackground(map, pyramid, rig.fp, 2, 2, 0.0f, 100.0f, true, none, fillScratch));
        CHECK(none.empty());
        std::vector<SampleRecord> plane = planeSample();
        CHECK(!appendHiddenBackground(map, pyramid, rig.fp, 2, 2, 0.0f, 100.0f, true, plane, fillScratch));
        CHECK(plane.size() == 1u);
    }
}

TEST_CASE("end to end on the halo rig: synthesis reads the twin's FG:BG mix in the vacated "
          "band within 1e-6 under both fill_smear states, the foreground mode reads the "
          "card's own colour there, and a holdout at 0.5 halves the synthesised deposits "
          "exactly as it halves the twin's")
{
    const SynthRig rig = makeSynthRig();
    const HoldoutSoA none;
    SynthRender synth, twin, foreground, smeared;
    renderSynthRig(synth,      rig, true,  false, false, 1.0f, none);
    renderSynthRig(smeared,    rig, true,  false, true,  1.0f, none);
    renderSynthRig(twin,       rig, false, true,  false, 1.0f, none);
    renderSynthRig(foreground, rig, false, false, false, 1.0f, none);
    CHECK(synth.appended == (kSynthCard1 - kSynthCard0) * (kSynthCard1 - kSynthCard0));
    CHECK(smeared.appended == synth.appended);
    CHECK(foreground.appended == 0);

    const SoADiff d = compareSoA(synth.soa, twin.soa);
    CHECK(d.sameShape);
    CHECK(d.worstUlps == 0);
    const SoADiff dS = compareSoA(smeared.soa, twin.soa);
    CHECK(dS.sameShape);
    CHECK(dS.worstUlps == 0);

    // Just inside the silhouette, where the card's own disc reaches out of
    // the card and its arrival falls short of one.
    const int probes[][2] = {{kSynthCard0 + 1, 32}, {kSynthCard0 + 3, 32}, {kSynthCard0 + 6, 32},
                             {32, kSynthCard1 - 2}, {kSynthCard1 - 4, kSynthCard0 + 4}};
    std::printf("\nhalo rig, vacated band (x, y): synth R/A  twin R/A  foreground R/A\n");
    double worstVsTwin = 0.0, worstVsForeground = 0.0;
    for (const auto& pr : probes) {
        const int x = pr[0], y = pr[1];
        const float aS = synth.band.outAlpha(x, y),      rS = synth.band.outColor(0, x, y);
        const float aT = twin.band.outAlpha(x, y),       rT = twin.band.outColor(0, x, y);
        const float aF = foreground.band.outAlpha(x, y), rF = foreground.band.outColor(0, x, y);
        std::printf("  (%2d, %2d): %.6f/%.6f  %.6f/%.6f  %.6f/%.6f\n",
                    x, y, rS, aS, rT, aT, rF, aF);
        CHECK(aS == doctest::Approx(1.0f).epsilon(1e-6));
        CHECK(aF == doctest::Approx(1.0f).epsilon(1e-6));
        CHECK(rF == doctest::Approx(kSynthCardC).epsilon(1e-5));
        CHECK(rS < kSynthCardC);
        CHECK(rS > kSynthPlaneC);
        worstVsTwin       = std::max(worstVsTwin, std::fabs(static_cast<double>(rS) - rT));
        worstVsTwin       = std::max(worstVsTwin, std::fabs(static_cast<double>(aS) - aT));
        worstVsForeground = std::max(worstVsForeground, std::fabs(static_cast<double>(rS) - rF));
    }
    CHECK(worstVsTwin <= 1e-6);
    CHECK(worstVsForeground > 1e-2);

    int differing = 0;
    for (std::size_t i = 0; i < synth.band.color.size(); ++i) {
        if (synth.band.color[i] != twin.band.color[i] || synth.band.alpha[i] != twin.band.alpha[i])
            ++differing;
        if (smeared.band.color[i] != twin.band.color[i] || smeared.band.alpha[i] != twin.band.alpha[i])
            ++differing;
    }
    CHECK(differing == 0);

    SUBCASE("a holdout at 0.5 everywhere")
    {
        HoldoutSampleSoA hs;
        HoldoutLut       lut;
        hs.begin(static_cast<std::ptrdiff_t>(kSynthW) * kSynthW);
        for (std::ptrdiff_t i = 0; i < static_cast<std::ptrdiff_t>(kSynthW) * kSynthW; ++i) {
            std::vector<SampleRecord> hv{makeSample(0.5f, 0.5f, 0.5f, {})};
            hs.appendPixel(hv, 1.0f);
        }
        lut.build(hs, rig.holdoutSet);
        const HoldoutSoA half = lut.view();
        REQUIRE(half.enabled());

        SynthRender synthH, twinH;
        renderSynthRig(twinH,  rig, false, true,  false, 1.0f, half);
        for (bool smear : {false, true}) {
            CAPTURE(smear);
            renderSynthRig(synthH, rig, true, false, smear, 1.0f, half);

            int differingH = 0;
            for (std::size_t i = 0; i < synthH.band.planes.color.size(); ++i)
                if (synthH.band.planes.color[i] != twinH.band.planes.color[i])
                    ++differingH;
            for (std::size_t i = 0; i < synthH.band.planes.alpha.size(); ++i)
                if (synthH.band.planes.alpha[i] != twinH.band.planes.alpha[i])
                    ++differingH;
            for (std::size_t i = 0; i < synthH.band.color.size(); ++i)
                if (synthH.band.color[i] != twinH.band.color[i] || synthH.band.alpha[i] != twinH.band.alpha[i])
                    ++differingH;
            CHECK(differingH == 0);
        }
    }
}

namespace {

// A checker of two premultiplied colours in 3 px cells, so a stride-2
// lattice samples both colours whatever P's own parity.
constexpr float kCheckerA = 0.2f;
constexpr float kCheckerB = 0.6f;

float checkerColour(int x, int y)
{
    return (((x / 3 + y / 3) & 1) == 0) ? kCheckerA : kCheckerB;
}

std::vector<SampleRecord> checkerSample(int x, int y)
{
    return {makeSample(kSynthPlaneZ, kSynthPlaneZ, 1.0f, {checkerColour(x, y)})};
}

} // namespace

TEST_CASE("fill_smear on a textured background: off, the synthetic is the nearest Q's colour "
          "verbatim; on, it is the mean over the qualifying stride lattice within the primary "
          "reach (1 ulp of an independent double sum) and a fallback-tier Q stays verbatim")
{
    const SynthRig rig = makeSynthRig();
    FillScratch fillScratch;

    SUBCASE("primary tier: 16 x 16 opaque card over the checker, every card pixel within reach 17")
    {
        const int W = 40, c0 = 12, c1 = 28;
        auto field = [&](int x, int y) -> std::vector<SampleRecord> {
            const bool card = (x >= c0 && x < c1 && y >= c0 && y < c1);
            return card ? cardSample(1.0f) : checkerSample(x, y);
        };
        SurfaceMap map;
        MaxDepthPyramid pyramid;
        buildSynthMap(map, pyramid, rig, 0, W, 0, W, field);

        const int reach  = 2 * 8 + 1;
        const int stride = fillAverageStride(reach);
        REQUIRE(stride == 2);
        const int steps = reach / stride;

        int checked = 0, worstUlps = 0;
        for (int y = c0; y < c1; ++y) {
            for (int x = c0; x < c1; ++x) {
                CAPTURE(x);
                CAPTURE(y);
                REQUIRE(map.plane(SurfaceMap::kRadius)[map.index(x, y)] == 8.0f);
                const BackgroundSource q = findBackgroundSource(map, pyramid, x, y, reach, 100);
                REQUIRE(q.found);
                REQUIRE(q.distance <= static_cast<float>(reach));
                const float nearestC = checkerColour(q.qx, q.qy);

                std::vector<SampleRecord> plain = cardSample(1.0f);
                REQUIRE(appendHiddenBackground(map, pyramid, rig.fp, x, y, 0.0f, 100.0f, false, plain, fillScratch));
                REQUIRE(plain.size() == 2u);
                CHECK(plain[1].channels[0] == nearestC);
                CHECK(plain[1].alpha  == 1.0f);
                CHECK(plain[1].zFront == kSynthPlaneZ);

                double sum = 0.0;
                int    n   = 0;
                for (int j = -steps; j <= steps; ++j) {
                    for (int i = -steps; i <= steps; ++i) {
                        const int dx = i * stride, dy = j * stride;
                        if (dx * dx + dy * dy > reach * reach)
                            continue;
                        const int nx = x + dx, ny = y + dy;
                        if (nx < 0 || nx >= W || ny < 0 || ny >= W)
                            continue;
                        if (nx >= c0 && nx < c1 && ny >= c0 && ny < c1)
                            continue;
                        sum += checkerColour(nx, ny);
                        ++n;
                    }
                }
                REQUIRE(n > 0);
                const float expected = static_cast<float>(sum / n);

                std::vector<SampleRecord> smeared = cardSample(1.0f);
                REQUIRE(appendHiddenBackground(map, pyramid, rig.fp, x, y, 0.0f, 100.0f, true, smeared, fillScratch));
                REQUIRE(smeared.size() == 2u);
                const int ulps = floatUlps(smeared[1].channels[0], expected);
                worstUlps = std::max(worstUlps, ulps);
                CHECK(ulps <= 1);
                CHECK(smeared[1].channels[0] != nearestC);
                CHECK(smeared[1].channels[0] > kCheckerA);
                CHECK(smeared[1].channels[0] < kCheckerB);
                CHECK(smeared[1].alpha  == 1.0f);
                CHECK(smeared[1].zFront == plain[1].zFront);
                CHECK(smeared[1].zBack  == plain[1].zBack);
                ++checked;
            }
        }
        std::printf("\nfill_smear checker: %d card pixels, worst %d ulp vs the independent mean\n",
                    checked, worstUlps);
        CHECK(checked == 256);
    }

    SUBCASE("fallback tier: a card pixel 20 px from the checker is copied verbatim with smear on")
    {
        auto field = [](int x, int y) -> std::vector<SampleRecord> {
            const bool card = (x >= 4 && x < 44 && y >= 4 && y < 44);
            return card ? cardSample(1.0f) : checkerSample(x, y);
        };
        SurfaceMap map;
        MaxDepthPyramid pyramid;
        buildSynthMap(map, pyramid, rig, 0, 48, 0, 48, field);

        const BackgroundSource q = findBackgroundSource(map, pyramid, 24, 24, 17, 100);
        REQUIRE(q.found);
        REQUIRE(q.distance > 17.0f);
        for (bool smear : {false, true}) {
            CAPTURE(smear);
            std::vector<SampleRecord> v = cardSample(1.0f);
            REQUIRE(appendHiddenBackground(map, pyramid, rig.fp, 24, 24, 0.0f, 100.0f, smear, v, fillScratch));
            REQUIRE(v.size() == 2u);
            CHECK(v[1].channels[0] == checkerColour(q.qx, q.qy));
        }
    }
}

TEST_CASE("fillAverageStride: reach 33 strides by 3, small reaches by 1, and the lattice read "
          "count never exceeds kFillAverageBudget up to max_radius 500")
{
    CHECK(fillAverageStride(33) == 3);
    CHECK(fillAverageStride(0)  == 1);
    CHECK(fillAverageStride(1)  == 1);
    CHECK(fillAverageStride(5)  == 1);
    CHECK(fillAverageStride(15) == 1);
    CHECK(fillAverageStride(16) == 2);

    int worst = 0, worstReach = -1;
    for (int reach = 0; reach <= 500; ++reach) {
        const int stride = fillAverageStride(reach);
        const int steps  = reach / stride;
        int reads = 0;
        for (int j = -steps; j <= steps; ++j)
            for (int i = -steps; i <= steps; ++i)
                if ((i * stride) * (i * stride) + (j * stride) * (j * stride) <= reach * reach)
                    ++reads;
        CAPTURE(reach);
        CHECK(reads <= kFillAverageBudget);
        if (reads > worst) {
            worst      = reads;
            worstReach = reach;
        }
    }
    std::printf("\nfillAverageStride: worst %d reads of %d at reach %d\n",
                worst, kFillAverageBudget, worstReach);
}


TEST_CASE("scatterKernelBin mirrors the scatter's own two radius decisions, at the edges")
{
    // The merge is only lossless when the two members rasterise LITERALLY the
    // same kernel, so this predicate has to agree with the scatter at both of
    // the scatter's decision points and not merely near them.  Both edges
    // survive a mutation set unless a case exercises them exactly.
    //
    // 1. THE SHARP THRESHOLD.  scatterStreamCPU() takes the sharp path for
    //    `!(radius > sharpRadiusPx)`, so radius == kSharpRadiusPx exactly is
    //    the sharp delta (the 1 px diameter IS the 1x1 kernel, whatever the
    //    LUT's r=0.5 entry holds) -- a `>=` here would call it a disc and
    //    refuse to merge it with the sharp fragment beside it that the
    //    scatter deposits identically.
    CHECK(refKernelBin(kSharpRadiusPx) == -1);
    CHECK(scatterKernelBin(kSharpRadiusPx) == -1);
    CHECK(scatterKernelBin(std::nextafter(kSharpRadiusPx, 0.0f)) == -1);
    CHECK(scatterKernelBin(0.0f) == -1);
    CHECK(sameScatterKernel(0.0f, kSharpRadiusPx));
    CHECK(sameScatterKernel(0.0f, std::nextafter(kSharpRadiusPx, 0.0f)));
    // ...and the first radius above the floor BLENDS grid node 0 with node 1
    // (0.5004888): it no longer rasterises the delta, so it must not merge
    // with the floor.  0.6 is ~200 nodes away.
    const float aboveFloor = std::nextafter(kSharpRadiusPx, 1.0f);
    CHECK(refKernelBin(aboveFloor) >= 0);
    CHECK(scatterKernelBin(aboveFloor) == refKernelBin(aboveFloor));
    CHECK_FALSE(sameScatterKernel(kSharpRadiusPx, 0.50024f));
    CHECK_FALSE(sameScatterKernel(kSharpRadiusPx, aboveFloor));
    CHECK_FALSE(sameScatterKernel(kSharpRadiusPx, 0.6f));
    CHECK_FALSE(sameScatterKernel(std::nextafter(kSharpRadiusPx, 0.0f), 0.50024f));

    // 2. NaN and +-inf.  NaN is sharp in the scatter (the test is negated), and
    //    an infinite radius must not reach std::lround, whose result there is
    //    unspecified -- kernelGridIndex() saturates it, and every radius
    //    beyond that saturation is one (clamped) bracket.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    CHECK(scatterKernelBin(nan) == -1);
    CHECK(sameScatterKernel(nan, nan));
    CHECK(sameScatterKernel(inf, inf));
    CHECK(scatterKernelBin(inf) == scatterKernelBin(2.0e6f));
    CHECK(scatterKernelBin(inf) == scatterKernelBin(3.0e6f));
    CHECK_FALSE(sameScatterKernel(inf, 1.0f));
    CHECK(scatterKernelBin(-1.0f) == -1);
    // The bin is (node, 2^20 blend cells) and the saturated node is ~2e6, so
    // the key needs 41 bits: it must not have been folded into an int.
    CHECK(scatterKernelBin(inf) == (static_cast<std::int64_t>(kernelGridIndex(inf))
                                     << kScatterKernelBlendBits));
    CHECK(scatterKernelBin(inf) > scatterKernelBin(DiscKernelLUT::kMaxSupportedRadius));

    // 3. The grid itself.  A node is its own bin; ONE ULP off it is a blend
    //    with the neighbour and a different bin -- in BOTH regions, since the
    //    grid is piecewise -- and pairs the old nearest-node rule merged
    //    (5.16/5.19 on node 926, 20.10/20.20 on node 17) no longer do.
    //    Node 0 is the floor itself and belongs to the sharp bin (case 1).
    for (float node : {1.0f, 5.171717f, 16.0f, 20.0f, 39.5f}) {
        CAPTURE(node);
        const float r = kernelGridRadius(kernelGridIndex(node));
        CHECK(sameScatterKernel(r, r));
        CHECK(scatterKernelBin(r) == (static_cast<std::int64_t>(kernelGridIndex(node))
                                      << kScatterKernelBlendBits));
        CHECK_FALSE(sameScatterKernel(r, std::nextafter(r, 100.0f)));
        CHECK_FALSE(sameScatterKernel(r, std::nextafter(r, 0.0f)));
    }
    CHECK_FALSE(sameScatterKernel(5.16f, 5.19f));
    CHECK_FALSE(sameScatterKernel(5.16f, std::nextafter(5.16f, 6.0f)));
    CHECK_FALSE(sameScatterKernel(20.10f, 20.20f));
    CHECK_FALSE(sameScatterKernel(20.10f, std::nextafter(20.10f, 21.0f)));
    CHECK(sameScatterKernel(5.16f, 5.16f));
    CHECK(sameScatterKernel(20.10f, 20.10f));
    //    ...and the two regions join without a gap or an overlap.
    CHECK(scatterKernelBin(kKernelCoarseFromPx)
          == (static_cast<std::int64_t>(kKernelFineLastIndex) << kScatterKernelBlendBits));
    CHECK(kernelGridRadius(kKernelFineLastIndex) == kKernelCoarseFromPx);
    CHECK(kernelGridRadius(kKernelFineLastIndex + 1)
          == doctest::Approx(kKernelCoarseFromPx + 0.5f));
    //    The independent reference agrees at a node, and off it by exactly the
    //    documented cell arithmetic.
    for (float r : {0.7f, 2.0f, 3.3f, 15.9f, 16.0f, 16.2f, 33.75f}) {
        CAPTURE(r);
        CHECK(scatterKernelBin(r) == refKernelBin(static_cast<double>(r)));
    }
}

// The kernel the scatter rasterises for `radius`, as a dense (2R+1)^2 plane
// centred on the fragment: the sharp path's single weight below the threshold,
// otherwise refBracket()'s blend of the two bracketing LUT entries.  `R` is the
// plane's half-extent, which must cover both radii being compared.
void refKernelPlane(const DiscKernelLUT& lut, float radius, int R,
                    std::vector<double>& plane)
{
    const int side = 2 * R + 1;
    plane.assign(static_cast<std::size_t>(side) * side, 0.0);
    if (!(radius > kSharpRadiusPx)) {
        plane[static_cast<std::size_t>(R) * side + R] = 1.0;
        return;
    }
    const RefBracket b = refBracket(radius);
    for (int p = 0; p < b.passes; ++p) {
        const KernelView kv = lut.kernel(kernelGridRadius(b.node[p]), 0, 0, 0.0f, 0);
        REQUIRE(kv.valid());
        REQUIRE(kv.radiusX <= R);
        REQUIRE(kv.radiusY <= R);
        for (int row = 0; row < kv.rowCount; ++row) {
            const RowSpan& span = kv.row(row);
            if (span.empty())
                continue;
            const int y = kv.rowY(row) + R;
            const float* w = kv.rowWeights(row);
            for (int i = 0; i < span.count(); ++i) {
                const int x = span.xStart + i + R;
                plane[static_cast<std::size_t>(y) * side + x] +=
                    static_cast<double>(w[i]) * b.blend[p];
            }
        }
    }
}

double maxAbsDiff(const std::vector<double>& a, const std::vector<double>& b)
{
    REQUIRE(a.size() == b.size());
    double m = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i)
        m = std::max(m, std::fabs(a[i] - b[i]));
    return m;
}

TEST_CASE("sameScatterKernel is never true for two radii that rasterise differently "
          "(fuzzed against the full blended planes)")
{
    // THE PREDICATE'S ONE HARD PROMISE: "same" means the two radii deposit
    // the same weights into the same pixels, within the 2^-20 the header
    // states.  It is checked here against the rasterised kernel itself --
    // refBracket()'s blend of the two LUT entries, on a dense plane -- and not
    // against any bin arithmetic, so a predicate that collapsed a bracket
    // (nearest node), dropped the node from the key, or widened the blend
    // cell would be caught by the planes and not merely by a changed number.
    //
    // Pairs are generated where the predicate is most likely to be wrong: a
    // separation swept log-uniformly from 1e-9 to 1 px (below one ulp, so a
    // fraction of pairs are exact duplicates), on and one ulp off grid nodes,
    // on and around bracket midpoints, astride the 16px coarse boundary, and
    // in the sharp region below 0.5px where every radius is one kernel.  Two
    // MEASURED-RANGE LUTs cover [2, 40]: edge_softness 1 (the default) and 0
    // (a hard edge, where adjacent nodes differ by a whole pixel's weight and
    // a widened cell shows up first).  Radii below the floor clamp onto the
    // 2px entry -- identical planes that the predicate may still call
    // different, which costs an absorb and never correctness.
    struct Fixture { const char* name; float softness; };
    const Fixture fixtures[] = {{"edge_softness 1", 1.0f}, {"edge_softness 0", 0.0f}};

    Lcg rng(0x5CA77E12ULL);

    // (a, b) pairs, built once and shared by both fixtures.
    std::vector<std::pair<float, float>> pairs;
    const auto nearby = [&](float a, int count) {
        for (int i = 0; i < count; ++i) {
            const float sep  = std::pow(10.0f, rng.range(-9.0f, 0.0f));
            const float sign = (rng.unit() < 0.5f) ? -1.0f : 1.0f;
            pairs.emplace_back(a, a + sign * sep);
        }
    };
    for (int i = 0; i < 6000; ++i) {
        const float a = rng.range(0.5f, 40.0f);
        nearby(a, 2);
        pairs.emplace_back(a, a);
        pairs.emplace_back(a, rng.range(0.5f, 40.0f));
    }
    for (int i = 0; i < 2000; ++i) {                    // the hyperbolic region
        const float a = rng.range(0.5f, 16.0f);
        nearby(a, 2);
    }
    for (int i = 0; i < 1500; ++i) {                    // the sub-0.5px sharp region
        const float a = rng.range(0.0f, 0.5f);
        pairs.emplace_back(a, rng.range(0.0f, 0.5f));
        pairs.emplace_back(a, rng.range(0.5f, 1.0f));
        pairs.emplace_back(a, std::nextafter(kSharpRadiusPx, 0.0f));
    }
    for (int idx = 1; idx <= kKernelFineLastIndex + 48; idx += 1) {   // every node to 40px
        const float node = kernelGridRadius(idx);
        const float next = kernelGridRadius(idx + 1);
        const float mid  = 0.5f * (node + next);
        pairs.emplace_back(node, node);
        pairs.emplace_back(node, std::nextafter(node, 100.0f));
        pairs.emplace_back(node, std::nextafter(node, 0.0f));
        pairs.emplace_back(node, next);
        pairs.emplace_back(mid, mid);
        pairs.emplace_back(mid, std::nextafter(mid, 100.0f));
        pairs.emplace_back(mid, std::nextafter(mid, 0.0f));
        pairs.emplace_back(node, mid);
        nearby(node, 1);
        nearby(mid, 1);
    }
    for (int i = 0; i < 500; ++i) {                     // astride the coarse boundary
        pairs.emplace_back(kKernelCoarseFromPx + rng.range(-0.6f, 0.6f),
                           kKernelCoarseFromPx + rng.range(-0.6f, 0.6f));
        nearby(kKernelCoarseFromPx + rng.range(-0.01f, 0.01f), 1);
    }

    // Independent of any LUT: the header's claim that one radius ulp always
    // moves the blend by more than a cell, so "same bin" is "same radius"
    // above the sharp threshold.  The reference bin agrees on every pair.
    std::size_t sameCount = 0, differentCount = 0;
    for (const auto& pr : pairs) {
        const float a = pr.first, b = pr.second;
        const bool same = sameScatterKernel(a, b);
        const bool bothSharp = !(a > kSharpRadiusPx) && !(b > kSharpRadiusPx);
        CAPTURE(a);
        CAPTURE(b);
        CHECK(same == (bothSharp || a == b));
        CHECK(same == (refKernelBin(static_cast<double>(a))
                       == refKernelBin(static_cast<double>(b))));
        if (same) ++sameCount; else ++differentCount;
    }
    // Neither answer is vacuous over the corpus.
    CHECK(sameCount >= 3000u);
    CHECK(differentCount >= 20000u);

    for (const Fixture& fx : fixtures) {
        CAPTURE(fx.name);
        const DiscKernelLUT lut(2.0f, 40.0f, fx.softness, 1.0f);
        const int R = 42;                               // 40px + half a softness, rounded up

        std::vector<double> planeA, planeB;
        double      worstSame      = 0.0;    // largest plane difference over "same" pairs
        double      smallestOther  = 1.0;    // smallest NON-ZERO difference over "different" pairs
        std::size_t differentPlanes = 0;     // "different" pairs whose planes really differ
        std::size_t identicalPlanes = 0;     // "different" pairs whose planes do not (allowed)
        for (const auto& pr : pairs) {
            const float a = pr.first, b = pr.second;
            refKernelPlane(lut, a, R, planeA);
            refKernelPlane(lut, b, R, planeB);
            const double d = maxAbsDiff(planeA, planeB);
            if (sameScatterKernel(a, b)) {
                CAPTURE(a);
                CAPTURE(b);
                CAPTURE(d);
                // The contract, and its tightest form on this grid.
                CHECK(d <= 1.0e-06);
                CHECK(d == 0.0);
                worstSame = std::max(worstSame, d);
            } else if (d > 0.0) {
                ++differentPlanes;
                smallestOther = std::min(smallestOther, d);
            } else {
                ++identicalPlanes;
            }
        }
        CAPTURE(worstSame);
        CAPTURE(smallestOther);
        CAPTURE(identicalPlanes);
        CHECK(worstSame == 0.0);
        // The negative direction is exercised on thousands of pairs whose
        // planes REALLY differ, including ones a single ulp apart -- so a
        // predicate that merged any of them would have been seen to.
        CHECK(differentPlanes >= 15000u);
        CHECK(smallestOther < 1.0e-06);
    }
}



// Centre-ROW weight sum of one LUT entry, S_r(0).  This is the quantity the
// adjacent-bin trough is made of: two vertically adjacent source scanlines
// that fall in different kernel bins leave their shared destination row short
// by exactly (S_r(0) - S_r'(0))/2, derivable from the LUT alone and matched in
double centreRowSum(const DiscKernelLUT& lut, float radiusPx)
{
    const KernelView v = lut.kernel(radiusPx, 0, 0, 0.0f, 0);
    REQUIRE(v.valid());
    const RowSpan& span = v.row(v.radiusY);         // row y == 0
    REQUIRE_FALSE(span.empty());
    const float* w = v.rowWeights(v.radiusY);
    double sum = 0.0;
    for (int i = 0; i < span.count(); ++i)
        sum += static_cast<double>(w[i]);
    return sum;
}

// S_r(0) from the same float edge weights the LUT builds, normalised in
// double: the LUT's only departure from it is its float normalisation.
double exactCentreRowSum(float radiusPx)
{
    const int reach = static_cast<int>(std::ceil(radiusPx + 0.5f)) + 1;
    double all = 0.0, row = 0.0;
    for (int dy = -reach; dy <= reach; ++dy)
        for (int dx = -reach; dx <= reach; ++dx) {
            const float  d = std::sqrt(static_cast<float>(dx * dx + dy * dy));
            const double e = discEdgeWeight(d, radiusPx, 1.0f);
            all += e;
            if (dy == 0)
                row += e;
        }
    return row / all;
}

TEST_CASE("adjacent kernel bins never lose a visible amount of alpha, pinned at the POD "
          "level")
{
    // THE DEFECT, and the guard against its silent return.  Quantising kernel
    // radius costs a flat opaque surface (S_r(0) - S_r'(0))/2 of its alpha on
    // every scanline where the CoC ramp crosses from bin r to bin r'.  On the
    // uniform 0.5px grid that is a ONE-SCANLINE 20% DARK LINE, with colour
    // tracking alpha exactly, so it reads as a visible dark line across an
    // opaque surface (validation scene (l), checks l1/l2/l5).
    //
    // Nothing in this test knows about Nuke, ramps or scenes: it is the same
    // arithmetic, straight off the LUT, so it fails the instant the grid is
    // coarsened again -- including by someone "simplifying" kernelGridIndex()
    // back to lround(radius / 0.5).
    DiscKernelLUT lut(0.0f, 20.0f, 1.0f, 1.0f);

    // --- 1. THE INSTRUMENT REPRODUCES THE DEFECT -------------------------
    // 0.5 and 1.0 are both nodes of this grid, so this measures a uniform
    // 0.5px step through it: 0.200881, i.e. the 0.799119 measured in Nuke at
    // the 0.5->1.0 crossing.  A test that
    // has never been made to fail proves nothing; this clause is the one that
    // makes the measurement demonstrably able to see the trough.
    CHECK(centreRowSum(lut, 0.5f) - centreRowSum(lut, 1.0f)
          == doctest::Approx(2.0 * 0.200881).epsilon(1e-4));
    // The next three crossings, at the nearest grid nodes to 1.5/2.0/2.5
    // (1.501466 / 2.000000 / 2.497561): Nuke read 0.905153 / 0.948266 /
    // 0.973739 for these.
    CHECK((centreRowSum(lut, 1.0f) - centreRowSum(lut, 1.501466f)) * 0.5
          == doctest::Approx(0.094974).epsilon(1e-3));
    CHECK((centreRowSum(lut, 1.501466f) - centreRowSum(lut, 2.0f)) * 0.5
          == doctest::Approx(0.051607).epsilon(1e-3));
    CHECK((centreRowSum(lut, 2.0f) - centreRowSum(lut, 2.497561f)) * 0.5
          == doctest::Approx(0.026135).epsilon(1e-3));

    // --- 2. THE SHIPPED GRID -------------------------------------------
    // Every ADJACENT pair of entries, over the whole fine region and into the
    // coarse one, reads the trough the grid itself implies: each LUT weight is
    // fl(fl(e) * fl(1/sum)), within 2u of e/sum, so the pair's trough is
    // within (Sa + Sb) * u of the double-normalised one.  The grid is pinned
    // exactly below, so this trough is fixed by it.
    double worst = 0.0;
    float  worstAt = 0.0f;
    for (int i = 0; i + 1 < lut.entryCount(); ++i) {
        const float  ra = lut.entryRadius(i), rb = lut.entryRadius(i + 1);
        const double sa = centreRowSum(lut, ra), sb = centreRowSum(lut, rb);
        const double d  = (sa - sb) * 0.5;
        const double dOracle = (exactCentreRowSum(ra) - exactCentreRowSum(rb)) * 0.5;
        CAPTURE(ra);
        CAPTURE(rb);
        CAPTURE(d);
        CAPTURE(dOracle);
        CHECK(std::fabs(d - dOracle) <= (sa + sb) * kUlp);
        if (d > worst) { worst = d; worstAt = ra; }
    }
    CAPTURE(worst);
    CAPTURE(worstAt);
    for (int i = 1; i <= kKernelFineLastIndex; ++i)
        CHECK(kernelGridRadius(i) == 512.0f / static_cast<float>(1025 - i));
    for (int i = kKernelFineLastIndex + 1; i <= 1400; ++i)
        CHECK(kernelGridRadius(i) == 16.0f + 0.5f * static_cast<float>(i - kKernelFineLastIndex));

    // --- 3. THE GRID ITSELF ----------------------------------------------
    // Strictly increasing, exactly invertible, and never stepping wider than
    // the coarse 0.5px -- the three properties radiusToIndex() and
    // scatterKernelBin() both rest on.
    for (int i = 1; i <= 1400; ++i) {
        CHECK(kernelGridRadius(i) > kernelGridRadius(i - 1));
        CHECK(kernelGridRadius(i) - kernelGridRadius(i - 1)
              <= DiscKernelLUT::kStepPx + 1e-5f);
        CHECK(kernelGridIndex(kernelGridRadius(i)) == i);
    }
    // ...and NEAREST at radii that are NOT nodes, which is the only place the
    // rounding rule is observable and the only place a wrong one hides.  On
    // node radii floor(), ceil() and round() all agree, so the loop above
    // passes unchanged if kernelGridIndex() is mutated to any of them -- this
    // clause is what fails.  Checked against the grid's own definition
    // (kernelGridRadius) by comparing the returned node with its neighbours,
    // in BOTH regions and across the join.
    for (int i = 1; i <= 1400; ++i) {
        const double lo = kernelGridRadius(i);
        const double hi = kernelGridRadius(i + 1);
        for (double t : {0.01, 0.3, 0.499, 0.501, 0.7, 0.99}) {
            const float r = static_cast<float>(lo + t * (hi - lo));
            const int got = kernelGridIndex(r);
            CAPTURE(i);
            CAPTURE(t);
            CAPTURE(r);
            CAPTURE(got);
            REQUIRE(got >= i);
            REQUIRE(got <= i + 1);
            const double dGot  = std::fabs(r - kernelGridRadius(got));
            const double dOther = std::fabs(r - kernelGridRadius(got == i ? i + 1 : i));
            CHECK(dGot <= dOther + 1e-6);
        }
    }
    // The LUT still brackets its requested range from OUTSIDE -- at rMin = 0
    // (what the node passes) and at a measured non-zero rMin, which the grid
    // change made non-trivial: the bracketing nodes are no longer floor/ceil
    // of radius/0.5.
    CHECK(lut.entryRadius(0) <= 0.0f);
    CHECK(lut.entryRadius(lut.entryCount() - 1) >= 20.0f);
    for (auto range : {std::make_pair(2.0f, 40.0f), std::make_pair(0.7f, 0.75f),
                       std::make_pair(15.9f, 16.1f), std::make_pair(0.0f, 0.3f)}) {
        const DiscKernelLUT ranged(range.first, range.second, 1.0f, 1.0f);
        CAPTURE(range.first);
        CAPTURE(range.second);
        REQUIRE(ranged.entryCount() >= 1);
        CHECK(ranged.entryRadius(0) <= range.first);
        CHECK(ranged.entryRadius(ranged.entryCount() - 1) >= range.second);
    }

    // --- 4. THE COST -----------------------------------------------------
    // The refinement lives entirely below kKernelCoarseFromPx, so it adds a
    // FIXED amount no matter how large max_radius is -- measured 197KB at both
    // [0,40] and [0,100], on 0.584MB and 8.414MB LUTs.  Pinned
    // so a future widening of the fine region cannot go unnoticed.
    const DiscKernelLUT big(0.0f, 100.0f, 1.0f, 1.0f);
    const DiscKernelLUT mid(0.0f, 40.0f, 1.0f, 1.0f);
    CHECK(mid.sizeBytes() - 612208u < 210u * 1024u);
    CHECK(big.sizeBytes() - 8822736u < 210u * 1024u);
    CHECK(big.entryCount() < 1200);
}


// FragmentKind, may not merge across a holdout bracket, and cannot take a
// same-kernel collision it is not adjacent to).
// ===========================================================================

namespace {

// An "occludes nothing" holdout: one opaque sample far behind every fixture
// that uses it.  The TRUTH is therefore unchanged by connecting it, which is
// exactly what makes it a parity gate — connecting input 1 stops both merges
// at holdout brackets.
void buildFarHoldout(const HoldoutBoundaries& hb, std::ptrdiff_t pixelCount,
                     float depth, HoldoutSampleSoA& hs, HoldoutLut& lut)
{
    hs.begin(pixelCount);
    for (std::ptrdiff_t i = 0; i < pixelCount; ++i) {
        std::vector<SampleRecord> hv{makeSample(depth, depth, 1.0f, {})};
        hs.appendPixel(hv, 1.0f);
    }
    lut.build(hs, hb);
}

} // namespace


TEST_CASE("with a holdout connected, two sharp samples IN FRONT of a card read the flatten")
{
    // Connecting the holdout stops the merges at brackets, so the two samples
    // reach the stream as two fragments; the stream must still composite them
    // to the plain flatten.  Nothing occludes them — the card is 55 units
    // behind the farther sample.
    const int W = 8, H = 8, K = 16;
    const CocParams    p  = makeManualRig(0.0f, 10.0f);

    // Hand-derived: 0.5 over 0.5.
    const double truth = 0.5 + 0.5 * (1.0 - 0.5);

    for (bool holdout : {false, true}) {
        CAPTURE(holdout);
        FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ true);
        fp.holdoutConnected  = holdout;
        fp.holdoutBoundaries = makeHoldoutBoundaries(p, 1.0f, 100.0f, K);

        float residualT = 1.0f, residualR = 0.0f;
        const SampleSoA soa = flattenOnePixel(fp, 4, 4,
            {makeSample(20.0f, 20.0f, 0.5f, {0.5f}),
             makeSample(40.0f, 40.0f, 0.5f, {0.5f})},
            &residualT, &residualR);

        HoldoutSampleSoA hs;
        HoldoutLut       lut;
        HoldoutSoA       view;
        if (holdout) {
            buildFarHoldout(makeHoldoutBoundaries(p, 1.0f, 100.0f, K), static_cast<std::ptrdiff_t>(W) * H, 95.0f, hs, lut);
            view = lut.view();
        }

        Band band;
        band.C = 1; band.W = W; band.H = H;
        DiscKernelLUT kernel(0.0f, 1.0f, 1.0f, 1.0f);
        ResidualWindow window;
        oneSourcePixelWindow(window, W, H, 4, 4, residualT, residualR);
        runBand(band, makeScatterParams(W, H),
                soa, view, kernel, /*useThread*/ false, &window);

        CHECK(std::fabs(static_cast<double>(band.outAlpha(4, 4)) - truth) <= 2e-07);
        CHECK(std::fabs(static_cast<double>(band.outColor(0, 4, 4)) - truth) <= 2e-07);
    }
}


TEST_CASE("two opaque layers at one pixel read exactly 1.000000 at any alpha pair and any "
          "kernel")
{
    // An opaque layer behind anything is still opaque, and an opaque layer in
    // FRONT of anything hides it: both readings are 1.0 with no coverage
    // fabricated.  Swept over the sharp path and three disc sizes, both
    // pre_merge states, and alpha pairs that put the opaque layer first, last
    // and both.  Read as a FLAT FIELD (every pixel carries the pair), which is
    // the reading that is 1.0 for a blurred pair as well — an ISOLATED pair of
    // different-sized discs band-sums to 2.0 by the recorded
    // occlusion-before-blur loss, which is a separate limitation.
    const int C = 1, W = 40, H = 40;

    for (float sizePx : {0.0f, 0.4f, 3.0f, 8.0f})
    for (bool  preMerge : {false, true})
    for (float a1 : {1.0f, 0.35f})
    for (float a2 : {1.0f, 0.7f}) {
        if (a1 < 1.0f && a2 < 1.0f)
            continue;                           // needs at least one opaque layer
        CAPTURE(sizePx);
        CAPTURE(preMerge);
        CAPTURE(a1);
        CAPTURE(a2);

        const CocParams    p  = makeManualRig(sizePx, 10.0f);
        const FlattenParams fp = makeFlattenParams(p, C, preMerge);

        SampleSoA soa;
        soa.begin(C, fp.groups);
        FlattenScratch scratch;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                std::vector<SampleRecord> v{makeSample(4.0f, 4.0f, a1, {a1}),
                                            makeSample(9.0f, 9.0f, a2, {a2})};
                flattenPixelToSoA(fp, x, y, v, scratch, soa, nullptr, nullptr, nullptr);
            }

        Band band;
        band.C = C; band.W = W; band.H = H;
        DiscKernelLUT kernel(0.0f, std::max(1.0f, sizePx + 1.0f), 1.0f, 1.0f);
        HoldoutSoA noHoldout;
        runBand(band, makeScatterParams(W, H),
                soa, noHoldout, kernel, /*useThread*/ false);

        // 1e-6, NOT equality: the disc LUT's ~5e-8 per-entry normalisation
        // residual over the contributing fragments (the same bound scene (c)'s
        // flat opaque field already carries).  It prints as 1.000000.
        const double got = band.outAlpha(W / 2, H / 2);
        CHECK(std::fabs(got - 1.0) <= 1e-06);
        CHECK(got <= 1.0f);                     // never scaled UP
    }
}


TEST_CASE("no step at the sharp threshold: a 0-2px ramp over a two-layer flat field")
{
    // Validation scene (l).  Without the collision pass this ramp wanders
    // 0.779-0.880 against a truth of 0.700 with a worst step of 1.01e-01, and a
    // fix confined to the sharp path adds a 1.80e-01 jump AT the threshold —
    // which is what disqualifies whole-weight assignment.  The gate is
    // comparative, not absolute: the step across a crossing must not exceed the
    // largest step anywhere else on the ramp.
    const int C = 1, W = 28, H = 28;
    const float z1 = 9.0f, z2 = 11.0f, a1 = 0.5f, a2 = 0.4f, focus = 10.0f;
    const double truth = 1.0 - (1.0 - a1) * (1.0 - a2);      // 0.70, exact

    double prev = 0.0;
    bool   havePrev = false;
    bool   prevSharp1 = true, prevSharp2 = true;
    double worstElsewhere = 0.0, worstCrossing = 0.0, worstError = 0.0;

    for (int step = 0; step <= 24; ++step) {
        const float size = 18.0f * static_cast<float>(step) / 24.0f;
        const CocParams    p  = makeManualRig(size, focus);
        const FlattenParams fp = makeFlattenParams(p, C, /*preMerge*/ true);

        const float r1 = radiusPixels(p, z1);
        const float r2 = radiusPixels(p, z2);
        const float rMax = std::max(r1, r2);
        const int   pad  = static_cast<int>(std::ceil(rMax)) + 2;

        SampleSoA soa;
        soa.begin(C, fp.groups);
        FlattenScratch scratch;
        ResidualWindow window;
        window.allocate(-pad, -pad, W + 2 * pad, H + 2 * pad,
                        autoBackgroundRadiusPx(p, 100.0f));
        flattenIntoWithResidual(fp, -pad, -pad, W + pad, H + pad,
            soa, scratch, window,
            [&](int, int) -> std::vector<SampleRecord> {
                return {makeSample(z1, z1, a1, {a1 * 0.5f}),
                       makeSample(z2, z2, a2, {a2 * 0.5f})};
            });

        Band band;
        band.C = C; band.W = W; band.H = H;
        HoldoutSoA noHoldout;
        DiscKernelLUT kernel(0.0f, std::max(1.0f, rMax), 1.0f, 1.0f);
        runBand(band, makeScatterParams(W, H),
                soa, noHoldout, kernel, /*useThread*/ false, &window);

        const double v = band.outAlpha(W / 2, H / 2);
        worstError = std::max(worstError, std::fabs(v - truth));

        const bool sharp1 = !(r1 > kSharpRadiusPx);
        const bool sharp2 = !(r2 > kSharpRadiusPx);
        if (havePrev) {
            const double d = std::fabs(v - prev);
            if (sharp1 != prevSharp1 || sharp2 != prevSharp2)
                worstCrossing = std::max(worstCrossing, d);
            else
                worstElsewhere = std::max(worstElsewhere, d);
        }
        prev = v; havePrev = true; prevSharp1 = sharp1; prevSharp2 = sharp2;
    }

    // The ramp DOES cross the threshold, or the test proves nothing.
    REQUIRE(worstCrossing >= 0.0);
    CHECK(worstCrossing <= worstElsewhere + 1e-09);
    // And scene (l)'s own wander, which the collision pass also closes:
    // measured 5.3e-03 worst error and 3.4e-03 worst step against 1.80e-01 /
    // 1.01e-01 before.
    CHECK(worstError <= 2e-02);
    CHECK(worstElsewhere <= 2e-02);
}

TEST_CASE("the collision merge does not carry a fragment across a holdout bracket")
{
    // The merge emits ONE fragment at ONE depth and the holdout is sampled per
    // fragment, so with a holdout connected it must not join two samples the
    // card sits between.  Card at z = 30, samples at z = 20 and z = 40: the
    // front one is unoccluded and the back one is fully erased, so the pixel is
    // exactly the front sample's own alpha.
    const int C = 1, W = 8, H = 8, K = 16;
    const CocParams    p  = makeManualRig(0.05f, 10.0f);     // all sharp
    const HoldoutBoundaries hb = makeHoldoutBoundaries(p, 1.0f, 100.0f, K);

    HoldoutSampleSoA hs;
    HoldoutLut       lut;
    buildHoldout(hs, lut, hb, W, H, [](int, int, std::vector<SampleRecord>& out) {
        out.push_back(makeSample(30.0f, 30.0f, 1.0f, {}));
    });

    for (bool connected : {false, true}) {
        CAPTURE(connected);
        FlattenParams fp = makeFlattenParams(p, C, /*preMerge*/ false);
        fp.holdoutConnected  = connected;
        fp.holdoutBoundaries = hb;

        SampleSoA soa;
        soa.begin(C, fp.groups);
        FlattenScratch scratch;
        ResidualWindow window;
        window.allocate(0, 0, W, H, autoBackgroundRadiusPx(p, 100.0f));
        flattenIntoWithResidual(fp, 0, 0, W, H, soa, scratch, window,
            [&](int, int) -> std::vector<SampleRecord> {
                return {makeSample(20.0f, 20.0f, 0.5f, {0.5f}),
                       makeSample(40.0f, 40.0f, 0.5f, {0.5f})};
            });

        const std::size_t perPixel =
            soa.fragmentCount() / static_cast<std::size_t>(W * H);

        Band band;
        band.C = C; band.W = W; band.H = H;
        DiscKernelLUT kernel(0.0f, 1.0f, 1.0f, 1.0f);
        HoldoutSoA view = lut.view();
        runBand(band, makeScatterParams(W, H),
                soa, view, kernel, /*useThread*/ false, &window);

        if (connected) {
            CHECK(perPixel == 2u);
            // Exactly the front sample: the back one is behind an opaque card.
            CHECK(std::fabs(static_cast<double>(band.outAlpha(3, 3)) - 0.5) <= 2e-06);
        } else {
            // Free to merge (nothing samples the depth downstream), and then
            // the whole pixel survives the card it was never told about.
            CHECK(perPixel == 1u);
            CHECK(band.outAlpha(3, 3) > 0.7f);
        }
    }
}

TEST_CASE("pre_merge does not carry a fragment across a holdout bracket either")
{
    // The SAME hazard through the OTHER merge, at the DEFAULT knob settings.
    // Two samples either side of a holdout card can fall inside the 0.25px
    // radius tolerance, and the group's union midpoint then lands BEHIND the
    // card: measured without the gate, alpha 0.000000 against an exact
    // 0.500000, genuinely unoccluded foreground erased outright.
    const int C = 1, W = 8, H = 8, K = 16;
    const CocParams    p  = makeStandardRig(10.0f);
    const HoldoutBoundaries hb = makeHoldoutBoundaries(p, 1.0f, 100.0f, K);

    HoldoutSampleSoA hs;
    HoldoutLut       lut;
    buildHoldout(hs, lut, hb, W, H, [](int, int, std::vector<SampleRecord>& out) {
        out.push_back(makeSample(50.0f, 50.0f, 1.0f, {}));      // opaque card
    });

    const float za = 40.0f, zb = 62.0f;                          // either side of it
    // They really do sit inside the DEFAULT tolerance, or the case is not
    // testing what it claims.
    REQUIRE(std::fabs(radiusPixels(p, za) - radiusPixels(p, zb)) <= 0.25f);

    for (bool preMerge : {false, true}) {
        CAPTURE(preMerge);
        FlattenParams fp = makeFlattenParams(p, C, preMerge);    // 0.25px default
        fp.holdoutConnected  = true;
        fp.holdoutBoundaries = hb;

        SampleSoA soa;
        soa.begin(C, fp.groups);
        FlattenScratch scratch;
        ResidualWindow window;
        window.allocate(0, 0, W, H, autoBackgroundRadiusPx(p, 100.0f));
        flattenIntoWithResidual(fp, 0, 0, W, H, soa, scratch, window,
            [&](int, int) -> std::vector<SampleRecord> {
                return {makeSample(za, za, 0.5f, {0.5f}),
                       makeSample(zb, zb, 0.5f, {0.5f})};
            });
        CHECK(soa.fragmentCount() == static_cast<std::size_t>(W * H) * 2u);

        Band band;
        band.C = C; band.W = W; band.H = H;
        DiscKernelLUT kernel(0.0f, 60.0f, 1.0f, 1.0f);
        HoldoutSoA view = lut.view();
        runBand(band, makeScatterParams(W, H),
                soa, view, kernel, /*useThread*/ false, &window);
        // The front sample survives whole; the back one is behind an opaque card.
        CHECK(std::fabs(static_cast<double>(band.outAlpha(4, 4)) - 0.5) <= 2e-06);
    }
}

// ===========================================================================
// The scatter core
// ===========================================================================


TEST_CASE("flat field identities: opaque field is alpha 1 to 2e-6, "
          "50% fog is 0.5, and the colour:alpha ratio is the input's")
{
    // Validation scene (c) at POD level.  |alpha - 1| <= 2e-6: the disc LUT's
    // per-entry normalisation residual is ~5e-8 over ~113 contributing
    // fragments.
    const CocParams    p  = makeStandardRig(10.0f);
    const int W = 48, H = 48, C = 3;
    DiscKernelLUT lut(0.0f, 40.0f, 1.0f, 1.0f);
    const float unpremult[3] = {0.2f, 0.4f, 0.8f};

    for (float depth : {2.0f, 3.0f}) {
        const float radius = radiusPixels(p, depth);
        const int   pad    = static_cast<int>(std::ceil(radius)) + 3;

        for (float alpha : {1.0f, 0.5f}) {
            const FlattenParams fp = makeFlattenParams(p, C, /*preMerge*/ true);
            SampleSoA soa;
            soa.begin(C, fp.groups);
            FlattenScratch scratch;
            ResidualWindow window;
            window.allocate(-pad, -pad, W + 2 * pad, H + 2 * pad,
                            autoBackgroundRadiusPx(p, 100.0f));
            flattenIntoWithResidual(fp, -pad, -pad, W + pad, H + pad,
                soa, scratch, window,
                [&](int, int) -> std::vector<SampleRecord> {
                    return {makeSample(depth, depth, alpha,
                        {alpha * unpremult[0], alpha * unpremult[1], alpha * unpremult[2]})};
                });

            {
                CAPTURE(depth);
                CAPTURE(alpha);

                Band band;
                band.C = C; band.W = W; band.H = H;
                HoldoutSoA noHoldout;
                runBand(band, makeScatterParams(W, H), soa, noHoldout, lut, true, &window);

                const int cx = W / 2, cy = H / 2;
                const double a = band.outAlpha(cx, cy);
                CHECK(std::fabs(a - alpha) <= 2e-06);
                CHECK(a != doctest::Approx(0.0));

                // THE STANDING INVARIANT: premultiplied colour and alpha must move
                // together.  Unpremultiplying the output must give the input's
                // own colour, to the same 1e-5 the alpha holds to.
                for (int c = 0; c < C; ++c)
                    CHECK(std::fabs(band.outColor(c, cx, cy) / a - unpremult[c])
                          <= 1e-05 * unpremult[c] + 1e-06);
            }
        }
    }
}

TEST_CASE("a checkerboard of two opaque depths reads alpha 1: the nearer claims free area, "
          "the farther what is left")
{
    // Two defocused opaque surfaces interleaved pixel by pixel at different
    // radii.  In depth order the nearer one's deposits take free area, the
    // farther one's the rest, and an opaque deposit adds exactly the free
    // area it takes, so Q == A == min(sum of weights, 1) at every pixel;
    // arrival is the same sum in the same order, so the fill restores any
    // shortfall to 1 within one division.
    const CocParams p = makeStandardRig(10.0f);
    const int W = 48, H = 48, pad = 14;
    DiscKernelLUT lut(0.0f, 40.0f, 1.0f, 1.0f);
    const float dNear = 3.0f, dFar = 3.6f;
    REQUIRE(radiusPixels(p, dNear) > radiusPixels(p, dFar) + 1.0f);

    const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ true);
    SampleSoA soa;
    soa.begin(1, fp.groups);
    FlattenScratch scratch;
    ResidualWindow window;
    window.allocate(-pad, -pad, W + 2 * pad, H + 2 * pad, radiusPixels(p, dFar));
    flattenIntoWithResidual(fp, -pad, -pad, W + pad, H + pad, soa, scratch, window,
        [&](int x, int y) -> std::vector<SampleRecord> {
            const float z = ((x + y) & 1) ? dNear : dFar;
            return {makeSample(z, z, 1.0f, {0.8f})};
        });

    Band band;
    band.C = 1; band.W = W; band.H = H;
    HoldoutSoA noHoldout;
    ScatterParams sp = makeScatterParams(W, H);
    runBand(band, sp, soa, noHoldout, lut, true, &window);

    const float rNear = radiusPixels(p, dNear), rFar = radiusPixels(p, dFar);
    const int reach = static_cast<int>(std::ceil(rNear)) + 3;
    double worstQ = 0.0, worstOut = 0.0;
    for (int y = 16; y < 32; ++y) {
        for (int x = 16; x < 32; ++x) {
            double sum = 0.0;
            int    terms = 0;
            for (int sy = y - reach; sy <= y + reach; ++sy)
                for (int sx = x - reach; sx <= x + reach; ++sx) {
                    const bool near = ((sx + sy) & 1) != 0;
                    const double w = refBlendedWeight(lut, near ? rNear : rFar, x - sx, y - sy);
                    if (w != 0.0) {
                        sum += w;
                        ++terms;
                    }
                }
            const std::size_t i = band.at(x, y);
            CHECK(band.planes.claimed[i] == band.planes.alpha[i]);
            worstQ = std::max(worstQ, std::fabs(static_cast<double>(band.planes.claimed[i])
                                                - std::min(sum, 1.0))
                                      / static_cast<double>(terms));
            worstOut = std::max(worstOut, std::fabs(static_cast<double>(band.outAlpha(x, y)) - 1.0));
            // colour:alpha twin
            CHECK(band.outColor(0, x, y) == doctest::Approx(0.8 * band.outAlpha(x, y)).epsilon(1e-6));
        }
    }
    // Per-term bound: each deposit rounds once into Q, and the reference sums
    // blended taps in double (the scatter's blend itself is within 2 ulps).
    const double ulp = 1.0 / 16777216.0;
    CHECK(worstQ <= 4.0 * ulp);
    CHECK(worstOut <= 2.0 * ulp);
}

TEST_CASE("arrival is bit-identical with and without a holdout LUT connected -- "
          "the deposit precedes the visibility fold")
{
    // arrival must accumulate the RAW kernel weight, before vis is folded in.
    // A semi-transparent card in front of both fragments drops alpha while
    // arrival does not move at all; a vis-folded arrival would differ by a
    // factor of two.
    const int C = 1, W = 10, H = 10, K = 8;
    const CocParams         p  = makeManualRig(0.05f, 10.0f);
    const HoldoutBoundaries hb = makeHoldoutBoundaries(p, 1.0f, 100.0f, K);

    HoldoutSampleSoA hs;
    HoldoutLut       lut;
    buildHoldout(hs, lut, hb, W, H, [](int, int, std::vector<SampleRecord>& out) {
        out.push_back(makeSample(5.0f, 5.0f, 0.5f, {}));   // semi-transparent card
    });

    DiscKernelLUT       kernel(0.0f, 8.0f, 1.0f, 1.0f);
    const ScatterParams sp = makeScatterParams(W, H);

    auto buildSoA = [&]() {
        SampleSoA soa;
        soa.begin(C, makeSingleChannelGroup(C));
        appendFragmentAt(soa, 4, 4, 2.5f, 80.0f, 0.7f, 0.7f, {0.7f * 0.4f});      // a disc
        appendFragmentAt(soa, 7, 6, 0.0f, 80.0f, 0.35f, 0.35f, {0.35f * 0.9f});   // sharp
        return soa;
    };

    Band withHoldout;
    withHoldout.C = C; withHoldout.W = W; withHoldout.H = H;
    HoldoutSoA vis = lut.view();
    runBand(withHoldout, sp, buildSoA(), vis, kernel, /*useThread*/ false);

    Band noHoldout;
    noHoldout.C = C; noHoldout.W = W; noHoldout.H = H;
    HoldoutSoA none;
    runBand(noHoldout, sp, buildSoA(), none, kernel, /*useThread*/ false);

    double alphaWith = 0.0, alphaWithout = 0.0, colorWith = 0.0, colorWithout = 0.0;
    for (std::size_t i = 0; i < static_cast<std::size_t>(W * H); ++i) {
        alphaWith    += withHoldout.planes.alpha[i];
        alphaWithout += noHoldout.planes.alpha[i];
        colorWith    += withHoldout.planes.color[i];
        colorWithout += noHoldout.planes.color[i];
    }
    // Every deposit here lands on free area, so the card halves alpha and
    // colour alike.
    CHECK(alphaWith == doctest::Approx(0.5 * alphaWithout).epsilon(1e-6));
    CHECK(colorWith == doctest::Approx(0.5 * colorWithout).epsilon(1e-6));

    REQUIRE(withHoldout.planes.arrival.size() == noHoldout.planes.arrival.size());
    std::size_t differing = 0;
    for (std::size_t i = 0; i < withHoldout.planes.arrival.size(); ++i)
        if (withHoldout.planes.arrival[i] != noHoldout.planes.arrival[i])
            ++differing;
    CHECK(differing == 0);
}

TEST_CASE("a single fragment's arrival deposits sum to its share within 1e-6")
{
    // The disc kernel's raw weights sum to 1, so arrival summed over an
    // unclipped disc recovers `share`, whatever the alpha -- pinned by a share
    // that is NOT the alpha, at a node radius and between two nodes.
    const int C = 1, W = 40, H = 40;
    DiscKernelLUT       lut(0.0f, 12.0f, 1.0f, 1.0f);
    const ScatterParams sp = makeScatterParams(W, H);
    HoldoutSoA none;

    for (float radius : {5.0f, kernelGridRadius(kernelGridIndex(5.0f)) + 0.37f}) {
        CAPTURE(radius);
        SampleSoA soa;
        soa.begin(C, makeSingleChannelGroup(C));
        appendFragmentAt(soa, W / 2, H / 2, radius, 5.0f, 0.63f, 0.417f, {0.63f * 0.5f});

        Band band;
        band.C = C; band.W = W; band.H = H;
        runBand(band, sp, soa, none, lut);

        double sum = 0.0;
        for (float v : band.planes.arrival)
            sum += static_cast<double>(v);
        CHECK(std::fabs(sum - 0.417) <= 1e-6);
    }
}

TEST_CASE("an ALPHA-ZERO fragment still deposits its colour: the cull is on alpha AND colour")
{
    // A thin fog piece's alpha can underflow to 0 while its colour does not
    // (partitionColorScale's emissive limit), so the scatter culls only a
    // fragment with neither.
    const int W = 12, H = 12;
    DiscKernelLUT lut(0.0f, 8.0f, 1.0f, 1.0f);

    SampleSoA soa;
    soa.begin(1, makeSingleChannelGroup(1));
    appendFragmentAt(soa, W / 2, H / 2, 0.0f, 5.0f, 0.0f, 0.0f, {0.5f});

    Band band;
    band.C = 1; band.W = W; band.H = H;
    HoldoutSoA none;
    runBand(band, makeScatterParams(W, H), soa, none, lut);

    const std::size_t i = band.at(W / 2, H / 2);
    CHECK(band.planes.color[i] == 0.5f);
    CHECK(band.planes.alpha[i] == 0.0f);
    CHECK(band.planes.claimed[i] == 1.0f);
}

TEST_CASE("ScatterStats accounts for every fragment exactly once")
{
    const int W = 20, H = 20;
    DiscKernelLUT lut(0.0f, 10.0f, 1.0f, 1.0f);

    SampleSoA soa;
    soa.begin(1, makeSingleChannelGroup(1));
    appendFragmentAt(soa, 10, 10, 0.0f, 5.0f, 0.8f, 0.8f, {0.8f});   // sharp, inside
    appendFragmentAt(soa,  6,  6, 4.0f, 5.1f, 0.7f, 0.7f, {0.7f});   // disc, inside
    appendFragmentAt(soa, 10, 10, 0.0f, 5.2f, 0.0f, 0.0f, {0.0f});   // nothing -> culled
    appendFragmentAt(soa, -40, 10, 0.0f, 5.3f, 0.5f, 0.5f, {0.5f});  // off-band -> culled

    Band band;
    band.C = 1; band.W = W; band.H = H;
    HoldoutSoA none;
    ScatterStats stats;
    runBand(band, makeScatterParams(W, H), soa, none, lut, false, nullptr, &stats);

    CHECK(stats.fragments == 4u);
    CHECK(stats.sharpFragments == 1u);
    CHECK(stats.culled == 2u);
    CHECK(stats.rowSpans > 0u);
    CHECK(stats.pixelDeposits > 1u);
}

namespace {

// One pixel's finished stream state, resolved.
void resolveOne(float alpha, float color, float arrival, float& outColor, float& outAlpha)
{
    StreamPlanes planes;
    planes.allocate(1, 1, 1);
    planes.alpha[0]   = alpha;
    planes.claimed[0] = alpha;
    planes.color[0]   = color;
    planes.arrival[0] = arrival;
    resolveStreamCPU(planes, &outColor, &outAlpha);
}

} // namespace

TEST_CASE("deficit-only fill: arrival divides the premultiplied pair together, "
          "and only for a real deficit")
{
    const float unpremult = 0.8f;
    float c = -1.0f, a = -1.0f;

    SUBCASE("A = 0.93 at D = 0.93 -> alpha exactly 1, colour:alpha preserved")
    {
        resolveOne(0.93f, 0.93f * unpremult, 0.93f, c, a);
        CHECK(std::fabs(a - 1.0f) <= 1.0f / 16777216.0f);
        REQUIRE(a > 0.0f);
        CHECK(std::fabs(c / a - unpremult) <= 1e-06f);
    }

    SUBCASE("a surplus, D = 1.3, is left untouched and bit-identical to D = 1")
    {
        resolveOne(0.6f, 0.6f * unpremult, 1.3f, c, a);
        float c2 = -1.0f, a2 = -1.0f;
        resolveOne(0.6f, 0.6f * unpremult, 1.0f, c2, a2);
        CHECK(a == 0.6f);
        CHECK(c == 0.6f * unpremult);
        CHECK(a == a2);
        CHECK(c == c2);
    }

    SUBCASE("D = 0 with a zero numerator -> 0, never manufactured")
    {
        resolveOne(0.0f, 0.0f, 0.0f, c, a);
        CHECK(a == 0.0f);
        CHECK(c == 0.0f);
    }

    SUBCASE("the kFillMinArrival boundary, approached from both sides")
    {
        resolveOne(0.5f, 0.5f * unpremult, kFillMinArrival - 1e-6f, c, a);
        CHECK(a == 0.5f);                       // below the floor: untouched
        resolveOne(0.5f, 0.5f * unpremult, kFillMinArrival, c, a);
        CHECK(a == 0.5f);                       // AT the floor: strict '>'
        resolveOne(0.5f, 0.5f * unpremult, kFillMinArrival + 1e-6f, c, a);
        CHECK(a == 1.0f);                       // above: the fill fired, the clamp capped it
        CHECK(std::fabs(c / a - unpremult) <= 1e-06f);
    }

    SUBCASE("a sharp pixel at D = 1 - 1ulp is bit-identical to D = 1; D = 1 - 2e-5 divides")
    {
        float cBase = -1.0f, aBase = -1.0f;
        resolveOne(0.77f, 0.77f * unpremult, 1.0f, cBase, aBase);

        const float oneUlpUnder = std::nextafter(1.0f, 0.0f);
        REQUIRE(!(oneUlpUnder < 1.0f - kFillDeficitTol));
        float cUlp = -1.0f, aUlp = -1.0f;
        resolveOne(0.77f, 0.77f * unpremult, oneUlpUnder, cUlp, aUlp);
        CHECK(aUlp == aBase);
        CHECK(cUlp == cBase);

        float cDiv = -1.0f, aDiv = -1.0f;
        resolveOne(0.77f, 0.77f * unpremult, 1.0f - 2e-5f, cDiv, aDiv);
        CHECK(aDiv != aBase);
        CHECK(aDiv == doctest::Approx(0.77f / (1.0f - 2e-5f)).epsilon(1e-6));
        CHECK(cDiv / aDiv == doctest::Approx(unpremult).epsilon(1e-6));
    }

    SUBCASE("the colour:alpha pair stays locked through the clamp above 1")
    {
        resolveOne(0.75f, 0.75f * unpremult, 0.5f, c, a);
        CHECK(a == 1.0f);
        CHECK(std::fabs(c / a - unpremult) <= 1e-06f);
    }
}

TEST_CASE("colour:alpha ratio is a standing invariant of the resolve over randomised states")
{
    Lcg rng(0x5EEDu);
    for (int iter = 0; iter < 20000; ++iter) {
        const float u = rng.range(0.05f, 1.0f);
        const float alpha = rng.range(0.001f, 1.0f);
        const float arrival = rng.range(0.0f, 1.5f);
        float c = -1.0f, a = -1.0f;
        resolveOne(alpha, alpha * u, arrival, c, a);
        CAPTURE(iter);
        REQUIRE(a >= 0.0f);
        REQUIRE(a <= 1.0f);
        CHECK(std::fabs(c / a - u) <= 6.0f / 16777216.0f * u);
    }
}

TEST_CASE("applyProxyScale scales the PIXEL-unit knobs only, and re-derives the cached members")
{
    // The proxy trap this function exists for: the mm-denominated knobs are
    // resolution-independent for free (CocParams::_formatWidthPx is already
    // Nuke's CURRENT format width), but `max_radius` and Manual `size` are in
    // pixels and are not.  Leaving max_radius unscaled makes a proxy-0.5 render
    // clamp at twice the radius the full-res one does.
    SUBCASE("Manual mode: size and max_radius both halve, and the radius follows")
    {
        CocParams full = makeCocParams(CocMode::Manual, 50.0f, 2.8f, 36.0f,
                                       /*focus*/ 10.0f, unitScale(WorldUnits::Meters),
                                       /*formatWidthPx*/ 1920.0f, 1.0f, 1.0f, 1.0f,
                                       /*maxRadiusPx*/ 100.0f, /*sizePx*/ 10.0f);
        // radius = size * |1 - S/d|, hand-derived: 10 * |1 - 10/2| = 40 -> clamped
        // by max_radius only above 100, so it is unclamped at d = 2.
        REQUIRE(radiusPixels(full, 2.0f) == doctest::Approx(40.0f));

        CocParams proxy = full;
        proxy._formatWidthPx = 960.0f;          // what Nuke reports at proxy 0.5
        proxy.recomputeDerived();
        applyProxyScale(proxy, 0.5f);

        CHECK(proxy._size == doctest::Approx(5.0f));
        CHECK(proxy._maxRadiusPx == doctest::Approx(50.0f));
        CHECK(radiusPixels(proxy, 2.0f) == doctest::Approx(20.0f));   // half, as it must be

        // The clamp really did move with it: at d = 1.05 the unclamped Manual
        // radius is 10*|1 - 10/1.05| = 85.2 full-res, i.e. below the full-res
        // clamp of 100 but above the proxy clamp of 50.
        CHECK(radiusPixels(full, 1.05f) == doctest::Approx(85.2381f).epsilon(1e-4));
        CHECK(radiusPixels(proxy, 1.05f) == doctest::Approx(42.619f).epsilon(1e-4));
    }

    SUBCASE("Physical mode: the mm knobs are untouched; only the pixel clamp moves")
    {
        CocParams p = makeStandardRig(10.0f, /*maxRadiusPx*/ 100.0f);
        const float focalBefore = p._focalLengthMm;
        const float filmBefore  = p._filmbackWidthMm;
        applyProxyScale(p, 0.5f);
        CHECK(p._focalLengthMm == focalBefore);
        CHECK(p._filmbackWidthMm == filmBefore);
        CHECK(p._maxRadiusPx == doctest::Approx(50.0f));
        // _pxPerMm is a DERIVED member: it must have been recomputed, not left
        // at whatever the pre-scale assignment produced.
        CHECK(p._pxPerMm == doctest::Approx(p._formatWidthPx / p._filmbackWidthMm));
    }

    SUBCASE("a scale of 1, 0, a negative or a NaN is a no-op, never a collapsed blur")
    {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        for (float scale : {1.0f, 0.0f, -0.5f, nan}) {
            CAPTURE(scale);
            CocParams p = makeStandardRig(10.0f, 100.0f);
            applyProxyScale(p, scale);
            CHECK(p._maxRadiusPx == 100.0f);
            CHECK(p._size == 10.0f);
        }
    }
}

TEST_CASE("SampleSoA lifecycle: clear() keeps the allocation, release() drops it")
{
    // The band loop reuses these buffers across bands and across cooks, so a
    // clear() that freed would re-malloc hundreds of megabytes per band.
    SampleSoA soa;
    soa.begin(3, makeSingleChannelGroup(3));
    soa.reserveFragments(256);
    const std::size_t reserved = soa.sizeBytes();
    CHECK(reserved > 0u);

    FragmentRecord f;
    f.x = 1; f.y = 2; f.radius = 3.0f; f.depth = 4.0f; f.alpha = 0.5f;
    const float ch[3] = {0.1f, 0.2f, 0.3f};
    for (int i = 0; i < 10; ++i)
        soa.appendFragment(f, ch);
    CHECK(soa.fragmentCount() == 10u);

    soa.clear();
    CHECK(soa.fragmentCount() == 0u);
    CHECK(soa.sizeBytes() == reserved);         // capacity KEPT

    soa.release();
    CHECK(soa.sizeBytes() == 0u);
    CHECK(soa.fragmentCount() == 0u);

    // begin() re-establishes the channel layout and empties the arrays.
    soa.begin(2, makeSingleChannelGroup(2));
    CHECK(soa.channelCount == 2);
    CHECK(soa.fragmentCount() == 0u);
}


TEST_CASE("HoldoutLut::build folds the in-span exponential in exactly at the boundaries, "
          "and agrees with evalBoundaries on overlapping and unsorted input")
{
    const CocParams    p  = makeStandardRig(10.0f);
    const HoldoutBoundaries hb = makeStandardHoldoutBoundaries(p);
    REQUIRE(hb.count() == 17);

    SUBCASE("a single volumetric holdout: the LUT equals the exact eval at every boundary")
    {
        HoldoutSampleSoA samples;
        HoldoutLut lut;
        buildHoldout(samples, lut, hb, 2, 1, [](int x, int, std::vector<SampleRecord>& out) {
            if (x == 0)
                out.push_back(makeSample(20.0f, 60.0f, 0.75f));
        });
        const HoldoutSoA view = lut.view();
        REQUIRE(view.enabled());

        const float zf = 20.0f, zb = 60.0f, a = 0.75f;
        double worst = 0.0;
        for (int b = 0; b < view.boundaryCount(); ++b) {
            const float got = view.pixelLut(0)[b];
            const float want = HoldoutVisibility::evalExact(&zf, &zb, &a, 1, hb.boundary(b));
            worst = std::max(worst, static_cast<double>(std::fabs(got - want)));
        }
        CHECK(worst == 0.0);            // exact: build() IS the fold-in

        // A pixel with no holdout samples is transmittance 1 everywhere.
        for (int b = 0; b < view.boundaryCount(); ++b)
            CHECK(view.pixelLut(1)[b] == 1.0f);
    }

    SUBCASE("build() == evalBoundaries() on OVERLAPPING and UNSORTED input (fuzz)")
    {
        // Neither is assumed: sorting is a fast-path precondition only, and the
        // fallback must be bit-identical, not merely close.
        Lcg rng(0x0B0D5E7Du);
        std::vector<float> fast(hb.count()), slow(hb.count());
        std::size_t differing = 0;

        for (int trial = 0; trial < 4000; ++trial) {
            const int n = rng.intRange(1, 6);
            std::vector<float> zf(n), zb(n), al(n);
            for (int i = 0; i < n; ++i) {
                zf[i] = rng.range(0.5f, 110.0f);
                zb[i] = (rng.unit() < 0.5f) ? zf[i] : zf[i] + rng.range(0.0f, 60.0f);
                al[i] = rng.range(0.0f, 1.0f);
            }
            // Deliberately NOT sorted, and deliberately overlapping.
            HoldoutVisibility::build(zf.data(), zb.data(), al.data(), n,
                                     hb.boundaries(), hb.count(), fast.data());
            HoldoutVisibility::evalBoundaries(zf.data(), zb.data(), al.data(), n,
                                              hb.boundaries(), hb.count(), slow.data());
            for (int b = 0; b < hb.count(); ++b)
                if (fast[b] != slow[b])
                    ++differing;
        }
        CHECK(differing == 0);
    }
}

TEST_CASE("opaque POINT-sample holdout accuracy on the default rig (the shape that exposed "
          "the decoupled boundary set)")
{
    // The commonest holdout there is: a solid card.  Read against the ΔCoC
    // bucket boundaries this card at z=50 starts occluding at z=10.9 and erases
    // a fragment at z=15 by 98%.  Against the decoupled uniform-in-z set it
    // must be essentially unattenuated in front of the card and fully
    // attenuated behind it.
    const CocParams    p  = makeStandardRig(10.0f);
    const HoldoutBoundaries hb = makeStandardHoldoutBoundaries(p);

    HoldoutSampleSoA samples;
    HoldoutLut lut;
    buildHoldout(samples, lut, hb, 1, 1, [](int, int, std::vector<SampleRecord>& out) {
        out.push_back(makeSample(50.0f, 50.0f, 1.0f));
    });
    const HoldoutSoA view = lut.view();
    REQUIRE(view.enabled());

    auto vis = [&](float z) {
        const BoundarySpan s = view.locate(z);
        return HoldoutVisibility::interpAtBucket(view.pixelLut(0), view.boundaryCount(),
                                                 s.index, s.frac);
    };

    // In front of the card: 1.000000 exactly (was 0.0215 at z=15 / 0.0 at 30/40).
    for (float z : {5.0f, 15.0f, 30.0f, 40.0f, 44.0f})
        CHECK(vis(z) == 1.0f);
    // Behind it: exactly 0.
    for (float z : {51.0f, 60.0f, 99.0f})
        CHECK(vis(z) == 0.0f);

    // The PINNED bite depth: the first depth reading below half visibility.
    // 44.37 against a true 50, i.e. inside the card's own 6.19-unit bracket
    // [44.3125, 50.5].  Read against the ΔCoC bucket set instead it is 10.9.
    float bite = 0.0f;
    for (int i = 0; i < 100000; ++i) {
        const float z = 1.0f + (99.0f * i) / 100000.0f;
        if (vis(z) < 0.5f) { bite = z; break; }
    }
    CAPTURE(bite);
    CHECK(bite > 44.0f);
    CHECK(bite < 44.7f);
    // The residue, stated plainly and pinned: the card erases the rest of its
    // own bracket, ~5 units of genuinely unoccluded geometry.
    CHECK(50.0f - bite > 5.0f);
    CHECK(50.0f - bite < 6.19f);
}

TEST_CASE("holdout SoA hygiene: NaN depths dropped, +/-inf kept, depthScale round-trips")
{
    const CocParams    p  = makeStandardRig(10.0f);
    const HoldoutBoundaries hb = makeStandardHoldoutBoundaries(p);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    SUBCASE("a NaN-DeepFront opaque sample is DROPPED, not clamped to 0")
    {
        // Clamping it to 0 (what the source flatten does) would put it in front
        // of boundary(0) and drive the whole pixel's LUT to zero -- a black hole
        // in the plate.
        HoldoutSampleSoA samples;
        HoldoutLut lut;
        buildHoldout(samples, lut, hb, 1, 1, [&](int, int, std::vector<SampleRecord>& out) {
            out.push_back(makeSample(nan, nan, 1.0f));
        });
        CHECK(samples.sampleCount == 0u);
        CHECK_FALSE(lut.view().enabled());          // nothing to build: the free path
    }

    SUBCASE("a NaN sample alongside a real one leaves the real one intact")
    {
        HoldoutSampleSoA samples;
        HoldoutLut lut;
        buildHoldout(samples, lut, hb, 1, 1, [&](int, int, std::vector<SampleRecord>& out) {
            out.push_back(makeSample(nan, 30.0f, 1.0f));
            out.push_back(makeSample(60.0f, 60.0f, 0.5f));
        });
        REQUIRE(samples.sampleCount == 1u);
        const HoldoutSoA view = lut.view();
        REQUIRE(view.enabled());
        CHECK(view.pixelLut(0)[0] == 1.0f);                     // nothing in front of z=60
        CHECK(view.pixelLut(0)[view.boundaryCount() - 1] == doctest::Approx(0.5f));
    }

    SUBCASE("appendPixel's ascending sort is LOAD-BEARING, not just a fast path")
    {
        // HoldoutVisibility::build() (and evalBoundaries() with it) walks the
        // samples in the order it is given, and for OVERLAPPING volumetric
        // spans that walk is order-dependent by far more than rounding --
        // measured 2.7e-02 ABSOLUTE (0.1117 against 0.0851) between an
        // ascending and a descending presentation of the same six spans over a
        // 200k-trial corpus.  The existing
        // build()-vs-evalBoundaries() fuzz cannot see that: both sides consume
        // the same order, so it pins their agreement, not the order.  So the
        // sort in appendPixel() is what makes a pixel's LUT a function of its
        // sample SET rather than of the order Nuke happened to hand them over.
        auto build = [&](bool reversed) {
            HoldoutSampleSoA samples;
            HoldoutLut lut;
            buildHoldout(samples, lut, hb, 1, 1, [&](int, int, std::vector<SampleRecord>& out) {
                const float zf[6] = {30.0f, 12.0f, 55.0f, 20.0f, 41.0f, 25.0f};
                const float zb[6] = {60.0f, 44.0f, 70.0f, 33.0f, 52.0f, 25.0f};
                const float a[6]  = {0.55f, 0.30f, 0.80f, 0.45f, 0.25f, 0.90f};
                for (int i = 0; i < 6; ++i) {
                    const int k = reversed ? (5 - i) : i;
                    out.push_back(makeSample(zf[k], zb[k], a[k]));
                }
            });
            const HoldoutSoA v = lut.view();
            REQUIRE(v.enabled());
            return std::vector<float>(v.pixelLut(0), v.pixelLut(0) + v.boundaryCount());
        };

        const std::vector<float> forward = build(false);
        const std::vector<float> reverse = build(true);
        CHECK(forward == reverse);              // bit-identical, not merely close

        // And the arrays really are ascending, which is the precondition
        // build()'s single-walk fast path is documented against.
        HoldoutSampleSoA samples;
        HoldoutLut lut;
        buildHoldout(samples, lut, hb, 1, 1, [](int, int, std::vector<SampleRecord>& out) {
            out.push_back(makeSample(60.0f, 70.0f, 0.5f));
            out.push_back(makeSample(20.0f, 30.0f, 0.5f));
            out.push_back(makeSample(40.0f, 50.0f, 0.5f));
        });
        REQUIRE(samples.sampleCount == 3u);
        for (std::size_t i = 1; i < samples.sampleCount; ++i)
            CHECK(samples.zFront[i] >= samples.zFront[i - 1]);
    }

    SUBCASE("a NaN DeepBACK drops the sample too, not just a NaN DeepFront")
    {
        // Only the front was covered before.  A NaN back survives sanitisation
        // as zBack = zFront (sanitizeSampleDepth maps it to 0, then the
        // `!(zb > zf)` collapse), so the sample silently becomes a POINT
        // holdout at its own front depth -- an opaque card where the input said
        // "unknown", which is a black hole in the plate for everything behind
        // it.  Dropping it is the documented behaviour.
        HoldoutSampleSoA samples;
        HoldoutLut lut;
        buildHoldout(samples, lut, hb, 1, 1, [&](int, int, std::vector<SampleRecord>& out) {
            out.push_back(makeSample(30.0f, nan, 1.0f));
        });
        CHECK(samples.sampleCount == 0u);
        CHECK_FALSE(lut.view().enabled());
    }

    SUBCASE("a short appendPixel run leaves the missing pixels at transmittance 1")
    {
        // pixelOffset is resizeUninitialized()'d, so a build() that trusted
        // pixelCount over pixelsAppended would read garbage CSR offsets and
        // index the sample arrays with them.  The documented behaviour is to
        // treat the un-appended tail as zero-sample.
        HoldoutSampleSoA samples;
        HoldoutLut lut;
        const std::ptrdiff_t px = 8;
        samples.begin(px);
        std::vector<SampleRecord> one{makeSample(20.0f, 40.0f, 0.8f)};
        samples.appendPixel(one);                    // ONE of eight pixels
        CHECK(samples.pixelsAppended == 1);
        lut.build(samples, hb);

        const HoldoutSoA view = lut.view();
        REQUIRE(view.enabled());
        REQUIRE(view.pixelCount == px);
        // Pixel 0 carries the card; every other pixel is untouched plate.
        CHECK(view.pixelLut(0)[view.boundaryCount() - 1] == doctest::Approx(0.2f));
        for (std::ptrdiff_t i = 1; i < px; ++i)
            for (int b = 0; b < view.boundaryCount(); ++b) {
                CAPTURE(i);
                CAPTURE(b);
                REQUIRE(view.pixelLut(i)[b] == 1.0f);
            }
    }

    SUBCASE("+inf is a legitimate far-field holdout and is KEPT")
    {
        HoldoutSampleSoA samples;
        HoldoutLut lut;
        buildHoldout(samples, lut, hb, 1, 1, [&](int, int, std::vector<SampleRecord>& out) {
            out.push_back(makeSample(inf, inf, 1.0f));
        });
        CHECK(samples.sampleCount == 1u);
        const HoldoutSoA view = lut.view();
        REQUIRE(view.enabled());
        for (int b = 0; b < view.boundaryCount(); ++b)
            CHECK(view.pixelLut(0)[b] == 1.0f);                 // it occludes nothing in range
    }

    SUBCASE("alpha <= 0 samples are dropped (they can only contribute a factor of 1)")
    {
        HoldoutSampleSoA samples;
        HoldoutLut lut;
        buildHoldout(samples, lut, hb, 1, 1, [&](int, int, std::vector<SampleRecord>& out) {
            out.push_back(makeSample(20.0f, 30.0f, 0.0f));
            out.push_back(makeSample(40.0f, 40.0f, -1.0f));
        });
        CHECK(samples.sampleCount == 0u);
    }

    SUBCASE("depthScale round-trip: pre-scaled samples at scale 1 == raw samples at that scale")
    {
        const float scale = 0.6789f;
        HoldoutSampleSoA rawSamples, preSamples;
        HoldoutLut rawLut, preLut;

        buildHoldout(rawSamples, rawLut, hb, 3, 1, [](int x, int, std::vector<SampleRecord>& out) {
            out.push_back(makeSample(20.0f + 5.0f * x, 45.0f + 5.0f * x, 0.7f));
        }, scale);
        buildHoldout(preSamples, preLut, hb, 3, 1, [&](int x, int, std::vector<SampleRecord>& out) {
            out.push_back(makeSample((20.0f + 5.0f * x) * scale, (45.0f + 5.0f * x) * scale, 0.7f));
        }, 1.0f);

        const HoldoutSoA rawView = rawLut.view();
        const HoldoutSoA preView = preLut.view();
        REQUIRE(rawView.enabled());
        REQUIRE(preView.enabled());
        std::size_t differing = 0;
        for (int i = 0; i < 3; ++i)
            for (int b = 0; b < rawView.boundaryCount(); ++b)
                if (rawView.pixelLut(i)[b] != preView.pixelLut(i)[b])
                    ++differing;
        CHECK(differing == 0);

        // And a scale of 1 (or a garbage one) is the identity.
        HoldoutSampleSoA plainSamples;
        HoldoutLut plainLut;
        buildHoldout(plainSamples, plainLut, hb, 3, 1, [](int x, int, std::vector<SampleRecord>& out) {
            out.push_back(makeSample(20.0f + 5.0f * x, 45.0f + 5.0f * x, 0.7f));
        }, 1.0f);
        HoldoutSampleSoA badSamples;
        HoldoutLut badLut;
        buildHoldout(badSamples, badLut, hb, 3, 1, [](int x, int, std::vector<SampleRecord>& out) {
            out.push_back(makeSample(20.0f + 5.0f * x, 45.0f + 5.0f * x, 0.7f));
        }, -3.0f);
        differing = 0;
        for (int i = 0; i < 3; ++i)
            for (int b = 0; b < plainLut.view().boundaryCount(); ++b)
                if (plainLut.view().pixelLut(i)[b] != badLut.view().pixelLut(i)[b])
                    ++differing;
        CHECK(differing == 0);
    }
}

TEST_CASE("the holdout multiplies into the scatter's deposits, per DESTINATION pixel, "
          "on both the sharp and the disc path")
{
    const CocParams    p  = makeStandardRig(10.0f);
    const HoldoutBoundaries hb = makeStandardHoldoutBoundaries(p);
    const int W = 40, H = 24;
    DiscKernelLUT lut(0.0f, 30.0f, 1.0f, 1.0f);
    const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ true);

    // A card covering the LEFT half of the band only, so the per-pixel LUT row
    // actually matters: a scatter reading pixel 0's row for every pixel would
    // attenuate the whole disc.
    auto leftHalfCard = [&](int x, int, std::vector<SampleRecord>& out) {
        if (x < W / 2)
            out.push_back(makeSample(4.0f, 4.0f, 1.0f));
    };

    SUBCASE("deposits match the independent rasterisation, disc path")
    {
        HoldoutSampleSoA samples;
        HoldoutLut hlut;
        buildHoldout(samples, hlut, hb, W, H, leftHalfCard);
        const HoldoutSoA view = hlut.view();
        REQUIRE(view.enabled());

        SampleSoA soa;
        soa.begin(1, fp.groups);
        FlattenScratch scratch;
        // Behind the card (z=6 > 4): its disc straddles the card's edge.
        std::vector<SampleRecord> behind{makeSample(6.0f, 6.0f, 0.9f, {0.45f})};
        flattenPixelToSoA(fp, W / 2, H / 2, behind, scratch, soa, nullptr, nullptr, nullptr);
        // In front of the card (z=3): unattenuated.
        std::vector<SampleRecord> front{makeSample(3.0f, 3.0f, 0.8f, {0.4f})};
        flattenPixelToSoA(fp, W / 2 - 4, H / 2, front, scratch, soa, nullptr, nullptr, nullptr);

        const ScatterParams sp = makeScatterParams(W, H);
        Band band;
        band.C = 1; band.W = W; band.H = H;
        runBand(band, sp, soa, view, lut);

        ExpectedState want;
        want.allocate(1, W, H);
        refRasterize(want, sp, soa, lut, &view);
        checkState(band.planes, want, 4.0);
    }

    SUBCASE("a fragment fully behind an opaque holdout deposits EXACTLY zero (sharp and disc)")
    {
        HoldoutSampleSoA samples;
        HoldoutLut hlut;
        buildHoldout(samples, hlut, hb, W, H, [](int, int, std::vector<SampleRecord>& out) {
            out.push_back(makeSample(4.0f, 4.0f, 1.0f));
        });
        const HoldoutSoA view = hlut.view();
        REQUIRE(view.enabled());

        // Both depths sit in the LUT bracket [7.1875, 13.375], whose NEAR
        // boundary is already fully occluded by the card, so vis is exactly 0
        // across the whole bracket under every interpolant.  (Inside the
        // card's OWN bracket the log chord leaves a ~1e-25 residue rather than
        // a hard zero -- that is the documented chord behaviour, pinned by the
        // point-holdout accuracy case above, not something to assert as 0 here.)
        for (float depth : {9.0f /* sharp: r=0.27px */, 8.0f /* disc: r=0.60px */}) {
            CAPTURE(depth);
            REQUIRE((radiusPixels(p, depth) < kSharpRadiusPx) == (depth > 8.5f));
            const SampleSoA soa = flattenOnePixel(fp, W / 2, H / 2,
                {makeSample(depth, depth, 1.0f, {0.5f})});
            Band band;
            band.C = 1; band.W = W; band.H = H;
            runBand(band, makeScatterParams(W, H),
                    soa, view, lut);
            CHECK(bandAlphaSum(band) == 0.0);
            CHECK(bandColorSum(band, 0) == 0.0);
        }
    }

    SUBCASE("a fragment fully in front is BIT-IDENTICAL to the no-holdout path (sharp and disc)")
    {
        HoldoutSampleSoA samples;
        HoldoutLut hlut;
        buildHoldout(samples, hlut, hb, W, H, [](int, int, std::vector<SampleRecord>& out) {
            out.push_back(makeSample(80.0f, 80.0f, 1.0f));
        });
        const HoldoutSoA view = hlut.view();
        REQUIRE(view.enabled());

        for (float depth : {9.0f, 3.0f}) {
            CAPTURE(depth);
            const SampleSoA soa = flattenOnePixel(fp, W / 2, H / 2,
                {makeSample(depth, depth, 0.8f, {0.4f})});
            const ScatterParams sp = makeScatterParams(W, H);

            Band withHoldout, without;
            withHoldout.C = 1; withHoldout.W = W; withHoldout.H = H;
            without.C = 1; without.W = W; without.H = H;
            HoldoutSoA disabled;
            runBand(withHoldout, sp, soa, view, lut);
            runBand(without, sp, soa, disabled, lut);

            std::size_t differing = 0;
            for (std::size_t i = 0; i < without.planes.alpha.size(); ++i) {
                if (withHoldout.planes.alpha[i] != without.planes.alpha[i]) ++differing;
                if (withHoldout.planes.claimed[i] != without.planes.claimed[i]) ++differing;
            }
            for (std::size_t i = 0; i < without.planes.color.size(); ++i)
                if (withHoldout.planes.color[i] != without.planes.color[i]) ++differing;
            CHECK(differing == 0);
        }
    }

    SUBCASE("an all-ones LUT is bit-identical to the disabled path")
    {
        HoldoutSampleSoA samples;
        HoldoutLut hlut;
        // One sample far behind everything: every boundary transmittance is 1.
        buildHoldout(samples, hlut, hb, W, H, [](int, int, std::vector<SampleRecord>& out) {
            out.push_back(makeSample(99.5f, 99.5f, 1.0f));
        });
        const HoldoutSoA view = hlut.view();
        REQUIRE(view.enabled());
        for (int b = 0; b + 1 < view.boundaryCount(); ++b)
            REQUIRE(view.pixelLut(0)[b] == 1.0f);

        const SampleSoA soa = flattenOnePixel(fp, W / 2, H / 2,
            {makeSample(3.0f, 3.0f, 0.8f, {0.4f})});
        const ScatterParams sp = makeScatterParams(W, H);

        Band a, b;
        a.C = 1; a.W = W; a.H = H;
        b.C = 1; b.W = W; b.H = H;
        HoldoutSoA disabled;
        runBand(a, sp, soa, view, lut);
        runBand(b, sp, soa, disabled, lut);

        std::size_t differing = 0;
        for (std::size_t i = 0; i < a.planes.alpha.size(); ++i)
            if (a.planes.alpha[i] != b.planes.alpha[i]) ++differing;
        for (std::size_t i = 0; i < a.planes.color.size(); ++i)
            if (a.planes.color[i] != b.planes.color[i]) ++differing;
        CHECK(differing == 0);
    }

    SUBCASE("a LUT that does not cover the whole band is treated as ABSENT, never read OOB")
    {
        HoldoutSampleSoA samples;
        HoldoutLut hlut;
        buildHoldout(samples, hlut, hb, W, H / 2, [](int, int, std::vector<SampleRecord>& out) {
            out.push_back(makeSample(4.0f, 4.0f, 1.0f));
        });
        HoldoutSoA view = hlut.view();
        REQUIRE(view.enabled());
        REQUIRE(view.pixelCount < static_cast<std::ptrdiff_t>(W) * H);

        float residualT = 1.0f, residualR = 0.0f;
        const SampleSoA soa = flattenOnePixel(fp, W / 2, H / 2,
            {makeSample(6.0f, 6.0f, 0.8f, {0.4f})}, &residualT, &residualR);
        Band band;
        band.C = 1; band.W = W; band.H = H;
        ResidualWindow window;
        oneSourcePixelWindow(window, W, H, W / 2, H / 2, residualT, residualR);
        runBand(band, makeScatterParams(W, H),
                soa, view, lut, true, &window);
        // Not erased: the short LUT was ignored rather than sampled.
        CHECK(bandAlphaSum(band) == doctest::Approx(0.8).epsilon(1e-5));
    }
}

TEST_CASE("the dense alpha<1 holdout underflow reaches DEPOSITED PIXELS on both scatter "
          "paths, and the floored chord's reading there is banded two-sided")
{
    // An underflowed bracket is NOT confined to fully-opaque content: 46 point
    // samples at alpha=0.9 packed inside one K=16 bracket underflow the stored
    // far-boundary transmittance to bitwise 0.  What is pinned is the log
    // chord's floored deposit -- in deposited pixels, not just in the LUT math,
    // and on both the sharp and the disc path.
    const CocParams    p  = makeStandardRig(10.0f);
    const HoldoutBoundaries hb = makeStandardHoldoutBoundaries(p);
    const int W = 40, H = 24;
    DiscKernelLUT lut(0.0f, 30.0f, 1.0f, 1.0f);

    // The bracket the 46 samples are packed into.
    const float lo = hb.boundary(7), hi = hb.boundary(8);
    REQUIRE(lo < 48.0f);
    REQUIRE(hi > 48.0f);

    HoldoutSampleSoA samples;
    HoldoutLut hlut;
    buildHoldout(samples, hlut, hb, W, H, [&](int, int, std::vector<SampleRecord>& out) {
        for (int i = 0; i < 46; ++i) {
            const float z = lo + (hi - lo) * (i + 0.5f) / 46.0f;
            out.push_back(makeSample(z, z, 0.9f));
        }
    });
    const HoldoutSoA view = hlut.view();
    REQUIRE(view.enabled());
    // The far boundary really did underflow to bitwise zero with no sample
    // anywhere near alpha 1 -- that is the whole point of the fixture.
    REQUIRE(view.pixelLut(0)[7] == 1.0f);
    REQUIRE(view.pixelLut(0)[8] == 0.0f);

    // A fragment at z=48 sits inside that bracket.  Two rigs: one sharp
    // (radius 0 by construction) and one with a real disc, since the two paths
    // read the LUT through different code.
    struct PathCase { const char* name; float radius; };
    const PathCase paths[] = {{"sharp", 0.0f}, {"disc", 6.0f}};

    for (const PathCase& path : paths) {
        CAPTURE(path.name);
        SampleSoA soa;
        soa.begin(1, makeSingleChannelGroup(1));
        appendFragmentAt(soa, W / 2, H / 2, path.radius, 48.0f, 1.0f, 1.0f, {0.5f},
                         FragmentKind::Volumetric);

        Band band;
        band.C = 1; band.W = W; band.H = H;
        runBand(band, makeScatterParams(W, H), soa, view, lut);
        const double got = vecSum(std::vector<float>(band.planes.alpha.data(),
                                                     band.planes.alpha.data() + band.planes.alpha.size()));

        // Read before the fill (which would divide each pixel by its own
        // weight here).  TWO-SIDED band: a one-sided `< 1e-12` cannot tell the
        // floored chord from an outright 0.  The fragment's whole kernel weight (sums to 1
        // on both paths) is scaled by the floored chord's 10^(-30*frac) at
        // z=48, frac ~0.59596 -> ~1.32e-18.  A regression to hard erasure
        // (0.0) fails the lower bound; a raised/lost floor leaks and fails
        // the upper bound.  Mutation-tested in both directions.
        CHECK(got > 1.0e-18);
        CHECK(got < 1.7e-18);
    }
}

// ===========================================================================
// Shared-code regression: tidyOverlapping()'s termination
// ===========================================================================

TEST_CASE("tidyOverlapping terminates and stays bounded on tie-heavy randomised input")
{
    // Non-termination here hangs Nuke UNKILLABLY on ordinary volumetric input.
    // The failure mode to guard: a split loop that always cuts samples[i] at
    // samples[i+1].zFront makes no progress when the two share a front -- and
    // creates that configuration itself.  Depths are drawn from
    // a SMALL DISCRETE SET here so exact ties (shared fronts, shared backs,
    // fully coincident spans) are the common case rather than a corner.
    //
    // The assertion is termination plus a bounded output: with E distinct
    // endpoints no span can be cut into more than E-1 pieces, so n spans can
    // never yield more than n*(E-1) records.  A non-terminating build hangs
    // here instead of hanging a compositor.
    Lcg rng(0xDEADBEEFu);
    const float depths[] = {1.0f, 1.0f, 2.0f, 3.0f, 3.0f, 5.0f, 8.0f, 13.0f};
    const int   nDepths  = static_cast<int>(sizeof(depths) / sizeof(depths[0]));

    for (int trial = 0; trial < 3000; ++trial) {
        const int n = rng.intRange(2, 12);
        std::vector<SampleRecord> samples;
        samples.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            const float a = depths[rng.intRange(0, nDepths - 1)];
            const float b = depths[rng.intRange(0, nDepths - 1)];
            SampleRecord s;
            s.zFront = std::min(a, b);
            s.zBack  = std::max(a, b);
            s.alpha  = rng.range(0.0f, 1.0f);
            s.channels = {s.alpha * rng.unit(), s.alpha * rng.unit()};
            samples.push_back(s);
        }

        tidyOverlapping(samples);

        // Bounded: at most one piece per distinct endpoint interval per input.
        REQUIRE(samples.size() <= static_cast<std::size_t>(n) * static_cast<std::size_t>(nDepths));
        // Tidy: sorted, non-inverted, and no two records partially overlap.
        for (std::size_t i = 0; i < samples.size(); ++i) {
            REQUIRE(samples[i].zBack >= samples[i].zFront);
            REQUIRE(std::isfinite(samples[i].alpha));
        }
        for (std::size_t i = 1; i < samples.size(); ++i) {
            const SampleRecord& a = samples[i - 1];
            const SampleRecord& b = samples[i];
            REQUIRE(a.zFront <= b.zFront);
            const bool disjoint  = !(b.zFront < a.zBack);
            const bool identical = (a.zFront == b.zFront) && (a.zBack == b.zBack);
            REQUIRE((disjoint || identical));
        }
    }
}

// ===========================================================================
//
//  Per-band lazy-claim concurrency
//
//  BandLedger is the SAME template the node instantiates over DD::Image::
//  SignalLock; here it runs over a std::mutex/std::condition_variable
//  monitor, driven by real std::threads, so the claim/wait/abort logic that
//  ships is the logic pinned here.  bandBudgetBytes()/planBands() are the
//  memory-limit arithmetic, pinned against hand-derived byte counts.
//
// ===========================================================================

namespace {

// The MonitorT contract, over std primitives (see BandLedger's header: the
// node's instantiation uses DD::Image::SignalLock, which has this exact
// surface).
struct StdMonitor {
    std::mutex              m;
    std::condition_variable cv;

    void lock()   { m.lock(); }
    void unlock() { m.unlock(); }

    bool wait(unsigned long timeoutMs = 0)
    {
        std::unique_lock<std::mutex> ul(m, std::adopt_lock);
        if (timeoutMs == 0)
            cv.wait(ul);
        else
            cv.wait_for(ul, std::chrono::milliseconds(timeoutMs));
        ul.release();
        return true;
    }

    void signal() { cv.notify_all(); }
};

using TestLedger = BandLedger<StdMonitor>;

bool neverAborted() { return false; }

} // namespace


TEST_CASE("bandBudgetBytes: stream state + virtual-background window + holdout LUT + "
          "resident SoA, against hand-derived byte counts")
{
    // The 4K default band: C=4, 4096x64, padY defaulted to 0.
    // State: W*B*(C+6)*4 = 4096*64*10*4 = 10,485,760.
    // Residual window: 2*W*B*4 = 2,097,152.
    // Total = 12,582,912, and no term of it depends on depth_layers.
    CHECK(bandBudgetBytes(16, 4, 4096, 64, false, 0.0) == doctest::Approx(12582912.0));
    CHECK(bandBudgetBytes(128, 4, 4096, 64, false, 0.0) == doctest::Approx(12582912.0));

    // The holdout LUT, (K+1)*W*B*4 = 17*4096*64*4 = 17,825,792 at K=16; it
    // gates on K > 0.
    CHECK(bandBudgetBytes(16, 4, 4096, 64, true, 0.0)
          - bandBudgetBytes(16, 4, 4096, 64, false, 0.0)
          == doctest::Approx(17825792.0));
    CHECK(bandBudgetBytes(0, 4, 4096, 64, true, 0.0) == doctest::Approx(12582912.0));

    // The SoA term, at the resident figure per fragment.
    CHECK(kSoAResidentBytesPerFragment == doctest::Approx(70.0));
    CHECK(bandBudgetBytes(16, 4, 4096, 64, false, 1.0e6)
          == doctest::Approx(12582912.0 + 7.0e7));

    // Negative width sanitises every plane term to 0; only the SoA survives.
    CHECK(bandBudgetBytes(16, 4, -1, 64, true, 100.0)
          == doctest::Approx(100.0 * kSoAResidentBytesPerFragment));
}

TEST_CASE("bandBudgetBytes: the virtual-background window scales with padY, "
          "not with K, against an independently hand-derived byte count")
{
    // max_radius=100 / edge_softness=1 defaults give padY=101 (see
    // DeepCDefocus.cpp's frameSetup(): ceil((100 + 0.5) * 1.0)).  At the 4K
    // default band (W=4096, B=64) the window height is B + 2*padY = 266, so
    // the term is 2*W*266*4 = 8,716,288 B (~8.72 MB) -- nearly 4x the
    // arrival plane's 1,048,576 B at the same geometry, and unaffected by K.
    const double planes16 = bandBudgetBytes(16, 4, 4096, 64, false, 0.0, 0);   // padY=0
    const double withPad16 = bandBudgetBytes(16, 4, 4096, 64, false, 0.0, 101);
    const double withPad128 = bandBudgetBytes(128, 4, 4096, 64, false, 0.0, 101);

    CHECK(withPad16 - planes16 == doctest::Approx(8716288.0 - 2097152.0));
    CHECK(withPad16 - bandBudgetBytes(16, 4, 4096, 64, false, 0.0)
          == doctest::Approx(8716288.0 - 2097152.0));

    // K-INDEPENDENT: the padY=101 window term is identical at K=16 and
    // K=128.
    CHECK(withPad128 - withPad16
          == doctest::Approx(bandBudgetBytes(128, 4, 4096, 64, false, 0.0)
                            - bandBudgetBytes(16, 4, 4096, 64, false, 0.0)));

    // ResidualWindow::bytesForWindow() directly, at the window's own size
    // (not the band's) -- the independent hand derivation for the 8.72 MB
    // figure quoted in bandBudgetBytes()'s doc block.
    CHECK(ResidualWindow::bytesForWindow(4096, 64 + 2 * 101) == 8716288u);
}

TEST_CASE("worstFetchWindowSum: the worst band's fetch-window sum of per-row counts, and "
          "background mode's +1 per non-empty pixel raises it by the window's pixel count")
{
    // 12 source rows at y = 4..15, one sample per row except row 9 (five);
    // output box y = 0..20; band height 4, padY 1.
    std::vector<double> rows(12, 1.0);
    rows[5] = 5.0;                                          // y = 9
    const int srcY0 = 4;
    CHECK(worstFetchWindowSum(rows, srcY0, 0, 20, 4, 1) == 10.0);    // band 8..12 -> fetch 7..13
    CHECK(worstFetchWindowSum(rows, srcY0, 0, 20, 4, 0) == 8.0);     // band 8..12 alone
    CHECK(worstFetchWindowSum(rows, srcY0, 0, 20, 20, 0) == 16.0);   // one band, every row
    CHECK(worstFetchWindowSum(rows, srcY0, 0, 20, 1, 0) == 5.0);
    CHECK(worstFetchWindowSum(rows, srcY0, 0, 4, 4, 0) == 0.0);      // no band reaches a source row
    CHECK(worstFetchWindowSum({}, srcY0, 0, 20, 4, 1) == 0.0);

    // Background mode: the node adds the per-row non-empty pixel count
    // before handing the rows over, so the bound grows by exactly the
    // fetch window's pixel count.
    std::vector<double> pixels(12, 3.0);
    std::vector<double> withFill = rows;
    for (std::size_t i = 0; i < rows.size(); ++i)
        withFill[i] += pixels[i];
    CHECK(worstFetchWindowSum(withFill, srcY0, 0, 20, 4, 1)
          == worstFetchWindowSum(rows, srcY0, 0, 20, 4, 1) + 6.0 * 3.0);
    const BandPlan fg = planBands(4096.0 * 300.0, 20, 16, 4, 64, false, 4,
                                  [&](int b) { return worstFetchWindowSum(rows, srcY0, 0, 20, b, 1); }, 1);
    const BandPlan bg = planBands(4096.0 * 300.0, 20, 16, 4, 64, false, 4,
                                  [&](int b) { return worstFetchWindowSum(withFill, srcY0, 0, 20, b, 1); }, 1);
    CHECK(bg.bandHeight <= fg.bandHeight);
    CHECK(bg.maxInFlight <= fg.maxInFlight);
}

TEST_CASE("planBands: shrink-to-fit floors at 1 row and the concurrent cap "
          "floors at 1 band — never 0, never a deadlock")
{
    const auto noFragments = [](int) { return 0.0; };

    // Per band row at C=4, W=4096, padY 0: the stream state 4096*10*4 =
    // 163,840 plus the residual window 2*4096*4 = 32,768, i.e. 196,608 B;
    // no term depends on K.

    // Fits outright: 4GB limit, B=256 -> 50,331,648 B; 85 bands' worth, so
    // the cap is the band count, ceil(2160/256) = 9.
    {
        const BandPlan p = planBands(4.0 * 1024.0 * 1024.0 * 1024.0,
                                     2160, 16, 4, 4096, false, 256, noFragments);
        CHECK(p.bandHeight == 256);
        CHECK(p.bandCount == 9);
        CHECK(p.maxInFlight == 9);
    }

    // Shrinks: 8MB limit.  256 -> 128 -> 64 (12,582,912) -> 32 (6,291,456,
    // fits 8,388,608).  One band in flight (8,388,608 / 6,291,456 < 2).
    {
        const BandPlan p = planBands(8.0 * 1024.0 * 1024.0,
                                     2160, 16, 4, 4096, false, 256, noFragments);
        CHECK(p.bandHeight == 32);
        CHECK(p.bandCount == (2160 + 31) / 32);
        CHECK(p.maxInFlight == 1);
    }

    // Even ONE row over the limit: bandHeight floors at 1 and the cap at 1.
    // bytes(1) = 196,608 > 128KB.
    {
        const BandPlan p = planBands(128.0 * 1024.0,
                                     2160, 16, 4, 4096, false, 256, noFragments);
        CHECK(p.bandHeight == 1);
        CHECK(p.bandCount == 2160);
        CHECK(p.maxInFlight == 1);
    }

    // The fragment estimator participates in the shrink: 20 spp over a 4096
    // window at 70 B resident makes a row cost 196,608 + 5,734,400 =
    // 5,931,008 B, so under 512MB (536,870,912) 128 rows (759,169,024) do
    // not fit and 64 (379,584,512) do.
    {
        const auto sppFragments = [](int b) {
            return 4096.0 * static_cast<double>(b) * 20.0;
        };
        const double limit = 512.0 * 1024.0 * 1024.0;
        const BandPlan withFrag = planBands(limit, 2160, 16, 4, 4096, false, 256, sppFragments);
        const BandPlan without  = planBands(limit, 2160, 16, 4, 4096, false, 256, noFragments);
        CHECK(withFrag.bandHeight < without.bandHeight);
        CHECK(withFrag.bandHeight == 64);
        CHECK(withFrag.maxInFlight == 1);
        CHECK(without.bandHeight == 256);
    }

    // padY is a real shrink input: at 9,000,000 B, padY=0 fits at 32 rows
    // (6,291,456), while padY=101 sizes the window at B + 202 rows and needs
    // 8 (8,192,000; 16 rows would be 9,764,864).
    {
        const double limit = 9000000.0;
        const BandPlan noPad = planBands(limit, 2160, 16, 4, 4096, false, 256,
                                         noFragments, /*padY*/ 0);
        const BandPlan pad101 = planBands(limit, 2160, 16, 4, 4096, false, 256,
                                          noFragments, /*padY*/ 101);
        CHECK(noPad.bandHeight == 32);
        CHECK(pad101.bandHeight == 8);
    }

    // The cap never exceeds the band count (extra slots could never be
    // claimed), and a degenerate frame is 0 bands with the floor cap.
    {
        const BandPlan tiny = planBands(64.0 * 1024.0 * 1024.0 * 1024.0,
                                        40, 16, 4, 64, false, 256, noFragments);
        CHECK(tiny.bandHeight == 40);   // clamped to the frame height
        CHECK(tiny.bandCount == 1);
        CHECK(tiny.maxInFlight == 1);

        const BandPlan empty = planBands(1.0e9, 0, 16, 4, 64, false, 256,
                                         noFragments);
        CHECK(empty.bandCount == 0);
        CHECK(empty.maxInFlight == 1);
    }
}

// ===========================================================================
// scatterBackgroundCPU — the virtual background
// ===========================================================================

TEST_CASE("scatterBackgroundCPU: background deposits sum to T per source pixel, "
          "within 1e-6 -- both the disc kernel and the sharp fast path")
{
    DiscKernelLUT lut(0.0f, 20.0f, 1.0f, 1.0f);
    const int W = 40, H = 40;
    const ScatterParams sp = makeScatterParams(W, H);

    SUBCASE("disc kernel, several radii and claims, unclipped (disc wholly inside the band)")
    {
        for (float r : {1.0f, 2.5f, 6.0f, 12.5f}) {
            for (float T : {1.0f, 0.6f, 0.1234f}) {
                ResidualWindow window;
                window.allocate(0, 0, W, H, r);
                for (int y = 0; y < H; ++y)
                    for (int x = 0; x < W; ++x)
                        window.setPixel(x, y, 0.0f, r);   // isolate: only (20,20) claims
                window.setPixel(20, 20, T, r);

                std::vector<float> arrival(static_cast<std::size_t>(W * H), 0.0f);
                scatterBackgroundCPU(sp, window, lut, arrival.data());

                const double sum =
                    vecSum(arrival);
                CHECK(sum == doctest::Approx(static_cast<double>(T)).epsilon(1e-6));
            }
        }
    }

    SUBCASE("sharp fast path (radius below kSharpRadiusPx): a single-pixel deposit of T, "
            "even when the LUT was never built down to that radius")
    {
        // minRadius = 2.0, deliberately ABOVE kSharpRadiusPx: this is what a
        // real frame's LUT looks like (built over the MEASURED radius range,
        // which rarely reaches literal 0). A residual radius of 0.1 must take
        // the sharp path and never touch this LUT at all -- routing it
        // through kernel.kernel(0.1, ...) instead would clamp to the LUT's
        // smallest built entry (radius 2.0) and spread the deposit over a
        // multi-pixel disc instead of the fragment's own single pixel.
        DiscKernelLUT sharpLut(2.0f, 20.0f, 1.0f, 1.0f);

        ResidualWindow window;
        window.allocate(0, 0, W, H, 0.1f);   // < kSharpRadiusPx -> sharp path
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                window.setPixel(x, y, 0.0f, 0.1f);
        window.setPixel(15, 15, 0.42f, 0.1f);

        std::vector<float> arrival(static_cast<std::size_t>(W * H), 0.0f);
        scatterBackgroundCPU(sp, window, sharpLut, arrival.data());

        const double sum = vecSum(arrival);
        CHECK(sum == doctest::Approx(0.42).epsilon(1e-6));
        CHECK(arrival[static_cast<std::size_t>(15) * W + 15]
              == doctest::Approx(0.42f));
    }

    SUBCASE("the skip floor is the composite's deficit tolerance, not a looser one")
    {
        // Anything dropped here is missing from the divisor the fill measures
        // against 1, so a residual the fill would still divide for must be
        // deposited.  At the tolerance it is dropped (the fill reads that
        // pixel as full anyway); a hair above it, it is not.
        auto sumAt = [&](float T) {
            ResidualWindow window;
            window.allocate(0, 0, W, H, 5.0f);
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x)
                    window.setPixel(x, y, 0.0f, 5.0f);
            window.setPixel(20, 20, T, 5.0f);

            std::vector<float> arrival(static_cast<std::size_t>(W * H), 0.0f);
            scatterBackgroundCPU(sp, window, lut, arrival.data());
            return vecSum(arrival);
        };

        CHECK(sumAt(kFillDeficitTol) == 0.0);
        CHECK(sumAt(2.0f * kFillDeficitTol)
              == doctest::Approx(2.0 * kFillDeficitTol).epsilon(1e-5));
    }
}

TEST_CASE("scatterBackgroundCPU: a pixel WITH samples uses its own residual radius; a "
          "pixel with NO samples uses the global background radius -- the kernel INDEX "
          "chosen, not merely the deposit sum")
{
    DiscKernelLUT lut(0.0f, 30.0f, 1.0f, 1.0f);
    const int W = 80, H = 80;
    const ScatterParams sp = makeScatterParams(W, H);

    const float rWithSamples = 3.0f;    // the pixel's own deepest-sample radius
    const float rGlobal      = 18.2f;   // background_depth's global radius -- far from it

    const int ax = 20, ay = 20;   // "has samples": scatters at rWithSamples
    const int bx = 55, by = 55;   // "no samples": scatters at rGlobal, far enough not to overlap A

    ResidualWindow window;
    window.allocate(0, 0, W, H, rGlobal);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            window.setPixel(x, y, 0.0f, rGlobal);
    window.setPixel(ax, ay, 0.5f, rWithSamples);
    window.setPixel(bx, by, 0.5f, rGlobal);

    std::vector<float> arrival(static_cast<std::size_t>(W * H), 0.0f);
    scatterBackgroundCPU(sp, window, lut, arrival.data());

    // The independently-derived expected footprint for EACH radius: the
    // blended kernel from refBracket()/refBlendedWeight(), which walks the
    // grid rather than calling kernelGridBracket().  Neither 3.0 nor 18.0 is
    // a grid node (the fine region's nodes are 512/n; the coarse region's
    // are 16 + m/2), so both are genuine two-kernel blends and a background
    // scatter that snapped to the nearest node would miss on every tap.
    REQUIRE(refBracket(rWithSamples).passes == 2);
    REQUIRE(refBracket(rGlobal).passes == 2);
    const KernelView kvA = lut.kernel(rWithSamples, ax, ay, 0.0f, 0);
    const KernelView kvB = lut.kernel(rGlobal,      bx, by, 0.0f, 0);
    REQUIRE(kvA.valid());
    REQUIRE(kvB.valid());
    REQUIRE(kvA.radiusX != kvB.radiusX);   // the two footprints are visibly different sizes

    // A's actual deposit matches the blend at rWithSamples pixel for pixel,
    // scaled by its own T=0.5 -- NOT the blend at rGlobal.  This is the
    // assertion a dropped per-pixel radius (always using the global one)
    // would break: A's footprint would come out kvB-shaped instead.  The
    // footprint walked is one pixel wider than the larger bracketing node in
    // every direction, so a deposit landing OUTSIDE the blend's support is
    // caught as well.
    auto checkFootprint = [&](int cx, int cy, float radius, int reach) -> int {
        int checked = 0;
        for (int dy = -reach; dy <= reach; ++dy) {
            for (int dx = -reach; dx <= reach; ++dx) {
                const double expected = refBlendedWeight(lut, radius, dx, dy) * 0.5;
                const double actual = arrival[
                    static_cast<std::size_t>(cy + dy) * W + static_cast<std::size_t>(cx + dx)];
                CHECK(std::fabs(actual - expected) <= 1e-6 * std::max(1.0, expected));
                ++checked;
            }
        }
        return checked;
    };
    const KernelView kvAhi = lut.kernel(kernelGridRadius(refBracket(rWithSamples).node[1]),
                                        0, 0, 0.0f, 0);
    const KernelView kvBhi = lut.kernel(kernelGridRadius(refBracket(rGlobal).node[1]),
                                        0, 0, 0.0f, 0);
    CHECK(checkFootprint(ax, ay, rWithSamples, kvAhi.radiusX + 1) > 0);
    CHECK(checkFootprint(bx, by, rGlobal,      kvBhi.radiusX + 1) > 0);
}

TEST_CASE("scatterBackgroundCPU: the per-pixel residual radius is what lets the "
          "composite division recover the true surface alpha")
{
    // A single alpha=0.9 point sample and its own residual (T = 1 - 0.9 = 0.1)
    // at the SAME pixel.  This function does not itself divide anything --
    // resolveStreamCPU() does -- but the arithmetic it must support is
    // alpha / arrival, and this pins exactly that at the fragment's own
    // centre pixel:
    //
    //   arrival_centre = share * wC(rSample) + residualT * wC(residualRadius)
    //   alpha_centre   = share * wC(rSample)
    //
    // When residualRadius == rSample, wC cancels and alpha/arrival is EXACTLY
    // trueAlpha, independent of wC's actual value -- see the derivation.  When
    // it does not, wC(rSample) != wC(residualRadius) and the ratio drifts off
    // trueAlpha by an amount set by how far the two kernels' centre weights
    // differ.  A background scatter that dropped the per-pixel radius (always
    // using the global one) would make the "correct" case behave exactly like
    // the "mismatched" one below, breaking the first CHECK.
    DiscKernelLUT lut(0.0f, 30.0f, 1.0f, 1.0f);
    const int W = 60, H = 60;
    const int cx = 30, cy = 30;
    const ScatterParams sp = makeScatterParams(W, H);
    const float trueAlpha   = 0.9f;
    const float rSample     = 6.0f;    // the pixel's own deepest-sample radius
    const float rMismatched = 5.8f;    // a global radius that does NOT match it -- close,
                                        // not wildly off, which is the realistic case

    // The two radii blend to genuinely different kernels -- not necessarily a
    // different pixel footprint (radiusX can coincide at a 0.2px spacing),
    // but a different centre weight, which is the quantity wC that actually
    // drives the mismatch below.  Both centre weights come from the test-side
    // blend oracle, not from the scatter under test.
    const double wcSample     = refBlendedWeight(lut, rSample, 0, 0);
    const double wcMismatched = refBlendedWeight(lut, rMismatched, 0, 0);
    REQUIRE(wcSample != wcMismatched);

    auto recover = [&](float residualRadius) -> float {
        SampleSoA soa;
        soa.begin(1, makeSingleChannelGroup(1));
        appendFragmentAt(soa, cx, cy, rSample, 5.0f, trueAlpha, trueAlpha, {0.0f});

        ResidualWindow window;
        window.allocate(0, 0, W, H, residualRadius);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                window.setPixel(x, y, 0.0f, residualRadius);
        window.setPixel(cx, cy, 1.0f - trueAlpha, residualRadius);

        Band band;
        band.C = 1; band.W = W; band.H = H;
        HoldoutSoA noHoldout;
        runBand(band, sp, soa, noHoldout, lut, false, &window);

        const std::size_t centre = band.at(cx, cy);
        REQUIRE(band.planes.arrival[centre] > 0.0f);
        return band.planes.alpha[centre] / band.planes.arrival[centre];
    };

    const float correct    = recover(rSample);
    const float mismatched = recover(rMismatched);

    CHECK(correct == doctest::Approx(trueAlpha).epsilon(1e-6));
    // A 0.2px radius mismatch (6.0 vs 5.8) reads ~0.8935 here -- the same
    // ~1.8-code-value scale (1.8/255 =~ 0.007) the design doc quotes for this
    // class of mismatch on a full scene.  The expected value is the centre-
    // pixel derivation above evaluated on the oracle's own centre weights,
    // and it is banded on top so the mismatch cannot quietly shrink towards
    // 0.9 or grow past the scale the design quotes.
    const double wantMismatched =
        trueAlpha * wcSample / (trueAlpha * wcSample + (1.0 - trueAlpha) * wcMismatched);
    CHECK(mismatched == doctest::Approx(wantMismatched).epsilon(1e-5));
    CHECK(mismatched > 0.890f);
    CHECK(mismatched < 0.896f);
    CHECK(std::abs(mismatched - trueAlpha) > 0.005f);   // unambiguously NOT 0.9
}

TEST_CASE("BandLedger: the Dirty -> InProgress -> Done protocol, single thread")
{
    TestLedger ledger;

    // Nothing exists yet: reads fail, band claims are Stale (caller must go
    // set up the frame).
    CHECK(!ledger.beginRead(1));
    CHECK(ledger.acquireBand(1, 0, neverAborted) == BandClaim::Stale);

    // First arrival claims setup; a repeat claim for the same key is Ready.
    CHECK(ledger.beginFrame(1, neverAborted) == FrameClaim::SetupCompute);
    ledger.endFrameSetup(true, 1, 8, 2);
    CHECK(ledger.beginFrame(1, neverAborted) == FrameClaim::Ready);
    CHECK(ledger.bandCount() == 8);

    // Reads now succeed, but no band is Done yet.
    CHECK(ledger.beginRead(1));
    CHECK(!ledger.bandDone(0));
    ledger.endRead();
    CHECK(!ledger.beginRead(2));   // wrong key

    // Claim -> InProgress -> complete -> Done -> later claims are Ready.
    CHECK(ledger.acquireBand(1, 0, neverAborted) == BandClaim::Compute);
    CHECK(ledger.bandState(0) == BandState::InProgress);
    CHECK(ledger.inFlight() == 1);
    ledger.completeBand(0);
    CHECK(ledger.bandState(0) == BandState::Done);
    CHECK(ledger.inFlight() == 0);
    CHECK(ledger.acquireBand(1, 0, neverAborted) == BandClaim::Ready);
    CHECK(ledger.beginRead(1));
    CHECK(ledger.bandDone(0));
    CHECK(!ledger.bandDone(1));
    ledger.endRead();

    // Abandon: back to Dirty — NEVER Done — and reclaimable.
    CHECK(ledger.acquireBand(1, 1, neverAborted) == BandClaim::Compute);
    ledger.abandonBand(1);
    CHECK(ledger.bandState(1) == BandState::Dirty);
    CHECK(ledger.inFlight() == 0);
    CHECK(ledger.acquireBand(1, 1, neverAborted) == BandClaim::Compute);
    ledger.completeBand(1);
    CHECK(ledger.bandState(1) == BandState::Done);

    // A Done band is served even under abort (the data is already valid);
    // a Dirty band under abort is Aborted, not claimed.
    const auto alwaysAborted = []() { return true; };
    CHECK(ledger.acquireBand(1, 1, alwaysAborted) == BandClaim::Ready);
    CHECK(ledger.acquireBand(1, 2, alwaysAborted) == BandClaim::Aborted);
    CHECK(ledger.bandState(2) == BandState::Dirty);
    CHECK(ledger.beginFrame(2, alwaysAborted) == FrameClaim::Aborted);

    // Out-of-range band: a black row, never a hang.
    CHECK(ledger.acquireBand(1, 8, neverAborted) == BandClaim::Aborted);
    CHECK(ledger.acquireBand(1, -1, neverAborted) == BandClaim::Aborted);

    // invalidate() = _validate's hash-change half: every non-in-flight band
    // goes Dirty, reads and claims for the old key fail, setup re-runs.
    ledger.invalidate();
    CHECK(!ledger.beginRead(1));
    CHECK(ledger.acquireBand(1, 0, neverAborted) == BandClaim::Stale);
    CHECK(ledger.bandState(0) == BandState::Dirty);
    CHECK(ledger.bandState(1) == BandState::Dirty);
    CHECK(ledger.beginFrame(2, neverAborted) == FrameClaim::SetupCompute);
    ledger.endFrameSetup(true, 2, 4, 1);
    CHECK(ledger.beginRead(2));
    CHECK(!ledger.bandDone(0));
    ledger.endRead();

    // A failed setup publishes nothing; the next caller re-claims.
    ledger.invalidate();
    CHECK(ledger.beginFrame(3, neverAborted) == FrameClaim::SetupCompute);
    ledger.endFrameSetup(false, 3, 0, 1);
    CHECK(!ledger.beginRead(3));
    CHECK(ledger.beginFrame(3, neverAborted) == FrameClaim::SetupCompute);
    ledger.endFrameSetup(true, 3, 2, 1);

    // The empty-frame path: every band is Done at publish, no claim cycle.
    ledger.invalidate();
    CHECK(ledger.beginFrame(4, neverAborted) == FrameClaim::SetupCompute);
    ledger.endFrameSetup(true, 4, 3, 1, /*allBandsDone=*/true);
    CHECK(ledger.beginRead(4));
    CHECK(ledger.bandDone(0));
    CHECK(ledger.bandDone(2));
    ledger.endRead();
}

TEST_CASE("BandLedger: 8 threads x 32 bands x 50 generations — every band "
          "computed EXACTLY once per generation, in-flight never exceeds the "
          "cap, waiters always wake")
{
    constexpr int kThreads     = 8;
    constexpr int kBands       = 32;
    constexpr int kGenerations = 50;
    constexpr int kCap         = 3;

    TestLedger ledger;

    for (int gen = 0; gen < kGenerations; ++gen) {
        const std::uint64_t key = 100 + static_cast<std::uint64_t>(gen);

        // Main is quiesced between generations, so the setup claim is
        // deterministic.
        ledger.invalidate();
        REQUIRE(ledger.beginFrame(key, neverAborted) == FrameClaim::SetupCompute);
        ledger.endFrameSetup(true, key, kBands, kCap);

        std::atomic<int> computes[kBands] = {};
        std::atomic<int> concurrent{0};
        std::atomic<int> maxConcurrent{0};
        std::atomic<int> readyServes{0};

        std::vector<std::thread> workers;
        workers.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            workers.emplace_back([&, t] {
                // Each thread sweeps every band, starting at its own offset,
                // exactly like render threads asking for rows in different
                // bands.
                for (int i = 0; i < kBands; ++i) {
                    const int band = (t * 5 + i) % kBands;
                    const BandClaim claim =
                        ledger.acquireBand(key, band, neverAborted);
                    if (claim == BandClaim::Compute) {
                        const int now = concurrent.fetch_add(1) + 1;
                        int prev = maxConcurrent.load();
                        while (prev < now
                               && !maxConcurrent.compare_exchange_weak(prev, now)) {
                        }
                        computes[band].fetch_add(1);
                        // A little real work, so claims overlap in time.
                        volatile double sink = 0.0;
                        for (int w = 0; w < 2000; ++w)
                            sink = sink + w * 1e-9;
                        concurrent.fetch_sub(1);
                        ledger.completeBand(band);
                    }
                    else {
                        // The ONLY other legal outcome here is Ready — the
                        // key never changes and nothing aborts, so a waiter
                        // blocked on an InProgress band must wake into Done.
                        // (CHECK, not REQUIRE: doctest's REQUIRE throws, which
                        // must not leave a spawned thread.)
                        CHECK(claim == BandClaim::Ready);
                        if (claim == BandClaim::Ready) {
                            readyServes.fetch_add(1);
                            const bool readable = ledger.beginRead(key);
                            CHECK(readable);
                            if (readable) {
                                CHECK(ledger.bandDone(band));
                                ledger.endRead();
                            }
                        }
                    }
                }
            });
        }
        for (std::thread& w : workers)
            w.join();

        for (int b = 0; b < kBands; ++b)
            REQUIRE(computes[b].load() == 1);        // exactly one owner, ever
        REQUIRE(maxConcurrent.load() <= kCap);       // the memory cap held
        REQUIRE(ledger.inFlight() == 0);
        REQUIRE(readyServes.load() == kThreads * kBands - kBands);
        for (int b = 0; b < kBands; ++b)
            REQUIRE(ledger.bandState(b) == BandState::Done);
    }
}

TEST_CASE("BandLedger: tryClaimDirtyBand hands a waiter a DIFFERENT band, "
          "respects the in-flight cap, and never claims twice")
{
    TestLedger ledger;
    REQUIRE(ledger.beginFrame(11, neverAborted) == FrameClaim::SetupCompute);
    ledger.endFrameSetup(true, 11, 6, 3);

    // The search starts at the caller's own band, so an uncontended caller
    // gets exactly the band it asked for.
    CHECK(ledger.tryClaimDirtyBand(11, 2, 0, 5) == 2);
    CHECK(ledger.bandState(2) == BandState::InProgress);
    CHECK(ledger.inFlight() == 1);

    // A second caller asking for the SAME band gets the next Dirty one
    // instead of blocking — this is the whole point of the call.
    CHECK(ledger.tryClaimDirtyBand(11, 2, 0, 5) == 3);
    CHECK(ledger.tryClaimDirtyBand(11, 2, 0, 5) == 4);
    CHECK(ledger.inFlight() == 3);

    // The in-flight cap is 3: nothing more may be claimed, even though bands
    // 0, 1 and 5 are still Dirty.
    CHECK(ledger.tryClaimDirtyBand(11, 0, 0, 5) == -1);
    CHECK(ledger.bandState(0) == BandState::Dirty);

    // Completing one frees exactly one slot, and the search wraps.
    ledger.completeBand(3);
    CHECK(ledger.tryClaimDirtyBand(11, 5, 0, 5) == 5);
    ledger.completeBand(5);
    CHECK(ledger.tryClaimDirtyBand(11, 5, 0, 5) == 0);   // wrapped past the end

    // A Done band is never re-claimed, and an abandoned one is.
    ledger.completeBand(0);
    ledger.completeBand(2);
    ledger.abandonBand(4);
    CHECK(ledger.tryClaimDirtyBand(11, 0, 0, 5) == 1);
    CHECK(ledger.tryClaimDirtyBand(11, 0, 0, 5) == 4);
    ledger.completeBand(1);
    ledger.completeBand(4);
    CHECK(ledger.tryClaimDirtyBand(11, 0, 0, 5) == -1);   // every band Done

    // Wrong key / no setup: -1, never a claim.
    CHECK(ledger.tryClaimDirtyBand(12, 0, 0, 5) == -1);
    ledger.invalidate();
    CHECK(ledger.tryClaimDirtyBand(11, 0, 0, 5) == -1);

    // A start OUTSIDE the window claims nothing at all.  It is the caller's
    // precondition to pass its own band, and the alternative -- clamping into
    // the window -- would hand the caller a band its row request cannot use,
    // costing it a whole band's compute before it reaches the one it needs.
    REQUIRE(ledger.beginFrame(13, neverAborted) == FrameClaim::SetupCompute);
    ledger.endFrameSetup(true, 13, 2, 2);
    CHECK(ledger.tryClaimDirtyBand(13, 99, 0, 1) == -1);
    CHECK(ledger.tryClaimDirtyBand(13, -7, 0, 1) == -1);
    CHECK(ledger.bandState(0) == BandState::Dirty);
    CHECK(ledger.bandState(1) == BandState::Dirty);
    CHECK(ledger.inFlight() == 0);
    CHECK(ledger.tryClaimDirtyBand(13, 0, 0, 1) == 0);
    CHECK(ledger.tryClaimDirtyBand(13, 1, 0, 1) == 1);
    // Both back, or the next beginFrame() would quiesce forever waiting on
    // claims this test never released.
    ledger.completeBand(0);
    ledger.completeBand(1);

    // THE WINDOW: bands outside [first, last] are never handed out, however
    // Dirty they are.  This is what keeps a waiter off the pad bands the node
    // decomposes but engine() is never called for -- computing those cost
    // 24 bands against 18 and 65% of the frame's wall clock when measured.
    REQUIRE(ledger.beginFrame(14, neverAborted) == FrameClaim::SetupCompute);
    ledger.endFrameSetup(true, 14, 10, 4);
    CHECK(ledger.tryClaimDirtyBand(14, 3, 3, 5) == 3);
    CHECK(ledger.tryClaimDirtyBand(14, 3, 3, 5) == 4);
    CHECK(ledger.tryClaimDirtyBand(14, 3, 3, 5) == 5);
    CHECK(ledger.tryClaimDirtyBand(14, 3, 3, 5) == -1);   // window exhausted
    CHECK(ledger.bandState(0) == BandState::Dirty);       // ...though 0 is free
    CHECK(ledger.bandState(9) == BandState::Dirty);
    // A window wider than the ledger clamps; an inverted one claims nothing.
    CHECK(ledger.tryClaimDirtyBand(14, 0, -5, 99) == 0);
    CHECK(ledger.tryClaimDirtyBand(14, 0, 6, 2) == -1);
}

TEST_CASE("BandLedger: tryClaimDirtyBand under 8 threads computes every band "
          "exactly once and never exceeds the cap")
{
    constexpr int kThreads = 8;
    constexpr int kBands   = 64;
    constexpr int kCap     = 3;

    TestLedger ledger;
    REQUIRE(ledger.beginFrame(21, neverAborted) == FrameClaim::SetupCompute);
    ledger.endFrameSetup(true, 21, kBands, kCap);

    std::vector<std::atomic<int>> computed(kBands);
    for (std::atomic<int>& c : computed)
        c.store(0);
    std::atomic<int> live{0};
    std::atomic<int> peak{0};
    std::atomic<int> claimed{0};

    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t] {
            for (;;) {
                const int band = ledger.tryClaimDirtyBand(21, t * 7, 0, kBands - 1);
                if (band < 0) {
                    if (claimed.load() >= kBands)
                        return;
                    std::this_thread::yield();
                    continue;
                }
                const int now = live.fetch_add(1) + 1;
                int seen = peak.load();
                while (now > seen && !peak.compare_exchange_weak(seen, now)) {}
                computed[static_cast<std::size_t>(band)].fetch_add(1);
                claimed.fetch_add(1);
                std::this_thread::yield();
                live.fetch_sub(1);
                ledger.completeBand(band);
            }
        });
    }
    for (std::thread& w : workers)
        w.join();

    for (int b = 0; b < kBands; ++b) {
        CHECK(computed[static_cast<std::size_t>(b)].load() == 1);
        CHECK(ledger.bandState(b) == BandState::Done);
    }
    CHECK(peak.load() <= kCap);
    CHECK(ledger.inFlight() == 0);
}

// StdMonitor's wait(ms) really does sleep, so it would HIDE a spin in the
// setup claim's reader drain rather than pin it.  DD::Image::SignalLock's
// does not: wait(ms) builds an ABSOLUTE timespec of {0, ms*1000}, always in
// the past, so pthread_cond_timedwait returns ETIMEDOUT at once (measured at
// 0.0106 ms per call against the 1 ms asked for).  This double reproduces
// that and counts the monitor traffic the drain generates.
namespace {

std::atomic<long> gDrainLocks{0};

struct SpinProneMonitor {
    std::mutex              m;
    std::condition_variable cv;

    void lock()   { m.lock(); gDrainLocks.fetch_add(1); }
    void unlock() { m.unlock(); }

    bool wait(unsigned long timeoutMs = 0)
    {
        if (timeoutMs != 0) {
            // ETIMEDOUT immediately -- but pthread_cond_timedwait still
            // releases and reacquires the mutex on its way out, and that
            // churn is what a spin here costs.
            unlock();
            lock();
            return false;
        }
        std::unique_lock<std::mutex> ul(m, std::adopt_lock);
        cv.wait(ul);
        ul.release();
        return true;
    }

    void signal() { cv.notify_all(); }
};

} // namespace

TEST_CASE("BandLedger: the setup claim's reader drain sleeps rather than "
          "spinning on a monitor whose timed wait does not wait")
{
    constexpr int kReadMs = 60;

    BandLedger<SpinProneMonitor> ledger;
    REQUIRE(ledger.beginFrame(31, neverAborted) == FrameClaim::SetupCompute);
    ledger.endFrameSetup(true, 31, 4, 2);

    // One reader holds the frame open; the setup claim below cannot return
    // until it lets go.
    REQUIRE(ledger.beginRead(31));
    std::thread reader([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(kReadMs));
        ledger.endRead();
    });

    gDrainLocks.store(0);
    const auto started = std::chrono::steady_clock::now();
    REQUIRE(ledger.beginFrame(32, neverAborted) == FrameClaim::SetupCompute);
    const double drainMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    const long locks = gDrainLocks.load();
    reader.join();
    ledger.endFrameSetup(true, 32, 4, 2);

    // The drain really did wait for the reader (without this the lock count
    // below would be small for the wrong reason).
    CHECK(drainMs >= 0.5 * kReadMs);

    // A 1 ms poll over ~60 ms is ~60 monitor acquisitions.  A spin is
    // ~5,600 per 60 ms at the measured 0.0106 ms per immediate return, and
    // unbounded once the sleep is removed entirely.
    CHECK(locks < 1000);
}

TEST_CASE("BandLedger: an aborted band resets to Dirty, wakes its waiters "
          "into Aborted, and is recomputable after the abort clears")
{
    TestLedger ledger;
    REQUIRE(ledger.beginFrame(9, neverAborted) == FrameClaim::SetupCompute);
    ledger.endFrameSetup(true, 9, 4, 2);

    std::atomic<bool> abortFlag{false};
    const auto abortedFn = [&]() { return abortFlag.load(); };

    // The owner claims band 0 and holds it.
    REQUIRE(ledger.acquireBand(9, 0, abortedFn) == BandClaim::Compute);

    // Four waiters block on it (they can neither claim it nor read it).
    constexpr int kWaiters = 4;
    std::atomic<int> abortedSeen{0};
    std::vector<std::thread> waiters;
    for (int t = 0; t < kWaiters; ++t) {
        waiters.emplace_back([&] {
            const BandClaim claim = ledger.acquireBand(9, 0, abortedFn);
            // Deterministic: the abort flag is set BEFORE the abandon
            // broadcast, so a woken waiter can only see Dirty + aborted.
            if (claim == BandClaim::Aborted)
                abortedSeen.fetch_add(1);
        });
    }

    // Let the waiters reach the wait; exact timing does not matter — a
    // waiter that has not yet blocked takes the same Aborted branch on
    // entry.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // The cook is cancelled: flag first, then abandon (matching engine(),
    // where Op::aborted() is already true when computeBand returns false).
    abortFlag.store(true);
    ledger.abandonBand(0);

    for (std::thread& w : waiters)
        w.join();

    CHECK(abortedSeen.load() == kWaiters);            // every waiter woke
    CHECK(ledger.bandState(0) == BandState::Dirty);   // NEVER Done
    CHECK(ledger.inFlight() == 0);

    // Next cook (abort cleared): the band is claimable and completable.
    abortFlag.store(false);
    REQUIRE(ledger.acquireBand(9, 0, abortedFn) == BandClaim::Compute);
    ledger.completeBand(0);
    CHECK(ledger.bandState(0) == BandState::Done);
}

TEST_CASE("BandLedger: a setup re-claim drains active readers first, and the "
          "read gate is closed while setup is in progress")
{
    TestLedger ledger;
    REQUIRE(ledger.beginFrame(1, neverAborted) == FrameClaim::SetupCompute);
    ledger.endFrameSetup(true, 1, 2, 1);
    REQUIRE(ledger.acquireBand(1, 0, neverAborted) == BandClaim::Compute);
    ledger.completeBand(0);

    // Main holds a read (mid-row-copy); a hash change arrives on another
    // thread.  Its setup claim MUST NOT return until the read ends —
    // otherwise it would reallocate the frame under the copy.
    REQUIRE(ledger.beginRead(1));
    REQUIRE(ledger.bandDone(0));

    std::atomic<int> seq{0};
    int mainOrder = -1, workerOrder = -1;
    std::thread worker([&] {
        const FrameClaim claim = ledger.beginFrame(2, neverAborted);
        workerOrder = seq.fetch_add(1);
        // CHECK, not REQUIRE: doctest's REQUIRE throws, which must not leave
        // a spawned thread — and endFrameSetup below must run regardless, or
        // the main thread would hang.
        CHECK(claim == FrameClaim::SetupCompute);

        // While setup is in progress the read gate is closed for EVERY key.
        CHECK(!ledger.beginRead(1));
        CHECK(!ledger.beginRead(2));

        ledger.endFrameSetup(true, 2, 2, 1);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    mainOrder = seq.fetch_add(1);   // BEFORE endRead: any later increment is
    ledger.endRead();               // provably after the drain completed
    worker.join();

    CHECK(workerOrder > mainOrder);   // the claim outwaited the reader
    CHECK(ledger.beginRead(2));       // reopened under the new key
    CHECK(!ledger.bandDone(0));       // ...with every band reset to Dirty
    ledger.endRead();
    CHECK(!ledger.beginRead(1));      // the old key stays dead
}

// ===========================================================================
//
//  The bracketing-kernel blend and the 1px minimum diameter
//
//  Above the sharp floor the scatter rasterises the two grid nodes bracketing
//  a radius at (1-f) and f.  Every check below reads the kernel the SHIPPED
//  driver actually deposits -- one fragment with unit share and unit alpha on
//  empty state, so its `claimed` plane is the effective kernel and its
//  `arrival` plane must carry the same numbers -- and compares it against
//  test-side derivations: refBracket()'s grid walk, an exact disc built
//  straight from discEdgeWeight(), or the radii snapped to their nearest node
//  first, which through the same driver IS the pre-blend scatter bit for bit
//  (a node radius brackets to a single pass at weight 1).
//
// ===========================================================================

namespace {

struct KernelRig {
    int W, H, cx, cy;
    ScatterParams            sp;
    StreamPlanes             planes;
    PodBuffer<std::uint32_t> order;
    StreamSortScratch        sortScratch;
    ScatterScratch           scratch;
    HoldoutSoA               none;

    KernelRig(int w, int h) : W(w), H(h), cx(w / 2), cy(h / 2), sp(makeScatterParams(w, h))
    {
        planes.allocate(1, W, H);
    }

    // Rasterise one unit fragment at `radius`.  Alone on empty state every
    // weight lands on free area, so `claimed` holds the effective kernel.
    void rasterize(const DiscKernelLUT& lut, float radius)
    {
        SampleSoA soa;
        soa.begin(1, makeSingleChannelGroup(1));
        appendFragmentAt(soa, cx, cy, radius, 5.0f, 1.0f, 1.0f, {0.5f});
        planes.zero();
        sortFragmentsByDepth(soa, order, sortScratch);
        scatterStreamCPU(sp, soa, order, none, lut, planes, scratch);
    }

    std::size_t pixels() const { return static_cast<std::size_t>(W) * H; }
    float weightAt(int dx, int dy) const
    { return planes.claimed[static_cast<std::size_t>(cy + dy) * W + static_cast<std::size_t>(cx + dx)]; }

    bool arrivalIsWeightBitExact() const
    {
        for (std::size_t i = 0; i < pixels(); ++i)
            if (planes.arrival[i] != planes.claimed[i])
                return false;
        return true;
    }
};

float snappedToNearestNode(float radius)
{
    return kernelGridRadius(kernelGridIndex(radius));
}

// The exact anti-aliased disc at `radius`, edge softness 1, normalised in
// double -- what DiscKernelLUT builds at a node, derived here without it.
std::vector<double> exactDisc(float radius, int W, int H, int cx, int cy)
{
    std::vector<double> k(static_cast<std::size_t>(W) * H, 0.0);
    double sum = 0.0;
    const int reach = static_cast<int>(std::ceil(radius + 0.5f)) + 1;
    for (int dy = -reach; dy <= reach; ++dy)
        for (int dx = -reach; dx <= reach; ++dx) {
            const float d = std::sqrt(static_cast<float>(dx * dx + dy * dy));
            const double w = discEdgeWeight(d, radius, 1.0f);
            k[static_cast<std::size_t>(cy + dy) * W + static_cast<std::size_t>(cx + dx)] = w;
            sum += w;
        }
    for (double& v : k)
        v /= sum;
    return k;
}

} // namespace


TEST_CASE("kernelGridBracket: on a node a single pass, between nodes the floor pair "
          "and a diameter-linear fraction, degenerate inputs a single pass at node 0")
{
    // On every node in the fine region and the first coarse nodes: one pass.
    for (int i = 1; i <= kKernelFineLastIndex + 40; ++i) {
        const KernelGridBracket b = kernelGridBracket(kernelGridRadius(i));
        CHECK(b.indexA == i);
        CHECK(b.indexB == i);
        CHECK(b.frac == 0.0f);
    }

    // Between nodes: agrees with the grid walk on the pair, and the fraction
    // is the diameter's position in the bracket.
    std::uint32_t state = 0x9E3779B9u;
    auto next01 = [&]() {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) / 16777216.0f;
    };
    for (int t = 0; t < 4000; ++t) {
        const float r = 0.5f + next01() * 39.5f;
        const KernelGridBracket b  = kernelGridBracket(r);
        const RefBracket        rb = refBracket(r);
        CAPTURE(r);
        CHECK(b.indexA == rb.node[0]);
        CHECK(b.indexB == rb.node[1]);
        CHECK(std::fabs(static_cast<double>(b.frac) - rb.blend[1]) <= 1e-6);
        CHECK(kernelGridRadius(b.indexA) <= r);
        if (b.indexB != b.indexA) {
            CHECK(b.indexB == b.indexA + 1);
            CHECK(kernelGridRadius(b.indexB) > r);
            CHECK(b.frac > 0.0f);
            CHECK(b.frac < 1.0f);
            const double dA = 2.0 * kernelGridRadius(b.indexA);
            const double dB = 2.0 * kernelGridRadius(b.indexB);
            CHECK(std::fabs(b.frac - (2.0 * r - dA) / (dB - dA)) <= 1e-6);
        }
    }

    // Monotone in radius across a bracket.
    {
        const float rA = kernelGridRadius(500), rB = kernelGridRadius(501);
        float prev = -1.0f;
        for (int k = 1; k < 100; ++k) {
            const float r = rA + (rB - rA) * static_cast<float>(k) / 100.0f;
            const KernelGridBracket b = kernelGridBracket(r);
            CHECK(b.indexA == 500);
            CHECK(b.frac > prev);
            prev = b.frac;
        }
    }

    // Degenerate inputs never ask for a second pass.
    for (float r : {0.0f, -1.0f, std::numeric_limits<float>::quiet_NaN(),
                    std::numeric_limits<float>::infinity(), 2.0e6f}) {
        const KernelGridBracket b = kernelGridBracket(r);
        CAPTURE(r);
        CHECK(b.indexA == b.indexB);
        CHECK(b.frac == 0.0f);
    }
    CHECK(kernelGridBracket(0.0f).indexA == 0);
    CHECK(kernelGridBracket(std::numeric_limits<float>::quiet_NaN()).indexA == 0);
}

TEST_CASE("the blended kernel is continuous in diameter: jumps shrink with the sweep step, "
          "the nearest-node scatter's do not, and arrival carries the same blend bit for bit")
{
    // A measured-range LUT whose floor IS the 1px-diameter floor, so no
    // sub-0.5 radius can degenerate to a shared entry.
    DiscKernelLUT lut(0.5f, 24.0f, 1.0f, 1.0f);
    KernelRig rig(52, 52);

    struct Sweep { double maxTapJump = 0.0, maxL1Jump = 0.0, atRadius = 0.0; };

    auto sweep = [&](double dFrom, double dTo, double h, bool snap) -> Sweep {
        Sweep s;
        std::vector<float> prev;
        for (double d = dFrom; d <= dTo + 1e-12; d += h) {
            float r = static_cast<float>(d * 0.5);
            if (snap)
                r = snappedToNearestNode(r);
            rig.rasterize(lut, r);
            // Both kernels feed arrival at the same f: with unit share and
            // unit alpha the arrival plane IS the coverage plane, to the bit.
            REQUIRE(rig.arrivalIsWeightBitExact());
            if (!prev.empty()) {
                double tap = 0.0, l1 = 0.0;
                for (std::size_t i = 0; i < rig.pixels(); ++i) {
                    const double dv = std::fabs(static_cast<double>(rig.planes.claimed[i]) - prev[i]);
                    tap = std::max(tap, dv);
                    l1 += dv;
                }
                if (tap > s.maxTapJump) { s.maxTapJump = tap; s.atRadius = r; }
                s.maxL1Jump = std::max(s.maxL1Jump, l1);
            }
            prev.assign(rig.planes.claimed.data(), rig.planes.claimed.data() + rig.pixels());
        }
        return s;
    };

    // (1) d in [1, 16] at h = 2e-3 px of diameter and at h/4.  A Lipschitz
    // family's jumps scale with the step; a step function's do not.  The
    // per-tap figure is dominated by the C1 kink at d = 1 (the delta node
    // against the first 3x3 node, 0.0005 px apart) and is the same slope for
    // both schemes there, so the discriminator is the L1 jump, which for the
    // nearest-node scatter is the whole difference between adjacent nodes no
    // matter how small the step.
    const double h = 2.0e-3;
    const Sweep blend  = sweep(1.0, 16.0, h, false);
    const Sweep blendQ = sweep(1.0, 16.0, h / 4.0, false);
    const Sweep snapd  = sweep(1.0, 16.0, h, true);
    const Sweep snapdQ = sweep(1.0, 16.0, h / 4.0, true);
    CAPTURE(blend.maxTapJump);  CAPTURE(blend.maxL1Jump);  CAPTURE(blend.atRadius);
    CAPTURE(blendQ.maxTapJump); CAPTURE(blendQ.maxL1Jump); CAPTURE(blendQ.atRadius);
    CAPTURE(snapd.maxTapJump);  CAPTURE(snapd.maxL1Jump);  CAPTURE(snapd.atRadius);
    CAPTURE(snapdQ.maxTapJump); CAPTURE(snapdQ.maxL1Jump); CAPTURE(snapdQ.atRadius);

    // Blend: measured 3.984e-03 / 7.968e-03 (tap / L1) at h, exactly a
    // quarter of each at h/4, both at r = 0.501.
    CHECK(blend.maxTapJump > 3.0e-03);
    CHECK(blend.maxTapJump < 5.0e-03);
    CHECK(blend.maxL1Jump  < 1.0e-02);
    CHECK(blendQ.maxTapJump < blend.maxTapJump * 0.30);
    CHECK(blendQ.maxTapJump > blend.maxTapJump * 0.20);
    CHECK(blendQ.maxL1Jump  < blend.maxL1Jump  * 0.30);
    CHECK(blendQ.maxL1Jump  > blend.maxL1Jump  * 0.20);
    // Nearest node: the L1 jump is a node step and does not shrink.
    CHECK(snapd.maxL1Jump  > 5.0 * blend.maxL1Jump);
    CHECK(snapdQ.maxL1Jump > 0.9 * snapd.maxL1Jump);

    // (2) One float ulp across every node and every bracket midpoint in
    // [0.5, 4]: the crossings where a broken bracket (at a node) or the
    // nearest-node rule (at a midpoint) would jump.  This is the literal
    // "no jump above 1e-6 per tap" gate, placed where a jump could be.
    double ulpNodeJump = 0.0, ulpMidJump = 0.0, ulpMidJumpSnap = 0.0;
    int crossings = 0;
    for (int i = 1; kernelGridRadius(i) < 4.0f; ++i) {
        const float rn = kernelGridRadius(i);
        const float rm = 0.5f * (rn + kernelGridRadius(i + 1));
        auto crossing = [&](float r, bool snap) -> double {
            const float lo = std::nextafter(r, 0.0f), hi = std::nextafter(r, 100.0f);
            rig.rasterize(lut, snap ? snappedToNearestNode(lo) : lo);
            std::vector<float> a(rig.planes.claimed.data(), rig.planes.claimed.data() + rig.pixels());
            rig.rasterize(lut, snap ? snappedToNearestNode(hi) : hi);
            double tap = 0.0;
            for (std::size_t k = 0; k < rig.pixels(); ++k)
                tap = std::max(tap, std::fabs(static_cast<double>(rig.planes.claimed[k]) - a[k]));
            return tap;
        };
        ulpNodeJump    = std::max(ulpNodeJump, crossing(rn, false));
        ulpMidJump     = std::max(ulpMidJump, crossing(rm, false));
        ulpMidJumpSnap = std::max(ulpMidJumpSnap, crossing(rm, true));
        ++crossings;
    }
    CAPTURE(crossings); CAPTURE(ulpNodeJump); CAPTURE(ulpMidJump); CAPTURE(ulpMidJumpSnap);
    CHECK(crossings > 800);
    CHECK(ulpNodeJump < 1.0e-06);           // measured 4.768e-07
    CHECK(ulpMidJump  < 1.0e-06);           // measured 5.364e-07
    CHECK(ulpMidJumpSnap > 1.0e-03);        // measured 1.951e-03: the step the blend removes
}

TEST_CASE("the effective kernel at a node radius is the exact disc at that radius, and "
          "between nodes the blend's deviation from the exact disc is banded")
{
    DiscKernelLUT lut(0.5f, 24.0f, 1.0f, 1.0f);
    KernelRig rig(52, 52);

    // Every 17th fine node and every coarse node up to 20 px, and the
    // midpoint of the bracket above each.
    double worstNode = 0.0, worstNodeR = 0.0;
    double worstMid = 0.0, worstMidR = 0.0, worstMidL1 = 0.0, bestMid = 1.0;
    double worstMidSnap = 0.0;
    int nodes = 0;
    for (int i = 1; kernelGridRadius(i) <= 20.0f; i += (i < kKernelFineLastIndex ? 17 : 1)) {
        const float rn = kernelGridRadius(i);
        rig.rasterize(lut, rn);
        const std::vector<double> exact = exactDisc(rn, rig.W, rig.H, rig.cx, rig.cy);
        double dev = 0.0, sumEff = 0.0, sumExact = 0.0;
        for (std::size_t k = 0; k < rig.pixels(); ++k) {
            dev = std::max(dev, std::fabs(static_cast<double>(rig.planes.claimed[k]) - exact[k]));
            sumEff   += rig.planes.claimed[k];
            sumExact += exact[k];
        }
        CHECK(std::fabs(sumEff - 1.0) <= 1e-6);
        CHECK(std::fabs(sumExact - 1.0) <= 1e-12);
        if (dev > worstNode) { worstNode = dev; worstNodeR = rn; }
        ++nodes;

        const float rm = 0.5f * (rn + kernelGridRadius(i + 1));
        const std::vector<double> exactMid = exactDisc(rm, rig.W, rig.H, rig.cx, rig.cy);
        double peak = 0.0;
        for (double v : exactMid)
            peak = std::max(peak, v);

        rig.rasterize(lut, rm);
        double devMid = 0.0, l1 = 0.0;
        for (std::size_t k = 0; k < rig.pixels(); ++k) {
            const double dv = std::fabs(static_cast<double>(rig.planes.claimed[k]) - exactMid[k]);
            devMid = std::max(devMid, dv);
            l1 += dv;
        }
        if (devMid / peak > worstMid) { worstMid = devMid / peak; worstMidR = rm; }
        worstMidL1 = std::max(worstMidL1, l1);
        bestMid = std::min(bestMid, devMid / peak);

        rig.rasterize(lut, snappedToNearestNode(rm));
        double devSnap = 0.0;
        for (std::size_t k = 0; k < rig.pixels(); ++k)
            devSnap = std::max(devSnap, std::fabs(static_cast<double>(rig.planes.claimed[k]) - exactMid[k]));
        worstMidSnap = std::max(worstMidSnap, devSnap / peak);
    }
    CAPTURE(nodes); CAPTURE(worstNode); CAPTURE(worstNodeR);
    CAPTURE(worstMid); CAPTURE(worstMidR); CAPTURE(worstMidL1); CAPTURE(bestMid);
    CAPTURE(worstMidSnap);

    CHECK(nodes >= 59);
    // On a node: float rounding only (measured 2.889e-08 at r = 0.5356).
    CHECK(worstNode < 5.0e-08);
    // Between nodes the blend is a blend of two discs, not the disc: worst
    // per-tap 0.0923 of peak at r = 13.66 and 0.0064 L1 (measured), against
    // the nearest node's 0.1769 of peak on the same midpoints.  Banded on
    // both sides: the lower bound is what says this IS the linear blend and
    // not something exact.
    CHECK(worstMid > 0.05);
    CHECK(worstMid < 0.12);
    CHECK(worstMidL1 > 0.003);
    CHECK(worstMidL1 < 0.012);
    CHECK(bestMid < 1.0e-05);               // the finest brackets are as good as exact
    CHECK(worstMidSnap > 1.5 * worstMid);   // and the blend is the closer of the two
    CHECK(worstMidSnap < 0.30);
}


TEST_CASE("size-0 parity: every diameter at or below 1px rasterises bit-identically to the "
          "sharp delta, planes and output alike, through a LUT that never reaches down to it")
{
    // THE GATE (a) PIN.  Three runs of one mixed corpus through the real
    // pipeline: size 0 (every radius exactly 0 -- the canonical sharp path),
    // a sub-pixel size whose radii fill (0, 0.49], and that same SoA with every
    // radius forced to the largest sharp float below 0.5.  All three must
    // agree TO THE BIT on every state plane but lastCoc (which records the
    // signed radius itself) and on the resolved output.  The LUT
    // is built over a measured range that starts at 2 px, so any radius that
    // leaked past the floor would come back as a 2 px disc, not a rounding
    // difference.  pre_merge is off so the three SoAs differ in nothing but
    // the radius column (its tolerance would otherwise regroup them).
    const int C = 3, W = 40, H = 40, spp = 4;
    DiscKernelLUT lut(2.0f, 20.0f, 1.0f, 1.0f);

    Lcg rng(0x5122u);
    std::vector<std::vector<SampleRecord>> pixels(static_cast<std::size_t>(W) * H);
    for (auto& v : pixels) {
        float z = rng.range(1.05f, 30.0f);
        for (int s = 0; s < spp; ++s) {
            const float a = rng.range(0.02f, 1.0f);
            const float zb = (s % 2 == 0) ? z : z + rng.range(0.1f, 4.0f);   // points and spans
            v.push_back(makeSample(z, zb, a, {a * rng.unit(), a * rng.unit(), a * rng.unit()}));
            z = zb + rng.range(0.05f, 5.0f);
        }
    }

    auto rigFor = [](float sizePx, float maxRadiusPx) {
        return makeCocParams(CocMode::Manual, 50.0f, 2.8f, 36.0f, 10.0f,
                             unitScale(WorldUnits::Meters), 1920.0f, 1.0f,
                             1.0f, 1.0f, maxRadiusPx, sizePx);
    };

    struct Run {
        Band band;
        float maxRadius = 0.0f, minRadius = 1.0f;
        std::size_t fragments = 0;
    };
    auto run = [&](float sizePx, float maxRadiusPx, bool forceLargestSharp) -> Run {
        const CocParams p = rigFor(sizePx, maxRadiusPx);
        const FlattenParams fp = makeFlattenParams(p, C, /*preMerge*/ false);
        SampleSoA soa;
        soa.begin(C, fp.groups);
        FlattenScratch scratch;
        ResidualWindow window;
        window.allocate(0, 0, W, H, autoBackgroundRadiusPx(p, 100.0f));
        flattenIntoWithResidual(fp, 0, 0, W, H, soa, scratch, window,
            [&](int x, int y) { return pixels[static_cast<std::size_t>(y) * W + x]; });

        Run r;
        r.fragments = soa.fragmentCount();
        for (std::size_t f = 0; f < soa.fragmentCount(); ++f) {
            r.maxRadius = std::max(r.maxRadius, soa.radius[f]);
            r.minRadius = std::min(r.minRadius, soa.radius[f]);
            if (forceLargestSharp)
                soa.radius[f] = std::nextafter(kSharpRadiusPx, 0.0f);
        }
        if (forceLargestSharp)
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x)
                    window.radiusPx[static_cast<std::size_t>(window.index(x, y))] =
                        std::nextafter(kSharpRadiusPx, 0.0f);

        r.band.C = C; r.band.W = W; r.band.H = H;
        HoldoutSoA none;
        runBand(r.band, makeScatterParams(W, H), soa, none, lut, false, &window);
        return r;
    };

    const Run sizeZero = run(0.0f, 100.0f, false);
    const Run subPixel = run(0.45f, 0.49f, false);
    const Run largest  = run(0.45f, 0.49f, true);

    // The sub-pixel run genuinely exercised the whole sharp band, and the
    // corpus is not trivially small.
    REQUIRE(sizeZero.fragments == subPixel.fragments);
    REQUIRE(subPixel.fragments == static_cast<std::size_t>(W * H));   // one run per pixel
    CHECK(sizeZero.maxRadius == 0.0f);
    CHECK(subPixel.maxRadius > 0.45f);
    CHECK(subPixel.maxRadius < kSharpRadiusPx);
    CHECK(subPixel.minRadius > 0.0f);

    auto bitIdentical = [&](const Band& a, const Band& b) {
        std::size_t diffs = 0;
        auto cmp = [&](const PodBuffer<float>& x, const PodBuffer<float>& y) {
            REQUIRE(x.size() == y.size());
            for (std::size_t i = 0; i < x.size(); ++i)
                if (x[i] != y[i])
                    ++diffs;
        };
        cmp(a.planes.color, b.planes.color);
        cmp(a.planes.alpha, b.planes.alpha);
        cmp(a.planes.claimed, b.planes.claimed);
        cmp(a.planes.oldArea, b.planes.oldArea);
        cmp(a.planes.oldMass, b.planes.oldMass);
        cmp(a.planes.arrival, b.planes.arrival);
        for (std::size_t i = 0; i < a.alpha.size(); ++i)
            if (a.alpha[i] != b.alpha[i])
                ++diffs;
        for (std::size_t i = 0; i < a.color.size(); ++i)
            if (a.color[i] != b.color[i])
                ++diffs;
        return diffs;
    };
    CHECK(bitIdentical(sizeZero.band, subPixel.band) == 0);
    CHECK(bitIdentical(sizeZero.band, largest.band) == 0);

    // And the output is the flatten of the input, so "bit-identical" is not
    // three copies of the same wrong answer.  The premultiplied pair is
    // checked as a pair.
    double worstAlpha = 0.0, worstColor = 0.0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const RefOver r = refSequentialOver(pixels[static_cast<std::size_t>(y) * W + x], C);
            worstAlpha = std::max(worstAlpha,
                std::fabs(static_cast<double>(subPixel.band.outAlpha(x, y)) - r.alpha));
            for (int c = 0; c < C; ++c)
                worstColor = std::max(worstColor,
                    std::fabs(static_cast<double>(subPixel.band.outColor(c, x, y))
                              - r.color[static_cast<std::size_t>(c)]));
        }
    CAPTURE(worstAlpha); CAPTURE(worstColor);
    CHECK(worstAlpha <= 1e-06);
    CHECK(worstColor <= 1e-06);
}

TEST_CASE("an exact 1 px diameter IS the sharp delta, at any edge_softness: fragments and "
          "residual alike deposit bit-identically to the sharp path in every plane")
{
    // The minimum kernel diameter is 1 px, and the 1x1 kernel is the sharp
    // path's own delta -- so radius == kSharpRadiusPx exactly must take the
    // sharp path, not the LUT.  Below edge_softness 1 the LUT's r=0.5 entry
    // happens to be a delta too and the distinction is invisible; above it
    // that entry is a soft 3x3 (centre row [0.1301, 0.3903, 0.1301] at 2.0)
    // and routing d = 1 through it would put a step at the very diameter the
    // size-0 parity gate protects.  Colour, alpha and arrival all go through
    // the same predicate at both deposit sites, and the residual's radius is
    // forced to 0.5 as well so scatterBackgroundCPU() is under the same test.
    // lastCoc is left out: it records the forced radius itself.
    const int C = 3, W = 40, H = 40, spp = 4;

    Lcg rng(0x5123u);
    std::vector<std::vector<SampleRecord>> pixels(static_cast<std::size_t>(W) * H);
    for (auto& v : pixels) {
        if (rng.unit() < 0.15f)
            continue;                               // some pixels stay empty
        float z = rng.range(1.05f, 30.0f);
        for (int s = 0; s < spp; ++s) {
            const float a = rng.range(0.02f, 1.0f);
            const float zb = (s % 2 == 0) ? z : z + rng.range(0.1f, 4.0f);
            v.push_back(makeSample(z, zb, a, {a * rng.unit(), a * rng.unit(), a * rng.unit()}));
            z = zb + rng.range(0.05f, 5.0f);
        }
    }

    const CocParams p = makeCocParams(CocMode::Manual, 50.0f, 2.8f, 36.0f, 10.0f,
                                      unitScale(WorldUnits::Meters), 1920.0f, 1.0f,
                                      1.0f, 1.0f, 100.0f, 30.0f);

    auto run = [&](const DiscKernelLUT& lut, float forcedRadius, Band& band) {
        const FlattenParams fp = makeFlattenParams(p, C, /*preMerge*/ false);
        SampleSoA soa;
        soa.begin(C, fp.groups);
        FlattenScratch scratch;
        ResidualWindow window;
        window.allocate(0, 0, W, H, forcedRadius);
        flattenIntoWithResidual(fp, 0, 0, W, H, soa, scratch, window,
            [&](int x, int y) { return pixels[static_cast<std::size_t>(y) * W + x]; });
        for (std::size_t f = 0; f < soa.fragmentCount(); ++f)
            soa.radius[f] = forcedRadius;
        for (std::size_t i = 0; i < window.radiusPx.size(); ++i)
            window.radiusPx[i] = forcedRadius;
        band.C = C; band.W = W; band.H = H;
        HoldoutSoA none;
        runBand(band, makeScatterParams(W, H), soa, none, lut, false, &window);
    };

    auto diffs = [&](const PodBuffer<float>& x, const PodBuffer<float>& y) {
        REQUIRE(x.size() == y.size());
        std::size_t n = 0;
        for (std::size_t i = 0; i < x.size(); ++i)
            if (x[i] != y[i])
                ++n;
        return n;
    };
    auto outputDiffs = [&](const Band& a, const Band& b) {
        std::size_t n = 0;
        for (std::size_t i = 0; i < a.alpha.size(); ++i)
            if (a.alpha[i] != b.alpha[i]) ++n;
        for (std::size_t i = 0; i < a.color.size(); ++i)
            if (a.color[i] != b.color[i]) ++n;
        return n;
    };

    const float sharp = std::nextafter(kSharpRadiusPx, 0.0f);
    for (float softness : {0.0f, 1.0f, 2.0f, 4.0f}) {
        CAPTURE(softness);
        // The LUT covers the boundary, so a radius that leaks into it comes
        // back as the r=0.5 entry -- soft above edge_softness 1 -- not as a
        // clamp artefact.
        const DiscKernelLUT lut(kSharpRadiusPx, 20.0f, softness, 1.0f);
        Band viaSharp, viaBoundary;
        run(lut, sharp, viaSharp);
        run(lut, kSharpRadiusPx, viaBoundary);
        CHECK(diffs(viaSharp.planes.color,     viaBoundary.planes.color)     == 0);
        CHECK(diffs(viaSharp.planes.alpha,     viaBoundary.planes.alpha)     == 0);
        CHECK(diffs(viaSharp.planes.claimed,   viaBoundary.planes.claimed)   == 0);
        CHECK(diffs(viaSharp.planes.oldArea,   viaBoundary.planes.oldArea)   == 0);
        CHECK(diffs(viaSharp.planes.oldMass,   viaBoundary.planes.oldMass)   == 0);
        CHECK(diffs(viaSharp.planes.arrival,   viaBoundary.planes.arrival)   == 0);
        CHECK(outputDiffs(viaSharp, viaBoundary) == 0);
    }
}

TEST_CASE("an alpha 0.9 surface on a gentle ramp of fractional diameters reads its weight-sum "
          "oracle within term count -- the over-read deficit-only division cannot fix, and the "
          "nearest-node scatter's weight sum of the same rig overshoots 1 by more than twice as much")
{
    // A flat alpha 0.9 field whose CoC radius climbs slowly down the band, so
    // every row is a different fractional diameter.  What a pixel reads
    // depends on the raw weight sum S, which is 1 only if the kernels tile.
    // Nearest-node snapping makes rows either side of a node crossing
    // rasterise discs a whole node apart, and the surplus rows read straight
    // through as alpha > 0.9 -- the fill divides deficits only.  The same rig
    // with every radius snapped to its nearest node first is that scatter,
    // bit for bit, through the same driver, and is the control that says the
    // rig can see the defect.
    //
    // Per pixel the oracle is S and its term count N from the reference
    // weights: one surface covers min(S, 1) at a, and its surplus S - 1 lands
    // on itself at a(1 - a).  The fill division is decided on the measured
    // float arrival, as the resolve decides it: deciding it on S puts pixels
    // within rounding of the deficit tolerance on the other side of it.
    //
    // The residual (0.1 per pixel) scatters at the same per-pixel radius, so
    // arrival and coverage move together and neither scheme is helped by
    // the fill here.
    const float size = 100.0f, F = 10.0f;
    const CocParams p = makeCocParams(CocMode::Manual, 50.0f, 2.8f, 36.0f, F, 1000.0f,
                                      256.0f, 1.0f, 1.0f, 1.0f, 100.0f, size);
    const int W = 24, H = 160;
    const float alpha = 0.9f, unpremult = 0.5f;

    struct Segment { float r0, slope; };
    // Radius 1 -> 1.8, 6 -> 6.8, 16 -> 16.8 px over the band's 160 rows: the
    // near-focus end where the grid is finest, the middle, and the coarse
    // 0.5 px region.
    const Segment segments[] = {{1.0f, 0.005f}, {6.0f, 0.005f}, {16.0f, 0.005f}};

    for (const Segment& seg : segments) {
        CAPTURE(seg.r0);
        auto rOf = [&](int y) { return seg.r0 + seg.slope * static_cast<float>(y); };
        auto zOf = [&](float r) { return F / (1.0f - r / size); };
        const int pad = static_cast<int>(std::ceil(rOf(H + 40) + 2.0f));
        DiscKernelLUT lut(0.5f, rOf(H + pad) + 1.0f, 1.0f, 1.0f);

        struct Reading { double worstAlpha = 0.0, worstRatio = 0.0, maxS = -9.0; };
        auto run = [&](bool snap) -> Reading {
            CAPTURE(snap);
            const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ true);
            SampleSoA soa;
            soa.begin(1, fp.groups);
            FlattenScratch scratch;
            ResidualWindow window;
            window.allocate(-pad, -pad, W + 2 * pad, H + 2 * pad, rOf(H + pad));
            flattenIntoWithResidual(fp, -pad, -pad, W + pad, H + pad, soa, scratch, window,
                [&](int, int y) -> std::vector<SampleRecord> {
                    const float z = zOf(rOf(y));
                    return {makeSample(z, z, alpha, {alpha * unpremult})};
                });
            for (std::size_t f = 0; f < soa.fragmentCount(); ++f) {
                REQUIRE(refBracket(soa.radius[f]).passes == 2);   // every row fractional
                if (snap)
                    soa.radius[f] = snappedToNearestNode(soa.radius[f]);
            }
            if (snap)
                for (int y = 0; y < window.height; ++y)
                    for (int x = 0; x < window.width; ++x) {
                        const std::size_t i = static_cast<std::size_t>(
                            window.index(window.x + x, window.y + y));
                        window.radiusPx[i] = snappedToNearestNode(window.radiusPx[i]);
                    }

            Band band;
            band.C = 1; band.W = W; band.H = H;
            HoldoutSoA none;
            runBand(band, makeScatterParams(W, H), soa, none, lut, false, &window);

            Reading rd;
            const double a = alpha;
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    double S = 0.0;
                    long   n = 0;
                    for (std::size_t f = 0; f < soa.fragmentCount(); ++f) {
                        const float r  = soa.radius[f];
                        const int   dx = x - static_cast<int>(soa.x[f]);
                        const int   dy = y - static_cast<int>(soa.y[f]);
                        if (std::abs(dy) > r + 2.0f || std::abs(dx) > r + 2.0f)
                            continue;
                        const double w = refBlendedWeight(lut, r, dx, dy);
                        if (w != 0.0) {
                            S += w;
                            ++n;
                        }
                    }
                    const float  arrivalF = band.planes.arrival[band.at(x, y)];
                    const bool   fill = arrivalF > kFillMinArrival && arrivalF < 1.0f - kFillDeficitTol;
                    const double aRaw = a * std::min(S, 1.0) + a * (1.0 - a) * std::max(S - 1.0, 0.0);
                    const double aOr  = std::min(fill ? aRaw / S : aRaw, 1.0);
                    const double got  = band.outAlpha(x, y);
                    const double tol  = 2.0 * static_cast<double>(n) * kUlp;
                    CAPTURE(x);
                    CAPTURE(y);
                    CAPTURE(S);
                    CAPTURE(n);
                    CAPTURE(got);
                    CAPTURE(aOr);
                    CAPTURE(arrivalF);
                    CHECK(std::fabs(got - aOr) <= tol);
                    CHECK(std::fabs(static_cast<double>(arrivalF) - S) <= tol);
                    rd.maxS       = std::max(rd.maxS, S);
                    rd.worstAlpha = std::max(rd.worstAlpha, std::fabs(got - a));
                    rd.worstRatio = std::max(rd.worstRatio,
                        std::fabs(static_cast<double>(band.outColor(0, x, y)) / got - unpremult));
                }
            return rd;
        };

        const Reading blend = run(false);
        const Reading snapd = run(true);
        CAPTURE(blend.worstAlpha); CAPTURE(blend.maxS);
        CAPTURE(snapd.worstAlpha); CAPTURE(snapd.maxS);

        CHECK(blend.worstRatio <= 1e-06);
        CHECK(snapd.worstRatio <= 1e-06);
        // Nearest node: never better than the blend, and wherever the
        // brackets are wide enough to see (at 1 px the grid is 0.001 px fine
        // and the residual error is the ramp's own) its weight sum overshoots
        // 1 by more than twice the blend's, and its alpha error with it.
        CHECK(snapd.worstAlpha >= blend.worstAlpha);
        if (seg.r0 >= 6.0f) {
            CHECK(snapd.worstAlpha > 2.0 * blend.worstAlpha);
            CHECK(snapd.maxS - 1.0 > 2.0 * (blend.maxS - 1.0));
        }
    }
}

TEST_CASE("the share-side arrival identity at large CoC: a fully-covered field's arrival "
          "reads 1 to the float accumulation bound of the deposits the design specifies, "
          "and those same deposits re-summed in double read 1 to 5e-7 -- node and "
          "off-node radii, K = 4/8/16, alpha 1.0/0.9/0.5/0.2, point and split parents")
{
    // THE ARRIVAL PLANE IS ONE FLOAT PER PIXEL AND THE COMPOSITE DIVIDES BY IT,
    // so its accuracy is the output's.  On a uniform fully-covered field every
    // pixel's claims tile to exactly 1 in exact arithmetic (one share claim per
    // parent at that parent's residual radius, plus the residual itself at the
    // same radius), and what the plane actually holds is the naive float sum
    // of those products in deposit order.  Two readings separate a weight bug
    // from accumulation: the SAME float products re-summed in double must land
    // on the target to the LUT's normalisation residual (~1e-7 per entry), and
    // the float plane must land inside the recursive-summation bound
    // n * 2^-24 for the n terms the DESIGN says the pixel receives -- counted
    // from the residual radius and the LUT, never from the fragments that
    // actually claimed.  The count is the load-bearing half: a 16-part parent
    // whose deepest part sits near focus pools 114 terms per pixel, while its
    // parts spread over their own radii sum 48564, and only the latter reads
    // 1e-5..1e-4 off -- uniform across the interior, erratic in sign across
    // alpha and K, vanishing below ~4 px.  Measured, pooled: worst 8.2e-5 at
    // 33.3 px against a 7.1e-4 bound, and 3.2e-6 on the near-focus split
    // parent against 7.3e-6..3.5e-5; unpooled, that parent reads 1.4e-4
    // against 7.3e-6.
    //
    // Three modes per fixture: a single sample with its virtual background
    // (arrival = share + residual = 1), the same with NO background (arrival =
    // the share sum alone, 1 - T), and fog over an opaque sample (T = 0
    // exactly, two claimants at two radii).  alpha 1.0 is the residual-free
    // control in every mode.
    constexpr double u = 5.9604644775390625e-08;   // 2^-24, float unit roundoff
    const float F = 10.0f;
    const int W = 12, H = 12;
    const float unpremult = 0.6f;

    struct Fixture { const char* name; float size; float zf, zb; bool onNode; };
    const Fixture fixtures[] = {
        {"point, fine node 512/43",   kernelGridRadius(kKernelFineOrigin - 43), 5.0f, 5.0f, true},
        {"point, fine off-node 12",   12.0f,  5.0f, 5.0f,  false},
        {"point, coarse node 20",     20.0f,  5.0f, 5.0f,  true},
        {"point, coarse off-node 20.25", 20.25f, 5.0f, 5.0f, false},
        {"point, coarse off-node 33.3",  33.3f,  5.0f, 5.0f, false},
        {"split parent behind focus [15,40], 10..22.5 px", 30.0f, 15.0f, 40.0f, false},
        {"split parent in front [4,6], 30..13.3 px",       20.0f, 4.0f,  6.0f,  false},
        {"split parent up to near focus [4,9], 36..3.7 px",  24.0f, 4.0f,  9.0f,  false},
    };

    for (const Fixture& fx : fixtures) {
        INFO("fixture: ", fx.name);
        const bool volumetric = fx.zb > fx.zf;
        const CocParams p = makeManualRig(fx.size, F);
        const float rMax = std::max(radiusPixels(p, fx.zf), radiusPixels(p, fx.zb));
        const int pad = static_cast<int>(std::ceil(rMax)) + 3;
        DiscKernelLUT lut(2.0f, rMax + 2.0f, 1.0f, 1.0f);
        REQUIRE(lut.minRadius() == 2.0f);
        REQUIRE(rMax >= 11.0f);

        // Non-zero weights over the bracket passes at `radius`: the term count
        // one unit claim at that radius costs a destination pixel.
        auto termCount = [&](float radius) {
            long n = 0;
            const KernelGridBracket br = kernelGridBracket(radius);
            const int   idx[2] = { br.indexA, br.indexB };
            const float bl[2]  = { 1.0f - br.frac, br.frac };
            const int passes = (br.indexB != br.indexA) ? 2 : 1;
            for (int q = 0; q < passes; ++q) {
                if (bl[q] == 0.0f)
                    continue;
                const KernelView kv = lut.kernel(kernelGridRadius(idx[q]), 0, 0, 0.0f, 0);
                REQUIRE(kv.valid());
                for (int r = 0; r < kv.rowCount; ++r) {
                    const RowSpan& span = kv.row(r);
                    if (span.empty())
                        continue;
                    const float* w = kv.rowWeights(r);
                    for (int i = 0; i <= span.xEnd - span.xStart; ++i)
                        if (w[i] != 0.0f)
                            ++n;
                }
            }
            return n;
        };

        for (int K : {4, 8, 16}) {
            CAPTURE(K);
            const FrameDepthRange range = makeFrameDepthRange(fx.zf, fx.zb, K);
            const float stepPx = volumetricPieceStepPx(p, range, 0.25f);
            // The deepest staged fragment's own radius: the last volumetric
            // piece's for a slab, the sample's own for a point.
            float rDeepest = fx.size;
            if (volumetric) {
                std::vector<VolumetricPiece> pieces(static_cast<std::size_t>(kMaxVolumetricPieces));
                const int n = volumetricPieceBounds(p, fx.zf, fx.zb, 0.5f, stepPx,
                                                    pieces.data(), K + 1);
                REQUIRE(n >= 1);
                const VolumetricPiece& last = pieces[static_cast<std::size_t>(n - 1)];
                rDeepest = radiusPixels(p, sampleMidDepth(last.zFront, last.zBack));
            }

            for (float alpha : {1.0f, 0.9f, 0.5f, 0.2f}) {
                CAPTURE(alpha);
                for (int mode = 0; mode < 3; ++mode) {
                    if (mode == 2 && volumetric)
                        continue;
                    CAPTURE(mode);
                    const bool withBackground = (mode != 1);
                    const bool fogOverOpaque  = (mode == 2);

                    FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ true);
                    fp.pieceStepPx         = stepPx;
                    fp.maxVolumetricPieces = K + 1;
                    SampleSoA soa;
                    soa.begin(1, fp.groups);
                    FlattenScratch scratch;
                    ResidualWindow window;
                    window.allocate(-pad, -pad, W + 2 * pad, H + 2 * pad, rMax);
                    flattenIntoWithResidual(fp, -pad, -pad, W + pad, H + pad,
                                            soa, scratch, window,
                        [&](int, int) -> std::vector<SampleRecord> {
                            if (fogOverOpaque)
                                return {makeSample(5.0f, 5.0f, alpha, {alpha * unpremult}),
                                        makeSample(5.5f, 5.5f, 1.0f, {unpremult})};
                            return {makeSample(fx.zf, fx.zb, alpha, {alpha * unpremult})};
                        });

                    const int cx = W / 2, cy = H / 2;
                    const std::size_t ci = static_cast<std::size_t>(window.index(cx, cy));
                    const float T = window.t[ci];
                    const float rRes = window.radiusPx[ci];
                    if (alpha == 1.0f || fogOverOpaque)
                        REQUIRE(T == 0.0f);
                    else
                        REQUIRE(T == doctest::Approx(1.0f - alpha).epsilon(1e-6));
                    REQUIRE(rRes == (fogOverOpaque ? radiusPixels(p, 5.5f) : rDeepest));
                    REQUIRE(rRes >= lut.minRadius());
                    const KernelGridBracket brOwn = kernelGridBracket(fogOverOpaque ? fx.size : rRes);
                    if (fx.onNode)
                        REQUIRE(brOwn.indexB == brOwn.indexA);
                    else
                        REQUIRE(brOwn.indexB != brOwn.indexA);
                    if (fogOverOpaque) {
                        const KernelGridBracket brRes = kernelGridBracket(rRes);
                        REQUIRE(brRes.indexB != brRes.indexA);
                    }

                    // The design's term count at a pixel: the share claim at
                    // the residual radius, the residual at the same radius when
                    // it is non-zero, and the second parent's claim in mode 2.
                    long nExpected = termCount(rRes);
                    if (withBackground && T > kFillDeficitTol)
                        nExpected += termCount(rRes);
                    if (fogOverOpaque)
                        nExpected += termCount(fx.size);
                    const double target = withBackground ? 1.0 : 1.0 - static_cast<double>(T);
                    const double arrivalBound = static_cast<double>(nExpected) * u + 5e-7;

                    // The numerator's term count: every fragment of one source
                    // pixel rasterises its own kernel into colour and alpha.
                    long nNumerator = 0;
                    for (std::size_t f = 0; f < soa.fragmentCount(); ++f)
                        if (soa.x[f] == cx && soa.y[f] == cy)
                            nNumerator += termCount(soa.radius[f]);
                    REQUIRE(nNumerator > 0);

                    Band band;
                    band.C = 1; band.W = W; band.H = H;
                    HoldoutSoA none;
                    runBand(band, makeScatterParams(W, H), soa, none, lut, false,
                            withBackground ? &window : nullptr);

                    // The double oracle: the very float products the plane
                    // received at the band centre, summed in double.
                    double dsum = 0.0;
                    auto addClaim = [&](float radius, float scale, int sx, int sy) {
                        const KernelGridBracket br = kernelGridBracket(radius);
                        const int   idx[2] = { br.indexA, br.indexB };
                        const float bl[2]  = { 1.0f - br.frac, br.frac };
                        const int passes = (br.indexB != br.indexA) ? 2 : 1;
                        for (int q = 0; q < passes; ++q) {
                            if (bl[q] == 0.0f)
                                continue;
                            const float s = scale * bl[q];
                            const KernelView kv = lut.kernel(kernelGridRadius(idx[q]), 0, 0, 0.0f, 0);
                            const int row = (cy - sy) + kv.radiusY;
                            if (row < 0 || row >= kv.rowCount)
                                continue;
                            const RowSpan& span = kv.row(row);
                            const int dx = cx - sx;
                            if (span.empty() || dx < span.xStart || dx > span.xEnd)
                                continue;
                            dsum += static_cast<double>(kv.rowWeights(row)[dx - span.xStart] * s);
                        }
                    };
                    std::size_t sharpFragments = 0;
                    for (std::size_t f = 0; f < soa.fragmentCount(); ++f) {
                        if (!(soa.radius[f] > kSharpRadiusPx))
                            ++sharpFragments;
                        if (soa.arrivalShare[f] != 0.0f)
                            addClaim(soa.radius[f], soa.arrivalShare[f], soa.x[f], soa.y[f]);
                    }
                    REQUIRE(sharpFragments == 0u);
                    if (withBackground)
                        for (int y = 0; y < window.height; ++y)
                            for (int x = 0; x < window.width; ++x) {
                                const std::size_t i = static_cast<std::size_t>(
                                    window.index(window.x + x, window.y + y));
                                if (window.t[i] > kFillDeficitTol)
                                    addClaim(window.radiusPx[i], window.t[i],
                                             window.x + x, window.y + y);
                            }
                    CHECK(std::fabs(dsum - target) <= 5e-7);

                    double worstArrival = 0.0, worstAlpha = 0.0, worstRatio = 0.0;
                    for (int y = 0; y < H; ++y)
                        for (int x = 0; x < W; ++x) {
                            const double d = band.planes.arrival[static_cast<std::size_t>(y) * W + x];
                            worstArrival = std::max(worstArrival, std::fabs(d - target));
                            const double a = band.outAlpha(x, y);
                            worstAlpha = std::max(worstAlpha, std::fabs(a - (fogOverOpaque ? 1.0 : alpha)));
                            worstRatio = std::max(worstRatio,
                                std::fabs(static_cast<double>(band.outColor(0, x, y)) / a - unpremult));
                        }
                    CAPTURE(nExpected); CAPTURE(nNumerator);
                    CAPTURE(worstArrival); CAPTURE(worstAlpha); CAPTURE(worstRatio);

                    CHECK(worstArrival <= arrivalBound);
                    // Without the background the fill divides the share sum by
                    // itself, so alpha reads 1 there by construction; the
                    // numerator's own accumulation is what alpha carries in the
                    // other two modes, on top of the arrival it divides by.
                    if (withBackground)
                        CHECK(worstAlpha <= static_cast<double>(nNumerator + nExpected) * u + 1e-6);
                    CHECK(worstRatio <= unpremult * 2.0 * static_cast<double>(nNumerator) * u + 1e-6);
                }
            }
        }
    }
}

// ===========================================================================
// The per-deposit body
// ===========================================================================


namespace {

constexpr double kUlp24 = 1.0 / 16777216.0;

struct StreamPixel {
    StreamPlanes planes;
    int          channels = 0;

    explicit StreamPixel(int channelCount) : channels(channelCount)
    {
        planes.allocate(channelCount, 1, 1);
    }

    void deposit(float w, float alpha, float signedCoc, const float* color)
    {
        float x = 0.0f;
        depositStreamSpanRecency(planes.view(), 0, &w, &x, 1, alpha, signedCoc, color, channels);
    }

    float Q() const { return planes.claimed.data()[0]; }
    float A() const { return planes.alpha.data()[0]; }
    float C(int c) const { return planes.color.data()[c]; }
};

// n positive weights whose running float sum is exactly 1 after the last:
// the last weight is 1 minus the running sum the deposit body itself forms,
// so the last deposit's fit fills the pixel.  Unequal on purpose.
std::vector<float> tilingWeights(int n, float total = 1.0f)
{
    std::vector<float> w;
    if (n == 1) {
        w.push_back(total);
        return w;
    }
    std::vector<double> r(static_cast<std::size_t>(n));
    std::uint32_t s = 0x9e3779b9u ^ static_cast<std::uint32_t>(n);
    double sum = 0.0;
    for (double& v : r) {
        s = s * 1664525u + 1013904223u;
        v = 0.5 + static_cast<double>(s >> 8) / 16777216.0;
        sum += v;
    }
    float q = 0.0f;
    for (int i = 0; i < n - 1; ++i) {
        const float wi = static_cast<float>(total * r[static_cast<std::size_t>(i)] / sum);
        w.push_back(wi);
        q += wi;
    }
    w.push_back(total - q);
    return w;
}

// The per-deposit rule with one pooled claimed share, x = fit + (w - fit)*T,
// T = (Q - A)/Q.  Kept only to show the identity tests can see why it was
// rejected.
struct NaivePixel {
    float Q = 0.0f, A = 0.0f, C = 0.0f;

    void deposit(float w, float alpha, float color)
    {
        const float fit = (w < 1.0f - Q) ? w : (1.0f - Q);
        const float T   = (Q > 0.0f) ? (Q - A) / Q : 1.0f;
        const float x   = fit + (w - fit) * T;
        A += alpha * x;
        C += color * x;
        Q += fit;
    }
};

} // namespace

TEST_CASE("stream deposit: a tiling of n opaque deposits reads A == 1 exactly, colour == its own "
          "colour within n roundings")
{
    for (int n : {1, 7, 1000}) {
        CAPTURE(n);
        const float color[3] = {0.5f, 0.3f, 1.7f};
        StreamPixel px(3);
        for (float w : tilingWeights(n))
            px.deposit(w, 1.0f, 4.0f, color);

        CHECK(px.A() == 1.0f);
        CHECK(px.Q() == 1.0f);
        // A power-of-two colour scales every rounding the alpha made, so its
        // colour:alpha ratio is exact; the others carry one rounding per
        // product and per sum.
        CHECK(px.C(0) == 0.5f * px.A());
        for (int c = 1; c < 3; ++c)
            CHECK(std::fabs(px.C(c) / px.A() - color[c]) <= 2.0 * n * color[c] * kUlp24);
    }
}

TEST_CASE("stream deposit: two full-coverage 0.5 layers of n deposits each read 0.75 within 4n ulps; "
          "the pooled (naive) rule reads 1 - 0.5*prod(1 - 0.5 w_i) -> 1 - 0.5*exp(-0.5)")
{
    const float u1 = 0.8f, u2 = 0.2f;                 // unpremultiplied colours
    const float c1[2] = {0.5f * u1, 0.5f * 0.6f};
    const float c2[2] = {0.5f * u2, 0.5f * 0.6f};
    for (float rearCoc : {2.0f, 6.0f}) {              // relabel on reaching N, jump rotation
        for (int n : {1, 7, 1000}) {
            CAPTURE(rearCoc);
            CAPTURE(n);
            const std::vector<float> w = tilingWeights(n);
            const double bound = 4.0 * n * kUlp24;

            StreamPixel px(2);
            for (float wi : w) px.deposit(wi, 0.5f, 2.0f, c1);
            for (float wi : w) px.deposit(wi, 0.5f, rearCoc, c2);

            // Front layer transmits 0.5 of the full pixel; the rear layer
            // adds 0.5 of that.  Colour follows with the same weights.
            CHECK(std::fabs(px.A() - 0.75) <= bound);
            CHECK(std::fabs(px.C(0) - (0.5 * u1 + 0.25 * u2)) <= bound);
            // Same unpremultiplied colour in both layers: C/A is that colour.
            CHECK(std::fabs(px.C(1) / px.A() - 0.6) <= 2.0 * bound);

            NaivePixel naive;
            for (float wi : w) naive.deposit(wi, 0.5f, c1[0]);
            for (float wi : w) naive.deposit(wi, 0.5f, c2[0]);
            double prod = 1.0;
            for (float wi : w) prod *= 1.0 - 0.5 * wi;
            CHECK(std::fabs(naive.A - (1.0 - 0.5 * prod)) <= bound);
            // The naive rule's colour follows its own (wrong) alpha.
            CHECK(std::fabs((naive.C - 0.5 * u1) - u2 * (naive.A - 0.5)) <= bound);
            if (n == 1) {
                CHECK(naive.A == 0.75f);
            } else {
                CHECK(std::fabs(naive.A - 0.75) > bound);
                if (n == 1000)
                    CHECK(std::fabs(prod - std::exp(-0.5)) < 1e-3);
            }
        }
    }
}

TEST_CASE("stream deposit: an opaque tiled layer behind a partly covering 0.8 layer reads A == 1 "
          "(the CoC-jump rotation's case)")
{
    const float uf = 0.9f, uo = 0.3f;
    const float cf[1] = {0.8f * uf};
    const float co[1] = {uo};
    for (int m : {1, 5}) {
        for (int n : {7, 1000}) {
            CAPTURE(m);
            CAPTURE(n);
            StreamPixel px(1);
            double fogArea = 0.0;
            for (float wi : tilingWeights(m, 0.4f)) {
                px.deposit(wi, 0.8f, -5.0f, cf);
                fogArea += wi;
            }
            for (float wi : tilingWeights(n))
                px.deposit(wi, 1.0f, 3.0f, co);

            const double bound = 4.0 * (m + n) * kUlp24;
            // The opaque layer fills the free area and then, through the
            // fog's transmittance 0.2, the fog's area.
            CHECK(std::fabs(px.A() - 1.0) <= bound);
            const double cOracle = 0.8 * uf * fogArea + uo * ((1.0 - fogArea) + 0.2 * fogArea);
            CHECK(std::fabs(px.C(0) - cOracle) <= bound);
        }
    }
}

TEST_CASE("stream deposit: straddle -- w 0.5 alpha 1 then w 1.0 alpha 0.5 reads 0.75 exactly")
{
    for (float rearCoc : {1.0f, 9.0f}) {
        CAPTURE(rearCoc);
        const float cA[1] = {0.5f};
        const float cB[1] = {0.3f};
        StreamPixel px(1);
        px.deposit(0.5f, 1.0f, 1.0f, cA);
        px.deposit(1.0f, 0.5f, rearCoc, cB);
        // The rear deposit's free half is exposed, its other half lies
        // behind an opaque front: x_B = 0.5 exactly.
        CHECK(px.A() == 0.75f);
        CHECK(px.C(0) == 0.5f * 0.5f + 0.3f * 0.5f);
    }
}

TEST_CASE("stream deposit: a single sharp deposit reads A == alpha and C == colour bit-exact")
{
    const float alphas[] = {0.0f, 1e-7f, 0.3f, 0.999f, 1.0f};
    for (float a : alphas) {
        CAPTURE(a);
        const float color[3] = {0.1f * a, 0.7f, 3.25f};
        StreamPixel px(3);
        px.deposit(1.0f, a, 0.0f, color);
        const float A = px.A();
        CHECK(std::memcmp(&A, &a, sizeof(float)) == 0);
        for (int c = 0; c < 3; ++c) {
            const float got = px.C(c);
            CHECK(std::memcmp(&got, &color[c], sizeof(float)) == 0);
        }
        CHECK(px.Q() == 1.0f);
    }
}

TEST_CASE("stream deposit: pixels of one span are independent -- a span equals per-pixel calls")
{
    const float color[2] = {0.25f, 0.6f};
    StreamPlanes row;
    row.allocate(2, 3, 1);
    StreamPixel p0(2), p1(2), p2(2);
    StreamPixel* single[3] = {&p0, &p1, &p2};
    const float seq[][3] = {{0.3f, 0.6f, 1.0f}, {0.9f, 0.2f, 0.5f}, {0.4f, 0.4f, 0.4f}};
    const float coc[3] = {-2.0f, -1.5f, 4.0f};
    const float alpha[3] = {0.5f, 1.0f, 0.25f};
    for (int d = 0; d < 3; ++d) {
        float x[3];
        depositStreamSpanRecency(row.view(), 0, seq[d], x, 3, alpha[d], coc[d], color, 2);
        for (int i = 0; i < 3; ++i)
            single[i]->deposit(seq[d][i], alpha[d], coc[d], color);
    }
    for (int i = 0; i < 3; ++i) {
        CHECK(row.alpha.data()[i] == single[i]->A());
        CHECK(row.claimed.data()[i] == single[i]->Q());
        for (int c = 0; c < 2; ++c)
            CHECK(row.color.data()[c * 3 + i] == single[i]->C(c));
    }
}

TEST_CASE("orderedDepthKey is monotone over negatives, -0/+0, subnormals, 1e12 and infinity")
{
    const float inf = std::numeric_limits<float>::infinity();
    const float denorm = std::numeric_limits<float>::denorm_min();
    const float values[] = {-inf, -1e12f, -3.5f, -1.0f, -1e-38f, -1e-40f, -denorm, -0.0f,
                            0.0f, denorm, 1e-40f, 1e-38f, 1e-6f, 1.0f, 1.0000001f, 10.0f,
                            1e12f, 3e38f, inf};
    const int n = static_cast<int>(sizeof(values) / sizeof(values[0]));
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            CAPTURE(values[i]);
            CAPTURE(values[j]);
            // -0 and +0 compare equal as floats but take adjacent keys: the
            // key order refines the float order, it never contradicts it.
            if (values[i] < values[j])
                CHECK(orderedDepthKey(values[i]) < orderedDepthKey(values[j]));
            if (i < j)
                CHECK(orderedDepthKey(values[i]) < orderedDepthKey(values[j]));
        }
    }

    std::uint32_t s = 12345u;
    for (int k = 0; k < 200000; ++k) {
        s = s * 1664525u + 1013904223u;
        std::uint32_t ba = s;
        s = s * 1664525u + 1013904223u;
        std::uint32_t bb = s;
        float a, b;
        std::memcpy(&a, &ba, sizeof a);
        std::memcpy(&b, &bb, sizeof b);
        if (std::isnan(a) || std::isnan(b))
            continue;
        if (a < b)
            CHECK(orderedDepthKey(a) < orderedDepthKey(b));
        else if (b < a)
            CHECK(orderedDepthKey(b) < orderedDepthKey(a));
    }
}

TEST_CASE("volumetricPieceBounds: shares sum to 1, the focal plane is a cut, at most K+1 pieces, "
          "at most one step of CoC per piece")
{
    const CocParams manual = makeCocParams(CocMode::Manual, 50.0f, 2.8f, 36.0f,
                                           10.0f, 1000.0f, 1920.0f, 1.0f,
                                           1.0f, 1.0f, 40.0f, 64.0f);
    const CocParams physical = makeCocParams(CocMode::Physical, 50.0f, 2.8f, 36.0f,
                                             10.0f, 1000.0f, 1920.0f, 1.0f,
                                             1.0f, 1.0f, 100.0f, 10.0f);

    const auto checkPieces = [](const CocParams& p, float zf, float zb, float a, float step,
                                const VolumetricPiece* pc, int count, int maxPieces, bool stepBound) {
        CAPTURE(zf);
        CAPTURE(zb);
        CAPTURE(count);
        REQUIRE(count >= 1);
        CHECK(count <= maxPieces);
        const bool allSharp = !(std::fabs(signedCocPixels(p, zf)) > kSharpRadiusPx)
                           && !(std::fabs(signedCocPixels(p, zb)) > kSharpRadiusPx);
        CHECK(pc[0].zFront == zf);
        CHECK(pc[count - 1].zBack == zb);

        const double n = count;
        double tSum = 0.0, trans = 1.0, colour = 0.0;
        bool focusIsCut = false;
        for (int i = 0; i < count; ++i) {
            if (i > 0)
                CHECK(pc[i].zFront == pc[i - 1].zBack);
            CHECK(pc[i].zBack > pc[i].zFront);
            tSum   += pc[i].t;
            colour += pc[i].colorScale * trans;
            trans  *= 1.0 - pc[i].alpha;
            if (pc[i].zBack == p._focusDistance && i + 1 < count)
                focusIsCut = true;
            if (stepBound && !allSharp) {
                const bool   front = pc[i].zBack <= p._focusDistance;
                const double k     = cocCoefficient(p, front);
                const double dCoc  = std::fabs(signedCocPixels(p, pc[i].zBack)
                                             - signedCocPixels(p, pc[i].zFront));
                // Each endpoint is a double inversion rounded to float and
                // re-evaluated in float: a few roundings of k*S/z each.
                const double slack = 12.0 * k * p._focusDistance / pc[i].zFront * kUlp24;
                CHECK(dCoc <= step + slack);
            }
        }
        CHECK(std::fabs(tSum - 1.0) <= n * kUlp24);
        // Transmittance of the pieces multiplies back to the parent's; the
        // premultiplied colour of their `over` reconstructs the parent's.
        CHECK(std::fabs(trans - (1.0 - a)) <= 8.0 * n * kUlp24);
        if (a > 0.0f)
            CHECK(std::fabs(colour - 1.0) <= 8.0 * n * kUlp24 / a);
        if (zf < p._focusDistance && zb > p._focusDistance && !allSharp)
            CHECK(focusIsCut);
        if (allSharp)
            CHECK(count == 1);
    };

    for (const CocParams& p : {manual, physical}) {
        for (int K : {4, 16, 64, 128}) {
            CAPTURE(K);
            const FrameDepthRange range = makeFrameDepthRange(2.0f, 200.0f, K);
            const float step = volumetricPieceStepPx(p, range, 0.25f);
            CHECK(step >= 0.5f);
            VolumetricPiece pieces[kMaxVolumetricPieces];

            const int whole = volumetricPieceBounds(p, 2.0f, 200.0f, 0.7f, step, pieces, kMaxVolumetricPieces);
            checkPieces(p, 2.0f, 200.0f, 0.7f, step, pieces, whole, K + 1, true);

            std::uint32_t s = 777u + static_cast<std::uint32_t>(K);
            const auto next = [&s]() {
                s = s * 1664525u + 1013904223u;
                return static_cast<float>(s >> 8) / 16777216.0f;
            };
            for (int r = 0; r < 400; ++r) {
                float zf = 2.0f + 198.0f * next();
                float zb = 2.0f + 198.0f * next();
                if (zb < zf) std::swap(zf, zb);
                if (!(zb > zf)) continue;
                if (r % 5 == 0) { zf = 9.0f + next(); zb = 10.0f + 5.0f * next(); }
                const float a = (r % 7 == 0) ? 1.0f : next();
                const int count = volumetricPieceBounds(p, zf, zb, a, step, pieces, kMaxVolumetricPieces);
                checkPieces(p, zf, zb, a, step, pieces, count, K + 1, true);
            }
        }
    }

    SUBCASE("a binding cap keeps the count, the shares and the focal cut")
    {
        VolumetricPiece pieces[kMaxVolumetricPieces];
        for (int cap : {2, 3, 5}) {
            CAPTURE(cap);
            const int count = volumetricPieceBounds(manual, 2.0f, 200.0f, 0.5f, 0.5f, pieces, cap);
            checkPieces(manual, 2.0f, 200.0f, 0.5f, 0.5f, pieces, count, cap, false);
        }
        const int one = volumetricPieceBounds(manual, 2.0f, 200.0f, 0.5f, 0.5f, pieces, 1);
        CHECK(one == 1);
        CHECK(pieces[0].t == 1.0f);
    }

    SUBCASE("a span on the max_radius plateau is one piece; a point sample passes through")
    {
        VolumetricPiece pieces[kMaxVolumetricPieces];
        // Manual size 64, max 40: |CoC| >= 40 in front of z = 10 / (1 + 40/64).
        CHECK(volumetricPieceBounds(manual, 2.0f, 5.0f, 0.4f, 0.5f, pieces, kMaxVolumetricPieces) == 1);
        CHECK(pieces[0].t == 1.0f);
        CHECK(pieces[0].alpha == 0.4f);

        CHECK(volumetricPieceBounds(manual, 7.0f, 7.0f, 0.4f, 0.5f, pieces, kMaxVolumetricPieces) == 1);
        CHECK(pieces[0].t == 1.0f);
        CHECK(pieces[0].alpha == 0.4f);
        CHECK(pieces[0].colorScale == 1.0f);
    }
}

