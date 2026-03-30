# S02 Research: Full-image PlanarIop cache + quality knob

**Calibration:** Light research. The quality knob is fully wired from S01. The cache is a well-understood C++ pattern applied to one known file. No new Rust changes needed.

---

## Summary

S02 has only one real deliverable: add a **full-image render cache** to `DeepCOpenDefocus.cpp`. The quality knob — S01 delivered it completely: `Enumeration_knob`, `int _quality`, `(int)_quality` at both FFI call sites, `quality.into()` in `settings.render.quality` on the Rust side. Nothing quality-related is missing.

The cache eliminates redundant Rust FFI calls when Nuke calls `renderStripe` multiple times for different stripes of the same frame. Without it, Nuke can invoke `renderStripe` with a sub-region (e.g. 256×64 stripe of a 256×256 image), causing four separate full renders — or worse, incorrect partial renders because convolution is a global operation requiring full-image context.

---

## Implementation Landscape

### File: `src/DeepCOpenDefocus.cpp` — only file that changes

**Current renderStripe flow:**
1. Takes `output.bounds()` (may be a stripe, not full image)
2. Calls `deepIn->deepEngine(bounds, ...)` — fetches deep samples for the stripe only
3. Builds FFI structs from stripe samples
4. Calls `opendefocus_deep_render(...)` or holdout variant on stripe data
5. Copies result to output ImagePlane

**Problem:** Nuke's PlanarIop calls `renderStripe` once per stripe. If Nuke stripes the 256×256 image into 4 horizontal bands, we call the Rust renderer 4 times instead of 1. More importantly, each sub-region render is **incorrect** — defocus is non-local (a sample at pixel (x,y) with a large CoC blurs energy far outside the stripe bounds).

**Fix: full-image cache**

New private members on `DeepCOpenDefocus`:
```cpp
// Full-image render cache — populated on first renderStripe per frame,
// served without re-render on subsequent stripes.
std::vector<float> _cachedRGBA;   // width*height*4 floats, RGBA interleaved
bool               _cacheValid = false;
// Knob state snapshot used to detect stale cache on next frame/param change
float              _cacheFL  = 0.f;
float              _cacheFS  = 0.f;
float              _cacheFD  = 0.f;
float              _cacheSSZ = 0.f;
int                _cacheQuality = -1;
int                _cacheUseGpu  = -1;
mutable std::mutex _cacheMutex;
```

`_validate()` additions:
```cpp
// Invalidate cache whenever Nuke revalidates (input or knob changed)
_cacheValid = false;
```
`_validate()` is called by Nuke on every dependency change (knob edits, upstream changes, frame changes). This is the canonical Nuke invalidation hook — no `DDImage::Hash` needed.

**Updated renderStripe logic:**

```
1. Compute effective lens params (fl, fs, fd, ssz) — camera override if input 2 connected
2. Lock _cacheMutex
3. If _cacheValid && effective params match cache snapshot:
       Copy the stripe sub-region from _cachedRGBA to output; unlock; return
4. Unlock
5. Get full image box: const Box& fullBox = info_.box()  [populated by _validate()]
6. deepIn->deepEngine(fullBox, ...) — fetch ALL deep samples (not just stripe)
7. Build sampleData for full image
8. Render full image: opendefocus_deep_render(fullImage, ...) OR holdout variant
9. Lock _cacheMutex
10. _cachedRGBA = output_buf (full image)
11. Update cache snapshot (fl, fs, fd, ssz, quality, use_gpu); _cacheValid = true
12. Copy stripe sub-region from _cachedRGBA to output
13. Unlock; return
```

**Stripe copy from cache:**
The output `ImagePlane` has `bounds()` = stripe bounds. The full image is stored row-major in `_cachedRGBA[y * fullW * 4 + x * 4 + c]`. To copy stripe:
```cpp
const int fullW = fullBox.w();
for (int y = bounds.y(); y < bounds.t(); ++y) {
    for (int x = bounds.x(); x < bounds.r(); ++x) {
        const int srcBase = (y - fullBox.y()) * fullW * 4 + (x - fullBox.x()) * 4;
        // copy to output ImagePlane at (y,x) using colStride/rowStride/chanStride
    }
}
```
The existing interleaved→planar copy logic from renderStripe can be reused with cache-relative offsets.

**Thread safety:**
- `std::mutex _cacheMutex` (stdlib, no DDImage dependency) — works with the mock headers
- `std::mutex` is non-copyable: fine, `DeepCOpenDefocus` is always heap-allocated via `new`
- Lock is held only for cache reads (cheap) and cache writes (also cheap — it's a memcpy)
- The slow Rust render happens **outside** the lock (step 8). Two threads racing here will both compute the full render and the second will just overwrite the cache. Wasteful but correct and deadlock-free.
- A more sophisticated double-check lock isn't needed for M012 scope.

**Camera input handling:**
Camera override happens inside renderStripe (reading `CameraOp*` at input 2). This is correct: compute effective params first, then check cache against them. The cache key includes the effective (post-override) values, not just knob values.

**Holdout input:**
Holdout changes are handled via `_validate()` invalidation. Nuke calls `_validate()` when the holdout input tree changes, which sets `_cacheValid = false`. The cache key doesn't need to encode holdout data explicitly.

---

## New Test: 256×256 SLA Proof

A `test/test_opendefocus_256.nk` script (256×256 single-layer, CPU-only, quality=Low) is needed to prove the ≤10s SLA from milestone R067. This runs in Docker via `nuke -x -m 1 test/test_opendefocus_256.nk` with `time` wrapping.

The verify script already checks for 7 `.nk` files; `test_opendefocus_256.nk` would be an 8th that isn't currently in the verification loop. Either:
- Add it to `verify-s01-syntax.sh`'s file list (rename to `verify.sh` or add a comment)
- Or create a separate `scripts/verify-s02-timing.sh` for the Docker timing SLA

Simplest: add `test_opendefocus_256.nk` to the existing check loop in `verify-s01-syntax.sh`.

---

## Mock Header Impact (verify-s01-syntax.sh)

- `std::mutex` — comes from `<mutex>`, no mock header change needed
- `info_.box()` — `Info2D::box()` already exists in the mock as `Box& box()`
- No `DDImage::Hash` used — mock is sufficient as-is

**If `<mutex>` isn't already included** in DeepCOpenDefocus.cpp, add `#include <mutex>`. Check: the current file includes `<algorithm>`, `<utility>`, `<vector>`, `<cstring>`, `<cstdio>` — `<mutex>` is not there, must be added.

---

## Key Constraints

1. `info_.box()` only has valid dimensions after `_validate()` runs. `renderStripe` is always called after `_validate()`, so this is safe.
2. `deepIn->deepEngine(fullBox, ...)` — the deep input must have been validated for the full bbox. In `_validate()` we call `in0->validate(for_real)`, so the full deep chain is validated before renderStripe.
3. The singleton renderer (OnceLock) from S01 is unaffected — S02 doesn't touch `crates/opendefocus-deep/src/lib.rs` or any Rust.
4. `getRequests()` currently calls `in0->validate(true)` — this is sufficient; no changes needed there.

---

## Verification Commands

```bash
# Syntax check (must still pass after adding <mutex> and cache members)
bash scripts/verify-s01-syntax.sh

# Cache members present
grep -q '_cacheValid' src/DeepCOpenDefocus.cpp
grep -q '_cacheMutex' src/DeepCOpenDefocus.cpp
grep -q 'info_.box()' src/DeepCOpenDefocus.cpp || grep -q 'fullBox' src/DeepCOpenDefocus.cpp

# Quality knob still exactly 1 (unchanged from S01)
grep -c 'Enumeration_knob' src/DeepCOpenDefocus.cpp   # → 1

# 256x256 test script present
test -f test/test_opendefocus_256.nk

# Full validate: syntax + artifact checks
bash scripts/verify-s01-syntax.sh   # exits 0

# Docker-only: SLA proof
# time docker run ... nuke -x -m 1 test/test_opendefocus_256.nk
# Must complete in < 10 seconds wall-clock
```

---

## Task Decomposition for Planner

S02 is naturally a single task:

**T01 — Full-image cache in DeepCOpenDefocus.cpp + 256×256 test script**
- Add `#include <mutex>` to DeepCOpenDefocus.cpp
- Add cache members to the class
- Add `_cacheValid = false` in `_validate()`
- Rewrite renderStripe: always render full image, cache result, serve stripe from cache
- Create `test/test_opendefocus_256.nk` (256×256, CPU-only, quality=Low)
- Update `verify-s01-syntax.sh` to check for `test_opendefocus_256.nk`
- Verify: `bash scripts/verify-s01-syntax.sh` exits 0; all grep checks pass

No Rust changes. No FFI changes. No header changes.
