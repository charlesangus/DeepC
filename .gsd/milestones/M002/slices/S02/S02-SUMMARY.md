---
id: S02
parent: M002
milestone: M002
provides:
  - DeepCBlur.png 24×24 RGBA placeholder icon
  - README.md updated to 24 plugins with DeepCBlur entry in correct alphabetical order
  - End-to-end Docker build proof: DeepCBlur.so + icon in release archive
  - Filter toolbar menu integration verified via generated menu.py
requires:
  - slice: S01
    provides: DeepCBlur.cpp compilable source, DeepSampleOptimizer.h shared header, CMakeLists.txt PLUGINS/FILTER_NODES entries
affects: []
key_files:
  - icons/DeepCBlur.png
  - README.md
  - src/CMakeLists.txt
  - python/menu.py.in
  - docker-build.sh
key_decisions:
  - No icon glob exists in src/CMakeLists.txt — icons are installed via install(DIRECTORY) in top-level CMakeLists.txt, not a file(GLOB) pattern
  - DeepCBlur placed before DeepCClamp in README (correct alphabetical: "Blur" < "Clamp")
patterns_established:
  - Python PIL for generating placeholder 24×24 toolbar icons
  - Docker build verification via docker-build.sh --linux --versions 16.0 takes ~40s with cached images
observability_surfaces:
  - file icons/DeepCBlur.png — confirms 24×24 RGBA PNG
  - grep -c '^\- !\[\]' README.md — returns 24
  - unzip -l release/DeepC-Linux-Nuke16.0.zip | grep DeepCBlur — confirms .so and .png in archive
  - grep DeepCBlur install/16.0-linux/DeepC/menu.py — confirms Filter menu placement
drill_down_paths:
  - .gsd/milestones/M002/slices/S02/tasks/T01-SUMMARY.md
  - .gsd/milestones/M002/slices/S02/tasks/T02-SUMMARY.md
duration: 8m
verification_result: passed
completed_at: 2026-03-20
---

# S02: Build integration and release

**DeepCBlur fully integrated into build system, toolbar menu, and release archives — README updated to 24 plugins.**

## What Happened

T01 created a 24×24 RGBA PNG placeholder icon (cornflower blue solid) via Python PIL and added DeepCBlur as the 24th plugin entry in README.md. The plan's suggested alphabetical position was corrected — "Blur" sorts before "Clamp", so the entry goes after DeepCAdjustBBox and before DeepCClamp.

T02 verified end-to-end build integration. All CMakeLists.txt integration points (PLUGINS list, FILTER_NODES variable, string(REPLACE) for menu generation) were confirmed intact from S01. The plan assumed an `file(GLOB ICONS ...)` pattern in `src/CMakeLists.txt` — no such pattern exists. Icons are installed via `install(DIRECTORY)` in the top-level CMakeLists.txt, which picks up all files from `icons/` automatically. A full Docker Linux build (`docker-build.sh --linux --versions 16.0`) completed in ~40 seconds, producing `release/DeepC-Linux-Nuke16.0.zip` containing both `DeepC/DeepCBlur.so` and `DeepC/icons/DeepCBlur.png`. The generated `menu.py` correctly places DeepCBlur in the Filter submenu alongside DeepThinner.

## Verification

All slice-level checks passed (5/5):
1. `icons/DeepCBlur.png` exists as valid 24×24 RGBA PNG
2. `README.md` contains DeepCBlur
3. Plugin count is exactly 24
4. `src/CMakeLists.txt` includes DeepCBlur in PLUGINS
5. `src/CMakeLists.txt` includes DeepCBlur in FILTER_NODES

Additional Docker build verification (T02):
- `docker-build.sh --linux --versions 16.0` exit code 0
- `unzip -l release/DeepC-Linux-Nuke16.0.zip | grep DeepCBlur` — 2 matches (DeepCBlur.so + DeepCBlur.png)
- `grep DeepCBlur install/16.0-linux/DeepC/menu.py` — Filter submenu confirmed

## New Requirements Surfaced

- none

## Deviations

- DeepCBlur placed before DeepCClamp in README rather than between DeepCClamp and DeepCColorLookup. The plan's alphabetical suggestion was wrong — "Blur" < "Clamp".
- Plan assumed `file(GLOB ICONS ...)` in `src/CMakeLists.txt`. No such pattern exists; icons use `install(DIRECTORY)` in top-level CMakeLists.txt. Not a problem — the icon is correctly picked up.

## Known Limitations

- Windows Docker build not verified in this environment (no Windows Docker image available). CMake integration is platform-agnostic, so the risk is low.
- Icon is a solid cornflower blue placeholder — not a proper design. Functional but not production-quality visually.

## Follow-ups

- Replace placeholder icon with a proper DeepCBlur icon design if/when an artist creates one.
- Windows Docker build verification when `nukedockerbuild:16.0-windows` image is available.

## Files Created/Modified

- `icons/DeepCBlur.png` — new 24×24 RGBA PNG placeholder icon
- `README.md` — added DeepCBlur as 24th plugin entry in alphabetical order

## Forward Intelligence

### What the next slice should know
- M002 is complete. All source artifacts, build integration, and release packaging are done. The only remaining proof is UAT — visual inspection of blurred deep output in Nuke.

### What's fragile
- Placeholder icon — if icon guidelines change or a real icon is designed, `icons/DeepCBlur.png` needs replacement.

### Authoritative diagnostics
- `unzip -l release/DeepC-Linux-Nuke16.0.zip | grep DeepCBlur` — most trustworthy proof that the plugin ships in the release archive.
- `grep -c '^\- !\[\]' README.md` — catches plugin count drift if entries are added/removed.

### What assumptions changed
- Plan assumed `file(GLOB ICONS ...)` in `src/CMakeLists.txt` — actually icons use `install(DIRECTORY)` in top-level CMakeLists.txt. No functional impact.
