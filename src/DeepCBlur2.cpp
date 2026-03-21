// SPDX-License-Identifier: MIT
//
// ============================================================================
//
//  DeepCBlur2 — Gaussian blur for deep images with sample propagation
//
//  Applies a separable 2D Gaussian blur (horizontal → vertical) to deep image
//  data. Samples from neighbouring pixels are propagated into the output with
//  depth preserved (not blurred). Per-pixel sample optimization merges
//  nearby-depth samples after accumulation to keep sample counts manageable.
//
// ============================================================================

#include "DDImage/DeepFilterOp.h"
#include "DDImage/DeepPixel.h"
#include "DDImage/DeepPlane.h"
#include "DDImage/Knobs.h"

#include "DeepSampleOptimizer.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace DD::Image;

static const char* const CLASS = "DeepCBlur2";
static const char* const HELP =
    "Separable Gaussian blur for deep images.\n\n"
    "Applies a 2D Gaussian blur (horizontal then vertical) to all colour and "
    "alpha channels while propagating depth (deep.front / deep.back) from the "
    "source samples. Neighbouring pixels contribute weighted samples into each "
    "output pixel.\n\n"
    "Blur Size — Width and height can be set independently via the lock toggle. "
    "Sigma = radius / 3. Large values will be slow.\n\n"
    "Kernel Quality — Low (fast/approximate), Medium (normalized, default), "
    "High (CDF sub-pixel integration).\n\n"
    "Alpha Correction — Corrects alpha darkening caused by over-compositing "
    "blurred deep samples. Enable when the blur result appears too dark after "
    "compositing.\n\n"
    "Sample Optimization (twirldown) — Max samples cap, merge Z tolerance, "
    "and colour tolerance control per-pixel sample merging after blur.\n\n"
    "Part of the DeepC plugin collection.";

// ---------------------------------------------------------------------------
// Kernel quality tier names for Enumeration_knob
// ---------------------------------------------------------------------------
static const char* const kernelQualityNames[] = { "Low", "Medium", "High", nullptr };

// ---------------------------------------------------------------------------
// Gaussian kernel generation — three accuracy tiers
// Each returns a half-kernel of size (radius + 1). The kernel is symmetric:
//   full kernel = kernel[radius], ..., kernel[1], kernel[0], kernel[1], ..., kernel[radius]
// ---------------------------------------------------------------------------

// LQ: Raw unnormalized Gaussian — fast but does not sum to 1
static std::vector<float> getLQGaussianKernel(float sigma, int radius)
{
    std::vector<float> kernel(radius + 1);
    const float twoSigmaSq = 2.0f * sigma * sigma;
    const float norm = 1.0f / (sigma * std::sqrt(2.0f * static_cast<float>(M_PI)));
    for (int i = 0; i <= radius; ++i) {
        kernel[i] = std::exp(-static_cast<float>(i * i) / twoSigmaSq) * norm;
    }
    return kernel;
}

// MQ: Normalized Gaussian — same formula as LQ but rescaled so full kernel sums to 1.0
static std::vector<float> getMQGaussianKernel(float sigma, int radius)
{
    std::vector<float> kernel = getLQGaussianKernel(sigma, radius);
    // Full symmetric sum: center + 2 * sum(tails)
    float fullSum = kernel[0];
    for (int i = 1; i <= radius; ++i)
        fullSum += 2.0f * kernel[i];
    if (fullSum > 0.0f) {
        const float inv = 1.0f / fullSum;
        for (auto& v : kernel)
            v *= inv;
    }
    return kernel;
}

// HQ: CDF-based sub-pixel integration for maximum accuracy
static std::vector<float> getHQGaussianKernel(float sigma, int radius)
{
    const int kernelWidth = 2 * radius + 1;
    const float coverage = 3.5f * sigma;          // cover -3.5σ to +3.5σ
    const float step = (2.0f * coverage) / static_cast<float>(kernelWidth);
    const float sqrt2sigma = std::sqrt(2.0f) * sigma;

    // Build full kernel via CDF differences, then extract half
    std::vector<float> fullKernel(kernelWidth);
    for (int i = 0; i < kernelWidth; ++i) {
        const float lo = -coverage + static_cast<float>(i) * step;
        const float hi = lo + step;
        fullKernel[i] = 0.5f * (std::erff(hi / sqrt2sigma) - std::erff(lo / sqrt2sigma));
    }

    // Extract half-kernel (center + right side)
    std::vector<float> kernel(radius + 1);
    for (int i = 0; i <= radius; ++i)
        kernel[i] = fullKernel[radius + i];

    // Normalize so full symmetric kernel sums to 1.0
    float fullSum = kernel[0];
    for (int i = 1; i <= radius; ++i)
        fullSum += 2.0f * kernel[i];
    if (fullSum > 0.0f) {
        const float inv = 1.0f / fullSum;
        for (auto& v : kernel)
            v *= inv;
    }
    return kernel;
}

// ---------------------------------------------------------------------------
// Kernel dispatcher — selects tier based on quality enum value
// ---------------------------------------------------------------------------
static std::vector<float> computeKernel(float blur, int quality)
{
    const float sigma = std::max(blur / 3.0f, 0.001f);
    const int radius = std::max(0, static_cast<int>(std::ceil(blur)));
    if (radius == 0)
        return {1.0f};
    switch (quality) {
        case 0:  return getLQGaussianKernel(sigma, radius);
        case 2:  return getHQGaussianKernel(sigma, radius);
        default: return getMQGaussianKernel(sigma, radius);
    }
}

// ---------------------------------------------------------------------------
class DeepCBlur2 : public DeepFilterOp
{
    double _blurSize[2];    // width/height pixel radius pair
    bool   _alphaCorrection; // post-blur alpha darkening correction
    int   _kernelQuality;   // kernel accuracy tier: 0=Low, 1=Medium, 2=High
    int   _maxSamples;      // per-pixel sample cap (0 = unlimited)
    float _mergeTolerance;  // Z-front distance for sample merge
    float _colorTolerance;  // channel-value distance for sample merge

    // Thread-local scratch buffers for zero-allocation hot path
    struct ScratchBuf {
        std::vector<deepc::SampleRecord> samples;
        DeepOutPixel                      outPixel;
    };

    // Compute kernel radius from blur parameter (blur = 3-sigma radius)
    static int kernelRadius(float blur) {
        return std::max(0, static_cast<int>(std::ceil(blur)));
    }

public:
    DeepCBlur2(Node* node) : DeepFilterOp(node),
        _blurSize{1.0, 1.0},
        _alphaCorrection(false),
        _kernelQuality(1),
        _maxSamples(100),
        _mergeTolerance(0.001f),
        _colorTolerance(0.01f)
    {}

    const char* Class() const override { return CLASS; }
    const char* node_help() const override { return HELP; }
    Op* op() override { return this; }

    // ------------------------------------------------------------------
    // Knobs
    // ------------------------------------------------------------------
    void knobs(Knob_Callback f) override
    {
        WH_knob(f, _blurSize, "blur_size", "blur size");
        SetRange(f, 0.0, 100.0);
        Tooltip(f, "Blur radius in pixels (sigma = radius / 3). Width and height "
                    "can be set independently using the lock toggle.");

        Enumeration_knob(f, &_kernelQuality, kernelQualityNames, "kernel_quality", "kernel quality");
        Tooltip(f, "Gaussian kernel accuracy tier. Low = fast/approximate, "
                    "Medium = normalized (default), High = CDF sub-pixel integration.");

        Bool_knob(f, &_alphaCorrection, "alpha_correction", "alpha correction");
        Tooltip(f, "Correct alpha darkening caused by over-compositing blurred deep samples. "
                    "Enable when the blur result appears too dark after compositing.");

        BeginClosedGroup(f, "Sample Optimization");

        Int_knob(f, &_maxSamples, "max_samples", "max samples");
        SetRange(f, 0, 500);
        Tooltip(f, "Maximum samples per output pixel after optimization. "
                    "0 = unlimited.");

        Float_knob(f, &_mergeTolerance, "merge_tolerance", "merge Z tolerance");
        SetRange(f, 0.0f, 1.0f);
        Tooltip(f, "Maximum Z-front distance to merge accumulated samples. "
                    "0 = no merging.");

        Float_knob(f, &_colorTolerance, "color_tolerance", "color tolerance");
        SetRange(f, 0.0f, 0.1f);
        Tooltip(f, "Maximum per-channel colour difference for sample merge. "
                    "0 = merge by Z only.");

        EndGroup(f);
    }

    // ------------------------------------------------------------------
    // _validate — expand bounding box by kernel radius
    // ------------------------------------------------------------------
    void _validate(bool for_real) override
    {
        DeepFilterOp::_validate(for_real);

        const int radX = kernelRadius(static_cast<float>(_blurSize[0]));
        const int radY = kernelRadius(static_cast<float>(_blurSize[1]));

        if (radX > 0 || radY > 0) {
            Box box = _deepInfo.box();
            _deepInfo.box().set(box.x() - radX,
                                box.y() - radY,
                                box.r() + radX,
                                box.t() + radY);
        }
    }

    // ------------------------------------------------------------------
    // getDeepRequests — request padded input region
    // ------------------------------------------------------------------
    void getDeepRequests(Box box, const ChannelSet& channels, int count,
                         std::vector<RequestData>& requests) override
    {
        if (!input0())
            return;

        const int radX = kernelRadius(static_cast<float>(_blurSize[0]));
        const int radY = kernelRadius(static_cast<float>(_blurSize[1]));

        Box padded(box.x() - radX,
                   box.y() - radY,
                   box.r() + radX,
                   box.t() + radY);

        ChannelSet requestChannels = channels;
        requestChannels += Chan_DeepFront;
        requestChannels += Chan_DeepBack;
        requestChannels += Chan_Alpha;

        requests.push_back(RequestData(input0(), padded, requestChannels, count));
    }

    // ------------------------------------------------------------------
    // doDeepEngine — separable H→V Gaussian blur with sample propagation
    // ------------------------------------------------------------------
    bool doDeepEngine(Box box, const ChannelSet& channels,
                      DeepOutputPlane& plane) override
    {
        DeepOp* in = input0();
        if (!in)
            return true;

        const float blurW = static_cast<float>(_blurSize[0]);
        const float blurH = static_cast<float>(_blurSize[1]);
        const int radX = kernelRadius(blurW);
        const int radY = kernelRadius(blurH);

        // If zero blur, pass through
        if (radX == 0 && radY == 0) {
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

        // Fetch padded input region
        Box inputBox(box.x() - radX,
                     box.y() - radY,
                     box.r() + radX,
                     box.t() + radY);

        DeepPlane inPlane;
        if (!in->deepEngine(inputBox, channels, inPlane))
            return false;

        // Compute 1D half-kernels for separable passes
        const auto kernelH = computeKernel(blurW, _kernelQuality);
        const auto kernelV = computeKernel(blurH, _kernelQuality);

        // Determine channel layout — identify which channels are depth vs data
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

        // ---------------------------------------------------------------
        // HORIZONTAL PASS: gather along X into intermediate buffer
        //
        // Intermediate buffer dimensions:
        //   height = inputBox height (padded Y range, needed for V pass)
        //   width  = output box width (only output columns are needed)
        // ---------------------------------------------------------------
        const int intW = box.r() - box.x();
        const int intH = inputBox.t() - inputBox.y();

        // Coordinate translation helpers
        auto intX = [&](int worldX) { return worldX - box.x(); };
        auto intY = [&](int worldY) { return worldY - inputBox.y(); };

        std::vector<std::vector<std::vector<deepc::SampleRecord>>> intermediateBuffer(
            intH, std::vector<std::vector<deepc::SampleRecord>>(intW));

        const Box& inBox = inPlane.box();

        for (int srcY = inputBox.y(); srcY < inputBox.t(); ++srcY) {
            if (srcY < inBox.y() || srcY >= inBox.t())
                continue;

            for (int outX = box.x(); outX < box.r(); ++outX) {
                if (Op::aborted())
                    return false;

                auto& destSamples = intermediateBuffer[intY(srcY)][intX(outX)];

                // Gather from horizontal neighbourhood
                for (int dx = -radX; dx <= radX; ++dx) {
                    const int srcX = outX + dx;
                    if (srcX < inBox.x() || srcX >= inBox.r())
                        continue;

                    DeepPixel srcPixel = inPlane.getPixel(srcY, srcX);
                    const int srcSamples = static_cast<int>(srcPixel.getSampleCount());
                    if (srcSamples == 0)
                        continue;

                    const float weight = kernelH[std::abs(dx)];
                    if (weight <= 0.0f)
                        continue;

                    for (int s = 0; s < srcSamples; ++s) {
                        deepc::SampleRecord rec;
                        rec.zFront = srcPixel.getUnorderedSample(s, Chan_DeepFront);
                        rec.zBack  = srcPixel.getUnorderedSample(s, Chan_DeepBack);
                        rec.alpha  = srcPixel.getUnorderedSample(s, Chan_Alpha) * weight;

                        rec.channels.resize(nChans);
                        int ci = 0;
                        foreach(z, channels) {
                            if (isDepthChan[ci]) {
                                rec.channels[ci] = srcPixel.getUnorderedSample(s, z);
                            } else {
                                rec.channels[ci] = srcPixel.getUnorderedSample(s, z) * weight;
                            }
                            ci++;
                        }

                        destSamples.push_back(std::move(rec));
                    }
                }
            }
        }

        // ---------------------------------------------------------------
        // VERTICAL PASS: gather along Y from intermediate → output
        //
        // For each output pixel (outX, outY), gather from intermediate
        // rows outY-radY..outY+radY, weighting by kernelV. The total
        // weight per sample is H_weight × V_weight (separable property).
        // optimizeSamples runs only after this final pass.
        // ---------------------------------------------------------------
        static thread_local ScratchBuf scratch;

        plane = DeepOutputPlane(channels, box, DeepPixel::eUnordered);

        for (Box::iterator it = box.begin(); it != box.end(); ++it) {
            if (Op::aborted())
                return false;

            const int outX = it.x;
            const int outY = it.y;

            scratch.samples.clear();

            for (int dy = -radY; dy <= radY; ++dy) {
                const int srcY = outY + dy;
                if (srcY < inputBox.y() || srcY >= inputBox.t())
                    continue;

                const int iy = intY(srcY);
                const int ix = intX(outX);
                if (ix < 0 || ix >= intW || iy < 0 || iy >= intH)
                    continue;

                const auto& hSamples = intermediateBuffer[iy][ix];
                if (hSamples.empty())
                    continue;

                const float weight = kernelV[std::abs(dy)];
                if (weight <= 0.0f)
                    continue;

                for (const auto& hRec : hSamples) {
                    deepc::SampleRecord rec;
                    rec.zFront = hRec.zFront;
                    rec.zBack  = hRec.zBack;
                    rec.alpha  = hRec.alpha * weight;

                    rec.channels.resize(nChans);
                    for (int ci = 0; ci < nChans; ++ci) {
                        if (isDepthChan[ci]) {
                            rec.channels[ci] = hRec.channels[ci];
                        } else {
                            rec.channels[ci] = hRec.channels[ci] * weight;
                        }
                    }

                    scratch.samples.push_back(std::move(rec));
                }
            }

            // Empty pixel → hole
            if (scratch.samples.empty()) {
                plane.addHole();
                continue;
            }

            // Per-pixel sample optimization (only after V pass)
            deepc::optimizeSamples(scratch.samples,
                                   _mergeTolerance,
                                   _colorTolerance,
                                   _maxSamples);

            // Alpha darkening correction — undo over-composite darkening
            if (_alphaCorrection && scratch.samples.size() > 1) {
                float cumTransp = 1.0f;
                for (auto& sr : scratch.samples) {
                    if (cumTransp > 1e-6f) {
                        const float inv = 1.0f / cumTransp;
                        for (int ci = 0; ci < nChans; ++ci) {
                            if (!isDepthChan[ci])
                                sr.channels[ci] *= inv;
                        }
                        sr.alpha *= inv;
                    }
                    cumTransp *= std::max(0.0f, 1.0f - sr.alpha);
                }
            }

            // Emit output samples
            scratch.outPixel.clear();
            scratch.outPixel.reserve(static_cast<int>(scratch.samples.size()) * nChans);

            for (const auto& sr : scratch.samples) {
                int ci = 0;
                foreach(z, channels) {
                    if (z == Chan_DeepFront)
                        scratch.outPixel.push_back(sr.zFront);
                    else if (z == Chan_DeepBack)
                        scratch.outPixel.push_back(sr.zBack);
                    else
                        scratch.outPixel.push_back(sr.channels[ci]);
                    ci++;
                }
            }

            plane.addPixel(scratch.outPixel);
        }

        return true;
    }

    static const Op::Description d;
};

// ---------------------------------------------------------------------------
static Op* build(Node* node) { return new DeepCBlur2(node); }
const Op::Description DeepCBlur2::d(::CLASS, "Deep/DeepCBlur2", build);
