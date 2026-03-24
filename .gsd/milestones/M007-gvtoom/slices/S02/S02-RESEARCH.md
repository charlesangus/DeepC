# S02 Research: DeepCDefocusPOThin Scatter Engine

**Slice:** S02 — DeepCDefocusPOThin scatter engine  
**Milestone:** M007-gvtoom  
**Researched:** 2026-03-24  
**Complexity:** Targeted — known technology (PlanarIop, poly.h, deepc_po_math.h), new scatter math  
**Requirements Owned:** R030 (primary), R032, R035 (both already structural; S02 provides runtime proof)

---

## Summary

S02 replaces the zero-output stub in `DeepCDefocusPOThin.cpp::renderStripe` with a working thin-lens CoC scatter engine. Everything else — scaffolding, knobs, `_validate`, holdout fetch, `getRequests`, poly loading, CA wavelength constants, Halton+Shirley sampling infrastructure — is **already in place from S01** and must not be touched.

The slice also creates `test/test_thin.nk` (adapted from the existing `test/test_deepcdefocus_po.nk`) and fixes one inherited defect in the mock header: `chanNo()` is missing from the `ImagePlane` stub.

Work is **medium risk**: the scatter math is straightforward; the only design question is how to use the polynomial to modulate bokeh shape within the CoC disk. Two viable options are documented below.

---

## What Exists (S01 Deliverables)

### `src/DeepCDefocusPOThin.cpp`

All infrastructure is complete. The `renderStripe` stub (lines ~288–302) contains only:

```cpp
void renderStripe(ImagePlane& imagePlane) override {
    imagePlane.makeWritable();
    const Box& bounds = imagePlane.bounds();
    const ChannelSet& chans = imagePlane.channels();
    foreach(z, chans)
        for (int y = bounds.y(); y < bounds.t(); ++y)
            for (int x = bounds.x(); x < bounds.r(); ++x)
                imagePlane.writableAt(x, y, z) = 0.0f;  // ← WRONG: see chanNo issue below
}
```

S02 replaces this body in its entirety.

**Available members in scope during renderStripe:**
- `_poly_sys` — loaded poly system (load/reload pattern already in M006 code; to be added back in renderStripe)
- `_poly_loaded`, `_reload_poly` — poly state flags
- `_poly_file` — file path
- `_focal_length_mm` — Float_knob, default 50.0f
- `_focus_distance` — Float_knob, default 200.0f
- `_fstop` — Float_knob, default 2.8f
- `_aperture_samples` — Int_knob, default 64
- `_max_degree` — Int_knob, default 11
- `static constexpr float CA_LAMBDA_R = 0.45f`, `CA_LAMBDA_G = 0.55f`, `CA_LAMBDA_B = 0.65f`
- `input0()` / `input1()` (holdout) — DeepOp accessors

**NOTE**: `_validate` in the S01 scaffold loads the poly (unlike M006 where loading was deferred to renderStripe for thread-safety). If this is kept, renderStripe must not re-load. But if `_validate` loads in `for_real` only and renderStripe also checks `_reload_poly`, the pattern is safe. The M006 KNOWLEDGE note says load in renderStripe to avoid data races (M006/S06 lesson). S02 should follow the M006 pattern: move or replicate the poly load/reload check into renderStripe, same as M006.

### `src/deepc_po_math.h`

Provides: `halton(index, base)`, `map_to_disk(u, v, &x, &y)`, `coc_radius(fl_mm, fstop, focus_dist, depth)`.

`coc_radius` returns CoC in **mm** (same units as `focal_length_mm`). Convert to normalised screen coords:
```
coc_norm = coc_mm / (focal_length_mm * 0.5f + 1e-6f)
```
This is the same approximation used for CoC culling in M006 and is consistent with `ap_radius = 1.0f / fstop` (normalised aperture disk radius).

### `src/poly.h`

`poly_system_evaluate(sys, in5, out5, num_out, max_degree)`:
- **inputs**: `float[5] = {sensor_x, sensor_y, aperture_x, aperture_y, lambda_um}`
- **outputs**: `float[5] = {aperture_x', aperture_y', sensor_x', sensor_y', lambda'}`
- All coordinates are in normalised `[-1, 1]` units
- `out5[0:1]` = aberrated exit pupil position (same units as input aperture)
- `out5[2:3]` = exit ray direction at sensor (spherical coords on the outer pupil — **NOT** scatter pixel positions)
- `out5[4]` = transmittance [0,1]
- Pass `_max_degree` from knob for R032

---

## The Core Fix: Why M006 Was Wrong

M006 used `out5[2:3]` as the landing pixel position (scatter destination). These are **exit ray directions on the outer pupil surface in spherical coordinates** — not sensor positions. That's why M006 rendered the aperture/pupil shape instead of defocus.

For the Thin variant, landing positions come from **thin-lens CoC physics**, not the polynomial. The poly's role is to *modulate* where within the CoC disk each sample lands (adding aberrations), not to determine the CoC size.

---

## Scatter Algorithm: Two Options

### Option A — Pure CoC Scatter + Poly Transmittance Only (Safest)

Landing position from pure thin-lens CoC; polynomial contributes only transmittance weighting.

```
coc_norm = coc_radius(fl, fstop, focus_dist, depth) / (fl * 0.5f)
ap_radius = 1.0f / fstop

for k in 0..N-1:
    ax_unit, ay_unit = map_to_disk(halton(k,2), halton(k,3))
    for c in {R=0.45, G=0.55, B=0.65}:
        ax = ax_unit * ap_radius
        ay = ay_unit * ap_radius
        poly_system_evaluate(sys, {sx0, sy0, ax, ay, lambda[c]}, out5, 5, _max_degree)
        transmit = clamp(out5[4], 0, 1)
        landing_x = sx0 + coc_norm * ax_unit
        landing_y = sy0 + coc_norm * ay_unit
        weight = transmit / N
        accumulate at (landing_x, landing_y) with weight
```

**Pros**: Guaranteed defocus. Zero risk of landing outside CoC. Poly transmittance gives cat-eye vignetting.  
**Cons**: No positional aberrations (no coma, astigmatism, positional CA). Satisfies R030 letter but minimally.

### Option B — CoC Scatter + Poly-Warped Aperture (Recommended, D032)

Use `out5[0:1]` (aberrated exit pupil position) as the warped aperture sample, scaled to CoC disk. This is what D032 means by "polynomial warps aperture sample positions within the CoC disk."

```
for c in {R, G, B}:
    ax = ax_unit * ap_radius
    ay = ay_unit * ap_radius
    poly_system_evaluate(sys, {sx0, sy0, ax, ay, lambda[c]}, out5, 5, _max_degree)
    transmit = clamp(out5[4], 0, 1)

    // out5[0:1] are in same normalised units as (ax, ay)
    // Clamp to aperture disk to prevent wild scattering
    float wx = out5[0], wy = out5[1];
    float wmag = sqrtf(wx*wx + wy*wy);
    if (wmag > ap_radius) { wx *= ap_radius/wmag; wy *= ap_radius/wmag; }

    // Scale warped aperture to CoC radius
    landing_x = sx0 + coc_norm * wx / (ap_radius + 1e-6f)
    landing_y = sy0 + coc_norm * wy / (ap_radius + 1e-6f)
    weight = transmit / N
    accumulate at (landing_x, landing_y) with weight
```

**Pros**: Aberrations (cat-eye, coma) modulate bokeh shape as D032 describes. Per-channel CA from wavelength-dependent `out5[0:1]` variation.  
**Cons**: If `out5[0:1]` are near-zero for some inputs, bokeh collapses to a point (acceptable — happens for in-focus samples). Requires testing to verify the scale factor produces visible aberrations.

**Recommendation**: Implement Option B. The clamping guard prevents wild outputs. If poly outputs turn out pathological (e.g., all near-zero at the test lens parameters), fall back to Option A and note this in the SUMMARY.

### Poly Load/Reload in renderStripe

Follow the M006 pattern exactly — the `_validate` note in S01 scaffold is deceptive. The poly must be loaded/reloaded at renderStripe entry (not `_validate`) to avoid thread-safety issues. M006 KNOWLEDGE.md documents this explicitly. Pattern:

```cpp
if (_poly_file && _poly_file[0] && (!_poly_loaded || _reload_poly)) {
    if (_poly_loaded) { poly_system_destroy(&_poly_sys); _poly_loaded = false; }
    if (poly_system_read(&_poly_sys, _poly_file) != 0) { zero_output(); return; }
    _poly_loaded = true; _reload_poly = false;
}
if (!_poly_loaded || !input0()) { zero_output(); return; }
```

---

## chanNo Issue (Must Fix)

### The Bug in S01's Stub

The S01 stub uses `imagePlane.writableAt(x, y, z)` where `z` is a `Channel` enum value. This compiles against the mock header (which accepts `Channel`) but would crash at runtime. The real `ImagePlane::writableAt(int, int, int)` takes a **channel index**, not a Channel enum. `Chan_Alpha = 4` writes one past a 4-element buffer.

**Fix required in S02 renderStripe**: use `imagePlane.chanNo(z)` everywhere:
```cpp
imagePlane.writableAt(x, y, imagePlane.chanNo(z))   // ← correct
imagePlane.writableAt(x, y, z)                       // ← WRONG (S01 stub)
```

Also use `chans.contains(chan)` guard before every `writableAt` call (KNOWLEDGE.md: missing guard → heap corruption via invalid stride offset).

### Mock Header Fix

`chanNo()` is missing from the `ImagePlane` mock in `scripts/verify-s01-syntax.sh`. Also `writableAt` should accept `int` (not `Channel`) to catch enum/index mistakes at syntax check time.

Add to the `ImagePlane` mock:
```cpp
int chanNo(Channel z) const { return static_cast<int>(z); }
float& writableAt(int /*x*/, int /*y*/, int /*z*/) {
    static float dummy = 0.0f;
    return dummy;
}
```

Remove (or keep alongside) the old `writableAt(int,int,Channel)` signature — keep both so the S01 zero-loop still compiles. But the new signature makes the correct pattern visible.

---

## Format Conversion: Normalised ↔ Pixel

Established M006 pattern (KNOWLEDGE.md):
```
Pixel → normalised:  sx = (px + 0.5 - half_w) / half_w
Normalised → pixel:  out_px = landing_x * half_w + half_w - 0.5  (round to nearest int)
```
Where `half_w = fmt.width() * 0.5f`, `half_h = fmt.height() * 0.5f`.  
`fmt` comes from `info_.full_size_format()`.

---

## Holdout Pattern (R035 — Already in Scaffold)

The holdout fetch is already present in both S01 scaffolds. S02's renderStripe must include the identical `transmittance_at` lambda and `holdoutOp->deepEngine(bounds, hNeeded, holdoutPlane)` fetch. Copy verbatim from M006 (git: `f5f1bd1`). The KNOWLEDGE entries for holdout are:

- Unordered iteration is correct (`getUnorderedSample`) — product is commutative
- Depth gate: `if (hzf >= Z) continue` — skip samples at or behind scatter depth Z
- Must fetch holdout at output `bounds`, not input sample position
- `DeepPlane::getPixel(y, x)` — row first (DDImage convention)

---

## Stripe-Boundary Seam (Accepted Limitation)

Scatter contributions landing outside the current stripe are silently discarded. Produces a dark seam at stripe boundaries for large bokeh. This is the accepted M006 limitation (KNOWLEDGE.md). Do NOT attempt to fix in S02.

---

## Test Script: `test/test_thin.nk`

The test directory doesn't exist yet in the worktree — create `test/` directory and write the test script. The reference script is at `/home/latuser/git/DeepC/test/test_deepcdefocus_po.nk`.

Key changes from the reference:
1. Node class: `DeepCDefocusPO` → `DeepCDefocusPOThin`
2. Add `max_degree 11` knob (or leave at default)
3. `poly_file` path: `/home/latuser/git/lentil/polynomial-optics/database/lenses/1953-angenieux-double-gauss/55/fitted/exitpupil.fit` (confirmed present)
4. `focal_length 55` (matching Angenieux 55mm)
5. `focus_distance 10000` (10 metres, in mm)
6. Write to `./test_thin.exr` at 128×72

The test script format must be `128 72` (128×72 resolution for CI gate). The grid checkerboard is generated at 128×72 via `Reformat1 → Grid1 → Dilate → Merge → DeepFromImage`. That chain can be copied verbatim from the reference script.

The `nuke -x test/test_thin.nk` exit-0 + non-black output is the S02 UAT gate.

---

## Implementation Landscape (Files Changed)

| File | Change |
|---|---|
| `src/DeepCDefocusPOThin.cpp` | Replace `renderStripe` stub with full thin-lens scatter engine; add poly load/reload pattern; fix `chanNo` usage throughout |
| `scripts/verify-s01-syntax.sh` | Add `chanNo(Channel)` and `writableAt(int,int,int)` to `ImagePlane` mock |
| `test/test_thin.nk` | New file — adapted from reference test script |

**Files NOT to touch**: `poly.h`, `deepc_po_math.h`, `DeepCDefocusPORay.cpp`, `CMakeLists.txt`, knob definitions in `DeepCDefocusPOThin.cpp`.

---

## Task Decomposition Recommendation

### T01: Implement renderStripe scatter engine + fix mock header
**Scope**: 
- `src/DeepCDefocusPOThin.cpp` — replace `renderStripe` stub with full scatter loop
- `scripts/verify-s01-syntax.sh` — add `chanNo` + `writableAt(int,int,int)` to ImagePlane mock
- Verify: `bash scripts/verify-s01-syntax.sh` exits 0

### T02: Write test script + run UAT
**Scope**:
- `test/test_thin.nk` — new file, adapted from reference
- Verify: `nuke -x test/test_thin.nk` exits 0, `test_thin.exr` is non-black
- Secondary verify: non-black proof (e.g., `exrstat test_thin.exr` shows non-zero luminance)

---

## Verification Contracts

All checks must pass before marking S02 complete:

```bash
# Syntax check (fast gate)
bash scripts/verify-s01-syntax.sh  # must exit 0

# Scatter engine structural checks
grep -q 'coc_radius' src/DeepCDefocusPOThin.cpp        # CoC from thin-lens
grep -q 'halton' src/DeepCDefocusPOThin.cpp             # Halton sampling
grep -q 'map_to_disk' src/DeepCDefocusPOThin.cpp        # Shirley disk mapping
grep -q 'chanNo' src/DeepCDefocusPOThin.cpp             # Correct channel index
grep -q 'transmittance_at' src/DeepCDefocusPOThin.cpp   # Holdout (R035)
grep -q '0.45f' src/DeepCDefocusPOThin.cpp              # CA wavelengths (R035)
grep -q '_max_degree' src/DeepCDefocusPOThin.cpp        # max_degree passed to poly (R032)
grep -q 'chans.contains' src/DeepCDefocusPOThin.cpp     # Missing guard → heap corruption

# Test script present
test -f test/test_thin.nk

# Runtime (docker / nuke -x required for proof)
# nuke -x test/test_thin.nk  → exit 0, non-black EXR
```

---

## Risks

| Risk | Severity | Mitigation |
|---|---|---|
| `out5[0:1]` near-zero for Option B | Low | Fall back to Option A; document in SUMMARY |
| `chanNo` mock missing → syntax check still passes | Fixed in T01 | Add `chanNo` to ImagePlane mock before testing scatter engine |
| Stripe-boundary dark seam in test output | Accepted | Document in SUMMARY; 128×72 at low aperture_samples may not show the seam at all |
| Poly file path hardcoded in test .nk | Low | Path is confirmed present; docker CI will need its own path or env var substitution |
| S01 scaffold's `_validate` loads poly (not renderStripe) | Medium | Move/replicate load check to renderStripe; do NOT rely on `_validate` having loaded it |

---

## Skills Discovered

No new skills installed — this slice uses established DDImage PlanarIop patterns already in the codebase. The `code-optimizer` and `best-practices` skills are not relevant to C++ Nuke plugin development at this scope.
