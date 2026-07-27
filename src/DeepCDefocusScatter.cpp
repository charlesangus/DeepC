// SPDX-License-Identifier: MIT
//
// ============================================================================
//
//  DeepCDefocusScatter — SoA flattening of deep samples (M1.P3.T1) and the
//                        band scatter core (M1.P3.T2)
//
//  See DeepCDefocusScatter.h for the API and for why nothing in this
//  translation unit may include a DDImage/NDK header.
//
//  Everything below the flatten is a LOOP DRIVER only: every per-fragment,
//  per-span and per-pixel body lives in the header marked DEEPC_HD, so M3
//  compiles those unchanged under nvcc and replaces only what is here.
//
// ============================================================================

#include "DeepCDefocusScatter.h"

#include <algorithm>
#include <cmath>

namespace deepc {

// ---------------------------------------------------------------------------
// SampleSoA
// ---------------------------------------------------------------------------

std::size_t SampleSoA::sizeBytes() const
{
    return x.sizeBytes() + y.sizeBytes()
         + radius.sizeBytes() + depth.sizeBytes() + alpha.sizeBytes()
         + bucketIndex0.sizeBytes() + bucketIndex1.sizeBytes()
         + bucketAlpha0.sizeBytes() + bucketAlpha1.sizeBytes()
         + colorScale0.sizeBytes() + colorScale1.sizeBytes()
         + boundaryIndex.sizeBytes() + boundaryFrac.sizeBytes()
         + flags.sizeBytes() + color.sizeBytes();
}

void SampleSoA::clear()
{
    x.clear();
    y.clear();
    radius.clear();
    depth.clear();
    alpha.clear();
    bucketIndex0.clear();
    bucketIndex1.clear();
    bucketAlpha0.clear();
    bucketAlpha1.clear();
    colorScale0.clear();
    colorScale1.clear();
    boundaryIndex.clear();
    boundaryFrac.clear();
    flags.clear();
    color.clear();
}

void SampleSoA::release()
{
    x.release();
    y.release();
    radius.release();
    depth.release();
    alpha.release();
    bucketIndex0.release();
    bucketIndex1.release();
    bucketAlpha0.release();
    bucketAlpha1.release();
    colorScale0.release();
    colorScale1.release();
    boundaryIndex.release();
    boundaryFrac.release();
    flags.release();
    color.release();
}

void SampleSoA::begin(int channelCountIn, const ChannelGroups& groupsIn)
{
    channelCount = (channelCountIn > 0) ? channelCountIn : 0;
    groups       = groupsIn;
    clear();
}

void SampleSoA::reserveFragments(std::size_t count)
{
    x.reserve(count);
    y.reserve(count);
    radius.reserve(count);
    depth.reserve(count);
    alpha.reserve(count);
    bucketIndex0.reserve(count);
    bucketIndex1.reserve(count);
    bucketAlpha0.reserve(count);
    bucketAlpha1.reserve(count);
    colorScale0.reserve(count);
    colorScale1.reserve(count);
    boundaryIndex.reserve(count);
    boundaryFrac.reserve(count);
    flags.reserve(count);
    color.reserve(count * static_cast<std::size_t>(channelCount));
}

void SampleSoA::appendFragment(const FragmentRecord& f, const float* __restrict__ channels)
{
    const std::size_t n = fragmentCount();
    const std::size_t next = n + 1;

    // growForAppend() is geometric, so the amortised cost of an append is a
    // handful of stores; resize() then only bumps the size.
    x.growForAppend(next);
    y.growForAppend(next);
    radius.growForAppend(next);
    depth.growForAppend(next);
    alpha.growForAppend(next);
    bucketIndex0.growForAppend(next);
    bucketIndex1.growForAppend(next);
    bucketAlpha0.growForAppend(next);
    bucketAlpha1.growForAppend(next);
    colorScale0.growForAppend(next);
    colorScale1.growForAppend(next);
    boundaryIndex.growForAppend(next);
    boundaryFrac.growForAppend(next);
    flags.growForAppend(next);
    color.growForAppend(next * static_cast<std::size_t>(channelCount));

    x.resize(next);
    y.resize(next);
    radius.resize(next);
    depth.resize(next);
    alpha.resize(next);
    bucketIndex0.resize(next);
    bucketIndex1.resize(next);
    bucketAlpha0.resize(next);
    bucketAlpha1.resize(next);
    colorScale0.resize(next);
    colorScale1.resize(next);
    boundaryIndex.resize(next);
    boundaryFrac.resize(next);
    flags.resize(next);
    color.resize(next * static_cast<std::size_t>(channelCount));

    x[n]             = static_cast<std::int32_t>(f.x);
    y[n]             = static_cast<std::int32_t>(f.y);
    radius[n]        = f.radius;
    depth[n]         = f.depth;
    alpha[n]         = f.alpha;
    bucketIndex0[n]  = static_cast<std::int32_t>(f.deposit.index0);
    bucketIndex1[n]  = static_cast<std::int32_t>(f.deposit.index1);
    bucketAlpha0[n]  = f.deposit.alpha0;
    bucketAlpha1[n]  = f.deposit.alpha1;
    colorScale0[n]   = f.deposit.colorScale0;
    colorScale1[n]   = f.deposit.colorScale1;
    boundaryIndex[n] = static_cast<std::int32_t>(f.boundary.index);
    boundaryFrac[n]  = f.boundary.frac;
    flags[n]         = packFragmentFlags(f.kind, f.coverageHead);

    if (channelCount > 0) {
        float* __restrict__ dst = colorOf(n);
        for (int c = 0; c < channelCount; ++c)
            dst[c] = channels[c];
    }
}

// ---------------------------------------------------------------------------
// applyProxyScale
// ---------------------------------------------------------------------------

void applyProxyScale(CocParams& p, float proxyScale)
{
    if (!(proxyScale > 0.0f) || !std::isfinite(proxyScale) || proxyScale == 1.0f)
        return;

    p._maxRadiusPx *= proxyScale;
    p._size        *= proxyScale;   // Manual mode's radius-at-infinity, in px
    p.recomputeDerived();
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
namespace {

// Only NON-FINITE depths are rewritten here, and they must be: a NaN in the
// list makes tidyOverlapping()'s and our own std::sort comparators a
// non-strict-weak ordering, which is undefined behaviour long before the value
// could reach the CoC math.
//
//   NaN, -inf -> 0.0f            (reads as "invalid depth" everywhere:
//                                 signedCocPixels() answers 0, i.e. sharp, and
//                                 bucketOf()/locateBoundary() clamp onto the
//                                 first bucket)
//   +inf      -> kMaxDepth       (a real far-field sample; keeping it finite
//                                 avoids inf-inf in span arithmetic while the
//                                 inverse-depth CoC form treats 1e12 as the
//                                 far-field limit anyway)
//
// Finite non-positive depths are deliberately left alone: signedCocPixels()
// already specifies d <= 0 -> radius 0, and rewriting them would turn a
// behind-camera sample into a near-field one clamped to max_radius.
inline float sanitizeSampleDepth(float v)
{
    if (std::isfinite(v))
        return v;
    return (v > 0.0f) ? DepthBuckets::kMaxDepth : 0.0f;
}

// The containing bucket, used as the pre-merge grouping key so a merge can
// never span a bucket boundary (which would change which plane the fragment
// lands in, and therefore the composite).
inline int containingBucket(const DepthBuckets& buckets, float depth)
{
    if (buckets.bucketCount() <= 0)
        return 0;
    return clampi(buckets.locateBoundary(depth).index, 0, buckets.bucketCount() - 1);
}

// -----------------------------------------------------------------------
// emitFragment — THE COMPOSITION CONTRACT, enforced at its single site
//
// This is the only place in the node that turns a depth into a bucket
// assignment, and it has exactly one if/else:
//
//   Volumetric (the piece came out of splitSpanAtBoundaries())
//        -> bucketOfContaining(): whole weight, frac 0, no second bucket
//   Point      (never span-split)
//        -> bucketOf(): the fractional two-bucket partition of unity
//
// There is no path through this function that applies both, and the caller
// derives `kind` once from `zBack > zFront` rather than writing it separately
// per branch — so the +8.3% double-count regression (DeepCDefocusMath.h,
// splitSpanAtBoundaries()'s composition contract) cannot be reached by a change
// to the assignment logic alone.  It CAN still be reached by mislabelling a
// fragment, and checkCompositionContract() cannot see that, because it audits
// against the same `kind` this branch reads.  See FragmentKind in the header.
//
// Note that fragmentDeposit() serves both branches unchanged: for frac == 0 it
// produces (index0, alpha 0, colorScale 0) as the second deposit, so the
// scatter can deposit twice unconditionally and stay in bounds.
// -----------------------------------------------------------------------
inline void emitFragment(const FlattenParams& params,
                         const DepthBuckets&  buckets,
                         int                  x,
                         int                  y,
                         float                zFront,
                         float                zBack,
                         float                alpha,
                         FragmentKind         kind,
                         bool                 coverageHead,
                         const float* __restrict__ channels,
                         SampleSoA&           out,
                         FlattenStats*        stats)
{
    const float depth = sampleMidDepth(zFront, zBack);

    FragmentRecord f;
    f.x      = x;
    f.y      = y;
    f.depth  = depth;
    f.radius = radiusPixels(params.coc, depth);
    f.alpha  = clampf(alpha, 0.0f, 1.0f);
    f.kind   = kind;
    f.coverageHead = coverageHead;

    const BucketWeight bw = (kind == FragmentKind::Volumetric)
                          ? buckets.bucketOfContaining(depth)
                          : buckets.bucketOf(depth);

    f.deposit  = fragmentDeposit(bw, f.alpha);
    f.boundary = buckets.locateBoundary(depth);

    out.appendFragment(f, channels);

    if (stats != nullptr)
        ++stats->emittedFragments;
}

// Staging slot allocator: grows the scratch vector but never shrinks it, so
// each slot's channel vector keeps its capacity across pixels and the
// per-pixel path allocates nothing once warmed up.
inline FlattenScratch::Staged& nextStaged(FlattenScratch& scratch)
{
    if (scratch.stagedCount >= scratch.staged.size())
        scratch.staged.resize(scratch.stagedCount + 1);
    return scratch.staged[scratch.stagedCount++];
}

} // namespace

// ---------------------------------------------------------------------------
// flattenPixelToSoA
// ---------------------------------------------------------------------------

void flattenPixelToSoA(const FlattenParams& params,
                       const DepthBuckets&  buckets,
                       int                  x,
                       int                  y,
                       std::vector<SampleRecord>& samples,
                       FlattenScratch&      scratch,
                       SampleSoA&           out,
                       FlattenStats*        stats)
{
    // The SoA's own channel count is authoritative, NOT params.channelCount.
    // appendFragment() copies exactly out.channelCount floats out of the
    // staging buffer, so sizing the staging buffers from params instead would
    // read past the end of them whenever a caller's FlattenParams and its
    // SampleSoA::begin() disagree — verified as a heap-buffer-overflow under
    // ASAN at params=2 / SoA=8.  Sizing from the SoA makes the mismatch a
    // (harmless) dropped-channel instead of undefined behaviour.  The two
    // should of course be set from the same place; this is the safety net.
    const int nChan = (out.channelCount > 0) ? out.channelCount : 0;

    if (samples.empty())
        return;

    if (stats != nullptr) {
        ++stats->pixels;
        stats->inputSamples += samples.size();
        if (samples.size() > stats->maxSamplesInPixel)
            stats->maxSamplesInPixel = samples.size();
    }

    // --- 1/2. sanitise, and optionally convert ray distance to Z ------------
    // The ray-distance correction is per PIXEL (it depends on the pixel's
    // radial filmback offset) but applies to every sample's endpoints, so it
    // is folded into the same pass.  It scales both endpoints by one positive
    // factor, so it is monotone and cannot reorder the list.
    float rayScale = 1.0f;
    if (params.depthIsRayDistance) {
        const float rMm = filmbackRadiusMm(static_cast<float>(x) + 0.5f,
                                           static_cast<float>(y) + 0.5f,
                                           params.coc._formatWidthPx,
                                           params.formatHeightPx,
                                           params.coc._filmbackWidthMm,
                                           params.coc._pixelAspect);
        rayScale = rayDistanceToZ(1.0f, params.coc._focalLengthMm, rMm);
        if (!(rayScale > 0.0f) || !std::isfinite(rayScale))
            rayScale = 1.0f;
    }

    for (std::size_t i = 0; i < samples.size(); ++i) {
        SampleRecord& s = samples[i];

        float zf = sanitizeSampleDepth(s.zFront) * rayScale;
        float zb = sanitizeSampleDepth(s.zBack) * rayScale;
        if (!(zb > zf))
            zb = zf;                    // also rejects a back-before-front span

        s.zFront = zf;
        s.zBack  = zb;
        s.alpha  = clampf(s.alpha, 0.0f, 1.0f);   // NaN -> 0
        s.channels.resize(static_cast<std::size_t>(nChan), 0.0f);
    }

    // --- 3. tidy pre-pass (always on, correctness-required) ----------------
    // Splits partially overlapping spans and merges coincident ones by the
    // OpenEXR mixture rule.  Without it, coincident same-pixel samples would be
    // ADDED by the scatter's within-bucket accumulation instead of composited,
    // and size-0 parity with DeepToImage would be unachievable.
    if (samples.size() > 1)
        tidyOverlapping(samples);

    // tidyOverlapping() only sorts when it has 2+ samples, so sort
    // unconditionally: the staging order below must be front-to-back for the
    // pre-merge's over-composite to be correct.
    std::sort(samples.begin(), samples.end(),
        [](const SampleRecord& a, const SampleRecord& b) {
            return (a.zFront != b.zFront) ? a.zFront < b.zFront
                                          : a.zBack  < b.zBack;
        });

    if (stats != nullptr)
        stats->tidiedSamples += samples.size();

    // --- 4. sample -> fragments (THE COMPOSITION CONTRACT) -----------------
    scratch.stagedCount = 0;

    // Caller-owned stack buffer for the span split: K+2 parts at the knob's
    // maximum K, ~2.6KB.  Declared once per pixel rather than per sample (same
    // stack slot either way) and never heap-allocated, per the brief.
    SpanSplitPart parts[kMaxSpanSplitParts];

    for (std::size_t i = 0; i < samples.size(); ++i) {
        const SampleRecord& s = samples[i];

        // Zero-alpha early-out.  This matches DD::Image::CompositeSamples (and
        // therefore this node's bit-exact DeepToImage parity gate): a
        // premultiplied sample with alpha 0 contributes nothing there, so
        // scattering its colour would be a visible divergence, not an ULP one.
        // The cost is that a purely emissive alpha-0 sample is invisible —
        // deliberate, and the same choice the flatten path already shipped.
        if (!(s.alpha > 0.0f))
            continue;

        // THE ONE DECISION.  `kind` is derived here, once, from the same
        // `zBack > zFront` test that selects the split — it is deliberately
        // NOT written independently in the two branches below, so that the
        // label emitFragment() branches on and the split a sample actually
        // received cannot drift apart in a later edit.
        const bool         volumetric = (s.zBack > s.zFront);
        const FragmentKind kind       = volumetric ? FragmentKind::Volumetric
                                                   : FragmentKind::Point;

        if (!volumetric) {
            // ---- POINT SAMPLE: no span split.  emitFragment() will use
            // bucketOf() + fragmentDeposit() (the fractional two-bucket
            // partition of unity).
            FlattenScratch::Staged& st = nextStaged(scratch);
            st.zFront = s.zFront;
            st.zBack  = s.zBack;
            st.alpha  = s.alpha;
            st.kind   = kind;
            // A point sample is its own parent, so it always carries its own
            // kernel coverage.
            st.coverageHead = true;
            st.channels.assign(s.channels.begin(), s.channels.end());
            st.depth  = sampleMidDepth(st.zFront, st.zBack);
            st.radius = radiusPixels(params.coc, st.depth);
            st.bucket = containingBucket(buckets, st.depth);
            continue;
        }

        // ---- VOLUMETRIC SAMPLE: split at the bucket boundaries.  Its pieces
        // are already graded across depth by their own thickness fraction, so
        // emitFragment() will use bucketOfContaining() (whole weight) — never
        // bucketOf() as well, which is the +8.3% double-count.
        const int nParts = splitSpanAtBoundaries(buckets, s.zFront, s.zBack,
                                                 s.alpha, parts, kMaxSpanSplitParts);
        if (stats != nullptr)
            stats->splitParts += static_cast<std::size_t>(nParts);

        // Parts of ONE parent are cut AT the boundaries, so they normally land
        // in distinct buckets and the scatter's additive within-bucket
        // accumulation never sees two of them at once.  That invariant has one
        // hole: a span reaching outside the measured range [depthMin, depthMax]
        // produces a head part below boundary(0) (or a tail above boundary(K))
        // whose containing bucket CLAMPS onto the first (last) in-range part's.
        // Those two would then be ADDED rather than `over`-composited — the
        // same failure mode as the double split, measured at +11.5% on alpha
        // for a [0.2, 400] span against a [1, 100] range at K=16.  The
        // pre-merge below does not rescue it: its radius tolerance (0.25px by
        // default) is far smaller than one bucket's ΔCoC step, and it is a
        // knob that can be turned off.
        //
        // So consecutive parts of one parent that share a containing bucket are
        // over-composited HERE, unconditionally.  That is exactly lossless —
        // the transmittance split's parts are built to reproduce their union
        // under front-to-back `over`, for alpha and for premultiplied colour —
        // and it restores "one parent's parts occupy distinct buckets" as an
        // invariant of the staged list rather than an assumption about the
        // measured range.
        bool haveParentPart = false;

        for (int p = 0; p < nParts; ++p) {
            const SpanSplitPart& part = parts[p];

            // A zero-thickness piece carries neither alpha nor colour.  Do NOT
            // test part.alpha here: a thin fog piece legitimately has alpha
            // underflowing to 0 while its colorScale is still non-zero.
            if (!(part.t > 0.0f))
                continue;

            const float partDepth  = sampleMidDepth(part.zFront, part.zBack);
            const int   partBucket = containingBucket(buckets, partDepth);

            if (haveParentPart) {
                FlattenScratch::Staged& prev = scratch.staged[scratch.stagedCount - 1];
                if (prev.bucket == partBucket) {
                    const float w = 1.0f - prev.alpha;
                    for (int c = 0; c < nChan; ++c)
                        prev.channels[static_cast<std::size_t>(c)] +=
                            s.channels[static_cast<std::size_t>(c)] * part.colorScale * w;
                    prev.alpha += part.alpha * w;
                    prev.zBack  = part.zBack;       // parts are consecutive
                    prev.depth  = sampleMidDepth(prev.zFront, prev.zBack);
                    prev.radius = radiusPixels(params.coc, prev.depth);
                    prev.bucket = containingBucket(buckets, prev.depth);
                    continue;
                }
            }

            FlattenScratch::Staged& st = nextStaged(scratch);
            st.zFront = part.zFront;
            st.zBack  = part.zBack;
            st.alpha  = part.alpha;
            st.kind   = kind;

            // THE COVERAGE HEAD (M1.P3.T8).  One split parent covers its
            // kernel's area ONCE, so exactly one of its parts deposits into
            // the `sum of w*vis` coverage plane, and it must be the FRONT-MOST
            // emitted part — the one the front-to-back composite visits first,
            // so that the co-located residual term has claimed area to attach
            // the remaining parts to.  `haveParentPart` is reset per parent
            // sample and is false only until the first part with t > 0 is
            // emitted, so a parent whose leading parts are zero-thickness
            // still gets exactly one head, and a part merged into `prev` above
            // inherits prev's flag rather than adding a second.
            st.coverageHead = !haveParentPart;

            // Premultiplied colour scales by alpha_piece/alpha_parent, so the
            // front-to-back over of the pieces reproduces the parent exactly.
            st.channels.resize(static_cast<std::size_t>(nChan));
            for (int c = 0; c < nChan; ++c)
                st.channels[static_cast<std::size_t>(c)] =
                    s.channels[static_cast<std::size_t>(c)] * part.colorScale;

            st.depth  = partDepth;
            st.radius = radiusPixels(params.coc, st.depth);
            st.bucket = partBucket;
            haveParentPart = true;
        }
    }

    const std::size_t staged = scratch.stagedCount;
    if (stats != nullptr)
        stats->stagedFragments += staged;
    if (staged == 0)
        return;

    // --- 5/6. pre-merge, then append -------------------------------------
    //
    // PRE-MERGE (Perf > pre_merge / merge_tolerance, default on / 0.25px).
    // Adjacent fragments are grouped and over-composited when all three hold:
    //
    //   * same FragmentKind — merging a point into a span would change which
    //     half of the composition contract the result takes;
    //   * same containing bucket — a merge across a boundary would move energy
    //     into a different accumulation plane;
    //   * |radius - radius(group start)| <= merge_tolerance.
    //
    // UNIT NOTE: the tolerance is in CoC-RADIUS PIXELS, not depth.  That is the
    // only reading under which the knob table's "0.25px, range 0-2px" and the
    // design reference's "lossless when radii are equal" are both true — a
    // depth tolerance would be 0.25 scene units (metres, by default), and
    // equality of radii would be irrelevant to it.  Merging fragments that
    // share a bucket and a kernel radius is exactly lossless for the scatter
    // (identical kernel, identical destination plane); the residual for a
    // non-zero tolerance is bounded by the tolerance in kernel radius, plus the
    // shift of the group's holdout depth to the merged span's midpoint.
    //
    // The over-composite itself is the same front-to-back
    //   acc_c += c_i * (1 - alphaAcc);  alphaAcc += alpha_i * (1 - alphaAcc)
    // as deepc::optimizeSamples()'s merge pass (DeepSampleOptimizer.h), down to
    // the early-out at full opacity and the zFront=min / zBack=max span union.
    // It is written out here rather than called because optimizeSamples() picks
    // its groups by zFront distance, and this node has to pick them by radius
    // and bucket for the reasons above; the composite arithmetic is unchanged.
    //
    // THE COVERAGE HEAD SURVIVES THE MERGE AS AN OR (M1.P3.T8), not as "the
    // group head's flag".  A group is a maximal run of ADJACENT staged
    // fragments, so it may mix parts of different parents — e.g. a rear part of
    // parent A (not a head) immediately followed by point sample B (a head), if
    // they share a bucket and a radius.  Taking the run's first flag would DROP
    // B's coverage; the OR cannot, because merging is strictly many-to-one and
    // every group emits exactly one fragment, so a head can be absorbed but
    // never duplicated.  Merging two heads into one deposit is not a loss
    // either: the members share a bucket and a kernel radius (that is the
    // grouping predicate), so they cover the SAME destination area, and the
    // coverage plane means area — the unmerged path's two deposits are the
    // over-count, not this one.  Parts of a single parent are cut AT the
    // boundaries and therefore sit in distinct buckets, so a group can never
    // contain two parts of the same parent, and the single-parent
    // reconstruction is identical with pre_merge on and off.
    const bool  merging = params.preMerge && (params.mergeTolerancePx > 0.0f);
    const float tol     = params.mergeTolerancePx;

    std::size_t i = 0;
    while (i < staged) {
        const FlattenScratch::Staged& head = scratch.staged[i];

        bool        groupHead = head.coverageHead;
        std::size_t j         = i + 1;
        if (merging) {
            while (j < staged) {
                const FlattenScratch::Staged& cand = scratch.staged[j];
                if (cand.kind != head.kind || cand.bucket != head.bucket)
                    break;
                if (!(std::fabs(cand.radius - head.radius) <= tol))
                    break;
                groupHead = groupHead || cand.coverageHead;
                ++j;
            }
        }

        if (j - i == 1) {
            emitFragment(params, buckets, x, y,
                         head.zFront, head.zBack, head.alpha, head.kind,
                         groupHead, head.channels.data(), out, stats);
            i = j;
            continue;
        }

        scratch.mergeAccum.assign(static_cast<std::size_t>(nChan), 0.0f);

        float zf       = head.zFront;
        float zb       = head.zBack;
        float alphaAcc = 0.0f;

        for (std::size_t s = i; s < j; ++s) {
            const FlattenScratch::Staged& src = scratch.staged[s];
            const float w = 1.0f - alphaAcc;
            if (w <= 0.0f)
                break;

            zf = std::min(zf, src.zFront);
            zb = std::max(zb, src.zBack);

            for (int c = 0; c < nChan; ++c)
                scratch.mergeAccum[static_cast<std::size_t>(c)] +=
                    src.channels[static_cast<std::size_t>(c)] * w;

            alphaAcc += src.alpha * w;
        }

        emitFragment(params, buckets, x, y, zf, zb, alphaAcc, head.kind,
                     groupHead, scratch.mergeAccum.data(), out, stats);
        i = j;
    }
}

// ---------------------------------------------------------------------------
// checkCompositionContract
// ---------------------------------------------------------------------------

bool checkCompositionContract(const SampleSoA& soa,
                              const DepthBuckets& buckets,
                              std::size_t* firstBadFragment)
{
    const std::size_t n = soa.fragmentCount();
    const int         k = buckets.bucketCount();
    const int         lastBucket = (k > 0) ? (k - 1) : 0;

    for (std::size_t i = 0; i < n; ++i) {
        bool ok = true;

        const float a  = soa.alpha[i];
        const float a0 = soa.bucketAlpha0[i];
        const float a1 = soa.bucketAlpha1[i];
        const float s0 = soa.colorScale0[i];
        const float s1 = soa.colorScale1[i];

        ok = ok && std::isfinite(soa.radius[i]) && soa.radius[i] >= 0.0f;
        ok = ok && std::isfinite(soa.depth[i]);
        ok = ok && std::isfinite(a) && a >= 0.0f && a <= 1.0f;
        ok = ok && std::isfinite(a0) && a0 >= 0.0f && a0 <= 1.0f;
        ok = ok && std::isfinite(a1) && a1 >= 0.0f && a1 <= 1.0f;
        ok = ok && std::isfinite(s0) && s0 >= 0.0f && s0 <= 1.0f;
        ok = ok && std::isfinite(s1) && s1 >= 0.0f && s1 <= 1.0f;

        const int i0 = static_cast<int>(soa.bucketIndex0[i]);
        const int i1 = static_cast<int>(soa.bucketIndex1[i]);
        const int ib = static_cast<int>(soa.boundaryIndex[i]);
        ok = ok && i0 >= 0 && i0 <= lastBucket;
        ok = ok && i1 >= 0 && i1 <= lastBucket;
        ok = ok && (i1 == i0 || i1 == i0 + 1);
        ok = ok && ib >= 0 && ib <= lastBucket;
        ok = ok && std::isfinite(soa.boundaryFrac[i])
                && soa.boundaryFrac[i] >= 0.0f && soa.boundaryFrac[i] <= 1.0f;

        // The contract itself: a span-split piece must carry NO fractional
        // spill into a second bucket.  Both splits applied to one sample is
        // the measured +8.3% double-count.
        // `flags` is packed (kind in bit 0, coverage head in bit 1): read it
        // through the accessor, never by casting the whole byte.
        if (fragmentKindOf(soa.flags[i]) == FragmentKind::Volumetric) {
            ok = ok && (i1 == i0);
            ok = ok && (a1 == 0.0f);
            ok = ok && (s1 == 0.0f);
        }

        // The deposits must reconstruct the fragment's own alpha under `over`.
        const float reconstructed = 1.0f - (1.0f - a0) * (1.0f - a1);
        ok = ok && (std::fabs(reconstructed - a) <= 1e-5f);

        if (!ok) {
            if (firstBadFragment != nullptr)
                *firstBadFragment = i;
            return false;
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// BucketPlanes
// ---------------------------------------------------------------------------

void BucketPlanes::allocate(int bucketCountIn, int channelCountIn,
                            int widthIn, int heightIn)
{
    bucketCount  = (bucketCountIn  > 0) ? bucketCountIn  : 0;
    channelCount = (channelCountIn > 0) ? channelCountIn : 0;
    width        = (widthIn  > 0) ? widthIn  : 0;
    height       = (heightIn > 0) ? heightIn : 0;
    pixelCount   = static_cast<std::ptrdiff_t>(width) * height;

    const std::size_t px    = static_cast<std::size_t>(pixelCount);
    const std::size_t k     = static_cast<std::size_t>(bucketCount);
    const std::size_t plane = k * px;

    color.assign(plane * static_cast<std::size_t>(channelCount), 0.0f);
    alpha.assign(plane, 0.0f);
    weight.assign(plane, 0.0f);
}

void BucketPlanes::zero()
{
    // assign() is resizeUninitialized + fill, and the sizes are unchanged, so
    // this is a pure fill: the band loop must not re-malloc per band.
    color.assign(color.size(), 0.0f);
    alpha.assign(alpha.size(), 0.0f);
    weight.assign(weight.size(), 0.0f);
}

void BucketPlanes::release()
{
    color.release();
    alpha.release();
    weight.release();
    bucketCount  = 0;
    channelCount = 0;
    width        = 0;
    height       = 0;
    pixelCount   = 0;
}

std::size_t BucketPlanes::sizeBytes() const
{
    return color.sizeBytes() + alpha.sizeBytes() + weight.sizeBytes();
}

BucketPlaneView BucketPlanes::view()
{
    BucketPlaneView v;
    v.color        = color.data();
    v.alpha        = alpha.data();
    v.weight       = weight.data();
    v.bucketCount  = bucketCount;
    v.channelCount = channelCount;
    v.width        = width;
    v.height       = height;
    v.pixelCount   = pixelCount;
    return v;
}

// ---------------------------------------------------------------------------
// scatterBandCPU
// ---------------------------------------------------------------------------

void scatterBandCPU(const ScatterParams& params,
                    const SampleSoA&     samples,
                    const HoldoutSoA&    holdout,
                    const KernelSampler& kernel,
                    BucketPlanes&        planes,
                    ScatterScratch&      scratch,
                    ScatterStats*        stats)
{
    BucketPlaneView view = planes.view();
    if (!view.valid())
        return;

    // The band geometry is the planes', not the params': the planes are what
    // gets written, so a disagreement must not be resolvable in favour of the
    // side that does not own the memory.  Only the ORIGIN comes from params.
    if (view.width != params.bandWidth || view.height != params.bandHeight)
        return;

    // See the header: the smaller of the two counts, so neither side is read
    // or written past its end.
    const int nChan = (samples.channelCount < view.channelCount)
                    ? samples.channelCount : view.channelCount;

    const std::size_t fragmentCount = samples.fragmentCount();
    if (fragmentCount == 0)
        return;

    // Channel groups: v1 always has exactly one, covering every channel with
    // radiusScale 1.0.  The loop below exists because the group array is the
    // M2 chromatic-aberration seam and a per-group kernel radius is the whole
    // point of it; with one group it is a single iteration and costs nothing.
    ChannelGroups groups = samples.groups;
    if (groups.groupCount <= 0)
        groups = makeSingleChannelGroup(nChan);

    const int groupCount = (groups.groupCount < ChannelGroups::kMaxGroups)
                         ? groups.groupCount : ChannelGroups::kMaxGroups;

    // Soft knob ranges: clamp at the use site, never trust the raw value.
    const float sharpRadius = clampf(params.sharpRadiusPx, 0.0f, 1e6f);

    // A holdout LUT that does not cover the whole band is treated as absent
    // rather than read out of bounds: the seam's contract is one boundary row
    // per band pixel (see HoldoutSoA), and "no holdout" is always safe.
    const bool useHoldout = holdout.enabled()
                         && holdout.pixelCount >= view.pixelCount;

    // One sanitised view is what the per-fragment bodies see, so their
    // enabled() test and this driver's rowScratch decision can never disagree.
    const HoldoutSoA vis = useHoldout ? holdout : HoldoutSoA{};

    for (std::size_t f = 0; f < fragmentCount; ++f) {
        if (stats != nullptr)
            ++stats->fragments;

        const int bucket0 = static_cast<int>(samples.bucketIndex0[f]);
        const int bucket1 = static_cast<int>(samples.bucketIndex1[f]);
        if (bucket0 < 0 || bucket0 >= view.bucketCount) {
            if (stats != nullptr)
                ++stats->culled;
            continue;                   // corrupt assignment: never write OOB
        }

        ScatterFragment frag;
        frag.destX = static_cast<int>(samples.x[f]) - params.bandX;
        frag.destY = static_cast<int>(samples.y[f]) - params.bandY;

        // THE COMPOSITION CONTRACT IS HONOURED, NOT RE-DECIDED.  The flatten
        // already chose bucketOf() (point) or bucketOfContaining() (span
        // split) per fragment; this reads its labels straight through.  A
        // second assignment here would be the measured +8.29% double-count.
        frag.bucket0     = bucket0;
        frag.bucket1     = (bucket1 >= 0 && bucket1 < view.bucketCount) ? bucket1 : bucket0;
        frag.alpha0      = samples.bucketAlpha0[f];
        frag.alpha1      = samples.bucketAlpha1[f];
        frag.colorScale0 = samples.colorScale0[f];
        frag.colorScale1 = samples.colorScale1[f];

        // locateBoundary()'s pair, NOT bucketOf()'s — see HoldoutSoA.
        frag.boundaryIndex = static_cast<int>(samples.boundaryIndex[f]);
        frag.boundaryFrac  = samples.boundaryFrac[f];

        frag.color = samples.colorOf(f);

        // Does this fragment own its parent sample's kernel coverage?  Point
        // samples always do; a split volumetric parent's front-most part does
        // and its remaining parts do not (M1.P3.T8).  Like the composition
        // contract above, this is a LABEL THE FLATTEN ALREADY DECIDED and this
        // file only honours — the scatter has no way to tell which fragments
        // came from one parent.
        frag.coverageHead = fragmentCoverageHeadOf(samples.flags[f]);

        // Zero-alpha early-out, before any rasterisation (design reference's
        // perf mitigations).  partitionColorScale() is alpha_i/alpha, so a
        // deposit's colour scale is zero exactly when its alpha is: a fragment
        // failing this test carries nothing in either deposit.  Its COVERAGE
        // is dropped with it, which is correct rather than merely convenient —
        // the flatten already drops alpha-0 samples outright (DeepToImage
        // parity), so a fragment reaching here with no alpha is not a
        // transparent surface the coverage plane should report, it is nothing.
        const bool anyAlpha = (frag.alpha0 != 0.0f) || (frag.alpha1 != 0.0f);
        const bool anyColor = (frag.colorScale0 != 0.0f) || (frag.colorScale1 != 0.0f);
        if (!anyAlpha && !anyColor) {
            if (stats != nullptr)
                ++stats->culled;
            continue;
        }

        const float baseRadius = samples.radius[f];
        const float depth      = samples.depth[f];

        std::size_t touched = 0;

        for (int g = 0; g < groupCount; ++g) {
            int first = groups.firstChannel[g];
            int cnt   = groups.channelCount[g];
            if (first < 0) {
                cnt += first;
                first = 0;
            }
            if (first + cnt > nChan)
                cnt = nChan - first;

            frag.firstChannel  = first;
            frag.groupChannels = (cnt > 0) ? cnt : 0;

            // The alpha and coverage planes are NOT per channel group: they
            // must be deposited exactly once per fragment or a multi-group
            // (M2) build would count them once per group.  Group 0 carries
            // them.  M2 NOTE: with radiusScale != 1 that ties alpha to group
            // 0's kernel radius, which is a real decision M2 has to make
            // (probably "alpha follows the base/green group"); in v1 every
            // scale is 1.0, so group 0's kernel IS the base kernel and the
            // choice is not observable.  Within group 0 the COVERAGE plane is
            // written by the fragment's FIRST deposit only — see
            // scatterSpanBothBuckets(), which is where that distinction lives.
            frag.depositCoverage = (g == 0);

            if (frag.groupChannels <= 0 && !frag.depositCoverage)
                continue;

            const float radius = groupRadius(groups, g, baseRadius);

            // --- sharp fast path -------------------------------------------
            if (!(radius >= sharpRadius)) {     // also catches NaN -> sharp
                touched += scatterFragmentSharp(view, vis, frag);
                if (stats != nullptr && g == 0)
                    ++stats->sharpFragments;
                continue;
            }

            // v1's DiscKernelLUT ignores destX/destY/depth/channelGroup; they
            // are passed anyway because that unused-ness IS the M2 seam.
            const KernelView kv = kernel.kernel(radius, frag.destX, frag.destY,
                                                depth, g);
            if (!kv.valid())
                continue;

            float* rowScratch = nullptr;
            if (useHoldout) {
                // The widest span a kernel row can have, before clipping.
                scratch.ensureRow(static_cast<std::size_t>(2 * kv.radiusX + 1));
                rowScratch = scratch.rowWeights.data();
            }

            std::size_t rows = 0;
            touched += scatterFragmentSpans(view, vis, kv, frag,
                                            rowScratch, &rows);
            if (stats != nullptr)
                stats->rowSpans += rows;
        }

        if (stats != nullptr) {
            stats->pixelDeposits += touched;
            if (touched == 0)
                ++stats->culled;
        }
    }
}

// ---------------------------------------------------------------------------
// resolveBandCPU
// ---------------------------------------------------------------------------

void resolveBandCPU(const ScatterParams& params,
                    BucketPlanes&        planes,
                    float* __restrict__  outColor,
                    float* __restrict__  outAlpha)
{
    BucketPlaneView view = planes.view();
    if (!view.valid() || outAlpha == nullptr)
        return;
    if (view.channelCount > 0 && outColor == nullptr)
        return;

    // ALWAYS, on the normal path.  Not a knob, not a debug switch: within-
    // bucket additive accumulation over-counts same-pixel fragments for
    // ordinary fog (+33.3% / +71.4% / +113.3% of alpha at 2 / 3 / 4 disjoint
    // spans sharing a bucket), so this is a correctness pass.  It only ever
    // scales DOWN.
    saturateBucketPlanes(view.color, view.alpha,
                         view.bucketCount, view.channelCount, view.pixelCount);

    switch (params.combine) {
    case BucketCombine::FrontToBackOver:
        compositeBucketsFrontToBack(view.color, view.alpha,
                                    view.bucketCount, view.channelCount,
                                    view.pixelCount, outColor, outAlpha);
        return;

    case BucketCombine::CoveragePartition:
        for (std::ptrdiff_t i = 0; i < view.pixelCount; ++i) {
            compositePixelCoveragePartition(view.color + i,
                                            view.alpha + i,
                                            view.weight + i,
                                            view.bucketCount,
                                            view.channelCount,
                                            view.pixelCount,
                                            outColor + i,
                                            outAlpha + i);
        }
        return;

    default:
        // Unreachable; the node's switches all carry a default + a trailing
        // return by standing convention.  It routes to the same rule as
        // ScatterParams' own default, so a garbage enum value renders what an
        // unset one would rather than silently switching candidates — and
        // NOT to plain `over`, which is the candidate measured (M1.P3.T2
        // review) to deposit up to +94% too much alpha for a fragment split
        // across two buckets at partial kernel coverage.
        for (std::ptrdiff_t i = 0; i < view.pixelCount; ++i) {
            compositePixelCoveragePartition(view.color + i,
                                            view.alpha + i,
                                            view.weight + i,
                                            view.bucketCount,
                                            view.channelCount,
                                            view.pixelCount,
                                            outColor + i,
                                            outAlpha + i);
        }
        return;
    }
}

} // namespace deepc
