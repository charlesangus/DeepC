# M004-ks4br0: DeepCBlur correctness + DeepCDepthBlur

**Vision:** Fix the colour-comparison premult bug in the sample optimizer producing jaggy blur output, add same-depth auto-merge, and ship a new DeepCDepthBlur node that spreads samples along the Z axis only with multiple falloff modes and an optional second-image mask input.

## Success Criteria

- DeepCBlur output is smooth with default colour tolerance — no concentric-rectangle jaggies on hard-surface inputs
- Same-depth samples are collapsed automatically before and after colour-tolerance merge
- DeepCDepthBlur flattens to the same 2D image as its input (before/after flatten are identical)
- DeepCDepthBlur produces tidy deep output (no overlapping samples, sorted by zFront)
- DeepCDepthBlur offers linear, gaussian, smoothstep, and exponential falloff modes
- When second input is connected, DeepCDepthBlur only spreads samples at pixels where the depth ranges interact
- docker-build.sh produces DeepCBlur.so, DeepCBlur2.so, DeepCDepthBlur.so

## Key Risks / Unknowns

- **Premult correction scope** — channels in SampleRecord are premultiplied. Unpremultiplying for comparison requires dividing by alpha, which has a near-zero guard issue. Must be done carefully.
- **Tidy output for DeepCDepthBlur** — "tidy" means non-overlapping samples sorted by zFront with zBack ≤ next zFront. Inserting spread samples that straddle existing sample boundaries requires splitting or clamping.
- **Second input depth-overlap test** — must not require a full sort of both sample lists per pixel; must be cheap enough to run per-pixel.

## Proof Strategy

- Premult bug → retire in S01 by showing blur output on test input is smooth (no jaggies) with default colour tolerance
- Same-depth collapse → retire in S01 by verifying merge pass eliminates duplicates even at mergeTolerance=0
- Tidy output → retire in S02 by proving flatten(before) == flatten(after) on a non-trivial deep input and that no overlapping samples remain in output

## Verification Classes

- Contract verification: docker-build.sh exits 0 for all three .so files; g++ -fsyntax-only passes
- Integration verification: DeepCDepthBlur output is tidy (zFront sorted, no overlap); flatten invariant holds
- Operational verification: none
- UAT / human verification: visual confirm that DeepCBlur jaggies are gone; DeepCDepthBlur shows softened holdout when merged

## Milestone Definition of Done

This milestone is complete only when all are true:

- DeepSampleOptimizer colourDistance operates on unpremultiplied values
- Same-depth samples are auto-merged regardless of colour/merge tolerance
- DeepCBlur and DeepCBlur2 both use the corrected optimizer
- DeepCDepthBlur compiles and loads in Nuke
- DeepCDepthBlur output passes flatten-invariant check
- DeepCDepthBlur output is tidy (non-overlapping, sorted)
- docker-build.sh exit 0 for all three plugins

## Requirement Coverage

- Covers: R001, R002, R003, R004, R005
- Partially covers: none
- Leaves for later: none
- Orphan risks: none

## Slices

- [ ] **S01: Fix DeepSampleOptimizer colour comparison** `risk:medium` `depends:[]`
  > After this: DeepCBlur and DeepCBlur2 both produce smooth blur on hard-surface inputs with default colour tolerance; same-depth samples are collapsed.

- [ ] **S02: DeepCDepthBlur node** `risk:high` `depends:[S01]`
  > After this: DeepCDepthBlur spreads samples in Z, passes flatten invariant, produces tidy output, and offers four falloff modes with optional second-input depth-overlap masking.

## Boundary Map

### S01 → S02

Produces:
- `DeepSampleOptimizer.h` — corrected `colorDistance` that unpremultiplies before comparison
- Same-depth auto-merge pass (Z-exact dedup regardless of tolerance settings)
- Both DeepCBlur and DeepCBlur2 using the corrected header

Consumes:
- nothing (first slice)

### S02 consumes from S01

Consumes:
- Corrected `DeepSampleOptimizer.h` (used internally for tidy-up after depth spread)

Produces:
- `src/DeepCDepthBlur.cpp` — new DeepFilterOp plugin
- `src/CMakeLists.txt` updated with DeepCDepthBlur entry
- Tidy output guarantee (non-overlapping, sorted by zFront)
- Four falloff modes: linear, gaussian, smoothstep, exponential
- Optional second deep input for depth-overlap gating
