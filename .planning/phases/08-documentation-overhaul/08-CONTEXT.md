# Phase 8: Documentation Overhaul - Context

**Gathered:** 2026-03-19
**Status:** Ready for planning

<domain>
## Phase Boundary

README.md and THIRD_PARTY_LICENSES.md accurately describe the current project — 23 plugins, docker-build.sh workflow, Nuke 16+ target, DeepThinner attribution — with no stale or dead content. The GitHub wiki is out of scope for this phase.

</domain>

<decisions>
## Implementation Decisions

### Plugin list presentation
- Keep the same icon + link format used for all other plugins
- DeepThinner: use `icons/DeepThinner.png` (confirmed present) and link to https://github.com/bratgot/DeepThinner
- DeepCShuffle entry: keep the entry named "DeepCShuffle" (Nuke convention — internal class gets a numeral, display name stays the same); update its description and any links to reflect DeepCShuffle2's capabilities (Qt6 custom knob UI, colored channel routing). The old DeepCShuffle is deprecated and DeepCShuffle2 is the current node.
- DeepCBlink entry: remove entirely (DOCS-03)

### Build section
- Replace the entire old Build section (CentOS, Docker-per-Nuke-version, VS2017, batchInstall.sh) with a new section covering `docker-build.sh`
- Include prerequisites upfront: docker, zip, git; note that NukeDockerBuild images auto-build on first run (~1 GB Nuke installer download, requires EULA acceptance)
- Show basic invocation + key flags: `./docker-build.sh` (default Nuke 16.0, both platforms), `--versions`, `--linux`, `--windows`
- Mention output: `release/DeepC-{Linux|Windows}-Nuke{version}.zip`
- Target: Nuke 16+ only

### Old sections
- "Why DeepC?" section: keep mostly as-is, just trim stale references (speed focus on Nuke 11.3, etc.)
- "Get Them!" (releases page link): keep
- "Examples" (DeepCExamples repo link): keep
- "Future Plans / DeepCompress coming soon": remove entirely (DOCS-03)
- "Contributing" section: keep as-is

### THIRD_PARTY_LICENSES.md
- Document all vendored third-party libraries: DeepThinner and FastNoise
- Format: attribution block (project name, author, source URL, license type) + full MIT license text for each
- DeepThinner: Copyright (c) 2025 Marten Blumen, https://github.com/bratgot/DeepThinner, MIT
- FastNoise: Copyright (c) 2017 Jordan Peck, MIT (license text from `FastNoise/LICENSE`)

### Claude's Discretion
- Exact wording of the DeepCShuffle description update
- Order of plugins in the list (alphabetical seems natural)
- Where in the README to place the "DeepThinner" attribution note (inline in plugin list is sufficient per DOCS-01)

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Current README state
- `README.md` — Current file; full content must be read before editing

### License sources
- `FastNoise/LICENSE` — Exact MIT license text for FastNoise (Jordan Peck, 2017)
- `src/DeepThinner.cpp` lines 1–10 — SPDX-License-Identifier and Copyright header for DeepThinner attribution

### Phase requirements
- `.planning/REQUIREMENTS.md` — DOCS-01 through DOCS-04 define exactly what must change

### Icon assets
- `icons/DeepThinner.png` — Confirmed present; use for DeepThinner plugin list entry

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `icons/DeepThinner.png`: Already exists from Phase 7 — use directly in plugin list
- `FastNoise/LICENSE`: Full MIT license text ready to copy into THIRD_PARTY_LICENSES.md

### Established Patterns
- Plugin list format in README.md: `![](icon_url) [PluginName](wiki_url)` — replicate this for DeepThinner entry, substituting GitHub URL for wiki URL
- All other plugins link to `https://github.com/charlesangus/DeepC/wiki/PluginName`

### Integration Points
- README.md TOC links to sections by anchor — any section renames need corresponding TOC updates
- DeepThinner copyright info lives in `src/DeepThinner.cpp` lines 1–2

</code_context>

<specifics>
## Specific Ideas

- DeepCShuffle naming: Nuke convention — internal class gets numeral (DeepCShuffle2), display name to users stays "DeepCShuffle". Old DeepCShuffle is deprecated; the entry represents DeepCShuffle2.
- THIRD_PARTY_LICENSES.md should cover all vendored libs, not just DeepThinner (FastNoise too).

</specifics>

<deferred>
## Deferred Ideas

- GitHub wiki update — User asked about updating the wiki to reflect current state (DeepThinner, docker-build.sh, etc.). This is a valid follow-on task but out of scope for Phase 8, which is scoped to in-repo files. Consider as Phase 8.1 or a standalone task in the next milestone.

</deferred>

---

*Phase: 08-documentation-overhaul*
*Context gathered: 2026-03-19*
