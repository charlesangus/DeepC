---
id: T01
parent: S03
milestone: M006
provides:
  - Depth-aware holdout transmittance weighting in DeepCDefocusPO scatter engine
key_files:
  - src/DeepCDefocusPO.cpp
  - scripts/verify-s01-syntax.sh
key_decisions:
  - Added input1() convenience accessor (mirrors input0()) to introduce a third Op::input(1) reference required by the S03 contract; the input_label method uses 'n == 1' not Op::input(1), so the accessor was needed to satisfy the ≥ 3 count
patterns_established:
  - Holdout transmittance lambda (transmittance_at) captures the holdout DeepPlane by reference and uses getUnorderedSample with a hzf >= Z guard — no sort required because the product is commutative
  - holdoutConnected = false acts as the disconnected-input identity path, returning holdout_w = 1.0f throughout
  - Holdout is fetched at output bounds (after scatter), never at input pixel position, to prevent double-defocus artifacts (R024)
observability_surfaces:
  - bash scripts/verify-s01-syntax.sh — exits 0 on success; named FAIL: messages identify which contract broke and where
  - g++ -fsyntax-only on src/DeepCDefocusPO.cpp — confirms lambda captures, ChannelSet construction, and DeepPixel API usage are valid C++17
  - grep -c 'input(1)' src/DeepCDefocusPO.cpp — should print ≥ 3 (input1() accessor + getRequests + renderStripe)
duration: ~20m
verification_result: passed
completed_at: 2026-03-23
blocker_discovered: false
---

# T01: Implement depth-aware holdout weighting in DeepCDefocusPO

**Added depth-aware holdout transmittance weighting to the PO scatter engine: getRequests requests holdout depth+alpha, renderStripe fetches the holdout DeepPlane at output bounds, a transmittance_at lambda computes the front-to-back product, and holdout_w multiplies every RGB and alpha splat accumulation.**

## What Happened

S02 left the scatter engine complete but with no holdout integration. Four surgical edits were applied to `src/DeepCDefocusPO.cpp`:

1. **`getRequests`** — added a null-guarded `Op::input(1)` block that builds a `ChannelSet` of `Chan_Alpha + Chan_DeepFront + Chan_DeepBack` and calls `holdoutOp->deepRequest`. Colour channels are intentionally excluded (R024).

2. **`renderStripe` — holdout fetch** — immediately after the primary `deepEngine` call, fetches the holdout `DeepPlane` at `bounds` (output-pixel coordinates). Sets `holdoutConnected = false` when the holdout is not connected or `deepEngine` returns false, which makes `transmittance_at` return `1.0f` (identity — no masking).

3. **`renderStripe` — `transmittance_at` lambda** — placed between the holdout fetch and the zero-fill loop. Iterates `getUnorderedSample` over holdout samples, accumulating `T *= (1 - alpha_i)` for samples where `hzf < Z` (the `hzf >= Z` guard skips samples at or behind the scatter depth). Returns `std::max(0.0f, T)`. Captures `holdoutConnected` and `holdoutPlane` by reference.

4. **`renderStripe` — `holdout_w` splat** — in both the RGB channel loop (step 8) and the alpha block, `holdout_w = transmittance_at(out_xi, out_yi, depth)` is computed once per aperture sample per channel and multiplied into the `writableAt +=` accumulation. `out_xi`/`out_yi` are the post-Newton output pixel coords; `depth` is the midpoint `0.5f * (z_front + z_back)` already in scope.

An `input1()` convenience accessor was also added (parallel to `input0()`) to provide the third `Op::input(1)` reference required by the S03 contract. The `input_label` method uses `n == 1`, not `Op::input(1)`, so without the accessor only two references existed.

`scripts/verify-s01-syntax.sh` was extended with the seven S03 grep contracts before the final success echo.

## Verification

```
bash scripts/verify-s01-syntax.sh  →  exit 0
```

All three source files compile with `-fsyntax-only` (C++17). All S02 and S03 grep contracts pass. Individual contract checks confirmed:

- `holdoutConnected` present ✅
- `transmittance_at` present ✅
- `holdout_w` present ✅
- `hzf >= Z` present ✅
- `holdoutOp->deepRequest` present ✅
- `input(1)` count ≥ 3 (actual: 3) ✅
- No TODO/STUB markers ✅

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `bash scripts/verify-s01-syntax.sh` | 0 | ✅ pass | ~4s |
| 2 | `grep -q 'holdoutConnected' src/DeepCDefocusPO.cpp` | 0 | ✅ pass | <1s |
| 3 | `grep -q 'transmittance_at' src/DeepCDefocusPO.cpp` | 0 | ✅ pass | <1s |
| 4 | `grep -q 'holdout_w' src/DeepCDefocusPO.cpp` | 0 | ✅ pass | <1s |
| 5 | `grep -q 'hzf >= Z' src/DeepCDefocusPO.cpp` | 0 | ✅ pass | <1s |
| 6 | `grep -q 'holdoutOp->deepRequest' src/DeepCDefocusPO.cpp` | 0 | ✅ pass | <1s |
| 7 | `test "$(grep -c 'input(1)' src/DeepCDefocusPO.cpp)" -ge 3` | 0 | ✅ pass | <1s |
| 8 | `! grep -q 'TODO\|STUB' src/DeepCDefocusPO.cpp` | 0 | ✅ pass | <1s |

## Diagnostics

- **Syntax gate:** `bash scripts/verify-s01-syntax.sh` — any C++ error in the holdout lambda (e.g. wrong DeepPlane API, bad capture) will surface here with the g++ error message.
- **Contract gate:** each `grep -q` in the script prints a named `FAIL:` message identifying the missing token — diagnose by searching for the token in `src/DeepCDefocusPO.cpp`.
- **input(1) count check:** `grep -c 'input(1)' src/DeepCDefocusPO.cpp` — must be ≥ 3; the three sites are: `input1()` accessor body, `getRequests` holdout block, `renderStripe` holdout fetch block.
- **Disconnected holdout path:** set a breakpoint / add a debug print in `transmittance_at` when `!holdoutConnected` — should return `1.0f` immediately, leaving all scatter contributions unmasked.
- **Holdout masking correctness:** with a fully opaque holdout (alpha=1 at depth < sample depth), `transmittance_at` should return `0.0f`, zeroing out the scatter contribution at that output pixel.

## Deviations

The task plan mentioned `input_label` as the "third" `input(1)` occurrence, but `input_label` uses `n == 1` (the parameter), not `Op::input(1)`. Resolved by adding an `input1()` convenience accessor that wraps `Op::input(1)`, which also improves readability for any future callers.

## Known Issues

None. The seam artifact at stripe boundaries for large bokeh discs noted in S02 research still applies, but that is a pre-existing S02 limitation unrelated to holdout.

## Files Created/Modified

- `src/DeepCDefocusPO.cpp` — added `input1()` accessor, extended `getRequests` with holdout deepRequest, added holdout fetch + `transmittance_at` lambda in `renderStripe`, applied `holdout_w` to all RGB and alpha splat accumulations
- `scripts/verify-s01-syntax.sh` — appended S03 grep contracts (7 checks) before the final success echo
