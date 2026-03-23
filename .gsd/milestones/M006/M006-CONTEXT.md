# M006: DeepCDefocusPO

**Gathered:** 2026-03-23
**Status:** Ready for planning

## Project Description

DeepC is an open-source suite of deep-compositing Nuke NDK plugins. This milestone adds DeepCDefocusPO — a new plugin that takes a Deep image, defocuses it using a polynomial optics lens model (Schrade et al. 2016, as used in lentil/pota), and outputs a flat 2D image. The polynomial optics system uses per-lens pre-fitted coefficient files (lentil .fit format) to model real lens behaviour including aberrations and physically correct bokeh sizing. A second optional Deep input provides a depth-aware holdout stencil — evaluated per-output-pixel at the sample's depth — so that foreground elements (e.g. roto characters) mask the defocused background correctly without themselves being defocused. This solves the double-defocus problem that afflicts pgBokeh and Nuke's built-in Bokeh node.

## Why This Milestone

No existing Nuke tool performs depth-of-field defocus with: (1) polynomial optics accuracy, (2) physically correct bokeh sizes matching a real lens, (3) chromatic aberration from the lens model, and (4) depth-correct holdout integration. Nuke's ZDefocus and pgBokeh operate on flat images — they cannot accept a Deep input and they have no mechanism for depth-aware holdouts. DeepCDefocusPO fills this gap and is architecturally novel within DeepC (first node to produce flat output from a Deep input).

## User-Visible Outcome

### When this milestone is complete, the user can:

- Connect a Deep image to DeepCDefocusPO, point the File_knob at a lentil .fit polynomial file, set focus distance and f-stop, and get a flat defocused output with physically correct bokeh shape, size, and chromatic aberration
- Connect a second Deep input as holdout and observe that foreground elements stencil out the defocused background at their correct depth — without the holdout geometry itself appearing in the output or being defocused

### Entry point / environment

- Entry point: Nuke node graph — DeepCDefocusPO node in the FILTER_NODES menu category
- Environment: Nuke 16+ local session; .fit polynomial file on disk (from lentil gencode toolchain)
- Live dependencies involved: lentil .fit binary file (poly_system_read format), Nuke 16+ DDImage SDK

## Completion Class

- Contract complete means: `docker-build.sh --linux --versions 16.0` exits 0; DeepCDefocusPO.so present in release zip; node loads a .fit file without crash; syntax verifier passes
- Integration complete means: node accepts Deep input and produces non-zero flat output; holdout input visually stencils the result at correct depths
- Operational complete means: none — no service lifecycle requirements

## Final Integrated Acceptance

To call this milestone complete, we must prove:

- Load a real lentil .fit file, connect a test Deep image, render a flat output — bokeh is visible and physically sized (not zero, not infinite)
- Connect a holdout Deep input with a foreground object — the defocused background is stencilled behind it without the holdout itself appearing or being blurred
- `docker-build.sh --linux --versions 16.0` exits 0 with DeepCDefocusPO.so confirmed in release zip

## Risks and Unknowns

- **Iop/PlanarIop base class for Deep→flat** — DeepCDefocusPO is the first node in DeepC that outputs flat 2D from a Deep input. The correct Nuke NDK base class (Iop vs PlanarIop vs custom) and the mechanism for fetching Deep upstream from inside an Iop subclass are not yet confirmed against the SDK. Must be validated in S01 before S02 can be attempted.
- **Eigen dependency in Newton solver** — the light-tracer Newton iteration in lentil's gencode.h uses Eigen::Matrix2d for 2×2 Jacobian inversion. Eigen is not currently in the DeepC build. Either vendor Eigen (header-only) or replace with a hand-rolled 2×2 inverse to avoid a large new dependency.
- **poly.h binary .fit format stability** — the .fit file format is internal to lentil's polynomial-optics toolchain. There is no published spec beyond the source. The format must be treated as opaque and read via poly_system_read exactly.
- **Performance with stochastic scatter** — forward scatter (per input sample, splat N aperture points to output buffer) requires a thread-safe accumulation buffer. Nuke's tile-parallel engine calls doEngine per output tile — the scatter buffer must cover the full output format, not just the current tile, requiring a different threading model or a gather reframing.
- **Holdout fetch in Iop context** — fetching a Deep input from inside an Iop (not a DeepFilterOp) requires explicit DeepOp::deepEngine() calls. The request/validate lifecycle for mixed Iop+DeepOp inputs is not well-documented and must be confirmed in S01.

## Existing Codebase / Prior Art

- `src/DeepCDepthBlur.cpp` — most recent Deep spatial op; patterns for minimum_inputs/maximum_inputs, getDeepRequests, doDeepEngine, input_label, falloff weight generators
- `src/DeepCBlur.cpp` — reference for scatter-into-output-buffer pattern (horizontal gather using intermediate buffer); threading model under DeepFilterOp
- `src/DeepCConstant.cpp` — reference for Iop::Description usage alongside DeepFilterOp; FormatPair and format-driven _validate
- `src/DeepSampleOptimizer.h` — header-only deep sample utility; pattern for zero DDImage deps in shared headers
- `polynomial-optics/src/poly.h` (lentil, MIT) — poly_system_t, poly_system_read, poly_system_evaluate; the runtime polynomial evaluation engine to vendor
- `polynomial-optics/src/gencode.h` (lentil, MIT) — print_lt_sample_aperture: the light-tracer Newton iteration that maps scene point + aperture sample → sensor position; this is the scatter kernel

> See `.gsd/DECISIONS.md` for all architectural and pattern decisions — it is an append-only register; read it during planning, append to it during execution.

## Relevant Requirements

- R019 — DeepCDefocusPO: Deep image → flat 2D defocus via polynomial optics scatter
- R020 — Runtime polynomial lens file loading from lentil .fit binary format
- R021 — Physically correct bokeh size derived from lens focal length, f-stop, and focus distance
- R022 — Chromatic aberration via per-channel wavelength tracing (R/G/B at distinct λ)
- R023 — Depth-aware holdout: DeepHoldout semantics evaluated at output pixel
- R024 — Holdout is never defocused — depth gate evaluated at output pixel
- R025 — Stochastic aperture sampling with artist-controlled sample count
- R026 — Output is a flat 2D image (not Deep)

## Scope

### In Scope

- DeepCDefocusPO node: Deep → flat 2D, polynomial optics scatter
- poly.h + poly_system_read vendored from lentil (MIT-licensed); Eigen replaced with hand-rolled 2×2 inverse
- File_knob for .fit polynomial file path
- Float_knob for focus distance; Float_knob for f-stop
- Int_knob for aperture sample count N
- Per-channel wavelength tracing (R=0.45μm, G=0.55μm, B=0.65μm)
- Depth-aware holdout input: transmittance accumulated front-to-back at output pixel, applied as `contribution * transmittance_at_sample_depth`
- CMake + menu integration; docker build confirmation

### Out of Scope / Non-Goals

- Aperture blade count / texture bokeh shape (R027, deferred)
- Bidirectional/gather sampling (R029, out of scope)
- Thin-lens fallback mode (R030, out of scope)
- Windows build for this milestone — Linux only until core algorithm is confirmed correct
- Zoom lens support / focus breathing

## Technical Constraints

- Nuke 16+ only; GCC 11.2.1; `_GLIBCXX_USE_CXX11_ABI=1`; GPL-3.0 licence for the plugin; vendored lentil code is MIT
- No Arnold SDK dependency — poly.h is standalone C; gencode.h Newton iteration must be adapted without Eigen
- Iop (or PlanarIop) base class — not DeepFilterOp — because output is flat 2D
- Scatter buffer must be thread-safe: either use a gather model (per output pixel, pull from Deep input) or mutex/atomic accumulation

## Integration Points

- Nuke DDImage SDK — Iop, DeepOp, DeepPlane, Knobs, Box, ChannelSet
- lentil polynomial-optics (MIT) — poly.h, poly_system_read/evaluate vendored into src/
- .fit binary files — produced by lentil's gencode toolchain; read-only at render time
- docker-build.sh / CMakeLists.txt — DeepCDefocusPO added to PLUGINS list and FILTER_NODES menu var

## Open Questions

- PlanarIop vs Iop: PlanarIop may be preferable for performance (tile-parallel render without scatter buffer threading issues) but requires different engine() signature. Investigate in S01.
- Gather vs scatter: a gather model (for each output pixel, iterate input Deep samples that could contribute — i.e. those whose CoC covers this pixel) avoids the scatter buffer threading problem entirely but requires knowing the CoC radius before fetching, which needs the depth. The Deep input must be fetched per pixel anyway. This may be the cleaner architecture. Decide in S01.
