// SPDX-License-Identifier: MIT
//
// ============================================================================
//
//  test_defocus_math — unit tests for DeepCDefocusMath.h / DeepCDefocusKernel.h
//
//  Pure-math test suite. No NDK/DDImage dependency anywhere in this file or
//  the headers it tests, so it builds and runs with a plain
//  `g++ -std=c++17 test_defocus_math.cpp -o test && ./test` as well as through
//  the DEEPC_BUILD_TESTS CMake option.
//
//  Deterministic: no randomness anywhere, so every run is bit-reproducible.
//  Every floating-point comparison carries an epsilon with a comment saying
//  where the number came from -- either a hand derivation shown in-line, or a
//  measured worst case.
//
// ============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "../src/DeepCDefocusMath.h"
#include "../src/DeepCDefocusKernel.h"

#include <cmath>
#include <limits>
#include <vector>

using namespace deepc;

namespace {

// ---------------------------------------------------------------------------
// Shared test fixtures
// ---------------------------------------------------------------------------

// Front-to-back `over` of a pixel's K bucket alphas, i.e. 1 - prod(1 - a_k).
//
// This is a TEST-LOCAL ORACLE, not a production path.  The identity it is
// used for below belongs to the FRAGMENT SPLIT, not to the bucket composite:
// partitionAlpha()'s whole contract is that its two deposits reconstruct the
// parent under `over` (`1 - (1-a0)(1-a1) == alpha`).
inline float overCompositeAlpha(const float* bucketAlpha, int bucketCount)
{
    float transmittance = 1.0f;
    for (int k = 0; k < bucketCount; ++k)
        transmittance *= (1.0f - clampf(bucketAlpha[k], 0.0f, 1.0f));
    return clampf(1.0f - transmittance, 0.0f, 1.0f);
}

// A "no-saturation" physical rig: 50mm f/2.8 lens, 36mm filmback, 1920px
// format, focused at 10m, over a depth range of [1m, 100m]. Chosen so the
// resulting CoC never reaches max_radius=100px anywhere in that range (see
// the ΔCoC spacing test below for the derivation), which keeps the bucket
// step-size identity clean of the deliberate saturated-plateau special case
// documented in DepthBuckets' class comment.
CocParams makeNoSaturationPhysicalParams()
{
    return makeCocParams(CocMode::Physical,
                          /*focalLengthMm*/   50.0f,
                          /*fStop*/           2.8f,
                          /*filmbackWidthMm*/ 36.0f,
                          /*focusDistance*/   10.0f,      // metres
                          /*unitScaleValue*/  unitScale(WorldUnits::Meters),
                          /*formatWidthPx*/   1920.0f,
                          /*pixelAspect*/     1.0f,
                          /*frontMult*/       1.0f,
                          /*backMult*/        1.0f,
                          /*maxRadiusPx*/     100.0f,
                          /*sizePx*/          10.0f);
}

// The SAME lens, but with max_radius = 5px, which makes the near field of a
// [1m, 100m] range saturate against the clamp (the CoC radius reaches 5px at
// d = 3.2367m and keeps growing to 21.5px at d = 1m). This is the rig that
// exercises the rule that ΔCoC bucket spacing is uniform in the CLAMPED CoC
// (min(coc, max_radius)) -- the saturated-plateau case the no-saturation rig
// above deliberately avoids.
// Without it, spacing on the raw (unclamped) CoC passes every other test in
// this file, so this fixture is load-bearing, not a variation.
CocParams makeSaturatingPhysicalParams()
{
    CocParams p = makeNoSaturationPhysicalParams();
    p._maxRadiusPx = 5.0f;
    p.recomputeDerived();
    return p;
}

// Asserts the bounded-ΔCoC post-condition along ONE side of the focal plane.
//
// `radii` are radiusPixels() evaluated at that side's boundaries, in boundary
// order. radiusPixels() is already clamped to max_radius, so this quantity IS
// the clamped CoC the spacing is defined in -- which is why the saturating rig
// can be fed through this same assertion rather than a weakened one.
//
// The 1e-4 relative band is not fitted to what the implementation emits: the
// measured worst relative step deviation is 3.6e-06 (front side, no-saturation
// [5,100] rig), 2.1e-06 (front side, saturating rig) and 1.0e-06 (back sides),
// so this is ~28x headroom over the builder's double->float chain while still
// catching a 1% progressive drift in the step (verified by mutation).
void checkUniformCocSteps(const std::vector<float>& radii, bool decreasing)
{
    // A side with fewer than three boundaries makes the uniform-step assertion
    // vacuous, so fail loudly rather than silently skipping it.
    REQUIRE(radii.size() >= 3);

    const float step0 = decreasing ? (radii[0] - radii[1]) : (radii[1] - radii[0]);
    REQUIRE(step0 > 0.0f);

    for (std::size_t i = 0; i + 1 < radii.size(); ++i) {
        const float step = decreasing ? (radii[i] - radii[i + 1])
                                       : (radii[i + 1] - radii[i]);
        CHECK(step > 0.0f);                                 // strictly monotone
        CHECK(std::fabs(step - step0) <= 1e-4f * step0);    // uniform
        CHECK(step <= step0 * (1.0f + 1e-4f));              // bounded
    }
}

// radiusPixels() at every boundary on one side of focus, in boundary order.
std::vector<float> sideRadii(const CocParams& p, const DepthBuckets& b,
                             bool front)
{
    std::vector<float> r;
    const int lo = front ? 0 : b.focusBoundary();
    const int hi = front ? b.focusBoundary() : b.bucketCount();
    for (int i = lo; i <= hi; ++i)
        r.push_back(radiusPixels(p, b.boundary(i)));
    return r;
}

} // namespace

// ===========================================================================
// CoC math
// ===========================================================================

TEST_CASE("cocMillimeters matches a hand-derived lens table value (50mm f/2.8, S=2m, d=4m)")
{
    // Hand derivation (mirrors the CoC model comment on CocParams):
    //   S_mm = 2000, f = 50, N = 2.8, d_mm = 4000
    //   cocScale = (f/N)*f / (S_mm - f) = (50/2.8)*50 / 1950
    //            = 17.857142857142858 * 50 / 1950 = 892.8571428571429 / 1950
    //            = 0.45787545787545787
    //   coc_mm(d) = cocScale * |1 - S_mm/d_mm| = 0.45787545787545787 * |1 - 0.5|
    //             = 0.45787545787545787 * 0.5 = 0.22893772893772894 mm
    // i.e. 0.2289mm to 4 decimal places.
    //
    // The literal below is that closed-form value, NOT a re-derivation through
    // the code under test -- it is reachable with a pocket calculator from the
    // thin-lens formula alone. The epsilon is 1e-6, i.e. a ~5.4e-06 relative
    // band around it; the float implementation lands 7.1e-08 relative away
    // (measured), so this is ~76x headroom and still tight enough that a wrong
    // literal in the 5th significant figure fails.
    CocParams p = makeCocParams(CocMode::Physical, 50.0f, 2.8f, 36.0f,
                                 2.0f, unitScale(WorldUnits::Meters),
                                 1920.0f, 1.0f, 1.0f, 1.0f, 1000.0f, 10.0f);

    const float coc = cocMillimeters(p, 4.0f);
    CHECK(coc == doctest::Approx(0.22893772893772894).epsilon(1e-6)); // == 0.2289mm
}

TEST_CASE("cocMillimeters is invariant to world_units for an equivalent physical setup (metres vs centimetres)")
{
    // Same S=2m/d=4m physical setup as above, but expressed in centimetres
    // (unitScale = 10 per world_units table) instead of metres (unitScale =
    // 1000): S=200cm, d=400cm both convert to the same S_mm=2000, d_mm=4000,
    // so the mm-space result must be identical -- this is exactly the "mixing
    // mm/scene-units is wrong by orders of magnitude" contract world_units
    // exists to guarantee.
    CocParams pMeters = makeCocParams(CocMode::Physical, 50.0f, 2.8f, 36.0f,
                                       2.0f, unitScale(WorldUnits::Meters),
                                       1920.0f, 1.0f, 1.0f, 1.0f, 1000.0f, 10.0f);
    CocParams pCentimeters = makeCocParams(CocMode::Physical, 50.0f, 2.8f, 36.0f,
                                            200.0f, unitScale(WorldUnits::Centimeters),
                                            1920.0f, 1.0f, 1.0f, 1.0f, 1000.0f, 10.0f);

    const float cocM  = cocMillimeters(pMeters, 4.0f);
    const float cocCm = cocMillimeters(pCentimeters, 400.0f);
    CHECK(cocM == doctest::Approx(cocCm).epsilon(1e-6));
    // Same independent closed-form literal as the test above.
    CHECK(cocCm == doctest::Approx(0.22893772893772894).epsilon(1e-6));
}

TEST_CASE("signedCocPixels: documented edge behaviour")
{
    CocParams p = makeNoSaturationPhysicalParams();

    SUBCASE("depth == +infinity -> finite radius at the far-field limit (invD = 0)")
    {
        const float r = signedCocPixels(p, std::numeric_limits<float>::infinity());
        CHECK(std::isfinite(r));
        // Behind focus -> non-negative (backMult side).
        CHECK(r >= 0.0f);

        // "Finite and non-negative" alone would be satisfied by a broken
        // far-field path that returned 0, so pin the VALUE. Closed form, from
        // the thin-lens formula with invD = 0, computed independently of the
        // code under test:
        //   cocScale = (50/2.8)*50 / (10000 - 50) = 892.8571428571429 / 9950
        //            = 0.08973438621680833 mm
        //   coc(inf) = cocScale * |1 - 0| = cocScale
        //   radius   = 0.5 * coc * (1920/36) = 0.5 * 0.08973438621680833
        //                                    * 53.333333... = 2.3929169657815556 px
        // (well under max_radius = 100, so the clamp does not engage).
        // Measured float error against that literal: 2.0e-08 relative.
        CHECK(r == doctest::Approx(2.3929169657815556).epsilon(1e-6));
    }

    SUBCASE("depth == focusDistance -> exactly 0")
    {
        CHECK(signedCocPixels(p, p._focusDistance) == 0.0f);
    }

    SUBCASE("depth <= 0 -> 0")
    {
        CHECK(signedCocPixels(p, 0.0f) == 0.0f);
        CHECK(signedCocPixels(p, -5.0f) == 0.0f);
    }

    SUBCASE("NaN depth -> 0")
    {
        CHECK(signedCocPixels(p, std::numeric_limits<float>::quiet_NaN()) == 0.0f);
    }

    SUBCASE("_maxRadiusPx <= 0 -> 0")
    {
        CocParams p2 = p;
        p2._maxRadiusPx = 0.0f;
        p2.recomputeDerived();
        CHECK(signedCocPixels(p2, 1.0f) == 0.0f);
    }

    SUBCASE("sign convention: negative in front of focus, positive behind it")
    {
        const float front = signedCocPixels(p, p._focusDistance - 1.0f);
        const float back  = signedCocPixels(p, p._focusDistance + 1.0f);
        CHECK(front < 0.0f);
        CHECK(back > 0.0f);
        CHECK(radiusPixels(p, p._focusDistance - 1.0f) == doctest::Approx(-front));
        CHECK(radiusPixels(p, p._focusDistance + 1.0f) == doctest::Approx(back));
    }
}

TEST_CASE("signedCocPixels magnitude matches the cocMillimeters relation (physical, unclamped)")
{
    // Uses the no-saturation rig so the max_radius clamp never engages and the
    // identity holds to float rounding rather than being clipped.
    CocParams p = makeNoSaturationPhysicalParams();
    const float depth = 25.0f; // behind focus, well inside [1,100]m

    // PRIMARY assertion: an independent closed-form literal, so this test is
    // not just the code re-run through the same expression.
    //   cocScale = 892.8571428571429 / 9950 = 0.08973438621680833 mm
    //   coc(25m) = cocScale * |1 - 10000/25000| = cocScale * 0.6
    //            = 0.053840631730085 mm
    //   radius   = 0.5 * 0.053840631730085 * (1920/36)
    //            = 1.4357501794689334 px
    // Measured float error against that literal: 4.7e-08 relative.
    CHECK(radiusPixels(p, depth) == doctest::Approx(1.4357501794689334).epsilon(1e-6));

    // SECONDARY: the same number reached through cocMillimeters(), which pins
    // the radius = 0.5 * coc_mm * pxPerMm * mult composition documented on
    // CocParams. On its own this would be a tautology (it is the code's own
    // expression), which is why it follows the literal rather than replacing it.
    const float expected = 0.5f * cocMillimeters(p, depth) * p._pxPerMm * p._backMult;
    CHECK(radiusPixels(p, depth) == doctest::Approx(expected).epsilon(1e-6));
}

TEST_CASE("Manual mode: `size` is the blur radius at d=infinity, not a diameter (no halving)")
{
    // The knob table calls `size` a radius, so Manual mode must NOT apply the
    // /2 the Physical branch uses (that branch genuinely computes a CoC
    // diameter).
    CocParams p = makeCocParams(CocMode::Manual, 50.0f, 2.8f, 36.0f,
                                 10.0f, unitScale(WorldUnits::Meters),
                                 1920.0f, 1.0f, 1.0f, 1.0f, 100.0f,
                                 /*sizePx*/ 10.0f);

    const float r = radiusPixels(p, std::numeric_limits<float>::infinity());
    CHECK(r == doctest::Approx(10.0f)); // == size, NOT size/2

    CHECK(signedCocPixels(p, p._focusDistance) == 0.0f);
    CHECK(signedCocPixels(p, 0.0f) == 0.0f);
}

TEST_CASE("rayDistanceToZ matches the pinhole projection formula")
{
    // z = rayDist * f / sqrt(f^2 + r^2); pick f=50mm, r=30mm, rayDist=100mm.
    // sqrt(2500+900) = sqrt(3400) = 58.309518...
    // z = 100 * 50 / 58.309518 = 5000 / 58.309518 = 85.756...
    const float z = rayDistanceToZ(100.0f, 50.0f, 30.0f);
    CHECK(z == doctest::Approx(85.756).epsilon(0.001));

    // focalLengthMm <= 0 degrades to passing rayDist through unchanged.
    CHECK(rayDistanceToZ(100.0f, 0.0f, 30.0f) == 100.0f);
}

// ===========================================================================
// Disc kernel LUT
// ===========================================================================

TEST_CASE("DiscKernelLUT: every entry normalizes to sum(weights) == 1 within 1e-6")
{
    // normalizeEntry() rounds each scaled weight back to float, so an entry's
    // weights do not re-sum to a bit-exact 1.0: measured over every entry of a
    // [0,100] LUT the worst |sum - 1| is ~5e-8. Assert the 1e-6 bound (NOT
    // float equality) -- more than an order of magnitude of headroom above
    // that worst case.
    DiscKernelLUT lut(0.0f, 40.0f, 1.0f, 1.0f);

    double worstAbsErr = 0.0;
    for (int i = 0; i < lut.entryCount(); ++i) {
        const float radius = lut.entryRadius(i);
        const KernelView v = lut.kernel(radius, 0, 0, 0.0f, 0);
        REQUIRE(v.valid());

        double sum = 0.0;
        for (int row = 0; row < v.rowCount; ++row) {
            const RowSpan& span = v.row(row);
            if (span.empty())
                continue;
            const float* w = v.rowWeights(row);
            for (int x = 0; x < span.count(); ++x)
                sum += static_cast<double>(w[x]);
        }

        const double err = std::fabs(sum - 1.0);
        worstAbsErr = std::max(worstAbsErr, err);
        CHECK(err < 1e-6);
    }
    CHECK(worstAbsErr < 1e-6);
}

TEST_CASE("discEdgeWeight: the AA ramp is monotonically non-increasing in radial distance")
{
    const float radius = 8.0f;
    const float softness = 2.0f;

    float prev = discEdgeWeight(0.0f, radius, softness);
    CHECK(prev == doctest::Approx(1.0f));

    for (float r = 0.25f; r <= radius + softness + 1.0f; r += 0.25f) {
        const float w = discEdgeWeight(r, radius, softness);
        CHECK(w <= prev + 1e-6f); // non-increasing, tiny float slack
        CHECK(w >= 0.0f);
        CHECK(w <= 1.0f);
        prev = w;
    }

    // Hard-edge case (softness == 0): step function, no ramp.
    CHECK(discEdgeWeight(radius - 0.01f, radius, 0.0f) == doctest::Approx(1.0f));
    CHECK(discEdgeWeight(radius + 0.01f, radius, 0.0f) == doctest::Approx(0.0f));
}

// ===========================================================================
// Holdout visibility
// ===========================================================================

TEST_CASE("HoldoutVisibility::inSpan step, product and in-span exponential identities")
{
    SUBCASE("real span: step outside, exponential inside")
    {
        // zFront=0, zBack=10, alpha=0.75. In front -> 1; behind -> (1-alpha);
        // midpoint -> (1-alpha)^0.5 = 0.25^0.5 = 0.5 exactly.
        CHECK(HoldoutVisibility::inSpan(0.0f, 10.0f, 0.75f, -1.0f) == doctest::Approx(1.0f));
        CHECK(HoldoutVisibility::inSpan(0.0f, 10.0f, 0.75f, 11.0f) == doctest::Approx(0.25f));
        CHECK(HoldoutVisibility::inSpan(0.0f, 10.0f, 0.75f, 5.0f) == doctest::Approx(0.5f).epsilon(1e-6));
    }

    SUBCASE("degenerate (point) sample: full attenuation from zFront onward, no span to interpolate")
    {
        CHECK(HoldoutVisibility::inSpan(3.0f, 3.0f, 0.5f, 2.9f) == doctest::Approx(1.0f));
        CHECK(HoldoutVisibility::inSpan(3.0f, 3.0f, 0.5f, 3.0f) == doctest::Approx(0.5f));
        CHECK(HoldoutVisibility::inSpan(3.0f, 3.0f, 0.5f, 100.0f) == doctest::Approx(0.5f));
    }

    SUBCASE("evalExact is the product of inSpan over all samples")
    {
        const float zFront[3] = {0.0f, 2.0f, 5.0f};
        const float zBack[3]  = {1.0f, 4.0f, 6.0f};
        const float alpha[3]  = {0.5f, 0.3f, 0.9f};

        const float z = 3.5f;
        const float manualProduct = HoldoutVisibility::inSpan(zFront[0], zBack[0], alpha[0], z)
                                   * HoldoutVisibility::inSpan(zFront[1], zBack[1], alpha[1], z)
                                   * HoldoutVisibility::inSpan(zFront[2], zBack[2], alpha[2], z);

        const float exact = HoldoutVisibility::evalExact(zFront, zBack, alpha, 3, z);
        CHECK(exact == doctest::Approx(manualProduct).epsilon(1e-6));
    }
}

TEST_CASE("HoldoutVisibility::build matches evalBoundaries for sorted input and falls back correctly for unsorted input")
{
    // Sorted (zFront ascending), the precondition build()'s O(H+K) walk needs.
    const float zFrontSorted[3] = {0.0f, 3.0f, 7.0f};
    const float zBackSorted[3]  = {2.0f, 5.0f, 9.0f};
    const float alphaSorted[3]  = {0.4f, 0.6f, 0.8f};
    const float boundaries[5]   = {-1.0f, 1.0f, 4.0f, 6.0f, 10.0f};

    float outBuild[5];
    float outRef[5];
    HoldoutVisibility::build(zFrontSorted, zBackSorted, alphaSorted, 3, boundaries, 5, outBuild);
    HoldoutVisibility::evalBoundaries(zFrontSorted, zBackSorted, alphaSorted, 3, boundaries, 5, outRef);

    for (int i = 0; i < 5; ++i)
        CHECK(outBuild[i] == doctest::Approx(outRef[i]).epsilon(1e-6));

    // Unsorted input: build() detects the violated precondition and falls
    // back to evalBoundaries() itself (per the header's "always bit-identical
    // to evalBoundaries(), sorted input or not" contract), so build() on the
    // unsorted array must still match evalBoundaries() on that SAME unsorted
    // array.
    const float zFrontUnsorted[3] = {7.0f, 0.0f, 3.0f};
    const float zBackUnsorted[3]  = {9.0f, 2.0f, 5.0f};
    const float alphaUnsorted[3]  = {0.8f, 0.4f, 0.6f};

    float outBuildUnsorted[5];
    float outRefUnsorted[5];
    HoldoutVisibility::build(zFrontUnsorted, zBackUnsorted, alphaUnsorted, 3, boundaries, 5, outBuildUnsorted);
    HoldoutVisibility::evalBoundaries(zFrontUnsorted, zBackUnsorted, alphaUnsorted, 3, boundaries, 5, outRefUnsorted);

    for (int i = 0; i < 5; ++i)
        CHECK(outBuildUnsorted[i] == doctest::Approx(outRefUnsorted[i]).epsilon(1e-6));

    // And the unsorted-vs-sorted result must agree too, since it's the same
    // sample set.
    for (int i = 0; i < 5; ++i)
        CHECK(outBuildUnsorted[i] == doctest::Approx(outBuild[i]).epsilon(1e-6));
}

TEST_CASE("HoldoutVisibility boundary-LUT interpolation is exact for a single full-range sample, "
          "and a BOUNDARY locator (not bucketOf) is the correct feed for interpAtBucket")
{
    // NOTE: in production the locator is HoldoutBoundaries::locate()
    // over the holdout LUT's OWN boundary set, not DepthBuckets::locateBoundary()
    // over the ΔCoC bucket set -- the two sets are decoupled and a pair from one
    // must never index the other.  What this case pins is the property the two
    // share and that bucketOf() does not: the fraction must be measured between
    // BOUNDARIES, not between bucket CENTRES.  DepthBuckets is used here only
    // because it is a convenient locator over the very array the LUT below is
    // built at; see the HoldoutBoundaries case that follows for the production
    // locator's own contract.

    // A single holdout sample spanning the whole boundary range makes log(T)
    // exactly linear in z across every interval (no sample edge falls inside
    // any bucket), so interpAtBucket() fed the correct boundary-fraction must
    // reproduce evalExact() to float precision -- this is the "interpolation
    // vs exact eval" identity.
    const float zFront[1] = {-100.0f};
    const float zBack[1]  = {100.0f};
    const float alpha[1]  = {0.6f};

    const float boundaries[5] = {-10.0f, -2.0f, 3.0f, 6.0f, 20.0f}; // deliberately non-uniform
    float boundaryT[5];
    HoldoutVisibility::build(zFront, zBack, alpha, 1, boundaries, 5, boundaryT);

    const float z = 4.5f; // strictly inside bucket [3, 6)
    const float exact = HoldoutVisibility::evalExact(zFront, zBack, alpha, 1, z);

    // Build a DepthBuckets whose _boundaries match the array above exactly and
    // whose _centres are deliberately NOT the boundary midpoints -- exactly
    // the case that goes wrong: bucketOf() measures position between bucket
    // CENTRES, so its fraction is a different number from locateBoundary()'s,
    // and feeding the wrong one into interpAtBucket() silently reads back the
    // wrong value.
    DepthBuckets buckets;
    buckets._bucketCount = 4;
    for (int i = 0; i < 5; ++i)
        buckets._boundaries[i] = boundaries[i];
    // Off-centre on purpose (real centres would be -6, 0.5, 4.5, 13).
    buckets._centres[0] = -9.0f;
    buckets._centres[1] = -1.5f;
    buckets._centres[2] = 5.5f;
    buckets._centres[3] = 19.0f;

    const BoundarySpan bs = buckets.locateBoundary(z);
    const float visCorrect = HoldoutVisibility::interpAtBucket(boundaryT, 5, bs.index, bs.frac);
    CHECK(visCorrect == doctest::Approx(exact).epsilon(1e-5));

    const BucketWeight wrongFeed = buckets.bucketOf(z);
    const float visWrongFeed = HoldoutVisibility::interpAtBucket(boundaryT, 5, wrongFeed.index, wrongFeed.frac);
    // The wrong feed is not asserted to differ by a specific magnitude (that
    // depends on the exact bucket geometry) -- what matters as a regression
    // guard is that the CORRECT feed is measurably closer to the exact
    // reference than the wrong one is.
    const float errCorrect = std::fabs(visCorrect - exact);
    const float errWrongFeed = std::fabs(visWrongFeed - exact);
    CHECK(errCorrect < 1e-5f);
    CHECK(errCorrect <= errWrongFeed);
}

// ---------------------------------------------------------------------------
// HoldoutBoundaries -- the holdout LUT's own boundary set.
//
// These post-conditions must not be dropped: locate() is on the per-fragment
// path, and its exactness AT the boundaries is what makes "LUT == exact at the
// boundaries" a hard identity rather than a tolerance.
// ---------------------------------------------------------------------------
TEST_CASE("HoldoutBoundaries::buildUniformZ post-conditions and locate() are exact "
          "at every boundary, across the whole K range")
{
    SUBCASE("a default-constructed set is inert")
    {
        HoldoutBoundaries h;
        CHECK(h.count() == 0);
        CHECK(h.enabled() == false);
        const BoundarySpan s = h.locate(5.0f);
        CHECK(s.index == 0);
        CHECK(s.frac == 0.0f);
    }

    SUBCASE("post-conditions and locate(boundary(i)) == {i, 0} for K in [4, 128]")
    {
        const float ranges[][2] = {
            {1.0f, 100.0f},          // the default rig
            {1e-6f, 1e12f},          // the full sanitised window
            {1000.0f, 1001.0f},      // narrow and far from zero
        };

        for (const auto& r : ranges) {
            for (int k = 4; k <= 128; ++k) {
                HoldoutBoundaries h;
                h.buildUniformZ(r[0], r[1], k + 1);

                CHECK(h.count() == k + 1);
                CHECK(h.boundary(0) == r[0]);
                CHECK(h.boundary(h.count() - 1) == r[1]);

                for (int i = 1; i < h.count(); ++i)
                    CHECK(h.boundary(i) > h.boundary(i - 1));   // strictly increasing

                for (int i = 0; i < h.count() - 1; ++i) {
                    const BoundarySpan s = h.locate(h.boundary(i));
                    CHECK(s.index == i);
                    CHECK(s.frac == 0.0f);          // exactly, not approximately
                }
                const BoundarySpan last = h.locate(h.boundary(h.count() - 1));
                CHECK(last.index == h.count() - 2);
                CHECK(last.frac == 1.0f);
            }
        }
    }

    SUBCASE("the closed-form index agrees with a binary search everywhere in range")
    {
        // locate() derives the bracket from a multiply and then reconciles it
        // with the STORED (float-rounded) array in two bounded steps.  This is
        // the case that would catch that reconciliation being too narrow.
        for (int k : {4, 16, 33, 64, 128}) {
            HoldoutBoundaries h;
            h.buildUniformZ(1.0f, 100.0f, k + 1);

            for (int t = 0; t < 4000; ++t) {
                const float z = 1.0f + 99.0f * (static_cast<float>(t) / 3999.0f);
                const BoundarySpan s = h.locate(z);

                int lo = 0;
                int hi = h.count() - 1;
                while (hi - lo > 1) {
                    const int mid = lo + (hi - lo) / 2;
                    if (h.boundary(mid) <= z) lo = mid; else hi = mid;
                }
                if (z >= h.boundary(h.count() - 1)) lo = h.count() - 2;

                CHECK(s.index == lo);
                CHECK(s.frac >= 0.0f);
                CHECK(s.frac <= 1.0f);
            }
        }
    }

    SUBCASE("out-of-range, NaN and infinite depths clamp rather than propagate")
    {
        HoldoutBoundaries h;
        h.buildUniformZ(1.0f, 100.0f, 17);

        const float inf = std::numeric_limits<float>::infinity();
        const BoundarySpan below = h.locate(-1e30f);
        const BoundarySpan above = h.locate(1e30f);
        const BoundarySpan nan   = h.locate(std::numeric_limits<float>::quiet_NaN());
        const BoundarySpan pinf  = h.locate(inf);
        const BoundarySpan ninf  = h.locate(-inf);

        CHECK(below.index == 0);       CHECK(below.frac == 0.0f);
        CHECK(ninf.index  == 0);       CHECK(ninf.frac  == 0.0f);
        CHECK(nan.index   == 0);       CHECK(nan.frac   == 0.0f);   // NOT propagated
        CHECK(above.index == 15);      CHECK(above.frac == 1.0f);
        CHECK(pinf.index  == 15);      CHECK(pinf.frac  == 1.0f);
    }

    SUBCASE("degenerate ranges still produce a usable strictly-increasing set")
    {
        HoldoutBoundaries h;

        h.buildUniformZ(50.0f, 50.0f, 17);              // zero-width measurement
        CHECK(h.count() == 17);
        CHECK(h.boundary(16) > h.boundary(0));

        h.buildUniformZ(100.0f, 1.0f, 17);              // reversed
        CHECK(h.boundary(0) == 1.0f);
        CHECK(h.boundary(16) == 100.0f);

        h.buildUniformZ(std::numeric_limits<float>::quiet_NaN(),
                        std::numeric_limits<float>::quiet_NaN(), 17);
        CHECK(h.boundary(16) > h.boundary(0));

        h.buildUniformZ(1.0f, 100.0f, 1);               // count clamps up to 2
        CHECK(h.count() == 2);

        h.buildUniformZ(1.0f, 100.0f, 100000);          // and down to kMaxBoundaries
        CHECK(h.count() == HoldoutBoundaries::kMaxBoundaries);
    }
}

TEST_CASE("HoldoutBoundaries::locate + interpAtBucket reproduces HoldoutVisibility::interp, "
          "and is BIT-EXACT against evalExact at the boundaries")
{
    // The identity: the O(1) closed-form pair and the O(log n) searching entry
    // point are the same number, and AT a boundary the LUT is not an
    // approximation of anything.
    HoldoutBoundaries h;
    h.buildUniformZ(1.0f, 100.0f, 17);

    const float zFront[3] = {12.0f, 40.0f, 71.0f};
    const float zBack[3]  = {12.0f, 55.0f, 71.0f};   // point, span, point
    const float alpha[3]  = {0.25f, 0.6f, 1.0f};

    float boundaryT[17];
    HoldoutVisibility::build(zFront, zBack, alpha, 3,
                             h.boundaries(), h.count(), boundaryT);

    for (int i = 0; i < h.count(); ++i) {
        const BoundarySpan s = h.locate(h.boundary(i));
        const float lut = HoldoutVisibility::interpAtBucket(boundaryT, h.count(),
                                                           s.index, s.frac);
        const float exact = clampf(HoldoutVisibility::evalExact(zFront, zBack, alpha, 3,
                                                               h.boundary(i)), 0.0f, 1.0f);
        CHECK(lut == exact);            // bit-exact, not Approx
    }

    for (int t = 0; t <= 200; ++t) {
        const float z = 1.0f + 99.0f * (static_cast<float>(t) / 200.0f);
        const BoundarySpan s = h.locate(z);
        const float viaPair = HoldoutVisibility::interpAtBucket(boundaryT, h.count(),
                                                               s.index, s.frac);
        const float viaSearch = HoldoutVisibility::interp(h.boundaries(), boundaryT,
                                                         h.count(), z);
        CHECK(viaPair == doctest::Approx(viaSearch).epsilon(1e-6));
    }
}

// ---------------------------------------------------------------------------
// interpAtBucket() -- the LUT-vs-exact identities, plus the interpolant's
// behaviour on an underflowed bracket (see the holdout-interpolant block in
// DeepCDefocusMath.h).  The last case pins the floored log chord's decay
// across such a bracket.
// ---------------------------------------------------------------------------

TEST_CASE("interpAtBucket identities: LUT vs exact at boundaries, all-ones, "
          "fully-behind/in-front, deep low-alpha precision, hard edge")
{
    SUBCASE("LUT vs exact is 0.000e+00 at every boundary")
    {
        HoldoutBoundaries h;
        h.buildUniformZ(1.0f, 100.0f, 17);
        const float zFront[3] = {12.0f, 40.0f, 71.0f};
        const float zBack[3]  = {12.0f, 55.0f, 71.0f};
        const float alpha[3]  = {0.25f, 0.6f, 1.0f};
        float boundaryT[17];
        HoldoutVisibility::build(zFront, zBack, alpha, 3, h.boundaries(), h.count(), boundaryT);

        for (int i = 0; i < h.count(); ++i) {
            const BoundarySpan s = h.locate(h.boundary(i));
            const float lut = HoldoutVisibility::interpAtBucket(boundaryT, h.count(),
                                                                s.index, s.frac);
            const float exact = clampf(HoldoutVisibility::evalExact(zFront, zBack, alpha, 3,
                                                                   h.boundary(i)), 0.0f, 1.0f);
            CHECK(lut == exact);   // bit-exact at the boundaries themselves
        }
    }

    SUBCASE("all-ones LUT is bit-identical to the disabled path")
    {
        HoldoutBoundaries h;
        h.buildUniformZ(1.0f, 100.0f, 17);
        float boundaryT[17];
        for (int i = 0; i < 17; ++i) boundaryT[i] = 1.0f;

        for (float z : {1.0f, 25.0f, 50.0f, 99.0f}) {
            const BoundarySpan s = h.locate(z);
            const float v = HoldoutVisibility::interpAtBucket(boundaryT, h.count(),
                                                              s.index, s.frac);
            CHECK(v == 1.0f);
        }
    }

    SUBCASE("fully behind an opaque holdout is exactly 0, fully in front is bit-identical "
            "to no-holdout (1.0)")
    {
        HoldoutBoundaries h;
        h.buildUniformZ(1.0f, 100.0f, 17);
        const float zFront[1] = {10.0f};
        const float zBack[1]  = {10.0f};
        const float alpha[1]  = {1.0f};
        float boundaryT[17];
        HoldoutVisibility::build(zFront, zBack, alpha, 1, h.boundaries(), h.count(), boundaryT);

        const BoundarySpan behind = h.locate(99.0f);
        CHECK(HoldoutVisibility::interpAtBucket(boundaryT, h.count(), behind.index,
                                                behind.frac) == 0.0f);

        const BoundarySpan front = h.locate(1.0f);
        CHECK(HoldoutVisibility::interpAtBucket(boundaryT, h.count(), front.index,
                                                front.frac) == 1.0f);
    }

    SUBCASE("alpha=1e-7, 200-sample stack: precision unregressed (the bracket's T1 "
            "never underflows here)")
    {
        HoldoutBoundaries h;
        h.buildUniformZ(1.0f, 100.0f, 17);
        float zFront[200], zBack[200], alpha[200];
        for (int i = 0; i < 200; ++i) {
            zFront[i] = 10.0f + 0.001f * static_cast<float>(i);
            zBack[i]  = zFront[i];
            alpha[i]  = 1e-7f;
        }
        float boundaryT[17];
        HoldoutVisibility::build(zFront, zBack, alpha, 200, h.boundaries(), h.count(), boundaryT);

        const BoundarySpan s = h.locate(50.0f);
        const float v = HoldoutVisibility::interpAtBucket(boundaryT, h.count(), s.index, s.frac);
        CHECK(v == doctest::Approx(1.0f).epsilon(1e-4));   // ~1 ulp-scale of 1.0
    }

    SUBCASE("the spatial hard silhouette edge stays exactly one pixel wide: "
            "interpAtBucket() reads exactly ONE pixel's own boundaryT array, so "
            "an occluded pixel and its clear neighbour can never blend into each other")
    {
        HoldoutBoundaries h;
        h.buildUniformZ(1.0f, 100.0f, 17);

        // Pixel A: an opaque card directly in front of the test fragment.
        const float zFrontA[1] = {10.0f};
        const float zBackA[1]  = {10.0f};
        const float alphaA[1]  = {1.0f};
        float lutA[17];
        HoldoutVisibility::build(zFrontA, zBackA, alphaA, 1, h.boundaries(), h.count(), lutA);

        // Pixel B: no holdout at all (the disabled path's all-ones LUT).
        float lutB[17];
        for (int i = 0; i < 17; ++i) lutB[i] = 1.0f;

        const BoundarySpan s = h.locate(50.0f);
        const float vA = HoldoutVisibility::interpAtBucket(lutA, h.count(), s.index, s.frac);
        const float vB = HoldoutVisibility::interpAtBucket(lutB, h.count(), s.index, s.frac);
        CHECK(vA == 0.0f);   // occluded pixel: exactly 0, not partially blended toward B
        CHECK(vB == 1.0f);   // clear pixel: exactly 1, not partially blended toward A
    }
}

TEST_CASE("underflowed brackets: T1==0 is NOT exclusive to alpha==1 content, and the "
          "floored log chord decays as 10^(-30*frac) across such a bracket")
{
    // A dense alpha<1 stack underflows the stored far-boundary transmittance
    // to bitwise 0.0f, and the chord then reads kMinTransmittance = 1e-30 as
    // its far endpoint -- the erase-toward-camera residual that harness check
    // f2 pins in rendered pixels.  Both the small-n safe regime and the dense
    // divergent one are pinned so neither regresses silently.

    SUBCASE("small-n regime (<=4 samples), even at very high alpha, stays well clear of "
            "underflow")
    {
        // Four samples at alpha=0.999999 (as close to 1 as is meaningfully
        // distinct from it): (1-alpha)^4 = (1e-6)^4 = 1e-24, nowhere near
        // float's ~1.4e-45 denormal floor.
        const float zFront[4] = {40.0f, 41.0f, 42.0f, 43.0f};
        const float zBack[4]  = {40.0f, 41.0f, 42.0f, 43.0f};
        const float alpha[4]  = {0.999999f, 0.999999f, 0.999999f, 0.999999f};
        HoldoutBoundaries h;
        h.buildUniformZ(1.0f, 100.0f, 17);
        float boundaryT[17];
        HoldoutVisibility::build(zFront, zBack, alpha, 4, h.boundaries(), h.count(), boundaryT);

        const BoundarySpan s = h.locate(50.0f);
        CHECK(boundaryT[s.index + 1] > 0.0f);   // did NOT underflow to bitwise 0
    }

    SUBCASE("46 samples at alpha=0.9, packed inside one K=16 bracket, underflow the far "
            "boundary to bitwise 0.0f -- with every sample individually alpha<1 -- and "
            "the chord's reading there is the floor's 10^(-30*frac), banded two-sided")
    {
        const int n = 46;
        float zFront[n], zBack[n], alpha[n];
        for (int i = 0; i < n; ++i) {
            // Spread strictly within the bracket [44.3125, 50.5] (K=16, range
            // [1,100]) so the NEAR boundary sees none of them settled yet.
            zFront[i] = 44.4f + (50.4f - 44.4f) * (static_cast<float>(i) / n);
            zBack[i]  = zFront[i];
            alpha[i]  = 0.9f;
        }
        HoldoutBoundaries h;
        h.buildUniformZ(1.0f, 100.0f, 17);
        float boundaryT[17];
        HoldoutVisibility::build(zFront, zBack, alpha, n, h.boundaries(), h.count(), boundaryT);

        const BoundarySpan s = h.locate(48.0f);   // inside the packed bracket
        REQUIRE(h.boundary(s.index)     == doctest::Approx(44.3125f));
        REQUIRE(h.boundary(s.index + 1) == doctest::Approx(50.5f));

        CHECK(boundaryT[s.index]     == 1.0f);   // near boundary: nothing settled yet
        CHECK(boundaryT[s.index + 1] == 0.0f);   // far boundary: underflowed, NOT alpha==1

        const float v = HoldoutVisibility::interpAtBucket(boundaryT, h.count(), s.index, s.frac);

        // TWO-SIDED band around the floored chord's closed form
        // 10^(-30*frac) at z=48, frac ~0.59596 -> 1.32e-18 (the rendered rig
        // reads the same decay at 0.2512 = 10^(-30*0.02) two percent into a
        // bracket).  A regression that erases outright (0.0)
        // fails the lower bound; one that leaks (a raised floor, or losing
        // the floor entirely) fails the upper bound.
        CHECK(v > 1.0e-18f);
        CHECK(v < 1.7e-18f);
        const float frac = s.frac;
        const float closedForm = std::pow(10.0f, -30.0f * frac);
        CHECK(v == doctest::Approx(closedForm).epsilon(1e-2));
    }
}

TEST_CASE("makeUniformHoldoutBoundaries takes its count and range from the buckets, "
          "and is NOT the ΔCoC boundary set")
{
    // The holdout set's whole point: same K+1 entries (so per-band LUT memory
    // is (K+1)*W*B*4), same measured range, deliberately different
    // placement.  On the node's defaults the ΔCoC set spends 15 of 16 buckets
    // inside [1,10]; the holdout set must not.
    const CocParams p = makeCocParams(CocMode::Physical, 50.0f, 2.8f, 36.0f,
                                      10.0f, 1000.0f, 1920.0f, 1.0f,
                                      1.0f, 1.0f, 100.0f, 10.0f);
    DepthBuckets buckets;
    buckets.buildBoundedDeltaCoc(p, 1.0f, 100.0f, 16);

    const HoldoutBoundaries h = makeUniformHoldoutBoundaries(buckets);

    CHECK(h.count() == buckets.boundaryCount());
    CHECK(h.depthMin() == buckets.depthMin());
    CHECK(h.depthMax() == buckets.depthMax());

    // The ΔCoC set's second-to-last boundary is at 10; the uniform one is not.
    CHECK(buckets.boundary(buckets.boundaryCount() - 2) == doctest::Approx(10.0f).epsilon(1e-3));
    CHECK(h.boundary(h.count() - 2) > 90.0f);

    // An opaque card at z=50 must land in a bracket that CONTAINS it, not in one
    // starting at 10.
    const BoundarySpan hs = h.locate(50.0f);
    CHECK(h.boundary(hs.index) > 40.0f);
    CHECK(h.boundary(hs.index + 1) < 55.0f);
}

// ===========================================================================
// Depth buckets
// ===========================================================================

TEST_CASE("DepthBuckets::buildBoundedDeltaCoc post-conditions")
{
    CocParams p = makeNoSaturationPhysicalParams();
    DepthBuckets buckets = makeBoundedDeltaCocBuckets(p, 1.0f, 100.0f, 16);

    CHECK(buckets.bucketCount() == 16);
    CHECK(buckets.boundary(0) == doctest::Approx(1.0f));
    CHECK(buckets.boundary(16) == doctest::Approx(100.0f).epsilon(1e-4));

    for (int i = 1; i <= buckets.bucketCount(); ++i)
        CHECK(buckets.boundary(i) > buckets.boundary(i - 1));
}

TEST_CASE("DepthBuckets: ΔCoC boundary spacing is uniform per side of focus and bounded")
{
    SUBCASE("no saturation, both sides well populated: uniform step on BOTH sides")
    {
        // Range [5,100]m against the no-saturation rig (max_radius=100px, CoC
        // radius ~2.39px at 5m and ~2.15px at 100m, so the clamp never
        // engages). The two CoC spans are nearly equal, so the bucket budget
        // splits 8/8 and BOTH sides genuinely have enough boundaries for the
        // uniform-step assertion to mean something -- see the asymmetric
        // subcase below for why that has to be arranged deliberately.
        CocParams p = makeNoSaturationPhysicalParams();
        DepthBuckets buckets = makeBoundedDeltaCocBuckets(p, 5.0f, 100.0f, 16);

        REQUIRE(buckets.focusBoundary() == 8);   // 8 front buckets, 8 back

        checkUniformCocSteps(sideRadii(p, buckets, /*front*/ true),  /*decreasing*/ true);
        checkUniformCocSteps(sideRadii(p, buckets, /*front*/ false), /*decreasing*/ false);
    }

    SUBCASE("SATURATED near field: spacing is uniform in the CLAMPED CoC and the "
            "plateau costs exactly one bucket")
    {
        // Near-field CoC is unbounded as d -> 0, so spacing on the RAW value
        // would let the saturated plateau consume the whole bucket budget;
        // spacing on min(coc, max_radius) spends exactly one bucket there.
        // Every other test in this file uses a rig where the clamp never
        // engages, so without this subcase that rule is unverified --
        // mutating the builder to space on the unclamped CoC passes everything
        // else (confirmed by mutation testing).
        CocParams p = makeSaturatingPhysicalParams();   // max_radius = 5px
        DepthBuckets buckets = makeBoundedDeltaCocBuckets(p, 1.0f, 100.0f, 16);

        // The clamped CoC spans 5px in front of focus and 2.154px behind it,
        // so the budget splits 11/5 -- both sides populated enough to assert on.
        REQUIRE(buckets.focusBoundary() == 11);

        // Uniform step in the CLAMPED CoC (radiusPixels() is already clamped).
        checkUniformCocSteps(sideRadii(p, buckets, /*front*/ true),  /*decreasing*/ true);
        checkUniformCocSteps(sideRadii(p, buckets, /*front*/ false), /*decreasing*/ false);

        // Exactly ONE bucket for the whole saturated plateau: the CoC radius
        // hits max_radius at d = 3.2367m and stays there down to d = 1m, so
        // boundary(0) (== depthMin == 1m) must be the ONLY boundary sitting at
        // max_radius. Under unclamped spacing several boundaries land inside
        // the plateau and this count rises.
        int saturatedBoundaries = 0;
        for (int i = 0; i <= buckets.bucketCount(); ++i) {
            if (radiusPixels(p, buckets.boundary(i)) >= p._maxRadiusPx - 1e-4f)
                ++saturatedBoundaries;
        }
        CHECK(saturatedBoundaries == 1);

        // ...and that one bucket really does contain the whole plateau: the
        // saturation edge (3.2367m) sits inside bucket 0, i.e. below boundary(1).
        CHECK(radiusPixels(p, 3.20f) == doctest::Approx(5.0f));   // still saturated
        CHECK(radiusPixels(p, 3.30f) < 5.0f);                     // past the edge
        CHECK(buckets.boundary(1) > 3.2367f);
    }

    SUBCASE("documented asymmetric split: a side that gets 1 bucket still has a "
            "bounded step (15/1 at S=10, range [1,100], K=16)")
    {
        // This configuration is the case where integer bucket allocation
        // leaves the front and back steps 1.5x apart.  Pinned here so a change
        // in the budget-split rule is visible.
        CocParams p = makeNoSaturationPhysicalParams();
        DepthBuckets buckets = makeBoundedDeltaCocBuckets(p, 1.0f, 100.0f, 16);

        CHECK(buckets.focusBoundary() == 15);   // 15 front buckets, 1 back

        checkUniformCocSteps(sideRadii(p, buckets, /*front*/ true), /*decreasing*/ true);

        // Back side has a single bucket -- no uniformity to assert, but its
        // step must still be finite, positive and of the documented magnitude.
        const std::vector<float> back = sideRadii(p, buckets, /*front*/ false);
        REQUIRE(back.size() == 2);
        const float backStep = back[1] - back[0];
        const float frontStep = sideRadii(p, buckets, true)[0] - sideRadii(p, buckets, true)[1];
        CHECK(backStep > 0.0f);
        // Exactly 1.5, not approximately: the CoC spans are in ratio 10:1
        // (|1-10/1| = 9 vs |1-10/100| = 0.9) and the budget splits 15:1, so
        // (9/15) : (0.9/1) = 1 : 1.5 by construction.
        CHECK(backStep == doctest::Approx(1.5f * frontStep).epsilon(1e-4)); // the logged 1.5x
    }
}

TEST_CASE("DepthBuckets::bucketOf: fractional two-bucket assignment is an exact partition of unity")
{
    // BucketWeight's doc claims this is a HARD guarantee under round-to-
    // nearest (Sterbenz / sub-half-ulp argument), not a tolerance -- so this
    // test asserts exact equality, not Approx.
    CocParams p = makeNoSaturationPhysicalParams();
    DepthBuckets buckets = makeBoundedDeltaCocBuckets(p, 1.0f, 100.0f, 16);

    for (float depth = 1.0f; depth <= 100.0f; depth += 0.37f) {
        const BucketWeight w = buckets.bucketOf(depth);
        CHECK(w.weightLow() + w.weightHigh() == 1.0f);
        CHECK(w.index >= 0);
        CHECK(w.index < buckets.bucketCount());
    }

    // Outside the outermost centres saturates to frac 0.
    const BucketWeight below = buckets.bucketOf(buckets.centre(0) - 1000.0f);
    CHECK(below.frac == 0.0f);
    CHECK(below.index == 0);

    const int last = buckets.bucketCount() - 1;
    const BucketWeight above = buckets.bucketOf(buckets.centre(last) + 1000.0f);
    CHECK(above.frac == 0.0f);
    CHECK(above.index == last);
}

// ===========================================================================
// Transmittance-preserving alpha split (partitionAlpha / partitionColorScale)
// ===========================================================================

TEST_CASE("partitionAlpha/partitionColorScale reconstruct exactly at the alpha=0 and alpha=1 endpoints")
{
    SUBCASE("alpha == 0 (emissive limit): alpha stays 0, colour scale is linear in t")
    {
        for (float t = 0.0f; t <= 1.0f; t += 0.25f) {
            CHECK(partitionAlpha(0.0f, t) == 0.0f);
            if (t > 0.0f)
                CHECK(partitionColorScale(0.0f, t) == doctest::Approx(t));
        }
    }

    SUBCASE("alpha == 1 (fully opaque): every part with t>0 is fully opaque -- the degenerate "
            "no-op that is the known cost of this form")
    {
        for (float t = 0.01f; t <= 1.0f; t += 0.11f) {
            CHECK(partitionAlpha(1.0f, t) == 1.0f);
            CHECK(partitionColorScale(1.0f, t) == 1.0f);
        }
        CHECK(partitionAlpha(1.0f, 0.0f) == 0.0f);
    }

    SUBCASE("t == 1: whole weight, part reproduces the parent alpha exactly")
    {
        for (float a = 0.0f; a <= 1.0f; a += 0.1f) {
            CHECK(partitionAlpha(a, 1.0f) == doctest::Approx(a));
            if (a > 0.0f)
                CHECK(partitionColorScale(a, 1.0f) == doctest::Approx(1.0f));
        }
    }

    SUBCASE("t == 0: no weight, no contribution")
    {
        for (float a = 0.0f; a <= 1.0f; a += 0.1f)
            CHECK(partitionAlpha(a, 0.0f) == 0.0f);
    }

    SUBCASE("reconstruction identity: two complementary parts recombine to the parent "
            "under `over`, in ALPHA and in PREMULTIPLIED COLOUR")
    {
        // BOTH quantities must reconstruct; the colour half is the one that
        // pins partitionColorScale()'s value away
        // from the endpoints (a 2% error in the interior passes every
        // alpha-only assertion -- confirmed by mutation testing).
        //
        //   A_out = 1 - (1-a_0)(1-a_1)                       == alpha
        //   C_out = C*cs_0 + C*cs_1*(1-a_0)                  == C
        //
        // Tolerances are ABSOLUTE and tied to this form's measured worst case
        // (8.3e-08 across the alpha x f grid). Measured over this test's own
        // grid: 4.6e-08 for alpha and
        // 1.3e-08 for premultiplied colour, so 1e-6 is ~22x headroom rather
        // than a tolerance fitted to the output.
        const float premultColor = 0.37f;
        for (float a : {0.01f, 0.1f, 0.5f, 0.9f, 0.999f}) {
            for (float t0 : {0.1f, 0.25f, 0.5f, 0.75f, 0.9f}) {
                const float t1 = 1.0f - t0;
                const float a0 = partitionAlpha(a, t0);
                const float a1 = partitionAlpha(a, t1);

                const float recomposed = 1.0f - (1.0f - a0) * (1.0f - a1);
                CHECK(std::fabs(recomposed - a) < 1e-6f);

                const float c0 = premultColor * partitionColorScale(a, t0);
                const float c1 = premultColor * partitionColorScale(a, t1);
                const float recomposedColor = c0 + c1 * (1.0f - a0);
                CHECK(std::fabs(recomposedColor - premultColor) < 1e-6f);
            }
        }
    }
}

TEST_CASE("Flat opaque field: fractional two-bucket deposit + front-to-back composite gives alpha == 1 exactly")
{
    // Validation scene (c)'s identity, exercised at the primitive level: an
    // opaque fragment (alpha=1) landing between two bucket centres deposits
    // FULL alpha into BOTH buckets (partitionAlpha(1,t)=1 for any t>0), so
    // the front-to-back composite of just those two buckets must read back
    // alpha exactly 1, not the ~0.75 a linear split would give.
    CocParams p = makeNoSaturationPhysicalParams();
    DepthBuckets buckets = makeBoundedDeltaCocBuckets(p, 1.0f, 100.0f, 16);

    // Pick a depth strictly between two centres (not saturated onto an end).
    const float depth = 0.5f * (buckets.centre(4) + buckets.centre(5));
    const BucketWeight w = buckets.bucketOf(depth);
    REQUIRE(w.frac > 0.0f);
    REQUIRE(w.frac < 1.0f);

    const BucketDeposit dep = fragmentDeposit(w, 1.0f);
    CHECK(dep.alpha0 == 1.0f);
    CHECK(dep.alpha1 == 1.0f);

    const int bucketCount = buckets.bucketCount();
    std::vector<float> bucketAlpha(static_cast<std::size_t>(bucketCount), 0.0f);
    bucketAlpha[static_cast<std::size_t>(dep.index0)] += dep.alpha0;
    bucketAlpha[static_cast<std::size_t>(dep.index1)] += dep.alpha1;

    // Alpha only: this identity is the split's, and colour follows it by
    // construction (premultiplied colour is scaled by alpha_i/alpha).
    const float outAlpha = overCompositeAlpha(bucketAlpha.data(), bucketCount);

    CHECK(outAlpha == doctest::Approx(1.0f).epsilon(1e-6));
}

TEST_CASE("saturateBucketPixel: alpha>1 saturates to exactly 1 with colour/alpha ratio preserved; alpha<=1 untouched")
{
    SUBCASE("overlap case: alpha=2, colour=(2,2,2) (i.e. unpremult colour (1,1,1)) -> alpha=1, colour=(1,1,1)")
    {
        float color[3] = {2.0f, 2.0f, 2.0f};
        float alpha = 2.0f;
        saturateBucketPixel(color, &alpha, 3, 1);

        CHECK(alpha == 1.0f);
        CHECK(color[0] == doctest::Approx(1.0f));
        CHECK(color[1] == doctest::Approx(1.0f));
        CHECK(color[2] == doctest::Approx(1.0f));
    }

    SUBCASE("colour:alpha ratio is preserved for NON-trivial values, to the measured 1e-6 bound")
    {
        // The alpha=2 / colour=2 case above is exact in binary floating point
        // (every operand is a power of two), so on its own it cannot detect a
        // systematic ratio drift at all -- a mutation multiplying the colour by
        // an extra (1 + 1e-5) survives it. This case uses alphas and channel
        // values with no exact binary representation, and asserts the
        // unpremultiplied colour (= the colour:alpha ratio, which is what
        // "ratio preserved" means) to a tolerance derived from this form's
        // measured 7.45e-08 drift: worst RELATIVE drift over alpha in (1, 6]
        // here is 1.39e-07, so 1e-6 is ~7x headroom.
        const float unpremult[3] = {0.3f, 0.77f, 0.999f};
        for (float a : {1.0000001f, 1.3f, 1.7f, 2.9f, 5.5f}) {
            float color[3] = {a * unpremult[0], a * unpremult[1], a * unpremult[2]};
            float alpha = a;
            saturateBucketPixel(color, &alpha, 3, 1);

            CHECK(alpha == 1.0f);   // assigned exactly 1, never a * (1/a)
            for (int c = 0; c < 3; ++c) {
                // alpha is now exactly 1, so the premultiplied colour IS the
                // ratio; it must equal the original unpremultiplied value.
                CHECK(std::fabs(color[c] - unpremult[c]) <= 1e-6f * unpremult[c]);
            }
        }
    }

    SUBCASE("a coverage deficit (alpha < 1) is left strictly alone by this pass -- never scaled up here")
    {
        float color[3] = {0.15f, 0.10f, 0.05f};
        float alpha = 0.5f;
        saturateBucketPixel(color, &alpha, 3, 1);

        CHECK(alpha == 0.5f);
        CHECK(color[0] == 0.15f);
        CHECK(color[1] == 0.10f);
        CHECK(color[2] == 0.05f);
    }

    SUBCASE("alpha == 1 exactly: no-op")
    {
        float color[1] = {0.5f};
        float alpha = 1.0f;
        saturateBucketPixel(color, &alpha, 1, 1);
        CHECK(alpha == 1.0f);
        CHECK(color[0] == 0.5f);
    }

    SUBCASE("NaN alpha fails the >1 test and is left untouched")
    {
        float color[1] = {0.3f};
        float alpha = std::numeric_limits<float>::quiet_NaN();
        saturateBucketPixel(color, &alpha, 1, 1);
        CHECK(std::isnan(alpha));
        CHECK(color[0] == 0.3f);
    }
}

TEST_CASE("saturateBucketPlanes: the whole-band driver indexes the documented plane layout")
{
    // saturateBucketPixel() above is the per-pixel primitive; this is the
    // driver the scatter core actually calls, and its bucket/channel/pixel
    // index arithmetic is untested by the per-pixel cases (a mutation passing
    // the wrong channel stride down survives all of them). Layout, per
    // DeepCDefocusMath.h's "Bucket plane layout" comment:
    //     color[(k * channelCount + c) * pixelCount + i]
    //     alpha[k * pixelCount + i]
    const int bucketCount = 3;
    const int channelCount = 2;
    const std::ptrdiff_t pixelCount = 4;

    std::vector<float> color(static_cast<std::size_t>(bucketCount * channelCount) * 4);
    std::vector<float> alpha(static_cast<std::size_t>(bucketCount) * 4);

    auto colorAt = [&](int k, int c, int i) -> float& {
        return color[static_cast<std::size_t>((k * channelCount + c) * pixelCount + i)];
    };
    auto alphaAt = [&](int k, int i) -> float& {
        return alpha[static_cast<std::size_t>(k * pixelCount + i)];
    };

    // Distinct alpha per (bucket, pixel): pixel 0 and 2 oversaturate, 1 and 3
    // don't -- so a driver that read the wrong pixel would saturate the wrong
    // entries. Unpremultiplied colour is distinct per channel too.
    const float alphaIn[3][4] = {{2.0f, 0.5f, 1.6f, 1.0f},
                                 {1.25f, 0.9f, 3.0f, 0.25f},
                                 {4.0f, 0.1f, 1.1f, 0.75f}};
    const float unpremult[2] = {0.4f, 0.85f};

    for (int k = 0; k < bucketCount; ++k)
        for (int i = 0; i < pixelCount; ++i) {
            alphaAt(k, static_cast<int>(i)) = alphaIn[k][i];
            for (int c = 0; c < channelCount; ++c)
                colorAt(k, c, static_cast<int>(i)) = alphaIn[k][i] * unpremult[c];
        }

    saturateBucketPlanes(color.data(), alpha.data(), bucketCount, channelCount, pixelCount);

    for (int k = 0; k < bucketCount; ++k) {
        for (int i = 0; i < pixelCount; ++i) {
            const float in = alphaIn[k][i];
            if (in > 1.0f) {
                CHECK(alphaAt(k, static_cast<int>(i)) == 1.0f);
                for (int c = 0; c < channelCount; ++c)
                    CHECK(std::fabs(colorAt(k, c, static_cast<int>(i)) - unpremult[c])
                          <= 1e-6f * unpremult[c]);
            } else {
                // Never scaled up here: a deficit is the composite's fill to restore.
                CHECK(alphaAt(k, static_cast<int>(i)) == in);
                for (int c = 0; c < channelCount; ++c)
                    CHECK(colorAt(k, c, static_cast<int>(i)) == in * unpremult[c]);
            }
        }
    }
}

// ===========================================================================
// Volumetric span split
// ===========================================================================

TEST_CASE("splitSpanAtBoundaries: parts recombine via `over` to reproduce the parent sample exactly")
{
    DepthBuckets buckets;
    buckets._bucketCount = 4;
    buckets._boundaries[0] = 0.0f;
    buckets._boundaries[1] = 10.0f;
    buckets._boundaries[2] = 20.0f;
    buckets._boundaries[3] = 30.0f;
    buckets._boundaries[4] = 40.0f;
    buckets._centres[0] = 5.0f;
    buckets._centres[1] = 15.0f;
    buckets._centres[2] = 25.0f;
    buckets._centres[3] = 35.0f;

    SUBCASE("span crossing no boundary returns 1 part identical to the input")
    {
        SpanSplitPart parts[6];
        const int n = splitSpanAtBoundaries(buckets, 12.0f, 18.0f, 0.7f, parts, 6);
        REQUIRE(n == 1);
        CHECK(parts[0].zFront == 12.0f);
        CHECK(parts[0].zBack == 18.0f);
        CHECK(parts[0].t == doctest::Approx(1.0f));
        CHECK(parts[0].alpha == doctest::Approx(0.7f));
    }

    SUBCASE("span crossing multiple boundaries: parts' t sum to 1 and alphas recombine to the parent")
    {
        const float parentAlpha = 0.85f;
        SpanSplitPart parts[6];
        const int n = splitSpanAtBoundaries(buckets, 3.0f, 33.0f, parentAlpha, parts, 6);
        REQUIRE(n >= 2);

        float tSum = 0.0f;
        float transmittance = 1.0f; // product over parts of (1 - alpha_i)
        for (int i = 0; i < n; ++i) {
            tSum += parts[i].t;
            transmittance *= (1.0f - parts[i].alpha);
        }
        // Absolute tolerances tied to measurement, not to what the code emits:
        // swept over alpha in (0,1) x 200 span placements the worst |sum t - 1|
        // is 3.0e-08 and the worst |reconstructed - parent| is 1.2e-07
        // (consistent with the ~8.3e-08 measured for this form), so 1e-6 is
        // ~8x headroom.
        CHECK(std::fabs(tSum - 1.0f) < 1e-6f);
        const float reconstructedAlpha = 1.0f - transmittance;
        CHECK(std::fabs(reconstructedAlpha - parentAlpha) < 1e-6f);
    }
}

TEST_CASE("splitPartCount is a safe upper bound, and bucketOfContaining's contract "
          "is {containing bucket, frac 0}")
{
    DepthBuckets buckets;
    buckets._bucketCount = 4;
    buckets._boundaries[0] = 0.0f;
    buckets._boundaries[1] = 10.0f;
    buckets._boundaries[2] = 20.0f;
    buckets._boundaries[3] = 30.0f;
    buckets._boundaries[4] = 40.0f;
    buckets._centres[0] = 5.0f;
    buckets._centres[1] = 15.0f;
    buckets._centres[2] = 25.0f;
    buckets._centres[3] = 35.0f;

    SUBCASE("bucketOfContaining returns the whole weight in the containing bucket")
    {
        // The direct statement of the contract the composition tests below rely
        // on indirectly: no fractional spill, and the index is the bucket whose
        // [boundary(i), boundary(i+1)) interval contains the depth.
        const struct { float depth; int index; } cases[] = {
            {  1.0f, 0 }, {  9.99f, 0 }, { 10.0f, 1 }, { 15.0f, 1 },
            { 25.0f, 2 }, { 39.9f, 3 },
        };
        for (const auto& c : cases) {
            const BucketWeight w = buckets.bucketOfContaining(c.depth);
            CHECK(w.index == c.index);
            CHECK(w.frac == 0.0f);
            CHECK(w.weightLow() == 1.0f);
            CHECK(w.weightHigh() == 0.0f);
            // frac == 0 folds the "second" bucket back onto the first, so an
            // unconditional two-deposit scatter loop can never write past K-1.
            CHECK(w.indexHigh() == c.index);
        }

        // Out of range clamps onto the end buckets, never out of bounds.
        CHECK(buckets.bucketOfContaining(-1000.0f).index == 0);
        CHECK(buckets.bucketOfContaining(1000.0f).index == buckets.bucketCount() - 1);
    }

    SUBCASE("splitPartCount never under-counts the parts splitSpanAtBoundaries writes")
    {
        // The buffer-sizing contract: an under-count would be a stack overrun
        // in the scatter core's per-sample path.
        for (float zFront = -5.0f; zFront <= 45.0f; zFront += 1.0f) {
            for (float thickness : {0.0f, 0.5f, 7.0f, 15.0f, 33.0f, 60.0f}) {
                const int bound = splitPartCount(buckets, zFront, zFront + thickness);
                CHECK(bound >= 1);
                CHECK(bound <= buckets.boundaryCount() + 1); // documented K+2 cap

                SpanSplitPart parts[DepthBuckets::kMaxBoundaries + 1];
                const int n = splitSpanAtBoundaries(buckets, zFront, zFront + thickness,
                                                    0.6f, parts,
                                                    DepthBuckets::kMaxBoundaries + 1);
                CHECK(n <= bound);
            }
        }
    }
}

TEST_CASE("Composition contract: bucketOfContaining (correct) exactly reconstructs a split span; "
          "bucketOf (contract violation) double-counts")
{
    // Volumetric span splitting and fractional bucket assignment are mutually
    // exclusive: a piece from splitSpanAtBoundaries() MUST be deposited via
    // bucketOfContaining(), not bucketOf() -- using bucketOf() re-splits an
    // already-split piece and over-counts alpha/colour (measured +8.3% at
    // parent alpha 0.9 on a 16-bucket frame).
    //
    // Hand-verifiable deterministic scenario (bucketCount=4, evenly spaced so
    // the numbers can be checked by hand):
    //   boundaries = {0, 10, 20, 30, 40}, centres = {5, 15, 25, 35}
    //   span [8, 12] straddles exactly boundary[1]=10, parent alpha = 0.9
    //   -> two parts, t0=t1=0.5, each alpha = 1 - sqrt(1-0.9) = 1 - sqrt(0.1)
    //                                        = 0.683772...
    //
    // Correct (bucketOfContaining): part0 (mid=9, in bucket [0,10)) deposits
    // WHOLLY into bucket 0; part1 (mid=11, in bucket [10,20)) deposits WHOLLY
    // into bucket 1. Composite: 1 - (1-a)(1-a) = 1 - (sqrt(0.1))^2 = 1 - 0.1
    // = 0.9 exactly == parent alpha.
    //
    // Violating (bucketOf): part0's mid=9 sits between CENTRES 5 and 15
    // (frac=0.4), so bucketOf() splits IT again 60/40 across buckets 0/1;
    // part1's mid=11 is also between centres 5 and 15 (frac=0.6), splitting
    // 40/60 across the SAME two buckets. The two parts' contributions land in
    // the same bucket pair and accumulate ADDITIVELY there (as the scatter's
    // per-bucket accumulation does), then get `over`-composited on top of
    // that -- double-counting. Hand computation gives ~0.9825, i.e. ~9.2%
    // high, consistent with the measured ballpark for a similar (not
    // identical) geometry.
    DepthBuckets buckets;
    buckets._bucketCount = 4;
    buckets._boundaries[0] = 0.0f;
    buckets._boundaries[1] = 10.0f;
    buckets._boundaries[2] = 20.0f;
    buckets._boundaries[3] = 30.0f;
    buckets._boundaries[4] = 40.0f;
    buckets._centres[0] = 5.0f;
    buckets._centres[1] = 15.0f;
    buckets._centres[2] = 25.0f;
    buckets._centres[3] = 35.0f;

    const float parentAlpha = 0.9f;
    SpanSplitPart parts[6];
    const int n = splitSpanAtBoundaries(buckets, 8.0f, 12.0f, parentAlpha, parts, 6);
    REQUIRE(n == 2);

    auto composite = [&](bool useContract) -> float {
        std::vector<float> bucketAlpha(static_cast<std::size_t>(buckets.bucketCount()), 0.0f);
        for (int i = 0; i < n; ++i) {
            const float mid = 0.5f * (parts[i].zFront + parts[i].zBack);
            const BucketWeight w = useContract ? buckets.bucketOfContaining(mid)
                                                : buckets.bucketOf(mid);
            const BucketDeposit dep = fragmentDeposit(w, parts[i].alpha);
            bucketAlpha[static_cast<std::size_t>(dep.index0)] += dep.alpha0;
            bucketAlpha[static_cast<std::size_t>(dep.index1)] += dep.alpha1;
        }
        return overCompositeAlpha(bucketAlpha.data(), buckets.bucketCount());
    };

    const float compliant = composite(/*useContract*/ true);
    const float violating  = composite(/*useContract*/ false);

    // Measured reconstruction error on the compliant path is 1.2e-07, so the
    // absolute 1e-6 bound below is ~8x headroom -- tight enough that the
    // violating path's +9.2% could never slip through it.
    CHECK(std::fabs(compliant - parentAlpha) < 1e-6f);
    // Regression guard on the violating path. The 1.05 threshold sits well
    // below the +9.17% this exact scenario produces, so the test is robust to
    // minor float-path differences while still catching a reintroduced contract
    // violation (it does: making bucketOfContaining() behave like bucketOf()
    // fails both this and the `compliant` assertion above -- verified by
    // mutation).
    CHECK(violating > parentAlpha * 1.05f);

    // ...and pin the actual value, so this stays a HAND-DERIVED scenario rather
    // than a threshold the rig could drift under. Closed form, all by hand:
    //   a_part      = 1 - sqrt(0.1)          = 0.6837722339831620
    //   1 - a_part  = sqrt(0.1)              = 0.31622776601683794
    //   deposits    = 1 - (1-a_part)^0.6     = 0.4988127663727278
    //                 1 - (1-a_part)^0.4     = 0.3690426555198068
    //   each bucket = 0.4988127663727278 + 0.3690426555198068
    //               = 0.8678554218925346    (both parts land in the SAME pair)
    //   composite   = 1 - (1 - 0.8678554218925346)^2 = 0.9825378043...
    // i.e. +9.171% over the parent 0.9. The +8.29% measured elsewhere is a
    // DIFFERENT scenario (a 16-bucket ΔCoC frame with non-uniform spacing, so
    // the two parts' centre-fractions are not the symmetric 0.4/0.6 this
    // hand-checkable 4-bucket uniform rig produces); the two numbers are
    // consistent in sign and magnitude, and neither is a fitted constant.
    CHECK(violating == doctest::Approx(0.9825378043).epsilon(1e-4));
}

TEST_CASE("Composition contract: bucketOfContaining reconstruction stays continuous as a "
          "volumetric slab slides across a bucket boundary")
{
    // Under the correct contract the result stays continuous (max alpha step
    // 6e-08) as a slab slides across a boundary.
    // Sweep a fixed-thickness span's front edge across boundary[1]=10 and
    // check the compliant reconstruction stays pinned to the parent alpha at
    // every step (which is itself the continuity guarantee: if it always
    // equals the same constant, consecutive steps trivially differ by ~0).
    DepthBuckets buckets;
    buckets._bucketCount = 4;
    buckets._boundaries[0] = 0.0f;
    buckets._boundaries[1] = 10.0f;
    buckets._boundaries[2] = 20.0f;
    buckets._boundaries[3] = 30.0f;
    buckets._boundaries[4] = 40.0f;
    buckets._centres[0] = 5.0f;
    buckets._centres[1] = 15.0f;
    buckets._centres[2] = 25.0f;
    buckets._centres[3] = 35.0f;

    const float parentAlpha = 0.9f;
    const float thickness = 4.0f;

    float prev = -1.0f;
    bool havePrev = false;
    for (float zFront = 8.0f; zFront <= 12.0f; zFront += 0.2f) {
        const float zBack = zFront + thickness;
        SpanSplitPart parts[6];
        const int n = splitSpanAtBoundaries(buckets, zFront, zBack, parentAlpha, parts, 6);
        REQUIRE(n >= 1);
        REQUIRE(n <= 2); // this span crosses at most one boundary (10)

        std::vector<float> bucketAlpha(static_cast<std::size_t>(buckets.bucketCount()), 0.0f);
        for (int i = 0; i < n; ++i) {
            const float mid = 0.5f * (parts[i].zFront + parts[i].zBack);
            const BucketWeight w = buckets.bucketOfContaining(mid);
            const BucketDeposit dep = fragmentDeposit(w, parts[i].alpha);
            bucketAlpha[static_cast<std::size_t>(dep.index0)] += dep.alpha0;
            bucketAlpha[static_cast<std::size_t>(dep.index1)] += dep.alpha1;
        }
        const float outAlpha = overCompositeAlpha(bucketAlpha.data(),
                                                  buckets.bucketCount());

        // Absolute bounds tied to the measured "max alpha step 6e-08": over a
        // 5x finer sweep of this rig the worst deviation from the parent is
        // 1.19e-07 and the worst consecutive step is 1.19e-07, so 1e-6 is ~8x
        // headroom. A 1e-4 / 1e-3 bound would be three to four orders of
        // magnitude looser than that measurement and would pass a visible
        // discontinuity.
        CHECK(std::fabs(outAlpha - parentAlpha) < 1e-6f);
        if (havePrev)
            CHECK(std::fabs(outAlpha - prev) < 1e-6f); // no discontinuity crossing the boundary
        prev = outAlpha;
        havePrev = true;
    }
}

