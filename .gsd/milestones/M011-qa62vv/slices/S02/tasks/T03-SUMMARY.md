---
id: T03
parent: S02
milestone: M011-qa62vv
provides: []
requires: []
affects: []
key_files: ["release/DeepC-Linux-Nuke16.0.zip", "install/16.0-linux/DeepC/DeepCOpenDefocus.so"]
key_decisions: ["No code changes required — T01 and T02 already produced correct compilable sources; T03 is a pure verification step"]
patterns_established: []
drill_down_paths: []
observability_surfaces: []
duration: ""
verification_result: "Ran bash docker-build.sh --linux --versions 16.0 end-to-end. Verified exit code 0 written to /tmp/docker-build-exit-code.txt. Confirmed install/16.0-linux/DeepC/DeepCOpenDefocus.so (21 MB) present. Confirmed release/DeepC-Linux-Nuke16.0.zip (10 MB) produced. All expected log lines present: Compiling opendefocus-kernel, Compiling opendefocus-deep, Linking CXX shared module DeepCOpenDefocus.so."
completed_at: 2026-03-29T11:54:23.460Z
blocker_discovered: false
---

# T03: Docker build bash docker-build.sh --linux --versions 16.0 exited 0; all four forked Rust crates compiled and DeepCOpenDefocus.so linked in nukedockerbuild:16.0-linux

> Docker build bash docker-build.sh --linux --versions 16.0 exited 0; all four forked Rust crates compiled and DeepCOpenDefocus.so linked in nukedockerbuild:16.0-linux

## What Happened
---
id: T03
parent: S02
milestone: M011-qa62vv
key_files:
  - release/DeepC-Linux-Nuke16.0.zip
  - install/16.0-linux/DeepC/DeepCOpenDefocus.so
key_decisions:
  - No code changes required — T01 and T02 already produced correct compilable sources; T03 is a pure verification step
duration: ""
verification_result: passed
completed_at: 2026-03-29T11:54:23.462Z
blocker_discovered: false
---

# T03: Docker build bash docker-build.sh --linux --versions 16.0 exited 0; all four forked Rust crates compiled and DeepCOpenDefocus.so linked in nukedockerbuild:16.0-linux

**Docker build bash docker-build.sh --linux --versions 16.0 exited 0; all four forked Rust crates compiled and DeepCOpenDefocus.so linked in nukedockerbuild:16.0-linux**

## What Happened

The nukedockerbuild:16.0-linux image was already present locally. Ran bash docker-build.sh --linux --versions 16.0 as an async job (154 seconds total). The container installed protobuf-compiler via dnf, set up the cc symlink for Rocky Linux 8, installed rustup stable 1.94.1, then ran CMake against the Nuke 16.0v2 SDK. The full Cargo workspace compiled in 1m 15s including opendefocus-deep v0.1.0 (our forked crate with opendefocus_deep_render_holdout). All CMake targets built and DeepCOpenDefocus.so linked at step 53%. Install and zip steps completed, producing release/DeepC-Linux-Nuke16.0.zip (10 MB). Docker container exited 0.

## Verification

Ran bash docker-build.sh --linux --versions 16.0 end-to-end. Verified exit code 0 written to /tmp/docker-build-exit-code.txt. Confirmed install/16.0-linux/DeepC/DeepCOpenDefocus.so (21 MB) present. Confirmed release/DeepC-Linux-Nuke16.0.zip (10 MB) produced. All expected log lines present: Compiling opendefocus-kernel, Compiling opendefocus-deep, Linking CXX shared module DeepCOpenDefocus.so.

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `bash docker-build.sh --linux --versions 16.0; echo "docker exit: $?"` | 0 | ✅ pass | 154100ms |
| 2 | `ls -lh install/16.0-linux/DeepC/DeepCOpenDefocus.so` | 0 | ✅ pass | 100ms |
| 3 | `cat /tmp/docker-build-exit-code.txt` | 0 | ✅ pass | 100ms |


## Deviations

None. Docker image was already present; build succeeded first attempt.

## Known Issues

None. Qt6 missing generates a CMake warning on the Linux build path but does not prevent compilation (Qt6 is only required for the Windows GUI build).

## Files Created/Modified

- `release/DeepC-Linux-Nuke16.0.zip`
- `install/16.0-linux/DeepC/DeepCOpenDefocus.so`


## Deviations
None. Docker image was already present; build succeeded first attempt.

## Known Issues
None. Qt6 missing generates a CMake warning on the Linux build path but does not prevent compilation (Qt6 is only required for the Windows GUI build).
