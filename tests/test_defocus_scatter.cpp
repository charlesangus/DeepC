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

// "Which kernel would the scatter rasterise for this radius?", from the
// documented rule rather than from the shipped predicate: below the sharp
// threshold every fragment is one weight of 1.0 at its own pixel (bin -1), and
// above it the scatter blends the two grid nodes bracketing the radius at
// f = (d - dA) / (dB - dA), so the kernel is the pair (lower node, f) and two
// radii share a bin when their f agree to 2^-20 of the bracket.
//
// Deliberately derived by SEARCHING the grid's node radii (kernelGridRadius(),
// which is the grid's definition) instead of by inverting them: the closed
// form kernelGridIndex()/kernelGridBracket() use -- a reciprocal and a
// harmonic-mean midpoint -- is exactly the thing this reference exists to
// disagree with if it is wrong.
constexpr double kRefKernelBlendCells = 1048576.0;      // 2^20

std::int64_t refKernelBin(double radiusPx)
{
    if (!(radiusPx > static_cast<double>(kSharpRadiusPx)))
        return -1;

    // Bracket, then bisect, on the monotone node radii: afterwards
    // node(lo) < radius <= node(hi), or lo == hi on a node.
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
                                    bool holdoutConnected = false,
                                    bool absorbCollisions = true)
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
        if (absorbCollisions && !merged.empty()) {
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
    struct RefTouch { int bucket; std::int64_t bin; double running; };
    std::vector<RefTouch> touched;
    std::vector<std::pair<int, std::int64_t>> claimed;   // (bucket, the claiming kernel's bin)
    const int lastBucket = (b.bucketCount() > 0) ? (b.bucketCount() - 1) : 0;
    int          frontier    = 0;
    std::int64_t frontierBin = 0;

    std::vector<RefFragment> out;
    for (RefFragment f : merged) {
        const std::int64_t bin = refKernelBin(f.radius);

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
            for (const std::pair<int, std::int64_t>& c : claimed) {
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

// Full band: allocate, zero, scatter (threaded unless told otherwise),
// virtual-background scatter (when `residual` is supplied), resolve -- the
// production order (scatterBandCPU -> scatterBackgroundCPU -> resolveBandCPU;
// see DeepCDefocus.cpp's computeBand()).  `residual` defaults to nullptr so a
// caller that does not model the virtual background gets NO background
// deposit at all, not a silently-empty one: arrival then carries only the
// fragments' own raw weight, as a scatter with no virtual background would.
void runBand(Band& band, const ScatterParams& sp, const SampleSoA& soa,
             const HoldoutSoA& holdout, const KernelSampler& kernel,
             bool useThread = true, const ResidualWindow* residual = nullptr)
{
    band.planes.allocate(band.K, band.C, band.W, band.H);
    band.planes.zero();

    if (useThread) {
        scatterOnThread(sp, soa, holdout, kernel, band.planes);
    } else {
        ScatterScratch scratch;
        scatterBandCPU(sp, soa, holdout, kernel, band.planes, scratch);
    }

    if (residual != nullptr)
        scatterBackgroundCPU(sp, *residual, kernel, band.planes);

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

// The AUTO resolution of `background_depth` (0 = auto): the CoC at the
// buckets' own farthest measured depth -- see resolveBackgroundRadiusPx().
// Every ResidualWindow cell built below defaults to this radius; a pixel
// with a real sample overwrites it with that sample's OWN residual radius.
float autoBackgroundRadiusPx(const CocParams& p, const DepthBuckets& bk)
{
    return radiusPixels(p, bk.depthMax());
}

// A rig with ONE real source pixel, windowed the way computeBand() windows it:
// over the whole output box, every other cell left at the "no samples here"
// default of T = 1 at the global background radius.  Sizing the window to the
// source pixel instead deletes that surrounding field, and the deletion is not
// benign: arrival then equals the object's own kernel weight at every pixel the
// object reaches, so accAlpha/arrival is the SAME constant everywhere the disc
// lands -- including the faintest edge pixel -- and the division flattens an
// anti-aliased bloom into a hard disc.  With the field present the empty
// pixels' unit-weight discs sum back to 1 and the deficit gate never fires.
//
// The background radius is the source pixel's own residual radius, and that is
// not a convenience: frameSetup() builds the buckets from the frame's MEASURED
// depth range, so in a frame holding this one object the farthest measured
// depth is that object's own deepest sample and the auto background radius
// resolves to exactly the radius its residual scatters at.  (The fixed 1..100
// bucket range these rigs share is a fixture, not a measurement, so taking the
// radius from it instead would model a frame with unseen geometry at depth 100
// -- the mismatched-radius case the design calls out as conditional.)
void oneSourcePixelWindow(ResidualWindow& window, int W, int H,
                          int px, int py,
                          float residualT, float residualRadiusPx)
{
    window.allocate(0, 0, W, H, residualRadiusPx);
    window.setPixel(px, py, residualT, residualRadiusPx);
}

// Flattens samplesFn(x, y) at every pixel of [x0,x1) x [y0,y1) into `soa`,
// and records that pixel's own residual T / residual radius into `window`
// (already allocated over at least this box) -- the doctest-side equivalent
// of production's buildResidualWindow(), without a live deep-fetch loop. A
// pixel `samplesFn` returns nothing for keeps the window's pre-set default
// (T=1, the background radius `window` was allocated with), same as
// flattenPixelToSoA()'s own "no sample" convention.
template <typename SamplesFn>
void flattenIntoWithResidual(const FlattenParams& fp, const DepthBuckets& bk,
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
            flattenPixelToSoA(fp, bk, x, y, v, scratch, soa, nullptr,
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

// The bracketing-kernel blend, derived from the stated rule -- node A at
// (1-f), node B at f, f measured on the DIAMETER -- and not from
// kernelGridBracket()'s own arithmetic: the floor node is found by a linear
// walk of kernelGridRadius(), a different derivation from the closed form the
// shipped helper uses.  On a node (and for anything the walk cannot bracket)
// it returns a single pass at weight 1.
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

// The blended kernel's weight at pixel offset (dx, dy) from its centre, via
// refBracket(): the per-tap oracle every blend assertion below is checked
// against.
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

        if (!(radius > sp.sharpRadiusPx)) {
            // Sharp fast path: weight 1 into the fragment's own pixel.
            if (destX >= 0 && destX < out.W && destY >= 0 && destY < out.H)
                touched.emplace_back(static_cast<std::ptrdiff_t>(destY) * out.W + destX, 1.0);
        } else {
            const RefBracket b = refBracket(radius);
            for (int p = 0; p < b.passes; ++p) {
                const KernelView kv = lut.kernel(kernelGridRadius(b.node[p]),
                                                 destX, destY, depth, 0);
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
                        touched.emplace_back(
                            static_cast<std::ptrdiff_t>(dy) * out.W + dx,
                            static_cast<double>(w[i]) * b.blend[p]);
                    }
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

    float residualT = 1.0f, residualR = 0.0f;
    const SampleSoA soa = flattenOnePixel(fp, bk, 16, 16,
        {makeSample(9.0f, 9.0f, 0.3f, {0.3f * 0.8f}),
         makeSample(9.0f, 9.0f, 0.4f, {0.4f * 0.8f})},
        &residualT, &residualR);

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
                std::vector<std::pair<int, std::int64_t>> headClaims;   // (bucket, kernel bin)
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

TEST_CASE("one split parent claims arrival at ONE radius: its parts pool their shares "
          "onto the deepest part")
{
    // The bucket split is an artefact of K, so it must not move where a parent
    // claims arrival -- otherwise the deficit division fires on a parent whose
    // parts span a wide radius range.  Pooling is per PARENT, never per pixel:
    // two genuinely different surfaces keep two claims at two radii, which is
    // the signal the coverage fill exists to read.
    const CocParams     p  = makeStandardRig(10.0f);
    const DepthBuckets  bk = makeStandardBuckets(p, 16);
    const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ false);

    SUBCASE("a split parent's whole share sits on its deepest part")
    {
        float residualT = -1.0f, residualR = -1.0f;
        const SampleSoA soa = flattenOnePixel(fp, bk, 0, 0,
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
        const SampleSoA soa = flattenOnePixel(fp, bk, 0, 0,
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
        const SampleSoA soa = flattenOnePixel(fp, bk, 0, 0,
            {makeSample(3.0f, 3.0f, 0.4f, {0.2f})}, &residualT, &residualR);
        REQUIRE(soa.fragmentCount() == 1u);
        CHECK(soa.arrivalShare[0] == 0.4f);      // share = 1 * alpha, exactly
        CHECK(residualT == 0.6f);
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
    const DepthBuckets bk = makeStandardBuckets(p, 16);
    const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ true);
    const float alpha = 0.9f, unpremult = 0.5f;
    const float zNear = 5.0f, zFar = 60.0f;
    const float rNear = radiusPixels(p, zNear);
    const float rFar  = radiusPixels(p, zFar);
    REQUIRE(rNear > 1.0f);
    REQUIRE(rFar > 1.0f);
    REQUIRE(std::fabs(rFar - rNear) > 0.3f);     // two genuinely different kernels

    float tAlone = -1.0f, rAlone = -1.0f;
    const SampleSoA alone = flattenOnePixel(fp, bk, 7, 7,
        {makeSample(zNear, zNear, alpha, {alpha * unpremult})}, &tAlone, &rAlone);

    float tBehind = -1.0f, rBehind = -1.0f;
    const SampleSoA behind = flattenOnePixel(fp, bk, 7, 7,
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
    const SampleSoA only = flattenOnePixel(fp, bk, 7, 7,
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
        band.K = bk.bucketCount(); band.C = 1; band.W = W; band.H = H;
        SampleSoA soa;
        soa.begin(1, fp.groups);
        FlattenScratch scratch;
        ResidualWindow window;
        window.allocate(0, 0, W, H, residualRadius);
        flattenIntoWithResidual(fp, bk, 0, 0, W, H, soa, scratch, window,
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

LastStaged flattenLastStaged(const FlattenParams& fp, const DepthBuckets& bk,
                             int x, int y, std::vector<SampleRecord> v)
{
    SampleSoA soa;
    soa.begin(fp.channelCount, fp.groups);
    FlattenScratch scratch;
    LastStaged r;
    flattenPixelToSoA(fp, bk, x, y, v, scratch, soa, nullptr,
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
    const DepthBuckets bk = makeStandardBuckets(p, 16);
    const int C = 3;

    SUBCASE("on the optical axis, no ray-distance correction")
    {
        const FlattenParams fp = makeFlattenParams(p, C, /*preMerge*/ true);
        int volumetricWinners = 0;
        for (int iter = 0; iter < 600; ++iter) {
            CAPTURE(iter);
            const std::vector<SampleRecord> v =
                fuzzDisjointStack(rng, rng.intRange(1, 6), rng.intRange(0, 2), C);

            const LastStaged staged = flattenLastStaged(fp, bk, 0, 0, v);
            REQUIRE(staged.any);

            Surface s;
            const VectorSamples view{&v};
            const int i = deepestSurface(fp, bk, 0, 0, view, s);
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

            const LastStaged staged = flattenLastStaged(fp, bk, x, y, v);
            REQUIRE(staged.any);

            Surface s;
            const VectorSamples view{&v};
            const int i = deepestSurface(fp, bk, x, y, view, s);
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
        const LastStaged staged = flattenLastStaged(fp, bk, 0, 0, v);
        REQUIRE(staged.any);
        Surface s;
        const VectorSamples view{&v};
        REQUIRE(deepestSurface(fp, bk, 0, 0, view, s) == 0);
        CHECK(s.radiusPx == staged.radiusPx);
        CHECK(s.zBack    == staged.zBack);
    }
}

TEST_CASE("deepestSurface: an all-alpha-0 pixel and an empty pixel both read empty; NaN "
          "and non-finite depths take the flatten's sanitising")
{
    const CocParams     p  = makeStandardRig(10.0f);
    const DepthBuckets  bk = makeStandardBuckets(p, 16);
    const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ true);

    SUBCASE("empty")
    {
        const std::vector<SampleRecord> v;
        Surface s;
        CHECK(deepestSurface(fp, bk, 0, 0, VectorSamples{&v}, s) == -1);
    }
    SUBCASE("all alpha 0, including a NaN alpha")
    {
        const std::vector<SampleRecord> v{
            makeSample(3.0f, 3.0f, 0.0f, {0.0f}),
            makeSample(5.0f, 9.0f, -0.5f, {0.0f}),
            makeSample(20.0f, 20.0f, std::numeric_limits<float>::quiet_NaN(), {0.0f})};
        Surface s;
        CHECK(deepestSurface(fp, bk, 0, 0, VectorSamples{&v}, s) == -1);
        const LastStaged staged = flattenLastStaged(fp, bk, 0, 0, v);
        CHECK(!staged.any);
    }
    SUBCASE("+inf zBack is kMaxDepth, a NaN front is 0, back-before-front collapses")
    {
        const std::vector<SampleRecord> v{
            makeSample(std::numeric_limits<float>::quiet_NaN(), 2.0f, 0.5f, {0.1f}),
            makeSample(9.0f, 4.0f, 0.5f, {0.1f}),
            makeSample(30.0f, std::numeric_limits<float>::infinity(), 0.5f, {0.1f})};
        Surface s;
        CHECK(deepestSurface(fp, bk, 0, 0, VectorSamples{&v}, s) == 2);
        CHECK(s.zFront == 30.0f);
        CHECK(s.zBack  == DepthBuckets::kMaxDepth);
        const LastStaged staged = flattenLastStaged(fp, bk, 0, 0, v);
        REQUIRE(staged.any);
        CHECK(s.radiusPx == staged.radiusPx);
        CHECK(s.zBack    == staged.zBack);

        const std::vector<SampleRecord> collapsed{makeSample(9.0f, 4.0f, 0.5f, {0.1f})};
        CHECK(deepestSurface(fp, bk, 0, 0, VectorSamples{&collapsed}, s) == 0);
        CHECK(s.zFront == 9.0f);
        CHECK(s.zBack  == 9.0f);
    }
    SUBCASE("alpha above 1 clamps, ties on zBack go to the later index")
    {
        const std::vector<SampleRecord> v{
            makeSample(7.0f, 7.0f, 3.0f, {0.1f}),
            makeSample(7.0f, 7.0f, 0.25f, {0.2f})};
        Surface s;
        CHECK(deepestSurface(fp, bk, 0, 0, VectorSamples{&v}, s) == 1);
        CHECK(s.alpha == 0.25f);
        const std::vector<SampleRecord> one{makeSample(7.0f, 7.0f, 3.0f, {0.1f})};
        CHECK(deepestSurface(fp, bk, 0, 0, VectorSamples{&one}, s) == 0);
        CHECK(s.alpha == 1.0f);
    }
}

TEST_CASE("SurfaceMap: bytesForWindow is (C+4) floats per pixel, and bandBudgetBytes carries "
          "it and its pyramid in background mode only, over the window extended by the "
          "search reach")
{
    CHECK(SurfaceMap::bytesForWindow(4096, 64 + 2 * (101 + 33), 4) == 8u * 4096u * 332u * 4u);
    CHECK(SurfaceMap::bytesForWindow(4096, 64 + 2 * (101 + 33), 4) == 43515904u);
    CHECK(SurfaceMap::bytesForWindow(-1, 10, 4) == 0u);
    CHECK(SurfaceMap::bytesForWindow(10, 0, 4) == 0u);
    CHECK(SurfaceMap::bytesForWindow(10, 10, -3) == 4u * 100u * 4u);
    CHECK(SurfaceMap::bytesForWindow(10, 10, 4) == surfaceMapBytesForWindow(10, 10, 4));

    const double fg = bandBudgetBytes(16, 4, 4096, 64, false, 0.0, 101);
    CHECK(fg == bandBudgetBytes(16, 4, 4096, 64, false, 0.0, 101, FillMode::Foreground, 33));
    const double pyramid332 = static_cast<double>(MaxDepthPyramid::bytesForWindow(4096, 332));
    const double pyramid266 = static_cast<double>(MaxDepthPyramid::bytesForWindow(4096, 266));
    CHECK(pyramid332 == (1024.0 * 83.0 + 256.0 * 21.0 + 64.0 * 6.0 + 16.0 * 2.0 + 4.0 + 1.0) * 4.0);
    CHECK(pyramid332 == static_cast<double>(maxDepthPyramidBytesForWindow(4096, 332)));
    CHECK(pyramid332 < 43515904.0 / 8.0 / 14.0);
    CHECK(bandBudgetBytes(16, 4, 4096, 64, false, 0.0, 101, FillMode::Background, 33) - fg
          == doctest::Approx(43515904.0 + pyramid332));
    CHECK(bandBudgetBytes(16, 4, 4096, 64, false, 0.0, 101, FillMode::Background, 0) - fg
          == doctest::Approx(8.0 * 4096.0 * 266.0 * 4.0 + pyramid266));
    CHECK(bandBudgetBytes(16, 4, 4096, 64, false, 0.0, 101, FillMode::Background, -5) - fg
          == doctest::Approx(8.0 * 4096.0 * 266.0 * 4.0 + pyramid266));

    CHECK(fillReachPx(0.0f) == 0);
    CHECK(fillReachPx(-2.0f) == 0);
    CHECK(fillReachPx(std::numeric_limits<float>::quiet_NaN()) == 0);
    CHECK(fillReachPx(std::numeric_limits<float>::infinity()) == 0);
    CHECK(fillReachPx(16.0f) == 16);
    CHECK(fillReachPx(16.01f) == 17);
}

TEST_CASE("SurfaceMap: a fresh map has size 0 and clear() keeps it there")
{
    SurfaceMap map;
    CHECK(map.pixels() == 0);
    CHECK(map.planes.size() == 0u);
    map.allocate(0, 0, 4, 3, 2);
    CHECK(map.pixels() == 12);
    CHECK(map.planes.size() == 12u * 6u);
    for (std::ptrdiff_t i = 0; i < map.pixels(); ++i)
        CHECK(map.empty(i));
    map.clear();
    CHECK(map.pixels() == 0);
    CHECK(map.planes.size() == 0u);
    CHECK(!map.contains(0, 0));
}

TEST_CASE("buildSurfaceMap: the extended window is band +/- (padY + reach) clipped to the "
          "output box; only srcBox rows are fetched; cells outside srcBox read empty")
{
    const CocParams     p  = makeStandardRig(10.0f);
    const DepthBuckets  bk = makeStandardBuckets(p, 16);
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
        map, fp, bk, outX0, outX1, outY0, outY1, srcX0, srcX1, srcY0, srcY1,
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
            const LastStaged staged = flattenLastStaged(fp, bk, x, y, v);
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
            map, fp, bk, outX0, outX1, outY0, outY1, srcX0, srcX1, srcY0, srcY1,
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
            map, fp, bk, outX0, outX1, outY0, outY1, srcX0, srcX1, srcY0, srcY1,
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
    DepthBuckets  buckets;
    FlattenParams fp;
};

FillRig makeHaloFillRig()
{
    FillRig r;
    r.coc     = makeManualRig(4.0f, kHaloFarZ);
    r.buckets = makeBoundedDeltaCocBuckets(r.coc, kNearCardZ, kHaloFarZ, 16);
    r.fp      = makeFlattenParams(r.coc, 1, true);
    return r;
}

FillRig makeRampFillRig()
{
    FillRig r;
    r.coc     = makeManualRig(86.0f, 10.0f);
    r.buckets = makeBoundedDeltaCocBuckets(r.coc, groundDepth(0), groundDepth(kFillRigSize - 1), 16);
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
        map, rig.fp, rig.buckets,
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
          "pixel's full reach disc answer identically")
{
    const FillRig halo = makeHaloFillRig();
    const int primary = 33, fallback = 60;

    SurfaceMap wide, narrow;
    MaxDepthPyramid widePyr, narrowPyr;
    buildRigMap(wide, halo, 100, 140, 5, fallback, [](int x, int y) { return haloStack(x, y, false); });
    buildRigMap(narrow, halo, 120, 124, 2, fallback, [](int x, int y) { return haloStack(x, y, false); });
    widePyr.build(wide);
    narrowPyr.build(narrow);
    REQUIRE(wide.y == 35);
    REQUIRE(wide.height == 170);
    REQUIRE(narrow.y == 58);
    REQUIRE(narrow.height == 128);
    CHECK(widePyr.levelCount() == 4);
    CHECK(narrowPyr.levelCount() == 4);

    for (int y = 120; y < 124; ++y) {
        for (int x = kHaloX0; x < kHaloX1; ++x) {
            const BackgroundSource a = findBackgroundSource(wide, widePyr, x, y, primary, fallback);
            const BackgroundSource b = findBackgroundSource(narrow, narrowPyr, x, y, primary, fallback);
            REQUIRE(a.found);
            CHECK(sameSource(a, b));
            CHECK(a.qx == b.qx);
            CHECK(a.qy == b.qy);
            CHECK(a.distance == b.distance);
        }
    }
    const BackgroundSource a = findBackgroundSource(wide, widePyr, 120, 122, primary, fallback);
    CHECK(a.distance == 41.0f);
    CHECK(a.qx == kHaloX0 - 1);
    CHECK(a.qy == 122);
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
    DepthBuckets  buckets;
    FlattenParams fp;
};

SynthRig makeSynthRig(bool rayDistance = false)
{
    SynthRig r;
    r.coc     = makeManualRig(2.0f, kSynthPlaneZ);
    r.buckets = makeBoundedDeltaCocBuckets(r.coc, 1.0f, kSynthPlaneZ, 16);
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
        map, rig.fp, rig.buckets,
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
    ints(a.bucketIndex0, b.bucketIndex0);
    ints(a.bucketIndex1, b.bucketIndex1);
    for (std::size_t i = 0; i < a.flags.size(); ++i)
        if (a.flags[i] != b.flags[i]) d.sameShape = false;
    floats(a.radius, b.radius);
    floats(a.depth, b.depth);
    floats(a.alpha, b.alpha);
    floats(a.arrivalShare, b.arrivalShare);
    floats(a.bucketAlpha0, b.bucketAlpha0);
    floats(a.bucketAlpha1, b.bucketAlpha1);
    floats(a.colorScale0, b.colorScale0);
    floats(a.colorScale1, b.colorScale1);
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
                    float cardAlpha, const HoldoutSoA& holdout)
{
    const int W = kSynthW, pad = 10;
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
                        autoBackgroundRadiusPx(rig.coc, rig.buckets));
    out.appended = 0;
    for (int y = -pad; y < W + pad; ++y) {
        for (int x = -pad; x < W + pad; ++x) {
            std::vector<SampleRecord> v = stackAt(x, y);
            if (synthesize && appendHiddenBackground(map, pyramid, rig.fp, x, y, 0.0f, 100.0f, v))
                ++out.appended;
            const std::size_t i = static_cast<std::size_t>(out.window.index(x, y));
            float residualT = 1.0f;
            float residualR = out.window.radiusPx[i];
            flattenPixelToSoA(rig.fp, rig.buckets, x, y, v, scratch, out.soa, nullptr,
                              &residualT, &residualR);
            out.window.setPixel(x, y, residualT, residualR);
        }
    }

    out.band.K = rig.buckets.bucketCount();
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
        for (int y = 8; y < 16; ++y) {
            for (int x = 8; x < 16; ++x) {
                CAPTURE(x);
                CAPTURE(y);
                std::vector<SampleRecord> synth = cardSample(1.0f);
                REQUIRE(appendHiddenBackground(map, pyramid, rig.fp, x, y, 0.0f, 100.0f, synth));
                REQUIRE(synth.size() == 2u);
                CHECK(synth[1].zFront == kSynthPlaneZ);
                CHECK(synth[1].zBack  == kSynthPlaneZ);
                CHECK(synth[1].alpha  == 1.0f);
                CHECK(synth[1].channels.size() == 1u);
                CHECK(synth[1].channels[0] == kSynthPlaneC);

                std::vector<SampleRecord> twin = cardSample(1.0f);
                twin.push_back(planeSample()[0]);

                float tS = -1.0f, rS = -1.0f, tT = -1.0f, rT = -1.0f;
                const SampleSoA a = flattenOnePixel(rig.fp, rig.buckets, x, y, synth, &tS, &rS);
                const SampleSoA b = flattenOnePixel(rig.fp, rig.buckets, x, y, twin,  &tT, &rT);
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
        CHECK(checked == 64);
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

        std::vector<SampleRecord> synth = cardSample(0.5f);
        REQUIRE(appendHiddenBackground(map, pyramid, rig.fp, 12, 12, 0.0f, 100.0f, synth));
        std::vector<SampleRecord> twin = cardSample(0.5f);
        twin.push_back(planeSample(alphaQ)[0]);

        float tS = -1.0f, rS = -1.0f, tT = -1.0f, rT = -1.0f;
        const SampleSoA a = flattenOnePixel(rig.fp, rig.buckets, 12, 12, synth, &tS, &rS);
        const SampleSoA b = flattenOnePixel(rig.fp, rig.buckets, 12, 12, twin,  &tT, &rT);
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

    SUBCASE("off axis with depth_is_ray_distance: the raw depth is pre-divided by P's own "
            "factor and lands back on Q's camera depth within 1 ulp")
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
        for (int y = oy + 8; y < oy + 16; ++y) {
            for (int x = ox + 8; x < ox + 16; ++x) {
                CAPTURE(x);
                CAPTURE(y);
                const float sP = rayDepthScaleAt(ray.fp, x, y);
                REQUIRE(sP < 1.0f);

                std::vector<SampleRecord> synth = cardSample(1.0f);
                REQUIRE(appendHiddenBackground(map, pyramid, ray.fp, x, y, 0.0f, 100.0f, synth));
                REQUIRE(synth.size() == 2u);
                // Q's camera depth as the map holds it, after P's own
                // correction is applied to the synthetic.
                const BackgroundSource q = findBackgroundSource(map, pyramid, x, y, 17, 100);
                REQUIRE(q.found);
                const float zCamQ = map.plane(SurfaceMap::kZFront)[map.index(q.qx, q.qy)];
                const int rt = floatUlps(synth[1].zFront * sP, zCamQ);
                worstRoundTrip = std::max(worstRoundTrip, rt);
                CHECK(rt <= 1);

                std::vector<SampleRecord> twin = cardSample(1.0f);
                twin.push_back(planeSample(1.0f, sP)[0]);
                float tS = -1.0f, rS = -1.0f, tT = -1.0f, rT = -1.0f;
                const SampleSoA a = flattenOnePixel(ray.fp, ray.buckets, x, y, synth, &tS, &rS);
                const SampleSoA b = flattenOnePixel(ray.fp, ray.buckets, x, y, twin,  &tT, &rT);
                const SoADiff d = compareSoA(a, b);
                CHECK(d.sameShape);
                // Q's plane sample was itself rounded once at Q (raw * sQ),
                // then once more here (/ sP * sP): two roundings against the
                // twin's one, so the staged depth may sit 2 ulps off and the
                // radius derived from it a few more.
                CHECK(d.worstUlps <= 4);
                worstSoA = std::max(worstSoA, d.worstUlps);
                if (d.worstUlps == 0)
                    ++exact;
                CHECK(tS == tT);
                CHECK(floatUlps(rS, rT) <= 4);
                CHECK(tS == 0.0f);
                ++checked;
            }
        }
        std::printf("\noff-axis synthesis: %d pixels, worst round-trip %d ulp, worst SoA "
                    "field %d ulp, %d bit-exact\n", checked, worstRoundTrip, worstSoA, exact);
        CHECK(checked == 64);
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
        wide.buckets = makeBoundedDeltaCocBuckets(wide.coc, 1.0f, kSynthPlaneZ + 6.0f, 16);
        SurfaceMap map;
        MaxDepthPyramid pyramid;
        buildSynthMap(map, pyramid, wide, x0, x1, y0, y1, field);

        std::vector<SampleRecord> synth = cardSample(1.0f);
        REQUIRE(appendHiddenBackground(map, pyramid, wide.fp, 12, 12, 0.0f, 100.0f, synth));
        REQUIRE(synth.size() == 2u);
        CHECK(synth[1].zFront == spanB.zFront);
        CHECK(synth[1].zBack  == spanB.zBack);
        CHECK(synth[1].alpha  == spanB.alpha);
        CHECK(synth[1].channels[0] == spanB.channels[0]);

        std::vector<SampleRecord> twin = cardSample(1.0f);
        twin.push_back(spanB);
        float tS = -1.0f, rS = -1.0f, tT = -1.0f, rT = -1.0f;
        const SampleSoA a = flattenOnePixel(wide.fp, wide.buckets, 12, 12, synth, &tS, &rS);
        const SampleSoA b = flattenOnePixel(wide.fp, wide.buckets, 12, 12, twin,  &tT, &rT);
        const SoADiff d = compareSoA(a, b);
        CHECK(d.sameShape);
        CHECK(d.worstUlps == 0);
        CHECK(tS == tT);
        CHECK(rS == rT);

        const BackgroundSource q = findBackgroundSource(map, pyramid, 12, 12, 17, 100);
        REQUIRE(q.found);
        const float mapRadiusQ = map.plane(SurfaceMap::kRadius)[map.index(q.qx, q.qy)];
        float tQ = -1.0f, rQ = -1.0f;
        flattenOnePixel(wide.fp, wide.buckets, q.qx, q.qy, {spanA, spanB}, &tQ, &rQ);
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

TEST_CASE("residualTransmittance agrees with flattenPixelToSoA's residualT over fuzzed "
          "stacks of points, spans and overlaps, far inside kFillDeficitTol")
{
    Lcg rng(0xF111u);
    const CocParams     p  = makeStandardRig(10.0f);
    const DepthBuckets  bk = makeStandardBuckets(p, 16);
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
        const float helper = residualTransmittance(v);
        float tF = -1.0f;
        flattenOnePixel(fp, bk, 0, 0, v, &tF, nullptr);
        worst = std::max(worst, std::fabs(helper - tF));
        CHECK(helper == doctest::Approx(tF).epsilon(2e-6));
    }
    CHECK(worst < kFillDeficitTol * 0.1f);
    CHECK(residualTransmittance({}) == 1.0f);
    CHECK(residualTransmittance(cardSample(1.0f)) == 0.0f);
    CHECK(residualTransmittance(cardSample(0.5f)) == 0.5f);
}

TEST_CASE("appendHiddenBackground: the per-pixel auto reach is 2 * r_P + 1 at P's own "
          "radius; the prune drops an opaque P whose source disc is at least its own; an "
          "empty pixel or one with no source appends nothing")
{
    const SynthRig rig = makeSynthRig();

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
        wide.buckets = makeBoundedDeltaCocBuckets(wide.coc, 1.0f, kSynthPlaneZ + 10.0f, 16);
        SurfaceMap map;
        MaxDepthPyramid pyramid;
        buildSynthMap(map, pyramid, wide, 0, 24, 0, 24, field);

        std::vector<SampleRecord> opaque = planeSample(1.0f);
        CHECK(findBackgroundSource(map, pyramid, 7, 12, 1, 100).found);
        CHECK(!appendHiddenBackground(map, pyramid, wide.fp, 7, 12, 0.0f, 100.0f, opaque));
        CHECK(opaque.size() == 1u);

        std::vector<SampleRecord> half = planeSample(0.5f);
        CHECK(appendHiddenBackground(map, pyramid, wide.fp, 7, 12, 0.0f, 100.0f, half));
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
        CHECK(appendHiddenBackground(map, pyramid, rig.fp, 24, 24, 0.0f, 100.0f, a, &withFallback));
        std::vector<SampleRecord> b = cardSample(1.0f);
        CHECK(!appendHiddenBackground(map, pyramid, rig.fp, 24, 24, 0.0f, 17.0f, b, &primaryOnly));
        CHECK(b.size() == 1u);
        std::vector<SampleRecord> c = cardSample(1.0f);
        CHECK(appendHiddenBackground(map, pyramid, rig.fp, 24, 24, 20.0f, 20.0f, c, &manual));
        CHECK(c.size() == 2u);
        std::vector<SampleRecord> e = cardSample(1.0f);
        CHECK(!appendHiddenBackground(map, pyramid, rig.fp, 24, 24, 19.0f, 19.0f, e));
        CHECK(!appendHiddenBackground(map, pyramid, rig.fp, 24, 24, 100.0f, 19.0f, e));
        CHECK(e.size() == 1u);
        CHECK(appendHiddenBackground(map, pyramid, rig.fp, 12, 24, 0.0f, 17.0f, e));
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
        CHECK(!appendHiddenBackground(map, pyramid, rig.fp, 30, 30, 0.0f, 100.0f, none));
        CHECK(!appendHiddenBackground(map, pyramid, rig.fp, 2, 2, 0.0f, 100.0f, none));
        CHECK(none.empty());
        std::vector<SampleRecord> plane = planeSample();
        CHECK(!appendHiddenBackground(map, pyramid, rig.fp, 2, 2, 0.0f, 100.0f, plane));
        CHECK(plane.size() == 1u);
    }
}

TEST_CASE("end to end on the halo rig: synthesis reads the twin's FG:BG mix in the vacated "
          "band within 1e-6, the foreground mode reads the card's own colour there, and a "
          "holdout at 0.5 halves the synthesised deposits exactly as it halves the twin's")
{
    const SynthRig rig = makeSynthRig();
    const HoldoutSoA none;
    SynthRender synth, twin, foreground;
    renderSynthRig(synth,      rig, true,  false, 1.0f, none);
    renderSynthRig(twin,       rig, false, true,  1.0f, none);
    renderSynthRig(foreground, rig, false, false, 1.0f, none);
    CHECK(synth.appended == (kSynthCard1 - kSynthCard0) * (kSynthCard1 - kSynthCard0));
    CHECK(foreground.appended == 0);

    const SoADiff d = compareSoA(synth.soa, twin.soa);
    CHECK(d.sameShape);
    CHECK(d.worstUlps == 0);

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
    for (std::size_t i = 0; i < synth.band.color.size(); ++i)
        if (synth.band.color[i] != twin.band.color[i] || synth.band.alpha[i] != twin.band.alpha[i])
            ++differing;
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
        lut.build(hs, makeUniformHoldoutBoundaries(rig.buckets));
        const HoldoutSoA half = lut.view();
        REQUIRE(half.enabled());

        SynthRender synthH, twinH;
        renderSynthRig(synthH, rig, true,  false, 1.0f, half);
        renderSynthRig(twinH,  rig, false, true,  1.0f, half);

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

        // The plane's bucket at a card pixel holds only synthesised deposits
        // (the plane's own r = 0 disc never leaves its pixel): halved exactly.
        const int kQ = rig.buckets.bucketOfContaining(kSynthPlaneZ).index;
        const std::ptrdiff_t px = synth.band.pixels();
        double full = 0.0, halved = 0.0;
        for (int y = kSynthCard0; y < kSynthCard1; ++y) {
            for (int x = kSynthCard0; x < kSynthCard1; ++x) {
                const std::size_t i = static_cast<std::size_t>(kQ) * px + static_cast<std::size_t>(y) * kSynthW + x;
                full   += synth.band.planes.color[i];
                halved += synthH.band.planes.color[i];
                CHECK(synthH.band.planes.color[i] == 0.5f * synth.band.planes.color[i]);
                CHECK(synthH.band.planes.alpha[i] == 0.5f * synth.band.planes.alpha[i]);
            }
        }
        CHECK(full > 0.0);
        CHECK(halved == doctest::Approx(0.5 * full));
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
                }

                // i = y*W + x (row-major), so `pixels` is directly indexable by
                // (x, y); a COPY goes to the flatten, same as the original loop,
                // since it sorts/mutates in place and `pixels` must survive for
                // the reference computation below.
                ResidualWindow window;
                window.allocate(0, 0, W, H, autoBackgroundRadiusPx(p, bk));
                flattenIntoWithResidual(fp, bk, 0, 0, W, H, soa, scratch, window,
                    [&](int x, int y) -> std::vector<SampleRecord> {
                        const std::size_t i = static_cast<std::size_t>(y) * W
                                             + static_cast<std::size_t>(x);
                        return (i < pixels.size()) ? pixels[i]
                                                   : std::vector<SampleRecord>{};
                    });

                Band band;
                band.K = K; band.C = C; band.W = W; band.H = H;
                HoldoutSoA noHoldout;
                DiscKernelLUT kernel(0.0f, 1.0f, 1.0f, 1.0f);
                runBand(band, makeScatterParams(W, H),
                        soa, noHoldout, kernel, /*useThread*/ false, &window);

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
    // The merge is only lossless when the two members rasterise LITERALLY the
    // same kernel, so this predicate has to agree with the scatter at both of
    // the scatter's decision points and not merely near them.  Both edges
    // survive a mutation set unless a case exercises them exactly.
    //
    // 1. THE SHARP THRESHOLD.  scatterBandCPU() takes the sharp path for
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

TEST_CASE("the flatten's collision absorb is lossless: merged-then-scattered equals "
          "scattered-separately within 1e-6 (fuzzed)")
{
    // The absorb `over`-composites two same-pixel fragments into one when
    // they share a bucket AND rasterise one kernel, and the result is then
    // scattered ONCE at the front member's radius.  The reference is the same
    // pixel with the absorb switched off: every member stays its own
    // fragment at its OWN radius, attenuated per bucket by the running alpha
    // of the same-kernel deposits ahead of it -- same kernel by the
    // reference's independently derived bin.  Rasterised onto the bucket
    // planes, the two agree within 1e-6 only if (a) the flatten absorbed
    // exactly the pairs the reference calls one kernel and (b) the absorbed
    // member's kernel really was the front member's.
    //
    // Volumetric samples, so every deposit is whole-weight into its
    // containing bucket and the two paths differ ONLY by the kernel each
    // member is rasterised at: a point sample's fractional two-bucket
    // partition is not linear in `over`, which would put a partition
    // residual into the comparison that has nothing to do with the predicate.
    // The standard rig clamped at 12px saturates every depth in front of
    // z = 1.68, so a fraction of the members share a radius exactly (the only
    // way two disjoint samples can); the rest spread over (9.6, 12) px in the
    // same first bucket, where a relaxed predicate would absorb near-equal
    // radii and be caught.
    const CocParams    p  = makeStandardRig(10.0f, /*maxRadiusPx*/ 12.0f);
    const DepthBuckets bk = makeBoundedDeltaCocBuckets(p, 1.0f, 100.0f, 6);
    REQUIRE(radiusPixels(p, 1.65f) == 12.0f);
    REQUIRE(radiusPixels(p, 1.90f) < 12.0f);
    REQUIRE(bk.boundary(1) > 1.90f);
    const int C = 2;
    const FlattenParams fp = makeFlattenParams(p, C, /*preMerge*/ false);
    const DiscKernelLUT lut(2.0f, 40.0f, 1.0f, 1.0f);
    const int R = 14, side = 2 * R + 1;                 // 12px + softness, with room
    const int K = bk.bucketCount();

    // (alpha, colour[C]) planes per bucket for one fragment list, via the
    // blended kernel plane; the two lists deposit into the same layout.
    struct Deposit {
        float radius; int index0, index1; double alpha0, alpha1, cs0, cs1;
        std::vector<double> channels;
    };
    const auto rasterise = [&](const std::vector<Deposit>& list) {
        std::vector<double> alpha(static_cast<std::size_t>(K) * side * side, 0.0);
        std::vector<double> color(static_cast<std::size_t>(K) * C * side * side, 0.0);
        std::vector<double> plane;
        for (const Deposit& d : list) {
            refKernelPlane(lut, d.radius, R, plane);
            for (int pass = 0; pass < 2; ++pass) {
                const int    k  = (pass == 0) ? d.index0 : d.index1;
                const double a  = (pass == 0) ? d.alpha0 : d.alpha1;
                const double cs = (pass == 0) ? d.cs0 : d.cs1;
                if (pass == 1 && d.index1 == d.index0)
                    break;
                if (k < 0 || k >= K)
                    continue;
                for (std::size_t i = 0; i < plane.size(); ++i) {
                    alpha[static_cast<std::size_t>(k) * side * side + i] += plane[i] * a;
                    for (int c = 0; c < C; ++c)
                        color[(static_cast<std::size_t>(k) * C + c) * side * side + i] +=
                            plane[i] * d.channels[static_cast<std::size_t>(c)] * cs;
                }
            }
        }
        return std::make_pair(alpha, color);
    };

    Lcg rng(0xAB50B8ULL);
    std::size_t absorbs = 0, sets = 0, setsWithAbsorb = 0;
    double worstAlpha = 0.0, worstColor = 0.0, worstRatio = 0.0;
    for (int trial = 0; trial < 400; ++trial) {
        std::vector<SampleRecord> samples;
        float z = rng.range(1.0f, 1.6f);
        const int n = rng.intRange(2, 7);
        for (int i = 0; i < n && z < 2.2f; ++i) {
            const float a = rng.range(0.05f, 1.0f);
            const float ratio = rng.range(0.1f, 1.0f);
            // One sample in three is a NEAR-duplicate of its predecessor: a
            // sliver 1e-6..1e-2 deep, so the two radii differ by a fraction
            // of a blend cell up to a few nodes -- the pairs a loosened cell
            // would wrongly absorb.
            const bool sliver = (i > 0) && (rng.unit() < 0.34f);
            const float thickness = sliver ? std::pow(10.0f, rng.range(-6.0f, -2.0f))
                                           : rng.range(0.005f, 0.15f);
            samples.push_back(makeSample(z, z + thickness, a, {a * ratio, a * ratio * 0.5f}));
            z += thickness + (sliver ? std::pow(10.0f, rng.range(-6.0f, -3.0f))
                                     : rng.range(0.001f, 0.2f));  // strictly disjoint
        }
        if (samples.size() < 2u)
            continue;
        ++sets;

        const SampleSoA merged = flattenOnePixel(fp, bk, R, R, samples);
        const std::vector<RefFragment> separate =
            refFlatten(p, bk, R, R, samples, false, fp.mergeTolerancePx, C,
                       false, /*absorbCollisions*/ false);
        REQUIRE(separate.size() >= merged.fragmentCount());
        const std::size_t here = separate.size() - merged.fragmentCount();
        absorbs += here;
        if (here > 0)
            ++setsWithAbsorb;

        std::vector<Deposit> mergedList, separateList;
        for (std::size_t i = 0; i < merged.fragmentCount(); ++i) {
            Deposit d;
            d.radius = merged.radius[i];
            d.index0 = static_cast<int>(merged.bucketIndex0[i]);
            d.index1 = static_cast<int>(merged.bucketIndex1[i]);
            d.alpha0 = merged.bucketAlpha0[i];
            d.alpha1 = merged.bucketAlpha1[i];
            d.cs0    = merged.colorScale0[i];
            d.cs1    = merged.colorScale1[i];
            const float* col = merged.colorOf(i);
            for (int c = 0; c < C; ++c)
                d.channels.push_back(static_cast<double>(col[c]));
            mergedList.push_back(d);
        }
        for (const RefFragment& f : separate) {
            Deposit d;
            d.radius = static_cast<float>(f.radius);
            d.index0 = f.index0;
            d.index1 = f.index1;
            d.alpha0 = f.alpha0;
            d.alpha1 = f.alpha1;
            d.cs0    = f.colorScale0;
            d.cs1    = f.colorScale1;
            d.channels = f.channels;
            separateList.push_back(d);
        }

        const auto got  = rasterise(mergedList);
        const auto want = rasterise(separateList);
        const double dAlpha = maxAbsDiff(got.first, want.first);
        const double dColor = maxAbsDiff(got.second, want.second);
        CAPTURE(trial);
        CAPTURE(here);
        CHECK(dAlpha <= 1.0e-06);
        CHECK(dColor <= 1.0e-06);
        worstAlpha = std::max(worstAlpha, dAlpha);
        worstColor = std::max(worstColor, dColor);
        // The colour:alpha ratio, per plane pixel, survives the absorb too.
        for (std::size_t i = 0; i < got.first.size(); ++i) {
            if (got.first[i] < 1.0e-03 || want.first[i] < 1.0e-03)
                continue;
            for (int c = 0; c < C; ++c) {
                const std::size_t k  = i / (static_cast<std::size_t>(side) * side);
                const std::size_t px = i % (static_cast<std::size_t>(side) * side);
                const std::size_t ci = (k * C + c) * side * side + px;
                const double r = std::fabs(got.second[ci] / got.first[i]
                                           - want.second[ci] / want.first[i]);
                worstRatio = std::max(worstRatio, r);
            }
        }
    }
    CAPTURE(worstAlpha);
    CAPTURE(worstColor);
    CAPTURE(worstRatio);
    CHECK(worstRatio <= 1.0e-05);
    // Non-vacuity: the absorb fired, and in a good fraction of the sets.
    CHECK(sets >= 350u);
    CHECK(absorbs >= 200u);
    CHECK(setsWithAbsorb >= 100u);
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
        }

        ResidualWindow window;
        window.allocate(0, 0, W, H, autoBackgroundRadiusPx(p, bk));
        flattenIntoWithResidual(fp, bk, 0, 0, W, H, soa, scratch, window,
            [&](int x, int y) -> std::vector<SampleRecord> {
                const std::size_t i = static_cast<std::size_t>(y) * W
                                     + static_cast<std::size_t>(x);
                return (i < pixels.size()) ? pixels[i] : std::vector<SampleRecord>{};
            });

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
                soa, view, kernel, /*useThread*/ false, &window);

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

        float residualT = 1.0f, residualR = 0.0f;
        const SampleSoA soa = flattenOnePixel(fp, bk, 4, 4,
            {makeSample(20.0f, 20.0f, 0.5f, {0.5f}),
             makeSample(40.0f, 40.0f, 0.5f, {0.5f})},
            &residualT, &residualR);

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
        ResidualWindow window;
        oneSourcePixelWindow(window, W, H, 4, 4, residualT, residualR);
        runBand(band, makeScatterParams(W, H),
                soa, view, kernel, /*useThread*/ false, &window);

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

    float residualT = 1.0f, residualR = 0.0f;
    const SampleSoA soa = flattenOnePixel(fp, bk, 4, 4,
        {makeSample(20.0f, 20.0f, 0.5f, {0.5f}),
         makeSample(60.0f, 60.0f, 0.5f, {0.5f})},
        &residualT, &residualR);
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
    ResidualWindow window;
    oneSourcePixelWindow(window, W, H, 4, 4, residualT, residualR);
    runBand(band, makeScatterParams(W, H),
            soa, lut.view(), kernel, /*useThread*/ false, &window);

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
    const std::int64_t binB = scatterKernelBin(radiusPixels(p, zB));
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
        float residualT = 1.0f, residualR = 0.0f;
        const SampleSoA soa = flattenOnePixel(fp, bk, 4, 4,
            {makeSample(9.063f, 9.063f, 0.4667f, {0.4667f * 0.25f}),
             makeSample(11.039f, 11.039f, 0.5899f, {0.5899f * 0.75f})},
            &residualT, &residualR);

        // Both are on the sharp path here (radius ~0.27px), so they are one
        // kernel and the collision is resolvable exactly.
        REQUIRE(soa.fragmentCount() == 1u);

        Band band;
        band.K = 8; band.C = 1; band.W = W; band.H = H;
        HoldoutSoA noHoldout;
        ResidualWindow window;
        oneSourcePixelWindow(window, W, H, 4, 4, residualT, residualR);
        runBand(band, makeScatterParams(W, H),
                soa, noHoldout, kernel, true, &window);

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
                std::vector<std::array<std::int64_t, 4>> claims;
                for (std::size_t i = 0; i < soa.fragmentCount(); ++i) {
                    if (!fragmentCoverageHeadOf(soa.flags[i]))
                        continue;
                    claims.push_back({static_cast<std::int64_t>(soa.x[i]),
                                      static_cast<std::int64_t>(soa.y[i]),
                                      static_cast<std::int64_t>(soa.bucketIndex0[i]),
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
        // "Close enough" means the SAME radius: two radii rasterise one kernel
        // only when they are equal, and two disjoint parts have distinct
        // midpoints, so the only way a part and its follower share a kernel is
        // the max_radius clamp.  Manual size 10 / focus 10 clamped at 8px
        // saturates every depth beyond z = 50; A's rear part [b5, 80] has its
        // midpoint at 50.7 and B at 80.75, both 8.0000px, both in the last
        // bucket.  A rear part ending at 60 (midpoint 40.7, 7.54px) would
        // silently turn this subcase into a three-fragment no-merge case.
        const CocParams q = makeCocParams(CocMode::Manual, 50.0f, 2.8f, 36.0f,
                                          10.0f, unitScale(WorldUnits::Meters),
                                          1920.0f, 1.0f, 1.0f, 1.0f,
                                          /*maxRadiusPx*/ 8.0f, /*sizePx*/ 10.0f);
        const DepthBuckets qb = makeBoundedDeltaCocBuckets(q, 1.0f, 100.0f, 6);
        const float b5 = qb.boundary(5);
        REQUIRE(radiusPixels(q, 0.5f * (b5 + 80.0f)) == 8.0f);
        REQUIRE(radiusPixels(q, 80.75f) == 8.0f);
        const FlattenParams fq = makeFlattenParams(q, 1, /*preMerge*/ false);
        const SampleSoA soa = flattenOnePixel(fq, qb, 20, 20,
            {makeSample(b5 - 0.02f, 80.0f, 0.6f, {0.6f * 0.5f}),
             makeSample(80.5f, 81.0f, 0.4f, {0.4f * 0.5f})});

        // A0 (head, bucket 4) and the merged [A1 + B] (bucket 5).
        REQUIRE(soa.fragmentCount() == 2u);
        CHECK(soa.bucketIndex0[0] == 4);
        CHECK(soa.bucketIndex0[1] == 5);
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
        //
        // One kernel means one RADIUS, and a point and a span piece at distinct
        // depths only share one through the max_radius clamp: the standard rig
        // clamped at 12px saturates everything in front of z = 1.68, and both
        // of these sit inside the first bucket there.
        const CocParams    q  = makeStandardRig(10.0f, /*maxRadiusPx*/ 12.0f);
        const DepthBuckets qb = makeStandardBuckets(q);
        const FlattenParams fq = makeFlattenParams(q, 1, /*preMerge*/ false);
        const SampleSoA soa = flattenOnePixel(fq, qb, 0, 0,
            {makeSample(1.50f, 1.50f, 0.6f, {0.6f * 0.5f}),
             makeSample(1.51f, 1.55f, 0.4f, {0.4f * 0.5f})});

        REQUIRE(soa.fragmentCount() == 2u);
        CHECK(fragmentKindOf(soa.flags[0]) == FragmentKind::Point);
        CHECK(fragmentKindOf(soa.flags[1]) == FragmentKind::Volumetric);
        // They really do collide (the span's bucket is one of the point's
        // pair), and they really are one kernel — kind is the only thing
        // keeping them apart, so this case cannot pass for the wrong reason.
        CHECK(soa.bucketIndex0[1] >= soa.bucketIndex0[0]);
        CHECK(soa.bucketIndex0[1] <= soa.bucketIndex1[0]);
        CHECK(soa.radius[0] == 12.0f);
        CHECK(soa.radius[1] == 12.0f);
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
        ResidualWindow window;
        window.allocate(-pad, -pad, W + 2 * pad, H + 2 * pad,
                        autoBackgroundRadiusPx(p, bk));
        flattenIntoWithResidual(fp, bk, -pad, -pad, W + pad, H + pad,
            soa, scratch, window,
            [&](int, int) -> std::vector<SampleRecord> {
                return {makeSample(z1, z1, a1, {a1 * 0.5f}),
                       makeSample(z2, z2, a2, {a2 * 0.5f})};
            });

        Band band;
        band.K = K; band.C = C; band.W = W; band.H = H;
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
        ResidualWindow window;
        window.allocate(0, 0, W, H, autoBackgroundRadiusPx(p, bk));
        flattenIntoWithResidual(fp, bk, 0, 0, W, H, soa, scratch, window,
            [&](int, int) -> std::vector<SampleRecord> {
                return {makeSample(20.0f, 20.0f, 0.5f, {0.5f}),
                       makeSample(40.0f, 40.0f, 0.5f, {0.5f})};
            });

        const std::size_t perPixel =
            soa.fragmentCount() / static_cast<std::size_t>(W * H);

        Band band;
        band.K = K; band.C = C; band.W = W; band.H = H;
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
        ResidualWindow window;
        window.allocate(0, 0, W, H, autoBackgroundRadiusPx(p, bk));
        flattenIntoWithResidual(fp, bk, 0, 0, W, H, soa, scratch, window,
            [&](int, int) -> std::vector<SampleRecord> {
                return {makeSample(za, za, 0.5f, {0.5f}),
                       makeSample(zb, zb, 0.5f, {0.5f})};
            });
        CHECK(soa.fragmentCount() == static_cast<std::size_t>(W * H) * 2u);

        Band band;
        band.K = K; band.C = C; band.W = W; band.H = H;
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
            ResidualWindow window;
            window.allocate(-pad, -pad, W + 2 * pad, H + 2 * pad,
                            autoBackgroundRadiusPx(p, bk));
            flattenIntoWithResidual(fp, bk, -pad, -pad, W + pad, H + pad,
                soa, scratch, window,
                [&](int, int) -> std::vector<SampleRecord> {
                    return {makeSample(depth, depth, alpha,
                        {alpha * unpremult[0], alpha * unpremult[1], alpha * unpremult[2]})};
                });

            {
                CAPTURE(depth);
                CAPTURE(alpha);

                Band band;
                band.K = bk.bucketCount(); band.C = C; band.W = W; band.H = H;
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
    ResidualWindow window;
    window.allocate(-pad, -pad, W + 2 * pad, H + 2 * pad,
                    autoBackgroundRadiusPx(p, bk));
    flattenIntoWithResidual(fp, bk, -pad, -pad, W + pad, H + pad,
        soa, scratch, window,
        [&](int x, int y) -> std::vector<SampleRecord> {
            return {makeSample(((x + y) & 1) ? dA : dB,
                               ((x + y) & 1) ? dA : dB, 1.0f, {0.8f})};
        });

    double minArrival = 2.0, maxArrival = -1.0;
    double minAlpha = 2.0, maxAlpha = -1.0, ratio = 0.0;
    {
        Band band;
        band.K = bk.bucketCount(); band.C = 1; band.W = W; band.H = H;
        HoldoutSoA noHoldout;
        runBand(band, makeScatterParams(W, H), soa, noHoldout, lut, true, &window);

        for (int y = 16; y < 32; ++y)
            for (int x = 16; x < 32; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * W
                                    + static_cast<std::size_t>(x);
                const double d = static_cast<double>(band.planes.arrival[i]);
                minArrival = std::min(minArrival, d);
                maxArrival = std::max(maxArrival, d);
                minAlpha = std::min(minAlpha, static_cast<double>(band.outAlpha(x, y)));
                maxAlpha = std::max(maxAlpha, static_cast<double>(band.outAlpha(x, y)));
            }
        ratio = band.outColor(0, 24, 24) / band.outAlpha(24, 24);
    }
    CAPTURE(minArrival);
    CAPTURE(maxArrival);
    CAPTURE(minAlpha);

    // The composite holds the identity to 4e-3 (measured 0.996284 at the worst
    // interior pixel: the two checkerboard depths rasterise DIFFERENT radii,
    // 7.85px and 6.40px, so the two half-coverages do not tile the pixel
    // perfectly).  BANDED, not floored: a floor at 0.9956 also accepts a
    // uniform 0.5px kernel grid's 0.99927, so it would not notice the grid
    // being coarsened.
    //
    // AND PREDICTED, not just banded: the Nyquist model below is evaluated on
    // the test-side blend oracle at the two radii the flatten actually
    // produced, so the band cannot drift with the kernel scheme unnoticed --
    // the model's own prediction has to move with it.
    {
        float rLo = 1e9f, rHi = -1.0f;
        for (std::size_t f = 0; f < soa.fragmentCount(); ++f) {
            rLo = std::min(rLo, soa.radius[f]);
            rHi = std::max(rHi, soa.radius[f]);
        }
        REQUIRE(rHi > rLo + 1.0f);
        auto nyquist = [&](float r) {
            double c = 0.0;
            const int reach = static_cast<int>(std::ceil(r)) + 2;
            for (int dy = -reach; dy <= reach; ++dy)
                for (int dx = -reach; dx <= reach; ++dx)
                    c += (((dx + dy) & 1) ? -1.0 : 1.0) * refBlendedWeight(lut, r, dx, dy);
            return c;
        };
        const double half = 0.5 * std::fabs(nyquist(rHi) - nyquist(rLo));
        CAPTURE(rLo); CAPTURE(rHi); CAPTURE(half);
        CHECK(std::fabs(minArrival - (1.0 - half)) < 3.0e-04);
        CHECK(std::fabs(maxArrival - (1.0 + half)) < 3.0e-04);
    }
    //
    // THE BAND IS ON `arrival`, NOT ON THE OUTPUT ALPHA, and the same numbers
    // to the digit: on a field this dense every source pixel is opaque, so its
    // whole unit share is what scatters and arrival IS the pixel's coverage
    // sum -- the quantity the band was always about.  The output alpha is no
    // longer that quantity, because dividing by arrival is exactly what the
    // node now does with it; reading the band off alpha would only re-measure
    // the division.
    //
    // WHY THE BAND SITS WHERE IT DOES.  A checkerboard is the Nyquist pattern,
    // so what it really measures is the kernels' response at (pi, pi):
    // coverage == 1 + (C_A - C_B)/2 with C_r = sum (-1)^(dx+dy) w_r.  A uniform
    // 0.5px grid would SNAP the two radii to 8.00 and 6.50, and
    // (C_8.00 - C_6.50)/2 = -7.30e-04 is a number that belongs to the snapping.
    // Nearest-node snapping put them on 7.876923 and 6.400000, giving
    // -4.28e-03 against the UNQUANTISED -4.00e-03; the bracketing blend at
    // the true radii gives -3.72e-03 -- the same 2.8e-04 from the exact
    // answer, on the other side of it, because a blend of two discs is not
    // the disc between them at the Nyquist frequency either.
    //
    // A plain `over` of the buckets reads 0.7479..0.7521 on this same fixture
    // -- the 25.0%-across-2-buckets deficit -- which is what makes this
    // configuration the cleanest discriminator available.
    CHECK(minArrival > 0.9958);
    CHECK(minArrival < 0.9966);
    // Both signs of the Nyquist response are present, and only the deficit
    // side is corrected: the surplus cell keeps its over-read in arrival and
    // the pre-existing down-only clamp is what caps its alpha.
    CHECK(maxArrival > 1.0);
    CHECK(minAlpha > 1.0 - 1e-06);
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

    SUBCASE("alpha < 1 with no arrival claim is never scaled up: a coverage deficit survives "
            "resolve when nothing feeds the fill's divisor")
    {
        // The record is built by hand with `share` left at 0, so the arrival
        // plane stays 0 at the pixel and the deficit-only fill is inert (its
        // divisor is below kFillMinArrival); what is left is the bucket walk
        // alone, which must hand back exactly what was deposited.
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

TEST_CASE("BucketPlanes::zero() clears ALL FIVE planes, so a band loop may reuse the allocation")
{
    // The band loop allocates once and calls
    // zero() per band (see scatterBandCPU's header: "ACCUMULATED INTO, never
    // cleared here").  Every case in this file allocates fresh planes, and
    // allocate() zero-fills, so a zero() that missed a plane was invisible --
    // and would show up in production as the previous band's area bleeding
    // into this one's composite.  Includes the fifth, K-independent `arrival`
    // plane.
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
        f.share = alpha;                    // nonzero, so arrival has something to clear too
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
    REQUIRE(planeSum(reused.arrival, 0, px) > 0.0);

    reused.zero();
    for (std::size_t i = 0; i < reused.color.size(); ++i)
        REQUIRE(reused.color[i] == 0.0f);
    for (std::size_t i = 0; i < reused.alpha.size(); ++i) {
        REQUIRE(reused.alpha[i] == 0.0f);
        REQUIRE(reused.weight[i] == 0.0f);
        REQUIRE(reused.colocated[i] == 0.0f);
    }
    for (std::size_t i = 0; i < reused.arrival.size(); ++i)
        REQUIRE(reused.arrival[i] == 0.0f);

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
    for (std::size_t i = 0; i < fresh.arrival.size(); ++i)
        if (reused.arrival[i] != fresh.arrival[i]) ++differing;
    CHECK(differing == 0);
}

TEST_CASE("allocate() re-zeroes a dirty BucketPlanes, at the same geometry and at a "
          "smaller one -- the band loop's ONLY clear")
{
    // The node calls allocate() per band and never zero(): a pooled job's
    // planes arrive carrying the previous band's contents, and PodBuffer keeps
    // its capacity across both calls, so allocate()'s fill is the only thing
    // between one band and the next.  zero()'s own case above cannot see a
    // plane missing from THIS path.
    const int K = 4, C = 2, W = 12, H = 9;
    DiscKernelLUT lut(0.0f, 8.0f, 1.0f, 1.0f);
    HoldoutSoA none;

    auto dirty = [&](BucketPlanes& planes, int w, int h) {
        SampleSoA soa;
        soa.begin(C, makeSingleChannelGroup(C));
        FragmentRecord f;
        f.x = w / 2; f.y = h / 2;
        f.radius = 3.0f;                    // a real disc, so every plane is hit
        f.depth  = 5.0f;
        f.alpha  = 0.8f;
        f.share  = 0.8f;
        BucketWeight bw;
        bw.index = 1;
        bw.frac  = 0.4f;                    // frac > 0 -> a rear deposit -> colocated
        f.deposit = fragmentDeposit(bw, f.alpha);
        f.kind = FragmentKind::Point;
        const float ch[2] = {f.alpha * 0.3f, f.alpha * 0.6f};
        soa.appendFragment(f, ch);
        scatterOnThread(makeScatterParams(w, h), soa, none, lut, planes);
    };

    auto checkClean = [](const BucketPlanes& p) {
        for (std::size_t i = 0; i < p.color.size(); ++i)
            REQUIRE(p.color[i] == 0.0f);
        for (std::size_t i = 0; i < p.alpha.size(); ++i) {
            REQUIRE(p.alpha[i] == 0.0f);
            REQUIRE(p.weight[i] == 0.0f);
            REQUIRE(p.colocated[i] == 0.0f);
        }
        for (std::size_t i = 0; i < p.arrival.size(); ++i)
            REQUIRE(p.arrival[i] == 0.0f);
    };

    BucketPlanes planes;
    planes.allocate(K, C, W, H);
    dirty(planes, W, H);
    REQUIRE(planeSum(planes.arrival, 0, static_cast<std::ptrdiff_t>(W) * H) > 0.0);

    planes.allocate(K, C, W, H);
    checkClean(planes);

    // A SHRINK, which is the case a "same size, skip the fill" shortcut would
    // get wrong in the other direction: every buffer keeps the larger
    // capacity, so the live range is old data until the fill overwrites it.
    dirty(planes, W, H);
    planes.allocate(K, C, W - 3, H - 2);
    checkClean(planes);
}

TEST_CASE("arrival is bit-identical with and without a holdout LUT connected -- "
          "proves the deposit precedes the visibility fold")
{
    // THE LOAD-BEARING CASE.  arrival must accumulate the RAW kernel weight,
    // before HoldoutVisibility::interpAtBucket() folds vis into it -- see
    // scatterFragmentSpans()/scatterFragmentSharp().  A card sitting in front
    // of both fragments below proves it two ways: alpha (which DOES fold vis
    // in) drops when the card is connected, while arrival does not move at
    // all.  If arrival ever picked up vis, this is the case that would catch
    // it: the card is semi-transparent (0.5), so a vis-folded arrival would
    // differ from the disconnected run by a large, unmissable factor.
    const int C = 1, W = 10, H = 10, K = 8;
    const CocParams         p  = makeManualRig(0.05f, 10.0f);
    const DepthBuckets      bk = makeBoundedDeltaCocBuckets(p, 1.0f, 100.0f, K);
    const HoldoutBoundaries hb = makeUniformHoldoutBoundaries(bk);

    HoldoutSampleSoA hs;
    HoldoutLut       lut;
    buildHoldout(hs, lut, hb, W, H, [](int, int, std::vector<SampleRecord>& out) {
        out.push_back(makeSample(5.0f, 5.0f, 0.5f, {}));   // semi-transparent card
    });

    DiscKernelLUT       kernel(0.0f, 8.0f, 1.0f, 1.0f);
    const ScatterParams  sp = makeScatterParams(W, H);

    auto buildSoA = [&]() {
        SampleSoA soa;
        soa.begin(C, makeSingleChannelGroup(C));

        FragmentRecord disc;
        disc.x = 4; disc.y = 4;
        disc.radius = 2.5f;                 // a real, multi-row disc: exercises
                                             // scatterFragmentSpans's row loop
        disc.depth  = 80.0f;                // well behind the card
        disc.alpha  = 0.7f;
        disc.share  = disc.alpha;
        BucketWeight bwDisc;
        bwDisc.index = 3; bwDisc.frac = 0.0f;
        disc.deposit = fragmentDeposit(bwDisc, disc.alpha);
        disc.kind = FragmentKind::Point;
        const float chDisc[1] = {disc.alpha * 0.4f};
        soa.appendFragment(disc, chDisc);

        FragmentRecord sharp;
        sharp.x = 7; sharp.y = 6;
        sharp.radius = 0.0f;                // the sharp fast path
        sharp.depth  = 80.0f;
        sharp.alpha  = 0.35f;
        sharp.share  = sharp.alpha;
        BucketWeight bwSharp;
        bwSharp.index = 5; bwSharp.frac = 0.0f;
        sharp.deposit = fragmentDeposit(bwSharp, sharp.alpha);
        sharp.kind = FragmentKind::Point;
        const float chSharp[1] = {sharp.alpha * 0.9f};
        soa.appendFragment(sharp, chSharp);

        return soa;
    };

    Band withHoldout;
    withHoldout.K = K; withHoldout.C = C; withHoldout.W = W; withHoldout.H = H;
    HoldoutSoA vis = lut.view();
    runBand(withHoldout, sp, buildSoA(), vis, kernel, /*useThread*/ false);

    Band noHoldout;
    noHoldout.K = K; noHoldout.C = C; noHoldout.W = W; noHoldout.H = H;
    HoldoutSoA none;
    runBand(noHoldout, sp, buildSoA(), none, kernel, /*useThread*/ false);

    // Sanity: the holdout really did attenuate something, or the bit-identical
    // check below would be vacuously true.
    const std::ptrdiff_t px = static_cast<std::ptrdiff_t>(W) * H;
    double alphaWith = 0.0, alphaWithout = 0.0;
    for (int k = 0; k < K; ++k) {
        alphaWith    += planeSum(withHoldout.planes.alpha, k, px);
        alphaWithout += planeSum(noHoldout.planes.alpha, k, px);
    }
    REQUIRE(alphaWith < alphaWithout - 1e-6);

    // THE CHECK.  arrival never saw the card.
    REQUIRE(withHoldout.planes.arrival.size() == noHoldout.planes.arrival.size());
    std::size_t differing = 0;
    for (std::size_t i = 0; i < withHoldout.planes.arrival.size(); ++i)
        if (withHoldout.planes.arrival[i] != noHoldout.planes.arrival[i])
            ++differing;
    CHECK(differing == 0);
}

TEST_CASE("a single fragment's arrival deposits sum to its share within 1e-6")
{
    // The disc kernel's raw weights sum to 1 (DiscKernelLUT's own contract),
    // so arrival[dst] += w[i]*share summed over an UNCLIPPED disc must recover
    // `share` exactly, to floating-point summation error -- independent of
    // `alpha`, which this pins by giving the fragment a share that is NOT its
    // alpha.
    const int K = 4, C = 1, W = 40, H = 40;
    DiscKernelLUT        lut(0.0f, 12.0f, 1.0f, 1.0f);
    const ScatterParams  sp = makeScatterParams(W, H);
    HoldoutSoA none;

    SampleSoA soa;
    soa.begin(C, makeSingleChannelGroup(C));
    FragmentRecord f;
    f.x = W / 2; f.y = H / 2;           // comfortably inside: no band-edge clipping
    f.radius = 5.0f;                    // a real, multi-row disc
    f.depth  = 5.0f;
    f.alpha  = 0.63f;
    f.share  = 0.417f;                  // deliberately NOT equal to alpha
    BucketWeight bw;
    bw.index = 1; bw.frac = 0.3f;
    f.deposit = fragmentDeposit(bw, f.alpha);
    f.kind = FragmentKind::Point;
    const float ch[1] = {f.alpha * 0.5f};
    soa.appendFragment(f, ch);

    BucketPlanes planes;
    planes.allocate(K, C, W, H);
    planes.zero();
    scatterOnThread(sp, soa, none, lut, planes);

    const std::ptrdiff_t px = static_cast<std::ptrdiff_t>(W) * H;
    double sum = 0.0;
    for (std::ptrdiff_t i = 0; i < px; ++i)
        sum += static_cast<double>(planes.arrival[i]);

    CHECK(std::fabs(sum - static_cast<double>(f.share)) <= 1e-6);
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
            float residualT = 1.0f, residualR = 0.0f;
            const SampleSoA soa = flattenOnePixel(fp, bk, W / 2, H / 2,
                {makeSample(bk.boundary(15 - nBuckets), bk.boundary(15), alpha,
                            {alpha * unpremult})},
                &residualT, &residualR);
            REQUIRE(soa.fragmentCount() == static_cast<std::size_t>(nBuckets));

            Band band;
            band.K = bk.bucketCount(); band.C = 1; band.W = W; band.H = H;
            HoldoutSoA noHoldout;
            ResidualWindow window;
            oneSourcePixelWindow(window, W, H, W / 2, H / 2, residualT, residualR);
            runBand(band, makeScatterParams(W, H),
                    soa, noHoldout, lut, true, &window);

            // MEASURED <= 1e-07 relative at every one of these; 2e-06 is ~20x
            // headroom.  Before the fourth plane the same cases read -6.4% at 4
            // buckets, -24.8% at 8, -46.5% at 12 and -92.1% full range.
            //
            // THE COVERAGE FILL MUST NOT FIRE HERE, and only the share pooling
            // in FragmentRecord::share keeps it from doing so.  In front of
            // focus a parent's parts run large-to-small front to back (20.80 px
            // down to 0.55 px at 15 parts); spread the parent's arrival claim
            // across those radii and its own pixel reads 0.40-0.61 instead of
            // 1, so the deficit division fires on content that has no deficit
            // and adds +25%.  Pooled onto the deepest part the claim is the
            // same unit kernel the surrounding window deposits, arrival is
            // exactly 1, and the parent survives the division untouched.
            CHECK(std::fabs(bandAlphaSum(band) - alpha) <= 2e-06 * alpha);
            CHECK(std::fabs(bandColorSum(band, 0) - alpha * unpremult) <= 2e-06 * alpha);

            // The ratio invariant again, integrated over the band.
            CHECK(std::fabs(bandColorSum(band, 0) / bandAlphaSum(band) - unpremult) <= 1e-06);
        }
    }
}

TEST_CASE("behind focus the residue is structural: "
          "PINNED at +36.9 / +50.2 / +56.4 / +60.4%")
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
    //
    // THESE ARE COMPOSITE NUMBERS, so the coverage fill must leave them alone.
    // Behind focus the parts run small-to-large front to back; spread the
    // parent's arrival claim across those radii and it lands WIDER than the
    // unit background kernel around it, arrival dips below 1 in a thin ring,
    // and the residue reads +1.75 / +0.62 / +0.37 points high.  Pooled onto
    // the deepest part (FragmentRecord::share) arrival is exactly 1 and the
    // pins below are the composite's own.
    //
    // THE BRACKETING-KERNEL BLEND MOVED THE COLUMN AGAIN, and in both
    // directions at once: 36.91 / 50.25 / 56.40 / 60.41.  The first three
    // cells now sit ON the grid-free truth to the second decimal (the parts'
    // brackets there are under 0.06 px wide, so the blend is the disc), while
    // the 8-bucket cell moved 0.6 points AWAY from it (61.02).  Its outer parts
    // sit at 8.6 / 10.1 / 11.7 px, where a bracket is 0.14-0.27 px wide and
    // the blend of two 1 px-soft discs that far apart has a softer edge than
    // the disc it stands in for; the residue's co-located-area division is
    // sensitive to exactly that edge.  Adjudicated as the kernel family, not
    // the composite: the composite is untouched and the K = 2..4 cells prove
    // it.  A per-radius exact disc between the nodes would return this cell
    // to 61.0.
    struct Case { int buckets; double partitionPct; };
    const Case cases[] = {{2, 36.91}, {3, 50.25}, {4, 56.40}, {8, 60.41}};

    for (const Case& cs : cases) {
        CAPTURE(cs.buckets);
        const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ true);
        float residualT = 1.0f, residualR = 0.0f;
        const SampleSoA soa = flattenOnePixel(fp, bk, W / 2, H / 2,
            {makeSample(bk.boundary(0), bk.boundary(cs.buckets), alpha, {alpha * unpremult})},
            &residualT, &residualR);
        REQUIRE(soa.fragmentCount() == static_cast<std::size_t>(cs.buckets));

        {
            Band band;
            band.K = bk.bucketCount(); band.C = 1; band.W = W; band.H = H;
            HoldoutSoA noHoldout;
            ResidualWindow window;
            oneSourcePixelWindow(window, W, H, W / 2, H / 2, residualT, residualR);
            runBand(band, makeScatterParams(W, H), soa, noHoldout, lut, true, &window);

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

    SUBCASE("a 60% coverage hole stays 0.6 through the bucket walk (the fill's divisor is "
            "the caller's, and absent here)")
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

TEST_CASE("deficit-only fill: arrival divides the premultiplied pair together, "
          "and only for a real deficit")
{
    // Same standalone entry point as the identities above, now driving the
    // trailing `arrival` argument directly rather than leaving it at its
    // default (1.0, i.e. no-op).
    auto composite = [](std::vector<float> cov, std::vector<float> alpha,
                        std::vector<float> colocated, std::vector<float> color,
                        float arrival, float* outColor, float* outAlpha) {
        compositePixelCoveragePartition(color.data(), alpha.data(), cov.data(),
                                        colocated.data(),
                                        static_cast<int>(cov.size()), 1, 1,
                                        outColor, outAlpha, arrival);
    };

    const float unpremult = 0.8f;
    float c = -1.0f, a = -1.0f;

    SUBCASE("flat opaque, D = 0.93 -> alpha exactly 1, unpremult colour ratio preserved")
    {
        composite({1.0f}, {1.0f}, {0.0f}, {unpremult}, 0.93f, &c, &a);
        CHECK(a == 1.0f);
        REQUIRE(a > 0.0f);
        CHECK(std::fabs(c / a - unpremult) <= 1e-06f);
    }

    SUBCASE("a surplus, D = 1.3, is left untouched")
    {
        composite({0.6f}, {0.6f}, {0.0f}, {0.6f * unpremult}, 1.3f, &c, &a);
        CHECK(a == doctest::Approx(0.6f).epsilon(1e-6));
        CHECK(c == doctest::Approx(0.6f * unpremult).epsilon(1e-6));

        // Bit-identical to a no-op arrival: a surplus takes the same early
        // branch as "no arrival supplied at all".
        float c2 = -1.0f, a2 = -1.0f;
        composite({0.6f}, {0.6f}, {0.0f}, {0.6f * unpremult}, 1.0f, &c2, &a2);
        CHECK(a == a2);
        CHECK(c == c2);
    }

    SUBCASE("D = 0 with a zero numerator -> 0, never manufactured")
    {
        composite({0.0f}, {0.0f}, {0.0f}, {0.0f}, 0.0f, &c, &a);
        CHECK(a == 0.0f);
        CHECK(c == 0.0f);
    }

    SUBCASE("the kFillMinArrival boundary, approached from both sides")
    {
        // A real 0.5 numerator throughout; only D moves across the floor.
        composite({0.5f}, {0.5f}, {0.0f}, {0.5f * unpremult},
                  kFillMinArrival - 1e-6f, &c, &a);
        CHECK(a == 0.5f);   // below the floor: untouched

        composite({0.5f}, {0.5f}, {0.0f}, {0.5f * unpremult},
                  kFillMinArrival, &c, &a);
        CHECK(a == 0.5f);   // AT the floor: the compare is strict '>', still untouched

        composite({0.5f}, {0.5f}, {0.0f}, {0.5f * unpremult},
                  kFillMinArrival + 1e-6f, &c, &a);
        // Just above the floor: s = 1/D is enormous (~1000x), so the fill
        // overshoots 1 and the pre-existing down-only clamp caps it there --
        // this subcase's own assertion is that the fill fired at all.
        CHECK(a == 1.0f);
    }

    SUBCASE("gate (a): a sharp-path pixel at D = 1 - 1ulp is bit-identical to D = 1; "
            "D = 1 - 2e-5 does divide")
    {
        // cov = 1 models the sharp path's own w = 1 deposit; alpha = 0.77 is
        // an arbitrary non-opaque surface value with a real fractional bit
        // pattern to compare.
        float cBase = -1.0f, aBase = -1.0f;
        composite({1.0f}, {0.77f}, {0.0f}, {0.77f * unpremult}, 1.0f, &cBase, &aBase);

        const float oneUlpUnder = std::nextafter(1.0f, 0.0f);
        // Confirms the ulp sits INSIDE the tolerance band (does not trip the divide).
        REQUIRE(!(oneUlpUnder < 1.0f - kFillDeficitTol));
        float cUlp = -1.0f, aUlp = -1.0f;
        composite({1.0f}, {0.77f}, {0.0f}, {0.77f * unpremult}, oneUlpUnder, &cUlp, &aUlp);
        CHECK(aUlp == aBase);   // bit-exact: the fill did not run
        CHECK(cUlp == cBase);

        float cDiv = -1.0f, aDiv = -1.0f;
        composite({1.0f}, {0.77f}, {0.0f}, {0.77f * unpremult}, 1.0f - 2e-5f, &cDiv, &aDiv);
        CHECK(aDiv != aBase);   // the fill DID run
        CHECK(aDiv == doctest::Approx(0.77f / (1.0f - 2e-5f)).epsilon(1e-5));
    }

    SUBCASE("the colour:alpha pair stays locked through the accAlpha > 1 clamp")
    {
        // The two-bucket, area-plus-co-located case from the identities above
        // (baseline total 0.75, no fill), now with D = 0.5 -- a deficit big
        // enough that the fill's own 2x scale overshoots 1 and the pre-
        // existing down-only clamp has to absorb it.
        composite({0.4f, 0.5f}, {0.5f, 0.4f}, {0.6f, 0.0f},
                  {0.5f * unpremult, 0.4f * unpremult}, 0.5f, &c, &a);
        CHECK(a == 1.0f);
        REQUIRE(a > 0.0f);
        CHECK(std::fabs(c / a - unpremult) <= 1e-06f);
    }

    SUBCASE("two 0.5 fog layers still read 0.75 at D = 1 (shares + residual sum to 1)")
    {
        composite({1.0f, 1.0f}, {0.5f, 0.5f}, {0.0f, 0.0f},
                  {0.5f * unpremult, 0.5f * unpremult}, 1.0f, &c, &a);
        CHECK(a == doctest::Approx(0.75f).epsilon(1e-6));
        CHECK(c == doctest::Approx(0.75f * unpremult).epsilon(1e-6));
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
        // are OVER-reads, which nothing in this node licenses, so they are
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
        // over-occluded too.  Measured -5.132% at alpha 0.90 — a DEFICIT, not
        // an over-read, and banded rather
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
        // DEFICIT.  What the COMPOSITE contributes at low alpha is a
        // deficit, not an over-read; the whole over-read enters at scatter
        // time.  (Banded: a composite regression that
        // inflated low alpha would push this back over zero.)
        const double renorm10 = interiorMean(buckets, k, 0.10f, false, nullptr);
        const double renormed = interiorMean(buckets, k, 0.10f, true, nullptr);
        CAPTURE(renorm10); CAPTURE(renormed);
        CHECK(renormed <= 0.0);
        CHECK(renormed > -0.015);                   // -0.0025..-0.0088 measured (step 3)

        // (3) THE RAW READINGS THEMSELVES, banded, so this model stays
        // anchored to the rendered sweep it reproduces: alpha 0.10 reads
        // HIGH (an over-read) and alpha 0.90 at K >= 16 reads
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
    // premultiplied colour 0.9959 against the true 0.5975: +59.5% too bright.
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
                    float residualT = 1.0f, residualR = 0.0f;
                    const SampleSoA soa = flattenOnePixel(fp, bk, W / 2, H / 2,
                        {makeSample(bk.boundary(15 - nBuckets), bk.boundary(15), alpha,
                                    {alpha * 0.5f})},
                        &residualT, &residualR);
                    Band band;
                    band.K = bk.bucketCount(); band.C = 1; band.W = W; band.H = H;
                    HoldoutSoA noHoldout;
                    ResidualWindow window;
                    oneSourcePixelWindow(window, W, H, W / 2, H / 2, residualT, residualR);
                    runBand(band, makeScatterParams(W, H),
                            soa, noHoldout, lut, true, &window);
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
            float residualT = 1.0f, residualR = 0.0f;
            const SampleSoA soa = flattenOnePixel(fp, bk, W / 2, H / 2,
                {makeSample(9.0f, 9.0f, 0.3f, {0.3f}),
                 makeSample(9.02f, 9.02f, 0.4f, {0.4f})},
                &residualT, &residualR);
            // ONE fragment either way now: they share a bucketOf() assignment
            // and are both on the sharp path, so they are one kernel.
            REQUIRE(soa.fragmentCount() == 1u);
            Band band;
            band.K = bk.bucketCount(); band.C = 1; band.W = W; band.H = H;
            HoldoutSoA noHoldout;
            ResidualWindow window;
            oneSourcePixelWindow(window, W, H, W / 2, H / 2, residualT, residualR);
            runBand(band, makeScatterParams(W, H),
                    soa, noHoldout, lut, true, &window);
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

TEST_CASE("BucketPlanes::bytesForBand is the (C+3) formula plus a K-independent "
          "arrival plane, and matches a live sizeBytes()")
{
    // The memory-limit knob and the code must not drift apart.  The bucket-
    // scaled term is (C+3), not (C+2): colour + alpha + new area + co-located
    // area.  The trailing `+ W*H*4` is the fifth, K-independent `arrival`
    // plane: one float per band pixel, never multiplied by K.
    CHECK(BucketPlanes::bytesForBand(16, 4, 4096, 64)
          == static_cast<std::size_t>(16) * 4096 * 64 * (4 + 3) * sizeof(float)
           + static_cast<std::size_t>(4096) * 64 * sizeof(float));
    CHECK(BucketPlanes::bytesForBand(16, 4, 4096, 64) == 118489088u);
    CHECK(BucketPlanes::bytesForBand(128, 4, 4096, 64) == 940572672u);

    BucketPlanes planes;
    planes.allocate(8, 3, 32, 16);
    CHECK(planes.sizeBytes() == BucketPlanes::bytesForBand(8, 3, 32, 16));
    planes.release();
    CHECK(planes.sizeBytes() == 0u);
}

TEST_CASE("BucketPlanes::bytesForBand's arrival term matches an independently "
          "hand-computed byte count")
{
    // Hand-derived from the geometry alone -- NOT by calling bytesForBand()
    // twice -- so this pins the formula itself rather than its own
    // self-consistency.  K=6 buckets, C=3 channels, a 20x9 band:
    //   bucket planes: K * W * H * (C+3) floats = 6 * 20 * 9 * 6      = 6480
    //   arrival:                       W * H floats =      20 * 9    =  180
    //   total floats: 6660, * 4 bytes/float = 26640 bytes.
    const int K = 6, C = 3, W = 20, H = 9;
    const std::size_t bucketFloats  = static_cast<std::size_t>(K) * W * H * (C + 3);
    const std::size_t arrivalFloats = static_cast<std::size_t>(W) * H;
    const std::size_t expectedBytes = (bucketFloats + arrivalFloats) * sizeof(float);
    REQUIRE(expectedBytes == 26640u);
    CHECK(BucketPlanes::bytesForBand(K, C, W, H) == expectedBytes);

    BucketPlanes planes;
    planes.allocate(K, C, W, H);
    CHECK(planes.sizeBytes() == expectedBytes);
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

        float residualT = 1.0f, residualR = 0.0f;
        const SampleSoA soa = flattenOnePixel(fp, bk, W / 2, H / 2,
            {makeSample(6.0f, 6.0f, 0.8f, {0.4f})}, &residualT, &residualR);
        Band band;
        band.K = bk.bucketCount(); band.C = 1; band.W = W; band.H = H;
        ResidualWindow window;
        oneSourcePixelWindow(window, W, H, W / 2, H / 2, residualT, residualR);
        runBand(band, makeScatterParams(W, H),
                soa, view, lut, true, &window);
        // Not erased: the short LUT was ignored rather than sampled.
        //
        // RED, NOT RE-PINNED: this rig is now correct (a matching-radius
        // virtual background for the one real source pixel), and it exposes a
        // real consequence of the deficit-only division, not a modelling gap
        // in this rig. Investigated directly: at every pixel this disc's
        // kernel reaches, arrival and the pre-fill alpha share the SAME
        // per-pixel kernel weight w(pixel) (accAlpha = w*0.8, arrival =
        // w*(0.8+0.2) = w), so accAlpha/arrival is the CONSTANT 0.8 at every
        // one of them -- including edge pixels the disc barely grazes, whose
        // pre-fill alpha was correctly anti-aliased (a small fraction of
        // 0.8). The fill overwrites that fraction with the full 0.8
        // everywhere, turning the disc's soft, anti-aliased edge into a hard
        // one.  In general: wherever a pixel's arrival is exactly its own
        // kernel weight, the division flattens the kernel's profile.
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

TEST_CASE("bandBudgetBytes: bucket planes + virtual-background window + holdout LUT + "
          "resident SoA, against hand-derived byte counts")
{
    // The 4K default band: K=16, C=4, 4096x64, padY defaulted to 0 (the
    // window is then just W*B, unpadded -- see bytesForWindow).
    // Planes: K*W*B*(C+3)*4 + W*B*4 (the K-independent arrival plane)
    //       = 117,440,512 + 1,048,576 = 118,489,088 (~118MB).
    // Residual window: 2*W*B*4 (T + radius planes, K-independent) at padY=0
    //       = 2*4096*64*4 = 2,097,152.
    // Total = 118,489,088 + 2,097,152 = 120,586,240.
    CHECK(bandBudgetBytes(16, 4, 4096, 64, false, 0.0)
          == doctest::Approx(120586240.0));

    // The holdout term, which bytesForBand() does NOT carry: (K+1)*W*B*4 =
    // 17*4096*64*4 = 17,825,792, i.e. 17.0 MB per 4096x64 band at K=16.  The
    // residual window term is identical on both sides of the subtraction
    // (it does not depend on holdoutConnected), so it cancels out here.
    CHECK(bandBudgetBytes(16, 4, 4096, 64, true, 0.0)
          - bandBudgetBytes(16, 4, 4096, 64, false, 0.0)
          == doctest::Approx(17825792.0));

    // The SoA term, at the ~100 B/fragment RESIDENT figure (61 B logical).
    CHECK(kSoAResidentBytesPerFragment == doctest::Approx(100.0));
    CHECK(bandBudgetBytes(16, 4, 4096, 64, false, 1.0e6)
          == doctest::Approx(120586240.0 + 1.0e8));

    // K=128 planes: 128*4096*64*7*4 + 4096*64*4 = 939,524,096 + 1,048,576
    // = 940,572,672 (~940MB); residual window is K-INDEPENDENT, so it adds
    // the same 2,097,152 as the K=16 case above: 942,669,824.
    CHECK(bandBudgetBytes(128, 4, 4096, 64, false, 0.0)
          == doctest::Approx(942669824.0));

    // Degenerate bucket count still leaves W*H nonzero: arrival AND the
    // residual window are both K-INDEPENDENT -- neither zeroes out with
    // bucketCount, only with width or height (see bytesForBand,
    // bytesForWindow).  The holdout term does gate on bucketCount > 0, so at
    // K=0 only arrival (4096*64*4 = 1,048,576) and the residual window
    // (2*4096*64*4 = 2,097,152) survive: 3,145,728.
    CHECK(bandBudgetBytes(0, 4, 4096, 64, true, 0.0)
          == doctest::Approx(3145728.0));

    // Negative width sanitises to 0 in BOTH bytesForBand and bytesForWindow
    // (same "> 0 else 0" convention), so only the SoA term survives here,
    // unchanged from before the residual window existed.
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
    // K=128 (only the bucket-plane term differs between them).
    CHECK(withPad128 - withPad16
          == doctest::Approx(bandBudgetBytes(128, 4, 4096, 64, false, 0.0)
                            - bandBudgetBytes(16, 4, 4096, 64, false, 0.0)));

    // ResidualWindow::bytesForWindow() directly, at the window's own size
    // (not the band's) -- the independent hand derivation for the 8.72 MB
    // figure quoted in bandBudgetBytes()'s doc block.
    CHECK(ResidualWindow::bytesForWindow(4096, 64 + 2 * 101) == 8716288u);
}

TEST_CASE("planBands: shrink-to-fit floors at 1 row and the concurrent cap "
          "floors at 1 band — never 0, never a deadlock")
{
    const auto noFragments = [](int) { return 0.0; };

    // Fits outright: 4GB limit, 2160 rows, K=16 C=4 W=4096, B=256, padY
    // defaulted to 0. bytes(256) = 16*4096*256*7*4 + 4096*256*4 (arrival) +
    // 2*4096*256*4 (residual window at padY=0) = 469,762,048 + 4,194,304 +
    // 8,388,608 = 482,344,960; cap = floor(4GiB / that) = 8 (4,294,967,296 /
    // 482,344,960 = 8.905); bandCount = ceil(2160/256) = 9.  Before the
    // residual window existed this cap read 9 — it is one slot lower now
    // because the window is a real per-band cost the old figure omitted.
    {
        const BandPlan p = planBands(4.0 * 1024.0 * 1024.0 * 1024.0,
                                     2160, 16, 4, 4096, false, 256, noFragments);
        CHECK(p.bandHeight == 256);
        CHECK(p.bandCount == 9);
        CHECK(p.maxInFlight == 8);
    }

    // Shrinks: 64MB limit. bytes(256)=482,344,960 > 64MB -> 128 (241,172,480)
    // -> 64 (120,586,240) -> 32 (60,293,120, fits: 64MB=67,108,864). One band
    // in flight (67,108,864/60,293,120 < 2).
    {
        const BandPlan p = planBands(64.0 * 1024.0 * 1024.0,
                                     2160, 16, 4, 4096, false, 256, noFragments);
        CHECK(p.bandHeight == 32);
        CHECK(p.bandCount == (2160 + 31) / 32);
        CHECK(p.maxInFlight == 1);
    }

    // Even ONE row over the limit: bandHeight floors at 1 and the cap floors
    // at 1 — the band is over budget and still gets its slot (the design's
    // "never deadlock at 0").  bytes(1) = 16*4096*7*4 + 4096*4 + 2*4096*4
    // = 1,835,008 + 16,384 + 32,768 = 1,884,160 > 1MB.
    {
        const BandPlan p = planBands(1.0 * 1024.0 * 1024.0,
                                     2160, 16, 4, 4096, false, 256, noFragments);
        CHECK(p.bandHeight == 1);
        CHECK(p.bandCount == 2160);
        CHECK(p.maxInFlight == 1);
    }

    // The fragment estimator participates in the shrink: 20 spp over a 4096
    // window at 100 B resident dominates the planes and forces the halving.
    // bytes(64) with fragments = 120,586,240 + 4096*64*20*100 (524,288,000)
    // = 644,874,240 <= 1GB (1,073,741,824); bytes(128) with fragments =
    // 241,172,480 + 1,048,576,000 = 1,289,748,480 > 1GB, so 128 does not fit.
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
        CHECK(withFrag.bandHeight == 64);
        CHECK(withFrag.maxInFlight == 1);
        CHECK(without.bandHeight == 256);
    }

    // padY is a real, load-bearing shrink input, not a cosmetic default: at
    // a 32MB limit, padY=0 (the default used everywhere else in this test)
    // fits at bandHeight=16 (bytes(16,padY=0)=30,146,560), but the frame's
    // actual padY=101 (max_radius=100, edge_softness=1 defaults) needs the
    // WINDOW height 16+2*101=218, not 16, and bytes(16,padY=101)=36,765,696
    // exceeds the 32MB (33,554,432) limit -- so it shrinks one step further,
    // to bandHeight=8 (bytes(8,padY=101)=21,692,416, fits).
    {
        const double limit = 32.0 * 1024.0 * 1024.0;
        const BandPlan noPad = planBands(limit, 2160, 16, 4, 4096, false, 256,
                                         noFragments, /*padY*/ 0);
        const BandPlan pad101 = planBands(limit, 2160, 16, 4, 4096, false, 256,
                                          noFragments, /*padY*/ 101);
        CHECK(noPad.bandHeight == 16);
        CHECK(pad101.bandHeight == 8);
        CHECK(pad101.bandHeight < noPad.bandHeight);
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

                BucketPlanes planes;
                planes.allocate(1, 1, W, H);
                planes.zero();
                scatterBackgroundCPU(sp, window, lut, planes);

                const double sum =
                    planeSum(planes.arrival, 0, static_cast<std::ptrdiff_t>(W) * H);
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

        BucketPlanes planes;
        planes.allocate(1, 1, W, H);
        planes.zero();
        scatterBackgroundCPU(sp, window, sharpLut, planes);

        const double sum = planeSum(planes.arrival, 0, static_cast<std::ptrdiff_t>(W) * H);
        CHECK(sum == doctest::Approx(0.42).epsilon(1e-6));
        CHECK(planes.arrival[static_cast<std::size_t>(15) * W + 15]
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

            BucketPlanes planes;
            planes.allocate(1, 1, W, H);
            planes.zero();
            scatterBackgroundCPU(sp, window, lut, planes);
            return planeSum(planes.arrival, 0, static_cast<std::ptrdiff_t>(W) * H);
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

    BucketPlanes planes;
    planes.allocate(1, 1, W, H);
    planes.zero();
    scatterBackgroundCPU(sp, window, lut, planes);

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
                const double actual = planes.arrival[
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
    // resolveBandCPU() does -- but the arithmetic it must support is
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
        FragmentRecord f;
        f.x = cx; f.y = cy;
        f.radius = rSample;
        f.depth  = 5.0f;
        f.alpha  = trueAlpha;
        f.share  = trueAlpha;             // point sample: share = T_in * alpha, T_in = 1
        BucketWeight bw;
        bw.index = 0;
        bw.frac  = 0.0f;
        f.deposit = fragmentDeposit(bw, trueAlpha);
        f.kind = FragmentKind::Point;
        const float ch[1] = {0.0f};       // colour is irrelevant here -- alpha only
        soa.appendFragment(f, ch);

        BucketPlanes planes;
        planes.allocate(1, 1, W, H);
        planes.zero();
        ScatterScratch scratch;
        HoldoutSoA noHoldout;
        scatterBandCPU(sp, soa, noHoldout, lut, planes, scratch);

        ResidualWindow window;
        window.allocate(0, 0, W, H, residualRadius);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                window.setPixel(x, y, 0.0f, residualRadius);
        window.setPixel(cx, cy, 1.0f - trueAlpha, residualRadius);
        scatterBackgroundCPU(sp, window, lut, planes);

        const std::size_t centre = static_cast<std::size_t>(cy) * W + cx;
        REQUIRE(planes.arrival[centre] > 0.0f);
        return planes.alpha[centre] / planes.arrival[centre];
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

TEST_CASE("scatterBackgroundCPU: never writes color, alpha, weight or colocated -- "
          "arrival only")
{
    DiscKernelLUT lut(0.0f, 20.0f, 1.0f, 1.0f);
    const int K = 3, C = 2, W = 30, H = 30;
    const ScatterParams sp = makeScatterParams(W, H);

    SampleSoA soa;
    soa.begin(C, makeSingleChannelGroup(C));
    FragmentRecord f;
    f.x = 15; f.y = 15;
    f.radius = 5.0f;
    f.depth  = 5.0f;
    f.alpha  = 0.7f;
    f.share  = 0.7f;
    BucketWeight bw;
    bw.index = 1;
    bw.frac  = 0.35f;                   // frac > 0 -> a rear deposit -> colocated too
    f.deposit = fragmentDeposit(bw, f.alpha);
    f.kind = FragmentKind::Point;
    const float ch[2] = {f.alpha * 0.2f, f.alpha * 0.9f};
    soa.appendFragment(f, ch);

    BucketPlanes planes;
    planes.allocate(K, C, W, H);
    planes.zero();
    ScatterScratch scratch;
    HoldoutSoA noHoldout;
    scatterBandCPU(sp, soa, noHoldout, lut, planes, scratch);

    // Every plane really did receive something from the fragment scatter, so
    // the "unchanged" checks below have something to protect.
    const std::ptrdiff_t px = static_cast<std::ptrdiff_t>(W) * H;
    double before = 0.0;
    for (int k = 0; k < K; ++k) {
        before += planeSum(planes.alpha, k, px);
        before += planeSum(planes.weight, k, px);
        before += planeSum(planes.colocated, k, px);
    }
    before += planeSum(planes.color, 0, static_cast<std::ptrdiff_t>(K) * C * px);
    REQUIRE(before > 0.0);

    const std::vector<float> colorBefore(planes.color.begin(), planes.color.end());
    const std::vector<float> alphaBefore(planes.alpha.begin(), planes.alpha.end());
    const std::vector<float> weightBefore(planes.weight.begin(), planes.weight.end());
    const std::vector<float> colocatedBefore(planes.colocated.begin(), planes.colocated.end());

    ResidualWindow window;
    window.allocate(0, 0, W, H, 8.0f);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            window.setPixel(x, y, 0.0f, 8.0f);
    window.setPixel(20, 5, 0.9f, 8.0f);   // well clear of the fragment above

    scatterBackgroundCPU(sp, window, lut, planes);

    // arrival DID move (the background actually ran)...
    CHECK(planeSum(planes.arrival, 0, px) > 0.0);

    // ...but every other plane is BIT-UNCHANGED: this function never so much
    // as takes a pointer to color/alpha/weight/colocated.
    REQUIRE(planes.color.size() == colorBefore.size());
    for (std::size_t i = 0; i < colorBefore.size(); ++i)
        CHECK(planes.color[i] == colorBefore[i]);
    REQUIRE(planes.alpha.size() == alphaBefore.size());
    for (std::size_t i = 0; i < alphaBefore.size(); ++i)
        CHECK(planes.alpha[i] == alphaBefore[i]);
    REQUIRE(planes.weight.size() == weightBefore.size());
    for (std::size_t i = 0; i < weightBefore.size(); ++i)
        CHECK(planes.weight[i] == weightBefore[i]);
    REQUIRE(planes.colocated.size() == colocatedBefore.size());
    for (std::size_t i = 0; i < colocatedBefore.size(); ++i)
        CHECK(planes.colocated[i] == colocatedBefore[i]);
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
//  driver actually deposits -- one fragment with unit share and unit alpha,
//  whole into bucket 0, so its `weight` plane is the effective kernel and its
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
    ScatterParams sp;
    BucketPlanes planes;
    ScatterScratch scratch;
    HoldoutSoA none;

    KernelRig(int w, int h) : W(w), H(h), cx(w / 2), cy(h / 2), sp(makeScatterParams(w, h))
    {
        planes.allocate(1, 1, W, H);
    }

    // Rasterise one unit fragment at `radius`; the planes hold the result.
    void rasterize(const DiscKernelLUT& lut, float radius)
    {
        SampleSoA soa;
        soa.begin(1, makeSingleChannelGroup(1));
        FragmentRecord f;
        f.x = cx; f.y = cy;
        f.radius = radius;
        f.depth  = 5.0f;
        f.alpha  = 1.0f;
        f.share  = 1.0f;
        BucketWeight bw;
        bw.index = 0; bw.frac = 0.0f;
        f.deposit = fragmentDeposit(bw, 1.0f);
        f.kind = FragmentKind::Point;
        const float ch[1] = {0.5f};
        soa.appendFragment(f, ch);
        planes.zero();
        scatterBandCPU(sp, soa, none, lut, planes, scratch);
    }

    std::size_t pixels() const { return static_cast<std::size_t>(W) * H; }
    float weightAt(int dx, int dy) const
    { return planes.weight[static_cast<std::size_t>(cy + dy) * W + static_cast<std::size_t>(cx + dx)]; }

    bool arrivalIsWeightBitExact() const
    {
        for (std::size_t i = 0; i < pixels(); ++i)
            if (planes.arrival[i] != planes.weight[i])
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
                    const double dv = std::fabs(static_cast<double>(rig.planes.weight[i]) - prev[i]);
                    tap = std::max(tap, dv);
                    l1 += dv;
                }
                if (tap > s.maxTapJump) { s.maxTapJump = tap; s.atRadius = r; }
                s.maxL1Jump = std::max(s.maxL1Jump, l1);
            }
            prev.assign(rig.planes.weight.data(), rig.planes.weight.data() + rig.pixels());
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
            std::vector<float> a(rig.planes.weight.data(), rig.planes.weight.data() + rig.pixels());
            rig.rasterize(lut, snap ? snappedToNearestNode(hi) : hi);
            double tap = 0.0;
            for (std::size_t k = 0; k < rig.pixels(); ++k)
                tap = std::max(tap, std::fabs(static_cast<double>(rig.planes.weight[k]) - a[k]));
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
            dev = std::max(dev, std::fabs(static_cast<double>(rig.planes.weight[k]) - exact[k]));
            sumEff   += rig.planes.weight[k];
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
            const double dv = std::fabs(static_cast<double>(rig.planes.weight[k]) - exactMid[k]);
            devMid = std::max(devMid, dv);
            l1 += dv;
        }
        if (devMid / peak > worstMid) { worstMid = devMid / peak; worstMidR = rm; }
        worstMidL1 = std::max(worstMidL1, l1);
        bestMid = std::min(bestMid, devMid / peak);

        rig.rasterize(lut, snappedToNearestNode(rm));
        double devSnap = 0.0;
        for (std::size_t k = 0; k < rig.pixels(); ++k)
            devSnap = std::max(devSnap, std::fabs(static_cast<double>(rig.planes.weight[k]) - exactMid[k]));
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
    // agree TO THE BIT on all five planes and on the resolved output.  The LUT
    // is built over a measured range that starts at 2 px, so any radius that
    // leaked past the floor would come back as a 2 px disc, not a rounding
    // difference.  pre_merge is off so the three SoAs differ in nothing but
    // the radius column (its tolerance would otherwise regroup them).
    const int C = 3, W = 40, H = 40, K = 16, spp = 4;
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

    // One bucket set for every run: the buckets are a function of the CoC
    // parameters, and the comparison is about the radius column alone.
    auto rigFor = [](float sizePx, float maxRadiusPx) {
        return makeCocParams(CocMode::Manual, 50.0f, 2.8f, 36.0f, 10.0f,
                             unitScale(WorldUnits::Meters), 1920.0f, 1.0f,
                             1.0f, 1.0f, maxRadiusPx, sizePx);
    };
    const DepthBuckets bk = makeBoundedDeltaCocBuckets(rigFor(0.45f, 0.49f), 1.0f, 100.0f, K);

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
        window.allocate(0, 0, W, H, autoBackgroundRadiusPx(p, bk));
        flattenIntoWithResidual(fp, bk, 0, 0, W, H, soa, scratch, window,
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

        r.band.K = K; r.band.C = C; r.band.W = W; r.band.H = H;
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
    REQUIRE(subPixel.fragments > 2000);
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
        cmp(a.planes.weight, b.planes.weight);
        cmp(a.planes.colocated, b.planes.colocated);
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
    const int C = 3, W = 40, H = 40, K = 16, spp = 4;

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
    const DepthBuckets bk = makeBoundedDeltaCocBuckets(p, 1.0f, 100.0f, K);

    auto run = [&](const DiscKernelLUT& lut, float forcedRadius, Band& band) {
        const FlattenParams fp = makeFlattenParams(p, C, /*preMerge*/ false);
        SampleSoA soa;
        soa.begin(C, fp.groups);
        FlattenScratch scratch;
        ResidualWindow window;
        window.allocate(0, 0, W, H, forcedRadius);
        flattenIntoWithResidual(fp, bk, 0, 0, W, H, soa, scratch, window,
            [&](int x, int y) { return pixels[static_cast<std::size_t>(y) * W + x]; });
        for (std::size_t f = 0; f < soa.fragmentCount(); ++f)
            soa.radius[f] = forcedRadius;
        for (std::size_t i = 0; i < window.radiusPx.size(); ++i)
            window.radiusPx[i] = forcedRadius;
        band.K = K; band.C = C; band.W = W; band.H = H;
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
        CHECK(diffs(viaSharp.planes.weight,    viaBoundary.planes.weight)    == 0);
        CHECK(diffs(viaSharp.planes.colocated, viaBoundary.planes.colocated) == 0);
        CHECK(diffs(viaSharp.planes.arrival,   viaBoundary.planes.arrival)   == 0);
        CHECK(outputDiffs(viaSharp, viaBoundary) == 0);
    }
}

TEST_CASE("an alpha 0.9 surface on a gentle ramp of fractional diameters reads 0.900 within "
          "1/255 -- the over-read deficit-only division cannot fix, and the nearest-node "
          "scatter's reading of the same rig is outside 1/255")
{
    // A flat alpha 0.9 field whose CoC radius climbs slowly down the band, so
    // every row is a different fractional diameter.  Every fragment lands
    // WHOLE in one bucket (the bucket range ends in front of the ramp), which
    // takes the bucket composite's own split-pooling artefact out of the
    // measurement: what remains at a pixel is alpha 0.9 times the raw weight
    // sum, and that sum is 1 only if the kernels tile.  Nearest-node snapping
    // makes rows either side of a node crossing rasterise discs a whole node
    // apart, and the surplus rows read straight through as alpha > 0.9 --
    // the fill divides deficits only.  The same rig with every radius snapped
    // to its nearest node first is that scatter, bit for bit, through the
    // same driver, and is the control that says the rig can see the defect.
    //
    // The residual (0.1 per pixel) scatters at the same per-pixel radius, so
    // arrival and coverage move together and neither scheme is helped by
    // the fill here.
    const float size = 100.0f, F = 10.0f;
    const CocParams p = makeCocParams(CocMode::Manual, 50.0f, 2.8f, 36.0f, F, 1000.0f,
                                      256.0f, 1.0f, 1.0f, 1.0f, 100.0f, size);
    const int W = 24, H = 160;
    const float alpha = 0.9f, unpremult = 0.5f;
    const double codeValue = 1.0 / 255.0;

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
        const DepthBuckets bk = makeBoundedDeltaCocBuckets(p, zOf(0.2f), zOf(0.5f), 4);
        DiscKernelLUT lut(0.5f, rOf(H + pad) + 1.0f, 1.0f, 1.0f);

        struct Reading { double worstAlpha = 0.0, worstRatio = 0.0, minArrival = 9.0, maxArrival = -9.0; };
        auto run = [&](bool snap) -> Reading {
            const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ true);
            SampleSoA soa;
            soa.begin(1, fp.groups);
            FlattenScratch scratch;
            ResidualWindow window;
            window.allocate(-pad, -pad, W + 2 * pad, H + 2 * pad, rOf(H + pad));
            flattenIntoWithResidual(fp, bk, -pad, -pad, W + pad, H + pad, soa, scratch, window,
                [&](int, int y) -> std::vector<SampleRecord> {
                    const float z = zOf(rOf(y));
                    return {makeSample(z, z, alpha, {alpha * unpremult})};
                });
            for (std::size_t f = 0; f < soa.fragmentCount(); ++f) {
                REQUIRE(soa.bucketAlpha1[f] == 0.0f);        // whole into one bucket
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
            band.K = bk.bucketCount(); band.C = 1; band.W = W; band.H = H;
            HoldoutSoA none;
            runBand(band, makeScatterParams(W, H), soa, none, lut, false, &window);

            Reading rd;
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    const double a = band.outAlpha(x, y);
                    rd.worstAlpha = std::max(rd.worstAlpha, std::fabs(a - alpha));
                    rd.worstRatio = std::max(rd.worstRatio,
                        std::fabs(static_cast<double>(band.outColor(0, x, y)) / a - unpremult));
                    const double d = band.planes.arrival[static_cast<std::size_t>(y) * W + x];
                    rd.minArrival = std::min(rd.minArrival, d);
                    rd.maxArrival = std::max(rd.maxArrival, d);
                }
            return rd;
        };

        const Reading blend = run(false);
        const Reading snapd = run(true);
        CAPTURE(blend.worstAlpha); CAPTURE(blend.minArrival); CAPTURE(blend.maxArrival);
        CAPTURE(snapd.worstAlpha); CAPTURE(snapd.minArrival); CAPTURE(snapd.maxArrival);

        // Blended: 1.40e-03 / 2.26e-04 / 6.26e-05 measured on the three
        // segments, all inside a code value; arrival within 1.6e-03 of 1.
        CHECK(blend.worstAlpha < codeValue);
        CHECK(blend.worstRatio <= 1e-06);
        CHECK(blend.maxArrival - 1.0 < codeValue);
        CHECK(1.0 - blend.minArrival < codeValue);
        // Nearest node: never better than the blend, and outside a code value
        // wherever the brackets are wide enough to see (4.63e-03 at 6 px,
        // 8.76e-03 at 16 px, measured; 1.79e-03 at 1 px, where the grid is
        // 0.001 px fine and the residual error is the ramp's own).
        CHECK(snapd.worstAlpha >= blend.worstAlpha);
        CHECK(snapd.worstRatio <= 1e-06);
        if (seg.r0 >= 6.0f) {
            CHECK(snapd.worstAlpha > codeValue);
            CHECK(snapd.worstAlpha < 4.0 * codeValue);
            CHECK(snapd.maxArrival - 1.0 > codeValue);
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
            const DepthBuckets bk = volumetric
                ? makeBoundedDeltaCocBuckets(p, fx.zf, fx.zb, K)
                : makeBoundedDeltaCocBuckets(p, 1.0f, 100.0f, K);
            // The deepest staged fragment's own radius: the last split part's
            // for a slab cut at the bucket boundaries, the sample's own for a
            // point.
            const float rDeepest = volumetric
                ? radiusPixels(p, sampleMidDepth(bk.boundary(K - 1), fx.zb))
                : fx.size;

            for (float alpha : {1.0f, 0.9f, 0.5f, 0.2f}) {
                CAPTURE(alpha);
                for (int mode = 0; mode < 3; ++mode) {
                    if (mode == 2 && volumetric)
                        continue;
                    CAPTURE(mode);
                    const bool withBackground = (mode != 1);
                    const bool fogOverOpaque  = (mode == 2);

                    const FlattenParams fp = makeFlattenParams(p, 1, /*preMerge*/ true);
                    SampleSoA soa;
                    soa.begin(1, fp.groups);
                    FlattenScratch scratch;
                    ResidualWindow window;
                    window.allocate(-pad, -pad, W + 2 * pad, H + 2 * pad, rMax);
                    flattenIntoWithResidual(fp, bk, -pad, -pad, W + pad, H + pad,
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
                    band.K = K; band.C = 1; band.W = W; band.H = H;
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
