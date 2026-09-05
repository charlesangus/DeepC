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
//    - BucketPlanes  : the band's K x (C + 3) accumulation planes (colour,
//                      alpha, and the two `sum of w*vis` AREA planes — new
//                      area and, since M1.P3.T9, co-located area).
//    - HoldoutSoA    : non-owning view of M1.P3.T3's per-dest-pixel boundary
//                      transmittance LUT, plus (M1.P3.T10) the HoldoutBoundaries
//                      set it was built at — which is NOT DepthBuckets'.
//                      Absent/unconnected holdout is an empty view and costs
//                      nothing.
//    - scatterBandCPU() : the scatter core proper — fragments -> planes.
//    - resolveBandCPU() : saturate-down, then combine the planes into the
//                      band's flat output by the bucket composite decided
//                      from pixels at M1.P3.T17.
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

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
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

// ---------------------------------------------------------------------------
// Per-fragment FLAG BYTE — FragmentKind plus the coverage-head bit (M1.P3.T8)
//
// THE COVERAGE HEAD BIT.  The coverage plane is `sum of w*vis`, pure kernel
// AREA, and a surface covers its kernel's area ONCE no matter how many depth
// layers it is later seen as.  Two constructions in this node turn one surface
// into several layers:
//
//   * the fractional two-bucket assignment — handled inside
//     scatterSpanBothBuckets(), which deposits coverage into the NEARER bucket
//     only (M1.P3.T2's review; depositing into both was a measured 2x energy
//     error on every partially covered region);
//   * the VOLUMETRIC SPAN SPLIT — one parent sample cut at the bucket
//     boundaries into P independent fragments.  Each of those used to deposit
//     its own `w*vis`, so one slab claimed the same pixel area once per bucket
//     it spanned: a 4-part alpha-0.9 slab measured a band alpha sum of 1.7506
//     against an honest 0.9000, growing toward K x as alpha -> 1, and exact
//     ONLY at full kernel coverage — i.e. wrong for every bokeh, every edge and
//     every isolated fog element.
//
// The fix is this bit.  Exactly ONE fragment per split parent — the FRONT-MOST
// emitted part, which is also the first the front-to-back composite visits —
// carries `coverageHead`, and only that fragment deposits into the coverage
// plane.  "Parent" here means a POST-TIDY sample: tidyOverlapping() runs first
// and cuts overlapping spans at one pixel into disjoint depth segments, each
// of which is its own parent with its own head.  One source pixel carrying N
// depth-disjoint samples therefore still deposits N coverages — that is the
// pre-existing same-pixel behaviour recorded in the milestone Decisions, not
// something this bit addresses.  The remaining parts arrive as alpha and colour with no coverage of
// their own, which is precisely the shape compositePixelCoveragePartition()'s
// residual term already handles (it was added for the fractional split's rear
// deposit): a co-located layer claiming no new area, `over`-attenuated by
// tClaimed.  See that function for the derivation showing this reconstructs the
// parent EXACTLY at any kernel coverage, not merely at full coverage.
//
// A point sample is its own parent and is always a head.
//
// STORED AS A BIT IN THE EXISTING `kind` BYTE, not as a sixteenth SoA array.
// M1.P3.T15 added two more bits to the same byte on the same argument (bits 2
// and 3, "does this deposit write area at all?"), so the flag byte now carries
// four pieces of per-fragment state and the SoA is still 61 B/fragment.
// The SoA is already ~100 B/fragment resident at C=4 (61 logical; milestone
// Decisions 2026-07-27) and
// dominates the memory-limit budget, so a new PodBuffer would add its own
// geometric capacity slack for one bool; bit 1 of a byte that only ever used
// bit 0 costs nothing.  SampleSoA's array is named `flags` rather than `kind`
// for exactly this reason: every reader must go through fragmentKindOf(), and
// renaming the member makes an un-updated `static_cast<FragmentKind>(...)` a
// compile error rather than a silent mis-read of a head fragment as kind 3.
// ---------------------------------------------------------------------------
constexpr std::uint8_t kFragmentKindMask    = 0x01;
constexpr std::uint8_t kFragmentHeadBit     = 0x02;
// M1.P3.T15: "does this deposit write its w*vis into an AREA plane at all?"
// Set for every deposit that covers area no earlier same-pixel deposit of the
// same kernel already covered; clear for the ones that do not (see
// visitBucket() in the .cpp).  Defaults SET, so a hand-built fragment
// behaves exactly as it did before T15.
constexpr std::uint8_t kFragmentArea0Bit    = 0x04;
constexpr std::uint8_t kFragmentArea1Bit    = 0x08;

// packFragmentFlags() MASKS the kind, so a third FragmentKind would be
// silently truncated into Point rather than mis-read.  Fail the build instead:
// adding a kind means widening the mask and moving the head bit.
static_assert(static_cast<std::uint8_t>(FragmentKind::Volumetric) <= kFragmentKindMask,
              "FragmentKind no longer fits in kFragmentKindMask — widen the mask "
              "and move kFragmentHeadBit");
static_assert((kFragmentKindMask & kFragmentHeadBit) == 0,
              "the kind mask and the coverage-head bit overlap");

DEEPC_HD inline std::uint8_t packFragmentFlags(FragmentKind kind, bool coverageHead,
                                              bool depositArea0 = true,
                                              bool depositArea1 = true)
{
    return static_cast<std::uint8_t>(
        (static_cast<std::uint8_t>(kind) & kFragmentKindMask)
        | (coverageHead  ? kFragmentHeadBit  : std::uint8_t{0})
        | (depositArea0  ? kFragmentArea0Bit : std::uint8_t{0})
        | (depositArea1  ? kFragmentArea1Bit : std::uint8_t{0}));
}

DEEPC_HD inline FragmentKind fragmentKindOf(std::uint8_t flags)
{
    return static_cast<FragmentKind>(flags & kFragmentKindMask);
}

DEEPC_HD inline bool fragmentCoverageHeadOf(std::uint8_t flags)
{
    return (flags & kFragmentHeadBit) != 0;
}

DEEPC_HD inline bool fragmentDepositsArea0Of(std::uint8_t flags)
{
    return (flags & kFragmentArea0Bit) != 0;
}

DEEPC_HD inline bool fragmentDepositsArea1Of(std::uint8_t flags)
{
    return (flags & kFragmentArea1Bit) != 0;
}

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
// scatterKernelBin — "would the scatter rasterise these two radii identically?"
//
// M1.P3.T13 needs a predicate that is TRUE only when two fragments deposit the
// same weights into the same pixels, because that is exactly the condition
// under which over-compositing them at the flatten is lossless.  It mirrors
// the only two decisions the scatter makes about a radius, and nothing else:
//
//   * scatterBandCPU() takes the SHARP path for `!(radius >= sharpRadiusPx)`,
//     where the "kernel" is a single weight of 1.0 at the fragment's own pixel
//     — so every sharp radius is the same kernel, and NaN is sharp there too;
//   * otherwise DiscKernelLUT::radiusToIndex() rounds to the nearest node of
//     the global kernel-radius grid (`kernelGridIndex()`, owned by
//     DeepCDefocusKernel.h and CALLED here rather than re-derived, so the two
//     cannot drift), so two radii on the same grid node return the SAME
//     KernelView — identical weights, identical row spans, identical support.
//
// M1.P3.T19 NOTE: that grid is no longer a uniform 0.5px one — below 16px it
// refines as `c*r^2` — so this predicate is now much STRICTER at small radii
// than it was (at r = 1px two radii must agree to ~0.002px to share a node,
// against 0.5px before). That is the conservative direction: the absorb below
// exists so a group never rasterises a disc none of its members has, and it
// now fires only when the members genuinely rasterise the same one.
//
// IT DOES NOT TOUCH `pre_merge`. The two are different mechanisms: `pre_merge`
// groups same-pixel fragments whose radii are within `merge_tolerance` and
// this predicate is not consulted, so the milestone Decisions' finding —
// `pre_merge` is reachable AND lossy at its 0.25px shipping default, because a
// pair 0.20px apart is grouped and then rasterised at the front member's
// radius — stands unchanged. Harness check `i7` (CoC radius 1.2 and 1.4 px)
// still reads the same 9.0000e-02 on 100% of pixels after this task as before
// it. M1.P4.T2's review of `merge_tolerance`'s default is NOT discharged here.
//
// The LUT additionally CLAMPS the index into its built [rMin, rMax] range, so
// two different bins can still resolve to one entry.  This function does not
// model that, which makes it conservative in the safe direction: it can answer
// "different kernels" for a pair the LUT would have merged, never the reverse.
//
// M2 NOTE: with several channel groups a group's radius is
// `groupRadius(groups, g, baseRadius)`, and equal BASE bins do not imply equal
// bins after a per-group `channelRadiusScale != 1`.  In v1 every scale is 1.0,
// so the base bin IS every group's bin; M2 must revisit this alongside the
// "which group owns alpha" decision.
// ---------------------------------------------------------------------------
DEEPC_HD inline int scatterKernelBin(float radiusPx)
{
    if (!(radiusPx >= kSharpRadiusPx))      // also catches NaN, as the scatter does
        return -1;                          // the sharp one-pixel kernel
    if (!(radiusPx <= 1.0e6f))              // +inf: the LUT clamps to its last entry
        return 0x40000000;
    return kernelGridIndex(radiusPx);
}

DEEPC_HD inline bool sameScatterKernel(float a, float b)
{
    return scatterKernelBin(a) == scatterKernelBin(b);
}

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
    float         alpha    = 0.0f;      // this fragment's own alpha AS DEPOSITED
                                        // (M1.P3.T15 scales a colliding
                                        // deposit by 1 - running_k, and this
                                        // follows, so the two deposits always
                                        // reconstruct it under `over`)
    BucketDeposit deposit  = {};        // the two bucket deposits (see contract)
    FragmentKind  kind     = FragmentKind::Point;

    // NOTE (M1.P3.T10): there is deliberately NO precomputed holdout boundary
    // pair here any more.  It used to be DepthBuckets::locateBoundary(depth),
    // which indexed the ΔCoC bucket boundaries; the holdout LUT now has its own
    // boundary set and the scatter derives the pair from `depth` in O(1) closed
    // form (HoldoutSoA::locate).  Storing it would re-create the possibility of
    // an index built against one boundary array being used against another.

    // Does this fragment carry its parent sample's kernel COVERAGE?  True for
    // every point sample (each is its own parent) and for the front-most
    // emitted part of a split volumetric sample; false for that parent's
    // remaining parts.  Defaults TRUE so a hand-built single fragment behaves
    // exactly as it did before M1.P3.T8.  See the flag-byte block above.
    bool          coverageHead = true;

    // M1.P3.T15.  Does this fragment's first / second deposit write its w*vis
    // into an area plane?  False for a deposit landing on area an earlier
    // same-pixel, same-kernel deposit already covered — its alpha is already
    // `over`-composited into that bucket, and counting the area twice is what
    // makes the composite read `a - a^2/4` instead of `a`.
    bool          depositArea0 = true;
    bool          depositArea1 = true;
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
// EXACTLY ONE (index, frac) PAIR LIVES HERE, AND IT IS THE SCATTER'S PLANE
// ASSIGNMENT: bucketIndex0/1 + the deposit alphas, from bucketOf() /
// bucketOfContaining(), which measure position between bucket CENTRES.
//
// THE HOLDOUT PAIR IS GONE FROM HERE (M1.P3.T10).  It used to be
// boundaryIndex/boundaryFrac from DepthBuckets::locateBoundary() — position
// between ΔCoC BUCKET boundaries — because the holdout LUT was sampled at
// those same boundaries.  It no longer is: the LUT has its own uniform-in-z
// boundary set (HoldoutBoundaries), so a pair precomputed against the bucket
// boundaries would index the wrong array.  scatterBandCPU() now derives the
// right pair from `depth` via HoldoutSoA::locate(), which is O(1) closed form
// — cheaper than the O(log K) search this used to precompute, and 8 bytes per
// fragment lighter (69 -> 61 B/fragment; ~173 MB off a 4K/20spp band).
//
// The plan budgeted "two binary searches per fragment (assignment + holdout
// vis)".  It is now ONE: bucketOf()'s O(log K), plus an O(1) locate.
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

    // Packed per-fragment flag byte: bit 0 = FragmentKind, bit 1 = coverage
    // head, bits 2/3 = "deposit 0 / deposit 1 writes area" (M1.P3.T15).  Read
    // it through fragmentKindOf() / fragmentCoverageHeadOf() /
    // fragmentDepositsArea0Of() / fragmentDepositsArea1Of(); never cast it
    // straight to FragmentKind.
    PodBuffer<std::uint8_t> flags;

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

    // IS A HOLDOUT CONNECTED?  Not a knob — input 1's presence, which the node
    // already knows when it builds these params.
    //
    // It gates ONE thing: how far the same-pixel deposit-collision merge
    // (M1.P3.T13, see the .cpp) may reach in depth.  That merge is exact for
    // the scatter — its members rasterise the identical kernel, so
    // over-compositing them is what a `DeepToImage` flatten does — but it emits
    // ONE fragment at ONE depth, and the holdout is sampled per fragment.  With
    // a holdout connected the merge is therefore restricted to members sharing
    // a HoldoutBoundaries bracket, which is the resolution the transmittance
    // LUT has anyway; without one, depth is unobservable downstream of the
    // merge (the kernel is provably identical and the composite is exact for a
    // single fragment) and the merge runs unrestricted.
    //
    // DEFAULTS FALSE, i.e. "merge freely", because that is the branch the
    // `DeepToImage` parity gate lives on and because it is what every
    // holdout-free caller wants.  A caller that connects a holdout and forgets
    // this gets the pre-T13 holdout behaviour of a `pre_merge` group, not a
    // crash or an out-of-range read.
    bool  holdoutConnected   = false;

    int           channelCount = 0;
    ChannelGroups groups       = {};
};

// ---------------------------------------------------------------------------
// sanitizeFragmentDepth — THE depth sanitiser the flatten applies, exported
//
// It lives here rather than in flattenPixelToSoA()'s translation unit because
// THREE passes have to agree about what a depth means, and the milestone has
// already been bitten twice by two of them disagreeing (the holdout's missing
// ray-distance factor, and computeDepthRange() measuring a range the flatten
// then falls below).  The node's depth-range pass calls this; so does the
// flatten; the holdout's appendPixel() calls it too and then applies its own
// extra NaN rule on top (see there — NaN is DROPPED on the holdout side, not
// mapped to 0).
//
//   NaN, -inf -> 0.0f       ("invalid depth": radius 0, first bucket)
//   +inf      -> kMaxDepth  (a real far-field sample, kept finite)
//
// Finite non-positive depths are left alone: signedCocPixels() already
// specifies d <= 0 -> radius 0, and rewriting them would turn a behind-camera
// sample into a near-field one clamped to max_radius.
// ---------------------------------------------------------------------------
DEEPC_HD inline float sanitizeFragmentDepth(float v)
{
    if (std::isfinite(v))
        return v;
    return (v > 0.0f) ? DepthBuckets::kMaxDepth : 0.0f;
}

// ---------------------------------------------------------------------------
// rayDepthScaleAt — THE per-pixel ray-distance -> Z factor, exported
//
// `depth_is_ray_distance` corrects a ray-LENGTH depth channel to camera-space
// Z, and the correction depends on the pixel's radial filmback offset, so it
// is a per-pixel scalar rather than a constant.  Every pass that reads depth
// must apply the SAME factor at the SAME pixel:
//
//   * flattenPixelToSoA() (below) — the source fragments;
//   * HoldoutSampleSoA::appendPixel(`depthScale`) — the holdout, which the
//     milestone measured sitting 47.3% too far back in Z at the corner of a
//     20mm / 36x24 frame when this was omitted;
//   * the node's computeDepthRange() — the frame's measured range, which the
//     buckets and the holdout boundary set are both built from.  The
//     correction always SHRINKS depth, so a range measured without it puts
//     every corner-pixel sample below depthMin.
//
// Returns exactly 1.0f when the toggle is off, or when the geometry degenerates
// (non-positive/non-finite factor), so a caller can multiply unconditionally.
// ---------------------------------------------------------------------------
DEEPC_HD inline float rayDepthScaleAt(const FlattenParams& params, int x, int y)
{
    if (!params.depthIsRayDistance)
        return 1.0f;

    const float rMm = filmbackRadiusMm(static_cast<float>(x) + 0.5f,
                                       static_cast<float>(y) + 0.5f,
                                       params.coc._formatWidthPx,
                                       params.formatHeightPx,
                                       params.coc._filmbackWidthMm,
                                       params.coc._pixelAspect);
    const float s = rayDistanceToZ(1.0f, params.coc._focalLengthMm, rMm);
    return (s > 0.0f && std::isfinite(s)) ? s : 1.0f;
}

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
        // See FragmentRecord::coverageHead.  A pre-merge group's merged
        // fragment is a head if ANY of its members was one (an OR, not the
        // group head's flag) — see the pre-merge block in the .cpp for why
        // that is both loss-free and duplication-free.
        bool               coverageHead = true;
        std::vector<float> channels;
    };

    std::vector<Staged> staged;
    std::size_t         stagedCount = 0;
    std::vector<float>  mergeAccum;          // over-composite accumulator

    // The deposit-collision pass (M1.P3.T13) holds ONE group back while it
    // decides whether the next one lands in a bucket it already occupies, so it
    // needs a second accumulator; it is never live at the same time as
    // mergeAccum's group is being built.
    std::vector<float>  pendingAccum;

    // The holdout LUT's boundary set, cached across pixels.  Only built (and
    // only read) when FlattenParams::holdoutConnected is set: the collision
    // merge may not cross a bracket there.  Cached on the bucket set's own
    // range + count so it is rebuilt exactly when makeUniformHoldoutBoundaries()
    // would return something different — the boundary set the LUT is built at
    // is derived from those three numbers and nothing else, which is what keeps
    // this copy from drifting onto a different array than HoldoutLut's.
    HoldoutBoundaries   holdoutBoundaries;
    float               holdoutRangeMin   = 0.0f;
    float               holdoutRangeMax   = 0.0f;
    int                 holdoutRangeCount = 0;

    // "Has a new-area claim already been made in this bucket, at this pixel,
    // and by WHICH KERNEL?" — the second half of M1.P3.T13, for the collisions
    // the merge above cannot take (fragments at one pixel whose kernels
    // genuinely differ).  Stamped rather than cleared: `claimStamp[k] ==
    // claimEpoch` means "claimed during the current pixel", so a pixel costs no
    // reset at all.  The epoch is incremented per pixel and both arrays are
    // sized to the bucket count on first use, so this is O(1) per fragment.
    // 8 B/bucket — 1KB per thread at K=128.  `claimBin` holds the claimer's
    // scatterKernelBin(): a later deposit yields the claim only to a DIFFERENT
    // kernel — see visitBucket() in the .cpp for the measurement that makes
    // that restriction load-bearing, and for the per-bucket running alpha
    // (M1.P3.T15) that `runAlpha` holds beside them.
    std::vector<std::uint32_t> claimStamp;
    std::vector<int>           claimBin;
    std::uint32_t              claimEpoch = 0;

    // M1.P3.T15's per-bucket TOUCH record, beside the claim above: which kernel
    // last deposited into this bucket at this pixel, and how much alpha it has
    // accumulated there ("how much of THIS bucket, at THIS pixel, has already
    // been written by fragments rasterising THIS kernel?").  Distinct from the
    // claim, which records only NEW-AREA claims — a fragment's rear deposit
    // touches a bucket without claiming any area in it — and stamped off the
    // same per-pixel epoch, so a pixel still costs no reset.  Folding the two
    // into one stamp is a measured regression; see claimNewArea() in the .cpp.
    //
    // COST, because M1.P4.T1 budgets on it: THREE arrays, not one float —
    // 12 B/bucket, i.e. 1.5KB per thread at K=128, NOT the 512 B the milestone
    // brief provisionally budgeted.  With the claim pair above the flatten's
    // per-bucket scratch is 20 B/bucket, 2.5KB per thread at K=128.
    std::vector<std::uint32_t> runStamp;
    std::vector<int>           runBin;
    std::vector<float>         runAlpha;

    // M1.P3.T15: the highest bucket any deposit at the CURRENT source pixel has
    // touched, and the kernel bin of the deposit that reached it.  A later
    // (further) fragment rasterising THAT SAME kernel may not deposit in front
    // of it -- see emitPending() in the .cpp.
    int                        frontierBucket = 0;
    int                        frontierBin    = 0;
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
//    scatterBandCPU(...)                 // fragments -> K*(C+3) planes
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
// THE BUCKET COMPOSITE — DECIDED (M1.P3.T17, 2026-08-16)
//
// Two candidate rules were built and carried behind a runtime enum so the
// choice could be made from RENDERED PIXELS rather than from identities
// (milestone Decisions, 2026-07-26, "Bucket-composite alpha deficit").  The
// bake-off ran at M1.P3.T17 on validation scenes (c), (f), (g) and (i) at
// K=8/16/64, plus scene (l), and `compositePixelCoveragePartition()` below WON.
// There is no enum, no flag and no switch any more: it is the only rule.
//
// WHAT WAS DELETED, and why it is not worth resurrecting — plain front-to-back
// `over` of the K planes, ignoring both area planes (the design reference's
// original sketch, and what classical layered DOF does).  It failed on:
//
//   * THE HOLDOUT LAW.  An opaque fragment's transmittance split is a no-op
//     (a0 == a1 == alpha == 1), so both bucket deposits carry the full
//     `1*vis` and `over` composites them as independent layers: it renders
//     `2*vis - vis^2` where the truth is `vis`.  Error `vis*(1 - vis)`, worst
//     0.25 at vis == 0.5, i.e. +50% RELATIVE on the node's differentiating
//     feature, at size 0, K-independent.  Confirmed to four decimals at every
//     probe depth of harness check `f1` (0.5012 -> 0.7512, 0.3758 -> 0.6104,
//     0.1585 -> 0.2919), against 4.367e-08 for the surviving rule.
//   * FLAT OPAQUE ACROSS BUCKETS.  Distinct opaque fragments whose disc
//     weights sum to exactly 1 at a destination pixel but land in DIFFERENT
//     buckets composite to 1 - prod(1 - W_k) < 1 — 25.0% alpha deficit across
//     2 buckets, 31.6% / 4, 34.4% / 8, 35.6% / 16.  It WORSENS with K, so the
//     design's own K knob is an anti-mitigation for it: scene (g)'s opaque
//     receding plane read -0.28% / -2.15% / -7.79% at K=8/16/64 where the
//     surviving rule read -1.05% / -0.44% / -2.8e-05%.
//   * THE OPPOSITE FAILURE, WHICH IT CANNOT AVOID EITHER.  Pixel-integrated
//     alpha is linear in kernel weight while `over` is not, so one fragment
//     split across two buckets over-composites to more than it deposited
//     wherever its kernel weight is below 1 — +93.8% worst case over 3000
//     random (alpha, fraction, radius) triples, i.e. an isolated opaque bokeh
//     at DOUBLE energy, and a defocused opaque edge's alpha/colour ramp
//     inflated from 0.437 to 0.683.  Erring HIGH is what the node's
//     honest-alpha contract forbids outright.
//
// It had NO plane to correct any of that with, which is the structural reason
// the decision was not close: this rule reads the coverage and co-located area
// planes and has been corrected against measurement twice (M1.P3.T8, T9).
//
// WHAT `over` WON, recorded so it is not rediscovered as a surprise: small-CoC
// content (harness l1/l2/l3/l5 all favour it, by 2-13x but never by more than
// ~1 8-bit code value), because BOTH its failure modes are quenched below
// ~2.5 px — its across-bucket deficit needs coverage spread over many buckets,
// and its split inflation needs kernel weights well below 1.  Its advantage
// shrinks monotonically as the CoC grows.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// BucketPlaneView — POD, non-owning view of one band's accumulation planes
//
// The layout is DeepCDefocusMath.h's ("Bucket plane layout"), extended with
// the third plane the design reference requires and the fourth M1.P3.T9 adds:
//
//   colour   : color    [(k * channelCount + c) * pixelCount + i]
//   alpha    : alpha    [k * pixelCount + i]
//   new area : weight   [k * pixelCount + i]
//   co-located area
//            : colocated[k * pixelCount + i]
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
// THE FOURTH PLANE (M1.P3.T9) IS THE SAME QUANTITY FOR THE OTHER HALF OF THE
// DEPOSIT: `sum of w*vis` over exactly the deposits that carry alpha but claim
// NO NEW AREA — a fractionally split fragment's rear (farther-bucket) deposit,
// and every non-head part of a split volumetric parent.  The third plane is
// "new area", this one is "CO-LOCATED AREA".  Together they say, per bucket
// and per pixel, both how much fresh pixel area the bucket claimed and how
// much already-claimed area its remaining alpha is spread over — which is the
// divisor compositePixelCoveragePartition()'s residual term needs in order to
// recover that alpha's own per-unit-area opacity.  Before it existed the
// composite substituted the running claimed area, which is only the same
// number when every co-located layer rasterises the SAME kernel as the head
// that claimed the area; see that function for the derivation and for the
// measured error when they do not.
//
// Both area planes are ALPHA-INDEPENDENT and are never touched by the
// saturation pass, exactly like the third.
//
// Pointers rather than PodBuffer so the DEEPC_HD bodies below never name a
// host container; BucketPlanes::view() produces one.
// ---------------------------------------------------------------------------
struct BucketPlaneView {
    float*         color       = nullptr;
    float*         alpha       = nullptr;
    float*         weight      = nullptr;   // new area
    float*         colocated   = nullptr;   // co-located area (M1.P3.T9)
    int            bucketCount = 0;
    int            channelCount = 0;
    int            width       = 0;
    int            height      = 0;
    std::ptrdiff_t pixelCount  = 0;

    DEEPC_HD inline bool valid() const
    {
        return alpha != nullptr && weight != nullptr && colocated != nullptr
            && (channelCount == 0 || color != nullptr)
            && bucketCount > 0 && channelCount >= 0 && width > 0 && height > 0
            && pixelCount == static_cast<std::ptrdiff_t>(width) * height;
    }
};

// ---------------------------------------------------------------------------
// BucketPlanes — the owning form: one band's K x (C + 3) planes
//
// One instance per band-computing thread, reused across bands (allocate()
// once, zero() per band) — the design's memory-limit knob caps how many of
// these can be in flight at a time, which is why bytesForBand() lives here.
// ---------------------------------------------------------------------------
struct BucketPlanes {
    PodBuffer<float> color;     // bucketCount * channelCount * pixelCount
    PodBuffer<float> alpha;     // bucketCount * pixelCount
    PodBuffer<float> weight;    // bucketCount * pixelCount — new area
    PodBuffer<float> colocated; // bucketCount * pixelCount — co-located area

    int            bucketCount  = 0;
    int            channelCount = 0;
    int            width        = 0;
    int            height       = 0;
    std::ptrdiff_t pixelCount   = 0;

    // Sizes the four buffers and ZEROES them.  Safe to call repeatedly with
    // the same geometry (PodBuffer keeps its capacity), which is the band
    // loop's normal path.
    void allocate(int bucketCountIn, int channelCountIn, int widthIn, int heightIn);

    // Zero-fill for the next band, keeping the allocation.
    void zero();

    void release();

    std::size_t sizeBytes() const;

    BucketPlaneView view();

    // The design reference's per-band scratch formula, K*W*B*(C+3)*4 bytes,
    // in one place so the memory-limit knob and the code cannot drift apart.
    //
    // (C+3), not (C+2), since M1.P3.T9: colour + alpha + new area + co-located
    // area.  ~+17% of the bucket planes (~+17MB at 4K defaults) against a
    // ~2.4GB SoA, bought to make the coverage-partition composite exact at any
    // within-parent radius spread — see BucketPlaneView and
    // compositePixelCoveragePartition().
    //
    // CLAMP AT THE CALL SITE.  Knob ranges are soft (milestone Decisions,
    // 2026-07-26), so `depth_layers` must be clamped to [4, 128] and the band
    // height derived from a CLAMPED `max_radius` before this is evaluated —
    // otherwise the budget is computed from a number the user typed rather
    // than from the one the node will use.  This function clamps nothing: it
    // is the formula, not the policy.
    //
    // NOTE this counts ONLY the planes.  M1.P3.T1 measured the SoA fragment
    // buffers at 113 B/fragment resident, ~2.4GB for a 4K band at 20spp
    // (61 logical / ~100 resident, ~2.1GB, since M1.P3.T10 dropped the
    // per-fragment holdout boundary pair),
    // which dwarfs them; budget on the combined total (milestone Decisions).
    static std::size_t bytesForBand(int bucketCount, int channelCount,
                                    int width, int height)
    {
        const std::size_t k = (bucketCount > 0) ? static_cast<std::size_t>(bucketCount) : 0;
        const std::size_t c = (channelCount > 0) ? static_cast<std::size_t>(channelCount) : 0;
        const std::size_t w = (width > 0) ? static_cast<std::size_t>(width) : 0;
        const std::size_t h = (height > 0) ? static_cast<std::size_t>(height) : 0;
        return k * w * h * (c + 3) * sizeof(float);
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
// *** THE BOUNDARY SET IS THIS VIEW'S OWN, NOT DepthBuckets' (M1.P3.T10) ***
//
// `boundaries` is carried BY VALUE, alongside the LUT it was built at, and it
// is the single source of truth for both halves of the seam: HoldoutLut::build()
// fills `boundaryT` at exactly these depths and the scatter locates a fragment
// in exactly these depths.  There is no second party to agree with and so no
// way for a build and a lookup to drift onto different boundary arrays — which
// is the whole failure class the original "the flatten precomputes the index"
// arrangement was exposed to once the two sets stopped being the same set.
//
// The pair fed to interpAtBucket() is therefore HoldoutBoundaries::locate()'s,
// computed by scatterBandCPU() from the fragment's own depth in O(1) closed
// form.  It is NOT DepthBuckets::bucketOf()'s (position between bucket
// CENTRES; milestone Decisions 2026-07-26 measured max |vis - exact| going
// 0.680 -> 0.869 if it is used) and, since M1.P3.T10, no longer
// DepthBuckets::locateBoundary()'s either (position between ΔCoC BUCKET
// boundaries — a different array from the one the LUT is sampled at).
// SampleSoA consequently no longer carries boundaryIndex/boundaryFrac at all.
//
// `boundaries.count()` is DepthBuckets::boundaryCount() == K+1, so per-band LUT
// memory is unchanged at (K+1)*W*B*4 bytes.  The two sets share that COUNT and
// nothing else; see HoldoutBoundaries for why the PLACEMENT had to diverge.
//
// NON-OWNING on purpose: HoldoutLut owns the storage (it belongs with the rest
// of the band's per-thread scratch and its lifetime is the band's), and a POD
// view is what a device kernel can take by value.
// ---------------------------------------------------------------------------
struct HoldoutSoA {
    const float*      boundaryT  = nullptr;
    HoldoutBoundaries boundaries = {};      // the set boundaryT was built at
    std::ptrdiff_t    pixelCount = 0;       // band pixels covered by the LUT

    DEEPC_HD inline int boundaryCount() const { return boundaries.count(); }

    DEEPC_HD inline bool enabled() const
    {
        return boundaryT != nullptr && boundaries.enabled() && pixelCount > 0;
    }

    DEEPC_HD inline const float* pixelLut(std::ptrdiff_t pixel) const
    {
        return boundaryT + pixel * static_cast<std::ptrdiff_t>(boundaries.count());
    }

    // The fragment's (index, frac) into the LUT.  O(1), closed form — this is
    // what replaced the flatten's precomputed pair, and it is the ONLY locator
    // that may feed HoldoutVisibility::interpAtBucket() on this LUT.
    DEEPC_HD inline BoundarySpan locate(float depth) const
    {
        return boundaries.locate(depth);
    }
};

// The storage behind the view above is HoldoutLut, built from
// HoldoutSampleSoA -- both immediately below.  See those for the M1.P3.T3
// implementation this comment block specifies.

// ---------------------------------------------------------------------------
// HoldoutSampleSoA — one band's holdout input, DeepFront/DeepBack/Alpha ONLY
//
// THE SOURCE-SIDE MODEL DOES NOT APPLY HERE.  Holdout occlusion is evaluated
// at each DESTINATION pixel from that SAME pixel's own holdout samples --
// there is no scatter, no CoC, no bucket split and no pre-merge on this side.
// That absence is deliberate and is exactly what makes the holdout edge
// pixel-sharp (see the design reference's Holdout mechanics paragraph): a
// source fragment's blur is a property of the SOURCE, but visibility is
// looked up at each dest pixel independently from that pixel's own samples,
// so no filtering of any kind can leak across pixels.
//
// deepc::tidyOverlapping() is likewise NOT run on holdout samples, and this
// is not an oversight: HoldoutVisibility's model multiplies independent
// per-sample transmittances (Beer's law -- extinction coefficients along one
// ray multiply regardless of how the underlying spans overlap), so two
// overlapping holdout spans need no merge to combine correctly.  tidy's
// over-composite pass exists to fix the SCATTER's additive bucket planes,
// which have no equivalent here.
//
// Storage is CSR ("compressed sparse row"): `pixelOffset[i]..pixelOffset[i+1]`
// delimits pixel i's samples in the flat zFront/zBack/alpha arrays.  Built by
// calling appendPixel() once per band pixel, in the SAME band-relative
// row-major order (`i = y*width + x`) that BucketPlaneView and HoldoutSoA
// both already use for pixel indexing -- appendPixel() must be called
// exactly `pixelCount` times, once per pixel, INCLUDING pixels with zero
// samples (an empty vector still needs its offset recorded, or the CSR
// desyncs for every pixel after it).
// ---------------------------------------------------------------------------
struct HoldoutSampleSoA {
    PodBuffer<float>        zFront;
    PodBuffer<float>        zBack;
    PodBuffer<float>        alpha;
    PodBuffer<std::int32_t> pixelOffset;   // size pixelCount + 1

    std::ptrdiff_t pixelCount     = 0;   // band pixelCount (width * height)
    std::ptrdiff_t pixelsAppended = 0;   // how many appendPixel() calls so far
    std::size_t    sampleCount    = 0;   // total samples across the band

    // Prepare for a band: sets pixelCount and empties every array.  Call once
    // per band before the appendPixel() loop.
    void begin(std::ptrdiff_t pixelCountIn);

    // Capacity hint; appending works without it.
    void reserveSamples(std::size_t count);

    // Appends the NEXT band-relative pixel's holdout samples -- which pixel
    // that is follows from call order (pixelsAppended), not a parameter, so a
    // caller cannot skip a pixel and silently desync the CSR offsets.
    // Sanitises depths/alpha exactly like flattenPixelToSoA's step 1/2 (a NaN
    // depth would make std::sort's comparator a non-strict-weak ordering,
    // which is UB), drops alpha<=0 samples (inSpan() returns exactly 1 for
    // one in every branch, so it can only ever contribute a no-op factor to
    // the product -- dropping it shrinks H for build()'s per-boundary walk
    // for free), and sorts the survivors by zFront ascending --
    // HoldoutVisibility::build()'s O(H+K) fast-path precondition (violating
    // input falls back to the O(H*K) evalBoundaries() automatically, so this
    // is a performance sort, not a correctness one).  `samples` is modified in
    // place and reusable as scratch across pixels, exactly like
    // flattenPixelToSoA's own `samples` parameter.
    //
    // NaN DEPTHS ARE DROPPED, not clamped -- the one place this path
    // deliberately diverges from flattenPixelToSoA.  See the .cpp: NaN -> 0
    // is harmless on the source side but here it makes an opaque sample
    // occlude the destination pixel at EVERY boundary.
    //
    // `depthScale` is the per-pixel ray-distance -> Z factor
    // (rayDistanceToZ(1, focalLengthMm, filmbackRadiusMm(x, y, ...))), or 1
    // when `depth_is_ray_distance` is off.  IT MUST MATCH WHAT
    // flattenPixelToSoA() APPLIED AT THE SAME PIXEL: the LUT is evaluated at
    // the HoldoutBoundaries depths (M1.P3.T10), which live in Z, so an
    // uncorrected holdout sits
    // systematically too far back off-axis (measured: 47% too far in Z at the
    // corner of a 20mm/36x24 frame).  This is the request/engine-style
    // "two passes disagreeing about depth" failure the milestone already
    // names for computeDepthRange(), reached through a different door.
    void appendPixel(std::vector<SampleRecord>& samples, float depthScale = 1.0f);

    // Size 0, capacity kept (band-to-band reuse).
    void clear();

    // Drops every allocation.
    void release();

    std::size_t sizeBytes() const;
};

// ---------------------------------------------------------------------------
// HoldoutLut — THE OWNING STORAGE behind HoldoutSoA (M1.P3.T3)
//
// One instance per band-computing thread, reused band to band exactly like
// BucketPlanes: build() sizes and refills it, view() hands the scatter its
// non-owning HoldoutSoA.  Lifetime is the band's -- the thread that calls
// scatterBandCPU() must keep this alive until that call returns, and may
// reuse (rebuild) it for the next band.
//
// WHAT build() DOES: for every band pixel, HoldoutVisibility::build() fills
// boundaryCount contiguous floats at the HoldoutBoundaries depths (K+1 of them,
// uniform in Z -- NOT the ΔCoC bucket boundaries; see below and
// HoldoutBoundaries), folding the in-span exponential attenuation in once per
// PIXEL rather than once per
// FRAGMENT -- this is the whole reason the scatter's per-fragment-pixel cost
// is O(1) instead of a binary search over holdout samples.  A pixel with zero
// holdout samples costs exactly HoldoutVisibility::build()'s empty-sample
// fast path (a boundaryCount-long fill of 1.0, no per-sample work at all),
// so a band that only PARTLY overlaps the holdout bbox needs no bbox test of
// its own -- the per-pixel sample count answers the same question for free.
//
// THE ZERO-COST CASE.  If the WHOLE band's holdout input is empty --
// unconnected, or the band lies wholly outside the holdout's bbox --
// `samples.sampleCount == 0` and build() RELEASES any previous allocation
// instead of filling a real array.  view() then returns a disabled
// HoldoutSoA (boundaryT == nullptr), scatterBandCPU()'s `useHoldout` gate is
// false for the whole band, and nothing in the per-fragment path so much as
// dereferences the holdout -- see scatterFragmentSpans/scatterFragmentSharp,
// which branch on holdout.enabled() before ever calling interpAtBucket().
// That is the genuinely-free path the design reference requires, not a
// multiply by 1.0 per fragment.
//
// ZERO COST IS THE CALLER'S HALF TOO.  build() itself on an empty band is one
// branch (measured 7.8 ns for a 4096x64 band), but running the appendPixel()
// loop to DISCOVER that the band is empty is NOT free: 262144 calls with an
// empty vector measured 1.98 ms/band, ~67 ms per 4K frame of pure
// bookkeeping.  So M1.P3.T5 must SKIP the fetch/append loop entirely when
// input(1) is unconnected or the band's box does not intersect the holdout's
// -- begin(N) followed by no appendPixel() at all is well defined and lands
// on exactly the same disabled view.  Only a band that genuinely straddles
// the holdout bbox should walk its pixels.
//
// COST WHEN IT IS ON, for M1.P4.T1's budget and the Phase 1.4 perf gate
// (4096x64 band, K=16, 2 holdout samples/pixel, measured at this review):
// appendPixel() 15.3 ms/band, build() 24.7 ms/band, 17.0 MB/band.  Roughly
// 1.4 s and 578 MB across a 4K frame's 34 bands, and both scale with K.  None
// of that is in BucketPlanes::bytesForBand() -- see M1.P4.T1.
//
// ***  THE BOUNDARY SET IS DECOUPLED FROM THE ΔCoC BUCKETS (M1.P3.T10)  *****
//
// This class used to sample transmittance at the SCATTER's ΔCoC bucket
// boundaries, as the design reference originally specified, and
// interpAtBucket() chorded between two of them in log space.  That is exact
// only while no holdout span edge falls strictly inside the bracket.  For the
// commonest holdout of all -- one opaque card, i.e. a POINT sample -- the true
// T is a step, and the log chord collapses it onto the bracket's NEAR
// boundary, so the card behaved as if it sat up to a whole bucket closer to
// camera.  Measured at M1.P3.T3's review, at the node's DEFAULTS
// (K=16, focus 10, depth range [1,100] -- the 15/1 front/back bucket split the
// milestone Decisions already record): an opaque holdout at z=50 landed in
// bucket [10,100] and started occluding at z=10.9.  A source fragment at z=15,
// 35 units IN FRONT of the holdout, came out 98% erased; mean |vis error|
// over the depth range was 0.391, max 1.0.  K did not rescue it -- at K=128
// the same card still bit at z=40.2.  The magnitude was also set by
// kMinTransmittance (moving the floor 1e-30 -> 1e-3 moved the bite 10.9 ->
// 19.0), which is the tell that it was not a principled approximation.
//
// The DOMINANT term was placement, and no interpolant could have recovered a
// 90-unit-wide bracket: with only two boundary values a monotone T can be
// anywhere between them, so no interpolant beats a worst case of (T0 - T1)/2.
// The fix is therefore a DIFFERENT boundary set for this LUT than
// for the scatter's buckets -- the ΔCoC spacing exists to bound BANDING (a CoC
// criterion) and spends 15/16 of its budget in front of focus, which is the
// wrong criterion for depth occlusion.  build() now takes HoldoutBoundaries
// (uniform in Z over the frame's measured depth range, the SAME K+1 count):
// mean |vis error| 0.057 against 0.391, biting at 44.4 against a true 50.  The
// per-fragment index stays O(1) and closed form, and per-band memory is
// unchanged.  See HoldoutBoundaries for the full bake-off, including why a
// holdout-depth-histogram-derived set lost and why uniform-in-1/z is not it.
//
// WHAT IS LEFT (decided at M1.P3.T18).  Half a bracket of placement
// uncertainty is irreducible with K+1 values; on top of it, for a bracket
// whose far transmittance is bitwise zero (an opaque step, or a dense
// alpha<1 stack whose product underflowed) the log chord floors log T at
// kMinTransmittance and so collapses to ~0 across the whole bracket,
// one-sided TOWARD CAMERA -- harness check f2 pins it at 78% of a
// depthRange/K bracket erased in front, decaying as 10^(-30*frac).  Two
// opaque-step alternates (midpoint step, linear-in-T) were shipped behind a
// runtime flag at M1.P3.T11 and DELETED at M1.P3.T18 after a rendered
// bake-off: they erased less in front of an opaque card (30% / ramp), but
// on a dense volumetric holdout -- 46 samples at alpha=0.9 in one bracket,
// scene (f)'s content class -- they LEAKED source through the fog at up to
// full visibility (midpoint +1.000, linear-in-T +0.840, log chord +1e-09
// worst leak, rendered end to end), and they leaked behind opaque cards at
// other card positions.  Erase-toward-camera is bounded and K-reducible;
// invented visibility through a holdout is neither.  See "THE HOLDOUT
// INTERPOLANT -- DECIDED" in DeepCDefocusMath.h for the full numbers.
// ***************************************************************************
// ---------------------------------------------------------------------------
struct HoldoutLut {
    PodBuffer<float> boundaryT;   // pixelCount * boundaryCount, PIXEL-MAJOR

    // The set boundaryT was built at.  Stored so view() can hand it to the
    // scatter with the LUT: the two must never come from separate places.
    HoldoutBoundaries boundaries = {};

    int            boundaryCount = 0;   // == boundaries.count() == K + 1
    std::ptrdiff_t pixelCount    = 0;

    // Builds (or, per the note above, clears) the LUT from one band's
    // flattened holdout samples and the frame's HOLDOUT boundary set -- NOT
    // its DepthBuckets (M1.P3.T10).  Safe to call repeatedly with the same
    // geometry (PodBuffer keeps its capacity), which is the band loop's normal
    // path.  If `samples` was not filled for the full `samples.pixelCount`
    // (a caller bug), the unfilled tail is treated as zero-sample rather than
    // read out of bounds.
    //
    // `boundaries` MUST be frame-global, not per-band: a fragment near a band
    // edge scatters into two bands, and if those bands' LUTs were sampled at
    // different depths the same fragment would get two different vis values --
    // a visible seam along every band boundary.  makeUniformHoldoutBoundaries()
    // derives it from the frame's DepthBuckets, which are already global.
    void build(const HoldoutSampleSoA& samples, const HoldoutBoundaries& boundaries);

    void release();

    std::size_t sizeBytes() const;

    // Non-owning view for scatterBandCPU().  Empty (disabled) whenever
    // build() found nothing to build.
    HoldoutSoA view() const;
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
    // (`holdoutInterp` sat below this until M1.P3.T18 decided the holdout
    // interpolant from rendered pixels and deleted the losing variants and
    // the flag, as M1.P3.T17 did for `combine` before it -- there is one
    // interpolant now and nothing to select.)
    float sharpRadiusPx = kSharpRadiusPx;
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
// coverageHead is the OTHER gate on the coverage plane, and it is per FRAGMENT
// rather than per group: a split volumetric parent's non-head parts carry
// alpha and colour but no coverage (M1.P3.T8).  The two are separate because
// they answer different questions — "is this the group that owns the alpha and
// coverage planes" versus "is this the fragment that owns its parent's area".
// The coverage plane is written when BOTH hold, and then only by the
// fragment's FIRST bucket deposit even when it straddles two — see
// scatterSpanBothBuckets().
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

    // HoldoutBoundaries::locate()'s pair — position between the HOLDOUT LUT's
    // own boundaries (M1.P3.T10), NOT the bucketOf() pair above and NOT
    // DepthBuckets::locateBoundary()'s.  Filled by scatterBandCPU() from the
    // fragment's depth in O(1); left at {0, 0} when there is no holdout, in
    // which case nothing reads it.
    int   boundaryIndex = 0;
    float boundaryFrac  = 0.0f;

    const float* color = nullptr;   // the fragment's interleaved channels
    int   firstChannel = 0;         // this group's channel range within them
    int   groupChannels = 0;

    bool  depositCoverage = false;  // alpha + coverage planes: one group only
    bool  coverageHead    = true;   // coverage plane: one fragment per parent
    bool  depositArea0    = true;   // M1.P3.T15: does deposit 0 write area?
    // ...and deposit 1.  A LIVE CASE, not a guard.  M1.P3.T15 shipped calling
    // it "unreachable through flattenPixelToSoA() today" and rested two
    // surviving mutants on that; its review disproved it.  The frontier clamp
    // is gated on the kernel bin and `frontierBin` is a single slot, while CoC
    // radius is V-SHAPED about the focal plane — so three same-pixel samples
    // straddling focus bin as A, B, A, the middle one leaves `frontierBin` on
    // B, the third one's clamp does not fire, and BOTH of its deposits land on
    // buckets the first already touched with kernel A.  Measured over a
    // 900-pixel randomised corpus at Manual size 6, `pre_merge` off: 25 such
    // fragments at K=4 and 2 at K=16 (0 at size 0 — one bin, so the milestone's
    // size-0 parity gates cannot reach this at all).
    //
    // The invariant is "at most one area plane PER DEPOSIT", not "per
    // fragment", and it is pinned end to end by "the `no area at all` deposit
    // is REACHABLE ... and the scatter honours both bits" in the suite.
    // (A per-fragment `holdoutInterp` copy of the M1.P3.T11 bake-off flag
    // lived here until M1.P3.T18 decided the interpolant from rendered
    // pixels and deleted the losing variants and the flag.)
    bool  depositArea1    = true;
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
// `depositAlpha` gates the alpha plane (one channel group only) and, INSIDE
// it, `depositWeight` the NEW-AREA plane and `depositColocated` the
// CO-LOCATED-AREA plane.  The three are separate because a fragment's alpha
// lands in both of its buckets, and every part of a split parent's alpha lands
// in its own bucket, while the parent's area lands in exactly one place — as
// new area for the deposit that claims it, and as co-located area for every
// other deposit that sits on it.
//
// `depositWeight` and `depositColocated` are mutually exclusive per deposit
// (one deposit's `w*vis` is either new area or co-located area, never both),
// but that is the caller's invariant — scatterSpanBothBuckets() — not
// something re-decided here.  Since M1.P3.T15 a deposit may write NEITHER: one
// that was `over`-composited onto an identical earlier deposit of the same
// kernel at the same source pixel covers area that is already in the plane, and
// counting it again is what makes the composite read `a - a^2/4` in place of
// `a`.  So the deposit invariant is now "at most one area plane per deposit",
// not "exactly one".
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
                                    bool                   depositWeight,
                                    bool                   depositColocated)
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

        // The same area, for a deposit that claims none of its own: the
        // composite needs it as the DIVISOR that turns this deposit's alpha
        // back into a per-unit-area opacity.  Alpha-independent for the same
        // reason the plane above is.
        if (depositColocated) {
            float* __restrict__ dp = planes.colocated + planeBase;
            for (int i = 0; i < count; ++i)
                dp[i] += w[i];
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
//
// AND THE DEPOSIT THAT WRITES NO AREA AT ALL (M1.P3.T15).  `depositArea0` /
// `depositArea1` are clear for a deposit whose area an earlier deposit of the
// SAME kernel at the SAME source pixel already put in the plane.  The two cover
// the identical destination pixels with the identical weights — the flatten has
// already `over`-composited the second onto the first — so writing the area
// twice would tell the composite that one surface covers two pixels' worth of
// area, and its C_k : D_k split then reads `a - a^2/4` where the truth is `a`
// (a flat 0.25 short once the alpha saturates: two opaque layers at one pixel
// read 0.750000 against a true 1.0).
//
// THE SAME RULE, ONE LEVEL UP (M1.P3.T8): a VOLUMETRIC parent cut at the
// bucket boundaries becomes several independent fragments, and only the
// front-most of them carries `coverageHead`.  A slab that spans four buckets
// covers its kernel's area once, not four times; before the flag it measured a
// band alpha sum of 1.7506 against an honest 0.9000.  The non-head parts take
// the identical "alpha and colour with no coverage" path the rear deposit
// takes, through the identical residual term — which is why this needed no
// composite change at all.
// ---------------------------------------------------------------------------
DEEPC_HD inline void scatterSpanBothBuckets(const BucketPlaneView& planes,
                                            const ScatterFragment& frag,
                                            std::ptrdiff_t         dstOffset,
                                            const float* __restrict__ w,
                                            int                    count)
{
    // THE AREA IS DEPOSITED EXACTLY ONCE PER DEPOSIT, into one of the two area
    // planes and never both: `w*vis` is NEW area when this deposit is the one
    // that claims it (the fragment's nearer bucket, and only if the fragment is
    // its parent's coverage head) and CO-LOCATED area otherwise.  Summed over
    // the two planes the total is exactly what a single "deposit every part"
    // coverage plane used to hold — which is why the split costs no energy and
    // why the composite can tell the two apart.
    // `coverageHead` already implies `depositArea0` (the flatten clears the
    // head for exactly the deposits it clears the area bit for), so the
    // NEW-AREA term does not test the bit again -- testing it would be an
    // unreachable branch, and a mutation removing it would be equivalent.
    depositRowSpan(planes, frag.bucket0, dstOffset, w, count,
                   frag.color, frag.firstChannel, frag.groupChannels,
                   frag.alpha0, frag.colorScale0,
                   frag.depositCoverage,
                   frag.depositCoverage && frag.coverageHead,
                   frag.depositCoverage && frag.depositArea0 && !frag.coverageHead);

    if (frag.bucket1 != frag.bucket0) {
        depositRowSpan(planes, frag.bucket1, dstOffset, w, count,
                       frag.color, frag.firstChannel, frag.groupChannels,
                       frag.alpha1, frag.colorScale1,
                       frag.depositCoverage, false,
                       frag.depositCoverage && frag.depositArea1);
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
                    holdout.boundaryCount(), bIndex, bFrac);
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
                                              holdout.boundaryCount(),
                                              frag.boundaryIndex,
                                              frag.boundaryFrac);
    }

    scatterSpanBothBuckets(planes, frag, dstOffset, &w, 1);
    return 1;
}

// ---------------------------------------------------------------------------
// compositePixelCoveragePartition — THE BUCKET COMPOSITE.  THE PER-PIXEL BODY.
//
// The rule M1.P3.T17 kept (see "THE BUCKET COMPOSITE — DECIDED" above).
// Pointers are pre-offset to their pixel and everything else is derived from
// pixelCount, exactly as the deleted candidate's body was, so the M3 CUDA seam
// is unchanged by the decision.
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
// split fragment, and (since M1.P3.T8) every non-head part of a split
// volumetric parent: scatterSpanBothBuckets() deposits the fragment's `w*vis`
// into the NEARER bucket's coverage plane only, and only for the parent's
// front-most part (see there for why, and for the measured energy errors of
// depositing it twice / once per bucket), so those arrive
// as alpha and colour with no coverage of their own.  They are CO-LOCATED with area
// its front half already claimed one bucket in front of it, so it claims no
// new area and is `over`-attenuated by tClaimed:
//
//   aCov = the share the bucket's own NEW area accounts for
//   aRes = A_k - aCov      the co-located residual
//   accAlpha += aRes * tHead;      tClaimed -= aRes * tHead / claimedArea
//
// where D_k IS THE FOURTH PLANE — the co-located AREA those same deposits
// wrote (M1.P3.T9).  It replaces `claimedArea`, which is what the divisor used
// to be; see "THE FOURTH PLANE" below for the derivation and for the measured
// error the substitution cost.  `tHead` IS THE FIFTH SCALAR (M1.P3.T20) — the
// transmittance of the SUB-AREA the residual actually sits on, rather than the
// pooled mean over everything claimed; see "THE HEAD TRANSMITTANCE" below.
// SINCE M1.P3.T21 IT IS A STACK OF SUCH SUB-AREAS, one per parent, and `aRes`
// is allocated across them by area — read the two sections together: T20's
// argument for WHICH transmittance a residual sees is unchanged, and T21 only
// stops one parent's tile from being thrown away to make room for another's.
//
// THE RESIDUAL'S ALPHA IS NOT CLAMPED TO THE CLAIMED AREA (M1.P3.T8 review).
// It was `min(aRes, claimedArea)` — "a layer cannot block more area than it
// sits on" — which is unreachable for the fractional split that term was
// written for (there aRes = w*a1 <= w = claimedArea, always) but is reached
// constantly once a split volumetric parent's non-head parts arrive as pure
// residual with a DIFFERENT kernel radius from their head: a rear part's disc
// is denser than the head's wherever it is smaller, so aRes > claimedArea over
// the whole inner disc and the clamp silently DESTROYED deposited alpha —
// measured -45.5% of a slab's energy for a 4-part span reaching the focal
// plane, against -4.6% without the clamp.  Worse, IT CLAMPED ONLY THE ALPHA:
// the colour term next to it takes the resShare/tClaimed path with no clamp of
// its own, so the same case came out with a premultiplied colour:alpha ratio
// of 0.8748 against the input's true unpremultiplied 0.5 — a 75%-too-bright
// pixel, not merely a dim one.  Without the clamp the ratio is 0.5000 on every
// case measured.  Dropping real deposited alpha is
// the same class of error as fabricating it, and nothing in this node's
// honest-alpha contract licenses it (that contract forbids scaling alpha UP,
// not accounting for what was actually deposited).  Removing it cannot
// over-count either: per bucket the three terms still sum to at most
// aCov + aRes = A_k, so accAlpha never exceeds the alpha the scatter
// deposited.  Every documented identity below is bit-unchanged by this
// (verified: two 50% fog layers 0.750000, receding opaque 1.000000, scene (i)
// 60% coverage 0.600000, fractional split exact at every (alpha, fraction)).
// The CLAMP IS RETAINED for the transmittance update, where it is a genuine
// bound: the claimed area cannot be more than fully blocked.
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
//   * A DEPTH RAMP — many fragments at DIFFERENT depths reaching one
//     destination pixel, each split across its own bucket pair, kernel weights
//     summing to 1.  The answer is the surface's own alpha, exactly, at every
//     fragment count, alpha and split fraction, because each fragment claims
//     its own tile of the pixel and its rear deposit is attenuated by that
//     tile alone.  This is what M1.P3.T20 fixed and what THE HEAD
//     TRANSMITTANCE below derives; before it the same rig lost 17.4% of an
//     alpha-0.90 surface at 16 fragments and diverged from there.
//
//   * A VOLUMETRIC parent split at the bucket boundaries (M1.P3.T8) — its
//     parts are one surface seen as P layers, so exactly the front-most part
//     deposits coverage and the other P-1 arrive as pure residual.  Writing
//     out the recursion: the head gives cov = w, alpha = w*a_0, so fit = w,
//     local = a_0, accAlpha = w*a_0, tClaimed = 1 - a_0 and claimedArea = w.
//     Every later part p has cov = 0, aRes = w*a_p and claimedArea = w, so
//     resLocal = a_p EXACTLY (never clamped, since a_p <= 1) and it
//     contributes w*a_p*prod_{j<p}(1 - a_j) while tClaimed picks up its
//     (1 - a_p).  Summing:
//
//         accAlpha = w * (1 - prod_p (1 - a_p)) = w * alpha
//         accColor = w * C * sum_p s_p prod_{j<p}(1 - a_j) = w * C
//
//     for ANY w in (0, 1] — i.e. the parent is reconstructed exactly at every
//     kernel coverage, not merely at full coverage, and the band sums come out
//     at the parent's own alpha and premultiplied colour because the disc
//     weights sum to 1.  Since M1.P3.T9 the recursion no longer needs `w` to be
//     ONE number: with D_k the per-part weight cancels part by part, so the
//     same result holds at any radius spread that does not increase front to
//     back — see THE FOURTH PLANE below for the measured before/after and for
//     the one direction it cannot reach.  Verified end to end at EQUAL part
//     radii (the pre-T9 premise, which the radius clamp produces for a slab in
//     the saturated near field): band alpha sum exact to <= 1.7e-07 across
//     alpha 0.01..1, 2/4/8 parts and kernel radii 0.2/3/21px, where the
//     depositing-every-part form read 2.00 / 4.00 / 8.00 for an opaque slab
//     and 1.3675 / 1.7506 / 2.0008 at alpha 0.9.
//
// THE FOURTH PLANE — WHY `claimedArea` WAS THE WRONG DIVISOR (M1.P3.T9).
// The residual's local opacity has to be `its own alpha / its own area`.  Until
// T9 the only area the composite had was `claimedArea`, the running total of
// what everything in front had claimed, and substituting it is correct exactly
// when the co-located layer rasterises the SAME kernel as the head that claimed
// that area.  For a fractionally split POINT fragment that always holds (one
// kernel, one pixel: aRes = w*a1 and claimedArea = w).  For a VOLUMETRIC parent
// cut at the bucket boundaries it does not: each part is CoC'd at its own
// midpoint, so a slab spanning N buckets rasterises N different-sized discs and
// the governing quantity is the RADIUS RATIO INSIDE ONE PARENT, not the bucket
// count.  That ratio is unbounded whenever a span reaches the focal plane — the
// in-focus part takes the sharp path (w == 1 into one pixel) while the head is
// spread over a disc.
//
// With D_k the fourth plane, the derivation above closes for real.  Part p of a
// split parent deposits alpha w_p*a_p and co-located area w_p into its own
// bucket, so
//
//     resLocal = aRes / D_k = (w_p * a_p) / w_p = a_p        EXACTLY, any radius
//
// and the front-to-back recursion telescopes per pixel:
//
//     accAlpha = sum_p w_p(i) * a_p * prod_{j<p} (1 - a_j)
//
// which integrates over the image to a_p's own `over` product, i.e. the parent's
// alpha and premultiplied colour, because every disc's weights sum to 1.
// Measured on the standard rig (Physical, f=50 N=2.8 filmback 36 at 1920px,
// range [1,100], K=16), band-alpha error against the parent, before -> after:
//
//   in front of focus (focus 10), alpha 0.9:
//     1-2 buckets  -0.00%  ->  -0.00%      4 buckets   -6.37% ->  +0.0000%
//     8 buckets   -24.79%  ->  +0.0000%   12 buckets  -46.49% ->  -0.0000%
//   the same cases at alpha 0.1: -0.00 / -0.52 / -3.02 / -12.10%  ->  0.0000%
//
// i.e. EXACT (<= 1e-6 relative) at every part count, at every alpha, whenever a
// parent's part radii do not INCREASE front to back — which is every parent
// lying WHOLLY in front of focus, including one spanning that side's entire
// depth range (measured -68.94% -> -0.0000% at alpha 0.9 for z in [1, 10] on
// the rig above).
//
// READ THE `-92.1% -> +6.65%` FULL-RANGE ROW BELOW WITH THAT QUALIFIER.  A span
// from the near clip to the background CROSSES the focal plane, so its last
// part sits BEHIND focus with a disc wider than the parts just in front of it;
// that row is a mixed case and its post-T9 residue is entirely the behind-focus
// mechanism described next, not an in-front one.  Split the same span at focus
// and each half is exact.
//
// WHAT THE PLANE CANNOT FIX, AND WHY IT IS NOT A COMPOSITE DEFECT.  Behind
// focus the head is the part NEAREST focus, so its disc is the SMALLEST and the
// parts behind it cover pixels the head never touched.  At such a pixel bucket
// b_0 holds zero colour, zero alpha, zero new area and zero co-located area —
// the head deposited nothing there — so NO per-bucket plane, of any number, can
// tell the composite that a_0 occludes what follows.  Within-parent occlusion
// happens along the ray BEFORE the blur; a rear part's disc reaching past its
// own head's is precisely the information a per-pixel image-space composite has
// lost.  What T9 does retire there is the `claimed == 0` branch's real defect:
// it used to leave the transmittance at 1 forever, so EVERY later part of the
// same parent also arrived unoccluded.  With D_k that branch claims its own area
// and the telescope closes over the parts that do reach the pixel:
//
//   behind focus (focus 1, near end of range), alpha 0.9, before -> after:
//     2 buckets  +28.22% -> +28.22%   3 buckets  +56.16% -> +45.21%
//     4 buckets  +76.49% -> +51.23%   8 buckets +113.25% -> +59.05%
//   full-range span:  alpha 0.9  +28.04% -> +7.69%,  alpha 0.1  +1.26% -> +0.36%
//   in-front full-range span: alpha 0.9 -92.06% -> +6.65%, alpha 0.1 -24.11% ->
//   +0.33%  (the residue is that span's behind-focus half)
//
// (2 buckets is unchanged by construction: with one residual there is no later
// part for the corrected transmittance to occlude.)  The candidate M1.P3.T17
// deleted read neither area plane and was untouched by all of this; on the same
// rig it read +51.7 / +93.4 / +118.2 / +120.8% in front of focus and +49.1 /
// +75.3 / +91.1 / +117.1% behind it, which is why the comparison at T17 was
// made on scenes rather than on this one number.
//
// Net over 219 randomised single-parent cases spanning both sides of focus:
// mean |band-alpha error| 20.51% -> 5.55%, worst 121.47% -> 62.76%.  Over 300
// randomised parents whose part radii do not increase front to back (random
// alpha, part count, start bucket, kernel radius) the worst relative error is
// 1.8e-07 on alpha AND on premultiplied colour — i.e. exact.
//
// THE HEAD TRANSMITTANCE — WHY THE POOLED `tClaimed` WAS THE WRONG
// ATTENUATION, AND WHY THE UPDATE HAD TO BECOME SUBTRACTIVE (M1.P3.T20).
//
// `tClaimed` is ONE number for the whole claimed share.  A depth ramp makes
// that share a MOSAIC: each fragment reaching the destination pixel claims its
// own tile at its own depth, and each tile has its own transmittance.  Two
// consequences, both deficits, both fixed here:
//
//   1. A co-located deposit was attenuated by the pooled mean instead of by the
//      tile its own head claimed.  Its head is the only thing in front of it at
//      this pixel — by construction, since a fractional split's rear lands one
//      bucket behind its head and a split parent's parts land in consecutive
//      buckets — so every other fragment's head AND rear were occluding it for
//      free.  The head-tile stack carries the right tile forward; see the
//      allocation in the residual branch for how a bucket that leaves two (or
//      sixteen) keeps all of them, which is M1.P3.T21.
//   2. `tClaimed *= (1 - resLocal)` occluded the WHOLE claimed share with a
//      layer that covered only `resArea` of it.  The area-weighted form is
//      subtractive: the tile loses `resLocal` of its own `tHead`, so the mean
//      falls by `resArea * tHead * resLocal / claimedArea == aRes * tHead /
//      claimedArea` — exactly the alpha the line above it added, which is what
//      makes the two telescope to 1 behind an opaque backing.
//
// MEASURED, on hand-built planes with no kernel, no holdout, no flatten and no
// depth quantisation (N equal-weight alpha-fragments, each split across its own
// bucket pair; truth is alpha because the weights sum to 1), before -> after:
//
//   alpha 0.99   -2.05 / -7.02 / -8.36%  at N=2/16/64   ->  EXACT
//   alpha 0.90   -4.11 / -17.44 / -21.63%               ->  EXACT
//   alpha 0.50   -3.03 / -22.39 / -33.65%               ->  EXACT
//
// and at split fractions 0.25 and 0.75 rather than 0.50, where the old form
// read -5.49% and -38.93% at N=16, likewise exact.  (M1.P3.T17 and the first
// draft of this block both quoted the -38.9% figure against "split fraction
// 0.25"; re-measured at T20's review it is the frac 0.75 cell under this
// file's own convention, a1 = partitionAlpha(alpha, frac).)
// End to end on validation scene (g)'s ramp
// (harness g4, alpha 0.90, K=16): -16.07% -> -5.43%, and the K DIVERGENCE is
// gone — at alpha 0.50 the sweep went +0.33 / +0.33 / -5.00 / -9.19 / -13.71 /
// -19.65 / -26.62% at K=2/4/8/16/32/64/128 and now reads +1.37 / +1.37 / -0.09
// / -0.13 / +0.10 / -0.02 / -1.01%.  The OPAQUE twin improved by five to six
// decades on the same scene (g1 K=8 1.048e-02 -> 1.703e-08, g2 K=8 4.909e-02 ->
// 1.848e-06, g3 K=8 3.191e-02 -> 1.907e-06): at alpha 1 the split itself is a
// no-op, but saturation still pushes part of a bucket's alpha into the residual
// term, so the same pooling was costing the banding scene its own criterion.
//
// WHAT IT DOES NOT FIX, said plainly.  A bucket that pools deposits at
// DIFFERENT per-unit opacities still loses them into one `A_k / C_k`.  That is
// information gone at ACCUMULATION, not at composition, it is harness f3c/f3d's
// mechanism, and no per-bucket rule can recover it.  ~~It is the whole of g4's
// remaining 5.4%.~~
//
// THAT LAST SENTENCE IS WRONG, MEASURED AT M1.P3.T23.  A fifth plane carrying
// the co-located ALPHA -- so the C_k : D_k split is READ rather than guessed --
// was built and rendered: it takes f3c from -0.113% to +0.000% and f3d from
// -1.676% to -0.000%, i.e. it closes that mechanism EXACTLY, and it moves g4
// only 0.0325 -> 0.0287.  So roughly 88% of g4's remainder is something ELSE,
// and WHAT is unexplained -- stated as unexplained rather than re-attributed,
// which is the sixth time in this milestone a correct number has carried a
// fabricated mechanism.  REPRODUCED INDEPENDENTLY at T23's review, on its own
// fifth-plane build: f3c 0.7500004, f3d 0.7499995, g4 0.874128 (0.0287), f3e
// +77.411% -> +74.702%.  Two mechanisms were then EXCLUDED from that remaining
// 88%, so "unexplained" is bounded rather than merely unexamined: it is NOT the
// tile-stack cap (kCompositeHeadTiles 16 -> 64 leaves g4 bit-identical at
// 0.874128) and it IS inside the residual-occlusion path (tHeadIn = 1 drives
// g4 to 1.000000, i.e. +11.1%, so it is this function's term and not the
// flatten's or the scatter's).  (The plane is NOT shipped: the user's ruling is that
// f3c/f3d/g4 are precision, not correctness, and it costs (C+3) -> (C+4), +14%
// of the bucket planes at C=4 and +25% at C=1.  See the milestone Decisions,
// 2026-08-16, M1.P3.T23.)
//
// THE TRIGGER, CORRECTED AT T20's REVIEW.  The first draft of this block said
// the trigger was fragments carrying DIFFERENT split fractions from each other.
// It is not, and the unit suite's own cells prove it: a dense ramp in which
// EVERY fragment carries the SAME split fraction reads -2.42 / -3.69 / -4.00%
// at N=4/16/64 for frac 0.25 and -5.79 / -4.53 / -4.21% for frac 0.75, and is
// exact ONLY at frac 0.50.  The reason is one bucket down: on a dense ramp
// bucket k carries fragment k's HEAD at per-unit opacity a0 = partitionAlpha
// (alpha, 1-frac) and fragment k-1's REAR at a1 = partitionAlpha(alpha, frac),
// and a0 != a1 for every frac != 0.50 — so the bucket pools two per-unit
// opacities whether or not the fragments differ from one another.  A real ramp
// (frac = (j+0.5)/N) reads -4.91% at N=16 and randomised fractions -4.35%, i.e.
// the same scale, which is why g4 cannot reach zero.
//
// AND ONE DETAIL NO UNIT TEST PINS: `claimA = fit` rather than `cov`.  It is
// right by the same argument as `claimT` — the excess share is not a new tile.
// It survives the whole unit suite; T20 recorded it as caught by harness g4
// alone, and T20's review re-ran the mutation and found it is ALSO caught by
// g1 (9.980e-03 against a 1.0e-03 gate), g2 (4.906e-02 against 3.9e-03) and
// g3 (3.191e-02 against 3.9e-03), i.e. by four rendered checks, not one.
//
// THE HEAD-TILE STACK — WHY ONE TILE WAS NOT ENOUGH (M1.P3.T21).
// T20 carried ONE head tile, so a bucket that both claimed new area and
// continued a residual chain had to DISCARD one of the two, and discarding is
// free only while the discarded chain has no deposits left.  Two multi-part
// parents at OVERLAPPING depth ranges — two fog slabs, or a fog slab and a
// point fragment, whose kernel weights tile one destination pixel — both have
// deposits left, and the dropped one was then attenuated by the survivor's
// tile, which is not in front of it.  Swept over parts x offset x weight x
// alpha that read up to +21.1% HIGH (the reviewer's own sweep found +18.3%),
// saturating the output alpha to exactly 1 at alpha 0.90, where the pre-T20
// composite read 4-16% LOW.  The sign is the honest-alpha contract's forbidden
// one, and two fog slabs at overlapping depths is ordinary comp content.
//
// THE FIX IS MORE STATE, and neither cheap alternative was a trade worth
// making: `always carry the chain` reads -9.3% on the dense ramp and `merge the
// two by area` -5.9% there, takes harness g4 to 0.0913 and fails 40 unit
// assertions — because the question was never "what is the mean" but "which
// tile does the NEXT deposit land on".  So the composite now carries a STACK of
// tiles, newest last, and a residual is ALLOCATED across it by area from the
// newest end (see the loop above), with only the overflow landing on the tile
// this bucket itself just claimed.  Both of T20's branches survive as special
// cases of that allocation: the dense depth ramp, where the residual is exactly
// the newest tile's own rear (T20's `else`), and the M1.P3.T13/T15 same-pixel
// collision, where it overflows onto this bucket's claim (T20's `>` branch).
// Both read identically to T20 to within float reassociation, NOT bit for bit
// (corrected at T21's review, which ran a 4 000-pixel single-open-chain corpus
// -- spaced splits, one volumetric parent, same-pixel collisions, the excess
// regime -- through T20's committed build and this one): 107 of 8 000 scalars
// differ, worst 2 ULP / 1.6e-07 relative.  The allocation reaches the same
// quantity by a different summation order, so the behaviour is preserved and
// the arithmetic is not.  Nothing depends on the difference; the claim does.
//
// WHAT IT BOUGHT, on hand-built planes with no kernel, no holdout and no
// flatten, against the disjoint-tiling oracle (the parents' weights sum to 1,
// so truth is alpha with no ordering assumption):
//
//   * the staggered sweep — parts {2,3,4} x offsets 1..5 x 7 weights x 5 alphas,
//     525 cells — goes from 153 cells over +0.5% (worst +12.3%) to ZERO, worst
//     +0.000%.  Widened to parts up to 8 and offsets to 7 (2268 cells) it goes
//     from 1024 cells and +21.1% to zero.
//   * 40 000 randomised pixels of 2-6 equal-alpha parents at random weights,
//     part counts and overlapping start buckets: T20 read 18 580 of 20 000
//     cells high at 5 parts, worst +28.8%; this reads ZERO high, worst
//     +0.0000%, i.e. EXACT wherever the per-unit opacities agree.
//   * end to end, harness g4 0.0543 -> 0.0325, g2/g3 at K=16 7.153e-07 ->
//     1.192e-07 and 2.980e-07 -> 1.192e-07, f3b -0.001% -> -0.000%.  Every
//     other check in the suite is bit-identical, INCLUDING scene (a)'s size-0
//     parity (a3 1.192e-07 against its 2.4e-07 gate, unmoved) and T9's pinned
//     behind-focus residue.
//
// READ THE THREE BULLETS ABOVE WITH THEIR SCOPE ATTACHED (T21's review).  Every
// one of them holds the parents' alpha EQUAL — that is the sweep's only
// unvaried axis, and it is exactly the constraint under which this composite
// CAN be exact.  "Zero cells, worst +0.000%" is a true statement about the
// equal-alpha family and not about staggered parents in general; see below.
//
// WHAT IS LEFT.  Over the same randomised corpus with the parents' alphas
// allowed to DIFFER, 34% of pixels still read over +0.5% and the worst is
// +83.8% (T21's review's own 320 000-pixel corpus reads +91.4% volumetric and
// +99.2% deep-mixed, against +105.5% / +113.0% under T20 — so this is a large
// improvement and no regression, but it is NOT closed).  Every one of them has
// two parents' deposits in ONE bucket: the corpus splits exactly, the pixels
// with no shared bucket reading EXACT and every error living among those that
// share one.  (That split is near-tautological — for consecutive-part parents,
// "shares a bucket" and "has an overlapping range" are the same condition — so
// it localises the error without identifying its mechanism.)
//
// AND THE MECHANISM IS NOT f3c/f3d's.  T21 recorded this residual as the
// accumulation-time pooling of unequal per-unit opacities, i.e. the C_k : D_k
// split guessing how a bucket's alpha divides between its new-area and
// co-located deposits.  T21's review tested that directly by rebuilding this
// function to take a FIFTH plane carrying the co-located alpha, so the split is
// READ rather than guessed.  Result: the dense ramp's -4.107% below goes to
// +/-0.0001% at every N and both split fractions — that term really is the
// C_k : D_k split and one more plane closes it — while the +64.6% two-parent
// case is left BIT-UNCHANGED.  Two mechanisms, and the big one is the other:
//
//   TILE MIS-ASSIGNMENT ACROSS OPEN CHAINS.  The planes carry no parent
//   identity, so when several chains are open, which tile a bucket's co-located
//   area sits on is undecidable from them.  Minimal case: two volumetric
//   parents, w 0.5/0.5, parts 5 and 1, alpha 0.99 and 0.10, offset 2.  At the
//   bucket carrying parent 1's fourth part the stack holds parent 1's tile
//   (T = 0.0631) and parent 2's fresh claim (T = 0.90); NEWEST-FIRST hands
//   parent 1's residual parent 2's tile, contributing 0.271 where truth is
//   0.019.  Swapping to OLDEST-FIRST moves that case to +22.4% and takes the
//   dense ramp from -4.11% to -27.4%: the two orders trade, neither is right,
//   and the choice is a Pareto point, not an approximation converging on
//   anything.  Unlike the C_k : D_k split, NO fixed number of planes recovers
//   this — parent count per bucket is unbounded — so it is permanent.
//
// AND MOST OF THE RENDERED OVER-READ IS NOT EVEN THIS TERM (M1.P3.T23).  Five
// candidate rules were built and the three that survived POD screening were
// rendered through harness f3e/f3f; none beats the trade, and the reason is
// that the biggest cell in that family is not a tile-allocation term at all.
// f3f's `overlap 100% (coincident spans)` reads +80.428% and is BIT-IDENTICAL
// under a fifth plane, because when two parents' spans coincide both heads
// land in the SAME bucket and every later part likewise: the planes for {A, B}
// are NUMERICALLY IDENTICAL to those of ONE parent at the pooled density --
// one (C_k, D_k, A_k) triple, one tile.  Nothing that reads only THESE FOUR
// planes can tell them apart.  That is f3g's argument one level up (f3g is
// exact only because a single-bucket pair leaves no residual to mis-attribute),
// and it is why f3e's high arm cannot be closed from the four planes.
//
// SAY "THESE FOUR PLANES", NOT "ANY PLANE COUNT" (M1.P3.T23's REVIEW).  The
// first draft of this block said no rule could reach that cell at ANY plane
// count, and that is too strong -- a plane of a DIFFERENT KIND reaches it.  The
// term the composite drops there is a COVARIANCE: it attenuates the pooled
// residual E[a_res] by the pooled head transmittance E[1-a_head], where the
// truth wants E[a_res*(1-a_head)].  A plane carrying the second moment
// sum_i w_i*a_i^2 gives the composite the within-bucket opacity SPREAD, and
// replacing the residual's `r*T_t` by `r*T_t - s_res*s_head` (the plan's own
// untried "split the tile stack by opacity band") is a STRICT no-op wherever
// either spread is zero -- so T21's staggered exactness and the dense ramp stay
// BIT-IDENTICAL -- while taking the coincident two-parent shape from +17.6 to
// +86.3% down to -5.8 to +15.6%, and to EXACTLY 0.000% at two parts.  Measured
// on hand-built planes at the review; NOT shipped, and not a fix either: it
// leaves the staggered/offset cells untouched (+51.7% unmoved), it doubles
// f3c/f3d's deficit (-0.753% -> -1.505%), and it costs one or two more planes
// on top of the fifth.  What IS permanent is the weaker statement already made
// above: parent count per bucket is unbounded, so no FIXED plane count recovers
// parent identity in general.  It is the four-plane layout that cannot reach
// this cell, not arithmetic as such.
// What IS reachable is the cells whose two heads land in DIFFERENT buckets:
// the fifth plane plus allocating the residual's alpha by each tile's own
// per-unit opacity takes f3f's `overlap 25%` from +52.251% to +5.464% and
// `overlap 0%` from +20.906% to +0.206% -- but it also takes the disjoint-span
// DEFICIT arm from -3.278% to -38.636% and turns f3h, an arrangement this
// composite is EXACT on, into a +6.510% FAIL, at +38-41% of the composite's
// time and one more float per tile.  A trade, not a fix; measured and
// rejected.  Every candidate, both axes, the K/alpha sweeps and the costs are
// in the milestone Decisions, 2026-08-16, M1.P3.T23.  Do not re-run the naive
// conservative rule, either tile ordering, opacity-matched tile SELECTION
// (breaks M1.P3.T21's staggered exactness at -25%) or weighting the allocation
// by each tile's own 1-T (takes the 32x32 overflow row from -5.13% to
// -25.09%): all four are measured and spent.
// AND ONE RESIDUAL THIS DID NOT TOUCH, PRE-DATING T20 AND STILL OPEN.  In the
// `excess` regime a fragment's head registers NO tile (see the fit branch), so
// its own co-located rear is attenuated by whatever tile the pixel happened to
// be carrying.  Behind a full-coverage foreground of alpha aF, a defocused
// fragment of coverage wB and alpha aB reads up to +94.8% HIGH (aF -> 0,
// wB 0.05, aB 1: truth 0.0501, composite 0.0976) — bit-identical before and
// after T20, so it is not this task's regression, but it means "+1.37% is the
// worst positive excursion" describes the g4 K-sweep and NOT the composite.
// A fragment that STRADDLES the free/claimed boundary is the other one: its
// excess is attenuated by the mean over the whole claimed area including the
// tile the same fragment just claimed, which the excess does not overlap, for
// up to +8.3% (wA 0.5 / aA 1 sharp, then wB 1.0 / aB 0.5 sharp: truth 0.75,
// composite 0.8125).  Both are upward and both are unbounded by any check.
//
// AND THE g4 RIG'S LOW-ALPHA OVER-READ IS NOT THIS FUNCTION'S AT ALL
// (M1.P3.T24), which corrects two recorded attributions: scene (g)'s ramp at
// alpha <= 0.30 reads HIGH -- +6.5% at alpha 0.10 / K=4, +5.9% at the default
// K=16, g4-RIG figures, rendered -- and both M1.P3.T21 ("f3c/f3d's pooling
// seen from its positive side") and the covariance / second-moment direction
// M1.P3.T23's review handed on attributed it to the composite.  It is the
// SCATTER's: each disc is normalised over its OWN kernel, and on a steep CoC
// gradient the adjoint sum at a destination pixel is not 1 -- on that scene's
// deliberately steep 0.5 CoC-px/scanline slope the deposited weight sums to
// ~1.07, measured directly by the rendered alpha->0 limit (+7.124 / +7.063 /
// +7.037% at K=4/16/64: K-FLAT, because the scatter is K-independent).  This
// function's only role is the alpha DEPENDENCE: the spurious excess is
// `over`-attenuated by tClaimed ~ (1 - alpha), so the over-delivery shows
// fully as alpha -> 0 and is absorbed entirely by the area clamp at alpha = 1
// (same weights, g1 reads 1e-08).  Renormalise the deposited weights per
// pixel on the faithful 1-column model of that rig (the M1.P3.T24 unit test)
// and every low-alpha cell flips to a small DEFICIT (-0.25/-0.70/-0.87% at
// K=4/16/64, alpha 0.10): what THIS function contributes at low alpha is in
// the permitted direction.  No composite rule at any plane count can remove
// the rest -- the same planes arise from ~190 independent small cards at the
// same depths, whose over-composited truth is HIGHER than alpha, so one plane
// set carries two truths ("one receding surface" vs "many overlapping
// surfaces" is parent identity, which no accumulation plane carries) -- and
// the scatter-side fix, per-destination-pixel weight renormalisation, breaks
// content this composite is exact on (two full-coverage 0.5 fog layers read
// 0.75 today, 0.50 renormalised; a direction-gated version is the design's
// own deferred v2 `alpha-renormalize` toggle).  ACCEPTED AND BOUNDED at
// M1.P3.T24: harness g5 pins alpha 0.10/0.30 at K=16, the worst corner
// (alpha 0.10 / K=4, +6.5%), and the alpha-0.01 scatter control, each as a
// two-sided band, mutation-tested in both directions.  It is content-driven,
// scaling with the CoC gradient -- RENDERED at alpha 0.01 / K=16 on the same
// ramp: +7.06% at this rig's slope 0.5, +1.54% at slope 0.25 (size 43),
// +0.34% at slope 0.125 (size 21.5), tracking the real-LUT adjoint sums
// +7.19 / +1.63 / +0.39% computed from DiscKernelLUT directly (T24 review;
// the chord model's -0.6% at slope 0.125 had the WRONG SIGN -- the real
// kernel's shallow-slope residue stays slightly high, it does not cross
// zero) -- so ordinary content sits far inside those pins.
//
// THE ALPHA SPLIT IS BY AREA, NOT BY min().  When one bucket carries a head from
// one parent AND a co-located part of another, `aCov = min(A_k, C_k)` attributed
// alpha to the new-area share until it was full — pushing `local` to 1 and
// starving the residual.  Splitting A_k in proportion to C_k : D_k gives both
// sub-layers the same per-unit-area opacity, which is the only split that does
// not invent a difference between them, and it is the pre-T9 expression to the
// bit whenever one of the two areas is zero (every single-parent case, and every
// case that predates the fourth plane).  Measured over 400 randomised
// multi-sample point pixels: mean |band-alpha error| 41.13% -> 35.99%, worst
// 223.06% -> 198.00%; on equal-radius co-located point pairs both forms are
// bit-identical and exact to 1.2e-07.
//
// THOSE MULTI-SAMPLE ERRORS ARE NOT THIS FUNCTION'S TO FIX, AND THE ~36% THAT
// REMAINS IS NOT A T9 RESIDUAL (reviewer, M1.P3.T9).  A deep pixel carrying
// several INDEPENDENT samples loses their along-the-ray occlusion for exactly
// the reason the behind-focus paragraph above gives — each parent rasterises
// its own disc, and two discs of different size cannot occlude each other in a
// per-bucket reduction.  Two opaque point samples at one pixel (radii 9.6 and
// 5.6px on the rig above) band-sum to 2.0000 against the flattened truth of
// 1.0000; four receding opaque samples to 4.0000; two 0.5 fog samples sharing
// a bucket to 0.9734 against 0.7500 (that last one is the milestone's recorded
// within-bucket additive over-count, +33.3% at 2 spans).  Every one of those is
// bit-identical before and after T9, and plain `over` is worse on all of them
// (3.97 / 6.66 / 0.9734).  The area split moves the mean because it un-starves
// the residual where one bucket mixes a head with another parent's co-located
// part; it does not, and cannot, address the ray-occlusion loss underneath.
//
// A bucket with neither coverage nor alpha is SKIPPED.  A bucket with coverage
// but no alpha still contributes its colour and still claims area.
// ---------------------------------------------------------------------------
// mergeOldestHeadTiles — capacity relief for the mosaic below.  Folds the two
// OLDEST tiles into one, which is the pooled behaviour M1.P3.T20 had for ALL of
// them, applied to the chains least likely to still be open (a residual is
// allocated from the newest end).  Area is conserved, so the mosaic never gains
// or loses any.
//
// THE MERGED TRANSMITTANCE IS THE MINIMUM, NOT THE AREA-WEIGHTED MEAN, and the
// reason is the honest-alpha contract rather than a measurement: min <= the
// mean, and a lower tile transmittance can only REDUCE the alpha a later
// residual adds, so whatever the cap costs it costs downward — the direction
// the contract permits.  Said plainly: over 80 000 randomised overflow pixels
// the two forms were indistinguishable (identical worst readings in every row,
// mean |error| within 0.02 points), so this is chosen on the argument and NOT
// on the numbers; if a later corpus separates them, that measurement decides.
DEEPC_HD inline void mergeOldestHeadTiles(float* __restrict__ tileT,
                                          float* __restrict__ tileA,
                                          int&                tileCount)
{
    if (tileCount < 2)
        return;
    tileT[0] = (tileT[0] < tileT[1]) ? tileT[0] : tileT[1];
    tileA[0] = tileA[0] + tileA[1];
    for (int t = 1; t < tileCount - 1; ++t) {
        tileT[t] = tileT[t + 1];
        tileA[t] = tileA[t + 1];
    }
    --tileCount;
}

// THE DEPTH OF THE HEAD-TILE STACK (M1.P3.T21).  ONE TILE PER PARENT WHOSE
// RESIDUAL CHAIN IS STILL OPEN, plus the tiles of parents whose chains have
// closed — nothing here can tell those apart, since a chain's end is not
// recorded in any plane, so a tile is retired only by capacity.  Measured over
// 20 000 randomised pixels of N equal-alpha parents (random weights summing to
// 1, random part counts, random overlapping start buckets), judged against the
// disjoint-tiling oracle `sum_j w_j*alpha_j` — worst UPWARD reading / mean
// |error|:
//
//   parents          4               8              16              32
//   M1.P3.T20  +21.86% / 2.32  +21.22% / 2.76  +22.75% / 3.10  +18.95% / 3.34
//   depth 2     +0.000% / 2.28   +0.000% / 5.34   -0.357% / 9.18   -0.769% /11.83
//   depth 4     +0.000% / 1.29   +0.000% / 2.27   -0.106% / 2.90   -0.160% / 3.00
//   depth 8     +0.000% / 1.29   +0.000% / 2.25   -0.106% / 2.85   -0.149% / 2.96
//
// TWO TILES ALREADY REMOVE THE UPWARD ERROR ENTIRELY — past the cap the stack
// folds its two oldest tiles together and a partly-covered frontier tile can no
// longer split, and both of those OVER-occlude, which is the direction the
// honest-alpha contract permits.  What the depth buys after that is the size of
// the remaining DEFICIT.  On the random corpus above that knee is at 4 and 8,
// 16 and 32 are indistinguishable out to 128 parents; on the WORST case the
// unit suite pins — 32 equal parents of 32 parts each, i.e. 32 chains open at
// once — it still matters: depth 8 reads -20.84%, depth 16 -5.13%.  16 is the
// shipped depth for that row, and it costs nothing at the default K (577 vs
// 581 ns/pixel, below this benchmark's own noise).
//
// COST.  A per-THREAD stack frame alive only inside one call: 2 floats per tile,
// 128 bytes at depth 16, independent of K, of the band size, of the format and of
// the thread count.  It adds NOTHING to the memory_limit formula
// (`K*W*B*(C+3)*4`), which counts the bucket planes — no plane, and no
// per-bucket state of any kind, is added by M1.P3.T21.  In time, on a
// worst-case synthetic where EVERY bucket carries both new and co-located area
// (4 channels, 20 000 pixels, best of 7), against M1.P3.T20's single tile:
//
//   K=16   319 -> 581 ns/pixel     K=64  1379 -> 2900     K=128  3009 -> 6901
//
// (depth 8 would read 577 / 2598 / 6030 — same at the default K, 14% cheaper at
// K=128, and worse on the 32-chain row above.)
//
// i.e. the composite roughly doubles, on a pass that is O(K) per destination
// pixel against the scatter's O(sum pi r^2) per fragment; the harness's own
// render totals do not separate it from run-to-run variance.
constexpr int kCompositeHeadTiles = 16;

// Two is the floor, not a formality: mergeOldestHeadTiles() folds tiles 0 and 1
// together, so a depth of 1 reads off the end of the array.  A mutation run that
// set this to 1 to approximate M1.P3.T20's single tile SEGFAULTED the whole
// harness rather than reporting a number; anyone re-running that comparison has
// to revert the rule, not shrink the stack.
static_assert(kCompositeHeadTiles >= 2,
              "the head-tile merge needs at least two tiles");

DEEPC_HD inline void compositePixelCoveragePartition(
    const float* __restrict__ bucketColor,
    const float* __restrict__ bucketAlpha,
    const float* __restrict__ bucketWeight,
    const float* __restrict__ bucketColocated,
    int                       bucketCount,
    int                       channelCount,
    std::ptrdiff_t            pixelCount,
    float* __restrict__       outColor,
    float* __restrict__       outAlpha)
{
    for (int c = 0; c < channelCount; ++c)
        outColor[static_cast<std::ptrdiff_t>(c) * pixelCount] = 0.0f;

    // freeArea and claimedArea are the same quantity twice (they sum to 1) and
    // that redundancy is DELIBERATE, not sloppiness: deriving the claimed
    // share as `1 - freeArea` cancels catastrophically when the pixel is only
    // slightly covered, which is the ordinary case at a large kernel radius
    // (a 21px disc puts w ~ 7e-4 in every pixel it touches, and its anti-
    // aliased rim far less).  The residual term divides by the claimed share,
    // so that cancellation lands straight on the alpha.  Measured on 11
    // co-located layers over one claimed deposit: -7.8e-06 relative at
    // coverage 1e-3, +1.0e-04 at 1e-4, +8.2e-04 at 1e-5, +11.1% at 1e-7 with
    // the subtraction; accumulating the claimed share instead makes every one
    // of those exact to float rounding.  freeArea keeps its own accumulator
    // because it is only ever used inside a min(), where its absolute error is
    // what matters.
    float freeArea   = 1.0f;
    float claimedArea = 0.0f;
    float tClaimed   = 1.0f;
    float accAlpha   = 0.0f;

    // THE HEAD-TILE MOSAIC (M1.P3.T20, made plural at M1.P3.T21).  The
    // transmittance of the SUB-AREA a co-located deposit lands on, as opposed
    // to `tClaimed`, which is the mean over EVERYTHING claimed so far.  A
    // co-located deposit sits on area its own parent's head claimed — one
    // bucket in front of it for a fractional split, the run of buckets in front
    // of it for a volumetric parent's parts, and THIS SAME BUCKET for a
    // same-pixel collision group (M1.P3.T13/T15, where a non-head fragment's
    // co-located area lands in the very bucket its group's head claimed).
    //
    // T20 carried ONE such tile, so a bucket that both claimed area and
    // continued a chain had to discard one of the two and the dropped parent's
    // later parts were attenuated by an unrelated tile — up to +18.3% HIGH on
    // two fog slabs at overlapping depths.  The stack below carries the tiles
    // side by side instead, newest LAST, and a residual is ALLOCATED across
    // them by area from the newest end.  See "THE HEAD-TILE STACK" in the
    // header block for the derivation, the LIFO argument and the depth.
    float tileT[kCompositeHeadTiles];
    float tileA[kCompositeHeadTiles];
    int   tileCount = 0;

    for (int k = 0; k < bucketCount; ++k) {
        const std::ptrdiff_t ko = static_cast<std::ptrdiff_t>(k) * pixelCount;

        const float cov = clampf(bucketWeight[ko], 0.0f, 1.0f);
        const float a   = clampf(bucketAlpha[ko], 0.0f, 1.0f);
        if (!(cov > 0.0f) && !(a > 0.0f))   // empty bucket (also rejects NaN)
            continue;

        // The fourth plane: the area this bucket's CO-LOCATED deposits are
        // spread over, which is the residual term's divisor.  Clamped like the
        // coverage plane and for the same reason — the saturation pass bounds
        // alpha, not area, so an over-covered pixel can carry more than a
        // pixel's worth of it.
        const float colo = clampf(bucketColocated[ko], 0.0f, 1.0f);

        const float* __restrict__ src = bucketColor
            + static_cast<std::ptrdiff_t>(k) * channelCount * pixelCount;

        // Split the bucket's alpha into the share its own coverage accounts
        // for and the co-located residual (a fractionally split fragment's
        // rear deposit, whose coverage went to the bucket in front of it).
        // Colour follows alpha, EXCEPT for a zero-alpha bucket, whose colour
        // all follows the coverage — an emissive or holdout-zeroed fragment
        // must not have its colour dropped.
        // SPLIT BY AREA when both kinds of deposit landed in this bucket
        // (M1.P3.T9).  The two sub-layers are then at the SAME per-unit-area
        // opacity a/(C_k + D_k), which is the only split that does not invent
        // a difference between them; `min(A_k, C_k)` instead attributed alpha
        // to the new-area share until it was full, which pushed `local` to 1
        // and understated the residual whenever one bucket carried a head from
        // one parent and a co-located part of another.
        //
        // With D_k == 0 this is the pre-T9 expression to the bit (min() and
        // the ratio agree when one side is empty), and with C_k == 0 both give
        // aRes == A_k exactly, so nothing that predates the fourth plane moves.
        float aCov;
        float aRes;
        if (colo > 0.0f) {
            aRes = a * (colo / (cov + colo));
            aCov = a - aRes;

            // aCov <= cov MUST hold, and it must hold in `aCov` itself rather
            // than only in the `local` clamp below (reviewer, M1.P3.T9).  The
            // alpha the fit/excess branches emit is capped by `local`, but the
            // COLOUR beside it is scaled by covShare = aCov/a, which is not —
            // so a clamped `local` alone emits colour for alpha it did not
            // add, i.e. a premultiplied colour:alpha ratio above 1.  That is
            // character-for-character the defect M1.P3.T8's review fixed on
            // the residual term (0.8748 against a true 0.5), and re-clamping
            // here keeps the two in lockstep by moving the excess into the
            // residual instead of dropping it.  Unreachable on the production
            // path — every deposit that writes alpha writes its `w*vis` into
            // exactly one area plane, so A_k <= C_k + D_k before the [0,1]
            // clamps and clamping either area down only lowers aCov — so this
            // costs nothing and moves no measured number; it is a guard for
            // hand-built planes, float rounding at alpha == 1, and any later
            // caller that breaks the deposit invariant.
            if (aCov > cov) {
                aCov = cov;
                aRes = a - aCov;
            }
        } else {
            aCov = (a < cov) ? a : cov;
            aRes = a - aCov;
        }

        const float resShare = (a > 0.0f) ? (aRes / a) : 0.0f;
        const float covShare = 1.0f - resShare;

        // The tile this bucket leaves behind for the co-located deposits that
        // follow it: `claimT` over `claimA`, the area it covers itself.  A
        // chain this bucket CONTINUES needs no such pair since M1.P3.T21 — its
        // tile is already on the stack and is attenuated there in place.
        // Negative means "this bucket claims no area of its own", which is what
        // keeps a bucket that only carries colour from touching the mosaic.
        float claimT = -1.0f, claimA = 0.0f;
        float claimRingT = -1.0f, claimRingA = 0.0f;

        if (cov > 0.0f) {
            // <= 1 by construction (both branches above bound aCov by cov);
            // the clamp is retained because `local` is a transmittance and a
            // caller-supplied plane must not be able to make it negative.
            const float local  = clampf(aCov / cov, 0.0f, 1.0f);
            const float fit    = (cov < freeArea) ? cov : freeArea;
            const float excess = cov - fit;

            // ---- the share that fits in still-unclaimed area: ADDITIVE ----
            if (fit > 0.0f) {
                const float f          = (fit / cov) * covShare;
                const float claimedOld = claimedArea;
                const float claimedNew = claimedOld + fit;

                accAlpha += fit * local;                // == aCov * fit/cov
                for (int c = 0; c < channelCount; ++c) {
                    const std::ptrdiff_t o = static_cast<std::ptrdiff_t>(c) * pixelCount;
                    outColor[o] += f * src[o];
                }

                tClaimed    = (claimedOld * tClaimed + fit * (1.0f - local)) / claimedNew;
                freeArea   -= fit;
                claimedArea = claimedNew;

                // THE FIT SHARE — AND ONLY IT — REGISTERS A NEW HEAD SUB-AREA
                // (M1.P3.T20).  `fit` is by definition area nothing in front of
                // it covers, so its transmittance afterwards is exactly
                // (1 - local): a genuinely new tile of the mosaic, and the one a
                // co-located deposit of this same parent will land on.  The
                // `excess` share below is NOT a new tile — it lands on area the
                // mosaic already has — so it attenuates the existing head
                // instead of registering one: registering it too double-counts
                // one physical area as two tiles at two stages of the same
                // composite.  RE-TESTED END TO END at T20's review, because the
                // isolated arithmetic argues the other way — on hand-built
                // planes, registering the excess as a tile of area `excess` at
                // tClaimed*(1-local) makes a defocused fragment behind a
                // full-coverage foreground EXACT where the shipped rule reads up
                // to +95% (see THE HEAD TRANSMITTANCE above).  Rendered, it is
                // decisively worse: scene (g) reads g1 K=8 1.082e-02 (against
                // 1.703e-08), g2 4.954e-02, g3 3.200e-02 and g4 0.0825 against
                // the 0.0543 pin.  Pixels decide; the fit-only rule stands, and
                // the excess-regime over-read stays a documented residual.
                claimT      = 1.0f - local;
                claimA      = fit;
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

                const float att = clampf(1.0f - excess * local, 0.0f, 1.0f);
                tClaimed *= att;

                // A layer spread over the whole claimed share also covers
                // every tile of the mosaic — and this bucket's own fit share.
                for (int t = 0; t < tileCount; ++t)
                    tileT[t] *= att;
                if (claimT >= 0.0f)
                    claimT *= att;
            }
        }

        // ---- the co-located residual: claims NO new area ------------------
        if (resShare > 0.0f) {
            // THE DIVISOR IS THE FOURTH PLANE, not the running claimed area.
            // D_k is the area these very deposits covered, so aRes/D_k is
            // their own per-unit-area opacity — a_p for a split parent's part
            // p — at ANY radius, where aRes/claimedArea was only the same
            // number when the co-located layer rasterised the head's kernel.
            // claimedArea remains the fallback for planes built without a
            // fourth (hand-built test planes, and any pre-T9 caller).
            const float resArea = (colo > 0.0f) ? colo : claimedArea;

            // Attenuated by ITS OWN HEAD'S sub-area transmittance, not by the
            // pooled mean over everything claimed (M1.P3.T20).  A co-located
            // layer is BEHIND the head that claimed the area it sits on, and
            // behind nothing else at this pixel by construction — the pooled
            // mean folds in area belonging to OTHER parents, which is what made
            // a depth ramp lose up to 38.9% of an alpha<1 surface.
            //
            // ALLOCATED ACROSS THE MOSAIC BY AREA, NEWEST TILE FIRST
            // (M1.P3.T21).  `resArea` of co-located area arrived; it lands on
            // the tiles the chains in front of it left, and only what does not
            // fit on those lands on the tile THIS bucket just claimed.  Newest
            // first because the newest open chain is the one a bucket's own
            // residual continues — that is the dense depth ramp, where bucket k
            // carries fragment k's head and fragment k-1's rear and the rear
            // must see its own head rather than a fresher one.  Each tile is
            // then attenuated by the share of the residual that landed ON IT,
            // so two parents' chains stop occluding each other.
            //
            // Normalised by the area actually allocated, not by `resArea`: a
            // rear part's disc can be denser than its head's (THE FOURTH PLANE
            // above), so `resArea` can exceed everything claimed, and dividing
            // by it would silently DROP the overhanging alpha rather than
            // attenuate it.  When nothing has claimed any area at all, nothing
            // is allocated and `tHeadIn` is exactly 1, which is the same
            // unoccluded deposit the pre-T9 `claimed == 0` branch made — bit
            // for bit.
            float need = resArea;
            float tSum = 0.0f;
            float aSum = 0.0f;
            int   lastTile = tileCount;         // tiles [lastTile, tileCount) took some
            float lastTake = 0.0f;              // ...and the OLDEST of them took this
            for (int t = tileCount - 1; t >= 0 && need > 0.0f; --t) {
                const float s = (tileA[t] < need) ? tileA[t] : need;
                if (!(s > 0.0f))
                    continue;
                tSum += s * tileT[t];
                aSum += s;
                need -= s;
                lastTile = t;
                lastTake = s;
            }
            // The overflow — and, when no chain is open, the whole of it —
            // lands on this bucket's own fit share.  That is the M1.P3.T13/T15
            // same-pixel collision shape, where a group's non-head fragment
            // deposits its co-located area into the very bucket the group's
            // head claimed.
            float claimTake = 0.0f;
            if (claimT >= 0.0f && need > 0.0f && claimA > 0.0f) {
                claimTake = (claimA < need) ? claimA : need;
                tSum += claimTake * claimT;
                aSum += claimTake;
                need -= claimTake;
            }
            const float tHeadIn = (aSum > 0.0f) ? (tSum / aSum) : 1.0f;

            accAlpha += aRes * tHeadIn;
            for (int c = 0; c < channelCount; ++c) {
                const std::ptrdiff_t o = static_cast<std::ptrdiff_t>(c) * pixelCount;
                outColor[o] += resShare * tHeadIn * src[o];
            }

            if (resArea > 0.0f) {
                const float resLocal = clampf(aRes / resArea, 0.0f, 1.0f);

                // EACH TILE THE RESIDUAL REACHED LOSES `resLocal` — its
                // OWN per-unit opacity `aRes / D_k`, which THE FOURTH PLANE
                // above shows is exactly a split parent's `a_p` at any radius.
                // A tile it never reached is untouched, which is what stops two
                // overlapping parents' chains from occluding each other.
                for (int t = lastTile + 1; t < tileCount; ++t)
                    tileT[t] = clampf(tileT[t] * (1.0f - resLocal), 0.0f, 1.0f);

                // THE OLDEST TILE REACHED MAY BE ONLY PARTLY COVERED, AND IT
                // SPLITS RATHER THAN AVERAGING.  Behind focus a parent's parts
                // rasterise ever WIDER discs, so each part's per-pixel weight is
                // smaller than its head's and every residual covers only a core
                // of the tile in front of it.  Scaling `resLocal` by the covered
                // share instead — treating the tile as one uniform area — is
                // algebraically M1.P3.T9's rejected `claimedArea` divisor and
                // moves that task's pinned behind-focus residue from 61.00% to
                // 70.53% (measured here, matching what T9 and M1.P3.T17
                // recorded).  Splitting keeps the covered core and the
                // uncovered ring as separate tiles: the core carries the
                // occlusion forward for the parts still to come — which is the
                // number T9 pinned, bit for bit — while the ring keeps its own
                // transmittance for anything wide enough to reach it, which is
                // what the single carried tile used to throw away.
                if (lastTile < tileCount) {
                    const float ring = tileA[lastTile] - lastTake;
                    // The split needs a free slot and MUST NOT make one by
                    // merging: mergeOldestHeadTiles() renumbers the stack, and
                    // `lastTile` was resolved before it.  A full stack takes
                    // the whole-tile branch instead, which over-occludes the
                    // ring — downward, the direction the contract permits.
                    if (ring > 0.0f && lastTake > 0.0f
                        && tileCount < kCompositeHeadTiles) {
                        for (int t = tileCount; t > lastTile; --t) {
                            tileT[t] = tileT[t - 1];
                            tileA[t] = tileA[t - 1];
                        }
                        ++tileCount;
                        tileA[lastTile]     = ring;             // uncovered: T unchanged
                        tileA[lastTile + 1] = lastTake;
                        tileT[lastTile + 1] =
                            clampf(tileT[lastTile + 1] * (1.0f - resLocal), 0.0f, 1.0f);
                    } else if (lastTake > 0.0f) {
                        tileT[lastTile] =
                            clampf(tileT[lastTile] * (1.0f - resLocal), 0.0f, 1.0f);
                    }
                }

                // The share that landed on THIS bucket's own claim splits the
                // same way; both halves are pushed at the bottom of the loop,
                // uncovered first so the covered core stays the newest tile.
                if (claimTake > 0.0f && claimA > 0.0f) {
                    claimRingA = claimA - claimTake;
                    claimRingT = claimT;
                    claimA     = claimTake;
                    claimT     = clampf(claimT * (1.0f - resLocal), 0.0f, 1.0f);
                }

                // NOT min(aRes, resArea): see the header block above for why
                // that clamp destroyed a split parent's rear parts.  resLocal
                // stays clamped because it is a transmittance, not an alpha.
                if (claimedArea > 0.0f) {
                    // SUBTRACTIVE AND AREA-WEIGHTED (M1.P3.T20).  Only the
                    // sub-area `resArea` loses transmittance, and it loses
                    // `resLocal` of its OWN `tHeadIn`, so the claimed mean drops
                    // by exactly (resArea * tHeadIn * resLocal) / claimedArea ==
                    // (aRes * tHeadIn) / claimedArea — the same quantity the
                    // alpha above gained, which is what makes the two telescope.
                    // The multiplicative `*= (1 - resLocal)` this replaces
                    // occluded the WHOLE claimed area with one parent's part.
                    tClaimed = clampf(tClaimed - (aRes * tHeadIn) / claimedArea,
                                      0.0f, 1.0f);
                } else {
                    // Nothing had claimed any area, so this layer is the first
                    // thing at this pixel: it claims its OWN area and becomes
                    // what the parts behind it are occluded by.  Before the
                    // fourth plane there was no area to claim and no opacity
                    // to derive, so this branch left the transmittance at 1
                    // and EVERY later part of the same parent also arrived
                    // unoccluded — the `claimed == 0` back-field over-count.
                    // Claiming here is what closes the telescope over the
                    // parts that do reach this pixel.
                    const float claim = (resArea < freeArea) ? resArea : freeArea;
                    claimedArea = claim;
                    freeArea   -= claim;
                    tClaimed    = 1.0f - resLocal;

                    // It is also the mosaic's first tile — nothing had claimed
                    // any area, so the stack is empty and this is a plain push.
                    tileT[0]  = clampf(tHeadIn * (1.0f - resLocal), 0.0f, 1.0f);
                    tileA[0]  = resArea;
                    tileCount = 1;
                }
            }
        }

        // PUSH THIS BUCKET'S OWN TILE, NEWEST LAST (M1.P3.T21).  T20 carried a
        // single tile and had to CHOOSE here between the area this bucket
        // claimed and the chain it continued, and discarding either is free
        // only while that one has no deposits left.  Two multi-part parents at
        // OVERLAPPING depth ranges both have deposits left, which is what read
        // up to +18.3% HIGH.  Both survive now: the chain's tiles were
        // attenuated in place above, and the claim goes on top of them.
        //
        // NEWEST LAST is the whole of the ordering rule, and it is what the
        // discarded T20 branch got right on a depth ramp: the residual arriving
        // in the next bucket is the rear of the head THIS bucket just claimed,
        // so it must be allocated from this end first.  T20's other branch --
        // the residual sitting on the very tile this bucket claimed, i.e. the
        // M1.P3.T13/T15 same-pixel collision shape -- is now the OVERFLOW case
        // in the allocation above, and reads bit-identically.
        //
        // The stack is bounded, so a pixel deep enough to overflow it merges
        // its two OLDEST tiles by area -- the pooled behaviour T20 had for all
        // of them, applied to the chains least likely to still be open, since a
        // residual is allocated from the newest end.  Area is conserved by the
        // merge, so the mosaic never gains or loses any.
        if (claimRingT >= 0.0f && claimRingA > 0.0f) {
            if (tileCount == kCompositeHeadTiles)
                mergeOldestHeadTiles(tileT, tileA, tileCount);
            tileT[tileCount] = claimRingT;
            tileA[tileCount] = claimRingA;
            ++tileCount;
        }
        if (claimT >= 0.0f && claimA > 0.0f) {
            if (tileCount == kCompositeHeadTiles)
                mergeOldestHeadTiles(tileT, tileA, tileCount);
            tileT[tileCount] = claimT;
            tileA[tileCount] = claimA;
            ++tileCount;
        }

        // The mosaic joins the early-out: the claimed mean can round to zero
        // while one tile's own sub-area still transmits, and a residual behind
        // it would then be dropped rather than attenuated.
        if (!(freeArea > 0.0f) && !(tClaimed > 0.0f)) {
            bool tileOpen = false;
            for (int t = 0; t < tileCount; ++t)
                if (tileT[t] > 0.0f) { tileOpen = true; break; }
            if (!tileOpen)
                break;                  // fully opaque: nothing behind shows
        }
    }

    // THE COLOUR IS RESCALED WITH THE ALPHA, NOT LEFT BEHIND (M1.P3.T4 review).
    // accAlpha CAN exceed 1 on the production path: a pixel carrying several
    // co-located volumetric residuals attenuates by resLocal = aRes/D_k, which
    // is weaker than the alpha each of them adds whenever D_k > aRes, so the
    // sum over buckets is not bounded by 1 the way the per-bucket terms are.
    // Measured over 300 randomised fields through the real flatten + scatter +
    // saturate path (24x24 band, K in [2,24], focus in [0.8, 60]): 970 of
    // 172800 pixels came out above 1, worst 1.6598 -- and with the alpha
    // clamped and the colour not, that pixel shipped premultiplied colour
    // 0.9959 against an honest 0.5975, i.e. +66% too bright.  Overlapping
    // volumetric fog reaches it easily; it is not a hand-built-planes case.
    //
    // Scaling both by the same factor is what the node already does one pass
    // earlier in saturateBucketPixel() and is DOWN-ONLY, so it fabricates no
    // coverage and leaves the honest-alpha contract (and validation scene
    // (i)'s deficit) untouched.  It is a no-op wherever accAlpha <= 1, which
    // is every identity documented above -- all of them are bit-unchanged.
    // FOURTH occurrence of "clamp one of a premultiplied pair and not the
    // other" in this phase (Decisions, 2026-07-27).
    const float outA = clampf(accAlpha, 0.0f, 1.0f);
    if (accAlpha > outA && accAlpha > 0.0f) {
        const float s = outA / accAlpha;
        for (int c = 0; c < channelCount; ++c)
            outColor[static_cast<std::ptrdiff_t>(c) * pixelCount] *= s;
    }
    *outAlpha = outA;
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
//   2. compositePixelCoveragePartition(), the bucket composite (M1.P3.T17).
//
// outColor is `channelCount` planes of `pixelCount` floats
// (outColor[c*pixelCount + i]); outAlpha is one.  Both are OVERWRITTEN.
// `planes` is modified in place by the saturation pass.
// ---------------------------------------------------------------------------
void resolveBandCPU(const ScatterParams& params,
                    BucketPlanes&        planes,
                    float* __restrict__  outColor,
                    float* __restrict__  outAlpha);

// ===========================================================================
//
//  PER-BAND LAZY-CLAIM CONCURRENCY (M1.P4.T1)
//
//  The node's frame is computed lazily, one horizontal band at a time, by
//  whichever of Nuke's render threads asks for a row in that band first.  The
//  state machine lives HERE, NDK-free, so it is unit-testable with plain
//  std::thread (the same property scatterBandCPU has); the node instantiates
//  BandLedger<DD::Image::SignalLock> and the tests instantiate it over a
//  std::mutex/std::condition_variable monitor.  The two are the SAME code —
//  what the tests pin is what ships.
//
//  Everything below is control state, not pixel data: the shared flat frame
//  itself stays on the node side.  The ledger only says who may write which
//  band and when a reader may copy rows out.
//
// ===========================================================================

// ---------------------------------------------------------------------------
// bandBudgetBytes — the memory-limit knob's COMBINED per-band figure
//
// The design reference's bucket-plane formula alone under-budgets by an order
// of magnitude (milestone Decisions, M1.P3.T1): the SoA fragment buffers are
// the larger term at 4K.  The combined figure is
//
//   K*W*B*(C+3)*4                bucket planes: colour + alpha + the two area
//                                planes (M1.P3.T9)
// + (K+1)*W*B*4                  the holdout transmittance LUT, when a holdout
//                                is connected — the term M1.P3.T3 left out of
//                                bytesForBand(), measured 17.0 MB per 4096x64
//                                band at K=16 (17*4096*64*4 = 17,825,792 B;
//                                the LUT dominates the holdout side's cost and
//                                its size is spp-independent)
// + fragments * 100 B            the SoA fragment stream at its RESIDENT cost:
//                                61 B/fragment logical since M1.P3.T10 dropped
//                                the boundary pair, ~100 B resident with
//                                PodBuffer's geometric capacity slack
//                                (milestone Decisions, 2026-07-27)
//
// `fragmentEstimate` is the caller's own forecast of the band's fragment
// count.  The node derives it from the depth-range pass's per-row sample
// counts over the band's FETCH window (band +/- padY — the fetch rows are
// what get flattened, not just the band's own rows), which over-counts
// alpha<=0 samples the flatten drops and under-counts volumetric splits; both
// errors are small against the 100-vs-61 resident margin already folded in.
// ---------------------------------------------------------------------------
constexpr double kSoAResidentBytesPerFragment = 100.0;

inline double bandBudgetBytes(int bucketCount, int channelCount, int width,
                              int height, bool holdoutConnected,
                              double fragmentEstimate)
{
    double bytes = static_cast<double>(
        BucketPlanes::bytesForBand(bucketCount, channelCount, width, height));
    if (holdoutConnected && bucketCount > 0 && width > 0 && height > 0) {
        bytes += static_cast<double>(bucketCount + 1)
               * static_cast<double>(width)
               * static_cast<double>(height) * 4.0;
    }
    if (fragmentEstimate > 0.0)
        bytes += fragmentEstimate * kSoAResidentBytesPerFragment;
    return bytes;
}

// ---------------------------------------------------------------------------
// planBands — band height + concurrent-band cap from the memory limit
//
// The design reference's policy, in one testable place:
//   * start from B = clamp(2*maxRadius, 32, 256) (the caller passes that in,
//     already clamped to the frame height);
//   * if even ONE band busts the limit, SHRINK B (halve, floor 1 row) until it
//     fits — the limit is honoured by making bands smaller, not by refusing to
//     render;
//   * the cap on CONCURRENT in-flight bands is then limit / bytes(B), floored
//     at 1 — never 0, so one band can always be in flight and nothing can
//     deadlock waiting for a slot that cannot exist.
//
// `fragmentsForBandHeight(b)` returns the caller's WORST-CASE per-band
// fragment estimate at band height b (worst over the frame's bands, since the
// cap is one number for all of them).
// ---------------------------------------------------------------------------
struct BandPlan {
    int bandHeight  = 1;
    int bandCount   = 0;
    int maxInFlight = 1;
};

template <typename FragmentsForBandHeight>
inline BandPlan planBands(double memoryLimitBytes,
                          int    frameHeight,
                          int    bucketCount,
                          int    channelCount,
                          int    width,
                          bool   holdoutConnected,
                          int    initialBandHeight,
                          FragmentsForBandHeight&& fragmentsForBandHeight)
{
    BandPlan plan;
    if (frameHeight <= 0 || width <= 0) {
        plan.bandHeight  = 1;
        plan.bandCount   = 0;
        plan.maxInFlight = 1;
        return plan;
    }

    int b = initialBandHeight;
    if (b < 1)
        b = 1;
    if (b > frameHeight)
        b = frameHeight;

    double bytes = bandBudgetBytes(bucketCount, channelCount, width, b,
                                   holdoutConnected, fragmentsForBandHeight(b));
    while (b > 1 && bytes > memoryLimitBytes) {
        b = (b / 2 > 0) ? b / 2 : 1;
        bytes = bandBudgetBytes(bucketCount, channelCount, width, b,
                                holdoutConnected, fragmentsForBandHeight(b));
    }

    plan.bandHeight = b;
    plan.bandCount  = (frameHeight + b - 1) / b;

    // Floor 1: even a band over the limit gets its one slot (the shrink above
    // already did what it could), so the ledger can never deadlock at 0.
    int cap = 1;
    if (bytes > 0.0 && memoryLimitBytes > bytes) {
        const double slots = memoryLimitBytes / bytes;
        cap = (slots >= 2.0) ? static_cast<int>(slots) : 1;
    }
    if (cap > plan.bandCount)
        cap = plan.bandCount;
    if (cap < 1)
        cap = 1;
    plan.maxInFlight = cap;
    return plan;
}

// ---------------------------------------------------------------------------
// BandLedger — per-band Dirty -> InProgress -> Done claim/wait state machine
//
// MonitorT contract (DD::Image::SignalLock satisfies it verbatim; the unit
// tests provide a std::mutex + std::condition_variable equivalent):
//
//   void lock();                       // plain, non-recursive mutex
//   void unlock();
//   bool wait(unsigned long ms = 0);   // atomically release + sleep +
//                                      // reacquire; 0 = no timeout; spurious
//                                      // wakeups allowed (every wait here is
//                                      // in a recheck loop)
//   void signal();                     // broadcast to ALL waiters
//
// PROTOCOL, node side (engine(), one loop per row request):
//
//   1. beginRead(key): succeeds iff frame setup is Done for `key`.  The FAST
//      path is two atomics and no lock at all — this is what replaced the
//      serial phase's per-row frame-wide lock acquisition (~2160 per thread
//      per 4K frame).  While reading, bandDone(band) says whether the row's
//      band is published; if so, copy rows and endRead().
//   2. Otherwise acquireBand(key, band): blocks while the band is InProgress
//      (or while the in-flight cap is full), and returns
//        Compute — the caller now OWNS the band: compute it into private
//                  bucket planes, write its disjoint region of the shared
//                  frame, then completeBand() (or abandonBand() on abort);
//        Ready   — another thread finished it while we waited;
//        Aborted — abortedFn() went true while waiting;
//        Stale   — the setup key no longer matches: go back to beginFrame().
//   3. beginFrame(key): the same claim pattern for the FRAME-GLOBAL setup
//      (depth range, buckets, kernel LUT, band decomposition, the shared
//      frame allocation).  SetupCompute's owner must call endFrameSetup().
//      A setup claim QUIESCES first: it waits until no band is in flight and
//      no reader is mid-copy, because setup reallocates what they touch.
//
// ABORT: a computing thread that sees Op::aborted() calls abandonBand() — the
// band goes back to Dirty (NEVER Done), every waiter is woken (they re-test
// abortedFn and leave), and the band's frame region keeps whatever it had
// (erased/black rows).  Nothing stale is ever published: Done is only ever
// set by completeBand() from the thread that just wrote the band under the
// CURRENT setup key, and a setup re-run cannot start while that thread is in
// flight.
//
// _validate's half is invalidate(): on an Op::hash() change it marks every
// non-in-flight band Dirty and forces the next engine() through beginFrame(),
// whose owner re-runs setup and resets everything under the new key.  Cheap —
// no compute, no waiting.
//
// MEMORY ORDER: band states are std::atomic so the read fast path needs no
// lock.  completeBand() stores Done with release AFTER the band's frame
// region is written; bandDone() loads with acquire before the row copy, so
// the copy sees the whole band.  Everything else is monitor-guarded.
// ---------------------------------------------------------------------------
enum class BandState : std::uint8_t {
    Dirty      = 0,
    InProgress = 1,
    Done       = 2
};

enum class FrameClaim : std::uint8_t {
    SetupCompute,   // caller owns setup; MUST call endFrameSetup()
    Ready,          // setup already Done for this key
    Aborted         // abortedFn() returned true
};

enum class BandClaim : std::uint8_t {
    Compute,        // caller owns the band; MUST completeBand()/abandonBand()
    Ready,          // band Done
    Stale,          // setup key changed under us — return to beginFrame()
    Aborted         // abortedFn() returned true (or band index invalid)
};

template <typename MonitorT>
class BandLedger {
public:
    // `key` is the frame identity (the node passes Op::hash().value()).  There
    // is no reserved key value: an all-ones key merely never takes the read
    // FAST path (kNoFastKey collides with it), it still works via the locked
    // path.
    static constexpr std::uint64_t kNoFastKey = ~0ULL;

    // ----- frame setup claim ------------------------------------------------
    template <typename AbortedFn>
    FrameClaim beginFrame(std::uint64_t key, AbortedFn&& abortedFn)
    {
        _monitor.lock();
        for (;;) {
            if (_setupDone && _setupKey == key) {
                _monitor.unlock();
                return FrameClaim::Ready;
            }
            if (abortedFn()) {
                _monitor.unlock();
                return FrameClaim::Aborted;
            }
            // Claim setup only when nothing else can be touching the shared
            // frame: no band in flight (their owners hold pointers into it)
            // and, below, no reader mid-copy.
            if (!_setupInProgress && _inFlight == 0) {
                _setupInProgress = true;
                _setupDone       = false;
                // Close the read fast path FIRST; a reader increments
                // _activeReaders before it checks this, so once the store is
                // visible no NEW reader can pass, and the drain below only
                // waits for the ones already copying.
                //
                // SEQ_CST IS LOAD-BEARING on this store and on the reader
                // drain below (and on beginRead()'s increment + key load):
                // "store gate, then load counter" against "add counter, then
                // load gate" is the store-buffer pattern, and with only
                // release/acquire BOTH sides may see the stale value — a
                // reader slipping past a closed gate exactly while the drain
                // reads zero.  The default (seq_cst) ordering forbids it.
                _fastKey.store(kNoFastKey);
                while (_activeReaders.load() != 0)
                    _monitor.wait(1);   // readers don't signal; poll at 1ms
                _monitor.unlock();
                return FrameClaim::SetupCompute;
            }
            _monitor.wait();
        }
    }

    // `ok` false = setup aborted/failed: nothing becomes Ready, the next
    // caller re-claims.  `allBandsDone` publishes an EMPTY frame (no content
    // anywhere — the shared frame is all zeros and every band is immediately
    // servable) without a per-band claim cycle.
    void endFrameSetup(bool ok, std::uint64_t key, int bandCount,
                       int maxInFlight, bool allBandsDone = false)
    {
        _monitor.lock();
        _setupInProgress = false;
        if (ok) {
            if (bandCount < 0)
                bandCount = 0;
            if (bandCount > _bandCapacity) {
                _states.reset(new std::atomic<std::uint8_t>[
                                  static_cast<std::size_t>(bandCount)]);
                _bandCapacity = bandCount;
            }
            for (int i = 0; i < bandCount; ++i) {
                _states[i].store(static_cast<std::uint8_t>(
                                     allBandsDone ? BandState::Done
                                                  : BandState::Dirty),
                                 std::memory_order_relaxed);
            }
            _bandCount   = bandCount;
            _maxInFlight = (maxInFlight < 1) ? 1 : maxInFlight;
            _setupDone   = true;
            _setupKey    = key;
            // Reopen the fast path.  The release store is what makes every
            // write above (band states, count, the caller's frame buffers)
            // visible to a fast-path reader that acquires this key.
            _fastKey.store(key, std::memory_order_release);
        }
        _monitor.signal();
        _monitor.unlock();
    }

    // ----- band claim -------------------------------------------------------
    template <typename AbortedFn>
    BandClaim acquireBand(std::uint64_t key, int band, AbortedFn&& abortedFn)
    {
        _monitor.lock();
        for (;;) {
            if (!_setupDone || _setupKey != key) {
                _monitor.unlock();
                return BandClaim::Stale;
            }
            if (band < 0 || band >= _bandCount) {
                // Caller bug (a row outside every band): surfaces as a black
                // row, never as a hang.
                _monitor.unlock();
                return BandClaim::Aborted;
            }
            const BandState s = static_cast<BandState>(
                _states[band].load(std::memory_order_relaxed));
            if (s == BandState::Done) {
                _monitor.unlock();
                return BandClaim::Ready;
            }
            if (abortedFn()) {
                _monitor.unlock();
                return BandClaim::Aborted;
            }
            if (s == BandState::Dirty && _inFlight < _maxInFlight) {
                _states[band].store(static_cast<std::uint8_t>(BandState::InProgress),
                                    std::memory_order_relaxed);
                ++_inFlight;
                _monitor.unlock();
                return BandClaim::Compute;
            }
            // InProgress, or Dirty with the in-flight cap full: block until a
            // completion/abandon broadcast, then re-test everything.
            _monitor.wait();
        }
    }

    // The claiming thread finished writing the band's region of the shared
    // frame.  The release store publishes those writes to the lock-free
    // read path.
    void completeBand(int band)
    {
        _monitor.lock();
        if (band >= 0 && band < _bandCount
            && _states[band].load(std::memory_order_relaxed)
                   == static_cast<std::uint8_t>(BandState::InProgress)) {
            _states[band].store(static_cast<std::uint8_t>(BandState::Done),
                                std::memory_order_release);
        }
        if (_inFlight > 0)
            --_inFlight;
        _monitor.signal();
        _monitor.unlock();
    }

    // Aborted / failed: back to Dirty — NEVER Done — and wake every waiter so
    // they can re-test abortedFn() and leave their rows black.
    void abandonBand(int band)
    {
        _monitor.lock();
        if (band >= 0 && band < _bandCount
            && _states[band].load(std::memory_order_relaxed)
                   == static_cast<std::uint8_t>(BandState::InProgress)) {
            _states[band].store(static_cast<std::uint8_t>(BandState::Dirty),
                                std::memory_order_relaxed);
        }
        if (_inFlight > 0)
            --_inFlight;
        _monitor.signal();
        _monitor.unlock();
    }

    // ----- _validate's half -------------------------------------------------
    // Op::hash() changed: every non-in-flight band goes Dirty and setup is
    // invalidated, so the next engine() re-runs it under the new key.  Bands
    // still InProgress are left for their owners; the setup re-claim cannot
    // start until they complete or abandon (beginFrame waits for
    // _inFlight == 0), and endFrameSetup then resets every state anyway.
    void invalidate()
    {
        _monitor.lock();
        _setupDone = false;
        _fastKey.store(kNoFastKey);   // seq_cst, same pairing as beginFrame's
        for (int i = 0; i < _bandCount; ++i) {
            if (_states[i].load(std::memory_order_relaxed)
                    == static_cast<std::uint8_t>(BandState::Done)) {
                _states[i].store(static_cast<std::uint8_t>(BandState::Dirty),
                                 std::memory_order_relaxed);
            }
        }
        _monitor.signal();
        _monitor.unlock();
    }

    // ----- the read path ----------------------------------------------------
    // beginRead()/endRead() bracket a row copy out of the shared frame.  The
    // fast path is lock-free: increment the reader count, THEN check the key
    // (that order is what lets a setup claim close the gate and drain).  The
    // slow path takes the monitor once — e.g. for a key that collides with
    // kNoFastKey — and is also what a caller lands on right after computing
    // its own band.
    bool beginRead(std::uint64_t key)
    {
        // Increment FIRST, then check the gate — and both at seq_cst, paired
        // with the setup claim's close-then-drain (see beginFrame): weaker
        // orders admit the store-buffer interleaving where this thread reads
        // the gate still open while the claimant reads the counter still
        // zero.  On x86 the RMW is a lock op anyway and the load is plain,
        // so the fast path stays two cheap atomics and no lock.
        _activeReaders.fetch_add(1);
        if (_fastKey.load() == key)
            return true;
        _activeReaders.fetch_sub(1);

        _monitor.lock();
        if (_setupDone && _setupKey == key) {
            _activeReaders.fetch_add(1);
            _monitor.unlock();
            return true;
        }
        _monitor.unlock();
        return false;
    }

    void endRead()
    {
        _activeReaders.fetch_sub(1);
    }

    // Only meaningful between beginRead() and endRead() (or under the
    // monitor): is this band published?
    bool bandDone(int band) const
    {
        if (band < 0 || band >= _bandCount)
            return false;
        return _states[band].load(std::memory_order_acquire)
            == static_cast<std::uint8_t>(BandState::Done);
    }

    // ----- observers (tests + instrumentation) ------------------------------
    int bandCount() const { return _bandCount; }

    int inFlight()
    {
        _monitor.lock();
        const int n = _inFlight;
        _monitor.unlock();
        return n;
    }

    BandState bandState(int band) const
    {
        if (band < 0 || band >= _bandCount)
            return BandState::Dirty;
        return static_cast<BandState>(
            _states[band].load(std::memory_order_acquire));
    }

private:
    MonitorT _monitor;

    // Guarded by _monitor:
    bool          _setupDone       = false;
    bool          _setupInProgress = false;
    std::uint64_t _setupKey        = 0;
    int           _bandCount       = 0;
    int           _bandCapacity    = 0;
    int           _inFlight        = 0;
    int           _maxInFlight     = 1;

    // Lock-free (the read fast path):
    std::atomic<std::uint64_t> _fastKey{kNoFastKey};
    std::atomic<int>           _activeReaders{0};
    std::unique_ptr<std::atomic<std::uint8_t>[]> _states;
};

} // namespace deepc

#endif // DEEPC_DEFOCUS_SCATTER_H
