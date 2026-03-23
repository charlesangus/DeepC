# docker/windows.Dockerfile
#
# Extends a NukeDockerBuild Windows cross-compilation image with Qt 6.5.3.
#
# Two Qt installations are needed for AUTOMOC cross-compilation:
#   - gcc_64   (Linux): provides host tools — moc, uic, rcc — run on the build machine
#   - msvc2019_64 (Windows): provides headers and import libs linked into the DLL
#
# CMake's QT_HOST_PATH is pointed at the Linux install so AUTOMOC uses the Linux
# moc binary. CMAKE_PREFIX_PATH is pointed at the Windows install for libraries.
#
# Usage (called automatically by docker-build.sh):
#   docker build --build-arg NUKE_VERSION=16.0 -t deepc-build:16.0-windows -f docker/windows.Dockerfile .

ARG NUKE_VERSION=16.0
FROM nukedockerbuild:${NUKE_VERSION}-windows

ARG QT_VERSION=6.5.3

RUN apt-get update && \
    apt-get install -y --no-install-recommends python3-pip && \
    pip3 install --break-system-packages aqtinstall && \
    aqt install-qt linux   desktop ${QT_VERSION} gcc_64           --outputdir /opt/Qt && \
    aqt install-qt windows desktop ${QT_VERSION} win64_msvc2019_64 --outputdir /opt/Qt && \
    pip3 uninstall -y --break-system-packages aqtinstall && \
    rm -rf /var/lib/apt/lists/*
