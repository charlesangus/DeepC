# T02: Tidy output + optional B input + docker verification

**Slice:** S02
**Milestone:** M004-ks4br0

## Goal

Add the tidy output pass (sort + overlap clamp), wire the optional B input with depth-range gating, verify the flatten invariant analytically, and confirm full docker build.

## Must-Haves

### Truths
- Output samples are sorted by zFront ascending
- No overlap: zBack[i] <= zFront[i+1] for all i in output (final clamp pass enforces this)
- When B input is disconnected: all source samples are spread
- When B input is connected: only source samples with depth overlap [zFront-spread, zBack+spread] ∩ any B sample [zFront_B, zBack_B] are spread; others pass through unchanged
- `docker-build.sh --linux --versions 16.0` exits 0 with DeepCDepthBlur.so in release archive
- `g++ -fsyntax-only` passes

### Artifacts
- `src/DeepCDepthBlur.cpp` — complete implementation with tidy pass and B input

### Key Links
- `doDeepEngine` → B input pixel fetch via `in->deepEngine(box, channels, bPlane)` with null-check on optional B

## Steps

1. Add tidy pass at end of `doDeepEngine` per-pixel processing:
   - `std::sort` samples by zFront
   - Walk i = 0..n-2: if `samples[i].zBack > samples[i+1].zFront`, set `samples[i].zBack = samples[i+1].zFront`
   - (Alpha is not adjusted for the clamp — the clamp only trims the depth range, treating the trimmed portion as having already been accounted for by the adjacent sample)
2. Add optional B input:
   - Override `minimumInputs()` → 1, `maximumInputs()` → 2
   - Override `input_knobs` or check `input(1) != nullptr` at runtime in doDeepEngine
   - In doDeepEngine: if B connected, fetch B's DeepPlane for current box
   - For each source pixel: build list of B depth intervals `[zFront_B, zBack_B]` from B pixel
   - For each source sample: test whether [zFront - _spread, zBack + _spread] intersects any B interval; if not, push source sample unchanged and skip spread
3. Depth-interval intersection test (simple O(n_B) scan per source sample is fine — B sample counts are small):
   ```
   bool overlaps = false;
   for each B sample b:
       if (src.zBack + spread >= b.zFront && src.zFront - spread <= b.zBack):
           overlaps = true; break
   ```
4. Handle edge case: B pixel has no samples → treat as no overlap → all source samples pass through unchanged
5. Update `getDeepRequests` if needed to request B input's plane (check how DeepThinner or DeepCKeymix handles two inputs)
6. Run `bash scripts/verify-s01-syntax.sh` — exit 0 (update stubs if DeepCDepthBlur uses new DDImage types)
7. Run `bash docker-build.sh --linux --versions 16.0` — exit 0; confirm DeepCDepthBlur.so in archive

## Context

- For the B input in a `DeepFilterOp`, check `Op* b = input(1)` — it's null when disconnected. Use `dynamic_cast<DeepOp*>(b)` to get the deep interface. See `DeepCKeymix.cpp` for two-input deep pattern.
- `getDeepRequests` for the B input: request the same box from B (depth spreading doesn't change spatial footprint)
- The overlap clamp (step 1) can slightly change the visual result compared to the invariant — it is trimming the spread sample's Z extent, not its alpha. This is acceptable: the tidy step is a secondary correction after the invariant-preserving spread. Document this in a comment.
- If B input plane fetch fails (empty tile), treat gracefully as "no B samples" and pass all source samples unchanged
