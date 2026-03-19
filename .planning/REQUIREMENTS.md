# Requirements: DeepC

**Defined:** 2026-03-18
**Core Value:** Artists can use deep-compositing operations in Nuke that don't exist in the built-in toolset, with professional-quality UIs and reliable deep-pixel math.

## v1.2 Requirements

### DeepThinner Integration

- [ ] **THIN-01**: DeepThinner.cpp vendored into `src/` with upstream MIT license header comment preserved
- [ ] **THIN-02**: DeepThinner added to CMake `PLUGINS` list and builds as a `.so`/`.dll`
- [ ] **THIN-03**: DeepThinner wired into `menu.py.in` for Deep toolbar registration
- [ ] **THIN-04**: DeepThinner builds successfully via `docker-build.sh` for Linux and Windows

### Documentation

- [ ] **DOCS-01**: README.md updated with accurate plugin list (23 plugins including DeepThinner with attribution and link to https://github.com/bratgot/DeepThinner)
- [ ] **DOCS-02**: README.md build section replaced with current `docker-build.sh` workflow, Nuke 16+ target, outdated CentOS/VS2017/batchInstall.sh content removed
- [ ] **DOCS-03**: README.md cleaned of removed/dead content (DeepCBlink, DeepCompress "coming soon")
- [ ] **DOCS-04**: `THIRD_PARTY_LICENSES.md` created documenting DeepThinner's MIT license with attribution to Marten Blumen

## Future Requirements

### Plugins

- **PLUG-01**: DeepDefocus plugin (deferred — CPU-first implementation)
- **PLUG-02**: DeepBlur plugin (deferred)

## Out of Scope

| Feature | Reason |
|---------|--------|
| Adapt DeepThinner to DeepCWrapper/DeepCMWrapper base classes | Not requested; adds risk with no user-visible benefit |
| DeepThinner modifications beyond vendoring | Upstream project is stable; changes should be contributed upstream |
| GL handles for DeepCPNoise or DeepCWorld | Not requested |
| GitHub Actions CI/CD | Shelved; NukeDockerBuild local builds cover the need |
| macOS support | Not actively maintained |

## Traceability

Which phases cover which requirements. Updated during roadmap creation.

| Requirement | Phase | Status |
|-------------|-------|--------|
| THIN-01 | — | Pending |
| THIN-02 | — | Pending |
| THIN-03 | — | Pending |
| THIN-04 | — | Pending |
| DOCS-01 | — | Pending |
| DOCS-02 | — | Pending |
| DOCS-03 | — | Pending |
| DOCS-04 | — | Pending |

**Coverage:**
- v1.2 requirements: 8 total
- Mapped to phases: 0
- Unmapped: 8 ⚠️

---
*Requirements defined: 2026-03-18*
*Last updated: 2026-03-18 after initial definition*
