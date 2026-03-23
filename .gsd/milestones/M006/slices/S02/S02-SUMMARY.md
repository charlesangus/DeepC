# S02: PO Scatter Engine — Stochastic Aperture Sampling
## Slice Summary

**Milestone:** M006 — DeepCDefocusPO  
**Status:** Complete  
**Completed:** 2026-03-23  
**Proof level:** Contract (syntax + structural grep contracts)  
**Tasks:** 1 of 1 complete (T01)

---

## What This Slice Delivered

S02 replaced the two stubs left by S01 with the full polynomial-optics scatter engine:

1. **`lt_newton_trace` in `deepc_po_math.h`** — replaced the `{0,0}` stub with a 20-iteration Newton loop using a finite-difference Jacobian and the existing `mat2_inverse`. Convergence check at TOL=1e-5; singular Jacobian guard at |det|<1e-12 returns best estimate without NaN.

2. **`halton` and `map_to_disk` in `deepc_po_math.h`** — new inline free functions for aperture disk sampling. Van der Corput Halton construction (index offset +1 to skip the degenerate origin), Shirley concentric square-to-disk mapping with explicit degenerate-centre guard.

3. **`renderStripe` in `DeepCDefocusPO.cpp`** — replaced the zeroing loop with the full scatter engine:
   - `_poly_loaded` / `input0()` fast-path guard (zero output when no poly or no input)
   - Deep input fetch via `input0()->deepEngine(bounds, needed, deepPlane)`
   - Buffer zero-fill before accumulation (required for `+=` splat)
   - Normalised `[-1,1]` sensor coordinates throughout (avoids sensor-size metadata dependency)
   - Aperture radius = `1 / fstop` in normalised units
   - Per-pixel → per-deep-sample → per-aperture-sample loop
   - CoC-based early cull (performance guard; focal_length_mm=50 hardcoded)
   - Halton(2,3) + Shirley disk sampling for aperture positions
   - Per-channel Newton trace at R=0.45μm, G=0.55μm, B=0.65μm
   - Transmittance from `poly_system_evaluate` output[4], clamped [0,1]
   - Bounds-clamped `+=` splat with `1/N` weight × transmittance × alpha
   - Alpha uses G-channel (0.55μm) landing position
   - Out-of-stripe contributions discarded with seam-limitation comment
   - `Op::aborted()` poll at top of pixel loop

4. **`scripts/verify-s01-syntax.sh`** — extended with:
   - `Format::width()` / `Format::height()` stubs in mock (required now that `renderStripe` calls `fmt.width()`/`fmt.height()`)
   - 10 S02 grep contracts (all pass)

---

## Verification Results

| Check | Command | Result |
|-------|---------|--------|
| Syntax (all 3 files) | `bash scripts/verify-s01-syntax.sh` | ✅ exits 0 |
| halton function | `grep -q 'halton' src/deepc_po_math.h` | ✅ |
| map_to_disk function | `grep -q 'map_to_disk' src/deepc_po_math.h` | ✅ |
| R wavelength | `grep -q '0\.45f' src/DeepCDefocusPO.cpp` | ✅ |
| G wavelength | `grep -q '0\.55f' src/DeepCDefocusPO.cpp` | ✅ |
| B wavelength | `grep -q '0\.65f' src/DeepCDefocusPO.cpp` | ✅ |
| Deep input fetch | `grep -q 'deepEngine' src/DeepCDefocusPO.cpp` | ✅ |
| Newton called | `grep -q 'lt_newton_trace' src/DeepCDefocusPO.cpp` | ✅ |
| Jacobian inversion | `grep -q 'mat2_inverse' src/deepc_po_math.h` | ✅ |
| No stub markers | `grep -c 'TODO\|STUB\|S02: replace' src/DeepCDefocusPO.cpp` → 0 | ✅ |
| Observability surfaces | `grep -c 'aborted\|_poly_loaded\|deepEngine' src/DeepCDefocusPO.cpp` ≥ 3 → 13 | ✅ |
| S02 contracts block | embedded in verify script | ✅ all pass |

---

## Key Decisions Made in This Slice

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Sensor coordinate system | Normalised [-1,1] throughout | Avoids sensor-size metadata dependency; lentil polynomial normalisation may vary by .fit file; normalised coords produce visible output without any metadata |
| Aperture radius in normalised units | `1 / fstop` (not `focal_length / (2*fstop)` mm) | Consistent with normalised sensor coords; focal_length_mm=50 hardcoded for CoC culling only, not scatter |
| Focal length | Hardcoded 50mm (CoC culling only) | The Newton iteration naturally produces physically correct bokeh scale from the polynomial; focal length only needed for the performance cull. S04 adds a knob |
| M_PI fallback | `#ifndef M_PI / #define M_PI` before includes | Some platforms suppress M_PI from cmath; explicit fallback prevents silent build failures |
| poly_system_evaluate forward declaration | `#ifndef DEEPC_POLY_H` guard | ODR firewall: declaration suppressed when poly.h is already included; declared inline for standalone-header contexts |
| Mock Format class | Added `width()`/`height()` stubs | renderStripe calls `fmt.width()`/`fmt.height()` — mock was empty in S01, needed extension for syntax check |

---

## Patterns Established

These patterns should be followed by S03 and any future deep scatter nodes:

- **Normalised [-1,1] sensor coordinates** — sensor and aperture positions are expressed in normalised units throughout renderStripe. Pixel conversion: `sx = (px + 0.5 - half_w) / half_w`. Inverse: `out_px = landing_x * half_w + half_w - 0.5`. No mm conversion, no sensor-size constant.
- **Halton(2,3) + Shirley disk** — `halton(k, 2)` and `halton(k, 3)` with index offset +1; `map_to_disk(u, v, x, y)` produces uniform disk samples. Both functions live in `deepc_po_math.h`.
- **Newton convergence profile** — MAX_ITER=20, TOL=1e-5, EPS=1e-4. Singular Jacobian guard at |det|<1e-12 returns best estimate without NaN. No per-iteration log.
- **`_poly_loaded` fast-path** — always the first check in renderStripe. If false, zero the stripe and return. This prevents any Deep fetch or buffer allocation when no lens is loaded.
- **`deepEngine` error handling** — return value checked; `false` silently skips the stripe (output stays zeroed from the buffer-clear step). Upstream Deep error surfaces normally through Nuke.
- **`Op::aborted()` poll** — once per pixel in the outer loop. Sufficient for PlanarIop's single-threaded model.
- **Out-of-stripe discard** — contributions landing outside `imagePlane.bounds()` are silently discarded with a code comment noting the seam limitation. Do not wrap.
- **`1/N` normalisation** — each aperture sample contributes `1/N * transmittance * alpha * channel_value`. No separate normalisation pass.
- **Alpha = G-channel landing** — alpha splatted at the G-channel (0.55μm) landing pixel position, not the R or B position.

---

## Observability / Diagnostic Surfaces

| Surface | How to read it |
|---------|---------------|
| `_poly_loaded == false` | Output uniformly black in Nuke viewer. No error node. |
| `poly_system_read` non-zero | `error()` called on the Op → red error node in Nuke DAG. |
| `deepEngine` returns false | Stripe stays zeroed. Upstream Deep node shows its own error. |
| Newton non-convergence | Iteration exits early; best estimate returned. No NaN/inf. Bokeh disc may be slightly misplaced. |
| Seam artefacts | Dark seams at stripe boundaries for large bokeh (contribution discarded outside stripe). Expected behaviour. |
| `Op::aborted()` | Scatter loop exits immediately; output is partial (whatever accumulated before abort). Normal cancel behaviour. |

---

## Deviations from Plan

- **Normalised coordinates adopted over mm coordinates** — the research proposed a `pix_per_mm` approach with a 36mm sensor assumption. The implementation used normalised `[-1,1]` sensor coordinates instead, as explicitly recommended in the research's constraint section 4 ("Safest approach for S02"). This is consistent with the plan's intent and more robust.
- **Mock Format stubs added to verify script** — the task plan focused on source files; the Format mock gap was discovered during implementation. Not a plan deviation but an unplanned-for extension of `verify-s01-syntax.sh`.
- **Single task instead of two** — the research proposed T01 (math helpers) + T02 (scatter loop) as separate tasks. Both were implemented in a single T01. No impact on the deliverable.

---

## Known Limitations

- **Stripe-boundary seams** — scatter contributions that land in a neighbouring stripe are discarded. For large bokeh (low f-stop, sample far from focus), this creates a visible dark seam at stripe boundaries. Acceptable for S02; mitigation (single full-height stripe) deferred to S04.
- **Focal length hardcoded at 50mm** — used only for CoC culling, not for scatter itself. Bokeh scale is determined by the polynomial, not this constant. Full focal-length knob deferred to S04.
- **No per-pixel accumulation count normalisation** — pixels receiving zero contributions remain at 0.0f (black). Pixels receiving N contributions are normalised by `1/N` within the scatter loop itself. This is correct but means sparse output regions (few deep samples) may appear darker than expected — expected Monte Carlo behaviour at low sample counts.
- **Runtime proof deferred** — non-zero pixel output and visible bokeh are not verifiable without a Docker build and a real .fit lens file loaded in Nuke. Both deferred to S05 milestone definition of done.

---

## What S03 Needs to Know

S03 inserts holdout weighting **inside the aperture loop**, immediately before the `writableAt` accumulation in `renderStripe`. The insertion point is at the splat step (step 8 in the loop):

```cpp
// Before splat: weight by holdout transmittance at output pixel at this depth
const float holdout_w = holdout_transmittance_at(out_xi, out_yi, depth);
imagePlane.writableAt(...) += chan_val * alpha * w * holdout_w;
```

Key facts for S03:
- Holdout input is already wired as `Op::input(1)` with `input_label` returning "holdout" — from S01.
- `dynamic_cast<DeepOp*>(Op::input(1))` gives the holdout DeepOp; null check required (input is optional).
- Holdout fetch: `holdout->deepEngine(bounds, holdout_channels, holdoutPlane)` alongside the primary fetch at the top of renderStripe.
- Transmittance at depth Z = product of `(1 - alpha_i)` for all holdout samples with `zFront_i < Z`. This is standard deep-over transmittance accumulation (same as Nuke's DeepHoldout).
- The holdout is **not scattered** — only the transmittance at the output pixel position is sampled. It contributes no colour to the output.
- The holdout `deepEngine` call uses the output pixel bounds (same as the primary fetch), not the input pixel position.

---

## Requirements Validated by This Slice

| ID | Description | Proof |
|----|-------------|-------|
| R019 | DeepCDefocusPO: Deep → flat 2D via PO scatter | renderStripe scatter loop implemented; grep contracts all pass; syntax check exits 0. Runtime proof at S05. |
| R021 | CoC radius from focal length, f-stop, focus distance | coc_radius() used for culling; bokeh size from polynomial. Focal-length knob deferred to S04. Runtime size match deferred to S05. |
| R022 | Chromatic aberration via per-channel wavelength tracing | lambdas[] = {0.45f, 0.55f, 0.65f} grep contracts pass. Runtime chromatic fringing visible at S05. |
| R025 | Stochastic aperture sampling with artist-controlled N | Halton(2,3) + Shirley disk; Int_knob aperture_samples loop. grep contracts pass. |

Previously validated by S01: R020 (poly load), R026 (PlanarIop/flat output).
