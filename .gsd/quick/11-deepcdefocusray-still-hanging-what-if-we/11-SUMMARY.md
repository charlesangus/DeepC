# Quick Task: DeepCDefocusPORay still hanging

**Date:** 2026-03-25
**Branch:** gsd/quick/11-deepcdefocusray-still-hanging-what-if-we

## What Changed
- Converted DeepCDefocusPORay from an O(output × neighbourhood² × N × 3) gather to an O(input × samples × N × 3) scatter — ~8000× fewer poly evaluations at 128×72 (from ~14.6B to ~1.8M).
- The Option B algorithm (CoC warp, no Newton iteration) is a forward scatter, not a true gather — the previous "gather" loop searched a huge neighbourhood per output pixel and discarded ~99.9% via a selectivity guard. The scatter loop splatting directly to landing pixels is both correct and tractable.
- All Ray-specific features preserved: aperture vignetting via `_aperture_sys`, `sphereToCs` for physical ray direction, lens geometry knobs, holdout, CA wavelengths, Halton sampling, `_max_degree` truncation.

## Files Modified
- `src/DeepCDefocusPORay.cpp` — replaced gather loop with scatter loop (135 insertions, 181 deletions)

## Verification
- `bash scripts/verify-s01-syntax.sh` passes (all 4 plugins + S05 contracts)
- All Ray-specific structural contracts confirmed: `_aperture_sys`, `sphereToCs`, `transmittance_at`, `0.45f`, `halton`, `_max_degree`, `aperture_housing_radius` all present
- Gather selectivity guard (`ox_land`/`oy_land`) removed; scatter bounds check (`out_px_i`/`out_py_i`) present
