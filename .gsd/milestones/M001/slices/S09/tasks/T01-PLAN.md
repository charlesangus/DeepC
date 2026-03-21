# T01: 08-documentation-overhaul 01

**Slice:** S09 — **Milestone:** M001

## Description

Overhaul README.md to reflect the current 23-plugin DeepC project with docker-build.sh workflow, and create THIRD_PARTY_LICENSES.md for vendored code attribution.

Purpose: README.md is stale — it references removed plugins (DeepCBlink), promises unbuilt features (DeepCompress), documents obsolete build steps (CentOS, VS2017), and lacks DeepThinner. This plan brings all documentation current.
Output: Updated README.md and new THIRD_PARTY_LICENSES.md

## Must-Haves

- [ ] "README.md lists all 23 plugins including DeepThinner with attribution and link"
- [ ] "README.md build section describes docker-build.sh workflow for Nuke 16+"
- [ ] "README.md contains no references to DeepCBlink or DeepCompress"
- [ ] "README.md contains no CentOS, VS2017, or batchInstall.sh references"
- [ ] "THIRD_PARTY_LICENSES.md exists with DeepThinner and FastNoise MIT attribution"

## Files

- `README.md`
- `THIRD_PARTY_LICENSES.md`
