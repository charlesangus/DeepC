# S01: Separable blur + kernel tiers — UAT

**Milestone:** M003
**Written:** 2026-03-21

## UAT Type

- UAT mode: artifact-driven
- Why this mode is sufficient: S01's proof level is contract verification only (compilation + structural correctness). No Nuke runtime is available in CI. Visual parity and runtime behaviour are deferred to S02 UAT. All verifiable claims are structural: kernel functions exist, two-pass structure is present, fast path is preserved, syntax compiles.

## Preconditions

- Working directory is the M003 worktree root (contains `src/DeepCBlur.cpp` and `scripts/verify-s01-syntax.sh`)
- `g++` with C++17 support is available on PATH
- No Nuke installation required for these tests

## Smoke Test

```bash
bash scripts/verify-s01-syntax.sh
```
**Expected:** Output includes "Syntax check passed." and exits 0. Any non-zero exit or compiler error indicates a broken build — stop here and fix before continuing.

## Test Cases

### 1. Kernel accuracy tier functions exist and are callable

```bash
grep -c "getLQGaussianKernel\|getMQGaussianKernel\|getHQGaussianKernel" src/DeepCBlur.cpp
```
**Expected:** Returns 7 (or ≥ 3 — each function name appears at least once in a definition).

```bash
grep -q "computeKernel" src/DeepCBlur.cpp && echo "PASS" || echo "FAIL"
```
**Expected:** `PASS` — dispatcher function exists.

### 2. Kernel quality enum knob is wired into the blur engine

```bash
grep -c "Enumeration_knob" src/DeepCBlur.cpp
```
**Expected:** Returns ≥ 1 (the kernel_quality knob is declared in `knobs()`).

```bash
grep -q "computeKernel.*_kernelQuality" src/DeepCBlur.cpp && echo "PASS" || echo "FAIL"
```
**Expected:** `PASS` — confirms the enum knob value is actually passed into the kernel dispatcher inside `doDeepEngine`, not just declared. If this fails, the tier selection is cosmetic only.

### 3. Two-pass intermediate buffer structure is present

```bash
grep -q "intermediateBuffer" src/DeepCBlur.cpp && echo "PASS" || echo "FAIL"
```
**Expected:** `PASS` — intermediate buffer variable exists (H→V intermediate store).

```bash
grep -c "HORIZONTAL PASS\|VERTICAL PASS" src/DeepCBlur.cpp
```
**Expected:** Returns 2 — one marker per pass, confirming the two-pass structure is explicit and not collapsed into a single loop.

### 4. Zero-blur fast path is preserved

```bash
grep -q "radX == 0 && radY == 0" src/DeepCBlur.cpp && echo "PASS" || echo "FAIL"
```
**Expected:** `PASS` — the zero-blur passthrough guard still exists at the top of `doDeepEngine`. This prevents any allocation or computation when blur size is zero.

### 5. Old 2D kernel code is removed (regression check)

```bash
grep -q "ScratchBuf::kernel" src/DeepCBlur.cpp && echo "FAIL: old 2D kernel member found" || echo "PASS: old code removed"
```
**Expected:** `PASS: old code removed` — the 2D kernel path is fully replaced, not just conditionally bypassed.

### 6. Sigma convention is backward-compatible

```bash
grep -q "blur / 3.0\|blur/3.0" src/DeepCBlur.cpp && echo "PASS" || echo "FAIL"
```
**Expected:** `PASS` — sigma = blur / 3.0 convention from M002 is preserved in the kernel dispatcher (not CMG99's 0.42466 factor).

### 7. Full syntax compilation with mock headers

```bash
bash scripts/verify-s01-syntax.sh && echo "SYNTAX OK"
```
**Expected:** `Syntax check passed.` followed by `SYNTAX OK`. This is the authoritative compilation proof for this slice — it catches any type errors, missing includes, or malformed expressions that grep checks would miss.

## Edge Cases

### Half-kernel convention: center weight at index 0

```bash
grep -A5 "getLQGaussianKernel" src/DeepCBlur.cpp | head -20
```
**Expected:** The function builds a vector where index 0 is the center weight and subsequent indices extend outward. The kernel loop in both H and V passes should apply `kernel[0]` to the source pixel and `kernel[k]` symmetrically at `±k` offsets.

### Verify-script mock headers are complete

```bash
bash scripts/verify-s01-syntax.sh 2>&1 | grep "error:" | wc -l
```
**Expected:** 0 — no compilation errors. If mock headers are missing stubs for types used in the file, this will report non-zero.

## Failure Signals

- `bash scripts/verify-s01-syntax.sh` exits non-zero: syntax error in DeepCBlur.cpp — the specific error line will be printed. Fix the C++ error before proceeding to S02.
- `grep -q "computeKernel.*_kernelQuality"` fails: kernel tier selection is declared but not wired into doDeepEngine — blur quality knob has no effect at runtime.
- `grep -q "ScratchBuf::kernel"` succeeds: old 2D kernel code was not fully removed — double-counting risk between old and new paths.
- `grep -q "radX == 0 && radY == 0"` fails: zero-blur fast path was accidentally deleted during refactor — performance regression for zero-blur case.
- `grep -c "HORIZONTAL PASS\|VERTICAL PASS"` returns < 2: one of the two passes is missing or was accidentally merged — separable structure is broken.

## Not Proven By This UAT

- **Visual output correctness** — that separable H→V blur produces visually equivalent results to the old 2D kernel at matching radii. Deferred to S02 UAT (requires Nuke runtime).
- **Runtime performance** — that the separable approach is actually ~10× faster at radius 20 than the old O(r²) loop. Deferred to S02/milestone success criteria verification.
- **Docker build** — that `docker-build.sh` produces a loadable `DeepCBlur.so`. Deferred to S02 (R008).
- **Kernel tier visual differences** — that LQ/MQ/HQ produce visually distinct output. Requires Nuke; deferred to S02.
- **WH_knob and alpha correction** — R003, R004, R005, R006 are all S02 deliverables.

## Notes for Tester

- The syntax check script (`verify-s01-syntax.sh`) creates its mock headers in a temp directory and cleans up after itself. Run it from the repo root.
- The grep check for `computeKernel.*_kernelQuality` matches both the H and V pass calls in doDeepEngine — this is correct and expected (both axes use the same quality tier).
- "Enumeration_knob" appearing 2 times is correct: one for the knob registration (without `_kernelQuality` nearby) and one with the quality variable. Don't be alarmed if the count is 2 rather than 1.
- The intermediate buffer type is `vector<vector<vector<SampleRecord>>>` — three levels of nesting: [rowIndex][colIndex][sampleIndex]. This is intentional to handle variable sample counts per deep pixel.
