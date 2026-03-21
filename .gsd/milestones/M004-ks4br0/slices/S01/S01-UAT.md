# S01: Fix DeepSampleOptimizer colour comparison — UAT

**Milestone:** M004-ks4br0
**Written:** 2026-03-21

## UAT Type

- UAT mode: artifact-driven
- Why this mode is sufficient: S01 is a header-only C++ change with no runtime service. Correctness is verified by compilation against the real Nuke SDK (Docker build exit 0) plus grep contracts on the modified header. Visual jaggy-elimination proof is deferred to human-experience UAT noted below.

## Preconditions

- Working directory is `/home/latuser/git/DeepC/.gsd/worktrees/M004-ks4br0`
- Docker daemon is running and `docker-build.sh` can reach the Nuke 16.0 SDK image
- `src/DeepSampleOptimizer.h` has been modified by T01 (the file under test)

## Smoke Test

```bash
bash scripts/verify-s01-syntax.sh
```
Expected: prints `Syntax check passed.` and exits 0. If this fails, the header is broken and nothing else matters.

## Test Cases

### 1. colorDistance unpremultiplies before comparison

**Purpose:** Confirm the old 2-arg signature is gone and the new 4-arg signature is present.

1. Run:
   ```bash
   grep -n "colorDistance" src/DeepSampleOptimizer.h
   ```
2. **Expected:** All hits show the 4-arg signature `colorDistance(const std::vector<float>& a, float alphaA, const std::vector<float>& b, float alphaB)`. No line shows a 2-arg call or declaration.

3. Run:
   ```bash
   grep -c "alphaA" src/DeepSampleOptimizer.h
   ```
4. **Expected:** Count ≥ 2 (declaration + at least one use of `alphaA` inside the function body).

### 2. Near-zero alpha guard returns 0

**Purpose:** Confirm the guard that prevents NaN/inf on transparent samples is present in the source.

1. Run:
   ```bash
   grep "1e-6" src/DeepSampleOptimizer.h
   ```
2. **Expected:** At least one match inside `colorDistance` referencing the near-zero guard.

### 3. tidyOverlapping pre-pass is called exactly once in optimizeSamples

**Purpose:** Confirm the tidy pass is wired in and not duplicated.

1. Run:
   ```bash
   grep -c "tidyOverlapping(samples)" src/DeepSampleOptimizer.h
   ```
2. **Expected:** Exactly `1`.

3. Run:
   ```bash
   grep -n "tidyOverlapping" src/DeepSampleOptimizer.h
   ```
4. **Expected:** Three lines — the comment above the declaration, the `inline void tidyOverlapping(...)` definition, and the `tidyOverlapping(samples);` call site inside `optimizeSamples`.

### 4. Volumetric alpha subdivision formula is present

**Purpose:** Confirm the correct `pow(1-alpha, ratio)` formula is used for splitting, not a linear approximation.

1. Run:
   ```bash
   grep "pow" src/DeepSampleOptimizer.h
   ```
2. **Expected:** At least one match inside `tidyOverlapping` implementing `1.0f - std::pow(1.0f - alpha, ratio)` or equivalent.

### 5. Syntax check — DeepCBlur.cpp

1. Run:
   ```bash
   bash scripts/verify-s01-syntax.sh
   ```
2. **Expected:** Exit 0, output contains `Syntax check passed.`

### 6. Syntax check — DeepCBlur2.cpp

1. Run (with the mock include dir created by the verify script, or reuse its temp dir):
   ```bash
   TMPDIR=$(mktemp -d)
   # Copy mock headers used by verify-s01-syntax.sh into TMPDIR
   bash scripts/verify-s01-syntax.sh  # note TMPDIR path from output if printed
   # Then:
   g++ -std=c++17 -fsyntax-only -I"$TMPDIR" src/DeepCBlur2.cpp
   ```
   Alternatively, re-run the verify script which internally checks both files if updated.
2. **Expected:** Exit 0 with no errors.

### 7. Full Docker build

**Purpose:** Authoritative proof that the header compiles against the real Nuke 16.0 DDImage SDK.

1. Run:
   ```bash
   bash docker-build.sh --linux --versions 16.0
   ```
2. **Expected:** Exit 0. A release zip is produced under `release/`. No compiler errors or warnings about `colorDistance` or `tidyOverlapping`.

## Edge Cases

### Transparent sample handling (near-zero alpha)

1. In `src/DeepSampleOptimizer.h`, locate the `colorDistance` function.
2. Verify the guard reads: if either `alphaA < 1e-6f` or `alphaB < 1e-6f`, return `0.0f` immediately.
3. **Expected:** Both alphas are checked independently (or combined with `||`). Returning 0 means transparent samples always match and will be merged — this prevents divide-by-zero and NaN propagation.

### Point sample skip in tidyOverlapping split pass

1. Grep for the point sample guard:
   ```bash
   grep "zFront == zBack\|point sample" src/DeepSampleOptimizer.h
   ```
2. **Expected:** A guard that skips splitting when `zFront == zBack` (point sample), since there is no range to subdivide.

## Failure Signals

- `bash scripts/verify-s01-syntax.sh` exits non-zero → header has C++ syntax errors; check recent edits to `DeepSampleOptimizer.h`
- `docker-build.sh` exits non-zero → DDImage API mismatch or link error; check that new function signatures are compatible with types used in DeepCBlur.cpp and DeepCBlur2.cpp
- `grep -c "tidyOverlapping(samples)"` returns 0 → pre-pass not wired in; `optimizeSamples` will not tidy overlaps
- `grep -c "tidyOverlapping(samples)"` returns > 1 → duplicate call; potential double-processing of samples
- `grep` finds old 2-arg `colorDistance` signature → partial edit; the call site in `optimizeSamples` still passes premultiplied values

## Not Proven By This UAT

- **Visual jaggy elimination** — that DeepCBlur output on a hard-surface reference render is actually smooth. This requires a Nuke session with a real deep image and visual comparison before/after the fix. Not automatable in this pipeline.
- **Runtime performance** — `tidyOverlapping` convergence loop has no iteration cap; worst-case behavior on adversarial depth layouts is unverified at runtime.
- **DeepCBlur2 full Docker build** — the Docker build compiles all targets including DeepCBlur2; if it exits 0 that covers this, but if only one plugin is built the second is not verified.
- **Same-depth collapse at mergeTolerance=0** — the over-merge pass in `tidyOverlapping` collapses samples with identical [zFront, zBack], but this is verified by code inspection only, not by a running Nuke session observing sample count reduction.

## Notes for Tester

- The Docker build is the only authoritative compile check. The mock-header syntax script is fast but cannot catch DDImage API mismatches.
- Visual confirmation of jaggy elimination is the highest-value human check. Load a hard-surface deep image (e.g. a flat-color box with depth), apply DeepCBlur with default tolerance, flatten, and compare to a reference render from before this fix. Smooth gradients with no banding = pass.
- If the Docker image is unavailable, the syntax script plus grep contracts provide strong but incomplete coverage — note this explicitly when reporting results.
