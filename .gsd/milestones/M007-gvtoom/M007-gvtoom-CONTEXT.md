# M007-gvtoom: DeepCDefocusPO Correctness — Thin-Lens + Raytraced Variants

**Gathered:** 2026-03-24
**Status:** Ready for planning

## Project Description

The current DeepCDefocusPO produces incorrect output — it renders the aperture/pupil shape instead of a defocused image. The root cause: the lentil polynomial maps sensor-plane rays to exit-pupil-surface rays in spherical coordinates, but the code was using the raw forward output as pixel scatter positions. The polynomial's input[2:3] are ray **directions** (not aperture positions), and output[2:3] are exit ray **directions** on the outer pupil (not scattered sensor positions).

This milestone replaces the broken single plugin with two correct variants:

- **DeepCDefocusPOThin** — Thin-lens CoC gives the scatter radius from depth. The polynomial modulates where within that CoC each aperture sample lands, adding physically-motivated aberrations (cat-eye vignetting, coma, astigmatism, CA). Fast, practical.

- **DeepCDefocusPORay** — Full lentil-style raytraced gather. Treats the Deep image as a 3D scene. Per output pixel, casts rays from the sensor through aperture points, evaluates the polynomial to get exit rays on the outer pupil, converts via `sphereToCs` to 3D Cartesian rays, and intersects with deep samples at their depth. Physically exact, slower, requires lens geometry constants.

Both also get a `max_degree` Int_knob for quality/speed control — the polynomial has 4368 terms at degree 11 but only 56 at degree 3.

## Why This Milestone

The M006 DeepCDefocusPO scaffold is complete (holdout, CA, knobs, .fit loading all work) but produces incorrect visual output. This milestone fixes the core scatter/gather math so the plugin actually defocuses images.

## User-Visible Outcome

### When this milestone is complete, the user can:

- Create a DeepCDefocusPOThin node, connect a Deep input, and see a correctly defocused flat image with thin-lens bokeh modulated by polynomial aberrations
- Create a DeepCDefocusPORay node, connect a Deep input with lens geometry constants, and see a physically raytraced defocused image matching lentil's approach
- Adjust `max_degree` on either node to trade quality for speed

### Entry point / environment

- Entry point: Nuke node menu → Deep/Filter → DeepCDefocusPOThin or DeepCDefocusPORay
- Environment: Nuke 16.0v6 on Linux
- Live dependencies involved: none (all computation is local)

## Completion Class

- Contract complete means: both plugins compile, produce non-black correctly-defocused output from a Deep checkerboard test, `nuke -x` exits 0
- Integration complete means: both nodes appear in Nuke's menu, accept Deep input, produce flat RGBA
- Operational complete means: none (single-shot render, no service lifecycle)

## Final Integrated Acceptance

To call this milestone complete, we must prove:

- DeepCDefocusPOThin renders a visibly defocused version of a Deep checkerboard (not a ring/circle/black)
- DeepCDefocusPORay renders a visibly defocused version of the same scene using raytraced gather
- max_degree knob changes the output quality/detail on both nodes
- `docker-build.sh --linux --versions 16.0` produces both .so files in the release zip

## Risks and Unknowns

- **Ray variant Newton convergence** — The lentil Newton solver uses lens-specific generated code with analytic Jacobians. Our generic `poly_system_evaluate` uses finite-difference Jacobians, which may converge slower or not at all for some lens configurations. Risk: medium.
- **Ray variant performance** — Newton iteration with finite-difference Jacobians requires 3× poly evaluations per iteration (function + 2 FD). Even with degree truncation, this could be slow. Risk: medium.
- **Lens geometry constants availability** — The ray variant needs `outer_pupil_curvature_radius`, `lens_length`, `aperture_housing_radius` etc. These are in `lens_constants.h` (generated code) and `lenses.json`, not in the `.fit` file itself. Exposing as knobs is the pragmatic approach. Risk: low.
- **`sphereToCs` correctness** — Converting from polynomial spherical coords to Cartesian 3D rays requires a correct port of lentil's `sphereToCs`. Getting the sign conventions or coordinate frame wrong would produce subtly incorrect output. Risk: low-medium.

## Existing Codebase / Prior Art

- `src/DeepCDefocusPO.cpp` — Current broken implementation. All the non-scatter code (knobs, _validate, holdout, zero_output, poly loading, format propagation) is correct and reusable.
- `src/poly.h` — Polynomial reader/evaluator with max_degree support (added in Q7). MIT vendored from lentil.
- `src/deepc_po_math.h` — Halton, Shirley disk, CoC radius, Newton trace, Mat2 inverse. The `lt_newton_trace` function is wrong for scatter but may be adaptable for the ray variant's gather iteration.
- `lentil/polynomial-optics/src/raytrace.h` — Contains `sphereToCs` and `csToSphere` functions we need to port for the ray variant.
- `lentil/polynomial-optics/src/gencode.h` — `print_lt_sample_aperture` shows the full Newton solver structure: dual residual (aperture + scene direction), dual Jacobian inversion, damped updates.
- `lentil/polynomial-optics/database/lenses/1953-angenieux-double-gauss/55/code/lens_constants.h` — Lens geometry constants for the test lens.
- `lentil/polynomial-optics/database/lenses/1953-angenieux-double-gauss/55/fitted/exitpupil.fit` — Exit pupil polynomial (524KB, 5 polys × 4368 terms).
- `lentil/polynomial-optics/database/lenses/1953-angenieux-double-gauss/55/fitted/aperture.fit` — Aperture polynomial (524KB) needed for ray variant.
- `lentil/polynomial-optics/database/lenses.json` — Full lens database with geometry constants.

> See `.gsd/DECISIONS.md` for all architectural and pattern decisions — it is an append-only register; read it during planning, append to it during execution.

## Relevant Requirements

- R030 — DeepCDefocusPOThin: thin-lens CoC scatter with poly aberration modulation
- R031 — DeepCDefocusPORay: lentil-style raytraced gather
- R032 — max_degree Int_knob on both nodes
- R033 — Lens geometry constants on Ray variant
- R034 — aperture.fit loading for Ray variant
- R035 — Holdout, CA, Halton sampling preserved on both
- R036 — Both nodes registered in Nuke menu

## Scope

### In Scope

- Two new .cpp files: `DeepCDefocusPOThin.cpp` and `DeepCDefocusPORay.cpp`
- Correct scatter math (thin-lens) and gather math (raytraced)
- `max_degree` knob on both
- `sphereToCs` port for ray variant
- Lens geometry knobs for ray variant
- aperture.fit loading for ray variant
- CMake registration of both new plugins, removal of old DeepCDefocusPO
- Updated verify-s01-syntax.sh mock headers
- Test .nk scripts for both variants

### Out of Scope / Non-Goals

- Bidirectional path tracing (R029 — out of scope)
- Polygonal/texture aperture blades (R027 — deferred)
- Auto-parsing lens constants from JSON database (future enhancement — knobs for now)
- Performance optimisation beyond max_degree (GPU, SIMD, threading)

## Technical Constraints

- PlanarIop base class (single-threaded renderStripe) — load-bearing for scatter write correctness
- poly.h included in exactly ONE translation unit per plugin — ODR firewall
- `ImagePlane::writableAt(x, y, chanNo(channel))` — channel INDEX not enum (Q4-Q6 root cause)
- Poly loading in renderStripe, not _validate — thread safety (Q6 root cause)
- Mock headers in verify-s01-syntax.sh must be extended for any new DDImage API usage

## Integration Points

- lentil polynomial-optics database — .fit files and lens geometry constants
- Nuke DDImage SDK — PlanarIop, DeepOp, ImagePlane, Knobs
- Docker build system — docker-build.sh for release .so compilation

## Open Questions

- **Thin variant: how to map poly output to CoC modulation?** Current thinking: evaluate poly at (sensor, direction_toward_aperture_sample, lambda), use the output direction perturbation as an offset within the CoC disk. The perturbation encodes the lens aberration at that aperture+field position.
- **Ray variant: is the dual-residual Newton solver (aperture + scene direction) stable with finite-difference Jacobians?** lentil uses analytic Jacobians; FD Jacobians add noise and cost. May need damping factor adjustment (lentil uses 0.72×).
- **Should the old DeepCDefocusPO.cpp be deleted or kept as a reference?** Leaning toward delete — the two new files supersede it completely.
