# Stack Research

**Domain:** Nuke NDK C++ deep-compositing plugin suite (DeepC)
**Researched:** 2026-03-12
**Confidence:** MEDIUM — NDK 16 release notes verified via official Foundry docs; channel knob API verified via official NDK developer guide; GL handle API verified via official NDK reference + community tutorial; CI/Docker verified via NukeDockerBuild project; defocus algorithms drawn from academic literature and NDK deep-to-2D guide (HIGH for architecture, MEDIUM for algorithm specifics)

---

## Recommended Stack

### Core Technologies

| Technology | Version | Purpose | Why Recommended |
|------------|---------|---------|-----------------|
| Foundry NDK (DDImage) | 16.0+ | Plugin API | The only supported plugin interface; all `DD::Image` classes, deep pixel processing, knob system, viewer handles, and channel routing live here |
| C++17 | ISO C++17 | Language standard | Required by VFX Reference Platform CY2024 and Nuke 16 NDK; `_GLIBCXX_USE_CXX11_ABI=1` new ABI is mandatory for Nuke 15+/16 |
| GCC 11.2.1 | 11.2.1 | Compiler | VFX Platform CY2024 specifies GCC 11.2.1 with `libstdc++` new ABI; must match the compiler Nuke itself was built with to avoid ODR violations |
| CMake | 3.15+ | Build system | Already in use; NDK sample projects use CMake; minimum 3.15 required for `cmake --install` and modern target syntax |
| Rocky Linux 9.0 | 9.0 | Build OS | Nuke 16 is built on Rocky Linux 9; plugins compiled on this OS are guaranteed ABI-compatible with the shipping binary |

### Channel Routing UI (DeepShuffle)

| API | Header | Purpose | Why |
|-----|--------|---------|-----|
| `Input_Channel_knob()` | `DDImage/Knobs.h` | Per-row source channel picker | Shows only layers that exist on the connected input (no "new layer" option). Use for the "from" side of each shuffle row. Already used in the existing `DeepCShuffle` |
| `Channel_knob()` | `DDImage/Knobs.h` | Per-row destination channel picker | Includes "new" and "none" options; use for the "to" side of each shuffle row. Already used in the existing `DeepCShuffle` |
| `Input_ChannelSet_knob()` | `DDImage/Knobs.h` | Multi-channel set picker from input | Use if you want to select a whole layer at once as input |
| `ChannelSet_knob()` | `DDImage/Knobs.h` | Multi-channel set picker (output) | Preferred over `ChannelMask_knob` — the legacy variant is the same thing but officially deprecated in the NDK dev guide |
| `Text_knob()` with `ClearFlags(f, Knob::STARTLINE)` | `DDImage/Knobs.h` | Visual separator / label within a row | Used to render `>>` arrows between input and output columns, giving a visual routing diagram |

**Shuffle2-style "noodle" connections:** No NDK C++ API exposes the visual wire-routing UI introduced in the built-in Shuffle2 node. That UI is implemented internally by Foundry and is not part of the public NDK. The best achievable approximation for a third-party plugin is the `Input_Channel_knob + Text_knob(">>") + Channel_knob` pattern already in `DeepCShuffle`, with clean labeling and logical grouping. Do not attempt to replicate noodles — the API surface does not exist.

**Do NOT use** `ChannelMask_knob()` — the NDK guide explicitly describes it as a legacy knob equivalent to `ChannelSet_knob`; prefer the ChannelSet variants.

### GL Handles / 3D Viewport (DeepCPMatte)

| API | Header | Purpose | Why |
|-----|--------|---------|-----|
| `Op::build_handles(ViewerContext* ctx)` | `DDImage/Op.h` | Register the node's viewer presence | Called by Nuke to ask what to draw; call `build_knob_handles(ctx)` to let knobs draw themselves, then `add_draw_handle(ctx)` to register your custom draw callback |
| `Op::draw_handle(ViewerContext* ctx)` | `DDImage/Op.h` | Issue OpenGL draw calls | Called per-frame by Nuke's GL thread; guarded with `ctx->draw_lines()`, `ctx->event()`, etc. |
| `ViewerContext::transform_mode()` | `DDImage/ViewerContext.h` | Test 2D vs. 3D viewer mode | Returns `VIEWER_2D` for the 2D viewer; only draw if in the correct mode |
| `ViewerContext::draw_lines()` | `DDImage/ViewerContext.h` | Guard wire/line drawing | Returns `true` when wireframe pass is active |
| `ViewerContext::event()` | `DDImage/ViewerContext.h` | Distinguish draw vs. hit-test pass | `DRAW_LINES`, `DRAW_SHADOW`, `PUSH` (mouse click), `DRAG`, `RELEASE` |
| `ViewerContext::node_color()` | `DDImage/ViewerContext.h` | Get node color for drawing | Returns `0` (black) during `DRAW_SHADOW` pass, otherwise the user-assigned color |
| `begin_handle()` / `end_handle()` | `DDImage/ViewerContext.h` | Wrap OpenGL geometry for hit detection | Any `glVertex*` calls between these are tested for mouse clicks; a callback fires on `PUSH` |
| `add_draw_handle(ctx)` | `DDImage/ViewerContext.h` | Register a draw callback on the Op | Called from `build_handles()`; triggers `draw_handle()` on appropriate viewer events |
| `Axis_knob()` | `DDImage/Knobs.h` | 6-DOF transform handle | Already used in `DeepCPMatte`; renders a 3D transform jack with translate/rotate/scale; use this for the sphere/cube selection shape transform |
| OpenGL immediate mode (`glBegin`, `glVertex2f`, `glEnd`, `GL_LINES`, `GL_LINE_LOOP`, `GL_POINTS`) | `DDImage/gl.h` | Draw viewport overlays | Nuke's NDK exposes legacy immediate-mode OpenGL for handle drawing; core profile / VAO-based drawing is not used for handles |

**RTLD_DEEPBIND note (Nuke 16):** Nuke 16 loads plugins with `RTLD_DEEPBIND` by default on Linux. This prevents OpenGL symbol conflicts between the plugin's libGL and Nuke's. No source changes needed; be aware that any global-scope OpenGL state from a plugin will be isolated.

**Do NOT** attempt to draw in the 3D scene graph (Nuke's new USD-based 3D system introduced in Nuke 14). The existing `DeepCPMatte` correctly targets the 2D viewer overlay path (`VIEWER_2D`), which is the appropriate context for a P-matte selection shape. The 3D scene graph is a separate rendering system requiring `GeoOp` subclassing and is out of scope.

### Deep to 2D Output (DeepDefocus)

| API | Header | Purpose | Why |
|-----|--------|---------|-----|
| `Iop` (inherit from, not `DeepFilterOp`) | `DDImage/Iop.h` | Output a 2D image from deep input | DeepDefocus produces a standard 2D image (flattened+blurred); inheriting `Iop` with `test_input()` accepting `DeepOp*` is the documented NDK pattern for deep-to-2D ops |
| `Op::test_input(int n, Op* op) const` | `DDImage/Op.h` | Accept deep inputs on an `Iop` | Override to `dynamic_cast<DeepOp*>(op) != nullptr`; tells Nuke this Iop accepts a deep connection on input 0 |
| `Iop::_request()` | `DDImage/Iop.h` | Request deep data from input | Call `input0()->deepRequest(bbox, channels)` instead of the normal `request()` |
| `Iop::engine(int y, int x, int r, ChannelMask, Row&)` | `DDImage/Iop.h` | Scanline output engine | Fetch the full-frame `DeepPlane` in `_validate()` or cache it; then serve scanlines from it in `engine()` |
| `DeepOp::deepEngine(Box, ChannelSet, DeepPlane&)` | `DDImage/DeepOp.h` | Fetch deep data from input | Call on `input0()` to obtain the `DeepPlane`; iterate pixels and samples |
| `DeepPixel::getUnorderedSample(sampleNo, channel)` | `DDImage/DeepPixel.h` | Read per-sample channel value | Standard access pattern for all existing DeepC plugins |
| `PlanarIop` (alternative base) | `DDImage/PlanarIop.h` | Output full stripes instead of scanlines | Prefer if the defocus kernel needs 2D access patterns (radius look-ups require rows above/below); `renderStripe()` replaces `engine()` |

**Deep defocus algorithm recommendation (CPU v1):** Use a **scatter-as-gather** approach.

1. Flatten the deep pixel at each (x, y) into a sorted sample list (nearest to farthest).
2. For each output pixel, compute its circle of confusion (CoC) radius from the depth of the nearest contributing sample and the user's focus-distance / f-stop knobs.
3. Accumulate contributions from all input pixels whose CoC disc covers the output pixel (gather), weighted by a circular kernel (1 inside radius, 0 outside — extend to soft edge with a smoothstep falloff for a more physically plausible look).
4. Output as a standard 2D `Iop` row by row.

This is simpler to implement correctly than scatter (which requires atomic writes or a separate accumulation buffer), avoids FFT convolution complexity (FFT is only worth it for very large radii), and is the pattern used by ZDefocus's layer-slicing approach. For v1 CPU correctness is the goal; GPU/Blink is explicitly deferred.

### Deep Blur (DeepBlur — Deep output)

| API | Header | Purpose | Why |
|-----|--------|---------|-----|
| `DeepFilterOp` (inherit from) | `DDImage/DeepFilterOp.h` | Accept and emit deep data | DeepBlur outputs a Deep stream, so `DeepFilterOp` is the correct base (same as most existing DeepC plugins) |
| `DeepFilterOp::doDeepEngine()` | `DDImage/DeepFilterOp.h` | Process deep pixel by pixel | Override to blur per-channel values across the deep plane; for a per-sample Gaussian blur, convolve sample values across spatial neighbours |
| `DeepInPlaceOutputPlane` | `DDImage/DeepPixel.h` | In-place output buffer | Already used by most DeepC plugins; write blurred samples into this, then assign to `deepOutPlane` |

**Note:** A true deep blur (blurring across the deep stream while preserving sample depth order) is significantly more complex than a 2D blur because samples at different depths within the blur radius may interact. For v1, a reasonable simplification is to treat each sample's RGB independently and apply a spatial Gaussian to same-depth-channel values. Document this limitation in the node's help text.

### CI / Build Infrastructure

| Technology | Version/Tag | Purpose | Why |
|------------|-------------|---------|-----|
| gillesvink/NukeDockerBuild | latest for Nuke 16.x | Pre-built Docker images containing NDK + GCC 11.2.1 + CMake on Rocky Linux 9 | Purpose-built for exactly this workflow; images ship with Nuke installed at `/usr/local/nuke_install` and `NUKE_VERSION` env var set; avoids the ASWF `ci-base` approach (ASWF images do not include Nuke) |
| GitHub Actions | N/A | CI runner | Native to the repo, free for public repos; use `jobs.<job>.container.image` to run the build inside the NukeDockerBuild container |
| CMake `--build` + `--install` | 3.15+ | Build invocation | Same pattern as existing build; add a `cmake -DCMAKE_INSTALL_PREFIX=artifact ...` step to produce a release artifact directory |
| GitHub Actions `upload-artifact` | v4 | Artifact publishing | Upload the install directory as a versioned zip per Nuke version; consumers download from the Releases page |

**ASWF `ci-base` images are NOT recommended for this project.** ASWF images provide the VFX platform toolchain (compilers, libraries) but do NOT include Nuke itself. They were used in the old README for Nuke 13–15 only because a developer already had Nuke volume-mounted. For CI without a mounted Nuke volume, NukeDockerBuild (which bundles the NDK) is the only practical option.

**Dockerfile authorship:** Build a `Dockerfile` that starts `FROM nukedockerbuild:{NUKE_VERSION}-linux` (or the equivalent Codeberg image tag), copies the source, runs `cmake --preset release` (or equivalent), then `cmake --install`. The GitHub Actions workflow matrix over `{NUKE_VERSION: ["16.0", "16.1"]}` to produce artifacts for each minor version.

---

## Supporting Libraries

| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| FastNoise (vendored) | Jordan Peck 2017 (existing) | 4D noise generation | Already in tree; use for the 4D noise exposure task in DeepCPNoise — no new vendored dependency needed |
| OpenGL (system `libGL`) | System / NDK-bundled | GL handle drawing for DeepCPMatte | Already linked; no change needed for Nuke 16 on Linux — RTLD_DEEPBIND handles symbol isolation |
| DeepThinner (bratgot/DeepThinner) | Latest commit | Deep sample thinning | Vendor as a git submodule (`git submodule add`); compile as an OBJECT library and link into the DeepThinner plugin target |

---

## Development Tools

| Tool | Purpose | Notes |
|------|---------|-------|
| `cmake/FindNuke.cmake` | Locate Nuke installation | Already present; update to add Nuke 16 to the known version list and remove Nuke < 15 entries once the legacy drop is confirmed |
| `clang-format` | Code formatting enforcement | Add a `.clang-format` file matching existing 4-space style; run in CI via `clang-format --dry-run --Werror` |
| GCC address sanitizer (`-fsanitize=address`) | Memory error detection | Add an optional CMake preset for ASAN builds; useful during DeepDefocus/DeepBlur development where buffer overruns are a risk |
| `valgrind` | Memory profiling | Available in Rocky Linux 9 repos; useful for deep pixel loop profiling |

---

## Alternatives Considered

| Recommended | Alternative | When to Use Alternative |
|-------------|-------------|-------------------------|
| `Iop` inheritance for DeepDefocus | `DeepFilterOp` + separate flatten | Never — `DeepFilterOp` cannot produce a 2D output; the NDK explicitly documents the `Iop`-with-deep-input pattern for deep-to-2D ops |
| Scatter-as-gather CPU defocus | FFT convolution | Only if CoC radii exceed ~64px and performance is unacceptable; FFT requires a full-frame buffer and is harder to implement correctly with per-pixel varying radii |
| NukeDockerBuild images | ASWF `ci-base` + volume-mount | Only if running builds on a machine where Nuke is installed locally and can be mounted; not viable for GitHub-hosted runners |
| `Input_Channel_knob` + `Channel_knob` pairs | Attempting to replicate Shuffle2 noodle UI | Never — Shuffle2 noodle UI is not in the public NDK; the knob-pair approach is the only available NDK option |
| `ChannelSet_knob` | `ChannelMask_knob` | Never — the NDK guide calls `ChannelMask_knob` a legacy equivalent; `ChannelSet_knob` is the current form |
| `PlanarIop` for DeepDefocus | `Iop` (scanline engine) | If the gather kernel requires access to multiple input rows simultaneously (likely for radii > 1 scanline); `PlanarIop::renderStripe()` provides a 2D tile buffer |

---

## What NOT to Use

| Avoid | Why | Use Instead |
|-------|-----|-------------|
| `ChannelMask_knob` | Officially deprecated in NDK dev guide; functionally identical to `ChannelSet_knob` but the legacy name will confuse readers and may be removed in future NDK versions | `ChannelSet_knob` or `Input_ChannelSet_knob` |
| ASWF `ci-base` images for CI | Do not include Nuke/NDK; require a locally-mounted Nuke volume — impossible on GitHub-hosted runners | `gillesvink/NukeDockerBuild` images (NDK bundled) |
| Nuke 3D scene graph / `GeoOp` for GL handles | Requires implementing the full USD/hydra scene graph interface introduced in Nuke 14; entirely wrong layer for a P-matte selection shape drawn in the 2D viewer | `Op::build_handles()` + `Op::draw_handle()` + immediate-mode OpenGL via `DDImage/gl.h` |
| `_GLIBCXX_USE_CXX11_ABI=0` | Old ABI, required for Nuke < 15 only; using it for Nuke 16 produces undefined symbols at link time when calling any NDK function that returns `std::string` | `_GLIBCXX_USE_CXX11_ABI=1` (already set in CMakeLists.txt for Nuke >= 15) |
| Nuke < 15 version targets in CMakeLists.txt | The milestone explicitly drops legacy support; the ABI branching and devtoolset complexity add maintenance cost with no benefit | Remove the `if (NUKE_VERSION_MAJOR VERSION_LESS 15)` branch entirely; hard-code C++17 and new ABI |
| `OutputContext(frame, view)` two-arg constructor | Deprecated in Nuke 16 NDK; the new `GraphScopePtr` member means this constructor may produce incorrect comparison behaviour | Copy-construct from Nuke-provided `OutputContext` objects |
| `DeepFilterOp` for DeepDefocus | `DeepFilterOp` routes deep-in to deep-out; it cannot produce a flat 2D `Row` output | `Iop` with `test_input()` accepting `DeepOp*` |

---

## Stack Patterns by Variant

**If adding a new channel-routing node (like improved DeepShuffle):**
- Use `Input_Channel_knob(f, &_inChannel, 1, 0, "inN")` for each source row
- Use `Text_knob(f, ">>"); ClearFlags(f, Knob::STARTLINE);` for the visual arrow
- Use `Channel_knob(f, &_outChannel, 1, "outN"); ClearFlags(f, Knob::STARTLINE);` for the destination
- Inherit from `DeepFilterOp` directly (not `DeepCWrapper`) because the pipeline is channel-routing, not color math
- In `_validate()`, add all output channels to `_deepInfo` to extend the channel stream

**If adding a GL viewport handle to a DeepCMWrapper node:**
- Override `build_handles(ViewerContext* ctx)` on the Op subclass (not on a Knob)
- Call `build_knob_handles(ctx)` first to let `Axis_knob` draw its own jack
- Call `add_draw_handle(ctx)` to register your `draw_handle` callback
- Guard `draw_handle` with `if (!ctx->draw_lines()) return;` and `if (ctx->transform_mode() != VIEWER_2D) return;`
- Use `begin_handle(ctx, callback, ...) / glVertex*() / end_handle(ctx)` to make geometry clickable

**If adding a new node with 2D output from deep input:**
- Inherit from `Iop`
- Override `test_input(int n, Op* op)` to accept `DeepOp*` on input 0
- Override `default_input(int input)` to return `nullptr` (no default 2D image)
- Fetch and cache the `DeepPlane` during `_validate()` or `_request()`; serve data in `engine()` per scanline
- Consider `PlanarIop` if the kernel requires tile-level 2D access

**If adding a new node with deep output:**
- Inherit from `DeepFilterOp` (or `DeepCWrapper` if it benefits from the existing pipeline)
- Implement `doDeepEngine()` using `DeepInPlaceOutputPlane`

---

## Version Compatibility

| Component | Compatible With | Notes |
|-----------|-----------------|-------|
| Plugin `.so` built with GCC 11.2.1 + new ABI | Nuke 15.0–16.x | Both Nuke 15 and 16 use GCC 11 / new ABI on Linux |
| Plugin `.so` built with old ABI (`_GLIBCXX_USE_CXX11_ABI=0`) | Nuke 9–14.x only | Incompatible with Nuke 15/16; do not mix |
| C++17 features (structured bindings, `if constexpr`, `std::optional`) | Nuke 15+ NDK headers | NDK 15+ headers themselves use C++17; safe to use |
| `OutputContext(frame, view)` two-arg constructor | Nuke 15.x only | Deprecated in 16; wrap in a helper that copies from `Op::outputContext()` |
| OpenGL immediate mode (`glBegin`/`glEnd`) | All Nuke versions up to 16 | Nuke 16 on macOS switched to FoundryGL; on Linux/Windows immediate mode remains valid for handle drawing |
| `Axis_knob` Matrix4 storage | All NDK versions | No change in Nuke 16 |
| `DeepInPlaceOutputPlane` | All NDK versions | No change in Nuke 16 |

---

## Sources

- [Nuke Developer Kit (NDK) 16.0.8 Reference](https://learn.foundry.com/nuke/developers/16.0/ndkreference/) — NDK 16 API index (MEDIUM confidence; index page only, class details require navigation)
- [Knob Types — NDK Developers Guide (14.0)](https://learn.foundry.com/nuke/developers/140/ndkdevguide/knobs-and-handles/knobtypes.html) — `ChannelMask_knob` deprecation, `ChannelSet_knob`, `Input_Channel_knob` descriptions (HIGH confidence; official Foundry documentation)
- [Deep to 2D Ops — NDK Developers Guide (11.2)](https://learn.foundry.com/nuke/developers/11.2/ndkdevguide/deep/deepto2d.html) — `Iop` pattern for deep-to-2D, `test_input()` with `DeepOp*`, `engine()` implementation pattern (HIGH confidence; official)
- [ViewerContext Class Reference — NDK 13.2](https://learn.foundry.com/nuke/developers/130/ndkreference/Plugins/classDD_1_1Image_1_1ViewerContext.html) — `transform_mode()`, `draw_lines()`, `event()`, `node_color()`, `add_draw_handle()` signatures (HIGH confidence; official)
- [Writing Nuke C++ Plugins Part 4 — Custom Knobs, GL Handles](https://erwanleroy.com/writing-nuke-c-plugins-ndk-part-4-custom-knobs-gl-handles-and-the-table-knob/) — `build_handle`/`draw_handle` pattern, `begin_handle`/`end_handle`, `glBegin(GL_LINE_LOOP)` usage (MEDIUM confidence; community tutorial, corroborated by official ViewerContext reference)
- [Release Notes for Nuke 16.0v1](https://learn.foundry.com/nuke/content/release_notes/16.0/nuke_16.0v1_releasenotes.html) — `RTLD_DEEPBIND`, `OutputContext` deprecation, Rocky Linux 9, VFX Platform CY2024 dependency versions (HIGH confidence; official Foundry release notes)
- [Linux NDK Appendix — NDK Developers Guide (15.0)](https://learn.foundry.com/nuke/developers/150/ndkdevguide/appendixa/linux.html) — GCC 11.2.1, `gcc-toolset-11`, `_GLIBCXX_USE_CXX11_ABI=1`, Rocky Linux 8.6 for Nuke 15 (HIGH confidence; official; Nuke 16 on Rocky 9 extrapolated from release notes)
- [VFX Reference Platform — Home](https://vfxplatform.com/) — CY2024: GCC 11.2.1, C++17, new libstdc++ ABI (HIGH confidence; authoritative VFX industry spec)
- [gillesvink/NukeDockerBuild — GitHub](https://github.com/gillesvink/NukeDockerBuild) — Docker images for Nuke NDK plugin builds; Rocky Linux for Nuke 15+; image naming pattern `nukedockerbuild:{VERSION}-linux` (MEDIUM confidence; third-party project, well-maintained, community recommended)
- [gillesvink/NukePluginTemplate — GitHub](https://github.com/gillesvink/NukePluginTemplate) — GitHub Actions workflow pattern for Nuke NDK CI builds (MEDIUM confidence; example project, no YAML visible in fetch)
- [NVIDIA GPU Gems 3, Ch. 28 — Practical Post-Process Depth of Field](https://developer.nvidia.com/gpugems/gpugems3/part-iv-image-effects/chapter-28-practical-post-process-depth-field) — Scatter-as-gather CoC algorithm description (HIGH confidence; authoritative GPU/algorithm reference)
- [Interpreting OpenEXR Deep Pixels](https://openexr.com/en/latest/InterpretingDeepPixels.html) — Deep sample ordering, compositing semantics for defocus (HIGH confidence; OpenEXR project official docs)

---

*Stack research for: DeepC — Nuke NDK deep-compositing plugin suite, milestone additions*
*Researched: 2026-03-12*
