# S01 Research: Shared Infrastructure, max_degree Knob, Two-Plugin Scaffold

**Slice:** S01 — Shared infrastructure, max_degree knob, two-plugin scaffold  
**Risk:** Low  
**Dependencies:** None (first slice)

---

## Summary

This is well-understood work: copy-and-adapt the working `DeepCDefocusPO.cpp` scaffold into two new .cpp files, add a `max_degree` Int_knob, port `sphereToCs` to `deepc_po_math.h`, wire up CMake, and extend `verify-s01-syntax.sh`. No new DDImage APIs. No Newton iteration in this slice. The existing code is a near-complete template — this is primarily a structured copy-and-modify job.

---

## Requirements Targeted

- **R032** — `max_degree` Int_knob on both nodes (primary owner: S01)
- **R035** — Holdout, CA, Halton preserved on both (structural setup; full proof in S02/S03)
- **R036** — Both nodes registered in Nuke's Filter menu (CMake + Description)
- **R030** — DeepCDefocusPOThin: thin-lens CoC scatter (scaffold only; engine in S02)
- **R031** — DeepCDefocusPORay: raytraced gather (scaffold only; engine in S03)

---

## Implementation Landscape

### Files That Change

| File | What happens |
|------|-------------|
| `src/DeepCDefocusPOThin.cpp` | **NEW** — scaffold from DeepCDefocusPO.cpp, with max_degree knob |
| `src/DeepCDefocusPORay.cpp` | **NEW** — scaffold from DeepCDefocusPO.cpp, with max_degree + aperture_file + lens geometry knobs |
| `src/poly.h` | Add `max_degree` parameter to `poly_system_evaluate` |
| `src/deepc_po_math.h` | Add `sphereToCs` function (port from lentil) |
| `src/CMakeLists.txt` | Replace DeepCDefocusPO with DeepCDefocusPOThin + DeepCDefocusPORay |
| `scripts/verify-s01-syntax.sh` | Add both new .cpp files to syntax check loop; update CMake grep counts |

### Files That Are REMOVED

- `src/DeepCDefocusPO.cpp` — superseded by both new files. Remove from `PLUGINS` in CMakeLists.txt.

---

## Key Findings

### 1. DeepCDefocusPO.cpp is the correct template

The existing file is complete and working. The non-scatter code (knobs, `_validate`, holdout fetch, zero_output, poly loading, CA wavelengths, Halton+Shirley) can be cloned directly. Important architectural facts:
- Poly loading happens in `_validate(for_real)` — the CONTEXT.md note about "renderStripe" is stale. The working code and the pattern to replicate is _validate.
- Two `poly_system_t` members will be needed for the Ray variant (exitpupil + aperture).
- ODR: each plugin is a separate .so, so each can `#include "poly.h"` in exactly one TU. No conflict between Thin.so and Ray.so.

### 2. poly.h lacks max_degree parameter — must be added

Current `poly_system_evaluate` signature:
```cpp
inline void poly_system_evaluate(const poly_system_t* sys,
                                 const float* input,
                                 float* output,
                                 int num_out)
```

There is NO max_degree parameter. To support early termination:
- Terms in .fit files are stored sorted by ascending total degree (roadmap states this).
- Total degree of a term = `sum(exp[0..4])` across all 5 exponents.
- When `sum(exp) > max_degree`, break the inner loop.
- Add `max_degree = -1` as a default (meaning: evaluate all terms, backward-compatible).

The `deepc_po_math.h` forward declaration of `poly_system_evaluate` must also be updated to include `max_degree = -1`.

The `lt_newton_trace` calls in `deepc_po_math.h` already use `poly_system_evaluate` — they will get the default (all terms) unless the new plugins explicitly pass a `max_degree` value. This is correct: backward-compatible, and the knob value gets passed at the call sites in renderStripe.

### 3. sphereToCs is not present in deepc_po_math.h — must be ported

The lentil source directory is **not present** in this worktree (`lentil/` does not exist). `sphereToCs` must be ported from memory of the lentil algorithm + the math.

The lentil `sphereToCs` converts a 2D point on a sphere's surface (in exit-pupil coordinates, output of the polynomial) to a 3D Cartesian ray direction. Standard spherical → Cartesian conversion with the sphere's curvature radius:

```cpp
// Converts a 2D point on a sphere of radius R to a unit 3D direction vector.
// (x, y) are the tangential coordinates on the sphere surface.
// R = outer_pupil_curvature_radius (from lens constants).
inline void sphereToCs(float x, float y, float R,
                       float& out_x, float& out_y, float& out_z)
{
    // R can be negative (concave sphere)
    const float r2 = x*x + y*y;
    // z on sphere: z = R - sqrt(R^2 - r^2), with sign of R
    // For lentil: if R > 0 (convex), z is positive toward sensor
    const float disc = R*R - r2;
    if (disc < 0.0f) {
        // Ray misses sphere — set degenerate direction
        out_x = x; out_y = y; out_z = 0.0f;
        return;
    }
    out_z = R - std::copysign(std::sqrt(disc), R);
    // The 3D point is (x, y, out_z) on the sphere
    // Direction from origin is the normalised vector
    const float len = std::sqrt(x*x + y*y + out_z*out_z);
    if (len < 1e-9f) { out_x = 0; out_y = 0; out_z = 1; return; }
    out_x = x / len;
    out_y = y / len;
    out_z = out_z / len;
}
```

> **Planner note:** Verify sign conventions against lentil's `gencode.h` `print_lt_sample_aperture` output during S03 implementation. For S01 the function only needs to compile — correctness proof is in S03.

### 4. CMakeLists.txt changes

Current state:
- `PLUGINS` list line 16: `DeepCDefocusPO`
- `FILTER_NODES` line 53: `DeepCBlur DeepCBlur2 DeepThinner DeepCDepthBlur DeepCDefocusPO`

Required:
- Remove `DeepCDefocusPO` from both
- Add `DeepCDefocusPOThin` and `DeepCDefocusPORay` to both
- The `add_nuke_plugin` loop already handles any name in `PLUGINS` — no per-plugin boilerplate needed
- No Qt or FastNoise dependency for either new plugin

### 5. verify-s01-syntax.sh changes

The script's for-loop:
```bash
for src_file in DeepCBlur.cpp DeepCDepthBlur.cpp DeepCDefocusPO.cpp; do
```
Must be updated to check the two new files. `DeepCDefocusPO.cpp` should be removed (it will be deleted). New list: `DeepCBlur.cpp DeepCDepthBlur.cpp DeepCDefocusPOThin.cpp DeepCDefocusPORay.cpp`.

The mock headers in the script already cover all needed DDImage APIs (PlanarIop, ImagePlane, DeepOp, Knobs, Box, Channel). The two new plugins use the same API surface plus `Float_knob` with `SetRange` — already stubbed. No new mocks needed unless Ray variant uses an API not yet mocked.

**Watch out:** The S05 contracts at the bottom of `verify-s01-syntax.sh` check:
```bash
grep -c 'DeepCDefocusPO' "$SRC_DIR/CMakeLists.txt" | grep -q '^2$'
```
This must be updated: after removing the old plugin and adding two new ones, there will be 4 occurrences (2 for Thin, 2 for Ray). The contract check needs to be restructured for the new plugin names.

### 6. aperture.fit loading for Ray variant

The Ray scaffold needs TWO File_knobs: `poly_file` (exitpupil.fit) and `aperture_file` (aperture.fit). Each will have its own `poly_system_t` member and load in `_validate(for_real)`. Since renderStripe is a stub in S01, the loading path just needs to compile — correctness proof is in S03.

### 7. Lens geometry knobs for Ray variant

Per D034, expose as Float_knobs with Angenieux 55mm defaults:
- `outer_pupil_curvature_radius` = 90.77
- `lens_length` = 100.89
- `aperture_housing_radius` = 14.10
- `inner_pupil_curvature_radius` = -112.58

These can go in a separate BeginClosedGroup("Lens geometry") for UI clarity.

---

## Knob Inventory

### DeepCDefocusPOThin knobs (copy from DeepCDefocusPO.cpp + add max_degree):

| Knob | Type | Default | Range |
|------|------|---------|-------|
| poly_file | File_knob | "" | — |
| focal_length | Float_knob | 50.0 | 1–1000 |
| focus_distance | Float_knob | 200.0 | 1–100000 |
| fstop | Float_knob | 2.8 | 0.5–64 |
| aperture_samples | Int_knob | 64 | 1–4096 |
| **max_degree** | **Int_knob** | **11** | **1–11** |

### DeepCDefocusPORay knobs (same + aperture_file + lens geometry):

All Thin knobs above, plus:

| Knob | Type | Default | Range |
|------|------|---------|-------|
| aperture_file | File_knob | "" | — |
| outer_pupil_curvature_radius | Float_knob | 90.77 | -1000–1000 |
| lens_length | Float_knob | 100.89 | 0–1000 |
| aperture_housing_radius | Float_knob | 14.10 | 0–100 |
| inner_pupil_curvature_radius | Float_knob | -112.58 | -1000–1000 |

---

## Verification Plan

After S01 implementation:

```bash
# 1. Syntax check — both new files must pass
bash scripts/verify-s01-syntax.sh

# 2. CMake check — names appear correctly
grep 'DeepCDefocusPOThin' src/CMakeLists.txt  # expect 2 lines (PLUGINS + FILTER_NODES)
grep 'DeepCDefocusPORay'  src/CMakeLists.txt  # expect 2 lines (PLUGINS + FILTER_NODES)
grep 'DeepCDefocusPO[^T]' src/CMakeLists.txt  # expect 0 lines (old plugin gone)

# 3. max_degree knob present in both
grep 'max_degree' src/DeepCDefocusPOThin.cpp
grep 'max_degree' src/DeepCDefocusPORay.cpp

# 4. poly.h has max_degree parameter
grep 'max_degree' src/poly.h

# 5. sphereToCs present in math header
grep 'sphereToCs' src/deepc_po_math.h

# 6. Both use stub renderStripe (zeros output)
grep -c 'writableAt.*= 0' src/DeepCDefocusPOThin.cpp  # >= 1
grep -c 'writableAt.*= 0' src/DeepCDefocusPORay.cpp   # >= 1
```

The docker-build.sh test is a future gate (S02/S03). S01 proof is syntax check + CMake grep.

---

## Natural Task Decomposition

The planner should decompose S01 into these independent tasks:

**T01** — Modify `src/poly.h`: add `max_degree` parameter with default `-1` to `poly_system_evaluate`. Update `deepc_po_math.h` forward declaration. Add `sphereToCs` to `deepc_po_math.h`.

**T02** — Create `src/DeepCDefocusPOThin.cpp`: copy from DeepCDefocusPO.cpp, rename class, add `max_degree` Int_knob (and member `_max_degree`), stub renderStripe to zero output and return.

**T03** — Create `src/DeepCDefocusPORay.cpp`: copy from DeepCDefocusPO.cpp, rename class, add `max_degree` + `aperture_file` + lens geometry knobs, stub renderStripe to zero output and return. Second `poly_system_t` member for aperture poly.

**T04** — CMake + verify-s01-syntax.sh: update PLUGINS + FILTER_NODES (remove old, add two new); update syntax script loop + grep contracts; optionally delete DeepCDefocusPO.cpp (or leave file on disk but removed from CMake).

Tasks T01–T04 can be executed in parallel except T02/T03 depend on T01 completing first (they need the updated poly.h/deepc_po_math.h signatures).

---

## Forward Intelligence (for S02 and S03)

- **poly.h max_degree signature:** `poly_system_evaluate(sys, input, output, num_out, max_degree=-1)`. The `lt_newton_trace` in deepc_po_math.h omits the `max_degree` arg (gets all-terms default). The new renderStripe bodies in S02/S03 pass `_max_degree` explicitly.
- **sphereToCs sign conventions:** Only needed at runtime in S03. The stub in S01 just needs to compile; S03 should validate against lentil's `print_lt_sample_aperture` output structure.
- **Stripe seam issue:** Inherits from the existing scatter pattern in DeepCDefocusPO — documented as accepted for S02. Both thin and ray scatter can drop contributions outside the stripe bounds.
- **poly loading in _validate:** Follow the existing pattern (_validate not renderStripe, despite CONTEXT.md's note — the working code uses _validate and it's correct for PlanarIop).
- **ODR per .so:** poly.h is included once in each plugin's sole .cpp. Each .so is a separate translation unit — no ODR conflict. But never include poly.h in deepc_po_math.h.
