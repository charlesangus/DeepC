---
id: T02
parent: S01
milestone: M011-qa62vv
provides: []
requires: []
affects: []
key_files: ["crates/opendefocus-kernel/src/lib.rs", "crates/opendefocus-kernel/src/stages/ring.rs", "crates/opendefocus/src/runners/runner.rs", "crates/opendefocus/src/runners/cpu.rs", "crates/opendefocus/src/runners/wgpu.rs", "crates/opendefocus/src/runners/shared_runner.rs"]
key_decisions: ["Used sample.coc as depth proxy for holdout threshold comparison per task plan direction", "Added holdout/output_width params under #[cfg(not(any(target_arch = spirv)))] guards — SPIR-V path unchanged", "GPU runner receives holdout as _holdout (ignored) — GPU holdout out of T02 scope", "convolve() passes &[] as holdout — caller wiring left for downstream task"]
patterns_established: []
drill_down_paths: []
observability_surfaces: []
duration: ""
verification_result: "Ran cargo test -p opendefocus-kernel: 19/19 pass (12 pre-existing + 7 new holdout tests). Ran cargo check -p opendefocus-kernel: zero errors. Ran cargo check -p opendefocus-deep: zero errors. Task-plan verification command output: PASS."
completed_at: 2026-03-29T04:11:38.884Z
blocker_discovered: false
---

# T02: Added holdout transmittance parameter to CPU kernel path and implemented depth-correct T attenuation in calculate_ring gather loop, verified by 19/19 passing tests

> Added holdout transmittance parameter to CPU kernel path and implemented depth-correct T attenuation in calculate_ring gather loop, verified by 19/19 passing tests

## What Happened
---
id: T02
parent: S01
milestone: M011-qa62vv
key_files:
  - crates/opendefocus-kernel/src/lib.rs
  - crates/opendefocus-kernel/src/stages/ring.rs
  - crates/opendefocus/src/runners/runner.rs
  - crates/opendefocus/src/runners/cpu.rs
  - crates/opendefocus/src/runners/wgpu.rs
  - crates/opendefocus/src/runners/shared_runner.rs
key_decisions:
  - Used sample.coc as depth proxy for holdout threshold comparison per task plan direction
  - Added holdout/output_width params under #[cfg(not(any(target_arch = spirv)))] guards — SPIR-V path unchanged
  - GPU runner receives holdout as _holdout (ignored) — GPU holdout out of T02 scope
  - convolve() passes &[] as holdout — caller wiring left for downstream task
duration: ""
verification_result: passed
completed_at: 2026-03-29T04:11:38.884Z
blocker_discovered: false
---

# T02: Added holdout transmittance parameter to CPU kernel path and implemented depth-correct T attenuation in calculate_ring gather loop, verified by 19/19 passing tests

**Added holdout transmittance parameter to CPU kernel path and implemented depth-correct T attenuation in calculate_ring gather loop, verified by 19/19 passing tests**

## What Happened

Modified opendefocus-kernel to thread holdout: &[f32] through the CPU execution path. Added holdout and output_width params to global_entrypoint and calculate_ring under #[cfg(not(spirv))] guards — SPIR-V entry point unchanged. Implemented apply_holdout_attenuation helper that reads (d0, T0, d1, T1) breakpoints per pixel, applies T attenuation to sample.color/alpha/alpha_masked, and leaves kernel/weight untouched. Updated ConvolveRunner trait and all runner implementations (cpu, wgpu, shared_runner) to carry the new holdout parameter. Added 7 unit tests covering all branches: below d0 (T=1.0), between d0/d1 (T=T0), beyond d1 (T=T1), empty holdout, out-of-bounds pixel, multi-pixel lookup, and kernel/weight unchanged invariant.

## Verification

Ran cargo test -p opendefocus-kernel: 19/19 pass (12 pre-existing + 7 new holdout tests). Ran cargo check -p opendefocus-kernel: zero errors. Ran cargo check -p opendefocus-deep: zero errors. Task-plan verification command output: PASS.

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `PROTOC=... cargo test -p opendefocus-kernel 2>&1 | tail -10` | 0 | ✅ pass | 2600ms |
| 2 | `PROTOC=... cargo check -p opendefocus-kernel 2>&1 | grep -c '^error' | grep -q '^0$' && echo PASS` | 0 | ✅ pass | 2300ms |
| 3 | `PROTOC=... cargo check -p opendefocus-deep 2>&1` | 0 | ✅ pass | 2700ms |


## Deviations

Updated opendefocus runner crates (runner.rs, cpu.rs, wgpu.rs, shared_runner.rs) in addition to the two kernel files named in the plan — necessary because the kernel API change would break compilation. wgpu GPU runner receives holdout as _holdout (ignored) since GPU holdout is out of T02 scope.

## Known Issues

sample.coc is CoC magnitude (not raw scene depth). Used as depth proxy per task plan direction — directionally correct but approximate. Future tasks may want raw depth for physically accurate holdout comparisons.

## Files Created/Modified

- `crates/opendefocus-kernel/src/lib.rs`
- `crates/opendefocus-kernel/src/stages/ring.rs`
- `crates/opendefocus/src/runners/runner.rs`
- `crates/opendefocus/src/runners/cpu.rs`
- `crates/opendefocus/src/runners/wgpu.rs`
- `crates/opendefocus/src/runners/shared_runner.rs`


## Deviations
Updated opendefocus runner crates (runner.rs, cpu.rs, wgpu.rs, shared_runner.rs) in addition to the two kernel files named in the plan — necessary because the kernel API change would break compilation. wgpu GPU runner receives holdout as _holdout (ignored) since GPU holdout is out of T02 scope.

## Known Issues
sample.coc is CoC magnitude (not raw scene depth). Used as depth proxy per task plan direction — directionally correct but approximate. Future tasks may want raw depth for physically accurate holdout comparisons.
