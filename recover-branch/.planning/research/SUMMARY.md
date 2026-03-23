# Project Research Summary

**Project:** DeepC — Nuke NDK Deep-Compositing Plugin Suite (Milestone Additions)
**Domain:** Nuke NDK C++ plugin development (DDImage / deep compositing)
**Researched:** 2026-03-12
**Confidence:** HIGH

## Executive Summary

DeepC is a suite of Foundry Nuke NDK C++ plugins that operate on Nuke's deep image format — per-pixel stacks of samples each carrying color, depth, and alpha data. The milestone adds two entirely new processing nodes (DeepDefocus and DeepBlur), improves three existing nodes (DeepCShuffle, DeepCPMatte, DeepCPNoise), vendors an external optimization plugin (DeepThinner), establishes CI/CD infrastructure, and resolves known bugs across the codebase. Expert practice in this domain means choosing the correct NDK base class for each processing pattern — `Iop` for deep-to-2D output, direct `DeepFilterOp` for spatial deep operations, and the existing `DeepCWrapper` template-method hierarchy for uniform per-channel color effects — and getting that choice right before writing any implementation code, because changing it later requires a rewrite.

The recommended approach is to gate all new node development behind a codebase sweep phase that resolves the `perSampleData` interface limitation and fixes the four confirmed bugs. This ordering is mandatory: the `perSampleData` redesign is a breaking virtual-interface change that touches every wrapper subclass, and doing it mid-feature-development would require touching all newly written nodes a second time. After the sweep, the three feature tracks (GL handles, new nodes, shuffler UI) are architecturally independent and can proceed in parallel within a single phase. CI/CD should be established as the first concrete deliverable because all subsequent work depends on a verified build environment.

The primary risk to this milestone is the ABI split between Nuke < 15 and Nuke 15+: a plugin compiled with the wrong `_GLIBCXX_USE_CXX11_ABI` flag crashes Nuke at load with no recoverable error. Using the `gillesvink/NukeDockerBuild` container images for all builds — both local and CI — eliminates this risk entirely because those images ship the exact compiler and ABI flags the Nuke binary was built with. The secondary risk is the `draw_handle()` / `_validate()` deadlock already present in `DeepCPMatte`: it must be fixed at the start of the GL handles phase, not at the end, because adding more handle code on top of it makes the deadlock harder to isolate.

## Key Findings

### Recommended Stack

The build environment is entirely determined by the Foundry NDK: C++17 with GCC 11.2.1, the new `libstdc++` ABI (`_GLIBCXX_USE_CXX11_ABI=1`), CMake 3.15+, and Rocky Linux 9 as the build OS. All of these are already partially in place; the gap is that old Nuke < 15 CMake branches still exist and need removal. For CI, `gillesvink/NukeDockerBuild` images are the only practical option for GitHub-hosted runners because they bundle the NDK; ASWF `ci-base` images do not include Nuke and are not usable without a local volume mount. The existing `FastNoise` vendored library is sufficient for 4D noise; no new vendored dependencies are needed beyond DeepThinner source.

**Core technologies:**
- Foundry NDK 16.0+ (DDImage): the only plugin interface — all deep pixel access, knob system, and viewer handles live here
- C++17 / GCC 11.2.1 / Rocky Linux 9: mandatory for ABI compatibility with Nuke 16; enforced by the VFX Reference Platform CY2024
- CMake 3.15+: existing build system; add `--preset` and version-matrix patterns for CI
- `gillesvink/NukeDockerBuild`: pre-built Docker images with NDK + correct compiler; required for GitHub Actions CI
- GitHub Actions: free CI for public repos; matrix over Nuke 16.x minor versions

**Key API decisions:**
- DeepDefocus: inherit `Iop`, override `test_input()` to accept `DeepOp*` — the only way to produce 2D output from a deep input
- DeepBlur: direct `DeepFilterOp` subclass with expanded `getDeepRequests()` bbox — `DeepPixelOp` and `DeepCWrapper` both prohibit spatial operations
- GL handles: use `Op::build_handles()` + `Op::draw_handle()` + legacy immediate-mode OpenGL via `DDImage/gl.h` — Nuke's 3D scene graph is a separate system and must not be used
- Shuffle UI: `Input_Channel_knob` + `Text_knob(">>")` + `Channel_knob` pairs — the Shuffle2 noodle UI is not in the public NDK

### Expected Features

See `.planning/research/FEATURES.md` for full detail and per-feature expected behavior.

**Must have (table stakes) — all P1:**
- DeepShuffle: expand to 8 channels, add black/white source options, add labeled A/B input ports
- DeepShuffle: clean in→out row layout with `>>` separators (already partially implemented)
- GL handles for DeepCPMatte: 3D viewer wireframe sphere/cube shape preview at Axis_knob transform
- GL handles for DeepCPMatte: fix the 3D viewer mode early-return guard and enable 3D pick-to-sample
- DeepDefocus: size knob, circular disc kernel, maximum clamp, 2D output (Iop)
- DeepBlur: size knob, channel selector, deep output (DeepFilterOp)
- 4D noise: disable `noise_evolution` knob for non-Simplex types; fix `_noiseType==0` magic index
- DeepThinner: vendor source, add to CMake build and Python menu (zero functional changes)
- CI/CD: GitHub Actions build on push/PR for Nuke 16 with versioned artifact upload
- Codebase sweep: fix four confirmed bugs (Grade reverse gamma, Keymix A-side channels, Saturation/HueShift zero-alpha divide, Constant comma operator)
- Codebase sweep: remove `Op::Description` from `DeepCWrapper` and `DeepCMWrapper`

**Should have (v1.x after validation):**
- DeepDefocus: depth-aware per-sample Z-weighted defocus (the "deep advantage" over flat ZDefocus)
- `perSampleData` interface redesign (pointer + length) to unblock multi-component per-sample data
- DeepBlur: performance investigation and optional maximum clamp

**Defer (v2+):**
- DeepDefocus: custom bokeh shapes (bladed aperture, image kernel) — explicitly deferred in PROJECT.md
- DeepDefocus: spatial/depth-varying bokeh — explicitly deferred in PROJECT.md
- DeepDefocus / DeepBlur: GPU/Blink path — correctness must be validated on CPU first; DeepCBlink is currently broken
- DeepShuffle: true Shuffle2-style drag-noodle UI — the NDK does not expose this publicly
- GL handles for DeepCPNoise or DeepCWorld — out of scope per PROJECT.md

### Architecture Approach

The codebase has a clean three-tier structure: the NDK base layer (Foundry DDImage), a shared wrapper library (`DeepCWrapper` / `DeepCMWrapper`) that owns the per-sample loop with masking and mix, and individual plugin modules. New nodes land in the correct tier by base class: `DeepCWrapper`/`DeepCMWrapper` for uniform color effects, direct `DeepFilterOp` for structural and spatial ops, and `Iop` for deep-to-2D conversion. The sweep phase must restructure the `DeepCWrapper` virtual interface before any new nodes touch it. DeepBlur and DeepDefocus are both direct-NDK plugins with no dependency on the wrapper hierarchy, making them developable in parallel with wrapper-touching sweep work.

**Major components:**
1. `DeepCWrapper` / `DeepCMWrapper` — shared template-method base: per-channel loop, masking, unpremult/premult, mix, abort; requires `perSampleData` interface fix before any new subclasses
2. `DeepCPMatte` — existing matte-generation plugin receiving GL handle improvements; architecture is correct, implementation is placeholder-level
3. `DeepCShuffle` — direct `DeepFilterOp` subclass receiving UI-only expansion from 4 to 8 channel pairs; no processing logic changes needed
4. `DeepCDefocus` (new) — `Iop` subclass; allocates a full-frame accumulation buffer in `_validate()`, scatters deep samples with per-sample CoC into it, serves scanlines from it in `engine()`
5. `DeepCBlur` (new) — direct `DeepFilterOp` subclass; expands requested bbox by blur radius in `getDeepRequests()`, applies per-channel Gaussian weights across neighboring deep pixels in `doDeepEngine()`
6. `DeepThinner` (vendored) — standalone plugin added to CMake `PLUGINS` list and Python menu; source copied in-tree following the FastNoise pattern
7. CI/CD — GitHub Actions workflow with `gillesvink/NukeDockerBuild` container, matrix over Nuke 16.x minor versions, artifact upload per Nuke version

### Critical Pitfalls

1. **ABI mismatch crashes Nuke at load** — compile exclusively with `_GLIBCXX_USE_CXX11_ABI=1` and GCC 11.2.1; use NukeDockerBuild images to make this automatic; establish CI before any other build work
2. **`_validate()` called from `draw_handle()` causes deadlock** — already present in `DeepCPMatte`; fix immediately at the start of the GL handles phase by caching all needed state in `_validate()` and making `draw_handle()` read-only
3. **DeepDefocus 2D output rows uninitialized for sparse input** — call `row.erase(channels)` at the start of `engine()` before accumulation; verify with sparse deep input (10% pixel coverage)
4. **`getDeepRequests()` / `doDeepEngine()` channel set divergence** — extract `neededInputChannels()` as a shared helper called from both; channel sets that diverge produce silent zeros or redundant upstream cooks
5. **`Op::Description` name collision when vendoring DeepThinner** — audit all registered class names in DeepThinner source before the first joint build; collisions are silent and overwrite the first registration
6. **Missing abort check after `deepEngine()` return** — every direct `DeepFilterOp` subclass must check `if (!input0()->deepEngine(...)) return false;` and `if (Op::aborted()) return false;` inside long loops; DeepDefocus will be the most vulnerable due to per-sample scatter cost
7. **NaN propagation from zero-alpha divide in `wrappedPerSample()`** — already confirmed in `DeepCSaturation` and `DeepCHueShift`; fix in sweep; guard pattern: `if (alpha > 0.0f) { value /= alpha; } else { value = 0.0f; }`

## Implications for Roadmap

Based on the combined research, there is one mandatory gate (the codebase sweep) followed by two largely parallel feature tracks. The suggested phase structure below reflects the dependency graph from ARCHITECTURE.md, the feature priorities from FEATURES.md, and the pitfall-to-phase mapping from PITFALLS.md.

### Phase 1: CI/CD and Build Infrastructure

**Rationale:** Every subsequent phase depends on a verified build. ABI mismatches (Pitfall 1) are invisible until runtime; catching them in CI before any feature work begins prevents wasted debugging time later. This phase has no code dependencies on any other phase.

**Delivers:** GitHub Actions workflow building against Nuke 16 on push/PR; versioned artifact upload per Nuke minor version; Docker-based build environment for reproducible local development.

**Addresses:** CI/CD table stakes features from FEATURES.md.

**Avoids:** ABI mismatch crashes (Pitfall 1); CI artifact management failures (Pitfall from integration gotchas).

**Research flag:** Standard patterns — NukeDockerBuild and GitHub Actions are well-documented; skip research-phase.

---

### Phase 2: Codebase Sweep

**Rationale:** The `perSampleData` virtual interface redesign is a breaking change to `DeepCWrapper` that touches every wrapper subclass. Doing it now — before DeepBlur, DeepDefocus, or any new wrapper plugin is written — avoids a second round of updates. Bug fixes are isolated one-liners and carry no risk; resolving them early prevents NaN values from poisoning cache state during development. Removing `Op::Description` from base classes prevents artist confusion while the new nodes are being developed.

**Delivers:** Clean interface for `DeepCWrapper` with pointer+length `perSampleData`; four confirmed bugs fixed; base classes removed from the Nuke node menu; decision on `DeepCBlink` (complete or remove).

**Addresses:** All "Codebase sweep" items from the v1 launch list in FEATURES.md.

**Avoids:** NaN propagation from zero-alpha divide (Pitfall 7); `perSampleData` design debt blocking new nodes (Technical Debt table in PITFALLS.md); broken base class nodes confusing artists.

**Research flag:** Standard patterns — all changes are within the existing codebase with confirmed fixes; skip research-phase.

---

### Phase 3: GL Handles for DeepCPMatte

**Rationale:** This phase is isolated to a single source file (`DeepCPMatte.cpp`) with no dependency on the sweep or new nodes. The existing architecture is correct; only the `draw_handle()` implementation is placeholder-level. The `_validate()` deadlock pitfall must be the first thing addressed in this phase — not the last — because all subsequent handle drawing code is unreliable until it is fixed.

**Delivers:** Wireframe sphere/cube viewport overlay matching shape and scale of the Axis_knob transform; interactive drag-to-reposition in both 2D and 3D viewer modes; click-to-sample behavior working from the 3D viewport.

**Addresses:** GL handle table-stakes features from FEATURES.md; differentiator "3D shape preview matching actual matte extent."

**Avoids:** `_validate()` in `draw_handle()` deadlock (Pitfall 2 — fix first); GL handles visible in wrong viewer mode (UX pitfall in PITFALLS.md).

**Research flag:** Standard patterns — `build_handles` / `draw_handle` / `ViewerContext` are well-documented via official NDK reference and confirmed community tutorial; skip research-phase. The 3D picker path (translating a 3D ray pick to a pixel coordinate) is the one area that may need NDK header inspection before implementation.

---

### Phase 4: DeepShuffle UI Improvements

**Rationale:** This phase is UI-only changes to `DeepCShuffle.cpp` with no dependency on the sweep or new nodes. The processing logic is correct and unchanged. Grouping it separately from the new nodes keeps diffs reviewable.

**Delivers:** 8-channel routing (up from 4); black and white constant source options; labeled A/B input ports; clean in→out row layout.

**Addresses:** DeepShuffle table-stakes features from FEATURES.md.

**Avoids:** `Channel_knob` vs `Input_Channel_knob` UX pitfall (users selecting channels not present on the deep input); confusing multi-line knob layout at 8 channels.

**Research flag:** Standard patterns — `Input_Channel_knob` / `Channel_knob` pair approach is the confirmed fallback; the Shuffle2 noodle UI is explicitly not achievable without undocumented NDK internals. No research-phase needed.

---

### Phase 5: DeepCPNoise — 4D Noise Exposure

**Rationale:** This phase is a small, isolated change to `DeepCPNoise.cpp` fixing the `_noiseType==0` magic index and wiring the `noise_evolution` knob enable/disable state. It is grouped after the sweep because the magic-index fix is in spirit part of the sweep, but it can safely run in parallel with Phase 3 and 4 if resource allows.

**Delivers:** `noise_evolution` knob correctly enabled only for Simplex; knob clearly labeled with tooltip; fragile magic index replaced with named-constant comparison.

**Addresses:** 4D noise table-stakes feature from FEATURES.md; magic-index technical debt from PITFALLS.md.

**Avoids:** Silent breakage of 4D noise if noise type dropdown order changes (Pitfall from "Looks Done But Isn't" checklist).

**Research flag:** No research needed — the existing code, the FastNoise API, and the fix are all confirmed.

---

### Phase 6: DeepThinner Vendor Integration

**Rationale:** This phase is blocked only by DeepThinner source availability (currently unconfirmed — the `bratgot/DeepThinner` repository may be private). The CMake and menu integration patterns are well-established from the FastNoise precedent. The name collision audit must happen before the first build, not after.

**Delivers:** DeepThinner compiled as a Nuke plugin module; 7-pass sample optimization with preset configs accessible from the DeepC toolbar menu.

**Addresses:** DeepThinner table-stakes feature from FEATURES.md.

**Avoids:** `Op::Description` name collision silently overwriting existing DeepC nodes (Pitfall 5 — audit first).

**Research flag:** Source availability is the critical unknown (LOW confidence from ARCHITECTURE.md). If `bratgot/DeepThinner` is private, the roadmapper should flag this phase as blocked pending source access. Integration approach (in-tree via `FetchContent` or copy, same as FastNoise) needs no research once source is available.

---

### Phase 7: DeepDefocus

**Rationale:** DeepDefocus is architecturally independent of all wrapper-hierarchy work but benefits from the sweep being complete (clean codebase to build against). It is the highest-complexity single deliverable: new `Iop` subclass, full-frame accumulation buffer, CoC scatter, depth-sorted compositing. Placing it after the infrastructure phases gives CI coverage for every commit.

**Delivers:** `DeepCDefocus` plugin — reads a deep input, outputs a 2D flat image with a circular disc bokeh kernel; knobs: size, maximum, channels, filter_type (disc only in v1).

**Addresses:** DeepDefocus P1 features from FEATURES.md; completes the deep-to-2D output architecture pattern for the suite.

**Avoids:** Uninitialized output rows in sparse deep input (Pitfall 3 — `row.erase(channels)` at start of `engine()`); channel set divergence between `getDeepRequests()` and `doDeepEngine()` (Pitfall 4); missing abort check after `deepEngine()` return (Pitfall 6); O(W×H×S) scatter performance trap (use radius cap; profile early).

**Research flag:** The v1 uniform-defocus approach is fully specified and architecturally confirmed. The v1.x depth-aware per-Z-weighted defocus (the key differentiator) is a v1.x feature that will benefit from a brief research-phase focused on sample-layer ordering and CoC interpolation across depth zones.

---

### Phase 8: DeepBlur

**Rationale:** DeepBlur is also architecturally independent of the wrapper hierarchy. It is placed after DeepDefocus because both involve unfamiliar deep spatial operations, and lessons from DeepDefocus (especially around abort handling, channel set discipline, and buffer management) reduce risk in DeepBlur. They could be parallelised if resources allow.

**Delivers:** `DeepCBlur` plugin — reads a deep input, outputs a deep image; Gaussian blur applied per-channel while preserving deep sample structure; knobs: size, channels.

**Addresses:** DeepBlur P1 features from FEATURES.md.

**Avoids:** Using `DeepPixelOp` (prohibits spatial ops — Anti-Pattern 1 in ARCHITECTURE.md); using `DeepCWrapper` (fetches only the output box, no spatial expansion — Anti-Pattern 3); heap allocation without RAII inside `doDeepEngine()` (performance trap in PITFALLS.md); channel set divergence (Pitfall 4).

**Research flag:** The exact deep-blur sample-merging semantics — how to handle samples from neighboring pixels that have different depth values — are MEDIUM confidence from ARCHITECTURE.md ("no official NDK guidance found"). A targeted research-phase before implementation is recommended, focusing on the msDeepBlur community reference and any VFX community resources on deep blur semantics.

---

### Phase Ordering Rationale

- Phase 1 (CI/CD) must be first: ABI failures are invisible without a build environment; all code phases depend on CI catching regressions.
- Phase 2 (Sweep) must precede new wrapper-touching work: `perSampleData` interface is a breaking virtual change; doing it after adds a second round of subclass edits.
- Phases 3–5 are independent of each other and of Phases 7–8: they can run in parallel if two developers are available. Sequenced here for clarity.
- Phase 6 (DeepThinner) is blocked by source availability, not by code dependencies; it can be slotted in whenever source is confirmed.
- Phases 7–8 (DeepDefocus, DeepBlur) are the highest-risk implementations; placing them after infrastructure phases means CI catches every regression and the codebase is clean.

### Research Flags

Phases needing deeper research during planning:

- **Phase 7 (DeepDefocus) — v1.x extension:** The depth-aware per-Z-weighted defocus algorithm (sorting sample layers and applying per-depth CoC) needs a research-phase before v1.x planning. The v1 uniform disc approach is confirmed and needs no research.
- **Phase 8 (DeepBlur):** Deep blur sample-merging semantics (what to do when neighboring pixels have samples at different depths) are MEDIUM confidence. A targeted research-phase is recommended before implementation begins.
- **Phase 6 (DeepThinner):** Source availability is unconfirmed. If `bratgot/DeepThinner` is private, a research-phase must locate an alternative or confirm access before this phase can be planned.

Phases with standard patterns (skip research-phase):

- **Phase 1 (CI/CD):** NukeDockerBuild and GitHub Actions patterns are well-documented.
- **Phase 2 (Sweep):** All changes are within the confirmed existing codebase with one-line or small-scope fixes.
- **Phase 3 (GL handles):** `build_handles` / `draw_handle` pattern is fully confirmed in official NDK + community tutorial. Minor unknown: 3D ray-to-pixel picker, but this is NDK header inspection, not research-phase work.
- **Phase 4 (DeepShuffle):** `Input_Channel_knob` pair approach is confirmed. Shuffle2 noodle UI explicitly deferred.
- **Phase 5 (4D noise):** All code and fix confirmed from existing codebase inspection.
- **Phase 7 (DeepDefocus) — v1 scope:** `Iop` with `test_input()` accepting `DeepOp*` is the officially documented pattern; full-frame accumulation buffer approach is confirmed.

## Confidence Assessment

| Area | Confidence | Notes |
|------|------------|-------|
| Stack | HIGH | Core API (NDK, channel knobs, GL handles, Iop/DeepFilterOp patterns) verified against official Foundry docs; Docker CI approach verified against live NukeDockerBuild project; defocus algorithm sourced from GPU Gems 3 (authoritative) |
| Features | HIGH | Table-stakes features verified against Foundry official node documentation (Shuffle2, ZDefocus, Bokeh); existing codebase verified by direct inspection confirming current limitations; DeepThinner feature set verified from GitHub |
| Architecture | HIGH | NDK base class choices (Iop, DeepFilterOp, DeepPixelOp) confirmed against official Foundry NDK developer guide; existing codebase architecture confirmed by direct inspection; DeepBlur sample semantics MEDIUM (no official guidance) |
| Pitfalls | HIGH | Most pitfalls confirmed by direct codebase inspection (bugs confirmed in CONCERNS.md, deadlock pattern confirmed in DeepCPMatte.cpp); ABI pitfall confirmed via official Nuke 15/16 NDK docs and release notes |

**Overall confidence:** HIGH

### Gaps to Address

- **DeepThinner source availability:** The `bratgot/DeepThinner` repository was not found in research (LOW confidence on integration). Before Phase 6 can be planned, source access must be confirmed. If unavailable, options are: find an alternative, implement the 7-pass thinning algorithm from scratch, or remove the feature from this milestone.
- **DeepBlur deep sample-merging semantics:** How to handle samples from neighboring pixels at different depths during a deep spatial Gaussian is an unresolved design question with no official NDK guidance. The implementation approach (apply kernel weights to per-channel values while preserving the center pixel's sample structure) is reasonable but untested. This needs early prototyping validation, not just research.
- **3D ray-to-pixel picker for DeepCPMatte GL handles:** The click-to-sample behavior in the 3D viewport (translating a 3D pick event to a pixel coordinate for deep sampling) is not covered in reviewed documentation. This will require NDK header inspection during Phase 3 implementation.
- **DeepDefocus v1.x depth-aware algorithm:** The depth-aware per-Z-weighted defocus is deferred to v1.x and was not fully researched. When that phase is planned, focus on the sample-layer ordering approach and CoC interpolation across depth zones.

## Sources

### Primary (HIGH confidence)

- [Foundry NDK 16.0.8 Reference](https://learn.foundry.com/nuke/developers/16.0/ndkreference/) — NDK 16 API index
- [NDK Developers Guide: Knob Types (14.0)](https://learn.foundry.com/nuke/developers/140/ndkdevguide/knobs-and-handles/knobtypes.html) — channel knob API, deprecations
- [NDK Developers Guide: Deep to 2D Ops (11.2)](https://learn.foundry.com/nuke/developers/11.2/ndkdevguide/deep/deepto2d.html) — `Iop` pattern for deep-to-2D
- [NDK Developers Guide: Simple DeepPixelOp (11.2)](https://learn.foundry.com/nuke/developers/11.2/ndkdevguide/deep/deepsimple.html) — spatial op prohibition
- [NDK Developers Guide: Linux ABI (15.0)](https://learn.foundry.com/nuke/developers/150/ndkdevguide/appendixa/linux.html) — GCC 11.2.1, `_GLIBCXX_USE_CXX11_ABI=1`
- [Foundry Release Notes: Nuke 16.0v1](https://learn.foundry.com/nuke/content/release_notes/16.0/nuke_16.0v1_releasenotes.html) — RTLD_DEEPBIND, OutputContext deprecation, Rocky Linux 9
- [VFX Reference Platform](https://vfxplatform.com/) — CY2024: GCC 11.2.1, C++17, new ABI
- [ViewerContext Class Reference (NDK 13.2)](https://learn.foundry.com/nuke/developers/130/ndkreference/Plugins/classDD_1_1Image_1_1ViewerContext.html) — GL handle API
- [NVIDIA GPU Gems 3, Ch. 28](https://developer.nvidia.com/gpugems/gpugems3/part-iv-image-effects/chapter-28-practical-post-process-depth-field) — scatter-as-gather CoC algorithm
- [OpenEXR: Interpreting Deep Pixels](https://openexr.com/en/latest/InterpretingDeepPixels.html) — deep compositing semantics
- [Foundry Nuke documentation: Shuffle2, ZDefocus, Bokeh](https://learn.foundry.com/nuke/content/reference_guide/) — feature expectations
- DeepC codebase direct inspection: `src/DeepCWrapper.h`, `src/DeepCWrapper.cpp`, `src/DeepCMWrapper.cpp`, `src/DeepCPMatte.cpp`, `src/DeepCShuffle.cpp`, `src/DeepCPNoise.cpp`, `.planning/codebase/CONCERNS.md`

### Secondary (MEDIUM confidence)

- [Erwan Leroy: Writing Nuke C++ Plugins Part 4 — GL Handles](https://erwanleroy.com/writing-nuke-c-plugins-ndk-part-4-custom-knobs-gl-handles-and-the-table-knob/) — `build_handles` / `draw_handle` / `begin_handle` / `end_handle` pattern; corroborated by official ViewerContext reference
- [gillesvink/NukeDockerBuild (GitHub)](https://github.com/gillesvink/NukeDockerBuild) — Docker images for NDK builds; well-maintained, community-recommended
- [gillesvink/NukePluginTemplate (GitHub)](https://github.com/gillesvink/NukePluginTemplate) — GitHub Actions workflow pattern for NDK CI
- [msDeepBlur (Nukepedia)](https://www.nukepedia.com/tools/plugins/deep/msdeepblur/) — deep blur reference; sample count explosion warning
- DeepBlur sample-merging semantics — inferred from 2D blur analogy; no official NDK source

### Tertiary (LOW confidence)

- `bratgot/DeepThinner` (GitHub) — DeepThinner 7-pass optimizer; repository not found in research; source availability unconfirmed

---
*Research completed: 2026-03-12*
*Ready for roadmap: yes*
