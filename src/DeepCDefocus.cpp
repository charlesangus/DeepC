// SPDX-License-Identifier: MIT
//
// ============================================================================
//
//  DeepCDefocus — Deep-input, flat-output defocus/DOF node
//
//  State: M1.P3.T5 — the node defocuses.  engine() serves rows out of a
//  hash-keyed frame cache that one render thread fills, under a single
//  frame-wide lock, by:
//
//    computeDepthRange()  alpha-weighted DeepFront/DeepBack/Alpha pass
//      -> DepthBuckets (bounded ΔCoC) + HoldoutBoundaries (uniform in Z,
//         frame-global) + DiscKernelLUT over the MEASURED radius range
//    computeBand() per horizontal band:
//         fetch band +/- padY source rows -> flattenPixelToSoA
//      -> holdout fetch (skipped entirely when it cannot matter) -> HoldoutLut
//      -> scatterBandCPU -> resolveBandCPU (saturate down, then composite)
//      -> write the band's disjoint region of the flat frame
//
//  Per-band concurrency is M1.P4.T1: this phase is deliberately serial so
//  correctness lands before the execution model changes.  Two standing
//  invariants that predate this task and outlive it: _validate()/_request()
//  must NEVER fall through to Iop's (they reach inputs through a bare
//  static_cast<Iop*> and this node's inputs are DeepOps — a verified Nuke
//  core dump), and row.erase(channels) is engine()'s first statement.
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
#include "DDImage/Hash.h"
#include "DDImage/Thread.h"

#include "DeepCDefocusMath.h"
#include "DeepCDefocusScatter.h"
#include "DeepSampleOptimizer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

using namespace DD::Image;

// ---------------------------------------------------------------------------
// Hard caps applied at every use site.
//
// The knob IRanges are deliberately *soft* (IRange::force defaults to false),
// so a user can type max_radius = 5000 into the panel. Every consumer of a
// knob value therefore clamps it here rather than trusting the slider bound —
// the design's memory formulas (K*W*B*(C+3)*4 per band, LUT ~2*pi*R^3/3) are
// only bounded if the use sites do the clamping. These caps are shared by
// every phase of this node so the bbox pad, the LUT extent and the scatter
// loop can never disagree about how big "big" is.
// ---------------------------------------------------------------------------
static const int   kMaxRadiusCap    = 2000;   // px
static const float kEdgeSoftnessCap = 64.0f;  // px

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

// --- BAKE-OFF KNOBS: ALL DECIDED, ALL DELETED -------------------------------
//
// The milestone carried two undecided candidates that had to be judged from
// rendered pixels rather than derived: the bucket composite and the holdout
// interpolant's opaque-step behaviour (M1.P3.T11).  Each was a runtime enum
// in the scatter core precisely so ONE build could render every candidate
// through a temporary knob here.  **M1.P3.T17 decided the bucket composite
// from pixels and deleted the loser** (`bucket_combine` and its enum are
// gone); **M1.P3.T18 did the same for the holdout interpolant**
// (`holdout_interp`, `HoldoutInterp` and the two losing variants are gone —
// the log chord won; see "THE HOLDOUT INTERPOLANT — DECIDED" in
// DeepCDefocusMath.h for the rendered numbers).  There is one composite and
// one interpolant now, and nothing to select.  The harness still sets
// `pre_merge` and `merge_tolerance` explicitly on every render.

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

    // (The two temporary bake-off knobs are gone: `bucket_combine` at
    // M1.P3.T17, `holdout_interp` at M1.P3.T18 — both decided from rendered
    // pixels, losers deleted.)

    // ----------------------------------------------------------------------
    // Derived state — rebuilt by _validate(), read by _request()/engine().
    // ----------------------------------------------------------------------
    deepc::CocParams _cocParams;    // cached lens state, ALREADY PROXY-SCALED
                                    // (applyProxyScale() is called exactly once
                                    // per _validate, on freshly built params);
                                    // _validate deliberately does NOT build the
                                    // kernel LUT — that waits until
                                    // computeFrame() knows the frame's measured
                                    // CoC range, see the milestone Decisions
    float _proxyScale;              // current format width / full-size width
    float _formatHeightPx;          // current (proxy) format height, for the
                                    // ray-distance correction's filmback offset
    ChannelSet _outChannels;        // advertised output: selection u alpha (+ matte)
    ChannelSet _flattenChannels;    // what the cook writes: selection u alpha

    // ----------------------------------------------------------------------
    // Frame cache.
    //
    // The precompute is frame-wide, not row-wide: engine() copies rows out of
    // a planar float buffer that one render thread filled. The buffer is
    // handed out as a shared_ptr snapshot so a reader never touches storage
    // that a concurrent recompute might be reallocating — the lock is only
    // held while swapping the pointer in (and while the one thread that wins
    // the race does the compute), never while rows are being copied out.
    //
    // Plane-major layout, planes.size() * box.h() * box.w() floats. This is a
    // deliberately plain std::vector<float>: M1.P3.T1 introduces PodBuffer<T>
    // in DeepCDefocusScatter.h as the project-wide owning POD buffer, and this
    // cache is its natural first adopter — it should be ported to PodBuffer
    // there rather than a competing wrapper being invented here.
    //
    // Ported at M1.P3.T1: `data` is now a deepc::PodBuffer<float>, so every
    // plane buffer in this node goes through the one allocation seam the CUDA
    // milestone swaps. Two consequences worth knowing: the buffer's BASE is
    // 64-byte aligned (individual plane starts are not — plane p begins at
    // p*w*h floats, which is cache-line aligned only when w*h happens to be a
    // multiple of 16, so don't rely on it for aligned loads), and FrameCache is
    // now move-only. Nothing copies it — it is built once inside
    // make_shared<FrameCache>() and published as a shared_ptr<const> — and the
    // compiler will say so loudly if that ever changes.
    // ----------------------------------------------------------------------
    struct FrameCache {
        DD::Image::Box        box;
        std::vector<Channel>  planes;
        deepc::PodBuffer<float> data;

        int planeIndex(Channel z) const
        {
            for (size_t i = 0; i < planes.size(); ++i) {
                if (planes[i] == z)
                    return static_cast<int>(i);
            }
            return -1;
        }

        size_t planeStride() const
        {
            return static_cast<size_t>(box.w()) * static_cast<size_t>(box.h());
        }

        float* rowPtr(int plane, int y)
        {
            return data.data() + static_cast<size_t>(plane) * planeStride()
                 + static_cast<size_t>(y - box.y()) * static_cast<size_t>(box.w());
        }

        const float* rowPtr(int plane, int y) const
        {
            return data.data() + static_cast<size_t>(plane) * planeStride()
                 + static_cast<size_t>(y - box.y()) * static_cast<size_t>(box.w());
        }
    };

    mutable DD::Image::Lock            _cacheLock;
    std::shared_ptr<const FrameCache>  _cache;      // guarded by _cacheLock
    DD::Image::Hash                    _cachedHash; // guarded by _cacheLock

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
        _memoryLimit(4.0f),
        _proxyScale(1.0f),
        _formatHeightPx(1080.0f),
        _outChannels(Mask_None),
        _flattenChannels(Mask_None)
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
    // Clamped knob accessors.
    //
    // Every knob range in this node is soft, so nothing downstream of a knob
    // may use its raw value (milestone Decisions, 2026-07-26). These are the
    // use-site clamps for the values M1.P2.T2 actually consumes; the scatter
    // and concurrency phases add their own for K, memory_limit and friends.
    // ------------------------------------------------------------------
    int clampedMaxRadius() const
    {
        return std::max(0, std::min(_maxRadius, kMaxRadiusCap));
    }

    float clampedEdgeSoftness() const
    {
        if (!(_edgeSoftness > 0.0f))
            return 0.0f;
        return std::min(_edgeSoftness, kEdgeSoftnessCap);
    }

    // K, the bucket count. DepthBuckets' own storage is sized for [4, 128], so
    // this clamp is not cosmetic: the memory formula K*W*B*(C+3)*4 is only
    // bounded because of it, and buildBoundedDeltaCoc() would clamp anyway.
    int clampedDepthLayers() const
    {
        return std::max(deepc::DepthBuckets::kMinBuckets,
                        std::min(_depthLayers, deepc::DepthBuckets::kMaxBuckets));
    }

    // Pre-merge tolerance, in CoC-RADIUS pixels (milestone Decisions). It is a
    // radius, so it is proxy-scaled exactly like the radii it is compared
    // against — applyProxyScale() cannot do it, because CocParams does not
    // carry the tolerance (same reason edge_softness is scaled at the LUT
    // build rather than in the params).
    float clampedMergeTolerancePx() const
    {
        const float t = (_mergeTolerance > 0.0f) ? std::min(_mergeTolerance, 16.0f) : 0.0f;
        return t * _proxyScale;
    }

    // memory_limit, in bytes. Soft range 1-64 GB; the floor is deliberately
    // well below the documented minimum so a user who types 0 gets the
    // smallest band this phase can compute rather than a division by zero.
    double memoryLimitBytes() const
    {
        const double gb = (_memoryLimit > 0.0625f) ? static_cast<double>(_memoryLimit) : 0.0625;
        return std::min(gb, 1024.0) * 1024.0 * 1024.0 * 1024.0;
    }

    // Output bbox pad, X. The anti-aliased disc edge is *centred* on the rim,
    // so the kernel's true nonzero extent runs half a softness beyond
    // max_radius; padding by max_radius alone would clip that outermost
    // scattered energy at the frame edge (milestone Decisions).
    int bboxPadX() const
    {
        return static_cast<int>(std::ceil(static_cast<float>(clampedMaxRadius())
                                          + 0.5f * clampedEdgeSoftness()));
    }

    // ...and Y, which is the X pad times the pixel aspect: the disc is a
    // circle in *square* pixels, so an anamorphic format stretches it in Y.
    int bboxPadY(float pixelAspect) const
    {
        const float a = (pixelAspect > 0.0f && std::isfinite(pixelAspect)) ? pixelAspect : 1.0f;
        return static_cast<int>(std::ceil(static_cast<float>(bboxPadX()) * a));
    }

    // ------------------------------------------------------------------
    // neededDeepChannels() — THE single source of truth for which deep
    // channels this node pulls from input 0.
    //
    // Contract: the channel selection, union {DeepFront, DeepBack, Alpha}.
    //   - the selection, because those are the channels being defocused;
    //   - DeepFront/DeepBack, because every sample's depth span drives CoC,
    //     bucketing and the tidy pre-pass (they are consumed internally and
    //     never appear in the output);
    //   - Alpha, because compositing needs it whether or not the user
    //     selected it (and the output always carries it).
    //
    // Both _request() and every precompute fetch MUST call this and nothing
    // else. Request/engine channel divergence is a known failure class in
    // this node's design (risk register), and the mitigation is precisely
    // that there is exactly one function that answers the question.
    // ------------------------------------------------------------------
    ChannelSet neededDeepChannels() const
    {
        ChannelSet chans = _channels;
        chans += Mask_Deep;    // Chan_DeepFront | Chan_DeepBack
        chans += Mask_Alpha;
        return chans;
    }

    // Holdout pulls depth + alpha only — it contributes visibility, never
    // colour, so requesting its colour channels would be wasted bandwidth.
    // Mirrored identically in _request() and (from M1.P3.T3) the fetch.
    static ChannelSet neededHoldoutChannels()
    {
        ChannelSet chans = Mask_Deep;
        chans += Mask_Alpha;
        return chans;
    }

    // ------------------------------------------------------------------
    // _validate()
    //
    // DANGER (milestone Decisions, verified core dump): this must never fall
    // through to Iop::_validate(). Iop::_validate() merges info from all
    // inputs via Iop::asIop(), which is a bare static_cast<Iop*> — this
    // node's inputs are DeepOps, which are not Iops, and the cast crashes
    // Nuke 17.0v3. Same for Iop::input0()/input(int); use this class's own
    // input0()/input1().
    //
    // Note what is deliberately absent: the kernel LUT is NOT built here.
    // It is built once the frame's measured CoC range is known, immediately
    // after computeDepthRange() at M1.P3.T5 — building it eagerly over
    // [0, max_radius] costs ~1.0GB at max_radius=500 (milestone Decisions).
    // _validate() cannot know that range: it is measured per cook, so the LUT
    // belongs to computeFrame() and nothing here may depend on it.
    // ------------------------------------------------------------------
    void _validate(bool forReal) override
    {
        DeepOp* src = input0();

        if (!src) {
            // No deep source: an honest empty image. Still never delegates
            // to Iop::_validate().
            _outChannels     = Mask_None;
            _flattenChannels = Mask_None;
            info_.set(DD::Image::Box());
            info_.channels(Mask_None);
            set_out_channels(Mask_None);
            return;
        }

        src->validate(forReal);

        DeepOp* holdout = input1();
        if (holdout)
            holdout->validate(forReal);

        const DeepInfo& deepInfo = src->deepInfo();

        // --- output channel set ---------------------------------------
        // selection u alpha, minus the deep depth channels (consumed
        // internally, never output), plus the holdout matte AOV if enabled.
        ChannelSet out = _channels;
        out -= Mask_Deep;
        out += Mask_Alpha;
        _flattenChannels = out;

        if (_outputHoldoutMatte && _holdoutMatteChannel != Chan_Black)
            out += _holdoutMatteChannel;
        _outChannels = out;

        // --- geometry --------------------------------------------------
        info_.setFormats(deepInfo.formats());

        const Format* fmt = deepInfo.format();
        const float pixelAspect = fmt ? static_cast<float>(fmt->pixel_aspect()) : 1.0f;

        DD::Image::Box box = deepInfo.box();
        box.pad(bboxPadX(), bboxPadY(pixelAspect));

        info_.set(box);
        info_.channels(_outChannels);
        info_.setFirstFrame(deepInfo.firstFrame());
        info_.setLastFrame(deepInfo.lastFrame());

        set_out_channels(_outChannels);

        // --- cached lens state -----------------------------------------
        // Clamped at the point of use, per the soft-range decision. The
        // format width makes the mm knobs resolution-independent, so proxy
        // scaling comes for free.
        const float formatWidth = fmt ? static_cast<float>(fmt->width()) : 1920.0f;
        _formatHeightPx = fmt ? static_cast<float>(fmt->height()) : 1080.0f;

        // Proxy scale = current format width / full-size format width. The mm
        // knobs are resolution-independent for free (formatWidth above is the
        // CURRENT format), but the PIXEL-unit ones — max_radius, Manual-mode
        // size, edge_softness, merge_tolerance — are not: see applyProxyScale().
        const Format* full = deepInfo.fullSizeFormat();
        const float fullWidth = full ? static_cast<float>(full->width()) : formatWidth;
        _proxyScale = (fullWidth > 0.0f && formatWidth > 0.0f)
                    ? (formatWidth / fullWidth) : 1.0f;
        if (!(_proxyScale > 0.0f) || !std::isfinite(_proxyScale))
            _proxyScale = 1.0f;

        _cocParams = deepc::makeCocParams(
            (_cocMode == static_cast<int>(deepc::CocMode::Manual)) ? deepc::CocMode::Manual
                                                                  : deepc::CocMode::Physical,
            std::max(1e-3f, _focalLength),
            std::max(1e-3f, _fstop),
            std::max(1e-3f, _filmbackWidth),
            std::max(1e-6f, _focusDistance),
            deepc::unitScale(clampedWorldUnits()),
            formatWidth,
            (pixelAspect > 0.0f && std::isfinite(pixelAspect)) ? pixelAspect : 1.0f,
            std::max(0.0f, _frontCocMult),
            std::max(0.0f, _backCocMult),
            static_cast<float>(clampedMaxRadius()),
            std::max(0.0f, _size));

        // EXACTLY ONCE, on freshly built params (applyProxyScale mutates and
        // re-derives). From here on _cocParams is in proxy pixels, which is the
        // space every radius, bucket boundary and kernel entry lives in.
        deepc::applyProxyScale(_cocParams, _proxyScale);

        // Mirror DeepToImage: propagate our caching state to the deep source.
        src->op()->cached(cached());
    }

    // ------------------------------------------------------------------
    // _request()
    //
    // Same DANGER as _validate(): never delegate to Iop::_request(), which
    // forwards through Iop::asIop()'s static_cast.
    //
    // The precompute is frame-wide, so the request is frame-wide too: the
    // full padded output box, not the caller's sub-box. Channels come from
    // neededDeepChannels() and nowhere else.
    // ------------------------------------------------------------------
    void _request(int /*x*/, int /*y*/, int /*r*/, int /*t*/,
                  ChannelMask /*channels*/, int count) override
    {
        DeepOp* src = input0();
        if (!src)
            return;

        const DD::Image::Box full = info_.box();

        src->deepRequest(full, neededDeepChannels(), count);

        DeepOp* holdout = input1();
        if (holdout)
            holdout->deepRequest(full, neededHoldoutChannels(), count);
    }

    // ------------------------------------------------------------------
    // engine()
    //
    // row.erase(channels) is the literal first statement: sparse deep pixels
    // otherwise emit whatever was left in the row buffer. Everything after
    // it is allowed to bail out at any point and the row stays black.
    // ------------------------------------------------------------------
    void engine(int y, int x, int r, ChannelMask channels, Row& row) override
    {
        row.erase(channels);

        if (!input0() || aborted())
            return;

        const std::shared_ptr<const FrameCache> cache = ensureComputed();
        if (!cache)
            return;   // aborted or upstream failure — rows stay erased/black

        const DD::Image::Box& box = cache->box;
        if (y < box.y() || y >= box.t())
            return;

        const int xs = std::max(x, box.x());
        const int xe = std::min(r, box.r());
        if (xs >= xe)
            return;

        foreach(z, channels) {
            const int plane = cache->planeIndex(z);
            if (plane < 0)
                continue;   // channel this node does not produce; stays erased

            const float* src = cache->rowPtr(plane, y) - box.x();
            float* dst = row.writable(z);
            for (int i = xs; i < xe; ++i)
                dst[i] = src[i];
        }
    }

private:
    deepc::WorldUnits clampedWorldUnits() const
    {
        if (_worldUnits < static_cast<int>(deepc::WorldUnits::Millimeters) ||
            _worldUnits > static_cast<int>(deepc::WorldUnits::Feet))
            return deepc::WorldUnits::Meters;
        return static_cast<deepc::WorldUnits>(_worldUnits);
    }

    // ------------------------------------------------------------------
    // ensureComputed() — hash-keyed, frame-wide precompute.
    //
    // Keyed on Op::hash() rather than a _computed flag cleared by
    // _validate(): _validate() runs far more often than the inputs actually
    // change, and a flag would throw away a perfectly good frame on every
    // viewer interaction.
    //
    // Concurrency contract for this phase (per-band concurrency is
    // M1.P4.T1): one frame-wide DD::Image::Lock. The first render thread to
    // arrive computes the whole frame while holding it; every other thread
    // blocks and then finds the finished cache. The lock is held across the
    // compute — including the deepEngine() calls into input 0 — which cannot
    // re-enter this node, because input 0 is strictly upstream in a DAG and
    // has no path back to us. (DD::Image::Lock is a plain, non-recursive
    // pthread mutex, so any re-entry would deadlock rather than misbehave
    // quietly — hence the invariant matters.)
    //
    // Readers DO take the lock, but only for the pointer snapshot: every
    // access to _cache/_cachedHash — read or write — happens under the guard,
    // and what leaves it is a shared_ptr copy to an immutable FrameCache. The
    // row copy in engine() then runs with no lock held, so a later recompute
    // can allocate and swap in a new cache without tearing anyone's read.
    // (Snapshotting under the lock is what makes a bare shared_ptr sufficient
    // here; a lock-free reader would need atomic_load/atomic_store.)
    //
    // On abort or upstream failure the freshly-allocated cache is dropped on
    // the floor: _cache/_cachedHash keep whatever they had, nothing is
    // marked valid, and engine() leaves its rows black.
    // ------------------------------------------------------------------
    std::shared_ptr<const FrameCache> ensureComputed()
    {
        Guard guard(_cacheLock);

        // Read the key INSIDE the guard. Op::hash() is a plain member read of
        // the value _validate() left behind, so it is safe to call here — and
        // sampling it outside would let a re-validate between the sample and
        // the lock publish a cache computed from the new info_ under the old
        // key, which then never matches and recomputes the frame forever.
        const DD::Image::Hash h = hash();

        if (_cache && _cachedHash == h)
            return _cache;

        std::shared_ptr<FrameCache> fresh = std::make_shared<FrameCache>();
        if (!computeFrame(*fresh))
            return nullptr;

        _cache      = fresh;
        _cachedHash = h;
        return _cache;
    }

    // ==================================================================
    //
    //  THE COOK (M1.P3.T5) — depth range, then band-by-band scatter
    //
    //  Sequence, all of it under _cacheLock, all of it serial (per-band
    //  concurrency is M1.P4.T1 — correctness first, deliberately):
    //
    //    1. computeDepthRange()  one cheap full-frame DeepFront/DeepBack/
    //                            Alpha pass, alpha-weighted
    //    2. DepthBuckets         bounded-DeltaCoC, from that range
    //       HoldoutBoundaries    uniform in Z over the SAME range, built ONCE
    //                            per frame (a per-band set seams every band
    //                            boundary — measured vis 0.0448 vs 1.0000 for
    //                            one fragment either side of one)
    //       DiscKernelLUT        over the frame's MEASURED radius range, with
    //                            rMin = 0 (milestone Decisions, both halves)
    //    3. for each band: fetch band +/- padY source rows -> SoA flatten ->
    //                      holdout LUT -> scatterBandCPU -> saturate +
    //                      resolveBandCPU -> write the band's disjoint region
    //
    //  Returns false if the cook was aborted or an upstream deepEngine()
    //  failed; in that case the caller must not publish the buffer.
    // ==================================================================

    // Everything one band needs, plus the scratch that is reused across
    // bands (and across cooks would be next — M1.P4.T1's per-thread state).
    struct BandJob {
        DeepOp* src     = nullptr;
        DeepOp* holdout = nullptr;

        const deepc::FlattenParams*     fp                = nullptr;
        const deepc::DepthBuckets*      buckets           = nullptr;
        const deepc::HoldoutBoundaries* holdoutBoundaries = nullptr;
        const deepc::KernelSampler*     kernel            = nullptr;

        DD::Image::Box srcBox;
        DD::Image::Box holdoutBox;

        const std::vector<Channel>* colorChannels = nullptr;
        const std::vector<int>*     colorPlanes   = nullptr;
        int alphaPlane = -1;
        int mattePlane = -1;
        int padY       = 0;

        // bandY / bandHeight are filled per band; everything else (origin,
        // width, sharp threshold, and BOTH bake-off selections) is set once,
        // explicitly, from the knobs.
        deepc::ScatterParams sp;

        deepc::SampleSoA        soa;
        deepc::FlattenScratch   flattenScratch;
        deepc::ScatterScratch   scatterScratch;
        deepc::BucketPlanes     planes;
        deepc::HoldoutSampleSoA holdoutSamples;
        deepc::HoldoutLut       holdoutLut;

        std::vector<deepc::SampleRecord> samples;         // source scratch
        std::vector<deepc::SampleRecord> holdoutRecords;  // holdout scratch

        std::vector<float> bandColor;
        std::vector<float> bandAlpha;
        std::vector<float> bandMatte;
    };

    bool computeFrame(FrameCache& fc)
    {
        DeepOp* src = input0();
        if (!src)
            return false;

        // Component-wise, not Box copy-assign: the NDK's Box has a
        // user-provided copy constructor, so its implicit copy-assignment
        // operator is deprecated and warns under -Wdeprecated-copy.
        fc.box.set(info_.box().x(), info_.box().y(), info_.box().r(), info_.box().t());
        fc.planes.clear();
        foreach(z, _outChannels)
            fc.planes.push_back(z);

        const size_t nPlanes = fc.planes.size();
        if (nPlanes == 0 || fc.box.w() <= 0 || fc.box.h() <= 0)
            return true;   // nothing to produce, but a valid (empty) result

        fc.data.assign(nPlanes * fc.planeStride(), 0.0f);

        // --- plane routing -------------------------------------------------
        // ALPHA IS NOT A SCATTER CHANNEL. The bucket planes carry alpha as a
        // first-class quantity (it is what the transmittance split, the
        // saturation pass and both bucket composites operate on), so the
        // scatter's channel list is the selection MINUS alpha and the
        // composite's own outAlpha is what lands in the alpha plane. Carrying
        // alpha as an ordinary channel as well would composite it through the
        // colour path — a different expression — and the two would disagree.
        std::vector<Channel> colorChannels;
        std::vector<int>     colorPlanes;
        foreach(z, _flattenChannels) {
            if (z == Chan_Alpha)
                continue;
            const int p = fc.planeIndex(z);
            if (p >= 0) {
                colorChannels.push_back(z);
                colorPlanes.push_back(p);
            }
        }
        const int alphaPlane = fc.planeIndex(Chan_Alpha);
        const int mattePlane = (_outputHoldoutMatte && _holdoutMatteChannel != Chan_Black)
                             ? fc.planeIndex(_holdoutMatteChannel)
                             : -1;

        // Only pixels the deep source covers can carry samples; everything
        // else in the padded box stays exactly 0.0 unless a disc reaches it.
        const DD::Image::Box srcBox = src->deepInfo().box();
        if (srcBox.w() <= 0 || srcBox.h() <= 0 || !fc.box.intersects(srcBox))
            return true;

        // --- flatten params, built ONCE ------------------------------------
        // The depth-range pass, the source flatten and the holdout's
        // depthScale all read this same instance, so they cannot disagree
        // about the ray-distance correction (see rayDepthScaleAt()).
        deepc::FlattenParams fp;
        fp.coc                = _cocParams;              // proxy-scaled already
        fp.preMerge           = _preMerge;               // EXPLICIT, never a default
        fp.mergeTolerancePx   = clampedMergeTolerancePx();
        fp.depthIsRayDistance = _depthIsRayDistance;
        fp.formatHeightPx     = _formatHeightPx;
        fp.channelCount       = static_cast<int>(colorChannels.size());
        fp.groups             = deepc::makeSingleChannelGroup(fp.channelCount);

        // --- 1. the depth-range pass ---------------------------------------
        float depthMin  = 0.0f;
        float depthMax  = 0.0f;
        bool  anyAlpha  = false;
        if (!computeDepthRange(src, srcBox, fp, depthMin, depthMax, anyAlpha))
            return false;
        if (!anyAlpha)
            return true;   // no contributing sample anywhere: frame stays black

        // --- 2. buckets, holdout boundary set, kernel LUT ------------------
        const deepc::DepthBuckets buckets =
            deepc::makeBoundedDeltaCocBuckets(fp.coc, depthMin, depthMax,
                                              clampedDepthLayers());

        // FRAME-GLOBAL, never per band: a fragment near a band edge scatters
        // into two bands, and per-band sets put a seam along every boundary.
        const deepc::HoldoutBoundaries holdoutBoundaries =
            deepc::makeUniformHoldoutBoundaries(buckets);

        // The frame's MEASURED radius range. radiusPixels() is monotone away
        // from the focal plane on each side, so the frame's largest radius is
        // at one of the two ends of the measured depth range — no second pass
        // is needed to find it. It is already clamped by max_radius (in proxy
        // pixels) inside radiusPixels().
        float rMax = std::max(deepc::radiusPixels(fp.coc, buckets.depthMin()),
                              deepc::radiusPixels(fp.coc, buckets.depthMax()));
        if (!(rMax > 0.0f) || !std::isfinite(rMax))
            rMax = 0.0f;
        // The LUT sanitises anything above its own cap to that cap, so clamp
        // here too and keep the band geometry consistent with what the
        // sampler will actually hand back.
        rMax = std::min(rMax, deepc::DiscKernelLUT::kMaxSupportedRadius);

        // edge_softness is proxy-scaled HERE. applyProxyScale() deliberately
        // does not touch it — CocParams does not carry it — so the LUT build
        // is where it lands (milestone brief, M1.P3.T5).
        const float softness    = clampedEdgeSoftness() * _proxyScale;
        const float pixelAspect = fp.coc._pixelAspect;

        // rMin = 0, NOT the measured minimum (milestone Decisions,
        // 2026-07-26): a query below rMin is clamped UP to the rMin kernel, so
        // a measured rMin would visibly over-blur every radius between the
        // sharp-path threshold and it. The range parameter exists to bound
        // rMax, which is where the ~1.0GB worst case lives.
        const deepc::DiscKernelLUT kernel(0.0f, rMax, softness, pixelAspect);

        // --- 3. band decomposition -----------------------------------------
        const int W = fc.box.w();
        const int C = static_cast<int>(colorChannels.size());
        const int K = buckets.bucketCount();

        DeepOp* holdout = input1();
        DD::Image::Box holdoutBox;
        if (holdout) {
            const DD::Image::Box& hb = holdout->deepInfo().box();
            holdoutBox.set(hb.x(), hb.y(), hb.r(), hb.t());
        }
        const bool holdoutConnected = (holdout != nullptr)
                                   && holdoutBox.w() > 0 && holdoutBox.h() > 0;

        // The flatten needs to know too (M1.P3.T13): with a holdout connected
        // its same-pixel deposit-collision merge may not carry a fragment
        // across a HoldoutBoundaries bracket, because the merged fragment is
        // sampled against the transmittance LUT at ONE depth. Measured on two
        // sharp samples at one pixel with an opaque card between them: 0.750
        // (both survive — wrong) unrestricted, 0.500 (front survives, back
        // erased — exact) with this set. Assigned here rather than up with the
        // other fields because that is where input 1's presence is decided,
        // and nothing between the two reads it.
        fp.holdoutConnected = holdoutConnected;

        // A band's dest rows need source samples from band +/- the kernel's
        // true vertical extent: (rMax + softness/2) * pixelAspect, the same
        // quantity the output bbox is padded by.
        const int padY = static_cast<int>(
            std::ceil((rMax + 0.5f * softness)
                      * ((pixelAspect > 0.0f && std::isfinite(pixelAspect)) ? pixelAspect : 1.0f)));

        // B = clamp(2*maxRadius, 32, 256), then shrunk (never below 1 row)
        // until the band's own scratch fits the memory limit. Every term is
        // evaluated on CLAMPED values, never on raw knob values.
        int bandHeight = deepc::clampi(static_cast<int>(std::ceil(2.0f * rMax)), 32, 256);
        bandHeight = std::min(bandHeight, fc.box.h());
        while (bandHeight > 1
               && bandScratchBytes(K, C, W, bandHeight, holdoutConnected) > memoryLimitBytes()) {
            bandHeight = std::max(1, bandHeight / 2);
        }

        // --- 4. the band loop ----------------------------------------------
        BandJob job;
        job.src               = src;
        job.holdout           = holdoutConnected ? holdout : nullptr;
        job.fp                = &fp;
        job.buckets           = &buckets;
        job.holdoutBoundaries = &holdoutBoundaries;
        job.kernel            = &kernel;
        job.srcBox.set(srcBox.x(), srcBox.y(), srcBox.r(), srcBox.t());
        job.holdoutBox.set(holdoutBox.x(), holdoutBox.y(), holdoutBox.r(), holdoutBox.t());
        job.colorChannels     = &colorChannels;
        job.colorPlanes       = &colorPlanes;
        job.alphaPlane        = alphaPlane;
        job.mattePlane        = mattePlane;
        job.padY              = padY;

        job.sp.bandX         = fc.box.x();
        job.sp.bandWidth     = W;
        job.sp.sharpRadiusPx = deepc::kSharpRadiusPx;

        for (int y0 = fc.box.y(); y0 < fc.box.t(); y0 += bandHeight) {
            if (aborted())
                return false;
            const int y1 = std::min(y0 + bandHeight, fc.box.t());
            if (!computeBand(fc, job, y0, y1))
                return false;
        }

        return true;
    }

    // ------------------------------------------------------------------
    // bandScratchBytes() — the memory-limit formula, in one place
    //
    // The design reference's K*W*B*(C+3)*4 bucket planes, plus the holdout
    // LUT's (K+1)*W*B*4 when a holdout is connected. It does NOT include the
    // SoA fragment buffers, which dominate both (~100 B/fragment resident;
    // milestone Decisions) but cannot be sized before the band is flattened —
    // M1.P4.T1 budgets on the combined figure.
    // ------------------------------------------------------------------
    static double bandScratchBytes(int k, int c, int w, int h, bool holdout)
    {
        double bytes = static_cast<double>(
            deepc::BucketPlanes::bytesForBand(k, c, w, h));
        if (holdout) {
            bytes += static_cast<double>(k + 1) * static_cast<double>(w)
                   * static_cast<double>(h) * 4.0;
        }
        return bytes;
    }

    // ------------------------------------------------------------------
    // computeDepthRange() — the frame's alpha-weighted depth range
    //
    // A separate, cheap, full-frame DeepFront/DeepBack/Alpha pass. It is what
    // the ΔCoC bucket boundaries, the holdout boundary set and the kernel
    // LUT's extent are all derived from, so getting it wrong is not a
    // resolution question, it is a correctness one:
    //
    //   * IT APPLIES THE SAME RAY-DISTANCE -> Z CORRECTION THE FLATTEN
    //     APPLIES (rayDepthScaleAt(), off the same FlattenParams). The
    //     correction always SHRINKS depth, so a range measured without it
    //     puts every corner-pixel sample below depthMin and piles the
    //     out-of-range spans into the edge bucket.
    //   * it sanitises depths with the flatten's own rule
    //     (sanitizeFragmentDepth), for the same reason.
    //
    // ALPHA WEIGHTING. Samples with alpha <= 0 are skipped outright — the
    // flatten drops them too (DeepToImage parity), so they are not content.
    // Beyond that, the endpoints are accumulated into a log-spaced histogram
    // weighted by alpha and the outermost bins carrying less than
    // kDepthTailFraction of the frame's total alpha mass are clipped, so one
    // stray alpha-1e-7 sample at the far clip cannot spend the whole bucket
    // budget on empty depth. Clipping is SAFE, not merely cheap: bucketOf()
    // and locateBoundary() clamp an out-of-range depth onto the nearest
    // bucket, which is monotone, so front-to-back ORDER is preserved and only
    // the depth RESOLUTION of the clipped tail is lost. The fragment's own
    // radius is still computed from its own depth, so its blur is unchanged.
    //
    // Returns false only on abort / upstream failure. `anyAlpha` false means
    // the frame carries no contributing sample at all.
    // ------------------------------------------------------------------
    bool computeDepthRange(DeepOp* src,
                          const DD::Image::Box& srcBox,
                          const deepc::FlattenParams& fp,
                          float& depthMin,
                          float& depthMax,
                          bool&  anyAlpha)
    {
        depthMin = 0.0f;
        depthMax = 0.0f;
        anyAlpha = false;

        const int    kBins   = 2048;
        const double kTail   = 1e-4;   // of the frame's total alpha mass
        const double logLo   = std::log(static_cast<double>(deepc::DepthBuckets::kMinDepth));
        const double logHi   = std::log(static_cast<double>(deepc::DepthBuckets::kMaxDepth));
        const double binPerL = static_cast<double>(kBins) / (logHi - logLo);

        std::vector<double> hist(static_cast<size_t>(kBins), 0.0);
        double total = 0.0;
        float  exactLo = std::numeric_limits<float>::infinity();
        float  exactHi = -std::numeric_limits<float>::infinity();

        // DeepFront/DeepBack/Alpha only — the same three channels _request()
        // asked the holdout for, through the same helper.
        const ChannelSet need = neededHoldoutChannels();

        for (int y = srcBox.y(); y < srcBox.t(); ++y) {
            if (aborted())
                return false;

            DeepPlane deepRow;
            if (!src->deepEngine(y, srcBox.x(), srcBox.r(), need, deepRow)) {
                Iop::abort();
                return false;
            }

            for (int x = srcBox.x(); x < srcBox.r(); ++x) {
                const DeepPixel pixel = deepRow.getPixel(y, x);
                const size_t n = pixel.getSampleCount();
                if (n == 0)
                    continue;

                const ChannelMap& have = pixel.channels();
                if (!have.contains(Chan_DeepFront) || !have.contains(Chan_Alpha))
                    continue;
                const bool haveBack = have.contains(Chan_DeepBack);

                const float rayScale = deepc::rayDepthScaleAt(fp, x, y);

                for (size_t s = 0; s < n; ++s) {
                    const float a = deepc::clampf(
                        pixel.getUnorderedSample(s, Chan_Alpha), 0.0f, 1.0f);
                    if (!(a > 0.0f))
                        continue;

                    const float zfRaw = pixel.getUnorderedSample(s, Chan_DeepFront);
                    const float zbRaw = haveBack
                                      ? pixel.getUnorderedSample(s, Chan_DeepBack)
                                      : zfRaw;

                    float z[2];
                    z[0] = deepc::DepthBuckets::sanitizeDepth(
                        deepc::sanitizeFragmentDepth(zfRaw) * rayScale);
                    z[1] = deepc::DepthBuckets::sanitizeDepth(
                        deepc::sanitizeFragmentDepth(zbRaw) * rayScale);

                    for (int e = 0; e < 2; ++e) {
                        if (z[e] < exactLo) exactLo = z[e];
                        if (z[e] > exactHi) exactHi = z[e];

                        int bin = static_cast<int>(
                            (std::log(static_cast<double>(z[e])) - logLo) * binPerL);
                        bin = deepc::clampi(bin, 0, kBins - 1);
                        hist[static_cast<size_t>(bin)] += static_cast<double>(a);
                        total += static_cast<double>(a);
                    }
                }
                anyAlpha = true;
            }
        }

        if (!anyAlpha || !(total > 0.0) || !(exactLo <= exactHi)) {
            anyAlpha = false;
            return true;
        }

        const double tail = total * kTail;

        int b0 = 0;
        for (double c = 0.0; b0 < kBins; ++b0) {
            c += hist[static_cast<size_t>(b0)];
            if (c > tail)
                break;
        }
        int b1 = kBins - 1;
        for (double c = 0.0; b1 > b0; --b1) {
            c += hist[static_cast<size_t>(b1)];
            if (c > tail)
                break;
        }
        b0 = deepc::clampi(b0, 0, kBins - 1);
        b1 = deepc::clampi(b1, b0, kBins - 1);

        const float binLo = static_cast<float>(std::exp(logLo + b0 / binPerL));
        const float binHi = static_cast<float>(std::exp(logLo + (b1 + 1) / binPerL));

        depthMin = std::max(exactLo, binLo);
        depthMax = std::min(exactHi, binHi);
        if (!(depthMin <= depthMax) || !std::isfinite(depthMin) || !std::isfinite(depthMax)) {
            depthMin = exactLo;    // clipping went degenerate: use the raw range
            depthMax = exactHi;
        }
        return true;
    }

    // ------------------------------------------------------------------
    // fillSampleRecords() — one DeepPixel -> the SampleRecord vector the
    // POD flatten consumes. Returns false when the pixel has nothing usable
    // (the same validity test the NDK's own DeepToImage applies).
    //
    // resize() only, never clear() + resize(): the vector is reused across
    // every pixel of the band, and clear() would free every SampleRecord's
    // channel vector, costing a malloc/free per sample per pixel.
    // ------------------------------------------------------------------
    static bool fillSampleRecords(const DeepPixel& pixel,
                                  const std::vector<Channel>& chans,
                                  std::vector<deepc::SampleRecord>& out)
    {
        const size_t n = pixel.getSampleCount();
        if (n == 0)
            return false;

        const ChannelMap& have = pixel.channels();
        if (!have.contains(Chan_DeepFront) || !have.contains(Chan_Alpha))
            return false;

        const bool   haveBack = have.contains(Chan_DeepBack);
        const size_t nChan    = chans.size();

        out.resize(n);
        for (size_t s = 0; s < n; ++s) {
            deepc::SampleRecord& rec = out[s];

            const float zf = pixel.getUnorderedSample(s, Chan_DeepFront);
            const float zb = haveBack ? pixel.getUnorderedSample(s, Chan_DeepBack) : zf;

            // Left raw: flattenPixelToSoA() sanitises, ray-corrects and orders
            // the endpoints itself, and it must be the one doing it.
            rec.zFront = zf;
            rec.zBack  = zb;
            rec.alpha  = pixel.getUnorderedSample(s, Chan_Alpha);

            rec.channels.resize(nChan);
            for (size_t c = 0; c < nChan; ++c) {
                rec.channels[c] = have.contains(chans[c])
                                ? pixel.getUnorderedSample(s, chans[c])
                                : 0.0f;
            }
        }
        return true;
    }

    // ------------------------------------------------------------------
    // computeBand() — one horizontal band, end to end
    //
    //   fetch band +/- padY source rows -> flattenPixelToSoA
    //   (only if it can matter) fetch the band's own rows of holdout
    //     -> HoldoutSampleSoA -> HoldoutLut at the FRAME-GLOBAL boundary set
    //   scatterBandCPU -> resolveBandCPU (saturate + composite)
    //   write the band's disjoint region of the frame
    //
    // Returns false on abort / upstream failure, leaving the frame unpublished.
    // ------------------------------------------------------------------
    bool computeBand(FrameCache& fc, BandJob& job, int y0, int y1)
    {
        const int h = y1 - y0;
        const int W = fc.box.w();
        if (h <= 0 || W <= 0)
            return true;

        const int C = static_cast<int>(job.colorChannels->size());
        const int K = job.buckets->bucketCount();
        const std::ptrdiff_t px = static_cast<std::ptrdiff_t>(W) * h;

        // --- source fetch: band +/- padY, clipped to the source bbox -------
        job.soa.begin(C, job.fp->groups);

        const int fy0 = std::max(job.srcBox.y(), y0 - job.padY);
        const int fy1 = std::min(job.srcBox.t(), y1 + job.padY);

        const ChannelSet need = neededDeepChannels();

        for (int y = fy0; y < fy1; ++y) {
            if (aborted())
                return false;

            DeepPlane deepRow;
            if (!job.src->deepEngine(y, job.srcBox.x(), job.srcBox.r(), need, deepRow)) {
                Iop::abort();
                return false;
            }

            for (int x = job.srcBox.x(); x < job.srcBox.r(); ++x) {
                if (!fillSampleRecords(deepRow.getPixel(y, x), *job.colorChannels,
                                       job.samples))
                    continue;

                deepc::flattenPixelToSoA(*job.fp, *job.buckets, x, y,
                                         job.samples, job.flattenScratch,
                                         job.soa, nullptr);
            }
        }

        // --- holdout -------------------------------------------------------
        // begin() with NO appendPixel() calls at all is well defined and lands
        // on exactly the same disabled view, so the per-pixel loop is skipped
        // ENTIRELY when there is no holdout or the band misses its bbox —
        // discovering emptiness by running it costs ~1.98 ms/band, ~67 ms per
        // 4K frame of pure bookkeeping (M1.P3.T3's review).
        job.holdoutSamples.begin(px);
        if (job.mattePlane >= 0)
            job.bandMatte.assign(static_cast<size_t>(px), 0.0f);

        DD::Image::Box bandBox(fc.box.x(), y0, fc.box.r(), y1);
        const bool holdoutActive = (job.holdout != nullptr)
                                && bandBox.intersects(job.holdoutBox);

        if (holdoutActive) {
            const ChannelSet hNeed = neededHoldoutChannels();

            const int hx0 = std::max(fc.box.x(), job.holdoutBox.x());
            const int hx1 = std::min(fc.box.r(), job.holdoutBox.r());

            for (int y = y0; y < y1; ++y) {
                if (aborted())
                    return false;

                const bool rowInside = (y >= job.holdoutBox.y() && y < job.holdoutBox.t())
                                    && (hx1 > hx0);

                DeepPlane deepRow;
                if (rowInside) {
                    if (!job.holdout->deepEngine(y, hx0, hx1, hNeed, deepRow)) {
                        Iop::abort();
                        return false;
                    }
                }

                for (int x = fc.box.x(); x < fc.box.r(); ++x) {
                    job.holdoutRecords.clear();

                    if (rowInside && x >= hx0 && x < hx1) {
                        const DeepPixel pixel = deepRow.getPixel(y, x);
                        const size_t n = pixel.getSampleCount();
                        const ChannelMap& have = pixel.channels();
                        if (n > 0 && have.contains(Chan_DeepFront)
                                  && have.contains(Chan_Alpha)) {
                            const bool haveBack = have.contains(Chan_DeepBack);
                            job.holdoutRecords.resize(n);
                            for (size_t s = 0; s < n; ++s) {
                                deepc::SampleRecord& rec = job.holdoutRecords[s];
                                const float zf = pixel.getUnorderedSample(s, Chan_DeepFront);
                                rec.zFront = zf;
                                rec.zBack  = haveBack
                                           ? pixel.getUnorderedSample(s, Chan_DeepBack)
                                           : zf;
                                rec.alpha  = pixel.getUnorderedSample(s, Chan_Alpha);
                                rec.channels.clear();   // holdout carries no colour
                            }
                        }
                    }

                    // THE SAME per-pixel ray-distance factor the flatten
                    // applied at this pixel. Without it the holdout sits
                    // systematically too far back in Z off-axis (measured:
                    // 47.3% at the corner of a 20mm / 36x24 frame).
                    const float depthScale = deepc::rayDepthScaleAt(*job.fp, x, y);
                    job.holdoutSamples.appendPixel(job.holdoutRecords, depthScale);

                    // The matte AOV, from the SAME sanitised sample set
                    // appendPixel() just left behind (it drops alpha<=0 and
                    // NaN depths in place). It is 1 - vis(infinity), and it is
                    // NOT computed as 1 - boundaryT: that subtraction's
                    // relative error is 100% at alpha 1e-7 and 19.2% over 200
                    // compounded samples of it (milestone Decisions). The
                    // log1p/expm1 form is exact in the same limit —
                    // 1 - prod(1-a) == -expm1(sum log1p(-a)).
                    if (job.mattePlane >= 0) {
                        double logT = 0.0;
                        for (size_t s = 0; s < job.holdoutRecords.size(); ++s) {
                            const double a = static_cast<double>(
                                deepc::clampf(job.holdoutRecords[s].alpha, 0.0f, 1.0f));
                            logT += std::log1p(-a);   // a == 1 -> -inf -> matte 1
                        }
                        const std::ptrdiff_t i =
                            static_cast<std::ptrdiff_t>(y - y0) * W + (x - fc.box.x());
                        // max(0, ...) rather than a bare negation: -expm1(0)
                        // is -0.0f, and writing a negative zero into a matte
                        // channel is a wart every downstream comparison then
                        // has to know about.
                        job.bandMatte[static_cast<size_t>(i)] =
                            std::max(0.0f, static_cast<float>(-std::expm1(logT)));
                    }
                }
            }
        }

        // Empty when nothing was appended: view() is then disabled and the
        // scatter's per-fragment path never touches the holdout at all.
        job.holdoutLut.build(job.holdoutSamples, *job.holdoutBoundaries);
        const deepc::HoldoutSoA holdoutView = job.holdoutLut.view();

        if (aborted())
            return false;

        // --- scatter + resolve ---------------------------------------------
        job.planes.allocate(K, C, W, h);   // sizes AND zeroes; keeps capacity

        job.bandColor.assign(static_cast<size_t>(C) * static_cast<size_t>(px), 0.0f);
        job.bandAlpha.assign(static_cast<size_t>(px), 0.0f);

        job.sp.bandY      = y0;
        job.sp.bandHeight = h;

        deepc::scatterBandCPU(job.sp, job.soa, holdoutView, *job.kernel,
                              job.planes, job.scatterScratch);
        deepc::resolveBandCPU(job.sp, job.planes,
                              job.bandColor.data(), job.bandAlpha.data());

        if (aborted())
            return false;

        // --- write the band's disjoint region ------------------------------
        for (int c = 0; c < C; ++c) {
            const int plane = (*job.colorPlanes)[static_cast<size_t>(c)];
            const float* srcPlane = job.bandColor.data()
                                  + static_cast<size_t>(c) * static_cast<size_t>(px);
            for (int y = 0; y < h; ++y) {
                std::memcpy(fc.rowPtr(plane, y0 + y),
                            srcPlane + static_cast<size_t>(y) * W,
                            static_cast<size_t>(W) * sizeof(float));
            }
        }
        if (job.alphaPlane >= 0) {
            for (int y = 0; y < h; ++y) {
                std::memcpy(fc.rowPtr(job.alphaPlane, y0 + y),
                            job.bandAlpha.data() + static_cast<size_t>(y) * W,
                            static_cast<size_t>(W) * sizeof(float));
            }
        }
        // Written last on purpose: if the user points the AOV at a channel the
        // node also processes, the AOV is what they asked for.
        if (job.mattePlane >= 0) {
            for (int y = 0; y < h; ++y) {
                std::memcpy(fc.rowPtr(job.mattePlane, y0 + y),
                            job.bandMatte.data() + static_cast<size_t>(y) * W,
                            static_cast<size_t>(W) * sizeof(float));
            }
        }

        return true;
    }

    // ------------------------------------------------------------------
    // flattenPixel() — one deep pixel to one flat pixel.
    //
    // *** NO LONGER ON THE COOK PATH (M1.P3.T5) ***  computeFrame() now
    // scatters; this is M1.P2.T2's plain flatten, kept deliberately for two
    // reasons the milestone names explicitly:
    //   * it is the REFERENCE the DeepToImage parity gate was established
    //     against, and the parity numbers below are the record of it;
    //   * it carries one of the TWO independent layers of the FMA /
    //     fp-contract parity guard (milestone Decisions, 2026-07-26: "the
    //     CMake-level omission is the belt; the pragma is the braces"), and
    //     M1.P5.T1 adds -mavx2 -mfma to this target.
    // M1.P3.T12 / M1.P5.T1 should decide whether it is retired — deleting it
    // here would silently drop a documented guard mid-phase.  NOTE for
    // M1.P5.T1: the arithmetic that now produces the shipped pixels lives in
    // DeepCDefocusScatter.{h,cpp} (the pre-merge `over` and both bucket
    // composites), and that TU has NO such pragma.
    //
    // Tidy pre-pass first (deepc::tidyOverlapping(), reused rather than
    // reimplemented), then a plain front-to-back over. The tidy pass is
    // correctness-required and always on: without it, coincident same-pixel
    // samples get *added* instead of over-composited and parity with a
    // DeepToImage flatten is unachievable.
    //
    // Alpha is carried twice on purpose — once as SampleRecord::alpha, which
    // is what the tidy pass's transmittance splits need, and once as an
    // ordinary entry in the channel vector, so that the composite treats it
    // exactly like every other channel. The two agree by construction:
    // over-compositing alpha as a channel *is* the accumulated alpha.
    //
    // MEASURED PARITY vs stock DeepToImage (Nuke 17.0v3, headless, all four
    // rgba channels over every pixel, at -O3 -mavx2 -mfma):
    //
    //   POINT SAMPLES — bit-exact, 0 ULP, in every case tried:
    //     - 4 depth-separated layers, per-pixel varying alpha ..... 0 ULP
    //     - 20 samples per pixel, all distinct depths ............. 0 ULP
    //     - alpha exactly 1 in front of others, alpha-0 samples ... 0 ULP
    //     - extreme alphas (1e-7, 1e-4, 0.5, 0.999999, 1) ......... 0 ULP
    //     - sparse input, zero-sample regions ..................... 0 ULP
    //     - 2048x1152 frame cooked by 16 render threads ........... 0 ULP
    //     - downstream Crop, channel subset, pixel aspect 2,
    //       proxy 0.5, holdout connected, matte AOV on ............ 0 ULP
    //
    //   COINCIDENT-DEPTH SAMPLES — small, and NOT bounded at 2 ULP. The tidy
    //   pass collapses samples sharing an exact [zFront, zBack] into a single
    //   over-composited sample *before* the flatten, whereas DeepToImage
    //   composites them one at a time; `over` is associative in exact
    //   arithmetic but not in float, so the two round differently. The error
    //   grows with the number of coincident samples in a pixel (measured, on
    //   the worst pixel of a 64x48 frame):
    //       2 coincident -> 1 ULP        8  coincident -> 4 ULP
    //       3 coincident -> 2 ULP        12 coincident -> 5 ULP
    //       5 coincident -> 3 ULP        20+ coincident -> 6 ULP (1.79e-07)
    //   The tidy pass is correctness-required for the scatter phases
    //   (coincident samples must not be *added*), so the re-association is
    //   deliberate — but any "<= N ULP" acceptance threshold has to be stated
    //   against a named scene, not as a universal bound.
    //
    //   VOLUMETRIC SPANS THAT OVERLAP OR COINCIDE — parity to float
    //   precision, and NOT the 1e-02 mismatch this comment used to report.
    //   M1.P3.T0 adjudicated the old disagreement and found the tidy pass, not
    //   Nuke, was wrong: it merged spans sharing an interval with `over`, but
    //   two samples on one interval are co-located media, so their optical
    //   depths and emission ADD rather than one occluding the other.
    //   deepc::tidyOverlapping() now merges them by the OpenEXR "Interpreting
    //   Deep Pixels" volume-mixture rule, which is what Nuke's own
    //   CombineOverlappingSamples computes. Re-measured (Nuke 17.0v3,
    //   headless, all four rgba channels over every pixel of a 64x48 frame,
    //   -O3 -mavx2 -mfma), against DeepToImage with volumetric_composition ON
    //   — its default:
    //     - two partially overlapping spans ................... 1.8e-07
    //     - two perfectly coincident spans .................... 6.0e-08
    //     - three-way overlap ................................. 2.4e-07
    //   i.e. the same ~1-2 ULP re-association noise as the coincident
    //   point-sample case above, so a DeepToImage-parity gate no longer has
    //   to be scoped to point samples.
    //
    //   Against DeepToImage with volumetric_composition OFF the same scenes
    //   now differ by 1.5e-02 to 4.7e-02, and that is deliberate: that
    //   setting selects Nuke's plain `over` of overlapping spans, which is
    //   the behaviour the tidy pass was changed away from. Do not use it as
    //   the parity reference for volumetric input.
    // ------------------------------------------------------------------
#if defined(__GNUC__) && !defined(__clang__)
    // See the composite loop below: fusing its multiply and add into an FMA
    // changes the rounding and costs bit-exact parity with DeepToImage
    // (measured: 1.2e-07 divergence under -O3 -mavx2 -mfma). M1.P4.T2/P5.T1
    // add -mavx2 -mfma to this target for the scatter loop's sake, so the
    // guard lives here rather than in the build files, where it could be
    // dropped without anything failing loudly.
    #pragma GCC push_options
    #pragma GCC optimize ("fp-contract=off")
#endif
    void flattenPixel(const DeepPixel& deepPixel,
                      const std::vector<Channel>& flatChannels,
                      std::vector<deepc::SampleRecord>& samples,
                      std::vector<float>& accum) const
    {
        const size_t nChan = flatChannels.size();
        std::fill(accum.begin(), accum.end(), 0.0f);

        const size_t nSamples = deepPixel.getSampleCount();
        if (nSamples == 0)
            return;

        // Same validity test the NDK's DeepToImage example applies before it
        // composites: without a depth front and an alpha there is nothing
        // meaningful to flatten, and the pixel stays black.
        const ChannelMap& have = deepPixel.channels();
        if (!have.contains(Chan_DeepFront) || !have.contains(Chan_Alpha))
            return;

        const bool haveBack = have.contains(Chan_DeepBack);

        // resize() only, deliberately NOT clear() + resize(): `samples` is
        // reused across every pixel of the frame, and clear() destroys each
        // SampleRecord's channel vector, so every sample of every pixel would
        // pay a free + malloc. Every field of every element is overwritten
        // below, so the surviving elements carry no stale state. (Measured on
        // a 2048x1152 / 6-samples-per-pixel frame: 3.07s -> 2.54s.)
        samples.resize(nSamples);

        for (size_t s = 0; s < nSamples; ++s) {
            deepc::SampleRecord& rec = samples[s];

            const float zf = deepPixel.getUnorderedSample(s, Chan_DeepFront);
            const float zb = haveBack ? deepPixel.getUnorderedSample(s, Chan_DeepBack) : zf;

            rec.zFront = zf;
            rec.zBack  = std::max(zf, zb);   // tidyOverlapping assumes zBack >= zFront
            rec.alpha  = deepPixel.getUnorderedSample(s, Chan_Alpha);

            rec.channels.resize(nChan);
            for (size_t c = 0; c < nChan; ++c) {
                const Channel z = flatChannels[c];
                rec.channels[c] = have.contains(z) ? deepPixel.getUnorderedSample(s, z) : 0.0f;
            }
        }

        // Splits partially-overlapping spans and over-merges coincident
        // ones, leaving the list sorted front-to-back.
        deepc::tidyOverlapping(samples);

        // tidyOverlapping() only sorts when it has 2+ samples to consider;
        // sort unconditionally so a single-sample pixel takes the same path.
        std::sort(samples.begin(), samples.end(),
            [](const deepc::SampleRecord& a, const deepc::SampleRecord& b) {
                return (a.zFront != b.zFront) ? a.zFront < b.zFront
                                              : a.zBack  < b.zBack;
            });

        // Back-to-front accumulation, farthest sample first:
        //
        //     acc[c] = acc[c] * (1 - alpha) + value[c]
        //
        // This is the "under" form of the same composite as a front-to-back
        // `acc += value * transmittance`, and identical to it in exact
        // arithmetic — but NOT in float, and this node's gate is bit-exact
        // parity with stock DeepToImage. DD::Image::CompositeSamples (the
        // function DeepToImage flattens with) walks DeepPixel::
        // getOrderedSample() from the back and evaluates exactly the
        // expression above, as a separate multiply and add. Front-to-back
        // accumulation reproduces it only to ~1 ULP (measured: 1.2e-07 at
        // pixel values near 1.0), so the flatten mirrors Nuke's association
        // and rounding order rather than choosing its own.
        //
        // Zero-alpha samples are skipped outright, which is also what
        // CompositeSamples does — a premultiplied sample with alpha 0 and
        // non-zero colour contributes nothing there, and adding its colour
        // here would be a visible, not just an ULP-level, divergence.
        //
        // NOTE for M1.P5.T1: this loop must NOT be compiled with FMA
        // contraction enabled (`-ffp-contract=fast` plus `-mfma`), which
        // would fuse the multiply and add and cost the bit-exactness.
        for (size_t i = samples.size(); i-- > 0; ) {
            const deepc::SampleRecord& rec = samples[i];
            if (rec.alpha == 0.0f)
                continue;
            const float oneMinusAlpha = 1.0f - rec.alpha;
            for (size_t c = 0; c < nChan; ++c) {
                const float scaled = accum[c] * oneMinusAlpha;
                accum[c] = scaled + rec.channels[c];
            }
        }
    }
#if defined(__GNUC__) && !defined(__clang__)
    #pragma GCC pop_options
#endif

public:
    static const Op::Description d;
};

// ---------------------------------------------------------------------------
static Op* build(Node* node) { return new DeepCDefocus(node); }
const Op::Description DeepCDefocus::d(::CLASS, "Deep/DeepCDefocus", build);
