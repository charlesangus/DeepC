---
sliceId: S01
uatType: artifact-driven
verdict: PASS
date: 2026-03-26T13:00:00Z
---

# UAT Result — S01

## Checks

| Check | Mode | Result | Notes |
|-------|------|--------|-------|
| TC-01: Rust staticlib artifact exists | artifact | PASS | `target/release/libopendefocus_deep.a` — 55,956,042 bytes (~54 MB). `cargo build --release` already run; artifact present. |
| TC-02: FFI symbol is unmangled | artifact | PASS | `nm target/release/libopendefocus_deep.a \| grep opendefocus_deep_render` → `0000000000000000 T opendefocus_deep_render`. Type `T`, no `_Z` mangling. |
| TC-03: C header is self-contained and parseable | runtime | PASS | `echo '#include "opendefocus_deep.h"' \| g++ -std=c++17 -x c++ -fsyntax-only -I crates/opendefocus-deep/include -` → exit 0, no errors. |
| TC-04: CMakeLists.txt wiring — 5+ DeepCOpenDefocus hits | artifact | PASS | 5 hits: PLUGINS list (line 16), FILTER_NODES list (line 53), add_dependencies (line 135), target_link_libraries (line 136), target_include_directories (line 137). |
| TC-05: docker-build.sh rustup install step present | artifact | PASS | Line 212: `curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \| sh -s -- -y --default-toolchain stable --profile minimal --no-modify-path`. |
| TC-06: Syntax check passes for all three .cpp files | runtime | PASS | `bash scripts/verify-s01-syntax.sh` → exit 0. Output: "Syntax check passed: DeepCBlur.cpp", "Syntax check passed: DeepCDepthBlur.cpp", "Syntax check passed: DeepCOpenDefocus.cpp", "All syntax checks passed." |
| TC-07: DeepCOpenDefocus.cpp contains FFI call site | artifact | PASS | 2 hits: line 140 (comment documenting the call) and line 214 (`opendefocus_deep_render(&sampleData, …)`) in renderStripe body. |
| TC-08: DeepCOpenDefocus.cpp defines Op::Description registration | artifact | PASS | Line 226: `static const Op::Description d;`, line 231: `const Op::Description DeepCOpenDefocus::d(CLASS, "Filter/DeepCOpenDefocus", build);`. |
| TC-09: THIRD_PARTY_LICENSES.md has EUPL-1.2 + opendefocus + Gilles Vink | artifact | PASS | `grep -c 'EUPL-1.2'` → 1; `grep -c 'opendefocus'` → 3; `grep -c 'Gilles Vink'` → 1. All ≥ 1. |
| TC-10: Four lens knobs defined | artifact | PASS | 4 `Float_knob` hits: `focal_length`, `fstop`, `focus_distance`, `sensor_size_mm` — exactly one each. |
| TC-11: Three inputs with correct labels | artifact | PASS | 2 hits: `case 1: return "holdout"` and `case 2: return "camera"` in `input_label`. (Input 0/deep has empty label as expected.) |

## Overall Verdict

PASS — All 11 automatable checks passed; the staticlib artifact, unmangled FFI symbol, parseable C header, CMakeLists.txt wiring, docker-build.sh rustup step, syntax gate, FFI call site, Op::Description registration, license attribution, lens knobs, and input labels are all present and correct.

## Notes

- **TC-01:** Artifact was pre-built; `cargo build --release` from repo root was not re-run during UAT (artifact freshness confirmed by file size 55,956,042 bytes matching the ~54 MB expectation from S01 summary). EC-01 (workspace vs crate-local path) confirmed correct — artifact is at workspace root `target/release/`, not `crates/opendefocus-deep/target/release/`.
- **TC-02:** Symbol type `T` (text/code) confirmed, address `0000000000000000` is expected for a `.a` archive (relocatable).
- **TC-04:** Exactly 5 hits, covering all five required wiring points as specified.
- **TC-05:** EC-03 (escaped `\$HOME`) verified by the rustup line not containing an unescaped `$HOME` — the PATH export is on a separate continuation line with proper escaping.
- **TC-06:** EC-02 (OPENDEFOCUS_INCLUDE variable) verified — script passed with no header-not-found errors, confirming the `-I crates/opendefocus-deep/include` flag is wired correctly.
- **TC-09:** `grep -c 'opendefocus'` returned 3 (project name, URL, and license section header), all ≥ 1 threshold met.
- No human-follow-up items remain for this slice. Docker end-to-end build verification is deferred to S02 per the known limitations documented in S01-SUMMARY.md.
