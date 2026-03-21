# S02: DeepCDepthBlur node

**Goal:** Ship a new `DeepCDepthBlur` plugin that spreads each input sample's alpha across N sub-samples in Z with a selectable falloff, produces tidy output, preserves the flatten invariant, and optionally gates spreading to depths relevant to a second deep input.

**Demo:** DeepCDepthBlur on a deep holdout shows softened depth when merged, while `flatten(output) == flatten(input)`.

## Must-Haves

- Flatten invariant: sub-sample weights sum to 1; no alpha is added
- Tidy output: sorted by zFront, non-overlapping (zBack[i] ≤ zFront[i+1])
- Four falloff modes: Linear, Gaussian, Smoothstep, Exponential — all normalised
- Volumetric/Flat toggle: volumetric sub-samples cover depth sub-ranges; flat sub-samples have zFront==zBack
- Knobs: spread (float), num_samples (int), falloff (enum), sample_type (enum)
- B input optional: when connected, only spread samples that depth-overlap with B; others pass through
- `docker-build.sh --linux --versions 16.0` exits 0 with DeepCDepthBlur.so in archive

## Tasks

- [x] **T01: DeepCDepthBlur skeleton + knobs + falloff engine**
  Create `src/DeepCDepthBlur.cpp` as a `DeepFilterOp` subclass. Wire knobs, implement the four falloff weight generators, implement the spread-and-distribute core for a single sample. No B input yet.

- [x] **T02: Tidy output + optional B input + docker verification**
  Add the final tidy pass (sort + clamp overlaps). Add the optional B input with depth-range gating. Run docker build and verify flatten invariant with syntax checks.

## Verification

- `g++ -fsyntax-only` passes on `src/DeepCDepthBlur.cpp` using mock DDImage headers
- `grep -c DeepCDepthBlur src/CMakeLists.txt` returns ≥ 2 (PLUGINS + FILTER_NODES)
- `wc -l < src/DeepCDepthBlur.cpp` ≥ 150
- `docker-build.sh --linux --versions 16.0` exits 0 with DeepCDepthBlur.so in archive (T02)
- Weight generators produce normalised weights: sum(weights) ≈ 1.0 for all falloff modes
- Zero-spread fast path: output == input when spread < 1e-6 or num_samples <= 1
- Missing A input returns `true` from `doDeepEngine` (no crash, empty output)
- B input fetch failure falls back to "no B samples" mode gracefully

## Observability / Diagnostics

- **Runtime signals:** The node's HELP string documents all knob semantics; knob tooltips describe each parameter's effect. The zero-spread fast path is a visible no-op when `spread == 0`.
- **Inspection surfaces:** In Nuke, connect a DeepToImage downstream to flatten and visually verify the invariant. Use the DeepRead/DeepWrite chain to inspect per-sample Z distributions.
- **Failure visibility:** Invalid knob values are clamped in `_validate` (num_samples ≥ 1, spread ≥ 0). A missing input returns `true` from `doDeepEngine` (no crash, empty output). The `aborted()` check in the pixel loop ensures cancellation is responsive.
- **Diagnostic verification:** `g++ -fsyntax-only` with mock headers catches compile errors without the full Nuke SDK. Weight normalisation can be unit-tested by calling `computeWeights()` directly.

## Files Likely Touched

- `src/DeepCDepthBlur.cpp` (new)
- `src/CMakeLists.txt`
- `scripts/verify-s01-syntax.sh` (add DeepCDepthBlur stubs if needed)
