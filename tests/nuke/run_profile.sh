#!/usr/bin/env bash
#
# One-command re-run of the DeepCDefocus headless perf profile.
#
#   tests/nuke/run_profile.sh                       # the 2K / 20 spp baseline
#   tests/nuke/run_profile.sh --reps 9 --stats
#   tests/nuke/run_profile.sh --threads 1           # forced-serial comparison
#   tests/nuke/run_profile.sh --variant source      # the scene WITHOUT the node
#
# Environment, as for run_validation.sh:
#
#   DEEPC_NUKE        path to the Nuke executable
#   DEEPC_PLUGIN_DIR  directory holding the built DeepC .so files
#
# DEEPC_PLUGIN_DIR IS WORTH SETTING EXPLICITLY.  The fallback below picks the
# NEWEST DeepCDefocus.so under build/*/src, and a scratch configure directory
# holding only that one plugin will win it while providing none of the other
# DeepC nodes the scenes need.
#
# --threads N is this wrapper's own option (Nuke's -m); everything else is
# forwarded to profile_defocus.py.

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
    exit 2
fi

threads=""
args=()
while (( $# )); do
    if [[ "$1" == "--threads" ]]; then
        threads="$2"
        shift 2
        continue
    elif [[ "$1" == --threads=* ]]; then
        threads="${1#--threads=}"
        shift
        continue
    fi
    # Nuke's terminal front-end eats a bare INTEGER argument as a frame range
    # before Python sees it, so every `--option value` pair is folded into the
    # `--option=value` form the script requires.
    if [[ "$1" == --* && "$1" != *=* && $# -ge 2 && "$2" != -* ]]; then
        args+=("$1=$2")
        shift 2
    else
        args+=("$1")
        shift
    fi
done

echo "nuke:     ${nukeBin}"
echo "plugin:   ${pluginDir} (DeepCDefocus.so built $(date -r "${pluginDir}/DeepCDefocus.so" '+%Y-%m-%d %H:%M:%S'))"
echo "cores:    $(nproc)${threads:+  (nuke -m ${threads})}"

nukeArgs=()
if [[ -n "${threads}" ]]; then
    nukeArgs+=(-m "${threads}")
fi

NUKE_PATH="${pluginDir}${NUKE_PATH:+:${NUKE_PATH}}" \
    exec "${nukeBin}" ${nukeArgs[@]+"${nukeArgs[@]}"} \
         -t "${scriptDir}/profile_defocus.py" \
         ${args[@]+"${args[@]}"}
