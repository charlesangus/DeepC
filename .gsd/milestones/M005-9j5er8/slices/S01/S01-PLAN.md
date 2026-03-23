# S01: Correct alpha decomposition + input rename

**Goal:** DeepCDepthBlur uses multiplicative alpha decomposition (`α_sub = 1 - (1-α)^w`), zero-alpha samples are handled correctly, and input labels follow Nuke conventions.
**Demo:** `scripts/verify-s01-syntax.sh` exits 0; `docker-build.sh --linux --versions 16.0` exits 0 producing DeepCDepthBlur.so; source inspection confirms multiplicative formula, "ref" label, and no "B" label.

## Must-Haves

- Alpha decomposition uses `1 - pow(1 - α, w)` instead of `α * w` (R013)
- Premult channels scale as `c * (α_sub / α)` not `c * w` (R013)
- Input samples with α < 1e-6 pass through unchanged without spreading (R018)
- Sub-samples with α_sub < 1e-6 are skipped (R018)
- Main input (0) labeled `""`, second input (1) labeled `"ref"` (R017, D020)
- Builds successfully via docker-build.sh

## Proof Level

- This slice proves: contract
- Real runtime required: no (flatten invariant holds by algebraic construction; visual UAT is user responsibility)
- Human/UAT required: yes (DeepToImage A/B comparison in Nuke — outside this slice's scope)

## Verification

- `scripts/verify-s01-syntax.sh` exits 0
- `docker-build.sh --linux --versions 16.0` exits 0 with DeepCDepthBlur.so in output
- `grep -q "pow(1" src/DeepCDepthBlur.cpp` — multiplicative formula present
- `grep -c '"B"' src/DeepCDepthBlur.cpp` returns 0 — old label removed
- `grep -q '"ref"' src/DeepCDepthBlur.cpp` — new label present
- `grep -q '1e-6f' src/DeepCDepthBlur.cpp` — zero-alpha guards present

## Tasks

- [x] **T01: Apply multiplicative alpha decomposition, zero-alpha guards, and input label rename** `est:30m`
  - Why: Core correctness fix — the current additive scaling (`channel * w`) breaks the flatten invariant for any α < 1. Input labels must follow Nuke conventions (D020, R017).
  - Files: `src/DeepCDepthBlur.cpp`
  - Do: (1) Change `input_label` to return `""` for input 0 and `"ref"` for input 1. (2) Before the sub-sample loop, fetch `srcAlpha` and add zero-alpha input guard that passes through unchanged. (3) Inside the sub-sample loop, compute `alphaSub = 1 - pow(1 - srcAlpha, w)`, skip if < 1e-6, add explicit `Chan_Alpha` branch, scale other channels by `alphaSub / srcAlpha`. Use `static_cast<float>` on the pow result.
  - Verify: `scripts/verify-s01-syntax.sh` exits 0; `grep -q "pow(1" src/DeepCDepthBlur.cpp`; `grep -c '"B"' src/DeepCDepthBlur.cpp` returns 0
  - Done when: syntax check passes, source contains multiplicative formula, zero-alpha guards, and correct labels

- [x] **T02: Docker build verification and source contract checks** `est:15m`
  - Why: Authoritative build proof — syntax check uses mock headers and cannot catch real SDK API mismatches. Docker build compiles against the real Nuke SDK.
  - Files: `src/DeepCDepthBlur.cpp`
  - Do: Run `docker-build.sh --linux --versions 16.0`. If it fails, diagnose and fix the C++ source. Run source inspection grep contracts.
  - Verify: `docker-build.sh --linux --versions 16.0` exits 0; `grep -c '"B"' src/DeepCDepthBlur.cpp` returns 0; `grep -q '"ref"' src/DeepCDepthBlur.cpp`; `grep -q '1e-6f' src/DeepCDepthBlur.cpp`
  - Done when: docker build produces DeepCDepthBlur.so, all grep contracts pass

## Files Likely Touched

- `src/DeepCDepthBlur.cpp`

## Observability / Diagnostics

- **Runtime signal**: Zero-alpha input guard produces a pass-through sample (same channel values as input) — observable via Nuke deep-inspect when α ≈ 0 samples appear unchanged in output.
- **Inspection surface**: `grep -c '1e-6f' src/DeepCDepthBlur.cpp` returns ≥ 2, confirming both guard paths are present in source.
- **Failure visibility**: If multiplicative decomposition produces NaN (e.g. negative alpha input), the `alphaSub < 1e-6f` guard skips the sub-sample rather than emitting corrupt data.
- **Diagnostic verification**: `scripts/verify-s01-syntax.sh` validates syntax with mock DDImage headers; grep contracts confirm formula and label correctness.
