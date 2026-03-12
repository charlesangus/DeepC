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
