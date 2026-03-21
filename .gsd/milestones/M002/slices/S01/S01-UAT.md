# S01: DeepCBlur core implementation — UAT

**Milestone:** M002
**Written:** 2026-03-20

## UAT Type

- UAT mode: mixed (artifact-driven for compilation + human-experience for visual output)
- Why this mode is sufficient: Compilation proves API correctness and linkage. Visual inspection in Nuke proves blur actually works on deep images — no automated deep-image comparison infrastructure exists.

## Preconditions

- Nuke 16+ installed and licensed on the test machine
- DeepCBlur.so (Linux) or DeepCBlur.dll (Windows) built via docker-build.sh and placed in the Nuke plugin path
- At least one deep EXR test image available (ideally: one sparse with a single object, one dense with overlapping objects at different depths)
- DeepC plugin path added to NUKE_PATH so menu.py loads

## Smoke Test

1. Launch Nuke 16+
2. Press Tab, type "DeepCBlur"
3. **Expected:** Node appears in autocomplete and can be created. If it doesn't appear, the Op::Description registration or menu integration failed.

## Test Cases

### 1. Node creates and shows correct knobs

1. Create a DeepCBlur node
2. Open the node properties panel
3. **Expected:** Five knobs visible: `blur_width` (float, default 1), `blur_height` (float, default 1), `max_samples` (int, default 100), `merge_tolerance` (float, default 0.001), `color_tolerance` (float, default 0.01)

### 2. Gaussian blur produces visible output on sparse deep image

1. Read a deep EXR with a single object (e.g., a sphere — samples only in center region, empty pixels around edges)
2. Connect DeepCBlur downstream
3. Set blur_width=5, blur_height=5
4. Connect a DeepToImage (flatten) node downstream
5. View the flattened output
6. **Expected:** The object edges appear soft/blurred. Color visibly spreads beyond the original object boundary into previously-empty pixels. Compare with the unblurred flattened output to confirm spreading.

### 3. Sample propagation creates samples in empty pixels

1. Using the same sparse deep image setup from test 2
2. Connect a DeepExpression or Python script to count samples per pixel before and after DeepCBlur
3. Check a pixel that was empty (0 samples) in the original but within blur radius of the object
4. **Expected:** That pixel now has >0 samples after DeepCBlur. This proves sample propagation (R003).

### 4. Non-uniform blur (anisotropic)

1. Read a deep EXR with a recognizable shape
2. Connect DeepCBlur with blur_width=10, blur_height=1
3. Flatten and view
4. **Expected:** Blur is visibly wider horizontally than vertically. Repeat with blur_width=1, blur_height=10 and confirm the opposite.

### 5. Per-pixel optimization reduces sample counts

1. Read a dense deep EXR (multiple overlapping objects at different depths)
2. Connect DeepCBlur with blur_width=3, blur_height=3, max_samples=10
3. Use DeepExpression or Python to check sample counts per pixel in the output
4. **Expected:** No pixel exceeds 10 samples. Compare with max_samples=1000 — higher cap should show more samples in dense regions.

### 6. Dense deep image composites correctly after blur

1. Read a deep EXR with two overlapping objects at different depths (e.g., foreground box, background sphere)
2. Connect DeepCBlur with blur_width=3, blur_height=3
3. Flatten with DeepToImage
4. **Expected:** Both objects remain visible at correct relative depths. Foreground still occludes background where expected. No black holes, NaN pixels, or compositing artifacts at object boundaries.

### 7. Zero blur is passthrough

1. Connect DeepCBlur with blur_width=0, blur_height=0
2. Compare output with input using DeepExpression or pixel probe
3. **Expected:** Output is identical to input — zero blur changes nothing.

### 8. Abort cancellation works

1. Connect DeepCBlur to a large deep image with blur_width=20, blur_height=20
2. Start rendering, then press Escape or click the cancel button in Nuke's progress bar
3. **Expected:** Rendering stops promptly. No hang, no crash. Node shows cancelled state.

## Edge Cases

### Large blur radius performance

1. Set blur_width=50, blur_height=50 on an HD deep image
2. **Expected:** Rendering completes (may be slow — minutes acceptable). No crash, no memory exhaustion. If max_samples is set low (e.g., 20), output sample counts stay bounded.

### Single-sample input

1. Create a deep image with exactly 1 sample per pixel (e.g., a flat card)
2. Apply DeepCBlur with blur_width=3
3. **Expected:** Output has blurred edges. No crash on single-sample pixels. Interior pixels unchanged in color.

### Empty input

1. Connect DeepCBlur to a DeepRead with no samples (fully transparent deep image)
2. **Expected:** Output is also empty. No crash, no error badge.

## Failure Signals

- Node doesn't appear in Tab menu → Op::Description registration failed or menu.py.in FILTER_NODES substitution broken
- Red error badge on DeepCBlur node → doDeepEngine() returned false, likely input fetch failure
- Black/zero output after flatten → kernel normalization bug (kernelSum incorrect) or samples not being emitted
- NaN or inf in output → division by zero in kernel weight calculation or merge math
- Memory spike or OOM → max_samples cap not working, or sample propagation creating unbounded samples
- Visible banding or stepping → kernel radius too small for sigma, or merge_tolerance too aggressive

## Not Proven By This UAT

- Cross-platform compilation (Windows DLL) — proven by docker-build.sh in S02
- Menu placement under DeepC > Filter — proven by S02's FILTER_NODES integration
- README accuracy (24 plugin count) — S02 deliverable
- Performance benchmarks — no formal timing targets defined; "reasonable time" is subjective

## Notes for Tester

- The merge_tolerance and color_tolerance knobs control how aggressively samples are merged. If blur output looks "stepped" or "posterized" in depth, try reducing merge_tolerance (e.g., 0.0001). If it looks too smooth/merged, increase it.
- Large blur radii are inherently slow (O(radius²) per sample). Blur sizes >10 on HD images will take significant time. This is expected for v1.
- The optimizer header (DeepSampleOptimizer.h) is shared infrastructure — if merge behavior seems wrong, the bug may be in the header, not in DeepCBlur.cpp itself.
