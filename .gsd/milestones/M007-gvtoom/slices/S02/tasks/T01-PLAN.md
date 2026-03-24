---
estimated_steps: 5
estimated_files: 2
skills_used: []
---

# T01: Implement thin-lens scatter engine in renderStripe + fix mock header

**Slice:** S02 — DeepCDefocusPOThin scatter engine
**Milestone:** M007-gvtoom

## Description

Replace the zero-output `renderStripe` stub in `DeepCDefocusPOThin.cpp` with a full thin-lens CoC scatter engine. The scatter model uses thin-lens physics for scatter radius (via `coc_radius()`) and the polynomial lens system to warp aperture sample positions within the CoC disk (Option B from D032), producing physically-motivated aberrations (cat-eye, coma, per-channel CA).

Also fix the `ImagePlane` mock in `verify-s01-syntax.sh` to include `chanNo(Channel)` and `writableAt(int,int,int)` so the syntax check validates correct channel-index usage.

## Steps

1. **Fix the mock header first.** In `scripts/verify-s01-syntax.sh`, add to the `ImagePlane` class mock:
   - `int chanNo(Channel z) const { return static_cast<int>(z); }` 
   - A second `writableAt` overload: `float& writableAt(int, int, int)` (accepting int channel index, not Channel enum)
   - Also add `bool contains(Channel) const { return true; }` to the `ChannelSet` class mock (needed for `chans.contains(z)` guard)

2. **Replace the `renderStripe` body** in `src/DeepCDefocusPOThin.cpp`. The new body must implement, in order:
   - `imagePlane.makeWritable()`
   - Extract bounds, channels, format dimensions (`half_w = fmt.width() * 0.5f`, `half_h = fmt.height() * 0.5f`)
   - **Poly load/reload check** at renderStripe entry (M006 thread-safety pattern — do NOT rely on `_validate` having loaded it):
     ```cpp
     if (_poly_file && _poly_file[0] && (!_poly_loaded || _reload_poly)) {
         if (_poly_loaded) { poly_system_destroy(&_poly_sys); _poly_loaded = false; }
         if (poly_system_read(&_poly_sys, _poly_file) != 0) { /* zero output and return */ }
         _poly_loaded = true; _reload_poly = false;
     }
     if (!_poly_loaded || !input0()) { /* zero output and return */ }
     ```
   - **Zero the output buffer** before accumulation (loop over channels × pixels, set to 0 using `imagePlane.chanNo(z)`)
   - **Holdout fetch** — if `input1()` is connected, fetch holdout deep plane. Define `transmittance_at` lambda that computes transmittance at a given (x, y, depth) from the holdout plane:
     ```cpp
     auto transmittance_at = [&](int px, int py, float Z) -> float {
         // getPixel(y, x) — row-first DDImage convention
         DeepPixel hp = holdoutPlane.getPixel(py, px);
         float transmit = 1.0f;
         for (int s = 0; s < hp.getSampleCount(); ++s) {
             float hzf = hp.getUnorderedSample(s, Chan_DeepFront);
             if (hzf >= Z) continue;  // skip samples at or behind scatter depth
             float ha = hp.getUnorderedSample(s, Chan_Alpha);
             transmit *= (1.0f - ha);
         }
         return transmit;
     };
     ```
   - **Deep input fetch** — fetch deep plane from `input0()` over bounds with needed channels
   - **Per-pixel, per-deep-sample, per-aperture-sample scatter loop:**
     - For each pixel (x, y) in bounds:
       - Convert pixel to normalised coords: `sx = (x + 0.5f - half_w) / half_w`, `sy = (y + 0.5f - half_h) / half_h`
       - Get deep pixel, iterate samples
       - For each deep sample: get `Z = getUnorderedSample(s, Chan_DeepFront)`, RGBA values
       - Compute `coc_norm = coc_radius(_focal_length_mm, _fstop, _focus_distance, Z) / (_focal_length_mm * 0.5f + 1e-6f)`
       - Compute `ap_radius = 1.0f / (_fstop + 1e-6f)`
       - Aperture sample loop `k = 0 .. max(_aperture_samples, 1) - 1`:
         - `ax_unit, ay_unit = map_to_disk(halton(k, 2), halton(k, 3))`
         - Per-channel CA loop over `{Chan_Red, CA_LAMBDA_R}`, `{Chan_Green, CA_LAMBDA_G}`, `{Chan_Blue, CA_LAMBDA_B}`:
           - `ax = ax_unit * ap_radius`, `ay = ay_unit * ap_radius`
           - `poly_system_evaluate(&_poly_sys, {sx, sy, ax, ay, lambda}, out5, 5, _max_degree)`
           - `transmit = clamp(out5[4], 0, 1)`
           - **Option B warp**: `wx = out5[0], wy = out5[1]`; clamp magnitude to `ap_radius`; scale to CoC: `landing_x = sx + coc_norm * wx / (ap_radius + 1e-6f)`, `landing_y = sy + coc_norm * wy / (ap_radius + 1e-6f)`
           - Convert landing to pixel: `out_px = landing_x * half_w + half_w - 0.5f`, `out_py = landing_y * half_h + half_h - 0.5f`
           - Round to int, bounds-check against `bounds`
           - Apply holdout: `weight = transmit * holdout_t / N` (where `holdout_t = transmittance_at(out_px_i, out_py_i, Z)` if holdout connected, else 1.0)
           - Accumulate colour channel: `imagePlane.writableAt(out_px_i, out_py_i, imagePlane.chanNo(chan)) += sample_color * weight`
         - Alpha uses green-channel (0.55μm) landing position, accumulated similarly
   - All `writableAt` calls must use `imagePlane.chanNo(z)` and be guarded by `chans.contains(chan)`

3. **Define a `zero_output` helper** (inline lambda or private method) that zeros all channels in bounds using `chanNo`. Used for early-return paths (no poly, no input).

4. **Verify syntax check passes**: Run `bash scripts/verify-s01-syntax.sh`.

5. **Verify grep contracts**: Run all 8 grep checks from the slice verification section.

## Must-Haves

- [ ] `coc_radius()` called with knob members to compute scatter radius
- [ ] `poly_system_evaluate` called with `_max_degree` parameter (R032)
- [ ] Option B warp: `out5[0:1]` used as warped aperture position, clamped to `ap_radius`, scaled to CoC
- [ ] Per-channel CA: separate `poly_system_evaluate` calls at `CA_LAMBDA_R`, `CA_LAMBDA_G`, `CA_LAMBDA_B`
- [ ] `halton(k, 2)` and `halton(k, 3)` + `map_to_disk` for aperture sampling
- [ ] `transmittance_at` holdout lambda with depth gate (`hzf >= Z` → continue)
- [ ] `imagePlane.chanNo(z)` for every `writableAt` call
- [ ] `chans.contains(chan)` guard before every channel write
- [ ] Poly load/reload at `renderStripe` entry (M006 pattern)
- [ ] Normalised↔pixel coordinate conversions using `half_w`/`half_h` from `info_.full_size_format()`
- [ ] `ImagePlane` mock updated with `chanNo` and `writableAt(int,int,int)`
- [ ] `ChannelSet` mock updated with `contains(Channel)`
- [ ] `verify-s01-syntax.sh` exits 0

## Verification

- `bash scripts/verify-s01-syntax.sh` exits 0
- `grep -q 'coc_radius' src/DeepCDefocusPOThin.cpp`
- `grep -q 'halton' src/DeepCDefocusPOThin.cpp`
- `grep -q 'map_to_disk' src/DeepCDefocusPOThin.cpp`
- `grep -q 'chanNo' src/DeepCDefocusPOThin.cpp`
- `grep -q 'transmittance_at' src/DeepCDefocusPOThin.cpp`
- `grep -q '0.45f' src/DeepCDefocusPOThin.cpp`
- `grep -q '_max_degree' src/DeepCDefocusPOThin.cpp`
- `grep -q 'chans.contains' src/DeepCDefocusPOThin.cpp`

## Observability Impact

- **New signals:** `error("Cannot open lens file: %s")` emitted on poly load failure. Zero-output lambda provides silent-but-diagnosable black output on early-return paths.
- **Inspection:** A future agent can confirm scatter engine is active by checking grep contracts (`coc_radius`, `halton`, `transmittance_at`, etc.) and by running `nuke -x test/test_thin.nk` to produce non-black output.
- **Failure state:** If renderStripe produces black output despite valid inputs, check: (1) `_poly_loaded` false → missing `.fit` file, (2) `_aperture_samples` = 0 → no scatter loop iterations, (3) bounds mismatch → all scatter samples land outside tile.

## Inputs

- `src/DeepCDefocusPOThin.cpp` — S01 scaffold with zero-output renderStripe stub, all knobs/members/infrastructure in place
- `src/deepc_po_math.h` — provides `coc_radius`, `halton`, `map_to_disk`
- `src/poly.h` — provides `poly_system_evaluate` with `max_degree` parameter
- `scripts/verify-s01-syntax.sh` — mock headers for syntax-only compilation

## Expected Output

- `src/DeepCDefocusPOThin.cpp` — renderStripe replaced with full thin-lens scatter engine
- `scripts/verify-s01-syntax.sh` — ImagePlane mock updated with `chanNo` and `writableAt(int,int,int)`, ChannelSet mock updated with `contains`
