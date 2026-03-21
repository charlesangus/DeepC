# S01: Separable blur + kernel tiers

**Goal:** DeepCBlur uses two-pass separable Gaussian blur with three kernel accuracy tiers selectable via enum knob. Compiles successfully.
**Demo:** `src/DeepCBlur.cpp` contains a `doDeepEngine` with sequential H→V 1D gather loops, an intermediate per-pixel sample buffer, three kernel generation functions (LQ/MQ/HQ), and an `Enumeration_knob` for kernel quality selection.

## Must-Haves

- Separable 2D Gaussian blur: horizontal pass → intermediate buffer → vertical pass (R001)
- Three kernel accuracy tiers: Low (raw unnormalized), Medium (normalized, default), High (CDF-based) as `Enumeration_knob` (R002)
- Zero-blur fast path preserved unchanged (R007)
- Half-kernel (symmetric) storage for all three tiers
- Intermediate buffer stores `std::vector<SampleRecord>` per pixel to handle variable-length deep samples
- `optimizeSamples` runs only after V pass (not between passes)
- Sigma convention: `blur / 3.0` (backward compatible with M002)
- Code compiles: `g++ -std=c++17 -fsyntax-only` passes (full docker build deferred to S02/R008)

## Proof Level

- This slice proves: contract (compilation + structural correctness)
- Real runtime required: no (visual UAT deferred to S02)
- Human/UAT required: no

## Verification

- `grep -c "Enumeration_knob" src/DeepCBlur.cpp` returns >= 1
- `grep -c "getLQGaussianKernel\|getMQGaussianKernel\|getHQGaussianKernel" src/DeepCBlur.cpp` returns >= 3 (function definitions)
- `grep -q "intermediateBuffer\|intermediate" src/DeepCBlur.cpp` confirms two-pass structure
- `grep -q "radX == 0 && radY == 0" src/DeepCBlur.cpp` confirms zero-blur fast path preserved
- `bash scripts/verify-s01-syntax.sh` exits 0 (syntax check with mock DDImage headers)
- `grep -q "computeKernel.*_kernelQuality" src/DeepCBlur.cpp` confirms kernel quality knob is wired into blur engine (diagnostic: if this fails, kernel tier selection has no effect)

## Integration Closure

- Upstream surfaces consumed: `src/DeepSampleOptimizer.h` (SampleRecord, optimizeSamples — unchanged)
- New wiring introduced in this slice: `_kernelQuality` enum knob + kernel dispatch in doDeepEngine
- What remains before the milestone is truly usable end-to-end: S02 adds alpha correction, WH_knob, twirldown UI, and full docker build verification

## Observability / Diagnostics

- **Kernel quality inspection:** The `_kernelQuality` knob value is visible in Nuke's node panel and saved/restored in scripts. A script can query it via `nuke.toNode("DeepCBlur1")["kernel_quality"].value()`.
- **Blur parameter logging:** Sigma and radius are derived deterministically from `_blurWidth`/`_blurHeight` divided by 3.0 — inspectable by reading those knob values.
- **Failure visibility:** If blur produces unexpected results, the kernel quality tier is the first diagnostic variable to check. Zero-blur fast path logs no diagnostic (transparent passthrough).
- **Redaction:** No secrets or user data involved — all values are numeric rendering parameters.

## Tasks

- [x] **T01: Add kernel tier functions and enum knob** `est:45m`
  - Why: Establishes the three kernel accuracy tiers (R002) as independent free functions and wires the Enumeration_knob into the UI. This is risk-free work that T02 builds on.
  - Files: `src/DeepCBlur.cpp`
  - Do: Port `getLQGaussianKernel`, `getMQGaussianKernel`, `getHQGaussianKernel` from CMG99 (commit 9551704) as static free functions returning `std::vector<float>` half-kernels. Add `_kernelQuality` int member (default 1 = Medium). Add `Enumeration_knob` in `knobs()`. Add `computeKernel()` dispatcher. Use sigma = blur / 3.0 (not CMG99's 0.42466). Do NOT change `doDeepEngine` yet — kernel functions are added but not wired into the blur loop.
  - Verify: `grep -c "getLQGaussianKernel\|getMQGaussianKernel\|getHQGaussianKernel" src/DeepCBlur.cpp` returns >= 3
  - Done when: Three kernel functions + enum knob + dispatcher exist in DeepCBlur.cpp; file compiles with no syntax errors in isolation

- [x] **T02: Replace 2D blur with separable H→V passes** `est:1h30m`
  - Why: Core separable blur engine (R001). Replaces the O(r²) 2D kernel loop with two sequential 1D passes using an intermediate per-pixel sample buffer. Preserves the zero-blur fast path (R007). Wires the kernel tier dispatcher from T01 into the blur passes.
  - Files: `src/DeepCBlur.cpp`, `scripts/verify-s01-syntax.sh`
  - Do: In `doDeepEngine`, replace the single 2D kernel computation and nested gather loop with: (1) compute 1D H and V half-kernels via `computeKernel()`; (2) H pass iterating rows, gathering from 1D H neighbourhood, storing in `intermediate[y][x]` as `vector<SampleRecord>`; (3) V pass iterating output pixels, gathering from 1D V neighbourhood in intermediate, calling `optimizeSamples`, emitting to plane. Create `scripts/verify-s01-syntax.sh` with mock DDImage headers for syntax-only compilation check. Preserve zero-blur fast path. Remove the old `ScratchBuf::kernel` and 2D kernel code. Use helper lambda for intermediate buffer indexing to avoid off-by-one errors.
  - Verify: `bash scripts/verify-s01-syntax.sh` exits 0
  - Done when: `doDeepEngine` has two sequential 1D gather loops with intermediate buffer; zero-blur fast path preserved; syntax check passes

## Files Likely Touched

- `src/DeepCBlur.cpp`
- `scripts/verify-s01-syntax.sh`
