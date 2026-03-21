---
estimated_steps: 5
estimated_files: 1
---

# T01: Implement DeepSampleOptimizer.h shared header

**Slice:** S01 — DeepCBlur core implementation
**Milestone:** M002

## Description

Create a standalone, header-only C++ utility (`src/DeepSampleOptimizer.h`) that provides reusable per-pixel deep sample optimization. This header extracts and generalizes the merge + cap logic from DeepThinner.cpp (Pass 6 "Smart Merge" and Pass 7 "Max Samples") into a function callable by DeepCBlur and future deep spatial ops. The header must have zero DDImage dependencies so it can be unit-tested independently.

The core function `optimizeSamples()` takes a vector of sample records (each with depth front/back, alpha, and a variable-length channel data array), a merge tolerance, color tolerance, and max_samples cap. It returns an optimized vector with nearby-depth samples merged via front-to-back over-compositing and the total count capped.

**Reference file:** Read `src/DeepThinner.cpp` lines 420–700 for the exact merge and cap logic to extract. Key patterns:
- `SampleRecord` struct (line ~30): depth front/back, alpha, rgb
- Pass 6 (line ~610): group formation by Z-proximity and color-proximity, then front-to-back over-composite merge within each group
- Pass 7 (line ~635): truncate groups to max_samples count
- Emit loop (line ~640–690): merged channel computation with `foreach(z, channels)` iteration

**Critical constraint:** This header is DDImage-independent. It works with its own `SampleRecord` struct containing a `std::vector<float>` for arbitrary channel data (not just rgb). The caller (DeepCBlur) is responsible for populating SampleRecords from DeepPixel data and writing results back to DeepOutPixel.

## Steps

1. **Define the `SampleRecord` struct** in namespace `deepc`:
   - `float zFront, zBack, alpha`
   - `std::vector<float> channels` — arbitrary channel values (caller decides channel order)
   - This generalizes DeepThinner's fixed rgb[3] to support any channel set

2. **Implement `inline void optimizeSamples()`** with parameters:
   - `std::vector<SampleRecord>& samples` (in/out — modified in place)
   - `float mergeTolerance` — Z-distance threshold for grouping
   - `float colorTolerance` — max channel-value distance for grouping (use first 3 channels as "color" if available, 0.0 to disable color check)
   - `int maxSamples` — hard cap (0 = unlimited)
   
3. **Inside optimizeSamples:**
   - Sort samples by zFront ascending
   - Mark all alive initially
   - **Merge pass:** Walk sorted samples, form groups of consecutive alive samples where `zFront[s] - zFront[groupStart] <= mergeTolerance` AND color distance <= colorTolerance. For each multi-sample group, merge via front-to-back over-compositing: accumulate `alpha_acc += alpha * (1 - alpha_acc)`, accumulate channels weighted by `(1 - alpha_acc_before)`, take min(zFront) and max(zBack). Replace group with single merged sample.
   - **Cap pass:** If resulting sample count > maxSamples and maxSamples > 0, keep only the first maxSamples (frontmost after sort).
   - Compact the vector to remove dead entries.

4. **Add a `colorDistance()` helper** (inline, same as DeepThinner's): max of absolute differences across the first min(3, channels.size()) channel values.

5. **Add header guards, include guards, and file header comment** with copyright matching existing DeepC style.

## Must-Haves

- [ ] `SampleRecord` struct with zFront, zBack, alpha, and variable-length channels vector
- [ ] `optimizeSamples()` function implementing merge + cap
- [ ] Merge uses front-to-back over-compositing (not simple averaging)
- [ ] Header-only — all functions `inline`, no .cpp file needed
- [ ] Zero DDImage dependencies — only standard library headers
- [ ] C++17 compatible, compiles with GCC 11.2.1

## Verification

- `test -f src/DeepSampleOptimizer.h` — file exists
- `grep -q "struct SampleRecord" src/DeepSampleOptimizer.h` — struct defined
- `grep -q "optimizeSamples" src/DeepSampleOptimizer.h` — function defined
- `grep -q "inline" src/DeepSampleOptimizer.h` — functions are inline for header-only use
- `grep -cP "#include\s+<" src/DeepSampleOptimizer.h` — only standard library includes (no DDImage)

## Inputs

- `src/DeepThinner.cpp` — reference implementation for SampleRecord, Pass 6 merge logic, Pass 7 max-samples cap, colorDistance helper

## Expected Output

- `src/DeepSampleOptimizer.h` — new standalone header-only file with optimizeSamples() function

## Observability Impact

- **New signal:** DeepSampleOptimizer.h is compilable standalone — a trivial test harness can verify merge/cap correctness without Nuke. This is the primary inspection surface for this task.
- **Failure visibility:** If the header has DDImage dependencies, downstream standalone compilation will fail with clear "file not found" errors for DDImage headers. The slice verification check `! grep -q "DDImage" src/DeepSampleOptimizer.h` catches this statically.
- **Inspection:** `grep -c "inline" src/DeepSampleOptimizer.h` shows how many functions are marked inline (must be all of them for header-only ODR safety).
