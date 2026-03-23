---
id: T02
parent: S01
milestone: M005-9j5er8
provides:
  - Authoritative compilation proof of DeepCDepthBlur.cpp against Nuke 16.0 SDK
  - DeepCDepthBlur.so binary in release zip
  - All source contract grep checks passing
key_files:
  - release/DeepC-Linux-Nuke16.0.zip
key_decisions: []
patterns_established: []
observability_surfaces:
  - "unzip -l release/DeepC-Linux-Nuke16.0.zip | grep DeepCDepthBlur — confirms .so presence and size"
  - "grep contracts on src/DeepCDepthBlur.cpp confirm formula, labels, and guards at source level"
duration: 5m
verification_result: passed
completed_at: 2026-03-22
blocker_discovered: false
---

# T02: Docker build verification and source contract checks

**Docker build against Nuke 16.0 SDK succeeds, producing DeepCDepthBlur.so with all source contracts verified**

## What Happened

Ran the authoritative Docker build (`docker-build.sh --linux --versions 16.0`) which compiled DeepCDepthBlur.cpp against the real Nuke 16.0v2 SDK with GCC 11.2.1. The build completed with zero errors (only pre-existing deprecation warnings in DeepCWorld.cpp from Nuke SDK headers — unrelated to our changes). DeepCDepthBlur.so was produced and included in the release zip at 415,696 bytes.

Ran all five source grep contracts confirming: multiplicative `pow(1` formula present, no old `"B"` label, `"ref"` label present, `1e-6f` zero-alpha guards present, and `Chan_Alpha` explicit alpha dispatch present.

The syntax check (`scripts/verify-s01-syntax.sh`) was run first as a fast pre-flight and passed, confirming T01's changes are intact.

## Verification

All slice verification checks pass — this is the final task of S01:

1. `scripts/verify-s01-syntax.sh` exits 0 — syntax valid with mock DDImage headers
2. `docker-build.sh --linux --versions 16.0` exits 0 — authoritative SDK compilation proof
3. `grep -q "pow(1" src/DeepCDepthBlur.cpp` — multiplicative formula present
4. `grep -c '"B"' src/DeepCDepthBlur.cpp` returns 0 — old label removed
5. `grep -q '"ref"' src/DeepCDepthBlur.cpp` — new label present
6. `grep -q '1e-6f' src/DeepCDepthBlur.cpp` — zero-alpha guards present
7. `unzip -l release/DeepC-Linux-Nuke16.0.zip | grep DeepCDepthBlur` — .so in zip

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `scripts/verify-s01-syntax.sh` | 0 | ✅ pass | ~2s |
| 2 | `docker-build.sh --linux --versions 16.0` | 0 | ✅ pass | ~42s |
| 3 | `grep -q 'pow(1' src/DeepCDepthBlur.cpp` | 0 | ✅ pass | <1s |
| 4 | `grep -c '"B"' src/DeepCDepthBlur.cpp` → 0 | 0 | ✅ pass | <1s |
| 5 | `grep -q '"ref"' src/DeepCDepthBlur.cpp` | 0 | ✅ pass | <1s |
| 6 | `grep -q '1e-6f' src/DeepCDepthBlur.cpp` | 0 | ✅ pass | <1s |
| 7 | `unzip -l release/*.zip \| grep DeepCDepthBlur` → 415696 bytes | 0 | ✅ pass | <1s |

## Diagnostics

- **Build artifact inspection**: `unzip -l release/DeepC-Linux-Nuke16.0.zip | grep DeepCDepthBlur` confirms .so presence and size.
- **Source contract inspection**: The five grep contracts (pow formula, no "B" label, "ref" label, 1e-6f guards, Chan_Alpha dispatch) can be re-run at any time to verify source-level invariants.
- **Build failure visibility**: Docker build logs show per-target compilation progress; DeepCDepthBlur.cpp compiles at the ~49-50% mark. Any SDK API mismatch would surface as a compile error at that point.

## Deviations

None — build succeeded on first attempt with no source fixes needed.

## Known Issues

- Pre-existing deprecation warnings in `DeepCWorld.cpp` from Nuke 16.0 SDK CameraOp headers (haperture_, focal_length_, etc.). These are unrelated to this slice's changes and exist in the upstream codebase.

## Files Created/Modified

- `release/DeepC-Linux-Nuke16.0.zip` — Build output containing DeepCDepthBlur.so and all other DeepC plugins
- `.gsd/milestones/M005-9j5er8/slices/S01/S01-PLAN.md` — Marked T02 done
