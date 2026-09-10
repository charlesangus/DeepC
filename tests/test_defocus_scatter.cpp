// SPDX-License-Identifier: MIT
//
// ============================================================================
//
//  test_defocus_scatter — unit tests for the POD scatter core
//
//  Covers DeepCDefocusScatter.h / .cpp: the SoA flatten, the band scatter and
//  the bucket composite, and the holdout SoA / boundary LUT.  POD level only:
//  no NDK, no DDImage type, no live Nuke session.  scatterBandCPU() is
//  thread-agnostic, so it is driven here through a plain std::thread.
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
//      refFlatten() re-derive the flatten from the documented formulae in
//      double precision (std::pow, not the shipped expm1/log1p chain), so a
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
//  are documentation-with-teeth: a change to them is meant to fail.
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
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
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

// THE STANDARD RIG, the one every pinned number in this file was measured on:
// Physical, f=50, N=2.8, filmback 36mm at 1920px, metres, over a measured
// depth range of [1, 100] at K=16.  Focused at 10m it splits 15 front /
// 1 back; focused at 1m the whole range is behind focus, which is the
// behind-focus rig.
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
// Written from the header documentation, in double, with
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

// "Which kernel would the scatter fetch for this radius?", from the documented
// rule rather than from the shipped predicate: below the sharp threshold every
// fragment is one weight of 1.0 at its own pixel (bin -1), and above it
// DiscKernelLUT rounds onto the NEAREST NODE, IN RADIUS, of the global
// kernel-radius grid.
//
// Deliberately derived by SEARCHING the grid's node radii (kernelGridRadius(),
// which is the grid's definition) instead of by inverting them: the closed
// form kernelGridIndex() uses -- a reciprocal and a harmonic-mean midpoint --
// is exactly the thing this reference exists to disagree with if it is wrong.
int refKernelBin(double radiusPx)
{
    if (!(radiusPx >= static_cast<double>(kSharpRadiusPx)))
        return -1;

    // Bracket, then bisect, on the monotone node radii.
    int lo = 0, hi = 1;
    while (static_cast<double>(kernelGridRadius(hi)) < radiusPx) {
        lo = hi;
        hi *= 2;
        if (hi > (1 << 26))
            return hi;                  // absurd radius; the LUT clamps anyway
    }
    while (hi - lo > 1) {
        const int mid = lo + (hi - lo) / 2;
        if (static_cast<double>(kernelGridRadius(mid)) < radiusPx)
            lo = mid;
        else
            hi = mid;
    }
    const double dLo = radiusPx - static_cast<double>(kernelGridRadius(lo));
    const double dHi = static_cast<double>(kernelGridRadius(hi)) - radiusPx;
    return (dHi < dLo) ? hi : lo;
}

// Which HoldoutBoundaries bracket a depth falls in.  The boundary SET is shared
// code with its own pinned post-conditions in tests/test_defocus_math.cpp
// (buildUniformZ / locate), exactly as tidyOverlapping() is reused above; what
// is re-derived here is the flatten's USE of it.
int refHoldoutBracket(const DepthBuckets& b, double depth)
{
    const HoldoutBoundaries hb = makeUniformHoldoutBoundaries(b);
    return hb.locate(static_cast<float>(depth)).index;
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
    double frac   = 0.0;      // position between the two bucket CENTRES
    double alpha0 = 0.0;
    double alpha1 = 0.0;
    double colorScale0 = 0.0;
    double colorScale1 = 0.0;
    bool   volumetric  = false;
    bool   coverageHead = true;
    bool   depositArea0 = true;   // does deposit 0 write area?
    bool   depositArea1 = true;
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
                                    int channelCount,
                                    bool holdoutConnected = false)
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
    // Each pre-merge group becomes ONE candidate fragment, complete with its
    // assignment; step 6 below then decides which candidates collide.
    std::vector<RefFragment> cands;
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
                // ...and, with a holdout connected, the same HoldoutBoundaries
                // bracket: the group emits ONE fragment at ONE depth while the
                // holdout is sampled per fragment, and the ΔCoC bucket this
                // group is keyed on can be an order of magnitude wider than a
                // holdout bracket.
                if (holdoutConnected
                    && refHoldoutBracket(b, cand.depth) != refHoldoutBracket(b, head.depth))
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
        f.frac   = w.frac;

        cands.push_back(f);
        i = j;
    }

    // --- 6. THE DEPOSIT-COLLISION PASS -------------------------------------
    // Re-derived from the contract, not from the shipped loop: within one
    // source pixel, candidates that land in a shared bucket AND rasterise one
    // kernel are `over`-composited into the FRONT-MOST of them (whose depth,
    // radius and assignment never move); and the pixel's area is claimed at
    // most once per bucket, later same-bucket deposits arriving as co-located.
    std::vector<RefFragment> merged;
    for (const RefFragment& c : cands) {
        bool absorb = false;
        if (!merged.empty()) {
            const RefFragment& g = merged.back();
            const bool oneKernel = (refKernelBin(g.radius) == refKernelBin(c.radius));
            const bool oneBracket =
                !holdoutConnected
                || refHoldoutBracket(b, g.depth) == refHoldoutBracket(b, c.depth);
            const bool share = !(g.index1 < c.index0 || c.index1 < g.index0);
            absorb = (g.volumetric == c.volumetric) && oneKernel && oneBracket && share;
        }

        if (!absorb) {
            merged.push_back(c);
            continue;
        }

        RefFragment& g = merged.back();
        const double w = 1.0 - g.alpha;
        if (w > 0.0) {
            for (int ch = 0; ch < channelCount; ++ch)
                g.channels[static_cast<std::size_t>(ch)] +=
                    c.channels[static_cast<std::size_t>(ch)] * w;
            g.alpha = std::min(g.alpha + c.alpha * w, 1.0);
        }
        g.coverageHead = g.coverageHead || c.coverageHead;
    }

    // --- 7. THE MONOTONE FRONTIER, THE PER-BUCKET ATTENUATION AND THE AREA
    //        CLAIM, then the deposit.
    //
    // Re-derived from the two contracts, not from the shipped loop:
    //
    //  * a deposit may not land in FRONT of a bucket an earlier fragment of the
    //    SAME kernel at this pixel already reached, because the bucket
    //    composite attenuates a whole plane by the whole plane in front of it —
    //    so the assignment is clamped forward to that frontier and takes whole
    //    weight there;
    //  * a deposit landing in a bucket an earlier deposit of the same kernel
    //    already wrote into covers the IDENTICAL destination area, so it is
    //    `over`-composited onto it (its alpha and colour scale by the
    //    transmittance already there) and writes NO area of its own;
    //  * a deposit landing on a bucket claimed by a DIFFERENT kernel keeps its
    //    area but arrives CO-LOCATED, never as a second new-area claim.
    struct RefTouch { int bucket; int bin; double running; };
    std::vector<RefTouch> touched;
    std::vector<std::pair<int, int>> claimed;   // (bucket, the claiming kernel's bin)
    const int lastBucket = (b.bucketCount() > 0) ? (b.bucketCount() - 1) : 0;
    int frontier = 0, frontierBin = 0;

    std::vector<RefFragment> out;
    for (RefFragment f : merged) {
        const int bin = refKernelBin(f.radius);

        if (b.bucketCount() > 0 && f.index0 < frontier && bin == frontierBin) {
            f.index0 = (frontier < b.bucketCount()) ? frontier : lastBucket;
            f.frac   = 0.0;
        }
        f.index1 = (f.frac > 0.0) ? (f.index0 + 1) : f.index0;
        if (f.index1 >= frontier) {
            frontier    = f.index1;
            frontierBin = bin;
        }

        f.alpha0 = refPartitionAlpha(f.alpha, 1.0 - f.frac);
        f.alpha1 = refPartitionAlpha(f.alpha, f.frac);
        f.colorScale0 = refPartitionColorScale(f.alpha, 1.0 - f.frac);
        f.colorScale1 = refPartitionColorScale(f.alpha, f.frac);

        // The visit, once per deposit, front bucket first.
        bool attenuated = false;
        for (int d = 0; d < 2; ++d) {
            if (d == 1 && f.index1 == f.index0)
                break;
            const int bucket = (d == 0) ? f.index0 : f.index1;
            double&   a      = (d == 0) ? f.alpha0 : f.alpha1;
            double&   cs     = (d == 0) ? f.colorScale0 : f.colorScale1;
            bool&     area   = (d == 0) ? f.depositArea0 : f.depositArea1;

            if (bucket < 0 || bucket >= b.bucketCount())
                continue;                       // out of range: nothing tracked

            RefTouch* hit = nullptr;
            for (RefTouch& t : touched)
                if (t.bucket == bucket) { hit = &t; break; }

            if (hit == nullptr) {
                touched.push_back({bucket, bin, a});
            } else if (hit->bin != bin) {
                // a different disc: keeps its area, takes no attenuation
            } else {
                const double t = 1.0 - hit->running;
                a  *= t;
                cs *= t;
                hit->running += a;
                area = false;
                attenuated = true;
                if (d == 0)
                    f.coverageHead = false;
            }
        }

        if (f.coverageHead) {
            bool seen = false;
            for (const std::pair<int, int>& c : claimed) {
                if (c.first != f.index0)
                    continue;
                seen = true;
                if (c.second != bin)
                    f.coverageHead = false;     // a different disc: co-located
                break;
            }
            if (!seen)
                claimed.emplace_back(f.index0, bin);
        }

        // The fragment's alpha AS DEPOSITED: the two deposits must still
        // reconstruct it under `over`.
        if (attenuated)
            f.alpha = 1.0 - (1.0 - f.alpha0) * (1.0 - f.alpha1);

        out.push_back(f);
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

// scatterBandCPU() ON A std::thread — its thread-agnostic contract, exercised
// literally.
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

// Flatten one hand-built pixel into a fresh SoA.  `residualT`/`residualRadiusPx`
// default to nullptr so the many callers that don't care about the residual
// need no change; pass real pointers to inspect it.
SampleSoA flattenOnePixel(const FlattenParams& fp, const DepthBuckets& b,
                          int x, int y, std::vector<SampleRecord> samples,
                          float* residualT = nullptr, float* residualRadiusPx = nullptr)
{
    SampleSoA soa;
    soa.begin(fp.channelCount, fp.groups);
    FlattenScratch scratch;
    flattenPixelToSoA(fp, b, x, y, samples, scratch, soa, nullptr,
                      residualT, residualRadiusPx);
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
                        depth));
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
// SoA flatten
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
    // THE AREA CLAIM, both limbs.  Without these two fixtures the reference's
    // step 7 is never reached at all -- deleting it outright leaves the whole
    // suite green, so it would certify nothing.  Both pairs sit in the rig's
    // single [10, 100] bucket.
    //
    // (a) DIFFERENT kernels: z=15 is 0.80px (LUT entry 2) and z=50 is 1.91px
    //     (entry 4), while both sit at bucketOf() index 14 -- so the merge may
    //     not take them and the trailing one must arrive as CO-LOCATED area
    //     rather than claiming the pixel a second time.
    fixtures.push_back({"two point samples in one bucket at DIFFERENT disc sizes (the claim)",
        {makeSample(15.0f, 15.0f, 0.55f, {0.11f, 0.22f, 0.33f}),
         makeSample(50.0f, 50.0f, 0.65f, {0.13f, 0.26f, 0.39f})}});
    // (b) THE SAME kernel across FragmentKind, so the merge may not take them
    //     either -- and here both claims must STAND, because the two cover the
    //     identical destination area and C_k : D_k has no geometry to describe.
    fixtures.push_back({"a point and a span piece in one bucket at ONE disc size (the claim)",
        {makeSample(60.0f, 60.0f, 0.6f, {0.12f, 0.24f, 0.36f}),
         makeSample(61.0f, 70.0f, 0.4f, {0.08f, 0.16f, 0.24f})}});

    // holdoutConnected is swept too: it gates how far BOTH merges may reach in
    // depth, and the reference re-derives the bracket from
    // makeUniformHoldoutBoundaries() independently of the flatten's cache.
    for (bool holdoutConnected : {false, true})
    for (bool preMerge : {false, true}) {
        for (const Fixture& fx : fixtures) {
            CAPTURE(fx.name);
            CAPTURE(preMerge);
            CAPTURE(holdoutConnected);

            FlattenParams fp = makeFlattenParams(p, C, preMerge);
            fp.holdoutConnected = holdoutConnected;
            const SampleSoA soa = flattenOnePixel(fp, bk, 11, 23, fx.samples);
            const std::vector<RefFragment> want =
                refFlatten(p, bk, 11, 23, fx.samples, preMerge, fp.mergeTolerancePx, C,
                           holdoutConnected);

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
                CHECK(fragmentDepositsArea0Of(soa.flags[i]) == w.depositArea0);
                CHECK(fragmentDepositsArea1Of(soa.flags[i]) == w.depositArea1);

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
    // "tidy + sharp-path = sequential over".  Two coincident point samples at
    // alpha 0.3 and 0.4 are ONE surface pair, so
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
    // This identity does not discriminate between bucket composites at all: a
    // sharp fragment's whole weight lands in one pixel.
    Band band;
    band.K = bk.bucketCount(); band.C = 1; band.W = W; band.H = H;
    HoldoutSoA noHoldout;
    runBand(band, makeScatterParams(W, H), soa, noHoldout, lut);

    // A sharp fragment deposits weight 1 into its own pixel, so the band
    // integral IS that pixel and it must be the sequential `over`.
    CHECK(std::fabs(bandAlphaSum(band) - expectedAlpha) <= 1e-6);
    CHECK(std::fabs(bandColorSum(band, 0) - expectedColor) <= 1e-6);
    CHECK(std::fabs(static_cast<double>(band.outAlpha(16, 16)) - expectedAlpha) <= 1e-6);
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
    // The node's depth-range pass has to apply this correction IDENTICALLY:
    // the two passes disagreeing puts every corner-pixel sample below
    // depthMin.  It is per PIXEL, always shrinks the depth, and is the
    // identity on the optical axis; unpinned, dropping it entirely is
    // invisible.
    const CocParams    p  = makeStandardRig(10.0f);
    const DepthBuckets bk = makeStandardBuckets(p);

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
        flattenPixelToSoA(fp, bk, 0, 0, v, scratch, soa, nullptr, nullptr, nullptr);
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
        scatterOnThread(makeScatterParams(W, H),
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
                std::vector<std::pair<int, int>> headClaims;   // (bucket, kernel bin)
                for (std::size_t i = 0; i < soa.fragmentCount(); ++i) {
                    if (!fragmentCoverageHeadOf(soa.flags[i]))
                        continue;
                    ++heads;
                    headClaims.emplace_back(static_cast<int>(soa.bucketIndex0[i]),
                                            refKernelBin(soa.radius[i]));
                }

                // Never more than one head per parent -- an over-count here
                // is a K-times coverage inflation.  It can be
                // FEWER when pre-merge absorbs two heads into one deposit,
                // which is loss-free (same bucket, same radius, same area).
                REQUIRE(heads <= liveParents);
                REQUIRE(heads >= 1);

                // The invariant is NOT `!preMerge => heads == liveParents`:
                // it is not the parent count that bounds the heads.  What claimNewArea
                // establishes instead is that within one bucket at one pixel,
                // EVERY head belongs to the SAME kernel.  Two depth-disjoint
                // parents at ONE pixel can land in one bucket at two different
                // disc sizes; the area planes model exactly that case (C_k
                // against D_k), so the second, differently-sized one must NOT
                // claim new area -- that double claim is what the composite
                // clamps into a fully opaque bucket (1.000 against a true
                // 0.781).
                //
                // WHY IT IS NOT "ONE HEAD PER BUCKET".
                // Two deposits sharing a bucket AND a kernel cover the IDENTICAL
                // destination area, so calling one of them "co-located" is not a
                // statement about geometry, and the composite's C_k : D_k split
                // then reads `a - a^2/4` where the truth is `a1 + a2 - a1*a2` --
                // short by ((a1-a2)/2)^2, and by a flat 0.25 once the additive
                // alpha saturates (an opaque span piece over an opaque point at
                // one pixel read 0.750000 against a true 1.0, where the double
                // claim reads the exact 1.0).  Those collisions are the MERGE's
                // to resolve, and where the merge may not reach them (across
                // FragmentKind, or across a holdout bracket) both claims stand
                // and the bucket degrades to a plain `over`, which
                // "coverage is clamped at USE, not in the plane" already
                // provides for.  Knob on or off either way.
                std::sort(headClaims.begin(), headClaims.end());
                for (std::size_t h = 1; h < headClaims.size(); ++h) {
                    const bool twoKernelsInOneBucket =
                        (headClaims[h].first == headClaims[h - 1].first)
                        && (headClaims[h].second != headClaims[h - 1].second);
                    REQUIRE_FALSE(twoKernelsInOneBucket);
                }
            }
        }
    }
}

// ===========================================================================
// Same-pixel bucket collisions
//
// THE INVARIANT: within one source pixel, deposits landing in one bucket are
// `over`-composited rather than added, and the pixel's area is claimed at most
// ONCE per bucket.  Both halves are easy to break for two same-pixel fragments
// whose bucketOf() assignments overlap while their CONTAINING buckets differ
// (so the pre-merge never groups them): the alpha plane then adds them and the
// new-area plane holds 2.0, the composite clamps both into [0,1], and the
// bucket reads fully opaque.
// ===========================================================================

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
CocParams makeManualRig(float sizePx, float focusDistance)
{
    return makeCocParams(CocMode::Manual, 50.0f, 2.8f, 36.0f, focusDistance,
                         unitScale(WorldUnits::Meters), 1920.0f, 1.0f,
                         1.0f, 1.0f, /*maxRadiusPx*/ 100.0f, sizePx);
}

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
    const DepthBuckets bk = makeStandardBuckets(p, 16);

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
            const SampleSoA soa = flattenOnePixel(fp, bk, 0, 0, v, &residualT, &residualR);
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
            const SampleSoA soa = flattenOnePixel(fp, bk, 0, 0, v, &residualT, &residualR);
            REQUIRE(residualT >= 0.0f);
            CHECK(std::fabs(shareSum(soa) + static_cast<double>(residualT) - 1.0) <= 1e-6);
        }
    }

    SUBCASE("pre-merged stacks")
    {
        // A generous tolerance and a tight depth cluster inside one WIDE
        // (K=4) containing bucket: pre-merge groups these aggressively.  The
        // per-iteration merge is not REQUIRE'd (a straddling cluster could
        // occasionally spill across a bucket boundary); instead the whole
        // subcase is checked to have exercised the merge at least once, so
        // the fuzz is not vacuous.
        const DepthBuckets bk4 = makeBoundedDeltaCocBuckets(p, 1.0f, 100.0f, 4);
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
            const SampleSoA soa = flattenOnePixel(fp, bk4, 0, 0, v, &residualT, &residualR);
            REQUIRE(residualT >= 0.0f);
            if (soa.fragmentCount() < static_cast<std::size_t>(n))
                mergedAtLeastOnce = true;
            CHECK(std::fabs(shareSum(soa) + static_cast<double>(residualT) - 1.0) <= 1e-6);
        }
        CHECK(mergedAtLeastOnce);
    }

    SUBCASE("same-pixel-collision stacks")
    {
        // Straddle the focal plane at K=8 (the standard rig's front/back
        // containing-bucket boundary sits exactly there, per the pinned
        // two-sample collision case elsewhere in this file): every sample
        // stays on the sharp path (one scatterKernelBin for all of them,
        // unconditionally -- see scatterKernelBin) while landing in
        // DIFFERENT containing buckets, which is what the deposit-collision
        // merge (not pre-merge) exists for.
        const DepthBuckets bk8 = makeBoundedDeltaCocBuckets(p, 1.0f, 100.0f, 8);
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
            const SampleSoA soa = flattenOnePixel(fp, bk8, 0, 0, v, &residualT, &residualR);
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
    // Exactly the "two-sample collision" rig used elsewhere in this file: z =
    // 9.063 / 11.039 at K = 8 straddle the front/back containing-bucket
    // boundary (the focal plane) while sharing the sharp-path kernel, so the
    // deposit-collision merge folds them into ONE emitted fragment -- NOT
    // pre-merge, whose own grouping predicate requires the SAME containing
    // bucket, which these do not share.
    const CocParams    p  = makeStandardRig(10.0f);
    const DepthBuckets bk = makeBoundedDeltaCocBuckets(p, 1.0f, 100.0f, 8);

    for (bool preMerge : {false, true}) {
        CAPTURE(preMerge);
        const FlattenParams fp = makeFlattenParams(p, 1, preMerge);
        std::vector<SampleRecord> v{makeSample(9.063f, 9.063f, 0.4667f, {0.4667f * 0.25f}),
                                    makeSample(11.039f, 11.039f, 0.5899f, {0.5899f * 0.75f})};
        SampleSoA soa;
        soa.begin(1, fp.groups);
        FlattenScratch scratch;
        flattenPixelToSoA(fp, bk, 4, 4, v, scratch, soa, nullptr, nullptr, nullptr);

        REQUIRE(scratch.stagedCount == 2u);      // two fragments were staged...
        REQUIRE(soa.fragmentCount() == 1u);      // ...and the collision merged them

        // The raw, pre-collision shares -- computed once in the flatten's
        // step 4, before either merge ever runs -- summed the exact same way
        // (a single float addition) the collision merge itself sums them.
        const float expected = scratch.staged[0].share + scratch.staged[1].share;
        CHECK(soa.arrivalShare[0] == expected);   // bit-exact, not approximate
    }
}

TEST_CASE("residualRadiusPx is the deepest STAGED fragment's own radius, for point, "
          "volumetric-split and pre-merged stacks")
{
    const CocParams    p  = makeStandardRig(10.0f);
    const DepthBuckets bk = makeStandardBuckets(p, 16);

    SUBCASE("point stack")
    {
        const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ false);
        std::vector<SampleRecord> v{makeSample(3.0f, 3.0f, 0.4f, {0.2f}),
                                    makeSample(20.0f, 20.0f, 0.6f, {0.3f})};   // deepest
        SampleSoA soa;
        soa.begin(1, fp.groups);
        FlattenScratch scratch;
        float residualT = -1.0f, residualR = -1.0f;
        flattenPixelToSoA(fp, bk, 0, 0, v, scratch, soa, nullptr, &residualT, &residualR);

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
        // One span crossing several bucket boundaries, well inside [1, 100].
        // The standard rig's back range [10, 100] is a SINGLE bucket (15
        // front / 1 back at focus 10), so the span must sit in front of
        // focus to actually cross more than one.
        std::vector<SampleRecord> v{makeSample(2.0f, 8.0f, 0.7f, {0.35f})};
        SampleSoA soa;
        soa.begin(1, fp.groups);
        FlattenScratch scratch;
        float residualT = -1.0f, residualR = -1.0f;
        flattenPixelToSoA(fp, bk, 0, 0, v, scratch, soa, nullptr, &residualT, &residualR);

        REQUIRE(scratch.stagedCount >= 2u);      // the span really did split
        CHECK(residualR == scratch.staged[scratch.stagedCount - 1].radius);

        // Cross-check: the tail part's own [zFront, zBack] via the
        // independent span-split reference, at its own mid-depth.
        const std::vector<RefPart> parts = refSplitSpan(bk, 2.0, 8.0, 0.7);
        REQUIRE(!parts.empty());
        const RefPart& tail = parts.back();
        const double   tailMid = tail.zFront + 0.5 * (tail.zBack - tail.zFront);
        CHECK(residualR == doctest::Approx(refRadiusPx(p, tailMid)).epsilon(1e-4));
    }

    SUBCASE("pre-merged stack")
    {
        // Two point samples close enough in depth to land in one (wide,
        // K = 4) containing bucket and inside a generous merge tolerance, so
        // pre-merge folds them into ONE emitted fragment -- whose own radius
        // (the union midpoint) residualRadiusPx must NOT report.
        const DepthBuckets bk4 = makeBoundedDeltaCocBuckets(p, 1.0f, 100.0f, 4);
        const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ true, /*tol*/ 5.0f);
        std::vector<SampleRecord> v{makeSample(50.0f, 50.0f, 0.4f, {0.2f}),
                                    makeSample(50.3f, 50.3f, 0.5f, {0.25f})};  // deepest
        SampleSoA soa;
        soa.begin(1, fp.groups);
        FlattenScratch scratch;
        float residualT = -1.0f, residualR = -1.0f;
        flattenPixelToSoA(fp, bk4, 0, 0, v, scratch, soa, nullptr, &residualT, &residualR);

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

TEST_CASE("an empty pixel leaves residualT at 1 and does not touch residualRadiusPx")
{
    const CocParams    p  = makeStandardRig(10.0f);
    const DepthBuckets bk = makeStandardBuckets(p, 16);
    const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ true);

    SUBCASE("no samples at all")
    {
        SampleSoA soa;
        soa.begin(1, fp.groups);
        FlattenScratch scratch;
        std::vector<SampleRecord> v;
        float residualT = -1.0f, residualR = 12345.0f;
        flattenPixelToSoA(fp, bk, 0, 0, v, scratch, soa, nullptr, &residualT, &residualR);
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
        flattenPixelToSoA(fp, bk, 0, 0, v, scratch, soa, nullptr, &residualT, &residualR);
        CHECK(residualT == 1.0f);
        CHECK(residualR == 12345.0f);
        CHECK(soa.fragmentCount() == 0u);
    }
}

TEST_CASE("size-0 flatten is a DeepToImage `over` of the pixel, at every K and both pre_merge "
          "states")
{
    // THE HEADLINE GATE.  Without the collision pass this corpus reads a worst
    // |d alpha| of 2.47e-01 with 5.7-11.8% of pixels wrong by more than 1e-3;
    // the defect is bimodal, so a spot check of a few pixels reads "3-5 ULP"
    // and misses it entirely.  Hence a corpus, and hence both a TAIL and a
    // rate assertion.
    const int C = 3, W = 64, H = 64, N = 900;

    for (bool preMerge : {false, true}) {
        for (int K : {4, 8, 16, 64}) {
            for (int spp : {2, 3, 5, 12}) {
                CAPTURE(preMerge);
                CAPTURE(K);
                CAPTURE(spp);

                const CocParams    p  = makeManualRig(0.0f, 10.0f);
                const DepthBuckets bk = makeBoundedDeltaCocBuckets(p, 1.0f, 100.0f, K);
                const FlattenParams fp = makeFlattenParams(p, C, preMerge);

                Lcg rng(0x7131u + static_cast<std::uint32_t>(K * 131 + spp * 7
                                                             + (preMerge ? 1 : 0)));
                std::vector<std::vector<SampleRecord>> pixels(
                    static_cast<std::size_t>(N));

                SampleSoA soa;
                soa.begin(C, fp.groups);
                FlattenScratch scratch;
                for (int i = 0; i < N; ++i) {
                    std::vector<SampleRecord>& v = pixels[static_cast<std::size_t>(i)];
                    float z = rng.range(1.05f, 20.0f);
                    for (int s = 0; s < spp; ++s) {
                        const float a = rng.range(0.02f, 1.0f);
                        v.push_back(makeSample(z, z, a,
                            {a * rng.unit(), a * rng.unit(), a * rng.unit()}));
                        z += rng.range(0.05f, 6.0f);    // strictly disjoint depths
                    }
                    std::vector<SampleRecord> copy = v;
                    flattenPixelToSoA(fp, bk, i % W, i / W, copy, scratch, soa, nullptr, nullptr, nullptr);
                }

                Band band;
                band.K = K; band.C = C; band.W = W; band.H = H;
                HoldoutSoA noHoldout;
                DiscKernelLUT kernel(0.0f, 1.0f, 1.0f, 1.0f);
                runBand(band, makeScatterParams(W, H),
                        soa, noHoldout, kernel, /*useThread*/ false);

                double worstAlpha = 0.0, worstColor = 0.0;
                int    bad = 0;
                for (int i = 0; i < N; ++i) {
                    const RefOver r = refSequentialOver(pixels[static_cast<std::size_t>(i)], C);
                    const int px = i % W, py = i / W;
                    const double da =
                        std::fabs(static_cast<double>(band.outAlpha(px, py)) - r.alpha);
                    double dc = 0.0;
                    for (int c = 0; c < C; ++c)
                        dc = std::max(dc, std::fabs(static_cast<double>(band.outColor(c, px, py))
                                                    - r.color[static_cast<std::size_t>(c)]));
                    worstAlpha = std::max(worstAlpha, da);
                    worstColor = std::max(worstColor, dc);
                    if (da > 1e-3 || dc > 1e-3)
                        ++bad;
                }

                // NOT A ULP BOUND, and deliberately not one: the residual is a
                // float `over` chain against a double reference, so it grows
                // with the sample count (measured 1.3e-07 at 2 samples,
                // 2.5e-07 at 20 — the same growth recorded for coincident
                // samples elsewhere).  4e-07 is
                // ~1.6x the worst measured over this corpus; the defect this
                // pins is five orders of magnitude larger.
                CHECK(worstAlpha <= 4e-07);
                CHECK(worstColor <= 6e-07);
                // The rate clause the tail alone would not catch.
                CHECK(bad == 0);
            }
        }
    }
}

TEST_CASE("scatterKernelBin mirrors the scatter's own two radius decisions, at the edges")
{
    // The merge is only lossless when the two members fetch LITERALLY the same
    // kernel, so this predicate has to agree with the scatter at both of the
    // scatter's decision points and not merely near them.  Both edges survive
    // a mutation set unless a case exercises them exactly.
    //
    // 1. THE SHARP THRESHOLD.  scatterBandCPU() takes the sharp path for
    //    `!(radius >= sharpRadiusPx)`, so radius == kSharpRadiusPx exactly is a
    //    DISC, not a sharp fragment -- a `>` here would call it sharp and then
    //    refuse to merge it with the disc beside it that the scatter rasterises
    //    identically.
    CHECK(refKernelBin(kSharpRadiusPx) >= 0);
    CHECK(scatterKernelBin(kSharpRadiusPx) == refKernelBin(kSharpRadiusPx));
    CHECK(scatterKernelBin(std::nextafter(kSharpRadiusPx, 0.0f)) == -1);
    CHECK(scatterKernelBin(0.0f) == -1);
    CHECK(sameScatterKernel(0.0f, std::nextafter(kSharpRadiusPx, 0.0f)));
    // ...and the exactly-0.5 fragment must merge with the one a hair above it,
    // because DiscKernelLUT rounds both onto grid node 1.  NOTE:
    // a uniform 0.5px grid would put 0.6f on node 1 too; on this grid node 1
    // is 0.5 and node 2 is 0.5004888, so 0.6 is ~200 nodes away and must NOT
    // merge -- 0.5 and 0.6 rasterise measurably different discs.
    CHECK(sameScatterKernel(kSharpRadiusPx, 0.50024f));
    CHECK_FALSE(sameScatterKernel(kSharpRadiusPx, 0.6f));
    CHECK_FALSE(sameScatterKernel(std::nextafter(kSharpRadiusPx, 0.0f), 0.50024f));

    // 2. NaN and +-inf.  NaN is sharp in the scatter (the test is negated), and
    //    an infinite radius must not reach std::lround, whose result there is
    //    unspecified -- the LUT clamps to its last entry instead.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    CHECK(scatterKernelBin(nan) == -1);
    CHECK(sameScatterKernel(nan, nan));
    CHECK(sameScatterKernel(inf, inf));
    CHECK(scatterKernelBin(inf) == scatterKernelBin(2.0e6f));
    CHECK_FALSE(sameScatterKernel(inf, 1.0f));
    CHECK(scatterKernelBin(-1.0f) == -1);

    // 3. The grid itself: same node merges, adjacent nodes do not -- in BOTH
    //    of its regions, since the grid is piecewise.
    //    Fine region (hyperbolic, node spacing ~r^2/512): nodes 926/927 are
    //    5.171717/5.224490 px, so 5.16 and 5.19 share node 926 while 5.20
    //    rounds to 927.
    CHECK(sameScatterKernel(5.16f, 5.19f));
    CHECK_FALSE(sameScatterKernel(5.16f, 5.20f));
    //    Coarse region (uniform 0.5px, unchanged, at and above 16px).
    CHECK(sameScatterKernel(20.10f, 20.20f));
    CHECK_FALSE(sameScatterKernel(20.10f, 20.60f));
    //    ...and the two regions join without a gap or an overlap.
    CHECK(scatterKernelBin(kKernelCoarseFromPx) == kKernelFineLastIndex);
    CHECK(kernelGridRadius(kKernelFineLastIndex) == kKernelCoarseFromPx);
    CHECK(kernelGridRadius(kKernelFineLastIndex + 1)
          == doctest::Approx(kKernelCoarseFromPx + 0.5f));
}

// Centre-ROW weight sum of one LUT entry, S_r(0).  This is the quantity the
// adjacent-bin trough is made of: two vertically adjacent source scanlines
// that fall in different kernel bins leave their shared destination row short
// by exactly (S_r(0) - S_r'(0))/2, derivable from the LUT alone and matched in
// Nuke to six decimals at four crossings.
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
    // coarse one.  Measured worst 1.256580e-03 at r=1.7415; gated at 1.4e-03,
    // which is still 2.8x inside the 1/255 = 3.92e-03 visibility gate the
    // harness uses.
    double worst = 0.0;
    float  worstAt = 0.0f;
    for (int i = 0; i + 1 < lut.entryCount(); ++i) {
        const double d = (centreRowSum(lut, lut.entryRadius(i))
                          - centreRowSum(lut, lut.entryRadius(i + 1))) * 0.5;
        if (d > worst) { worst = d; worstAt = lut.entryRadius(i); }
    }
    CAPTURE(worst);
    CAPTURE(worstAt);
    CHECK(worst < 1.4e-03);
    CHECK(worst < (1.0 / 255.0) / 2.5);
    // ...and it really is the measured value, not merely small: a grid that
    // over-refined would also pass the bound above while costing memory.
    CHECK(worst == doctest::Approx(1.256580e-03).epsilon(1e-3));

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

TEST_CASE("the area claim is per PIXEL and survives a degenerate bucket set")
{
    SUBCASE("the front-most fragment at every source pixel always claims its area")
    {
        // The claim state is stamped, not cleared: `claimStamp[k] == claimEpoch`
        // means "claimed during THIS pixel".  If the epoch stopped advancing per
        // pixel the marks would leak across pixels and a later pixel's FIRST
        // fragment could be denied a claim it must always get.  Nothing else in
        // the suite pins that, and it survived the implementer's mutation set.
        const int W = 16, H = 16;
        Lcg rng(0x9E37u);
        for (float sizePx : {0.0f, 9.0f}) {
            const CocParams p = makeManualRig(sizePx, 7.0f);
            for (int K : {4, 16}) {
                CAPTURE(sizePx);
                CAPTURE(K);
                const DepthBuckets bk = makeBoundedDeltaCocBuckets(p, 1.0f, 100.0f, K);
                const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ false);

                SampleSoA soa;
                soa.begin(1, fp.groups);
                FlattenScratch scratch;
                for (int y = 0; y < H; ++y)
                    for (int x = 0; x < W; ++x) {
                        std::vector<SampleRecord> v;
                        float z = rng.range(1.05f, 40.0f);
                        const int n = rng.intRange(1, 6);
                        for (int s = 0; s < n; ++s) {
                            const float a = rng.range(0.05f, 1.0f);
                            v.push_back(makeSample(z, z, a, {a * 0.5f}));
                            z += rng.range(0.05f, 6.0f);
                        }
                        flattenPixelToSoA(fp, bk, x, y, v, scratch, soa, nullptr, nullptr, nullptr);
                    }

                int lastX = -12345, lastY = -12345, firsts = 0;
                for (std::size_t i = 0; i < soa.fragmentCount(); ++i) {
                    if (soa.x[i] == lastX && soa.y[i] == lastY)
                        continue;
                    lastX = soa.x[i];
                    lastY = soa.y[i];
                    ++firsts;
                    REQUIRE(fragmentCoverageHeadOf(soa.flags[i]));
                }
                CHECK(firsts == W * H);
            }
        }
    }

    SUBCASE("the cached holdout boundary set follows the bucket set it was derived from")
    {
        // FlattenScratch caches makeUniformHoldoutBoundaries()'s output on the
        // three numbers it is derived from (range min, range max, boundary
        // count).  A scratch is reused across cooks and across bucket sets, so a
        // cache that only ever builds once would keep indexing the FIRST set --
        // exactly the "a build and a lookup drift onto different arrays"
        // failure HoldoutSoA's structure exists to make impossible.  Driven by
        // reusing one scratch across two genuinely different bucket sets and
        // comparing against a fresh one.
        const CocParams    p  = makeManualRig(0.0f, 4.0f);
        const DepthBuckets bkA = makeBoundedDeltaCocBuckets(p, 1.0f, 100.0f, 16);
        const DepthBuckets bkB = makeBoundedDeltaCocBuckets(p, 1.0f, 8.0f, 4);
        FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ false);
        fp.holdoutConnected = true;

        // Under set B these two share bucketOf() index 2 and one (sharp) kernel
        // but sit in DIFFERENT holdout brackets (1 and 2 of [1,8]/4), so the
        // merge must not take them.  Under set A's much coarser brackets
        // ([1,100]/16) they share bracket 0 and WOULD merge — which is exactly
        // what a cache that never rebuilds would do.
        const std::vector<SampleRecord> pixel{
            makeSample(3.0f, 3.0f, 0.5f, {0.5f}),
            makeSample(5.0f, 5.0f, 0.5f, {0.5f})};

        // Warm the cache on set A, then flatten set B through the same scratch.
        SampleSoA reused;
        reused.begin(1, fp.groups);
        FlattenScratch shared;
        {
            std::vector<SampleRecord> v = pixel;
            SampleSoA throwaway;
            throwaway.begin(1, fp.groups);
            flattenPixelToSoA(fp, bkA, 0, 0, v, shared, throwaway, nullptr, nullptr, nullptr);
        }
        {
            std::vector<SampleRecord> v = pixel;
            flattenPixelToSoA(fp, bkB, 0, 0, v, shared, reused, nullptr, nullptr, nullptr);
        }

        const SampleSoA fresh = flattenOnePixel(fp, bkB, 0, 0, pixel);
        REQUIRE(reused.fragmentCount() == fresh.fragmentCount());
        for (std::size_t i = 0; i < fresh.fragmentCount(); ++i) {
            CHECK(reused.depth[i] == fresh.depth[i]);
            CHECK(reused.alpha[i] == fresh.alpha[i]);
            CHECK(reused.bucketIndex0[i] == fresh.bucketIndex0[i]);
        }
    }

    SUBCASE("the stamp epoch wrapping does not turn every bucket into a stale claim")
    {
        // `claimStamp[k] == claimEpoch` means "claimed during this pixel", and
        // the epoch is a uint32 bumped per pixel.  A freshly sized array is all
        // zeros, so epoch 0 must never be live -- on wrap the marks are retired
        // and the epoch skips 0.  Without that, the first pixel after the wrap
        // sees EVERY bucket as already claimed by kernel bin 0 and a sharp
        // fragment (bin -1) is denied the claim it must always get.  Four
        // billion pixels is not a unit test, so the epoch is driven straight to
        // its last value instead.
        const CocParams    p  = makeManualRig(0.0f, 10.0f);
        const DepthBuckets bk = makeBoundedDeltaCocBuckets(p, 1.0f, 100.0f, 16);
        const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ false);

        SampleSoA soa;
        soa.begin(1, fp.groups);
        FlattenScratch scratch;
        scratch.claimEpoch = 0xFFFFFFFFu;
        // There is a SECOND stamped record beside the claim -- the per-bucket
        // touch/running-alpha the attenuation reads -- and the wrap must
        // retire BOTH.
        //
        // The marks are poisoned with the epoch the counter will hold AFTER the
        // wrap (1, since 0 is skipped), which is what a mark left behind four
        // billion pixels ago actually looks like when the counter comes back
        // round to it.  Poisoning them with the PRE-wrap epoch tests nothing at
        // all -- 0xFFFFFFFF never equals 1 -- and both retirement lines survive
        // deletion under that setup.
        //
        // The two records are poisoned with DIFFERENT bins on purpose, because
        // they fail in opposite directions.  A stale TOUCH record bites when it
        // matches the incoming kernel, so `runBin` gets -1, the sharp kernel the
        // fragment below actually uses (scatterKernelBin() returns -1 for the
        // sharp path and >= 1 for every disc; a default-constructed 0 is not a
        // bin any fragment can have, and poisoning with it would test nothing at
        // all).  A stale CLAIM bites when it does NOT match, so `claimBin` gets
        // a disc bin: the fragment below would then be denied the new-area claim
        // its own pixel must always give it.  With a stale touch the fragment is
        // attenuated by a full running alpha and deposits nothing.
        scratch.claimStamp.assign(static_cast<std::size_t>(bk.bucketCount()), 1u);
        scratch.claimBin.assign(static_cast<std::size_t>(bk.bucketCount()), 3);
        scratch.runStamp.assign(static_cast<std::size_t>(bk.bucketCount()), 1u);
        scratch.runBin.assign(static_cast<std::size_t>(bk.bucketCount()), -1);
        scratch.runAlpha.assign(static_cast<std::size_t>(bk.bucketCount()), 1.0f);
        for (int i = 0; i < 3; ++i) {
            std::vector<SampleRecord> v{makeSample(5.0f, 5.0f, 0.5f, {0.5f})};
            flattenPixelToSoA(fp, bk, i, 0, v, scratch, soa, nullptr, nullptr, nullptr);
        }
        REQUIRE(soa.fragmentCount() == 3u);
        for (std::size_t i = 0; i < soa.fragmentCount(); ++i) {
            CHECK(fragmentCoverageHeadOf(soa.flags[i]));
            CHECK(fragmentDepositsArea0Of(soa.flags[i]));
            // Nothing attenuated: the fragment deposits its own alpha.
            CHECK(std::fabs(static_cast<double>(soa.alpha[i]) - 0.5) <= 1e-06);
            CHECK(soa.bucketAlpha0[i] > 0.0f);
        }
    }

    SUBCASE("an inert bucket set does not silently strip every coverage head")
    {
        // A default-constructed DepthBuckets has bucketCount() == 0, so every
        // bucket index is out of range.  The claim must be GRANTED there rather
        // than denied: denying it would leave the coverage plane empty and the
        // composite with nothing to attach alpha to.
        const CocParams p = makeManualRig(0.0f, 10.0f);
        const DepthBuckets inert;
        REQUIRE(inert.bucketCount() == 0);
        const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ false);
        const SampleSoA soa = flattenOnePixel(fp, inert, 2, 3,
            {makeSample(5.0f, 5.0f, 0.5f, {0.5f})});
        REQUIRE(soa.fragmentCount() == 1u);
        CHECK(fragmentCoverageHeadOf(soa.flags[0]));
    }
}

// ===========================================================================
// Per-bucket transmittance attenuation at the flatten
//
// THE INVARIANT: within one source pixel, deposits of the SAME kernel landing
// in one bucket are `over`-composited rather than added, they claim that
// bucket's area exactly once, and no deposit ever lands in FRONT of a bucket an
// earlier (nearer) same-kernel deposit already reached.  Together those make
// the bucket composite reproduce the pixel's flatten for ANY mixture of point
// and volumetric content, with or without a holdout connected — including the
// three holes the collision merge alone cannot close (it may not merge across
// FragmentKind, may not merge across a holdout bracket, and cannot take a
// same-kernel collision it is not adjacent to).
// ===========================================================================

namespace {

// An "occludes nothing" holdout: one opaque sample far behind every fixture in
// this file.  The TRUTH is therefore unchanged by connecting it, which is
// exactly what makes it a parity gate — connecting input 1 switches the
// collision merge off across holdout brackets, and without the attenuation the
// same corpus reads 2.30e-01.
void buildFarHoldout(const DepthBuckets& bk, std::ptrdiff_t pixelCount,
                     float depth, HoldoutSampleSoA& hs, HoldoutLut& lut)
{
    hs.begin(pixelCount);
    for (std::ptrdiff_t i = 0; i < pixelCount; ++i) {
        std::vector<SampleRecord> hv{makeSample(depth, depth, 1.0f, {})};
        hs.appendPixel(hv, 1.0f);
    }
    lut.build(hs, makeUniformHoldoutBoundaries(bk));
}

} // namespace

TEST_CASE("size-0 flatten is a DeepToImage `over` for MIXED point+volumetric content, with a "
          "holdout connected and without")
{
    // THE HEADLINE GATE, including the two rows the collision merge alone
    // cannot reach.  Measured on this corpus without the attenuation:
    //   mixed,  holdout off : worst |d alpha| 2.61e-01, |d colour| 8.30e-01,
    //                         99.8% of a row's pixels beyond 1e-3
    //   mixed,  holdout on  : 2.64e-01 / 8.84e-01 / 99.9%
    //   point,  holdout on  : 2.30e-01 / 5.60e-01 / 99.0%
    //   span,   holdout on  : 2.94e-01 / 5.91e-01 / 99.2%
    // and with it: 4.83e-07 / 4.24e-07 / 0.00% everywhere below.
    const int C = 3, W = 32, H = 32, N = 400;

    for (int content = 0; content < 3; ++content)      // 0 point, 1 span, 2 mixed
    for (bool holdout : {false, true})
    for (bool preMerge : {false, true})
    for (int K : {4, 8, 16, 32, 64, 128}) {
        const int spp = (K == 16) ? 20 : 5;            // the tail count, once per K
        CAPTURE(content);
        CAPTURE(holdout);
        CAPTURE(preMerge);
        CAPTURE(K);
        CAPTURE(spp);

        const CocParams    p  = makeManualRig(0.0f, 10.0f);
        const DepthBuckets bk = makeBoundedDeltaCocBuckets(p, 1.0f, 100.0f, K);
        FlattenParams fp = makeFlattenParams(p, C, preMerge);
        fp.holdoutConnected = holdout;

        Lcg rng(0x7131u + static_cast<std::uint32_t>(K * 131 + spp * 7
                                                     + (preMerge ? 1 : 0)
                                                     + content * 977));
        std::vector<std::vector<SampleRecord>> pixels(static_cast<std::size_t>(N));

        SampleSoA soa;
        soa.begin(C, fp.groups);
        FlattenScratch scratch;
        for (int i = 0; i < N; ++i) {
            std::vector<SampleRecord>& v = pixels[static_cast<std::size_t>(i)];
            float z = rng.range(1.05f, 20.0f);
            for (int s = 0; s < spp; ++s) {
                const float a   = rng.range(0.02f, 1.0f);
                const bool  vol = (content == 1) || (content == 2 && (rng.next() & 1u));
                const float th  = vol ? rng.range(0.05f, 3.0f) : 0.0f;
                v.push_back(makeSample(z, z + th, a,
                    {a * rng.unit(), a * rng.unit(), a * rng.unit()}));
                z += th + rng.range(0.05f, 6.0f);      // strictly disjoint
            }
            std::vector<SampleRecord> copy = v;
            flattenPixelToSoA(fp, bk, i % W, i / W, copy, scratch, soa, nullptr, nullptr, nullptr);
        }

        HoldoutSampleSoA hs;
        HoldoutLut       lut;
        HoldoutSoA       view;
        if (holdout) {
            buildFarHoldout(bk, static_cast<std::ptrdiff_t>(W) * H, 500.0f, hs, lut);
            view = lut.view();
            REQUIRE(view.enabled());
        }

        Band band;
        band.K = K; band.C = C; band.W = W; band.H = H;
        DiscKernelLUT kernel(0.0f, 1.0f, 1.0f, 1.0f);
        runBand(band, makeScatterParams(W, H),
                soa, view, kernel, /*useThread*/ false);

        double worstAlpha = 0.0, worstColor = 0.0;
        int    bad = 0;
        for (int i = 0; i < N; ++i) {
            const RefOver r = refSequentialOver(pixels[static_cast<std::size_t>(i)], C);
            const int px = i % W, py = i / W;
            const double da =
                std::fabs(static_cast<double>(band.outAlpha(px, py)) - r.alpha);
            double dc = 0.0;
            for (int c = 0; c < C; ++c)
                dc = std::max(dc, std::fabs(static_cast<double>(band.outColor(c, px, py))
                                            - r.color[static_cast<std::size_t>(c)]));
            worstAlpha = std::max(worstAlpha, da);
            worstColor = std::max(worstColor, dc);
            if (da > 1e-3 || dc > 1e-3)
                ++bad;
        }

        // NOT a ULP bound, and it GROWS WITH SAMPLE COUNT (a float `over`
        // chain against a double reference).  Measured worst over
        // this corpus is 5.74e-07 at 20 spp / K=128 on the volumetric rows and
        // 2.4e-07 at 5 spp; 1e-06 is ~2x that.  The defect these rows pin is
        // five orders of magnitude larger.
        CHECK(worstAlpha <= 1e-06);
        CHECK(worstColor <= 1e-06);
        CHECK(bad == 0);
    }
}

TEST_CASE("with a holdout connected, two sharp samples IN FRONT of a card read the flatten")
{
    // Two sharp samples at z=20/40 behind a card at z=95 read 0.816118 against
    // a true 0.750000 when this goes wrong: connecting the holdout switches the
    // collision merge off (the two sit in different HoldoutBoundaries
    // brackets), the two deposits are ADDED, and validation scene (b) fails the
    // way scene (a) does.  Nothing occludes them — the card is 55 units behind
    // the farther sample — so the truth is the plain flatten.
    const int W = 8, H = 8, K = 16;
    const CocParams    p  = makeManualRig(0.0f, 10.0f);
    const DepthBuckets bk = makeBoundedDeltaCocBuckets(p, 1.0f, 100.0f, K);

    // Hand-derived: 0.5 over 0.5.
    const double truth = 0.5 + 0.5 * (1.0 - 0.5);

    for (bool holdout : {false, true}) {
        CAPTURE(holdout);
        FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ true);
        fp.holdoutConnected = holdout;

        const SampleSoA soa = flattenOnePixel(fp, bk, 4, 4,
            {makeSample(20.0f, 20.0f, 0.5f, {0.5f}),
             makeSample(40.0f, 40.0f, 0.5f, {0.5f})});

        HoldoutSampleSoA hs;
        HoldoutLut       lut;
        HoldoutSoA       view;
        if (holdout) {
            buildFarHoldout(bk, static_cast<std::ptrdiff_t>(W) * H, 95.0f, hs, lut);
            view = lut.view();
        }

        Band band;
        band.K = K; band.C = 1; band.W = W; band.H = H;
        DiscKernelLUT kernel(0.0f, 1.0f, 1.0f, 1.0f);
        runBand(band, makeScatterParams(W, H),
                soa, view, kernel, /*useThread*/ false);

        CHECK(std::fabs(static_cast<double>(band.outAlpha(4, 4)) - truth) <= 2e-07);
        CHECK(std::fabs(static_cast<double>(band.outColor(0, 4, 4)) - truth) <= 2e-07);
    }
}

TEST_CASE("the per-bucket attenuation needs no holdout gate: each fragment keeps its own depth "
          "and its own vis")
{
    // The property that makes a holdout gate unnecessary here where the
    // collision MERGE needs one: the merge emits ONE fragment at ONE depth, so
    // it can carry a sample from behind a card to in front of it; the
    // attenuation moves no fragment's depth at all.  Holdout transmittance is
    // monotone in z and the attenuating fragment is in FRONT, so vis_front >=
    // vis_back always.
    //
    // The rig: an opaque card at z = 40, one sample at z = 20 (unoccluded) and
    // one at z = 60 (fully occluded), sharing a source pixel.  The pixel must
    // read the FRONT sample alone.  If the attenuation carried the rear sample
    // forward it would read 0.75; if it carried the front sample back, 0.
    const int W = 8, H = 8, K = 8;
    const CocParams    p  = makeManualRig(0.0f, 10.0f);
    const DepthBuckets bk = makeBoundedDeltaCocBuckets(p, 1.0f, 100.0f, K);

    FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ true);
    fp.holdoutConnected = true;

    const SampleSoA soa = flattenOnePixel(fp, bk, 4, 4,
        {makeSample(20.0f, 20.0f, 0.5f, {0.5f}),
         makeSample(60.0f, 60.0f, 0.5f, {0.5f})});
    // They really do collide: same pixel, same (sharp) kernel, and deposit
    // ranges that intersect -- so the attenuation is engaged here.
    REQUIRE(soa.fragmentCount() == 2u);
    REQUIRE(sameScatterKernel(soa.radius[0], soa.radius[1]));
    REQUIRE(soa.bucketIndex1[0] >= soa.bucketIndex0[1]);
    REQUIRE(soa.bucketIndex1[1] >= soa.bucketIndex0[0]);
    // ...and each kept ITS OWN depth, which is what the holdout is sampled at.
    CHECK(soa.depth[0] == 20.0f);
    CHECK(soa.depth[1] == 60.0f);

    HoldoutSampleSoA hs;
    HoldoutLut       lut;
    buildFarHoldout(bk, static_cast<std::ptrdiff_t>(W) * H, 40.0f, hs, lut);

    Band band;
    band.K = K; band.C = 1; band.W = W; band.H = H;
    DiscKernelLUT kernel(0.0f, 1.0f, 1.0f, 1.0f);
    runBand(band, makeScatterParams(W, H),
            soa, lut.view(), kernel, /*useThread*/ false);

    CHECK(std::fabs(static_cast<double>(band.outAlpha(4, 4)) - 0.5) <= 2e-06);
    CHECK(std::fabs(static_cast<double>(band.outColor(0, 4, 4)) - 0.5) <= 2e-06);
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
    const int C = 1, W = 40, H = 40, K = 16;

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
        const DepthBuckets bk = makeBoundedDeltaCocBuckets(p, 1.0f, 100.0f, K);
        const FlattenParams fp = makeFlattenParams(p, C, preMerge);

        SampleSoA soa;
        soa.begin(C, fp.groups);
        FlattenScratch scratch;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                std::vector<SampleRecord> v{makeSample(4.0f, 4.0f, a1, {a1}),
                                            makeSample(9.0f, 9.0f, a2, {a2})};
                flattenPixelToSoA(fp, bk, x, y, v, scratch, soa, nullptr, nullptr, nullptr);
            }

        Band band;
        band.K = K; band.C = C; band.W = W; band.H = H;
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

TEST_CASE("the bucket alphas MULTIPLY to the pixel's flatten: 1 - prod(1 - A_k)")
{
    // The identity the attenuation is built on, checked in the PLANES rather
    // than after the composite, so it holds independently of the bucket
    // composite:
    //     1 - prod_k (1 - A_k) = 1 - prod_k prod_i (1 - a_{i,k})
    //                          = 1 - prod_i (1 - a_i)
    // Sharp path at one pixel, so every deposit lands with weight 1 and A_k is
    // read straight off the plane.  Without the attenuation the planes ADD, so
    // this reads above the truth on every colliding pixel.
    const int W = 4, H = 4, C = 1;
    Lcg rng(0xB105u);

    for (int K : {4, 8, 16, 64})
    for (bool preMerge : {false, true})
    for (int trial = 0; trial < 60; ++trial) {
        CAPTURE(K);
        CAPTURE(preMerge);
        CAPTURE(trial);

        const CocParams    p  = makeManualRig(0.0f, 10.0f);
        const DepthBuckets bk = makeBoundedDeltaCocBuckets(p, 1.0f, 100.0f, K);
        const FlattenParams fp = makeFlattenParams(p, C, preMerge);

        std::vector<SampleRecord> v;
        double truthT = 1.0;
        float  z = rng.range(1.05f, 15.0f);
        const int n = rng.intRange(2, 6);
        for (int i = 0; i < n; ++i) {
            const float a  = rng.range(0.05f, 0.95f);
            const float th = (rng.next() & 1u) ? rng.range(0.02f, 1.0f) : 0.0f;
            v.push_back(makeSample(z, z + th, a, {a}));
            truthT *= (1.0 - static_cast<double>(a));
            z += th + rng.range(0.05f, 3.0f);
        }

        SampleSoA soa;
        soa.begin(C, fp.groups);
        FlattenScratch scratch;
        std::vector<SampleRecord> copy = v;
        flattenPixelToSoA(fp, bk, 2, 2, copy, scratch, soa, nullptr, nullptr, nullptr);

        Band band;
        band.K = K; band.C = C; band.W = W; band.H = H;
        band.planes.allocate(K, C, W, H);
        band.planes.zero();
        HoldoutSoA noHoldout;
        DiscKernelLUT kernel(0.0f, 1.0f, 1.0f, 1.0f);
        ScatterScratch ss;
        scatterBandCPU(makeScatterParams(W, H),
                       soa, noHoldout, kernel, band.planes, ss);

        double t = 1.0;
        for (int k = 0; k < K; ++k)
            t *= (1.0 - static_cast<double>(band.planeAlpha(k, 2, 2)));

        // 2e-6 absolute on the transmittance PRODUCT: a float `over` chain of up
        // to 6 samples against a double reference (measured worst 1.9e-07).
        CHECK(std::fabs(t - truthT) <= 2e-06);
    }
}

TEST_CASE("the monotone bucket frontier: a trailing deposit never lands in FRONT of an earlier "
          "one, and moves by at most ONE bucket")
{
    // The rule that keeps the front-to-back bucket composite honest: bucket k+1
    // is attenuated by the WHOLE of bucket k, so a fragment depositing into a
    // bucket an earlier same-kernel fragment already passed would put a spurious
    // factor of its own alpha onto that earlier fragment's rear deposit
    // (measured: the front layer of two alpha-0.5 samples sharing a bucketOf()
    // pair reads 0.879 of its colour against a true 1.0, and up to 8.1e-01 of
    // premultiplied colour over the mixed corpus).
    //
    // Two things are asserted, because the rule is only safe if BOTH hold: the
    // ordering it establishes, and the bound on how far it may move anything.
    const int W = 16, H = 16;
    Lcg rng(0x5AFEu);

    for (float sizePx : {0.0f, 2.0f})
    for (int K : {4, 16, 64})
    for (bool preMerge : {false, true}) {
        CAPTURE(sizePx);
        CAPTURE(K);
        CAPTURE(preMerge);

        const CocParams    p  = makeManualRig(sizePx, 10.0f);
        const DepthBuckets bk = makeBoundedDeltaCocBuckets(p, 1.0f, 100.0f, K);
        const FlattenParams fp = makeFlattenParams(p, 1, preMerge);

        SampleSoA soa;
        soa.begin(1, fp.groups);
        FlattenScratch scratch;
        std::vector<int> pixelOf;
        for (int i = 0; i < 120; ++i) {
            std::vector<SampleRecord> v;
            float z = rng.range(1.05f, 25.0f);
            const int n = rng.intRange(2, 5);
            for (int s = 0; s < n; ++s) {
                const float a  = rng.range(0.05f, 1.0f);
                const float th = (rng.next() & 1u) ? rng.range(0.02f, 2.0f) : 0.0f;
                v.push_back(makeSample(z, z + th, a, {a * 0.5f}));
                z += th + rng.range(0.02f, 4.0f);
            }
            flattenPixelToSoA(fp, bk, i % W, i / W, v, scratch, soa, nullptr, nullptr, nullptr);
        }

        int frontier = -1, lastX = -1, lastY = -1;
        for (std::size_t i = 0; i < soa.fragmentCount(); ++i) {
            const int i0 = static_cast<int>(soa.bucketIndex0[i]);
            const int i1 = static_cast<int>(soa.bucketIndex1[i]);
            if (soa.x[i] != lastX || soa.y[i] != lastY) {
                lastX = soa.x[i];
                lastY = soa.y[i];
                frontier = -1;
            }
            // THE ORDERING.  Never in front of what this pixel already reached.
            // (Fragments of DIFFERENT kernels are exempt: they cover different
            // destination areas, the area planes model that pair directly, and
            // pushing one back measured as a regression.  The review
            // reproduced that on an independent rig -- an ungated clamp reads
            // -2.08e-02 / -2.16e-02 / -2.25e-02 at K=4/8/16 on a two-layer
            // defocused field (z 15/45, alpha 0.5/0.5, Manual size 6) against a
            // shipped +1.39e-02 / +1.09e-02 / +7.87e-03 -- but NOT the
            // originally recorded +1.69e-01 at K=32, which is rig-specific.)
            if (frontier >= 0)
                CHECK(i0 >= frontier - 1);      // >= frontier-1 for the exempt case
            frontier = std::max(frontier, i1);
            // A clamped fragment took WHOLE weight, which is a legal Point
            // assignment (frac 0), never a second split.
            CHECK((i1 == i0 || i1 == i0 + 1));
        }
    }

    // ...AND NO FURTHER.  A fragment whose front bucket is exactly AT the
    // frontier is not in front of anything -- the two share one bucket, which
    // the running alpha resolves exactly -- so it must keep its fractional
    // two-bucket split.  Collapsing it as well is the whole-weight assignment
    // that defeats the K knob, and it is one character
    // away (`<` vs `<=`), so it gets a direct differential: the same sample
    // flattened ALONE and flattened behind a leading fragment that pushes the
    // frontier exactly onto its front bucket must produce the same assignment.
    {
        const CocParams    p  = makeManualRig(0.0f, 10.0f);
        const DepthBuckets bk = makeBoundedDeltaCocBuckets(p, 1.0f, 100.0f, 16);
        const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ false);

        // The pair has to be CROSS-KIND: two same-kernel POINT samples whose
        // deposits touch are collapsed by the collision merge before this rule
        // is reached, so the case only exists across the composition contract.
        // A span piece wholly
        // inside bucket k takes whole weight there and puts the frontier at k;
        // a point sample just behind bucket k's centre has bucketOf() index k
        // with a fraction, i.e. it sits EXACTLY at the frontier.
        const int   k  = 5;
        const float zSpanA = bk.boundary(k) + 0.02f * (bk.centre(k) - bk.boundary(k));
        const float zSpanB = bk.boundary(k) + 0.30f * (bk.centre(k) - bk.boundary(k));
        const float zPoint = bk.centre(k) + 0.25f * (bk.centre(k + 1) - bk.centre(k));

        const SampleSoA alone = flattenOnePixel(fp, bk, 3, 3,
            {makeSample(zPoint, zPoint, 0.5f, {0.25f})});
        const SampleSoA pair  = flattenOnePixel(fp, bk, 3, 3,
            {makeSample(zSpanA, zSpanB, 0.4f, {0.2f}),
             makeSample(zPoint, zPoint, 0.5f, {0.25f})});

        REQUIRE(alone.fragmentCount() == 1u);
        REQUIRE(pair.fragmentCount() == 2u);
        REQUIRE(fragmentKindOf(pair.flags[0]) == FragmentKind::Volumetric);
        // The premise: the leading fragment reaches the trailing one's own
        // front bucket, and no further.
        REQUIRE(pair.bucketIndex1[0] == alone.bucketIndex0[0]);
        REQUIRE(alone.bucketIndex1[0] == alone.bucketIndex0[0] + 1);
        // The conclusion: the split survives, bit for bit.
        CHECK(pair.bucketIndex0[1] == alone.bucketIndex0[0]);
        CHECK(pair.bucketIndex1[1] == alone.bucketIndex1[0]);
        CHECK(pair.bucketAlpha1[1] == alone.bucketAlpha1[0]);
        CHECK(pair.colorScale1[1]  == alone.colorScale1[0]);
    }
}

TEST_CASE("the `no area at all` deposit is REACHABLE through the flatten on BOTH deposits, and "
          "the scatter honours both bits")
{
    // `depositArea1` is a LIVE case, not a guard: it IS reachable through
    // flattenPixelToSoA().  Three independent mutants that disable the bit —
    // clearing it in the flatten, dropping the guard in
    // scatterSpanBothBuckets(), and making fragmentDepositsArea1Of() return
    // true — all pass without this case.
    //
    // WHY IT IS REACHABLE.  The frontier clamp is gated on the kernel bin, and
    // `frontierBin` is a single slot holding whichever deposit last reached the
    // frontier.  CoC radius is V-SHAPED about the focal plane, so three
    // same-pixel samples straddling focus bin as A, B, A: the middle one leaves
    // `frontierBin` on B, the third one's clamp therefore does not fire, and it
    // lands back on the bucket PAIR the first one already touched with kernel A
    // — so BOTH of its deposits take the SameKernel branch.  Measured over a
    // 900-pixel randomised corpus at Manual size 6, `pre_merge` off: 25 such
    // fragments at K=4 and 2 at K=16 (0 at size 0, where every radius is 0 and
    // there is only one bin).
    //
    // That residual is REAL but bounded: on the fixture below the pixel reads
    // 1.26e-02 against the flatten, where without the attenuation it reads
    // 2.23e-01 — and the size-0 gates cannot reach it at all (one bin).
    //
    // FIXTURE: Manual size 8, focus 10, so radius = 8*|1 - 10/z|; z = 8 / 10 /
    // 13.3333 give radii 2.0 / 0.0 / 2.0, i.e. bins 4 / -1 / 4.
    const CocParams p = makeManualRig(8.0f, 10.0f);
    const float zA = 8.0f, zB = 10.0f, zC = 13.3333333f;
    REQUIRE(scatterKernelBin(radiusPixels(p, zA)) == scatterKernelBin(radiusPixels(p, zC)));
    REQUIRE(scatterKernelBin(radiusPixels(p, zB)) != scatterKernelBin(radiusPixels(p, zA)));

    SUBCASE("K=4: the trailing fragment's BOTH deposits write no area")
    {
        const DepthBuckets bk = makeBoundedDeltaCocBuckets(p, 1.0f, 100.0f, 4);
        const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ false);
        const SampleSoA soa = flattenOnePixel(fp, bk, 3, 3,
            {makeSample(zA, zA, 0.5f,  {0.5f}),
             makeSample(zB, zB, 0.25f, {0.25f}),
             makeSample(zC, zC, 0.5f,  {0.5f})});
        REQUIRE(soa.fragmentCount() == 3u);
        // The premise: all three share one bucket PAIR, and the third was not
        // clamped (the middle one's bin left the frontier gate shut).
        REQUIRE(soa.bucketIndex0[0] == soa.bucketIndex0[2]);
        REQUIRE(soa.bucketIndex1[0] == soa.bucketIndex1[2]);
        REQUIRE(soa.bucketIndex1[2] == soa.bucketIndex0[2] + 1);
        // The conclusion, asserted on each bit SEPARATELY so that swapping the
        // two bit constants is a failure rather than a relabel.
        CHECK(fragmentDepositsArea0Of(soa.flags[0]));
        CHECK(fragmentDepositsArea1Of(soa.flags[0]));
        CHECK(fragmentDepositsArea0Of(soa.flags[1]));
        CHECK(fragmentDepositsArea1Of(soa.flags[1]));
        CHECK_FALSE(fragmentDepositsArea0Of(soa.flags[2]));
        CHECK_FALSE(fragmentDepositsArea1Of(soa.flags[2]));
        CHECK_FALSE(fragmentCoverageHeadOf(soa.flags[2]));
    }

    SUBCASE("K=16: area0 clear and area1 SET on one fragment, so the two bits are distinct")
    {
        // The two bits must not be interchangeable: here the trailing fragment's
        // front deposit collides and its rear one does not.
        const DepthBuckets bk = makeBoundedDeltaCocBuckets(p, 1.0f, 100.0f, 16);
        const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ false);
        const SampleSoA soa = flattenOnePixel(fp, bk, 3, 3,
            {makeSample(zA, zA, 0.5f,  {0.5f}),
             makeSample(zB, zB, 0.25f, {0.25f}),
             makeSample(zC, zC, 0.5f,  {0.5f})});
        REQUIRE(soa.fragmentCount() == 3u);
        CHECK_FALSE(fragmentDepositsArea0Of(soa.flags[2]));
        CHECK(fragmentDepositsArea1Of(soa.flags[2]));
    }

    SUBCASE("the SCATTER honours both bits: the area planes count the suppressed deposit once")
    {
        // The flag has to reach the planes, not merely the SoA.  Hand-derived
        // from the deposit rules and the K=4 fragment list above (each fragment's
        // kernel weights sum to exactly 1, so every deposit contributes 1.0):
        //
        //   bucket i0 : NEW AREA  = f0 only (the one coverage head)          = 1
        //               CO-LOCATED= f1 only (f0 is a head, f2 writes nothing) = 1
        //   bucket i1 : NEW AREA  = none (a rear deposit never claims)        = 0
        //               CO-LOCATED= f0 and f1 (f2 writes nothing)             = 2
        //
        // Ignoring `depositArea0` reads 2 for i0's co-located plane; ignoring
        // `depositArea1` reads 3 for i1's.
        const int W = 64, H = 64, K = 4;
        const DepthBuckets bk = makeBoundedDeltaCocBuckets(p, 1.0f, 100.0f, K);
        const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ false);
        const SampleSoA soa = flattenOnePixel(fp, bk, W / 2, H / 2,
            {makeSample(zA, zA, 0.5f,  {0.5f}),
             makeSample(zB, zB, 0.25f, {0.25f}),
             makeSample(zC, zC, 0.5f,  {0.5f})});
        REQUIRE(soa.fragmentCount() == 3u);
        const int i0 = static_cast<int>(soa.bucketIndex0[0]);
        const int i1 = static_cast<int>(soa.bucketIndex1[0]);

        Band band;
        band.K = K; band.C = 1; band.W = W; band.H = H;
        HoldoutSoA noHoldout;
        DiscKernelLUT kernel(20.0f, 1.0f, 1.0f, 1.0f);
        runBand(band, makeScatterParams(W, H),
                soa, noHoldout, kernel, /*useThread*/ false);

        const std::ptrdiff_t px = band.pixels();
        CHECK(planeSum(band.planes.weight,    i0, px) == doctest::Approx(1.0).epsilon(1e-5));
        CHECK(planeSum(band.planes.colocated, i0, px) == doctest::Approx(1.0).epsilon(1e-5));
        CHECK(planeSum(band.planes.weight,    i1, px) == doctest::Approx(0.0).epsilon(1e-5));
        CHECK(planeSum(band.planes.colocated, i1, px) == doctest::Approx(2.0).epsilon(1e-5));
    }

    SUBCASE("packFragmentFlags DEFAULTS both area bits SET")
    {
        // The header promises both area bits default to SET.  Every shipping
        // call site passes them explicitly, so only a direct assertion pins
        // that documented default.
        const std::uint8_t f = packFragmentFlags(FragmentKind::Point, /*coverageHead*/ true);
        CHECK((f & kFragmentArea0Bit) != 0);
        CHECK((f & kFragmentArea1Bit) != 0);
        CHECK(fragmentDepositsArea0Of(f));
        CHECK(fragmentDepositsArea1Of(f));
    }
}

TEST_CASE("claimNewArea() RECORDS the claiming kernel, not just the stamp")
{
    // `claimBin` is read only when the stamp matches, so failing to WRITE it
    // leaves the different-kernel test reading a bin from an arbitrary earlier
    // pixel — and the restriction it guards is load-bearing: the unrestricted
    // form punches a 25% hole in in-focus opaque geometry.  Deleting the write
    // otherwise passes the suite.
    //
    // Poisoned with the SECOND fragment's own bin, which is the direction that
    // bites: with the write in place the first fragment overwrites it with its
    // OWN bin and the second is correctly denied the claim; without it the
    // second matches the poison and takes a second head, double-claiming the
    // pixel's area in one bucket.
    const CocParams    p  = makeManualRig(8.0f, 10.0f);
    const DepthBuckets bk = makeBoundedDeltaCocBuckets(p, 1.0f, 100.0f, 4);
    const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ false);

    const float zA = 8.0f, zB = 10.0f;                  // bins 4 and -1 (sharp)
    const int   binB = scatterKernelBin(radiusPixels(p, zB));
    REQUIRE(binB != scatterKernelBin(radiusPixels(p, zA)));

    FlattenScratch scratch;
    const std::size_t n = static_cast<std::size_t>(bk.bucketCount());
    // Sized here, so ensureBucketScratch() leaves the poison alone.  The stamps
    // are 0, which is never a live epoch, so only `claimBin` is poisoned.
    scratch.claimStamp.assign(n, 0u);
    scratch.claimBin.assign(n, binB);
    scratch.runStamp.assign(n, 0u);
    scratch.runBin.assign(n, 0);
    scratch.runAlpha.assign(n, 0.0f);

    SampleSoA soa;
    soa.begin(1, fp.groups);
    std::vector<SampleRecord> v{makeSample(zA, zA, 0.5f,  {0.5f}),
                                makeSample(zB, zB, 0.25f, {0.25f})};
    flattenPixelToSoA(fp, bk, 0, 0, v, scratch, soa, nullptr, nullptr, nullptr);

    REQUIRE(soa.fragmentCount() == 2u);
    REQUIRE(soa.bucketIndex0[0] == soa.bucketIndex0[1]);      // they do collide
    CHECK(fragmentCoverageHeadOf(soa.flags[0]));
    CHECK_FALSE(fragmentCoverageHeadOf(soa.flags[1]));
    // ...and the claim really was re-recorded, not merely left alone.
    CHECK(scratch.claimBin[static_cast<std::size_t>(soa.bucketIndex0[0])]
          == scatterKernelBin(radiusPixels(p, zA)));
}

TEST_CASE("a NON-colliding fragment's own alpha is the untouched float, not a reconstruction")
{
    // emitPending() recomputes `f.alpha` from its two deposits ONLY when an
    // attenuation actually happened, so that a non-colliding fragment keeps
    // the exact float it arrived with.  Making the recompute unconditional
    // otherwise passes the suite, yet it is not a no-op:
    // 1 - (1-a0)*(1-a1) differs from `a` in the last ULPs for 46% of single
    // fragments over a 7761-point (alpha, depth) grid — e.g. alpha 0.005 reads
    // 0.00499999523 against the input's 0.00499999989.
    const CocParams    p  = makeStandardRig(10.0f);
    const DepthBuckets bk = makeStandardBuckets(p, 32);
    const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ false);

    int checked = 0;
    for (int i = 1; i < 40; ++i) {
        const float a = static_cast<float>(i) / 200.0f;
        for (int j = 1; j < 20; ++j) {
            const float z = 1.0f + static_cast<float>(j) * 0.7f;
            const SampleSoA soa = flattenOnePixel(fp, bk, 0, 0,
                {makeSample(z, z, a, {a})});
            if (soa.fragmentCount() != 1u)
                continue;
            ++checked;
            CHECK(soa.alpha[0] == a);           // BIT equality, not a tolerance
        }
    }
    REQUIRE(checked > 100);
}

TEST_CASE("a pixel with ONE fragment per bucket pair is BIT-IDENTICAL to a flatten with no "
          "collision merge at all")
{
    // The whole mechanism is gated on a COLLISION, so a fragment that does not
    // collide must come out of the flatten with the exact floats it went in
    // with: no attenuation, no clamped assignment, no lost area claim.
    // Checked as "the same fragment flattened alone == flattened alongside a
    // far-away one", bit for bit -- the strongest form available inside the
    // suite, and the one that protects every identity this file pins.
    const CocParams    p  = makeStandardRig(10.0f);
    const DepthBuckets bk = makeStandardBuckets(p, 32);
    const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ false);

    const SampleSoA alone = flattenOnePixel(fp, bk, 5, 5,
        {makeSample(3.0f, 3.0f, 0.7f, {0.35f})});
    // ...and again with a second sample far enough away in depth that their
    // bucket pairs cannot touch.
    const SampleSoA pair = flattenOnePixel(fp, bk, 5, 5,
        {makeSample(3.0f, 3.0f, 0.7f, {0.35f}),
         makeSample(60.0f, 60.0f, 0.4f, {0.2f})});

    REQUIRE(alone.fragmentCount() == 1u);
    REQUIRE(pair.fragmentCount() == 2u);
    CHECK(pair.bucketIndex0[0] == alone.bucketIndex0[0]);
    CHECK(pair.bucketIndex1[0] == alone.bucketIndex1[0]);
    CHECK(pair.alpha[0]        == alone.alpha[0]);          // BIT equality
    CHECK(pair.bucketAlpha0[0] == alone.bucketAlpha0[0]);
    CHECK(pair.bucketAlpha1[0] == alone.bucketAlpha1[0]);
    CHECK(pair.colorScale0[0]  == alone.colorScale0[0]);
    CHECK(pair.colorScale1[0]  == alone.colorScale1[0]);
    CHECK(pair.flags[0]        == alone.flags[0]);
    CHECK(pair.colorOf(0)[0]   == alone.colorOf(0)[0]);
    // Both deposits of BOTH fragments still write their area.
    CHECK(fragmentDepositsArea0Of(pair.flags[1]));
    CHECK(fragmentDepositsArea1Of(pair.flags[1]));
    CHECK(fragmentCoverageHeadOf(pair.flags[1]));
}

TEST_CASE("the two-sample collision case resolves to the flatten, not to a fully opaque "
          "bucket")
{
    // The two-sample case a plane dump exposes it with: z = 9.063 alpha 0.4667
    // and z = 11.039 alpha 0.5899 at K = 8, whose bucketOf() assignments
    // overlap while their CONTAINING buckets differ.  Without the collision
    // pass: bucket[6] alpha 1.0127, newArea 2.0, colocated 0.0, so cov clamps
    // 2.0 -> 1.0, a clamps 1.0127 -> 1.0, local = a/cov = 1.0 and the pixel
    // comes out at 1.000.
    const CocParams    p  = makeStandardRig(10.0f);
    const DepthBuckets bk = makeBoundedDeltaCocBuckets(p, 1.0f, 100.0f, 8);
    const int W = 8, H = 8;
    DiscKernelLUT kernel(0.0f, 1.0f, 1.0f, 1.0f);

    // Hand-derived, in the test's own arithmetic: a1 over a2.
    const double truth = 0.4667 + 0.5899 * (1.0 - 0.4667);   // 0.7812929...
    REQUIRE(truth > 0.78);
    REQUIRE(truth < 0.79);

    for (bool preMerge : {false, true}) {
        CAPTURE(preMerge);
        const FlattenParams fp = makeFlattenParams(p, 1, preMerge);
        const SampleSoA soa = flattenOnePixel(fp, bk, 4, 4,
            {makeSample(9.063f, 9.063f, 0.4667f, {0.4667f * 0.25f}),
             makeSample(11.039f, 11.039f, 0.5899f, {0.5899f * 0.75f})});

        // Both are on the sharp path here (radius ~0.27px), so they are one
        // kernel and the collision is resolvable exactly.
        REQUIRE(soa.fragmentCount() == 1u);

        Band band;
        band.K = 8; band.C = 1; band.W = W; band.H = H;
        HoldoutSoA noHoldout;
        runBand(band, makeScatterParams(W, H),
                soa, noHoldout, kernel);

        CHECK(std::fabs(static_cast<double>(band.outAlpha(4, 4)) - truth) <= 2e-06);
        // Premultiplied colour follows the same `over`.
        const double truthC = 0.4667 * 0.25 + (1.0 - 0.4667) * 0.5899 * 0.75;
        CHECK(std::fabs(static_cast<double>(band.outColor(0, 4, 4)) - truthC) <= 2e-06);

        // The plane the defect lived in: ONE unit of new area at this pixel,
        // never two.
        double newArea = 0.0;
        for (int k = 0; k < band.K; ++k)
            newArea += planeSum(band.planes.weight, k, band.pixels());
        CHECK(newArea == doctest::Approx(1.0).epsilon(1e-5));
    }
}

TEST_CASE("within one bucket at one pixel, every area claim belongs to ONE kernel")
{
    // The structural half of the invariant, fuzzed on the real path across a
    // sharp rig and a defocused one.  The area planes describe a bucket as
    // "C_k of the pixel claimed by one kernel, D_k co-located on top of it", so
    // a claim from a SECOND, differently-sized disc has nowhere to go: that is
    // the `cov = 2 -> clamp 1 -> local = a/cov = 1` half of the defect, and it
    // is what turns a bucket fully opaque (1.000 against a true 0.781).
    //
    // It is deliberately NOT "one head per bucket".  Two deposits sharing a
    // bucket AND a kernel cover the identical destination area, which C_k : D_k
    // cannot describe at all -- the composite then reads `a - a^2/4` against a
    // true `a1 + a2 - a1*a2`, short by ((a1-a2)/2)^2 and by a flat 0.25 once the
    // additive alpha saturates.  Those are the merge's to resolve; where it may
    // not reach them both claims stand and the bucket degrades to a plain
    // `over`.  Measured: bounding by the bucket alone
    // instead moves a 2000-pixel mixed point+volumetric size-0 corpus from mean
    // |d alpha| 2.27e-02 to 6.00e-02 at 20 spp / K=16 (rate beyond 1e-3 from
    // 38.9% to 98.0%), and turned an exact opaque reading into a 25% hole.
    const int W = 24, H = 24;
    Lcg rng(0x5A17u);

    for (float sizePx : {0.0f, 12.0f}) {
        const CocParams p = makeManualRig(sizePx, 8.0f);
        for (bool preMerge : {false, true}) {
            for (int K : {4, 16, 64}) {
                CAPTURE(sizePx);
                CAPTURE(preMerge);
                CAPTURE(K);
                const DepthBuckets bk = makeBoundedDeltaCocBuckets(p, 1.0f, 100.0f, K);
                const FlattenParams fp = makeFlattenParams(p, 1, preMerge);

                SampleSoA soa;
                soa.begin(1, fp.groups);
                FlattenScratch scratch;
                for (int y = 0; y < H; ++y)
                    for (int x = 0; x < W; ++x) {
                        std::vector<SampleRecord> v;
                        float z = rng.range(1.05f, 40.0f);
                        const int n = rng.intRange(1, 8);
                        for (int s = 0; s < n; ++s) {
                            const float a  = rng.range(0.02f, 1.0f);
                            const float th = (rng.unit() < 0.5f) ? 0.0f
                                                                 : rng.range(0.02f, 5.0f);
                            v.push_back(makeSample(z, z + th, a, {a * 0.5f}));
                            z += th + rng.range(0.05f, 5.0f);
                        }
                        flattenPixelToSoA(fp, bk, x, y, v, scratch, soa, nullptr, nullptr, nullptr);
                    }

                // (source pixel, bucket, kernel bin) of every head, read off the
                // SoA -- the plane is downstream of this and cannot be
                // attributed to a depositor once the deposits have been summed.
                std::vector<std::array<int, 4>> claims;
                for (std::size_t i = 0; i < soa.fragmentCount(); ++i) {
                    if (!fragmentCoverageHeadOf(soa.flags[i]))
                        continue;
                    claims.push_back({soa.x[i], soa.y[i],
                                      static_cast<int>(soa.bucketIndex0[i]),
                                      refKernelBin(soa.radius[i])});
                }
                REQUIRE(!claims.empty());
                std::sort(claims.begin(), claims.end());
                for (std::size_t i = 1; i < claims.size(); ++i) {
                    const bool sameSlot = claims[i][0] == claims[i - 1][0]
                                       && claims[i][1] == claims[i - 1][1]
                                       && claims[i][2] == claims[i - 1][2];
                    // Sorted, so a differing bin in the same slot is adjacent.
                    const bool twoKernelsInOneBucket =
                        sameSlot && (claims[i][3] != claims[i - 1][3]);
                    REQUIRE_FALSE(twoKernelsInOneBucket);
                }
            }
        }
    }
}

TEST_CASE("the collision merge is bounded: different kernels are not collapsed, and a "
          "focus-straddling group keeps its radius")
{
    const CocParams    p  = makeManualRig(10.0f, 10.0f);
    const DepthBuckets bk = makeBoundedDeltaCocBuckets(p, 1.0f, 100.0f, 4);

    SUBCASE("two same-pixel fragments with genuinely different discs stay separate")
    {
        // K = 4 puts these two in one bucket; their radii are 23.3px and 8.5px,
        // i.e. 15px and ~29 LUT entries apart.  Merging them would render the
        // far layer at the near layer's bokeh size, which is why the merge is
        // gated on the kernel and not on the bucket alone.
        const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ false);
        const SampleSoA soa = flattenOnePixel(fp, bk, 80, 80,
            {makeSample(3.0f, 3.0f, 1.0f, {1.0f}),
             makeSample(5.4f, 5.4f, 1.0f, {1.0f})});
        REQUIRE(soa.fragmentCount() == 2u);
        REQUIRE(soa.bucketIndex0[0] == soa.bucketIndex0[1]);   // they DO collide
        CHECK(soa.radius[0] == doctest::Approx(23.3333f).epsilon(1e-4));
        CHECK(soa.radius[1] == doctest::Approx(8.5185f).epsilon(1e-4));
        // ...and exactly one of them claims the bucket's area.
        CHECK(fragmentCoverageHeadOf(soa.flags[0]));
        CHECK(!fragmentCoverageHeadOf(soa.flags[1]));

        // AND THE PIXELS: two opaque surfaces at one source pixel flatten to
        // ONE opaque surface, so the band's alpha must integrate to exactly 1
        // (every disc's weights sum to 1).  With both of them claiming new area
        // it reads 2.000000 — the recorded occlusion-before-blur number, which
        // is the double claim wherever the two land in one bucket.  The composite's C_k : D_k area split then hands the
        // whole of the alpha to the nearer, larger disc, which is exact.
        const int W2 = 160, H2 = 160;
        Band band;
        band.K = 4; band.C = 1; band.W = W2; band.H = H2;
        HoldoutSoA noHoldout;
        DiscKernelLUT k2(0.0f, 40.0f, 1.0f, 1.0f);
        runBand(band, makeScatterParams(W2, H2),
                soa, noHoldout, k2);
        CHECK(bandAlphaSum(band) == doctest::Approx(1.0).epsilon(1e-5));
    }

    SUBCASE("a collision group that mixes a NON-head with a following head keeps the coverage "
            "(the survival rule is an OR here too, and it fires with pre_merge OFF)")
    {
        // The coverage-survival OR, reached through the collision pass instead
        // of through the knob.  Parent A is cut at boundary(10), so its second part is a
        // NON-head sitting in bucket 10; parent B lies wholly inside bucket 10
        // immediately behind it, is a head, and is close enough in depth to
        // share A1's kernel.  They collide, so they merge — and taking the
        // group's first flag instead of the OR would drop B's coverage
        // entirely, leaving one claimed bucket where there are two.
        //
        // "Close enough" is a MEASURED distance, not a guess: at these depths
        // the radii are ~7.14px and ~7.08px and the kernel-radius grid is
        // 0.103px wide there, so both land on grid node 953.  A span of
        // [b10+0.02, b10+0.05] puts the two on ADJACENT nodes instead, which
        // silently turns this subcase into a three-fragment no-merge case.
        const CocParams    q  = makeStandardRig(10.0f);
        const DepthBuckets qb = makeStandardBuckets(q);
        const float b10 = qb.boundary(10);
        const FlattenParams fq = makeFlattenParams(q, 1, /*preMerge*/ false);
        const SampleSoA soa = flattenOnePixel(fq, qb, 20, 20,
            {makeSample(b10 - 0.02f, b10 + 0.02f, 0.6f, {0.6f * 0.5f}),
             makeSample(b10 + 0.02f, b10 + 0.03f, 0.4f, {0.4f * 0.5f})});

        // A0 (head, bucket 9) and the merged [A1 + B] (bucket 10).
        REQUIRE(soa.fragmentCount() == 2u);
        CHECK(soa.bucketIndex0[0] == 9);
        CHECK(soa.bucketIndex0[1] == 10);
        CHECK(fragmentCoverageHeadOf(soa.flags[0]));
        CHECK(fragmentCoverageHeadOf(soa.flags[1]));

        const int W2 = 96, H2 = 96;
        Band band;
        band.K = qb.bucketCount(); band.C = 1; band.W = W2; band.H = H2;
        HoldoutSoA noHoldout;
        DiscKernelLUT k2(0.0f, 60.0f, 1.0f, 1.0f);
        runBand(band, makeScatterParams(W2, H2),
                soa, noHoldout, k2);
        double newArea = 0.0;
        for (int k = 0; k < band.K; ++k)
            newArea += planeSum(band.planes.weight, k, band.pixels());
        // TWO parents, TWO claimed buckets -- 1.0 would mean B's was dropped.
        CHECK(newArea == doctest::Approx(2.0).epsilon(1e-5));
    }

    SUBCASE("fragments that do NOT share a bucket are never merged")
    {
        // The merge exists to resolve a COLLISION.  Two same-pixel fragments in
        // different buckets are already composited correctly by the front-to-
        // back bucket walk, and joining them would destroy exactly the depth
        // separation the K knob buys — so the bucket test is not an
        // optimisation and dropping it is not equivalent.  All-sharp rig, so
        // the kernel gate cannot be what keeps them apart.
        const CocParams    q  = makeManualRig(0.05f, 10.0f);
        const DepthBuckets qb = makeBoundedDeltaCocBuckets(q, 1.0f, 100.0f, 16);
        const FlattenParams fq = makeFlattenParams(q, 1, /*preMerge*/ false);
        const SampleSoA soa = flattenOnePixel(fq, qb, 0, 0,
            {makeSample(5.0f, 5.0f, 0.5f, {0.5f}),
             makeSample(60.0f, 60.0f, 0.5f, {0.5f})});
        REQUIRE(soa.fragmentCount() == 2u);
        // ...and they really are in disjoint bucket ranges, or the case is not
        // testing what it claims.
        const int hi0 = static_cast<int>(soa.bucketIndex1[0]);
        const int lo1 = static_cast<int>(soa.bucketIndex0[1]);
        CHECK(hi0 < lo1);
        // Both claim their own area: distinct buckets, no collision.
        CHECK(fragmentCoverageHeadOf(soa.flags[0]));
        CHECK(fragmentCoverageHeadOf(soa.flags[1]));
    }

    SUBCASE("a POINT and a SPAN PIECE sharing a bucket are NOT merged (the composition contract "
            "wins over the collision)")
    {
        // The one collision the pass deliberately leaves standing.  Merging
        // across FragmentKind would force the result to take one half of the
        // composition contract: as a Point it would give a span piece a second,
        // fractional split on top of the boundary split it already received —
        // the +8.3% double-count shape — and as Volumetric it would strip a
        // point sample of the fractional assignment the design calls mandatory
        // for layer-transition banding.  Neither is acceptable, so the two stay
        // separate and the collision remains.
        //
        // KNOWN RESIDUAL, measured over 2000 random size-0 pixels of mixed
        // point + volumetric content: worst |d alpha| 2.4e-01 with 34-61% of
        // pixels beyond 1e-3, which the collision pass does not improve on
        // (2.3e-01, 47-67% without it) — pure-point and pure-span content are
        // both at 2e-07.  Recorded here so the hole has a test that names it.
        const CocParams    q  = makeStandardRig(10.0f);
        const DepthBuckets qb = makeStandardBuckets(q);
        const float b10 = qb.boundary(10);
        const FlattenParams fq = makeFlattenParams(q, 1, /*preMerge*/ false);
        const SampleSoA soa = flattenOnePixel(fq, qb, 0, 0,
            {makeSample(b10 + 0.05f, b10 + 0.05f, 0.6f, {0.6f * 0.5f}),
             makeSample(b10 + 0.051f, b10 + 0.055f, 0.4f, {0.4f * 0.5f})});

        REQUIRE(soa.fragmentCount() == 2u);
        CHECK(fragmentKindOf(soa.flags[0]) == FragmentKind::Point);
        CHECK(fragmentKindOf(soa.flags[1]) == FragmentKind::Volumetric);
        // They really do collide (the point's rear bucket is the span's), and
        // they really are one kernel — kind is the only thing keeping them
        // apart, so this case cannot pass for the wrong reason.  NOTE: a span
        // of [b10+0.06, b10+0.10] does NOT work here — at ~6.99px the
        // kernel-radius grid's node spacing is 0.096px and those two midpoints
        // are 0.109px apart in radius, so they do not share a kernel and the
        // subcase passes for the wrong reason.
        CHECK(soa.bucketIndex1[0] == soa.bucketIndex0[1]);
        CHECK(sameScatterKernel(soa.radius[0], soa.radius[1]));
    }

    SUBCASE("an UNMERGEABLE same-kernel collision is `over`-composited PER BUCKET and claims "
            "its area ONCE, not once per colliding deposit")
    {
        // A span piece and a point sample inside ONE bucket at ONE pixel, both
        // opaque, both on the sharp path — i.e. an in-focus card behind fog.
        // The merge may not take them (FragmentKind differs, and merging would
        // have to mislabel one of them against the COMPOSITION CONTRACT), so
        // this is a CROSS-KIND collision.
        //
        // THE RULE THIS PINS:
        //   * the trailing deposit is scaled by `1 - running_k` — here the
        //     leading piece is opaque, so the point contributes NOTHING;
        //   * it therefore claims no area either, because the area is already
        //     in the plane and the two deposits cover the IDENTICAL region.
        // Leaving BOTH claims standing instead only makes sense when the alpha
        // is ADDED, where the C_k : D_k split reads `a - a^2/4` (0.750000
        // against a true 1.0 at a = 1).  With the alpha composited that trade is
        // gone: `cov` clamps to 1, `aCov = min(a, cov) = a` and `local` is the
        // true composited alpha, so the bucket reads the flatten exactly.
        // Measured over the mixed size-0 corpus (900 pixels x K 4..128 x
        // 2..20 spp x pre_merge both, holdout both ways): worst |d alpha|
        // 2.61e-01 -> 4.30e-07 and worst |d colour| 8.30e-01 -> 3.16e-07.
        const int W2 = 8, H2 = 8, K2 = 16;
        const CocParams    q  = makeManualRig(0.0f, 10.0f);
        const DepthBuckets qb = makeBoundedDeltaCocBuckets(q, 1.0f, 100.0f, K2);
        // Bucket 8's centre, so the point's fractional assignment sits wholly in
        // it and the collision is a whole-bucket one.
        const float zc = qb.centre(8);
        const FlattenParams fq = makeFlattenParams(q, 1, /*preMerge*/ false);
        const SampleSoA soa2 = flattenOnePixel(fq, qb, 4, 4,
            {makeSample(zc - 0.01f, zc - 0.005f, 1.0f, {0.5f}),   // span piece, opaque
             makeSample(zc,         zc,          1.0f, {0.5f})}); // point,      opaque

        REQUIRE(soa2.fragmentCount() == 2u);
        REQUIRE(fragmentKindOf(soa2.flags[0]) == FragmentKind::Volumetric);
        REQUIRE(fragmentKindOf(soa2.flags[1]) == FragmentKind::Point);
        REQUIRE(soa2.bucketIndex0[0] == soa2.bucketIndex0[1]);   // one bucket
        REQUIRE(sameScatterKernel(soa2.radius[0], soa2.radius[1]));

        // The FRONT one owns the bucket: its area, its alpha.
        CHECK(fragmentCoverageHeadOf(soa2.flags[0]));
        CHECK(fragmentDepositsArea0Of(soa2.flags[0]));
        // The trailing one is attenuated to nothing behind an opaque layer, and
        // writes no area on top of the area already there.
        CHECK_FALSE(fragmentCoverageHeadOf(soa2.flags[1]));
        CHECK_FALSE(fragmentDepositsArea0Of(soa2.flags[1]));
        CHECK(soa2.bucketAlpha0[1] == 0.0f);
        CHECK(soa2.colorScale0[1] == 0.0f);

        Band band2;
        band2.K = K2; band2.C = 1; band2.W = W2; band2.H = H2;
        HoldoutSoA noHoldout2;
        DiscKernelLUT k3(0.0f, 1.0f, 1.0f, 1.0f);
        runBand(band2, makeScatterParams(W2, H2),
                soa2, noHoldout2, k3, /*useThread*/ false);
        // Two opaque layers at one pixel flatten to one opaque pixel.
        CHECK(std::fabs(static_cast<double>(band2.outAlpha(4, 4)) - 1.0) <= 2e-06);
        // ...and to the FRONT layer's colour, not to a mixture of the two: the
        // trailing deposit contributes nothing at all.
        CHECK(std::fabs(static_cast<double>(band2.outColor(0, 4, 4)) - 0.5) <= 2e-06);
    }

    SUBCASE("a group straddling the focal plane renders at its front member's radius, not at 0")
    {
        // Equal radii either side of focus.  Deriving the merged fragment's
        // radius from the union midpoint would put it ON the focal plane and
        // render two blurred layers sharp; the group's depth is its FRONT
        // member's and never moves, so it cannot.
        const float back = 10.0f * 10.0f / (10.0f - 1.0f);   // radius(back) == radius(9)
        const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ false);
        const SampleSoA soa = flattenOnePixel(fp, bk, 0, 0,
            {makeSample(9.0f, 9.0f, 0.5f, {0.5f}),
             makeSample(back, back, 0.5f, {0.5f})});
        REQUIRE(soa.fragmentCount() >= 1u);
        // radius(9) = size * |1 - 10/9| = 10 * 1/9
        CHECK(soa.radius[0] == doctest::Approx(10.0f / 9.0f).epsilon(1e-4));
        for (std::size_t i = 0; i < soa.fragmentCount(); ++i)
            CHECK(soa.radius[i] > 0.9f);
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
    const int C = 1, W = 28, H = 28, K = 16;
    const float z1 = 9.0f, z2 = 11.0f, a1 = 0.5f, a2 = 0.4f, focus = 10.0f;
    const double truth = 1.0 - (1.0 - a1) * (1.0 - a2);      // 0.70, exact

    double prev = 0.0;
    bool   havePrev = false;
    bool   prevSharp1 = true, prevSharp2 = true;
    double worstElsewhere = 0.0, worstCrossing = 0.0, worstError = 0.0;

    for (int step = 0; step <= 24; ++step) {
        const float size = 18.0f * static_cast<float>(step) / 24.0f;
        const CocParams    p  = makeManualRig(size, focus);
        const DepthBuckets bk = makeBoundedDeltaCocBuckets(p, 1.0f, 100.0f, K);
        const FlattenParams fp = makeFlattenParams(p, C, /*preMerge*/ true);

        const float r1 = radiusPixels(p, z1);
        const float r2 = radiusPixels(p, z2);
        const float rMax = std::max(r1, r2);
        const int   pad  = static_cast<int>(std::ceil(rMax)) + 2;

        SampleSoA soa;
        soa.begin(C, fp.groups);
        FlattenScratch scratch;
        for (int y = -pad; y < H + pad; ++y)
            for (int x = -pad; x < W + pad; ++x) {
                std::vector<SampleRecord> v{makeSample(z1, z1, a1, {a1 * 0.5f}),
                                            makeSample(z2, z2, a2, {a2 * 0.5f})};
                flattenPixelToSoA(fp, bk, x, y, v, scratch, soa, nullptr, nullptr, nullptr);
            }

        Band band;
        band.K = K; band.C = C; band.W = W; band.H = H;
        HoldoutSoA noHoldout;
        DiscKernelLUT kernel(0.0f, std::max(1.0f, rMax), 1.0f, 1.0f);
        runBand(band, makeScatterParams(W, H),
                soa, noHoldout, kernel, /*useThread*/ false);

        const double v = band.outAlpha(W / 2, H / 2);
        worstError = std::max(worstError, std::fabs(v - truth));

        const bool sharp1 = !(r1 >= kSharpRadiusPx);
        const bool sharp2 = !(r2 >= kSharpRadiusPx);
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
    const DepthBuckets bk = makeBoundedDeltaCocBuckets(p, 1.0f, 100.0f, K);
    const HoldoutBoundaries hb = makeUniformHoldoutBoundaries(bk);

    HoldoutSampleSoA hs;
    HoldoutLut       lut;
    buildHoldout(hs, lut, hb, W, H, [](int, int, std::vector<SampleRecord>& out) {
        out.push_back(makeSample(30.0f, 30.0f, 1.0f, {}));
    });

    for (bool connected : {false, true}) {
        CAPTURE(connected);
        FlattenParams fp = makeFlattenParams(p, C, /*preMerge*/ false);
        fp.holdoutConnected = connected;

        SampleSoA soa;
        soa.begin(C, fp.groups);
        FlattenScratch scratch;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                std::vector<SampleRecord> v{makeSample(20.0f, 20.0f, 0.5f, {0.5f}),
                                            makeSample(40.0f, 40.0f, 0.5f, {0.5f})};
                flattenPixelToSoA(fp, bk, x, y, v, scratch, soa, nullptr, nullptr, nullptr);
            }

        const std::size_t perPixel =
            soa.fragmentCount() / static_cast<std::size_t>(W * H);

        Band band;
        band.K = K; band.C = C; band.W = W; band.H = H;
        DiscKernelLUT kernel(0.0f, 1.0f, 1.0f, 1.0f);
        HoldoutSoA view = lut.view();
        runBand(band, makeScatterParams(W, H),
                soa, view, kernel, /*useThread*/ false);

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
    // The pre-merge groups by containing ΔCoC bucket, and that bucket is the
    // wrong width for occlusion: on the default rig (K=16, focus 10, measured
    // range [1,100]) the last one is [10, 100] -- ninety units -- against the
    // holdout LUT's ~6.2-unit uniform-in-z brackets.  So two samples either side
    // of a holdout card can share a bucket AND fall inside the 0.25px radius
    // tolerance, and the group's union midpoint then lands BEHIND the card.
    // Measured before the gate: alpha 0.000000 against an exact 0.500000, i.e.
    // genuinely unoccluded foreground erased outright, with pre_merge at its
    // default ON.
    const int C = 1, W = 8, H = 8, K = 16;
    const CocParams    p  = makeStandardRig(10.0f);
    const DepthBuckets bk = makeStandardBuckets(p, K);
    REQUIRE(bk.boundary(K - 1) == doctest::Approx(10.0f).epsilon(1e-4));
    REQUIRE(bk.boundary(K) == doctest::Approx(100.0f).epsilon(1e-4));
    const HoldoutBoundaries hb = makeUniformHoldoutBoundaries(bk);

    HoldoutSampleSoA hs;
    HoldoutLut       lut;
    buildHoldout(hs, lut, hb, W, H, [](int, int, std::vector<SampleRecord>& out) {
        out.push_back(makeSample(50.0f, 50.0f, 1.0f, {}));      // opaque card
    });

    const float za = 40.0f, zb = 62.0f;                          // either side of it
    // They really do share a bucket and sit inside the DEFAULT tolerance, or the
    // case is not testing what it claims.
    REQUIRE(std::fabs(radiusPixels(p, za) - radiusPixels(p, zb)) <= 0.25f);

    for (bool preMerge : {false, true}) {
        CAPTURE(preMerge);
        FlattenParams fp = makeFlattenParams(p, C, preMerge);    // 0.25px default
        fp.holdoutConnected = true;

        SampleSoA soa;
        soa.begin(C, fp.groups);
        FlattenScratch scratch;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                std::vector<SampleRecord> v{makeSample(za, za, 0.5f, {0.5f}),
                                            makeSample(zb, zb, 0.5f, {0.5f})};
                flattenPixelToSoA(fp, bk, x, y, v, scratch, soa, nullptr, nullptr, nullptr);
            }
        CHECK(soa.fragmentCount() == static_cast<std::size_t>(W * H) * 2u);

        Band band;
        band.K = K; band.C = C; band.W = W; band.H = H;
        DiscKernelLUT kernel(0.0f, 60.0f, 1.0f, 1.0f);
        HoldoutSoA view = lut.view();
        runBand(band, makeScatterParams(W, H),
                soa, view, kernel, /*useThread*/ false);
        // The front sample survives whole; the back one is behind an opaque card.
        CHECK(std::fabs(static_cast<double>(band.outAlpha(4, 4)) - 0.5) <= 2e-06);
    }
}

// ===========================================================================
// The scatter core
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
            flattenPixelToSoA(fp, bk, s.x, s.y, v, scratch, soa, nullptr, nullptr, nullptr);
        }
        REQUIRE(soa.fragmentCount() >= 7);

        const ScatterParams sp = makeScatterParams(W, H);
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
        flattenPixelToSoA(fp, bk, 120, 214, v, scratch, soa, nullptr, nullptr, nullptr);

        ScatterParams sp = makeScatterParams(W, H);
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
            flattenPixelToSoA(fp, bk, 10 + 3 * i, 12, v, scratch, whole, nullptr, nullptr, nullptr);
            flattenPixelToSoA(fp, bk, 10 + 3 * i, 12, v2, scratch,
                              (i < 3) ? partA : partB, nullptr, nullptr, nullptr);
        }

        const ScatterParams sp = makeScatterParams(W, H);
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
    // plain "deposit every part" coverage -- and that sum
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
    flattenPixelToSoA(fp, bk, 60, 60, a, scratch, soa, nullptr, nullptr, nullptr);
    flattenPixelToSoA(fp, bk, 58, 62, b, scratch, soa, nullptr, nullptr, nullptr);

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

    const ScatterParams sp = makeScatterParams(W, H);
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
          "the bucket composite conserves")
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

    std::thread worker([&] {
        {
            Lcg rng(0x5EED1234u);
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
                runBand(band, makeScatterParams(W, H), soa, noHoldout, lut,
                        /*useThread*/ false);

                const double relAlpha = std::fabs(bandAlphaSum(band) - alpha) / alpha;
                const double relColor =
                    std::fabs(bandColorSum(band, 1) - alpha * unpremult[1]) / (alpha * unpremult[1]);

                worstPartitionAlpha = std::max(worstPartitionAlpha, relAlpha);
                worstPartitionColor = std::max(worstPartitionColor, relColor);
            }
        }
    });
    worker.join();

    // MEASURED: 2.18e-07 alpha / 2.67e-07 colour over this corpus (1.37e-07 /
    // 1.24e-07 over an independently generated one).  3e-06 is ~13x headroom
    // and still two orders of magnitude below any structural error -- the
    // smallest one this file pins is the +1.9% mislabel at alpha 0.1.
    CHECK(worstPartitionAlpha <= 3e-06);
    CHECK(worstPartitionColor <= 3e-06);
    // What this pins is the identity itself, which is what the composite has
    // to hold: a composite that adds the two deposits instead inflates this
    // same corpus by up to +91.55% -- an isolated opaque bokeh at double
    // energy.
}

TEST_CASE("flat field identities: opaque field is alpha 1 to 1e-6 (NOT exactly 1), "
          "50% fog is 0.5, and the colour:alpha ratio is the input's")
{
    // Validation scene (c) at POD level.  |alpha - 1| <= ~1e-6, NOT equality:
    // the disc LUT's per-entry normalisation residual is ~5e-8 over ~113
    // contributing fragments (an "exactly 1" reading here means an over-count
    // is being clamped).  The assertion below is 2e-06, against a measured
    // 8e-07.
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
                    flattenPixelToSoA(fp, bk, x, y, v, scratch, soa, nullptr, nullptr, nullptr);
                }

            {
                CAPTURE(depth);
                CAPTURE(alpha);

                Band band;
                band.K = bk.bucketCount(); band.C = C; band.W = W; band.H = H;
                HoldoutSoA noHoldout;
                runBand(band, makeScatterParams(W, H), soa, noHoldout, lut);

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

TEST_CASE("flat opaque field ACROSS buckets: the bucket composite holds alpha 1")
{
    // The flat-opaque-across-buckets identity, on the configuration that
    // discriminates bucket composites hardest: a checkerboard of two depths
    // sitting EXACTLY on two bucket centres, so every fragment's assignment is
    // whole-weight (frac == 0) into one bucket and each bucket receives half
    // the disc weight.  Plain front-to-back `over` gives 1 - (1-0.5)^2 = 0.75
    // here -- a 25.0%-across-2-buckets alpha deficit -- while the coverage
    // partition adds the two disjoint half-coverages back to 1.
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
            flattenPixelToSoA(fp, bk, x, y, v, scratch, soa, nullptr, nullptr, nullptr);
        }

    double minAlpha = 2.0, maxAlpha = -1.0, ratio = 0.0;
    {
        Band band;
        band.K = bk.bucketCount(); band.C = 1; band.W = W; band.H = H;
        HoldoutSoA noHoldout;
        runBand(band, makeScatterParams(W, H), soa, noHoldout, lut);

        for (int y = 16; y < 32; ++y)
            for (int x = 16; x < 32; ++x) {
                minAlpha = std::min(minAlpha, static_cast<double>(band.outAlpha(x, y)));
                maxAlpha = std::max(maxAlpha, static_cast<double>(band.outAlpha(x, y)));
            }
        ratio = band.outColor(0, 24, 24) / band.outAlpha(24, 24);
    }

    // The composite holds the identity to 5e-3 (measured 0.995718 at the worst
    // interior pixel: the two checkerboard depths rasterise DIFFERENT radii,
    // 7.85px and 6.40px, so the two half-coverages do not tile the pixel
    // perfectly).  BANDED, not floored: a floor at 0.9956 also accepts a
    // uniform 0.5px kernel grid's 0.99927, so it would not notice the grid
    // being coarsened.
    //
    // WHY THE BAND SITS WHERE IT DOES.  A checkerboard is the Nyquist pattern,
    // so what it really measures is the kernels' response at (pi, pi):
    // alpha == 1 + (C_A - C_B)/2 with C_r = sum (-1)^(dx+dy) w_r.  A uniform
    // 0.5px grid would SNAP the two radii to 8.00 and 6.50, and
    // (C_8.00 - C_6.50)/2 = -7.30e-04 is a number that belongs to the snapping.
    // This grid snaps them to 7.876923 and 6.400000, giving -4.28e-03 against
    // the UNQUANTISED -4.00e-03, i.e. within 2.8e-04 of the exact answer
    // instead of 3.3e-03 away from it.
    //
    // A plain `over` of the buckets reads 0.7479..0.7521 on this same fixture
    // -- the 25.0%-across-2-buckets deficit -- which is what makes this
    // configuration the cleanest discriminator available.
    CHECK(minAlpha > 0.9954);
    CHECK(minAlpha < 0.9960);
    CHECK(maxAlpha <= 1.0);
    // The ratio holds: any error here is an alpha deficit, not a colour desync.
    CHECK(std::fabs(ratio - 0.8) <= 1e-05);
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

        const ScatterParams sp = makeScatterParams(W, H);
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

        const ScatterParams sp = makeScatterParams(W, H);
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
    // The band loop allocates once and calls
    // zero() per band (see scatterBandCPU's header: "ACCUMULATED INTO, never
    // cleared here").  Every case in this file allocates fresh planes, and
    // allocate() zero-fills, so a zero() that missed a plane was invisible --
    // and would show up in production as the previous band's area bleeding
    // into this one's composite.
    const int K = 4, C = 2, W = 10, H = 8;
    DiscKernelLUT lut(0.0f, 8.0f, 1.0f, 1.0f);
    const ScatterParams sp = makeScatterParams(W, H);
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
    scatterOnThread(makeScatterParams(W, H),
                    soa, none, lut, planes);

    // The colour is split between the two buckets by the partition; the alpha
    // planes stay empty, which is the honest answer for a transparent fragment.
    CHECK(planeSum(planes.color, 2, px) == doctest::Approx(0.5f * 0.6f));
    CHECK(planeSum(planes.color, 3, px) == doctest::Approx(0.5f * 0.4f));
    CHECK(planeSum(planes.alpha, 2, px) == 0.0);
    CHECK(planeSum(planes.alpha, 3, px) == 0.0);
}

TEST_CASE("ScatterStats accounts for every fragment exactly once")
{
    // The stats block feeds the perf gate and the node's own reporting.
    // Counts are hand-derived from the fixture.
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
    scatterBandCPU(makeScatterParams(W, H),
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
    // The branch and the audit read the same `flags` field, so a span-split
    // part labelled Point is internally consistent and passes
    // the audit while double-counting.  Parent reconstruction is the only check
    // that sees it -- this case proves BOTH halves of that claim.
    const CocParams    p  = makeStandardRig(10.0f);
    const DepthBuckets bk = makeStandardBuckets(p);
    const int W = 160, H = 160;
    DiscKernelLUT lut(0.0f, 60.0f, 1.0f, 1.0f);
    const float alpha = 0.9f, unpremult = 0.5f;

    struct Case { int buckets; double misAlphaPct; };
    // These figures ARE the truth: driven through a grid-free kernel sampler
    // (one exact disc per radius, no quantisation at all) this reads 45.30 /
    // 76.70 to two decimals, i.e. what the shipped grid gives.  A uniform 0.5px
    // grid reads 40.09 / 70.70 instead.
    //
    // WHY THE GRID MOVES IT -- the mechanism, measured, not assumed.  It is NOT
    // the same-kernel collision rule: this case builds its SoA by hand and
    // never goes through flattenPixelToSoA(), and in any case the four parts
    // sit at radius 4.95565 / 3.50323 / 2.04028 / 0.55221 px, which even a
    // 0.5px grid puts in four different bins (10 / 7 / 4 / 1).  What differs is
    // the DISC EACH PART RASTERISES.  A 0.5px grid snaps those radii to
    // 5.0 / 3.5 / 2.0 / 0.5, and 0.5 with edgeSoftness 1.0 IS the single-pixel
    // delta -- so the smallest part deposits its whole alpha on one pixel
    // instead of spreading it over a 0.55px disc, and the over-count is
    // measured against a footprint the content does not have.  The refined
    // grid puts them on 4.97087 / 3.50685 / 2.03984 / 0.55232 and the mislabel
    // costs what it actually costs.
    const Case cases[] = {{2, 45.30}, {4, 76.70}};

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
            runBand(band, makeScatterParams(W, H),
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

        // Mislabelled: PINNED at the measured over-count (+45.30% at 2 parts,
        // +76.70% at 4, alpha 0.9), asserted as a band rather than a floor so
        // that neither a fix nor a worsening slips through.
        const double got = (alphaSum[1] - alpha) / alpha * 100.0;
        CAPTURE(got);
        CHECK(got > cs.misAlphaPct - 0.5);
        CHECK(got < cs.misAlphaPct + 0.5);
    }
}

TEST_CASE("volumetric parent reconstruction is EXACT in front of focus, at any part count")
{
    // With the fourth (co-located area) plane the
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
            runBand(band, makeScatterParams(W, H),
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

TEST_CASE("behind focus the residue is structural: "
          "PINNED at +36.9 / +50.2 / +56.3 / +61.0%")
{
    // Behind focus the residue is structurally irreducible by any per-bucket
    // plane.  Pinned so it is documentation-with-teeth rather than something
    // that drifts silently -- a change here means the composite changed, and
    // must be adjudicated, not re-fitted.
    const CocParams    p  = makeStandardRig(1.0f);      // focus at the near end
    const DepthBuckets bk = makeStandardBuckets(p);
    REQUIRE(bk.focusBoundary() == 0);                   // the whole range is behind focus

    const int W = 160, H = 160;
    DiscKernelLUT lut(0.0f, 60.0f, 1.0f, 1.0f);
    const float alpha = 0.9f, unpremult = 0.5f;

    // THE KERNEL-RADIUS GRID MOVES THE PARTITION COLUMN AND IT MUST BE READ
    // WITH IT: a uniform 0.5px grid reads 28.22 / 45.21 / 51.23 / 59.05 where
    // this grid reads 36.86 / 50.25 / 56.31 / 61.00, while `over` barely moves
    // (49.13 / 75.30 / 91.13 / 117.12 against 48.77 / 75.03 / 90.96 /
    // 117.12).
    //
    // THE NEW COLUMN IS THE TRUTH, and that is measured, not argued: driven
    // through a grid-free kernel sampler (one exact disc per radius, no
    // quantisation at all) the same cases read 36.91 / 50.25 / 56.40 / 61.02
    // under partition and 48.78 / 75.04 / 90.97 / 117.10 under `over`.  The
    // shipped grid is within 0.1 of that everywhere; a uniform 0.5px grid sits
    // 8.6 / 5.0 / 5.2 / 2.0 points BELOW it in the partition column.
    //
    // WHY THE GRID MOVES IT -- the mechanism, measured, not assumed.  It is NOT
    // the same-kernel collision rule: the parts here sit at radius 0.80013 /
    // 2.35257 / 3.90526 / 5.45825 / ... px, which even a 0.5px grid puts in
    // different bins (2 / 5 / 8 / 11 / ...), so nothing is ever absorbed.  What
    // differs is the DISC EACH PART RASTERISES.  A 0.5px grid snaps those radii
    // to 1.0 / 2.5 / 4.0 / 5.5, i.e. it inflates the front part -- the one
    // carrying the most alpha -- by 25% in radius and 56% in area, spreading
    // its coverage over pixels the content never covered and flattering the
    // residue downward.  This grid puts them on 0.80000 / 2.34862 / 3.90840 /
    // 5.44681 and the structural residue shows its true size: the composite is
    // not worse, the measurement is simply no longer flattered by kernel
    // quantisation.  A plain `over` of the buckets reads 48.77 / 75.03 / 90.96
    // / 117.12 on the same cases, i.e. far worse.  These are the SHIPPED
    // composite's numbers.
    struct Case { int buckets; double partitionPct; };
    const Case cases[] = {{2, 36.86}, {3, 50.25}, {4, 56.31}, {8, 61.00}};

    for (const Case& cs : cases) {
        CAPTURE(cs.buckets);
        const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ true);
        const SampleSoA soa = flattenOnePixel(fp, bk, W / 2, H / 2,
            {makeSample(bk.boundary(0), bk.boundary(cs.buckets), alpha, {alpha * unpremult})});
        REQUIRE(soa.fragmentCount() == static_cast<std::size_t>(cs.buckets));

        {
            Band band;
            band.K = bk.bucketCount(); band.C = 1; band.W = W; band.H = H;
            HoldoutSoA noHoldout;
            runBand(band, makeScatterParams(W, H), soa, noHoldout, lut);

            const double pct = (bandAlphaSum(band) - alpha) / alpha * 100.0;
            const double want = cs.partitionPct;
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
        // 1 - 0.5*0.5 = 0.75 is ALSO what plain front-to-back `over` gives:
        // dense scenes are `over`, identically, not approximately.  That is
        // why this subcase never discriminated the two candidates.
    }

    SUBCASE("receding opaque plane (four quarter-coverages) -> exactly 1")
    {
        composite({0.25f, 0.25f, 0.25f, 0.25f}, {0.25f, 0.25f, 0.25f, 0.25f},
                  {0.0f, 0.0f, 0.0f, 0.0f},
                  {0.25f * unpremult, 0.25f * unpremult, 0.25f * unpremult, 0.25f * unpremult},
                  &c, &a);
        CHECK(a == doctest::Approx(1.0f).epsilon(1e-6));
        CHECK(c == doctest::Approx(unpremult).epsilon(1e-6));
        // Plain front-to-back `over` gives 1 - 0.75^4 = 0.68359375 here, a
        // 31.6%-over-4-buckets deficit.
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
        // The configuration the area split exists for: one bucket holds
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
        // An `aCov = min(A_k, C_k)` split instead gives aCov = 0.4, local = 1,
        // accAlpha = 0.4, tClaimed = 0 and a starved residual, i.e. 0.40 -- so
        // this one number separates the two forms.
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

// ---------------------------------------------------------------------------
// THE DEPTH-RAMP MOSAIC.
//
// A destination pixel fed by a receding surface receives many fragments at
// DIFFERENT depths, each fractionally split across its own bucket pair.  Their
// kernel weights sum to 1, so the composite must return the surface's own
// alpha.  A single scalar `tClaimed` cannot: it attenuates every fragment's
// co-located rear deposit by a pooled mean over the whole claimed area, so each
// fragment's rear is occluded by every OTHER fragment's head and rear.
// Measured on this very rig in that form: -17.44% at
// alpha 0.90 / N=16 / frac 0.50, -22.39% at alpha 0.50, -38.9% at frac 0.25,
// diverging in N.  Truth is the surface's alpha and is hand-derived, not
// re-run: the fragments' weights sum to 1 and each claims its own tile, so the
// area model's answer is sum_j w_j * alpha == alpha exactly.
// ---------------------------------------------------------------------------
TEST_CASE("the depth-ramp mosaic reconstructs the surface EXACTLY, at every N, alpha and "
          "split fraction")
{
    auto composite = [](std::vector<float> cov, std::vector<float> alpha,
                        std::vector<float> colocated, std::vector<float> color,
                        float* outColor, float* outAlpha) {
        compositePixelCoveragePartition(color.data(), alpha.data(), cov.data(),
                                        colocated.data(),
                                        static_cast<int>(cov.size()), 1, 1,
                                        outColor, outAlpha);
    };

    const float unpremult = 0.8f;

    // One point fragment of kernel weight w and alpha a, transmittance-split by
    // `frac` across buckets (m, m+1): the near half claims NEW area, the far
    // half is CO-LOCATED on it.  This is scatterSpanBothBuckets()'s deposit
    // shape, re-derived here rather than called.
    auto deposit = [&](std::vector<float>& cov, std::vector<float>& alpha,
                       std::vector<float>& colo, std::vector<float>& color,
                       int m, float frac, float w, float a) {
        const float a0 = partitionAlpha(a, 1.0f - frac);
        const float a1 = partitionAlpha(a, frac);
        cov[m]        += w;
        alpha[m]      += w * a0;
        color[m]      += w * a0 * unpremult;
        colo[m + 1]   += w;
        alpha[m + 1]  += w * a1;
        color[m + 1]  += w * a1 * unpremult;
    };

    float c = -1.0f, a = -1.0f;

    // 3e-06 everywhere below is float accumulation over up to 64 fragments,
    // measured at 1.19e-06 worst across the whole (N, alpha, frac) grid.
    const float kAcc = 3e-06f;

    SUBCASE("spaced bucket pairs -- no bucket carries two fragments")
    {
        // The pure form of the mechanism: every bucket holds either one head or
        // one rear, so nothing is pooled and the ONLY thing that can go wrong is
        // which transmittance the rear is attenuated by.  With a single pooled
        // head tile this reads -4.11 / -17.44 / -21.63% at N=2/16/64 for alpha
        // 0.90; with the tile stack it is exact at every N, alpha and split
        // fraction.
        for (int N : {2, 16, 64}) {
            for (float alphaIn : {0.99f, 0.90f, 0.50f, 0.10f}) {
                for (float frac : {0.50f, 0.25f, 0.75f}) {
                    CAPTURE(N); CAPTURE(alphaIn); CAPTURE(frac);
                    std::vector<float> cov(3 * N + 2, 0.0f), al(3 * N + 2, 0.0f),
                                       co(3 * N + 2, 0.0f), col(3 * N + 2, 0.0f);
                    for (int j = 0; j < N; ++j)
                        deposit(cov, al, co, col, 3 * j, frac,
                                1.0f / static_cast<float>(N), alphaIn);
                    composite(cov, al, co, col, &c, &a);
                    CHECK(std::fabs(a - alphaIn) <= kAcc);
                    CHECK(std::fabs(c - alphaIn * unpremult) <= kAcc);
                }
            }
        }
    }

    SUBCASE("adjacent bucket pairs -- every bucket carries one head AND the previous "
            "fragment's rear")
    {
        // The dense case a smooth depth ramp actually produces, and the one
        // that decides how the head transmittance is carried across a bucket
        // that both claims new area and continues a chain.  Exact at split
        // fraction 0.5, where the composite's C_k : D_k area split of the
        // bucket's pooled alpha coincides with the true head:rear split
        // (a0 == a1).  With a single pooled head tile: -11.75 / -18.24 /
        // -21.84% at N=4/16/64 for alpha 0.90.
        for (int N : {2, 4, 16, 64}) {
            for (float alphaIn : {0.99f, 0.90f, 0.50f, 0.10f}) {
                CAPTURE(N); CAPTURE(alphaIn);
                std::vector<float> cov(N + 2, 0.0f), al(N + 2, 0.0f),
                                   co(N + 2, 0.0f), col(N + 2, 0.0f);
                for (int j = 0; j < N; ++j)
                    deposit(cov, al, co, col, j, 0.50f,
                            1.0f / static_cast<float>(N), alphaIn);
                composite(cov, al, co, col, &c, &a);
                CHECK(std::fabs(a - alphaIn) <= kAcc);
                CHECK(std::fabs(c - alphaIn * unpremult) <= kAcc);
            }
        }
    }

    SUBCASE("a mosaic of VOLUMETRIC parents, each cut into P parts, reconstructs too")
    {
        // Same mechanism one level up: a split parent's non-head parts are the
        // same co-located deposits, so the same pooling destroys them: -24.5%
        // at N=8 / alpha 0.90 with a single pooled head tile.
        for (int N : {2, 8}) {
            for (int P : {2, 3, 4}) {
                for (float alphaIn : {0.90f, 0.50f}) {
                    CAPTURE(N); CAPTURE(P); CAPTURE(alphaIn);
                    const int K = N * (P + 1) + 2;
                    std::vector<float> cov(K, 0.0f), al(K, 0.0f),
                                       co(K, 0.0f), col(K, 0.0f);
                    const float w  = 1.0f / static_cast<float>(N);
                    const float t  = 1.0f / static_cast<float>(P);
                    const float pa = partitionAlpha(alphaIn, t);
                    for (int j = 0; j < N; ++j)
                        for (int i = 0; i < P; ++i) {
                            const int k = j * (P + 1) + i;
                            if (i == 0) cov[k] += w; else co[k] += w;
                            al[k]  += w * pa;
                            col[k] += w * pa * unpremult;
                        }
                    composite(cov, al, co, col, &c, &a);
                    CHECK(std::fabs(a - alphaIn) <= kAcc);
                    CHECK(std::fabs(c - alphaIn * unpremult) <= kAcc);
                }
            }
        }
    }

    SUBCASE("the two upward errors the head-tile stack does NOT cause, bounded here")
    {
        // NEITHER IS CAUSED BY THE HEAD-TILE STACK -- both read bit-identically
        // with a single pooled tile -- but nothing else bounds them, and both
        // err in the honest-alpha contract's FORBIDDEN direction, so they are
        // pinned here rather than left as prose.  Truth is the area model, hand-derived.

        // (1) THE EXCESS REGIME REGISTERS NO TILE.  A fragment whose head lands
        // entirely on already-claimed area registers no head sub-area, so its
        // own co-located rear is attenuated by whatever tile the pixel happened
        // to be carrying instead of by (1 - local) of its own head.
        //   b0/b1: a full-coverage foreground, alpha ~0, claims the whole pixel
        //   b2:    an opaque fragment of coverage 0.05 -- all excess
        //   b3:    its rear, co-located on the 0.05 it just covered
        // TRUTH: the foreground contributes ~0, the opaque fragment covers 0.05
        // of the pixel once => 0.0501.  The composite counts it TWICE, once in
        // b2 and again in b3, for 0.0976.
        {
            std::vector<float> cov(5, 0.0f), al(5, 0.0f), co(5, 0.0f), col(5, 0.0f);
            deposit(cov, al, co, col, 0, 0.50f, 1.00f, 0.0001f);
            deposit(cov, al, co, col, 2, 0.50f, 0.05f, 1.0000f);
            composite(cov, al, co, col, &c, &a);
            const double truth = 0.0001 + (1.0 - 0.0001) * 0.05;   // 0.050095
            const double pct   = (a / truth - 1.0) * 100.0;
            CAPTURE(a); CAPTURE(pct);                       // +94.8%
            CHECK(pct > 94.81 - 1.0);
            CHECK(pct < 94.81 + 1.0);
        }

        // (2) A FRAGMENT STRADDLING THE FREE/CLAIMED BOUNDARY.  Its `excess`
        // share is attenuated by the mean over the WHOLE claimed area -- which
        // by then includes the tile this same fragment just claimed with its
        // `fit` share, and which the excess does not overlap.
        //   b0: opaque, coverage 0.5, sharp  -> claims 0.5, tClaimed 0
        //   b2: alpha 0.5, coverage 1.0, sharp -> fit 0.5 (new area, +0.25),
        //       excess 0.5 landing on b0's opaque half, which must contribute 0
        // TRUTH 0.75.  The composite reads 0.8125: its `tClaimed` after the fit
        // is (0.5*0 + 0.5*0.5)/1 = 0.25 rather than b0's own 0.
        {
            std::vector<float> cov{0.5f, 0.0f, 1.0f};
            std::vector<float> al{0.5f, 0.0f, 0.5f};
            std::vector<float> co(3, 0.0f);
            std::vector<float> col{0.5f * unpremult, 0.0f, 0.5f * unpremult};
            composite(cov, al, co, col, &c, &a);
            CAPTURE(a);                                     // +8.33% over 0.75
            CHECK(a > 0.8125f - 1e-05f);
            CHECK(a < 0.8125f + 1e-05f);
        }
    }

    SUBCASE("staggered multi-part parents at OVERLAPPING depths reconstruct EXACTLY")
    {
        // WHY ONE HEAD TILE IS NOT ENOUGH.  The subcase above is a mosaic of
        // volumetric parents whose bucket runs do NOT overlap, and a single
        // head tile is exact there too -- which is exactly why a
        // non-overlapping check cannot see this.  Give two multi-part parents
        // OVERLAPPING depth ranges -- two fog slabs at different depths, or a
        // fog slab and a point fragment, whose kernel weights tile one
        // destination pixel -- and BOTH have co-located deposits still to come
        // when a single bucket carries one parent's fit share AND the other's
        // residual chain.  With only ONE (tHead, headArea) pair the merge rule
        // has to DISCARD one of the two tiles, and the dropped parent's later
        // parts are then attenuated by a tile that is not in front of them:
        // the `t20` column below, up to +11.1% HIGH and saturating alpha to 1.
        //
        // TRUTH IS ALPHA AND NEEDS NO ORDERING ASSUMPTION: the two parents'
        // coverages sum to 1 and both fit in free area, so they tile the pixel
        // as two disjoint sub-areas at the same alpha whatever their relative
        // depth order.  sum_j w_j * alpha == alpha, exactly.  That is the
        // independent oracle these cells are re-pinned against -- a hand
        // derivation, not a re-run of the composite.
        //
        // THE THREE COLUMNS ARE HERE SO A REGRESSION IN EITHER DIRECTION IS
        // RECOGNISABLE: a pooled-mean attenuation (the `pre` column) reads
        // 4-16% LOW, a single head tile (the `t20` column) reads up to +11.1%
        // HIGH, and the head-tile stack reads the truth.
        auto addVol = [&](std::vector<float>& cov, std::vector<float>& alpha,
                          std::vector<float>& colo, std::vector<float>& color,
                          int m, int parts, float w, float a) {
            const float pa = partitionAlpha(a, 1.0f / static_cast<float>(parts));
            for (int i = 0; i < parts; ++i) {
                const int k = m + i;
                if (i == 0) cov[k] += w; else colo[k] += w;
                alpha[k] += w * pa;
                color[k] += w * pa * unpremult;
            }
        };

        struct Cell { int parts; int off; float wA; float alphaIn; double t20; double pre; };
        const Cell cells[] = {
            // parts, offset, wA,   alpha,   t20,    pre  (both for the record)
            {  2, 1, 0.50f, 0.90f,   0.000, -8.214 },   // 2 parts leave no chain
            {  2, 2, 0.50f, 0.90f,   0.000, -4.107 },
            {  3, 1, 0.50f, 0.90f,  +7.404, -10.841 },
            {  3, 1, 0.75f, 0.90f, +11.106,  -5.420 },
            {  3, 1, 0.50f, 0.50f,  +3.378,  -6.059 },
            {  4, 1, 0.50f, 0.90f,  +9.349, -11.242 },
            {  4, 2, 0.50f, 0.90f, +11.111,  -9.728 },
            {  4, 2, 0.50f, 0.50f,  +4.983,  -5.745 },
        };
        for (const Cell& cell : cells) {
            CAPTURE(cell.parts); CAPTURE(cell.off);
            CAPTURE(cell.wA); CAPTURE(cell.alphaIn);
            const int K = cell.parts + cell.off + 3;
            std::vector<float> cov(K, 0.0f), al(K, 0.0f), co(K, 0.0f), col(K, 0.0f);
            addVol(cov, al, co, col, 0, cell.parts, cell.wA, cell.alphaIn);
            addVol(cov, al, co, col, cell.off, cell.parts,
                   1.0f - cell.wA, cell.alphaIn);
            composite(cov, al, co, col, &c, &a);
            CAPTURE((a / cell.alphaIn - 1.0) * 100.0);
            CHECK(std::fabs(a - cell.alphaIn) <= kAcc);
            CHECK(std::fabs(c - cell.alphaIn * unpremult) <= kAcc);
        }

        // ... and the same shape with a POINT fragment instead of the second
        // slab, which is the commoner form: a defocused fog slab and a
        // defocused surface reaching one pixel with complementary weights.
        // NOT exact, and the reason is a DIFFERENT mechanism the head-tile
        // stack does not address: the point fragment's rear (per-unit opacity
        // partitionAlpha(0.90, 0.5) = 0.6838) lands in the same bucket as the
        // slab's second part (partitionAlpha(0.90, 1/3) = 0.5358), so ONE
        // bucket pools TWO per-unit opacities and the C_k : D_k area split
        // hands both the mean -- harness f3c/f3d's term, information lost at
        // accumulation.  Banded, and the band is BELOW zero: a single pooled
        // head tile reads +4.557% here.
        {
            std::vector<float> cov(9, 0.0f), al(9, 0.0f), co(9, 0.0f), col(9, 0.0f);
            addVol(cov, al, co, col, 0, 3, 0.5f, 0.90f);
            const float a0 = partitionAlpha(0.90f, 0.5f);
            cov[1] += 0.5f;  al[1] += 0.5f * a0;  col[1] += 0.5f * a0 * unpremult;
            co[2]  += 0.5f;  al[2] += 0.5f * a0;  col[2] += 0.5f * a0 * unpremult;
            composite(cov, al, co, col, &c, &a);
            const double pct = (a / 0.90 - 1.0) * 100.0;
            CAPTURE(pct);                       // -1.273%; +4.557% pooled
            CHECK(pct > -1.273 - 0.75);
            CHECK(pct < -1.273 + 0.75);
        }
    }

    SUBCASE("the head-tile stack holds at every depth the arrangement needs, and degrades "
            "by pooling the OLDEST chains when it runs out")
    {
        // HOW MANY TILES THE MOSAIC NEEDS: one per parent whose residual chain
        // is open at the same time.  N equal-weight, equal-alpha parents each
        // cut into P parts, started one bucket apart, keep up to min(N, P)
        // chains open at once; truth is alpha by the same disjoint-tiling
        // argument as the subcase above (the weights sum to 1).
        //
        // THE GRID BELOW DOES NOT REACH THE CAP.  kCompositeHeadTiles is 16
        // and the grid's open-chain count is min(nPar, parts) <= 6, so the
        // `<= kCompositeHeadTiles` guard below is ALWAYS TRUE and every cell is
        // asserted exact.  That is a fine check -- it is the exactness claim --
        // but it is not a cap check, and the separate overflow row further down
        // is what reaches the cap.
        //
        // ALSO NOTE THE GRID'S BLIND SPOT: every parent shares one `alphaIn`.
        // That is the constraint under which the composite CAN be exact.  Give
        // two parents DIFFERENT alphas and the upward error returns: +64.6%
        // here (parts 5/1, alphas 0.99/0.10, offset 2), against +83.5% with a
        // single head tile -- improved but not closed.  See the subcase
        // below.
        auto addVol = [&](std::vector<float>& cov, std::vector<float>& alpha,
                          std::vector<float>& colo, std::vector<float>& color,
                          int m, int parts, float w, float a) {
            const float pa = partitionAlpha(a, 1.0f / static_cast<float>(parts));
            for (int i = 0; i < parts; ++i) {
                const int k = m + i;
                if (i == 0) cov[k] += w; else colo[k] += w;
                alpha[k] += w * pa;
                color[k] += w * pa * unpremult;
            }
        };

        for (int nPar : {2, 3, 4, 6, 8}) {
            for (int parts : {2, 3, 4, 6}) {
                for (float alphaIn : {0.90f, 0.50f}) {
                    CAPTURE(nPar); CAPTURE(parts); CAPTURE(alphaIn);
                    const int K = nPar + parts + 3;
                    std::vector<float> cov(K, 0.0f), al(K, 0.0f),
                                       co(K, 0.0f), col(K, 0.0f);
                    for (int j = 0; j < nPar; ++j)
                        addVol(cov, al, co, col, j, parts,
                               1.0f / static_cast<float>(nPar), alphaIn);
                    composite(cov, al, co, col, &c, &a);
                    const double pct = (a / alphaIn - 1.0) * 100.0;
                    CAPTURE(pct);
                    // The open-chain count is min(nPar, parts); everything at or
                    // under the cap is EXACT, and nothing may ever read high.
                    if (std::min(nPar, parts) <= kCompositeHeadTiles)
                        CHECK(std::fabs(a - alphaIn) <= kAcc);
                    CHECK(pct < 0.5);
                }
            }
        }

        // THE OVERFLOW ROW, banded.  kCompositeHeadTiles is 16, so 32 parents
        // one bucket apart, each cut into 32 parts, is the case that runs the
        // stack out: the oldest tiles are folded together and the parents on
        // them are attenuated by that fold, and a partly-covered frontier tile
        // can no longer split (the split needs a free slot and must not make
        // one by merging, which renumbers the stack) so its uncovered ring is
        // over-occluded too.  Measured -5.132% at alpha 0.90 — a DEFICIT, which
        // is the direction the honest-alpha contract permits, and banded rather
        // than one-sided because a change that simply dropped the residual term
        // would satisfy a ceiling.  RAISE THE DEPTH AND THIS ROW GOES EXACT;
        // that is the trade the constant records, not a defect.
        {
            const int nPar = 32, parts = 32;
            const int K = nPar + parts + 3;
            std::vector<float> cov(K, 0.0f), al(K, 0.0f), co(K, 0.0f), col(K, 0.0f);
            for (int j = 0; j < nPar; ++j)
                addVol(cov, al, co, col, j, parts,
                       1.0f / static_cast<float>(nPar), 0.90f);
            composite(cov, al, co, col, &c, &a);
            const double pct = (a / 0.90 - 1.0) * 100.0;
            CAPTURE(pct);
            CHECK(pct > -5.132 - 0.30);
            CHECK(pct < -5.132 + 0.30);
        }
    }

    SUBCASE("THE UNEQUAL-DENSITY OVER-READ, pinned in both directions")
    {
        // THE BLIND SPOT THE SUBCASE ABOVE NAMES, TURNED INTO A GATE.  Every
        // cell in that grid shares one `alphaIn`, which is the constraint
        // under which the composite CAN be exact; harness f3c/f3d pin equal
        // density too.  Give two parents DIFFERENT densities with overlapping
        // bucket runs -- a dense fog card beside a thin one, ordinary comp
        // content -- and the composite INVENTS alpha, by up to +127.5% here
        // and +83.6% rendered (harness f3e/f3f).  That is the direction the
        // coverage-deficit rule forbids: the saturation rule never scales
        // alpha up to hide a deficit.  This subcase is the POD-level twin of
        // f3e/f3f: same arrangement, same oracle, no Nuke.
        //
        // TRUTH IS HAND-DERIVED AND NEEDS NO ORDERING ASSUMPTION, exactly as
        // in the staggered subcase above: the two parents' kernel weights sum
        // to 1 and both fit in free area, so they tile the destination pixel
        // as two DISJOINT sub-areas and the answer is
        //
        //     wA * alphaA + (1 - wA) * alphaB
        //
        // whatever their relative depth order.  It is a derivation, not a
        // re-run of the composite on its own output.
        //
        // PINNED AS BANDS, NOT CEILINGS.  These are readings of current
        // behaviour, so they are documentation-with-teeth: a mutation that
        // pushed the error DOWNWARD -- trading the over-read for a deficit of
        // the same size, which is not a fix -- has to fail them too.  Both
        // mutation directions have been run and both do.
        auto addVol = [&](std::vector<float>& cov, std::vector<float>& alpha,
                          std::vector<float>& colo, std::vector<float>& color,
                          int m, int parts, float w, float a) {
            const float pa = partitionAlpha(a, 1.0f / static_cast<float>(parts));
            for (int i = 0; i < parts; ++i) {
                const int k = m + i;
                if (i == 0) cov[k] += w; else colo[k] += w;
                alpha[k] += w * pa;
                color[k] += w * pa * unpremult;
            }
        };

        // One cell: parent A of `partsA` parts from bucket 0 at weight wA and
        // alpha alphaA, parent B of `partsB` parts from bucket `off` at the
        // complementary weight and alpha alphaB.  Returns the signed error in
        // percent of the truth, and hands back the composite's own outputs.
        auto cell = [&](int partsA, int partsB, int off, float wA,
                        float alphaA, float alphaB,
                        float* outColor, float* outAlpha) {
            const int K = partsA + partsB + off + 4;
            std::vector<float> cov(K, 0.0f), al(K, 0.0f), co(K, 0.0f),
                               col(K, 0.0f);
            addVol(cov, al, co, col, 0, partsA, wA, alphaA);
            addVol(cov, al, co, col, off, partsB, 1.0f - wA, alphaB);
            composite(cov, al, co, col, outColor, outAlpha);
            const double truth = static_cast<double>(wA) * alphaA
                               + (1.0 - static_cast<double>(wA)) * alphaB;
            return (*outAlpha / truth - 1.0) * 100.0;
        };

        // THE NAMED CELLS.  The first is the base +64.6% cell; the rest walk
        // the axes the rendered sweep walks:
        // density ratio, depth overlap (`off`), part counts and the weight
        // split.
        struct Named { int partsA, partsB, off; float wA, alphaA, alphaB;
                       double pct; const char* what; };
        const Named named[] = {
            {5, 1, 2, 0.50f, 0.99f, 0.10f,  +64.611, "the base cell, equal weights"},
            {5, 1, 2, 0.25f, 0.99f, 0.10f,  +70.964, "same, weighted to the thin card"},
            {5, 1, 2, 0.75f, 0.99f, 0.10f,  +24.030, "same, weighted to the dense card"},
            {3, 1, 1, 0.50f, 0.99f, 0.10f,  +61.439, "3 parts, adjacent"},
            {3, 3, 0, 0.50f, 0.99f, 0.10f,  +45.713, "both multi-part, coincident runs"},
            {3, 3, 2, 0.50f, 0.99f, 0.10f,  +30.475, "the same pair, runs half apart"},
            {8, 8, 0, 0.25f, 0.99f, 0.03f, +127.513, "the worst cell on the grid below"},
            {5, 1, 2, 0.50f, 0.90f, 0.30f,  +22.511, "a 3x density ratio, not 10x"},
            {5, 1, 2, 0.50f, 0.50f, 0.10f,   +9.696, "both thin, 5x ratio"},
            {5, 1, 2, 0.50f, 0.10f, 0.99f,   -3.516, "the ratio reversed: a DEFICIT"},
        };
        for (const Named& n : named) {
            CAPTURE(n.what);
            CAPTURE(n.partsA); CAPTURE(n.partsB); CAPTURE(n.off);
            CAPTURE(n.wA); CAPTURE(n.alphaA); CAPTURE(n.alphaB);
            float cc = -1.0f, aa = -1.0f;
            const double pct = cell(n.partsA, n.partsB, n.off, n.wA,
                                    n.alphaA, n.alphaB, &cc, &aa);
            CAPTURE(pct);
            // 0.5 points either side: the arithmetic is deterministic, so the
            // band is there to survive float reassociation, not to leave the
            // reading room to drift.
            CHECK(pct > n.pct - 0.5);
            CHECK(pct < n.pct + 0.5);
            // The standing invariant: whatever the alpha does, colour must
            // follow it.
            CHECK(std::fabs(cc / aa - unpremult) <= 1e-05);
        }

        // THE GRID, and the TWO STRUCTURAL CONTROLS asserted inside it.  The
        // controls are what say this measures the composite pooling two
        // DENSITIES rather than the rig:
        //   (1) `off >= partsA` -- the two parents' bucket runs do not touch,
        //       so no bucket pools them, and every such cell is EXACT at any
        //       density ratio (worst |error| over the grid: 1.5e-05%);
        //   (2) both parents single-part -- each is one head deposit with no
        //       residual chain to pool, also EXACT (worst 6e-06%).
        // So on THIS model the over-read needs both unequal density and a
        // residual chain landing in another parent's bucket.  Control (2) is
        // exactly harness f3g.
        //
        // WHERE THIS MODEL STOPS SHORT OF A RENDER.
        // `addVol` gives every part of a parent the SAME weight `w`, i.e. it
        // assumes a parent's parts rasterise the same disc.  They do not: a
        // volumetric parent's parts sit at different depths and so at
        // different CoC, and their per-pixel weights differ.  Two consequences
        // the grid cannot see, both measured RENDERED:
        //   * EQUAL density is not exempt.  Equal-alpha parents here read 0%
        //     at every offset (and -2.26% for 5-vs-1 parts at offset 2), but
        //     two equal-alpha rendered cards with spans staggered by one unit
        //     read +8.373% HIGH -- harness f3f's `ratio 1.00` cell.
        //   * `off >= partsA` is exact here but NOT rendered: depth-disjoint
        //     cards read -3.278% (harness f3f), because a residual whose disc
        //     OVERHANGS its own head tile spills onto a foreign parent's tile.
        // Both are composite-side and both vanish under tHeadIn = 1, so they
        // are the same machinery as the cells above -- treat the grid's two
        // "EXACT" controls as statements about THIS model, not about the node.
        //
        // WHAT MAKES THIS SUBCASE FAIL, MEASURED RATHER THAN ASSERTED.  Seven
        // perturbations of the composite (each built in its own tree; src/ was
        // never modified) all fail this subcase, in both directions:
        //   tHeadIn = 1 (never occlude)      worstHigh 232.6, disjoint 195.5
        //   residual carries 30% of alpha    worstHigh  52.8, worstLow -57.1
        //   tiles allocated OLDEST-first     worstHigh 152.2, worstLow -19.1
        //   the DENSEST tile occludes every
        //     residual (the conservative rule)  worstLow -37.6, over 844
        // `worstDisjoint` is a DETECTOR as well as a control (1.5e-05% here,
        // 195%/48%/113%/37.6% under those four).  `worstSinglePart` is NOT,
        // and structurally cannot be: two single-part parents have no
        // co-located deposit at all, so the composite sees one (coverage,
        // alpha) pair and cannot tell them from a single parent -- it reads
        // exact under all eight perturbations tried.  It earns its place by
        // ATTRIBUTING the defect (the over-read needs the residual chain, not
        // the density ratio as such), not by detecting one, and saying so
        // here is the point -- an assertion nobody has made fail proves
        // nothing.  Its rendered twin is harness f3g.
        {
            const int   partsList[] = {1, 2, 3, 5, 8};
            const int   offList[]   = {0, 1, 2, 3};
            const float wList[]     = {0.25f, 0.50f, 0.75f};
            struct AB { float a, b; };
            const AB abList[] = {{0.99f, 0.10f}, {0.10f, 0.99f},
                                 {0.99f, 0.03f}, {0.90f, 0.30f},
                                 {0.50f, 0.10f}};
            double worstHigh = 0.0, worstLow = 0.0;
            double worstDisjoint = 0.0, worstSinglePart = 0.0;
            int over = 0, cells = 0;
            for (int partsA : partsList)
            for (int partsB : partsList)
            for (int off : offList)
            for (float wA : wList)
            for (const AB& ab : abList) {
                float cc = -1.0f, aa = -1.0f;
                const double pct = cell(partsA, partsB, off, wA,
                                        ab.a, ab.b, &cc, &aa);
                ++cells;
                if (pct > 0.5) ++over;
                if (pct > worstHigh) worstHigh = pct;
                if (pct < worstLow)  worstLow  = pct;
                if (off >= partsA)
                    worstDisjoint = std::max(worstDisjoint, std::fabs(pct));
                if (partsA == 1 && partsB == 1)
                    worstSinglePart = std::max(worstSinglePart, std::fabs(pct));
            }
            REQUIRE(cells == 1500);
            CAPTURE(worstHigh); CAPTURE(worstLow); CAPTURE(over);
            CAPTURE(worstDisjoint); CAPTURE(worstSinglePart);
            // The controls: 1e-03 % is ~70x the measured worst and still five
            // decades under the readings above.
            CHECK(worstDisjoint    < 1e-03);
            CHECK(worstSinglePart  < 1e-03);
            // The defect itself, banded on BOTH ends of the grid and on how
            // MUCH of the grid it reaches -- a rule that fixed one cell by
            // spending another has to move one of these three.
            CHECK(worstHigh > 127.513 - 0.5);
            CHECK(worstHigh < 127.513 + 0.5);
            CHECK(worstLow  >  -9.566 - 0.5);
            CHECK(worstLow  <  -9.566 + 0.5);
            CHECK(over >= 942 - 25);
            CHECK(over <= 942 + 25);
        }
    }

    SUBCASE("the residual sees ITS OWN head, not the pooled mean -- two fragments, by hand")
    {
        // The smallest case that separates the two rules.  Two fragments of
        // weight 0.5 and alpha 0.5, split 50/50, at bucket pairs (0,1) and
        // (2,3).  a0 = a1 = 1 - sqrt(0.5) = 0.2928932.
        //
        //   b0: cov 0.5, a 0.5*a0 -> local a0, fit 0.5
        //       accAlpha  = 0.5*a0                       = 0.1464466
        //       tClaimed  = 1 - a0 = 0.7071068, claimed 0.5, tHead = 1 - a0
        //   b1: colo 0.5, aRes = 0.5*a1
        //       stack:  accAlpha += 0.5*a1*(1 - a0)       = 0.1035534
        //             -> 0.25 exactly, i.e. w * alpha for fragment 0
        //       a pooled tile uses the same value here (nothing else has
        //       claimed).
        //       tClaimed = 0.7071068 - 0.1035534/0.5 = 0.5, tHead = 0.5
        //   b2: cov 0.5, fit 0.5 -> accAlpha += 0.1464466 -> 0.3964466
        //       tClaimed = (0.5*0.5 + 0.5*0.7071068)/1 = 0.6035534
        //       tHead = 1 - a0 = 0.7071068   <-- the fragment's OWN head
        //   b3: colo 0.5, aRes = 0.5*a1
        //       stack:  accAlpha += 0.5*a1*0.7071068 = 0.1035534 -> 0.50 EXACT
        //       pooled: accAlpha += 0.5*a1*0.6035534 = 0.0883883 -> 0.4848349
        //               i.e. -3.03%, the N=2 row of the pooled column above.
        const float a0 = partitionAlpha(0.5f, 0.5f);
        std::vector<float> cov{0.5f, 0.0f, 0.5f, 0.0f};
        std::vector<float> al{0.5f * a0, 0.5f * a0, 0.5f * a0, 0.5f * a0};
        std::vector<float> co{0.0f, 0.5f, 0.0f, 0.5f};
        std::vector<float> col(4, 0.5f * a0 * unpremult);
        composite(cov, al, co, col, &c, &a);
        CHECK(a == doctest::Approx(0.5f).epsilon(1e-6));
        CHECK(c == doctest::Approx(0.5f * unpremult).epsilon(1e-6));
    }

    SUBCASE("a co-located layer removes only ITS OWN share of the claimed transmittance")
    {
        // THE INDEPENDENT IDENTITY BEHIND THE SUBTRACTIVE UPDATE: an opaque
        // surface covering the whole pixel must read alpha exactly 1, whatever
        // sits in front of it.  That truth needs no arithmetic -- it is the
        // definition of opaque -- and it is what fixes the residual's effect on
        // the claimed mean.
        //
        //   b0: cov 1.0, a 0.5      -> fit 1.0, accAlpha 0.5, tClaimed 0.5,
        //                              claimedArea 1, tHead 0.5
        //   b1: colo 0.5, a 0.25    -> aRes 0.25 over resArea 0.5, resLocal 0.5
        //         accAlpha += 0.25 * 0.5 = 0.125
        //         HALF the pixel loses half of its 0.5, so the claimed mean must
        //         fall by 0.5*0.5*0.5 = 0.125, to 0.375 -- exactly the alpha
        //         just added.  `tClaimed -= aRes*tHead/claimedArea` does that.
        //         The multiplicative `*= (1 - resLocal)` this replaces takes it
        //         to 0.25 instead, i.e. it occludes the OTHER half of the pixel
        //         with a layer that never covered it.
        //   b2: cov 1.0, a 1.0      -> all excess, accAlpha += tClaimed
        //         TOTAL 0.5 + 0.125 + 0.375 = 1.0 EXACTLY.
        //         With the multiplicative update: 0.875, i.e. a 12.5% hole
        //         punched through an opaque backing.
        std::vector<float> cov{1.0f, 0.0f, 1.0f};
        std::vector<float> al{0.5f, 0.25f, 1.0f};
        std::vector<float> co{0.0f, 0.5f, 0.0f};
        std::vector<float> col{0.5f * unpremult, 0.25f * unpremult, unpremult};
        composite(cov, al, co, col, &c, &a);
        CHECK(a == doctest::Approx(1.0f).epsilon(1e-6));
        CHECK(c == doctest::Approx(unpremult).epsilon(1e-6));
    }

    SUBCASE("a residual co-located on the SAME bucket's head is still occluded by it "
            "(the same-pixel collision shape)")
    {
        // The counter-case that decides how the head transmittance is carried:
        // here the co-located deposit's head is in the bucket it landed in, not
        // in an earlier one, so it must see (1 - local) of THAT bucket -- 0 for
        // an opaque head.  Two opaque same-pixel discs, the second not a
        // coverage head: cov 0.6 + colo 0.4 in b0, both rears in b1.
        //   aRes = 1.0 * 0.4/1.0 = 0.4, aCov = 0.6, local = 1
        //   fit 0.6 -> accAlpha 0.6, tClaimed 0, tHead 0 -> residual adds 0
        //   b1: colo 1.0 (0.4 + 0.6, clamped), attenuated by tHead 0 -> 0
        // TOTAL 0.6, i.e. the two surfaces cover 0.6 of the pixel ONCE.
        std::vector<float> cov{0.6f, 0.0f};
        std::vector<float> al{1.0f, 1.0f};
        std::vector<float> co{0.4f, 1.0f};
        std::vector<float> col{unpremult, unpremult};
        composite(cov, al, co, col, &c, &a);
        CHECK(a == doctest::Approx(0.6f).epsilon(1e-6));
        CHECK(c == doctest::Approx(0.6f * unpremult).epsilon(1e-6));
    }

    SUBCASE("WHAT THE HEAD-TILE STACK DOES NOT FIX: a bucket pooling two DIFFERENT "
            "per-unit opacities")
    {
        // THE RESIDUAL THE HEAD-TILE STACK DOES NOT FIX, ISOLATED.  Both fragments
        // occupy the SAME bucket pair but at different split fractions, so the
        // bucket's pooled alpha carries two different per-unit opacities and the
        // composite's C_k : D_k area split cannot recover them -- it hands both
        // sub-layers the same a/(C_k + D_k), which is the only split that does
        // not invent a difference, and is right only when the two
        // really are equal.  This is harness f3c/f3d's mechanism, NOT the mosaic
        // one above, and no per-bucket rule can undo it: the information is gone
        // at accumulation, not at composition.
        //
        // THE CONTROL immediately below is what attributes it: the same two
        // fragments at the SAME split fraction are EXACT, so the number belongs
        // to the fraction mixture and not to pooling two fragments as such.
        const float w = 0.5f, alphaIn = 0.9f;
        {
            std::vector<float> cov(3, 0.0f), al(3, 0.0f), co(3, 0.0f), col(3, 0.0f);
            deposit(cov, al, co, col, 0, 0.10f, w, alphaIn);
            deposit(cov, al, co, col, 0, 0.90f, w, alphaIn);
            composite(cov, al, co, col, &c, &a);
            // BANDED, not a ceiling: measured -12.41%, and a one-sided bound
            // would be met by a mutation that removed the residual term
            // altogether.
            CHECK(a > 0.9f * (1.0f - 0.140f));
            CHECK(a < 0.9f * (1.0f - 0.108f));
            CHECK(std::fabs(c / a - unpremult) <= 1e-05);
        }
        {
            std::vector<float> cov(3, 0.0f), al(3, 0.0f), co(3, 0.0f), col(3, 0.0f);
            deposit(cov, al, co, col, 0, 0.50f, w, alphaIn);
            deposit(cov, al, co, col, 0, 0.50f, w, alphaIn);
            composite(cov, al, co, col, &c, &a);
            CHECK(std::fabs(a - alphaIn) <= kAcc);        // the control: EXACT
        }
    }

    SUBCASE("...and the same term on a DENSE ramp at any split fraction but 0.5")
    {
        // The rendered form of the above: fragment j at bucket pair (j, j+1) at
        // split fraction 0.25 or 0.75 rather than 0.5.  These are the numbers
        // harness g4's remaining deficit is made of, and they have a CLOSED
        // FORM that is derived here rather than re-measured -- which is what
        // pins them independently of the code.
        //
        // NOTE THE TRIGGER: EVERY fragment below carries the SAME split
        // fraction, so this is NOT "fragments at different split fractions".
        // On a dense ramp bucket k carries fragment k's head at per-unit
        // opacity a0 = partitionAlpha(alpha, 1-frac) and fragment k-1's rear at
        // a1 = partitionAlpha(alpha, frac); those differ for every frac != 0.5,
        // so ONE bucket already pools two per-unit opacities.  The composite's
        // C_k : D_k area split can only hand both sub-layers the mean
        // m = (a0 + a1)/2, so each fragment's tile reads
        //
        //     1 - (1 - m)^2      instead of      1 - (1 - a0)(1 - a1) = alpha
        //
        // and since (1-m) is the arithmetic mean of (1-a0) and (1-a1), AM-GM
        // makes (1-m)^2 >= (1-a0)(1-a1): the error is a DEFICIT for every
        // fraction but 0.5, where it vanishes.  It is also independent of N,
        // and that is the check.  With a SINGLE carried tile the readings here
        // are -2.42/-3.69/-4.00% (frac 0.25) and -5.79/-4.53/-4.21% (frac 0.75)
        // at N=4/16/64, i.e. N-dependent and asymmetric in the fraction,
        // because that tile mixes this pooling term with the mosaic error.
        // With the head-tile stack the mosaic term is gone and the reading is
        // the closed form to five decimals at every N and both fractions.
        for (float frac : {0.25f, 0.75f}) {
            const double a0    = 1.0 - std::pow(1.0 - 0.90, 1.0 - frac);
            const double a1    = 1.0 - std::pow(1.0 - 0.90, frac);
            const double mean  = 0.5 * (a0 + a1);
            const double want  = 1.0 - (1.0 - mean) * (1.0 - mean);   // hand-derived
            const double wantPct = (want / 0.90 - 1.0) * 100.0;       // -4.1070%
            CHECK(wantPct < -0.5);                                    // AM-GM: a deficit
            for (int n : {4, 16, 64}) {
                CAPTURE(n); CAPTURE(frac); CAPTURE(wantPct);
                std::vector<float> cov(n + 2, 0.0f), al(n + 2, 0.0f),
                                   co(n + 2, 0.0f), col(n + 2, 0.0f);
                for (int j = 0; j < n; ++j)
                    deposit(cov, al, co, col, j, frac,
                            1.0f / static_cast<float>(n), 0.90f);
                composite(cov, al, co, col, &c, &a);
                const double pct = (a / 0.90 - 1.0) * 100.0;
                CAPTURE(pct);
                // 0.02 points, against a measured worst departure from the
                // closed form of 0.0003 over this grid: the residue is float
                // accumulation over up to 64 fragments, not a second term.
                CHECK(std::fabs(pct - wantPct) < 0.02);
            }
        }
    }
}


TEST_CASE("the g4 rig's low-alpha over-read is the SCATTER's weight over-delivery, "
          "not the composite's")
{
    // THE MECHANISM BEHIND HARNESS g5, pinned at POD level on a faithful
    // 1-column model of scene (g)'s ground ramp (THE ISOLATED UNIT RIG -- the
    // target numbers live in the harness, rendered from the g4 rig itself;
    // this model reproduces every rendered cell of the alpha x K sweep to
    // within 0.35 points).  Ground ramp z(y) = 1720/(300-y), manual CoC
    // size 86 / focus 10, so radius(y) = 0.5*|y-128| -- the scene's own
    // deliberately steep slope.  A destination pixel y0 receives from each
    // source row y' the row-sum weight of a normalised disc of radius r(y')
    // at offset y'-y0, all at depth z(y'), through the REAL DepthBuckets,
    // fragmentDeposit() and compositePixelCoveragePartition().
    //
    // THE FINDING, which is neither f3c/f3d's pooling seen from its positive
    // side nor a covariance / second-moment effect: each
    // disc is normalised over its OWN kernel, and on a steep CoC gradient
    // the adjoint sum at a destination pixel is NOT 1 -- nearer-focus rows
    // arrive with denser discs than farther rows lose, and the deposited
    // weight sums to ~1.07 here.  The composite then honestly attenuates
    // the spurious excess by tClaimed ~ (1 - alpha): fully visible as
    // alpha -> 0, absorbed by the area clamp at alpha = 1.  No composite
    // rule at ANY plane count can remove it: these same planes arise from
    // ~190 independent small cards at the same depths (every deposit here
    // is a legitimate lone fragment), for which the over-composited truth
    // is HIGHER than alpha -- one plane set, two truths.  And the scatter-
    // side fix, per-destination-pixel renormalisation, breaks genuine
    // overlap (two full-coverage 0.5 fog layers: 0.75 exact as it stands, 0.50
    // renormalised).
    const float slope = 86.0f * 10.0f / 1720.0f;            // 0.5 px per row
    const float zMin  = 1720.0f / 300.0f;                   // row 0
    const float zMax  = 1720.0f / 45.0f;                    // row 255

    auto rowWeight = [](float r, float dy) -> float {
        if (std::fabs(dy) > r)
            return 0.0f;                                    // outside the disc
        const float chord = 2.0f * std::sqrt(r * r - dy * dy);
        return chord / (3.14159265f * r * r);               // row / disc area
    };

    // One interior destination pixel: deposit, optionally renormalised so the
    // weights sum to exactly 1, composite, return the excursion a/alpha - 1.
    // outSumW reports the raw deposited-weight sum (the over-delivery).
    auto pixelExcursion = [&](const deepc::DepthBuckets& buckets, int bucketCount,
                              float alpha, int y0, bool renormalise,
                              double* outSumW) -> double {
        std::vector<float> ws(256, 0.0f);
        double sumW = 0.0;
        for (int y = 0; y < 256; ++y) {
            const float r = slope * std::fabs(static_cast<float>(y) - 128.0f);
            const float w = (r < 0.5f)
                          ? ((y == y0) ? 1.0f : 0.0f)       // sharp fast path
                          : rowWeight(r, static_cast<float>(y - y0));
            ws[y] = w;
            sumW += w;
        }
        if (outSumW != nullptr)
            *outSumW = sumW;
        std::vector<float> cov(bucketCount, 0.0f), al(bucketCount, 0.0f),
                           co(bucketCount, 0.0f), col(bucketCount, 0.0f);
        for (int y = 0; y < 256; ++y) {
            float w = ws[y];
            if (!(w > 0.0f))
                continue;
            if (renormalise)
                w = static_cast<float>(w / sumW);
            const float z = 1720.0f / (300.0f - static_cast<float>(y));
            const deepc::BucketWeight  bw = buckets.bucketOf(z);
            const deepc::BucketDeposit d  = deepc::fragmentDeposit(bw, alpha);
            cov[d.index0] += w;
            al[d.index0]  += w * d.alpha0;
            col[d.index0] += w * d.alpha0 * 0.8f;
            if (d.index1 != d.index0) {
                co[d.index1]  += w;
                al[d.index1]  += w * d.alpha1;
                col[d.index1] += w * d.alpha1 * 0.8f;
            }
        }
        float c = -1.0f, a = -1.0f;
        deepc::compositePixelCoveragePartition(col.data(), al.data(), cov.data(),
                                               co.data(), bucketCount, 1, 1,
                                               &c, &a);
        return a / alpha - 1.0;
    };

    // Interior mean over the same rows harness sceneG averages (both sides of
    // focus, minus the small-CoC band), decimated x3 for speed.  MEASURED, not
    // assumed: the decimated subset reads 0.08-0.47 points ABOVE the full set
    // (step 3 lands on rows whose sum(w) runs slightly high -- a sampling bias
    // of the row subset, identical at every K, not a different mechanism), and
    // every band below allows for it.  Anyone tightening a band must re-check
    // against step 1.
    auto interiorMean = [&](const deepc::DepthBuckets& buckets, int bucketCount,
                            float alpha, bool renormalise,
                            double* outMeanW) -> double {
        double acc = 0.0, wAcc = 0.0;
        int n = 0;
        for (int y0 = 66; y0 < 190; y0 += 3) {
            if (y0 >= 122 && y0 < 134)
                continue;
            double sw = 0.0;
            acc  += pixelExcursion(buckets, bucketCount, alpha, y0,
                                   renormalise, &sw);
            wAcc += sw;
            ++n;
        }
        if (outMeanW != nullptr)
            *outMeanW = wAcc / n;
        return acc / n;
    };

    const deepc::CocParams params = deepc::makeCocParams(
        deepc::CocMode::Manual, 50.0f, 2.8f, 36.0f, 10.0f, 1000.0f,
        256.0f, 1.0f, 1.0f, 1.0f, 100.0f, 86.0f);

    for (int k : {4, 16, 64}) {
        CAPTURE(k);
        deepc::DepthBuckets buckets;
        buckets.buildBoundedDeltaCoc(params, zMin, zMax, k);

        // (1) THE RIG OVER-DELIVERS, and at vanishing alpha the composite
        // hands that number straight through: excursion(alpha->0) == sumW - 1
        // to 0.1 points, AT EVERY K -- the K-invariance is the composite-
        // independence (the harness rendered +7.124/+7.063/+7.037% at
        // K=4/16/64 for alpha 0.01 on the real kernel).
        double meanW = 0.0;
        const double limit = interiorMean(buckets, k, 0.001f, false, &meanW);
        CAPTURE(meanW); CAPTURE(limit);
        CHECK(meanW - 1.0 > 0.05);                  // ~ +0.068 on this rig
        CHECK(meanW - 1.0 < 0.09);
        CHECK(std::fabs(limit - (meanW - 1.0)) < 1.0e-03);

        // (2) THE ATTRIBUTION CONTROL: renormalise the weights per pixel --
        // deliver exactly 1 -- and every low-alpha cell flips to a small
        // DEFICIT.  What the COMPOSITE contributes at low alpha is in the
        // permitted direction; the whole forbidden-direction excursion
        // enters at scatter time.  (Banded: a composite regression that
        // inflated low alpha would push this back over zero.)
        const double renorm10 = interiorMean(buckets, k, 0.10f, false, nullptr);
        const double renormed = interiorMean(buckets, k, 0.10f, true, nullptr);
        CAPTURE(renorm10); CAPTURE(renormed);
        CHECK(renormed <= 0.0);
        CHECK(renormed > -0.015);                   // -0.0025..-0.0088 measured (step 3)

        // (3) THE RAW READINGS THEMSELVES, banded, so this model stays
        // anchored to the rendered sweep it reproduces: alpha 0.10 reads
        // HIGH (the forbidden direction) and alpha 0.90 at K >= 16 reads
        // LOW (g4's own deficit) on the very same weights.
        CHECK(renorm10 > 0.04);                     // +0.058..+0.066 measured (step 3)
        CHECK(renorm10 < 0.07);
        if (k >= 16) {
            const double raw90 = interiorMean(buckets, k, 0.90f, false, nullptr);
            CAPTURE(raw90);
            CHECK(raw90 < -0.02);                   // -0.032/-0.049 measured (step 3)
            CHECK(raw90 > -0.08);
        }
    }
}


TEST_CASE("colour:alpha ratio is a standing invariant of the composite over randomised planes")
{
    // One invariant instead of three cases: clamping one of a premultiplied
    // pair and not the other is a defect the residual term, the area split and
    // the saturation pass are all capable of.
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

        // DELIBERATELY NOT SCOPED TO THE UNCLAMPED RESULT.  Excluding
        // `outAlpha == 1` would exclude exactly where the invariant breaks: a
        // final clamp that touches the alpha and leaves the colour beside it
        // alone ships, for a pixel whose accumulated alpha exceeded 1, a
        // premultiplied colour:alpha ratio above the input's -- "clamp one of a
        // premultiplied pair and not the other", and the production path
        // reaches it (see the fog case below).  The clamp rescales both, so the
        // invariant holds everywhere.
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
    // THE REGRESSION GATE for the clamp asymmetry.  Ordinary
    // overlapping volumetric fog drives compositePixelCoveragePartition's
    // accAlpha above 1 -- several co-located residuals attenuate by
    // aRes/D_k, which is weaker than the alpha each of them adds whenever
    // D_k > aRes, so the sum over buckets is not bounded the way the
    // per-bucket terms are.  On THIS fixture 19 of 576 pixels clamp, and
    // without the colour rescaled alongside the alpha the worst of them ships
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
            flattenPixelToSoA(fp, bk, x, y, v, scratch, soa, nullptr, nullptr, nullptr);
        }
    REQUIRE(soa.fragmentCount() > 1000u);

    Band band;
    band.K = K; band.C = 1; band.W = W; band.H = H;
    HoldoutSoA none;
    runBand(band, makeScatterParams(W, H), soa, none, lut);

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
    // A group can never contain two parts of the same parent (they are cut AT
    // the boundaries, so they sit in distinct buckets), which is
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
                    runBand(band, makeScatterParams(W, H),
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

    SUBCASE("two distinct co-located point parents: the knob does not move them")
    {
        // WHY THE KNOB IS NOT THE VARIABLE HERE.  Without the collision pass
        // this case reads alpha 0.694518 with a new-area plane of 2.0 at
        // pre_merge OFF, against 0.580000 / 1.0 at ON -- which invites reading
        // ON as "the accurate branch".  Both readings are of the SAME defect:
        // two depth-disjoint parents at one pixel deposit into one bucket
        // ADDITIVELY and both claim the pixel's area, so the composite clamps
        // `cov` 2.0 -> 1.0 and `a` alongside it.  pre_merge ON happens to group
        // these two (same containing bucket, radii 0.0006px apart) and so
        // accidentally produces the right answer.  The knob is the difference
        // between "the collision was resolved" and "it was not", never between
        // accurate and inaccurate.
        //
        // THE TRUE VALUE IS DERIVED, NOT MEASURED: two point samples at one
        // pixel flatten to a sequential `over`, which is what a DeepToImage
        // flatten of this pixel gives and what the size-0 parity gate is
        // written against.  0.3 over 0.4 is 0.3 + 0.4*0.7 = 0.58 exactly, and
        // ONE surface at one pixel covers its kernel's area ONCE.
        //
        // Both are now knob-INDEPENDENT, which is the point: the collision
        // merge is a correctness pass, not a perf option.
        const double expected = 0.3 + 0.4 * 0.7;      // 0.58, exact
        double alphaSum[2] = {0, 0}, newArea[2] = {0, 0};

        for (int pm = 0; pm < 2; ++pm) {
            const FlattenParams fp = makeFlattenParams(p, 1, pm != 0);
            const SampleSoA soa = flattenOnePixel(fp, bk, W / 2, H / 2,
                {makeSample(9.0f, 9.0f, 0.3f, {0.3f}),
                 makeSample(9.02f, 9.02f, 0.4f, {0.4f})});
            // ONE fragment either way now: they share a bucketOf() assignment
            // and are both on the sharp path, so they are one kernel.
            REQUIRE(soa.fragmentCount() == 1u);
            Band band;
            band.K = bk.bucketCount(); band.C = 1; band.W = W; band.H = H;
            HoldoutSoA noHoldout;
            runBand(band, makeScatterParams(W, H),
                    soa, noHoldout, lut);
            alphaSum[pm] = bandAlphaSum(band);
            for (int k = 0; k < band.K; ++k)
                newArea[pm] += planeSum(band.planes.weight, k, band.pixels());
        }

        CHECK(std::fabs(alphaSum[1] - expected) <= 1e-06);
        CHECK(std::fabs(alphaSum[0] - expected) <= 1e-06);
        CHECK(newArea[1] == doctest::Approx(1.0).epsilon(1e-5));
        CHECK(newArea[0] == doctest::Approx(1.0).epsilon(1e-5));
        // The knob moves NEITHER quantity any more.
        CHECK(alphaSum[0] == doctest::Approx(alphaSum[1]).epsilon(1e-9));
    }

    SUBCASE("a group that mixes a NON-head part with a following head keeps BOTH coverages "
            "(the survival rule is an OR, not the group head's flag)")
    {
        // The configuration the OR exists for: parent A is cut at
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
        runBand(band, makeScatterParams(W, H),
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
    // The memory-limit knob and the code must not drift apart.  The formula is
    // (C+3), not (C+2): colour + alpha + new area + co-located area.
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
// Holdout
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
    // The commonest holdout there is: a solid card.  Read against the ΔCoC
    // bucket boundaries this card at z=50 starts occluding at z=10.9 and erases
    // a fragment at z=15 by 98%.  Against the decoupled uniform-in-z set it
    // must be essentially unattenuated in front of the card and fully
    // attenuated behind it.
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
        flattenPixelToSoA(fp, bk, W / 2, H / 2, behind, scratch, soa, nullptr, nullptr, nullptr);
        // In front of the card (z=3): unattenuated.
        std::vector<SampleRecord> front{makeSample(3.0f, 3.0f, 0.8f, {0.4f})};
        flattenPixelToSoA(fp, bk, W / 2 - 4, H / 2, front, scratch, soa, nullptr, nullptr, nullptr);

        const ScatterParams sp = makeScatterParams(W, H);
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
            const SampleSoA soa = flattenOnePixel(fp, bk, W / 2, H / 2,
                {makeSample(depth, depth, 0.8f, {0.4f})});
            const ScatterParams sp = makeScatterParams(W, H);

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
        const ScatterParams sp = makeScatterParams(W, H);

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
        runBand(band, makeScatterParams(W, H),
                soa, view, lut);
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

        Band band;
        band.K = bk.bucketCount(); band.C = 1; band.W = W; band.H = H;
        runBand(band, makeScatterParams(W, H), soa, view, lut);
        const double got = bandAlphaSum(band);

        // TWO-SIDED band: a one-sided `< 1e-12` cannot tell the floored chord
        // from an outright 0.  The fragment's whole kernel weight (sums to 1
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

TEST_CASE("bandBudgetBytes: bucket planes + holdout LUT + resident SoA, "
          "against hand-derived byte counts")
{
    // The 4K default band: K=16, C=4, 4096x64.
    // Planes: K*W*B*(C+3)*4 = 16*4096*64*7*4 = 117,440,512 (~117MB).
    CHECK(bandBudgetBytes(16, 4, 4096, 64, false, 0.0)
          == doctest::Approx(117440512.0));

    // The holdout term, which bytesForBand() does NOT carry: (K+1)*W*B*4 =
    // 17*4096*64*4 = 17,825,792, i.e. 17.0 MB per 4096x64 band at K=16.
    CHECK(bandBudgetBytes(16, 4, 4096, 64, true, 0.0)
          - bandBudgetBytes(16, 4, 4096, 64, false, 0.0)
          == doctest::Approx(17825792.0));

    // The SoA term, at the ~100 B/fragment RESIDENT figure (61 B logical).
    CHECK(kSoAResidentBytesPerFragment == doctest::Approx(100.0));
    CHECK(bandBudgetBytes(16, 4, 4096, 64, false, 1.0e6)
          == doctest::Approx(117440512.0 + 1.0e8));

    // K=128 planes: 128*4096*64*7*4 = 939,524,096 (~940MB).
    CHECK(bandBudgetBytes(128, 4, 4096, 64, false, 0.0)
          == doctest::Approx(939524096.0));

    // Degenerate inputs count as zero, never negative or wrapped.
    CHECK(bandBudgetBytes(0, 4, 4096, 64, true, 0.0) == doctest::Approx(0.0));
    CHECK(bandBudgetBytes(16, 4, -1, 64, true, 100.0)
          == doctest::Approx(100.0 * kSoAResidentBytesPerFragment));
}

TEST_CASE("planBands: shrink-to-fit floors at 1 row and the concurrent cap "
          "floors at 1 band — never 0, never a deadlock")
{
    const auto noFragments = [](int) { return 0.0; };

    // Fits outright: 4GB limit, 2160 rows, K=16 C=4 W=4096, B=256.
    // bytes(256) = 16*4096*256*7*4 = 469,762,048; cap = floor(4GiB / that)
    // = 9; bandCount = ceil(2160/256) = 9.
    {
        const BandPlan p = planBands(4.0 * 1024.0 * 1024.0 * 1024.0,
                                     2160, 16, 4, 4096, false, 256, noFragments);
        CHECK(p.bandHeight == 256);
        CHECK(p.bandCount == 9);
        CHECK(p.maxInFlight == 9);
    }

    // Shrinks: 64MB limit. bytes(256)=470MB > 64MB -> 128 (235MB) -> 64
    // (117MB) -> 32 (58.7MB fits). One band in flight (64MB/58.7MB < 2).
    {
        const BandPlan p = planBands(64.0 * 1024.0 * 1024.0,
                                     2160, 16, 4, 4096, false, 256, noFragments);
        CHECK(p.bandHeight == 32);
        CHECK(p.bandCount == (2160 + 31) / 32);
        CHECK(p.maxInFlight == 1);
    }

    // Even ONE row over the limit: bandHeight floors at 1 and the cap floors
    // at 1 — the band is over budget and still gets its slot (the design's
    // "never deadlock at 0").  bytes(1) = 16*4096*7*4 = 1,835,008 > 1MB.
    {
        const BandPlan p = planBands(1.0 * 1024.0 * 1024.0,
                                     2160, 16, 4, 4096, false, 256, noFragments);
        CHECK(p.bandHeight == 1);
        CHECK(p.bandCount == 2160);
        CHECK(p.maxInFlight == 1);
    }

    // The fragment estimator participates in the shrink: 20 spp over a 4096
    // window at 100 B resident dominates the planes and forces the halving.
    // bytes(256) with fragments = 470MB + 4096*256*20*100 = 2.56GB.
    {
        const auto sppFragments = [](int b) {
            return 4096.0 * static_cast<double>(b) * 20.0;
        };
        const BandPlan withFrag = planBands(1.0 * 1024.0 * 1024.0 * 1024.0,
                                            2160, 16, 4, 4096, false, 256,
                                            sppFragments);
        const BandPlan without  = planBands(1.0 * 1024.0 * 1024.0 * 1024.0,
                                            2160, 16, 4, 4096, false, 256,
                                            noFragments);
        CHECK(withFrag.bandHeight < without.bandHeight);
        CHECK(withFrag.bandHeight == 64);   // 64: 117MB + 524MB = 642MB <= 1GB
        CHECK(withFrag.maxInFlight == 1);
        CHECK(without.bandHeight == 256);
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
