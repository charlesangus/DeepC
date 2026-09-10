#!/usr/bin/env python3
"""PreToolUse(Bash) hook: refuse heavy commands that are not under hostguard.

The container this repo is worked in has no cgroup limits, so an unguarded
build or headless Nuke render competes with the user's desktop for the whole
4-core / 15 GB host and can thrash it into a hard reboot.  scripts/hostguard.sh
caps the damage, but only if it is actually used -- hence this hook, which
blocks the small set of commands that are known to be heavy.

Blocking is by exit code 2: the message on stderr goes back to the model, which
re-runs the command through the guard.  Every other outcome, including any bug
in here, exits 0 and lets the command through: a broken guard must not be able
to wedge a session.
"""

import json
import os
import re
import shlex
import sys

GUARD = "scripts/hostguard.sh"

# argv[0] basenames that are heavy enough to be worth a wrapper.  Single-file
# compiles (g++ foo.cpp) and cmake CONFIGURE runs are cheap and stay off it.
HEAVY = {"make", "ninja", "ctest", "run_validation.sh", "run_profile.sh",
         "batchInstall.sh", "docker-build.sh"}
NUKE = re.compile(r"^Nuke[0-9.]*v?[0-9]*$")

# Split on the shell operators that start a new command, keeping it simple:
# anything subtler than this is rare enough that a miss just means the command
# runs unguarded, which is the status quo, not a regression.
SPLIT = re.compile(r"\|\||&&|[;|&\n]")
ASSIGN = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*=")


def heavyIn(segment):
    """The heavy command in this segment, or None."""
    try:
        words = shlex.split(segment)
    except ValueError:
        return None
    while words and (ASSIGN.match(words[0]) or words[0] in ("exec", "command", "nohup", "time")):
        words.pop(0)
    if not words:
        return None
    base = os.path.basename(words[0].rstrip("\\"))
    if base == "cmake":
        return "cmake --build" if "--build" in words else None
    if base in HEAVY or NUKE.match(base):
        return base
    return None


def main():
    payload = json.load(sys.stdin)
    if payload.get("tool_name") != "Bash":
        return 0
    command = payload.get("tool_input", {}).get("command", "")
    if not command or GUARD in command or "hostguard" in command:
        return 0

    found = [h for h in (heavyIn(s) for s in SPLIT.split(command)) if h]
    if not found:
        return 0

    sys.stderr.write(
        "hostguard: blocked `%s` -- it is heavy enough to thrash this host, which has no\n"
        "cgroup limits and is shared with the user's desktop session.\n"
        "\n"
        "Re-run it through the guard, which caps it at 2 jobs and a quarter of RAM and\n"
        "kills it before the machine loses interactivity:\n"
        "\n"
        "    %s -- <the same command>\n"
        "\n"
        "Pass --jobs N / --mem-gb F if this particular run genuinely needs more, or\n"
        "--no-watchdog to keep the caps without the policing.\n"
        % (found[0], GUARD)
    )
    return 2


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception:
        # Fail open, always.
        sys.exit(0)
