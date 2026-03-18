---
phase: 06-local-build-system
plan: 01
subsystem: infra
tags: [docker, cmake, bash, nuke, nukedockerbuild, build-system]

# Dependency graph
requires: []
provides:
  - docker-build.sh build orchestration script for Linux and Windows
  - NukeDockerBuild integration for zero-SDK local builds
  - Distributable zip archives at release/DeepC-{Platform}-Nuke{version}.zip
affects: [release, packaging, ci]

# Tech tracking
tech-stack:
  added: [NukeDockerBuild (ghcr.io/gillesvink/nukedockerbuild), zip]
  patterns: [per-platform isolated build dirs, container-based cross-compilation, escape GLOBAL_TOOLCHAIN in bash -c strings]

key-files:
  created:
    - docker-build.sh
  modified: []

key-decisions:
  - "NUKE_VERSIONS defaults to (\"16.0\") only -- no 16.1 or 17.0; locked decision from research"
  - "Use zip not tar.gz -- matches batchInstall.sh convention"
  - "Separate build and install dirs per platform (16.0-linux vs 16.0-windows) to avoid CMake cache conflicts"
  - "Always pass -D Nuke_ROOT=/usr/local/nuke_install explicitly -- FindNuke.cmake uses NO_SYSTEM_ENVIRONMENT_PATH"
  - "Escape \\$GLOBAL_TOOLCHAIN in bash -c string so it expands inside container, not on host"

patterns-established:
  - "Container-based cross-compilation: mount repo at /nuke_build_directory, run cmake fully inside Docker"
  - "Platform isolation: separate build/${version}-{platform} and install/${version}-{platform} dirs per invocation"

requirements-completed: [BUILD-01, BUILD-02, BUILD-03]

# Metrics
duration: ~10min
completed: 2026-03-18
---

# Phase 06 Plan 01: Local Build System Summary

**docker-build.sh using NukeDockerBuild Docker images to compile DeepC for Linux (.so) and Windows (.dll) from a single Linux host with no local Nuke SDK installation**

## Performance

- **Duration:** ~10 min
- **Started:** 2026-03-18T04:00:00Z
- **Completed:** 2026-03-18T04:35:05Z
- **Tasks:** 2 (1 auto + 1 human-verify checkpoint)
- **Files modified:** 1

## Accomplishments

- Created 186-line docker-build.sh with full argument parsing (--linux, --windows, --all, --versions, --help)
- Integrated ghcr.io/gillesvink/nukedockerbuild images for both Linux slim and Windows cross-compilation
- Implemented per-platform isolated build/install directories and zip archive creation at release/
- Human verification checkpoint passed -- user confirmed script is correct

## Task Commits

Each task was committed atomically:

1. **Task 1: Create docker-build.sh with full build orchestration** - `229d663` (feat)
2. **Task 2: Verify docker-build.sh works end-to-end** - human-verify checkpoint, approved (no code changes)

**Plan metadata:** (this commit)

## Files Created/Modified

- `docker-build.sh` - Non-interactive build orchestration script; builds DeepC for Nuke 16.0 on Linux and Windows via Docker; produces release/DeepC-{Platform}-Nuke{version}.zip archives

## Decisions Made

- Default version list is ("16.0") only -- research confirmed 16.0 is the stable target; 16.1 and 17.0 deliberately excluded
- zip archives chosen over tar.gz to match existing batchInstall.sh convention
- Separate build and install directories per platform prevents CMake cache conflicts when building both Linux and Windows in a single invocation
- Nuke_ROOT passed explicitly via -D flag because FindNuke.cmake uses NO_SYSTEM_ENVIRONMENT_PATH, meaning CMAKE_PREFIX_PATH is ignored
- $GLOBAL_TOOLCHAIN escaped as \\$GLOBAL_TOOLCHAIN inside the bash -c string so it is evaluated by the container shell, not the host

## Deviations from Plan

None - plan executed exactly as written.

## Issues Encountered

None.

## User Setup Required

None - no external service configuration required. Docker must be installed on the build host.

## Next Phase Readiness

- docker-build.sh is complete and human-verified
- Phase 06 is complete -- the local build system is fully operational
- Any developer with Docker can run `bash docker-build.sh` to produce distributable zip archives for DeepC on Linux and Windows for Nuke 16.0

---
*Phase: 06-local-build-system*
*Completed: 2026-03-18*
