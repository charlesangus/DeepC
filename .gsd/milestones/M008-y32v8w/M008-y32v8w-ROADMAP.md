# M008-y32v8w: DeepCDefocusPORay Path Tracer

**Vision:** Replace DeepCDefocusPORay's scatter engine with a real lentil-style polynomial optics path tracer — trace physical rays through the lens system for each output pixel, producing correct defocused output from Deep input. Fix _validate format bug on both PO nodes.

## Success Criteria

- DeepCDefocusPORay traces rays through the polynomial lens (Newton aperture match → forward eval → sphereToCs → ray direction → input pixel) and produces visibly defocused output
- Both DeepCDefocusPOThin and DeepCDefocusPORay output correct frame size matching Deep input format
- `nuke -x test/test_ray.nk` exits 0 with non-black 128×72 EXR
- `nuke -x test/test_thin.nk` still exits 0 (no regression)
- Vignetting retry loop keeps field-edge pixels from going dark

## Key Risks / Unknowns

- **Newton convergence with FD Jacobians** — finite-difference Jacobians are slower and potentially less stable than lentil's analytic per-lens Jacobians. Mitigated by vignetting retry loop and dampened step (0.72 factor from lentil).
- **Coordinate convention alignment** — mm coordinates must match lentil's convention exactly. sensor_width, aperture_radius, pupil radii all in mm. Any scale/sign error produces black or nonsensical output.

## Proof Strategy

- Newton convergence → retire in S02 by producing non-black defocused output at 128×72 with Angenieux 55mm
- Coordinate conventions → retire in S01 by verifying sensor_shift search finds a reasonable focus distance for Angenieux 55mm defaults

## Verification Classes

- Contract verification: syntax check (verify-s01-syntax.sh), structural grep contracts
- Integration verification: docker build, `nuke -x` test scripts exit 0, non-black output
- Operational verification: none
- UAT / human verification: visual comparison — does Ray output look like defocused input with lens-specific aberrations?

## Milestone Definition of Done

This milestone is complete only when all are true:

- DeepCDefocusPORay.so and DeepCDefocusPOThin.so in docker-build release zip
- `nuke -x test/test_ray.nk` exits 0 with non-black output at 128×72
- `nuke -x test/test_thin.nk` exits 0 (no regression)
- Both nodes output correct frame size
- verify-s01-syntax.sh passes for both source files
- Ray's renderStripe uses the path-trace flow (Newton aperture match, forward eval, sphereToCs, ray-to-pixel projection, deep flatten)
- Vignetting retry loop present and functional

## Requirement Coverage

- Covers: R037, R038, R039, R040, R041, R042, R043, R044, R045
- Partially covers: R031 (replaces structural-only validation with real implementation)
- Leaves for later: R027 (polygonal aperture — deferred)
- Orphan risks: none

## Slices

- [ ] **S01: Path-trace infrastructure + _validate format fix** `risk:medium` `depends:[]`
  > After this: both nodes output correct frame size. New lens constant knobs compile. pt_sample_aperture, full sphereToCs, and logarithmic_focus_search are implemented in deepc_po_math.h and pass syntax check. Ray's renderStripe still uses old scatter engine — infrastructure only, no engine swap yet.

- [ ] **S02: Ray path-trace engine** `risk:high` `depends:[S01]`
  > After this: DeepCDefocusPORay traces real polynomial-optics rays for each output pixel. `nuke -x test/test_ray.nk` produces non-black defocused 128×72 EXR. Vignetting retry loop functional. Deep flatten per ray with per-sample holdout. CA wavelength tracing preserved.

## Boundary Map

### S01 → S02

Produces:
- `src/deepc_po_math.h` → `pt_sample_aperture()` (Newton aperture match with FD Jacobians)
- `src/deepc_po_math.h` → `sphereToCs_full()` (position+direction on sphere → 3D origin+direction)
- `src/deepc_po_math.h` → `logarithmic_focus_search()` (sensor_shift for focus distance)
- `src/DeepCDefocusPORay.cpp` → new Float_knobs: `_sensor_width`, `_back_focal_length`, `_outer_pupil_radius`, `_inner_pupil_radius`, `_aperture_pos` with Angenieux 55mm defaults
- `src/DeepCDefocusPORay.cpp` → `_validate` format fix
- `src/DeepCDefocusPOThin.cpp` → `_validate` format fix
- `scripts/verify-s01-syntax.sh` → mock headers extended if needed

Consumes: nothing (first slice)

### S02 → (standalone)

Produces:
- `src/DeepCDefocusPORay.cpp` → complete path-trace renderStripe replacing scatter engine
- `test/test_ray.nk` → updated test script with new knob values if needed

Consumes from S01:
- `pt_sample_aperture()` for Newton aperture matching
- `sphereToCs_full()` for 3D ray conversion
- `logarithmic_focus_search()` for sensor_shift
- New lens constant knob members (`_sensor_width`, `_back_focal_length`, `_outer_pupil_radius`, `_inner_pupil_radius`, `_aperture_pos`)
- Fixed `_validate` format propagation
