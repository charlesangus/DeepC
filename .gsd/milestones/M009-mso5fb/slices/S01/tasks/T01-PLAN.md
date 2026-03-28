---
estimated_steps: 9
estimated_files: 4
skills_used: []
---

# T01: Verify and finalize Rust FFI crate (opendefocus-deep)

The opendefocus-deep Rust staticlib crate was scaffolded during worktree setup. This task verifies it is complete and correct: the DeepSampleData #[repr(C)] struct layout matches the C header, the stub opendefocus_deep_render() is properly exported, cargo builds cleanly from the workspace root, and the C header is consistent with the Rust struct. Fix any gaps found.

Steps:
1. Read `crates/opendefocus-deep/src/lib.rs` — confirm DeepSampleData has `#[repr(C)]` and fields match `include/opendefocus_deep.h` (sample_counts, rgba, depth, pixel_count, total_samples). Confirm `opendefocus_deep_render` is `#[no_mangle] pub extern "C"` with the right signature.
2. Read `crates/opendefocus-deep/include/opendefocus_deep.h` — confirm the typedef struct fields match lib.rs exactly. Confirm extern "C" guard and correct parameter types.
3. Read `crates/opendefocus-deep/Cargo.toml` — confirm `crate-type = ["staticlib"]`, `name = "opendefocus-deep"`, edition 2021.
4. Read `Cargo.toml` (workspace root) — confirm it is a workspace with `members = ["crates/opendefocus-deep"]`.
5. Run `cargo build --release` from the workspace root. Confirm exit 0 and `target/release/libopendefocus_deep.a` exists and is ≥ 40 MB.
6. Fix any issues found. If the lib is absent or wrong, re-run cargo build after fixes.
7. Confirm `nm target/release/libopendefocus_deep.a | grep opendefocus_deep_render` shows the symbol exported.

## Inputs

- ``crates/opendefocus-deep/src/lib.rs``
- ``crates/opendefocus-deep/include/opendefocus_deep.h``
- ``crates/opendefocus-deep/Cargo.toml``
- ``Cargo.toml``

## Expected Output

- ``crates/opendefocus-deep/src/lib.rs``
- ``crates/opendefocus-deep/include/opendefocus_deep.h``
- ``crates/opendefocus-deep/Cargo.toml``
- ``Cargo.toml``
- ``target/release/libopendefocus_deep.a``

## Verification

cargo build --release 2>&1 | tail -3 && ls -lh target/release/libopendefocus_deep.a && nm target/release/libopendefocus_deep.a | grep opendefocus_deep_render
