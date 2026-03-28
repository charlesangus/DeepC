---
id: T04
parent: S01
milestone: M009-mso5fb
provides: []
requires: []
affects: []
key_files: ["scripts/verify-s01-syntax.sh", "THIRD_PARTY_LICENSES.md"]
key_decisions: ["Added OPENDEFOCUS_INCLUDE as an explicit script variable pointing at crates/opendefocus-deep/include so the opendefocus_deep.h include path is canonical in the verify script", "Inserted EUPL-1.2 full text verbatim from official EU source for complete license compliance"]
patterns_established: []
drill_down_paths: []
observability_surfaces: []
duration: ""
verification_result: "bash scripts/verify-s01-syntax.sh exited 0 with all three DeepC*.cpp files passing; grep -q 'opendefocus' and grep -q 'EUPL-1.2' both confirmed presence in THIRD_PARTY_LICENSES.md."
completed_at: 2026-03-26T13:21:15.073Z
blocker_discovered: false
---

# T04: Added OPENDEFOCUS_INCLUDE -I flag to verify-s01-syntax.sh and appended full EUPL-1.2 license text attributing opendefocus (Gilles Vink) to THIRD_PARTY_LICENSES.md

> Added OPENDEFOCUS_INCLUDE -I flag to verify-s01-syntax.sh and appended full EUPL-1.2 license text attributing opendefocus (Gilles Vink) to THIRD_PARTY_LICENSES.md

## What Happened
---
id: T04
parent: S01
milestone: M009-mso5fb
key_files:
  - scripts/verify-s01-syntax.sh
  - THIRD_PARTY_LICENSES.md
key_decisions:
  - Added OPENDEFOCUS_INCLUDE as an explicit script variable pointing at crates/opendefocus-deep/include so the opendefocus_deep.h include path is canonical in the verify script
  - Inserted EUPL-1.2 full text verbatim from official EU source for complete license compliance
duration: ""
verification_result: passed
completed_at: 2026-03-26T13:21:15.079Z
blocker_discovered: false
---

# T04: Added OPENDEFOCUS_INCLUDE -I flag to verify-s01-syntax.sh and appended full EUPL-1.2 license text attributing opendefocus (Gilles Vink) to THIRD_PARTY_LICENSES.md

**Added OPENDEFOCUS_INCLUDE -I flag to verify-s01-syntax.sh and appended full EUPL-1.2 license text attributing opendefocus (Gilles Vink) to THIRD_PARTY_LICENSES.md**

## What Happened

T03 had already wired mock headers and the DeepCOpenDefocus.cpp loop entry, so the script was passing. This task added OPENDEFOCUS_INCLUDE as an explicit script variable pointing at crates/opendefocus-deep/include and wired it into the g++ -I flag, making the FFI header path explicit rather than relying solely on the relative include in the .cpp. THIRD_PARTY_LICENSES.md received a new opendefocus section with project metadata and the complete EUPL-1.2 English text fetched verbatim from the official EU source.

## Verification

bash scripts/verify-s01-syntax.sh exited 0 with all three DeepC*.cpp files passing; grep -q 'opendefocus' and grep -q 'EUPL-1.2' both confirmed presence in THIRD_PARTY_LICENSES.md.

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `bash scripts/verify-s01-syntax.sh && grep -q 'opendefocus' THIRD_PARTY_LICENSES.md && grep -q 'EUPL-1.2' THIRD_PARTY_LICENSES.md && echo 'All checks passed'` | 0 | ✅ pass | 591ms |


## Deviations

None. T03 had already done the mock-header and loop extension work; this task added the -I flag and license attribution as planned.

## Known Issues

None.

## Files Created/Modified

- `scripts/verify-s01-syntax.sh`
- `THIRD_PARTY_LICENSES.md`


## Deviations
None. T03 had already done the mock-header and loop extension work; this task added the -I flag and license attribution as planned.

## Known Issues
None.
