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
//    1. On the first stripe per frame/param-set: render the FULL image via
//       Rust FFI (non-local convolution requires the full domain).
//    2. Cache the full-image result in _cachedRGBA (invalidated in _validate).
//    3. Serve every subsequent stripe from the cache — one FFI call per frame.
//
//  Full-image cache:
//    _cacheValid   — invalidated by _validate() on every knob/upstream change.
//    _cacheMutex   — guards reads and writes to _cacheValid / _cachedRGBA.
//    Cache key     — (fl, fs, fd, ssz, quality, use_gpu).
//    Concurrency   — the slow Rust render runs outside the lock; two threads
//                    racing on a cache miss each do a full render but the
//                    second write is harmless (idempotent result, no deadlock).
//
//  Holdout branch:
//    2a. If input 1 connected: build HoldoutData via buildHoldoutData() for
//        the full image, call opendefocus_deep_render_holdout().
//    2b. If input 1 not connected: call opendefocus_deep_render().
//
//  Knobs:
//    focal_length   — lens focal length (mm); overridden by camera link
//    fstop          — lens aperture f-stop; overridden by camera link
//    focus_distance — focus plane distance (scene units); camera link
//    sensor_size_mm — sensor width (mm, full-frame = 36); camera link
//    use_gpu        — attempt GPU backend (default on)
//    quality        — Low / Medium / High render quality
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
#include <mutex>
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
// Build a flat HoldoutData buffer from a deep plane.
//
// Iterates pixels in y-outer × x-inner scan order (matching the sample
// flatten loop in renderStripe).  For each pixel sorts samples front-to-back
// by Chan_DeepFront, computes transmittance T = product(1 - clamp(alpha,0,1)),
// and encodes as [d_front, T, d_back, T] into the result vector.
// Pixels with no samples encode [0.0, 1.0, 0.0, 1.0] (fully transparent).
//
// Returns a flat float vector of size width * height * 4, ready to wrap in
// a HoldoutData FFI struct.
// ---------------------------------------------------------------------------
static std::vector<float> buildHoldoutData(
    const DD::Image::DeepPlane& plane,
    const DD::Image::Box&       bounds,
    int w, int h)
{
    std::vector<float> result;
    result.reserve(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);

    for (int y = bounds.y(); y < bounds.t(); ++y) {
        for (int x = bounds.x(); x < bounds.r(); ++x) {
            DD::Image::DeepPixel px = plane.getPixel(y, x);
            const int nSamples = px.getSampleCount();

            if (nSamples == 0) {
                // No holdout geometry at this pixel — T=1 everywhere
                result.push_back(0.0f);
                result.push_back(1.0f);
                result.push_back(0.0f);
                result.push_back(1.0f);
                continue;
            }

            // Build (depth, alpha) pairs sorted front-to-back
            std::vector<std::pair<float, float>> samples;
            samples.reserve(static_cast<size_t>(nSamples));
            for (int s = 0; s < nSamples; ++s) {
                const float d = px.getUnorderedSample(s, DD::Image::Chan_DeepFront);
                const float a = px.getUnorderedSample(s, DD::Image::Chan_Alpha);
                samples.push_back({d, a});
            }
            std::sort(samples.begin(), samples.end(),
                      [](const std::pair<float,float>& lhs,
                         const std::pair<float,float>& rhs) {
                          return lhs.first < rhs.first;
                      });

            // Accumulate transmittance front-to-back
            float T = 1.0f;
            for (const auto& s : samples) {
                const float a = (s.second < 0.0f) ? 0.0f
                              : (s.second > 1.0f  ? 1.0f : s.second);
                T *= (1.0f - a);
            }

            const float d_front = samples.front().first;
            const float d_back  = samples.back().first;

            result.push_back(d_front);
            result.push_back(T);
            result.push_back(d_back);
            result.push_back(T);
        }
    }

    return result;
}

static const char* const kQualityEntries[] = {"Low", "Medium", "High", nullptr};

// ---------------------------------------------------------------------------
class DeepCOpenDefocus : public PlanarIop
{
    float _focal_length;    ///< mm, default 50
    float _fstop;           ///< default 2.8
    float _focus_distance;  ///< scene units, default 5.0
    float _sensor_size_mm;  ///< mm, default 36
    bool  _use_gpu;         ///< true = attempt GPU backend (default); false = force CPU
    int   _quality;         ///< 0=Low, 1=Medium, 2=High

    // Full-image render cache.
    //
    // _cachedRGBA holds width*height*4 interleaved floats for the full image
    // (fullBox coordinates, row-major).  _cacheValid is set false by
    // _validate() on every upstream/knob change, and set true after a
    // successful full-image render.
    //
    // Cache key: (fl, fs, fd, ssz, quality, use_gpu).
    //
    // _cacheMutex is mutable so the hit-check can be done inside a
    // conceptually-read-only renderStripe.  std::mutex is non-copyable —
    // this is fine because DeepCOpenDefocus is always heap-allocated via
    // new (see build() at the bottom of this file).
    std::vector<float>  _cachedRGBA;
    bool                _cacheValid   = false;
    float               _cacheFL      = 0.f;
    float               _cacheFS      = 0.f;
    float               _cacheFD      = 0.f;
    float               _cacheSSZ     = 0.f;
    int                 _cacheQuality = -1;
    int                 _cacheUseGpu  = -1;
    mutable std::mutex  _cacheMutex;

public:
    DeepCOpenDefocus(Node* node)
        : PlanarIop(node),
          _focal_length(50.0f),
          _fstop(2.8f),
          _focus_distance(5.0f),
          _sensor_size_mm(36.0f),
          _use_gpu(true),
          _quality(0)
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

        Enumeration_knob(f, &_quality, kQualityEntries, "quality", "Quality");
        Tooltip(f, "Render quality level. Low = fastest, High = best quality.");
    }

    // ------------------------------------------------------------------
    // _validate — declare RGBA output channels; invalidate full-image cache.
    //
    // Cache invalidation is unconditional: any knob edit, upstream change,
    // or frame change triggers _validate before the next renderStripe, so
    // setting _cacheValid = false here ensures stale cache data is never
    // served.
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

        // Invalidate the full-image cache: any validate call means knobs,
        // upstream data, or frame has changed — the cached render is stale.
        _cacheValid = false;
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
    // Full-image cache strategy:
    //   • On cache miss: render the complete image (fullBox = info_.box())
    //     via Rust FFI, store in _cachedRGBA, then copy the requested stripe
    //     sub-region to the output ImagePlane.
    //   • On cache hit: copy the stripe sub-region from _cachedRGBA directly.
    //
    // This ensures at most one Rust FFI call per frame/param-set regardless
    // of how many stripes Nuke's PlanarIop scheduler dispatches.
    //
    // Concurrency note: the slow Rust render runs outside _cacheMutex.
    // Two threads racing on the same cache miss each perform a full render;
    // the second cache write is harmless (identical result, no deadlock).
    // ------------------------------------------------------------------
    void renderStripe(ImagePlane& output) override
    {
        const Box& bounds = output.bounds();

        // Guard: if no deep input, write black and return
        DeepOp* deepIn = dynamic_cast<DeepOp*>(Op::input(0));
        if (!deepIn || bounds.w() <= 0 || bounds.h() <= 0) {
            output.makeWritable();
            float* dst0 = output.writable();
            if (dst0) {
                std::memset(dst0, 0,
                            static_cast<size_t>(bounds.w()) *
                            static_cast<size_t>(bounds.h()) *
                            static_cast<size_t>(output.nComps()) * sizeof(float));
            }
            return;
        }

        // -------------------------------------------------------------------
        // Compute effective lens params (camera override logic unchanged)
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

        // -------------------------------------------------------------------
        // Cache hit check (lock, compare key, copy stripe, return)
        // -------------------------------------------------------------------
        {
            std::unique_lock<std::mutex> lock(_cacheMutex);
            if (_cacheValid &&
                _cacheFL      == fl  &&
                _cacheFS      == fs  &&
                _cacheFD      == fd  &&
                _cacheSSZ     == ssz &&
                _cacheQuality == static_cast<int>(_quality) &&
                _cacheUseGpu  == static_cast<int>(_use_gpu))
            {
                // Full-image cache hit — serve the stripe from _cachedRGBA.
                const Box& fullBox = info_.box();
                const int  fullW   = fullBox.w();

                output.makeWritable();
                float* dst         = output.writable();
                const int rowStride      = output.rowStride();
                const int colStride      = output.colStride();
                const int64_t chanStride = output.chanStride();
                const int nComps         = output.nComps();

                if (dst) {
                    for (int y = bounds.y(); y < bounds.t(); ++y) {
                        const int localY = y - bounds.y();
                        for (int x = bounds.x(); x < bounds.r(); ++x) {
                            const int localX    = x - bounds.x();
                            const int fullPix   = (y - fullBox.y()) * fullW +
                                                  (x - fullBox.x());
                            const int srcBase   = fullPix * 4;
                            const int64_t dstBase =
                                static_cast<int64_t>(localY) * rowStride +
                                static_cast<int64_t>(localX) * colStride;
                            for (int c = 0; c < std::min(nComps, 4); ++c) {
                                dst[dstBase + static_cast<int64_t>(c) * chanStride] =
                                    _cachedRGBA[srcBase + c];
                            }
                        }
                    }
                }
                fprintf(stderr,
                        "DeepCOpenDefocus: cache hit — stripe y=[%d,%d)\n",
                        bounds.y(), bounds.t());
                return;
            }
            // Cache miss — release lock before the expensive render.
        }

        // -------------------------------------------------------------------
        // Cache miss: fetch and render the FULL image
        // -------------------------------------------------------------------
        const Box& fullBox = info_.box();
        const int  fullW   = fullBox.w();
        const int  fullH   = fullBox.h();

        // Guard degenerate full image (should not happen after _validate)
        if (fullW <= 0 || fullH <= 0) {
            output.makeWritable();
            float* dst0 = output.writable();
            if (dst0) {
                std::memset(dst0, 0,
                            static_cast<size_t>(bounds.w()) *
                            static_cast<size_t>(bounds.h()) *
                            static_cast<size_t>(output.nComps()) * sizeof(float));
            }
            return;
        }

        // Request RGBA + depth channels for the FULL image
        ChannelSet requestChans;
        requestChans += Chan_Red;
        requestChans += Chan_Green;
        requestChans += Chan_Blue;
        requestChans += Chan_Alpha;
        requestChans += Chan_DeepFront;

        DeepPlane deepPlane;
        deepIn->deepEngine(fullBox, requestChans, deepPlane);

        // -------------------------------------------------------------------
        // Flatten deep samples for the full image into contiguous C arrays
        // -------------------------------------------------------------------
        std::vector<uint32_t> sampleCounts;
        sampleCounts.reserve(static_cast<size_t>(fullW) * static_cast<size_t>(fullH));

        std::vector<float> rgba_flat;
        std::vector<float> depth_flat;

        for (int y = fullBox.y(); y < fullBox.t(); ++y) {
            for (int x = fullBox.x(); x < fullBox.r(); ++x) {
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

        const uint32_t pixelCount   = static_cast<uint32_t>(fullW) *
                                      static_cast<uint32_t>(fullH);
        const uint32_t totalSamples = static_cast<uint32_t>(depth_flat.size());

        DeepSampleData sampleData;
        sampleData.sample_counts = sampleCounts.data();
        sampleData.rgba          = rgba_flat.empty()  ? nullptr : rgba_flat.data();
        sampleData.depth         = depth_flat.empty() ? nullptr : depth_flat.data();
        sampleData.pixel_count   = pixelCount;
        sampleData.total_samples = totalSamples;

        // Output buffer — full image, filled by Rust FFI
        std::vector<float> output_buf(
            static_cast<size_t>(fullW) * static_cast<size_t>(fullH) * 4u, 0.0f);

        // Build LensParams
        LensParams lensParams;
        lensParams.focal_length_mm = fl;
        lensParams.fstop           = fs;
        lensParams.focus_distance  = fd;
        lensParams.sensor_size_mm  = ssz;

        // -------------------------------------------------------------------
        // Call Rust FFI — outside lock (see concurrency note above).
        // Holdout branch: fetch holdout deep plane for full image too.
        // -------------------------------------------------------------------
        DeepOp* holdoutIn = dynamic_cast<DeepOp*>(Op::input(1));
        if (holdoutIn) {
            ChannelSet holdoutChans;
            holdoutChans += Chan_Alpha;
            holdoutChans += Chan_DeepFront;  // required for depth ordering

            DeepPlane holdoutPlane;
            holdoutIn->deepEngine(fullBox, holdoutChans, holdoutPlane);

            std::vector<float> holdout_flat =
                buildHoldoutData(holdoutPlane, fullBox, fullW, fullH);
            HoldoutData holdout_ffi;
            holdout_ffi.data = holdout_flat.data();
            holdout_ffi.len  = static_cast<uint32_t>(holdout_flat.size());

            opendefocus_deep_render_holdout(&sampleData, output_buf.data(),
                static_cast<uint32_t>(fullW), static_cast<uint32_t>(fullH),
                &lensParams, (int)_use_gpu, &holdout_ffi, (int)_quality);

            fprintf(stderr,
                    "DeepCOpenDefocus: holdout attenuation applied (%u pixels)\n",
                    pixelCount);
        } else {
            opendefocus_deep_render(&sampleData,
                                    output_buf.data(),
                                    static_cast<uint32_t>(fullW),
                                    static_cast<uint32_t>(fullH),
                                    &lensParams,
                                    (int)_use_gpu,
                                    (int)_quality);
        }

        fprintf(stderr,
                "DeepCOpenDefocus: rendered full image %dx%d, serving stripe y=[%d,%d)\n",
                fullW, fullH, bounds.y(), bounds.t());

        // -------------------------------------------------------------------
        // Cache write — lock, store full image, update key, release.
        // -------------------------------------------------------------------
        {
            std::lock_guard<std::mutex> lock(_cacheMutex);
            _cachedRGBA   = output_buf;          // deep copy into cache
            _cacheFL      = fl;
            _cacheFS      = fs;
            _cacheFD      = fd;
            _cacheSSZ     = ssz;
            _cacheQuality = static_cast<int>(_quality);
            _cacheUseGpu  = static_cast<int>(_use_gpu);
            _cacheValid   = true;
        }

        // -------------------------------------------------------------------
        // Copy stripe sub-region from output_buf to the output ImagePlane.
        //
        // output_buf is stored row-major for the full image:
        //   index = (y - fullBox.y()) * fullW + (x - fullBox.x())
        //
        // The output ImagePlane uses local (0-based) row/col addressing:
        //   dstBase = localY * rowStride + localX * colStride
        // -------------------------------------------------------------------
        output.makeWritable();
        float* dst               = output.writable();
        const int rowStride      = output.rowStride();
        const int colStride      = output.colStride();
        const int64_t chanStride = output.chanStride();
        const int nComps         = output.nComps();

        if (dst) {
            for (int y = bounds.y(); y < bounds.t(); ++y) {
                const int localY = y - bounds.y();
                for (int x = bounds.x(); x < bounds.r(); ++x) {
                    const int localX  = x - bounds.x();
                    const int fullPix = (y - fullBox.y()) * fullW + (x - fullBox.x());
                    const int srcBase = fullPix * 4;
                    const int64_t dstBase =
                        static_cast<int64_t>(localY) * rowStride +
                        static_cast<int64_t>(localX) * colStride;
                    for (int c = 0; c < std::min(nComps, 4); ++c) {
                        dst[dstBase + static_cast<int64_t>(c) * chanStride] =
                            output_buf[srcBase + c];
                    }
                }
            }
        }
    }

    static const Op::Description d;
};

// ---------------------------------------------------------------------------
static Op* build(Node* node) { return new DeepCOpenDefocus(node); }
const Op::Description DeepCOpenDefocus::d(CLASS, "Filter/DeepCOpenDefocus", build);
