---
estimated_steps: 5
estimated_files: 2
skills_used: []
---

# T01: Replace Ray scatter engine with path-trace gather loop + add S02 contracts

**Slice:** S02 — Ray path-trace engine
**Milestone:** M008-y32v8w

## Description

Replace the scatter renderStripe in `DeepCDefocusPORay.cpp` with a gather path-trace engine that traces real polynomial-optics rays per output pixel. This is R037 — the core deliverable of S02. Also update `getRequests` for expanded deep bounds, and add S02 structural contracts to the verify script.

The existing scatter loop (lines ~516–650) iterates input pixels, computes thin-lens CoC, and splats to output pixels using an Option B warp. The new gather loop iterates output pixels, converts to sensor mm coordinates, samples aperture points, uses Newton aperture matching (pt_sample_aperture), forward polynomial evaluation, outer/inner pupil culls, sphereToCs_full for 3D ray direction, pinhole projection back to input pixel, reads the deep column at that input pixel, and composites front-to-back with holdout transmittance.

## Steps

1. **Update `getRequests` for expanded deep bounds.** Replace the current `input0()->deepRequest(box, ...)` with an expanded box computed from `_aperture_housing_radius / _sensor_width * info_.full_size_format().width() * 2 + 2` pixels of padding on each side, clamped to image bounds. This ensures the gather engine can read deep pixels outside the output stripe.

2. **Replace the scatter loop in `renderStripe` with the gather path-trace engine.** Delete the scatter constants section (`ap_radius`, `coc_norm`/`coc_radius` usage, Option B warp), the scatter loop, and alpha scatter. Replace with the gather engine following this exact flow:

   **Per-renderStripe setup:**
   ```cpp
   // Compute sensor_shift for focus
   const float sensor_shift = logarithmic_focus_search(
       _focus_distance, 0.0f, 0.0f, CA_LAMBDA_G, &_poly_sys,
       _outer_pupil_curvature_radius, -_outer_pupil_curvature_radius,
       _aperture_housing_radius, _max_degree);
   
   // Physical aperture radius (clamped to housing)
   const float aperture_radius_mm = std::min(
       _focal_length_mm / (2.0f * _fstop + 1e-6f),
       _aperture_housing_radius);
   
   // Focal length in pixels for ray-to-pixel projection
   const float f_px = _focal_length_mm / _sensor_width * fmt.width();
   
   // Expanded deep fetch
   const int max_coc_px = static_cast<int>(
       std::ceil(aperture_radius_mm / _sensor_width * fmt.width() * 2.0f)) + 2;
   Box expanded(
       std::max(bounds.x() - max_coc_px, 0),
       std::max(bounds.y() - max_coc_px, 0),
       std::min(bounds.r() + max_coc_px, fmt.width()),
       std::min(bounds.t() + max_coc_px, fmt.height()));
   // Deep fetch uses expanded box instead of bounds
   input0()->deepEngine(expanded, deep_chans, deepPlane);
   
   static const int VIGNETTING_RETRIES = 10;
   const int N = std::max(_aperture_samples, 1);
   const float inv_N = 1.0f / static_cast<float>(N);
   ```

   **Per output pixel (ox, oy):**
   ```cpp
   // Sensor position in mm — CRITICAL: y uses half_w (not half_h) for aspect ratio
   const float sx = (ox + 0.5f - half_w) * _sensor_width / (2.0f * half_w);
   const float sy = (oy + 0.5f - half_h) * _sensor_width / (2.0f * half_w);
   ```

   **Per aperture sample with vignetting retry:**
   ```cpp
   for (int k = 0; k < N; ++k) {
       bool sample_accepted = false;
       for (int retry = 0; retry <= VIGNETTING_RETRIES && !sample_accepted; ++retry) {
           const int idx = k + retry * N;
           float disk_x, disk_y;
           map_to_disk(halton(idx, 2), halton(idx, 3), disk_x, disk_y);
           const float ax = disk_x * aperture_radius_mm;
           const float ay = disk_y * aperture_radius_mm;
           // ... per-CA-channel trace ...
       }
   }
   ```

   **Per CA channel (R=0.45, G=0.55, B=0.65):**
   ```cpp
   float dx = 0.0f, dy = 0.0f;
   bool ok = pt_sample_aperture(sx, sy, dx, dy, ax, ay, lambda,
                                sensor_shift, &_poly_sys, _max_degree);
   if (!ok) break;  // retry
   
   // Forward evaluate
   const float sx_sh = sx + dx * sensor_shift;
   const float sy_sh = sy + dy * sensor_shift;
   float in5[5] = { sx_sh, sy_sh, dx, dy, lambda };
   float out5[5] = {};
   poly_system_evaluate(&_poly_sys, in5, out5, 5, _max_degree);
   
   float transmit = std::max(0.0f, std::min(1.0f, out5[4]));
   if (transmit <= 0.0f) break;  // retry
   
   // Outer pupil cull
   if (out5[0]*out5[0] + out5[1]*out5[1] > _outer_pupil_radius*_outer_pupil_radius) break;
   
   // Inner pupil cull
   float px_in = sx_sh + dx * _back_focal_length;
   float py_in = sy_sh + dy * _back_focal_length;
   if (px_in*px_in + py_in*py_in > _inner_pupil_radius*_inner_pupil_radius) break;
   
   // 3D ray via sphereToCs_full (center = -R convention)
   float ray_ox, ray_oy, ray_oz, ray_dx, ray_dy, ray_dz;
   sphereToCs_full(out5[0], out5[1], out5[2], out5[3],
                   -_outer_pupil_curvature_radius, _outer_pupil_curvature_radius,
                   ray_ox, ray_oy, ray_oz, ray_dx, ray_dy, ray_dz);
   
   if (std::fabs(ray_dz) < 1e-8f) continue;  // degenerate, skip channel
   
   // Pinhole projection to input pixel
   const float in_x = ray_dx / ray_dz * f_px + half_w - 0.5f;
   const float in_y = ray_dy / ray_dz * f_px + half_h - 0.5f;
   const int in_px = static_cast<int>(std::round(in_x));
   const int in_py = static_cast<int>(std::round(in_y));
   if (in_px < expanded.x() || in_px >= expanded.r() ||
       in_py < expanded.y() || in_py >= expanded.t()) continue;
   
   sample_accepted = true;
   
   // Deep column flatten (front-to-back)
   DeepPixel dp_in = deepPlane.getPixel(in_py, in_px);
   float flat_color = 0.0f, flat_alpha = 0.0f, transmit_accum = 1.0f;
   for (int ds = 0; ds < dp_in.getSampleCount(); ++ds) {
       float zf = dp_in.getUnorderedSample(ds, Chan_DeepFront);
       float holdout_t = transmittance_at(ox, oy, zf);  // holdout at OUTPUT pixel
       float w = transmit * transmit_accum * holdout_t * inv_N;
       float sc = dp_in.getUnorderedSample(ds, chan);
       flat_color += sc * w;
       if (chan == Chan_Green) {
           flat_alpha += dp_in.getUnorderedSample(ds, Chan_Alpha) * w;
       }
       transmit_accum *= (1.0f - dp_in.getUnorderedSample(ds, Chan_Alpha));
   }
   
   imagePlane.writableAt(ox, oy, imagePlane.chanNo(chan)) += flat_color;
   if (chan == Chan_Green) {
       imagePlane.writableAt(ox, oy, imagePlane.chanNo(Chan_Alpha)) += flat_alpha;
   }
   ```

   **Key constraints:**
   - `sy` must divide by `half_w` (not `half_h`) — preserves aspect ratio in mm coords
   - `sphereToCs_full` center argument = `-_outer_pupil_curvature_radius` (negative R)
   - Outer pupil cull BEFORE inner pupil cull (lentil ordering)
   - Holdout evaluated at output pixel (ox, oy), not input pixel
   - Remove all scatter vestiges: `coc_radius` call, `coc_norm`, `ap_radius` normalised, Option B warp code
   - Keep `ca_table` struct and `ChanWave` for CA channels

3. **Remove scatter vestiges.** Delete: the `coc_norm`/`coc_radius` computation, the normalised `ap_radius`, the `Option B landing` section, the old alpha scatter section, and the per-input-pixel outer loops. The `coc_radius` function remains in `deepc_po_math.h` (used by Thin) but must not appear in Ray's renderStripe.

4. **Add S02 contracts to `scripts/verify-s01-syntax.sh`.** After the S01 contracts section, add:
   ```bash
   echo "--- S02 contracts ---"
   grep -q 'pt_sample_aperture'        "$SRC_DIR/DeepCDefocusPORay.cpp" || { echo "FAIL: pt_sample_aperture call missing from Ray renderStripe"; exit 1; }
   echo "PASS: pt_sample_aperture called in Ray"
   grep -q 'sphereToCs_full'           "$SRC_DIR/DeepCDefocusPORay.cpp" || { echo "FAIL: sphereToCs_full call missing from Ray renderStripe"; exit 1; }
   echo "PASS: sphereToCs_full called in Ray"
   grep -q 'logarithmic_focus_search'  "$SRC_DIR/DeepCDefocusPORay.cpp" || { echo "FAIL: logarithmic_focus_search call missing from Ray renderStripe"; exit 1; }
   echo "PASS: logarithmic_focus_search called in Ray"
   grep -q 'VIGNETTING_RETRIES'        "$SRC_DIR/DeepCDefocusPORay.cpp" || { echo "FAIL: VIGNETTING_RETRIES missing from Ray"; exit 1; }
   echo "PASS: VIGNETTING_RETRIES in Ray"
   grep -q 'getPixel'                  "$SRC_DIR/DeepCDefocusPORay.cpp" || { echo "FAIL: deep getPixel missing from Ray renderStripe"; exit 1; }
   echo "PASS: getPixel in Ray"
   ! grep -q 'coc_norm'               "$SRC_DIR/DeepCDefocusPORay.cpp" || { echo "FAIL: scatter vestige coc_norm still in Ray"; exit 1; }
   echo "PASS: coc_norm removed from Ray"
   echo "All S02 contracts passed."
   ```

5. **Run verification.** Execute `bash scripts/verify-s01-syntax.sh` and confirm all S01 contracts + all S02 contracts pass, and all 4 source files compile cleanly.

## Must-Haves

- [ ] renderStripe is a gather loop (output pixel → input pixel), not scatter
- [ ] sensor_shift computed via logarithmic_focus_search per renderStripe
- [ ] aperture_radius_mm = focal_length / (2 * fstop), clamped to housing
- [ ] sy uses half_w divisor (not half_h) for aspect-ratio-correct mm coords
- [ ] pt_sample_aperture called for Newton aperture matching
- [ ] sphereToCs_full called with center = -R convention
- [ ] Outer pupil cull before inner pupil cull
- [ ] Vignetting retry loop with VIGNETTING_RETRIES constant
- [ ] Per-ray deep column flatten with front-to-back compositing
- [ ] Holdout evaluated at output pixel position
- [ ] getRequests expanded by max CoC pixels
- [ ] Deep fetch in renderStripe uses expanded bounds
- [ ] CA wavelength tracing preserved (R/G/B channels)
- [ ] All scatter vestiges removed (coc_norm, Option B warp)
- [ ] S02 contracts added to verify script
- [ ] All S01 contracts still pass
- [ ] All 4 source files compile via verify script

## Verification

- `bash scripts/verify-s01-syntax.sh` exits 0
- Output includes "All S01 contracts passed." and "All S02 contracts passed."
- Output includes compilation success for all 4 source files
- `grep -q 'coc_norm' src/DeepCDefocusPORay.cpp` returns non-zero (vestige removed)

## Observability Impact

- **New compile-time contracts**: S02 section added to verify-s01-syntax.sh provides structural grep checks for all key gather-engine functions and absence of scatter vestiges.
- **Failure state visible**: Vignetted rays (all retries exhausted) contribute zero weight — visible as expected field-edge darkening. Degenerate rays (ray_dz ≈ 0) are silently skipped per channel, not per sample.
- **Inspection**: Future agents can verify gather engine presence by running `bash scripts/verify-s01-syntax.sh` — all S02 contracts must pass. The `coc_norm` negative grep confirms scatter removal.
- **No new runtime logging**: This is a Nuke plugin compiled against the NDK — runtime observability is via Nuke viewer output and the structural contracts.

## Inputs

- `src/DeepCDefocusPORay.cpp` — existing scatter renderStripe to replace; contains all class members, knobs, _validate, getRequests
- `src/deepc_po_math.h` — S01 math functions consumed: pt_sample_aperture, sphereToCs_full, logarithmic_focus_search, halton, map_to_disk, coc_radius, CA_LAMBDA_R/G/B
- `scripts/verify-s01-syntax.sh` — existing S01 contract script to extend with S02 contracts

## Expected Output

- `src/DeepCDefocusPORay.cpp` — renderStripe replaced with gather path-trace engine, getRequests expanded
- `scripts/verify-s01-syntax.sh` — S02 contracts section added
