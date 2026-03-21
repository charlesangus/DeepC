# S02: Alpha correction + UI polish

**Goal:** DeepCBlur has WH_knob blur size, sample optimization twirldown, alpha correction toggle, and compiles via docker-build.sh.
**Demo:** `grep -c "WH_knob" src/DeepCBlur.cpp` returns 1; `grep -c "BeginClosedGroup" src/DeepCBlur.cpp` returns 1; `grep -c "Bool_knob.*alpha_correction" src/DeepCBlur.cpp` returns 1; `bash scripts/verify-s01-syntax.sh` exits 0; `bash docker-build.sh --linux --versions 16.0` exits 0 with DeepCBlur.so in archive.

## Must-Haves

- WH_knob with `double _blurSize[2]` replaces separate Float_knobs for blur width/height (R004)
- `BeginClosedGroup("Sample Optimization")` wraps max_samples, merge_tolerance, color_tolerance knobs (R005)
- `Bool_knob` for `_alphaCorrection` (default false) added to knobs() (R006)
- Post-blur alpha correction pass in doDeepEngine between optimizeSamples and emit loop (R003)
- Help text updated to reflect new capabilities
- Mock DDImage headers updated with WH_knob, Bool_knob, BeginClosedGroup, EndGroup stubs
- docker-build.sh produces DeepCBlur.so (R008)

## Proof Level

- This slice proves: final-assembly
- Real runtime required: no (syntax + docker compilation proof; visual UAT is human-verified)
- Human/UAT required: yes (visual comparison of correction on vs off — deferred to user)

## Verification

- `grep -c "WH_knob" src/DeepCBlur.cpp` returns 1
- `grep "Float_knob.*blur" src/DeepCBlur.cpp` returns no output (old float knobs removed)
- `grep -c "BeginClosedGroup" src/DeepCBlur.cpp` returns 1
- `grep -c "Bool_knob.*alpha_correction" src/DeepCBlur.cpp` returns 1
- `grep -q "cumTransp" src/DeepCBlur.cpp` (alpha correction logic present)
- `bash scripts/verify-s01-syntax.sh` exits 0
- `bash docker-build.sh --linux --versions 16.0` exits 0

## Integration Closure

- Upstream surfaces consumed: S01's separable `doDeepEngine()` with optimizeSamples + emit loop in `src/DeepCBlur.cpp`; `scripts/verify-s01-syntax.sh` mock headers
- New wiring introduced in this slice: alpha correction post-pass in doDeepEngine; WH_knob feeding _blurWidth/_blurHeight via _blurSize[2]
- What remains before the milestone is truly usable end-to-end: nothing — this is the final slice

## Tasks

- [x] **T01: Implement WH_knob, twirldown, alpha correction, and verify build** `est:45m`
  - Why: Delivers all four S02 changes (R003, R004, R005, R006, R008) in a single coherent edit — the member declarations, knobs(), and doDeepEngine changes are tightly coupled within one file
  - Files: `src/DeepCBlur.cpp`, `scripts/verify-s01-syntax.sh`
  - Do: (1) Replace `float _blurWidth`/`_blurHeight` with `double _blurSize[2]` + add `bool _alphaCorrection`; (2) rewrite knobs() with WH_knob, BeginClosedGroup twirldown, Bool_knob; (3) update _validate/getDeepRequests/doDeepEngine to cast `_blurSize[0]`/`_blurSize[1]` to float; (4) insert alpha correction loop after optimizeSamples; (5) update HELP string; (6) add WH_knob/Bool_knob/BeginClosedGroup/EndGroup stubs to mock headers in verify script; (7) run syntax check; (8) run docker build
  - Verify: `bash scripts/verify-s01-syntax.sh` exits 0 AND `bash docker-build.sh --linux --versions 16.0` exits 0
  - Done when: All six grep checks pass, syntax check passes, docker build produces DeepCBlur.so

## Files Likely Touched

- `src/DeepCBlur.cpp`
- `scripts/verify-s01-syntax.sh`

## Observability / Diagnostics

- **Runtime signals:** The `_alphaCorrection` bool knob is inspectable from Nuke's knob panel and script editor (`node["alpha_correction"].value()`). When enabled, the alpha correction pass modifies channel values in-place on the front-to-back sorted samples — visible as brighter composited output vs. uncorrected.
- **Inspection surfaces:** `cumTransp` tracks cumulative transmittance during the correction pass. If a sample sits behind fully opaque layers (cumTransp < 1e-6), it is left uncorrected — this prevents division-by-near-zero artifacts. No additional logging; behaviour is verified visually in Nuke viewer by toggling the knob.
- **Failure visibility:** If the correction produces unexpected brightness, disable the `alpha_correction` knob to isolate. Build failures surface via `docker-build.sh` exit code and compiler diagnostics; syntax issues surface via `verify-s01-syntax.sh`.
- **Redaction:** No secrets or user data involved in this slice.
