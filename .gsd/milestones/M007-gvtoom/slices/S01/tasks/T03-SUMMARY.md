---
id: T03
parent: S01
milestone: M007-gvtoom
provides:
  - CMake build lists updated with DeepCDefocusPOThin and DeepCDefocusPORay
  - verify-s01-syntax.sh updated for new two-plugin file set
  - Old DeepCDefocusPO.cpp removed from repo
key_files:
  - src/CMakeLists.txt
  - scripts/verify-s01-syntax.sh
key_decisions:
  - S02/S03/S04 contract blocks removed from verify script since they referenced deleted DeepCDefocusPO.cpp; future slices will add their own contracts for the new files
patterns_established:
  - S05 contracts now validate both new plugin names independently in CMake and FILTER_NODES
observability_surfaces:
  - bash scripts/verify-s01-syntax.sh — full syntax + contract verification gate
  - grep -c 'DeepCDefocusPOThin' src/CMakeLists.txt — expect 2 (PLUGINS + FILTER_NODES)
  - grep -c 'DeepCDefocusPORay' src/CMakeLists.txt — expect 2
duration: 10m
verification_result: passed
completed_at: 2026-03-24
blocker_discovered: false
---

# T03: Update CMake, verify script, and remove old DeepCDefocusPO.cpp

**Wire both new plugins into CMake PLUGINS and FILTER_NODES, update verify script for new file set with cleaned-up contracts, and delete superseded DeepCDefocusPO.cpp**

## What Happened

Replaced `DeepCDefocusPO` with `DeepCDefocusPOThin` and `DeepCDefocusPORay` in both the `PLUGINS` and `FILTER_NODES` lists in `src/CMakeLists.txt`. Updated `scripts/verify-s01-syntax.sh` to syntax-check the two new `.cpp` files instead of the old one. Removed all S02/S03/S04 grep contract blocks that referenced the now-deleted `DeepCDefocusPO.cpp` — those contracts will be re-added by their respective slices targeting the new files. Rewrote S05 contracts to validate both new plugin names in CMake and verify the old file is gone. Deleted `src/DeepCDefocusPO.cpp`.

## Verification

Ran `bash scripts/verify-s01-syntax.sh` — all four syntax checks passed (DeepCBlur, DeepCDepthBlur, DeepCDefocusPOThin, DeepCDefocusPORay) and S05 contracts passed. Ran the full slice verification battery (13 checks): CMake counts, max_degree presence, sphereToCs, stub zeros, error paths, old file deletion — all pass.

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `bash scripts/verify-s01-syntax.sh` | 0 | ✅ pass | ~3s |
| 2 | `test "$(grep -c 'DeepCDefocusPOThin' src/CMakeLists.txt)" -eq 2` | 0 | ✅ pass | <1s |
| 3 | `test "$(grep -c 'DeepCDefocusPORay' src/CMakeLists.txt)" -eq 2` | 0 | ✅ pass | <1s |
| 4 | `test "$(grep -c 'DeepCDefocusPO[^TR]' src/CMakeLists.txt)" -eq 0` | 0 | ✅ pass | <1s |
| 5 | `grep -q 'max_degree' src/DeepCDefocusPOThin.cpp` | 0 | ✅ pass | <1s |
| 6 | `grep -q 'max_degree' src/DeepCDefocusPORay.cpp` | 0 | ✅ pass | <1s |
| 7 | `grep -q 'max_degree' src/poly.h` | 0 | ✅ pass | <1s |
| 8 | `grep -q 'sphereToCs' src/deepc_po_math.h` | 0 | ✅ pass | <1s |
| 9 | `grep -q 'writableAt.*= 0' src/DeepCDefocusPOThin.cpp` | 0 | ✅ pass | <1s |
| 10 | `grep -q 'writableAt.*= 0' src/DeepCDefocusPORay.cpp` | 0 | ✅ pass | <1s |
| 11 | `grep -q 'error.*Cannot open lens file' src/DeepCDefocusPOThin.cpp` | 0 | ✅ pass | <1s |
| 12 | `grep -q 'error.*Cannot open lens file' src/DeepCDefocusPORay.cpp` | 0 | ✅ pass | <1s |
| 13 | `grep -q 'error.*Cannot open aperture file' src/DeepCDefocusPORay.cpp` | 0 | ✅ pass | <1s |
| 14 | `test ! -f src/DeepCDefocusPO.cpp` | 0 | ✅ pass | <1s |

## Diagnostics

- `bash scripts/verify-s01-syntax.sh` — full syntax + contract gate, exits 0 when all plugins compile and contracts hold
- `grep -c 'DeepCDefocusPOThin' src/CMakeLists.txt` — expect 2 (PLUGINS list + FILTER_NODES)
- `grep -c 'DeepCDefocusPORay' src/CMakeLists.txt` — expect 2
- `grep 'DeepCDefocusPO[^TR]' src/CMakeLists.txt` — should return nothing (old plugin fully removed)

## Deviations

- First CMake edit attempt failed silently because there were two occurrences of `DeepCDefocusPO` followed by `)` — the FILTER_NODES line matched first (inline format). The PLUGINS list entry (indented, separate line) was caught on second pass. No impact on final result.

## Known Issues

- S02/S03/S04 grep contract blocks were removed entirely. Future slices must add their own contracts targeting the new Thin/Ray files.

## Files Created/Modified

- `src/CMakeLists.txt` — replaced DeepCDefocusPO with DeepCDefocusPOThin + DeepCDefocusPORay in PLUGINS and FILTER_NODES
- `scripts/verify-s01-syntax.sh` — updated for-loop to compile new files, removed stale S02-S04 contracts, rewrote S05 contracts for new plugin names
- `src/DeepCDefocusPO.cpp` — deleted (superseded by Thin and Ray variants)
