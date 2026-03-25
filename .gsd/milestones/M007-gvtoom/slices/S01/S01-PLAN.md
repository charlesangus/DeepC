# S01: Shared infrastructure, max_degree knob, two-plugin scaffold

**Goal:** Both DeepCDefocusPOThin and DeepCDefocusPORay compile (syntax check), appear in CMake build lists, expose all required knobs including max_degree, and have stub renderStripe that zeros output.
**Demo:** `bash scripts/verify-s01-syntax.sh` passes; grep confirms both plugins in CMake PLUGINS and FILTER_NODES; old DeepCDefocusPO removed from build.

## Must-Haves

- `poly_system_evaluate` in `src/poly.h` accepts `max_degree` parameter with default `-1` (all terms) and early-exits when term degree exceeds limit
- `sphereToCs` function added to `src/deepc_po_math.h` (compiles; correctness verified in S03)
- `src/DeepCDefocusPOThin.cpp` scaffold: PlanarIop, all Thin knobs (poly_file, focal_length, focus_distance, fstop, aperture_samples, max_degree), _validate with poly loading, holdout input, stub renderStripe (zeros output)
- `src/DeepCDefocusPORay.cpp` scaffold: same as Thin plus aperture_file File_knob, 4 lens geometry Float_knobs, second poly_system_t for aperture poly, stub renderStripe (zeros output)
- Both plugins in CMake PLUGINS and FILTER_NODES; old DeepCDefocusPO removed from both
- `scripts/verify-s01-syntax.sh` updated for both new files, old file removed from loop
- Holdout, CA wavelengths (0.45/0.55/0.65), Halton+Shirley sampling preserved in both scaffolds
- `src/DeepCDefocusPO.cpp` deleted (superseded)

## Verification

```bash
# Primary gate: syntax check passes for both new files
bash scripts/verify-s01-syntax.sh

# CMake: both new plugins present, old one gone
test "$(grep -c 'DeepCDefocusPOThin' src/CMakeLists.txt)" -eq 2
test "$(grep -c 'DeepCDefocusPORay' src/CMakeLists.txt)" -eq 2
test "$(grep -c 'DeepCDefocusPO[^TR]' src/CMakeLists.txt)" -eq 0

# max_degree knob in both new files and poly.h
grep -q 'max_degree' src/DeepCDefocusPOThin.cpp
grep -q 'max_degree' src/DeepCDefocusPORay.cpp
grep -q 'max_degree' src/poly.h

# sphereToCs in math header
grep -q 'sphereToCs' src/deepc_po_math.h

# Stub renderStripe (zeros output)
grep -q 'writableAt.*= 0' src/DeepCDefocusPOThin.cpp
grep -q 'writableAt.*= 0' src/DeepCDefocusPORay.cpp

# Failure path: _validate reports error via Nuke error() on bad poly file
grep -q 'error.*Cannot open lens file' src/DeepCDefocusPOThin.cpp
grep -q 'error.*Cannot open lens file' src/DeepCDefocusPORay.cpp
grep -q 'error.*Cannot open aperture file' src/DeepCDefocusPORay.cpp

# Old file gone
test ! -f src/DeepCDefocusPO.cpp
```

## Tasks

- [x] **T01: Add max_degree to poly_system_evaluate and sphereToCs to deepc_po_math.h** `est:30m`
  - Why: Shared infrastructure both new plugins depend on — must land first
  - Files: `src/poly.h`, `src/deepc_po_math.h`
  - Do: Add `int max_degree = -1` parameter to `poly_system_evaluate` with early-exit when term degree sum exceeds limit. Update the forward declaration in `deepc_po_math.h`. Add `sphereToCs` inline function to `deepc_po_math.h`.
  - Verify: `grep -q 'max_degree' src/poly.h && grep -q 'sphereToCs' src/deepc_po_math.h && grep -q 'max_degree' src/deepc_po_math.h`
  - Done when: poly.h evaluator supports max_degree truncation; deepc_po_math.h has sphereToCs and updated forward declaration

- [x] **T02: Create DeepCDefocusPOThin.cpp and DeepCDefocusPORay.cpp scaffolds** `est:1h`
  - Why: The two new plugin source files, derived from the working DeepCDefocusPO.cpp template
  - Files: `src/DeepCDefocusPOThin.cpp`, `src/DeepCDefocusPORay.cpp`, `src/DeepCDefocusPO.cpp` (read-only template)
  - Do: Copy DeepCDefocusPO.cpp to create both scaffolds. Thin: rename class/CLASS/HELP, add `_max_degree` Int_knob (default 11, range 1–11), replace renderStripe body with zero-output stub. Ray: same as Thin plus `aperture_file` File_knob, 4 lens geometry Float_knobs in "Lens geometry" group (Angenieux 55mm defaults), second `poly_system_t _aperture_sys` with loading in _validate, stub renderStripe. Both preserve: holdout input, CA wavelengths, Halton/Shirley, knob_changed, _validate poly loading, _close cleanup.
  - Verify: `grep -q '_max_degree' src/DeepCDefocusPOThin.cpp && grep -q 'aperture_file' src/DeepCDefocusPORay.cpp && grep -q 'outer_pupil_curvature_radius' src/DeepCDefocusPORay.cpp`
  - Done when: Both .cpp files exist with all required knobs, stub renderStripe, and preserved holdout/CA/Halton infrastructure

- [x] **T03: Update CMake, verify script, and remove old DeepCDefocusPO.cpp** `est:30m`
  - Why: Wires the new plugins into the build system and updates the syntax-check gate
  - Files: `src/CMakeLists.txt`, `scripts/verify-s01-syntax.sh`, `src/DeepCDefocusPO.cpp` (delete)
  - Do: In CMakeLists.txt replace `DeepCDefocusPO` with `DeepCDefocusPOThin` and `DeepCDefocusPORay` in both PLUGINS and FILTER_NODES lists. In verify-s01-syntax.sh update the for-loop to check `DeepCDefocusPOThin.cpp DeepCDefocusPORay.cpp` (remove DeepCDefocusPO.cpp). Update the S05 CMake grep contracts for the new plugin names (4 occurrences total: 2 Thin + 2 Ray). Remove the old S02/S03/S04/S05 grep contracts that reference DeepCDefocusPO.cpp (they test the old single-plugin and will fail since the file is gone). Delete `src/DeepCDefocusPO.cpp`.
  - Verify: `bash scripts/verify-s01-syntax.sh`
  - Done when: verify-s01-syntax.sh exits 0; CMake has both new plugins and no old one; DeepCDefocusPO.cpp is deleted

## Files Likely Touched

- `src/poly.h`
- `src/deepc_po_math.h`
- `src/DeepCDefocusPOThin.cpp` (new)
- `src/DeepCDefocusPORay.cpp` (new)
- `src/CMakeLists.txt`
- `scripts/verify-s01-syntax.sh`
- `src/DeepCDefocusPO.cpp` (delete)

## Observability / Diagnostics

- **max_degree early-exit**: When `max_degree >= 0`, `poly_system_evaluate` breaks the term loop once cumulative degree exceeds the limit — callers can observe the effect by comparing output with `max_degree = -1` vs a truncated value.
- **sphereToCs fallback**: When the input point lies outside the sphere (disc < 0), `sphereToCs` returns `(x, y, 0)` — a degenerate but safe direction. No NaN or crash on out-of-range inputs.
- **Compile-time verification**: `scripts/verify-s01-syntax.sh` performs syntax-only compilation (`-fsyntax-only`) of both plugin files, catching missing includes, signature mismatches, and undefined symbols at build time.
- **Failure path**: If `poly_system_read` fails (returns non-zero), `_validate` in both plugin scaffolds should report the error via Nuke's `error()` mechanism (validated in T02).
- **Diagnostic grep checks**: All slice verification checks are grep-based, verifiable without a running Nuke instance.
