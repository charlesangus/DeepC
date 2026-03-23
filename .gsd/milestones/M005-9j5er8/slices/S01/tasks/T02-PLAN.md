---
estimated_steps: 3
estimated_files: 1
skills_used: []
---

# T02: Docker build verification and source contract checks

**Slice:** S01 — Correct alpha decomposition + input rename
**Milestone:** M005-9j5er8

## Description

Run the authoritative Docker build against the real Nuke 16.0 SDK to prove the modified `DeepCDepthBlur.cpp` compiles correctly. The syntax check (T01) uses mock DDImage headers and cannot catch real SDK API mismatches — `docker-build.sh` is the only reliable compilation proof. If the build fails, diagnose and fix the source, then re-run until it passes. Finally, run all source inspection grep contracts.

## Steps

1. **Run Docker build**: `docker-build.sh --linux --versions 16.0`. This must exit 0 and produce a zip containing `DeepCDepthBlur.so`. If it fails, read the error output, fix the C++ source in `src/DeepCDepthBlur.cpp`, re-run `scripts/verify-s01-syntax.sh` first (fast iteration), then retry docker build.

2. **Run source grep contracts**:
   - `grep -q "pow(1" src/DeepCDepthBlur.cpp` — multiplicative formula present
   - `test "$(grep -c '"B"' src/DeepCDepthBlur.cpp)" -eq 0` — old label completely removed
   - `grep -q '"ref"' src/DeepCDepthBlur.cpp` — new label present
   - `grep -q '1e-6f' src/DeepCDepthBlur.cpp` — zero-alpha guards present
   - `grep -q 'Chan_Alpha' src/DeepCDepthBlur.cpp` — explicit alpha dispatch exists

3. **Verify .so in build output**: Confirm `DeepCDepthBlur.so` exists in the build output zip.

## Must-Haves

- [ ] `docker-build.sh --linux --versions 16.0` exits 0
- [ ] `DeepCDepthBlur.so` present in build output
- [ ] All source grep contracts pass

## Verification

- `docker-build.sh --linux --versions 16.0` exits 0
- Build output zip contains `DeepCDepthBlur.so`
- All grep contracts from step 2 pass

## Inputs

- `src/DeepCDepthBlur.cpp` — modified source from T01 with multiplicative alpha decomposition

## Expected Output

- `src/DeepCDepthBlur.cpp` — potentially with minor fixes if docker build reveals SDK mismatches
