# S02 Summary: Ray path-trace engine

**Slice goal:** Replace DeepCDefocusPORay's scatter engine with a real lentil-style polynomial optics gather path tracer.
**Status:** Complete — all contracts pass, all three S02 requirements validated (R037, R041, R044).
**Proof level:** Contract (structural grep + g++ syntax check). Runtime proof (docker build + nuke -x) is the milestone DoD, not S02 gate.

---

## What This Slice Actually Delivered

### Core: Gather path-trace engine in `renderStripe`

The scatter loop that copied the Thin variant's CoC warp has been completely replaced with a gather engine. The new engine iterates **output pixels**, not input pixels — matching lentil's camera-shader architecture.

Full pipeline per output pixel × aperture sample:

1. **Sensor mm coords** — `sx = (ox + 0.5 - half_w) * _sensor_width / (2 * half_w)`, `sy` same (y uses `half_w`, not `half_h` — critical for correct aspect ratio in physical mm space)
2. **Halton disk sample** — aperture point via `halton(2/3, index)` + Shirley concentric mapping
3. **Vignetting retry loop** — up to `VIGNETTING_RETRIES=10` retries on Newton failure, outer pupil clip, or inner pupil clip; each retry resamples aperture point at fresh Halton index
4. **Newton aperture match** — `pt_sample_aperture(sx, sy, dx, dy, ax, ay, lambda, sensor_shift, &_poly_sys, _max_degree)` solves sensor direction (dx,dy) for given aperture target (ax,ay) using FD Jacobians
5. **Forward poly eval** — `poly_system_evaluate` on the shifted sensor position `(sx+dx*sensor_shift, sy+dy*sensor_shift)`; output is 5 values (aperture xy, direction xy, transmittance)
6. **Pupil culls** — outer: `|(out[0],out[1])| > _outer_pupil_radius` → retry; inner: `|(out[0],out[1])| < _inner_pupil_radius` → retry
7. **3D ray** — `sphereToCs_full(out[0..3], _outer_pupil_radius, origin, direction)` using center=-R convention
8. **Pinhole project** — `in_x = ray_dx/ray_dz * f_px + half_w - 0.5`; `f_px = _back_focal_length / _sensor_width * image_width`
9. **Deep column flatten** — `deepPlane.getPixel(in_py, in_px)` → iterate DeepPixel front-to-back, evaluate holdout transmittance at output pixel, front-to-back composite → flat RGBA contribution
10. **CA loop** — R/G/B traced at 0.45/0.55/0.65 μm; all_channels_ok flag: if any channel fails (Newton or pupil), the whole aperture sample is rejected (no partial-channel accumulation)

### `getRequests` expanded bounds

Deep request box padded by `max_coc_pixels = aperture_housing_radius / sensor_width * image_width * 2 + 2` to cover rays landing outside the current stripe. Matching bounds used for the deep fetch in `renderStripe`.

### `sensor_shift` computed once per stripe

`logarithmic_focus_search` called once at stripe start with `_focus_distance`, lens constants, and `_poly_sys`. Result reused for every aperture sample in the stripe.

### Scatter vestiges removed

`coc_norm`, `coc_radius` usage, `ap_radius` normalised factor, and all Option B warp code deleted. Confirmed by negative grep contract in verify script.

---

## Patterns Established

### All-or-nothing CA channel policy
When any CA channel fails Newton convergence or pupil clipping, the whole aperture sample is rejected (`all_channels_ok` flag). This prevents partial-channel contributions where R lands but B misses, which would produce colour fringing artifacts unrelated to chromatic aberration (artefactual, not physical).

### Gather loop requires expanded deep request bounds
A scatter loop only needs input data for pixels it will write. A gather loop needs input data for any pixel a ray might land on — which can be far outside the output stripe bounds. The `getRequests` expansion pattern (pad by max_coc_pixels) must be adopted by any future gather-style renderer on deep data.

### Sensor mm convention: y uses half_w not half_h
The polynomial is fitted in a coordinate space where x and y are both normalised by the same half-width (not separately by half-height). Using half_h for y would distort the sensor position for non-square images and break the polynomial evaluation. This is the lentil convention and must be preserved in any future sensor coordinate computation.

---

## What Remains for the Milestone DoD

- Docker build: compile both .so plugins against real Nuke SDK
- `nuke -x test/test_ray.nk`: runtime proof — non-black 128×72 EXR, exit 0
- `nuke -x test/test_thin.nk`: regression check, exit 0

These are milestone-level checks that require the Docker environment, not slice-level requirements.

---

## Key Files Changed

| File | Change |
|------|--------|
| `src/DeepCDefocusPORay.cpp` | Replaced scatter `renderStripe` with gather path-trace engine; expanded `getRequests` bounds |
| `scripts/verify-s01-syntax.sh` | Added S02 contracts section (6 structural grep checks: 5 positive + 1 negative) |

---

## Verification Evidence

| Check | Command | Result |
|-------|---------|--------|
| All S01+S02 contracts | `bash scripts/verify-s01-syntax.sh` | ✅ exit 0, all 19 contracts PASS |
| Scatter vestige absent | `grep -q 'coc_norm' src/DeepCDefocusPORay.cpp` | ✅ exit 1 (not found) |
| Syntax clean | g++ -fsyntax-only (4 files) | ✅ all pass |

---

## Requirements Validated in This Slice

| ID | Description |
|----|-------------|
| R037 | Ray renderStripe: lentil-style gather path trace (sensor mm → pt_sample_aperture → poly eval → sphereToCs_full → project → deep flatten) |
| R041 | Vignetting retry loop (VIGNETTING_RETRIES=10) for outer/inner pupil clips and Newton failures |
| R044 | Per-ray deep column fetch, front-to-back flatten with holdout transmittance, accumulate flat RGBA |

R035 (CA wavelengths 0.45/0.55/0.65 μm in gather engine) confirmed preserved in the new engine.
