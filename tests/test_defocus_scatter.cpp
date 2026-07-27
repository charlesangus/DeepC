// SPDX-License-Identifier: MIT
//
// ============================================================================
//
//  test_defocus_scatter — unit tests for the POD scatter core (M1.P3.T4)
//
//  Covers DeepCDefocusScatter.h / .cpp: the SoA flatten (M1.P3.T1), the band
//  scatter and both bucket-composite candidates (M1.P3.T2, T8, T9), and the
//  holdout SoA / boundary LUT (M1.P3.T3, T10, T11).  POD level only: no NDK,
//  no DDImage type, no live Nuke session.  scatterBandCPU() is driven through
//  a plain std::thread, which is the property the design reference asks for
//  ("thread-agnostic so unit tests can drive it directly with std::thread").
//
//  DETERMINISTIC.  The suite has no RNG: the fuzz/corpus cases use the
//  fixed-seed 64-bit LCG below, so every run — and every mutation-test run —
//  is bit-reproducible.  This matches tests/test_defocus_math.cpp's no-RNG
//  policy: the point of that policy is reproducibility, and a seeded, in-file
//  generator with no library dependency has it, while <random>'s distributions
//  are not required to be reproducible across implementations.
//
//  REFERENCE VALUES ARE DERIVED INDEPENDENTLY, never by calling the function
//  under test to produce its own expectation:
//    * refRadiusPx()/refPartitionAlpha()/refBucketOf()/refSplitSpan()/
//      refFlatten() re-derive the flatten from the design reference's formulae
//      in double precision (std::pow, not the shipped expm1/log1p chain), so a
//      change to the shipped expression is a difference, not a shared error;
//    * refRasterize() re-derives the whole deposit — which plane, which bucket,
//      which pixel — from the documented layout, independently of
//      depositRowSpan()/scatterSpanBothBuckets()/scatterBandCPU()'s drivers;
//    * every band-level identity is asserted against a hand-derived closed
//      form (the fragment's own alpha, the parent sample's alpha, an exact
//      sequential `over`), never against a re-run of the code.
//
//  Every tolerance is either exact (`==`) or carries a comment naming the
//  MEASURED error it was set from, at ~10x headroom.  Numbers marked PINNED
//  are documentation-with-teeth from PLAN/MILESTONES/M1-deepcdefocus-v1.md's
//  Decisions log, re-measured at this task; a change to them is meant to fail.
//
//  Like test_defocus_math.cpp this builds standalone with plain
//  `g++ -std=c++17 tests/test_defocus_scatter.cpp src/DeepCDefocusScatter.cpp`
//  as well as through the DEEPC_BUILD_TESTS CMake option — no NDK anywhere.
//
// ============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "../src/DeepCDefocusScatter.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <thread>
#include <vector>

using namespace deepc;

namespace {

// ===========================================================================
// Deterministic generator
// ===========================================================================

// Fixed-seed 64-bit LCG (Knuth's MMIX constants).  Deliberately not <random>:
// this suite must be bit-reproducible across toolchains, and only the raw
// engine — not the distributions — is specified to be.
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

// THE STANDARD RIG, and it is the one every pinned number in the milestone's
// Decisions log was measured on: Physical, f=50, N=2.8, filmback 36mm at
// 1920px, metres, over a measured depth range of [1, 100] at K=16.  Focused at
// 10m it splits 15 front / 1 back (the split the milestone records); focused at
// 1m the whole range is behind focus, which is the behind-focus rig.
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

DepthBuckets makeStandardBuckets(const CocParams& p, int k = 16)
{
    return makeBoundedDeltaCocBuckets(p, 1.0f, 100.0f, k);
}

// ===========================================================================
// Independent reference implementations
//
// Written from the design reference / header documentation, in double, with
// std::pow rather than the shipped expm1/log1p forms.  Nothing here calls the
// function it is the reference for.
// ===========================================================================

// radius = clamp(0.5 * coc_mm * (formatWidth/filmbackWidth) * sideMult, 0, maxR)
// with coc_mm = (f/N)*f/(S_mm - f) * |1 - S_mm/d_mm|, straight off the design
// reference's CoC model block.  Physical mode only (the flatten fixtures below
// are all physical); rebuilt from the RAW knobs, not CocParams' cached members.
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

// 1 - (1-a)^t, and its premultiplied-colour partner a(t)/a — the design
// reference's transmittance-preserving split, via std::pow in double.
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
        return t;                       // emissive limit: linear in t
    if (a >= 1.0)
        return (t > 0.0) ? 1.0 : 0.0;
    return refPartitionAlpha(a, t) / a;
}

// The bucket whose [boundary(i), boundary(i+1)] contains `depth`.
int refContainingBucket(const DepthBuckets& b, double depth)
{
    const int k = b.bucketCount();
    if (k <= 0)
        return 0;
    if (!(depth > b.boundary(0)))
        return 0;
    if (depth >= b.boundary(k))
        return k - 1;
    for (int i = 0; i < k; ++i)
        if (depth >= b.boundary(i) && depth < b.boundary(i + 1))
            return i;
    return k - 1;
}

struct RefWeight {
    int    index = 0;
    double frac  = 0.0;
};

// Position between bucket CENTRES — the fractional two-bucket assignment.
RefWeight refBucketOf(const DepthBuckets& b, double depth)
{
    RefWeight w;
    const int k = b.bucketCount();
    if (k <= 1)
        return w;
    if (!(depth > b.centre(0)))
        return w;
    if (depth >= b.centre(k - 1)) {
        w.index = k - 1;
        return w;
    }
    for (int i = 0; i + 1 < k; ++i) {
        if (depth >= b.centre(i) && depth < b.centre(i + 1)) {
            const double span = static_cast<double>(b.centre(i + 1)) - b.centre(i);
            w.index = i;
            w.frac  = (span > 0.0) ? (depth - b.centre(i)) / span : 0.0;
            return w;
        }
    }
    w.index = k - 1;
    return w;
}

struct RefPart {
    double zFront = 0.0;
    double zBack  = 0.0;
    double t      = 1.0;
    double alpha  = 0.0;
    double colorScale = 1.0;
};

// Cut a span at every bucket boundary strictly inside it.
std::vector<RefPart> refSplitSpan(const DepthBuckets& b, double zFront, double zBack, double alpha)
{
    std::vector<RefPart> out;
    if (!(zBack > zFront)) {
        RefPart p;
        p.zFront = zFront;
        p.zBack  = zBack;
        p.t      = 1.0;
        p.alpha  = std::min(std::max(alpha, 0.0), 1.0);
        p.colorScale = 1.0;
        out.push_back(p);
        return out;
    }

    const double inv = 1.0 / (zBack - zFront);
    double partFront = zFront;
    double uPrev     = 0.0;

    for (int i = 0; i <= b.bucketCount(); ++i) {
        const double bz = b.boundary(i);
        if (!(bz > zFront))
            continue;
        if (!(bz < zBack))
            break;
        const double u = std::min(std::max((bz - zFront) * inv, 0.0), 1.0);
        if (!(u > uPrev))
            continue;

        RefPart p;
        p.zFront = partFront;
        p.zBack  = bz;
        p.t      = u - uPrev;
        p.alpha  = refPartitionAlpha(alpha, p.t);
        p.colorScale = refPartitionColorScale(alpha, p.t);
        out.push_back(p);

        partFront = bz;
        uPrev     = u;
    }

    RefPart tail;
    tail.zFront = partFront;
    tail.zBack  = zBack;
    tail.t      = 1.0 - uPrev;
    tail.alpha  = refPartitionAlpha(alpha, tail.t);
    tail.colorScale = refPartitionColorScale(alpha, tail.t);
    out.push_back(tail);
    return out;
}

// One expected SoA fragment.
struct RefFragment {
    int    x = 0;
    int    y = 0;
    double radius = 0.0;
    double depth  = 0.0;
    double alpha  = 0.0;
    int    index0 = 0;
    int    index1 = 0;
    double alpha0 = 0.0;
    double alpha1 = 0.0;
    double colorScale0 = 0.0;
    double colorScale1 = 0.0;
    bool   volumetric  = false;
    bool   coverageHead = true;
    std::vector<double> channels;
};

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
    return (v > 0.0f) ? static_cast<double>(DepthBuckets::kMaxDepth) : 0.0;
}

// The whole flatten, re-derived: sanitise -> tidy -> (span split | point) ->
// pre-merge -> deposit.  `tidyOverlapping()` itself is shared code with its own
// coverage (the parity gate, plus the termination fuzz at the bottom of this
// file), so the reference reuses it — which still catches the flatten DROPPING
// it, because then the two disagree.
std::vector<RefFragment> refFlatten(const CocParams& p,
                                    const DepthBuckets& b,
                                    int x, int y,
                                    std::vector<SampleRecord> samples,
                                    bool preMerge,
                                    double mergeTolerancePx,
                                    int channelCount)
{
    // --- 1. sanitise -------------------------------------------------------
    for (SampleRecord& s : samples) {
        const double zf = refSanitizeDepth(s.zFront);
        double       zb = refSanitizeDepth(s.zBack);
        if (!(zb > zf))
            zb = zf;
        s.zFront = static_cast<float>(zf);
        s.zBack  = static_cast<float>(zb);
        s.alpha  = (s.alpha > 0.0f) ? ((s.alpha < 1.0f) ? s.alpha : 1.0f) : 0.0f;
        s.channels.resize(static_cast<std::size_t>(channelCount), 0.0f);
    }

    // --- 2. tidy, then front-to-back ---------------------------------------
    if (samples.size() > 1)
        tidyOverlapping(samples);
    std::sort(samples.begin(), samples.end(),
        [](const SampleRecord& a, const SampleRecord& c) {
            return (a.zFront != c.zFront) ? a.zFront < c.zFront : a.zBack < c.zBack;
        });

    // --- 3. stage ----------------------------------------------------------
    struct Staged {
        double zFront = 0.0, zBack = 0.0, alpha = 0.0, depth = 0.0, radius = 0.0;
        int    bucket = 0;
        bool   volumetric = false;
        bool   coverageHead = true;
        std::vector<double> channels;
    };
    std::vector<Staged> staged;

    for (const SampleRecord& s : samples) {
        if (!(s.alpha > 0.0f))
            continue;

        const bool volumetric = (s.zBack > s.zFront);

        if (!volumetric) {
            Staged st;
            st.zFront = s.zFront;
            st.zBack  = s.zBack;
            st.alpha  = s.alpha;
            st.volumetric = false;
            st.coverageHead = true;
            st.channels.assign(s.channels.begin(), s.channels.end());
            st.depth  = refMidDepth(st.zFront, st.zBack);
            st.radius = refRadiusPx(p, st.depth);
            st.bucket = refContainingBucket(b, st.depth);
            staged.push_back(st);
            continue;
        }

        const std::vector<RefPart> parts = refSplitSpan(b, s.zFront, s.zBack, s.alpha);
        bool haveParentPart = false;
        for (const RefPart& part : parts) {
            if (!(part.t > 0.0))
                continue;

            const double partDepth  = refMidDepth(part.zFront, part.zBack);
            const int    partBucket = refContainingBucket(b, partDepth);

            // Consecutive parts of ONE parent that land in the same containing
            // bucket are over-composited here, so a parent's parts always
            // occupy distinct buckets (see the .cpp's out-of-range note).
            if (haveParentPart && staged.back().bucket == partBucket) {
                Staged& prev = staged.back();
                const double w = 1.0 - prev.alpha;
                for (int c = 0; c < channelCount; ++c)
                    prev.channels[static_cast<std::size_t>(c)] +=
                        static_cast<double>(s.channels[static_cast<std::size_t>(c)])
                        * part.colorScale * w;
                prev.alpha += part.alpha * w;
                prev.zBack  = part.zBack;
                prev.depth  = refMidDepth(prev.zFront, prev.zBack);
                prev.radius = refRadiusPx(p, prev.depth);
                prev.bucket = refContainingBucket(b, prev.depth);
                continue;
            }

            Staged st;
            st.zFront = part.zFront;
            st.zBack  = part.zBack;
            st.alpha  = part.alpha;
            st.volumetric = true;
            st.coverageHead = !haveParentPart;   // the FRONT-MOST emitted part
            st.channels.resize(static_cast<std::size_t>(channelCount));
            for (int c = 0; c < channelCount; ++c)
                st.channels[static_cast<std::size_t>(c)] =
                    static_cast<double>(s.channels[static_cast<std::size_t>(c)]) * part.colorScale;
            st.depth  = partDepth;
            st.radius = refRadiusPx(p, st.depth);
            st.bucket = partBucket;
            staged.push_back(st);
            haveParentPart = true;
        }
    }

    // --- 4/5. pre-merge, then deposit --------------------------------------
    std::vector<RefFragment> out;
    const bool merging = preMerge && (mergeTolerancePx > 0.0);

    std::size_t i = 0;
    while (i < staged.size()) {
        const Staged& head = staged[i];
        bool        groupHead = head.coverageHead;
        std::size_t j = i + 1;
        if (merging) {
            while (j < staged.size()) {
                const Staged& cand = staged[j];
                if (cand.volumetric != head.volumetric || cand.bucket != head.bucket)
                    break;
                if (!(std::fabs(cand.radius - head.radius) <= mergeTolerancePx))
                    break;
                groupHead = groupHead || cand.coverageHead;
                ++j;
            }
        }

        double zf = head.zFront;
        double zb = head.zBack;
        double alphaAcc = 0.0;
        std::vector<double> accum(static_cast<std::size_t>(channelCount), 0.0);

        for (std::size_t s = i; s < j; ++s) {
            const double w = 1.0 - alphaAcc;
            if (w <= 0.0)
                break;
            zf = std::min(zf, staged[s].zFront);
            zb = std::max(zb, staged[s].zBack);
            for (int c = 0; c < channelCount; ++c)
                accum[static_cast<std::size_t>(c)] +=
                    staged[s].channels[static_cast<std::size_t>(c)] * w;
            alphaAcc += staged[s].alpha * w;
        }

        RefFragment f;
        f.x = x;
        f.y = y;
        f.depth  = refMidDepth(zf, zb);
        f.radius = refRadiusPx(p, f.depth);
        f.alpha  = std::min(std::max(alphaAcc, 0.0), 1.0);
        f.volumetric   = head.volumetric;
        f.coverageHead = groupHead;
        f.channels = accum;

        // THE COMPOSITION CONTRACT: whole weight for a span-split piece, the
        // fractional two-bucket partition for a point sample — never both.
        RefWeight w;
        if (f.volumetric) {
            w.index = refContainingBucket(b, f.depth);
            w.frac  = 0.0;
        } else {
            w = refBucketOf(b, f.depth);
        }
        f.index0 = w.index;
        f.index1 = (w.frac > 0.0) ? (w.index + 1) : w.index;
        f.alpha0 = refPartitionAlpha(f.alpha, 1.0 - w.frac);
        f.alpha1 = refPartitionAlpha(f.alpha, w.frac);
        f.colorScale0 = refPartitionColorScale(f.alpha, 1.0 - w.frac);
        f.colorScale1 = refPartitionColorScale(f.alpha, w.frac);

        out.push_back(f);
        i = j;
    }

    return out;
}

// ===========================================================================
// Band plumbing
// ===========================================================================

struct Band {
    int K = 0, C = 0, W = 0, H = 0;
    BucketPlanes planes;
    std::vector<float> color;   // C planes of W*H, band-relative row-major
    std::vector<float> alpha;   // W*H

    std::ptrdiff_t pixels() const { return static_cast<std::ptrdiff_t>(W) * H; }

    float outAlpha(int x, int y) const
    { return alpha[static_cast<std::size_t>(y) * W + x]; }

    float outColor(int c, int x, int y) const
    { return color[static_cast<std::size_t>(c) * pixels() + static_cast<std::size_t>(y) * W + x]; }

    float planeAlpha(int k, int x, int y) const
    { return planes.alpha[static_cast<std::size_t>(k) * pixels() + static_cast<std::size_t>(y) * W + x]; }
};

// scatterBandCPU() ON A std::thread — the design reference's "thread-agnostic
// so unit tests can drive it directly with std::thread", exercised literally.
void scatterOnThread(const ScatterParams& sp, const SampleSoA& soa,
                     const HoldoutSoA& holdout, const KernelSampler& kernel,
                     BucketPlanes& planes)
{
    ScatterScratch scratch;
    std::thread worker([&] {
        scatterBandCPU(sp, soa, holdout, kernel, planes, scratch);
    });
    worker.join();
}

// Full band: allocate, zero, scatter (threaded unless told otherwise), resolve.
void runBand(Band& band, const ScatterParams& sp, const SampleSoA& soa,
             const HoldoutSoA& holdout, const KernelSampler& kernel,
             bool useThread = true)
{
    band.planes.allocate(band.K, band.C, band.W, band.H);
    band.planes.zero();

    if (useThread) {
        scatterOnThread(sp, soa, holdout, kernel, band.planes);
    } else {
        ScatterScratch scratch;
        scatterBandCPU(sp, soa, holdout, kernel, band.planes, scratch);
    }

    // Poisoned, so a composite that fails to overwrite is visible rather than
    // reading as a zero it never wrote.
    band.color.assign(static_cast<std::size_t>(band.C) * band.pixels(), -777.0f);
    band.alpha.assign(static_cast<std::size_t>(band.pixels()), -777.0f);
    resolveBandCPU(sp, band.planes, band.color.data(), band.alpha.data());
}

// The band-integrated alpha / premultiplied colour.  For an isolated fragment
// whose disc lies wholly inside the band these are the quantities that must
// reconstruct the fragment (kernel weights sum to 1), which is the identity
// almost every scatter case below is built on.
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

double planeSum(const PodBuffer<float>& p, int k, std::ptrdiff_t pixelCount)
{
    double s = 0.0;
    for (std::ptrdiff_t i = 0; i < pixelCount; ++i)
        s += static_cast<double>(p[static_cast<std::size_t>(k * pixelCount + i)]);
    return s;
}

// Flatten one hand-built pixel into a fresh SoA.
SampleSoA flattenOnePixel(const FlattenParams& fp, const DepthBuckets& b,
                          int x, int y, std::vector<SampleRecord> samples)
{
    SampleSoA soa;
    soa.begin(fp.channelCount, fp.groups);
    FlattenScratch scratch;
    flattenPixelToSoA(fp, b, x, y, samples, scratch, soa, nullptr);
    return soa;
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

ScatterParams makeScatterParams(int w, int h, BucketCombine combine,
                                HoldoutInterp interp = HoldoutInterp::LogChord)
{
    ScatterParams sp;
    sp.bandX = 0;
    sp.bandY = 0;
    sp.bandWidth = w;
    sp.bandHeight = h;
    sp.sharpRadiusPx = kSharpRadiusPx;
    // EXPLICIT IN EVERY CASE, never the provisional default (Decisions,
    // 2026-07-26: the default is non-authoritative until M1.P3.T5 decides).
    sp.combine = combine;
    sp.holdoutInterp = interp;
    return sp;
}

// ===========================================================================
// Independent rasteriser — the expected bucket planes for a fragment stream
//
// Derived from the documented deposit rules, NOT from the shipped drivers:
//
//   colour   : color    [(k * channelCount + c) * pixelCount + i] += w*vis * col[c]*scale
//   alpha    : alpha    [k * pixelCount + i]                      += w*vis * bucketAlpha
//   new area : weight   [k * pixelCount + i]                      += w*vis
//              (fragment's NEARER bucket only, and only if it is its parent's
//               coverage head)
//   colocated: colocated[k * pixelCount + i]                      += w*vis
//              (every other deposit that carries alpha)
//
// The kernel geometry comes from the KernelView seam (radiusY rows, row y =
// row - radiusY, contiguous weights over [xStart, xEnd]); the holdout factor
// from HoldoutVisibility::interp(), which locates the bracket BY DEPTH with a
// binary search — a different code path from the scatter's O(1) closed-form
// HoldoutBoundaries::locate(), so agreement is a real check on the scatter's
// index derivation and on which pixel's LUT row it reads.
// ===========================================================================
struct ExpectedPlanes {
    int K = 0, C = 0, W = 0, H = 0;
    std::vector<double> color, alpha, weight, colocated;

    void allocate(int k, int c, int w, int h)
    {
        K = k; C = c; W = w; H = h;
        const std::size_t px = static_cast<std::size_t>(w) * h;
        color.assign(static_cast<std::size_t>(k) * c * px, 0.0);
        alpha.assign(static_cast<std::size_t>(k) * px, 0.0);
        weight.assign(static_cast<std::size_t>(k) * px, 0.0);
        colocated.assign(static_cast<std::size_t>(k) * px, 0.0);
    }
};

void refRasterize(ExpectedPlanes& out, const ScatterParams& sp, const SampleSoA& soa,
                  const DiscKernelLUT& lut, const HoldoutSoA* holdout)
{
    const std::ptrdiff_t px = static_cast<std::ptrdiff_t>(out.W) * out.H;

    for (std::size_t f = 0; f < soa.fragmentCount(); ++f) {
        const int    b0 = static_cast<int>(soa.bucketIndex0[f]);
        int          b1 = static_cast<int>(soa.bucketIndex1[f]);
        const double a0 = soa.bucketAlpha0[f];
        const double a1 = soa.bucketAlpha1[f];
        const double s0 = soa.colorScale0[f];
        const double s1 = soa.colorScale1[f];
        const bool   head = fragmentCoverageHeadOf(soa.flags[f]);
        const float  radius = soa.radius[f];
        const float  depth  = soa.depth[f];
        const float* col = soa.colorOf(f);

        if (b0 < 0 || b0 >= out.K)
            continue;
        if (b1 < 0 || b1 >= out.K)
            b1 = b0;
        if (a0 == 0.0 && a1 == 0.0 && s0 == 0.0 && s1 == 0.0)
            continue;

        const int destX = static_cast<int>(soa.x[f]) - sp.bandX;
        const int destY = static_cast<int>(soa.y[f]) - sp.bandY;

        // (pixel, weight) list for this fragment's whole footprint.
        std::vector<std::pair<std::ptrdiff_t, double>> touched;

        if (!(radius >= sp.sharpRadiusPx)) {
            // Sharp fast path: weight 1 into the fragment's own pixel.
            if (destX >= 0 && destX < out.W && destY >= 0 && destY < out.H)
                touched.emplace_back(static_cast<std::ptrdiff_t>(destY) * out.W + destX, 1.0);
        } else {
            const KernelView kv = lut.kernel(radius, destX, destY, depth, 0);
            REQUIRE(kv.valid());
            for (int row = 0; row < kv.rowCount; ++row) {
                const RowSpan& span = kv.row(row);
                if (span.empty())
                    continue;
                const int dy = destY + kv.rowY(row);
                if (dy < 0 || dy >= out.H)
                    continue;
                const float* w = kv.rowWeights(row);
                for (int i = 0; i < span.count(); ++i) {
                    const int dx = destX + span.xStart + i;
                    if (dx < 0 || dx >= out.W)
                        continue;
                    touched.emplace_back(static_cast<std::ptrdiff_t>(dy) * out.W + dx,
                                          static_cast<double>(w[i]));
                }
            }
        }

        for (const auto& tp : touched) {
            const std::ptrdiff_t dst = tp.first;
            double w = tp.second;

            if (holdout != nullptr && holdout->enabled()) {
                w *= static_cast<double>(HoldoutVisibility::interp(
                        holdout->boundaries.boundaries(),
                        holdout->pixelLut(dst),
                        holdout->boundaryCount(),
                        depth,
                        sp.holdoutInterp));
            }

            out.alpha[static_cast<std::size_t>(b0 * px + dst)] += w * a0;
            if (head)
                out.weight[static_cast<std::size_t>(b0 * px + dst)] += w;
            else
                out.colocated[static_cast<std::size_t>(b0 * px + dst)] += w;
            for (int c = 0; c < out.C; ++c)
                out.color[static_cast<std::size_t>((b0 * out.C + c) * px + dst)] +=
                    w * static_cast<double>(col[c]) * s0;

            if (b1 != b0) {
                out.alpha[static_cast<std::size_t>(b1 * px + dst)] += w * a1;
                out.colocated[static_cast<std::size_t>(b1 * px + dst)] += w;
                for (int c = 0; c < out.C; ++c)
                    out.color[static_cast<std::size_t>((b1 * out.C + c) * px + dst)] +=
                        w * static_cast<double>(col[c]) * s1;
            }
        }
    }
}

// Compare the shipped planes against the reference, entry for entry.  Both
// accumulate in the same order, so the residual is float rounding only: the
// worst |got - want| MEASURED over every case in this file is 1.42e-08
// absolute, so the 2e-06 below is ~140x headroom -- and it still catches any
// index, stride, sign, off-by-one or wrong-plane error, all of which move a
// value by its whole magnitude rather than by an ulp.
void checkPlanes(const BucketPlanes& got, const ExpectedPlanes& want)
{
    REQUIRE(got.bucketCount == want.K);
    REQUIRE(got.channelCount == want.C);
    REQUIRE(got.pixelCount == static_cast<std::ptrdiff_t>(want.W) * want.H);

    const double tol = 2e-06;
    std::size_t badAlpha = 0, badWeight = 0, badColocated = 0, badColor = 0;

    for (std::size_t i = 0; i < want.alpha.size(); ++i) {
        if (std::fabs(static_cast<double>(got.alpha[i]) - want.alpha[i]) > tol) ++badAlpha;
        if (std::fabs(static_cast<double>(got.weight[i]) - want.weight[i]) > tol) ++badWeight;
        if (std::fabs(static_cast<double>(got.colocated[i]) - want.colocated[i]) > tol) ++badColocated;
    }
    for (std::size_t i = 0; i < want.color.size(); ++i)
        if (std::fabs(static_cast<double>(got.color[i]) - want.color[i]) > tol) ++badColor;

    CHECK(badAlpha == 0);
    CHECK(badWeight == 0);
    CHECK(badColocated == 0);
    CHECK(badColor == 0);
}

// ===========================================================================
// Holdout plumbing
// ===========================================================================

// Build a band's holdout LUT from a per-pixel sample supplier.  The supplier
// must fill `out` for EVERY band pixel in row-major order — appendPixel()'s
// documented CSR contract.
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
// SoA flatten (M1.P3.T1)
// ===========================================================================

TEST_CASE("flattenPixelToSoA reproduces an independent tidy + split + merge reference")
{
    const CocParams    p  = makeStandardRig(10.0f);
    const DepthBuckets bk = makeStandardBuckets(p);
    const int          C  = 3;

    struct Fixture {
        const char* name;
        std::vector<SampleRecord> samples;
    };

    // Hand-built fixtures, each aimed at a different limb of the pipeline.
    std::vector<Fixture> fixtures;
    fixtures.push_back({"one point sample, mid-range",
        {makeSample(3.0f, 3.0f, 0.7f, {0.7f * 0.2f, 0.7f * 0.5f, 0.7f * 0.9f})}});
    fixtures.push_back({"one point sample exactly on a bucket centre",
        {makeSample(bk.centre(6), bk.centre(6), 0.4f, {0.1f, 0.2f, 0.3f})}});
    fixtures.push_back({"two DISJOINT point samples, different buckets",
        {makeSample(2.0f, 2.0f, 0.5f, {0.1f, 0.2f, 0.3f}),
         makeSample(6.0f, 6.0f, 0.25f, {0.05f, 0.1f, 0.15f})}});
    fixtures.push_back({"two COINCIDENT point samples (tidy must over-composite them)",
        {makeSample(9.0f, 9.0f, 0.3f, {0.3f * 0.4f, 0.3f * 0.5f, 0.3f * 0.6f}),
         makeSample(9.0f, 9.0f, 0.4f, {0.4f * 0.1f, 0.4f * 0.2f, 0.4f * 0.3f})}});
    fixtures.push_back({"volumetric span across four buckets",
        {makeSample(bk.boundary(8), bk.boundary(12), 0.9f, {0.9f * 0.2f, 0.9f * 0.4f, 0.9f * 0.8f})}});
    fixtures.push_back({"volumetric span reaching outside the measured range",
        {makeSample(0.2f, 400.0f, 0.6f, {0.6f * 0.3f, 0.6f * 0.3f, 0.6f * 0.3f})}});
    fixtures.push_back({"volumetric span inside ONE bucket (no split)",
        {makeSample(2.55f, 2.65f, 0.35f, {0.05f, 0.06f, 0.07f})}});
    fixtures.push_back({"point plus volumetric at the same pixel",
        {makeSample(1.5f, 1.5f, 0.8f, {0.2f, 0.3f, 0.4f}),
         makeSample(4.0f, 7.0f, 0.45f, {0.1f, 0.1f, 0.1f})}});
    fixtures.push_back({"OVERLAPPING volumetric spans (tidy splits and mixes them)",
        {makeSample(2.0f, 6.0f, 0.5f, {0.2f, 0.2f, 0.2f}),
         makeSample(4.0f, 8.0f, 0.35f, {0.1f, 0.15f, 0.2f})}});
    // Straddling a boundary at nearly equal depth: the radii differ by 0.08px,
    // WELL inside the 0.25px merge tolerance, so only the "same containing
    // bucket" half of the grouping predicate keeps these two apart.  A merge
    // across the boundary would move energy into a different plane.
    fixtures.push_back({"two point samples straddling a bucket boundary, radii within tolerance",
        {makeSample(bk.boundary(10) - 0.01f, bk.boundary(10) - 0.01f, 0.5f, {0.1f, 0.2f, 0.3f}),
         makeSample(bk.boundary(10) + 0.01f, bk.boundary(10) + 0.01f, 0.45f, {0.09f, 0.18f, 0.27f})}});
    fixtures.push_back({"three near-identical sharp samples (pre-merge groups them)",
        {makeSample(9.0f, 9.0f, 0.2f, {0.02f, 0.04f, 0.06f}),
         makeSample(9.001f, 9.001f, 0.3f, {0.03f, 0.06f, 0.09f}),
         makeSample(9.002f, 9.002f, 0.25f, {0.025f, 0.05f, 0.075f})}});

    for (bool preMerge : {false, true}) {
        for (const Fixture& fx : fixtures) {
            CAPTURE(fx.name);
            CAPTURE(preMerge);

            const FlattenParams fp = makeFlattenParams(p, C, preMerge);
            const SampleSoA soa = flattenOnePixel(fp, bk, 11, 23, fx.samples);
            const std::vector<RefFragment> want =
                refFlatten(p, bk, 11, 23, fx.samples, preMerge, fp.mergeTolerancePx, C);

            REQUIRE(soa.fragmentCount() == want.size());

            for (std::size_t i = 0; i < want.size(); ++i) {
                CAPTURE(i);
                const RefFragment& w = want[i];

                // Exact: integers and labels have no rounding to hide behind.
                CHECK(soa.x[i] == w.x);
                CHECK(soa.y[i] == w.y);
                CHECK(static_cast<int>(soa.bucketIndex0[i]) == w.index0);
                CHECK(static_cast<int>(soa.bucketIndex1[i]) == w.index1);
                CHECK((fragmentKindOf(soa.flags[i]) == FragmentKind::Volumetric) == w.volumetric);
                CHECK(fragmentCoverageHeadOf(soa.flags[i]) == w.coverageHead);

                // 2e-6 absolute: the reference runs std::pow/double throughout
                // and the shipped path expm1/log1p/float, so the two agree only
                // to float resolution.  MEASURED worst difference over these
                // fixtures is 7.41e-08 on the alphas and colour scales and
                // 2.05e-07 relative on the radius, so these are ~27x and ~100x
                // headroom — while any wrong bucket, wrong split or dropped
                // tidy moves them by 1e-2 or more.
                CHECK(std::fabs(static_cast<double>(soa.alpha[i]) - w.alpha) <= 2e-6);
                CHECK(std::fabs(static_cast<double>(soa.bucketAlpha0[i]) - w.alpha0) <= 2e-6);
                CHECK(std::fabs(static_cast<double>(soa.bucketAlpha1[i]) - w.alpha1) <= 2e-6);
                CHECK(std::fabs(static_cast<double>(soa.colorScale0[i]) - w.colorScale0) <= 2e-6);
                CHECK(std::fabs(static_cast<double>(soa.colorScale1[i]) - w.colorScale1) <= 2e-6);
                CHECK(std::fabs(static_cast<double>(soa.depth[i]) - w.depth) <= 2e-6 * (1.0 + std::fabs(w.depth)));
                CHECK(std::fabs(static_cast<double>(soa.radius[i]) - w.radius) <= 2e-5 * (1.0 + w.radius));

                const float* got = soa.colorOf(i);
                for (int c = 0; c < C; ++c)
                    CHECK(std::fabs(static_cast<double>(got[c]) - w.channels[static_cast<std::size_t>(c)]) <= 2e-6);
            }

            // The audit the node ships must accept everything the flatten emits.
            std::size_t bad = 0;
            CHECK(checkCompositionContract(soa, bk, &bad));
        }
    }
}

TEST_CASE("the tidy pre-pass is correctness-required: coincident samples over-composite, "
          "and the sharp path reproduces a sequential `over` exactly")
{
    // "tidy + sharp-path = sequential over", carried from M1.P1.T4.  Two
    // coincident point samples at alpha 0.3 and 0.4 are ONE surface pair, so
    // the answer is the sequential over 0.3 + 0.4*0.7 = 0.58 -- not the 0.7
    // the scatter's additive within-bucket accumulation would give.
    const CocParams    p  = makeStandardRig(10.0f);
    const DepthBuckets bk = makeStandardBuckets(p);
    const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ false);

    // depth 9.0 is 0.266px of CoC on this rig, i.e. the SHARP fast path.
    REQUIRE(radiusPixels(p, 9.0f) < kSharpRadiusPx);

    const SampleSoA soa = flattenOnePixel(fp, bk, 16, 16,
        {makeSample(9.0f, 9.0f, 0.3f, {0.3f * 0.8f}),
         makeSample(9.0f, 9.0f, 0.4f, {0.4f * 0.8f})});

    // Tidy collapsed the coincident pair into ONE sample before bucketing.
    REQUIRE(soa.fragmentCount() == 1);
    const double expectedAlpha = 0.3 + 0.4 * (1.0 - 0.3);          // 0.58, exact
    const double expectedColor = 0.3 * 0.8 + 0.4 * 0.8 * (1.0 - 0.3);
    CHECK(std::fabs(static_cast<double>(soa.alpha[0]) - expectedAlpha) <= 1e-6);

    const int W = 32, H = 32;
    DiscKernelLUT lut(0.0f, 4.0f, 1.0f, 1.0f);
    for (BucketCombine combine : {BucketCombine::FrontToBackOver,
                                  BucketCombine::CoveragePartition}) {
        Band band;
        band.K = bk.bucketCount(); band.C = 1; band.W = W; band.H = H;
        HoldoutSoA noHoldout;
        runBand(band, makeScatterParams(W, H, combine), soa, noHoldout, lut);

        // A sharp fragment deposits weight 1 into its own pixel, so the band
        // integral IS that pixel and it must be the sequential `over`.
        CHECK(std::fabs(bandAlphaSum(band) - expectedAlpha) <= 1e-6);
        CHECK(std::fabs(bandColorSum(band, 0) - expectedColor) <= 1e-6);
        CHECK(std::fabs(static_cast<double>(band.outAlpha(16, 16)) - expectedAlpha) <= 1e-6);
    }
}

TEST_CASE("flatten sanitisation: non-finite depths, inverted spans and zero-alpha samples")
{
    const CocParams    p  = makeStandardRig(10.0f);
    const DepthBuckets bk = makeStandardBuckets(p);
    const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ false);

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    SUBCASE("alpha-0 samples are dropped outright (DeepToImage parity)")
    {
        const SampleSoA soa = flattenOnePixel(fp, bk, 0, 0,
            {makeSample(3.0f, 3.0f, 0.0f, {0.9f}),
             makeSample(4.0f, 4.0f, 0.5f, {0.5f})});
        REQUIRE(soa.fragmentCount() == 1);
        CHECK(soa.alpha[0] == doctest::Approx(0.5f));
    }

    SUBCASE("a NaN depth becomes 0 (sharp, first bucket) rather than poisoning the sort")
    {
        const SampleSoA soa = flattenOnePixel(fp, bk, 0, 0,
            {makeSample(nan, nan, 0.5f, {0.5f})});
        REQUIRE(soa.fragmentCount() == 1);
        CHECK(soa.depth[0] == 0.0f);
        CHECK(soa.radius[0] == 0.0f);          // d <= 0 -> radius 0
        CHECK(soa.bucketIndex0[0] == 0);
        CHECK(std::isfinite(soa.alpha[0]));
    }

    SUBCASE("+inf becomes the far-field limit, and stays finite through the split")
    {
        const SampleSoA soa = flattenOnePixel(fp, bk, 0, 0,
            {makeSample(inf, inf, 0.5f, {0.5f})});
        REQUIRE(soa.fragmentCount() == 1);
        CHECK(soa.depth[0] == DepthBuckets::kMaxDepth);
        CHECK(std::isfinite(soa.radius[0]));
        CHECK(soa.bucketIndex0[0] == bk.bucketCount() - 1);
    }

    SUBCASE("an inverted span (zBack < zFront) collapses to a point sample")
    {
        const SampleSoA soa = flattenOnePixel(fp, bk, 0, 0,
            {makeSample(5.0f, 2.0f, 0.5f, {0.5f})});
        REQUIRE(soa.fragmentCount() == 1);
        CHECK(fragmentKindOf(soa.flags[0]) == FragmentKind::Point);
        CHECK(soa.depth[0] == doctest::Approx(5.0f));
    }

    SUBCASE("a NaN alpha clamps to 0 and the sample is dropped")
    {
        const SampleSoA soa = flattenOnePixel(fp, bk, 0, 0,
            {makeSample(3.0f, 3.0f, nan, {0.5f})});
        CHECK(soa.fragmentCount() == 0);
    }
}

TEST_CASE("depthIsRayDistance applies the per-pixel ray-distance -> Z correction")
{
    // The correction M1.P3.T5's computeDepthRange() has to apply IDENTICALLY
    // (see the milestone: the two passes disagreeing puts every corner-pixel
    // sample below depthMin).  It is per PIXEL, always shrinks the depth, and
    // is the identity on the optical axis -- none of which was pinned before,
    // so dropping it entirely was invisible.
    const CocParams    p  = makeStandardRig(10.0f);
    const DepthBuckets bk = makeStandardBuckets(p);

    FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ false);
    fp.depthIsRayDistance = true;
    fp.formatHeightPx     = 1080.0f;

    // Independent derivation, in double, from the design reference's geometry:
    // the pixel's radial filmback offset r, then z = ray * f / sqrt(f^2 + r^2).
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
        const SampleSoA soa = flattenOnePixel(fp, bk, 1900, 1050,
            {makeSample(ray, ray, 0.5f, {0.25f})});
        REQUIRE(soa.fragmentCount() == 1);
        const double want = refZ(1900, 1050, ray);
        CHECK(want < 19.0);                                  // it really is a big correction
        CHECK(std::fabs(static_cast<double>(soa.depth[0]) - want) <= 2e-5 * want);
    }

    // On axis: the identity to within half a pixel of offset.
    {
        const SampleSoA soa = flattenOnePixel(fp, bk, 960, 540,
            {makeSample(ray, ray, 0.5f, {0.25f})});
        REQUIRE(soa.fragmentCount() == 1);
        CHECK(std::fabs(static_cast<double>(soa.depth[0]) - refZ(960, 540, ray)) <= 2e-5 * ray);
        CHECK(std::fabs(static_cast<double>(soa.depth[0]) - ray) <= 1e-4);
    }

    // Knob OFF at the same pixel: the raw depth, untouched.
    {
        FlattenParams off = fp;
        off.depthIsRayDistance = false;
        const SampleSoA soa = flattenOnePixel(off, bk, 1900, 1050,
            {makeSample(ray, ray, 0.5f, {0.25f})});
        REQUIRE(soa.fragmentCount() == 1);
        CHECK(soa.depth[0] == doctest::Approx(ray));
    }

    // Both ENDPOINTS of a span are scaled by the one factor, so the span stays
    // a span and the split still sees a monotone range.
    {
        const SampleSoA soa = flattenOnePixel(fp, bk, 1900, 1050,
            {makeSample(6.0f, 30.0f, 0.5f, {0.25f})});
        REQUIRE(soa.fragmentCount() >= 1);
        CHECK(static_cast<double>(soa.depth[0]) < refZ(1900, 1050, 30.0));
        CHECK(static_cast<double>(soa.depth[0]) > refZ(1900, 1050, 6.0));
    }
}

TEST_CASE("the pre-merge grouping predicate keeps a POINT and a SPAN apart even when "
          "they share a bucket and a radius")
{
    // Merging across FragmentKind would change which half of the composition
    // contract the merged fragment takes -- bucketOf()'s fractional two-bucket
    // partition instead of bucketOfContaining()'s whole weight, i.e. the +8.3%
    // double split.  The other two thirds of the predicate (same bucket, radius
    // within tolerance) are already covered by the reference fixtures above;
    // this is the one that was not.
    const CocParams    p  = makeStandardRig(10.0f);
    const DepthBuckets bk = makeStandardBuckets(p);

    // A point sample and a volumetric span, BOTH wholly inside bucket 10 and
    // adjacent front-to-back, at a 1.0px tolerance (a legal knob value).
    const float lo = bk.boundary(10), hi = bk.boundary(11);
    const float zPoint = lo + 0.20f * (hi - lo);
    const float zSpanF = lo + 0.45f * (hi - lo);
    const float zSpanB = lo + 0.75f * (hi - lo);

    const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ true, /*tolerance*/ 1.0f);
    const SampleSoA soa = flattenOnePixel(fp, bk, 5, 5,
        {makeSample(zPoint, zPoint, 0.5f, {0.25f}),
         makeSample(zSpanF, zSpanB, 0.4f, {0.2f})});

    // The predicate's OTHER two clauses both hold here, so only `kind` can be
    // keeping these two fragments apart.
    REQUIRE(soa.fragmentCount() == 2u);
    const int b0 = bk.bucketOfContaining(soa.depth[0]).index;
    const int b1 = bk.bucketOfContaining(soa.depth[1]).index;
    REQUIRE(b0 == b1);
    REQUIRE(std::fabs(soa.radius[0] - soa.radius[1]) <= 1.0f);

    CHECK(fragmentKindOf(soa.flags[0]) == FragmentKind::Point);
    CHECK(fragmentKindOf(soa.flags[1]) == FragmentKind::Volumetric);
    // And the contract each one took is the one its label asks for.
    CHECK(soa.bucketIndex1[1] == soa.bucketIndex0[1]);       // span: whole weight
    CHECK(checkCompositionContract(soa, bk));
}

TEST_CASE("channel counts: the flatten sizes its staging from the SoA, the scatter from "
          "min(SoA, planes) -- neither runs off the smaller of the two")
{
    // Both of these are safety nets against a caller whose FlattenParams and
    // SampleSoA::begin() (or whose SampleSoA and BucketPlanes) disagree; the
    // first was a heap-buffer-overflow under ASAN before it was added.  Nothing
    // pinned either of them.
    const CocParams    p  = makeStandardRig(10.0f);
    const DepthBuckets bk = makeStandardBuckets(p);

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
        std::vector<SampleRecord> v{makeSample(bk.boundary(6), bk.boundary(9), 0.9f, ch)};
        flattenPixelToSoA(fp, bk, 0, 0, v, scratch, soa, nullptr);
        REQUIRE(soa.fragmentCount() >= 1u);

        // Every channel the SoA declared carries its scaled value; the parts'
        // colour scales sum back to the parent under `over`, so the simplest
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
        const int K = 6, W = 12, H = 12;
        DiscKernelLUT lut(0.0f, 8.0f, 1.0f, 1.0f);

        SampleSoA soa;
        soa.begin(3, makeSingleChannelGroup(3));            // three channels...
        FragmentRecord f;
        f.x = W / 2; f.y = H / 2;
        f.radius = 0.0f;                                     // sharp: one pixel
        f.depth = 5.0f;
        f.alpha = 1.0f;
        BucketWeight bw;
        bw.index = 1;
        bw.frac  = 0.0f;
        f.deposit = fragmentDeposit(bw, 1.0f);
        f.kind = FragmentKind::Point;
        const float ch[3] = {0.3f, 0.5f, 0.7f};
        soa.appendFragment(f, ch);

        BucketPlanes planes;
        planes.allocate(K, /*channelCount*/ 1, W, H);        // ...ONE plane
        planes.zero();
        HoldoutSoA none;
        scatterOnThread(makeScatterParams(W, H, BucketCombine::CoveragePartition),
                        soa, none, lut, planes);

        // With one colour plane per bucket, a scatter that wrote three channels
        // would spill into buckets 2 and 3 -- which is a silent wrong-bucket
        // deposit, not merely an overflow.
        const std::ptrdiff_t px = static_cast<std::ptrdiff_t>(W) * H;
        CHECK(planeSum(planes.color, 1, px) == doctest::Approx(0.3f));
        for (int k = 0; k < K; ++k) {
            if (k == 1)
                continue;
            CAPTURE(k);
            CHECK(planeSum(planes.color, k, px) == 0.0);
        }
    }
}

TEST_CASE("the COMPOSITION CONTRACT is data: volumetric fragments carry no fractional spill, "
          "and checkCompositionContract rejects the branch error it can see")
{
    const CocParams    p  = makeStandardRig(10.0f);
    const DepthBuckets bk = makeStandardBuckets(p);
    const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ false);

    const SampleSoA soa = flattenOnePixel(fp, bk, 0, 0,
        {makeSample(bk.boundary(6), bk.boundary(11), 0.9f, {0.9f * 0.5f})});

    REQUIRE(soa.fragmentCount() == 5);
    for (std::size_t i = 0; i < soa.fragmentCount(); ++i) {
        CHECK(fragmentKindOf(soa.flags[i]) == FragmentKind::Volumetric);
        CHECK(soa.bucketIndex1[i] == soa.bucketIndex0[i]);
        CHECK(soa.bucketAlpha1[i] == 0.0f);
        CHECK(soa.colorScale1[i] == 0.0f);
    }
    // Parts of one parent land in DISTINCT buckets, front to back.
    for (std::size_t i = 1; i < soa.fragmentCount(); ++i)
        CHECK(soa.bucketIndex0[i] > soa.bucketIndex0[i - 1]);

    CHECK(checkCompositionContract(soa, bk));

    SUBCASE("a Volumetric fragment that ALSO took bucketOf() is rejected (the +8.3% double split)")
    {
        SampleSoA bad;
        bad.begin(1, makeSingleChannelGroup(1));
        const float depth = 0.5f * (bk.centre(5) + bk.centre(6));
        FragmentRecord f;
        f.depth  = depth;
        f.radius = radiusPixels(p, depth);
        f.alpha  = 0.9f;
        f.kind   = FragmentKind::Volumetric;             // labelled span-split...
        f.deposit = fragmentDeposit(bk.bucketOf(depth), f.alpha);  // ...but assigned by centres
        const float ch[1] = {0.45f};
        bad.appendFragment(f, ch);

        std::size_t firstBad = 999;
        CHECK_FALSE(checkCompositionContract(bad, bk, &firstBad));
        CHECK(firstBad == 0);
    }

    SUBCASE("an out-of-range bucket index is rejected")
    {
        SampleSoA bad;
        bad.begin(1, makeSingleChannelGroup(1));
        FragmentRecord f;
        f.depth = 3.0f;
        f.radius = 1.0f;
        f.alpha = 0.5f;
        f.kind = FragmentKind::Point;
        f.deposit = fragmentDeposit(bk.bucketOf(3.0f), 0.5f);
        f.deposit.index0 = bk.bucketCount();            // one past the last plane
        f.deposit.index1 = bk.bucketCount();
        const float ch[1] = {0.25f};
        bad.appendFragment(f, ch);
        CHECK_FALSE(checkCompositionContract(bad, bk));
    }
}

TEST_CASE("coverage head: exactly one per POST-TIDY parent, fuzzed over single- and "
          "multi-parent pixels")
{
    const CocParams    p  = makeStandardRig(10.0f);
    const DepthBuckets bk = makeStandardBuckets(p);
    Lcg rng(0xC0FFEEu);

    SUBCASE("single volumetric parent: exactly one head, whatever the span and pre-merge state")
    {
        for (bool preMerge : {false, true}) {
            const FlattenParams fp = makeFlattenParams(p, 1, preMerge);
            for (int trial = 0; trial < 4000; ++trial) {
                const float a  = rng.range(0.001f, 1.0f);
                const float z0 = rng.range(0.2f, 120.0f);
                const float z1 = z0 + rng.range(0.0f, 200.0f);

                const SampleSoA soa = flattenOnePixel(fp, bk, 3, 4,
                    {makeSample(z0, z1, a, {a * 0.5f})});
                if (soa.fragmentCount() == 0)
                    continue;

                int heads = 0;
                for (std::size_t i = 0; i < soa.fragmentCount(); ++i)
                    heads += fragmentCoverageHeadOf(soa.flags[i]) ? 1 : 0;
                REQUIRE(heads == 1);
                // The head is the FRONT-MOST part: the composite visits it first.
                REQUIRE(fragmentCoverageHeadOf(soa.flags[0]));
            }
        }
    }

    SUBCASE("depth-DISJOINT parents each keep their own head; a merged group never duplicates one")
    {
        for (bool preMerge : {false, true}) {
            const FlattenParams fp = makeFlattenParams(p, 1, preMerge);
            for (int trial = 0; trial < 2000; ++trial) {
                const int n = rng.intRange(1, 5);
                std::vector<SampleRecord> samples;
                float z = rng.range(1.0f, 5.0f);
                int   liveParents = 0;
                for (int s = 0; s < n; ++s) {
                    const float a = rng.range(0.02f, 1.0f);
                    const float thickness = (rng.unit() < 0.5f) ? 0.0f : rng.range(0.01f, 3.0f);
                    samples.push_back(makeSample(z, z + thickness, a, {a * 0.5f}));
                    z += thickness + rng.range(0.05f, 4.0f);      // strictly disjoint
                    ++liveParents;
                }

                const SampleSoA soa = flattenOnePixel(fp, bk, 3, 4, samples);
                int heads = 0;
                for (std::size_t i = 0; i < soa.fragmentCount(); ++i)
                    heads += fragmentCoverageHeadOf(soa.flags[i]) ? 1 : 0;

                // Never more than one head per parent -- an over-count here is
                // the K-times coverage inflation M1.P3.T8 fixed.  It can be
                // FEWER when pre-merge absorbs two heads into one deposit,
                // which is loss-free (same bucket, same radius, same area).
                REQUIRE(heads <= liveParents);
                REQUIRE(heads >= 1);
                if (!preMerge)
                    REQUIRE(heads == liveParents);
            }
        }
    }
}

// ===========================================================================
// The scatter core (M1.P3.T2 / T8 / T9)
// ===========================================================================

TEST_CASE("scatterBandCPU's deposits match an independent rasterisation, plane for plane")
{
    // The strongest structural check in this file: it re-derives WHICH plane,
    // WHICH bucket and WHICH pixel every deposit lands in from the documented
    // layout, so a wrong plane stride, a flipped row offset, an off-by-one span
    // or a coverage deposited twice is a direct mismatch rather than an
    // energy-sum coincidence.
    const CocParams    p  = makeStandardRig(10.0f);
    const DepthBuckets bk = makeStandardBuckets(p);
    const int W = 40, H = 28, C = 3;
    DiscKernelLUT lut(0.0f, 30.0f, 1.0f, 1.0f);

    SUBCASE("point, volumetric, sharp and band-edge-clipped fragments together")
    {
        const FlattenParams fp = makeFlattenParams(p, C, /*preMerge*/ true);
        SampleSoA soa;
        soa.begin(C, fp.groups);
        FlattenScratch scratch;

        struct Src { int x, y; float zf, zb, a; };
        const Src srcs[] = {
            { 20, 14, 3.0f,  3.0f,  0.8f },      // point, 5.6px disc, interior
            {  2,  3, 2.0f,  2.0f,  0.5f },      // point, 9.6px disc, clipped at two edges
            { 38, 25, 2.5f,  2.5f,  0.9f },      // point, clipped at the other two
            { 15,  8, 9.0f,  9.0f,  0.7f },      // SHARP (radius 0.27px)
            { 30, 20, 2.2f,  4.5f,  0.6f },      // volumetric, several buckets
            { -6, 12, 2.4f,  2.4f,  1.0f },      // wholly outside the band, scatters IN
            { 25, -4, 3.5f,  3.5f,  0.45f },     // ditto, from below
        };
        for (const Src& s : srcs) {
            std::vector<SampleRecord> v{makeSample(s.zf, s.zb, s.a,
                {s.a * 0.2f, s.a * 0.55f, s.a * 0.9f})};
            flattenPixelToSoA(fp, bk, s.x, s.y, v, scratch, soa, nullptr);
        }
        REQUIRE(soa.fragmentCount() >= 7);

        const ScatterParams sp = makeScatterParams(W, H, BucketCombine::CoveragePartition);
        Band band;
        band.K = bk.bucketCount(); band.C = C; band.W = W; band.H = H;
        band.planes.allocate(band.K, band.C, W, H);
        band.planes.zero();
        HoldoutSoA noHoldout;
        scatterOnThread(sp, soa, noHoldout, lut, band.planes);

        ExpectedPlanes want;
        want.allocate(band.K, C, W, H);
        refRasterize(want, sp, soa, lut, nullptr);
        checkPlanes(band.planes, want);
    }

    SUBCASE("a non-zero band origin is subtracted from the SoA's ABSOLUTE coordinates")
    {
        const FlattenParams fp = makeFlattenParams(p, C, /*preMerge*/ true);
        SampleSoA soa;
        soa.begin(C, fp.groups);
        FlattenScratch scratch;
        std::vector<SampleRecord> v{makeSample(3.0f, 3.0f, 0.8f, {0.16f, 0.44f, 0.72f})};
        flattenPixelToSoA(fp, bk, 120, 214, v, scratch, soa, nullptr);

        ScatterParams sp = makeScatterParams(W, H, BucketCombine::CoveragePartition);
        sp.bandX = 100;
        sp.bandY = 200;

        Band band;
        band.K = bk.bucketCount(); band.C = C; band.W = W; band.H = H;
        band.planes.allocate(band.K, band.C, W, H);
        band.planes.zero();
        HoldoutSoA noHoldout;
        scatterOnThread(sp, soa, noHoldout, lut, band.planes);

        ExpectedPlanes want;
        want.allocate(band.K, C, W, H);
        refRasterize(want, sp, soa, lut, nullptr);
        checkPlanes(band.planes, want);

        // And the disc really did land at (20, 14), not at (120, 214).  Its
        // interior weights are flat (only the rim is anti-aliased), so the
        // CENTROID is the meaningful locator, not the arg-max.
        double mass = 0.0, cx = 0.0, cy = 0.0;
        for (int k = 0; k < band.K; ++k)
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    const double a = band.planeAlpha(k, x, y);
                    mass += a;
                    cx   += a * x;
                    cy   += a * y;
                }
        REQUIRE(mass > 0.0);
        CHECK(cx / mass == doctest::Approx(20.0).epsilon(1e-3));
        CHECK(cy / mass == doctest::Approx(14.0).epsilon(1e-3));
    }

    SUBCASE("the planes are ACCUMULATED, so a band may be scattered from several SoA chunks")
    {
        const FlattenParams fp = makeFlattenParams(p, C, /*preMerge*/ true);
        SampleSoA whole, partA, partB;
        whole.begin(C, fp.groups);
        partA.begin(C, fp.groups);
        partB.begin(C, fp.groups);
        FlattenScratch scratch;

        for (int i = 0; i < 6; ++i) {
            const float a = 0.3f + 0.1f * i;
            std::vector<SampleRecord> v{makeSample(2.0f + 0.5f * i, 2.0f + 0.5f * i, a,
                                                    {a * 0.2f, a * 0.5f, a * 0.9f})};
            std::vector<SampleRecord> v2 = v;
            flattenPixelToSoA(fp, bk, 10 + 3 * i, 12, v, scratch, whole, nullptr);
            flattenPixelToSoA(fp, bk, 10 + 3 * i, 12, v2, scratch,
                              (i < 3) ? partA : partB, nullptr);
        }

        const ScatterParams sp = makeScatterParams(W, H, BucketCombine::CoveragePartition);
        HoldoutSoA noHoldout;

        BucketPlanes one;
        one.allocate(bk.bucketCount(), C, W, H);
        one.zero();
        scatterOnThread(sp, whole, noHoldout, lut, one);

        BucketPlanes two;
        two.allocate(bk.bucketCount(), C, W, H);
        two.zero();
        scatterOnThread(sp, partA, noHoldout, lut, two);
        scatterOnThread(sp, partB, noHoldout, lut, two);

        std::size_t differing = 0;
        for (std::size_t i = 0; i < one.alpha.size(); ++i) {
            if (one.alpha[i] != two.alpha[i]) ++differing;
            if (one.weight[i] != two.weight[i]) ++differing;
            if (one.colocated[i] != two.colocated[i]) ++differing;
        }
        for (std::size_t i = 0; i < one.color.size(); ++i)
            if (one.color[i] != two.color[i]) ++differing;
        CHECK(differing == 0);
    }
}

TEST_CASE("the deposit invariant: new area + co-located area == the fragments' own w*vis, "
          "which checkCompositionContract does NOT audit")
{
    // Every deposit that carries alpha writes its w*vis into EXACTLY ONE of the
    // two area planes.  Summed over both planes the total is therefore the
    // plain "deposit every part" coverage the node used to keep -- and that sum
    // is a closed form here: kernel weights are normalised to 1, so a fragment
    // whose disc lies wholly inside the band contributes 1 per deposit.
    const CocParams    p  = makeStandardRig(10.0f);
    const DepthBuckets bk = makeStandardBuckets(p);
    const int W = 120, H = 120;
    DiscKernelLUT lut(0.0f, 30.0f, 1.0f, 1.0f);
    const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ true);

    SampleSoA soa;
    soa.begin(1, fp.groups);
    FlattenScratch scratch;
    std::vector<SampleRecord> a{makeSample(3.0f, 3.0f, 0.8f, {0.4f})};     // point: 2 deposits
    std::vector<SampleRecord> b{makeSample(2.2f, 4.5f, 0.6f, {0.3f})};     // span: N parts
    flattenPixelToSoA(fp, bk, 60, 60, a, scratch, soa, nullptr);
    flattenPixelToSoA(fp, bk, 58, 62, b, scratch, soa, nullptr);

    // Independently: one unit of area per deposit that carries alpha.
    double expectedArea = 0.0;
    int    expectedHeads = 0;
    for (std::size_t i = 0; i < soa.fragmentCount(); ++i) {
        expectedArea += 1.0;                                        // deposit 0
        if (soa.bucketIndex1[i] != soa.bucketIndex0[i])
            expectedArea += 1.0;                                    // deposit 1
        if (fragmentCoverageHeadOf(soa.flags[i]))
            ++expectedHeads;
    }

    const ScatterParams sp = makeScatterParams(W, H, BucketCombine::CoveragePartition);
    Band band;
    band.K = bk.bucketCount(); band.C = 1; band.W = W; band.H = H;
    band.planes.allocate(band.K, 1, W, H);
    band.planes.zero();
    HoldoutSoA noHoldout;
    scatterOnThread(sp, soa, noHoldout, lut, band.planes);

    double newArea = 0.0, colocated = 0.0;
    for (int k = 0; k < band.K; ++k) {
        newArea   += planeSum(band.planes.weight, k, band.pixels());
        colocated += planeSum(band.planes.colocated, k, band.pixels());
    }

    // 5e-6 absolute: the disc LUT's per-entry normalisation residual is ~5e-8
    // per entry (DiscKernelLUT's own documented bound) over the ~10 deposits
    // here, so this is ~10x headroom.
    CHECK(std::fabs(newArea - static_cast<double>(expectedHeads)) <= 5e-6);
    CHECK(std::fabs((newArea + colocated) - expectedArea) <= 5e-6);
    // The two planes are disjoint per deposit, so neither can hold the total.
    CHECK(colocated > 0.0);
    CHECK(newArea < expectedArea - 0.5);
}

TEST_CASE("single-fragment energy identity over 3000 random (alpha, split fraction, radius): "
          "CoveragePartition conserves, FrontToBackOver inflates")
{
    // The mutation-resistant gate for the whole coverage-plane defect class.
    // One fragment, fractionally split across two bucket centres, at any kernel
    // radius: the band's alpha and premultiplied-colour integrals must be the
    // fragment's own, because the disc weights sum to 1.
    // Band and bucket count kept as small as the fixture allows (a 20px disc
    // plus its AA rim spans 43px, so a 64px band contains it whole, which is
    // what makes "the weights sum to 1" the closed form this case asserts
    // against): the corpus is 6000 full scatter+resolve passes and the resolve
    // is O(K * C * pixels).
    const int K = 4, C = 2, W = 64, H = 64;
    DiscKernelLUT lut(0.0f, 20.0f, 1.0f, 1.0f);
    const float unpremult[2] = {0.25f, 0.9f};

    double worstPartitionAlpha = 0.0, worstPartitionColor = 0.0;
    double worstOverAlpha = 0.0;

    std::thread worker([&] {
        for (int candidate = 0; candidate < 2; ++candidate) {
            const BucketCombine combine = (candidate == 0) ? BucketCombine::CoveragePartition
                                                            : BucketCombine::FrontToBackOver;
            Lcg rng(0x5EED1234u);       // same corpus for both candidates
            for (int trial = 0; trial < 3000; ++trial) {
                const float alpha  = rng.range(0.01f, 1.0f);
                const float frac   = rng.unit();
                const float radius = rng.range(0.0f, 20.0f);
                const int   index  = rng.intRange(0, K - 2);

                SampleSoA soa;
                soa.begin(C, makeSingleChannelGroup(C));
                FragmentRecord f;
                f.x = W / 2;
                f.y = H / 2;
                f.radius = radius;
                f.depth  = 1.0f;
                f.alpha  = alpha;
                BucketWeight bw;
                bw.index = index;
                bw.frac  = frac;
                f.deposit = fragmentDeposit(bw, alpha);
                f.kind = FragmentKind::Point;
                f.coverageHead = true;
                const float ch[2] = {alpha * unpremult[0], alpha * unpremult[1]};
                soa.appendFragment(f, ch);

                Band band;
                band.K = K; band.C = C; band.W = W; band.H = H;
                HoldoutSoA noHoldout;
                runBand(band, makeScatterParams(W, H, combine), soa, noHoldout, lut,
                        /*useThread*/ false);

                const double relAlpha = std::fabs(bandAlphaSum(band) - alpha) / alpha;
                const double relColor =
                    std::fabs(bandColorSum(band, 1) - alpha * unpremult[1]) / (alpha * unpremult[1]);

                if (candidate == 0) {
                    worstPartitionAlpha = std::max(worstPartitionAlpha, relAlpha);
                    worstPartitionColor = std::max(worstPartitionColor, relColor);
                } else {
                    worstOverAlpha = std::max(worstOverAlpha, relAlpha);
                }
            }
        }
    });
    worker.join();

    // MEASURED at this task: 2.18e-07 alpha / 2.67e-07 colour over this corpus
    // (Decisions log records 1.37e-07 / 1.24e-07 on the reviewer's own corpus).
    // 3e-06 is ~13x headroom and still two orders of magnitude below any
    // structural error -- the smallest one this file pins is +1.9% (M1.P3.T8's
    // mislabel at alpha 0.1).
    CHECK(worstPartitionAlpha <= 3e-06);
    CHECK(worstPartitionColor <= 3e-06);

    // PINNED, and it is a FAILURE of the other candidate, not a tolerance:
    // plain `over` inflates an isolated split fragment by up to +91.55% on this
    // corpus (Decisions: +93.8% on the reviewer's).  Asserted as a BAND, not a
    // floor, so the case cannot silently stop discriminating the two
    // candidates in either direction.
    CHECK(worstOverAlpha > 0.85);
    CHECK(worstOverAlpha < 1.00);
}

TEST_CASE("flat field identities: opaque field is alpha 1 to 1e-6 (NOT exactly 1), "
          "50% fog is 0.5, and the colour:alpha ratio is the input's")
{
    // Validation scene (c) at POD level.  |alpha - 1| <= ~1e-6, NOT equality:
    // the disc LUT's per-entry normalisation residual is ~5e-8 over ~113
    // contributing fragments (Decisions, 2026-07-26 -- the earlier "exactly 1"
    // reading was an artifact of an over-count being clamped).  The assertion
    // below is 2e-06, which is the milestone's stated bound; it was 1e-05 as
    // written, i.e. 12x above the measured 8e-07, and tightening it costs
    // nothing (M1.P3.T4 review).
    const CocParams    p  = makeStandardRig(10.0f);
    const DepthBuckets bk = makeStandardBuckets(p);
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
            for (int y = -pad; y < H + pad; ++y)
                for (int x = -pad; x < W + pad; ++x) {
                    std::vector<SampleRecord> v{makeSample(depth, depth, alpha,
                        {alpha * unpremult[0], alpha * unpremult[1], alpha * unpremult[2]})};
                    flattenPixelToSoA(fp, bk, x, y, v, scratch, soa, nullptr);
                }

            for (BucketCombine combine : {BucketCombine::FrontToBackOver,
                                          BucketCombine::CoveragePartition}) {
                CAPTURE(depth);
                CAPTURE(alpha);
                CAPTURE(static_cast<int>(combine));

                Band band;
                band.K = bk.bucketCount(); band.C = C; band.W = W; band.H = H;
                HoldoutSoA noHoldout;
                runBand(band, makeScatterParams(W, H, combine), soa, noHoldout, lut);

                const int cx = W / 2, cy = H / 2;
                const double a = band.outAlpha(cx, cy);
                CHECK(std::fabs(a - alpha) <= 2e-06);
                CHECK(a != doctest::Approx(0.0));

                // THE STANDING INVARIANT (Decisions, 2026-07-27, third
                // occurrence): premultiplied colour and alpha must move
                // together.  Unpremultiplying the output must give the input's
                // own colour, to the same 1e-5 the alpha holds to.
                for (int c = 0; c < C; ++c)
                    CHECK(std::fabs(band.outColor(c, cx, cy) / a - unpremult[c])
                          <= 1e-05 * unpremult[c] + 1e-06);
            }
        }
    }
}

TEST_CASE("flat opaque field ACROSS buckets: CoveragePartition holds alpha 1, "
          "FrontToBackOver shows its PINNED 25% deficit")
{
    // The flat-opaque-across-buckets identity carried from M1.P1.T4, and the
    // one configuration that actually discriminates the two candidates: a
    // checkerboard of two depths sitting EXACTLY on two bucket centres, so
    // every fragment's assignment is whole-weight (frac == 0) into one bucket
    // and each bucket receives half the disc weight.  Front-to-back `over`
    // then gives 1 - (1-0.5)^2 = 0.75 -- the documented 25.0%-across-2-buckets
    // alpha deficit -- while the coverage partition adds the two disjoint
    // half-coverages back to 1.
    const CocParams    p  = makeStandardRig(10.0f);
    const DepthBuckets bk = makeStandardBuckets(p);
    const int W = 48, H = 48;
    DiscKernelLUT lut(0.0f, 40.0f, 1.0f, 1.0f);

    const float dA = bk.centre(9), dB = bk.centre(10);
    REQUIRE(bk.bucketOf(dA).frac == 0.0f);
    REQUIRE(bk.bucketOf(dB).frac == 0.0f);
    REQUIRE(bk.bucketOf(dA).index != bk.bucketOf(dB).index);

    const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ true);
    SampleSoA soa;
    soa.begin(1, fp.groups);
    FlattenScratch scratch;
    const int pad = 14;
    for (int y = -pad; y < H + pad; ++y)
        for (int x = -pad; x < W + pad; ++x) {
            std::vector<SampleRecord> v{makeSample(((x + y) & 1) ? dA : dB,
                                                    ((x + y) & 1) ? dA : dB, 1.0f, {0.8f})};
            flattenPixelToSoA(fp, bk, x, y, v, scratch, soa, nullptr);
        }

    double minAlpha[2] = {2.0, 2.0}, maxAlpha[2] = {-1.0, -1.0};
    double ratio[2] = {0.0, 0.0};
    for (int candidate = 0; candidate < 2; ++candidate) {
        const BucketCombine combine = (candidate == 0) ? BucketCombine::FrontToBackOver
                                                        : BucketCombine::CoveragePartition;
        Band band;
        band.K = bk.bucketCount(); band.C = 1; band.W = W; band.H = H;
        HoldoutSoA noHoldout;
        runBand(band, makeScatterParams(W, H, combine), soa, noHoldout, lut);

        for (int y = 16; y < 32; ++y)
            for (int x = 16; x < 32; ++x) {
                minAlpha[candidate] = std::min(minAlpha[candidate],
                                                static_cast<double>(band.outAlpha(x, y)));
                maxAlpha[candidate] = std::max(maxAlpha[candidate],
                                                static_cast<double>(band.outAlpha(x, y)));
            }
        ratio[candidate] = band.outColor(0, 24, 24) / band.outAlpha(24, 24);
    }

    // PINNED: the deficit is 25.0% across two buckets (Decisions, 2026-07-26).
    // Measured here 0.7496..0.7504 over the band interior.
    CHECK(minAlpha[0] > 0.749);
    CHECK(maxAlpha[0] < 0.751);
    // The partition candidate holds the identity to 1e-3 (measured 0.99927 at
    // the worst interior pixel: the two checkerboard depths rasterise
    // DIFFERENT radii, 7.85px and 6.40px, so the two half-coverages do not
    // tile the pixel perfectly).
    CHECK(minAlpha[1] > 0.999);
    CHECK(maxAlpha[1] <= 1.0);
    // Both candidates keep the ratio: this is an alpha deficit, not a colour
    // desync.
    CHECK(std::fabs(ratio[0] - 0.8) <= 1e-05);
    CHECK(std::fabs(ratio[1] - 0.8) <= 1e-05);
}

TEST_CASE("saturation is down-only and preserves the colour:alpha ratio, on the real path")
{
    const CocParams    p  = makeStandardRig(10.0f);
    const DepthBuckets bk = makeStandardBuckets(p);
    const int W = 24, H = 24;
    DiscKernelLUT lut(0.0f, 4.0f, 1.0f, 1.0f);
    const float unpremult = 0.8f;

    SUBCASE("coincident opaque sharp fragments: bucket alpha -> exactly 1, colour rescaled, "
            "across THREE channels and several pixels")
    {
        // Multi-channel and multi-pixel on purpose: with one channel the
        // saturation driver's channel stride is never exercised, and with one
        // saturating pixel neither is its pixel indexing.
        const int C = 3;
        const float unpre[3] = {0.35f, 0.62f, 0.97f};
        struct Spot { int x, y, count; };
        const Spot spots[] = {{8, 6, 2}, {17, 15, 3}, {5, 19, 1}};   // 1 does NOT saturate

        SampleSoA soa;
        soa.begin(C, makeSingleChannelGroup(C));
        for (const Spot& s : spots) {
            for (int i = 0; i < s.count; ++i) {
                FragmentRecord f;
                f.x = s.x; f.y = s.y;
                f.radius = 0.0f;                       // sharp path
                f.depth = 5.0f;
                f.alpha = 1.0f;
                BucketWeight bw;
                bw.index = 3;
                bw.frac  = 0.0f;
                f.deposit = fragmentDeposit(bw, 1.0f);
                f.kind = FragmentKind::Point;
                const float ch[3] = {unpre[0], unpre[1], unpre[2]};
                soa.appendFragment(f, ch);
            }
        }

        const ScatterParams sp = makeScatterParams(W, H, BucketCombine::CoveragePartition);
        Band band;
        band.K = bk.bucketCount(); band.C = C; band.W = W; band.H = H;
        band.planes.allocate(band.K, C, W, H);
        band.planes.zero();
        HoldoutSoA noHoldout;
        scatterOnThread(sp, soa, noHoldout, lut, band.planes);

        // Additive within a bucket: honestly `count` before the pass.
        for (const Spot& s : spots)
            CHECK(band.planeAlpha(3, s.x, s.y)
                  == doctest::Approx(static_cast<float>(s.count)));

        band.color.assign(static_cast<std::size_t>(C) * band.pixels(), -777.0f);
        band.alpha.assign(static_cast<std::size_t>(band.pixels()), -777.0f);
        resolveBandCPU(sp, band.planes, band.color.data(), band.alpha.data());

        for (const Spot& s : spots) {
            CAPTURE(s.x);
            CAPTURE(s.count);
            CHECK(band.planeAlpha(3, s.x, s.y) == 1.0f);        // exactly 1, assigned
            CHECK(band.outAlpha(s.x, s.y) == doctest::Approx(1.0f));
            for (int c = 0; c < C; ++c)
                CHECK(std::fabs(band.outColor(c, s.x, s.y) - unpre[c]) <= 1e-06);
        }

        // Everywhere else stayed empty -- the pass touched only what it should.
        CHECK(band.outAlpha(0, 0) == 0.0f);
        CHECK(band.outColor(1, 0, 0) == 0.0f);
    }

    SUBCASE("alpha < 1 is never scaled up: an honest coverage deficit survives resolve")
    {
        SampleSoA soa;
        soa.begin(1, makeSingleChannelGroup(1));
        FragmentRecord f;
        f.x = W / 2; f.y = H / 2;
        f.radius = 0.0f;
        f.depth = 5.0f;
        f.alpha = 0.4f;
        BucketWeight bw;
        bw.index = 3;
        bw.frac = 0.0f;
        f.deposit = fragmentDeposit(bw, 0.4f);
        f.kind = FragmentKind::Point;
        const float ch[1] = {0.4f * unpremult};
        soa.appendFragment(f, ch);

        const ScatterParams sp = makeScatterParams(W, H, BucketCombine::CoveragePartition);
        Band band;
        band.K = bk.bucketCount(); band.C = 1; band.W = W; band.H = H;
        HoldoutSoA noHoldout;
        runBand(band, sp, soa, noHoldout, lut);

        CHECK(band.planeAlpha(3, W / 2, H / 2) == doctest::Approx(0.4f));
        CHECK(band.outAlpha(W / 2, H / 2) == doctest::Approx(0.4f));
        CHECK(std::fabs(band.outColor(0, W / 2, H / 2) / band.outAlpha(W / 2, H / 2) - unpremult)
              <= 1e-06);
    }
}

TEST_CASE("BucketPlanes::zero() clears ALL FOUR planes, so a band loop may reuse the allocation")
{
    // The band loop the design reference specifies allocates once and calls
    // zero() per band (see scatterBandCPU's header: "ACCUMULATED INTO, never
    // cleared here").  Every case in this file allocates fresh planes, and
    // allocate() zero-fills, so a zero() that missed a plane was invisible --
    // and would show up in production as the previous band's area bleeding
    // into this one's composite.
    const int K = 4, C = 2, W = 10, H = 8;
    DiscKernelLUT lut(0.0f, 8.0f, 1.0f, 1.0f);
    const ScatterParams sp = makeScatterParams(W, H, BucketCombine::CoveragePartition);
    HoldoutSoA none;

    auto oneFragment = [&](int x, int bucket, float alpha) {
        SampleSoA soa;
        soa.begin(C, makeSingleChannelGroup(C));
        FragmentRecord f;
        f.x = x; f.y = H / 2;
        f.radius = 3.0f;                    // a real disc, so every plane is hit
        f.depth = 5.0f;
        f.alpha = alpha;
        BucketWeight bw;
        bw.index = bucket;
        bw.frac  = 0.4f;                    // frac > 0 -> a rear deposit -> colocated
        f.deposit = fragmentDeposit(bw, alpha);
        f.kind = FragmentKind::Point;
        const float ch[2] = {alpha * 0.3f, alpha * 0.6f};
        soa.appendFragment(f, ch);
        return soa;
    };

    BucketPlanes reused;
    reused.allocate(K, C, W, H);
    reused.zero();
    scatterOnThread(sp, oneFragment(3, 0, 0.8f), none, lut, reused);

    // Every plane really did receive something, so the clear below has work to do.
    const std::ptrdiff_t px = static_cast<std::ptrdiff_t>(W) * H;
    double before = 0.0;
    for (int k = 0; k < K; ++k) {
        before += planeSum(reused.alpha, k, px);
        before += planeSum(reused.weight, k, px);
        before += planeSum(reused.colocated, k, px);
    }
    REQUIRE(before > 0.0);

    reused.zero();
    for (std::size_t i = 0; i < reused.color.size(); ++i)
        REQUIRE(reused.color[i] == 0.0f);
    for (std::size_t i = 0; i < reused.alpha.size(); ++i) {
        REQUIRE(reused.alpha[i] == 0.0f);
        REQUIRE(reused.weight[i] == 0.0f);
        REQUIRE(reused.colocated[i] == 0.0f);
    }

    // And a second band scattered into the reused planes matches a fresh one,
    // plane for plane -- the property the band loop actually depends on.
    scatterOnThread(sp, oneFragment(7, 2, 0.55f), none, lut, reused);

    BucketPlanes fresh;
    fresh.allocate(K, C, W, H);
    fresh.zero();
    scatterOnThread(sp, oneFragment(7, 2, 0.55f), none, lut, fresh);

    std::size_t differing = 0;
    for (std::size_t i = 0; i < fresh.alpha.size(); ++i) {
        if (reused.alpha[i] != fresh.alpha[i]) ++differing;
        if (reused.weight[i] != fresh.weight[i]) ++differing;
        if (reused.colocated[i] != fresh.colocated[i]) ++differing;
    }
    for (std::size_t i = 0; i < fresh.color.size(); ++i)
        if (reused.color[i] != fresh.color[i]) ++differing;
    CHECK(differing == 0);
}

TEST_CASE("an ALPHA-ZERO fragment still deposits its colour: the cull is on alpha AND colour")
{
    // partitionColorScale(0, t) == t (the emissive limit), so a fragment whose
    // alpha underflowed to 0 -- which the flatten's span split explicitly
    // allows for a thin fog piece -- still carries colour.  The scatter's
    // early-out therefore culls only when BOTH deposits are empty.  A cull on
    // either one alone silently drops emissive and holdout-zeroed content.
    const int K = 5, W = 12, H = 12;
    DiscKernelLUT lut(0.0f, 8.0f, 1.0f, 1.0f);
    const std::ptrdiff_t px = static_cast<std::ptrdiff_t>(W) * H;

    SampleSoA soa;
    soa.begin(1, makeSingleChannelGroup(1));
    FragmentRecord f;
    f.x = W / 2; f.y = H / 2;
    f.radius = 0.0f;                        // sharp: one pixel, weight 1
    f.depth  = 5.0f;
    f.alpha  = 0.0f;
    BucketWeight bw;
    bw.index = 2;
    bw.frac  = 0.4f;
    f.deposit = fragmentDeposit(bw, 0.0f);
    f.kind = FragmentKind::Point;
    // The emissive limit really is what this fixture rests on.
    REQUIRE(f.deposit.alpha0 == 0.0f);
    REQUIRE(f.deposit.alpha1 == 0.0f);
    REQUIRE(f.deposit.colorScale0 == doctest::Approx(0.6f));
    REQUIRE(f.deposit.colorScale1 == doctest::Approx(0.4f));
    const float ch[1] = {0.5f};
    soa.appendFragment(f, ch);

    BucketPlanes planes;
    planes.allocate(K, 1, W, H);
    planes.zero();
    HoldoutSoA none;
    scatterOnThread(makeScatterParams(W, H, BucketCombine::CoveragePartition),
                    soa, none, lut, planes);

    // The colour is split between the two buckets by the partition; the alpha
    // planes stay empty, which is the honest answer for a transparent fragment.
    CHECK(planeSum(planes.color, 2, px) == doctest::Approx(0.5f * 0.6f));
    CHECK(planeSum(planes.color, 3, px) == doctest::Approx(0.5f * 0.4f));
    CHECK(planeSum(planes.alpha, 2, px) == 0.0);
    CHECK(planeSum(planes.alpha, 3, px) == 0.0);
}

TEST_CASE("resolveBandCPU's default branch routes to CoveragePartition, NOT to plain `over`")
{
    // The milestone is explicit that an out-of-range `combine` must render what
    // an unset one would and must NOT fall through to plain `over` (measured up
    // to +94% alpha for a split fragment).  Every other case in this file sets
    // the enum explicitly, so nothing exercised the default at all.
    const int K = 4, W = 32, H = 32;
    DiscKernelLUT lut(0.0f, 12.0f, 1.0f, 1.0f);

    SampleSoA soa;
    soa.begin(1, makeSingleChannelGroup(1));
    FragmentRecord f;
    f.x = W / 2; f.y = H / 2;
    f.radius = 6.0f;
    f.depth  = 5.0f;
    f.alpha  = 0.9f;
    BucketWeight bw;
    bw.index = 1;
    bw.frac  = 0.5f;                        // split across two buckets: the
    f.deposit = fragmentDeposit(bw, 0.9f);  // configuration the two differ on
    f.kind = FragmentKind::Point;
    f.coverageHead = true;
    const float ch[1] = {0.9f * 0.5f};
    soa.appendFragment(f, ch);

    auto run = [&](BucketCombine combine) {
        Band band;
        band.K = K; band.C = 1; band.W = W; band.H = H;
        HoldoutSoA none;
        ScatterParams sp = makeScatterParams(W, H, BucketCombine::CoveragePartition);
        sp.combine = combine;               // set AFTER, so a garbage value survives
        runBand(band, sp, soa, none, lut, /*useThread*/ false);
        return std::vector<float>(band.alpha.begin(), band.alpha.end());
    };

    const std::vector<float> partition = run(BucketCombine::CoveragePartition);
    const std::vector<float> over      = run(BucketCombine::FrontToBackOver);
    const std::vector<float> garbage   = run(static_cast<BucketCombine>(99));

    double sumP = 0.0, sumO = 0.0;
    for (float v : partition) sumP += v;
    for (float v : over)      sumO += v;
    // The two candidates really do disagree on this fixture (+~60% for `over`),
    // so "garbage == partition" is a statement with content.
    REQUIRE(sumO > sumP * 1.2);

    CHECK(garbage == partition);
    CHECK_FALSE(garbage == over);
}

TEST_CASE("ScatterStats accounts for every fragment exactly once")
{
    // The stats block feeds M1.P4.T2's perf gate and the node's own reporting,
    // and nothing exercised it.  Counts are hand-derived from the fixture.
    const int K = 4, W = 20, H = 20;
    DiscKernelLUT lut(0.0f, 10.0f, 1.0f, 1.0f);

    SampleSoA soa;
    soa.begin(1, makeSingleChannelGroup(1));

    auto push = [&](int x, int y, float radius, float alpha, int bucket, bool empty = false) {
        FragmentRecord f;
        f.x = x; f.y = y;
        f.radius = radius;
        f.depth  = 5.0f;
        f.alpha  = alpha;
        BucketWeight bw;
        bw.index = bucket;
        bw.frac  = 0.0f;
        f.deposit = fragmentDeposit(bw, alpha);
        if (empty) {
            // Neither alpha nor colour in EITHER deposit: the one shape the
            // zero-deposit early-out is allowed to drop.  (Alpha 0 alone is
            // not it -- partitionColorScale's emissive limit leaves the colour
            // scale at 1, which the case above pins.)
            f.deposit.colorScale0 = 0.0f;
            f.deposit.colorScale1 = 0.0f;
        }
        f.kind = FragmentKind::Point;
        soa.appendFragment(f, &alpha);
    };

    push(10, 10, 0.0f, 0.8f, 1);            // sharp, inside   -> 1 deposit
    push( 6,  6, 4.0f, 0.7f, 2);            // disc, inside    -> many rows
    push(10, 10, 0.0f, 0.0f, 1, true);      // no alpha, no colour -> culled
    push(10, 10, 0.0f, 0.5f, K + 3);        // bad bucket      -> culled
    push(-40, 10, 0.0f, 0.5f, 0);           // wholly outside  -> culled

    BucketPlanes planes;
    planes.allocate(K, 1, W, H);
    planes.zero();
    ScatterScratch scratch;
    HoldoutSoA none;
    ScatterStats stats;
    scatterBandCPU(makeScatterParams(W, H, BucketCombine::CoveragePartition),
                   soa, none, lut, planes, scratch, &stats);

    CHECK(stats.fragments == 5u);
    // The two radius-0 fragments that reached the group loop; the empty one is
    // culled before it and the bad-bucket one before that.
    CHECK(stats.sharpFragments == 2u);
    CHECK(stats.culled == 3u);              // empty deposits, bad bucket, off-band
    CHECK(stats.rowSpans > 0u);
    CHECK(stats.pixelDeposits > 1u);
}

TEST_CASE("parent reconstruction catches a MISLABEL that checkCompositionContract accepts")
{
    // M1.P3.T1's review: the branch and the audit read the same `flags` field,
    // so a span-split part labelled Point is internally consistent and passes
    // the audit while double-counting.  Parent reconstruction is the only check
    // that sees it -- this case proves BOTH halves of that claim.
    const CocParams    p  = makeStandardRig(10.0f);
    const DepthBuckets bk = makeStandardBuckets(p);
    const int W = 160, H = 160;
    DiscKernelLUT lut(0.0f, 60.0f, 1.0f, 1.0f);
    const float alpha = 0.9f, unpremult = 0.5f;

    struct Case { int buckets; double misAlphaPct; };
    const Case cases[] = {{2, 40.09}, {4, 70.70}};

    for (const Case& cs : cases) {
        CAPTURE(cs.buckets);
        const float zf = bk.boundary(15 - cs.buckets);
        const float zb = bk.boundary(15);

        SpanSplitPart parts[kMaxSpanSplitParts];
        const int nParts = splitSpanAtBoundaries(bk, zf, zb, alpha, parts, kMaxSpanSplitParts);
        REQUIRE(nParts == cs.buckets);

        double alphaSum[2] = {0.0, 0.0};
        double colorSum[2] = {0.0, 0.0};
        bool   contractOk[2] = {false, false};

        for (int mislabel = 0; mislabel < 2; ++mislabel) {
            SampleSoA soa;
            soa.begin(1, makeSingleChannelGroup(1));
            for (int i = 0; i < nParts; ++i) {
                const float d = sampleMidDepth(parts[i].zFront, parts[i].zBack);
                FragmentRecord f;
                f.x = W / 2; f.y = H / 2;
                f.depth = d;
                f.radius = radiusPixels(p, d);
                f.alpha = parts[i].alpha;
                if (mislabel) {
                    // What a mislabel really produces: the part is treated as
                    // its own point sample, so it re-splits by CENTRES and
                    // claims its own coverage.
                    f.kind = FragmentKind::Point;
                    f.deposit = fragmentDeposit(bk.bucketOf(d), f.alpha);
                    f.coverageHead = true;
                } else {
                    f.kind = FragmentKind::Volumetric;
                    f.deposit = fragmentDeposit(bk.bucketOfContaining(d), f.alpha);
                    f.coverageHead = (i == 0);
                }
                const float ch[1] = {unpremult * alpha * parts[i].colorScale};
                soa.appendFragment(f, ch);
            }

            contractOk[mislabel] = checkCompositionContract(soa, bk);

            Band band;
            band.K = bk.bucketCount(); band.C = 1; band.W = W; band.H = H;
            HoldoutSoA noHoldout;
            runBand(band, makeScatterParams(W, H, BucketCombine::CoveragePartition),
                    soa, noHoldout, lut);
            alphaSum[mislabel] = bandAlphaSum(band);
            colorSum[mislabel] = bandColorSum(band, 0);
        }

        // THE AUDIT CANNOT SEE IT.  Both labellings are internally consistent.
        CHECK(contractOk[0]);
        CHECK(contractOk[1]);

        // PARENT RECONSTRUCTION DOES.  Correct: the parent, exactly.
        CHECK(std::fabs(alphaSum[0] - alpha) <= 1e-06);
        CHECK(std::fabs(colorSum[0] - alpha * unpremult) <= 1e-06);

        // Mislabelled: PINNED at the measured over-count (+40.09% at 2 parts,
        // +70.70% at 4, alpha 0.9), asserted as a band rather than a floor so
        // that neither a fix nor a worsening slips through.
        const double got = (alphaSum[1] - alpha) / alpha * 100.0;
        CAPTURE(got);
        CHECK(got > cs.misAlphaPct - 0.5);
        CHECK(got < cs.misAlphaPct + 0.5);
    }
}

TEST_CASE("volumetric parent reconstruction is EXACT in front of focus, at any part count")
{
    // M1.P3.T9's headline: with the fourth (co-located area) plane the
    // coverage-partition composite reconstructs a split parent exactly wherever
    // its part radii do not increase front to back -- i.e. every parent lying
    // wholly in front of focus, including one spanning that whole side.
    const CocParams    p  = makeStandardRig(10.0f);
    const DepthBuckets bk = makeStandardBuckets(p);
    const int W = 160, H = 160;
    DiscKernelLUT lut(0.0f, 60.0f, 1.0f, 1.0f);
    const float unpremult = 0.5f;

    for (float alpha : {0.9f, 0.1f}) {
        for (int nBuckets : {1, 2, 4, 8, 12, 15}) {     // 15 == the whole front side
            CAPTURE(alpha);
            CAPTURE(nBuckets);
            const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ true);
            const SampleSoA soa = flattenOnePixel(fp, bk, W / 2, H / 2,
                {makeSample(bk.boundary(15 - nBuckets), bk.boundary(15), alpha,
                            {alpha * unpremult})});
            REQUIRE(soa.fragmentCount() == static_cast<std::size_t>(nBuckets));

            Band band;
            band.K = bk.bucketCount(); band.C = 1; band.W = W; band.H = H;
            HoldoutSoA noHoldout;
            runBand(band, makeScatterParams(W, H, BucketCombine::CoveragePartition),
                    soa, noHoldout, lut);

            // MEASURED <= 1e-07 relative at every one of these; 2e-06 is ~20x
            // headroom.  Before the fourth plane the same cases read -6.4% at 4
            // buckets, -24.8% at 8, -46.5% at 12 and -92.1% full range.
            CHECK(std::fabs(bandAlphaSum(band) - alpha) <= 2e-06 * alpha);
            CHECK(std::fabs(bandColorSum(band, 0) - alpha * unpremult) <= 2e-06 * alpha);

            // The ratio invariant again, integrated over the band.
            CHECK(std::fabs(bandColorSum(band, 0) / bandAlphaSum(band) - unpremult) <= 1e-06);
        }
    }
}

TEST_CASE("behind focus the residue is structural: PINNED at +28.2 / +45.2 / +51.2 / +59.1%")
{
    // Decisions, 2026-07-27: "Behind focus the residue is structurally
    // irreducible by any per-bucket plane.  This is settled, not open."  Pinned
    // so it is documentation-with-teeth rather than something that drifts
    // silently -- a change here means the composite changed, and must be
    // adjudicated, not re-fitted.
    const CocParams    p  = makeStandardRig(1.0f);      // focus at the near end
    const DepthBuckets bk = makeStandardBuckets(p);
    REQUIRE(bk.focusBoundary() == 0);                   // the whole range is behind focus

    const int W = 160, H = 160;
    DiscKernelLUT lut(0.0f, 60.0f, 1.0f, 1.0f);
    const float alpha = 0.9f, unpremult = 0.5f;

    struct Case { int buckets; double partitionPct; double overPct; };
    const Case cases[] = {{2, 28.22, 49.13}, {3, 45.21, 75.30},
                          {4, 51.23, 91.13}, {8, 59.05, 117.12}};

    for (const Case& cs : cases) {
        CAPTURE(cs.buckets);
        const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ true);
        const SampleSoA soa = flattenOnePixel(fp, bk, W / 2, H / 2,
            {makeSample(bk.boundary(0), bk.boundary(cs.buckets), alpha, {alpha * unpremult})});
        REQUIRE(soa.fragmentCount() == static_cast<std::size_t>(cs.buckets));

        for (int candidate = 0; candidate < 2; ++candidate) {
            const BucketCombine combine = (candidate == 0) ? BucketCombine::CoveragePartition
                                                            : BucketCombine::FrontToBackOver;
            Band band;
            band.K = bk.bucketCount(); band.C = 1; band.W = W; band.H = H;
            HoldoutSoA noHoldout;
            runBand(band, makeScatterParams(W, H, combine), soa, noHoldout, lut);

            const double pct = (bandAlphaSum(band) - alpha) / alpha * 100.0;
            const double want = (candidate == 0) ? cs.partitionPct : cs.overPct;
            CAPTURE(candidate);
            CAPTURE(pct);
            CHECK(pct > want - 0.1);
            CHECK(pct < want + 0.1);

            // Even where the alpha is wrong the pair stays in lockstep: this is
            // an occlusion-modelling residue, never a colour desync.
            CHECK(std::fabs(bandColorSum(band, 0) / bandAlphaSum(band) - unpremult) <= 1e-06);
        }
    }
}

TEST_CASE("the four hand-built composite identities, with the fourth plane both zero and populated")
{
    // compositePixelCoveragePartition() driven directly on one pixel's planes
    // (pixelCount = 1), which is what the header documents as the standalone
    // entry point.  Expected values are hand-derived, not re-run.
    auto composite = [](std::vector<float> cov, std::vector<float> alpha,
                        std::vector<float> colocated, std::vector<float> color,
                        float* outColor, float* outAlpha) {
        compositePixelCoveragePartition(color.data(), alpha.data(), cov.data(),
                                        colocated.data(),
                                        static_cast<int>(cov.size()), 1, 1,
                                        outColor, outAlpha);
    };

    const float unpremult = 0.8f;
    float c = -1.0f, a = -1.0f;

    SUBCASE("two 50% fog layers -> exactly plain `over`, 0.75")
    {
        composite({1.0f, 1.0f}, {0.5f, 0.5f}, {0.0f, 0.0f},
                  {0.5f * unpremult, 0.5f * unpremult}, &c, &a);
        CHECK(a == doctest::Approx(0.75f).epsilon(1e-6));
        CHECK(c == doctest::Approx(0.75f * unpremult).epsilon(1e-6));

        // Plain `over` agrees here -- dense scenes are `over`, identically.
        float c2 = 0.0f, a2 = 0.0f;
        std::vector<float> alpha{0.5f, 0.5f};
        std::vector<float> color{0.5f * unpremult, 0.5f * unpremult};
        compositePixelFrontToBack(color.data(), alpha.data(), 2, 1, 1, &c2, &a2);
        CHECK(a2 == doctest::Approx(0.75f).epsilon(1e-6));
    }

    SUBCASE("receding opaque plane (four quarter-coverages) -> exactly 1, where `over` gives 0.684")
    {
        composite({0.25f, 0.25f, 0.25f, 0.25f}, {0.25f, 0.25f, 0.25f, 0.25f},
                  {0.0f, 0.0f, 0.0f, 0.0f},
                  {0.25f * unpremult, 0.25f * unpremult, 0.25f * unpremult, 0.25f * unpremult},
                  &c, &a);
        CHECK(a == doctest::Approx(1.0f).epsilon(1e-6));
        CHECK(c == doctest::Approx(unpremult).epsilon(1e-6));

        float c2 = 0.0f, a2 = 0.0f;
        std::vector<float> alpha(4, 0.25f);
        std::vector<float> color(4, 0.25f * unpremult);
        compositePixelFrontToBack(color.data(), alpha.data(), 4, 1, 1, &c2, &a2);
        // 1 - 0.75^4 = 0.68359375 exactly: the PINNED 31.6%-over-4-buckets deficit.
        CHECK(a2 == doctest::Approx(0.68359375f).epsilon(1e-6));
    }

    SUBCASE("validation scene (i): an honest 60% coverage hole stays 0.6, never scaled up")
    {
        composite({0.6f, 0.0f, 0.0f, 0.0f}, {0.6f, 0.0f, 0.0f, 0.0f},
                  {0.0f, 0.0f, 0.0f, 0.0f},
                  {0.6f * unpremult, 0.0f, 0.0f, 0.0f}, &c, &a);
        CHECK(a == doctest::Approx(0.6f).epsilon(1e-6));
        CHECK(c == doctest::Approx(0.6f * unpremult).epsilon(1e-6));
    }

    SUBCASE("4-part opaque slab, FOURTH PLANE POPULATED -> exactly 1 (not 4)")
    {
        // The head claims the area; the other three parts are co-located on it.
        composite({1.0f, 0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f},
                  {0.0f, 1.0f, 1.0f, 1.0f},
                  {unpremult, unpremult, unpremult, unpremult}, &c, &a);
        CHECK(a == doctest::Approx(1.0f).epsilon(1e-6));
        CHECK(c == doctest::Approx(unpremult).epsilon(1e-6));
    }

    SUBCASE("4-part alpha-0.9 slab, FOURTH PLANE POPULATED -> exactly the parent's 0.9")
    {
        const double partAlpha = 1.0 - std::pow(1.0 - 0.9, 0.25);   // hand-derived
        const float  pa = static_cast<float>(partAlpha);
        composite({1.0f, 0.0f, 0.0f, 0.0f}, {pa, pa, pa, pa},
                  {0.0f, 1.0f, 1.0f, 1.0f},
                  {pa * unpremult, pa * unpremult, pa * unpremult, pa * unpremult}, &c, &a);
        CHECK(a == doctest::Approx(0.9f).epsilon(1e-6));
        CHECK(c == doctest::Approx(0.9f * unpremult).epsilon(1e-6));
    }

    SUBCASE("a fractionally split opaque fragment reconstructs at ANY kernel coverage")
    {
        // The rear deposit carries alpha with no new area, so it lands in the
        // co-located plane; the pair must read back as w, not 2w.
        for (float w : {1.0f, 0.5f, 0.001f}) {
            CAPTURE(w);
            const float f  = 0.3f;
            const float a0 = partitionAlpha(1.0f, 1.0f - f);
            const float a1 = partitionAlpha(1.0f, f);
            composite({w, 0.0f}, {w * a0, w * a1}, {0.0f, w},
                      {w * a0 * unpremult, w * a1 * unpremult}, &c, &a);
            CHECK(a == doctest::Approx(w).epsilon(1e-5));
            CHECK(c == doctest::Approx(w * unpremult).epsilon(1e-5));
        }
    }

    SUBCASE("a bucket carrying BOTH new area and co-located area splits its alpha BY AREA")
    {
        // The configuration M1.P3.T9's area split exists for: one bucket holds
        // a head from one parent AND a co-located part of another.  Hand
        // derivation for cov 0.4 / colocated 0.6 / alpha 0.5, unpremult 0.8:
        //
        //   aRes = a * D/(C+D) = 0.5 * 0.6/1.0 = 0.30,  aCov = 0.20
        //   local = aCov/cov = 0.5;  fit = min(0.4, 1) = 0.4, excess = 0
        //     accAlpha  = 0.4 * 0.5                       = 0.20
        //     tClaimed  = (0*1 + 0.4*(1-0.5)) / 0.4       = 0.5
        //   residual: accAlpha += aRes * tClaimed = 0.30 * 0.5 = 0.15
        //     TOTAL = 0.35
        //
        // The pre-T9 `aCov = min(A_k, C_k)` split instead gives aCov = 0.4,
        // local = 1, accAlpha = 0.4, tClaimed = 0 and a starved residual, i.e.
        // 0.40 -- so this one number separates the two forms.
        composite({0.4f}, {0.5f}, {0.6f}, {0.5f * unpremult}, &c, &a);
        CHECK(a == doctest::Approx(0.35f).epsilon(1e-6));
        CHECK(c == doctest::Approx(0.35f * unpremult).epsilon(1e-6));

        // Two buckets, so the carried transmittance is exercised too:
        //   bucket 0 as above leaves tClaimed = 0.5*(1 - 0.30/0.6) = 0.25,
        //   claimedArea 0.4, freeArea 0.6.
        //   bucket 1: cov 0.5, colo 0, a 0.4 -> aCov = 0.4, local = 0.8,
        //             fit = min(0.5, 0.6) = 0.5 -> accAlpha += 0.5*0.8 = 0.40
        //   TOTAL = 0.35 + 0.40 = 0.75
        composite({0.4f, 0.5f}, {0.5f, 0.4f}, {0.6f, 0.0f},
                  {0.5f * unpremult, 0.4f * unpremult}, &c, &a);
        CHECK(a == doctest::Approx(0.75f).epsilon(1e-6));
        CHECK(c == doctest::Approx(0.75f * unpremult).epsilon(1e-6));
    }

    SUBCASE("the FOURTH plane is clamped into [0,1] like the coverage plane")
    {
        // An over-covered pixel can carry more than a pixel's worth of
        // co-located area (the saturation pass bounds alpha, not area), and the
        // residual's divisor is that plane -- so leaving it unclamped inflates
        // the transmittance every later bucket is attenuated by.  Hand derived,
        // three buckets, all pure residual:
        //   b0: cov 0, colo 3 -> 1, a 0.5 -> aRes 0.5, resLocal 0.5,
        //       claimedArea 1, freeArea 0, tClaimed 0.5, accAlpha 0.5
        //   b1: cov 0, colo 1,   a 0.4 -> accAlpha += 0.4*0.5 = 0.2
        //   TOTAL 0.70  (unclamped: resLocal 0.5/3 -> tClaimed 0.8333 and 0.8333)
        composite({0.0f, 0.0f}, {0.5f, 0.4f}, {3.0f, 1.0f},
                  {0.5f * unpremult, 0.4f * unpremult}, &c, &a);
        CHECK(a == doctest::Approx(0.70f).epsilon(1e-6));
        CHECK(c == doctest::Approx(0.70f * unpremult).epsilon(1e-6));
    }

    SUBCASE("the colour:alpha ratio survives an out-of-contract plane (aCov <= cov guard)")
    {
        // Hand-built planes that BREAK the deposit invariant (A_k > C_k + D_k):
        // the guard must move the excess into the residual rather than emit
        // colour for alpha it did not add.  This is the exact shape of the
        // defect that has now appeared three times.
        composite({0.3f}, {1.0f}, {0.2f}, {1.0f * unpremult}, &c, &a);
        REQUIRE(a > 0.0f);
        CHECK(std::fabs(c / a - unpremult) <= 1e-05);
        CHECK(a <= 1.0f);
    }
}

TEST_CASE("colour:alpha ratio is a standing invariant of the composite over randomised planes")
{
    // One invariant instead of three cases (Decisions, 2026-07-27): clamping
    // one of a premultiplied pair and not the other has been the defect in the
    // residual term, in the area split and in the saturation pass.
    Lcg rng(0xA11CEu);
    const int K = 6, C = 3;
    const float unpremult[3] = {0.35f, 0.7f, 0.95f};

    for (int trial = 0; trial < 5000; ++trial) {
        std::vector<float> cov(K), alpha(K), colocated(K), color(static_cast<std::size_t>(K) * C);
        for (int k = 0; k < K; ++k) {
            // Respect the production deposit invariant A_k <= C_k + D_k, which
            // is what the composite may assume; the case above covers the
            // violation path.
            cov[k]       = (rng.unit() < 0.3f) ? 0.0f : rng.range(0.0f, 1.4f);
            colocated[k] = (rng.unit() < 0.4f) ? 0.0f : rng.range(0.0f, 1.4f);
            alpha[k]     = rng.range(0.0f, 1.0f) * std::min(1.0f, cov[k] + colocated[k]);
            for (int c = 0; c < C; ++c)
                color[static_cast<std::size_t>(k * C + c)] = alpha[k] * unpremult[c];
        }

        float outColor[3] = {0.0f, 0.0f, 0.0f};
        float outAlpha = 0.0f;
        compositePixelCoveragePartition(color.data(), alpha.data(), cov.data(),
                                        colocated.data(), K, C, 1, outColor, &outAlpha);

        CHECK(outAlpha >= 0.0f);
        CHECK(outAlpha <= 1.0f);

        // NOT SCOPED TO THE UNCLAMPED RESULT (M1.P3.T4 review).  An earlier
        // draft of this case excluded `outAlpha == 1`, which is exactly where
        // the invariant was broken: the composite's last statement clamped the
        // alpha and left the colour beside it alone, so a pixel whose
        // accumulated alpha exceeded 1 shipped a premultiplied colour:alpha
        // ratio above the input's -- the FOURTH appearance of "clamp one of a
        // premultiplied pair and not the other" (Decisions, 2026-07-27), and
        // one that the production path reaches (see the fog case below).  The
        // clamp now rescales both, so the invariant holds everywhere.
        if (outAlpha > 1e-04f) {
            for (int c = 0; c < C; ++c) {
                CAPTURE(trial);
                CAPTURE(c);
                // 1e-4 relative: the composite sums several attenuated shares
                // in float; the measured worst drift over this corpus is
                // 3.2e-07 relative, so this is ~300x headroom and still two
                // orders below any real desync (the recorded ones were 75%).
                CHECK(std::fabs(outColor[c] / outAlpha - unpremult[c]) <= 1e-04 * unpremult[c]);
            }
        }
    }
}

TEST_CASE("volumetric fog through the REAL path: a pixel whose alpha clamps keeps its "
          "colour:alpha ratio")
{
    // THE REGRESSION GATE for the clamp asymmetry (M1.P3.T4 review).  Ordinary
    // overlapping volumetric fog drives compositePixelCoveragePartition's
    // accAlpha above 1 -- several co-located residuals attenuate by
    // aRes/D_k, which is weaker than the alpha each of them adds whenever
    // D_k > aRes, so the sum over buckets is not bounded the way the
    // per-bucket terms are.  On THIS fixture 19 of 576 pixels clamp, and
    // before the colour was rescaled with the alpha the worst of them shipped
    // premultiplied colour 0.9959 against an honest 0.5975: +59.5% too bright.
    // (Over 300 randomised fields the worst was +66.0%, at accAlpha 1.6598.)
    const CocParams    p  = makeStandardRig(30.0f);
    const DepthBuckets bk = makeBoundedDeltaCocBuckets(p, 1.0f, 100.0f, 8);
    const int W = 24, H = 24, K = 8;
    DiscKernelLUT lut(0.0f, 40.0f, 1.0f, 1.0f);
    const float unpremult = 0.6f;

    const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ true);
    SampleSoA soa;
    soa.begin(1, fp.groups);
    FlattenScratch scratch;

    Lcg rng(0x1234ABCDu);
    const int pad = 14;
    for (int y = -pad; y < H + pad; ++y)
        for (int x = -pad; x < W + pad; ++x) {
            std::vector<SampleRecord> v;
            for (int i = 0; i < 3; ++i) {
                const float zf = rng.range(1.0f, 90.0f);
                const float zb = zf + rng.range(0.1f, 60.0f);
                const float a  = rng.range(0.05f, 0.99f);
                v.push_back(makeSample(zf, zb, a, {a * unpremult}));
            }
            flattenPixelToSoA(fp, bk, x, y, v, scratch, soa, nullptr);
        }
    REQUIRE(soa.fragmentCount() > 1000u);

    Band band;
    band.K = K; band.C = 1; band.W = W; band.H = H;
    HoldoutSoA none;
    runBand(band, makeScatterParams(W, H, BucketCombine::CoveragePartition), soa, none, lut);

    int clamped = 0;
    double worst = 0.0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const double a = band.outAlpha(x, y);
            if (a >= 1.0)
                ++clamped;
            if (a > 1e-04) {
                CAPTURE(x);
                CAPTURE(y);
                CAPTURE(a);
                worst = std::max(worst, std::fabs(band.outColor(0, x, y) / a - unpremult));
            }
        }

    // The fixture must KEEP reaching the clamp, or the gate stops gating.
    CHECK(clamped >= 10);
    // Measured worst 1.6e-07 absolute with the rescale in place, against
    // 0.357 (i.e. 0.9959 against 0.5975) without it.
    CHECK(worst <= 1e-05);
    // Nothing is scaled UP: the clamp is down-only, exactly like the
    // saturation pass one stage earlier.
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            REQUIRE(band.outAlpha(x, y) <= 1.0f);
}

TEST_CASE("pre_merge moves neither alpha nor coverage for a SINGLE parent")
{
    // M1.P3.T8: a group can never contain two parts of the same parent (they
    // are cut AT the boundaries, so they sit in distinct buckets), which is
    // what makes the single-parent reconstruction knob-independent.  The knob
    // DOES move multi-parent pixels, which the next subcase pins.
    const CocParams    p  = makeStandardRig(10.0f);
    const DepthBuckets bk = makeStandardBuckets(p);
    const int W = 160, H = 160;
    DiscKernelLUT lut(0.0f, 60.0f, 1.0f, 1.0f);

    SUBCASE("single volumetric parent: identical in alpha AND in both area planes")
    {
        for (float alpha : {0.9f, 0.35f}) {
            for (int nBuckets : {2, 4, 8}) {
                CAPTURE(alpha);
                CAPTURE(nBuckets);
                double alphaSum[2] = {0, 0}, newArea[2] = {0, 0}, colocated[2] = {0, 0};

                for (int pm = 0; pm < 2; ++pm) {
                    const FlattenParams fp = makeFlattenParams(p, 1, pm != 0);
                    const SampleSoA soa = flattenOnePixel(fp, bk, W / 2, H / 2,
                        {makeSample(bk.boundary(15 - nBuckets), bk.boundary(15), alpha,
                                    {alpha * 0.5f})});
                    Band band;
                    band.K = bk.bucketCount(); band.C = 1; band.W = W; band.H = H;
                    HoldoutSoA noHoldout;
                    runBand(band, makeScatterParams(W, H, BucketCombine::CoveragePartition),
                            soa, noHoldout, lut);
                    alphaSum[pm] = bandAlphaSum(band);
                    for (int k = 0; k < band.K; ++k) {
                        newArea[pm]   += planeSum(band.planes.weight, k, band.pixels());
                        colocated[pm] += planeSum(band.planes.colocated, k, band.pixels());
                    }
                }

                CHECK(alphaSum[0] == doctest::Approx(alphaSum[1]).epsilon(1e-9));
                CHECK(newArea[0]  == doctest::Approx(newArea[1]).epsilon(1e-9));
                CHECK(colocated[0] == doctest::Approx(colocated[1]).epsilon(1e-9));
                CHECK(newArea[0] == doctest::Approx(1.0).epsilon(1e-5));   // ONE parent, ONE area
            }
        }
    }

    SUBCASE("two distinct co-located point parents: ON is the accurate branch (PINNED)")
    {
        // Decisions, 2026-07-26: pre_merge ON merges them into the exact
        // sequential `over` with ONE coverage; OFF leaves two additive deposits
        // and two coverages.  Measured here 0.694518 / coverage 2.0 (OFF)
        // against 0.580000 / coverage 1.0 (ON).
        const double expectedOn = 0.3 + 0.4 * 0.7;      // 0.58, exact
        double alphaSum[2] = {0, 0}, newArea[2] = {0, 0};

        for (int pm = 0; pm < 2; ++pm) {
            const FlattenParams fp = makeFlattenParams(p, 1, pm != 0);
            const SampleSoA soa = flattenOnePixel(fp, bk, W / 2, H / 2,
                {makeSample(9.0f, 9.0f, 0.3f, {0.3f}),
                 makeSample(9.02f, 9.02f, 0.4f, {0.4f})});
            REQUIRE(soa.fragmentCount() == (pm ? 1u : 2u));
            Band band;
            band.K = bk.bucketCount(); band.C = 1; band.W = W; band.H = H;
            HoldoutSoA noHoldout;
            runBand(band, makeScatterParams(W, H, BucketCombine::CoveragePartition),
                    soa, noHoldout, lut);
            alphaSum[pm] = bandAlphaSum(band);
            for (int k = 0; k < band.K; ++k)
                newArea[pm] += planeSum(band.planes.weight, k, band.pixels());
        }

        CHECK(std::fabs(alphaSum[1] - expectedOn) <= 1e-06);
        CHECK(newArea[1] == doctest::Approx(1.0).epsilon(1e-5));
        CHECK(std::fabs(alphaSum[0] - 0.694518) <= 5e-04);
        CHECK(newArea[0] == doctest::Approx(2.0).epsilon(1e-5));
    }

    SUBCASE("a group that mixes a NON-head part with a following head keeps BOTH coverages "
            "(the survival rule is an OR, not the group head's flag)")
    {
        // The configuration the OR exists for (M1.P3.T8): parent A is cut at
        // boundary(10), so its second part is a NON-head sitting in bucket 10;
        // parent B lies wholly inside bucket 10 immediately behind it and IS a
        // head.  They are adjacent in the staged list, same kind, same bucket,
        // and their radii are 0.44px apart -- so at a 1.0px tolerance (a legal
        // knob value, range 0-2px) they merge into ONE fragment.  Taking the
        // group HEAD's flag would drop B's coverage entirely.
        const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ true, /*tolerance*/ 1.0f);
        const SampleSoA soa = flattenOnePixel(fp, bk, W / 2, H / 2,
            {makeSample(bk.boundary(9), 2.6f, 0.6f, {0.6f * 0.5f}),
             makeSample(2.6f, 2.75f, 0.4f, {0.4f * 0.5f})});

        // A0 (head, bucket 9) and the merged [A1 + B] (bucket 10).
        REQUIRE(soa.fragmentCount() == 2u);
        CHECK(fragmentCoverageHeadOf(soa.flags[0]));
        CHECK(fragmentCoverageHeadOf(soa.flags[1]));
        CHECK(soa.bucketIndex0[0] == 9);
        CHECK(soa.bucketIndex0[1] == 10);

        Band band;
        band.K = bk.bucketCount(); band.C = 1; band.W = W; band.H = H;
        HoldoutSoA noHoldout;
        runBand(band, makeScatterParams(W, H, BucketCombine::CoveragePartition),
                soa, noHoldout, lut);

        double newArea = 0.0;
        for (int k = 0; k < band.K; ++k)
            newArea += planeSum(band.planes.weight, k, band.pixels());
        // TWO parents, TWO coverages -- 1.0 would mean B's was dropped.
        CHECK(newArea == doctest::Approx(2.0).epsilon(1e-5));
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

TEST_CASE("BucketPlanes::bytesForBand is the (C+3) formula and matches a live sizeBytes()")
{
    // The memory-limit knob and the code must not drift apart, and the formula
    // moved from (C+2) to (C+3) at M1.P3.T9.
    CHECK(BucketPlanes::bytesForBand(16, 4, 4096, 64)
          == static_cast<std::size_t>(16) * 4096 * 64 * (4 + 3) * sizeof(float));
    CHECK(BucketPlanes::bytesForBand(16, 4, 4096, 64) == 117440512u);
    CHECK(BucketPlanes::bytesForBand(128, 4, 4096, 64) == 939524096u);

    BucketPlanes planes;
    planes.allocate(8, 3, 32, 16);
    CHECK(planes.sizeBytes() == BucketPlanes::bytesForBand(8, 3, 32, 16));
    planes.release();
    CHECK(planes.sizeBytes() == 0u);
}

// ===========================================================================
// Holdout (M1.P3.T3 / T10 / T11)
// ===========================================================================

TEST_CASE("HoldoutLut::build folds the in-span exponential in exactly at the boundaries, "
          "and agrees with evalBoundaries on overlapping and unsorted input")
{
    const CocParams    p  = makeStandardRig(10.0f);
    const DepthBuckets bk = makeStandardBuckets(p);
    const HoldoutBoundaries hb = makeUniformHoldoutBoundaries(bk);
    REQUIRE(hb.count() == bk.boundaryCount());

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
    // The commonest holdout there is: a solid card.  Before M1.P3.T10 this card
    // at z=50 started occluding at z=10.9 and erased a fragment at z=15 by 98%.
    // Against the decoupled uniform-in-z set it must be essentially
    // unattenuated in front of the card and fully attenuated behind it.
    const CocParams    p  = makeStandardRig(10.0f);
    const DepthBuckets bk = makeStandardBuckets(p);
    const HoldoutBoundaries hb = makeUniformHoldoutBoundaries(bk);

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
                                                 s.index, s.frac, HoldoutInterp::LogChord);
    };

    // In front of the card: 1.000000 exactly (was 0.0215 at z=15 / 0.0 at 30/40).
    for (float z : {5.0f, 15.0f, 30.0f, 40.0f, 44.0f})
        CHECK(vis(z) == 1.0f);
    // Behind it: exactly 0.
    for (float z : {51.0f, 60.0f, 99.0f})
        CHECK(vis(z) == 0.0f);

    // The PINNED bite depth: the first depth reading below half visibility.
    // 44.37 against a true 50, i.e. inside the card's own 6.19-unit bracket
    // [44.3125, 50.5] -- Decisions records 44.4 against the pre-T10 10.9.
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
    const DepthBuckets bk = makeStandardBuckets(p);
    const HoldoutBoundaries hb = makeUniformHoldoutBoundaries(bk);
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
        // 200k-trial corpus at this task's review.  The existing
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
    const DepthBuckets bk = makeStandardBuckets(p);
    const HoldoutBoundaries hb = makeUniformHoldoutBoundaries(bk);
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
        flattenPixelToSoA(fp, bk, W / 2, H / 2, behind, scratch, soa, nullptr);
        // In front of the card (z=3): unattenuated.
        std::vector<SampleRecord> front{makeSample(3.0f, 3.0f, 0.8f, {0.4f})};
        flattenPixelToSoA(fp, bk, W / 2 - 4, H / 2, front, scratch, soa, nullptr);

        const ScatterParams sp = makeScatterParams(W, H, BucketCombine::CoveragePartition);
        BucketPlanes planes;
        planes.allocate(bk.bucketCount(), 1, W, H);
        planes.zero();
        scatterOnThread(sp, soa, view, lut, planes);

        ExpectedPlanes want;
        want.allocate(bk.bucketCount(), 1, W, H);
        refRasterize(want, sp, soa, lut, &view);
        checkPlanes(planes, want);
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
            const SampleSoA soa = flattenOnePixel(fp, bk, W / 2, H / 2,
                {makeSample(depth, depth, 1.0f, {0.5f})});
            Band band;
            band.K = bk.bucketCount(); band.C = 1; band.W = W; band.H = H;
            runBand(band, makeScatterParams(W, H, BucketCombine::CoveragePartition),
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
            const SampleSoA soa = flattenOnePixel(fp, bk, W / 2, H / 2,
                {makeSample(depth, depth, 0.8f, {0.4f})});
            const ScatterParams sp = makeScatterParams(W, H, BucketCombine::CoveragePartition);

            BucketPlanes withHoldout, without;
            withHoldout.allocate(bk.bucketCount(), 1, W, H);
            withHoldout.zero();
            without.allocate(bk.bucketCount(), 1, W, H);
            without.zero();
            HoldoutSoA disabled;
            scatterOnThread(sp, soa, view, lut, withHoldout);
            scatterOnThread(sp, soa, disabled, lut, without);

            std::size_t differing = 0;
            for (std::size_t i = 0; i < without.alpha.size(); ++i) {
                if (withHoldout.alpha[i] != without.alpha[i]) ++differing;
                if (withHoldout.weight[i] != without.weight[i]) ++differing;
                if (withHoldout.colocated[i] != without.colocated[i]) ++differing;
            }
            for (std::size_t i = 0; i < without.color.size(); ++i)
                if (withHoldout.color[i] != without.color[i]) ++differing;
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

        const SampleSoA soa = flattenOnePixel(fp, bk, W / 2, H / 2,
            {makeSample(3.0f, 3.0f, 0.8f, {0.4f})});
        const ScatterParams sp = makeScatterParams(W, H, BucketCombine::CoveragePartition);

        BucketPlanes a, b;
        a.allocate(bk.bucketCount(), 1, W, H);
        a.zero();
        b.allocate(bk.bucketCount(), 1, W, H);
        b.zero();
        HoldoutSoA disabled;
        scatterOnThread(sp, soa, view, lut, a);
        scatterOnThread(sp, soa, disabled, lut, b);

        std::size_t differing = 0;
        for (std::size_t i = 0; i < a.alpha.size(); ++i)
            if (a.alpha[i] != b.alpha[i]) ++differing;
        for (std::size_t i = 0; i < a.color.size(); ++i)
            if (a.color[i] != b.color[i]) ++differing;
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

        const SampleSoA soa = flattenOnePixel(fp, bk, W / 2, H / 2,
            {makeSample(6.0f, 6.0f, 0.8f, {0.4f})});
        Band band;
        band.K = bk.bucketCount(); band.C = 1; band.W = W; band.H = H;
        runBand(band, makeScatterParams(W, H, BucketCombine::CoveragePartition),
                soa, view, lut);
        // Not erased: the short LUT was ignored rather than sampled.
        CHECK(bandAlphaSum(band) == doctest::Approx(0.8).epsilon(1e-5));
    }
}

TEST_CASE("ScatterParams::holdoutInterp threads through scatterBandCPU on BOTH paths, and the "
          "dense alpha<1 holdout is where the three variants diverge")
{
    // M1.P3.T11's review: the variants are NOT confined to fully-opaque
    // content.  46 point samples at alpha=0.9 packed inside one K=16 bracket
    // underflow the stored far-boundary transmittance to bitwise 0, and the
    // three then disagree hard -- LogChord 1.32e-18, MidpointStep 0.0,
    // LinearInT 0.404 at z=48.  Pinned here in DEPOSITED PIXELS, not just in
    // the LUT math, and on both the sharp and the disc path.
    const CocParams    p  = makeStandardRig(10.0f);
    const DepthBuckets bk = makeStandardBuckets(p);
    const HoldoutBoundaries hb = makeUniformHoldoutBoundaries(bk);
    const int W = 40, H = 24;
    DiscKernelLUT lut(0.0f, 30.0f, 1.0f, 1.0f);
    const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ true);

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
        FragmentRecord f;
        f.x = W / 2;
        f.y = H / 2;
        f.radius = path.radius;
        f.depth  = 48.0f;
        f.alpha  = 1.0f;
        f.deposit = fragmentDeposit(bk.bucketOfContaining(48.0f), 1.0f);
        f.kind = FragmentKind::Volumetric;
        f.coverageHead = true;
        const float ch[1] = {0.5f};
        soa.appendFragment(f, ch);

        double got[3] = {0, 0, 0};
        const HoldoutInterp variants[3] = {HoldoutInterp::LogChord,
                                            HoldoutInterp::MidpointStep,
                                            HoldoutInterp::LinearInT};
        for (int v = 0; v < 3; ++v) {
            Band band;
            band.K = bk.bucketCount(); band.C = 1; band.W = W; band.H = H;
            runBand(band, makeScatterParams(W, H, BucketCombine::CoveragePartition, variants[v]),
                    soa, view, lut);
            got[v] = bandAlphaSum(band);
        }

        // PINNED (measured at this task, matching the Decisions log's LUT-level
        // figures): the knob reaches the deposit, and the three answers are
        // materially different -- so a build that stopped threading it would
        // collapse all three onto one number.
        CHECK(got[0] < 1e-12);                  // LogChord ~1.3e-18 * kernel weight
        CHECK(got[1] == 0.0);                   // MidpointStep: hard step, past the midpoint
        CHECK(got[2] == doctest::Approx(0.40404).epsilon(1e-3));   // LinearInT
    }

    SUBCASE("every alpha<1 bracket whose far transmittance did NOT underflow is variant-agnostic")
    {
        // The safe regime: a handful of alpha<1 samples cannot underflow, so
        // all three variants must be bit-identical there.
        HoldoutSampleSoA smallSamples;
        HoldoutLut smallLut;
        buildHoldout(smallSamples, smallLut, hb, W, H, [](int, int, std::vector<SampleRecord>& out) {
            out.push_back(makeSample(30.0f, 55.0f, 0.6f));
            out.push_back(makeSample(20.0f, 20.0f, 0.4f));
        });
        const HoldoutSoA smallView = smallLut.view();
        REQUIRE(smallView.enabled());
        for (int b = 0; b < smallView.boundaryCount(); ++b)
            REQUIRE(smallView.pixelLut(0)[b] > 0.0f);

        const SampleSoA soa = flattenOnePixel(fp, bk, W / 2, H / 2,
            {makeSample(40.0f, 40.0f, 0.8f, {0.4f})});

        std::vector<float> reference;
        for (HoldoutInterp variant : {HoldoutInterp::LogChord, HoldoutInterp::MidpointStep,
                                      HoldoutInterp::LinearInT}) {
            BucketPlanes planes;
            planes.allocate(bk.bucketCount(), 1, W, H);
            planes.zero();
            scatterOnThread(makeScatterParams(W, H, BucketCombine::CoveragePartition, variant),
                            soa, smallView, lut, planes);
            std::vector<float> got(planes.alpha.begin(), planes.alpha.end());
            if (reference.empty())
                reference = got;
            else
                CHECK(got == reference);        // bit-identical
        }
    }
}

// ===========================================================================
// Shared-code regression: tidyOverlapping()'s termination
// ===========================================================================

TEST_CASE("tidyOverlapping terminates and stays bounded on tie-heavy randomised input")
{
    // The non-termination bug (fixed during M1.P2.T2) hung Nuke UNKILLABLY on
    // ordinary volumetric input: the split loop always cut samples[i] at
    // samples[i+1].zFront, which made no progress when the two shared a front
    // -- and the loop created that configuration itself.  Depths are drawn from
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
