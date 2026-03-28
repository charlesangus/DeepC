# M009-mso5fb Research — DeepCOpenDefocus GPU-Accelerated Deep Defocus

**Date:** 2026-03-28
**Status:** Pre-planning (S01 complete, S02/S03 need planning)

---

## Executive Summary

S01 is fully complete and all its artifacts are verified. The build foundation is solid: `libopendefocus_deep.a` (55.9 MB) exists, the extern "C" FFI boundary is defined, CMake links the static lib, docker-build.sh installs Rust stable before cmake, and `scripts/verify-s01-syntax.sh` passes for all three DeepC*.cpp files.

The codebase is well ahead of where the milestone context describes it — **the scaffold already exists and passes all verification**. S02 and S03 are the substantive work.

The highest technical risk for S02 is the **Rust version gap**: the local dev environment has rustc 1.75, but `opendefocus` uses Cargo edition 2024 (stabilized in Rust 1.85). This means S02 Rust code can only be compiled inside Docker (which installs current stable from rustup, ~1.87). Local `cargo build` will fail. This is a significant workflow constraint.

---

## What Exists (S01 Delivered)

### Crate: `crates/opendefocus-deep/`
- `Cargo.toml` — `crate-type = ["staticlib"]`, edition 2021, **zero dependencies**
- `src/lib.rs` — `DeepSampleData` as `#[repr(C)]` struct; `opendefocus_deep_render()` as no-op `extern "C"` stub
- `include/opendefocus_deep.h` — hand-written C header, typedef + extern prototype
- Built artifact at `target/release/libopendefocus_deep.a` (55,956,042 bytes ✓)
- Symbol `T opendefocus_deep_render` confirmed unmangled by `nm`

### C++ scaffold: `src/DeepCOpenDefocus.cpp`
Complete `PlanarIop` subclass with:
- 4 Float_knobs: `focal_length` (50mm), `fstop` (2.8), `focus_distance` (5.0), `sensor_size_mm` (36mm)
- 3 inputs: 0=deep (required), 1=holdout deep (optional), 2=camera (optional)
- `test_input()` with `dynamic_cast<DeepOp*>` and `dynamic_cast<CameraOp*>` guards
- `input_label()` returning `""` / `"holdout"` / `"camera"`
- `renderStripe()` that iterates DeepPlane, builds `DeepSampleData` FFI struct (sample_counts[], rgba[], depth[]), calls `opendefocus_deep_render()` stub, zero-fills output
- `Op::Description` registration at `"Filter/DeepCOpenDefocus"`

### Build wiring
- `src/CMakeLists.txt`: DeepCOpenDefocus in PLUGINS + FILTER_NODES; `add_custom_command` runs `cargo build --release` (WORKING_DIRECTORY = CMAKE_SOURCE_DIR, producing artifact at workspace root `target/`); `add_dependencies`, `target_link_libraries(PRIVATE .a dl pthread m)`, `target_include_directories`
- `docker-build.sh`: curl guard + `rustup install stable --profile minimal --no-modify-path` + escaped `\$HOME` PATH export before cmake in Linux docker run block
- Root `Cargo.toml` workspace: `members = ["crates/opendefocus-deep"]`, resolver=2 — ensures `cargo build --release` from repo root lands artifact at `target/release/`

### Attribution
- `THIRD_PARTY_LICENSES.md`: full EUPL-1.2 text with Gilles Vink / codeberg.org/gillesvink/opendefocus attribution

---

## S02 Technical Landscape

### The opendefocus crate (upstream, version 0.1.10)

**Key facts from crates.io registry:**
- Uses Cargo **edition 2024** — requires `cargo` ≥ 1.85 (stabilized 2025-02-20)
- No `rust-version` constraint set, but `wgpu ^28` sets `rust_version = "1.92"` — so **rustc ≥ 1.92 required** when using the wgpu feature
- `wgpu` is a conditional dependency under the `"wgpu"` feature (default-on)
- CPU-only build possible via `default-features = false, features = ["std"]` — no wgpu, usable with rustc 1.85+
- **No build.rs** — SPIR-V kernel is pre-embedded as bytes in the crate; no nightly toolchain required for building `opendefocus` as a library
- Core async API uses `futures ^0.3` — `futures::executor::block_on` wraps async calls from sync extern "C" FFI
- `ndarray ^0.16` with rayon — `render_stripe` takes `ArrayViewMut3<f32>` (H×W×C) and optional `Array2<f32>` depth
- `circle-of-confusion ^0.2` — separate CoC crate (no GPU, pure math) for per-sample CoC computation
- `bokeh-creator`, `inpaint`, `resize` — bokeh kernel generation and gap-filling (standard flat image ops)

**opendefocus-kernel crate** (included as dep in opendefocus):
- Depends on `spirv-std = 0.9.0` (rust-gpu GPU shader std library)
- This is for the GPU shader code — the SPIR-V bytes come **pre-compiled** in the published crate
- No nightly required when using opendefocus as a *library dependency*; nightly is only needed to rebuild the SPIR-V from source (relevant only if we modify the kernel itself)

### Rust Version Constraint Summary

| Environment | rustc | Status |
|-------------|-------|--------|
| Local dev (this worktree) | 1.75.0 | ❌ Cannot compile `opendefocus` (edition2024 requires cargo 1.85+) |
| docker-build.sh Linux | fresh stable from rustup (~1.87 today) | ✅ Works for CPU path; ✅ Works for GPU path (stable ≥ 1.92 for wgpu) |

**Implication:** S02 Rust code modifications in `opendefocus-deep/src/lib.rs` can be authored but only verified via Docker. Local `cargo build` will fail. The S02 verification gate must be docker-based or use a mock build approach.

**Mitigation options:**
1. Document constraint, verify via Docker (D023 pattern — simplest)
2. Add a local `rustup toolchain install stable` step as a dev setup note
3. Upgrade the workspace dev environment to rustup stable (least infrastructure friction)

Option 1 is lowest friction for the plan.

### The Core S02 Architectural Decision: Layer Peeling vs. Native Deep Kernel

The milestone context (R049) calls for the opendefocus kernel to "natively accept deep sample data." This is the highest-risk item. Two approaches:

**Approach A: Layer Peeling (recommended for S02)**
- Sort deep samples front-to-back per pixel
- For each "layer" (iterate from max sample count across image): extract a flat RGBA image + single depth value for that layer position
- Call `opendefocus_renderer.render_stripe()` per layer with that layer's depth as a uniform depth map
- Alpha-composite each layer's output into the accumulation buffer front-to-back using correct `over` operation
- Benefit: reuses existing API unchanged; no kernel modification; works on CPU and GPU paths
- Drawback: samples at different pixels may have different depths at the same layer index → using one depth value per layer is an approximation; artifacts at depth boundaries where nearby pixels have very different sample counts
- Mitigation: can use per-pixel CoC by replacing the uniform depth array with actual per-pixel depths for each layer

**Approach B: Native Deep Kernel Modification (R049 literally)**
- Modify `opendefocus-kernel` to accept a GPU buffer with variable-length sample arrays (using offset/count array approach for GPU-compatible data layout)
- Requires modifying `opendefocus-kernel` (no-std SPIR-V Rust), which requires nightly to recompile the SPIR-V shader
- Requires a `spirv-cli-build`-style step in docker-build.sh
- High risk: GPU kernel modification is the hardest part of this whole milestone
- Genuine quality win: per-pixel per-sample CoC, exact front-to-back compositing at the GPU level

**Recommendation for planning:** S02 should implement Approach A (layer peeling) as the primary implementation — this proves R046, R047, R050, R051, R053, R058 with a real GPU path. Approach B (native kernel) can be flagged as a stretch goal or a separate S04 slice if the milestone DoD strictly requires it. The milestone completion criteria say "non-black defocused output at test resolution" — layer peeling satisfies this.

**Candidate requirement note:** If layer peeling is accepted as the S02 approach, R049 as written ("natively accept deep sample data") may need to be scoped to "layer-peeled approach with per-pixel depth" rather than "GPU kernel with variable-length deep buffers." This is a planning decision for the roadmap planner.

### renderStripe + opendefocus API Bridging

The current `renderStripe` in `DeepCOpenDefocus.cpp` already:
1. Fetches the DeepPlane for the output bounds
2. Iterates pixels and builds `sample_counts[]`, `rgba[]`, `depth[]` FFI arrays
3. Calls `opendefocus_deep_render()` with these arrays + output buffer + width + height

S02's Rust side must:
1. Add `opendefocus` as a dependency to `Cargo.toml` (with appropriate features)
2. Implement `opendefocus_deep_render()` body using the layer-peeling approach:
   - Reconstruct per-pixel sample data from the flat FFI arrays
   - Initialize `OpenDefocusRenderer` (async, block_on)
   - Compute per-pixel per-sample CoC using `circle-of-confusion` crate
   - Per layer: assemble flat RGBA + depth arrays → call `render_stripe`
   - Front-to-back composite into output_rgba buffer
3. Wire camera parameters (focal_length, fstop, focus_distance, sensor_size_mm) through the FFI — the current `opendefocus_deep_render` signature has only `*const DeepSampleData`, `*mut f32 output`, `width`, `height`. The lens parameters must be added to either a `LensParams` struct in the FFI or appended as scalar args.

**FFI ABI change note:** The current C header `opendefocus_deep_render(const DeepSampleData*, float*, uint32_t, uint32_t)` does NOT pass lens parameters. S02 must either:
- Add a `LensParams` C struct (focal_length, fstop, focus_distance, sensor_size_mm) — **recommended**, mirrors the DeepSampleData pattern
- Or add 4 float args to the signature

Updating the signature requires updating both `lib.rs`, `opendefocus_deep.h`, and `DeepCOpenDefocus.cpp`.

### GPU path wiring for S02

The `opendefocus` crate's wgpu feature enables Vulkan/Metal GPU dispatch. When `prefer_gpu = true`:
- `OpenDefocusRenderer::new(true, &mut settings)` selects the GPU backend
- Vulkan on Linux, Metal on macOS
- Falls back to CPU (rayon) if GPU unavailable

For S02, the GPU path should be enabled in the Rust code (`prefer_gpu = true`) but verified to fall back gracefully when no GPU is present (Docker CI path). The docker-build test uses CPU fallback — this is the correct test environment.

**wgpu features to enable in Cargo.toml:**
```toml
[dependencies]
opendefocus = { version = "0.1.10", features = ["std", "wgpu"] }
```
This pulls in `wgpu ^28` with `metal`, `vulkan`, `wgsl`, `spirv`, `vulkan-portability` features.

For CI/CPU-only environments:
```toml
opendefocus = { version = "0.1.10", default-features = false, features = ["std"] }
```

The recommended approach: single dep declaration with full wgpu feature (GPU + CPU fallback handled at runtime by wgpu's backend selection).

### Settings / bokeh artifacts

`opendefocus-datastructure` provides `Settings` (protobuf-backed) which configures all bokeh artifact parameters: catseye, barndoor, astigmatism, axial aberration, inverse foreground. S02's initial implementation needs a minimal `Settings` object with sane defaults; S03 can expose individual knobs. The camera lens parameters (focal_length, fstop, focus_distance, sensor_size) feed into `circle-of-confusion` for CoC, then into `Settings::RenderSpecs`.

### Test .nk script

The milestone DoD includes `nuke -x test/test_opendefocus.nk`. A minimal test script needs:
- A DeepConstant or DeepRead node generating a small deep image (128×72)
- DeepCOpenDefocus node with default parameters
- DeepToImage or Write node to verify non-black output

This can be authored as a plain text `.nk` file without running Nuke.

---

## S03 Technical Landscape

### Holdout deep input (R048)

The holdout deep input is already scaffolded in `DeepCOpenDefocus.cpp` (input 1 = "holdout", `dynamic_cast<DeepOp*>` guard). S03 implements the attenuation logic:
1. For each output pixel, after layer-peeled defocus accumulation, multiply by holdout transmittance
2. Holdout transmittance = front-to-back product of `(1 - alpha_sample)` for holdout samples whose depth ≤ current main sample's depth
3. This is a per-output-pixel depth-ordered operation

The holdout is sharp (not defocused) — it only affects the weighting of defocused main samples. Implementation is in Rust (added to `opendefocus_deep_render` or as a separate step in `renderStripe`).

**Alternative:** Holdout can be applied on the C++ side in `renderStripe`, after the Rust FFI call returns the defocused flat image, by fetching the holdout DeepPlane and computing per-pixel transmittance in C++. This avoids adding holdout data to the FFI boundary and is simpler. **Recommended for S03.**

### Camera node link (R052)

Pattern from existing opendefocus upstream plugin (`opendefocus.cpp`):
```cpp
CameraOp* cam = dynamic_cast<CameraOp*>(Op::input(2));
if (cam) {
    cam->validate();
    _focal_length = cam->focal_length();
    _fstop = cam->fStop();
    _focus_distance = cam->projection_distance();
    _sensor_size_mm = cam->film_width() * 10.0f; // filmback in cm → mm
}
```

This overrides the manual knobs when a camera is connected. The DDImage `CameraOp` API:
- `focal_length()` → mm
- `fStop()` → f-number  
- `projection_distance()` → scene units (focus distance)
- `film_width()` → in cm (multiply by 10 for mm)

All of this is in the C++ layer — no FFI changes needed.

### Menu registration and icon

`src/CMakeLists.txt` already has `DeepCOpenDefocus` in the `FILTER_NODES` list which drives `python/menu.py.in`. The menu entry will be created automatically by the existing CMake template.

An icon at `icons/DeepCOpenDefocus.png` (64×64 or 48×48 px) should be added. This is a cosmetic item but part of the S03 polish pass.

---

## Requirements Analysis

### Table stakes (must be done for the milestone to be usable)
- **R046** — flat RGBA output from deep input (S02 core)
- **R047** — per-sample CoC from real depth (S02 core, drives the whole defocus quality)
- **R050** — GPU acceleration with CPU fallback (S02, wgpu handles this automatically)
- **R053** — lens Float_knobs (already scaffolded in S01, just need to wire to Rust)
- **R055** — Docker build integration (S01 complete, S02 adds Rust dep build)
- **R056** — menu registration (CMake template handles this, S03 verification)
- **R058** — front-to-back depth-correct compositing (S02 core, the point of deep defocus)

### High-value but requires care
- **R051** — non-uniform bokeh artifacts (catseye, barndoor etc.) — these come "for free" through opendefocus Settings once the basic rendering works; risk is confirming they work with per-sample dispatch
- **R048** — holdout input (S03) — implementable on C++ side without FFI complexity
- **R052** — camera node link (S03) — pattern is well-established from upstream plugin

### Items that may need scoping
- **R049** — "natively accept deep sample data" in the kernel: layer-peeling approach satisfies the outcome (correct per-sample depth + front-to-back compositing) without native GPU kernel deep buffer support. If the planner accepts layer peeling, R049's validation criterion should be updated to reflect the implementation approach.
- **R057** — "opendefocus fork" (EUPL-1.2): currently `opendefocus-deep` is NOT a fork of opendefocus — it is a new crate that uses opendefocus as a dependency. The THIRD_PARTY_LICENSES.md attribution is correct. The "fork" language in R057 refers to the intent to maintain a modified version if needed; for S02 using opendefocus as an unmodified dependency is sufficient if layer peeling works.

### Missing/not yet surfaced
- **No test generation requirement**: The milestone DoD mentions `nuke -x test/test_opendefocus.nk` but there is no explicit requirement for a `test/` directory or test script. Should be treated as part of S03's DoD.
- **Icon requirement**: Not surfaced as a requirement; part of S03 polish. Not blocking.
- **Async runtime/thread safety**: `OpenDefocusRenderer` initialization is async. In Nuke's multi-threaded rendering, multiple `renderStripe` calls may occur concurrently. The renderer instance needs to either be per-call (expensive) or stored as a lazily-initialized singleton. This is an implementation concern for S02.

---

## Key Risks and Slice Ordering

### Risk: Rust version gap blocks local verification
**Impact:** S02 Rust code cannot be compiled locally. All cargo-level verification must happen in Docker.
**Mitigation:** S02 plan should include a docker-build verification task. The local `verify-s01-syntax.sh` script (C++ side) continues to work.

### Risk: opendefocus async API + per-frame renderer initialization latency
**Impact:** `OpenDefocusRenderer::new()` is async and may take measurable time (GPU initialization, shader compilation). If called per `renderStripe`, it will be unacceptably slow.
**Mitigation:** Use a `std::once_cell` or `lazy_static` singleton for the renderer, initialized on first call. The renderer is thread-safe per wgpu guarantees.

### Risk: Layer peeling produces visual artifacts at depth discontinuities
**Impact:** Quality degradation compared to true per-sample defocus. Acceptable for proof-of-concept, may not be acceptable for production.
**Mitigation:** This is a known approximation. The milestone's completion criterion is "non-black defocused output" not production quality. Layer count can be adaptive (use actual max sample count per stripe).

### Risk: wgpu GPU init fails silently in Docker
**Impact:** Docker build test uses CPU fallback without realizing it, GPU path unverified.
**Mitigation:** Log GPU vs CPU selection at INFO level. GPU path tested manually (or in a separate env with Vulkan).

### Risk: CoC computation with scene units vs. mm
**Impact:** `focus_distance` is in Nuke scene units (typically meters or cm). `circle-of-confusion` crate likely expects consistent units. If scene units are meters and focal_length is mm, CoC will be wrong.
**Mitigation:** Use `circle-of-confusion`'s "Real" mode which handles unit conversion. Document expected scene units in the knob tooltip. This should be investigated in S02.

---

## Existing Patterns to Reuse

| Pattern | Source | How to reuse |
|---------|--------|-------------|
| `deepEngine` + pixel iteration | `src/DeepCDepthBlur.cpp` | `renderStripe` deep fetch pattern already in DeepCOpenDefocus.cpp |
| `test_input` with `dynamic_cast` | `DeepCOpenDefocus.cpp` (S01) | Already correct |
| CameraOp link | opendefocus upstream | Copy pattern in S03 |
| Rust staticlib + extern C | `crates/opendefocus-deep/` | Add deps, implement body |
| docker-build Rust install | `docker-build.sh` | Already done; S02 only adds new dep (cargo resolves automatically) |
| `add_custom_command` cargo build | `src/CMakeLists.txt` | Already done; no CMake changes needed for adding Rust deps |
| `once_cell` / lazy init | N/A (new pattern) | Singleton renderer pattern for S02 |

---

## Slice Boundary Recommendations

### S02: Deep kernel + defocus engine (high risk, highest value)
**What to prove:** Non-black defocused flat output from a deep input. GPU path enabled, CPU fallback works.
**Key tasks:**
1. Add `opendefocus` dep to `Cargo.toml` + extend FFI signature with `LensParams` struct
2. Update `opendefocus_deep.h` and `DeepCOpenDefocus.cpp` call site for new signature
3. Implement `opendefocus_deep_render()` body: layer peeling + CoC + GPU dispatch + front-to-back composite
4. Docker build verification: `docker run ... cargo build` succeeds, DeepCOpenDefocus.so built
5. Syntax gate extension for any new C++ changes

**Boundary contract:** S02 complete = DeepCOpenDefocus.so built in Docker with GPU path compiled; `nuke -x test/test_opendefocus.nk` exits 0 with non-black EXR (CPU path in headless Docker).

### S03: Holdout + camera node + polish (medium risk)
**What to prove:** Holdout attenuates correctly; camera link overrides manual knobs; menu registration verified; test .nk and icon present.
**Key tasks:**
1. Holdout transmittance in C++ renderStripe (post-defocus, no FFI changes)
2. Camera node link in C++ (overrides knob values before passing to Rust)
3. Test .nk script in `test/` directory
4. Icon at `icons/DeepCOpenDefocus.png`
5. Full milestone DoD verification

---

## Candidate New Requirements

These are advisory (not auto-binding) but the planner should evaluate:

- **RC-A**: DeepCOpenDefocus should log GPU vs CPU backend selection at initialization (helps debugging production issues) — currently not tracked
- **RC-B**: `focus_distance` knob tooltip should document expected scene units, with a note matching opendefocus convention — minor UX gap
- **RC-C**: Minimum lens parameter bounds guards (e.g., fstop > 0, focal_length > 0) should be enforced in `_validate()` — prevents divide-by-zero in CoC calculation

---

## Summary for Roadmap Planner

**What to build first:** S02 is the core risk. The FFI signature needs extending (add LensParams), then the Rust implementation. Layer peeling approach is recommended over native kernel modification — reduces risk significantly while satisfying all output requirements.

**What constrains the approach:** opendefocus edition2024 means Rust code can only be compiled in Docker. This shapes the verification strategy.

**What existing patterns to follow:** The layer peeling approach calls `opendefocus`'s existing API per layer — no kernel modification. The singleton renderer pattern (`once_cell::Lazy`) handles the async initialization cost.

**The one irreversible choice:** Layer peeling vs. native kernel modification. Once S02 is implemented as layer peeling, moving to native kernel modification in a later slice requires replacing the entire Rust implementation body. The planner should make this call before S02 begins and update R049's validation criterion accordingly.
