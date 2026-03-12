# Coding Conventions

**Analysis Date:** 2026-03-12

## Language & Standard

This is a C++ codebase (with a tiny Python footprint). The C++ standard is C++17 for Nuke 15+ and C++14 for earlier versions, controlled via CMake in `/workspace/CMakeLists.txt`. There is no auto-formatter or linter configured; style is maintained by hand.

## Naming Patterns

**Files:**
- One class per file, filename matches the class name exactly.
- Example: class `DeepCGrade` lives in `DeepCGrade.cpp` (and declares/defines itself entirely within that file — no separate `.h` except for base classes).
- Base/wrapper class headers are the exception: `DeepCWrapper.h`, `DeepCMWrapper.h`.

**Classes:**
- `PascalCase` prefixed with `DeepC`, e.g. `DeepCAdd`, `DeepCGrade`, `DeepCPNoise`.
- Base wrapper classes follow the same pattern: `DeepCWrapper`, `DeepCMWrapper`.

**Member variables:**
- Private/protected member variables use `_camelCase` with a leading underscore, e.g. `_processChannelSet`, `_doDeepMask`, `_invertSideMask`.
- Exception: some older or simpler members omit the underscore when their scope makes collisions unlikely (e.g. `float value[3]` in `DeepCAdd`, `bool invertMask` in `DeepCKeymix`). Prefer `_camelCase` for new members.
- Knob-backing arrays (plain float arrays for Color knobs) follow lowercase naming without underscore: `blackpoint[3]`, `whitepoint[3]`, `gamma[3]`.

**Local variables:**
- `camelCase` or short descriptive names: `deepInPixel`, `inPixelSamples`, `sideMaskVal`, `gradedPerSampleData`.
- `cIndex` is the conventional name for `colourIndex(z)` results.
- `z` is the conventional loop variable for iterating channels via `foreach(z, channelset)`.
- Conventional loop iterators `i`, `j`, `it`, `sampleNo` are acceptable.

**Functions / Methods:**
- Public virtual methods: `lowerCamelCase`, e.g. `findNeededDeepChannels`, `wrappedPerSample`, `wrappedPerChannel`, `doDeepEngine`.
- Nuke SDK override names are kept verbatim: `_validate`, `knobs`, `knob_changed`, `node_help`, `Class`, etc.
- Static factory function is always named `build`: `static Op* build(Node* node)`.

**Enums:**
- Enum values in `UPPER_SNAKE_CASE`, e.g. `REPLACE`, `UNION`, `MASK`, `SMOOTH`, `LINEAR`.
- Enums are anonymous (`enum { ... }`) or named only when referenced by type.
- Numeric suffix `_OP` used to avoid collisions with reserved words: `MIN_OP`, `MAX_OP`, `_OUT`.

**Static string arrays (for enumeration knobs):**
- `static const char* const thingNames[] = { "name1", "name2", 0 };` — always null-terminated with `0`.
- Array name ends in `Names`, parallel enum follows immediately after.

**Constants:**
- `static const char*` for `CLASS`, `HELP`, knob tooltips.
- `static const char* const` for null-terminated string arrays.

## Code Style

**Formatting:**
- No auto-formatter is configured.
- Braces: opening brace on the same line for `class`, `if`, `for`, `switch` in `DeepCKeymix`-style newer files. Older files (DeepCWrapper, DeepCGrade) place opening braces on the next line. Both styles coexist — no single enforced rule.
- Indentation: 4 spaces. Tabs are not used.
- Function parameter lists use one parameter per line with the closing `)` on its own line for longer signatures.

**Linting:**
- Not configured. No `.clang-format`, `.clang-tidy`, or equivalent found.

## Import Organization

**Order (observed pattern):**
1. Local project headers (`"DeepCWrapper.h"`, `"DeepCMWrapper.h"`, `"FastNoise.h"`)
2. Nuke DDImage headers (`"DDImage/DeepFilterOp.h"`, `"DDImage/Knobs.h"`, etc.)
3. Standard library C++ headers (`<string>`, `<stdio.h>`, `<math.h>`, `<iostream>`)

**Namespace:**
- `using namespace DD::Image;` is placed at file scope after includes in every plugin file.
- `using namespace std;` appears only in `DeepCMWrapper.cpp`; generally avoided elsewhere.

## Class Structure Pattern

All plugins follow a consistent single-file structure:

```cpp
// 1. Includes
#include "DeepCWrapper.h"

using namespace DD::Image;

// 2. Optional: file-scope static constants, enums, string arrays
static const char* const operationNames[] = { "replace", ..., 0 };
enum { REPLACE, UNION, ... };

// 3. Class declaration (with inline short methods, constructor init list)
class DeepCFoo : public DeepCWrapper
{
    // member variables (knob-backing data first, then flags, then computed)
    float value[3];
    bool _reverse;

    public:
        DeepCFoo(Node* node) : DeepCWrapper(node) { ... }

        virtual void wrappedPerChannel(...);
        virtual void custom_knobs(Knob_Callback f);

        static const Iop::Description d;
        const char* Class() const { return d.name; }
        virtual Op* op() { return this; }
        const char* node_help() const;
};

// 4. Method implementations (in declaration order)
void DeepCFoo::wrappedPerChannel(...) { ... }
void DeepCFoo::custom_knobs(...) { ... }
const char* DeepCFoo::node_help() const { return "..."; }

// 5. Registration (always at end of file)
static Op* build(Node* node) { return new DeepCFoo(node); }
const Op::Description DeepCFoo::d("DeepCFoo", 0, build);
```

## Inheritance Hierarchy

Three tiers — choose the right base class:

| Base Class | Location | Use When |
|---|---|---|
| `DD::Image::DeepFilterOp` | Nuke SDK | Plugin needs full manual control (e.g. `DeepCKeymix`, `DeepCAddChannels`) |
| `DeepCWrapper` | `src/DeepCWrapper.h` + `.cpp` | Per-channel colour processing with masking/mix support |
| `DeepCMWrapper` | `src/DeepCMWrapper.h` + `.cpp` | Matte generation from auxiliary channel data (extends `DeepCWrapper`) |

Subclasses of `DeepCWrapper` override:
- `custom_knobs(Knob_Callback f)` — add plugin-specific knobs
- `wrappedPerChannel(...)` — implement per-channel pixel math
- `wrappedPerSample(...)` — optionally compute per-sample auxiliary data
- `_validate(bool)` — optionally add pre-computation; must call parent first

## Error Handling

**Strategy:** Return-value based, no exceptions. The Nuke SDK uses `bool` return codes on engine calls.

**Patterns:**
- Early-return `return true` (empty success) when input is null: `if (!input0()) return true;`
- Early-return `return false` on downstream engine failure: `if (!input0()->deepEngine(...)) return false;`
- Bail on user-interrupt: `if (Op::aborted()) return false;`
- `mFnAssert(inPlaceOutPlane.isComplete())` as a post-condition assertion before returning.
- No `throw` or `try/catch` anywhere in the codebase.
- Null-guard via `dynamic_cast` before use: `_maskOp = dynamic_cast<Iop*>(Op::input(1)); if (_maskOp != NULL && ...)`

## Comments

**When to comment:**
- Block comments above non-obvious method implementations describing purpose and any constraints on callers (e.g., "Subclasses should call the parent implementation before their own.").
- Inline comments on non-obvious logic, especially masking and premultiplication math.
- TODO comments are used actively to flag incomplete or fragile areas (see `CONCERNS.md`).
- `// values knobs write into go here` style section headers within class bodies.

**Comment style:**
- `/* block */` for multi-sentence method-level explanations.
- `//` inline for single-line notes.
- No Doxygen/JavaDoc-style documentation.

## Knob Registration Pattern

Every plugin registers exactly one static `Op::Description`:

```cpp
static Op* build(Node* node) { return new DeepCFoo(node); }
const Op::Description DeepCFoo::d("DeepCFoo", 0, build);
```

The string `"DeepCFoo"` must match the class name. The `Class()` method returns `d.name`.

Knob layout is split into three virtual methods in `DeepCWrapper` subclasses:
- `top_knobs(f)` — channel selector and unpremult toggle
- `custom_knobs(f)` — plugin-specific controls
- `bottom_knobs(f)` — mask inputs, mix, and dividers

Direct `DeepFilterOp` subclasses implement `knobs(f)` directly.

## Python Conventions

The Python footprint is minimal:
- `python/init.py` — single-line plugin path registration.
- `python/menu.py.in` — CMake template; node lists injected by build system.
- Python files use standard `import nuke` and Nuke Python API calls. No style tooling configured.

---

*Convention analysis: 2026-03-12*
