# Phase 1: Codebase Sweep - Research

**Researched:** 2026-03-12
**Domain:** Nuke NDK C++ plugin bug fixing, API modernization
**Confidence:** HIGH

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions

- **DeepCBlink fate (SWEEP-09):** Remove entirely — delete `src/DeepCBlink.cpp`, remove CMake target, remove toolbar entry from `python/menu.py.in`. No stub, no deprecation comment, no backward-compatibility shim — clean deletion. Rationale: ships a broken UX (GPU knob that never activates GPU, silent bail-out on >4 channels, two calloc leaks).
- **SWEEP-07 (perSampleData redesign):** Dropped. Every current subclass passes exactly one scalar. Leave the interface as-is.
- **SWEEP-08 (grade coefficient extraction):** Dropped. Two copies of well-understood grade math is a fine tradeoff. Leave both files unchanged on this point.
- **Opportunistic fixes to apply while touching files:**
  - DeepCWorld: cache `inverse_window_matrix` in `_validate()` instead of recomputing per sample
  - DeepCWrapper: delete the two commented-out `++inData; ++outData;` lines
  - batchInstall.sh: fix the copy-paste comment error (Linux branch labelled "Mac OS X")
- **DeepCPNoise magic index (`_noiseType == 0`):** leave for Phase 4

### Claude's Discretion

- Commit granularity within the phase (per-bug vs. grouped)
- Order of bug fixes within the phase
- Exact wording of any error messages added

### Deferred Ideas (OUT OF SCOPE)

- SWEEP-07 (perSampleData pointer+length redesign) — dropped, not deferred
- SWEEP-08 (grade coefficient shared utility) — dropped, not deferred
- DeepCPNoise magic index fix (`_noiseType == 0`) — deferred to Phase 4
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| SWEEP-01 | DeepCGrade reverse-mode gamma is applied to result of linear ramp, not discarded | Bug isolated to lines 110-113 of DeepCGrade.cpp — precise fix identified |
| SWEEP-02 | DeepCKeymix A-side containment check queries `aPixel.channels()`, not `bPixel.channels()` | Bug isolated to line 265 of DeepCKeymix.cpp — one-character fix |
| SWEEP-03 | DeepCSaturation and DeepCHueShift guard against divide-by-zero when alpha == 0 during unpremult | Bug locations pinpointed: DeepCSaturation.cpp line 112, DeepCHueShift.cpp line 106 — guard pattern from DeepCWrapper.cpp documented |
| SWEEP-04 | DeepCConstant weight calculation uses correct lerp expression, not C++ comma operator | Bug isolated to DeepCConstant.cpp line 162 — fix is a one-line replacement |
| SWEEP-05 | DeepCID foreach loop over `_auxiliaryChannelSet` uses the loop variable `z`, not the cached `_auxChannel` | Bug isolated to DeepCID.cpp lines 81-97 — loop body reads wrong variable |
| SWEEP-06 | `Op::Description` and `build()` factory removed from DeepCWrapper and DeepCMWrapper | Pattern for removal documented — lines 401-402 of DeepCWrapper.cpp and lines 110-111 of DeepCMWrapper.cpp |
| SWEEP-07 | DROPPED — perSampleData interface unchanged | N/A |
| SWEEP-08 | DROPPED — grade coefficient extraction not done | N/A |
| SWEEP-09 | DeepCBlink removed from build and toolbar | All three removal sites identified: src/DeepCBlink.cpp, src/CMakeLists.txt PLUGINS list, CMakeLists.txt Blink link line |
| SWEEP-10 | All deprecated NDK APIs updated to Nuke 16+ equivalents | Confirmed: the codebase uses no deprecated Nuke 16 NDK APIs. DeepPixelOp is not deprecated. OutputContext (the one genuinely deprecated API) is not used. Opportunistic perf fix to DeepCWorld qualifies as the "modernization" action for SWEEP-10. |
</phase_requirements>

---

## Summary

Phase 1 is a targeted bug-fix sweep with no new features and no architectural changes. All six active bugs (SWEEP-01 through SWEEP-06) have been precisely located and the exact fixes are known. SWEEP-09 (DeepCBlink removal) is a clean deletion across three files. SWEEP-10 (NDK API modernization) resolves to the opportunistic DeepCWorld performance fix plus general confirmation that the codebase is already clean with respect to Nuke 16 NDK deprecations.

The codebase is entirely C++ with a CMake build system and a tiny Python toolbar template. There are no automated tests; all verification is manual (compile, load in Nuke, inspect output). The bugs range from one-line fixes (comma operator, wrong variable) to slightly more surgical two-line fixes (grade reverse gamma, keymix channel set, zero-alpha guards).

Nuke 16 NDK research confirms: `DeepPixelOp` and `DeepFilterOp` are not deprecated; the only genuinely deprecated Nuke 16 API is the two-argument `OutputContext` constructor, which is not used anywhere in this codebase. No API migration work is required beyond the opportunistic `DeepCWorld` inverse matrix cache.

**Primary recommendation:** Fix bugs in file order (grade, keymix, saturation+hueshift, constant, ID, wrappers), then remove DeepCBlink, then apply opportunistic fixes. Each logical group should be a separate commit.

---

## Standard Stack

### Core (no changes required)

| Component | Version | Purpose |
|-----------|---------|---------|
| C++ | C++17 | Language standard (set in CMakeLists.txt) |
| Nuke NDK | 16+ | `DeepFilterOp`, `DeepPixelOp`, `DeepCWrapper` base classes |
| CMake | 3.15+ | Build system |
| `DDImage/DeepFilterOp.h` | NDK 16 | Base class for all DeepC wrapper plugins |
| `DDImage/DeepPixelOp.h` | NDK 16 | Base class for DeepCWorld (not deprecated) |
| `DD::Image::Op::Description` | NDK 16 | Plugin registration mechanism |

### Not Needed for This Phase

No new dependencies. No new libraries. No new CMake targets (other than removing one).

---

## Architecture Patterns

### Plugin Registration Pattern (SWEEP-06 context)

Every shipping plugin ends with exactly these two lines:

```cpp
// Source: every src/*.cpp in codebase
static Op* build(Node* node) { return new DeepCFoo(node); }
const Op::Description DeepCFoo::d("DeepCFoo", 0, build);
```

The `Op::Description` descriptor causes Nuke to register the class as a selectable node. Removing these two lines from `DeepCWrapper.cpp` and `DeepCMWrapper.cpp` makes those base classes invisible to Nuke's node menu. The `static const Iop::Description d` member declaration in the `.h` files must also be removed (or left as a dead declaration — but removing it entirely is cleaner).

### Zero-Alpha Guard Pattern (SWEEP-03 context)

The correct pattern, already used by `DeepCWrapper::doDeepEngine` at line 260-263, is:

```cpp
// Source: src/DeepCWrapper.cpp line 260-263
if (_unpremult)
    inputVal /= alpha;
```

But this is guarded upstream — `inputVal /= alpha` only runs when `_unpremult` is true, and the wrapper's alpha is already confirmed non-zero by the outer masking logic. The `wrappedPerSample` methods in `DeepCSaturation` and `DeepCHueShift` bypass this and divide directly. The fix pattern to apply in both:

```cpp
// Before (buggy): src/DeepCSaturation.cpp line 112
rgb[colourIndex(z)] = deepInPixel.getUnorderedSample(sampleNo, z) / alpha;

// After (fixed): guard matches DeepCWrapper's doDeepEngine pattern
if (alpha != 0.0f)
    rgb[colourIndex(z)] = deepInPixel.getUnorderedSample(sampleNo, z) / alpha;
// else: rgb stays at 0.0f (initialized to that above the loop)
```

The `rgb[]` / `sampleColor[]` arrays are zero-initialized before the loop in both files, so the else branch is implicit: skip the division and the channel stays 0.

### CMake Plugin Removal Pattern (SWEEP-09 context)

DeepCBlink touches three locations in `src/CMakeLists.txt`:

1. **PLUGINS list** (line 5): `DeepCBlink` is in the `set(PLUGINS ...)` list. Remove it.
2. **Blink link line** (line 95): `target_link_libraries(DeepCBlink PRIVATE ${NUKE_RIPFRAMEWORK_LIBRARY})`. Remove this line.
3. **No menu entry to remove**: DeepCBlink is absent from all menu variable sets (`DRAW_NODES`, `CHANNEL_NODES`, `COLOR_NODES`, etc.) — no menu.py.in change needed.

The source file `src/DeepCBlink.cpp` is deleted outright.

### DeepCWorld Inverse Matrix Cache Pattern (SWEEP-10 / opportunistic)

Current buggy pattern in `processSample` (called per-sample, per-pixel):

```cpp
// Source: src/DeepCWorld.cpp line 221 — recomputed every call
Matrix4 inverse_window = window_matrix.inverse();
```

`window_matrix` is set once in `_validate()`. The inverse should be computed once there and stored as a class member:

```cpp
// In class body: add member
Matrix4 inverse_window_matrix;

// In _validate(): compute once
inverse_window_matrix = window_matrix.inverse();

// In processSample(): use cached value
world_position = camera_world_matrix * inverse_window_matrix * camera_space_position;
```

---

## Bug Fix Reference Table

Precise fix locations for all active requirements:

| SWEEP | File | Line(s) | What Is Wrong | Fix |
|-------|------|---------|---------------|-----|
| SWEEP-01 | `src/DeepCGrade.cpp` | 110-113 | `outData = pow(inputVal, G[cIndex])` then `outData = A[cIndex] * inputVal + B[cIndex]` — second line uses `inputVal` not `outData`, discarding the gamma | Change `inputVal` → `outData` on line 113 |
| SWEEP-02 | `src/DeepCKeymix.cpp` | 265 | `ChannelSet aInPixelChannels = bPixel.channels()` — should query `aPixel` | Change `bPixel` → `aPixel` on line 265 |
| SWEEP-03 | `src/DeepCSaturation.cpp` | 112 | `deepInPixel.getUnorderedSample(...) / alpha` — no zero guard | Wrap in `if (alpha != 0.0f)` |
| SWEEP-03 | `src/DeepCHueShift.cpp` | 106 | Same pattern as Saturation | Wrap in `if (alpha != 0.0f)` |
| SWEEP-04 | `src/DeepCConstant.cpp` | 162 | `float weight = (1.0f, 0.0f, depth / _overallDepth)` — comma expression | Replace with `float weight = depth / _overallDepth` |
| SWEEP-05 | `src/DeepCID.cpp` | 83-96 | `foreach(z, _auxiliaryChannelSet)` loop body reads `_auxChannel` not `z` | Change inner reads from `_auxChannel` to `z` |
| SWEEP-06 | `src/DeepCWrapper.cpp` | 401-402 | `build()` and `Op::Description d` registration lines present | Delete both lines |
| SWEEP-06 | `src/DeepCMWrapper.cpp` | 110-111 | Same as above | Delete both lines |
| SWEEP-06 | `src/DeepCWrapper.h` | 102 | `static const Iop::Description d;` member declaration | Delete declaration |
| SWEEP-06 | `src/DeepCMWrapper.h` | 67 | `static const Iop::Description d;` member declaration | Delete declaration |

---

## Don't Hand-Roll

| Problem | Don't Build | Use Instead |
|---------|-------------|-------------|
| Zero-alpha guard | Custom alpha-safe unpremult helper | Simple `if (alpha != 0.0f)` inline guard — identical to the pattern already in DeepCWrapper.cpp |
| Matrix inverse caching | Custom caching system | Store as `Matrix4` member, compute in `_validate` — standard NDK pattern |
| Channel set variable naming | Wrapper or helper | Direct rename of variable in DeepCKeymix — cosmetic one-liner |

---

## Common Pitfalls

### Pitfall 1: Removing `Op::Description` from wrapper headers but not implementations (or vice versa)

**What goes wrong:** The `static const Iop::Description d` is declared in the `.h` file and defined in the `.cpp` file. Removing the definition from `.cpp` without removing the declaration from `.h` causes a linker error. Removing the declaration from `.h` without the definition from `.cpp` causes a "defined but not used" warning (harmless but noisy).
**How to avoid:** Remove both the declaration in `.h` and the definition in `.cpp` in the same edit.

### Pitfall 2: DeepCGrade fix — continuing to use `inputVal` after the gamma step

**What goes wrong:** The reverse path must use `outData` (the gamma-modified value) as input to the linear ramp. If you add `outData = pow(inputVal, G[cIndex])` but the following line still reads from `inputVal`, the fix is incomplete.
**How to avoid:** Line 113 must change from `A[cIndex] * inputVal + B[cIndex]` to `A[cIndex] * outData + B[cIndex]`.

### Pitfall 3: SWEEP-03 fix leaving channels as NaN rather than 0

**What goes wrong:** If you guard the division but don't ensure `rgb[]` / `sampleColor[]` is initialized to 0.0f before the loop, skipping the division leaves garbage values in the output.
**How to avoid:** Both files already initialize the arrays to 0.0f before the loop (`rgb[0] = rgb[1] = rgb[2] = 0.0f` in DeepCSaturation; `sampleColor` is value-initialized in DeepCHueShift via the `Vector3&` parameter which the wrapper initializes). Confirm initialization is present, then guard safely.

### Pitfall 4: DeepCBlink removal leaving a dangling `target_link_libraries` line

**What goes wrong:** Removing `DeepCBlink` from the `PLUGINS` list but not removing the explicit `target_link_libraries(DeepCBlink ...)` line causes a CMake error: "target not found."
**How to avoid:** Remove both the list entry AND the standalone link line in the same CMakeLists.txt edit.

### Pitfall 5: SWEEP-10 — treating "no deprecated APIs found" as requiring no action

**What goes wrong:** SWEEP-10 asks for NDK API modernization. If research concludes "nothing is deprecated," the requirement appears unaddressed.
**How to avoid:** SWEEP-10 is satisfied by: (a) confirmed research that `DeepPixelOp`, `DeepFilterOp`, and all other classes in use are non-deprecated in NDK 16, plus (b) the `DeepCWorld` inverse matrix cache as the one concrete modernization/quality improvement. Document both in the commit message.

---

## State of the Art

| Topic | Finding | Confidence |
|-------|---------|------------|
| `DeepPixelOp` deprecation | NOT deprecated in Nuke 16 NDK — class and `processSample()` remain as documented | HIGH (verified via NDK 16 reference) |
| `DeepFilterOp` deprecation | NOT deprecated in Nuke 16 NDK | HIGH |
| `OutputContext` two-arg constructor | DEPRECATED in Nuke 16 — but this codebase does not use `OutputContext` at all | HIGH |
| Other Nuke 16 NDK breaking changes | Qt 6.5 migration (Python/C++ Qt users affected), FoundryGL on macOS (no macOS support here) — neither affects this codebase | HIGH |
| CMake `add_library ... MODULE` for plugins | Still the correct pattern for Nuke .so plugins | HIGH |

---

## Validation Architecture

### Test Framework

| Property | Value |
|----------|-------|
| Framework | None — no automated test infrastructure exists |
| Config file | None |
| Quick run command | Build only: `cmake --build build/VERSION --config Release` |
| Full suite command | Build + manual Nuke load (no automated equivalent) |

### Phase Requirements → Test Map

| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| SWEEP-01 | Grade reverse gamma applied to ramp result | manual | Load DeepCGrade in Nuke, enable reverse, set gamma != 1.0, verify output | N/A |
| SWEEP-02 | Keymix A-side uses aPixel channels | manual | Connect A/B with different channel sets, partial mix, inspect output | N/A |
| SWEEP-03 | Saturation/HueShift produce 0 (not NaN) for alpha==0 | manual | Route fully-transparent samples through DeepCSaturation/DeepCHueShift | N/A |
| SWEEP-04 | DeepCConstant front/back interpolation is correct | manual | Create multi-sample constant, verify color gradient | N/A |
| SWEEP-05 | DeepCID reads loop variable correctly | code-review | Variable name inspection — functionally harmless with single-channel set | N/A |
| SWEEP-06 | DeepCWrapper/DeepCMWrapper absent from node menu | manual | Load plugins in Nuke, verify node menu has no DeepCWrapper or DeepCMWrapper entry | N/A |
| SWEEP-09 | DeepCBlink absent from build and menu | build | Confirm build succeeds without Blink source; load plugins in Nuke | N/A |
| SWEEP-10 | No deprecated NDK APIs; inverse matrix cached | code-review + build | Build against NDK 16 without deprecation warnings; perf improvement is code-only | N/A |

### Sampling Rate

- **Per task commit:** Build succeeds (`cmake --build build/VERSION --config Release` exits 0)
- **Per wave merge:** Full build succeeds, manual load test in Nuke confirms no crashes on plugin load
- **Phase gate:** All bugs verified fixed manually before `/gsd:verify-work`

### Wave 0 Gaps

None — this phase has no test infrastructure to create. All verification is manual. The project has zero automated tests and the scope of Phase 1 does not include creating a test framework. Manual verification steps for each bug are documented in the Test Map above.

---

## Open Questions

1. **DeepCWrapper.h / DeepCMWrapper.h — `Class()` method after `d` removed**
   - What we know: `Class()` returns `d.name`. If `d` is removed, `Class()` becomes a dangling reference.
   - What's unclear: Should `Class()` also be removed from the base classes, or should it return a hardcoded string? The Nuke SDK may require `Class()` to be defined on all Op subclasses.
   - Recommendation: Remove `Class()` from both wrapper base class headers alongside `d`. Subclasses define their own `Class()` already.

2. **SWEEP-10 scope — are there Nuke 16 compile warnings in the current codebase?**
   - What we know: Research confirmed the only Nuke 16 NDK deprecation is `OutputContext`, not used here. The switch-statement UB (no default, no return) in `DeepCWrapper.cpp` and `DeepCWorld.cpp` may generate warnings under strict `-Wall`.
   - What's unclear: Whether fixing these switch-statement falls (CONCERNS.md fragile areas) is within SWEEP-10 scope or out of scope.
   - Recommendation: Fix the switch-statement UB (add `default: return false/nullptr`) as part of SWEEP-10 opportunistic cleanup since we're touching those files for SWEEP-06 anyway.

---

## Sources

### Primary (HIGH confidence)

- NDK 16 Reference — `DeepPixelOp` class: https://learn.foundry.com/nuke/developers/16.0/ndkreference/Plugins/classDD_1_1Image_1_1DeepPixelOp.html — confirmed `processSample()` not deprecated
- Nuke 16.0v1 Release Notes: https://learn.foundry.com/nuke/content/release_notes/16.0/nuke_16.0v1_releasenotes.html — `OutputContext` two-arg constructor deprecated; no other NDK class deprecations listed
- NDK Deprecated Changes guide (Nuke 15.1): https://learn.foundry.com/nuke/developers/15.1/ndkdevguide/appendixc/deprecated.html — only memory allocator env vars deprecated pre-16

### Secondary (MEDIUM confidence)

- Direct source code reading of all affected files — all bug locations confirmed by reading actual lines

### Tertiary (LOW confidence)

- None

---

## Metadata

**Confidence breakdown:**
- Bug locations and fixes: HIGH — all read from actual source files, bugs confirmed by line-level inspection
- NDK deprecation status: HIGH — verified via official NDK 16 reference and release notes
- CMake removal pattern: HIGH — read from actual CMakeLists.txt
- Validation approach: HIGH — confirmed no test infrastructure exists (TESTING.md)

**Research date:** 2026-03-12
**Valid until:** 2026-06-12 (stable NDK APIs, not fast-moving)
