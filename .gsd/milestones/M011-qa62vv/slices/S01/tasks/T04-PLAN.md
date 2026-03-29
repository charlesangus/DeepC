---
estimated_steps: 24
estimated_files: 1
skills_used: []
---

# T04: Write CPU unit test proving depth-correct T attenuation in holdout gather

Add a `#[test]` in `crates/opendefocus-deep/src/lib.rs` (in a `#[cfg(test)] mod tests` block) that directly exercises the holdout path through `opendefocus_deep_render_holdout` on a minimal synthetic image.

The test constructs a 3×1 deep image:
- Pixel 0 (left): red sample at depth z=5.0 (behind holdout)
- Pixel 1 (center): green sample at depth z=2.0 (in front of holdout)
- Pixel 2 (right): blue sample at depth z=5.0 (behind holdout)

Holdout: all three pixels have a holdout surface at depth z=3.0 with cumulative T=0.1 for samples behind it. Encode as `[3.0, 0.1, 3.0, 0.1]` per pixel (d0=3.0, T0=0.1, d1=3.0, T1=0.1 — same breakpoint repeated for simplicity).

Expected behavior after defocus (using CPU path, minimal blur, quality=Low):
- Output pixel 0: red channel attenuated toward 0 by ~T=0.1
- Output pixel 1: green channel largely unattenuated (T=1.0, sample is at z=2.0 < z=3.0 holdout)
- Output pixel 2: blue channel attenuated toward 0 by ~T=0.1

Note: opendefocus convolves the image, so exact values depend on CoC size. Use a large focus distance (e.g. focus_distance=100.0) to minimize blur and make the test more deterministic. The key assertion is: output_pixel_1.green > output_pixel_0.red (green passes, red is attenuated), not exact float equality.

Steps:
1. Add `#[cfg(test)] mod tests { use super::*; ... }` at the bottom of `lib.rs`.
2. Write `fn test_holdout_attenuates_background_samples()`. Construct the data:
   - `sample_counts = [1u32, 1, 1]` (one sample per pixel)
   - `rgba = [1,0,0,1, 0,1,0,1, 0,0,1,1]` (R, G, B premultiplied)
   - `depth = [5.0f32, 2.0, 5.0]`
   - `lens = LensParams { focal_length_mm: 50.0, fstop: 8.0, focus_distance: 100.0, sensor_size_mm: 36.0 }`
   - `holdout_data = [3.0f32, 0.1, 3.0, 0.1, 3.0, 0.1, 3.0, 0.1, 3.0, 0.1, 3.0, 0.1]` (4 floats × 3 pixels)
   - `HoldoutData { data: holdout_data.as_ptr(), len: 12 }`
3. Allocate `output_rgba = [0f32; 12]` (3 pixels × 4 channels). Call `opendefocus_deep_render_holdout` with use_gpu=0.
4. Assert: `output_rgba[4] > output_rgba[0]` (pixel 1 green > pixel 0 red — holdout attenuated pixel 0's red).
5. Assert: `output_rgba[4] > output_rgba[8]` (pixel 1 green > pixel 2 blue — holdout attenuated pixel 2's blue).
6. Run: `cargo test -p opendefocus-deep -- holdout 2>&1 | grep -E 'ok|FAILED'`.

## Inputs

- ``crates/opendefocus-deep/src/lib.rs``
- ``crates/opendefocus/src/runners/cpu.rs``
- ``crates/opendefocus-kernel/src/stages/ring.rs``

## Expected Output

- ``crates/opendefocus-deep/src/lib.rs``

## Verification

cd /home/latuser/git/DeepC/.gsd/worktrees/M011-qa62vv && cargo test -p opendefocus-deep -- holdout 2>&1 | grep -q 'test.*ok' && echo PASS
