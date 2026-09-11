// SPDX-License-Identifier: MIT
//
// ============================================================================
//
//  DeepCDefocus — Deep-input, flat-output defocus/DOF node
//
//  The frame is computed lazily, one horizontal band at a time, by whichever
//  of Nuke's render threads asks for a row in that band first, coordinated by
//  a per-band Dirty -> InProgress -> Done state machine (deepc::BandLedger,
//  POD and unit-tested with std::thread; instantiated here over
//  DD::Image::SignalLock).  Per band:
//
//    frameSetup()  ONCE per Op::hash(), claimed like a band ("band -1"):
//         computeDepthRange()  alpha-weighted DeepFront/DeepBack/Alpha pass
//           -> DepthBuckets (bounded ΔCoC) + HoldoutBoundaries (uniform in Z,
//              frame-global) + DiscKernelLUT over the MEASURED radius range
//           -> band decomposition + the memory-limit cap (deepc::planBands)
//    computeBand() per claimed band, into the claiming thread's PRIVATE
//         bucket planes (a pooled BandJob):
//         fetch band +/- padY source rows -> flattenPixelToSoA
//      -> holdout fetch (skipped entirely when it cannot matter) -> HoldoutLut
//      -> scatterBandCPU -> resolveBandCPU (saturate down, then composite)
//      -> write the band's DISJOINT region of the shared flat frame
//      -> BandLedger::completeBand() publishes it (release/acquire) to the
//         lock-free row-copy path
//
//  A thread whose band another thread is already computing claims a
//  different Dirty band instead (BandLedger::tryClaimDirtyBand) and only
//  blocks on the ledger when there is none: Nuke hands its render threads
//  consecutive rows, so all of them land in one band and three of four
//  otherwise wait out its compute.  An aborted band goes back to Dirty
//  (never Done), wakes its waiters, and leaves erased/black rows.  _validate
//  marks all bands Dirty on an Op::hash() change.
//
//  Two standing invariants:
//  _validate()/_request() must NEVER fall through to Iop's (they reach
//  inputs through a bare static_cast<Iop*> and this node's inputs are
//  DeepOps — a verified Nuke core dump), and row.erase(channels) is
//  engine()'s first statement.
//
//  Node shape:
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
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include <pthread.h>   // pthread_self(), for the band-claim debug counter

using namespace DD::Image;

// ---------------------------------------------------------------------------
// Hard caps applied at every use site.
//
// The knob IRanges are deliberately *soft* (IRange::force defaults to false),
// so a user can type max_radius = 5000 into the panel. Every consumer of a
// knob value therefore clamps it here rather than trusting the slider bound —
// the memory formulas (K*W*B*(C+3)*4 per band, LUT ~2*pi*R^3/3) are only
// bounded if the use sites do the clamping. These caps are shared node-wide
// so the bbox pad, the LUT extent and the scatter loop can never disagree
// about how big "big" is.
// ---------------------------------------------------------------------------
static const int   kMaxRadiusCap    = 2000;   // px
static const float kEdgeSoftnessCap = 64.0f;  // px

static const char* const CLASS = "DeepCDefocus";
static const char* const HELP =
    "Depth of field / defocus for deep images. Input 0 is a deep image, the "
    "output is flat 2D.\n"
    "\n"
    "Every deep sample is scattered into a disc whose radius comes from the "
    "sample's own depth, and the discs are composited back to front in depth "
    "buckets. Depth comes from the deep front/back channels, so no separate "
    "depth channel or Z-defocus pass is involved. The bokeh in this version "
    "is a plain anti-aliased circle.\n"
    "\n"
    "HOLDOUT (input 1)\n"
    "\n"
    "Input 1 is an optional deep holdout. There is no enable knob -- "
    "connecting something switches it on, disconnecting switches it off. "
    "Only the holdout's deep front, deep back and alpha are read.\n"
    "\n"
    "Every source sample is multiplied by the holdout's transmittance at that "
    "sample's depth, at each destination pixel it scatters into, and that "
    "happens at scatter time, before anything is accumulated. The holdout is "
    "therefore never defocused: it stays pixel-sharp at every depth, however "
    "large the beauty's circle of confusion is. A defocused foreground blooms "
    "over a held-out element while the element's own edge stays hard, and "
    "geometry behind the holdout attenuates to zero.\n"
    "\n"
    "A volumetric holdout sample attenuates exponentially across its own "
    "span, so a fog slab holds out progressively rather than as a hard card. "
    "With input 1 disconnected, or outside the holdout's bounding box, "
    "visibility is 1 and costs nothing.\n"
    "\n"
    "COVERAGE FILL\n"
    "\n"
    "A defocused foreground scatters outward off its own silhouette. The "
    "pixels it vacates need whatever was behind it, and a deep image only "
    "contains that if the renderer wrote the samples hidden behind the "
    "foreground. Most renderers stop a ray at the first opaque hit and write "
    "nothing behind it. Without a fill that leaves an alpha dip roughly one "
    "circle of confusion wide just inside the silhouette, and the same "
    "shortfall appears as bands of reduced opacity either side of the focal "
    "line on a surface receding through focus, where neighbouring scanlines "
    "scatter different-sized discs.\n"
    "\n"
    "This node fills both, the way a 2D defocus of a flat image does "
    "implicitly. Every source pixel's unit area is partitioned over its "
    "samples front to back (each sample claims its alpha of what is left, "
    "and the remainder is a virtual background that carries no colour and "
    "no alpha), each claim is scattered with its owner's disc into an "
    "ARRIVAL plane, and a destination pixel whose arrival falls short of 1 "
    "has its premultiplied colour and alpha scaled up together by "
    "1/arrival. The fill only ever restores a shortfall; where more than "
    "unit weight arrives, the overlap saturation rule scales down as "
    "before.\n"
    "\n"
    "What the fill invents is FOREGROUND-coloured coverage, only where the "
    "renderer wrote none: it scales the samples that did arrive at a pixel "
    "to full coverage and adds nothing of its own, so it never invents "
    "background colour and never guesses at what an occluded surface would "
    "have looked like. Elements combined with DeepMerge carry their "
    "occluded samples and show through the defocused edge in their own "
    "colour; a sparse render is filled with the foreground's colour "
    "instead.\n"
    "\n"
    "The fill divides by the UN-HELD-OUT arrival: the arrival plane is "
    "deposited before holdout visibility is applied, while every colour and "
    "alpha deposit carries it, so holdout attenuation is preserved exactly "
    "and the fill and the holdout commute -- a holdout applied before the "
    "fill gives the same result as one applied after.\n"
    "\n"
    "'background depth' sets the defocus radius, in pixels, of the invented "
    "coverage for pixels the deep image left completely EMPTY. 0 (the "
    "default) is auto: the circle of confusion at the frame's farthest "
    "measured depth. Coverage behind a pixel's OWN samples always takes that "
    "pixel's deepest sample's defocus and ignores this knob. The knob "
    "matters when an isolated element sits far from the frame's farthest "
    "depth: its fringe over emptiness is then divided by a background "
    "scattered at a different radius than its own, and reads boosted where "
    "its disc is smaller than the background's and cut where it is larger "
    "-- tens of percent on a small element whose radius is more than twice "
    "the background's. Setting 'background depth' to the element's own "
    "radius removes that; the shortfall it fills is the same either way.\n"
    "\n"
    "LINEAR LIGHT, NO BLOOM\n"
    "\n"
    "The scatter is an energy-conserving weighted average of the values it is "
    "given. The node expects scene-linear premultiplied light and applies no "
    "highlight gain, no bloom, no glow and no clipping model.\n"
    "\n"
    "This matters if you are after a photographic look. Photographic bokeh "
    "gets its character from highlights far brighter than the recording "
    "medium can hold, so a small bright source spreads into a disc that still "
    "reads at full brightness. Feed this node genuinely unclamped linear "
    "values -- highlights well above 1.0 -- and it does that on its own. Feed "
    "it a source whose highlights were already clipped upstream, by a display "
    "transform or a clamp, and the limited energy spreads over the disc and "
    "reads as a dull grey circle. A plain gain before the node and its "
    "inverse after cannot recover it: the scatter is linear in colour, so a "
    "linear gain cancels exactly. It takes a non-linear highlight expansion "
    "upstream and its inverse downstream.\n"
    "\n"
    "VOLUMETRIC SAMPLES ARE SPLIT AND TREATED AT THEIR MIDPOINTS\n"
    "\n"
    "A sample with a depth span (deep front != deep back) is split where it "
    "crosses depth bucket boundaries, with its transmittance split "
    "analytically, so a fog slab does not collapse into one hard layer. Each "
    "resulting part is then given a single circle of confusion, computed at "
    "that part's midpoint depth, and scattered as one disc.\n"
    "\n"
    "So a volumetric sample is defocused in as many discrete steps as it "
    "crosses depth buckets, never continuously. A slab that sits inside one "
    "bucket gets exactly one radius, taken from its middle, however much the "
    "true circle of confusion varies across it. This is most visible on slabs "
    "that span a long depth range close to the camera, where the circle of "
    "confusion changes fastest. Raising 'depth layers' gives such a slab more "
    "steps.\n"
    "\n"
    "NO depth.Z AOV\n"
    "\n"
    "Unlike stock DeepToImage, this node does not synthesise a depth.Z "
    "output. A Z channel selected in 'channels' is flattened as an ordinary "
    "data channel: scattered and averaged like colour, which does not produce "
    "a meaningful depth value. The same applies to any other non-colour data "
    "channel -- normals, position, motion vectors all scatter identically to "
    "colour, which is usually not what is wanted from a defocused frame.\n"
    "\n"
    "KNOWN LIMITATIONS\n"
    "\n"
    "All of these are measured and reproducible, and none is fixed in this "
    "version.\n"
    "\n"
    "1. Alpha reads high on semi-transparent content whose blur radius "
    "changes steeply across the frame. Each disc is normalised over its own "
    "footprint, and where the circle of confusion changes fast a "
    "destination pixel receives MORE than unit weight from its neighbours. "
    "The coverage fill corrects only shortfalls of arrival, deliberately: "
    "dividing surpluses out as well was built and measured, and the arrival "
    "plane cannot tell a continuous surface's over-delivery from a "
    "defocused neighbour legitimately overlapping an occluder, so it "
    "punched holes in opaque geometry instead. Measured on a ground plane "
    "receding through focus at 0.5 circle-of-confusion pixels per scanline: "
    "interior alpha reads +5.9% high at alpha 0.10 and +3.4% high at alpha "
    "0.30 at the default 16 depth layers, +6.5% at 4 depth layers, and "
    "+7.1% as alpha approaches 0; at alpha 0.9 the rows nearest focus read "
    "up to +0.073. It is worst at low alpha and disappears at alpha 1, "
    "where the saturation clamp absorbs it. It tracks the blur gradient "
    "rather than the depth layer count: at a quarter of that slope the same "
    "measurement reads +0.34%. Ordinary content has far gentler gradients "
    "and sits orders of magnitude below these figures.\n"
    "\n"
    "2. Alpha reads high where defocused volumes of DIFFERENT density "
    "overlap in depth. Two semi-transparent elements whose depth ranges "
    "overlap and whose per-unit densities differ report more alpha together "
    "than they do apart. Worst measured case: two cards at alpha 0.99 and "
    "alpha 0.10, depth spans 8 to 12 and 9 to 13, defocused until their "
    "discs overlap, read 0.432 where the sum of the two rendered separately "
    "is 0.252, i.e. +71%; an equal-density pair of the same shape reads "
    "+1.5%. The information is lost when the fragments are accumulated, so "
    "no later compositing rule can recover it. With the coverage fill live "
    "the same measurement reads +87%: the extra is the fill's background-"
    "radius mismatch described under 'background depth' (two cards at "
    "different depths pool onto different radii, so no one knob value "
    "removes it), not more lost information.\n"
    "\n"
    "3. A holdout erases unoccluded geometry for one depth bracket in front "
    "of itself. Holdout visibility is interpolated between per-pixel "
    "transmittance values sampled at 'depth layers' + 1 depths spread "
    "uniformly over the frame's depth range. In the bracket immediately in "
    "front of a hard holdout edge that interpolation collapses to zero: "
    "visibility reads 1.0 just below the bracket's near boundary, 0.25 at 2% "
    "into it and 0.001 at 10% into it, so effectively the whole bracket is "
    "erased even though nothing occludes it. The bracket is the frame's "
    "depth range divided by 'depth layers' -- about 6 units on a scene "
    "spanning 100 units at the default 16 layers, and it shrinks "
    "proportionally as 'depth layers' rises. The error is one-sided: it errs "
    "toward erasing in front of a holdout rather than leaking through one, "
    "which is the safer direction for holdout work but is visible in the "
    "standard setup of an element sitting just in front of a held-out "
    "object.\n"
    "\n"
    "4. Cancelling a render mid-cook is NOT verified end to end. The band "
    "scheduler's abort protocol -- an aborted band returns to its dirty "
    "state, wakes any threads waiting on it, leaves those rows black, and "
    "the next cook recomputes it -- is unit-tested directly, including under "
    "thread stress. What has never been exercised is that path driven by "
    "Nuke's own cancel and row scheduler during an interactive cook. It is "
    "not known to work and it is not known to be broken. If a cancelled "
    "render leaves this node showing black rows that do not refresh, change "
    "any knob to force a recompute.\n"
    "\n"
    "NOT IN THIS VERSION\n"
    "\n"
    "Highlight and bloom controls; lens aberrations and shaped irises; GPU "
    "acceleration. This node is CPU only, and is built "
    "for Linux only -- it is excluded from the Windows build.\n"
    "\n"
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

static const char* const fillModeNames[] = {
    "foreground", "background", nullptr
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
    float _backgroundDepth;      // Float px, default 0.0 = auto, 0-500
    int   _fill;                 // Enum {foreground, background}, default foreground
    float _fillSearch;           // Float px, default 0.0 = auto, 0-500

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

    // ----------------------------------------------------------------------
    // Derived state — rebuilt by _validate(), read by _request()/engine().
    // ----------------------------------------------------------------------
    deepc::CocParams _cocParams;    // cached lens state, ALREADY PROXY-SCALED
                                    // (applyProxyScale() is called exactly once
                                    // per _validate, on freshly built params);
                                    // _validate deliberately does NOT build the
                                    // kernel LUT — that waits until
                                    // frameSetup() knows the frame's measured
                                    // CoC range
    float _proxyScale;              // current format width / full-size width
    float _formatHeightPx;          // current (proxy) format height, for the
                                    // ray-distance correction's filmback offset
    ChannelSet _outChannels;        // advertised output: selection u alpha (+ matte)
    ChannelSet _flattenChannels;    // what the cook writes: selection u alpha

    // ----------------------------------------------------------------------
    // The shared flat frame.
    //
    // Plane-major layout, planes.size() * box.h() * box.w() floats, through
    // PodBuffer (the allocation seam; base 64-byte aligned, individual
    // plane starts are not).
    //
    // This is a MUTABLE SHARED frame, not a published-by-copy snapshot.
    // Each claiming render thread writes exactly its own band's
    // DISJOINT row range of every plane; nothing else writes it.  Readers
    // (the engine() row copy) never touch a band that is not Done, and the
    // ledger's release/acquire pair on the band state is what publishes the
    // writes — see BandLedger's header.  Reallocation happens only inside
    // frameSetup(), which the ledger admits only after every in-flight band
    // has completed and every reader has drained (BandLedger::beginFrame).
    //
    // Publish-by-copy (a shared_ptr<const FrameCache> snapshot) is the wrong
    // primitive here: per-band claims need a mutable shared frame plus
    // per-band atomics, and snapshotting the pointer costs a frame-wide lock
    // acquisition on every row (~2160 per thread per 4K frame).
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

    struct BandJob;   // per-thread band scratch, defined below

    // ----------------------------------------------------------------------
    // Frame-global cook state, rebuilt by frameSetup() once per Op::hash()
    // under the ledger's setup claim.  Everything here is written
    // by exactly one thread (the setup owner, while nothing else runs) and
    // read by many (band computes + row copies) — the ledger's key handshake
    // is what makes that safe.
    // ----------------------------------------------------------------------
    struct FrameShared {
        deepc::FlattenParams                  fp;
        deepc::DepthBuckets                   buckets;
        deepc::HoldoutBoundaries              holdoutBoundaries;
        std::unique_ptr<deepc::DiscKernelLUT> kernel;

        DD::Image::Box       srcBox;
        DD::Image::Box       holdoutBox;
        std::vector<Channel> colorChannels;
        std::vector<int>     colorPlanes;
        int  alphaPlane       = -1;
        int  mattePlane       = -1;
        int  padY             = 0;
        float backgroundRadiusPx = 0.0f;   // resolveBackgroundRadiusPx(), see frameSetup()
        deepc::FillMode fillMode = deepc::FillMode::Foreground;   // resolvedFillMode()
        float fillSearchPx   = 0.0f;   // resolveFillSearchPx(), see frameSetup()
        int  bandHeight       = 1;    // never < 1
        int  bandCount        = 0;
        int  maxInFlight      = 1;    // memory-limit cap, floor 1
        bool holdoutConnected = false;
        bool allBandsDone     = false; // empty frame: publish zeros directly

        deepc::ScatterParams spBase;   // bandX/bandWidth/sharp threshold;
                                       // bandY/bandHeight are per band
    };

    FrameCache  _frame;    // the shared flat frame — disjoint band regions
    FrameShared _shared;

    // The per-band Dirty -> InProgress -> Done state machine, over the NDK's
    // own monitor (SignalLock = Lock + condition; wait() blocks, signal()
    // broadcasts).  The claim/wait/abort logic itself is POD and unit-tested
    // with std::thread in tests/test_defocus_scatter.cpp.
    deepc::BandLedger<DD::Image::SignalLock> _ledger;

    // BandJob pool: one job per CONCURRENT band, not per thread — the
    // memory-limit cap bounds in-flight bands, and pooling means an idle
    // render thread holds no band-sized scratch.  Cleared at frameSetup() so
    // a hash change releases the previous frame's capacity.
    DD::Image::Lock                       _jobLock;   // guards _jobPool only
    std::vector<std::unique_ptr<BandJob>> _jobPool;

    DD::Image::Hash _validatedHash;   // _validate()'s half of invalidation
    bool            _debugBands = false;   // DEEPC_DEFOCUS_DEBUG_BANDS=1:
                                           // log band -> thread claims
    bool            _debugStats = false;   // DEEPC_DEFOCUS_DEBUG_STATS=1:
                                           // log per-band scatter work counts

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
        _backgroundDepth(0.0f),
        _fill(static_cast<int>(deepc::FillMode::Foreground)),
        _fillSearch(0.0f),
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

        // Concurrency instrumentation: when set, every completed
        // band claim logs its band index, row range and pthread id to stderr,
        // so a headless render demonstrates (or refutes) that distinct render
        // threads claim distinct bands.  Off by default; costs one getenv per
        // Op construction and nothing per row.
        _debugBands = (std::getenv("DEEPC_DEFOCUS_DEBUG_BANDS") != nullptr);

        // Work instrumentation: when set, every band logs the scatter's own
        // counters to stderr.  Wall clock cannot resolve a knob that changes
        // the fragment count by a few percent on a thermally throttled box;
        // these counters are exact and machine-independent.
        _debugStats = (std::getenv("DEEPC_DEFOCUS_DEBUG_STATS") != nullptr);
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
    // "no deep source" (_validate() produces an empty image) and a
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
    // file must go through Op::input(n) and a dynamic_cast, i.e. through
    // these two helpers.
    DeepOp* input0() const { return dynamic_cast<DeepOp*>(Op::input(0)); }
    DeepOp* input1() const { return dynamic_cast<DeepOp*>(Op::input(1)); }

    // The Nuke viewer picks 2D/3D from this. Iop's default returns
    // (2D | 3D) when an Iop has inputs of a different type than itself —
    // which is exactly this node — so it must be pinned to 2D, as the NDK's
    // own DeepToImage example does.
    int getViewableModes() const override { return eViewableMode2D; }

    const char* Class() const override { return CLASS; }
    const char* node_help() const override { return HELP; }

    // Iop's default is the plain 2D box. This node consumes deep, so it
    // should read as a deep node in the DAG, like stock DeepToImage.
    const char* node_shape() const override { return DeepOp::DeepNodeShape(); }

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

        Float_knob(f, &_backgroundDepth, IRange(0.0, 500.0), "background_depth", "background depth");
        Tooltip(f, "Defocus radius, in pixels, of the invented coverage that fills in "
                    "behind pixels the deep image left completely empty.\n\n"
                    "0 = auto: the CoC at the frame's farthest measured depth. A manual "
                    "value is clamped to the frame's own measured radius range.\n\n"
                    "Has no effect on any pixel with at least one sample — those always "
                    "borrow their own deepest sample's defocus instead.");

        Enumeration_knob(f, &_fill, fillModeNames, "fill", "fill");
        Tooltip(f, "Chooses whether coverage the renderer left completely empty is "
                    "filled with foreground colour (default) or with background "
                    "colour borrowed from surrounding pixels.");

        Float_knob(f, &_fillSearch, IRange(0.0, 500.0), "fill_search", "fill search");
        Tooltip(f, "Search radius, in pixels, for the background colour that fill "
                    "borrows from surrounding pixels. 0 = auto.");

        // --- Output ----------------------------------------------------------
        Divider(f, "Output");

        Input_ChannelSet_knob(f, &_channels, 0, "channels", "channels");
        Tooltip(f, "Channels to defocus, from input 0. Alpha is always "
                    "processed regardless of this selection.\n\n"
                    "Non-colour data channels scatter exactly like colour, "
                    "which is rarely meaningful for them. In particular "
                    "this node does NOT synthesise a depth.Z output the way "
                    "DeepToImage does: a Z channel selected here is "
                    "flattened as an ordinary data channel.");

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
                    "compositor. Memory scales with K.\n\n"
                    "It also sets how many depths the holdout's "
                    "transmittance is sampled at, so it fixes the width of "
                    "the bracket in front of a holdout in which unoccluded "
                    "geometry is erased: that bracket is the frame's depth "
                    "range divided by this value. Raise it to shrink the "
                    "artefact. See the node help.");

        Bool_knob(f, &_preMerge, "pre_merge", "pre-merge");
        Tooltip(f, "Group adjacent-depth samples of one pixel whose CoC radii "
                    "are within merge_tolerance, and scatter each group as a "
                    "single fragment. A speed/accuracy trade, not a free "
                    "optimisation - see merge_tolerance. The tidy pass "
                    "itself is correctness-required and always on; this knob "
                    "does not affect it.");

        Float_knob(f, &_mergeTolerance, IRange(0.0, 2.0), "merge_tolerance", "merge tolerance");
        Tooltip(f, "How far apart, in CoC-radius pixels, two samples of one "
                    "pixel may be and still be grouped by pre-merge.\n\n"
                    "LOSSY AT ANY USEFUL VALUE. A group rasterises ONE disc, "
                    "at its front member's radius, so it is exact only when "
                    "the grouped radii round to the same kernel-radius bin - "
                    "and those bins are radius^2/512 px wide below 16 px "
                    "(0.0005 px at radius 0.5, 0.002 px at radius 1, 0.03 px "
                    "at radius 4). At the 0.25 default, two same-pixel layers "
                    "0.20 px apart at radius 1.2 and 1.4 move every rendered "
                    "pixel by 9.0e-02.\n\n"
                    "It buys that back: on a 2048x1080 frame at 20 samples "
                    "per pixel the default removes 53% of the fragments and "
                    "61% of the pixel deposits against 0, and renders 1.37x "
                    "faster. Set 0 for an exact scatter.");

        Float_knob(f, &_memoryLimit, IRange(1.0, 64.0), "memory_limit", "memory limit (GB)");
        Tooltip(f, "Caps concurrent in-flight bands (floors at 1 band, then "
                    "shrinks the band height — never deadlocks at 0).");
    }

    // ------------------------------------------------------------------
    // Clamped knob accessors.
    //
    // Every knob range in this node is soft, so nothing downstream of a knob
    // may use its raw value. These are the use-site clamps.
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

    // Pre-merge tolerance, in CoC-RADIUS pixels. It is a radius, so it is proxy-scaled exactly like the radii it is compared
    // against — applyProxyScale() cannot do it, because CocParams does not
    // carry the tolerance (same reason edge_softness is scaled at the LUT
    // build rather than in the params).
    float clampedMergeTolerancePx() const
    {
        const float t = (_mergeTolerance > 0.0f) ? std::min(_mergeTolerance, 16.0f) : 0.0f;
        return t * _proxyScale;
    }

    // background_depth, a radius in pixels like max_radius, so it is
    // proxy-scaled here rather than by applyProxyScale() — CocParams does not
    // carry it, same reasoning as clampedMergeTolerancePx() above. <= 0 is
    // left as-is (deepc::resolveBackgroundRadiusPx() reads that as "auto").
    float clampedBackgroundDepthPx() const
    {
        return (_backgroundDepth > 0.0f) ? _backgroundDepth * _proxyScale : 0.0f;
    }

    // fill's resolved mode. The knob is a plain enum index, so no clamp is
    // needed beyond the cast (Enumeration_knob already bounds it to the
    // label table).
    deepc::FillMode resolvedFillMode() const
    {
        return static_cast<deepc::FillMode>(_fill);
    }

    // fill_search, a search-radius in pixels like background_depth, so it is
    // proxy-scaled here rather than by applyProxyScale() — same reasoning as
    // clampedBackgroundDepthPx() above. <= 0 is left as-is (
    // deepc::resolveFillSearchPx() reads that as "auto").
    float clampedFillSearchPx() const
    {
        return (_fillSearch > 0.0f) ? _fillSearch * _proxyScale : 0.0f;
    }

    // memory_limit, in bytes. Soft range 1-64 GB; the floor is deliberately
    // well below the documented minimum so a user who types 0 gets the
    // smallest computable band rather than a division by zero.
    double memoryLimitBytes() const
    {
        const double gb = (_memoryLimit > 0.0625f) ? static_cast<double>(_memoryLimit) : 0.0625;
        return std::min(gb, 1024.0) * 1024.0 * 1024.0 * 1024.0;
    }

    // Output bbox pad, X. The anti-aliased disc edge is *centred* on the rim,
    // so the kernel's true nonzero extent runs half a softness beyond
    // max_radius; padding by max_radius alone would clip that outermost
    // scattered energy at the frame edge.
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
    // this node, and the mitigation is precisely that there is exactly one
    // function that answers the question.
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
    // Mirrored identically in _request() and in the fetch.
    static ChannelSet neededHoldoutChannels()
    {
        ChannelSet chans = Mask_Deep;
        chans += Mask_Alpha;
        return chans;
    }

    // ------------------------------------------------------------------
    // _validate()
    //
    // DANGER (verified core dump): this must never fall
    // through to Iop::_validate(). Iop::_validate() merges info from all
    // inputs via Iop::asIop(), which is a bare static_cast<Iop*> — this
    // node's inputs are DeepOps, which are not Iops, and the cast crashes
    // Nuke 17.0v3. Same for Iop::input0()/input(int); use this class's own
    // input0()/input1().
    //
    // Note what is deliberately absent: the kernel LUT is NOT built here.
    // It is built once the frame's measured CoC range is known, immediately
    // after computeDepthRange() — building it eagerly over [0, max_radius]
    // costs ~1.0GB at max_radius=500.
    // _validate() cannot know that range: it is measured per cook, so the LUT
    // belongs to frameSetup() and nothing here may depend on it.
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

        // Mark all bands Dirty on an Op::hash() change — cheap, no compute,
        // and NOT a _computed flag cleared unconditionally (that would throw
        // away a good frame on every viewer interaction).  The ledger's key
        // check in engine() is the authoritative gate; this half also catches
        // a hash change that never reaches engine() (e.g. a knob wiggled back
        // and forth).
        if (hash() != _validatedHash) {
            _validatedHash = hash();
            _ledger.invalidate();
        }
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
    // engine() — the per-band lazy-claim loop
    //
    // row.erase(channels) is the literal first statement: sparse deep pixels
    // otherwise emit whatever was left in the row buffer. Everything after
    // it is allowed to bail out at any point and the row stays black.
    //
    // The loop has exactly three outcomes per pass:
    //   * the row's band is Done for the current hash -> copy rows, return.
    //     THE COMMON CASE TAKES NO LOCK AT ALL: BandLedger::beginRead()'s
    //     fast path is two atomic ops, and bandDone() is an acquire load, so
    //     no frame-wide lock is taken per row.
    //   * the band is not Done -> claim a Dirty band, PREFERRING the row's
    //     own but taking any other rather than queueing behind it, compute it
    //     into a pooled BandJob's private planes, write its disjoint region of
    //     the shared frame, publish, loop to copy.  Only a thread that finds
    //     no Dirty band at all blocks on the ledger.
    //   * abort / upstream failure -> the band goes back to Dirty (never
    //     Done), waiters are woken, and the row stays erased/black.
    // ------------------------------------------------------------------
    void engine(int y, int x, int r, ChannelMask channels, Row& row) override
    {
        row.erase(channels);

        if (!input0() || aborted())
            return;

        const auto abortedFn = [this]() { return aborted(); };
        const std::uint64_t key = hash().value();

        for (;;) {
            if (_ledger.beginRead(key)) {
                // Setup is Done for this key, so _frame/_shared are stable
                // for as long as this read is held (a setup re-run drains
                // readers before touching either).
                const DD::Image::Box& box = _frame.box;
                if (y < box.y() || y >= box.t()) {
                    _ledger.endRead();
                    return;   // outside the frame: stays erased
                }
                const int band = (y - box.y()) / _shared.bandHeight;
                if (_ledger.bandDone(band)) {
                    copyRow(y, x, r, channels, row);
                    _ledger.endRead();
                    return;
                }
                const int boxY        = box.y();
                const int boxT        = box.t();
                const int bandHeight  = _shared.bandHeight;

                // The bands a row request can ever reach.  The decomposition
                // covers the PADDED box; engine() is never called outside
                // requestedBox(), so the pad bands beyond it are work nobody
                // asks for and the claim below must not offer them.
                const DD::Image::Box& req = requestedBox();
                const int firstBand = (std::max(req.y(), boxY) - boxY) / bandHeight;
                const int lastBand  = (std::min(req.t(), boxT) - 1 - boxY) / bandHeight;
                _ledger.endRead();

                // Take another Dirty band rather than queueing behind this
                // one.  Nuke hands its render threads consecutive rows, so
                // they all sit in the same band and all but the first would
                // otherwise wait out its whole compute — measured at 1.47
                // bands in flight and 1.72 of 4 cores busy without this.  The
                // search starts at the row's own band, so the uncontended case
                // still claims exactly the band that was asked for.
                //
                // In-window only: that is tryClaimDirtyBand()'s precondition,
                // and it is also the guard against an empty requestedBox(),
                // where req.t()-1-boxY is negative and truncates TOWARD ZERO
                // -- lastBand would come out 0, not -1.
                int work = -1;
                if (band >= firstBand && band <= lastBand) {
                    work = _ledger.tryClaimDirtyBand(key, band,
                                                     firstBand, lastBand);
                }
                if (work < 0) {
                    const deepc::BandClaim claim =
                        _ledger.acquireBand(key, band, abortedFn);
                    if (claim == deepc::BandClaim::Aborted)
                        return;
                    if (claim != deepc::BandClaim::Compute)
                        continue;   // Ready: copy next pass. Stale: re-setup.
                    work = band;
                }

                const int y0 = boxY + work * bandHeight;
                const int y1 = std::min(y0 + bandHeight, boxT);

                // This thread OWNS the band.  Compute into private planes,
                // write the disjoint region, publish or abandon.
                std::unique_ptr<BandJob> job = acquireJob();
                double fetchMs = 0.0;
                const auto bandStart = std::chrono::steady_clock::now();
                const bool ok = computeBand(_frame, *job, y0, y1,
                                            _debugBands ? &fetchMs : nullptr);
                const double bandMs =
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - bandStart).count();
                releaseJob(std::move(job));

                if (!ok) {
                    // Abort or upstream failure: Dirty (never Done), wake
                    // waiters, leave erased/black rows.
                    _ledger.abandonBand(work);
                    return;
                }
                _ledger.completeBand(work);

                if (_debugBands) {
                    std::fprintf(stderr,
                                 "DeepCDefocus: band %d rows [%d,%d) computed by "
                                 "thread 0x%lx in %.3f ms fetch %.3f ms\n",
                                 work, y0, y1,
                                 static_cast<unsigned long>(pthread_self()),
                                 bandMs, fetchMs);
                }
                continue;   // copy on the next pass
            }

            // No frame for this key yet: claim (or wait for) the frame-global
            // setup, then loop back to the read path.
            const deepc::FrameClaim fclaim = _ledger.beginFrame(key, abortedFn);
            if (fclaim == deepc::FrameClaim::Aborted)
                return;
            if (fclaim == deepc::FrameClaim::SetupCompute) {
                const bool ok = frameSetup();
                _ledger.endFrameSetup(ok, key,
                                      ok ? _shared.bandCount : 0,
                                      ok ? _shared.maxInFlight : 1,
                                      ok && _shared.allBandsDone);
                if (!ok)
                    return;   // aborted/failed: rows stay erased/black
                if (_debugBands) {
                    std::fprintf(stderr,
                                 "DeepCDefocus: setup bands %d height %d "
                                 "maxInFlight %d\n",
                                 _shared.bandCount, _shared.bandHeight,
                                 _shared.maxInFlight);
                }
            }
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
    // copyRow() — one row out of the shared frame, no lock held.
    //
    // Caller contract: between BandLedger::beginRead()/endRead(), and only
    // after bandDone(band) — the acquire load there is what makes the band's
    // writes visible.
    // ------------------------------------------------------------------
    void copyRow(int y, int x, int r, ChannelMask channels, Row& row) const
    {
        const DD::Image::Box& box = _frame.box;

        const int xs = std::max(x, box.x());
        const int xe = std::min(r, box.r());
        if (xs >= xe)
            return;

        foreach(z, channels) {
            const int plane = _frame.planeIndex(z);
            if (plane < 0)
                continue;   // channel this node does not produce; stays erased

            const float* src = _frame.rowPtr(plane, y) - box.x();
            float* dst = row.writable(z);
            for (int i = xs; i < xe; ++i)
                dst[i] = src[i];
        }
    }

    // ------------------------------------------------------------------
    // The BandJob pool.  One job per CONCURRENT band (bounded by the
    // memory-limit cap), reused across bands and cooks so warmed-up capacity
    // survives; cleared at frameSetup() so a hash change releases the old
    // frame's scratch.  _jobLock guards the pool vector only — it is held
    // for a pointer move, never across any compute.
    // ------------------------------------------------------------------
    std::unique_ptr<BandJob> acquireJob()
    {
        std::unique_ptr<BandJob> job;
        {
            Guard guard(_jobLock);
            if (!_jobPool.empty()) {
                job = std::move(_jobPool.back());
                _jobPool.pop_back();
            }
        }
        if (!job)
            job.reset(new BandJob);

        // Prime from the frame-global state (pointers and PODs only).  Safe
        // without the ledger read guard: the caller holds a band claim, and
        // frameSetup() cannot run while any band is in flight.
        job->src               = input0();
        job->holdout           = _shared.holdoutConnected ? input1() : nullptr;
        job->fp                = &_shared.fp;
        job->buckets           = &_shared.buckets;
        job->holdoutBoundaries = &_shared.holdoutBoundaries;
        job->kernel            = _shared.kernel.get();
        job->srcBox.set(_shared.srcBox.x(), _shared.srcBox.y(),
                        _shared.srcBox.r(), _shared.srcBox.t());
        job->holdoutBox.set(_shared.holdoutBox.x(), _shared.holdoutBox.y(),
                            _shared.holdoutBox.r(), _shared.holdoutBox.t());
        job->colorChannels     = &_shared.colorChannels;
        job->colorPlanes      = &_shared.colorPlanes;
        job->alphaPlane        = _shared.alphaPlane;
        job->mattePlane        = _shared.mattePlane;
        job->padY              = _shared.padY;
        job->backgroundRadiusPx = _shared.backgroundRadiusPx;
        job->fillMode           = _shared.fillMode;
        job->fillSearchPx       = _shared.fillSearchPx;
        job->sp                = _shared.spBase;
        return job;
    }

    void releaseJob(std::unique_ptr<BandJob> job)
    {
        Guard guard(_jobLock);
        _jobPool.push_back(std::move(job));
    }

    // ==================================================================
    //
    //  THE COOK — frame setup once, then per-band lazy claim
    //
    //  frameSetup() — ONCE per Op::hash(), under the ledger's setup claim
    //  (nothing else runs while it does; see BandLedger::beginFrame):
    //
    //    1. computeDepthRange()  one cheap full-frame DeepFront/DeepBack/
    //                            Alpha pass, alpha-weighted; ALSO counts
    //                            samples per source row, which is what the
    //                            memory budget's per-band SoA estimate is
    //                            derived from
    //    2. DepthBuckets         bounded-DeltaCoC, from that range
    //       HoldoutBoundaries    uniform in Z over the SAME range, built ONCE
    //                            per frame (a per-band set seams every band
    //                            boundary — measured vis 0.0448 vs 1.0000 for
    //                            one fragment either side of one)
    //       DiscKernelLUT        over the frame's MEASURED radius range, with
    //                            rMin = 0
    //    3. deepc::planBands()   band height + the memory-limit cap on
    //                            CONCURRENT in-flight bands, from the
    //                            COMBINED bucket-plane + holdout-LUT +
    //                            SoA-fragment budget (floor 1 band, then
    //                            shrink B — never deadlock at 0)
    //
    //  computeBand() — per CLAIMED band, on whichever render thread claimed
    //  it: fetch band +/- padY source rows -> SoA flatten -> holdout LUT ->
    //  scatterBandCPU -> saturate + resolveBandCPU -> write the band's
    //  disjoint region of the shared frame.
    //
    //  Both return false if the cook was aborted or an upstream deepEngine()
    //  failed; the caller then abandons (band -> Dirty, never Done).
    // ==================================================================

    // Everything one band needs, plus the scratch that is reused across
    // bands and cooks.  One instance per CONCURRENT band, pooled (_jobPool):
    // these are the claiming thread's private bucket planes.
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
        float backgroundRadiusPx = 0.0f;   // see FrameShared::backgroundRadiusPx
        deepc::FillMode fillMode = deepc::FillMode::Foreground;   // see FrameShared::fillMode
        float fillSearchPx   = 0.0f;   // see FrameShared::fillSearchPx

        // bandY / bandHeight are filled per band; everything else (origin,
        // width, sharp threshold) is set once, explicitly, from the knobs.
        deepc::ScatterParams sp;

        deepc::SampleSoA        soa;
        deepc::FlattenScratch   flattenScratch;
        deepc::ScatterScratch   scatterScratch;
        deepc::BucketPlanes     planes;
        deepc::ResidualWindow   residual;   // virtual-background T/radius, full fetch window
        deepc::HoldoutSampleSoA holdoutSamples;
        deepc::HoldoutLut       holdoutLut;

        std::vector<deepc::SampleRecord> samples;         // source scratch
        std::vector<deepc::SampleRecord> holdoutRecords;  // holdout scratch

        std::vector<float> bandColor;
        std::vector<float> bandAlpha;
        std::vector<float> bandMatte;
    };

    bool frameSetup()
    {
        FrameCache& fc = _frame;

        DeepOp* src = input0();
        if (!src)
            return false;

        // A hash change made every pooled job's warmed-up capacity stale
        // (band geometry, channel count, fragment counts all move with it);
        // release rather than carry two frames' worth. Safe: the setup claim
        // guarantees no band is in flight, so the pool holds every job.
        {
            Guard guard(_jobLock);
            _jobPool.clear();
        }

        // Publish-empty defaults: every early "nothing to produce" return
        // below leaves a valid all-zero frame in which every band is
        // immediately Done (engine()'s endFrameSetup call passes
        // _shared.allBandsDone through).
        _shared.allBandsDone = true;
        _shared.bandHeight   = 1;
        _shared.bandCount    = 1;
        _shared.maxInFlight  = 1;

        // Component-wise, not Box copy-assign: the NDK's Box has a
        // user-provided copy constructor, so its implicit copy-assignment
        // operator is deprecated and warns under -Wdeprecated-copy.
        fc.box.set(info_.box().x(), info_.box().y(), info_.box().r(), info_.box().t());
        fc.planes.clear();
        foreach(z, _outChannels)
            fc.planes.push_back(z);

        const size_t nPlanes = fc.planes.size();
        if (nPlanes == 0 || fc.box.w() <= 0 || fc.box.h() <= 0) {
            fc.data.assign(0, 0.0f);
            _shared.bandCount = 0;   // engine() rejects every y as outside
            return true;             // nothing to produce, but a valid result
        }

        _shared.bandHeight = fc.box.h();   // one all-Done band, once valid
        fc.data.assign(nPlanes * fc.planeStride(), 0.0f);

        // --- plane routing -------------------------------------------------
        // ALPHA IS NOT A SCATTER CHANNEL. The bucket planes carry alpha as a
        // first-class quantity (it is what the transmittance split, the
        // saturation pass and both bucket composites operate on), so the
        // scatter's channel list is the selection MINUS alpha and the
        // composite's own outAlpha is what lands in the alpha plane. Carrying
        // alpha as an ordinary channel as well would composite it through the
        // colour path — a different expression — and the two would disagree.
        _shared.colorChannels.clear();
        _shared.colorPlanes.clear();
        foreach(z, _flattenChannels) {
            if (z == Chan_Alpha)
                continue;
            const int p = fc.planeIndex(z);
            if (p >= 0) {
                _shared.colorChannels.push_back(z);
                _shared.colorPlanes.push_back(p);
            }
        }
        _shared.alphaPlane = fc.planeIndex(Chan_Alpha);
        _shared.mattePlane = (_outputHoldoutMatte && _holdoutMatteChannel != Chan_Black)
                           ? fc.planeIndex(_holdoutMatteChannel)
                           : -1;

        // Only pixels the deep source covers can carry samples; everything
        // else in the padded box stays exactly 0.0 unless a disc reaches it.
        const DD::Image::Box srcBox = src->deepInfo().box();
        _shared.srcBox.set(srcBox.x(), srcBox.y(), srcBox.r(), srcBox.t());
        if (srcBox.w() <= 0 || srcBox.h() <= 0 || !fc.box.intersects(srcBox))
            return true;

        // --- flatten params, built ONCE ------------------------------------
        // The depth-range pass, the source flatten and the holdout's
        // depthScale all read this same instance, so they cannot disagree
        // about the ray-distance correction (see rayDepthScaleAt()).
        deepc::FlattenParams& fp = _shared.fp;
        fp                    = deepc::FlattenParams();
        fp.coc                = _cocParams;              // proxy-scaled already
        fp.preMerge           = _preMerge;               // EXPLICIT, never a default
        fp.mergeTolerancePx   = clampedMergeTolerancePx();
        fp.depthIsRayDistance = _depthIsRayDistance;
        fp.formatHeightPx     = _formatHeightPx;
        fp.channelCount       = static_cast<int>(_shared.colorChannels.size());
        fp.groups             = deepc::makeSingleChannelGroup(fp.channelCount);

        // --- 1. the depth-range pass ---------------------------------------
        // Also counts deep samples per source row: the memory budget below
        // estimates each band's SoA fragment count from its fetch window's
        // sample count, and this pass already touches every sample.
        float depthMin  = 0.0f;
        float depthMax  = 0.0f;
        bool  anyAlpha  = false;
        std::vector<double> rowSamples;
        if (!computeDepthRange(src, srcBox, fp, depthMin, depthMax, anyAlpha,
                               rowSamples))
            return false;
        if (!anyAlpha)
            return true;   // no contributing sample anywhere: frame stays black

        // --- 2. buckets, holdout boundary set, kernel LUT ------------------
        _shared.buckets =
            deepc::makeBoundedDeltaCocBuckets(fp.coc, depthMin, depthMax,
                                              clampedDepthLayers());
        const deepc::DepthBuckets& buckets = _shared.buckets;

        // FRAME-GLOBAL, never per band: a fragment near a band edge scatters
        // into two bands, and per-band sets put a seam along every boundary.
        _shared.holdoutBoundaries = deepc::makeUniformHoldoutBoundaries(buckets);

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

        // background_depth: 0 (auto) resolves to the CoC at the frame's
        // farthest measured depth; a manual value is clamped to rMax. Passed
        // through explicitly (this file's convention — see BandJob) rather
        // than read as a member from inside computeBand()'s residual-map
        // build, which is the only consumer for now.
        const float cocAtDepthMax = deepc::radiusPixels(fp.coc, buckets.depthMax());
        _shared.backgroundRadiusPx = deepc::resolveBackgroundRadiusPx(
            clampedBackgroundDepthPx(), cocAtDepthMax, rMax);

        // fill / fill_search: carried through explicitly, same convention as
        // background_depth above. Read by nothing else yet.
        _shared.fillMode = resolvedFillMode();
        _shared.fillSearchPx = deepc::resolveFillSearchPx(
            clampedFillSearchPx(), _shared.backgroundRadiusPx,
            static_cast<float>(clampedMaxRadius()));

        // edge_softness is proxy-scaled HERE. applyProxyScale() deliberately
        // does not touch it — CocParams does not carry it — so the LUT build
        // is where it lands.
        const float softness    = clampedEdgeSoftness() * _proxyScale;
        const float pixelAspect = fp.coc._pixelAspect;

        // rMin = 0, NOT the measured minimum: a query below rMin is clamped
        // UP to the rMin kernel, so a measured rMin would visibly over-blur
        // every radius between the sharp-path threshold and it. The range
        // parameter exists to bound rMax, which is where the ~1.0GB worst
        // case lives.
        _shared.kernel.reset(
            new deepc::DiscKernelLUT(0.0f, rMax, softness, pixelAspect));

        // --- 3. band decomposition -----------------------------------------
        const int W = fc.box.w();
        const int C = static_cast<int>(_shared.colorChannels.size());
        const int K = buckets.bucketCount();

        DeepOp* holdout = input1();
        DD::Image::Box holdoutBox;
        if (holdout) {
            const DD::Image::Box& hb = holdout->deepInfo().box();
            holdoutBox.set(hb.x(), hb.y(), hb.r(), hb.t());
        }
        _shared.holdoutBox.set(holdoutBox.x(), holdoutBox.y(),
                               holdoutBox.r(), holdoutBox.t());
        const bool holdoutConnected = (holdout != nullptr)
                                   && holdoutBox.w() > 0 && holdoutBox.h() > 0;
        _shared.holdoutConnected = holdoutConnected;

        // The flatten needs to know too: with a holdout connected
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
        _shared.padY = padY;

        // --- 4. band height + the concurrent-band cap ----------------------
        //
        // B = clamp(2*maxRadius, 32, 256), then deepc::planBands() shrinks it
        // (never below 1 row) until ONE band's scratch fits the memory limit,
        // and derives the cap on CONCURRENT in-flight bands (floor 1 — never
        // 0, so the ledger cannot deadlock).  Every term is evaluated on
        // CLAMPED values, never raw knob values.
        //
        // The budget is a COMBINED figure: bucket planes K*W*B*(C+3)*4, PLUS
        // the virtual-background window 2*W*(B+2*padY)*4 (ResidualWindow —
        // it is owned per-BandJob exactly like the bucket planes, so `padY`
        // is passed through here rather than left at the default 0), PLUS
        // the holdout LUT (K+1)*W*B*4 (measured 17.0 MB per 4096x64 band at
        // K=16), PLUS the SoA fragment stream at ~100 B/fragment RESIDENT
        // (61 B logical), which is the DOMINANT term at 4K (~1.49GB against
        // ~117MB of planes).  The fragment count is estimated per
        // band as the deep-sample count over its FETCH window (band +/- padY
        // rows), from the per-row counts the depth-range pass just gathered;
        // the cap uses the WORST band's figure, since it is one number for
        // the whole frame.  See deepc::bandBudgetBytes() for the estimate's
        // stated error terms.
        const std::size_t nSrcRows = rowSamples.size();
        std::vector<double> prefix(nSrcRows + 1, 0.0);
        for (std::size_t i = 0; i < nSrcRows; ++i)
            prefix[i + 1] = prefix[i] + rowSamples[i];

        const auto worstBandFragments = [&](int b) -> double {
            double worst = 0.0;
            for (int y0 = fc.box.y(); y0 < fc.box.t(); y0 += b) {
                const int y1  = std::min(y0 + b, fc.box.t());
                const int fy0 = std::max(srcBox.y(), y0 - padY);
                const int fy1 = std::min(srcBox.t(), y1 + padY);
                if (fy1 <= fy0)
                    continue;
                const double s = prefix[static_cast<std::size_t>(fy1 - srcBox.y())]
                               - prefix[static_cast<std::size_t>(fy0 - srcBox.y())];
                if (s > worst)
                    worst = s;
            }
            return worst;
        };

        const int initialBandHeight = std::min(
            deepc::clampi(static_cast<int>(std::ceil(2.0f * rMax)), 32, 256),
            fc.box.h());

        const deepc::BandPlan plan = deepc::planBands(
            memoryLimitBytes(), fc.box.h(), K, C, W, holdoutConnected,
            initialBandHeight, worstBandFragments, padY);

        _shared.bandHeight   = plan.bandHeight;
        _shared.bandCount    = plan.bandCount;
        _shared.maxInFlight  = plan.maxInFlight;
        _shared.allBandsDone = false;   // real content: bands start Dirty

        if (_debugBands) {
            const double fragments = worstBandFragments(plan.bandHeight);
            const double perBand = deepc::bandBudgetBytes(
                K, C, W, plan.bandHeight, holdoutConnected, fragments, padY);
            std::fprintf(stderr,
                         "DeepCDefocus: budget limitGB %.3f bandGB %.3f "
                         "maxInFlight %d promisedGB %.3f fragments %.0f\n",
                         memoryLimitBytes() / 1073741824.0,
                         perBand / 1073741824.0, plan.maxInFlight,
                         perBand * plan.maxInFlight / 1073741824.0,
                         fragments);
        }

        _shared.spBase               = deepc::ScatterParams();
        _shared.spBase.bandX         = fc.box.x();
        _shared.spBase.bandWidth     = W;
        _shared.spBase.sharpRadiusPx = deepc::kSharpRadiusPx;

        // No band loop here: bands are computed lazily, per claim, on Nuke's
        // own render threads — see engine().
        return true;
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
    //
    // Also fills `rowSamples` — deep samples per source row (index
    // y - srcBox.y()) — for the memory budget's per-band SoA estimate.  It
    // counts every sample of every depth-and-alpha-bearing pixel, INCLUDING
    // alpha<=0 samples the flatten later drops: over-counting is the safe
    // direction for a budget, and this pass is the one place that already
    // touches every sample for free.
    // ------------------------------------------------------------------
    bool computeDepthRange(DeepOp* src,
                          const DD::Image::Box& srcBox,
                          const deepc::FlattenParams& fp,
                          float& depthMin,
                          float& depthMax,
                          bool&  anyAlpha,
                          std::vector<double>& rowSamples)
    {
        depthMin = 0.0f;
        depthMax = 0.0f;
        anyAlpha = false;
        rowSamples.assign(static_cast<std::size_t>(std::max(0, srcBox.h())), 0.0);

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

                rowSamples[static_cast<std::size_t>(y - srcBox.y())]
                    += static_cast<double>(n);

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
    // Returns false on abort / upstream failure; the caller then abandons the
    // band (Dirty, never Done), so nothing partial is ever published — this
    // function writes the shared frame only after a fully successful band.
    // ------------------------------------------------------------------
    bool computeBand(FrameCache& fc, BandJob& job, int y0, int y1,
                     double* fetchMs = nullptr)
    {
        const int h = y1 - y0;
        const int W = fc.box.w();
        if (h <= 0 || W <= 0)
            return true;

        const int C = static_cast<int>(job.colorChannels->size());
        const int K = job.buckets->bucketCount();
        const std::ptrdiff_t px = static_cast<std::ptrdiff_t>(W) * h;

        // --- source fetch: band +/- padY, clipped to the source bbox -------
        // Also builds the virtual-background window: T = 1 (and the
        // background radius) for every fetch-window pixel inside the OUTPUT
        // box, whether or not it lies in `srcBox` — see
        // deepc::buildResidualWindow(). A deep input whose bbox is tight
        // around its content still needs a virtual background on every bloom
        // pixel outside that bbox, or arrival there equals the bloom's own
        // weight and a soft edge divides to a hard disc.
        job.soa.begin(C, job.fp->groups);

        const ChannelSet need = neededDeepChannels();

        DeepPlane deepRow;   // captured by both callbacks below
        const auto fetchStart = std::chrono::steady_clock::now();
        const bool fetchOk = deepc::buildResidualWindow(
            job.residual,
            fc.box.x(), fc.box.r(), fc.box.y(), fc.box.t(),
            job.srcBox.x(), job.srcBox.r(), job.srcBox.y(), job.srcBox.t(),
            y0, y1, job.padY, job.backgroundRadiusPx,
            [&](int y) -> bool {
                if (aborted())
                    return false;
                if (!job.src->deepEngine(y, job.srcBox.x(), job.srcBox.r(), need, deepRow)) {
                    Iop::abort();
                    return false;
                }
                return true;
            },
            [&](int x, int y, float& residualT, float& residualRadiusPx) -> bool {
                if (!fillSampleRecords(deepRow.getPixel(y, x), *job.colorChannels,
                                       job.samples))
                    return false;
                deepc::flattenPixelToSoA(*job.fp, *job.buckets, x, y,
                                         job.samples, job.flattenScratch,
                                         job.soa, nullptr,
                                         &residualT, &residualRadiusPx);
                return true;
            });
        if (!fetchOk)
            return false;
        if (fetchMs) {
            *fetchMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - fetchStart).count();
        }

        // --- holdout -------------------------------------------------------
        // begin() with NO appendPixel() calls at all is well defined and lands
        // on exactly the same disabled view, so the per-pixel loop is skipped
        // ENTIRELY when there is no holdout or the band misses its bbox —
        // discovering emptiness by running it costs ~1.98 ms/band, ~67 ms per
        // 4K frame of pure bookkeeping.
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
                    // compounded samples of it. The log1p/expm1 form is
                    // exact in the same limit —
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
        job.sp.bandY      = y0;
        job.sp.bandHeight = h;

        job.planes.allocate(K, C, W, h);   // sizes AND zeroes; keeps capacity

        // CALLER-SIDE GEOMETRY ASSERT: scatterBandCPU() SILENTLY RETURNS
        // when the planes' geometry disagrees with params.bandWidth/
        // bandHeight (the planes own the memory, so the disagreement must not
        // be resolved in favour of the side that doesn't).  Under per-band
        // claiming that silent return would surface as a BLACK BAND published
        // as Done, so the mismatch is surfaced as a loud error here instead.
        {
            const deepc::BucketPlaneView v = job.planes.view();
            if (!v.valid()
                || v.width != job.sp.bandWidth || v.height != job.sp.bandHeight) {
                error("DeepCDefocus: internal band geometry mismatch "
                      "(planes %dx%d vs band %dx%d) — band left black",
                      v.width, v.height, job.sp.bandWidth, job.sp.bandHeight);
                return false;
            }
        }

        job.bandColor.assign(static_cast<size_t>(C) * static_cast<size_t>(px), 0.0f);
        job.bandAlpha.assign(static_cast<size_t>(px), 0.0f);

        deepc::ScatterStats stats;
        deepc::scatterBandCPU(job.sp, job.soa, holdoutView, *job.kernel,
                              job.planes, job.scatterScratch,
                              _debugStats ? &stats : nullptr);
        if (_debugStats) {
            std::fprintf(stderr,
                         "DeepCDefocus: stats rows [%d,%d) fragments %zu "
                         "sharp %zu culled %zu rowSpans %zu pixelDeposits %zu\n",
                         y0, y1, stats.fragments, stats.sharpFragments,
                         stats.culled, stats.rowSpans, stats.pixelDeposits);
        }

        // The virtual background: `arrival` only, never color/alpha/weight/
        // colocated -- see scatterBackgroundCPU(). It has to run between the
        // fragment scatter and the resolve, because the resolve divides by the
        // finished `arrival` and a residual missing from it reads as a
        // coverage deficit that is not there.
        deepc::scatterBackgroundCPU(job.sp, job.residual, *job.kernel,
                                    job.planes);

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
    // *** NOT ON THE COOK PATH ***  the cook scatters
    // (frameSetup()/computeBand()).  This plain flatten is kept for two
    // reasons:
    //   * it is the REFERENCE the DeepToImage parity gate is established
    //     against, and the parity numbers below are the record of it;
    //   * it carries one of the TWO independent layers of the FMA /
    //     fp-contract parity guard — the CMake-level omission of -mfma is
    //     the belt, the pragma below is the braces.
    // The arithmetic that produces the shipped pixels lives in
    // DeepCDefocusScatter.{h,cpp} (the pre-merge `over` and the bucket
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
    //   The tidy pass is correctness-required for the scatter
    //   (coincident samples must not be *added*), so the re-association is
    //   deliberate — but any "<= N ULP" acceptance threshold has to be stated
    //   against a named scene, not as a universal bound.
    //
    //   VOLUMETRIC SPANS THAT OVERLAP OR COINCIDE — parity to float
    //   precision.  Two samples sharing an interval are co-located media, so
    //   their optical depths and emission ADD rather than one occluding the
    //   other: deepc::tidyOverlapping() merges them by the OpenEXR
    //   "Interpreting Deep Pixels" volume-mixture rule, which is what Nuke's
    //   own CombineOverlappingSamples computes. Measured (Nuke 17.0v3,
    //   headless, all four rgba channels over every pixel of a 64x48 frame,
    //   -O3 -mavx2 -mfma), against DeepToImage with volumetric_composition ON
    //   — its default:
    //     - two partially overlapping spans ................... 1.8e-07
    //     - two perfectly coincident spans .................... 6.0e-08
    //     - three-way overlap ................................. 2.4e-07
    //   i.e. the same ~1-2 ULP re-association noise as the coincident
    //   point-sample case above, so a DeepToImage-parity gate does not have
    //   to be scoped to point samples.
    //
    //   Against DeepToImage with volumetric_composition OFF the same scenes
    //   differ by 1.5e-02 to 4.7e-02, and that is deliberate: that setting
    //   selects Nuke's plain `over` of overlapping spans, not the mixture
    //   rule the tidy pass applies. Do not use it as the parity reference
    //   for volumetric input.
    // ------------------------------------------------------------------
#if defined(__GNUC__) && !defined(__clang__)
    // See the composite loop below: fusing its multiply and add into an FMA
    // changes the rounding and costs bit-exact parity with DeepToImage
    // (measured: 1.2e-07 divergence under -O3 -mavx2 -mfma). The build may
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
        // This loop must NOT be compiled with FMA contraction enabled
        // (`-ffp-contract=fast` plus `-mfma`), which would fuse the multiply
        // and add and cost the bit-exactness.
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
