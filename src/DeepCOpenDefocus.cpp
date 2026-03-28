// SPDX-License-Identifier: MIT
//
// ============================================================================
//
//  DeepCOpenDefocus — Deep defocus using the opendefocus-deep Rust library
//
//  Input 0: Deep image (required)
//  Input 1: Holdout Deep image (optional) — attenuates defocused output by
//            the accumulated transmittance through holdout deep samples.
//            Transmittance T = product of (1 - alpha_s) for each holdout
//            sample sorted front-to-back.  All four RGBA channels are
//            multiplied by T so that both coverage and colour are attenuated.
//            When input 1 is not connected the defocused output is unchanged.
//  Input 2: Camera (optional) — overrides focal_length, fstop,
//            focus_distance, and sensor_size_mm knobs when connected.
//
//  Pipeline (renderStripe):
//    1. Flatten input 0 deep samples to C arrays.
//    2. Call opendefocus_deep_render() → fills output_buf with flat RGBA.
//    3. [S03] If input 1 connected: apply holdout transmittance attenuation
//       in-place on output_buf.
//    4. Copy output_buf to the output ImagePlane.
//
//  Knobs:
//    focal_length   — lens focal length (mm); overridden by camera link
//    fstop          — lens aperture f-stop; overridden by camera link
//    focus_distance — focus plane distance (scene units); camera link
//    sensor_size_mm — sensor width (mm, full-frame = 36); camera link
//
// ============================================================================

#include "DDImage/PlanarIop.h"
#include "DDImage/DeepOp.h"
#include "DDImage/DeepPlane.h"
#include "DDImage/CameraOp.h"
#include "DDImage/Knobs.h"
#include "DDImage/Row.h"
#include "../crates/opendefocus-deep/include/opendefocus_deep.h"
#include <algorithm>
#include <utility>
#include <vector>
#include <cstring>
#include <cstdio>

using namespace DD::Image;

static const char* const CLASS = "DeepCOpenDefocus";
static const char* const HELP  =
    "Deep defocus using the opendefocus-deep Rust library (EUPL-1.2).\n\n"
    "Input 0: Deep image (required)\n"
    "Input 1: Holdout Deep image (optional) — attenuates defocused output behind holdout geometry.\n"
    "Input 2: Camera (optional) — drives lens knobs from scene camera.\n\n"
    "Connect a Camera node to input 2 to automatically sync focal length, f-stop, "
    "focus distance, and sensor size from the scene camera.\n\n"
    "Part of the DeepC plugin collection.";

// ---------------------------------------------------------------------------
// Compute holdout transmittance for a single pixel's deep samples.
//
// T = product of (1 - alpha_s) for each sample in the DeepPixel, regardless
// of depth order.  Returns a value in [0, 1]: 0 means fully occluded,
// 1 means fully transparent (no holdout geometry).
//
// Exposed as a free function for unit-testability without a live render.
// ---------------------------------------------------------------------------
static float computeHoldoutTransmittance(const DD::Image::DeepPixel& dp)
{
    float T = 1.0f;
    const int nSamples = dp.getSampleCount();
    for (int s = 0; s < nSamples; ++s) {
        const float alpha = dp.getUnorderedSample(s, DD::Image::Chan_Alpha);
        // clamp to [0, 1] to guard against over-bright / negative samples
        const float a = (alpha < 0.0f) ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
        T *= (1.0f - a);
    }
    return T;
}

// ---------------------------------------------------------------------------
class DeepCOpenDefocus : public PlanarIop
{
    float _focal_length;    ///< mm, default 50
    float _fstop;           ///< default 2.8
    float _focus_distance;  ///< scene units, default 5.0
    float _sensor_size_mm;  ///< mm, default 36
    bool  _use_gpu;         ///< true = attempt GPU backend (default); false = force CPU

public:
    DeepCOpenDefocus(Node* node)
        : PlanarIop(node),
          _focal_length(50.0f),
          _fstop(2.8f),
          _focus_distance(5.0f),
          _sensor_size_mm(36.0f),
          _use_gpu(true)
    {
        inputs(3);
    }

    // ------------------------------------------------------------------
    // Identity
    // ------------------------------------------------------------------
    const char* Class() const override    { return CLASS; }
    const char* node_help() const override { return HELP; }

    // ------------------------------------------------------------------
    // Input specification
    // ------------------------------------------------------------------
    int minimum_inputs() const override { return 1; }
    int maximum_inputs() const override { return 3; }

    bool test_input(int n, Op* op) const override
    {
        if (op == nullptr) return true;          // allow optional / unconnected
        if (n == 0 || n == 1)
            return dynamic_cast<DeepOp*>(op) != nullptr;
        if (n == 2)
            return dynamic_cast<CameraOp*>(op) != nullptr;
        return false;
    }

    const char* input_label(int n, char*) const override
    {
        switch (n) {
            case 0:  return "";
            case 1:  return "holdout";
            case 2:  return "camera";
            default: return "";
        }
    }

    // ------------------------------------------------------------------
    // Knobs
    // ------------------------------------------------------------------
    void knobs(Knob_Callback f) override
    {
        Float_knob(f, &_focal_length,   "focal_length",   "Focal Length");
        Tooltip(f, "Lens focal length in millimetres (e.g. 50 mm). "
                   "Overridden by a connected Camera node on input 2.");

        Float_knob(f, &_fstop,          "fstop",          "F-Stop");
        Tooltip(f, "Lens aperture f-stop (e.g. 2.8). Lower values = wider aperture = more blur. "
                   "Overridden by a connected Camera node on input 2.");

        Float_knob(f, &_focus_distance, "focus_distance", "Focus Distance");
        Tooltip(f, "Distance to the plane of focus in scene units. "
                   "Overridden by a connected Camera node on input 2 (focal_point).");

        Float_knob(f, &_sensor_size_mm, "sensor_size_mm", "Sensor Size (mm)");
        Tooltip(f, "Sensor width in millimetres (full-frame = 36 mm). "
                   "Overridden by a connected Camera node on input 2 (film_width × 10).");

        Bool_knob(f, &_use_gpu, "use_gpu", "Use GPU");
        Tooltip(f, "Enable GPU-accelerated defocus (default: on). "
                   "Disable to force CPU-only path (slower but useful for debugging or headless renders).");
    }

    // ------------------------------------------------------------------
    // _validate — declare RGBA output channels
    // ------------------------------------------------------------------
    void _validate(bool for_real) override
    {
        // PlanarIop::_validate calls copy_info(0) which expects an Iop at
        // input 0.  Our input 0 is a DeepOp (not an Iop), so calling
        // Iop::input() would trigger asIop() which asserts.  We must use
        // Op::input() to bypass the Iop cast, then validate the deep input
        // directly and set up our output info manually.

        Op* in0 = Op::input(0);
        if (in0) {
            in0->validate(for_real);
        }

        // Derive output format and bbox from the deep input's deepInfo.
        DeepOp* deepIn = dynamic_cast<DeepOp*>(in0);
        if (deepIn) {
            const DeepInfo& di = deepIn->deepInfo();
            info_.format(*di.format());
            info_.full_size_format(*di.format());
            info_.set(di.box());
        }

        ChannelSet rgba;
        rgba += Chan_Red;
        rgba += Chan_Green;
        rgba += Chan_Blue;
        rgba += Chan_Alpha;
        info_.channels(rgba);
    }

    // ------------------------------------------------------------------
    // Override request/getRequests to prevent PlanarIop base from calling
    // input0() / asIop() — our inputs are DeepOps, not Iops.
    // Deep data is pulled on-demand in renderStripe via deepEngine().
    // ------------------------------------------------------------------
    void getRequests(const Box& box, const ChannelSet& channels,
                     int count, RequestOutput& reqData) const override
    {
        // Deep inputs handle their own data flow via deepEngine().
        // We still need to validate the deep input so its upstream
        // chain is ready when renderStripe calls deepEngine().
        // Validation is idempotent and safe to call here.
        Op* in0 = const_cast<DeepCOpenDefocus*>(this)->Op::input(0);
        if (in0) {
            in0->validate(true);
        }
    }

    // ------------------------------------------------------------------
    // renderStripe
    //
    // Pipeline:
    //   1. Obtain deep input (input 0).
    //   2. Request RGBA + DeepFront deep plane for the output bounds.
    //   3. Flatten per-pixel sample data into three contiguous arrays
    //      (sample_counts, rgba_flat, depth_flat) required by the FFI struct.
    //   4. Call opendefocus_deep_render() — GPU layer-peel defocus via Rust.
    //   5. [S03] If input 1 (holdout) is connected: apply per-pixel holdout
    //      transmittance attenuation to the defocused output_buf in-place.
    //   6. Copy output_buf to the output ImagePlane.
    // ------------------------------------------------------------------
    void renderStripe(ImagePlane& output) override
    {
        const Box& bounds = output.bounds();
        const int  width  = bounds.w();
        const int  height = bounds.h();

        // Guard: if no deep input, write black and return
        DeepOp* deepIn = dynamic_cast<DeepOp*>(Op::input(0));
        if (!deepIn || width <= 0 || height <= 0) {
            output.makeWritable();
            std::memset(output.writable(), 0,
                        static_cast<size_t>(width) * static_cast<size_t>(height) *
                        static_cast<size_t>(output.nComps()) * sizeof(float));
            return;
        }

        // Request RGBA + depth channels from the deep input
        ChannelSet requestChans;
        requestChans += Chan_Red;
        requestChans += Chan_Green;
        requestChans += Chan_Blue;
        requestChans += Chan_Alpha;
        requestChans += Chan_DeepFront;

        DeepPlane deepPlane;
        deepIn->deepEngine(bounds, requestChans, deepPlane);

        // -------------------------------------------------------------------
        // Flatten deep samples into contiguous C arrays for the FFI struct
        // -------------------------------------------------------------------
        std::vector<uint32_t> sampleCounts;
        sampleCounts.reserve(static_cast<size_t>(width) * static_cast<size_t>(height));

        std::vector<float> rgba_flat;
        std::vector<float> depth_flat;

        for (int y = bounds.y(); y < bounds.t(); ++y) {
            for (int x = bounds.x(); x < bounds.r(); ++x) {
                DeepPixel px = deepPlane.getPixel(y, x);
                const int nSamples = px.getSampleCount();
                sampleCounts.push_back(static_cast<uint32_t>(nSamples));

                for (int s = 0; s < nSamples; ++s) {
                    rgba_flat.push_back(px.getUnorderedSample(s, Chan_Red));
                    rgba_flat.push_back(px.getUnorderedSample(s, Chan_Green));
                    rgba_flat.push_back(px.getUnorderedSample(s, Chan_Blue));
                    rgba_flat.push_back(px.getUnorderedSample(s, Chan_Alpha));
                    depth_flat.push_back(px.getUnorderedSample(s, Chan_DeepFront));
                }
            }
        }

        const uint32_t pixelCount   = static_cast<uint32_t>(width) *
                                      static_cast<uint32_t>(height);
        const uint32_t totalSamples = static_cast<uint32_t>(depth_flat.size());

        DeepSampleData sampleData;
        sampleData.sample_counts = sampleCounts.data();
        sampleData.rgba          = rgba_flat.empty()  ? nullptr : rgba_flat.data();
        sampleData.depth         = depth_flat.empty() ? nullptr : depth_flat.data();
        sampleData.pixel_count   = pixelCount;
        sampleData.total_samples = totalSamples;

        // Output buffer — filled by the Rust GPU defocus kernel
        std::vector<float> output_buf(
            static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0.0f);

        // -------------------------------------------------------------------
        // Camera link override (input 2)
        //
        // If a CameraOp is connected, read its scene properties and use them
        // instead of the manual knob values.  This keeps the knobs as a
        // visible fallback/reference but the live camera always takes priority.
        //
        // Unit conversions:
        //   focal_length()       → mm (Nuke stores as mm)
        //   fStop()              → dimensionless f-stop number
        //   focal_point()        → scene units (focus distance; deprecated alias for focusDistance())
        //   film_width()         → cm; multiply by 10 to get mm
        // -------------------------------------------------------------------
        float fl  = _focal_length;
        float fs  = _fstop;
        float fd  = _focus_distance;
        float ssz = _sensor_size_mm;

        CameraOp* cam = dynamic_cast<CameraOp*>(Op::input(2));
        if (cam) {
            cam->validate(false);
            fl  = static_cast<float>(cam->focal_length());
            fs  = static_cast<float>(cam->fStop());
            fd  = static_cast<float>(cam->focal_point());
            ssz = static_cast<float>(cam->film_width()) * 10.0f;   // cm → mm
            fprintf(stderr, "DeepCOpenDefocus: camera link active "
                    "(fl=%.2f fstop=%.2f fd=%.2f ssz=%.2f)\n",
                    fl, fs, fd, ssz);
        } else {
            fprintf(stderr, "DeepCOpenDefocus: camera link inactive (using knobs)\n");
        }

        // Build LensParams — either from camera or from knob values above
        LensParams lensParams;
        lensParams.focal_length_mm = fl;
        lensParams.fstop           = fs;
        lensParams.focus_distance  = fd;
        lensParams.sensor_size_mm  = ssz;

        // Call Rust FFI — GPU layer-peel defocus; fills output_buf with flat RGBA
        opendefocus_deep_render(&sampleData,
                                output_buf.data(),
                                static_cast<uint32_t>(width),
                                static_cast<uint32_t>(height),
                                &lensParams,
                                (int)_use_gpu);

        // -------------------------------------------------------------------
        // S03: Holdout transmittance attenuation
        //
        // If input 1 is a connected DeepOp, fetch its deep plane and compute
        // per-pixel transmittance T = product(1 - alpha_s).  Multiply all
        // four channels of the defocused output by T so that pixels behind
        // holdout geometry are attenuated proportionally to the holdout alpha
        // coverage.  The holdout deep input is never defocused itself.
        // -------------------------------------------------------------------
        DeepOp* holdoutIn = dynamic_cast<DeepOp*>(Op::input(1));
        if (holdoutIn) {
            ChannelSet holdoutChans;
            holdoutChans += Chan_Alpha;
            holdoutChans += Chan_DeepFront;  // needed by deepEngine for ordering

            DeepPlane holdoutPlane;
            holdoutIn->deepEngine(bounds, holdoutChans, holdoutPlane);

            int pixIdx = 0;
            for (int y = bounds.y(); y < bounds.t(); ++y) {
                for (int x = bounds.x(); x < bounds.r(); ++x) {
                    DeepPixel hp = holdoutPlane.getPixel(y, x);
                    const float T = computeHoldoutTransmittance(hp);

                    // output_buf layout: R G B A per pixel, 4 floats each
                    const int base = pixIdx * 4;
                    output_buf[base + 0] *= T;
                    output_buf[base + 1] *= T;
                    output_buf[base + 2] *= T;
                    output_buf[base + 3] *= T;

                    ++pixIdx;
                }
            }

            fprintf(stderr, "DeepCOpenDefocus: holdout attenuation applied (%d pixels)\n",
                    static_cast<int>(pixelCount));
        }

        // -------------------------------------------------------------------
        // Copy output_buf (interleaved RGBA) to the output ImagePlane.
        //
        // ImagePlane can be packed (interleaved RGBARGBA) or unpacked
        // (planar RRRR...GGGG...).  Use the stride accessors to handle
        // both layouts correctly.
        // -------------------------------------------------------------------
        output.makeWritable();
        float* dst = output.writable();
        const int rowStride  = output.rowStride();
        const int colStride  = output.colStride();
        const int64_t chanStride = output.chanStride();
        const int nComps = output.nComps();
        if (dst && !output_buf.empty()) {
            if (chanStride == 1 && colStride == nComps) {
                // Packed layout (RGBARGBA...) — matches output_buf exactly
                const size_t totalBytes = static_cast<size_t>(width) * height * nComps * sizeof(float);
                std::memcpy(dst, output_buf.data(),
                            std::min(totalBytes, output_buf.size() * sizeof(float)));
            } else {
                // Unpacked / planar layout — de-interleave
                int pixIdx = 0;
                for (int y = 0; y < height; ++y) {
                    for (int x = 0; x < width; ++x) {
                        const int srcBase = pixIdx * 4;
                        const int64_t dstBase = static_cast<int64_t>(y) * rowStride
                                              + static_cast<int64_t>(x) * colStride;
                        for (int c = 0; c < std::min(nComps, 4); ++c) {
                            dst[dstBase + c * chanStride] = output_buf[srcBase + c];
                        }
                        ++pixIdx;
                    }
                }
            }
        } else if (dst) {
            std::memset(dst, 0,
                        static_cast<size_t>(width) * static_cast<size_t>(height) *
                        static_cast<size_t>(nComps) * sizeof(float));
        }
    }

    static const Op::Description d;
};

// ---------------------------------------------------------------------------
static Op* build(Node* node) { return new DeepCOpenDefocus(node); }
const Op::Description DeepCOpenDefocus::d(CLASS, "Filter/DeepCOpenDefocus", build);
