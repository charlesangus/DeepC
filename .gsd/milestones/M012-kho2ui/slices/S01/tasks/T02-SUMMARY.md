---
id: T02
parent: S01
milestone: M012-kho2ui
provides: []
requires: []
affects: []
key_files: ["crates/opendefocus/src/lib.rs", "crates/opendefocus/src/worker/engine.rs", "crates/opendefocus-deep/src/lib.rs"]
key_decisions: ["render_convolve_prepped skips Telea inpaint and uses zero inpaint_image + duplicate-channel depth, matching the 2-D code path; correct for layer-peeling because each layer already has a valid alpha channel", "MINIMUM_FILTER_SIZE promoted to pub(crate) so lib.rs can reference it in prepare_filter_mipmaps without re-declaring the constant", "render_stripe_prepped bypasses the Image-filter validation check since the filter is already baked into the MipmapBuffer"]
patterns_established: []
drill_down_paths: []
observability_surfaces: []
duration: ""
verification_result: "grep verification: all four target symbols present in correct files. cargo test -p opendefocus-deep compiled cleanly and ran 2 tests — both passed: test_holdout_attenuates_background_samples and test_singleton_reuse. Output values identical to T01 baseline, confirming no regression."
completed_at: 2026-03-30T23:03:24.995Z
blocker_discovered: false
---

# T02: Added prepare_filter_mipmaps + render_stripe_prepped to opendefocus and hoisted filter/mipmap prep outside the layer loop in opendefocus-deep, eliminating per-layer filter construction and Telea inpaint

> Added prepare_filter_mipmaps + render_stripe_prepped to opendefocus and hoisted filter/mipmap prep outside the layer loop in opendefocus-deep, eliminating per-layer filter construction and Telea inpaint

## What Happened
---
id: T02
parent: S01
milestone: M012-kho2ui
key_files:
  - crates/opendefocus/src/lib.rs
  - crates/opendefocus/src/worker/engine.rs
  - crates/opendefocus-deep/src/lib.rs
key_decisions:
  - render_convolve_prepped skips Telea inpaint and uses zero inpaint_image + duplicate-channel depth, matching the 2-D code path; correct for layer-peeling because each layer already has a valid alpha channel
  - MINIMUM_FILTER_SIZE promoted to pub(crate) so lib.rs can reference it in prepare_filter_mipmaps without re-declaring the constant
  - render_stripe_prepped bypasses the Image-filter validation check since the filter is already baked into the MipmapBuffer
duration: ""
verification_result: passed
completed_at: 2026-03-30T23:03:24.995Z
blocker_discovered: false
---

# T02: Added prepare_filter_mipmaps + render_stripe_prepped to opendefocus and hoisted filter/mipmap prep outside the layer loop in opendefocus-deep, eliminating per-layer filter construction and Telea inpaint

**Added prepare_filter_mipmaps + render_stripe_prepped to opendefocus and hoisted filter/mipmap prep outside the layer loop in opendefocus-deep, eliminating per-layer filter construction and Telea inpaint**

## What Happened

Extended opendefocus crate with MipmapBuffer re-export, prepare_filter_mipmaps method, and render_stripe_prepped method on OpenDefocusRenderer. Added render_with_prebuilt_mipmaps and render_convolve_prepped to RenderEngine (the latter skips Telea inpaint by using a zero inpaint image and duplicate-channel depth). In opendefocus-deep, hoisted a single prepare_filter_mipmaps call above the layer loop and replaced all render_stripe calls inside the loop with render_stripe_prepped, so filter/mipmap preparation happens exactly once per render_impl call instead of once per layer.

## Verification

grep verification: all four target symbols present in correct files. cargo test -p opendefocus-deep compiled cleanly and ran 2 tests — both passed: test_holdout_attenuates_background_samples and test_singleton_reuse. Output values identical to T01 baseline, confirming no regression.

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `grep -q 'prepare_filter_mipmaps' crates/opendefocus/src/lib.rs && grep -q 'render_stripe_prepped' crates/opendefocus/src/lib.rs && grep -q 'render_with_prebuilt_mipmaps' crates/opendefocus/src/worker/engine.rs && grep -q 'prepare_filter_mipmaps' crates/opendefocus-deep/src/lib.rs` | 0 | ✅ pass | 50ms |
| 2 | `~/.cargo/bin/cargo test -p opendefocus-deep --features 'opendefocus/protobuf-vendored' -- --nocapture` | 0 | ✅ pass | 5000ms |


## Deviations

MINIMUM_FILTER_SIZE promoted to pub(crate) for lib.rs access. render_convolve_prepped added as private RenderEngine method. Both were minor adaptations consistent with the plan.

## Known Issues

None.

## Files Created/Modified

- `crates/opendefocus/src/lib.rs`
- `crates/opendefocus/src/worker/engine.rs`
- `crates/opendefocus-deep/src/lib.rs`


## Deviations
MINIMUM_FILTER_SIZE promoted to pub(crate) for lib.rs access. render_convolve_prepped added as private RenderEngine method. Both were minor adaptations consistent with the plan.

## Known Issues
None.
