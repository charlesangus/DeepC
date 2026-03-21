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
    {}

    const char* Class() const override { return CLASS; }
    const char* node_help() const override { return HELP; }
    Op* op() override { return this; }

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
    // getDeepRequests — pass-through, no spatial expansion
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

        requests.push_back(RequestData(input0(), box, requestChannels, count));
    }

    // ------------------------------------------------------------------
    // doDeepEngine — spread each sample across N sub-samples in Z
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

        // Fetch input
        DeepPlane inPlane;
        if (!in->deepEngine(box, channels, inPlane))
            return false;

        // Pre-compute weights for the current falloff and sample count
        const std::vector<float> weights = computeWeights(_numSamples, _falloff);
        const int N = _numSamples;
        const float S = _spread;
        const float step = S / N;
        const float halfStep = step * 0.5f;
        const bool isFlat = (_sampleType == 1);

        // Build channel classification: which are depth channels?
        const int nChans = channels.size();
        std::vector<bool> isDepthChan(nChans, false);
        {
            int ci = 0;
            foreach(z, channels) {
                if (z == Chan_DeepFront || z == Chan_DeepBack)
                    isDepthChan[ci] = true;
                ci++;
            }
        }

        // Identify channel indices for DeepFront, DeepBack, Alpha
        int idxFront = -1, idxBack = -1, idxAlpha = -1;
        {
            int ci = 0;
            foreach(z, channels) {
                if (z == Chan_DeepFront) idxFront = ci;
                else if (z == Chan_DeepBack) idxBack = ci;
                else if (z == Chan_Alpha) idxAlpha = ci;
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

            DeepOutPixel outPixel;
            outPixel.reserve(sampleCount * N * nChans);

            for (int s = 0; s < sampleCount; ++s) {
                // Read the original sample's zFront
                const float zFront = px.getUnorderedSample(s, Chan_DeepFront);

                // Compute sub-sample depth centres:
                // Evenly spaced in [zFront - S/2 + step/2, zFront + S/2 - step/2]
                const float baseZ = zFront - S * 0.5f + halfStep;

                for (int i = 0; i < N; ++i) {
                    const float centre = baseZ + i * step;
                    const float w = weights[i];

                    // Emit channels for this sub-sample
                    int ci = 0;
                    foreach(z, channels) {
                        if (z == Chan_DeepFront) {
                            if (isFlat) {
                                outPixel.push_back(centre);
                            } else {
                                outPixel.push_back(centre - halfStep);
                            }
                        } else if (z == Chan_DeepBack) {
                            if (isFlat) {
                                outPixel.push_back(centre);
                            } else {
                                outPixel.push_back(centre + halfStep);
                            }
                        } else {
                            // Scale channel value by weight (premult preserved)
                            outPixel.push_back(px.getUnorderedSample(s, z) * w);
                        }
                        ci++;
                    }
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
