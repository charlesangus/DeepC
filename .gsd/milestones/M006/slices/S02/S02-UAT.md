# S02 UAT Script: PO Scatter Engine — Stochastic Aperture Sampling

**Milestone:** M006 — DeepCDefocusPO  
**Slice:** S02  
**Proof level:** Contract (syntax + grep). Runtime checks require Docker build + Nuke (deferred to S05).

---

## Preconditions

1. Working directory is the repo root (same as `scripts/verify-s01-syntax.sh`).
2. `g++` (C++17 capable) is on `PATH`.
3. `bash` is on `PATH`.
4. `src/deepc_po_math.h`, `src/DeepCDefocusPO.cpp`, `scripts/verify-s01-syntax.sh` are as modified by S02/T01.
5. S01 deliverables are intact: `src/poly.h`, `src/CMakeLists.txt`, `scripts/verify-s01-syntax.sh` base version.

---

## Test Cases

### TC-01: Full syntax check (all three source files)

**Purpose:** Confirms the scatter engine compiles without errors under mock DDImage headers.

**Steps:**
1. Run: `bash scripts/verify-s01-syntax.sh`

**Expected:**
```
Syntax check passed: DeepCBlur.cpp
Syntax check passed: DeepCDepthBlur.cpp
Syntax check passed: DeepCDefocusPO.cpp
Checking S02 contracts...
S02 contracts: all pass.
All syntax checks passed.
```
Exit code: 0.

**Failure signals:**
- Any "error:" line from g++ → C++ syntax error introduced by S02. Check mock headers first (see KNOWLEDGE.md: "Mock DDImage headers in syntax scripts").
- "FAIL:" line in S02 contracts → a grep contract is missing. Check whether the corresponding code was accidentally removed.

---

### TC-02: Halton and map_to_disk helpers present in deepc_po_math.h

**Purpose:** Confirms the aperture disk sampling helpers were added to the math header, not inlined only in the .cpp.

**Steps:**
1. `grep -q 'halton' src/deepc_po_math.h && echo PASS || echo FAIL`
2. `grep -q 'map_to_disk' src/deepc_po_math.h && echo PASS || echo FAIL`
3. `grep -q 'inline float halton' src/deepc_po_math.h && echo PASS || echo FAIL`
4. `grep -q 'inline void map_to_disk' src/deepc_po_math.h && echo PASS || echo FAIL`

**Expected:** All four return PASS.

**Failure signals:** Functions defined only in the .cpp — they must live in the shared header for future callers (e.g. S03 may reuse `halton`).

---

### TC-03: Newton iteration body in lt_newton_trace

**Purpose:** Confirms the Newton stub was replaced with a real implementation.

**Steps:**
1. `grep -q 'poly_system_evaluate' src/deepc_po_math.h && echo PASS || echo FAIL`
2. `grep -q 'mat2_inverse' src/deepc_po_math.h && echo PASS || echo FAIL`
3. `grep -q 'MAX_ITER' src/deepc_po_math.h && echo PASS || echo FAIL`
4. `grep -q 'TOL' src/deepc_po_math.h && echo PASS || echo FAIL`
5. `grep -q '1e-12' src/deepc_po_math.h && echo PASS || echo FAIL`

**Expected:** All five return PASS.

**Failure signals:**
- `poly_system_evaluate` missing → Newton body not calling the polynomial.
- `1e-12` missing → singular Jacobian guard absent; renders may NaN on edge-case aperture samples.

---

### TC-04: Three-wavelength tracing (chromatic aberration)

**Purpose:** Confirms R/G/B channels are traced at distinct wavelengths in the scatter loop.

**Steps:**
1. `grep -q '0\.45f' src/DeepCDefocusPO.cpp && echo PASS || echo FAIL`
2. `grep -q '0\.55f' src/DeepCDefocusPO.cpp && echo PASS || echo FAIL`
3. `grep -q '0\.65f' src/DeepCDefocusPO.cpp && echo PASS || echo FAIL`

**Expected:** All three return PASS.

**Failure signals:** Missing wavelength → single-wavelength tracing, no chromatic aberration in output.

---

### TC-05: Deep input fetch present in renderStripe

**Purpose:** Confirms `renderStripe` fetches the Deep input (not stuck in zeroing-only mode).

**Steps:**
1. `grep -q 'deepEngine' src/DeepCDefocusPO.cpp && echo PASS || echo FAIL`
2. `grep -q 'deepPlane' src/DeepCDefocusPO.cpp && echo PASS || echo FAIL`
3. `grep -q 'getPixel' src/DeepCDefocusPO.cpp && echo PASS || echo FAIL`

**Expected:** All three return PASS.

---

### TC-06: Newton trace called in scatter loop

**Purpose:** Confirms `lt_newton_trace` is called inside renderStripe (not just defined in the math header).

**Steps:**
1. `grep -q 'lt_newton_trace' src/DeepCDefocusPO.cpp && echo PASS || echo FAIL`
2. `grep -c 'lt_newton_trace' src/DeepCDefocusPO.cpp` — expected count ≥ 1.

**Expected:** Both pass.

---

### TC-07: Guard and abort surfaces present

**Purpose:** Confirms the fast-path guard and abort poll are in place for production safety.

**Steps:**
1. `grep -q '_poly_loaded' src/DeepCDefocusPO.cpp && echo PASS || echo FAIL`
2. `grep -q 'aborted' src/DeepCDefocusPO.cpp && echo PASS || echo FAIL`
3. `grep -c 'aborted\|_poly_loaded\|deepEngine' src/DeepCDefocusPO.cpp` — expected ≥ 3.

**Expected:** All pass; count ≥ 3.

**Failure signals:**
- `_poly_loaded` missing → no fast-path when no poly loaded; `deepEngine` called on invalid state.
- `aborted` missing → Nuke Cancel button has no effect during scatter; UI freeze on large renders.

---

### TC-08: No leftover stub markers

**Purpose:** Confirms all S02-targeted stubs were replaced and no TODO/STUB comments remain.

**Steps:**
1. `grep -c 'TODO\|STUB\|S02: replace' src/DeepCDefocusPO.cpp` — expected 0.
2. `grep -c 'TODO\|STUB\|S02: replace' src/deepc_po_math.h` — expected 0.

**Expected:** Both return 0.

**Failure signals:** Non-zero count → stub was not replaced; `renderStripe` or `lt_newton_trace` is still a no-op.

---

### TC-09: S02 grep contracts in verify script

**Purpose:** Confirms the verify script was updated with S02 contracts (so future regressions are caught automatically).

**Steps:**
1. `grep -q 'S02 grep contracts' scripts/verify-s01-syntax.sh && echo PASS || echo FAIL`
2. `grep -q 'halton.*deepc_po_math' scripts/verify-s01-syntax.sh && echo PASS || echo FAIL`
3. `grep -q '0\\\\\.45f' scripts/verify-s01-syntax.sh && echo PASS || echo FAIL`

**Expected:** All pass.

---

### TC-10: Format mock stubs updated in verify script

**Purpose:** Confirms `Format::width()` and `Format::height()` were added to the mock headers (required now that renderStripe calls these methods).

**Steps:**
1. `grep -q 'width()' scripts/verify-s01-syntax.sh && echo PASS || echo FAIL`
2. `grep -q 'height()' scripts/verify-s01-syntax.sh && echo PASS || echo FAIL`

**Expected:** Both pass.

**Failure signals:** Missing stubs → syntax check fails as soon as renderStripe calls `fmt.width()`.

---

### TC-11: Accumulation write pattern (scatter splat)

**Purpose:** Confirms the scatter loop uses `+=` accumulation into the output buffer (not `=` assignment, which would overwrite contributions).

**Steps:**
1. `grep -q 'writableAt.*+=' src/DeepCDefocusPO.cpp && echo PASS || echo FAIL`

**Expected:** PASS.

**Failure signals:** `=` without `+` → only the last aperture sample contributes per pixel; bokeh appears at wrong intensity.

---

### TC-12: Alpha uses G-channel landing position

**Purpose:** Confirms alpha channel is splatted at the green (0.55μm) wavelength landing position, not R or B.

**Steps:**
1. Check that the alpha splat block appears after the three-channel loop and references `landing_x[1]` / `landing_y[1]` (index 1 = G):
   ```bash
   grep -A 10 'Alpha.*G-channel\|G-channel.*Alpha' src/DeepCDefocusPO.cpp
   ```

**Expected:** Code shows alpha write using `landing_x[1]` or `landing_y[1]` (or equivalent).

**Rationale:** Using G ensures luminance-correct alpha placement; chromatic aberration shifts R and B, so alpha following the G disc gives visually correct coverage.

---

### TC-13: Out-of-stripe discard documented

**Purpose:** Confirms the known stripe-seam limitation is documented in the code (important for future maintainers and S04 single-stripe mitigation work).

**Steps:**
1. `grep -q 'seam\|stripe\|outside.*stripe\|out-of-stripe' src/DeepCDefocusPO.cpp && echo PASS || echo FAIL`

**Expected:** PASS.

---

## Edge Cases

### EC-01: No poly file loaded

- **Condition:** `_poly_loaded == false` (knob left empty or `poly_system_read` failed).
- **Expected behaviour:** `renderStripe` zeros the stripe immediately and returns. Output is uniformly black.
- **Diagnostic:** In Nuke, the node output is black; no crash, no error node (unless the file path was non-empty and `poly_system_read` returned non-zero, in which case a red error node appears).
- **Verify in code:** `grep -q '_poly_loaded' src/DeepCDefocusPO.cpp`

### EC-02: No Deep input connected

- **Condition:** `Op::input(0) == nullptr` or not a DeepOp.
- **Expected behaviour:** `_validate` sets empty format; `renderStripe` skips via `!input0()` guard. Output is zero-size or black.
- **Verify in code:** `grep -q '!input0()' src/DeepCDefocusPO.cpp`

### EC-03: Deep input returns false from deepEngine

- **Condition:** Upstream Deep node errors during `renderStripe`.
- **Expected behaviour:** `renderStripe` returns immediately; stripe stays zeroed. The upstream node's error surfaces through Nuke's normal error propagation.
- **Verify in code:** `grep -q 'if.*deepEngine' src/DeepCDefocusPO.cpp`

### EC-04: Zero-alpha deep samples

- **Condition:** A deep sample has `alpha < 1e-6f`.
- **Expected behaviour:** Skipped entirely in the scatter loop (no contribution, no NaN).
- **Verify in code:** `grep -q 'alpha.*1e-6f' src/DeepCDefocusPO.cpp`

### EC-05: Newton non-convergence / singular Jacobian

- **Condition:** Aperture sample at aperture disk rim, or polynomial ill-conditioned.
- **Expected behaviour:** Best estimate returned; no NaN/inf; bokeh disc may be slightly misplaced but render completes cleanly.
- **Verify in code:** `grep -q '1e-12' src/deepc_po_math.h`

### EC-06: aperture_samples = 1

- **Condition:** Artist sets N = 1.
- **Expected behaviour:** Single aperture sample per deep sample. Output is noisy (single-sample Monte Carlo) but not incorrect. No divide-by-zero (N=1 is handled by `max(_aperture_samples, 1)`).
- **Verify in code:** `grep -q 'max.*_aperture_samples.*1\|max(1.*aperture' src/DeepCDefocusPO.cpp`

### EC-07: User cancels during scatter

- **Condition:** User clicks Cancel in Nuke during a renderStripe call.
- **Expected behaviour:** `Op::aborted()` returns true at the next pixel poll; renderStripe returns immediately with a partially-accumulated buffer. Output is partial (some stripes complete, current stripe partial). Normal Nuke cancel behaviour.
- **Verify in code:** `grep -q 'aborted' src/DeepCDefocusPO.cpp`

---

## Failure Signals

| Signal | Likely cause |
|--------|--------------|
| `bash scripts/verify-s01-syntax.sh` exits non-zero | C++ syntax error in DeepCDefocusPO.cpp or deepc_po_math.h |
| "FAIL: halton missing" | `halton()` not added to deepc_po_math.h |
| "FAIL: R wavelength 0.45 missing" | Wavelength constants not present in scatter loop |
| "FAIL: deepEngine call missing" | Deep input fetch not implemented — scatter loop not present |
| "FAIL: lt_newton_trace not called" | Newton trace not called from renderStripe |
| "FAIL: leftover S02 stub marker" | Old `renderStripe` zeroing loop not replaced |
| Output uniformly black in Nuke | `_poly_loaded == false` or no Deep input; check poly file path and input connection |
| Red error node in Nuke DAG | `poly_system_read` failed; check that the .fit file path is correct and the file is a valid lentil binary |
| Dark seams at stripe boundaries | Expected limitation; large bokeh disc contributions discarded outside stripe |
| No chromatic fringing on bokeh | Three-wavelength tracing not working; check lambdas[] values and per-channel loop |

---

## Runtime UAT (Deferred to S05)

The following checks require a Docker build and a Nuke session with a real lentil .fit file. They are the definitive pass criteria for the milestone definition of done and are documented here for completeness:

1. **Non-zero output:** Connect a Deep image to input 0. Set a valid poly_file. Render. Output must have non-zero pixels in regions where the Deep input has samples.
2. **Visible bokeh:** Connect an out-of-focus highlight in the Deep input. Output must show a visible bokeh disc, not a point.
3. **Chromatic aberration:** On a bright white out-of-focus highlight, bokeh disc must show colour fringing (R disc shifted relative to B disc). Visible as coloured edges on the bokeh shape.
4. **Aperture samples control:** Setting N=1 produces noisy output; setting N=256 produces smooth output. Both complete without crash.
5. **No poly loaded → black output:** Clear the poly_file knob. Output must be uniformly black with no error node.
6. **Wrong poly path → red error node:** Set poly_file to a non-existent path. A red error node must appear in the DAG.
7. **Cancel during render:** Cancel a render mid-progress. Nuke must not hang; output is partial but no crash.
