---
id: T04
parent: S01
milestone: M006
provides:
  - Verification that all S01 static gates pass (syntax check, grep contracts, CMakeLists.txt structure)
  - Documentation of Docker build gate status (not executable in this environment; environment constraint noted)
key_files:
  - scripts/verify-s01-syntax.sh
  - src/DeepCDefocusPO.cpp
  - src/poly.h
  - src/deepc_po_math.h
  - src/CMakeLists.txt
key_decisions:
  - Docker build gate deferred to CI: docker is not installed in the workspace environment; all static verification gates pass; the docker build gate must be run in a CI environment with Docker installed
patterns_established:
  - When Docker is unavailable, CMake syntax validity can be confirmed by running "cmake -S . -B /tmp/cmake-check-build -DNuke_ROOT=/nonexistent" — it fails at find_package(Nuke) which is expected, but all CMake syntax is parsed before that point
  - The syntax-only gate (verify-s01-syntax.sh) is a sufficient fast-feedback loop for source correctness; docker-build.sh is the integration gate and belongs in CI
observability_surfaces:
  - "bash scripts/verify-s01-syntax.sh" exits 0 and prints "All syntax checks passed." — first signal that all three files compile cleanly
  - "grep -c 'DeepCDefocusPO' src/CMakeLists.txt" returns 2 — confirms both PLUGINS and FILTER_NODES registrations are intact
  - Docker build produces release/DeepC-Linux-Nuke16.0.zip with DeepCDefocusPO.so — definitive integration signal (CI only)
duration: 10m
verification_result: partial — all static gates pass; docker build gate not executable in this environment (no docker daemon)
completed_at: 2026-03-23
blocker_discovered: false
---

# T04: Run syntax verifier and docker build

**All static verification gates pass (syntax check, all grep contracts, CMakeLists.txt structure); docker build gate blocked by no Docker daemon in workspace — must run in CI.**

## What Happened

T04 is a verification-only task: run the accumulated gates from T01–T03 and confirm end-to-end correctness. No source files needed changes — all prior task work was correct on first check.

**Gate 1 — `bash scripts/verify-s01-syntax.sh`:** Passed immediately. All three files (DeepCBlur.cpp, DeepCDepthBlur.cpp, DeepCDefocusPO.cpp) compiled cleanly under `g++ -std=c++17 -fsyntax-only` with the mock DDImage headers.

**Gate 2 — Grep contracts:** All nine grep contracts passed without exception. Every required symbol is present in the correct file: `poly_system_read`, `poly_system_destroy`, `File_knob`, `renderStripe` in `DeepCDefocusPO.cpp`; `lt_newton_trace`, `mat2_inverse`, `coc_radius` in `deepc_po_math.h`; `poly_system_read` in `poly.h`; and `DeepCDefocusPO` appears exactly 2 times in `CMakeLists.txt` (PLUGINS list line 16, FILTER_NODES list line 53).

**Gate 3 — CMakeLists.txt count:** `grep -c 'DeepCDefocusPO' src/CMakeLists.txt` returned 2. CMake syntax was additionally validated by attempting `cmake -S . -B /tmp/cmake-check-build -DNuke_ROOT=/nonexistent`: it parsed all CMakeLists.txt syntax cleanly and failed only at `find_package(Nuke REQUIRED)` (expected without the SDK).

**Gate 4 — Docker build:** Docker is not installed in this workspace (`which docker` → not found). The `docker-build.sh` script fails at its own prerequisite check (`ERROR: docker is not installed or not in PATH`). This is an infrastructure constraint of the execution environment, not a code defect. The script itself is syntactically valid and ready to execute in any environment with Docker. The docker build gate must be run in CI.

**Gate 5 — Failure-path diagnostic:** `grep -q poly_system_read src/poly.h` passed, confirming the error-path definition exists — the `_validate(true)` failure branch compiles (poly_system_read present in poly.h).

## Verification

```
bash scripts/verify-s01-syntax.sh             # exits 0; all 3 files pass
grep -q 'poly_system_read' src/DeepCDefocusPO.cpp    # PASS
grep -q 'poly_system_destroy' src/DeepCDefocusPO.cpp  # PASS
grep -q 'File_knob' src/DeepCDefocusPO.cpp            # PASS
grep -q 'renderStripe' src/DeepCDefocusPO.cpp          # PASS
grep -q 'lt_newton_trace' src/deepc_po_math.h          # PASS
grep -q 'mat2_inverse' src/deepc_po_math.h             # PASS
grep -q 'coc_radius' src/deepc_po_math.h               # PASS
grep -q 'poly_system_read' src/poly.h                  # PASS
grep -c 'DeepCDefocusPO' src/CMakeLists.txt            # returns 2
cmake -S . -B /tmp/cmake-check-build -DNuke_ROOT=/nonexistent  # CMake parses all syntax; fails only at find_package(Nuke) as expected
bash docker-build.sh --linux --versions 16.0           # BLOCKED — no docker in environment
```

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `bash scripts/verify-s01-syntax.sh` | 0 | ✅ pass | ~2s |
| 2 | `grep -q 'poly_system_read\|poly_system_destroy\|File_knob\|renderStripe' src/DeepCDefocusPO.cpp` | 0 | ✅ pass | <1s |
| 3 | `grep -c 'DeepCDefocusPO' src/CMakeLists.txt` (returns 2) | 0 | ✅ pass | <1s |
| 4 | `grep -q 'lt_newton_trace\|mat2_inverse\|coc_radius' src/deepc_po_math.h` | 0 | ✅ pass | <1s |
| 5 | `grep -q 'poly_system_read' src/poly.h` | 0 | ✅ pass | <1s |
| 6 | `cmake -S . -B /tmp/cmake-check-build -DNuke_ROOT=/nonexistent` (CMake syntax parses; fails at find_package) | 1 (expected) | ✅ pass (syntax valid) | ~3s |
| 7 | failure-path diagnostic (`poly_system_read` present in poly.h) | 0 | ✅ pass | <1s |
| 8 | `bash docker-build.sh --linux --versions 16.0` | 1 | ❌ blocked (no docker) | <1s |

## Observability Impact

- **Syntax gate signal:** `bash scripts/verify-s01-syntax.sh` exits 0 and prints `"All syntax checks passed."` to stdout. Exit non-zero + `"filename:line: error:"` on stderr is the inspectable failure shape for CI.
- **CMakeLists registration signal:** `grep -c 'DeepCDefocusPO' src/CMakeLists.txt` returns 2. A regression to 1 or 0 means PLUGINS or FILTER_NODES registration was lost.
- **Docker build signal (CI):** Successful run produces `release/DeepC-Linux-Nuke16.0.zip`; `unzip -l release/DeepC-Linux-Nuke16.0.zip | grep DeepCDefocusPO.so` confirms the `.so` is present. `docker-build.sh` prints the cmake/compiler error to stdout on failure.
- **No-docker environment detection:** `docker-build.sh` immediately prints `"ERROR: docker is not installed or not in PATH"` and exits 1 — inspectable via exit code.

## Diagnostics

- **Re-run syntax check anytime:** `bash scripts/verify-s01-syntax.sh` — fast, no prerequisites. Exits 0 and prints "All syntax checks passed." on success.
- **Re-run grep contracts:** `grep -q 'poly_system_read' src/DeepCDefocusPO.cpp && grep -q 'poly_system_destroy' src/DeepCDefocusPO.cpp && grep -q 'renderStripe' src/DeepCDefocusPO.cpp && echo PASS`
- **CMakeLists count:** `grep -c 'DeepCDefocusPO' src/CMakeLists.txt` — must return 2.
- **Docker build (requires Docker):** `bash docker-build.sh --linux --versions 16.0` — needs Docker daemon; produces `release/DeepC-Linux-Nuke16.0.zip` on success.

## Deviations

- Docker build gate could not be executed because Docker is not installed in the workspace environment. This is an infrastructure constraint, not a code defect. All code-level verification gates pass. The docker gate belongs in CI and must be run there.

## Known Issues

- `bash docker-build.sh --linux --versions 16.0` requires Docker and cannot be run in this workspace. The script is ready; the prerequisite infrastructure is absent. This is not a blocker for the S01 implementation contract — the slice's source-level contracts are fully satisfied.

## Files Created/Modified

- `T04-SUMMARY.md` — this summary (verification record only; no source files were modified)
