# S02: PO Scatter Engine — Stochastic Aperture Sampling

**Goal:** Replace the two stubs left by S01 — `lt_newton_trace` in `deepc_po_math.h` and the zeroing loop in `renderStripe` in `DeepCDefocusPO.cpp` — with the full polynomial-optics scatter engine. After this slice, DeepCDefocusPO is algorithmically complete: it fetches Deep input, scatters each sample through the lens polynomial via Newton iteration, traces R/G/B at three wavelengths for chromatic aberration, and accumulates results into the flat RGBA output buffer.

**Demo:** `bash scripts/verify-s01-syntax.sh` exits 0; grep contracts confirm `halton`, `map_to_disk`, Newton iteration body, per-channel wavelength loop (0.45/0.55/0.65), and `deepEngine` call are all present in the modified source files.

## Must-Haves

- `lt_newton_trace` in `deepc_po_math.h` replaced with real Newton iteration (finite-difference Jacobian, `mat2_inverse`, convergence check, singular Jacobian guard)
- `halton(index, base)` and `map_to_disk(u, v, x, y)` added as inline free functions in `deepc_po_math.h`
- `renderStripe` in `DeepCDefocusPO.cpp` replaced with: Deep input fetch via `deepEngine`, per-pixel/per-deep-sample/per-aperture-sample loop, per-channel wavelength trace at 0.45/0.55/0.65 μm, sensor→pixel coordinate conversion, bounds-clamped splat with `+=`
- `_poly_loaded` guard present in `renderStripe` (zero output path when no poly is loaded)
- Alpha channel uses G-channel (0.55 μm) landing position
- Out-of-stripe contributions discarded (not wrapped), with a code comment noting the seam limitation
- Normalisation weight `1/N` applied per aperture sample contribution
- Syntax check passes: `bash scripts/verify-s01-syntax.sh` exits 0
- S02 grep contracts added to `scripts/verify-s01-syntax.sh` and all pass

## Proof Level

- This slice proves: contract (syntax + structural grep contracts)
- Real runtime required: no (Docker not available in workspace; scatter correctness visible in Nuke at S05)
- Human/UAT required: no (runtime visual check deferred to S05 milestone definition of done)

## Verification

- `bash scripts/verify-s01-syntax.sh` exits 0 (all three source files: DeepCBlur.cpp, DeepCDepthBlur.cpp, DeepCDefocusPO.cpp)
- `grep -q 'halton' src/deepc_po_math.h`
- `grep -q 'map_to_disk' src/deepc_po_math.h`
- `grep -q '0\.45f' src/DeepCDefocusPO.cpp` — R wavelength
- `grep -q '0\.55f' src/DeepCDefocusPO.cpp` — G wavelength
- `grep -q '0\.65f' src/DeepCDefocusPO.cpp` — B wavelength
- `grep -q 'deepEngine' src/DeepCDefocusPO.cpp` — Deep input fetch
- `grep -q 'lt_newton_trace' src/DeepCDefocusPO.cpp` — Newton trace called in scatter loop
- `grep -q 'mat2_inverse' src/deepc_po_math.h` — Newton Jacobian inversion (already present, guard that stub body is replaced)
- `grep -c 'TODO\|STUB\|S02: replace' src/DeepCDefocusPO.cpp` returns 0 — no leftover stub markers

## Observability / Diagnostics

**Runtime signals added by this slice:**

- `_poly_loaded` flag: when `false`, `renderStripe` takes the fast-path zero branch and returns immediately — inspectable in the Nuke node by the absence of any output pixels.
- `deepEngine` return value: checked on each stripe call; a `false` return causes the stripe to be silently skipped (output remains zeroed from the buffer-clear step). Visible as a black stripe in Nuke's viewer; the upstream Deep error, if any, propagates through Nuke's normal error system.
- `Op::aborted()` poll: scatter loop exits early when the user cancels — no orphaned threads since `PlanarIop` is single-threaded.
- Newton convergence: no per-iteration log is emitted (would be too verbose for production); failure modes (singular Jacobian, non-convergence) silently return the best estimate so far — bokeh discs may be slightly misplaced but no NaN/crash occurs.

**Inspecting failure state:**
- No poly loaded: `_poly_loaded == false` → output is uniformly black. Visible immediately in viewer.
- Wrong poly file: `poly_system_read` returns non-zero → `error(...)` is called on the Op, which surfaces as a red error node in Nuke's DAG.
- `deepEngine` failure: stripe output is zeroed. Nuke will show black for that region; the upstream error is reported by the upstream node.
- Seam artifacts at stripe boundaries (large bokeh): contributions that land outside the current stripe are discarded (not wrapped). Visible as dark seams when bokeh radius is large relative to stripe height — expected behaviour, documented in the seam-limitation comment in the scatter loop.

**Redaction constraints:** None — no secrets or user-identifiable data in any signal.

**Verification step for failure visibility:**
- `grep -q '_poly_loaded' src/DeepCDefocusPO.cpp` — confirms the guard is present in source
- `grep -q 'aborted' src/DeepCDefocusPO.cpp` — confirms abort-poll is present



- Upstream surfaces consumed: `src/deepc_po_math.h` (Mat2, mat2_inverse, Vec2, coc_radius stubs from S01), `src/poly.h` (poly_system_evaluate), `src/DeepCDefocusPO.cpp` (PlanarIop scaffold, `_poly_sys` member, `_fstop`/`_focus_distance`/`_aperture_samples` knobs, `renderStripe` entry point)
- New wiring introduced in this slice: `halton`/`map_to_disk` added to `deepc_po_math.h`; `lt_newton_trace` stub body replaced with Newton iteration; `renderStripe` scatter loop calling `deepEngine`, `lt_newton_trace`, and `poly_system_evaluate`
- What remains before the milestone is truly usable end-to-end: S03 (depth-aware holdout), S04 (knob polish), S05 (docker build + menu registration)

## Tasks

- [x] **T01: Implement Newton trace helpers and PO scatter loop** `est:2h`
  - Why: The two stubs from S01 (`lt_newton_trace` and `renderStripe` zeroing loop) are the only missing pieces. This task replaces both with production implementations, completing the core algorithm.
  - Files: `src/deepc_po_math.h`, `src/DeepCDefocusPO.cpp`, `scripts/verify-s01-syntax.sh`
  - Do: (1) In `deepc_po_math.h`, add `halton` and `map_to_disk` inline free functions above `lt_newton_trace`. (2) Replace the `lt_newton_trace` stub body with the Newton iteration (finite-difference Jacobian, `mat2_inverse`, 20-iteration loop, TOL=1e-5, singular-Jacobian guard). (3) In `DeepCDefocusPO.cpp`, replace the `renderStripe` zeroing loop body with: Deep input fetch via `input0()->deepEngine(bounds, needed, deepPlane)`; accumulation zero-fill; per-pixel/per-deep-sample/per-aperture-sample loop using Halton+Shirley disk sampling; per-channel (`c=0,1,2`) `lt_newton_trace` at lambdas `{0.45f, 0.55f, 0.65f}`; sensor→pixel conversion using normalised `[-1,1]` coordinates; bounds-clamped `+=` splat; alpha uses G landing. (4) Add S02 grep contracts to `verify-s01-syntax.sh` and confirm all pass.
  - Verify: `bash scripts/verify-s01-syntax.sh && grep -q 'halton' src/deepc_po_math.h && grep -q 'map_to_disk' src/deepc_po_math.h && grep -q '0\.45f' src/DeepCDefocusPO.cpp && grep -q 'deepEngine' src/DeepCDefocusPO.cpp`
  - Done when: All verification commands above pass; no S02 stub markers remain in the source files

## Files Likely Touched

- `src/deepc_po_math.h`
- `src/DeepCDefocusPO.cpp`
- `scripts/verify-s01-syntax.sh`
