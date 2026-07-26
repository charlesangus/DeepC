// SPDX-License-Identifier: MIT
//
// ============================================================================
//
//  DeepCDefocus — Deep-input, flat-output defocus/DOF node
//
//  Phase 1.2 skeleton (M1.P2.T1): Iop subclass shape, input wiring, and the
//  full knob set from the milestone design doc. No flatten behaviour lands
//  here — that is M1.P2.T2. _validate()/_request()/engine() are present only
//  as the minimum safe overrides: Iop's defaults for the first two reach the
//  inputs through a static_cast<Iop*>, which is undefined behaviour (and a
//  verified Nuke core dump) against this node's DeepOp inputs, and engine()
//  is stubbed to the one invariant the design mandates unconditionally —
//  row.erase(channels) as its first statement, so a premature call cannot
//  emit garbage into sparse deep pixels.
//
//  Node shape (PLAN/MILESTONES/M1-deepcdefocus-v1.md, "Node shape"):
//    - Iop subclass (not DeepFilterOp — this is the first in-repo node that
//      consumes a deep input and produces flat 2D output), inputs(2).
//    - Input 0: deep source, required. test_input() -> dynamic_cast<DeepOp*>.
//    - Input 1: deep holdout, optional. default_input() -> nullptr, so a
//      disconnected input 1 stays disconnected rather than substituting a
//      black Iop; the "holdout enabled" question is decided purely by
//      whether input 1 is connected (no separate enable knob).
//    - Modelled on Foundry's "Deep to 2D Ops" pattern; the in-tree precedent
//      is the NDK's own DeepToImage.cpp example (single deep-in Iop), not
//      any node in this repo's src/ (all of which are DeepFilterOp).
//
//  Knob set: full inventory from the milestone's "Knob list (grouped)"
//  table. Every default/range below matches that table exactly; the values
//  are unused by any behaviour until M1.P2.T2 (_validate/_request/engine)
//  and later phases (scatter, holdout, concurrency) wire them up.
//
// ============================================================================

#include "DDImage/Iop.h"
#include "DDImage/DeepOp.h"
#include "DDImage/Row.h"
#include "DDImage/Knobs.h"

#include "DeepCDefocusMath.h"

using namespace DD::Image;

static const char* const CLASS = "DeepCDefocus";
static const char* const HELP =
    "Deep-input, flat-output defocus (depth of field) node.\n\n"
    "Input 0 is the required deep source; input 1 is an optional deep "
    "holdout — connecting it enables depth-correct holdout compositing "
    "(the holdout stays pixel-sharp, never defocused, because visibility "
    "is evaluated per destination pixel, per fragment depth, after "
    "scatter). There is no separate holdout enable knob: connection "
    "presence is the switch.\n\n"
    "This is the Phase 1.2 skeleton: inputs, knobs, and the Iop shape are "
    "in place, but the scatter/composite path is not wired up yet.\n\n"
    "Part of the DeepC plugin collection.";

// ---------------------------------------------------------------------------
// Enum knob label tables
// ---------------------------------------------------------------------------
static const char* const cocModeNames[] = {
    "Physical", "Manual", nullptr
};

static const char* const worldUnitsNames[] = {
    "mm", "cm", "dm", "m", "in", "ft", nullptr
};

// ---------------------------------------------------------------------------
class DeepCDefocus : public DD::Image::Iop
{
    // --- Focus ---------------------------------------------------------
    int   _cocMode;              // Enum {Physical, Manual}, default Physical
    float _focusDistance;        // Float, default 10.0, 0.001-1e6, log slider
    int   _worldUnits;           // Enum {mm, cm, dm, m, in, ft}, default m
    bool  _depthIsRayDistance;   // Bool, default false

    // --- Lens ------------------------------------------------------------
    float _focalLength;          // Float mm, default 50, 8-300 (Physical)
    float _fstop;                // Float, default 2.8, 0.7-32
    float _filmbackWidth;        // Float mm, default 36.0, 4-70

    // --- Focus (the knob table groups `size` under Focus, not Lens) ------
    float _size;                 // Float px, default 10, 0-100 (Manual)

    // --- Bokeh -----------------------------------------------------------
    float _frontCocMult;         // Float, default 1.0, 0-4
    float _backCocMult;          // Float, default 1.0, 0-4
    float _edgeSoftness;         // Float px, default 1.0, 0-4

    // --- Output ------------------------------------------------------------
    ChannelSet _channels;        // Input_ChannelSet, default rgba
    bool       _outputHoldoutMatte;    // Bool, default false
    Channel    _holdoutMatteChannel;   // Channel, paired with the bool above.
                                       // Defaults to Chan_Black ("none"): the
                                       // knob table leaves this default
                                       // unspecified, and any real channel
                                       // would be silently overwritten the
                                       // moment the bool is ticked — Chan_Alpha
                                       // in particular would destroy the node's
                                       // own alpha output.

    // --- Perf --------------------------------------------------------------
    int   _maxRadius;            // Int px, default 100, 1-500
    int   _depthLayers;          // Int, default 16, 4-128 (K buckets)
    bool  _preMerge;             // Bool, default true (tidy pass is always on)
    float _mergeTolerance;       // Float, default 0.25px, 0-2px
    float _memoryLimit;          // Float GB, default 4.0, 1-64

public:
    DeepCDefocus(Node* node) : Iop(node),
        _cocMode(static_cast<int>(deepc::CocMode::Physical)),
        _focusDistance(10.0f),
        _worldUnits(static_cast<int>(deepc::WorldUnits::Meters)),
        _depthIsRayDistance(false),
        _focalLength(50.0f),
        _fstop(2.8f),
        _filmbackWidth(36.0f),
        _size(10.0f),
        _frontCocMult(1.0f),
        _backCocMult(1.0f),
        _edgeSoftness(1.0f),
        _channels(Mask_RGBA),
        _outputHoldoutMatte(false),
        _holdoutMatteChannel(Chan_Black),
        _maxRadius(100),
        _depthLayers(16),
        _preMerge(true),
        _mergeTolerance(0.25f),
        _memoryLimit(4.0f)
    {
        inputs(2);  // input 0 = deep source (required), input 1 = deep holdout (optional)
    }

    int minimum_inputs() const override { return 1; }
    int maximum_inputs() const override { return 2; }

    // ------------------------------------------------------------------
    // Input 0: deep source, required. Input 1: deep holdout, optional —
    // both accept any DeepOp, distinguished only by index.
    // ------------------------------------------------------------------
    bool test_input(int input, Op* op) const override
    {
        switch (input) {
            case 0:
            case 1:
                return dynamic_cast<DeepOp*>(op) != nullptr;
            default:
                return false;
        }
        return false;
    }

    // No default substitute for either input: a disconnected input 0 means
    // "no deep source" (an error the later _validate task handles) and a
    // disconnected input 1 means "no holdout" (a valid, zero-cost state).
    Op* default_input(int input) const override
    {
        switch (input) {
            case 0:
                return nullptr;
            case 1:
                return nullptr;
            default:
                return nullptr;
        }
        return nullptr;
    }

    const char* input_label(int input, char*) const override
    {
        switch (input) {
            case 0:
                return "";
            case 1:
                return "holdout";
            default:
                return "";
        }
        return "";
    }

    // Convenience accessors, mirroring the DeepFilterOp::input0() idiom used
    // elsewhere in this repo (this node isn't a DeepFilterOp, so it isn't
    // inherited — it's redeclared here for the same call-site convenience).
    //
    // DANGER, and the reason these exist: Iop's own input accessors —
    // Iop::input0(), Iop::input1(), Iop::input(int) — all funnel through
    // Iop::asIop(), which is a bare static_cast<Iop*> (the dynamic_cast is
    // only an assert in debug builds). This node's inputs are DeepOps, which
    // are NOT Iops, so any use of the inherited accessors is undefined
    // behaviour and crashes in a release Nuke. Every input access in this
    // file — now and in M1.P2.T2 and beyond — must go through Op::input(n)
    // and a dynamic_cast, i.e. through these two helpers.
    DeepOp* input0() const { return dynamic_cast<DeepOp*>(Op::input(0)); }
    DeepOp* input1() const { return dynamic_cast<DeepOp*>(Op::input(1)); }

    // The Nuke viewer picks 2D/3D from this. Iop's default returns
    // (2D | 3D) when an Iop has inputs of a different type than itself —
    // which is exactly this node — so it must be pinned to 2D, as the NDK's
    // own DeepToImage example does.
    int getViewableModes() const override { return eViewableMode2D; }

    const char* Class() const override { return CLASS; }
    const char* node_help() const override { return HELP; }

    // ------------------------------------------------------------------
    // Knobs — full set from the milestone's "Knob list (grouped)" table.
    // Values are unused until _validate/_request/engine (M1.P2.T2) and the
    // scatter/holdout phases wire them up.
    // ------------------------------------------------------------------
    void knobs(Knob_Callback f) override
    {
        // --- Focus -------------------------------------------------------
        Enumeration_knob(f, &_cocMode, cocModeNames, "coc_mode", "coc mode");
        Tooltip(f, "Physical: CoC derived from focal length / fstop / focus "
                    "distance / filmback width.\n"
                    "Manual: CoC derived directly from the size knob.");

        Float_knob(f, &_focusDistance, IRange(0.001, 1e6), "focus_distance", "focus distance");
        Tooltip(f, "Distance to the focal plane, in scene Z units (see world_units).");

        Enumeration_knob(f, &_worldUnits, worldUnitsNames, "world_units", "world units");
        Tooltip(f, "Scene-unit -> millimetre scale, required for physical-mode "
                    "correctness. Mixing mm and scene units is wrong by orders "
                    "of magnitude.");

        Bool_knob(f, &_depthIsRayDistance, "depth_is_ray_distance", "depth is ray distance");
        Tooltip(f, "Correct a ray-length depth channel to camera-space Z using "
                    "the focal length and each pixel's filmback offset.");

        Float_knob(f, &_size, IRange(0.0, 100.0), "size", "size");
        Tooltip(f, "Blur radius in pixels at d = infinity (Manual mode); "
                    "unit-agnostic, unrelated to world_units.");

        // --- Lens ----------------------------------------------------------
        Divider(f, "Lens");

        Float_knob(f, &_focalLength, IRange(8.0, 300.0), "focal_length", "focal length");
        Tooltip(f, "Lens focal length in mm (Physical mode).");

        Float_knob(f, &_fstop, IRange(0.7, 32.0), "fstop", "fstop");
        Tooltip(f, "Lens aperture, f-number (Physical mode).");

        Float_knob(f, &_filmbackWidth, IRange(4.0, 70.0), "filmback_width", "filmback width");
        Tooltip(f, "Filmback (sensor) width in mm (Physical mode).");

        // --- Bokeh ---------------------------------------------------------
        Divider(f, "Bokeh");

        Float_knob(f, &_frontCocMult, IRange(0.0, 4.0), "front_coc_mult", "front coc mult");
        Tooltip(f, "Radius multiplier for samples in front of the focal plane.");

        Float_knob(f, &_backCocMult, IRange(0.0, 4.0), "back_coc_mult", "back coc mult");
        Tooltip(f, "Radius multiplier for samples behind the focal plane.");

        Float_knob(f, &_edgeSoftness, IRange(0.0, 4.0), "edge_softness", "edge softness");
        Tooltip(f, "Width, in pixels, of the anti-aliased disc-edge falloff band.");

        // --- Output ----------------------------------------------------------
        Divider(f, "Output");

        Input_ChannelSet_knob(f, &_channels, 0, "channels", "channels");
        Tooltip(f, "Channels to defocus, from input 0. Alpha is always "
                    "processed regardless of this selection.");

        Bool_knob(f, &_outputHoldoutMatte, "output_holdout_matte", "output holdout matte");
        Tooltip(f, "Write a flattened holdout coverage AOV (1 - vis(infinity)) "
                    "to the channel below. Has no effect unless input 1 is "
                    "connected.");

        Channel_knob(f, &_holdoutMatteChannel, 1, "holdout_matte_channel", "matte channel");
        Tooltip(f, "Destination channel for the holdout coverage AOV. Defaults "
                    "to none — pick or create a channel (the popup's \"new\" "
                    "option) rather than overwriting an existing one.");

        // --- Perf --------------------------------------------------------------
        Divider(f, "Perf");

        Int_knob(f, &_maxRadius, IRange(1, 500), "max_radius", "max radius");
        Tooltip(f, "Hard bound, in pixels, on the scatter radius. Bounds the "
                    "output bbox pad and the kernel LUT's worst-case size.");

        Int_knob(f, &_depthLayers, IRange(4, 128), "depth_layers", "depth layers");
        Tooltip(f, "Number (K) of depth buckets used by the scatter "
                    "compositor. Memory scales with K.");

        Bool_knob(f, &_preMerge, "pre_merge", "pre-merge");
        Tooltip(f, "Merge adjacent-depth samples within merge_tolerance before "
                    "scatter (lossless when radii are equal). The tidy pass "
                    "itself is correctness-required and always on.");

        Float_knob(f, &_mergeTolerance, IRange(0.0, 2.0), "merge_tolerance", "merge tolerance");

        Float_knob(f, &_memoryLimit, IRange(1.0, 64.0), "memory_limit", "memory limit (GB)");
        Tooltip(f, "Caps concurrent in-flight bands (floors at 1 band, then "
                    "shrinks the band height — never deadlocks at 0).");
    }

    // ------------------------------------------------------------------
    // _validate()/_request()/engine() — the real flatten path is M1.P2.T2.
    // These three are NOT optional stubs, they are mandatory safety
    // overrides: Iop's default _validate() merges info from every input and
    // its default _request() forwards the request to every input, and both
    // reach the inputs through Iop::asIop()'s static_cast<Iop*>. With DeepOp
    // inputs that is undefined behaviour — verified: leaving _validate() to
    // the base class core-dumps Nuke 17.0v3 the moment the node is
    // validated with a deep source connected.
    //
    // Until M1.P2.T2 lands, the node therefore reports an empty image and
    // no output channels, which is the honest description of a skeleton
    // that cannot yet flatten anything. M1.P2.T2 replaces the body of
    // _validate() with the deepInfo -> info_ mapping (bbox padded by
    // ceil(max_radius + edge_softness/2)) and the body of _request() with
    // the padded deep pull via neededDeepChannels(); the "don't fall
    // through to Iop" constraint above survives that change and must not be
    // undone by calling Iop::_validate()/Iop::_request() from them.
    // ------------------------------------------------------------------
    void _validate(bool /*forReal*/) override
    {
        info_.set(DD::Image::Box());
        info_.channels(Mask_None);
        set_out_channels(Mask_None);
    }

    void _request(int /*x*/, int /*y*/, int /*r*/, int /*t*/,
                  ChannelMask /*channels*/, int /*count*/) override
    {
    }

    // row.erase(channels) is the one invariant that already applies
    // unconditionally: it must be engine()'s first statement, so sparse deep
    // pixels can never emit garbage.
    void engine(int y, int x, int r, ChannelMask channels, Row& row) override
    {
        row.erase(channels);
        (void)y;
        (void)x;
        (void)r;
    }

    static const Op::Description d;
};

// ---------------------------------------------------------------------------
static Op* build(Node* node) { return new DeepCDefocus(node); }
const Op::Description DeepCDefocus::d(::CLASS, "Deep/DeepCDefocus", build);
