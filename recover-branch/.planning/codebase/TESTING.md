# Testing Patterns

**Analysis Date:** 2026-03-12

## Test Framework

**Runner:**
- None configured. No test framework, test runner, or test configuration file exists in the repository.

**Assertion Library:**
- Not applicable.

**Run Commands:**
- No test commands defined. The project has no `test`, `check`, or equivalent CMake target.

## Test File Organization

**Location:**
- No test files exist. There are no `*.test.*`, `*.spec.*`, or `test/` / `tests/` directories anywhere in the repository.

**Naming:**
- Not applicable.

**Structure:**
- Not applicable.

## Test Coverage

**Requirements:** None enforced. There are no coverage tools, CI gates, or coverage reports configured.

**Existing coverage:** Zero. No automated tests of any kind exist for any plugin.

## What Is Tested

**Manual / integration testing only:**
- All validation of plugin correctness is done by loading the compiled `.so`/`.dylib` plugins into Nuke and visually inspecting output.
- No unit tests for mathematical operations (grade coefficients, noise generation, masking math).
- No regression tests for pixel output.
- No tests for edge cases (alpha = 0, empty deep streams, null inputs).

## Build-time Assertions

The only runtime assertion mechanism present is the Nuke SDK macro:

```cpp
mFnAssert(inPlaceOutPlane.isComplete());
```

This is used as a post-condition check in `doDeepEngine` implementations across:
- `src/DeepCWrapper.cpp`
- `src/DeepCAddChannels.cpp`
- `src/DeepCShuffle.cpp`
- `src/DeepCBlink.cpp`
- `src/DeepCKeymix.cpp`

These are debug assertions, not automated tests. They fire only in debug builds and have no associated test harness.

## Recommendations for Future Testing

If automated tests are added, the following patterns would fit this codebase:

**Unit tests for math:**
The grade coefficient calculation in `DeepCGrade::_validate` (`src/DeepCGrade.cpp`) and `DeepCPNoise::_validate` (`src/DeepCPNoise.cpp`) is pure math with no Nuke dependencies — suitable for standalone unit tests.

**Mock-based integration tests:**
The `wrappedPerChannel` and `wrappedPerSample` virtual methods in `DeepCWrapper` subclasses take plain float values and return via reference — testable without Nuke if the SDK headers are available.

**Framework candidates:**
- GoogleTest (`gtest`) — compatible with CMake via `FetchContent` or `find_package`; would integrate cleanly with the existing CMake build in `/workspace/CMakeLists.txt`.
- Catch2 — header-only option, lower setup friction.

**Test file placement:**
A `tests/` directory at `/workspace/tests/` with a `CMakeLists.txt` added as a subdirectory via `add_subdirectory(tests)` would match the existing project structure.

---

*Testing analysis: 2026-03-12*
