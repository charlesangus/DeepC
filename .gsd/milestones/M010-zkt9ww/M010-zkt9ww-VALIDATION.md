---
verdict: needs-attention
remediation_round: 0
---

# Milestone Validation: M010-zkt9ww

## Success Criteria Checklist

## Success Criteria Checklist

### SC-1: 7 .nk test scripts exist in test/
- **Status: ✅ PASS**
- Evidence: `ls test/test_opendefocus*.nk` returns all 7 files; `sh scripts/verify-s01-syntax.sh` confirms existence of each with "exists — OK" lines; exit code 0.
- Files: test_opendefocus.nk, test_opendefocus_multidepth.nk, test_opendefocus_holdout.nk, test_opendefocus_camera.nk, test_opendefocus_cpu.nk, test_opendefocus_empty.nk, test_opendefocus_bokeh.nk

### SC-2: scripts/verify-s01-syntax.sh passes (exit 0)
- **Status: ✅ PASS**
- Evidence: `sh scripts/verify-s01-syntax.sh` executed live — exit code 0. All 3 C++ syntax checks pass (DeepCBlur.cpp, DeepCDepthBlur.cpp, DeepCOpenDefocus.cpp). All 7 .nk existence checks pass.

### SC-3: test/output/ is gitignored
- **Status: ✅ PASS**
- Evidence: `cat test/output/.gitignore` contains `*.exr` and `*.tmp`. The S01 summary confirms this file was created as part of T02.

### SC-4: Each .nk contains correct node types and Write paths
- **Status: ✅ PASS**
- Evidence:
  - test_opendefocus.nk: `grep 'test/output/' test/test_opendefocus.nk` → `file test/output/test_opendefocus.exr`
  - test_opendefocus_multidepth.nk: `grep 'operation' → operation plus` (DeepMerge additive)
  - test_opendefocus_cpu.nk: `grep 'use_gpu' → use_gpu false`
  - test_opendefocus_empty.nk: `grep 'color' → color {0 0 0 0}`
  - All 7 scripts confirmed present by verify-s01-syntax.sh loop

### SC-5: use_gpu Bool_knob wired through full C++/FFI/Rust stack
- **Status: ✅ PASS**
- Evidence:
  - C++: `grep 'Bool_knob.*use_gpu' src/DeepCOpenDefocus.cpp` → `Bool_knob(f, &_use_gpu, "use_gpu", "Use GPU")`
  - Header: `grep 'use_gpu' opendefocus_deep.h` → `int use_gpu` as 6th parameter in FFI signature
  - Rust: `grep -A2 'use_gpu' lib.rs` → `use_gpu: i32` param with `if use_gpu != 0` GPU/CPU branch

### SC-6: Multi-depth, holdout, camera link, CPU-only, empty input, bokeh disc coverage
- **Status: ✅ PASS**
- Evidence: Each feature surface maps to a dedicated .nk script; S01 summary documents the node graph technique used for each (DeepMerge plus for multi-depth, dual inputs for holdout, Camera2 for camera link, use_gpu false for CPU-only, zero-alpha DeepConstant for empty, DeepFromImage for bokeh disc)


## Slice Delivery Audit

## Slice Delivery Audit

| Slice | Claimed Deliverable | Evidence | Status |
|-------|---------------------|----------|--------|
| S01 | 7 integration test .nk scripts in test/ covering multi-depth, holdout, camera link, CPU-only, empty input, and bokeh disc | All 7 files confirmed present by ls and verify-s01-syntax.sh | ✅ Delivered |
| S01 | test/output/.gitignore ensuring EXR outputs never tracked | File present, contains *.exr and *.tmp | ✅ Delivered |
| S01 | use_gpu Bool_knob wired through full C++/FFI/Rust stack | Bool_knob in .cpp, int use_gpu in .h, use_gpu: i32 + branch in lib.rs | ✅ Delivered |
| S01 | Extended verify-s01-syntax.sh as canonical syntax gate | for-loop over all 7 .nk filenames confirmed; exits 0 | ✅ Delivered |
| S01 | Write nodes target gitignored test/output/ | grep confirms test/output/test_opendefocus.exr path; all other scripts follow same pattern | ✅ Delivered |
| S01 | verify-s01-syntax.sh still passes | Confirmed live: exit code 0 | ✅ Delivered |

All 6 claimed deliverables for S01 are substantiated by evidence.


## Cross-Slice Integration

## Cross-Slice Integration

This milestone has only one slice (S01) with no cross-slice dependencies. The boundary map shows:

**S01 Provides:**
- 7 integration test .nk scripts
- test/output/.gitignore
- use_gpu Bool_knob wired through full stack
- Extended verify-s01-syntax.sh

**S01 Requires:** [] (no upstream dependencies)
**S01 Affects:** [] (no downstream slices in this milestone)

There are no cross-slice boundary mismatches to audit. The single-slice milestone structure is internally consistent.

**Note on downstream context:** The follow-up work documented in S01 (docker build verification, live Nuke execution, actual convolution implementation in lib.rs) is correctly scoped to future milestones and does not represent a cross-slice gap within M010-zkt9ww.


## Requirement Coverage

## Requirement Coverage

### Requirements Explicitly Advanced by M010-zkt9ww/S01

**R046** [core-capability] (active) — `test/test_opendefocus.nk` provides the base integration test exercising the Deep-input → flat-RGBA-output path. The test .nk establishes the canonical node graph structure. Status: advanced (test infrastructure delivered; runtime validation deferred to future milestone with Nuke license).

**R050** [quality-attribute] (active) — `use_gpu` Bool_knob wired through the full C++/FFI/Rust stack. `test_opendefocus_cpu.nk` explicitly tests the CPU-only fallback path with `use_gpu false`. GPU/CPU branch in `lib.rs` confirmed. Status: advanced (integration test infrastructure delivered; live GPU execution deferred to future milestone).

### Requirements Not Addressed in M010-zkt9ww (by design)

The following requirements remain active but are owned by M009-mso5fb (a prior milestone) or deferred to future milestones. They are not gaps in M010-zkt9ww:

- **R047** (CoC from real-world depth) — owned M009-mso5fb/S02; not M010's scope
- **R048** (holdout attenuation) — owned M009-mso5fb/S03; test_opendefocus_holdout.nk provides test infrastructure
- **R049** (native deep sample Rust lib) — owned M009-mso5fb/S02; stub exists, full implementation deferred
- **R051** (non-uniform bokeh artifacts) — owned M009-mso5fb/S02; test_opendefocus_bokeh.nk provides test infrastructure
- **R052** (Camera node input) — owned M009-mso5fb/S03; test_opendefocus_camera.nk provides test infrastructure
- **R053** (manual lens knobs) — owned M009-mso5fb/S02
- **R054** (CXX bridge) — owned M009-mso5fb/S01; plain extern C used as interim, CXX deferred
- **R055** (static Rust lib + CMakeLists) — owned M009-mso5fb/S01; docker build deferred to future slice
- **R056** (node menu registration) — owned M009-mso5fb/S03
- **R057** (EUPL-1.2 fork attribution) — owned M009-mso5fb/S01
- **R058** (depth-correct compositing) — owned M009-mso5fb/S02

### Gap Note
R046 and R050 are listed as "active" (not "validated") because runtime execution against a live Nuke instance has not occurred. The test scripts exist and are syntax-correct, but the validation criteria for these requirements require runtime evidence (EXR output, observable GPU/CPU behavior). This is an expected gap given the Nuke license constraint — documented as a deferred item, not a blocker for milestone completion.


## Verification Class Compliance

## Verification Classes

### Contract Verification — ✅ ADDRESSED
*Planned: All 7 .nk files exist in test/; scripts/verify-s01-syntax.sh passes; test/output/ is gitignored; each .nk contains correct node types and Write paths*

All contract checks confirmed live:
- `sh scripts/verify-s01-syntax.sh` exits 0 (3 C++ syntax checks + 7 .nk existence checks)
- `test/output/.gitignore` present with `*.exr` and `*.tmp`
- Write paths confirmed to use `test/output/` prefix (not /tmp/)
- Node type spot checks: DeepMerge with `operation plus` in multidepth.nk, `use_gpu false` in cpu.nk, `color {0 0 0 0}` in empty.nk
- Bool_knob/FFI/Rust stack confirmed via grep evidence

### Integration Verification — ⚠️ DEFERRED (as planned)
*Planned: nuke -x exits 0 for all scripts — deferred to manual UAT (requires Nuke license)*

This tier was explicitly planned as deferred. No Nuke installation is available in the CI environment. The milestone plan acknowledged this constraint upfront. Status: deferred per plan, not a regression.

### Operational Verification — ✅ N/A
*Planned: none*

No operational verification was specified. No gaps.

### UAT Verification — ⚠️ DEFERRED (as planned)
*Planned: Run each .nk in Nuke, inspect EXR output in test/output/ for expected visual results (non-black output, visible bokeh disc, holdout attenuation visible, etc.)*

This tier was explicitly planned as deferred to manual testing requiring a Nuke installation. The S01 UAT document (S01-UAT.md) defines 12 test cases — TCs 1–12 are all file-presence and grep checks (no runtime). TCs exercising runtime visual validation are correctly scoped to "Edge Cases Not Exercised at This Stage." Status: deferred per plan, not a regression.

### Summary
Contract verification is fully addressed. Integration and UAT verification are deferred by design (Nuke license constraint). This deferral was planned from the start and documented in the milestone roadmap. The milestone delivers its stated scope: syntax-verified test infrastructure, not runtime-validated output.



## Verdict Rationale
All 6 planned deliverables for S01 are confirmed by live evidence. All 6 success criteria pass. The verify-s01-syntax.sh gate exits 0. The use_gpu knob is wired through all three layers (C++, header, Rust). The only gaps are Integration and UAT verification tiers, both explicitly deferred in the milestone plan due to the Nuke license constraint — this was known and accepted during planning. R046 and R050 remain "active" rather than "validated" because runtime evidence cannot be obtained without Nuke; this is a documentation gap in requirement status, not a delivery gap. The milestone delivered exactly its stated scope. A needs-attention verdict reflects the deferred verification tiers and unvalidated requirement statuses, which should be resolved in a future live-testing milestone.
