---
id: T03
parent: S01
milestone: M008-y32v8w
provides:
  - verify-s01-syntax.sh with complete mock headers and 13 S01 structural contracts
  - sphereToCs direction-only function restored in deepc_po_math.h (needed by Ray renderStripe)
key_files:
  - scripts/verify-s01-syntax.sh
  - src/deepc_po_math.h
key_decisions:
  - Restored M007 sphereToCs (direction-only) alongside T02's sphereToCs_full — Ray's renderStripe calls the simpler variant
patterns_established:
  - Verify script PASS/FAIL output pattern: each contract prints PASS on success, FAIL + exit 1 on failure, making broken contracts immediately identifiable
observability_surfaces:
  - bash scripts/verify-s01-syntax.sh — single gate for all S01 deliverables (syntax + contracts)
  - grep -c FAIL output of the script returns 0 when all contracts pass
duration: 6m
verification_result: passed
completed_at: 2026-03-25
blocker_discovered: false
---

# T03: Update verify-s01-syntax.sh with new mocks and contracts, verify all pass

**Updated verify script with DeepInfo::format() and Info::format() mocks, added 13 S01 structural grep contracts, restored sphereToCs function needed by Ray compilation — all syntax checks and contracts pass.**

## What Happened

Extracted the M007 version of `verify-s01-syntax.sh` from commit `89de100` (which already targets Thin/Ray instead of the old monolithic file). Added two missing mock methods: `DeepInfo::format()` returning `const Format*` (needed by the `_validate` format fix `*di.format()`), and `Info::format(const Format&)` setter (needed by `info_.format(...)`). Replaced the old S05 contracts with 13 new S01 structural contracts covering: format fix in both nodes, all 5 lens constant knobs in Ray, 3 math functions in deepc_po_math.h, max_degree in poly.h, and CMakeLists.txt registration for both plugins.

During compilation, discovered that Ray's `renderStripe` calls `sphereToCs` (direction-only variant from M007) which was not present in deepc_po_math.h — T02 only added `sphereToCs_full`. Restored the original M007 `sphereToCs` implementation from commit `89de100` into deepc_po_math.h, placed immediately before `sphereToCs_full`.

## Verification

Both slice-level verification commands pass: `bash scripts/verify-s01-syntax.sh` exits 0, and `grep -c FAIL` returns 0 (no failed contracts). All 4 source files compile (DeepCBlur, DeepCDepthBlur, DeepCDefocusPOThin, DeepCDefocusPORay) and all 13 S01 contracts pass.

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `bash scripts/verify-s01-syntax.sh` | 0 | ✅ pass | 3s |
| 2 | `bash scripts/verify-s01-syntax.sh 2>&1 \| grep -c FAIL` | 0 (count) | ✅ pass (0 failures) | 3s |

## Diagnostics

- Run `bash scripts/verify-s01-syntax.sh` — prints per-contract PASS/FAIL lines with descriptive labels.
- On failure, the script exits immediately with the failing contract name (e.g., "FAIL: _sensor_width knob missing in Ray").
- To check a specific contract manually: `grep -q 'PATTERN' src/FILE || echo FAIL`.

## Deviations

- Restored `sphereToCs` (direction-only) from M007 commit 89de100 into `deepc_po_math.h` — not in the task plan but required for Ray compilation. The M007 Ray code calls this function in its renderStripe, and T02 only added `sphereToCs_full`.

## Known Issues

None — all S01 deliverables verified.

## Files Created/Modified

- `scripts/verify-s01-syntax.sh` — Updated with DeepInfo::format() and Info::format() mocks, replaced S05 contracts with 13 S01 structural contracts
- `src/deepc_po_math.h` — Added sphereToCs direction-only function (restored from M007)
