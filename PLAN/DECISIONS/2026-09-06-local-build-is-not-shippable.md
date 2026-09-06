# The local dev build's binaries cannot load on RHEL 8 — the docker gate is not ceremony

**2026-09-06.** Found while proving the Linux docker gate runs here
([2026-09-06-docker-linux-gate-runs-here.md](2026-09-06-docker-linux-gate-runs-here.md)). This is a
**repo-wide release fact, not a `DeepCDefocus` one**, and it is the concrete argument for keeping
the docker gate rather than treating the local SDK build as equivalent.

The local build (Debian 12 / GCC 12.2, `-D Nuke_ROOT=/usr/local/Nuke17.0v3`) emits modules that
require **`GLIBC_2.29`**:

```
$ objdump -T build/local-17.0/src/DeepCDefocus.so | grep GLIBC_2.29
  (GLIBC_2.29) log
  (GLIBC_2.29) exp
```

Rocky/RHEL 8 — the platform Nuke 16 and 17 are supported on — ships **glibc 2.28**, so those modules
will not load there. Four plugins are affected: `DeepCDefocus.so`, `DeepCDepthBlur.so`,
`DeepCGamma.so`, `DeepCPMatte.so`. The container build resolves the same symbols at `GLIBC_2.27` and
tops out at `GLIBC_2.27` / `GLIBCXX_3.4.21`, so its output loads everywhere the local one does and on
RHEL 8 besides.

**Consequence:** the local Nuke SDK build is a **dev loop only**. It is the right compile gate for
day-to-day iteration and it is what every task in M1 used, but its binaries are not shippable and no
release artifact should ever be cut from it. That is what `./docker-build.sh` is for.

**A second, currently latent difference:** Qt6 is not found inside the container, so `CMAKE_AUTOMOC`
stays off there, while the local build finds `/opt/Qt/6.5.3/gcc_64` and turns it on. All 28 plugins
build either way because nothing in `src/` uses `Q_OBJECT` — but `find_package(Qt6 ...)` at
`src/CMakeLists.txt:86` is not `REQUIRED`, so it degrades silently. A future Qt-using knob would
compile locally and fail in the release image with no warning at configure time.
