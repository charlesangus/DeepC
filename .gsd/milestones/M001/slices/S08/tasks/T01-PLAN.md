# T01: 07-deepthinner-integration 01

**Slice:** S08 — **Milestone:** M001

## Description

Vendor DeepThinner into the DeepC project: copy the upstream source into src/, wire it into the CMake build system and menu.py.in template, and verify both Linux and Windows docker builds produce working release archives with the DeepThinner binary.

Purpose: Complete all four THIN requirements in a single plan -- DeepThinner becomes a fully integrated plugin shipping alongside the existing 22 DeepC plugins.
Output: DeepThinner.cpp in src/, updated CMakeLists.txt files, updated menu.py.in, successful docker builds for both platforms.

## Must-Haves

- [ ] "DeepThinner.cpp exists in src/ with upstream MIT license header intact"
- [ ] "CMake builds DeepThinner as a .so (Linux) and .dll (Windows) without errors"
- [ ] "DeepThinner appears in the DeepC toolbar under a Filter submenu after sourcing the generated menu.py"
- [ ] "docker-build.sh produces release archives containing the DeepThinner binary for both platforms"

## Files

- `src/DeepThinner.cpp`
- `src/CMakeLists.txt`
- `python/menu.py.in`
- `CMakeLists.txt`
