// SPDX-License-Identifier: MIT
//
// ============================================================================
//
//  DeepCDefocusScatter — POD scatter core for DeepCDefocus
//
//  This header (and its .cpp) is the CUDA seam.  ABSOLUTELY NO DDImage/NDK
//  type may cross into it: it compiles, and is unit-testable, with a bare
//  `g++ -std=c++17 -fsyntax-only`.  The node's NDK side fetches deep pixels and
//  hands this file plain std::vector<deepc::SampleRecord> and POD parameter
//  structs; everything from there to the flat band output is NDK-free.
//
//  Contents:
//
//    - PodBuffer<T> : thin OWNING wrapper over a 64-byte-aligned host
//                     allocation.  EVERY SoA / plane buffer in this node goes
//                     through it, so a device build swaps only the two static
//                     allocate/deallocate functions (-> cudaMalloc /
//                     cudaFree) and the loop drivers, never the kernel source.
//    - ChannelGroups: the SoA channel-group layout, carrying the
//                     channelRadiusScale[] hook (all 1.0 for the single disc
//                     kernel; a chromatic kernel fills it in).
//    - SampleSoA   : the flattened fragment stream the scatter consumes.
//    - flattenPixelToSoA() : tidy -> volumetric pieces -> CoC -> pre-merge
//                     and collision merge -> SoA append, for one deep pixel.
//    - StreamPlanes  : the band's per-pixel running state, C + 6 planes with
//                      no factor of the depth_layers count.
//    - HoldoutSoA    : non-owning view of the per-dest-pixel boundary
//                      transmittance LUT, plus the HoldoutBoundaries set it
//                      was built at.  Absent/unconnected holdout is an empty
//                      view and costs nothing.
//    - sortFragmentsByDepth() / scatterStreamCPU() / resolveStreamCPU() :
//                      the depth-ordered streaming composite.
//
//  The per-fragment / per-span / per-pixel BODIES of all of the above live in
//  this header marked DEEPC_HD; only the loop drivers and the allocations are
//  in the .cpp.  That split IS the CUDA seam: a .cu translation unit
//  includes this header unchanged and replaces the drivers.
//
//  DEEPC_HD is NOT defined here.  It is owned by DeepCDefocusMath.h and
//  inherited by including that header, so a .cu translation unit that
//  pre-defines the macro gets one consistent expansion no matter which of
//  these headers it includes first.
//
//  Style: scalar, __restrict__-annotated, auto-vectorization-friendly; no
//  intrinsics, no SIMD library, no OpenMP.
//
// ============================================================================

#ifndef DEEPC_DEFOCUS_SCATTER_H
#define DEEPC_DEFOCUS_SCATTER_H

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

// Owns DEEPC_HD, CocParams, FrameDepthRange, HoldoutBoundaries.
#include "DeepCDefocusMath.h"

// deepc::SampleRecord, deepc::tidyOverlapping(), deepc::optimizeSamples().
// Reused rather than reimplemented.
#include "DeepSampleOptimizer.h"

// KernelSampler / KernelView / RowSpan — the scatter consumes the kernel seam's
// precomputed contiguous row spans.  Header-only and NDK-free, like this file.
#include "DeepCDefocusKernel.h"

namespace deepc {

// ---------------------------------------------------------------------------
// PodBuffer<T> — owning, aligned, trivially-copyable-only host buffer
//
// WHY THIS EXISTS.  It is not a std::vector replacement for its own sake: it
// is the allocation seam for a device build.  Every SoA array, state plane
// and holdout LUT this node allocates goes through PodBuffer, so a CUDA build
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
                  "and a device allocator never runs constructors.");
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
    // ----- THE ALLOCATION SEAM ----------------------------------------------
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

    // Behind one function so a device build can turn it into a cudaMemcpy
    // without touching any call site.
    static void copyBytes(void* dst, const void* src, std::size_t bytes) noexcept
    {
        std::memcpy(dst, src, bytes);
    }

    // Likewise for assign()'s fill.  It is here, and not inlined into assign(),
    // for the same reason copyBytes() is: it DEREFERENCES the buffer from the
    // host.  Swapping allocateBytes() alone to a plain cudaMalloc would leave
    // this (and every caller of operator[]/begin()/end()) writing to device
    // memory from host code, so a device build either uses managed/pinned
    // memory or replaces this with a cudaMemset/fill kernel.  "Two functions know where
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
// ChannelGroups — the SoA channel-group layout + the radius-scale hook
//
// Channels are partitioned into groups that share a kernel.  The disc kernel
// uses exactly one group covering every channel with radiusScale 1.0
// (makeSingleChannelGroup); the array exists so that a chromatic kernel is a
// data change, not a structural one.
//
// The hook is consumed via groupRadius(): the SoA stores ONE base radius per
// fragment and the scatter multiplies it by the group's scale when it asks the
// KernelSampler for a kernel.  Storing per-group radii per fragment instead
// would multiply the SoA's per-fragment cost by the group count for no
// benefit while there is one group.
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
// FragmentKind — whether a fragment came from a point sample or is a piece of
// a volumetric one (volumetricPieceBounds).  Every fragment is one deposit
// either way; the kind is carried for diagnostics and tests.
// ---------------------------------------------------------------------------
enum class FragmentKind : std::uint8_t {
    Point      = 0,
    Volumetric = 1
};

// The per-fragment flag byte: the kind, and the sign of the signed CoC.  The
// SoA stores |CoC| as the radius and the sign here, so the jump rotation can
// tell a fragment in front of focus from one behind it at the same radius.
constexpr std::uint8_t kFragmentKindMask       = 0x01;
constexpr std::uint8_t kFragmentNegativeCocBit = 0x02;

static_assert(static_cast<std::uint8_t>(FragmentKind::Volumetric) <= kFragmentKindMask,
              "FragmentKind no longer fits in kFragmentKindMask");
static_assert((kFragmentKindMask & kFragmentNegativeCocBit) == 0,
              "the kind mask and the CoC sign bit overlap");

DEEPC_HD inline std::uint8_t packFragmentFlags(FragmentKind kind, bool cocNegative)
{
    return static_cast<std::uint8_t>(
        (static_cast<std::uint8_t>(kind) & kFragmentKindMask)
        | (cocNegative ? kFragmentNegativeCocBit : std::uint8_t{0}));
}

DEEPC_HD inline FragmentKind fragmentKindOf(std::uint8_t flags)
{
    return static_cast<FragmentKind>(flags & kFragmentKindMask);
}

DEEPC_HD inline bool fragmentCocNegativeOf(std::uint8_t flags)
{
    return (flags & kFragmentNegativeCocBit) != 0;
}

DEEPC_HD inline float fragmentSignedCoc(std::uint8_t flags, float radius)
{
    return fragmentCocNegativeOf(flags) ? -radius : radius;
}

// THE MINIMUM KERNEL DIAMETER IS 1 PIXEL, stated as its radius. At or below
// it the scatter takes the sharp fast path — the fragment deposits into its
// own pixel only, which IS the 1x1 delta kernel — and above it the
// scatter blends the two grid nodes bracketing the radius.
// Defined here so the flatten, the scatter and the LUT's rMin contract all
// read the same number; the flatten itself does not branch on it.
//
// The floor is FIXED at 0.5 px rather than tracking the LUT's own smallest
// entry, and the floor itself belongs to the sharp path: at `edge_softness`
// above 1 the LUT's r=0.5 entry is not a delta (at 2.0 its centre row is
// [0.1301, 0.3903, 0.1301]) while the sharp path deposits a literal single
// pixel regardless.  Routing d = 1 through the sharp path keeps every
// d <= 1 the same kernel at any softness; the step between the delta and
// the first soft LUT entry just above d = 1 remains, a known limitation of
// `edge_softness` above 1.
constexpr float kSharpRadiusPx = 0.5f;

// ---------------------------------------------------------------------------
// scatterKernelBin — "would the scatter rasterise these two radii identically?"
//
// The scatter rasterises a radius as (1 - f) * K[A] + f * K[B] with
// (A, B, f) = kernelGridBracket(radius) — scatterStreamCPU() for fragments,
// scatterBackgroundCPU() for the residual — so the rasterised kernel is a
// function of (A, f) and of nothing else.  The bin is the lattice cell
// (A, floor(f * 2^20)).  Equal bins therefore mean the same node pair and
// blend weights within 2^-20 of each other, and since every LUT weight lies
// in [0, 1] the two rasterisations differ by less than 2^-20 < 1e-6 per
// pixel at ANY edge_softness.  That is the bound the flatten's absorb needs,
// and no coarser cell is sound: at edge_softness 0 the nodes at 0.998 and
// 1.0 px differ by 0.8 in one weight.  Conversely one radius ulp moves f by
// at least 2^-19 everywhere on the grid (a bracket is never wider than 0.5 px,
// nor than r^2/512 below 16 px), so distinct radii never share a cell: in
// practice "same bin" means "same radius".  Both directions are deliberate.
// This predicate never answers "same" for a pair that rasterises differently;
// it may answer "different" for a pair that does not (two radii the LUT clamps
// onto one entry, say), which costs an absorb and never correctness.
//
// At or below kSharpRadiusPx — NaN included, exactly as the scatter tests it
// — the kernel is one weight of 1.0 at the fragment's own pixel, so every
// sharp radius is one bin.
//
// `pre_merge` does not consult this predicate: it groups on `merge_tolerance`
// (0.25 px by default) within one side of focus and rasterises the group at
// the radius of its depth union's midpoint, which lies between its members'
// radii and is lossy whenever those differ at all (harness check `i7`).
//
// With several channel groups a group's radius is
// `groupRadius(groups, g, baseRadius)`, and equal BASE bins do not imply equal
// bins after a per-group `channelRadiusScale != 1`.  Every scale is currently
// 1.0, so the base bin IS every group's bin.
// ---------------------------------------------------------------------------
constexpr int kScatterKernelBlendBits = 20;

DEEPC_HD inline std::int64_t scatterKernelBin(float radiusPx)
{
    if (!(radiusPx > kSharpRadiusPx))       // also catches NaN, as the scatter does
        return -1;                          // the sharp one-pixel kernel
    const KernelGridBracket br    = kernelGridBracket(radiusPx);
    const float             cells = static_cast<float>(1 << kScatterKernelBlendBits);
    const std::int64_t      cell  = static_cast<std::int64_t>(br.frac * cells);
    return (static_cast<std::int64_t>(br.indexA) << kScatterKernelBlendBits) + cell;
}

DEEPC_HD inline bool sameScatterKernel(float a, float b)
{
    return scatterKernelBin(a) == scatterKernelBin(b);
}

// Two fragments of one source pixel with the same kernel bin on the same side
// of focus see the same lens patch from every destination pixel, so the rear
// one is exactly behind the front one and `over` is the physical answer.
// Opposite sides of focus mirror the patch; every sharp radius sees the whole
// lens.
DEEPC_HD inline bool sameLensPatch(float signedA, float signedB)
{
    const float a = (signedA < 0.0f) ? -signedA : signedA;
    const float b = (signedB < 0.0f) ? -signedB : signedB;
    if (!sameScatterKernel(a, b))
        return false;
    if (!(a > kSharpRadiusPx))
        return true;
    return (signedA < 0.0f) == (signedB < 0.0f);
}

// ---------------------------------------------------------------------------
// FragmentRecord — one fragment as appended to the SoA: a post-tidy,
// post-cut, post-merge piece of one source pixel's samples, deposited once.
// ---------------------------------------------------------------------------
struct FragmentRecord {
    int          x           = 0;       // source pixel, absolute image coords
    int          y           = 0;
    float        radius      = 0.0f;    // |signed CoC|, clamped, base group
    float        depth       = 0.0f;    // the stream's sort key
    float        alpha       = 0.0f;
    FragmentKind kind        = FragmentKind::Point;
    bool         cocNegative = false;   // in front of focus

    // This fragment's slice of the source pixel's unit area, in front-to-back
    // arrival order: share = t * alpha, t *= (1 - alpha), so a pixel's shares
    // plus its final residual t sum to 1.  The merges sum it and never
    // rescale it, or the partition stops summing to 1.  The pieces of one
    // volumetric parent pool their shares onto the DEEPEST piece, so the cut
    // cannot move the parent's arrival claim off the radius its residual
    // scatters at.
    float        share       = 0.0f;
};

// ---------------------------------------------------------------------------
// SampleSoA — the flattened fragment stream
//
// One array per attribute, so the scatter streams only what it reads and a
// device build can upload them independently.  Channel values are the one
// interleaved array (`color[i*channelCount + c]`): a fragment's channels are
// consumed together as scalars, and a planar layout's stride would be the
// fragment count, unknown until the band is flattened.
//
// x/y are absolute image coordinates, so the SoA does not depend on the band
// decomposition.  No holdout boundary pair is stored: the LUT carries its own
// boundary set and the scatter locates `depth` in it in O(1).
//
// 4 * 6 + 1 + 4 * C bytes per fragment: 37 B at C = 3.
// ---------------------------------------------------------------------------
struct SampleSoA {
    PodBuffer<std::int32_t> x;
    PodBuffer<std::int32_t> y;
    PodBuffer<float>        radius;
    PodBuffer<float>        depth;
    PodBuffer<float>        alpha;
    PodBuffer<float>        arrivalShare;   // FragmentRecord::share

    // Read through fragmentKindOf() / fragmentCocNegativeOf().
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

    std::size_t sizeBytes() const;

    // Size 0, capacity kept (band-to-band reuse).
    void clear();

    void release();

    // Prepare for a band: sets the channel layout and empties the arrays.
    void begin(int channelCountIn, const ChannelGroups& groupsIn);

    void reserveFragments(std::size_t count);

    // `channels` must point at channelCount premultiplied values.
    void appendFragment(const FragmentRecord& f, const float* __restrict__ channels);
};

// ---------------------------------------------------------------------------
// Volumetric pieces
//
// A volumetric sample is cut into pieces, each drawn at its midpoint radius,
// so a piece is off by at most half its CoC extent.  The pre-merge already
// accepts drawing a member merge_tolerance from its group's radius, so a
// step of 2 * merge_tolerance stays in that error class; it is widened to
// the frame's CoC variation / K so no span inside the frame's range is cut
// into more than K + 1 pieces.
// ---------------------------------------------------------------------------
constexpr int kMaxVolumetricPieces = FrameDepthRange::kMaxLayers + 1;

inline float volumetricPieceStepPx(const CocParams& coc, const FrameDepthRange& range,
                                   float mergeTolerancePx)
{
    const float rNear = signedCocPixels(coc, range.depthMin);
    const float rFar  = signedCocPixels(coc, range.depthMax);
    const float variation = ((rNear < 0.0f) == (rFar < 0.0f))
                          ? std::fabs(rFar - rNear)
                          : (std::fabs(rNear) + std::fabs(rFar));
    const float tol     = (mergeTolerancePx > 0.125f) ? mergeTolerancePx : 0.125f;
    const float tolStep = 2.0f * tol;
    const float capStep = (range.K > 0) ? variation / static_cast<float>(range.K) : 0.0f;
    return (capStep > tolStep) ? capStep : tolStep;
}

struct VolumetricPiece {
    float zFront     = 0.0f;
    float zBack      = 0.0f;
    float t          = 1.0f;    // share of the parent's thickness
    float alpha      = 0.0f;    // partitionAlpha(parent alpha, t)
    float colorScale = 1.0f;    // partitionColorScale(parent alpha, t)
};

// ---------------------------------------------------------------------------
// volumetricPieceBounds — cut [zFront, zBack] into pieces of at most stepPx
// of CLAMPED CoC each, with the focal plane as a cut of its own
//
// CoC is affine in 1/z on each side of focus, so the cuts are uniform in 1/z
// there; targets are placed in clamped CoC and inverted through the
// unclamped law, which puts any max_radius plateau inside a single piece
// instead of spending the budget where the radius cannot vary.  At most
// maxPieces pieces are written: a side that would need more gets what is
// left of the budget (its pieces then exceed the step).  Point samples and
// non-finite spans come back as one piece with t = 1, and so does a span
// whose two ends are within the sharp radius (CoC grows away from focus, so
// the whole span is): every piece would take the one sharp kernel, and a cut
// would only cost a sharp pixel its bit-exact flatten.  The pieces'
// t telescope, so they sum to 1 to within one rounding per piece.
// ---------------------------------------------------------------------------
inline int volumetricPieceBounds(const CocParams& coc, float zFront, float zBack, float alpha,
                                 float stepPx, VolumetricPiece* out, int maxPieces)
{
    if (out == nullptr || maxPieces < 1)
        return 0;

    const float a = clampf(alpha, 0.0f, 1.0f);
    if (!(zBack > zFront) || !std::isfinite(zFront) || !std::isfinite(zBack) || !(zFront > 0.0f)) {
        out[0] = VolumetricPiece{zFront, zBack, 1.0f, a, 1.0f};
        return 1;
    }

    if (!(std::fabs(signedCocPixels(coc, zFront)) > kSharpRadiusPx)
        && !(std::fabs(signedCocPixels(coc, zBack)) > kSharpRadiusPx)) {
        out[0] = VolumetricPiece{zFront, zBack, 1.0f, a, 1.0f};
        return 1;
    }

    const float focus = coc._focusDistance;
    float cuts[3] = {zFront, zBack, zBack};
    int   sides   = 1;
    if (focus > zFront && focus < zBack && maxPieces >= 2) {
        cuts[1] = focus;
        sides   = 2;
    }

    const double s       = static_cast<double>(focus);
    const float  step    = (stepPx > 0.0f) ? stepPx : 0.25f;
    const float  invSpan = 1.0f / (zBack - zFront);

    int   count     = 0;
    float partFront = zFront;
    float uPrev     = 0.0f;

    const auto emit = [&](float zb, bool last) {
        const float u = last ? 1.0f : clampf((zb - zFront) * invSpan, 0.0f, 1.0f);
        if (!last && !(u > uPrev))
            return;
        const float t = u - uPrev;
        out[count] = VolumetricPiece{partFront, zb, t, partitionAlpha(a, t), partitionColorScale(a, t)};
        ++count;
        partFront = zb;
        uPrev     = u;
    };

    for (int side = 0; side < sides; ++side) {
        const float  z0    = cuts[side];
        const float  z1    = cuts[side + 1];
        const bool   front = z1 <= focus;
        const double r0    = std::fabs(signedCocPixels(coc, z0));
        const double r1    = std::fabs(signedCocPixels(coc, z1));
        const double k     = static_cast<double>(cocCoefficient(coc, front));

        const int room = maxPieces - count - (sides - 1 - side);
        int n = static_cast<int>(std::ceil(std::fabs(r1 - r0) / static_cast<double>(step)));
        if (n > room) n = room;
        if (n < 1)    n = 1;
        if (!(k > 0.0) || !(s > 0.0)) n = 1;

        for (int j = 1; j < n; ++j) {
            const double r = r0 + (r1 - r0) * (static_cast<double>(j) / n);
            const double denom = front ? (r / k + 1.0) : (1.0 - r / k);
            if (!(denom > 0.0))
                continue;
            const float z = static_cast<float>(s / denom);
            if (z > partFront && z < z1)
                emit(z, false);
        }
        emit(z1, side == sides - 1);
    }
    return count;
}

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
    // It gates ONE thing: how far the pre-merge and the collision merge
    // (see the .cpp) may reach in depth.  That merge is exact for
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
    // this gets an unrestricted merge — a holdout sampled at one depth for the
    // whole group — not a crash or an out-of-range read.
    bool  holdoutConnected   = false;

    // THE FRAME'S HoldoutBoundaries — built once in frameSetup() (uniform in
    // Z over the measured depth range, count from depth_layers) and handed
    // down unchanged, so the flatten's merge brackets and the holdout LUT
    // are located against the one set.  Default is inert (count() == 0),
    // matching a frame with no holdout connected.
    HoldoutBoundaries holdoutBoundaries = {};

    // The volumetric cut: pieces of at most pieceStepPx of CoC
    // (volumetricPieceStepPx), at most maxVolumetricPieces per span
    // (depth_layers + 1).
    float pieceStepPx         = 0.5f;
    int   maxVolumetricPieces = kMaxVolumetricPieces;

    int           channelCount = 0;
    ChannelGroups groups       = {};
};

// ---------------------------------------------------------------------------
// sanitizeFragmentDepth — THE depth sanitiser the flatten applies, exported
//
// It lives here rather than in flattenPixelToSoA()'s translation unit because
// THREE passes have to agree about what a depth means, and two of them
// disagreeing is a live failure class (the holdout's missing ray-distance
// factor, and computeDepthRange() measuring a range the flatten then falls
// below).  The node's depth-range pass calls this; so does the
// flatten; the holdout's appendPixel() calls it too and then applies its own
// extra NaN rule on top (see there — NaN is DROPPED on the holdout side, not
// mapped to 0).
//
//   NaN, -inf -> 0.0f       ("invalid depth": radius 0)
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
    return (v > 0.0f) ? FrameDepthRange::kMaxDepth : 0.0f;
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
//     measured sitting 47.3% too far back in Z at the corner of a
//     20mm / 36x24 frame when this was omitted;
//   * the node's computeDepthRange() — the frame's measured range, which the
//     holdout boundary set and the kernel LUT's extent are built from.  The
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
// FlattenStats — optional instrumentation
//
// Pass nullptr to skip.  All counters are cumulative across pixels so a band
// or a whole frame can share one instance.
// ---------------------------------------------------------------------------
struct FlattenStats {
    std::size_t pixels           = 0;   // pixels with at least one input sample
    std::size_t inputSamples     = 0;   // samples handed in
    std::size_t tidiedSamples    = 0;   // samples after tidyOverlapping()
    std::size_t splitParts       = 0;   // pieces produced by the volumetric cut
    std::size_t stagedFragments  = 0;   // fragments before pre-merge
    std::size_t emittedFragments = 0;   // fragments appended to the SoA
    std::size_t maxSamplesInPixel = 0;
};

// ---------------------------------------------------------------------------
// FlattenScratch — caller-owned per-thread scratch
//
// One instance per band-computing thread, reused across every pixel of the
// band: the staging vector keeps its capacity (and each slot's channel vector
// keeps its own), so nothing in this file allocates per pixel once warmed up.
// deepc::tidyOverlapping() still allocates once per pixel of 2+ samples.
// ---------------------------------------------------------------------------
struct FlattenScratch {
    // One staged fragment: a point sample or one volumetric piece.  Public
    // because tests drive it directly.
    struct Staged {
        float              zFront       = 0.0f;
        float              zBack        = 0.0f;
        float              alpha        = 0.0f;
        float              depth        = 0.0f;
        float              radius       = 0.0f;
        float              signedRadius = 0.0f;
        FragmentKind       kind         = FragmentKind::Point;
        float              share        = 0.0f;   // see FragmentRecord::share
        std::vector<float> channels;
    };

    std::vector<Staged> staged;
    std::size_t         stagedCount = 0;
    std::vector<float>  mergeAccum;          // one merged fragment's colour
};

// ---------------------------------------------------------------------------
// applyProxyScale — scale the pixel-unit knobs into proxy resolution
//
// The mm-denominated physical knobs are resolution-independent for free,
// because CocParams::_formatWidthPx is Nuke's CURRENT (proxy) format width, so
// coc_px already comes out in proxy pixels.  The PIXEL-unit knobs do not:
//
//   * max_radius — the radius CLAMP.  The bbox pad may use it unscaled (over-
//     padding is harmless), but here it bounds the actual blur: leaving it at
//     the full-res value makes a proxy-0.5
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
//   x, y        : the source pixel's absolute image coordinates
//   samples     : THIS PIXEL'S samples.  Modified in place (sanitised, tidied,
//                 sorted) and reusable as scratch across pixels — pass the same
//                 vector every time so the per-sample channel vectors keep
//                 their capacity.
//   scratch     : per-thread scratch, see FlattenScratch
//   out         : SoA to append to; call out.begin() once per band first
//   stats       : optional, may be nullptr
//   residualT   : optional.  Set to the pixel's virtual background claim —
//                 the running transmittance left after every fragment's share
//                 was taken front to back, so shares + *residualT sum to 1.
//                 A pixel with nothing to flatten sets it to 1.
//   residualRadiusPx : optional.  Set to the deepest staged fragment's
//                 radius (the last piece of the deepest span), the radius
//                 the residual scatters at.  Left untouched when nothing was
//                 staged.
//
// Pipeline, in order:
//   1. sanitise depths and alphas (a NaN depth would make the sorts'
//      comparators a non-strict-weak ordering, which is UB)
//   2. optional ray-distance -> Z, per pixel
//   3. deepc::tidyOverlapping() — always on, correctness-required
//   4. per sample: a point is one fragment; a span is cut by
//      volumetricPieceBounds() into independent pieces.  The pixel's unit
//      area is partitioned front to back into shares here.
//   5. pre-merge (if on): adjacent fragments within merge_tolerance of the
//      group's first radius and in one holdout bracket
//   6. collision merge: the next group joins the held-back one when both see
//      the same lens patch (sameLensPatch) and share a holdout bracket
//   7. each merged run of fragments is composited BACK TO FRONT,
//      C = c + (1 - a) * C, and appended as one fragment
// ---------------------------------------------------------------------------
void flattenPixelToSoA(const FlattenParams& params,
                       int                  x,
                       int                  y,
                       std::vector<SampleRecord>& samples,
                       FlattenScratch&      scratch,
                       SampleSoA&           out,
                       FlattenStats*        stats,
                       float*               residualT,
                       float*               residualRadiusPx);

// ---------------------------------------------------------------------------
// checkCompositionContract — every fragment is one deposit, so the audit is
// of its fields: all finite (colour included), alpha in [0, 1], radius >= 0.
// Returns false and, if `firstBadFragment` is non-null, the offending index.
// O(fragments), no allocation; for tests and debug builds.
// ---------------------------------------------------------------------------
bool checkCompositionContract(const SampleSoA& soa,
                              std::size_t* firstBadFragment = nullptr);

// ===========================================================================
//
//  THE STREAMING COMPOSITE
//
//  Pipeline for one band, in order:
//
//    planes.allocate(C, W, B)            // sizes and zeroes the state
//    sortFragmentsByDepth(soa, order)    // stable, by orderedDepthKey
//    scatterStreamCPU(...)               // deposits, in depth order
//    scatterBackgroundCPU(...)           // the residual claim on arrival
//    resolveStreamCPU(...)               // coverage fill + premultiplied clamp
//
//  Each destination pixel folds the deposits that reach it front to back on
//  its own running state (depositStreamSpanRecency), so the composite is done
//  by the time the last deposit lands.  A destination pixel's deposits all
//  come from source rows within padY of it, which every band containing it
//  fetches, and the stable sort puts them in the same relative order in every
//  band: the per-pixel arithmetic is the same under any band plan.
//
// ===========================================================================

// ---------------------------------------------------------------------------
// THE DEPTH-ORDERED STREAMING COMPOSITE — per-pixel state and the deposit body
//
// Fragments reach the body in depth order (orderedDepthKey, ties in emission
// order), so each destination pixel folds its deposits front to back.  Per
// pixel:
//
//   claimed   Q   area some deposit has covered
//   alpha     A   accumulated alpha; A == Q * (1 - T) with T the claimed
//                 share's mean transmittance, so T needs no plane
//   oldArea   uO  the claimed area least recently covered (chunk O) ...
//   oldMass   sO  ... and the transmitted mass on it.  Chunk N (the area the
//                 latest deposits covered) is implied: uN = Q - uO,
//                 sN = Q - A - sO
//   lastCoc       signed CoC of the previous deposit here
//   color[c]      accumulated premultiplied colour
//   arrival       the coverage fill's denominator
//
// Why two recency chunks and not one pooled claimed share: the deposits of
// one continuous surface tile the lens.  Pooled, the n deposits of a layer
// of alpha a over full coverage transmit prod(1 - a*w_i) -> e^-a instead of
// 1 - a (two full-coverage 0.5 layers read 0.697, not 0.75).  Covering free
// area first, then O, then N, a surface only re-covers its own area once O is
// exhausted, and a full-coverage layer exhausts O exactly when it is
// complete, so a stack of layers composes exactly.
// ---------------------------------------------------------------------------
struct StreamPlaneView {
    float*         claimed      = nullptr;
    float*         alpha        = nullptr;
    float*         oldArea      = nullptr;
    float*         oldMass      = nullptr;
    float*         lastCoc      = nullptr;
    float*         color        = nullptr;   // color[c * pixelCount + i]
    float*         arrival      = nullptr;
    int            channelCount = 0;
    int            width        = 0;
    int            height       = 0;
    std::ptrdiff_t pixelCount   = 0;

    DEEPC_HD inline bool valid() const
    {
        return claimed != nullptr && alpha != nullptr && oldArea != nullptr
            && oldMass != nullptr && lastCoc != nullptr && arrival != nullptr
            && (channelCount == 0 || color != nullptr)
            && channelCount >= 0 && width > 0 && height > 0
            && pixelCount == static_cast<std::ptrdiff_t>(width) * height;
    }
};

struct StreamPlanes {
    PodBuffer<float> claimed;
    PodBuffer<float> alpha;
    PodBuffer<float> oldArea;
    PodBuffer<float> oldMass;
    PodBuffer<float> lastCoc;
    PodBuffer<float> color;     // channelCount * pixelCount
    PodBuffer<float> arrival;

    int            channelCount = 0;
    int            width        = 0;
    int            height       = 0;
    std::ptrdiff_t pixelCount   = 0;

    // Sizes every plane and zeroes it; repeat calls with the same geometry
    // keep the allocation.
    void allocate(int channelCountIn, int widthIn, int heightIn);
    void zero();
    void release();
    std::size_t sizeBytes() const;
    StreamPlaneView view();

    // W*B*(C+6)*4: no factor of K anywhere.
    static std::size_t bytesForBand(int channelCount, int width, int height)
    {
        const std::size_t c = (channelCount > 0) ? static_cast<std::size_t>(channelCount) : 0;
        const std::size_t w = (width > 0) ? static_cast<std::size_t>(width) : 0;
        const std::size_t h = (height > 0) ? static_cast<std::size_t>(height) : 0;
        return w * h * (c + 6) * sizeof(float);
    }
};

// Unsigned order of the keys is the order of the depths for every non-NaN
// float, -0 sorting just before +0: positives get the sign bit set, negatives
// have every bit flipped.
DEEPC_HD inline std::uint32_t orderedDepthKey(float depth)
{
    std::uint32_t u = 0;
    memcpy(&u, &depth, sizeof(u));
    return (u & 0x80000000u) ? ~u : (u | 0x80000000u);
}

// A deposit arriving more than this far in signed CoC from the previous
// deposit at a pixel is taken to be a different surface.  It sits above a
// receding plane's row-to-row CoC step (0.5 px on scene (g)) and below any
// separation between distinct surfaces that matters: a missed rotation costs
// only the straddle case, a spurious one only the kernel's adjoint surplus.
constexpr float kCocJumpRotatePx = 1.0f;

DEEPC_HD inline float clampUnit(float v)
{
    return (v > 0.0f) ? ((v < 1.0f) ? v : 1.0f) : 0.0f;
}

// ---------------------------------------------------------------------------
// depositStreamSpanRecency — one fragment's weights over one clipped row span
//
//   w         effective weights (kernel weight times holdout visibility)
//   xRow      scratch, >= count floats: receives each pixel's composited
//             weight x, which the colour pass then scales
//   alpha     the fragment's alpha;  color: its premultiplied channels
//   signedCoc the fragment's signed CoC radius, for the jump rotation
//
// Per pixel, with F = 1 - Q the free area:
//
//   rotate (uO = Q, sO = Q - A) only when the deposit lies more than
//     kCocJumpRotatePx from lastCoc and Q > 0
//   pf = min(w, F);  pO = min(w - pf, uO);  pN = w - pf - pO
//   x  = pf + pO * sO/uO + pN * sN/uN
//   A += a*x;  C += c*x;  Q += pf
//   pN > 0:  O becomes everything claimed except the part of N just covered
//            (Q' - pN, (Q' - A') - pN*(sN/uN)*(1 - a))
//   else:    O shrinks by what was covered (uO - pO, sO - pO*sO/uO)
//
// The jump rotation is what a surface needs that first fills free area and
// then reaches claimed area (a card behind a partly covering fog): without
// it, its own free-area deposits would be pooled into the O it then covers.
// A deposit that reaches N is re-covering its own surface's area, so every
// other claimed area, including what the deposit itself just covered, is
// older than that part.  Taking O as uN - pN would file the deposit's whole
// footprint as recent, and the next piece of the surface would read it
// pooled with its own coverage.  With O empty the relabel yields the same x
// and O as a rotation to O = Q, so a surface reaching claimed area needs no
// rotation of its own below the jump threshold.
//
// Every pixel is a different destination, so there is no cross-iteration
// dependency.  Both transmittances are always computed and the branches are
// selects, so the loop has no per-pixel control flow.  A zero area divides
// by 1 instead (its mass is zero too), and the clamp absorbs rounding drift
// of sO/sN outside [0, u].
// ---------------------------------------------------------------------------
DEEPC_HD inline void depositStreamSpanRecency(const StreamPlaneView&    planes,
                                              std::ptrdiff_t            dstOffset,
                                              const float* __restrict__ w,
                                              float* __restrict__       xRow,
                                              int                       count,
                                              float                     alpha,
                                              float                     signedCoc,
                                              const float* __restrict__ color,
                                              int                       channelCount)
{
    float* __restrict__ qp  = planes.claimed + dstOffset;
    float* __restrict__ ap  = planes.alpha + dstOffset;
    float* __restrict__ uop = planes.oldArea + dstOffset;
    float* __restrict__ sop = planes.oldMass + dstOffset;
    float* __restrict__ cp  = planes.lastCoc + dstOffset;

    for (int i = 0; i < count; ++i) {
        const float wi = w[i];
        const float q  = qp[i];
        const float a  = ap[i];
        const float fr = 1.0f - q;

        const float dc     = signedCoc - cp[i];
        const bool  jump   = (q > 0.0f) && ((dc > kCocJumpRotatePx) || (dc < -kCocJumpRotatePx));
        const float uO     = jump ? q : uop[i];
        const float sO     = jump ? (q - a) : sop[i];

        const float uNraw = q - uO;
        const float uN    = (uNraw > 0.0f) ? uNraw : 0.0f;
        const float sN    = q - a - sO;
        const float tO    = clampUnit(sO / ((uO > 0.0f) ? uO : 1.0f));
        const float tN    = clampUnit(sN / ((uN > 0.0f) ? uN : 1.0f));

        const float pf = (wi < fr) ? wi : fr;
        const float r  = wi - pf;
        const float pO = (r < uO) ? r : uO;
        const float pN = r - pO;
        const float x  = pf + pO * tO + pN * tN;

        const bool  reachN = pN > 0.0f;
        const float qNew   = q + pf;
        const float aNew   = a + alpha * x;
        uop[i]  = reachN ? (qNew - pN) : (uO - pO);
        sop[i]  = reachN ? ((qNew - aNew) - pN * tN * (1.0f - alpha)) : (sO - pO * tO);
        qp[i]   = qNew;
        ap[i]   = aNew;
        cp[i]   = signedCoc;
        xRow[i] = x;
    }

    for (int c = 0; c < channelCount; ++c) {
        const float v = color[c];
        if (v == 0.0f)
            continue;
        float* __restrict__ dst = planes.color
            + static_cast<std::ptrdiff_t>(c) * planes.pixelCount + dstOffset;
        for (int i = 0; i < count; ++i)
            dst[i] += xRow[i] * v;
    }
}

// ---------------------------------------------------------------------------
// residualWindowYRange — the Y extent of the virtual-background window:
// band +/- padY, clipped to the OUTPUT box, NEVER to `srcBox`.
//
// The SoA fetch loop clips its OWN row range to `srcBox` (correct for that
// loop — deepEngine() has no data outside it), and reusing that clip to size
// or default the residual map is exactly the bug this function exists to
// avoid: a deep input whose bbox is tight around its content would then give
// every bloom pixel outside `srcBox` no virtual background at all, arrival
// there would equal the bloom's own weight, and a soft defocused edge would
// divide to a hard disc. See ResidualWindow.
// ---------------------------------------------------------------------------
DEEPC_HD inline void residualWindowYRange(int outputBoxY0, int outputBoxY1,
                                          int bandY0, int bandY1, int padY,
                                          int& windowY0, int& windowY1)
{
    const int lo = bandY0 - padY;
    const int hi = bandY1 + padY;
    windowY0 = (lo > outputBoxY0) ? lo : outputBoxY0;
    windowY1 = (hi < outputBoxY1) ? hi : outputBoxY1;
}

// ---------------------------------------------------------------------------
// resolveBackgroundRadiusPx — the `background_depth` knob's resolution.
//
// backgroundDepthKnob <= 0 (including NaN, same convention as clampf) means
// auto: the CoC at the frame's farthest measured depth. A positive manual
// value is clamped to rMax, the frame's own measured radius range, so
// invented coverage for pixels with no samples at all never blurs wider than
// anything the frame actually measured.
// ---------------------------------------------------------------------------
DEEPC_HD inline float resolveBackgroundRadiusPx(float backgroundDepthKnob,
                                                float cocAtDepthMax,
                                                float rMax)
{
    if (!(backgroundDepthKnob > 0.0f))
        return cocAtDepthMax;
    return clampf(backgroundDepthKnob, 0.0f, rMax);
}

enum class FillMode {
    Foreground = 0,
    Background = 1
};

// knobPx <= 0 (NaN included, as resolveBackgroundRadiusPx) is auto: 2r + 1,
// wide enough to reach past the invented disc's own radius on either side.
DEEPC_HD inline float resolveFillSearchPx(float knobPx, float radiusPx, float maxRadiusPx)
{
    if (!(knobPx > 0.0f)) {
        const float autoPx = 2.0f * radiusPx + 1.0f;
        return (autoPx < maxRadiusPx) ? autoPx : maxRadiusPx;
    }
    return (knobPx < maxRadiusPx) ? knobPx : maxRadiusPx;
}

// ---------------------------------------------------------------------------
// ResidualWindow — the virtual background's per-pixel claim, over the FULL
// fetch window (band +/- padY rows, clipped only to the output box) rather
// than `srcBox`.
//
// allocate() defaults every cell to T = 1 (fully unclaimed) and the caller's
// background radius BEFORE any source pixel is visited.  A deep input whose
// bbox is tight around its content must still get a virtual background on
// every bloom pixel outside that bbox, or arrival there equals the bloom's
// own weight and a soft edge divides to a hard disc — so this default is set
// over the window's full extent, not just the sub-rectangle the fetch loop
// can query DeepPlane pixels from. setPixel() is called only for pixels the
// fetch loop actually visits (inside `srcBox` AND this window — see
// contains()); every other cell keeps the allocate()-time default.
//
// Owned by one BandJob, exactly like StreamPlanes — one instance per
// CONCURRENT band (the memory-limit cap), not per render thread — so it is a
// genuine per-in-flight-band resident cost and bandBudgetBytes() counts it
// (bytesForWindow() below), not merely names it as an omission.
//
// Consumed by scatterBackgroundCPU(), which walks every window pixel and
// deposits its `t` into the arrival plane through a kernel at its own
// `radiusPx` -- the virtual background's claim on the coverage denominator.
// ---------------------------------------------------------------------------
struct ResidualWindow {
    PodBuffer<float> t;         // residual transmittance, one per window pixel
    PodBuffer<float> radiusPx;  // residual scatter radius, one per window pixel

    int x      = 0;   // window origin, absolute image coordinates
    int y      = 0;
    int width  = 0;
    int height = 0;

    void allocate(int xIn, int yIn, int widthIn, int heightIn,
                  float backgroundRadiusPx)
    {
        x      = xIn;
        y      = yIn;
        width  = (widthIn  > 0) ? widthIn  : 0;
        height = (heightIn > 0) ? heightIn : 0;

        const std::size_t n = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
        t.assign(n, 1.0f);
        radiusPx.assign(n, backgroundRadiusPx);
    }

    std::ptrdiff_t index(int px, int py) const
    {
        return static_cast<std::ptrdiff_t>(py - y) * width + (px - x);
    }

    bool contains(int px, int py) const
    {
        return px >= x && px < x + width && py >= y && py < y + height;
    }

    // residualT/residualRadiusPx are flattenPixelToSoA()'s out-params for
    // pixel (px, py); write them straight through, no reinterpretation here.
    void setPixel(int px, int py, float residualT, float residualRadiusPx)
    {
        const std::ptrdiff_t i = index(px, py);
        t[static_cast<std::size_t>(i)]        = residualT;
        radiusPx[static_cast<std::size_t>(i)] = residualRadiusPx;
    }

    // Two buffers (T + residual radius), K-INDEPENDENT like the arrival
    // plane — `height` here is the WINDOW height (band height + 2*padY,
    // clipped to the output box; see residualWindowYRange()), not the band
    // height alone, because that is what allocate() above actually sizes.
    // One place so bandBudgetBytes() and the code cannot drift apart, same
    // convention as StreamPlanes::bytesForBand().
    static std::size_t bytesForWindow(int width, int height)
    {
        const std::size_t w = (width  > 0) ? static_cast<std::size_t>(width)  : 0;
        const std::size_t h = (height > 0) ? static_cast<std::size_t>(height) : 0;
        return 2 * w * h * sizeof(float);
    }
};

// ---------------------------------------------------------------------------
// buildResidualWindow — the ONE place that sizes and fills a ResidualWindow,
// shared by production (DeepCDefocus.cpp's computeBand()) and the doctest
// suite, so the output-box-vs-srcBox decision this exists to get right is
// tested through the exact code that runs it, not a re-implementation of it.
//
// `outputBox*` bounds the window (see ResidualWindow, residualWindowYRange);
// `srcBox*` bounds the ROWS AND COLUMNS actually visited — deepEngine() has
// no data outside it, same as the pre-existing fetch loop.
//
// `fetchRow(y)` runs once per visited row (production: deepEngine() into a
// DeepPlane, returning false on abort/failure); `flattenPixel(x, y, t, r)`
// runs once per visited column of that row and returns false for a pixel
// with nothing to flatten (production: fillSampleRecords()'s own false),
// leaving `t`/`r` at the caller-seeded "no sample" defaults it was called
// with. Returns false the moment either callback does.
// ---------------------------------------------------------------------------
template <typename FetchRowFn, typename FlattenPixelFn>
bool buildResidualWindow(ResidualWindow& window,
                         int outputBoxX0, int outputBoxX1,
                         int outputBoxY0, int outputBoxY1,
                         int srcBoxX0, int srcBoxX1,
                         int srcBoxY0, int srcBoxY1,
                         int bandY0, int bandY1, int padY,
                         float backgroundRadiusPx,
                         FetchRowFn&&     fetchRow,
                         FlattenPixelFn&& flattenPixel)
{
    int wy0 = 0, wy1 = 0;
    residualWindowYRange(outputBoxY0, outputBoxY1, bandY0, bandY1, padY, wy0, wy1);
    window.allocate(outputBoxX0, wy0, outputBoxX1 - outputBoxX0, wy1 - wy0,
                    backgroundRadiusPx);

    const int fy0raw = bandY0 - padY;
    const int fy1raw = bandY1 + padY;
    const int fy0 = (srcBoxY0 > fy0raw) ? srcBoxY0 : fy0raw;
    const int fy1 = (srcBoxY1 < fy1raw) ? srcBoxY1 : fy1raw;

    for (int py = fy0; py < fy1; ++py) {
        if (!fetchRow(py))
            return false;
        for (int px = srcBoxX0; px < srcBoxX1; ++px) {
            float t = 1.0f;
            float r = backgroundRadiusPx;
            if (!flattenPixel(px, py, t, r))
                continue;
            if (window.contains(px, py))
                window.setPixel(px, py, t, r);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// HoldoutSoA — per-dest-pixel boundary transmittance LUT.
//
// The scatter takes `vis` from here and multiplies it into every deposit
// before the deposit rule sees it.  An absent holdout is an empty view:
// enabled() is false, the scatter takes a separate loop with no per-pixel work
// at all, and vis is identically 1 at ZERO cost (not "vis = 1.0f multiplied
// in").
//
// `boundaryT` is filled per destination pixel by HoldoutVisibility::build(),
// which writes exactly boundaryCount contiguous floats — i.e.
// `build(..., holdout.pixelLut(i))` for pixel i.  The layout is therefore
// PIXEL-MAJOR:
//
//     boundaryT[i * boundaryCount + b]
//
// which is what build() emits and what HoldoutVisibility::interpAtBucket()
// consumes, so the build and the consumption need no re-striding and no
// reimplementation of either.  (Boundary-major would let the scatter read
// two contiguous rows per span, but interpAtBucket() takes no stride and the
// vis path is a log/exp per fragment-pixel anyway — it is not the loop that
// vectorizes.)
//
// *** THE BOUNDARY SET IS THIS VIEW'S OWN ***
//
// `boundaries` is carried BY VALUE, alongside the LUT it was built at, and it
// is the single source of truth for both halves of the seam: HoldoutLut::build()
// fills `boundaryT` at exactly these depths and the scatter locates a fragment
// in exactly these depths.  There is no second party to agree with and so no
// way for a build and a lookup to drift onto different boundary arrays.  The
// pair fed to interpAtBucket() is HoldoutBoundaries::locate()'s, computed by
// scatterStreamCPU() from the fragment's own depth in O(1) closed form, so
// SampleSoA carries no boundaryIndex/boundaryFrac at all.
//
// `boundaries.count()` is depth_layers + 1, so per-band LUT memory is
// (K+1)*W*B*4 bytes.
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

    // The fragment's (index, frac) into the LUT.  O(1), closed form, and the
    // ONLY locator that may feed HoldoutVisibility::interpAtBucket() on this
    // LUT.
    DEEPC_HD inline BoundarySpan locate(float depth) const
    {
        return boundaries.locate(depth);
    }
};

// The storage behind the view above is HoldoutLut, built from
// HoldoutSampleSoA -- both immediately below.

// ---------------------------------------------------------------------------
// HoldoutSampleSoA — one band's holdout input, DeepFront/DeepBack/Alpha ONLY
//
// THE SOURCE-SIDE MODEL DOES NOT APPLY HERE.  Holdout occlusion is evaluated
// at each DESTINATION pixel from that SAME pixel's own holdout samples --
// there is no scatter, no CoC, no volumetric cut and no pre-merge on this side.
// That absence is deliberate and is exactly what makes the holdout edge
// pixel-sharp: a source fragment's blur is a property of the SOURCE, but
// visibility is
// looked up at each dest pixel independently from that pixel's own samples,
// so no filtering of any kind can leak across pixels.
//
// deepc::tidyOverlapping() is likewise NOT run on holdout samples, and this
// is not an oversight: HoldoutVisibility's model multiplies independent
// per-sample transmittances (Beer's law -- extinction coefficients along one
// ray multiply regardless of how the underlying spans overlap), so two
// overlapping holdout spans need no merge to combine correctly.
//
// Storage is CSR ("compressed sparse row"): `pixelOffset[i]..pixelOffset[i+1]`
// delimits pixel i's samples in the flat zFront/zBack/alpha arrays.  Built by
// calling appendPixel() once per band pixel, in the SAME band-relative
// row-major order (`i = y*width + x`) that StreamPlaneView and HoldoutSoA
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
    // the HoldoutBoundaries depths, which live in Z, so an
    // uncorrected holdout sits
    // systematically too far back off-axis (measured: 47% too far in Z at the
    // corner of a 20mm/36x24 frame).  This is the request/engine-style
    // "two passes disagreeing about depth" failure this node has already
    // names for computeDepthRange(), reached through a different door.
    void appendPixel(std::vector<SampleRecord>& samples, float depthScale = 1.0f);

    // Size 0, capacity kept (band-to-band reuse).
    void clear();

    // Drops every allocation.
    void release();

    std::size_t sizeBytes() const;
};

// ---------------------------------------------------------------------------
// HoldoutLut — THE OWNING STORAGE behind HoldoutSoA
//
// One instance per band-computing thread, reused band to band exactly like
// StreamPlanes: build() sizes and refills it, view() hands the scatter its
// non-owning HoldoutSoA.  Lifetime is the band's -- the thread that calls
// scatterStreamCPU() must keep this alive until that call returns, and may
// reuse (rebuild) it for the next band.
//
// build(): for every band pixel, HoldoutVisibility::build() fills
// boundaryCount contiguous floats at the HoldoutBoundaries depths (K+1 of them,
// uniform in Z; see below and HoldoutBoundaries), folding the in-span exponential attenuation in once per
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
// HoldoutSoA (boundaryT == nullptr), scatterStreamCPU()'s `useHoldout` gate
// is false for the whole band, and nothing in the per-fragment path so much
// as dereferences the holdout.
// That is a genuinely free path, not a multiply by 1.0 per fragment.
//
// ZERO COST IS THE CALLER'S HALF TOO.  build() itself on an empty band is one
// branch (measured 7.8 ns for a 4096x64 band), but running the appendPixel()
// loop to DISCOVER that the band is empty is NOT free: 262144 calls with an
// empty vector measured 1.98 ms/band, ~67 ms per 4K frame of pure
// bookkeeping.  So the caller must SKIP the fetch/append loop entirely when
// input(1) is unconnected or the band's box does not intersect the holdout's
// -- begin(N) followed by no appendPixel() at all is well defined and lands
// on exactly the same disabled view.  Only a band that genuinely straddles
// the holdout bbox should walk its pixels.
//
// COST WHEN IT IS ON (4096x64 band, K=16, 2 holdout samples/pixel, measured):
// appendPixel() 15.3 ms/band, build() 24.7 ms/band, 17.0 MB/band.  Roughly
// 1.4 s and 578 MB across a 4K frame's 34 bands, and both scale with K.  None
// of that is in StreamPlanes::bytesForBand(), so a memory budget must add it
// separately.
//
// ***  WHY THE BOUNDARIES ARE UNIFORM IN Z  *********************************
//
// Chording in log space between two boundary values is exact only while no
// holdout span edge falls strictly inside the bracket, and for the commonest
// holdout of all -- one opaque card, i.e. a POINT sample -- the true T is a
// step, which the log chord collapses onto the bracket's NEAR boundary.  The
// dominant error is therefore placement: no interpolant beats a worst case of
// (T0 - T1)/2 inside one bracket, so the brackets are spent uniformly in depth
// (occlusion's criterion) rather than uniformly in CoC.  On boundaries spaced
// uniformly in CoC at the node's defaults (K=16, focus 10, depth range
// [1,100]) an opaque card at z=50 lands in a [10,100] bracket and erases a
// fragment at z=15 by 98%; uniform in Z measures mean |vis error| 0.057
// against 0.391.  See HoldoutBoundaries.
//
// WHAT REMAINS.  Half a bracket of placement uncertainty is irreducible with
// K+1 values; on top of it, for a bracket whose far transmittance is bitwise
// zero (an opaque step, or a dense alpha<1 stack whose product underflowed)
// the log chord floors log T at kMinTransmittance and so collapses to ~0
// across the whole bracket, one-sided TOWARD CAMERA -- harness check f2 pins
// it at 78% of a depthRange/K bracket erased in front, decaying as
// 10^(-30*frac).  That is the deliberate trade: interpolants that erase less
// in front of an opaque card (a midpoint step, or linear-in-T) LEAK source
// through a dense volumetric holdout -- 46 samples at alpha=0.9 in one
// bracket, scene (f)'s content class -- at up to full visibility (midpoint
// +1.000, linear-in-T +0.840, against the log chord's +1e-09 worst leak,
// rendered end to end).  Erase-toward-camera is bounded and K-reducible;
// invented visibility through a holdout is neither.  See "THE HOLDOUT
// INTERPOLANT" in DeepCDefocusMath.h for the numbers.
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
    // flattened holdout samples and the frame's holdout boundary set.  Safe
    // to call repeatedly with the same
    // geometry (PodBuffer keeps its capacity), which is the band loop's normal
    // path.  If `samples` was not filled for the full `samples.pixelCount`
    // (a caller bug), the unfilled tail is treated as zero-sample rather than
    // read out of bounds.
    //
    // `boundaries` MUST be frame-global, not per-band: a fragment near a band
    // edge scatters into two bands, and if those bands' LUTs were sampled at
    // different depths the same fragment would get two different vis values --
    // a visible seam along every band boundary.
    void build(const HoldoutSampleSoA& samples, const HoldoutBoundaries& boundaries);

    void release();

    std::size_t sizeBytes() const;

    // Non-owning view for scatterStreamCPU().  Empty (disabled) whenever
    // build() found nothing to build.
    HoldoutSoA view() const;
};

// ---------------------------------------------------------------------------
// ScatterParams — everything scatterStreamCPU() needs that is not per-fragment
//
// Band geometry is expressed as the band's ORIGIN in absolute image
// coordinates plus its size; SampleSoA stores absolute source coordinates (so
// it is independent of the band decomposition), and the scatter subtracts the
// origin.  Fragments outside the band are NOT an error and are not culled:
// their discs are exactly how a band gets the energy scattered in from the
// `band +/- ceil(maxRadius*aspect)` rows fetched around it.  Every span is
// clipped to the band.
// ---------------------------------------------------------------------------
struct ScatterParams {
    int bandX      = 0;         // band origin, absolute image coordinates
    int bandY      = 0;
    int bandWidth  = 0;
    int bandHeight = 0;

    // Radius at or below which a fragment takes the sharp fast path: it
    // deposits weight 1 into its OWN pixel instead of rasterising a
    // disc.  This is also what keeps DiscKernelLUT's documented caller
    // contract ("never reaches the sampler for radius <= 0.5px") true from
    // this side.
    float sharpRadiusPx = kSharpRadiusPx;
};

// ---------------------------------------------------------------------------
// ScatterStats — optional instrumentation.  Cumulative.
// ---------------------------------------------------------------------------
struct ScatterStats {
    std::size_t fragments      = 0;   // fragments examined
    std::size_t sharpFragments = 0;   // took the sharp fast path
    std::size_t culled         = 0;   // deposited nothing (zero alpha+colour,
                                      // or wholly outside the band)
    std::size_t rowSpans       = 0;   // clipped kernel rows rasterised
    std::size_t pixelDeposits  = 0;   // total span pixels touched
};

// ---------------------------------------------------------------------------
// ScatterScratch — caller-owned per-thread scratch
//
// Three rows, each at least one kernel row wide: the blended raw weights
// (blendBracketRow), the holdout-folded weights, and the deposit body's
// per-pixel composited weight.
// ---------------------------------------------------------------------------
struct ScatterScratch {
    PodBuffer<float> rowWeights;
    PodBuffer<float> visWeights;
    PodBuffer<float> xRow;

    void ensureRow(std::size_t count)
    {
        if (rowWeights.size() < count) {
            rowWeights.resizeUninitialized(count);
            visWeights.resizeUninitialized(count);
            xRow.resizeUninitialized(count);
        }
    }

    void release()
    {
        rowWeights.release();
        visWeights.release();
        xRow.release();
    }
};

// Arrival must exceed a noise floor before it is trusted as a divisor, and
// must stay clear of size-0's one-ulp-under-1 sums so the bit-exact parity
// gate never sees a division.
constexpr float kFillMinArrival  = 1e-3f;
constexpr float kFillDeficitTol  = 1e-5f;

// ---------------------------------------------------------------------------
// blendBracketRow — row dy of (1 - f) * K[A] + f * K[B], as one row
//
// The deposit rule is not linear in the weight, so a radius between two grid
// nodes must be deposited as one blended row: two passes at (1 - f) and f
// would find the first pass's claimed area under the second and composite
// the fragment over itself.
//
// Writes the union of the two kernels' spans on row dy into out[] and returns
// its pixel count, with xStartOut its first column relative to the fragment's
// centre; 0 when neither kernel has a span there.  `out` must hold
// 2 * max(radiusX) + 1 floats.
// ---------------------------------------------------------------------------
DEEPC_HD inline int blendBracketRow(const KernelView& kvA, float fA,
                                    const KernelView& kvB, float fB,
                                    int dy, float* __restrict__ out, int& xStartOut)
{
    const int rowA = dy + kvA.radiusY;
    const int rowB = dy + kvB.radiusY;
    const bool haveA = rowA >= 0 && rowA < kvA.rowCount && !kvA.row(rowA).empty();
    const bool haveB = rowB >= 0 && rowB < kvB.rowCount && !kvB.row(rowB).empty();
    if (!haveA && !haveB)
        return 0;

    int xs = 0;
    int xe = -1;
    if (haveA) {
        xs = kvA.row(rowA).xStart;
        xe = kvA.row(rowA).xEnd;
    }
    if (haveB) {
        const RowSpan& sb = kvB.row(rowB);
        xs = haveA ? ((sb.xStart < xs) ? sb.xStart : xs) : sb.xStart;
        xe = haveA ? ((sb.xEnd > xe) ? sb.xEnd : xe) : sb.xEnd;
    }

    const int count = xe - xs + 1;
    for (int k = 0; k < count; ++k)
        out[k] = 0.0f;
    if (haveA) {
        const RowSpan& sa = kvA.row(rowA);
        const float*   wa = kvA.rowWeights(rowA);
        float* __restrict__ o = out + (sa.xStart - xs);
        for (int k = 0; k < sa.count(); ++k)
            o[k] += fA * wa[k];
    }
    if (haveB) {
        const RowSpan& sb = kvB.row(rowB);
        const float*   wb = kvB.rowWeights(rowB);
        float* __restrict__ o = out + (sb.xStart - xs);
        for (int k = 0; k < sb.count(); ++k)
            o[k] += fB * wb[k];
    }
    xStartOut = xs;
    return count;
}

// ---------------------------------------------------------------------------
// resolveStreamPixel — one pixel's output from its finished stream state
//
// The composite already happened, deposit by deposit.  What is left is the
// deficit-only coverage fill (a pixel whose arrival fell short of 1 is scaled
// up by 1/arrival, alpha and colour together) and the premultiplied clamp
// (alpha above 1 is read as 1 with the colour scaled down by the same factor,
// never the alpha alone).
// ---------------------------------------------------------------------------
DEEPC_HD inline void resolveStreamPixel(const StreamPlaneView& view, std::ptrdiff_t i,
                                        float* __restrict__ outColor,
                                        float* __restrict__ outAlpha)
{
    float a = view.alpha[i];
    float s = 1.0f;
    const float arrival = view.arrival[i];
    if (arrival > kFillMinArrival && arrival < 1.0f - kFillDeficitTol) {
        s = 1.0f / arrival;
        a *= s;
    }
    const float outA  = clampf(a, 0.0f, 1.0f);
    const float clamp = (a > outA && a > 0.0f) ? (outA / a) : 1.0f;
    for (int c = 0; c < view.channelCount; ++c) {
        float v = view.color[static_cast<std::ptrdiff_t>(c) * view.pixelCount + i];
        if (s != 1.0f)
            v *= s;
        if (clamp != 1.0f)
            v *= clamp;
        outColor[static_cast<std::ptrdiff_t>(c) * view.pixelCount + i] = v;
    }
    outAlpha[i] = outA;
}

// ---------------------------------------------------------------------------
// sortFragmentsByDepth — the band's stream order
//
// `order` receives a permutation of [0, fragmentCount) in orderedDepthKey
// order, equal keys in increasing SoA index.  The SoA's emission order is
// (source row, source column, front to back), and a band's SoA is a
// contiguous run of fetch rows, so the tie order is the same in every band
// and the key needs no tie-breaking field.
//
// Stable LSD radix, 8 bits per pass, over an index permutation; a pass whose
// byte is identical in every key is skipped.  Scratch is reused band to band.
// ---------------------------------------------------------------------------
struct StreamSortScratch {
    PodBuffer<std::uint32_t> keys;
    PodBuffer<std::uint32_t> keysAlt;
    PodBuffer<std::uint32_t> orderAlt;

    void release()
    {
        keys.release();
        keysAlt.release();
        orderAlt.release();
    }

    std::size_t sizeBytes() const
    {
        return keys.sizeBytes() + keysAlt.sizeBytes() + orderAlt.sizeBytes();
    }
};

void sortFragmentsByDepth(const SampleSoA&          samples,
                          PodBuffer<std::uint32_t>& order,
                          StreamSortScratch&        scratch);

// True when `order` is a permutation of the SoA's fragments with keys
// non-decreasing and equal keys in increasing index.  O(n) with one
// allocation; for tests and debug builds.
bool checkStreamOrder(const SampleSoA& samples, const PodBuffer<std::uint32_t>& order);

// ---------------------------------------------------------------------------
// scatterStreamCPU — the band's fragments, in stream order, into the state
//
//   order   : sortFragmentsByDepth()'s permutation of `samples`; a size
//             mismatch deposits nothing
//   holdout : empty view means vis == 1 at no per-pixel cost
//   kernel  : the KernelSampler seam; destX/destY/depth/group are passed
//             through even though the disc kernel ignores them
//   planes  : accumulated into; allocate()/zero() per band
//
// Per fragment: its radius picks the bracketing grid kernels, whose rows are
// blended into one scratch row (blendBracketRow); `arrival` takes
// share * raw weight, before the holdout fold, so held-out alpha is never
// renormalised back up by the fill; vis multiplies the weights; the row goes
// through depositStreamSpanRecency.  At or below the sharp radius the
// fragment is one weight-1 deposit at its own pixel.
//
// Every channel is deposited at the base radius, through one alpha state:
// the rule's state (claimed area, alpha, the recency chunks) is per pixel,
// not per channel group, so channel groups of different radii would each
// need their own state planes.  With every channelRadiusScale at 1 the base
// radius is every group's.
//
// Thread-agnostic: no statics, nothing shared between calls except what the
// caller passes.  Channel count is min(planes, samples).
// ---------------------------------------------------------------------------
void scatterStreamCPU(const ScatterParams&            params,
                      const SampleSoA&                samples,
                      const PodBuffer<std::uint32_t>& order,
                      const HoldoutSoA&               holdout,
                      const KernelSampler&            kernel,
                      StreamPlanes&                   planes,
                      ScatterScratch&                 scratch,
                      ScatterStats*                   stats = nullptr);

// ---------------------------------------------------------------------------
// scatterBackgroundCPU — the virtual background's claim, into `arrival` only
//
// `arrival` is the band's plane, params.bandWidth * params.bandHeight floats.
// Every window pixel with T > kFillDeficitTol deposits w * T through a kernel
// at its OWN residual radius (a single frame-wide radius is a measured
// artifact: a true alpha-0.9 surface reads ~0.893), bracketed like a
// fragment's; the claim is linear in the weight, so the two nodes are two
// passes.  The skip floor is the fill's deficit tolerance on purpose: each
// skipped pixel withholds at most that much of a unit kernel, so the fill
// still reads the pixel as full.
//
// Deliberately naive: one disc per non-opaque source pixel.
// ---------------------------------------------------------------------------
void scatterBackgroundCPU(const ScatterParams&  params,
                          const ResidualWindow& residual,
                          const KernelSampler&  kernel,
                          float*                arrival);

// outColor is channelCount planes of pixelCount floats; both outputs are
// overwritten.
void resolveStreamCPU(StreamPlanes&       planes,
                      float* __restrict__ outColor,
                      float* __restrict__ outAlpha);

// ===========================================================================
//
//  PER-BAND LAZY-CLAIM CONCURRENCY
//
//  The node's frame is computed lazily, one horizontal band at a time, by
//  whichever of Nuke's render threads asks for a row in that band first.  The
//  state machine lives HERE, NDK-free, so it is unit-testable with plain
//  std::thread (the same property scatterStreamCPU has); the node instantiates
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
// The state planes ALONE under-budget by orders of magnitude: the fragment
// stream is the larger term at any real sample count.  The combined figure is
//
//   W*B*(C+6)*4                  the stream state (StreamPlanes): claimed
//                                area, alpha, the two recency-chunk floats,
//                                the last CoC, arrival and C colour planes.
//                                No factor of depth_layers anywhere.
// + 2*W*(B+2*padY)*4             the virtual-background window (T +
//                                residual radius planes), sized to the WINDOW height
//                                (B+2*padY, clipped to the output box; see
//                                residualWindowYRange()), not B alone. One
//                                BandJob per CONCURRENT band owns it exactly
//                                like the state planes (ResidualWindow), so
//                                it is COUNTED here, not named as an
//                                omission. Measured 8.72 MB per 4096x64 band
//                                at the max_radius=100 / edge_softness=1
//                                defaults (padY=101): windowHeight=64+2*101
//                                =266, 2*4096*266*4 = 8,716,288 B.
// + (K+1)*W*B*4                  the holdout transmittance LUT, when a holdout
//                                is connected (`holdoutLayers` is K, the
//                                depth_layers count): 17.0 MB per 4096x64
//                                band at K=16 (17*4096*64*4 = 17,825,792 B;
//                                spp-independent)
// + fragments * kSoAResidentBytesPerFragment
//                                the fragment stream at its RESIDENT cost:
//                                the SoA (4*6 + 1 + 4*C B logical, 37 B at
//                                C = 3) with PodBuffer's geometric capacity
//                                slack, plus the sort's order array and its
//                                scratch (keys, alternate keys, alternate
//                                order: 16 B, sized exactly)
//
// kSoAResidentBytesPerFragment is measured, not derived: on the profile rig
// (2048x1080, 20 spp, C = 3, K = 16, 2 threads, 34 bands of 32 rows) the
// per-band capacity of SoA + order + sort scratch over that band's fragment
// count, read from DEEPC_DEFOCUS_DEBUG_STATS (soaBytes, orderSortBytes), is
// median 65.98 B (SoA 49.98 + sort 16.00), range 65.69-68.80 B.  The top of
// the range is a pooled job reusing the capacity of a larger earlier band.
//
// `fragmentEstimate` is the caller's own forecast of the band's fragment
// count.  The node derives it from the depth-range pass's per-row sample
// counts over the band's FETCH window (band +/- padY — the fetch rows are
// what get flattened, not just the band's own rows), which over-counts
// alpha<=0 samples the flatten drops and every sample the merges collapse,
// and under-counts volumetric pieces.  On the profile rig the over-count
// dominates: worst band estimate 1,720,320 against at most 430,080 emitted
// fragments, so the figure promises about 4x the stream's real resident size
// there; a volumetric-heavy frame (up to depth_layers + 1 pieces per span)
// is where it can under-promise.
//
// `padY` defaults to 0 (no virtual-background window reach) so a caller that
// does not pass it still gets a term — 2*W*B*4 at padY=0, the window's
// UNPADDED size — never a silent zero; the real caller (frameSetup()) always
// passes its actual padY.
//
// NOT a per-band term: the background fill's surface map and its pyramid
// (surfaceMapFrameBytes, DeepCDefocusFill.h).  There is ONE per frame, over
// the whole output box, so frameSetup() counts it once against the limit
// before the bands are planned from what is left.
//
// Also omitted: the band's own output planes (W*B*(C+1)*4, plus the matte
// when it is on) and the flatten/scatter scratch, which is sized per PIXEL
// (one pixel's sample count) or per kernel row, not per band.
//
// The plan may pick any band height and any concurrency: no pixel depends on
// either (see the streaming composite's pipeline note above), so the limit
// trades memory against parallelism and never against the image.
// ---------------------------------------------------------------------------
constexpr double kSoAResidentBytesPerFragment = 70.0;

inline double bandBudgetBytes(int holdoutLayers, int channelCount, int width,
                              int height, bool holdoutConnected,
                              double fragmentEstimate, int padY = 0)
{
    double bytes = static_cast<double>(
        StreamPlanes::bytesForBand(channelCount, width, height));
    bytes += static_cast<double>(
        ResidualWindow::bytesForWindow(width, height + 2 * padY));
    if (holdoutConnected && holdoutLayers > 0 && width > 0 && height > 0) {
        bytes += static_cast<double>(holdoutLayers + 1)
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
// The policy, in one testable place:
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
//
// `padY` (default 0) is the virtual-background window's reach beyond the
// band on each side — see bandBudgetBytes(). Trailing and defaulted so every
// existing caller stays source-compatible; the real caller passes its actual
// padY.
// ---------------------------------------------------------------------------
struct BandPlan {
    int bandHeight  = 1;
    int bandCount   = 0;
    int maxInFlight = 1;
};

// The worst over the frame's bands of the sum of `rowCounts` (one entry per
// source row, index y - srcY0) over the band's fetch window, band +/- padY
// rows clipped to the source rows.  The node feeds planBands() this over its
// per-row deep-sample counts; in fill: background mode each non-empty pixel
// can gain one synthetic sample, so the row counts handed in must already
// carry that pixel count too.
inline double worstFetchWindowSum(const std::vector<double>& rowCounts,
                                  int srcY0,
                                  int boxY0, int boxY1,
                                  int bandHeight, int padY)
{
    const std::size_t nRows = rowCounts.size();
    std::vector<double> prefix(nRows + 1, 0.0);
    for (std::size_t i = 0; i < nRows; ++i)
        prefix[i + 1] = prefix[i] + rowCounts[i];

    const int srcY1 = srcY0 + static_cast<int>(nRows);
    const int b     = (bandHeight > 0) ? bandHeight : 1;
    double worst = 0.0;
    for (int y0 = boxY0; y0 < boxY1; y0 += b) {
        const int y1  = (y0 + b < boxY1) ? y0 + b : boxY1;
        const int fy0 = (srcY0 > y0 - padY) ? srcY0 : y0 - padY;
        const int fy1 = (srcY1 < y1 + padY) ? srcY1 : y1 + padY;
        if (fy1 <= fy0)
            continue;
        const double s = prefix[static_cast<std::size_t>(fy1 - srcY0)]
                       - prefix[static_cast<std::size_t>(fy0 - srcY0)];
        if (s > worst)
            worst = s;
    }
    return worst;
}

template <typename FragmentsForBandHeight>
inline BandPlan planBands(double memoryLimitBytes,
                          int    frameHeight,
                          int    holdoutLayers,
                          int    channelCount,
                          int    width,
                          bool   holdoutConnected,
                          int    initialBandHeight,
                          FragmentsForBandHeight&& fragmentsForBandHeight,
                          int    padY = 0)
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

    double bytes = bandBudgetBytes(holdoutLayers, channelCount, width, b,
                                   holdoutConnected, fragmentsForBandHeight(b), padY);
    while (b > 1 && bytes > memoryLimitBytes) {
        b = (b / 2 > 0) ? b / 2 : 1;
        bytes = bandBudgetBytes(holdoutLayers, channelCount, width, b,
                                holdoutConnected, fragmentsForBandHeight(b), padY);
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
//   2. Otherwise tryClaimDirtyBand(key, band): a NON-blocking claim of the
//      first Dirty band from `band` onwards, which is what keeps the other
//      render threads off the monitor while one of them computes the band
//      they all asked for (see that function).
//   3. Only when that finds nothing, acquireBand(key, band): blocks while the
//      band is InProgress (or while the in-flight cap is full), and returns
//        Compute — the caller now OWNS the band: compute it into private
//                  state planes, write its disjoint region of the shared
//                  frame, then completeBand() (or abandonBand() on abort);
//        Ready   — another thread finished it while we waited;
//        Aborted — abortedFn() went true while waiting;
//        Stale   — the setup key no longer matches: go back to beginFrame().
//   4. beginFrame(key): the same claim pattern for the FRAME-GLOBAL setup
//      (depth range, holdout boundaries, kernel LUT, band decomposition, the shared
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
                // Readers never signal, so this drain has to poll -- but NOT
                // with the monitor's timed wait.  DD::Image::SignalLock::
                // wait(ms) builds an ABSOLUTE timespec of {0, ms*1000}, which
                // is always in the past, so pthread_cond_timedwait returns
                // ETIMEDOUT at once and the loop would spin on the lock
                // (measured: 0.0106 ms per call, not 1 ms).  Sleeping off the
                // lock is also the same wait in the tests as in the node.
                while (_activeReaders.load() != 0) {
                    _monitor.unlock();
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    _monitor.lock();
                }
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

    // Non-blocking claim of a Dirty band inside [first, last], searched
    // forward from `from` and wrapping within that window.  Returns the
    // claimed band, or -1 when there is nothing to claim right now (no Dirty
    // band in the window, the in-flight cap is full, the setup key has moved
    // on, or `from` lies outside the window).  A claim carries the same
    // obligation as acquireBand()'s Compute: completeBand() or abandonBand().
    //
    // WHY THIS EXISTS.  Nuke hands its render threads CONSECUTIVE rows, so all
    // of them sit inside one band; without this the first arrival computes it
    // and the rest block in acquireBand() for the whole of that compute.
    // Measured back to back on a 2048x1080 / 20-samples-per-pixel frame (34
    // bands of 32 rows), four render threads on four cores: without this call
    // 22.66 s median wall, 1.47 bands in flight, 1.72 of 4 cores busy; with it
    // 18.74 s, 2.62 bands, 2.76 cores.  The same 34 bands are computed either
    // way.  On a frame where the deep-input pull is a smaller share of the
    // band (1024x540, max_radius 100, radii to 10 px) it is 10.66 s -> 6.80 s.
    //
    // THE WINDOW IS NOT OPTIONAL.  The band decomposition covers the node's
    // PADDED output box, and engine() is never called outside Iop::
    // requestedBox() — so an unwindowed search hands waiters the pad bands
    // nobody will ever ask for.  Measured with the window left out, on that
    // 1024x540 frame: 24 bands computed against the 18 that carry a requested
    // row, and 16.06 s against 9.74 s median wall — a LOSS.  The caller passes
    // the band range its requested box covers.
    int tryClaimDirtyBand(std::uint64_t key, int from, int first, int last)
    {
        _monitor.lock();
        if (!_setupDone || _setupKey != key || _bandCount <= 0
            || _inFlight >= _maxInFlight) {
            _monitor.unlock();
            return -1;
        }
        if (first < 0)          first = 0;
        if (last > _bandCount - 1) last = _bandCount - 1;
        if (first > last) {
            _monitor.unlock();
            return -1;
        }
        // PRECONDITION: `from` is inside the window.  Outside it the only
        // safe answer is none -- clamping would hand the caller a band its
        // own row request cannot use, which costs a whole band's compute
        // before it reaches the one it asked for.
        if (from < first || from > last) {
            _monitor.unlock();
            return -1;
        }
        const int span = last - first + 1;
        for (int i = 0; i < span; ++i) {
            const int band = first + (from - first + i) % span;
            if (static_cast<BandState>(
                    _states[band].load(std::memory_order_relaxed))
                != BandState::Dirty)
                continue;
            _states[band].store(static_cast<std::uint8_t>(BandState::InProgress),
                                std::memory_order_relaxed);
            ++_inFlight;
            _monitor.unlock();
            return band;
        }
        _monitor.unlock();
        return -1;
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
