---
id: T03
parent: S01
milestone: M006
provides:
  - src/CMakeLists.txt — DeepCDefocusPO added to PLUGINS and FILTER_NODES lists
  - scripts/verify-s01-syntax.sh — PlanarIop/ImagePlane/Iop/DeepOp mock stubs added; File_knob stub added; DeepCDefocusPO.cpp added to check loop
key_files:
  - src/CMakeLists.txt
  - scripts/verify-s01-syntax.sh
key_decisions:
  - DeepOp.h stub created as a separate header (not embedded in DeepFilterOp.h) so DeepCDefocusPO.cpp can include it independently
  - DeepInfo::box() declared with both const and non-const overloads to satisfy DeepCBlur (mutable) and DeepCDefocusPO (const) usages in the same mock
  - Op mock gained error(const char*, ...) variadic method stub; Knob got name() const method stub
patterns_established:
  - When adding a new PlanarIop plugin to the verifier, three new mocks are required: Iop.h (info_ + set_out_channels), PlanarIop.h (renderStripe + getRequests), ImagePlane.h (writableAt); File_knob stub goes in Knobs.h
  - DeepInfo mock must expose both Box& box() and const Box& box() const to satisfy existing (mutable) and new (read-only) call sites in the same mock environment
observability_surfaces:
  - scripts/verify-s01-syntax.sh exits non-zero and prints "filename:line: error:" to stderr when any file fails — first inspectable CI failure surface
  - grep -c 'DeepCDefocusPO' src/CMakeLists.txt returns 2 to confirm both PLUGINS and FILTER_NODES registrations
duration: 25m
verification_result: passed
completed_at: 2026-03-23
blocker_discovered: false
---

# T03: CMake integration and syntax verifier update

**Added DeepCDefocusPO to CMakeLists.txt PLUGINS+FILTER_NODES and extended verify-s01-syntax.sh with Iop/PlanarIop/ImagePlane/DeepOp mock stubs; all three syntax checks now pass.**

## What Happened

Two files were modified to wire the new plugin into the build system and the syntax verifier.

**`src/CMakeLists.txt`**: `DeepCDefocusPO` was appended after `DeepCDepthBlur` in the `PLUGINS` list (non-wrapped; the `foreach` loop handles the rest) and appended to `FILTER_NODES` (puts the node in Nuke's Filter menu category). No other CMake changes were needed — the existing infrastructure handles the `.cpp` → `.so` pipeline automatically.

**`scripts/verify-s01-syntax.sh`**: The existing script had no stubs for `PlanarIop`, `ImagePlane`, `Iop` (as a distinct class with `info_`), or a standalone `DeepOp.h`. Several additions were required:

1. `DDImage/Op.h` — added `error(const char*, ...)` variadic method and `default_input()` virtual (both called by `DeepCDefocusPO.cpp`).
2. `DDImage/Knobs.h` — added `File_knob(Knob_Callback, const char**, const char*, const char* = nullptr)` stub; added `Knob::name() const` to support `knob_changed` call-site in `DeepCDefocusPO.cpp`; added `ChannelMask` typedef and `Mask_None`/`Mask_RGBA` constants to `Channel.h`.
3. `DDImage/Iop.h` — new stub; `Info` class with `set(Box)`, `channels(ChannelMask)`, `full_size_format()` overloads; `Iop` class with `info_`, `set_out_channels()`, `copy_info()`, `_validate()`, `_close()`, `knob_changed()`.
4. `DDImage/ImagePlane.h` — new stub; `writableAt(int x, int y, Channel z)` returning `float&` (the actual signature used by `DeepCDefocusPO.cpp`'s `renderStripe`).
5. `DDImage/PlanarIop.h` — new stub; `PlanarIop : Iop` with `renderStripe(ImagePlane&) = 0`, `getRequests()`, `_close()`.
6. `DDImage/DeepOp.h` — new stub; `DeepInfo` struct (non-const + const `box()` overloads to satisfy both `DeepCBlur.cpp` (mutates `_deepInfo.box()`) and `DeepCDefocusPO.cpp` (reads `in->deepInfo().box()`)); `DeepOp` class with `validate()`, `deepInfo()`, `deepRequest()`.
7. `DDImage/DeepFilterOp.h` — updated to include `DeepOp.h` rather than re-defining `DeepInfo` inline.

`DeepCDefocusPO.cpp` was added to the check loop. The `-I$SRC_DIR` flag was also added so `poly.h` and `deepc_po_math.h` resolve from the `src/` directory.

## Verification

All slice verification checks ran and passed:

```
bash scripts/verify-s01-syntax.sh   # exits 0; all 3 files pass
grep -c 'DeepCDefocusPO' src/CMakeLists.txt   # 2
grep -q 'poly_system_read' src/DeepCDefocusPO.cpp  # PASS
grep -q 'poly_system_destroy' src/DeepCDefocusPO.cpp  # PASS
grep -q 'File_knob' src/DeepCDefocusPO.cpp  # PASS
grep -q 'renderStripe' src/DeepCDefocusPO.cpp  # PASS
grep -q 'lt_newton_trace' src/deepc_po_math.h  # PASS
grep -q 'mat2_inverse' src/deepc_po_math.h  # PASS
grep -q 'coc_radius' src/deepc_po_math.h  # PASS
grep -q 'poly_system_read' src/poly.h  # PASS
grep -q 'PlanarIop' scripts/verify-s01-syntax.sh  # PASS
grep -q 'ImagePlane' scripts/verify-s01-syntax.sh  # PASS
grep -q 'File_knob' scripts/verify-s01-syntax.sh  # PASS
grep -q 'DeepCDefocusPO' scripts/verify-s01-syntax.sh  # PASS
failure-path diagnostic (poly_system_read in poly.h)  # PASS
```

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `bash scripts/verify-s01-syntax.sh` | 0 | ✅ pass | ~2s |
| 2 | `grep -q 'poly_system_read\|poly_system_destroy\|File_knob\|renderStripe' src/DeepCDefocusPO.cpp` | 0 | ✅ pass | <1s |
| 3 | `grep -c 'DeepCDefocusPO' src/CMakeLists.txt` (returns 2) | 0 | ✅ pass | <1s |
| 4 | `grep -q 'lt_newton_trace\|mat2_inverse\|coc_radius' src/deepc_po_math.h` | 0 | ✅ pass | <1s |
| 5 | `grep -q 'poly_system_read' src/poly.h` | 0 | ✅ pass | <1s |
| 6 | `grep -q 'PlanarIop\|ImagePlane\|File_knob\|DeepCDefocusPO' scripts/verify-s01-syntax.sh` | 0 | ✅ pass | <1s |
| 7 | failure-path diagnostic (poly_system_read present in poly.h) | 0 | ✅ pass | <1s |

## Diagnostics

- **Syntax verifier failure mode:** `bash scripts/verify-s01-syntax.sh` exits non-zero and prints `filename:line: error:` to stderr; the first failing filename + compiler error is the inspectable signal for CI and future agents.
- **CMakeLists regression check:** `grep -c 'DeepCDefocusPO' src/CMakeLists.txt` must return 2 — one in PLUGINS, one in FILTER_NODES. A return of 1 means one registration was lost; 0 means the plugin will not build at all.
- **New stub diagnostic:** If a future file includes a new DDImage header not present in the mock directory, `g++ -fsyntax-only` will print `fatal error: DDImage/Xyz.h: No such file or directory` — add a minimal stub following the pattern established here.
- **`writableAt` vs `writable`:** `DeepCDefocusPO.cpp` uses `imagePlane.writableAt(x, y, z)` (returns `float&`), which differs from the `writable(Channel, int y)` pointer form used by older plugins. Both are stubbed in `ImagePlane.h`.

## Deviations

- `DDImage/Op.h` required `error(const char*, ...)` and `default_input()` additions — the plan said to add these only if needed; they were needed and added.
- `DDImage/Channel.h` required `ChannelMask` typedef and `Mask_None`/`Mask_RGBA` constants — used in `_validate()`; not called out explicitly in the plan but consistent with the stated approach.
- `Knob` class needed a `name() const` method for `knob_changed` to compile — minimal addition to `Knobs.h`.
- `DeepInfo` needed non-const + const `box()` overloads; the plan only described the const form but `DeepCBlur.cpp`'s `_deepInfo.box().set(...)` call required the non-const overload to remain in the shared mock.
- `-I$SRC_DIR` added to `g++` invocation so `poly.h` and `deepc_po_math.h` resolve without explicit path in the `#include` directives.

## Known Issues

None. The docker build check (T04) is the remaining integration gate.

## Files Created/Modified

- `src/CMakeLists.txt` — added `DeepCDefocusPO` to `PLUGINS` list (after `DeepCDepthBlur`) and to `FILTER_NODES` list
- `scripts/verify-s01-syntax.sh` — added Iop.h, PlanarIop.h, ImagePlane.h, DeepOp.h mock stubs; added File_knob/Knob.name()/error()/Mask_RGBA/ChannelMask to existing stubs; added DeepCDefocusPO.cpp to check loop; added -I$SRC_DIR to compiler invocation
