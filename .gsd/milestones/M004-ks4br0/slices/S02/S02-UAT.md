# S02: DeepCDepthBlur node — UAT

**Milestone:** M004-ks4br0
**Written:** 2026-03-21

## UAT Type

- UAT mode: artifact-driven (docker build + syntax checks + code inspection) + human-experience (Nuke visual confirmation)
- Why this mode is sufficient: The plugin's core contracts (flatten invariant by construction, tidy output by struct-sort + clamp, falloff normalisation by sum=1) are verified structurally through code inspection and docker build. Visual confirmation in Nuke provides the human-experience layer for the holdout-softening use case. The flatten invariant cannot be fully proved without a live Nuke session, so that test case is marked for human follow-up.

## Preconditions

**Artifact-driven tests (no Nuke required):**
- Working directory: root of the DeepC repository (worktree `M004-ks4br0`)
- `bash docker-build.sh --linux --versions 16.0` has been run and `release/DeepC-Linux-Nuke16.0.zip` exists
- `bash scripts/verify-s01-syntax.sh` is available

**Human-experience tests (Nuke required):**
- Nuke 16.0 installed with Nuke plugin path pointing to the extracted `release/DeepC-Linux-Nuke16.0.zip` contents
- A deep EXR render available (any scene with depth variation, e.g. a deep holdout matte)
- DeepRead node loaded with the test render

---

## Smoke Test

Run the syntax script and check the zip:

```bash
bash scripts/verify-s01-syntax.sh
unzip -l release/DeepC-Linux-Nuke16.0.zip | grep DeepCDepthBlur
```

**Expected:** Script prints `All syntax checks passed.`; zip listing shows `DeepC/DeepCDepthBlur.so` with size > 400 000 bytes.

---

## Test Cases

### 1. Plugin compiles and links against Nuke 16.0 SDK (docker build)

1. From the repo root, run: `bash docker-build.sh --linux --versions 16.0`
2. Wait for the build to complete (approx. 30–60s).
3. Run: `unzip -l release/DeepC-Linux-Nuke16.0.zip | grep -E 'DeepCBlur|DeepCDepthBlur'`
4. **Expected:** Exit code 0 from docker-build.sh. Zip listing shows all three entries:
   - `DeepC/DeepCBlur.so`
   - `DeepC/DeepCBlur2.so`
   - `DeepC/DeepCDepthBlur.so` (≥ 400 000 bytes)

---

### 2. Syntax check covers both plugins

1. Run: `bash scripts/verify-s01-syntax.sh`
2. **Expected:** Output contains both:
   - `Syntax check passed: DeepCBlur.cpp`
   - `Syntax check passed: DeepCDepthBlur.cpp`
   - Final line: `All syntax checks passed.`
   - Exit code: 0

---

### 3. CMakeLists.txt registration

1. Run: `grep -c DeepCDepthBlur src/CMakeLists.txt`
2. **Expected:** Output is `2` (one entry in PLUGINS list, one in FILTER_NODES set).

---

### 4. All four knobs present in source

1. Run:
   ```bash
   for knob in spread num_samples falloff sample_type; do
     grep -q "\"$knob\"" src/DeepCDepthBlur.cpp && echo "$knob: OK" || echo "$knob: MISSING"
   done
   ```
2. **Expected:** All four lines print `OK`.

---

### 5. All four falloff weight generators normalise to sum ≈ 1.0

1. Extract the four weight generator functions from `src/DeepCDepthBlur.cpp` into a standalone test harness, or inspect the implementation directly.
2. For each of `weightsLinear`, `weightsGaussian`, `weightsSmoothstep`, `weightsExponential`, and for each n in {1, 2, 3, 5, 10, 32}:
   - Compute `sum = Σ weights[i]`
   - **Expected:** `|sum - 1.0| < 1e-5` for all combinations.
3. **Expected (code inspection shortcut):** Each generator ends with `float sum = ...; if (sum > 0) for (auto& v : w) v /= sum; else for (auto& v : w) v = 1.0f / n;` — the uniform fallback handles the n=2 degenerate case.

---

### 6. Zero-spread fast path (code inspection)

1. Open `src/DeepCDepthBlur.cpp` and locate the `doDeepEngine` function.
2. Verify there is an early return path triggered when `_spread < 1e-6f || _numSamples <= 1` that copies source samples to output unchanged.
3. **Expected:** A clear early-return block passes all source samples through without modification.

---

### 7. Tidy output — sort + overlap clamp (code inspection)

1. Open `src/DeepCDepthBlur.cpp` and locate the tidy output pass.
2. Verify:
   - Sub-samples are collected into a struct/vector before sorting.
   - `std::sort` (or equivalent) is called, ordering by `zFront` ascending.
   - A loop clamps `zBack[i] = min(zBack[i], zFront[i+1])`.
   - The clamp does **not** modify alpha or channel values.
3. **Expected:** All three steps are present; only `zBack` is modified by the clamp.

---

### 8. Optional B input wiring (code inspection)

1. Open `src/DeepCDepthBlur.cpp` and verify:
   - Constructor calls `inputs(2)`.
   - `minimum_inputs()` returns 1; `maximum_inputs()` returns 2.
   - `doDeepEngine` uses `dynamic_cast<DeepOp*>(Op::input(1))` with a null check.
   - When B is null or B pixel is empty, all A samples are spread (no crash).
   - When B is connected and a B pixel has no depth-overlapping samples, A samples at that pixel pass through unchanged.
2. **Expected:** All checks confirmed in source.

---

### 9. Flatten invariant (Nuke visual confirmation — human required)

1. In Nuke 16.0, create the following graph:
   ```
   DeepRead (deep EXR) ──┬── DeepToImage ── Viewer (A)
                          └── DeepCDepthBlur (default knobs) ── DeepToImage ── Viewer (B)
   ```
2. Set DeepCDepthBlur knobs: `spread=1.0`, `num_samples=5`, `falloff=Linear`, `sample_type=Volumetric`.
3. Compare viewers A and B at multiple pixels across the image.
4. **Expected:** Viewers A and B are pixel-identical. No channel values differ between the flattened input and flattened output.

---

### 10. Holdout softening visual (Nuke visual confirmation — human required)

1. In Nuke 16.0, connect DeepCDepthBlur to a deep holdout matte (a deep matte that excludes a foreground object from a background).
2. Set `spread=2.0`, `num_samples=9`, `falloff=Smoothstep`, `sample_type=Volumetric`.
3. Use DeepMerge to composite the softened holdout over a background.
4. **Expected:** The holdout edge shows a smooth depth-gradient softening (not a hard cut) while the flat-composite alpha matches the original (flatten invariant holds).

---

## Edge Cases

### n=2, Linear or Smoothstep falloff (degenerate weight case)

1. In Nuke, set DeepCDepthBlur knobs: `num_samples=2`, `falloff=Linear`.
2. Apply to a deep input and flatten.
3. **Expected:** No NaN values in output; flatten(output) == flatten(input). The uniform fallback (weight = 0.5 for each of 2 sub-samples) should fire.

### Zero spread (pass-through)

1. Set `spread=0.0` (or below 1e-6).
2. Apply to a deep input and flatten.
3. **Expected:** Output is identical to input (zero-spread fast path engaged). Pixel viewer shows no change to sample positions or values.

### Missing A input

1. In Nuke, create a DeepCDepthBlur node and disconnect its A input.
2. **Expected:** Node does not crash; output is empty/black; no error dialog.

### B input connected, no B samples at pixel

1. Connect a B input that is a DeepConstant or empty deep stream.
2. Apply to a non-trivial A deep input.
3. **Expected:** All A samples pass through unchanged at pixels where B has no samples. Node does not crash.

### B input connected, all A samples overlap B

1. Connect a B input that has deep samples covering the same depth range as all A samples.
2. **Expected:** All A samples are spread (B-gating does not incorrectly suppress spreading when all samples qualify).

### num_samples=1 (degenerate single sub-sample)

1. Set `num_samples=1` with any falloff mode and any spread value.
2. **Expected:** Zero-spread fast path fires (single sub-sample ≡ pass-through). Output matches input exactly.

---

## Failure Signals

- `scripts/verify-s01-syntax.sh` exits non-zero → C++ syntax error introduced in DeepCDepthBlur.cpp or mock headers out of date
- `docker-build.sh` exits non-zero → DDImage API mismatch (check `minimumInputs` vs `minimum_inputs`, override keywords, or missing headers)
- `unzip -l release/*.zip | grep DeepCDepthBlur` returns empty → plugin not registered in CMakeLists.txt or not linked into archive
- Nuke crash on load → `inputs(2)` or `input()` wiring error; check against DeepCCopyBBox pattern
- Flatten invariant fails (A viewer ≠ B viewer) → weight sum is not 1 for some falloff/n combination, or the tidy clamp incorrectly modified alpha
- NaN values in output → degenerate weight sum=0 case not handled (uniform fallback missing), or division by near-zero alpha in channel scaling

---

## Not Proven By This UAT

- **Real render performance:** Sample count inflation (N sub-samples per source sample) is not benchmarked. On dense deep frames the node may be slow; no performance regression baseline exists.
- **Correct B-input gating with animated B depth:** The gating is per-pixel and per-frame but correctness on rapidly changing B depth has not been tested.
- **Windows build:** `docker-build.sh --windows --versions 16.0` was not run as part of this slice.
- **Nuke 13, 14, 15 compatibility:** Only Nuke 16.0 was tested via docker. Earlier SDK compatibility is not verified.
- **Visual quality of Gaussian/Exponential falloff modes:** Only structural correctness (sum=1) is proven; the artistic quality on real holdout edges is not evaluated.

---

## Notes for Tester

- The flatten invariant (test case 9) is the most important human check. If this fails, it likely points to the weight normalisation: run the weight generator with the exact n and mode used, compute the sum, and check for the degenerate case.
- For test case 7 (tidy output inspection), pay attention to whether the sort is stable — `std::sort` is not stable, but for this use case (depth order, not identity order) stability is not required.
- The B-input edge cases (empty B, disconnected B) are critical for production safety — these guard against crashes when artists accidentally leave the B input unwired or wire a non-deep node.
- Known rough edge: if `spread` is very large (e.g. 1000 units in a scene with depth range of 10 units), sub-samples will be spread far outside the scene's depth range. This is not a bug — the node does what the knob says — but may confuse artists. Document this in the knob tooltip if it causes user confusion.
