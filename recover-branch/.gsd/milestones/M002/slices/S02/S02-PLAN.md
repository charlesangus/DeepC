# S02: Build integration and release

**Goal:** DeepCBlur appears in DeepC > Filter toolbar menu. docker-build.sh produces Linux and Windows release archives containing DeepCBlur. README updated to 24 plugins.
**Demo:** `grep 'DeepCBlur' README.md` shows the plugin entry; `ls icons/DeepCBlur.png` shows icon exists; docker-build.sh (when run) includes DeepCBlur in release archives.

## Must-Haves

- `icons/DeepCBlur.png` exists as a 24×24 RGBA PNG icon
- `README.md` lists DeepCBlur as the 24th plugin, inserted alphabetically between DeepCClamp and DeepCColorLookup
- docker-build.sh integration verified — DeepCBlur will be built and included in release archives (no script changes needed; PLUGINS list already includes it)

## Proof Level

- This slice proves: integration
- Real runtime required: no (Docker build is the integration proof; Nuke runtime is UAT)
- Human/UAT required: no

## Verification

- `test -f icons/DeepCBlur.png && file icons/DeepCBlur.png | grep -q "PNG image data"` — icon exists and is valid PNG
- `grep -q 'DeepCBlur' README.md` — README mentions DeepCBlur
- `grep -c '^\- !\[\]' README.md` returns 24 — correct plugin count
- `grep -q 'DeepCBlur' src/CMakeLists.txt` — build system includes DeepCBlur (already done by S01, verify unchanged)
- `grep -q 'FILTER_NODES.*DeepCBlur' src/CMakeLists.txt` — menu integration present

## Tasks

- [x] **T01: Create DeepCBlur icon and update README to 24 plugins** `est:15m`
  - Why: DeepCBlur needs a toolbar icon and README documentation entry to be discoverable and to update the advertised plugin count from 23 to 24.
  - Files: `icons/DeepCBlur.png`, `README.md`
  - Do: Create a 24×24 RGBA PNG icon (copy an existing icon as placeholder and modify, or generate a minimal one). Add DeepCBlur entry to README.md between DeepCClamp and DeepCColorLookup following the existing `- ![](url) [Name](wiki-link)` pattern.
  - Verify: `test -f icons/DeepCBlur.png && file icons/DeepCBlur.png | grep -q PNG && grep -c '^\- !\[\]' README.md | grep -q 24`
  - Done when: Icon file exists as valid PNG, README has exactly 24 plugin entries including DeepCBlur in alphabetical order.

- [x] **T02: Verify docker-build.sh and CMake integration for DeepCBlur** `est:15m`
  - Why: R006 and R007 require DeepCBlur to build and appear in release archives. The build system was wired by S01 but integration must be verified end-to-end — confirm PLUGINS list, FILTER_NODES, icon glob, and docker-build.sh script will produce correct output.
  - Files: `src/CMakeLists.txt`, `docker-build.sh`, `python/menu.py.in`
  - Do: Verify CMakeLists.txt has DeepCBlur in PLUGINS and FILTER_NODES. Verify the icon glob pattern picks up DeepCBlur.png. Verify menu.py.in's @FILTER_NODES@ placeholder will generate the correct toolbar entry. If Docker is available, run `docker-build.sh --linux --versions 16.0` and confirm DeepCBlur.so appears in the release archive. If Docker is unavailable, document the verification steps for CI and confirm all file-level integration is correct.
  - Verify: `grep -q 'DeepCBlur' src/CMakeLists.txt && grep -q 'FILTER_NODES.*DeepCBlur' src/CMakeLists.txt`
  - Done when: All CMake/menu/icon integration points verified. Docker build attempted or deferred with documented verification commands.

## Observability / Diagnostics

- **Icon integrity:** `file icons/DeepCBlur.png` — confirms valid PNG with dimensions and color type
- **Plugin count drift:** `grep -c '^\- !\[\]' README.md` — should return 24; any other value indicates a missing or duplicate entry
- **Build integration:** `grep 'DeepCBlur' src/CMakeLists.txt` — confirms plugin is in PLUGINS and FILTER_NODES lists
- **Failure visibility:** If docker-build.sh fails, build logs in the Docker container stdout will show which plugin failed to compile; `ls release/*.zip` confirms archive creation
- **Diagnostic failure check:** `file icons/DeepCBlur.png | grep -q 'PNG image data, 24 x 24' || echo 'ICON FORMAT ERROR: expected 24x24 PNG'` — catches icon dimension/format corruption

## Files Likely Touched

- `icons/DeepCBlur.png`
- `README.md`
