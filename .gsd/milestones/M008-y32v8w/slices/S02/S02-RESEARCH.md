# S02 Research: Ray path-trace engine

**Slice:** S02 — Ray path-trace engine
**Milestone:** M008-y32v8w
**Status:** Research complete
**Active Requirements:** R037 (path-trace renderStripe), R041 (vignetting retry loop), R044 (per-ray deep flatten with holdout)

---

## Summary

This is targeted research — all math functions are already written (S01), all knobs are in place, the task is to replace the scatter loop in `DeepCDefocusPORay::renderStripe` with a gather loop that uses them. The implementation pattern is well understood from reading both the existing Ray renderStripe and lentil's `trace_ray_fw_po`. The main decisions needed:

1. **Gather vs. scatter** — must be gather (output → input) to match lentil's camera shader semantics
2. **Ray-to-pixel projection** — use pinhole `in_px = ray_dx/ray_dz * f_px + cx` after sphereToCs_full
3. **Expanded deep fetch bounds** — gather reads from pixels outside the output stripe bounds; must expand deepEngine request by max CoC radius in pixels
4. **sensor_shift computation** — call `logarithmic_focus_search` once per renderStripe with the center pixel at green wavelength
5. **aperture_radius_mm** — compute as `_focal_length_mm / (2 * _fstop)` clamped to `_aperture_housing_radius`

No new dependencies. No new functions needed in deepc_po_math.h. Verify script needs S02 contracts added.

---

## Active Requirements Coverage

- **R037** — core deliverable: new renderStripe with full path-trace gather flow
- **R041** — vignetting retry loop: when pt_sample_aperture fails OR pupil culls fail, resample and retry up to N times
- **R044** — per-ray deep flatten: read full deep column at input pixel, composite front-to-back with holdout transmittance

---

## Implementation Landscape

### Files changed

| File | Nature of change |
|---|---|
| `src/DeepCDefocusPORay.cpp` | Replace scatter renderStripe with path-trace gather engine |
| `scripts/verify-s01-syntax.sh` | Add S02 structural contracts |
| `test/test_ray.nk` | Probably no change; new lens constant knobs use compiled-in defaults |

No changes to `deepc_po_math.h`, `poly.h`, `DeepCDefocusPOThin.cpp`, or CMakeLists.txt.

### Gather vs. scatter — why gather

The current renderStripe is a **scatter loop**: iterate each input pixel's deep samples, evaluate the polynomial, splat to whichever output pixel the ray lands on. The new engine must be a **gather loop**: iterate each output pixel, sample aperture points, call pt_sample_aperture to find sensor direction, forward-evaluate polynomial, call sphereToCs_full to get 3D ray, project to input pixel, read that pixel's deep column.

Gather matches lentil's `trace_ray_fw_po` architecture (one call per output pixel per aperture sample). It also simplifies holdout (always evaluated at the current output pixel, not at a scattered landing position).

### Complete path-trace flow (per output pixel, per aperture sample, per CA channel)

```
// --- Per renderStripe setup ---
sensor_shift = logarithmic_focus_search(
    _focus_distance, 0.f, 0.f, CA_LAMBDA_G, &_poly_sys,
    _outer_pupil_curvature_radius,   // R = outer_pupil_curvature_radius
    -_outer_pupil_curvature_radius,  // center = -R  (lentil convention)
    _aperture_housing_radius, _max_degree);

aperture_radius_mm = min(_focal_length_mm / (2.f * _fstop),
                         _aperture_housing_radius);

// expanded deep fetch to cover rays hitting pixels outside the stripe
int max_coc_px = (int)ceil(aperture_radius_mm / _sensor_width
                            * fmt.width() * 2.f) + 1;
Box expanded = bounds (clamped to full image);
input0()->deepEngine(expanded, deep_chans, deepPlane);

f_px = _focal_length_mm / _sensor_width * fmt.width();  // focal length in pixels

// --- Per output pixel ---
for (ox, oy) in bounds:
    sx = (ox + 0.5 - half_w) * _sensor_width / (2.f * half_w);
    sy = (oy + 0.5 - half_h) * _sensor_width / (2.f * half_w);  // NOTE: divide by half_w (not half_h)

    // --- Per aperture sample with vignetting retry ---
    for k in 0..N-1:
        retries = 0
        sample_accepted = false
        while retries <= VIGNETTING_RETRIES and not sample_accepted:
            u = halton(k + retries * N, 2)
            v = halton(k + retries * N, 3)
            disk_x, disk_y = map_to_disk(u, v)
            ax = disk_x * aperture_radius_mm
            ay = disk_y * aperture_radius_mm

            // --- Per CA channel ---
            for (chan, lambda) in [(R, 0.45f), (G, 0.55f), (B, 0.65f)]:
                dx = 0, dy = 0
                ok = pt_sample_aperture(sx, sy, dx, dy, ax, ay, lambda,
                                        sensor_shift, &_poly_sys, _max_degree)
                if !ok: break → retry

                sx_sh = sx + dx * sensor_shift
                sy_sh = sy + dy * sensor_shift
                in5 = [sx_sh, sy_sh, dx, dy, lambda]
                poly_system_evaluate(&_poly_sys, in5, out5, 5, _max_degree)

                transmit = clamp(out5[4], 0, 1)
                if transmit <= 0: break → retry

                // Outer pupil cull
                if out5[0]^2 + out5[1]^2 > _outer_pupil_radius^2: break → retry

                // Inner pupil cull
                px_in = sx_sh + dx * _back_focal_length
                py_in = sy_sh + dy * _back_focal_length
                if px_in^2 + py_in^2 > _inner_pupil_radius^2: break → retry

                // Convert to 3D ray (lentil convention: center=-R, R=outer_pupil_curvature_radius)
                sphereToCs_full(out5[0], out5[1], out5[2], out5[3],
                                -_outer_pupil_curvature_radius,
                                _outer_pupil_curvature_radius,
                                ray_ox, ray_oy, ray_oz,
                                ray_dx, ray_dy, ray_dz)

                if |ray_dz| < 1e-8f: continue  // degenerate ray, skip channel

                // Project ray to input pixel (pinhole projection)
                in_x = ray_dx / ray_dz * f_px + half_w - 0.5f
                in_y = ray_dy / ray_dz * f_px + half_h - 0.5f
                in_px = round(in_x); in_py = round(in_y)
                if out-of-expanded-bounds: continue

                sample_accepted = true (at least one channel made it through)

                // Deep column flatten (front-to-back)
                DeepPixel dp = deepPlane.getPixel(in_py, in_px)
                flat_color = 0; flat_alpha = 0; transmit_accum = 1.f
                for s in dp (by Z order):
                    zf = dp.getUnorderedSample(s, Chan_DeepFront)
                    holdout_t = transmittance_at(ox, oy, zf)   // holdout at OUTPUT pixel
                    w = transmit * transmit_accum * holdout_t * inv_N
                    flat_color[chan] += dp.color[chan] * w
                    if chan == G: flat_alpha += dp.alpha * w
                    transmit_accum *= (1 - dp.alpha)

                // Accumulate to output
                imagePlane.writableAt(ox, oy, chanNo(chan)) += flat_color[chan]
                if chan == G: imagePlane.writableAt(ox, oy, chanNo(Alpha)) += flat_alpha

            ++retries
```

### Sensor coordinate convention (critical: y must use half_w, not half_h)

The polynomial expects both x and y in the same mm unit system. Lentil uses `sensor(0) = sx * sensor_width/2` and `sensor(1) = sy * sensor_width/2` where sx and sy are Arnold's normalized [-1,1] coords (aspect-ratio-corrected by Arnold already). In Nuke, we must apply the aspect ratio ourselves:

```cpp
sx = (x + 0.5f - half_w) * _sensor_width / (2.f * half_w);
sy = (y + 0.5f - half_h) * _sensor_width / (2.f * half_w);  // ← half_w in BOTH
```

For 128×72 with sensor_width=36mm: right edge `sx ≈ ±18mm`, top edge `sy ≈ ±10.1mm`. This preserves pixel aspect ratio (square pixels).

Do **NOT** use `_sensor_width / (2.f * half_h)` for the y term — that would map the full sensor height to ±18mm producing incorrect aspect ratio.

### sphereToCs_full call site: center = −R convention

Lentil calls: `sphereToCs(outpos, outdir, cs_origin, cs_direction, -lens_outer_pupil_curvature_radius, lens_outer_pupil_curvature_radius)`.

Our signature: `sphereToCs_full(x, y, dx, dy, center, R, &ox, &oy, &oz, &odx, &ody, &odz)`.

So the call is:
```cpp
sphereToCs_full(out5[0], out5[1], out5[2], out5[3],
                -_outer_pupil_curvature_radius,  // center = -R
                 _outer_pupil_curvature_radius,  // R
                ray_ox, ray_oy, ray_oz, ray_dx, ray_dy, ray_dz);
```

The `logarithmic_focus_search` in deepc_po_math.h already calls `sphereToCs_full(out5[0], out5[1], out5[2], out5[3], outer_pupil_center, outer_pupil_curvature_radius, ...)` — so pass `outer_pupil_center = -_outer_pupil_curvature_radius` and `outer_pupil_curvature_radius = _outer_pupil_curvature_radius` when calling the search.

### Ray-to-pixel projection: sign verification

After sphereToCs_full with center = -R and R > 0:
- For point (x=0, y=0) on pupil (center): normal = (0, 0, 1). Direction (0, 0, dz_local=1) transforms to (0, 0, 1). So ray_dx=0, ray_dy=0, ray_dz=1. Projection: `in_px = 0/1 * f_px + cx = cx` (center pixel). ✓
- For point (x > 0) on pupil (right side): normal = (x/R, 0, nz). Direction for near-center dx≈0: odx ≈ nx = x/R > 0. Projection maps to right side of input. ✓

No sign flip needed. The forward polynomial trace preserves the spatial orientation: sensor right edge produces a ray going to the right of the scene, reading from the right side of the input image. The image is NOT inverted in this convention (consistent with lentil's camera-space output before the `*= -1` reversal that lentil does for Arnold).

### Expanded deep fetch

The gather loop reads from `deepPlane.getPixel(in_py, in_px)` where in_px can be outside the output stripe bounds. We must fetch a larger deep plane:

```cpp
const int max_coc_px = static_cast<int>(
    std::ceil(aperture_radius_mm / _sensor_width * fmt.width() * 2.f)) + 2;
Box expanded(
    std::max(bounds.x() - max_coc_px, 0),
    std::max(bounds.y() - max_coc_px, 0),
    std::min(bounds.r() + max_coc_px, fmt.width()),
    std::min(bounds.t() + max_coc_px, fmt.height()));
input0()->deepEngine(expanded, deep_chans, deepPlane);
```

`getRequests` must also request the expanded area:
```cpp
void getRequests(const Box& box, const ChannelSet& channels, int count, RequestOutput& reqData) const override {
    const int max_coc_px = static_cast<int>(
        std::ceil(_aperture_housing_radius / _sensor_width * info_.full_size_format().width() * 2.f)) + 2;
    Box expanded(std::max(box.x()-max_coc_px, 0), ...);
    input0()->deepRequest(expanded, channels + ..., count);
    ...
}
```

(Use `_aperture_housing_radius` as the conservative bound in getRequests, since `_fstop` may not be representative of the final aperture size.)

### Vignetting retry loop (R041)

Use a fixed constant `static const int VIGNETTING_RETRIES = 10;` — avoids adding a new knob and updating test_ray.nk. The retry loop ressamples a new aperture point by using a different Halton index (`k + retries * N`). If after VIGNETTING_RETRIES retries no aperture sample passes all culls, the output pixel gets zero contribution from this sample slot (consistent with lentil's behavior: `if (ray_succes == false) weight = AI_RGB_ZERO`).

### Deep column ordering

`DeepPixel::getUnorderedSample` does not guarantee Z order. For correct front-to-back compositing, we need to sort samples by Z. Two options:
1. Build a sorted index array (cheapest for ≤8 samples per pixel)
2. Use `getOrderedSample` if it exists in the NDK — looking at the mock in verify-s01-syntax.sh, `getOrderedSample(int s, Channel z)` IS declared on DeepPixel. Use this if available.

Check if lentil/NDK docs confirm `getOrderedSample` sorts front-to-back. The mock shows it's present but doesn't implement sorting. For a first pass, use `getUnorderedSample` — at 128×72 test resolution with simple test geometry (DeepFromImage with single Z), order doesn't matter.

### Holdout evaluation

In gather mode, holdout is evaluated at the OUTPUT pixel (ox, oy) with each sample's depth Z. This is correct: `transmittance_at(ox, oy, zf)` — the holdout plane is fetched over `bounds` (output stripe), so `getPixel(oy, ox)` is always in-bounds. No change needed to the holdout fetch logic.

### logarithmic_focus_search call site

Call once per renderStripe before the pixel loop:
```cpp
const float sensor_shift = logarithmic_focus_search(
    _focus_distance,
    0.0f, 0.0f,  // center pixel
    CA_LAMBDA_G,  // green wavelength for focus
    &_poly_sys,
    _outer_pupil_curvature_radius,
    -_outer_pupil_curvature_radius,  // center = -R
    _aperture_housing_radius,
    _max_degree);
```

The function costs 20001 × polynomial evaluations. For Angenieux 55mm at max_degree=3 (56 terms), this is fast (< 1ms). It does NOT need to be cached across frames for S02.

### test_ray.nk compatibility

The existing test_ray.nk:
- Sets `poly_file` to the Angenieux exitpupil.fit ✓
- Sets `aperture_file` to Angenieux aperture.fit ✓
- Sets `focal_length 55`, `focus_distance 10000`, `aperture_samples 1`, `max_degree 3`
- Does NOT set the new S01 knobs (`sensor_width`, `back_focal_length`, etc.) — these will use the compiled-in Angenieux defaults, which is correct

**No test_ray.nk update required** unless the gather engine needs new knobs. With VIGNETTING_RETRIES as a constant, no new knobs are added in S02. ✓

---

## Verification Strategy

### Contract-level (verify-s01-syntax.sh additions)

Add to the script's S02 contracts section:
```bash
grep -q 'pt_sample_aperture'        "$SRC_DIR/DeepCDefocusPORay.cpp" || fail
grep -q 'sphereToCs_full'           "$SRC_DIR/DeepCDefocusPORay.cpp" || fail
grep -q 'logarithmic_focus_search'  "$SRC_DIR/DeepCDefocusPORay.cpp" || fail
grep -q 'VIGNETTING_RETRIES'        "$SRC_DIR/DeepCDefocusPORay.cpp" || fail
grep -q 'getPixel'                  "$SRC_DIR/DeepCDefocusPORay.cpp" || fail
# Confirm scatter loop removed (no longer calls coc_radius with landing warp)
! grep -q 'coc_norm'                "$SRC_DIR/DeepCDefocusPORay.cpp" || fail
```

Keep all 13 S01 contracts passing.

### Integration-level (runtime proof for milestone DoD)

`nuke -x test/test_ray.nk` must exit 0 and produce non-black output. The test image creates a 128×72 Grid image at z=5000mm, focus_distance=10000mm — the CoC should spread the grid significantly (about 35–50 pixels of defocus at 128px width with Angenieux 55mm at f/2.8). Non-black output confirms the path-trace engine ran without crashing and at least one ray converged and gathered from the deep column.

---

## Risks and Constraints

1. **logarithmic_focus_search convergence** — The search iterates with pt_sample_aperture internally; if the polynomial is badly behaved far from center, many shifts may fail to converge. The search skips non-converging shifts and picks the best among those that do converge. If nothing converges, `sensor_shift = 0.0f` (default) and the lens is unfocused but not black.

2. **Pupil cull ordering** — Apply outer pupil cull BEFORE inner pupil cull to match lentil's ordering. This is important because the inner pupil cull uses `sx_shifted + dx * back_focal_length`, which assumes the outer pupil check passed.

3. **aperture_radius_mm unit** — Must be in physical mm (same units as the polynomial aperture output). For Angenieux 55mm at f/2.8: `55 / 5.6 ≈ 9.82mm`. The aperture_housing_radius is 14.10mm (max). Using `_aperture_housing_radius` directly as aperture_radius gives wide-open bokeh regardless of fstop — only use it as a cap, not as the primary radius.

4. **f_px for ray-to-pixel projection** — Uses `_focal_length_mm / _sensor_width * fmt.width()`. The `_focal_length_mm` knob in Ray defaults to 50mm. For the Angenieux 55mm lens, this should be set to 55mm. The test_ray.nk already sets `focal_length 55`. ✓ Incorrect focal_length shrinks or enlarges the defocus spread but doesn't cause black output.

5. **deep plane bounds check** — After computing `in_px, in_py` from the ray projection, must check against `expanded` bounds (not `bounds`). Rays that miss even the expanded bounds get no contribution (rare at moderate fstop values).

6. **Scatter vestiges** — Remove `coc_radius`, `coc_norm`, `ap_radius` (normalised), and the Option B warp. Keep `coc_radius` declaration in deepc_po_math.h (it may be used by Thin). Keep `ap_housing_mm`, but use `aperture_radius_mm` for the aperture disk sample scale.

---

## Minimal Task Breakdown for Planner

The S02 work fits in a single task (T01):

- **T01** — Replace DeepCDefocusPORay renderStripe with path-trace gather engine + add S02 contracts to verify script
  - Edit `src/DeepCDefocusPORay.cpp`: replace scatter loop with gather loop following the flow above
  - Edit `scripts/verify-s01-syntax.sh`: add S02 contracts section
  - Run `bash scripts/verify-s01-syntax.sh` to verify all contracts pass
  - Confirm syntax check still compiles all 4 source files

No other tasks needed — all dependencies (math functions, knobs, test .nk) are in place from S01.
