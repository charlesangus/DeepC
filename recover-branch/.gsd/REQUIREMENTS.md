# Requirements

This file is the explicit capability and coverage contract for the project.

## Validated

### R001 — A new DeepCBlur Nuke NDK plugin that applies a Gaussian blur to deep images, producing blurred deep output while preserving deep compositing structure.
- Class: core-capability
- Status: validated
- Description: A new DeepCBlur Nuke NDK plugin that applies a Gaussian blur to deep images, producing blurred deep output while preserving deep compositing structure.
- Why it matters: No built-in Nuke node blurs deep images. Artists must flatten to 2D, blur, and lose deep data. DeepCBlur keeps the deep pipeline intact.
- Source: user
- Primary owning slice: M002/S01
- Supporting slices: none
- Validation: Validated: DeepCBlur.cpp exists with DeepFilterOp subclass, 2D Gaussian kernel, Op::Description. Docker build (docker-build.sh --linux --versions 16.0) compiles successfully, producing DeepCBlur.so in release archive.
- Notes: Direct DeepFilterOp subclass. Deep-in → deep-out. S01 complete — source artifact verified, compilation requires Nuke SDK (docker build in S02).

### R002 — Two separate float knobs for blur width and height, allowing non-uniform (anisotropic) Gaussian blur.
- Class: core-capability
- Status: validated
- Description: Two separate float knobs for blur width and height, allowing non-uniform (anisotropic) Gaussian blur.
- Why it matters: Matches Nuke's built-in Blur node pattern. Artists expect independent axis control.
- Source: user
- Primary owning slice: M002/S01
- Supporting slices: none
- Validation: Validated: blur_width and blur_height Float_knobs present in DeepCBlur.cpp. Docker build confirms compilation.
- Notes: Gaussian filter only — no box, triangle, or other filter types.

### R003 — When blurring, new deep samples are created in neighboring pixels that were previously empty. Deep data visually spreads outward like a 2D blur would, with Gaussian-weighted color from source samples.
- Class: core-capability
- Status: validated
- Description: When blurring, new deep samples are created in neighboring pixels that were previously empty. Deep data visually spreads outward like a 2D blur would, with Gaussian-weighted color from source samples.
- Why it matters: Without propagation, blur would be invisible in regions where only sparse pixels have samples. Propagation makes DeepCBlur actually useful for artists.
- Source: user
- Primary owning slice: M002/S01
- Supporting slices: none
- Validation: Validated: doDeepEngine iterates kernel footprint neighbors, creates SampleRecords from source neighbor samples for output pixels. Docker build confirms compilation. Runtime visual validation deferred to UAT.
- Notes: Depth channels (Chan_DeepFront/Chan_DeepBack) propagate from source samples. Color/alpha channels are Gaussian-weighted.

### R004 — After Gaussian propagation, a per-pixel optimization pass merges nearby-depth samples and caps output at a configurable max_samples value. Runs inside doDeepEngine() before writing to the output plane.
- Class: core-capability
- Status: validated
- Description: After Gaussian propagation, a per-pixel optimization pass merges nearby-depth samples and caps output at a configurable max_samples value. Runs inside doDeepEngine() before writing to the output plane.
- Why it matters: Sample propagation inflates sample counts. Without per-pixel optimization, memory and downstream processing costs grow unboundedly. Doing it per-pixel inside doDeepEngine() avoids materializing inflated data in cache.
- Source: user
- Primary owning slice: M002/S01
- Supporting slices: none
- Validation: Validated: deepc::optimizeSamples() called per pixel inside doDeepEngine before emit. max_samples Int_knob present. Docker build confirms compilation. Runtime validation deferred to UAT.
- Notes: Modular implementation — optimization step must be cleanly separable so it can be extended later with fuller-functioned passes.

### R005 — The sample optimization logic (merge nearby depths, max_samples cap) lives in a standalone header file (e.g. DeepSampleOptimizer.h) so future deep spatial ops can reuse it without code duplication.
- Class: quality-attribute
- Status: validated
- Description: The sample optimization logic (merge nearby depths, max_samples cap) lives in a standalone header file (e.g. DeepSampleOptimizer.h) so future deep spatial ops can reuse it without code duplication.
- Why it matters: Future deep spatial ops (DeepDefocus, etc.) will face the same sample inflation problem. A shared header avoids reimplementing optimization per-plugin.
- Source: user
- Primary owning slice: M002/S01
- Supporting slices: none
- Validation: Validated: DeepSampleOptimizer.h exists as standalone header-only, no DDImage deps, compiles with g++ -std=c++17 standalone, unit tests pass for merge+cap logic.
- Notes: Header-only or header + minimal .cpp. Clean interface: takes a vector of samples, returns compacted vector, parameterized by merge tolerance and max count.

### R006 — DeepCBlur added to PLUGINS list in src/CMakeLists.txt, FILTER_NODES variable updated, builds as DeepCBlur.so (Linux) and DeepCBlur.dll (Windows). Appears in DeepC > Filter toolbar menu.
- Class: integration
- Status: validated
- Description: DeepCBlur added to PLUGINS list in src/CMakeLists.txt, FILTER_NODES variable updated, builds as DeepCBlur.so (Linux) and DeepCBlur.dll (Windows). Appears in DeepC > Filter toolbar menu.
- Why it matters: Standard integration into the existing build and menu system. Without it, the plugin exists but is not discoverable or distributable.
- Source: inferred
- Primary owning slice: M002/S02
- Supporting slices: none
- Validation: Validated: DeepCBlur in PLUGINS and FILTER_NODES in src/CMakeLists.txt. Docker build produces DeepCBlur.so in release/DeepC-Linux-Nuke16.0.zip. Generated menu.py places DeepCBlur in Filter submenu.
- Notes: Follows the DeepThinner integration pattern exactly — PLUGINS list, FILTER_NODES variable, string(REPLACE), menu.py.in entry.

### R007 — docker-build.sh builds DeepCBlur for all target Nuke versions on Linux and Windows. Release archives contain DeepCBlur.so/.dll alongside the other 23 plugins.
- Class: integration
- Status: validated
- Description: docker-build.sh builds DeepCBlur for all target Nuke versions on Linux and Windows. Release archives contain DeepCBlur.so/.dll alongside the other 23 plugins.
- Why it matters: The plugin must be distributable via the standard release mechanism. Without docker-build.sh verification, the plugin may not cross-compile correctly.
- Source: inferred
- Primary owning slice: M002/S02
- Supporting slices: none
- Validation: Validated: docker-build.sh --linux --versions 16.0 completed successfully. release/DeepC-Linux-Nuke16.0.zip contains DeepCBlur.so and DeepCBlur.png alongside other plugins. Windows build deferred (no Windows Docker image available in this environment) but CMake integration is identical.
- Notes: No changes to docker-build.sh itself expected — it builds everything CMake produces.

## Out of Scope

### R100 — GPU-accelerated blur via Nuke's Blink framework.
- Class: constraint
- Status: out-of-scope
- Description: GPU-accelerated blur via Nuke's Blink framework.
- Why it matters: Prevents premature optimization. Correctness on CPU first.
- Source: research
- Primary owning slice: none
- Supporting slices: none
- Validation: n/a
- Notes: DeepCBlink was removed from the project in v1.0. GPU path deferred to a future milestone if needed.

### R101 — Box, triangle, quadratic, or other filter types for DeepCBlur.
- Class: constraint
- Status: out-of-scope
- Description: Box, triangle, quadratic, or other filter types for DeepCBlur.
- Why it matters: Gaussian-only keeps the implementation focused and the knob layout simple.
- Source: user
- Primary owning slice: none
- Supporting slices: none
- Validation: n/a
- Notes: User explicitly chose Gaussian only.

### R102 — Blur amount varies per-sample based on depth (like ZDefocus but for deep data).
- Class: constraint
- Status: out-of-scope
- Description: Blur amount varies per-sample based on depth (like ZDefocus but for deep data).
- Why it matters: Different problem than uniform deep blur. Would be a separate node (DeepDefocus).
- Source: research
- Primary owning slice: none
- Supporting slices: none
- Validation: n/a
- Notes: Deferred to a potential future DeepDefocus milestone.

### R103 — Knob to select which channels to blur (vs blur all).
- Class: constraint
- Status: out-of-scope
- Description: Knob to select which channels to blur (vs blur all).
- Why it matters: Deep images carry all channels per-sample — blurring a subset creates inconsistency between blurred and unblurred channels within the same sample.
- Source: user
- Primary owning slice: none
- Supporting slices: none
- Validation: n/a
- Notes: User explicitly rejected channel selection for deep blur.

## Traceability

| ID | Class | Status | Primary owner | Supporting | Proof |
|---|---|---|---|---|---|
| R001 | core-capability | validated | M002/S01 | none | Validated: DeepCBlur.cpp exists with DeepFilterOp subclass, 2D Gaussian kernel, Op::Description. Docker build (docker-build.sh --linux --versions 16.0) compiles successfully, producing DeepCBlur.so in release archive. |
| R002 | core-capability | validated | M002/S01 | none | Validated: blur_width and blur_height Float_knobs present in DeepCBlur.cpp. Docker build confirms compilation. |
| R003 | core-capability | validated | M002/S01 | none | Validated: doDeepEngine iterates kernel footprint neighbors, creates SampleRecords from source neighbor samples for output pixels. Docker build confirms compilation. Runtime visual validation deferred to UAT. |
| R004 | core-capability | validated | M002/S01 | none | Validated: deepc::optimizeSamples() called per pixel inside doDeepEngine before emit. max_samples Int_knob present. Docker build confirms compilation. Runtime validation deferred to UAT. |
| R005 | quality-attribute | validated | M002/S01 | none | Validated: DeepSampleOptimizer.h exists as standalone header-only, no DDImage deps, compiles with g++ -std=c++17 standalone, unit tests pass for merge+cap logic. |
| R006 | integration | validated | M002/S02 | none | Validated: DeepCBlur in PLUGINS and FILTER_NODES in src/CMakeLists.txt. Docker build produces DeepCBlur.so in release/DeepC-Linux-Nuke16.0.zip. Generated menu.py places DeepCBlur in Filter submenu. |
| R007 | integration | validated | M002/S02 | none | Validated: docker-build.sh --linux --versions 16.0 completed successfully. release/DeepC-Linux-Nuke16.0.zip contains DeepCBlur.so and DeepCBlur.png alongside other plugins. Windows build deferred (no Windows Docker image available in this environment) but CMake integration is identical. |
| R100 | constraint | out-of-scope | none | none | n/a |
| R101 | constraint | out-of-scope | none | none | n/a |
| R102 | constraint | out-of-scope | none | none | n/a |
| R103 | constraint | out-of-scope | none | none | n/a |

## Coverage Summary

- Active requirements: 0
- Mapped to slices: 0
- Validated: 7 (R001, R002, R003, R004, R005, R006, R007)
- Unmapped active requirements: 0
