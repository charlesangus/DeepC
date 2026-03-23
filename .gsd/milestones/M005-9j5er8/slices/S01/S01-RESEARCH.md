# S01: Correct alpha decomposition + input rename — Research

**Date:** 2026-03-22
**Calibration:** Light — well-understood math fix to known code, pattern already in codebase.

## Summary

`src/DeepCDepthBlur.cpp` is a 491-line DDImage `DeepFilterOp`. The spreading section of `doDeepEngine` scales every non-Z channel by `w` (`px.getUnorderedSample(s, z) * w`). This treats alpha additively (`Σ α·wᵢ = α` only because weights sum to 1), but Nuke's flatten formula is `1 - ∏(1-αᵢ)`, not `Σαᵢ`. As a result `flatten(output) ≠ flatten(input)` for any α < 1.

The fix is three surgical changes to `doDeepEngine` plus one string change in `input_label`:

1. **Zero-alpha input guard** — before spreading, if the source sample's alpha is < 1e-6, pass it through unchanged (avoids division by zero in premult scaling, satisfies R018).
2. **Multiplicative alpha decomposition** — `α_sub = 1 - pow(1 - α, w)`. Because weights sum to 1, `∏(1-α)^wᵢ = (1-α)^(Σwᵢ) = 1-α`, so `1 - ∏(1-α_sub_i) = α`. Flatten invariant holds (R013).
3. **Premult channel scaling** — `c_sub = c * (α_sub / α)` instead of `c * w`. This maintains premult correctness: channel is re-premultiplied by the sub-sample alpha.
4. **Zero-sub-alpha skip** — after computing `α_sub`, if it is < 1e-6, skip emitting that sub-sample (R018). This handles the precision edge case noted in the roadmap (`pow(1-α, w)` for α ≈ 1 and small w).
5. **Input label** — `input_label(0, …)` returns `"Source"` today; change to `""`. `input_label(1, …)` returns `"B"`; change to `"ref"` (D020, R017).

The weights from `computeWeights` are already the correct exponents — they are normalised to sum=1, making them directly usable as the `wᵢ` in the multiplicative formula without any further change to the weight generators.

## Recommendation

Make exactly the changes above to `src/DeepCDepthBlur.cpp`. No other files need to change. The `verify-s01-syntax.sh` mock headers cover all DDImage types already used; the fix introduces no new types (`std::pow` is stdlib, already included via `<cmath>`). Run syntax check first for fast iteration, then `docker-build.sh --linux --versions 16.0` for final proof.

## Implementation Landscape

### Key Files

- `src/DeepCDepthBlur.cpp` — the only file that changes. Three areas within `doDeepEngine`:
  1. **`input_label` method** (lines ~175–181): change `"Source"` → `""`, `"B"` → `"ref"`.
  2. **Before the spread loop's per-sub-sample block** (after `shouldSpread=true` path, before the `for (int i = 0; i < N; ++i)` loop): add alpha fetch + zero-alpha-input guard.
  3. **Inside the `for (int i = 0; i < N; ++i)` loop** (channel value assignment, the `else` branch of the Z/alpha dispatch): replace `px.getUnorderedSample(s, z) * w` with multiplicative decomposition for alpha and proportional scaling for other channels; add sub-alpha skip.
- `scripts/verify-s01-syntax.sh` — no changes needed. All types (`std::pow`, `Chan_Alpha`) are already covered.

### Exact Change Points in `doDeepEngine`

**A. `input_label` (~line 178)**
```cpp
// Before:
case 0: return "Source";
case 1: return "B";
// After:
case 0: return "";
case 1: return "ref";
```

**B. Before `for (int i = 0; i < N; ++i)` loop (~line 370)**
Fetch source alpha once per source sample (outside the sub-sample loop):
```cpp
const float srcAlpha = px.getUnorderedSample(s, Chan_Alpha);
// Zero-alpha input: pass through unchanged, no spreading
if (srcAlpha < 1e-6f) {
    SampleData sd;
    sd.zFront = srcFront;
    sd.zBack  = srcBack;
    sd.chanValues.reserve(nChans);
    foreach(z, channels) {
        sd.chanValues.push_back(px.getUnorderedSample(s, z));
    }
    outSamples.push_back(std::move(sd));
    continue;
}
```

**C. Inside `for (int i = 0; i < N; ++i)` — replace channel assignment**
```cpp
// Multiplicative alpha decomposition
const float alphaSub = 1.0f - std::pow(1.0f - srcAlpha, w);

// Skip near-zero sub-samples (numerical precision guard)
if (alphaSub < 1e-6f)
    continue;

// Channel values
foreach(z, channels) {
    if (z == Chan_DeepFront) {
        sd.chanValues.push_back(sd.zFront);
    } else if (z == Chan_DeepBack) {
        sd.chanValues.push_back(sd.zBack);
    } else if (z == Chan_Alpha) {
        sd.chanValues.push_back(alphaSub);
    } else {
        // Premult channel: scale by (alphaSub / srcAlpha)
        sd.chanValues.push_back(
            px.getUnorderedSample(s, z) * (alphaSub / srcAlpha));
    }
}
```

Note: `alphaSub / srcAlpha` is safe because we guard `srcAlpha < 1e-6f` before entering the sub-sample loop.

### Build Order

1. Edit `src/DeepCDepthBlur.cpp` (all changes in one pass).
2. Run `scripts/verify-s01-syntax.sh` — fast C++ syntax check (~seconds).
3. Run `docker-build.sh --linux --versions 16.0` — authoritative proof, produces `DeepCDepthBlur.so`.

### Verification Approach

- `scripts/verify-s01-syntax.sh` exits 0 — confirms C++ parses correctly.
- `docker-build.sh --linux --versions 16.0` exits 0 with `DeepCDepthBlur.so` in zip — confirms full SDK build.
- Source inspection: `grep "pow(1" src/DeepCDepthBlur.cpp` shows the multiplicative formula; `grep -c '"B"' src/DeepCDepthBlur.cpp` returns 0; `grep '"ref"' src/DeepCDepthBlur.cpp` returns 1.
- Flatten invariant holds by construction (proven above by algebra) — no runtime assertion needed. UAT in Nuke confirms visually.

## Constraints

- `std::pow` for `float` arguments uses the `double` overload in `<cmath>`; cast explicitly: `std::pow(1.0f - srcAlpha, w)` produces `double`, assign to `float alphaSub` — implicit narrowing is fine here, or cast explicitly with `static_cast<float>(...)`. Use `static_cast<float>` to be safe with `-Wconversion`.
- The `continue` inside the sub-sample loop skips emitting a sub-sample. `outSamples` is pre-reserved for `sampleCount * N` entries; skipping some is fine — `reserve` is an upper bound.
- Zero-alpha input samples that pass through unchanged still go through the tidy sort — no special handling needed.
- The `verify-s01-syntax.sh` mock `ChannelSet::size()` returns 0; `nChans` will be 0 under syntax-only check. This is fine since `reserve(0)` and a zero-iteration `foreach` are valid.

## Common Pitfalls

- **Alpha channel must be handled in the `foreach` dispatch** — the existing code only dispatches `Chan_DeepFront` / `Chan_DeepBack` in the `foreach` loop and falls through to `* w` for everything else, including alpha. Must add an explicit `else if (z == Chan_Alpha)` branch for the new formula.
- **`srcAlpha` fetch location** — fetch outside the `for (int i = 0; i < N; ++i)` loop, not inside. Fetching inside repeats the same read N times (harmless but wasteful) and more importantly the zero-alpha guard must fire before entering the sub-sample loop entirely.
- **`continue` skips sub-sample push** — if `alphaSub < 1e-6f`, the `SampleData sd` has been constructed but not pushed to `outSamples`. The `continue` discards it cleanly. No partial push happens because `push_back` is only called after the `if (alphaSub < 1e-6f) continue` guard.
