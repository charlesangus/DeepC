---
estimated_steps: 5
estimated_files: 1
skills_used:
  - review
---

# T02: Write DeepCDefocusPO.cpp PlanarIop skeleton

**Slice:** S01 — Iop Scaffold, Poly Loader, and Architecture Decision
**Milestone:** M006

## Description

Write the main plugin source file `src/DeepCDefocusPO.cpp`. This is a `PlanarIop` subclass that:
- Accepts two Deep inputs (main + optional holdout) and produces a flat RGBA output
- Loads a lentil `.fit` polynomial file via `File_knob` and `poly_system_read` in `_validate(for_real)`
- Outputs flat black in `renderStripe()` (the scatter math goes in S02)
- Destroys the poly system in the destructor (and in `_close()`)

This task retires the "which base class?" risk from the milestone: PlanarIop is confirmed by research as the correct choice for stochastic scatter (single-threaded `renderStripe`, no shared output buffer race condition).

**Architecture decision locked in this task:** PlanarIop + gather model (D021 already recorded in DECISIONS.md — no new entry needed).

## Steps

1. Create `src/DeepCDefocusPO.cpp`. Add the standard copyright comment, includes:
   ```cpp
   #include "DDImage/PlanarIop.h"
   #include "DDImage/ImagePlane.h"
   #include "DDImage/DeepOp.h"
   #include "DDImage/DeepPixel.h"
   #include "DDImage/DeepPlane.h"
   #include "DDImage/Knobs.h"
   #include "poly.h"
   #include "deepc_po_math.h"
   #include <string>
   #include <cstring>
   ```

2. Define the class. `using namespace DD::Image;` at file scope. Static `CLASS` and `HELP` strings. Member variables:
   ```cpp
   const char* _poly_file;       // File_knob binds here
   float       _focus_distance;  // default 200.0f (mm — 2m)
   float       _fstop;           // default 2.8f
   int         _aperture_samples;// default 64
   bool        _poly_loaded;     // false until poly_system_read succeeds
   bool        _reload_poly;     // set true by knob_changed
   poly_system_t _poly_sys;      // zeroed in constructor
   ```

3. Implement constructor: call `PlanarIop(node)`, `inputs(2)`, zero-init all members, set defaults.

4. Implement `Class()`, `node_help()`, `op()`.

5. Implement input helpers:
   ```cpp
   int minimum_inputs() const override { return 1; }
   int maximum_inputs() const override { return 2; }
   bool test_input(int idx, Op* op) const override {
       return dynamic_cast<DeepOp*>(op) != nullptr;
   }
   Op* default_input(int idx) const override { return nullptr; }
   const char* input_label(int n, char*) const override {
       return (n == 1) ? "holdout" : "";
   }
   DeepOp* input0() const { return dynamic_cast<DeepOp*>(Op::input(0)); }
   ```

6. Implement `knobs()`:
   ```cpp
   File_knob(f, &_poly_file, "poly_file", "lens file");
   Float_knob(f, &_focus_distance, "focus_distance", "focus distance");
   Float_knob(f, &_fstop, "fstop", "f-stop");
   Int_knob(f, &_aperture_samples, "aperture_samples", "aperture samples");
   ```

7. Implement `knob_changed()`: if `std::string(k->name()) == "poly_file"`, set `_reload_poly = true; return 1;`. Otherwise delegate to `PlanarIop::knob_changed(k)`.

8. Implement `_validate(bool for_real)`:
   - Call `input0()->validate(for_real)` if input0 exists.
   - Copy DeepInfo to info_:
     ```cpp
     const DeepInfo& di = input0()->deepInfo();
     info_.set(di.box());
     info_.full_size_format(*di.full_size_format());
     info_.channels(Mask_RGBA);
     set_out_channels(Mask_RGBA);
     ```
   - If `!input0()`: set `info_.set(Box()); info_.channels(Mask_None);` and return.
   - If `for_real && _poly_file && (_reload_poly || !_poly_loaded)`: if `_poly_loaded`, call `poly_system_destroy(&_poly_sys)`; call `poly_system_read(&_poly_sys, _poly_file)`; if return is non-zero, `error("Cannot open lens file: %s", _poly_file); return;`; set `_poly_loaded = true; _reload_poly = false;`.

9. Implement `getRequests()`:
   ```cpp
   void getRequests(const Box& box, const ChannelSet& channels,
                    int count, RequestOutput& reqData) const override {
       if (input0())
           input0()->deepRequest(box,
               channels + Chan_DeepFront + Chan_DeepBack + Chan_Alpha,
               count);
   }
   ```

10. Implement `renderStripe(ImagePlane& imagePlane)`: call `imagePlane.makeWritable()`, then fill the writable buffer with zeros using `std::memset` or by zeroing each writable row. This produces flat black output — the scatter math replaces this in S02. Comment: `// S02: replace with PO scatter loop`.

11. Implement destructor: if `_poly_loaded`, call `poly_system_destroy(&_poly_sys)`.

12. Implement `_close()`: if `_poly_loaded`, call `poly_system_destroy(&_poly_sys); _poly_loaded = false;`.

13. Add `Description` registration at file scope:
    ```cpp
    static Op* build(Node* node) { return new DeepCDefocusPO(node); }
    const Op::Description DeepCDefocusPO::d(::CLASS, "Deep/DeepCDefocusPO", build);
    ```

## Must-Haves

- [ ] Class inherits from `PlanarIop` (not `Iop` or `DeepFilterOp`)
- [ ] `renderStripe(ImagePlane&)` implemented (not `engine()` or `doDeepEngine()`)
- [ ] `getRequests(...)` implemented (not `_request()`)
- [ ] Both `poly_system_read` and `poly_system_destroy` called (load + cleanup)
- [ ] `error()` called on load failure (not silent ignore)
- [ ] `File_knob` wired to `&_poly_file` (note: `const char**` in real NDK — mock uses same)
- [ ] Both inputs accept `DeepOp*`; input 1 labelled "holdout"
- [ ] `_validate` copies DeepInfo to `info_` including format and channel mask
- [ ] `renderStripe` outputs flat black (zeros) — no scatter yet
- [ ] `poly.h` included in this TU (only); `deepc_po_math.h` included (does not re-include poly.h)
- [ ] Op::Description registered with class name `"DeepCDefocusPO"` and path `"Deep/DeepCDefocusPO"`

## Verification

```bash
grep -q 'PlanarIop'           src/DeepCDefocusPO.cpp
grep -q 'renderStripe'        src/DeepCDefocusPO.cpp
grep -q 'getRequests'         src/DeepCDefocusPO.cpp
grep -q 'poly_system_read'    src/DeepCDefocusPO.cpp
grep -q 'poly_system_destroy' src/DeepCDefocusPO.cpp
grep -q 'File_knob'           src/DeepCDefocusPO.cpp
grep -q 'holdout'             src/DeepCDefocusPO.cpp
grep -q 'DeepCDefocusPO'      src/DeepCDefocusPO.cpp
```

## Inputs

- `src/poly.h` — poly_system_t and API (produced by T01)
- `src/deepc_po_math.h` — deepc_po_math types (produced by T01)
- `src/DeepCDepthBlur.cpp` — reference for multi-input Deep op patterns (minimum/maximum_inputs, input_label, test_input, getDeepRequests lifecycle) — do not modify
- `src/DeepCConstant.cpp` — reference for Op::Description registration style — do not modify

## Expected Output

- `src/DeepCDefocusPO.cpp` — compilable PlanarIop skeleton: Deep input wiring, poly load/destroy, flat-black renderStripe, all four knobs, Description registration

## Observability Impact

**Signals introduced by this task:**

- **`_poly_loaded` flag** (`bool`, member of `DeepCDefocusPO`): transitions `false → true` after the first successful `poly_system_read`. An agent inspecting Nuke's script state (or a standalone harness that calls `_validate(true)`) can query this flag to confirm poly load occurred. Visible indirectly as the node tile turning from red (error) to green (healthy) in the Nuke DAG after a valid `.fit` file is supplied.
- **`error()` on load failure**: `_validate(true)` calls `Op::error("Cannot open lens file: %s", _poly_file)` when `poly_system_read` returns non-zero. Nuke surfaces this as a red error tile and a descriptive message in the DAG and script log — inspectable without a debugger. The error string includes the file path for direct diagnosis.
- **`renderStripe` black output**: Until S02, `renderStripe` writes all-zeros. Agents can verify flat black by sampling the output image; any non-zero pixel value indicates unintended code regression.
- **Flat-black compile-time proof**: `g++ -fsyntax-only` on this file (via `scripts/verify-s01-syntax.sh`) exits 0 on success, prints a compiler error on failure — the first inspectable CI failure surface.
- **Knob state**: `_poly_file`, `_focus_distance`, `_fstop`, `_aperture_samples` are all directly readable from the Nuke node's knob panel after construction — no internal state is hidden from the UI.

**Failure inspection procedure:**

1. If node tile is red: read the error message from the Nuke DAG; it contains the file path that `poly_system_read` could not open.
2. If output is not black: the stub `renderStripe` has been inadvertently replaced. Compare against the `// S02: replace with PO scatter loop` comment.
3. If `g++ -fsyntax-only` fails: the compiler error message identifies the exact file and line. All deviations from this plan (e.g. missing includes, wrong base class, wrong method signatures) produce distinct compiler errors.
4. If `_poly_loaded` is never `true` at runtime: check that the `File_knob` path is set and the `.fit` file is readable by the process. `poly_system_read` returns 1 on any I/O or alloc failure; the `error()` call will have fired.

**No new persistent or external signals** (no file writes, no network, no subprocess) are introduced by this task at runtime.
