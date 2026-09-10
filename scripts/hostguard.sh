#!/usr/bin/env bash
#
# Run a heavy command without taking the host down with it.
#
#   scripts/hostguard.sh -- cmake --build build/local-17.0
#   scripts/hostguard.sh -- tests/nuke/run_validation.sh
#   scripts/hostguard.sh --jobs 3 --mem-gb 6 -- tests/nuke/run_profile.sh --reps 9
#
# Why this exists: the Bash tool runs in a container with NO cgroup limits, on
# a 4-core / 15 GB host shared with the user's desktop session.  An unguarded
# `cmake --build -j8` (what the docs used to say) is 8 C++ compiles on 4 cores;
# a headless Nuke deep render defaults to every core plus a cache sized from
# total RAM.  Either one can drive the machine into swap thrash, which on Linux
# reads as "extreme lag" and ends in a hard reboot.
#
# No root, no user D-Bus, so a real cgroup cap is off the table.  What is
# enforceable from userspace, and what this does:
#
#   nice + ionice idle   the job never wins CPU or disk against the desktop
#   choom -n 1000        if the kernel OOM-kills anything, it kills THIS job
#   concurrency cap      -j / -m default 2, not nproc
#   watchdog             kills the job before the host starts thrashing
#
# Options (all optional; every one has a polite default):
#
#   --jobs N        parallelism for the job          (default: 2)
#   --mem-gb F      RSS budget for the process tree  (default: MemTotal / 4)
#   --floor-gb F    system MemAvailable floor        (default: 2.0)
#   --nice N        niceness                         (default: 19)
#   --poll S        watchdog sample interval, s      (default: 2)
#   --no-watchdog   run the caps but do not police the job
#
# Exit status is the command's own, except 137 when the watchdog killed it.

set -uo pipefail

jobs=2
memGb=""
floorGb=2.0
niceness=19
poll=2
watchdog=1

while (( $# )); do
    case "$1" in
        --jobs)         jobs="$2"; shift 2 ;;
        --jobs=*)       jobs="${1#*=}"; shift ;;
        --mem-gb)       memGb="$2"; shift 2 ;;
        --mem-gb=*)     memGb="${1#*=}"; shift ;;
        --floor-gb)     floorGb="$2"; shift 2 ;;
        --floor-gb=*)   floorGb="${1#*=}"; shift ;;
        --nice)         niceness="$2"; shift 2 ;;
        --nice=*)       niceness="${1#*=}"; shift ;;
        --poll)         poll="$2"; shift 2 ;;
        --poll=*)       poll="${1#*=}"; shift ;;
        --no-watchdog)  watchdog=0; shift ;;
        --)             shift; break ;;
        -h|--help)      sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)              break ;;
    esac
done

if (( $# == 0 )); then
    echo "hostguard: nothing to run (usage: $0 [options] -- <command>)" >&2
    exit 2
fi

memTotalGb="$(awk '/^MemTotal:/ {printf "%.2f", $2 / 1048576}' /proc/meminfo)"
if [[ -z "${memGb}" ]]; then
    # A quarter of the box.  Leaves the desktop, the browser and the rest of
    # the session the other three quarters even at the job's peak.
    memGb="$(awk -v t="${memTotalGb}" 'BEGIN {printf "%.2f", t / 4}')"
fi

# Polite defaults for anything that reads the usual environment. A build that
# honours MAKEFLAGS or CMAKE_BUILD_PARALLEL_LEVEL needs no -j on the command
# line; one that hardcodes -j8 overrides this, which is why the hook exists.
export MAKEFLAGS="-j${jobs}"
export CMAKE_BUILD_PARALLEL_LEVEL="${jobs}"
export OMP_NUM_THREADS="${jobs}"
export HOSTGUARD_JOBS="${jobs}"

# Nuke takes its thread count as `-m N` and will otherwise use every core.
# The two wrappers in tests/nuke/ forward --threads to it.
cmdBase="$(basename -- "$1")"
case "${cmdBase}" in
    run_validation.sh|run_profile.sh)
        if [[ " ${*:2} " != *" --threads"* ]]; then
            set -- "$@" "--threads=${jobs}"
        fi
        ;;
    Nuke*)
        if [[ " ${*:2} " != *" -m "* ]]; then
            set -- "$1" -m "${jobs}" "${@:2}"
        fi
        ;;
esac

printf 'hostguard: %d job(s), %.2f GB RSS budget, %.2f GB floor of %.2f GB total, nice %d\n' \
    "${jobs}" "${memGb}" "${floorGb}" "${memTotalGb}" "${niceness}" >&2
printf 'hostguard: running %s\n' "$*" >&2

# choom raises oom_score_adj, which any user may do (only LOWERING it needs
# privilege). It makes this process tree the kernel's first pick, so a genuine
# OOM takes the build and not the user's session.
launcher=(nice -n "${niceness}")
command -v ionice >/dev/null 2>&1 && launcher+=(ionice -c 3)
command -v choom  >/dev/null 2>&1 && launcher+=(choom -n 1000 --)

# setsid puts the job in its own process group: the watchdog can then account
# for, and signal, every descendant it spawns rather than just the top process.
setsid "${launcher[@]}" "$@" &
child=$!

killTree() {
    kill -TERM "-${child}" 2>/dev/null || kill -TERM "${child}" 2>/dev/null
}
trap 'echo "hostguard: interrupted, stopping the job" >&2; killTree; exit 130' INT TERM

peakGb=0
warned=0
killedReason=""

availGb() { awk '/^MemAvailable:/ {printf "%.2f", $2 / 1048576}' /proc/meminfo; }
treeGb()  { ps -eo pgid=,rss= 2>/dev/null | awk -v g="${child}" '$1 == g {s += $2} END {printf "%.2f", s / 1048576}'; }
psiFull() { awk '/^full/ {split($2, a, "="); print a[2]; exit}' /proc/pressure/memory 2>/dev/null || echo 0; }

if (( watchdog )); then
    while kill -0 "${child}" 2>/dev/null; do
        sleep "${poll}"
        kill -0 "${child}" 2>/dev/null || break

        rss="$(treeGb)"; avail="$(availGb)"; psi="$(psiFull)"
        awk -v a="${rss}" -v b="${peakGb}" 'BEGIN {exit !(a > b)}' && peakGb="${rss}"

        if (( ! warned )) && awk -v r="${rss}" -v m="${memGb}" 'BEGIN {exit !(r > 0.8 * m)}'; then
            printf 'hostguard: warning, job at %.2f GB of its %.2f GB budget\n' "${rss}" "${memGb}" >&2
            warned=1
        fi

        if awk -v r="${rss}" -v m="${memGb}" 'BEGIN {exit !(r > m)}'; then
            killedReason="$(printf 'job RSS %.2f GB exceeded the %.2f GB budget' "${rss}" "${memGb}")"
        elif awk -v a="${avail}" -v f="${floorGb}" 'BEGIN {exit !(a < f)}'; then
            killedReason="$(printf 'system MemAvailable fell to %.2f GB, below the %.2f GB floor' "${avail}" "${floorGb}")"
        elif awk -v p="${psi}" 'BEGIN {exit !(p > 10)}'; then
            # /proc/pressure/memory "full avg10" is the share of the last ten
            # seconds in which EVERY task stalled on memory. Above ~10% the
            # host is already thrashing; this is the signal that arrives before
            # the desktop stops redrawing.
            killedReason="$(printf 'host memory pressure (full avg10 %.2f%%) means it is already thrashing' "${psi}")"
        fi

        if [[ -n "${killedReason}" ]]; then
            printf 'hostguard: KILLING the job -- %s\n' "${killedReason}" >&2
            killTree
            for _ in 1 2 3 4 5 6 7 8 9 10; do
                kill -0 "${child}" 2>/dev/null || break
                sleep 1
            done
            kill -KILL "-${child}" 2>/dev/null
            wait "${child}" 2>/dev/null
            printf 'hostguard: peak job RSS %.2f GB. Re-run with a smaller --jobs, or --mem-gb if the budget was simply too tight.\n' "${peakGb}" >&2
            exit 137
        fi
    done
fi

wait "${child}"
status=$?
printf 'hostguard: finished, status %d, peak job RSS %.2f GB\n' "${status}" "${peakGb}" >&2
exit "${status}"
