# Milestone 7: Solid alpha on opaque geometry — the fix

Makes `DeepCDefocus` read alpha **exactly `1.0`** (within a term-count ulp bound) wherever opaque
geometry covers the output pixel — on a slanted opaque plane with small opaque objects in front of
it, around the objects, across the plane, and where object discs overlap — matching Bokeh on the
identical deep scene. M6.P2.T2 ruled the fix **local**: today `saturateBucketPlanes()` pulls a
pooled bucket's alpha to 1 *before* the composite, and `compositePixelCoveragePartition()` then
splits that clamped 1 over new area `C_k` and co-located area `D_k`, so the fit term's per-unit
opacity falls below 1 and the pixel under-adds (arrival is > 1 at every dip, so the deficit-only
fill never engages). M7 folds saturation into the composite so the raw `A_k` is still in hand at
the split: when `colo > 0` and `A_raw > 1`, `u = min(A_raw/(C_raw+D_raw), 1)`, `aCov = u·C`,
`aRes = u·D`, colour scaled by `1/A_raw` exactly as saturation did. No new plane, no memory, no
knob, not a fill option — a correctness change to the default behaviour, like M4. Scene (o)'s six
XFAILs retire to PASS; i7/i7d, whose signal *was* this defect, are re-derived from an independent
oracle; g1/g2/g3/m3a improve and are re-pinned. Runs after M6 and before M2.

## What we know (read before any task)

**The reference fix is measured, not proposed.** `~/deepc-validation/M6-P2T2/patches/m6-mut-all.patch`
(applied on `71a3ee3`, runtime bitmask `DEEPC_M6_MUT`, `patches/INDEX.txt`) bit **64** is the fix.
Under it: all six o XFAILs read DIP GONE, o3's worst pixel (171,103) reads α `1.000000000`, the
interior worst `1−α` is 1.192e-7 and o5b's max |DeepCDefocus−Bokeh| is 1.788e-7; g1 K=16
3.03e-8→1.38e-8, g2 K=16 2.09e-6→1.79e-7, g3 K=16 2.09e-6→1.19e-7, m3a 2.09e-6→1.19e-7 (improve);
i7 0.0900→2.96e-3 and i7d 0.0900→9.53e-4 (FAIL); a, b, c, d, e, f, h, j, k, l, n bit-identical;
g4/g5, m0b, m1, m3b/m3c unchanged. Kept EXRs (mut64 vs M6-T0, `exrdiff.py`, measured for this
plan): `over_checker_m1_halo` 0 ulps, `over_checker_m3_ramp_a0.9` ≤4 ulps, `over_checker_m3_ramp_a1`
≤32 ulps. Logs: `logs/cmp-mut64.txt`, `cmp-rest-mut64.txt`, `cmp-acn-mut64.txt`,
`harness-*-mut64.log`; renders `harness-out-mut64/`.

**The patch is not the code to ship.** It was a measurement rig: it buffers scaled colour in
`float srcSat[8]` (breaks for `channelCount > 8`), sets `covShare = aCov/a` for *every* bucket, and
reads `getenv` from a `DEEPC_HD` path. M7's version must: scale colour in place of every `src[o]`
read as `(src[o] * s)` — parenthesised so it rounds exactly like `saturateBucketPixel()`'s in-place
`color *= s` (the patch's H6 control, bit 4, measured the fold bit-identical); use `aCov/a` /
`aRes/a` shares **only** in the new saturated two-area branch so every other path is bit-unchanged
by construction; stay `DEEPC_HD`-clean (no `std::`, no heap, no host calls, no fixed-size channel
buffers).

**Why the saturate pass goes rather than stashing `A_raw`.** `saturateBucketPlanes()` overwrites
the alpha plane in place, so `A_raw` is gone before the composite runs; keeping it would need a
fifth plane (memory, and the M3 seam). Folding saturation into the composite needs nothing: the
composite already clamps `A_k` to `[0,1]` at read (matches saturate's exact `1.0f` for `a`), and
the colour factor becomes a per-bucket scalar.

**Corrections to M6's "What we know" that bear on M7:** hidden (share-0) samples still deposit
full alpha, colour and area — share feeds arrival only; an opaque split's rear carries alpha `w`,
not `w·(1−f)`; H6's defect is the aCov/aRes split, not `fit = min(C_k, freeArea)`; H2's axis is
size/CoC slope, not integer diameter. Scene (o)'s o2 cell note still states the share-0 premise.

**Baselines (T0).** M6's closing a–o tally is the reference: **`PASS=152 FAIL=2 XFAIL=17 SKIP=2`**
= M6-T0's a–n 134/2/11/2 + scene (o) 18/0/6/0, logs in `~/deepc-validation/M6-P3T1/` (a–f, g–j,
k–n, o as separate batches — hostguard's memory floor killed a combined g–o run); scene (o) after
PR #108's review fixes is `review-fix/harness-half2-o.log`, row-identical. Scene (m) EXRs:
`~/deepc-baselines/M6-T0/renders/` (0 ulps vs M6-P3T1 and the review fix). FAILs are f3e/f3f
(pre-existing); the 11 a–n XFAILs are f's five, g4, g5×4, m3c; SKIPs n4b and one in a–f. There is
no fresh T0 `.so` for M7 yet — P1.T1 captures the complete pre-fix plugin set, which the profile
and the mutation runs both need.

**Constraints from the board apply in full:** Nuke 16.0v9 only, `build/local-16.0`, configured
with `-D "DEEPC_DEFOCUS_ISA_FLAGS=-mavx;-mfma"`; every build and Nuke run through
`scripts/hostguard.sh` (`--mem-gb 5.5`/`6` for the harness), harness in batches (a–f, g–j, k–n,
o); no 8-bit tolerances — pins are float-exact or `N·2⁻²⁴` term-count bounds against an
independent oracle, never against the new output; no hand SIMD; `DEEPC_HD` kernels nvcc-clean;
`PodBuffer<T>` planes; doctest; 4-space/`_`member/lowerCamelCase; comment policy (no narrative, no
plan/task IDs in code). Every new alpha assertion gets a colour:alpha ratio assertion beside it;
every surviving XFAIL keeps a hard outer bound; every new check is mutation-tested. Work happens on
`claude/deep-defocus-node-plan-o0ld83`.

## Phase 7.1: Baseline and the release gate

- [x] M7.P1.T1 — Capture M7-T0: the pre-fix plugin set from `088a76f`
  - files: `~/deepc-baselines/M7-T0/` (new, outside the repo)
  - approach: from the clean tree at `088a76f` (master = M6 + PR #108 review fixes):
    `cmake -S . -B build/local-16.0 -D Nuke_ROOT=/usr/local/Nuke16.0v9 -D DEEPC_BUILD_TESTS=ON -D "DEEPC_DEFOCUS_ISA_FLAGS=-mavx;-mfma"`
    then `scripts/hostguard.sh -- cmake --build build/local-16.0 -j2`. Copy **every** plugin `.so`
    from the build into `~/deepc-baselines/M7-T0/plugins/` (the harness and `run_profile.sh` need
    the other DeepC nodes). Write `PROVENANCE.txt` (commit, date, SDK, configure line, `sha256sum`
    of each `.so`). Run both doctest binaries and keep their logs. Render the pre-fix scene (o) with
    `DEEPC_PLUGIN_DIR=~/deepc-baselines/M7-T0/plugins scripts/hostguard.sh --mem-gb 6 -- tests/nuke/run_validation.sh --scenes o --threads=2 --out-dir ~/deepc-baselines/M7-T0/renders`
    (keeps `o5_alpha_diff_defocus_minus_bokeh.exr`). Do **not** re-run a–n: `0d647d8` touched only
    the probe re-composite, `makeBokeh`, scene (o) wording and the README — confirm with
    `git diff --stat 71a3ee3 088a76f`; the a–n reference stays M6-P3T1's logs.
  - verify: doctests 27/27 + 117/117; scene (o) `PASS=18 FAIL=0 XFAIL=6 SKIP=0`, every row equal to
    `~/deepc-validation/M6-P3T1/review-fix/harness-half2-o.log` (whitespace-normalised);
    `DeepCDefocus.so` from `plugins/` loads headless and creates a node; the diff-stat claim, paths
    and sha256 reported to the PM for `## Decisions`.
  - size: S

- [x] M7.P1.T2 — Make `docker-build.sh` archive with `python3 -m zipfile` instead of `zip`
  - files: `docker-build.sh`
  - approach: replace both `(cd … && zip -r <archive> "DeepC")` calls (Linux and Windows) with
    `(cd … && python3 -m zipfile -c <archive> "DeepC")`; replace the `command -v zip` prerequisite
    with `command -v python3` and update the header's `Prerequisites:` line. Delete any existing
    `release/DeepC-*-Nuke<ver>.zip` before writing so behaviour doesn't depend on the tool
    (`zipfile -c` overwrites; `zip -r` appended). No other behaviour change. Retires the `PATH` shim
    from `PLAN/DECISIONS/2026-09-06-docker-linux-gate-runs-here.md`.
  - verify: `bash -n docker-build.sh`; `grep -nE '\bzip -r|command -v zip' docker-build.sh` empty;
    the archive step run by hand on a scratch tree (`<scratch>/install/DeepC/{plugins/a.so,README}`,
    `a.so` mode 0755) gives an archive whose `python3 -m zipfile -l` lists both entries under
    `DeepC/`, `external_attr >> 16` of `a.so` is `0o100755`, and running it twice leaves one copy
    of each entry.
  - size: S

- [x] M7.P1.T3 — Rebuild the `nukedockerbuild:16.0-linux` image per the 2026-09-06 recipe and prove the gate on the unchanged tree
  - files: none in the repo; the edited upstream Dockerfile, exact commands and logs under
    `~/deepc-validation/M7-docker/`; the PM adds a dated addendum to
    `PLAN/DECISIONS/2026-09-06-docker-linux-gate-runs-here.md`
  - approach: check `docker images` (expected empty) and `df -h` on Docker's data root (the Nuke SDK
    context is ~13.7 GB — confirm room first). Follow the recipe: (1) `docker pull
    mirror.gcr.io/almalinux:8` (probe `mirror.gcr.io/library/almalinux:8` too) and retag to the
    edited Dockerfile's `FROM`; (2) `git clone --depth 1 https://github.com/gillesvink/NukeDockerBuild
    <scratch>/ndb`, edit `dockerfiles/16.0/linux/Dockerfile`: base → AlmaLinux 8, `cmake3` → `cmake`,
    Foundry installer download → `COPY --from=nukesdk . /usr/local/nuke_install`; (3) `docker build
    --build-context nukesdk=/usr/local/Nuke16.0v9 -t <the tag docker-build.sh expects> -f <edited
    Dockerfile> <its dir>` (read `docker-build.sh` for the tag/invocation it uses and make the image
    satisfy it without editing the script beyond P1.T2). Save the edited Dockerfile and a
    `commands.sh` reproducing steps 1–3 verbatim. Then on the tree at `088a76f` + P1.T2 run
    `./docker-build.sh --linux`, with nothing heavy alongside (hostguard doesn't bound the daemon).
  - verify: `docker image inspect` succeeds; `./docker-build.sh --linux` exits 0 writing
    `release/DeepC-Linux-Nuke16.0.zip` (`SKIP` for 16.1/17.0); `python3 -m zipfile -l` lists **28**
    plugins incl. `DeepCDefocus.so`; build log shows 0 errors and no warning in `DeepCDefocus*`; the
    unzipped `DeepCDefocus.so` loads in headless 16.0v9 and creates a node — do **not** render with it
    (built `-mavx2`; confirm AVX2 via `objdump -d` mnemonics rather than asserting); `commands.sh` in
    the evidence dir.
  - size: M

- [x] M7.P1.T4 — `docker-build.sh`: opt-in read-only bind mount of a local Nuke SDK, then prove the gate
  - files: `docker-build.sh`, `README.md` (build section, one line if it documents docker-build flags)
  - approach: add an opt-in `--nuke-sdk <dir>` flag (and/or `DEEPC_NUKE_SDK` env) that, when set,
    adds `-v <dir>:${NUKE_SDK_PATH}:ro` to the Linux `docker run` and skips NukeDockerBuild's
    installer-based image build in `ensure_image` (the image must already exist, toolchain-only, from
    P1.T3). Default behaviour (no flag) unchanged. Windows path untouched. Then run
    `./docker-build.sh --linux --nuke-sdk /usr/local/Nuke16.0v9` on the tree at P1.T2's head + this
    change, nothing heavy alongside.
  - verify: `bash -n`; without the flag the generated `docker run` line is byte-identical to before
    (echo/dry-run or diff of the script's command construction); with it, exit 0,
    `release/DeepC-Linux-Nuke16.0.zip` lists **28** plugins incl. `DeepCDefocus.so`, 0 errors and no
    `DeepCDefocus*` warnings in the log, `SKIP` for 16.1/17.0; the unzipped `DeepCDefocus.so` loads
    headless in 16.0v9 (no render — AVX2 build; confirm AVX2 via `objdump` mnemonics).
  - size: S

## Phase 7.2: The composite fix

- [x] M7.P2.T1 — Fold saturation into `compositePixelCoveragePartitionImpl()` and split a saturated two-area bucket from the raw `A_k`
  - files: `src/DeepCDefocusScatter.h` (`compositePixelCoveragePartitionImpl` ~2921–3485, its
    contract block ~2221–2330, `CompositeTrace*` structs ~2851–2919, the `resolveBandCPU` comment
    ~3606), `src/DeepCDefocusScatter.cpp` (`resolveBandCPU` ~1887, `recordProbePlanes` ~1860),
    `src/DeepCDefocusMath.h` (`saturateBucketPixel`/`saturateBucketPlanes` ~1827–1884),
    `tests/test_defocus_scatter.cpp`, `tests/test_defocus_math.cpp`
  - approach: **Composite, per bucket:** read `aRaw = bucketAlpha[ko]`; keep `a = clampf(aRaw,0,1)`
    as now; add `s = (aRaw > 1.0f) ? 1.0f / aRaw : 1.0f` (saturate's reciprocal; NaN fails `> 1` as
    before). New branch first: `if (colo > 0.0f && aRaw > 1.0f)` → `u = clampf(aRaw / (max(C_raw,0)
    + max(D_raw,0)), 0, 1)` from the **unclamped** planes, `aCov = u·cov`, `aRes = u·colo`,
    `covShare = aCov / a`, `resShare = aRes / a` (`a == 1`, exact). Else the existing branches,
    unchanged. Every `src[o]` read in the fit, excess and residual colour terms becomes
    `(src[o] * s)` — parentheses load-bearing. **`resolveBandCPU`:** drop the
    `saturateBucketPlanes()` call. **Trace:** the traced composite writes `planes.aSat = a` and
    `planes.colorSat[c] = src[c]*s` itself; `recordProbePlanes`'s saturated arm goes; keep field
    names so `DeepCDefocus.cpp` compiles unchanged; add `u` and `satScale` to `CompositeTraceTerms`.
    **`DeepCDefocusMath.h`:** delete `saturateBucketPlanes`; replace `saturateBucketPixel` with a
    `DEEPC_HD` scalar `saturationScale(float aRaw)` returning `s`, math doctest rewritten against it
    (down-only, NaN untouched, 1/alpha). **Contract block:** rewrite the claims "C_k is clamped to 1
    at use, which keeps a_k ≤ 1 after saturation has pulled A_k to 1" and "per bucket the three terms
    sum to at most A_k" — state the saturated two-area rule, its per-unit-opacity argument and the
    new per-bucket bound (≤ `u·(C+D)` ≤ `A_raw`, post-walk clamp scaling the premultiplied pair
    together as before); policy-compliant. **Doctests, each vs an independent oracle:** (a) one
    bucket `C=1, D=1, A=2`, opaque colour `2·c̄`: alpha `== 1.0f`, `out/alpha == c̄` within `N·2⁻²⁴`
    (pre-fix 0.75); (b) two buckets, front `C0=0.4,A0=0.4`, rear `C1=0.7,D1=0.5,A1=1.2`, all opaque:
    alpha `== 1.0f` within bound, with ratio arm; (c) the (171,103) replay — raw planes from
    `~/deepc-validation/M6-P2T1/probe_o3.txt` (`%.9g`, round-trips exactly), alpha `== 1.0f`
    (pre-fix 0.969), colour ratio vs the planes' shared G/A, B/A; (d) end to end through
    `scatterBandCPU`→`resolveBandCPU`: two full-field same-bucket layers with different kernel radii
    (the i7 shape) at `a ∈ {0.6, 1.0}` — interior reads `1−(1−a)²` within `N·2⁻²⁴` (`over`
    algebra, `N` from the two discs' taps), colour/alpha equals the source's; (e) saturated
    `colo == 0` bucket (`C=1, A=1.5`): colour `== colour·(1.0f/1.5f)` **bit-exactly**; (f) update the
    existing real-path saturation case (~6888): planes keep their additive count after resolve,
    outputs unchanged; (g) update the probe replay (~11705): fixture pixel 4 (`C=0.7, A=1.3, D=0.4`)
    takes the new branch with `u = 1`, replay still bit-exact. **Mutation:** on a scratch copy revert
    the split to `a*(colo/(cov+colo))` — (a)–(d) must fail; never committed.
  - verify: build clean; doctests `27/27` + `117+n`; the scratch mutation fails (a)–(d); grep of the
    `compositePixelCoveragePartitionImpl` body and `saturationScale` for `std::`, `getenv`, `new`,
    `malloc` and fixed channel buffers finds nothing outside the `kTrace` sink;
    `--scenes a,c,d,n` every row byte-identical to M6-P3T1's logs; `--scenes o` reads
    `PASS=18 FAIL=6 XFAIL=0 SKIP=0`, the six being the pinned dips reading DIP GONE — any other o row
    moving stops the task.
  - size: L

- [x] M7.P2.T2 — Probe printout and pipeline comments follow the fold
  - files: `src/DeepCDefocus.cpp` (`printProbe` ~1978; pipeline comments ~24, ~1359, ~2065)
  - approach: print `u` and `satScale` beside each bucket's `aRaw/aSat`; label `aSat`/`colorSat` as
    the composite's clamped alpha and scaled colour (no longer read from planes). Update the three
    pipeline comments that say "saturate down, then composite" / "saturate + resolveBandCPU" to
    describe one resolve step. No behaviour change; zero-cost-when-unset contract unchanged.
  - verify: build clean, doctests green; `DEEPC_DEFOCUS_DEBUG_PROBE="171,103"` on o3 (reuse
    `~/deepc-validation/M6-P2T1/probe_o3.py`) prints one block showing buckets 5–7 with `aRaw > 1`,
    `u < 1` where `D > 0`, and `outAlpha 1` — saved as `~/deepc-validation/M7-P2T2/probe_o3.txt`;
    with the variable unset, scene (m)'s three kept EXRs are 0 ulps vs P2.T1's.
  - size: S

## Phase 7.3: Re-pin, and retire scene (o)'s XFAILs

- [x] M7.P3.T1 — Scene (o): the six dips → PASS, o5's node−Bokeh alpha gated, the o2 note corrected
  - files: `tests/nuke/scenes.py` (`O_PIN_*` ~5526–5531, `sceneO()` ~5534, `oCheck` ~5492),
    `tests/nuke/generate_scene_scripts.py` (imports ~39–40, o StickyNote ~169–175, ~1027),
    `tests/nuke/scene_o_solid_alpha.nk` (regenerated)
  - approach: remove every `O_PIN_*` value; o1b, o2b, o3, o3b, o4, o4b become unpinned `oCheck` rows
    (0 px past `oTolerance` per pixel). Keep the two-sided XFAIL machinery in `oCheck` for future
    pins; retire any comment describing these six as known dips. New row **o5c** — "DeepCDefocus −
    Bokeh alpha over the interior": `|α_node − α_Bokeh| ≤ tol(x,y) + 2⁻²⁴` per pixel (`tol` =
    `rigTolerance`; `2⁻²⁴` is Bokeh's own measured ±1 ulp from M6.P1.T3), from the o3 and o5 renders
    already in hand — literal zero isn't attainable (Bokeh reads `1 ± 1 ulp`; mut64's o5b max was
    1.788e-7); the note carries the count of pixels whose difference is exactly 0. **o5cr** beside
    it: node vs Bokeh G/A and B/A per pixel, bound `2·tol(x,y)`. o2's note: replace the share-0
    premise with the corrected one (hidden samples deposit full alpha, colour and area; share feeds
    arrival only). Regenerate the `.nk` with `NUKE_PATH=build/local-16.0/src
    /usr/local/Nuke16.0v9/Nuke16.0 -t tests/nuke/generate_scene_scripts.py`, StickyNote rewritten
    (no pins, all PASS).
  - verify: `--scenes o` on the P2 build reads **`PASS=26 FAIL=0 XFAIL=0 SKIP=0`** (the 24 existing
    rows + o5c/o5cr); o0/o0b/o5/o5r unchanged from T0; **mutation:** the same run on
    `~/deepc-baselines/M7-T0/plugins` FAILs o1b, o2b, o3, o3b, o4, o4b and o5c with values equal to
    M6's pins to the printed digit (3.067e-2 at (171,103) etc.); `git diff --stat` shows only
    `sceneO`-scoped lines and the generator; the regenerated `.nk` loads headless.
  - size: M

- [x] M7.P3.T2 — Gate scene (o)'s mutation runs (K=64, K=4, same depth, sparse K=64, probe ring K=64) as unpinned rows
  - files: `tests/nuke/scenes.py` (`sceneO()`: the o2b K=64 render ~5720, the o3/o3b/o4/o4b
    `MUTATIONS:` notes), `tests/nuke/generate_scene_scripts.py` (StickyNote row list)
  - approach: PR #108 review finding #12, deferred here. Promote each note-only reading to its own
    `oCheck` row requiring 0 px past bound, with a colour-ratio row beside each: `o3m` (rig interior
    at K=64 / K=4 / pair at one depth), `o3bm` (same three over the overlap), `o4m` (sparse K=64,
    interior + overlap), `o2bm` (the r_plane 8 ring at K=64 — render unconditionally now). Keep the
    notes' cross-references. **Mutation-test each new row and name the mutation in its note:** rows
    that dipped pre-fix (K=4, same depth, sparse K=64, per mut64's `logs/harness-mut64.log` and the
    M6.P1.T3 record) must FAIL on `~/deepc-baselines/M7-T0/plugins`; rows clean even pre-fix (K=64)
    need a scratch source mutation that moves them (try `m6-mut-all.patch` bit 16, opaque rear-deposit
    drop, ported to the M7 tree; or an un-parenthesised/omitted `s` on the residual colour term for
    ratio rows); a row no mutation moves is dropped and the drop recorded in the note of the row it
    would have guarded — no vacuous gates. Scratch builds never committed.
  - verify: `--scenes o` reads `PASS=26+M FAIL=0 XFAIL=0 SKIP=0` with `M` the rows added (stated in
    the report); each new row has a recorded mutation run showing it FAIL, logs under
    `~/deepc-validation/M7-P3T2/`; regenerated `.nk` loads headless.
  - size: M

- [x] M7.P3.T3 — Re-derive i7/i7d from an independent oracle: `over` in the interior, reachability as a non-zero floor
  - files: `tests/nuke/scenes.py` (`sceneI()` i7/i7d ~2607–2735, the
    `collisionResidual`/`residualBand` block and its comment)
  - approach: i7/i7d pinned `a²/4 = 0.09` — this defect (a pooled co-located pair saturated to 1
    and split 0.5/0.5 read 0.75 instead of `over`'s 0.84 at a = 0.6). Under mut64 they read 2.96e-3 /
    9.53e-4 — must **not** be pinned (it's the new output). Probe i7's and i7d's argmax pixels on the
    P2 build with `DEEPC_DEFOCUS_DEBUG_PROBE`, `pre_merge` on and off. Expected (to confirm): the
    remaining delta lives only inside the z=3 corner element's bloom — the corner box `(0,0,24,24)`
    outset by its CoC `20·|1−10/3| ≈ 46.7 px` (its `max_radius` clamp for i7d), ∩ `reachBox` — where
    the pair sits under partial front coverage and grouped vs ungrouped discs take different
    excess/residual paths. If the argmax lies outside that box, stop and report rather than re-pin.
    Replace each of i7, i7d with two rows: (1) **interior arm** — over `reachBox` minus the corner
    bloom box, both renders (`pre_merge` on and off) read alpha `1 − (1−0.6)² = 0.84` and R/A equal
    to the stock `deepToImage` flatten's R/A, bound `N·2⁻²⁴` per pixel with `N` =
    `O_TERMS_PER_TAP · (taps of the two pair discs)` counted as `oTolerance` does; (2)
    **reachability arm** — max |on − off| over `reachBox` exceeds that per-pixel bound somewhere and
    its argmax lies inside the corner bloom box; magnitude reported, deliberately not pinned. i7b/i7c
    unchanged; rewrite the i7 comment block (drop the "collision residual a²/4" account); keep i7d's
    role as i7c's non-vacuity guard.
  - verify: `--scenes i` on the P2 build: i7/i7d each replaced by two PASS rows, every other i row
    byte-identical to M6-P3T1's `harness-half2-ghij.log`; **mutation:** on
    `~/deepc-baselines/M7-T0/plugins` the interior arms FAIL (`pre_merge` off ~0.75 — the old 0.09
    deficit; numbers in the note) while the reachability arms still PASS; probe logs and argmax
    locations under `~/deepc-validation/M7-P3T3/`.
  - size: L

- [ ] M7.P3.T4 — Re-pin g1/g2/g3 and m3a to term-count bounds, with every move explained against M6-T0
  - files: `tests/nuke/scenes.py` (`sceneG()` g1/g2/g3 ~1755–1860 and its before→after comment
    table; `sceneM()` m3a ~3885–3918)
  - approach: these rows improve under the fix (g1 K=16 3.03e-8→1.38e-8, g2 K=16 2.09e-6→1.79e-7,
    g3 K=16 2.09e-6→1.19e-7, m3a 2.09e-6→1.19e-7; K=8/K=64 columns and m3a's R/A measured here) but
    are still gated at `1e-3`/`1/255`; since M7 moves them, re-pin to the project rule. Oracle
    analytic (alpha 1 on an opaque ramp; R/A equal to the stock flatten's — m3a already reads
    `sourceRatio`); bound `O_TERMS_PER_TAP · _planeTaps(size, y) · 2⁻²⁴` (scene (g) and m3a render the
    same `groundPlane()` ramp, so scene (o)'s tap count applies at each scene's `size`); a row mean
    gated at the max per-pixel bound over its row, g3's row-to-row step at the sum of the two rows'
    bounds, g2's K-vs-K=64 difference at the sum of both renders' bounds. Extend scene (g)'s
    before→after table with M6-T0 → M7 readings for every K; rewrite its "WHY AN OPAQUE PLANE SHOWED
    THE ALPHA<1 DEFECT AT ALL" paragraph (saturation no longer strips the new-area share's opacity).
    **Mutation:** each re-pinned row goes red under a scratch mutation whose reading lies between the
    new bound and the old 8-bit gate (e.g. drop the `s` scale on the fit colour term for ratio rows;
    a named composite perturbation for alpha rows — M6 patch bit 2, symmetric `1/D`, is a
    candidate); record the reading. Other `1/255` gates in g, i, m that M7 doesn't move stay (listed
    in `## Decisions`).
  - verify: `--scenes g,m` on the P2 build: every g/m row PASS/XFAIL as at T0, only g1/g2/g3/m3a's
    readings and gates changed, g4/g5/m0b/m1/m3b/m3c byte-identical to M6-P3T1; each re-pinned row
    PASSes on **both** the P2 build and `M7-T0/plugins` (bound not fitted to the new output) and fails
    under its recorded mutation; the comment table carries both columns.
  - size: M

- [ ] M7.P3.T5 — Mixed-opacity co-located stack vs Bokeh: measure the clamped-split rule's residual gap
  - files: `tests/nuke/scenes.py` (a new scene-(o) cell or a small new scene — follow how scene (o)
    registers cells and uses `makeBokeh()`), `tests/nuke/generate_scene_scripts.py` (StickyNote /
    `.nk` if the scene gets a script), the regenerated `.nk`
  - approach: the P2.T1 split rule divides a saturated bucket's raw alpha over the clamped new and
    co-located areas; planes cannot distinguish a mixed-opacity stack from uniform layers, so an
    opaque surface with a fog card co-located in the same bucket, under a large new-area share, is
    predicted to read slightly low (doctest planes: ε=0.5 fog → 0.919 vs 1). Measure it on a real
    render against Bokeh. Rig: complete deep, `fill: foreground`; an opaque card at depth z, a
    translucent "fog" card (alpha ∈ {0.2, 0.5}) at z − δ with δ inside one bucket's depth span at
    K=16, and a small opaque object nearer the camera with its own CoC so its disc supplies new area
    over the stack; sweep K ∈ {4, 16, 64} and the two fog alphas. Render `DeepCDefocus` and
    `makeBokeh()` on the identical deep input. Rows: per cell, worst `1 − α` over the covered box
    where the opaque card is present (oracle: alpha 1 — the opaque card covers every such pixel), node
    vs Bokeh alpha difference (bound `tol + 2⁻²⁴` as o5c), and a colour-ratio arm beside each.
    Outcome rules: a cell within bound PASSes; a cell that dips while Bokeh reads 1 becomes a
    two-sided XFAIL pinned at its measured value with a hard outer bound (2× pin) and a note naming the
    mechanism (bucket-sum loses per-layer opacity) — the gap is documented, not fixed, in M7. Report
    Bokeh's own readings for every cell. If δ-vs-bucket placement is fragile, derive δ from the
    node's bucket centres at each K (read how scene (o)/(g) place depths) rather than guessing.
    Mutation-test every new row: XFAIL rows must FAIL "DIP GONE" when the dip is removed (e.g. a
    scratch build with the per-layer-exact alpha for the rig's two layers, or placing the fog in a
    different bucket), PASS rows must fail under the pre-fix `~/deepc-baselines/M7-T0/plugins` or a
    named scratch mutation — no vacuous gates.
  - verify: `--scenes o` (or the new scene) on the P2 build: tally stated in the report as
    `PASS=… FAIL=0 XFAIL=X` with every pre-existing row unchanged from P3.T2's log; per-cell table of
    node alpha, Bokeh alpha and node−Bokeh difference, logged under `~/deepc-validation/M7-P3T5/`;
    each new row's mutation run recorded; regenerated `.nk` loads headless.
  - size: M

## Phase 7.4: Gate

- [ ] M7.P4.T1 — Profile against M7-T0 under the same conditions
  - files: none; evidence under `~/deepc-validation/M7-P4T1/`
  - approach: M5's 28.0 s (2K/20spp, 2 threads) was on the pre-reprovision host (AVX2, 17.0v3) —
    context only. Measure M7 vs M7-T0 on this host, interleaved T0, M7, T0, M7, each
    `DEEPC_NUKE=/usr/local/Nuke16.0v9/Nuke16.0 DEEPC_PLUGIN_DIR=<dir> scripts/hostguard.sh --mem-gb 6 -- tests/nuke/run_profile.sh --reps 5 --threads 2 --stats`
    with `<dir>` = `~/deepc-baselines/M7-T0/plugins` then `build/local-16.0/src`. The fix drops a full
    pass over the K·C·P planes and adds a divide+multiply per saturated bucket — measure, don't
    assume. "Within noise": |median(M7) − median(T0)| ≤ the larger same-`.so` median spread
    (|T0a−T0b|, |M7a−M7b|), or M7 faster; RSS not higher than T0's by more than the same spread.
  - verify: four runs logged; medians, CPU times, RSS and the noise figure reported for
    `## Decisions` beside M5's 28.0 s (marked old-host); within noise or faster, else the regression
    goes to the user before P4.T2.
  - size: S

- [ ] M7.P4.T2 — Full a–o suite, cross-`.so` accounting, the docker gate, and the PR
  - files: none — verification only; any fix lands in the task that owns the code
  - approach: clean rebuild of `build/local-16.0` at the M7 head; both doctest binaries; the a–o
    harness in four batches under `scripts/hostguard.sh --mem-gb 5.5`, `DEEPC_PLUGIN_DIR=build/local-16.0/src`,
    `--threads=2`: a–f, g–j, k–n (keeping renders into `~/deepc-validation/M7-P4T2/renders`), o. Diff
    every a–n row against M6-P3T1's logs (whitespace-normalised); every changed row must be on the
    explained list — g1/g2/g3/m3a (P3.T4), i7/i7d → the four new i rows (P3.T3), nothing else.
    `exrdiff.py` scene (m)'s kept EXRs vs `~/deepc-baselines/M6-T0/renders`: expected `m1_halo` 0
    ulps, `m3_ramp_a0.9` and `m3_ramp_a1` small moves (mut64 read ≤4 and ≤32 ulps), explained by
    probing the worst pixel of each (a saturated bucket with `D > 0` on the ramp — the fix's own term).
    `./docker-build.sh --linux` on the M7 head with P1.T3's checks. Then the PR from
    `claude/deep-defocus-node-plan-o0ld83` to `master`.
  - verify: doctests `26/26` + scatter count from P2.T1 (120+n); a–o tally **`PASS=160+I+M+F FAIL=2 XFAIL=11+X SKIP=2`** — T0's
    152/2/17/2 with scene (o)'s six XFAILs → PASS (+6), o5c/o5cr (+2), `I` = net new i rows (P3.T3:
    +2), `M` = P3.T2's rows, `F`/`X` = P3.T5's PASS/XFAIL rows; restate the exact number from the task reports before checking; FAILs
    f3e/f3f only; the 11 surviving XFAILs (f's five, g4, g5×4, m3c) byte-identical with hard bounds;
    size-0 parity (a) bit-exact, (d) empties exactly black, m4a/m4b, n1, n7 PASS; every changed a–n
    row on the explained list; EXR ulp figures recorded and explained; docker gate green.
  - size: M

**Verification gate:** both doctest suites green including the saturated two-area cases (a)–(e)
against their independent oracles, mutation-tested; scene (o) all PASS with no XFAIL — the six
former dips read `1 − α` within their per-pixel `N·2⁻²⁴` bound, o5c's node−Bokeh alpha difference
within bound over the interior, every colour-ratio arm PASS; the K=64/K=4/same-depth/sparse/ring
mutation runs are gated rows each shown to fail under a named mutation; i7/i7d re-derived (`over`
interior arm + reachability floor); g1/g2/g3/m3a re-pinned to term-count bounds; every surviving
XFAIL unchanged with its hard bound; a–o tally `PASS=160+I+M+F FAIL=2 XFAIL=11+X SKIP=2` with every moved
a–n row explained against M6-T0/M6-P3T1; scene (a) bit-exact, (d) black, m4a/m4b/n1/n7 hold;
profile within noise of M7-T0 on this host; `./docker-build.sh --linux` green on the rebuilt image;
then PR to `master` from `claude/deep-defocus-node-plan-o0ld83`.

## Decisions

- 2026-09-23 — **Elaborated from M6.P2.T2's mechanism ruling** (local fix: mut64's corrected C:D
  split with saturation's `1/A_raw` colour scale). Implementation choice: **fold saturation into the
  composite and delete the in-place pass** — the pass overwrites `A_raw`, keeping it otherwise costs
  a fifth plane, M6's H6 control (bit 4) measured the fold bit-identical, and parenthesised `(src·s)`
  plus branch-local shares keep every unsaturated and `colo == 0` path bit-unchanged by construction.
  The patch's `srcSat[8]` buffer, global `covShare = aCov/a` and `getenv` are not carried over.
- 2026-09-23 — **Sequencing: docker image rebuilt first (Phase 7.1), on the unchanged tree.** M6
  waived the gate; proving the rebuilt image and the `python3 -m zipfile` archive step before any M7
  code lands means a later docker failure can only be M7's. T0 reuses M6-P3T1's a–o logs (`0d647d8`
  touched no a–n path) and captures only the pre-fix plugin set, which the profile and mutation runs
  need.
- 2026-09-23 — **i7/i7d are not re-pinned to a number:** their old value was the defect and the
  residual delta under the fix has no independent oracle, so the correctness claim moves to an
  `over`-algebra interior arm vs the stock flatten, and reachability becomes a non-zero floor at a
  derived location (magnitude reported, not pinned).
- 2026-09-23 — **g1/g2/g3/m3a convert from `1e-3`/`1/255` to term-count bounds** because M7 moves
  these rows (per the 2026-09-18 no-8-bit ruling). Other legacy `1/255` gates in g, i, m that M7
  doesn't move are left for a later sweep.
- 2026-09-23 — **o5's "zero" difference is gated at the term-count bound + Bokeh's own 1 ulp**
  (o5c/o5cr), not literal zero — Bokeh itself reads `1 ± 1 ulp`.
- 2026-09-23 — **Profile baseline is M7-T0 re-measured on this host;** M5's 28.0 s came from the
  pre-reprovision host (AVX2, 17.0v3) and is quoted only as context.
- 2026-09-23 — **Freshness note for M2:** its briefs must be checked against the composite's new
  saturated branch and the removed `saturateBucketPlanes`.
- 2026-09-23 — **M7.P1.T1: M7-T0 captured** at `088a76f` → `~/deepc-baselines/M7-T0/` (28 plugins,
  `PROVENANCE.txt` with every sha256; `DeepCDefocus.so` `06bc7c47…8a6a`). Doctests 27/27 + 117/117;
  scene (o) 18/0/6/0, all 24 rows equal to M6-P3T1's review-fix log; loads headless.
  `git diff --stat 71a3ee3 088a76f`: 9 files, all PR #108's review fixes (probe re-composite,
  `makeBokeh`, scene (o) wording, `run_validation` kept-EXR line, README) — no a–n render path.
- 2026-09-23 — **Docker image is toolchain-only; the SDK is bind-mounted read-only at `docker run`**
  (user's suggestion) instead of `COPY`ed into the image via a BuildKit named context — avoids a
  ~14 GB image layer (plus build-cache copy) on a disk with 33 GB free. P1.T3 now builds only the
  toolchain image; new P1.T4 adds the opt-in `--nuke-sdk` mount to `docker-build.sh` and runs the
  gate. P1.T3's first attempt failed at the image's `dnf install gcc-toolset-11…` step.
- 2026-09-23 — **M7.P1.T3: toolchain-only image built** — `nukedockerbuild:16.0-linux` (`28421e024520`,
  851 MB on disk): AlmaLinux 8, gcc-toolset-11 11.2.1, cmake 3.26.5, empty `/usr/local/nuke_install`
  mount point. dnf root cause: stock repo files use `mirrorlist=` on `mirrors.almalinux.org`
  (unroutable); a `sed` step comments out `mirrorlist=` and uncomments `# baseurl=` →
  `repo.almalinux.org` (routable). Evidence and `commands.sh` in `~/deepc-validation/M7-docker/`.
  `release/`, `install/` are gitignored. The gate run moved to P1.T4.
- 2026-09-23 — **M7.P1.T4: docker gate green on the pre-fix tree** (`656f9eb`). `--nuke-sdk DIR` /
  `DEEPC_NUKE_SDK` mounts the SDK `:ro`; missing image with the flag → SKIP, no installer fallback;
  combined with a Windows build → exit 1; no-flag `docker run` line identical (stub-verified).
  `./docker-build.sh --linux --nuke-sdk /usr/local/Nuke16.0v9`: EXIT=0, 28 plugins incl.
  `DeepCDefocus.so`, 0 errors, 0 `DeepCDefocus` warnings, SKIP 16.1/17.0; release `.so` loads headless;
  15 AVX2 mnemonics (not rendered here). Log `~/deepc-validation/M7-docker/docker-build.log`.
- 2026-09-23 — **M7.P2.T1 rule corrected: split the raw alpha over the CLAMPED areas**
  (`u = clamp(aRaw/(cov+colo),0,1)`), not the raw `C_raw+D_raw` the brief/mut64 used. As briefed, the
  doctest "share-side arrival identity" (fog over opaque, rear bucket C=0, D=1.99998784, A=1.14224541)
  read 0.907 vs 1; the implementer's `cov > 0` guard masked it only at C==0 — consultant measured
  alpha 1 → 0.657 at C=1e-7 (0.693 at C=0.1) and a jump at A crossing 1 (C=0.5, D=2: 0.5556 →
  0.4400), because D > 1 means stacked layers and raw C+D overstates area. Clamped-area rule: 1.0 for
  every ε ≤ 0.1, continuous in C at 0 and in A at 1, no special case; doctests 26/26 + 120/120, scene
  (o) and a,c,d,n rows identical to the guarded build. Residual ambiguity (documented in code): planes
  can't tell a mixed-opacity stack from uniform layers, so such a stack under a large new area reads
  slightly low (ε=0.5 fog: 0.919). Also ruled acceptable: 7 non-dip scene (o) rows' *readings* move
  (o5b 3.067e-2→1.788e-7, o1c 2.086e-6→1.192e-7, o1/o2/o2r/o2br/o3br by 1–2 ulps near α≈1) — the
  fix's own term, statuses unchanged; the brief's "only six rows move" was too strict.
  `test_defocus_math` is 26 cases (two saturation cases merged into one `saturationScale` case).
- 2026-09-23 — **User: add M7.P3.T5** — measure the clamped-split rule's residual gap
  (mixed-opacity co-located stack under large new area) on a real render against Bokeh; pin it as a
  two-sided XFAIL if Bokeh reads 1 and the node dips. Final tally gains `F` PASS / `X` XFAIL rows.
- 2026-09-23 — **M7.P2.T1 landed** (`af9fd59`): doctests math 26/26, scatter 121/121 (117 + three
  saturated two-area cases + one continuity case: fog-stack C∈{0,1e-7,1e-2,0.1} alpha == 1, A-crossing
  |Δα| ≤ 180·2⁻²⁴ measured 18 ulps; both fail under the guarded raw-area rule at 0.657 / 0.556→0.440);
  mutation (split reverted) fails (a)–(d) and (g); scene (o) PASS=18 FAIL=6 — exactly o1b/o2b/o3/o3b/
  o4/o4b reading DIP GONE; a,c,d,n 39/39 rows identical to M6-P3T1. Evidence `~/deepc-validation/M7-P2T1/`
  (`rule2/` = final rule).
- 2026-09-23 — **M7.P2.T2 landed** (`59c9231`): probe at o3 (171,103) prints `satScale`/`u`, alpha 1;
  bucket 5 reads aRaw 1.329, satScale 0.753, D 0.328, **u = 1** — expected under the clamped-area rule
  (u < 1 only when min(C,1)+min(D,1) > A_raw); the brief's "u < 1 where D > 0" was written for the
  raw-area rule. Scene (m) EXRs 0 ulps vs `af9fd59`. `~/deepc-validation/M7-P2T2/probe_o3.txt`.
- 2026-09-23 — **M7.P3.T1 landed** (`77778b2`): scene (o) `PASS=26 FAIL=0 XFAIL=0` (24 + o5c/o5cr);
  o0/o0b/o5/o5r unchanged. On M7-T0 plugins: 19/7 — o1b 1.777e-3, o2b 5.138e-4, o3 3.067e-2 @ (171,103),
  o3b 2.493e-3, o4 4.389e-3, o4b 2.199e-3, o5c 3.067e-2 — each equal to M6's pins. The o2b K=64
  re-probe (gated on the removed pin) was dropped; P3.T2's `o2bm` re-adds it unconditionally.
  Regenerating scripts also rewrote the other 14 `.nk` (17.0v3 → 16.0v9 header, random stack names) —
  reverted as out of scope.
- 2026-09-23 — **M7.P3.T2 landed** (`a2f6136`): M = 8 rows (o3m/o3mr, o3bm/o3bmr, o4m/o4mr, o2bm/o2bmr);
  scene (o) `PASS=34 FAIL=0 XFAIL=0`, the 26 prior rows identical. Mutations: o3m/o3bm fail on M7-T0
  (K=4 1.378e-2, same depth 3.067e-2 / 2.385e-3); every ratio row fails with `satScale` dropped on the
  fit colour (4.6e-2–2.6e-1). **Weak point:** the K=64 alpha entries (o4m, o2bm, o3m's K=64 arm) are
  immune to the historical defect and to small perturbations (raw-area split, bit-16, `local` capped
  at 0.9/0.999) — fine bucketing self-heals per-bucket deficits — and only fail under a 50% cut of both
  fit and residual alpha (2.5e-1). They guard against gross breakage only. Evidence `~/deepc-validation/M7-P3T2/`.
- 2026-09-23 — **M7.P3.T3 landed** (`5e568b0`): hypothesis confirmed — pre_merge on/off delta lives only in
  the z=3 corner bloom; argmax (32,32) for both, in bucket k=15 (grouped pair pays via fit/excess,
  ungrouped via the co-located residual under the corner's partial coverage). i7 2.9608e-3, i7d 9.4765e-4.
  Scene (i) 13 → 15 rows, `PASS=15`: new interior arms i7o/i7do (|A−0.84| and R/A vs stock flatten ≤
  N·2⁻²⁴; i7 N=140 → 8.34e-6, i7d 4.70e-4), i7/i7d become reachability arms (bound = sum of both
  renders' pair-bucket terms — deviation from the brief's single-render bound: a difference carries
  both renders' rounding). M7-T0: i7o/i7do FAIL at 0.75 (dev 9.0e-2). **Weak point:** i7d's
  reachability reads only ~1.35× its 7.02e-4 bound. Other i rows identical to M6-P3T1. I = +2.
