---
id: S04
milestone: M006
title: "Knobs, UI, and Lens Parameter Polish"
status: complete
risk: low
depends: [S01]
completed_at: 2026-03-23
tasks_completed: [T01]
verification_result: passed
---

# S04: Knobs, UI, and Lens Parameter Polish — Summary

## What This Slice Delivered

S04 made a single focused pass over `src/DeepCDefocusPO.cpp` and `scripts/verify-s01-syntax.sh` to complete the artist-facing knob surface of the node. All changes are surgical edits — no new runtime code paths, no architectural changes.

### Changes to `src/DeepCDefocusPO.cpp`

1. **`_focal_length_mm` float member** — added to the class body (declaration), constructor initialiser list (default `50.0f`), `knobs()` (exposed as `Float_knob` with range 1–1000 mm and tooltip), and `renderStripe` (local constant reads from `_focal_length_mm` instead of a hardcoded `50.0f` literal). Closes D029.

2. **`Divider(f, "")` in `knobs()`** — inserted between the lens-setup group (`poly_file`, `focal_length`) and the render-control group (`focus_distance`, `fstop`, `aperture_samples`). Matches DeepC UI conventions for logical grouping.

3. **`focus_distance` label** — updated from `"focus distance"` to `"focus distance (mm)"` to match the inline unit convention used throughout DeepC (and to be consistent with `focal_length (mm)` just above it).

4. **Stale HELP text removed** — `"S01 skeleton: outputs flat black. Scatter math added in S02.\n\n"` deleted from the HELP string. The remaining HELP accurately describes the fully implemented node.

### Changes to `scripts/verify-s01-syntax.sh`

5. **`Divider` mock stub** — `inline void Divider(Knob_Callback, const char*) {}` added to the `Knobs.h` heredoc, after `BeginClosedGroup`. Required so the syntax check compiles `knobs()` without a "Divider undeclared" error.

6. **S04 grep contracts block** — five contracts appended before the final `echo "All syntax checks passed."`:
   - `_focal_length_mm` member exists in source
   - `focal_length` knob wired
   - no hardcoded `const float focal_length_mm = 50` remains
   - no `S01 skeleton` stale text remains
   - `Divider` present in `knobs()`

## Verification Results

```
bash scripts/verify-s01-syntax.sh   → exits 0; all S01–S04 contracts pass
grep -c '_focal_length_mm' src/DeepCDefocusPO.cpp  → 4 (≥ 3 required)
! grep -q 'const float focal_length_mm = 50' ...  → PASS
! grep -q 'S01 skeleton' ...                       → PASS
grep -q 'Divider' ...                              → PASS
grep -q 'focus distance (mm)' ...                  → PASS
grep -q 'focal_length_mm = _focal_length_mm' ...   → PASS
```

All seven spot-checks and the full verify script exit 0.

## Decisions Closed

- **D029** (focal_length_mm hardcoded to 50.0f in renderStripe) is now closed. The Float_knob exposes this to the artist; `50.0f` is retained only as the constructor default (sensible for a "normal" 50mm lens). The renderStripe local variable now reads `_focal_length_mm` directly.

No new architectural decisions were required in S04. All seven edits were mechanical consequence of D029 and the slice plan.

## Requirements State After S04

- **R021** (focal length from knob) — the S04 Float_knob means the focal-length wire from knob → CoC culling is now complete. R021's validation note called out S02's hardcoded 50mm as a temporary state; S04 closes that gap. Full runtime proof (bokeh sizes matching lentil/Arnold) is still deferred to S05.
- No other requirement status changes — S04 is a UI-only polish slice; the core correctness requirements (R019–R026) were addressed in S01–S03.

## What S05 Should Know

- **Knob surface is complete.** All six artist-facing knobs (`poly_file`, `focal_length`, [Divider], `focus_distance`, `fstop`, `aperture_samples`) are wired, have tooltips, and have sensible defaults for a 50mm f/2.8 lens at 2m focus.
- **Verify script covers S01–S04 contracts.** `bash scripts/verify-s01-syntax.sh` is the primary structural gate for CI; S05 should add its own contract group to the same script for docker-build/zip artefact contracts.
- **No stripe-boundary seam mitigation landed in S04.** This was flagged in S02 as a known limitation — deferred. S05 may choose to request a full-height single stripe to eliminate it, or document it as an accepted artefact.
- **The node comment block at the top of DeepCDefocusPO.cpp** (lines ~13–25) still refers to "S01 state: skeleton only" and "S02 replaces...". S05 should update these to reflect the fully implemented state.

## Patterns Established

- **Verify-script `Knobs.h` heredoc must be extended** whenever a new DDImage knob helper is used in `knobs()`. The syntax check is the first structural gate and will fail with "undeclared identifier" if the stub is missing. This is now documented in KNOWLEDGE.md and in the S02 pattern entry.
- **Unit-labelled knob names** (`"focus distance (mm)"`, `"focal length (mm)"`) are the standard for this node. Follow the same convention for any future knobs that have physical units.
