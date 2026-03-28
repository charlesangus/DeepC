# S01 Research — Fork + Build Integration + FFI Scaffold

## Summary

S01 is high-complexity work: it creates brand-new integration between three build systems (CMake, cargo stable, cargo nightly), introduces Rust as a dependency into a CMake/Docker-only project, and must validate a DDImage API combination (PlanarIop + DeepOp inputs + CameraOp) that is not used anywhere in the existing codebase. The work divides cleanly into five independently verifiable units. The riskiest choices are the CMake↔cargo integration seam and the CXX-vs-extern-C FFI boundary design.

**Key finding:** The `crates/opendefocus-nuke/` referenced in the CONTEXT does **not exist** in this repo. The opendefocus project lives entirely at `codeberg.org/gillesvink/opendefocus`. S01 must create the fork structure from scratch.

**Second key finding:** S01 does NOT need the GPU kernel or nightly Rust. The GPU kernel modification is S02 scope. S01 only needs stable Rust 1.92+ to define `DeepSampleData`, compile the static lib, and link into the plugin scaffold.

---

## Requirements This Slice Owns

- **R054** — CXX FFI bridge (per-pixel sample arrays across Rust↔C++ boundary)
- **R055** — Rust static lib + Docker build integration
- **R057** — opendefocus fork with EUPL-1.2, attribution in THIRD_PARTY_LICENSES.md

Partially supports (scaffold only, full implementation in S02/S03):
- R046, R047, R049, R050 — scaffolded in no-op plugin; functional in S02

---

## Implementation Landscape

### Existing codebase — what's here

**src/DeepCDepthBlur.cpp** — The primary deep-pixel access reference. Patterns to copy:
- `DeepFilterOp` base class (deep → deep, not what we need for flat output)
- `inputs(2)`, `minimum_inputs() = 1`, `maximum_inputs() = 2`
- `getDeepRequests()` populates `RequestData` for each deep input
- `doDeepEngine()` calls `in->deepEngine(box, channels, inPlane)`, then iterates `box.begin()..end()`, calls `inPlane.getPixel(it)`, `px.getSampleCount()`, `px.getUnorderedSample(s, Chan_DeepFront)` etc.
- `Chan_DeepFront`, `Chan_DeepBack`, `Chan_Alpha` — the channels to request

**For DeepCOpenDefocus, the base class is `PlanarIop` (flat output), not `DeepFilterOp`.** The existing opendefocus plugin (`opendefocus.cpp` on GitHub) uses `PlanarIop` with `renderStripe(ImagePlane&)`. Inside `renderStripe` we must call `deepOp->deepEngine()` to fetch deep data, then flatten to flat RGBA.

**src/CMakeLists.txt** — Plugin registration pattern:
```cmake
# To add DeepCOpenDefocus:
list(APPEND PLUGINS DeepCOpenDefocus)
list(APPEND FILTER_NODES DeepCOpenDefocus)
# add_nuke_plugin macro handles the rest
```
Static lib linking must be added to `target_link_libraries(DeepCOpenDefocus ...)`.

**docker-build.sh** — The Linux build runs a single `docker run` with a bash `-c` string that does `cmake ... && cmake --build ... && cmake --install ...`. Rust toolchain must be installed **inside the container** before CMake runs. The image is `nukedockerbuild:${nuke_version}-linux`. That image is immutable — we can't modify it. The bash `-c` string must be extended to `curl ... rustup install ... cargo build ... cmake ... cmake --build ...`.

**scripts/verify-s01-syntax.sh** — Mock DDImage headers currently cover: `Op`, `Channel`, `Box`, `DeepPixel`, `DeepPlane`, `DeepFilterOp`, `Knobs`. For DeepCOpenDefocus it must be extended with:
- `DDImage/PlanarIop.h` (PlanarIop base class, ImagePlane)
- `DDImage/CameraOp.h` (CameraOp, focalLength, fStop, focusDistance, horizontalAperture)
- `DDImage/DeepOp.h` if not already in DeepFilterOp.h

**THIRD_PARTY_LICENSES.md** — Pattern established for DeepThinner and FastNoise. opendefocus entry needs: project, author (Gilles Vink), source (codeberg.org/gillesvink/opendefocus), license (EUPL-1.2), full license text.

---

### opendefocus upstream — what to fork

**Repo:** `https://codeberg.org/gillesvink/opendefocus` (primary), GitHub mirror at `https://github.com/gillesvink/opendefocus`

**Crate structure:**
```
crates/
  opendefocus/          # Main library — OpenDefocusRenderer, render_stripe
  opendefocus-shared/   # Shared types between kernel and main
  opendefocus-kernel/   # no-std GPU kernel (SPIR-V via rust-gpu)
  opendefocus-datastructure/  # Settings, protobuf bindings
  opendefocus-nuke/     # Nuke plugin (CXX FFI, cdylib)
  spirv-cli-build/      # nightly wrapper for SPIR-V compilation
```

**Key API (opendefocus crate):**
```rust
pub struct OpenDefocusRenderer { runner: SharedRunner, gpu: bool }

impl OpenDefocusRenderer {
    pub async fn new(prefer_gpu: bool, settings: &mut Settings) -> Result<Self>
    pub async fn render_stripe(
        &self, render_specs: RenderSpecs, settings: Settings,
        image: ArrayViewMut3<f32>,    // H×W×C
        depth: Option<Array2<f32>>,   // H×W — ONE depth per pixel
        filter: Option<Array2<f32>>,
    ) -> Result<()>
}
```

**Critical constraint:** `render_stripe` takes a 2D depth array (one value per pixel). Deep images have **variable sample counts per pixel, each with their own depth**. The existing API cannot accept deep data — this is why the fork is needed.

**S01 fork scope (minimal — kernel mods deferred to S02):**
Only needs to:
1. Define `DeepSampleData` Rust struct
2. Expose C-compatible FFI entry points (stubs in S01, real impl in S02)
3. Build as `staticlib` so CMake can link it

S01 does NOT need to modify `opendefocus-kernel` or build SPIR-V shaders.

**Rust version requirements:**
- Stable 1.92+ — sufficient for S01 (data structures, CXX FFI, static lib)
- Nightly — only needed for `spirv-cli-build` and GPU kernel (S02)
- S01 docker-build.sh extension: install stable only

**opendefocus-nuke build.rs pattern** (reference for our bridge):
```rust
// build.rs uses cxx_build:
CFG.exported_header_dirs.extend([nuke_path.as_path()]);
let mut builder = cxx_build::bridge("src/lib.rs");
builder.std("c++17").file("src/opendefocus.cpp").cpp(true);
builder.compile("opendefocus-nuke");
// Links DDImage dynamically:
println!("cargo:rustc-link-lib=dylib=DDImage");
```

---

## FFI Boundary Design — Critical Decision

**Decision D022** says: use CXX crate. **Decision D023** says: cargo → static .a linked from CMake.

These create a tension. CXX's `cxx_build` in `build.rs` compiles C++ glue internally during `cargo build`. For a static lib linked from CMake, CMake must also compile the CXX-generated C++ header/source files.

**Two viable approaches:**

### Approach A: Plain extern "C" (recommended for S01)
- Rust exports `#[no_mangle] pub extern "C"` functions
- Hand-written C header in `crates/opendefocus-deep/include/opendefocus_deep.h`
- CMake: `target_link_libraries(DeepCOpenDefocus PRIVATE ${CMAKE_SOURCE_DIR}/crates/opendefocus-deep/target/release/libopendefocus_deep.a)`
- Much simpler CMake integration, no generated header path chasing
- Type safety enforced by Rust side

```c
// opendefocus_deep.h (hand-written)
typedef struct DeepSampleData {
    const uint32_t* sample_counts; // per pixel
    const float* rgba;             // flattened [total_samples × 4]
    const float* depth;            // flattened [total_samples]
    uint32_t pixel_count;
    uint32_t total_samples;
} DeepSampleData;

extern void opendefocus_deep_render(const DeepSampleData*, float* output_rgba, uint32_t width, uint32_t height);
```

### Approach B: CXX crate (matches D022 exactly)
- `cxx::bridge` in Rust defines `DeepSampleData` as a shared struct
- `build.rs` with `cxx_build` generates `target/cxxbridge/crate_name/src/lib.rs.h`
- CMake must add `crates/opendefocus-deep/target/cxxbridge/` to include paths
- CMake must also compile `target/cxxbridge/crates/opendefocus-deep/src/lib.rs.cc`
- More complex but matches the upstream opendefocus FFI pattern exactly

**Recommendation:** Implement Approach A for S01 (extern "C" + hand header) for build simplicity. The planner can revisit D022 — the "spirit" of D022 was to avoid cbindgen/manual unsafe; extern "C" with Rust is perfectly safe and simpler than CXX for a static lib. If the planner wants strict D022 adherence (CXX), Approach B needs explicit CMake steps to include cxxbridge output.

---

## PlanarIop + DeepOp Input Validation

The CONTEXT flags this as an open question: "Verify DDImage supports 3 inputs with mixed types (2 DeepOp + 1 CameraOp)."

**Analysis from existing code:**
- `DeepCKeymix` uses `dynamic_cast<DeepOp*>(Op::input(1))` — inputs can be cast to DeepOp
- `opendefocus.cpp` (upstream) does `dynamic_cast<DD::Image::CameraOp*>(DD::Image::Op::input(CAMERA_INPUT))` from a PlanarIop
- `test_input()` controls what types are accepted per index

**Expected DDImage pattern for DeepCOpenDefocus:**
```cpp
class DeepCOpenDefocus : public PlanarIop {
    bool test_input(int n, Op* op) const override {
        if (n == 0 || n == 1) return dynamic_cast<DeepOp*>(op) != nullptr;
        if (n == 2) return dynamic_cast<CameraOp*>(op) != nullptr;
        return false;
    }
    int minimum_inputs() const override { return 1; }
    int maximum_inputs() const override { return 3; }
};
```

Inside `renderStripe`, fetch deep data directly:
```cpp
DeepOp* deepIn = dynamic_cast<DeepOp*>(Op::input(0));
DeepPlane deepPlane;
deepIn->deepEngine(output_plane.bounds(), channels, deepPlane);
// iterate pixels, extract samples...
```

This is validated by the upstream opendefocus plugin doing CameraOp cast from PlanarIop. The DeepOp cast from a PlanarIop should similarly work — it's DDImage's Op polymorphism, not specific to DeepFilterOp.

---

## CMake↔Cargo Integration

**Pattern:** Add a CMake `add_custom_command` or `execute_process` that runs `cargo build --release` before building DeepCOpenDefocus, then link the `.a`.

```cmake
# In CMakeLists.txt or src/CMakeLists.txt:

set(OPENDEFOCUS_DEEP_CRATE_DIR "${CMAKE_SOURCE_DIR}/crates/opendefocus-deep")
set(OPENDEFOCUS_DEEP_LIB "${OPENDEFOCUS_DEEP_CRATE_DIR}/target/release/libopendefocus_deep.a")

add_custom_command(
    OUTPUT ${OPENDEFOCUS_DEEP_LIB}
    COMMAND cargo build --release
    WORKING_DIRECTORY ${OPENDEFOCUS_DEEP_CRATE_DIR}
    COMMENT "Building opendefocus-deep Rust static lib"
)
add_custom_target(opendefocus_deep_lib ALL DEPENDS ${OPENDEFOCUS_DEEP_LIB})

add_nuke_plugin(DeepCOpenDefocus DeepCOpenDefocus.cpp)
add_dependencies(DeepCOpenDefocus opendefocus_deep_lib)
target_link_libraries(DeepCOpenDefocus PRIVATE ${OPENDEFOCUS_DEEP_LIB})
# Also need system libs that Rust static libs depend on:
target_link_libraries(DeepCOpenDefocus PRIVATE dl pthread)
```

**Docker constraint:** The NukeDockerBuild images do NOT have Rust installed. The docker-build.sh bash `-c` string must install rustup before calling CMake:

```bash
bash -c "
    # Install Rust stable (no nightly needed for S01)
    curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --default-toolchain stable --profile minimal &&
    export PATH=\"\$HOME/.cargo/bin:\$PATH\" &&
    # Build Rust static lib
    cargo build --release --manifest-path /nuke_build_directory/crates/opendefocus-deep/Cargo.toml &&
    # CMake build as before
    cmake -S /nuke_build_directory ... &&
    cmake --build ... &&
    cmake --install ...
"
```

**Alternative:** Use CMake `execute_process` instead of `add_custom_command` — runs at configure time, simpler but less incremental. For the Docker build (always clean), either works.

---

## Task Decomposition (for planner)

### T01: Create opendefocus fork Rust crate
**Files:** `crates/opendefocus-deep/Cargo.toml`, `crates/opendefocus-deep/src/lib.rs`, `crates/opendefocus-deep/include/opendefocus_deep.h`

- New Rust workspace or standalone crate at `crates/opendefocus-deep/`
- `crate-type = ["staticlib"]` 
- Defines `DeepSampleData` struct (per-pixel sample counts, RGBA, depth slices)
- Exports stub `opendefocus_deep_render()` via `#[no_mangle] extern "C"`
- Minimal dependencies (no wgpu, no opendefocus-kernel yet)
- Hand-written C header at `crates/opendefocus-deep/include/opendefocus_deep.h`
- `Cargo.toml` at root (`crates/opendefocus-deep/Cargo.toml`) — no workspace.toml needed yet
- Verify: `cargo build --release` produces `target/release/libopendefocus_deep.a`

### T02: CMake + docker-build.sh integration
**Files:** `CMakeLists.txt` (or `src/CMakeLists.txt`), `docker-build.sh`

- Add `add_custom_command` / `add_custom_target` for cargo build
- `target_link_libraries(DeepCOpenDefocus PRIVATE ${OPENDEFOCUS_DEEP_LIB} dl pthread)`
- `list(APPEND PLUGINS DeepCOpenDefocus)` and `list(APPEND FILTER_NODES DeepCOpenDefocus)` in `src/CMakeLists.txt`
- Extend docker-build.sh Linux bash `-c` string with rustup install + cargo build
- Verify: docker run succeeds with Rust installed and lib built

### T03: DeepCOpenDefocus.cpp PlanarIop scaffold
**Files:** `src/DeepCOpenDefocus.cpp`

- `#include "DDImage/PlanarIop.h"`, `DeepOp.h`, `DeepPlane.h`, `CameraOp.h`, `Knobs.h`
- `#include "../crates/opendefocus-deep/include/opendefocus_deep.h"`
- `class DeepCOpenDefocus : public PlanarIop`
- `inputs(3)`, `minimum_inputs() = 1`, `maximum_inputs() = 3`
- `test_input()` — 0/1 = DeepOp, 2 = CameraOp
- `input_label()` — "" / "holdout" / "camera"
- `knobs()` — `Float_knob` for focal_length, fstop, focus_distance, sensor_size_mm (4 knobs)
- `_validate()` — `copy_info()`, clear alpha, set out channels to RGBA, validate deep input connected
- `renderStripe()` — fetch deep plane from input 0, extract per-pixel sample data into `DeepSampleData` struct, call `opendefocus_deep_render()` (stub returns zeros), write zeros to output plane (no-op)
- Menu registration: `const Op::Description DeepCOpenDefocus::d(CLASS, "Filter/DeepCOpenDefocus", build)`

### T04: Extend verify script + THIRD_PARTY_LICENSES.md
**Files:** `scripts/verify-s01-syntax.sh`, `THIRD_PARTY_LICENSES.md`

- Add mock `DDImage/PlanarIop.h` (PlanarIop base, ImagePlane, renderStripe signature)
- Add mock `DDImage/CameraOp.h` (CameraOp, focalLength, fStop, focusDistance, horizontalAperture, verticalAperture)
- Add `opendefocus_deep.h` to include path in g++ invocation
- Add `DeepCOpenDefocus.cpp` to the syntax check loop
- Add opendefocus EUPL-1.2 entry to THIRD_PARTY_LICENSES.md (author: Gilles Vink, source: codeberg.org/gillesvink/opendefocus)

### T05: Integration verification
**Verify:** `docker-build.sh --linux --versions 16.0` produces `DeepCOpenDefocus.so` in release zip. File exists, non-zero size. No load errors (structural check only — no Nuke execution in Docker).

---

## Verification Commands

```bash
# T01 — Rust lib compiles
cd crates/opendefocus-deep && cargo build --release
ls -la target/release/libopendefocus_deep.a

# T03 — C++ syntax check
bash scripts/verify-s01-syntax.sh

# T04 — License attribution present
grep -q "opendefocus" THIRD_PARTY_LICENSES.md && grep -q "EUPL-1.2" THIRD_PARTY_LICENSES.md

# T05 — Docker build (authoritative)
./docker-build.sh --linux --versions 16.0
ls release/DeepC-Linux-Nuke16.0.zip
unzip -l release/DeepC-Linux-Nuke16.0.zip | grep DeepCOpenDefocus.so
```

---

## Risks and Constraints

### Risk 1: Rust syscall dependencies in static lib (HIGH)
When Rust staticlib is linked into a C++ .so, Rust's standard library pulls in system dependencies. On Linux: `libdl`, `libpthread`, `libm`, `libc`. These must be added to `target_link_libraries`. Missing any of them produces linker errors at docker build time (not at compile time). **Mitigation:** Add `-Wl,--start-group ${LIB_A} -Wl,--end-group dl pthread m` to avoid link order issues.

### Risk 2: PlanarIop + DeepOp input combination not validated (MEDIUM)
No existing DeepC plugin combines PlanarIop (flat output) with `deepOp->deepEngine()` calls inside `renderStripe`. The upstream opendefocus plugin uses `input->fetchPlane()` (flat inputs). T03 should verify this compiles and that `deepEngine()` is callable from a PlanarIop context — **the mock headers must include the right declarations for both**.

### Risk 3: Rustup in NukeDockerBuild image (MEDIUM)
The nukedockerbuild images are minimal GCC/CMake images. `curl` may or may not be present. If not: use `wget` fallback or install curl first. Test with `docker run nukedockerbuild:16.0-linux bash -c "which curl || which wget"`. If neither: `apt-get install -y curl` first. The bash `-c` string should be robust.

### Risk 4: cargo CARGO_HOME in Docker (LOW)
`rustup` installs to `$HOME/.cargo` inside the container. The container runs as root (NukeDockerBuild convention). `$HOME` = `/root`. `$PATH` must be extended with `/root/.cargo/bin`. The bash `-c` string must source or export `PATH` before calling `cargo`.

### Risk 5: CMake add_custom_command not triggered if lib already exists (LOW)
`add_custom_command` uses the OUTPUT file as a staleness check. In Docker (always clean), this is fine. In a local dev build, touching a Rust source file won't invalidate the CMake dependency automatically. This is acceptable for S01 (Docker is the authoritative gate).

---

## What NOT to Build in S01

- Do NOT modify `opendefocus-kernel` (the GPU kernel) — that's S02
- Do NOT implement `opendefocus_deep_render()` beyond a stub — S02 does the real impl
- Do NOT add nightly Rust or spirv-cli-build — S02 needs it, S01 does not
- Do NOT implement holdout attenuation or camera node extraction — those are S03
- Do NOT implement the test .nk script — S02
- Do NOT implement front-to-back compositing — S02

---

## Skills Discovered

No additional skills installed — this is primarily Rust/CMake/DDImage work covered by existing knowledge base and the upstream opendefocus source code reference.

---

## Sources Consulted

- `src/DeepCDepthBlur.cpp` — deep pixel access pattern (DeepFilterOp)
- `src/CMakeLists.txt` — plugin registration and add_nuke_plugin pattern
- `docker-build.sh` — Linux build command structure
- `scripts/verify-s01-syntax.sh` — mock header pattern
- `THIRD_PARTY_LICENSES.md` — attribution format
- `https://github.com/gillesvink/opendefocus` — upstream repo structure
- `crates/opendefocus/Cargo.toml` — opendefocus crate dependencies
- `crates/opendefocus-nuke/Cargo.toml` — CXX FFI crate structure
- `crates/opendefocus-nuke/build.rs` — cxx_build pattern
- `crates/opendefocus-nuke/src/opendefocus.cpp` — PlanarIop + CXX pattern
- `crates/opendefocus/src/lib.rs` — OpenDefocusRenderer API (render_stripe signature)
