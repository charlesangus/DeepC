---
estimated_steps: 5
estimated_files: 1
skills_used: []
---

# T01: Implement gather renderStripe in DeepCDefocusPORay.cpp

**Slice:** S03 — DeepCDefocusPORay gather engine
**Milestone:** M007-gvtoom

## Description

Replace the zero-output stub `renderStripe` in `src/DeepCDefocusPORay.cpp` with a full gather engine. The gather iterates each output pixel, searches a CoC-bounded neighbourhood of input pixels, and accumulates contributions from deep samples whose polynomial-warped landing position matches the output pixel.

The implementation follows the Thin scatter pattern (S02) for all boilerplate — zero_output lambda, poly reload guard, holdout fetch, transmittance_at lambda, CA per-channel loop, alpha at green-channel landing — and adds gather-specific logic:

1. **Expanded input fetch**: `deepEngine(expanded_bounds, ...)` where `expanded_bounds = bounds.pad(max_coc_px)` intersected with input format bounds.
2. **Input neighbourhood loop**: For each output pixel `(ox, oy)`, iterate input pixels `(ix, iy)` within `±max_coc_px`.
3. **Aperture vignetting**: Evaluate `_aperture_sys` with `num_out=2`; skip if `|apt_out| > aperture_housing_radius`. Skip entirely if `!_aperture_loaded`.
4. **sphereToCs call**: Convert `poly_out[2:3]` to 3D direction via `sphereToCs(poly_out[2], poly_out[3], _outer_pupil_curvature_radius, rdx, rdy, rdz)`. Note: the actual landing uses Option B (CoC warp from `poly_out[0:1]`, same as Thin), but `sphereToCs` must be called for physical completeness and to satisfy R033.
5. **Gather selectivity**: Convert landing position to integer pixel; only accumulate if `ox_land == ox && oy_land == oy`.

## Steps

1. **Read the current stub renderStripe** in `src/DeepCDefocusPORay.cpp` (starts at the `renderStripe` method, ~10 lines that just zero output). Also read the Thin renderStripe in `src/DeepCDefocusPOThin.cpp` (starts around line 290) as the pattern to follow.

2. **Write the gather renderStripe** replacing the stub. Structure:
   - `imagePlane.makeWritable()`, get bounds, chans, format, half_w, half_h
   - `zero_output` lambda (identical to Thin)
   - Poly reload guard for `_poly_sys` (identical to Thin); also reload guard for `_aperture_sys` (same pattern, but with `_aperture_file`, `_aperture_loaded`, `_reload_aperture`, `_aperture_sys`)
   - Early return if `!_poly_loaded || !input0()`
   - Zero output buffer via `zero_output()`
   - Deep channels setup (RGBA + DeepFront + DeepBack)
   - **CoC neighbourhood bound**:
     ```cpp
     const float max_coc_mm = coc_radius(_focal_length_mm, _fstop, _focus_distance, 1.0f);
     const int max_coc_px = static_cast<int>(std::ceil(std::fabs(max_coc_mm) / (_focal_length_mm * 0.5f + 1e-6f) * half_w)) + 1;
     ```
   - **Expanded bounds** for deepEngine:
     ```cpp
     Box expanded = bounds;
     expanded.pad(max_coc_px);
     const Box& in_box = input0()->deepInfo().box();
     expanded.intersect(in_box);
     ```
   - Holdout fetch over `bounds` (not expanded — holdout is evaluated at output pixel)
   - `transmittance_at` lambda (identical to Thin)
   - Deep input fetch via `input0()->deepEngine(expanded, deep_chans, deepPlane)`
   - Scatter constants: `N`, `inv_N`, `ap_radius = _aperture_housing_radius` (NOT `1/fstop` like Thin — Ray uses physical aperture radius)
   - CA table: `{Chan_Red, CA_LAMBDA_R}, {Chan_Green, CA_LAMBDA_G}, {Chan_Blue, CA_LAMBDA_B}`
   - **Gather loop** (outermost: output y, output x; then input neighbourhood ix, iy; then deep samples; then aperture samples k; then CA channels c):
     ```
     for y in [bounds.y, bounds.t):
       for ox in [bounds.x, bounds.r):
         sx_out = (ox + 0.5 - half_w) / half_w
         sy_out = (oy + 0.5 - half_h) / half_h
         for iy in [max(expanded.y, oy-max_coc_px), min(expanded.t, oy+max_coc_px+1)):
           for ix in [max(expanded.x, ox-max_coc_px), min(expanded.r, ox+max_coc_px+1)):
             sx_in = (ix + 0.5 - half_w) / half_w
             sy_in = (iy + 0.5 - half_h) / half_h
             dp = deepPlane.getPixel(iy, ix)
             for each sample s:
               Z, sR, sG, sB, sA from dp
               coc_mm, coc_norm
               for k in [0, N):
                 map_to_disk(halton(k,2), halton(k,3), ax_unit, ay_unit)
                 ax = ax_unit * _aperture_housing_radius
                 ay = ay_unit * _aperture_housing_radius
                 track alpha_out_px, alpha_out_py for green channel
                 for c in {R, G, B}:
                   in5 = {sx_in, sy_in, ax, ay, lambda}
                   // Aperture vignetting
                   if _aperture_loaded:
                     poly_system_evaluate(&_aperture_sys, in5, apt_out, 2, _max_degree)
                     if apt_out magnitude > _aperture_housing_radius: continue
                   // Exitpupil forward trace
                   poly_system_evaluate(&_poly_sys, in5, out5, 5, _max_degree)
                   transmit = clamp(out5[4], 0, 1)
                   // sphereToCs (R033 requirement — call it, use for validation)
                   sphereToCs(out5[2], out5[3], _outer_pupil_curvature_radius, rdx, rdy, rdz)
                   // Option B landing (consistent with Thin)
                   wx, wy = out5[0:1] clamped to _aperture_housing_radius
                   landing_x = sx_in + coc_norm * wx / (_aperture_housing_radius + 1e-6f)
                   landing_y = sy_in + coc_norm * wy / (_aperture_housing_radius + 1e-6f)
                   ox_land = round to int
                   oy_land = round to int
                   if (ox_land != ox || oy_land != oy): continue  // GATHER SELECTIVITY
                   holdout_w = transmittance_at(ox, oy, Z)
                   weight = transmit * holdout_w * inv_N
                   writableAt(ox, oy, chanNo(chan)) += sample_color * weight
                   if c==1: track alpha landing
                 // Alpha at green landing (same as Thin)
     ```

3. **Key differences from Thin's scatter to note**:
   - Thin's outer loop is input pixels; Ray's outer loop is **output pixels** with inner loop over input neighbourhood
   - Thin uses `ap_radius = 1.0f / (_fstop + 1e-6f)` for aperture scaling; Ray uses `_aperture_housing_radius` (physical mm)
   - Ray adds aperture vignetting check via `_aperture_sys` before exitpupil eval
   - Ray adds `sphereToCs` call after exitpupil eval
   - Ray's bounds check is the **gather selectivity guard** (`ox_land == ox`), not the Thin scatter bounds check
   - Ray fetches deep input from `expanded` bounds, not `bounds`

4. **Run syntax check**: `bash scripts/verify-s01-syntax.sh`

5. **Run structural grep checks**:
   ```bash
   grep -q '_aperture_sys' src/DeepCDefocusPORay.cpp
   grep -q 'sphereToCs' src/DeepCDefocusPORay.cpp
   test $(grep -c '_max_degree' src/DeepCDefocusPORay.cpp) -ge 2
   grep -q 'transmittance_at' src/DeepCDefocusPORay.cpp
   grep -q 'halton' src/DeepCDefocusPORay.cpp
   grep -q '0.45f' src/DeepCDefocusPORay.cpp
   grep -qE 'ox_land|oy_land' src/DeepCDefocusPORay.cpp
   ```

## Must-Haves

- [ ] Gather loop structure: outer loop over output pixels, inner loop over input neighbourhood bounded by max_coc_px
- [ ] Expanded deepEngine fetch with `bounds.pad(max_coc_px)` intersected with input format box
- [ ] Aperture vignetting via `_aperture_sys` with `num_out=2` (guarded by `_aperture_loaded`)
- [ ] `sphereToCs` called with `poly_out[2], poly_out[3], _outer_pupil_curvature_radius`
- [ ] `_max_degree` passed to both `poly_system_evaluate` calls
- [ ] Gather selectivity: `if (ox_land != ox || oy_land != oy) continue`
- [ ] Holdout transmittance_at lambda (identical to Thin pattern)
- [ ] Halton + map_to_disk aperture sampling
- [ ] CA per-channel evaluation with 0.45/0.55/0.65 wavelengths
- [ ] Alpha accumulated at green-channel landing position
- [ ] Aperture sample positions scaled by `_aperture_housing_radius` (physical mm), NOT `1/fstop`
- [ ] zero_output lambda and poly reload guard (identical to Thin pattern)
- [ ] Aperture poly reload guard (same pattern as exitpupil, using `_aperture_file`, `_aperture_loaded`, `_reload_aperture`)

## Verification

- `bash scripts/verify-s01-syntax.sh` exits 0
- `grep -q '_aperture_sys' src/DeepCDefocusPORay.cpp` — aperture vignetting present
- `grep -q 'sphereToCs' src/DeepCDefocusPORay.cpp` — ray direction conversion present
- `test $(grep -c '_max_degree' src/DeepCDefocusPORay.cpp) -ge 2` — max_degree in both poly evals
- `grep -q 'transmittance_at' src/DeepCDefocusPORay.cpp` — holdout functional
- `grep -q 'halton' src/DeepCDefocusPORay.cpp` — Halton sampling
- `grep -qE 'ox_land|oy_land' src/DeepCDefocusPORay.cpp` — gather selectivity

## Inputs

- `src/DeepCDefocusPORay.cpp` — S01 scaffold with stub renderStripe to replace
- `src/DeepCDefocusPOThin.cpp` — reference implementation of scatter renderStripe (pattern to follow for boilerplate)
- `src/deepc_po_math.h` — halton, map_to_disk, coc_radius, sphereToCs helpers
- `src/poly.h` — poly_system_evaluate with max_degree parameter

## Expected Output

- `src/DeepCDefocusPORay.cpp` — stub renderStripe replaced with full gather engine (~120 lines)
