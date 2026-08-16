"""Headless validation harness for DeepCDefocus (M1.P3.T12).

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

RGBA = ("R", "G", "B", "A")

FORMAT_NAME = "deepc_val_256"
FORMAT_W = 256
FORMAT_H = 256


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
        self._renderIndex = 0

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
             expectedFailure=False):
    """A `measured <= tol` gate, formatted for the report table."""
    ok = measured <= tol
    if ok:
        status = PASS
    else:
        status = XFAIL if expectedFailure else FAIL
    return Check(scene, name, "%.3e" % measured, "<= %.1e" % tol, status,
                 population, note)


def boolCheck(scene, name, ok, measured, gate, population=None, note="",
              expectedFailure=False):
    status = PASS if ok else (XFAIL if expectedFailure else FAIL)
    return Check(scene, name, measured, gate, status, population, note)


# --- scene / graph plumbing --------------------------------------------------

def resetScript(pixelAspect=1.0):
    """Fresh node graph plus the harness format. Called once per scene so a
    scene can never inherit another scene's nodes or root knobs."""
    nuke.scriptClear()
    nuke.addFormat("%d %d %g %s" % (FORMAT_W, FORMAT_H, pixelAspect, FORMAT_NAME))
    nuke.root()["format"].setValue(FORMAT_NAME)
    nuke.root()["first_frame"].setValue(1)
    nuke.root()["last_frame"].setValue(1)


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
    settings._renderIndex += 1
    safeTag = "".join(ch if (ch.isalnum() or ch in "-_.") else "_" for ch in tag)
    path = os.path.join(settings.tmpDir,
                        "%03d_%s.exr" % (settings._renderIndex, safeTag))

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
    node = nuke.nodes.Constant(format=FORMAT_NAME)
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


def pointLayer(image2d, z, keepZeroAlpha=False, premult=False):
    """One deep sample per pixel at a single depth (zFront == zBack)."""
    node = nuke.nodes.DeepFromImage(inputs=[image2d])
    node["set_z"].setValue(True)
    node["z"].setValue(float(z))
    node["keepZeroAlpha"].setValue(bool(keepZeroAlpha))
    node["premult"].setValue(bool(premult))
    return node


def slab(front, back, color, samples=1, alphaMode=0):
    """A full-frame volumetric slab. ``alphaMode``: 0 uniform, 1 additive,
    2 multiplicative (DeepCConstant's own enum).

    NOTE ``back`` must differ from ``front``: DeepCConstant divides by
    ``back - front`` and emits NaN for a zero-thickness slab.
    """
    if abs(back - front) < 1e-9:
        raise ValueError("DeepCConstant emits NaN when front == back")
    node = nuke.nodes.DeepCConstant(format=FORMAT_NAME)
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


def channelStats(image, channel, box):
    x0, y0, x1, y1 = box
    out = Stats()
    for y in range(y0, y1):
        row = image.row(channel, y)
        for x in range(x0, x1):
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


def insetBox(box, inset):
    x0, y0, x1, y1 = box
    return (x0 + inset, y0 + inset, x1 - inset, y1 - inset)
