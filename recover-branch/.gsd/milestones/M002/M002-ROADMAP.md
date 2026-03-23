# M002: DeepCBlur

**Vision:** A Gaussian blur node for deep images — propagates samples outward, applies Gaussian-weighted blending, and includes built-in per-pixel sample optimization to keep memory bounded. Adds the 24th plugin to the DeepC suite.

## Success Criteria

- DeepCBlur node loads in Nuke 16+ and produces blurred deep output
- Separate width/height size knobs allow non-uniform Gaussian blur
- Sample propagation creates new samples in previously-empty neighboring pixels
- Built-in per-pixel optimization merges nearby-depth samples and caps at max_samples
- DeepSampleOptimizer.h is a standalone shared header reusable by future ops
- docker-build.sh produces release archives for Linux and Windows containing DeepCBlur

## Key Risks / Unknowns

- **Sample-merging semantics** — when propagated samples from multiple source pixels overlap in depth at an output pixel, the merge logic must produce correct compositing results. Wrong merging = visible artifacts.
- **Performance at large blur sizes** — O(W×H×S×kernel_area) is inherent. Dense deep images with large blur radius will be slow. Tooltip warning + max_samples cap are the pragmatic mitigations.
- **Separable Gaussian feasibility** — may need full 2D kernel per pixel if horizontal/vertical decomposition is too complex for variable-length sample arrays. Performance tradeoff.

## Proof Strategy

- Sample-merging semantics → retire in S01 by proving a sparse deep image (single sample at center) produces visible blurred output with new samples in neighboring pixels, and a dense deep image (overlapping objects at different depths) produces correct compositing after blur + flatten
- Performance → retire in S01 by verifying blur size=5 on a representative deep image completes in reasonable time (<30s for HD) and max_samples clamp reduces output sample counts

## Verification Classes

- Contract verification: local CMake build succeeds, DeepCBlur.so loads in Nuke 16+
- Integration verification: docker-build.sh produces release archives for both platforms with DeepCBlur
- Operational verification: none (plugin, not a service)
- UAT / human verification: visual inspection of blurred deep output in Nuke viewer

## Milestone Definition of Done

This milestone is complete only when all are true:

- DeepCBlur.cpp compiles and loads cleanly in Nuke 16+
- Gaussian blur with separate width/height produces visible blurred deep output
- Sample propagation creates new samples in empty pixels
- Per-pixel optimization keeps sample counts bounded
- DeepSampleOptimizer.h exists as a standalone shared header
- DeepCBlur appears in DeepC > Filter toolbar menu
- docker-build.sh produces release archives for Linux and Windows containing DeepCBlur
- README reflects 24 plugins

## Requirement Coverage

- Covers: R001, R002, R003, R004, R005, R006, R007
- Partially covers: none
- Leaves for later: none
- Orphan risks: none

## Slices

- [x] **S01: DeepCBlur core implementation** `risk:high` `depends:[]`
  > After this: DeepCBlur.cpp compiles with local CMake, loads in Nuke 16+, and produces Gaussian-blurred deep output with sample propagation and per-pixel optimization. DeepSampleOptimizer.h exists as a shared header.

- [x] **S02: Build integration and release** `risk:low` `depends:[S01]`
  > After this: DeepCBlur appears in DeepC > Filter toolbar. docker-build.sh produces Linux and Windows release archives containing DeepCBlur. README updated to 24 plugins.

## Boundary Map

### S01 → S02

Produces:
- `src/DeepCBlur.cpp` — complete DeepFilterOp subclass with Gaussian blur, sample propagation, per-pixel optimization, Op::Description registration
- `src/DeepSampleOptimizer.h` — shared header with `optimizeSamples()` function (merge nearby depths + max_samples cap)
- Local CMake build verified: DeepCBlur.so compiles and links against Nuke 16.0 SDK

Consumes:
- nothing (first slice)

### S02 (final)

Produces:
- Updated `src/CMakeLists.txt` — DeepCBlur in PLUGINS list, FILTER_NODES updated
- docker-build.sh verification for Linux + Windows
- Updated README.md with 24-plugin count
- DeepCBlur.png icon (placeholder or proper)

Consumes from S01:
- `src/DeepCBlur.cpp` — the compilable plugin source
- `src/DeepSampleOptimizer.h` — included by DeepCBlur.cpp
