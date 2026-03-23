---
id: T01
parent: S04
milestone: M006
provides:
  - _focal_length_mm Float_knob wired (D029 closed)
  - Divider between lens-setup and render-control knob groups
  - focus_distance label updated to include (mm) units
  - Stale S01 skeleton HELP text removed
  - Divider mock stub in verify-s01-syntax.sh Knobs.h heredoc
  - S04 grep contracts block in verify-s01-syntax.sh (all pass)
key_files:
  - src/DeepCDefocusPO.cpp
  - scripts/verify-s01-syntax.sh
key_decisions:
  - Comment block above renderStripe focal_length_mm updated to reflect knob-driven value (no longer describes hardcoded fallback assumption)
patterns_established:
  - Verify-script Knobs.h heredoc must gain a stub for every DDImage knob helper used in knobs() before the syntax check will pass
observability_surfaces:
  - bash scripts/verify-s01-syntax.sh — prints FAIL:<reason> + exits 1 per failing S04 contract; prints "S04 contracts: all pass." on success
  - grep -c '_focal_length_mm' src/DeepCDefocusPO.cpp — should return >= 3
duration: ~10m
verification_result: passed
completed_at: 2026-03-23
blocker_discovered: false
---

# T01: Add focal_length knob, Divider, and clean up HELP; extend verify script

**Wired `_focal_length_mm` as a Float_knob with range 1–1000 mm, added a Divider between knob groups, updated focus_distance label to include units, removed stale S01 skeleton HELP text, and extended verify-s01-syntax.sh with a Divider stub and S04 grep contracts — all checks pass.**

## What Happened

Seven surgical edits were applied across two files:

1. **Member declaration** — `float _focal_length_mm;` inserted after `int _aperture_samples;` in the class body.
2. **Constructor init** — `, _focal_length_mm(50.0f)` added after `, _aperture_samples(64)` in the initialiser list.
3. **`knobs()` restructured** — added `Float_knob(f, &_focal_length_mm, "focal_length", "focal length (mm)")` with `SetRange(f, 1.0f, 1000.0f)` and tooltip, followed by `Divider(f, "")`, then the existing `focus_distance` knob with its label updated to `"focus distance (mm)"`. All four original knobs are retained with their tooltips and ranges.
4. **`renderStripe` constant** — `const float focal_length_mm = 50.0f;` replaced with `const float focal_length_mm = _focal_length_mm;`. The stale comment describing a 50 mm hardcoded fallback was also updated to reflect that the value is now knob-driven.
5. **HELP cleanup** — `"S01 skeleton: outputs flat black. Scatter math added in S02.\n\n"` removed from the HELP string.
6. **Divider stub** — `inline void Divider(Knob_Callback, const char*) {}` added to the `Knobs.h` heredoc in `verify-s01-syntax.sh`, immediately after the `BeginClosedGroup` stub.
7. **S04 contracts block** — five grep-based checks appended to `verify-s01-syntax.sh` between `echo "S03 contracts: all pass."` and `echo "All syntax checks passed."`.

The `_focal_length_mm` identifier appears 4 times in the source (member decl, ctor init, knob call, renderStripe local variable), satisfying the ≥ 3 requirement.

## Verification

Ran `bash scripts/verify-s01-syntax.sh` — exits 0, all four contract groups (S01–S04) pass. Ran 7 individual spot checks — all pass.

## Verification Evidence

| # | Command | Exit Code | Verdict | Duration |
|---|---------|-----------|---------|----------|
| 1 | `bash scripts/verify-s01-syntax.sh` | 0 | ✅ pass | ~3s |
| 2 | `grep -c '_focal_length_mm' src/DeepCDefocusPO.cpp` | 0 (returns 4) | ✅ pass | <1s |
| 3 | `grep -q 'focal_length' src/DeepCDefocusPO.cpp` | 0 | ✅ pass | <1s |
| 4 | `grep -q 'Divider' src/DeepCDefocusPO.cpp` | 0 | ✅ pass | <1s |
| 5 | `grep -q 'focus distance (mm)' src/DeepCDefocusPO.cpp` | 0 | ✅ pass | <1s |
| 6 | `! grep -q 'const float focal_length_mm = 50' src/DeepCDefocusPO.cpp` | 0 | ✅ pass | <1s |
| 7 | `! grep -q 'S01 skeleton' src/DeepCDefocusPO.cpp` | 0 | ✅ pass | <1s |
| 8 | `grep -q 'Divider' scripts/verify-s01-syntax.sh` | 0 | ✅ pass | <1s |

## Diagnostics

- **Script failure visibility:** `bash scripts/verify-s01-syntax.sh` prints `FAIL: <human-readable description>` and exits 1 on any failing S04 contract. No log file needed.
- **Knob wiring check:** `grep -n 'focal_length_mm' src/DeepCDefocusPO.cpp` shows all 4 occurrences (member, ctor, Float_knob call, renderStripe local).
- **Mock stub check:** `grep 'Divider' scripts/verify-s01-syntax.sh` shows the inline stub in the Knobs.h heredoc.

## Deviations

The stale comment block above the `focal_length_mm` local variable in `renderStripe` (which described the original hardcoded 50 mm assumption) was also updated to be accurate. This is a minor improvement not explicitly in the plan but consistent with the goal of removing stale scaffolding text.

## Known Issues

None.

## Files Created/Modified

- `src/DeepCDefocusPO.cpp` — added `_focal_length_mm` member + ctor init; restructured `knobs()` with new `Float_knob`, `Divider`, and updated label; fixed `renderStripe` to use `_focal_length_mm`; removed stale HELP line
- `scripts/verify-s01-syntax.sh` — added `Divider` mock stub to `Knobs.h` heredoc; appended S04 grep contracts block
