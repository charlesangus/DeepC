# S04: Knobs, UI, and Lens Parameter Polish

**Goal:** Wire `focal_length_mm` as a `Float_knob` (closing D029), add a visual `Divider` between lens-setup and render-control groups, remove stale S01 scaffolding language from the HELP string, and update the `focus_distance` label to include units — so all artist controls are complete, correctly labelled, and match DeepC UI conventions.

**Demo:** `bash scripts/verify-s01-syntax.sh` exits 0 with S04 contracts passing; `grep -c '_focal_length_mm' src/DeepCDefocusPO.cpp` returns ≥ 3; no hardcoded `const float focal_length_mm = 50` remains in the source; no "S01 skeleton" text in HELP.

## Must-Haves

- `_focal_length_mm` float member added (default 50.0f), constructor-initialised, exposed as `Float_knob` with range 1–1000 and tooltip
- `renderStripe` local constant `focal_length_mm` reads from `_focal_length_mm` (not hardcoded 50.0f)
- `Divider(f, "")` present between `poly_file`/`focal_length` knobs and `focus_distance`/`fstop`/`aperture_samples` knobs
- `focus_distance` label updated to `"focus distance (mm)"`
- Stale `"S01 skeleton: outputs flat black. Scatter math added in S02.\n\n"` line removed from HELP string
- `scripts/verify-s01-syntax.sh` mock `Knobs.h` gains `inline void Divider(Knob_Callback, const char*) {}`
- S04 grep contracts block added to `verify-s01-syntax.sh`
- `bash scripts/verify-s01-syntax.sh` exits 0 with all contracts (S01–S04) passing

## Verification

```bash
bash scripts/verify-s01-syntax.sh
grep -c '_focal_length_mm' src/DeepCDefocusPO.cpp   # must be >= 3
! grep -q 'const float focal_length_mm = 50' src/DeepCDefocusPO.cpp
! grep -q 'S01 skeleton' src/DeepCDefocusPO.cpp
grep -q 'Divider' src/DeepCDefocusPO.cpp
grep -q 'focus distance (mm)' src/DeepCDefocusPO.cpp
```

## Tasks

- [x] **T01: Add focal_length knob, Divider, and clean up HELP; extend verify script** `est:30m`
  - Why: Closes D029 (focal_length_mm hardcoded to 50.0f in renderStripe), adds UI polish, and removes stale scaffolding language; all must-haves land in this single surgical edit pass
  - Files: `src/DeepCDefocusPO.cpp`, `scripts/verify-s01-syntax.sh`
  - Do: Follow the exact steps in T01-PLAN.md
  - Verify: `bash scripts/verify-s01-syntax.sh` exits 0; all spot-check greps pass
  - Done when: verify script exits 0 and all six spot-checks above pass

## Files Likely Touched

- `src/DeepCDefocusPO.cpp`
- `scripts/verify-s01-syntax.sh`

## Observability / Diagnostics

These changes are knob-configuration edits with no new runtime paths; there is no background process, async flow, or network call involved.

**Inspection surfaces:**
- `bash scripts/verify-s01-syntax.sh` — exits 0 / prints pass lines per contract group (S01–S04); any FAIL line indicates the failing check with a human-readable message and exits 1.
- `grep -c '_focal_length_mm' src/DeepCDefocusPO.cpp` — confirms member is wired (expected ≥ 3 occurrences).
- `grep -q 'Divider' src/DeepCDefocusPO.cpp` — confirms Divider present in knobs().
- `grep -q 'focus distance (mm)' src/DeepCDefocusPO.cpp` — confirms updated label.
- `! grep -q 'S01 skeleton' src/DeepCDefocusPO.cpp` — confirms stale HELP text removed.
- `! grep -q 'const float focal_length_mm = 50' src/DeepCDefocusPO.cpp` — confirms hardcoded constant removed.

**Failure visibility:** If any S04 contract fails, `verify-s01-syntax.sh` prints `FAIL: <description>` to stdout and exits 1, making CI failures immediately readable without log inspection.

**Redaction:** None of these signals contain sensitive data.
