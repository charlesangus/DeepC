# M005-9j5er8: DeepCDepthBlur Correctness

**Vision:** Fix DeepCDepthBlur's alpha decomposition to use multiplicative splitting (`α_sub = 1 - (1-α)^w`) instead of additive scaling (`α * w`), ensuring the flatten invariant holds under Nuke's deep compositing model. Rename second input from "B" to "ref". Eliminate zero-alpha samples from output.

## Success Criteria

- Flattened output of DeepCDepthBlur matches flattened input (flatten invariant holds)
- No zero-alpha samples in output for any combination of spread, num_samples, and falloff mode
- Second input labeled "ref", main input unlabelled, both always visible
- `docker-build.sh --linux --versions 16.0` exits 0 with DeepCDepthBlur.so

## Key Risks / Unknowns

- Numerical precision with `pow(1-α, w)` at extreme alpha values — retire in S01 by clamping sub-sample alpha and skipping near-zero results

## Proof Strategy

- Numerical precision → retire in S01 by implementing clamp threshold and testing against zero-alpha guard

## Verification Classes

- Contract verification: docker-build.sh, scripts/verify-s01-syntax.sh, source code inspection of alpha formula
- Integration verification: .so loads in Nuke 16+ (user UAT)
- Operational verification: none
- UAT / human verification: DeepToImage A/B comparison in Nuke

## Milestone Definition of Done

This milestone is complete only when all are true:

- S01 deliverables complete (alpha math fix, input rename, zero-alpha guard)
- `docker-build.sh --linux --versions 16.0` exits 0, DeepCDepthBlur.so in zip
- `scripts/verify-s01-syntax.sh` passes
- Source code uses `1 - pow(1-α, w)` formula, not `α * w`
- No code path can emit a sub-sample with α < 1e-6

## Requirement Coverage

- Covers: R013, R017, R018
- Partially covers: none
- Leaves for later: none
- Orphan risks: none

## Slices

- [x] **S01: Correct alpha decomposition + input rename** `risk:high` `depends:[]`
  > After this: DeepCDepthBlur builds via docker-build.sh; spreading uses `1-(1-α)^w` multiplicative alpha split; zero-alpha input samples pass through; zero-alpha sub-samples skipped; second input labeled "ref", main unlabelled. Flatten invariant correctness provable by source inspection; full visual confirmation via user UAT in Nuke.

## Boundary Map

### S01

Produces:
- Modified `src/DeepCDepthBlur.cpp` — corrected `doDeepEngine` spreading section with multiplicative alpha decomposition, zero-alpha guard, renamed input labels
- Passing `docker-build.sh --linux --versions 16.0` producing DeepCDepthBlur.so
- Passing `scripts/verify-s01-syntax.sh`

Consumes:
- nothing (standalone correctness fix)
