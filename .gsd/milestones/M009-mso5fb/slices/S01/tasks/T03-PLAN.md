---
estimated_steps: 23
estimated_files: 1
skills_used: []
---

# T03: Write DeepCOpenDefocus.cpp PlanarIop no-op scaffold

Create `src/DeepCOpenDefocus.cpp` — a PlanarIop-based Nuke plugin that accepts a Deep input (input 0), optional holdout Deep input (input 1), and optional CameraOp input (input 2). The plugin loads in Nuke, exposes lens knobs, and calls the stub FFI function, writing zeros to the output plane. This is the no-op scaffold; S02 will replace the stub call with the real GPU kernel invocation.

This satisfies R054 (CXX FFI bridge: C++ side calls across Rust boundary) and partially satisfies R046/R047/R049/R050 (scaffolded plugin exists).

Implementation steps:
1. Includes: `DDImage/PlanarIop.h`, `DDImage/DeepOp.h`, `DDImage/DeepPlane.h`, `DDImage/CameraOp.h`, `DDImage/Knobs.h`, `DDImage/Row.h`, `../crates/opendefocus-deep/include/opendefocus_deep.h`, `<vector>`, `<cstring>`.
2. Class `DeepCOpenDefocus : public DD::Image::PlanarIop` — private members: `float _focal_length`, `_fstop`, `_focus_distance`, `_sensor_size_mm` with sensible defaults (50mm, f/2.8, 5.0, 36mm).
3. `inputs(3)`, `minimum_inputs() const override { return 1; }`, `maximum_inputs() const override { return 3; }`.
4. `test_input(int n, Op* op) const override` — n==0||n==1: `dynamic_cast<DD::Image::DeepOp*>(op) != nullptr`; n==2: `dynamic_cast<DD::Image::CameraOp*>(op) != nullptr`; else false. Return true for nullptr to allow optional inputs.
5. `input_label(int n, char*) const override` — returns "", "holdout", "camera" for 0/1/2.
6. `knobs(DD::Image::Knob_Callback f) override` — add `Float_knob(f, &_focal_length, "focal_length", "Focal Length")`, `Float_knob(f, &_fstop, "fstop", "F-Stop")`, `Float_knob(f, &_focus_distance, "focus_distance", "Focus Distance")`, `Float_knob(f, &_sensor_size_mm, "sensor_size_mm", "Sensor Size (mm)")`.
7. `_validate(bool for_real) override` — call `PlanarIop::_validate(for_real)`, set output ChannelSet to RGBA (Chan_Red|Chan_Green|Chan_Blue|Chan_Alpha).
8. `getRequirements(DD::Image::Descriptions& desc) override` — call `PlanarIop::getRequirements(desc)` (or minimal stub if not needed by mock headers).
9. `renderStripe(DD::Image::ImagePlane& output) override`:
   - Get deep input: `DD::Image::DeepOp* deepIn = dynamic_cast<DD::Image::DeepOp*>(input(0));`
   - Fetch deep plane: `DD::Image::DeepPlane deepPlane; deepIn->deepEngine(output.bounds(), channels, deepPlane);` (use appropriate ChannelSet).
   - Build `std::vector<uint32_t> sampleCounts`, `std::vector<float> rgba_flat`, `std::vector<float> depth_flat` by iterating pixels and calling `deepPlane.getPixel(y, x)`, `px.getSampleCount()`, `px.getUnorderedSample(s, Chan_*)` for RGBA channels and `Chan_DeepFront` for depth.
   - Populate `DeepSampleData` struct with pointers to those vectors.
   - Allocate `output_buf` of zeros (width * height * 4 floats).
   - Call `opendefocus_deep_render(&sampleData, output_buf.data(), width, height)` (stub returns immediately).
   - Write zeros to `output` ImagePlane via `output.writable()` row iteration (or `std::memset` equivalent via the plane's pointer).
10. `static const char* const CLASS = "DeepCOpenDefocus";`
11. `static DD::Image::Op* build(DD::Image::Node* node) { return new DeepCOpenDefocus(node); }`
12. `const DD::Image::Op::Description DeepCOpenDefocus::d(CLASS, "Filter/DeepCOpenDefocus", build);`

Note on ImagePlane writing: look at how existing PlanarIop plugins write output (the upstream opendefocus.cpp pattern). The simplest correct approach for a no-op: call `output.makeWritable()` then `std::memset` the writable pointer to 0 for the full plane stride*height*channels bytes.

## Inputs

- `src/DeepCDepthBlur.cpp`
- `src/CMakeLists.txt`
- `crates/opendefocus-deep/include/opendefocus_deep.h`
- `scripts/verify-s01-syntax.sh`

## Expected Output

- `src/DeepCOpenDefocus.cpp`

## Verification

bash scripts/verify-s01-syntax.sh 2>&1 | grep -E 'passed|error' | head -20
