# M010-zkt9ww: DeepCOpenDefocus Integration Tests

## Vision
A suite of 7 nuke -x test scripts covering the full feature surface of DeepCOpenDefocus (multi-depth layer peel, holdout attenuation, camera link, CPU-only mode, empty input, bokeh disc). All EXR output lands in gitignored test/output/ for manual inspection.

## Slice Overview
| ID | Slice | Risk | Depends | Done | After this |
|----|-------|------|---------|------|------------|
| S01 | Integration test .nk suite | low | — | ✅ | 7 test scripts in test/ covering multi-depth, holdout, camera link, CPU-only, empty input, and bokeh disc. All Write nodes target gitignored test/output/. verify-s01-syntax.sh still passes. |
