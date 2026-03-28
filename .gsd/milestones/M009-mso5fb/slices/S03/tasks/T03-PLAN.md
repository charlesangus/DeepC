---
estimated_steps: 6
estimated_files: 2
skills_used: []
---

# T03: Add DeepCOpenDefocus.png icon and verify menu registration

Add the node icon and verify menu registration.

1. Create `icons/DeepCOpenDefocus.png` — a 48x48 or 64x64 PNG. Generate a simple placeholder icon (circular aperture shape in blue/white tones) using Python + PIL or a base64-embedded minimal PNG. If no image tool is available, copy and recolor an existing icon from the icons/ directory.
2. Verify that `src/CMakeLists.txt` already lists DeepCOpenDefocus in FILTER_NODES (it does per S01) — no CMake changes needed.
3. Check that `python/menu.py.in` (or generated menu.py) includes the DeepCOpenDefocus entry — confirm with grep.
4. Update `test/test_opendefocus.nk` if needed to match any knob name changes from S02/S03.
5. Add a comprehensive comment block at the top of DeepCOpenDefocus.cpp documenting the node's inputs, knobs, behavior, and the layer-peel approach.

## Inputs

- `src/CMakeLists.txt`
- `python/menu.py.in`
- `icons/`

## Expected Output

- `icons/DeepCOpenDefocus.png`
- `Confirmed menu registration in CMakeLists.txt / menu.py.in`

## Verification

ls icons/DeepCOpenDefocus.png returns file. file icons/DeepCOpenDefocus.png shows PNG data. grep -r DeepCOpenDefocus src/CMakeLists.txt confirms FILTER_NODES entry.
