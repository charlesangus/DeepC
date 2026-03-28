---
id: T02
parent: S02
milestone: M009-mso5fb
provides: []
requires: []
affects: []
key_files: ["crates/opendefocus-deep/src/lib.rs", "crates/opendefocus-deep/Cargo.toml", "scripts/verify-s01-syntax.sh"]
key_decisions: ["POSIX sh compat: replaced BASH_SOURCE[0] with $0 and set -euo pipefail with set -eu", "CoC driven by opendefocus internal CameraData + DefocusMode::Depth — no circle-of-confusion direct dep needed", "Renderer created per-call for safety; singleton optimisation deferred to S03"]
patterns_established: []
drill_down_paths: []
observability_surfaces: []
duration: ""
verification_result: "sh scripts/verify-s01-syntax.sh exits 0 with POSIX sh. cargo metadata --no-deps parses Cargo.toml and shows opendefocus, futures, once_cell deps. Brace balance confirmed (52==52). Full cargo build deferred to T04 docker build (rustc 1.75 local is too old for opendefocus edition 2024)."
completed_at: 2026-03-28T11:00:27.369Z
blocker_discovered: false
---

# T02: Implemented full layer-peel deep defocus engine in lib.rs with opendefocus GPU dispatch; added Cargo deps; fixed verify-s01-syntax.sh for POSIX sh compatibility

> Implemented full layer-peel deep defocus engine in lib.rs with opendefocus GPU dispatch; added Cargo deps; fixed verify-s01-syntax.sh for POSIX sh compatibility

## What Happened
---
id: T02
parent: S02
milestone: M009-mso5fb
key_files:
  - crates/opendefocus-deep/src/lib.rs
  - crates/opendefocus-deep/Cargo.toml
  - scripts/verify-s01-syntax.sh
key_decisions:
  - POSIX sh compat: replaced BASH_SOURCE[0] with $0 and set -euo pipefail with set -eu
  - CoC driven by opendefocus internal CameraData + DefocusMode::Depth — no circle-of-confusion direct dep needed
  - Renderer created per-call for safety; singleton optimisation deferred to S03
duration: ""
verification_result: passed
completed_at: 2026-03-28T11:00:27.369Z
blocker_discovered: false
---

# T02: Implemented full layer-peel deep defocus engine in lib.rs with opendefocus GPU dispatch; added Cargo deps; fixed verify-s01-syntax.sh for POSIX sh compatibility

**Implemented full layer-peel deep defocus engine in lib.rs with opendefocus GPU dispatch; added Cargo deps; fixed verify-s01-syntax.sh for POSIX sh compatibility**

## What Happened

The verification gate failure was caused by verify-s01-syntax.sh using bash-specific constructs (BASH_SOURCE[0], set -euo pipefail) that fail when invoked with /bin/sh. Fixed by replacing with POSIX equivalents. For T02 proper: updated Cargo.toml with opendefocus 0.1.10 (std+wgpu features), futures 0.3, once_cell 1. Implemented the full opendefocus_deep_render() body using the layer-peeling approach: reconstruct per-pixel deep samples from flat FFI arrays, sort front-to-back by depth, for each layer build flat RGBA + depth arrays, configure opendefocus Settings with CameraData (driving internal CoC from lens params), call render_stripe via block_on, and alpha-composite front-to-back into the accumulator. circle-of-confusion dep was intentionally omitted as it is a transitive dep with no direct public API needed — CoC computation is internal to opendefocus when DefocusMode::Depth + camera_data is set.

## Verification

sh scripts/verify-s01-syntax.sh exits 0 with POSIX sh. cargo metadata --no-deps parses Cargo.toml and shows opendefocus, futures, once_cell deps. Brace balance confirmed (52==52). Full cargo build deferred to T04 docker build (rustc 1.75 local is too old for opendefocus edition 2024).

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `sh scripts/verify-s01-syntax.sh` | 0 | ✅ pass | 1100ms |
| 2 | `bash scripts/verify-s01-syntax.sh` | 0 | ✅ pass | 1100ms |
| 3 | `cargo metadata --no-deps --format-version 1 --manifest-path crates/opendefocus-deep/Cargo.toml` | 0 | ✅ pass | 120ms |


## Deviations

circle-of-confusion dep omitted from Cargo.toml (no standalone compute() public API needed — CoC is internal). Renderer created per-call rather than singleton (safer initial impl; singleton optimisation is S03 scope).

## Known Issues

Full cargo build not verifiable locally (rustc 1.75 vs edition 2024 requirement). The settings.defocus.camera_data field path was inferred from upstream sources; T04 docker build will confirm or surface a compile error.

## Files Created/Modified

- `crates/opendefocus-deep/src/lib.rs`
- `crates/opendefocus-deep/Cargo.toml`
- `scripts/verify-s01-syntax.sh`


## Deviations
circle-of-confusion dep omitted from Cargo.toml (no standalone compute() public API needed — CoC is internal). Renderer created per-call rather than singleton (safer initial impl; singleton optimisation is S03 scope).

## Known Issues
Full cargo build not verifiable locally (rustc 1.75 vs edition 2024 requirement). The settings.defocus.camera_data field path was inferred from upstream sources; T04 docker build will confirm or surface a compile error.
