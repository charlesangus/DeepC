# S02: Build integration and release — UAT

**Milestone:** M002
**Written:** 2026-03-20

## UAT Type

- UAT mode: artifact-driven
- Why this mode is sufficient: This slice produces build artifacts (icon, README entry, release archive) — all verifiable without running Nuke. Runtime verification of DeepCBlur functionality is deferred to milestone-level UAT.

## Preconditions

- Docker is available and `nukedockerbuild:16.0-linux` image is built
- Working directory is the DeepC repository root
- S01 has been completed (DeepCBlur.cpp and DeepSampleOptimizer.h exist)

## Smoke Test

```bash
test -f icons/DeepCBlur.png && grep -q 'DeepCBlur' README.md && grep -q 'DeepCBlur' src/CMakeLists.txt && echo "SMOKE OK"
```

## Test Cases

### 1. Icon file validity

1. Run `file icons/DeepCBlur.png`
2. **Expected:** Output contains "PNG image data, 24 x 24, 8-bit/color RGBA, non-interlaced"

### 2. README plugin count is exactly 24

1. Run `grep -c '^\- !\[\]' README.md`
2. **Expected:** Output is `24`

### 3. DeepCBlur README entry is in correct alphabetical position

1. Run `grep -n '^\- !\[\]' README.md | head -5`
2. **Expected:** DeepCBlur appears after DeepCAdjustBBox and before DeepCClamp

### 4. CMakeLists.txt PLUGINS list includes DeepCBlur

1. Run `grep 'DeepCBlur' src/CMakeLists.txt`
2. **Expected:** DeepCBlur appears in both the PLUGINS list and FILTER_NODES variable

### 5. Menu template will generate correct Filter entry

1. Run `grep '@FILTER_NODES@' python/menu.py.in`
2. **Expected:** The FILTER_NODES placeholder exists in the Filter submenu section of menu.py.in

### 6. Docker Linux build produces release archive with DeepCBlur

1. Run `docker-build.sh --linux --versions 16.0`
2. Run `unzip -l release/DeepC-Linux-Nuke16.0.zip | grep DeepCBlur`
3. **Expected:** Two matches — `DeepC/DeepCBlur.so` and `DeepC/icons/DeepCBlur.png`

### 7. Generated menu.py places DeepCBlur in Filter submenu

1. Run `grep 'DeepCBlur' install/16.0-linux/DeepC/menu.py`
2. **Expected:** DeepCBlur appears in the Filter submenu section alongside DeepThinner

## Edge Cases

### Duplicate plugin entry in README

1. Run `grep -c 'DeepCBlur' README.md`
2. **Expected:** Exactly 1 match (no duplicates)

### Icon dimensions wrong

1. Run `python3 -c "from PIL import Image; img=Image.open('icons/DeepCBlur.png'); assert img.size == (24,24) and img.mode == 'RGBA', f'BAD: {img.size} {img.mode}'; print('OK')"`
2. **Expected:** Output is `OK`

### Plugin count regression

1. Add a new fake entry to README, run `grep -c '^\- !\[\]' README.md`
2. **Expected:** Returns 25, not 24 — demonstrates the count check catches drift
3. Revert the fake entry

## Failure Signals

- `file icons/DeepCBlur.png` does not contain "PNG image data" — icon is corrupt or wrong format
- `grep -c '^\- !\[\]' README.md` returns anything other than 24 — missing or duplicate entry
- `docker-build.sh` exits non-zero — compilation or packaging failure; check Docker stdout for which target failed
- `unzip -l release/*.zip | grep DeepCBlur` returns no matches — plugin not included in release

## Not Proven By This UAT

- DeepCBlur actually loads in Nuke and produces correct visual output (requires Nuke runtime — milestone-level UAT)
- Windows cross-compilation (requires Windows Docker image)
- Blur correctness, sample propagation, or optimization behavior (S01 scope, runtime-dependent)

## Notes for Tester

- The icon is a solid cornflower blue placeholder — this is intentional for now. Visual quality is not the goal; discoverability in the toolbar is.
- Docker build test (case 6) takes ~40 seconds with cached images, longer on first run.
- If Docker is unavailable, cases 6 and 7 can be skipped — the file-level checks (cases 1–5) provide sufficient integration proof.
