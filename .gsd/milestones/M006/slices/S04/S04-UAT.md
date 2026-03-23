---
id: S04-UAT
milestone: M006
slice: S04
title: "Knobs, UI, and Lens Parameter Polish — UAT Script"
prepared_for: S05 integration verification / manual Nuke session
---

# S04 UAT: Knobs, UI, and Lens Parameter Polish

## Preconditions

1. Workspace is `/workspace/.gsd/worktrees/M006`.
2. `g++` with C++17 support is available (`g++ --version` should succeed).
3. No Docker access required for these tests — all checks run via `bash` and `grep` on the source tree.
4. For TC-05 (visual UI check), a Nuke 16 session with `DeepCDefocusPO.so` loaded is needed; this is a deferred gate for S05.

---

## TC-01: Verify script exits 0 with all S01–S04 contracts passing

**Purpose:** Confirm the full structural gate passes before any downstream work depends on this slice.

**Steps:**
```bash
cd /workspace/.gsd/worktrees/M006
bash scripts/verify-s01-syntax.sh
echo "Exit: $?"
```

**Expected outcome:**
- Output contains:
  - `Syntax check passed: DeepCBlur.cpp`
  - `Syntax check passed: DeepCDepthBlur.cpp`
  - `Syntax check passed: DeepCDefocusPO.cpp`
  - `S02 contracts: all pass.`
  - `S03 contracts: all pass.`
  - `S04 contracts: all pass.`
  - `All syntax checks passed.`
- Script exits with code `0`.
- **No** `FAIL:` lines appear anywhere in the output.

**Edge case:** If the script exits non-zero, the failing `FAIL:` line identifies which contract failed. Fix the named issue and re-run before proceeding.

---

## TC-02: `_focal_length_mm` wired in all four required locations

**Purpose:** Confirm D029 is fully closed — member declared, constructor-initialised, exposed as a knob, and read in renderStripe.

**Steps:**
```bash
cd /workspace/.gsd/worktrees/M006

# Must return >= 3 (actual: 4)
grep -c '_focal_length_mm' src/DeepCDefocusPO.cpp

# Each individual occurrence
grep -n '_focal_length_mm' src/DeepCDefocusPO.cpp
```

**Expected outcome:**
- `grep -c` returns `4` (or at minimum `3`).
- `grep -n` shows four distinct lines matching:
  1. Member declaration in class body (`float _focal_length_mm;`)
  2. Constructor initialiser (`, _focal_length_mm(50.0f)`)
  3. `Float_knob` call in `knobs()` (`Float_knob(f, &_focal_length_mm, ...)`)
  4. Local constant in `renderStripe` (`const float focal_length_mm = _focal_length_mm;`)

---

## TC-03: No hardcoded 50mm literal remains in renderStripe

**Purpose:** Ensure the focal-length constant in the scatter loop reads from the knob, not from a compile-time literal.

**Steps:**
```bash
cd /workspace/.gsd/worktrees/M006

# Should produce NO output (no match)
grep 'const float focal_length_mm = 50' src/DeepCDefocusPO.cpp
echo "Exit (expect non-zero = no match): $?"

# Confirm the correct form is present
grep 'focal_length_mm = _focal_length_mm' src/DeepCDefocusPO.cpp
```

**Expected outcome:**
- First `grep` returns exit code `1` (no match — hardcoded literal absent).
- Second `grep` matches exactly one line in `renderStripe`.

---

## TC-04: Divider and knob layout order

**Purpose:** Confirm the visual separator is present between knob groups and `focus_distance` label includes units.

**Steps:**
```bash
cd /workspace/.gsd/worktrees/M006

grep -q 'Divider' src/DeepCDefocusPO.cpp && echo "PASS: Divider present" || echo "FAIL"
grep -q 'focus distance (mm)' src/DeepCDefocusPO.cpp && echo "PASS: label correct" || echo "FAIL"
grep -q 'focal length (mm)' src/DeepCDefocusPO.cpp && echo "PASS: focal_length label correct" || echo "FAIL"

# Confirm knob order: poly_file BEFORE Divider BEFORE focus_distance
python3 - << 'EOF'
import re
src = open("src/DeepCDefocusPO.cpp").read()
pos_poly = src.find('poly_file')
pos_focal = src.find('focal_length')
pos_div = src.find('Divider')
pos_focus = src.find('focus_distance')
pos_fstop = src.find('"fstop"')
pos_ap = src.find('aperture_samples')
assert pos_poly < pos_focal < pos_div < pos_focus < pos_fstop < pos_ap, \
    f"Knob order wrong: poly={pos_poly} focal={pos_focal} div={pos_div} focus={pos_focus} fstop={pos_fstop} ap={pos_ap}"
print("PASS: knob order correct (poly_file → focal_length → Divider → focus_distance → fstop → aperture_samples)")
EOF
```

**Expected outcome:**
- All three `grep` commands print `PASS`.
- Python assertion passes and prints `PASS: knob order correct ...`.

---

## TC-05: Stale S01 scaffolding text absent

**Purpose:** Confirm the HELP string no longer contains development-phase placeholder text that would be visible to artists in Nuke's node tooltip.

**Steps:**
```bash
cd /workspace/.gsd/worktrees/M006

# Should produce NO output
grep 'S01 skeleton' src/DeepCDefocusPO.cpp
echo "Exit (expect 1 = no match): $?"

# Confirm HELP string is still non-empty and contains real content
grep -c 'Polynomial-optics defocus' src/DeepCDefocusPO.cpp
```

**Expected outcome:**
- First `grep` finds no match (exit code `1`).
- Second `grep` returns at least `1` — the HELP string retains the correct production description.

---

## TC-06: Default values are sensible for a 50mm f/2.8 at 2m

**Purpose:** Ensure the constructor defaults match the slice plan specification — a "normal" portrait lens at typical subject distance.

**Steps:**
```bash
cd /workspace/.gsd/worktrees/M006

python3 - << 'EOF'
src = open("src/DeepCDefocusPO.cpp").read()

import re
# focal_length_mm default
m = re.search(r'_focal_length_mm\s*\(\s*([\d.]+f?)\s*\)', src)
assert m and float(m.group(1).rstrip('f')) == 50.0, f"focal_length_mm default wrong: {m}"

# fstop default
m = re.search(r'_fstop\s*\(\s*([\d.]+f?)\s*\)', src)
assert m and float(m.group(1).rstrip('f')) == 2.8, f"fstop default wrong: {m}"

# focus_distance default
m = re.search(r'_focus_distance\s*\(\s*([\d.]+f?)\s*\)', src)
assert m and float(m.group(1).rstrip('f')) == 200.0, f"focus_distance default wrong: {m}"

# aperture_samples default
m = re.search(r'_aperture_samples\s*\(\s*(\d+)\s*\)', src)
assert m and int(m.group(1)) == 64, f"aperture_samples default wrong: {m}"

print("PASS: all constructor defaults correct (focal=50mm, fstop=2.8, focus=200mm, samples=64)")
EOF
```

**Expected outcome:**
- Script prints `PASS: all constructor defaults correct ...`.
- No assertion errors.

---

## TC-07: Verify script Knobs.h heredoc has `Divider` stub

**Purpose:** Confirm the syntax check itself won't regress if `Divider` is used in future knob additions.

**Steps:**
```bash
cd /workspace/.gsd/worktrees/M006
grep -q 'inline void Divider' scripts/verify-s01-syntax.sh && echo "PASS: Divider stub present" || echo "FAIL"
```

**Expected outcome:**
- `PASS: Divider stub present`

---

## TC-08: (Deferred — Nuke session) Knob panel matches specification

**Purpose:** Visual confirmation in Nuke that the knob panel renders correctly with the Divider separating knob groups.

**Precondition:** `DeepCDefocusPO.so` built (S05) and loaded in Nuke 16.

**Steps:**
1. Create a `DeepCDefocusPO` node in Nuke.
2. Open the Properties panel.
3. Observe the knob layout.

**Expected outcome:**
- Top group (no section label): **lens file** (File_knob), **focal length (mm)** (Float_knob, default 50).
- Horizontal Divider line.
- Bottom group: **focus distance (mm)** (Float_knob, default 200), **f-stop** (Float_knob, default 2.8), **aperture samples** (Int_knob, default 64).
- All six knobs have non-empty tooltips visible on hover.
- Node tooltip (`?` button) shows the production HELP string with no "S01 skeleton" text.

**Note:** This test cannot be completed until S05 delivers a docker build. Mark as pending S05.

---

## TC-09: (Deferred — Nuke session) Changing focal length knob produces expected CoC culling change

**Purpose:** Confirm the `_focal_length_mm` member is correctly read in `renderStripe` — not dead code.

**Precondition:** `DeepCDefocusPO.so` loaded; a Deep test input with samples at multiple depths connected.

**Steps:**
1. Set `focal_length` to `24` (wide angle).
2. Render and observe bokeh strip at large CoC distances.
3. Change `focal_length` to `135` (telephoto).
4. Render same frame.

**Expected outcome:**
- The node re-evaluates on knob change (Nuke invalidates correctly).
- CoC culling threshold changes between renders (bokeh pattern may differ due to changed culling aggressiveness; this is a performance effect, not a visual accuracy test).
- No crash or error in the Script Editor.

**Note:** The focal length does not affect bokeh *shape* (the polynomial handles that); it affects only the early-cull threshold. The visual difference may be subtle. This test primarily confirms no crash and knob invalidation works.

---

## Pass Criteria Summary

| TC | Description | Gate | Status |
|----|-------------|------|--------|
| TC-01 | verify-s01-syntax.sh exits 0 | structural | ✅ must pass |
| TC-02 | `_focal_length_mm` wired ≥ 3 times | structural | ✅ must pass |
| TC-03 | No hardcoded 50mm literal | structural | ✅ must pass |
| TC-04 | Divider + label units + knob order | structural | ✅ must pass |
| TC-05 | No stale S01 HELP text | structural | ✅ must pass |
| TC-06 | Constructor defaults correct | structural | ✅ must pass |
| TC-07 | Verify script has Divider stub | structural | ✅ must pass |
| TC-08 | Nuke knob panel visual check | operational | ⏳ deferred to S05 |
| TC-09 | focal_length knob re-evaluates node | operational | ⏳ deferred to S05 |

**S04 slice acceptance:** TC-01 through TC-07 must all pass. TC-08 and TC-09 are deferred to S05 Nuke runtime session.
