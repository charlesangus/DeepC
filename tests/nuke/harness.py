"""Headless validation harness for DeepCDefocus (M1.P3.T12, extended by T16).

Everything in here runs *inside* Nuke's terminal interpreter:

    NUKE_PATH=<plugin dir> Nuke17.0 -t tests/nuke/run_validation.py [options]

The harness owns three things:

  * ``Settings`` — the knob values every render must set EXPLICITLY.  The
    milestone forbids relying on a default for ``combine``, ``holdoutInterp``,
    ``K`` and ``pre_merge`` (a non-occluding holdout alone moves defocused
    pixels by up to 1.78e-01 through merge regrouping), so ``makeDefocus()``
    writes all of them on every node it builds.
  * scene plumbing — format setup, deep-source builders, and ``render()``,
    which flattens an Op to an uncompressed 32-bit-float EXR and reads the
    pixels straight back (Nuke's Python has no numpy/OpenImageIO).
  * measurement — ``compareImages()`` and friends, so a scene reports a
    number and a pixel population rather than an eyeballed frame.

Scene bodies live in ``scenes.py``.
"""

import os
import struct
import sys

import nuke

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from exrio import readExr                                       # noqa: E402


# --- knob vocabularies -------------------------------------------------------
#
# Indices, not labels: Enumeration_knob.setValue(str) matches the *label*, and
# the labels are cosmetic while the indices are the enum values the scatter
# core reads (src/DeepCDefocus.cpp, bucketCombineNames / holdoutInterpNames).

COC_MODE = {"physical": 0, "manual": 1}

BUCKET_COMBINE = {"over": 0, "partition": 1}
BUCKET_COMBINE_LABEL = {0: "FrontToBackOver", 1: "CoveragePartition"}

HOLDOUT_INTERP = {"logchord": 0, "midpoint": 1, "lineart": 2}
HOLDOUT_INTERP_LABEL = {0: "LogChord", 1: "MidpointStep", 2: "LinearInT"}

# Index -> option name, so Settings.derive() can rebuild a Settings from one.
BUCKET_COMBINE_NAME = dict((v, k) for k, v in BUCKET_COMBINE.items())
HOLDOUT_INTERP_NAME = dict((v, k) for k, v in HOLDOUT_INTERP.items())

RGBA = ("R", "G", "B", "A")

FORMAT_NAME = "deepc_val_256"
FORMAT_W = 256
FORMAT_H = 256

# The format the CURRENT scene is being built in.  Every 2D source builder below
# reads this rather than FORMAT_NAME, because scene (j) runs at pixel aspect 2
# and a `Constant` pinned to the square-pixel format would hand the node a
# square-pixel `Format` no matter what the root said — measured: the node's bbox
# pad stayed 41 px in Y instead of 82, and the bokeh came out circular, i.e. the
# whole anamorphic scene silently tested nothing.
_currentFormat = FORMAT_NAME


def currentFormat():
    return _currentFormat


class Settings(object):
    """Harness-wide knob settings. T16/T17/T18 drive the same scenes through
    different values of these; nothing here is allowed to be implicit."""

    def __init__(self, k=16, combine="partition", holdoutInterp="logchord",
                 preMerge=True, mergeTolerance=0.25, maxRadius=100,
                 tmpDir=None, keepRenders=False, verbose=False):
        self.k = int(k)
        self.combine = BUCKET_COMBINE[combine]
        self.holdoutInterp = HOLDOUT_INTERP[holdoutInterp]
        self.preMerge = bool(preMerge)
        self.mergeTolerance = float(mergeTolerance)
        self.maxRadius = int(maxRadius)
        self.tmpDir = tmpDir
        self.keepRenders = keepRenders
        self.verbose = verbose
        # Shared so a derive()d clone cannot reuse a filename the original
        # already wrote (and then read back the wrong image).
        self._renderIndex = [0]

    def derive(self, **overrides):
        """A copy with some knobs changed, sharing the render counter/tmp dir.

        Scene (g) has to render one scene at K=8/16/64 under BOTH bucket
        combines in a single pass (M1.P3.T17 inherits that table rather than
        re-rendering it), and scene (i) has to render with ``pre_merge`` both
        ways.  Cloning keeps every OTHER knob at the run's settings, so a sweep
        cell still differs from the run in exactly one stated way.
        """
        clone = Settings(k=self.k,
                         combine=BUCKET_COMBINE_NAME[self.combine],
                         holdoutInterp=HOLDOUT_INTERP_NAME[self.holdoutInterp],
                         preMerge=self.preMerge,
                         mergeTolerance=self.mergeTolerance,
                         maxRadius=self.maxRadius,
                         tmpDir=self.tmpDir, keepRenders=self.keepRenders,
                         verbose=self.verbose)
        for key, value in overrides.items():
            if key == "combine":
                clone.combine = BUCKET_COMBINE[value]
            elif key == "holdoutInterp":
                clone.holdoutInterp = HOLDOUT_INTERP[value]
            elif key == "k":
                clone.k = int(value)
            elif key == "preMerge":
                clone.preMerge = bool(value)
            elif key == "mergeTolerance":
                clone.mergeTolerance = float(value)
            elif key == "maxRadius":
                clone.maxRadius = int(value)
            else:
                raise KeyError("Settings.derive: unknown knob %r" % key)
        clone._renderIndex = self._renderIndex          # shared counter
        return clone

    def describe(self):
        return ("K=%d  combine=%s  holdoutInterp=%s  pre_merge=%s  "
                "merge_tolerance=%.3f  max_radius=%d"
                % (self.k, BUCKET_COMBINE_LABEL[self.combine],
                   HOLDOUT_INTERP_LABEL[self.holdoutInterp],
                   "on" if self.preMerge else "off",
                   self.mergeTolerance, self.maxRadius))


# --- results -----------------------------------------------------------------

PASS = "PASS"
FAIL = "FAIL"
XFAIL = "XFAIL"     # documented residual: reported with numbers, not a regression
SKIP = "SKIP"       # cannot be exercised in this environment


class Check(object):
    def __init__(self, scene, name, measured, gate, status,
                 population=None, note=""):
        self.scene = scene
        self.name = name
        self.measured = measured        # pre-formatted string
        self.gate = gate                # pre-formatted string
        self.status = status
        self.population = population    # pre-formatted string or None
        self.note = note


def tolCheck(scene, name, measured, tol, population=None, note="",
             expectedFailure=False, hardTol=None):
    """A `measured <= tol` gate, formatted for the report table.

    ``expectedFailure`` alone is an UNBOUNDED licence to fail: a check written
    that way stays XFAIL no matter how far the number moves, so a regression on
    top of a documented residual is indistinguishable from the residual.  Any
    XFAIL that pins a documented deficit must therefore also pass ``hardTol`` —
    the magnitude beyond which the reading is no longer that deficit and the
    check goes back to a hard FAIL.
    """
    if measured <= tol:
        status = PASS
    elif not expectedFailure:
        status = FAIL
    elif hardTol is None or measured <= hardTol:
        status = XFAIL
    else:
        status = FAIL
    gate = "<= %.1e" % tol
    if expectedFailure and hardTol is not None:
        gate += " (xfail < %.1e)" % hardTol
    return Check(scene, name, "%.3e" % measured, gate, status, population, note)


def boolCheck(scene, name, ok, measured, gate, population=None, note="",
              expectedFailure=False, hardTol=None, hardValue=None):
    """A boolean gate, formatted for the report table.

    Same ``hardTol`` discipline as ``tolCheck`` (see its docstring): an
    ``expectedFailure`` with no ``hardTol`` is an UNBOUNDED licence to fail,
    so any XFAIL that pins a documented deficit must also pass ``hardTol`` —
    checked against ``hardValue``, the numeric magnitude of the deficit (a
    fraction, a percentage, ...) rather than against the boolean ``ok`` a
    caller may have derived from several conditions at once. ``hardValue`` is
    required exactly when ``hardTol`` is; the caller is responsible for
    saying so in ``gate``/``note`` since, unlike ``tolCheck``, this helper
    never formats a number into either.
    """
    if hardTol is not None and hardValue is None:
        raise ValueError("boolCheck: hardTol needs hardValue")
    if ok:
        status = PASS
    elif not expectedFailure:
        status = FAIL
    elif hardTol is None or hardValue <= hardTol:
        status = XFAIL
    else:
        status = FAIL
    return Check(scene, name, measured, gate, status, population, note)


# --- scene / graph plumbing --------------------------------------------------

def resetScript(pixelAspect=1.0, proxyScale=None):
    """Fresh node graph plus the harness format. Called once per scene so a
    scene can never inherit another scene's nodes or root knobs.

    ``pixelAspect`` gets its OWN named format: ``nuke.scriptClear()`` does not
    drop registered formats, so re-adding one name with a different aspect makes
    Nuke rename the second ("Script contains two different formats named ...")
    and ``setValue(name)`` then picks the first — the aspect never reached the
    node.  ``proxyScale`` (e.g. 0.5) switches the root into scaled proxy mode,
    which is what drives ``DeepInfo::fullSizeFormat()`` and hence the node's own
    ``_proxyScale``; it is reset to off on every other scene.
    """
    global _currentFormat
    nuke.scriptClear()
    if pixelAspect == 1.0:
        name = FORMAT_NAME
    else:
        name = "%s_pa%g" % (FORMAT_NAME, pixelAspect)
    if not any(f.name() == name for f in nuke.formats()):
        nuke.addFormat("%d %d %g %s" % (FORMAT_W, FORMAT_H, pixelAspect, name))
    _currentFormat = name
    nuke.root()["format"].setValue(name)
    nuke.root()["first_frame"].setValue(1)
    nuke.root()["last_frame"].setValue(1)

    root = nuke.root()
    if proxyScale is None:
        root["proxy"].setValue(False)
    else:
        root["proxy_type"].setValue("scale")
        root["proxy_scale"].setValue(float(proxyScale))
        root["proxy"].setValue(True)


def formatBox():
    return (0, 0, FORMAT_W, FORMAT_H)


def makeDefocus(settings, source, holdout=None, size=0.0,
                focusDistance=10.0, cocMode="manual", channels="rgba",
                **overrides):
    """Build a DeepCDefocus with EVERY bake-off knob written explicitly.

    ``overrides`` sets any further knob by name (``front_coc_mult``,
    ``depth_is_ray_distance``, ...), so a scene never has to reach past this
    helper and silently skip the explicit-knob discipline.
    """
    node = nuke.nodes.DeepCDefocus(inputs=[source])
    node["coc_mode"].setValue(COC_MODE[cocMode])
    node["size"].setValue(float(size))
    node["focus_distance"].setValue(float(focusDistance))
    node["max_radius"].setValue(settings.maxRadius)
    node["channels"].setValue(channels)

    # The four harness parameters, always explicit (milestone Decisions).
    node["depth_layers"].setValue(settings.k)
    node["pre_merge"].setValue(settings.preMerge)
    node["merge_tolerance"].setValue(settings.mergeTolerance)
    node["bucket_combine"].setValue(settings.combine)
    node["holdout_interp"].setValue(settings.holdoutInterp)

    for knobName, value in overrides.items():
        node[knobName].setValue(value)

    if holdout is not None:
        connected = node.setInput(1, holdout)
        if not connected:
            raise RuntimeError("DeepCDefocus refused the holdout input; "
                               "scene (b)/(f) cannot be built")
    return node


def deepToImage(source, volumetric=True):
    node = nuke.nodes.DeepToImage(inputs=[source])
    # Pinned ON deliberately: with it off Nuke selects the plain-`over` form
    # this node moved away from, and the two disagree at ~1e-02 by design
    # (Design reference, scene (a)).
    node["volumetric_composition"].setValue(bool(volumetric))
    return node


def render(settings, node, tag, channels="rgba", box=None):
    """Flatten an Op to an uncompressed 32-bit-float EXR and read it back."""
    if box is None:
        box = formatBox()
    settings._renderIndex[0] += 1
    safeTag = "".join(ch if (ch.isalnum() or ch in "-_.") else "_" for ch in tag)
    path = os.path.join(settings.tmpDir,
                        "%03d_%s.exr" % (settings._renderIndex[0], safeTag))

    crop = nuke.nodes.Crop(inputs=[node])
    crop["box"].setValue([float(v) for v in box])
    crop["reformat"].setValue(False)
    crop["intersect"].setValue(False)
    crop["crop"].setValue(True)

    write = nuke.nodes.Write(inputs=[crop])
    write["file"].setValue(path)
    write["file_type"].setValue("exr")
    write["channels"].setValue(channels)
    write["datatype"].setValue("32 bit float")
    write["compression"].setValue("none")
    write["autocrop"].setValue(False)
    write["raw"].setValue(True)             # no colourspace transform, ever
    # Scene (k) renders with the root in proxy mode, where a Write with no
    # proxy path aborts ("You must specify a proxy file name to write to").
    # Same file: only one of the two is ever used in a given cook.
    write["proxy"].setValue(path)

    nuke.execute(write, 1, 1)
    image = readExr(path)
    if not settings.keepRenders:
        try:
            os.remove(path)
        except OSError:
            pass
    return image


# --- deep source builders ----------------------------------------------------

def constant2d(color):
    node = nuke.nodes.Constant(format=currentFormat())
    node["color"].setValue(list(color))
    return node


def rectangle2d(area, color, softness=0.0):
    node = nuke.nodes.Rectangle(inputs=[constant2d((0.0, 0.0, 0.0, 0.0))])
    node["area"].setValue([float(v) for v in area])
    node["color"].setValue(list(color))
    node["softness"].setValue(float(softness))
    return node


def ramp2d(p0, p1, color):
    node = nuke.nodes.Ramp(inputs=[constant2d((0.0, 0.0, 0.0, 0.0))])
    node["p0"].setValue([float(v) for v in p0])
    node["p1"].setValue([float(v) for v in p1])
    node["color"].setValue(list(color))
    return node


def pointLayer(image2d, z, keepZeroAlpha=False, premult=False, setZ=True):
    """One deep sample per pixel at a single depth (zFront == zBack).

    ``setZ=False`` takes the depth from the input's ``depth.Z`` channel instead
    — see ``depthRampLayer()``, which is the only caller that does.
    """
    node = nuke.nodes.DeepFromImage(inputs=[image2d])
    node["set_z"].setValue(bool(setZ))
    node["z"].setValue(float(z))
    node["keepZeroAlpha"].setValue(bool(keepZeroAlpha))
    node["premult"].setValue(bool(premult))
    return node


def depthRampLayer(image2d, zExpr):
    """ONE deep sample per pixel at a PER-PIXEL depth given by ``zExpr``.

    ``pointLayer()`` can only place a whole layer at one constant Z
    (``DeepFromImage``'s ``set_z``), which cannot build a receding ground plane
    (scene (g)/(l)) or a single-sample near/far pair (scene (i)).  With
    ``set_z`` OFF, ``DeepFromImage`` takes the depth from the input's ``depth.Z``
    channel — and it reads that channel as INVERSE depth: measured here, a
    ``depth.Z`` of 10 produces a sample whose deep front is 0.1 (a
    ``DeepCrop`` at znear/zfar [5,15] drops it and one at [0.05,0.15] keeps it).
    So this writes ``1/z``.

    ``zExpr`` is a Nuke expression in x/y returning the wanted Z; it must stay
    strictly positive over the whole frame.  Samples are point samples
    (zFront == zBack), and zero-alpha pixels emit NO sample at all, which is how
    scene (i) builds a coverage hole with no hidden data behind it.
    """
    node = nuke.nodes.Expression(inputs=[image2d])
    node["channel0"].setValue("depth")
    node["expr0"].setValue("1.0/(%s)" % zExpr)
    return pointLayer(node, 0.0, keepZeroAlpha=False, premult=False,
                      setZ=False)


def slab(front, back, color, samples=1, alphaMode=0):
    """A full-frame volumetric slab. ``alphaMode``: 0 uniform, 1 additive,
    2 multiplicative (DeepCConstant's own enum).

    NOTE ``back`` must differ from ``front``: DeepCConstant divides by
    ``back - front`` and emits NaN for a zero-thickness slab.
    """
    if abs(back - front) < 1e-9:
        raise ValueError("DeepCConstant emits NaN when front == back")
    node = nuke.nodes.DeepCConstant(format=currentFormat())
    node["front"].setValue(float(front))
    node["back"].setValue(float(back))
    node["samples"].setValue(int(samples))
    node["alpha_mode"].setValue(int(alphaMode))
    node["color_front"].setValue(list(color))
    node["color_back"].setValue(list(color))
    return node


def cropDeep(source, box):
    """Spatially limit a deep source — a full-frame slab becomes a card."""
    node = nuke.nodes.DeepCrop(inputs=[source])
    node["use_bbox"].setValue(True)
    node["bbox"].setValue([float(v) for v in box])
    node["outside_bbox"].setValue(False)        # drop samples outside
    node["use_znear"].setValue(False)
    node["use_zfar"].setValue(False)
    return node


def deepMerge(sources, operation="combine"):
    node = nuke.nodes.DeepMerge2(inputs=list(sources))
    node["operation"].setValue(operation)
    node["drop_hidden"].setValue(False)
    return node


def deepHoldout(source, holdout):
    """Scene (b)'s reference. DeepHoldout2, NOT DeepHoldout: the latter's
    input 1 is a 2D depth image and cannot take a deep input (milestone
    Decisions, 2026-07-27).

    DeepHoldout2 outputs a **flat 2D image** — it does its own flatten, so it
    must NOT be followed by a DeepToImage (measured here: feeding its output
    to DeepToImage yields a uniform (0,0,0,1) frame).  That own flatten is
    what differs from volumetric DeepToImage by |dc| 3.8e-03 on 11.9% of
    pixels on volumetric spans.
    """
    node = nuke.nodes.DeepHoldout2(inputs=[source])
    if not node.setInput(1, holdout):
        raise RuntimeError("DeepHoldout2 refused the deep holdout input")
    return node


def deepMergeHoldout(source, holdout):
    """A second, independent holdout reference: DeepMerge2's `holdout`
    operation still returns a deep image, so it flattens through the same
    DeepToImage scene (a) is gated against. Where this and DeepHoldout2
    disagree, the gap is the reference's flatten, not this node's holdout."""
    node = nuke.nodes.DeepMerge2(inputs=[source, holdout])
    node["operation"].setValue("holdout")
    node["drop_hidden"].setValue(False)
    return node


# --- measurement -------------------------------------------------------------

def ulps(a, b):
    """Distance in representable floats between two same-signed finite floats."""
    ia = struct.unpack("<i", struct.pack("<f", a))[0]
    ib = struct.unpack("<i", struct.pack("<f", b))[0]
    if ia < 0:
        ia = -2147483648 - ia
    if ib < 0:
        ib = -2147483648 - ib
    return abs(ia - ib)


class Diff(object):
    def __init__(self):
        self.maxAbs = 0.0
        self.maxChannel = None
        self.maxAt = (0, 0)
        self.maxUlps = 0
        self.maxUlpsAny = 0
        self.overCount = 0
        self.total = 0

    @property
    def rate(self):
        return (100.0 * self.overCount / self.total) if self.total else 0.0

    def population(self, threshold=1e-3):
        return "%d/%d px (%.2f%%) > %.0e" % (self.overCount, self.total,
                                             self.rate, threshold)

    def where(self):
        if self.maxChannel is None:
            return "-"
        return "%s@(%d,%d) %d ULP; worst ULP anywhere %d" % (
            self.maxChannel, self.maxAt[0], self.maxAt[1], self.maxUlps,
            self.maxUlpsAny)


def compareImages(imgA, imgB, channels=RGBA, box=None, threshold=1e-3):
    """Worst absolute channel difference and the pixel population over
    ``threshold`` — the two numbers every parity gate in this milestone is
    stated in."""
    if box is None:
        box = formatBox()
    x0, y0, x1, y1 = box
    out = Diff()
    for y in range(y0, y1):
        rowsA = dict((c, imgA.row(c, y)) for c in channels)
        rowsB = dict((c, imgB.row(c, y)) for c in channels)
        for x in range(x0, x1):
            worst = 0.0
            for c in channels:
                ia = x - imgA.x0
                ib = x - imgB.x0
                va = rowsA[c][ia] if 0 <= ia < imgA.width else 0.0
                vb = rowsB[c][ib] if 0 <= ib < imgB.width else 0.0
                d = abs(va - vb)
                u = ulps(va, vb)
                if u > out.maxUlpsAny:
                    out.maxUlpsAny = u
                if d > worst:
                    worst = d
                if d > out.maxAbs:
                    out.maxAbs = d
                    out.maxChannel = c
                    out.maxAt = (x, y)
                    out.maxUlps = ulps(va, vb)
            out.total += 1
            if worst > threshold:
                out.overCount += 1
    return out


class Stats(object):
    def __init__(self):
        self.minimum = float("inf")
        self.maximum = float("-inf")
        self.total = 0.0
        self.count = 0
        self.nonZero = 0
        self.minAt = (0, 0)
        self.maxAt = (0, 0)

    @property
    def mean(self):
        return self.total / self.count if self.count else 0.0

    @property
    def spread(self):
        return self.maximum - self.minimum if self.count else 0.0

    def __str__(self):
        return "min %.7f max %.7f mean %.7f" % (self.minimum, self.maximum,
                                                self.mean)


def channelStats(image, channel, box, exclude=None):
    """Min/max/mean of ``channel`` over ``box``.

    ``exclude(x, y) -> bool`` drops pixels from the statistic.  Scene (l) uses
    it to measure the CoC field's own extremum SEPARATELY from the rest of the
    frame, because the two are different mechanisms with different magnitudes;
    the excluded region is never simply discarded, it gets its own check.
    """
    x0, y0, x1, y1 = box
    out = Stats()
    for y in range(y0, y1):
        row = image.row(channel, y)
        for x in range(x0, x1):
            if exclude is not None and exclude(x, y):
                continue
            i = x - image.x0
            v = row[i] if 0 <= i < image.width else 0.0
            if v < out.minimum:
                out.minimum = v
                out.minAt = (x, y)
            if v > out.maximum:
                out.maximum = v
                out.maxAt = (x, y)
            out.total += v
            out.count += 1
            if v != 0.0:
                out.nonZero += 1
    if out.count == 0:
        out.minimum = out.maximum = 0.0
    return out


def rowMeans(image, channel, box):
    """Per-scanline mean of ``channel`` over ``box``, bottom row first.

    Scenes (g) and (l) ramp depth along Y, so a bucket seam is a step in this
    profile: averaging along X kills the per-pixel noise a single scanline would
    carry while leaving any horizontal band intact.
    """
    x0, y0, x1, y1 = box
    width = float(x1 - x0) or 1.0
    out = []
    for y in range(y0, y1):
        row = image.row(channel, y)
        total = 0.0
        for x in range(x0, x1):
            i = x - image.x0
            total += row[i] if 0 <= i < image.width else 0.0
        out.append(total / width)
    return out


def stepProfile(values):
    """(max |first difference|, median |first difference|, index of the max).

    The seam metric scenes (g)/(l) are gated on: a smoothly varying profile has
    a max step close to its median step, while a bucket boundary shows up as one
    localised spike against an otherwise flat neighbourhood.  Both numbers are
    ABSOLUTE, and the median is returned alongside the max precisely so a
    caller can see whether a spike stands out from its neighbourhood or the
    whole profile is that noisy — the gates are stated in absolute output
    levels (1/255 is the step the eye resolves in an 8-bit view), because a
    ratio has no meaning on a profile that is flat to 1e-9.
    """
    if len(values) < 2:
        return 0.0, 0.0, 0
    steps = [abs(values[i] - values[i - 1]) for i in range(1, len(values))]
    ordered = sorted(steps)
    median = ordered[len(ordered) // 2]
    worst = max(steps)
    return worst, median, steps.index(worst) + 1


def insetBox(box, inset):
    x0, y0, x1, y1 = box
    return (x0 + inset, y0 + inset, x1 - inset, y1 - inset)
