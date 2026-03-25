# Quick Task: DeepCDefocusPORay hangs at 100% CPU

**Date:** 2026-03-25
**Branch:** gsd/quick/10-deepcdefocusporay-just-hangs-pegs-one-cp

## What Changed
- Fixed the CoC neighbourhood bound calculation that was using `coc_radius(..., 1.0f)` (depth = 1mm from the lens). With focus_distance=200mm, this produced a ~9098 pixel neighbourhood radius — the O(N⁴) gather loop never finished even at 128×72.
- Now scans the actual deep input's DeepFront depth range (min/max) and computes CoC at both extremes to get the realistic worst-case bound.
- Added a hard cap of 256 pixels on `max_coc_px` to prevent degenerate configurations from hanging.

## Files Modified
- `src/DeepCDefocusPORay.cpp` — replaced fixed depth=1mm CoC bound with actual depth-range scan + 256px cap

## Verification
- `bash scripts/verify-s01-syntax.sh` passes
- With test depth=5000mm and focus=200mm, max_coc_px is now ~45 instead of ~9098
