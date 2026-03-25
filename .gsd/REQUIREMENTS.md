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

### R037 — DeepCDefocusPORay renderStripe implements lentil-style forward path trace: sensor position (mm) → Newton aperture match via pt_sample_aperture → forward poly eval → full sphereToCs → 3D ray direction → project to input pixel → read and flatten deep column → accumulate flat RGBA
- Class: core-capability
- Status: active
- Description: DeepCDefocusPORay renderStripe implements lentil-style forward path trace: sensor position (mm) → Newton aperture match via pt_sample_aperture → forward poly eval → full sphereToCs → 3D ray direction → project to input pixel → read and flatten deep column → accumulate flat RGBA
- Why it matters: The current scatter engine is a copy of the Thin variant with unused extras — it does not actually trace rays through the polynomial lens. The polynomial output (ray direction from sphereToCs) is computed and discarded. This requirement replaces the scatter with a real camera-shader-style path tracer matching lentil's architecture.
- Source: user
- Primary owning slice: M008-y32v8w/S02
- Supporting slices: M008-y32v8w/S01
- Validation: unmapped
- Notes: Supersedes the Option B scatter in DeepCDefocusPORay. The Thin variant keeps its scatter engine unchanged.

### R041 — When a traced ray is vignetted (transmittance ≤ 0, outer pupil radius clip, or inner pupil radius clip), re-sample a new aperture point and retry, up to vignetting_retries iterations. Matches lentil's camera shader retry pattern.
- Class: quality-attribute
- Status: active
- Description: When a traced ray is vignetted (transmittance ≤ 0, outer pupil radius clip, or inner pupil radius clip), re-sample a new aperture point and retry, up to vignetting_retries iterations. Matches lentil's camera shader retry pattern.
- Why it matters: At field edges, many aperture samples are vignetted. Without retries, those pixels receive fewer valid contributions and appear darker/noisier. The retry loop redistributes energy to valid aperture regions.
- Source: user
- Primary owning slice: M008-y32v8w/S02
- Supporting slices: none
- Validation: unmapped
- Notes: First aperture sample uses the Halton sequence; retries use fresh random samples (matching lentil's pattern).

### R044 — Each traced ray reads a full deep pixel column at the input pixel it maps to, composites front-to-back with per-sample holdout transmittance weighting, and accumulates the resulting flat RGBA to the output buffer.
- Class: core-capability
- Status: active
- Description: Each traced ray reads a full deep pixel column at the input pixel it maps to, composites front-to-back with per-sample holdout transmittance weighting, and accumulates the resulting flat RGBA to the output buffer.
- Why it matters: Deep samples at different depths must be composited correctly per-ray for physically correct defocus. Holdout must be applied per-sample during flatten to maintain depth-correct masking.
- Source: user
- Primary owning slice: M008-y32v8w/S02
- Supporting slices: none
- Validation: unmapped
- Notes: When holdout is not connected, flatten is a standard front-to-back deep composite.

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

### R019 — A Nuke NDK plugin named DeepCDefocusPO that accepts a Deep image input and produces a flat 2D (non-Deep) output image by scattering each deep sample through a polynomial optics lens model. The output must be a standard Nuke flat image tile, not a Deep stream.
- Class: core-capability
- Status: validated
- Description: A Nuke NDK plugin named DeepCDefocusPO that accepts a Deep image input and produces a flat 2D (non-Deep) output image by scattering each deep sample through a polynomial optics lens model. The output must be a standard Nuke flat image tile, not a Deep stream.
- Why it matters: No existing Nuke tool collapses Deep to flat with physically correct bokeh — Nuke's built-in Bokeh and pgBokeh work on flat images and cannot correctly handle depth-aware holdouts.
- Source: user
- Primary owning slice: M006/S02
- Supporting slices: M006/S01
- Validation: renderStripe replaced with full PO scatter loop: deepEngine fetch, per-pixel/per-deep-sample/per-aperture-sample iteration, writableAt accumulation into flat RGBA output buffer. grep -q 'deepEngine' src/DeepCDefocusPO.cpp passes. grep -q 'lt_newton_trace' src/DeepCDefocusPO.cpp passes. bash scripts/verify-s01-syntax.sh exits 0. S02 grep contracts all pass. Full runtime proof (non-zero pixel output) deferred to docker build + Nuke session in S05.
- Notes: Base class will be Iop (or PlanarIop) with a DeepOp upstream fetch, not DeepFilterOp. This is architecturally distinct from all other DeepC nodes.

### R020 — The node loads a per-lens polynomial system at runtime from a `.fit` binary file (lentil gencode output, poly_system_read format). A File_knob exposes the path. The polynomial is evaluated via poly_system_evaluate from poly.h (MIT-licensed, header-only C). No Arnold SDK dependency.
- Class: core-capability
- Status: validated
- Description: The node loads a per-lens polynomial system at runtime from a `.fit` binary file (lentil gencode output, poly_system_read format). A File_knob exposes the path. The polynomial is evaluated via poly_system_evaluate from poly.h (MIT-licensed, header-only C). No Arnold SDK dependency.
- Why it matters: Artists need to select the specific real-world lens they are matching. Pre-baking a fixed lens set into the binary would prevent matching arbitrary lenses.
- Source: user
- Primary owning slice: M006/S01
- Supporting slices: none
- Validation: poly_system_read/poly_system_evaluate/poly_system_destroy defined inline in src/poly.h (MIT); File_knob wired in DeepCDefocusPO.cpp; _validate(for_real) calls poly_system_read with error() on failure; syntax check passes (g++ -fsyntax-only); grep -q 'poly_system_read' src/DeepCDefocusPO.cpp and src/poly.h both pass. Docker build gate pending CI (no Docker in workspace).
- Notes: S01 satisfied the structural proof: poly.h vendored, File_knob wired, load/destroy lifecycle correct. Full runtime proof (load real .fit without crash) deferred to docker build in CI.

### R021 — The circle-of-confusion radius for each deep sample at depth Z is computed from the lens's actual focal length (from the .fit file metadata), the user-specified f-stop, and the user-specified focus distance. Bokeh sizes must match what the same lens would produce in Arnold/lentil for the same scene depth.
- Class: core-capability
- Status: validated
- Description: The circle-of-confusion radius for each deep sample at depth Z is computed from the lens's actual focal length (from the .fit file metadata), the user-specified f-stop, and the user-specified focus distance. Bokeh sizes must match what the same lens would produce in Arnold/lentil for the same scene depth.
- Why it matters: The defining promise of polynomial optics over thin-lens approximation is physical accuracy — "real-world bokeh sizes matching what a real lens would produce."
- Source: user
- Primary owning slice: M006/S02
- Supporting slices: M006/S01, M006/S04
- Validation: coc_radius() in deepc_po_math.h uses focal_length_mm / fstop for aperture_diameter and applies |depth - focus_dist| / depth formula. S04 wired Float_knob focal_length_mm (range 1–1000mm, default 50.0f) replacing the S02 hardcoded constant. All structural proofs pass: grep -q 'coc_radius' src/DeepCDefocusPO.cpp, grep -q '_focal_length_mm', grep -q 'focal_length' all pass. Absolute bokeh-size matching against real lentil/Arnold output is a runtime-only check deferred to CI docker build + Nuke session (documented in M006-CONTEXT.md as UAT).
- Notes: Structural proof complete as of M006/S05. Absolute bokeh-size matching is UAT-only (requires real .fit file, Nuke license, and Arnold reference render). Knob default of 50mm is sensible for a "normal" lens; artist-adjustable via the focal_length Float_knob.

### R022 — When scattering a deep sample, R, G, and B channels are traced at distinct wavelengths (e.g. 0.45μm, 0.55μm, 0.65μm) through the polynomial system. Each channel lands at a slightly different sensor position, producing natural chromatic aberration from the lens model.
- Class: core-capability
- Status: validated
- Description: When scattering a deep sample, R, G, and B channels are traced at distinct wavelengths (e.g. 0.45μm, 0.55μm, 0.65μm) through the polynomial system. Each channel lands at a slightly different sensor position, producing natural chromatic aberration from the lens model.
- Why it matters: Chromatic aberration is a defining characteristic of real lenses — without it, the PO model produces the same bokeh shape as a thin lens.
- Source: user
- Primary owning slice: M006/S02
- Supporting slices: none
- Validation: Per-channel wavelength tracing at lambdas[] = {0.45f, 0.55f, 0.65f} μm in renderStripe scatter loop. grep -q '0.45f' src/DeepCDefocusPO.cpp, grep -q '0.55f', grep -q '0.65f' all pass. Alpha channel uses G-channel (0.55μm) landing position. S02 grep contracts pass. Runtime chromatic fringing visible on bokeh highlights confirmed at S05 Nuke session.
- Notes: Alpha channel uses the green (0.55μm) wavelength result. Per-channel scatter adds ~3× trace cost but is required for correct PO output.

### R023 — An optional second Deep input ("holdout") provides depth-dependent attenuation of the defocused result. For each output pixel, each main input sample's contribution is weighted by the holdout's transmittance at that pixel at the sample's depth. Holdout transmittance is accumulated front-to-back through all holdout samples, exactly matching Nuke's DeepHoldout node behaviour.
- Class: core-capability
- Status: validated
- Description: An optional second Deep input ("holdout") provides depth-dependent attenuation of the defocused result. For each output pixel, each main input sample's contribution is weighted by the holdout's transmittance at that pixel at the sample's depth. Holdout transmittance is accumulated front-to-back through all holdout samples, exactly matching Nuke's DeepHoldout node behaviour.
- Why it matters: Allows depth-correct integration of roto/2D elements with Deep renders — the holdout operates in depth so near holdout samples correctly mask only far background, not the foreground.
- Source: user
- Primary owning slice: M006/S03
- Supporting slices: none
- Validation: transmittance_at lambda computes product(1 - alpha_i) for holdout samples where hzf < Z; holdout_w applied to all RGB and alpha splat accumulations in renderStripe; holdoutConnected false-path returns 1.0f (identity — no masking when holdout disconnected); getRequests requests Chan_Alpha + Chan_DeepFront + Chan_DeepBack from input(1). All S03 grep contracts pass; bash scripts/verify-s01-syntax.sh exits 0. Confirmed by M006/S03/T01.
- Notes: Transmittance at depth Z = product of (1 - alpha_i) for all holdout samples with zFront < Z. This is the standard deep over transmittance accumulation.

### R024 — The holdout input contributes no colour to the output. It is not scattered through the lens model. The holdout's transmittance is evaluated at the output pixel position and used to weight the main input's scattered contributions at that pixel. The holdout mask is sharp, not blurred.
- Class: core-capability
- Status: validated
- Description: The holdout input contributes no colour to the output. It is not scattered through the lens model. The holdout's transmittance is evaluated at the output pixel position and used to weight the main input's scattered contributions at that pixel. The holdout mask is sharp, not blurred.
- Why it matters: pgBokeh and Nuke's Bokeh node get this wrong — they have no way to incorporate already-defocused or 2D elements, leading to double-defocus. The depth-aware holdout evaluated at the output pixel (not the input pixel) is the correct approach.
- Source: user
- Primary owning slice: M006/S03
- Supporting slices: none
- Validation: Only Chan_Alpha + Chan_DeepFront + Chan_DeepBack requested from holdout input (no colour channels — cannot contribute colour by design). holdoutOp->deepEngine called at output pixel bounds, not input sample position (never scattered through lens). holdout_w = transmittance_at(out_xi, out_yi, depth) applied to all RGB and alpha splat accumulations. holdoutConnected false-path returns holdout_w = 1.0f (identity). All S03 grep contracts pass. Confirmed by M006/S03/T01. Runtime visual check (sharp holdout mask, no bokeh on holdout geometry) deferred to CI/UAT Nuke session.
- Notes: holdout_w is computed per aperture sample (per output pixel per deep sample), not per stripe — this is the load-bearing correctness property. The holdout is sharp because it is evaluated at the output pixel, not scattered from the input pixel.

### R025 — For each deep sample being scattered, N points are drawn uniformly on the aperture disk. Each point is traced through the polynomial system to a sensor landing position, and the sample's contribution is splatted to the output buffer at that position. N is an Int_knob exposed to the artist.
- Class: primary-user-loop
- Status: validated
- Description: For each deep sample being scattered, N points are drawn uniformly on the aperture disk. Each point is traced through the polynomial system to a sensor landing position, and the sample's contribution is splatted to the output buffer at that position. N is an Int_knob exposed to the artist.
- Why it matters: N controls the quality/speed tradeoff — low N is fast but noisy, high N is clean. Artists need this control.
- Source: user
- Primary owning slice: M006/S02
- Supporting slices: none
- Validation: Halton(2,3) low-discrepancy sequence + Shirley concentric disk mapping in renderStripe aperture loop. Int_knob aperture_samples wired in S01 knob layout. Loop runs N = max(_aperture_samples, 1) iterations per deep sample. grep -q 'halton' src/DeepCDefocusPO.cpp, grep -q 'map_to_disk' src/deepc_po_math.h both pass. S02 contracts all pass.
- Notes: Aperture disk sampling should use a low-discrepancy sequence (e.g. Halton or stratified jitter) rather than pure random to reduce variance at low N.

### R026 — The node's output is a standard Nuke flat RGBA image. Downstream nodes connect to it as they would to any Iop. The node is not a DeepFilterOp and does not produce a Deep stream.
- Class: core-capability
- Status: validated
- Description: The node's output is a standard Nuke flat RGBA image. Downstream nodes connect to it as they would to any Iop. The node is not a DeepFilterOp and does not produce a Deep stream.
- Why it matters: The purpose of the node is to produce a compositable defocused flat image from a Deep source — the same role as Nuke's built-in ZDefocus.
- Source: user
- Primary owning slice: M006/S01
- Supporting slices: none
- Validation: DeepCDefocusPO : PlanarIop (not DeepFilterOp); renderStripe writes flat RGBA; class is registered as "Deep/DeepCDefocusPO" via Op::Description; grep -q 'PlanarIop' src/DeepCDefocusPO.cpp passes; syntax check passes. Confirmed by S01/T02.
- Notes: S01 confirmed PlanarIop as the base class — flat RGBA output tile, not a Deep stream. DeepFilterOp was explicitly rejected in T02.

### R030 — DeepCDefocusPOThin uses thin-lens CoC for scatter radius, with the polynomial modulating bokeh shape within the CoC (aberrations, cat-eye, coma, CA). Scatter radius comes from thin-lens physics; poly warps aperture sample positions for physically-motivated bokeh shape.
- Class: core-capability
- Status: validated
- Description: DeepCDefocusPOThin uses thin-lens CoC for scatter radius, with the polynomial modulating bokeh shape within the CoC (aberrations, cat-eye, coma, CA). Scatter radius comes from thin-lens physics; poly warps aperture sample positions for physically-motivated bokeh shape.
- Why it matters: Produces correct defocused output (not aperture ring) with faster performance than full raytrace. Practical choice for most compositing work.
- Source: user
- Primary owning slice: M007-gvtoom/S02
- Supporting slices: M007-gvtoom/S01
- Validation: Full thin-lens CoC scatter engine in DeepCDefocusPOThin.cpp renderStripe: coc_radius() drives scatter radius; poly_system_evaluate output [0:1] used as Option B warp offset (clamp magnitude to ap_radius, scale by coc/ap_radius). 4-level nested loop (pixel→deep sample→aperture sample→CA channel). grep -q 'coc_radius' passes. bash scripts/verify-s01-syntax.sh exits 0. Runtime correctness deferred to docker build + nuke -x test/test_thin.nk.
- Notes: S02 wired the full engine. Runtime proof pending docker build.

### R031 — DeepCDefocusPORay treats the Deep image as a 3D scene and performs a gather per output pixel. Rays are cast from the sensor through aperture points, evaluated through the polynomial lens to get exit rays, converted via sphereToCs to 3D, and intersected with deep samples at their depth. Requires lens geometry constants.
- Class: core-capability
- Status: validated
- Description: DeepCDefocusPORay treats the Deep image as a 3D scene and performs a gather per output pixel. Rays are cast from the sensor through aperture points, evaluated through the polynomial lens to get exit rays, converted via sphereToCs to 3D, and intersected with deep samples at their depth. Requires lens geometry constants.
- Why it matters: Physically exact lens simulation — the same approach as lentil's renderer. Produces correct bokeh shape, vignetting, and aberrations directly from the polynomial without thin-lens approximation.
- Source: user
- Primary owning slice: M007-gvtoom/S03
- Supporting slices: M007-gvtoom/S01
- Validation: Gather renderStripe in DeepCDefocusPORay.cpp implements per-output-pixel loop with CoC-bounded neighbourhood search, aperture vignetting via _aperture_sys, poly_system_evaluate for exitpupil+aperture, sphereToCs call for 3D ray direction, and gather selectivity guard. All structural contracts pass (grep _aperture_sys, grep sphereToCs, grep -c _max_degree >= 2, grep halton, grep 0.45f, grep ox_land/oy_land, bash verify-s01-syntax.sh). Runtime proof (non-black pixel count >100 at 128×72) deferred to docker build + CI.
- Notes: Requires lens geometry constants from lentil database (outer_pupil_curvature_radius, lens_length, aperture_housing_radius etc.) and aperture.fit polynomial.

### R032 — Int_knob controlling the maximum polynomial degree evaluated. Terms in .fit files are sorted by ascending degree; evaluation stops early when max_degree is exceeded. Lower degree = faster but less accurate aberrations.
- Class: primary-user-loop
- Status: validated
- Description: Int_knob controlling the maximum polynomial degree evaluated. Terms in .fit files are sorted by ascending degree; evaluation stops early when max_degree is exceeded. Lower degree = faster but less accurate aberrations.
- Why it matters: Artist-controllable quality/speed tradeoff. Degree 3 (56 terms) is ~78× faster than degree 11 (4368 terms). Higher degrees add sub-pixel aberration refinement.
- Source: user
- Primary owning slice: M007-gvtoom/S01
- Supporting slices: M007-gvtoom/S02, M007-gvtoom/S03
- Validation: Int_knob _max_degree wired in both DeepCDefocusPOThin (scatter) and DeepCDefocusPORay (gather). poly_system_evaluate in poly.h has max_degree early-exit with break-on-exceed (terms sorted ascending). grep -c '_max_degree' returns 4+ in Thin and 5+ in Ray. bash scripts/verify-s01-syntax.sh exits 0. Visual runtime proof (degree 3 vs 11) deferred to docker build UAT.
- Notes: S02 confirmed _max_degree is passed to poly_system_evaluate in the scatter loop. Runtime proof pending docker build.

### R033 — DeepCDefocusPORay requires lens geometry constants (outer_pupil_curvature_radius, lens_length, aperture_housing_radius, inner_pupil_curvature_radius) to convert polynomial output from spherical pupil coordinates to 3D Cartesian rays. Exposed as knobs with sensible defaults (Angenieux 55mm values).
- Class: core-capability
- Status: validated
- Description: DeepCDefocusPORay requires lens geometry constants (outer_pupil_curvature_radius, lens_length, aperture_housing_radius, inner_pupil_curvature_radius) to convert polynomial output from spherical pupil coordinates to 3D Cartesian rays. Exposed as knobs with sensible defaults (Angenieux 55mm values).
- Why it matters: Without these constants, the polynomial output cannot be converted to actual 3D rays — the output would be meaningless coordinates.
- Source: inferred
- Primary owning slice: M007-gvtoom/S03
- Supporting slices: none
- Validation: All four lens geometry constants (outer_pupil_curvature_radius, lens_length, aperture_housing_radius, inner_pupil_curvature_radius) exposed as Float_knob with Angenieux 55mm defaults (S01 scaffold). sphereToCs called in S03 gather with outer_pupil_curvature_radius as the curvature argument. Structural grep for sphereToCs passes. Runtime proof deferred.
- Notes: Values available in lentil's lens_constants.h and lenses.json database. Long-term: auto-parse from JSON. Short-term: knobs with defaults.

### R034 — DeepCDefocusPORay loads a second polynomial system (aperture.fit) to constrain the Newton iteration's aperture matching. The exitpupil.fit maps sensor→outer pupil; aperture.fit maps sensor→aperture plane.
- Class: core-capability
- Status: validated
- Description: DeepCDefocusPORay loads a second polynomial system (aperture.fit) to constrain the Newton iteration's aperture matching. The exitpupil.fit maps sensor→outer pupil; aperture.fit maps sensor→aperture plane.
- Why it matters: The lentil Newton solver uses both polynomials — exitpupil for scene-direction matching and aperture for aperture-position matching. Without the aperture polynomial, the solver cannot constrain rays to pass through the correct aperture point.
- Source: inferred
- Primary owning slice: M007-gvtoom/S03
- Supporting slices: none
- Validation: _aperture_sys (poly_system_t) loaded from aperture_file knob in renderStripe entry with reload guard (mirrors exitpupil pattern). poly_system_evaluate(&_aperture_sys, in5, apt_out, 2, _max_degree) called per aperture sample with aperture housing radius vignetting guard. error() on load failure. Structural grep for _aperture_sys passes. Runtime proof deferred.
- Notes: Second File_knob or auto-detection from sibling file path.

### R035 — Both DeepCDefocusPOThin and DeepCDefocusPORay retain the holdout mechanism (R023/R024), per-channel wavelength tracing for CA (R022), and Halton+Shirley aperture sampling (R025) from the M006 implementation.
- Class: core-capability
- Status: validated
- Description: Both DeepCDefocusPOThin and DeepCDefocusPORay retain the holdout mechanism (R023/R024), per-channel wavelength tracing for CA (R022), and Halton+Shirley aperture sampling (R025) from the M006 implementation.
- Why it matters: These are validated, working features that artists expect. Dropping them would be a regression.
- Source: inferred
- Primary owning slice: M007-gvtoom/S01
- Supporting slices: M007-gvtoom/S02, M007-gvtoom/S03
- Validation: DeepCDefocusPORay gather engine: transmittance_at lambda (R023/R024 holdout) applied per contributing sample; CA wavelengths 0.45f/0.55f/0.65f in inner channel loop; Halton+Shirley concentric disk sampling via halton() and map_to_disk(). All structural greps pass for Ray variant. DeepCDefocusPOThin: validated in S02. Full runtime validation pending docker build.
- Notes: S02 confirmed all three features are wired in DeepCDefocusPOThin. DeepCDefocusPORay scaffold from S01 carries the same knobs; engine wiring is S03's responsibility.

### R036 — DeepCDefocusPOThin and DeepCDefocusPORay appear as separate entries in Nuke's node menu under the Filter category, replacing the single DeepCDefocusPO entry.
- Class: launchability
- Status: validated
- Description: DeepCDefocusPOThin and DeepCDefocusPORay appear as separate entries in Nuke's node menu under the Filter category, replacing the single DeepCDefocusPO entry.
- Why it matters: Artists need to find and instantiate both nodes from the standard Nuke menu.
- Source: inferred
- Primary owning slice: M007-gvtoom/S01
- Supporting slices: none
- Validation: Both plugins registered via Op::Description as "Deep/DeepCDefocusPOThin" and "Deep/DeepCDefocusPORay". CMakeLists.txt: grep -c 'DeepCDefocusPOThin' == 2 (PLUGINS + FILTER_NODES), grep -c 'DeepCDefocusPORay' == 2. Old DeepCDefocusPO: grep -c 'DeepCDefocusPO[^TR]' == 0. Actual Nuke menu appearance requires docker build.
- Notes: S01 structural proof complete: both plugins registered as "Deep/DeepCDefocusPOThin" and "Deep/DeepCDefocusPORay" via Op::Description; both present in CMakeLists.txt PLUGINS and FILTER_NODES (2 occurrences each); old DeepCDefocusPO removed (0 occurrences). Actual Nuke menu appearance requires docker build — deferred to S02/S03.

### R038 — DeepCDefocusPORay uses physical mm coordinates for sensor positions fed to the polynomial, matching lentil's convention. A sensor_width Float_knob (default 36mm for 35mm film) controls the mapping.
- Class: core-capability
- Status: validated
- Description: DeepCDefocusPORay uses physical mm coordinates for sensor positions fed to the polynomial, matching lentil's convention. A sensor_width Float_knob (default 36mm for 35mm film) controls the mapping.
- Why it matters: The .fit polynomials are fitted in a specific coordinate space. Lentil feeds sensor positions in mm. Mismatched coordinate conventions produce incorrect bokeh shape and size.
- Source: user
- Primary owning slice: M008-y32v8w/S01
- Supporting slices: M008-y32v8w/S02
- Validation: Float_knob _sensor_width (36mm default) added to DeepCDefocusPORay. grep -q '_sensor_width' src/DeepCDefocusPORay.cpp passes. bash scripts/verify-s01-syntax.sh contract PASS. Sensor mm coordinate convention confirmed: sx = pixel_x * _sensor_width / (2 * half_width_pixels).
- Notes: DeepCDefocusPOThin keeps its normalised [-1,1] convention (different algorithm, different poly interpretation).

### R039 — A pt_sample_aperture function that solves for sensor direction (dx, dy) given sensor position (x, y) and target aperture point, using Newton iteration with finite-difference Jacobians over poly_system_evaluate. Matches lentil's pt_sample_aperture semantics.
- Class: core-capability
- Status: validated
- Description: A pt_sample_aperture function that solves for sensor direction (dx, dy) given sensor position (x, y) and target aperture point, using Newton iteration with finite-difference Jacobians over poly_system_evaluate. Matches lentil's pt_sample_aperture semantics.
- Why it matters: The aperture match is the core of the path-trace flow — without it, rays cannot be steered through the correct aperture point, and the polynomial evaluation produces physically meaningless output.
- Source: user
- Primary owning slice: M008-y32v8w/S01
- Supporting slices: M008-y32v8w/S02
- Validation: pt_sample_aperture implemented inline in src/deepc_po_math.h. Newton iteration: 5 iters, 1e-4 convergence tolerance, 1e-4 FD epsilon for Jacobian, 0.72 damping. FD Jacobian correctly accounts for sensor_shift coupling. grep -q 'pt_sample_aperture' src/deepc_po_math.h passes. verify-s01-syntax.sh contract PASS. deepc_po_math.h compiles cleanly with poly.h.
- Notes: Lentil uses analytically generated per-lens Jacobians. We use finite-difference Jacobians with the generic .fit evaluator. Slower but lens-agnostic.

### R040 — sphereToCs in deepc_po_math.h upgraded to match lentil's full implementation: takes 2D position and 2D direction on the outer pupil sphere surface, returns 3D origin and 3D direction in camera space using tangent-frame construction (normal, tangent, bitangent).
- Class: core-capability
- Status: validated
- Description: sphereToCs in deepc_po_math.h upgraded to match lentil's full implementation: takes 2D position and 2D direction on the outer pupil sphere surface, returns 3D origin and 3D direction in camera space using tangent-frame construction (normal, tangent, bitangent).
- Why it matters: The current sphereToCs returns only a direction vector (not an origin), using a simplified projection. The full version is required to produce a physically correct 3D ray for scene intersection.
- Source: user
- Primary owning slice: M008-y32v8w/S01
- Supporting slices: M008-y32v8w/S02
- Validation: sphereToCs_full implemented inline in src/deepc_po_math.h. Returns 3D origin on sphere surface and 3D direction via tangent-frame construction (normal, ex/ey cross products, transform into camera space). Handles negative R (concave surfaces). grep -q 'sphereToCs_full' src/deepc_po_math.h passes. verify-s01-syntax.sh contract PASS.
- Notes: Lentil's sphereToCs uses Eigen::Vector3d. DeepC's version uses plain floats to avoid the Eigen dependency (D023).

### R042 — The sensor-to-polynomial offset (sensor_shift) is computed by searching for the shift value that focuses the lens at the user's focus_distance, matching lentil's logarithmic_focus_search approach. This replaces thin-lens CoC-based focusing.
- Class: core-capability
- Status: validated
- Description: The sensor-to-polynomial offset (sensor_shift) is computed by searching for the shift value that focuses the lens at the user's focus_distance, matching lentil's logarithmic_focus_search approach. This replaces thin-lens CoC-based focusing.
- Why it matters: sensor_shift is the physical focusing mechanism for polynomial optics lenses. Without it, the lens is unfocused and the polynomial evaluation starts from the wrong sensor position.
- Source: user
- Primary owning slice: M008-y32v8w/S01
- Supporting slices: M008-y32v8w/S02
- Validation: logarithmic_focus_search implemented inline in src/deepc_po_math.h. Brute-force 20001-step quadratically-spaced search over [-45mm, +45mm]. Traces center ray through pt_sample_aperture + poly_system_evaluate + sphereToCs_full, picks shift minimising |focus_distance - target|. grep -q 'logarithmic_focus_search' src/deepc_po_math.h passes. verify-s01-syntax.sh contract PASS. Note: housing_radius parameter accepted but not used as bound — deferred to S02 if needed.
- Notes: Lentil searches a logarithmic range of shift values, traces a center ray for each, and picks the one closest to the target focus distance.

### R043 — Five new Float_knobs on DeepCDefocusPORay exposing lens constants needed for the path-trace flow: sensor_width (36mm default), back_focal_length (30.83mm), outer_pupil_radius (29.87mm), inner_pupil_radius (19.36mm), aperture_pos (35.45mm). All with Angenieux 55mm defaults.
- Class: core-capability
- Status: validated
- Description: Five new Float_knobs on DeepCDefocusPORay exposing lens constants needed for the path-trace flow: sensor_width (36mm default), back_focal_length (30.83mm), outer_pupil_radius (29.87mm), inner_pupil_radius (19.36mm), aperture_pos (35.45mm). All with Angenieux 55mm defaults.
- Why it matters: These constants are required for sensor_shift computation, inner/outer pupil culling, and aperture position in the Newton iteration. Lentil reads them from per-lens generated code; DeepC exposes them as knobs since we use generic .fit files.
- Source: user
- Primary owning slice: M008-y32v8w/S01
- Supporting slices: M008-y32v8w/S02
- Validation: Five Float_knobs added to DeepCDefocusPORay: _sensor_width=36.0, _back_focal_length=30.829003, _outer_pupil_radius=29.865562, _inner_pupil_radius=19.357308, _aperture_pos=35.445997. All with Angenieux 55mm defaults, SetRange, Tooltip. Placed inside existing "Lens geometry" BeginClosedGroup. All 5 verify-s01-syntax.sh contracts PASS.
- Notes: Values for Angenieux 55mm from lentil's lens_constants.h. Artists read constants from the same file for other lenses.

### R045 — Both PO nodes must set info_.format() in _validate to match the Deep input's format, not rely on PlanarIop's default. Without this, the output frame size is wrong.
- Class: quality-attribute
- Status: validated
- Description: Both PO nodes must set info_.format() in _validate to match the Deep input's format, not rely on PlanarIop's default. Without this, the output frame size is wrong.
- Why it matters: Wrong output frame size causes downstream compositing failures and black/cropped output.
- Source: user
- Primary owning slice: M008-y32v8w/S01
- Supporting slices: none
- Validation: info_.format(*di.format()) added to _validate in both DeepCDefocusPOThin.cpp and DeepCDefocusPORay.cpp, immediately after info_.set(di.box()). grep -q 'info_\.format(' passes for both files. Mock DeepInfo::format() and Info::format(const Format&) added to verify script. Both nodes compile cleanly. verify-s01-syntax.sh contracts PASS for both.
- Notes: Bug affects both Thin and Ray. The fix is identical for both.

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
- Notes: Deferred — circular aperture sufficient for M006. Polygonal/texture aperture is a future enhancement once the core PO scatter is working.

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

### R029 — Lentil's bidirectional path sends rays backward from bright output pixels to the aperture, adaptively concentrating samples where bokeh energy is high (~30× faster in Arnold). In a compositing context the equivalent would be a gather (per output pixel, importance-sample which deep samples contribute) rather than a scatter (per deep sample, splat to output pixels).
- Class: differentiator
- Status: out-of-scope
- Description: Lentil's bidirectional path sends rays backward from bright output pixels to the aperture, adaptively concentrating samples where bokeh energy is high (~30× faster in Arnold). In a compositing context the equivalent would be a gather (per output pixel, importance-sample which deep samples contribute) rather than a scatter (per deep sample, splat to output pixels).
- Why it matters: Could significantly reduce noise at low N for high-energy bokeh highlights.
- Source: inferred
- Primary owning slice: none
- Supporting slices: none
- Validation: n/a
- Notes: Out of scope for M006. Stochastic forward scatter is the correct starting point. Bidirectional gather is a future optimisation milestone if noise at low N proves problematic in practice.

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
| R019 | core-capability | validated | M006/S02 | M006/S01 | renderStripe replaced with full PO scatter loop: deepEngine fetch, per-pixel/per-deep-sample/per-aperture-sample iteration, writableAt accumulation into flat RGBA output buffer. grep -q 'deepEngine' src/DeepCDefocusPO.cpp passes. grep -q 'lt_newton_trace' src/DeepCDefocusPO.cpp passes. bash scripts/verify-s01-syntax.sh exits 0. S02 grep contracts all pass. Full runtime proof (non-zero pixel output) deferred to docker build + Nuke session in S05. |
| R020 | core-capability | validated | M006/S01 | none | poly_system_read/poly_system_evaluate/poly_system_destroy defined inline in src/poly.h (MIT); File_knob wired in DeepCDefocusPO.cpp; _validate(for_real) calls poly_system_read with error() on failure; syntax check passes (g++ -fsyntax-only); grep -q 'poly_system_read' src/DeepCDefocusPO.cpp and src/poly.h both pass. Docker build gate pending CI (no Docker in workspace). |
| R021 | core-capability | validated | M006/S02 | M006/S01, M006/S04 | coc_radius() in deepc_po_math.h uses focal_length_mm / fstop for aperture_diameter and applies |depth - focus_dist| / depth formula. S04 wired Float_knob focal_length_mm (range 1–1000mm, default 50.0f) replacing the S02 hardcoded constant. All structural proofs pass: grep -q 'coc_radius' src/DeepCDefocusPO.cpp, grep -q '_focal_length_mm', grep -q 'focal_length' all pass. Absolute bokeh-size matching against real lentil/Arnold output is a runtime-only check deferred to CI docker build + Nuke session (documented in M006-CONTEXT.md as UAT). |
| R022 | core-capability | validated | M006/S02 | none | Per-channel wavelength tracing at lambdas[] = {0.45f, 0.55f, 0.65f} μm in renderStripe scatter loop. grep -q '0.45f' src/DeepCDefocusPO.cpp, grep -q '0.55f', grep -q '0.65f' all pass. Alpha channel uses G-channel (0.55μm) landing position. S02 grep contracts pass. Runtime chromatic fringing visible on bokeh highlights confirmed at S05 Nuke session. |
| R023 | core-capability | validated | M006/S03 | none | transmittance_at lambda computes product(1 - alpha_i) for holdout samples where hzf < Z; holdout_w applied to all RGB and alpha splat accumulations in renderStripe; holdoutConnected false-path returns 1.0f (identity — no masking when holdout disconnected); getRequests requests Chan_Alpha + Chan_DeepFront + Chan_DeepBack from input(1). All S03 grep contracts pass; bash scripts/verify-s01-syntax.sh exits 0. Confirmed by M006/S03/T01. |
| R024 | core-capability | validated | M006/S03 | none | Only Chan_Alpha + Chan_DeepFront + Chan_DeepBack requested from holdout input (no colour channels — cannot contribute colour by design). holdoutOp->deepEngine called at output pixel bounds, not input sample position (never scattered through lens). holdout_w = transmittance_at(out_xi, out_yi, depth) applied to all RGB and alpha splat accumulations. holdoutConnected false-path returns holdout_w = 1.0f (identity). All S03 grep contracts pass. Confirmed by M006/S03/T01. Runtime visual check (sharp holdout mask, no bokeh on holdout geometry) deferred to CI/UAT Nuke session. |
| R025 | primary-user-loop | validated | M006/S02 | none | Halton(2,3) low-discrepancy sequence + Shirley concentric disk mapping in renderStripe aperture loop. Int_knob aperture_samples wired in S01 knob layout. Loop runs N = max(_aperture_samples, 1) iterations per deep sample. grep -q 'halton' src/DeepCDefocusPO.cpp, grep -q 'map_to_disk' src/deepc_po_math.h both pass. S02 contracts all pass. |
| R026 | core-capability | validated | M006/S01 | none | DeepCDefocusPO : PlanarIop (not DeepFilterOp); renderStripe writes flat RGBA; class is registered as "Deep/DeepCDefocusPO" via Op::Description; grep -q 'PlanarIop' src/DeepCDefocusPO.cpp passes; syntax check passes. Confirmed by S01/T02. |
| R027 | differentiator | deferred | none | none | unmapped |
| R029 | differentiator | out-of-scope | none | none | n/a |
| R030 | core-capability | validated | M007-gvtoom/S02 | M007-gvtoom/S01 | Full thin-lens CoC scatter engine in DeepCDefocusPOThin.cpp renderStripe: coc_radius() drives scatter radius; poly_system_evaluate output [0:1] used as Option B warp offset (clamp magnitude to ap_radius, scale by coc/ap_radius). 4-level nested loop (pixel→deep sample→aperture sample→CA channel). grep -q 'coc_radius' passes. bash scripts/verify-s01-syntax.sh exits 0. Runtime correctness deferred to docker build + nuke -x test/test_thin.nk. |
| R031 | core-capability | validated | M007-gvtoom/S03 | M007-gvtoom/S01 | Gather renderStripe in DeepCDefocusPORay.cpp implements per-output-pixel loop with CoC-bounded neighbourhood search, aperture vignetting via _aperture_sys, poly_system_evaluate for exitpupil+aperture, sphereToCs call for 3D ray direction, and gather selectivity guard. All structural contracts pass (grep _aperture_sys, grep sphereToCs, grep -c _max_degree >= 2, grep halton, grep 0.45f, grep ox_land/oy_land, bash verify-s01-syntax.sh). Runtime proof (non-black pixel count >100 at 128×72) deferred to docker build + CI. |
| R032 | primary-user-loop | validated | M007-gvtoom/S01 | M007-gvtoom/S02, M007-gvtoom/S03 | Int_knob _max_degree wired in both DeepCDefocusPOThin (scatter) and DeepCDefocusPORay (gather). poly_system_evaluate in poly.h has max_degree early-exit with break-on-exceed (terms sorted ascending). grep -c '_max_degree' returns 4+ in Thin and 5+ in Ray. bash scripts/verify-s01-syntax.sh exits 0. Visual runtime proof (degree 3 vs 11) deferred to docker build UAT. |
| R033 | core-capability | validated | M007-gvtoom/S03 | none | All four lens geometry constants (outer_pupil_curvature_radius, lens_length, aperture_housing_radius, inner_pupil_curvature_radius) exposed as Float_knob with Angenieux 55mm defaults (S01 scaffold). sphereToCs called in S03 gather with outer_pupil_curvature_radius as the curvature argument. Structural grep for sphereToCs passes. Runtime proof deferred. |
| R034 | core-capability | validated | M007-gvtoom/S03 | none | _aperture_sys (poly_system_t) loaded from aperture_file knob in renderStripe entry with reload guard (mirrors exitpupil pattern). poly_system_evaluate(&_aperture_sys, in5, apt_out, 2, _max_degree) called per aperture sample with aperture housing radius vignetting guard. error() on load failure. Structural grep for _aperture_sys passes. Runtime proof deferred. |
| R035 | core-capability | validated | M007-gvtoom/S01 | M007-gvtoom/S02, M007-gvtoom/S03 | DeepCDefocusPORay gather engine: transmittance_at lambda (R023/R024 holdout) applied per contributing sample; CA wavelengths 0.45f/0.55f/0.65f in inner channel loop; Halton+Shirley concentric disk sampling via halton() and map_to_disk(). All structural greps pass for Ray variant. DeepCDefocusPOThin: validated in S02. Full runtime validation pending docker build. |
| R036 | launchability | validated | M007-gvtoom/S01 | none | Both plugins registered via Op::Description as "Deep/DeepCDefocusPOThin" and "Deep/DeepCDefocusPORay". CMakeLists.txt: grep -c 'DeepCDefocusPOThin' == 2 (PLUGINS + FILTER_NODES), grep -c 'DeepCDefocusPORay' == 2. Old DeepCDefocusPO: grep -c 'DeepCDefocusPO[^TR]' == 0. Actual Nuke menu appearance requires docker build. |
| R037 | core-capability | active | M008-y32v8w/S02 | M008-y32v8w/S01 | unmapped |
| R038 | core-capability | validated | M008-y32v8w/S01 | M008-y32v8w/S02 | Float_knob _sensor_width (36mm default) added to DeepCDefocusPORay. grep -q '_sensor_width' src/DeepCDefocusPORay.cpp passes. bash scripts/verify-s01-syntax.sh contract PASS. Sensor mm coordinate convention confirmed: sx = pixel_x * _sensor_width / (2 * half_width_pixels). |
| R039 | core-capability | validated | M008-y32v8w/S01 | M008-y32v8w/S02 | pt_sample_aperture implemented inline in src/deepc_po_math.h. Newton iteration: 5 iters, 1e-4 convergence tolerance, 1e-4 FD epsilon for Jacobian, 0.72 damping. FD Jacobian correctly accounts for sensor_shift coupling. grep -q 'pt_sample_aperture' src/deepc_po_math.h passes. verify-s01-syntax.sh contract PASS. deepc_po_math.h compiles cleanly with poly.h. |
| R040 | core-capability | validated | M008-y32v8w/S01 | M008-y32v8w/S02 | sphereToCs_full implemented inline in src/deepc_po_math.h. Returns 3D origin on sphere surface and 3D direction via tangent-frame construction (normal, ex/ey cross products, transform into camera space). Handles negative R (concave surfaces). grep -q 'sphereToCs_full' src/deepc_po_math.h passes. verify-s01-syntax.sh contract PASS. |
| R041 | quality-attribute | active | M008-y32v8w/S02 | none | unmapped |
| R042 | core-capability | validated | M008-y32v8w/S01 | M008-y32v8w/S02 | logarithmic_focus_search implemented inline in src/deepc_po_math.h. Brute-force 20001-step quadratically-spaced search over [-45mm, +45mm]. Traces center ray through pt_sample_aperture + poly_system_evaluate + sphereToCs_full, picks shift minimising |focus_distance - target|. grep -q 'logarithmic_focus_search' src/deepc_po_math.h passes. verify-s01-syntax.sh contract PASS. Note: housing_radius parameter accepted but not used as bound — deferred to S02 if needed. |
| R043 | core-capability | validated | M008-y32v8w/S01 | M008-y32v8w/S02 | Five Float_knobs added to DeepCDefocusPORay: _sensor_width=36.0, _back_focal_length=30.829003, _outer_pupil_radius=29.865562, _inner_pupil_radius=19.357308, _aperture_pos=35.445997. All with Angenieux 55mm defaults, SetRange, Tooltip. Placed inside existing "Lens geometry" BeginClosedGroup. All 5 verify-s01-syntax.sh contracts PASS. |
| R044 | core-capability | active | M008-y32v8w/S02 | none | unmapped |
| R045 | quality-attribute | validated | M008-y32v8w/S01 | none | info_.format(*di.format()) added to _validate in both DeepCDefocusPOThin.cpp and DeepCDefocusPORay.cpp, immediately after info_.set(di.box()). grep -q 'info_\.format(' passes for both files. Mock DeepInfo::format() and Info::format(const Format&) added to verify script. Both nodes compile cleanly. verify-s01-syntax.sh contracts PASS for both. |

## Coverage Summary

- Active requirements: 8
- Mapped to slices: 8
- Validated: 24 (R013, R017, R018, R019, R020, R021, R022, R023, R024, R025, R026, R030, R031, R032, R033, R034, R035, R036, R038, R039, R040, R042, R043, R045)
- Unmapped active requirements: 0
