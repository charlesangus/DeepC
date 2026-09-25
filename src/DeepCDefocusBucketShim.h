// SPDX-License-Identifier: MIT
//
// The depth-bucket composite's assignment primitives.  Only the bucket
// composite (flatten, scatterBandCPU, the bucket-composite fill runs) still
// calls them; nothing in the streaming composite may include this header.

#ifndef DEEPC_DEEPCDEFOCUS_BUCKET_SHIM_H
#define DEEPC_DEEPCDEFOCUS_BUCKET_SHIM_H

#include "DeepCDefocusMath.h"

namespace deepc {

struct BucketWeight {
    int   index = 0;
    float frac  = 0.0f;

    DEEPC_HD inline float weightLow()  const { return 1.0f - frac; }
    DEEPC_HD inline float weightHigh() const { return frac; }
    DEEPC_HD inline int   indexHigh()  const { return (frac > 0.0f) ? (index + 1) : index; }
};

struct BucketDeposit {
    int   index0      = 0;
    int   index1      = 0;
    float alpha0      = 0.0f;
    float alpha1      = 0.0f;
    float colorScale0 = 0.0f;
    float colorScale1 = 0.0f;
};

struct SpanSplitPart {
    float zFront     = 0.0f;
    float zBack      = 0.0f;
    float t          = 1.0f;
    float alpha      = 0.0f;
    float colorScale = 1.0f;
};

DEEPC_HD inline float bucketCentre(const DepthBuckets& b, int i)
{
    return 0.5f * (b._boundaries[i] + b._boundaries[i + 1]);
}

DEEPC_HD inline BucketWeight bucketOf(const DepthBuckets& b, float depth)
{
    BucketWeight w;
    if (b._bucketCount <= 1)
        return w;

    const int last = b._bucketCount - 1;
    if (!(depth > bucketCentre(b, 0)))
        return w;
    if (depth >= bucketCentre(b, last)) {
        w.index = last;
        return w;
    }

    int lo = 0;
    int hi = last;
    while (hi - lo > 1) {
        const int mid = lo + (hi - lo) / 2;
        if (bucketCentre(b, mid) <= depth)
            lo = mid;
        else
            hi = mid;
    }

    const float c0   = bucketCentre(b, lo);
    const float span = bucketCentre(b, lo + 1) - c0;
    w.index = lo;
    w.frac  = (span > 0.0f) ? clampf((depth - c0) / span, 0.0f, 1.0f) : 0.0f;
    return w;
}

DEEPC_HD inline BoundarySpan locateBoundary(const DepthBuckets& b, float depth)
{
    BoundarySpan s;
    if (b._bucketCount <= 0)
        return s;

    const int lastB = b._bucketCount;
    if (!(depth > b._boundaries[0]))
        return s;
    if (depth >= b._boundaries[lastB]) {
        s.index = lastB - 1;
        s.frac  = 1.0f;
        return s;
    }

    int lo = 0;
    int hi = lastB;
    while (hi - lo > 1) {
        const int mid = lo + (hi - lo) / 2;
        if (b._boundaries[mid] <= depth)
            lo = mid;
        else
            hi = mid;
    }

    const float span = b._boundaries[lo + 1] - b._boundaries[lo];
    s.index = lo;
    s.frac  = (span > 0.0f) ? clampf((depth - b._boundaries[lo]) / span, 0.0f, 1.0f) : 0.0f;
    return s;
}

DEEPC_HD inline BucketWeight bucketOfContaining(const DepthBuckets& b, float depth)
{
    BucketWeight w;
    if (b._bucketCount <= 0)
        return w;
    const BoundarySpan s = locateBoundary(b, depth);
    w.index = clampi(s.index, 0, b._bucketCount - 1);
    w.frac  = 0.0f;
    return w;
}

DEEPC_HD inline int firstBoundaryAbove(const DepthBuckets& b, float z)
{
    const int n = b.boundaryCount();
    int lo = 0;
    int hi = n;
    while (lo < hi) {
        const int mid = lo + (hi - lo) / 2;
        if (b._boundaries[mid] > z)
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo;
}

DEEPC_HD inline BucketDeposit fragmentDeposit(const BucketWeight& w, float alpha)
{
    const float w0 = w.weightLow();
    const float w1 = w.weightHigh();

    BucketDeposit d;
    d.index0      = w.index;
    d.index1      = w.indexHigh();
    d.alpha0      = partitionAlpha(alpha, w0);
    d.alpha1      = partitionAlpha(alpha, w1);
    d.colorScale0 = partitionColorScale(alpha, w0);
    d.colorScale1 = partitionColorScale(alpha, w1);
    return d;
}

DEEPC_HD inline int splitSpanAtBoundaries(const DepthBuckets& buckets,
                                          float zFront,
                                          float zBack,
                                          float alpha,
                                          SpanSplitPart* __restrict__ out,
                                          int maxParts)
{
    if (out == nullptr || maxParts <= 0)
        return 0;

    const float a = clampf(alpha, 0.0f, 1.0f);

    if (!(zBack > zFront) || !std::isfinite(zFront) || !std::isfinite(zBack)) {
        out[0].zFront     = zFront;
        out[0].zBack      = zBack;
        out[0].t          = 1.0f;
        out[0].alpha      = a;
        out[0].colorScale = 1.0f;
        return 1;
    }

    const float invThickness = 1.0f / (zBack - zFront);
    const int   n            = buckets.boundaryCount();

    int   count     = 0;
    float partFront = zFront;
    float uPrev     = 0.0f;

    for (int i = firstBoundaryAbove(buckets, zFront); i < n && count < maxParts - 1; ++i) {
        const float b = buckets.boundary(i);
        if (!(b < zBack))
            break;

        const float u = clampf((b - zFront) * invThickness, 0.0f, 1.0f);
        if (!(u > uPrev))
            continue;

        const float t = u - uPrev;
        out[count].zFront     = partFront;
        out[count].zBack      = b;
        out[count].t          = t;
        out[count].alpha      = partitionAlpha(a, t);
        out[count].colorScale = partitionColorScale(a, t);
        ++count;

        partFront = b;
        uPrev     = u;
    }

    const float tailT = 1.0f - uPrev;
    out[count].zFront     = partFront;
    out[count].zBack      = zBack;
    out[count].t          = tailT;
    out[count].alpha      = partitionAlpha(a, tailT);
    out[count].colorScale = partitionColorScale(a, tailT);
    return count + 1;
}

DEEPC_HD inline float saturationScale(float aRaw)
{
    return (aRaw > 1.0f) ? (1.0f / aRaw) : 1.0f;
}

} // namespace deepc

#endif // DEEPC_DEEPCDEFOCUS_BUCKET_SHIM_H
