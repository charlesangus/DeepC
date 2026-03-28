---
estimated_steps: 12
estimated_files: 1
skills_used: []
---

# T02: Wire CameraOp input to override lens knob values before FFI call

Wire the CameraOp input (input 2) to override the lens knob values before the LensParams struct is constructed for the FFI call.

Following the existing opendefocus-nuke pattern:
1. In renderStripe (or _validate), retrieve `CameraOp* cam = dynamic_cast<CameraOp*>(Op::input(2))`.
2. If cam is not null: call `cam->validate()` then extract:
   - focal_length = cam->focal_length() (mm)
   - fstop = cam->fStop()
   - focus_distance = cam->projection_distance()
   - sensor_size_mm = cam->film_width() * 10.0f (filmback cm -> mm)
3. If cam is null: use the manual knob values (already in place).
4. Add a fprintf(stderr, ...) INFO log: 'DeepCOpenDefocus: camera link %s' (active/inactive).
5. Add brief tooltip comments to the Float_knob declarations noting units and camera override behavior.
6. Run verify script.

## Inputs

- `src/DeepCOpenDefocus.cpp`
- `.gsd/milestones/M009-mso5fb/M009-mso5fb-RESEARCH.md`
- `crates/opendefocus-nuke/src/opendefocus.cpp`

## Expected Output

- `Updated src/DeepCOpenDefocus.cpp with camera link extraction`

## Verification

scripts/verify-s01-syntax.sh passes. Grep confirms cam->focal_length() and cam->fStop() usage in DeepCOpenDefocus.cpp.
