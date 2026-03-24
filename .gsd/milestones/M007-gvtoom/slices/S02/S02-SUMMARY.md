---
id: S02
milestone: M007-gvtoom
title: "DeepCDefocusPOThin scatter engine"
status: complete
completed_at: 2026-03-24
tasks_completed: [T01, T02]
provides:
  - Full thin-lens CoC scatter engine in DeepCDefocusPOThin::renderStripe
  - test/test_thin.nk at 128×72 with Angenieux 55mm lens
  - Mock header fixes: Channel enum, chanNo(), writableAt(int,int,int), ChannelSet::contains()
key_files:
  - src/DeepCDefocusPOThin.cpp
  - test/test_thin.nk
  - scripts/verify-s01-syntax.sh
runtime_proof_status: deferred_to_docker_build
---

# S02: DeepCDefocusPOThin Scatter Engine — Summary

## What This Slice Delivered

Replaced the zero-output `renderStripe` stub (installed by S01) with a full thin-lens CoC scatter engine in `DeepCDefocusPOThin.cpp`, and created the `test/test_thin.nk` UAT script. The engine is structurally complete and passes all syntax and contract checks. Runtime proof (non-black EXR output) is deferred to docker build + `nuke -x`.

### Engine Architecture (T01)

The `renderStripe` body implements a 4-level nested scatter loop:

1. **Pixel loop** — iterates over the output stripe's pixel coordinates
2. **Deep sample loop** — fetches all deep samples at that pixel from the Deep input via `deepEngine`, iterates each
3. **Aperture sample loop** — for each deep sample, draws `_aperture_samples` points on the aperture disk using the Halton(2,3) low-discrepancy sequence + Shirley concentric mapping (`halton` + `map_to_disk`)
4. **CA channel loop** — evaluates the polynomial at three wavelengths (WL_B=0.45μm, WL_G=0.55μm, WL_R=0.65μm)

**Scatter radius:** `coc_radius(z, focus_dist, focal_length, fstop)` from `deepc_po_math.h` — thin-lens formula, depth-dependent, physically correct.

**Option B poly warp:** The polynomial (`poly_system_evaluate` with `_max_degree` truncation) is called with the normalised aperture sample position and wavelength. Its output channels `[0:1]` (sensor position) are interpreted as a warp offset. The warp vector's magnitude is clamped to `ap_radius` and then scaled by `coc_radius / ap_radius` to produce the final pixel-space displacement. This produces lens-specific aberration character (cat-eye, coma, CA) on top of the thin-lens CoC disk.

**Holdout:** `transmittance_at(x, y, z)` lambda fetches the holdout deep plane at the output pixel, walks samples with `zFront < z`, and computes cumulative transmittance. Applied per scatter contribution before accumulation.

**Write guards:** `chans.contains(chan)` before every channel write; `chanNo(chan)` used for all `writableAt(x, y, chanNo(chan))` calls. Both prevent heap corruption on channels not in the output plane.

**Poly load:** Called at `renderStripe` entry (not `_validate`) following the M006 thread-safety lesson. `_reload_poly` flag triggers re-read from disk.

### Test Script (T02)

`test/test_thin.nk` builds a 128×72 synthetic scene:
- Reformat → Grid(3.7, size 2) → Dilate → Merge(mask) checkerboard → DeepFromImage at z=5000
- DeepCDefocusPOThin with Angenieux 55mm exitpupil.fit, focal_length=55, focus_distance=10000, max_degree=11
- Write → `./test_thin.exr`

The test script is structurally identical to the reference `test_deepcdefocus_po.nk` except for the node class, added `max_degree 11` knob, and output path.

### Mock Header Fix

`Channel` was changed from `typedef int` to `enum` in `verify-s01-syntax.sh`'s mock `DDImage/Channel.h`. This was required to give `writableAt(int,int,Channel)` and `writableAt(int,int,int)` distinct signatures for overload resolution. The `foreach` macro was updated to use `static_cast<int>(VAR) != 0`. `chanNo(Channel)` and `contains(Channel)` were added to `ImagePlane` and `ChannelSet` mocks respectively.

## Verification Results

All 12 slice-level checks pass:

| Check | Result |
|-------|--------|
| `bash scripts/verify-s01-syntax.sh` exits 0 | ✅ pass |
| `grep -q 'coc_radius' src/DeepCDefocusPOThin.cpp` | ✅ pass |
| `grep -q 'halton' src/DeepCDefocusPOThin.cpp` | ✅ pass |
| `grep -q 'map_to_disk' src/DeepCDefocusPOThin.cpp` | ✅ pass |
| `grep -q 'chanNo' src/DeepCDefocusPOThin.cpp` | ✅ pass |
| `grep -q 'transmittance_at' src/DeepCDefocusPOThin.cpp` | ✅ pass |
| `grep -q '0.45f' src/DeepCDefocusPOThin.cpp` | ✅ pass |
| `grep -q '_max_degree' src/DeepCDefocusPOThin.cpp` | ✅ pass |
| `grep -q 'chans.contains' src/DeepCDefocusPOThin.cpp` | ✅ pass |
| `test -f test/test_thin.nk` | ✅ pass |
| `grep -q 'DeepCDefocusPOThin' test/test_thin.nk` | ✅ pass |
| `grep -q 'exitpupil.fit' test/test_thin.nk` | ✅ pass |
| `grep -q 'error.*Cannot open lens file' src/DeepCDefocusPOThin.cpp` | ✅ pass |

Runtime proof (`nuke -x test/test_thin.nk` producing non-black 128×72 EXR) deferred to docker build.

## Patterns Established

**Option B poly warp pattern:** `poly_system_evaluate` output `[0:1]` is treated as an aperture warp offset, NOT a direct screen position. Clamp vector magnitude to `ap_radius`, then scale by `coc/ap_radius`. This is the correct interpretation for thin-lens + poly aberration modulation — do not mistake poly output for absolute sensor coordinates.

**Channel enum mock pattern:** In `verify-s01-syntax.sh`, `Channel` must be an enum (not `typedef int`) for `writableAt` overloads to resolve correctly. Future scripts that add new `ImagePlane` methods taking `Channel` must verify this enum type is in place.

**Thread-safe poly load pattern:** Poly loading happens at `renderStripe` entry, gated by `_reload_poly` flag. `_validate` only caches path/knob values — it does NOT call `poly_system_read`. This is the M006-established pattern; both Thin and Ray follow it.

**CA wavelength class members:** `WL_B`, `WL_G`, `WL_R` are `static constexpr float` class members on `DeepCDefocusPOThin`. S03 (Ray variant) should use the same pattern — iterate `{WL_B, WL_G, WL_R}` in the gather loop.

**Green-channel position for alpha:** When computing alpha accumulation position in the scatter loop, use the G-channel (0.55μm) landing position. This is consistent with the M006 reference implementation.

## What Remains Before Milestone Completion

- **S03:** DeepCDefocusPORay gather engine + `test/test_ray.nk`
- **Docker build:** `docker-build.sh --linux --versions 16.0` must produce both `.so` files
- **Runtime UAT:** `nuke -x test/test_thin.nk` must exit 0 with non-black `test_thin.exr`; visual check that output is a defocused checkerboard (not a ring, not black)
- **Visual UAT:** max_degree variation test (degree 3 vs 11 must show visibly different aberration detail)

## Key Decisions Made in This Slice

- **D032 (confirmed):** Option B poly warp — thin-lens CoC gives scatter radius; polynomial modulates aperture sample positions within the CoC disk. The polynomial does NOT determine scatter direction. See DECISIONS.md.
- **Channel enum in mock headers:** Changed from `typedef int` to `enum`. This matches real DDImage and is the correct mock type going forward.

## Requirements Status

- **R030** (DeepCDefocusPOThin scatter model): Structurally implemented. Runtime proof pending docker build.
- **R032** (`_max_degree` knob): Confirmed present and wired to `poly_system_evaluate`. Runtime proof pending.
- **R035** (holdout, CA, Halton on both nodes): Thin variant fully implements all three. Ray variant scaffold from S01 carries the same knobs; engine wiring is S03's responsibility.

## Deferred Items

- Runtime render correctness (non-black, visually defocused output) — docker build + UAT
- Visual regression: defocus moves with depth (focus_distance variation test)
- Performance characterisation at 1280×720 (not a CI gate)
