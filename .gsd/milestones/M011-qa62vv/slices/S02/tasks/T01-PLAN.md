---
estimated_steps: 23
estimated_files: 1
skills_used: []
---

# T01: Fix C++ HoldoutData wiring in DeepCOpenDefocus.cpp

Replace the incorrect post-defocus holdout approach with the depth-correct `opendefocus_deep_render_holdout` path.

The current code: (1) always calls `opendefocus_deep_render()`, then (2) in an S03 block applies `computeHoldoutTransmittance()` as a scalar post-multiply. This blurs holdout edges and is wrong.

New code:
- Remove `computeHoldoutTransmittance()` free function entirely.
- Add `buildHoldoutData(const DeepPlane& plane, const Box& bounds, int w, int h) → std::vector<float>` free function.
- In `renderStripe()`: check `dynamic_cast<DeepOp*>(Op::input(1))`. If connected: call `buildHoldoutData()`, build `HoldoutData` struct, call `opendefocus_deep_render_holdout(...)`. Else: call `opendefocus_deep_render(...)` unchanged.
- Remove the entire S03 post-render block.

`buildHoldoutData()` implementation contract:
1. Iterate y outer (`bounds.y()` to `bounds.t()`) × x inner (`bounds.x()` to `bounds.r()`) — same scan order as the sample flatten loop.
2. For each pixel: call `holdoutPlane.getPixel(y, x)`, sort samples by `Chan_DeepFront` ascending, compute `T = product of (1 - clamp(alpha, 0, 1))` front-to-back.
3. Encode into 4 floats:
   - No samples: `[0.0f, 1.0f, 0.0f, 1.0f]` (T=1 everywhere)
   - Samples present: `[d_front, T_total, d_back, T_total]` where `d_front` = first sample's `DeepFront`, `d_back` = last sample's `DeepFront`.
4. Return flat `std::vector<float>` of size `width * height * 4`.

`opendefocus_deep_render_holdout` call site (after building `HoldoutData`):
```cpp
std::vector<float> holdout_flat = buildHoldoutData(holdoutPlane, bounds, width, height);
HoldoutData holdout_ffi { holdout_flat.data(), (uint32_t)holdout_flat.size() };
opendefocus_deep_render_holdout(&sampleData, output_buf.data(),
    (uint32_t)width, (uint32_t)height, &lensParams, (int)_use_gpu, &holdout_ffi);
```
Keep the `fprintf(stderr, ...)` holdout log line inside the new holdout branch.

Note: `opendefocus_deep_render_holdout` requires Chan_DeepFront on the holdout plane — request it in `holdoutChans`. The existing S03 block already does this.

## Inputs

- ``src/DeepCOpenDefocus.cpp` — current source with wrong post-defocus approach`
- ``crates/opendefocus-deep/include/opendefocus_deep.h` — HoldoutData struct and opendefocus_deep_render_holdout declaration`

## Expected Output

- ``src/DeepCOpenDefocus.cpp` — updated with buildHoldoutData() and conditional opendefocus_deep_render_holdout / opendefocus_deep_render dispatch`

## Verification

PATH=$HOME/.cargo/bin:$PATH PROTOC=/tmp/protoc_dir/bin/protoc cargo test -p opendefocus-deep -- holdout 2>&1 | grep -q 'test result: ok' && bash scripts/verify-s01-syntax.sh 2>&1 | tail -3

## Observability Impact

The existing stderr log line `DeepCOpenDefocus: holdout attenuation applied (%d pixels)` moves into the new `opendefocus_deep_render_holdout` branch, preserving visibility of when holdout is active.
