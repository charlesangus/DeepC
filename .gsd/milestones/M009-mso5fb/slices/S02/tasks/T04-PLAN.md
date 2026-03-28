---
estimated_steps: 9
estimated_files: 3
skills_used: []
---

# T04: Docker build verification: cargo + cmake produce DeepCOpenDefocus.so

Run docker-build.sh to verify the full pipeline compiles end-to-end with the new Cargo dep.

1. Run `bash docker-build.sh` (or the Linux-target docker run command extracted from it) and capture output.
2. Confirm cargo resolves opendefocus 0.1.10 + wgpu deps without error.
3. Confirm cmake links DeepCOpenDefocus.so successfully.
4. Capture the docker log line showing 'CPU fallback' or 'GPU backend' (if nuke test runs inside docker).
5. If nuke -x is available inside the docker image: run nuke -x test/test_opendefocus.nk and confirm exit 0 + non-black EXR.
6. If nuke is not in the docker image: confirm .so was produced and record the constraint — nuke test must be run separately in an env with Nuke installed.
7. Document any build errors encountered and their fixes.
8. Update scripts/verify-s01-syntax.sh to add a note about docker-only cargo verification.

## Inputs

- `docker-build.sh`
- `src/CMakeLists.txt`
- `crates/opendefocus-deep/Cargo.toml`

## Expected Output

- `Successful docker build log`
- `DeepCOpenDefocus.so in docker build output`

## Verification

docker-build.sh exits 0. DeepCOpenDefocus.so present in build output. cargo build --release log shows opendefocus dep resolved. Log contains 'Compiling opendefocus' and 'Compiling opendefocus-deep'.
