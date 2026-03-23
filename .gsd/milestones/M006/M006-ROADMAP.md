# M006: DeepCDefocusPO - Polynomial Optics Defocus for Deep Images

**Vision:** A new Nuke NDK plugin, DeepCDefocusPO, that accepts a Deep image and produces a flat 2D defocused output using a polynomial optics lens model loaded at runtime from lentil .fit files. Bokeh shape, size, and chromatic aberration match what the real lens would produce. An optional Deep holdout input stencils the defocused output depth-correctly — evaluated per-output-pixel at each sample's depth — so the holdout is never itself defocused, solving the double-defocus problem of pgBokeh and Nuke's Bokeh node.

## Success Criteria

- DeepCDefocusPO node loads a lentil .fit polynomial file at runtime without crash
- Flat 2D output is produced from a Deep input; bokeh is visible and physically sized
- Chromatic aberration is visible on out-of-focus highlights
- Holdout input stencils the defocused result at correct depths; holdout geometry does not appear in output and is not blurred
- `docker-build.sh --linux --versions 16.0` exits 0 with DeepCDefocusPO.so in release zip
- Node appears in FILTER_NODES category in Nuke

## Key Risks / Unknowns

- **Iop/PlanarIop base class for Deep→flat** — first node in DeepC to output flat from Deep input; correct base class and Deep upstream fetch pattern must be confirmed against the SDK before scatter engine can be built
- **Eigen dependency** — lentil's Newton iteration uses Eigen::Matrix2d; must replace with hand-rolled 2×2 inverse to avoid adding Eigen to the build
- **Scatter vs gather threading** — stochastic forward scatter into a shared output buffer requires thread-safe accumulation; gather model (per output pixel, pull from Deep upstream) avoids this but requires a different architecture decision

## Proof Strategy

- Iop/PlanarIop + Deep fetch pattern → retire in S01 by producing a node that compiles, loads a .fit file, and outputs flat black without crash
- Scatter/gather threading model → retire in S01 by deciding and implementing the architecture; confirmed in S02 when output is non-zero
- Eigen removal → retire in S01 by implementing hand-rolled 2×2 inverse; confirmed by syntax check and docker build

## Verification Classes

- Contract verification: `scripts/verify-s01-syntax.sh` (g++ -fsyntax-only with mock headers); `grep` contracts on key patterns
- Integration verification: `docker-build.sh --linux --versions 16.0` exits 0; DeepCDefocusPO.so present in release zip
- Operational verification: none
- UAT / human verification: visual check in Nuke — bokeh visible and physically sized; holdout depth-correctness confirmed by artist

## Milestone Definition of Done

This milestone is complete only when all are true:

- All five slices are complete with summaries written
- DeepCDefocusPO.so present in release/DeepC-Linux-Nuke16.0.zip (confirmed by unzip -l)
- Node loads a real lentil .fit polynomial file without crash or error in Nuke
- Flat output visually shows bokeh on a test Deep image (non-zero pixels in defocused regions)
- Holdout input verified to operate in depth at output pixel — not as a 2D post-process
- `docker-build.sh --linux --versions 16.0` exits 0
- Node appears in FILTER_NODES menu category

## Requirement Coverage

- Covers: R019, R020, R021, R022, R023, R024, R025, R026
- Partially covers: none
- Leaves for later: R027 (aperture blade shape), R029 (bidirectional sampling — out of scope)
- Orphan risks: none

## Slices

- [x] **S01: Iop Scaffold, Poly Loader, and Architecture Decision** `risk:high` `depends:[]`
  > After this: Node compiles and loads into Nuke; accepts a Deep input; outputs flat black (no scatter yet); .fit file loads and poly evaluates without crash; gather vs scatter architecture decided and stubbed.

- [x] **S02: PO Scatter Engine — Stochastic Aperture Sampling** `risk:high` `depends:[S01]`
  > After this: Deep input is scattered to flat output via polynomial optics; bokeh shape and size visible; chromatic aberration visible on out-of-focus highlights; per-channel wavelength tracing (R/G/B at 0.45/0.55/0.65μm) working.

- [x] **S03: Depth-Aware Holdout** `risk:medium` `depends:[S02]`
  > After this: Holdout Deep input stencils defocused output at correct depths; holdout is never scattered through the lens; contribution weighting by holdout transmittance at sample depth confirmed correct; matches DeepHoldout semantics.

- [x] **S04: Knobs, UI, and Lens Parameter Polish** `risk:low` `depends:[S01]`
  > After this: All artist controls exposed with tooltips and sensible defaults; f-stop, focus distance, aperture sample count, lens file path all wired; node matches DeepC UI conventions.

- [x] **S05: Build Integration, Menu Registration, and Final Verification** `risk:low` `depends:[S01,S02,S03,S04]`
  > After this: docker-build.sh exits 0; DeepCDefocusPO.so in release zip; node in FILTER_NODES menu; syntax verifier passes; end-to-end test with real .fit file documented.

## Boundary Map

### S01 → S02

Produces:
- `src/DeepCDefocusPO.cpp` — compilable Iop (or PlanarIop) subclass skeleton; Deep input fetch via DeepOp::deepEngine(); flat output buffer allocation; File_knob wired to poly file path
- `src/poly.h` — vendored from lentil (MIT); poly_system_t, poly_system_read, poly_system_evaluate
- `deepc_po_math.h` — standalone 2×2 matrix inverse (replaces Eigen); Newton iteration for light-tracer (sensor pos + aperture point → outer pupil direction); CoC radius formula
- Architecture decision recorded in DECISIONS.md: gather vs scatter, Iop vs PlanarIop

Consumes:
- nothing (first slice)

### S01 → S04

Produces:
- Knob layout skeleton (File_knob, Float_knob focus_distance, Float_knob fstop, Int_knob aperture_samples)

Consumes:
- nothing (first slice)

### S02 → S03

Produces:
- `doEngine()` / `renderStripe()` implementation: per-pixel Deep fetch, per-sample aperture scatter loop, per-channel wavelength tracing, accumulation buffer write
- Flat RGBA output tile with physically correct bokeh
- Verified: non-zero output on test Deep input; chromatic fringing visible

Consumes from S01:
- `deepc_po_math.h` → Newton iteration, CoC formula
- `poly.h` → poly_system_evaluate
- Iop/PlanarIop scaffold → `doEngine()` / `renderStripe()` entry point

### S02 → S05

Produces:
- Working scatter engine for final integration verification

Consumes from S01:
- Full scaffold

### S03 → S05

Produces:
- `holdout` second input wired: DeepOp fetch, transmittance accumulation front-to-back, per-contribution weighting
- Verified: holdout stencils result at correct depth; holdout not scattered

Consumes from S02:
- `doEngine()` scatter loop — holdout weighting inserted per contribution before accumulation

### S04 → S05

Produces:
- All knobs complete with tooltips; default values sensible for a 50mm f/2.8 at 2m focus
- Node label and node_help string written

Consumes from S01:
- Knob skeleton
