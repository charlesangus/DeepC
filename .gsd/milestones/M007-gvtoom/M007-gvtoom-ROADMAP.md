# M007-gvtoom: DeepCDefocusPO Correctness - Thin-Lens + Raytraced Variants

**Vision:** Replace the broken DeepCDefocusPO scatter model with two correct variants — DeepCDefocusPOThin (thin-lens CoC + polynomial aberration modulation) and DeepCDefocusPORay (lentil-style raytraced gather) — both producing correct defocused output from Deep input.

## Success Criteria

- DeepCDefocusPOThin renders a visibly defocused version of a Deep checkerboard input (not a ring, circle, or black image)
- DeepCDefocusPORay renders a visibly defocused version of the same scene using raytraced gather with lens geometry
- max_degree knob visibly affects quality/detail on both nodes
- Both nodes compile via docker-build.sh and appear in Nuke's menu
- Existing holdout, CA wavelength tracing, and Halton aperture sampling work on both

## Key Risks / Unknowns

- **Ray variant Newton convergence** — Finite-difference Jacobians instead of lentil's analytic ones may converge slower or diverge for some lens configurations. Retire in S03 by building the full solver and testing against the Angenieux 55mm.
- **Performance at production resolution** — Even with max_degree truncation, 1280×720 may be slow for the ray variant. Accepted: the 128×72 test resolution is the CI gate; production performance is a future concern.

## Proof Strategy

- Newton convergence risk → retire in S03 by proving the gather solver produces non-black defocused output at 128×72 with the Angenieux 55mm lens
- Scatter math correctness → retire in S02 by proving thin-lens CoC produces visible defocus that moves with depth

## Verification Classes

- Contract verification: syntax check (verify-s01-syntax.sh), docker build, `nuke -x` test scripts exit 0
- Integration verification: both nodes load in Nuke, accept Deep input, produce flat RGBA
- Operational verification: none
- UAT / human verification: visual comparison of defocused output vs input — does it look defocused, not like an aperture ring?

## Milestone Definition of Done

This milestone is complete only when all are true:

- Both DeepCDefocusPOThin.so and DeepCDefocusPORay.so in the docker-build release zip
- Both test scripts (`nuke -x`) exit 0 with non-black output at 128×72
- max_degree knob present and functional on both
- Old DeepCDefocusPO removed from CMake PLUGINS and FILTER_NODES
- verify-s01-syntax.sh passes for both new source files
- Holdout, CA, Halton sampling preserved on both

## Requirement Coverage

- Covers: R030, R031, R032, R033, R034, R035, R036
- Partially covers: none
- Leaves for later: R027 (polygonal aperture — deferred), R029 (bidirectional — out of scope)
- Orphan risks: none

## Slices

- [x] **S01: Shared infrastructure, max_degree knob, two-plugin scaffold** `risk:low` `depends:[]`
  > After this: both DeepCDefocusPOThin and DeepCDefocusPORay load in Nuke, show all knobs including max_degree, compile via docker-build. No render output yet — renderStripe zeros the output.

- [x] **S02: DeepCDefocusPOThin scatter engine** `risk:medium` `depends:[S01]`
  > After this: DeepCDefocusPOThin renders a defocused checkerboard with visible bokeh. `nuke -x test/test_thin.nk` exits 0 with non-black 128×72 EXR. Thin-lens CoC controls scatter radius; polynomial modulates bokeh shape. Holdout, CA, and Halton sampling functional.

- [ ] **S03: DeepCDefocusPORay gather engine** `risk:high` `depends:[S01]`
  > After this: DeepCDefocusPORay renders the same test scene with physically raytraced bokeh. `nuke -x test/test_ray.nk` exits 0 with non-black 128×72 EXR. Full lentil-style Newton solver with sphereToCs, lens geometry constants, and aperture.fit. Holdout, CA, and Halton sampling functional.

## Boundary Map

### S01 → S02

Produces:
- `src/DeepCDefocusPOThin.cpp` — PlanarIop scaffold with knobs (poly_file, focus_distance, fstop, focal_length, aperture_samples, max_degree), _validate, getRequests, holdout fetch, zero_output lambda, poly loading in renderStripe. renderStripe body is a stub (zeros output).
- `src/deepc_po_math.h` — existing helpers (halton, map_to_disk, coc_radius) unchanged; `sphereToCs` added for S03.
- `src/poly.h` — max_degree parameter already present in poly_system_evaluate.
- CMake: DeepCDefocusPOThin in PLUGINS and FILTER_NODES lists.
- `scripts/verify-s01-syntax.sh` — mock headers extended for both new source files.

Consumes: nothing (first slice)

### S01 → S03

Produces:
- `src/DeepCDefocusPORay.cpp` — PlanarIop scaffold with knobs (poly_file, aperture_file, focus_distance, fstop, focal_length, aperture_samples, max_degree, lens geometry constants). renderStripe body is a stub.
- `src/deepc_po_math.h` — `sphereToCs` function ported from lentil for converting poly output to 3D rays.
- CMake: DeepCDefocusPORay in PLUGINS and FILTER_NODES lists.

Consumes: nothing (first slice)

### S02 → (standalone)

Produces:
- Working thin-lens scatter engine in DeepCDefocusPOThin.cpp renderStripe
- `test/test_thin.nk` — test script

Consumes from S01:
- DeepCDefocusPOThin.cpp scaffold (knobs, _validate, poly loading, holdout)
- deepc_po_math.h (coc_radius, halton, map_to_disk)
- poly.h (poly_system_evaluate with max_degree)

### S03 → (standalone)

Produces:
- Working raytraced gather engine in DeepCDefocusPORay.cpp renderStripe
- `test/test_ray.nk` — test script

Consumes from S01:
- DeepCDefocusPORay.cpp scaffold (knobs, _validate, poly loading, holdout, lens geometry)
- deepc_po_math.h (sphereToCs, halton, map_to_disk)
- poly.h (poly_system_evaluate with max_degree)
