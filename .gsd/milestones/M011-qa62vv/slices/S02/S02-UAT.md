# S02: Wire holdout through full stack and integration test — UAT

**Milestone:** M011-qa62vv
**Written:** 2026-03-29T11:56:09.278Z

## UAT: S02 — Wire holdout through full stack and integration test

### Preconditions
- Working directory: repo root of DeepC (M011-qa62vv worktree)
- Rust toolchain: `$HOME/.cargo/bin` on PATH, stable channel installed
- protoc at `/tmp/protoc_dir/bin/protoc` (or re-fetch per docker-build.sh notes)
- Docker available with `nukedockerbuild:16.0-linux` image present

---

### Test 1: Rust unit test — holdout attenuates background samples

**Purpose:** Confirm the Rust kernel correctly attenuates a sample behind the holdout plane.

**Steps:**
1. Run: `PATH=$HOME/.cargo/bin:$PATH PROTOC=/tmp/protoc_dir/bin/protoc cargo test -p opendefocus-deep -- holdout`

**Expected outcome:**
- `test tests::test_holdout_attenuates_background_samples ... ok`
- `test result: ok. 1 passed; 0 failed`
- Exit code 0

---

### Test 2: C++ syntax check — DeepCOpenDefocus.cpp compiles clean

**Purpose:** Confirm the updated C++ file (with `buildHoldoutData()` and holdout dispatch) passes g++ syntax check.

**Steps:**
1. Run: `bash scripts/verify-s01-syntax.sh`

**Expected outcome:**
- All `.cpp` files pass `g++ -std=c++17 -fsyntax-only`
- All `.nk` test files present
- Final line: `All S02 checks passed.`
- Exit code 0

---

### Test 3: .nk integration test structure — depth-selective subjects present

**Purpose:** Confirm `test/test_opendefocus_holdout.nk` encodes the two-depth test structure correctly.

**Steps:**
1. Run: `grep -q 'DeepMerge' test/test_opendefocus_holdout.nk && echo "DeepMerge: OK"`
2. Run: `grep -q 'holdout_depth_selective.exr' test/test_opendefocus_holdout.nk && echo "output EXR: OK"`
3. Run: `grep -q '2.0' test/test_opendefocus_holdout.nk && echo "z=2 subject: OK"`
4. Run: `grep -q '5.0' test/test_opendefocus_holdout.nk && echo "z=5 subject: OK"`

**Expected outcome:** All four greps match (exit 0). The file contains a `DeepMerge` node, a `Write` to `holdout_depth_selective.exr`, a subject at depth 2.0 (green, passes through), and a subject at depth 5.0 (red, attenuated).

---

### Test 4: Docker build — full stack compiles and links

**Purpose:** Confirm the forked Rust crates compile and `DeepCOpenDefocus.so` links against the Nuke 16.0v2 SDK inside the production Docker environment.

**Steps:**
1. Run: `bash docker-build.sh --linux --versions 16.0`
2. Check exit code.
3. Check artifact: `ls -lh install/16.0-linux/DeepC/DeepCOpenDefocus.so`
4. Check zip: `ls -lh release/DeepC-Linux-Nuke16.0.zip`

**Expected outcome:**
- Docker container exits 0
- Log contains: `Compiling opendefocus-kernel`, `Compiling opendefocus-deep`, `Linking CXX shared module DeepCOpenDefocus.so`
- `DeepCOpenDefocus.so` ≥ 20 MB
- `release/DeepC-Linux-Nuke16.0.zip` present

---

### Test 5: Holdout code path is pre-render (structural inspection)

**Purpose:** Confirm the post-defocus scalar multiply pattern is gone and the pre-render dispatch is present.

**Steps:**
1. Run: `grep -c 'computeHoldoutTransmittance' src/DeepCOpenDefocus.cpp`
2. Run: `grep -c 'buildHoldoutData' src/DeepCOpenDefocus.cpp`
3. Run: `grep -c 'opendefocus_deep_render_holdout' src/DeepCOpenDefocus.cpp`

**Expected outcome:**
- `computeHoldoutTransmittance` count = 0 (function removed entirely)
- `buildHoldoutData` count ≥ 2 (declaration + call site)
- `opendefocus_deep_render_holdout` count ≥ 1 (call site present)

---

### Edge Cases

**EC1: No holdout connected** — When input 1 of `DeepCOpenDefocus` is disconnected, `renderStripe()` falls through to `opendefocus_deep_render()` and the `fprintf` log line is NOT emitted. Verify: `grep 'holdout' src/DeepCOpenDefocus.cpp` shows the log line is inside the `if (holdoutOp)` branch.

**EC2: Empty holdout plane** — If the holdout input produces no samples for a pixel, `buildHoldoutData()` encodes `[0.0f, 1.0f, 0.0f, 1.0f]` (T=1.0, fully transmissive). The sample at that pixel passes through unattenuated. This is the correct fallback for pixels not covered by holdout geometry.

**EC3: Qt6 CMake warning on Linux** — The Docker build emits a CMake warning about Qt6 not found. This is expected and benign — Qt6 is only required for the Windows GUI build. The warning does not affect the `.so` output or exit code.

