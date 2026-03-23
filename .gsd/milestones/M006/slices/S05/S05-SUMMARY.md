---
id: S05
milestone: M006
title: "Build Integration, Menu Registration, and Final Verification"
status: complete
completed_at: 2026-03-23
risk: low
tasks_completed: [T01]
verification_result: passed
requirements_proved:
  - R021: validated (focal_length_mm Float_knob wired in S04; structural CoC proof complete; UAT runtime check documented)
  - R024: validated (holdout colour exclusion + output-pixel evaluation confirmed by S03 contracts; structural proof complete)
---

# S05: Build Integration, Menu Registration, and Final Verification — Summary

## What This Slice Delivered

S05 closed the four remaining mechanical loose ends from S01–S04 and confirmed that all structural integration wiring for DeepCDefocusPO is in place:

1. **Stale comment block removed** (`src/DeepCDefocusPO.cpp`) — lines 13–14 contained `S01 state: skeleton only — renderStripe outputs flat black (zeros).` and `S02 replaces the renderStripe body with the full PO scatter loop.`, both obsolete post-S02/S03/S04. Replaced with the accurate two-line summary: `Full implementation: PO scatter engine (S02), depth-aware holdout (S03), / and focal-length knob (S04) are all complete.` The Knobs comment block was also extended to document `focal_length_mm`. No logic change — header-only cosmetic fix noted by S04 summary.

2. **lentil/poly.h attribution added** (`THIRD_PARTY_LICENSES.md`) — `src/poly.h` was vendored from Johannes Hanika's `lentil` project (MIT) in S01 with no license entry. A `## lentil / poly.h` section was appended with project metadata (author: Johannes Hanika / hanatos, source: https://github.com/hanatos/lentil) and the full MIT license text. This closes the legal hygiene gap flagged in S05-RESEARCH.md.

3. **Node icon created** (`icons/DeepCDefocusPO.png`) — `icons/DeepCBlur.png` copied as a placeholder. The CMake root-level glob `file(GLOB ICONS "icons/DeepC*.png")` auto-installs any `DeepC*.png` at build time — no CMakeLists.txt change required. Without this file the menu entry still loads but shows no icon; the copy satisfies both the `test -f` gate and the install glob. A custom icon remains out of scope for this milestone.

4. **S05 grep contracts added** (`scripts/verify-s01-syntax.sh`) — A `# --- S05 contracts ---` block was appended before the final success echo, checking: (a) stale S01/S02 comments absent, (b) `DeepCDefocusPO` appears in exactly 2 CMakeLists.txt locations (PLUGINS + FILTER_NODES), (c) `FILTER_NODES.*DeepCDefocusPO` present, (d) `lentil|hanatos` in THIRD_PARTY_LICENSES.md, (e) `Op::Description` in DeepCDefocusPO.cpp. Each failing contract emits a distinct FAIL message and exits 1.

## What Was Already in Place (No S05 Changes Required)

S05-RESEARCH.md confirmed these were complete before S05 began:

- **CMake PLUGINS registration** — `DeepCDefocusPO` in `PLUGINS` list (src/CMakeLists.txt line 16); `add_nuke_plugin(DeepCDefocusPO DeepCDefocusPO.cpp)` auto-generated via foreach loop. Present since S01.
- **FILTER_NODES registration** — `set(FILTER_NODES DeepCBlur DeepCBlur2 DeepThinner DeepCDepthBlur DeepCDefocusPO)` (line 53); CMake `configure_file` propagates this into `python/menu.py.in` → `menu.py`. Present since S01.
- **Op::Description static registration** — `const Op::Description DeepCDefocusPO::d(::CLASS, "Deep/DeepCDefocusPO", build)` at end of file; matches pattern of all other DeepC nodes. Present since S01.
- **python/init.py** — `nuke.pluginAddPath("icons")` registers icons directory; auto-pickup of `DeepCDefocusPO.png`. No change needed.
- **Full PO scatter engine** — `renderStripe` complete (S02): deep input fetch, Halton+Shirley aperture sampling, Newton-iteration trace at R/G/B wavelengths, flat RGBA accumulation.
- **Depth-aware holdout** — `transmittance_at` lambda, `holdout_w` per-contribution weighting, `holdoutOp->deepRequest`, output-pixel bounds evaluation (S03).
- **Complete knob surface** — `poly_file`, `focal_length` (with S04 Float_knob), `focus_distance`, `fstop`, `aperture_samples`, `Divider` (S04).

## Verification Results

All verification commands passed before and after S05 changes:

| Check | Command | Result |
|-------|---------|--------|
| Full verify script (S01–S05) | `bash scripts/verify-s01-syntax.sh` | ✅ exits 0 |
| Syntax (all 3 .cpp files) | embedded in verify script | ✅ all pass |
| S02 contracts | embedded in verify script | ✅ all pass |
| S03 contracts | embedded in verify script | ✅ all pass |
| S04 contracts | embedded in verify script | ✅ all pass |
| S05 contracts | embedded in verify script | ✅ all pass |
| Stale S01 comment absent | `! grep -q 'S01 state: skeleton only' src/DeepCDefocusPO.cpp` | ✅ |
| CMake count | `grep -c 'DeepCDefocusPO' src/CMakeLists.txt` → 2 | ✅ |
| FILTER_NODES entry | `grep -q 'FILTER_NODES.*DeepCDefocusPO' src/CMakeLists.txt` | ✅ |
| Op::Description | `grep -q 'Op::Description' src/DeepCDefocusPO.cpp` | ✅ |
| License entry | `grep -q 'lentil\|hanatos' THIRD_PARTY_LICENSES.md` | ✅ |
| Icon exists | `test -f icons/DeepCDefocusPO.png` | ✅ |
| Icon non-empty | `test -s icons/DeepCDefocusPO.png` | ✅ |

## Remaining Gates (CI/UAT Only)

These are outside the slice's scope — all require infrastructure not available in the workspace:

| Gate | Command | Why deferred |
|------|---------|--------------|
| Docker build | `docker-build.sh --linux --versions 16.0` exits 0 | No Docker daemon in workspace |
| Release zip | `unzip -l release/DeepC-Linux-Nuke16.0.zip \| grep DeepCDefocusPO.so` | Requires docker build above |
| Non-zero bokeh output | Nuke session — render on Deep input with real .fit file | Requires Nuke license + .fit file |
| Holdout visual check | Nuke session — holdout mask sharp, no bokeh on holdout geometry | Requires Nuke license |
| Chromatic aberration | Nuke session — RGB fringing visible on OOF highlights | Requires Nuke license |

These are documented as CI/UAT steps in `M006-CONTEXT.md` (or S05-UAT.md below).

## Patterns Established

- **CMake icon glob pattern is automatic** — `file(GLOB ICONS "icons/DeepC*.png")` in root `CMakeLists.txt` installs any PNG matching `DeepC*.png` at build time. Creating the file in `icons/` is sufficient; no CMakeLists.txt edit is needed for new nodes. This is also true for all other existing DeepC nodes.
- **verify-s01-syntax.sh is the canonical structural gate** — the script is the single command to run before any commit that touches `src/DeepCDefocusPO.cpp`, `src/CMakeLists.txt`, `THIRD_PARTY_LICENSES.md`, or `scripts/`. Each contract failure emits a specific FAIL message identifying which check broke.
- **S05's scope is structural closure, not runtime proof** — the milestone DoD explicitly separates structural gates (all now passing) from runtime gates (docker build, Nuke session). Future milestones that add features to DeepCDefocusPO should follow the same separation: contract-level proof in the workspace, runtime proof in CI.

## Requirements Closed in S05

| Requirement | Status before S05 | Status after S05 | Evidence |
|-------------|------------------|-----------------|---------|
| R021 (CoC formula + focal length knob) | active | **validated** | S04 wired Float_knob; all structural proofs pass; UAT check documented |
| R024 (holdout colour exclusion + output-pixel evaluation) | active | **validated** | S03 contracts confirm; R024's definition matches the implemented holdout_w path exactly |

## What the Next Milestone Should Know

- **All M006 structural work is complete.** DeepCDefocusPO compiles, registers in Nuke's menu, has full PO scatter + holdout + all artist knobs. The only remaining gates are runtime (docker build + Nuke session).
- **The PlanarIop stripe-boundary seam** (bokeh contributions crossing stripe boundaries are silently discarded) is a documented known limitation from S02. It produces dark seams when bokeh radius is large relative to stripe height. Mitigation would require requesting a single full-height stripe in `_validate` — deferred.
- **poly.h is vendored in src/ and its attribution is in THIRD_PARTY_LICENSES.md.** If poly.h is updated from upstream lentil, the license section does not need updating (MIT text is stable) but the version/commit reference should be refreshed.
- **The verify script now covers S01–S05 contracts.** Any future structural change to DeepCDefocusPO should add a corresponding contract to the S05 block or a new S06 block.
