# S01: Integration test .nk suite

**Goal:** Add use_gpu Bool_knob through the C++/FFI/Rust stack, create test/output/ gitignore, update the existing .nk test path, and author 6 new .nk integration test scripts covering multi-depth, holdout, camera link, CPU-only, empty input, and bokeh disc. Extend verify-s01-syntax.sh to assert all 7 .nk files exist and pass syntax checks.
**Demo:** After this: 7 test scripts in test/ covering multi-depth, holdout, camera link, CPU-only, empty input, and bokeh disc. All Write nodes target gitignored test/output/. verify-s01-syntax.sh still passes.

## Tasks
- [x] **T01: Added use_gpu Bool_knob to DeepCOpenDefocus, extended C FFI header with int use_gpu param, and wired GPU/CPU branch through Rust opendefocus_deep_render** — The test_opendefocus_cpu.nk script (T02) needs to write `use_gpu false` to a real knob. That knob does not yet exist. This task adds it as a Bool_knob in DeepCOpenDefocus.cpp, adds an `int use_gpu` parameter to the C FFI function signature in opendefocus_deep.h, and updates lib.rs to accept and honour that parameter by passing it to OpenDefocusRenderer::new(). These are small but cross-language changes; doing this first ensures T02 uses the correct knob name and the syntax check passes before any .nk files are authored.
  - Estimate: 45m
  - Files: src/DeepCOpenDefocus.cpp, crates/opendefocus-deep/include/opendefocus_deep.h, crates/opendefocus-deep/src/lib.rs
  - Verify: sh scripts/verify-s01-syntax.sh
- [x] **T02: Created test/output/.gitignore, updated test_opendefocus.nk Write path to test/output/, and authored 6 new .nk integration test scripts (multi-depth, holdout, camera, CPU-only, empty, bokeh)** — Pure file-authoring task. Creates the test/output/ gitignore so EXR outputs are never tracked, fixes the existing test_opendefocus.nk to write to test/output/ instead of /tmp/, and creates the 6 new .nk integration test scripts covering R060–R062 and R064–R066. All scripts use the Nuke .nk stack-based node graph format confirmed in the research doc. The use_gpu knob name must match T01's output exactly.
  - Estimate: 45m
  - Files: test/output/.gitignore, test/test_opendefocus.nk, test/test_opendefocus_multidepth.nk, test/test_opendefocus_holdout.nk, test/test_opendefocus_camera.nk, test/test_opendefocus_cpu.nk, test/test_opendefocus_empty.nk, test/test_opendefocus_bokeh.nk
  - Verify: ls test/test_opendefocus.nk test/test_opendefocus_multidepth.nk test/test_opendefocus_holdout.nk test/test_opendefocus_camera.nk test/test_opendefocus_cpu.nk test/test_opendefocus_empty.nk test/test_opendefocus_bokeh.nk && grep 'test/output/' test/test_opendefocus.nk && cat test/output/.gitignore
- [x] **T03: Extended verify-s01-syntax.sh S02 section to loop over all 7 .nk test scripts; script exits 0 with all checks passing** — The current verify-s01-syntax.sh S02 section only checks for test/test_opendefocus.nk. This task extends it to check all 7 .nk files exist, then runs the script as the slice's objective stopping condition. All 7 files must exist from T02 before this task.
  - Estimate: 15m
  - Files: scripts/verify-s01-syntax.sh
  - Verify: sh scripts/verify-s01-syntax.sh
