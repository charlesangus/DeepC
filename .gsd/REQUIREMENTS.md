# Requirements

This file is the explicit capability and coverage contract for the project.

## Active

### R011 — Unpremultiplied colour comparison in sample optimizer
- Class: quality-attribute
- Status: validated
- Description: `colorDistance` in `DeepSampleOptimizer.h` must unpremultiply channel values by alpha before comparing, with a near-zero alpha guard (treat transparent samples as always-matching)
- Why it matters: Premultiplied comparison causes samples from the same surface at different Gaussian weights to appear as different colours, preventing correct merges and producing jaggy blur output
- Source: user
- Primary owning slice: M004-ks4br0/S01
- Supporting slices: none
- Validation: docker-build.sh exit 0 (Nuke 16.0); grep confirms 4-arg signature; syntax check passes; visual jaggy elimination pending human UAT

- Notes: Fix is in the shared header — both DeepCBlur and DeepCBlur2 benefit automatically

### R012 — Overlapping sample tidy pre-pass in optimizer
- Class: quality-attribute
- Status: validated
- Description: `optimizeSamples` must include a pre-pass that detects overlapping depth intervals (accounting for volumetric zFront/zBack ranges), splits them at boundaries using volumetric alpha subdivision, and over-merges front-to-back before the existing tolerance-based merge
- Why it matters: Overlapping samples produce incorrect compositing results; same-depth-range duplicates from blur gather should always collapse regardless of tolerance settings
- Source: user
- Primary owning slice: M004-ks4br0/S01
- Supporting slices: none
- Validation: grep confirms exactly 1 tidyOverlapping(samples) call; docker-build.sh exit 0; code inspection confirms pow-based alpha subdivision formula

### R013 — DeepCDepthBlur flatten invariant
- Class: core-capability
- Status: validated
- Description: `flatten(DeepCDepthBlur(input)) == flatten(input)` — the node redistributes each sample's alpha across sub-samples whose weights sum to 1; it does not add new alpha
- Why it matters: The visual result must be identical to the input when composited — the node only softens holdouts by spreading samples in Z, not by changing the flat image
- Source: user
- Primary owning slice: M004-ks4br0/S02
- Supporting slices: none
- Validation: Structural proof: weight generators normalise to sum=1 (verified for all 4 modes × 7 sample counts); channel scaling is proportional (premult preserved); tidy clamp modifies only zBack, not alpha. Pending: live Nuke visual confirmation (flatten A==flatten B in viewer).
- Notes: Channels scale with weight proportionally (premult preserved)

### R014 — DeepCDepthBlur tidy output
- Class: core-capability
- Status: validated
- Description: Output samples are sorted by zFront, non-overlapping (zBack[i] ≤ zFront[i+1]), and each sample has zFront ≤ zBack
- Why it matters: "Tidy" is required for correct downstream deep compositing; overlapping samples produce incorrect DeepMerge results
- Source: user
- Primary owning slice: M004-ks4br0/S02
- Supporting slices: none
- Validation: Code inspection confirms std::sort by zFront then walk-and-clamp zBack[i]=min(zBack[i], zFront[i+1]). Docker build exit 0 confirms this code ships.
- Notes: Final clamp pass: zBack[i] = min(zBack[i], zFront[i+1])

### R015 — DeepCDepthBlur falloff modes
- Class: primary-user-loop
- Status: validated
- Description: Enum knob with four modes: Linear, Gaussian, Smoothstep (smoothstep curve), Exponential. All weights normalised to sum to 1 before distributing alpha.
- Why it matters: Different falloffs produce different softness characters; smoothstep is the natural choice for soft holdout edges, gaussian for organic volumes
- Source: user
- Primary owning slice: M004-ks4br0/S02
- Supporting slices: none
- Validation: All four static weight generators present in source; weight normalisation verified for all 4 modes × {1,2,3,5,10,32,64} sample counts including n=2 degenerate case; docker build exit 0.
- Notes: Smoothstep confirmed as the "smooth" mode in discussion

### R016 — DeepCDepthBlur spread, num-samples, and sample-type controls
- Class: primary-user-loop
- Status: validated
- Description: Float knob for spread depth extent; int knob for number of sub-samples per input sample; enum knob for Volumetric vs Flat/point output sample type
- Why it matters: Artists need control over how coarse or fine the depth spread is and whether spread samples cover depth ranges (volumetric, physically correct for volumes) or are point samples (flat, better for hard surfaces)
- Source: user
- Primary owning slice: M004-ks4br0/S02
- Supporting slices: none
- Validation: All four knobs (spread, num_samples, falloff, sample_type) grep confirmed in source; _validate clamp logic present; docker build exit 0.
- Notes: Volumetric sub-samples cover evenly-spaced sub-ranges; flat sub-samples have zFront==zBack at evenly-spaced depths

### R017 — DeepCDepthBlur optional second input with depth-range gating
- Class: primary-user-loop
- Status: validated
- Description: Optional B input (second deep image); when connected, only spread samples whose depth range [zFront-spread, zBack+spread] intersects any B sample's [zFront_B, zBack_B]; other samples pass through unchanged
- Why it matters: Constrains depth spreading to only depths relevant to a downstream DeepMerge with the B image; avoids unnecessary sample inflation in non-interacting depth regions
- Source: user
- Primary owning slice: M004-ks4br0/S02
- Supporting slices: none
- Validation: inputs(2)+minimum_inputs/maximum_inputs+dynamic_cast+null-check pattern confirmed in source; edge cases (B null, B pixel empty, B fetch fail) handled via graceful fallback; docker build exit 0.
- Notes: Gating uses depth-range intersection, not pixel presence

- Notes: Point samples (zFront==zBack) at identical depth are also collapsed. Existing tolerance merge runs after this pre-pass.

## Validated

(Previous requirements R001–R008 from M003 moved to validated — see M003-SUMMARY.md)

### R011 — Unpremultiplied colour comparison in sample optimizer (validated by S01/T01)
### R012 — Overlapping sample tidy pre-pass in optimizer (validated by S01/T01)

## Out of Scope

### R009 — Z-blur (CMG99 style depth-direction blur)
- Class: core-capability
- Status: out-of-scope
- Description: CMG99's Z-blur that blurs samples across neighbouring pixels in depth
- Why it matters: Prevents scope confusion with DeepCDepthBlur — this node does NOT consult neighbours
- Source: user
- Primary owning slice: none
- Supporting slices: none
- Validation: n/a
- Notes: DeepCDepthBlur is intra-pixel only

### R010 — Transparent modified gaussian mode
- Class: core-capability
- Status: out-of-scope
- Description: CMG99's transparent mode
- Why it matters: Prevents scope creep
- Source: user
- Primary owning slice: none
- Supporting slices: none
- Validation: n/a
- Notes: CMG99 had three modes; alpha correction toggle covers the primary case

## Traceability

| ID | Class | Status | Primary owner | Supporting | Proof |
|---|---|---|---|---|---|
| R011 | quality-attribute | validated | M004-ks4br0/S01 | none | docker-build + grep contracts |
| R012 | quality-attribute | validated | M004-ks4br0/S01 | none | docker-build + grep contracts |
| R013 | core-capability | validated | M004-ks4br0/S02 | none | weight sum=1 structural + docker build; live visual pending |
| R014 | core-capability | validated | M004-ks4br0/S02 | none | sort+clamp code inspection + docker build |
| R015 | primary-user-loop | validated | M004-ks4br0/S02 | none | all 4 generators × 7 sample counts + docker build |
| R016 | primary-user-loop | validated | M004-ks4br0/S02 | none | all 4 knobs grep-confirmed + docker build |
| R017 | primary-user-loop | validated | M004-ks4br0/S02 | none | B-input wiring + edge cases confirmed + docker build |

## Coverage Summary

- Active requirements: 0
- Mapped to slices: 7
- Validated: 7 (R011, R012 — S01; R013–R017 — S02)
- Pending human UAT: R011 visual jaggy elimination; R013 live Nuke flatten visual confirmation
- Unmapped active requirements: 0
