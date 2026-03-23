---
estimated_steps: 4
estimated_files: 2
skills_used:
  - review
---

# T03: CMake integration and syntax verifier update

**Slice:** S01 — Iop Scaffold, Poly Loader, and Architecture Decision
**Milestone:** M006

## Description

Two wiring tasks that make the plugin buildable and verifiable:

1. **`src/CMakeLists.txt`**: Add `DeepCDefocusPO` to the `PLUGINS` list (non-wrapped; no DeepCWrapper dependency) and to the `FILTER_NODES` list (puts the node in the FILTER menu category in Nuke). No other changes needed — the existing `foreach(PLUGIN_NAME ${PLUGINS})` loop handles the rest.

2. **`scripts/verify-s01-syntax.sh`**: The existing script has no mock stubs for `PlanarIop`, `ImagePlane`, or `File_knob`. Adding `DeepCDefocusPO.cpp` to the check loop would fail at `g++ -fsyntax-only` without these mocks. Add them, and add the new file to the loop.

The mock stubs needed are minimal — they only need to compile, not to reproduce the full NDK API. The existing style (inline stubs in `HEADER` heredocs under `$TMPDIR/DDImage/`) should be followed exactly.

## Steps

1. In `src/CMakeLists.txt`, add `DeepCDefocusPO` to the `PLUGINS` list (after `DeepCDepthBlur` is a natural position). Add `DeepCDefocusPO` to the `FILTER_NODES` list (after `DeepCDepthBlur`). These are the only two changes to CMakeLists.txt.

2. In `scripts/verify-s01-syntax.sh`, add a new heredoc block for `DDImage/Iop.h` (if not already present — check first). The `Iop` base class needs: `info_` member (`class Info { ... } info_;`), `set_out_channels(ChannelMask)`, `copy_info()`. Note: the existing `DeepFilterOp.h` mock already includes `Op.h` — `Iop.h` builds on `Op.h`.

3. Add a heredoc block for `DDImage/PlanarIop.h`. Minimal PlanarIop:
   ```cpp
   #pragma once
   #include "Iop.h"
   #include "ImagePlane.h"
   namespace DD { namespace Image {
   struct RequestOutput {};
   class PlanarIop : public Iop {
   public:
       PlanarIop(Node* n) : Iop(n) {}
       virtual ~PlanarIop() {}
       virtual void renderStripe(ImagePlane& plane) = 0;
       virtual void getRequests(const Box&, const ChannelSet&, int, RequestOutput&) const {}
       virtual int knob_changed(Knob* k) { return Iop::knob_changed(k); }
       virtual void _close() {}
   };
   }} // DD::Image
   ```
   Note: `Iop::knob_changed` must be declared in the Iop mock too (see step 2).

4. Add a heredoc block for `DDImage/ImagePlane.h`. Minimal ImagePlane:
   ```cpp
   #pragma once
   #include "Box.h"
   #include "Channel.h"
   namespace DD { namespace Image {
   class ImagePlane {
   public:
       ImagePlane() {}
       const Box& bounds() const { static Box b; return b; }
       const ChannelSet& channels() const { static ChannelSet c; return c; }
       void makeWritable() {}
       float* writable(Channel, int /*y*/) { return nullptr; }
   };
   }} // DD::Image
   ```

5. In the existing `DDImage/Knobs.h` heredoc, add the `File_knob` stub:
   ```cpp
   inline Knob* File_knob(Knob_Callback, const char**, const char*, const char* = nullptr) { return nullptr; }
   ```
   Important: the second parameter is `const char**` (pointer to `const char*`), matching the NDK signature for `File_knob`. This is different from `Float_knob` which takes `float*`.

6. Add `DeepCDefocusPO.cpp` to the syntax-check loop in the script, alongside the existing entries. The loop currently ends with `DeepCDepthBlur.cpp` — add `DeepCDefocusPO.cpp` after it.

7. Also add `DDImage/DeepOp.h` stub if not already present (the existing `DeepFilterOp.h` mock embeds `DeepOp` inline — check if a separate `DeepOp.h` is needed for the `#include "DDImage/DeepOp.h"` in `DeepCDefocusPO.cpp`). If needed, create a minimal stub that `#include`s `DeepFilterOp.h` or re-declares `DeepOp`.

## Must-Haves

- [ ] `DeepCDefocusPO` appears in `PLUGINS` list in `src/CMakeLists.txt`
- [ ] `DeepCDefocusPO` appears in `FILTER_NODES` list in `src/CMakeLists.txt`
- [ ] `DDImage/PlanarIop.h` mock stub added to `scripts/verify-s01-syntax.sh`
- [ ] `DDImage/ImagePlane.h` mock stub added to `scripts/verify-s01-syntax.sh`
- [ ] `File_knob` stub added to `DDImage/Knobs.h` heredoc in the script (with `const char**` parameter)
- [ ] `DeepCDefocusPO.cpp` added to the syntax-check loop in the script
- [ ] Existing syntax checks (DeepCBlur.cpp, DeepCDepthBlur.cpp) still pass after changes

## Verification

```bash
# CMake changes
grep -c 'DeepCDefocusPO' src/CMakeLists.txt    # should output 2 (PLUGINS + FILTER_NODES)

# Script changes
grep -q 'PlanarIop'        scripts/verify-s01-syntax.sh
grep -q 'ImagePlane'       scripts/verify-s01-syntax.sh
grep -q 'File_knob'        scripts/verify-s01-syntax.sh
grep -q 'DeepCDefocusPO'   scripts/verify-s01-syntax.sh

# Existing checks still pass
bash scripts/verify-s01-syntax.sh  # must exit 0 overall
```

## Inputs

- `src/CMakeLists.txt` — existing build config to modify (add plugin to PLUGINS and FILTER_NODES lists)
- `scripts/verify-s01-syntax.sh` — existing syntax verifier to extend (add new mock stubs + new file)
- `src/DeepCDefocusPO.cpp` — the file being added to the syntax check loop (produced by T02)

## Expected Output

- `src/CMakeLists.txt` — with `DeepCDefocusPO` in PLUGINS and FILTER_NODES
- `scripts/verify-s01-syntax.sh` — with PlanarIop/ImagePlane/File_knob mocks and DeepCDefocusPO.cpp in the loop
