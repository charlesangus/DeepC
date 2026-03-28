# S02: Deep defocus engine — layer peeling + GPU dispatch — UAT

**Milestone:** M009-mso5fb
**Written:** 2026-03-28T11:26:09.374Z

## UAT: S02 — Deep defocus engine layer peeling + GPU dispatch

### Preconditions
- Docker is installed and `nukedockerbuild:16.0-linux` image is available locally
- Working directory: repo root (M009-mso5fb worktree)
- `target/release/libopendefocus_deep.a` exists (58 MB, from T04 docker build) OR a clean build will be performed
- Rust stable toolchain NOT required locally — all Rust compilation runs inside docker

---

### TC-01: docker-build.sh exits 0 and produces DeepCOpenDefocus.so

**Precondition:** nukedockerbuild:16.0-linux docker image present

**Steps:**
1. Run `bash docker-build.sh --linux --versions 16.0`
2. Observe output

**Expected:**
- Exit code 0
- Output contains `[53%] Built target DeepCOpenDefocus` (or equivalent cmake target line)
- Output contains `Linux build complete: release/DeepC-Linux-Nuke16.0.zip`
- File `build/16.0-linux/src/DeepCOpenDefocus.so` exists, size ≥ 20 MB
- File `release/DeepC-Linux-Nuke16.0.zip` exists and contains `DeepC/DeepCOpenDefocus.so`

**Failure signal:** Exit code non-zero, or DeepCOpenDefocus.so missing from build output.

---

### TC-02: libopendefocus_deep.a built with opendefocus 0.1.10 deps resolved

**Steps:**
1. Run `ls -lh target/release/libopendefocus_deep.a`
2. Run `grep -A2 'name = "opendefocus"' Cargo.lock | head -4`

**Expected:**
- `libopendefocus_deep.a` exists, size ≥ 55 MB (actual: 58 MB)
- Cargo.lock contains `name = "opendefocus"` / `version = "0.1.10"`

---

### TC-03: opendefocus_deep_render exported as defined text symbol

**Steps:**
1. Run `nm build/16.0-linux/src/DeepCOpenDefocus.so | grep opendefocus_deep_render`

**Expected:**
- Output: `<address> T opendefocus_deep_render`
- The `T` flag means the symbol is defined (not `U` = undefined) in the text section
- Symbol is NOT marked `U` (which would mean the Rust function was not linked)

---

### TC-04: verify-s01-syntax.sh passes all S01 + S02 checks

**Steps:**
1. Run `bash scripts/verify-s01-syntax.sh`

**Expected output (exact lines):**
```
Syntax check passed: DeepCBlur.cpp
Syntax check passed: DeepCDepthBlur.cpp
Syntax check passed: DeepCOpenDefocus.cpp
All syntax checks passed.
test/test_opendefocus.nk exists — OK
All S02 checks passed.
```
- Exit code 0

---

### TC-05: LensParams FFI struct and updated prototype in C header

**Steps:**
1. Run `grep -A8 "LensParams" crates/opendefocus-deep/include/opendefocus_deep.h`
2. Run `grep "opendefocus_deep_render" crates/opendefocus-deep/include/opendefocus_deep.h`

**Expected:**
- LensParams typedef with fields: `focal_length_mm`, `fstop`, `focus_distance`, `sensor_size_mm` (all float)
- `opendefocus_deep_render` prototype includes `const LensParams*` as 5th parameter

---

### TC-06: test/test_opendefocus.nk contains required Nuke nodes

**Steps:**
1. Run `grep -l "DeepConstant\|DeepCOpenDefocus\|Write" test/test_opendefocus.nk` (returns filename)
2. Run `grep "DeepConstant" test/test_opendefocus.nk`
3. Run `grep "DeepCOpenDefocus" test/test_opendefocus.nk`
4. Run `grep "Write" test/test_opendefocus.nk`

**Expected:**
- All three node types are present in the .nk file
- DeepConstant specifies 128×72 frame format and depth value of 5.0
- DeepCOpenDefocus has knob values: focal_length 50, fstop 2.8, focus_distance 5.0, sensor_size_mm 36
- Write node output path is `/tmp/test_opendefocus.exr`

---

### TC-07: Cargo.toml has opendefocus dep with wgpu feature

**Steps:**
1. Run `cat crates/opendefocus-deep/Cargo.toml`

**Expected:**
- `opendefocus = { version = "0.1.10", features = ["std", "wgpu"] }` present
- `ndarray = "0.16"` present (required for Array2/Array3 in lib.rs)
- `futures = "0.3"` present (required for block_on)
- `once_cell = "1"` present (scaffolded for S03 singleton)
- `crate-type = ["staticlib"]` present

---

### TC-08: lib.rs implements layer-peel algorithm

**Steps:**
1. Run `grep -n "front.to.back\|block_on\|render_stripe\|front_back\|layer\|accum" crates/opendefocus-deep/src/lib.rs | head -20`
2. Run `grep "LensParams" crates/opendefocus-deep/src/lib.rs | head -5`

**Expected:**
- `block_on` or `executor::block_on` present (async render dispatch)
- `render_stripe` or equivalent render call present
- Accumulator pattern present (front-to-back compositing)
- `LensParams` struct with `#[repr(C)]` attribute

---

### TC-09: Nuke runtime test (requires Nuke installation — deferred to S03 or manual UAT)

**Precondition:** Nuke 16.0 installed at a known path with DeepCOpenDefocus.so in the plugin path

**Steps:**
1. Copy `build/16.0-linux/src/DeepCOpenDefocus.so` to the Nuke plugin directory
2. Run `nuke -x test/test_opendefocus.nk`
3. Check `ls -lh /tmp/test_opendefocus.exr`
4. Open `/tmp/test_opendefocus.exr` in Nuke or nuke-compatible viewer

**Expected:**
- nuke -x exits 0
- `/tmp/test_opendefocus.exr` exists, size > 0
- EXR is non-black: pixel values ≠ 0 in RGBA channels
- Output shows visible defocus blur (not identical to input flat constant)

**Note:** This test CANNOT be automated without Nuke in the CI environment. It is a manual UAT gate for S03 sign-off.

---

### Edge Cases

**EC-01: Incremental rebuild (artifacts cached)**
- If `target/release/libopendefocus_deep.a` already exists and is newer than source, `cmake --build` will print `[x%] Built target opendefocus_deep_lib` but NOT re-run cargo. This is correct and expected — exit 0 is still a valid pass.

**EC-02: Clean rebuild (no cached artifacts)**
- Remove `target/` and `build/` and re-run `bash docker-build.sh --linux --versions 16.0`
- Should produce the same artifacts; expect "Compiling opendefocus v0.1.10" and "Compiling opendefocus-deep v0.1.0" in build log
- Takes ~2-3 minutes (cargo + cmake combined)

**EC-03: Rocky Linux 8 container prerequisites**
- The docker-build.sh now handles cc symlink and protobuf-compiler installation automatically
- If these fail silently (dnf repo unreachable), the build will fail with "linker `cc` not found" or "protoc not found" — check network connectivity inside container

