---
id: T01
parent: S02
milestone: M012-kho2ui
provides: []
requires: []
affects: []
key_files: ["src/DeepCOpenDefocus.cpp", "test/test_opendefocus_256.nk", "scripts/verify-s01-syntax.sh"]
key_decisions: ["Rust FFI render runs outside _cacheMutex to avoid holding the lock during a potentially long operation; concurrent misses each do a full render but the second write is idempotent.", "Stripe copy uses y/x loop with fullBox-relative pixel index to correctly handle both packed and planar ImagePlane layouts.", "_cacheValid invalidated unconditionally in _validate() — conservative but correct since Nuke always calls _validate before a new render."]
patterns_established: []
drill_down_paths: []
observability_surfaces: []
duration: ""
verification_result: "Ran: bash scripts/verify-s01-syntax.sh (exits 0; syntax checks DeepCBlur.cpp, DeepCDepthBlur.cpp, DeepCOpenDefocus.cpp; checks all 8 .nk test files exist). Plus grep checks for _cacheValid, _cacheMutex, info_.box(), Enumeration_knob count==1, and test -f test/test_opendefocus_256.nk. All pass."
completed_at: 2026-03-30T23:28:55.322Z
blocker_discovered: false
---

# T01: Added full-image PlanarIop render cache to DeepCOpenDefocus (mutex-guarded, cache-invalidated in _validate, holdout branch uses fullBox) and created 256×256 Nuke SLA test script

> Added full-image PlanarIop render cache to DeepCOpenDefocus (mutex-guarded, cache-invalidated in _validate, holdout branch uses fullBox) and created 256×256 Nuke SLA test script

## What Happened
---
id: T01
parent: S02
milestone: M012-kho2ui
key_files:
  - src/DeepCOpenDefocus.cpp
  - test/test_opendefocus_256.nk
  - scripts/verify-s01-syntax.sh
key_decisions:
  - Rust FFI render runs outside _cacheMutex to avoid holding the lock during a potentially long operation; concurrent misses each do a full render but the second write is idempotent.
  - Stripe copy uses y/x loop with fullBox-relative pixel index to correctly handle both packed and planar ImagePlane layouts.
  - _cacheValid invalidated unconditionally in _validate() — conservative but correct since Nuke always calls _validate before a new render.
duration: ""
verification_result: passed
completed_at: 2026-03-30T23:28:55.352Z
blocker_discovered: false
---

# T01: Added full-image PlanarIop render cache to DeepCOpenDefocus (mutex-guarded, cache-invalidated in _validate, holdout branch uses fullBox) and created 256×256 Nuke SLA test script

**Added full-image PlanarIop render cache to DeepCOpenDefocus (mutex-guarded, cache-invalidated in _validate, holdout branch uses fullBox) and created 256×256 Nuke SLA test script**

## What Happened

Rewrote renderStripe() to render the complete image (info_.box()) on cache miss, store the result in _cachedRGBA, and serve all subsequent stripe calls from the cache — eliminating redundant Rust FFI calls per frame. Added std::mutex, six cache-key fields, and _cacheValid member. Added _cacheValid = false to _validate() for unconditional cache invalidation on every knob/upstream/frame change. Both holdout and non-holdout FFI branches now operate on fullBox. Created test/test_opendefocus_256.nk (256×256, 1-frame, CPU quality=Low). Extended verify-s01-syntax.sh to check the new .nk file. All six task verification checks pass.

## Verification

Ran: bash scripts/verify-s01-syntax.sh (exits 0; syntax checks DeepCBlur.cpp, DeepCDepthBlur.cpp, DeepCOpenDefocus.cpp; checks all 8 .nk test files exist). Plus grep checks for _cacheValid, _cacheMutex, info_.box(), Enumeration_knob count==1, and test -f test/test_opendefocus_256.nk. All pass.

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `bash scripts/verify-s01-syntax.sh` | 0 | ✅ pass | 4000ms |
| 2 | `grep -q '_cacheValid' src/DeepCOpenDefocus.cpp` | 0 | ✅ pass | 50ms |
| 3 | `grep -q '_cacheMutex' src/DeepCOpenDefocus.cpp` | 0 | ✅ pass | 50ms |
| 4 | `grep -q 'info_.box()' src/DeepCOpenDefocus.cpp` | 0 | ✅ pass | 50ms |
| 5 | `test $(grep -c 'Enumeration_knob' src/DeepCOpenDefocus.cpp) -eq 1` | 0 | ✅ pass | 50ms |
| 6 | `test -f test/test_opendefocus_256.nk` | 0 | ✅ pass | 10ms |


## Deviations

None.

## Known Issues

None.

## Files Created/Modified

- `src/DeepCOpenDefocus.cpp`
- `test/test_opendefocus_256.nk`
- `scripts/verify-s01-syntax.sh`


## Deviations
None.

## Known Issues
None.
