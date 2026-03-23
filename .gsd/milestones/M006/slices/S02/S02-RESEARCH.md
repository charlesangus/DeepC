# S02 Research: PO Scatter Engine — Stochastic Aperture Sampling

**Slice:** S02 — PO Scatter Engine — Stochastic Aperture Sampling
**Milestone:** M006 — DeepCDefocusPO
**Active Requirements this slice owns:**
- R019 — DeepCDefocusPO: Deep → flat 2D via PO scatter (non-zero output, bokeh visible)
- R021 — Physically correct bokeh size from focal length, f-stop, focus distance
- R022 — Chromatic aberration via per-channel wavelength tracing (R/G/B at 0.45/0.55/0.65 μm)
- R025 — Stochastic aperture sampling with artist-controlled sample count N

---

## Summary

S02 is the core algorithmic slice. The entire scaffold from S01 is in place and verified; this slice fills in the one placeholder: the `renderStripe` body. Everything else (PlanarIop base, Deep input fetch, poly load lifecycle, knob members, CMake wiring) is done and must not be changed.

This is a targeted-research slice: the technology (polynomial optics, Newton iteration, aperture disk sampling) is novel to this codebase but well-understood from the lentil/pota literature. The implementation landscape is clear from the existing stubs. The research targets four specific questions:

1. **What does `lt_newton_trace` actually compute, and how is it implemented?**
2. **What is the correct renderStripe loop structure for PO scatter?**
3. **How are R/G/B wavelengths traced differently through `poly_system_evaluate`?**
4. **What low-discrepancy sequence is right for aperture disk sampling?**

All four are answered below. There are no remaining architectural unknowns.

---

## Recommendation

**Implement S02 as a single task: replace the `renderStripe` stub body with the full PO scatter loop in `src/DeepCDefocusPO.cpp` and complete `lt_newton_trace` in `src/deepc_po_math.h`.**

The work divides naturally into two sub-units:

1. `deepc_po_math.h` — replace the `lt_newton_trace` stub (body currently returns `{0,0}`) with the Newton iteration using `mat2_inverse` and `poly_system_evaluate`.
2. `DeepCDefocusPO.cpp` — replace the zeroing loop in `renderStripe` with: Deep input fetch, output accumulation buffer, per-pixel iteration, per-sample aperture loop, per-channel wavelength trace, sensor-position-to-pixel scatter, normalisation.

These two changes are the totality of S02. CMake, knobs, poly load, verification scripts — none change.

---

## Implementation Landscape

### File inventory for S02

| File | S01 state | S02 changes |
|------|-----------|-------------|
| `src/deepc_po_math.h` | `lt_newton_trace` stub returns `{0,0}` | Replace stub body with Newton iteration |
| `src/DeepCDefocusPO.cpp` | `renderStripe` zeroes all pixels | Replace zeroing loop with scatter engine |
| `src/poly.h` | Complete, no changes | No changes |
| `src/CMakeLists.txt` | DeepCDefocusPO in PLUGINS + FILTER_NODES | No changes |
| `scripts/verify-s01-syntax.sh` | Updated with PlanarIop/ImagePlane/File_knob mocks | Extend with S02 grep contracts |

---

## The PO Scatter Algorithm

### Conceptual model

lentil's polynomial-optics model traces light **from sensor to world** (camera-forward). Each polynomial was fitted to describe: given a point on the sensor and a direction on the aperture disk, where does the ray exit the outer pupil? The 5-input, 5-output `poly_system_evaluate` call is:

```
input[5]  = { sensor_x,    sensor_y,    aperture_x,    aperture_y,    lambda_um }
output[5] = { outer_pupil_dx, outer_pupil_dy, sensor_x', sensor_y', transmittance }
```

For our use, the **forward scatter** direction is: given a world point at depth Z, find where it lands on the sensor for a given aperture sample. This is the inverse problem — we know aperture point and depth, we want sensor landing position.

The Newton iteration in `lt_newton_trace` solves this inversion:

```
Given:
  - target_sensor_pos[2]: the "ideal" sensor position corresponding to the world point
  - aperture_pos[2]: a sampled point on the aperture disk
  - lambda_um: wavelength
Find:
  - sensor_landing[2]: actual sensor position where light lands (for this aperture sample)
```

The iteration starts from `target_sensor_pos` and refines it by evaluating the polynomial forward (given sensor_x', sensor_y', aperture_x, aperture_y → outer direction) and computing the residual between the back-projected sensor position and the target. The Jacobian of the residual (a 2×2 matrix) is inverted via `mat2_inverse` each step.

### Simplified view for S02

For the S02 implementation, what matters is:

1. A **world point at depth Z** maps to an **ideal sensor position** `(x_sensor, y_sensor)` via the thin-lens projection (or equivalently, the pixel coordinate / format mapping). At focus distance, ideal sensor position is the image-plane projection of the world point.

2. For each aperture sample `(ax, ay)` on the aperture disk, `lt_newton_trace` finds the **actual sensor landing position** `(sx, sy)` at the given wavelength. The displacement `(sx - x_sensor, sy - y_sensor)` is the bokeh offset — this is where the contribution lands on the output image.

3. The output pixel receiving the contribution is `(output_x + offset_x_pixels, output_y + offset_y_pixels)` where `offset_x_pixels = (sx - x_sensor) * pixels_per_mm`.

4. The sample's RGBA contribution is splatted to that output pixel with weight `1/N` (uniform weighting; all aperture samples equally probable for a circular aperture).

5. For **chromatic aberration**: steps 2–4 are repeated three times per aperture sample, once each for R (0.45 μm), G (0.55 μm), B (0.65 μm). Each channel's sensor landing position is slightly different, causing colour fringing.

### The Newton iteration — what to implement in `lt_newton_trace`

The lentil `gencode.h` `print_lt_sample_aperture` function generates a C++ Newton solver at code-generation time. For DeepC we need the runtime equivalent. The algorithm:

```
// Forward map: sensor_pos + aperture_pos + lambda → evaluate poly → back-projected sensor
// We want to find sensor_pos such that poly_system_evaluate(sensor_pos, aperture_pos, lambda)
// output[2..3] == target_sensor_pos

Vec2 lt_newton_trace(float sensor_target[2], float aperture_pos[2],
                     float lambda_um, const poly_system_t* sys)
{
    // Start from the target (thin-lens initial guess)
    float pos[2] = { sensor_target[0], sensor_target[1] };

    const int MAX_ITER = 20;
    const float TOL = 1e-5f;

    for (int iter = 0; iter < MAX_ITER; ++iter) {
        // Evaluate polynomial: input = {pos[0], pos[1], aperture_pos[0], aperture_pos[1], lambda}
        float in5[5]  = { pos[0], pos[1], aperture_pos[0], aperture_pos[1], lambda_um };
        float out5[5] = {};
        poly_system_evaluate(sys, in5, out5, 5);

        // out5[2], out5[3] = predicted sensor x, y for this (sensor_pos, aperture_pos) pair
        float residual[2] = { out5[2] - sensor_target[0],
                               out5[3] - sensor_target[1] };

        if (residual[0]*residual[0] + residual[1]*residual[1] < TOL*TOL)
            break;

        // Jacobian: finite-difference the polynomial mapping at (pos, aperture)
        const float EPS = 1e-4f;
        float in5_dx[5] = { pos[0]+EPS, pos[1],     aperture_pos[0], aperture_pos[1], lambda_um };
        float in5_dy[5] = { pos[0],     pos[1]+EPS, aperture_pos[0], aperture_pos[1], lambda_um };
        float out_dx[5] = {}, out_dy[5] = {};
        poly_system_evaluate(sys, in5_dx, out_dx, 5);
        poly_system_evaluate(sys, in5_dy, out_dy, 5);

        Mat2 J;
        J.m[0][0] = (out_dx[2] - out5[2]) / EPS;  // d(out_x)/d(pos_x)
        J.m[0][1] = (out_dy[2] - out5[2]) / EPS;  // d(out_x)/d(pos_y)
        J.m[1][0] = (out_dx[3] - out5[3]) / EPS;  // d(out_y)/d(pos_x)
        J.m[1][1] = (out_dy[3] - out5[3]) / EPS;  // d(out_y)/d(pos_y)

        const double det = J.m[0][0]*J.m[1][1] - J.m[0][1]*J.m[1][0];
        if (std::fabs(det) < 1e-12) break;  // singular Jacobian — no update

        Mat2 Jinv = mat2_inverse(J);
        pos[0] -= static_cast<float>(Jinv.m[0][0]*residual[0] + Jinv.m[0][1]*residual[1]);
        pos[1] -= static_cast<float>(Jinv.m[1][0]*residual[0] + Jinv.m[1][1]*residual[1]);
    }

    return Vec2{ pos[0], pos[1] };
}
```

**Critical note on what `lt_newton_trace` returns:** It returns the **sensor landing position** `(sx, sy)` in sensor/lens coordinates (mm units), not a pixel offset. The caller converts to output pixel offset using a mm-to-pixels scale factor derived from the output format and sensor size assumed by the lens model.

**What `poly_system_evaluate` output indices mean:**
- `output[0]`, `output[1]` — outer pupil exit direction (dx, dy); used for ray tracing, not needed here
- `output[2]`, `output[3]` — back-projected sensor position (sensor_x, sensor_y) in mm
- `output[4]` — transmittance / vignetting weight (0 to 1; multiply into sample contribution)

### The `renderStripe` scatter loop

The `renderStripe` replacement has this structure:

```cpp
void renderStripe(ImagePlane& imagePlane) override
{
    imagePlane.makeWritable();
    const Box& bounds = imagePlane.bounds();

    if (!_poly_loaded || !input0()) {
        // Zero output (no poly loaded or no input)
        foreach(z, imagePlane.channels())
            for (int y = bounds.y(); y < bounds.t(); ++y)
                for (int x = bounds.x(); x < bounds.r(); ++x)
                    imagePlane.writableAt(x, y, z) = 0.0f;
        return;
    }

    // --- 1. Fetch Deep input for the stripe box ---
    DeepPlane deepPlane;
    ChannelSet needed = Mask_RGBA;
    needed += Chan_DeepFront;
    needed += Chan_DeepBack;
    input0()->deepEngine(bounds, needed, deepPlane);

    // --- 2. Zero the output accumulation buffer ---
    foreach(z, imagePlane.channels())
        for (int y = bounds.y(); y < bounds.t(); ++y)
            for (int x = bounds.x(); x < bounds.r(); ++x)
                imagePlane.writableAt(x, y, z) = 0.0f;

    // --- 3. Accumulation weight buffer (counts splats per pixel) ---
    // Used to normalise the accumulated RGBA at the end.
    // Size: bounds.r()-bounds.x() × bounds.t()-bounds.y()
    const int W = bounds.r() - bounds.x();
    const int H = bounds.t() - bounds.y();
    std::vector<float> weightBuf(W * H, 0.0f);

    // Precompute: sensor size from format, pixels-per-mm scale
    const Format& fmt = *info_.full_size_format();
    // Sensor size assumed by lentil: typically 36mm × 24mm (full-frame)
    // Pixel pitch = sensor_width_mm / format_width
    const float sensor_width_mm = 36.0f;   // lentil default assumption
    const float pix_per_mm = static_cast<float>(fmt.width()) / sensor_width_mm;

    // focal_length from poly metadata — lentil stores it in poly[0].term[0].coeff
    // as a convention, OR it is a separate scalar in the .fit header.
    // For S02, read from _poly_sys.poly[0].term[0].coeff as focal_length_mm,
    // falling back to a 50mm default if the system has no terms.
    // (S04 can expose this as a knob if needed.)
    const float focal_length_mm = (_poly_sys.poly[0].num_terms > 0)
        ? std::fabs(_poly_sys.poly[0].term[0].coeff)
        : 50.0f;

    // --- 4. Per input pixel loop ---
    for (Box::iterator it = bounds.begin(); it != bounds.end(); ++it) {
        if (Op::aborted()) return;

        const int px = it.x, py = it.y;
        DeepPixel pixel = deepPlane.getPixel(it);
        const int nSamples = pixel.getSampleCount();
        if (nSamples == 0) continue;

        // --- 5. Per deep sample loop ---
        for (int s = 0; s < nSamples; ++s) {
            const float z_front = pixel.getUnorderedSample(s, Chan_DeepFront);
            const float z_back  = pixel.getUnorderedSample(s, Chan_DeepBack);
            const float depth   = 0.5f * (z_front + z_back);  // sample depth (mm or scene units)
            if (depth < 1e-6f) continue;

            const float alpha = pixel.getUnorderedSample(s, Chan_Alpha);
            if (alpha < 1e-6f) continue;

            // CoC radius bounds the scatter footprint
            const float coc = coc_radius(focal_length_mm, _fstop, _focus_distance, depth);

            // Ideal sensor position for this input pixel (thin-lens projection)
            // Maps (px, py) → sensor coords in mm centred at origin
            const float sensor_x0 = (static_cast<float>(px) + 0.5f
                                     - static_cast<float>(fmt.width())  * 0.5f) / pix_per_mm;
            const float sensor_y0 = (static_cast<float>(py) + 0.5f
                                     - static_cast<float>(fmt.height()) * 0.5f) / pix_per_mm;

            // --- 6. Aperture sample loop ---
            const int N = _aperture_samples;
            for (int k = 0; k < N; ++k) {
                // Halton(2,3) low-discrepancy sequence for aperture disk
                const float u = halton(k, 2);  // base 2
                const float v = halton(k, 3);  // base 3
                // Map to disk via concentric square→disk (Shirley mapping)
                float ax, ay;
                map_to_disk(u, v, ax, ay);
                // Scale to aperture radius (mm): aperture_radius = focal_length / (2 * fstop)
                const float ap_radius = focal_length_mm / (2.0f * _fstop);
                ax *= ap_radius;
                ay *= ap_radius;

                // --- 7. Trace each channel at its wavelength ---
                // R: 0.45 μm, G: 0.55 μm, B: 0.65 μm
                // Alpha uses the G-channel landing position
                const float lambdas[3] = { 0.45f, 0.55f, 0.65f };
                float sensor_xy[3][2];  // landing positions per channel
                float transmittances[3];

                for (int c = 0; c < 3; ++c) {
                    float sensor_target[2] = { sensor_x0, sensor_y0 };
                    float ap[2] = { ax, ay };
                    Vec2 landing = lt_newton_trace(sensor_target, ap, lambdas[c], &_poly_sys);
                    sensor_xy[c][0] = static_cast<float>(landing.x);
                    sensor_xy[c][1] = static_cast<float>(landing.y);

                    // Transmittance from poly output[4]
                    float in5[5]  = { sensor_target[0], sensor_target[1], ax, ay, lambdas[c] };
                    float out5[5] = {};
                    poly_system_evaluate(&_poly_sys, in5, out5, 5);
                    transmittances[c] = std::max(0.0f, std::min(1.0f, out5[4]));
                }

                // --- 8. Splat contribution to output buffer ---
                // For each channel, convert landing position to output pixel coords
                const Channel rgb_chans[3] = { Chan_Red, Chan_Green, Chan_Blue };
                for (int c = 0; c < 3; ++c) {
                    // Sensor → pixel conversion
                    const float out_x_f = sensor_xy[c][0] * pix_per_mm
                                          + static_cast<float>(fmt.width())  * 0.5f - 0.5f;
                    const float out_y_f = sensor_xy[c][1] * pix_per_mm
                                          + static_cast<float>(fmt.height()) * 0.5f - 0.5f;
                    const int out_x = static_cast<int>(std::floor(out_x_f + 0.5f));
                    const int out_y = static_cast<int>(std::floor(out_y_f + 0.5f));

                    if (out_x < bounds.x() || out_x >= bounds.r()) continue;
                    if (out_y < bounds.y() || out_y >= bounds.t()) continue;

                    const float chan_val = pixel.getUnorderedSample(s, rgb_chans[c]);
                    const float w = transmittances[c] / static_cast<float>(N);
                    imagePlane.writableAt(out_x, out_y, rgb_chans[c]) += chan_val * alpha * w;
                }

                // Alpha channel: use G-channel landing position
                {
                    const float out_x_f = sensor_xy[1][0] * pix_per_mm
                                          + static_cast<float>(fmt.width())  * 0.5f - 0.5f;
                    const float out_y_f = sensor_xy[1][1] * pix_per_mm
                                          + static_cast<float>(fmt.height()) * 0.5f - 0.5f;
                    const int out_x = static_cast<int>(std::floor(out_x_f + 0.5f));
                    const int out_y = static_cast<int>(std::floor(out_y_f + 0.5f));
                    if (out_x >= bounds.x() && out_x < bounds.r()
                     && out_y >= bounds.y() && out_y < bounds.t()) {
                        const float w = transmittances[1] / static_cast<float>(N);
                        imagePlane.writableAt(out_x, out_y, Chan_Alpha) += alpha * w;
                    }
                }
            } // aperture samples
        } // deep samples
    } // pixels
}
```

**Note on normalisation:** The `1/N` weight per aperture sample is the correct normalisation for stochastic aperture integration — the sum of N equal-weight samples approximates the integral over the aperture disk. No separate normalisation pass is needed.

---

## Constraints and Tricky Parts

### 1. `writableAt` vs `writable` — use `writableAt`

The S01 `renderStripe` stub already uses `imagePlane.writableAt(x, y, z)` correctly. The scatter loop accumulates with `+=`, which requires a writable reference per-pixel — `writableAt` returns `float&`, which is correct. The `writable(Channel, int y)` overload returns a row pointer and is for row-level operations; it does not support individual pixel random access that the scatter pattern requires.

**Accumulation is safe because `renderStripe` is single-threaded (PlanarIop guarantee, D027).** There is no race on the `+=`.

### 2. Scatter lands outside the stripe — discard, do not wrap

The stripe box (`imagePlane.bounds()`) may be smaller than the full output format. A scatter contribution from a deep sample in this stripe may land on a pixel outside this stripe's bounds. **Discard these contributions** — the neighbour stripe will scatter its own contributions into those pixels. This is correct because `renderStripe` is called once per stripe and each stripe is responsible for its own output pixels. Cross-stripe contributions are lost.

**This is the fundamental limitation of a per-stripe scatter model.** It means that at stripe boundaries, bokeh discs that cross the boundary will be slightly clipped. For the majority of the output this is invisible; for large bokeh discs (very out of focus, low f-stop) at stripe boundaries, there may be a seam.

**Mitigation (for S02):** Accept the seam. For correctness at production quality, S02 can optionally add a `renderStripe` override that requests a full-frame stripe by adjusting what box PlanarIop presents. The simplest approach: in `_validate`, set the stripe height to the full format height so there is always one stripe. This makes the scatter boundary-free but may be slower than multi-stripe. This is a quality knob question deferred to S04.

**For S02: discard out-of-stripe contributions. Mark this as a known limitation in the code comment.**

### 3. The focal length from `_poly_sys` — convention ambiguity

The lentil `.fit` format does not have a standardised header for focal length; it is baked into the polynomial coefficients implicitly. The approach used in S02:

- Use `coc_radius` (already implemented) for **bounding** the scatter footprint to cull deep pixels that cannot contribute to the current stripe (performance optimisation, not strictly needed for correctness).
- For the **actual bokeh size**, trust the polynomial: the Newton iteration naturally produces the physically correct bokeh size because it uses the lens's actual aberration model. No explicit focal length is needed for the scatter itself.
- The `focal_length_mm` is needed for:
  - Aperture radius: `ap_radius = focal_length_mm / (2 * fstop)` — this scales the aperture disk sample
  - CoC radius for culling
- **Source for `focal_length_mm`**: The `.fit` file does not contain a metadata block. The correct approach is to expose focal length as a Float_knob (S04) and use a hardcoded 50mm default in S02. The aperture disk radius and CoC radius both scale linearly with `focal_length_mm`, so a wrong value produces wrong bokeh sizes but not a crash or NaN.

**For S02: use `_fstop` and a hardcoded focal_length of 50.0mm as default. Wire a Float_knob for focal_length in S04.**

### 4. Sensor coordinate system — lentil convention

lentil's polynomial uses a **normalised sensor coordinate system** where the sensor is assumed to be in the range `[-1, 1]` in x and `[-aspect, aspect]` in y (or equivalently `[-1, 1]` × `[-1, 1]` for square sensors). This is typical for polynomial optics fitted by lentil's gencode toolchain.

However the exact normalisation depends on how the polynomial was fitted. To avoid assuming a specific sensor size:

**Safest approach for S02:** Use normalised coordinates throughout. Map pixel `(px, py)` to `(-1..1, -1..1)` based on the output format dimensions. The aperture disk is also normalised to radius 1 (or scaled to `1/fstop`). The Newton iteration residuals are in these normalised units.

**Alternative:** Use physical mm coordinates (36mm × 24mm full-frame sensor). This matches lentil's default fitting assumption but may not match all `.fit` files.

**Decision for S02:** Use normalised `[-1, 1]` sensor coordinates and a unit aperture disk scaled by `1/fstop`. The `pix_per_mm` conversion is replaced by a `pix_per_norm_unit = fmt.width() / 2.0f`. This is the simplest approach that produces visible output without needing `.fit` metadata.

### 5. Halton sequence for aperture disk sampling

The Halton low-discrepancy sequence in bases 2 and 3 is standard for aperture sampling (D026 notes "Halton or stratified preferred over pure random"). The Halton sequence is trivially self-contained:

```cpp
inline float halton(int index, int base) {
    float result = 0.0f;
    float f = 1.0f;
    int i = index + 1;  // start from index 1 to skip the origin
    while (i > 0) {
        f /= base;
        result += f * (i % base);
        i /= base;
    }
    return result;
}
```

Concentric disk mapping (Shirley) for `(u, v) ∈ [0,1)² → (x, y)` on unit disk:

```cpp
inline void map_to_disk(float u, float v, float& x, float& y) {
    float a = 2.0f * u - 1.0f;  // remap to [-1, 1]
    float b = 2.0f * v - 1.0f;
    if (a == 0.0f && b == 0.0f) { x = y = 0.0f; return; }
    float r, phi;
    if (std::fabs(a) > std::fabs(b)) {
        r = a;
        phi = (M_PI / 4.0f) * (b / a);
    } else {
        r = b;
        phi = (M_PI / 2.0f) - (M_PI / 4.0f) * (a / b);
    }
    x = r * std::cos(phi);
    y = r * std::sin(phi);
}
```

Both `halton` and `map_to_disk` should be added as `inline` free functions in `deepc_po_math.h`. They have no poly.h or DDImage dependency.

### 6. `deepEngine` call signature in `renderStripe`

From the mock stub and S01 `getRequests` pattern:

```cpp
input0()->deepEngine(bounds, needed, deepPlane);
```

Where `bounds` is `imagePlane.bounds()` and `needed` is the required channel set. The S01 `getRequests` already requests `Mask_RGBA + Chan_DeepFront + Chan_DeepBack + Chan_Alpha` for this box. The `deepEngine` call in `renderStripe` must match the channel set that was requested.

**Important:** `input0()` must be checked for null before calling. `_poly_loaded` must be true. Both are already guarded by the `_validate` pattern — but defensive checks in `renderStripe` are correct practice.

### 7. `DeepPlane::getPixel` iterator vs coordinate

The mock shows two overloads:
```cpp
DeepPixel getPixel(int y, int x) const;
DeepPixel getPixel(const Box::iterator&) const;
```

The iterator overload is preferred (same pattern as `DeepCDepthBlur`'s `doDeepEngine` loop). The iterator is obtained from `Box::iterator it = bounds.begin()`. The pixel loop becomes:

```cpp
for (Box::iterator it = bounds.begin(); it != bounds.end(); ++it) {
    DeepPixel pixel = deepPlane.getPixel(it);
    const int px = it.x;
    const int py = it.y;
    ...
}
```

### 8. Abort checking

The outer pixel loop must call `Op::aborted()` periodically. Pattern from `DeepCDepthBlur`:

```cpp
for (Box::iterator it = bounds.begin(); it != bounds.end(); ++it) {
    if (Op::aborted()) return;
    ...
}
```

For N-sample inner loops, abort checking every outer pixel is sufficient.

---

## Verification Plan for S02

### Updated syntax check

`verify-s01-syntax.sh` already includes `DeepCDefocusPO.cpp`. After S02 edits, re-run it to confirm no new compile errors. The mock `writableAt` returns `float&` and supports `+=` implicitly.

**New mock needed:** `<vector>` and `<cmath>` and `<cstdlib>` are already `#include`d by the existing mocks. No new mock stubs required for the S02 additions — `halton`, `map_to_disk`, and the scatter loop use only `float`, `int`, standard math, and types already mocked.

**Potential mock gap:** `fmt.width()` and `fmt.height()` on `Format` — the existing `Format` mock in `Box.h` is empty (no `width()` or `height()` methods). S02 needs these to compute `pix_per_norm_unit`. Add stubs:
```cpp
class Format {
public:
    Format() {}
    int width()  const { return 2048; }
    int height() const { return 1556; }
};
```

### Grep contracts for S02

```bash
# lt_newton_trace has real implementation (no longer just {0,0})
grep -q 'poly_system_evaluate' src/deepc_po_math.h        # newton body calls poly
grep -q 'mat2_inverse'          src/deepc_po_math.h        # jacobian inversion present
grep -q 'halton'                src/deepc_po_math.h        # Halton function present
grep -q 'map_to_disk'           src/deepc_po_math.h        # disk mapping present

# renderStripe calls deepEngine and scatters
grep -q 'deepEngine'            src/DeepCDefocusPO.cpp     # deep fetch present
grep -q 'writableAt'            src/DeepCDefocusPO.cpp     # scatter write present
grep -q 'lt_newton_trace'       src/DeepCDefocusPO.cpp     # newton called in renderStripe
grep -q '0\.45f'                src/DeepCDefocusPO.cpp     # R wavelength present
grep -q '0\.55f'                src/DeepCDefocusPO.cpp     # G wavelength present
grep -q '0\.65f'                src/DeepCDefocusPO.cpp     # B wavelength present
grep -q 'halton'                src/DeepCDefocusPO.cpp     # Halton sampling present
grep -q '_aperture_samples'     src/DeepCDefocusPO.cpp     # N controls loop count

# Syntax check still passes
bash scripts/verify-s01-syntax.sh   # must exit 0
```

### Non-zero output contract (integration)

The definitive S02 pass criterion is visible non-zero output in Nuke. Because Docker build is CI-only, the contract verifiable locally is:

1. Syntax check passes.
2. Grep contracts all pass.
3. Code review confirms the scatter loop is structurally correct: deepEngine called, iterator loop present, three-wavelength trace, writableAt accumulation.

The visual check (bokeh visible) is confirmed at docker build + Nuke session in S05.

---

## Natural Seams (for task decomposition)

S02 decomposes into exactly **two sequential tasks**:

| Task | File | What it builds |
|------|------|---------------|
| T01 | `src/deepc_po_math.h` | Replace `lt_newton_trace` stub with Newton iteration; add `halton`, `map_to_disk` helper functions |
| T02 | `src/DeepCDefocusPO.cpp` | Replace `renderStripe` zeroing loop with full PO scatter engine; update `verify-s01-syntax.sh` if Format mock needs `width()`/`height()` |

T01 must complete before T02 (T02 calls `lt_newton_trace`, `halton`, `map_to_disk` from `deepc_po_math.h`).

No other files change in S02.

---

## Known Risks / Watch-outs

1. **Scatter outside stripe bounds is silently discarded.** At large bokeh radii, contributions from a pixel in stripe N that should land in stripe N-1 are lost. This produces a slight seam artefact at stripe boundaries. Acceptable for S02; note in comments. Mitigation: request a single full-height stripe in `_validate` (set stripe height = format height) — this is a PlanarIop option but the API for controlling stripe size may vary by Nuke version.

2. **lentil coordinate normalisation.** The `.fit` polynomial was fitted in a specific coordinate space. If the polynomial assumes `[-1, 1]` normalised sensor coords, using physical mm coords will produce bokeh at the wrong scale (but not a crash). Start with normalised coords and verify visually. The `coc_radius` result can serve as a sanity check on bokeh scale.

3. **`poly_system_evaluate` output index [2],[3] for sensor back-projection.** Based on the `poly.h` comment: `output : float[5] = (aperture_x', aperture_y', sensor_x', sensor_y', lambda')`. Indices 2 and 3 are the back-projected sensor position. If a particular `.fit` file was generated with a different polynomial convention, these indices could differ. This is a property of how lentil's gencode toolchain generates the polynomial, and all standard lentil-generated files use this convention.

4. **Newton iteration may not converge.** For aperture samples at the rim of the aperture disk and for samples far from focus, the Jacobian can be ill-conditioned. The singularity check (`if (std::fabs(det) < 1e-12) break;`) handles this — the iteration exits early with whatever position it reached, producing a misplaced splat. For most samples this is fine. Add a maximum iteration count (20 is sufficient; lentil uses 20).

5. **`writableAt` += requires the accumulation to start from 0.** The zeroing pass before the scatter loop is mandatory. If `renderStripe` is called more than once for the same stripe (which PlanarIop guarantees it is not, but defensively), the result would double-accumulate. The PlanarIop contract says `renderStripe` is called exactly once per stripe per render. Safe.

6. **`Format::width()` / `Format::height()` missing from mock.** The `Format` mock in `Box.h` in `verify-s01-syntax.sh` currently has no method bodies. Adding `int width() const { return 2048; }` and `int height() const { return 1556; }` is needed for S02's compile. This is a mock-only change; the real NDK `Format` has these methods.

7. **`deepEngine` called with the stripe box only.** If the scatter radius for a deep sample would push its contribution outside the current stripe, those out-of-stripe contributions are lost (risk #1 above). Additionally, `deepEngine` is called with `imagePlane.bounds()` — the same box that was pre-requested in `getRequests`. This is consistent and correct. No over-fetch is needed because scatter in a gather model fetches all deep samples visible in the current stripe and then writes to that same stripe.

---

## Forward Intelligence for S03

- **S03 inserts inside the aperture loop.** The holdout weighting goes immediately before the `writableAt` accumulation — not before the deepEngine fetch. The insertion point is at step 8 in the scatter loop above: `contribution *= holdout_transmittance_at_depth`. S03 will fetch the holdout DeepPlane in the same call structure as the primary Deep fetch.
- **Holdout fetch uses `Op::input(1)` cast to `DeepOp*`.** Already wired in S01. S03 calls `holdout->deepEngine(bounds, depth_channels, holdoutPlane)` in `renderStripe` alongside the primary fetch.
- **The holdout fetch is `for_real` guarded.** The holdout input is optional (`minimum_inputs() == 1`); S03 must null-check `dynamic_cast<DeepOp*>(Op::input(1))` before accessing it.

---

## Forward Intelligence for S04

- **Focal length as a Float_knob.** S02 hardcodes 50mm. S04 should add `Float_knob(f, &_focal_length_mm, "focal_length", "focal length (mm)")` and replace the hardcoded constant.
- **Sensor size as a knob or enum.** Different `.fit` files may assume different sensor sizes. S04 could expose a "sensor width" knob or a format preset (35mm full frame / APS-C / etc.).
- **Single-stripe override.** If S02's stripe-boundary seam is noticeable in testing, S04 can add a `bool _single_stripe = true` path that overrides PlanarIop's tile request to request the full output format in one stripe. The mechanism is implementation-defined for PlanarIop but achievable by overriding `getRequests` to request a single stripe.
