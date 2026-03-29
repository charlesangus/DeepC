---
estimated_steps: 12
estimated_files: 1
skills_used: []
---

# T02: Update integration test .nk for depth-selective holdout

Rewrite `test/test_opendefocus_holdout.nk` to contain two subjects at different depths, proving depth-selective holdout behaviour:
- z=5 red subject (behind holdout z=3 → attenuated by T≈0.5)
- z=2 green subject (in front of holdout z=3 → passes through uncut)

Nuke .nk stack ordering (LIFO — last defined before `inputs N` = input 0):
1. `DeepCConstant_holdout` — black, alpha=0.5, front=3.0, back=3.5 (unchanged from original)
2. `DeepCConstant_back` — red {1.0, 0.0, 0.0, 1.0}, front=5.0, back=5.5 (new)
3. `DeepCConstant_front` — green {0.0, 1.0, 0.0, 1.0}, front=2.0, back=2.5 (new)
4. `DeepMerge { inputs 2 }` — merges front(input0) and back(input1); stack after: [merge, holdout]
5. `DeepCOpenDefocus { inputs 2 }` — merge=input0, holdout=input1 ✓
6. `Write { inputs 1 }` → `test/output/holdout_depth_selective.exr`

This stack ordering guarantees: after DeepMerge consumes front+back, DeepCOpenDefocus sees merge as input 0 and holdout as input 1.

The file replaces the existing `test/test_opendefocus_holdout.nk` entirely. Keep the same Root block (format DeepTest128x72). Use `DeepMerge` Nuke node (standard deep compositing node).

## Inputs

- ``test/test_opendefocus_holdout.nk` — current single-subject holdout test`

## Expected Output

- ``test/test_opendefocus_holdout.nk` — updated with two subjects (z=2 green, z=5 red), DeepMerge, and Write to holdout_depth_selective.exr`

## Verification

grep -q 'DeepMerge' test/test_opendefocus_holdout.nk && grep -q 'holdout_depth_selective.exr' test/test_opendefocus_holdout.nk && grep -q '2.0' test/test_opendefocus_holdout.nk && grep -q '5.0' test/test_opendefocus_holdout.nk
