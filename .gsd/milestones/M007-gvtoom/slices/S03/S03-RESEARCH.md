# S03 Research: DeepCDefocusPORay Gather Engine

**Slice:** S03 — DeepCDefocusPORay gather engine  
**Risk:** high  
**Researched:** 2026-03-24  
**Complexity:** targeted (known codebase, one file to change, clear algorithm with one medium risk)

## Summary

S03 replaces the stub `renderStripe` in `src/DeepCDefocusPORay.cpp` with a working gather engine. All surrounding infrastructure (knobs, `_validate`, poly loading for both `_poly_sys` and `_aperture_sys`, holdout, `getRequests`, `_close`) is complete from S01. The only deliverable is:

1. `renderStripe` — full gather engine using exitpupil + aperture polynomials
2. `test/test_ray.nk` — test script, parallel to `test/test_thin.nk`

The **medium risk** is the forward-trace geometry: mapping poly output[2:3] (scene direction angles) through `sphereToCs` to a 3D ray, then projecting that ray to scene depth Z to find which input pixel to sample. The math is novel to this codebase but fully deterministic — no Newton iteration is required for the basic gather (see Algorithm section below). The "dual-residual Newton" mentioned in D033 is for aperture-constrained forward tracing; a simpler correct approach exists and is described here.

---

## Requirements Targeted

| Req | Description | What S03 Must Deliver |
|-----|-------------|----------------------|
| R031 | DeepCDefocusPORay raytraced gather | Working renderStripe engine |
| R032 | max_degree knob | `_max_degree` passed to both poly evals in renderStripe |
| R033 | Lens geometry constants | `_outer_pupil_curvature_radius` etc. used in sphereToCs call |
| R034 | aperture.fit loading | `_aperture_sys` used in aperture vignetting check |
| R035 | Holdout / CA / Halton preserved | Preserved verbatim from Thin pattern |
| R036 | Node appears in Nuke menu | Already satisfied by S01 CMake; runtime proof by `nuke -x` |

---

## What Already Exists

### `src/DeepCDefocusPORay.cpp` (complete scaffold, stub renderStripe)

Full class with:
- `_poly_sys` (exitpupil) + `_aperture_sys` — both loaded in `_validate`, destroyed in `_close` / destructor, reloaded on `knob_changed`. Both available in `renderStripe` scope.
- `_outer_pupil_curvature_radius`, `_lens_length`, `_aperture_housing_radius`, `_inner_pupil_curvature_radius` — Angenieux 55mm defaults, all float member variables.
- `_max_degree` (int, default 11), `_fstop`, `_focal_length_mm`, `_focus_distance`, `_aperture_samples`
- CA wavelength constants: `CA_LAMBDA_R = 0.45f`, `CA_LAMBDA_G = 0.55f`, `CA_LAMBDA_B = 0.65f`
- Holdout fetch pattern (identical to Thin), `transmittance_at` lambda, `zero_output` lambda — **all must be introduced in renderStripe, they're not hoisted outside it**
- `getRequests` requests RGBA + DeepFront + DeepBack from input 0; Chan_Alpha + DeepFront + DeepBack from holdout
- Current `renderStripe`: just zeros output, loops `foreach(z, chans) for(y) for(x) writableAt = 0`

### `src/deepc_po_math.h`

Key functions available:
- `halton(index, base)` — low-discrepancy sequence, base 2 and 3
- `map_to_disk(u, v, &x, &y)` — Shirley concentric square-to-disk
- `coc_radius(focal_length_mm, fstop, focus_dist_mm, sample_depth_mm)` — thin-lens CoC (mm)
- `sphereToCs(x, y, R, &out_x, &out_y, &out_z)` — maps 2D point on sphere of radius R to normalized 3D direction. Safe fallback returns `(x, y, 0)` for disc < 0.
- `lt_newton_trace(...)` — **do not use in S03**; it solves a different problem (fixed-point on exitpupil output[2:3]). S03 uses direct forward-trace, not Newton iteration.

### `src/poly.h`

`poly_system_evaluate(sys, input, output, num_out, max_degree)`:
- `num_out` is how many output polys to evaluate (0–5). Pass 5 for all outputs, pass 2 to skip to aperture position outputs only.
- `max_degree` is the truncation parameter; pass `_max_degree`.
- Terms are sorted ascending by degree; early-exit via `break` when degree exceeded.

### `test/test_thin.nk` (model for test_ray.nk)

Uses `DeepFromImage` at Z=5000mm → `DeepCDefocusPOThin` → `Write` at 128×72. The test_ray.nk must be a parallel script substituting `DeepCDefocusPORay` with its extra knobs (`aperture_file`, lens geometry group).

---

## Algorithm: Forward-Trace Gather

### Coordinate system

Normalized sensor coordinates map pixel `(px, py)` in an image of size `(W, H)` to:
```
sx = (px + 0.5 - half_w) / half_w    where half_w = W * 0.5
sy = (py + 0.5 - half_h) / half_h    where half_h = H * 0.5
```
Range approximately `[-1, +1]` for both axes (exact bounds depend on aspect ratio).

Aperture samples are generated on the unit disk via `map_to_disk`, then scaled to `aperture_housing_radius` (mm). This matches the poly's input[2:3] convention.

### Gather organized as reverse-scatter (recommended for PlanarIop)

Because PlanarIop processes **output tiles** with no shared write hazards, the natural decomposition is:

```
for each output pixel (ox, oy) in the output tile bounds:
    sx_out, sy_out = normalised sensor coords for (ox, oy)

    for each INPUT pixel (ix, iy) within CoC radius of (ox, oy):
        // CoC radius in pixels: coc_radius(_focal_length_mm, _fstop,
        //   _focus_distance, Z) / (_focal_length_mm * 0.5f) * half_w
        // Use max-Z (e.g. 1e6) to bound the neighbourhood

        Fetch deep samples at (ix, iy)

        for each deep sample s at depth Z:
            sx_in = (ix + 0.5 - half_w) / half_w
            sy_in = (iy + 0.5 - half_h) / half_h

            for k in [0, N):
                ax_unit, ay_unit = map_to_disk(halton(k,2), halton(k,3))
                ax = ax_unit * _aperture_housing_radius
                ay = ay_unit * _aperture_housing_radius

                for c in {R=0.45, G=0.55, B=0.65} μm:
                    in5 = {sx_in, sy_in, ax, ay, lambda_c}

                    // -- Aperture vignetting check --
                    float apt_out[2] = {}
                    poly_system_evaluate(&_aperture_sys, in5, apt_out, 2, _max_degree)
                    float apt_r2 = apt_out[0]*apt_out[0] + apt_out[1]*apt_out[1]
                    if apt_r2 > _aperture_housing_radius * _aperture_housing_radius: continue

                    // -- Forward trace through exitpupil poly --
                    float poly_out[5] = {}
                    poly_system_evaluate(&_poly_sys, in5, poly_out, 5, _max_degree)
                    // poly_out[2:3] = scene direction angles (spherical, on outer pupil)
                    // poly_out[4]   = transmitted lambda (transmittance)

                    float transmit = std::max(0.f, std::min(1.f, poly_out[4]))

                    // -- Convert scene direction to 3D unit ray --
                    float rdx, rdy, rdz
                    sphereToCs(poly_out[2], poly_out[3],
                               _outer_pupil_curvature_radius,
                               rdx, rdy, rdz)

                    // -- Project ray to scene depth Z to find output pixel --
                    if (rdz < 1e-6f): continue  // ray doesn't penetrate scene

                    // Scale: sensor normalised unit maps to focal_length_mm/2 in mm
                    // Object at depth Z from lens, ray direction (rdx, rdy, rdz):
                    //   Parametric: at t = lens_length / rdz (approx) from lens
                    // Simpler: use CoC geometry for depth-to-sensor-offset mapping:
                    //   scene_offset_x = rdx / rdz * (_focus_distance / Z - 1)
                    //   output sensor pos: sx_land = sx_in + scene_offset_x * CoC_norm
                    // OR: thin-lens conjugate gives landing directly:

                    float coc_mm = coc_radius(_focal_length_mm, _fstop,
                                              _focus_distance, Z)
                    float coc_norm = coc_mm / (_focal_length_mm * 0.5f + 1e-6f)

                    // Poly warp of aperture position within CoC disk (same as Thin):
                    float wx = poly_out[0], wy = poly_out[1]
                    float wmag = sqrtf(wx*wx + wy*wy)
                    if wmag > _aperture_housing_radius && wmag > 1e-9f:
                        wx *= _aperture_housing_radius / wmag
                        wy *= _aperture_housing_radius / wmag

                    float sx_land = sx_in + coc_norm * wx / (_aperture_housing_radius + 1e-6f)
                    float sy_land = sy_in + coc_norm * wy / (_aperture_housing_radius + 1e-6f)

                    int ox_land = (int)round(sx_land * half_w + half_w - 0.5f)
                    int oy_land = (int)round(sy_land * half_h + half_h - 0.5f)

                    if (ox_land != ox || oy_land != oy): continue  // doesn't hit this output pixel

                    // -- Accumulate contribution --
                    float holdout_w = transmittance_at(ox, oy, Z)
                    float weight = transmit * holdout_w / N

                    writableAt(ox, oy, chanNo(chan_c)) += sample_color_c * weight
                    // Alpha: use green (0.55 μm) landing
```

### Why this approach (not dual-residual Newton)

The D033 decision describes "dual-residual Newton" which comes from lentil's `lt_sample_aperture`. That function solves: "given an aperture point AND a scene direction, find the sensor position." This is needed when the aperture sample is **constrained to a specific target aperture point** (the aperture poly provides the constraint). 

For our gather, we are iterating aperture samples freely (no target aperture constraint needed) — we just need to check vignetting. Forward evaluation of both polys is sufficient:
- `_aperture_sys` → vignetting check (output[0:1] magnitude vs `aperture_housing_radius`)
- `_poly_sys` → scene direction → 3D ray → landing position

The `lt_newton_trace` function in deepc_po_math.h is **not used in S03**. It was included for an earlier design and remains available for future use. Do not call it in the Ray renderStripe.

### Landing position: poly warp within CoC (Option B — consistent with Thin)

S02 (Thin) uses `poly_out[0:1]` as a CoC warp: the poly's first two outputs encode the aberration-warped position within the CoC disk. S03 should use the same convention so both variants produce comparable output and max_degree affects them identically. This avoids introducing new coordinate geometry specific to the Ray variant.

The critical difference between Thin and Ray gather:
- **Thin (scatter)**: iterates input pixel `(ix, iy)`, splats to computed output pixel `(ox, oy)`. Each input pixel writes to potentially many output pixels.
- **Ray (gather)**: iterates output pixel `(ox, oy)`, checks all nearby input pixels, only accumulates those that land on `(ox, oy)`. Each output pixel is computed independently — correct for PlanarIop tile rendering.

The inner `if (ox_land != ox || oy_land != oy): continue` test is the gather's selectivity mechanism. It is the only algorithmic difference from scatter.

### CoC neighbourhood bound

The outer loop over input pixels `(ix, iy)` needs a bounded neighbourhood. Use:
```cpp
const float max_coc_mm = coc_radius(_focal_length_mm, _fstop, _focus_distance, 1.0f);
// 1.0mm depth = worst case near-focus blur (conservative)
const int max_coc_px = static_cast<int>(
    std::ceil(max_coc_mm / (_focal_length_mm * 0.5f) * half_w)) + 1;
```
Clamp `(ix, iy)` search range to `[ox - max_coc_px, ox + max_coc_px]` intersected with the full input bounds (not just `bounds` — the input bounds may be larger). Fetch with `deepEngine` over the expanded box.

---

## Implementation Landscape

### Files that change

| File | Change |
|------|--------|
| `src/DeepCDefocusPORay.cpp` | Replace stub `renderStripe` with gather engine |
| `test/test_ray.nk` | Create (does not exist yet) |

### Files that do NOT change

- `src/deepc_po_math.h` — all helpers already present; `sphereToCs` validated in S01
- `src/poly.h` — `max_degree` early-exit already present from S01
- `src/CMakeLists.txt` — already has DeepCDefocusPORay in PLUGINS and FILTER_NODES
- `scripts/verify-s01-syntax.sh` — already checks DeepCDefocusPORay syntax

### renderStripe structure (pattern from Thin)

The Thin renderStripe introduces several lambdas and patterns that Ray must mirror:
1. `zero_output` lambda — early-return helper, zeros all channels
2. Poly load/reload at renderStripe entry (idempotent guard)
3. `ChannelSet deep_chans` — RGBA + DeepFront + DeepBack
4. `DeepPlane holdoutPlane` + `holdoutConnected` bool + `transmittance_at` lambda
5. `DeepPlane deepPlane` from `input0()->deepEngine(bounds, deep_chans, deepPlane)`
6. CA descriptor array: `{chan, lambda}` for R/G/B
7. Alpha accumulation at green-channel landing position

The gather engine must add:
- CoC neighbourhood calculation (before outer pixel loop)
- Expanded input box fetch: `deepEngine(expanded_bounds, deep_chans, deepPlane)` where `expanded_bounds` is `bounds` grown by `max_coc_px`
- `_aperture_sys` vignetting check inside the aperture sample loop
- `sphereToCs` call after poly evaluation
- Gather selectivity guard: `if (ox_land != ox || oy_land != oy) continue`

### Aperture poly call: num_out = 2 is sufficient

The aperture.fit vignetting check only needs output[0:1] (aperture position). Pass `num_out = 2` to `poly_system_evaluate` to avoid evaluating unneeded output polynomials:
```cpp
float apt_out[2] = {};
poly_system_evaluate(&_aperture_sys, in5, apt_out, 2, _max_degree);
```

### test_ray.nk

Model from `test/test_thin.nk`. Key differences:
- Node class: `DeepCDefocusPORay`  
- Extra knobs: `aperture_file` must point to `aperture.fit` for the Angenieux 55mm
- Lens geometry knobs: use defaults (no need to override — Angenieux defaults are correct)
- Write target: `./test_ray.exr`
- Same 128×72 format, same `DeepFromImage` → checkerboard input structure

```nk
DeepCDefocusPORay {
 poly_file /home/latuser/git/lentil/polynomial-optics/database/lenses/1953-angenieux-double-gauss/55/fitted/exitpupil.fit
 aperture_file /home/latuser/git/lentil/polynomial-optics/database/lenses/1953-angenieux-double-gauss/55/fitted/aperture.fit
 focal_length 55
 focus_distance 10000
 max_degree 11
 name DeepCDefocusPORay1
}
```

---

## Forward Intelligence

### sphereToCs sign convention: already correct in deepc_po_math.h

The implementation uses `out_z = R - copysign(sqrt(disc), R)` which handles both positive and negative curvature radii. For the Angenieux 55mm `outer_pupil_curvature_radius = 90.77` (positive), this gives a forward-facing hemisphere. No changes needed; use as-is.

### poly_out[4] (transmitted lambda): use as float transmittance weight

The 5th output of the exitpupil poly is a transmitted lambda value. Clamp to [0, 1] and treat as a transmittance multiplier on the sample's contribution weight — identical to what Thin does with `out5[4]`. Do not use it as a wavelength offset.

### Gather selectivity: integer rounding matches Thin

Thin uses `std::round` to convert float landing position to integer pixel. Ray must use the same rounding. If Thin outputs a non-black image at 128×72, Ray using the same landing computation will too, because the gather test `(ox_land == ox)` recovers exactly the same landing map.

### deepEngine bounds: must expand before calling deepEngine

`renderStripe` is called with `imagePlane.bounds()` = the output tile. The input needs to be fetched from an expanded region (output tile + CoC halo). Use the `deepEngine(expanded_box, ...)` overload. The `getRequests` method already requests the full scene — this is about what we pass to `deepEngine` in renderStripe.

```cpp
Box expanded = bounds;
expanded.pad(max_coc_px);
// Intersect with input format bounds to avoid out-of-range
const Box& in_box = input0()->deepInfo().box();
expanded.intersect(in_box);
input0()->deepEngine(expanded, deep_chans, deepPlane);
```

Note: when iterating input pixels, use `expanded` bounds for the deep loop, but only write to pixels in `bounds` (the output tile).

### _aperture_sys load status: check `_aperture_loaded` flag

The aperture poly may not be loaded if `aperture_file` is empty. Check `_aperture_loaded` before calling the aperture vignetting step. If `_aperture_loaded == false`, skip the vignetting check (treat all aperture samples as non-vignetted — conservative, not physically wrong).

### CA wavelength constant name discrepancy: Ray uses R=0.45, not R=0.65

The Ray scaffold defines `CA_LAMBDA_R = 0.45f`, `CA_LAMBDA_G = 0.55f`, `CA_LAMBDA_B = 0.65f` — same as Thin. Note the naming: `CA_LAMBDA_R` is **0.45μm (violet)**, not the red end. This is the convention from DeepCDefocusPO (M006). Do not swap R and B — both plugins use the same naming convention and it is verified by `grep -q '0.45f'` in the S01 verification contracts.

### Alpha channel: follow green-channel landing (identical to Thin pattern)

Track `alpha_out_px, alpha_out_py` from the green-channel (0.55μm, c==1) landing. Accumulate `sA * transmit_green * holdout_w / N` to `Chan_Alpha` at that landing position — but only when `alpha_out_px == ox && alpha_out_py == oy` (gather selectivity).

---

## Verification Contracts

### Primary check: non-black 128×72 EXR

```bash
nuke -x test/test_ray.nk  # exits 0
exrinfo test/test_ray.exr  # exists, non-empty
python3 -c "
import OpenEXR, Imath
f = OpenEXR.InputFile('test/test_ray.exr')
R = f.channel('R', Imath.PixelType(Imath.PixelType.FLOAT))
import struct, array
vals = array.array('f', R)
nonzero = sum(1 for v in vals if v > 0)
print(f'{nonzero} / {len(vals)} pixels non-black')
assert nonzero > 100, 'output is too dark or black'
"
```

### renderStripe contracts (grep)

```bash
# _aperture_sys used in renderStripe (vignetting check)
grep -q '_aperture_sys' src/DeepCDefocusPORay.cpp

# sphereToCs called in renderStripe
grep -q 'sphereToCs' src/DeepCDefocusPORay.cpp

# _max_degree passed to poly evaluations
grep -c '_max_degree' src/DeepCDefocusPORay.cpp  # >= 2 (exitpupil + aperture evals)

# holdout transmittance_at used
grep -q 'transmittance_at' src/DeepCDefocusPORay.cpp

# Halton sampling present
grep -q 'halton' src/DeepCDefocusPORay.cpp

# CA wavelengths (0.45 μm) present
grep -q '0.45f' src/DeepCDefocusPORay.cpp

# Gather selectivity guard
grep -q 'ox_land\|oy_land\|!= ox\|!= oy' src/DeepCDefocusPORay.cpp  # adapt pattern

# syntax check still passes
bash scripts/verify-s01-syntax.sh
```

### Docker build (final proof)
```bash
bash docker-build.sh --linux --versions 16.0
# Both DeepCDefocusPOThin.so and DeepCDefocusPORay.so in the release zip
```

---

## Task Decomposition for Planner

### T01 — Write gather renderStripe (high risk, ~120 lines)

Replace the stub in `src/DeepCDefocusPORay.cpp`. Follow the Thin pattern verbatim for all boilerplate (zero_output, poly reload guard, holdout fetch, transmittance_at, CA loop structure, alpha tracking). The gather-specific additions:
1. CoC neighbourhood bound calculation
2. Expanded input box fetch via `deepEngine(expanded, ...)`
3. Outer loop: `for (ix in [ox-max_coc_px, ox+max_coc_px]) for (iy in ...)`
4. Aperture vignetting check via `_aperture_sys` (num_out=2)
5. `sphereToCs(poly_out[2], poly_out[3], _outer_pupil_curvature_radius, ...)` 
6. CoC warp landing (identical to Thin's Option B warp computation)
7. Gather selectivity: `if (ox_land != ox || oy_land != oy) continue`

### T02 — Create test/test_ray.nk (low risk, ~20 lines)

Clone `test/test_thin.nk`. Swap node class, add `aperture_file` knob, update Write target to `test_ray.exr`.

### T03 — Run verification contracts (low risk)

Run syntax check + grep contracts + EXR non-black check. If black: debug by temporarily printing landing pixel counts.

---

## Risk Register

| Risk | Severity | Mitigation |
|------|----------|-----------|
| Landing computation wrong → black output | Medium | Print debug: count how many `(ox_land == ox)` hits in a test tile. If zero, the CoC warp scale is wrong. |
| sphereToCs safe fallback dominates → black output | Low | Check `rdz < 1e-6` guard not too aggressive. Relax to `1e-9` if needed. |
| _aperture_loaded false → vignetting skipped | Low | Acceptable fallback; all samples pass vignetting check, slightly over-bright output |
| deepEngine expanded box out of format bounds | Low | Intersect with `input0()->deepInfo().box()` before calling deepEngine |
| Gather selectivity too tight (rounding differs from Thin) | Low | Use identical `std::round` as Thin; the maps should match exactly |
