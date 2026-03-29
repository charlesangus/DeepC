# S01: Fork opendefocus-kernel with holdout gather — UAT

**Milestone:** M011-qa62vv
**Written:** 2026-03-29T11:32:21.138Z

## UAT: S01 — Fork opendefocus-kernel with holdout gather

### Preconditions

- Working directory: `/home/latuser/git/DeepC/.gsd/worktrees/M011-qa62vv`
- Rust 1.94.1 installed via rustup: `$HOME/.cargo/bin/cargo --version` shows `cargo 1.94.1`
- protoc available at `/tmp/protoc_dir/bin/protoc` (re-fetch if /tmp was cleared)
- Canonical env prefix for all cargo commands: `PATH=$HOME/.cargo/bin:$PATH PROTOC=/tmp/protoc_dir/bin/protoc`

---

### TC-01: Workspace compiles cleanly

**What this proves:** All four forked crates plus opendefocus-deep form a coherent workspace with zero compilation errors.

**Steps:**
1. Run: `PATH=$HOME/.cargo/bin:$PATH PROTOC=/tmp/protoc_dir/bin/protoc cargo check -p opendefocus-deep`
2. Capture exit code and count lines matching `^error`.

**Expected outcome:**
- Zero lines matching `^error`
- `cargo check` exits 0

**Edge cases:**
- If protoc is missing: `failed to run custom build command for opendefocus-datastructure` — re-fetch protoc from GitHub releases
- If Rust 1.75 is active: `feature 'edition2024' is required` — run `rustup default 1.94.1`

---

### TC-02: Kernel unit tests — all 19 pass

**What this proves:** `apply_holdout_attenuation` covers all branch conditions: below d0 (T=1.0), between d0/d1 (T=T0), beyond d1 (T=T1), empty holdout, out-of-bounds pixel, multi-pixel lookup, and kernel/weight unchanged invariant.

**Steps:**
1. Run: `PATH=$HOME/.cargo/bin:$PATH PROTOC=/tmp/protoc_dir/bin/protoc cargo test -p opendefocus-kernel --lib`
2. Check the summary line.

**Expected outcome:**
- `test result: ok. 19 passed; 0 failed; 0 ignored`
- All 7 holdout-specific tests in `stages::ring::tests` report `ok`

**Edge cases:**
- If `test_holdout_*` tests are missing: the `#[cfg(not(spirv))]` guard on the test module may have been removed accidentally
- If `ConvolveSettings` size test fails: `ConvolveSettings` struct was inadvertently changed — it has a hard-coded 160-byte size assertion

---

### TC-03: Depth-correct holdout unit test — green > red and green > blue

**What this proves:** The full CPU pipeline from `opendefocus_deep_render_holdout` FFI → `render_impl` → `holdout_transmittance()` → `render_stripe` correctly attenuates samples behind the holdout (z=5.0 with T=0.1) while passing samples in front (z=2.0 with T=1.0).

**Steps:**
1. Run: `PATH=$HOME/.cargo/bin:$PATH PROTOC=/tmp/protoc_dir/bin/protoc cargo test -p opendefocus-deep -- holdout`
2. Check output.

**Expected outcome:**
- `test tests::test_holdout_attenuates_background_samples ... ok`
- `test result: ok. 1 passed; 0 failed`
- Internally: output pixel 1 green ≈ 0.97, pixels 0 and 2 red/blue ≈ 0.099 (10× attenuation)

**Edge cases:**
- If all three pixels read the same value: holdout is not being applied — check that `render_impl` calls `holdout_transmittance()` before passing samples to `render_stripe`, and that `render_stripe` receives `&[]`
- If test panics with index-out-of-bounds: holdout slice length mismatch — `HoldoutData.len` must equal `width * height * 4`
- If green reads near 0: holdout is being double-applied — check that `render_stripe` receives `&[]`, not the real holdout slice

---

### TC-04: SPIR-V entry point unchanged (no holdout param on GPU compute kernel)

**What this proves:** The `#[spirv(compute)]` entry point was not modified — only CPU-path code was changed.

**Steps:**
1. Run: `grep -n 'spirv(compute)' crates/opendefocus-kernel/src/lib.rs`
2. Find the function signature immediately following the attribute.
3. Confirm it does NOT include `holdout` or `output_width` parameters.

**Expected outcome:**
- The `#[spirv(compute)]` function signature is identical to the upstream 0.1.10 version
- Any `holdout` parameters appear only inside `#[cfg(not(any(target_arch = "spirv")))]` blocks

---

### TC-05: C FFI header declares HoldoutData and opendefocus_deep_render_holdout

**What this proves:** The C header exposes the correct interface for the C++ Nuke plugin to call.

**Steps:**
1. Run: `grep -c 'HoldoutData' crates/opendefocus-deep/include/opendefocus_deep.h`
2. Run: `grep -c 'opendefocus_deep_render_holdout' crates/opendefocus-deep/include/opendefocus_deep.h`
3. Run: `grep 'const float\* data' crates/opendefocus-deep/include/opendefocus_deep.h`

**Expected outcome:**
- `HoldoutData` appears at least 2 times (struct def + function param)
- `opendefocus_deep_render_holdout` appears at least 1 time
- `const float* data` appears in the `HoldoutData` struct

**Edge cases:**
- Missing `HoldoutData` struct: S02 cannot implement the Nuke plugin holdout input — this is a blocker for S02
- Missing `uint32_t len` field: S02 will have no safe way to guard against null/zero-length slices

---

### TC-06: Empty holdout slice is a no-op (backward compatibility)

**What this proves:** Existing `opendefocus_deep_render` callers (S02 Nuke plugin without holdout connected) are unaffected.

**Steps:**
1. Inspect `crates/opendefocus-deep/src/lib.rs` for the `opendefocus_deep_render` function body.
2. Confirm it calls `render_impl` with `&[]` as the holdout argument.
3. Confirm `holdout_transmittance(z, &[])` returns 1.0 (no attenuation) for any depth z.

**Expected outcome:**
- `opendefocus_deep_render` body contains `render_impl(... &[])` or equivalent
- `holdout_transmittance` with empty slice returns 1.0 for all inputs

---

### TC-07: Lockfile is version 3 (not version 4)

**What this proves:** cargo 1.75 (used in the Docker build environment) can parse the lockfile.

**Steps:**
1. Run: `head -4 Cargo.lock | grep 'version = 3'`

**Expected outcome:**
- Match found: `version = 3`

**Edge cases:**
- If version = 4: delete `Cargo.lock` and re-run `cargo check -p opendefocus-deep` with Rust 1.94.1 to regenerate as v3. Verify the Docker build environment uses cargo 1.75 or newer that accepts v3.

