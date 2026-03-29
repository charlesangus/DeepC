---
id: T01
parent: S01
milestone: M011-qa62vv
provides: []
requires: []
affects: []
key_files: ["Cargo.toml", "crates/opendefocus-deep/Cargo.toml", "crates/opendefocus-kernel/Cargo.toml", "crates/opendefocus/Cargo.toml", "crates/opendefocus-shared/Cargo.toml", "crates/opendefocus-datastructure/Cargo.toml", "Cargo.lock"]
key_decisions: ["Used [patch.crates-io] in workspace root to redirect all four crates to local paths — preserves normalized crate manifests", "Installed Rust 1.94.1 via rustup because system Rust 1.75 cannot parse edition 2024 manifests from transitive deps", "Fetched protoc 29.3 from GitHub releases to /tmp since apt requires root and prost-build build scripts need it", "Patched edition=2024 to edition=2021 in extracted crate Cargo.toml files as compatibility measure"]
patterns_established: []
drill_down_paths: []
observability_surfaces: []
duration: ""
verification_result: "Ran `PROTOC=/tmp/protoc_dir/bin/protoc cargo check -p opendefocus-deep 2>&1 | grep -c '^error' | grep -q '^0$' && echo PASS` — output: PASS. Confirmed Cargo.lock is version 3."
completed_at: 2026-03-29T04:03:38.916Z
blocker_discovered: false
---

# T01: Extracted four opendefocus 0.1.10 crates to crates/ as workspace path deps via [patch.crates-io], installed Rust 1.94.1 and protoc, deleted v4 lockfile, cargo check -p opendefocus-deep passes with zero errors

> Extracted four opendefocus 0.1.10 crates to crates/ as workspace path deps via [patch.crates-io], installed Rust 1.94.1 and protoc, deleted v4 lockfile, cargo check -p opendefocus-deep passes with zero errors

## What Happened
---
id: T01
parent: S01
milestone: M011-qa62vv
key_files:
  - Cargo.toml
  - crates/opendefocus-deep/Cargo.toml
  - crates/opendefocus-kernel/Cargo.toml
  - crates/opendefocus/Cargo.toml
  - crates/opendefocus-shared/Cargo.toml
  - crates/opendefocus-datastructure/Cargo.toml
  - Cargo.lock
key_decisions:
  - Used [patch.crates-io] in workspace root to redirect all four crates to local paths — preserves normalized crate manifests
  - Installed Rust 1.94.1 via rustup because system Rust 1.75 cannot parse edition 2024 manifests from transitive deps
  - Fetched protoc 29.3 from GitHub releases to /tmp since apt requires root and prost-build build scripts need it
  - Patched edition=2024 to edition=2021 in extracted crate Cargo.toml files as compatibility measure
duration: ""
verification_result: passed
completed_at: 2026-03-29T04:03:38.917Z
blocker_discovered: false
---

# T01: Extracted four opendefocus 0.1.10 crates to crates/ as workspace path deps via [patch.crates-io], installed Rust 1.94.1 and protoc, deleted v4 lockfile, cargo check -p opendefocus-deep passes with zero errors

**Extracted four opendefocus 0.1.10 crates to crates/ as workspace path deps via [patch.crates-io], installed Rust 1.94.1 and protoc, deleted v4 lockfile, cargo check -p opendefocus-deep passes with zero errors**

## What Happened

Downloaded and extracted all four opendefocus 0.1.10 crates from static.crates.io to crates/. Used [patch.crates-io] in the workspace root to redirect registry deps to local paths — cleaner than editing normalized crate manifests individually. Two unplanned obstacles resolved: (1) installed Rust 1.94.1 via rustup because system Rust 1.75 cannot parse edition 2024 manifests from transitive deps; (2) fetched protoc 29.3 binary from GitHub releases because prost-build requires it and apt needs root. Patched edition 2024→2021 in extracted Cargo.toml files and switched opendefocus-deep's dep to path="../opendefocus". Deleted v4 Cargo.lock; cargo regenerated a v3 lockfile on first check.

## Verification

Ran `PROTOC=/tmp/protoc_dir/bin/protoc cargo check -p opendefocus-deep 2>&1 | grep -c '^error' | grep -q '^0$' && echo PASS` — output: PASS. Confirmed Cargo.lock is version 3.

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `PROTOC=/tmp/protoc_dir/bin/protoc cargo check -p opendefocus-deep 2>&1 | grep -c '^error' | grep -q '^0$' && echo PASS` | 0 | ✅ pass | 10000ms |
| 2 | `head -4 Cargo.lock | grep 'version = 3'` | 0 | ✅ pass | 100ms |


## Deviations

Used [patch.crates-io] instead of editing per-crate manifests. Installed Rust 1.94.1 via rustup (unplanned). Fetched protoc binary from GitHub (unplanned). Patched edition 2024→2021 in all four extracted Cargo.toml files.

## Known Issues

PROTOC env var must be set on every future cargo invocation touching opendefocus-datastructure. The protoc binary at /tmp/protoc_dir/bin/protoc is ephemeral and must be re-downloaded if /tmp is cleared.

## Files Created/Modified

- `Cargo.toml`
- `crates/opendefocus-deep/Cargo.toml`
- `crates/opendefocus-kernel/Cargo.toml`
- `crates/opendefocus/Cargo.toml`
- `crates/opendefocus-shared/Cargo.toml`
- `crates/opendefocus-datastructure/Cargo.toml`
- `Cargo.lock`


## Deviations
Used [patch.crates-io] instead of editing per-crate manifests. Installed Rust 1.94.1 via rustup (unplanned). Fetched protoc binary from GitHub (unplanned). Patched edition 2024→2021 in all four extracted Cargo.toml files.

## Known Issues
PROTOC env var must be set on every future cargo invocation touching opendefocus-datastructure. The protoc binary at /tmp/protoc_dir/bin/protoc is ephemeral and must be re-downloaded if /tmp is cleared.
