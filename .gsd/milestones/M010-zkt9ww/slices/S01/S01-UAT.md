# S01: Integration test .nk suite — UAT

**Milestone:** M010-zkt9ww
**Written:** 2026-03-28T16:22:39.713Z

## Preconditions
- Working directory is the DeepC worktree root (`M010-zkt9ww`)
- `g++` with C++17 support is available on PATH
- `bash`/`sh` is available
- No Nuke installation required — these tests are syntax/file-presence only

---

## Test Cases

### TC-1: verify-s01-syntax.sh exits 0 (full gate)
**What it tests:** The canonical stop condition for S01 — all C++ syntax checks and all 7 .nk existence checks pass in a single command.

**Steps:**
1. Run `sh scripts/verify-s01-syntax.sh`

**Expected outcome:**
```
Syntax check passed: DeepCBlur.cpp
Syntax check passed: DeepCDepthBlur.cpp
Syntax check passed: DeepCOpenDefocus.cpp
All syntax checks passed.
test/test_opendefocus.nk exists — OK
test/test_opendefocus_multidepth.nk exists — OK
test/test_opendefocus_holdout.nk exists — OK
test/test_opendefocus_camera.nk exists — OK
test/test_opendefocus_cpu.nk exists — OK
test/test_opendefocus_empty.nk exists — OK
test/test_opendefocus_bokeh.nk exists — OK
All S02 checks passed.
```
Exit code: **0**

---

### TC-2: All 7 .nk files exist
**What it tests:** T02 created all required scripts, none silently missing.

**Steps:**
1. Run `ls test/test_opendefocus.nk test/test_opendefocus_multidepth.nk test/test_opendefocus_holdout.nk test/test_opendefocus_camera.nk test/test_opendefocus_cpu.nk test/test_opendefocus_empty.nk test/test_opendefocus_bokeh.nk`

**Expected outcome:** All 7 paths listed, exit code 0.

---

### TC-3: test/output/.gitignore is present and correct
**What it tests:** EXR outputs will be ignored by git.

**Steps:**
1. Run `cat test/output/.gitignore`

**Expected outcome:** File contains `*.exr` and `*.tmp` entries. Exit code 0.

---

### TC-4: test_opendefocus.nk Write path is test/output/
**What it tests:** The original test script no longer writes to /tmp/.

**Steps:**
1. Run `grep 'test/output/' test/test_opendefocus.nk`

**Expected outcome:** At least one match showing `test/output/test_opendefocus.exr`. Exit code 0.

---

### TC-5: use_gpu Bool_knob is present in DeepCOpenDefocus.cpp
**What it tests:** T01 wired the Bool_knob correctly.

**Steps:**
1. Run `grep 'Bool_knob.*use_gpu' src/DeepCOpenDefocus.cpp`

**Expected outcome:** Matches `Bool_knob(f, &_use_gpu, "use_gpu", "Use GPU")` (or similar). Exit code 0.

---

### TC-6: FFI header declares int use_gpu as 6th parameter
**What it tests:** T01 extended the C header correctly.

**Steps:**
1. Run `grep 'use_gpu' crates/opendefocus-deep/include/opendefocus_deep.h`

**Expected outcome:** Shows `int use_gpu` in the function signature. Exit code 0.

---

### TC-7: Rust lib.rs contains GPU/CPU branch on use_gpu
**What it tests:** T01 wired the CPU-only branch in Rust.

**Steps:**
1. Run `grep -A2 'use_gpu' crates/opendefocus-deep/src/lib.rs | head -10`

**Expected outcome:** Shows `use_gpu: i32` parameter and `if use_gpu != 0` branch. Exit code 0.

---

### TC-8: test_opendefocus_cpu.nk sets use_gpu false
**What it tests:** The CPU-only test script actually disables GPU.

**Steps:**
1. Run `grep 'use_gpu' test/test_opendefocus_cpu.nk`

**Expected outcome:** Shows `use_gpu false`. Exit code 0.

---

### TC-9: test_opendefocus_multidepth.nk uses DeepMerge with operation plus
**What it tests:** Multi-depth script uses additive compositing, not over.

**Steps:**
1. Run `grep 'operation' test/test_opendefocus_multidepth.nk`

**Expected outcome:** Shows `operation plus`. Exit code 0.

---

### TC-10: test_opendefocus_empty.nk has transparent (zero-alpha) DeepConstant
**What it tests:** Empty input edge case uses correct zero-alpha color.

**Steps:**
1. Run `grep 'color' test/test_opendefocus_empty.nk`

**Expected outcome:** Shows `color {0 0 0 0}`. Exit code 0.

---

### TC-11: DeepCOpenDefocus.cpp still passes g++ -fsyntax-only with use_gpu changes
**What it tests:** T01's knob addition did not introduce C++ syntax errors.

**Steps:**
1. Run the syntax check from verify-s01-syntax.sh manually targeting DeepCOpenDefocus.cpp only (or run the full script per TC-1).

**Expected outcome:** `Syntax check passed: DeepCOpenDefocus.cpp`. Exit code 0.

---

### TC-12 (edge case): Adding a new .nk file to verify-s01-syntax.sh loop
**What it tests:** The loop pattern added in T03 is extensible.

**Steps:**
1. Inspect `scripts/verify-s01-syntax.sh` S02 section — confirm it uses a for-loop variable over a filename list, not N separate hardcoded blocks.
2. Conceptually: adding a new filename to the list would extend coverage without copy-paste.

**Expected outcome:** S02 section contains a `for nk_file in ... ; do` loop (or equivalent list-driven loop). 

---

## Edge Cases Not Exercised at This Stage
- **Runtime execution**: None of the .nk scripts can be run without a Nuke installation and a built DeepCOpenDefocus.so. EXR output correctness, GPU/CPU parity, holdout attenuation accuracy, and bokeh disc shape require live Nuke testing.
- **Docker build**: Full Rust + CMake compilation against the real Nuke SDK is not part of S01 verification — this is deferred to the build integration slice.
- **Camera link math**: test_opendefocus_camera.nk exercises the node graph wiring but not the actual CoC computation (requires runtime).

