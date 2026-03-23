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

# Architecture Research

**Domain:** Nuke NDK C++ deep-compositing plugin suite (DeepC milestone additions)
**Researched:** 2026-03-12
**Confidence:** HIGH (NDK patterns verified against Foundry official docs + direct codebase inspection)

---

## System Overview

The existing architecture has three layers a new component can land in. Every new feature must
choose the right layer and the right NDK base class before any code is written.

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        NDK BASE LAYER (Foundry DDImage)                     │
│  DeepFilterOp   DeepPixelOp   Iop   Op   ViewerContext   DeepOp interface   │
└──────────────────────────────┬──────────────────────────────────────────────┘
                               │ inherits / wraps
┌──────────────────────────────▼──────────────────────────────────────────────┐
│                     SHARED WRAPPER LIBRARY (DeepC-owned)                    │
│                                                                             │
│   DeepCWrapper (DeepFilterOp subclass)                                      │
│   └─ DeepCMWrapper (DeepCWrapper subclass, adds matte-op semantics)         │
│                                                                             │
│   [Optional planned addition: GradeCoefficients utility struct/mixin]       │
└────────────┬──────────────────────────────┬────────────────────────────────┘
             │ 11 color-effect plugins       │ 3 matte-gen plugins
┌────────────▼──────────┐        ┌───────────▼─────────────────────────────┐
│   PLUGINS_WRAPPED     │        │          PLUGINS_MWRAPPED               │
│   (DeepCWrapper base) │        │          (DeepCMWrapper base)           │
│   DeepCAdd            │        │   DeepCID  DeepCPNoise  DeepCPMatte     │
│   DeepCGrade          │        │   [GL handles live in DeepCPMatte only] │
│   DeepCSaturation     │        └─────────────────────────────────────────┘
│   DeepCHueShift etc.  │
└───────────────────────┘
┌──────────────────────────────────────────────────────────────────────────────┐
│   PLUGINS (direct NDK base, no wrapper)                                      │
│   DeepCKeymix  DeepCAddChannels  DeepCShuffle  DeepCWorld  DeepCConstant ... │
│                                                                              │
│   NEW: DeepBlur (DeepFilterOp direct)                                        │
│   NEW: DeepDefocus (Iop with DeepOp input)                                   │
│   NEW: [DeepThinner — same layer, same category if integrated]               │
└──────────────────────────────────────────────────────────────────────────────┘
┌──────────────────────────────────────────────────────────────────────────────┐
│   THIRD-PARTY LAYER                                                          │
│   FastNoise (vendored in-tree)   DeepThinner (to be vendored)                │
└──────────────────────────────────────────────────────────────────────────────┘
```

---

## Component Responsibilities and Boundaries

| Component | Responsibility | Communicates With |
|-----------|---------------|-------------------|
| `DeepCWrapper` | Per-channel color pipeline: masking, unpremult/premult, mix, abort | Nuke runtime (DDImage), subclasses via virtual hooks |
| `DeepCMWrapper` | Extends wrapper for matte generation; adds operation knob (replace/union/mask/stencil/min/max) | `DeepCWrapper` (parent), subclasses |
| `DeepCPMatte` | Position-based spherical/cubic matte; owns GL handle drawing | `DeepCMWrapper` (parent), Nuke viewer (ViewerContext, OpenGL) |
| `DeepCShuffle` | Per-sample channel remapping; direct `DeepFilterOp` subclass | Nuke runtime only |
| `DeepDefocus` (new) | Depth-sorted circle-of-confusion scatter; deep-in → 2D-out | Nuke runtime via `Iop::engine()` + `DeepOp` input |
| `DeepBlur` (new) | Spatial gaussian/box blur on deep samples; deep-in → deep-out | Nuke runtime via `DeepFilterOp::doDeepEngine()` |
| `DeepThinner` (vendored) | External thin-deep algorithm; standalone plugin | Nuke runtime, CMake build only |
| FastNoise | Noise generation algorithms | `DeepCPNoise` only |
| CMake build system | Compiles plugins as MODULE targets; generates menu.py | Developer / CI |
| Python menu | Registers plugins into Nuke toolbar at startup | Nuke Python runtime |

---

## Architectural Patterns

### Pattern 1: Template Method via DeepCWrapper (existing, proven)

**What:** Base class owns the entire `doDeepEngine()` loop. Subclasses inject behavior at two points:
`wrappedPerSample()` (once per deep sample, channel-agnostic) and `wrappedPerChannel()` (once per
channel per sample).

**When to use:** Any plugin that applies a color transformation uniformly across the requested
channel set, with optional masking and mix. This covers all 11 existing color-effect plugins.

**Trade-offs:**
- Pro: masking, unpremult, mix, abort-check for free
- Pro: consistent knob layout across all wrapper plugins
- Con: `perSampleData` is a single `float` — passing multi-value state from per-sample to
  per-channel requires workarounds (see DeepCHueShift's `Vector3& sampleColor` parameter reuse)
- Con: not suitable for spatial operations (needs the full input plane before per-pixel work)

**Example skeleton:**
```cpp
class DeepCFoo : public DeepCWrapper {
    void wrappedPerChannel(float inputVal, float perSampleData,
                           Channel z, float& outData, Vector3& sampleColor) override {
        outData = /* math on inputVal */;
    }
};
```

### Pattern 2: Direct DeepFilterOp Subclass (existing, for structural ops)

**What:** Plugin implements `_validate()`, `getDeepRequests()`, and `doDeepEngine()` directly,
without the wrapper pipeline. Full control over the processing loop.

**When to use:** Any plugin that is not a uniform color effect — structural changes (channel
routing, bbox ops, merge ops, spatial operations).

**New application — DeepBlur:** A spatial blur must read neighboring pixels' deep data before
writing the output plane. This requires expanding the requested bounding box in `getDeepRequests()`
(inflate the input Box by the blur radius in x and y), then using the full `DeepPlane` across the
expanded region. `DeepFilterOp` allows this; `DeepCWrapper` does not because its loop iterates
only over the output box. `DeepPixelOp` explicitly prohibits spatial operations. Therefore
`DeepBlur` must be a direct `DeepFilterOp` subclass.

**Trade-offs:**
- Pro: complete control, no wrapper overhead
- Con: must implement masking, mix, abort-check manually if wanted
- Con: more boilerplate

### Pattern 3: Iop with DeepOp Input (new for DeepDefocus)

**What:** A standard 2D `Iop` that accepts a `DeepOp` connection on input 0. The `engine()`
function (not `doDeepEngine()`) fetches a `DeepPlane` row-by-row via `deepIn->deepEngine(y, x, r,
channels, deepRow)`, then scatters or flattens samples to produce 2D output pixels.

**Why Iop and not DeepFilterOp:** `DeepFilterOp` always produces deep output. `DeepDefocus` must
produce 2D output (the scattered bokeh discs need to accumulate into a flat image, not remain as
discrete deep samples). The NDK explicitly supports this pattern: "It is possible to write a
regular 2D Iop that takes deep inputs."

**Required method overrides:**
```
test_input(int n, Op* op)   → dynamic_cast<DeepOp*>(op) != nullptr for input 0
default_input(int n)         → nullptr (deep input required)
_validate(bool)              → copy deepInfo from input; set info_ (output 2D info)
_request(int x, y, r, ...)  → call deepIn->deepRequest() on the deep input
engine(int y, x, r, cs, row)→ fetch DeepPlane via deepIn->deepEngine(y,x,r,...),
                               iterate samples, scatter/accumulate to row
```

**Trade-offs:**
- Pro: correct output type (2D), integrates naturally into Nuke's 2D node graph downstream
- Con: engine() is called per-scanline; accumulation across scanlines (e.g. for a disc that
  spans multiple rows) requires a full-frame intermediate buffer allocated in `_validate()` or
  computed row-by-row with a radius-constrained kernel

### Pattern 4: GL Handles for Viewport Interaction (existing in DeepCPMatte, to be improved)

**What:** Three virtual methods on `Op` enable custom OpenGL drawing in the Nuke viewer:
- `build_handles(ViewerContext* ctx)` — decides whether to add handles; calls
  `build_knob_handles(ctx)` for knob-driven handles and `add_draw_handle(ctx)` to register the
  custom draw callback
- `draw_handle(ViewerContext* ctx)` — executes OpenGL calls; anything drawn between
  `begin_handle()` / `end_handle()` is interactive (responds to mouse events)
- `knob_changed(Knob* k)` — reacts to knob changes that should move/update the handle

**Current state in DeepCPMatte:** Stubs exist (`build_handles`, `draw_handle`, `knob_changed`),
but the `draw_handle` implementation only draws a `GL_LINES` point at `_center` — a minimal
placeholder. The center XY knob's `knob_changed` already queries the deep input at the picked
pixel and sets the axis translate. The architecture is correct; it needs richer drawing (sphere
outline, radius indicator, falloff shell).

**Integration rules:**
- Must include `DDImage/ViewerContext.h` and `DDImage/gl.h`
- Must link the plugin module against `OpenGL::GL` in CMake (already done for DeepCPMatte via the
  `if (OpenGL_FOUND)` guard in `src/CMakeLists.txt`)
- `build_handles` should guard on `ctx->transform_mode() != VIEWER_2D` for 3D handles or the
  inverse for 2D handles — the existing guard is correct for 2D center picking
- Interactive dragging: wrap drawn geometry in `begin_handle(ctx, ...)`/`end_handle(ctx)` and
  respond to `ctx->event() == PUSH` / `DRAG` / `RELEASE` events in `draw_handle` or a registered
  static callback

---

## Data Flow

### Deep-in → Deep-out (Wrapper and Direct DeepFilterOp plugins)

```
Nuke scheduler
    │
    ▼ _validate(bool)
DeepCWrapper / DeepFilterOp
    │  validates channels, mask inputs, bbox
    │
    ▼ getDeepRequests(Box, ChannelSet, count, requests)
    │  pushes RequestData to upstream (expands Box for spatial ops)
    │
    ▼ doDeepEngine(Box, ChannelSet, DeepOutputPlane&)
    │
    ├─ input0()->deepEngine(box, neededChannels, deepInPlane)
    │     [fetches upstream DeepPlane into memory]
    │
    ├─ for each pixel in box:
    │   ├─ deepInPlane.getPixel(it) → DeepPixel
    │   ├─ for each sample:
    │   │   ├─ wrappedPerSample() [hook: matte/position math → perSampleData]
    │   │   └─ for each channel:
    │   │       └─ wrappedPerChannel() [hook: color math → outData]
    │   └─ write to DeepOutputPlane
    │
    ▼ DeepOutputPlane delivered to Nuke
```

For **DeepBlur**: `getDeepRequests()` expands the requested `Box` by ±blurRadius in x and y.
`doDeepEngine()` fetches the expanded `DeepPlane`, then for each output pixel samples the
surrounding deep pixels to compute weighted output samples.

### Deep-in → 2D-out (DeepDefocus — Iop pattern)

```
Nuke scheduler
    │
    ▼ _validate(bool)
DeepDefocus (Iop subclass)
    │  copies format/bbox from deepIn->deepInfo()
    │  sets info_.channels to output channel set (e.g. RGBA)
    │
    ▼ _request(int x, y, r, ChannelSet, count)
    │  deepIn->deepRequest(Box, channels, count)
    │
    ▼ engine(int y, int x, int r, ChannelSet, Row& row)
    │
    ├─ deepIn->deepEngine(y, x, r, neededChannels, deepRow)
    │     [fetches one scanline's worth of DeepPlane]
    │
    ├─ for each pixel x..r in the scanline:
    │   ├─ deepRow.getPixel(y, x) → DeepPixel
    │   ├─ for each sample (front-to-back, sorted by depth):
    │   │   ├─ compute circle-of-confusion radius from sample depth + focus params
    │   │   └─ splat sample's color into row (or accumulation buffer)
    │   └─ write accumulated result to row[channel][x]
    │
    ▼ Row delivered to Nuke (2D pixels)
```

Note: Row-by-row processing means a sample's CoC disc that spans multiple scanlines cannot be
trivially accumulated using only the current scanline's data. For v1 (CPU, correctness first),
the recommended approach is a full-frame pre-pass: in `_validate()`, allocate an RGBA buffer
sized to the output format; in `engine()` for y==bbox.y() (first scanline), scatter all samples
into the buffer; for subsequent scanlines, copy from the buffer. This is the standard approach
used by Nuke's own DeepToImage-style nodes.

### Plugin Registration Flow

```
CMake build
    │ compiles each .cpp as MODULE .so
    ├─ add_nuke_plugin(DeepCFoo DeepCFoo.cpp)
    ├─ [if wrapped] target_sources(... $<TARGET_OBJECTS:DeepCWrapper>)
    └─ [if gl] target_link_libraries(... OpenGL::GL)
    │
    ▼ Nuke startup
    init.py → menu.py → create_deepc_menu()
    │  nuke.menu("Nodes").addCommand("DeepC/DeepCFoo", ...)
    │
    ▼ User creates node
    Nuke dlopen("DeepCFoo.so")
    → finds Op::Description::d registered via static initializer
    → calls build(node) → new DeepCFoo(node)
```

---

## Recommended Project Structure (additions only)

The existing `src/` flat layout is correct. New files follow the same pattern:

```
src/
├── DeepCWrapper.h / .cpp       # existing — perSampleData interface redesign here
├── DeepCMWrapper.h / .cpp      # existing — remove Op::Description here
├── DeepCPMatte.cpp             # existing — GL handle improvements here
├── DeepCShuffle.cpp            # existing — channel routing UI improvements here
│
├── DeepCDefocus.cpp            # NEW — Iop subclass, deep-in → 2D-out
├── DeepCBlur.cpp               # NEW — DeepFilterOp direct subclass, spatial
│
└── [DeepThinner vendored dir]  # NEW — see vendoring section
    DeepThinner/
    ├── CMakeLists.txt          # minimal, wraps upstream source
    └── [upstream .cpp/.h]
```

---

## Integration Architecture for Each New Component

### 1. DeepDefocus — Iop with DeepOp Input

**Base class:** `DD::Image::Iop`
**CMake category:** `PLUGINS` (non-wrapped, direct NDK)
**NDK headers:** `DDImage/Iop.h`, `DDImage/Row.h`, `DDImage/DeepOp.h`, `DDImage/DeepFilterOp.h`

Key design choices:
- `test_input(0, op)` → `dynamic_cast<DeepOp*>(op) != nullptr`
- `default_input(0)` → `nullptr` (deep connection required)
- `_validate()` derives output `format()` and `bbox()` from `deepIn->deepInfo()`, sets
  `info_.set_channels(outputChannelSet)` for RGBA or user-selected channels
- v1 approach: allocate a per-validate frame buffer (`std::vector<float>` per channel) in
  `_validate()`, scatter all samples into it in the first `engine()` call, serve subsequent rows
  from the buffer — avoids multi-pass complexity while remaining correct
- Depth sort: deep samples must be processed front-to-back; use `getOrderedSample()` (which sorts
  by `Chan_DeepFront`) or sort manually if the input plane is unordered
- CoC radius per sample: `coc = abs(sampleDepth - focusDistance) * cocScale`; clamp to max radius
- Alpha compositing: over-composite samples front-to-back into the accumulation buffer using the
  standard deep compositing formula

**Confidence:** HIGH — pattern confirmed in Foundry official NDK docs (Deep to 2D Ops page)

### 2. DeepBlur — Direct DeepFilterOp Subclass

**Base class:** `DD::Image::DeepFilterOp`
**CMake category:** `PLUGINS` (non-wrapped, direct NDK)
**NDK headers:** `DDImage/DeepFilterOp.h`, `DDImage/Knobs.h`

Key design choices:
- Cannot use `DeepCWrapper` — the wrapper's `doDeepEngine()` fetches the input plane for exactly
  the output box; a blur needs an expanded input region
- `getDeepRequests()` must inflate the requested `Box` by the blur radius in both x and y:
  ```cpp
  Box expandedBox = box;
  expandedBox.pad(_blurRadius);
  requests.push_back(RequestData(input0(), expandedBox, neededChannels, count));
  ```
- `doDeepEngine()` fetches the expanded `DeepPlane`, then for each output pixel gathers the
  surrounding deep pixels, weights each neighbor sample by a Gaussian kernel based on XY distance,
  and writes weighted output samples. Output sample count equals input sample count at the center
  pixel (samples are not merged across pixels — this is a deep blur, not a flatten-then-blur).
- Deep blur semantics: apply kernel weights to per-channel values while preserving the sample
  structure of the center pixel. Neighboring samples are blended per-channel by their kernel
  weight contribution.
- Cannot use `DeepPixelOp` — NDK docs explicitly state DeepPixelOp "provide[s] no support for
  spatial operations: each pixel on the input corresponds exactly to a pixel on the output."

**Confidence:** HIGH for base class choice. MEDIUM for exact deep blur sample-merging semantics
(no official NDK guidance found; approach matches how 2D blur works but adapted to deep stacks).

### 3. GL Handles for DeepCPMatte — Improving Existing Plugin

**No base class change required** — `DeepCPMatte` already inherits `DeepCMWrapper` and already
has the correct method stubs (`build_handles`, `draw_handle`, `knob_changed`) plus the OpenGL
CMake linkage. This is a pure implementation improvement.

**Current state:**
- `build_handles()` — correct: calls `build_knob_handles(ctx)` + `add_draw_handle(ctx)`, guards
  on `VIEWER_2D` mode
- `draw_handle()` — placeholder: draws a `GL_LINES` single point at `_center`; no sphere, no
  radius indicator, no interactivity
- `knob_changed("center")` — functional: queries deep input at picked pixel, sets axis translate

**Target state (what to add):**
- Draw sphere outline (lat/lon lines) in `draw_handle()` using `glBegin(GL_LINE_LOOP)`
- Draw falloff shell at `1.0 / _falloff` scaled radius
- Wrap geometry in `begin_handle()` / `end_handle()` to make it mouse-interactive
- In `draw_handle()`, detect `ctx->event() == PUSH` to begin a drag; `DRAG` to update `_center`
  or axis knob values; `RELEASE` to commit
- For 3D viewer support: add a branch for `ctx->transform_mode() != VIEWER_2D` to draw the same
  geometry in 3D space using the axis matrix

**Confidence:** HIGH — pattern fully confirmed in existing DeepCPMatte code + Foundry NDK docs +
community tutorial (Erwan LeRoy's Part 4)

### 4. DeepCShuffle Improvements

**No base class change required** — `DeepCShuffle` is a direct `DeepFilterOp` subclass with
correct architecture. The improvements are UI-only (knob changes).

**Current limitation:** Four fixed `Input_Channel_knob` / `Channel_knob` pairs for in0→out0
through in3→out3. This does not match Nuke's built-in Shuffle node's UX (labeled input ports,
dynamic routing rows, Shuffle2 noodle style).

**Recommended approach:**
- For Nuke 12+ Shuffle2 parity: investigate `ShuffleLayer_knob()` or equivalent NDK knob type if
  it exists in the public API. If not public, use `Script_knob` to generate dynamic Python-driven
  UI or stick with the existing fixed 4-pair layout expanded to support more channels.
- Label the two inputs clearly via `input_label()` (already standard in DeepFilterOp subclasses)
- The processing logic in `doDeepEngine()` is correct and does not need changes for basic UI parity

**Confidence:** MEDIUM — NDK does not publicly document a "Shuffle2 noodle" style knob; needs
direct NDK header inspection or Nukepedia community research before committing to approach.

### 5. DeepThinner Vendor Integration

**Search result:** No public repository found for "bratgot/DeepThinner" or any "DeepThinner" Nuke
plugin. The project may be private, renamed, or not yet published.

**Assuming source is obtained, recommended CMake integration pattern:**

Option A (preferred for small, compile-once source): Copy source files into `src/DeepThinner/` or
directly into `src/`. Add to the `PLUGINS` list in `src/CMakeLists.txt`. Use `add_nuke_plugin()`.
This is exactly how FastNoise is handled — sources live in the tree, no separate CMake project.

Option B (for larger, complex dependency): Use `FetchContent` (CMake 3.14+, already within the
project's CMake 3.15+ floor) to pull from a remote URL at configure time:
```cmake
include(FetchContent)
FetchContent_Declare(DeepThinner
    GIT_REPOSITORY https://github.com/bratgot/DeepThinner.git
    GIT_TAG        main
)
FetchContent_MakeAvailable(DeepThinner)
```
Then link the DeepThinner target into the plugin module.

Avoid `ExternalProject_Add` — it builds at build time, not configure time, making target linking
complex. `FetchContent` is simpler and is the modern CMake idiom for vendoring.

**CMake list category:** Add to `PLUGINS` in `src/CMakeLists.txt` (non-wrapped, direct NDK).
**Menu category:** Likely `Util_NODES` or a new "Depth" category alongside DeepDefocus/DeepBlur.

**Confidence:** LOW — source availability unknown. Architecture recommendation is HIGH confidence
given the existing FastNoise/DeepCBlink vendoring patterns in the codebase.

---

## Codebase Sweep: Recommended Order of Concern Resolution

The five tech-debt items in `CONCERNS.md` have ordering dependencies. Address them in this order:

**Step 1 — `perSampleData` interface redesign (DeepCWrapper.h, DeepCWrapper.cpp, DeepCMWrapper)**

Do this first because it is a breaking change to the virtual interface. Every subclass that
overrides `wrappedPerSample()` or `wrappedPerChannel()` must be updated. Doing this before
adding DeepBlur or any other new wrapper-based plugin avoids a second round of updates.

Recommended signature change:
```cpp
// Before:
virtual void wrappedPerSample(..., float& perSampleData, Vector3& sampleColor);
virtual void wrappedPerChannel(float inputVal, float perSampleData, Channel z,
                               float& outData, Vector3& sampleColor);
// After:
virtual void wrappedPerSample(..., float* perSampleData, int perSampleDataLen,
                               Vector3& sampleColor);
virtual void wrappedPerChannel(float inputVal, const float* perSampleData,
                               int perSampleDataLen, Channel z,
                               float& outData, Vector3& sampleColor);
```
Default length = 1, keeping existing behavior for plugins that use the single float.

**Step 2 — Remove `Op::Description` from DeepCWrapper and DeepCMWrapper**

Remove the `build()` functions and `Op::Description` static members from `src/DeepCWrapper.cpp`
and `src/DeepCMWrapper.cpp`. These are abstract base classes that should never appear in Nuke's
node menu. This change is non-breaking to subclasses and has zero runtime risk.

**Step 3 — Extract grade coefficient utility**

After the interface is stabilised (Step 1), extract the duplicated `A[]`, `B[]`, `G[]` arrays
and `_validate` precompute block from `DeepCGrade` and `DeepCPNoise` into a shared utility struct
(e.g. `DeepCGradeCoefficients`) in a new header included by both. This is purely a code quality
improvement with no API impact.

**Step 4 — Fix known bugs**

In dependency order:
1. `DeepCGrade` reverse gamma (one-line fix, isolated)
2. `DeepCKeymix` A/B containment bug (one-line fix, isolated)
3. `DeepCSaturation` / `DeepCHueShift` divide-by-zero (add alpha guard in `wrappedPerSample`)
4. `DeepCID` unused loop variable (rename or restructure loop)
5. `DeepCConstant` comma operator (replace with explicit lerp call)

**Step 5 — DeepCBlink decision**

Evaluate whether to complete or remove `DeepCBlink`. If removing: delete the file, remove from
`PLUGINS` list, remove from `DRAW_NODES`, remove `target_link_libraries(DeepCBlink ...)`. If
keeping: fix GPU path, fix memory leaks (replace `calloc`/`free` with `std::vector`), handle
all channel counts.

---

## Anti-Patterns

### Anti-Pattern 1: Using DeepPixelOp for DeepBlur

**What people do:** Inherit from `DeepPixelOp` because `processSample()` looks like the right
per-sample hook.
**Why it's wrong:** NDK docs state explicitly that `DeepPixelOp` "provide[s] no support for
spatial operations: each pixel on the input corresponds exactly to a pixel on the output." A blur
that reads neighboring pixels will simply not have access to them.
**Do this instead:** Direct `DeepFilterOp` subclass with expanded `getDeepRequests()` bbox.

### Anti-Pattern 2: Using DeepFilterOp for DeepDefocus

**What people do:** Try to produce 2D scattered output from a `DeepFilterOp` by using a "flat"
output channel.
**Why it's wrong:** `DeepFilterOp` always outputs a `DeepOutputPlane`. There is no way to produce
a true 2D composited result (accumulated over depth) from this base class.
**Do this instead:** `Iop` subclass with `test_input()` accepting `DeepOp*`; output via
`engine()`'s `Row&` parameter.

### Anti-Pattern 3: Using DeepCWrapper for DeepBlur

**What people do:** Attempt to reuse the wrapper pipeline (free masking, mix) by inheriting
`DeepCWrapper` and doing kernel math in `wrappedPerChannel()`.
**Why it's wrong:** The wrapper's `doDeepEngine()` fetches the input plane for exactly the output
box with no expansion. Neighboring pixels outside the output box are not available.
**Do this instead:** Direct `DeepFilterOp` subclass. Implement masking and mix manually if needed,
or leave them out for v1.

### Anti-Pattern 4: Registering Base Classes as Nuke Nodes

**What currently exists:** `DeepCWrapper` and `DeepCMWrapper` have `Op::Description` objects that
make them appear as instantiatable nodes in Nuke.
**Why it's wrong:** These are abstract base classes. Instantiating them applies a no-op gain of
1.0 with no meaningful operation, confusing artists.
**Do this instead:** Remove `Op::Description` and `build()` from both files. See codebase sweep
Step 2 above.

### Anti-Pattern 5: ExternalProject_Add for small vendored NDK plugins

**What people do:** Use `ExternalProject_Add` to pull and build DeepThinner as a separate CMake
build at build time.
**Why it's wrong:** `ExternalProject_Add` targets are not available at configure time, so you
cannot link directly against them in other `add_nuke_plugin()` targets without complex import
chains. The build system becomes hard to understand.
**Do this instead:** `FetchContent_Declare` / `FetchContent_MakeAvailable` (configure-time) for
remote source, or simply copy source files into the tree (as done for FastNoise).

---

## Integration Points

### Internal Boundaries

| Boundary | Communication | Notes |
|----------|---------------|-------|
| Plugin → DeepCWrapper | C++ inheritance + virtual function calls | Interface change (perSampleData) is breaking — do first |
| DeepCWrapper → DeepCMWrapper | C++ inheritance | DeepCMWrapper overrides `wrappedPerSample` / `wrappedPerChannel` |
| DeepCPMatte → Nuke Viewer | OpenGL calls via `draw_handle()` + `ViewerContext` | Requires `OpenGL::GL` CMake link; already present |
| DeepDefocus → DeepOp input | `dynamic_cast<DeepOp*>(Op::input(0))` + `deepIn->deepEngine()` | Standard NDK deep-to-2D pattern |
| DeepBlur → deep input | `input0()->deepEngine(expandedBox, ...)` | Box must be expanded in `getDeepRequests()` |
| CMake → DeepThinner | `FetchContent` or in-tree sources | Mirrors existing FastNoise integration |
| menu.py ↔ CMake | CMake configures `menu.py.in`, injecting plugin name lists | Add new plugins to correct category lists in `src/CMakeLists.txt` |

### External Dependencies

| Dependency | Integration Pattern | Notes |
|------------|---------------------|-------|
| Foundry Nuke NDK | Headers + DDImage link at build time | `FindNuke.cmake` already handles this |
| OpenGL | `find_package(OpenGL)` + `target_link_libraries(... OpenGL::GL)` | Already gated on `OpenGL_FOUND`; DeepPMatte already linked |
| FastNoise | In-tree OBJECT library | Existing pattern for DeepCPNoise |
| DeepThinner | In-tree (preferred) or `FetchContent` | Source availability TBD |

---

## Build Order Implications for Roadmap Phases

The dependency graph between the five feature areas determines safe phase ordering:

```
Phase N: Codebase Sweep (must come first)
    └─ perSampleData interface redesign         [blocks: any new wrapper-based plugin]
    └─ Remove base class Op::Description        [no blockers]
    └─ Grade coefficient extraction             [no blockers beyond perSampleData being stable]
    └─ Known bug fixes                          [no blockers]
    └─ DeepCBlink decision                      [no blockers]

Phase N+1: DeepCShuffle UI improvements         [no dependencies on sweep]
Phase N+1: GL handles for DeepCPMatte           [no dependencies on sweep; isolated to one file]

Phase N+2: DeepBlur                             [depends on: perSampleData if wrapper approach needed
                                                  (it doesn't — direct DeepFilterOp, so independent)]
Phase N+2: DeepDefocus                          [fully independent of sweep and wrappers]

Phase N+3: DeepThinner vendor integration       [fully independent; blocked only by source availability]
```

All Phase N+2 and N+3 items can be developed in parallel once Phase N is complete. The sweep
is the only true dependency gate because the `perSampleData` interface change touches all
`PLUGINS_WRAPPED` and `PLUGINS_MWRAPPED` source files.

---

## Sources

- [NDK Developers Guide: Deep to 2D Ops](https://learn.foundry.com/nuke/developers/11.2/ndkdevguide/deep/deepto2d.html) — HIGH confidence (official Foundry docs)
- [NDK Developers Guide: Simple DeepPixelOp](https://learn.foundry.com/nuke/developers/11.2/ndkdevguide/deep/deepsimple.html) — HIGH confidence (confirms spatial op prohibition)
- [NDK Developers Guide: Basic DeepOps](https://learn.foundry.com/nuke/developers/140/ndkdevguide/deep/deep.html) — HIGH confidence (Nuke 14 current)
- [Writing Nuke C++ Plugins Part 4 — GL Handles](https://erwanleroy.com/writing-nuke-c-plugins-ndk-part-4-custom-knobs-gl-handles-and-the-table-knob/) — MEDIUM confidence (community tutorial, confirmed against DeepCPMatte source)
- [DeepC codebase: `src/DeepCPMatte.cpp`](../codebase/ARCHITECTURE.md) — HIGH confidence (direct inspection)
- [DeepC codebase: `src/DeepCWrapper.h`, `src/DeepCWrapper.cpp`](../codebase/ARCHITECTURE.md) — HIGH confidence (direct inspection)
- [DeepC codebase: `src/DeepCShuffle.cpp`](../codebase/ARCHITECTURE.md) — HIGH confidence (direct inspection)
- [CMake ExternalProject docs](https://cmake.org/cmake/help/latest/module/ExternalProject.html) — HIGH confidence (official CMake docs)
- DeepThinner source availability — NOT FOUND (search returned nothing; treat as LOW confidence unknown)

---

*Architecture research for: DeepC Nuke NDK Plugin Suite — milestone feature additions*
*Researched: 2026-03-12*

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

# Pitfalls Research

**Domain:** Nuke NDK C++ Deep-Compositing Plugin Suite
**Researched:** 2026-03-12
**Confidence:** HIGH (code-confirmed from codebase analysis) / MEDIUM (NDK docs and community sources)

---

## Critical Pitfalls

### Pitfall 1: ABI Mismatch Between Nuke Versions Silently Crashes Nuke at Load

**What goes wrong:**
A plugin compiled with `_GLIBCXX_USE_CXX11_ABI=0` (old ABI, Nuke 14 and earlier) is loaded into Nuke 15+ (which uses `_GLIBCXX_USE_CXX11_ABI=1`, new ABI). Nuke crashes immediately or produces a symbol resolution error like "undefined symbol: DD::Image::Op::input_longlabel(int) const". The binary loads without a clean linker error but exports the wrong symbols.

**Why it happens:**
The Foundry switched the GCC C++11 ABI flag at Nuke 15.0 (aligned with VFX Reference Platform CY2024). Plugins compiled against old headers with old ABI are binary-incompatible with the new runtime even though the API surface looks identical. Since this is a deep dive into name mangling, the compiler gives no warning; the mismatch only surfaces at runtime.

**How to avoid:**
- Nuke 16+ target: compile with `-D_GLIBCXX_USE_CXX11_ABI=1` and GCC 11.2.1+
- Gate the flag in CMake on `Nuke_VERSION` — already partially done; the existing CMake sets C++17 for Nuke >= 15 but must also ensure the ABI flag is set consistently
- Use the NukeDockerBuild images (Rocky Linux base for Nuke 15+) which set the correct compiler environment by default
- Never mix plugin binaries from different ABI builds in the same Nuke plugin directory

**Warning signs:**
- Nuke crashes or prints a symbol resolution error immediately after loading a `.so`
- `nm -D plugin.so | grep GLIBCXX` shows `GLIBCXX_3.4.21` or older (old ABI) when targeting Nuke 16
- CI builds pass but Nuke at runtime crashes before any node is created

**Phase to address:** Docker + GitHub Actions CI/CD phase — the Docker image and CMake flags must be correct before any other build work matters.

---

### Pitfall 2: Calling `_validate()` from `draw_handle()` Causes Deadlock or Crash

**What goes wrong:**
DeepCPMatte already does this: `draw_handle()` calls `DeepCPMatte::_validate(false)` directly. In current (Nuke 15+) multithreaded rendering, `_validate()` can trigger upstream recalculation, which may attempt to acquire a lock already held by the rendering thread. The result is either a deadlock or a crash if the upstream graph is mid-computation.

**Why it happens:**
`draw_handle()` runs on the UI/GL thread. `_validate()` may internally touch `Op` graph state that is also being written by a render thread. NDK documentation warns that handle drawing must be read-only with respect to the node graph — only reading already-validated state is safe.

**How to avoid:**
- In `draw_handle()`, read only from member variables that were set during `_validate()` and are stable during rendering (e.g., `_center`, cached matrices)
- Never call `_validate()`, `input0()->validate()`, or `input0()->deepEngine()` from within `draw_handle()` or `build_handles()`
- Move any computation needed for handle display into `_validate()` and cache the result in a member variable
- The correct pattern for GL handles that need upstream data is `knob_changed()` — see existing DeepCPMatte implementation where position picking happens in `knob_changed("center")` not in `draw_handle()`

**Warning signs:**
- Nuke freezes (stops responding) immediately after scrubbing a parameter while rendering is active
- The GL handle draws correctly when no render is running but causes a hang when cook threads are active
- Valgrind or ThreadSanitizer reports a data race inside an `Op::_validate` call from a GL-related stack frame

**Phase to address:** GL handles for DeepCPMatte phase — the existing `draw_handle()` calling `_validate(false)` is already exhibiting this pattern and must be fixed before adding more handle functionality.

---

### Pitfall 3: Deep-to-2D Engine Does Not Initialize All Output Channels (Uninitialized Rows)

**What goes wrong:**
A DeepDefocus node (Deep input, 2D Iop output) iterates deep samples to accumulate a 2D result but leaves some output channels unwritten for pixels with zero samples. Downstream nodes read garbage floating-point values for those pixels rather than black/zero.

**Why it happens:**
The NDK `engine()` function for an Iop that pulls from deep data must explicitly zero-initialize its output row before accumulating. Unlike standard Iop nodes where `engine()` is always given a pre-zeroed row buffer, the deep-to-2D conversion pattern requires the implementor to clear or write every pixel in the requested span (`x` to `r`). Developers assume the buffer arrives zeroed — it may not.

**How to avoid:**
- At the start of `engine()`, call `row.erase(channels)` or explicitly zero every output channel across the full `[x, r)` span before the accumulation loop
- Verify with a test case: pass a deep image with sparse coverage (many zero-sample pixels); the 2D output must be black (0.0f) in those regions, not indeterminate noise
- The NDK guide's DeepToImage example explicitly shows the erase pattern — follow it verbatim for the initialization step

**Warning signs:**
- Output image shows "salt and pepper" noise or random non-black values in regions where no deep samples exist
- The artifact disappears when every pixel has at least one sample (non-sparse input)
- The bug is frame-dependent because uninitialized memory differs between renders

**Phase to address:** DeepDefocus node implementation phase.

---

### Pitfall 4: `getDeepRequests()` and `doDeepEngine()` Request Different Channel Sets (Request/Engine Divergence)

**What goes wrong:**
`getDeepRequests()` tells Nuke's cache system what channels to pull from upstream. `doDeepEngine()` then calls `input0()->deepEngine()` with a different (usually larger) channel set. The channels that were not requested are either unavailable (producing silent zeros) or force a redundant upstream cook that bypasses the cache.

**Why it happens:**
The two functions are implemented independently and can drift. DeepCShuffle already shows the correct pattern — it builds `neededChannels` identically in both functions by unioning `requestedChannels` with the input channel set. Developers writing new nodes add channels in the engine call for convenience but forget to mirror the change in `getDeepRequests()`.

**How to avoid:**
- Extract the "channels this node needs beyond what downstream requested" into a method on the class (e.g., `neededInputChannels()`) and call it from both `getDeepRequests()` and `doDeepEngine()`
- For `DeepCWrapper` subclasses, use `findNeededDeepChannels()` which is already called from both `_validate()` (to populate `_allNeededDeepChannels`) and `getDeepRequests()` / `doDeepEngine()` — new nodes must follow this pattern
- Never add a channel to the `deepEngine()` call inside the engine that is not also in the `getDeepRequests()` call

**Warning signs:**
- Upstream nodes recook unexpectedly when moving the camera or changing an unrelated parameter
- Deep channels that should be present read as 0.0f inside `doDeepEngine()` even though the upstream node clearly outputs them
- Performance profiling shows repeated upstream cooks that should have been cache hits

**Phase to address:** DeepDefocus and DeepBlur node implementation phases; also relevant to the codebase sweep.

---

### Pitfall 5: `Op::Description` Name Collision with Vendored Plugin Registers Silently Overwrites Prior Plugin

**What goes wrong:**
When DeepThinner is vendored and built into the same plugin directory as DeepC, if any DeepThinner node uses a class name that conflicts with an existing Nuke built-in or another plugin, Nuke silently loads the second registration and the first plugin becomes unreachable. The symptom is that existing scripts break (a node type disappears from the menu or produces wrong behavior) with no error in the log.

**Why it happens:**
`Op::Description` registration is a global table keyed by the string passed to the constructor (e.g., `"DeepCPMatte"`). If two `.so` files both register the same string, the second one wins silently. DeepThinner uses its own class names but if it defines any helper node with a generic name (or if a future refactor renames a DeepC node to match a DeepThinner node), the collision is invisible at build time.

**How to avoid:**
- Audit all class names in DeepThinner source before integration; confirm none collide with existing DeepC class names (`DeepCWrapper`, `DeepCMWrapper`, `DeepCAdd`, `DeepCGrade`, etc.) or with Nuke built-in node names
- Maintain a single authoritative list of registered node names in `src/CMakeLists.txt` and cross-reference against DeepThinner's list before the first CI build
- When adding DeepThinner to the menu, build it as a separate `.so` per node (same pattern as DeepC) rather than a single monolithic library, so that load order is deterministic

**Warning signs:**
- After adding DeepThinner to the build, an existing DeepC node disappears from Nuke's node menu or behaves incorrectly
- Nuke's Script Editor or `nuke.allNodes()` shows a node type present but pointing to the wrong implementation
- `nm -D` on two `.so` files shows identical exported `Op::Description` symbol names

**Phase to address:** DeepThinner vendor integration phase — audit before building, not after.

---

### Pitfall 6: `doDeepEngine()` Return Value Ignored After `deepEngine()` Call Causes Corrupt Output Plane

**What goes wrong:**
A node calls `input0()->deepEngine(...)` but does not check the return value. When the upstream cook is aborted (user hits Escape, or Nuke needs to re-render), the `DeepOutputPlane` reference is in an incomplete state. The current node then writes into it anyway, producing garbage output that may crash Nuke or corrupt the node graph cache.

**Why it happens:**
`doDeepEngine()` returns `bool` — `false` signals abort. Developers familiar with 2D `engine()` (which has no return value and uses a separate `aborted()` call) forget that the deep equivalent uses the return value. The existing `DeepCWrapper` base class handles this correctly, but any node inheriting directly from `DeepFilterOp` (like `DeepCShuffle`, `DeepCKeymix`) must implement the check independently, and the pattern is easy to miss.

**How to avoid:**
- Every call to `input0()->deepEngine(...)` must immediately check `if (!input0()->deepEngine(...)) return false;`
- Also check `if (Op::aborted()) return false;` inside the per-pixel loop for long-running engines (DeepDefocus will be long-running due to per-sample scatter)
- Add this as a code review checklist item for any new direct `DeepFilterOp` subclass

**Warning signs:**
- Nuke hangs briefly then shows a corrupted frame after the user hits Escape mid-cook
- Crash reports in the deep engine call stack after user cancellation
- The `mFnAssert(inPlaceOutPlane.isComplete())` fires on abort paths

**Phase to address:** DeepDefocus and DeepBlur implementation phases; codebase sweep should audit all existing direct subclasses.

---

### Pitfall 7: `wrappedPerSample()` Subclass Divides by Alpha Without Zero Guard, Propagating NaN into Downstream Cache

**What goes wrong:**
A `DeepCWrapper` subclass overrides `wrappedPerSample()` and performs unpremult by dividing by `alpha` without checking for zero. Any transparent sample (alpha == 0) produces NaN or inf. These values propagate downstream and are cached, so the corruption persists even after the offending node is removed from the graph, until Nuke's cache is flushed.

**Why it happens:**
The base class `doDeepEngine()` guards the unpremult in the per-channel pass (lines 259-260 in `DeepCWrapper.cpp`), but the guard only applies to the `wrappedPerChannel()` path. Subclasses overriding `wrappedPerSample()` and performing their own unpremult must implement their own guard. `DeepCSaturation` and `DeepCHueShift` already exhibit this bug (confirmed in CONCERNS.md).

**How to avoid:**
- Any `wrappedPerSample()` override that touches alpha must use the pattern: `if (alpha > 0.0f) { value /= alpha; } else { value = 0.0f; }`
- The codebase sweep must audit all `wrappedPerSample()` implementations for naked division by `alpha`
- New nodes (DeepDefocus, DeepBlur) must follow this pattern if they access alpha in the per-sample hook

**Warning signs:**
- Node outputs NaN or inf values that persist in downstream nodes even after removing the problematic node
- `DeepCWrapper::doDeepEngine()` receives NaN values from upstream that it did not produce itself
- Transparent regions of a deep image (alpha=0 samples) produce bright or black-and-white pixels in node output

**Phase to address:** Codebase sweep phase (fix existing nodes); any new node implementation phase.

---

## Technical Debt Patterns

Shortcuts that seem reasonable but create long-term problems specific to this codebase.

| Shortcut | Immediate Benefit | Long-term Cost | When Acceptable |
|----------|-------------------|----------------|-----------------|
| Passing `perSampleData` as a single `float` between `wrappedPerSample` and `wrappedPerChannel` | Simple interface, no allocation | Any node needing more than one per-sample scalar (color triplet, normal, etc.) must work around the interface — `DeepCHueShift` already does | Never for new nodes; redesign the interface in the sweep phase |
| `DeepCWrapper` and `DeepCMWrapper` registered as usable Nuke nodes via `Op::Description` | No extra code to suppress them | Users can instantiate these broken base nodes; scripts that accidentally reference them will silently produce wrong output | Never acceptable; remove the descriptors in the sweep |
| Copy-pasting grade coefficient logic between `DeepCGrade` and `DeepCPNoise` instead of extracting a shared utility | Faster initial implementation | Bug fixes must be applied twice; implementations can silently diverge | Never for new shared logic; extract during sweep |
| Hard-coding magic index `0` to detect Simplex noise type instead of comparing against `FastNoise::SimplexFractal` | Simpler conditional | Reordering `noiseTypeNames[]` silently breaks 4D noise selection | Never; fix during the 4D noise exposure phase |
| `switch` statements in `test_input()` / `default_input()` / `input_label()` with no `default` case and no trailing return | Shorter code | UB on any input beyond handled indices; easy to miss when adding a third input to any node | Never; add `default` cases and explicit returns during sweep |

---

## Integration Gotchas

Common mistakes when connecting to Nuke's systems.

| Integration | Common Mistake | Correct Approach |
|-------------|----------------|------------------|
| Nuke's GL viewer (build_handles) | Calling `_validate()` or `deepEngine()` from `draw_handle()` — already present in `DeepCPMatte` | Cache required state in `_validate()`; `draw_handle()` reads only cached member variables |
| Nuke's deep request pipeline | Adding channels in `doDeepEngine()` that were not in `getDeepRequests()` | Mirror channel needs identically in both; use a shared helper method or member variable |
| Docker CI for NDK builds | Distributing compiled plugins that bundle NDK headers or Nuke runtime symbols beyond what GPL-3.0 allows | Distribute only your `.so` files and Python; never redistribute NDK headers or `libDDImage.so` |
| GitHub Actions artifact storage | Committing compiled `.so` files to the repository or storing them without version tagging | Store artifacts as GitHub Release assets tagged by Nuke version + semver; use artifact naming `DeepC_Nuke16v1_linux_x86_64.tar.gz` |
| `Op::Description` with external plugins (DeepThinner) | Assuming class names are unique without auditing | Cross-reference all registered names before the first joint build |
| Python `menu.py` generated by CMake | Manually editing `menu.py.in` to add DeepThinner nodes, then having CMake overwrite the file on rebuild | Add DeepThinner entries to the CMake variable lists in `src/CMakeLists.txt`, not directly to `menu.py.in` |

---

## Performance Traps

Patterns that cause correctness issues or unacceptable render times at production resolution.

| Trap | Symptoms | Prevention | When It Breaks |
|------|----------|------------|----------------|
| Inverting a matrix per sample inside `wrappedPerSample()` or `doDeepEngine()` | Frame cook times scale with sample count even for unchanged parameters; `DeepCWorld` already does this for `window_matrix.inverse()` | Compute and cache inverse in `_validate()`; read the cached value per sample | At production resolution with deep EXR files having 8-20 samples/pixel, this is a measurable slowdown |
| Allocating heap buffers (`calloc`, `new`) per `doDeepEngine()` call without RAII | Memory leaked on early-exit code paths (abort, exception); `DeepCBlink` already exhibits this | Use `std::vector<float>` or RAII wrappers so cleanup is guaranteed on all paths | Every cook call; memory leak accumulates during interactive sessions |
| DeepDefocus: iterating all deep samples at every output pixel for scatter-based bokeh | O(W * H * S) where S = sample count; slow for high-sample-count deep data | Bound the scatter radius; use tile/bucket decomposition; profile early with realistic deep EXR data before optimizing | Immediately at 2K+ resolution with any meaningful defocus radius |
| Reading 2D mask rows inside the per-sample loop instead of per-scanline | Redundant `Iop::get()` calls; `DeepCWrapper` already handles this correctly with `currentYRow` caching | Mirror the `DeepCWrapper` pattern: fetch the row once per Y scanline, cache the `Row`, index by X per sample | At any resolution; each extra `get()` call may trigger upstream processing |

---

## UX Pitfalls

Common user-facing mistakes for Nuke plugin UI design.

| Pitfall | User Impact | Better Approach |
|---------|-------------|-----------------|
| Using `Channel_knob` where `Input_Channel_knob` is needed for a deep stream selector | The dropdown shows all channels in the script, not just those present in the connected deep input; users can select channels that don't exist on the input, leading to silent zeros | Use `Input_Channel_knob(f, &channel, input_number, 0, "knob_name")` with the correct input index; it filters the dropdown to show only channels present on that specific input |
| Using deprecated `ChannelMask_knob` instead of `Input_ChannelSet_knob` | Deprecated knob type; may have inconsistent behavior across Nuke versions; `ChannelMask_knob` docs explicitly say "legacy, use ChannelSet variety" | Use `Input_ChannelSet_knob` for multi-channel selection from a specific input |
| DeepShuffle UI: using 8 individual `Channel_knob` pairs (in/out) with no visual grouping | Artists cannot see which pairs are connected at a glance; feels different from Nuke's built-in Shuffle2 "noodle" style | Group pairs visually with `Text_knob(">>")` between in/out on the same row; use `ClearFlags(f, Knob::STARTLINE)` to prevent line breaks between paired knobs (existing `DeepCShuffle` already does this for 4 pairs) |
| GL handles visible in 3D viewer when they should only appear in 2D viewer (or vice versa) | Handle draws in wrong viewport context; can interfere with 3D camera handles | In `build_handles()`, check `ctx->transform_mode()` and return early if the viewer mode is wrong — `DeepCPMatte` already correctly guards with `if (ctx->transform_mode() != VIEWER_2D) return;` |
| Exposing a "Use GPU" knob when GPU path is not implemented | Artists enable GPU expecting speed improvement; actually no-ops silently | Either implement the GPU path or hide/disable the knob — `DeepCBlink` is the current offender |

---

## "Looks Done But Isn't" Checklist

Things that appear complete in Nuke NDK plugins but are missing critical pieces.

- [ ] **New node compiled and loads:** Verify the `Op::Description` name exactly matches the `Class()` return value — a mismatch causes Nuke to load the `.so` but fail to create nodes of that type with a cryptic error
- [ ] **DeepDefocus 2D output:** Verify `row.erase(channels)` or equivalent is called at the start of `engine()` — output appears correct for dense input but shows garbage for sparse deep images
- [ ] **GL handles interactive:** Verify handles respond to mouse drag — `add_draw_handle(ctx)` alone is not enough; `begin_handle()` / `end_handle()` must wrap the geometry for hit detection
- [ ] **DeepThinner integrated:** Verify DeepThinner nodes appear in the `menu.py` generated by CMake — the menu template must be updated, not just the compiled binary
- [ ] **CI build produces artifact:** Verify the GitHub Actions artifact is named with the Nuke version — a generic `plugins.tar.gz` is indistinguishable from builds for different Nuke versions
- [ ] **Codebase sweep complete:** Verify `DeepCWrapper` and `DeepCMWrapper` `Op::Description` descriptors have been removed — they will still appear as usable nodes in Nuke until the descriptor is gone, even if `node_help()` says "should never be used directly"
- [ ] **4D noise exposed:** Verify the Simplex detection uses the enum value from `noiseTypes[]` rather than the hard-coded index `0` — otherwise reordering the noise type dropdown silently breaks 4D evolution

---

## Recovery Strategies

When pitfalls occur despite prevention, how to recover.

| Pitfall | Recovery Cost | Recovery Steps |
|---------|---------------|----------------|
| ABI mismatch crashing Nuke at load | LOW | Recompile with correct `-D_GLIBCXX_USE_CXX11_ABI` flag; run `nm -D` to verify symbols before distributing |
| `_validate()` in `draw_handle()` deadlock | MEDIUM | Remove the `_validate()` call; cache all needed state in `_validate()`; test by enabling interactive render (Nuke's "A" mode) while dragging a handle |
| Uninitialized 2D output rows in DeepDefocus | LOW | Add `row.erase(channels)` at start of `engine()` before any accumulation logic |
| NaN propagation from zero-alpha divide in wrappedPerSample | MEDIUM | Add the zero guard; flush Nuke's cache after fixing (Nuke caches NaN values and they persist across re-cooks without a flush) |
| Op::Description name collision with DeepThinner | HIGH | Rename the colliding node in DeepThinner (requires rebuilding and re-releasing); any existing scripts using the old name break |
| Docker CI artifact contains wrong ABI binary | LOW | Re-tag the Docker image with the correct Nuke version; re-run CI; overwrite the release artifact |
| `DeepCBlink` memory leak on abort | MEDIUM | Replace `calloc`/`free` with `std::vector<float>`; verify with Valgrind under abort conditions |

---

## Pitfall-to-Phase Mapping

How roadmap phases should address these pitfalls.

| Pitfall | Prevention Phase | Verification |
|---------|------------------|--------------|
| ABI mismatch crashing Nuke at load | Docker + GitHub Actions CI/CD | CI build must link against Nuke 16 headers; run `nm -D` check on the artifact |
| `_validate()` in `draw_handle()` deadlock | GL handles for DeepCPMatte | Enable interactive render ("A" mode) while dragging all handles — Nuke must not freeze |
| Uninitialized 2D output rows | DeepDefocus implementation | Test with sparse deep input (10% of pixels have samples); verify black output elsewhere |
| NaN from zero-alpha divide in `wrappedPerSample` | Codebase sweep | Pass deep image with zero-alpha samples through `DeepCSaturation` and `DeepCHueShift`; verify no NaN |
| `getDeepRequests` / `doDeepEngine` channel divergence | Any new node (DeepDefocus, DeepBlur) | Profile upstream cook count; should equal 1 per downstream request, not more |
| `Op::Description` name collision with DeepThinner | DeepThinner vendor integration | Run a name audit script against all `.cpp` files in both repos before the first joint build |
| Missing abort check after `deepEngine()` return | Codebase sweep + new node implementation | Press Escape during a long cook; Nuke should cancel cleanly without crash or corrupted output |
| Vendored base class nodes appearing in Nuke menu | Codebase sweep | After removing `Op::Description` from `DeepCWrapper.cpp` / `DeepCMWrapper.cpp`, verify neither appears in Nuke's node browser |
| Hard-coded magic index for Simplex noise type | 4D noise exposure | Swap the order of noise types in `noiseTypeNames[]`; confirm 4D noise still activates for Simplex |
| Switch statements with no default / trailing return | Codebase sweep | Enable `-Wreturn-type` `-Wswitch-default` compiler warnings; treat as errors |
| `perSampleData` single-float design limitation | Codebase sweep (redesign before new nodes use the interface) | DeepDefocus and DeepBlur implementation must not require more than one float — or the redesign must precede those phases |
| CI artifact management (Nuke version tagging) | Docker + GitHub Actions CI/CD | Artifacts on GitHub Releases must include Nuke version in filename; download and test in corresponding Nuke version |

---

## Sources

- Foundry NDK Developer Guide: Deep-to-2D Operations — https://learn.foundry.com/nuke/developers/11.2/ndkdevguide/deep/deepto2d.html
- Foundry NDK Developer Guide: Linux ABI requirements for Nuke 15 — https://learn.foundry.com/nuke/developers/15.0/ndkdevguide/appendixa/linux.html
- Foundry NDK Developer Guide: Deprecated API changes (Nuke 15.1) — https://learn.foundry.com/nuke/developers/15.1/ndkdevguide/appendixc/deprecated.html
- Foundry NDK Developer Guide: Knob Types — https://learn.foundry.com/nuke/developers/63/ndkdevguide/knobs-and-handles/knobtypes.html
- Foundry NDK: ViewerContext class reference — https://learn.foundry.com/nuke/developers/11.2/ndkreference/classDD_1_1Image_1_1ViewerContext.html
- Erwan Leroy: Writing Nuke C++ Plugins Part 4 — GL Handles — https://erwanleroy.com/writing-nuke-c-plugins-ndk-part-4-custom-knobs-gl-handles-and-the-table-knob/
- NukeDockerBuild project (Gilles Vink) — https://github.com/gillesvink/NukeDockerBuild
- Foundry community thread: compiling with `_GLIBCXX_USE_CXX11_ABI=1` — https://community.foundry.com/discuss/topic/162428/compiling-with-glibcxx-use-cxx11-abi-1
- DeepC codebase: `src/DeepCWrapper.cpp`, `src/DeepCPMatte.cpp`, `src/DeepCShuffle.cpp`, `src/DeepCWrapper.h`
- DeepC `.planning/codebase/CONCERNS.md` — confirmed bugs: NaN from zero-alpha divide (`DeepCSaturation`, `DeepCHueShift`), memory leak (`DeepCBlink`), `_validate()` called from `draw_handle()` (`DeepCPMatte`)

---

*Pitfalls research for: Nuke NDK deep-compositing C++ plugin suite (DeepC)*
*Researched: 2026-03-12*