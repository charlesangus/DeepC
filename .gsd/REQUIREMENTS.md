# Requirements

This file is the explicit capability and coverage contract for the project.

## Active

### R001 — Separable 2D Gaussian blur
- Class: core-capability
- Status: active
- Description: DeepCBlur must decompose the 2D Gaussian kernel into two sequential 1D passes (horizontal then vertical) for O(2r) instead of O(r²) per pixel
- Why it matters: Performance is unusable at radii above ~10 with the current non-separable approach
- Source: user
- Primary owning slice: M003/S01
- Supporting slices: none
- Validation: docker-build.sh exits 0 (S01); visual parity with non-separable pending human UAT
- Notes: Intermediate buffer holds gathered samples per pixel between passes. Must produce visually equivalent output to the current non-separable kernel at small radii.

### R002 — Kernel accuracy tiers (low/medium/high)
- Class: core-capability
- Status: active
- Description: Enum knob offering three Gaussian kernel computation methods — low (raw unnormalized), medium (normalized to sum=1, current default), high (CDF-based sub-pixel integration)
- Why it matters: Gives artists control over quality/speed tradeoff; high quality eliminates banding at small radii
- Source: user
- Primary owning slice: M003/S01
- Supporting slices: none
- Validation: docker-build.sh exits 0 (S01); visual distinctiveness pending human UAT
- Notes: Algorithms match CMG99's BlurKernels.cpp approach. Medium is default. 1D kernels only (half-kernel stored, symmetric).

### R003 — Alpha darkening correction
- Class: core-capability
- Status: active
- Description: Post-blur correction pass that iterates samples back-to-front per pixel, dividing RGB and alpha by cumulative transparency to counteract the darkening caused by over-compositing blurred deep samples
- Why it matters: Without correction, Gaussian-blurred deep images darken when composited — this is the core visual artifact of naive deep blur
- Source: user
- Primary owning slice: M003/S02
- Supporting slices: none
- Validation: grep -q "cumTransp" exits 0; docker-build.sh exits 0 (S02); visual brightening pending human UAT
- Notes: Algorithm from CMG99's modified gaussian mode. Applied as a post-blur pass, not woven into the blur itself.

### R004 — WH_knob blur size control
- Class: primary-user-loop
- Status: active
- Description: Single blur size knob using DDImage WH_knob with double[2] array, showing width and height components with a lock toggle, replacing the two separate Float_knobs
- Why it matters: Matches Nuke's built-in Blur node convention; more intuitive for artists
- Source: user
- Primary owning slice: M003/S02
- Supporting slices: none
- Validation: grep -c "WH_knob" == 1; grep "Float_knob.*blur" == empty; docker-build.sh exits 0 (S02)
- Notes: Follows DeepCAdjustBBox's existing WH_knob pattern in the codebase.

### R005 — Sample optimization twirldown group
- Class: primary-user-loop
- Status: active
- Description: max_samples, merge_tolerance, and color_tolerance knobs must be inside a BeginClosedGroup twirldown labeled "Sample Optimization"
- Why it matters: Keeps the default UI clean — most artists won't need to touch these
- Source: user
- Primary owning slice: M003/S02
- Supporting slices: none
- Validation: grep -c "BeginClosedGroup" == 1; docker-build.sh exits 0 (S02)
- Notes: Follows DeepThinner's existing BeginClosedGroup pattern.

### R006 — Alpha correction enable/disable knob
- Class: primary-user-loop
- Status: active
- Description: Boolean knob to enable/disable the alpha darkening correction, defaulting to off
- Why it matters: Correction changes the look — artists need explicit control; off by default preserves backward compatibility
- Source: user
- Primary owning slice: M003/S02
- Supporting slices: none
- Validation: grep -c "Bool_knob.*alpha_correction" == 1; docker-build.sh exits 0 (S02)
- Notes: Off by default because it changes visual output from M002.

### R007 — Zero-blur fast path preserved
- Class: quality-attribute
- Status: active
- Description: When blur size is zero in both dimensions, DeepCBlur must pass through input unchanged with no performance penalty
- Why it matters: Prevents unnecessary computation when the node is disabled or animated to zero
- Source: inferred
- Primary owning slice: M003/S01
- Supporting slices: none
- Validation: docker-build.sh exits 0 (S01); fast path code path confirmed present; live test pending human UAT
- Notes: Already exists in current implementation; must be preserved through refactor.

### R008 — Docker build compiles DeepCBlur.so
- Class: quality-attribute
- Status: active
- Description: docker-build.sh must successfully compile DeepCBlur.so and include it in the release archive
- Why it matters: Build verification is the definitive compilation proof for this project
- Source: inferred
- Primary owning slice: M003/S02
- Supporting slices: none
- Validation: docker-build.sh --linux --versions 16.0 exits 0 with DeepCBlur.so in archive (S02)
- Notes: Verified in S02 T01 against Nuke 16.0 SDK.

## Out of Scope

### R009 — Z-blur (depth-direction blur)
- Class: core-capability
- Status: out-of-scope
- Description: Blurring samples along the Z/depth axis, creating volumetric expansion
- Why it matters: Prevents scope creep — CMG99 had this but it's a separate feature
- Source: user
- Primary owning slice: none
- Supporting slices: none
- Validation: n/a
- Notes: Could be added as a future milestone if requested.

### R010 — Transparent modified gaussian mode
- Class: core-capability
- Status: out-of-scope
- Description: CMG99's second blur mode that sets alpha to near-zero and pre-processes RGB for transparent compositing
- Why it matters: Prevents scope creep — the alpha correction mode (R003) covers the primary use case
- Source: user
- Primary owning slice: none
- Supporting slices: none
- Validation: n/a
- Notes: CMG99 had three modes; we're implementing one correction toggle.

## Traceability

| ID | Class | Status | Primary owner | Supporting | Proof |
|---|---|---|---|---|---|
| R001 | core-capability | active | M003/S01 | none | build-verified; visual UAT pending |
| R002 | core-capability | active | M003/S01 | none | build-verified; visual UAT pending |
| R003 | core-capability | active | M003/S02 | none | build-verified; visual UAT pending |
| R004 | primary-user-loop | active | M003/S02 | none | build-verified (grep + docker) |
| R005 | primary-user-loop | active | M003/S02 | none | build-verified (grep + docker) |
| R006 | primary-user-loop | active | M003/S02 | none | build-verified (grep + docker) |
| R007 | quality-attribute | active | M003/S01 | none | build-verified; live test pending |
| R008 | quality-attribute | active | M003/S02 | none | docker-build.sh exits 0, DeepCBlur.so confirmed |

## Coverage Summary

- Active requirements: 8
- Mapped to slices: 8
- Validated: 0 (all build-verified; visual UAT requires human sign-off in Nuke)
- Unmapped active requirements: 0
