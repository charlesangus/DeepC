# S04 Research: Knobs, UI, and Lens Parameter Polish

**Slice:** S04 — Knobs, UI, and Lens Parameter Polish  
**Milestone:** M006 — DeepCDefocusPO  
**Risk:** Low  
**Depends:** S01  
**Researched:** 2026-03-23

---

## Summary

S04 is light-to-targeted work. The S01 skeleton already wired all four primary knobs
(`poly_file`, `focus_distance`, `fstop`, `aperture_samples`) with tooltips, default values,
and `SetRange`. The one substantive gap is **D029**: `focal_length_mm` is hardcoded to `50.0f`
in `renderStripe` and must become a `Float_knob` member so artists can match the CoC cull to
the actual lens. Beyond that, S04 adds UI decoration (Divider, optional grouping) and ensures
the `node_help` string is accurate now that S02/S03 have replaced the "flat black" scaffolding
language. The verify script needs S04 grep contracts added and the mock `Knobs.h` stub needs
one new function (`Divider`/`BeginGroup`/`EndGroup` are already present).

**Current syntax state:** `bash scripts/verify-s01-syntax.sh` exits 0; all S01, S02, and S03
contracts pass.

---

## What Already Exists (Do Not Redo)

From S01 (`src/DeepCDefocusPO.cpp`):

```cpp
// Member vars — already present:
const char* _poly_file;        // File_knob
float       _focus_distance;   // Float_knob, default 200.0f mm
float       _fstop;            // Float_knob, default 2.8f
int         _aperture_samples; // Int_knob, default 64

// Constructor initialisers — already present:
, _focus_distance(200.0f)
, _fstop(2.8f)
, _aperture_samples(64)

// knobs() — already present:
File_knob(f, &_poly_file, "poly_file", "lens file");
Tooltip(f, "Path to the lentil polynomial lens file (.fit binary). ...");

Float_knob(f, &_focus_distance, "focus_distance", "focus distance");
SetRange(f, 1.0f, 100000.0f);
Tooltip(f, "Distance from the lens to the focus plane in mm. ...");

Float_knob(f, &_fstop, "fstop", "f-stop");
SetRange(f, 0.5f, 64.0f);
Tooltip(f, "Lens aperture (f-stop). ...");

Int_knob(f, &_aperture_samples, "aperture_samples", "aperture samples");
SetRange(f, 1, 4096);
Tooltip(f, "Number of aperture sample positions traced per deep sample. ...");
```

---

## The One Substantive Change: `focal_length` Knob (D029)

Decision D029 deferred a `Float_knob focal_length` to S04. Currently in `renderStripe`:

```cpp
const float focal_length_mm = 50.0f;  // line 357 — must become a member
```

This value feeds:
1. `coc_radius(focal_length_mm, _fstop, _focus_distance, depth)` — CoC-based early cull
2. The pixel-space CoC conversion: `coc_px = coc * half_w / (focal_length_mm * 0.5f + 1e-6f)`

**S04 must:**
1. Add `float _focal_length_mm;` member (default `50.0f`)
2. Initialise it in the constructor: `, _focal_length_mm(50.0f)`
3. Add `Float_knob(f, &_focal_length_mm, "focal_length", "focal length (mm)");`
   - `SetRange(f, 1.0f, 1000.0f)` (covers fisheye 8mm through 1000mm telephoto)
   - Tooltip: `"Lens focal length in mm. Used for circle-of-confusion culling only — the polynomial already encodes the correct bokeh shape. Typical values: 24 (wide), 50 (normal), 85 (portrait), 135 (tele)."`
4. Replace the local `const float focal_length_mm = 50.0f;` with `const float focal_length_mm = _focal_length_mm;`

**No other renderStripe changes needed.** The `coc_radius` call signature and pixel-space
formula already reference the local `focal_length_mm` variable — replacing the local
constant with the member value is the entire implementation.

---

## Recommendation: Divider for Visual Grouping

The existing four knobs fall into two logical groups:

| Group | Knobs |
|-------|-------|
| Lens setup | `poly_file`, `focal_length` |
| Render settings | `focus_distance`, `fstop`, `aperture_samples` |

Recommended UI layout (matches DeepC conventions — see `DeepCConstant.cpp` line 81,
`DeepCWorld.cpp` line 256):

```cpp
void knobs(Knob_Callback f) override
{
    // --- Lens setup ---
    File_knob(f, &_poly_file, "poly_file", "lens file");
    Tooltip(f, "...");

    Float_knob(f, &_focal_length_mm, "focal_length", "focal length (mm)");
    SetRange(f, 1.0f, 1000.0f);
    Tooltip(f, "...");

    Divider(f, "");

    // --- Render controls ---
    Float_knob(f, &_focus_distance, "focus_distance", "focus distance (mm)");
    SetRange(f, 1.0f, 100000.0f);
    Tooltip(f, "...");

    Float_knob(f, &_fstop, "fstop", "f-stop");
    SetRange(f, 0.5f, 64.0f);
    Tooltip(f, "...");

    Int_knob(f, &_aperture_samples, "aperture_samples", "aperture samples");
    SetRange(f, 1, 4096);
    Tooltip(f, "...");
}
```

`Divider` is already stubbed in `scripts/verify-s01-syntax.sh`'s `Knobs.h` mock
(`inline void EndGroup(Knob_Callback) {}`), but `Divider` itself is not present yet
in the mock. **The mock needs `inline void Divider(Knob_Callback, const char*) {}`**
added to `Knobs.h`. Check: `grep -q 'Divider' $TMPDIR/DDImage/Knobs.h`. Without this
stub the syntax check will fail when `Divider(f, "")` is called.

---

## `node_help` Update

The current `HELP` string (lines 52–63 of `DeepCDefocusPO.cpp`) still contains:

```
"S01 skeleton: outputs flat black. Scatter math added in S02.\n\n"
```

S02 and S03 are complete. That sentence must be removed. The rest of the HELP string
is accurate and should be kept.

Replacement last paragraph (remove the S01/S02 reference entirely; the help string
already documents all the other parameters accurately).

---

## Label Wording Audit

Existing label strings are clean. One minor inconsistency to fix: the `focus_distance`
knob label is `"focus distance"` (no units). Since all values are in mm and the focus
distance range is `1–100000 mm`, the label should read `"focus distance (mm)"` to
match the `focal_length` knob label `"focal length (mm)"`. This is cosmetic only —
the knob name (`focus_distance`) is the persistent identifier in Nuke scripts.

---

## Verify Script: S04 Contracts to Add

After S04 edits, add these checks to `scripts/verify-s01-syntax.sh` after the S03 block:

```bash
# --- S04 grep contracts ---
echo "Checking S04 contracts..."
grep -q '_focal_length_mm'       "$SRC_DIR/DeepCDefocusPO.cpp" || { echo "FAIL: _focal_length_mm member missing"; exit 1; }
grep -q 'focal_length'           "$SRC_DIR/DeepCDefocusPO.cpp" || { echo "FAIL: focal_length knob missing"; exit 1; }
# Confirm hardcoded 50.0f is gone from renderStripe (replaced by member)
! grep -q 'const float focal_length_mm = 50' "$SRC_DIR/DeepCDefocusPO.cpp" || { echo "FAIL: hardcoded 50mm still in renderStripe"; exit 1; }
# Confirm S01 stale HELP language is gone
! grep -q 'S01 skeleton' "$SRC_DIR/DeepCDefocusPO.cpp" || { echo "FAIL: stale S01 skeleton comment in HELP string"; exit 1; }
echo "S04 contracts: all pass."
```

The `! grep -q 'const float focal_length_mm = 50'` check verifies the hardcoded value
is gone; the member assignment `const float focal_length_mm = _focal_length_mm;` will
NOT match this pattern so the negative check is safe.

---

## Implementation Landscape

### Files to change

| File | Change |
|------|--------|
| `src/DeepCDefocusPO.cpp` | Add `_focal_length_mm` member + ctor init; add `Float_knob` in `knobs()`; add `Divider`; replace local constant in `renderStripe`; remove stale HELP line; update `focus_distance` label to include `(mm)` |
| `scripts/verify-s01-syntax.sh` | Add `Divider` stub to mock `Knobs.h`; add S04 grep contracts at end |

### Files NOT to change

- `src/deepc_po_math.h` — `coc_radius` signature is unchanged; no edit needed
- `src/poly.h` — no change
- `src/CMakeLists.txt` — already has `DeepCDefocusPO` in `PLUGINS` and `FILTER_NODES`

### Order of work

1. Add `_focal_length_mm` member and constructor init
2. Add `Float_knob` for `focal_length` in `knobs()` with `Divider` above render controls
3. Replace hardcoded `50.0f` local with `_focal_length_mm` in `renderStripe`
4. Update HELP string (remove stale S01 line)
5. Update `focus_distance` label to `"focus distance (mm)"`
6. Extend `verify-s01-syntax.sh`: add `Divider` mock stub; add S04 contracts block
7. Run `bash scripts/verify-s01-syntax.sh` — must exit 0 with all contracts passing

### Verification commands

```bash
# Full verify (syntax + all contracts)
bash scripts/verify-s01-syntax.sh

# Spot checks
grep -c '_focal_length_mm' src/DeepCDefocusPO.cpp   # expect ≥ 3
grep -q 'focal_length'     src/DeepCDefocusPO.cpp   # knob present
grep -q 'Divider'          src/DeepCDefocusPO.cpp   # divider present
! grep -q 'S01 skeleton'   src/DeepCDefocusPO.cpp   # stale HELP gone
! grep -q 'const float focal_length_mm = 50' src/DeepCDefocusPO.cpp  # hardcoded gone
```

---

## Requirements Coverage

- **R021** (CoC formula uses focal length from knob): S04 closes the deferred TODO in D029 by
  wiring `_focal_length_mm` as a knob. After S04, the CoC cull uses the artist-specified focal
  length rather than a hardcoded 50mm. Note: R021 states the CoC should use "focal length from
  the .fit file metadata" — the .fit format has no standardised metadata field for this (D029
  rationale), so the knob is the correct substitute. R021 validation is runtime-only (requires
  Nuke + real .fit file), confirmed at S05.

---

## No Novel Risks

S04 is a straightforward wiring task using patterns already established in the codebase. There
are no new APIs, no new dependencies, and no architectural decisions to make. The only failure
mode is forgetting the `Divider` stub in the mock (which would break the syntax check) or
leaving the hardcoded `50.0f` in renderStripe (which the S04 negative grep contract catches).
