---
id: T01
parent: S05
milestone: M006
provides:
  - stale-S01/S02-comments-removed
  - lentil-poly.h-license-entry
  - DeepCDefocusPO-icon
  - S05-verify-contracts
key_files:
  - src/DeepCDefocusPO.cpp
  - THIRD_PARTY_LICENSES.md
  - scripts/verify-s01-syntax.sh
  - icons/DeepCDefocusPO.png
key_decisions:
  - none
patterns_established:
  - none
observability_surfaces:
  - "bash scripts/verify-s01-syntax.sh — exits 0 with 'S05 contracts: all pass.' when all structural checks hold; each contract prints a distinct FAIL message and exits 1 on failure"
duration: ~10m
verification_result: passed
completed_at: 2026-03-23
blocker_discovered: false
---

# T01: Fix stale comments, add license entry, create icon, extend verify script

**Replaced stale S01/S02 comment block in DeepCDefocusPO.cpp, appended lentil/poly.h MIT license entry to THIRD_PARTY_LICENSES.md, copied DeepCBlur.png as icons/DeepCDefocusPO.png, and extended verify-s01-syntax.sh with S05 contracts — all spot checks and the full verify script pass.**

## What Happened

Four independent mechanical changes closed the remaining loose ends from S01–S04:

1. **Stale comments removed** — Lines 13–14 of `src/DeepCDefocusPO.cpp` contained `//  S01 state: skeleton only — renderStripe outputs flat black (zeros).` and `//  S02 replaces the renderStripe body with the full PO scatter loop.`. These were replaced with an accurate two-line comment: `//  Full implementation: PO scatter engine (S02), depth-aware holdout (S03), / and focal-length knob (S04) are all complete.` The Knobs comment block was also extended to document `focal_length_mm`.

2. **lentil/poly.h license** — A new `## lentil / poly.h` section was appended to `THIRD_PARTY_LICENSES.md`, including project metadata (author: Johannes Hanika/hanatos, source: https://github.com/hanatos/lentil) and the full MIT license text.

3. **Icon created** — `icons/DeepCBlur.png` was copied to `icons/DeepCDefocusPO.png`. The CMake root-level glob `file(GLOB ICONS "icons/DeepC*.png")` picks this up automatically at build time — no CMakeLists.txt change required.

4. **S05 verify contracts appended** — A `# --- S05 contracts ---` block was inserted in `scripts/verify-s01-syntax.sh` immediately before the final `echo "All syntax checks passed."` line. The block checks: (a) stale S01/S02 comments absent, (b) DeepCDefocusPO appears in exactly 2 CMakeLists.txt locations, (c) FILTER_NODES entry present, (d) lentil/hanatos in THIRD_PARTY_LICENSES.md, (e) Op::Description registration present.

The pre-flight observability gaps flagged for S05-PLAN.md and T01-PLAN.md were also addressed: an `## Observability / Diagnostics` section was added to S05-PLAN.md and an `## Observability Impact` section to T01-PLAN.md.

## Verification

All verification commands ran clean:

- `bash scripts/verify-s01-syntax.sh` — exited 0; output included "S05 contracts: all pass." and "All syntax checks passed."
- `! grep -q 'S01 state: skeleton only' src/DeepCDefocusPO.cpp` — exited 0 (comment gone)
- `grep -q 'lentil\|hanatos' THIRD_PARTY_LICENSES.md` — exited 0
- `test -f icons/DeepCDefocusPO.png` — exited 0
- `test -s icons/DeepCDefocusPO.png` — exited 0 (file non-empty, same size as DeepCBlur.png)
- All slice spot checks (CMake count=2, FILTER_NODES, Op::Description, no stale comment, icon exists) — all PASS

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `bash scripts/verify-s01-syntax.sh` | 0 | ✅ pass | ~3s |
| 2 | `! grep -q 'S01 state: skeleton only' src/DeepCDefocusPO.cpp` | 0 | ✅ pass | <1s |
| 3 | `grep -q 'lentil\|hanatos' THIRD_PARTY_LICENSES.md` | 0 | ✅ pass | <1s |
| 4 | `test -f icons/DeepCDefocusPO.png` | 0 | ✅ pass | <1s |
| 5 | `test -s icons/DeepCDefocusPO.png` | 0 | ✅ pass | <1s |
| 6 | `grep -c 'DeepCDefocusPO' src/CMakeLists.txt \| grep -q '^2$'` | 0 | ✅ pass | <1s |
| 7 | `grep -q 'FILTER_NODES.*DeepCDefocusPO' src/CMakeLists.txt` | 0 | ✅ pass | <1s |
| 8 | `grep -q 'Op::Description' src/DeepCDefocusPO.cpp` | 0 | ✅ pass | <1s |

## Diagnostics

Inspection commands for future agents:

- `bash scripts/verify-s01-syntax.sh` — primary gate; each S05 contract emits a distinct FAIL message and exits 1 on failure
- `grep -q 'S01 state: skeleton only' src/DeepCDefocusPO.cpp` — exits 0 means stale comment still present (failure); exits 1 means it is gone (success)
- `file icons/DeepCDefocusPO.png` — confirms PNG MIME type if icon integrity is in doubt
- `grep -A5 'lentil' THIRD_PARTY_LICENSES.md` — shows the vendored attribution section

## Deviations

None — all four changes exactly matched the T01-PLAN specification.

## Known Issues

None — all slice verification gates pass. Remaining milestone blockers are CI-only: `docker-build.sh --linux --versions 16.0` (requires Docker daemon, not available in workspace) and Nuke runtime visual check (requires Nuke license/runtime).

## Files Created/Modified

- `src/DeepCDefocusPO.cpp` — replaced stale S01/S02 comment lines 13–14 with accurate implementation-state comment; added `focal_length_mm` entry to Knobs comment block
- `THIRD_PARTY_LICENSES.md` — appended `## lentil / poly.h` section with full MIT license attribution for Johannes Hanika's polynomial-optics code
- `scripts/verify-s01-syntax.sh` — appended S05 contract block before final success echo; script now exits 0 end-to-end
- `icons/DeepCDefocusPO.png` — new file (copy of `icons/DeepCBlur.png`); auto-picked up by CMake install glob
- `.gsd/milestones/M006/slices/S05/S05-PLAN.md` — added `## Observability / Diagnostics` section (pre-flight gap)
- `.gsd/milestones/M006/slices/S05/tasks/T01-PLAN.md` — added `## Observability Impact` section (pre-flight gap)
