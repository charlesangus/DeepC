# S02: Alpha correction + UI polish — UAT

**Milestone:** M003
**Written:** 2026-03-21

## UAT Type

- UAT mode: artifact-driven (grep + build) + human-experience (visual Nuke comparison)
- Why this mode is sufficient: The knob wiring and alpha correction algorithm are machine-verifiable via grep and docker build. The visual correctness of alpha correction (blurred deep images appear brighter when compositing) requires a human with a Nuke session and a deep EXR.

## Preconditions

1. DeepC repo checked out with M003 changes applied
2. `docker-build.sh` available and Docker running (for build test)
3. For visual tests: Nuke 16.0 or later, a deep EXR with semi-transparent samples (e.g. a rendered deep smoke or volume)
4. `scripts/verify-s01-syntax.sh` available (no Docker needed for syntax test)

## Smoke Test

```
grep -c "WH_knob" src/DeepCBlur.cpp
```
Expected output: `1`
If this returns anything other than `1`, the WH_knob refactor did not land.

---

## Test Cases

### 1. WH_knob replaces separate Float_knobs

```bash
grep -c "WH_knob" src/DeepCBlur.cpp
grep "Float_knob.*blur" src/DeepCBlur.cpp
```

1. Run first command.
2. **Expected:** output is `1`
3. Run second command.
4. **Expected:** no output (exit code 1)

Both conditions must hold. The old `Float_knob` calls for blur width/height are gone; a single `WH_knob` call is present.

---

### 2. Sample optimization twirldown

```bash
grep -c "BeginClosedGroup" src/DeepCBlur.cpp
```

1. Run command.
2. **Expected:** output is `1`

Optional manual check in Nuke:
1. Load DeepCBlur.so into Nuke and create a `DeepCBlur` node.
2. Open the node's properties panel.
3. **Expected:** "Sample Optimization" is visible as a closed twirldown group. Opening it reveals `max_samples`, `merge_tolerance`, and `color_tolerance`. These knobs are NOT visible at the top level.

---

### 3. Alpha correction knob present and wired

```bash
grep -c "Bool_knob.*alpha_correction" src/DeepCBlur.cpp
grep -q "cumTransp" src/DeepCBlur.cpp && echo "found"
```

1. Run first command.
2. **Expected:** output is `1`
3. Run second command.
4. **Expected:** output is `found`

These two checks together confirm the knob is declared and the correction logic (cumTransp tracking) is present.

---

### 4. Syntax check passes

```bash
bash scripts/verify-s01-syntax.sh
echo "Exit: $?"
```

1. Run command.
2. **Expected:** output contains `Syntax check passed.` and exit code is `0`

If this fails, check that mock headers in the script include stubs for `WH_knob`, `Bool_knob`, `BeginClosedGroup`, `EndGroup`, and `SetRange(Knob_Callback, double, double)`.

---

### 5. Docker build produces DeepCBlur.so

```bash
bash docker-build.sh --linux --versions 16.0
echo "Exit: $?"
```

1. Run command (takes ~40s).
2. **Expected:** exit code `0`, and `DeepCBlur.so` is present in the release archive.

This is the definitive proof that the refactored code compiles against real Nuke 16.0 SDK headers.

---

### 6. Visual: Alpha correction brightens composited deep output (human-verified)

*Requires Nuke session with DeepCBlur.so loaded and a deep EXR with semi-transparent samples.*

1. Import a deep EXR (smoke, volume, or multi-layer deep render) into Nuke.
2. Connect it to a `DeepCBlur` node. Set blur size to ~10 in both dimensions.
3. Connect the DeepCBlur output to a `DeepToImage` node, then view the result.
4. In the DeepCBlur properties panel, confirm `alpha correction` checkbox is **unchecked** (default).
5. Note the brightness of the composited image.
6. Check the `alpha correction` checkbox.
7. **Expected:** The composited image becomes visibly brighter. Areas with semi-transparent samples show the most change. Fully opaque areas show minimal or no change.
8. Uncheck the box again.
9. **Expected:** Image returns to the uncorrected (darker) appearance.

This is the core visual proof of R003.

---

## Edge Cases

### Zero blur with alpha correction enabled

1. Set blur size to `0.0 × 0.0` in the WH_knob.
2. Enable `alpha correction`.
3. **Expected:** Node passes through input unchanged. The zero-blur fast path fires before any blur or correction occurs. Output is identical to input.

### WH_knob lock toggle

1. In Nuke, open DeepCBlur properties.
2. Set width to `20`, then click the lock icon on the WH_knob.
3. **Expected:** Height snaps to `20` (or vice versa, depending on which was changed). Changing either value while locked changes both simultaneously.

### Fully opaque sample (correction guard)

*If you have a deep render where some pixels contain fully opaque samples near the front:*

1. Enable `alpha correction` and apply blur.
2. **Expected:** Samples behind fully opaque layers are not corrected (division guard at `cumTransp < 1e-6`). No divide-by-zero NaN/inf values appear in the output. The `DeepToImage` result shows no bright speckles or NaN pixels.

---

## Failure Signals

- `grep -c "WH_knob"` returns `0` or `>1` → WH_knob not present or duplicated
- `grep "Float_knob.*blur"` returns output → old float knobs were not removed
- `verify-s01-syntax.sh` exits non-zero with `error: use of undeclared identifier 'WH_knob'` (or similar) → mock header stubs missing for new DDImage types
- `docker-build.sh` exits non-zero → compilation failure against real Nuke headers; check compiler output for type mismatch on `_blurSize` casts
- Alpha correction enabled but output appears identical to uncorrected → correction pass not executing (check `_alphaCorrection` is read in `doDeepEngine`); or input has no semi-transparent samples
- NaN/inf values in Nuke viewer with correction enabled → `cumTransp < 1e-6` guard not active; check the correction loop guard condition

---

## Not Proven By This UAT

- Visual parity between separable and non-separable blur at small radii (R001 — deferred from S01, requires human visual comparison)
- Performance improvement at radius 20 (R001 — requires timing benchmarks in Nuke)
- Kernel tier visual distinctiveness (R002 — requires human comparison of LQ/MQ/HQ outputs on a test image)
- Correction accuracy for all sample configurations (the cumTransp algorithm is correct for standard front-to-back over-compositing, but extreme cases with interleaved alpha may differ from CMG99 reference)

---

## Notes for Tester

- The alpha correction effect is most visible on deep renders of smoke, fire, or volumetric effects where samples have partial alpha. A single-layer deep surface (alpha 0 or 1 only) will show minimal or no visible difference.
- The `alpha correction` knob defaults to **off** to preserve backward compatibility with M002 outputs. Always check the default state before comparing.
- Use a `DeepToImage` node (not just `DeepMerge`) for the visual comparison — `DeepToImage` performs the compositing that makes the correction effect visible.
- If `verify-s01-syntax.sh` starts failing after adding new DDImage types to `DeepCBlur.cpp`, check the mock headers inside the script first — they are hand-maintained and must be updated for each new type.
