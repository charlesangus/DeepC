---
id: T02
parent: S02
milestone: M007-gvtoom
provides:
  - test/test_thin.nk Nuke test script for DeepCDefocusPOThin at 128×72
key_files:
  - test/test_thin.nk
key_decisions: []
patterns_established:
  - Nuke test scripts adapted from reference by swapping node class, adding new knobs, updating output path
observability_surfaces:
  - Runtime proof via `nuke -x test/test_thin.nk` producing non-black test_thin.exr (deferred to docker build)
duration: 5m
verification_result: passed
completed_at: 2026-03-24
blocker_discovered: false
---

# T02: Write test/test_thin.nk test script

**Created test/test_thin.nk Nuke script that renders DeepCDefocusPOThin at 128×72 using Angenieux 55mm lens with max_degree 11 and outputs test_thin.exr.**

## What Happened

Adapted the reference test script `test/test_deepcdefocus_po.nk` to create `test/test_thin.nk`. Changes from reference: node class `DeepCDefocusPO` → `DeepCDefocusPOThin`, added `max_degree 11` knob, Root format set to `128 72`, output file → `./test_thin.exr`, Root name updated. Kept the entire grid generation chain (Reformat1 → Grid1 → Dilate → Merge → DeepFromImage at depth 5000) identical. Lens config preserved: `poly_file` pointing to Angenieux 55mm exitpupil.fit, `focal_length 55`, `focus_distance 10000`. Stripped verbose `node_layout` user knobs for cleanliness.

## Verification

All 7 task-level and all 13 slice-level verification checks pass. This is the final task of S02 — full slice verification is green.

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `test -f test/test_thin.nk` | 0 | ✅ pass | <1s |
| 2 | `grep -q 'DeepCDefocusPOThin' test/test_thin.nk` | 0 | ✅ pass | <1s |
| 3 | `grep -q 'exitpupil.fit' test/test_thin.nk` | 0 | ✅ pass | <1s |
| 4 | `grep -q 'test_thin.exr' test/test_thin.nk` | 0 | ✅ pass | <1s |
| 5 | `grep -q 'focal_length 55' test/test_thin.nk` | 0 | ✅ pass | <1s |
| 6 | `grep -q 'focus_distance 10000' test/test_thin.nk` | 0 | ✅ pass | <1s |
| 7 | `grep -q 'max_degree 11' test/test_thin.nk` | 0 | ✅ pass | <1s |
| 8 | `bash scripts/verify-s01-syntax.sh` | 0 | ✅ pass | 3s |
| 9 | `grep -q 'coc_radius' src/DeepCDefocusPOThin.cpp` | 0 | ✅ pass | <1s |
| 10 | `grep -q 'halton' src/DeepCDefocusPOThin.cpp` | 0 | ✅ pass | <1s |
| 11 | `grep -q 'map_to_disk' src/DeepCDefocusPOThin.cpp` | 0 | ✅ pass | <1s |
| 12 | `grep -q 'chanNo' src/DeepCDefocusPOThin.cpp` | 0 | ✅ pass | <1s |
| 13 | `grep -q 'transmittance_at' src/DeepCDefocusPOThin.cpp` | 0 | ✅ pass | <1s |
| 14 | `grep -q '0.45f' src/DeepCDefocusPOThin.cpp` | 0 | ✅ pass | <1s |
| 15 | `grep -q '_max_degree' src/DeepCDefocusPOThin.cpp` | 0 | ✅ pass | <1s |
| 16 | `grep -q 'chans.contains' src/DeepCDefocusPOThin.cpp` | 0 | ✅ pass | <1s |
| 17 | `grep -q 'error.*Cannot open lens file' src/DeepCDefocusPOThin.cpp` | 0 | ✅ pass | <1s |

## Diagnostics

- **Runtime proof:** `nuke -x test/test_thin.nk` is the UAT gate for S02. It requires Nuke + the compiled plugin, so it's deferred to docker build. The script is structurally complete and references all correct knobs/paths.
- **Failure modes in test script:** If the poly_file path is wrong, Nuke will show `error("Cannot open lens file: ...")`. If the node class isn't loaded, Nuke will report an unknown node type. Black output means the scatter engine early-returned (check `_poly_loaded`, `_aperture_samples`).

## Deviations

- Stripped verbose `node_layout_tab`/`node_layout_state` user knobs from all nodes — these are Nuke UI layout metadata irrelevant to rendering. The reference had them on every node; the test script is cleaner without them and functionally identical.

## Files Created/Modified

- `test/test_thin.nk` — New Nuke test script for DeepCDefocusPOThin
