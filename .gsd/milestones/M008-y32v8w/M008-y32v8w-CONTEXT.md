# M008-y32v8w: DeepCDefocusPORay Path Tracer

**Gathered:** 2026-03-25
**Status:** Ready for planning

## Project Description

Replace DeepCDefocusPORay's current scatter engine (a copy of DeepCDefocusPOThin's thin-lens CoC scatter with unused polynomial extras bolted on) with a real lentil-style polynomial optics path tracer. The node should mimic lentil's camera shader: for each output pixel, trace rays through the polynomial lens system by Newton-iterating to match aperture samples, forward-evaluating the polynomial, converting to 3D rays via `sphereToCs`, then reading the deep image at the pixel the ray sees.

Also fix the `_validate` format propagation bug affecting both Thin and Ray nodes (output frame size doesn't match Deep input).

## Why This Milestone

DeepCDefocusPORay currently computes `sphereToCs` and throws the result away — the final pixel landing uses a thin-lens CoC warp identical to the Thin variant. This defeats the purpose of having a separate "raytraced" node. The user wants Ray to actually trace rays through the polynomial lens, matching lentil's camera shader architecture.

## User-Visible Outcome

### When this milestone is complete, the user can:

- Load DeepCDefocusPORay in Nuke, point it at a Deep image and Angenieux 55mm .fit files, and get physically correct defocused output where the bokeh shape comes from actual ray tracing through the polynomial lens system
- See correct output frame size on both DeepCDefocusPOThin and DeepCDefocusPORay (matching the Deep input's format)
- Run `nuke -x test/test_ray.nk` and get non-black output at 128×72

### Entry point / environment

- Entry point: Nuke node — load DeepCDefocusPORay, connect Deep input, set .fit file paths
- Environment: Nuke 16+ on Linux (docker build for .so, local symlink for dev iteration)
- Live dependencies: Nuke NDK, lentil .fit files on disk

## Completion Class

- Contract complete means: syntax check passes, structural grep contracts pass, test .nk scripts exist
- Integration complete means: docker build produces both .so files, `nuke -x` test scripts exit 0
- Operational complete means: none (plugin, not a service)

## Final Integrated Acceptance

To call this milestone complete, we must prove:

- `nuke -x test/test_ray.nk` exits 0 with non-black 128×72 EXR output (the ray actually traced through the lens and produced defocus)
- `nuke -x test/test_thin.nk` still exits 0 (no regression on Thin)
- Both nodes output correct frame size matching the Deep input format

## Risks and Unknowns

- **Newton convergence with finite-difference Jacobians** — lentil uses analytically generated per-lens Jacobians for the aperture match Newton iteration. We use finite-difference Jacobians with the generic `.fit` evaluator. FD Jacobians are slower and potentially less stable at extreme field angles. Mitigated by the vignetting retry loop (R041).
- **Coordinate convention alignment** — lentil uses physical mm throughout (sensor_width, aperture_radius, pupil radii). Getting the mm ↔ pixel mapping exactly right is critical. Any sign or scale error produces black output or nonsensical scatter.
- **sensor_shift computation** — lentil computes this via a logarithmic search that traces center rays. We need to replicate this using `poly_system_evaluate`. If the search doesn't converge for a given lens, the entire path trace produces unfocused output.

## Existing Codebase / Prior Art

- `src/DeepCDefocusPORay.cpp` — current scatter engine (to be replaced). Knobs, _validate, poly loading, holdout fetch are reusable infrastructure.
- `src/DeepCDefocusPOThin.cpp` — Thin variant (not changed in this milestone except _validate format fix). Reference for the working scatter pattern.
- `src/deepc_po_math.h` — shared math primitives. `sphereToCs` needs upgrading to full lentil version. `lt_newton_trace` exists but solves a different problem (sensor position, not direction). New `pt_sample_aperture` function needed.
- `src/poly.h` — `poly_system_evaluate` with `max_degree` early-exit. Used as the generic polynomial evaluator for all Newton iterations.
- `/home/latuser/git/lentil/pota/src/lentil.h` — **reference implementation**. `trace_ray_fw_po` (lines 283–393) is the complete forward path-trace flow. `lens_pt_sample_aperture` (line 1272) is the Newton aperture sampler. `logarithmic_focus_search` computes sensor_shift.
- `/home/latuser/git/lentil/pota/src/lens.h` — **reference `sphereToCs`** (full version with tangent-frame construction, origin + direction output).
- `/home/latuser/git/lentil/polynomial-optics/database/lenses/1953-angenieux-double-gauss/55/code/lens_constants.h` — Angenieux 55mm constants for knob defaults.
- `/home/latuser/git/lentil/polynomial-optics/database/lenses/1953-angenieux-double-gauss/55/code/pt_sample_aperture.h` — per-lens Newton iteration with analytic Jacobians (reference for the FD version we'll build).

> See `.gsd/DECISIONS.md` for all architectural and pattern decisions — it is an append-only register; read it during planning, append to it during execution.

## Relevant Requirements

- R037 — Path-trace renderStripe (core deliverable)
- R038 — Physical mm sensor coordinates
- R039 — Newton aperture sampler
- R040 — Full sphereToCs upgrade
- R041 — Vignetting retry loop
- R042 — sensor_shift via logarithmic search
- R043 — Additional lens constant knobs
- R044 — Per-ray deep flatten with holdout
- R045 — Fix _validate format propagation

## Scope

### In Scope

- Replace DeepCDefocusPORay renderStripe with lentil-style path-trace engine
- Full sphereToCs (position + direction → 3D origin + direction)
- Newton aperture sampler with FD Jacobians (pt_sample_aperture equivalent)
- sensor_shift logarithmic search for focus distance
- Additional lens constant knobs (sensor_width, back_focal_length, outer/inner pupil radii, aperture_pos)
- Physical mm coordinate convention for Ray
- Vignetting retry loop
- Per-ray deep column flatten with per-sample holdout
- Fix _validate format propagation on both Thin and Ray
- CA wavelength tracing preserved (per-channel polynomial evaluation)
- Halton+Shirley aperture sampling preserved
- max_degree truncation preserved

### Out of Scope / Non-Goals

- Changes to DeepCDefocusPOThin's scatter engine (stays as-is, only _validate fix)
- Polygonal aperture / bokeh image (R027, deferred)
- Bidirectional path tracing (R029, out of scope)
- Performance optimization beyond what's needed for 128×72 test resolution
- Auto-parsing lens constants from lentil's JSON files

## Technical Constraints

- No Eigen dependency (D023) — hand-rolled matrix math in deepc_po_math.h
- PlanarIop base class — single-threaded renderStripe, no shared buffer races (D027)
- Polynomial evaluated via generic `poly_system_evaluate` with `.fit` files, not per-lens generated code
- Mock headers in `verify-s01-syntax.sh` must be extended for any new DDImage API usage

## Integration Points

- `poly_system_evaluate` in `poly.h` — all polynomial evaluations go through this. `max_degree` truncation applies.
- `deepc_po_math.h` — new functions (pt_sample_aperture, logarithmic_focus_search, upgraded sphereToCs) live here
- `verify-s01-syntax.sh` — mock headers may need extension if new DDImage methods are called
- `test/test_ray.nk` — test script may need knob updates (new knob names/values)

## Open Questions

- **Aperture radius units in the Newton iteration** — lentil's `aperture_radius` is computed from fstop at runtime. Need to verify: does the aperture polynomial expect physical mm or normalised units for the target aperture position? Probably physical mm (matching the housing_radius), but must verify against lentil's code path.
- **sensor_shift search range** — lentil uses `logarithmic_values()` which returns a range from 0 to 45mm. This may need adjustment for extreme focal lengths. The Angenieux 55mm back_focal_length is ~30.8mm.
