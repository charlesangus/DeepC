# Phase 6: Local Build System - Context

**Gathered:** 2026-03-17
**Status:** Ready for planning

<domain>
## Phase Boundary

A developer runs `docker-build.sh` to build DeepC for all target Nuke versions on Linux and Windows, using NukeDockerBuild Docker images — no manual Nuke SDK installation required. The existing `batchInstall.sh` (for developers with local Nuke installs) is retained unchanged alongside the new script.

</domain>

<decisions>
## Implementation Decisions

### Nuke version targeting
- Default curated list defined as an array at the top of the script (e.g. `NUKE_VERSIONS=("16.0")`)
- Default list: `16.0` as confirmed; researcher to investigate whether `16.1` and `17.0` have NukeDockerBuild images available and add them if so
- `--versions` flag overrides the default list at runtime (e.g. `--versions 16.0` to test a single version)

### Script design
- New script named `docker-build.sh`, lives at repo root alongside `batchInstall.sh`
- Silent and non-interactive — no prompts, sensible defaults, optional flags for overrides
- `batchInstall.sh` is kept as-is for developers with local Nuke installations

### Platform flags
- `--linux` — build Linux `.so` artifacts only
- `--windows` — build Windows artifacts only (cross-compiled from Linux via NukeDockerBuild)
- `--all` — build both platforms (default when no platform flag is specified)
- No need to support a Windows developer environment; all builds run from Linux

### Output/artifact structure
- Mirrors `batchInstall.sh` conventions: `install/<version>/` for staged plugin files, `release/DeepC-Linux-Nuke<version>.zip` and `release/DeepC-Windows-Nuke<version>.zip` for distributable archives
- Script produces zip archives automatically after each version build

### Claude's Discretion
- How NukeDockerBuild mounts the repo source into the container (bind mount vs copy)
- Container cleanup behavior after each version build
- Error handling / early exit if a specific version's image is unavailable
- Exact zip tool used (zip vs tar.gz — match batchInstall.sh convention)

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### NukeDockerBuild
- `https://github.com/gillesvink/NukeDockerBuild` — Primary upstream. Provides Docker images pre-built with Nuke NDK for multiple versions. Researcher MUST read: available image tags (which Nuke versions exist), Windows cross-compilation mechanism (how to produce Windows builds from a Linux host), recommended usage pattern (docker run invocation, volume mounts, cmake integration).

### Existing build system
- `CMakeLists.txt` — Current CMake configuration; uses `Nuke_ROOT` to locate the SDK. Docker images provide the equivalent of `Nuke_ROOT`.
- `cmake/FindNuke.cmake` — Nuke CMake find module; understand how it resolves `Nuke_ROOT` inside a container context.
- `batchInstall.sh` — Reference for output conventions (`install/<version>/`, `release/` zip naming) and the cmake invocation pattern to replicate.

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `batchInstall.sh` — The loop structure (iterate versions → cmake configure → cmake build → cmake install → zip) is directly replicable. The Docker version replaces the local `Nuke_ROOT` path with a container image invocation.
- `cmake/FindNuke.cmake` — Already supports `Nuke_ROOT` as an explicit override; no changes needed to the CMake layer.

### Established Patterns
- CMake invocation: `cmake -S $BASEDIR -D CMAKE_INSTALL_PREFIX="$INSTALL/$VERSION" -D Nuke_ROOT="$nukeFolder" -B "build/$VERSION"` then `cmake --build ... --config Release` then `cmake --install`
- Output naming: `DeepC-Linux-Nuke$VERSION.zip` / `DeepC-Windows-Nuke$VERSION.zip`
- Linux + Windows detection via `uname` already in batchInstall.sh — note: docker-build.sh runs on Linux only, so no uname branching needed for the host

### Integration Points
- `docker-build.sh` sits at repo root, same level as `batchInstall.sh`
- Builds write to `build/`, `install/`, and `release/` directories (already gitignored or should be)

</code_context>

<specifics>
## Specific Ideas

- User believes NukeDockerBuild handles Windows cross-compilation natively from Linux — researcher to confirm and document the exact mechanism (separate image tag? build arg? separate docker run invocation?)
- `--versions` flag is explicitly useful for quickly iterating on one Nuke version during development

</specifics>

<deferred>
## Deferred Ideas

- None — discussion stayed within phase scope

</deferred>

---

*Phase: 06-local-build-system*
*Context gathered: 2026-03-17*
