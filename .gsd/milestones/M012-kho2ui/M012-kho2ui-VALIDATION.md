---
verdict: needs-attention
remediation_round: 0
---

# Milestone Validation: M012-kho2ui

## Success Criteria Checklist

## Success Criteria Checklist

The roadmap vision: "Eliminate the CPU render bottlenecks in DeepCOpenDefocus — singleton renderer, hoisted per-layer prep, full-image PlanarIop cache, and quality knob — so that a 256×256 single-layer deep scene renders in under 10 seconds on CPU."

### S01 Demo Claim: "Same 256×256 test renders significantly faster — renderer created once, filter/mipmap/inpaint prep hoisted out of layer loop. cargo test passes. Timed comparison before/after."

- [x] **Renderer created once (OnceLock singleton)** — `OnceLock<Arc<Mutex<OpenDefocusRenderer>>>` in `crates/opendefocus-deep/src/lib.rs`; `get_or_init_renderer` helper; `test_singleton_reuse` test passes. ✅
- [x] **Filter/mipmap prep hoisted out of layer loop** — `prepare_filter_mipmaps` + `render_stripe_prepped` API added to `opendefocus` crate; `render_with_prebuilt_mipmaps` + `render_convolve_prepped` on `RenderEngine`; call site in deep/lib.rs hoists above layer loop. ✅
- [x] **cargo test passes** — `cargo test -p opendefocus-deep` confirmed passing throughout all T01–T04 tasks; 4 tests pass with timing output. ✅
- [x] **Timed comparison (regression guard)** — `test_render_timing` and `test_holdout_timing` added with `warm_ms < 5000` assertion; cold/warm times printed via `eprintln!`. ✅

### S02 Demo Claim: "DeepCOpenDefocus renders full deep image in one shot. Quality knob visible in Nuke. 256×256 single-layer renders under 10s. Docker build exits 0."

- [x] **Renders full image in one shot (PlanarIop cache)** — `_cachedRGBA`, `_cacheValid`, `_cacheMutex`, 6 cache-key fields added; `_validate()` unconditionally invalidates; `renderStripe()` rewrites render full image on miss, serves stripe on hit; `info_.box()` used for both deepEngine branches. ✅
- [x] **Quality knob visible in Nuke** — `Enumeration_knob` + `_quality` member in `DeepCOpenDefocus.cpp` (from S01); exactly 1 occurrence verified; `(int)_quality` passed at both FFI call sites. ✅ (code present, live Nuke UI confirmation not available in environment)
- [ ] **256×256 single-layer renders under 10s** — `test/test_opendefocus_256.nk` fixture created; but live `nuke -x -m 1 test/test_opendefocus_256.nk` timing not run (no Nuke environment). ⚠️ DEFERRED
- [ ] **Docker build exits 0** — `scripts/docker-build.sh --linux --versions 16.0` not run; explicitly acknowledged as environment constraint consistent with prior milestones. ⚠️ DEFERRED


## Slice Delivery Audit

## Slice Delivery Audit

| Slice | Claimed Deliverable | Substantiated? | Notes |
|-------|---------------------|---------------|-------|
| S01 | OnceLock singleton renderer — init once, lock per render_impl | ✅ Yes | `OnceLock` + `get_or_init_renderer` confirmed in lib.rs; test_singleton_reuse passes |
| S01 | prepare_filter_mipmaps / render_stripe_prepped hoisted above layer loop | ✅ Yes | API added to opendefocus crate; render_with_prebuilt_mipmaps on RenderEngine; deep/lib.rs call site updated |
| S01 | quality: i32 FFI parameter + Enumeration_knob | ✅ Yes | FFI header updated (≥2 quality occurrences); Enumeration_knob in DeepCOpenDefocus.cpp (count=1); _quality member; both FFI call sites pass (int)_quality |
| S01 | Timing test harness with warm_ms < 5000 regression guard | ✅ Yes | test_render_timing + test_holdout_timing present and grepped; warm_ms < 5000 assertion confirmed |
| S01 | verify-s01-syntax.sh exits 0 (3 .cpp files + 7 .nk files) | ✅ Yes | Script passes; all syntax checks pass; 7 .nk artifacts confirmed |
| S02 | Full-image PlanarIop render cache (mutex, _cachedRGBA, 6 cache-key fields) | ✅ Yes | All fields grepped in DeepCOpenDefocus.cpp; _cacheValid, _cacheMutex, _cachedRGBA, _cacheFL all confirmed |
| S02 | _validate() invalidates cache unconditionally | ✅ Yes | `_cacheValid = false` in _validate() confirmed by grep |
| S02 | fullBox used for both deepEngine calls (correct non-local convolution) | ✅ Yes | `info_.box()` + `deepEngine(fullBox` count ≥ 2 confirmed |
| S02 | Cache-key-tracked quality invalidation | ✅ Yes | `_cacheQuality` among 6 key fields; cache checked on every stripe call |
| S02 | test/test_opendefocus_256.nk SLA test fixture | ✅ Yes | File exists; contains `256 256` and `first_frame 1`; well-formed |
| S02 | verify-s01-syntax.sh updated to 8 .nk files | ✅ Yes | `test_opendefocus_256.nk` in script's file-existence loop confirmed |
| S02 | Docker build exits 0 / DeepCOpenDefocus.so produced | ⚠️ Not run | Environment constraint; g++ -fsyntax-only passes as proxy |
| S02 | nuke -x 256×256 renders non-black under 10s | ⚠️ Not run | No live Nuke environment; test fixture exists for when environment available |


## Cross-Slice Integration

## Cross-Slice Integration

### S01 → S02 boundary

**S01 provides to S02:**
1. `OnceLock singleton renderer` — prerequisite for PlanarIop cache correctness (single renderer instance = single coherent cache). S02 consumed this: the cache is per-renderer-instance and the singleton ensures one renderer per process. ✅
2. `prepare_filter_mipmaps / render_stripe_prepped API` — S02 did not need to use this directly (the cache call still goes through deepEngine → FFI), but the API is available. ✅
3. `quality: i32 FFI parameter + Enumeration_knob` — S02 consumed this: quality is one of the 6 cache-key fields (`_cacheQuality`), ensuring quality changes invalidate the cache without duplicating the knob. ✅
4. `Timing test harness` — S02 did not add new timing tests (single-task slice); existing tests serve as baseline. ✅

**No mismatches found.** S02's summary explicitly confirms quality was already wired from S01, and the cache correctly key-tracks it. The singleton from S01 is the structural prerequisite for S02's correctness guarantee (one cache per renderer instance) and was consumed as designed.

### Boundary Map Alignment

- S01 `affects: [S02]` → S02 `requires: []` (empty, but the S02 summary text confirms the dependency was consumed via pre-existing code from S01). Minor documentation discrepancy: S02 listed `requires: []` but textually describes consuming S01's singleton and quality knob. This is cosmetic, not a functional gap.


## Requirement Coverage

## Requirement Coverage

### R046 (advanced this milestone)
**Description:** Full-image cache ensures renderStripe produces correct output (non-local convolution on complete data), advancing the correctness guarantee of the flat 2D RGBA output from deep input.

**Evidence:** S02 explicitly addresses this requirement. `renderStripe()` was rewritten to:
- Fetch `fullBox = info_.box()` (complete image bounds, set by `_validate()`) for both normal and holdout deepEngine calls
- Build `sampleCounts`/`rgba_flat`/`depth_flat` over the full image
- Run FFI render on the complete data
- Cache result and serve stripe sub-regions from cache

This directly satisfies the correctness requirement that non-local convolution (defocus blur) receives complete image data, not stripe-bounded partial data. ✅

### Other active requirements

The milestone scope (performance optimization, not new features) primarily addresses R046. No other requirements were listed as advanced, validated, or invalidated by this milestone per the pipeline context. The requirement coverage is appropriate for the milestone's scope.


## Verification Class Compliance

## Verification Classes Compliance

### Contract ✅ PASS
- `cargo test -p opendefocus-deep` — confirmed passing throughout S01 tasks (T01: 2 tests, T02: 2 tests, T03: 2 tests, T04: tests pass); 4 total tests pass with timing output.
- `cargo check -p opendefocus-deep` — implied clean (cargo test subsumes cargo check; no compilation errors reported across any task).
- `bash scripts/verify-s01-syntax.sh` — confirmed exits 0 in both S01 and S02; covers DeepCBlur.cpp, DeepCDepthBlur.cpp, DeepCOpenDefocus.cpp + 8 .nk file existence checks.

### Integration ⚠️ DEFERRED (environment constraint)
- `bash docker-build.sh --linux --versions 16.0` — NOT run; explicitly acknowledged in both S01 and S02 summaries as environment constraint consistent with prior milestones. KNOWLEDGE.md states this is "the only reliable compilation proof."
- `DeepCOpenDefocus.so produced` — no evidence (depends on Docker build above).
- `nuke -x test/test_opendefocus.nk renders non-black output` — NOT run; no live Nuke environment available.

The g++ `-fsyntax-only` check (which passes) is the best available proxy for structural correctness in this environment. All structural changes (cache members, mutex, renderStripe rewrite) are syntactically valid C++17.

### Operational ⚠️ DEFERRED (environment constraint)
- `nuke -x 256×256 single-layer CPU render completes under 10 seconds (-m 1)` — NOT run. `test/test_opendefocus_256.nk` fixture exists and is well-formed. The SLA target requires a live Nuke environment with `DeepCOpenDefocus.so` installed. Deferred to release environment per S02 follow-ups.

### UAT ⚠️ PARTIALLY VERIFIED
- **Quality knob visible in Nuke UI** — Code-level evidence only: `Enumeration_knob` present (count=1), `_quality` member initialized to 0. Live Nuke UI confirmation not available.
- **Changing quality affects render time** — Code-level evidence: quality is one of the 6 cache-key fields; different quality values trigger cache miss and full re-render (different FFI quality parameter → different algorithm path). Live timing comparison not available.
- **Multi-layer test produces correct depth-separated defocus** — `test_holdout_attenuates_background_samples` passes with correct z=2/z=5 attenuation values (T≈0.97 and T≈0.099); multi-layer correctness confirmed at unit test level. Full-stack live Nuke test not available.

### Summary
Contract verification is fully satisfied. Integration, Operational, and full UAT verification are deferred due to consistent environment constraint (no Docker build, no live Nuke) that applies across all milestones in this project per KNOWLEDGE.md.



## Verdict Rationale
All code-level deliverables for both slices are fully substantiated with grep evidence, cargo test passage, and C++ syntax checks. The implementation is structurally complete: OnceLock singleton, layer-peel dedup, full-image PlanarIop cache with mutex and 6-field cache key, quality Enumeration_knob wired end-to-end, and 256×256 SLA test fixture. The only gaps are Docker build verification and live Nuke operational testing — both explicitly deferred as consistent environment constraints across all milestones in this project (per KNOWLEDGE.md: "docker-build.sh is the only reliable compilation proof"). These deferred items do not indicate implementation gaps; they are environment-gated validation steps documented with clear runbook commands for the release environment. No remediation slices are needed.
