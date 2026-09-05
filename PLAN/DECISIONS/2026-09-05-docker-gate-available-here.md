# The docker gate still cannot run here — the daemon came up, the registry did not

**2026-09-05.** The plan's `# Context and constraints` said docker was installed but its daemon
was not running here. Half of that is now stale: the daemon IS running (`/var/run/docker.sock`
present, server 29.7.2). The conclusion is unchanged anyway, for a different and more stubborn
reason, and this file exists so nobody re-tests the daemon and concludes the gate is available.

The user asked for the NukeDockerBuild images to be built here and `./docker-build.sh --linux`
run from this session. Attempting it produced two hard blockers:

1. **Docker Hub's blob CDN is unroutable from this machine.** `registry-1.docker.io` answers
   (401, the normal unauthenticated response), but every blob fetch goes to
   `production.cloudfront.docker.com`, which fails `connect: no route to host`. This is NOT a
   sandbox restriction — a bare `docker pull hello-world` outside the sandbox fails identically.
   No image can be pulled here, so no image can be built here either: NukeDockerBuild's `build.sh`
   itself needs `docker:dind`, which is the pull that failed first.

2. **NukeDockerBuild has no Dockerfile for Nuke 16.1 or 17.0.** Its `dockerfiles/` tree stops at
   `16.0`. `docker-build.sh` iterates `NUKE_VERSIONS=(16.0 16.1 17.0)` and its `ensure_image()`
   prints `SKIP: NukeDockerBuild has no Dockerfile for Nuke <v> <platform> yet` for the latter
   two. So the plan's description of the docker build as "the exact production toolchain across
   all three Nuke minor versions" overstates what that gate can currently deliver — wherever it
   is run, today it covers **16.0 only**, until upstream adds the newer Dockerfiles.

**Consequence:** the docker-build gate reverts to the user's machine or CI before the M1 PR merges,
exactly as the original constraint had it, and whoever runs it should expect 16.0 coverage only.
The local Nuke SDK build (`-D Nuke_ROOT=/usr/local/Nuke17.0v3`, and the 16.0/16.1 SDKs beside it)
remains this environment's compile gate, and it covers all three minor versions — which is more
version coverage than the docker path currently offers, though with this machine's toolchain
rather than the release one.
