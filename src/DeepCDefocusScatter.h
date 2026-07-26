// SPDX-License-Identifier: MIT
//
// ============================================================================
//
//  DeepCDefocusScatter — POD scatter core for DeepCDefocus
//
//  This header (and its .cpp) is the M3 CUDA seam.  ABSOLUTELY NO DDImage/NDK
//  type may cross into it: it compiles, and is unit-testable, with a bare
//  `g++ -std=c++17 -fsyntax-only`.  The node's NDK side fetches deep pixels and
//  hands this file plain std::vector<deepc::SampleRecord> and POD parameter
//  structs; everything from there to the flat band output is NDK-free.
//
//  Contents (M1.P3.T1):
//
//    - PodBuffer<T> : thin OWNING wrapper over a 64-byte-aligned host
//                     allocation.  EVERY SoA / plane buffer in this node goes
//                     through it from day 1, so M3 swaps only the two static
//                     allocate/deallocate functions (-> cudaMalloc /
//                     cudaFree) and the loop drivers, never the kernel source.
//    - ChannelGroups: the SoA channel-group layout, carrying the
//                     channelRadiusScale[] hook required by the design
//                     reference (all 1.0 in v1; M2's chromatic aberration
//                     fills it in).
//    - SampleSoA   : the flattened, bucket-assigned fragment stream the
//                     scatter (M1.P3.T2) consumes.
//    - flattenPixelToSoA() : tidy -> volumetric bucket split -> CoC -> bucket
//                     assignment -> pre-merge -> SoA append, for one deep
//                     pixel.
//
//  DEEPC_HD is NOT defined here.  It is owned by DeepCDefocusMath.h (milestone
//  Decisions, 2026-07-26) and inherited by including that header, so a .cu
//  translation unit that pre-defines the macro gets one consistent expansion
//  no matter which of these headers it includes first.
//
//  Style: scalar, __restrict__-annotated, auto-vectorization-friendly; no
//  intrinsics, no SIMD library, no OpenMP (board-level Context).
//
// ============================================================================

#ifndef DEEPC_DEFOCUS_SCATTER_H
#define DEEPC_DEFOCUS_SCATTER_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

// Owns DEEPC_HD, CocParams, DepthBuckets, the composition-contract primitives
// (splitSpanAtBoundaries / bucketOf / bucketOfContaining / fragmentDeposit)
// and the bucket-plane layout this file's output feeds.
#include "DeepCDefocusMath.h"

// deepc::SampleRecord, deepc::tidyOverlapping(), deepc::optimizeSamples().
// Reused rather than reimplemented, per the milestone's Context.
#include "DeepSampleOptimizer.h"

namespace deepc {

// ---------------------------------------------------------------------------
// PodBuffer<T> — owning, aligned, trivially-copyable-only host buffer
//
// WHY THIS EXISTS.  It is not a std::vector replacement for its own sake: it
// is the allocation seam for the CUDA milestone.  Every SoA array, bucket
// plane and holdout LUT this node allocates goes through PodBuffer, so M3
// replaces exactly two functions — allocateBytes()/deallocateBytes() below —
// with cudaMalloc/cudaFree (or a managed/pinned variant) and the kernel source
// that indexes .data() is untouched.  std::vector cannot play that role: its
// allocator is baked into the type, it value-initialises on resize (a
// measurable cost on multi-hundred-megabyte plane buffers that are about to be
// overwritten anyway), and it gives no alignment guarantee.
//
// SEMANTICS, in one place, because they differ from std::vector on purpose:
//   * T must be trivially copyable AND trivially destructible.  No constructor
//     or destructor is ever run; growth is a memcpy.  This is what makes the
//     device swap legal.
//   * MOVE-ONLY.  A copy of a several-hundred-megabyte plane buffer is always
//     a bug in this node, so it is a compile error rather than a silent cost.
//   * resize(n)               — preserves min(oldSize, n) elements.  New tail
//                               elements are UNINITIALISED.
//   * resizeUninitialized(n)  — preserves nothing.  Cheaper when the caller is
//                               about to overwrite everything (the band path).
//   * assign(n, v)            — resizeUninitialized(n) + fill.
//   * reserve(n)              — grows capacity, preserving contents.
//   * clear()                 — size 0, capacity KEPT (band-to-band reuse:
//                               a band must not re-malloc every cook).
//   * release()               — frees.
//   * data() is nullptr at size 0 (never a dangling one-past-the-end pointer).
//
// Alignment is 64 bytes: one cache line, and enough for any AVX-512 load, so
// the scatter's `dst[i] += w[i]*c` row spans start aligned whenever their pixel
// offset is.
// ---------------------------------------------------------------------------
template <typename T>
class PodBuffer {
public:
    static_assert(std::is_trivially_copyable<T>::value,
                  "PodBuffer<T> requires a trivially copyable T: growth is a memcpy "
                  "and the M3 device allocator never runs constructors.");
    static_assert(std::is_trivially_destructible<T>::value,
                  "PodBuffer<T> requires a trivially destructible T: it never runs "
                  "destructors.");

    static constexpr std::size_t kAlignment = 64;

    PodBuffer() noexcept = default;

    explicit PodBuffer(std::size_t count) { resizeUninitialized(count); }

    PodBuffer(std::size_t count, T value) { assign(count, value); }

    ~PodBuffer() { release(); }

    PodBuffer(const PodBuffer&) = delete;
    PodBuffer& operator=(const PodBuffer&) = delete;

    PodBuffer(PodBuffer&& other) noexcept
        : _data(other._data), _size(other._size), _capacity(other._capacity)
    {
        other._data     = nullptr;
        other._size     = 0;
        other._capacity = 0;
    }

    PodBuffer& operator=(PodBuffer&& other) noexcept
    {
        if (this != &other) {
            release();
            _data           = other._data;
            _size           = other._size;
            _capacity       = other._capacity;
            other._data     = nullptr;
            other._size     = 0;
            other._capacity = 0;
        }
        return *this;
    }

    // --- observers ---------------------------------------------------------
    T*       data() noexcept       { return _data; }
    const T* data() const noexcept { return _data; }

    std::size_t size() const noexcept     { return _size; }
    std::size_t capacity() const noexcept { return _capacity; }
    bool        empty() const noexcept    { return _size == 0; }

    // Real allocation footprint, for the memory-limit knob's band budgeting.
    std::size_t sizeBytes() const noexcept { return _capacity * sizeof(T); }

    T&       operator[](std::size_t i) noexcept       { return _data[i]; }
    const T& operator[](std::size_t i) const noexcept { return _data[i]; }

    T*       begin() noexcept       { return _data; }
    T*       end() noexcept         { return _data + _size; }
    const T* begin() const noexcept { return _data; }
    const T* end() const noexcept   { return _data + _size; }

    // --- modifiers ---------------------------------------------------------

    // Grow capacity, preserving the live elements.  Never shrinks.
    void reserve(std::size_t count)
    {
        if (count <= _capacity)
            return;

        T* fresh = allocate(count);
        if (_data != nullptr && _size > 0)
            copyBytes(fresh, _data, _size * sizeof(T));
        deallocate(_data, _capacity);
        _data     = fresh;
        _capacity = count;
    }

    // Preserves min(oldSize, count) elements; any new tail is uninitialised.
    void resize(std::size_t count)
    {
        reserve(count);
        _size = count;
    }

    // Preserves NOTHING.  Use on the band path, where every element is about
    // to be written; it skips the copy that resize() would do on a grow.
    void resizeUninitialized(std::size_t count)
    {
        if (count > _capacity) {
            T* fresh = allocate(count);
            deallocate(_data, _capacity);
            _data     = fresh;
            _capacity = count;
        }
        _size = count;
    }

    void assign(std::size_t count, T value)
    {
        resizeUninitialized(count);
        fillElements(_data, count, value);
    }

    // Size 0, capacity kept: the band loop reuses its buffers across bands and
    // across cooks, so this must not free.
    void clear() noexcept { _size = 0; }

    void release() noexcept
    {
        deallocate(_data, _capacity);
        _data     = nullptr;
        _size     = 0;
        _capacity = 0;
    }

    void swap(PodBuffer& other) noexcept
    {
        std::swap(_data, other._data);
        std::swap(_size, other._size);
        std::swap(_capacity, other._capacity);
    }

    // Amortised geometric growth for append-style builders (SampleSoA).
    void growForAppend(std::size_t needed)
    {
        if (needed <= _capacity)
            return;
        std::size_t next = (_capacity < 64) ? 64 : _capacity;
        while (next < needed) {
            const std::size_t doubled = next * 2;
            next = (doubled > next) ? doubled : needed;   // overflow-safe
        }
        reserve(next);
    }

private:
    // ----- THE M3 SEAM ------------------------------------------------------
    // These two functions, and nothing else in this class, know where the
    // memory comes from.  A CUDA build replaces their bodies (cudaMalloc /
    // cudaMallocManaged / cudaFree); everything above is allocator-agnostic.
    static void* allocateBytes(std::size_t bytes)
    {
        return ::operator new(bytes, std::align_val_t{kAlignment});
    }

    static void deallocateBytes(void* p, std::size_t bytes) noexcept
    {
        ::operator delete(p, bytes, std::align_val_t{kAlignment});
    }
    // ------------------------------------------------------------------------

    static T* allocate(std::size_t count)
    {
        if (count == 0)
            return nullptr;
        // Element-count * sizeof(T) overflow is a real possibility on a
        // corrupted band size; fail loudly rather than allocating a wrapped,
        // far-too-small buffer.
        if (count > (static_cast<std::size_t>(-1) / sizeof(T)))
            throw std::bad_alloc();
        return static_cast<T*>(allocateBytes(count * sizeof(T)));
    }

    static void deallocate(T* p, std::size_t count) noexcept
    {
        if (p == nullptr)
            return;
        deallocateBytes(static_cast<void*>(p), count * sizeof(T));
    }

    // Behind one function so M3 can turn it into a cudaMemcpy without touching
    // any call site.
    static void copyBytes(void* dst, const void* src, std::size_t bytes) noexcept
    {
        std::memcpy(dst, src, bytes);
    }

    // Likewise for assign()'s fill.  It is here, and not inlined into assign(),
    // for the same reason copyBytes() is: it DEREFERENCES the buffer from the
    // host.  Swapping allocateBytes() alone to a plain cudaMalloc would leave
    // this (and every caller of operator[]/begin()/end()) writing to device
    // memory from host code, so M3 either uses managed/pinned memory or
    // replaces this with a cudaMemset/fill kernel.  "Two functions know where
    // the memory comes from" is true of PROVENANCE; ADDRESSABILITY is assumed
    // by this function, copyBytes(), and the element accessors.
    static void fillElements(T* p, std::size_t count, T value) noexcept
    {
        for (std::size_t i = 0; i < count; ++i)
            p[i] = value;
    }

    T*          _data     = nullptr;
    std::size_t _size     = 0;
    std::size_t _capacity = 0;
};

// ---------------------------------------------------------------------------
// ChannelGroups — the SoA channel-group layout + the M2 radius-scale hook
//
// Channels are partitioned into groups that share a kernel.  v1 has exactly
// one group covering every channel with radiusScale 1.0 (makeSingleChannelGroup);
// the array exists because the design reference requires the hook to be present
// from v1 so M2's chromatic aberration is a data change, not a structural one.
//
// The hook is consumed via groupRadius(): the SoA stores ONE base radius per
// fragment and the scatter multiplies it by the group's scale when it asks the
// KernelSampler for a kernel.  Storing per-group radii per fragment instead
// would multiply the SoA's per-fragment cost by the group count for no v1
// benefit.
// ---------------------------------------------------------------------------
struct ChannelGroups {
    static constexpr int kMaxGroups = 8;

    int   groupCount                = 0;
    int   firstChannel[kMaxGroups]  = {};
    int   channelCount[kMaxGroups]  = {};
    float radiusScale[kMaxGroups]   = {};

    DEEPC_HD inline int count() const { return groupCount; }
};

inline ChannelGroups makeSingleChannelGroup(int channelCount)
{
    ChannelGroups g;
    g.groupCount       = 1;
    g.firstChannel[0]  = 0;
    g.channelCount[0]  = (channelCount > 0) ? channelCount : 0;
    g.radiusScale[0]   = 1.0f;
    return g;
}

// Base radius -> this group's radius.  Out-of-range groups and non-finite or
// non-positive scales degrade to the base radius (never to 0, which would
// silently sharpen a channel).
DEEPC_HD inline float groupRadius(const ChannelGroups& g, int group, float baseRadius)
{
    if (group < 0 || group >= g.groupCount)
        return baseRadius;
    const float s = g.radiusScale[group];
    if (!(s > 0.0f) || !(s < 1e6f))
        return baseRadius;
    return baseRadius * s;
}

// ---------------------------------------------------------------------------
// FragmentKind — which half of the COMPOSITION CONTRACT a fragment took
//
// This is the contract, made data.  DeepCDefocusMath.h's splitSpanAtBoundaries()
// documents that a volumetric sample is graded across depth by the SPAN SPLIT
// and assigned whole-weight with bucketOfContaining(), while a point sample is
// graded by the FRACTIONAL TWO-BUCKET assignment (bucketOf + fragmentDeposit) —
// and that doing both to one sample over-counts it by a measured +8.3% on alpha
// and premultiplied colour at parent alpha 0.9.
//
// flattenPixelToSoA() derives this once, from `zBack > zFront`, into a single
// const local that both branches store; emitFragment() then has exactly one
// if/else on it and no path that can reach both splits.
//
// LIMIT OF THAT GUARANTEE, because it is easy to over-read: the branch and the
// audit key off the SAME field.  A change that span-splits a sample and ALSO
// labels its pieces Point reintroduces the full double-count (measured by the
// reviewer at +8.97% on alpha and premultiplied colour at parent alpha 0.9,
// +4.09% at 0.5, +0.70% at 0.1) and checkCompositionContract() accepts it,
// because every such fragment is internally consistent.  What the auditor
// really certifies is "the deposit matches the label", not "the label matches
// the split the sample received".  The test that catches a mislabel is the
// parent-reconstruction identity — accumulate the fragments into bucket planes
// additively, composite front-to-back, and compare against the parent sample's
// own alpha and premultiplied colour — which M1.P3.T4 must carry.
// ---------------------------------------------------------------------------
enum class FragmentKind : std::uint8_t {
    Point      = 0,     // zBack <= zFront: bucketOf() + fragmentDeposit()
    Volumetric = 1      // zBack >  zFront: splitSpanAtBoundaries() + bucketOfContaining()
};

// Stack budget for splitSpanAtBoundaries(): K+2 parts at the knob's K maximum.
// 130 * sizeof(SpanSplitPart) == 2600 bytes — a per-sample stack array, never a
// heap allocation (milestone brief).
constexpr int kMaxSpanSplitParts = DepthBuckets::kMaxBoundaries + 1;

// Radius below which the scatter takes the sharp fast path (fragment
// composites into its own pixel's bucket instead of rasterising a disc).
// Defined here so the flatten, the scatter and the LUT's rMin contract all
// read the same number; the flatten itself does not branch on it.
constexpr float kSharpRadiusPx = 0.5f;

// ---------------------------------------------------------------------------
// FragmentRecord — one fragment as appended to the SoA
//
// A "fragment" is a post-tidy, post-span-split, post-pre-merge piece of one
// deep sample at one source pixel.  It is what the scatter rasterises.
// ---------------------------------------------------------------------------
struct FragmentRecord {
    int           x        = 0;         // source pixel, absolute image coords
    int           y        = 0;
    float         radius   = 0.0f;      // clamped CoC radius, X pixels, base group
    float         depth    = 0.0f;      // midpoint depth used for CoC + bucketing
    float         alpha    = 0.0f;      // this fragment's own alpha
    BucketDeposit deposit  = {};        // the two bucket deposits (see contract)
    BoundarySpan  boundary = {};        // for HoldoutVisibility::interpAtBucket()
    FragmentKind  kind     = FragmentKind::Point;
};

// ---------------------------------------------------------------------------
// SampleSoA — the flattened fragment stream
//
// Per-fragment ATTRIBUTES are stored one array each (structure of arrays), so
// the scatter's outer loop streams exactly the arrays it needs and M3 can
// upload them independently.
//
// Per-fragment CHANNEL VALUES are stored INTERLEAVED (`color[i*channelCount + c]`),
// which is deliberate and is the one departure from strict SoA: a channel value
// is consumed as a SCALAR (`dst[i] += w[i] * c`), never as a vector across
// fragments, so a planar layout would buy no vectorization — and a planar
// layout's stride is the fragment count, which is not known until the band is
// flattened, so appending would have to re-stride the whole buffer on every
// growth.  A fragment's channels are used together, so interleaving is also the
// cache-friendlier of the two.
//
// COORDINATES: x/y are absolute image pixel coordinates, not band-relative.
// The scatter subtracts the band origin.  Keeping them absolute means the SoA
// is independent of the band decomposition, which is what makes it directly
// testable and directly reusable by M1.P4.T1's re-banding.
//
// Both deposits are always present.  For a Volumetric fragment (and for a Point
// fragment sitting exactly on a bucket centre) the second deposit is
// (index0, alpha 0, colorScale 0), so the scatter's inner loop can deposit
// unconditionally and stay in bounds — see fragmentDeposit()'s note.
//
// TWO DIFFERENT (index, frac) PAIRS LIVE HERE, AND THEY ARE NOT INTERCHANGEABLE
// (milestone Decisions, 2026-07-26):
//   * bucketIndex0/1 + the deposit alphas come from bucketOf()/
//     bucketOfContaining(), which measure position between bucket CENTRES.
//     They are the scatter's plane assignment.
//   * boundaryIndex/boundaryFrac come from DepthBuckets::locateBoundary(),
//     which measures position between bucket BOUNDARIES.  They are what
//     HoldoutVisibility::interpAtBucket() must be fed (M1.P3.T3).  Feeding it
//     the bucketOf() fraction instead was measured to take max |vis − exact|
//     from 0.680 to 0.869.
// Both are precomputed here, once per fragment, so the scatter never has to
// choose — and never pays a second binary search per fragment.
// ---------------------------------------------------------------------------
struct SampleSoA {
    PodBuffer<std::int32_t> x;
    PodBuffer<std::int32_t> y;
    PodBuffer<float>        radius;
    PodBuffer<float>        depth;
    PodBuffer<float>        alpha;

    PodBuffer<std::int32_t> bucketIndex0;
    PodBuffer<std::int32_t> bucketIndex1;
    PodBuffer<float>        bucketAlpha0;
    PodBuffer<float>        bucketAlpha1;
    PodBuffer<float>        colorScale0;
    PodBuffer<float>        colorScale1;

    PodBuffer<std::int32_t> boundaryIndex;
    PodBuffer<float>        boundaryFrac;

    PodBuffer<std::uint8_t> kind;

    PodBuffer<float>        color;      // channelCount interleaved values/fragment

    int           channelCount = 0;
    ChannelGroups groups       = {};

    std::size_t fragmentCount() const { return radius.size(); }

    float*       colorOf(std::size_t i)
    {
        return color.data() + i * static_cast<std::size_t>(channelCount);
    }
    const float* colorOf(std::size_t i) const
    {
        return color.data() + i * static_cast<std::size_t>(channelCount);
    }

    // Total owned bytes — for the memory-limit knob's per-band budget, which
    // the design reference currently sizes from the bucket planes alone.
    std::size_t sizeBytes() const;

    // Size 0, capacity kept (band-to-band reuse).
    void clear();

    // Drops every allocation.
    void release();

    // Prepare for a band: sets the channel layout and empties the arrays.
    void begin(int channelCountIn, const ChannelGroups& groupsIn);

    // Capacity hint; appending works without it, this just avoids the growth
    // copies when the caller can estimate the band's fragment count.
    void reserveFragments(std::size_t count);

    // Appends one fragment.  `channels` must point at channelCount values
    // (premultiplied, already scaled by any span-split colorScale).
    void appendFragment(const FragmentRecord& f, const float* __restrict__ channels);
};

// ---------------------------------------------------------------------------
// FlattenParams — everything flattenPixelToSoA() needs that is not per-pixel
//
// `coc` must ALREADY be proxy-scaled: call applyProxyScale() on it once per
// _validate.  See that function for why max_radius in particular cannot be
// left unscaled here.
// ---------------------------------------------------------------------------
struct FlattenParams {
    CocParams coc = {};

    // Pre-merge (Perf > pre_merge / merge_tolerance).  The tidy pass is always
    // on and is not covered by this knob.
    bool  preMerge          = true;
    float mergeTolerancePx  = 0.25f;    // CoC-RADIUS pixels; see the .cpp

    // Focus > depth_is_ray_distance.  Needs the format height, which CocParams
    // does not carry (it only needs the width for mm->px).
    bool  depthIsRayDistance = false;
    float formatHeightPx     = 1080.0f;

    int           channelCount = 0;
    ChannelGroups groups       = {};
};

// ---------------------------------------------------------------------------
// FlattenStats — optional instrumentation, for the perf gate at M1.P4.T2
//
// Pass nullptr to skip.  All counters are cumulative across pixels so a band
// or a whole frame can share one instance.
// ---------------------------------------------------------------------------
struct FlattenStats {
    std::size_t pixels           = 0;   // pixels with at least one input sample
    std::size_t inputSamples     = 0;   // samples handed in
    std::size_t tidiedSamples    = 0;   // samples after tidyOverlapping()
    std::size_t splitParts       = 0;   // parts produced by the span split
    std::size_t stagedFragments  = 0;   // fragments before pre-merge
    std::size_t emittedFragments = 0;   // fragments appended to the SoA
    std::size_t maxSamplesInPixel = 0;
};

// ---------------------------------------------------------------------------
// FlattenScratch — caller-owned per-thread scratch
//
// One instance per band-computing thread, reused across every pixel of the
// band: the staging vectors keep their capacity (and their per-fragment channel
// vectors keep theirs), so nothing in THIS file allocates per pixel once warmed
// up.  The span-split part array is NOT here — it is a stack array in the
// per-sample path (kMaxSpanSplitParts, ~2.6KB at K=128), per the brief.
//
// The whole per-pixel path is not allocation-free, though, and the difference
// matters at 4K: deepc::tidyOverlapping() builds a fresh
// std::vector<SampleRecord> for its over-merge result on every call, so a pixel
// with 2+ samples costs exactly one malloc/free pair (measured: 0.00
// allocations/pixel at one sample, 1.00 at two and at twenty, and the same 1.00
// for tidyOverlapping() called alone).  That is one allocation PER PIXEL, not
// per sample, and it lives in shared code the milestone requires reusing — but
// it is ~12.7M malloc/free pairs per 4K frame and belongs on M1.P4.T2's perf
// list, not in a claim that the path allocates nothing.
// ---------------------------------------------------------------------------
struct FlattenScratch {
    // One staged fragment: a SampleRecord-shaped piece plus the grouping keys
    // the pre-merge needs.  Public because tests drive it directly.
    struct Staged {
        float              zFront = 0.0f;
        float              zBack  = 0.0f;
        float              alpha  = 0.0f;
        float              depth  = 0.0f;
        float              radius = 0.0f;
        int                bucket = 0;       // containing bucket, grouping key
        FragmentKind       kind   = FragmentKind::Point;
        std::vector<float> channels;
    };

    std::vector<Staged> staged;
    std::size_t         stagedCount = 0;
    std::vector<float>  mergeAccum;          // over-composite accumulator
};

// ---------------------------------------------------------------------------
// applyProxyScale — scale the pixel-unit knobs into proxy resolution
//
// The mm-denominated physical knobs are resolution-independent for free,
// because CocParams::_formatWidthPx is Nuke's CURRENT (proxy) format width, so
// coc_px already comes out in proxy pixels.  The PIXEL-unit knobs do not:
//
//   * max_radius — the radius CLAMP.  M1.P2.T2 used it only for the bbox pad,
//     where over-padding is harmless, so it was left unscaled there.  Here it
//     bounds the actual blur: leaving it at full-res value makes a proxy-0.5
//     render clamp at twice the radius the full-res render clamps at, so proxy
//     and full res disagree exactly where the clamp bites (the near field).
//   * size (Manual mode) — a blur radius in pixels at d = infinity.  Unscaled,
//     a proxy-0.5 render would blur by the same pixel count over half as many
//     pixels, i.e. twice as much of the image.
//
// proxyScale is (current format width / full-size format width), i.e. 1.0 at
// full res and 0.5 at proxy 0.5.  Non-finite or non-positive values are
// ignored (treated as 1.0) rather than collapsing the blur to nothing.
//
// Idempotency: this MUTATES the params, so call it exactly once per _validate,
// on freshly built params.  It re-derives the cached members.
// ---------------------------------------------------------------------------
void applyProxyScale(CocParams& p, float proxyScale);

// ---------------------------------------------------------------------------
// flattenPixelToSoA — one deep pixel -> zero or more SoA fragments
//
//   params      : lens/knob state (coc already proxy-scaled)
//   buckets     : the frame's K depth buckets (built once per cook)
//   x, y        : the source pixel's absolute image coordinates
//   samples     : THIS PIXEL'S samples.  Modified in place (sanitised, tidied,
//                 sorted) and reusable as scratch across pixels — pass the same
//                 vector every time so the per-sample channel vectors keep
//                 their capacity.
//   scratch     : per-thread scratch, see FlattenScratch
//   out         : SoA to append to; call out.begin() once per band first
//   stats       : optional, may be nullptr
//
// Pipeline, in order:
//   1. sanitise depths and alphas (NaN/inf depths would make std::sort's
//      comparator a non-strict-weak ordering, which is UB, before they ever
//      reached the CoC math)
//   2. optional ray-distance -> Z (per pixel: it depends on the pixel's radial
//      filmback offset)
//   3. deepc::tidyOverlapping()  — ALWAYS ON, correctness-required
//   4. per sample: point  -> bucketOf() + fragmentDeposit()
//                  volume -> splitSpanAtBoundaries() + bucketOfContaining()
//      (the COMPOSITION CONTRACT: one if/else, never both)
//   5. optional pre-merge of adjacent fragments within merge_tolerance
//   6. append to the SoA
// ---------------------------------------------------------------------------
void flattenPixelToSoA(const FlattenParams& params,
                       const DepthBuckets&  buckets,
                       int                  x,
                       int                  y,
                       std::vector<SampleRecord>& samples,
                       FlattenScratch&      scratch,
                       SampleSoA&           out,
                       FlattenStats*        stats);

// ---------------------------------------------------------------------------
// checkCompositionContract — runtime audit of the SoA's contract invariants
//
// Returns false (and, if `firstBadFragment` is non-null, the offending index)
// when any fragment violates one of:
//   * every field finite, alpha and colour scales in [0,1]
//   * bucket indices in [0, K-1], boundary index in [0, K-1], frac in [0,1]
//   * a Volumetric fragment has NO fractional spill (index1 == index0,
//     alpha1 == 0, colorScale1 == 0) — i.e. it was not additionally split by
//     bucketOf(), which is the +8.3% double-count bug
//   * the two deposits reconstruct the fragment's own alpha under `over`:
//     1 - (1 - a0)(1 - a1) == alpha, to float tolerance
//
// WHAT IT DOES NOT CATCH.  Every check above reads the fragment's own recorded
// `kind`, which is also what emitFragment() branched on, so this certifies
// internal consistency, not that the label describes the split the sample
// actually received.  A span-split piece mislabelled Point passes cleanly while
// carrying the full +8.97% double-count; see FragmentKind above for the
// parent-reconstruction test that does catch it.  Verified by reviewer
// mutation: rewriting emitFragment()'s branch to use bucketOf() for both kinds
// IS rejected here (first bad fragment 0, at every alpha tried); relabelling
// the span-split pieces Point is NOT.
//
// Cheap enough (O(fragments), no allocation) to call from a debug build or a
// test; not called from the production path.
// ---------------------------------------------------------------------------
bool checkCompositionContract(const SampleSoA& soa,
                              const DepthBuckets& buckets,
                              std::size_t* firstBadFragment = nullptr);

} // namespace deepc

#endif // DEEPC_DEFOCUS_SCATTER_H
