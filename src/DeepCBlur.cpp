// SPDX-License-Identifier: MIT
//
// ============================================================================
//
//  DeepCBlur — Gaussian blur for deep images with sample propagation
//
//  Applies a 2D Gaussian blur to deep image data. Samples from neighbouring
//  pixels are propagated into the output with depth preserved (not blurred).
//  Per-pixel sample optimization merges nearby-depth samples after
//  accumulation to keep sample counts manageable.
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

static const char* const CLASS = "DeepCBlur";
static const char* const HELP =
    "Gaussian blur for deep images.\n\n"
    "Applies a 2D Gaussian blur kernel to all colour and alpha channels while "
    "propagating depth (deep.front / deep.back) from the source samples. "
    "Neighbouring pixels contribute weighted samples into each output pixel, "
    "and per-pixel sample optimization merges nearby-depth samples to keep "
    "counts manageable.\n\n"
    "blur_width / blur_height control the pixel radius of the kernel in each "
    "direction (sigma = radius / 3). Values above 10 will be slow due to "
    "large kernel footprints.\n\n"
    "Part of the DeepC plugin collection.";

// ---------------------------------------------------------------------------
class DeepCBlur : public DeepFilterOp
{
    float _blurWidth;       // pixel radius in X
    float _blurHeight;      // pixel radius in Y
    int   _maxSamples;      // per-pixel sample cap (0 = unlimited)
    float _mergeTolerance;  // Z-front distance for sample merge
    float _colorTolerance;  // channel-value distance for sample merge

    // Thread-local scratch buffers for zero-allocation hot path
    struct ScratchBuf {
        std::vector<deepc::SampleRecord> samples;
        DeepOutPixel                      outPixel;
        std::vector<float>                kernel;
    };

    // Compute kernel radius from blur parameter (blur = 3-sigma radius)
    static int kernelRadius(float blur) {
        return std::max(0, static_cast<int>(std::ceil(blur)));
    }

public:
    DeepCBlur(Node* node) : DeepFilterOp(node),
        _blurWidth(1.0f),
        _blurHeight(1.0f),
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
        Float_knob(f, &_blurWidth, "blur_width", "blur width");
        SetRange(f, 0.0f, 100.0f);
        Tooltip(f, "Horizontal blur radius in pixels (sigma = radius / 3). "
                    "Values above 10 will be slow.");

        Float_knob(f, &_blurHeight, "blur_height", "blur height");
        SetRange(f, 0.0f, 100.0f);
        Tooltip(f, "Vertical blur radius in pixels (sigma = radius / 3). "
                    "Values above 10 will be slow.");

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
    }

    // ------------------------------------------------------------------
    // _validate — expand bounding box by kernel radius
    // ------------------------------------------------------------------
    void _validate(bool for_real) override
    {
        DeepFilterOp::_validate(for_real);

        const int radX = kernelRadius(_blurWidth);
        const int radY = kernelRadius(_blurHeight);

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

        const int radX = kernelRadius(_blurWidth);
        const int radY = kernelRadius(_blurHeight);

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
    // doDeepEngine — apply 2D Gaussian blur with sample propagation
    // ------------------------------------------------------------------
    bool doDeepEngine(Box box, const ChannelSet& channels,
                      DeepOutputPlane& plane) override
    {
        DeepOp* in = input0();
        if (!in)
            return true;

        const int radX = kernelRadius(_blurWidth);
        const int radY = kernelRadius(_blurHeight);

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

        // Pre-compute 2D Gaussian kernel
        const float sigmaX = std::max(_blurWidth  / 3.0f, 0.001f);
        const float sigmaY = std::max(_blurHeight / 3.0f, 0.001f);
        const int kernelW = 2 * radX + 1;
        const int kernelH = 2 * radY + 1;

        static thread_local ScratchBuf scratch;
        scratch.kernel.resize(kernelW * kernelH);

        float kernelSum = 0.0f;
        for (int dy = -radY; dy <= radY; ++dy) {
            for (int dx = -radX; dx <= radX; ++dx) {
                const float gx = std::exp(-0.5f * (dx * dx) / (sigmaX * sigmaX));
                const float gy = std::exp(-0.5f * (dy * dy) / (sigmaY * sigmaY));
                const float val = gx * gy;
                scratch.kernel[(dy + radY) * kernelW + (dx + radX)] = val;
                kernelSum += val;
            }
        }
        // Normalize to sum = 1.0
        if (kernelSum > 0.0f) {
            const float invSum = 1.0f / kernelSum;
            for (auto& v : scratch.kernel)
                v *= invSum;
        }

        // Determine channel layout — identify which channels are depth vs data
        const int nChans = channels.size();

        // Build channel-index maps: for each channel position, record whether
        // it is a depth channel (propagated raw) or a data channel (weighted)
        std::vector<bool> isDepthChan(nChans, false);
        {
            int ci = 0;
            foreach(z, channels) {
                if (z == Chan_DeepFront || z == Chan_DeepBack)
                    isDepthChan[ci] = true;
                ci++;
            }
        }

        // Output plane
        plane = DeepOutputPlane(channels, box, DeepPixel::eUnordered);

        const Box& inBox = inPlane.box();

        for (Box::iterator it = box.begin(); it != box.end(); ++it) {
            if (Op::aborted())
                return false;

            const int outX = it.x;
            const int outY = it.y;

            scratch.samples.clear();

            // Accumulate weighted samples from kernel neighbourhood
            for (int dy = -radY; dy <= radY; ++dy) {
                const int srcY = outY + dy;
                if (srcY < inBox.y() || srcY >= inBox.t())
                    continue;

                for (int dx = -radX; dx <= radX; ++dx) {
                    const int srcX = outX + dx;
                    if (srcX < inBox.x() || srcX >= inBox.r())
                        continue;

                    DeepPixel srcPixel = inPlane.getPixel(srcY, srcX);
                    const int srcSamples = static_cast<int>(srcPixel.getSampleCount());
                    if (srcSamples == 0)
                        continue;

                    const float weight = scratch.kernel[(dy + radY) * kernelW + (dx + radX)];
                    if (weight <= 0.0f)
                        continue;

                    for (int s = 0; s < srcSamples; ++s) {
                        deepc::SampleRecord rec;
                        rec.zFront = srcPixel.getUnorderedSample(s, Chan_DeepFront);
                        rec.zBack  = srcPixel.getUnorderedSample(s, Chan_DeepBack);
                        rec.alpha  = srcPixel.getUnorderedSample(s, Chan_Alpha) * weight;

                        // Collect data channels (non-depth) weighted, depth raw
                        rec.channels.resize(nChans);
                        int ci = 0;
                        foreach(z, channels) {
                            if (isDepthChan[ci]) {
                                // Depth propagated from source — NOT weighted
                                rec.channels[ci] = srcPixel.getUnorderedSample(s, z);
                            } else {
                                rec.channels[ci] = srcPixel.getUnorderedSample(s, z) * weight;
                            }
                            ci++;
                        }

                        scratch.samples.push_back(std::move(rec));
                    }
                }
            }

            // Empty pixel → hole
            if (scratch.samples.empty()) {
                plane.addHole();
                continue;
            }

            // Per-pixel sample optimization
            deepc::optimizeSamples(scratch.samples,
                                   _mergeTolerance,
                                   _colorTolerance,
                                   _maxSamples);

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
static Op* build(Node* node) { return new DeepCBlur(node); }
const Op::Description DeepCBlur::d(::CLASS, "Deep/DeepCBlur", build);
