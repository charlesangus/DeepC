---
estimated_steps: 5
estimated_files: 2
skills_used:
  - best-practices
---

# T01: Implement depth-aware holdout weighting in DeepCDefocusPO

**Slice:** S03 — Depth-Aware Holdout
**Milestone:** M006

## Description

S02 left the scatter engine fully working but with no holdout integration. The holdout input (input 1) is already wired as `Op::input(1)` with `input_label` returning `"holdout"` from S01, but `getRequests` never requests it and `renderStripe` never fetches or uses it.

This task makes four surgical edits to `src/DeepCDefocusPO.cpp` and extends `scripts/verify-s01-syntax.sh` with S03 grep contracts.

**Key architectural invariant (R024):** The holdout is evaluated at the *output* pixel position after scatter, not at the input pixel position before scatter. This keeps the holdout mask sharp. The holdout contributes no colour — only transmittance used as a weight multiplier.

**Transmittance formula (R023):** Standard deep-over front-to-back accumulation:
```
T = product of (1 - alpha_i) for all holdout samples where zFront_i < Z
```
This is order-independent (commutative product), so `getUnorderedSample` is correct — no sort needed.

## Steps

1. **Edit `getRequests`** — after the `input0()->deepRequest(...)` block, add a null-guarded `holdoutOp` request for depth and alpha channels only:
   ```cpp
   DeepOp* holdoutOp = dynamic_cast<DeepOp*>(Op::input(1));
   if (holdoutOp)
       holdoutOp->deepRequest(box, ChannelSet(Chan_DeepFront) + Chan_DeepBack + Chan_Alpha, count);
   ```
   Note: avoid `ChannelSet(Mask_Alpha)` constructor — `Mask_Alpha` is not in the mock header. Use `ChannelSet needed_h; needed_h += Chan_Alpha; needed_h += Chan_DeepFront; needed_h += Chan_DeepBack;` and pass `needed_h` instead, or build up with `+= Chan_Alpha` inline.

2. **Edit `renderStripe` — fetch holdout plane** — immediately after the `if (!input0()->deepEngine(...)) return;` block (step 1 in the scatter engine), insert:
   ```cpp
   DeepOp* holdoutOp = dynamic_cast<DeepOp*>(Op::input(1));
   DeepPlane holdoutPlane;
   bool holdoutConnected = false;
   if (holdoutOp) {
       ChannelSet hNeeded;
       hNeeded += Chan_Alpha;
       hNeeded += Chan_DeepFront;
       hNeeded += Chan_DeepBack;
       holdoutConnected = holdoutOp->deepEngine(bounds, hNeeded, holdoutPlane);
   }
   ```
   If `deepEngine` returns false, `holdoutConnected` stays false → `holdout_w = 1.0f` throughout (no masking). Place this *after* the primary fetch but *before* the zero-fill loop (step 2).

3. **Edit `renderStripe` — add `transmittance_at` lambda** — immediately after the `holdoutConnected` declaration and before the scatter loop (before `for (Box::iterator ...)`):
   ```cpp
   auto transmittance_at = [&](int ox, int oy, float Z) -> float {
       if (!holdoutConnected) return 1.0f;
       DeepPixel hpx = holdoutPlane.getPixel(oy, ox);  // note: y first
       const int nH = hpx.getSampleCount();
       if (nH == 0) return 1.0f;
       float T = 1.0f;
       for (int h = 0; h < nH; ++h) {
           const float hzf = hpx.getUnorderedSample(h, Chan_DeepFront);
           if (hzf >= Z) continue;
           const float ha = hpx.getUnorderedSample(h, Chan_Alpha);
           T *= (1.0f - ha);
       }
       return std::max(0.0f, T);
   };
   ```
   `getPixel(oy, ox)` — DDImage takes y first. Captures `holdoutConnected` and `holdoutPlane` by reference.

4. **Edit `renderStripe` — apply `holdout_w` in splat** — in the RGB splat loop (step 8), change:
   ```cpp
   const float chan_val = pixel.getUnorderedSample(s, rgb_chans[c]);
   const float w = transmit[c] / static_cast<float>(N);
   imagePlane.writableAt(out_xi, out_yi, rgb_chans[c])
       += chan_val * alpha * w;
   ```
   to:
   ```cpp
   const float chan_val = pixel.getUnorderedSample(s, rgb_chans[c]);
   const float w = transmit[c] / static_cast<float>(N);
   const float holdout_w = transmittance_at(out_xi, out_yi, depth);
   imagePlane.writableAt(out_xi, out_yi, rgb_chans[c])
       += chan_val * alpha * w * holdout_w;
   ```
   And in the alpha splat block, change:
   ```cpp
   const float w = transmit[1] / static_cast<float>(N);
   imagePlane.writableAt(out_xi, out_yi, Chan_Alpha)
       += alpha * w;
   ```
   to:
   ```cpp
   const float w = transmit[1] / static_cast<float>(N);
   const float holdout_w = transmittance_at(out_xi, out_yi, depth);
   imagePlane.writableAt(out_xi, out_yi, Chan_Alpha)
       += alpha * w * holdout_w;
   ```
   `out_xi`/`out_yi` are the *output* pixel coords (after Newton trace), not the input `px`/`py`. `depth` is already in scope (`const float depth = 0.5f * (z_front + z_back)`).

5. **Extend `scripts/verify-s01-syntax.sh`** — append before the final `echo "All syntax checks passed."` line:
   ```bash
   echo "Checking S03 contracts..."
   grep -q 'holdoutConnected'      "$SRC_DIR/DeepCDefocusPO.cpp" || { echo "FAIL: holdoutConnected missing"; exit 1; }
   grep -q 'transmittance_at'      "$SRC_DIR/DeepCDefocusPO.cpp" || { echo "FAIL: transmittance_at lambda missing"; exit 1; }
   grep -q 'holdout_w'             "$SRC_DIR/DeepCDefocusPO.cpp" || { echo "FAIL: holdout_w factor missing from splat"; exit 1; }
   grep -q 'hzf >= Z'              "$SRC_DIR/DeepCDefocusPO.cpp" || { echo "FAIL: depth gate (hzf >= Z) missing"; exit 1; }
   grep -q 'holdoutOp->deepRequest\|holdout.*deepRequest' "$SRC_DIR/DeepCDefocusPO.cpp" || { echo "FAIL: holdout deepRequest missing from getRequests"; exit 1; }
   test "$(grep -c 'input(1)' "$SRC_DIR/DeepCDefocusPO.cpp")" -ge 3 || { echo "FAIL: input(1) appears fewer than 3 times (label + getRequests + renderStripe)"; exit 1; }
   ! grep -q 'TODO\|STUB' "$SRC_DIR/DeepCDefocusPO.cpp" || { echo "FAIL: leftover TODO/STUB markers"; exit 1; }
   echo "S03 contracts: all pass."
   ```
   Run `bash scripts/verify-s01-syntax.sh` to confirm everything passes.

## Must-Haves

- [ ] `getRequests` null-checks `Op::input(1)` and calls `holdoutOp->deepRequest` for depth+alpha only
- [ ] `renderStripe` fetches `holdoutPlane` at `bounds` (output-pixel coordinates) — not at input pixel position
- [ ] `transmittance_at` lambda uses `getUnorderedSample` + `hzf >= Z` gate (samples at-or-behind Z do not attenuate)
- [ ] `holdout_w` multiplied into every `writableAt +=` — both the RGB channel loop and the alpha block
- [ ] `holdoutConnected = false` path results in `holdout_w = 1.0f` (no masking when input is disconnected)
- [ ] `scripts/verify-s01-syntax.sh` extended with S03 contracts; all contracts pass
- [ ] `bash scripts/verify-s01-syntax.sh` exits 0

## Verification

```bash
bash scripts/verify-s01-syntax.sh
```

Also confirm each contract individually:

```bash
grep -q 'holdoutConnected'                              src/DeepCDefocusPO.cpp && echo OK
grep -q 'transmittance_at'                              src/DeepCDefocusPO.cpp && echo OK
grep -q 'holdout_w'                                     src/DeepCDefocusPO.cpp && echo OK
grep -q 'hzf >= Z'                                      src/DeepCDefocusPO.cpp && echo OK
grep -q 'holdoutOp->deepRequest\|holdout.*deepRequest'  src/DeepCDefocusPO.cpp && echo OK
test "$(grep -c 'input(1)' src/DeepCDefocusPO.cpp)" -ge 3 && echo OK
! grep -q 'TODO\|STUB' src/DeepCDefocusPO.cpp && echo OK
```

## Inputs

- `src/DeepCDefocusPO.cpp` — S02 scatter engine; `getRequests` and `renderStripe` need the four holdout edits applied
- `scripts/verify-s01-syntax.sh` — existing S01+S02 syntax + grep contract script; S03 contracts to be appended

## Expected Output

- `src/DeepCDefocusPO.cpp` — modified with holdout request, fetch, transmittance lambda, and `holdout_w` splat weighting
- `scripts/verify-s01-syntax.sh` — modified with S03 grep contracts appended before the final success echo
