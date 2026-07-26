// SPDX-License-Identifier: MIT
//
// ============================================================================
//
//  DeepSampleOptimizer — Header-only deep sample merge & cap utility
//
//  Provides reusable per-pixel deep sample optimization: merges nearby-depth
//  samples via front-to-back over-compositing and caps total sample count.
//  Extracted from DeepThinner v2.0 Pass 6 (Smart Merge) and Pass 7 (Max
//  Samples) and generalized to arbitrary channel sets.
//
//  Zero Nuke SDK dependencies — only standard library headers. Designed to be
//  testable in isolation with a trivial harness.
//
// ============================================================================

#ifndef DEEPC_DEEP_SAMPLE_OPTIMIZER_H
#define DEEPC_DEEP_SAMPLE_OPTIMIZER_H

#include <algorithm>
#include <cmath>
#include <vector>

namespace deepc {

// ---------------------------------------------------------------------------
// SampleRecord — one deep sample with arbitrary channel data
// ---------------------------------------------------------------------------
struct SampleRecord {
    float zFront;               // depth front
    float zBack;                // depth back
    float alpha;                // sample alpha (unpremultiplied)
    std::vector<float> channels; // arbitrary channel values (caller decides order)
};

// ---------------------------------------------------------------------------
// colorDistance — max absolute difference across first min(3, N) channels
//
// Unpremultiplied comparison: divides each channel by its sample's alpha
// before computing the max-abs-diff.  Near-zero alpha (< 1e-6) is treated
// as transparent / always-matching → returns 0.
// ---------------------------------------------------------------------------
inline float colorDistance(const std::vector<float>& a, float alphaA,
                           const std::vector<float>& b, float alphaB)
{
    if (alphaA < 1e-6f || alphaB < 1e-6f)
        return 0.0f;

    const float invA = 1.0f / alphaA;
    const float invB = 1.0f / alphaB;
    const size_t n = std::min<size_t>(3, std::min(a.size(), b.size()));
    float d = 0.0f;
    for (size_t i = 0; i < n; ++i)
        d = std::max(d, std::fabs(a[i] * invA - b[i] * invB));
    return d;
}

// ---------------------------------------------------------------------------
// tidyOverlapping — split overlapping depth intervals and merge coincident ones
//
// Walks a depth-sorted sample list.  When sample[i].zBack > sample[i+1].zFront
// (overlap), one of the two volumetric samples is split at the overlap
// boundary that lies strictly inside it.  After all splits, samples sharing an
// identical [zFront,zBack] are merged — by the OpenEXR volume-mixture rule if
// the interval has extent, by `over` if it is a point.  See the merge pass.
//
// This is the tidying algorithm of the OpenEXR "Interpreting Deep Pixels"
// note, and it reproduces stock Nuke DeepToImage (volumetric_composition on,
// its default) to float precision.
// ---------------------------------------------------------------------------
inline void tidyOverlapping(std::vector<SampleRecord>& samples)
{
    if (samples.size() < 2)
        return;

    // --- Split pass: iterate until no overlaps remain ---
    bool changed = true;
    while (changed) {
        changed = false;
        // Sort by zFront, then by zBack ascending
        std::sort(samples.begin(), samples.end(),
            [](const SampleRecord& a, const SampleRecord& b) {
                return (a.zFront != b.zFront) ? a.zFront < b.zFront
                                              : a.zBack < b.zBack;
            });

        for (size_t i = 0; i + 1 < samples.size(); ++i) {
            const SampleRecord& lo = samples[i];
            const SampleRecord& hi = samples[i + 1];

            // No overlap?
            if (lo.zBack <= hi.zFront)
                continue;

            // Decide WHICH of the pair to split, and where.  The split point
            // must lie STRICTLY inside the span being split; splitting at one
            // of its own endpoints produces a zero-extent piece plus an
            // unchanged copy of the original, so the scan restarts on the same
            // overlap forever (verified: the naive "always split lo at
            // hi.zFront" form never terminates for ANY overlapping pair of
            // volumetric samples, growing the vector without bound).
            //
            // The sort above guarantees lo.zFront <= hi.zFront, and
            // lo.zBack <= hi.zBack whenever the fronts are equal.
            size_t splitIdx;
            float  z;
            if (lo.zFront < hi.zFront) {
                // hi starts strictly inside lo -> split lo at hi's front.
                splitIdx = i;
                z        = hi.zFront;
            } else {
                // Shared front: lo is the shorter (or equal) span.
                if (hi.zBack <= lo.zBack)
                    continue;   // perfectly coincident — the over-merge pass below handles it
                // Split the longer span at the shorter one's back, which
                // leaves a piece coincident with lo (merged below) and a
                // strictly shorter remainder.
                splitIdx = i + 1;
                z        = lo.zBack;
            }

            SampleRecord& cur = samples[splitIdx];

            // Guard the strictness the termination argument relies on. Every
            // split point is an endpoint that already existed in the list, so
            // the set of distinct endpoints never grows and the number of
            // reachable spans is finite — hence the loop terminates.
            if (!(z > cur.zFront && z < cur.zBack))
                continue;

            const double totalRange = static_cast<double>(cur.zBack)
                                    - static_cast<double>(cur.zFront);
            const double ratio = (static_cast<double>(z)
                                - static_cast<double>(cur.zFront)) / totalRange;

            // Subdivide the span's OPTICAL DEPTH, not its alpha directly.
            // A homogeneous medium of alpha a across the whole interval has
            // optical depth u = -ln(1-a); the piece covering a fraction
            // `ratio` of the interval carries u*ratio of it, so
            //
            //     alpha_front = 1 - e^(-u*ratio)
            //
            // which is algebraically the same 1 - (1-a)^ratio as before but
            // evaluated through log1p/expm1, so it stays accurate for thin
            // media instead of cancelling against 1.
            //
            // The identity that has to hold is that splitting a span
            // preserves its transmittance: 1 - (1-a_front)(1-a_back) == a.
            // Measured on a span split in half, relative error in that
            // identity, old float `pow` form vs this one:
            //     a = 1e-02   1.8e-06  ->  1.4e-08
            //     a = 1e-04   1.4e-04  ->  2.8e-09
            //     a = 1e-06   7.3e-02  ->  2.5e-08
            //     a = 1e-07   1.9e-01  ->  1.4e-08
            // Alphas that small are routine rather than exotic here:
            // DeepCBlur multiplies every gathered sample's alpha by its
            // kernel weight before calling this.
            //
            // Premultiplied colour scales with alpha — the transfer
            // equation's homogeneous solution is C = (j/sigma)*alpha, so a
            // piece keeps alpha_piece/a of it.  That ratio has a finite
            // limit as a -> 0 (the purely emissive case, where colour simply
            // splits by length), which the old `alpha > 1e-6 ? ... : 0` guard
            // discarded along with 100% of a thin span's colour: below that
            // threshold BOTH pieces came out black. This is the same
            // small-alpha cancellation the coincident merge below avoids, and
            // it is fixed the same way rather than with a magic epsilon.
            double alphaFrontD, alphaBackD, scaleFrontD, scaleBackD;
            const double a = (cur.alpha < 0.0f) ? 0.0
                           : (cur.alpha > 1.0f) ? 1.0
                           : static_cast<double>(cur.alpha);
            if (a >= 1.0) {
                // Opaque: every piece is opaque. Both keep the full colour,
                // which is right because only the front piece is ever
                // visible — the back sits behind an alpha-1 sample.
                alphaFrontD = alphaBackD = 1.0;
                scaleFrontD = scaleBackD = 1.0;
            } else if (a <= 0.0) {
                // Non-absorbing emissive limit: alpha stays 0 and the
                // emission divides by length.
                alphaFrontD = alphaBackD = 0.0;
                scaleFrontD = ratio;
                scaleBackD  = 1.0 - ratio;
            } else {
                const double u = -std::log1p(-a);
                alphaFrontD = -std::expm1(-u * ratio);
                alphaBackD  = -std::expm1(-u * (1.0 - ratio));
                scaleFrontD = alphaFrontD / a;
                scaleBackD  = alphaBackD  / a;
            }

            const float alphaFront = static_cast<float>(alphaFrontD);
            const float alphaBack  = static_cast<float>(alphaBackD);

            // Build back portion first (we'll overwrite cur for the front)
            SampleRecord back;
            back.zFront = z;
            back.zBack  = cur.zBack;
            back.alpha  = alphaBack;
            back.channels.resize(cur.channels.size());

            const float scaleFront = static_cast<float>(scaleFrontD);
            const float scaleBack  = static_cast<float>(scaleBackD);

            for (size_t c = 0; c < cur.channels.size(); ++c) {
                back.channels[c] = cur.channels[c] * scaleBack;
                cur.channels[c]  = cur.channels[c] * scaleFront;
            }

            cur.zBack  = z;
            cur.alpha  = alphaFront;

            samples.insert(samples.begin() + static_cast<long>(splitIdx + 1), std::move(back));
            changed = true;
            break;  // restart scan after structural change
        }
    }

    // --- Over-merge pass: collapse samples at identical [zFront, zBack] ---
    std::sort(samples.begin(), samples.end(),
        [](const SampleRecord& a, const SampleRecord& b) {
            return (a.zFront != b.zFront) ? a.zFront < b.zFront
                                          : a.zBack < b.zBack;
        });

    std::vector<SampleRecord> result;
    result.reserve(samples.size());

    // Scratch for the volumetric merge below, hoisted so a pixel pays at most
    // one allocation for it however many coincident groups it contains (and
    // none at all if it contains none).
    std::vector<double> acc;

    size_t i = 0;
    while (i < samples.size()) {
        size_t j = i + 1;
        while (j < samples.size() &&
               samples[j].zFront == samples[i].zFront &&
               samples[j].zBack  == samples[i].zBack)
        {
            ++j;
        }

        if (j - i == 1) {
            result.push_back(std::move(samples[i]));
        } else {
            const size_t nChan = samples[i].channels.size();
            SampleRecord merged;
            merged.zFront = samples[i].zFront;
            merged.zBack  = samples[i].zBack;
            merged.alpha  = 0.0f;
            merged.channels.resize(nChan, 0.0f);

            if (samples[i].zBack > samples[i].zFront) {
                // --- VOLUMETRIC group: co-located media, NOT stacked layers.
                //
                // Samples sharing an interval [zf,zb] with zb > zf are two
                // volumes occupying the same space, so neither is "in front"
                // of the other and `over` is the wrong composite: it is
                // order-dependent, and it biases the result toward whichever
                // sample the sort happened to place first.
                //
                // The right combination is the one the OpenEXR "Interpreting
                // Deep Pixels" note calls mergeOverlappingSamples: a uniform
                // medium of alpha a over the interval has optical depth
                // u = -ln(1-a), and co-located media ADD optical depth and
                // ADD emission.  Solving the transfer equation over the
                // combined medium gives
                //
                //     u     = sum_s u_s,          u_s   = -log1p(-a_s)
                //     alpha = 1 - e^-u            (== 1 - prod(1 - a_s),
                //                                  i.e. the same alpha `over`
                //                                  produces — only colour
                //                                  differs)
                //     C     = (sum_s C_s * u_s/a_s) * alpha/u
                //
                // which is order-independent and reproduces a direct ray
                // march through the media exactly.  This is also what Nuke's
                // own CombineOverlappingSamples does (measured: stock
                // DeepToImage with volumetric_composition on agrees to 7
                // decimals; with it off it reproduces the `over` form below).
                //
                // NOTE the alpha channel, when the caller carries alpha as an
                // ordinary channel too, comes out of this consistent with
                // `merged.alpha` for free: C_s = a_s makes its term u_s, so
                // the sum is u and the result is u * alpha/u == alpha.
                //
                // Both the optical depth (where the small-alpha cancellation
                // lives) and the colour sum accumulate in double: a float sum
                // drifts past the 2e-07 tolerance the coincident-sample gate
                // is stated at once a group holds ~5 or more samples
                // (measured 2.1e-07 at 5, 4.2e-07 at 40; in double it stays
                // under 2e-07 at every count).
                double u = 0.0;
                int    opaque = 0;
                acc.assign(nChan, 0.0);

                for (size_t s = i; s < j; ++s) {
                    const double a = (samples[s].alpha < 0.0f) ? 0.0
                                   : (samples[s].alpha > 1.0f) ? 1.0
                                   : static_cast<double>(samples[s].alpha);
                    const size_t nc = std::min(nChan, samples[s].channels.size());
                    if (a >= 1.0) {
                        ++opaque;
                        continue;                       // handled below
                    }
                    const double us = -std::log1p(-a);
                    // v = u/a is the sample's emission per unit optical
                    // depth; a -> 0 is the non-absorbing emissive limit
                    // v -> 1 (colour simply adds), which is also OpenEXR's
                    // guarded value.
                    const double v = (a > 0.0) ? us / a : 1.0;
                    u += us;
                    for (size_t c = 0; c < nc; ++c)
                        acc[c] += static_cast<double>(samples[s].channels[c]) * v;
                }

                if (opaque > 0) {
                    // An opaque member makes the whole interval opaque and
                    // swamps every finite-density member.  With several,
                    // none is in front, so they average — the u -> infinity
                    // limit of the formula above, and OpenEXR's own
                    // (c1 + c2) / 2 case.
                    std::fill(acc.begin(), acc.end(), 0.0);
                    for (size_t s = i; s < j; ++s) {
                        if (!(samples[s].alpha >= 1.0f)) continue;
                        const size_t nc = std::min(nChan, samples[s].channels.size());
                        for (size_t c = 0; c < nc; ++c)
                            acc[c] += static_cast<double>(samples[s].channels[c]);
                    }
                    merged.alpha = 1.0f;
                    for (size_t c = 0; c < nChan; ++c)
                        merged.channels[c] = static_cast<float>(acc[c] / opaque);
                } else {
                    const double alpha = -std::expm1(-u);
                    const double w     = (u > 0.0) ? alpha / u : 1.0;
                    merged.alpha = static_cast<float>(alpha);
                    for (size_t c = 0; c < nChan; ++c)
                        merged.channels[c] = static_cast<float>(acc[c] * w);
                }
            } else {
                // --- POINT group (zFront == zBack): genuine coincident
                // surfaces with an arbitrary but real ordering.  Stock
                // DeepToImage composites these one at a time with `over`
                // (measured: Nuke gives the same, order-dependent, answer
                // with volumetric_composition on OR off), and DeepCDefocus'
                // bit-exact point-sample parity gate depends on matching it,
                // so this path stays exactly as it was.
                float alphaAcc = 0.0f;
                for (size_t s = i; s < j; ++s) {
                    float w = 1.0f - alphaAcc;
                    if (w <= 0.0f) break;
                    const size_t nc = std::min(nChan, samples[s].channels.size());
                    for (size_t c = 0; c < nc; ++c)
                        merged.channels[c] += samples[s].channels[c] * w;
                    alphaAcc += samples[s].alpha * w;
                }
                merged.alpha = alphaAcc;
            }
            result.push_back(std::move(merged));
        }
        i = j;
    }

    samples = std::move(result);
}

// ---------------------------------------------------------------------------
// optimizeSamples — merge nearby-depth samples and cap total count
//
//   samples         : in/out vector of SampleRecords (modified in place)
//   mergeTolerance  : max Z-front distance for grouping (0 = no merge)
//   colorTolerance  : max channel-value distance for grouping (0 = Z-only)
//   maxSamples      : hard cap on output count (0 = unlimited)
//
// Merge uses front-to-back over-compositing:
//   alpha_acc += alpha_i * (1 - alpha_acc)
//   channel_acc += channel_i * (1 - alpha_acc_before)
//
// After merge, samples exceeding maxSamples are truncated (frontmost kept).
// ---------------------------------------------------------------------------
inline void optimizeSamples(std::vector<SampleRecord>& samples,
                            float mergeTolerance,
                            float colorTolerance,
                            int   maxSamples)
{
    if (samples.empty())
        return;

    // --- Overlap tidy pre-pass: split and merge overlapping intervals ---
    if (samples.size() > 1)
        tidyOverlapping(samples);

    // --- Sort by zFront ascending ---
    std::sort(samples.begin(), samples.end(),
        [](const SampleRecord& a, const SampleRecord& b) {
            return a.zFront < b.zFront;
        });

    const int count = static_cast<int>(samples.size());

    // --- Merge pass ---
    // Form groups of consecutive samples where Z-distance and color-distance
    // are within tolerance, then merge each group via over-compositing.
    if (mergeTolerance > 0.0f) {
        std::vector<SampleRecord> merged;
        merged.reserve(count);

        int groupStart = 0;
        while (groupStart < count) {
            int groupEnd = groupStart + 1;

            // Extend group while next sample is within tolerance of group start
            while (groupEnd < count) {
                bool zClose = (samples[groupEnd].zFront -
                               samples[groupStart].zFront) <= mergeTolerance;
                bool cClose = (colorTolerance <= 0.0f) ||
                    (colorDistance(samples[groupEnd].channels,
                                  samples[groupEnd].alpha,
                                  samples[groupStart].channels,
                                  samples[groupStart].alpha) <= colorTolerance);
                if (zClose && cClose)
                    ++groupEnd;
                else
                    break;
            }

            if (groupEnd - groupStart == 1) {
                // Single-sample group — pass through unchanged
                merged.push_back(std::move(samples[groupStart]));
            } else {
                // Multi-sample group — merge via front-to-back over-compositing
                const size_t nChan = samples[groupStart].channels.size();
                SampleRecord result;
                result.zFront =  1e30f;
                result.zBack  = -1e30f;
                result.alpha  = 0.0f;
                result.channels.resize(nChan, 0.0f);

                float alphaAcc = 0.0f;

                for (int s = groupStart; s < groupEnd; ++s) {
                    const SampleRecord& sr = samples[s];
                    const float w = 1.0f - alphaAcc;
                    if (w <= 0.0f)
                        break;

                    result.zFront = std::min(result.zFront, sr.zFront);
                    result.zBack  = std::max(result.zBack,  sr.zBack);

                    // Accumulate channels weighted by remaining coverage
                    const size_t nc = std::min(nChan, sr.channels.size());
                    for (size_t c = 0; c < nc; ++c)
                        result.channels[c] += sr.channels[c] * w;

                    alphaAcc += sr.alpha * w;
                }

                result.alpha = alphaAcc;
                merged.push_back(std::move(result));
            }

            groupStart = groupEnd;
        }

        samples = std::move(merged);
    }

    // --- Cap pass ---
    if (maxSamples > 0 && static_cast<int>(samples.size()) > maxSamples)
        samples.resize(maxSamples);
}

} // namespace deepc

#endif // DEEPC_DEEP_SAMPLE_OPTIMIZER_H
