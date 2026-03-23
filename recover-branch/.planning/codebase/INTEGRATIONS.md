# External Integrations

**Analysis Date:** 2026-03-12

## APIs & External Services

**Foundry Nuke (Host Application):**
- Nuke NDK (DDImage API) - The sole external integration. Plugins are native shared libraries that Nuke loads at startup and communicates with via the NDK plugin interface.
  - SDK/Client: DDImage headers from Nuke installation (`DDImage/*.h`)
  - Auth: Not applicable - filesystem-based plugin loading
  - Integration point: Each plugin registers itself via a static `Iop::Description` object and a `build()` factory function
  - Nuke discovers plugins by scanning paths registered via `nuke.pluginAddPath()` in `python/init.py`

**Foundry Blink (GPU Compute Framework):**
- Blink API - Used experimentally in `src/DeepCBlink.cpp` for GPU-accelerated pixel processing.
  - SDK/Client: `Blink/Blink.h` from Nuke installation
  - Linked via: `NUKE_RIPFRAMEWORK_LIBRARY` (`libRIPFramework.so`)
  - Status: Proof-of-concept only; not production-ready per source comments

## Data Storage

**Databases:**
- None - no database of any kind

**File Storage:**
- Local filesystem only - plugins read/write image data exclusively through Nuke's in-memory `DeepPixel` and `DeepOutputPlane` structures passed by the NDK at render time
- Icons installed to `icons/` subdirectory of plugin install path
- Python scripts (`init.py`, `menu.py`) installed to root of plugin install path

**Caching:**
- None - no external cache layer; Nuke manages its own tile cache internally

## Authentication & Identity

**Auth Provider:**
- None - no authentication of any kind
- Access control is at the filesystem level (Nuke plugin path permissions)

## Monitoring & Observability

**Error Tracking:**
- None detected - no external error tracking service

**Logs:**
- `printf`/`fprintf` to stdout/stderr in source files (e.g. `src/DeepCBlink.cpp`)
- Nuke's own console captures plugin stdout at runtime

## CI/CD & Deployment

**Hosting:**
- GitHub (https://github.com/charlesangus/DeepC) - source repository and releases
- Compiled binaries distributed via GitHub Releases page as zip archives

**CI Pipeline:**
- None detected - no `.github/workflows/`, no Jenkins, no CircleCI configuration found

**Release packaging:**
- `batchInstall.sh` automates multi-version builds and creates zip archives:
  - Linux: `release/DeepC-Linux-NukeVERSION.zip`
  - Windows: `release/DeepC-Windows-NukeVERSION.zip`

## Environment Configuration

**Required env vars:**
- None at runtime
- Build-time CMake variables only:
  - `Nuke_ROOT` - path to Nuke installation (optional; FindNuke.cmake auto-searches standard paths if not set)
  - `CMAKE_INSTALL_PREFIX` - plugin install destination

**Secrets location:**
- Not applicable - no secrets, API keys, or credentials of any kind

## Webhooks & Callbacks

**Incoming:**
- None

**Outgoing:**
- None

## Plugin-to-Nuke Integration Points

**Startup registration (`python/init.py`):**
- Calls `nuke.pluginAddPath("icons")` to register the icons directory

**Menu registration (`python/menu.py.in` → compiled `menu.py`):**
- Registers all DeepC nodes into the Nuke Nodes toolbar under a "DeepC" submenu
- Submenu categories: Draw, Channel, Color, 3D, Merge, Util
- Node list is injected at CMake configure time via `@VAR@` substitution

**NDK plugin registration (per `.cpp` file):**
- Each plugin defines a static `Op::Description` with its class name and a `build()` function
- Nuke calls `build()` to instantiate the plugin when the user creates a node

---

*Integration audit: 2026-03-12*
