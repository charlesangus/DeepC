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
// evaluates transmittance once per destination pixel at the K+1 bucket
// boundaries (build()) and interpolates between two boundaries per fragment
// (interp/interpAtBucket).  Because transmittance is exponential in depth,
// interpolating in LOG space is exact for the model, not an approximation:
// log T is linear in z across any interval that no holdout sample starts or
// ends inside.  Where a sample edge does fall strictly between two boundaries,
// log T is piecewise linear there and the interpolation is the (still
// monotone) chord — the one bucket-wide approximation the design accepts in
// exchange for O(1) per fragment.
//
// Buffers are plain contiguous float arrays owned by the caller: one LUT is
// built per destination pixel per band, so a per-pixel std::vector would be a
// performance defect.
// ---------------------------------------------------------------------------
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
    //   boundaries         : the K+1 bucket boundary depths, ascending
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
    // The production path: the scatter core already knows a fragment's bucket
    // index and its fraction between boundary[index] and boundary[index+1]
    // (from DepthBuckets::bucketOf), so no search is needed.
    //
    // Interpolating log T is exact for the exponential in-span model.  A zero
    // boundary transmittance would give log(0) = -inf, so values are floored
    // at kMinTransmittance and a result at/below that floor returns exactly 0.
    // -----------------------------------------------------------------------
    static DEEPC_HD inline float interpAtBucket(const float* boundaryT,
                                                int boundaryCount,
                                                int index,
                                                float frac)
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

        const float l0 = std::log(clampf(t0, kMinTransmittance, 1.0f));
        const float l1 = std::log(clampf(t1, kMinTransmittance, 1.0f));
        const float v  = std::exp(l0 + (l1 - l0) * t);

        return (v <= kMinTransmittance) ? 0.0f : clampf(v, 0.0f, 1.0f);
    }

    // -----------------------------------------------------------------------
    // interp — same interpolation, locating the bracketing boundaries by depth
    //
    // O(log boundaryCount) because the ΔCoC boundary spacing is non-uniform.
    // Convenience/verification entry point; the scatter core should use
    // interpAtBucket() with the index it already has.
    //
    // Depths outside the boundary range clamp to the nearest boundary value
    // (the LUT is built to span the frame's depth range, so this is a
    // degenerate path, and clamping keeps visibility monotone in depth).
    // -----------------------------------------------------------------------
    static DEEPC_HD inline float interp(const float* boundaries,
                                        const float* boundaryT,
                                        int boundaryCount,
                                        float z)
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
        return interpAtBucket(boundaryT, boundaryCount, lo, frac);
    }
};

} // namespace deepc

#endif // DEEPC_DEEPCDEFOCUS_MATH_H
