# Project Retrospective

*A living document updated after each milestone. Lessons feed forward into future planning.*

## Milestone: v1.0 — DeepC

**Shipped:** 2026-03-17
**Phases:** 6 (5 planned + 1 inserted) | **Plans:** 19 | **Timeline:** 5 days (2026-03-13 → 2026-03-17)

### What Was Built
- Fixed 4 deep-pixel math bugs (DeepCGrade, DeepCKeymix, DeepCSaturation, DeepCHueShift) and removed deprecated NDK usage, DeepCWrapper/DeepCMWrapper menu registrations, and DeepCBlink
- Deadlock-free 3D wireframe GL handle for DeepCPMatte selection volume, drag-to-reposition via Axis knob
- Full Qt6 custom knob UI for DeepCShuffle2: colored X-mark channel buttons, native NDK ChannelSet pickers, radio enforcement per output group, const:0/1 columns, identity routing on first open
- DeepCPNoise true 4D noise evolution across all 9 FastNoise types via `GetNoise(x,y,z,w)` dispatch
- Release cleanup: DeepCShuffle2 icon, single "DeepCShuffle" menu entry via display-name rename dict, SWEEP-07/08 marked Dropped

### What Worked
- **Research-first planning**: RESEARCH.md files with exact file locations, line numbers, and pre-identified pitfalls meant executors rarely deviated from plan
- **Interfaces block in PLANs**: Providing exact before/after code snippets in `<interfaces>` sections made execution fast and deviation-free for most tasks
- **Phase 3.1 insertion**: The decimal phase pattern worked cleanly — UAT revealed real gaps, a fresh plan phase closed them without disrupting the integer phase sequence
- **Op::Description removal pattern**: Established in Phase 1 for DeepCWrapper/DeepCMWrapper, reused identically in Phase 5 for DeepCShuffle — consistent pattern reduced research cost for Phase 5
- **Manual UAT as phase gate**: With no automated test framework for Nuke C++ plugins, blocking on UAT checkpoints correctly caught regressions (QComboBox segfault, radio scope bug) before shipping

### What Was Inefficient
- **Phase 3.1 was preventable**: The QComboBox approach failed at UAT due to a Nuke-specific segfault. The research phase for Phase 3 noted NDK knobs as the safer approach; the plan chose QComboBox for visual reasons. Trusting the research note would have saved Phase 3.1 (5 plans, ~25min).
- **Nyquist validation adoption was uneven**: Only Phase 1 achieved full Nyquist compliance. Phases 2–4 were partial. For a C++ Nuke plugin project with no test framework, the VALIDATION.md structure is mostly placeholder — the real validation signal is UAT. Consider simplifying the validation strategy for projects without automatable test suites.
- **ROADMAP date typos**: Phases 1–4 completion dates were written as `2005-03-XX` instead of `2026-03-XX` in the original ROADMAP.md. Small but worth a quick review step during phase completion.

### Patterns Established
- **Silent binary pattern**: include plugin in CMake `PLUGINS` (compiles to .so), exclude from `CHANNEL_NODES` (no menu entry), remove `Op::Description` (no self-registration). Established Phase 1, reused Phase 5.
- **Display-name rename dict**: `_display_name = {"ClassName": "MenuLabel"}` in `menu.py.in` decouples internal class names from artist-facing labels. No CMake changes needed.
- **Qt6 discovery**: `list(APPEND CMAKE_PREFIX_PATH /opt/Qt/6.5.3/gcc_64)` before `find_package(Qt6)` — pre-installed Qt matches Nuke 16 ABI exactly, no sudo needed.
- **NDK knob ordering as layout control**: Registration order in `knobs()` determines panel position top-to-bottom; using this for in/out ChannelSet knobs relative to custom widget avoids NDK layout limitations.

### Key Lessons
1. **Trust research-identified risk flags**: When RESEARCH.md flags an approach as risky and recommends an alternative, the plan should follow the recommendation. QComboBox was flagged; it failed exactly as predicted.
2. **For Nuke C++ projects, validation = manual UAT**: The Nyquist validation framework adds overhead for projects without a test runner. Simplify VALIDATION.md to a UAT checklist format for pure C++ NDK projects.
3. **Exact code snippets in plan `<interfaces>` blocks pay dividends**: Phases with precise before/after code in the plan executed in under 5 minutes with zero deviations. Phases without them (Phase 3 initial pass) required more executor judgment.
4. **Decimal phases work cleanly for UAT-driven fixes**: Insert as X.Y between the phase that needs fixing and the next planned phase. No renumbering, clean git history, clear audit trail.

### Cost Observations
- Model mix: ~100% Sonnet (sonnet model profile used throughout)
- Sessions: ~5 sessions across 5 days
- Notable: Research agents with high-confidence, file-verified outputs significantly reduced executor time. Phase 5 research identified the Op::Description open question; the planner resolved it by reading the source file, saving a potential Phase 5.1 fix cycle.

---

## Milestone: v1.1 — Local Build System

**Shipped:** 2026-03-18
**Phases:** 1 | **Plans:** 1 | **Timeline:** ~1 day (2026-03-17 → 2026-03-18)

### What Was Built
- `docker-build.sh` — 186-line NukeDockerBuild orchestration script; builds DeepC for Linux (.so) and Windows (.dll) from a single Linux host via Docker; produces `release/DeepC-{Platform}-Nuke{version}.zip` archives
- NukeDockerBuild auto-bootstrap: clones and builds images from source if prebuilt Docker Hub images are unavailable
- Per-platform isolated build/install directories prevent CMake cache conflicts in multi-platform builds

### What Worked
- **Research-first approach again paid off**: Identified the correct NukeDockerBuild image tag format, `$GLOBAL_TOOLCHAIN` escaping requirement, and `Nuke_ROOT` flag necessity before writing a line of code — no debugging loops
- **Human verification checkpoint**: The verify step caught real post-plan issues (image tag format, Windows Qt6 path, zip structure) that required fix commits after the main plan; structured checkpoint made these visible
- **Single-phase scope**: A tight, focused milestone (one deliverable, one script) executed quickly and cleanly with no scope creep

### What Was Inefficient
- **Post-plan fix commits**: Four fix commits were needed after the initial `feat(06-01)` commit — NukeDockerBuild image tag, auto-bootstrap, Qt6 Windows path, zip structure. These were real discoveries during verification, but earlier research into the Windows container environment specifics could have front-loaded some of them
- **NUKE_VERSIONS decision reversal**: The plan locked `("16.0")` only, but subsequent commits added 16.1 and 17.0 to defaults. The research decision was correct at plan time; post-plan additions suggest the "locked decision" framing was too strong

### Patterns Established
- **Container cross-compilation pattern**: Mount repo at `/nuke_build_directory`, run full CMake configure + build inside Docker, extract artifacts — no host-side Nuke SDK needed
- **`$VAR` escaping in `bash -c` strings**: Variables that must expand inside the container shell must be escaped as `\\$VAR` when passed via `bash -c "..."` on the host

### Key Lessons
1. **Verification checkpoints surface real gaps**: Don't skip them even for "simple" scripts — the Windows Qt6 path and zip structure issues were real and non-obvious
2. **Single-phase milestones are fast and clean**: When scope is tight and well-understood, a 1-phase milestone executes in ~1 day with minimal overhead
3. **"Locked decision" framing should match actual lock confidence**: If a decision is likely to be revisited during verification (e.g., default version list), mark it as a default that can be overridden rather than a hard lock

### Cost Observations
- Model mix: ~100% Sonnet
- Sessions: 1 session (~1 day)
- Notable: Short milestone with clear deliverable — minimal orchestrator overhead; most time was in verification fix commits, not planning or execution

---

## Cross-Milestone Trends

### Process Evolution

| Milestone | Phases | Plans | Key Change |
|-----------|--------|-------|------------|
| v1.0 | 6 | 19 | First milestone; established silent binary, display-name rename, and decimal phase insertion patterns |
| v1.1 | 1 | 1 | Infra/tooling milestone; established container cross-compilation pattern and `\\$VAR` escaping convention |

### Cumulative Quality

| Milestone | Automated Tests | Manual UAT | Zero-Dep Additions |
|-----------|-----------------|------------|-------------------|
| v1.0 | 0 (no test framework) | Checkpoints in every feature phase | 1 (Qt6, bundled by Nuke) |
| v1.1 | 0 | Human verify checkpoint for build script | 1 (NukeDockerBuild images, Docker-only) |

### Top Lessons (Verified Across Milestones)

1. Research agents with direct file inspection produce high-confidence plans — exact line numbers and before/after snippets are the highest-value research output
2. UAT/verification checkpoints at the end of each feature phase catch integration issues before they compound across phases
3. Trust research-identified risk flags in plans — when research recommends an alternative approach, follow it (v1.0: NDK knobs; v1.1: exact container env vars)
