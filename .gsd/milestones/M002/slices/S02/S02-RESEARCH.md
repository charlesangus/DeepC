# S02 — Research

**Date:** 2026-03-20

## Summary

S02 is straightforward integration work. S01 already added DeepCBlur to both `PLUGINS` and `FILTER_NODES` in `src/CMakeLists.txt`, so the build system and menu template (`menu.py.in`) will pick it up automatically. The remaining work is: (1) create a `DeepCBlur.png` icon placeholder, (2) add the DeepCBlur entry to README.md's plugin list, and (3) verify that `docker-build.sh` produces release archives containing DeepCBlur. No code changes to DeepCBlur.cpp or DeepSampleOptimizer.h are needed.

The icon install in `CMakeLists.txt` uses `file(GLOB ICONS "icons/DeepC*.png")` which will automatically pick up `DeepCBlur.png` — no CMake changes needed for the icon either.

## Recommendation

Three small, independent tasks: icon creation, README update, docker-build verification. All are low-risk. The docker-build verification is the only task that could surface surprises (compilation against real Nuke SDK headers), but S01's code follows established patterns from other plugins, so failure is unlikely. Icon and README can be done in parallel; docker-build is the integration proof.

## Implementation Landscape

### Key Files

- `icons/DeepCBlur.png` — **does not exist yet**. Must be created. The glob `file(GLOB ICONS "icons/DeepC*.png")` in `CMakeLists.txt:44` will auto-install it. No CMake changes needed.
- `README.md` — currently lists 23 plugins (23 `- ![]` entries). DeepCBlur must be added as the 24th. Insert alphabetically between DeepCClamp and DeepCColorLookup (or at end of the list, matching the style).
- `src/CMakeLists.txt` — **no changes needed**. S01 already added DeepCBlur to `PLUGINS` (line 10) and `FILTER_NODES` (line 47). The `string(REPLACE)` pattern at line 119 will format it into `menu.py`.
- `python/menu.py.in` — **no changes needed**. The `@FILTER_NODES@` placeholder is already wired to the Filter submenu. DeepCBlur will appear automatically.
- `docker-build.sh` — **no changes needed**. It builds all plugins in `PLUGINS` list. Verification only.

### Build Order

1. **Icon + README** (independent, no risk) — create placeholder icon, update README plugin list and count
2. **Docker-build verification** (integration proof) — run `docker-build.sh --linux` and confirm DeepCBlur.so appears in the release archive. This is the definitive proof for R006 and R007.

### Verification Approach

- **Icon installed**: `ls icons/DeepCBlur.png` — file exists
- **README updated**: `grep -c '^\- !\[\]' README.md` returns 24; `grep 'DeepCBlur' README.md` finds the entry
- **CMake integration** (already done by S01): `grep 'DeepCBlur' src/CMakeLists.txt` shows it in PLUGINS and FILTER_NODES
- **Menu integration**: `cmake -S . -B build-check -D Nuke_ROOT=... && cat build-check/src/menu.py | grep DeepCBlur` — confirm DeepCBlur appears in the generated menu.py (requires Nuke SDK, so deferred to docker build)
- **Docker build**: `docker-build.sh --linux --versions 16.0` then `unzip -l release/DeepC-Linux-Nuke16.0.zip | grep DeepCBlur` — confirms DeepCBlur.so is in the archive
- **Windows archive**: same check on the Windows zip if `--all` is used

## Constraints

- Docker must be available to run `docker-build.sh`. If not available in this environment, verification is deferred to CI or manual run — but the file changes (icon, README) can still be completed.
- The icon glob pattern `file(GLOB ICONS "icons/DeepC*.png")` means the icon filename **must** start with `DeepC` — `DeepCBlur.png` matches.

## Common Pitfalls

- **Op::Description class string vs menu placement** — The Description string is `"Deep/DeepCBlur"` (line 337 of DeepCBlur.cpp). This is Nuke's internal class path, not the toolbar location. Toolbar placement is handled entirely by `menu.py.in` via FILTER_NODES. No conflict here — both are correctly set.
- **README alphabetical ordering** — existing entries are roughly alphabetical. DeepCBlur should go between DeepCClamp and DeepCColorLookup to maintain order.
