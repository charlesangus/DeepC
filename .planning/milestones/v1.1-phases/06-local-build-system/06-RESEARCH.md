# Phase 6: Local Build System - Research

**Researched:** 2026-03-17
**Domain:** Bash scripting, Docker, NukeDockerBuild, CMake cross-compilation
**Confidence:** HIGH

---

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions

- **Nuke version targeting:** Default curated list defined as an array at the top of the script (e.g. `NUKE_VERSIONS=("16.0")`). Default list: `16.0`; researcher to investigate whether `16.1` and `17.0` have NukeDockerBuild images available and add them if so.
- **`--versions` flag:** Overrides the default list at runtime (e.g. `--versions 16.0` to test a single version).
- **Script name:** `docker-build.sh`, lives at repo root alongside `batchInstall.sh`.
- **Silent and non-interactive:** No prompts, sensible defaults, optional flags for overrides.
- **`batchInstall.sh` kept as-is** for developers with local Nuke installations.
- **Platform flags:** `--linux` (Linux `.so` only), `--windows` (Windows artifacts only, cross-compiled from Linux), `--all` (both; default when no flag specified).
- **No Windows developer environment support:** All builds run from Linux.
- **Output/artifact structure:** Mirrors `batchInstall.sh` conventions — `install/<version>/` for staged plugin files; `release/DeepC-Linux-Nuke<version>.zip` and `release/DeepC-Windows-Nuke<version>.zip` for distributable archives.
- **Script produces zip archives automatically** after each version build.

### Claude's Discretion

- How NukeDockerBuild mounts the repo source into the container (bind mount vs copy).
- Container cleanup behavior after each version build.
- Error handling / early exit if a specific version's image is unavailable.
- Exact zip tool used (zip vs tar.gz — match `batchInstall.sh` convention).

### Deferred Ideas (OUT OF SCOPE)

- None — discussion stayed within phase scope.
</user_constraints>

---

## Summary

Phase 6 creates `docker-build.sh`, a non-interactive Bash script at the repo root that builds DeepC for all target Nuke versions (Linux `.so` and Windows DLL artifacts) using the NukeDockerBuild Docker images hosted at `ghcr.io/gillesvink/nukedockerbuild`. The script replaces the need for a local Nuke SDK installation by delegating all compilation to containers.

The NukeDockerBuild project provides separate Docker images per Nuke version per platform, using the tag pattern `<version>-linux-slim-latest` and `<version>-windows-latest`. Linux images compile natively; Windows images cross-compile from Linux using wine-msvc (no Windows host required). As of 2026-03-17, only Nuke `16.0` images exist — `16.1` and `17.0` do not yet have published images. The default `NUKE_VERSIONS` array should contain only `16.0`.

The key integration detail is that `FindNuke.cmake` uses `NO_SYSTEM_ENVIRONMENT_PATH` in its `find_library` calls, which means the container's `CMAKE_PREFIX_PATH` environment variable is NOT automatically honoured. The `docker run` invocation must explicitly pass `-D Nuke_ROOT=/usr/local/nuke_install` to CMake so the find module resolves the Nuke SDK correctly inside the container.

**Primary recommendation:** Use `--rm` `docker run` with a bind mount of the repo at `/nuke_build_directory`, explicitly passing `-D Nuke_ROOT=/usr/local/nuke_install` to all CMake invocations. For Windows builds, additionally pass `-GNinja -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_TOOLCHAIN_FILE=$GLOBAL_TOOLCHAIN`.

---

## Standard Stack

### Core

| Tool | Version | Purpose | Why Standard |
|------|---------|---------|--------------|
| NukeDockerBuild | `16.0-linux-slim-latest` / `16.0-windows-latest` | Pre-built Docker images with Nuke NDK + compiler toolchain | Officially maintained; eliminates SDK installation; used by NukePluginTemplate CI |
| Docker | any recent | Container runtime | Required by NukeDockerBuild |
| CMake | 3.15+ (host) | Build orchestration | Already used by project |
| zip | system | Archive creation | Already used by `batchInstall.sh` for Linux zips |

### Supporting

| Tool | Version | Purpose | When to Use |
|------|---------|---------|-------------|
| Ninja | pre-installed in Windows image | CMake generator for Windows cross-compilation builds | Windows builds inside `ghcr.io/gillesvink/nukedockerbuild:*-windows-latest` |

### Alternatives Considered

| Instead of | Could Use | Tradeoff |
|------------|-----------|----------|
| `ghcr.io/gillesvink/nukedockerbuild` | Self-built images from Dockerfiles | Self-builds require cloning NukeDockerBuild and running `build.sh`; GHCR images are ready to pull |
| `zip` | `tar.gz` | `batchInstall.sh` already uses `zip` for Linux; match that convention |

**Image pull (no installation needed):**
```bash
docker pull ghcr.io/gillesvink/nukedockerbuild:16.0-linux-slim-latest
docker pull ghcr.io/gillesvink/nukedockerbuild:16.0-windows-latest
```

---

## Architecture Patterns

### NukeDockerBuild Container Layout

Inside every `ghcr.io/gillesvink/nukedockerbuild` container:

```
/
├── usr/local/nuke_install/   # Nuke SDK (headers, libs, executables)
│   └── include/DDImage/      # DDImage headers
└── nuke_build_directory/     # Working directory (bind-mount target)
```

**Environment variables set by the image:**
- `CMAKE_PREFIX_PATH=/usr/local/nuke_install` — CMake hint (NOT used by FindNuke.cmake due to NO_SYSTEM_ENVIRONMENT_PATH)
- `NUKE_VERSION=<version>` — e.g. `16.0`
- `GLOBAL_TOOLCHAIN=/nukedockerbuild/toolchain.cmake` — Windows image only; path to wine-msvc toolchain

### Pattern 1: Linux Build per Version

```bash
# Source: https://github.com/gillesvink/NukeDockerBuild README + Dockerfile analysis
CONTAINER_IMAGE="ghcr.io/gillesvink/nukedockerbuild:${nuke_version}-linux-slim-latest"
BUILD_DIR="/nuke_build_directory/build/${nuke_version}-linux"
INSTALL_DIR="/nuke_build_directory/install/${nuke_version}"

docker run --rm \
    -v "$(pwd):/nuke_build_directory" \
    "${CONTAINER_IMAGE}" \
    bash -c "
        cmake -S /nuke_build_directory \
              -B ${BUILD_DIR} \
              -D CMAKE_INSTALL_PREFIX=${INSTALL_DIR} \
              -D Nuke_ROOT=/usr/local/nuke_install && \
        cmake --build ${BUILD_DIR} --config Release && \
        cmake --install ${BUILD_DIR}
    "
```

**Critical:** Pass `-D Nuke_ROOT=/usr/local/nuke_install` explicitly. `FindNuke.cmake` uses `NO_SYSTEM_ENVIRONMENT_PATH` and will NOT pick up `CMAKE_PREFIX_PATH` from the environment.

### Pattern 2: Windows Cross-Compilation per Version

```bash
# Source: https://github.com/gillesvink/NukeDockerBuild README + NukePluginTemplate workflow
CONTAINER_IMAGE="ghcr.io/gillesvink/nukedockerbuild:${nuke_version}-windows-latest"
BUILD_DIR="/nuke_build_directory/build/${nuke_version}-windows"
INSTALL_DIR="/nuke_build_directory/install/${nuke_version}"

docker run --rm \
    -v "$(pwd):/nuke_build_directory" \
    "${CONTAINER_IMAGE}" \
    bash -c "
        cmake -S /nuke_build_directory \
              -B ${BUILD_DIR} \
              -GNinja \
              -DCMAKE_SYSTEM_NAME=Windows \
              -DCMAKE_BUILD_TYPE=Release \
              -DCMAKE_TOOLCHAIN_FILE=\$GLOBAL_TOOLCHAIN \
              -D CMAKE_INSTALL_PREFIX=${INSTALL_DIR} \
              -D Nuke_ROOT=/usr/local/nuke_install && \
        cmake --build ${BUILD_DIR} --config Release && \
        cmake --install ${BUILD_DIR}
    "
```

**Note:** `$GLOBAL_TOOLCHAIN` is expanded inside the container (it is a container env var, not a host env var). Use `\$GLOBAL_TOOLCHAIN` in the bash -c string so it is not expanded by the host shell.

### Pattern 3: Argument Parsing

```bash
# Source: CONTEXT.md decisions
NUKE_VERSIONS=("16.0")
BUILD_LINUX=true
BUILD_WINDOWS=true

while [[ $# -gt 0 ]]; do
    case "$1" in
        --versions)
            IFS=',' read -ra NUKE_VERSIONS <<< "$2"
            shift 2
            ;;
        --linux)
            BUILD_LINUX=true
            BUILD_WINDOWS=false
            shift
            ;;
        --windows)
            BUILD_LINUX=false
            BUILD_WINDOWS=true
            shift
            ;;
        --all)
            BUILD_LINUX=true
            BUILD_WINDOWS=true
            shift
            ;;
        *)
            echo "Unknown flag: $1" >&2
            exit 1
            ;;
    esac
done
```

### Pattern 4: Post-Build Zipping (Matching batchInstall.sh)

```bash
# Source: batchInstall.sh — match existing convention exactly
# Linux
zip -r "./release/DeepC-Linux-Nuke${nuke_version}.zip" "${BASEDIR}/install/${nuke_version}"

# Windows
zip -r "./release/DeepC-Windows-Nuke${nuke_version}.zip" "${BASEDIR}/install/${nuke_version}"
```

The `batchInstall.sh` uses `zip` on Linux. Use `zip` for both platforms' archives since `docker-build.sh` always runs on Linux (no `powershell Compress-Archive` needed).

### Recommended Script Structure

```
docker-build.sh            # Repo root — the new script
├── Header: NUKE_VERSIONS array, defaults
├── Argument parsing: --versions, --linux, --windows, --all
├── Prerequisite check: docker available
├── Directory creation: build/, install/, release/
└── Main loop: for each version
    ├── [if BUILD_LINUX]  Linux docker run → cmake → zip
    └── [if BUILD_WINDOWS] Windows docker run → cmake → zip
```

### Anti-Patterns to Avoid

- **Not passing `Nuke_ROOT` to CMake:** `CMAKE_PREFIX_PATH` is set in the container environment but `FindNuke.cmake` ignores it due to `NO_SYSTEM_ENVIRONMENT_PATH`. Always pass `-D Nuke_ROOT=/usr/local/nuke_install`.
- **Sharing build directories between Linux and Windows:** Use separate build subdirectories per platform (e.g. `build/16.0-linux/` vs `build/16.0-windows/`) to avoid CMake cache conflicts.
- **Expanding `$GLOBAL_TOOLCHAIN` on the host:** This variable exists only inside the Windows container. Escape it as `\$GLOBAL_TOOLCHAIN` in bash -c strings.
- **Using `powershell Compress-Archive` for Windows zips:** Unnecessary since `docker-build.sh` always runs on Linux — use `zip` for both archive types.
- **Interactive prompts or uname branching:** The script always runs on Linux; no platform detection needed.

---

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Nuke SDK availability in container | Custom Dockerfile with Nuke SDK | `ghcr.io/gillesvink/nukedockerbuild` images | Already maintained; handles licensing, SDK extraction, compiler toolchain setup |
| Windows cross-compilation toolchain | wine-msvc setup in custom image | `*-windows-latest` image | wine-msvc setup is non-trivial; requires accepting MS EULA; already solved |
| CMake cross-compilation toolchain file | Custom toolchain.cmake | `$GLOBAL_TOOLCHAIN` from Windows image | Toolchain is pre-configured inside the image |

**Key insight:** NukeDockerBuild solves the hardest problems (Windows cross-compilation from Linux, per-Nuke-version SDK isolation) — the script's job is only to orchestrate `docker run` invocations.

---

## Common Pitfalls

### Pitfall 1: FindNuke.cmake Ignores CMAKE_PREFIX_PATH

**What goes wrong:** CMake configure step fails with "Could not find Nuke" even though `CMAKE_PREFIX_PATH=/usr/local/nuke_install` is set in the container.
**Why it happens:** `FindNuke.cmake` uses `NO_SYSTEM_ENVIRONMENT_PATH` in its `find_library` calls. This flag disables using both `CMAKE_PREFIX_PATH` env var AND `CMAKE_PREFIX_PATH` CMake variable as search prefixes for `find_library`. The Nuke_ROOT guard at line 30 short-circuits all path logic correctly — but only if `Nuke_ROOT` is explicitly passed.
**How to avoid:** Always include `-D Nuke_ROOT=/usr/local/nuke_install` in every cmake invocation inside the container.
**Warning signs:** CMake error "NUKE_DDIMAGE_LIBRARY NOT FOUND" on first attempt.

### Pitfall 2: Shared Build Directory CMake Cache Conflicts

**What goes wrong:** Building Linux then Windows for the same version, both writing to `build/16.0/`, causes the second build to fail with stale cache values from the first build (e.g. wrong compiler, wrong platform).
**Why it happens:** CMake caches compiler detection in `CMakeCache.txt`. Reusing the same build directory between different toolchains causes detection failures.
**How to avoid:** Use separate build subdirectories: `build/${version}-linux/` and `build/${version}-windows/`.
**Warning signs:** CMake errors about inconsistent or cached compiler settings on the second platform build.

### Pitfall 3: $GLOBAL_TOOLCHAIN Expanded by Host Shell

**What goes wrong:** Windows cmake invocation fails because the toolchain path expands to empty string on the host.
**Why it happens:** `$GLOBAL_TOOLCHAIN` is only defined inside the Windows container, not on the host. If the bash -c string uses double quotes without escaping, the host shell expands it to empty.
**How to avoid:** Use `\$GLOBAL_TOOLCHAIN` (escaped dollar sign) inside the docker `bash -c "..."` string so expansion happens inside the container.
**Warning signs:** CMake error "toolchain file '' not found" or empty TOOLCHAIN path.

### Pitfall 4: Image Unavailable for a Requested Version

**What goes wrong:** `docker run` returns non-zero exit code with "manifest unknown" or "image not found" when a version like `16.1` is requested.
**Why it happens:** NukeDockerBuild only builds images for released Nuke minor versions. A version in `NUKE_VERSIONS` that has no published image will fail the pull.
**How to avoid:** Check image availability before the build loop. On failure, print a clear error message and continue with remaining versions (or exit 1 — per Claude's discretion). Do not silently skip.
**Warning signs:** `docker pull` exits non-zero; "manifest unknown: manifest unknown" error from GHCR.

### Pitfall 5: Install Directory Collision Between Linux and Windows

**What goes wrong:** Linux and Windows builds for the same Nuke version both install into `install/${version}/`, with Windows `.dll` files overwriting Linux `.so` files or vice versa.
**Why it happens:** Both CMake invocations use the same `CMAKE_INSTALL_PREFIX`.
**How to avoid:** Use separate install paths: `install/${version}-linux/` and `install/${version}-windows/`. Zip archives can then be named correctly. Alternatively, if output artifacts don't share filenames (`.so` vs `.dll`), the same install dir works — but research shows Nuke plugins use the same base filename with different suffixes, so separation is safer.

---

## Code Examples

### Full Linux Docker Run Invocation

```bash
# Source: NukeDockerBuild README + Dockerfile ENV analysis + FindNuke.cmake line 30 guard
BASEDIR="$(pwd)"
NUKE_VERSION="16.0"
LINUX_IMAGE="ghcr.io/gillesvink/nukedockerbuild:${NUKE_VERSION}-linux-slim-latest"

docker run --rm \
    -v "${BASEDIR}:/nuke_build_directory" \
    "${LINUX_IMAGE}" \
    bash -c "
        cmake -S /nuke_build_directory \
              -B /nuke_build_directory/build/${NUKE_VERSION}-linux \
              -D CMAKE_INSTALL_PREFIX=/nuke_build_directory/install/${NUKE_VERSION}-linux \
              -D Nuke_ROOT=/usr/local/nuke_install &&
        cmake --build /nuke_build_directory/build/${NUKE_VERSION}-linux --config Release &&
        cmake --install /nuke_build_directory/build/${NUKE_VERSION}-linux
    "
```

### Full Windows Docker Run Invocation

```bash
# Source: NukeDockerBuild README + NukePluginTemplate .github/workflows/build.yaml
BASEDIR="$(pwd)"
NUKE_VERSION="16.0"
WINDOWS_IMAGE="ghcr.io/gillesvink/nukedockerbuild:${NUKE_VERSION}-windows-latest"

docker run --rm \
    -v "${BASEDIR}:/nuke_build_directory" \
    "${WINDOWS_IMAGE}" \
    bash -c "
        cmake -S /nuke_build_directory \
              -B /nuke_build_directory/build/${NUKE_VERSION}-windows \
              -GNinja \
              -DCMAKE_SYSTEM_NAME=Windows \
              -DCMAKE_BUILD_TYPE=Release \
              -DCMAKE_TOOLCHAIN_FILE=\$GLOBAL_TOOLCHAIN \
              -D CMAKE_INSTALL_PREFIX=/nuke_build_directory/install/${NUKE_VERSION}-windows \
              -D Nuke_ROOT=/usr/local/nuke_install &&
        cmake --build /nuke_build_directory/build/${NUKE_VERSION}-windows --config Release &&
        cmake --install /nuke_build_directory/build/${NUKE_VERSION}-windows
    "
```

### Zip Archive Creation (Matching batchInstall.sh)

```bash
# Source: batchInstall.sh lines 82, 80 — using zip on Linux for both platforms
zip -r "./release/DeepC-Linux-Nuke${NUKE_VERSION}.zip"   "${BASEDIR}/install/${NUKE_VERSION}-linux"
zip -r "./release/DeepC-Windows-Nuke${NUKE_VERSION}.zip" "${BASEDIR}/install/${NUKE_VERSION}-windows"
```

### Image Availability Check

```bash
# Check before attempting a build to give clear error messages
check_image_available() {
    local image_tag="$1"
    if ! docker manifest inspect "${image_tag}" > /dev/null 2>&1; then
        echo "ERROR: Docker image not available: ${image_tag}" >&2
        return 1
    fi
    return 0
}
```

Note: `docker manifest inspect` requires Docker BuildKit or experimental features. Alternatively, use `docker pull "${image_tag}" && docker image inspect "${image_tag}"` as a fallback check.

---

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| Local Nuke SDK install + batchInstall.sh | NukeDockerBuild containers | NukeDockerBuild introduced ~2023 | Developers without Nuke license can now build; CI-friendly |
| Windows build requires Windows host | Cross-compilation via wine-msvc in Linux container | wine-msvc approach | Single Linux developer can produce both platforms |

**Deprecated/outdated:**
- `NukeDockerBuild` tag format `nukedockerbuild:VERSION-linux`: The actual registry path is `ghcr.io/gillesvink/nukedockerbuild` with tags like `16.0-linux-slim-latest`. The README examples use short local names; always use the full GHCR path.

---

## Available NukeDockerBuild Versions

**Confirmed available (as of 2026-03-17, from Dockerfiles directory listing):**
- 10.0, 10.5, 11.0, 11.1, 11.2, 11.3, 12.0, 12.1, 12.2, 13.0, 13.1, 13.2, 14.0, 14.1, 15.0, 15.1, 15.2, **16.0**

**Confirmed NOT available:**
- 16.1 — no Dockerfile exists in repository
- 17.0 — no Dockerfile exists in repository

**Decision for `NUKE_VERSIONS` default array:** `("16.0")` only. Do not add 16.1 or 17.0.

**Confidence:** MEDIUM — confirmed from GitHub Dockerfiles directory tree listing. GHCR image publication is automated from those Dockerfiles; assumed published for all listed versions.

---

## Open Questions

1. **Separate install dirs vs shared install dir for Linux and Windows**
   - What we know: Linux produces `.so` files, Windows produces `.dll` files and import libs
   - What's unclear: Whether CMake install rules write to the same relative paths (same `init.py`, icons, etc.)
   - Recommendation: Use separate install dirs (`install/16.0-linux/` and `install/16.0-windows/`) to be safe; zip them separately. This avoids any filename collision risk.

2. **`docker manifest inspect` availability on target developer machine**
   - What we know: Requires Docker BuildKit or experimental features enabled
   - What's unclear: Whether the developer's Docker daemon has this enabled by default
   - Recommendation: Use `docker pull` for the availability check (slower but universally supported), or skip pre-check and let `docker run` fail naturally with a clear `set -e` exit.

3. **CMakeLists.txt CXX_STANDARD and Windows cross-compilation**
   - What we know: `CMakeLists.txt` sets `CXX_STANDARD 17` for Nuke >= 15.0. The Windows image provides MSVC 17 via wine-msvc.
   - What's unclear: Whether `CXXFLAGS=-std=c++17` set by the Linux image causes issues with MSVC (MSVC uses `/std:c++17` not `-std=c++17`).
   - Recommendation: The `GLOBAL_TOOLCHAIN` in the Windows image likely handles compiler flag normalization. Monitor for compile errors on the Windows build; if CXXFLAGS bleeds through from the Linux image layers, it may need to be cleared.

---

## Validation Architecture

### Test Framework

| Property | Value |
|----------|-------|
| Framework | None detected — shell script only |
| Config file | N/A |
| Quick run command | `bash -n docker-build.sh` (syntax check) |
| Full suite command | `bash docker-build.sh --versions 16.0 --linux` (real build, ~2-5 min) |

### Phase Requirements to Test Map

| Behavior | Test Type | Automated Command | Notes |
|----------|-----------|-------------------|-------|
| Script syntax is valid | smoke | `bash -n docker-build.sh` | < 1s |
| `--linux` flag builds Linux artifacts | manual/integration | `bash docker-build.sh --versions 16.0 --linux` | Requires Docker |
| `--windows` flag builds Windows artifacts | manual/integration | `bash docker-build.sh --versions 16.0 --windows` | Requires Docker |
| Output zip exists at expected path | manual/integration | check `release/DeepC-Linux-Nuke16.0.zip` exists | Part of full build |
| `--versions` override uses specified list | smoke | `bash -x docker-build.sh --versions 16.0 --linux 2>&1 \| grep NUKE_VERSION` | Dry-run trace |

### Sampling Rate

- **Per task commit:** `bash -n docker-build.sh` (syntax check only — no Docker dependency)
- **Per wave merge:** Full build test: `bash docker-build.sh --versions 16.0 --linux`
- **Phase gate:** Both `--linux` and `--windows` produce correct zip archives before `/gsd:verify-work`

### Wave 0 Gaps

- [ ] Docker must be available on developer machine — no automated installer; document as prerequisite in script header comment
- [ ] `release/` and `install/` directories — created by the script itself; no pre-existing test fixtures needed
- [ ] No test framework install needed — shell script validation uses `bash -n`

---

## Sources

### Primary (HIGH confidence)

- `https://github.com/gillesvink/NukeDockerBuild/blob/main/dockerfiles/16.0/linux/Dockerfile` — confirmed `CMAKE_PREFIX_PATH=/usr/local/nuke_install`, `NUKE_VERSION`, working dir `/nuke_build_directory`
- `https://github.com/gillesvink/NukeDockerBuild/blob/main/dockerfiles/16.0/windows/Dockerfile` — confirmed `GLOBAL_TOOLCHAIN=/nukedockerbuild/toolchain.cmake`, MSVC 17 via wine-msvc
- `/workspace/cmake/FindNuke.cmake` — confirmed `NO_SYSTEM_ENVIRONMENT_PATH` on find_library calls; confirmed Nuke_ROOT guard at line 30
- `/workspace/batchInstall.sh` — confirmed `zip` usage for Linux archives; confirmed cmake invocation pattern

### Secondary (MEDIUM confidence)

- `https://github.com/gillesvink/NukePluginTemplate/blob/main/.github/workflows/build.yaml` — confirmed `ghcr.io/gillesvink/nukedockerbuild` as registry; confirmed `-linux-slim-latest` and `-windows-latest` tag suffixes; confirmed `--rm -v` pattern
- `https://github.com/gillesvink/NukeDockerBuild/blob/main/README.md` — confirmed Nuke installed at `/usr/local/nuke_install`; confirmed Windows image uses Ninja + `CMAKE_SYSTEM_NAME=Windows`
- `https://github.com/gillesvink/NukeDockerBuild/tree/main/dockerfiles` — confirmed version list up to 16.0; confirmed 16.1 and 17.0 do not exist

### Tertiary (LOW confidence)

- WebSearch results for NukeDockerBuild — project description and general approach confirmed; version-specific availability not independently verified against GHCR registry (GHCR tag list was inaccessible)

---

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH — confirmed from Dockerfiles and NukePluginTemplate CI workflow
- Architecture / cmake invocation: HIGH — confirmed from Dockerfiles (env vars), FindNuke.cmake (NO_SYSTEM_ENVIRONMENT_PATH), and NukePluginTemplate workflow
- Nuke version availability: MEDIUM — confirmed from Dockerfiles directory tree; GHCR image list not directly verified
- Pitfalls: HIGH — FindNuke.cmake NO_SYSTEM_ENVIRONMENT_PATH confirmed from source; other pitfalls derived from confirmed architecture facts
- Windows cross-compilation mechanism: HIGH — confirmed from Dockerfile (wine-msvc, GLOBAL_TOOLCHAIN, Ninja)

**Research date:** 2026-03-17
**Valid until:** 2026-06-17 (stable tooling; NukeDockerBuild adds versions slowly)
