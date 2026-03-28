---
id: T04
parent: S02
milestone: M009-mso5fb
provides: []
requires: []
affects: []
key_files: ["crates/opendefocus-deep/src/lib.rs", "crates/opendefocus-deep/Cargo.toml", "src/DeepCOpenDefocus.cpp", "scripts/verify-s01-syntax.sh"]
key_decisions: ["Added ndarray = 0.16 to Cargo.toml because lib.rs uses Array2/Array3 directly", "proto-generated enum fields require .into() for i32 assignment", "camera_data nested under settings.defocus.circle_of_confusion.camera_data", "focus_distance maps to focal_plane on CoC Settings; CameraData uses near_field/far_field", "Removed getRequirements override — DD::Image::Descriptions absent from Nuke 16.0 SDK", "cc symlink and protobuf-compiler required in nukedockerbuild Rocky Linux 8 container"]
patterns_established: []
drill_down_paths: []
observability_surfaces: []
duration: ""
verification_result: "1. docker run nukedockerbuild:16.0-linux full pipeline exit 0 (54.3s); 2. libopendefocus_deep.a 58 MB confirmed; 3. DeepCOpenDefocus.so 21 MB confirmed; 4. nm confirms opendefocus_deep_render T symbol; 5. bash scripts/verify-s01-syntax.sh exit 0. Build log shows Compiling opendefocus v0.1.10 and Compiling opendefocus-deep v0.1.0 with 0 errors."
completed_at: 2026-03-28T11:20:31.471Z
blocker_discovered: false
---

# T04: Docker build verified: cargo resolves opendefocus 0.1.10 (248 deps) and cmake links DeepCOpenDefocus.so (21 MB); six API mismatches fixed in lib.rs and DeepCOpenDefocus.cpp

> Docker build verified: cargo resolves opendefocus 0.1.10 (248 deps) and cmake links DeepCOpenDefocus.so (21 MB); six API mismatches fixed in lib.rs and DeepCOpenDefocus.cpp

## What Happened
---
id: T04
parent: S02
milestone: M009-mso5fb
key_files:
  - crates/opendefocus-deep/src/lib.rs
  - crates/opendefocus-deep/Cargo.toml
  - src/DeepCOpenDefocus.cpp
  - scripts/verify-s01-syntax.sh
key_decisions:
  - Added ndarray = 0.16 to Cargo.toml because lib.rs uses Array2/Array3 directly
  - proto-generated enum fields require .into() for i32 assignment
  - camera_data nested under settings.defocus.circle_of_confusion.camera_data
  - focus_distance maps to focal_plane on CoC Settings; CameraData uses near_field/far_field
  - Removed getRequirements override — DD::Image::Descriptions absent from Nuke 16.0 SDK
  - cc symlink and protobuf-compiler required in nukedockerbuild Rocky Linux 8 container
duration: ""
verification_result: passed
completed_at: 2026-03-28T11:20:31.471Z
blocker_discovered: false
---

# T04: Docker build verified: cargo resolves opendefocus 0.1.10 (248 deps) and cmake links DeepCOpenDefocus.so (21 MB); six API mismatches fixed in lib.rs and DeepCOpenDefocus.cpp

**Docker build verified: cargo resolves opendefocus 0.1.10 (248 deps) and cmake links DeepCOpenDefocus.so (21 MB); six API mismatches fixed in lib.rs and DeepCOpenDefocus.cpp**

## What Happened

The nukedockerbuild:16.0-linux Docker image (Rocky Linux 8 + Nuke 16.0v2 SDK) was used for the full build pipeline. Three pre-build blockers were resolved: (1) Rocky Linux 8 has gcc but no cc symlink — fixed with ln -sf /usr/bin/gcc /usr/local/bin/cc; (2) circle-of-confusion prost build script requires protoc — fixed with dnf install -y protobuf-compiler; (3) compiling against real opendefocus 0.1.10 revealed six API mismatches in lib.rs: missing ndarray dep, proto-generated i32 enum fields requiring .into(), wrong camera_data nesting path, wrong CameraData field names (no focal_point — use near_field/far_field + focal_plane), and type annotation needed for Array3. Additionally DD::Image::Descriptions does not exist in Nuke 16.0 DDImage SDK, requiring removal of the getRequirements override from DeepCOpenDefocus.cpp. After all fixes, cargo build produced libopendefocus_deep.a (58 MB) and cmake linked DeepCOpenDefocus.so (21 MB) with opendefocus_deep_render exported as a T symbol. Nuke is not in the Docker image, so nuke -x test verification must run in a separate Nuke environment.

## Verification

1. docker run nukedockerbuild:16.0-linux full pipeline exit 0 (54.3s); 2. libopendefocus_deep.a 58 MB confirmed; 3. DeepCOpenDefocus.so 21 MB confirmed; 4. nm confirms opendefocus_deep_render T symbol; 5. bash scripts/verify-s01-syntax.sh exit 0. Build log shows Compiling opendefocus v0.1.10 and Compiling opendefocus-deep v0.1.0 with 0 errors.

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `docker run nukedockerbuild:16.0-linux bash -c '...(full cargo+cmake pipeline)...'` | 0 | ✅ pass | 54300ms |
| 2 | `ls -lh target/release/libopendefocus_deep.a (inside docker)` | 0 | ✅ pass | 1ms |
| 3 | `ls -lh build/16.0-linux/src/DeepCOpenDefocus.so (inside docker)` | 0 | ✅ pass | 1ms |
| 4 | `nm build/16.0-linux/src/DeepCOpenDefocus.so | grep opendefocus_deep_render` | 0 | ✅ pass | 200ms |
| 5 | `bash scripts/verify-s01-syntax.sh` | 0 | ✅ pass | 1200ms |


## Deviations

lib.rs required 6 API fixes vs T02 implementation (no runtime compiler available in T02). getRequirements removed from .cpp (discovered as cmake blocker). ndarray added to Cargo.toml (was implicit dep in T02's plan but lib.rs uses it directly). Docker prerequisites (cc symlink, protoc) not in task plan but trivially fixed.

## Known Issues

Nuke not available in nukedockerbuild image — nuke -x test/test_opendefocus.nk must run in a separate Nuke installation. Unused RENDERER static emits dead_code warning — intentional scaffolding for S03 singleton.

## Files Created/Modified

- `crates/opendefocus-deep/src/lib.rs`
- `crates/opendefocus-deep/Cargo.toml`
- `src/DeepCOpenDefocus.cpp`
- `scripts/verify-s01-syntax.sh`


## Deviations
lib.rs required 6 API fixes vs T02 implementation (no runtime compiler available in T02). getRequirements removed from .cpp (discovered as cmake blocker). ndarray added to Cargo.toml (was implicit dep in T02's plan but lib.rs uses it directly). Docker prerequisites (cc symlink, protoc) not in task plan but trivially fixed.

## Known Issues
Nuke not available in nukedockerbuild image — nuke -x test/test_opendefocus.nk must run in a separate Nuke installation. Unused RENDERER static emits dead_code warning — intentional scaffolding for S03 singleton.
