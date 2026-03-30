# S02: Full-image PlanarIop cache + quality knob — UAT

**Milestone:** M012-kho2ui
**Written:** 2026-03-30T23:31:22.891Z

## Preconditions

- Working directory: repo root (this worktree)
- `g++` with C++17 support available (`g++ --version`)
- `src/DeepCOpenDefocus.cpp`, `test/test_opendefocus_256.nk`, `scripts/verify-s01-syntax.sh` present

---

## Test Cases

### TC-01: Syntax verification script exits 0

**Purpose:** Confirm DeepCOpenDefocus.cpp compiles cleanly with mock headers (no syntax errors from cache additions).

**Steps:**
1. Run `bash scripts/verify-s01-syntax.sh`
2. Observe output

**Expected outcome:**
- Lines: `Syntax check passed: DeepCOpenDefocus.cpp`, `All syntax checks passed.`, `All S02 checks passed.`
- Exit code: 0

**Edge case:** If mock headers are missing new types introduced in cache code (e.g. `<mutex>`), the script fails. `std::mutex` comes from the standard library, not mock headers — this should always pass.

---

### TC-02: Cache members present in source

**Purpose:** Confirm all required cache fields are declared in the class.

**Steps:**
1. `grep -q '_cacheValid' src/DeepCOpenDefocus.cpp && echo PASS || echo FAIL`
2. `grep -q '_cacheMutex' src/DeepCOpenDefocus.cpp && echo PASS || echo FAIL`
3. `grep -q '_cachedRGBA' src/DeepCOpenDefocus.cpp && echo PASS || echo FAIL`
4. `grep -q '_cacheFL' src/DeepCOpenDefocus.cpp && echo PASS || echo FAIL`

**Expected outcome:** All four commands print `PASS`.

---

### TC-03: fullBox (not stripe bounds) used for deepEngine

**Purpose:** Confirm non-local convolution fetches full-image deep data.

**Steps:**
1. `grep -q 'info_.box()' src/DeepCOpenDefocus.cpp && echo PASS || echo FAIL`
2. `grep -c 'deepEngine(fullBox' src/DeepCOpenDefocus.cpp` — should be ≥ 2 (normal + holdout branches)

**Expected outcome:** TC-03.1 prints `PASS`; TC-03.2 prints `2` or greater.

---

### TC-04: Exactly one Enumeration_knob (quality knob not duplicated)

**Purpose:** Confirm quality knob appears exactly once (no accidental duplication from cache work).

**Steps:**
1. `grep -c 'Enumeration_knob' src/DeepCOpenDefocus.cpp`

**Expected outcome:** Output is `1`.

---

### TC-05: SLA test fixture exists and is well-formed

**Purpose:** Confirm `test/test_opendefocus_256.nk` is present and has the required format declaration.

**Steps:**
1. `test -f test/test_opendefocus_256.nk && echo PASS || echo FAIL`
2. `grep -q '256 256' test/test_opendefocus_256.nk && echo PASS || echo FAIL`
3. `grep -q 'first_frame 1' test/test_opendefocus_256.nk && echo PASS || echo FAIL`

**Expected outcome:** All three print `PASS`.

---

### TC-06: Verify script covers all 8 .nk files including the new SLA test

**Purpose:** Confirm `verify-s01-syntax.sh` checks `test_opendefocus_256.nk` in its file-existence loop.

**Steps:**
1. `grep -q 'test_opendefocus_256.nk' scripts/verify-s01-syntax.sh && echo PASS || echo FAIL`
2. Run `bash scripts/verify-s01-syntax.sh` and confirm output includes `test/test_opendefocus_256.nk exists — OK`

**Expected outcome:** TC-06.1 prints `PASS`; TC-06.2 shows the `exists — OK` line.

---

### TC-07: _validate invalidates cache

**Purpose:** Confirm cache is reset on every revalidate.

**Steps:**
1. `grep -n '_cacheValid = false' src/DeepCOpenDefocus.cpp`

**Expected outcome:** At least one match inside `_validate()` function body.

---

### TC-08: Cache-miss path uses correct index arithmetic (fullBox-relative)

**Purpose:** Confirm stripe copy uses `(y - fullBox.y()) * fullW` not absolute y.

**Steps:**
1. `grep -q 'fullBox.y()' src/DeepCOpenDefocus.cpp && echo PASS || echo FAIL`
2. `grep -q 'fullBox.x()' src/DeepCOpenDefocus.cpp && echo PASS || echo FAIL`

**Expected outcome:** Both print `PASS`.

---

### TC-09 (live Nuke, optional): 256×256 single-layer renders under 10s

**Purpose:** Confirm the SLA target is met in a real Nuke environment.

**Precondition:** Nuke 16.0+ installed; `DeepCOpenDefocus.so` built and in `NUKE_PATH`.

**Steps:**
1. `time nuke -x -m 1 test/test_opendefocus_256.nk`
2. Note wall-clock time

**Expected outcome:** Completes in < 10 seconds. With cache active, any second render of the same frame (same parameters) completes in < 1s (stripe copies only, no FFI call).

---

### TC-10 (live Nuke, optional): Cache prevents redundant Rust FFI calls

**Purpose:** Confirm that repeated stripe calls within one frame do not re-enter the Rust FFI.

**Precondition:** As TC-09; stderr logging enabled (existing `fprintf(stderr, ...)` in renderStripe).

**Steps:**
1. Render a frame with a 256×256 format (multiple stripes).
2. Inspect stderr for FFI call log lines.

**Expected outcome:** FFI call log appears exactly **once** per frame per unique parameter set, regardless of how many stripes Nuke issues.

