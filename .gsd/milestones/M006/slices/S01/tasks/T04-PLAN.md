---
estimated_steps: 3
estimated_files: 3
skills_used: []
---

# T04: Run syntax verifier and docker build

**Slice:** S01 — Iop Scaffold, Poly Loader, and Architecture Decision
**Milestone:** M006

## Description

Run the verification gates to confirm the S01 implementation is correct end-to-end:

1. **Syntax verifier** (`scripts/verify-s01-syntax.sh`) — fast gate, no Docker required. Catches C++ compilation errors in `DeepCDefocusPO.cpp`, `poly.h`, and `deepc_po_math.h` before attempting the full build.

2. **Docker build** (`docker-build.sh --linux --versions 16.0`) — confirms the plugin builds successfully against the real Nuke 16.0 SDK and produces `DeepCDefocusPO.so` in the release zip.

If either gate fails, diagnose the error, fix the relevant source file from T01–T03, and re-run. The most common failure modes are: missing mock stub for a DDImage type, ODR violation from poly.h included in multiple TUs, incorrect `File_knob` signature, or missing `DeepOp.h` include.

## Steps

1. Run `bash scripts/verify-s01-syntax.sh` from the repo root. If it fails:
   - Read the g++ error output carefully to identify which type/function is missing from the mock headers
   - Fix the relevant heredoc in `verify-s01-syntax.sh` or fix a bug in `DeepCDefocusPO.cpp`
   - Re-run until it exits 0

2. Run the grep contract suite to confirm key patterns are present:
   ```bash
   grep -q 'poly_system_read'    src/DeepCDefocusPO.cpp
   grep -q 'poly_system_destroy' src/DeepCDefocusPO.cpp
   grep -q 'File_knob'           src/DeepCDefocusPO.cpp
   grep -q 'renderStripe'        src/DeepCDefocusPO.cpp
   grep -q 'lt_newton_trace'     src/deepc_po_math.h
   grep -q 'mat2_inverse'        src/deepc_po_math.h
   grep -q 'coc_radius'          src/deepc_po_math.h
   grep -q 'poly_system_read'    src/poly.h
   grep -c 'DeepCDefocusPO' src/CMakeLists.txt   # should be ≥2
   ```

3. Run `bash docker-build.sh --linux --versions 16.0`. Confirm exit 0. Confirm the artifact:
   ```bash
   unzip -l release/DeepC-Linux-Nuke16.0.zip | grep DeepCDefocusPO.so
   ```

## Must-Haves

- [ ] `bash scripts/verify-s01-syntax.sh` exits 0
- [ ] All grep contracts from step 2 pass (no failures)
- [ ] `docker-build.sh --linux --versions 16.0` exits 0
- [ ] `DeepCDefocusPO.so` present in `release/DeepC-Linux-Nuke16.0.zip`

## Verification

```bash
bash scripts/verify-s01-syntax.sh && echo "SYNTAX PASS"
grep -q 'poly_system_read' src/DeepCDefocusPO.cpp && \
grep -q 'poly_system_destroy' src/DeepCDefocusPO.cpp && \
grep -q 'renderStripe' src/DeepCDefocusPO.cpp && \
grep -q 'lt_newton_trace' src/deepc_po_math.h && \
grep -q 'coc_radius' src/deepc_po_math.h && \
echo "GREP CONTRACTS PASS"
bash docker-build.sh --linux --versions 16.0 && \
unzip -l release/DeepC-Linux-Nuke16.0.zip | grep -q 'DeepCDefocusPO.so' && \
echo "DOCKER BUILD PASS"
```

## Inputs

- `src/DeepCDefocusPO.cpp` — plugin source (produced by T02)
- `src/poly.h` — poly API header (produced by T01)
- `src/deepc_po_math.h` — math stubs header (produced by T01)
- `src/CMakeLists.txt` — with DeepCDefocusPO entries (modified by T03)
- `scripts/verify-s01-syntax.sh` — with new mock stubs and file in loop (modified by T03)

## Expected Output

- `src/DeepCDefocusPO.cpp` — possibly patched to fix any syntax issues found during this task
- `src/poly.h` — possibly patched to fix any compilation issues
- `src/deepc_po_math.h` — possibly patched to fix any compilation issues
- `scripts/verify-s01-syntax.sh` — possibly patched to fix missing stubs
