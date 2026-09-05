# Local Nuke SDK as the dev-loop compile gate

This dev environment has no reachable docker daemon (`docker` binary is present but
`/var/run/docker.sock` doesn't exist), so `./docker-build.sh` — previously assumed to be the
only way to compile NDK-facing code — cannot run here. However, licensed Nuke SDK installs
already exist locally at `/usr/local/Nuke16.0v9`, `/usr/local/Nuke16.1v3`, and
`/usr/local/Nuke17.0v3`, each with full NDK headers and `libDDImage.so`. A local CMake build
(`cmake -S . -B build/local -D Nuke_ROOT=/usr/local/Nuke17.0v3 && cmake --build build/local`)
was verified during planning to configure and build the existing repo clean (warnings only).

Decision: Milestone 1 gets a new **Phase 1.0** that locks in and documents this local build as
the milestone's day-to-day compile/test gate in this environment. `docker-build.sh` remains the
pre-merge/release gate (exact production toolchain across all three Nuke minor versions, plus
the Windows cross-compile), to be run wherever docker is actually available (the user's machine
or CI), not treated as a per-task blocker in this environment. This corrects the board's prior
"no NDK headers exist on this machine" assumption, which is no longer true.
