# S03: Depth-Aware Holdout

**Goal:** Wire the optional holdout Deep input so it stencils the defocused output depth-correctly, weighting each scatter contribution by the holdout's transmittance at the output pixel and the sample's depth — without ever scattering the holdout through the lens.

**Demo:** `bash scripts/verify-s01-syntax.sh` exits 0; all S03 grep contracts pass; `src/DeepCDefocusPO.cpp` contains `holdoutConnected`, `transmittance_at`, `holdout_w`, and `hzf >= Z`; `input(1)` referenced ≥ 3 times; no TODO/STUB markers remain.

## Must-Haves

- `getRequests` requests the holdout DeepOp for depth+alpha channels only (R024: no colour)
- `renderStripe` fetches holdout DeepPlane at output-pixel bounds (not input-pixel position)
- `transmittance_at` lambda computes `product(1 - alpha_i)` for holdout samples with `zFront_i < Z`
- `holdout_w` applied to every `writableAt +=` accumulation (both RGB and alpha splats)
- When holdout is not connected, `holdout_w = 1.0f` (identity — no masking)
- `scripts/verify-s01-syntax.sh` extended with S03 grep contracts; script exits 0

## Verification

```bash
bash scripts/verify-s01-syntax.sh
```

All of the following must pass:

```bash
grep -q 'holdoutConnected'  src/DeepCDefocusPO.cpp
grep -q 'transmittance_at'  src/DeepCDefocusPO.cpp
grep -q 'holdout_w'         src/DeepCDefocusPO.cpp
grep -q 'hzf >= Z'          src/DeepCDefocusPO.cpp
test "$(grep -c 'input(1)' src/DeepCDefocusPO.cpp)" -ge 3
grep -q 'holdout.*deepRequest\|holdoutOp->deepRequest' src/DeepCDefocusPO.cpp
! grep -q 'TODO\|STUB' src/DeepCDefocusPO.cpp
```

## Tasks

- [x] **T01: Implement depth-aware holdout weighting in DeepCDefocusPO** `est:45m`
  - Why: Closes R023 and R024 — the only remaining work in S03. All four edits are interdependent and must land together.
  - Files: `src/DeepCDefocusPO.cpp`, `scripts/verify-s01-syntax.sh`
  - Do: Four surgical edits in order: (1) extend `getRequests` to request holdout depth+alpha channels; (2) fetch holdout `DeepPlane` at `bounds` immediately after the primary `deepEngine` call; (3) add `transmittance_at` lambda before the scatter loop; (4) apply `holdout_w` in the RGB and alpha splat steps. Then add S03 grep contracts to the verify script.
  - Verify: `bash scripts/verify-s01-syntax.sh` exits 0
  - Done when: syntax check exits 0 and all S03 grep contracts pass

## Files Likely Touched

- `src/DeepCDefocusPO.cpp`
- `scripts/verify-s01-syntax.sh`
