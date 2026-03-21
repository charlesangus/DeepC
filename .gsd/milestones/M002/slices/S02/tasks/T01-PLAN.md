---
estimated_steps: 3
estimated_files: 2
---

# T01: Create DeepCBlur icon and update README to 24 plugins

**Slice:** S02 — Build integration and release
**Milestone:** M002

## Description

Create a 24×24 RGBA PNG placeholder icon for DeepCBlur and add the DeepCBlur entry to README.md as the 24th plugin. The icon is auto-installed by the CMake glob pattern `file(GLOB ICONS "icons/DeepC*.png")` — no CMake changes needed. The README entry follows the existing format and is inserted alphabetically between DeepCClamp and DeepCColorLookup.

## Steps

1. Create `icons/DeepCBlur.png` as a 24×24 RGBA PNG. Use Python PIL/Pillow or ImageMagick to generate a simple placeholder icon (e.g., copy the dimensions and format of an existing icon like `icons/DeepCGrade.png`, or generate a solid-color 24×24 PNG). The key constraint: it must be a valid 24×24 PNG with RGBA color type to match the other icons.
2. Add the DeepCBlur entry to `README.md`. Insert it on a new line between the DeepCClamp entry (line 27) and the DeepCColorLookup entry (line 28). Use exactly this format: `- ![](https://raw.githubusercontent.com/charlesangus/DeepC/master/icons/DeepCBlur.png)  [DeepCBlur](https://github.com/charlesangus/DeepC/wiki/DeepCBlur)`
3. Verify the plugin count is now 24 with `grep -c '^\- !\[\]' README.md`.

## Must-Haves

- [ ] `icons/DeepCBlur.png` is a valid 24×24 RGBA PNG file
- [ ] README.md lists DeepCBlur between DeepCClamp and DeepCColorLookup
- [ ] `grep -c '^\- !\[\]' README.md` returns 24

## Verification

- `test -f icons/DeepCBlur.png && file icons/DeepCBlur.png | grep -q "PNG image data"` — icon is a valid PNG
- `grep -q 'DeepCBlur' README.md` — README contains DeepCBlur
- `grep -c '^\- !\[\]' README.md` returns 24 — plugin count is correct

## Inputs

- `icons/DeepCGrade.png` — reference for icon dimensions and format (24×24 RGBA PNG)
- `README.md` — current file with 23 plugin entries to be updated

## Expected Output

- `icons/DeepCBlur.png` — new 24×24 RGBA PNG placeholder icon
- `README.md` — updated with DeepCBlur as the 24th plugin entry
