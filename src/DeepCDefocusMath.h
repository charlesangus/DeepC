// SPDX-License-Identifier: MIT
//
// ============================================================================
//
//  DeepCDefocusMath — Header-only lens/CoC and holdout-visibility math
//
//  The numerical foundation of DeepCDefocus: circle-of-confusion evaluation
//  (physical and manual), ray-distance -> Z correction, and the holdout
//  transmittance model (per-sample in-span exponential attenuation plus the
//  per-destination-pixel boundary LUT the scatter core interpolates).
//
//  Zero Nuke SDK dependencies — only standard library headers, so this file
//  compiles and unit-tests with a plain `g++ -std=c++17`.
//
//  Per-fragment / per-sample scalar entry points are marked DEEPC_HD: they
//  take raw pointers and scalars only, allocate nothing, and throw nothing,
//  so a later CUDA milestone can compile this same header under nvcc with
//  DEEPC_HD pre-defined as `__host__ __device__`.  Functions that walk whole
//  sample lists to fill a caller-owned buffer are host-side builders and are
//  deliberately NOT marked.
//
// ============================================================================

#ifndef DEEPC_DEEPCDEFOCUS_MATH_H
#define DEEPC_DEEPCDEFOCUS_MATH_H

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

// Device/host seam.  A .cu translation unit pre-defines this as
// `__host__ __device__` before including the header; on the CPU it vanishes.
#ifndef DEEPC_HD
#define DEEPC_HD
#endif

namespace deepc {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// Floor for (S_mm - f).  A focus distance at or inside the focal length would
// otherwise divide by zero (or flip the sign of cocScale, inverting front and
// back bokeh); clamping keeps cocScale large-but-finite and max_radius bounds
// the result.
constexpr float kFocusFocalEpsilonMm = 1e-4f;

// Transmittance floor used by the log-space interpolation.  log(0) is -inf, so
// a fully opaque boundary is represented by this value and any interpolated
// result at or below it collapses to exactly 0.
//
// The floor also sets how fast an opaque holdout edge falls off *inside* a
// bucket: interpolating between T=1 and T=0 gives exp(0.5*log(floor)) at the
// half-way point, i.e. 1e-4 at a 1e-8 floor.  In scene-linear HDR a 1e-4
// transmittance leak over a 1e4 background is a visible ~1.0 halo one bucket
// deep, so the floor is set far below any perceptually relevant transmittance
// (1e-15 at the half-way point) while staying comfortably inside normal float
// range and safe for std::log/std::exp.
constexpr float kMinTransmittance = 1e-30f;

// Saturating stand-in for an unbounded circle of confusion.  As d -> 0+ the
// true CoC diameter grows without bound and eventually overflows float;
// returning a large finite value (rather than 0, which would snap the near
// field back to "sharp", or inf, which would turn into NaN on the first
// multiply by a zero scale) lets the caller's max_radius clamp do its job.
constexpr float kSaturatedCocMm = 1e30f;

// ---------------------------------------------------------------------------
// Small scalar helpers (device-callable, no std::min/max dependency)
// ---------------------------------------------------------------------------

// NaN-safe: a NaN input returns `lo`, so a poisoned alpha/depth/knob degrades
// to the low end of the range instead of propagating NaN into the accumulator.
// Callers are still responsible for passing lo <= hi.
DEEPC_HD inline float clampf(float v, float lo, float hi)
{
    return v > lo ? (v < hi ? v : hi) : lo;
}

// Integer twin of clampf (no NaN case to worry about).  Used for the K-bucket
// knob clamp, which must never let a garbage knob value size a loop.
DEEPC_HD inline int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Double-precision clamp, used only by the host-side bucket builder (which
// works in double so that the inverse-depth inversion stays well conditioned
// when the focal plane is very near and the far depth very distant).  Same
// NaN behaviour as clampf: a NaN returns `lo`.
inline double clampd(double v, double lo, double hi)
{
    return v > lo ? (v < hi ? v : hi) : lo;
}

// ---------------------------------------------------------------------------
// World units — scene depth unit -> millimetres
//
// Physical-mode CoC math is entirely in mm; mixing scene units with mm lens
// values is wrong by orders of magnitude, so every depth crosses through
// unitScale() first.
// ---------------------------------------------------------------------------
enum class WorldUnits {
    Millimeters = 0,
    Centimeters = 1,
    Decimeters  = 2,
    Meters      = 3,
    Inches      = 4,
    Feet        = 5
};

DEEPC_HD inline float unitScale(WorldUnits u)
{
    switch (u) {
        case WorldUnits::Millimeters: return 1.0f;
        case WorldUnits::Centimeters: return 10.0f;
        case WorldUnits::Decimeters:  return 100.0f;
        case WorldUnits::Meters:      return 1000.0f;
        case WorldUnits::Inches:      return 25.4f;
        case WorldUnits::Feet:        return 304.8f;
        default:                      return 1000.0f;
    }
    return 1000.0f;
}

// ---------------------------------------------------------------------------
// CocMode
// ---------------------------------------------------------------------------
enum class CocMode {
    Physical = 0,
    Manual   = 1
};

// ---------------------------------------------------------------------------
// CocParams — precomputed lens state, rebuilt once per _validate
//
// Physical model, inverse-depth form (robust as d -> infinity):
//
//   S_mm      = focusDistance * unitScale
//   d_mm      = depth * unitScale,            invD = 1 / d_mm   (d=inf => 0)
//   cocScale  = (f/N) * f / max(S_mm - f, eps)
//   coc_mm(d) = cocScale * |1 - S_mm * invD|
//   coc_px    = coc_mm / filmbackWidthMm * formatWidthPx
//   radius    = (coc_px / 2) * (d < S ? frontMult : backMult), clamped
//
// Manual model works in raw scene units, skips the mm conversion entirely and
// — unlike the physical branch — does NOT halve: `size` is already a radius.
//
//   radius    = size * |1 - S / d| * (d < S ? frontMult : backMult), clamped
//
// The derived members (_focusDistanceMm, _invFocusDistanceMm, _cocScale,
// _pxPerMm) are cached, not inputs: assign the inputs then call
// recomputeDerived(), or build the whole thing with makeCocParams().
// ---------------------------------------------------------------------------
struct CocParams {
    // --- inputs ---
    CocMode _mode            = CocMode::Physical;
    float   _focalLengthMm   = 50.0f;   // f
    float   _fStop           = 2.8f;    // N
    float   _filmbackWidthMm = 36.0f;
    float   _focusDistance   = 10.0f;   // S, scene units
    float   _unitScale       = 1000.0f; // scene unit -> mm (default: metres)
    float   _formatWidthPx   = 1920.0f;
    float   _pixelAspect     = 1.0f;    // kernel Y scale; radius itself is in X px
    float   _frontMult       = 1.0f;    // d < S
    float   _backMult        = 1.0f;    // d > S
    float   _maxRadiusPx     = 100.0f;
    float   _size            = 10.0f;   // Manual mode blur *radius* in px at d=inf

    // --- derived (recomputeDerived()) ---
    float _focusDistanceMm    = 10000.0f;
    float _invFocusDistanceMm = 1e-4f;
    float _cocScale           = 0.0f;
    float _pxPerMm            = 0.0f;   // formatWidthPx / filmbackWidthMm

    void recomputeDerived()
    {
        _focusDistanceMm = _focusDistance * _unitScale;
        _invFocusDistanceMm = (_focusDistanceMm > 0.0f && std::isfinite(_focusDistanceMm))
                            ? 1.0f / _focusDistanceMm
                            : 0.0f;

        // Clamped so focus at/inside the focal length can neither divide by
        // zero nor produce a negative (front/back-swapped) cocScale.
        float denom = _focusDistanceMm - _focalLengthMm;
        if (!(denom > kFocusFocalEpsilonMm))
            denom = kFocusFocalEpsilonMm;

        const float aperture = (_fStop > 0.0f) ? (_focalLengthMm / _fStop) : 0.0f;
        _cocScale = aperture * _focalLengthMm / denom;

        _pxPerMm = (_filmbackWidthMm > 0.0f) ? (_formatWidthPx / _filmbackWidthMm) : 0.0f;
    }
};

inline CocParams makeCocParams(CocMode mode,
                               float focalLengthMm,
                               float fStop,
                               float filmbackWidthMm,
                               float focusDistance,
                               float unitScaleValue,
                               float formatWidthPx,
                               float pixelAspect,
                               float frontMult,
                               float backMult,
                               float maxRadiusPx,
                               float sizePx)
{
    CocParams p;
    p._mode            = mode;
    p._focalLengthMm   = focalLengthMm;
    p._fStop           = fStop;
    p._filmbackWidthMm = filmbackWidthMm;
    p._focusDistance   = focusDistance;
    p._unitScale       = unitScaleValue;
    p._formatWidthPx   = formatWidthPx;
    p._pixelAspect     = pixelAspect;
    p._frontMult       = frontMult;
    p._backMult        = backMult;
    p._maxRadiusPx     = maxRadiusPx;
    p._size            = sizePx;
    p.recomputeDerived();
    return p;
}

// ---------------------------------------------------------------------------
// cocMillimeters — physical CoC *diameter* on the filmback, in mm
//
// Independent of _mode (Manual mode has no physical diameter); this is the
// quantity a lens table quotes, e.g. f=50, N=2.8, S=2m, d=4m -> 0.2289mm.
// depth <= 0, NaN depth and any NaN-producing knob combination yield 0.  A
// genuine near-field overflow (d -> 0+, where the true CoC is unbounded)
// saturates to kSaturatedCocMm so the caller clamps to max_radius rather than
// snapping back to sharp.
// ---------------------------------------------------------------------------
DEEPC_HD inline float cocMillimeters(const CocParams& p, float depth)
{
    if (!(depth > 0.0f))            // also rejects NaN
        return 0.0f;

    const float dMm = depth * p._unitScale;
    if (dMm == p._focusDistanceMm)  // exactly in focus — no rounding slop
        return 0.0f;

    // invD = 0 at d = infinity: the inverse-depth form degenerates gracefully
    // to the far-field limit coc = cocScale instead of overflowing.
    const float invD = std::isfinite(dMm) ? (1.0f / dMm) : 0.0f;
    const float coc  = p._cocScale * std::fabs(1.0f - p._focusDistanceMm * invD);
    if (std::isfinite(coc))
        return coc;
    // NaN => a poisoned knob (non-finite focal length / focus distance): fail
    // safe to "sharp".  +/-inf => the near-field limit really did overflow:
    // saturate so max_radius bounds it.
    return std::isnan(coc) ? 0.0f : kSaturatedCocMm;
}

// ---------------------------------------------------------------------------
// signedCocPixels — bokeh radius in pixels, signed by side of the focal plane
//
// SIGN CONVENTION: negative in FRONT of the focal plane (depth < S, nearer the
// camera), positive BEHIND it (depth > S), exactly 0 at depth == S.  The
// magnitude is the clamped radius, so radiusPixels() == |signedCocPixels()|.
//
// Guaranteed edge behaviour (each independently testable):
//   depth == +infinity -> finite radius (invD = 0)
//   depth == S         -> exactly 0
//   depth <= 0         -> 0
//   NaN depth          -> 0
//   depth -> 0+        -> unbounded in theory, clamped to _maxRadiusPx here
//   any NaN knob       -> 0 (fail safe to sharp, never NaN into the scatter)
//   _maxRadiusPx <= 0  -> 0
// ---------------------------------------------------------------------------
DEEPC_HD inline float signedCocPixels(const CocParams& p, float depth)
{
    if (!(depth > 0.0f))            // also rejects NaN
        return 0.0f;

    // Unmultiplied radius in pixels, before the front/back bias and the clamp.
    float radiusPx = 0.0f;

    if (p._mode == CocMode::Manual) {
        // Unitless ratio — scene units cancel, no mm conversion needed, and
        // NO halving: `size` is the blur *radius* in pixels at d = infinity
        // (resolved convention, see the milestone's Decisions log — the knob
        // table's "radius at infinity" is the user-facing contract and the CoC
        // model block was amended to match).  The physical branch below keeps
        // its /2 because it genuinely computes a CoC *diameter*.
        if (depth == p._focusDistance)
            return 0.0f;
        const float invD = std::isfinite(depth) ? (1.0f / depth) : 0.0f;
        radiusPx = p._size * std::fabs(1.0f - p._focusDistance * invD);
    } else {
        radiusPx = 0.5f * cocMillimeters(p, depth) * p._pxPerMm;
    }

    const bool  inFront = (depth < p._focusDistance);
    const float mult    = inFront ? p._frontMult : p._backMult;

    // Sanitised separately: clampf() requires lo <= hi, and a negative, NaN or
    // infinite max_radius must degrade to "no blur", not to a negative or
    // unbounded radius.
    const float maxR = (p._maxRadiusPx > 0.0f && std::isfinite(p._maxRadiusPx))
                     ? p._maxRadiusPx : 0.0f;

    float radius = radiusPx * mult;
    if (std::isnan(radius))         // poisoned knob upstream: degrade to sharp
        return 0.0f;
    if (!std::isfinite(radius))     // near-field saturation: bounded by max_radius
        radius = maxR;

    radius = clampf(radius, 0.0f, maxR);

    if (!(radius > 0.0f))           // avoid handing back -0.0
        return 0.0f;

    return inFront ? -radius : radius;
}

// ---------------------------------------------------------------------------
// radiusPixels — clamped bokeh radius magnitude in pixels
// ---------------------------------------------------------------------------
DEEPC_HD inline float radiusPixels(const CocParams& p, float depth)
{
    return std::fabs(signedCocPixels(p, depth));
}

// ---------------------------------------------------------------------------
// rayDistanceToZ — convert a ray-length depth channel to camera-space Z
//
//   z = rayDist * f / sqrt(f^2 + r_mm^2)
//
// r_mm is the radial offset of the pixel on the filmback (see
// filmbackRadiusMm).  Kept free of any pixel/format geometry so it stays a
// trivially device-callable scalar op.
// ---------------------------------------------------------------------------
DEEPC_HD inline float rayDistanceToZ(float rayDist, float focalLengthMm, float radialOffsetMm)
{
    if (!(focalLengthMm > 0.0f))
        return rayDist;
    const float denom = std::sqrt(focalLengthMm * focalLengthMm +
                                  radialOffsetMm * radialOffsetMm);
    if (!(denom > 0.0f))
        return rayDist;
    return rayDist * focalLengthMm / denom;
}

// ---------------------------------------------------------------------------
// filmbackRadiusMm — radial filmback offset of a pixel, in mm
//
// x/y are pixel coordinates in the format (pixel centres, origin bottom-left).
// Pixel aspect is width/height of a pixel, so the physical filmback height is
// filmbackWidthMm * H / (W * pixelAspect) and mm-per-pixel in Y is the X value
// divided by the aspect.
// ---------------------------------------------------------------------------
DEEPC_HD inline float filmbackRadiusMm(float x, float y,
                                       float formatWidthPx, float formatHeightPx,
                                       float filmbackWidthMm, float pixelAspect)
{
    if (!(formatWidthPx > 0.0f) || !(filmbackWidthMm > 0.0f))
        return 0.0f;

    const float mmPerPxX = filmbackWidthMm / formatWidthPx;
    const float mmPerPxY = (pixelAspect > 0.0f) ? (mmPerPxX / pixelAspect) : mmPerPxX;

    const float dx = (x - 0.5f * formatWidthPx) * mmPerPxX;
    const float dy = (y - 0.5f * formatHeightPx) * mmPerPxY;
    return std::sqrt(dx * dx + dy * dy);
}

// ---------------------------------------------------------------------------
// HoldoutVisibility — deep holdout transmittance
//
// A fragment's visibility at a destination pixel is the product over all
// holdout samples in front of it of (1 - alpha).  A *volumetric* holdout
// sample that contains the fragment attenuates only partially: opacity is
// assumed uniform along the span, which makes transmittance exponential in
// depth, so the in-span law is (1 - alpha)^t with t the normalised position
// inside the span.
//
// Per-fragment binary search over holdout samples is far too slow (the scatter
// touches O(sum of pi*r^2) fragment-pixels), so the scatter core instead
// evaluates transmittance once per destination pixel at a fixed set of
// boundary depths (build()) and interpolates between two of them per fragment
// (interp/interpAtBucket).  Because transmittance is exponential in depth,
// interpolating in LOG space is exact for the model, not an approximation:
// log T is linear in z across any interval that no holdout sample starts or
// ends inside.  Where a sample edge does fall strictly between two boundaries,
// log T is piecewise linear there and the interpolation is the (still
// monotone) chord — the one bracket-wide approximation the design accepts in
// exchange for O(1) per fragment.
//
// THOSE BOUNDARIES ARE **HoldoutBoundaries**, NOT DepthBuckets' (M1.P3.T10).
// The design reference's "at the K+1 bucket boundaries" is wrong and was
// replaced: the ΔCoC bucket spacing bounds banding, not occlusion, and sampling
// this LUT at it made an opaque card at z=50 start occluding at z=10.9.  See
// HoldoutBoundaries for the measurement and for why uniform-in-z won.  The
// chord's accepted worst case, (T0-T1)/2, is only meaningful once the
// boundaries are placed by an occlusion criterion; nothing in this struct
// depends on WHICH ascending boundary array it is handed.
//
// Buffers are plain contiguous float arrays owned by the caller: one LUT is
// built per destination pixel per band, so a per-pixel std::vector would be a
// performance defect.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// HoldoutInterp — interpAtBucket()'s selectable opaque-step alternates (M1.P3.T11)
//
// M1.P3.T10 fixed the LUT's boundary PLACEMENT; this is the INTERPOLANT half
// of the same residual, measured not to be irreducible after all.  The
// shipped log chord floors log T at kMinTransmittance, so an opaque step
// (a fully-opaque far boundary, T1 == 0) collapses vis to ~0 across almost
// the whole bracket instead of the monotone-chord bound's 0.5, and always
// toward camera — a holdout card fully erases genuinely-unoccluded geometry
// for most of one bracket in front of it (5.07 of a 6.19-unit bracket at
// K=16 on the default rig).
//
// Both alternates below fire ONLY when the far boundary transmittance is
// EXACTLY zero.  THIS IS NOT THE SAME THING AS "fully-opaque holdout content"
// and the two must not be conflated (found at this task's independent
// review, which is why this paragraph replaces the original "cannot move any
// alpha<1 case" claim). A single alpha==1 sample is one way to reach T1==0.
// The OTHER way is float underflow of the cumulative transmittance product
// over a sufficiently long/dense run of alpha<1 samples landing in one
// bracket: build()'s running product and evalExact()'s are both plain
// float accumulation, and (1-alpha)^n underflows to a bitwise 0.0f at
// measured n=150 (alpha=0.5), n=87 (alpha=0.7), n=46 (alpha=0.9), n=23
// (alpha=0.99) -- all ordinary sample counts for a dense volumetric holdout
// column (fog/smoke), i.e. validation scene (f)'s content class. A
// synthetic 20,000-trial sweep over 20-200-sample alpha<1 stacks hit a
// bitwise-zero boundary in 52% of trials; a 1-4-sample sweep (200,000
// trials) hit zero, which is almost certainly why the original "unchanged"
// verification missed this -- small holdout sample counts can't underflow.
// When this happens, MidpointStep/LinearInT are NOT bit-identical to
// LogChord for that bracket even though every constituent sample has
// alpha<1: measured divergence up to LogChord 0.123 vs LinearInT 0.970 vs
// MidpointStep 1.000 at the same depth, for a 46-sample alpha=0.9 stack.
// There is no cheap fix within this task's budget (zero added memory, O(1),
// ~4 lines): (t0, t1) alone cannot distinguish "one truly-opaque sample" from
// "many alpha<1 samples whose product underflowed" -- both present
// identically as a non-degenerate t0 and a bitwise-zero t1. This is a real,
// bounded gap in the safety argument, not a hypothetical one: M1.P3.T5 MUST
// render a genuine dense volumetric holdout (not just a discrete opaque
// card) under all three variants before judging from pixels, and the
// milestone's Decisions log must record this correction. See HoldoutLut's
// "WHAT IS LEFT" note and the milestone's M1.P3.T10 Decisions entry ("THE
// RESIDUAL, STATED PLAINLY") for the rest of the measurement:
//
//   variant                    headline mean   erased in front   leaks behind
//   LogChord (shipped)         0.0565          5.07 u            0.00 u
//   MidpointStep on T1==0      0.0262          2.59 u            <= half bracket
//   LinearInT on T1==0         0.0266          0.00 u            0.49 u (6.16 worst)
//
// The trade is erasing FG in front vs leaking BG behind; scene (e) can fail
// either way, which is why M1.P3.T5 judges it from rendered pixels rather
// than this task deriving it.  All three are selectable at runtime (a knob
// value, not a build flag) so T5 can render all three; the losing two and
// this enum's unused values are deleted at T5.
// ---------------------------------------------------------------------------
enum class HoldoutInterp : std::uint8_t {
    LogChord     = 0,   // shipped: log-space chord, floored at kMinTransmittance
    MidpointStep = 1,   // T1==0 only: hard step at the bracket midpoint
    LinearInT    = 2    // T1==0 only: linear ramp from T0 to 0 across the bracket
};

struct HoldoutVisibility {

    // -----------------------------------------------------------------------
    // inSpan — transmittance contributed by ONE holdout sample at depth z
    //
    //   z <  zFront            -> 1                     (fragment is in front)
    //   z >= zBack             -> (1 - alpha)           (fragment is behind)
    //   otherwise              -> (1 - alpha)^((z-zFront)/(zBack-zFront))
    //
    // Zero-thickness samples (zBack <= zFront) are point samples: full
    // (1 - alpha) from zFront onward, 1 before it — no division by zero.
    // -----------------------------------------------------------------------
    static DEEPC_HD inline float inSpan(float zFront, float zBack, float alpha, float z)
    {
        const float a = clampf(alpha, 0.0f, 1.0f);
        const float oneMinusA = 1.0f - a;

        // Degenerate case first: a point sample has no span to interpolate
        // across, so it is fully applied from zFront onward (checking this
        // before the in-front test is what keeps build()'s fast path — which
        // folds any sample with zBack <= z into its running product — in
        // agreement with evalExact() at exactly z == zFront == zBack).
        if (!(zBack > zFront))
            return (z >= zFront) ? oneMinusA : 1.0f;

        if (!(z > zFront))          // in front of (or exactly at) the sample
            return 1.0f;
        if (z >= zBack)
            return oneMinusA;
        if (oneMinusA <= 0.0f)      // fully opaque: nothing gets through
            return 0.0f;

        const float t = (z - zFront) / (zBack - zFront);
        return std::pow(oneMinusA, t);
    }

    // -----------------------------------------------------------------------
    // evalExact — visibility at an arbitrary depth, straight from the samples
    //
    // O(sampleCount).  The reference implementation of the model: build() and
    // interp() must agree with this.  Samples are SoA and need not be sorted.
    // -----------------------------------------------------------------------
    static DEEPC_HD inline float evalExact(const float* zFront,
                                           const float* zBack,
                                           const float* alpha,
                                           int sampleCount,
                                           float z)
    {
        float t = 1.0f;
        for (int i = 0; i < sampleCount; ++i) {
            t *= inSpan(zFront[i], zBack[i], alpha[i], z);
            if (t <= 0.0f)
                return 0.0f;
        }
        return t;
    }

    // -----------------------------------------------------------------------
    // build — fill the per-destination-pixel boundary transmittance LUT
    //
    //   zFront/zBack/alpha : one dest pixel's holdout samples, SoA,
    //                        sorted front-to-back by zFront (ascending)
    //   boundaries         : HoldoutBoundaries::boundaries(), ascending
    //                        (K+1 of them — the COUNT is shared with
    //                        DepthBuckets, the PLACEMENT is not; M1.P3.T10)
    //   outT               : caller-owned, at least boundaryCount floats
    //
    // Host-side (loops the whole sample list); the in-span exponential is
    // folded in here so the per-fragment path is pure interpolation.
    //
    // Walks samples and boundaries together: samples entirely in front of the
    // current boundary collapse into a running product, and only the samples
    // straddling it (usually none or one) are evaluated per boundary.  Both
    // orderings above are *preconditions* of that O(H + K) walk, so they are
    // verified up front (O(H + K), no allocation) and violating input falls
    // back to the order-free evalBoundaries().  The result is therefore always
    // bit-identical to evalBoundaries(), sorted input or not.
    // -----------------------------------------------------------------------
    static void build(const float* zFront,
                      const float* zBack,
                      const float* alpha,
                      int sampleCount,
                      const float* boundaries,
                      int boundaryCount,
                      float* outT)
    {
        for (int b = 1; b < boundaryCount; ++b) {
            if (!(boundaries[b] >= boundaries[b - 1])) {
                evalBoundaries(zFront, zBack, alpha, sampleCount,
                               boundaries, boundaryCount, outT);
                return;
            }
        }
        for (int i = 1; i < sampleCount; ++i) {
            if (!(zFront[i] >= zFront[i - 1])) {
                evalBoundaries(zFront, zBack, alpha, sampleCount,
                               boundaries, boundaryCount, outT);
                return;
            }
        }

        int settled = 0;            // samples wholly in front of the boundary
        float cumFront = 1.0f;      // product of (1 - alpha) over [0, settled)

        for (int b = 0; b < boundaryCount; ++b) {
            const float z = boundaries[b];

            // A sample is "wholly in front" once inSpan() can only return
            // (1 - alpha) for it and every later (deeper) boundary.  That is
            // z >= zBack for a real span, but z >= zFront for a degenerate
            // one (zBack <= zFront), which inSpan() treats as a point sample
            // at zFront — hence max(zFront, zBack), not zBack.
            while (settled < sampleCount) {
                const float zf = zFront[settled];
                const float zb = zBack[settled];
                const float effBack = (zb > zf) ? zb : zf;
                if (!(effBack <= z))
                    break;
                cumFront *= (1.0f - clampf(alpha[settled], 0.0f, 1.0f));
                ++settled;
            }

            float t = cumFront;
            // Samples are zFront-ascending, so everything that can still
            // affect this boundary starts at `settled` and ends at the first
            // sample beginning at or after it.
            for (int i = settled; i < sampleCount && zFront[i] < z; ++i) {
                t *= inSpan(zFront[i], zBack[i], alpha[i], z);
                if (t <= 0.0f) {
                    t = 0.0f;
                    break;
                }
            }

            outT[b] = clampf(t, 0.0f, 1.0f);
        }
    }

    // -----------------------------------------------------------------------
    // evalBoundaries — reference fill of the same LUT, exact and order-free
    //
    // O(sampleCount * boundaryCount) and makes no sortedness assumption; used
    // to validate build() and as a fallback for unsorted input.
    // -----------------------------------------------------------------------
    static void evalBoundaries(const float* zFront,
                               const float* zBack,
                               const float* alpha,
                               int sampleCount,
                               const float* boundaries,
                               int boundaryCount,
                               float* outT)
    {
        for (int b = 0; b < boundaryCount; ++b)
            outT[b] = clampf(evalExact(zFront, zBack, alpha, sampleCount, boundaries[b]),
                             0.0f, 1.0f);
    }

    // -----------------------------------------------------------------------
    // interpAtBucket — O(1) visibility from the LUT, log-transmittance lerp
    //
    // The production path: the scatter core already knows a fragment's bracket
    // index and its fraction between boundary[index] and boundary[index+1] —
    // that pair is HoldoutBoundaries::locate(), which is O(1) and closed form.
    // It is NOT DepthBuckets::bucketOf() (whose fraction is measured between
    // bucket *centres*, for the scatter's partition-of-unity split) and, since
    // M1.P3.T10, it is no longer DepthBuckets::locateBoundary() either: that
    // one locates between the ΔCoC BUCKET boundaries, which is a different
    // boundary set from the one this LUT is sampled at.  All three are
    // different numbers for the same depth.
    //
    // Interpolating log T is exact for the exponential in-span model.  A zero
    // boundary transmittance would give log(0) = -inf, so values are floored
    // at kMinTransmittance and a result at/below that floor returns exactly 0.
    //
    // `variant` (M1.P3.T11) selects between the shipped log chord and two
    // opaque-step alternates — see HoldoutInterp.  It defaults to LogChord so
    // every pre-existing 4-argument call site (including the M1.P3.T3/T10
    // unit tests) is untouched and bit-identical.  The alternates only ever
    // read inside the `t1 == 0.0f` branch below; every other bracket is
    // identical across all three variants.  NOTE: "every other bracket" is
    // NOT the same claim as "every alpha<1 bracket" — see HoldoutInterp's
    // comment above the enum for the measured counterexample (a dense
    // alpha<1 stack can drive t1 to bitwise 0.0f by float underflow, with no
    // sample anywhere near alpha==1).
    // -----------------------------------------------------------------------
    static DEEPC_HD inline float interpAtBucket(const float* boundaryT,
                                                int boundaryCount,
                                                int index,
                                                float frac,
                                                HoldoutInterp variant = HoldoutInterp::LogChord)
    {
        if (boundaryCount <= 0)
            return 1.0f;
        if (index < 0)
            return clampf(boundaryT[0], 0.0f, 1.0f);
        if (index >= boundaryCount - 1)
            return clampf(boundaryT[boundaryCount - 1], 0.0f, 1.0f);

        const float t = clampf(frac, 0.0f, 1.0f);
        const float t0 = boundaryT[index];
        const float t1 = boundaryT[index + 1];

        if (t <= 0.0f) return clampf(t0, 0.0f, 1.0f);
        if (t >= 1.0f) return clampf(t1, 0.0f, 1.0f);

        // Opaque-step alternates (M1.P3.T11): fire on a bitwise-zero far
        // boundary transmittance. That is usually a fully-opaque sample, but
        // NOT only that -- a long/dense run of alpha<1 samples can underflow
        // the stored transmittance to bitwise 0.0f too (see HoldoutInterp's
        // comment above the enum). Any alpha<1 bracket whose stored t1 has
        // NOT underflowed keeps t1 > 0 and falls straight through to the
        // untouched log chord below unaffected.
        if (t1 == 0.0f) {
            if (variant == HoldoutInterp::MidpointStep)
                return (t < 0.5f) ? clampf(t0, 0.0f, 1.0f) : 0.0f;
            if (variant == HoldoutInterp::LinearInT)
                return clampf(t0, 0.0f, 1.0f) * (1.0f - t);
        }

        const float l0 = std::log(clampf(t0, kMinTransmittance, 1.0f));
        const float l1 = std::log(clampf(t1, kMinTransmittance, 1.0f));
        const float v  = std::exp(l0 + (l1 - l0) * t);

        return (v <= kMinTransmittance) ? 0.0f : clampf(v, 0.0f, 1.0f);
    }

    // -----------------------------------------------------------------------
    // interp — same interpolation, locating the bracketing boundaries by depth
    //
    // O(log boundaryCount), and correct for ANY ascending boundary array.
    // Convenience/verification entry point (it is what the unit tests check
    // HoldoutBoundaries::locate() + interpAtBucket() against); the scatter core
    // uses the O(1) closed-form pair instead and never calls this.
    //
    // Depths outside the boundary range clamp to the nearest boundary value
    // (the LUT is built to span the frame's depth range, so this is a
    // degenerate path, and clamping keeps visibility monotone in depth).
    // -----------------------------------------------------------------------
    static DEEPC_HD inline float interp(const float* boundaries,
                                        const float* boundaryT,
                                        int boundaryCount,
                                        float z,
                                        HoldoutInterp variant = HoldoutInterp::LogChord)
    {
        if (boundaryCount <= 0)
            return 1.0f;
        if (boundaryCount == 1 || !(z > boundaries[0]))
            return clampf(boundaryT[0], 0.0f, 1.0f);
        if (z >= boundaries[boundaryCount - 1])
            return clampf(boundaryT[boundaryCount - 1], 0.0f, 1.0f);

        // Binary search for the last boundary <= z.
        int lo = 0;
        int hi = boundaryCount - 1;
        while (hi - lo > 1) {
            const int mid = lo + (hi - lo) / 2;
            if (boundaries[mid] <= z)
                lo = mid;
            else
                hi = mid;
        }

        const float span = boundaries[lo + 1] - boundaries[lo];
        const float frac = (span > 0.0f) ? ((z - boundaries[lo]) / span) : 0.0f;
        return interpAtBucket(boundaryT, boundaryCount, lo, frac, variant);
    }
};

// ---------------------------------------------------------------------------
// sampleMidDepth — the depth a deep sample is bucketed and CoC'd at
//
// The design fixes this as the span midpoint.  A sample whose span crosses
// bucket boundaries is expected to have been through splitSpanAtBoundaries()
// first, so by the time this is called the span lies inside a single bucket
// and its midpoint is a faithful representative of it.
//
// Degenerate spans (zBack <= zFront, i.e. a point sample) collapse to zFront;
// a non-finite endpoint falls back to the other one so a poisoned channel
// cannot turn into a NaN depth downstream.
// ---------------------------------------------------------------------------
DEEPC_HD inline float sampleMidDepth(float zFront, float zBack)
{
    const bool okFront = std::isfinite(zFront);
    const bool okBack  = std::isfinite(zBack);
    if (!okFront) return okBack ? zBack : 0.0f;
    if (!okBack)  return zFront;
    if (!(zBack > zFront)) return zFront;
    return zFront + 0.5f * (zBack - zFront);
}

// ---------------------------------------------------------------------------
// partitionAlpha / partitionColorScale — transmittance-preserving alpha split
//
// The one primitive behind BOTH places this node cuts a surface in two:
//
//   1. the volumetric bucket-boundary split (a deep sample spanning several
//      buckets is cut at the boundaries so a fog slab does not collapse into
//      one hard layer), and
//   2. the fractional two-bucket assignment (one fragment deposited into its
//      two adjacent buckets, the mandatory fix for layer-transition banding).
//
// Both are "the same surface, seen as several layers that will later be
// over-composited", so the split must be multiplicative in TRANSMITTANCE, not
// linear in alpha:
//
//     alpha(t) = 1 - (1 - alpha)^t          sum of t == 1  =>  product of
//     colorScale(t) = alpha(t) / alpha      (1 - alpha_i) == (1 - alpha)
//
// which makes the front-to-back over of the parts reproduce the parent
// exactly, for alpha AND for premultiplied colour:
//
//     A_out = 1 - prod(1 - alpha_i)                      = alpha
//     C_out = sum_i C*colorScale_i*prod_{j<i}(1-alpha_j)
//           = (C/alpha) * (1 - prod(1-alpha_i))          = C
//
// Splitting alpha LINEARLY (alpha_i = w_i*alpha) does not: an opaque fragment
// split 50/50 composites to 1 - 0.5*0.5 = 0.75, i.e. a 25% alpha hole on a
// flat opaque field, which would break validation scene (c) ("alpha == 1
// exactly at any CoC").  See the milestone notes — the design reference's
// "Sigma alpha*w*vis" accumulation is linear, and reconciling it with the
// front-to-back composite and the flat-field identity is what forces this
// form.  Note that the two readings agree to first order anyway: for small
// alpha, 1 - (1-alpha)^t -> t*alpha, so dense low-alpha fog is unaffected;
// the forms only diverge as alpha approaches 1, which is exactly where the
// linear reading is wrong.
//
// The kernel (disc) weight is NOT split this way — it is genuine partial
// coverage of distinct destination pixels and stays linear.  Only the depth
// split, which duplicates one surface across layers, is exponential.
//
// KNOWN COST OF THIS FORM — read before relying on the fractional assignment
// as a banding fix.  Exact reconstruction under `over` and a smooth
// bucket-to-bucket fade are mathematically incompatible at alpha == 1: the
// reconstruction identity 1 - (1-alpha_0)(1-alpha_1) == 1 forces at least one
// of the two deposits to be fully opaque for every split, so an opaque
// fragment lands at FULL alpha and FULL colour in BOTH of its buckets and the
// nearer one wins the composite outright.  For alpha == 1 the fractional
// assignment therefore degenerates to a hard quantisation at the bucket
// CENTRES (measured: the composited result is constant as the fragment
// travels from centre[m] to centre[m+1], then steps discontinuously), and an
// opaque fragment slightly past a centre occludes content up to one bucket in
// front of it.  Below roughly alpha 0.9 the fade is smooth and the effect
// falls off with alpha; at fog alphas it vanishes entirely (see the
// first-order note above).  The linear reading has the opposite trade: it
// fades smoothly at every alpha but loses up to 25% of a flat opaque field's
// coverage.  The design reference demands the exact flat-field identity
// (validation scene (c), "alpha == 1 exactly"), which is why this form is the
// one implemented; validation scene (g) (banding on a plane receding through
// focus) is the test that can still find the residual, and resolving it needs
// a design-level decision about the per-bucket coverage/weight plane, not a
// change to this primitive.
//
// Written with expm1/log1p rather than pow so that the small-alpha limit
// keeps full relative precision (1 - (1-a)^t suffers catastrophic
// cancellation when computed directly and a is tiny — exactly the fog case).
//
// Edge behaviour: alpha <= 0 or t <= 0 -> 0; t >= 1 -> alpha; alpha >= 1 and
// t > 0 -> 1 (an opaque surface is opaque in every part it is split into,
// which is what makes the flat-field identity exact).  NaN alpha/t clamp to
// 0 via clampf.
// ---------------------------------------------------------------------------
DEEPC_HD inline float partitionAlpha(float alpha, float t)
{
    const float a = clampf(alpha, 0.0f, 1.0f);
    const float f = clampf(t, 0.0f, 1.0f);

    if (!(a > 0.0f) || !(f > 0.0f))
        return 0.0f;
    if (f >= 1.0f)
        return a;
    if (a >= 1.0f)
        return 1.0f;

    // -expm1(t * log1p(-a)) == 1 - (1-a)^t, accurate for tiny a.
    return clampf(-std::expm1(f * std::log1p(-a)), 0.0f, 1.0f);
}

// Colour scale that goes with partitionAlpha: premultiplied colour scales by
// alpha(t)/alpha so the unpremultiplied colour of every part is unchanged.
// As alpha -> 0 that ratio tends to t (a non-absorbing emissive span splits
// its light linearly), which is the value returned at alpha == 0 exactly, so
// the function is continuous through the zero-alpha case rather than 0/0.
DEEPC_HD inline float partitionColorScale(float alpha, float t)
{
    const float a = clampf(alpha, 0.0f, 1.0f);
    const float f = clampf(t, 0.0f, 1.0f);

    if (!(a > 0.0f))
        return f;                       // emissive limit: linear in t
    if (a >= 1.0f)
        return (f > 0.0f) ? 1.0f : 0.0f;

    return partitionAlpha(a, f) / a;
}

// ---------------------------------------------------------------------------
// BucketWeight — one fragment's fractional two-bucket assignment
//
// `index` is the nearer of the two buckets and `frac` the share going to
// index+1, so the two weights are (1 - frac) and frac.  Those sum to exactly
// 1.0f for every frac in [0,1] under round-to-nearest (for frac >= 0.5,
// 1 - frac is exact by Sterbenz; for frac < 0.5 the rounding error of
// 1 - frac is at most 2^-25, which is below half an ulp of 1.0, so the sum
// rounds back to exactly 1) — the partition-of-unity identity the unit tests
// assert is therefore a hard guarantee, not a tolerance.
//
// INVARIANT from DepthBuckets::bucketOf: index is in [0, bucketCount-1], and
// whenever frac > 0 the second bucket index+1 is also in range.  When frac
// is 0 the "second" bucket carries zero weight and indexHigh() folds it back
// onto `index` so a caller that deposits unconditionally can never write past
// the last plane.
// ---------------------------------------------------------------------------
struct BucketWeight {
    int   index = 0;
    float frac  = 0.0f;

    DEEPC_HD inline float weightLow()  const { return 1.0f - frac; }
    DEEPC_HD inline float weightHigh() const { return frac; }
    DEEPC_HD inline int   indexHigh()  const { return (frac > 0.0f) ? (index + 1) : index; }
};

// ---------------------------------------------------------------------------
// BoundarySpan — a depth's position between two adjacent bucket BOUNDARIES
//
// Distinct from BucketWeight, which is a position between bucket CENTRES.
// This is the pair HoldoutVisibility::interpAtBucket() consumes — but note
// that TWO different locators produce it over TWO different boundary arrays:
// HoldoutBoundaries::locate() (the holdout LUT's own set, the only one that
// may feed interpAtBucket) and DepthBuckets::locateBoundary() (the ΔCoC bucket
// set, for span splitting and pre-merge grouping).  See HoldoutBoundaries.
// ---------------------------------------------------------------------------
struct BoundarySpan {
    int   index = 0;
    float frac  = 0.0f;
};

// ---------------------------------------------------------------------------
// BucketDeposit — what one fragment actually adds to the two bucket planes
//
// Produced by fragmentDeposit(); see partitionAlpha() for why the alphas are
// not simply weightLow()*alpha and weightHigh()*alpha.  The scatter core
// deposits, for each of the two buckets:
//
//     colorPlane += kernelWeight * vis * fragColor * colorScale
//     alphaPlane += kernelWeight * vis * bucketAlpha
//
// i.e. the disc kernel weight and the holdout visibility stay linear and
// multiply on top; only the depth split is exponential.
// ---------------------------------------------------------------------------
struct BucketDeposit {
    int   index0      = 0;
    int   index1      = 0;
    float alpha0      = 0.0f;
    float alpha1      = 0.0f;
    float colorScale0 = 0.0f;
    float colorScale1 = 0.0f;
};

// ---------------------------------------------------------------------------
// DepthBuckets — the frame's K depth buckets and their K+1 boundaries
//
// Boundary spacing is bounded-ΔCoC, NOT equal population: the CoC-radius step
// between adjacent boundaries is uniform on each side of the focal plane, so
// what is held constant across the frame is exactly the quantity that governs
// banding visibility.  The two sides get independent step sizes but share the
// bucket budget in proportion to their CoC spans, which equalises the steps
// across the focal plane up to the rounding of that budget to whole buckets —
// exactly, not approximately, is impossible when a side's proportional share
// is fractional, and the error is largest when one side gets very few buckets
// (measured: S=10 over a [1,100] range at K=16 splits 15/1 and leaves the
// back step 1.5x the front one).  Within one side the step is uniform to
// float rounding, which is the property the design reference actually
// specifies and the one the unit tests assert.
//
// Both sides use the fact that CoC is AFFINE in inverse depth u = 1/d:
//
//     front (d < S):  coc = A_front * (S*u - 1),   u in [1/S,   1/dNear]
//     back  (d > S):  coc = A_back  * (1 - S*u),   u in [1/dFar, 1/S  ]
//
// so a target CoC inverts in closed form with no search, and the boundaries
// come out strictly monotone in depth on each side by construction.  The
// spacing coordinate is the CLAMPED CoC (min(coc, max_radius)), which matters
// in the near field: past the depth where the CoC saturates at `max_radius`
// every fragment has the same radius and so cannot band, and spacing on the
// clamped value spends exactly one bucket on that whole plateau instead of
// starving the range where the radius actually varies.  The plateau bucket is
// still bounded: its clamped-CoC step is the same uniform delta as every
// other bucket's, because the clamped CoC at the near end IS max_radius.
// The cost, which is deliberate but worth knowing when reading a render: the
// whole plateau becomes ONE layer, so the design's accepted "within-bucket
// loss of ordering between different-pixel fragments" applies across all of
// it at once.  With a large aperture that saturates both sides, most of the
// scene's depth can end up in the two outermost buckets.  Spacing on the
// UNclamped CoC would instead spend nearly every bucket inside the plateau,
// where by construction nothing can band, and starve the range where the
// radius actually varies — which is worse for the artefact the K knob exists
// to control.
//
// The measured range is the frame's own [depthMin, depthMax] (from the
// alpha-weighted depth pass), so empty depth ranges are clipped rather than
// bucketed — boundary(0) == depthMin and boundary(K) == depthMax exactly.
//
// STORAGE: fixed-size arrays sized by the knob's documented 4..128 maximum,
// so the object allocates nothing, is trivially copyable, and can be handed
// to a device kernel by value or by a single memcpy.  It costs ~1KB; it is
// built once per frame (bucket boundaries are global — see the design
// reference), never per band and never per fragment.
//
// A default-constructed DepthBuckets is inert: bucketCount() == 0, bucketOf()
// answers {0, 0} and locateBoundary() answers {0, 0}, so a scatter driven by
// an unbuilt instance degrades to "everything in bucket 0" rather than
// reading uninitialised depths.
// ---------------------------------------------------------------------------
struct DepthBuckets {

    // K knob range (Perf > depth_layers, 4..128).  kMaxBoundaries is K+1.
    static constexpr int kMinBuckets    = 4;
    static constexpr int kMaxBuckets    = 128;
    static constexpr int kMaxBoundaries = kMaxBuckets + 1;

    // Depths are clamped into this window before anything else happens.  The
    // low end keeps 1/d finite; the high end keeps a background written at
    // +inf (or at some renderer's 1e30 "far" sentinel) from making boundary
    // arithmetic — midpoints, differences — non-finite.  Neither is a knob
    // limit: both sit far outside any plausible scene scale.
    static constexpr float kMinDepth = 1e-6f;
    static constexpr float kMaxDepth = 1e12f;

    // --- state (built by buildBoundedDeltaCoc) ---
    float _boundaries[kMaxBoundaries] = {};
    float _centres[kMaxBuckets]       = {};
    int   _bucketCount                = 0;
    int   _focusBoundary              = 0;

    // -----------------------------------------------------------------------
    // accessors
    // -----------------------------------------------------------------------
    DEEPC_HD inline int bucketCount() const   { return _bucketCount; }
    DEEPC_HD inline int boundaryCount() const { return _bucketCount + 1; }

    // Raw boundary array — the K+1 depths HoldoutVisibility::build() wants.
    DEEPC_HD inline const float* boundaries() const { return _boundaries; }

    DEEPC_HD inline float boundary(int i) const { return _boundaries[i]; }
    DEEPC_HD inline float centre(int i) const   { return _centres[i]; }

    DEEPC_HD inline float depthMin() const { return _boundaries[0]; }
    DEEPC_HD inline float depthMax() const { return _boundaries[_bucketCount]; }

    // Index of the boundary sitting on the focal plane: buckets
    // [0, focusBoundary) are in front of focus and [focusBoundary, K) behind
    // it.  0 when the whole measured range is behind focus, K when it is all
    // in front — i.e. the "other" side simply has no buckets.
    DEEPC_HD inline int focusBoundary() const { return _focusBoundary; }

    // -----------------------------------------------------------------------
    // cocCoefficient — unclamped CoC radius per unit of |1 - S/d|
    //
    // signedCocPixels() computes, in both modes,
    //     |radius| = clamp(A_side * |1 - S/d|, 0, max_radius)
    // with A = size*mult (Manual) or 0.5*cocScale*pxPerMm*mult (Physical).
    // This is the only place the bucket builder needs to know about the lens
    // model, and it is deliberately expressed as the same factorisation
    // signedCocPixels() uses so the two cannot drift apart.  Non-finite or
    // negative combinations degrade to 0, which makes the whole side
    // "all in focus" and sends the builder down its uniform-inverse-depth
    // fallback rather than producing garbage boundaries.
    // -----------------------------------------------------------------------
    static inline float cocCoefficient(const CocParams& p, bool front)
    {
        const float base = (p._mode == CocMode::Manual)
                         ? p._size
                         : 0.5f * p._cocScale * p._pxPerMm;
        const float a = base * (front ? p._frontMult : p._backMult);
        return (a > 0.0f && std::isfinite(a)) ? a : 0.0f;
    }

    // Depth sanitiser: NaN and non-positive depths collapse to kMinDepth,
    // +inf (and any absurd far sentinel) to kMaxDepth.
    static inline float sanitizeDepth(float d)
    {
        if (!(d > kMinDepth))
            return kMinDepth;
        if (!(d < kMaxDepth))
            return kMaxDepth;
        return d;
    }

    // -----------------------------------------------------------------------
    // buildBoundedDeltaCoc — fill the boundaries from the frame's measured
    // depth range
    //
    //   p                : the same CocParams the scatter evaluates radii with
    //   depthMin/depthMax: the frame's measured depth range (alpha-weighted
    //                      depth pass); order is irrelevant, both are clamped
    //   requestedBuckets : the depth_layers knob, clamped to [4, 128]
    //
    // Host-side builder (runs once per cook, walks the bucket list, uses
    // double internally) — deliberately NOT marked DEEPC_HD, matching this
    // header's convention for whole-list builders.  The double is not
    // decoration: the back-side inversion computes 1 - coc/A with coc/A in
    // [0,1] and then divides by S, so in float a very near focal plane with a
    // very distant background loses most of its significant digits; in double
    // that same case is exact to ~1e-16 relative.
    //
    // Post-conditions (each one directly asserted by the unit tests):
    //   * bucketCount() == clamp(requestedBuckets, 4, 128)
    //   * boundary(0) == sanitised depthMin, boundary(K) == sanitised depthMax
    //     (the second to within the fix-up below: if the last interior pair
    //     collapses, boundary(K) is nudged up by an ulp.  It takes K
    //     consecutive collapses reaching the top of the range for that to
    //     happen and it has not been observed even on the degenerate
    //     [1e-6, 1e12] range at K=128, but the guarantee is strict
    //     monotonicity, not an exact top endpoint)
    //   * boundaries are strictly increasing (a fix-up pass nudges any pair
    //     that float rounding collapsed, so bucketOf() can never divide by a
    //     zero span)
    //   * within one side of focus, |radiusPixels(b[i+1]) - radiusPixels(b[i])|
    //     is that side's uniform step (to float rounding), and never exceeds
    //     it — the bounded-ΔCoC property
    // -----------------------------------------------------------------------
    void buildBoundedDeltaCoc(const CocParams& p,
                              float depthMin,
                              float depthMax,
                              int requestedBuckets)
    {
        const int k = clampi(requestedBuckets, kMinBuckets, kMaxBuckets);

        // --- measured range -------------------------------------------------
        float loF = sanitizeDepth(depthMin);
        float hiF = sanitizeDepth(depthMax);
        if (hiF < loF) {
            const float t = loF;
            loF = hiF;
            hiF = t;
        }
        if (!(hiF > loF)) {
            // A single-depth frame (or a degenerate/empty measurement): open
            // the range by a hair so every bucket still has a non-zero extent
            // and the boundaries stay strictly increasing.
            float pad = loF * 1e-4f;
            if (!(pad > 0.0f))
                pad = kMinDepth;
            hiF = loF + pad;
            if (!(hiF < kMaxDepth)) {
                hiF = kMaxDepth;
                loF = hiF - pad;
            }
            if (!(hiF > loF)) {         // unreachable in practice
                loF = kMinDepth;
                hiF = kMinDepth * 2.0f;
            }
        }

        const double lo = static_cast<double>(loF);
        const double hi = static_cast<double>(hiF);

        // --- focal plane ----------------------------------------------------
        // A garbage focus knob (NaN, <= 0) becomes kMinDepth, which puts the
        // whole frame behind focus: a defined, monotone, non-degenerate answer
        // instead of a division by zero in the 1 - S*u inversion.
        double s = static_cast<double>(p._focusDistance);
        if (!(s > 0.0))
            s = static_cast<double>(kMinDepth);
        if (!(s < static_cast<double>(kMaxDepth)))
            s = static_cast<double>(kMaxDepth);

        const double aFront = static_cast<double>(cocCoefficient(p, true));
        const double aBack  = static_cast<double>(cocCoefficient(p, false));
        const double maxR   = (p._maxRadiusPx > 0.0f && std::isfinite(p._maxRadiusPx))
                            ? static_cast<double>(p._maxRadiusPx)
                            : 0.0;

        // The two sides, each clipped to the measured range.  At least one
        // always exists because hi > lo.
        const bool   hasFront = (lo < s);
        const bool   hasBack  = (hi > s);
        const double frontHi  = (s < hi) ? s : hi;      // min(S, hi)
        const double backLo   = (s > lo) ? s : lo;      // max(S, lo)

        // Inverse depths.  CoC is affine in u on each side (see class comment).
        const double uNear     = 1.0 / lo;
        const double uFrontFar = hasFront ? (1.0 / frontHi) : 0.0;
        const double uBackNear = hasBack  ? (1.0 / backLo)  : 0.0;
        const double uFar      = 1.0 / hi;

        const double cocFrontNear = hasFront
            ? clampd(aFront * (s * uNear - 1.0), 0.0, maxR) : 0.0;
        const double cocFrontFar  = hasFront
            ? clampd(aFront * (s * uFrontFar - 1.0), 0.0, maxR) : 0.0;
        const double cocBackNear  = hasBack
            ? clampd(aBack * (1.0 - s * uBackNear), 0.0, maxR) : 0.0;
        const double cocBackFar   = hasBack
            ? clampd(aBack * (1.0 - s * uFar), 0.0, maxR) : 0.0;

        const double spanFront = cocFrontNear - cocFrontFar;    // >= 0
        const double spanBack  = cocBackFar - cocBackNear;      // >= 0

        // --- split the bucket budget ----------------------------------------
        // Proportional to each side's CoC span, so the ΔCoC step comes out
        // equal on both sides (the split that minimises the worst step).  With
        // no CoC variation at all — an all-in-focus frame, size 0, or a fully
        // saturated range — fall back to splitting by inverse-depth extent,
        // and finally to an even split.
        int kFront = 0;
        int kBack  = 0;
        if (hasFront && hasBack) {
            const double total = spanFront + spanBack;
            double ratio;
            if (total > 0.0) {
                ratio = spanFront / total;
            } else {
                const double uFrontSpan = uNear - uFrontFar;
                const double uBackSpan  = uBackNear - uFar;
                const double uTotal     = uFrontSpan + uBackSpan;
                ratio = (uTotal > 0.0) ? (uFrontSpan / uTotal) : 0.5;
            }
            kFront = static_cast<int>(ratio * static_cast<double>(k) + 0.5);
            kFront = clampi(kFront, 1, k - 1);      // both sides keep >= 1
            kBack  = k - kFront;
        } else if (hasFront) {
            kFront = k;
        } else {
            kBack = k;
        }

        // --- front side: boundaries 0 .. kFront ------------------------------
        if (kFront > 0) {
            const double step  = spanFront / static_cast<double>(kFront);
            const bool   byCoc = (spanFront > 0.0) && (aFront > 0.0);
            for (int j = 0; j <= kFront; ++j) {
                double d;
                if (j == 0) {
                    d = lo;                         // pinned: clips empty range
                } else if (j == kFront) {
                    d = frontHi;                    // pinned: the focal plane
                } else if (byCoc) {
                    // Uniform step DOWN in clamped CoC, inverted through the
                    // unclamped affine law.  Inside the saturated plateau this
                    // yields depths at or before `lo`, which the clamp folds
                    // back onto boundary 0 — one bucket for the whole plateau.
                    const double c = cocFrontNear - step * static_cast<double>(j);
                    const double u = (c / aFront + 1.0) / s;
                    d = (u > 0.0) ? (1.0 / u) : frontHi;
                } else {
                    const double t = static_cast<double>(j) / static_cast<double>(kFront);
                    const double u = uNear + (uFrontFar - uNear) * t;
                    d = (u > 0.0) ? (1.0 / u) : frontHi;
                }
                _boundaries[j] = static_cast<float>(clampd(d, lo, frontHi));
            }
        }

        // --- back side: boundaries (kFront) .. k -----------------------------
        if (kBack > 0) {
            const int    base  = hasFront ? kFront : 0;
            const double step  = spanBack / static_cast<double>(kBack);
            const bool   byCoc = (spanBack > 0.0) && (aBack > 0.0);
            for (int j = 0; j <= kBack; ++j) {
                double d;
                if (j == 0) {
                    d = backLo;                     // == frontHi when both sides
                } else if (j == kBack) {
                    d = hi;
                } else if (byCoc) {
                    const double c = cocBackNear + step * static_cast<double>(j);
                    const double u = (1.0 - c / aBack) / s;
                    d = (u > 0.0) ? (1.0 / u) : hi;
                } else {
                    const double t = static_cast<double>(j) / static_cast<double>(kBack);
                    const double u = uBackNear + (uFar - uBackNear) * t;
                    d = (u > 0.0) ? (1.0 / u) : hi;
                }
                _boundaries[base + j] = static_cast<float>(clampd(d, backLo, hi));
            }
        }

        _bucketCount   = k;
        _focusBoundary = hasFront ? kFront : 0;

        // Strict monotonicity fix-up.  Rounding a double boundary to float, or
        // a saturated plateau collapsing several targets onto `lo`, can leave
        // two boundaries equal; nudging by one ulp keeps every bucket's depth
        // span non-zero so bucketOf()/locateBoundary() never divide by zero.
        for (int i = 1; i <= k; ++i) {
            if (!(_boundaries[i] > _boundaries[i - 1])) {
                _boundaries[i] = std::nextafter(_boundaries[i - 1],
                                                std::numeric_limits<float>::infinity());
            }
        }

        for (int i = 0; i < k; ++i)
            _centres[i] = 0.5f * (_boundaries[i] + _boundaries[i + 1]);
    }

    // -----------------------------------------------------------------------
    // bucketOf — fractional two-bucket assignment, a partition of unity
    //
    // A fragment at `depth` lands between the centres of two adjacent buckets
    // and is deposited into both, weighted by where it sits between them.
    // Assigning a POINT fragment wholly to its containing bucket instead is
    // what produces visible layer-transition banding on a surface receding
    // through the focal plane (validation scene (g)), so this split is
    // mandatory for point fragments, not an optimisation.  (How much it
    // actually smooths depends on alpha — see partitionAlpha()'s "known cost"
    // note; it fades smoothly for translucent fragments and degenerates to a
    // hard centre quantisation as alpha -> 1.)
    //
    // ONLY FOR SAMPLES THAT WERE NOT SPAN-SPLIT.  A piece produced by
    // splitSpanAtBoundaries() must go through bucketOfContaining() instead —
    // re-splitting an already-split piece double-counts it.  See that
    // function's contract note.
    //
    // Returns index in [0, K-1] and frac in [0, 1]; the weights are
    // (1 - frac) for `index` and frac for `index + 1`, summing to exactly 1.
    // Outside the outermost centres the assignment saturates onto the first or
    // last bucket with frac 0 (no bucket -1 / K to spill into); a NaN depth
    // takes the same first-bucket path rather than propagating.
    //
    // O(log K) — the ΔCoC spacing is non-uniform, so there is no closed-form
    // index.  The search mirrors HoldoutVisibility::interp()'s.
    // -----------------------------------------------------------------------
    DEEPC_HD inline BucketWeight bucketOf(float depth) const
    {
        BucketWeight w;
        if (_bucketCount <= 1)
            return w;                       // {0, 0}: inert / single bucket

        const int last = _bucketCount - 1;
        if (!(depth > _centres[0]))         // also catches NaN
            return w;
        if (depth >= _centres[last]) {
            w.index = last;
            return w;
        }

        int lo = 0;
        int hi = last;
        while (hi - lo > 1) {
            const int mid = lo + (hi - lo) / 2;
            if (_centres[mid] <= depth)
                lo = mid;
            else
                hi = mid;
        }

        const float span = _centres[lo + 1] - _centres[lo];
        w.index = lo;
        w.frac  = (span > 0.0f) ? clampf((depth - _centres[lo]) / span, 0.0f, 1.0f) : 0.0f;
        return w;
    }

    // -----------------------------------------------------------------------
    // locateBoundary — position between the two BUCKET BOUNDARIES bracketing a
    // depth
    //
    // Deliberately separate from bucketOf(): that one measures between bucket
    // *centres* for the scatter's partition of unity, this one between bucket
    // *boundaries*, and the two fractions are different numbers for the same
    // depth.  bucketOfContaining(), splitSpanAtBoundaries() and the flatten's
    // pre-merge grouping key are its callers.
    //
    // IT NO LONGER FEEDS THE HOLDOUT LUT (M1.P3.T10).  That is
    // HoldoutBoundaries::locate(), over a decoupled uniform-in-z boundary set;
    // sampling the LUT at the ΔCoC boundaries is the defect that task fixed.
    // Passing this pair to HoldoutVisibility::interpAtBucket() would index a
    // different array than the one the LUT was built at.
    //
    // Depths outside the range clamp onto the first/last boundary, matching
    // interp()'s own out-of-range behaviour.
    // -----------------------------------------------------------------------
    DEEPC_HD inline BoundarySpan locateBoundary(float depth) const
    {
        BoundarySpan s;
        if (_bucketCount <= 0)
            return s;                       // {0, 0}

        const int lastB = _bucketCount;     // index of the last boundary
        if (!(depth > _boundaries[0]))      // also catches NaN
            return s;
        if (depth >= _boundaries[lastB]) {
            s.index = lastB - 1;
            s.frac  = 1.0f;
            return s;
        }

        int lo = 0;
        int hi = lastB;
        while (hi - lo > 1) {
            const int mid = lo + (hi - lo) / 2;
            if (_boundaries[mid] <= depth)
                lo = mid;
            else
                hi = mid;
        }

        const float span = _boundaries[lo + 1] - _boundaries[lo];
        s.index = lo;
        s.frac  = (span > 0.0f) ? clampf((depth - _boundaries[lo]) / span, 0.0f, 1.0f) : 0.0f;
        return s;
    }

    // -----------------------------------------------------------------------
    // bucketOfContaining — whole-weight assignment to the CONTAINING bucket
    //
    // Returns {index, 0}, i.e. all the weight in the one bucket whose
    // [boundary(i), boundary(i+1)] interval contains `depth`, with no
    // fractional spill into a neighbour.  It is the assignment a piece coming
    // out of splitSpanAtBoundaries() must use, and the ONLY one that keeps
    // the split composable — see that function's "composition contract".
    //
    // This is not a downgrade of bucketOf(): a span-split piece is already
    // graded across buckets by its own thickness fraction `t`, which varies
    // continuously as the parent span slides over a boundary (a piece just
    // past a boundary has t -> 0, hence alpha -> 0 and colourScale -> 0), so
    // the pieces need no second, redundant smoothing — and applying one is
    // what double-counts them.  Point samples, which are never span-split and
    // have no such grading of their own, are exactly the case bucketOf()
    // exists for.
    // -----------------------------------------------------------------------
    DEEPC_HD inline BucketWeight bucketOfContaining(float depth) const
    {
        BucketWeight w;
        if (_bucketCount <= 0)
            return w;                       // {0, 0}: inert
        const BoundarySpan s = locateBoundary(depth);
        w.index = clampi(s.index, 0, _bucketCount - 1);
        w.frac  = 0.0f;
        return w;
    }

    // -----------------------------------------------------------------------
    // firstBoundaryAbove — smallest index with boundary(index) > z
    //
    // boundaryCount() when there is none.  O(log K); the volumetric splitter
    // uses it to jump straight to the boundaries a span actually crosses,
    // which is what keeps the common "sample inside one bucket" case at a
    // binary search plus one compare rather than a walk over all K+1.
    // -----------------------------------------------------------------------
    DEEPC_HD inline int firstBoundaryAbove(float z) const
    {
        const int n = boundaryCount();
        int lo = 0;
        int hi = n;                         // answer is in [lo, hi]
        while (lo < hi) {
            const int mid = lo + (hi - lo) / 2;
            if (_boundaries[mid] > z)
                hi = mid;
            else
                lo = mid + 1;
        }
        return lo;
    }
};

// ---------------------------------------------------------------------------
// makeBoundedDeltaCocBuckets — build-and-return convenience
//
// Mirrors makeCocParams(): the members are public so the object can be built
// piecewise, but the normal call site is this one-liner.
// ---------------------------------------------------------------------------
inline DepthBuckets makeBoundedDeltaCocBuckets(const CocParams& p,
                                               float depthMin,
                                               float depthMax,
                                               int requestedBuckets)
{
    DepthBuckets b;
    b.buildBoundedDeltaCoc(p, depthMin, depthMax, requestedBuckets);
    return b;
}

// ---------------------------------------------------------------------------
// HoldoutBoundaries — THE HOLDOUT LUT'S OWN BOUNDARY SET (M1.P3.T10)
//
// The holdout transmittance LUT is sampled at THESE depths, not at
// DepthBuckets' ones.  The two sets share a COUNT (K+1, so per-band LUT memory
// stays at the documented (K+1)*W*B*4 bytes) and nothing else.
//
// WHY THEY ARE DECOUPLED.  The design reference originally specified the LUT
// "at the K+1 bucket boundaries".  That is wrong, and it inverted the node's
// differentiator.  DepthBuckets' spacing is bounded-ΔCoC: it holds the CoC
// step constant, which is the criterion that governs BANDING, and it therefore
// spends its budget wherever the CoC changes fastest — on the node's own
// defaults (K=16, focus 10, measured range [1,100]) that is 15 buckets inside
// [1,10] and ONE covering [10,100].  Depth OCCLUSION has no such bias: an
// opaque point-sample holdout (a solid card — the commonest holdout shape
// there is) at z=50 landed in that single [10,100] bracket, and because
// interpAtBucket() chords a step onto the bracket's NEAR boundary the card
// started occluding at z=10.9.  A fragment at z=15, thirty-five units IN FRONT
// of the card, came out 98% erased; fragments at z=30/40/49 vanished outright.
// Mean |vis error| 0.391, max 1.000.
//
// Neither K nor a better interpolant fixes THAT (the bite only moves to
// 25.8/40.6/40.2 at K=32/64/128; linear-in-T moves the mean 0.476 -> 0.450),
// because the dominant term on a 90-unit-wide bracket is boundary PLACEMENT.
// With two boundary values a monotone T can be anywhere between them, so no
// interpolant beats a worst case of (T0-T1)/2 — the goal here is correct
// placement, not exactness.
//
// PLACEMENT IS NOT THE WHOLE RESIDUAL, THOUGH — see the "what is left" note on
// HoldoutLut.  Once the brackets are the right width, the log chord's own
// collapse on an OPAQUE step (it floors log T at kMinTransmittance, so vis
// hits ~0 across the whole bracket rather than the bound's half) is the
// remaining reducible term, and it is one-sided toward camera.  M1.P3.T10's
// review measured 5.07 of the 6.19-unit bracket fully erased at K=16.  Do not
// read "(T0-T1)/2 is irreducible" as "what ships is irreducible".
//
// WHY UNIFORM-IN-Z (measured at M1.P3.T10, same 17 entries/pixel, mean
// |vis error| / bite depth against a true 50; full numbers in the milestone
// Decisions):
//
//   set                         headline card   opaque points   opaque spans
//   dCoC buckets (was)          0.391 @ 10.9    0.115           0.110 / 0.113
//   uniform-in-1/z              0.352 @ 14.8    0.116           0.111 / 0.114
//   equal-occlusion-mass hist.  0.000 @ 50.0    0.004           0.270 / 0.318
//   UNIFORM-IN-Z (this)         0.057 @ 44.4    0.014           0.014 / 0.013
//
// * uniform-in-1/z is NOT the answer — it reproduces almost exactly the
//   front-loaded bias that broke the ΔCoC set.
// * a holdout-depth-HISTOGRAM-derived set (boundaries at equal-occlusion-mass
//   quantiles, i.e. equal optical depth) is exact for isolated opaque cards,
//   but it is 20x WORSE than uniform-in-z on VOLUMETRIC holdouts — an opaque
//   fog slab carries essentially all the frame's mass, so every quantile
//   crossing lands inside the slab's front edge and the rest of the range
//   collapses into one bracket: the ΔCoC failure reproduced through a
//   different door, and precisely validation scene (f).  It also needs an
//   eager full-frame holdout pass to build the histogram, and a non-uniform
//   set has no closed-form index.  Rejected on the measurement.
// * sub-refining the ΔCoC set to the same accuracy needs S=16, i.e. 257
//   entries/pixel — 16x the memory and build time.  Rejected.
//
// THE INDEX IS CLOSED FORM, so the per-fragment cost is O(1) with no search:
// the plan's budget of "two binary searches per fragment (assignment + holdout
// vis)" is now ONE (bucketOf's O(log K)) plus this O(1) locate.
//
// STORAGE mirrors DepthBuckets': a fixed array sized by the K knob's maximum,
// so the object allocates nothing, is trivially copyable and can be handed to
// a device kernel by value.  It is built once per frame, never per band.
//
// A default-constructed instance is inert: count() == 0, enabled() == false,
// locate() answers {0, 0}.
// ---------------------------------------------------------------------------
struct HoldoutBoundaries {

    static constexpr int kMaxBoundaries = DepthBuckets::kMaxBoundaries;

    float _z[kMaxBoundaries] = {};
    float _z0      = 0.0f;       // == _z[0]
    float _invStep = 0.0f;       // (count-1) / (_z[count-1] - _z[0])
    int   _count   = 0;

    DEEPC_HD inline int          count() const      { return _count; }
    DEEPC_HD inline bool         enabled() const    { return _count > 1; }
    DEEPC_HD inline const float* boundaries() const { return _z; }
    DEEPC_HD inline float        boundary(int i) const { return _z[i]; }
    DEEPC_HD inline float        depthMin() const   { return _z[0]; }
    DEEPC_HD inline float        depthMax() const   { return _z[(_count > 0) ? _count - 1 : 0]; }

    // -----------------------------------------------------------------------
    // buildUniformZ — equal steps in Z across the frame's measured depth range
    //
    //   depthMin/depthMax : the frame's measured range (the SAME alpha-weighted
    //                       depth pass DepthBuckets is built from — so this
    //                       costs no extra pass, and the two sets clip the same
    //                       empty ranges).  Order is irrelevant; both clamped.
    //   count             : DepthBuckets::boundaryCount() == K+1.
    //
    // A holdout wholly outside the range degrades correctly rather than
    // arbitrarily: one entirely BEHIND depthMax leaves every boundary
    // transmittance at 1 (it occludes nothing, which is right), and one
    // entirely IN FRONT of depthMin drives every boundary to its own
    // (1-alpha) (it occludes everything, which is also right).
    //
    // Host-side builder, matching this header's convention for whole-list
    // builders; uses double for the step exactly as DepthBuckets does.
    //
    // Post-conditions (asserted NOW, in tests/test_defocus_math.cpp's
    // HoldoutBoundaries cases -- T10 shipped this struct before M1.P3.T4
    // exists, so they are pinned there rather than promised; T4 may move them
    // into the scatter-core suite but must not drop them):
    //   * count() == clamp(count, 2, kMaxBoundaries)
    //   * boundary(0) == sanitised depthMin, boundary(count-1) == sanitised
    //     depthMax
    //   * boundaries strictly increasing (same one-ulp fix-up DepthBuckets uses)
    //   * locate(boundary(i)) == {i, 0} for every interior i
    // -----------------------------------------------------------------------
    void buildUniformZ(float depthMin, float depthMax, int count)
    {
        const int n = clampi(count, 2, kMaxBoundaries);

        float lo = DepthBuckets::sanitizeDepth(depthMin);
        float hi = DepthBuckets::sanitizeDepth(depthMax);
        if (hi < lo) {
            const float t = lo;
            lo = hi;
            hi = t;
        }
        if (!(hi > lo)) {
            // Degenerate/single-depth measurement: open the range by a hair so
            // every bracket keeps a non-zero extent (DepthBuckets does the
            // same, for the same reason).
            float pad = lo * 1e-4f;
            if (!(pad > 0.0f))
                pad = DepthBuckets::kMinDepth;
            hi = lo + pad;
            if (!(hi < DepthBuckets::kMaxDepth)) {
                hi = DepthBuckets::kMaxDepth;
                lo = hi - pad;
            }
            if (!(hi > lo)) {                   // unreachable in practice
                lo = DepthBuckets::kMinDepth;
                hi = DepthBuckets::kMinDepth * 2.0f;
            }
        }

        const double dlo  = static_cast<double>(lo);
        const double dhi  = static_cast<double>(hi);
        const double step = (dhi - dlo) / static_cast<double>(n - 1);

        for (int i = 0; i < n; ++i)
            _z[i] = static_cast<float>(dlo + step * static_cast<double>(i));
        _z[0]     = lo;
        _z[n - 1] = hi;

        // Same strict-monotonicity fix-up as DepthBuckets: rounding a double
        // boundary to float can collapse a pair on a very wide range, and
        // locate() must never divide by a zero span.
        for (int i = 1; i < n; ++i) {
            if (!(_z[i] > _z[i - 1])) {
                _z[i] = std::nextafter(_z[i - 1],
                                       std::numeric_limits<float>::infinity());
            }
        }

        _count   = n;
        _z0      = _z[0];
        const float total = _z[n - 1] - _z[0];
        _invStep = (total > 0.0f)
                 ? (static_cast<float>(n - 1) / total)
                 : 0.0f;
    }

    // -----------------------------------------------------------------------
    // locate — the bracket containing `z`, and the position inside it.  O(1).
    //
    // The pair HoldoutVisibility::interpAtBucket() consumes.  It replaces
    // DepthBuckets::locateBoundary() on the holdout path ONLY: locateBoundary()
    // still answers for the ΔCoC boundaries (pre-merge grouping, span splits,
    // bucketOfContaining) and its fraction is a different number.
    //
    // The index comes from the uniform step in closed form; the two guarded
    // correction steps that follow reconcile it with the STORED array, whose
    // float rounding can differ from the multiply by at most one bracket
    // (the worst relative error over 128 steps is ~1.3e-05 of a bracket).
    // Deriving `frac` from the stored boundaries rather than from the
    // multiplication is what makes locate(boundary(i)) return frac exactly 0,
    // which is what keeps "LUT == exact AT the boundaries" a hard identity
    // rather than a tolerance.
    //
    // Depths outside the range clamp onto the first/last boundary, matching
    // DepthBuckets::locateBoundary() and HoldoutVisibility::interp(); NaN takes
    // the first-boundary path rather than propagating.
    // -----------------------------------------------------------------------
    DEEPC_HD inline BoundarySpan locate(float z) const
    {
        BoundarySpan s;
        if (_count <= 1)
            return s;                       // {0, 0}: inert

        const int last = _count - 1;        // index of the last boundary
        if (!(z > _z[0]))                   // also catches NaN
            return s;
        if (z >= _z[last]) {
            s.index = last - 1;
            s.frac  = 1.0f;
            return s;
        }

        int i = static_cast<int>((z - _z0) * _invStep);
        i = clampi(i, 0, last - 1);

        // Bounded (never a loop over the array): the closed form is off by at
        // most one, and two steps each way is slack, not necessity.
        for (int g = 0; g < 2 && i > 0 && z < _z[i]; ++g)
            --i;
        for (int g = 0; g < 2 && i < last - 1 && z >= _z[i + 1]; ++g)
            ++i;

        const float span = _z[i + 1] - _z[i];
        s.index = i;
        s.frac  = (span > 0.0f) ? clampf((z - _z[i]) / span, 0.0f, 1.0f) : 0.0f;
        return s;
    }
};

// ---------------------------------------------------------------------------
// makeUniformHoldoutBoundaries — build-and-return convenience
//
// THE CANONICAL CALL SITE.  The holdout LUT's boundary count is the bucket
// boundary count (K+1) and its range is the frame's measured depth range, both
// of which DepthBuckets already carries — so the whole decoupling costs one
// line at the point where the buckets are built, and no extra frame pass.
// ---------------------------------------------------------------------------
inline HoldoutBoundaries makeUniformHoldoutBoundaries(const DepthBuckets& buckets)
{
    HoldoutBoundaries h;
    h.buildUniformZ(buckets.depthMin(), buckets.depthMax(), buckets.boundaryCount());
    return h;
}

// ---------------------------------------------------------------------------
// fragmentDeposit — turn a bucket assignment + alpha into the two deposits
//
// The alphas are transmittance-split (partitionAlpha), not linearly scaled,
// so the front-to-back composite of the two buckets reproduces the fragment
// exactly; see partitionAlpha() for the derivation and for why the linear
// reading breaks the flat-opaque-field identity.
//
// When frac == 0 the second deposit is (index0, 0, 0) — same index, zero
// alpha, zero colour scale — so an unconditional two-deposit scatter loop
// stays in bounds and adds nothing.  That is also exactly what a
// bucketOfContaining() assignment produces, so the same call site serves both
// the point-sample path (bucketOf) and the span-split path
// (bucketOfContaining) with no branch; see splitSpanAtBoundaries()'s
// composition contract for which to pass.
// ---------------------------------------------------------------------------
DEEPC_HD inline BucketDeposit fragmentDeposit(const BucketWeight& w, float alpha)
{
    const float w0 = w.weightLow();
    const float w1 = w.weightHigh();

    BucketDeposit d;
    d.index0      = w.index;
    d.index1      = w.indexHigh();
    d.alpha0      = partitionAlpha(alpha, w0);
    d.alpha1      = partitionAlpha(alpha, w1);
    d.colorScale0 = partitionColorScale(alpha, w0);
    d.colorScale1 = partitionColorScale(alpha, w1);
    return d;
}

// ---------------------------------------------------------------------------
// SpanSplitPart — one piece of a volumetric sample cut at a bucket boundary
//
//   t          : this piece's share of the parent span's thickness; the parts
//                of one split sum to 1
//   alpha      : 1 - (1 - parentAlpha)^t, so the parts' transmittances
//                multiply back to (1 - parentAlpha)
//   colorScale : factor for the parent's PREMULTIPLIED colour (alpha/parent
//                alpha, tending to t as the parent alpha tends to 0)
// ---------------------------------------------------------------------------
struct SpanSplitPart {
    float zFront     = 0.0f;
    float zBack      = 0.0f;
    float t          = 1.0f;
    float alpha      = 0.0f;
    float colorScale = 1.0f;
};

// ---------------------------------------------------------------------------
// splitPartCount — upper bound on splitSpanAtBoundaries()'s part count
//
// Counts the boundaries strictly inside the span, plus one.  It is an upper
// bound rather than the exact count because the splitter additionally drops
// boundaries that round onto the previous cut, so the actual result can be
// smaller; it is never larger, which is what makes it safe for sizing.
//
// Lets a caller size its output buffer without a trial split.  Never exceeds
// boundaryCount() + 1 = K + 2, which is therefore a safe fixed bound for a
// stack array in the scatter core (K <= 128, so 130 SpanSplitPart is ~2.6KB —
// still no heap, per the no-per-fragment-allocation rule).
// ---------------------------------------------------------------------------
DEEPC_HD inline int splitPartCount(const DepthBuckets& buckets, float zFront, float zBack)
{
    if (!(zBack > zFront) || !std::isfinite(zFront) || !std::isfinite(zBack))
        return 1;

    const int n = buckets.boundaryCount();
    int parts = 1;
    for (int i = buckets.firstBoundaryAbove(zFront); i < n; ++i) {
        if (!(buckets.boundary(i) < zBack))
            break;
        ++parts;
    }
    return parts;
}

// ---------------------------------------------------------------------------
// splitSpanAtBoundaries — the volumetric bucket-boundary transmittance split
//
// A deep sample spanning several buckets is cut at every boundary strictly
// inside it, so a fog slab grades across the depth layers instead of
// collapsing into one hard layer at its midpoint.  Opacity is assumed uniform
// along the span (the same assumption HoldoutVisibility::inSpan() makes),
// which makes transmittance exponential in depth and the split analytic:
// a piece covering fraction t of the span carries alpha 1 - (1-alpha)^t, and
// because the pieces' t sum to 1 their transmittances multiply back to
// exactly (1 - alpha).  Premultiplied colour scales by alpha_piece/alpha, so
// the front-to-back over of the pieces reproduces the original sample.
//
//   out       : caller-owned, at least maxParts entries (no allocation here)
//   maxParts  : capacity; if the span crosses more boundaries than fit, the
//               remainder is merged into one final part rather than
//               overflowing, so the transmittance identity still holds and
//               only depth resolution is lost.  Size it with splitPartCount()
//               (or the fixed K+2 bound) to avoid that entirely.
//
// Returns the number of parts written (>= 1, always <= maxParts).  A sample
// that crosses no boundary — the common case — returns 1 part identical to
// its input, after one binary search and one compare.  Zero-length spans
// (zBack <= zFront, i.e. point samples) and non-finite endpoints likewise
// pass through untouched as a single part.
//
// COMPOSITION CONTRACT — the scatter core MUST deposit a part produced here
// via DepthBuckets::bucketOfContaining(), never via bucketOf().
//
// The identities above hold because the parts land in DISTINCT buckets (they
// are cut at the boundaries precisely so that they do) and are therefore
// combined by the front-to-back `over` that partitionAlpha() is built for.
// bucketOf() splits by bucket CENTRES, not boundaries, so two consecutive
// parts can be pushed into the same pair of planes — where the scatter's
// within-bucket accumulation is ADDITIVE, not `over`, and the transmittance
// split's alphas (which are deliberately super-linear: alpha_a + alpha_b >
// alpha whenever the parts are meant to be composited rather than added)
// over-count.  Measured on a 16-bucket frame with a span straddling one
// boundary: alpha and premultiplied colour come out up to +8.2% high at
// parent alpha 0.9 (+5.6% at 0.5, +1.0% at 0.1), i.e. fog slabs render
// denser and brighter than the sample they came from.  With
// bucketOfContaining() the same case reproduces the parent to float
// precision.
//
// The two splits are therefore alternatives, not a pipeline: a volumetric
// sample is graded across depth by THIS function, a point sample by
// bucketOf().  Neither needs the other, and applying both is the bug above.
// ---------------------------------------------------------------------------
DEEPC_HD inline int splitSpanAtBoundaries(const DepthBuckets& buckets,
                                          float zFront,
                                          float zBack,
                                          float alpha,
                                          SpanSplitPart* __restrict__ out,
                                          int maxParts)
{
    if (out == nullptr || maxParts <= 0)
        return 0;

    const float a = clampf(alpha, 0.0f, 1.0f);

    if (!(zBack > zFront) || !std::isfinite(zFront) || !std::isfinite(zBack)) {
        out[0].zFront     = zFront;
        out[0].zBack      = zBack;
        out[0].t          = 1.0f;
        out[0].alpha      = a;
        out[0].colorScale = 1.0f;
        return 1;
    }

    const float invThickness = 1.0f / (zBack - zFront);
    const int   n            = buckets.boundaryCount();

    int   count     = 0;
    float partFront = zFront;
    float uPrev     = 0.0f;     // normalised position of partFront in the span

    // The last slot is reserved for the tail part, so `count` can never reach
    // maxParts inside the loop and the write after it is always in bounds.
    for (int i = buckets.firstBoundaryAbove(zFront); i < n && count < maxParts - 1; ++i) {
        const float b = buckets.boundary(i);
        if (!(b < zBack))
            break;

        // Positions are accumulated as differences of normalised offsets from
        // the span front (rather than per-part thicknesses divided by the
        // total) so the parts' t values telescope and sum to 1 to within a
        // rounding of the final subtraction.
        const float u = clampf((b - zFront) * invThickness, 0.0f, 1.0f);
        if (!(u > uPrev))       // boundary coincides with the previous cut
            continue;

        const float t = u - uPrev;
        out[count].zFront     = partFront;
        out[count].zBack      = b;
        out[count].t          = t;
        out[count].alpha      = partitionAlpha(a, t);
        out[count].colorScale = partitionColorScale(a, t);
        ++count;

        partFront = b;
        uPrev     = u;
    }

    const float tailT = 1.0f - uPrev;
    out[count].zFront     = partFront;
    out[count].zBack      = zBack;
    out[count].t          = tailT;
    out[count].alpha      = partitionAlpha(a, tailT);
    out[count].colorScale = partitionColorScale(a, tailT);
    return count + 1;
}

// ---------------------------------------------------------------------------
// Bucket plane layout
//
// The band's bucket planes are plain SoA float buffers, laid out so that both
// the saturation pass and the composite walk them contiguously:
//
//   colour: color[(k * channelCount + c) * pixelCount + i]
//   alpha : alpha[k * pixelCount + i]
//
// with k the bucket (front to back, 0 = nearest), c the channel and i the
// destination pixel within the band (pixelCount == bandWidth * bandHeight).
// This is the colour+alpha half of the (C+3)-plane-per-bucket layout the
// design reference's memory formula assumes (K*W*B*(C+3)*4 since M1.P3.T9);
// the two `sum of w*vis` AREA planes the scatter also keeps — new area and
// co-located area — live alongside and are not touched by anything here.
//
// The per-pixel entry points below take pointers ALREADY OFFSET to their
// pixel (`plane + i`) and derive everything else from `pixelCount`, which
// keeps their signatures short enough to stay readable and makes them
// directly usable as the body of a one-thread-per-pixel CUDA kernel in M3.
// Pass pixelCount = 1 (and pointers to a single pixel's K*C values) to use
// them standalone, e.g. from a unit test.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// saturateBucketPixel — the alpha-saturation renormalize, one bucket, one px
//
// Additive premultiplied accumulation is energy-conserving in flat regions,
// but where two surfaces overlap in screen space *within the same bucket*
// their alphas add and can exceed 1.  Rescaling colour AND alpha by 1/alpha
// pulls that back to alpha == 1 with the colour:alpha ratio — i.e. the
// unpremultiplied colour — preserved exactly.
//
// SATURATE DOWN ONLY, NEVER SCALE UP.  alpha < 1 is left strictly alone: a
// defocused foreground scattering outward with nothing behind it produces an
// honest alpha dip inside the silhouette, and that coverage deficit is
// specified behaviour for this node (it is what makes the comp-over-plate
// workflow correct).  Scaling up would fabricate coverage and hide it.
//
// alpha is assigned exactly 1.0f rather than multiplied by 1/alpha: the two
// are the same analytically, but the multiply can land a half-ulp above 1 and
// leave a plane the composite would then have to clamp.  The colour multiply
// uses that same reciprocal, so the ratio is preserved to float precision.
// NaN alpha fails the `> 1` test and is left untouched.
// ---------------------------------------------------------------------------
DEEPC_HD inline void saturateBucketPixel(float* __restrict__ color,
                                         float* __restrict__ alpha,
                                         int channelCount,
                                         std::ptrdiff_t channelStride)
{
    const float a = *alpha;
    if (!(a > 1.0f))                    // <= 1, NaN: never scale up
        return;

    const float s = 1.0f / a;
    for (int c = 0; c < channelCount; ++c)
        color[static_cast<std::ptrdiff_t>(c) * channelStride] *= s;

    *alpha = 1.0f;
}

// ---------------------------------------------------------------------------
// saturateBucketPlanes — saturation pass over a whole band's bucket planes
//
// Host-side driver over the layout documented above; runs after scatter and
// before compositeBucketsFrontToBack().
// ---------------------------------------------------------------------------
inline void saturateBucketPlanes(float* __restrict__ color,
                                 float* __restrict__ alpha,
                                 int bucketCount,
                                 int channelCount,
                                 std::ptrdiff_t pixelCount)
{
    for (int k = 0; k < bucketCount; ++k) {
        float* bucketColor = color
            + static_cast<std::ptrdiff_t>(k) * channelCount * pixelCount;
        float* bucketAlpha = alpha + static_cast<std::ptrdiff_t>(k) * pixelCount;

        for (std::ptrdiff_t i = 0; i < pixelCount; ++i)
            saturateBucketPixel(bucketColor + i, bucketAlpha + i, channelCount, pixelCount);
    }
}

// ---------------------------------------------------------------------------
// compositePixelFrontToBack — over-composite the K bucket planes at one pixel
//
// The planes hold PREMULTIPLIED colour, bucket 0 nearest the camera, so this
// is the standard front-to-back over:
//
//   out += T * color[k];   outAlpha += T * alpha[k];   T *= (1 - alpha[k])
//
// which yields outAlpha = 1 - prod(1 - alpha[k]) — exactly 1 as soon as any
// bucket is opaque, hence the flat-opaque-field identity (validation scene
// (c)) given the transmittance-preserving fragment split.
//
// Alpha is clamped into [0,1] before use: after saturateBucketPlanes() that
// is a no-op, but it guarantees the transmittance stays in [0,1] even if a
// caller composites unsaturated planes, rather than letting a negative
// (1 - alpha) flip the sign of everything behind it.
//
// outColor/outAlpha are OVERWRITTEN, not accumulated.
// ---------------------------------------------------------------------------
DEEPC_HD inline void compositePixelFrontToBack(const float* __restrict__ bucketColor,
                                               const float* __restrict__ bucketAlpha,
                                               int bucketCount,
                                               int channelCount,
                                               std::ptrdiff_t pixelCount,
                                               float* __restrict__ outColor,
                                               float* __restrict__ outAlpha)
{
    for (int c = 0; c < channelCount; ++c)
        outColor[static_cast<std::ptrdiff_t>(c) * pixelCount] = 0.0f;

    float transmittance = 1.0f;
    float accAlpha      = 0.0f;

    for (int k = 0; k < bucketCount; ++k) {
        const float a = clampf(bucketAlpha[static_cast<std::ptrdiff_t>(k) * pixelCount],
                               0.0f, 1.0f);
        const float* __restrict__ src = bucketColor
            + static_cast<std::ptrdiff_t>(k) * channelCount * pixelCount;

        for (int c = 0; c < channelCount; ++c) {
            const std::ptrdiff_t o = static_cast<std::ptrdiff_t>(c) * pixelCount;
            outColor[o] += transmittance * src[o];
        }

        accAlpha      += transmittance * a;
        transmittance *= (1.0f - a);

        if (!(transmittance > 0.0f))    // fully occluded: nothing behind shows
            break;
    }

    *outAlpha = clampf(accAlpha, 0.0f, 1.0f);
}

// ---------------------------------------------------------------------------
// compositeBucketsFrontToBack — composite a whole band's bucket planes
//
// Host-side driver; the final step of a band, after scatter and saturation.
//
// Pixel-outer / bucket-inner deliberately: the alternative (bucket-outer, so
// the inner loop is a contiguous vectorizable multiply-add) needs a per-pixel
// transmittance plane to carry state across bucket iterations, i.e. a caller
// scratch buffer, and buys little — this pass touches K*(C+1)*pixelCount
// floats exactly once either way, against the scatter's O(sum of pi*r^2), and
// the K*C planes a single pixel steps through stay resident in L1 for the
// whole row (32KB at the K=128 / C=4 worst case).  If profiling ever
// contradicts that, the bucket-outer form plus a transmittance plane is a
// drop-in replacement for this function alone.
//
// outColor is channelCount planes of pixelCount floats, outAlpha one; both
// are overwritten.
// ---------------------------------------------------------------------------
inline void compositeBucketsFrontToBack(const float* __restrict__ bucketColor,
                                        const float* __restrict__ bucketAlpha,
                                        int bucketCount,
                                        int channelCount,
                                        std::ptrdiff_t pixelCount,
                                        float* __restrict__ outColor,
                                        float* __restrict__ outAlpha)
{
    for (std::ptrdiff_t i = 0; i < pixelCount; ++i) {
        compositePixelFrontToBack(bucketColor + i,
                                  bucketAlpha + i,
                                  bucketCount,
                                  channelCount,
                                  pixelCount,
                                  outColor + i,
                                  outAlpha + i);
    }
}

} // namespace deepc

#endif // DEEPC_DEEPCDEFOCUS_MATH_H
