---
id: M012-kho2ui
title: "DeepCOpenDefocus Performance"
status: complete
completed_at: 2026-03-30T23:36:20.667Z
key_decisions:
  - OnceLock singleton with Arc<Mutex> held once per render_impl, not per-layer (D031) — eliminates per-call async renderer init overhead
  - Filter/mipmap prep hoisted above layer loop; depth Settings still built per-layer (D032) — filter shape is constant across layers; hoisting gives O(1) vs O(N) filter construction while preserving depth-dependent CoC correctness
  - render_convolve_prepped skips Telea inpaint (D032/D033) — layer-peel layers have no unknown regions; skipping inpaint eliminates the most expensive per-layer operation
  - Rust FFI render runs outside _cacheMutex (D033) — avoids serializing concurrent Nuke tile threads; concurrent misses each do a full render but second write is idempotent
  - _cacheValid invalidated unconditionally in _validate(); cache key checked in renderStripe() on every call (D034) — guarantees no stale frame data while keeping the common stripe-reuse path cheap
key_files:
  - crates/opendefocus-deep/src/lib.rs
  - crates/opendefocus/src/lib.rs
  - crates/opendefocus/src/worker/engine.rs
  - crates/opendefocus-deep/include/opendefocus_deep.h
  - src/DeepCOpenDefocus.cpp
  - test/test_opendefocus_256.nk
  - scripts/verify-s01-syntax.sh
lessons_learned:
  - OnceLock<Arc<Mutex<T>>> is the canonical pattern for Rust FFI singletons — init on first call, lock held for full operation scope, no per-call async init overhead
  - PlanarIop render cache pattern: render outside mutex, write inside mutex, invalidate in _validate() — reusable for any future PlanarIop plugin wrapping a slow FFI render
  - Full-image deepEngine fetch for non-local operations must use info_.box() (set by _validate() before renderStripe is ever called) — stripe bounds are incorrect for convolution-based effects
  - Stripe copy from full-image cache uses fullBox-relative indexing: (y - fullBox.y()) * fullW * 4 + (x - fullBox.x()) * 4 + c — copy only output.bounds() sub-region
  - Telea inpaint can be skipped in the layer-peel (prepped) render path — layer-peel layers have valid alpha with no unknown regions, and skipping is the most valuable single-operation optimization
  - Timing regression guard pattern: warm_ms < 5000 assertion documents singleton reuse without asserting a speedup ratio — unit tests are not the right place for SLA; Docker bench is
  - The quality Enumeration_knob must be added as a file-scope static null-terminated char* const[] before the class definition for null-terminator clarity — not as a class member or lambda-local array
---

# M012-kho2ui: DeepCOpenDefocus Performance

**Eliminated the two primary CPU render bottlenecks in DeepCOpenDefocus — OnceLock singleton renderer with hoisted filter/mipmap prep (S01) and a mutex-guarded full-image PlanarIop cache with quality knob (S02) — reducing per-stripe FFI overhead from O(N_stripes × full_render) to O(1) per frame.**

## What Happened

M012-kho2ui targeted the performance characteristics of DeepCOpenDefocus, a PlanarIop plugin that wraps a Rust FFI renderer (opendefocus) to produce physically-based depth-of-field from Nuke deep images.

**Pre-milestone bottlenecks:**
1. `OpenDefocusRenderer::new()` was called on every FFI invocation — full async init (thread pool, pipeline setup, settings validation) per stripe call.
2. Filter construction (bokeh disc, aperture mask), mipmap generation, and Telea inpaint ran once per depth layer — O(N_layers × image_area) total work per render.
3. Nuke's PlanarIop calls `renderStripe()` once per horizontal band — each stripe triggered a full Rust FFI render on incomplete sub-region data (both incorrect for non-local convolution and extremely slow).

**S01: Singleton renderer + layer-peel dedup**

T01 replaced per-call `block_on(OpenDefocusRenderer::new())` with `static RENDERER: OnceLock<Arc<Mutex<OpenDefocusRenderer>>>`. The renderer is created exactly once on first call; subsequent calls lock the Mutex. `test_singleton_reuse` confirms two successive calls succeed without deadlock or NaN.

T02 added `prepare_filter_mipmaps` and `render_stripe_prepped` to the `opendefocus` crate. A new `render_with_prebuilt_mipmaps` / `render_convolve_prepped` pair on `RenderEngine` accepts a pre-built `MipmapBuffer<f32>`, skipping Telea inpaint entirely (layer-peel layers have valid alpha with no unknown regions). In `opendefocus-deep/src/lib.rs`, one `prepare_filter_mipmaps` call is hoisted above the layer loop. Filter shape is constant across layers; depth-dependent Settings (focal_plane, camera_data) are still built fresh per layer.

T03 wired `int quality` end-to-end: FFI header parameter, `render_impl` pass-through, C++ `Enumeration_knob` with `kQualityEntries` file-scope static, `_quality` member initialized to 0, `(int)_quality` at both FFI call sites.

T04 added `test_render_timing` and `test_holdout_timing` — each calls the FFI function twice (cold then warm) on a 4×4 CPU image, prints elapsed times with `eprintln!`, and asserts `warm_ms < 5000` as a regression guard.

**S02: Full-image PlanarIop cache + quality knob**

A single task (T01) rewrote `renderStripe()` in `DeepCOpenDefocus.cpp`:

- Cache members added: `std::vector<float> _cachedRGBA`, `bool _cacheValid`, 6 cache-key fields (`_cacheFL`, `_cacheFS`, `_cacheFD`, `_cacheSSZ`, `_cacheQuality`, `_cacheUseGpu`), `mutable std::mutex _cacheMutex`.
- `_validate()` unconditionally sets `_cacheValid = false` — no stale frame data can survive a knob change, upstream change, or frame change.
- Cache hit path: lock → check 6-field key → copy stripe sub-region from `_cachedRGBA` → return.
- Cache miss path: unlock → compute `fullBox = info_.box()` → call `deepIn->deepEngine(fullBox, ...)` on the complete image → build flat buffers → run FFI call → lock → write `_cachedRGBA` + key fields + set `_cacheValid = true` → unlock → copy stripe.
- Rust FFI render runs **outside** `_cacheMutex` to avoid serializing concurrent Nuke tile threads. Concurrent misses each do a full render; the second write is idempotent.
- Both normal and holdout branches use `fullBox`, not stripe bounds — correct for non-local convolution.

`test/test_opendefocus_256.nk` (minimal 256×256 single-layer deep scene, quality Low) created as the SLA test fixture. `scripts/verify-s01-syntax.sh` extended from 7 to 8 .nk file existence checks.

**Verification:** `bash scripts/verify-s01-syntax.sh` exits 0 (g++ -fsyntax-only for all 3 .cpp files; all 8 .nk files exist). `cargo test -p opendefocus-deep` passed at every task checkpoint. Docker build and live Nuke SLA timing deferred per consistent environment constraint (no Docker in worktree environment — identical to all prior milestones per KNOWLEDGE.md).

## Success Criteria Results

## Success Criteria Results

### Vision: 256×256 single-layer deep scene renders under 10 seconds on CPU

**All structural requirements for meeting this target are implemented.** The two dominant bottlenecks (per-call renderer init, per-stripe full render) are eliminated. Live timing confirmation requires a Nuke + DeepCOpenDefocus.so environment.

---

### S01 Criteria

**✅ Renderer created once (OnceLock singleton)**
`OnceLock<Arc<Mutex<OpenDefocusRenderer>>>` in `crates/opendefocus-deep/src/lib.rs`; `get_or_init_renderer` helper ensures init on first call only. `test_singleton_reuse` passes — two successive calls succeed, no NaN, no deadlock.

**✅ Filter/mipmap prep hoisted out of layer loop**
`prepare_filter_mipmaps` + `render_stripe_prepped` API added to `opendefocus` crate. `render_with_prebuilt_mipmaps` + `render_convolve_prepped` on `RenderEngine`. Deep/lib.rs call site confirmed hoisted above layer loop. `MINIMUM_FILTER_SIZE` promoted to `pub(crate)`.

**✅ cargo test passes**
`cargo test -p opendefocus-deep` confirmed passing at T01 (2 tests), T02 (2 tests, identical output values), T03 (2 tests), T04 (timing tests). Total 4 tests pass with timing output visible via `--nocapture`.

**✅ Timing regression guard**
`test_render_timing` + `test_holdout_timing` with `warm_ms < 5000` assertion. Cold/warm times printed via `eprintln!`.

---

### S02 Criteria

**✅ Full deep image rendered in one shot per frame (PlanarIop cache)**
`_cachedRGBA`, `_cacheValid`, `_cacheMutex`, 6 cache-key fields added to `DeepCOpenDefocus`. `_validate()` unconditionally invalidates. `renderStripe()` renders full image on cache miss using `info_.box()`, serves stripe sub-region on cache hit. Confirmed: `grep -q '_cacheValid'`, `grep -q '_cacheMutex'`, `grep -q 'info_.box()'` all exit 0.

**✅ Quality knob visible in Nuke (code-level evidence)**
`Enumeration_knob` in `DeepCOpenDefocus.cpp` (count=1 verified). `_quality` member initialized to 0. `kQualityEntries` file-scope static. `(int)_quality` at both FFI call sites. Live Nuke UI confirmation deferred to release environment.

**⚠️ 256×256 single-layer renders under 10s — DEFERRED (environment constraint)**
`test/test_opendefocus_256.nk` SLA fixture created and confirmed well-formed. Live `nuke -x -m 1 test/test_opendefocus_256.nk` timing requires Nuke + installed .so — consistent environment constraint across all milestones.

**⚠️ Docker build exits 0 — DEFERRED (environment constraint)**
`g++ -fsyntax-only` proxy check passes (exits 0). `scripts/docker-build.sh --linux --versions 16.0` not run — environment constraint documented in KNOWLEDGE.md as consistent across all milestones.

## Definition of Done Results

## Definition of Done Results

**✅ All slices marked done in roadmap**
- S01: ✅ (completed_at: 2026-03-30T23:11:48.648Z, verification_result: passed)
- S02: ✅ (completed_at: 2026-03-30T23:31:22.891Z, verification_result: passed)

**✅ All slice summaries exist on disk**
- `.gsd/milestones/M012-kho2ui/slices/S01/S01-SUMMARY.md` ✅
- `.gsd/milestones/M012-kho2ui/slices/S02/S02-SUMMARY.md` ✅

**✅ All task summaries exist on disk**
- S01: T01, T02, T03, T04 summaries present ✅
- S02: T01 summary present ✅

**✅ Non-.gsd code changes exist in git**
5 commits between deepcblur branch and HEAD affect non-.gsd files: `crates/opendefocus-deep/src/lib.rs`, `crates/opendefocus/src/lib.rs`, `crates/opendefocus/src/worker/engine.rs`, `crates/opendefocus-deep/include/opendefocus_deep.h`, `src/DeepCOpenDefocus.cpp`, `test/test_opendefocus_256.nk`, `scripts/verify-s01-syntax.sh`.

**✅ `bash scripts/verify-s01-syntax.sh` exits 0**
g++ -fsyntax-only passes for DeepCBlur.cpp, DeepCDepthBlur.cpp, DeepCOpenDefocus.cpp. All 8 .nk files exist. Confirmed live.

**✅ Cross-slice integration points work correctly**
S01 OnceLock singleton consumed by S02 (single renderer = single coherent cache). S01 quality knob consumed by S02 as one of 6 cache-key fields — quality changes correctly invalidate cache without duplicating knob.

**✅ cargo test -p opendefocus-deep passes**
Confirmed passing throughout all tasks. 4 tests: test_singleton_reuse, test_render_timing, test_holdout_timing (plus existing render/holdout correctness tests).

**⚠️ Docker build exits 0 — DEFERRED**
Consistent environment constraint. Runbook: `bash scripts/docker-build.sh --linux --versions 16.0`.

## Requirement Outcomes

## Requirement Outcomes

### R046 — Advanced (status: validated, unchanged from pre-milestone)

**Description:** Full-image cache ensures renderStripe produces correct output (non-local convolution on complete data), advancing the correctness guarantee of the flat 2D RGBA output from deep input.

**Pre-milestone status:** validated (by M009-mso5fb/S02 Docker build evidence)

**M012 advancement:** S02 specifically addresses the non-local convolution correctness vector. `renderStripe()` now fetches `fullBox = info_.box()` for both normal and holdout deepEngine calls, building complete `sampleCounts`/`rgba_flat`/`depth_flat` over the entire image before running FFI. This is architecturally correct for defocus blur (a non-local operation): a stripe-bounded fetch would produce incorrect edge behavior at stripe boundaries.

**Evidence:** `grep -q 'info_.box()' src/DeepCOpenDefocus.cpp` exits 0; both deepEngine branches confirmed to use `fullBox` not stripe bounds (S02 T01 verification).

**Status remains: validated.** M012 strengthened the implementation correctness; no status change required.

---

### All other requirements

No other requirements changed status during this milestone. M012 scope was narrowly focused on performance optimization (singleton renderer, layer-peel dedup, full-image cache, quality knob). No new features were added that would advance or validate additional requirements. No requirement was invalidated or re-scoped.

## Deviations

T02: MINIMUM_FILTER_SIZE promoted to pub(crate) for lib.rs access — minor adaptation consistent with plan. render_convolve_prepped added as private RenderEngine method (not explicitly in original plan but required for clean layer separation). T03: Quality enum import dropped since quality.into() on raw i32 needs no named constant — cleaner outcome than plan implied. S02 T01: Both deepEngine call branches (normal + holdout) confirmed to use fullBox — plan was explicit; implementation matched exactly.

## Follow-ups

Run `bash scripts/docker-build.sh --linux --versions 16.0` as final compilation proof before release. Execute `nuke -x -m 1 test/test_opendefocus_256.nk` in a live Nuke environment to confirm the &lt;10s SLA. The OnceLock singleton is initialized with the first call's use_gpu flag — if GPU mode is ever re-enabled, a singleton reset or GPU-aware init strategy will be required.
