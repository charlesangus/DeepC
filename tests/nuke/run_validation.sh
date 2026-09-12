#!/usr/bin/env bash
#
# One-command re-run of the DeepCDefocus headless validation harness.
#
#   tests/nuke/run_validation.sh                       # scenes (a)-(n), defaults
#   tests/nuke/run_validation.sh --scenes g,h --k 64
#   tests/nuke/run_validation.sh --pre-merge off
#   tests/nuke/run_validation.sh --threads 2           # cap Nuke's core count
#
# Nothing here is tied to one machine: the Nuke executable and the plugin
# directory come from the environment, with a search over the usual locations
# as the fallback.
#
#   DEEPC_NUKE        path to the Nuke executable
#                     (default: the highest-versioned /usr/local/Nuke*/Nuke*)
#   DEEPC_PLUGIN_DIR  directory holding the built DeepC .so files
#                     (default: the newest <repo>/build/*/src containing
#                      DeepCDefocus.so; falls back to $NUKE_PATH)
#
# Exits non-zero if any scene fails its stated check.

set -euo pipefail

scriptDir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repoRoot="$(cd "${scriptDir}/../.." && pwd)"

nukeBin="${DEEPC_NUKE:-}"
if [[ -z "${nukeBin}" ]]; then
    for candidate in $(ls -d /usr/local/Nuke*/ 2>/dev/null | sort -Vr); do
        exe="$(ls "${candidate}"Nuke*.* 2>/dev/null | grep -E 'Nuke[0-9.]+v?[0-9]*$' | head -n 1 || true)"
        if [[ -n "${exe}" && -x "${exe}" ]]; then
            nukeBin="${exe}"
            break
        fi
    done
fi
if [[ -z "${nukeBin}" || ! -x "${nukeBin}" ]]; then
    echo "error: no Nuke executable found; set DEEPC_NUKE" >&2
    exit 2
fi

pluginDir="${DEEPC_PLUGIN_DIR:-}"
if [[ -z "${pluginDir}" ]]; then
    # Newest DeepCDefocus.so wins. Alphabetical/version order silently
    # validates a stale tree — this repo already carries a build/local-*-O3
    # tree three weeks older than HEAD, and every number in the report would
    # have described that plugin instead of the one just built.
    newest=""
    while IFS= read -r so; do
        [[ -n "${so}" ]] || continue
        if [[ -z "${newest}" || "${so}" -nt "${newest}" ]]; then
            newest="${so}"
        fi
    done < <(ls -1 "${repoRoot}"/build/*/src/DeepCDefocus.so 2>/dev/null || true)
    if [[ -n "${newest}" ]]; then
        pluginDir="$(dirname "${newest}")"
    fi
fi
if [[ -z "${pluginDir}" ]]; then
    pluginDir="${NUKE_PATH:-}"
fi
if [[ -z "${pluginDir}" || ! -f "${pluginDir}/DeepCDefocus.so" ]]; then
    echo "error: no directory with DeepCDefocus.so found; set DEEPC_PLUGIN_DIR" >&2
    echo "       (build it with: cmake -S . -B build/local-17.0 -D Nuke_ROOT=<sdk> \\" >&2
    echo "        && scripts/hostguard.sh -- cmake --build build/local-17.0)" >&2
    echo "       -j8 on this 4-core host is what thrashes it; the guard caps the job." >&2
    exit 2
fi

echo "nuke:   ${nukeBin}"
# The .so's timestamp is part of the result: a report against a stale plugin
# is worse than no report.
echo "plugin: ${pluginDir} (DeepCDefocus.so built $(date -r "${pluginDir}/DeepCDefocus.so" '+%Y-%m-%d %H:%M:%S'))"

# Nuke's terminal front-end eats a bare INTEGER argument as a frame range
# before Python sees it, so `--k 64` reaches run_validation.py as just `--k`
# ("--k needs a value"), and `--max-radius 40` the same. Fold every
# `--option value` pair into `--option=value`, which it forwards intact.
# Bare flags (--strict, --keep-renders, --list) pass through untouched.
threads=""
args=()
while (( $# )); do
    # --threads N is this wrapper's own option (Nuke's -m), not the runner's.
    # Without it a validation run takes every core on the box.
    if [[ "$1" == "--threads" ]]; then
        threads="$2"
        shift 2
        continue
    elif [[ "$1" == --threads=* ]]; then
        threads="${1#--threads=}"
        shift
        continue
    fi
    if [[ "$1" == --* && "$1" != *=* && $# -ge 2 && "$2" != -* ]]; then
        args+=("$1=$2")
        shift 2
    else
        args+=("$1")
        shift
    fi
done

nukeArgs=()
if [[ -n "${threads}" ]]; then
    nukeArgs+=(-m "${threads}")
fi

NUKE_PATH="${pluginDir}${NUKE_PATH:+:${NUKE_PATH}}" \
    exec "${nukeBin}" ${nukeArgs[@]+"${nukeArgs[@]}"} \
         -t "${scriptDir}/run_validation.py" \
         ${args[@]+"${args[@]}"}
