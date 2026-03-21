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
// tidyOverlapping — split overlapping depth intervals and over-merge
//
// Walks a depth-sorted sample list.  When sample[i].zBack > sample[i+1].zFront
// (overlap), the earlier volumetric sample is split at the overlap boundary.
// After all splits, samples at identical [zFront,zBack] are over-composited.
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
            SampleRecord& cur  = samples[i];
            const SampleRecord& nxt = samples[i + 1];

            // No overlap?
            if (cur.zBack <= nxt.zFront)
                continue;

            // Point sample — cannot be split; skip
            if (cur.zFront == cur.zBack)
                continue;

            // Split cur at z = nxt.zFront
            float z = nxt.zFront;
            float totalRange = cur.zBack - cur.zFront;
            float frontRange = z - cur.zFront;
            float ratio      = frontRange / totalRange;

            // Subdivide alpha: alpha_front = 1 - (1 - alpha)^ratio
            float oneMinusA = 1.0f - cur.alpha;
            float alphaFront = (oneMinusA <= 0.0f) ? cur.alpha
                             : 1.0f - std::pow(oneMinusA, ratio);
            float alphaBack  = (oneMinusA <= 0.0f) ? cur.alpha
                             : 1.0f - std::pow(oneMinusA, 1.0f - ratio);

            // Build back portion first (we'll overwrite cur for the front)
            SampleRecord back;
            back.zFront = z;
            back.zBack  = cur.zBack;
            back.alpha  = alphaBack;
            back.channels.resize(cur.channels.size());

            // Premultiplied channels scale proportionally with alpha
            float scaleFront = (cur.alpha > 1e-6f) ? alphaFront / cur.alpha : 0.0f;
            float scaleBack  = (cur.alpha > 1e-6f) ? alphaBack  / cur.alpha : 0.0f;

            for (size_t c = 0; c < cur.channels.size(); ++c) {
                back.channels[c] = cur.channels[c] * scaleBack;
                cur.channels[c]  = cur.channels[c] * scaleFront;
            }

            cur.zBack  = z;
            cur.alpha  = alphaFront;

            samples.insert(samples.begin() + static_cast<long>(i + 1), std::move(back));
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
            // Over-composite the group front-to-back
            const size_t nChan = samples[i].channels.size();
            SampleRecord merged;
            merged.zFront = samples[i].zFront;
            merged.zBack  = samples[i].zBack;
            merged.alpha  = 0.0f;
            merged.channels.resize(nChan, 0.0f);

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
