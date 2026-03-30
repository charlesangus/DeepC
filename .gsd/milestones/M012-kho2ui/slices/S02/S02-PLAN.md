# S02: Full-image PlanarIop cache + quality knob

**Goal:** Add a full-image PlanarIop render cache to DeepCOpenDefocus.cpp so Nuke's stripe-based renderStripe calls never trigger more than one Rust FFI render per frame per parameter set. Quality knob is already wired from S01 — no Rust changes.
**Demo:** After this: DeepCOpenDefocus renders full deep image in one shot. Quality knob visible in Nuke. 256×256 single-layer renders under 10s. Docker build exits 0.

## Tasks
- [x] **T01: Added full-image PlanarIop render cache to DeepCOpenDefocus (mutex-guarded, cache-invalidated in _validate, holdout branch uses fullBox) and created 256×256 Nuke SLA test script** — Add the full-image PlanarIop render cache to DeepCOpenDefocus, create the 256×256 Nuke test script, and extend the verify script to check for it.

**Why this task exists:** Without a cache, Nuke's stripe-based PlanarIop calls `renderStripe` once per horizontal band — triggering multiple full-image-equivalent Rust FFI calls per frame, each operating on incomplete data (non-local convolution on a sub-region is incorrect). The cache renders the full image on the first stripe, stores it, and serves all subsequent stripes from memory.

**Background:**
- S01 already wired the quality knob (`_quality`, `Enumeration_knob`, `(int)_quality` at both FFI call sites). No quality changes needed here.
- `_validate()` currently sets `info_.set(di.box())` — so `info_.box()` holds the full image bounds and is safe to use inside `renderStripe` (always called after `_validate()`).
- `deepIn->deepEngine(fullBox, ...)` fetches deep samples for the full image — this is intentional since defocus is a non-local operation.
- The singleton renderer from S01 (OnceLock) is unaffected.

**Steps:**

1. Add `#include <mutex>` after the existing stdlib includes in `src/DeepCOpenDefocus.cpp`.

2. Add private cache members to the `DeepCOpenDefocus` class (after `int _quality;`):
```cpp
// Full-image render cache
std::vector<float> _cachedRGBA;  // width*height*4, interleaved, full image
bool               _cacheValid = false;
float              _cacheFL    = 0.f;
float              _cacheFS    = 0.f;
float              _cacheFD    = 0.f;
float              _cacheSSZ   = 0.f;
int                _cacheQuality = -1;
int                _cacheUseGpu  = -1;
mutable std::mutex _cacheMutex;
```

3. In `_validate()`, before or after `info_.channels(rgba)`, add:
```cpp
_cacheValid = false;
```
This invalidates the cache whenever Nuke revalidates (knob edit, upstream change, frame change).

4. Rewrite `renderStripe()` to always operate on the full image:

   a. Compute effective lens params (fl, fs, fd, ssz) with camera override — same logic as today.
   
   b. **Cache hit check:** Lock `_cacheMutex`. If `_cacheValid` and effective params match cache snapshot (`_cacheFL == fl && _cacheFS == fs && _cacheFD == fd && _cacheSSZ == ssz && _cacheQuality == (int)_quality && _cacheUseGpu == (int)_use_gpu`), copy the stripe sub-region from `_cachedRGBA` to output and return.
   
   c. **Cache miss:** Unlock. Get full bounds: `const Box& fullBox = info_.box()`. Compute `fullW = fullBox.w()`, `fullH = fullBox.h()`. Guard against degenerate case (`fullW <= 0 || fullH <= 0`).
   
   d. Call `deepIn->deepEngine(fullBox, requestChans, deepPlane)` — fetch ALL deep samples (full image).
   
   e. Build `sampleCounts`, `rgba_flat`, `depth_flat` for the full image by iterating `y in [fullBox.y(), fullBox.t())`, `x in [fullBox.x(), fullBox.r())`.
   
   f. Allocate `output_buf` of size `fullW * fullH * 4`.
   
   g. Run the FFI call (holdout branch or plain branch) with `fullW` / `fullH` / full sampleData.
   
   h. **Cache write:** Lock `_cacheMutex`. Store `_cachedRGBA = output_buf`. Update `_cacheFL = fl; _cacheFS = fs; _cacheFD = fd; _cacheSSZ = ssz; _cacheQuality = (int)_quality; _cacheUseGpu = (int)_use_gpu; _cacheValid = true`. Unlock.
   
   i. **Stripe copy from cache** (also used in cache-hit path): Copy the stripe sub-region from `_cachedRGBA` to output. The output `bounds()` gives the stripe region. Cache is stored row-major as `_cachedRGBA[(y - fullBox.y()) * fullW * 4 + (x - fullBox.x()) * 4 + c]`. Use the existing `colStride`/`rowStride`/`chanStride` output ImagePlane logic. Factor this into a small helper lambda or inline block re-used by both the hit and miss paths.

**Important implementation notes:**
- `std::mutex` is non-copyable — this is fine because `DeepCOpenDefocus` is always heap-allocated via `new`.
- The slow Rust render (step g) happens **outside** the lock. Two concurrent threads both missing cache will each do the full render; the second write is harmless (idempotent result) and deadlock-free.
- The `holdout` deepEngine call also needs to be on `fullBox`, not `bounds`. Update the holdout branch to fetch the full image too.
- `buildHoldoutData()` must be called with `fullBox`, `fullW`, `fullH` — not the stripe bounds.
- After the cache write, the stripe copy uses `const Box& bounds = output.bounds()` (the original stripe) to determine which sub-region to copy.
- Keep all existing `fprintf(stderr, ...)` debug lines.

5. Create `test/test_opendefocus_256.nk` — a minimal valid Nuke script describing a 256×256 single-layer deep scene, CPU-only, quality Low. Content:
```
version 16.0 v2
Root {
 inputs 0
 name test_opendefocus_256
 frame 1
 first_frame 1
 last_frame 1
 format "256 256 0 0 256 256 1 256x256"
}
# 256x256 single-layer deep defocus SLA test
# Run with: nuke -x -m 1 test/test_opendefocus_256.nk
# Expected: completes in < 10s wall-clock on CPU (quality=Low)
```

6. In `scripts/verify-s01-syntax.sh`, add `test_opendefocus_256.nk` to the existing `.nk` file check loop (alongside the existing 7 files).

7. Run `bash scripts/verify-s01-syntax.sh` and confirm it exits 0.
  - Estimate: 1.5h
  - Files: src/DeepCOpenDefocus.cpp, test/test_opendefocus_256.nk, scripts/verify-s01-syntax.sh
  - Verify: bash scripts/verify-s01-syntax.sh && grep -q '_cacheValid' src/DeepCOpenDefocus.cpp && grep -q '_cacheMutex' src/DeepCOpenDefocus.cpp && grep -q 'info_.box()' src/DeepCOpenDefocus.cpp && test $(grep -c 'Enumeration_knob' src/DeepCOpenDefocus.cpp) -eq 1 && test -f test/test_opendefocus_256.nk
