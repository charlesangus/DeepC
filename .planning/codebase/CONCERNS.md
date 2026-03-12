# Codebase Concerns

**Analysis Date:** 2026-03-12

## Tech Debt

**DeepCBlink is an unfinished proof of concept:**
- Issue: The entire node is explicitly marked as a proof-of-concept with 7 unresolved TODO comments. The GPU code path is hardcoded to always use CPU (`Blink::ComputeDevice::CurrentCPUDevice()` on line 248), ignoring the `_useGPUIfAvailable` flag the user can set. The node bails out silently (`return false`) if the channel set is `all()` or has more than 4 channels.
- Files: `src/DeepCBlink.cpp`
- Impact: The node misleads users with a "Use GPU if available" knob that never activates GPU. Early returns silently produce no output with no error message.
- Fix approach: Either complete the implementation (proper GPU path, handle all channel counts) or remove the node from the shipping build. At minimum, disable the GPU knob or raise an error on unsupported channel count.

**Grade coefficient duplication between DeepCGrade and DeepCPNoise:**
- Issue: The grading coefficient logic (`A[]`, `B[]`, `G[]` arrays, `_validate` precompute block, `wrappedPerChannel` reverse/forward paths) is copy-pasted verbatim between `src/DeepCGrade.cpp` and `src/DeepCPNoise.cpp` with no shared base class or utility function.
- Files: `src/DeepCGrade.cpp`, `src/DeepCPNoise.cpp`
- Impact: Any bug fix or enhancement to the grade math must be applied twice. The two implementations can diverge silently.
- Fix approach: Extract grade math into a shared utility struct or mixin class within the `DeepCWrapper` hierarchy.

**DeepCWrapper base class is a shipping plugin:**
- Issue: `DeepCWrapper` and `DeepCMWrapper` register themselves as usable Nuke nodes (`DeepCWrapper::d` and `DeepCMWrapper::d` descriptors at the bottom of `src/DeepCWrapper.cpp` and `src/DeepCMWrapper.cpp`). `node_help()` in `DeepCWrapper` says "Should never be used directly."
- Files: `src/DeepCWrapper.cpp`, `src/DeepCMWrapper.cpp`
- Impact: Users can instantiate these partial base class nodes, which behave unexpectedly (applying a raw gain of 1.0, no real operation).
- Fix approach: Remove the `Op::Description` descriptors and `build()` functions from both wrapper base classes so they do not appear in Nuke's node menu.

**`perSampleData` data-flow design is fragile:**
- Issue: The `wrappedPerSample` / `wrappedPerChannel` virtual split passes a single `float` (`perSampleData`) from the per-sample phase to the per-channel phase. The TODO in `src/DeepCWrapper.cpp:78`, `src/DeepCMWrapper.cpp:23`, and `src/DeepCPMatte.cpp:63` notes this should be "a pointer and length" to support arrays, but it has not been changed.
- Files: `src/DeepCWrapper.h`, `src/DeepCWrapper.cpp`, `src/DeepCMWrapper.cpp`, `src/DeepCPMatte.cpp`, `src/DeepCPNoise.cpp`
- Impact: Any future node that needs to pass more than one value per sample from the sample phase to the channel phase (e.g. a full color triplet) must work around this constraint, as `DeepCHueShift` already does by repurposing the `Vector3& sampleColor` parameter instead.
- Fix approach: Redesign the per-sample data interface to pass a small array or struct before the design fossilizes further.

---

## Known Bugs

**DeepCGrade reverse-mode loses gamma application:**
- Symptoms: When `_reverse` is true, the gamma step (`outData = pow(inputVal, G[cIndex])`) writes to `outData` but the very next line overwrites `outData` with the linear ramp, discarding the gamma result entirely.
- Files: `src/DeepCGrade.cpp` lines 110-113
- Trigger: Enable the "reverse" checkbox on a DeepCGrade node with a non-1.0 gamma value.
- Workaround: None; gamma is silently ignored in reverse mode.

**DeepCKeymix uses B-side channels for both A and B containment checks:**
- Symptoms: Both `aInPixelChannels` and `bInPixelChannels` are assigned from `bPixel.channels()`. The A-side containment check (`aInPixelChannels.contains(z)`) therefore queries the B pixel's channel set, not A's.
- Files: `src/DeepCKeymix.cpp` lines 265-266
- Trigger: Keymixing deep streams where A and B have different channel sets with partial mixing values.
- Workaround: None; incorrect containment check can produce wrong output for mixed partial-blend pixels.

**DeepCConstant weight calculation uses C++ comma operator:**
- Symptoms: `float weight = (1.0f, 0.0f, depth / _overallDepth);` is a comma expression. It evaluates all three sub-expressions but assigns only the last one (`depth / _overallDepth`). The intended call was likely `lerp` or a vector constructor.
- Files: `src/DeepCConstant.cpp` line 162
- Trigger: Any use of DeepCConstant with more than 1 sample causes front/back color interpolation to silently malfunction — `weight` is correct numerically by accident (equals `depth / _overallDepth`) but only because the last operand happens to be the desired value.
- Workaround: Output appears partially correct because the comma expression coincidentally yields the right scalar; actual lerp between `_values_front` and `_values_back` on line 178 does work correctly with that scalar.

**DeepCSaturation and DeepCHueShift divide by alpha without a zero guard:**
- Symptoms: When unpremultiplying, both nodes execute `/ alpha` directly with no check for alpha == 0, producing NaN or inf values for transparent samples.
- Files: `src/DeepCSaturation.cpp` line 112, `src/DeepCHueShift.cpp` line 106
- Trigger: Any deep pixel sample with alpha == 0 and unpremult enabled (which is the default).
- Workaround: `DeepCWrapper::doDeepEngine` does guard this for the normal per-channel unpremult path (line 260-263 of `src/DeepCWrapper.cpp`), but the `wrappedPerSample` implementations in these subclasses bypass that guard.

**DeepCID iterates `_auxiliaryChannelSet` but reads `_auxChannel` inside the loop:**
- Symptoms: The `foreach` loop over `_auxiliaryChannelSet` in `wrappedPerSample` ignores the loop variable `z` entirely and always reads `_auxChannel` directly. The loop variable is unused.
- Files: `src/DeepCID.cpp` lines 81-97
- Trigger: Any use of DeepCID; currently harmless because `_auxiliaryChannelSet` contains exactly one channel (`_auxChannel`), but the loop abstraction is misleading and fragile if that ever changes.
- Workaround: Functionally works because the channel set always has one member.

---

## Performance Bottlenecks

**DeepCBlink allocates two raw heap buffers per engine call:**
- Problem: `calloc(totalSamples * 4, sizeof(float))` is called twice every time `doDeepEngine` runs (`imageBuffer` and `outImageBuffer`). These are C-style heap allocations, not freed on early-exit code paths (e.g., `return false` on `Op::aborted()` at line 299).
- Files: `src/DeepCBlink.cpp` lines 161-162, 299
- Cause: No RAII wrapper (e.g. `std::vector`) used; raw `calloc` with no matching `free` on the early-return path.
- Improvement path: Replace with `std::vector<float>` to guarantee cleanup, and consider whether the whole Blink approach (flattening a deep plane into a contiguous buffer, copying to/from Blink) is appropriate given Deep's per-scanline processing model.

**`DeepCWorld::processSample` recomputes `window_matrix.inverse()` per sample:**
- Problem: `Matrix4 inverse_window = window_matrix.inverse()` is called once per sample inside `processSample`, a `const` method. The window matrix does not change per sample — it is set in `_validate`.
- Files: `src/DeepCWorld.cpp` line 221
- Cause: The inverse is not cached alongside `window_matrix` during `_validate`.
- Improvement path: Store `inverse_window_matrix` as a class member, compute it once in `_validate`.

---

## Fragile Areas

**Switch statements without default return in `DeepCWrapper` and `DeepCWorld`:**
- Files: `src/DeepCWrapper.cpp` lines 359-368 (`test_input`), `src/DeepCWorld.cpp` lines 110-118 (`default_input`), lines 121-126 (`input_label`)
- Why fragile: All three functions have `switch` statements with no `default` case and no return after the switch body. For inputs beyond the handled cases, the function falls off the end and returns undefined behavior (UB in C++). The compiler may warn but will not always error.
- Safe modification: Any future addition of a third input to these nodes must update all three switch statements, which is easy to miss.
- Test coverage: None.

**`DeepCPNoise` noise type selection is order-dependent magic index:**
- Files: `src/DeepCPNoise.cpp` lines 270-278
- Why fragile: The code `if (_noiseType==0)` hard-codes the magic number 0 to select Simplex (the only 4D noise type). A TODO comment explicitly flags this. If the `noiseTypeNames[]` / `noiseTypes[]` arrays are reordered (e.g., to sort alphabetically), Simplex loses its 4D evolution parameter silently.
- Safe modification: Use a named constant or compare against `FastNoise::SimplexFractal` via the `noiseTypes[]` mapping rather than the raw enum index.
- Test coverage: None.

**`batchInstall.sh` misidentifies platforms in comments:**
- Files: `batchInstall.sh` lines 3-4
- Why fragile: The Linux branch comment reads "Do something under Mac OS X platform" — a copy-paste error. This does not affect functionality but will mislead any contributor maintaining the script.

**`DeepCWrapper.cpp` has multiple commented-out `++inData; ++outData;` pointer increments:**
- Files: `src/DeepCWrapper.cpp` lines 253-254, 273-274
- Why fragile: These are leftovers from a previous raw-pointer iteration approach. They sit next to active channel-iteration code and may confuse future contributors into thinking the pointer-stepping logic is still needed or intentionally disabled.

---

## Missing Critical Features

**No automated tests of any kind:**
- Problem: There are no unit tests, integration tests, or automated render comparison tests anywhere in the repository. All verification is manual.
- Blocks: Safe refactoring, catching regressions across new Nuke versions, verifying bug fixes (e.g., the DeepCGrade reverse-gamma bug).
- Files: Entire `src/` directory.

**No macOS build support:**
- Problem: The top-level `CMakeLists.txt` applies compile flags only under `if (UNIX)`, and `.dylib` suffix is handled for Apple in `src/CMakeLists.txt`, but there is no macOS-specific NDK path handling or CI. Nuke supports macOS; whether plugins build there is untested.
- Files: `CMakeLists.txt`, `src/CMakeLists.txt`

**`DeepCBlink` GPU path is never exercised:**
- Problem: The GPU device is queried and displayed in the UI, but the processing path is hardcoded to CPU. This feature is documented nowhere as intentionally disabled.
- Files: `src/DeepCBlink.cpp` lines 244-248

---

## Test Coverage Gaps

**All nodes - functional correctness:**
- What's not tested: Pixel output correctness for any node (grade math, saturation, hue shift, keymix blending, world position calculation, noise generation).
- Files: `src/DeepCGrade.cpp`, `src/DeepCSaturation.cpp`, `src/DeepCHueShift.cpp`, `src/DeepCKeymix.cpp`, `src/DeepCWorld.cpp`, `src/DeepCPNoise.cpp`
- Risk: Regressions in math or channel handling go undetected until a user reports them.
- Priority: High

**DeepCGrade - reverse mode:**
- What's not tested: The reverse-mode gamma bug described above would be caught by a simple numerical test.
- Files: `src/DeepCGrade.cpp`
- Risk: Known bug persists undetected.
- Priority: High

**DeepCKeymix - mixed channel sets:**
- What's not tested: A and B inputs with differing channel compositions under partial mix values.
- Files: `src/DeepCKeymix.cpp`
- Risk: The `aInPixelChannels`/`bInPixelChannels` copy-paste bug goes undetected.
- Priority: High

**DeepCWrapper - zero-alpha unpremult:**
- What's not tested: Transparent sample handling (alpha == 0) through nodes that divide in `wrappedPerSample` without a guard.
- Files: `src/DeepCSaturation.cpp`, `src/DeepCHueShift.cpp`
- Risk: NaN/inf propagation into downstream nodes.
- Priority: High

---

*Concerns audit: 2026-03-12*
