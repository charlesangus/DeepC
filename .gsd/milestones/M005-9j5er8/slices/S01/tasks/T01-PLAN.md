---
estimated_steps: 4
estimated_files: 1
skills_used: []
---

# T01: Apply multiplicative alpha decomposition, zero-alpha guards, and input label rename

**Slice:** S01 — Correct alpha decomposition + input rename
**Milestone:** M005-9j5er8

## Description

Fix the core alpha decomposition in `DeepCDepthBlur::doDeepEngine` to use multiplicative splitting instead of additive scaling, add zero-alpha guards for both input and sub-samples, and rename input labels per Nuke conventions.

The current code at line 445 does `px.getUnorderedSample(s, z) * w` for all non-Z channels including alpha. This treats alpha additively which breaks the flatten invariant (`flatten(output) ≠ flatten(input)`) for any α < 1. The fix uses `α_sub = 1 - pow(1-α, w)` which preserves the invariant by construction: `1 - ∏(1-α_sub_i) = 1 - (1-α)^(Σw_i) = α` since weights sum to 1.

## Steps

1. **Rename input labels** (~line 184-185): Change `case 0: return "Source";` → `case 0: return "";` and `case 1: return "B";` → `case 1: return "ref";`.

2. **Add zero-alpha input guard** — In the spreading section, after the `shouldSpread` check succeeds but before the `for (int i = 0; i < N; ++i)` sub-sample loop (~line 430), add:
   ```cpp
   const float srcAlpha = px.getUnorderedSample(s, Chan_Alpha);
   if (srcAlpha < 1e-6f) {
       SampleData sd;
       sd.zFront = srcFront;
       sd.zBack  = srcBack;
       sd.chanValues.reserve(nChans);
       foreach(z, channels) {
           sd.chanValues.push_back(px.getUnorderedSample(s, z));
       }
       outSamples.push_back(std::move(sd));
       continue;  // skip spreading for zero-alpha input
   }
   ```

3. **Replace additive scaling with multiplicative decomposition** — Inside the `for (int i = 0; i < N; ++i)` loop, replace the `foreach(z, channels)` block (currently ~line 437-446) with:
   ```cpp
   const float alphaSub = static_cast<float>(1.0 - std::pow(1.0 - static_cast<double>(srcAlpha), static_cast<double>(w)));
   if (alphaSub < 1e-6f)
       continue;  // skip near-zero sub-samples

   foreach(z, channels) {
       if (z == Chan_DeepFront) {
           sd.chanValues.push_back(sd.zFront);
       } else if (z == Chan_DeepBack) {
           sd.chanValues.push_back(sd.zBack);
       } else if (z == Chan_Alpha) {
           sd.chanValues.push_back(alphaSub);
       } else {
           sd.chanValues.push_back(px.getUnorderedSample(s, z) * (alphaSub / srcAlpha));
       }
   }
   ```
   Note: `alphaSub / srcAlpha` is safe because `srcAlpha >= 1e-6f` is guaranteed by the guard in step 2. The `static_cast<double>` ensures precision for the `pow` computation. Move the `sd.chanValues.reserve(nChans)` to before the `alphaSub` check so it's only done for non-skipped samples (or leave it after — minor).

4. **Run syntax check**: `scripts/verify-s01-syntax.sh` must exit 0. Run source grep contracts: `grep -q "pow(1" src/DeepCDepthBlur.cpp`, `grep -c '"B"' src/DeepCDepthBlur.cpp` returns 0, `grep -q '"ref"' src/DeepCDepthBlur.cpp`.

## Must-Haves

- [ ] `input_label(0, …)` returns `""` (not `"Source"`)
- [ ] `input_label(1, …)` returns `"ref"` (not `"B"`)
- [ ] `srcAlpha` fetched once per source sample, outside sub-sample loop
- [ ] Zero-alpha input guard: samples with α < 1e-6f pass through unchanged
- [ ] Sub-sample alpha: `alphaSub = 1 - pow(1 - srcAlpha, w)` (multiplicative decomposition)
- [ ] Sub-alpha skip: sub-samples with alphaSub < 1e-6f are not emitted
- [ ] Explicit `Chan_Alpha` branch in foreach dispatch sets alphaSub
- [ ] Other channels scale by `alphaSub / srcAlpha` (premult-correct)
- [ ] `static_cast<float>` on pow result for -Wconversion safety

## Verification

- `scripts/verify-s01-syntax.sh` exits 0
- `grep -q "pow(1" src/DeepCDepthBlur.cpp` succeeds
- `grep -c '"B"' src/DeepCDepthBlur.cpp` outputs `0`
- `grep -q '"ref"' src/DeepCDepthBlur.cpp` succeeds
- `grep -c '1e-6f' src/DeepCDepthBlur.cpp` outputs at least `2` (input guard + sub-alpha guard)

## Inputs

- `src/DeepCDepthBlur.cpp` — current source with additive alpha scaling at line 445 and "Source"/"B" input labels at lines 184-185
- `scripts/verify-s01-syntax.sh` — syntax check script with mock DDImage headers

## Expected Output

- `src/DeepCDepthBlur.cpp` — modified with multiplicative alpha decomposition, zero-alpha guards, and renamed input labels
