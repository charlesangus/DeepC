---
id: T01
parent: S02
milestone: M007-gvtoom
provides:
  - Full thin-lens CoC scatter engine in DeepCDefocusPOThin::renderStripe
  - Mock header fixes for chanNo/writableAt(int,int,int)/ChannelSet::contains
key_files:
  - src/DeepCDefocusPOThin.cpp
  - scripts/verify-s01-syntax.sh
key_decisions:
  - Channel mock changed from typedef-int to enum to enable writableAt overload resolution
patterns_established:
  - Option B warp: poly out5[0:1] used as warped aperture position, clamped to ap_radius, scaled to CoC
  - Green-channel (0.55μm) landing position reused for alpha accumulation
observability_surfaces:
  - error("Cannot open lens file: %s") on poly load failure
  - Zero-output lambda for diagnosable black output on early-return paths
duration: 15m
verification_result: passed
completed_at: 2026-03-24
blocker_discovered: false
---

# T01: Implement thin-lens scatter engine in renderStripe + fix mock header

**Replaced zero-output renderStripe stub with full thin-lens CoC scatter engine using poly-warped aperture sampling, per-channel CA, holdout transmittance, and chanNo-guarded writes.**

## What Happened

Replaced the zero-output `renderStripe` stub in `DeepCDefocusPOThin.cpp` with the complete thin-lens scatter engine implementing Option B from D032. The engine: (1) loads/reloads the polynomial lens system at renderStripe entry (M006 thread-safety pattern), (2) zeros output buffer via `chanNo`-indexed writes, (3) fetches holdout deep plane and defines `transmittance_at` lambda with depth gate, (4) fetches deep input and runs a 4-level nested scatter loop (pixel → deep sample → aperture sample → CA channel), (5) computes CoC via `coc_radius()`, generates aperture samples via `halton`+`map_to_disk`, evaluates polynomial at three wavelengths (0.45/0.55/0.65 μm), applies Option B warp (clamp magnitude to `ap_radius`, scale to CoC), converts to pixel coords, applies holdout transmittance, and accumulates via `writableAt(x, y, chanNo(chan))` with `chans.contains` guards. Alpha uses the green-channel landing position.

Also fixed the mock headers in `verify-s01-syntax.sh`: changed `Channel` from `typedef int` to `enum` so `writableAt(int,int,int)` overload resolves distinctly from `writableAt(int,int,Channel)`, added `chanNo(Channel)` to `ImagePlane`, and added `contains(Channel)` to `ChannelSet`. Updated the `foreach` macro for enum compatibility.

## Verification

- `bash scripts/verify-s01-syntax.sh` exits 0 — all 4 source files pass syntax check plus S05 contracts
- All 8 grep contracts pass (coc_radius, halton, map_to_disk, chanNo, transmittance_at, 0.45f, _max_degree, chans.contains)
- Diagnostic failure-path check passes (error message string present)

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `bash scripts/verify-s01-syntax.sh` | 0 | ✅ pass | 3s |
| 2 | `grep -q 'coc_radius' src/DeepCDefocusPOThin.cpp` | 0 | ✅ pass | <1s |
| 3 | `grep -q 'halton' src/DeepCDefocusPOThin.cpp` | 0 | ✅ pass | <1s |
| 4 | `grep -q 'map_to_disk' src/DeepCDefocusPOThin.cpp` | 0 | ✅ pass | <1s |
| 5 | `grep -q 'chanNo' src/DeepCDefocusPOThin.cpp` | 0 | ✅ pass | <1s |
| 6 | `grep -q 'transmittance_at' src/DeepCDefocusPOThin.cpp` | 0 | ✅ pass | <1s |
| 7 | `grep -q '0.45f' src/DeepCDefocusPOThin.cpp` | 0 | ✅ pass | <1s |
| 8 | `grep -q '_max_degree' src/DeepCDefocusPOThin.cpp` | 0 | ✅ pass | <1s |
| 9 | `grep -q 'chans.contains' src/DeepCDefocusPOThin.cpp` | 0 | ✅ pass | <1s |
| 10 | `grep -q 'error.*Cannot open lens file' src/DeepCDefocusPOThin.cpp` | 0 | ✅ pass | <1s |

## Diagnostics

- **Poly load failure:** `error("Cannot open lens file: %s")` visible in Nuke error console.
- **Black output debugging:** If renderStripe produces black despite valid inputs, check: `_poly_loaded` (missing .fit), `_aperture_samples` (zero = no iterations), bounds mismatch (all samples land outside tile).
- **Scatter verification:** Runtime proof requires `nuke -x test/test_thin.nk` (deferred to T02 + docker build).

## Deviations

- Changed `Channel` from `typedef int` to `enum` in the mock Channel.h to resolve the `writableAt` overload ambiguity (both `writableAt(int,int,Channel)` and `writableAt(int,int,int)` had identical signatures when Channel was int). This required adding explicit `static_cast<Channel>()` in ChannelSet methods and updating the `foreach` macro to use `static_cast<int>(VAR) != 0` instead of implicit bool conversion. This matches real DDImage where Channel is an enum type.

## Known Issues

- Slice-level verification checks for `test/test_thin.nk` are not yet passing (T02 scope).

## Files Created/Modified

- `src/DeepCDefocusPOThin.cpp` — Replaced zero-output renderStripe with full thin-lens scatter engine
- `scripts/verify-s01-syntax.sh` — Added chanNo/writableAt(int,int,int) to ImagePlane mock, contains(Channel) to ChannelSet mock, changed Channel to enum
- `.gsd/milestones/M007-gvtoom/slices/S02/S02-PLAN.md` — Added Observability/Diagnostics section and diagnostic verification step
- `.gsd/milestones/M007-gvtoom/slices/S02/tasks/T01-PLAN.md` — Added Observability Impact section
