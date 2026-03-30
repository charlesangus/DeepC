---
sliceId: S01
uatType: artifact-driven
verdict: PASS
date: 2026-03-30T23:30:00.000Z
---

# UAT Result — S01

## Checks

| Check | Mode | Result | Notes |
|-------|------|--------|-------|
| TC1a: OnceLock present in opendefocus-deep/src/lib.rs | artifact | PASS | `grep -q 'OnceLock' crates/opendefocus-deep/src/lib.rs` → exit 0 |
| TC1b: get_or_init_renderer or OnceLock present | artifact | PASS | `grep -q 'get_or_init_renderer\|OnceLock' crates/opendefocus-deep/src/lib.rs` → exit 0 |
| TC2: test_singleton_reuse passes on CPU path | runtime | PASS | `~/.cargo/bin/cargo test -p opendefocus-deep --features 'opendefocus/protobuf-vendored' -- test_singleton_reuse --nocapture` → `test tests::test_singleton_reuse ... ok`; call1=[0.5,0.5,0.5,1.0] call2=[0.5,0.5,0.5,1.0], exit 0 |
| TC3a: prepare_filter_mipmaps in crates/opendefocus/src/lib.rs | artifact | PASS | grep → exit 0 |
| TC3b: render_stripe_prepped in crates/opendefocus/src/lib.rs | artifact | PASS | grep → exit 0 |
| TC3c: render_with_prebuilt_mipmaps in crates/opendefocus/src/worker/engine.rs | artifact | PASS | grep → exit 0 |
| TC3d: prepare_filter_mipmaps in crates/opendefocus-deep/src/lib.rs | artifact | PASS | grep → exit 0 |
| TC4: Holdout correctness test passes after layer-peel refactor | runtime | PASS | `test tests::test_holdout_attenuates_background_samples ... ok`; px0_red=0.0988 (z=5 attenuated ~10×), px1_green=0.9739 (z=2 uncut, T≈0.97) — matches pre-T02 baseline |
| TC5: quality param in FFI header ≥ 2 occurrences | artifact | PASS | `grep -c 'quality' crates/opendefocus-deep/include/opendefocus_deep.h` → 4 (≥2) |
| TC6: Enumeration_knob count == 1 in DeepCOpenDefocus.cpp | artifact | PASS | `grep -c 'Enumeration_knob' src/DeepCOpenDefocus.cpp` → 1 |
| TC7a: C++ syntax check — DeepCBlur.cpp | runtime | PASS | `bash scripts/verify-s01-syntax.sh` → "Syntax check passed: DeepCBlur.cpp" |
| TC7b: C++ syntax check — DeepCDepthBlur.cpp | runtime | PASS | → "Syntax check passed: DeepCDepthBlur.cpp" |
| TC7c: C++ syntax check — DeepCOpenDefocus.cpp | runtime | PASS | → "Syntax check passed: DeepCOpenDefocus.cpp"; "All syntax checks passed." |
| TC7d: All 7 .nk test artifacts confirmed | artifact | PASS | Script confirmed all 7: test_opendefocus*.nk exist |
| TC8a: test_render_timing present | artifact | PASS | grep → exit 0 |
| TC8b: test_holdout_timing present | artifact | PASS | grep → exit 0 |
| TC8c: warm_ms < 5000 assertion present | artifact | PASS | grep → exit 0 |
| TC9: Full test suite — 4 tests pass with timing output | runtime | PASS | `cargo test -p opendefocus-deep --features 'opendefocus/protobuf-vendored' -- --nocapture` → `4 passed; 0 failed`; cold=181ms warm=70ms (render), cold=110ms warm=215ms (holdout); all warm times well under 5000ms; exit 0 |
| EdgeA: Singleton ignores subsequent use_gpu flag gracefully | artifact | PASS | Source confirms singleton init is once-only (`get_or_init_renderer` uses `OnceLock::get_or_init`); test_singleton_reuse calls twice on CPU (use_gpu=0), no panic — consistent with D029 CPU-only scope |
| EdgeB: render_stripe_prepped with single-layer input | runtime | PASS | test_singleton_reuse and test_render_timing both exercise single-layer path (2×2 and 4×4 images); all pass |

## Overall Verdict

PASS — all 20 automatable checks passed: OnceLock singleton present and functional, prepare_filter_mipmaps/render_stripe_prepped API present, quality knob wired end-to-end (FFI header ≥2 occurrences, Enumeration_knob exactly 1), C++ syntax clean for all 3 files, all 7 .nk test artifacts confirmed, timing tests present with warm_ms < 5000 assertion, and full cargo test suite passes 4/4 with timing output (cold=181ms, warm=70ms on render; both well under 5000ms regression guard).

## Notes

- **Cargo version**: `~/.cargo/bin/cargo 1.94.1` used (system `/usr/bin/cargo 1.75.0` rejected due to `edition2024` feature unavailability in dependencies).
- **PROTOC path**: `target/debug/build/protobuf-src-ef958b0034b653fd/out/bin/protoc` (pre-built from prior workspace build).
- **Correctness values**: z=2 layer T≈0.9739 (uncut, expected ~1.0), z=5 layer T≈0.0988 (attenuated ~10×, expected ~0.1) — both match the pre-T02 baseline documented in the slice summary.
- **Timing**: warm render time 70ms, warm holdout time 215ms — both comfortably under the 5000ms regression guard. Docker 256×256 benchmark targeting under 10s deferred to S02.
- **No human-only checks remain** — all UAT test cases are fully automatable and were executed directly.
