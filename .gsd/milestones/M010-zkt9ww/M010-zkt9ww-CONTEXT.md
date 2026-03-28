# M010-zkt9ww: DeepCOpenDefocus Integration Tests

**Gathered:** 2026-03-28
**Status:** Ready for planning

## Project Description

Create a suite of Nuke `.nk` test scripts that exercise the full feature surface of DeepCOpenDefocus via `nuke -x` batch rendering. Each script writes EXR output to a gitignored `test/output/` directory for manual inspection.

## Why This Milestone

M009 built DeepCOpenDefocus and verified it compiles and links (docker build exit 0, nm symbol check). But the nuke -x runtime tests were deferred because nukedockerbuild doesn't include a Nuke license. The existing `test/test_opendefocus.nk` is minimal — single DeepConstant, no holdout, no camera, no multi-depth. This milestone creates the test scripts so they're ready to run the moment a Nuke environment is available.

## User-Visible Outcome

### When this milestone is complete, the user can:

- Run `nuke -x test/test_opendefocus_*.nk` to exercise all DeepCOpenDefocus features
- Inspect EXR results in `test/output/` for visual correctness
- See a visible bokeh disc from a single-pixel point source (the most visually meaningful test)

### Entry point / environment

- Entry point: `nuke -x test/test_opendefocus_*.nk`
- Environment: Machine with Nuke 16+ installed and DeepCOpenDefocus.so in plugin path
- Live dependencies involved: Nuke license, GPU (or CPU fallback via use_gpu false)

## Completion Class

- Contract complete means: All .nk files exist with correct node graph structure, all Write nodes target test/output/, test/output/ is gitignored
- Integration complete means: nuke -x exits 0 for all scripts (requires Nuke environment — deferred to manual UAT)
- Operational complete means: none

## Final Integrated Acceptance

To call this milestone complete, we must prove:

- All 7 .nk test scripts exist in test/ with correct node graphs
- test/output/ directory exists and is gitignored
- scripts/verify-s01-syntax.sh still passes (no C++ regressions)
- Existing test/test_opendefocus.nk updated to write to test/output/ instead of /tmp/

## Risks and Unknowns

- Nuke .nk TCL syntax for Camera2 node — need exact knob names (focal_length, haperture, fstop, etc.)
- DeepFromImage node — need correct knob names for Z channel assignment and premult behavior
- DeepMerge node — need correct operation mode for combining deep streams
- No Nuke available to test-run the scripts — structural correctness only, runtime deferred to UAT

## Existing Codebase / Prior Art

- `test/test_opendefocus.nk` — existing minimal test (single DeepConstant, no holdout/camera)
- `src/DeepCOpenDefocus.cpp` — knob names: focal_length, fstop, focus_distance, sensor_size_mm, use_gpu
- `test/test_ray.nk~` — backup file with examples of Nuke node syntax (Reformat, Grid, Write, etc.)

> See `.gsd/DECISIONS.md` for all architectural and pattern decisions.

## Relevant Requirements

- R060–R066 — All seven integration test requirements

## Scope

### In Scope

- 6 new .nk test scripts (multi-depth, holdout, camera, CPU-only, empty input, bokeh disc)
- Update existing test/test_opendefocus.nk to write to test/output/
- test/output/ directory with .gitignore
- Update scripts/verify-s01-syntax.sh S02 checks to validate new .nk files exist

### Out of Scope / Non-Goals

- Actually running nuke -x (requires Nuke license)
- Modifying any C++ or Rust source code
- Automated pixel-value assertions (manual visual inspection via EXR viewer)

## Technical Constraints

- .nk files must use only built-in Nuke nodes (DeepConstant, DeepMerge, Camera2, Constant, Crop, DeepFromImage, Write) plus DeepCOpenDefocus
- All scripts use small formats (128×72 or 256×256) for fast execution
- EXR output paths must be relative-friendly: test/output/ within the repo

## Integration Points

- DeepCOpenDefocus.so must be in Nuke's plugin path at runtime
- Camera2 node must use knob names that match DDImage CameraOp accessors

## Open Questions

- Exact Camera2 .nk knob names — will use standard Nuke 16 names (focal, haperture, fstop, focal_point)
- DeepFromImage Z assignment — will use the standard `Z` knob or `deep_front` channel assignment
