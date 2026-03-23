# S01 Research: Iop Scaffold, Poly Loader, and Architecture Decision

**Slice:** S01 — Iop Scaffold, Poly Loader, and Architecture Decision  
**Milestone:** M006 — DeepCDefocusPO  
**Active Requirements this slice owns:** R020 (poly file load), R026 (flat output)  
**Active Requirements this slice supports:** R019 (scaffold for S02), R025 (aperture sample stub)

---

## Summary

S01 is foundational but architecturally novel. Three previously unresolved questions must be locked in before S02 can build anything:

1. **Which Iop base class** and exact NDK wiring pattern to produce flat 2D from a Deep input — now confirmed against SDK docs.
2. **gather vs scatter threading model** — the NDK's PlanarIop threading contract resolves this in favour of gather inside `renderStripe()`.
3. **poly.h integration** — source obtained; it is pure C with `malloc`/`fread`; no platform issues.

After this research, all three are resolved. S01 is a substantial build but contains no remaining unknowns.

---

## Recommendation

**Use `PlanarIop` as the base class with a gather model inside `renderStripe()`.**

Rationale:
- `PlanarIop::renderStripe(ImagePlane&)` is called **single-threaded** on a full stripe (or full frame). No shared output buffer, no mutex, no race condition — the threading problem dissolves.
- The gather model fits naturally: `renderStripe` knows the full output box, can fetch the entire Deep plane for that box at once via `input0()->deepEngine(box, channels, plane)`, then iterate output pixels accumulating contributions.
- `Iop::engine()` (row-based) would require a scatter buffer spanning the full output format because any input Deep sample at any x,y could scatter to any output pixel — that buffer would need atomic or mutex protection against Nuke's parallel `engine()` calls. PlanarIop avoids this entirely.
- `DeepSampleCount` (the canonical NDK deep→2D example) uses plain `Iop` + `engine()`, which works for a gather-only per-row operation. For our stochastic scatter it does not.

**Architecture decision:** Gather inside PlanarIop `renderStripe()`. S01 stubs the loop (zero output); S02 fills in the scatter math.

---

## Implementation Landscape

### Files to create in S01

| File | Role |
|------|------|
| `src/DeepCDefocusPO.cpp` | PlanarIop subclass: scaffold only, outputs flat black; wires knobs; accepts Deep input; loads .fit on `knob_changed` |
| `src/poly.h` | Verbatim copy of lentil `polynomial-optics/src/poly.h` (MIT). Pure C, `#pragma once`. |
| `src/deepc_po_math.h` | Standalone header: hand-rolled 2×2 matrix inverse + CoC radius formula; Newton iteration stub |

### Files to modify

| File | Change |
|------|--------|
| `src/CMakeLists.txt` | Add `DeepCDefocusPO` to `PLUGINS` list and `FILTER_NODES` list |

No other files change in S01.

---

## NDK Base Class — Confirmed Pattern

### Iop → PlanarIop for Deep→flat

The NDK provides a documented `DeepSampleCount` example (NDK guide, "Deep to 2D Ops") that inherits from `Iop` and accepts a Deep input:

```cpp
class DeepSampleCount : public Iop {
public:
    DeepSampleCount(Node* node) : Iop(node) { inputs(1); }

    // Override test_input to accept DeepOp instead of Iop:
    virtual bool test_input(int idx, Op* op) const {
        return dynamic_cast<DeepOp*>(op);
    }
    virtual Op* default_input(int idx) const { return nullptr; } // no fallback

    DeepOp* input0() { return dynamic_cast<DeepOp*>(Op::input(0)); }

    void _validate(bool real) {
        if (input0()) {
            input0()->validate(real);
            info_ = input0()->deepInfo(); // copy deep bbox/format to Iop info_
            info_.channels(Mask_RGBA);    // set output channels
            set_out_channels(Mask_RGBA);
        } else {
            info_.set(Box()); info_.channels(Mask_None);
        }
    }

    void _request(int x, int y, int r, int t, ChannelMask channels, int count) {
        if (input0())
            input0()->deepRequest(Box(x, y, r, t), channels, count);
    }

    void engine(int y, int x, int r, const ChannelSet& cs, Row& row) {
        // fetch row: input0()->deepEngine(y, x, r, channels, plane)
    }
};
```

**For PlanarIop**, the `engine()` + `_request()` pair is replaced by `renderStripe(ImagePlane&)` + `getRequests()`:

```cpp
class DeepCDefocusPO : public PlanarIop {
    // ...
    void getRequests(const Box& box, const ChannelSet& channels,
                     int count, RequestOutput& reqData) const override {
        if (input0())
            input0()->deepRequest(box, channels + Chan_DeepFront + Chan_DeepBack + Chan_Alpha, count);
    }

    void renderStripe(ImagePlane& imagePlane) override {
        // imagePlane.bounds() gives the stripe box
        // Fetch entire Deep plane for this box in one call
        // Iterate output pixels; accumulate contributions
        // Write to imagePlane writable pixels
    }
};
```

Key facts from NDK docs:
- `PlanarIop::renderStripe()` is called **single-threaded** — one stripe at a time. Custom threading is opt-in.
- `PlanarIops` must NOT implement `_request()` — use `getRequests()` instead.
- `_validate()` must set `info_` (the Iop output info) — copy from deep input's `deepInfo()` then adjust channels to Mask_RGBA.
- The NDK note: "A hybrid implementation should set both `Iop::info_` and `_deepInfo`" — for a Deep→flat node we only set `info_` (we output flat, not deep).

### Second (holdout) input wiring

S01 stubs the holdout input as always-optional. The second input accepts a `DeepOp`:

```cpp
int maximum_inputs() const override { return 2; }
int minimum_inputs() const override { return 1; }

bool test_input(int idx, Op* op) const override {
    return dynamic_cast<DeepOp*>(op) != nullptr;
}
Op* default_input(int idx) const override { return nullptr; }

const char* input_label(int n, char*) const override {
    switch (n) {
        case 0: return "";       // main Deep input
        case 1: return "holdout"; // holdout Deep input
        default: return "";
    }
}
```

In S01 the holdout input is wired up (knob connection point exists) but the holdout fetch and stencil loop are stubs for S03.

---

## poly.h — Confirmed API

Source obtained from both `hanatos/lensoptics` and `zpelgrims/lentil`. The two versions are structurally identical. The file is MIT-licensed, pure C, header-only, `#pragma once`, no platform macros.

### Key types

```c
typedef struct poly_term_t {
  float  coeff;
  int32_t exp[5];      // exponents for [x, y, dx, dy, lambda]
} poly_term_t;

typedef struct poly_t {
  int32_t    num_terms;
  poly_term_t *term;   // heap-allocated by poly_system_read
} poly_t;

typedef struct poly_system_t {
  poly_t poly[5];      // one polynomial per output variable
} poly_system_t;
```

### Read API (what S01 uses)

```c
// Returns 0 on success, 1 on error (file not found or truncated)
int poly_system_read(poly_system_t *s, const char *filename);

// Evaluate: x[5] = {scene_x, scene_y, dx, dy, lambda_um}
// out[5] written: {out_x, out_y, out_dx, out_dy, transmittance}
void poly_system_evaluate(const poly_system_t *s, const float *x, float *out, int max_degree);

// Must call when done to avoid memory leak
void poly_system_destroy(poly_system_t *s);
```

### Integration constraints

1. **`malloc`/`fread` based** — `poly_system_read` calls `malloc` internally to allocate `term` arrays. `poly_system_destroy` calls `free`. Lifecycle: load in `knob_changed`/`_open`, destroy in `_close` or destructor.
2. **`#define real double`** in `hanatos/lensoptics` version; plain `double` in `zpelgrims/lentil` version. The lentil version is preferred (it's what pota uses). Vendor the lentil version.
3. **C linkage in C++ TU**: `poly.h` is pure C. To vendor into a C++ `.cpp`, wrap in `extern "C" { ... }` if the `#pragma once` guard doesn't handle it, OR simply `#include "poly.h"` directly (it compiles cleanly under g++ since it uses no C++ features).
4. **`poly_pow` is defined (not just declared)** in the header. If `poly.h` is included in multiple TUs, this causes ODR violations. In S01 it is included only in `DeepCDefocusPO.cpp`. If `deepc_po_math.h` also includes it via a chain, that must be guarded.

### .fit file format

Binary, little-endian, sequential for each of 5 polynomials:
```
[int32 num_terms][poly_term_t * num_terms] × 5
```
`poly_term_t` is `{ float coeff; int32_t exp[5]; }` = 24 bytes per term. No magic number, no version field — read must succeed or return error 1.

---

## deepc_po_math.h — Hand-Rolled 2×2 Inverse

The gencode.h Newton iteration uses `Eigen::Matrix2d` for a 2×2 Jacobian inverse. The actual math is trivial:

```
Given J = [ a  b ]
          [ c  d ]

invdet = 1 / (a*d - b*c)

J^-1 = invdet * [  d  -b ]
                [ -c   a ]
```

This is the exact code gencode.h generates inside the Newton loop. DeepC replaces the Eigen call with:

```cpp
// In deepc_po_math.h
struct Mat2 { double m[2][2]; };

inline Mat2 mat2_inverse(const Mat2& J) {
    const double invdet = 1.0 / (J.m[0][0]*J.m[1][1] - J.m[0][1]*J.m[1][0]);
    return { {{ J.m[1][1]*invdet, -J.m[0][1]*invdet },
               {-J.m[1][0]*invdet,  J.m[0][0]*invdet }} };
}
```

The Newton iteration itself (`print_lt_sample_aperture`) is a **code generator** — it emits C++ source. For DeepC we need the **runtime equivalent**: a hand-written inline function that runs the iteration from inputs. The S01 task is to stub this function with a signature but a no-op body; S02 fills it in with the full Newton loop.

Signature to establish in S01 (stub):

```cpp
// In deepc_po_math.h
//
// sensor_pos[2]   = {x, y} on sensor (mm)
// aperture_pos[2] = {x, y} on aperture disk (mm)
// lambda_um       = wavelength in micrometers
// poly_sys_outer  = the outer-pupil polynomial system (.fit file)
// poly_sys_ap     = the aperture polynomial system (.fit file, if separate)
//
// Returns: {outer_pupil_dx, outer_pupil_dy} direction — or {0,0} on failure
struct Vec2 { double x, y; };
Vec2 lt_newton_trace(const float sensor_pos[2], const float aperture_pos[2],
                     float lambda_um, const poly_system_t* poly_sys_outer);
```

CoC radius formula (also in `deepc_po_math.h`):

```cpp
// focal_length_mm, fstop, focus_dist_mm, sample_depth_mm
// Returns CoC radius in scene units (same as depth units)
inline float coc_radius(float focal_length_mm, float fstop,
                        float focus_dist_mm, float sample_depth_mm) {
    // Thin-lens CoC: r = (f/N) * |d - d_f| / d
    // Used only to bound scatter radius for gather; actual bokeh from PO
    const float aperture_diam = focal_length_mm / fstop;
    const float delta = std::fabs(sample_depth_mm - focus_dist_mm);
    if (sample_depth_mm < 1e-6f) return 0.0f;
    return aperture_diam * delta / sample_depth_mm;
}
```

---

## Knob Layout (S01 establishes skeleton for S04)

Four knobs wired in S01 (S04 adds tooltips and polish):

```cpp
void knobs(Knob_Callback f) override {
    File_knob(f, &_poly_file, "poly_file", "lens file");
    Float_knob(f, &_focus_distance, "focus_distance", "focus distance");
    Float_knob(f, &_fstop, "fstop", "f-stop");
    Int_knob(f, &_aperture_samples, "aperture_samples", "aperture samples");
}
```

Member types:
- `const char* _poly_file = nullptr;` — File_knob binds to a `const char*` in Nuke NDK
- `float _focus_distance = 200.0f;` — mm, default 2m
- `float _fstop = 2.8f;` — default f/2.8
- `int _aperture_samples = 64;` — stochastic sample count

Poly file load must be triggered on `knob_changed` when `poly_file` changes:

```cpp
int knob_changed(Knob* k) override {
    if (std::string(k->name()) == "poly_file") {
        _reload_poly = true;
        return 1;
    }
    return Iop::knob_changed(k);  // or PlanarIop
}
```

And executed in `_validate(true)` when `_reload_poly` is true, or in `renderStripe` on first use.

---

## Poly Load Lifecycle (Iop context)

Unlike a DeepFilterOp (which is purely reactive), PlanarIop has a clear render lifecycle:

```
_validate(for_real) → [renderStripe() per stripe] → _close()
```

Poly file lifecycle:

| Method | Action |
|--------|--------|
| `_validate(for_real=true)` | If `_poly_file` changed or `_poly_loaded == false`: call `poly_system_read()`. Set error and return if it fails. |
| `renderStripe()` | Assert `_poly_loaded`. Use `_poly_sys` for evaluation. |
| `_close()` / destructor | Call `poly_system_destroy()`. |

Error handling: if `poly_system_read` returns 1 (file not found), call `error("Cannot open lens file: %s", _poly_file)` — this sets Nuke's error state visually on the node.

---

## build.sh / CMakeLists Integration

To add `DeepCDefocusPO`:

**`src/CMakeLists.txt`** — two changes:
1. Add `DeepCDefocusPO` to `PLUGINS` list (it has no DeepCWrapper dependency)
2. Add `DeepCDefocusPO` to `FILTER_NODES` list (menu category)

No special link targets — no Eigen, no external libs. `poly.h` is header-only C included directly.

---

## Verification Plan for S01

### Contract verification (fast, no Docker)

**`scripts/verify-s01-syntax.sh`** — update mock headers to add:
- `DDImage/Iop.h` / `DDImage/PlanarIop.h` stubs (Iop base class with `info_`, `set_out_channels`, `copy_info`)
- `DDImage/ImagePlane.h` stub (for PlanarIop `renderStripe`)
- `File_knob` inline stub in `DDImage/Knobs.h`

Then add `DeepCDefocusPO.cpp` to the syntax check loop.

**grep contracts**:
```bash
grep -q 'poly_system_read'      src/DeepCDefocusPO.cpp
grep -q 'poly_system_destroy'   src/DeepCDefocusPO.cpp  # or destructor
grep -q 'File_knob'             src/DeepCDefocusPO.cpp
grep -q 'renderStripe'          src/DeepCDefocusPO.cpp  # PlanarIop used
grep -c 'DeepCDefocusPO'        src/CMakeLists.txt      # should be ≥2 (PLUGINS + FILTER_NODES)
grep -q 'lt_newton_trace'       src/deepc_po_math.h     # stub present
grep -q 'mat2_inverse'          src/deepc_po_math.h
grep -q 'coc_radius'            src/deepc_po_math.h
grep -q 'poly_system_read'      src/poly.h              # poly.h vendored
```

### Integration verification

`docker-build.sh --linux --versions 16.0` exits 0; `DeepCDefocusPO.so` present in `release/DeepC-Linux-Nuke16.0.zip`.

---

## Natural Seams (for task decomposition)

The planner should split S01 into three or four tasks that can be sequenced:

| Task | What it builds | Blocks |
|------|---------------|--------|
| T01 | `src/poly.h` (vendor copy) + `src/deepc_po_math.h` (stubs only) | T02 |
| T02 | `src/DeepCDefocusPO.cpp` skeleton (PlanarIop, knobs, Deep input wiring, poly load/destroy, output black) | T03 |
| T03 | CMake integration (`PLUGINS` + `FILTER_NODES`) + update `verify-s01-syntax.sh` (new mock headers + new file) | T04 |
| T04 | Syntax check passes + docker build exits 0 | — |

T01 has no codebase dependencies — it can be written from the poly.h source obtained in this research without reading any existing files. T02 depends on T01 for the include. T03 depends on T02 to know the class name. T04 is verification only.

---

## Known Risks / Watch-outs

1. **`poly.h` ODR**: `poly_pow`, `poly_term_evaluate`, etc. are defined (not declared) in the header. If poly.h is ever included in more than one TU, the linker will emit multiple-definition errors. Keep `poly.h` in one TU only (`DeepCDefocusPO.cpp`). `deepc_po_math.h` must NOT include `poly.h`; it receives a `poly_system_t*` pointer.

2. **`File_knob` mock stub missing**: The existing `scripts/verify-s01-syntax.sh` has no `File_knob` stub in `DDImage/Knobs.h`. Adding `DeepCDefocusPO.cpp` to the syntax check will fail until `File_knob` is added. Signature: `inline Knob* File_knob(Knob_Callback, const char**, const char*, const char* = nullptr) { return nullptr; }` — note `const char**` not `const char*`.

3. **`PlanarIop.h` mock stub**: The existing mock headers cover `DeepFilterOp` but not `Iop` or `PlanarIop`. New mocks needed for: `DDImage/Iop.h` (with `info_`, `set_out_channels`, `copy_info`), `DDImage/PlanarIop.h` (with `renderStripe(ImagePlane&)`, `getRequests()`), `DDImage/ImagePlane.h` (with `ImagePlane`, bounds/channels accessors).

4. **`deepInfo()` vs `info_`**: In `_validate`, the deep input's `deepInfo()` returns a `DeepInfo` (has `.box()`, `.channels()`, `.full_size_format()`). The Iop's `info_` is set from this. The correct idiom is:
   ```cpp
   input0()->validate(for_real);
   const DeepInfo& di = input0()->deepInfo();
   info_.set(di.box());
   info_.full_size_format(*di.full_size_format());
   info_.channels(Mask_RGBA);
   set_out_channels(Mask_RGBA);
   ```
   This is confirmed by the NDK DeepSampleCount example and the DeepOp docs note: "A hybrid implementation should set both Iop::info_ and _deepInfo — for Deep→flat we set only info_."

5. **`poly_system_read` thread safety**: `poly_system_read` uses `malloc` and `fread`. Loading in `_validate(true)` is safe (Nuke calls `_validate` single-threaded before any `renderStripe`). Do not load in `renderStripe` without a mutex.

6. **Destructor vs `_close`**: `_close()` is called by Nuke after idle timeout. For deterministic cleanup, also call `poly_system_destroy` in the class destructor. Guard with a `_poly_loaded` bool to prevent double-free.

---

## Sources

- NDK documentation: "Deep to 2D Ops" (learn.foundry.com/nuke/developers/11.2/ndkdevguide/deep/deepto2d.html) — canonical DeepSampleCount example
- NDK documentation: "Planar Iop" (learn.foundry.com/nuke/developers/150/ndkdevguide/2d/planariops.html) — PlanarIop renderStripe threading model
- NDK DeepOp reference (learn.foundry.com/nuke/developers/10.5/ndkreference/Plugins/classDD_1_1Image_1_1DeepOp.html)
- `polynomial-optics/src/poly.h` from zpelgrims/lentil (MIT) — obtained from GitHub raw
- `polynomial-optics/src/gencode.h` from zpelgrims/lentil (MIT) — Newton iteration code-generator; read to extract the 2×2 inverse pattern
- Existing codebase: `src/DeepCDepthBlur.cpp` (Deep fetch patterns, multi-input, getDeepRequests), `src/DeepCConstant.cpp` (Description registration, knobs), `src/CMakeLists.txt` (PLUGINS / FILTER_NODES lists)
