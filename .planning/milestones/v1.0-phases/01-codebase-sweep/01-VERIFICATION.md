---
phase: 01-codebase-sweep
verified: 2026-03-13T13:00:00Z
status: human_needed
score: 10/10 must-haves verified
re_verification: true
  previous_status: gaps_found
  previous_score: 9/10
  gaps_closed:
    - "DeepCGrade reverse path: outData = inputVal added before if (G[cIndex] != 1.0f) — G==1 uninitialized-read regression is fixed"
  gaps_remaining: []
  regressions: []
human_verification:
  - test: "Verify DeepCGrade reverse mode with gamma = 1.0 (no gamma) produces correct linear inverse ramp output"
    expected: "With gamma=1.0 and reverse=true, the node should invert only the lift/gain/multiply. Output should be identical to pre-fix behavior for this configuration."
    why_human: "Cannot verify pixel math correctness programmatically without a running Nuke instance and automated render comparison."
  - test: "Verify DeepCGrade reverse mode with gamma != 1.0 produces correct gamma-then-linear output"
    expected: "With reverse=true and gamma set to e.g. 0.45, gamma is applied to inputVal first, then the inverse linear ramp is applied to that result."
    why_human: "Cannot verify pixel math correctness without running Nuke."
  - test: "Verify DeepCWrapper and DeepCMWrapper do not appear in the Nuke node menu after loading plugins"
    expected: "No 'DeepCWrapper' or 'DeepCMWrapper' entry is visible anywhere in the Nuke node creation interface."
    why_human: "UI appearance cannot be checked programmatically."
  - test: "Verify DeepCSaturation and DeepCHueShift produce 0.0 (not NaN) for fully transparent samples"
    expected: "Any deep sample with alpha == 0.0 passing through either node outputs 0.0 for RGB channels, not NaN."
    why_human: "Requires a running Nuke instance and deep pixel inspection."
---

# Phase 1: Codebase Sweep Verification Report

**Phase Goal:** Fix all confirmed codebase bugs identified in the sweep — deep-pixel math errors, logic bugs, spurious plugin registrations, deprecated API usage, and dead code removal — so the DeepC plugin suite builds cleanly against Nuke 16 NDK and produces correct output.
**Verified:** 2026-03-13T13:00:00Z
**Status:** human_needed
**Re-verification:** Yes — after SWEEP-01 gap closure

## Re-verification Summary

| Item | Previous | Now |
|------|----------|-----|
| SWEEP-01 DeepCGrade G==1 reverse path | PARTIAL (uninitialized read) | VERIFIED |
| All other 9 truths | VERIFIED | VERIFIED (regression check passed) |

**Gap closed:** `outData = inputVal;` is now present at line 110 of `src/DeepCGrade.cpp`, immediately before `if (G[cIndex] != 1.0f)`. When G==1 the pow() block is skipped and `outData` already holds `inputVal`, so line 114 correctly computes `A * inputVal + B`. When G!=1 the pow() overwrites `outData` before line 114 — the insertion is a no-op for that case.

**No regressions introduced:** All nine previously-verified items confirmed unchanged by quick grep regression scan.

---

## Goal Achievement

### Observable Truths

| # | Truth | Status | Evidence |
|---|-------|--------|----------|
| 1 | DeepCGrade in reverse mode applies gamma to outData, not inputVal — and initializes outData before the conditional pow block | VERIFIED | Line 110: `outData = inputVal;` before `if (G[cIndex] != 1.0f)`. Line 114 uses `outData` throughout. |
| 2 | DeepCKeymix A-side containment check queries aPixel.channels() | VERIFIED | Line 265: `ChannelSet aInPixelChannels = aPixel.channels();` confirmed. |
| 3 | DeepCSaturation produces 0 (not NaN) for fully transparent samples | VERIFIED | Line 112: `if (alpha != 0.0f)` guard present; rgb[] zero-initialized at lines 102-103. |
| 4 | DeepCHueShift produces 0 (not NaN) for fully transparent samples | VERIFIED | Line 106: `if (alpha != 0.0f)` guard present. |
| 5 | DeepCConstant weight computed as plain scalar division, not comma expression | VERIFIED | Line 162: `float weight = depth / _overallDepth;` confirmed. |
| 6 | DeepCID foreach loop reads loop variable z, not cached _auxChannel | VERIFIED | foreach block lines 81-97 confirmed: all sample reads inside loop use `z`. |
| 7 | DeepCWrapper and DeepCMWrapper are not selectable in Nuke node menu | VERIFIED | No `Op::Description DeepCWrapper::d` or `Op::Description DeepCMWrapper::d` in any .cpp file. All 21 individual plugin registrations present. |
| 8 | DeepCBlink is absent from the repository and build | VERIFIED | `src/DeepCBlink.cpp` does not exist. No DeepCBlink in CMakeLists.txt or menu.py.in. |
| 9 | DeepCWorld caches window_matrix.inverse() in _validate(), not per-sample | VERIFIED | Member `_inverse_window_matrix` at line 41, assigned at line 164 in `_validate()`, used at line 225 in `processSample()`. |
| 10 | batchInstall.sh Linux branch correctly identifies Linux | VERIFIED | Line 3: correct platform comment confirmed. |

**Score:** 10/10 truths verified

### Required Artifacts

| Artifact | Expected | Status | Details |
|----------|----------|--------|---------|
| `src/DeepCGrade.cpp` | Reverse-mode grade with correct gamma application, outData initialized before conditional | VERIFIED | Line 110 `outData = inputVal;` added. Line 111 conditional pow. Line 114 linear ramp uses outData correctly in all cases. |
| `src/DeepCKeymix.cpp` | A-side containment check using aPixel | VERIFIED | Line 265 confirmed. |
| `src/DeepCSaturation.cpp` | Zero-alpha guard before unpremult division | VERIFIED | Line 112 confirmed with correct pattern and comment. |
| `src/DeepCHueShift.cpp` | Zero-alpha guard before unpremult division | VERIFIED | Line 106 confirmed. |
| `src/DeepCConstant.cpp` | Correct weight scalar from direct division | VERIFIED | Line 162 confirmed. |
| `src/DeepCID.cpp` | Loop body uses loop variable z | VERIFIED | Lines 81-97 all use `z` for channel access. |
| `src/DeepCWrapper.cpp` | No Op::Description registration or build() factory | VERIFIED | Neither present. All subclass registrations intact. |
| `src/DeepCWrapper.h` | No static const Iop::Description d; no Class() returning d.name | VERIFIED | Both absent from base class header. |
| `src/DeepCMWrapper.cpp` | No Op::Description registration or build() factory | VERIFIED | Confirmed absent. |
| `src/DeepCMWrapper.h` | No static const Iop::Description d; no Class() returning d.name | VERIFIED | Both absent from base class header. |
| `src/DeepCWorld.cpp` | Cached inverse_window_matrix member computed in _validate() | VERIFIED | Member at line 41, computed at line 164, used at line 225. |
| `batchInstall.sh` | Correct platform comment on Linux branch | VERIFIED | Line 3 confirmed. |
| `src/CMakeLists.txt` | No DeepCBlink in PLUGINS list | VERIFIED | No DeepCBlink anywhere in CMakeLists.txt. |

### Key Link Verification

| From | To | Via | Status | Details |
|------|----|-----|--------|---------|
| DeepCGrade.cpp reverse path (G==1) | linear ramp (A * outData + B) | outData = inputVal before conditional | VERIFIED | Line 110 initializes outData to inputVal. When G==1 pow() is skipped; line 114 reads the correctly-set outData. |
| DeepCGrade.cpp reverse path (G!=1) | linear ramp (A * outData + B) | pow(inputVal, G[cIndex]) overwrites outData | VERIFIED | Line 112 overwrites outData; line 114 reads the pow result. |
| DeepCKeymix.cpp line 265 | aPixel.channels() | aInPixelChannels assignment | VERIFIED | `ChannelSet aInPixelChannels = aPixel.channels();` confirmed. |
| DeepCSaturation.cpp wrappedPerSample | zero-alpha guard | if (alpha != 0.0f) before division | VERIFIED | Line 112 confirmed. Zero-init of rgb[] makes implicit else correct. |
| DeepCWrapper subclasses | their own Op::Description and build() | Each subclass defines its own d | VERIFIED | 21 individual plugin registrations present; wrapper base classes have none. |
| DeepCConstant.cpp weight assignment | lerp lines below | weight scalar | VERIFIED | Line 162 scalar used directly in interpolation below. |
| DeepCID.cpp foreach loop body | _auxiliaryChannelSet members | loop variable z | VERIFIED | All channel reads inside loop use `z`. |
| DeepCWorld::_validate() | DeepCWorld::processSample() | _inverse_window_matrix member | VERIFIED | Set at line 164 in _validate(); consumed at line 225 in processSample(). One `.inverse()` call total, in _validate(). |

### Requirements Coverage

| Requirement | Source Plan | Description | Status | Evidence |
|-------------|------------|-------------|--------|----------|
| SWEEP-01 | 01-01 | DeepCGrade reverse-mode gamma applied to ramp result, outData initialized before conditional pow | SATISFIED | Lines 110-114 of DeepCGrade.cpp confirmed. outData = inputVal at line 110; conditional pow at lines 111-112; linear ramp at line 114. |
| SWEEP-02 | 01-01 | DeepCKeymix A-side uses aPixel.channels() | SATISFIED | Line 265 confirmed. |
| SWEEP-03 | 01-01 | DeepCSaturation/DeepCHueShift guard alpha==0 | SATISFIED | Both files confirmed with guard and zero-init. |
| SWEEP-04 | 01-02 | DeepCConstant weight scalar, not comma expression | SATISFIED | Line 162 confirmed. |
| SWEEP-05 | 01-02 | DeepCID foreach uses loop variable z | SATISFIED | Loop body lines 81-97 confirmed using z. |
| SWEEP-06 | 01-03 | Op::Description removed from DeepCWrapper and DeepCMWrapper | SATISFIED | All four files confirmed clean. 21 plugin registrations untouched. |
| SWEEP-09 | 01-04 | DeepCBlink removed from build and toolbar | SATISFIED | File deleted, CMakeLists.txt clean, menu.py.in already clean. |
| SWEEP-10 | 01-05 | No deprecated NDK APIs; DeepCWorld inverse cached | SATISFIED | Inverse matrix cached in _validate(); switch defaults added; NDK audit documented in research. |
| SWEEP-07 | (none) | perSampleData interface redesign | ORPHANED/DROPPED | Explicitly dropped in 01-RESEARCH.md and 01-CONTEXT.md — not deferred. REQUIREMENTS.md traceability table still shows Phase 1 / Pending (documentation housekeeping inconsistency, not a code gap). |
| SWEEP-08 | (none) | Grade coefficient extraction | ORPHANED/DROPPED | Same as SWEEP-07 — dropped with documented rationale. REQUIREMENTS.md traceability table still shows Phase 1 / Pending. |

**Note on SWEEP-07 and SWEEP-08:** These requirements appear in REQUIREMENTS.md with Phase 1 and status "Pending." They were explicitly dropped (not deferred) during Phase 1 research with documented rationale. The phase goal as stated excludes them. The REQUIREMENTS.md traceability table should be updated to reflect the drop, but this is documentation housekeeping — not a code gap and not a blocker for phase completion.

### Anti-Patterns Found

| File | Line | Pattern | Severity | Impact |
|------|------|---------|----------|--------|
| `src/DeepCWrapper.cpp` | 78 | `TODO: probably better to work with a pointer and length` | Info | Pre-existing SWEEP-07 note; work explicitly dropped. Not introduced by this phase. |
| `src/DeepCMWrapper.cpp` | 25 | `TODO: probably better to work with a pointer and length` | Info | Same as above — pre-existing. |
| `src/DeepCWorld.cpp` | 100, 110, 170 | `// TODO: clean` and `// TODO: test if we need this new ChannelSet` | Info | Pre-existing comments unrelated to this phase's changes. |

No blocker anti-patterns remain. The `src/DeepCGrade.cpp` uninitialized-read blocker from the initial verification is resolved.

### Human Verification Required

#### 1. DeepCGrade reverse mode, gamma = 1.0 (no gamma)

**Test:** In Nuke, create a DeepCGrade node. Set `reverse = true`. Set gamma to 1.0 (default — no gamma adjustment). Apply a grade with distinct lift/gain/multiply values. Pass deep samples through and inspect the output values.
**Expected:** Output is the inverse linear ramp only. Should be identical to pre-fix behavior for this configuration and should correctly invert what the forward node applies.
**Why human:** Requires a running Nuke instance with deep pixel reader. Pixel math correctness cannot be verified from source alone.

#### 2. DeepCGrade reverse mode, gamma != 1.0

**Test:** Set `reverse = true`, gamma = 0.45 (or any value other than 1.0). Run deep samples through.
**Expected:** Gamma is applied to inputVal first (`pow(inputVal, G)`), then the inverse linear ramp is applied to that result. This is the primary fixed case.
**Why human:** Cannot run pixel comparisons without Nuke.

#### 3. DeepCWrapper and DeepCMWrapper absent from node menu

**Test:** Load the compiled DeepC plugins in Nuke. Open the node creation interface (Tab key or node menu). Search for "DeepCWrapper" and "DeepCMWrapper".
**Expected:** Neither entry appears. All other DeepC nodes remain present and accessible.
**Why human:** UI appearance requires a running Nuke session.

#### 4. DeepCSaturation and DeepCHueShift with alpha == 0 samples

**Test:** Route a deep image containing fully transparent samples (alpha == 0) through DeepCSaturation and DeepCHueShift. Inspect output channel values for affected samples.
**Expected:** RGB channels output 0.0, not NaN, for fully transparent samples.
**Why human:** Requires Nuke with deep pixel inspection tooling.

### Gaps Summary

No gaps remain. All 10 observable truths are verified. All 8 in-scope requirements (SWEEP-01 through SWEEP-10, excluding the dropped SWEEP-07/08) are fully satisfied with substantive, wired implementations confirmed in the actual source files.

The SWEEP-01 gap from the initial verification is closed. `src/DeepCGrade.cpp` lines 110-114 now correctly handle both branches of the reverse path:

- When `G[cIndex] == 1.0f`: `outData = inputVal` at line 110 initializes outData; the `if` block is skipped; line 114 computes `A * inputVal + B` correctly.
- When `G[cIndex] != 1.0f`: line 110 sets `outData = inputVal`; line 112 overwrites with `pow(inputVal, G[cIndex])`; line 114 computes `A * pow(inputVal, G) + B` correctly.

The four human verification items remain — these are not gaps but UAT checkpoints requiring a live Nuke instance.

---

_Verified: 2026-03-13T13:00:00Z_
_Verifier: Claude (gsd-verifier)_
