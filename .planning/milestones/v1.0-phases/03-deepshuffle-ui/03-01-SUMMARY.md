---
phase: 03-deepshuffle-ui
plan: 01
subsystem: build
tags: [cmake, qt6, automoc, nuke-ndk, deepshuffle]

# Dependency graph
requires:
  - phase: 02-deepcpmatte-gl-handles
    provides: "Working CMake build with OpenGL; pattern for per-target link additions after foreach loops"
provides:
  - "Qt6 6.5.3 headers and moc available via /opt/Qt/6.5.3/gcc_64"
  - "src/CMakeLists.txt wired with find_package(Qt6), CMAKE_AUTOMOC, and DeepCShuffle Qt6 link"
  - "Baseline DeepCShuffle build green with AUTOMOC active"
affects: [03-02, 03-03, 03-04]

# Tech tracking
tech-stack:
  added:
    - "Qt 6.5.3 (via /opt/Qt/6.5.3/gcc_64 — same version as Nuke 16 runtime; already present on build machine)"
  patterns:
    - "CMAKE_PREFIX_PATH append before find_package to point CMake at Qt6 cmake config dir"
    - "Conditional AUTOMOC: set(CMAKE_AUTOMOC ON) inside if(Qt6_FOUND) block"
    - "Per-target Qt6 link added after foreach loops that create targets, not inside add_nuke_plugin function"

key-files:
  created: []
  modified:
    - src/CMakeLists.txt

key-decisions:
  - "Used pre-installed Qt 6.5.3 at /opt/Qt/6.5.3/gcc_64 instead of apt install qt6-base-dev — exact version match with Nuke 16 runtime (6.5.3 vs 6.4.x from Bookworm), no sudo required"
  - "CMAKE_PREFIX_PATH append approach used instead of HINTS in find_package — both work; PREFIX_PATH is the canonical approach for non-standard Qt installs"
  - "target_sources(DeepCShuffle PRIVATE ShuffleMatrixWidget.h) omitted from this plan — file does not exist yet; AUTOMOC will pick it up automatically when Plan 02 creates it"
  - "Used Qt6::Core/Qt6::Gui/Qt6::Widgets (versioned targets) not Qt::Core — required when Qt6 is found via cmake config rather than unversioned aliases"

patterns-established:
  - "Qt6 discovery: list(APPEND CMAKE_PREFIX_PATH /opt/Qt/6.5.3/gcc_64) then find_package(Qt6 COMPONENTS Core Gui Widgets)"
  - "AUTOMOC placement: set(CMAKE_AUTOMOC ON) must follow find_package and be inside if(Qt6_FOUND) guard"

requirements-completed: [SHUF-01, SHUF-03, SHUF-04]

# Metrics
duration: 3min
completed: 2026-03-15
---

# Phase 3 Plan 01: DeepShuffle UI — Qt6 Build Infrastructure Summary

**Qt6 6.5.3 wired into DeepCShuffle CMake build via /opt/Qt/6.5.3/gcc_64, AUTOMOC enabled, baseline build green**

## Performance

- **Duration:** 3 min
- **Started:** 2026-03-15T14:33:53Z
- **Completed:** 2026-03-15T14:36:54Z
- **Tasks:** 3 (all complete)
- **Files modified:** 1

## Accomplishments

- Discovered Qt 6.5.3 pre-installed at `/opt/Qt/6.5.3/gcc_64` — exact version match with Nuke 16 runtime; no package installation needed
- Added `find_package(Qt6)`, `CMAKE_AUTOMOC`, and `target_link_libraries(DeepCShuffle Qt6::Core Qt6::Gui Qt6::Widgets)` to `src/CMakeLists.txt`
- Verified baseline build: `cmake --build /workspace/build --target DeepCShuffle` exits 0 with AUTOMOC running (`DeepCShuffle_autogen/mocs_compilation.cpp` compiled successfully)

## Task Commits

Each task was committed atomically:

1. **Task 1: Install Qt6 dev headers and verify moc** — No commit (Qt6 was already present at `/opt/Qt/6.5.3/gcc_64`; `moc` verified at `6.5.3`)
2. **Task 2: Add Qt6 find_package and AUTOMOC to CMakeLists.txt** — `a2dc62b` (chore)
3. **Task 3: Verify baseline build** — included in `a2dc62b` (build verified before commit)

**Plan metadata:** TBD (docs commit)

## Files Created/Modified

- `src/CMakeLists.txt` — Added Qt6 find_package with CMAKE_PREFIX_PATH hint, AUTOMOC guard, and per-target Qt6 link for DeepCShuffle

## Decisions Made

- Used `/opt/Qt/6.5.3/gcc_64` instead of `apt install qt6-base-dev`: The pre-installed Qt 6.5.3 is an exact version match with Nuke 16's bundled runtime (6.5.3), which eliminates the ABI mismatch risk the plan flagged for Bookworm's 6.4.x packages. No sudo needed.
- `CMAKE_PREFIX_PATH` append used rather than `HINTS` in `find_package`: standard CMake pattern for non-default Qt installs; more reliable than per-call HINTS.
- `target_sources(DeepCShuffle PRIVATE ShuffleMatrixWidget.h)` intentionally omitted: AUTOMOC will automatically process any `Q_OBJECT` header added to the src directory when Plan 02 creates it. Adding a reference to a non-existent file is unnecessary.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] Used pre-installed Qt 6.5.3 instead of apt install qt6-base-dev**
- **Found during:** Task 1 (Install Qt6 dev headers)
- **Issue:** `sudo apt-get update` requires a password; `latuser` only has NOPASSWD sudo for one specific script. apt cache has no Qt6 packages indexed. The plan's primary approach (`sudo apt-get install -y qt6-base-dev`) was blocked.
- **Fix:** Discovered Qt 6.5.3 fully installed at `/opt/Qt/6.5.3/gcc_64/` including `moc` at `libexec/moc`. Used `list(APPEND CMAKE_PREFIX_PATH "/opt/Qt/6.5.3/gcc_64")` before `find_package(Qt6)` to make CMake find it.
- **Files modified:** `src/CMakeLists.txt` (CMAKE_PREFIX_PATH line added)
- **Verification:** `cmake -S /workspace -B /workspace/build` set `Qt6_DIR=/opt/Qt/6.5.3/gcc_64/lib/cmake/Qt6` in CMakeCache; `/opt/Qt/6.5.3/gcc_64/libexec/moc --version` prints `moc 6.5.3`
- **Committed in:** `a2dc62b`

---

**Total deviations:** 1 auto-fixed (Rule 3 - blocking)
**Impact on plan:** Outcome is strictly better than the planned approach — Qt 6.5.3 exactly matches Nuke's runtime, eliminating the version mismatch risk the plan explicitly flagged. No scope creep.

## Issues Encountered

None beyond the deviation documented above.

## User Setup Required

None - no external service configuration required.

## Next Phase Readiness

- Qt6 6.5.3 headers and moc are available; CMake is configured to use them
- AUTOMOC is active for the DeepCShuffle target — any `Q_OBJECT` header in `src/` will be processed automatically
- Plan 02 can immediately create `ShuffleMatrixWidget.h` and `ShuffleMatrixKnob.h` without any additional build setup
- The Qt6 link (`Qt6::Core`, `Qt6::Gui`, `Qt6::Widgets`) is already wired into DeepCShuffle — Plan 02 code can include Qt headers directly

---
*Phase: 03-deepshuffle-ui*
*Completed: 2026-03-15*
