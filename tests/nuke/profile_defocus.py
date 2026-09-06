"""Headless perf profile for DeepCDefocus — the project's regression baseline.

Run it through Nuke's terminal interpreter, or through ``run_profile.sh``
which locates Nuke and the plugin directory for you:

    NUKE_PATH=<plugin dir> Nuke17.0 -t tests/nuke/profile_defocus.py [options]

Every option must be written ``--option=value``: Nuke's terminal front-end
consumes a bare integer argument as a frame range before Python sees it, so
``--reps 5`` arrives as just ``['--reps']``.  ``run_profile.sh`` folds the
spaced form into the ``=`` form, exactly as ``run_validation.sh`` does.

    --width=N            frame width                          (default 2048)
    --height=N           frame height                         (default 1080)
    --spp=N              deep samples per pixel                (default 20)
    --size=F             manual-mode blur radius at infinity  (default 20)
    --focus=F            focus distance, scene units          (default 10)
    --radius-min=F       CoC radius of the nearest cluster    (default 0.25)
    --radius-max=F       CoC radius of the farthest cluster   (default 4.0)
    --clusters=N         depth clusters the layers group into (default 5)
    --cluster-step=F     radius step BETWEEN members of one cluster, in px
                                                              (default 0.06)
    --k=N                depth_layers                          (default 16)
    --merge-tolerance=F                                        (default 0.25)
    --pre-merge=on|off                                         (default on)
    --max-radius=N                                             (default 100)
    --memory-limit=F     memory_limit knob, GB       (default: the node's own)
    --reps=N             timed repetitions                     (default 5)
    --warmup=N           untimed repetitions first             (default 1)
    --variant=defocus|source
                         ``source`` renders the SAME deep scene straight
                         through DeepToImage, i.e. everything the timed
                         render does EXCEPT this node.  The difference is
                         the node's own cost.        (default defocus)
    --stats              gate DEEPC_DEFOCUS_DEBUG_STATS on and report the
                         scatter's own work counters for one render
    --bands              gate DEEPC_DEFOCUS_DEBUG_BANDS on and report the
                         band -> thread claim distribution for one render
    --accuracy           render the scene at --merge-tolerance AND at 0, and
                         report what the tolerance costs in pixels.  Tolerance
                         0 is the reference, not another approximation: the
                         pre-merge does not run at all there, so every fragment
                         rasterises its own radius.  Wants a SMALL --width /
                         --height; the comparison is pure Python, per pixel
    --tmp-dir=DIR        where the render lands (default /dev/shm, then a
                         temp dir) — the write is inside the timed region,
                         so it wants to be a memory filesystem
    --label=TEXT         echoed into the summary line, for the caller's log

The scene is 20 (``--spp``) full-frame point-sample layers from
``scenes.texturedLayer`` — per-pixel colour and alpha, exactly reproducible
across runs and Nuke versions, and non-zero alpha everywhere so the sample
count really is ``width * height * spp`` with no sparsity to flatter the
measurement.  Depths come from INVERTING the manual CoC model, so the layers
sit at stated CoC radii rather than at stated depths, and the radii are
printed with every run.

THE LAYERS ARE CLUSTERED IN DEPTH, and that is load-bearing rather than
decorative.  A flat spread of 20 radii over [0.25, 4] px puts consecutive
layers ~0.2 px apart in radius but in DIFFERENT ΔCoC buckets at K=16, and the
pre-merge groups only within one bucket — so ``merge_tolerance`` would have
had nothing to do on the scene and a sweep of it would have measured
nothing.  Clustering (5 surfaces of 4 samples each, members ``--cluster-step``
apart in radius) is also what a renderer emits for hair, transparency and
motion-blurred geometry.
"""

import os
import resource
import threading
import statistics
import sys
import tempfile
import time

import nuke

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import harness                                                  # noqa: E402
import scenes                                                   # noqa: E402


LAST_RSS = None


# --- options -----------------------------------------------------------------

DEFAULTS = {
    "width": 2048,
    "height": 1080,
    "spp": 20,
    "size": 20.0,
    "focus": 10.0,
    "radiusMin": 0.25,
    "radiusMax": 4.0,
    "clusters": 5,
    "clusterStep": 0.06,
    "k": 16,
    "mergeTolerance": 0.25,
    "preMerge": True,
    "maxRadius": 100,
    "memoryLimit": None,
    "reps": 5,
    "warmup": 1,
    "variant": "defocus",
    "stats": False,
    "bands": False,
    "accuracy": False,
    "tmpDir": None,
    "label": "",
}

_SPELLING = {
    "--width": ("width", int),
    "--height": ("height", int),
    "--spp": ("spp", int),
    "--size": ("size", float),
    "--focus": ("focus", float),
    "--radius-min": ("radiusMin", float),
    "--radius-max": ("radiusMax", float),
    "--clusters": ("clusters", int),
    "--cluster-step": ("clusterStep", float),
    "--k": ("k", int),
    "--merge-tolerance": ("mergeTolerance", float),
    "--max-radius": ("maxRadius", int),
    "--memory-limit": ("memoryLimit", float),
    "--reps": ("reps", int),
    "--warmup": ("warmup", int),
    "--variant": ("variant", str),
    "--tmp-dir": ("tmpDir", str),
    "--label": ("label", str),
}


def parseArgs(argv):
    options = dict(DEFAULTS)
    for arg in argv:
        if arg in ("-h", "--help"):
            print(__doc__)
            raise SystemExit(0)
        if arg == "--stats":
            options["stats"] = True
            continue
        if arg == "--bands":
            options["bands"] = True
            continue
        if arg == "--accuracy":
            options["accuracy"] = True
            continue
        if "=" not in arg:
            raise SystemExit("options must be written --option=value, got %r "
                             "(see --help)" % arg)
        name, value = arg.split("=", 1)
        if name == "--pre-merge":
            if value not in ("on", "off"):
                raise SystemExit("--pre-merge must be on or off")
            options["preMerge"] = (value == "on")
            continue
        if name not in _SPELLING:
            raise SystemExit("unknown option %r (see --help)" % name)
        key, caster = _SPELLING[name]
        options[key] = caster(value)
    if options["variant"] not in ("defocus", "source"):
        raise SystemExit("--variant must be defocus or source")
    return options


# --- scene -------------------------------------------------------------------

def setFormat(width, height):
    """Retarget the harness (and ``scenes``) at a profiling-sized format.

    ``scenes`` did ``from harness import FORMAT_W, FORMAT_H``, which binds the
    values at import; patching only ``harness`` would leave every scene helper
    still building 256-px content inside a 2048-px format.
    """
    harness.FORMAT_W = width
    harness.FORMAT_H = height
    harness.FORMAT_NAME = "deepc_perf_%dx%d" % (width, height)
    scenes.FORMAT_W = width
    scenes.FORMAT_H = height


def layerDepths(options):
    """Depths for `--clusters` surfaces of `spp/clusters` samples each.

    Manual mode is ``radius = size * |1 - focus/z|``, so a wanted radius r
    behind focus inverts to ``z = focus / (1 - r/size)``.  Everything is behind
    focus, which makes depth order and radius order the same and keeps the
    scene's ΔCoC field monotonic — the pre-merge stages fragments in depth
    order, so a cluster's members are adjacent there and really are offered to
    each other.
    """
    n = int(options["spp"])
    clusters = int(options["clusters"])
    if clusters < 1 or n % clusters:
        raise SystemExit("--spp (%d) must be a positive multiple of "
                         "--clusters (%d)" % (n, clusters))
    perCluster = n // clusters
    rMin = float(options["radiusMin"])
    rMax = float(options["radiusMax"])
    step = float(options["clusterStep"])
    size = float(options["size"])
    focus = float(options["focus"])
    if not (rMax + step * perCluster < size):
        raise SystemExit("--radius-max must be below --size: a radius of "
                         "size is reached only at z = infinity")

    depths = []
    radii = []
    for c in range(clusters):
        base = rMin if clusters == 1 else \
            rMin + (rMax - rMin) * (c / float(clusters - 1))
        for j in range(perCluster):
            r = base + j * step
            depths.append(focus / (1.0 - r / size))
            radii.append(r)
    return depths, radii


def buildGraph(options, depths):
    harness.resetScript()
    layers = [scenes.texturedLayer(z, seed=i + 1)
              for i, z in enumerate(depths)]
    source = harness.deepMerge(layers)

    if options["variant"] == "source":
        head = harness.deepToImage(source)
    else:
        settings = harness.Settings(k=options["k"],
                                    preMerge=options["preMerge"],
                                    mergeTolerance=options["mergeTolerance"],
                                    maxRadius=options["maxRadius"])
        overrides = {}
        if options["memoryLimit"] is not None:
            overrides["memory_limit"] = float(options["memoryLimit"])
        head = harness.makeDefocus(settings, source,
                                   size=options["size"],
                                   focusDistance=options["focus"],
                                   cocMode="manual",
                                   **overrides)

    crop = nuke.nodes.Crop(inputs=[head])
    crop["box"].setValue([float(v) for v in harness.formatBox()])
    crop["reformat"].setValue(False)
    crop["intersect"].setValue(False)
    crop["crop"].setValue(True)

    write = nuke.nodes.Write(inputs=[crop])
    write["file"].setValue(options["_renderPath"])
    write["file_type"].setValue("exr")
    write["channels"].setValue("rgba")
    write["datatype"].setValue("32 bit float")
    write["compression"].setValue("none")
    write["autocrop"].setValue(False)
    write["raw"].setValue(True)
    return write


# --- stderr capture ----------------------------------------------------------

class CapturedStderr(object):
    """Redirect fd 2 to a file for the duration of a render.

    The node's instrumentation is ``fprintf(stderr, ...)`` from C++, so
    ``sys.stderr`` interposition would not see a byte of it; only a real fd
    redirect does.
    """

    def __init__(self, path):
        self._path = path
        self._saved = None

    def __enter__(self):
        sys.stderr.flush()
        self._saved = os.dup(2)
        fd = os.open(self._path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
        os.dup2(fd, 2)
        os.close(fd)
        return self

    def __exit__(self, *exc):
        sys.stderr.flush()
        os.dup2(self._saved, 2)
        os.close(self._saved)
        self._saved = None
        return False

    def lines(self):
        with open(self._path) as handle:
            return handle.read().splitlines()


# --- measurement -------------------------------------------------------------

def procStatusKb(field):
    """One /proc/self/status field, in kB.  VmRSS is the live resident set,
    VmHWM the process-lifetime high-water mark (never decreases)."""
    with open("/proc/self/status") as handle:
        for line in handle:
            if line.startswith(field + ":"):
                return int(line.split()[1])
    return 0


class RssSampler(object):
    """Peak VmRSS over the render, sampled off-thread.

    VmHWM alone cannot separate this render from an earlier one in the same
    process: it is a lifetime high-water mark.  Sampling gives the peak of
    THIS render, and the baseline it started from.
    """

    def __init__(self, intervalMs=10):
        self._interval = intervalMs / 1000.0
        self._stop = threading.Event()
        self._thread = None
        self.baselineKb = 0
        self.peakKb = 0

    def _run(self):
        while not self._stop.is_set():
            rss = procStatusKb("VmRSS")
            if rss > self.peakKb:
                self.peakKb = rss
            self._stop.wait(self._interval)

    def __enter__(self):
        self.baselineKb = procStatusKb("VmRSS")
        self.peakKb = self.baselineKb
        self._thread = threading.Thread(target=self._run)
        self._thread.daemon = True
        self._thread.start()
        return self

    def __exit__(self, *exc):
        self._stop.set()
        self._thread.join()
        rss = procStatusKb("VmRSS")
        if rss > self.peakKb:
            self.peakKb = rss
        return False


def timedRender(options, depths):
    """One full cook, from a freshly built graph, timed around execute().

    Returns (wall seconds, process CPU seconds).  THE CPU/WALL RATIO IS THE
    CONCURRENCY MEASURE THAT SURVIVES A THROTTLED BOX: it is the mean number
    of cores kept busy, and unlike a wall-clock speedup it does not fall when
    the part sits on a power budget rather than on a scheduler.

    The graph is rebuilt per repetition on purpose: Nuke caches an Op's output
    against its hash, so re-executing the SAME Write would time a cache hit and
    report a speed this node does not have.  ``scriptClear()`` inside
    ``resetScript()`` retires every hash in the script.
    """
    write = buildGraph(options, depths)
    before = resource.getrusage(resource.RUSAGE_SELF)
    with RssSampler() as rss:
        started = time.time()
        nuke.execute(write, 1, 1)
        wall = time.time() - started
    after = resource.getrusage(resource.RUSAGE_SELF)
    cpu = ((after.ru_utime - before.ru_utime)
           + (after.ru_stime - before.ru_stime))
    global LAST_RSS
    LAST_RSS = rss
    return wall, cpu


def parseStats(lines):
    total = {"bands": 0, "fragments": 0, "sharp": 0, "culled": 0,
             "rowSpans": 0, "pixelDeposits": 0}
    for line in lines:
        if "DeepCDefocus: stats rows" not in line:
            continue
        fields = line.split()
        total["bands"] += 1
        for key, token in (("fragments", "fragments"), ("sharp", "sharp"),
                           ("culled", "culled"), ("rowSpans", "rowSpans"),
                           ("pixelDeposits", "pixelDeposits")):
            total[key] += int(fields[fields.index(token) + 1])
    return total


def parseBands(lines):
    """Band claims: which thread took which band, and how long it held it."""
    claims = []
    for line in lines:
        if "DeepCDefocus: band" not in line or " computed by " not in line:
            continue
        fields = line.split()
        claims.append({
            "band": int(fields[2]),
            "thread": fields[fields.index("thread") + 1],
            "ms": float(fields[fields.index("in") + 1]),
            "fetchMs": (float(fields[fields.index("fetch") + 1])
                        if "fetch" in fields else 0.0),
        })
    return claims


def scratchDir(options):
    """/dev/shm when it can actually hold the render, else the normal temp dir.

    The write is inside the timed region, so a memory filesystem is worth
    having — but Nuke writes ``<name>.exr.tmp`` and renames, so it needs TWO
    uncompressed frames plus headroom, and a 64 MB /dev/shm (what a container
    typically gets) silently fails the render at 2K rather than being slow.
    """
    frameBytes = options["width"] * options["height"] * 4 * 4
    if os.path.isdir("/dev/shm"):
        stat = os.statvfs("/dev/shm")
        if stat.f_bavail * stat.f_frsize >= 3 * frameBytes:
            return "/dev/shm"
    return tempfile.gettempdir()


def accuracyDelta(options, depths, tmpDir):
    """Pixels lost to `--merge-tolerance`, against the same scene at 0."""
    base = harness.Settings(k=options["k"], preMerge=True,
                            mergeTolerance=options["mergeTolerance"],
                            maxRadius=options["maxRadius"], tmpDir=tmpDir)
    images = []
    for tolerance in (options["mergeTolerance"], 0.0):
        cell = base.derive(mergeTolerance=tolerance)
        harness.resetScript()
        layers = [scenes.texturedLayer(z, seed=i + 1)
                  for i, z in enumerate(depths)]
        node = harness.makeDefocus(cell, harness.deepMerge(layers),
                                   size=options["size"],
                                   focusDistance=options["focus"],
                                   cocMode="manual")
        images.append(harness.render(cell, node, "acc_%g" % tolerance,
                                     box=harness.formatBox()))
    return harness.compareImages(images[0], images[1], box=harness.formatBox())


def main(argv):
    options = parseArgs(argv)

    tmpDir = options["tmpDir"] or scratchDir(options)
    ownTmp = tempfile.mkdtemp(prefix="deepc-profile-", dir=tmpDir)
    options["_renderPath"] = os.path.join(ownTmp, "profile.exr")
    logPath = os.path.join(ownTmp, "stderr.log")

    setFormat(options["width"], options["height"])
    depths, radii = layerDepths(options)

    print("DeepCDefocus perf profile%s"
          % (" — %s" % options["label"] if options["label"] else ""))
    print("scene:    %d x %d, %d samples/px (%d total), %s variant"
          % (options["width"], options["height"], options["spp"],
             options["width"] * options["height"] * options["spp"],
             options["variant"]))
    print("coc:      manual, size=%.4g focus=%.4g -> radii %.4g .. %.4g px "
          "(mean %.4g), %d cluster(s) of %d, step %.4g px"
          % (options["size"], options["focus"], min(radii), max(radii),
             sum(radii) / len(radii), options["clusters"],
             options["spp"] // options["clusters"], options["clusterStep"]))
    print("radii:    %s" % " ".join("%.4f" % r for r in radii))
    print("depths:   %s" % " ".join("%.4f" % z for z in depths))
    print("knobs:    K=%d pre_merge=%s merge_tolerance=%.4g max_radius=%d"
          % (options["k"], "on" if options["preMerge"] else "off",
             options["mergeTolerance"], options["maxRadius"]))
    print("renders:  %s" % ownTmp)
    print("")

    for i in range(options["warmup"]):
        elapsed, cpu = timedRender(options, depths)
        print("warmup %d: %.3f s wall, %.3f s cpu, rss peak %.3f GB "
              "(discarded)"
              % (i + 1, elapsed, cpu, LAST_RSS.peakKb / 1048576.0))

    times = []
    cpus = []
    peaks = []
    for i in range(options["reps"]):
        elapsed, cpu = timedRender(options, depths)
        times.append(elapsed)
        cpus.append(cpu)
        peaks.append(LAST_RSS.peakKb)
        print("rep %d: %.3f s wall, %.3f s cpu (%.2f cores busy), "
              "rss %.3f -> %.3f GB (+%.3f)"
              % (i + 1, elapsed, cpu, cpu / elapsed if elapsed else 0.0,
                 LAST_RSS.baselineKb / 1048576.0, LAST_RSS.peakKb / 1048576.0,
                 (LAST_RSS.peakKb - LAST_RSS.baselineKb) / 1048576.0))
    print("")

    if times:
        print("TIMING %s variant=%s reps=%d min=%.3f median=%.3f mean=%.3f "
              "max=%.3f  cpuMedian=%.3f coresBusyMedian=%.2f"
              % (options["label"] or "-", options["variant"], len(times),
                 min(times), statistics.median(times),
                 sum(times) / len(times), max(times),
                 statistics.median(cpus),
                 statistics.median([c / t for c, t in zip(cpus, times)])))
        print("MEMORY %s memory_limit=%s rssPeakGB=%.3f rssHwmGB=%.3f"
              % (options["label"] or "-",
                 ("%.3f" % options["memoryLimit"])
                 if options["memoryLimit"] is not None else "default",
                 max(peaks) / 1048576.0,
                 procStatusKb("VmHWM") / 1048576.0))

    if options["stats"]:
        os.environ["DEEPC_DEFOCUS_DEBUG_STATS"] = "1"
        with CapturedStderr(logPath) as cap:
            timedRender(options, depths)
        total = parseStats(cap.lines())
        del os.environ["DEEPC_DEFOCUS_DEBUG_STATS"]
        print("STATS %s bands=%d fragments=%d sharp=%d culled=%d "
              "rowSpans=%d pixelDeposits=%d"
              % (options["label"] or "-", total["bands"], total["fragments"],
                 total["sharp"], total["culled"], total["rowSpans"],
                 total["pixelDeposits"]))

    if options["bands"]:
        os.environ["DEEPC_DEFOCUS_DEBUG_BANDS"] = "1"
        with CapturedStderr(logPath) as cap:
            elapsed, cpu = timedRender(options, depths)
        lines = cap.lines()
        claims = parseBands(lines)
        del os.environ["DEEPC_DEFOCUS_DEBUG_BANDS"]
        for line in lines:
            if ("DeepCDefocus: setup" in line
                    or "DeepCDefocus: budget" in line):
                print("      %s" % line.strip())
        threads = {}
        for claim in claims:
            entry = threads.setdefault(claim["thread"], [0, 0.0])
            entry[0] += 1
            entry[1] += claim["ms"]
        summed = sum(c["ms"] for c in claims) / 1000.0
        fetched = sum(c["fetchMs"] for c in claims) / 1000.0
        print("BANDS %s wall=%.3f s cpu=%.3f s (%.2f cores busy) claims=%d "
              "distinct=%d threads=%d summedCompute=%.3f s summedFetch=%.3f s "
              "bandConcurrency=%.2fx"
              % (options["label"] or "-", elapsed, cpu,
                 (cpu / elapsed) if elapsed > 0 else 0.0,
                 len(claims), len(set(c["band"] for c in claims)),
                 len(threads), summed, fetched,
                 (summed / elapsed) if elapsed > 0 else 0.0))
        for name in sorted(threads, key=lambda t: -threads[t][1]):
            count, ms = threads[name]
            print("      thread %s: %d band(s), %.3f s" % (name, count, ms / 1000.0))
        print("      summedFetch is the deep-input pull inside computeBand; "
              "the remainder is this node's own flatten/scatter/resolve")

    if options["accuracy"]:
        diff = accuracyDelta(options, depths, ownTmp)
        print("ACCURACY %s merge_tolerance %.4g vs 0: maxAbs=%.4e %s  %s"
              % (options["label"] or "-", options["mergeTolerance"],
                 diff.maxAbs, diff.population(), diff.where()))

    for name in os.listdir(ownTmp):
        try:
            os.remove(os.path.join(ownTmp, name))
        except OSError:
            pass
    os.rmdir(ownTmp)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
