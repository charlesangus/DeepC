---
estimated_steps: 1
estimated_files: 3
skills_used: []
---

# T01: Add use_gpu Bool_knob and thread through FFI (C++, header, Rust)

The test_opendefocus_cpu.nk script (T02) needs to write `use_gpu false` to a real knob. That knob does not yet exist. This task adds it as a Bool_knob in DeepCOpenDefocus.cpp, adds an `int use_gpu` parameter to the C FFI function signature in opendefocus_deep.h, and updates lib.rs to accept and honour that parameter by passing it to OpenDefocusRenderer::new(). These are small but cross-language changes; doing this first ensures T02 uses the correct knob name and the syntax check passes before any .nk files are authored.

## Inputs

- ``src/DeepCOpenDefocus.cpp` — existing C++ plugin; needs use_gpu member + knob + FFI call update`
- ``crates/opendefocus-deep/include/opendefocus_deep.h` — existing C header; needs int use_gpu param added to opendefocus_deep_render`
- ``crates/opendefocus-deep/src/lib.rs` — existing Rust FFI impl; needs use_gpu: i32 param, pass to renderer init`

## Expected Output

- ``src/DeepCOpenDefocus.cpp` — adds `bool _use_gpu = true` private member, `Bool_knob(f, &_use_gpu, "use_gpu", "Use GPU")` in knobs(), and passes `(int)_use_gpu` as 6th argument to opendefocus_deep_render()`
- ``crates/opendefocus-deep/include/opendefocus_deep.h` — adds `int use_gpu` as 6th parameter to opendefocus_deep_render declaration`
- ``crates/opendefocus-deep/src/lib.rs` — adds `use_gpu: i32` as 6th parameter to opendefocus_deep_render, changes get_renderer() call to OpenDefocusRenderer::new(use_gpu != 0, ...) inline`

## Verification

sh scripts/verify-s01-syntax.sh

## Observability Impact

FFI signature mismatch between header and lib.rs is caught at cargo build time (Docker). C++ syntax errors are caught by verify-s01-syntax.sh (g++ -fsyntax-only).
