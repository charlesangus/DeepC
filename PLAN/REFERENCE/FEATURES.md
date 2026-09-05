# Feature Research

**Domain:** Nuke NDK C++ deep-compositing plugin suite (DeepC)
**Researched:** 2026-03-12
**Confidence:** HIGH (NDK behavior verified via official Foundry docs; Shuffle2 UI verified via official docs; ZDefocus/Bokeh parameters verified via official docs; DeepThinner verified via GitHub; GL handle API verified via NDK reference + community tutorial; 4D noise verified via existing codebase)

---

## Feature Landscape

### Table Stakes (Users Expect These)

Features users assume exist. Missing these = product feels incomplete.

| Feature | Why Expected | Complexity | Notes |
|---------|--------------|------------|-------|
| DeepShuffle: full channel routing (up to 8 channels, any-to-any) | Nuke's own Shuffle2 routes up to 8 channels; existing DeepCShuffle only routes 4 | MEDIUM | Current node is hardcoded to 4 channels (in0–in3, out0–out3). Expansion to 8 requires adding 4 more Input_Channel_knob + Channel_knob pairs and updating `_validate`, `getDeepRequests`, `doDeepEngine`. |
| DeepShuffle: labeled input/output ports | Shuffle2 shows layer-and-channel labels on sockets; users orient themselves by label | MEDIUM | Current node has no input labels at all. NDK supports `Input_Label` knob text on node tile inputs. Adding descriptive port names ("A", "B") aligns with Nuke conventions. |
| DeepShuffle: black/white constant source options | Shuffle2 lets any output be set to black or white (e.g., clear alpha) | LOW | `Chan_Black` is already tested in `doDeepEngine` (line 118). Add a `Chan_White` constant (value 1.0) to the same guard. Needs a sentinel channel value for "white". |
| GL handles for DeepCPMatte: drag-to-reposition in 3D viewer | Artists expect to click and drag a 3D position handle directly in the viewer | HIGH | `draw_handle` currently draws only a single 2D `glVertex2d` at `_center`. The Axis_knob already exists and provides a 3D transform jack, but `build_handles` bails early for 3D mode (`if (ctx->transform_mode() != VIEWER_2D) return`). Must fix the 3D guard and implement proper 3D drawing. |
| GL handles for DeepCPMatte: visual shape preview (sphere/cube) | Users need to see the matte volume in 3D space matching the shape/falloff knobs | HIGH | No shape outline is drawn today. The viewer needs wire-frame sphere or box drawn at the Axis_knob transform to show extent. |
| GL handles for DeepCPMatte: click-to-sample from 3D viewer | The `knob_changed("center")` mechanism does deep-engine picking on XY click; this must work from the 3D viewport too | HIGH | Currently the click-to-sample only operates in 2D (`XY_knob`). 3D picking requires translating a 3D ray pick into a pixel coordinate and running the same deep-request. |
| DeepDefocus: size (blur radius) control | All Nuke defocus nodes (ZDefocus, Bokeh) expose a primary size/radius knob | LOW | Most fundamental parameter. Directly controls convolution kernel radius in pixels. |
| DeepDefocus: circular disc bokeh kernel | ZDefocus and Bokeh both default to a disc/circular kernel; users expect this as the baseline | MEDIUM | CPU convolution using a disc kernel (radius-test per offset pixel). Computationally O(r^2) per sample. Must be the v1 baseline. |
| DeepDefocus: maximum size clamp | Bokeh node has "Maximum Kernel Size" to cap render cost; ZDefocus has "maximum" clamp | LOW | Without a cap, large radii cause unbounded render time. Simple parameter, critical for usability. |
| DeepDefocus: 2D output | Node reads deep input, outputs a flat 2D composited result | MEDIUM | This is the defining v1 scoping decision (from PROJECT.md). Node must flatten the deep image while applying defocus. Depth-sorted compositing order must be preserved during blending. |
| DeepBlur: size (blur radius/sigma) control | Every Gaussian blur node exposes size or sigma | LOW | Primary parameter. Can expose as pixel radius or Gaussian sigma. |
| DeepBlur: channel selection | Users need to choose which channels to blur (e.g., only RGB, not Z) | LOW | Standard `ChannelSet` knob, as used in DeepCWrapper. |
| DeepBlur: deep output (pass-through topology) | Unlike DeepDefocus, DeepBlur emits a deep image; users expect a deep stream out | HIGH | This is structurally different from 2D blur. Each sample position shifts in XY based on blur; deep metadata (Z depth, alpha) must be preserved or redistributed. msDeepBlur (community) warns sample count grows substantially—this must be documented. |
| 4D noise in DeepCPNoise: expose `noise_evolution` parameter across all applicable types | The `noise_evolution` knob already exists but only activates for Simplex (index 0); users assume "evolution" does something for all noise types | MEDIUM | FastNoise's `GetNoise(x, y, z, w)` 4D overload is only implemented for SimplexFractal. The task is UI clarity: either grey-out/disable the knob for non-Simplex types, or wire up a generic time-offset trick for other types. The fragile magic-index `_noiseType==0` check (CONCERNS.md) must be replaced with a named-constant comparison. |
| DeepThinner: sample-count reduction with 7 optimization passes | DeepThinner (bratgot/DeepThinner) provides: Depth Range clip, Alpha Cull, Occlusion Cutoff, Contribution Cull, Volumetric Collapse, Smart Merge, Max Samples. Users expect all 7 passes to be available. | LOW (integration only) | This is a vendor/integrate task, not new code. The plugin already exists and compiles against Nuke 16. Task = add it to CMake build system + Python toolbar menu. No functional changes needed for v1. |
| DeepThinner: preset configurations (Light Touch, Moderate, Aggressive) | The upstream plugin includes presets; omitting them would be a regression | LOW | Presets are defined in the upstream plugin's knob defaults. Preserved automatically by vendoring. |
| Codebase sweep: fix DeepCGrade reverse-mode gamma bug | Users who enable "reverse" with non-1.0 gamma get silently wrong output | LOW | Bug is documented (CONCERNS.md): `outData` is overwritten immediately after the `pow()` call. One-line fix. |
| Codebase sweep: fix DeepCKeymix B-side channel containment | Wrong channel set used for A-side check; silently wrong output with mixed channel sets | LOW | One-line fix: assign `aInPixelChannels` from `aPixel.channels()` not `bPixel.channels()`. |
| Codebase sweep: fix DeepCSaturation/DeepCHueShift zero-alpha divide | NaN/inf propagation on transparent samples with unpremult enabled (the default) | LOW | Add `if (alpha != 0.0f)` guard before the divide, same pattern as `DeepCWrapper::doDeepEngine` lines 260–263. |
| CI/CD: build on push/PR against Nuke 16 | Any contributor expects a green/red CI badge and not to break others | MEDIUM | GitHub Actions workflow using ASWF Docker image. Matrix of Nuke 16.x. Artifact upload of built `.so` files. |
| CI/CD: automated build on tagged releases | Users expect downloadable release artifacts | MEDIUM | Add release job triggered on tag push; uploads zip archives. |

### Differentiators (Competitive Advantage)

Features that set the product apart. Not required, but baseline usefulness is already established.

| Feature | Value Proposition | Complexity | Notes |
|---------|-------------------|------------|-------|
| DeepShuffle: Shuffle2-style noodle socket UI | Drag-and-drop visual channel wiring matching Nuke's own Shuffle2 UI (introduced Nuke 12.1). No other third-party deep shuffle node does this. | HIGH | Shuffle2's "noodle" UI is implemented as a custom knob type in Nuke's native node — it draws draggable socket points in the node properties panel and previews connections in white on hover. This requires a custom NDK knob (`Knob::KNOB_CHANGED_ALWAYS`, custom `drawWidget()` or Python gizmo approach). The NDK does not expose a built-in "NoodleChannelRouter" knob type. Achieving pixel-perfect Shuffle2 parity in a deep plugin is HIGH complexity. An acceptable v1 may use Layer_knob + Channel_knob pairs with labeled dividers rather than true drag noodles. |
| DeepDefocus: depth-aware per-sample defocus | Each deep sample is defocused proportionally to its Z depth relative to a focal plane, producing genuine depth-of-field from deep data — not achievable with flat Nuke ZDefocus | HIGH | Requires sorting or weighting samples by depth, then applying per-Z blur kernels before flattening to 2D. Architecturally novel: no standard Nuke node does this starting from deep. |
| GL handles: 3D shape preview matching actual matte extent | Showing the sphere or cube in the 3D viewer at the correct world-space scale gives artists immediate spatial feedback. No built-in Nuke matte node does this. | HIGH | Requires `draw_handle` to switch between wireframe sphere (lat/lon lines) and box outline based on `_shape` knob value, scaled by the Axis_knob transform. All OpenGL via `DDImage/gl.h`. |
| 4D noise: time-animated noise with continuity guarantee | The `noise_evolution` parameter (4th dimension in Simplex) provides temporal coherence impossible to achieve by simply shifting the 3D seed. Useful for volumetric animated effects. | LOW (UI task, algorithm already exists) | FastNoise `GetNoise(x,y,z,w)` already implemented. Task is only exposing the knob cleanly with correct enable/disable logic. |
| DeepThinner: in-panel statistics tool | The upstream DeepThinner includes a reduction statistics display (measures before/after sample counts). This is unique quality-of-life for deep pipeline optimization. | LOW | Preserved by vendoring; no extra work. |
| codebase sweep: remove DeepCWrapper/DeepCMWrapper from Nuke node menu | Prevents artist confusion from being able to instantiate broken base class nodes labeled "should never be used directly" | LOW | Remove `Op::Description d` and `build()` from both wrapper `.cpp` files. |
| codebase sweep: redesign `perSampleData` to pointer+length | Unblocks future nodes that need to pass multi-component per-sample data (e.g., full color triplet). Fixes the existing technical debt before it fossilizes. | MEDIUM | Changes the virtual interface signature in `DeepCWrapper`. All subclasses overriding `wrappedPerSample` must be updated. `DeepCHueShift` already works around the limitation. |

### Anti-Features (Commonly Requested, Often Problematic)

Features that seem good but create problems. Deliberately not building these in v1.

| Feature | Why Requested | Why Problematic | Alternative |
|---------|---------------|-----------------|-------------|
| DeepDefocus v1: custom bokeh shape (non-circular kernels) | Artists use heart-shaped or star-shaped bokeh for creative DOF | Custom kernel convolution multiplies render time significantly; sample-correct deep defocus is already CPU-expensive without variable kernels. PROJECT.md explicitly defers this. | Ship circular disc in v1. Expose a `filter_type` enum placeholder with only "disc" enabled for v2 upgrade path. |
| DeepDefocus v1: GPU/Blink path | Faster renders on GPU | Blink's deep-image support is unproven (DeepCBlink is broken; see CONCERNS.md). Correctness must come first. A broken GPU path that silently produces wrong output is worse than a slow correct CPU path. | Mark as stretch goal. Implement CPU path with correctness tests. GPU path in v2 once CPU is validated. |
| DeepBlur v1: GPU/Blink path | Same as above | Same risk as DeepDefocus GPU path. | Same approach: CPU first. |
| DeepShuffle: true drag-noodle UI matching Shuffle2 exactly | Users who know Shuffle2 want identical interaction | The NDK does not provide a public "NoodleShuffle" knob API. Implementing a pixel-perfect clone requires undocumented internal NDK class usage that could break on Nuke version updates. | Use clearly labeled Layer_knob and Channel_knob rows with visual arrows (`>>` text), which is the existing approach. Improve labels and add A/B input support. Document that full Shuffle2 noodle UI is a future milestone contingent on NDK support. |
| DeepDefocus: spatial/depth-varying bokeh (cat's eye, barn door) | Physically accurate lens effects | Requires per-pixel kernel variation; dramatically increases complexity; explicitly deferred to a future milestone in PROJECT.md. | Uniform circular bokeh in v1 covers the primary use case. |
| Nuke < 16 support for new features | Wider audience | Adds ABI complexity, legacy API workarounds, and CMake branching. All new features rely on Nuke 16+ NDK capabilities. | Target Nuke 16+ only as stated in PROJECT.md. Existing nodes continue to build for older Nuke via the existing CMake version matrix. |
| macOS support | Developer machines are often macOS | Currently unmaintained; no CI infrastructure; Nuke macOS ABI differs. Out of scope per PROJECT.md. | Linux (primary) + Windows. macOS can be community-contributed later. |
| Complete DeepCBlink | It exists in the codebase, might as well finish it | Blink deep support is architecturally questionable (CONCERNS.md documents memory leaks, hardcoded CPU path, silent failures). Completing it requires solving deep-to-contiguous-buffer conversion correctly, which is a significant research task. | Evaluate during codebase sweep: if completing it is feasible, create a separate ticket. If not, remove it to avoid misleading users. |

---

## Feature Dependencies

```
DeepShuffle UI improvements
    └──requires──> Existing DeepCShuffle routing logic (already present)
    └──requires──> NDK Input_Channel_knob / Channel_knob (already used)
    └──enhances──> Optional: 2nd deep input (A port) for dual-stream routing

GL handles for DeepCPMatte
    └──requires──> Axis_knob already present on DeepCPMatte
    └──requires──> build_handles / draw_handle framework (partially implemented, 2D only)
    └──requires──> fix: remove early-return for 3D viewer mode in build_handles
    └──requires──> draw sphere/cube wireframe in draw_handle (new code)
    └──requires──> 3D pick-to-sample in knob_changed (new code path)

DeepDefocus (new node)
    └──requires──> New plugin file, independent of wrapper hierarchy
    └──requires──> Deep input (DeepFilterOp or DeepPixelOp)
    └──requires──> 2D output (Iop output, not deep) — requires different base class than DeepCWrapper
    └──enhances──> Could use Axis_knob for focal plane positioning (optional, v2)

DeepBlur (new node)
    └──requires──> New plugin file
    └──requires──> Deep output — inherits from DeepFilterOp
    └──requires──> Sample redistribution logic (scatter blur in XY on deep samples)
    └──conflicts──> DeepDefocus (outputs differ: deep vs 2D; do not share implementation)

4D noise in DeepCPNoise
    └──requires──> FastNoise already implements GetNoise(x,y,z,w) for Simplex
    └──requires──> Fix fragile _noiseType==0 magic index (CONCERNS.md)
    └──requires──> Knob enable/disable for non-Simplex types

DeepThinner (vendor)
    └──requires──> bratgot/DeepThinner source available and buildable against Nuke 16
    └──requires──> CMake integration (add_nuke_plugin or equivalent)
    └──requires──> Python menu.py entry added

CI/CD
    └──requires──> Dockerfile or ASWF CI image available for Nuke 16
    └──requires──> GitHub Actions workflow file
    └──enhances──> All other features (validates they build correctly)

Codebase sweep
    └──requires──> perSampleData redesign must happen BEFORE adding DeepBlur/DeepDefocus
                   (new nodes should use the improved interface from the start)
    └──conflicts──> Completing DeepCBlink and other sweep tasks simultaneously
                    (best done as isolated PRs to keep diffs reviewable)
```

### Dependency Notes

- **DeepDefocus requires a different output type than the wrapper hierarchy:** `DeepCWrapper` and `DeepCMWrapper` both emit deep output. DeepDefocus emits 2D (`Iop`). It must inherit from `Iop` and pull deep input via the deep engine API rather than extending the wrapper base classes. This is architecturally independent of the rest of the suite.
- **DeepBlur conflicts with DeepDefocus at the architecture level:** Despite being "similar blur operations," their output types differ (deep vs 2D). They must be separate plugins with no shared implementation beyond potentially a shared kernel utility function.
- **perSampleData redesign should precede new node work:** DeepBlur and other future nodes that pass multi-component data would immediately hit the single-float limitation. Fixing it during the codebase sweep (before those nodes are authored) avoids a second round of subclass edits.
- **4D noise depends on fixing the magic-index fragility:** The `_noiseType==0` check is explicitly flagged in CONCERNS.md. Any 4D noise UI work that touches this code path must fix the index fragility at the same time to avoid making the fragile code harder to audit.
- **CI/CD enhances all features:** A working CI pipeline should be established early so that every subsequent feature PR gets automated build validation.

---

## MVP Definition

### Launch With (v1 — this milestone)

- [ ] DeepShuffle: expand to 8 channels, add black/white source options, add labeled A/B input ports — essential for parity with what users expect from a modern shuffle node
- [ ] DeepShuffle: improved knob layout (clear input→output arrow rows with layer grouping) — without this the UI is confusing at 8 channels
- [ ] GL handles: fix 3D viewer mode guard, draw sphere/cube wireframe at Axis_knob transform — without visible shape feedback the position-based matte is guesswork in 3D
- [ ] GL handles: click-to-sample in 3D viewport — essential workflow feature matching the existing 2D pick behavior
- [ ] DeepDefocus: size knob, circular disc kernel, maximum clamp, 2D output — minimum viable defocus; without a size knob the node is unusable
- [ ] DeepBlur: size knob, channel selector, deep output — minimum viable blur
- [ ] 4D noise: clean up `noise_evolution` knob visibility (disable for non-Simplex), fix magic-index fragility — without this fix the knob misleads users
- [ ] DeepThinner: vendor and integrate into build + menu — integration only, zero functional work
- [ ] CI/CD: GitHub Actions build on push/PR for Nuke 16 — needed to validate all other work
- [ ] Codebase sweep: fix the four documented bugs (Grade reverse gamma, Keymix A-side channels, Saturation/HueShift zero-alpha, Constant comma operator) — these are known regressions
- [ ] Codebase sweep: remove DeepCWrapper/DeepCMWrapper from node menu — corrects confusing UX

### Add After Validation (v1.x)

- [ ] DeepShuffle: investigate true Shuffle2-style drag-noodle UI — only if NDK API makes it feasible without undocumented internals
- [ ] DeepDefocus: depth-aware per-sample Z-weighted defocus — adds the "deep advantage"; v1 proves the 2D output pipeline works first
- [ ] DeepBlur: GPU/Blink path — only after CPU correctness is validated with render comparison tests
- [ ] perSampleData redesign (pointer + length) — valuable refactor; sequence after sweep bugs are fixed and new nodes are working

### Future Consideration (v2+)

- [ ] DeepDefocus: custom bokeh shapes (bladed aperture, image kernel) — high complexity, PROJECT.md explicitly defers
- [ ] DeepDefocus: spatial/depth-varying bokeh (cat's eye, barn door) — PROJECT.md defers to future milestone
- [ ] GL handles for DeepCPNoise or DeepCWorld — explicitly out of scope in PROJECT.md
- [ ] Grade coefficient shared utility (de-duplicate DeepCGrade vs DeepCPNoise math) — valuable refactor but no user-facing impact; defer until tests exist to validate equivalence

---

## Feature Prioritization Matrix

| Feature | User Value | Implementation Cost | Priority |
|---------|------------|---------------------|----------|
| DeepDefocus (size + disc + 2D out) | HIGH | HIGH | P1 |
| DeepBlur (size + channels + deep out) | HIGH | HIGH | P1 |
| GL handles (3D viewer, shape preview) | HIGH | HIGH | P1 |
| DeepShuffle UI (8ch + labels + black/white) | HIGH | MEDIUM | P1 |
| DeepThinner vendor integration | HIGH | LOW | P1 |
| CI/CD GitHub Actions | HIGH | MEDIUM | P1 |
| Bug fixes (Grade, Keymix, Saturation, Constant) | HIGH | LOW | P1 |
| 4D noise cleanup + knob visibility | MEDIUM | LOW | P1 |
| Remove base classes from node menu | MEDIUM | LOW | P1 |
| DeepShuffle drag-noodle UI | MEDIUM | HIGH | P3 |
| perSampleData redesign | LOW (internal) | MEDIUM | P2 |
| Grade coefficient de-duplication | LOW (internal) | MEDIUM | P3 |
| DeepDefocus GPU/Blink path | MEDIUM | HIGH | P3 |
| Complete/remove DeepCBlink | LOW | HIGH | P3 |

**Priority key:**
- P1: Must have for this milestone launch
- P2: Should have, add when possible within milestone
- P3: Nice to have, future milestone

---

## Competitor Feature Analysis

| Feature | Nuke Built-in | msDeepBlur (community) | DeepThinner (bratgot) | Our Approach |
|---------|--------------|----------------------|----------------------|--------------|
| Channel shuffle (deep) | Shuffle2 (2D only) | None | None | DeepCShuffle — extend to 8ch + labels |
| Depth-of-field from deep | Bokeh, ZDefocus (2D input) | None | None | DeepDefocus — works from actual deep data |
| Blur on deep | None | Gaussian only, slow, no GPU | None | DeepBlur — deep output, CPU first |
| Position matte with 3D handle | None (2D only) | None | None | DeepCPMatte — add 3D viewer handle |
| Sample count optimization | None | None | 7-pass optimization, presets | Vendor DeepThinner directly |
| Noise on deep | None | None | None | DeepCPNoise — add 4D (already partial) |

---

## Per-Feature Expected Behavior Details

### DeepShuffle

**Current state:** 4 fixed channel slots (in0→out0 through in3→out3), no input labels, no black/white source.

**Expected v1 behavior:**
- 8 channel slots (matching Shuffle2 capacity)
- Each slot: dropdown for input channel (including black=0.0 and white=1.0 constants) and dropdown for output channel
- Two labeled inputs on the node tile: "A" and "B" (current node only has input 0)
- Clear in→out row layout with a `>>` separator per row
- Channels not present in input pass through unchanged (current behavior preserved)

**What "noodle style" means vs old Shuffle:**
Old Shuffle: dropdown menus for layer → channel mappings. Shuffle2 (Nuke 12.1+): visual sockets drawn in the properties panel; drag a line from an input socket to an output socket; connections are shown as colored noodles with white preview on hover. The NDK does not expose this as a standard knob type — it is internal to Foundry's Shuffle2 implementation. For v1, the acceptable fallback is well-labeled Channel_knob rows.

### GL Handles for DeepCPMatte

**Current state:** `build_handles` immediately returns for 3D viewer mode. `draw_handle` draws a single 2D point (degenerate). The `Axis_knob` exists and provides a 3D transform jack, but the volume shape is not visualized.

**Expected v1 behavior:**
- In the 3D viewer, a wireframe sphere or wireframe box (matching the `shape` knob) is drawn centered at and scaled by the `Axis_knob` transform
- Dragging the Axis_knob transform handle in 3D space repositions the matte volume — this already works via the Axis_knob's built-in 3D jack once the early-return guard is removed
- Clicking a pixel in the 3D viewport triggers the existing position-sample-and-set behavior (same as the 2D `center` XY click)
- Standard NDK event model: `PUSH` event in `draw_handle` for click detection; `DRAG` event updates the transform

**Snap:** Not required for v1. Nuke's own transform handles do not snap by default either.

### DeepDefocus

**Expected v1 behavior:**
- Inputs: one deep image input
- Output: 2D flat image (Iop, not deep)
- Knobs:
  - `size` (float) — blur radius in pixels at focus plane; default 10
  - `maximum` (float) — cap on kernel radius to bound render time; default 50
  - `channels` (ChannelSet) — which channels to defocus; default rgba
  - `filter_type` (enum) — "disc" only in v1 (enum placeholder for v2 "bladed", "image")
- No focal plane / depth-of-field zone in v1 — uniform defocus applied to all samples; depth-aware per-Z weighting is a v1.x feature

**What a disc kernel is:** A circular area filter. For each output pixel, sum the contribution of all input deep samples whose projected XY position falls within radius `size` pixels of the output pixel, weighted by the disc kernel (flat weight for disc, or a smoothed edge).

**Sample ordering:** Because the output is 2D, samples must be composited in depth order (back to front or use DeepToImage compositing semantics) before or during the blur.

### DeepBlur

**Expected v1 behavior:**
- Inputs: one deep image input
- Output: deep image (Deep output, not 2D)
- Knobs:
  - `size` (float) — Gaussian sigma or pixel radius; default 5
  - `channels` (ChannelSet) — which channels to blur; default rgba
- Behavior: Each input sample is scattered to neighboring pixel positions within the blur radius, creating new samples. Sample count will increase substantially (msDeepBlur community tool confirms this). The deep output must remain valid (samples sorted by Z, Z values preserved).
- No maximum clamp in v1 (can be added as a performance guard in v1.x once the implementation is understood)

### 4D Noise in DeepCPNoise

**Current state:** `noise_evolution` knob exists but is only wired for `_noiseType==0` (Simplex). Other types call `GetNoise(x,y,z)` only. The CONCERNS.md flags the magic index as fragile.

**Expected v1 behavior:**
- `noise_evolution` knob is visually disabled (greyed out via `SetEnabled(false)` or equivalent) when a non-Simplex noise type is selected
- A `knob_changed` handler toggles knob enabled state when `noiseType` changes
- The `_noiseType==0` check is replaced with `noiseTypes[_noiseType] == FastNoise::SimplexFractal`
- Tooltip on the knob clarifies: "Only available for Simplex noise. Other noise types use 3D coordinates only."

### DeepThinner Vendoring

**What DeepThinner does:** Reduces per-pixel deep sample count via 7 independently-controllable passes: Depth Range (near/far clip), Alpha Cull (remove low-opacity samples), Occlusion Cutoff (remove occluded samples), Contribution Cull (remove visually insignificant samples), Volumetric Collapse (condense low-alpha volume runs), Smart Merge (merge Z-adjacent color-similar samples), Max Samples (hard per-pixel limit). Includes preset configurations (Light Touch, Moderate, Aggressive) and an in-panel statistics readout showing sample count reduction.

**Expected v1 behavior:**
- DeepThinner source is added to the repository under `vendor/DeepThinner/` or equivalent
- CMakeLists.txt builds it as a plugin module alongside the other DeepC plugins
- `menu.py.in` includes it in the DeepC toolbar submenu under an appropriate category (Utility/Optimize)
- No functional changes to DeepThinner itself

---

## Sources

- Foundry Nuke documentation — Shuffle node: https://learn.foundry.com/nuke/content/reference_guide/channel_nodes/shuffle.html
- Foundry Nuke documentation — Shuffling Channels (Shuffle2 noodle UI): https://learn.foundry.com/nuke/content/comp_environment/channels/swapping_channels.html
- Foundry Nuke documentation — ZDefocus parameters: https://learn.foundry.com/nuke/content/reference_guide/filter_nodes/zdefocus.html
- Foundry Nuke documentation — Bokeh parameters: https://learn.foundry.com/nuke/content/reference_guide/filter_nodes/bokeh.html
- Erwan Leroy NDK blog — GL handles, build_handles, draw_handle: https://erwanleroy.com/writing-nuke-c-plugins-ndk-part-4-custom-knobs-gl-handles-and-the-table-knob/
- Foundry NDK reference — ViewerContext class: https://learn.foundry.com/nuke/developers/130/ndkreference/Plugins/classDD_1_1Image_1_1ViewerContext.html
- DeepThinner (bratgot/DeepThinner, GitHub): https://github.com/bratgot/DeepThinner — 7-pass optimizer with preset configs and statistics panel
- msDeepBlur (Nukepedia): https://www.nukepedia.com/tools/plugins/deep/msdeepblur/ — Gaussian deep blur reference; warns of sample count explosion
- Existing codebase: `src/DeepCShuffle.cpp`, `src/DeepCPMatte.cpp`, `src/DeepCPNoise.cpp` — current feature state verified by direct read

---
*Feature research for: DeepC Nuke NDK plugin suite, milestone 2 features*
*Researched: 2026-03-12*
