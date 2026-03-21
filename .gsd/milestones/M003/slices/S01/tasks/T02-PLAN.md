---
estimated_steps: 5
estimated_files: 3
---

# T02: Replace 2D blur with separable H→V passes

**Slice:** S01 — Separable blur + kernel tiers
**Milestone:** M003

## Description

Replace the O(r²) 2D Gaussian blur in `doDeepEngine` with two sequential 1D passes (horizontal → intermediate buffer → vertical) using the kernel functions added in T01. Create a syntax verification script with mock DDImage headers. This is the core structural change for R001 (separable blur) and preserves R007 (zero-blur fast path).

**Relevant skills:** None needed — this is C++ refactoring.

## Steps

1. **Create the syntax verification script** `scripts/verify-s01-syntax.sh`. This script creates a temporary directory with mock DDImage headers (minimal stubs for `DeepFilterOp.h`, `DeepPixel.h`, `DeepPlane.h`, `Knobs.h` with the types/macros used in DeepCBlur.cpp), then runs `g++ -std=c++17 -fsyntax-only -I<tmpdir> src/DeepCBlur.cpp`. The mock headers need to define at minimum:
   - `namespace DD::Image { class DeepFilterOp : public Op { ... }; }` with virtual methods
   - `class DeepPixel`, `class DeepPlane`, `class DeepOutputPlane`, `class DeepOutPixel` with the methods called in the code (`getPixel`, `getSampleCount`, `getUnorderedSample`, `addPixel`, `addHole`, `box()`, etc.)
   - `Box`, `Box::iterator`, `ChannelSet`, `Channel` enums (`Chan_DeepFront`, `Chan_DeepBack`, `Chan_Alpha`), `Knob_Callback`, `Float_knob`, `Int_knob`, `Enumeration_knob`, `SetRange`, `Tooltip`, `RequestData`
   - `Op` base class with `aborted()`, `Description`, `Node*`
   - `foreach(z, channels)` macro — this is DDImage's channel iterator macro: `for (Channel z = channels.first(); z; z = channels.next(z))`
   
   The goal is to verify the code compiles syntactically — not to provide working implementations. All method bodies can be stubs returning default values. Full docker build is deferred to S02.

2. **Refactor doDeepEngine: remove the 2D kernel code.** Delete the block that pre-computes the 2D kernel (sigmaX/sigmaY/kernelW/kernelH, the nested `dx/dy` kernel computation loop, `kernelSum` normalization). Delete `ScratchBuf::kernel`. Keep `ScratchBuf::samples` and `ScratchBuf::outPixel` (still needed for V pass output).

3. **Implement the horizontal pass.** After fetching `inPlane`, compute the H half-kernel: `auto kernelH = computeKernel(_blurWidth, _kernelQuality);` The H pass iterates each row in the padded input Y range (`inputBox.y()` to `inputBox.t()`) and each column in the output X range (`box.x()` to `box.r()`). For each output pixel `(x, y)`, gather from `x - radX` to `x + radX`, weighting samples by `kernelH[abs(dx)]`. Store gathered samples in `intermediateBuffer`:
   ```cpp
   // Intermediate buffer: [relY][relX] → vector<SampleRecord>
   // relY covers inputBox.y()..inputBox.t(), relX covers box.x()..box.r()
   const int intW = box.r() - box.x();
   const int intH = inputBox.t() - inputBox.y();
   std::vector<std::vector<std::vector<deepc::SampleRecord>>> intermediateBuffer(
       intH, std::vector<std::vector<deepc::SampleRecord>>(intW));
   ```
   Use helper lambdas for coordinate translation:
   ```cpp
   auto intX = [&](int worldX) { return worldX - box.x(); };
   auto intY = [&](int worldY) { return worldY - inputBox.y(); };
   ```
   In the H pass, do NOT call `optimizeSamples` — raw accumulated samples are stored. Weight alpha and colour channels by kernel weight; propagate depth channels raw. This matches the existing weighting pattern.

4. **Implement the vertical pass.** Iterate output pixels in `box`. For each `(outX, outY)`, gather from the intermediate buffer at positions `(intX(outX), intY(outY + dy))` for `dy = -radV..+radV`, weighting by `kernelV[abs(dy)]`. Since the intermediate samples are already H-weighted, the V pass multiplies them again by V kernel weight — this is correct for separable convolution (total weight = H_weight × V_weight). After gathering all V-direction samples into `scratch.samples`, call `optimizeSamples`, then emit to `plane` using the existing output loop (unchanged). If `scratch.samples` is empty, call `plane.addHole()`.

5. **Preserve zero-blur fast path.** The existing `if (radX == 0 && radY == 0)` block at the top of `doDeepEngine` must remain exactly as-is. Do not modify it.

## Must-Haves

- [ ] `doDeepEngine` has two sequential 1D gather loops (H then V) with an intermediate `vector<vector<vector<SampleRecord>>>` buffer between them
- [ ] H pass uses `computeKernel(_blurWidth, _kernelQuality)` for the 1D kernel
- [ ] V pass uses `computeKernel(_blurHeight, _kernelQuality)` for the 1D kernel
- [ ] `optimizeSamples` is called only after V pass, not between passes
- [ ] Zero-blur fast path (`radX == 0 && radY == 0`) is preserved unchanged
- [ ] Old 2D kernel computation and `ScratchBuf::kernel` are removed
- [ ] `scripts/verify-s01-syntax.sh` exists and passes
- [ ] H pass weights alpha and colour channels by kernel weight, propagates depth raw (same pattern as existing code)
- [ ] Intermediate buffer dimensions: width = output box width, height = input box height (padded Y range)

## Verification

- `bash scripts/verify-s01-syntax.sh` exits 0
- `grep -q "intermediateBuffer\|intermediate" src/DeepCBlur.cpp` exits 0
- `grep -q "computeKernel" src/DeepCBlur.cpp` exits 0 (dispatcher is called from doDeepEngine)
- `grep -q "radX == 0 && radY == 0" src/DeepCBlur.cpp` exits 0 (fast path preserved)
- `grep -v "kernelW\|kernelH\|kernelSum" src/DeepCBlur.cpp | wc -l` confirms old 2D kernel variables are removed

## Inputs

- `src/DeepCBlur.cpp` — with kernel functions and enum knob from T01
- `src/DeepSampleOptimizer.h` — SampleRecord struct and optimizeSamples (unchanged, consumed)

## Expected Output

- `src/DeepCBlur.cpp` — doDeepEngine refactored to separable H→V passes with intermediate buffer, old 2D kernel removed
- `scripts/verify-s01-syntax.sh` — syntax verification script with mock DDImage headers
