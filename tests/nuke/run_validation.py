"""DeepCDefocus headless validation runner (M1.P3.T12, scenes g-l at T16).

Run it through Nuke's terminal interpreter:

    NUKE_PATH=<plugin dir> Nuke17.0 -t tests/nuke/run_validation.py [options]

or, more simply, through the wrapper that locates both for you:

    tests/nuke/run_validation.sh

Options (the sweep parameters are harness options because M1.P3.T16-T18
drove these same scenes at different settings; ``--combine`` went with the
bucket composite M1.P3.T17 deleted, and ``--holdout-interp`` with the
holdout interpolants M1.P3.T18 deleted after its rendered bake-off):

Both ``--option value`` and ``--option=value`` are accepted.  Through the
wrapper either form works; calling Nuke directly you must use ``=`` for any
INTEGER value (``--k=64``, ``--max-radius=40``), because Nuke's own terminal
argument parser consumes a bare integer as a frame range and never forwards
it to ``sys.argv``.

    --scenes a,b,...,m      which scenes to run          (default: all)
    --k N                   depth_layers                 (default: 16)
    --pre-merge on|off                                   (default: on)
    --merge-tolerance F                                  (default: 0.25)
    --max-radius N                                       (default: 100)
    --tmp-dir DIR           where renders go             (default: a temp dir)
    --keep-renders          do not delete the EXRs
    --full-sweep            widen scene (f)'s f3f unequal-density sweep to
                            the whole grid (~26s more; the default run keeps
                            representative cells on every axis)
    --strict                treat documented XFAILs as failures
    --list                  list the scenes and exit

Exit code is 0 when every selected scene passes its stated check, 1 when any
check FAILs.  Documented residuals are reported as XFAIL with their measured
magnitude and affected pixel population; --strict makes them fail too.
"""

import os
import shutil
import sys
import tempfile
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import (                                           # noqa: E402
    FAIL, PASS, SKIP, XFAIL,
    Check, Settings,
)
from scenes import SCENES                                       # noqa: E402


def parseArgs(argv):
    options = {
        "scenes": "abcdefghijklm",
        "k": 16,
        "preMerge": True,
        "mergeTolerance": 0.25,
        "maxRadius": 100,
        "tmpDir": None,
        "keepRenders": False,
        "fullSweep": False,
        "strict": False,
        "list": False,
    }
    i = 0
    while i < len(argv):
        arg = argv[i]
        # `--option=value` must be accepted, not just `--option value`: Nuke's
        # terminal front-end steals a bare INTEGER argument as a frame range
        # before Python is reached, so `-t run_validation.py --k 64` arrives as
        # just `['--k']` (with a warning on stderr).  run_validation.sh folds
        # pairs into the `=` form for exactly this reason; anyone invoking the
        # script through Nuke directly must use `--k=64` too.
        inline = None
        if arg.startswith("--") and "=" in arg:
            arg, inline = arg.split("=", 1)
        step = 1 if inline is not None else 2

        def nextValue():
            if inline is not None:
                return inline
            if i + 1 >= len(argv):
                raise SystemExit("%s needs a value (if you are calling Nuke "
                                 "directly, use %s=<value>)" % (arg, arg))
            return argv[i + 1]

        def noValue():
            if inline is not None:
                raise SystemExit("%s takes no value" % arg)

        if arg == "--scenes":
            options["scenes"] = nextValue().replace(",", "").strip()
            i += step
        elif arg == "--k":
            options["k"] = int(nextValue())
            i += step
        elif arg == "--pre-merge":
            value = nextValue()
            if value not in ("on", "off"):
                raise SystemExit("--pre-merge must be on or off")
            options["preMerge"] = (value == "on")
            i += step
        elif arg == "--merge-tolerance":
            options["mergeTolerance"] = float(nextValue())
            i += step
        elif arg == "--max-radius":
            options["maxRadius"] = int(nextValue())
            i += step
        elif arg == "--tmp-dir":
            options["tmpDir"] = nextValue()
            i += step
        elif arg == "--keep-renders":
            noValue()
            options["keepRenders"] = True
            i += 1
        elif arg == "--full-sweep":
            noValue()
            options["fullSweep"] = True
            i += 1
        elif arg == "--strict":
            noValue()
            options["strict"] = True
            i += 1
        elif arg == "--list":
            noValue()
            options["list"] = True
            i += 1
        elif arg in ("-h", "--help"):
            print(__doc__)
            raise SystemExit(0)
        else:
            raise SystemExit("unknown option %r (try --help)" % arg)
    return options


def printTable(checks):
    header = ("scene", "check", "measured", "gate", "status")
    widths = [len(h) for h in header]
    rows = []
    for check in checks:
        row = (check.scene, check.name, check.measured, check.gate,
               check.status)
        rows.append(row)
        for i, cell in enumerate(row):
            widths[i] = max(widths[i], len(str(cell)))

    line = "  ".join("-" * w for w in widths)
    print("  ".join(h.ljust(widths[i]) for i, h in enumerate(header)))
    print(line)
    for check, row in zip(checks, rows):
        print("  ".join(str(cell).ljust(widths[i])
                        for i, cell in enumerate(row)).rstrip())
        if check.population:
            print("      population: %s" % check.population)
        if check.note:
            print("      %s" % check.note)
    print(line)


def main(argv):
    options = parseArgs(argv)

    if options["list"]:
        for key in sorted(SCENES):
            print("%s  %s" % (key, SCENES[key][0]))
        return 0

    unknown = [s for s in options["scenes"] if s not in SCENES]
    if unknown:
        raise SystemExit("unknown scene(s): %s" % ", ".join(unknown))

    ownTmp = options["tmpDir"] is None
    tmpDir = options["tmpDir"] or tempfile.mkdtemp(prefix="deepc-validation-")
    if not os.path.isdir(tmpDir):
        os.makedirs(tmpDir)

    settings = Settings(k=options["k"],
                        preMerge=options["preMerge"],
                        mergeTolerance=options["mergeTolerance"],
                        maxRadius=options["maxRadius"],
                        tmpDir=tmpDir, keepRenders=options["keepRenders"],
                        fullSweep=options["fullSweep"])

    print("DeepCDefocus headless validation — scenes (a)-(m)")
    print("settings: %s" % settings.describe())
    print("renders:  %s%s" % (tmpDir,
                              "" if options["keepRenders"] else " (deleted)"))
    print("")

    checks = []
    for key in options["scenes"]:
        title, runner = SCENES[key]
        started = time.time()
        try:
            sceneChecks = runner(settings)
        except Exception:                                   # noqa: BLE001
            traceback.print_exc()
            sceneChecks = [Check(key, "%s — scene raised" % title, "-", "-",
                                 FAIL,
                                 note="see traceback above")]
        elapsed = time.time() - started
        print("scene (%s) %s — %d check(s) in %.1fs"
              % (key, title, len(sceneChecks), elapsed))
        checks.extend(sceneChecks)
    print("")

    printTable(checks)

    counts = {}
    for check in checks:
        counts[check.status] = counts.get(check.status, 0) + 1
    print("")
    print("summary: " + "  ".join("%s=%d" % (status, counts.get(status, 0))
                                  for status in (PASS, FAIL, XFAIL, SKIP)))

    if ownTmp and not options["keepRenders"]:
        shutil.rmtree(tmpDir, ignore_errors=True)

    failed = counts.get(FAIL, 0)
    if options["strict"]:
        failed += counts.get(XFAIL, 0)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
