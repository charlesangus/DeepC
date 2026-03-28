# S01 Research: Integration test .nk suite

**Slice:** Integration test .nk suite  
**Milestone:** M010-zkt9ww — DeepCOpenDefocus Integration Tests  
**Requirements:** R060–R066 (all owned by this slice)

---

## Summary

This slice is straightforward file-authoring work with one genuine surprise: **`use_gpu` is referenced in the CONTEXT.md and required by R063, but the knob does NOT exist in `src/DeepCOpenDefocus.cpp`**. The Rust backend already supports CPU-only mode (`OpenDefocusRenderer::new(false, ...)`), but there is no C++ knob or FFI parameter to activate it. This means three small additional files must change (C++, FFI header, Rust `lib.rs`) to satisfy R063. Everything else — the 6 new `.nk` files, updating `test/test_opendefocus.nk`, the gitignore, and the verify script — is pure file creation/editing with no technical unknowns.

---

## What Exists Today

### `test/test_opendefocus.nk` (existing, needs update)
- Single `DeepConstant` (128×72, RGBA 0.5/0.5/0.5/1.0, depth 5.0–5.5)
- `DeepCOpenDefocus` with default knobs
- `Write` node targets `/tmp/test_opendefocus.exr` — **must be changed to `test/output/test_opendefocus.exr`**

### `test/output/` directory
- Does NOT exist yet. Must be created with a `.gitignore` that ignores all EXR files.

### `src/DeepCOpenDefocus.cpp` (352 lines)
Confirmed knobs (4 × `Float_knob`):
- `focal_length` (default 50 mm)
- `fstop` (default 2.8)  
- `focus_distance` (default 5.0)
- `sensor_size_mm` (default 36)

**No `use_gpu` Bool_knob exists.** The CONTEXT.md lists it as an existing knob — this is incorrect. The Rust `lib.rs` `get_renderer()` function calls `OpenDefocusRenderer::new(true, ...)` for GPU-with-fallback and `OpenDefocusRenderer::new(false, ...)` for CPU-only, but there is no C++ → Rust signal to select between them.

Inputs:
- Input 0: deep image (required) — `input_label` → `""`
- Input 1: holdout deep (optional) — `input_label` → `"holdout"`
- Input 2: camera (optional) — `input_label` → `"camera"`

Camera link reads: `focal_length()`, `fStop()`, `focal_point()`, `film_width() * 10.0f` (cm→mm).

### `crates/opendefocus-deep/include/opendefocus_deep.h`
FFI structs:
- `LensParams { focal_length_mm, fstop, focus_distance, sensor_size_mm }` — all `float`
- `DeepSampleData { sample_counts, rgba, depth, pixel_count, total_samples }`
- Function: `opendefocus_deep_render(data, output_rgba, width, height, lens)`

### `scripts/verify-s01-syntax.sh`
- Runs `g++ -std=c++17 -fsyntax-only` on all 3 `.cpp` files with mock DDImage headers
- `Bool_knob` **is already mocked** in the mock headers
- Currently checks that `test/test_opendefocus.nk` exists (S02 section)
- Must be extended to check all 7 `.nk` files

### Reference Nuke `.nk` syntax (from `test/test_ray.nk~`)
```nuke
version 16.0 v6

Root {
 inputs 0
 format "128 72 0 0 128 72 1 128x72"
}

DeepFromImage {
 premult true
 set_z true
 z 5000
 name DeepFromImage1
}

Write {
 file ./test_ray.exr
 file_type exr
 first_part rgba
 name Write1
}
```

---

## Confirmed Nuke Node Knob Names

### Camera2 knobs (`.nk` TCL format)
From Nuke Python metadata API and CONTEXT.md Open Questions resolution:
| `.nk` knob | C++ DDImage method | LensParams field | Notes |
|---|---|---|---|
| `focal` | `focal_length()` | `focal_length_mm` | in mm |
| `haperture` | `film_width()` | `sensor_size_mm / 10` | stored in cm; C++ does `* 10.0f` |
| `fstop` | `fStop()` | `fstop` | dimensionless |
| `focal_point` | `focal_point()` | `focus_distance` | scene units |

Camera2 `.nk` example:
```
Camera2 {
 focal 35
 haperture 2.4
 fstop 1.4
 focal_point 200
 name Camera1
}
```

### DeepFromImage knobs (from `test_ray.nk~`)
```
DeepFromImage {
 premult true
 set_z true
 z 5.0
}
```

### DeepConstant knobs (from `test/test_opendefocus.nk`)
```
DeepConstant {
 format "128 72 0 0 128 72 1 DeepTest128x72"
 color {0.5 0.5 0.5 1.0}
 deep_front 5.0
 deep_back 5.5
}
```

### DeepMerge operation knob
Standard value: `operation plus` (or `operation over`). For combining multiple deep streams, `operation plus` (additive merge) is conventional.

---

## The `use_gpu` Gap (R063 Requires This)

**Problem:** R063 requires a test script that sets `use_gpu false`. That knob does not exist in `DeepCOpenDefocus.cpp`. An `.nk` file that writes `use_gpu false` would be silently ignored by Nuke's TCL parser — the test would run but would not actually exercise the CPU path.

**Required changes to satisfy R063:**

**1. `src/DeepCOpenDefocus.cpp`** — add:
```cpp
bool _use_gpu = true;  // private member
// in knobs():
Bool_knob(f, &_use_gpu, "use_gpu", "Use GPU");
// in renderStripe(): pass _use_gpu to FFI
opendefocus_deep_render(&sampleData, output_buf.data(), w, h, &lensParams, _use_gpu);
```

**2. `crates/opendefocus-deep/include/opendefocus_deep.h`** — add param to function signature:
```c
void opendefocus_deep_render(const DeepSampleData* data,
                             float*                output_rgba,
                             uint32_t              width,
                             uint32_t              height,
                             const LensParams*     lens,
                             int                   use_gpu);  // 0=CPU-only, 1=try GPU
```

**3. `crates/opendefocus-deep/src/lib.rs`** — update `opendefocus_deep_render` signature and pass `use_gpu != 0` to `get_renderer_mode(use_gpu_flag)`, or replace the existing `get_renderer()` call with `OpenDefocusRenderer::new(use_gpu != 0, &mut settings)` inline.

These are small, contained changes. The `Bool_knob` is already mocked in `verify-s01-syntax.sh`'s mock headers, so the syntax check will pass without further header changes.

---

## Files to Create / Modify

### CREATE
| File | Requirement | Notes |
|---|---|---|
| `test/output/.gitignore` | R065 | Contents: `*.exr\n*.tmp\n` |
| `test/test_opendefocus_multidepth.nk` | R060 | 3× DeepConstant at z=2,5,10 → DeepMerge → DeepCOpenDefocus → Write |
| `test/test_opendefocus_holdout.nk` | R061 | DeepConstant(subject) on input 0, DeepConstant(holdout, semi-transparent) on input 1 |
| `test/test_opendefocus_camera.nk` | R062 | Camera2 (non-default: focal=35, haperture=2.4, fstop=1.4, focal_point=200) on input 2 |
| `test/test_opendefocus_cpu.nk` | R063 | use_gpu false (requires the new knob to be added to C++) |
| `test/test_opendefocus_empty.nk` | R064 | DeepConstant with color {0 0 0 0} alpha=0 (closest to "empty" without Nuke internals) |
| `test/test_opendefocus_bokeh.nk` | R066 | Constant(1pxwhite) → Crop(256×256 center 1px) → DeepFromImage(z=1) → DeepCOpenDefocus(focus_distance=10) |

### MODIFY
| File | Change |
|---|---|
| `test/test_opendefocus.nk` | `file /tmp/test_opendefocus.exr` → `file test/output/test_opendefocus.exr` |
| `scripts/verify-s01-syntax.sh` | Extend S02 section to check all 7 `.nk` files exist |
| `src/DeepCOpenDefocus.cpp` | Add `bool _use_gpu = true` + `Bool_knob "use_gpu"` + pass to FFI |
| `crates/opendefocus-deep/include/opendefocus_deep.h` | Add `int use_gpu` param to `opendefocus_deep_render` |
| `crates/opendefocus-deep/src/lib.rs` | Accept `use_gpu: i32` in FFI fn, pass to `OpenDefocusRenderer::new()` |

---

## Natural Task Seams

The slice decomposes cleanly into 3 tasks:

**T01 — Add `use_gpu` Bool_knob + thread through FFI (C++, header, Rust)**
- Touches: `src/DeepCOpenDefocus.cpp`, `crates/opendefocus-deep/include/opendefocus_deep.h`, `crates/opendefocus-deep/src/lib.rs`
- Verify: `scripts/verify-s01-syntax.sh` passes (g++ -fsyntax-only + clang header check)
- This is the only risky task — it changes 3 files across 2 languages. Do this first.

**T02 — Infrastructure: test/output/ directory + update existing test + create all 6 new .nk scripts**
- Touches: `test/output/.gitignore`, `test/test_opendefocus.nk` (1-line change), 6 new `.nk` files
- Verify: all 7 `.nk` files exist (`ls test/test_opendefocus*.nk` shows 7 results)
- Pure file authoring, no risk.

**T03 — Update verify-s01-syntax.sh S02 section + run final verification**
- Touches: `scripts/verify-s01-syntax.sh` (add 6 new file existence checks)
- Verify: `sh scripts/verify-s01-syntax.sh` exits 0

---

## What to Build/Prove First

T01 (use_gpu knob + FFI threading) is the only real risk and should be executed first. It is a small but cross-language change; if the syntax check fails it must be caught before authoring 6 `.nk` files that depend on the new knob name being correct.

---

## `.nk` Script Node Graph Patterns

### Multi-depth (R060)
```
DeepConstant(z=2) ──┐
DeepConstant(z=5) ──┤ DeepMerge → DeepCOpenDefocus → Write(test/output/multidepth.exr)
DeepConstant(z=10) ─┘
```
Format: 128×72. All three DeepConstants have `color {0.5 0.5 0.5 1.0}`.

### Holdout (R061)
```
DeepConstant(subject, z=5) → DeepCOpenDefocus → Write(test/output/holdout.exr)
DeepConstant(holdout, alpha=0.5, z=3) ──────────────────────[input 1: "holdout"]
```

### Camera link (R062)
```
DeepConstant → DeepCOpenDefocus → Write(test/output/camera.exr)
Camera2(focal=35, haperture=2.4, fstop=1.4, focal_point=200) ─[input 2: "camera"]
```

### CPU-only (R063)
```
DeepConstant → DeepCOpenDefocus(use_gpu false) → Write(test/output/cpu.exr)
```

### Empty input (R064)
```
DeepConstant(color {0 0 0 0}) → DeepCOpenDefocus → Write(test/output/empty.exr)
```
Note: This produces 1 sample/pixel with alpha=0, which is the closest achievable to "empty" in batch-mode `.nk`. The C++ renderStripe will process it normally (samples exist but are transparent). The lib.rs `total_samples == 0` guard is a different path exercised only when no samples at all are provided — not reachable from standard Nuke nodes. The "empty" test verifies graceful handling of zero-alpha input without crashing.

### Bokeh disc (R066) — most visually meaningful
```
Constant(1×1 white, format 256×256) → Crop(center 1px) → DeepFromImage(set_z true, z 1.0) 
  → DeepCOpenDefocus(focal_length 50, fstop 2.8, focus_distance 10, sensor_size_mm 36)
  → Write(test/output/bokeh.exr)
```
The 1px bright source at depth 1.0 with focus plane at 10.0 should produce a visible bokeh disc. 256×256 format gives the kernel enough room to spread.

**DeepFromImage detail:** Nuke's `DeepFromImage` converts a flat image into deep by assigning depth. Knob `set_z true` enables the `z` knob for manual depth assignment. With `z 1.0` each pixel becomes a single deep sample at depth 1.0.

**Crop to 1px center for bokeh:** Use `Crop { box {127 127 128 128} }` on a 256×256 Constant to isolate the center pixel. Everything else becomes black (0 alpha), so the defocus kernel only processes one non-zero source sample.

---

## Verification Strategy

After all tasks complete, the slice is verified by:
```sh
ls test/test_opendefocus.nk test/test_opendefocus_multidepth.nk \
   test/test_opendefocus_holdout.nk test/test_opendefocus_camera.nk \
   test/test_opendefocus_cpu.nk test/test_opendefocus_empty.nk \
   test/test_opendefocus_bokeh.nk
# → all 7 files present, exit 0

grep "test/output/" test/test_opendefocus.nk
# → confirms path was updated from /tmp/

cat test/output/.gitignore
# → shows *.exr gitignored

sh scripts/verify-s01-syntax.sh
# → All syntax checks passed + All S02 checks passed, exit 0
```

Runtime `nuke -x` verification is deferred to UAT (requires Nuke license).

---

## Risks and Constraints

| Risk | Severity | Mitigation |
|---|---|---|
| `use_gpu` FFI param change breaks `opendefocus_deep_render` signature in a way the mock headers don't fully exercise | Low | `verify-s01-syntax.sh` checks C++ syntax; clang checks header; Rust compilation happens at Docker build time |
| Camera2 `.nk` knob names wrong at runtime | Low | Names confirmed from Nuke Python metadata API (`focal`, `haperture`); DeepCWorld.cpp confirms `film_width()` / `focal_length()` C++ method mapping |
| Bokeh disc test produces black output at runtime (wrong CoC computation) | Low for this milestone | Runtime deferred to UAT; structural correctness is what this milestone proves |
| `test_empty.nk` doesn't actually exercise the `total_samples == 0` lib.rs guard | Low | Documented above — this guard requires truly no samples, not achievable from standard nodes. Test still proves "no crash on zero-alpha input." |

---

## Skills Discovered

None installed — this work involves `.nk` file authoring and small C++/Rust edits. No new skills are applicable.
