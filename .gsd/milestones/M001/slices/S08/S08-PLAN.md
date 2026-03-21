# S08: Deepthinner Integration

**Goal:** Vendor DeepThinner into the DeepC project: copy the upstream source into src/, wire it into the CMake build system and menu.
**Demo:** Vendor DeepThinner into the DeepC project: copy the upstream source into src/, wire it into the CMake build system and menu.

## Must-Haves


## Tasks

- [x] **T01: 07-deepthinner-integration 01** `est:5min`
  - Vendor DeepThinner into the DeepC project: copy the upstream source into src/, wire it into the CMake build system and menu.py.in template, and verify both Linux and Windows docker builds produce working release archives with the DeepThinner binary.

Purpose: Complete all four THIN requirements in a single plan -- DeepThinner becomes a fully integrated plugin shipping alongside the existing 22 DeepC plugins.
Output: DeepThinner.cpp in src/, updated CMakeLists.txt files, updated menu.py.in, successful docker builds for both platforms.

## Files Likely Touched

- `src/DeepThinner.cpp`
- `src/CMakeLists.txt`
- `python/menu.py.in`
- `CMakeLists.txt`
