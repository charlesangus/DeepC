# S01: Fork + build integration + FFI scaffold — UAT

**Milestone:** M009-mso5fb
**Written:** 2026-03-28T10:51:47.337Z

## UAT: S01 — Fork + build integration + FFI scaffold

### Preconditions
- Rust stable toolchain (≥1.75) installed and on PATH: `rustc --version` shows `rustc 1.75.x` or later
- `cargo` available on PATH
- `g++` with C++17 support available (for syntax gate)
- Working directory: repo root (`/home/latuser/git/DeepC/.gsd/worktrees/M009-mso5fb`)

---

### TC-01: Rust staticlib builds from workspace root

**What it tests:** Root Cargo workspace resolution; artifact at workspace-level target/ not crate-local target/

**Steps:**
1. Run `cargo build --release` from the repo root
2. Confirm exit code 0
3. Run `ls -lh target/release/libopendefocus_deep.a`

**Expected:**
- Exit 0
- `target/release/libopendefocus_deep.a` exists
- File size ≥ 40 MB (actual: ~54 MB)

**Edge case:** If `Cargo.toml` at root is missing or lacks `[workspace]`, cargo will error with "no Cargo.toml found". The artifact MUST NOT be at `crates/opendefocus-deep/target/release/libopendefocus_deep.a` — CMake points at the workspace path.

---

### TC-02: FFI symbol exported without name mangling

**What it tests:** `#[no_mangle] pub extern "C"` annotation on `opendefocus_deep_render`

**Steps:**
1. Run `nm target/release/libopendefocus_deep.a | grep opendefocus_deep_render`

**Expected:**
- Exactly one line containing `T opendefocus_deep_render`
- No C++ mangled name (no `_ZN...` prefix)

---

### TC-03: DeepSampleData struct layout matches C header

**What it tests:** `#[repr(C)]` field order and types are consistent between Rust and C

**Steps:**
1. Read `crates/opendefocus-deep/src/lib.rs` — confirm fields in order: `sample_counts: *const u32`, `rgba: *const f32`, `depth: *const f32`, `pixel_count: u32`, `total_samples: u32`
2. Read `crates/opendefocus-deep/include/opendefocus_deep.h` — confirm same fields with C types: `const uint32_t* sample_counts`, `const float* rgba`, `const float* depth`, `uint32_t pixel_count`, `uint32_t total_samples`

**Expected:** Field names, types, and order match exactly. `extern "C"` guard present in header.

---

### TC-04: C++ syntax gate passes for all three plugins

**What it tests:** PlanarIop scaffold, existing plugins, mock header coverage

**Steps:**
1. Run `bash scripts/verify-s01-syntax.sh`
2. Observe per-file output lines

**Expected:**
- Exit code 0
- Three lines: "Syntax check passed: DeepCBlur.cpp", "Syntax check passed: DeepCDepthBlur.cpp", "Syntax check passed: DeepCOpenDefocus.cpp"
- Final line: "All syntax checks passed."
- Zero lines containing "error:" from g++

---

### TC-05: Plugin registration and input structure

**What it tests:** PlanarIop scaffold structural requirements

**Steps:**
1. Run `grep -n '"Filter/DeepCOpenDefocus"' src/DeepCOpenDefocus.cpp`
2. Run `grep -n 'minimum_inputs\|maximum_inputs\|inputs(3' src/DeepCOpenDefocus.cpp`
3. Run `grep -n 'input_label' src/DeepCOpenDefocus.cpp`
4. Run `grep -n 'focal_length\|fstop\|focus_distance\|sensor_size' src/DeepCOpenDefocus.cpp`

**Expected:**
- Registration string `"Filter/DeepCOpenDefocus"` present exactly once
- `inputs(3)`, `minimum_inputs()` returning 1, `maximum_inputs()` returning 3 all present
- `input_label` returns `""` for 0, `"holdout"` for 1, `"camera"` for 2
- All four Float_knob registrations present

---

### TC-06: CMake wiring for Rust staticlib

**What it tests:** add_custom_command + opendefocus_deep_lib target + linkage

**Steps:**
1. Run `grep -n 'opendefocus_deep_lib\|OPENDEFOCUS_DEEP_LIB\|cargo build' src/CMakeLists.txt`
2. Run `grep -n 'DeepCOpenDefocus' src/CMakeLists.txt`
3. Run `grep -n 'target_link_libraries.*DeepCOpenDefocus' src/CMakeLists.txt`

**Expected:**
- `OPENDEFOCUS_DEEP_LIB` set to `${CMAKE_SOURCE_DIR}/target/release/libopendefocus_deep.a`
- `add_custom_command` with `cargo build --release` and `WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}`
- `add_custom_target opendefocus_deep_lib ALL DEPENDS ${OPENDEFOCUS_DEEP_LIB}`
- `add_dependencies(DeepCOpenDefocus opendefocus_deep_lib)`
- `target_link_libraries(DeepCOpenDefocus PRIVATE ... dl pthread m)`
- `DeepCOpenDefocus` appears in PLUGINS and FILTER_NODES lists

---

### TC-07: docker-build.sh Rust install

**What it tests:** rustup install step + PATH export before cmake

**Steps:**
1. Run `grep -n 'rustup\|sh.rustup.rs\|\.cargo/bin' docker-build.sh`

**Expected:**
- Line with `curl ... sh.rustup.rs | sh -s -- -y --default-toolchain stable --profile minimal --no-modify-path`
- Line with `export PATH="\$HOME/.cargo/bin:\$PATH"` (escaped dollar sign in bash -c string)
- Both lines appear BEFORE the cmake invocation in the Linux build section
- `which curl || apt-get install -y curl` guard present

---

### TC-08: EUPL-1.2 license attribution

**What it tests:** R057 constraint — opendefocus attribution in THIRD_PARTY_LICENSES.md

**Steps:**
1. Run `grep -A5 'opendefocus' THIRD_PARTY_LICENSES.md | head -20`
2. Run `grep -c 'EUPL-1.2' THIRD_PARTY_LICENSES.md`

**Expected:**
- Section header for opendefocus present
- Author: Gilles Vink
- Source: codeberg.org/gillesvink/opendefocus
- License identifier: EUPL-1.2
- Full EUPL-1.2 license text body present (not just the identifier)
- `grep -c 'EUPL-1.2'` returns ≥ 1

---

### TC-09: Combined S01 gate (smoke test)

**What it tests:** All acceptance criteria in one command

**Steps:**
1. Run the combined gate:
```bash
bash scripts/verify-s01-syntax.sh && \
grep -q '"Filter/DeepCOpenDefocus"' src/DeepCOpenDefocus.cpp && \
grep -q 'opendefocus_deep_lib' src/CMakeLists.txt && \
grep -q 'rustup.rs' docker-build.sh && \
grep -q 'EUPL-1.2' THIRD_PARTY_LICENSES.md && \
ls target/release/libopendefocus_deep.a >/dev/null && \
nm target/release/libopendefocus_deep.a | grep -q opendefocus_deep_render && \
echo 'S01 verification PASSED'
```

**Expected:** Exit 0, final output "S01 verification PASSED"

---

### TC-10: Edge case — cargo build from crate directory produces crate-local artifact

**What it tests:** Awareness that running cargo from the crate directory breaks CMake

**Steps:**
1. Confirm `ls crates/opendefocus-deep/target/` does NOT exist (or if it does, that CMake does NOT reference it)
2. Confirm `grep 'CMAKE_SOURCE_DIR.*target/release' src/CMakeLists.txt`

**Expected:**
- `OPENDEFOCUS_DEEP_LIB` always points at `${CMAKE_SOURCE_DIR}/target/release/libopendefocus_deep.a`
- No CMake reference to `crates/opendefocus-deep/target/`

**Note:** This edge case is documented in KNOWLEDGE.md. If someone runs `cargo build` from the crate dir, the .a appears in the crate-local target — CMake will then not find it at link time and will trigger a rebuild from the correct location.
