# Quick Task: DeepCDefocusPO segfault — free(): invalid pointer / malloc assertions

**Date:** 2026-03-24
**Branch:** gsd/quick/6-we-re-still-getting-a-segfault-wen-using
**Commit:** 820dd2b

## Root Cause

`ImagePlane::writableAt(int x, int y, int z)` takes `z` as a **channel index** (0-based position in the plane's channel list), not a `Channel` enum value.

Our code was passing `Channel` enum values directly:
- `Chan_Red=1, Chan_Green=2, Chan_Blue=3, Chan_Alpha=4`

For a 4-component RGBA plane, valid indices are `0–3`. Passing `Chan_Alpha=4` writes one element **past the buffer end** on every call, silently corrupting the heap. The downstream symptoms — `malloc(): invalid size (unsorted)`, `free(): invalid pointer`, `_int_malloc` assertion, and SIGSEGV — are all heap corruption artifacts from this single off-by-one. The crash location shifts between runs because the corruption's effect depends on what the allocator checks next.

This affected three call sites: the zero-clearing loop (via `foreach(z, chans)` which iterates Channel enum values), the RGB scatter splat, and the alpha scatter splat.

## Secondary Fix: poly_system_read data race

`_validate` can be called by Nuke on the main thread while `renderStripe` is executing on a worker thread. `_validate` was calling `poly_system_destroy` + `poly_system_read` which frees and reallocates `_poly_sys.poly[k].term` pointers simultaneously being read in `renderStripe` — a use-after-free that also manifested as `free(): invalid pointer` and heap assertions.

Fix: moved poly loading entirely into `renderStripe`. PlanarIop's sequential single-stripe guarantee makes loading at `renderStripe` entry thread-safe without additional locking.

## Tertiary Fix: early return without zero

`deepEngine` failure path returned without zeroing the `ImagePlane`, leaving Nuke with uninitialized pixel data. Fixed with a consolidated `zero_output` lambda used by all early-exit paths.

## What Changed

- `writableAt(x, y, channel)` → `writableAt(x, y, imagePlane.chanNo(channel))` at all three call sites
- Poly loading moved from `_validate` to `renderStripe` entry (thread-safety)
- Early `deepEngine` failure now zeros output before returning
- `verify-s01-syntax.sh` mock ImagePlane: `writableAt(int,int,Channel)` → `writableAt(int,int,int)` + `chanNo(Channel)` stub (the old mock accepted the wrong type, masking this entire bug class)

## Files Modified

- `src/DeepCDefocusPO.cpp`
- `scripts/verify-s01-syntax.sh`

## Verification

```
nuke -x test/test_deepcdefocus_po.nk
# Writing test/test.exr took 0.21 seconds
# Frame 1 (1 of 1)
# Total render time: 0.21 seconds
# EXIT: 0
```

Valid 33KB EXR written. No crash, no sanitizer output.
