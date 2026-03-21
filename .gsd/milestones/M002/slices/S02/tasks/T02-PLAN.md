---
estimated_steps: 4
estimated_files: 3
---

# T02: Verify docker-build.sh and CMake integration for DeepCBlur

**Slice:** S02 — Build integration and release
**Milestone:** M002

## Description

Verify that all build-system integration points are correctly wired so docker-build.sh will produce release archives containing DeepCBlur. S01 already added DeepCBlur to PLUGINS and FILTER_NODES in CMakeLists.txt. This task confirms: (1) PLUGINS list includes DeepCBlur, (2) FILTER_NODES includes DeepCBlur, (3) icon glob will pick up DeepCBlur.png, (4) menu.py.in template will generate the correct Filter menu entry, (5) docker-build.sh builds all PLUGINS (no per-plugin exclusion). If Docker is available, attempt a real build; otherwise document the deferred verification.

This task covers requirements R006 (build integration, menu placement) and R007 (docker-build.sh release archives).

## Steps

1. Verify `src/CMakeLists.txt` has DeepCBlur in the PLUGINS list: `grep 'DeepCBlur' src/CMakeLists.txt`
2. Verify `src/CMakeLists.txt` has DeepCBlur in FILTER_NODES: `grep 'FILTER_NODES.*DeepCBlur' src/CMakeLists.txt`
3. Verify the icon glob pattern in CMakeLists.txt (`file(GLOB ICONS "icons/DeepC*.png")`) will match `icons/DeepCBlur.png`: `grep 'GLOB ICONS' src/CMakeLists.txt`
4. Verify `python/menu.py.in` uses `@FILTER_NODES@` in the Filter submenu so DeepCBlur will appear: `grep 'FILTER_NODES' python/menu.py.in`
5. If Docker is available (`which docker`), run `docker-build.sh --linux --versions 16.0` and verify DeepCBlur.so in the output archive. If Docker is not available, document the manual verification commands and confirm all file-level integration is correct.

## Must-Haves

- [ ] DeepCBlur confirmed in PLUGINS list in `src/CMakeLists.txt`
- [ ] DeepCBlur confirmed in FILTER_NODES in `src/CMakeLists.txt`
- [ ] menu.py.in confirmed to use @FILTER_NODES@ for Filter menu
- [ ] Icon glob pattern confirmed to match DeepCBlur.png
- [ ] Docker build attempted or deferred with documented commands

## Verification

- `grep -q 'DeepCBlur' src/CMakeLists.txt` — in PLUGINS
- `grep -q 'FILTER_NODES.*DeepCBlur' src/CMakeLists.txt` — in FILTER_NODES
- `grep -q '@FILTER_NODES@' python/menu.py.in` — menu template wired

## Observability Impact

- **Build verification:** `unzip -l release/DeepC-Linux-Nuke16.0.zip | grep DeepCBlur` — confirms both DeepCBlur.so and DeepCBlur.png are in the release archive
- **Menu integration:** `grep 'DeepCBlur' install/16.0-linux/DeepC/menu.py` — confirms DeepCBlur appears in Filter submenu of generated menu
- **Failure visibility:** Docker build stdout shows per-target compile progress; `grep 'DeepCBlur' build/16.0-linux/CMakeCache.txt` traces CMake configuration
- **Future agent inspection:** Run `docker-build.sh --linux --versions 16.0` and check exit code; non-zero means build failure

## Inputs

- `src/CMakeLists.txt` — build system with PLUGINS and FILTER_NODES lists (modified by S01)
- `python/menu.py.in` — menu template using @FILTER_NODES@ placeholder
- `docker-build.sh` — release build script
- `icons/DeepCBlur.png` — icon created by T01

## Expected Output

- `src/CMakeLists.txt` — verified unchanged (no modifications needed)
- `docker-build.sh` — verified unchanged (no modifications needed)
- `python/menu.py.in` — verified unchanged (no modifications needed)
