---
estimated_steps: 6
estimated_files: 3
skills_used: []
---

# T01: Extend FFI: add LensParams C struct + update header, lib.rs, and C++ call site

The current opendefocus_deep_render() signature takes only DeepSampleData*, float*, uint32_t, uint32_t. Lens parameters (focal_length, fstop, focus_distance, sensor_size_mm) must be passed from C++ to Rust for CoC computation.

1. Add `LensParams` #[repr(C)] struct to `crates/opendefocus-deep/src/lib.rs` with fields: focal_length_mm (f32), fstop (f32), focus_distance (f32), sensor_size_mm (f32).
2. Update `opendefocus_deep_render` extern C signature to accept `*const LensParams` as a 5th argument.
3. Update `include/opendefocus_deep.h`: add LensParams typedef + updated prototype.
4. Update `src/DeepCOpenDefocus.cpp` renderStripe call site to construct and pass a LensParams on the stack from the knob values.
5. Run scripts/verify-s01-syntax.sh to confirm C++ still compiles (local syntax check, no cargo needed).

## Inputs

- `crates/opendefocus-deep/src/lib.rs`
- `crates/opendefocus-deep/include/opendefocus_deep.h`
- `src/DeepCOpenDefocus.cpp`
- `.gsd/milestones/M009-mso5fb/M009-mso5fb-RESEARCH.md`

## Expected Output

- `Updated lib.rs with LensParams struct`
- `Updated opendefocus_deep.h with LensParams typedef`
- `Updated DeepCOpenDefocus.cpp passing LensParams to FFI`

## Verification

scripts/verify-s01-syntax.sh passes (C++ side). nm on existing .a still shows opendefocus_deep_render (note: .a from S01 is for the old signature — full verification happens in T04 docker build).
