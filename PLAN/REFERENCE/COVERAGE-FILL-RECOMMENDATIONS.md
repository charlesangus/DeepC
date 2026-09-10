# Recommendations for COVERAGE-FILL-PLAN.md

Execute the plan with the following changes, in this order. The first three are correctness
requirements; the fourth is a correctness requirement for semi-transparent geometry only.

1. **Virtual background over the full fetch window.** The plan currently gives the virtual
   background (the assumed backdrop used to fill missing coverage) a residual transmittance `T`
   of 1 only inside `srcBox`, the deep image's own bounding box. For a deep input whose bounding
   box is tight around its content, every bloom pixel outside that box gets no virtual
   background at all: arrival equals the bloom's own weight, alpha divides to exactly 1, and a
   soft edge becomes a hard disc. Instead, residual `T` should default to 1 for every pixel of
   the fetch window inside the output box, whether or not it lies in `srcBox`. Add a unit test:
   an isolated blurred fragment with a tight bounding box keeps its bloom profile. This replaces
   the plan's "background exists only where the deep image is defined" sentence.

2. **Per-pixel residual radius.** The plan scatters the background residual (the leftover
   transmittance behind a surface, used to fill coverage gaps) at one global radius, while the
   surface itself is scattered at its own circle-of-confusion (CoC) radius. When those radii
   differ, the residual doesn't share the surface's defocus, and a semi-transparent surface's
   alpha comes out wrong by about 1.8 code values (e.g. a true alpha-0.9 surface reads back as
   0.893 instead of 0.900). Fix: for a source pixel that has samples, scatter its residual `T_px`
   at the CoC of that pixel's own deepest sample. Use the global `background_depth` (auto = CoC
   at `depthMax()`) only for pixels with no samples at all. Unit test: alpha-0.9 flat field with
   a forced 0.93 arrival reads 0.900 to 1e-6.

3. **Tolerance on the deficit test.** The plan's fill only divides by arrival `D` when there's a
   deficit (`D < 1`); at exact size-0 (no blur), floating-point sums can land one ulp below 1 and
   spuriously trigger the division, moving pixels that should be untouched by a single ulp. Fix:
   divide only when `D < 1 - 1e-5`. Unit test: a sharp-path-only pixel with `D = 1 - 1 ulp` is
   left untouched.

4. **Kernel blending for fractional diameters** (previously deferred in the plan as "Option B,
   ~2e-3 scale discretization polish"). The plan's current kernel step is a hard switch between a
   sharp (unblurred) path and a fixed-size disc kernel. This produces an over-read of arrival of
   up to ~5.9% for surfaces near focus, which the plan otherwise has no mechanism to correct.
   Fix: replace the sharp/disc step with a minimum kernel diameter of 1 px, and blend the two
   odd-sized kernels that bracket any fractional diameter `d`. This is the only route to correct
   alpha for semi-transparent surfaces near focus; for opaque geometry the existing alpha clamp
   hides the error, so this fix is optional there. If schedule forces a split, land items 1–3
   first and land this item as the immediate next change, re-pinning the affected over-read test
   cells only as an interim measure.

## Also

- Keep the single, kernel-size-independent arrival plane and deficit-only division as designed.
  Do not add per-bucket arrival planes: the coverage plane also carries holdout visibility and
  follows an area model (one full deposit per parent sample), so a bucket can legitimately hold
  more than unit weight, and per-bucket raw arrival would only fix the narrow same-surface
  surplus case at K times the memory cost.
- Add a holdout commutation test (call it m4): render a scene with and without a 0.5-alpha
  holdout card placed in front of everything, and require the ratio of the two renders under the
  card to hold to float precision at every pixel, including rows the fill changes. This confirms
  the fill's numerator scales linearly with visibility per fragment while its divisor ignores
  visibility, so fill and holdout commute.
- Strengthen the existing interior-alpha test (m0) to assert colour ratio as well as alpha value.
  Near silhouettes, occluded samples can be scattered into the numerator but excluded from the
  denominator, causing alpha to overshoot 1; the existing alpha-above-1 clamp brings it back by
  scaling colour together with alpha, so both need checking.
- Re-word the plan's "blooms over emptiness unchanged" property as conditional rather than an
  invariant: arrival only equals 1 around an isolated object when that object's CoC equals the
  background radius. That holds for a single-depth scene under the auto default, but in a scene
  with far geometry elsewhere, a near object's fringe over emptiness can be boosted where its
  kernel is smaller than the background kernel. The per-pixel residual radius from item 2 does
  not remove this, because truly empty pixels have no sample to take a radius from.
- In the node_help rewrite (plan task T6), state that the fill divides by un-held-out arrival so
  that holdout attenuation is preserved exactly, and that the fill invents foreground-coloured
  coverage only where the renderer wrote none.
