---
estimated_steps: 9
estimated_files: 2
skills_used: []
---

# T03: Write test/test_opendefocus.nk and extend verify script for S02 syntax check

Create the test .nk script that the milestone DoD requires.

1. Create `test/` directory if not present.
2. Write `test/test_opendefocus.nk` as a plain-text Nuke script that:
   a. Creates a DeepConstant node generating a 128×72 deep image with non-zero RGBA and a constant depth of 5.0.
   b. Connects it to a DeepCOpenDefocus node with default knob values (focal_length=50, fstop=2.8, focus_distance=5.0, sensor_size_mm=36).
   c. Connects the output to a Write node writing to /tmp/test_opendefocus.exr format exr.
   d. Sets the frame range to 1-1.
3. Extend `scripts/verify-s01-syntax.sh` (or create `scripts/verify-s02-syntax.sh`) to also check that `test/test_opendefocus.nk` exists.
4. Optionally: check that `include/opendefocus_deep.h` parses with clang -fsyntax-only (if clang available).

## Inputs

- `scripts/verify-s01-syntax.sh`
- `src/DeepCOpenDefocus.cpp`

## Expected Output

- `test/test_opendefocus.nk`
- `Updated scripts/verify-s01-syntax.sh or new verify-s02-syntax.sh`

## Verification

bash scripts/verify-s01-syntax.sh passes. cat test/test_opendefocus.nk shows DeepConstant, DeepCOpenDefocus, Write node entries.
