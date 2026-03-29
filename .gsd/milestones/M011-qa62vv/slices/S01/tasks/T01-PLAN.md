---
estimated_steps: 16
estimated_files: 7
skills_used: []
---

# T01: Extract opendefocus crate sources and convert workspace to path dependencies

Download the four opendefocus 0.1.10 crates from static.crates.io, extract them to the crates/ directory, update workspace and crate Cargo.toml files to use path dependencies, delete the v4 Cargo.lock so cargo 1.75 regenerates a v3-compatible one, and confirm `cargo check -p opendefocus-deep` compiles.

The Cargo.lock in the worktree is version 4 which cargo 1.75 cannot parse. Deleting it and letting cargo regenerate it on the first `cargo check` produces a v3 lockfile, which is fine for this project.

The four forked crates already reference each other via internal path deps (`opendefocus-shared = { path = "../opendefocus-shared" }` etc.). After extracting them to `crates/`, these relative paths will be correct. The only change needed is in `crates/opendefocus-deep/Cargo.toml` (switch from registry dep to `path = "../opendefocus"`) and in the workspace `Cargo.toml` (add four new members).

Steps:
1. Download and extract the four crates:
   ```bash
   for crate in opendefocus-kernel opendefocus opendefocus-shared opendefocus-datastructure; do
     curl -L "https://static.crates.io/crates/$crate/$crate-0.1.10.crate" -o /tmp/$crate.crate
     tar xzf /tmp/$crate.crate -C crates/ && mv crates/$crate-0.1.10 crates/$crate
   done
   ```
2. In workspace `Cargo.toml`, add four members to the `[workspace]` members array: `"crates/opendefocus-kernel"`, `"crates/opendefocus"`, `"crates/opendefocus-shared"`, `"crates/opendefocus-datastructure"`.
3. In `crates/opendefocus-deep/Cargo.toml`, replace `opendefocus = { version = "0.1.10", features = ["std", "wgpu"] }` with `opendefocus = { path = "../opendefocus", features = ["std", "wgpu"] }`.
4. In each extracted crate's `Cargo.toml`, verify the inter-crate path references (e.g. `opendefocus-shared = { path = "../opendefocus-shared", version = "=0.1.10" }`) — they should already be correct if the crate used path deps upstream. If any still reference the registry, patch them to use `path = "../"` equivalents.
5. Delete the v4 `Cargo.lock`: `rm Cargo.lock`.
6. Run `cargo check -p opendefocus-deep 2>&1 | tail -5` — must show no errors.

## Inputs

- ``Cargo.toml``
- ``Cargo.lock``
- ``crates/opendefocus-deep/Cargo.toml``

## Expected Output

- ``crates/opendefocus-kernel/``
- ``crates/opendefocus/``
- ``crates/opendefocus-shared/``
- ``crates/opendefocus-datastructure/``
- ``Cargo.toml``
- ``crates/opendefocus-deep/Cargo.toml``
- ``Cargo.lock``

## Verification

cd /home/latuser/git/DeepC/.gsd/worktrees/M011-qa62vv && cargo check -p opendefocus-deep 2>&1 | grep -c '^error' | grep -q '^0$' && echo PASS
