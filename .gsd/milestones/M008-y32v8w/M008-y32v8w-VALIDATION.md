---
verdict: needs-attention
remediation_round: 0
---

# Milestone Validation: M008-y32v8w

## Success Criteria Checklist

- [x] **Ray traces rays through polynomial lens** — evidence: S02 summary documents full 10-step gather pipeline (sensor mm → pt_sample_aperture Newton → forward poly eval → sphereToCs_full → pinhole project → deep flatten). S02 UAT verify script passes all 19 contracts (13 S01 + 6 S02) including positive grep for `pt_sample_aperture`, `sphereToCs_full`, `logarithmic_focus_search` in renderStripe context, and negative grep confirming scatter vestiges (`coc_norm`) removed. **Runtime proof (non-black output) not yet executed — requires docker + Nuke.**
- [x] **Both nodes output correct frame size** — evidence: S01 UAT TC-02 confirms `info_.format(*di.format())` present in both DeepCDefocusPOThin.cpp (line 229) and DeepCDefocusPORay.cpp (line 325), ordered after `info_.set(di.box())`. Syntax compilation passes.
- [ ] **`nuke -x test/test_ray.nk` exits 0 with non-black 128×72 EXR** — gap: not executed. Requires Docker build environment with Nuke SDK. No runtime proof available at contract level.
- [ ] **`nuke -x test/test_thin.nk` still exits 0 (no regression)** — gap: not executed. Same Docker/Nuke dependency.
- [x] **Vignetting retry loop keeps field-edge pixels from going dark** — evidence: S02 summary confirms `VIGNETTING_RETRIES=10` with retry on Newton failure, outer pupil clip (`|(out[0],out[1])| > _outer_pupil_radius`), and inner pupil clip. S02 verify contract passes. **Functional proof (dark-pixel prevention) requires runtime.**

## Slice Delivery Audit

| Slice | Claimed | Delivered | Status |
|-------|---------|-----------|--------|
| S01 | _validate format fix on both nodes; 5 lens constant knobs with Angenieux defaults; pt_sample_aperture, sphereToCs_full, logarithmic_focus_search in deepc_po_math.h; updated verify script | All items confirmed: format fix in both .cpp files, 5 Float_knobs with correct defaults (36.0, 30.829003, 29.865562, 19.357308, 35.445997), 3 math functions at lines 265/332/421, verify script with 13 contracts, poly.h max_degree default. 15/15 UAT checks pass. | **pass** |
| S02 | Replace scatter renderStripe with gather path-trace engine; vignetting retry loop; deep column flatten with holdout; CA wavelength tracing; expanded getRequests bounds | All items confirmed: full gather engine (10-step pipeline), VIGNETTING_RETRIES=10, front-to-back deep flatten with holdout transmittance, CA at 0.45/0.55/0.65 μm with all-or-nothing channel policy, getRequests padded by max_coc_pixels. Scatter vestiges removed (coc_norm grep returns exit 1). 19/19 contracts pass. | **pass** |

## Cross-Slice Integration

S01 → S02 boundary map is fully satisfied:

| Boundary Item | Produced (S01) | Consumed (S02) | Match |
|---------------|----------------|----------------|-------|
| `pt_sample_aperture()` | deepc_po_math.h:332 | Used in Newton aperture match step | ✅ |
| `sphereToCs_full()` | deepc_po_math.h:265 | Used for 3D ray construction (center=-R convention) | ✅ |
| `logarithmic_focus_search()` | deepc_po_math.h:421 | Called once per stripe for sensor_shift | ✅ |
| 5 lens constant knobs | Ray class members + Float_knob calls | Read directly in renderStripe | ✅ |
| _validate format fix | Both Thin and Ray | Prerequisite for correct frame size | ✅ |
| Direction-only `sphereToCs` | Retained at line 248 | S02 migrated to `sphereToCs_full`; old variant unused but harmless | ✅ |

No boundary mismatches detected.

## Requirement Coverage

| Req | Description | Covered By | Evidence |
|-----|-------------|------------|----------|
| R037 | Ray renderStripe gather path trace | S02 | Full pipeline implemented, scatter removed |
| R038 | sensor_width Float_knob (36mm) | S01 | UAT TC-03: member, ctor init 36.0f, Float_knob call |
| R039 | pt_sample_aperture | S01 | UAT TC-04: Newton 5 iter, 1e-4 tol, 0.72 damping |
| R040 | sphereToCs_full | S01 | UAT TC-04: line 265, origin+direction variant |
| R041 | Vignetting retry loop (10 retries) | S02 | VIGNETTING_RETRIES=10, outer/inner pupil clips, Newton failures |
| R042 | logarithmic_focus_search | S01 | UAT TC-04: 20001-step brute-force |
| R043 | 5 lens constant knobs, Angenieux defaults | S01 | UAT TC-03: all 5 knobs with correct float values |
| R044 | Per-ray deep column fetch + flatten | S02 | Front-to-back holdout transmittance, flat RGBA accumulation |
| R045 | info_.format(*di.format()) in both _validate | S01 | UAT TC-02: Thin line 229, Ray line 325 |
| R035 | CA wavelengths 0.45/0.55/0.65 μm | S02 | Preserved in gather engine with all-or-nothing channel policy |

All requirements addressed. No gaps.

## Verdict Rationale

**All code deliverables are complete and verified at contract level.** Both slices pass their UAT gates. All 9 requirements (R037–R045) plus R035 are covered. Cross-slice boundaries are satisfied. The verify script (19/19 contracts) and g++ syntax checks (4/4 files) pass cleanly.

**Two success criteria remain unverified:** `nuke -x test/test_ray.nk` and `nuke -x test/test_thin.nk` exit-0 checks. These require a Docker build with the Nuke SDK, which is infrastructure outside the worktree scope. Both slices explicitly acknowledged this — S02's summary states "Runtime proof (docker build + nuke -x) is the milestone DoD, not S02 gate."

This is **not a code gap** — no additional slices or code changes are needed. The remaining items are integration-environment checks that will be executed when the milestone is built in the Docker CI pipeline. Verdict is `needs-attention` rather than `needs-remediation` because:

1. No new code slices are required
2. The contract-level evidence is comprehensive (structural grep + compilation)
3. The runtime gap was planned and documented in the roadmap's verification classes ("Integration verification: docker build, nuke -x test scripts")

## Items Requiring Attention

1. **Docker build + `nuke -x` execution** — must be run before the milestone is sealed. Expected to pass based on contract evidence but runtime proof is mandatory per DoD.
2. **Direction-only `sphereToCs`** — still present in deepc_po_math.h but no longer called by Ray's renderStripe after S02 migration. Harmless dead code; can be cleaned up in a future milestone.
3. **`logarithmic_focus_search` housing radius parameter** — accepted but unused (full [-45,+45] range searched). Documented in S01 summary. No functional impact but could be tightened in a future optimization pass.

## Remediation Plan

None required. All code deliverables are complete. The outstanding items (docker build, nuke -x tests) are infrastructure-level integration checks, not code gaps.
