// SPDX-License-Identifier: MIT
//
// ============================================================================
//
//  DeepCDefocusScatter — SoA flattening of deep samples and the band's
//                        depth-ordered streaming composite
//
//  See DeepCDefocusScatter.h for the API and for why nothing in this
//  translation unit may include a DDImage/NDK header.
//
//  Everything below the flatten is a LOOP DRIVER only: every per-span and
//  per-pixel body lives in the header marked DEEPC_HD, so a .cu translation
//  unit compiles those unchanged under nvcc and replaces only what is here.
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
         + arrivalShare.sizeBytes() + flags.sizeBytes() + color.sizeBytes();
}

void SampleSoA::clear()
{
    x.clear();
    y.clear();
    radius.clear();
    depth.clear();
    alpha.clear();
    arrivalShare.clear();
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
    flags.reserve(count);
    color.reserve(count * static_cast<std::size_t>(channelCount));
}

void SampleSoA::appendFragment(const FragmentRecord& f, const float* __restrict__ channels)
{
    const std::size_t n = fragmentCount();
    const std::size_t next = n + 1;

    x.growForAppend(next);
    y.growForAppend(next);
    radius.growForAppend(next);
    depth.growForAppend(next);
    alpha.growForAppend(next);
    arrivalShare.growForAppend(next);
    flags.growForAppend(next);
    color.growForAppend(next * static_cast<std::size_t>(channelCount));

    x.resize(next);
    y.resize(next);
    radius.resize(next);
    depth.resize(next);
    alpha.resize(next);
    arrivalShare.resize(next);
    flags.resize(next);
    color.resize(next * static_cast<std::size_t>(channelCount));

    x[n]            = static_cast<std::int32_t>(f.x);
    y[n]            = static_cast<std::int32_t>(f.y);
    radius[n]       = f.radius;
    depth[n]        = f.depth;
    alpha[n]        = f.alpha;
    arrivalShare[n] = f.share;
    flags[n]        = packFragmentFlags(f.kind, f.cocNegative);

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
// flattenPixelToSoA
// ---------------------------------------------------------------------------

namespace {

// The holdout LUT's bracket for a depth, or 0 with no holdout connected (then
// nothing reads it).  Located in the frame's own set, the one the LUT is
// built at.
inline int holdoutBracketOf(const FlattenParams& params, float depth)
{
    if (!params.holdoutConnected)
        return 0;
    return params.holdoutBoundaries.locate(depth).index;
}

// A run of staged fragments [first, end) about to be emitted as one.  Depth,
// radius and bracket are its first pre-merge group's and never move while
// later groups join, so a chain of joins cannot walk them away one step at a
// time.
struct MergeRun {
    bool         valid          = false;
    std::size_t  first          = 0;
    std::size_t  end            = 0;
    FragmentKind kind           = FragmentKind::Point;
    float        depth          = 0.0f;
    float        radius         = 0.0f;
    float        signedRadius   = 0.0f;
    int          holdoutBracket = 0;
};

inline void setDepthDerived(const FlattenParams& params, MergeRun& run, float depth)
{
    run.depth          = depth;
    run.signedRadius   = signedCocPixels(params.coc, depth);
    run.radius         = std::fabs(run.signedRadius);
    run.holdoutBracket = holdoutBracketOf(params, depth);
}

// Back to front, `C = c + (1 - a) * C`: DeepToImage's own order and rounding,
// so a size-0 pixel, which the merges collapse into one fragment, reproduces
// its flatten bit for bit.  Front to back rounds differently (measured: it
// matches DeepToImage's alpha on 51 329 of 65 536 pixels, back to front on
// all of them).
inline void emitRun(FlattenScratch&     scratch,
                    int                 x,
                    int                 y,
                    const MergeRun&     run,
                    int                 nChan,
                    SampleSoA&          out,
                    FlattenStats*       stats)
{
    scratch.mergeAccum.assign(static_cast<std::size_t>(nChan), 0.0f);
    float alpha = 0.0f;
    float share = 0.0f;
    for (std::size_t s = run.first; s < run.end; ++s)
        share += scratch.staged[s].share;
    for (std::size_t s = run.end; s-- > run.first;) {
        const FlattenScratch::Staged& src = scratch.staged[s];
        const float t = 1.0f - src.alpha;
        for (int c = 0; c < nChan; ++c) {
            float& acc = scratch.mergeAccum[static_cast<std::size_t>(c)];
            acc = src.channels[static_cast<std::size_t>(c)] + acc * t;
        }
        alpha = src.alpha + alpha * t;
    }

    FragmentRecord f;
    f.x           = x;
    f.y           = y;
    f.depth       = run.depth;
    f.radius      = run.radius;
    f.alpha       = clampf(alpha, 0.0f, 1.0f);
    f.kind        = run.kind;
    f.cocNegative = run.signedRadius < 0.0f;
    f.share       = share;
    out.appendFragment(f, scratch.mergeAccum.data());

    if (stats != nullptr)
        ++stats->emittedFragments;
}

// Grows the staging vector but never shrinks it, so each slot's channel
// vector keeps its capacity across pixels.
inline FlattenScratch::Staged& nextStaged(FlattenScratch& scratch)
{
    if (scratch.stagedCount >= scratch.staged.size())
        scratch.staged.resize(scratch.stagedCount + 1);
    return scratch.staged[scratch.stagedCount++];
}

} // namespace

void flattenPixelToSoA(const FlattenParams& params,
                       int                  x,
                       int                  y,
                       std::vector<SampleRecord>& samples,
                       FlattenScratch&      scratch,
                       SampleSoA&           out,
                       FlattenStats*        stats,
                       float*               residualT,
                       float*               residualRadiusPx)
{
    // The SoA's own channel count is authoritative: appendFragment() copies
    // out.channelCount floats, so staging sized from params would read past
    // its end whenever the two disagree (a heap overflow under ASAN).
    const int nChan = (out.channelCount > 0) ? out.channelCount : 0;

    if (samples.empty()) {
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

    // One positive factor on both endpoints: monotone, so it cannot reorder
    // the list, and the same number the holdout and the depth-range pass
    // apply at this pixel.
    const float rayScale = rayDepthScaleAt(params, x, y);

    for (std::size_t i = 0; i < samples.size(); ++i) {
        SampleRecord& s = samples[i];

        float zf = sanitizeFragmentDepth(s.zFront) * rayScale;
        float zb = sanitizeFragmentDepth(s.zBack) * rayScale;
        if (!(zb > zf))
            zb = zf;                    // also rejects a back-before-front span

        s.zFront = zf;
        s.zBack  = zb;
        s.alpha  = clampf(s.alpha, 0.0f, 1.0f);   // NaN -> 0
        s.channels.resize(static_cast<std::size_t>(nChan), 0.0f);
    }

    // Without the tidy, coincident same-pixel samples would reach the stream
    // as separate full-coverage layers instead of their mixture, and size-0
    // parity with DeepToImage would be unachievable.
    if (samples.size() > 1)
        tidyOverlapping(samples);

    // tidyOverlapping() only sorts at 2+ samples; the staging below must be
    // front to back for the share partition and the merges.
    std::sort(samples.begin(), samples.end(),
        [](const SampleRecord& a, const SampleRecord& b) {
            return (a.zFront != b.zFront) ? a.zFront < b.zFront
                                          : a.zBack  < b.zBack;
        });

    if (stats != nullptr)
        stats->tidiedSamples += samples.size();

    scratch.stagedCount = 0;

    const int maxPieces = clampi(params.maxVolumetricPieces, 1, kMaxVolumetricPieces);
    VolumetricPiece pieces[kMaxVolumetricPieces];

    // THE GATHER-SHARE PARTITION: one running transmittance for the pixel,
    // taken front to back by every staged fragment, so the shares plus the
    // final `arrivalT` (the virtual background's claim) sum to 1.  The
    // merges regroup shares and never recompute them.
    float arrivalT = 1.0f;

    for (std::size_t i = 0; i < samples.size(); ++i) {
        const SampleRecord& s = samples[i];

        // DeepToImage (DD::Image::CompositeSamples) skips alpha-0 samples, and
        // an alpha-0 sample deeper than the content is not a surface for the
        // residual radius either.
        if (!(s.alpha > 0.0f))
            continue;

        if (!(s.zBack > s.zFront)) {
            FlattenScratch::Staged& st = nextStaged(scratch);
            st.zFront       = s.zFront;
            st.zBack        = s.zBack;
            st.alpha        = s.alpha;
            st.kind         = FragmentKind::Point;
            st.channels.assign(s.channels.begin(), s.channels.end());
            st.depth        = sampleMidDepth(st.zFront, st.zBack);
            st.signedRadius = signedCocPixels(params.coc, st.depth);
            st.radius       = std::fabs(st.signedRadius);
            st.share        = arrivalT * s.alpha;
            arrivalT *= (1.0f - s.alpha);
            continue;
        }

        const int nPieces = volumetricPieceBounds(params.coc, s.zFront, s.zBack, s.alpha,
                                                  params.pieceStepPx, pieces, maxPieces);
        if (stats != nullptr)
            stats->splitParts += static_cast<std::size_t>(nPieces);

        const std::size_t parentFirst = scratch.stagedCount;
        float             parentShare = 0.0f;

        for (int p = 0; p < nPieces; ++p) {
            const VolumetricPiece& piece = pieces[p];
            // Not piece.alpha: a thin fog piece's alpha can underflow to 0
            // while its colour scale does not.
            if (!(piece.t > 0.0f))
                continue;

            const float pieceShare = arrivalT * piece.alpha;
            arrivalT *= (1.0f - piece.alpha);
            parentShare += pieceShare;

            FlattenScratch::Staged& st = nextStaged(scratch);
            st.zFront = piece.zFront;
            st.zBack  = piece.zBack;
            st.alpha  = piece.alpha;
            st.kind   = FragmentKind::Volumetric;
            st.share  = pieceShare;
            st.channels.resize(static_cast<std::size_t>(nChan));
            for (int c = 0; c < nChan; ++c)
                st.channels[static_cast<std::size_t>(c)] =
                    s.channels[static_cast<std::size_t>(c)] * piece.colorScale;
            st.depth        = sampleMidDepth(st.zFront, st.zBack);
            st.signedRadius = signedCocPixels(params.coc, st.depth);
            st.radius       = std::fabs(st.signedRadius);
        }

        // The cut must not move where the parent claims arrival: spread over
        // its pieces' radii, a wide-to-narrow parent claims far less at its
        // own pixel than the unit background kernel its neighbours deposit,
        // and the deficit fill fires on it.
        if (scratch.stagedCount > parentFirst) {
            for (std::size_t k = parentFirst; k + 1 < scratch.stagedCount; ++k)
                scratch.staged[k].share = 0.0f;
            scratch.staged[scratch.stagedCount - 1].share = parentShare;
        }
    }

    const std::size_t staged = scratch.stagedCount;
    if (stats != nullptr)
        stats->stagedFragments += staged;
    if (staged == 0) {
        if (residualT != nullptr)
            *residualT = 1.0f;
        return;
    }

    // Before the merges regroup the list: "deepest" is the last fragment
    // staged, not whichever run is emitted last.
    const float deepestRadius = scratch.staged[staged - 1].radius;

    // PRE-MERGE (pre_merge / merge_tolerance, CoC-RADIUS pixels).  Adjacent
    // fragments join the group while within the tolerance of its first
    // radius and, with a holdout connected, in its holdout bracket: the group
    // is emitted at ONE depth and the holdout is sampled per fragment, so an
    // unbracketed group could carry a sample from behind a card to in front
    // of it.  No depth key: the stream orders fragments by depth, so a merge
    // moves nothing between layers.  Volumetric pieces are cut at twice the
    // tolerance and so only regroup on the max_radius plateau, where their
    // kernels are identical.
    //
    // COLLISION MERGE (always on).  A group joins the held-back run when both
    // see the same lens patch from every destination pixel (sameLensPatch)
    // and share a holdout bracket: the rear one is then exactly behind the
    // front one, and `over` is the physical answer the unmerged stream would
    // only approximate (it would treat the rear as landing on free area
    // first).  It also collapses what pre_merge off leaves, so the two knob
    // states flatten a size-0 pixel identically.
    const bool  merging = params.preMerge && (params.mergeTolerancePx > 0.0f);
    const float tol     = params.mergeTolerancePx;

    MergeRun    pending;
    std::size_t i = 0;
    while (i < staged) {
        const FlattenScratch::Staged& head = scratch.staged[i];

        std::size_t j = i + 1;
        if (merging) {
            const int headBracket = holdoutBracketOf(params, head.depth);
            while (j < staged) {
                const FlattenScratch::Staged& cand = scratch.staged[j];
                if (!(std::fabs(cand.radius - head.radius) <= tol))
                    break;
                if (holdoutBracketOf(params, cand.depth) != headBracket)
                    break;
                ++j;
            }
        }

        float zf = head.zFront;
        float zb = head.zBack;
        for (std::size_t s = i + 1; s < j; ++s) {
            zf = std::min(zf, scratch.staged[s].zFront);
            zb = std::max(zb, scratch.staged[s].zBack);
        }

        MergeRun group;
        group.valid = true;
        group.first = i;
        group.end   = j;
        group.kind  = head.kind;
        setDepthDerived(params, group, sampleMidDepth(zf, zb));
        i = j;

        const bool join = pending.valid
                       && sameLensPatch(pending.signedRadius, group.signedRadius)
                       && pending.holdoutBracket == group.holdoutBracket;
        if (join) {
            pending.end = group.end;
        } else {
            if (pending.valid)
                emitRun(scratch, x, y, pending, nChan, out, stats);
            pending = group;
        }
    }
    if (pending.valid)
        emitRun(scratch, x, y, pending, nChan, out, stats);

    if (residualT != nullptr)
        *residualT = arrivalT;
    if (residualRadiusPx != nullptr)
        *residualRadiusPx = deepestRadius;
}

// ---------------------------------------------------------------------------
// checkCompositionContract
// ---------------------------------------------------------------------------

bool checkCompositionContract(const SampleSoA& soa, std::size_t* firstBadFragment)
{
    const std::size_t n = soa.fragmentCount();
    for (std::size_t i = 0; i < n; ++i) {
        const float a = soa.alpha[i];
        const float r = soa.radius[i];
        bool ok = std::isfinite(a) && std::isfinite(r) && std::isfinite(soa.depth[i])
               && std::isfinite(soa.arrivalShare[i]);
        ok = ok && a >= 0.0f && a <= 1.0f && r >= 0.0f;
        const float* c = soa.colorOf(i);
        for (int k = 0; k < soa.channelCount && ok; ++k)
            ok = std::isfinite(c[k]);
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
    // sampled at holdout boundaries that live in Z, so a holdout left in
    // ray-distance space sits systematically too far back off-axis.
    const float zScale = (depthScale > 0.0f && std::isfinite(depthScale))
                       ? depthScale : 1.0f;

    // --- sanitise, drop alpha<=0 and NaN depths, compact in place -----------
    std::size_t kept = 0;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        SampleRecord& s = samples[i];

        // A NON-FINITE FRONT DEPTH IS DROPPED HERE, unlike in the source
        // flatten.  sanitizeFragmentDepth() maps NaN to 0, which is harmless on
        // the source side (signedCocPixels() gives d <= 0 radius 0 and the
        // sample still composites in its own pixel) but is catastrophic here:
        // depth 0 is in front of boundary(0), so ONE NaN in the holdout's
        // DeepFront makes an opaque sample attenuate the destination pixel at
        // every boundary — a black hole in the plate rather than a lost
        // sample.  +/-inf is a real far-field holdout and is kept (mapped to
        // kMaxDepth), exactly as on the source side; only NaN is dropped.
        const bool badDepth = std::isnan(s.zFront) || std::isnan(s.zBack);

        float zf = sanitizeFragmentDepth(s.zFront) * zScale;
        float zb = sanitizeFragmentDepth(s.zBack) * zScale;
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

    // THE BOUNDARY DEPTHS ARE THE HOLDOUT SET'S -- see the
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
// StreamPlanes
// ---------------------------------------------------------------------------

void StreamPlanes::allocate(int channelCountIn, int widthIn, int heightIn)
{
    channelCount = (channelCountIn > 0) ? channelCountIn : 0;
    width        = (widthIn  > 0) ? widthIn  : 0;
    height       = (heightIn > 0) ? heightIn : 0;
    pixelCount   = static_cast<std::ptrdiff_t>(width) * height;

    const std::size_t px = static_cast<std::size_t>(pixelCount);
    claimed.assign(px, 0.0f);
    alpha.assign(px, 0.0f);
    oldArea.assign(px, 0.0f);
    oldMass.assign(px, 0.0f);
    lastCoc.assign(px, 0.0f);
    color.assign(px * static_cast<std::size_t>(channelCount), 0.0f);
    arrival.assign(px, 0.0f);
}

void StreamPlanes::zero()
{
    claimed.assign(claimed.size(), 0.0f);
    alpha.assign(alpha.size(), 0.0f);
    oldArea.assign(oldArea.size(), 0.0f);
    oldMass.assign(oldMass.size(), 0.0f);
    lastCoc.assign(lastCoc.size(), 0.0f);
    color.assign(color.size(), 0.0f);
    arrival.assign(arrival.size(), 0.0f);
}

void StreamPlanes::release()
{
    claimed.release();
    alpha.release();
    oldArea.release();
    oldMass.release();
    lastCoc.release();
    color.release();
    arrival.release();
    channelCount = 0;
    width        = 0;
    height       = 0;
    pixelCount   = 0;
}

std::size_t StreamPlanes::sizeBytes() const
{
    return claimed.sizeBytes() + alpha.sizeBytes() + oldArea.sizeBytes()
         + oldMass.sizeBytes() + lastCoc.sizeBytes() + color.sizeBytes()
         + arrival.sizeBytes();
}

StreamPlaneView StreamPlanes::view()
{
    StreamPlaneView v;
    v.claimed      = claimed.data();
    v.alpha        = alpha.data();
    v.oldArea      = oldArea.data();
    v.oldMass      = oldMass.data();
    v.lastCoc      = lastCoc.data();
    v.color        = color.data();
    v.arrival      = arrival.data();
    v.channelCount = channelCount;
    v.width        = width;
    v.height       = height;
    v.pixelCount   = pixelCount;
    return v;
}

// ---------------------------------------------------------------------------
// sortFragmentsByDepth
// ---------------------------------------------------------------------------

void sortFragmentsByDepth(const SampleSoA&          samples,
                          PodBuffer<std::uint32_t>& order,
                          StreamSortScratch&        scratch)
{
    const std::size_t n = samples.fragmentCount();
    order.resizeUninitialized(n);
    if (n == 0)
        return;

    scratch.keys.resizeUninitialized(n);
    scratch.keysAlt.resizeUninitialized(n);
    scratch.orderAlt.resizeUninitialized(n);

    std::uint32_t* keys    = scratch.keys.data();
    std::uint32_t* keysAlt = scratch.keysAlt.data();
    std::uint32_t* ord     = order.data();
    std::uint32_t* ordAlt  = scratch.orderAlt.data();

    std::uint32_t allAnd = 0xffffffffu;
    std::uint32_t allOr  = 0u;
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint32_t k = orderedDepthKey(samples.depth[i]);
        keys[i] = k;
        ord[i]  = static_cast<std::uint32_t>(i);
        allAnd &= k;
        allOr  |= k;
    }
    const std::uint32_t varying = allAnd ^ allOr;

    for (int shift = 0; shift < 32; shift += 8) {
        if (((varying >> shift) & 0xffu) == 0u)
            continue;
        std::size_t count[257] = {};
        for (std::size_t i = 0; i < n; ++i)
            ++count[((keys[i] >> shift) & 0xffu) + 1];
        for (int b = 0; b < 256; ++b)
            count[b + 1] += count[b];
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t dst = count[(keys[i] >> shift) & 0xffu]++;
            keysAlt[dst] = keys[i];
            ordAlt[dst]  = ord[i];
        }
        std::swap(keys, keysAlt);
        std::swap(ord, ordAlt);
    }

    if (ord != order.data())
        std::memcpy(order.data(), ord, n * sizeof(std::uint32_t));
}

bool checkStreamOrder(const SampleSoA& samples, const PodBuffer<std::uint32_t>& order)
{
    const std::size_t n = samples.fragmentCount();
    if (order.size() != n)
        return false;
    std::vector<char> seen(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint32_t f = order[i];
        if (f >= n || seen[f])
            return false;
        seen[f] = 1;
        if (i > 0) {
            const std::uint32_t g  = order[i - 1];
            const std::uint32_t kf = orderedDepthKey(samples.depth[f]);
            const std::uint32_t kg = orderedDepthKey(samples.depth[g]);
            if (kg > kf || (kg == kf && g > f))
                return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// scatterStreamCPU
// ---------------------------------------------------------------------------

void scatterStreamCPU(const ScatterParams&            params,
                      const SampleSoA&                samples,
                      const PodBuffer<std::uint32_t>& order,
                      const HoldoutSoA&               holdout,
                      const KernelSampler&            kernel,
                      StreamPlanes&                   planes,
                      ScatterScratch&                 scratch,
                      ScatterStats*                   stats)
{
    const StreamPlaneView view = planes.view();
    if (!view.valid())
        return;

    // The planes own the memory, so a disagreement with params is never
    // resolved in favour of the side that does not.
    if (view.width != params.bandWidth || view.height != params.bandHeight)
        return;

    const std::size_t n = samples.fragmentCount();
    if (order.size() != n || n == 0)
        return;

    const int nChan = (samples.channelCount < view.channelCount)
                    ? samples.channelCount : view.channelCount;

    ChannelGroups groups = samples.groups;
    if (groups.groupCount <= 0)
        groups = makeSingleChannelGroup(nChan);

    const float sharpRadius = clampf(params.sharpRadiusPx, 0.0f, 1e6f);

    // A LUT that does not cover the whole band is treated as absent rather
    // than read out of bounds.
    const bool useHoldout = holdout.enabled() && holdout.pixelCount >= view.pixelCount;

    for (std::size_t oi = 0; oi < n; ++oi) {
        const std::size_t f = order[oi];
        if (stats != nullptr)
            ++stats->fragments;

        const float  alpha = samples.alpha[f];
        const float* color = samples.colorOf(f);
        if (alpha == 0.0f) {
            bool anyColor = false;
            for (int c = 0; c < nChan; ++c)
                anyColor = anyColor || (color[c] != 0.0f);
            if (!anyColor) {
                if (stats != nullptr)
                    ++stats->culled;
                continue;
            }
        }

        const int   destX     = static_cast<int>(samples.x[f]) - params.bandX;
        const int   destY     = static_cast<int>(samples.y[f]) - params.bandY;
        const float depth     = samples.depth[f];
        const float share     = samples.arrivalShare[f];
        const float radius    = groupRadius(groups, 0, samples.radius[f]);
        const float signedCoc = fragmentSignedCoc(samples.flags[f], radius);

        BoundarySpan hb;
        if (useHoldout)
            hb = holdout.locate(depth);

        // AT the floor as well as below it: a 1 px diameter IS the sharp
        // delta, which keeps d = 1 one kernel at every edge_softness.
        if (!(radius > sharpRadius)) {
            if (destX < 0 || destX >= view.width || destY < 0 || destY >= view.height) {
                if (stats != nullptr)
                    ++stats->culled;
                continue;
            }
            const std::ptrdiff_t off = static_cast<std::ptrdiff_t>(destY) * view.width + destX;
            view.arrival[off] += share;
            float w = 1.0f;
            if (useHoldout)
                w = HoldoutVisibility::interpAtBucket(holdout.pixelLut(off),
                                                      holdout.boundaryCount(),
                                                      hb.index, hb.frac);
            scratch.ensureRow(1);
            depositStreamSpanRecency(view, off, &w, scratch.xRow.data(), 1,
                                     alpha, signedCoc, color, nChan);
            if (stats != nullptr) {
                ++stats->sharpFragments;
                ++stats->pixelDeposits;
            }
            continue;
        }

        const KernelGridBracket br = kernelGridBracket(radius);
        const bool blend = (br.indexB != br.indexA) && (br.frac != 0.0f);

        // DiscKernelLUT ignores destX/destY/depth/group; they are passed
        // because that unused-ness IS the sampler seam.
        const KernelView kvA = kernel.kernel(kernelGridRadius(br.indexA),
                                             destX, destY, depth, 0);
        KernelView kvB;
        if (blend)
            kvB = kernel.kernel(kernelGridRadius(br.indexB), destX, destY, depth, 0);
        if (!kvA.valid() || (blend && !kvB.valid())) {
            if (stats != nullptr)
                ++stats->culled;
            continue;
        }

        const int rx = (blend && kvB.radiusX > kvA.radiusX) ? kvB.radiusX : kvA.radiusX;
        const int ry = (blend && kvB.radiusY > kvA.radiusY) ? kvB.radiusY : kvA.radiusY;
        scratch.ensureRow(static_cast<std::size_t>(2 * rx + 1));
        const float fA = 1.0f - br.frac;
        const float fB = br.frac;

        std::size_t touched = 0;
        for (int dy = -ry; dy <= ry; ++dy) {
            const int py = destY + dy;
            if (py < 0 || py >= view.height)
                continue;

            const float* row = nullptr;
            int xs = 0;
            int count = 0;
            if (blend) {
                count = blendBracketRow(kvA, fA, kvB, fB, dy, scratch.rowWeights.data(), xs);
                row   = scratch.rowWeights.data();
            } else {
                const int r = dy + kvA.radiusY;
                if (r < 0 || r >= kvA.rowCount || kvA.row(r).empty())
                    continue;
                xs    = kvA.row(r).xStart;
                count = kvA.row(r).count();
                row   = kvA.rowWeights(r);
            }
            if (count <= 0)
                continue;

            int dxs = destX + xs;
            int dxe = dxs + count - 1;
            int skip = 0;
            if (dxs < 0) {
                skip = -dxs;
                dxs  = 0;
            }
            if (dxe >= view.width)
                dxe = view.width - 1;
            if (dxe < dxs)
                continue;

            const int            span = dxe - dxs + 1;
            const std::ptrdiff_t off  = static_cast<std::ptrdiff_t>(py) * view.width + dxs;
            const float*         raw  = row + skip;

            float* __restrict__ arrival = view.arrival + off;
            for (int k = 0; k < span; ++k)
                arrival[k] += raw[k] * share;

            const float* w = raw;
            if (useHoldout) {
                float* __restrict__ wv = scratch.visWeights.data();
                for (int k = 0; k < span; ++k)
                    wv[k] = raw[k] * HoldoutVisibility::interpAtBucket(
                        holdout.pixelLut(off + k), holdout.boundaryCount(), hb.index, hb.frac);
                w = wv;
            }

            depositStreamSpanRecency(view, off, w, scratch.xRow.data(), span,
                                     alpha, signedCoc, color, nChan);

            touched += static_cast<std::size_t>(span);
            if (stats != nullptr)
                ++stats->rowSpans;
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
                          const ResidualWindow& residual,
                          const KernelSampler&  kernel,
                          float*                arrival)
{
    const int width  = params.bandWidth;
    const int height = params.bandHeight;
    if (arrival == nullptr || width <= 0 || height <= 0)
        return;

    const float sharpRadius = clampf(params.sharpRadiusPx, 0.0f, 1e6f);

    for (int wy = 0; wy < residual.height; ++wy) {
        const int py    = residual.y + wy;
        const int destY = py - params.bandY;

        for (int wx = 0; wx < residual.width; ++wx) {
            const int px = residual.x + wx;
            const std::size_t i = static_cast<std::size_t>(residual.index(px, py));
            const float T = residual.t[i];
            if (!(T > kFillDeficitTol))
                continue;

            const int   destX    = px - params.bandX;
            const float radiusPx = residual.radiusPx[i];

            if (!(radiusPx > sharpRadius)) {
                if (destX < 0 || destX >= width || destY < 0 || destY >= height)
                    continue;
                arrival[static_cast<std::ptrdiff_t>(destY) * width + destX] += T;
                continue;
            }

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
                    if (dy < 0 || dy >= height)
                        continue;

                    int xs = destX + span.xStart;
                    int xe = destX + span.xEnd;
                    int skip = 0;
                    if (xs < 0) {
                        skip = -xs;
                        xs   = 0;
                    }
                    if (xe >= width)
                        xe = width - 1;
                    if (xe < xs)
                        continue;

                    const int count = xe - xs + 1;
                    const float* w = kv.rowWeights(row) + skip;
                    float* __restrict__ dst =
                        arrival + static_cast<std::ptrdiff_t>(dy) * width + xs;
                    for (int k = 0; k < count; ++k)
                        dst[k] += w[k] * claim;
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// resolveStreamCPU
// ---------------------------------------------------------------------------

void resolveStreamCPU(StreamPlanes&       planes,
                      float* __restrict__ outColor,
                      float* __restrict__ outAlpha)
{
    const StreamPlaneView view = planes.view();
    if (!view.valid() || outAlpha == nullptr)
        return;
    if (view.channelCount > 0 && outColor == nullptr)
        return;

    for (std::ptrdiff_t i = 0; i < view.pixelCount; ++i)
        resolveStreamPixel(view, i, outColor, outAlpha);
}

} // namespace deepc
