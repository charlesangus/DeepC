# M004-ks4br0: DeepCBlur correctness + DeepCDepthBlur — Context

**Gathered:** 2026-03-21
**Status:** Ready for planning

## Project Description

DeepC deep compositing plugin suite for Nuke. This milestone has two parts: fix correctness bugs in the sample optimizer used by DeepCBlur/DeepCBlur2, and ship a new DeepCDepthBlur node that spreads samples along the Z axis only.

## Why This Milestone

DeepCBlur produces jaggy output because `colorDistance` in `DeepSampleOptimizer` operates on premultiplied channel values. After Gaussian weighting, two samples from the same surface at different weights appear to have different colours, preventing merges that should happen. Separately, overlapping samples (including volumetric ranges that intersect) are not tidied before the merge pass, compounding the artifacts.

DeepCDepthBlur is a new node to soften holdouts. It adds samples in Z only — no spatial neighbours consulted. The flat image is invariant before and after the node.

## Bug Analysis

### colorDistance operates on premultiplied values
- Channels in `SampleRecord` are stored as `original_channel * gaussianWeight`
- Alpha is stored as `original_alpha * gaussianWeight`
- So `channels[i] / alpha = original_channel / original_alpha` — correct unpremultiplied colour regardless of blur pass
- Fix: divide each channel by alpha before comparison; guard at `alpha < 1e-6` (treat as transparent — use zero distance, i.e. always mergeable)
- Fix is in `DeepSampleOptimizer.h` — both blur nodes benefit automatically

### Overlapping samples not pre-tidied
- After gather, the sample list may contain overlapping depth intervals (e.g. volumetric sample [1,5] and [3,7])
- The existing merge pass only groups by Z-distance tolerance — it does not handle true depth-range overlap
- Fix: add an overlap pre-pass before the existing merge pass:
  1. Sort by zFront
  2. Walk sorted list; for each overlapping pair, split at the boundary
  3. For volumetric splits, use `alpha_front = 1 - (1-A)^((z-zFront)/(zBack-zFront))` to subdivide alpha; channel values scale proportionally
  4. Sort again (now non-overlapping)
  5. Over-merge front-to-back (existing compositing math applies)
- Point samples (`zFront == zBack`) can overlap only with other point samples at the exact same depth; collapse those by over-compositing in place

## DeepCDepthBlur Design

### Core invariant
`flatten(DeepCDepthBlur(input)) == flatten(input)`

Achieved by: distribute each source sample's alpha across N sub-samples using a falloff weight vector that sums to 1. The sub-samples together represent the same coverage as the original. Do NOT add new alpha — redistribute.

### Tidy output
- Samples sorted by zFront ascending
- No overlapping depth ranges: `zBack[i] <= zFront[i+1]` for all i
- Each sample: `zFront <= zBack`

### Output sample type (user toggle)
- **Volumetric**: each sub-sample covers a sub-range of depth, `zFront < zBack`. More physically correct for volumes.
- **Flat/point**: each sub-sample has `zFront == zBack`. Simpler; matches hard-surface renders.

### Falloff modes (enum knob)
- **Linear**: `weight = 1 - |t|` where `t = offset / spread` ∈ [-1, 1]
- **Gaussian**: `weight = exp(-0.5 * (offset/sigma)²)`, `sigma = spread/3`
- **Smoothstep**: `weight = 3t²-2t³` applied to `(1 - |offset|/spread)`
- **Exponential**: `weight = exp(-3 * |offset| / spread)` (edge weight ~5%)
- All weights normalised to sum to 1 before distributing alpha

### Controls
- `spread` (float): total depth extent over which sub-samples are placed
- `num_samples` (int): number of sub-samples to generate per input sample
- `falloff` (enum): Linear / Gaussian / Smoothstep / Exponential
- `sample_type` (enum): Volumetric / Flat
- Input B (optional): second deep image

### Second input gating
When B is connected: for each pixel, only apply spreading to source samples whose depth range `[zFront - spread, zBack + spread]` intersects any sample's `[zFront_B, zBack_B]` in the B pixel. Source samples outside that overlap pass through unchanged. Gating is depth-range intersection, not pixel presence.

### Algorithm outline
1. Fetch source pixel samples, sort by zFront
2. If B connected: fetch B pixel, build B depth interval list
3. For each source sample:
   a. If B connected and no depth overlap with B: pass through unchanged
   b. Else: generate `num_samples` sub-samples at evenly-spaced depth offsets centred on source zFront, within ±spread
   c. Compute falloff weights, normalise to sum=1
   d. Assign sub-sample alpha = source.alpha × weight[i]
   e. Assign sub-sample channels = source.channels × weight[i] (maintains premult)
   f. Set sub-sample z: volumetric = [centre_i - halfStep, centre_i + halfStep]; flat = [centre_i, centre_i]
4. Collect all output samples (pass-through + spread), sort by zFront
5. Final tidy: clamp zBack[i] = min(zBack[i], zFront[i+1]) to eliminate any remaining overlap

## Existing Codebase

- `src/DeepSampleOptimizer.h` — FIX HERE: colorDistance unpremult + overlap pre-pass
- `src/DeepCBlur.cpp` — M002 non-separable blur, benefits from optimizer fix automatically
- `src/DeepCBlur2.cpp` — M003 separable blur, benefits from optimizer fix automatically
- `src/DeepThinner.cpp` — reference for multi-input patterns and existing merge logic
- `src/CMakeLists.txt` — add DeepCDepthBlur to PLUGINS + FILTER_NODES

> See `.gsd/DECISIONS.md` for all architectural decisions.

## Relevant Requirements

- R011 — colorDistance unpremult fix
- R012 — overlap tidy pre-pass in optimizeSamples
- R013 — DeepCDepthBlur flatten invariant
- R014 — DeepCDepthBlur tidy output
- R015 — DeepCDepthBlur falloff modes
- R016 — DeepCDepthBlur spread/num-samples controls + volumetric/flat toggle
- R017 — DeepCDepthBlur second input depth-overlap gating

## Scope

### In Scope
- `colorDistance` unpremult fix in `DeepSampleOptimizer.h`
- Overlap tidy pre-pass in `optimizeSamples`
- New `DeepCDepthBlur` node: 4 falloff modes, volumetric/flat toggle, spread + num-samples knobs
- Optional B input with depth-range gating
- Docker build for all three blur plugins

### Out of Scope / Non-Goals
- Spatial neighbours in DeepCDepthBlur
- Fixing DeepThinner's own colorDistance (separate, lower priority)
- Per-channel spread amounts
- Animated spread

## Technical Constraints
- `DeepSampleOptimizer.h` must remain zero-DDImage-dependency
- `DeepCDepthBlur` is a `DeepFilterOp` subclass
- Tidy output required; emit samples sorted by zFront
- Volumetric alpha subdivision: `alpha_sub = 1 - (1-A)^(subRange/totalRange)`
