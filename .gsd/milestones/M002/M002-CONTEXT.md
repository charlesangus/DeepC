# M002: DeepCBlur

**Gathered:** 2026-03-20
**Status:** Ready for planning

## Project Description

DeepCBlur is a new Nuke NDK plugin that applies a Gaussian blur to deep images. It takes deep input, propagates samples outward into empty neighboring pixels (creating new samples where none existed), applies Gaussian-weighted blending across all channels, and produces blurred deep output with the sample structure intact. A built-in per-pixel sample optimization step (merge nearby depths + max_samples cap) prevents memory explosion from sample propagation.

## Why This Milestone

No built-in Nuke node blurs deep images. Artists must flatten to 2D, blur, and lose deep data — breaking the deep pipeline. DeepCBlur keeps the deep compositing chain intact, enabling downstream deep operations (merge, holdout, color correct) to work on blurred deep data.

## User-Visible Outcome

### When this milestone is complete, the user can:

- Create a DeepCBlur node from the DeepC > Filter toolbar menu in Nuke
- Connect a deep input and see Gaussian-blurred deep output with separate width/height control
- Observe deep samples propagating into previously-empty neighboring pixels
- Control sample density via the max_samples knob to bound memory and render time

### Entry point / environment

- Entry point: DeepC > Filter > DeepCBlur in Nuke's node toolbar
- Environment: Nuke 16+ on Linux or Windows
- Live dependencies involved: none

## Completion Class

- Contract complete means: DeepCBlur.cpp compiles, links, and loads in Nuke 16+; local CMake build succeeds
- Integration complete means: docker-build.sh produces release archives containing DeepCBlur for both platforms; menu.py includes the DeepCBlur entry
- Operational complete means: none (plugin, not a service)

## Final Integrated Acceptance

To call this milestone complete, we must prove:

- DeepCBlur node loads in Nuke 16+ and appears in the DeepC Filter submenu
- A deep image with sparse samples is blurred and new samples appear in neighboring empty pixels
- The max_samples knob limits output sample count per pixel
- docker-build.sh produces release archives for Linux and Windows containing DeepCBlur

## Risks and Unknowns

- **Sample-merging semantics when multiple source pixels contribute overlapping depths** — when samples from different source pixels propagate into the same output pixel at similar depths, the merge-by-depth logic must produce correct compositing results. Getting this wrong produces visible artifacts. Mitigated by early prototype validation.
- **Performance at large blur sizes** — O(W×H×S×kernel_area) is inherent. Large blur sizes on dense deep images will be slow. Mitigated by tooltip warning and max_samples cap, same as msDeepBlur's approach.
- **Separable Gaussian on deep data** — standard 2D Gaussian blur uses separable horizontal+vertical passes for O(n) per pixel instead of O(n²). Deep data's per-pixel sample stacks may make the separable approach difficult or impossible because intermediate per-row results need to carry variable-length sample arrays. May need full 2D kernel evaluation per pixel.

## Existing Codebase / Prior Art

- `src/DeepCKeymix.cpp` — direct `DeepFilterOp` subclass, best reference for the doDeepEngine() pattern with DeepPlane/DeepPixel iteration, sample output, and multi-input handling
- `src/DeepThinner.cpp` — sample optimization patterns (SampleRecord struct, sorted sample manipulation, merge logic)
- `src/CMakeLists.txt` — PLUGINS list, FILTER_NODES variable, add_nuke_plugin() function
- `python/menu.py.in` — menu template with Filter submenu already wired for FILTER_NODES
- `.gsd/milestones/M001/M001-RESEARCH.md` — extensive prior research on DeepCBlur architecture, base class choice, sample semantics, pitfalls

> See `.gsd/DECISIONS.md` for all architectural and pattern decisions — it is an append-only register; read it during planning, append to it during execution.

## Relevant Requirements

- R001 — DeepCBlur core Gaussian blur capability
- R002 — Separate width/height knobs
- R003 — Sample propagation into empty pixels
- R004 — Built-in per-pixel sample optimization
- R005 — Shared DeepSampleOptimizer header
- R006 — CMake and menu integration
- R007 — docker-build.sh release archives

## Scope

### In Scope

- DeepCBlur node: Gaussian blur, separate width/height, all-channel blur, sample propagation, per-pixel optimization
- DeepSampleOptimizer.h: shared header for sample merge + max_samples, reusable by future ops
- CMake integration: PLUGINS list, FILTER_NODES, menu.py.in
- docker-build.sh verification: Linux + Windows release archives
- README update: 24-plugin count

### Out of Scope / Non-Goals

- GPU/Blink acceleration
- Non-Gaussian filter types (box, triangle, quadratic)
- Depth-aware / depth-varying blur (future DeepDefocus territory)
- Channel selector knob (blur all channels — subset blur creates per-sample inconsistency)
- Animated/keyframeable blur (standard NDK knob animation handles this automatically)

## Technical Constraints

- C++17, Nuke 16+ NDK, `_GLIBCXX_USE_CXX11_ABI=1`, GCC 11.2.1
- Must use `DeepFilterOp` base class (not `DeepPixelOp` — spatial op prohibition; not `DeepCWrapper` — can't expand input bbox)
- `getDeepRequests()` must expand the requested Box by the blur radius in x and y
- Depth channels (`Chan_DeepFront`, `Chan_DeepBack`) propagate from source samples, not blurred
- DeepSampleOptimizer.h must be header-only or header + minimal .cpp, with a clean interface taking a vector of samples and returning compacted output

## Integration Points

- `src/CMakeLists.txt` — add to PLUGINS list, update FILTER_NODES
- `python/menu.py.in` — no changes needed (FILTER_NODES variable already wired)
- `CMakeLists.txt` — icon install if DeepCBlur.png is added
- `README.md` — update plugin count from 23 to 24

## Open Questions

- **Separable Gaussian feasibility** — can the blur be decomposed into horizontal + vertical passes for deep data, or must it use the full 2D kernel? Horizontal pass produces intermediate deep output with variable sample counts per pixel; vertical pass must read that. Feasible but adds complexity. May be worth doing for performance, or may not be worth the complexity for v1. Decide during implementation.
- **Merge tolerance default** — what depth proximity threshold should be used for merging propagated samples? Needs experimentation. Start with a small absolute tolerance (e.g. 0.001) and expose as a knob.
