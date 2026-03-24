# Quick Task: CMake parse error at src/CMakeLists.txt:153

**Date:** 2026-03-24
**Branch:** gsd/quick/8-getting-a-build-error-building-deepc-for

## What Changed
- Removed a stray duplicate ` .)` line at line 153 of `src/CMakeLists.txt` — a merge artifact from the M007-gvtoom merge that left two closing parens for the `install(TARGETS ...)` block.

## Files Modified
- `src/CMakeLists.txt` — deleted 1 line

## Verification
- No remaining stray `.)` lines in the file (`grep` confirms)
- File structure ends cleanly with the single `install(TARGETS ... DESTINATION .)` block
