---
estimated_steps: 8
estimated_files: 2
---

# T01: Implement WH_knob, twirldown, alpha correction, and verify build

**Slice:** S02 — Alpha correction + UI polish
**Milestone:** M003

## Description

Make all four S02 deliverables in `src/DeepCBlur.cpp` — WH_knob replacing separate Float_knobs (R004), BeginClosedGroup twirldown for sample optimization knobs (R005), Bool_knob alpha correction toggle (R006), and the post-blur alpha correction pass (R003) — then update mock headers for syntax verification and run both syntax check and docker build (R008).

**Relevant skills:** none needed (pure C++ edits to a Nuke plugin, no frameworks involved).

## Steps

1. **Replace member variables.** In the `DeepCBlur` class, replace:
   ```cpp
   float _blurWidth;
   float _blurHeight;
   ```
   with:
   ```cpp
   double _blurSize[2];     // WH_knob: [0]=width, [1]=height
   bool   _alphaCorrection; // post-blur alpha darkening correction
   ```
   Update the constructor initializer list: `_blurSize{1.0, 1.0}` and `_alphaCorrection(false)`.

2. **Rewrite `knobs()`.** Replace the two `Float_knob` calls with a single `WH_knob`:
   ```cpp
   WH_knob(f, _blurSize, "blur_size", "blur size");
   SetRange(f, 0.0, 100.0);
   Tooltip(f, "Blur radius in pixels (sigma = radius / 3). Width and height can be set independently using the lock toggle.");
   ```
   Keep the `Enumeration_knob` for kernel quality as-is.
   Add:
   ```cpp
   Bool_knob(f, &_alphaCorrection, "alpha_correction", "alpha correction");
   Tooltip(f, "Correct alpha darkening caused by over-compositing blurred deep samples. "
               "Enable when the blur result appears too dark after compositing.");
   ```
   Wrap the three sample optimization knobs with:
   ```cpp
   BeginClosedGroup(f, "Sample Optimization");
   // existing Int_knob max_samples, Float_knob merge_tolerance, Float_knob color_tolerance
   EndGroup(f);
   ```

3. **Update `_validate` and `getDeepRequests`.** Replace all references to `_blurWidth` with `(float)_blurSize[0]` and `_blurHeight` with `(float)_blurSize[1]`. The `kernelRadius` static method takes float, so the cast is needed since `_blurSize` is `double[2]` (required by `WH_knob`).

4. **Update `doDeepEngine` member access.** At the top of `doDeepEngine`, replace:
   ```cpp
   const int radX = kernelRadius(_blurWidth);
   const int radY = kernelRadius(_blurHeight);
   ```
   with:
   ```cpp
   const float blurW = static_cast<float>(_blurSize[0]);
   const float blurH = static_cast<float>(_blurSize[1]);
   const int radX = kernelRadius(blurW);
   const int radY = kernelRadius(blurH);
   ```
   Replace `_blurWidth` and `_blurHeight` in `computeKernel` calls with `blurW`/`blurH`.

5. **Insert alpha correction pass.** In `doDeepEngine`, after the `optimizeSamples` call and before the "Emit output samples" block, insert:
   ```cpp
   // Alpha darkening correction — undo over-composite darkening
   if (_alphaCorrection && scratch.samples.size() > 1) {
       float cumTransp = 1.0f;
       for (auto& sr : scratch.samples) {
           if (cumTransp > 1e-6f) {
               const float inv = 1.0f / cumTransp;
               for (int ci = 0; ci < nChans; ++ci) {
                   if (!isDepthChan[ci])
                       sr.channels[ci] *= inv;
               }
               sr.alpha *= inv;
           }
           cumTransp *= std::max(0.0f, 1.0f - sr.alpha);
       }
   }
   ```
   Note: `optimizeSamples` already sorts samples front-to-back (zFront ascending), so no re-sort is needed. The `cumTransp` variable tracks cumulative transmittance from front to back. Samples behind opaque layers (cumTransp ≈ 0) are left uncorrected to avoid division by near-zero.

6. **Update HELP string.** Replace the existing help text to mention:
   - Separable Gaussian blur with kernel quality tiers
   - WH_knob blur size with lock toggle
   - Alpha correction toggle for compositing correctness
   - Sample optimization controls in twirldown

7. **Update mock DDImage headers.** In `scripts/verify-s01-syntax.sh`, add these stubs to the `Knobs.h` mock:
   ```cpp
   inline Knob* WH_knob(Knob_Callback, double*, const char*, const char* = nullptr) { return nullptr; }
   inline Knob* Bool_knob(Knob_Callback, bool*, const char*, const char* = nullptr) { return nullptr; }
   inline void BeginClosedGroup(Knob_Callback, const char*) {}
   inline void EndGroup(Knob_Callback) {}
   ```
   Also rename the script banner comment from "verify-s01-syntax.sh" to reflect it now covers S02 changes too (optional but helpful).

8. **Verify.** Run `bash scripts/verify-s01-syntax.sh` — must print "Syntax check passed." Then run `bash docker-build.sh --linux --versions 16.0` — must exit 0 with DeepCBlur.so in the output archive.

## Must-Haves

- [ ] `double _blurSize[2]` member replaces `float _blurWidth` and `float _blurHeight`
- [ ] `WH_knob` in knobs() replaces two `Float_knob` calls for blur size
- [ ] `bool _alphaCorrection` member with `Bool_knob` in knobs(), default false
- [ ] `BeginClosedGroup("Sample Optimization")` / `EndGroup` wraps the three sample optimization knobs
- [ ] Alpha correction loop inserted after `optimizeSamples`, before emit, using cumulative transmittance
- [ ] Alpha correction guarded by `_alphaCorrection && scratch.samples.size() > 1`
- [ ] `_blurSize` cast to float at all call sites (kernelRadius, computeKernel)
- [ ] Mock headers updated with WH_knob, Bool_knob, BeginClosedGroup, EndGroup stubs
- [ ] `bash scripts/verify-s01-syntax.sh` exits 0
- [ ] `bash docker-build.sh --linux --versions 16.0` exits 0

## Verification

- `grep -c "WH_knob" src/DeepCBlur.cpp` returns 1
- `grep "Float_knob.*blur" src/DeepCBlur.cpp` returns no output
- `grep -c "BeginClosedGroup" src/DeepCBlur.cpp` returns 1
- `grep -c "Bool_knob.*alpha_correction" src/DeepCBlur.cpp` returns 1
- `grep -q "cumTransp" src/DeepCBlur.cpp` confirms correction logic present
- `bash scripts/verify-s01-syntax.sh` prints "Syntax check passed."
- `bash docker-build.sh --linux --versions 16.0` exits 0

## Inputs

- `src/DeepCBlur.cpp` — S01's separable blur engine with doDeepEngine (H→V passes), knobs(), _validate, getDeepRequests; current state has Float_knobs for blur size, no twirldown, no alpha correction
- `scripts/verify-s01-syntax.sh` — S01's syntax verification script with mock DDImage headers; needs WH_knob/Bool_knob/BeginClosedGroup/EndGroup stubs added
- `src/DeepCAdjustBBox.cpp` — reference for WH_knob pattern (line 17: `double numpixels[2]`, line 42: `WH_knob(f, numpixels, ...)`)
- `src/DeepThinner.cpp` — reference for BeginClosedGroup/EndGroup pattern (line 183)
- `src/DeepSampleOptimizer.h` — SampleRecord struct definition (confirms `float alpha` + `vector<float> channels` layout)

## Expected Output

- `src/DeepCBlur.cpp` — modified: WH_knob, Bool_knob, BeginClosedGroup/EndGroup, alpha correction pass, updated help text
- `scripts/verify-s01-syntax.sh` — modified: mock Knobs.h with WH_knob/Bool_knob/BeginClosedGroup/EndGroup stubs
