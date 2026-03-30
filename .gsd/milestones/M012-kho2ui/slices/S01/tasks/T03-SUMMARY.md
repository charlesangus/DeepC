---
id: T03
parent: S01
milestone: M012-kho2ui
provides: []
requires: []
affects: []
key_files: ["crates/opendefocus-deep/include/opendefocus_deep.h", "crates/opendefocus-deep/src/lib.rs", "src/DeepCOpenDefocus.cpp"]
key_decisions: ["Dropped the now-unused Quality enum import from lib.rs; quality.into() maps the raw i32 directly", "kQualityEntries placed as file-scope static before the class definition to keep null-terminator convention clear"]
patterns_established: []
drill_down_paths: []
observability_surfaces: []
duration: ""
verification_result: "bash scripts/verify-s01-syntax.sh passes for all three C++ files. grep -c Enumeration_knob returns 1. grep -c quality in header returns 4 (>=2). cargo test -p opendefocus-deep passes both tests with identical output values to prior baselines."
completed_at: 2026-03-30T23:06:56.516Z
blocker_discovered: false
---

# T03: Wired quality: i32 end-to-end from C++ Enumeration_knob through both FFI entry points and render_impl to Settings.render.quality, replacing hardcoded Quality::Low

> Wired quality: i32 end-to-end from C++ Enumeration_knob through both FFI entry points and render_impl to Settings.render.quality, replacing hardcoded Quality::Low

## What Happened
---
id: T03
parent: S01
milestone: M012-kho2ui
key_files:
  - crates/opendefocus-deep/include/opendefocus_deep.h
  - crates/opendefocus-deep/src/lib.rs
  - src/DeepCOpenDefocus.cpp
key_decisions:
  - Dropped the now-unused Quality enum import from lib.rs; quality.into() maps the raw i32 directly
  - kQualityEntries placed as file-scope static before the class definition to keep null-terminator convention clear
duration: ""
verification_result: passed
completed_at: 2026-03-30T23:06:56.516Z
blocker_discovered: false
---

# T03: Wired quality: i32 end-to-end from C++ Enumeration_knob through both FFI entry points and render_impl to Settings.render.quality, replacing hardcoded Quality::Low

**Wired quality: i32 end-to-end from C++ Enumeration_knob through both FFI entry points and render_impl to Settings.render.quality, replacing hardcoded Quality::Low**

## What Happened

Added int quality as the last parameter to both opendefocus_deep_render and opendefocus_deep_render_holdout in the FFI header. Updated both #[no_mangle] pub extern "C" functions and render_impl in lib.rs with quality: i32, replacing settings.render.quality = Quality::Low.into() with settings.render.quality = quality.into(). Removed the now-unused Quality import. Updated both test call sites to pass 0. In DeepCOpenDefocus.cpp added kQualityEntries (null-terminated), _quality member initialised to 0, Enumeration_knob in knobs(), and (int)_quality at both FFI call sites.

## Verification

bash scripts/verify-s01-syntax.sh passes for all three C++ files. grep -c Enumeration_knob returns 1. grep -c quality in header returns 4 (>=2). cargo test -p opendefocus-deep passes both tests with identical output values to prior baselines.

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `bash scripts/verify-s01-syntax.sh` | 0 | ✅ pass | 2000ms |
| 2 | `grep -c 'Enumeration_knob' src/DeepCOpenDefocus.cpp | grep -q '^1$'` | 0 | ✅ pass | 50ms |
| 3 | `grep -c 'quality' crates/opendefocus-deep/include/opendefocus_deep.h | awk '{exit ($1 < 2)}'` | 0 | ✅ pass | 50ms |
| 4 | `~/.cargo/bin/cargo test -p opendefocus-deep --features 'opendefocus/protobuf-vendored' -- --nocapture` | 0 | ✅ pass | 4700ms |


## Deviations

Removed the Quality enum from the use import block since quality.into() on a raw i32 requires no named constant.

## Known Issues

None.

## Files Created/Modified

- `crates/opendefocus-deep/include/opendefocus_deep.h`
- `crates/opendefocus-deep/src/lib.rs`
- `src/DeepCOpenDefocus.cpp`


## Deviations
Removed the Quality enum from the use import block since quality.into() on a raw i32 requires no named constant.

## Known Issues
None.
