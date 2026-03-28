# S02: Deep defocus engine — layer peeling + GPU dispatch

**Goal:** Implement the real opendefocus_deep_render() body using the layer-peel approach: add opendefocus as a Cargo dep, extend the FFI with a LensParams struct, implement per-pixel CoC via circle-of-confusion, dispatch each layer through OpenDefocusRenderer (GPU with CPU fallback), front-to-back composite, and verify with docker build + test .nk that produces non-black defocused EXR output.
**Demo:** After this: Deep input → visibly defocused flat RGBA output. Per-sample CoC from real depth. GPU-accelerated convolution with all non-uniform bokeh artifacts. Front-to-back depth-correct compositing. nuke -x test/test_opendefocus.nk produces non-black 128×72 EXR.

## Tasks
- [x] **T01: Added LensParams #[repr(C)] struct and updated opendefocus_deep_render FFI signature to carry lens parameters (focal_length_mm, fstop, focus_distance, sensor_size_mm) across the C++/Rust boundary** — The current opendefocus_deep_render() signature takes only DeepSampleData*, float*, uint32_t, uint32_t. Lens parameters (focal_length, fstop, focus_distance, sensor_size_mm) must be passed from C++ to Rust for CoC computation.

1. Add `LensParams` #[repr(C)] struct to `crates/opendefocus-deep/src/lib.rs` with fields: focal_length_mm (f32), fstop (f32), focus_distance (f32), sensor_size_mm (f32).
2. Update `opendefocus_deep_render` extern C signature to accept `*const LensParams` as a 5th argument.
3. Update `include/opendefocus_deep.h`: add LensParams typedef + updated prototype.
4. Update `src/DeepCOpenDefocus.cpp` renderStripe call site to construct and pass a LensParams on the stack from the knob values.
5. Run scripts/verify-s01-syntax.sh to confirm C++ still compiles (local syntax check, no cargo needed).
  - Estimate: 1h
  - Files: crates/opendefocus-deep/src/lib.rs, crates/opendefocus-deep/include/opendefocus_deep.h, src/DeepCOpenDefocus.cpp
  - Verify: scripts/verify-s01-syntax.sh passes (C++ side). nm on existing .a still shows opendefocus_deep_render (note: .a from S01 is for the old signature — full verification happens in T04 docker build).
- [x] **T02: Implemented full layer-peel deep defocus engine in lib.rs with opendefocus GPU dispatch; added Cargo deps; fixed verify-s01-syntax.sh for POSIX sh compatibility** — Implement the full opendefocus_deep_render() body in Rust.

1. Update `crates/opendefocus-deep/Cargo.toml`: add `opendefocus = { version = "0.1.10", features = ["std", "wgpu"] }`, `circle-of-confusion = "0.2"`, `futures = "0.3"`, `once_cell = "1"`.
2. Add workspace-level dep versions to root `Cargo.toml` if needed.
3. In `lib.rs` implement `opendefocus_deep_render()`:
   a. Log crate init: eprintln! backend TBD (actual logging from renderer init in step c).
   b. Reconstruct per-pixel sample arrays from flat FFI arrays (sample_counts, rgba, depth) into a Vec<Vec<(f32,f32,f32,f32,f32)>> (pixel → samples of (r,g,b,a,z)).
   c. Init renderer via `once_cell::sync::Lazy<Mutex<OpenDefocusRenderer>>` (block_on the async new()). Log 'GPU backend: Vulkan' or 'CPU fallback: rayon' based on renderer backend field.
   d. Sort each pixel's samples by depth front-to-back.
   e. Determine max_layers = max sample count across all pixels.
   f. Allocate accum buffer (width×height×4 f32, zeroed).
   g. For layer i in 0..max_layers:
      - Build flat RGBA array (width×height×4): for pixels with sample i, use that sample's rgba; others use 0,0,0,0.
      - Build depth map (width×height): per-pixel use sample[i].z if it exists, else 0.0.
      - Compute per-pixel CoC from depth map using circle_of_confusion::compute() with lens params.
      - Construct opendefocus Settings with sane defaults + CoC values.
      - Call renderer.render_stripe() on the flat RGBA + CoC.
      - Front-to-back composite rendered layer into accum buffer: out = over(layer, accum).
   h. Write accum buffer into output_rgba.
4. Add `use` imports and handle all unsafe pointer dereferences safely.
5. Note: `cargo build` will fail locally (rustc 1.75) — this is expected. Verification is in T04.
  - Estimate: 4h
  - Files: crates/opendefocus-deep/Cargo.toml, crates/opendefocus-deep/src/lib.rs, Cargo.toml
  - Verify: cargo metadata --no-deps (inspect Cargo.toml is parseable). Rust source file well-formed (rustfmt --check if available). Full cargo build verified in T04.
- [x] **T03: Created test/test_opendefocus.nk with DeepConstant/DeepCOpenDefocus/Write nodes and extended verify-s01-syntax.sh to enforce .nk existence** — Create the test .nk script that the milestone DoD requires.

1. Create `test/` directory if not present.
2. Write `test/test_opendefocus.nk` as a plain-text Nuke script that:
   a. Creates a DeepConstant node generating a 128×72 deep image with non-zero RGBA and a constant depth of 5.0.
   b. Connects it to a DeepCOpenDefocus node with default knob values (focal_length=50, fstop=2.8, focus_distance=5.0, sensor_size_mm=36).
   c. Connects the output to a Write node writing to /tmp/test_opendefocus.exr format exr.
   d. Sets the frame range to 1-1.
3. Extend `scripts/verify-s01-syntax.sh` (or create `scripts/verify-s02-syntax.sh`) to also check that `test/test_opendefocus.nk` exists.
4. Optionally: check that `include/opendefocus_deep.h` parses with clang -fsyntax-only (if clang available).
  - Estimate: 1h
  - Files: test/test_opendefocus.nk, scripts/verify-s01-syntax.sh
  - Verify: bash scripts/verify-s01-syntax.sh passes. cat test/test_opendefocus.nk shows DeepConstant, DeepCOpenDefocus, Write node entries.
- [x] **T04: Docker build verified: cargo resolves opendefocus 0.1.10 (248 deps) and cmake links DeepCOpenDefocus.so (21 MB); six API mismatches fixed in lib.rs and DeepCOpenDefocus.cpp** — Run docker-build.sh to verify the full pipeline compiles end-to-end with the new Cargo dep.

1. Run `bash docker-build.sh` (or the Linux-target docker run command extracted from it) and capture output.
2. Confirm cargo resolves opendefocus 0.1.10 + wgpu deps without error.
3. Confirm cmake links DeepCOpenDefocus.so successfully.
4. Capture the docker log line showing 'CPU fallback' or 'GPU backend' (if nuke test runs inside docker).
5. If nuke -x is available inside the docker image: run nuke -x test/test_opendefocus.nk and confirm exit 0 + non-black EXR.
6. If nuke is not in the docker image: confirm .so was produced and record the constraint — nuke test must be run separately in an env with Nuke installed.
7. Document any build errors encountered and their fixes.
8. Update scripts/verify-s01-syntax.sh to add a note about docker-only cargo verification.
  - Estimate: 2h
  - Files: docker-build.sh, crates/opendefocus-deep/Cargo.toml, crates/opendefocus-deep/src/lib.rs
  - Verify: docker-build.sh exits 0. DeepCOpenDefocus.so present in build output. cargo build --release log shows opendefocus dep resolved. Log contains 'Compiling opendefocus' and 'Compiling opendefocus-deep'.
