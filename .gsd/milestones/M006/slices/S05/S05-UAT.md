# S05 UAT: Build Integration, Menu Registration, and Final Verification

**Milestone:** M006 — DeepCDefocusPO  
**Slice:** S05  
**UAT Level:** Structural (workspace-verifiable) + CI/Runtime (requires docker + Nuke)  
**Prepared:** 2026-03-23

---

## Overview

This UAT covers two tiers:

- **Tier 1 — Structural (no Docker, no Nuke required):** All checks verifiable in the workspace now. These form the S05 gate and were all confirmed passing before this UAT was written.
- **Tier 2 — Runtime/CI (requires Docker + Nuke 16+):** These are deferred to the CI pipeline and Nuke artist session. They represent the milestone Definition of Done runtime gates.

---

## Preconditions

### For Tier 1 (Structural)
- Working directory: `/workspace/.gsd/worktrees/M006` (or the root DeepC workspace after merge)
- `g++` (C++17 capable) available on PATH
- `bash` available
- `scripts/verify-s01-syntax.sh` present and executable

### For Tier 2 (Runtime/CI)
- Docker daemon running with access to NukeDockerBuild images
- `docker-build.sh` at repo root
- Nuke 16.0+ with a valid license
- A real lentil `.fit` polynomial file (e.g. `Canon_50mm_f1.4.fit` from a lentil gencode run)
- A test Nuke comp with a Deep image input (e.g. a rendered EXR with deep samples)

---

## Tier 1: Structural Tests (Workspace-Verifiable)

### T1-1: Full verify script passes

**Purpose:** Confirm all S01–S05 structural contracts pass as a single gate.

**Steps:**
1. From the repo root: `bash scripts/verify-s01-syntax.sh`

**Expected outcome:**
```
Syntax check passed: DeepCBlur.cpp
Syntax check passed: DeepCDepthBlur.cpp
Syntax check passed: DeepCDefocusPO.cpp
Checking S02 contracts...
S02 contracts: all pass.
Checking S03 contracts...
S03 contracts: all pass.
Checking S04 contracts...
S04 contracts: all pass.
Checking S05 contracts...
S05 contracts: all pass.
All syntax checks passed.
```
Exit code: 0.

**Failure modes:**
- Exit 1 + `FAIL: stale S01 skeleton comment still present` → the comment edit was reverted; re-apply the edit to lines 13–14 of `src/DeepCDefocusPO.cpp`
- Exit 1 + `FAIL: DeepCDefocusPO not in exactly 2 CMakeLists.txt locations` → CMake registration was lost; check lines 16 and 53 of `src/CMakeLists.txt`
- Exit 1 + `FAIL: lentil/poly.h not in THIRD_PARTY_LICENSES.md` → license entry missing or file was truncated; check `THIRD_PARTY_LICENSES.md` for the `## lentil / poly.h` section
- Exit 1 + syntax error in DeepCDefocusPO.cpp → a source edit introduced a compile error; run `g++ -std=c++17 -fsyntax-only` manually to see the line

---

### T1-2: No stale S01/S02 comment lines in source

**Purpose:** Confirm the inaccurate pre-S02 scaffolding comment was removed.

**Steps:**
1. `grep -n 'S01 state: skeleton only' src/DeepCDefocusPO.cpp`; expect: no output, exit 1
2. `grep -n 'S02 replaces the renderStripe' src/DeepCDefocusPO.cpp`; expect: no output, exit 1

**Expected outcome:**  
Both greps return no output and exit 1 (pattern not found = pass).

**Edge case:** If `S01 state:` was added in a comment elsewhere (e.g. a test file or this UAT itself), the contract may incorrectly trigger. The contract checks only `src/DeepCDefocusPO.cpp`.

---

### T1-3: CMake PLUGINS + FILTER_NODES registration

**Purpose:** Confirm `DeepCDefocusPO` is registered in exactly two CMakeLists.txt locations.

**Steps:**
1. `grep -n 'DeepCDefocusPO' src/CMakeLists.txt`

**Expected outcome:**
```
16:    DeepCDefocusPO
53:set(FILTER_NODES DeepCBlur DeepCBlur2 DeepThinner DeepCDepthBlur DeepCDefocusPO)
```
Two matches: one in the PLUGINS list (line 16), one in the FILTER_NODES list (line 53).

2. `grep -q 'FILTER_NODES.*DeepCDefocusPO' src/CMakeLists.txt && echo PASS || echo FAIL`

**Expected outcome:** `PASS`

**Edge case:** If DeepCDefocusPO appears a third time (e.g. in a comment), the count contract (`grep -c` → 2) will fail. Ensure no other CMakeLists.txt references exist.

---

### T1-4: Op::Description registration

**Purpose:** Confirm the Nuke plugin factory registration is present in source.

**Steps:**
1. `grep -n 'Op::Description' src/DeepCDefocusPO.cpp`

**Expected outcome:**
```
<line>: const Op::Description DeepCDefocusPO::d(::CLASS, "Deep/DeepCDefocusPO", build);
```
One match. Menu path `"Deep/DeepCDefocusPO"` places the node in Nuke's Deep menu category.

---

### T1-5: lentil/poly.h attribution in THIRD_PARTY_LICENSES.md

**Purpose:** Confirm the vendored MIT-licensed poly.h has proper attribution.

**Steps:**
1. `grep -A10 'lentil' THIRD_PARTY_LICENSES.md`

**Expected outcome:**
```
## lentil / poly.h

- **Project:** lentil — polynomial-optics lens simulation
- **File vendored:** `src/poly.h`
- **Author:** Johannes Hanika (hanatos)
- **Source:** https://github.com/hanatos/lentil
- **License:** MIT
...
```

2. `grep -q 'hanatos' THIRD_PARTY_LICENSES.md && echo PASS || echo FAIL` → `PASS`

---

### T1-6: Icon file present and non-empty

**Purpose:** Confirm `icons/DeepCDefocusPO.png` exists and is a valid (non-zero) PNG.

**Steps:**
1. `test -f icons/DeepCDefocusPO.png && echo PASS || echo FAIL` → `PASS`
2. `test -s icons/DeepCDefocusPO.png && echo PASS || echo FAIL` → `PASS` (97 bytes, same as DeepCBlur.png)
3. `file icons/DeepCDefocusPO.png`

**Expected outcome:**
```
icons/DeepCDefocusPO.png: PNG image data, <dimensions>, 8-bit/color RGB, non-interlaced
```
(Same metadata as `icons/DeepCBlur.png` — placeholder copy.)

**Edge case:** If `file` is unavailable, `xxd icons/DeepCDefocusPO.png | head -1` should show `89 50 4e 47` (PNG magic bytes).

---

### T1-7: CMake syntax validation (no Docker required)

**Purpose:** Confirm CMakeLists.txt parses correctly (beyond `verify-s01-syntax.sh` which checks C++ syntax only).

**Steps:**
1. `cmake -S . -B /tmp/deepc-cmake-check -DNuke_ROOT=/nonexistent 2>&1 | head -20`

**Expected outcome:**  
CMake outputs configuration progress until `find_package(Nuke REQUIRED)` fails with a "Nuke not found" error. No CMake syntax errors appear before that failure. The configure step should list `DeepCDefocusPO` in the PLUGINS variable.

**Note:** This test is advisory — the verify script is the primary gate. But it confirms the CMakeLists.txt syntax is clean without running a docker build.

---

## Tier 2: CI / Runtime Tests (Docker + Nuke Required)

These tests cannot be run in the workspace (no Docker daemon, no Nuke license). They represent the milestone Definition of Done runtime gates. Run these in CI and during artist UAT.

---

### T2-1: Docker build exits 0

**Purpose:** Confirm DeepCDefocusPO.so is compiled and included in the release zip by the official build script.

**Preconditions:** Docker daemon running; NukeDockerBuild images available.

**Steps:**
1. `bash docker-build.sh --linux --versions 16.0`

**Expected outcome:** Exit 0. No compiler errors. Output includes:
```
[100%] Built target DeepCDefocusPO
...
Creating release/DeepC-Linux-Nuke16.0.zip
```

2. `unzip -l release/DeepC-Linux-Nuke16.0.zip | grep DeepCDefocusPO`

**Expected outcome:**
```
  DeepCDefocusPO.so
  icons/DeepCDefocusPO.png
```
Both the shared object and the icon appear in the zip.

**Failure mode:** Linker error for `poly_system_read` / `poly_system_evaluate` → poly.h ODR violation; confirm poly.h is included in exactly one TU (DeepCDefocusPO.cpp).

---

### T2-2: Node loads in Nuke without crash

**Purpose:** Confirm Nuke can create the node from the menu without crash or error.

**Preconditions:** DeepC plugin directory installed from release zip; Nuke 16.0+ running.

**Steps:**
1. In Nuke: `Tab` → type `DeepCDefocusPO` → press Enter to create the node.

**Expected outcome:**  
Node appears in the Node Graph. Properties panel shows:
- `poly_file` — empty file path (File_knob)
- `focal_length (mm)` — 50.0 (Float_knob, range 1–1000)
- `focus_distance (mm)` — 200.0 (Float_knob)
- `f-stop` — 2.8 (Float_knob)
- `aperture samples` — 64 (Int_knob)
- A divider between focal_length and focus_distance groups
- Two input pipes: one unlabelled (main Deep), one labelled "holdout" (optional Deep)

**Failure mode:** "No such node: DeepCDefocusPO" → Op::Description not registered; check that DeepCDefocusPO.so is in Nuke's plugin path.

---

### T2-3: .fit file loads without error

**Purpose:** Confirm `poly_system_read` handles a real lentil .fit file without crash or Nuke error().

**Preconditions:** A real lentil `.fit` file available (e.g. `Canon_50mm_f1.4.fit`).

**Steps:**
1. In the DeepCDefocusPO properties panel, set `poly_file` to the path of the `.fit` file.
2. Press Enter or Tab to trigger `_validate(for_real)`.

**Expected outcome:**  
No error dialog in Nuke. The node does not turn red/error. The Nuke Script Editor shows no `Error: DeepCDefocusPO: ...` messages.

**Edge case:** Test with a nonexistent path → node should turn red with a clear error message ("Cannot open poly file: ..."). Do NOT crash Nuke.

**Edge case:** Test with a valid path followed by clearing it → node should degrade gracefully (output black or error) without crash.

---

### T2-4: Flat non-zero output with bokeh visible

**Purpose:** Confirm the PO scatter engine produces physically correct bokeh output from a real Deep input.

**Preconditions:** A test Nuke comp with a Deep EXR input (rendered with depth information); a real `.fit` file loaded as above.

**Steps:**
1. Connect a Deep image to the main (unlabelled) input of DeepCDefocusPO.
2. Set focus_distance to a value that puts most of the scene out of focus (e.g. 100mm if scene is at 1000mm).
3. Set fstop to 1.4 (wide aperture, large bokeh).
4. Set aperture_samples to 256.
5. Connect a Viewer to the output of DeepCDefocusPO and render.

**Expected outcome:**
- Output is a flat (non-Deep) 2D RGBA image.
- Out-of-focus regions show circular or lens-shaped bokeh highlights.
- Bokeh size is physically proportional to `focal_length / fstop` and to depth distance from focus_distance.
- The image is not all black (confirms scatter engine is producing contributions).
- No NaN or Inf pixels in the output.

**Edge case:** Set aperture_samples = 1 → output should be noisy but non-zero.

**Edge case:** Connect a single-sample Deep input with known depth → bokeh disc should appear at a predictable location in the output.

---

### T2-5: Chromatic aberration visible on out-of-focus highlights

**Purpose:** Confirm per-channel wavelength tracing (R=0.45μm, G=0.55μm, B=0.65μm) produces colour fringing.

**Preconditions:** Same setup as T2-4; use a scene with bright, out-of-focus point lights or specular highlights.

**Steps:**
1. Render the defocused output as in T2-4.
2. Zoom into a bright bokeh disc at the edge of the frame.

**Expected outcome:**  
Visible RGB colour fringing at the edge of bokeh discs — not purely white/monochrome halos. The fringing should match the characteristic pattern of the lens represented by the `.fit` file.

**Note:** For a lens with minimal chromatic aberration, fringing may be subtle. Use a wide-angle or zoom lens `.fit` file for more visible fringing.

---

### T2-6: Holdout stencils defocused output depth-correctly

**Purpose:** Confirm the depth-aware holdout evaluates at the output pixel and prevents double-defocus.

**Preconditions:** Two Deep inputs: (A) a background Deep scene with out-of-focus elements; (B) a foreground Deep roto/object at a known depth closer than the focus plane.

**Steps:**
1. Connect input A to the main (unlabelled) input of DeepCDefocusPO.
2. Connect input B ("holdout") to the holdout input pipe.
3. Set focus_distance to the depth of background elements in A (so A is in focus, holdout B is in front).
4. Render the output.

**Expected outcome:**
- Regions covered by the holdout (B) are correctly masked: contributions from background (A) behind the holdout depth are attenuated.
- The holdout mask is **sharp** — the boundary of B does not appear blurred or defocused in the output.
- Holdout geometry (B's colour) does NOT appear in the output — the holdout contributes transmittance only, no colour.
- No "double-defocus" artefact: if B was pre-defocused (e.g. a Blur was applied to B before the holdout input), the output should still show a sharp holdout boundary (not a blurred version of the pre-blurred B mask).

**Edge case:** Disconnect the holdout input → output should match a defocus of only the main input, with no masking.

**Edge case:** Connect a fully-opaque holdout (alpha=1.0 everywhere) → output should be completely black (holdout fully blocks all contributions).

**Edge case:** Connect a holdout at a depth deeper than all main input samples → holdout should have no effect (transmittance = 1.0 everywhere, because no holdout samples are in front of any main sample).

---

### T2-7: Node visible in FILTER_NODES menu category

**Purpose:** Confirm `DeepCDefocusPO` appears in the correct Nuke menu category.

**Steps:**
1. In Nuke: `Filters` → `DeepC` → look for `DeepCDefocusPO`.

**Expected outcome:**  
`DeepCDefocusPO` appears in the DeepC submenu under Filters (FILTER_NODES category). An icon (even if placeholder) appears to its left.

**Alternative check:**
```python
# In Nuke Script Editor:
nuke.createNode('DeepCDefocusPO')
```
Should succeed without error.

---

## Edge Cases and Regression Guards

| Scenario | Expected behaviour |
|----------|-------------------|
| poly_file path points to a corrupt/truncated .fit file | `_validate(for_real)` calls `Op::error()` with a clear message; node turns red; no crash |
| aperture_samples = 0 | Clamped to `max(_aperture_samples, 1)` = 1 in renderStripe; produces valid (noisy) output, not a division by zero |
| Focus distance = 0 | CoC formula divides by depth (Z); Z=0 input samples should either be culled by the near guard or produce a very large CoC and wide scatter (not a crash) |
| Input Deep image has no samples at a given pixel | deepEngine returns an empty DeepPlane pixel; scatter loop iterates zero times; output pixel remains zero (correct: no contribution) |
| Holdout connected but has no samples at a pixel | transmittance_at returns 1.0f (no holdout samples → full transmittance → no masking) |
| Render with tile size smaller than bokeh radius | Stripe-boundary seam artefact: dark seam at stripe edges when bokeh discs cross stripe boundaries. This is a known limitation (documented in S02 summary); not a regression. |

---

## Sign-Off Criteria

S05 is complete when:

- [x] `bash scripts/verify-s01-syntax.sh` exits 0 (all S01–S05 contracts pass) — **confirmed**
- [x] All six T1 spot checks pass — **confirmed**
- [ ] `docker-build.sh --linux --versions 16.0` exits 0 with `DeepCDefocusPO.so` in zip — **CI pending**
- [ ] T2-2 through T2-7 pass in Nuke session — **UAT pending**

The milestone Definition of Done (M006-ROADMAP.md) requires all of the above including the CI and UAT gates.
