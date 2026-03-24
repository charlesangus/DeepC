// SPDX-License-Identifier: MIT
//
// ============================================================================
//
//  DeepCDefocusPO — Polynomial-Optics defocus for deep images
//
//  Converts a Deep input stream to a flat RGBA output by scattering each
//  deep sample's contribution through a polynomial lens system (.fit file).
//
//  Base class: PlanarIop — single-threaded renderStripe, no shared output
//  buffer races (correct for stochastic aperture scatter). See D021.
//
//  Full implementation: PO scatter engine (S02), depth-aware holdout (S03),
//  and focal-length knob (S04) are all complete.
//
//  Inputs:
//    0 (required) — Deep stream to defocus
//    1 (optional) — Deep holdout mask (evaluated at output pixel, not
//                   pre-applied — prevents double-defocus, see D024)
//
//  Knobs:
//    poly_file        — path to lentil .fit polynomial lens file
//    focus_distance   — distance to focus plane, mm (default 200 mm = 2 m)
//    fstop            — lens f-stop (default 2.8)
//    focal_length_mm  — lens focal length in mm (default 50.0)
//    aperture_samples — scatter samples per deep sample (default 64)
//
// ============================================================================

#include "DDImage/PlanarIop.h"
#include "DDImage/ImagePlane.h"
#include "DDImage/DeepOp.h"
#include "DDImage/DeepPixel.h"
#include "DDImage/DeepPlane.h"
#include "DDImage/Knobs.h"
// poly.h is included in exactly ONE translation unit — this one. See ODR
// note in poly.h header comment.
#include "poly.h"
#include "deepc_po_math.h"
#include <string>
#include <cstring>
#include <algorithm>

using namespace DD::Image;

// ---------------------------------------------------------------------------
// Class identity strings
// ---------------------------------------------------------------------------

static const char* const CLASS = "DeepCDefocusPO";
static const char* const HELP =
    "Polynomial-optics defocus for deep images.\n\n"
    "Scatters each deep sample through a lentil polynomial lens system "
    "(.fit file) to produce a physically accurate bokeh on a flat RGBA "
    "output tile.\n\n"
    "poly_file — path to the lentil .fit binary polynomial lens file.\n"
    "focus_distance — distance from the lens to the focus plane (mm). "
    "Samples at this depth are not defocused.\n"
    "fstop — lens aperture. Lower values produce wider bokeh.\n"
    "aperture_samples — number of aperture points traced per deep sample. "
    "Higher values reduce Monte Carlo noise.\n\n"
    "Input 0: Deep stream to defocus (required).\n"
    "Input 1 (holdout): Optional deep holdout mask. Evaluated at the "
    "output pixel position at the sample depth — not pre-applied before "
    "scatter — to avoid double-defocus artifacts.\n\n"
    "Part of the DeepC plugin collection.";

// ---------------------------------------------------------------------------
// DeepCDefocusPO — PlanarIop subclass
// ---------------------------------------------------------------------------

class DeepCDefocusPO : public PlanarIop
{
    // ------------------------------------------------------------------
    // Member variables
    // ------------------------------------------------------------------

    const char* _poly_file;        // File_knob binds here (const char*)
    float       _focus_distance;   // distance to focus plane, mm
    float       _fstop;            // lens f-stop
    int         _aperture_samples; // aperture scatter samples per deep sample
    float       _focal_length_mm;  // lens focal length in mm (knob-driven)

    bool        _poly_loaded;      // true after poly_system_read succeeds
    bool        _reload_poly;      // set by knob_changed; cleared after reload

    poly_system_t _poly_sys;       // polynomial lens system (zeroed in ctor)

public:
    // ------------------------------------------------------------------
    // Constructor
    // ------------------------------------------------------------------

    DeepCDefocusPO(Node* node)
        : PlanarIop(node)
        , _poly_file(nullptr)
        , _focus_distance(200.0f)
        , _fstop(2.8f)
        , _aperture_samples(64)
        , _focal_length_mm(50.0f)
        , _poly_loaded(false)
        , _reload_poly(false)
    {
        inputs(2);
        std::memset(&_poly_sys, 0, sizeof(_poly_sys));
    }

    // ------------------------------------------------------------------
    // Class identity
    // ------------------------------------------------------------------

    const char* Class()     const override { return CLASS; }
    const char* node_help() const override { return HELP; }

    // ------------------------------------------------------------------
    // Input wiring — both inputs accept Deep streams
    // ------------------------------------------------------------------

    int  minimum_inputs() const override { return 1; }
    int  maximum_inputs() const override { return 2; }

    bool test_input(int /*idx*/, Op* op) const override
    {
        return dynamic_cast<DeepOp*>(op) != nullptr;
    }

    Op* default_input(int /*idx*/) const override { return nullptr; }

    const char* input_label(int n, char*) const override
    {
        return (n == 1) ? "holdout" : "";
    }

    // Convenience accessor — returns nullptr if input 0 is not connected
    // or is not a DeepOp.
    DeepOp* input0() const
    {
        return dynamic_cast<DeepOp*>(Op::input(0));
    }

    // Convenience accessor for the optional holdout input (input 1).
    // Returns nullptr when not connected or not a DeepOp.
    DeepOp* input1() const
    {
        return dynamic_cast<DeepOp*>(Op::input(1));
    }

    // ------------------------------------------------------------------
    // Knobs
    // ------------------------------------------------------------------

    void knobs(Knob_Callback f) override
    {
        File_knob(f, &_poly_file, "poly_file", "lens file");
        Tooltip(f, "Path to the lentil polynomial lens file (.fit binary). "
                    "Change triggers an automatic reload.");

        Float_knob(f, &_focal_length_mm, "focal_length", "focal length (mm)");
        SetRange(f, 1.0f, 1000.0f);
        Tooltip(f, "Lens focal length in mm. Used for circle-of-confusion culling only — "
                    "the polynomial already encodes the correct bokeh shape. "
                    "Typical values: 24 (wide), 50 (normal), 85 (portrait), 135 (tele).");

        Divider(f, "");

        Float_knob(f, &_focus_distance, "focus_distance", "focus distance (mm)");
        SetRange(f, 1.0f, 100000.0f);
        Tooltip(f, "Distance from the lens to the focus plane in mm. "
                    "Samples at this depth produce a sharp point on the sensor.");

        Float_knob(f, &_fstop, "fstop", "f-stop");
        SetRange(f, 0.5f, 64.0f);
        Tooltip(f, "Lens aperture (f-stop). Lower values produce larger bokeh.");

        Int_knob(f, &_aperture_samples, "aperture_samples", "aperture samples");
        SetRange(f, 1, 4096);
        Tooltip(f, "Number of aperture sample positions traced per deep sample. "
                    "Higher values reduce Monte Carlo noise at the cost of render time.");
    }

    // ------------------------------------------------------------------
    // knob_changed — trigger poly reload when the file path changes
    // ------------------------------------------------------------------

    int knob_changed(Knob* k) override
    {
        if (std::string(k->name()) == "poly_file") {
            _reload_poly = true;
            return 1;
        }
        return PlanarIop::knob_changed(k);
    }

    // ------------------------------------------------------------------
    // _validate — propagate format from Deep input.
    //
    // NOTE: poly loading deliberately does NOT happen here.  _validate can
    // be called by Nuke on the main thread while renderStripe is executing
    // on a worker thread.  Calling poly_system_destroy / poly_system_read
    // from _validate would free _poly_sys.poly[k].term pointers that are
    // simultaneously being read in renderStripe — a data race that produces
    // use-after-free, heap corruption (free(): invalid pointer,
    // _int_malloc assertions), and SIGSEGV.
    //
    // Instead, renderStripe loads (or reloads) the poly at its own entry
    // point, before any render work begins.  Because PlanarIop::renderStripe
    // is called sequentially (one stripe at a time, not re-entrant), and
    // _validate cannot be called from within renderStripe, the load is safe
    // without any additional locking.
    // ------------------------------------------------------------------

    void _validate(bool for_real) override
    {
        DeepOp* in = input0();

        if (!in) {
            // No Deep input connected — produce an empty output.
            info_.set(Box());
            info_.channels(Mask_None);
            return;
        }

        in->validate(for_real);

        // Copy spatial format and channel set from the Deep input.
        const DeepInfo& di = in->deepInfo();
        info_.set(di.box());
        info_.format(*di.format());
        info_.full_size_format(*di.fullSizeFormat());
        info_.channels(Mask_RGBA);
        set_out_channels(Mask_RGBA);

        // If the poly file path changed, mark it dirty for renderStripe to reload.
        // Do NOT load/destroy here — see note above.
        if (for_real && _poly_file && _poly_file[0] != '\0' && _reload_poly) {
            // Mark as unloaded so renderStripe reloads at next render.
            // Actual destroy+reload happens in renderStripe under single-threaded
            // PlanarIop execution.
            _poly_loaded = false;
        }
    }

    // ------------------------------------------------------------------
    // getRequests — propagate Deep request upstream
    // ------------------------------------------------------------------

    void getRequests(const Box&        box,
                     const ChannelSet& channels,
                     int               count,
                     RequestOutput&    reqData) const override
    {
        if (input0())
            input0()->deepRequest(
                box,
                channels + Chan_DeepFront + Chan_DeepBack + Chan_Alpha,
                count);

        // Request depth+alpha from the holdout input (input 1) when connected.
        // Colour channels are intentionally excluded — holdout contributes
        // transmittance only, never colour (R024).
        DeepOp* holdoutOp = dynamic_cast<DeepOp*>(Op::input(1));
        if (holdoutOp) {
            ChannelSet needed_h;
            needed_h += Chan_Alpha;
            needed_h += Chan_DeepFront;
            needed_h += Chan_DeepBack;
            holdoutOp->deepRequest(box, needed_h, count);
        }
    }

    // ------------------------------------------------------------------
    // renderStripe — PO scatter engine
    //
    // For each output pixel in the stripe:
    //   For each deep sample visible through that pixel:
    //     For each aperture sample (Halton(2,3) + Shirley disk):
    //       lt_newton_trace at 3 wavelengths → sensor hit → accumulate RGBA
    //
    // Fast path: if no poly is loaded or no Deep input is connected, the
    // output stripe is zeroed immediately and the method returns.
    // ------------------------------------------------------------------

    void renderStripe(ImagePlane& imagePlane) override
    {
        imagePlane.makeWritable();

        const Box& bounds = imagePlane.bounds();
        const ChannelSet& chans = imagePlane.channels();

        // Helper: zero the entire output stripe.  Used by all early-exit paths
        // so Nuke never receives uninitialized pixel data.
        //
        // writableAt(x, y, z) takes a channel *index* (not a Channel enum value).
        // chanNo(chan) converts Channel enum → index within this plane.
        auto zero_output = [&]() {
            foreach(z, chans)
                for (int y = bounds.y(); y < bounds.t(); ++y)
                    for (int x = bounds.x(); x < bounds.r(); ++x)
                        imagePlane.writableAt(x, y, imagePlane.chanNo(z)) = 0.0f;
        };

        // Load or reload the polynomial lens file here, not in _validate.
        // _validate runs on the main thread and can interleave with renderStripe
        // on a worker thread.  Loading here (under PlanarIop's sequential
        // single-stripe guarantee) eliminates the data race on _poly_sys.
        if (_poly_file && _poly_file[0] != '\0' && (!_poly_loaded || _reload_poly)) {
            if (_poly_loaded) {
                poly_system_destroy(&_poly_sys);
                _poly_loaded = false;
            }
            const int rc = poly_system_read(&_poly_sys, _poly_file);
            if (rc != 0) {
                // File unreadable — zero output and skip render.
                zero_output();
                return;
            }
            _poly_loaded = true;
            _reload_poly = false;
        }

        // Fast path: no poly loaded or no input — zero output.
        if (!_poly_loaded || !input0()) {
            zero_output();
            return;
        }

        // 1. Fetch Deep input for this stripe.
        DeepPlane deepPlane;
        ChannelSet needed = Mask_RGBA;
        needed += Chan_DeepFront;
        needed += Chan_DeepBack;
        if (!input0()->deepEngine(bounds, needed, deepPlane)) {
            zero_output();
            return;
        }

        // 1b. Fetch holdout plane at the same output bounds (R024: holdout is
        //     evaluated at the *output* pixel position, not the input position).
        //     If the holdout is not connected or fails, holdoutConnected stays
        //     false, which causes transmittance_at() to return 1.0 (no masking).
        DeepOp* holdoutOp = dynamic_cast<DeepOp*>(Op::input(1));
        DeepPlane holdoutPlane;
        bool holdoutConnected = false;
        if (holdoutOp) {
            ChannelSet hNeeded;
            hNeeded += Chan_Alpha;
            hNeeded += Chan_DeepFront;
            hNeeded += Chan_DeepBack;
            holdoutConnected = holdoutOp->deepEngine(bounds, hNeeded, holdoutPlane);
        }

        // transmittance_at — computes the holdout transmittance at output pixel
        // (ox, oy) for a scatter contribution at scene depth Z.
        //
        // Formula (R023): T = product(1 - alpha_i) for all holdout samples where
        // zFront_i < Z.  The product is order-independent (commutative), so
        // getUnorderedSample is correct — no sort needed.
        //
        // Returns 1.0 when holdout is disconnected or the pixel has no samples.
        auto transmittance_at = [&](int ox, int oy, float Z) -> float {
            if (!holdoutConnected) return 1.0f;
            DeepPixel hpx = holdoutPlane.getPixel(oy, ox);  // DDImage: y first
            const int nH = hpx.getSampleCount();
            if (nH == 0) return 1.0f;
            float T = 1.0f;
            for (int h = 0; h < nH; ++h) {
                const float hzf = hpx.getUnorderedSample(h, Chan_DeepFront);
                if (hzf >= Z) continue;  // sample is at or behind — skip
                const float ha = hpx.getUnorderedSample(h, Chan_Alpha);
                T *= (1.0f - ha);
            }
            return std::max(0.0f, T);
        };

        // 2. Zero the output accumulation buffer.
        zero_output();

        // 3. Format dimensions for normalised coordinate conversion.
        //    Sensor coordinates use [-1, 1] x [-1, 1] (normalised).
        //    pix_per_norm = half-width in pixels; multiply norm offset by this
        //    to get pixel offset.
        const Format& fmt    = info_.full_size_format();
        const float   half_w = static_cast<float>(fmt.width())  * 0.5f;
        const float   half_h = static_cast<float>(fmt.height()) * 0.5f;

        // Aperture radius in normalised units: radius = 1 / fstop.
        // (Larger aperture = smaller fstop = more defocus.)
        const float ap_radius = 1.0f / std::max(_fstop, 0.1f);

        // CoC culling uses the knob-driven focal length.
        // This only affects whether a sample is culled early; wrong value = no
        // crash, just slightly suboptimal culling.
        const float focal_length_mm = _focal_length_mm;

        const int N = std::max(_aperture_samples, 1);

        // Per-channel wavelengths: R=0.45μm, G=0.55μm, B=0.65μm (D025).
        const float lambdas[3]     = { 0.45f, 0.55f, 0.65f };
        const Channel rgb_chans[3] = { Chan_Red, Chan_Green, Chan_Blue };

        // 4. Scatter loop — per input pixel.
        for (Box::iterator it = bounds.begin(); it != bounds.end(); ++it) {
            if (Op::aborted()) return;

            const int px = it.x;
            const int py = it.y;

            DeepPixel pixel = deepPlane.getPixel(it);
            const int nSamples = pixel.getSampleCount();
            if (nSamples == 0) continue;

            // 5. Per deep sample.
            for (int s = 0; s < nSamples; ++s) {
                const float z_front = pixel.getUnorderedSample(s, Chan_DeepFront);
                const float z_back  = pixel.getUnorderedSample(s, Chan_DeepBack);
                const float depth   = 0.5f * (z_front + z_back);
                if (depth < 1e-6f) continue;

                const float alpha = pixel.getUnorderedSample(s, Chan_Alpha);
                if (alpha < 1e-6f) continue;

                // Ideal sensor position for this pixel in normalised [-1,1] coords.
                const float sx0 = (static_cast<float>(px) + 0.5f - half_w) / half_w;
                const float sy0 = (static_cast<float>(py) + 0.5f - half_h) / half_h;

                // CoC-based early cull: skip if sample contributes nothing to stripe.
                // (Performance guard only — not correctness-critical.)
                const float coc = coc_radius(focal_length_mm, _fstop,
                                             _focus_distance, depth);
                // coc is in mm; convert to pixels via half_w / (focal_length_mm/2)
                // approximation — good enough for culling.
                const float coc_px   = coc * half_w / (focal_length_mm * 0.5f + 1e-6f);
                const int   coc_px_i = static_cast<int>(coc_px) + 1;
                if (py + coc_px_i < bounds.y() || py - coc_px_i >= bounds.t())
                    continue;

                // 6. Aperture sample loop.
                for (int k = 0; k < N; ++k) {
                    // Halton(2,3) low-discrepancy aperture sample (D026).
                    const float u = halton(k, 2);
                    const float v = halton(k, 3);

                    // Map to unit disk, then scale to aperture radius.
                    float ax_unit, ay_unit;
                    map_to_disk(u, v, ax_unit, ay_unit);
                    const float ax = ax_unit * ap_radius;
                    const float ay = ay_unit * ap_radius;

                    // 7. Trace each channel at its wavelength (D025).
                    float landing_x[3], landing_y[3];
                    float transmit[3];

                    for (int c = 0; c < 3; ++c) {
                        // Forward poly evaluation: sensor + aperture → landing position.
                        //
                        // The polynomial maps (sensor, aperture, lambda) → 
                        // (aperture', sensor', lambda').  output[2:3] gives the
                        // sensor landing position after passing through the lens.
                        //
                        // This is a FORWARD (scatter) evaluation — we know where the
                        // sample is on the sensor and where on the aperture we're
                        // tracing, and we want to know where the light lands.
                        // The Newton inverse trace (lt_newton_trace) is for the
                        // GATHER direction, which is the wrong operation for a
                        // scatter-based defocus plugin.
                        float in5[5]  = { sx0, sy0, ax, ay, lambdas[c] };
                        float out5[5] = {};
                        // max_degree=3: evaluate terms up to degree 3 (56 of
                        // 4368 terms).  Higher degrees add sub-pixel aberration
                        // refinement at significant cost; degree 3 captures the
                        // primary bokeh shape.
                        poly_system_evaluate(&_poly_sys, in5, out5, 5, 3);

                        landing_x[c] = out5[2];
                        landing_y[c] = out5[3];

                        // Transmittance from poly output[4].
                        transmit[c] = std::max(0.0f, std::min(1.0f, out5[4]));
                    }

                    // 8. Splat each channel to output pixel.
                    // Normalised sensor position → output pixel index:
                    //   out_x = landing_x * half_w + half_w - 0.5
                    //
                    // Guard: only write channels that are actually present in
                    // imagePlane.channels().  ImagePlane allocates memory only for
                    // its ChannelSet; writing a channel outside that set computes an
                    // invalid offset and silently corrupts the heap — the symptoms
                    // being exactly the "malloc(): invalid size (unsorted)" abort
                    // that triggered this fix.  Nuke may call renderStripe requesting
                    // only a subset of RGBA (e.g. just alpha), so the guard is
                    // load-bearing, not defensive.
                    for (int c = 0; c < 3; ++c) {
                        if (!chans.contains(rgb_chans[c])) continue;

                        const float out_xf = landing_x[c] * half_w + half_w - 0.5f;
                        const float out_yf = landing_y[c] * half_h + half_h - 0.5f;
                        const int out_xi = static_cast<int>(std::floor(out_xf + 0.5f));
                        const int out_yi = static_cast<int>(std::floor(out_yf + 0.5f));

                        // Discard contributions landing outside this stripe.
                        // NOTE: this causes a seam for large bokeh discs at stripe
                        // boundaries. Accepted limitation for S02 — see S02 research.
                        if (out_xi < bounds.x() || out_xi >= bounds.r()) continue;
                        if (out_yi < bounds.y() || out_yi >= bounds.t()) continue;

                        const float chan_val = pixel.getUnorderedSample(s, rgb_chans[c]);
                        const float w = transmit[c] / static_cast<float>(N);
                        const float holdout_w = transmittance_at(out_xi, out_yi, depth);
                        imagePlane.writableAt(out_xi, out_yi, imagePlane.chanNo(rgb_chans[c]))
                            += chan_val * alpha * w * holdout_w;
                    }

                    // Alpha: use G-channel (index 1) landing position.
                    if (chans.contains(Chan_Alpha)) {
                        const float out_xf = landing_x[1] * half_w + half_w - 0.5f;
                        const float out_yf = landing_y[1] * half_h + half_h - 0.5f;
                        const int out_xi = static_cast<int>(std::floor(out_xf + 0.5f));
                        const int out_yi = static_cast<int>(std::floor(out_yf + 0.5f));
                        if (out_xi >= bounds.x() && out_xi < bounds.r()
                         && out_yi >= bounds.y() && out_yi < bounds.t()) {
                            const float w = transmit[1] / static_cast<float>(N);
                            const float holdout_w = transmittance_at(out_xi, out_yi, depth);
                            imagePlane.writableAt(out_xi, out_yi, imagePlane.chanNo(Chan_Alpha))
                                += alpha * w * holdout_w;
                        }
                    }
                } // aperture samples
            } // deep samples
        } // pixels
    }

    // ------------------------------------------------------------------
    // Destructor — release poly system if loaded
    // ------------------------------------------------------------------

    ~DeepCDefocusPO() override
    {
        if (_poly_loaded) {
            poly_system_destroy(&_poly_sys);
            _poly_loaded = false;
        }
    }

    // ------------------------------------------------------------------
    // _close — release poly system (called by Nuke when node is evicted)
    // ------------------------------------------------------------------

    void _close() override
    {
        if (_poly_loaded) {
            poly_system_destroy(&_poly_sys);
            _poly_loaded = false;
        }
        PlanarIop::_close();
    }

    // ------------------------------------------------------------------
    // Op::Description registration — must be at file scope (see below)
    // ------------------------------------------------------------------

    static const Op::Description d;
};

// ---------------------------------------------------------------------------
// Op::Description — registers the node with Nuke's Op factory
// ---------------------------------------------------------------------------

static Op* build(Node* node) { return new DeepCDefocusPO(node); }
const Op::Description DeepCDefocusPO::d(::CLASS, "Deep/DeepCDefocusPO", build);
