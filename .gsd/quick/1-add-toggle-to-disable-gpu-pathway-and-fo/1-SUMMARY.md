# Quick Task: add toggle to disable gpu pathway and force cpu execution

**Date:** 2026-03-28
**Branch:** gsd/quick/1-add-toggle-to-disable-gpu-pathway-and-fo

## What Changed
- Added `use_gpu` Bool_knob to DeepCOpenDefocus C++ plugin (default: true) with tooltip explaining GPU vs CPU modes
- Extended `LensParams` FFI struct with `use_gpu` field (`u32` in Rust, `uint32_t` in C header) to carry the flag across the C++/Rust boundary
- Updated `get_renderer()` in `lib.rs` to accept a `use_gpu: bool` parameter, passing it to `OpenDefocusRenderer::new(use_gpu, ...)` — when false, forces CPU (rayon) execution
- Updated `verify-s01-syntax.sh` mock `Knob` class: changed from forward declaration to class with `STARTLINE` enum constant; added `SetFlags()` stub
- Updated `test/test_opendefocus.nk` with `use_gpu true` knob entry

## Files Modified
- `src/DeepCOpenDefocus.cpp` — Added `_use_gpu` member, Bool_knob, SetFlags(STARTLINE), LensParams population
- `crates/opendefocus-deep/src/lib.rs` — Added `use_gpu: u32` to LensParams, parameterized `get_renderer(use_gpu)`
- `crates/opendefocus-deep/include/opendefocus_deep.h` — Added `use_gpu` field to LensParams typedef
- `scripts/verify-s01-syntax.sh` — Updated Knob mock with STARTLINE enum and SetFlags stub
- `test/test_opendefocus.nk` — Added `use_gpu true` knob

## Verification
- `bash scripts/verify-s01-syntax.sh` → exit 0, all three DeepC*.cpp syntax checks pass
- `bash docker-build.sh --linux --versions 16.0` → exit 0, DeepCOpenDefocus.so built (21 MB), no errors
- `git diff` confirmed all five files changed correctly with matching struct layouts across C header, Rust, and C++
