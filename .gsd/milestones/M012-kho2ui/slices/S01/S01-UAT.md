# S01: Singleton renderer + layer-peel dedup — UAT

**Milestone:** M012-kho2ui
**Written:** 2026-03-30T23:11:48.648Z

## S01 UAT: Singleton renderer + layer-peel dedup

### Preconditions
- Rust toolchain ≥ 1.94.1 installed (`~/.cargo/bin/cargo --version`)
- `PROTOC` env var pointing to protoc ≥ 29.x binary (required for prost-build in opendefocus-datastructure)
- Working directory: repo root (where `Cargo.toml` workspace is)
- All source edits from T01–T04 committed to the working tree

---

### Test Case 1: OnceLock singleton is present in source
**What it proves:** T01 deliverable — static renderer singleton introduced

**Steps:**
1. Run: `grep -q 'OnceLock' crates/opendefocus-deep/src/lib.rs && echo PASS || echo FAIL`
2. Run: `grep -q 'get_or_init_renderer\|OnceLock' crates/opendefocus-deep/src/lib.rs && echo PASS || echo FAIL`

**Expected outcome:** Both commands print `PASS`.

---

### Test Case 2: test_singleton_reuse passes on CPU path
**What it proves:** Singleton is reused across two FFI calls without panic or NaN output

**Steps:**
1. Run: `PROTOC=<protoc-path> ~/.cargo/bin/cargo test -p opendefocus-deep --features 'opendefocus/protobuf-vendored' -- test_singleton_reuse --nocapture`
2. Observe output: both `opendefocus_deep_render` calls complete without panic.
3. Verify exit code 0.

**Expected outcome:** `test test_singleton_reuse ... ok`. No panic, no thread abort.

---

### Test Case 3: prepare_filter_mipmaps and render_stripe_prepped API present
**What it proves:** T02 deliverable — hoisted mipmap prep API added to opendefocus crate

**Steps:**
1. `grep -q 'prepare_filter_mipmaps' crates/opendefocus/src/lib.rs && echo PASS || echo FAIL`
2. `grep -q 'render_stripe_prepped' crates/opendefocus/src/lib.rs && echo PASS || echo FAIL`
3. `grep -q 'render_with_prebuilt_mipmaps' crates/opendefocus/src/worker/engine.rs && echo PASS || echo FAIL`
4. `grep -q 'prepare_filter_mipmaps' crates/opendefocus-deep/src/lib.rs && echo PASS || echo FAIL`

**Expected outcome:** All four commands print `PASS`.

---

### Test Case 4: Existing correctness tests pass after layer-peel dedup refactor
**What it proves:** T02 refactor introduced no regression in holdout correctness

**Steps:**
1. Run: `PROTOC=<protoc-path> ~/.cargo/bin/cargo test -p opendefocus-deep --features 'opendefocus/protobuf-vendored' -- --nocapture`
2. Observe: `test_holdout_attenuates_background_samples` and `test_singleton_reuse` both pass.
3. Check output values: z=2 layer uncut (T≈0.97), z=5 layer attenuated ~10× (T≈0.099).

**Expected outcome:** `2 tests passed` (or more if timing tests also run). Output values match the pre-T02 baseline.

---

### Test Case 5: Quality parameter wired end-to-end — FFI header
**What it proves:** T03 deliverable — `int quality` present in both FFI signatures

**Steps:**
1. `grep -c 'quality' crates/opendefocus-deep/include/opendefocus_deep.h`
2. Verify the count is ≥ 2 (appears in both `opendefocus_deep_render` and `opendefocus_deep_render_holdout`).

**Expected outcome:** Count ≥ 2.

---

### Test Case 6: Quality Enumeration_knob in C++ — exactly one occurrence
**What it proves:** T03 deliverable — `Enumeration_knob` present in DeepCOpenDefocus.cpp exactly once (no duplicates, no comment noise)

**Steps:**
1. `grep -c 'Enumeration_knob' src/DeepCOpenDefocus.cpp`
2. Verify output is `1`.

**Expected outcome:** `1`

---

### Test Case 7: C++ syntax check passes for all three files
**What it proves:** T03 quality knob additions and all prior changes are syntactically valid C++17

**Steps:**
1. `bash scripts/verify-s01-syntax.sh`
2. Observe per-file output: "Syntax check passed: DeepCBlur.cpp", "Syntax check passed: DeepCDepthBlur.cpp", "Syntax check passed: DeepCOpenDefocus.cpp"
3. Observe final line: "All syntax checks passed."
4. Observe .nk artifact checks: all 7 test_opendefocus_*.nk confirmed.

**Expected outcome:** Exit code 0. All "passed" lines visible.

---

### Test Case 8: Timing tests present and assert warm < 5000 ms
**What it proves:** T04 deliverable — regression guard for singleton reuse

**Steps:**
1. `grep -q 'test_render_timing' crates/opendefocus-deep/src/lib.rs && echo PASS || echo FAIL`
2. `grep -q 'test_holdout_timing' crates/opendefocus-deep/src/lib.rs && echo PASS || echo FAIL`
3. `grep -q 'warm_ms < 5000' crates/opendefocus-deep/src/lib.rs && echo PASS || echo FAIL`

**Expected outcome:** All three print `PASS`.

---

### Test Case 9 (Optional — requires Docker): Full test suite with timing output
**What it proves:** All tests pass in the Docker build environment with real Rust toolchain

**Steps:**
1. `PROTOC=<protoc-path> ~/.cargo/bin/cargo test -p opendefocus-deep --features 'opendefocus/protobuf-vendored' -- --nocapture 2>&1 | tee /tmp/s01-test-output.txt`
2. Verify exit code 0.
3. Observe `eprintln!` output for `test_render_timing` and `test_holdout_timing` — cold and warm elapsed times printed.
4. Verify both warm times are well under 5000 ms on first run.

**Expected outcome:** 4 tests pass. Cold time visible (includes singleton init). Warm time visible and < 5000 ms.

---

### Edge Case A: GPU flag on second call (CPU singleton reuse)
**What it proves:** Singleton ignores subsequent use_gpu=1 calls gracefully (does not crash)

**Steps:**
1. In a test context, call `opendefocus_deep_render` with `use_gpu=0` then with `use_gpu=1` on a 2×2 image.
2. Verify both calls return without panic.

**Expected outcome:** No panic. The second call silently uses the CPU renderer. (GPU re-init on flag change is deferred per D029.)

---

### Edge Case B: render_stripe_prepped with single-layer input
**What it proves:** Layer loop with N=1 layer still calls prepare_filter_mipmaps and render_stripe_prepped without error

**Steps:**
1. `test_render_timing` and `test_singleton_reuse` both exercise the single-layer path.
2. Both should pass per Test Case 2 and Test Case 9 above.

**Expected outcome:** Pass — single-layer is the baseline case.

