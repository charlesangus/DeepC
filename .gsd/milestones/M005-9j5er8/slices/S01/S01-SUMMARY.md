---
id: S01
parent: M005-9j5er8
milestone: M005-9j5er8
provides:
  - Multiplicative alpha decomposition in DeepCDepthBlur::doDeepEngine (α_sub = 1 - pow(1-α, w))
  - Zero-alpha input guard: samples with α < 1e-6f pass through unchanged
  - Sub-sample skip: alphaSub < 1e-6f sub-samples are dropped rather than emitting NaN
  - Nuke-convention input labels: input 0 → "", input 1 → "ref" (was "Source"/"B")
  - Compiled DeepCDepthBlur.so in release/DeepC-Linux-Nuke16.0.zip (verified against Nuke 16.0v2 SDK)
requires: []
affects: []
key_files:
  - src/DeepCDepthBlur.cpp
  - release/DeepC-Linux-Nuke16.0.zip
key_decisions:
  - Use static_cast<double> for pow precision, static_cast<float> on result for -Wconversion safety
  - Place srcAlpha fetch and zero-alpha guard before sub-sample loop (not per-sub-sample) to avoid redundant work
patterns_established:
  - Multiplicative alpha decomposition: α_sub = 1 - pow(1-α, w) with premult scaling c * (α_sub / α)
observability_surfaces:
  - "grep -c '1e-6f' src/DeepCDepthBlur.cpp → ≥2 confirms both guard paths present"
  - "unzip -l release/DeepC-Linux-Nuke16.0.zip | grep DeepCDepthBlur → confirms .so presence and size"
  - "Zero-alpha guard produces pass-through samples observable in Nuke deep-inspect as identical input/output"
drill_down_paths:
  - .gsd/milestones/M005-9j5er8/slices/S01/tasks/T01-SUMMARY.md
  - .gsd/milestones/M005-9j5er8/slices/S01/tasks/T02-SUMMARY.md
duration: 15m
verification_result: passed
completed_at: 2026-03-22
---

# S01: Correct alpha decomposition + input rename

**Fixed DeepCDepthBlur to use multiplicative alpha decomposition with zero-alpha guards and Nuke-convention input labels; confirmed clean compile against Nuke 16.0v2 SDK**

## What Happened

**T01** made three targeted changes to `src/DeepCDepthBlur.cpp`:

1. **Input labels**: `input_label` now returns `""` for input 0 and `"ref"` for input 1, replacing the non-standard `"Source"` and `"B"` labels (D020, R017).

2. **Zero-alpha input guard**: Before the sub-sample loop, `srcAlpha` is fetched once via `px.getUnorderedSample(s, Chan_Alpha)`. If `srcAlpha < 1e-6f`, the sample passes through unchanged and sub-sample spreading is skipped entirely, preventing division-by-zero in premult scaling.

3. **Multiplicative alpha decomposition**: Inside the sub-sample loop, the old additive formula (`channel * w`) is replaced with `alphaSub = static_cast<float>(1.0 - std::pow(1.0 - static_cast<double>(srcAlpha), static_cast<double>(w)))`. An explicit `Chan_Alpha` branch emits `alphaSub`; all other channels scale by `alphaSub / srcAlpha` (premult-correct). Sub-samples with `alphaSub < 1e-6f` are silently dropped to prevent NaN propagation.

**T02** ran the authoritative Docker build against the real Nuke 16.0v2 SDK (GCC 11.2.1). The build completed with zero errors. DeepCDepthBlur.so was produced at 415,696 bytes and included in `release/DeepC-Linux-Nuke16.0.zip`. All five source grep contracts were confirmed passing.

## Verification

All slice verification checks passed:

| # | Check | Verdict |
|---|-------|---------|
| 1 | `scripts/verify-s01-syntax.sh` exits 0 | ✅ pass |
| 2 | `docker-build.sh --linux --versions 16.0` exits 0 | ✅ pass |
| 3 | `grep -q "pow(1" src/DeepCDepthBlur.cpp` | ✅ pass |
| 4 | `grep -c '"B"' src/DeepCDepthBlur.cpp` → 0 | ✅ pass |
| 5 | `grep -q '"ref"' src/DeepCDepthBlur.cpp` | ✅ pass |
| 6 | `grep -q '1e-6f' src/DeepCDepthBlur.cpp` | ✅ pass |
| 7 | `unzip -l release/*.zip \| grep DeepCDepthBlur` → 415696 bytes | ✅ pass |

## Diagnostics

- **Source contracts**: The five grep checks above can be re-run at any time to verify formula, labels, and guards at source level.
- **Build artifact**: `unzip -l release/DeepC-Linux-Nuke16.0.zip | grep DeepCDepthBlur` confirms .so presence and size.
- **Runtime guard**: Zero-alpha input samples pass through unchanged — observable in Nuke deep-inspect as identical input/output samples when α ≈ 0.
- **NaN prevention**: `alphaSub < 1e-6f` guard ensures degenerate pow results (e.g. from negative alpha inputs) are dropped rather than emitted as corrupt data.

## Deviations

None — both tasks matched their plans exactly. Docker build succeeded on first attempt with no source fixes needed.

## Known Issues

- Pre-existing deprecation warnings in `DeepCWorld.cpp` from Nuke 16.0 SDK `CameraOp.h` headers (`haperture_`, `focal_length_`, etc.). Unrelated to this slice's changes; present in upstream codebase.
- Human/UAT (DeepToImage A/B comparison in Nuke) is outside this slice's scope and deferred to user verification.

## Files Created/Modified

- `src/DeepCDepthBlur.cpp` — Multiplicative alpha decomposition, zero-alpha guards, Nuke-convention input labels
- `release/DeepC-Linux-Nuke16.0.zip` — Build output containing verified DeepCDepthBlur.so
