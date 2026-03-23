---
estimated_steps: 7
estimated_files: 2
skills_used: []
---

# T01: Add focal_length knob, Divider, and clean up HELP; extend verify script

**Slice:** S04 — Knobs, UI, and Lens Parameter Polish
**Milestone:** M006

## Description

Seven surgical edits to two files: wire `_focal_length_mm` as a Float_knob (closing D029),
insert a `Divider` between knob groups, fix the `focus_distance` label, remove stale HELP
scaffolding text, add the `Divider` mock stub to the verify script, and append S04 grep
contracts. All changes are interdependent — the `Divider(f, "")` call in `knobs()` will fail
the syntax check until the mock stub is also present, so both files are edited atomically in
this one task.

### Context: what already exists

`src/DeepCDefocusPO.cpp` (509 lines) currently has:
- Members: `_focus_distance`, `_fstop`, `_aperture_samples` (no `_focal_length_mm`)
- Constructor initialises those three; `_focal_length_mm` is missing
- `knobs()` has four knobs in order: `poly_file`, `focus_distance`, `fstop`, `aperture_samples`; no `Divider`
- `renderStripe` line ~357: `const float focal_length_mm = 50.0f;` (hardcoded)
- HELP string line ~65: `"S01 skeleton: outputs flat black. Scatter math added in S02.\n\n"`
- `focus_distance` knob label: `"focus distance"` (no units)

`scripts/verify-s01-syntax.sh` mock `Knobs.h` section has `BeginClosedGroup`/`EndGroup` stubs
but no `Divider` stub. The S03 contracts block ends at ~line 404; S04 contracts are not present.

## Steps

1. **Add `_focal_length_mm` member** to the class member block in `DeepCDefocusPO.cpp`.
   Insert `float _focal_length_mm;` immediately after `int _aperture_samples;` (before the blank
   line that separates data knob members from the bool flags).

2. **Initialise in constructor.** In the constructor initialiser list, add
   `, _focal_length_mm(50.0f)` after `, _aperture_samples(64)`.

3. **Restructure `knobs()`.** Replace the existing four-knob body with this layout:
   ```cpp
   File_knob(f, &_poly_file, "poly_file", "lens file");
   Tooltip(f, "Path to the lentil polynomial lens file (.fit binary). "
               "Change triggers an automatic reload.");

   Float_knob(f, &_focal_length_mm, "focal_length", "focal length (mm)");
   SetRange(f, 1.0f, 1000.0f);
   Tooltip(f, "Lens focal length in mm. Used for circle-of-confusion culling only — "
               "the polynomial already encodes the correct bokeh shape. "
               "Typical values: 24 (wide), 50 (normal), 85 (portrait), 135 (tele).");

   Divider(f, "");

   Float_knob(f, &_focus_distance, "focus_distance", "focus distance (mm)");
   SetRange(f, 1.0f, 100000.0f);
   Tooltip(f, "Distance from the lens to the focus plane in mm. "
               "Samples at this depth produce a sharp point on the sensor.");

   Float_knob(f, &_fstop, "fstop", "f-stop");
   SetRange(f, 0.5f, 64.0f);
   Tooltip(f, "Lens aperture (f-stop). Lower values produce larger bokeh.");

   Int_knob(f, &_aperture_samples, "aperture_samples", "aperture samples");
   SetRange(f, 1, 4096);
   Tooltip(f, "Number of aperture sample positions traced per deep sample. "
               "Higher values reduce Monte Carlo noise at the cost of render time.");
   ```
   Note: `"focus distance"` → `"focus distance (mm)"` is the label change for R021 unit clarity.

4. **Replace the hardcoded constant in `renderStripe`.** Find:
   ```cpp
   const float focal_length_mm = 50.0f;
   ```
   Replace with:
   ```cpp
   const float focal_length_mm = _focal_length_mm;
   ```
   The two downstream uses (`coc_radius(focal_length_mm, ...)` and the pixel-space coc_px formula)
   already reference the local variable and require no change.

5. **Remove the stale HELP line.** In the `HELP` string, remove exactly this line:
   ```cpp
       "S01 skeleton: outputs flat black. Scatter math added in S02.\n\n"
   ```
   The preceding and following lines of the HELP string remain intact.

6. **Add `Divider` mock stub to `verify-s01-syntax.sh`.** In the `Knobs.h` heredoc section
   (near line 220 where `BeginClosedGroup` and `EndGroup` stubs are), add:
   ```cpp
   inline void Divider(Knob_Callback, const char*) {}
   ```
   Add it on the line immediately after `inline void BeginClosedGroup(Knob_Callback, const char*) {}`.

7. **Append S04 grep contracts** to `verify-s01-syntax.sh` immediately after the line
   `echo "S03 contracts: all pass."` and before `echo "All syntax checks passed."`:
   ```bash
   # --- S04 grep contracts ---
   echo "Checking S04 contracts..."
   grep -q '_focal_length_mm'       "$SRC_DIR/DeepCDefocusPO.cpp" || { echo "FAIL: _focal_length_mm member missing"; exit 1; }
   grep -q 'focal_length'           "$SRC_DIR/DeepCDefocusPO.cpp" || { echo "FAIL: focal_length knob missing"; exit 1; }
   ! grep -q 'const float focal_length_mm = 50' "$SRC_DIR/DeepCDefocusPO.cpp" || { echo "FAIL: hardcoded 50mm still in renderStripe"; exit 1; }
   ! grep -q 'S01 skeleton' "$SRC_DIR/DeepCDefocusPO.cpp" || { echo "FAIL: stale S01 skeleton comment in HELP string"; exit 1; }
   grep -q 'Divider' "$SRC_DIR/DeepCDefocusPO.cpp" || { echo "FAIL: Divider missing from knobs()"; exit 1; }
   echo "S04 contracts: all pass."
   ```

## Must-Haves

- [ ] `float _focal_length_mm;` member declared in the class, default 50.0f in ctor init list
- [ ] `Float_knob(f, &_focal_length_mm, "focal_length", "focal length (mm)")` with `SetRange(f, 1.0f, 1000.0f)` and tooltip in `knobs()`
- [ ] `Divider(f, "")` present between the `focal_length` and `focus_distance` knobs
- [ ] `focus_distance` label is `"focus distance (mm)"` (not `"focus distance"`)
- [ ] `renderStripe` reads `const float focal_length_mm = _focal_length_mm;` (not hardcoded 50.0f)
- [ ] HELP string no longer contains "S01 skeleton" text
- [ ] `scripts/verify-s01-syntax.sh` mock `Knobs.h` contains `inline void Divider(Knob_Callback, const char*) {}`
- [ ] S04 contracts block present and all five checks pass when the script runs

## Verification

```bash
# Full pipeline — must exit 0
bash scripts/verify-s01-syntax.sh

# Spot checks
grep -c '_focal_length_mm' src/DeepCDefocusPO.cpp     # must be >= 3 (member decl + ctor init + renderStripe)
grep -q 'focal_length'     src/DeepCDefocusPO.cpp     # knob present
grep -q 'Divider'          src/DeepCDefocusPO.cpp     # divider present
grep -q 'focus distance (mm)' src/DeepCDefocusPO.cpp  # label updated
! grep -q 'const float focal_length_mm = 50' src/DeepCDefocusPO.cpp  # hardcoded gone
! grep -q 'S01 skeleton'   src/DeepCDefocusPO.cpp     # stale HELP gone
grep -q 'Divider'          scripts/verify-s01-syntax.sh  # mock stub present
```

## Inputs

- `src/DeepCDefocusPO.cpp` — existing PlanarIop implementation with hardcoded focal_length_mm and stale HELP
- `scripts/verify-s01-syntax.sh` — existing syntax + contract verifier; currently passes S01–S03 contracts

## Expected Output

- `src/DeepCDefocusPO.cpp` — updated with `_focal_length_mm` member, `Float_knob`, `Divider`, corrected label, and cleaned HELP
- `scripts/verify-s01-syntax.sh` — updated with `Divider` mock stub and S04 contracts block

## Observability Impact

This task makes no runtime behaviour changes (no new async paths, no new background processes). The observability changes are purely diagnostic/inspection:

- **`bash scripts/verify-s01-syntax.sh`** — the S04 contracts block added in step 7 surfaces failure state explicitly: each failing check prints `FAIL: <human-readable reason>` and exits 1 immediately. Passing state is confirmed by the `S04 contracts: all pass.` line.
- **`_focal_length_mm` knob** — the new knob value is readable at render time via `grep -c '_focal_length_mm' src/DeepCDefocusPO.cpp` (≥ 3). Within Nuke, the value is visible in the node's Properties panel as "focal length (mm)".
- **`Divider` stub in `Knobs.h`** — presence confirmed by `grep -q 'Divider' scripts/verify-s01-syntax.sh`.
- **Failure inspection** — if a future agent suspects the focal-length knob is not wired, they can run `grep -n 'focal_length_mm' src/DeepCDefocusPO.cpp` to see all four occurrences (member, ctor, knob, renderStripe).

No logs, background processes, or API calls are involved. No redaction concerns.
