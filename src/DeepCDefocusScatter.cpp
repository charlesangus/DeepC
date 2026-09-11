// SPDX-License-Identifier: MIT
//
// ============================================================================
//
//  DeepCDefocusScatter — SoA flattening of deep samples and the band scatter
//                        core
//
//  See DeepCDefocusScatter.h for the API and for why nothing in this
//  translation unit may include a DDImage/NDK header.
//
//  Everything below the flatten is a LOOP DRIVER only: every per-fragment,
//  per-span and per-pixel body lives in the header marked DEEPC_HD, so a .cu
//  translation unit compiles those unchanged under nvcc and replaces only what
//  is here.
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
         + arrivalShare.sizeBytes()
         + bucketIndex0.sizeBytes() + bucketIndex1.sizeBytes()
         + bucketAlpha0.sizeBytes() + bucketAlpha1.sizeBytes()
         + colorScale0.sizeBytes() + colorScale1.sizeBytes()
         + flags.sizeBytes() + color.sizeBytes();
}

void SampleSoA::clear()
{
    x.clear();
    y.clear();
    radius.clear();
    depth.clear();
    alpha.clear();
    arrivalShare.clear();
    bucketIndex0.clear();
    bucketIndex1.clear();
    bucketAlpha0.clear();
    bucketAlpha1.clear();
    colorScale0.clear();
    colorScale1.clear();
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
    arrivalShare.release();
    bucketIndex0.release();
    bucketIndex1.release();
    bucketAlpha0.release();
    bucketAlpha1.release();
    colorScale0.release();
    colorScale1.release();
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
    arrivalShare.reserve(count);
    bucketIndex0.reserve(count);
    bucketIndex1.reserve(count);
    bucketAlpha0.reserve(count);
    bucketAlpha1.reserve(count);
    colorScale0.reserve(count);
    colorScale1.reserve(count);
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
    arrivalShare.growForAppend(next);
    bucketIndex0.growForAppend(next);
    bucketIndex1.growForAppend(next);
    bucketAlpha0.growForAppend(next);
    bucketAlpha1.growForAppend(next);
    colorScale0.growForAppend(next);
    colorScale1.growForAppend(next);
    flags.growForAppend(next);
    color.growForAppend(next * static_cast<std::size_t>(channelCount));

    x.resize(next);
    y.resize(next);
    radius.resize(next);
    depth.resize(next);
    alpha.resize(next);
    arrivalShare.resize(next);
    bucketIndex0.resize(next);
    bucketIndex1.resize(next);
    bucketAlpha0.resize(next);
    bucketAlpha1.resize(next);
    colorScale0.resize(next);
    colorScale1.resize(next);
    flags.resize(next);
    color.resize(next * static_cast<std::size_t>(channelCount));

    x[n]             = static_cast<std::int32_t>(f.x);
    y[n]             = static_cast<std::int32_t>(f.y);
    radius[n]        = f.radius;
    depth[n]         = f.depth;
    alpha[n]         = f.alpha;
    arrivalShare[n]  = f.share;
    bucketIndex0[n]  = static_cast<std::int32_t>(f.deposit.index0);
    bucketIndex1[n]  = static_cast<std::int32_t>(f.deposit.index1);
    bucketAlpha0[n]  = f.deposit.alpha0;
    bucketAlpha1[n]  = f.deposit.alpha1;
    colorScale0[n]   = f.deposit.colorScale0;
    colorScale1[n]   = f.deposit.colorScale1;
    flags[n]         = packFragmentFlags(f.kind, f.coverageHead,
                                        f.depositArea0, f.depositArea1);

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
//
// THE RULE ITSELF LIVES IN THE HEADER (sanitizeFragmentDepth), because the
// node's depth-range pass has to apply exactly this one — a range measured
// under a different rule makes the two passes disagree about what a depth
// means.  This is a forwarder, so the call sites below stay readable.
inline float sanitizeSampleDepth(float v)
{
    return sanitizeFragmentDepth(v);
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
// assignBucket — THE COMPOSITION CONTRACT, enforced at its single site
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
inline BucketWeight assignBucket(const DepthBuckets& buckets,
                                 FragmentKind        kind,
                                 float               depth)
{
    return (kind == FragmentKind::Volumetric) ? buckets.bucketOfContaining(depth)
                                              : buckets.bucketOf(depth);
}

// -----------------------------------------------------------------------
// PendingGroup — one about-to-be-emitted fragment, held back by one step
//
// The flatten emits fragments through a one-slot delay so that the
// deposit-collision pass (see flattenPixelToSoA()) can decide
// whether the NEXT group lands in a bucket this one already occupies before
// this one is committed to the SoA.
//
// THE DEPTH-DERIVED FIELDS ARE THE GROUP'S FIRST MEMBER'S AND NEVER MOVE.
// Absorbing a member updates alpha, colour and the coverage-head flag; it does
// NOT re-derive depth, radius or the bucket assignment.  That is deliberate and
// is what makes the pass bounded:
//
//   * the merged fragment renders exactly as its front-most member would have,
//     which is the member the composite visits first and the one the others sit
//     behind — the same "front-most owns it" convention the coverage head uses;
//   * every absorb is tested against the SAME anchor, so a long chain cannot
//     walk the group's radius, bucket or holdout bracket away from where it
//     started one small step at a time;
//   * re-deriving from the union span is actively wrong near focus, where
//     radius is V-shaped: two members at 0.60px and 0.49px on opposite sides of
//     the focal plane have a union midpoint sitting ON the focal plane, i.e. a
//     re-derived radius of 0, so the group would render sharp.  (Measured: that
//     is what re-deriving did to validation scene (l)'s ramp.)
//
// The pre-merge's own group (Perf > pre_merge) takes its depth from the union
// midpoint instead; a pixel with no collisions never reaches this delay slot
// at all.
// -----------------------------------------------------------------------
struct PendingGroup {
    bool         valid          = false;
    float        alpha          = 0.0f;
    FragmentKind kind           = FragmentKind::Point;
    bool         coverageHead   = true;

    float        depth          = 0.0f;
    float        radius         = 0.0f;
    BucketWeight bw             = {};
    int          holdoutBracket = 0;

    // See FragmentRecord::share.  A sum of member shares, never rescaled by
    // the collision merge's attenuation below.
    float        share          = 0.0f;
};

// The holdout LUT's bracket index for a depth, or 0 when no holdout is
// connected (in which case nothing reads it).  The boundary set is
// makeUniformHoldoutBoundaries()'s — the same one HoldoutLut::build() is
// handed — cached in the scratch on the three numbers it is derived from, so
// this cannot end up indexing a different array than the LUT was built at.
inline int holdoutBracketOf(const FlattenParams& params,
                            const DepthBuckets&  buckets,
                            FlattenScratch&      scratch,
                            float                depth)
{
    if (!params.holdoutConnected)
        return 0;

    if (scratch.holdoutRangeCount != buckets.boundaryCount()
        || scratch.holdoutRangeMin != buckets.depthMin()
        || scratch.holdoutRangeMax != buckets.depthMax()) {
        scratch.holdoutBoundaries = makeUniformHoldoutBoundaries(buckets);
        scratch.holdoutRangeMin   = buckets.depthMin();
        scratch.holdoutRangeMax   = buckets.depthMax();
        scratch.holdoutRangeCount = buckets.boundaryCount();
    }
    return scratch.holdoutBoundaries.locate(depth).index;
}

inline void setDepthDerived(const FlattenParams& params,
                            const DepthBuckets&  buckets,
                            FlattenScratch&      scratch,
                            PendingGroup&        g,
                            float                depth)
{
    g.depth          = depth;
    g.radius         = radiusPixels(params.coc, depth);
    g.bw             = assignBucket(buckets, g.kind, depth);
    g.holdoutBracket = holdoutBracketOf(params, buckets, scratch, depth);
}

// Do two assignments share a bucket?  Both are closed index ranges of one or
// two adjacent buckets (indexHigh() folds onto index when frac == 0), so this
// is a plain interval overlap.
inline bool depositsCollide(const BucketWeight& a, const BucketWeight& b)
{
    return !(a.indexHigh() < b.index || b.indexHigh() < a.index);
}

// ------------------------------------------------------------------------
// visitBucket / claimNewArea — ONE source pixel's bucket bookkeeping: the
//                              area claim and the per-bucket transmittance
//                              attenuation
//
// Every deposit a source pixel makes passes through here, in front-to-back
// order, and the bucket it lands in answers ONE question: has this pixel
// already put THIS KERNEL's area into that bucket?
//
//   * NO (a fresh bucket) — the deposit is the first thing in it.  It keeps its
//     alpha, it writes its `w*vis` into an area plane, and it may claim NEW
//     area if it is its parent's coverage head.  A pixel with one fragment per
//     bucket — every isolated sample, every flat field and every split parent
//     — never leaves this branch.
//
//   * YES, SAME KERNEL — the two deposits cover the IDENTICAL
//     destination area with the identical weights, so this one is BEHIND the
//     other over exactly that area: its alpha and its premultiplied colour are
//     scaled by the transmittance already accumulated there, `1 - running_k`,
//     and `running_k` takes the `over` update.  Per bucket independently, never
//     by the leading fragment's total alpha (that form — whole-FRAGMENT
//     attenuation — measures systematically under, -9.8e-02 at truth 0.963).
//     It writes NO area at all: the area is already in the plane, and counting
//     it twice is what makes the composite's C_k : D_k split read `a - a^2/4`
//     where the truth is `a` (a flat 0.25 short once the alpha saturates).
//     This is EXACT by construction, for any number of fragments:
//         1 - prod_k (1 - A_k) = 1 - prod_k prod_i (1 - a_{i,k})
//                              = 1 - prod_i (1 - a_i)
//     and it is LABEL-NEUTRAL — no FragmentKind decision is involved — which is
//     why it closes the cross-kind (Point vs span piece) collision that the
//     collision merge cannot take without mislabelling one of its members.
//
//   * YES, A DIFFERENT KERNEL — two genuinely different discs from
//     one source pixel.  They cover DIFFERENT amounts of the destination pixel,
//     which is exactly what the C_k : D_k area pair models, so this deposit
//     keeps its area but arrives as CO-LOCATED (the caller drops its head) and
//     takes no attenuation: attenuating it would apply a transmittance measured
//     over one disc to a deposit spread over another.  Without this, two opaque
//     points at one pixel at radii 23.3px and 8.5px claim the pixel's area
//     twice and band-sum to 2.000000 against the flattened truth of 1.0.
//
// NO HOLDOUT GATE, deliberately.  Unlike the collision merge, which
// emits ONE fragment at ONE depth, this changes no fragment's depth: each keeps
// its own `depth` and therefore its own `vis`.  Holdout transmittance is
// monotone in z and the attenuating fragment is in FRONT, so vis_front >=
// vis_back and a sample can never be carried from behind a card to in front of
// one.  Only the attenuation FACTOR is stale when vis_front < 1 (it uses the
// leading fragment's unoccluded alpha), which is bounded by that fragment's own
// alpha and soft.  Measured: with a holdout connected the size-0 corpus reads
// the same 2.4e-07 as with it disconnected.
//
// The state is stamped, not cleared (`claimStamp[k] == claimEpoch` means
// "touched during the current pixel"), so a pixel costs no reset.  The
// attenuation's memory cost is the THREE `run*` arrays, not one float:
// 16 B/bucket, i.e. 2 KB per thread at K=128, which is what the node's band
// budget has to size on; see FlattenScratch.
// ------------------------------------------------------------------------
enum class BucketVisit {
    Fresh,          // first deposit into this bucket at this pixel
    SameKernel,     // attenuated onto an identical earlier deposit
    OtherKernel     // a different disc already covered this bucket
};

inline void ensureBucketScratch(FlattenScratch& scratch, int bucketCount)
{
    if (scratch.claimStamp.size() < static_cast<std::size_t>(bucketCount)) {
        scratch.claimStamp.resize(static_cast<std::size_t>(bucketCount), 0u);
        scratch.claimBin.resize(static_cast<std::size_t>(bucketCount), 0);
        scratch.runStamp.resize(static_cast<std::size_t>(bucketCount), 0u);
        scratch.runBin.resize(static_cast<std::size_t>(bucketCount), 0);
        scratch.runAlpha.resize(static_cast<std::size_t>(bucketCount), 0.0f);
    }
}

// THE NEW-AREA CLAIM.  Separate from the touch record above
// because they answer different questions: this one is "has a fragment already
// claimed this bucket's AREA at this pixel, and with which kernel?", and only
// a coverage head ever claims.  A fragment's REAR deposit touches a bucket
// (so the attenuation sees it) without claiming any area in it, and treating
// that touch as a claim is a regression on a two-layer defocused field,
// because it pushes a differently-sized disc's honest new area into the
// co-located plane.  Measured at K=32 on two independent rigs, folding the two
// stamps costs of order 1e-2 to 1e-1 of alpha; read it as a direction, not as
// a number to re-measure.
//
// The claim is yielded only to a DIFFERENT kernel: two deposits sharing a
// bucket AND a kernel cover the identical area, and the attenuation above is
// what resolves those -- the area planes cannot (the C_k : D_k split
// reads `a - a^2/4` where the truth is `a`).  Across kernels the areas really
// do differ, which is exactly what the pair models: two opaque points at one
// pixel at radii 23.3px and 8.5px band-sum to 1.000000 with this and 2.000000
// without it.
inline bool claimNewArea(FlattenScratch& scratch, int bucketCount, int bucket,
                         std::int64_t kernelBin)
{
    if (bucket < 0 || bucket >= bucketCount)
        return true;                            // never index out of range

    std::uint32_t& slot = scratch.claimStamp[static_cast<std::size_t>(bucket)];
    if (slot == scratch.claimEpoch)
        return scratch.claimBin[static_cast<std::size_t>(bucket)] == kernelBin;

    slot = scratch.claimEpoch;
    scratch.claimBin[static_cast<std::size_t>(bucket)] = kernelBin;
    return true;
}

inline BucketVisit visitBucket(FlattenScratch& scratch, int bucketCount,
                               std::int64_t kernelBin, int bucket,
                               float& alpha, float& colorScale)
{
    if (bucket < 0 || bucket >= bucketCount)
        return BucketVisit::Fresh;              // never index out of range
    const std::size_t k = static_cast<std::size_t>(bucket);

    if (scratch.runStamp[k] != scratch.claimEpoch) {
        scratch.runStamp[k] = scratch.claimEpoch;
        scratch.runBin[k]   = kernelBin;
        scratch.runAlpha[k] = alpha;
        return BucketVisit::Fresh;
    }

    if (scratch.runBin[k] != kernelBin)
        return BucketVisit::OtherKernel;

    const float run = scratch.runAlpha[k];
    const float t   = 1.0f - run;
    alpha      *= t;
    colorScale *= t;
    scratch.runAlpha[k] = run + alpha;          // the `over` update
    return BucketVisit::SameKernel;
}

inline void emitPending(FlattenScratch&      scratch,
                        int                  bucketCount,
                        int                  x,
                        int                  y,
                        const PendingGroup&  g,
                        const float* __restrict__ channels,
                        SampleSoA&           out,
                        FlattenStats*        stats)
{
    FragmentRecord f;
    f.x      = x;
    f.y      = y;
    f.depth  = g.depth;
    f.radius = g.radius;
    f.alpha  = clampf(g.alpha, 0.0f, 1.0f);
    f.kind   = g.kind;
    // Carried straight through: the collision attenuation below rescales
    // alpha/colorScale ONLY, never share, or the gather-share partition would
    // stop summing to 1.
    f.share  = g.share;

    const std::int64_t kernelBin = scatterKernelBin(g.radius);

    // --- THE MONOTONE BUCKET FRONTIER ------------------------------------
    // The bucket composite is front-to-back over the PLANES: everything in
    // bucket k is attenuated by the whole of bucket k-1.  So a deposit may
    // never land in FRONT of a bucket an earlier (nearer) fragment at this
    // pixel already wrote into, or that earlier fragment's own rear deposit
    // picks up a spurious factor of this one's alpha — measured 0.879 against
    // a true 1.0 for the front layer of two alpha-0.5 samples sharing a
    // bucketOf() pair, and up to 8.1e-01 of premultiplied colour over the
    // mixed size-0 corpus.  The alpha comes out right either way (a product
    // does not care about order); it is the COLOUR that is redistributed
    // between the two layers.
    //
    // The fix is to clamp the assignment forward to the frontier — the highest
    // bucket this pixel has touched — and give the fragment whole weight
    // there.  Two properties make that cheap rather than a return of the
    // disproved whole-weight assignment:
    //
    //   * it fires ONLY on a collision.  Fragments are staged front-to-back
    //     and bucketOf()'s index is monotone in depth, so `bw.index` is already
    //     >= frontier - 1: the clamp moves a fragment by at most ONE bucket,
    //     and only when it shares its front bucket with the fragment ahead of
    //     it.  A pixel with one fragment per bucket pair never reaches it, so
    //     every isolated sample, flat field and split parent is bit-identical.
    //   * it converges with K.  Collisions get rarer as the buckets get finer,
    //     so the rule fires less and less and the K -> infinity limit is the
    //     unclamped one — the property a whole-weight assignment on the sharp
    //     path does not have.
    //
    // It moves the fragment's PLANE, never its depth or its radius: the disc
    // it rasterises and the holdout visibility it is sampled at are unchanged.
    BucketWeight bw = g.bw;
    if (bucketCount > 0 && bw.index < scratch.frontierBucket
        && kernelBin == scratch.frontierBin) {
        bw.index = (scratch.frontierBucket < bucketCount) ? scratch.frontierBucket
                                                          : (bucketCount - 1);
        bw.frac  = 0.0f;
    }
    if (bw.indexHigh() >= scratch.frontierBucket) {
        scratch.frontierBucket = bw.indexHigh();
        scratch.frontierBin    = kernelBin;
    }

    f.deposit = fragmentDeposit(bw, f.alpha);

    // --- the per-bucket claim + attenuation (see visitBucket) --------------
    ensureBucketScratch(scratch, bucketCount);

    const BucketVisit v0 = visitBucket(scratch, bucketCount, kernelBin,
                                       f.deposit.index0,
                                       f.deposit.alpha0, f.deposit.colorScale0);
    // A fragment that already carries no coverage (a split parent's non-head
    // part) must not consume the claim: it never had one to give.  A deposit
    // that was attenuated onto an identical earlier one carries no area at all,
    // so it is not a head either.
    f.coverageHead = g.coverageHead
                  && (v0 != BucketVisit::SameKernel)
                  && claimNewArea(scratch, bucketCount, f.deposit.index0, kernelBin);
    f.depositArea0 = (v0 != BucketVisit::SameKernel);

    BucketVisit v1 = BucketVisit::Fresh;
    if (f.deposit.index1 != f.deposit.index0) {
        v1 = visitBucket(scratch, bucketCount, kernelBin, f.deposit.index1,
                         f.deposit.alpha1, f.deposit.colorScale1);
        f.depositArea1 = (v1 != BucketVisit::SameKernel);
    }

    // `alpha` is the fragment's own alpha AS DEPOSITED, so that the two
    // deposits still reconstruct it under `over` (checkCompositionContract's
    // last clause) once they have been attenuated.  Recomputed ONLY when an
    // attenuation actually happened, so that a non-colliding fragment keeps
    // the exact float the deposit split gave it.
    if (v0 == BucketVisit::SameKernel || v1 == BucketVisit::SameKernel) {
        f.alpha = 1.0f - (1.0f - f.deposit.alpha0) * (1.0f - f.deposit.alpha1);
    }

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
                       FlattenStats*        stats,
                       float*               residualT,
                       float*               residualRadiusPx)
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

    if (samples.empty()) {
        // No sample to take a residual radius from; the caller's own
        // "empty pixel" default is left in place.
        if (residualT != nullptr)
            *residualT = 1.0f;
        return;
    }

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
    //
    // The factor comes from rayDepthScaleAt() rather than being recomputed
    // here, so the holdout SoA's `depthScale` and the node's depth-range pass
    // are provably the same number at the same pixel (see that function).
    const float rayScale = rayDepthScaleAt(params, x, y);

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

    // A new pixel: every bucket's area is unclaimed again.  Bumping the epoch
    // IS the reset (see FlattenScratch::claimStamp); slot 0 is never a live
    // epoch, so a freshly-resized array reads as unclaimed.
    scratch.frontierBucket = 0;
    scratch.frontierBin    = 0;
    ++scratch.claimEpoch;
    if (scratch.claimEpoch == 0u) {             // wrapped: retire the old marks
        scratch.claimStamp.assign(scratch.claimStamp.size(), 0u);
        scratch.runStamp.assign(scratch.runStamp.size(), 0u);
        ++scratch.claimEpoch;
    }

    // Caller-owned stack buffer for the span split: K+2 parts at the knob's
    // maximum K, ~2.6KB.  Declared once per pixel rather than per sample (same
    // stack slot either way) and never heap-allocated, per the brief.
    SpanSplitPart parts[kMaxSpanSplitParts];

    // THE GATHER-SHARE PARTITION.  One running transmittance for the whole
    // pixel, decremented front-to-back as every fragment (point sample or
    // split part, whichever this loop is staging) takes its slice:
    // share = t * alpha; t *= (1 - alpha).  By construction the shares of
    // every fragment staged below plus the value `arrivalT` holds once the
    // loop ends sum to exactly 1 — that final value IS the residual (the
    // virtual background's claim).  Untouched by pre-merge (which only sums
    // shares) and by the deposit-collision merge (see PendingGroup::share);
    // this is the one place the partition is computed.
    float arrivalT = 1.0f;

    for (std::size_t i = 0; i < samples.size(); ++i) {
        const SampleRecord& s = samples[i];

        // Zero-alpha early-out.  This matches DD::Image::CompositeSamples (and
        // therefore this node's bit-exact DeepToImage parity gate): a
        // premultiplied sample with alpha 0 contributes nothing there, so
        // scattering its colour would be a visible divergence, not an ULP one.
        // The cost is that a purely emissive alpha-0 sample is invisible,
        // which is deliberate and matches the flatten path.
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
            st.share  = arrivalT * s.alpha;
            arrivalT *= (1.0f - s.alpha);
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

        const std::size_t parentFirstStaged = scratch.stagedCount;
        float             parentShare       = 0.0f;

        for (int p = 0; p < nParts; ++p) {
            const SpanSplitPart& part = parts[p];

            // A zero-thickness piece carries neither alpha nor colour.  Do NOT
            // test part.alpha here: a thin fog piece legitimately has alpha
            // underflowing to 0 while its colorScale is still non-zero.
            if (!(part.t > 0.0f))
                continue;

            const float partDepth  = sampleMidDepth(part.zFront, part.zBack);
            const int   partBucket = containingBucket(buckets, partDepth);

            // Each part consumes its own slice of the pixel's running
            // transmittance, in the same front-to-back order it is staged —
            // regardless of whether it ends up folded into `prev` (the
            // same-bucket over-composite just above) or starting a fresh
            // Staged entry: either way the pixel's unit area has one fewer
            // part's worth of transmittance left after it.
            const float partShare = arrivalT * part.alpha;
            arrivalT *= (1.0f - part.alpha);
            parentShare += partShare;

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
                    prev.share += partShare;
                    continue;
                }
            }

            FlattenScratch::Staged& st = nextStaged(scratch);
            st.zFront = part.zFront;
            st.zBack  = part.zBack;
            st.alpha  = part.alpha;
            st.kind   = kind;
            st.share  = partShare;

            // THE COVERAGE HEAD.  One split parent covers its
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

        // The split is an artefact of K and must not move where the parent
        // claims arrival: spread over the parts' own radii, a wide-to-narrow
        // parent claims far less at its own pixel than the unit background
        // kernel its neighbours deposit, and the deficit division fires on it.
        if (scratch.stagedCount > parentFirstStaged) {
            for (std::size_t k = parentFirstStaged; k + 1 < scratch.stagedCount; ++k)
                scratch.staged[k].share = 0.0f;
            scratch.staged[scratch.stagedCount - 1].share = parentShare;
        }
    }

    const std::size_t staged = scratch.stagedCount;
    if (stats != nullptr)
        stats->stagedFragments += staged;
    if (staged == 0) {
        // Every sample failed the zero-alpha early-out: nothing was staged,
        // so there is no "deepest sample" to take a residual radius from —
        // same convention as the samples.empty() early-out above.
        if (residualT != nullptr)
            *residualT = 1.0f;
        return;
    }

    // The deepest staged fragment's radius, AFTER the same-bucket split-merge
    // above has folded any trailing parts into it — captured now, before
    // pre-merge/collision-merge regroup the staged list for the SoA, because
    // "deepest sample" means the last one staged front-to-back, not whatever
    // fragment a later grouping pass happens to emit last.
    const float deepestRadius = scratch.staged[staged - 1].radius;

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
    // only reading under which the knob's "0.25px, range 0-2px" and
    // "lossless when radii are equal" are both true — a
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
    // THE COVERAGE HEAD SURVIVES THE MERGE AS AN OR, not as "the
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
    //
    // --- 7. THE DEPOSIT-COLLISION MERGE -----------------------------------
    //
    // The pre-merge above groups by CONTAINING bucket, but a Point fragment
    // deposits through bucketOf(), which measures position between bucket
    // CENTRES.  Two same-pixel fragments in different containing buckets can
    // therefore share a bucketOf() index: they are never grouped, and both
    // deposit `w` of NEW AREA plus their own alpha into one plane.  The plane
    // ADDS them, the composite then clamps `cov` (2.0 -> 1.0) and `a`
    // (1.0127 -> 1.0) independently, `local = a/cov` reads 1.0, and the bucket
    // comes out fully opaque: 1.000 against a true 0.781 on two samples, up to
    // 2.4e-01 of alpha over a random corpus — the coverage plane
    // double-counted.
    //
    // THE INVARIANT THIS PASS ESTABLISHES: within one source pixel, deposits
    // that land in the same bucket AND rasterise the same kernel are
    // `over`-composited, never added — so that pixel's NEW-AREA plane can never
    // hold more of one kernel's area than that kernel actually deposited.
    //
    // It is NOT behind `pre_merge`: a knob that changed the fragment count
    // would change the rounding of every pixel it touched, and the two-way
    // `pre_merge` sweeps in the suite and the parity gates all expect the knob
    // to move nothing measurable.
    //
    // WHY THE MERGE IS KEPT EVEN THOUGH emitPending() ATTENUATES.
    // It is not load-bearing for CORRECTNESS: the per-bucket attenuation, the
    // area claim and the monotone frontier in emitPending() reproduce the
    // pixel's flatten with the merge compiled out (measured over the size-0
    // corpus at 900 pixels x K 4..128 x 2..20 spp x pre_merge both x holdout
    // both: worst |d alpha| 5.7e-07, worst |d colour| 4.6e-07, 0.00% of pixels
    // beyond 1e-3 either way).
    //
    // What it still buys is FRAGMENT COUNT, and the size of that saving depends
    // entirely on `pre_merge`, so state the knob with the number:
    //   * `pre_merge` OFF — the merge is the only thing collapsing a pixel's
    //     colliding same-kernel groups, and it is worth a lot: 10105 emitted
    //     fragments against 18399 over 900 pixels at 20 spp / size 0 (-45%),
    //     12870 against 18399 at size 6 (-30%), and ~18% of the band's scatter
    //     wall time at size 12.
    //   * `pre_merge` ON, the SHIPPING DEFAULT — the pre-merge has already taken
    //     most of those groups, and the saving collapses to 10104 against 10215
    //     at size 0 (-1.1%), 12147 against 12676 at size 6 (-4.2%) and 13692
    //     against 14382 at size 12 (-4.8%), with NO scatter wall-time difference
    //     measurable at 0.1 ms resolution.
    // So it is kept for the knob-off path, not for the default one, and the
    // gates stay unchanged — the merge must stay LOSSLESS (one kernel, one
    // holdout bracket, one FragmentKind) or it would put the error back that
    // the attenuation just removed.
    //
    // THE COST OF KEEPING IT, so it is not read as a node defect: the merge is
    // gated on the holdout bracket, so CONNECTING A NON-OCCLUDING HOLDOUT
    // regroups fragments and moves defocused pixels.  With `pre_merge` off it
    // is the only such gate, and the output goes from bitwise-identical (0 of
    // 4096 pixels) with the merge compiled out to |d alpha| up to 1.78e-01 on
    // 442 of 4096 pixels with it in.  At the default (`pre_merge` on) the
    // pre-merge's own bracket gate dominates and deleting this pass would not
    // recover the invariance.
    //
    // The attenuation alone does not replace it without the monotone frontier:
    // depositing the trailing fragment's alpha pre-attenuated into the same
    // planes makes the ALPHA exact (transmittances multiply, and order does not
    // matter to a product) but not the COLOUR, because the composite attenuates
    // a whole bucket by the whole of the bucket in front of it — the leading
    // fragment's own rear deposit then picks up a spurious factor of
    // `1 - a(trailing, front bucket)`, measured 0.879 against a true 1.0 for the
    // front layer at alpha 0.5.  That is what the monotone frontier fixes, by
    // keeping the trailing fragment out of the leading one's front bucket.
    //
    // WHY IT PRESERVES THE K KNOB — the property a whole-weight assignment
    // does not have.  This pass does not touch the ASSIGNMENT: every fragment
    // still goes through bucketOf()'s fractional two-bucket partition, so the
    // K -> infinity limit is unchanged by it.  All it
    // does is replace an addition with the exact `over` in the cases where two
    // same-pixel deposits already share a plane — and those cases get RARER as K
    // rises, so the pass fires less and less and converges to a no-op.  It can
    // only lower the error at any K, never raise it.
    //
    // THE TWO GATES ON HOW FAR IT MAY REACH, both required:
    //
    //   * ONE KERNEL (sameScatterKernel, see the header): the scatter must
    //     fetch literally the same kernel for both, so that they deposit the
    //     same weights into the same pixels and over-compositing them is
    //     exactly what a flatten of that pixel does.  Two genuinely different
    //     discs from one source pixel occlude each other along the ray BEFORE
    //     the blur, which is a known occlusion-before-blur loss and is NOT this
    //     merge's to fix — collapsing them would render the far layer at the
    //     near layer's bokeh size.  What keeps THOSE from double-claiming the
    //     pixel's area is visitBucket()'s OtherKernel branch above.
    //     The test is against the held-back group's own radius, which absorbing
    //     never moves.  It must NOT be re-derived from the union span: radius
    //     is V-shaped about the focal plane, so two members at equal radius on
    //     opposite sides of focus have a union midpoint sitting ON it, and the
    //     group renders sharp.
    //   * SAME HOLDOUT BRACKET, when a holdout is connected.  The group emits
    //     one fragment at one depth and the holdout is sampled per fragment, so
    //     an unrestricted merge could carry a sample from behind a holdout card
    //     to in front of it.  Inside one bracket the LUT has only two values
    //     anyway, so the merge adds nothing to the error already there.
    //
    // Cross-KIND merges stay forbidden for the same reason the pre-merge forbids
    // them: the merged fragment would have to pick one half of the composition
    // contract, and either choice is wrong for the other member.  A Point and a
    // span piece colliding in one bucket therefore still add.
    const bool  merging = params.preMerge && (params.mergeTolerancePx > 0.0f);
    const float tol     = params.mergeTolerancePx;

    scratch.pendingAccum.assign(static_cast<std::size_t>(nChan), 0.0f);
    PendingGroup pending;

    std::size_t i = 0;
    while (i < staged) {
        const FlattenScratch::Staged& head = scratch.staged[i];

        bool        groupHead = head.coverageHead;
        std::size_t j         = i + 1;
        if (merging) {
            // THE HOLDOUT BRACKET GATES THIS GROUP TOO.
            // The pre-merge emits ONE fragment at the group's union midpoint and
            // the holdout is sampled per fragment, so without this a group may
            // carry a sample from in front of a holdout card to behind it — and
            // the ΔCoC bucket the group is keyed on is the WRONG width for that:
            // on the default rig (K=16, focus 10, range [1,100]) the last bucket
            // is [10, 100], ninety units, against ~6.2-unit holdout brackets.
            // Measured at the DEFAULT knob settings (pre_merge on, tolerance
            // 0.25px), opaque card at z=50, samples at z=40 and z=62: the merged
            // fragment lands behind the card and the pixel reads alpha
            // 0.000000 against an exact 0.500000 — the unoccluded foreground
            // erased outright.  Same reasoning, same cache and same cost as the
            // collision pass's gate below; free when no holdout is connected.
            const int headBracket = holdoutBracketOf(params, buckets, scratch,
                                                     head.depth);
            while (j < staged) {
                const FlattenScratch::Staged& cand = scratch.staged[j];
                if (cand.kind != head.kind || cand.bucket != head.bucket)
                    break;
                if (!(std::fabs(cand.radius - head.radius) <= tol))
                    break;
                if (holdoutBracketOf(params, buckets, scratch, cand.depth)
                    != headBracket)
                    break;
                groupHead = groupHead || cand.coverageHead;
                ++j;
            }
        }

        // --- the pre-merge group's over-composite, into mergeAccum ---------
        scratch.mergeAccum.assign(static_cast<std::size_t>(nChan), 0.0f);

        PendingGroup cand;
        cand.valid        = true;
        cand.kind         = head.kind;
        cand.coverageHead = groupHead;
        cand.alpha        = 0.0f;
        cand.share        = 0.0f;

        // A pre-merge group's share is the plain sum of its members' — every
        // one of them, independent of the alpha/colour over-composite below,
        // which may stop early (`w <= 0.0f`) once the group is opaque.  Each
        // member's share was already committed at staging time, whether or
        // not the group's own alpha bookkeeping still has use for it.
        for (std::size_t s = i; s < j; ++s)
            cand.share += scratch.staged[s].share;

        float zf = head.zFront;
        float zb = head.zBack;

        for (std::size_t s = i; s < j; ++s) {
            const FlattenScratch::Staged& src = scratch.staged[s];
            const float w = 1.0f - cand.alpha;
            if (w <= 0.0f)
                break;

            zf = std::min(zf, src.zFront);
            zb = std::max(zb, src.zBack);

            for (int c = 0; c < nChan; ++c)
                scratch.mergeAccum[static_cast<std::size_t>(c)] +=
                    src.channels[static_cast<std::size_t>(c)] * w;

            cand.alpha += src.alpha * w;
        }
        // The pre-merge group's own depth is the union midpoint; the
        // collision pass below never moves it.
        setDepthDerived(params, buckets, scratch, cand, sampleMidDepth(zf, zb));
        i = j;

        // --- offer it to the held-back group -------------------------------
        // Everything is tested against the held-back group's OWN (anchor)
        // assignment, radius and bracket, which absorbing never changes — see
        // PendingGroup.  So the group cannot drift, and a rejected absorb costs
        // nothing beyond these four tests.
        const bool absorb = pending.valid
                         && cand.kind == pending.kind
                         && sameScatterKernel(pending.radius, cand.radius)
                         && pending.holdoutBracket == cand.holdoutBracket
                         && depositsCollide(pending.bw, cand.bw);

        if (absorb) {
            const float w = 1.0f - pending.alpha;
            if (w > 0.0f) {                 // else the group is already opaque
                for (int c = 0; c < nChan; ++c)
                    scratch.pendingAccum[static_cast<std::size_t>(c)] +=
                        scratch.mergeAccum[static_cast<std::size_t>(c)] * w;
                pending.alpha += cand.alpha * w;
            }
            pending.coverageHead = pending.coverageHead || cand.coverageHead;
            // Unconditional, unlike alpha/colour above: the collision merge
            // must NOT attenuate share (see FragmentRecord::share), so the
            // held-back group's share is a plain sum regardless of whether it
            // is already opaque.
            pending.share += cand.share;
        } else {
            if (pending.valid)
                emitPending(scratch, buckets.bucketCount(), x, y, pending,
                            scratch.pendingAccum.data(), out, stats);
            pending = cand;
            scratch.pendingAccum.swap(scratch.mergeAccum);
        }
    }

    if (pending.valid)
        emitPending(scratch, buckets.bucketCount(), x, y, pending,
                    scratch.pendingAccum.data(), out, stats);

    // `arrivalT` is the running transmittance after every staged fragment's
    // share was taken in step 4, above — untouched by pre-merge or the
    // collision merge, which only regroup already-committed shares, never
    // recompute the partition.  It is therefore the virtual background's
    // claim on this pixel: shares + *residualT sum to exactly 1.
    if (residualT != nullptr)
        *residualT = arrivalT;
    if (residualRadiusPx != nullptr)
        *residualRadiusPx = deepestRadius;
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
        ok = ok && i0 >= 0 && i0 <= lastBucket;
        ok = ok && i1 >= 0 && i1 <= lastBucket;
        ok = ok && (i1 == i0 || i1 == i0 + 1);
        // There is no holdout boundary pair to audit here: the holdout LUT
        // has its own boundary set and the pair is derived in the scatter from
        // `depth`, which is already checked finite above.
        // HoldoutBoundaries::locate() clamps its own output.

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
// HoldoutSampleSoA
// ---------------------------------------------------------------------------

void HoldoutSampleSoA::begin(std::ptrdiff_t pixelCountIn)
{
    pixelCount     = (pixelCountIn > 0) ? pixelCountIn : 0;
    pixelsAppended = 0;
    sampleCount    = 0;
    zFront.clear();
    zBack.clear();
    alpha.clear();

    // resizeUninitialized(), not resize(): every element from index 1 onward
    // is about to be written by appendPixel() in order, so only index 0 (the
    // CSR's fixed starting offset) needs an explicit store here.
    pixelOffset.resizeUninitialized(static_cast<std::size_t>(pixelCount) + 1);
    pixelOffset[0] = 0;
}

void HoldoutSampleSoA::reserveSamples(std::size_t count)
{
    zFront.reserve(count);
    zBack.reserve(count);
    alpha.reserve(count);
}

void HoldoutSampleSoA::appendPixel(std::vector<SampleRecord>& samples,
                                   float depthScale)
{
    // The ray-distance -> Z correction, if any.  It must be the SAME factor
    // flattenPixelToSoA() applied at this pixel (see the header): the LUT is
    // sampled at bucket boundaries that live in Z, so a holdout left in
    // ray-distance space sits systematically too far back off-axis.
    const float zScale = (depthScale > 0.0f && std::isfinite(depthScale))
                       ? depthScale : 1.0f;

    // --- sanitise, drop alpha<=0 and NaN depths, compact in place -----------
    std::size_t kept = 0;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        SampleRecord& s = samples[i];

        // A NON-FINITE FRONT DEPTH IS DROPPED HERE, unlike in the source
        // flatten.  sanitizeSampleDepth() maps NaN to 0, which is harmless on
        // the source side (signedCocPixels() gives d <= 0 radius 0 and the
        // sample still composites in its own pixel) but is catastrophic here:
        // depth 0 is in front of boundary(0), so ONE NaN in the holdout's
        // DeepFront makes an opaque sample attenuate the destination pixel at
        // every boundary — a black hole in the plate rather than a lost
        // sample.  +/-inf is a real far-field holdout and is kept (mapped to
        // kMaxDepth), exactly as on the source side; only NaN is dropped.
        const bool badDepth = std::isnan(s.zFront) || std::isnan(s.zBack);

        float zf = sanitizeSampleDepth(s.zFront) * zScale;
        float zb = sanitizeSampleDepth(s.zBack) * zScale;
        if (!(zb > zf))
            zb = zf;                 // also rejects a back-before-front span
        s.zFront = zf;
        s.zBack  = zb;
        s.alpha  = clampf(s.alpha, 0.0f, 1.0f);   // NaN -> 0

        if (s.alpha > 0.0f && !badDepth) {
            if (kept != i)
                samples[kept] = std::move(samples[i]);
            ++kept;
        }
    }
    samples.resize(kept);

    // --- sort ascending by zFront (build()'s fast-path precondition) -------
    std::sort(samples.begin(), samples.end(),
        [](const SampleRecord& a, const SampleRecord& b) {
            return a.zFront < b.zFront;
        });

    // --- append the flat SoA arrays -----------------------------------------
    const std::size_t n    = samples.size();
    const std::size_t next = sampleCount + n;

    zFront.growForAppend(next);
    zBack.growForAppend(next);
    alpha.growForAppend(next);
    zFront.resize(next);
    zBack.resize(next);
    alpha.resize(next);

    for (std::size_t i = 0; i < n; ++i) {
        zFront[sampleCount + i] = samples[i].zFront;
        zBack[sampleCount + i]  = samples[i].zBack;
        alpha[sampleCount + i]  = samples[i].alpha;
    }
    sampleCount = next;

    // --- close this pixel's CSR entry ---------------------------------------
    if (pixelsAppended >= 0 && pixelsAppended < pixelCount) {
        pixelOffset[static_cast<std::size_t>(pixelsAppended) + 1] =
            static_cast<std::int32_t>(sampleCount);
    }
    // A call past pixelCount is a caller bug (there is no pixelCount+1'th
    // offset slot to write); still counted so build()'s `filled` clamp can
    // detect and safely ignore the excess rather than write out of bounds.
    ++pixelsAppended;
}

void HoldoutSampleSoA::clear()
{
    zFront.clear();
    zBack.clear();
    alpha.clear();
    pixelOffset.clear();
    sampleCount    = 0;
    pixelsAppended = 0;
}

void HoldoutSampleSoA::release()
{
    zFront.release();
    zBack.release();
    alpha.release();
    pixelOffset.release();
    pixelCount     = 0;
    sampleCount    = 0;
    pixelsAppended = 0;
}

std::size_t HoldoutSampleSoA::sizeBytes() const
{
    return zFront.sizeBytes() + zBack.sizeBytes() + alpha.sizeBytes()
         + pixelOffset.sizeBytes();
}

// ---------------------------------------------------------------------------
// HoldoutLut
// ---------------------------------------------------------------------------

void HoldoutLut::build(const HoldoutSampleSoA& samples,
                       const HoldoutBoundaries& boundarySet)
{
    const int bCount = boundarySet.count();

    // THE ZERO-COST PATH.  Nothing to build: release rather than fill, so an
    // unconnected holdout (or a band wholly outside its bbox) costs nothing
    // beyond this one branch -- see the header for why this has to be
    // genuinely free, not a fill of 1.0.
    if (samples.sampleCount == 0 || samples.pixelCount <= 0 || bCount <= 1) {
        release();
        return;
    }

    boundaries    = boundarySet;
    boundaryCount = bCount;
    pixelCount    = samples.pixelCount;

    boundaryT.resizeUninitialized(static_cast<std::size_t>(pixelCount)
                                 * static_cast<std::size_t>(boundaryCount));

    // THE BOUNDARY DEPTHS ARE THE HOLDOUT SET'S, NOT DepthBuckets' -- see the
    // header.  They are copied into this object so view() can hand the scatter
    // the LUT and the depths it was built at together, with no second party to
    // agree with.
    const float* boundaryZ = boundaries.boundaries();

    // A caller that did not call appendPixel() the full pixelCount times is a
    // bug, but it must not become an out-of-bounds read here: pixels beyond
    // what was actually appended are treated as zero-sample (an empty
    // [sampleCount, sampleCount) range) rather than reading past pixelOffset's
    // end.
    const std::ptrdiff_t filled =
        (samples.pixelsAppended < samples.pixelCount) ? samples.pixelsAppended
                                                       : samples.pixelCount;

    for (std::ptrdiff_t i = 0; i < pixelCount; ++i) {
        std::int32_t begin;
        std::int32_t end;
        if (i < filled) {
            begin = samples.pixelOffset[static_cast<std::size_t>(i)];
            end   = samples.pixelOffset[static_cast<std::size_t>(i) + 1];
        } else {
            begin = end = static_cast<std::int32_t>(samples.sampleCount);
        }
        const int n = static_cast<int>(end - begin);

        float* outRow = boundaryT.data() + static_cast<std::size_t>(i) * boundaryCount;

        // THE FOLD-IN.  One O(H_i + K) walk per pixel (build()'s own fast
        // path, verified sorted by appendPixel()) -- no search, per-fragment
        // or otherwise, happens here or downstream in the scatter.
        HoldoutVisibility::build(samples.zFront.data() + begin,
                                 samples.zBack.data() + begin,
                                 samples.alpha.data() + begin,
                                 n, boundaryZ, boundaryCount, outRow);
    }
}

void HoldoutLut::release()
{
    boundaryT.release();
    boundaries    = HoldoutBoundaries{};
    boundaryCount = 0;
    pixelCount    = 0;
}

std::size_t HoldoutLut::sizeBytes() const
{
    return boundaryT.sizeBytes();
}

HoldoutSoA HoldoutLut::view() const
{
    HoldoutSoA v;
    v.boundaryT  = boundaryT.data();
    v.boundaries = boundaries;
    v.pixelCount = pixelCount;
    return v;
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
    colocated.assign(plane, 0.0f);
    arrival.assign(px, 0.0f);   // K-independent: one per pixel, not per bucket
}

void BucketPlanes::zero()
{
    // assign() is resizeUninitialized + fill, and the sizes are unchanged, so
    // this is a pure fill: the band loop must not re-malloc per band.
    color.assign(color.size(), 0.0f);
    alpha.assign(alpha.size(), 0.0f);
    weight.assign(weight.size(), 0.0f);
    colocated.assign(colocated.size(), 0.0f);
    arrival.assign(arrival.size(), 0.0f);
}

void BucketPlanes::release()
{
    color.release();
    alpha.release();
    weight.release();
    colocated.release();
    arrival.release();
    bucketCount  = 0;
    channelCount = 0;
    width        = 0;
    height       = 0;
    pixelCount   = 0;
}

std::size_t BucketPlanes::sizeBytes() const
{
    return color.sizeBytes() + alpha.sizeBytes() + weight.sizeBytes()
         + colocated.sizeBytes() + arrival.sizeBytes();
}

BucketPlaneView BucketPlanes::view()
{
    BucketPlaneView v;
    v.color        = color.data();
    v.alpha        = alpha.data();
    v.weight       = weight.data();
    v.colocated    = colocated.data();
    v.arrival      = arrival.data();
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

    // Channel groups: there is always exactly one, covering every channel
    // with radiusScale 1.0.  The loop below exists because the group array is
    // the chromatic-aberration seam and a per-group kernel radius is the whole
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
        frag.share       = samples.arrivalShare[f];

        frag.color = samples.colorOf(f);

        // Does this fragment own its parent sample's kernel coverage?  Point
        // samples always do; a split volumetric parent's front-most part does
        // and its remaining parts do not.  Like the composition
        // contract above, this is a LABEL THE FLATTEN ALREADY DECIDED and this
        // file only honours — the scatter has no way to tell which fragments
        // came from one parent.
        frag.coverageHead = fragmentCoverageHeadOf(samples.flags[f]);
        frag.depositArea0 = fragmentDepositsArea0Of(samples.flags[f]);
        frag.depositArea1 = fragmentDepositsArea1Of(samples.flags[f]);

        // Zero-alpha early-out, before any rasterisation.
        // partitionColorScale() is alpha_i/alpha, so a
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

        // THE HOLDOUT LUT'S OWN (index, frac), from the LUT's own boundary
        // set.  O(1) closed form, derived here rather than precomputed
        // by the flatten: the pair is only meaningful against the boundary
        // array the LUT was built at, and that array travels with the LUT.
        // Not DepthBuckets::locateBoundary()'s pair, and not bucketOf()'s.
        // Skipped entirely when there is no holdout -- the zero-cost path.
        if (useHoldout) {
            const BoundarySpan hb = vis.locate(depth);
            frag.boundaryIndex = hb.index;
            frag.boundaryFrac  = hb.frac;
        }

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
            // build would count them once per group.  Group 0 carries them.
            // GOTCHA for a future multi-group build: with radiusScale != 1
            // that ties alpha to group 0's kernel radius, which is a real
            // decision to make then (probably "alpha follows the base/green
            // group"); with every scale at 1.0, group 0's kernel IS the base
            // kernel and the choice is not observable.  Within group 0 the COVERAGE plane is
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

            // --- the bracketing-kernel blend --------------------------------
            // The radius picks the two grid nodes around it, not the nearest
            // one, and the fragment rasterises both at (1 - f) and f.  Every
            // deposit is linear in the weight, so the pair is exactly one pass
            // over the blended row.  On a node the bracket collapses to a
            // single pass at weight 1, which needs no scratch row and deposits
            // the raw kernel row unchanged.
            const KernelGridBracket br = kernelGridBracket(radius);
            const int   nodeIndex[2] = { br.indexA, br.indexB };
            const float nodeBlend[2] = { 1.0f - br.frac, br.frac };
            const int   passes       = (br.indexB != br.indexA) ? 2 : 1;

            for (int p = 0; p < passes; ++p) {
                if (nodeBlend[p] == 0.0f)
                    continue;

                // DiscKernelLUT ignores destX/destY/depth/channelGroup; they
                // are passed anyway because that unused-ness IS the sampler
                // seam.
                const KernelView kv = kernel.kernel(kernelGridRadius(nodeIndex[p]),
                                                    frag.destX, frag.destY,
                                                    depth, g);
                if (!kv.valid())
                    continue;

                float* rowScratch = nullptr;
                if (useHoldout || nodeBlend[p] != 1.0f) {
                    // The widest span a kernel row can have, before clipping.
                    scratch.ensureRow(static_cast<std::size_t>(2 * kv.radiusX + 1));
                    rowScratch = scratch.rowWeights.data();
                }

                std::size_t rows = 0;
                touched += scatterFragmentSpans(view, vis, kv, frag,
                                                nodeBlend[p], rowScratch, &rows);
                if (stats != nullptr)
                    stats->rowSpans += rows;
            }
        }

        if (stats != nullptr) {
            stats->pixelDeposits += touched;
            if (touched == 0)
                ++stats->culled;
        }
    }
}

// ---------------------------------------------------------------------------
// scatterBackgroundCPU
// ---------------------------------------------------------------------------

void scatterBackgroundCPU(const ScatterParams&  params,
                          const ResidualWindow&  residual,
                          const KernelSampler&   kernel,
                          BucketPlanes&          planes)
{
    const BucketPlaneView view = planes.view();
    if (!view.valid())
        return;
    if (view.width != params.bandWidth || view.height != params.bandHeight)
        return;

    const float sharpRadius = clampf(params.sharpRadiusPx, 0.0f, 1e6f);

    for (int wy = 0; wy < residual.height; ++wy) {
        const int py    = residual.y + wy;
        const int destY = py - params.bandY;

        for (int wx = 0; wx < residual.width; ++wx) {
            const int px = residual.x + wx;
            const std::size_t i =
                static_cast<std::size_t>(residual.index(px, py));
            // The skip floor IS the fill's deficit tolerance, and the two
            // cannot be set independently: whatever this drops is missing from
            // the divisor the fill later measures against 1.  Each dropped
            // pixel withholds at most kFillDeficitTol of a unit kernel, so the
            // whole dropped field costs arrival at most kFillDeficitTol and
            // the fill still reads that pixel as full.  Drop at a looser floor
            // than the fill's and a genuinely non-opaque pixel divides by an
            // arrival short by more than the tolerance, which pulls it to
            // alpha 1 -- the error is exactly the residual that was withheld.
            const float T = residual.t[i];
            if (!(T > kFillDeficitTol))
                continue;

            const int   destX    = px - params.bandX;
            const float radiusPx = residual.radiusPx[i];

            // Sharp fast path, same threshold the fragment scatter uses: a
            // pixel this close to the focal plane deposits its own claim at
            // its own pixel, no disc rasterised.
            if (!(radiusPx >= sharpRadius)) {
                if (destX < 0 || destX >= view.width
                 || destY < 0 || destY >= view.height)
                    continue;
                const std::ptrdiff_t dstOffset =
                    static_cast<std::ptrdiff_t>(destY) * view.width + destX;
                view.arrival[dstOffset] += T;
                continue;
            }

            // The per-pixel kernel lookup, through the same bracketing blend
            // the fragment scatter uses: this pixel's OWN residual radius --
            // not a single frame-wide one -- picks the pair of grid nodes, and
            // the claim splits across them.  See the header doc: a mismatched
            // radius is a measured artifact, not a rounding difference, and
            // snapping the residual to the nearest node while fragments blend
            // is the same mismatch one grid step wide.
            const KernelGridBracket br = kernelGridBracket(radiusPx);
            const int   nodeIndex[2] = { br.indexA, br.indexB };
            const float nodeBlend[2] = { 1.0f - br.frac, br.frac };
            const int   passes       = (br.indexB != br.indexA) ? 2 : 1;

            for (int p = 0; p < passes; ++p) {
                const float claim = T * nodeBlend[p];
                if (claim == 0.0f)
                    continue;

                const KernelView kv = kernel.kernel(kernelGridRadius(nodeIndex[p]),
                                                    destX, destY, 0.0f, 0);
                if (!kv.valid())
                    continue;

                for (int row = 0; row < kv.rowCount; ++row) {
                    const RowSpan& span = kv.row(row);
                    if (span.empty())
                        continue;

                    const int dy = destY + kv.rowY(row);
                    if (dy < 0 || dy >= view.height)
                        continue;

                    int xs = destX + span.xStart;
                    int xe = destX + span.xEnd;
                    int skip = 0;
                    if (xs < 0) {
                        skip = -xs;
                        xs   = 0;
                    }
                    if (xe >= view.width)
                        xe = view.width - 1;
                    if (xe < xs)
                        continue;

                    const int count = xe - xs + 1;
                    const std::ptrdiff_t dstOffset =
                        static_cast<std::ptrdiff_t>(dy) * view.width + xs;
                    const float* w = kv.rowWeights(row) + skip;

                    float* __restrict__ arrivalDst = view.arrival + dstOffset;
                    for (int k = 0; k < count; ++k)
                        arrivalDst[k] += w[k] * claim;
                }
            }
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
    // Unused: there is one bucket composite and nothing to select.  The
    // parameter stays because scatterBandCPU() and resolveBandCPU() take the
    // same ScatterParams, and per-band kernel state lands in it.
    (void)params;

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

    // The bucket composite: one rule, the coverage partition.  A plain
    // front-to-back `over` of the planes is NOT equivalent -- see the
    // bucket-composite block in DeepCDefocusScatter.h for the measured
    // difference and the regimes it shows up in.
    for (std::ptrdiff_t i = 0; i < view.pixelCount; ++i) {
        compositePixelCoveragePartition(view.color + i,
                                        view.alpha + i,
                                        view.weight + i,
                                        view.colocated + i,
                                        view.bucketCount,
                                        view.channelCount,
                                        view.pixelCount,
                                        outColor + i,
                                        outAlpha + i,
                                        view.arrival[i]);
    }
}

} // namespace deepc
