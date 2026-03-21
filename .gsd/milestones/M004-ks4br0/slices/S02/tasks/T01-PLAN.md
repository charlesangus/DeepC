# T01: DeepCDepthBlur skeleton, knobs, and falloff engine

**Slice:** S02
**Milestone:** M004-ks4br0

## Goal

Create `src/DeepCDepthBlur.cpp` — a working `DeepFilterOp` with all knobs, four falloff weight generators, and the core per-sample depth-spread logic. Output is not yet tidy and B input is not yet wired.

## Must-Haves

### Truths
- Plugin registers as `"DeepCDepthBlur"` with menu path `"Deep/DeepCDepthBlur"`
- Knobs present: `spread` (Float_knob, default 1.0), `num_samples` (Int_knob, default 5), `falloff` (Enumeration_knob: Linear/Gaussian/Smoothstep/Exponential), `sample_type` (Enumeration_knob: Volumetric/Flat)
- For a single input sample at zFront=Z, spread=S, num_samples=N: produces N sub-samples whose alpha sums equal original alpha (flatten invariant)
- `g++ -fsyntax-only` passes on DeepCDepthBlur.cpp

### Artifacts
- `src/DeepCDepthBlur.cpp` — minimum ~150 lines, substantive implementation

### Key Links
- `src/CMakeLists.txt` → `DeepCDepthBlur` in PLUGINS list and FILTER_NODES

## Steps

1. Read `src/DeepCBlur.cpp` for DeepFilterOp boilerplate (getDeepRequests, doDeepEngine structure)
2. Read `src/DeepThinner.cpp` for Enumeration_knob and Int_knob patterns
3. Create `src/DeepCDepthBlur.cpp`:
   - CLASS = "DeepCDepthBlur", HELP string
   - Members: `_spread` (float), `_numSamples` (int), `_falloff` (int), `_sampleType` (int)
   - `_validate`: clamp `_numSamples` >= 1; clamp `_spread` >= 0
   - `getDeepRequests`: pass-through (same box, same channels, same count — no spatial expansion)
   - `knobs()`: Float_knob spread (range 0–1000), Int_knob num_samples (range 1–64), Enumeration_knob falloff, Enumeration_knob sample_type
4. Implement four weight generators as static free functions returning `std::vector<float>` of length N, normalised to sum to 1:
   - `weightsLinear(int n)`: positions t ∈ [-1,1] uniformly, weight = 1 - |t|, normalise
   - `weightsGaussian(int n)`: positions t ∈ [-1,1], weight = exp(-0.5*(t*3)²), normalise
   - `weightsSmoothstep(int n)`: weight = smoothstep(1-|t|) = 3u²-2u³ where u=(1-|t|), normalise
   - `weightsExponential(int n)`: weight = exp(-3*|t|), normalise
   - Dispatcher: `computeWeights(int n, int mode)` → calls the right one
5. Implement `doDeepEngine`:
   - Zero-spread fast path: if `_spread < 1e-6` or `_numSamples <= 1`, pass through unchanged
   - For each pixel: fetch input plane, iterate samples
   - For each sample: compute N depth positions evenly spaced in [zFront-spread/2, zFront+spread/2]
   - Get weights from `computeWeights`
   - Emit N sub-samples: alpha[i] = sample.alpha * weight[i]; channels[i] = sample.channels * weight[i] (premult preserved)
   - Volumetric: zFront_i = centre_i - halfStep; zBack_i = centre_i + halfStep
   - Flat: zFront_i = zBack_i = centre_i
6. Add `DeepCDepthBlur` to `src/CMakeLists.txt` PLUGINS list and FILTER_NODES
7. Run `g++ -fsyntax-only` with mock headers to confirm no syntax errors

## Context

- `getDeepRequests` for a pass-through node can simply forward: same box, same channels, count unchanged — see DeepCBlur.cpp lines ~127-148 for the general pattern but note DeepCDepthBlur doesn't expand the bbox
- Sub-sample depth positions: with N samples and spread S, step = S / N, positions = [zFront - S/2 + step/2 + i*step] for i in 0..N-1 (centred, evenly spaced)
- Weights normalisation: divide each weight by sum(weights)
- The flatten invariant follows automatically if sum(weights) = 1 and channels scale with weight (premult relationship preserved)
- No B input yet — add in T02

## Expected Output

- `src/DeepCDepthBlur.cpp`
- `src/CMakeLists.txt` (modified)

## Observability Impact

- **New signals:** `DeepCDepthBlur` registers as a Nuke plugin with menu path `Deep/DeepCDepthBlur`. Knob values (spread, num_samples, falloff, sample_type) are visible in the Nuke properties panel.
- **Inspection:** A future agent can verify the node exists by checking `g++ -fsyntax-only` passes, and by grepping CMakeLists.txt for the plugin name. Weight normalisation can be tested by extracting the static functions.
- **Failure state:** Invalid knob values are clamped silently in `_validate`. Missing input0() returns true (empty output, no crash). The `aborted()` check ensures render cancellation is responsive.
