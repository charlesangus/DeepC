# S01 Research: Singleton renderer + layer-peel dedup

**Slice goal:** Eliminate CPU render bottlenecks — singleton renderer, hoisted per-layer prep, and quality knob.

**Calibration:** Targeted research — all technology is established Rust/C++ in this codebase; risks are around API seams in the forked opendefocus crate and thread-safety of the singleton.

---

## Active Requirements This Slice Owns

- **R063** — Singleton renderer (eliminate per-call init overhead)
- **R064** — Per-layer dedup (hoist filter/mipmap/inpaint prep out of layer loop)

The quality knob (R066) is partially addressed here (FFI param + C++ knob); the 10s target (R067) is measured after S02 (cache). R065 (full-image cache) is S02.

---

## Implementation Landscape

### Where the bottlenecks live

**Entry point:** `crates/opendefocus-deep/src/lib.rs` — `render_impl()`.

Current call flow per C++ `renderStripe` call:
1. Deserialize FFI arrays → `pixels: Vec<Vec<(r,g,b,a,z)>>`  
2. Sort each pixel's samples front-to-back  
3. Create `OpenDefocusRenderer` — **bottleneck A** (per FFI call)  
4. Loop over `max_layers` (once per layer):
   - Build `layer_rgba` + `layer_depth` arrays (O(W×H))
   - Build `Settings`, `RenderSpecs`, ndarray views
   - Call `block_on(renderer.render_stripe(...))` — **bottleneck B** (per layer, see below)
5. Front-to-back "over" composite

**What `render_stripe` does inside (per layer):**
- `RenderEngine::new(settings, render_specs)` → cheap
- `prepare_filter_image(...)` — Simple mode returns `Array3::zeros(8×8×N)` — cheap but allocates
- `create_mipmaps(filter_image)` — 8→4→2→1 mipmap chain (cheap, but called N times)
- `render_convolve(...)`:
  - `get_inpaint_image(original_image, depth)` — **Telea inpaint O(W×H)** — called N times
  - `runner.convolve(...)` — rayon per-pixel kernel — main real work per layer

### Bottleneck sizing

For a 256×256 image with 5 deep layers:
- **Bottleneck A (singleton):** `OpenDefocusRenderer::new()` → CPU path = `SharedRunner::init(false)` which trivially returns `Cpu(CpuRunner)` (an empty struct). The overhead is the `block_on(async { ... })` wrapper + Settings allocation. Likely 1–10ms per FFI call (not per layer). Worth fixing but not the main savings.
- **Bottleneck B (inpaint):** `get_inpaint_image()` (Telea inpaint) called once per layer = 5×(256×256) = 327,680 pixel iterations. This is the primary per-layer dedup win.
- **Bonus:** `prepare_filter_image` + `create_mipmaps` called N times needlessly — small allocation overhead.

### Key discovery: inpaint is useless for deep-defocus layers

`get_inpaint_image` uses `get_inpaint_mask(depth)` which marks `depth < 0` as holes to fill. The opendefocus depth pipeline applies `prepare_depth_map` which negates the CoC output: `depth_value → -CoC(depth_value)`. For the deep-defocus use case, nearly all pixels end up with negative CoC values (including empty pixels where `depth=0.0`). The inpaint algorithm is invoked but cannot produce useful output because the "holes" it sees don't correspond to real coverage gaps in a flat image — each layer is a partial projection where transparent pixels are simply "no sample at this depth" rather than a spatial inpainting problem. The inpaint output is discarded conceptually; the kernel already handles transparent region contribution correctly. **Skipping inpaint for the layer-peel path is safe and eliminates the main per-layer redundancy.**

### Thread safety of the singleton

`OpenDefocusRenderer` has `#[derive(Debug, Clone)]`. It holds `SharedRunner` which holds either `CpuRunner` (empty struct) or `WgpuRunner` (holds `wgpu::Device` + `wgpu::Queue`). `render_stripe` takes `&self` (immutable). Nuke can call `renderStripe` from multiple threads concurrently. For the CPU path, `CpuRunner::execute_kernel_pass` creates all local arrays per call (no shared mutable state), so concurrent calls on the same singleton are safe. A `std::sync::OnceLock<OpenDefocusRenderer>` is sufficient — no `Mutex` needed for the CPU path. The `once_cell` crate is already in `Cargo.toml` as a dep; std `OnceLock` (stable since 1.70) works too.

**Note:** GPU path (`WgpuRunner::execute_kernel_pass` → `device.poll()` + `queue.submit()`) is not `&self`-safe for concurrent calls — but per D029, GPU performance is explicitly deferred. The singleton design should wrap the renderer in `Arc<Mutex<_>>` for safety even in S01, so it's future-proof when GPU is re-enabled.

### API seam for hoisting prep

`prepare_filter_image` and `create_mipmaps` are private functions in `crates/opendefocus/src/worker/engine.rs` and `crates/opendefocus/src/renders/resize.rs`. `MipmapBuffer<T>` is a `pub struct` but in the private `renders` module. To hoist prep out of the layer loop, the cleanest approach is:

**Option A (recommended):** Add a new `pub async fn render_stripe_prepped()` method to `OpenDefocusRenderer` that accepts a pre-built `&MipmapBuffer<f32>` and skips the filter/mipmap setup. Re-export `MipmapBuffer` from `crates/opendefocus/src/lib.rs`. Add `pub async fn prepare_filter_mipmaps()` to `OpenDefocusRenderer` for the one-time setup. This keeps all prep logic in the opendefocus crate and exposes a clean API.

**Option B:** Make `prepare_filter_image` and `create_mipmaps` pub(crate) and call `RenderEngine` internals directly. More invasive, less clean.

Option A is preferred — single-crate responsibility boundary maintained.

For inpaint: the `render_stripe_prepped` path should pass `Array3::zeros(...)` as the inpaint image (equivalent to "no inpaint data"). `render_convolve` already handles the Twod path by using `Array3::zeros(original_image.dim())` as inpaint — the same approach works for the prepped path.

### Quality FFI parameter

Quality is a proto-generated enum (`LOW=0, MEDIUM=1, HIGH=2, CUSTOM=3`). Currently hardcoded to `Quality::Low` in `render_impl`. To expose as a knob:

1. Add `quality: i32` parameter to both `opendefocus_deep_render` and `opendefocus_deep_render_holdout` in the FFI header and Rust entry points.
2. In `render_impl`, set `settings.render.quality = quality.into()` (the `.into()` pattern confirmed by KNOWLEDGE.md for proto i32 enums).
3. In `DeepCOpenDefocus.cpp`: add `int _quality = 0` member, add `Enumeration_knob(f, &_quality, kQualityEntries, "quality", "Quality")` in `knobs()`, pass `(int)_quality` to FFI calls.
4. `kQualityEntries = {"Low", "Medium", "High", nullptr}` (null-terminated per Nuke convention).
5. `verify-s01-syntax.sh` already has `Enumeration_knob` stub — no mock header change needed.

---

## File Map

| File | Change |
|------|--------|
| `crates/opendefocus/src/lib.rs` | Re-export `MipmapBuffer`; add `prepare_filter_mipmaps()` and `render_stripe_prepped()` methods |
| `crates/opendefocus/src/worker/engine.rs` | Make `render_convolve` pub(crate) or restructure to support the prepped path |
| `crates/opendefocus-deep/src/lib.rs` | Singleton `OnceLock<Arc<Mutex<OpenDefocusRenderer>>>`; hoist prep before layer loop; add `quality: i32` param to FFI fns and `render_impl` |
| `crates/opendefocus-deep/include/opendefocus_deep.h` | Add `quality: i32` to both function signatures |
| `src/DeepCOpenDefocus.cpp` | Add `int _quality` member, `kQualityEntries`, `Enumeration_knob`, pass `quality` to FFI |
| `test/test_opendefocus_cpu.nk` | Optionally add a 256×256 timing test variant |
| `scripts/verify-s01-syntax.sh` | Should pass unchanged; verify `quality` knob grep contract if added |

---

## Natural Seams (Task Decomposition)

**T01 — Singleton renderer**
- Scope: `crates/opendefocus-deep/src/lib.rs` only
- Change: Replace `let renderer = if use_gpu != 0 { ... }` with static `OnceLock<Arc<Mutex<OpenDefocusRenderer>>>` initialized on first call
- Verify: `cargo test -p opendefocus-deep` passes; add a `test_singleton_reuse` unit test that calls `render_impl` twice and asserts the renderer instance pointer is the same (or just that both calls succeed quickly)
- Risk: Mutex contention under Nuke's multithreaded engine — low, Mutex overhead per render call is negligible vs render cost

**T02 — Hoist filter/mipmap prep + skip inpaint**
- Scope: `crates/opendefocus/src/lib.rs` + `engine.rs` (minor changes); `crates/opendefocus-deep/src/lib.rs` (restructure layer loop)
- Change: Add `prepare_filter_mipmaps(settings: &Settings)` → `Result<MipmapBuffer<f32>>` to `OpenDefocusRenderer`; add `render_stripe_prepped(render_specs, settings, image, depth, &MipmapBuffer<f32>, empty_inpaint, holdout)` that calls `render_convolve` directly bypassing prep; call from `render_impl` before loop
- Verify: Existing `test_holdout_attenuates_background_samples` must still pass; timing before/after measurable with `time cargo test --no-run` or a benchmarking harness

**T03 — Quality FFI param + C++ knob**
- Scope: `opendefocus_deep.h`, `crates/opendefocus-deep/src/lib.rs`, `src/DeepCOpenDefocus.cpp`
- Change: Add `quality: i32` to both FFI functions; propagate to `Settings.render.quality`; add `Enumeration_knob` in C++
- Verify: `scripts/verify-s01-syntax.sh` passes; `grep -c 'Enumeration_knob' src/DeepCOpenDefocus.cpp` == 1

**T04 — Timing comparison + cargo test**
- Scope: Tests and benchmark timing
- Change: Add a 256×256 `.nk` test script; add unit test with `std::time::Instant` before/after
- Verify: `cargo test -p opendefocus-deep` all pass; timed comparison documents speedup

---

## Key Risks and Constraints

1. **Mutex wrapping the singleton** — `OpenDefocusRenderer::render_stripe` takes `&self`. For CPU singleton, `OnceLock<OpenDefocusRenderer>` works. But to be GPU-forward-compatible (D029 says deferred, not never), wrap in `Arc<Mutex<_>>`. Mutex lock per render call is negligible vs render cost.

2. **render_stripe_prepped API surface** — `render_convolve` is currently `async fn render_convolve` with private visibility. Making it `pub(crate)` is the minimal change. The `MipmapBuffer` re-export from `lib.rs` is a one-line addition.

3. **Settings differ per layer (focal_plane changes per depth)** — The filter kernel (bokeh shape) doesn't change between layers; mipmap prep is safe to hoist. Per-layer depth arrays and Settings.defocus.focal_plane DO change per layer. The prepped path must still accept fresh `Settings` per layer for the depth-related fields.

4. **Inpaint skip correctness** — Skipping inpaint means passing zeros as the inpaint image. The opendefocus kernel uses inpaint for edge bleeding correction (filling CoC halos at depth discontinuities). For the deep-defocus layer-peel use case, there are no depth discontinuities within a layer (each layer is a single depth slice), so skipping is correct. KNOWLEDGE.md confirms this as an open question (M012 context section).

5. **Quality enum `.into()` for proto i32** — KNOWLEDGE.md explicitly notes: "opendefocus Quality enum is proto-generated i32 — use `.into()` for assignment." Confirmed at line 351 of `lib.rs`: `settings.render.quality = Quality::Low.into()`. New code: `settings.render.quality = quality.into()` where `quality: i32`.

6. **verify-s01-syntax.sh grep contract** — The script checks for specific patterns. If a `quality` grep count check is added (e.g., `grep -c 'Enumeration_knob' == 1`), ensure no duplicate in comments or HELP text (established pattern from KNOWLEDGE.md).

7. **Rust toolchain version** — Local Cargo is 1.75 which fails on `edition2024` deps (avif-serialize). Docker build uses stable toolchain. Do not run `cargo test` locally without Docker; use the verify script + Docker for final proof.

---

## Verification Commands

```sh
# Fast: syntax check
bash scripts/verify-s01-syntax.sh

# Cargo test (requires Docker or compatible local Rust ≥1.85):
# cargo test -p opendefocus-deep -- --nocapture

# Docker full build:
# (see scripts/verify-s01-syntax.sh Docker note at bottom)
```

---

## Skills Discovered

None applicable — all technology (Rust, C++, Nuke DDImage) is established in the codebase.
