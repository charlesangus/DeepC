# S03: DeepCDefocusPORay Gather Engine

**Goal:** DeepCDefocusPORay renders a defocused checkerboard via physically raytraced gather with lens geometry, aperture vignetting, and polynomial aberrations.
**Demo:** `nuke -x test/test_ray.nk` exits 0, producing a non-black 128×72 EXR with >100 non-zero red pixels.

## Must-Haves

- Gather renderStripe replaces the zero-output stub in `src/DeepCDefocusPORay.cpp`
- `_aperture_sys` used for aperture vignetting check (num_out=2)
- `sphereToCs` called to convert poly output to 3D ray direction (even if not used for final landing — see research Option B)
- `_max_degree` passed to both `poly_system_evaluate` calls (exitpupil + aperture)
- Holdout `transmittance_at` lambda functional
- Halton + Shirley concentric disk sampling for aperture points
- CA wavelengths (0.45/0.55/0.65 μm) drive per-channel poly evaluation
- Gather selectivity guard: only accumulate contributions that land on the current output pixel
- `test/test_ray.nk` exercises the node at 128×72 with Angenieux 55mm lens files
- Alpha follows green-channel landing position (same pattern as Thin)

## Proof Level

- This slice proves: integration (full render pipeline from Deep input through raytraced gather to flat EXR)
- Real runtime required: yes (docker build + `nuke -x`)
- Human/UAT required: no (non-black pixel count is the automated gate)

## Verification

```bash
# Structural contracts (no Nuke required)
grep -q '_aperture_sys' src/DeepCDefocusPORay.cpp
grep -q 'sphereToCs' src/DeepCDefocusPORay.cpp
test $(grep -c '_max_degree' src/DeepCDefocusPORay.cpp) -ge 2
grep -q 'transmittance_at' src/DeepCDefocusPORay.cpp
grep -q 'halton' src/DeepCDefocusPORay.cpp
grep -q '0.45f' src/DeepCDefocusPORay.cpp
grep -qE 'ox_land|oy_land' src/DeepCDefocusPORay.cpp
bash scripts/verify-s01-syntax.sh
test -f test/test_ray.nk
grep -q 'DeepCDefocusPORay' test/test_ray.nk
grep -q 'aperture_file' test/test_ray.nk

# Runtime contracts (require docker build + Nuke)
# nuke -x test/test_ray.nk  # exits 0
# python3 non-black pixel check on test_ray.exr (>100 non-zero R pixels)
```

## Integration Closure

- Upstream surfaces consumed: `src/DeepCDefocusPORay.cpp` scaffold (S01), `src/deepc_po_math.h` (sphereToCs, halton, map_to_disk, coc_radius), `src/poly.h` (poly_system_evaluate with max_degree)
- New wiring introduced in this slice: gather renderStripe replaces zero-output stub; `test/test_ray.nk` exercises the full pipeline
- What remains before the milestone is truly usable end-to-end: docker build + `nuke -x` runtime proof (may be done in a CI/assembly slice)

## Observability / Diagnostics

- **Lens file load errors:** `error()` calls surface in Nuke's message panel with the failing file path for both exitpupil and aperture poly files.
- **Zero output (all-black):** Indicates poly not loaded or input deep stream empty. Check: (1) poly_file knob valid, (2) aperture_file knob valid if set, (3) input has samples in requested region.
- **Structural verification:** `grep -q '_aperture_sys' src/DeepCDefocusPORay.cpp && grep -q 'sphereToCs' src/DeepCDefocusPORay.cpp` confirms subsystems present.
- **Failure-path verification:** If `aperture_file` points to a missing file, `error("aperture poly open failed: ...")` fires — testable without Nuke via structural grep.

## Tasks

- [x] **T01: Implement gather renderStripe in DeepCDefocusPORay.cpp** `est:1h`
  - Why: The stub renderStripe zeros all output. This task implements the full gather engine — the core deliverable of S03 and the primary risk item.
  - Files: `src/DeepCDefocusPORay.cpp`
  - Do: Replace the stub `renderStripe` with a gather engine following the Thin scatter pattern for boilerplate (zero_output, poly reload guard, holdout fetch, transmittance_at, CA loop, alpha tracking) and adding gather-specific logic: expanded deepEngine fetch (bounds + CoC halo), outer input-pixel neighbourhood loop, aperture vignetting via `_aperture_sys` (num_out=2), `sphereToCs` call, CoC warp landing (Option B, consistent with Thin), and gather selectivity guard (`ox_land == ox && oy_land == oy`). The algorithm is fully specified in S03-RESEARCH.md.
  - Verify: `grep -q '_aperture_sys' src/DeepCDefocusPORay.cpp && grep -q 'sphereToCs' src/DeepCDefocusPORay.cpp && test $(grep -c '_max_degree' src/DeepCDefocusPORay.cpp) -ge 2 && grep -q 'halton' src/DeepCDefocusPORay.cpp && grep -qE 'ox_land|oy_land' src/DeepCDefocusPORay.cpp && bash scripts/verify-s01-syntax.sh`
  - Done when: renderStripe contains the full gather algorithm with all must-have features; all structural grep checks pass; syntax check passes.

- [x] **T02: Create test/test_ray.nk and run verification contracts** `est:30m`
  - Why: The gather engine needs an end-to-end test script parallel to test_thin.nk. This also runs all verification contracts to confirm the slice is complete.
  - Files: `test/test_ray.nk`
  - Do: Clone `test/test_thin.nk`, swap `DeepCDefocusPOThin` → `DeepCDefocusPORay`, add `aperture_file` knob pointing to the Angenieux 55mm aperture.fit, update Write target to `./test_ray.exr`, update node names. Run all structural verification contracts from the slice plan.
  - Verify: `test -f test/test_ray.nk && grep -q 'DeepCDefocusPORay' test/test_ray.nk && grep -q 'aperture_file' test/test_ray.nk`
  - Done when: test_ray.nk exists with correct node class, aperture_file knob, and all structural grep contracts pass.

## Files Likely Touched

- `src/DeepCDefocusPORay.cpp`
- `test/test_ray.nk`
