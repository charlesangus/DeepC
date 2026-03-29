---
estimated_steps: 9
estimated_files: 1
skills_used: []
---

# T03: Docker build — confirm fork compiles in production build environment

Run the Docker build to confirm the full Cargo workspace (including all four forked opendefocus crates) and CMake build against the real Nuke 16.0 SDK both succeed. Exit 0 is the only pass criterion.

Command: `bash docker-build.sh --linux --versions 16.0`

Expected log lines confirming success:
- `Compiling opendefocus-kernel`
- `Compiling opendefocus-deep`
- `Linking CXX shared library ... DeepCOpenDefocus.so` (or similar)
- Docker container exits 0

If docker is not available or nukedockerbuild image is not present, document the failure clearly and check that `bash scripts/verify-s01-syntax.sh` passes as a minimum signal.

Note: protoc must be available at `/tmp/protoc_dir/bin/protoc`. If `/tmp` was cleared, re-fetch it: `mkdir -p /tmp/protoc_dir && curl -L https://github.com/protocolbuffers/protobuf/releases/download/v29.3/protoc-29.3-linux-x86_64.zip -o /tmp/protoc.zip && unzip -o /tmp/protoc.zip -d /tmp/protoc_dir`

## Inputs

- ``src/DeepCOpenDefocus.cpp` — updated in T01 (must be complete first)`
- ``docker-build.sh` — build orchestration script`
- ``Cargo.toml` — workspace root with [patch.crates-io] section`
- ``crates/opendefocus-deep/include/opendefocus_deep.h` — C header for FFI`

## Expected Output

- ``/tmp/docker-build-exit-code.txt` — record the exit code (create with `echo $? > /tmp/docker-build-exit-code.txt` after docker-build.sh)or confirm exit 0 in shell output`

## Verification

bash docker-build.sh --linux --versions 16.0; echo "docker exit: $?"
