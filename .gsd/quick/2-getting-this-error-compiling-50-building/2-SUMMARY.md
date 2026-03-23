# Quick Task: DeepCDefocusPO not compiling — three SDK errors

**Date:** 2026-03-23
**Branch:** gsd/quick/2-getting-this-error-compiling-50-building

## What Changed

Three compile errors fixed, all in `src/DeepCDefocusPO.cpp`:

1. **Removed spurious `Op* op() override`** — `Op` has no virtual `Op* op()` method;
   `Iop` provides `iop()` instead. The `override` keyword caused a hard error.
   No replacement needed — the method was never called.

2. **Fixed `*di.full_size_format()` → `di.full_size_format()`** in `_validate` —
   `Info2D::full_size_format()` (inherited by `DeepInfo`) returns `const Format&`,
   not a pointer. Dereferencing a reference is a type error. Removed the `*`.

3. **Fixed `*info_.full_size_format()` → `info_.full_size_format()`** in
   `renderStripe` — same root cause: `IopInfoOwner::full_size_format()` getter
   returns `const Format&`. Removed the `*`.

Also fixed `scripts/verify-s01-syntax.sh`: the mock `Info::full_size_format()`
and `DeepInfo::full_size_format()` stubs were returning `const Format*` instead
of `const Format&`, masking both pointer-dereference bugs during local syntax
checks. Updated both to return `const Format&` so the mock matches the real SDK.

## Files Modified

- `src/DeepCDefocusPO.cpp` — 3 lines changed
- `scripts/verify-s01-syntax.sh` — 2 mock stub return types corrected

## Verification

- `cmake --build /tmp/nuke-build --target DeepCDefocusPO` → exits 0,
  `DeepCDefocusPO.so` produced; no warnings on the changed lines
- `bash scripts/verify-s01-syntax.sh` → exits 0, all S02–S05 contracts pass
- All three original compiler errors are gone:
  - `error: 'op()' marked 'override', but does not override` ✅ fixed
  - `error: no match for 'operator*' (operand type is 'const Format')` in `_validate` ✅ fixed
  - `error: no match for 'operator*' (operand type is 'const Format')` in `renderStripe` ✅ fixed
