# Technology Stack

**Analysis Date:** 2026-03-12

## Languages

**Primary:**
- C++14 (Nuke < 15.0) / C++17 (Nuke >= 15.0) - All plugin source code in `src/`

**Secondary:**
- Python 3 (Nuke-embedded) - Menu registration and plugin path setup in `python/`
- CMake - Build system definitions in `CMakeLists.txt` and `src/CMakeLists.txt`
- Bash - Batch install automation in `batchInstall.sh`

## Runtime

**Environment:**
- Foundry Nuke (versions 9.0 through 15.1) - Host application that loads and runs the compiled plugins
- Plugins are native shared libraries (`.so` on Linux, `.dylib` on macOS) loaded by Nuke at startup

**C++ ABI:**
- Nuke < 15.0: `_GLIBCXX_USE_CXX11_ABI=0` (old ABI)
- Nuke >= 15.0: `_GLIBCXX_USE_CXX11_ABI=1` (new ABI)

**Package Manager:**
- None - vendored dependencies only (FastNoise bundled as source)
- Lockfile: Not applicable

## Frameworks

**Core:**
- Foundry NDK (Nuke Developer Kit) - The Nuke plugin API accessed via `DDImage/*` headers. Required. Located at `Nuke_ROOT/include/DDImage/`. Key headers: `DDImage/DeepFilterOp.h`, `DDImage/DeepPixelOp.h`, `DDImage/Knobs.h`, `DDImage/ChannelSet.h`
- Foundry Blink API - GPU compute framework accessed via `Blink/Blink.h`. Used only in `src/DeepCBlink.cpp` (experimental). Linked via `NUKE_RIPFRAMEWORK_LIBRARY`.

**Testing:**
- None detected - no test framework present

**Build/Dev:**
- CMake 3.15+ - Build system, configured in `/workspace/CMakeLists.txt` and `/workspace/src/CMakeLists.txt`
- `cmake/FindNuke.cmake` - Custom CMake module to locate Nuke installation
- Docker (ASWF CI images) - Optional containerized build environment documented in `README.md`
  - Nuke 15: `aswf/ci-base:2024.1`
  - Nuke 14: `aswf/ci-base:2022.4`
  - Nuke 13: `aswf/ci-base:2020.9`

## Key Dependencies

**Critical:**
- Foundry Nuke (NDK) - Version 9.0 through 15.1 supported. Provides `DDImage` library (`libDDImage.so`) and `RIPFramework` library. Located via `cmake/FindNuke.cmake`. Detected known versions: `9.0, 10.0, 10.5, 11.0, 11.1, 11.2, 11.3, 12.0, 12.1, 12.2, 13.0, 13.1, 13.2, 14.0, 14.1, 15.0, 15.1`

**Vendored (bundled as source):**
- FastNoise (MIT License, Jordan Peck 2017) - Procedural noise generation library. Source at `/workspace/FastNoise/FastNoise.cpp` and `/workspace/FastNoise/FastNoise.h`. Compiled as a CMake OBJECT library and linked only into `DeepCPNoise`. Provides Simplex, Cellular, Cubic, Perlin, and Value noise types including 4D variants.

**Optional:**
- OpenGL (`libGL`) - Required only for `DeepCPMatte`. Build skips this plugin if OpenGL development libraries are not found.

## Configuration

**Environment:**
- No runtime environment variables - configuration is entirely at build time via CMake variables
- `Nuke_ROOT` - CMake variable pointing to Nuke installation directory (e.g. `/usr/local/Nuke15.1v1`)
- `CMAKE_INSTALL_PREFIX` - Destination for compiled plugins and Python scripts

**Build:**
- `/workspace/CMakeLists.txt` - Top-level build definition
- `/workspace/src/CMakeLists.txt` - Plugin compilation and install rules
- `/workspace/cmake/FindNuke.cmake` - Nuke discovery module
- `/workspace/python/menu.py.in` - CMake-configured template; generates `menu.py` during build with node lists substituted

**Compiler flags (Linux/Unix):**
- `-DUSE_GLEW -fPIC -msse -msse2 -msse3 -mssse3 -msse4 -msse4.1 -msse4.2 -mavx`

## Platform Requirements

**Development:**
- Linux: CentOS 7 with a devtoolset (devtoolset-3 through devtoolset-7), `mesa-libGLU-devel` for OpenGL support
- Windows: Visual Studio 2017 (v15), x64 platform, CMake GUI or CLI
- macOS: Unsupported ("Unlikely..." per README)

**Production:**
- Linux: Primary target; `.so` shared libraries
- Windows: Supported; `.dll` shared libraries
- macOS: Not actively supported; `.dylib` would be emitted but untested

---

*Stack analysis: 2026-03-12*
