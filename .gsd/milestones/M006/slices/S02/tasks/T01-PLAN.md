---
estimated_steps: 4
estimated_files: 3
skills_used:
  - review
---

# T01: Implement Newton Trace Helpers and PO Scatter Loop

**Slice:** S02 — PO Scatter Engine — Stochastic Aperture Sampling
**Milestone:** M006

## Description

S01 left two stubs: `lt_newton_trace` (returns `{0,0}`) in `src/deepc_po_math.h` and the `renderStripe` body (zeroes all pixels) in `src/DeepCDefocusPO.cpp`. This task replaces both with production implementations, completing the core scatter algorithm. Nothing else changes — CMake, knobs, poly load lifecycle, mock headers, and the rest of the verification script are untouched.

There are three concrete deliverables:

1. **`src/deepc_po_math.h`** — Two new inline free functions (`halton`, `map_to_disk`) added above `lt_newton_trace`; the `lt_newton_trace` stub body replaced with the Newton iteration.

2. **`src/DeepCDefocusPO.cpp`** — The zeroing loop in `renderStripe` replaced with the full PO scatter loop: Deep input fetch, per-pixel/per-sample accumulation, per-channel wavelength tracing (0.45/0.55/0.65 μm), sensor→pixel conversion, bounds-clamped splat.

3. **`scripts/verify-s01-syntax.sh`** — S02 grep contracts appended (halton, map_to_disk, wavelengths, deepEngine, lt_newton_trace call) and run to confirm all pass.

The coordinate system used throughout is **normalised `[-1, 1]`**: pixel `(px, py)` maps to `((px + 0.5 - width/2) / (width/2), (py + 0.5 - height/2) / (height/2))`. The aperture disk is scaled to radius `1 / _fstop` in these same units. This avoids the sensor-size-in-mm ambiguity across different `.fit` files and produces visible bokeh without needing lens metadata. The default `focal_length_mm = 50.0f` is used only for the `coc_radius` culling heuristic (which is a performance guard, not correctness-critical).

## Steps

1. **Add `halton` and `map_to_disk` to `src/deepc_po_math.h`**

   Insert the following two inline free functions immediately above the `lt_newton_trace` declaration. They have no DDImage or poly.h dependency.

   ```cpp
   // halton — Halton low-discrepancy sequence, base `base`, index `index`.
   // index should start from 1 (index 0 gives 0.0 for all bases).
   inline float halton(int index, int base)
   {
       float result = 0.0f;
       float f = 1.0f;
       int i = index + 1;  // skip index 0 (maps to 0 for all bases)
       while (i > 0) {
           f /= static_cast<float>(base);
           result += f * static_cast<float>(i % base);
           i /= base;
       }
       return result;
   }

   // map_to_disk — Shirley concentric square-to-disk mapping.
   // Input (u, v) in [0, 1); output (x, y) on unit disk.
   inline void map_to_disk(float u, float v, float& x, float& y)
   {
       const float a = 2.0f * u - 1.0f;
       const float b = 2.0f * v - 1.0f;
       if (a == 0.0f && b == 0.0f) { x = y = 0.0f; return; }
       float r, phi;
       if (std::fabs(a) > std::fabs(b)) {
           r   = a;
           phi = (static_cast<float>(M_PI) / 4.0f) * (b / a);
       } else {
           r   = b;
           phi = (static_cast<float>(M_PI) / 2.0f)
               - (static_cast<float>(M_PI) / 4.0f) * (a / b);
       }
       x = r * std::cos(phi);
       y = r * std::sin(phi);
   }
   ```

   Also add `#include <cmath>` guard check — `<cmath>` is already present; confirm `M_PI` is available or define a fallback:
   ```cpp
   #ifndef M_PI
   #define M_PI 3.14159265358979323846
   #endif
   ```
   Place this define after the `#pragma once` and before any includes.

2. **Replace the `lt_newton_trace` stub body with the Newton iteration**

   Replace the entire `inline Vec2 lt_newton_trace(...)` body (the current one-liner that returns `{0.0, 0.0}`) with:

   ```cpp
   inline Vec2 lt_newton_trace(const float sensor_target[2],
                               const float aperture_pos[2],
                               float       lambda_um,
                               const poly_system_t* poly_sys)
   {
       float pos[2] = { sensor_target[0], sensor_target[1] };

       const int   MAX_ITER = 20;
       const float TOL      = 1e-5f;
       const float EPS      = 1e-4f;

       for (int iter = 0; iter < MAX_ITER; ++iter) {
           float in5[5]  = { pos[0], pos[1],
                              aperture_pos[0], aperture_pos[1], lambda_um };
           float out5[5] = {};
           poly_system_evaluate(poly_sys, in5, out5, 5);

           // out5[2], out5[3] = back-projected sensor position at (pos, aperture)
           const float res0 = out5[2] - sensor_target[0];
           const float res1 = out5[3] - sensor_target[1];

           if (res0 * res0 + res1 * res1 < TOL * TOL)
               break;

           // Finite-difference Jacobian
           float in_dx[5] = { pos[0]+EPS, pos[1],     aperture_pos[0], aperture_pos[1], lambda_um };
           float in_dy[5] = { pos[0],     pos[1]+EPS, aperture_pos[0], aperture_pos[1], lambda_um };
           float out_dx[5] = {}, out_dy[5] = {};
           poly_system_evaluate(poly_sys, in_dx, out_dx, 5);
           poly_system_evaluate(poly_sys, in_dy, out_dy, 5);

           Mat2 J;
           J.m[0][0] = static_cast<double>((out_dx[2] - out5[2]) / EPS);
           J.m[0][1] = static_cast<double>((out_dy[2] - out5[2]) / EPS);
           J.m[1][0] = static_cast<double>((out_dx[3] - out5[3]) / EPS);
           J.m[1][1] = static_cast<double>((out_dy[3] - out5[3]) / EPS);

           const double det = J.m[0][0] * J.m[1][1] - J.m[0][1] * J.m[1][0];
           if (std::fabs(det) < 1e-12)
               break;  // singular Jacobian — no update, return current estimate

           const Mat2 Jinv = mat2_inverse(J);
           pos[0] -= static_cast<float>(Jinv.m[0][0] * res0 + Jinv.m[0][1] * res1);
           pos[1] -= static_cast<float>(Jinv.m[1][0] * res0 + Jinv.m[1][1] * res1);
       }

       return Vec2{ static_cast<double>(pos[0]), static_cast<double>(pos[1]) };
   }
   ```

   Remove the old parameter comment block that says "Stub body returns {0.0, 0.0}; full Newton iteration in S02." and replace it with:

   ```cpp
   // lt_newton_trace — polynomial-optics Newton trace
   //
   // Solves the inverse lens mapping: given a target sensor position and an
   // aperture sample, iterates Newton's method through the polynomial system
   // to find where the aperture sample actually lands on the sensor at the
   // given wavelength.
   //
   // Returns: Vec2{sensor_x, sensor_y} of the landing position in normalised
   //          sensor coordinates (same units as sensor_target).
   //
   // poly_sys must be non-null and loaded (caller's responsibility).
   ```

3. **Replace the `renderStripe` zeroing loop with the PO scatter loop in `src/DeepCDefocusPO.cpp`**

   Locate the `renderStripe` method. The existing body after `imagePlane.makeWritable()` is the block to replace. Keep `imagePlane.makeWritable()` and the `const Box& bounds = imagePlane.bounds();` line. Remove the comment `// S02: replace with PO scatter loop` and everything after it (the zeroing loop), and insert:

   ```cpp
   const ChannelSet& chans = imagePlane.channels();

   // Fast path: no poly loaded or no input — zero output.
   if (!_poly_loaded || !input0()) {
       foreach(z, chans)
           for (int y = bounds.y(); y < bounds.t(); ++y)
               for (int x = bounds.x(); x < bounds.r(); ++x)
                   imagePlane.writableAt(x, y, z) = 0.0f;
       return;
   }

   // 1. Fetch Deep input for this stripe.
   DeepPlane deepPlane;
   ChannelSet needed = Mask_RGBA;
   needed += Chan_DeepFront;
   needed += Chan_DeepBack;
   if (!input0()->deepEngine(bounds, needed, deepPlane))
       return;

   // 2. Zero the output accumulation buffer.
   foreach(z, chans)
       for (int y = bounds.y(); y < bounds.t(); ++y)
           for (int x = bounds.x(); x < bounds.r(); ++x)
               imagePlane.writableAt(x, y, z) = 0.0f;

   // 3. Format dimensions for normalised coordinate conversion.
   //    Sensor coordinates use [-1, 1] x [-1, 1] (normalised).
   //    pix_per_norm = half-width in pixels; multiply norm offset by this
   //    to get pixel offset.
   const Format& fmt      = *info_.full_size_format();
   const float   half_w   = static_cast<float>(fmt.width())  * 0.5f;
   const float   half_h   = static_cast<float>(fmt.height()) * 0.5f;

   // Aperture radius in normalised units: radius = 1 / fstop.
   // (Larger aperture = smaller fstop = more defocus.)
   const float ap_radius = 1.0f / std::max(_fstop, 0.1f);

   // CoC culling uses physical units — keep 50mm as a default focal length
   // since the .fit file has no explicit metadata field for it.
   // This only affects whether a sample is culled early; wrong value = no crash,
   // just slightly suboptimal culling.
   const float focal_length_mm = 50.0f;

   const int N = std::max(_aperture_samples, 1);

   // Per-channel wavelengths: R=0.45μm, G=0.55μm, B=0.65μm (D025).
   const float lambdas[3]         = { 0.45f, 0.55f, 0.65f };
   const Channel rgb_chans[3]     = { Chan_Red, Chan_Green, Chan_Blue };

   // 4. Scatter loop — per input pixel.
   for (Box::iterator it = bounds.begin(); it != bounds.end(); ++it) {
       if (Op::aborted()) return;

       const int px = it.x;
       const int py = it.y;

       DeepPixel pixel = deepPlane.getPixel(it);
       const int nSamples = pixel.getSampleCount();
       if (nSamples == 0) continue;

       // 5. Per deep sample.
       for (int s = 0; s < nSamples; ++s) {
           const float z_front = pixel.getUnorderedSample(s, Chan_DeepFront);
           const float z_back  = pixel.getUnorderedSample(s, Chan_DeepBack);
           const float depth   = 0.5f * (z_front + z_back);
           if (depth < 1e-6f) continue;

           const float alpha = pixel.getUnorderedSample(s, Chan_Alpha);
           if (alpha < 1e-6f) continue;

           // Ideal sensor position for this pixel in normalised [-1,1] coords.
           const float sx0 = (static_cast<float>(px) + 0.5f - half_w) / half_w;
           const float sy0 = (static_cast<float>(py) + 0.5f - half_h) / half_h;

           // CoC-based early cull: skip if sample contributes nothing to stripe.
           // (Performance guard only — not correctness-critical.)
           const float coc = coc_radius(focal_length_mm, _fstop,
                                        _focus_distance, depth);
           // coc is in mm; convert to pixels via half_w / (focal_length_mm/2)
           // approximation — good enough for culling.
           const float coc_px = coc * half_w / (focal_length_mm * 0.5f + 1e-6f);
           const int   coc_px_i = static_cast<int>(coc_px) + 1;
           if (py + coc_px_i < bounds.y() || py - coc_px_i >= bounds.t())
               continue;

           // 6. Aperture sample loop.
           for (int k = 0; k < N; ++k) {
               // Halton(2,3) low-discrepancy aperture sample (D026).
               const float u = halton(k, 2);
               const float v = halton(k, 3);

               // Map to unit disk, then scale to aperture radius.
               float ax_unit, ay_unit;
               map_to_disk(u, v, ax_unit, ay_unit);
               const float ax = ax_unit * ap_radius;
               const float ay = ay_unit * ap_radius;

               // 7. Trace each channel at its wavelength (D025).
               float landing_x[3], landing_y[3];
               float transmit[3];

               for (int c = 0; c < 3; ++c) {
                   const float sensor_t[2] = { sx0, sy0 };
                   const float ap[2]       = { ax, ay };
                   Vec2 land = lt_newton_trace(sensor_t, ap, lambdas[c], &_poly_sys);
                   landing_x[c] = static_cast<float>(land.x);
                   landing_y[c] = static_cast<float>(land.y);

                   // Transmittance from poly output[4].
                   float in5[5]  = { sensor_t[0], sensor_t[1], ax, ay, lambdas[c] };
                   float out5[5] = {};
                   poly_system_evaluate(&_poly_sys, in5, out5, 5);
                   transmit[c] = std::max(0.0f, std::min(1.0f, out5[4]));
               }

               // 8. Splat each channel to output pixel.
               // Normalised sensor position → output pixel index:
               //   out_x = landing_x * half_w + half_w - 0.5
               for (int c = 0; c < 3; ++c) {
                   const float out_xf = landing_x[c] * half_w + half_w - 0.5f;
                   const float out_yf = landing_y[c] * half_h + half_h - 0.5f;
                   const int out_xi = static_cast<int>(std::floor(out_xf + 0.5f));
                   const int out_yi = static_cast<int>(std::floor(out_yf + 0.5f));

                   // Discard contributions landing outside this stripe.
                   // NOTE: this causes a seam for large bokeh discs at stripe
                   // boundaries. Accepted limitation for S02 — see S02 research.
                   if (out_xi < bounds.x() || out_xi >= bounds.r()) continue;
                   if (out_yi < bounds.y() || out_yi >= bounds.t()) continue;

                   const float chan_val = pixel.getUnorderedSample(s, rgb_chans[c]);
                   const float w = transmit[c] / static_cast<float>(N);
                   imagePlane.writableAt(out_xi, out_yi, rgb_chans[c])
                       += chan_val * alpha * w;
               }

               // Alpha: use G-channel (index 1) landing position.
               {
                   const float out_xf = landing_x[1] * half_w + half_w - 0.5f;
                   const float out_yf = landing_y[1] * half_h + half_h - 0.5f;
                   const int out_xi = static_cast<int>(std::floor(out_xf + 0.5f));
                   const int out_yi = static_cast<int>(std::floor(out_yf + 0.5f));
                   if (out_xi >= bounds.x() && out_xi < bounds.r()
                    && out_yi >= bounds.y() && out_yi < bounds.t()) {
                       const float w = transmit[1] / static_cast<float>(N);
                       imagePlane.writableAt(out_xi, out_yi, Chan_Alpha)
                           += alpha * w;
                   }
               }
           } // aperture samples
       } // deep samples
   } // pixels
   ```

   Also add `#include <algorithm>` at the top of the file (after the existing `#include <cstring>`) to ensure `std::max`/`std::min` are available.

4. **Extend `scripts/verify-s01-syntax.sh` with S02 grep contracts and run**

   Append the following block before the final `echo "All syntax checks passed."` line:

   ```bash
   # --- S02 grep contracts ---
   echo "Checking S02 contracts..."
   grep -q 'halton'         "$SRC_DIR/deepc_po_math.h"     || { echo "FAIL: halton missing from deepc_po_math.h"; exit 1; }
   grep -q 'map_to_disk'    "$SRC_DIR/deepc_po_math.h"     || { echo "FAIL: map_to_disk missing from deepc_po_math.h"; exit 1; }
   grep -q 'mat2_inverse'   "$SRC_DIR/deepc_po_math.h"     || { echo "FAIL: mat2_inverse missing from deepc_po_math.h"; exit 1; }
   grep -q '0\.45f'         "$SRC_DIR/DeepCDefocusPO.cpp"  || { echo "FAIL: R wavelength 0.45 missing"; exit 1; }
   grep -q '0\.55f'         "$SRC_DIR/DeepCDefocusPO.cpp"  || { echo "FAIL: G wavelength 0.55 missing"; exit 1; }
   grep -q '0\.65f'         "$SRC_DIR/DeepCDefocusPO.cpp"  || { echo "FAIL: B wavelength 0.65 missing"; exit 1; }
   grep -q 'deepEngine'     "$SRC_DIR/DeepCDefocusPO.cpp"  || { echo "FAIL: deepEngine call missing"; exit 1; }
   grep -q 'lt_newton_trace' "$SRC_DIR/DeepCDefocusPO.cpp" || { echo "FAIL: lt_newton_trace not called in scatter loop"; exit 1; }
   grep -q 'halton'         "$SRC_DIR/DeepCDefocusPO.cpp"  || { echo "FAIL: halton not called in scatter loop"; exit 1; }
   grep -q 'map_to_disk'    "$SRC_DIR/DeepCDefocusPO.cpp"  || { echo "FAIL: map_to_disk not called in scatter loop"; exit 1; }
   # Ensure no leftover stub markers
   if grep -q 'S02: replace' "$SRC_DIR/DeepCDefocusPO.cpp"; then
       echo "FAIL: leftover S02 stub marker in DeepCDefocusPO.cpp"; exit 1
   fi
   echo "S02 contracts: all pass."
   ```

   Then run the full script: `bash scripts/verify-s01-syntax.sh`

## Must-Haves

- [ ] `halton(int index, int base)` added as inline free function in `src/deepc_po_math.h`
- [ ] `map_to_disk(float u, float v, float& x, float& y)` added as inline free function in `src/deepc_po_math.h`
- [ ] `lt_newton_trace` stub body replaced with Newton iteration (20 iterations max, TOL=1e-5, singular Jacobian guard via `fabs(det) < 1e-12`)
- [ ] `M_PI` fallback `#define` present (before `#include <cmath>` or after `#pragma once`)
- [ ] `renderStripe` zeroing loop replaced with scatter loop in `src/DeepCDefocusPO.cpp`
- [ ] Deep input fetched via `input0()->deepEngine(bounds, needed, deepPlane)`
- [ ] Per-channel wavelength trace at `{0.45f, 0.55f, 0.65f}` μm (R, G, B)
- [ ] Alpha channel uses G-channel (index 1) landing position
- [ ] Out-of-stripe contributions discarded with a code comment naming the seam limitation
- [ ] `_poly_loaded` guard: fast-path zeroing when poly not loaded or no input
- [ ] `1/N` normalisation weight applied per aperture-sample contribution
- [ ] `#include <algorithm>` present in `DeepCDefocusPO.cpp`
- [ ] S02 grep contracts appended to `scripts/verify-s01-syntax.sh`
- [ ] `bash scripts/verify-s01-syntax.sh` exits 0 (all three files + all S02 contracts)

## Verification

- `bash scripts/verify-s01-syntax.sh` — must exit 0
- `grep -q 'halton' src/deepc_po_math.h`
- `grep -q 'map_to_disk' src/deepc_po_math.h`
- `grep -q '0\.45f' src/DeepCDefocusPO.cpp`
- `grep -q '0\.55f' src/DeepCDefocusPO.cpp`
- `grep -q '0\.65f' src/DeepCDefocusPO.cpp`
- `grep -q 'deepEngine' src/DeepCDefocusPO.cpp`
- `grep -q 'lt_newton_trace' src/DeepCDefocusPO.cpp`
- `! grep -q 'S02: replace' src/DeepCDefocusPO.cpp`

## Inputs

- `src/deepc_po_math.h` — Mat2, mat2_inverse, Vec2, coc_radius already present; lt_newton_trace stub to replace
- `src/DeepCDefocusPO.cpp` — PlanarIop scaffold from S01; renderStripe zeroing loop to replace; `_poly_sys`, `_fstop`, `_focus_distance`, `_aperture_samples` members already defined
- `src/poly.h` — poly_system_evaluate signature (used in lt_newton_trace body)
- `scripts/verify-s01-syntax.sh` — existing S01 syntax check to extend with S02 contracts

## Observability Impact

**Signals introduced or changed by this task:**

- `_poly_loaded` fast-path branch in `renderStripe`: when poly is absent or no Deep input is connected, output zeroes immediately. Visible as a black output in Nuke viewer — no crash, no hang.
- `deepEngine` return-value check: a false return skips the stripe silently; the upstream Deep error propagates through Nuke's standard error system.
- Newton iteration: 20 iterations max with convergence tolerance 1e-5 and singular-Jacobian guard (`|det| < 1e-12`). Non-convergence returns the best estimate — bokeh may be slightly off but no NaN/inf is produced.
- `Op::aborted()` poll inside the scatter loop: honours Nuke's cancel signal without requiring threads.

**How a future agent inspects this task's runtime behaviour:**
1. Connect a Deep stream to DeepCDefocusPO with a valid `.fit` file — output should show coloured bokeh discs.
2. Remove the `.fit` file path — output should be black (fast-path zero).
3. Increase `aperture_samples` — render time should increase roughly linearly.
4. Inspect `grep -c 'aborted\|_poly_loaded\|deepEngine' src/DeepCDefocusPO.cpp` to confirm all observability hooks are in place.

## Expected Output

- `src/deepc_po_math.h` — halton and map_to_disk helpers added; lt_newton_trace stub replaced with Newton iteration
- `src/DeepCDefocusPO.cpp` — renderStripe scatter loop complete; Deep input fetch, per-channel wavelength trace, bounds-clamped splat
- `scripts/verify-s01-syntax.sh` — S02 grep contracts appended; script exits 0 on the modified source files
