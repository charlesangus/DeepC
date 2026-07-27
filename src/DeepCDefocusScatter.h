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
//  Contents (M1.P3.T2):
//
//    - BucketPlanes  : the band's K x (C + 2) accumulation planes (colour,
//                      alpha, and the `sum of w*vis` COVERAGE plane).
//    - HoldoutSoA    : non-owning view of M1.P3.T3's per-dest-pixel boundary
//                      transmittance LUT.  Absent/unconnected holdout is an
//                      empty view and costs nothing.
//    - scatterBandCPU() : the scatter core proper — fragments -> planes.
//    - resolveBandCPU() : saturate-down, then combine the planes into the
//                      band's flat output by one of the two candidate
//                      bucket-composite rules (see BucketCombine).
//
//  The per-fragment / per-span / per-pixel BODIES of all of the above live in
//  this header marked DEEPC_HD; only the loop drivers and the allocations are
//  in the .cpp.  That split IS the M3 CUDA seam: a .cu translation unit
//  includes this header unchanged and replaces the drivers.
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

// KernelSampler / KernelView / RowSpan — the scatter consumes the kernel seam's
// precomputed contiguous row spans.  Header-only and NDK-free, like this file.
#include "DeepCDefocusKernel.h"

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

// ===========================================================================
//
//  THE SCATTER CORE (M1.P3.T2)
//
//  Pipeline for one band, in order:
//
//    planes.allocate(K, C, W*B)          // once, then zero() per band
//    scatterBandCPU(...)                 // fragments -> K*(C+2) planes
//    resolveBandCPU(...)                 // saturate down, then combine
//
//  scatterBandCPU() only ACCUMULATES, so a band may be scattered from several
//  SoA chunks (the design fetches source rows band +/- ceil(maxRadius*aspect)
//  and may flatten them in pieces).  resolveBandCPU() is what must run exactly
//  once at the end, and it ALWAYS runs the saturate-down pass — that pass is
//  load-bearing for ordinary fog, not a safety net (milestone Decisions,
//  2026-07-26: within-bucket additive accumulation over-counts same-pixel
//  fragments by +33.3% / +71.4% / +113.3% of alpha at 2 / 3 / 4 disjoint fog
//  spans sharing a bucket, and `pre_merge` does not mitigate it), so it is
//  deliberately NOT behind a flag.
//
// ===========================================================================

// ---------------------------------------------------------------------------
// BucketCombine — the two candidate bucket-composite rules
//
// BOTH ARE BUILT ON PURPOSE and the choice is made from rendered pixels at
// M1.P3.T5, not from identities (milestone Decisions, 2026-07-26, "Bucket-
// composite alpha deficit"); the loser is deleted before the milestone gate.
// It is a runtime enum, not a preprocessor flag, precisely so T5 can render
// both from one build.
//
//   FrontToBackOver    the design reference's original rule: plain
//                      front-to-back `over` of the K planes, ignoring the
//                      coverage plane.  Conventional, and what classical
//                      layered DOF does.  Its known failure: distinct opaque
//                      fragments whose disc weights sum to exactly 1 at a
//                      destination pixel but land in DIFFERENT buckets
//                      composite to 1 - prod(1 - W_k) < 1 — a measured 25.0%
//                      alpha deficit across 2 buckets, 31.6% / 4, 34.4% / 8,
//                      35.6% / 16.  It worsens with K, so the K knob is not a
//                      mitigation, and it fails validation scenes (c) and (g)
//                      as written.  M1.P3.T2's review found a SECOND, opposite
//                      failure it cannot avoid either: because pixel-integrated
//                      alpha is linear in kernel weight while `over` is not,
//                      one fragment split across two buckets over-composites to
//                      more than it deposited wherever its kernel weight is
//                      below 1 — measured +93.8% worst case over 3000 random
//                      (alpha, fraction, radius) triples, i.e. an isolated
//                      opaque bokeh at DOUBLE energy (band alpha sum 1.988 for
//                      one alpha-1 fragment), and a defocused opaque edge's
//                      alpha/colour ramp inflated from 0.437 to 0.683.  This
//                      candidate has no coverage plane to correct it with.
//
//   CoveragePartition  front-to-back with occlusion driven off the `sum of
//                      w*vis` COVERAGE plane: coverage that still fits inside
//                      the destination pixel's unclaimed area is ADDITIVE (it
//                      is disjoint from everything already composited, so
//                      nothing occludes it) and only the excess is
//                      `over`-attenuated.  See
//                      compositePixelCoveragePartition() for the model, the
//                      reduction proofs and the measured numbers.  No direct
//                      published precedent, hence the empirical bake-off.
//
// The default is CoveragePartition: FrontToBackOver is *known* to fail scene
// (c)'s alpha == 1 identity AND to double an isolated bokeh's energy (above),
// so defaulting to it would ship a known-failing default while T5 runs.  That
// default is provisional, not the decision — M1.P3.T5 must render BOTH
// explicitly rather than relying on whatever this enum initialises to, and
// M1.P3.T4's tests must set `combine` explicitly in every case for the same
// reason.
// ---------------------------------------------------------------------------
enum class BucketCombine : std::uint8_t {
    FrontToBackOver   = 0,
    CoveragePartition = 1
};

// ---------------------------------------------------------------------------
// BucketPlaneView — POD, non-owning view of one band's accumulation planes
//
// The layout is DeepCDefocusMath.h's ("Bucket plane layout"), extended with
// the third plane the design reference requires:
//
//   colour  : color [(k * channelCount + c) * pixelCount + i]
//   alpha   : alpha [k * pixelCount + i]
//   coverage: weight[k * pixelCount + i]
//
// with k the bucket (0 = nearest the camera), c the channel, i the
// destination pixel within the band (i = y * width + x, band-relative).
//
// THE THIRD PLANE IS `sum of w*vis`, PURE KERNEL COVERAGE, INDEPENDENT OF
// ALPHA (milestone Decisions, 2026-07-26 — the design reference's original
// `sum of w*alpha*vis` was character-for-character the alpha plane and is a
// typo).  It is what distinguishes bucketing-induced alpha loss from the
// honest coverage deficit of validation scene (i), and it is what
// CoveragePartition composites against.
//
// Pointers rather than PodBuffer so the DEEPC_HD bodies below never name a
// host container; BucketPlanes::view() produces one.
// ---------------------------------------------------------------------------
struct BucketPlaneView {
    float*         color       = nullptr;
    float*         alpha       = nullptr;
    float*         weight      = nullptr;
    int            bucketCount = 0;
    int            channelCount = 0;
    int            width       = 0;
    int            height      = 0;
    std::ptrdiff_t pixelCount  = 0;

    DEEPC_HD inline bool valid() const
    {
        return alpha != nullptr && weight != nullptr
            && (channelCount == 0 || color != nullptr)
            && bucketCount > 0 && channelCount >= 0 && width > 0 && height > 0
            && pixelCount == static_cast<std::ptrdiff_t>(width) * height;
    }
};

// ---------------------------------------------------------------------------
// BucketPlanes — the owning form: one band's K x (C + 2) planes
//
// One instance per band-computing thread, reused across bands (allocate()
// once, zero() per band) — the design's memory-limit knob caps how many of
// these can be in flight at a time, which is why bytesForBand() lives here.
// ---------------------------------------------------------------------------
struct BucketPlanes {
    PodBuffer<float> color;     // bucketCount * channelCount * pixelCount
    PodBuffer<float> alpha;     // bucketCount * pixelCount
    PodBuffer<float> weight;    // bucketCount * pixelCount

    int            bucketCount  = 0;
    int            channelCount = 0;
    int            width        = 0;
    int            height       = 0;
    std::ptrdiff_t pixelCount   = 0;

    // Sizes the three buffers and ZEROES them.  Safe to call repeatedly with
    // the same geometry (PodBuffer keeps its capacity), which is the band
    // loop's normal path.
    void allocate(int bucketCountIn, int channelCountIn, int widthIn, int heightIn);

    // Zero-fill for the next band, keeping the allocation.
    void zero();

    void release();

    std::size_t sizeBytes() const;

    BucketPlaneView view();

    // The design reference's per-band scratch formula, K*W*B*(C+2)*4 bytes,
    // in one place so the memory-limit knob and the code cannot drift apart.
    //
    // CLAMP AT THE CALL SITE.  Knob ranges are soft (milestone Decisions,
    // 2026-07-26), so `depth_layers` must be clamped to [4, 128] and the band
    // height derived from a CLAMPED `max_radius` before this is evaluated —
    // otherwise the budget is computed from a number the user typed rather
    // than from the one the node will use.  This function clamps nothing: it
    // is the formula, not the policy.
    //
    // NOTE this counts ONLY the planes.  M1.P3.T1 measured the SoA fragment
    // buffers at 113 B/fragment resident, ~2.4GB for a 4K band at 20spp,
    // which dwarfs them; budget on the combined total (milestone Decisions).
    static std::size_t bytesForBand(int bucketCount, int channelCount,
                                    int width, int height)
    {
        const std::size_t k = (bucketCount > 0) ? static_cast<std::size_t>(bucketCount) : 0;
        const std::size_t c = (channelCount > 0) ? static_cast<std::size_t>(channelCount) : 0;
        const std::size_t w = (width > 0) ? static_cast<std::size_t>(width) : 0;
        const std::size_t h = (height > 0) ? static_cast<std::size_t>(height) : 0;
        return k * w * h * (c + 2) * sizeof(float);
    }
};

// ---------------------------------------------------------------------------
// HoldoutSoA — THE M1.P3.T3 SEAM.  Per-dest-pixel boundary transmittance LUT.
//
// WHAT T2 (this task) DOES: takes `vis` as an input and multiplies it into
// every deposit, before the fragment enters any accumulation structure — which
// is the whole reason the design does not need full fragment sorting.  An
// absent holdout is an empty view: enabled() is false, the scatter takes a
// separate loop with no per-pixel work at all, and vis is identically 1 at
// ZERO cost (not "vis = 1.0f multiplied in").
//
// WHAT T3 MUST DO: fill `boundaryT` per destination pixel with
// HoldoutVisibility::build(), which writes exactly boundaryCount contiguous
// floats — i.e. `build(..., holdout.pixelLut(i))` for pixel i, using
// DepthBuckets::boundaries() and the holdout input's samples at that pixel.
// The layout is therefore PIXEL-MAJOR:
//
//     boundaryT[i * boundaryCount + b]
//
// which is what build() emits and what HoldoutVisibility::interpAtBucket()
// consumes, so T3's build and this file's consumption need no re-striding and
// no reimplementation of either.  (Boundary-major would let the scatter read
// two contiguous rows per span, but interpAtBucket() takes no stride and the
// vis path is a log/exp per fragment-pixel anyway — it is not the loop that
// vectorizes.)
//
// THE INTERPOLATION IS FED FROM locateBoundary(), NOT bucketOf() — position
// between BOUNDARIES, not between bucket CENTRES (milestone Decisions,
// 2026-07-26: feeding it the bucketOf fraction takes max |vis - exact| from
// 0.680 to 0.869).  SampleSoA already carries the right pair as
// boundaryIndex/boundaryFrac, precomputed by the flatten, and this file uses
// those and never the bucketIndex/bucketAlpha pair.
//
// NON-OWNING on purpose: T3 owns the storage (it belongs with the rest of the
// band's per-thread scratch and its lifetime is the band's), and a POD view is
// what a device kernel can take by value.  boundaryCount must be
// DepthBuckets::boundaryCount() == K+1; a mismatch is treated as "disabled"
// rather than read out of bounds.
// ---------------------------------------------------------------------------
struct HoldoutSoA {
    const float*   boundaryT     = nullptr;
    int            boundaryCount = 0;       // K + 1
    std::ptrdiff_t pixelCount    = 0;       // band pixels covered by the LUT

    DEEPC_HD inline bool enabled() const
    {
        return boundaryT != nullptr && boundaryCount > 1 && pixelCount > 0;
    }

    DEEPC_HD inline const float* pixelLut(std::ptrdiff_t pixel) const
    {
        return boundaryT + pixel * static_cast<std::ptrdiff_t>(boundaryCount);
    }
};

// ---------------------------------------------------------------------------
// ScatterParams — everything scatterBandCPU() needs that is not per-fragment
//
// Band geometry is expressed as the band's ORIGIN in absolute image
// coordinates plus its size; SampleSoA stores absolute source coordinates (so
// it is independent of the band decomposition), and the scatter subtracts the
// origin.  Fragments outside the band are NOT an error and are not culled:
// their discs are exactly how a band gets the energy scattered in from the
// `band +/- ceil(maxRadius*aspect)` rows the design fetches around it.  Every
// span is clipped to the band.
// ---------------------------------------------------------------------------
struct ScatterParams {
    int bandX      = 0;         // band origin, absolute image coordinates
    int bandY      = 0;
    int bandWidth  = 0;
    int bandHeight = 0;

    // Radius below which a fragment takes the sharp fast path: it deposits
    // weight 1 into its OWN pixel's bucket instead of rasterising a disc.
    // This is also what keeps DiscKernelLUT's documented caller contract
    // ("never reaches the sampler for radius < 0.5px") true from this side.
    float sharpRadiusPx = kSharpRadiusPx;

    // Which of the two candidate bucket composites resolveBandCPU() runs.
    BucketCombine combine = BucketCombine::CoveragePartition;
};

// ---------------------------------------------------------------------------
// ScatterStats — optional instrumentation (M1.P4.T2's perf gate).  Cumulative.
// ---------------------------------------------------------------------------
struct ScatterStats {
    std::size_t fragments      = 0;   // fragments examined
    std::size_t sharpFragments = 0;   // took the sharp fast path
    std::size_t culled         = 0;   // deposited nothing (zero alpha+colour,
                                      // bad bucket, or wholly outside the band)
    std::size_t rowSpans       = 0;   // clipped kernel rows rasterised
    std::size_t pixelDeposits  = 0;   // total span pixels touched
};

// ---------------------------------------------------------------------------
// ScatterScratch — caller-owned per-thread scratch
//
// Holds exactly one row of effective weights (kernel weight * holdout vis).
// It is only ever touched when a holdout is connected; with no holdout the
// scatter reads the kernel's own weight row directly and this stays empty.
// ---------------------------------------------------------------------------
struct ScatterScratch {
    PodBuffer<float> rowWeights;

    void ensureRow(std::size_t count)
    {
        if (rowWeights.size() < count)
            rowWeights.resizeUninitialized(count);
    }

    void release() { rowWeights.release(); }
};

// ---------------------------------------------------------------------------
// ScatterFragment — one fragment's deposit, as the per-span body sees it
//
// POD, by value, no SoA indexing: this is the argument bundle a CUDA thread
// would build for itself in M3.  It is ONE CHANNEL GROUP's worth of one
// fragment, which is why it carries a channel range rather than a count.
//
// bucket1 == bucket0 means "no second deposit" — the same convention
// fragmentDeposit() produces for frac == 0 and for every bucketOfContaining()
// (i.e. span-split) assignment, so the caller never has to branch on
// FragmentKind here.  THE COMPOSITION CONTRACT IS NOT RE-DECIDED IN THE
// SCATTER: the flatten (M1.P3.T1) already chose bucketOf() vs
// bucketOfContaining() per fragment and this file honours the labels it is
// given.  Doing both is a measured +8.29% double-count.
//
// depositCoverage exists because the alpha and coverage planes are per
// fragment and NOT per channel group: with several channel groups (M2's
// chromatic aberration) each group rasterises its own radius, but only one of
// them may write the alpha and coverage planes or they would be counted once
// per group.
//
// NOTE the coverage plane is written by the FIRST DEPOSIT ONLY, even when the
// fragment straddles two buckets — see scatterSpanBothBuckets().
// ---------------------------------------------------------------------------
struct ScatterFragment {
    int   destX = 0;            // band-relative destination centre
    int   destY = 0;

    int   bucket0 = 0;
    int   bucket1 = 0;          // == bucket0 when there is no second deposit
    float alpha0  = 0.0f;
    float alpha1  = 0.0f;
    float colorScale0 = 0.0f;
    float colorScale1 = 0.0f;

    // locateBoundary()'s pair — position between BOUNDARIES, for the holdout
    // LUT.  NOT the bucketOf() pair above.
    int   boundaryIndex = 0;
    float boundaryFrac  = 0.0f;

    const float* color = nullptr;   // the fragment's interleaved channels
    int   firstChannel = 0;         // this group's channel range within them
    int   groupChannels = 0;

    bool  depositCoverage = false;  // alpha + coverage planes: one group only
};

// ---------------------------------------------------------------------------
// depositRowSpan — THE INNER LOOP.  One contiguous clipped row span, one
// bucket, one channel group.
//
// Every loop here is a flat `dst[i] += w[i] * scalar` over a contiguous span
// with `__restrict__` on both pointers and no disc test, no branch and no
// gather inside — which is the shape GCC auto-vectorizes at -O3 (the per-
// target `-mavx2 -mfma` lands at M1.P5.T1; `-fopt-info-vec` verification is
// part of the Phase 1.4 gate, not an assumption).  No intrinsics, no SIMD
// library, no OpenMP, by the board-level Context.
//
// `w` is the EFFECTIVE weight — kernel weight already multiplied by holdout
// visibility — so the holdout costs this function nothing.
//
// The zero tests are worth their branch: they skip a whole plane pass for a
// black channel (common: mattes, and any fragment that saturated to nothing),
// and they are per span, not per pixel.  A NaN value fails `== 0.0f` and is
// still deposited, so poison stays visible rather than being silently dropped.
//
// `depositAlpha` gates the alpha plane (one channel group only) and
// `depositWeight` the coverage plane INSIDE it (one channel group AND the
// fragment's first bucket only) — the two are separate because a fragment's
// alpha lands in both of its buckets while its coverage lands in one.
// ---------------------------------------------------------------------------
DEEPC_HD inline void depositRowSpan(const BucketPlaneView& planes,
                                    int                    bucket,
                                    std::ptrdiff_t         dstOffset,
                                    const float* __restrict__ w,
                                    int                    count,
                                    const float* __restrict__ color,
                                    int                    firstChannel,
                                    int                    groupChannels,
                                    float                  bucketAlpha,
                                    float                  colorScale,
                                    bool                   depositAlpha,
                                    bool                   depositWeight)
{
    const std::ptrdiff_t planeBase =
        static_cast<std::ptrdiff_t>(bucket) * planes.pixelCount + dstOffset;

    if (depositAlpha) {
        if (bucketAlpha != 0.0f) {
            float* __restrict__ ap = planes.alpha + planeBase;
            for (int i = 0; i < count; ++i)
                ap[i] += w[i] * bucketAlpha;
        }

        // The coverage plane is alpha-INDEPENDENT: it accumulates w*vis
        // itself, so a fully transparent fragment still reports the pixel area
        // its kernel covers.  That is what makes an honest coverage deficit
        // (validation scene (i)) distinguishable from a bucketing artefact.
        if (depositWeight) {
            float* __restrict__ cp = planes.weight + planeBase;
            for (int i = 0; i < count; ++i)
                cp[i] += w[i];
        }
    }

    if (colorScale == 0.0f)
        return;

    for (int c = firstChannel; c < firstChannel + groupChannels; ++c) {
        const float v = color[c] * colorScale;
        if (v == 0.0f)
            continue;
        float* __restrict__ dst = planes.color
            + (static_cast<std::ptrdiff_t>(bucket) * planes.channelCount + c)
                  * planes.pixelCount
            + dstOffset;
        for (int i = 0; i < count; ++i)
            dst[i] += w[i] * v;
    }
}

// ---------------------------------------------------------------------------
// scatterSpanBothBuckets — one clipped span into the fragment's two deposits
//
// Split out from scatterFragmentSpans() so the sharp fast path (a one-pixel
// "span") and the disc path share exactly one deposit body.
//
// THE COVERAGE PLANE TAKES THE FRAGMENT'S `w*vis` EXACTLY ONCE, into the
// NEARER of the two buckets, even though BOTH buckets receive alpha and
// colour.  This is not a rounding choice, it is what the plane means
// (milestone Decisions, 2026-07-26: the third plane is `sum of w*vis`, pure
// kernel coverage).  A fractionally split fragment is ONE surface seen as two
// co-located depth layers — the transmittance split exists precisely so that
// `over`-compositing them reproduces the original — so it covers its kernel's
// area once, not twice.  Depositing it into both planes makes a pixel with an
// honest 60% coverage deficit (validation scene (i)) report 120% coverage,
// which is exactly the distinction the plane exists to preserve, and
// CoveragePartition then reads the rear deposit as landing on fresh, unclaimed
// pixel area: measured by the M1.P3.T2 reviewer, an isolated opaque fragment's
// bokeh came out at DOUBLE energy (band alpha sum 2.000 against an honest
// 1.000) and a defocused opaque edge's alpha ramp was doubled and clipped
// (0.437 -> 0.874, 0.563 -> 1.000) — i.e. the specified honest alpha dip was
// filled in with fabricated colour.  With the single deposit the same cases
// reconstruct the unbucketed additive scatter EXACTLY, at alpha 1 and at fog
// alphas alike.
//
// The rear deposit therefore carries alpha and colour with NO coverage, and
// compositePixelCoveragePartition() recognises that (alpha in excess of a
// bucket's own coverage is a co-located layer that claims no new area).
// ---------------------------------------------------------------------------
DEEPC_HD inline void scatterSpanBothBuckets(const BucketPlaneView& planes,
                                            const ScatterFragment& frag,
                                            std::ptrdiff_t         dstOffset,
                                            const float* __restrict__ w,
                                            int                    count)
{
    depositRowSpan(planes, frag.bucket0, dstOffset, w, count,
                   frag.color, frag.firstChannel, frag.groupChannels,
                   frag.alpha0, frag.colorScale0,
                   frag.depositCoverage, frag.depositCoverage);

    if (frag.bucket1 != frag.bucket0) {
        depositRowSpan(planes, frag.bucket1, dstOffset, w, count,
                       frag.color, frag.firstChannel, frag.groupChannels,
                       frag.alpha1, frag.colorScale1,
                       frag.depositCoverage, false);
    }
}

// ---------------------------------------------------------------------------
// scatterFragmentSpans — one fragment's whole disc.  THE PER-FRAGMENT BODY.
//
// This is the function a CUDA thread runs in M3 (one thread per fragment per
// channel group); the .cpp holds only the loop over fragments, the virtual
// KernelSampler call and the allocations, all of which M3 replaces.
//
//   rowScratch : at least (2*kv.radiusX + 1) floats, and REQUIRED only when
//                holdout.enabled(); pass nullptr otherwise.  With no holdout
//                the kernel's own weight row is used directly — the vis == 1
//                short-circuit is a different loop, not a multiply by one.
//
// Returns the number of destination pixels deposited into (per group), for
// ScatterStats.
// ---------------------------------------------------------------------------
DEEPC_HD inline std::size_t scatterFragmentSpans(const BucketPlaneView& planes,
                                                 const HoldoutSoA&      holdout,
                                                 const KernelView&      kv,
                                                 const ScatterFragment& frag,
                                                 float* __restrict__    rowScratch,
                                                 std::size_t*           rowSpansOut)
{
    std::size_t touched = 0;
    std::size_t rows    = 0;

    const bool  useHoldout = holdout.enabled();
    const int   bIndex     = frag.boundaryIndex;
    const float bFrac      = frag.boundaryFrac;

    for (int row = 0; row < kv.rowCount; ++row) {
        const RowSpan& span = kv.row(row);
        if (span.empty())
            continue;

        const int dy = frag.destY + kv.rowY(row);
        if (dy < 0 || dy >= planes.height)
            continue;

        int xs = frag.destX + span.xStart;
        int xe = frag.destX + span.xEnd;

        int skip = 0;
        if (xs < 0) {
            skip = -xs;
            xs   = 0;
        }
        if (xe >= planes.width)
            xe = planes.width - 1;
        if (xe < xs)
            continue;

        const int count = xe - xs + 1;
        const std::ptrdiff_t dstOffset =
            static_cast<std::ptrdiff_t>(dy) * planes.width + xs;

        // Deliberately NOT __restrict__ here: it is reassigned below, and the
        // qualifier that matters is the one on depositRowSpan's parameter,
        // which is what lets the deposit loops vectorize.
        const float* w = kv.rowWeights(row) + skip;

        if (useHoldout) {
            // Holdout visibility is multiplied in HERE, before the fragment
            // enters any accumulation structure — that is what makes the whole
            // depth-bucketed scatter work without sorting fragments, and it is
            // why a defocused foreground blooms over a held-out element with
            // pixel-sharp edges (visibility is never blurred).
            //
            // Folding it into the weights once per span keeps the C channel
            // passes below free of it.  No early-out on vis == 0 inside the
            // span: that would break the contiguity the deposit loops depend
            // on, and the deposit of a zero weight is a multiply-add of zero.
            for (int i = 0; i < count; ++i) {
                const float vis = HoldoutVisibility::interpAtBucket(
                    holdout.pixelLut(dstOffset + i),
                    holdout.boundaryCount, bIndex, bFrac);
                rowScratch[i] = w[i] * vis;
            }
            w = rowScratch;
        }

        scatterSpanBothBuckets(planes, frag, dstOffset, w, count);

        touched += static_cast<std::size_t>(count);
        ++rows;
    }

    if (rowSpansOut != nullptr)
        *rowSpansOut += rows;
    return touched;
}

// ---------------------------------------------------------------------------
// scatterFragmentSharp — the sharp fast path.  THE OTHER PER-FRAGMENT BODY.
//
// radius < sharpRadiusPx (0.5px): the fragment composites into its OWN
// pixel's bucket with weight 1 instead of rasterising a disc.  With the tidy
// pre-pass in front of it this is what makes size-0 output a flatten of the
// input rather than an approximation of one.
//
// Returns 1 if it deposited, 0 if the fragment's own pixel is outside the band.
// ---------------------------------------------------------------------------
DEEPC_HD inline std::size_t scatterFragmentSharp(const BucketPlaneView& planes,
                                                 const HoldoutSoA&      holdout,
                                                 const ScatterFragment& frag)
{
    if (frag.destX < 0 || frag.destX >= planes.width ||
        frag.destY < 0 || frag.destY >= planes.height)
        return 0;

    const std::ptrdiff_t dstOffset =
        static_cast<std::ptrdiff_t>(frag.destY) * planes.width + frag.destX;

    float w = 1.0f;
    if (holdout.enabled()) {
        w = HoldoutVisibility::interpAtBucket(holdout.pixelLut(dstOffset),
                                              holdout.boundaryCount,
                                              frag.boundaryIndex,
                                              frag.boundaryFrac);
    }

    scatterSpanBothBuckets(planes, frag, dstOffset, &w, 1);
    return 1;
}

// ---------------------------------------------------------------------------
// compositePixelCoveragePartition — CANDIDATE 2.  THE PER-PIXEL BODY.
//
// Same pointer conventions as DeepCDefocusMath.h's
// compositePixelFrontToBack() (pointers pre-offset to their pixel, everything
// else derived from pixelCount) plus the coverage plane, so the two candidates
// are drop-in alternatives for each other.
//
// THE MODEL.  Front-to-back over a pixel that is treated as a unit AREA, not
// as a single point sample:
//
//   freeArea : the share of the pixel no bucket has claimed yet (starts 1)
//   tClaimed : the AREA-WEIGHTED mean transmittance of the claimed share
//
// For bucket k with coverage C_k = sum(w*vis) (clamped into [0,1]) and alpha
// A_k, its per-unit-area opacity is a_k = A_k / C_k, and its coverage splits:
//
//   fit    = min(C_k, freeArea)   lands on area NOTHING in front of it covers,
//                                 so it is ADDITIVE — unattenuated
//   excess = C_k - fit            necessarily overlaps the claimed share, so
//                                 it is `over`-attenuated by tClaimed
//
//   accAlpha += fit*a_k                      + (excess/C_k)*A_k*tClaimed
//   accColor += (fit/C_k)*Colour_k           + (excess/C_k)*Colour_k*tClaimed
//   tClaimed  = area-weighted merge of the old claimed share with the newly
//              claimed area (whose transmittance is 1 - a_k), then attenuated
//              by the excess spread back over it
//
// PLUS ONE TERM THE AREA MODEL ALONE DOES NOT COVER — alpha in EXCESS of the
// bucket's own coverage (A_k > C_k).  That is the rear half of a fractionally
// split fragment: scatterSpanBothBuckets() deposits the fragment's `w*vis`
// into the NEARER bucket's coverage plane only (see there for why, and for the
// measured 2x energy error of depositing it twice), so the far deposit arrives
// as alpha and colour with no coverage of its own.  It is CO-LOCATED with area
// its front half already claimed one bucket in front of it, so it claims no
// new area and is `over`-attenuated by tClaimed:
//
//   aCov = min(A_k, C_k)   the share the bucket's own coverage accounts for
//   aRes = A_k - aCov      the co-located residual
//   accAlpha += min(aRes, claimedArea) * tClaimed;  tClaimed *= 1 - aRes/claimed
//
// Colour follows alpha: the residual takes the aRes/A_k share of Colour_k.
//
// WHY IT REDUCES CORRECTLY (all verified numerically, end to end through
// scatterBandCPU(), by the M1.P3.T2 reviewer — see the driver figures quoted
// in each bullet):
//
//   * receding opaque plane / flat opaque field — coverages sum to exactly 1
//     with a_k == 1, so every bucket is pure `fit` and the alphas simply ADD
//     to exactly 1.0 (measured 0.999999 on a checkerboard of two buckets at
//     K=4).  Plain `over` gives 0.750 on the same input.
//   * two 50% fog layers — the first fills freeArea, so the second is pure
//     `excess` and the result is 0.75, i.e. EXACTLY plain `over`.  Three
//     layers give exactly 0.875.  Dense scenes are plain `over`, identically,
//     not approximately.
//   * ONE fragment, split across two bucket centres, at ANY kernel weight —
//     reconstructs the unbucketed additive scatter exactly: band alpha sum
//     1.0000 for an opaque fragment and 0.5000 for an alpha-0.5 one, against
//     2.0000 / 0.5858 before the residual term existed.
//   * validation scene (i)'s honest coverage hole — total coverage < 1 means
//     everything is `fit` and the answer is the (deficient) coverage itself.
//     NOTHING here ever scales alpha UP.
//   * over-covered bucket (C_k > 1, i.e. the case the saturate-down pass
//     exists for) — C_k is clamped to 1 at use, which is what keeps
//     a_k = A_k/C_k <= 1 after saturation has pulled A_k down to 1.
//
// KNOWN RESIDUAL: a VOLUMETRIC sample split at the bucket boundaries emits one
// independent fragment per part, each depositing its own coverage into its own
// bucket, so the parts of one slab claim the same pixel area once per bucket.
// At full kernel coverage that is exact (freeArea is consumed by the first
// part and the rest are pure `over`, which the transmittance split
// reconstructs); at PARTIAL coverage it over-counts the same way the point
// split used to — measured 0.72 against an honest 0.54 for a 4-part alpha-0.9
// slab at 60% coverage (plain `over` gives 0.70 on the same planes, so this is
// not specific to this candidate).  Fixing it needs the flatten to mark which
// part of a split parent carries the coverage; see the review notes for
// M1.P3.T4/T5.
//
// A bucket with neither coverage nor alpha is SKIPPED.  A bucket with coverage
// but no alpha still contributes its colour and still claims area.
// ---------------------------------------------------------------------------
DEEPC_HD inline void compositePixelCoveragePartition(
    const float* __restrict__ bucketColor,
    const float* __restrict__ bucketAlpha,
    const float* __restrict__ bucketWeight,
    int                       bucketCount,
    int                       channelCount,
    std::ptrdiff_t            pixelCount,
    float* __restrict__       outColor,
    float* __restrict__       outAlpha)
{
    for (int c = 0; c < channelCount; ++c)
        outColor[static_cast<std::ptrdiff_t>(c) * pixelCount] = 0.0f;

    float freeArea = 1.0f;
    float tClaimed = 1.0f;
    float accAlpha = 0.0f;

    for (int k = 0; k < bucketCount; ++k) {
        const std::ptrdiff_t ko = static_cast<std::ptrdiff_t>(k) * pixelCount;

        const float cov = clampf(bucketWeight[ko], 0.0f, 1.0f);
        const float a   = clampf(bucketAlpha[ko], 0.0f, 1.0f);
        if (!(cov > 0.0f) && !(a > 0.0f))   // empty bucket (also rejects NaN)
            continue;

        const float* __restrict__ src = bucketColor
            + static_cast<std::ptrdiff_t>(k) * channelCount * pixelCount;

        // Split the bucket's alpha into the share its own coverage accounts
        // for and the co-located residual (a fractionally split fragment's
        // rear deposit, whose coverage went to the bucket in front of it).
        // Colour follows alpha, EXCEPT for a zero-alpha bucket, whose colour
        // all follows the coverage — an emissive or holdout-zeroed fragment
        // must not have its colour dropped.
        const float aCov     = (a < cov) ? a : cov;
        const float aRes     = a - aCov;
        const float resShare = (a > 0.0f) ? (aRes / a) : 0.0f;
        const float covShare = 1.0f - resShare;

        if (cov > 0.0f) {
            const float local  = aCov / cov;            // <= 1 by construction
            const float fit    = (cov < freeArea) ? cov : freeArea;
            const float excess = cov - fit;

            // ---- the share that fits in still-unclaimed area: ADDITIVE ----
            if (fit > 0.0f) {
                const float f          = (fit / cov) * covShare;
                const float claimedOld = 1.0f - freeArea;
                const float claimedNew = claimedOld + fit;

                accAlpha += fit * local;                // == aCov * fit/cov
                for (int c = 0; c < channelCount; ++c) {
                    const std::ptrdiff_t o = static_cast<std::ptrdiff_t>(c) * pixelCount;
                    outColor[o] += f * src[o];
                }

                tClaimed = (claimedOld * tClaimed + fit * (1.0f - local)) / claimedNew;
                freeArea -= fit;
            }

            // ---- the excess: `over`-attenuated by the claimed share -------
            // excess > 0 implies fit consumed all of freeArea, so the claimed
            // share is the whole pixel and no area re-weighting is needed.
            if (excess > 0.0f) {
                const float g = excess / cov;

                accAlpha += aCov * g * tClaimed;
                for (int c = 0; c < channelCount; ++c) {
                    const std::ptrdiff_t o = static_cast<std::ptrdiff_t>(c) * pixelCount;
                    outColor[o] += g * covShare * tClaimed * src[o];
                }

                tClaimed *= clampf(1.0f - excess * local, 0.0f, 1.0f);
            }
        }

        // ---- the co-located residual: claims NO new area ------------------
        if (resShare > 0.0f) {
            const float claimed = 1.0f - freeArea;
            if (claimed > 0.0f) {
                const float resLocal = clampf(aRes / claimed, 0.0f, 1.0f);

                accAlpha += claimed * resLocal * tClaimed;  // min(aRes, claimed)
                for (int c = 0; c < channelCount; ++c) {
                    const std::ptrdiff_t o = static_cast<std::ptrdiff_t>(c) * pixelCount;
                    outColor[o] += resShare * tClaimed * src[o];
                }

                tClaimed *= (1.0f - resLocal);
            } else {
                // Nothing has claimed any area yet, so there is nothing for it
                // to be co-located WITH: a rear deposit whose front deposit
                // contributed no coverage at this pixel (only reachable from
                // hand-built planes).  Treat it as its own unoccluded layer
                // rather than dropping it.
                accAlpha += aRes;
                for (int c = 0; c < channelCount; ++c) {
                    const std::ptrdiff_t o = static_cast<std::ptrdiff_t>(c) * pixelCount;
                    outColor[o] += resShare * src[o];
                }
            }
        }

        if (!(freeArea > 0.0f) && !(tClaimed > 0.0f))
            break;                      // fully opaque: nothing behind shows
    }

    *outAlpha = clampf(accAlpha, 0.0f, 1.0f);
}

// ---------------------------------------------------------------------------
// scatterBandCPU — THE SCATTER CORE.  Fragments -> bucket planes.
//
//   params  : band geometry + the sharp-path threshold (POD)
//   samples : the flattened, bucket-assigned fragment stream (M1.P3.T1)
//   holdout : M1.P3.T3's boundary transmittance LUT view; an empty view means
//             vis == 1 at zero cost
//   kernel  : the KernelSampler seam (v1: DiscKernelLUT).  This is the ONE
//             non-POD argument, and it is the design's required seam, not a
//             convenience — `destX/destY/depth/channelGroup` are passed
//             through to it even though v1 ignores them, because that
//             unused-ness IS the M2 seam.
//   planes  : ACCUMULATED INTO, never cleared here — a band may be scattered
//             from several SoA chunks.  Call planes.zero() per band.
//   scratch : per-thread; only touched when a holdout is connected
//   stats   : optional
//
// Thread-agnostic by construction: no locks, no globals, no statics, no NDK
// type, and nothing shared between two calls except what the caller passes in.
// Two threads may run it concurrently on disjoint `planes`; Phase 1.4 adds
// concurrency AROUND it, never inside it.
//
// CHANNEL COUNT is min(planes.channelCount, samples.channelCount): reading or
// writing the larger of the two would run off the end of the smaller (the same
// trap M1.P3.T1 hit between FlattenParams and SampleSoA, where it was a heap
// overflow under ASAN).  The two should of course be set from one place.
// ---------------------------------------------------------------------------
void scatterBandCPU(const ScatterParams& params,
                    const SampleSoA&     samples,
                    const HoldoutSoA&    holdout,
                    const KernelSampler& kernel,
                    BucketPlanes&        planes,
                    ScatterScratch&      scratch,
                    ScatterStats*        stats = nullptr);

// ---------------------------------------------------------------------------
// resolveBandCPU — saturate down, then combine, into the band's flat output
//
// Runs, in order:
//   1. saturateBucketPlanes() — wherever a bucket's alpha exceeds 1, rescale
//      its colour AND alpha by 1/alpha.  DOWN ONLY, NEVER UP.  This is not
//      optional and is not behind a flag: see the header of the scatter
//      section for the measured over-count it exists to correct.  Scaling up
//      would fabricate coverage and hide validation scene (i)'s honest alpha
//      dip, which is specified behaviour for this node.
//   2. the bucket composite selected by params.combine.
//
// outColor is `channelCount` planes of `pixelCount` floats
// (outColor[c*pixelCount + i]); outAlpha is one.  Both are OVERWRITTEN.
// `planes` is modified in place by the saturation pass.
// ---------------------------------------------------------------------------
void resolveBandCPU(const ScatterParams& params,
                    BucketPlanes&        planes,
                    float* __restrict__  outColor,
                    float* __restrict__  outAlpha);

} // namespace deepc

#endif // DEEPC_DEFOCUS_SCATTER_H
