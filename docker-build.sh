#!/bin/bash
set -euo pipefail

# docker-build.sh — Build DeepC for Nuke using NukeDockerBuild Docker images.
#
# Builds DeepC for all target Nuke versions on Linux and/or Windows using
# pre-built NukeDockerBuild containers. No local Nuke SDK installation required.
#
# Prerequisites: docker, zip, git
# NukeDockerBuild images are built automatically on first use (requires ~1 GB
# Nuke installer download and acceptance of Foundry's EULA).
#
# Usage: docker-build.sh [OPTIONS]
#   --versions VER[,VER...]   Override default Nuke versions (default: 16.0)
#   --linux                    Build Linux artifacts only
#   --windows                  Build Windows artifacts only
#   --all                      Build both platforms (default)
#   -h, --help                 Show this help

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------

NUKE_VERSIONS=("16.0")

BUILD_LINUX=true
BUILD_WINDOWS=true

NUKEDOCKERBUILD_IMAGE="nukedockerbuild"
NUKEDOCKERBUILD_REPO="https://github.com/gillesvink/NukeDockerBuild"

# Path to the Nuke SDK inside every NukeDockerBuild container.
# FindNuke.cmake uses NO_SYSTEM_ENVIRONMENT_PATH, so CMAKE_PREFIX_PATH is
# NOT used automatically — Nuke_ROOT must be passed explicitly to CMake.
NUKE_SDK_PATH="/usr/local/nuke_install"

# ---------------------------------------------------------------------------
# Usage
# ---------------------------------------------------------------------------

usage() {
    cat <<EOF
Usage: docker-build.sh [OPTIONS]

Build DeepC for Nuke using NukeDockerBuild Docker images.
No local Nuke SDK installation is required.

OPTIONS:
  --versions VER[,VER...]   Override default Nuke versions (default: 16.0)
                            Comma-separated, e.g. --versions 16.0
  --linux                    Build Linux artifacts only (.so)
  --windows                  Build Windows artifacts only (.dll)
  --all                      Build both platforms (default)
  -h, --help                 Show this help and exit

OUTPUT:
  release/DeepC-Linux-Nuke<version>.zip
  release/DeepC-Windows-Nuke<version>.zip
EOF
}

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------

while [[ $# -gt 0 ]]; do
    case "$1" in
        --versions)
            IFS=',' read -ra NUKE_VERSIONS <<< "$2"
            shift 2
            ;;
        --linux)
            BUILD_LINUX=true
            BUILD_WINDOWS=false
            shift
            ;;
        --windows)
            BUILD_LINUX=false
            BUILD_WINDOWS=true
            shift
            ;;
        --all)
            BUILD_LINUX=true
            BUILD_WINDOWS=true
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown flag: $1" >&2
            echo "Run 'docker-build.sh --help' for usage." >&2
            exit 1
            ;;
    esac
done

# ---------------------------------------------------------------------------
# Prerequisite checks
# ---------------------------------------------------------------------------

if ! command -v docker &>/dev/null; then
    echo "ERROR: docker is not installed or not in PATH" >&2
    exit 1
fi

if ! command -v zip &>/dev/null; then
    echo "ERROR: zip is not installed or not in PATH" >&2
    exit 1
fi

if ! command -v git &>/dev/null; then
    echo "ERROR: git is not installed or not in PATH" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Image bootstrap
# ---------------------------------------------------------------------------

# Ensures nukedockerbuild:<version>-<platform> exists locally.
# If not, clones NukeDockerBuild and builds it automatically.
ensure_image() {
    local nuke_version="$1"
    local platform="$2"
    local image_tag="${NUKEDOCKERBUILD_IMAGE}:${nuke_version}-${platform}"

    if docker image inspect "${image_tag}" &>/dev/null; then
        return 0
    fi

    echo "  Image '${image_tag}' not found locally — building via NukeDockerBuild..."
    echo "  NOTE: Building requires acceptance of Foundry's EULA."
    echo "  NOTE: Nuke installer will be downloaded automatically (~1 GB)."

    local build_tmp_dir
    build_tmp_dir="$(mktemp -d)"
    trap 'rm -rf "${build_tmp_dir}"' EXIT

    echo "  Cloning ${NUKEDOCKERBUILD_REPO} into ${build_tmp_dir}..."
    git clone --depth 1 "${NUKEDOCKERBUILD_REPO}" "${build_tmp_dir}"

    echo "  Building ${image_tag} (this may take 10–30 minutes)..."
    bash "${build_tmp_dir}/build.sh" "${nuke_version}" "${platform}"

    if ! docker image inspect "${image_tag}" &>/dev/null; then
        echo "ERROR: NukeDockerBuild finished but '${image_tag}' was not found in local Docker images." >&2
        exit 1
    fi

    echo "  Image '${image_tag}' ready."
}

# ---------------------------------------------------------------------------
# Directory setup
# ---------------------------------------------------------------------------

BASEDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "${BASEDIR}/build" "${BASEDIR}/install" "${BASEDIR}/release"

# ---------------------------------------------------------------------------
# Build loop
# ---------------------------------------------------------------------------

for nuke_version in "${NUKE_VERSIONS[@]}"; do
    echo "=== Building DeepC for Nuke ${nuke_version} ==="

    # -----------------------------------------------------------------------
    # Linux build
    # -----------------------------------------------------------------------

    if [[ "${BUILD_LINUX}" == "true" ]]; then
        ensure_image "${nuke_version}" "linux"
        echo "  [Linux] Starting build..."

        docker run --rm \
            -v "${BASEDIR}:/nuke_build_directory" \
            "${NUKEDOCKERBUILD_IMAGE}:${nuke_version}-linux" \
            bash -c "
                cmake -S /nuke_build_directory \
                      -B /nuke_build_directory/build/${nuke_version}-linux \
                      -D CMAKE_INSTALL_PREFIX=/nuke_build_directory/install/${nuke_version}-linux \
                      -D Nuke_ROOT=${NUKE_SDK_PATH} &&
                cmake --build /nuke_build_directory/build/${nuke_version}-linux --config Release &&
                cmake --install /nuke_build_directory/build/${nuke_version}-linux
            "

        zip -r "${BASEDIR}/release/DeepC-Linux-Nuke${nuke_version}.zip" "${BASEDIR}/install/${nuke_version}-linux"
        echo "  Linux build complete: release/DeepC-Linux-Nuke${nuke_version}.zip"
    fi

    # -----------------------------------------------------------------------
    # Windows build (cross-compiled from Linux using wine-msvc in container)
    # -----------------------------------------------------------------------

    if [[ "${BUILD_WINDOWS}" == "true" ]]; then
        ensure_image "${nuke_version}" "windows"
        echo "  [Windows] Starting build..."

        # NOTE: \$GLOBAL_TOOLCHAIN is intentionally escaped so it is NOT expanded
        # by the host shell. GLOBAL_TOOLCHAIN is a container-only env var pointing
        # to the wine-msvc toolchain file inside the Windows NukeDockerBuild image.
        docker run --rm \
            -v "${BASEDIR}:/nuke_build_directory" \
            "${NUKEDOCKERBUILD_IMAGE}:${nuke_version}-windows" \
            bash -c "
                cmake -S /nuke_build_directory \
                      -B /nuke_build_directory/build/${nuke_version}-windows \
                      -GNinja \
                      -DCMAKE_SYSTEM_NAME=Windows \
                      -DCMAKE_BUILD_TYPE=Release \
                      -DCMAKE_TOOLCHAIN_FILE=\$GLOBAL_TOOLCHAIN \
                      -D CMAKE_INSTALL_PREFIX=/nuke_build_directory/install/${nuke_version}-windows \
                      -D Nuke_ROOT=${NUKE_SDK_PATH} &&
                cmake --build /nuke_build_directory/build/${nuke_version}-windows --config Release &&
                cmake --install /nuke_build_directory/build/${nuke_version}-windows
            "

        zip -r "${BASEDIR}/release/DeepC-Windows-Nuke${nuke_version}.zip" "${BASEDIR}/install/${nuke_version}-windows"
        echo "  Windows build complete: release/DeepC-Windows-Nuke${nuke_version}.zip"
    fi

done

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

echo ""
echo "=== Build complete ==="
echo "Archives in: ${BASEDIR}/release/"
ls -la "${BASEDIR}/release/"*.zip 2>/dev/null || echo "  (no archives produced)"
