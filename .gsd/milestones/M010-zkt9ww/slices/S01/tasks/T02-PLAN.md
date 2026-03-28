---
estimated_steps: 1
estimated_files: 8
skills_used: []
---

# T02: Create test/output/ gitignore, update existing .nk path, and author all 6 new .nk test scripts

Pure file-authoring task. Creates the test/output/ gitignore so EXR outputs are never tracked, fixes the existing test_opendefocus.nk to write to test/output/ instead of /tmp/, and creates the 6 new .nk integration test scripts covering R060–R062 and R064–R066. All scripts use the Nuke .nk stack-based node graph format confirmed in the research doc. The use_gpu knob name must match T01's output exactly.

## Inputs

- ``test/test_opendefocus.nk` — existing test script; change Write file path from /tmp/test_opendefocus.exr to test/output/test_opendefocus.exr`
- ``src/DeepCOpenDefocus.cpp` — confirms knob names: focal_length, fstop, focus_distance, sensor_size_mm, use_gpu (added in T01)`

## Expected Output

- ``test/output/.gitignore` — contents: `*.exr\n*.tmp\n``
- ``test/test_opendefocus.nk` — Write file knob changed from `/tmp/test_opendefocus.exr` to `test/output/test_opendefocus.exr``
- ``test/test_opendefocus_multidepth.nk` — R060: 3 DeepConstants at deep_front 2/5/10 merged via DeepMerge(inputs 3, operation plus) → DeepCOpenDefocus → Write(test/output/multidepth.exr)`
- ``test/test_opendefocus_holdout.nk` — R061: DeepConstant(holdout, alpha=0.5, deep_front 3.0) defined first, then DeepConstant(subject, deep_front 5.0) on top of stack → DeepCOpenDefocus(inputs 2) → Write(test/output/holdout.exr)`
- ``test/test_opendefocus_camera.nk` — R062: Camera2(focal 35, haperture 2.4, fstop 1.4, focal_point 200) defined first, push 0 for null holdout, DeepConstant → DeepCOpenDefocus(inputs 3) → Write(test/output/camera.exr)`
- ``test/test_opendefocus_cpu.nk` — R063: DeepConstant → DeepCOpenDefocus(use_gpu false) → Write(test/output/cpu.exr)`
- ``test/test_opendefocus_empty.nk` — R064: DeepConstant(color {0 0 0 0}, deep_front 5.0, deep_back 5.5) → DeepCOpenDefocus → Write(test/output/empty.exr)`
- ``test/test_opendefocus_bokeh.nk` — R066: Constant(color {1 1 1 1}, format 256x256) → Crop(box {127 127 128 128}) → DeepFromImage(set_z true, z 1.0) → DeepCOpenDefocus(focus_distance 10) → Write(test/output/bokeh.exr)`

## Verification

ls test/test_opendefocus.nk test/test_opendefocus_multidepth.nk test/test_opendefocus_holdout.nk test/test_opendefocus_camera.nk test/test_opendefocus_cpu.nk test/test_opendefocus_empty.nk test/test_opendefocus_bokeh.nk && grep 'test/output/' test/test_opendefocus.nk && cat test/output/.gitignore
