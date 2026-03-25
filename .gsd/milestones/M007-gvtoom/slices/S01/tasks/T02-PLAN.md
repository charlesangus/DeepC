---
estimated_steps: 4
estimated_files: 3
skills_used: []
---

# T02: Create DeepCDefocusPOThin.cpp and DeepCDefocusPORay.cpp scaffolds

**Slice:** S01 — Shared infrastructure, max_degree knob, two-plugin scaffold
**Milestone:** M007-gvtoom

## Description

Create two new Nuke plugin source files by copying and modifying the existing `src/DeepCDefocusPO.cpp`. Both get a stub renderStripe (zeros output) and a new `max_degree` Int_knob. The Ray variant additionally gets `aperture_file`, lens geometry knobs, and a second `poly_system_t`.

## Steps

1. **Create `src/DeepCDefocusPOThin.cpp`** by copying `src/DeepCDefocusPO.cpp` and making these changes:
   - Rename `CLASS` to `"DeepCDefocusPOThin"`, class name to `DeepCDefocusPOThin`
   - Update `HELP` string to describe the thin-lens variant
   - Update `Op::Description` registration: `"Deep/DeepCDefocusPOThin"`
   - Update the `build` function and static `d` member for the new class name
   - Add member `int _max_degree;` initialized to `11` in constructor
   - Add `Int_knob(f, &_max_degree, "max_degree", "max degree");` with `SetRange(f, 1, 11);` in `knobs()` after aperture_samples
   - **Replace the entire renderStripe body** with a zero-output stub:
     ```cpp
     void renderStripe(ImagePlane& imagePlane) override {
         imagePlane.makeWritable();
         const Box& bounds = imagePlane.bounds();
         const ChannelSet& chans = imagePlane.channels();
         foreach(z, chans)
             for (int y = bounds.y(); y < bounds.t(); ++y)
                 for (int x = bounds.x(); x < bounds.r(); ++x)
                     imagePlane.writableAt(x, y, z) = 0.0f;
     }
     ```
   - Keep ALL non-renderStripe code: holdout input wiring (input 1, "holdout" label, test_input with DeepOp cast), CA wavelength constants (0.45f/0.55f/0.65f — keep as comments or constants for S02), knob_changed, _validate with poly loading, _close with poly_system_destroy, getRequests with holdout deepRequest, destructor. The holdout/CA/Halton infrastructure must be present in the source even though renderStripe doesn't use it yet.
   - Keep the `#include "poly.h"` and `#include "deepc_po_math.h"` includes

2. **Create `src/DeepCDefocusPORay.cpp`** by copying the new Thin file and making additional changes:
   - Rename `CLASS` to `"DeepCDefocusPORay"`, class to `DeepCDefocusPORay`
   - Update `HELP` to describe the raytraced gather variant
   - Update `Op::Description` registration: `"Deep/DeepCDefocusPORay"`
   - Add second `const char* _aperture_file;` member (initialized to nullptr)
   - Add second `poly_system_t _aperture_sys;` member (zeroed in constructor)
   - Add tracking bools: `bool _aperture_loaded;` and `bool _reload_aperture;`
   - Add `File_knob(f, &_aperture_file, "aperture_file", "aperture file");` in knobs() after poly_file
   - Add `BeginClosedGroup(f, "Lens geometry");` section with 4 Float_knobs:
     - `float _outer_pupil_curvature_radius;` default `90.77f`, range -1000 to 1000
     - `float _lens_length;` default `100.89f`, range 0 to 1000
     - `float _aperture_housing_radius;` default `14.10f`, range 0 to 100
     - `float _inner_pupil_curvature_radius;` default `-112.58f`, range -1000 to 1000
     - `EndGroup(f);` after the 4 knobs
   - In `knob_changed`: add check for `"aperture_file"` setting `_reload_aperture = true`
   - In `_validate(for_real)`: add aperture poly loading block (same pattern as exitpupil poly — read `_aperture_file` into `_aperture_sys` when `_reload_aperture || !_aperture_loaded`)
   - In `_close` and destructor: also destroy `_aperture_sys` if loaded
   - renderStripe stays the same zero-output stub as Thin

3. **Verify both files have the required content:**
   - `grep -q '_max_degree' src/DeepCDefocusPOThin.cpp`
   - `grep -q 'aperture_file' src/DeepCDefocusPORay.cpp`
   - `grep -q 'outer_pupil_curvature_radius' src/DeepCDefocusPORay.cpp`
   - `grep -q 'holdout' src/DeepCDefocusPOThin.cpp`
   - `grep -q 'holdout' src/DeepCDefocusPORay.cpp`

4. **Verify the stub renderStripe pattern:**
   - `grep -q 'writableAt.*= 0' src/DeepCDefocusPOThin.cpp`
   - `grep -q 'writableAt.*= 0' src/DeepCDefocusPORay.cpp`

## Must-Haves

- [ ] DeepCDefocusPOThin.cpp exists with correct class name, Op::Description, all Thin knobs including max_degree (Int_knob, default 11, range 1–11)
- [ ] DeepCDefocusPORay.cpp exists with correct class name, Op::Description, all Thin knobs plus aperture_file, plus 4 lens geometry Float_knobs with Angenieux 55mm defaults in a "Lens geometry" closed group
- [ ] Both have stub renderStripe that zeros all output channels
- [ ] Both preserve holdout input wiring (input 1 as "holdout", test_input, getRequests)
- [ ] Both preserve _validate poly loading, _close cleanup, destructor cleanup
- [ ] Ray variant has second poly_system_t for aperture, loaded in _validate
- [ ] Both include poly.h and deepc_po_math.h

## Verification

- `grep -q 'DeepCDefocusPOThin' src/DeepCDefocusPOThin.cpp`
- `grep -q 'DeepCDefocusPORay' src/DeepCDefocusPORay.cpp`
- `grep -q '_max_degree' src/DeepCDefocusPOThin.cpp && grep -q '_max_degree' src/DeepCDefocusPORay.cpp`
- `grep -q 'aperture_file' src/DeepCDefocusPORay.cpp`
- `grep -q '90.77' src/DeepCDefocusPORay.cpp` — outer_pupil_curvature_radius default
- `grep -q 'writableAt.*= 0' src/DeepCDefocusPOThin.cpp`
- `grep -q 'holdout' src/DeepCDefocusPOThin.cpp && grep -q 'holdout' src/DeepCDefocusPORay.cpp`

## Inputs

- `src/DeepCDefocusPO.cpp` — template to copy from (518 lines, complete working plugin)
- `src/poly.h` — T01 output (needed for include, now has max_degree parameter)
- `src/deepc_po_math.h` — T01 output (needed for include, now has sphereToCs)

## Expected Output

- `src/DeepCDefocusPOThin.cpp` — new thin-lens scaffold plugin
- `src/DeepCDefocusPORay.cpp` — new raytraced gather scaffold plugin
