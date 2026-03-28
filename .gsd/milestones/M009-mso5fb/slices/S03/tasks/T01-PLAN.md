---
estimated_steps: 12
estimated_files: 1
skills_used: []
---

# T01: Implement holdout transmittance attenuation in C++ renderStripe (no FFI changes)

Implement holdout attenuation entirely in C++ renderStripe after the FFI call returns the defocused flat RGBA buffer.

Approach (C++ only, no FFI changes):
1. After `opendefocus_deep_render()` returns, check whether input(1) (holdout) is connected and is a DeepOp.
2. If connected: fetch the holdout DeepPlane for the output bounds.
3. For each output pixel (x, y):
   a. Retrieve the holdout deep samples at that pixel.
   b. Sort samples by depth front-to-back.
   c. Compute transmittance T = product of (1 - alpha_s) for each holdout sample.
   d. Multiply the defocused output_rgba[pixel] channels by T (premultiplied: multiply all 4 channels including alpha by T, then re-premultiply if needed — or simply multiply rgb*T and alpha*T since holdout attenuates coverage).
4. If input(1) not connected: no-op, output unchanged.
5. Add helper function `computeHoldoutTransmittance(const DeepPixel& dp) -> float` for testability.
6. Run scripts/verify-s01-syntax.sh to confirm C++ compiles.

## Inputs

- `src/DeepCOpenDefocus.cpp`
- `src/DeepCDepthBlur.cpp`
- `.gsd/milestones/M009-mso5fb/M009-mso5fb-RESEARCH.md`

## Expected Output

- `Updated src/DeepCOpenDefocus.cpp with holdout attenuation`

## Verification

scripts/verify-s01-syntax.sh passes. Grep confirms computeHoldoutTransmittance in DeepCOpenDefocus.cpp. Grep confirms input(1) gated by dynamic_cast check.
