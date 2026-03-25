// SPDX-License-Identifier: MIT
//
// ============================================================================
//
//  DeepCDefocusPOThin — Thin-lens polynomial-optics defocus for deep images
//
//  Thin-lens variant: traces rays through a single polynomial lens system
//  (.fit file) to scatter each deep sample's contribution onto a flat RGBA
//  output tile. Uses the thin-lens approximation with configurable
//  max_degree truncation for speed/quality tradeoff.
//
//  Base class: PlanarIop — single-threaded renderStripe, no shared output
//  buffer races (correct for stochastic aperture scatter). See D021.
//
//  Inputs:
//    0 (required) — Deep stream to defocus
//    1 (optional) — Deep holdout mask (evaluated at output pixel, not
//                   pre-applied — prevents double-defocus, see D024)
//
//  Knobs:
//    poly_file        — path to lentil .fit polynomial lens file
//    focal_length     — lens focal length in mm (default 50.0)
//    focus_distance   — distance to focus plane, mm (default 200 mm = 2 m)
//    fstop            — lens f-stop (default 2.8)
//    aperture_samples — scatter samples per deep sample (default 64)
//    max_degree       — polynomial truncation degree (default 11, range 1–11)
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

static const char* const CLASS = "DeepCDefocusPOThin";
static const char* const HELP =
    "Thin-lens polynomial-optics defocus for deep images.\n\n"
    "Scatters each deep sample through a single lentil polynomial lens "
    "system (.fit file) using the thin-lens approximation to produce "
    "physically accurate bokeh on a flat RGBA output tile.\n\n"
    "poly_file — path to the lentil .fit binary polynomial lens file.\n"
    "focus_distance — distance from the lens to the focus plane (mm). "
    "Samples at this depth are not defocused.\n"
    "fstop — lens aperture. Lower values produce wider bokeh.\n"
    "aperture_samples — number of aperture points traced per deep sample. "
    "Higher values reduce Monte Carlo noise.\n"
    "max_degree — polynomial truncation degree (1–11). Lower values are "
    "faster but less accurate.\n\n"
    "Input 0: Deep stream to defocus (required).\n"
    "Input 1 (holdout): Optional deep holdout mask. Evaluated at the "
    "output pixel position at the sample depth — not pre-applied before "
    "scatter — to avoid double-defocus artifacts.\n\n"
    "Part of the DeepC plugin collection.";

// ---------------------------------------------------------------------------
// DeepCDefocusPOThin — PlanarIop subclass
// ---------------------------------------------------------------------------

class DeepCDefocusPOThin : public PlanarIop
{
    // ------------------------------------------------------------------
    // Member variables
    // ------------------------------------------------------------------

    const char* _poly_file;        // File_knob binds here (const char*)
    float       _focus_distance;   // distance to focus plane, mm
    float       _fstop;            // lens f-stop
    int         _aperture_samples; // aperture scatter samples per deep sample
    float       _focal_length_mm;  // lens focal length in mm (knob-driven)
    int         _max_degree;       // polynomial truncation degree (1–11)

    bool        _poly_loaded;      // true after poly_system_read succeeds
    bool        _reload_poly;      // set by knob_changed; cleared after reload

    poly_system_t _poly_sys;       // polynomial lens system (zeroed in ctor)

    // Chromatic aberration wavelengths (μm): R, G, B channels (D025).
    // Preserved from DeepCDefocusPO for use in S02 renderStripe.
    static constexpr float CA_LAMBDA_R = 0.45f;
    static constexpr float CA_LAMBDA_G = 0.55f;
    static constexpr float CA_LAMBDA_B = 0.65f;

public:
    // ------------------------------------------------------------------
    // Constructor
    // ------------------------------------------------------------------

    DeepCDefocusPOThin(Node* node)
        : PlanarIop(node)
        , _poly_file(nullptr)
        , _focus_distance(200.0f)
        , _fstop(2.8f)
        , _aperture_samples(64)
        , _focal_length_mm(50.0f)
        , _max_degree(11)
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

        Int_knob(f, &_max_degree, "max_degree", "max degree");
        SetRange(f, 1, 11);
        Tooltip(f, "Maximum polynomial degree for lens evaluation. Lower values "
                    "are faster but less accurate. Default 11 uses the full polynomial.");
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
    // _validate — propagate format from Deep input; load poly if needed
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

        // Load or reload the polynomial lens file when running for-real
        // and the file path is non-empty and either has never been loaded
        // or has changed since last load.
        if (for_real && _poly_file && _poly_file[0] != '\0'
            && (_reload_poly || !_poly_loaded))
        {
            // Destroy any previously loaded system before reading a new one.
            if (_poly_loaded) {
                poly_system_destroy(&_poly_sys);
                _poly_loaded = false;
            }

            const int rc = poly_system_read(&_poly_sys, _poly_file);
            if (rc != 0) {
                // poly_system_read already freed partial allocations — safe.
                error("Cannot open lens file: %s", _poly_file);
                return;
            }

            _poly_loaded  = true;
            _reload_poly  = false;
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
    // renderStripe — stub: zeros all output channels
    //
    // S02 will replace this with the full thin-lens PO scatter engine.
    // ------------------------------------------------------------------

    void renderStripe(ImagePlane& imagePlane) override
    {
        imagePlane.makeWritable();
        const Box& bounds = imagePlane.bounds();
        const ChannelSet& chans = imagePlane.channels();
        const Format& fmt = info_.full_size_format();
        const float half_w = fmt.width()  * 0.5f;
        const float half_h = fmt.height() * 0.5f;

        // ---- zero_output lambda (used for early returns) ----
        auto zero_output = [&]() {
            foreach(z, chans) {
                const int ci = imagePlane.chanNo(z);
                for (int y = bounds.y(); y < bounds.t(); ++y)
                    for (int x = bounds.x(); x < bounds.r(); ++x)
                        imagePlane.writableAt(x, y, ci) = 0.0f;
            }
        };

        // ---- Poly load/reload at renderStripe entry (M006 pattern) ----
        if (_poly_file && _poly_file[0] && (!_poly_loaded || _reload_poly)) {
            if (_poly_loaded) { poly_system_destroy(&_poly_sys); _poly_loaded = false; }
            if (poly_system_read(&_poly_sys, _poly_file) != 0) {
                error("Cannot open lens file: %s", _poly_file);
                zero_output();
                return;
            }
            _poly_loaded = true;
            _reload_poly = false;
        }
        if (!_poly_loaded || !input0()) {
            zero_output();
            return;
        }

        // ---- Zero the output buffer before accumulation ----
        zero_output();

        // ---- Deep channels needed ----
        ChannelSet deep_chans;
        deep_chans += Chan_Red;
        deep_chans += Chan_Green;
        deep_chans += Chan_Blue;
        deep_chans += Chan_Alpha;
        deep_chans += Chan_DeepFront;
        deep_chans += Chan_DeepBack;

        // ---- Holdout fetch ----
        DeepPlane holdoutPlane;
        const bool has_holdout = (input1() != nullptr);
        if (has_holdout) {
            ChannelSet holdout_chans;
            holdout_chans += Chan_Alpha;
            holdout_chans += Chan_DeepFront;
            holdout_chans += Chan_DeepBack;
            input1()->deepEngine(bounds, holdout_chans, holdoutPlane);
        }

        auto transmittance_at = [&](int px, int py, float Z) -> float {
            if (!has_holdout) return 1.0f;
            // getPixel(y, x) — row-first DDImage convention
            DeepPixel hp = holdoutPlane.getPixel(py, px);
            float transmit = 1.0f;
            for (int s = 0; s < hp.getSampleCount(); ++s) {
                float hzf = hp.getUnorderedSample(s, Chan_DeepFront);
                if (hzf >= Z) continue;  // skip samples at or behind scatter depth
                float ha = hp.getUnorderedSample(s, Chan_Alpha);
                transmit *= (1.0f - ha);
            }
            return transmit;
        };

        // ---- Deep input fetch ----
        DeepPlane deepPlane;
        if (!input0()->deepEngine(bounds, deep_chans, deepPlane)) {
            return;  // already zeroed
        }

        // ---- Scatter constants ----
        const int N = std::max(_aperture_samples, 1);
        const float inv_N = 1.0f / static_cast<float>(N);
        const float ap_radius = 1.0f / (_fstop + 1e-6f);

        // Per-channel CA descriptor: {DDImage channel, wavelength}
        struct ChanWave { Channel chan; float lambda; };
        const ChanWave ca_table[3] = {
            { Chan_Red,   CA_LAMBDA_R },
            { Chan_Green, CA_LAMBDA_G },
            { Chan_Blue,  CA_LAMBDA_B },
        };

        // ---- Per-pixel, per-sample, per-aperture scatter loop ----
        for (int y = bounds.y(); y < bounds.t(); ++y) {
            for (int x = bounds.x(); x < bounds.r(); ++x) {
                // Normalised sensor coords for this pixel
                const float sx = (x + 0.5f - half_w) / half_w;
                const float sy = (y + 0.5f - half_h) / half_h;

                DeepPixel dp = deepPlane.getPixel(y, x);
                const int sample_count = dp.getSampleCount();

                for (int s = 0; s < sample_count; ++s) {
                    const float Z = dp.getUnorderedSample(s, Chan_DeepFront);
                    const float sR = dp.getUnorderedSample(s, Chan_Red);
                    const float sG = dp.getUnorderedSample(s, Chan_Green);
                    const float sB = dp.getUnorderedSample(s, Chan_Blue);
                    const float sA = dp.getUnorderedSample(s, Chan_Alpha);

                    // Thin-lens CoC in normalised coords
                    const float coc_mm = coc_radius(_focal_length_mm, _fstop,
                                                     _focus_distance, Z);
                    const float coc_norm = coc_mm / (_focal_length_mm * 0.5f + 1e-6f);

                    // Aperture sample loop
                    for (int k = 0; k < N; ++k) {
                        float ax_unit, ay_unit;
                        map_to_disk(halton(k, 2), halton(k, 3), ax_unit, ay_unit);

                        // ---- Per-channel CA: R, G, B ----
                        const float sample_colors[3] = { sR, sG, sB };

                        // We'll track the green-channel landing for alpha
                        int alpha_out_px = x, alpha_out_py = y;
                        float alpha_transmit = 1.0f;

                        for (int c = 0; c < 3; ++c) {
                            const Channel chan = ca_table[c].chan;
                            const float lambda = ca_table[c].lambda;

                            if (!chans.contains(chan)) continue;

                            const float ax = ax_unit * ap_radius;
                            const float ay = ay_unit * ap_radius;

                            float in5[5] = { sx, sy, ax, ay, lambda };
                            float out5[5] = {};
                            poly_system_evaluate(&_poly_sys, in5, out5, 5,
                                                 _max_degree);

                            const float transmit = std::max(0.0f,
                                                   std::min(1.0f, out5[4]));

                            // Option B warp: out5[0:1] are warped aperture positions
                            float wx = out5[0];
                            float wy = out5[1];
                            // Clamp magnitude to ap_radius
                            const float wmag = std::sqrt(wx * wx + wy * wy);
                            if (wmag > ap_radius && wmag > 1e-9f) {
                                wx *= ap_radius / wmag;
                                wy *= ap_radius / wmag;
                            }

                            // Scale warped position to CoC
                            const float landing_x = sx + coc_norm * wx
                                                    / (ap_radius + 1e-6f);
                            const float landing_y = sy + coc_norm * wy
                                                    / (ap_radius + 1e-6f);

                            // Convert landing to pixel coords
                            const float out_px_f = landing_x * half_w + half_w - 0.5f;
                            const float out_py_f = landing_y * half_h + half_h - 0.5f;
                            const int out_px_i = static_cast<int>(std::round(out_px_f));
                            const int out_py_i = static_cast<int>(std::round(out_py_f));

                            // Bounds check
                            if (out_px_i < bounds.x() || out_px_i >= bounds.r() ||
                                out_py_i < bounds.y() || out_py_i >= bounds.t())
                                continue;

                            const float holdout_t = transmittance_at(
                                                        out_px_i, out_py_i, Z);
                            const float weight = transmit * holdout_t * inv_N;

                            imagePlane.writableAt(out_px_i, out_py_i,
                                imagePlane.chanNo(chan)) += sample_colors[c] * weight;

                            // Track green-channel landing for alpha
                            if (c == 1) {
                                alpha_out_px = out_px_i;
                                alpha_out_py = out_py_i;
                                alpha_transmit = transmit;
                            }
                        }

                        // ---- Alpha: use green-channel (0.55μm) landing position ----
                        if (chans.contains(Chan_Alpha) &&
                            alpha_out_px >= bounds.x() && alpha_out_px < bounds.r() &&
                            alpha_out_py >= bounds.y() && alpha_out_py < bounds.t())
                        {
                            const float holdout_t = transmittance_at(
                                                        alpha_out_px, alpha_out_py, Z);
                            const float weight = alpha_transmit * holdout_t * inv_N;
                            imagePlane.writableAt(alpha_out_px, alpha_out_py,
                                imagePlane.chanNo(Chan_Alpha)) += sA * weight;
                        }
                    }  // k (aperture samples)
                }  // s (deep samples)
            }  // x
        }  // y
    }

    // ------------------------------------------------------------------
    // Destructor — release poly system if loaded
    // ------------------------------------------------------------------

    ~DeepCDefocusPOThin() override
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

static Op* build(Node* node) { return new DeepCDefocusPOThin(node); }
const Op::Description DeepCDefocusPOThin::d(::CLASS, "Deep/DeepCDefocusPOThin", build);
