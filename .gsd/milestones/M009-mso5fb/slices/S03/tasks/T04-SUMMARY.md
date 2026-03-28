---
id: T04
parent: S03
milestone: M009-mso5fb
provides: []
requires: []
affects: []
key_files: ["src/DeepCOpenDefocus.cpp", "scripts/verify-s01-syntax.sh", ".gsd/KNOWLEDGE.md"]
key_decisions: ["Fixed CameraOp focus-distance accessor: projection_distance() does not exist in DDImage SDK, correct method is focal_point()", "All legacy CameraOp accessors return double — added static_cast&lt;float&gt;() on focal_length, fStop, focal_point, film_width assignments", "Updated CameraOp mock stub in verify-s01-syntax.sh to use double return types matching real SDK"]
patterns_established: []
drill_down_paths: []
observability_surfaces: []
duration: ""
verification_result: "bash scripts/verify-s01-syntax.sh exits 0. Docker build exits 0 ([100%] Built target DeepCOpenDefocus). All 9 DoD items checked: .so built (21MB), GPU path (5295 wgpu/Vulkan symbols), holdout (21 source refs + .so string), camera link (7 source refs + .so string), test_opendefocus.nk present, icon 64x64 RGBA PNG, THIRD_PARTY_LICENSES.md EUPL-1.2, non-uniform bokeh (22 symbol hits in .so)."
completed_at: 2026-03-28T11:41:26.561Z
blocker_discovered: false
---

# T04: Fixed CameraOp projection_distance()→focal_point() compile error, added static_cast&lt;float&gt; for double returns, rebuilt DeepCOpenDefocus.so via Docker exit 0, and confirmed all 9 milestone DoD items pass

> Fixed CameraOp projection_distance()→focal_point() compile error, added static_cast&lt;float&gt; for double returns, rebuilt DeepCOpenDefocus.so via Docker exit 0, and confirmed all 9 milestone DoD items pass

## What Happened
---
id: T04
parent: S03
milestone: M009-mso5fb
key_files:
  - src/DeepCOpenDefocus.cpp
  - scripts/verify-s01-syntax.sh
  - .gsd/KNOWLEDGE.md
key_decisions:
  - Fixed CameraOp focus-distance accessor: projection_distance() does not exist in DDImage SDK, correct method is focal_point()
  - All legacy CameraOp accessors return double — added static_cast&lt;float&gt;() on focal_length, fStop, focal_point, film_width assignments
  - Updated CameraOp mock stub in verify-s01-syntax.sh to use double return types matching real SDK
duration: ""
verification_result: passed
completed_at: 2026-03-28T11:41:26.562Z
blocker_discovered: false
---

# T04: Fixed CameraOp projection_distance()→focal_point() compile error, added static_cast&lt;float&gt; for double returns, rebuilt DeepCOpenDefocus.so via Docker exit 0, and confirmed all 9 milestone DoD items pass

**Fixed CameraOp projection_distance()→focal_point() compile error, added static_cast&lt;float&gt; for double returns, rebuilt DeepCOpenDefocus.so via Docker exit 0, and confirmed all 9 milestone DoD items pass**

## What Happened

The verification gate exposed a malformed shell command string. On retry, a fresh Docker build revealed the real issue: cam->projection_distance() does not exist in the DDImage SDK — the correct deprecated method is focal_point(). Additionally, all legacy CameraOp accessors return double not float, requiring static_cast&lt;float&gt; on all four assignments. The mock CameraOp stub in verify-s01-syntax.sh was updated to match real SDK signatures (double return types, focal_point() replacing projection_distance()). After fixes, scripts/verify-s01-syntax.sh passes and the Docker build exits 0 producing a 21MB DeepCOpenDefocus.so with holdout, camera, and non-uniform bokeh symbols. All 9 DoD items confirmed.

## Verification

bash scripts/verify-s01-syntax.sh exits 0. Docker build exits 0 ([100%] Built target DeepCOpenDefocus). All 9 DoD items checked: .so built (21MB), GPU path (5295 wgpu/Vulkan symbols), holdout (21 source refs + .so string), camera link (7 source refs + .so string), test_opendefocus.nk present, icon 64x64 RGBA PNG, THIRD_PARTY_LICENSES.md EUPL-1.2, non-uniform bokeh (22 symbol hits in .so).

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `bash scripts/verify-s01-syntax.sh` | 0 | ✅ pass | 2000ms |
| 2 | `docker run nukedockerbuild:16.0-linux ... cmake --build build/16.0-linux --target DeepCOpenDefocus` | 0 | ✅ pass | 17700ms |
| 3 | `ls build/16.0-linux/src/DeepCOpenDefocus.so` | 0 | ✅ pass | 100ms |
| 4 | `file icons/DeepCOpenDefocus.png` | 0 | ✅ pass | 100ms |
| 5 | `ls test/test_opendefocus.nk` | 0 | ✅ pass | 100ms |
| 6 | `grep -A3 '## opendefocus' THIRD_PARTY_LICENSES.md` | 0 | ✅ pass | 100ms |
| 7 | `strings build/16.0-linux/src/DeepCOpenDefocus.so | grep -c 'non_uniform|AxialAberration'` | 0 | ✅ pass | 500ms |


## Deviations

projection_distance() was wrong — DDImage uses focal_point() for focus distance. All four CameraOp legacy accessors return double not float; added static_cast&lt;float&gt;() wrappers. Mock CameraOp stub in verify-s01-syntax.sh updated to match.

## Known Issues

Nuke 16.1 and 17.0 builds not run. nuke -x test requires licensed Nuke installation — .nk and .so are ready but EXR output check is manual.

## Files Created/Modified

- `src/DeepCOpenDefocus.cpp`
- `scripts/verify-s01-syntax.sh`
- `.gsd/KNOWLEDGE.md`


## Deviations
projection_distance() was wrong — DDImage uses focal_point() for focus distance. All four CameraOp legacy accessors return double not float; added static_cast&lt;float&gt;() wrappers. Mock CameraOp stub in verify-s01-syntax.sh updated to match.

## Known Issues
Nuke 16.1 and 17.0 builds not run. nuke -x test requires licensed Nuke installation — .nk and .so are ready but EXR output check is manual.
