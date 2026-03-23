---
id: T01
parent: S01
milestone: M005-9j5er8
provides:
  - Multiplicative alpha decomposition in DeepCDepthBlur::doDeepEngine
  - Zero-alpha input guard and sub-sample skip
  - Nuke-convention input labels ("" and "ref")
key_files:
  - src/DeepCDepthBlur.cpp
key_decisions:
  - Use static_cast<double> for pow precision, static_cast<float> on result for -Wconversion safety
  - Place srcAlpha fetch and zero-alpha guard before sub-sample loop to avoid redundant per-sub-sample checks
patterns_established:
  - Multiplicative alpha decomposition pattern: α_sub = 1 - pow(1-α, w) with premult scaling c * (α_sub / α)
observability_surfaces:
  - Zero-alpha guard produces pass-through samples observable via Nuke deep-inspect
  - alphaSub < 1e-6f guard silently skips corrupt sub-samples rather than emitting NaN
duration: 10m
verification_result: passed
completed_at: 2026-03-22
blocker_discovered: false
---

# T01: Apply multiplicative alpha decomposition, zero-alpha guards, and input label rename

**Replace additive alpha scaling (channel * w) with multiplicative decomposition (α_sub = 1 - pow(1-α, w)), add zero-alpha guards, and rename input labels to Nuke conventions**

## What Happened

Made three changes to `src/DeepCDepthBlur.cpp`:

1. **Input labels**: Changed `input_label` to return `""` for input 0 (was `"Source"`) and `"ref"` for input 1 (was `"B"`), following Nuke conventions per D020/R017.

2. **Zero-alpha input guard**: Before the sub-sample loop, fetch `srcAlpha` once via `px.getUnorderedSample(s, Chan_Alpha)`. If `srcAlpha < 1e-6f`, pass the sample through unchanged and skip spreading entirely. This prevents division by zero in the premult scaling and avoids unnecessary work.

3. **Multiplicative alpha decomposition**: Inside the sub-sample loop, replaced `px.getUnorderedSample(s, z) * w` with the correct multiplicative formula: `alphaSub = static_cast<float>(1.0 - std::pow(1.0 - static_cast<double>(srcAlpha), static_cast<double>(w)))`. Added explicit `Chan_Alpha` branch that emits `alphaSub`, and other channels scale by `alphaSub / srcAlpha` (premult-correct). Sub-samples with `alphaSub < 1e-6f` are skipped.

## Verification

- `scripts/verify-s01-syntax.sh` exits 0 — syntax valid with mock DDImage headers
- `grep -q "pow(1" src/DeepCDepthBlur.cpp` — multiplicative formula present
- `grep -c '"B"' src/DeepCDepthBlur.cpp` returns 0 — old label removed
- `grep -q '"ref"' src/DeepCDepthBlur.cpp` — new label present
- `grep -c '1e-6f' src/DeepCDepthBlur.cpp` returns 4 — guards present (input + sub-alpha + existing pass-through threshold + spread threshold)

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `scripts/verify-s01-syntax.sh` | 0 | ✅ pass | ~2s |
| 2 | `grep -q 'pow(1' src/DeepCDepthBlur.cpp` | 0 | ✅ pass | <1s |
| 3 | `grep -c '"B"' src/DeepCDepthBlur.cpp` → 0 | 0 | ✅ pass | <1s |
| 4 | `grep -q '"ref"' src/DeepCDepthBlur.cpp` | 0 | ✅ pass | <1s |
| 5 | `grep -c '1e-6f' src/DeepCDepthBlur.cpp` → 4 | 0 | ✅ pass | <1s |

## Diagnostics

- Zero-alpha guard: samples with α < 1e-6f pass through unchanged — observable in Nuke deep-inspect as identical input/output samples.
- Sub-alpha skip: sub-samples with alphaSub < 1e-6f are silently dropped, preventing NaN propagation from degenerate pow results.
- Source inspection: `grep -c '1e-6f'` confirms both guard paths exist; `grep 'pow(1'` confirms multiplicative formula.

## Deviations

None — implementation matches the task plan exactly.

## Known Issues

- Docker build verification deferred to T02 (this task only covers source changes and syntax checks).

## Files Created/Modified

- `src/DeepCDepthBlur.cpp` — Multiplicative alpha decomposition, zero-alpha guards, input label rename
- `.gsd/milestones/M005-9j5er8/slices/S01/S01-PLAN.md` — Added Observability / Diagnostics section, marked T01 done
