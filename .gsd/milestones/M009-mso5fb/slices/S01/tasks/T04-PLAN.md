---
estimated_steps: 21
estimated_files: 2
skills_used: []
---

# T04: Extend verify script with PlanarIop/CameraOp/DeepOp mock headers and add THIRD_PARTY_LICENSES.md attribution

Extend `scripts/verify-s01-syntax.sh` so it syntax-checks `src/DeepCOpenDefocus.cpp` correctly, and add the opendefocus EUPL-1.2 attribution to `THIRD_PARTY_LICENSES.md`. This satisfies R057 (opendefocus fork with EUPL-1.2 attribution).

Verify script changes:
1. Add mock `DDImage/PlanarIop.h` in the TMPDIR creation block — must declare `PlanarIop` class inheriting from `Op`, with `virtual void _validate(bool)`, `virtual void renderStripe(ImagePlane&) = 0`, `virtual int minimum_inputs() const`, `virtual int maximum_inputs() const`. Also declare `ImagePlane` class with `bounds()` returning `Box`, `makeWritable()`, `writable()` returning `float*` (or suitable stub), `channels()` returning `ChannelSet`, `stride()` returning `int`, and `nChans()` returning `int`. Declare `Descriptions` struct if referenced.
2. Add mock `DDImage/CameraOp.h` — declare `CameraOp` inheriting from `Op` with stubs for `focalLength()`, `fStop()`, `focusDistance()`, `horizontalAperture()`, `verticalAperture()` returning float 0.
3. Add mock `DDImage/DeepOp.h` (or extend existing `DeepFilterOp.h`) — ensure `DeepOp` is declared as a standalone class (not just nested in DeepFilterOp) so `dynamic_cast<DeepOp*>` compiles against Op base.
4. Add mock `DDImage/Row.h` if referenced — minimal stub.
5. Update the g++ syntax-check invocation to include `-I${OPENDEFOCUS_INCLUDE}` where `OPENDEFOCUS_INCLUDE` points at `${SCRIPT_DIR}/../crates/opendefocus-deep/include` (so `opendefocus_deep.h` resolves).
6. Add `DeepCOpenDefocus.cpp` to the loop (or add it as an explicit extra check after the loop).
7. Run `bash scripts/verify-s01-syntax.sh` — must exit 0.

THIRD_PARTY_LICENSES.md changes:
- Add a new section for opendefocus following the existing FastNoise/DeepThinner pattern:
  - Project: opendefocus
  - Author: Gilles Vink
  - Source: https://codeberg.org/gillesvink/opendefocus
  - License: EUPL-1.2
  - Full EUPL-1.2 license text (fetch from https://joinup.ec.europa.eu/collection/eupl/eupl-text-eupl-12 or include verbatim — the EUPL-1.2 text is ~2500 words, include it in full as the existing MIT entries do)

Verify:
```bash
bash scripts/verify-s01-syntax.sh
grep -q 'opendefocus' THIRD_PARTY_LICENSES.md && grep -q 'EUPL-1.2' THIRD_PARTY_LICENSES.md && echo OK
```

## Inputs

- `scripts/verify-s01-syntax.sh`
- `THIRD_PARTY_LICENSES.md`
- `src/DeepCOpenDefocus.cpp`
- `crates/opendefocus-deep/include/opendefocus_deep.h`

## Expected Output

- `scripts/verify-s01-syntax.sh`
- `THIRD_PARTY_LICENSES.md`

## Verification

bash scripts/verify-s01-syntax.sh && grep -q 'EUPL-1.2' THIRD_PARTY_LICENSES.md && echo 'All checks passed'
