# S02 UAT: Ray path-trace engine

**Slice:** S02 — Ray path-trace engine
**Milestone:** M008-y32v8w

This UAT script validates that the gather path-trace engine was correctly implemented in `DeepCDefocusPORay`. The slice's proof level is **contract** (structural + syntax), not runtime. Full runtime proof (docker build + nuke -x) is the milestone DoD. All tests here can be run without Docker or Nuke.

---

## Preconditions

- Working directory: repo root (where `src/` and `scripts/` live)
- g++ available in PATH (any version with C++17 support)
- All S01 source files already in place (`src/deepc_po_math.h`, `src/DeepCDefocusPORay.cpp` with S01 knobs, `src/DeepCDefocusPOThin.cpp`)

---

## Test Cases

### TC-01: Full verify script passes (S01 + S02 contracts)

**What it proves:** All structural contracts for both slices pass and all 4 source files compile cleanly.

**Steps:**
1. `bash scripts/verify-s01-syntax.sh`

**Expected output (exact):**
```
Syntax check passed: DeepCBlur.cpp
Syntax check passed: DeepCDepthBlur.cpp
Syntax check passed: DeepCDefocusPOThin.cpp
Syntax check passed: DeepCDefocusPORay.cpp
--- S01 contracts ---
PASS: _validate format fix in Thin
PASS: _validate format fix in Ray
PASS: _sensor_width knob in Ray
PASS: _back_focal_length knob in Ray
PASS: _outer_pupil_radius knob in Ray
PASS: _inner_pupil_radius knob in Ray
PASS: _aperture_pos knob in Ray
PASS: pt_sample_aperture in deepc_po_math.h
PASS: sphereToCs_full in deepc_po_math.h
PASS: logarithmic_focus_search in deepc_po_math.h
PASS: max_degree in poly.h
PASS: DeepCDefocusPOThin in CMakeLists.txt
PASS: DeepCDefocusPORay in CMakeLists.txt
All S01 contracts passed.
--- S02 contracts ---
PASS: pt_sample_aperture called in Ray
PASS: sphereToCs_full called in Ray
PASS: logarithmic_focus_search called in Ray
PASS: VIGNETTING_RETRIES in Ray
PASS: getPixel in Ray
PASS: coc_norm removed from Ray
All S02 contracts passed.
All syntax checks passed.
```

**Exit code:** 0

**Failure:** Any FAIL line or non-zero exit indicates a missing or broken implementation element.

---

### TC-02: Scatter vestige absent — coc_norm removed

**What it proves:** The old scatter engine's normalised CoC variable is fully gone from Ray.

**Steps:**
1. `grep -q 'coc_norm' src/DeepCDefocusPORay.cpp && echo FOUND || echo NOT_FOUND`

**Expected output:** `NOT_FOUND`

**Expected exit code of grep:** 1 (not found = correct)

---

### TC-03: Gather engine key functions present in Ray

**What it proves:** All five core path-trace functions are actually called (not just declared) in the Ray renderStripe.

**Steps:**
1. `grep -n 'pt_sample_aperture\|sphereToCs_full\|logarithmic_focus_search\|VIGNETTING_RETRIES\|getPixel' src/DeepCDefocusPORay.cpp`

**Expected:** At least one hit per function in the output. Key line patterns:
- `pt_sample_aperture(sx, sy, dx, dy, ax, ay, lambda, sensor_shift, &_poly_sys, _max_degree)` — Newton call in retry loop
- `sphereToCs_full(out5[0], out5[1], out5[2], out5[3], ...)` — 3D ray construction
- `logarithmic_focus_search(...)` — called before the output pixel loop, assigned to `const float sensor_shift`
- `VIGNETTING_RETRIES` — used as loop bound in retry loop
- `getPixel` — used to fetch deep column at projected input pixel

---

### TC-04: Sensor mm convention — y uses half_w not half_h

**What it proves:** The sensor coordinate computation uses the correct lentil convention (y normalised by half_w, not half_h).

**Steps:**
1. `grep -n 'half_w\|half_h\|sy =' src/DeepCDefocusPORay.cpp | head -20`

**Expected:** Lines show:
- `half_w` computed from `fmt.width() * 0.5f`
- `half_h` computed from `fmt.height() * 0.5f`
- The `sy` line references `_sensor_width / (2.0f * half_w)` — **same divisor as sx** (not `half_h`)

**Failure pattern:** If `sy` uses `half_h` in the divisor, the sensor y coordinate is incorrectly scaled for non-square images.

---

### TC-05: CA wavelength tracing preserved

**What it proves:** Chromatic aberration tracing at three wavelengths is present in the gather engine (R035 regression check).

**Steps:**
1. `grep -n '0\.45f\|0\.55f\|0\.65f' src/DeepCDefocusPORay.cpp`

**Expected:** All three wavelengths appear in a `lambdas[]` array or equivalent loop construct in the renderStripe gather engine.

---

### TC-06: VIGNETTING_RETRIES constant value

**What it proves:** The retry limit is set to the specified value of 10.

**Steps:**
1. `grep -n 'VIGNETTING_RETRIES' src/DeepCDefocusPORay.cpp`

**Expected:** One line defining `VIGNETTING_RETRIES = 10` and at least one line using it as a loop bound.

---

### TC-07: getRequests expanded bounds present

**What it proves:** The deep request box is padded for gather-style ray coverage outside stripe bounds.

**Steps:**
1. `grep -n 'aperture_housing_radius\|max_coc\|pad' src/DeepCDefocusPORay.cpp | head -10`

**Expected:** Lines showing `max_coc_pixels` or equivalent padding computation in `getRequests`, involving `aperture_housing_radius` and `sensor_width`.

---

### TC-08: No Option B warp vestiges

**What it proves:** The old thin-lens Option B polynomial warp code (which computed a warp offset and applied it to scatter positions) is fully removed.

**Steps:**
1. `grep -n 'Option B\|warp\|ap_radius\|coc_radius' src/DeepCDefocusPORay.cpp`

**Expected:** Empty output (no matches). Any match indicates scatter vestige remains.

---

### TC-09: all_channels_ok flag — no partial CA accumulation

**What it proves:** The all-or-nothing CA channel policy is implemented — a failed channel aborts the whole aperture sample.

**Steps:**
1. `grep -n 'all_channels_ok\|channels_ok\|channel.*fail\|abort.*channel' src/DeepCDefocusPORay.cpp`

**Expected:** At least one line defining a boolean flag for channel-level rejection, and the flag checked before accumulating the aperture sample's RGBA contribution.

---

## Edge Cases (Manual Inspection)

### EC-01: Field-edge pixel behaviour under full vignetting

**Scenario:** An output pixel at the image edge where all 10 retry attempts are vignetted.

**Expected behaviour:** That aperture sample contributes zero to the output — pixel accumulates fewer valid samples, appears darker. This is physically correct vignetting, not an error. No assert/crash should occur.

**How to verify at runtime (milestone DoD, requires Docker):** Render a 128×72 frame and inspect corners — some darkening is expected and correct. Fully black corners with no gradual falloff would indicate a bug in the retry or accumulation logic.

### EC-02: focus_distance = 0 or near-zero

**Scenario:** `_focus_distance` knob set to 0.

**Expected:** `logarithmic_focus_search` should handle gracefully (returns some shift value or 0). No divide-by-zero crash in the gather engine.

### EC-03: Holdout not connected

**Scenario:** No holdout input connected.

**Expected:** Flatten proceeds as standard front-to-back deep composite (transmittance weight = 1 per sample). Output should match a non-holdout render.

---

## Runtime Verification (Milestone DoD — requires Docker + Nuke)

These tests are NOT required for S02 sign-off but are the next step:

```bash
bash scripts/docker-build.sh --linux --versions 16.0
nuke -x test/test_ray.nk   # expect: exit 0, non-black 128×72 EXR
nuke -x test/test_thin.nk  # expect: exit 0, regression check
```
