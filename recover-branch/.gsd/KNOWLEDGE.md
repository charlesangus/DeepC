# Knowledge Base

Patterns, gotchas, and non-obvious lessons learned during development.

## DeepFilterOp Subclass Pattern (M002/S01)

When building a deep spatial op that needs to expand output beyond input bounds:
1. **_validate()**: Expand `_deepInfo.box()` by the kernel radius in all four directions
2. **getDeepRequests()**: Pad the requested input box by the same radius; request all channels + `Mask_Deep`
3. **doDeepEngine()**: Fetch the padded `DeepPlane`, iterate output pixels, gather from neighbors

Reference implementations: DeepCAdjustBBox (bbox expansion), DeepCKeymix (RequestData pattern), DeepThinner (DeepOutPixel emit pattern).

## Icon Installation Mechanism (M002/S02)

Plugin icons are NOT installed via a `file(GLOB)` in `src/CMakeLists.txt`. The top-level `CMakeLists.txt` uses `install(DIRECTORY icons/ ...)` to copy the entire `icons/` directory into the install tree. Adding a new icon requires only placing the PNG in `icons/` — no CMake changes needed. The Docker build and release archive packaging pick it up automatically.

## thread_local ScratchBuf for DeepFilterOp Hot Paths (M002/S01)

Nuke calls `doDeepEngine()` from multiple threads. Heap allocation per-pixel kills performance. The `thread_local` scratch buffer pattern from DeepThinner avoids this: declare a `thread_local` struct with pre-allocated vectors, clear (not deallocate) at the start of each pixel. This gives zero-allocation steady state after warmup.

## DeepSampleOptimizer Merge Math (M002/S01)

Front-to-back over-compositing for merged alpha: `alpha_acc += alpha * (1.0f - alpha_acc)`. This is the standard deep compositing formula. Color channels are weighted the same way: `channel_acc += channel * (1.0f - alpha_acc_before)`. Getting the order wrong (back-to-front, or forgetting the `1 - alpha_acc` term) produces visible compositing errors.

## Standalone Header Testing (M002/S01)

Headers intended for cross-plugin reuse (like DeepSampleOptimizer.h) should be compilable standalone: `g++ -std=c++17 -fsyntax-only -I. src/Header.h`. If DDImage types leak into the header, it can't be unit-tested outside the Nuke build environment. Use only `<vector>`, `<algorithm>`, `<cmath>` and similar standard headers.

## FILTER_NODES and Menu Integration (M002/S02)

New filter plugins need to be added to the `FILTER_NODES` variable in `src/CMakeLists.txt`. The `string(REPLACE)` call converts the semicolon-separated CMake list into a quoted comma-separated list for `menu.py.in`'s `@FILTER_NODES@` placeholder. No changes to `menu.py.in` itself are needed — the template already has the `Filter` submenu wired to the variable. Same pattern applies to `DRAW_NODES`, `COLOR_NODES`, etc.
