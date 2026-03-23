# S01: Iop Scaffold, Poly Loader, and Architecture Decision

**Goal:** Node compiles and loads into Nuke; accepts a Deep input; outputs flat black (no scatter yet); .fit file loads and poly evaluates without crash; gather vs scatter architecture decided and stubbed.
**Demo:** `scripts/verify-s01-syntax.sh` passes for `DeepCDefocusPO.cpp`; `grep` contracts on poly load/destroy, `renderStripe`, `File_knob`, and `CMakeLists.txt` entries all pass; `docker-build.sh --linux --versions 16.0` exits 0 with `DeepCDefocusPO.so` in the release zip.

## Must-Haves

- `src/poly.h` vendored (lentil MIT); `poly_system_read`, `poly_system_evaluate`, `poly_system_destroy` present
- `src/deepc_po_math.h` present with `Mat2`, `mat2_inverse`, `Vec2`, `lt_newton_trace` stub, and `coc_radius` inline
- `src/DeepCDefocusPO.cpp` compiles: PlanarIop subclass with `renderStripe()` + `getRequests()`; `_validate()` copies DeepInfo to `info_`; poly load in `_validate(true)` + destroy in destructor; `File_knob`, `Float_knob` (focus_distance, fstop), `Int_knob` (aperture_samples) all wired; two Deep inputs with labels "" and "holdout"; `renderStripe` writes flat black (zeros); `poly_system_read` path exercised in `_validate`; error set on load failure
- `DeepCDefocusPO` in `PLUGINS` list and `FILTER_NODES` list in `src/CMakeLists.txt`
- `scripts/verify-s01-syntax.sh` updated: `DDImage/PlanarIop.h`, `DDImage/ImagePlane.h`, `File_knob` stub added; `DeepCDefocusPO.cpp` in the check loop
- Architecture decision recorded (PlanarIop + gather model) — see DECISIONS.md D021 (already recorded)

## Proof Level

- This slice proves: contract — node compiles, loads a .fit file without crash, and produces flat output wired into the NDK correctly
- Real runtime required: yes — docker build must exit 0 and produce `DeepCDefocusPO.so`
- Human/UAT required: no (deferred to S02 when output is non-zero)

## Observability / Diagnostics

- **Poly load failure:** `_validate(true)` calls `Op::error("DeepCDefocusPO: failed to load poly file: %s", path)` on `poly_system_read` error. Nuke surfaces this as a red error tile and a message in the DAG — inspectable without a debugger.
- **Poly load success:** `_poly_loaded` flag transitions `false → true` after the first successful `poly_system_read`. Agents can confirm state by calling `_validate(true)` via a stub test harness that checks the flag.
- **Failure artifact:** If `poly_system_read` opens a file but reads corrupt data (e.g. truncated), the incomplete `poly_t.term` pointer is freed in `poly_system_destroy` — no leak. Inspect with `valgrind --tool=memcheck` on the standalone syntax-check binary.
- **Build-time inspection:** `scripts/verify-s01-syntax.sh` exits non-zero and prints the failing filename + compiler error to stdout — the first inspectable failure surface agents and CI can check.
- **CI failure shape:** `docker-build.sh` failure prints the cmake/compiler error from inside the container; `DeepCDefocusPO.so` absent from the release zip is the definitive failure signal.
- **Redaction:** No secrets or user-identifying data in any signal above.

## Verification

- `bash scripts/verify-s01-syntax.sh` — exits 0; `DeepCDefocusPO.cpp` passes `g++ -fsyntax-only`
- `grep -q 'poly_system_read' src/DeepCDefocusPO.cpp && grep -q 'poly_system_destroy' src/DeepCDefocusPO.cpp && grep -q 'File_knob' src/DeepCDefocusPO.cpp && grep -q 'renderStripe' src/DeepCDefocusPO.cpp`
- `grep -c 'DeepCDefocusPO' src/CMakeLists.txt | grep -q '[2-9]'` — at least 2 occurrences (PLUGINS + FILTER_NODES)
- `grep -q 'lt_newton_trace' src/deepc_po_math.h && grep -q 'mat2_inverse' src/deepc_po_math.h && grep -q 'coc_radius' src/deepc_po_math.h`
- `grep -q 'poly_system_read' src/poly.h` — poly.h vendored correctly
- `docker-build.sh --linux --versions 16.0` exits 0; `unzip -l release/DeepC-Linux-Nuke16.0.zip | grep -q DeepCDefocusPO.so`
- **Failure-path diagnostic:** `python3 -c "import sys; open('/tmp/bad.fit','wb').write(b'JUNK')"  && (cd /tmp && python3 -c "import subprocess,sys; r=subprocess.run(['bash','-c','grep -q poly_system_read /workspace/.gsd/worktrees/M006/src/poly.h'],capture_output=True); sys.exit(r.returncode)")`  — confirms the error path definition exists (poly_system_read present in poly.h so the failure branch in _validate compiles)

## Tasks

- [x] **T01: Vendor poly.h and write deepc_po_math.h stubs** `est:45m`
  - Why: T02 includes both headers — they must exist before DeepCDefocusPO.cpp is written. poly.h is an exact copy of the lentil MIT header; deepc_po_math.h establishes the math contracts that S02 fills in.
  - Files: `src/poly.h`, `src/deepc_po_math.h`
  - Do: Write `src/poly.h` — verbatim lentil poly.h (poly_system_t, poly_term_t, poly_system_read, poly_system_evaluate, poly_system_destroy, poly_pow, poly_term_evaluate — all defined in-header, `#pragma once`). Write `src/deepc_po_math.h` — `#pragma once`, no `poly.h` include (receives `poly_system_t*` by pointer only), `Mat2`, `mat2_inverse`, `Vec2`, `lt_newton_trace` (stub body returns {0,0}), `coc_radius` inline formula.
  - Verify: `grep -q 'poly_system_read' src/poly.h && grep -q 'lt_newton_trace' src/deepc_po_math.h && grep -q 'mat2_inverse' src/deepc_po_math.h && grep -q 'coc_radius' src/deepc_po_math.h`
  - Done when: Both files exist and grep contracts pass

- [x] **T02: Write DeepCDefocusPO.cpp PlanarIop skeleton** `est:90m`
  - Why: The main plugin file. Establishes PlanarIop as the base class (retiring the Iop vs PlanarIop risk from the roadmap), wires Deep input fetch, loads poly file, outputs flat black. S02 fills in `renderStripe` with scatter math.
  - Files: `src/DeepCDefocusPO.cpp`
  - Do: PlanarIop subclass; constructor sets `inputs(2)`; `test_input` accepts `DeepOp*` on both inputs; `input_label` returns "" and "holdout"; `minimum_inputs()=1`, `maximum_inputs()=2`; member vars `const char* _poly_file`, `float _focus_distance=200.0f`, `float _fstop=2.8f`, `int _aperture_samples=64`, `bool _poly_loaded=false`, `poly_system_t _poly_sys`; `knobs()` wires File_knob, two Float_knobs, one Int_knob; `knob_changed` sets `_reload_poly=true` on poly_file change; `_validate(for_real)` copies DeepInfo to `info_`, calls `poly_system_read` if `_poly_loaded==false || _reload_poly`, calls `error()` on load failure; `getRequests()` calls `input0()->deepRequest()`; `renderStripe()` allocates output ImagePlane to zero; destructor calls `poly_system_destroy` if `_poly_loaded`; `Description` registration with class name "DeepCDefocusPO" and "Deep/DeepCDefocusPO". Include `poly.h` and `deepc_po_math.h`. Do NOT include poly.h from deepc_po_math.h.
  - Verify: `grep -q 'renderStripe' src/DeepCDefocusPO.cpp && grep -q 'poly_system_read' src/DeepCDefocusPO.cpp && grep -q 'poly_system_destroy' src/DeepCDefocusPO.cpp && grep -q 'File_knob' src/DeepCDefocusPO.cpp`
  - Done when: All grep contracts pass and the file is self-consistent C++ (no obvious syntax errors)

- [x] **T03: CMake integration and syntax verifier update** `est:45m`
  - Why: Without CMakeLists.txt changes the plugin never builds. Without verifier updates, the syntax check has no mock stubs for PlanarIop/ImagePlane/File_knob and will fail at the g++ step.
  - Files: `src/CMakeLists.txt`, `scripts/verify-s01-syntax.sh`
  - Do: In `src/CMakeLists.txt`, add `DeepCDefocusPO` to `PLUGINS` list and to `FILTER_NODES` list. In `scripts/verify-s01-syntax.sh`, add mock headers: `DDImage/Iop.h` (with `info_`, `set_out_channels`, `copy_info`, `_request`, `engine` stubs), `DDImage/PlanarIop.h` (with `PlanarIop` class — `renderStripe(ImagePlane&)`, `getRequests(Box, ChannelSet, int, RequestOutput&)` pure virtuals), `DDImage/ImagePlane.h` (with `ImagePlane` class — `bounds()`, `channels()`, writable pixel accessor stubs), and `File_knob` stub in `DDImage/Knobs.h` (`inline Knob* File_knob(Knob_Callback, const char**, const char*, const char* = nullptr)`). Add `DeepCDefocusPO.cpp` to the syntax-check loop in the script.
  - Verify: `grep -c 'DeepCDefocusPO' src/CMakeLists.txt` outputs ≥2; `grep -q 'PlanarIop' scripts/verify-s01-syntax.sh && grep -q 'File_knob' scripts/verify-s01-syntax.sh && grep -q 'DeepCDefocusPO.cpp' scripts/verify-s01-syntax.sh`
  - Done when: CMakeLists has ≥2 DeepCDefocusPO entries; verifier script has new stubs and the new file in its loop

- [x] **T04: Run syntax verifier and docker build** `est:30m`
  - Why: Confirms all three prior tasks integrate correctly — the syntax check is a fast gate; docker build is the integration gate that produces the final `.so`.
  - Files: (verification only — no new files)
  - Do: Run `bash scripts/verify-s01-syntax.sh` from the repo root; fix any compilation errors in `src/DeepCDefocusPO.cpp`, `src/poly.h`, or `src/deepc_po_math.h` that surface. Then run `docker-build.sh --linux --versions 16.0` and confirm exit 0. Confirm `DeepCDefocusPO.so` appears in `release/DeepC-Linux-Nuke16.0.zip`.
  - Verify: `bash scripts/verify-s01-syntax.sh && echo PASS` exits 0; `docker-build.sh --linux --versions 16.0` exits 0; `unzip -l release/DeepC-Linux-Nuke16.0.zip | grep -q DeepCDefocusPO.so`
  - Done when: All three verification commands succeed
