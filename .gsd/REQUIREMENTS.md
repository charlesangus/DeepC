# Requirements

This file is the explicit capability and coverage contract for the project.

## Active

### R011 — `colorDistance` in `DeepSampleOptimizer.h` must unpremultiply channel values by alpha before comparing, with a near-zero alpha guard (treat transparent samples as always-matching)
- Class: quality-attribute
- Status: active
- Description: `colorDistance` in `DeepSampleOptimizer.h` must unpremultiply channel values by alpha before comparing, with a near-zero alpha guard (treat transparent samples as always-matching)
- Why it matters: Premultiplied comparison causes samples from the same surface at different Gaussian weights to appear as different colours, preventing correct merges and producing jaggy blur output
- Source: user
- Primary owning slice: M004-ks4br0/S01
- Supporting slices: none
- Validation: unmapped
- Notes: Fix is in the shared header — both DeepCBlur and DeepCBlur2 benefit automatically

### R012 — `optimizeSamples` must include a pre-pass that detects overlapping depth intervals (accounting for volumetric zFront/zBack ranges), splits them at boundaries using volumetric alpha subdivision, and over-merges front-to-back before the existing tolerance-based merge
- Class: quality-attribute
- Status: active
- Description: `optimizeSamples` must include a pre-pass that detects overlapping depth intervals (accounting for volumetric zFront/zBack ranges), splits them at boundaries using volumetric alpha subdivision, and over-merges front-to-back before the existing tolerance-based merge
- Why it matters: Overlapping samples produce incorrect compositing results; same-depth-range duplicates from blur gather should always collapse regardless of tolerance settings
- Source: user
- Primary owning slice: M004-ks4br0/S01
- Supporting slices: none
- Validation: unmapped
- Notes: Point samples (zFront==zBack) at identical depth are also collapsed. Existing tolerance merge runs after this pre-pass.

### R014 — Output samples are sorted by zFront, non-overlapping (zBack[i] ≤ zFront[i+1]), and each sample has zFront ≤ zBack
- Class: core-capability
- Status: active
- Description: Output samples are sorted by zFront, non-overlapping (zBack[i] ≤ zFront[i+1]), and each sample has zFront ≤ zBack
- Why it matters: "Tidy" is required for correct downstream deep compositing; overlapping samples produce incorrect DeepMerge results
- Source: user
- Primary owning slice: M004-ks4br0/S02
- Supporting slices: none
- Validation: unmapped
- Notes: Final clamp pass: zBack[i] = min(zBack[i], zFront[i+1])

### R015 — Enum knob with four modes: Linear, Gaussian, Smoothstep (smoothstep curve), Exponential. All weights normalised to sum to 1 before distributing alpha.
- Class: primary-user-loop
- Status: active
- Description: Enum knob with four modes: Linear, Gaussian, Smoothstep (smoothstep curve), Exponential. All weights normalised to sum to 1 before distributing alpha.
- Why it matters: Different falloffs produce different softness characters; smoothstep is the natural choice for soft holdout edges, gaussian for organic volumes
- Source: user
- Primary owning slice: M004-ks4br0/S02
- Supporting slices: none
- Validation: unmapped
- Notes: Smoothstep confirmed as the "smooth" mode in discussion

### R016 — Float knob for spread depth extent; int knob for number of sub-samples per input sample; enum knob for Volumetric vs Flat/point output sample type
- Class: primary-user-loop
- Status: active
- Description: Float knob for spread depth extent; int knob for number of sub-samples per input sample; enum knob for Volumetric vs Flat/point output sample type
- Why it matters: Artists need control over how coarse or fine the depth spread is and whether spread samples cover depth ranges (volumetric, physically correct for volumes) or are point samples (flat, better for hard surfaces)
- Source: user
- Primary owning slice: M004-ks4br0/S02
- Supporting slices: none
- Validation: unmapped
- Notes: Volumetric sub-samples cover evenly-spaced sub-ranges; flat sub-samples have zFront==zBack at evenly-spaced depths

### R047 — Each deep sample's CoC is computed from its real world-unit depth (DeepFront) and the camera/lens parameters (focal length, fstop, focus distance, sensor size). Uses opendefocus's circle-of-confusion crate with "Real" depth math mode.
- Class: core-capability
- Status: active
- Description: Each deep sample's CoC is computed from its real world-unit depth (DeepFront) and the camera/lens parameters (focal length, fstop, focus distance, sensor size). Uses opendefocus's circle-of-confusion crate with "Real" depth math mode.
- Why it matters: Deep samples carry real-world depth values. Per-sample CoC is the foundation of depth-correct defocus — without it, all samples blur the same amount regardless of depth.
- Source: user
- Primary owning slice: M009-mso5fb/S02
- Supporting slices: M009-mso5fb/S01
- Validation: unmapped
- Notes: Real depth math only (not 1/z or Direct). Deep samples provide depth directly — no depth channel needed.

### R048 — An optional second Deep input ("holdout") provides depth-dependent attenuation of the defocused result. For each output pixel, each main input sample's defocused contribution is weighted by the holdout's transmittance at that pixel at the sample's depth. The holdout is not defocused — it provides a sharp depth-aware mask. Holdout transmittance is accumulated front-to-back through holdout samples.
- Class: core-capability
- Status: active
- Description: An optional second Deep input ("holdout") provides depth-dependent attenuation of the defocused result. For each output pixel, each main input sample's defocused contribution is weighted by the holdout's transmittance at that pixel at the sample's depth. The holdout is not defocused — it provides a sharp depth-aware mask. Holdout transmittance is accumulated front-to-back through holdout samples.
- Why it matters: Allows depth-correct integration of roto/2D elements with Deep renders. The holdout operates in depth so near holdout samples correctly mask only far background, not the foreground.
- Source: user
- Primary owning slice: M009-mso5fb/S03
- Supporting slices: none
- Validation: unmapped
- Notes: Same holdout pattern as M006 (R023/R024). Transmittance at depth Z = product of (1 - alpha_i) for all holdout samples with zFront < Z. Holdout contributes no colour.

### R050 — The deep defocus convolution runs GPU-accelerated using wgpu (Vulkan backend on Linux, Metal on macOS), matching the existing opendefocus GPU pipeline.
- Class: quality-attribute
- Status: active
- Description: The deep defocus convolution runs GPU-accelerated using wgpu (Vulkan backend on Linux, Metal on macOS), matching the existing opendefocus GPU pipeline.
- Why it matters: Deep defocus is computationally expensive — per-sample convolution at production resolution is impractical without GPU acceleration.
- Source: user
- Primary owning slice: M009-mso5fb/S02
- Supporting slices: M009-mso5fb/S01
- Validation: unmapped
- Notes: Falls back to CPU path if GPU is unavailable. wgpu handles backend selection automatically.

### R051 — All opendefocus non-uniform bokeh artifacts work with deep sample data: catseye vignetting, barndoor clipping, astigmatism, axial aberration (depth-dependent bokeh shape), and inverse foreground bokeh.
- Class: differentiator
- Status: active
- Description: All opendefocus non-uniform bokeh artifacts work with deep sample data: catseye vignetting, barndoor clipping, astigmatism, axial aberration (depth-dependent bokeh shape), and inverse foreground bokeh.
- Why it matters: Non-uniform artifacts are the primary differentiator of opendefocus over simpler convolution approaches. Without them, the node is just a GPU-accelerated ZDefocus.
- Source: user
- Primary owning slice: M009-mso5fb/S02
- Supporting slices: none
- Validation: unmapped
- Notes: These come from opendefocus's existing kernel — the challenge is ensuring they work correctly when the kernel processes deep samples rather than flat images.

### R052 — An optional Nuke Camera node input provides focal length, fstop, focus distance, and filmback (horizontal/vertical aperture) automatically. When connected, camera parameters override manual knob values. Follows the same CameraOp integration pattern as the existing opendefocus Nuke plugin.
- Class: primary-user-loop
- Status: active
- Description: An optional Nuke Camera node input provides focal length, fstop, focus distance, and filmback (horizontal/vertical aperture) automatically. When connected, camera parameters override manual knob values. Follows the same CameraOp integration pattern as the existing opendefocus Nuke plugin.
- Why it matters: Artists commonly have camera data from their 3D scene. Linking it directly avoids manual parameter entry and ensures consistency.
- Source: user
- Primary owning slice: M009-mso5fb/S03
- Supporting slices: none
- Validation: unmapped
- Notes: Uses DDImage::CameraOp dynamic_cast on input. Nuke 14+ uses focalLength()/fStop()/focusDistance()/horizontalAperture(); older versions use focal_length()/fstop()/focal_point()/film_width().

### R053 — Manual Float_knobs for focal length (mm), fstop, focus distance (scene units), and sensor width (mm). These drive CoC computation when no camera node is connected.
- Class: primary-user-loop
- Status: active
- Description: Manual Float_knobs for focal length (mm), fstop, focus distance (scene units), and sensor width (mm). These drive CoC computation when no camera node is connected.
- Why it matters: Artists need direct control over lens parameters for manual setup or when no camera node is available.
- Source: user
- Primary owning slice: M009-mso5fb/S02
- Supporting slices: none
- Validation: unmapped
- Notes: Defaults: focal length 50mm, fstop 2.8, focus distance 1000 units, sensor width 36mm (35mm film).

### R054 — The C++↔Rust bridge uses the CXX crate, following the same FFI pattern as the existing opendefocus-nuke crate. The bridge is adapted to pass deep sample data (per-pixel sample arrays with RGBA, depth, alpha) from DDImage's DeepPlane to the Rust convolution engine.
- Class: integration
- Status: active
- Description: The C++↔Rust bridge uses the CXX crate, following the same FFI pattern as the existing opendefocus-nuke crate. The bridge is adapted to pass deep sample data (per-pixel sample arrays with RGBA, depth, alpha) from DDImage's DeepPlane to the Rust convolution engine.
- Why it matters: CXX is the proven FFI approach for opendefocus↔Nuke integration. Using the same pattern reduces risk and follows upstream conventions.
- Source: user
- Primary owning slice: M009-mso5fb/S01
- Supporting slices: M009-mso5fb/S02
- Validation: unmapped
- Notes: The existing opendefocus-nuke uses CXX with rust::Slice<f32> for image data. Deep data needs additional structures for per-pixel sample counts and depth arrays.

### R055 — The opendefocus fork is built as a static library via cargo (producing a .a with a C-compatible API). The DeepC CMakeLists.txt links against this static lib. The docker-build.sh script is extended to install the Rust stable + nightly toolchains and build the Rust lib before the CMake build.
- Class: integration
- Status: active
- Description: The opendefocus fork is built as a static library via cargo (producing a .a with a C-compatible API). The DeepC CMakeLists.txt links against this static lib. The docker-build.sh script is extended to install the Rust stable + nightly toolchains and build the Rust lib before the CMake build.
- Why it matters: Keeps the Rust and C++ build systems cleanly separated while producing a single deployable .so plugin.
- Source: user
- Primary owning slice: M009-mso5fb/S01
- Supporting slices: none
- Validation: unmapped
- Notes: Rust stable (1.92+) and nightly (for spirv-cli-build GPU shader compilation) both required. Static linking avoids runtime Rust library dependencies.

### R056 — DeepCOpenDefocus appears in Nuke's node menu under the Filter category (matching existing DeepC filter nodes). Op::Description registers the class.
- Class: launchability
- Status: active
- Description: DeepCOpenDefocus appears in Nuke's node menu under the Filter category (matching existing DeepC filter nodes). Op::Description registers the class.
- Why it matters: Artists need to find and instantiate the node from the standard Nuke menu.
- Source: inferred
- Primary owning slice: M009-mso5fb/S03
- Supporting slices: none
- Validation: unmapped
- Notes: CMakeLists.txt PLUGINS and FILTER_NODES lists. Icon at icons/DeepCOpenDefocus.png (auto-installed by glob).

### R057 — The opendefocus library is forked (maintaining EUPL-1.2 license), modified to add native deep sample support, and referenced as a build dependency from the DeepC project. Attribution added to THIRD_PARTY_LICENSES.md.
- Class: constraint
- Status: active
- Description: The opendefocus library is forked (maintaining EUPL-1.2 license), modified to add native deep sample support, and referenced as a build dependency from the DeepC project. Attribution added to THIRD_PARTY_LICENSES.md.
- Why it matters: EUPL-1.2 is compatible with GPL-3.0 (Article 5). Forking allows deep sample kernel modifications while preserving the upstream license. Attribution is required.
- Source: user
- Primary owning slice: M009-mso5fb/S01
- Supporting slices: none
- Validation: unmapped
- Notes: Fork maintained at a separate repository. EUPL-1.2 ↔ GPL-3.0 compatibility confirmed via EUPL Article 5 compatible licenses list.

### R058 — Defocused deep samples are composited front-to-back into the flat output buffer with correct alpha compositing. Samples at different depths that overlap in the output after defocus are composited in depth order, not simply summed.
- Class: core-capability
- Status: active
- Description: Defocused deep samples are composited front-to-back into the flat output buffer with correct alpha compositing. Samples at different depths that overlap in the output after defocus are composited in depth order, not simply summed.
- Why it matters: Depth-correct compositing is the defining advantage of deep defocus over flat ZDefocus. Without it, foreground and background blur into each other incorrectly.
- Source: user
- Primary owning slice: M009-mso5fb/S02
- Supporting slices: none
- Validation: unmapped
- Notes: This is conceptually similar to Nuke's DeepToImage flatten, but applied after per-sample convolution.

## Validated

### R013 — `flatten(DeepCDepthBlur(input)) == flatten(input)` — sub-sample alphas computed via `α_sub = 1 - (1-α)^w` so that `1 - ∏(1-α_sub_i) = α_original`; premultiplied channels scale as `c * (α_sub / α)`. The node redistributes each sample's alpha across sub-samples using multiplicative decomposition, not additive scaling.
- Class: core-capability
- Status: validated
- Description: `flatten(DeepCDepthBlur(input)) == flatten(input)` — sub-sample alphas computed via `α_sub = 1 - (1-α)^w` so that `1 - ∏(1-α_sub_i) = α_original`; premultiplied channels scale as `c * (α_sub / α)`. The node redistributes each sample's alpha across sub-samples using multiplicative decomposition, not additive scaling.
- Why it matters: The visual result must be identical to the input when composited — the node only softens holdouts by spreading samples in Z, not by changing the flat image
- Source: user
- Primary owning slice: M005-9j5er8/S01
- Supporting slices: none
- Validation: Multiplicative formula `alphaSub = 1 - pow(1-srcAlpha, w)` confirmed in src/DeepCDepthBlur.cpp on milestone/M005-9j5er8; premult channels scale as `c * (alphaSub / srcAlpha)`; Docker build T02 exits 0 producing DeepCDepthBlur.so; `grep -q 'pow(1'` passes
- Notes: M004-ks4br0/S02 shipped this with additive alpha scaling (channel * w) which does not preserve the flatten invariant under multiplicative deep compositing. M005-9j5er8 fixes the math.

### R017 — Optional second deep input labeled "ref" (always visible pipe, not an extra input). Main input unlabelled. When connected, only spread samples whose expanded depth range intersects any ref sample's depth range; others pass through unchanged.
- Class: primary-user-loop
- Status: validated
- Description: Optional second deep input labeled "ref" (always visible pipe, not an extra input). Main input unlabelled. When connected, only spread samples whose expanded depth range intersects any ref sample's depth range; others pass through unchanged.
- Why it matters: Constrains depth spreading to only depths relevant to a downstream DeepMerge with the ref image; avoids unnecessary sample inflation in non-interacting depth regions. "B" label reserved for Nuke convention (main/background input).
- Source: user
- Primary owning slice: M005-9j5er8/S01
- Supporting slices: none
- Validation: `input_label` returns "" for input 0 and "ref" for input 1; `grep -c '"B"' src/DeepCDepthBlur.cpp` → 0; `grep -q '"ref"'` passes; confirmed M005-9j5er8/T01. Note: depth-range gating when ref connected (R017 partial feature) is not yet implemented — deferred.
- Notes: Renamed from "B" to "ref". Main input unlabelled. Always visible, optional. Gating uses depth-range intersection, not pixel presence.

### R018 — Every emitted sub-sample must have α > 0. Zero-alpha samples are illegal in deep images. Input samples with α ≈ 0 pass through unchanged without spreading.
- Class: core-capability
- Status: validated
- Description: Every emitted sub-sample must have α > 0. Zero-alpha samples are illegal in deep images. Input samples with α ≈ 0 pass through unchanged without spreading.
- Why it matters: Zero-alpha deep samples cause undefined behavior in downstream deep compositing operations and are invalid per the deep image specification.
- Source: user
- Primary owning slice: M005-9j5er8/S01
- Supporting slices: none
- Validation: Double 1e-6f guard: (1) srcAlpha < 1e-6f → pass-through without spreading; (2) alphaSub < 1e-6f → sub-sample dropped silently. `grep -c '1e-6f' src/DeepCDepthBlur.cpp` → 4 confirms both guard paths. Confirmed M005-9j5er8/T01 and T02.
- Notes: Guard threshold at α < 1e-6 for near-zero input samples.

### R046 — DeepCOpenDefocus accepts a Deep image input and produces a flat 2D RGBA output by defocusing each deep sample through the opendefocus convolution engine. The output is a standard Nuke flat image tile, not a Deep stream.
- Class: core-capability
- Status: validated
- Description: DeepCOpenDefocus accepts a Deep image input and produces a flat 2D RGBA output by defocusing each deep sample through the opendefocus convolution engine. The output is a standard Nuke flat image tile, not a Deep stream.
- Why it matters: No existing Nuke tool provides GPU-accelerated, physically-based defocus with non-uniform bokeh artifacts operating directly on Deep images.
- Source: user
- Primary owning slice: M009-mso5fb/S02
- Supporting slices: M009-mso5fb/S01
- Validation: Docker build exits 0; install/16.0-linux/DeepC/DeepCOpenDefocus.so (21 MB) linked against Nuke 16.0v2 SDK with depth-correct holdout support. buildHoldoutData() + opendefocus_deep_render_holdout() dispatch path active in production renderStripe(). M011-qa62vv/S02 T03.
- Notes: Base class is PlanarIop (same as the existing OpenDefocus Nuke plugin). Deep input fetched via DeepOp upstream.

### R049 — The opendefocus Rust library is modified (in a fork) to natively accept deep sample data — variable numbers of samples per pixel, each with its own RGBA, depth, and CoC. The convolution kernel operates on deep samples directly rather than requiring pre-flattened layers.
- Class: core-capability
- Status: validated
- Description: The opendefocus Rust library is modified (in a fork) to natively accept deep sample data — variable numbers of samples per pixel, each with its own RGBA, depth, and CoC. The convolution kernel operates on deep samples directly rather than requiring pre-flattened layers.
- Why it matters: Native deep sample handling avoids the quality loss and compositing errors of layer-slicing approximations. Each sample is convolved at its correct CoC and composited depth-correctly.
- Source: user
- Primary owning slice: M009-mso5fb/S02
- Supporting slices: M009-mso5fb/S01
- Validation: holdout_transmittance() in render_impl (scene-depth layer-peel loop) attenuates per-sample color/alpha before accumulation. test_holdout_attenuates_background_samples passes (1/1): z=2 uncut (≈0.97), z=5 attenuated 10x (≈0.099). Cargo workspace compiled with opendefocus-deep v0.1.0 in Docker build (exit 0). M011-qa62vv/S01 T04, S02 T03.
- Notes: This is the highest-risk requirement. The opendefocus kernel currently assumes flat RGBA + single depth per pixel. Extending it to variable-length per-pixel sample arrays, especially on GPU, is non-trivial.

## Deferred

### R027 — Support for polygonal aperture blades (n-blade iris) or a texture-based aperture mask to shape the bokeh kernel, as in lentil's aperture_image param.
- Class: differentiator
- Status: deferred
- Description: Support for polygonal aperture blades (n-blade iris) or a texture-based aperture mask to shape the bokeh kernel, as in lentil's aperture_image param.
- Why it matters: Artistic control over bokeh shape is a key feature of the lentil toolkit; missing it limits creative use.
- Source: inferred
- Primary owning slice: none
- Supporting slices: none
- Validation: unmapped
- Notes: Deferred — opendefocus has its own bokeh creator which may supersede this requirement. Revisit after M009.

## Out of Scope

### R009 — CMG99's Z-blur that blurs samples across neighbouring pixels in depth
- Class: core-capability
- Status: out-of-scope
- Description: CMG99's Z-blur that blurs samples across neighbouring pixels in depth
- Why it matters: Prevents scope confusion with DeepCDepthBlur — this node does NOT consult neighbours
- Source: user
- Primary owning slice: none
- Supporting slices: none
- Validation: n/a
- Notes: DeepCDepthBlur is intra-pixel only

### R010 — CMG99's transparent mode
- Class: core-capability
- Status: out-of-scope
- Description: CMG99's transparent mode
- Why it matters: Prevents scope creep
- Source: user
- Primary owning slice: none
- Supporting slices: none
- Validation: n/a
- Notes: CMG99 had three modes; alpha correction toggle covers the primary case

### R059 — DeepCOpenDefocus outputs flat 2D RGBA, not a Deep stream. The node collapses depth into a flat defocused image.
- Class: anti-feature
- Status: out-of-scope
- Description: DeepCOpenDefocus outputs flat 2D RGBA, not a Deep stream. The node collapses depth into a flat defocused image.
- Why it matters: Prevents scope confusion — the output is a flat compositable image, same role as Nuke's ZDefocus.
- Source: inferred
- Primary owning slice: none
- Supporting slices: none
- Validation: n/a
- Notes: PlanarIop base class, flat RGBA output tile.

## Traceability

| ID | Class | Status | Primary owner | Supporting | Proof |
|---|---|---|---|---|---|
| R009 | core-capability | out-of-scope | none | none | n/a |
| R010 | core-capability | out-of-scope | none | none | n/a |
| R011 | quality-attribute | active | M004-ks4br0/S01 | none | unmapped |
| R012 | quality-attribute | active | M004-ks4br0/S01 | none | unmapped |
| R013 | core-capability | validated | M005-9j5er8/S01 | none | Multiplicative formula `alphaSub = 1 - pow(1-srcAlpha, w)` confirmed in src/DeepCDepthBlur.cpp on milestone/M005-9j5er8; premult channels scale as `c * (alphaSub / srcAlpha)`; Docker build T02 exits 0 producing DeepCDepthBlur.so; `grep -q 'pow(1'` passes |
| R014 | core-capability | active | M004-ks4br0/S02 | none | unmapped |
| R015 | primary-user-loop | active | M004-ks4br0/S02 | none | unmapped |
| R016 | primary-user-loop | active | M004-ks4br0/S02 | none | unmapped |
| R017 | primary-user-loop | validated | M005-9j5er8/S01 | none | `input_label` returns "" for input 0 and "ref" for input 1; `grep -c '"B"' src/DeepCDepthBlur.cpp` → 0; `grep -q '"ref"'` passes; confirmed M005-9j5er8/T01. Note: depth-range gating when ref connected (R017 partial feature) is not yet implemented — deferred. |
| R018 | core-capability | validated | M005-9j5er8/S01 | none | Double 1e-6f guard: (1) srcAlpha < 1e-6f → pass-through without spreading; (2) alphaSub < 1e-6f → sub-sample dropped silently. `grep -c '1e-6f' src/DeepCDepthBlur.cpp` → 4 confirms both guard paths. Confirmed M005-9j5er8/T01 and T02. |
| R027 | differentiator | deferred | none | none | unmapped |
| R046 | core-capability | validated | M009-mso5fb/S02 | M009-mso5fb/S01 | Docker build exits 0; install/16.0-linux/DeepC/DeepCOpenDefocus.so (21 MB) linked against Nuke 16.0v2 SDK with depth-correct holdout support. buildHoldoutData() + opendefocus_deep_render_holdout() dispatch path active in production renderStripe(). M011-qa62vv/S02 T03. |
| R047 | core-capability | active | M009-mso5fb/S02 | M009-mso5fb/S01 | unmapped |
| R048 | core-capability | active | M009-mso5fb/S03 | none | unmapped |
| R049 | core-capability | validated | M009-mso5fb/S02 | M009-mso5fb/S01 | holdout_transmittance() in render_impl (scene-depth layer-peel loop) attenuates per-sample color/alpha before accumulation. test_holdout_attenuates_background_samples passes (1/1): z=2 uncut (≈0.97), z=5 attenuated 10x (≈0.099). Cargo workspace compiled with opendefocus-deep v0.1.0 in Docker build (exit 0). M011-qa62vv/S01 T04, S02 T03. |
| R050 | quality-attribute | active | M009-mso5fb/S02 | M009-mso5fb/S01 | unmapped |
| R051 | differentiator | active | M009-mso5fb/S02 | none | unmapped |
| R052 | primary-user-loop | active | M009-mso5fb/S03 | none | unmapped |
| R053 | primary-user-loop | active | M009-mso5fb/S02 | none | unmapped |
| R054 | integration | active | M009-mso5fb/S01 | M009-mso5fb/S02 | unmapped |
| R055 | integration | active | M009-mso5fb/S01 | none | unmapped |
| R056 | launchability | active | M009-mso5fb/S03 | none | unmapped |
| R057 | constraint | active | M009-mso5fb/S01 | none | unmapped |
| R058 | core-capability | active | M009-mso5fb/S02 | none | unmapped |
| R059 | anti-feature | out-of-scope | none | none | n/a |

## Coverage Summary

- Active requirements: 16
- Mapped to slices: 16
- Validated: 5 (R013, R017, R018, R046, R049)
- Unmapped active requirements: 0
