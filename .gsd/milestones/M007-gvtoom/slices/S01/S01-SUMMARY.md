# S01 Summary: Shared Infrastructure, max_degree Knob, Two-Plugin Scaffold

**Status:** Complete  
**Completed:** 2026-03-24  
**Duration:** ~32 minutes across 3 tasks

## What This Slice Delivered

S01 established the complete shared infrastructure for the M007 two-plugin replacement of `DeepCDefocusPO`:

1. **`src/poly.h`** — `poly_system_evaluate` now accepts `int max_degree = -1` with early-exit when a term's total degree (sum of absolute exponents) exceeds the limit. Break (not continue) is correct because `.fit` terms are sorted ascending by degree — once a term exceeds the limit, all subsequent terms will too. Default `-1` preserves full evaluation for existing callers (`lt_newton_trace` et al.).

2. **`src/deepc_po_math.h`** — `sphereToCs` added: maps a 2D point on a sphere of radius R to a normalised 3D direction vector. Safe fallback returns `(x, y, 0)` for out-of-sphere inputs (disc < 0) instead of NaN. Forward declaration of `poly_system_evaluate` updated to include `max_degree = -1`.

3. **`src/DeepCDefocusPOThin.cpp`** — PlanarIop scaffold with all thin-lens knobs: `poly_file`, `focal_length`, `focus_distance`, `fstop`, `aperture_samples`, `max_degree` (Int_knob, default 11, range 1–11). `_validate` loads poly and calls `error()` on failure. Holdout input, CA wavelengths (0.45/0.55/0.65 as `static constexpr` members), Halton+Shirley sampling, `knob_changed` reload, `_close`/destructor cleanup all preserved. `renderStripe` is a stub that zeros all output channels. Registered as `"Deep/DeepCDefocusPOThin"`.

4. **`src/DeepCDefocusPORay.cpp`** — extends the Thin scaffold with: `aperture_file` File_knob, second `poly_system_t _aperture_sys` with independent load/reload and separate error message, 4 lens geometry Float_knobs in a closed "Lens geometry" group (`outer_pupil_curvature_radius=90.77`, `lens_length=100.89`, `aperture_housing_radius=14.10`, `inner_pupil_curvature_radius=-112.58` — Angenieux 55mm defaults). Stub `renderStripe` zeros output. Registered as `"Deep/DeepCDefocusPORay"`.

5. **`src/CMakeLists.txt`** — `DeepCDefocusPOThin` and `DeepCDefocusPORay` present in both `PLUGINS` and `FILTER_NODES` (2 occurrences each). `DeepCDefocusPO` fully removed (0 occurrences).

6. **`scripts/verify-s01-syntax.sh`** — syntax-checks `DeepCDefocusPOThin.cpp` and `DeepCDefocusPORay.cpp` (replacing the old file). S05 contracts validate both new plugin names in CMake.

7. **`src/DeepCDefocusPO.cpp`** — deleted. No longer in the build.

## Verification Results

All slice-level checks passed:

| Check | Result |
|---|---|
| `bash scripts/verify-s01-syntax.sh` | ✅ exit 0 |
| `grep -c 'DeepCDefocusPOThin' src/CMakeLists.txt` == 2 | ✅ |
| `grep -c 'DeepCDefocusPORay' src/CMakeLists.txt` == 2 | ✅ |
| `grep -c 'DeepCDefocusPO[^TR]' src/CMakeLists.txt` == 0 | ✅ |
| `max_degree` in Thin.cpp, Ray.cpp, poly.h | ✅ |
| `sphereToCs` in deepc_po_math.h | ✅ |
| `writableAt.*= 0` in both scaffolds | ✅ |
| `error.*Cannot open lens file` in both | ✅ |
| `error.*Cannot open aperture file` in Ray | ✅ |
| `! -f src/DeepCDefocusPO.cpp` | ✅ |

## Patterns Established for S02/S03

- **max_degree truncation**: `poly_system_evaluate(sys, in, out, max_degree)` — pass `_max_degree` from the knob. Value `-1` = all terms.
- **CA wavelength constants**: `static constexpr float WL_R = 0.65f`, `WL_G = 0.55f`, `WL_B = 0.45f` as class members; iterate across them in renderStripe loops.
- **Two-poly pattern (Ray only)**: `_aperture_sys` mirrors `_lens_sys` — load in `_validate`, destroy in `_close` and destructor, reload in `knob_changed`.
- **Stub → engine replacement**: S02 replaces the `writableAt` zero loop in Thin with the scatter engine; S03 does the same for Ray. All surrounding infrastructure (holdout fetch, getRequests, _validate, knob_changed) is already in place and must not be disturbed.
- **S05 contracts in verify script**: grep-count contracts for both new plugin names should be maintained in the `# S05 contracts` block at the bottom of `verify-s01-syntax.sh`.

## What S02 Needs to Know

- The Thin scaffold's `renderStripe` stub is the only thing to replace — all surrounding code is complete.
- CA wavelengths are available as `WL_R`, `WL_G`, `WL_B` — no need to redeclare.
- `_max_degree` is an `int` member, already wired to the knob. Pass it directly to `poly_system_evaluate`.
- `coc_radius()` in `deepc_po_math.h` takes `focal_length_mm`, `fstop`, `depth`, `focus_dist` — all knob members are already present.
- The holdout fetch pattern from M006's DeepCDefocusPO is preserved verbatim; consult that file if reference is needed (it was deleted, but the holdout section is in both new scaffolds).

## What S03 Needs to Know

- `sphereToCs` is in `deepc_po_math.h` — use it to convert polynomial output (2D aperture point) to a 3D ray direction.
- Angenieux 55mm lens geometry defaults are already baked in as knob defaults. Artists can override via UI.
- `_aperture_sys` is loaded alongside `_lens_sys`. Both are available in `renderStripe` scope.
- `_max_degree` applies to both polynomial evaluations — use consistent truncation for lens and aperture polys.

## Requirements Updated

- **R032** (max_degree knob): structural proof complete — Int_knob present in both plugins, early-exit in poly.h. Runtime proof (visible quality change) deferred to S02/S03 execution.
- **R035** (holdout, CA, Halton preserved): structural proof complete in both scaffolds. Runtime proof deferred to S02/S03.
- **R036** (both nodes in Nuke menu): CMake entries confirm both appear under Filter category. Runtime proof (actual Nuke menu) deferred to docker build.
