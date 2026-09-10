# Performance notes for COVERAGE-FILL-PLAN.md

These are real costs identified while reviewing the coverage-fill plan. None of them should
influence the correctness recommendations (see `COVERAGE-FILL-RECOMMENDATIONS.md`) — they're
deferred until after correctness is fixed.

- **Virtual-background scatter cost grows with per-pixel residual radii.** The virtual-background
  scatter is a convolution of the residual-transmittance map with one kernel per distinct radius.
  Today that's a single convolution. Once residual radii become per-pixel (recommendation 2 in
  the companion file — scattering each source pixel's residual at its own deepest sample's
  circle-of-confusion radius), it's no longer a single convolution; the naive cost is `pi * r^2`
  per non-opaque source pixel. Measure with `tests/nuke/run_profile.sh` before optimizing.
  Bucketing residuals by kernel-size bin and convolving per bin is the obvious mitigation.
- **Kernel blending roughly doubles per-fragment raster work near integer kernel sizes.** The
  fix that blends two bracketing kernel sizes for fractional diameters (recommendation 4 in the
  companion file) scatters two kernels per fragment instead of one, near integer kernel sizes.
- **Arrival-plane memory is a small addition.** The arrival plane is one float per band pixel. On
  holdout-connected bands the existing lookup table is already K+1 floats per pixel (K = number
  of kernel samples), so the new plane is a small fraction of current memory. Update
  `bytesForBand` and the memory-limit comment accordingly.
- **The extra per-span loop over raw weights is cheap relative to existing work.** It costs less
  than the visibility interpolation that already runs alongside it in the same loop.
