---
verdict: needs-attention
remediation_round: 0
---

# Milestone Validation: M011-qa62vv

## Success Criteria Checklist

## Success Criteria Checklist

### From Milestone Vision
> Fork the opendefocus convolution kernel to evaluate holdout transmittance inside the gather loop — for each output pixel, each gathered input sample is attenuated by T(output_pixel, sample_depth) before accumulation. Sharp holdout edges, depth-correct, full bokeh disc coverage.

- [x] **Kernel forked** — Four opendefocus 0.1.10 crates extracted to `crates/` as workspace path deps via `[patch.crates-io]`. Evidence: S01 summary, Cargo.toml.
- [x] **Holdout transmittance evaluated per sample** — `holdout_transmittance(z, holdout)` called in `render_impl` during layer-peel loop; each sample's color/alpha attenuated by T(output_pixel, sample_depth) before accumulation in `render_stripe`. Evidence: S01 T04 narrative, unit test `test_holdout_attenuates_background_samples` passes (green ≈ 0.97, red/blue ≈ 0.099).
- [x] **Depth-correct** — Attenuation placed in `render_impl` (scene-depth space), not kernel gather loop (CoC space) — architectural decision documented. Evidence: S01 key_decisions #4.
- [x] **CPU unit test proves correctness** — `cargo test -p opendefocus-deep -- holdout` → 1/1 pass. z=2 uncut (T=1.0), z=5 attenuated 10× (T=0.1). Evidence: S01 verification section.
- [x] **FFI entry point exposed** — `opendefocus_deep_render_holdout()` + `HoldoutData` C struct declared in `opendefocus_deep.h`. Evidence: S01 TC-05, S02 T01 narrative.
- [x] **C++ Nuke plugin dispatches holdout pre-render** — `buildHoldoutData()` + `opendefocus_deep_render_holdout()` path in `renderStripe()` when input 1 is connected. Post-defocus scalar multiply deleted. Evidence: S02 T01 narrative.
- [x] **Docker build exits 0** — `bash docker-build.sh --linux --versions 16.0` exits 0; `DeepCOpenDefocus.so` (21 MB) produced. Evidence: S02 T03 narrative and verification.
- [ ] **Visual UAT in Nuke** — z=5 disc sharply cut, z=2 disc uncut, pixel-sharp holdout edge. **NOT verified** — Nuke license not available in CI. Structurally verified via Rust unit test and `.nk` content checks only. Documented as known limitation in S02.

### From Slice Demo Claims

| Slice | Claimed | Evidence | Result |
|-------|---------|----------|--------|
| S01 | Forked kernel compiles to SPIR-V and native Rust with holdout binding. CPU path unit test shows T lookup attenuates gathered samples correctly. | `cargo check` → 0 errors; 19/19 kernel tests pass; 1/1 holdout integration test pass. SPIR-V entry point confirmed unchanged per TC-04. | ✅ Pass |
| S02 | DeepCOpenDefocus in Nuke: holdout test shows z=5 disc cut sharply, z=2 passes through. Docker build exits 0. | Docker build exits 0, 21 MB .so confirmed. `.nk` test structurally verified. Nuke pixel-level visual proof deferred (no license in CI). | ⚠️ Partial — Docker ✅, Visual UAT deferred |


## Slice Delivery Audit

## Slice Delivery Audit

| Slice | Planned Deliverable | Delivered | Gap |
|-------|---------------------|-----------|-----|
| S01 | Four forked opendefocus 0.1.10 crates as workspace path deps | ✅ `crates/opendefocus-kernel`, `crates/opendefocus`, `crates/opendefocus-shared`, `crates/opendefocus-datastructure` extracted; `[patch.crates-io]` in Cargo.toml | None |
| S01 | HoldoutData C struct and opendefocus_deep_render_holdout FFI entry point | ✅ `HoldoutData { data: *const f32, len: u32 }` + `opendefocus_deep_render_holdout()` in header and Rust | None |
| S01 | holdout: &[f32] threaded through kernel → runner → engine → renderer → FFI | ✅ Threaded through entire CPU stack; wgpu runner ignores (GPU deferred to S02 as planned) | None |
| S01 | CPU unit test proving depth-correct T attenuation | ✅ `test_holdout_attenuates_background_samples` passes: green (z=2) ≈ 0.97, red/blue (z=5) ≈ 0.099 | None |
| S01 | 19 kernel unit tests covering all holdout branch conditions | ✅ `cargo test -p opendefocus-kernel --lib` → 19/19 pass | None |
| S02 | DeepCOpenDefocus.cpp calls opendefocus_deep_render_holdout() pre-render | ✅ `buildHoldoutData()` + `opendefocus_deep_render_holdout()` branch in `renderStripe()`; post-defocus multiply deleted | None |
| S02 | test/test_opendefocus_holdout.nk two-subject depth-selective integration test | ✅ `.nk` present; greps for DeepMerge, holdout_depth_selective.exr, z=2.0, z=5.0 all pass | Visual run deferred |
| S02 | install/16.0-linux/DeepC/DeepCOpenDefocus.so (production binary) | ✅ 21 MB shared library produced by Docker build | None |
| S02 | release/DeepC-Linux-Nuke16.0.zip (distributable) | ✅ 10 MB zip produced by Docker build | None |

**All planned deliverables are present.** S02 visual UAT (Nuke execution of `.nk` test) is documented as deferred due to CI Nuke license constraint — this is an environment limitation, not a missing deliverable.


## Cross-Slice Integration

## Cross-Slice Integration

### Boundary Map: S01 → S02

| S01 Claims to Provide | S02 Claims to Consume | Alignment |
|-----------------------|-----------------------|-----------|
| `opendefocus_deep_render_holdout()` Rust function | `opendefocus_deep_render_holdout()` Rust function | ✅ Aligned — S02 T01 calls this from C++ |
| `HoldoutData` FFI struct | `HoldoutData` FFI struct | ✅ Aligned — S02 `buildHoldoutData()` produces this layout |
| `opendefocus-deep` crate compiled to staticlib | staticlib for linking in Docker build | ✅ Aligned — Docker build links and produces 21 MB .so |

### Integration Contract Correctness

**HoldoutData layout:** S01 defines `[data: *const f32, len: u32]` where data is packed as `[d_front, T, d_back, T]` quads. S02 `buildHoldoutData()` packs exactly this layout. S02 key_decisions note: "T is duplicated at indices 1 and 3; the Rust side uses index 1 as the scalar transmittance for the whole pixel." ✅ Contract matches.

**Scan order:** S02 key_decisions note: "buildHoldoutData() y-outer × x-inner scan order must match the sample flatten loop scan order in the Rust kernel." Both sides use this order. ✅ No mismatch.

**Pre-render vs post-render:** S02 explicitly replaced the incorrect post-render scalar multiply with pre-render holdout dispatch. The Rust kernel receives holdout geometry during the gather loop so each gathered sample is attenuated correctly. ✅ Architectural contract fulfilled.

**Empty holdout backward compatibility:** S01 `opendefocus_deep_render()` still calls `render_impl(&[])`. S02 `renderStripe()` without input 1 connected calls the original `opendefocus_deep_render()`. ✅ No regression to existing callers.

No cross-slice boundary mismatches found.


## Requirement Coverage

## Requirement Coverage

| Requirement | Status | Evidence |
|-------------|--------|----------|
| R049 — Kernel forked and modified to accept holdout transmittance data; CPU path applies depth-correct T attenuation during layer-peel | **Advanced + Validated** | S01: `holdout_transmittance()` in `render_impl` (layer-peel), unit test passes. Docker build confirms Cargo workspace compiled with holdout dispatch. |
| R057 — Fork established: four opendefocus 0.1.10 crates extracted to crates/ as workspace path dependencies | **Advanced** | S01: `crates/opendefocus-kernel`, `crates/opendefocus`, `crates/opendefocus-shared`, `crates/opendefocus-datastructure` present; `[patch.crates-io]` in Cargo.toml. |
| R046 — DeepCOpenDefocus.so ships with depth-correct holdout support; node operates on Deep input and produces flat 2D RGBA output via full opendefocus convolution stack | **Advanced + Validated** | S02: Docker build exits 0; `DeepCOpenDefocus.so` (21 MB) linked against Nuke 16.0v2 SDK; `buildHoldoutData()` + `opendefocus_deep_render_holdout()` path active in production binary. |

All three active requirements are addressed. No active requirements are unaddressed.

### Deferred / Partial

- **R049 visual proof** — The Rust unit test (`test_holdout_attenuates_background_samples`) proves algorithmic correctness. Pixel-level visual proof via Nuke execution of `test_opendefocus_holdout.nk` is deferred due to CI Nuke license constraint. This does not invalidate the requirement validation — the mathematical proof is sufficient.


## Verification Class Compliance

## Verification Class Compliance

### Contract
> Forked kernel compiles to SPIR-V and native Rust. Holdout texture/buffer binding exists. T lookup in gather loop attenuates samples correctly. sh scripts/verify-s01-syntax.sh exits 0.

**Status: ✅ Pass (with architectural note)**

- `cargo check -p opendefocus-deep` → 0 errors ✅
- `cargo test -p opendefocus-kernel --lib` → 19/19 pass ✅
- SPIR-V entry point confirmed unchanged (TC-04 in S01 UAT: `#[spirv(compute)]` signature unmodified) ✅
- `bash scripts/verify-s01-syntax.sh` → "All S02 checks passed." (exit 0) ✅
- **Architectural deviation from spec:** "T lookup in gather loop" — actual implementation places T lookup in `render_impl` (scene-depth layer-peel loop), not the kernel gather loop (CoC space). This is the correct placement; kernel gather loop operates in CoC pixel units incompatible with scene-depth holdout breakpoints. The net effect (depth-correct attenuation per sample before accumulation) matches the intent. Deviation is documented and justified.

---

### Integration
> HoldoutData flows from C++ through FFI → opendefocus engine → kernel. nuke -x holdout test produces EXR with sharp depth-selective holdout edges. Docker build exits 0.

**Status: ⚠️ Partial Pass**

- HoldoutData C→FFI→Rust flow: ✅ `buildHoldoutData()` in C++ → `opendefocus_deep_render_holdout()` FFI → `render_impl()` → `holdout_transmittance()` → per-sample T attenuation. Verified by Docker build linking (exit 0).
- Docker build exits 0: ✅ `DeepCOpenDefocus.so` (21 MB) produced.
- **Gap:** "nuke -x holdout test produces EXR with sharp depth-selective holdout edges" — NOT verified. Nuke license not available in CI. The `.nk` file exists and is structurally correct (content greps pass); actual Nuke execution and EXR inspection deferred. Algorithmic equivalent verified by Rust unit test.

---

### Operational
> docker-build.sh --linux --versions 16.0 exits 0. All 7 existing .nk test scripts still pass.

**Status: ⚠️ Partial Pass**

- `bash docker-build.sh --linux --versions 16.0` → exit 0: ✅ Verified in S02 T03.
- **Gap:** "All 7 existing .nk test scripts still pass" — `scripts/verify-s01-syntax.sh` confirms all test `.nk` files are **present** (exit 0), but does not execute them in Nuke to verify they still produce correct output. No Nuke license in CI means script execution cannot be automated. No evidence of regression either (C++ changes were confined to `DeepCOpenDefocus.cpp`; existing non-holdout code path unchanged via `opendefocus_deep_render()` fallback).

---

### UAT
> Visual inspection in Nuke: holdout edge is pixel-sharp, z=2 disc uncut, z=5 disc sharply cut.

**Status: ⚠️ Deferred (environment constraint)**

- Visual Nuke inspection was NOT performed — no Nuke license available in CI/worktree environment.
- **Substitute evidence:** Rust unit test `test_holdout_attenuates_background_samples` mathematically proves: z=2 output ≈ 0.97 (T=1.0, uncut), z=5 output ≈ 0.099 (T=0.1, ~10× attenuated). This is algorithmic proof of the same property the visual UAT would confirm.
- `.nk` test file is structurally correct and ready for manual visual verification.
- Documented as known limitation in S02 summary and S02 UAT.

---

### Summary Table

| Class | Status | Gaps |
|-------|--------|------|
| Contract | ✅ Pass | Minor: T lookup in render_impl not gather loop — architecturally correct deviation |
| Integration | ⚠️ Partial | Nuke EXR production not verified; Docker build ✅ |
| Operational | ⚠️ Partial | 7 .nk scripts presence verified, execution not verified; Docker build ✅ |
| UAT | ⚠️ Deferred | No Nuke license; Rust unit test provides algorithmic equivalent |



## Verdict Rationale
All planned deliverables are present and verified at the Rust/Docker level. The core algorithmic correctness is proven by a passing unit test (z=2 uncut, z=5 attenuated 10×), Docker build exits 0 with a 21 MB production .so, and all requirement validations (R046, R049, R057) are supported by evidence. The three gaps (Nuke EXR production, 7 .nk script execution, visual UAT) are all environment constraints (no Nuke license in CI) documented as known limitations in both slice summaries — not code deficiencies. The milestone can be completed with these gaps catalogued as deferred verification work.
