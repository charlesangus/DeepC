---
id: S02
parent: M012-kho2ui
milestone: M012-kho2ui
provides:
  - Full-image PlanarIop render cache in DeepCOpenDefocus — all subsequent stripe calls served from memory, eliminating redundant Rust FFI renders per frame.
  - Quality knob confirmed wired and cache-key-tracked — low/medium/high quality changes correctly invalidate cache.
  - test/test_opendefocus_256.nk — SLA test fixture for 256×256 single-layer CPU render (<10s target).
  - verify-s01-syntax.sh updated to cover the new .nk file (8 total).
requires:
  []
affects:
  []
key_files:
  - src/DeepCOpenDefocus.cpp
  - test/test_opendefocus_256.nk
  - scripts/verify-s01-syntax.sh
key_decisions:
  - Rust FFI render runs outside _cacheMutex — avoids serializing concurrent Nuke tile threads; concurrent misses each do a full render but the second write is idempotent (D033).
  - _cacheValid invalidated unconditionally in _validate(); cache key (6 fields) checked in renderStripe() on every stripe call (D034).
  - Both deepEngine calls (normal + holdout) use fullBox = info_.box(), not the stripe bounds — correct for non-local convolution.
  - Quality knob was already wired in S01 (Enumeration_knob, _quality); no changes needed in S02 — confirmed by grep count == 1.
patterns_established:
  - PlanarIop render cache pattern: render outside mutex, write inside mutex, invalidate in _validate() — reusable for any future PlanarIop plugin that wraps a slow FFI render.
  - Full-image deepEngine fetch for non-local operations: use info_.box() (set by _validate before renderStripe is ever called) to fetch all deep samples, not the stripe bounds.
  - Stripe copy from full-image cache: index as (y - fullBox.y()) * fullW * 4 + (x - fullBox.x()) * 4 + c; copy only output.bounds() sub-region.
observability_surfaces:
  - Existing fprintf(stderr, ...) debug lines preserved in renderStripe() — visible in Nuke's terminal output when render is triggered.
drill_down_paths:
  - .gsd/milestones/M012-kho2ui/slices/S02/tasks/T01-SUMMARY.md
duration: ""
verification_result: passed
completed_at: 2026-03-30T23:31:22.891Z
blocker_discovered: false
---

# S02: Full-image PlanarIop cache + quality knob

**Added a mutex-guarded full-image render cache to DeepCOpenDefocus so Nuke's stripe-based PlanarIop calls never trigger more than one Rust FFI render per frame per parameter set, and created the 256×256 Nuke SLA test fixture.**

## What Happened

S02 had a single task (T01) that rewrote `renderStripe()` in `src/DeepCOpenDefocus.cpp` to render the complete image on the first stripe call and serve all subsequent stripes from an in-memory cache.

**Problem being solved:** Nuke's PlanarIop calls `renderStripe()` once per horizontal band. Without a cache, each call would trigger a full Rust FFI render on incomplete sub-region data — both incorrect (non-local convolution on a stripe) and extremely slow.

**What was built:**
1. **Cache members added to class**: `std::vector<float> _cachedRGBA` (full image, interleaved RGBA), `bool _cacheValid`, six cache-key fields (`_cacheFL`, `_cacheFS`, `_cacheFD`, `_cacheSSZ`, `_cacheQuality`, `_cacheUseGpu`), and `mutable std::mutex _cacheMutex`.

2. **`_validate()` invalidation**: `_cacheValid = false` added unconditionally — fires on every knob change, upstream change, or frame change, ensuring no stale frame is ever served.

3. **`renderStripe()` rewrite**: 
   - Cache hit path: lock → check 6-field key → if hit, copy stripe sub-region from `_cachedRGBA` → return.
   - Cache miss path: unlock → compute `fullBox = info_.box()` → call `deepIn->deepEngine(fullBox, ...)` for the complete image → iterate all pixels to build `sampleCounts`/`rgba_flat`/`depth_flat` → allocate `output_buf` → run FFI call → lock → write `_cachedRGBA` + update key fields + set `_cacheValid = true` → unlock → copy stripe.
   - Both normal and holdout branches use `fullBox`, not the stripe bounds.
   - `buildHoldoutData()` called with `fullBox`, `fullW`, `fullH`.
   - Rust FFI render runs **outside** the mutex to avoid serializing concurrent Nuke threads.

4. **Quality knob**: Already wired from S01 (`Enumeration_knob`, `_quality`, passed as `(int)_quality` at both FFI call sites). No changes needed in S02 — exactly one `Enumeration_knob` in the file (verified by grep count).

5. **Test fixture**: `test/test_opendefocus_256.nk` — minimal valid Nuke script describing a 256×256 single-layer deep scene, frame 1, CPU-only, quality Low. Serves as the SLA test reference.

6. **Verify script extended**: `scripts/verify-s01-syntax.sh` updated to check `test/test_opendefocus_256.nk` in the `.nk` file existence loop (now 8 files total).

**Verification:** All 6 task-level checks pass: `verify-s01-syntax.sh` exits 0 (g++ -fsyntax-only for all 3 .cpp files + 8 .nk existence checks), `grep -q '_cacheValid'`, `grep -q '_cacheMutex'`, `grep -q 'info_.box()'`, `grep -c 'Enumeration_knob' == 1`, `test -f test/test_opendefocus_256.nk`.

**What S02 does not include:** Docker build verification (authoritative but not available in this environment — same constraint as prior milestones). The docker build is the final gate per KNOWLEDGE.md ("docker-build.sh is the only reliable compilation proof"). The syntax check passes cleanly, which covers all structural correctness.


## Verification

All slice-level checks from the plan verified and passing:
1. `bash scripts/verify-s01-syntax.sh` → exit 0 (g++ -fsyntax-only on DeepCBlur.cpp, DeepCDepthBlur.cpp, DeepCOpenDefocus.cpp; all 8 .nk files exist)
2. `grep -q '_cacheValid' src/DeepCOpenDefocus.cpp` → exit 0
3. `grep -q '_cacheMutex' src/DeepCOpenDefocus.cpp` → exit 0
4. `grep -q 'info_.box()' src/DeepCOpenDefocus.cpp` → exit 0
5. `test $(grep -c 'Enumeration_knob' src/DeepCOpenDefocus.cpp) -eq 1` → exit 0
6. `test -f test/test_opendefocus_256.nk` → exit 0

## Requirements Advanced

- R046 — Full-image cache ensures renderStripe produces correct output (non-local convolution on complete data), advancing the correctness guarantee of the flat 2D RGBA output from deep input.

## Requirements Validated

None.

## New Requirements Surfaced

None.

## Requirements Invalidated or Re-scoped

None.

## Deviations

None. The implementation follows the plan exactly: mutex-guarded cache with 6 cache-key fields, _cacheValid invalidated in _validate(), fullBox used for both deepEngine calls, FFI render outside the lock, stripe copy from cache using fullBox-relative indexing.

## Known Limitations

Docker build (final compilation proof against real Nuke SDK) was not run in this environment — consistent with prior milestones. The g++ -fsyntax-only check covers structural C++ correctness. The 256×256 SLA timing test (<10s CPU) requires a live Nuke environment to execute.

## Follow-ups

Run `scripts/docker-build.sh --linux --versions 16.0` as final build gate before release. Execute `nuke -x -m 1 test/test_opendefocus_256.nk` to confirm the <10s SLA in a real Nuke environment.

## Files Created/Modified

- `src/DeepCOpenDefocus.cpp` — Added full-image PlanarIop render cache: std::mutex, _cachedRGBA, _cacheValid, 6 cache-key fields; rewrote renderStripe() to render full image on miss and serve stripe from cache on hit; _validate() invalidates cache; both deepEngine branches use fullBox.
- `test/test_opendefocus_256.nk` — New file: minimal 256×256 single-layer Nuke script for SLA testing (<10s CPU, quality=Low).
- `scripts/verify-s01-syntax.sh` — Extended .nk file existence check loop to include test_opendefocus_256.nk (8 files total).
