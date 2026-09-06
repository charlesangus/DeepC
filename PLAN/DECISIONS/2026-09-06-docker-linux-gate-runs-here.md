# The Linux docker gate DOES run here — via mirror.gcr.io and the local Nuke SDK

**2026-09-06.** This **supersedes blocker #1 of
[2026-09-05-docker-gate-available-here.md](2026-09-05-docker-gate-available-here.md)**, which
concluded "no image can be pulled here, so no image can be built here either." That inference was
too broad. `./docker-build.sh --linux` now runs unmodified on this machine and produces
`release/DeepC-Linux-Nuke16.0.zip` with all **28** plugins including `DeepCDefocus.so`, built by
GCC 11.2.1 (gcc-toolset-11) against the Nuke 16.0v9 SDK: **0 errors, 47 warnings, none of them in
`DeepCDefocus*`**, and the resulting module loads in Nuke 16.0v9.

What was actually blocked, and what routes around each:

1. **Docker Hub's blob CDN** (`production.cloudfront.docker.com`) is unroutable — that part of the
   2026-09-05 finding stands. But **`mirror.gcr.io` serves Docker Hub and IS routable.** Pull the
   base from there and retag it to the name the Dockerfile asks for
   (`docker tag mirror.gcr.io/rockylinux:8 docker.io/rockylinux:8`); BuildKit then resolves
   `FROM docker.io/rockylinux:8` out of the local store and never contacts the CDN. NukeDockerBuild's
   `docker:dind` wrapper is likewise avoidable by invoking `docker build` on its Dockerfile directly.
2. **Every Rocky 8 package mirror is unroutable** (17 probed, including `dl.`/`download.`/
   `download.cf.rockylinux.org`, kernel.org, MIT, Berkeley, RWTH Aachen, aliyun, plus
   `mirror.stream.centos.org` and `vault.centos.org`). `repo.almalinux.org` **is** routable, and
   AlmaLinux 8 is the same RHEL 8 rebuild shipping the identical compiler
   (`gcc-toolset-11-gcc 11.2.1-9.2.el8_6.alma.1`). Swapping the base is the one substantive
   deviation from upstream's Dockerfile. Minor second deviation: Alma 8 has no `cmake3` package, so
   `cmake` (3.26.5) is used; DeepC needs ≥3.15.
3. **`thefoundry.s3.amazonaws.com` is unroutable**, so NukeDockerBuild cannot download the Nuke
   installer — supply the local `/usr/local/Nuke16.0v9` install instead, as a BuildKit named context
   (`--build-context nukesdk=...` plus `COPY --from=nukesdk . /usr/local/nuke_install`), which does a
   real `COPY` without staging a 13.7 GB copy under `/tmp`.
4. **`zip` is not installed and cannot be** (no root). `docker-build.sh` hard-requires it. Worked
   around with a `python3 -m zipfile` shim on `PATH`; see the follow-up below.

**Blocker #2 of the 2026-09-05 file is unchanged and still stands:** NukeDockerBuild ships no
Dockerfile past 16.0, so `./docker-build.sh --linux` prints `SKIP` for 16.1 and 17.0 and the gate
covers **16.0 only** wherever it runs. The same gcc-11 image was verified by hand to build all 28
plugins cleanly against the 16.1 and 17.0 SDKs, so extending coverage is a ~15-minute image build
per version if anyone wants it.

**Windows remains genuinely impossible here.** It needs the Windows Nuke SDK zip from the same
unroutable Foundry host, and only Linux installs exist on this machine.

**Follow-up worth taking (not done — outside M1's scope):** `docker-build.sh`'s `command -v zip`
precondition is the only step that needs a `PATH` shim, and its sole use is `zip -r <archive> DeepC`.
Replacing it with `python3 -m zipfile -c <archive> DeepC` — `python3` is already a de facto
dependency of the repo's test tooling — would make this gate reproducible here with no `PATH`
manipulation at all.
