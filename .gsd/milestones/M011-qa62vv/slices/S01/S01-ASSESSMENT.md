---
sliceId: S01
uatType: artifact-driven
verdict: PASS
date: 2026-03-29T11:35:00.000Z
---

# UAT Result — S01

## Checks

| Check | Mode | Result | Notes |
|-------|------|--------|-------|
| TC-01: Workspace compiles cleanly | runtime | PASS | `cargo check -p opendefocus-deep` exits 0; zero lines matching `^error`. Output: `Finished dev profile [unoptimized + debuginfo] target(s) in 0.65s` |
| TC-02: Kernel unit tests — all 19 pass | runtime | PASS | `cargo test -p opendefocus-kernel --lib` → `test result: ok. 19 passed; 0 failed; 0 ignored`. All 7 holdout-specific tests in `stages::ring::tests` reported `ok` (test_holdout_kernel_and_weight_unchanged, test_holdout_empty_slice_no_attenuation, test_holdout_no_attenuation_below_d0, test_holdout_out_of_bounds_pixel_no_attenuation, test_holdout_multi_pixel_correct_lookup, etc.) |
| TC-03: Depth-correct holdout unit test — green > red and green > blue | runtime | PASS | `cargo test -p opendefocus-deep -- holdout` → `test tests::test_holdout_attenuates_background_samples ... ok`, `test result: ok. 1 passed; 0 failed` |
| TC-04: SPIR-V entry point unchanged | artifact | PASS | `#[spirv(compute(threads(16, 16)))]` at lib.rs:138; function `convolve_kernel_f32` has NO `holdout` or `output_width` parameters. The holdout param appears only in the `#[cfg(not(any(target_arch = "spirv")))]` block at line 45 of global_entrypoint. SPIR-V function is under `#[cfg(target_arch = "spirv")]` guard with 10 params, identical to upstream 0.1.10. |
| TC-05: C FFI header declares HoldoutData and opendefocus_deep_render_holdout | artifact | PASS | `HoldoutData` appears 4 times (≥2 required); `opendefocus_deep_render_holdout` appears 1 time; `const float* data` found in HoldoutData struct body |
| TC-06: Empty holdout slice is a no-op (backward compatibility) | artifact | PASS | `opendefocus_deep_render` body calls `render_impl(data, output_rgba, width, height, lens, use_gpu, &[])` (confirmed at src/lib.rs:111). `holdout_transmittance` returns `1.0` immediately when `holdout.is_empty()` — confirmed by code read. |
| TC-07: Lockfile is version 3 | artifact | PASS | `head -4 Cargo.lock \| grep 'version = 3'` → matched `version = 3` |

## Overall Verdict

PASS — All 7 test cases passed; workspace compiles cleanly, all 19 kernel unit tests pass, depth-correct holdout attenuation unit test passes, SPIR-V entry point is unmodified, C FFI header is correct, backward-compat empty holdout is a no-op, and lockfile is v3.

## Notes

- protoc was available at `/tmp/protoc_dir/bin/protoc` as expected; no re-fetch required.
- `convolve_kernel_f32` (SPIR-V entry point) is guarded by `#[cfg(target_arch = "spirv")]`, ensuring the holdout parameter is unreachable from SPIR-V compilation. Holdout params are only in the CPU path under `#[cfg(not(any(target_arch = "spirv")))]`.
- `apply_holdout_attenuation` in `ring.rs` is now dead code when called via opendefocus-deep (always receives `&[]`), but remains valid for non-deep paths.
- TC-02 test list confirmed: the 7 holdout-specific tests plus 12 pre-existing tests = 19 total.
