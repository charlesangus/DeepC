// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Marten Blumen
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
// ---------------------------------------------------------------------------
inline float colorDistance(const std::vector<float>& a,
                           const std::vector<float>& b)
{
    const size_t n = std::min<size_t>(3, std::min(a.size(), b.size()));
    float d = 0.0f;
    for (size_t i = 0; i < n; ++i)
        d = std::max(d, std::fabs(a[i] - b[i]));
    return d;
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
                                  samples[groupStart].channels) <= colorTolerance);
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
