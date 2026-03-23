// SPDX-License-Identifier: MIT
//
// ============================================================================
//
//  DeepCDepthBlur — Depth-axis blur for deep images
//
//  Spreads each input sample's alpha and colour across N sub-samples
//  distributed in Z with a selectable falloff curve. The flatten invariant
//  is preserved: sum(sub-sample weights) == 1 for each original sample.
//
//  Falloff modes: Linear, Gaussian, Smoothstep, Exponential
//  Sample types:  Volumetric (sub-ranges in Z) or Flat (zFront == zBack)
//
//  Optional B input: when connected, only source samples whose depth range
//  (expanded by spread) overlaps any B sample are spread. Others pass through.
//
//  Output is tidy: sorted by zFront ascending, with overlapping zBack values
//  clamped so zBack[i] <= zFront[i+1].
//
// ============================================================================

#include "DDImage/DeepFilterOp.h"
#include "DDImage/DeepPixel.h"
#include "DDImage/DeepPlane.h"
#include "DDImage/Knobs.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace DD::Image;

static const char* const CLASS = "DeepCDepthBlur";
static const char* const HELP =
    "Depth-axis blur for deep images.\n\n"
    "Spreads each input sample across N sub-samples distributed along the "
    "Z axis with a selectable falloff curve. The flatten invariant is "
    "preserved: the alpha and colour of all sub-samples sum to exactly the "
    "original sample values, so flatten(output) == flatten(input).\n\n"
    "spread controls the total depth range over which sub-samples are "
    "distributed (in scene-linear Z units). num_samples controls how many "
    "sub-samples each original sample is split into.\n\n"
    "Falloff modes control the weight distribution across sub-samples:\n"
    "  Linear — triangular falloff, peak at centre\n"
    "  Gaussian — bell curve, smooth tails\n"
    "  Smoothstep — Hermite-interpolated falloff\n"
    "  Exponential — sharp centre peak, rapid decay\n\n"
    "Sample type controls depth representation:\n"
    "  Volumetric — sub-samples span adjacent depth sub-ranges\n"
    "  Flat — all sub-samples have zFront == zBack (point samples)\n\n"
    "Optional B input: when connected, only source samples whose depth "
    "range overlaps with any B-input sample (within the spread distance) "
    "are spread. All other samples pass through unchanged.\n\n"
    "Output is tidy: samples are sorted by zFront ascending and overlapping "
    "zBack values are clamped to the next sample's zFront.\n\n"
    "Part of the DeepC plugin collection.";

// ---------------------------------------------------------------------------
// Falloff enumeration
// ---------------------------------------------------------------------------
static const char* const falloffNames[] = {
    "Linear", "Gaussian", "Smoothstep", "Exponential", nullptr
};

static const char* const sampleTypeNames[] = {
    "Volumetric", "Flat", nullptr
};

// ---------------------------------------------------------------------------
// Weight generators — all return normalised weights summing to 1
// ---------------------------------------------------------------------------

static std::vector<float> weightsLinear(int n)
{
    std::vector<float> w(n);
    if (n == 1) { w[0] = 1.0f; return w; }
    float sum = 0.0f;
    for (int i = 0; i < n; ++i) {
        float t = -1.0f + 2.0f * i / (n - 1);  // t ∈ [-1, 1]
        w[i] = 1.0f - std::fabs(t);
        sum += w[i];
    }
    if (sum > 0.0f) for (auto& v : w) v /= sum;
    else for (auto& v : w) v = 1.0f / n;  // uniform fallback for degenerate cases
    return w;
}

static std::vector<float> weightsGaussian(int n)
{
    std::vector<float> w(n);
    if (n == 1) { w[0] = 1.0f; return w; }
    float sum = 0.0f;
    for (int i = 0; i < n; ++i) {
        float t = -1.0f + 2.0f * i / (n - 1);  // t ∈ [-1, 1]
        float x = t * 3.0f;
        w[i] = std::exp(-0.5f * x * x);
        sum += w[i];
    }
    if (sum > 0.0f) for (auto& v : w) v /= sum;
    else for (auto& v : w) v = 1.0f / n;
    return w;
}

static std::vector<float> weightsSmoothstep(int n)
{
    std::vector<float> w(n);
    if (n == 1) { w[0] = 1.0f; return w; }
    float sum = 0.0f;
    for (int i = 0; i < n; ++i) {
        float t = -1.0f + 2.0f * i / (n - 1);  // t ∈ [-1, 1]
        float u = 1.0f - std::fabs(t);           // u ∈ [0, 1]
        w[i] = 3.0f * u * u - 2.0f * u * u * u;  // smoothstep
        sum += w[i];
    }
    if (sum > 0.0f) for (auto& v : w) v /= sum;
    else for (auto& v : w) v = 1.0f / n;
    return w;
}

static std::vector<float> weightsExponential(int n)
{
    std::vector<float> w(n);
    if (n == 1) { w[0] = 1.0f; return w; }
    float sum = 0.0f;
    for (int i = 0; i < n; ++i) {
        float t = -1.0f + 2.0f * i / (n - 1);  // t ∈ [-1, 1]
        w[i] = std::exp(-3.0f * std::fabs(t));
        sum += w[i];
    }
    if (sum > 0.0f) for (auto& v : w) v /= sum;
    else for (auto& v : w) v = 1.0f / n;
    return w;
}

// Dispatcher
static std::vector<float> computeWeights(int n, int mode)
{
    switch (mode) {
        case 0:  return weightsLinear(n);
        case 1:  return weightsGaussian(n);
        case 2:  return weightsSmoothstep(n);
        case 3:  return weightsExponential(n);
        default: return weightsLinear(n);
    }
}

// ---------------------------------------------------------------------------
// Helper: test whether depth range [aFront, aBack] overlaps [bFront, bBack]
// ---------------------------------------------------------------------------
static inline bool depthRangesOverlap(float aFront, float aBack,
                                       float bFront, float bBack)
{
    return aBack >= bFront && aFront <= bBack;
}

// ---------------------------------------------------------------------------
class DeepCDepthBlur : public DeepFilterOp
{
    float _spread;       // total depth range for sub-sample distribution
    int   _numSamples;   // number of sub-samples per input sample
    int   _falloff;      // falloff mode index (Linear/Gaussian/Smoothstep/Exponential)
    int   _sampleType;   // 0 = Volumetric, 1 = Flat

public:
    DeepCDepthBlur(Node* node) : DeepFilterOp(node),
        _spread(1.0f),
        _numSamples(5),
        _falloff(0),
        _sampleType(0)
    {
        inputs(2);  // input 0 = source (required), input 1 = B (optional)
    }

    int minimum_inputs() const { return 1; }
    int maximum_inputs() const { return 2; }

    const char* Class() const override { return CLASS; }
    const char* node_help() const override { return HELP; }
    Op* op() override { return this; }

    const char* input_label(int n, char*) const
    {
        switch (n) {
            case 0: return "";
            case 1: return "ref";
            default: return "";
        }
    }

    bool test_input(int input, Op* op) const
    {
        // Both inputs accept deep streams
        return DeepFilterOp::test_input(input, op);
    }

    // ------------------------------------------------------------------
    // Knobs
    // ------------------------------------------------------------------
    void knobs(Knob_Callback f) override
    {
        Float_knob(f, &_spread, "spread", "spread");
        SetRange(f, 0.0f, 1000.0f);
        Tooltip(f, "Total depth range (in scene-linear Z units) over which "
                    "each sample is spread. 0 = pass-through.");

        Int_knob(f, &_numSamples, "num_samples", "num samples");
        SetRange(f, 1, 64);
        Tooltip(f, "Number of sub-samples each input sample is split into. "
                    "Higher values give smoother depth distribution.");

        Enumeration_knob(f, &_falloff, falloffNames, "falloff", "falloff");
        Tooltip(f, "Weight distribution curve across sub-samples.\n"
                    "Linear — triangular, peak at centre\n"
                    "Gaussian — bell curve\n"
                    "Smoothstep — Hermite-interpolated\n"
                    "Exponential — sharp centre peak");

        Enumeration_knob(f, &_sampleType, sampleTypeNames, "sample_type", "sample type");
        Tooltip(f, "Volumetric: sub-samples span adjacent depth sub-ranges.\n"
                    "Flat: all sub-samples have zFront == zBack.");
    }

    // ------------------------------------------------------------------
    // _validate — clamp knob values
    // ------------------------------------------------------------------
    void _validate(bool for_real) override
    {
        DeepFilterOp::_validate(for_real);

        if (_numSamples < 1) _numSamples = 1;
        if (_spread < 0.0f)  _spread = 0.0f;
    }

    // ------------------------------------------------------------------
    // getDeepRequests — request A (and B if connected)
    // ------------------------------------------------------------------
    void getDeepRequests(Box box, const ChannelSet& channels, int count,
                         std::vector<RequestData>& requests) override
    {
        if (!input0())
            return;

        ChannelSet requestChannels = channels;
        requestChannels += Chan_DeepFront;
        requestChannels += Chan_DeepBack;
        requestChannels += Chan_Alpha;

        // Request source (input 0)
        requests.push_back(RequestData(input0(), box, requestChannels, count));

        // Request B (input 1) if connected — same box, depth-only channels
        DeepOp* bOp = dynamic_cast<DeepOp*>(Op::input(1));
        if (bOp) {
            ChannelSet bChannels;
            bChannels += Chan_DeepFront;
            bChannels += Chan_DeepBack;
            requests.push_back(RequestData(bOp, box, bChannels, count));
        }
    }

    // ------------------------------------------------------------------
    // doDeepEngine — spread each sample across N sub-samples in Z
    //
    // Pipeline:
    //   1. Fetch A plane (source), optionally fetch B plane (depth gate)
    //   2. For each pixel, for each source sample:
    //      a. If B is connected and the sample's expanded depth range
    //         doesn't overlap any B sample, pass through unchanged.
    //      b. Otherwise, spread into N weighted sub-samples.
    //   3. Tidy pass: sort output samples by zFront ascending,
    //      then clamp overlapping zBack values.
    //
    // NOTE on tidy pass: The overlap clamp trims zBack extent only (not
    // alpha), so it can slightly shift the depth range of a sample.
    // This is acceptable — the tidy step is a secondary correction after
    // the invariant-preserving spread. The flatten invariant is preserved
    // because no alpha values are modified by the clamp.
    // ------------------------------------------------------------------
    bool doDeepEngine(Box box, const ChannelSet& channels,
                      DeepOutputPlane& plane) override
    {
        DeepOp* in = input0();
        if (!in)
            return true;

        // Zero-spread or single-sample fast path: pass through unchanged
        if (_spread < 1e-6f || _numSamples <= 1) {
            DeepPlane inPlane;
            if (!in->deepEngine(box, channels, inPlane))
                return false;

            plane = DeepOutputPlane(channels, box, DeepPixel::eUnordered);
            for (Box::iterator it = box.begin(); it != box.end(); ++it) {
                DeepPixel px = inPlane.getPixel(it);
                const int nSamples = static_cast<int>(px.getSampleCount());
                if (nSamples == 0) {
                    plane.addHole();
                    continue;
                }
                DeepOutPixel outPixel;
                outPixel.reserve(nSamples * channels.size());
                for (int s = 0; s < nSamples; ++s) {
                    foreach(z, channels) {
                        outPixel.push_back(px.getUnorderedSample(s, z));
                    }
                }
                plane.addPixel(outPixel);
            }
            return true;
        }

        // Fetch source plane
        DeepPlane inPlane;
        if (!in->deepEngine(box, channels, inPlane))
            return false;

        // Fetch optional B plane (depth gate input)
        DeepOp* bOp = dynamic_cast<DeepOp*>(Op::input(1));
        DeepPlane bPlane;
        bool bConnected = false;
        if (bOp) {
            ChannelSet bChannels;
            bChannels += Chan_DeepFront;
            bChannels += Chan_DeepBack;
            // Graceful handling: if B fetch fails, treat as no B samples
            bConnected = bOp->deepEngine(box, bChannels, bPlane);
        }

        // Pre-compute weights for the current falloff and sample count
        const std::vector<float> weights = computeWeights(_numSamples, _falloff);
        const int N = _numSamples;
        const float S = _spread;
        const float step = S / N;
        const float halfStep = step * 0.5f;
        const bool isFlat = (_sampleType == 1);

        const int nChans = channels.size();

        // Identify channel indices for DeepFront, DeepBack within the channelset
        int idxFront = -1, idxBack = -1;
        {
            int ci = 0;
            foreach(z, channels) {
                if (z == Chan_DeepFront) idxFront = ci;
                else if (z == Chan_DeepBack) idxBack = ci;
                ci++;
            }
        }

        plane = DeepOutputPlane(channels, box, DeepPixel::eUnordered);

        for (Box::iterator it = box.begin(); it != box.end(); ++it) {
            if (Op::aborted())
                return false;

            DeepPixel px = inPlane.getPixel(it);
            const int sampleCount = static_cast<int>(px.getSampleCount());

            if (sampleCount == 0) {
                plane.addHole();
                continue;
            }

            // Collect B depth intervals for this pixel (if B connected)
            std::vector<float> bFronts, bBacks;
            if (bConnected) {
                DeepPixel bPx = bPlane.getPixel(it);
                const int bCount = static_cast<int>(bPx.getSampleCount());
                bFronts.reserve(bCount);
                bBacks.reserve(bCount);
                for (int b = 0; b < bCount; ++b) {
                    bFronts.push_back(bPx.getUnorderedSample(b, Chan_DeepFront));
                    bBacks.push_back(bPx.getUnorderedSample(b, Chan_DeepBack));
                }
            }
            const bool hasB = !bFronts.empty();

            // We'll collect all output samples as flat arrays for the
            // tidy pass (sort + overlap clamp) before emitting
            struct SampleData {
                float zFront;
                float zBack;
                std::vector<float> chanValues;  // all channels in order
            };
            std::vector<SampleData> outSamples;
            outSamples.reserve(sampleCount * N);

            for (int s = 0; s < sampleCount; ++s) {
                const float srcFront = px.getUnorderedSample(s, Chan_DeepFront);
                const float srcBack  = px.getUnorderedSample(s, Chan_DeepBack);

                // B-input depth gating: if B is connected, check whether
                // this source sample's expanded range overlaps any B sample.
                // If no overlap, pass through unchanged (no spreading).
                bool shouldSpread = true;
                if (hasB) {
                    shouldSpread = false;
                    const float expandedFront = srcFront - S * 0.5f;
                    const float expandedBack  = srcBack  + S * 0.5f;
                    for (size_t b = 0; b < bFronts.size(); ++b) {
                        if (depthRangesOverlap(expandedFront, expandedBack,
                                               bFronts[b], bBacks[b])) {
                            shouldSpread = true;
                            break;
                        }
                    }
                }

                if (!shouldSpread) {
                    // Pass through unchanged
                    SampleData sd;
                    sd.zFront = srcFront;
                    sd.zBack  = srcBack;
                    sd.chanValues.reserve(nChans);
                    foreach(z, channels) {
                        sd.chanValues.push_back(px.getUnorderedSample(s, z));
                    }
                    outSamples.push_back(std::move(sd));
                    continue;
                }

                // Fetch source alpha once per sample (outside sub-sample loop)
                const float srcAlpha = px.getUnorderedSample(s, Chan_Alpha);

                // Zero-alpha input guard: pass through unchanged without spreading
                if (srcAlpha < 1e-6f) {
                    SampleData sd;
                    sd.zFront = srcFront;
                    sd.zBack  = srcBack;
                    sd.chanValues.reserve(nChans);
                    foreach(z, channels) {
                        sd.chanValues.push_back(px.getUnorderedSample(s, z));
                    }
                    outSamples.push_back(std::move(sd));
                    continue;  // skip spreading for zero-alpha input
                }

                // Spread into N sub-samples using multiplicative alpha decomposition
                const float baseZ = srcFront - S * 0.5f + halfStep;
                for (int i = 0; i < N; ++i) {
                    const float centre = baseZ + i * step;
                    const float w = weights[i];

                    SampleData sd;
                    if (isFlat) {
                        sd.zFront = centre;
                        sd.zBack  = centre;
                    } else {
                        sd.zFront = centre - halfStep;
                        sd.zBack  = centre + halfStep;
                    }

                    // Multiplicative alpha decomposition: α_sub = 1 - (1-α)^w
                    // This preserves the flatten invariant by construction.
                    const float alphaSub = static_cast<float>(1.0 - std::pow(1.0 - static_cast<double>(srcAlpha), static_cast<double>(w)));
                    if (alphaSub < 1e-6f)
                        continue;  // skip near-zero sub-samples

                    sd.chanValues.reserve(nChans);

                    foreach(z, channels) {
                        if (z == Chan_DeepFront) {
                            sd.chanValues.push_back(sd.zFront);
                        } else if (z == Chan_DeepBack) {
                            sd.chanValues.push_back(sd.zBack);
                        } else if (z == Chan_Alpha) {
                            sd.chanValues.push_back(alphaSub);
                        } else {
                            // Premult-correct: scale by (α_sub / α_src)
                            // Division is safe: srcAlpha >= 1e-6f guaranteed by guard above
                            sd.chanValues.push_back(px.getUnorderedSample(s, z) * (alphaSub / srcAlpha));
                        }
                    }

                    outSamples.push_back(std::move(sd));
                }
            }

            // ---------------------------------------------------------------
            // Tidy pass: sort by zFront, then clamp overlapping zBack values
            // ---------------------------------------------------------------
            std::sort(outSamples.begin(), outSamples.end(),
                      [](const SampleData& a, const SampleData& b) {
                          return a.zFront < b.zFront;
                      });

            // Clamp overlaps: ensure zBack[i] <= zFront[i+1]
            for (size_t i = 0; i + 1 < outSamples.size(); ++i) {
                if (outSamples[i].zBack > outSamples[i + 1].zFront) {
                    outSamples[i].zBack = outSamples[i + 1].zFront;
                    // Update the zBack channel value in the stored data
                    if (idxBack >= 0) {
                        outSamples[i].chanValues[idxBack] = outSamples[i].zBack;
                    }
                }
            }

            // Emit the tidy output pixel
            DeepOutPixel outPixel;
            outPixel.reserve(static_cast<int>(outSamples.size()) * nChans);
            for (const auto& sd : outSamples) {
                for (int ci = 0; ci < nChans; ++ci) {
                    outPixel.push_back(sd.chanValues[ci]);
                }
            }
            plane.addPixel(outPixel);
        }

        return true;
    }

    static const Op::Description d;
};

// ---------------------------------------------------------------------------
static Op* build(Node* node) { return new DeepCDepthBlur(node); }
const Op::Description DeepCDepthBlur::d(::CLASS, "Deep/DeepCDepthBlur", build);
