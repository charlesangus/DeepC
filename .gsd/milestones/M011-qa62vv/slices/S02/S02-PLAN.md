# S02: Wire holdout through full stack and integration test

**Goal:** Wire the C++ DeepCOpenDefocus node to use `opendefocus_deep_render_holdout` (built in S01) instead of the incorrect post-defocus scalar multiply. Update the integration test .nk to verify depth-selective holdout (z=2 passes through, z=5 is attenuated). Confirm the fork compiles in the production Docker build environment.
**Demo:** After this: DeepCOpenDefocus in Nuke: holdout test shows z=5 disc cut sharply by holdout at z=3, z=2 disc passes through uncut. Docker build exits 0.

## Tasks
- [x] **T01: Replaced incorrect post-defocus scalar holdout multiply with depth-correct opendefocus_deep_render_holdout dispatch via new buildHoldoutData() helper in DeepCOpenDefocus.cpp** — Replace the incorrect post-defocus holdout approach with the depth-correct `opendefocus_deep_render_holdout` path.

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
  - Estimate: 45m
  - Files: src/DeepCOpenDefocus.cpp
  - Verify: PATH=$HOME/.cargo/bin:$PATH PROTOC=/tmp/protoc_dir/bin/protoc cargo test -p opendefocus-deep -- holdout 2>&1 | grep -q 'test result: ok' && bash scripts/verify-s01-syntax.sh 2>&1 | tail -3
- [x] **T02: Rewrote test/test_opendefocus_holdout.nk with two subjects at z=2 (green, passes through) and z=5 (red, attenuated), DeepMerge node, and Write to holdout_depth_selective.exr** — Rewrite `test/test_opendefocus_holdout.nk` to contain two subjects at different depths, proving depth-selective holdout behaviour:
- z=5 red subject (behind holdout z=3 → attenuated by T≈0.5)
- z=2 green subject (in front of holdout z=3 → passes through uncut)

Nuke .nk stack ordering (LIFO — last defined before `inputs N` = input 0):
1. `DeepCConstant_holdout` — black, alpha=0.5, front=3.0, back=3.5 (unchanged from original)
2. `DeepCConstant_back` — red {1.0, 0.0, 0.0, 1.0}, front=5.0, back=5.5 (new)
3. `DeepCConstant_front` — green {0.0, 1.0, 0.0, 1.0}, front=2.0, back=2.5 (new)
4. `DeepMerge { inputs 2 }` — merges front(input0) and back(input1); stack after: [merge, holdout]
5. `DeepCOpenDefocus { inputs 2 }` — merge=input0, holdout=input1 ✓
6. `Write { inputs 1 }` → `test/output/holdout_depth_selective.exr`

This stack ordering guarantees: after DeepMerge consumes front+back, DeepCOpenDefocus sees merge as input 0 and holdout as input 1.

The file replaces the existing `test/test_opendefocus_holdout.nk` entirely. Keep the same Root block (format DeepTest128x72). Use `DeepMerge` Nuke node (standard deep compositing node).
  - Estimate: 20m
  - Files: test/test_opendefocus_holdout.nk
  - Verify: grep -q 'DeepMerge' test/test_opendefocus_holdout.nk && grep -q 'holdout_depth_selective.exr' test/test_opendefocus_holdout.nk && grep -q '2.0' test/test_opendefocus_holdout.nk && grep -q '5.0' test/test_opendefocus_holdout.nk
- [x] **T03: Docker build bash docker-build.sh --linux --versions 16.0 exited 0; all four forked Rust crates compiled and DeepCOpenDefocus.so linked in nukedockerbuild:16.0-linux** — Run the Docker build to confirm the full Cargo workspace (including all four forked opendefocus crates) and CMake build against the real Nuke 16.0 SDK both succeed. Exit 0 is the only pass criterion.

Command: `bash docker-build.sh --linux --versions 16.0`

Expected log lines confirming success:
- `Compiling opendefocus-kernel`
- `Compiling opendefocus-deep`
- `Linking CXX shared library ... DeepCOpenDefocus.so` (or similar)
- Docker container exits 0

If docker is not available or nukedockerbuild image is not present, document the failure clearly and check that `bash scripts/verify-s01-syntax.sh` passes as a minimum signal.

Note: protoc must be available at `/tmp/protoc_dir/bin/protoc`. If `/tmp` was cleared, re-fetch it: `mkdir -p /tmp/protoc_dir && curl -L https://github.com/protocolbuffers/protobuf/releases/download/v29.3/protoc-29.3-linux-x86_64.zip -o /tmp/protoc.zip && unzip -o /tmp/protoc.zip -d /tmp/protoc_dir`
  - Estimate: 30m
  - Verify: bash docker-build.sh --linux --versions 16.0; echo "docker exit: $?"
