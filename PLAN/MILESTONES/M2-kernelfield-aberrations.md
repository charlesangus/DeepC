# Milestone 2: DeepCKernelField + aberrations

Adds physically-styled bokeh aberrations (blade notching, cat's-eye vignetting, spherical
aberration, astigmatism, coma, chromatic radius scaling, near-field kernel flip) via a separate
`DeepCKernelField` generator node consumed by `DeepCDefocus` through an optional third input.
This is the seam M1 deliberately left in place — `KernelSampler`/`KernelView` already exist and
`destX/destY/depth/channelGroup` are already real (if unused) parameters on the v1 disc kernel.
**The scatter loop itself does not change in this milestone** — only the kernel it samples from.

Depends on Milestone 1 being `done`: this milestone's `KernelFieldSampler` implements the same
`KernelSampler` interface M1's `DiscKernelLUT` implements (see
`PLAN/MILESTONES/M1-deepcdefocus-v1.md` "Kernel seam for v2" for the interface shape and the
rationale for choosing a typed, resolution-independent interface over a tiled-image interchange).

## Phase 2.1: Kernel field interface and generator node

- [ ] M2.P1.T1 — `DeepCKernelFieldOp` abstract interface
  - files: `src/DeepCKernelFieldOp.h` (new)
  - approach: define the abstract interface
    `{ virtual KernelView kernel(radius, u, v, depth, channelGroup); virtual Hash kernelHash(); }`.
    Move `KernelView`'s definition here from `src/DeepCDefocusKernel.h` (M1) if that creates a
    cleaner dependency direction — `DeepCDefocusKernel.h` should depend on this header, not vice
    versa, so M1's `DiscKernelLUT` stays usable standalone when no kernel field is connected.
  - verify: `./docker-build.sh --linux` compiles (header-only, included but not yet consumed).
  - size: S

- [ ] M2.P1.T2 — `DeepCKernelField` generator node
  - files: `src/DeepCKernelField.cpp` (new)
  - approach: `Iop` generator node whose 2D output renders the kernel grid for visual
    inspection. Knobs: blade count/curvature (barn-door notching), cat's-eye vignetting
    strength/falloff, spherical-aberration ring bias, astigmatism ellipse ratio+angle, coma
    skew, per-channel chromatic radius scale, and a near-field kernel flip (rays cross at focus,
    so kernel orientation inverts for the near field — precedent: opendefocus's
    `INVERSE_FOREGROUND_BOKEH_SHAPE`). Implements `DeepCKernelFieldOp`.
  - verify: `./docker-build.sh --linux` compiles; load in Nuke, sweep each aberration knob, and
    visually confirm the rendered kernel grid changes as expected (blade notches appear/rotate,
    vignetting increases toward frame edges, near-field flip inverts orientation when depth
    crosses focus).
  - size: L

## Phase 2.2: Wire into DeepCDefocus

- [ ] M2.P2.T1 — Optional kernel-field input and adapter
  - files: `src/DeepCDefocus.cpp`/`.h`
  - approach: add optional input 2 (`dynamic_cast<DeepCKernelFieldOp*>`, `default_input` →
    `nullptr` when unconnected — falls back to M1's `DiscKernelLUT`). Implement
    `KernelFieldSampler : KernelSampler` adapting `DeepCKernelFieldOp::kernel()` calls, with an
    LRU cache keyed by `(quantized radius, grid cell, channel group)` since per-fragment calls
    into the field would be too slow uncached. Wire `channelRadiusScale[]` (M1's dormant hook)
    through to the field's per-channel chromatic radius scale. Add the field's `kernelHash()`
    into `Op::hash()` for cache invalidation. The scatter loop (`scatterBandCPU`) itself is
    untouched — only the `KernelSampler` implementation it calls changes.
  - verify: with no kernel field connected, re-run M1's validation scenes (a)–(l) from
    `tests/nuke/` unchanged — must still pass bit-for-bit (proves the adapter path is truly
    opt-in and doesn't perturb the M1 default). With a kernel field connected and an aberration
    knob swept, confirm the defocused output visibly changes to match (e.g. bladed highlights,
    edge vignetting) on a simple point-light test scene.
  - size: M

## Notes carried from the v1 review (not committed scope for this milestone)

Two v2-shaped knobs were identified during M1's design review but deliberately deferred, since
they're independent of the kernel-field work above and may warrant their own milestone or may
fold into M2 if they turn out cheap once the kernel field exists:
- Per-sample highlight pre-gain (compensates clipped/HDR sources — M1 works strictly in
  scene-linear premultiplied light and documents the resulting duller-than-photographic bokeh
  on clipped sources).
- Disocclusion fill / alpha-renormalize toggle (M1 specifies coverage-deficit alpha as honest
  by default; see M1's "Coverage deficit" design note for why that's the v1 default).

## Decisions

(none yet)

**Verification gate:** `./docker-build.sh --linux` green; `DeepCKernelField`'s rendered kernel
grid responds correctly to every aberration knob; `DeepCDefocus` with a kernel field connected
produces visibly aberrated bokeh; M1's `tests/nuke/` validation scenes (a)–(l) still pass with
no kernel field connected (regression proof that the scatter core is unchanged). PR merges via
`/address-pr-review --auto`.
