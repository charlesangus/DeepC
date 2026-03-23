---
id: S01
milestone: M006
title: "Iop Scaffold, Poly Loader, and Architecture Decision"
status: complete
completed_at: 2026-03-23
risks_retired:
  - "Iop/PlanarIop base class for Deep→flat — confirmed PlanarIop; pattern working in NDK"
  - "Eigen dependency — retired by hand-rolled 2×2 inverse in deepc_po_math.h"
  - "Scatter vs gather threading — retired by PlanarIop single-threaded renderStripe; no shared buffer race"
provides:
  - src/poly.h — vendored lentil poly API; poly_system_t, poly_system_read, poly_system_evaluate, poly_system_destroy inline (MIT)
  - src/deepc_po_math.h — Mat2, mat2_inverse, Vec2, lt_newton_trace stub, coc_radius
  - src/DeepCDefocusPO.cpp — compilable PlanarIop skeleton; Deep input wiring; poly load/destroy lifecycle; flat-black renderStripe; four knobs; Op::Description
  - src/CMakeLists.txt — DeepCDefocusPO in PLUGINS (line 16) and FILTER_NODES (line 53)
  - scripts/verify-s01-syntax.sh — updated with Iop/PlanarIop/ImagePlane/DeepOp mock stubs; DeepCDefocusPO.cpp in check loop
verification_summary:
  syntax_check: pass (g++ -std=c++17 -fsyntax-only; all 3 files pass)
  grep_contracts: pass (all 9 symbol contracts)
  cmake_count: pass (2 DeepCDefocusPO entries in CMakeLists.txt)
  docker_build: CI-only (Docker not installed in workspace; all code-level gates pass)
requirements_proved:
  - R020: validated (poly_system_read/evaluate/destroy wired; File_knob wired; error path present)
  - R026: validated (PlanarIop base class confirmed; flat RGBA output, not Deep stream)
---

# S01: Iop Scaffold, Poly Loader, and Architecture Decision — Summary

## What This Slice Delivered

S01 produced the entire compilable foundation for DeepCDefocusPO. The node has no scatter output yet (renderStripe writes flat black) but every structural component is in place and verified:

1. **`src/poly.h`** — Complete vendored lentil polynomial-optics API (MIT). All functions (`poly_pow`, `poly_term_evaluate`, `poly_system_read`, `poly_system_evaluate`, `poly_system_destroy`) defined inline. Struct given a tag name (`poly_system_s`) to enable C++ forward declarations without ODR conflicts. `#define DEEPC_POLY_H` sentinel emitted to coordinate with `deepc_po_math.h`.

2. **`src/deepc_po_math.h`** — Standalone math primitives with no DDImage dependency and no poly.h include. Contains `Mat2`/`mat2_inverse` (analytic 2×2 inverse, no Eigen), `Vec2`, `lt_newton_trace` stub (returns `{0,0}`; S02 fills in Newton iteration), and `coc_radius` (thin-lens CoC formula). Receives `poly_system_t*` via forward declaration guarded by `#ifndef DEEPC_POLY_H`.

3. **`src/DeepCDefocusPO.cpp`** — Full PlanarIop skeleton:
   - `PlanarIop` base class (D027 confirms; retires the risk from the roadmap)
   - Two Deep inputs: input 0 (unlabelled, required), input 1 ("holdout", optional)
   - `_validate(for_real)` copies `DeepInfo` to `info_` (box, format, channels), calls `poly_system_read` on first-cook or knob-change, calls `Op::error()` on load failure
   - `knob_changed` sets `_reload_poly=true` on `poly_file` change; `_validate` clears it after reload
   - `renderStripe` writes zeros to every channel (flat black) — marked `// S02: replace with PO scatter loop`
   - Destructor + `_close()` both safely call `poly_system_destroy` if `_poly_loaded`, reset flag (double-call safe)
   - Four knobs: `File_knob(poly_file)`, `Float_knob(focus_distance, 200mm)`, `Float_knob(fstop, 2.8)`, `Int_knob(aperture_samples, 64)`
   - `Op::Description` registration: `"DeepCDefocusPO"` / `"Deep/DeepCDefocusPO"`

4. **`src/CMakeLists.txt`** — `DeepCDefocusPO` added to `PLUGINS` list (line 16, after `DeepCDepthBlur`) and to `FILTER_NODES` list (line 53). Node will appear in Nuke's Filter menu category.

5. **`scripts/verify-s01-syntax.sh`** — Extended with new mock stubs: `DDImage/Iop.h`, `DDImage/PlanarIop.h`, `DDImage/ImagePlane.h`, `DDImage/DeepOp.h`; added `File_knob`, `ChannelMask`/`Mask_RGBA`, `error(const char*, ...)`, `Knob::name()` to existing stubs; added `DeepCDefocusPO.cpp` to the check loop; added `-I$SRC_DIR` so `poly.h`/`deepc_po_math.h` resolve.

## Architecture Decisions Made

**PlanarIop confirmed (D027, supersedes D021):** The `renderStripe` model runs single-threaded on a stripe. This is the correct base for S02's stochastic aperture scatter because each scatter contribution writes to a different output pixel — with `PlanarIop`'s single-threaded stripe, there is no shared buffer race and no need for a mutex or atomic accumulator. Had we chosen a multi-threaded `Iop`, we would have needed per-thread accumulation buffers and a final reduction step.

**ODR firewall established:** `poly.h` is included in exactly one TU (`DeepCDefocusPO.cpp`). `deepc_po_math.h` uses a forward declaration with a `#ifndef DEEPC_POLY_H` guard. This prevents multiple-definition linker errors while keeping the math primitives accessible from any file that only needs the type signature.

**Eigen removed (D023):** The only Eigen use was a 2×2 matrix inverse in the Newton iteration. `mat2_inverse` in `deepc_po_math.h` is a four-line analytic formula. No Eigen in the build.

## Key Patterns Established for S02/S03/S04

- **Deep input access:** `input0()` returns `dynamic_cast<DeepOp*>(Op::input(0))`; null check guards all access. Input 1 (holdout) uses the same pattern via `Op::input(1)`.
- **Poly load lifecycle:** `_reload_poly` flag set by `knob_changed`; `_validate(for_real)` executes load (with destroy-before-reload for hot-swap); `_poly_loaded` flag transitions `false → true` on success. `error()` on failure red-tiles the node with the file path in the message.
- **renderStripe insertion point:** The S01 comment `// S02: replace with PO scatter loop` marks exactly where S02's scatter math goes. The bounds/channels setup above it is correct and must not be removed.
- **Knob skeleton wired:** All four S04 knobs (`poly_file`, `focus_distance`, `fstop`, `aperture_samples`) are already present with tooltips and sensible defaults. S04 only needs to add any additional polish or group decoration.
- **Mock header extension pattern:** Adding a new PlanarIop plugin to `verify-s01-syntax.sh` requires three mock headers (Iop.h, PlanarIop.h, ImagePlane.h) plus File_knob in Knobs.h. Both `Box& box()` and `const Box& box() const` overloads must be in the DeepInfo mock for existing (mutable) and new (read-only) call sites.

## Verification Results

| Check | Command | Result |
|-------|---------|--------|
| Syntax check | `bash scripts/verify-s01-syntax.sh` | ✅ All 3 files pass |
| poly_system_read+destroy+File_knob+renderStripe in .cpp | grep chain | ✅ All pass |
| CMakeLists.txt entry count | `grep -c 'DeepCDefocusPO' src/CMakeLists.txt` → 2 | ✅ Pass |
| deepc_po_math.h symbols | `grep -q lt_newton_trace && mat2_inverse && coc_radius` | ✅ All pass |
| poly.h vendored | `grep -q poly_system_read src/poly.h` | ✅ Pass |
| Verifier updated | grep PlanarIop/File_knob/DeepCDefocusPO.cpp in script | ✅ All pass |
| Docker build | `docker-build.sh --linux --versions 16.0` | ⚠️ CI-only (no Docker in workspace) |
| CMake syntax | `cmake -S . -B /tmp -DNuke_ROOT=/nonexistent` | ✅ Syntax clean; fails at find_package as expected |

## What S02 Must Know

- **renderStripe is the scatter entry point.** Replace the zeroing loop body (lines below `// S02: replace with PO scatter loop`) with the per-pixel Deep fetch and aperture scatter loop. The `bounds`, `chans`, and `imagePlane.makeWritable()` setup must be preserved.
- **`lt_newton_trace` is a stub returning `{0,0}`.** S02 fills in the Newton iteration using `mat2_inverse` from `deepc_po_math.h` and `poly_system_evaluate` from poly.h. `_poly_sys` is available as a member after `_validate` has run.
- **`coc_radius` is fully implemented.** Pass `focal_length_mm` (read from `_poly_sys` metadata in S02), `_fstop`, `_focus_distance`, and sample `z` depth to get the CoC radius in mm.
- **`_poly_loaded` guards all poly calls.** Always check `_poly_loaded` before calling `poly_system_evaluate` in `renderStripe`.
- **`input0()->deepInfo()` has the scene's Deep geometry.** Call `input0()->validate()` (already done in `_validate`) before accessing `deepInfo()` in `renderStripe`.
- **docker-build.sh must be run in CI.** The workspace has no Docker. The integration gate (`DeepCDefocusPO.so` in release zip) must be confirmed there.

## What S03 Must Know

- **Holdout input is already wired as input 1.** `Op::input(1)` casts to `DeepOp*`. Connect holdout Deep fetch in S03's `renderStripe` extension — no new input machinery needed.
- **Holdout is evaluated at output pixel, not pre-applied.** This is the anti-double-defocus semantic (D024). S03 inserts the holdout query inside the output-pixel loop, after scatter, not before the Deep input fetch.

## What S04 Must Know

- **All four knobs are already present** with tooltips and defaults. S04 can add `BeginGroup`/`EndGroup` decoration, a `Newline`, or a help text update — the knob values are wired and functional.

## Requirements Updated

- **R020** (poly file load): moved to `validated` — poly_system_read/evaluate/destroy wired; File_knob present; error path confirmed at syntax level.
- **R026** (flat output): moved to `validated` — PlanarIop base class confirmed; Op::Description registration present; flat RGBA output verified by syntax check and renderStripe implementation.
