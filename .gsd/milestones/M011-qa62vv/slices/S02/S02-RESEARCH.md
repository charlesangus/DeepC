# S02 Research: Wire holdout through full stack and integration test

**Calibration:** Light-to-targeted. The Rust side is entirely complete from S01. S02 is C++ wiring (~50 lines) + an integration test .nk update (~25 lines) + Docker build verification. No new technology, no ambiguous architecture. The primary job is confirming the exact C++ HoldoutData encoding, then removing the wrong post-defocus approach already sketched in DeepCOpenDefocus.cpp.

---

## Summary

S01 delivered everything on the Rust side:
- `opendefocus_deep_render_holdout()` FFI entry point exists and works (unit-tested)
- `HoldoutData` C struct defined in both header and Rust
- `holdout_transmittance()` in `render_impl` applies depth-correct T attenuation per layer
- `HoldoutData` C struct layout: `data[i*4 .. i*4+4]` = `(d0, T0, d1, T1)` per pixel

`DeepCOpenDefocus.cpp` has a **placeholder S03 block** that does the *wrong* thing: it calls `opendefocus_deep_render()` (no holdout), then applies `product(1-alpha)` as a scalar post-render multiply. This is the pre-existing incorrect post-defocus approach — blurs holdout edges. S02 must replace this.

---

## Implementation Landscape

### Files that change

| File | What changes |
|------|-------------|
| `src/DeepCOpenDefocus.cpp` | Remove `computeHoldoutTransmittance()`. Add `buildHoldoutData()`. Switch from `opendefocus_deep_render` to conditional `opendefocus_deep_render_holdout`. Remove S03 post-render block. |
| `test/test_opendefocus_holdout.nk` | Add green z=2 subject (in-front, uncut) + red z=5 subject (behind holdout, cut). DeepMerge both into input 0. Holdout stays on input 1. |

### Files that do NOT change
- `crates/opendefocus-deep/src/lib.rs` — complete, unit-tested, no changes needed
- `crates/opendefocus-deep/include/opendefocus_deep.h` — header is correct
- All four opendefocus crate sources — no changes needed
- `src/CMakeLists.txt` — links correctly against `libopendefocus_deep.a`
- `Cargo.toml` — `[patch.crates-io]` section is correct

---

## C++ HoldoutData Encoding Contract

The Rust `holdout_transmittance()` function (in `lib.rs`, lines ~152-165) implements:
```
if z > d1  → T1
if z > d0  → T0
else       → 1.0
```

So `d0` = shallowest holdout surface depth, `d1` = deepest holdout surface depth.
For a single-layer holdout (typical case):  `d0 = d1 = first sample depth`, `T0 = T1 = cumulative_T`.

The C++ `buildHoldoutData()` function must:
1. For each pixel: collect holdout samples sorted by `DeepFront` ascending
2. Compute `T = product of (1 - alpha_i)` across all samples (front-to-back)
3. Encode:
   - No samples → `[0.0, 1.0, 0.0, 1.0]` (T=1 at all depths)
   - Samples present → `[d_front, T_total, d_back, T_total]` where `d_front` = first sample's `DeepFront`, `d_back` = last sample's `DeepFront`
4. Build `HoldoutData { data: flat_vec.data(), len: width*height*4 }`

Alpha guard: clamp alpha to [0, 1] before the `(1-alpha)` multiply, matching `computeHoldoutTransmittance()`.

---

## C++ Wiring Pattern

```cpp
// After collecting sampleData and output_buf:
if (DeepOp* holdoutIn = dynamic_cast<DeepOp*>(Op::input(1))) {
    // Build flat HoldoutData from holdout deep plane
    ChannelSet holdoutChans;
    holdoutChans += Chan_Alpha;
    holdoutChans += Chan_DeepFront;
    DeepPlane holdoutPlane;
    holdoutIn->deepEngine(bounds, holdoutChans, holdoutPlane);

    std::vector<float> holdout_flat = buildHoldoutData(holdoutPlane, bounds, width, height);
    HoldoutData holdout_ffi { holdout_flat.data(), (uint32_t)holdout_flat.size() };

    opendefocus_deep_render_holdout(&sampleData, output_buf.data(),
                                    width, height, &lensParams, (int)_use_gpu,
                                    &holdout_ffi);
} else {
    opendefocus_deep_render(&sampleData, output_buf.data(),
                            width, height, &lensParams, (int)_use_gpu);
}
// Remove entire S03 post-render block
```

The `buildHoldoutData` free function iterates pixels in the same y-outer, x-inner scan order as the main sample flatten loop to guarantee pixel index alignment.

---

## Integration Test .nk Update

Current `test/test_opendefocus_holdout.nk` has:
- Holdout: black, alpha=0.5, depth z=3.0 → input 1
- Subject: grey, alpha=1.0, depth z=5.0 → input 0

Missing: a z=2 in-front subject to prove depth-selectivity.

Updated test needs:
- `DeepCConstant_back`: red `{1.0, 0.0, 0.0, 1.0}` at z=5 (behind holdout → cut)
- `DeepCConstant_front`: green `{0.0, 1.0, 0.0, 1.0}` at z=2 (in front of holdout → uncut)
- `DeepMerge` combining both → input 0 of DeepCOpenDefocus
- `DeepCConstant_holdout` unchanged → input 1

In Nuke .nk format, `inputs 2` connects to the 2 most recently defined nodes (last = input 0, second-to-last = input 1). Order in the file must be: holdout constant first, then DeepMerge last → DeepCOpenDefocus `inputs 2`.

Write node outputs `test/output/holdout_depth_selective.exr`. Pass criterion: output at holdout pixels shows strong green channel (z=2 uncut), weak red channel (z=5 attenuated by T≈0.5).

---

## Verification Commands

```bash
# 1. Rust unit tests must still pass
PATH=$HOME/.cargo/bin:$PATH PROTOC=/tmp/protoc_dir/bin/protoc \
  cargo test -p opendefocus-deep -- holdout

# 2. C++ syntax check (fast)
bash scripts/verify-s01-syntax.sh 2>&1 | tail -5

# 3. Docker build (authoritative — compiles against real Nuke SDK)
bash docker-build.sh --linux --versions 16.0
```

Note: `nuke -x test/test_opendefocus_holdout.nk` requires a local Nuke 16.0 installation. The Docker build confirms compilation; visual verification requires a Nuke-available environment.

---

## Known Constraints

1. **protoc path**: All `cargo` commands require `PROTOC=/tmp/protoc_dir/bin/protoc` — may need re-fetch if `/tmp` was cleared between sessions.
2. **GPU holdout deferred**: The wgpu runner ignores `_holdout` — only CPU path is active. This is correct per S01 decision. No GPU work needed for S02.
3. **S03 comment tag**: The existing S03 block in the C++ is the wrong post-defocus approach and must be removed entirely in S02. The S03 label was a placeholder annotation, not a future task.
4. **Pixel scan order alignment**: `buildHoldoutData()` must iterate `y` outer, `x` inner over `[bounds.y()..bounds.t()) × [bounds.x()..bounds.r())` — same order as the sample flatten loop — to guarantee `pixel_idx` alignment between `sampleData` and `HoldoutData`.

---

## Task Decomposition Recommendation

**T01 — Fix C++ HoldoutData wiring** (~50 lines)
- Remove `computeHoldoutTransmittance` free function
- Add `buildHoldoutData(const DeepPlane&, const Box&, int w, int h) → std::vector<float>` free function
- Replace S03 post-render block with conditional `opendefocus_deep_render_holdout` / `opendefocus_deep_render` call
- Files: `src/DeepCOpenDefocus.cpp`
- Verify: `cargo test -p opendefocus-deep -- holdout` (Rust still passes), then `bash scripts/verify-s01-syntax.sh`

**T02 — Update integration test .nk for depth-selective holdout**
- Add red z=5 and green z=2 subjects, DeepMerge them, update Write output path
- Files: `test/test_opendefocus_holdout.nk`
- Verify: script is syntactically valid (check node count, connections)

**T03 — Docker build**
- Run `bash docker-build.sh --linux --versions 16.0`
- Exit 0 is the pass criterion
- Files: none (read-only for this task)

---

## Requirements Targeted

- **R049** — CPU path applies depth-correct T attenuation during layer-peel: already satisfied in Rust (S01). S02 wires the C++ caller to use `opendefocus_deep_render_holdout` so R049 is exercised end-to-end.
- **R057** — Fork established: satisfied in S01. S02 confirms via Docker build that the fork compiles in the production build environment.
