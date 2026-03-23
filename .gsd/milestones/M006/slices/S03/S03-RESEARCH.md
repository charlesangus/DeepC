# S03: Depth-Aware Holdout — Research

**Milestone:** M006 — DeepCDefocusPO  
**Slice:** S03  
**Requirements owned:** R023, R024  
**Research type:** Targeted — known patterns, clear insertion point, no unfamiliar APIs

---

## Summary

S03 is **low-risk, single-file work**. The holdout scaffold (input 1 wired, `input_label` returning "holdout") already exists in `DeepCDefocusPO.cpp` from S01. The S02 scatter loop's insertion point for holdout weighting is explicitly documented in the S02 summary. No new data structures are needed. No new files need to be created.

The entire slice reduces to four coordinated edits to `src/DeepCDefocusPO.cpp` plus adding S03 grep contracts to `scripts/verify-s01-syntax.sh`.

---

## Requirements Targeted

### R023 — Depth-aware holdout at output pixel
> For each output pixel, each main input sample's contribution is weighted by the holdout's transmittance at that pixel at the sample's depth. Transmittance accumulated front-to-back through all holdout samples.

**Gap:** `renderStripe` currently accumulates with weight `transmit[c] / N * alpha * chan_val` — no holdout factor. R023 adds `holdout_w` to this multiply.

### R024 — Holdout never defocused, no colour contribution
> Holdout contributes no colour. Not scattered through lens. Transmittance evaluated at output pixel position. Holdout mask is sharp.

**Gap:** Holdout DeepOp is not fetched at all in `renderStripe`. No transmittance is computed. R024 is satisfied by the architecture: holdout is fetched at the *output* pixel position (not the input/source pixel), transmittance evaluated there at the sample's depth, then discarded. The holdout contributes no colour.

---

## Implementation Landscape

### Files touched

| File | Change |
|------|--------|
| `src/DeepCDefocusPO.cpp` | 4 surgical edits (details below) |
| `scripts/verify-s01-syntax.sh` | Add S03 grep contracts at the end |

**No new files. No new headers. No changes to `deepc_po_math.h` or `poly.h`.**

### What already exists (from S01/S02)

From `DeepCDefocusPO.cpp`:
- `inputs(2)` — both inputs declared in constructor (line ~95)
- `test_input` — accepts any `DeepOp` for both inputs 0 and 1
- `input_label(1)` → `"holdout"` — wired and correct
- `Op::input(1)` — available; null when holdout is not connected

The S02 summary's "What S03 Needs to Know" section specifies exact insertion points.

---

## Transmittance Algorithm

Standard deep-over front-to-back transmittance at a given depth Z:

```
transmittance = 1.0
for each holdout sample i (sorted front-to-back by zFront):
    if holdout_sample[i].zFront >= Z:
        break
    transmittance *= (1 - holdout_sample[i].alpha)
```

This is identical to Nuke's native DeepHoldout node semantics:
- Samples **at or behind** depth Z do not attenuate (their `zFront >= Z`)
- Samples **in front of** depth Z attenuate by `(1 - alpha_i)` each
- Holdout alpha of 1.0 at a depth shallower than Z → `holdout_w = 0` → contribution fully masked
- No holdout connected → `holdout_w = 1.0` → full contribution (identity, no change)

**Important:** The depth Z to use for each main input contribution is `depth = 0.5 * (z_front + z_back)` of the *main* sample — already computed in the per-deep-sample loop at line ~328 as `const float depth`.

**Important:** The holdout DeepPlane is fetched at `bounds` (the *output* stripe box), not at the input pixel position. This is deliberate (R024): the holdout is evaluated in output-pixel space, not pre-applied before scatter.

---

## The Four Edits

### Edit 1 — `getRequests`: add holdout DeepOp request

**Current** (lines ~230–242):
```cpp
void getRequests(const Box& box, const ChannelSet& channels,
                 int count, RequestOutput& reqData) const override
{
    if (input0())
        input0()->deepRequest(
            box, channels + Chan_DeepFront + Chan_DeepBack + Chan_Alpha, count);
}
```

**Add after the `input0()` request:**
```cpp
    // Holdout input request (optional — null-checked).
    DeepOp* holdoutOp = dynamic_cast<DeepOp*>(Op::input(1));
    if (holdoutOp)
        holdoutOp->deepRequest(
            box, ChannelSet(Mask_Alpha) + Chan_DeepFront + Chan_DeepBack, count);
```

The holdout only needs depth channels and alpha. No RGB channels required (R024: holdout contributes no colour).

### Edit 2 — `renderStripe`: fetch holdout DeepPlane after primary fetch

**Current location:** immediately after step 1 (`deepEngine` call), before step 2 (zero buffer).

**Insert:**
```cpp
        // Fetch optional holdout input (R023/R024).
        // Fetched at output pixel bounds — NOT at source pixel.
        // Null if holdout input is not connected.
        DeepOp* holdoutOp = dynamic_cast<DeepOp*>(Op::input(1));
        DeepPlane holdoutPlane;
        bool holdoutConnected = false;
        if (holdoutOp) {
            ChannelSet hNeeded(Mask_Alpha);
            hNeeded += Chan_DeepFront;
            hNeeded += Chan_DeepBack;
            holdoutConnected = holdoutOp->deepEngine(bounds, hNeeded, holdoutPlane);
        }
```

If `holdoutOp->deepEngine` returns `false`, `holdoutConnected` stays false and the engine continues normally with `holdout_w = 1.0` (full contribution). This matches the S02 pattern for primary fetch failure: graceful degradation, no crash.

### Edit 3 — `renderStripe`: helper lambda for transmittance lookup

**Location:** immediately after the `holdoutConnected` declaration, before the scatter loop.

```cpp
        // transmittance_at: returns holdout transmittance at output pixel (ox, oy)
        // for a main sample at depth Z.
        // When holdout not connected: returns 1.0 (identity — no masking).
        // Accumulates front-to-back: T = product of (1 - alpha_i) for all
        // holdout samples with zFront_i < Z. (Nuke DeepHoldout semantics.)
        auto transmittance_at = [&](int ox, int oy, float Z) -> float {
            if (!holdoutConnected) return 1.0f;
            DeepPixel hpx = holdoutPlane.getPixel(oy, ox);
            const int nH = hpx.getSampleCount();
            if (nH == 0) return 1.0f;
            float T = 1.0f;
            for (int h = 0; h < nH; ++h) {
                const float hzf = hpx.getUnorderedSample(h, Chan_DeepFront);
                if (hzf >= Z) continue;  // sample at or behind Z — no contribution
                const float ha = hpx.getUnorderedSample(h, Chan_Alpha);
                T *= (1.0f - ha);
            }
            return std::max(0.0f, T);
        };
```

**Notes on the lambda:**
- Uses `getUnorderedSample` — same as primary input. No sort required: the formula is commutative over unordered samples (product of `(1-alpha)` terms is order-independent).
- The `hzf >= Z` condition implements "samples at or behind Z do not attenuate". This is the standard DeepHoldout formula.
- Returns `float`, not `double`. No precision issues at this scale.
- Clamped to `[0, 1]` with `std::max(0.0f, T)` to guard against negative transmittance from out-of-range alpha values.
- `getPixel(oy, ox)` — note y-first argument order, matching the DDImage API (this is the `int y, int x` overload of `DeepPlane::getPixel`).

### Edit 4 — `renderStripe`: apply `holdout_w` in the splat step

**Location:** inside the per-colour-channel splat loop (step 8) and the alpha splat, just before the `writableAt +=` accumulation.

**Current splat for RGB channels:**
```cpp
                        const float chan_val = pixel.getUnorderedSample(s, rgb_chans[c]);
                        const float w = transmit[c] / static_cast<float>(N);
                        imagePlane.writableAt(out_xi, out_yi, rgb_chans[c])
                            += chan_val * alpha * w;
```

**After S03 (R/G/B channels):**
```cpp
                        const float chan_val = pixel.getUnorderedSample(s, rgb_chans[c]);
                        const float w = transmit[c] / static_cast<float>(N);
                        const float holdout_w = transmittance_at(out_xi, out_yi, depth);
                        imagePlane.writableAt(out_xi, out_yi, rgb_chans[c])
                            += chan_val * alpha * w * holdout_w;
```

**After S03 (alpha channel):**
```cpp
                            const float w = transmit[1] / static_cast<float>(N);
                            const float holdout_w = transmittance_at(out_xi, out_yi, depth);
                            imagePlane.writableAt(out_xi, out_yi, Chan_Alpha)
                                += alpha * w * holdout_w;
```

**Why `transmittance_at(out_xi, out_yi, depth)`?**
- `out_xi, out_yi` — the *output* pixel where the contribution lands (after Newton trace). The holdout is evaluated at the *output* position, not the *input* position (R024: holdout evaluated at output pixel).
- `depth` — depth of the *main* sample being scattered. Already in scope from step 5 (`const float depth = 0.5f * (z_front + z_back)`).
- This is called once per aperture sample per channel. For N=64 with a holdout, that's 64 × 3 = 192 calls per deep sample, each walking up to `nH` holdout samples. For typical holdout sample counts (1-10 samples per pixel), this is negligible.

**Performance note:** `transmittance_at` is called with `(out_xi, out_yi)` not `(px, py)`. For the R and B channels, `out_xi/out_yi` differs due to chromatic aberration. Each channel gets its own holdout lookup at its own landing position — this is correct behaviour.

---

## Mock Header Requirement for Syntax Check

The existing mock `DeepPlane.h` in `verify-s01-syntax.sh` has:
```cpp
DeepPixel getPixel(int y, int x) const { return DeepPixel(); }
DeepPixel getPixel(const Box::iterator&) const { return DeepPixel(); }
```

The `getPixel(int y, int x)` overload is already present — no mock changes needed for the lambda's `holdoutPlane.getPixel(oy, ox)` call. ✓

The `Mask_Alpha` constant is used in `ChannelSet(Mask_Alpha)`. Check the mock `Channel.h`:
```cpp
static const ChannelMask Mask_RGBA = 0xF;
static const ChannelMask Mask_All  = 0xFFFFFFFF;
```
`Mask_Alpha` is **not** defined in the mock. It must be added, or the holdout channel request must use `ChannelSet()` + `Chan_Alpha` instead of `ChannelSet(Mask_Alpha)`. The safer approach: use `ChannelSet needed_h; needed_h += Chan_Alpha; needed_h += Chan_DeepFront; needed_h += Chan_DeepBack;` to avoid adding a new mock constant.

Alternatively, add `static const ChannelMask Mask_Alpha = 0x4;` to the mock Channel.h. Either works; the explicit `+=` approach requires no mock change and is consistent with the existing `needed` setup in step 1.

---

## S03 Grep Contracts

Add these to the end of `verify-s01-syntax.sh`:

```bash
echo "Checking S03 contracts..."
grep -q 'holdoutConnected'      "$SRC_DIR/DeepCDefocusPO.cpp" || { echo "FAIL: holdoutConnected missing"; exit 1; }
grep -q 'transmittance_at'      "$SRC_DIR/DeepCDefocusPO.cpp" || { echo "FAIL: transmittance_at lambda missing"; exit 1; }
grep -q 'holdout_w'             "$SRC_DIR/DeepCDefocusPO.cpp" || { echo "FAIL: holdout_w factor missing from splat"; exit 1; }
grep -q 'Chan_DeepFront.*>= Z\|hzf >= Z' "$SRC_DIR/DeepCDefocusPO.cpp" || { echo "FAIL: depth gate (>= Z) missing from transmittance_at"; exit 1; }
# input(1) must be used for the holdout Op (already present from S01: input_label; now must also appear in getRequests and renderStripe)
test "$(grep -c 'input(1)' "$SRC_DIR/DeepCDefocusPO.cpp")" -ge 3 || { echo "FAIL: input(1) appears fewer than 3 times (label + getRequests + renderStripe)"; exit 1; }
echo "S03 contracts: all pass."
```

---

## Verification Plan

| Check | Command | Expected |
|-------|---------|----------|
| Syntax check (all 3 files) | `bash scripts/verify-s01-syntax.sh` | exits 0 |
| holdoutConnected present | `grep -q 'holdoutConnected' src/DeepCDefocusPO.cpp` | ✅ |
| transmittance_at lambda | `grep -q 'transmittance_at' src/DeepCDefocusPO.cpp` | ✅ |
| holdout_w in splat | `grep -q 'holdout_w' src/DeepCDefocusPO.cpp` | ✅ |
| Depth gate expression | `grep -q 'hzf >= Z' src/DeepCDefocusPO.cpp` | ✅ |
| input(1) usage count | `grep -c 'input(1)'` ≥ 3 | ✅ |
| No new TODO/STUB markers | `grep -c 'TODO\|STUB' src/DeepCDefocusPO.cpp` == 0 | ✅ |
| Holdout deepRequest present | `grep -q 'holdoutOp->deepRequest\|holdout.*deepRequest' src/DeepCDefocusPO.cpp` | ✅ |

---

## Constraints and Risk

**Risk level: low.** All patterns are established in the codebase.

1. **`getPixel(y, x)` argument order** — DDImage `DeepPlane::getPixel(int y, int x)` takes y first. The mock already has the right signature. The lambda must call `holdoutPlane.getPixel(oy, ox)` (not `(ox, oy)`). This is a silent bug if wrong — produces wrong holdout samples but no compile error.

2. **Unordered vs ordered samples** — the transmittance formula using `getUnorderedSample` is correct because `product((1-alpha_i))` is commutative. If the holdout has overlapping volumetric samples, the result may be slightly over-attenuated. This is acceptable and matches the standard DeepHoldout approximation (Nuke's own node does the same thing).

3. **`holdoutOp->deepEngine` returning false** — must not abort the stripe. Set `holdoutConnected = false`, continue with `holdout_w = 1.0`. This is already modelled on the S02 primary fetch pattern.

4. **`transmittance_at` called with out-of-stripe output coords** — this cannot happen because `transmittance_at` is only called after the bounds check (`if (out_xi < bounds.x() || ...`) passes. No range issue.

5. **Mask_Alpha in mock headers** — avoid `ChannelSet(Mask_Alpha)` constructor; use explicit `+= Chan_Alpha` pattern instead. No mock change needed.

6. **`dynamic_cast<DeepOp*>(Op::input(1))` in two places** — both `getRequests` and `renderStripe` call this. This is the established codebase pattern (see `DeepCDepthBlur.cpp` doing the same for its optional B input). Both are null-guarded.

---

## Implementation Recommendation

Single task, T01. All four edits are small and interdependent — they can't be split without breaking the intermediate state. Implement in order:

1. Edit `getRequests` — add holdout `deepRequest`
2. Edit `renderStripe` — fetch holdout plane (after primary fetch, before zero-fill)
3. Edit `renderStripe` — add `transmittance_at` lambda (before scatter loop)
4. Edit `renderStripe` — apply `holdout_w` in both the RGB splat and alpha splat
5. Add S03 grep contracts to `verify-s01-syntax.sh`
6. Run `bash scripts/verify-s01-syntax.sh` — must exit 0

Total LOC change: ~35 lines added to `DeepCDefocusPO.cpp`, ~10 lines added to `verify-s01-syntax.sh`. No new files.
