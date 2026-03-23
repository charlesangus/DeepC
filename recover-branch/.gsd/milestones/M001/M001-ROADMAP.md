# M001: DeepC v1.0–v1.2

**Vision:** DeepC is an open-source suite of deep-compositing Nuke NDK plugins targeting Foundry Nuke 16+. v1.2 ships 23 plugins covering color, matte, noise, shuffle, utility, and deep-sample optimisation operations on deep images.

## Success Criteria

- All 23 plugins build and load cleanly in Nuke 16+
- DeepCShuffle2 provides full colored channel routing UI
- DeepCPMatte provides interactive 3D wireframe GL handles
- DeepCPNoise supports true 4D noise evolution across all 9 noise types
- DeepThinner vendored and integrated into the build/menu system
- docker-build.sh produces Linux and Windows release archives
- README and licensing documentation fully up to date


## Slices

- [x] **S01: Codebase Sweep** `risk:medium` `depends:[]`
  > All deep-pixel math bugs fixed, NDK APIs modernized for Nuke 16+, DeepCBlink removed, Op::Description removal pattern established. Completed 2026-03-13.
- [x] **S02: DeepCPMatte GL Handles** `risk:medium` `depends:[S01]`
  > Deadlock-free 3D wireframe viewport handle with interactive drag-to-reposition for DeepCPMatte. Completed 2026-03-14.
- [x] **S03: DeepShuffle UI** `risk:medium` `depends:[S02]`
  > DeepCShuffle2 with full colored channel routing UI — ChannelSet pickers, radio enforcement, const:0/1, identity routing. Completed 2026-03-15.
- [x] **S04: Refine and Fix DeepCShuffle UI/Behaviour** `risk:medium` `depends:[S03]`
  > UAT-driven fixes to DeepCShuffle2 — reverted QComboBox to NDK ChannelSet knobs, fixed edge cases. Completed 2026-03-16.
- [x] **S05: DeepCPNoise 4D** `risk:medium` `depends:[S04]`
  > noise_evolution wired as w dimension unconditionally for all 9 noise types via 4D GetNoise dispatch. Completed 2026-03-17.
- [x] **S06: Release Cleanup** `risk:medium` `depends:[S05]`
  > 22 plugins shipped, silent backward-compat .so pattern for DeepCShuffle, SWEEP-07/08 scoped out. Completed 2026-03-17.
- [x] **S07: Local Build System** `risk:medium` `depends:[S06]`
  > docker-build.sh orchestration for Linux and Windows builds via NukeDockerBuild. Completed 2026-03-18.
- [x] **S08: Deepthinner Integration** `risk:medium` `depends:[S07]`
  > DeepThinner vendored into src/, wired into CMake and menu.py.in, local build verified. Completed 2026-03-19.
- [x] **S09: Documentation Overhaul** `risk:medium` `depends:[S08]`
  > README overhauled with 23-plugin list, docker-build.sh instructions; THIRD_PARTY_LICENSES.md created. Completed 2026-03-19.
