# Requirements: DeepC

**Defined:** 2026-03-12
**Core Value:** Artists can use deep-compositing operations in Nuke that don't exist in the built-in toolset, with professional-quality UIs and reliable deep-pixel math.

## v1 Requirements

### Codebase Sweep

- [x] **SWEEP-01**: DeepCGrade reverse-mode gamma is applied to result of linear ramp, not discarded (fix `outData = A * inputVal + B` → `outData = A * outData + B` on reverse path)
- [x] **SWEEP-02**: DeepCKeymix A-side containment check queries `aPixel.channels()`, not `bPixel.channels()`
- [x] **SWEEP-03**: DeepCSaturation and DeepCHueShift guard against divide-by-zero when alpha == 0 during unpremult
- [x] **SWEEP-04**: DeepCConstant weight calculation uses correct lerp expression, not C++ comma operator
- [x] **SWEEP-05**: DeepCID foreach loop over `_auxiliaryChannelSet` uses the loop variable `z`, not the cached `_auxChannel`
- [x] **SWEEP-06**: `Op::Description` and `build()` factory removed from DeepCWrapper and DeepCMWrapper so they do not appear in the Nuke node menu
- [ ] **SWEEP-07**: `perSampleData` interface redesigned to pass a pointer + length instead of a single `float`, updating all subclasses
- [ ] **SWEEP-08**: Grade coefficient arrays (`A[]`, `B[]`, `G[]`) and precompute logic extracted into a shared utility and reused by both DeepCGrade and DeepCPNoise
- [x] **SWEEP-09**: DeepCBlink either completed (functional GPU path, all channel counts handled, no silent bail-out) or removed from the build and toolbar
- [x] **SWEEP-10**: All deprecated NDK APIs used in the codebase updated to their Nuke 16+ equivalents

### DeepShuffle UI

- [ ] **SHUF-01**: DeepCShuffle exposes per-channel input/output routing knobs matching the layout of the legacy Nuke Shuffle node
- [ ] **SHUF-02**: DeepCShuffle input and output ports are labeled with their assigned channel names
- [ ] **SHUF-03**: DeepCShuffle supports routing up to 8 channels (expanded from current 4)
- [ ] **SHUF-04**: DeepCShuffle provides layer-level routing via `Input_ChannelSet_knob` in addition to per-channel selection

### DeepCPMatte GL Handles

- [x] **GLPM-01**: `draw_handle()` in DeepCPMatte no longer calls `_validate()` on the GL thread (deadlock fix)
- [x] **GLPM-02**: DeepCPMatte draws a wireframe sphere or cube in the 3D viewport representing the current selection volume
- [x] **GLPM-03**: The wireframe handle in DeepCPMatte is interactive — user can click and drag to reposition the selection volume

### DeepCPNoise 4D

- [ ] **NOIS-01**: DeepCPNoise exposes a 4D noise option in the UI for all supported noise types (Simplex, Perlin, Cellular, Cubic, Value), wired to the existing FastNoise 4D methods

### DeepThinner Vendor

- [ ] **THIN-01**: DeepThinner license confirmed compatible with GPL-3.0 before source is integrated
- [ ] **THIN-02**: DeepThinner source integrated into the CMake build system and compiled as a Nuke plugin
- [ ] **THIN-03**: DeepThinner registered in the Nuke toolbar menu under the DeepC submenu

## v2 Requirements

### CI/CD & Build Infrastructure

- **CI-01**: Docker container (NukeDockerBuild image, Nuke 16) for reproducible local and CI builds
- **CI-02**: GitHub Actions workflow triggering automated build on push and pull request
- **CI-03**: Compiled `.so` artifacts uploaded as GitHub Actions build outputs
- **CI-04**: Build matrix covering multiple Nuke 16.x minor versions

### DeepDefocus (v1)

- **DDFX-01**: New `Iop` subclass accepting deep input and producing 2D output
- **DDFX-02**: Circular bokeh (disc filter) applied uniformly across all depth samples
- **DDFX-03**: Knobs: size, channels, max_samples (ZDefocus-style conventions)
- **DDFX-04**: Explicit row zeroing for sparse/empty deep pixels to prevent garbage output

### DeepBlur

- **DBLR-01**: New `DeepFilterOp` subclass with Deep output
- **DBLR-02**: Spatial Gaussian/box blur across neighboring pixels
- **DBLR-03**: Expanded bbox request (inflate by blur radius) in `getDeepRequests()`
- **DBLR-04**: Z-sort order preserved in output samples

## Out of Scope

| Feature | Reason |
|---------|--------|
| Shuffle2 noodle/wire routing UI | NDK does not expose this API; internal to Foundry's built-in node only |
| DeepDefocus v2 (custom bokeh shapes) | Deferred to future milestone |
| DeepDefocus v3 (spatial/depth-varying bokeh) | Deferred to future milestone |
| GL handles for DeepCPNoise or DeepCWorld | Not requested for this milestone |
| CI/CD automation | Deferred to next milestone |
| DeepBlur (all) | Deferred to next milestone |
| Nuke < 16 support for new/modified features | Dropping legacy constraints |
| macOS support | Not actively maintained |

## Traceability

Which phases cover which requirements. Updated during roadmap creation.

| Requirement | Phase | Status |
|-------------|-------|--------|
| SWEEP-01 | Phase 1 | Complete |
| SWEEP-02 | Phase 1 | Complete |
| SWEEP-03 | Phase 1 | Complete |
| SWEEP-04 | Phase 1 | Complete |
| SWEEP-05 | Phase 1 | Complete |
| SWEEP-06 | Phase 1 | Complete |
| SWEEP-07 | Phase 1 | Pending |
| SWEEP-08 | Phase 1 | Pending |
| SWEEP-09 | Phase 1 | Complete |
| SWEEP-10 | Phase 1 | Complete |
| GLPM-01 | Phase 2 | Complete |
| GLPM-02 | Phase 2 | Complete |
| GLPM-03 | Phase 2 | Complete |
| SHUF-01 | Phase 3 | Pending |
| SHUF-02 | Phase 3 | Pending |
| SHUF-03 | Phase 3 | Pending |
| SHUF-04 | Phase 3 | Pending |
| NOIS-01 | Phase 4 | Pending |
| THIN-01 | Phase 5 | Pending |
| THIN-02 | Phase 5 | Pending |
| THIN-03 | Phase 5 | Pending |

**Coverage:**
- v1 requirements: 21 total (note: initial count of 20 in this file was off by one; actual count is 21)
- Mapped to phases: 21
- Unmapped: 0

---
*Requirements defined: 2026-03-12*
*Last updated: 2026-03-12 after roadmap creation — all requirements mapped*
