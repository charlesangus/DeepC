# M003/S02 — Research

**Date:** 2026-03-21
**Slice:** Alpha correction + UI polish

## Summary

S02 is straightforward application of existing codebase patterns to known integration points. All four deliverables (WH_knob, BeginClosedGroup twirldown, alpha correction toggle, docker build) have direct reference implementations already in the repo or precise insertion points identified by S01's forward intelligence. No novel architecture, no unfamiliar APIs.

The alpha correction algorithm is a post-blur, per-pixel front-to-back pass that divides each sample's RGBA by cumulative transmittance of samples in front of it — undoing the darkening that the over-operator would impose during compositing. The insertion point is between `optimizeSamples` and the emit loop in `doDeepEngine`, exactly where S01's forward intelligence flagged it. Since `optimizeSamples` already sorts samples front-to-back (zFront ascending), no additional sort is needed.

The implementation is three independent edits to `src/DeepCBlur.cpp` plus a mock header update for `scripts/verify-s01-syntax.sh`.

## Recommendation

Work in a single task that makes all changes to `DeepCBlur.cpp` together (members → knobs() → doDeepEngine), then updates the syntax script mock headers, then runs both syntax verification and docker build. The three `DeepCBlur.cpp` changes are interdependent (member types feed knobs(), which feeds engine access), so splitting them across tasks adds coordination overhead for no benefit.

## Implementation Landscape

### Key Files

- `src/DeepCBlur.cpp` — all changes land here; current state is post-S01 (337 lines, separable blur)
- `scripts/verify-s01-syntax.sh` — mock headers need `WH_knob`, `Bool_knob`, `BeginClosedGroup`, `EndGroup` stubs added to `Knobs.h`
- `src/DeepCAdjustBBox.cpp:17,42` — canonical WH_knob pattern: `double numpixels[2]` member + `WH_knob(f, numpixels, "numpixels", "Add Pixels")`
- `src/DeepThinner.cpp:183,217` — canonical `BeginClosedGroup` / `EndGroup` pattern
- `src/DeepSampleOptimizer.h:71-74` — confirms `optimizeSamples` sorts front-to-back (zFront ascending); no re-sort needed before correction pass

### Change 1 — WH_knob (R004)

Replace `float _blurWidth` and `float _blurHeight` members with `double _blurSize[2]` array. In `knobs()`, replace the two `Float_knob` calls with one `WH_knob(f, _blurSize, "blur_size", "blur size")`. In `doDeepEngine` and `_validate`, access `_blurSize[0]` / `_blurSize[1]` casting to float where kernelRadius and computeKernel expect floats. Constructor initializes `_blurSize[0] = _blurSize[1] = 1.0`.

Alternatively (lower-risk per S01 forward intelligence): keep `_blurWidth` / `_blurHeight` floats for engine use, add `double _blurSize[2]` for the knob, and assign `_blurWidth = (float)_blurSize[0]; _blurHeight = (float)_blurSize[1];` in `_validate`. This insulates `doDeepEngine` from any type change. Either approach works; the direct replacement is cleaner.

### Change 2 — Sample Optimization twirldown (R005)

Wrap the three existing sample knobs in `knobs()` with:
```cpp
BeginClosedGroup(f, "Sample Optimization");
// Int_knob max_samples
// Float_knob merge_tolerance  
// Float_knob color_tolerance
EndGroup(f);
```
The knob variable assignments and Tooltip calls stay identical — only the group delimiters are added.

### Change 3 — Alpha correction (R003, R006)

Add `bool _alphaCorrection` member (default `false`) and `Bool_knob(f, &_alphaCorrection, "alpha_correction", "alpha correction")` in `knobs()`.

In `doDeepEngine`, after the `optimizeSamples` call and before the emit loop, insert:
```cpp
if (_alphaCorrection && scratch.samples.size() > 1) {
    // samples are already sorted front-to-back by optimizeSamples
    float cumTransp = 1.0f;
    for (auto& sr : scratch.samples) {
        if (cumTransp > 1e-6f) {
            const float inv = 1.0f / cumTransp;
            for (int ci = 0; ci < nChans; ++ci) {
                if (!isDepthChan[ci])
                    sr.channels[ci] *= inv;
            }
            sr.alpha *= inv;
        }
        cumTransp *= std::max(0.0f, 1.0f - sr.alpha);
    }
}
```
Note: `sr.alpha` in `SampleRecord` is the weighted alpha accumulated through both passes; `sr.channels` holds the channel array (including a redundant alpha slot at the Chan_Alpha index). Both must be corrected consistently. Check `SampleRecord` layout in `DeepSampleOptimizer.h` to confirm alpha is stored separately from channels — it is (`float alpha` + `std::vector<float> channels`). The emit loop uses `sr.channels[ci]` for all non-depth channels, so channels must be corrected. `sr.alpha` is used by `optimizeSamples` but at this point optimization is done, so only the channel array matters for output. Correct both for consistency.

### Change 4 — Mock header update

Add to `Knobs.h` stub in `verify-s01-syntax.sh`:
```cpp
inline Knob* WH_knob(Knob_Callback, double*, const char*, const char* = nullptr) { return nullptr; }
inline Knob* Bool_knob(Knob_Callback, bool*, const char*, const char* = nullptr) { return nullptr; }
inline void BeginClosedGroup(Knob_Callback, const char*) {}
inline void EndGroup(Knob_Callback) {}
```

### Build Order

1. Edit `DeepCBlur.cpp`: members + knobs() + doDeepEngine (all three changes together)
2. Update mock headers in `verify-s01-syntax.sh`
3. Run `bash scripts/verify-s01-syntax.sh` — confirms syntax
4. Run docker build — confirms R008

### Verification Approach

```bash
# V1 — WH_knob present
grep -c "WH_knob" src/DeepCBlur.cpp  # → 1

# V2 — Float_knobs for blur removed
grep "Float_knob.*blur" src/DeepCBlur.cpp  # → no output

# V3 — BeginClosedGroup present
grep -c "BeginClosedGroup" src/DeepCBlur.cpp  # → 1

# V4 — Bool_knob alpha_correction present
grep -c "Bool_knob.*alpha_correction" src/DeepCBlur.cpp  # → 1

# V5 — Syntax check
bash scripts/verify-s01-syntax.sh  # → "Syntax check passed."

# V6 — Docker build
bash docker-build.sh  # → exit 0, DeepCBlur.so in archive
```

## Constraints

- `WH_knob` takes `double[2]`; `kernelRadius` and `computeKernel` take `float` — explicit cast required at call sites
- `optimizeSamples` sorts samples; alpha correction must come after it (samples are already in the right order)
- `Bool_knob` and `BeginClosedGroup`/`EndGroup` are not yet in the syntax script mock headers — must be added before V5 check
- Alpha correction must not run on empty pixels or single-sample pixels (no darkening artifact to correct; guarded by `scratch.samples.size() > 1`)

## Common Pitfalls

- **Double-correcting alpha in emit loop** — the emit loop writes `sr.channels[ci]` for non-depth channels and reads `sr.zFront`/`sr.zBack` directly. It does not read `sr.alpha`. Confirm there's no second reference to `sr.alpha` in the emit path that would apply correction twice.
- **cumTransp underflow** — if alpha accumulates to ≥ 1.0 early, subsequent samples divide by near-zero. Guard with `if (cumTransp > 1e-6f)` before dividing; clamp or skip correction for those samples.
- **Mock header stubs missing** — syntax check will fail with "WH_knob not declared" if stubs aren't updated before running V5. Do mock header update in the same task as the DeepCBlur.cpp edit.
