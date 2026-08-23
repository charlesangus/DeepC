"""Validation scenes (a)-(l) from the M1 Design reference's scene list.

Every scene builds its graph from Python nodes (no committed ``.nk`` — that is
M1.P5.T3's job), renders through ``harness.render()`` and reports a number.
No scene reads or writes anything under ``src/``: a failure here is reported
with its magnitude and pixel population, never patched.
"""

import math

import nuke

from harness import (
    FORMAT_H, FORMAT_W, RGBA, SKIP, Check,
    boolCheck, channelStats, compareImages, constant2d, cropDeep, deepHoldout,
    deepMerge, deepMergeHoldout, deepToImage, depthRampLayer, formatBox,
    insetBox, makeDefocus, pointLayer, rectangle2d, render, resetScript,
    rowMeans, slab, stepProfile, tolCheck,
)


# --- shared content ----------------------------------------------------------

def texturedLayer(z, seed, alphaLow=0.05, alphaHigh=0.95):
    """A per-pixel-varying point-sample layer.

    An ``Expression`` gives full control of colour and alpha as functions of
    x and y — richer than a ramp and, unlike a Noise, exactly reproducible
    across runs and Nuke versions.
    """
    node = nuke.nodes.Expression(inputs=[constant2d((0.0, 0.0, 0.0, 0.0))])
    s = float(seed)
    alphaExpr = ("(%f+%f*(0.5+0.5*sin(x*0.013+y*0.021+%f)))"
                 % (alphaLow, alphaHigh - alphaLow, s * 3.1))
    # Premultiplied at source (colour <= alpha), which is what a renderer
    # emits and what keeps a composite of these layers inside [0, 1]. Doing
    # it here rather than through DeepFromImage's own `premult` knob keeps the
    # data valid regardless of how that knob is interpreted.
    node["expr0"].setValue("(0.5+0.5*sin(x*0.031+%f))*%s" % (s * 1.7, alphaExpr))
    node["expr1"].setValue("(0.5+0.5*sin(y*0.027+%f))*%s" % (s * 2.3, alphaExpr))
    node["expr2"].setValue("(0.5+0.5*sin((x+y)*0.019+%f))*%s"
                           % (s * 0.9, alphaExpr))
    node["expr3"].setValue(alphaExpr)
    return pointLayer(node, z, keepZeroAlpha=False, premult=False)


def stripSource(depths, width=None):
    """Vertical opaque strips, one per depth — a depth staircase whose
    per-strip output alpha reads holdout visibility directly."""
    n = len(depths)
    stripWidth = width or (FORMAT_W // n)
    layers = []
    boxes = []
    for i, z in enumerate(depths):
        x0 = i * stripWidth
        x1 = x0 + stripWidth
        rect = rectangle2d((x0, 0, x1, FORMAT_H), (1.0, 1.0, 1.0, 1.0))
        layers.append(pointLayer(rect, z, keepZeroAlpha=False, premult=True))
        boxes.append((x0, x1))
    return deepMerge(layers), boxes


# --- scene (a) ---------------------------------------------------------------

def sceneA(settings):
    """size=0 / all-in-focus => matches stock DeepToImage.

    Three scoped rows, per the Design reference: point samples and
    coincident-depth samples at <=2e-07 absolute (NOT 0 ULP — the mandatory
    fractional two-bucket split makes bit-exactness unavailable by design),
    overlapping volumetric spans at <=2.4e-07 with `volumetric_composition`
    pinned ON.
    """
    checks = []
    box = formatBox()

    rows = [
        ("a1 point samples,   4 layers", 2.0e-07,
         lambda: deepMerge([texturedLayer(4.0, 0),
                            texturedLayer(7.5, 1),
                            texturedLayer(11.0, 2),
                            texturedLayer(17.0, 3)])),
        ("a2 coincident depth, 5 layers", 2.0e-07,
         lambda: deepMerge([texturedLayer(9.0, i) for i in range(5)])),
        ("a3 overlapping volumetric spans", 2.4e-07,
         lambda: deepMerge([slab(3.0, 9.0, (0.30, 0.45, 0.20, 0.40)),
                            slab(6.0, 14.0, (0.15, 0.35, 0.55, 0.60)),
                            slab(10.0, 20.0, (0.50, 0.20, 0.25, 0.30))])),
    ]

    for name, tol, build in rows:
        resetScript()
        source = build()
        reference = render(settings, deepToImage(source, volumetric=True),
                           "a_ref", box=box)
        node = makeDefocus(settings, source, size=0.0, cocMode="manual")
        measured = render(settings, node, "a_deepc", box=box)
        diff = compareImages(reference, measured, box=box)
        checks.append(tolCheck("a", name, diff.maxAbs, tol,
                               population=diff.population(),
                               note="worst " + diff.where()))

    # a4/a5: connecting a holdout that occludes NOTHING must not move the
    # image. The milestone measured defocused pixels moving by up to 1.78e-01
    # on 442/4096 px through merge regrouping when one is connected
    # (Decisions, 2026-07-27), so the sharp and defocused cases are reported
    # separately — T17/T18 need to know which of their deltas is this.
    holdoutRows = [
        ("a4 non-occluding holdout connected, size=0", 0.0, False),
        ("a5 non-occluding holdout connected, size=6", 6.0, True),
    ]
    for name, size, documented in holdoutRows:
        resetScript()
        radius = size * abs(1.0 - 10.0 / 4.0)
        pad = int(math.ceil(radius)) + 6
        renderBox = (-pad, -pad, 256 + pad, 256 + pad)
        source = deepMerge([texturedLayer(4.0, 0), texturedLayer(7.5, 1),
                            texturedLayer(11.0, 2), texturedLayer(17.0, 3)])
        # Behind every source sample, so vis == 1 everywhere by construction.
        holdout = pointLayer(rectangle2d((0, 0, 256, 256), (0.0, 0.0, 0.0, 1.0)),
                             40.0, keepZeroAlpha=False, premult=True)
        without = render(settings,
                         makeDefocus(settings, source, size=size,
                                     focusDistance=10.0, cocMode="manual"),
                         "a_noholdout", box=renderBox)
        withHoldout = render(settings,
                             makeDefocus(settings, source, holdout=holdout,
                                         size=size, focusDistance=10.0,
                                         cocMode="manual"),
                             "a_holdout", box=renderBox)
        diff = compareImages(without, withHoldout, box=renderBox)
        checks.append(tolCheck("a", name, diff.maxAbs, 2.0e-07,
                               population=diff.population(),
                               note="worst " + diff.where(),
                               expectedFailure=documented))
    return checks


# --- scene (b) ---------------------------------------------------------------

def sceneB(settings):
    """Holdout with everything in focus => matches stock DeepHoldout2.

    Built with DeepHoldout2 deliberately: DeepHoldout's input 1 is a 2D depth
    image and cannot take a deep input (milestone Decisions, 2026-07-27).
    DeepHoldout2 flattens internally, so it is the whole reference — putting a
    DeepToImage after it yields a uniform (0,0,0,1) frame.  The
    source is kept to point samples because DeepHoldout2's own flatten differs
    from volumetric DeepToImage by |dc| 3.8e-03 on 11.9% of pixels on
    volumetric spans — that gap is the reference's, not this node's.
    """
    checks = []
    box = formatBox()

    variants = [
        ("b1 opaque card holdout", 2.0e-07,
         lambda: pointLayer(rectangle2d((48, 48, 208, 208),
                                        (0.0, 0.0, 0.0, 1.0)),
                            10.0, keepZeroAlpha=False, premult=True)),
        ("b2 partial-alpha holdout", 2.0e-07,
         lambda: pointLayer(rectangle2d((32, 32, 224, 224),
                                        (0.0, 0.0, 0.0, 0.55)),
                            10.0, keepZeroAlpha=False, premult=True)),
    ]

    for name, tol, buildHoldout in variants:
        resetScript()
        source = deepMerge([texturedLayer(5.0, 4), texturedLayer(16.0, 5)])
        holdout = buildHoldout()

        # DeepHoldout2 flattens internally — no DeepToImage after it.
        reference = render(settings, deepHoldout(source, holdout),
                           "b_ref", box=box)

        # b0: non-vacuity. "matches the reference" is free if BOTH sides
        # ignore the holdout, so pin that the reference is actually held out
        # before believing a bit-exact match (same guard as d0/e0/f0).
        unheld = render(settings, deepToImage(source, volumetric=True),
                        "b_unheld", box=box)
        heldDiff = compareImages(unheld, reference, box=box)
        checks.append(boolCheck("b", name + " b0 reference really is held out",
                                heldDiff.maxAbs > 0.1,
                                "%.3e" % heldDiff.maxAbs, "> 1e-01",
                                population=heldDiff.population(),
                                note="how far DeepHoldout2 moves the plain "
                                     "flatten of the same source"))

        node = makeDefocus(settings, source, holdout=holdout, size=0.0,
                           cocMode="manual")
        measured = render(settings, node, "b_deepc", box=box)
        diff = compareImages(reference, measured, box=box)
        checks.append(tolCheck("b", name + " vs DeepHoldout2", diff.maxAbs,
                               tol, population=diff.population(),
                               note="worst " + diff.where()))

        # Cross-check against the same flatten scene (a) is gated on, so a
        # disagreement can be attributed to the right side.
        crossRef = render(settings,
                          deepToImage(deepMergeHoldout(source, holdout),
                                      volumetric=True),
                          "b_ref_merge", box=box)
        crossDiff = compareImages(crossRef, measured, box=box)
        checks.append(tolCheck("b", name + " vs DeepMerge2 holdout+DeepToImage",
                               crossDiff.maxAbs, tol,
                               population=crossDiff.population(),
                               note="worst " + crossDiff.where()))
    return checks


# --- scene (c) ---------------------------------------------------------------

def sceneC(settings):
    """Energy conservation.

    Band-alpha and flat-field readings are reported SEPARATELY (they read two
    orders of magnitude apart on the same input): the flat-field rows are the
    interior value of a full-frame slab, the alpha-sum row is total scattered
    alpha over a bounded card, sharp versus defocused.
    """
    checks = []
    size = 12.0
    focus = 30.0

    # --- c1/c2: opaque full-frame slab, flat field in the bbox interior.
    resetScript()
    source = slab(9.0, 9.5, (0.40, 0.55, 0.70, 1.0))
    node = makeDefocus(settings, source, size=size, focusDistance=focus,
                       cocMode="manual")
    image = render(settings, node, "c_flat", box=formatBox())

    radius = size * abs(1.0 - focus / 9.25)
    inset = int(math.ceil(radius)) + 4
    interior = insetBox(formatBox(), inset)

    alpha = channelStats(image, "A", interior)
    checks.append(tolCheck("c", "c1 flat-field alpha, opaque slab |a-1|",
                           max(abs(alpha.minimum - 1.0),
                               abs(alpha.maximum - 1.0)),
                           1.0e-06,
                           population="interior %dx%d px"
                                      % (interior[2] - interior[0],
                                         interior[3] - interior[1]),
                           note="min %.7f max %.7f" % (alpha.minimum,
                                                       alpha.maximum)))
    red = channelStats(image, "R", interior)
    checks.append(tolCheck("c", "c2 flat-field colour spread (red)",
                           red.spread, 1.0e-06,
                           note="min %.7f max %.7f expect %.7f"
                                % (red.minimum, red.maximum, 0.40)))
    # Reported separately from the spread: alpha saturates at 1 and hides any
    # deficit, colour does not, so the flat field can be perfectly flat and
    # still sit below the input value.
    checks.append(tolCheck("c", "c2b flat-field colour vs input (red)",
                           abs(red.mean - 0.40), 1.0e-04,
                           note="mean %.7f input 0.4000000 (%.4f%% low)"
                                % (red.mean, (0.40 - red.mean) / 0.40 * 100.0)))

    # --- c3: band-alpha reading. Total scattered alpha over a bounded card,
    # sharp vs defocused, on the padded bbox so nothing scatters off-frame.
    resetScript()
    pad = int(math.ceil(radius)) + 6      # all the scattered energy, no more
    paddedBox = (-pad, -pad, 256 + pad, 256 + pad)
    card = pointLayer(rectangle2d((64, 64, 192, 192), (0.4, 0.55, 0.7, 1.0)),
                      9.25, keepZeroAlpha=False, premult=True)
    sharp = render(settings, makeDefocus(settings, card, size=0.0,
                                         cocMode="manual"),
                   "c_sharp", box=paddedBox)
    blurred = render(settings, makeDefocus(settings, card, size=size,
                                           focusDistance=focus,
                                           cocMode="manual"),
                     "c_blur", box=paddedBox)
    sharpSum = channelStats(sharp, "A", paddedBox).total
    blurSum = channelStats(blurred, "A", paddedBox).total
    ratio = blurSum / sharpSum if sharpSum else float("nan")
    # Gate tightened at T12's review from 1e-03: the reading is 8.984e-06 and
    # is INVARIANT across K=4/8/16/64/128 and was invariant across both
    # bucket-combine candidates too (measured, before M1.P3.T17 deleted one),
    # so 1e-03 left two orders of magnitude of dead slack.
    checks.append(tolCheck("c", "c3 band-alpha sum ratio |defocus/sharp - 1|",
                           abs(ratio - 1.0), 1.0e-04,
                           population="sum %.4f vs %.4f" % (blurSum, sharpSum),
                           note="ratio %.7f" % ratio))

    # --- c4: the same reading as c1 but on an alpha<1 full-range fog slab —
    # reported separately from c3 on purpose.
    resetScript()
    fog = slab(2.0, 30.0, (0.45, 0.45, 0.45, 0.90))
    fogImage = render(settings,
                      makeDefocus(settings, fog, size=size,
                                  focusDistance=focus, cocMode="manual"),
                      "c_fog", box=formatBox())
    fogInterior = insetBox(formatBox(), settings.maxRadius + 4)
    fogAlpha = channelStats(fogImage, "A", fogInterior)
    checks.append(tolCheck("c", "c4 flat-field alpha, a=0.9 full-range fog",
                           abs(fogAlpha.mean - 0.90), 1.0e-04,
                           population="interior %dx%d px"
                                      % (fogInterior[2] - fogInterior[0],
                                         fogInterior[3] - fogInterior[1]),
                           note="mean %.7f min %.7f max %.7f"
                                % (fogAlpha.mean, fogAlpha.minimum,
                                   fogAlpha.maximum)))
    return checks


# --- scene (d) ---------------------------------------------------------------

def sceneD(settings):
    """Sparse deep input => zero-sample regions exactly black."""
    checks = []
    cardBox = (64, 64, 192, 192)

    # --- d1: sharp. Every pixel outside the card must be bitwise zero —
    # read over the node's WHOLE output bbox, which is padded by max_radius,
    # not just the format. Restricting the read to the format left the pad
    # ring untested while the population line reported it as checked
    # (T12 review; measured 0 nonzero over the full ±max_radius ring).
    resetScript()
    card = pointLayer(rectangle2d(cardBox, (0.6, 0.4, 0.2, 1.0)),
                      10.0, keepZeroAlpha=False, premult=True)
    sharpBox = (-settings.maxRadius, -settings.maxRadius,
                FORMAT_W + settings.maxRadius, FORMAT_H + settings.maxRadius)
    sharp = render(settings, makeDefocus(settings, card, size=0.0,
                                         cocMode="manual"),
                   "d_sharp", box=sharpBox)

    # d0: non-vacuity — the card itself must be there, or "outside is black"
    # is satisfied by an empty frame.
    inside = channelStats(sharp, "A", insetBox(cardBox, 4))
    checks.append(boolCheck("d", "d0 card interior is present (alpha)",
                            inside.minimum > 0.999,
                            "min %.6f" % inside.minimum, "> 0.999"))

    nonZero, total = _countNonZeroOutside(sharp, cardBox, sharpBox)
    checks.append(boolCheck("d", "d1 size=0, outside-card pixels bitwise 0",
                            nonZero == 0, "%d nonzero" % nonZero, "== 0",
                            population="%d/%d px checked (whole padded bbox)"
                                       % (nonZero, total)))

    # --- d2: defocused. Beyond the scatter radius it must still be bitwise 0.
    resetScript()
    size, focus, depth = 12.0, 30.0, 10.0
    radius = size * abs(1.0 - focus / depth)
    margin = int(math.ceil(radius + 2.0)) + 2
    card = pointLayer(rectangle2d(cardBox, (0.6, 0.4, 0.2, 1.0)),
                      depth, keepZeroAlpha=False, premult=True)
    pad = margin + 8
    paddedBox = (-pad, -pad, 256 + pad, 256 + pad)
    blurred = render(settings, makeDefocus(settings, card, size=size,
                                           focusDistance=focus,
                                           cocMode="manual"),
                     "d_blur", box=paddedBox)
    grown = (cardBox[0] - margin, cardBox[1] - margin,
             cardBox[2] + margin, cardBox[3] + margin)
    nonZero, total = _countNonZeroOutside(blurred, grown, paddedBox)
    checks.append(boolCheck("d",
                            "d2 size=%g, beyond r=%.1f+margin bitwise 0"
                            % (size, radius),
                            nonZero == 0, "%d nonzero" % nonZero, "== 0",
                            population="%d/%d px checked" % (nonZero, total)))
    return checks


def _countNonZeroOutside(image, keepBox, testBox):
    kx0, ky0, kx1, ky1 = keepBox
    x0, y0, x1, y1 = testBox
    nonZero = 0
    total = 0
    for y in range(y0, y1):
        rows = dict((c, image.row(c, y)) for c in RGBA)
        inKeepRow = ky0 <= y < ky1
        for x in range(x0, x1):
            if inKeepRow and kx0 <= x < kx1:
                continue
            total += 1
            i = x - image.x0
            for c in RGBA:
                v = rows[c][i] if 0 <= i < image.width else 0.0
                if v != 0.0:
                    nonZero += 1
                    break
    return nonZero, total


# --- scene (e) ---------------------------------------------------------------

def sceneE(settings):
    """Occlusion: three depth-separated cards, FG defocused over a sharp mid.

    The `Escape` mid-cook cancel clause is NOT exercised — headless Nuke
    cannot trigger a recoverable mid-cook cancel (`nuke.cancel()` from a timer
    thread does nothing, SIGINT kills the process).  It is reported SKIP and
    stays with the interactive pass M1.P3.T5 already owes.
    """
    checks = []
    resetScript()

    fgBox = (30, 30, 120, 220)
    midBox = (90, 60, 200, 200)
    size, focus = 8.0, 10.0
    fgZ, midZ, bgZ = 4.0, 10.0, 20.0

    fg = cropDeep(slab(fgZ, fgZ + 0.05, (1.0, 0.0, 0.0, 1.0)), fgBox)
    mid = cropDeep(slab(midZ, midZ + 0.05, (0.0, 1.0, 0.0, 1.0)), midBox)
    bg = slab(bgZ, bgZ + 0.05, (0.0, 0.0, 1.0, 1.0))
    source = deepMerge([fg, mid, bg])

    fgRadius = size * abs(1.0 - focus / (fgZ + 0.025))
    pad = int(math.ceil(fgRadius)) + 8
    paddedBox = (-pad, -pad, 256 + pad, 256 + pad)
    image = render(settings,
                   makeDefocus(settings, source, size=size,
                               focusDistance=focus, cocMode="manual"),
                   "e_occlusion", box=paddedBox)

    # e0: non-vacuity — the BG must actually be visible where nothing
    # occludes it, or e3's "no bleed" reading is free.
    corner = channelStats(image, "B", (230, 230, 250, 250))
    checks.append(boolCheck("e", "e0 BG visible where unoccluded (blue)",
                            corner.minimum > 0.9,
                            "min %.4f" % corner.minimum, "> 0.9",
                            note="frame corner, outside both cards"))

    # e1: the in-focus mid card keeps a one-pixel edge. Scanned at a y where
    # the FG card's bloom cannot reach the mid card's right edge.
    scanY = 130
    soft = 0
    for x in range(midBox[2] - 6, midBox[2] + 7):
        g = image.at("G", x, scanY)
        if 0.01 < g < 0.99:
            soft += 1
    checks.append(boolCheck("e", "e1 in-focus mid card edge width",
                            soft <= 1, "%d px in (0.01,0.99)" % soft, "<= 1 px",
                            note="scanline y=%d across x=%d" % (scanY,
                                                                midBox[2])))

    # e2: the defocused FG blooms outward by its own CoC radius, over the
    # sharp mid card. Red is FG-only, so the channel isolates it.
    lastRed = fgBox[2] - 1
    for x in range(fgBox[2], fgBox[2] + int(fgRadius) + 12):
        if image.at("R", x, scanY) > 1.0e-4:
            lastRed = x
    bloom = lastRed - (fgBox[2] - 1)
    checks.append(boolCheck("e", "e2 FG bloom extent vs CoC radius",
                            abs(bloom - fgRadius) <= 1.5,
                            "%d px" % bloom, "%.2f +/- 1.5 px" % fgRadius,
                            note="red beyond the sharp FG silhouette"))

    # e3: the opaque FG must not let the BG through. Sampled deep inside the
    # FG silhouette, away from the mid card.
    interior = channelStats(image, "B", (fgBox[0] + int(fgRadius) + 4,
                                         fgBox[1] + int(fgRadius) + 4,
                                         min(midBox[0], fgBox[2]) - int(fgRadius) - 4,
                                         fgBox[3] - int(fgRadius) - 4))
    checks.append(tolCheck("e", "e3 BG bleed inside opaque FG (max blue)",
                           interior.maximum, 1.0e-04,
                           population="interior mean %.3e" % interior.mean))

    checks.append(Check("e", "e4 Escape mid-cook cancel", "-", "-", SKIP,
                        note="not exercisable headless; owed by M1.P3.T5's "
                             "interactive pass"))
    return checks


# --- scene (f) ---------------------------------------------------------------

def sceneF(settings):
    """Volumetric holdout: partial, depth-graded attenuation.

    f1 measures the fog slab's in-span attenuation against the analytic
    (1-a)^t.  f2 pins the documented M1.P3.T10 log-chord erasure — a holdout
    still erases genuinely-unoccluded source geometry for ~depthRange/K in
    front of it; M1.P3.T18 judged that residual in its interpolant bake-off
    and KEPT it (the log chord won — the deleted alternates leaked source
    through dense volumetric holdouts instead, the forbidden direction), so
    this stays a bounded XFAIL, reported here, not fixed.
    f3 is the fog-density reading against M1.P3.T8's coverage-head fix.
    """
    checks = []
    depths = [6.0, 8.5, 11.0, 13.5, 16.0, 18.5, 21.0, 23.5]

    # --- f0: non-vacuity. With the holdout DISCONNECTED every strip must read
    # 1.0, so f1/f2's attenuation is attributable to the holdout and nothing
    # else. (The milestone has already been burned once by a holdout scene
    # that executed no holdout code at all — Decisions, 2026-07-27.)
    resetScript()
    source, boxes = stripSource(depths)
    bare = render(settings,
                  makeDefocus(settings, source, size=0.0, cocMode="manual"),
                  "f_noholdout", box=formatBox())
    # min/max, not the mean: a mean lets a +eps pixel cancel a -eps one, and
    # this row is the guard that the whole scene is not measuring a holdout
    # that never ran (T12 review).
    bareStats = [channelStats(bare, "A", (boxes[i][0] + 6, 16,
                                          boxes[i][1] - 6, 240))
                 for i in range(len(depths))]
    bareValues = [s.mean for s in bareStats]
    worstBare = max(max(abs(s.minimum - 1.0), abs(s.maximum - 1.0))
                    for s in bareStats)
    checks.append(tolCheck("f", "f0 holdout disconnected: strips read 1.0",
                           worstBare, 1.0e-06,
                           population="%d strips" % len(depths),
                           note=" ".join("z%.1f:%.4f" % (depths[i],
                                                         bareValues[i])
                                         for i in range(len(depths)))))

    # --- f1: fog slab holdout, alpha 0.9 across [5, 25].
    resetScript()
    fogFront, fogBack, fogAlpha = 5.0, 25.0, 0.90
    source, boxes = stripSource(depths)
    holdout = slab(fogFront, fogBack, (0.0, 0.0, 0.0, fogAlpha))
    image = render(settings,
                   makeDefocus(settings, source, holdout=holdout, size=0.0,
                               cocMode="manual"),
                   "f_fog", box=formatBox())

    worst = 0.0
    worstStrip = -1
    measuredValues = []
    expectedValues = []
    for i, z in enumerate(depths):
        x0, x1 = boxes[i]
        stats = channelStats(image, "A", (x0 + 6, 16, x1 - 6, 240))
        t = min(1.0, max(0.0, (z - fogFront) / (fogBack - fogFront)))
        expected = (1.0 - fogAlpha) ** t
        measuredValues.append(stats.mean)
        expectedValues.append(expected)
        deviation = abs(stats.mean - expected)
        if deviation > worst:
            worst = deviation
            worstStrip = i
    monotone = all(measuredValues[i] <= measuredValues[i - 1] + 1e-6
                   for i in range(1, len(measuredValues)))
    detail = " ".join("z%.1f:%.4f/%.4f" % (depths[i], measuredValues[i],
                                           expectedValues[i])
                      for i in range(len(depths)))
    # Gate tightened at T12's review from 2.0e-02, which was six orders of
    # magnitude of dead slack. log T is LINEAR in z across a single
    # exponential fog span, so the log chord is exact here and the reading is
    # 4.367e-08 — measured invariant across K=4/8/16/32/64/128, all three
    # of the pre-T18 holdout interpolants, pre_merge on/off and
    # merge_tolerance 0.25/2.0.
    # THIS IS THE READING THAT DECIDED M1.P3.T17.  Under the deleted bucket
    # composite it was 2.500e-01: an opaque fragment's transmittance split is a
    # no-op, so both bucket deposits carried the full `1*vis` and plain `over`
    # composited them as independent layers, rendering `2*vis - vis^2` where the
    # truth is `vis` — +50% relative at vis = 0.5, on the node's differentiating
    # feature, at size 0.  Confirmed to four decimals at every strip above.
    # 1e-06 catches anything smaller than a quarter of that.
    checks.append(tolCheck("f", "f1 fog-slab vis vs analytic (1-a)^t",
                           worst, 1.0e-06,
                           population="worst strip %d (z=%.1f) of %d"
                                      % (worstStrip, depths[worstStrip],
                                         len(depths)),
                           note=detail))
    checks.append(boolCheck("f", "f1b attenuation monotone in depth",
                            monotone, "monotone" if monotone else "NOT monotone",
                            "non-increasing"))

    # --- f2: opaque card holdout, with the strips packed into the bracket
    # immediately in front of it. The two outer strips anchor the frame's
    # depth range so the bracket width is a known (range/K), not an artefact
    # of where the probes happen to sit.
    resetScript()
    rangeLow, rangeHigh = 5.0, 25.0
    bracket = (rangeHigh - rangeLow) / float(settings.k)
    # The holdout LUT's boundaries are uniform in Z over the frame's measured
    # depth range, so put the holdout deliberately INSIDE a bracket (not on a
    # boundary, where the erasure is invisible) and spread the probe strips
    # across that same bracket in front of it. Scaling off `bracket` keeps
    # this valid when T16/T17/T18 change K.
    bracketLow = rangeLow + (settings.k // 2) * bracket
    holdoutZ = bracketLow + 0.80 * bracket
    # Probe fractions of the bracket, STRADDLING its lower boundary. The
    # erasure runs from that boundary up to the holdout, so a coarse grid
    # reports its own step instead of the defect: the original
    # +0.05/+0.25/+0.45/+0.65/+0.75 grid returned exactly 0.75*bracket at
    # K=8/16/32/64 alike, which is the grid, not a measurement. +/-0.02 either
    # side pins the onset to 2% of a bracket (T12 review; the true onset is
    # the boundary itself — vis reads 1.0 at -0.02 and 0.251 at +0.02,
    # decaying as kMinTransmittance^frac = 10^(-30*frac) across the bracket).
    probeFractions = [-0.30, -0.02, 0.02, 0.10, 0.30, 0.50, 0.70]
    probeDepths = ([rangeLow]
                   + [bracketLow + f * bracket for f in probeFractions]
                   + [rangeHigh])
    source, boxes = stripSource(probeDepths)
    holdout = slab(holdoutZ, holdoutZ + 0.05, (0.0, 0.0, 0.0, 1.0))
    image = render(settings,
                   makeDefocus(settings, source, holdout=holdout, size=0.0,
                               cocMode="manual"),
                   "f_opaque", box=formatBox())
    erased = []
    values = []
    stripPixels = 0
    erasedPixels = 0
    lastClean = None                    # deepest probe in front that survives
    for i, z in enumerate(probeDepths):
        x0, x1 = boxes[i]
        region = (x0 + 6, 16, x1 - 6, 240)
        stats = channelStats(image, "A", region)
        values.append(stats.mean)
        if z < holdoutZ:
            stripPixels += stats.count
            if stats.mean < 0.999:
                erased.append((z, stats.mean))
                erasedPixels += stats.count
            elif not erased:
                lastClean = z
    onset = min(z for z, _ in erased) if erased else holdoutZ
    erasedDepth = holdoutZ - onset
    erasedFraction = erasedDepth / bracket
    detail = " ".join("z%.3f:%.4f" % (probeDepths[i], values[i])
                      for i in range(len(probeDepths)))
    if erased and lastClean is not None:
        onsetNote = ("erasure onset in (%.4f, %.4f], i.e. %+.1f%% to %+.1f%% "
                     "of the bracket from its lower boundary %.4f — the "
                     "resolution of this grid, not a floor"
                     % (lastClean, onset,
                        100.0 * (lastClean - bracketLow) / bracket,
                        100.0 * (onset - bracketLow) / bracket, bracketLow))
    else:
        onsetNote = "no erasure onset bracketed by this grid"
    # HARD BOUND on this XFAIL, same discipline as scene (g)'s g1/g2/g3
    # (harness.boolCheck's hardTol/hardValue, reusing tolCheck's mechanism).
    # erasedFraction is (erasedDepth / bracket): measured EXACTLY 0.78 at
    # K=4/8/16/64/128 (M1.P3.T16 review) — it is pinned by the probe grid, not
    # by K, because onset always lands on the +0.02-of-bracket probe (the
    # true onset is the bracket's lower boundary itself, per the comment
    # above). A regression that pushes the onset out to the NEXT probe on the
    # grid (-0.02, then -0.30) cannot land between 0.78 and 1.00 at all — it
    # jumps straight to 0.82, then 1.10 — so 1.00 sits in the gap the grid
    # itself guarantees is empty: comfortably above the documented 0.78 and
    # comfortably below the next value a regression could even produce.
    F2_HARD = 1.00
    checks.append(boolCheck(
        "f", "f2 opaque holdout erases unoccluded FG (M1.P3.T10)",
        not erased,
        "%d strip(s); %.3f depth units (%.0f%% of the bracket) erased in front"
        % (len(erased), erasedDepth, 100.0 * erasedFraction),
        "0 strips (xfail < %.0f%% of bracket)" % (100.0 * F2_HARD),
        population="%d/%d unoccluded strip px erased; %s"
                   % (erasedPixels, stripPixels,
                      ", ".join("z=%.3f -> %.4f" % e for e in erased) or "none"),
        note="holdout z=%.3f, bracket (range/K) = %.3f spanning [%.3f, %.3f]; "
             "%s; %s"
             % (holdoutZ, bracket, bracketLow, bracketLow + bracket,
                onsetNote, detail),
        expectedFailure=True, hardTol=F2_HARD, hardValue=erasedFraction))

    # --- f3: fog density against M1.P3.T8's coverage-head fix. Two
    # fully-covering 50% layers composite to exactly 0.75. Three arrangements,
    # reported separately, because the residual only appears when fragments
    # carrying DIFFERENT split fractions land in one bucket.
    exact = 1.0 - 0.5 * 0.5
    grey = (0.5, 0.5, 0.5, 0.5)

    def separatedSlabs():
        return deepMerge([slab(6.0, 10.0, grey), slab(12.0, 16.0, grey)])

    def overlappingSlabs():
        return deepMerge([slab(8.0, 12.0, grey), slab(9.0, 13.0, grey)])

    def spanPlusPoint():
        return deepMerge([slab(5.0, 25.0, grey),
                          pointLayer(constant2d(grey), 10.0,
                                     keepZeroAlpha=False, premult=False)])

    # HARD BOUNDS on f3c/f3d, same discipline as scene (g) (harness.boolCheck's
    # hardTol/hardValue, reusing tolCheck's mechanism) — measured against the
    # exact fraction, |mean-exact|/exact, i.e. the loss %.
    #   f3c (overlapping slabs) is K-CONVERGENT: 0.763/0.610/0.113% at
    #   K=4/8/16, exact (0.0003%) at K=64/128 (M1.P3.T16 review, reproducing
    #   M1.P3.T12's K=4/8/16 numbers exactly). Worst on record is K=4's
    #   0.763%; F3C_HARD is ~2.6x that.
    #   f3d (span + point) is NON-MONOTONE in K: 1.539/1.395/1.675/1.950% at
    #   K=4/8/16/64, exact (0.0004%) at K=128 (M1.P3.T16 review, reproducing
    #   M1.P3.T12's K=4/8/16 numbers exactly and confirming the K=64 peak and
    #   K=128 convergence). Worst on record is K=64's 1.950%; F3D_HARD is
    #   ~2.6x that, the same multiple as f3c's for a residual whose peak K
    #   is not pinned down as tightly (non-monotone, only sampled at
    #   K=4/8/16/64/128).
    F3C_HARD = 2.0e-02
    F3D_HARD = 5.0e-02
    densityRows = [
        ("f3  fog density, size=0, separated slabs", separatedSlabs, 0.0,
         1.0e-05, 8, False, None),
        ("f3b fog density, defocused, separated slabs", separatedSlabs, 12.0,
         1.0e-05, settings.maxRadius + 4, False, None),
        ("f3c fog density, defocused, overlapping slabs", overlappingSlabs,
         12.0, 1.0e-05, settings.maxRadius + 4, True, F3C_HARD),
        ("f3d fog density, defocused, span + point (shared bucket, "
         "different split fractions)", spanPlusPoint, 12.0, 1.0e-05,
         settings.maxRadius + 4, True, F3D_HARD),
    ]
    for name, build, size, tol, inset, documented, hardPct in densityRows:
        resetScript()
        node = makeDefocus(settings, build(), size=size, focusDistance=30.0,
                           cocMode="manual")
        img = render(settings, node, "f_density", box=formatBox())
        stats = channelStats(img, "A", insetBox(formatBox(), inset))
        loss = (exact - stats.mean) / exact * 100.0
        gate = "0.75 +/- %.0e" % tol
        if hardPct is not None:
            gate += " (xfail < %.1f%% loss)" % (100.0 * hardPct)
        checks.append(boolCheck(
            "f", name, abs(stats.mean - exact) <= tol,
            "%.7f (%+.3f%%)" % (stats.mean, -loss), gate,
            population="interior flat field, %d px" % stats.count,
            # The Decisions entry of 2026-07-26 calls this a "~4%/layer LOSS"
            # and "identical under both bucket-combine candidates". Neither
            # held as stated: the deviation is SIGNED and flipped with the
            # candidate (-0.113%/-1.675% under the shipped composite against
            # +0.074%/+0.967% under the one M1.P3.T17 deleted), which is why
            # T17 weighed it as evidence rather than as a common-mode term.
            note="documented different-split-fraction residual (Decisions "
                 "2026-07-26); signed, and NOT candidate-independent as that "
                 "entry states — see the T12 review and M1.P3.T17"
                 if documented else "",
            expectedFailure=documented,
            hardTol=hardPct,
            hardValue=(abs(loss) / 100.0) if hardPct is not None else None))

    checks.extend(unequalDensityChecks(settings))
    return checks


# --- f3e-f3i: the UNEQUAL-DENSITY over-read (M1.P3.T22) -----------------------
#
# WHY THIS EXISTS.  f3c/f3d above are the only other rendered checks with
# overlapping depth content, and BOTH pin EQUAL density — the one arrangement
# the bucket composite is nearly exact on.  Two cards at DIFFERENT densities
# whose depth spans overlap (a dense fog card beside a thin one) read tens of
# percent HIGH, i.e. the node invents alpha, which is the direction the
# Design reference's coverage-deficit spec explicitly forbids ("the saturation
# rule never scales alpha up to hide this").  Nothing in this harness saw that
# until this task; f3e/f3f are the gate M1.P3.T23 has to close, and f3g/f3h
# are the controls that say what the gate is actually measuring.
#
# THE ORACLE, AND WHY IT NEEDS NO ORDERING ASSUMPTION.  Below saturation the
# node's own area model is ADDITIVE: two parents whose kernel weights claim
# w_A and w_B of a destination pixel with w_A + w_B <= 1 occupy DISJOINT
# sub-areas of it, so the pixel's alpha is w_A*alpha_A + w_B*alpha_B whatever
# their relative depth order.  Each card RENDERED ALONE is that same model
# with one term, so
#
#     merged  ==  soloA + soloB          (per pixel, below saturation)
#
# and neither side of that identity is a re-run of the thing under test on
# its own output: the two solo renders are measurements of the node, and the
# identity between them is hand-derived.
#
# THE ONE CONFOUND, AND HOW IT IS REMOVED.  The node measures its depth range
# per cook, and the range drives the bucket boundaries AND the kernel LUT's
# extent — so a card rendered ALONE is bucketed and rasterised differently
# from the same card rendered beside another one, and the identity above would
# be comparing two different renders of card A.  Measured, that confound is
# NOT small: with no anchor, adding a SPATIALLY DISTANT second card (its blur
# never reaches the probed pixels, verified per pixel) moves card A's own
# alpha by 3.04%.  RE-MEASURED AT THIS TASK'S REVIEW, which corrected what
# drives it: the magnitude tracks the DISTANT CARD'S OWN DEPTH SPAN — i.e. how
# far it moves the measured range — and NOT the probed card's alpha or
# thickness, which is how the first version of this comment read it.  Card A
# thick at alpha 0.99, size 14, second card 40x40 at alpha 0.10: its span
# [9,13] moves A by 3.04%, [16,20] by 9.65%, [14,16] by 13.68% and [4,6] by
# 44.45%.  The same sweep on a THIN card (span [10,10.05]) never exceeds
# 0.0013%, and card A at alpha 0.10 never exceeds 1.97% — so the "13.68% at
# alpha 0.99 / 30.3% on a thin card" reading was the right order of magnitude
# attributed to the wrong variable.  It is removed by
# putting the SAME range anchor — a small, faint card in the corner spanning
# the whole working depth range — in EVERY render of the family, solo and
# merged alike, so all of them measure the identical range, build the
# identical bucket set and the identical LUT.  With it in place that same
# distant card moves card A's own pixels by EXACTLY 0.00000%, over 1342-3921
# probed pixels in all SIXTEEN combinations of K in {16, 64} x size in
# {14, 20} x alpha in {0.99, 0.10} x span in {thick, thin} (re-measured
# independently at this task's review) — a
# bit-exact decomposition, not an approximate one — so every difference
# the family reports is the bucket composite pooling two parents' deposits at
# one destination pixel and nothing else.  On f3e's own cell the anchor is
# worth 1.9 points (+79.278% unanchored against +77.411% with it), and on the
# cells whose two spans COINCIDE it is worth nothing at all, since those
# measure the same range either way — it is not there because the readings
# would otherwise be wildly wrong, it is there so the identity is exact by
# construction rather than approximately true, which is what lets a 0.5% gate
# mean anything.
#
# Pixels the anchor's own blur reaches are DROPPED (it must contribute exactly
# 0 to a probed pixel, which is checked per pixel rather than argued from
# geometry), and so are pixels whose estimated coverage w_A + w_B exceeds
# UNEQ_COVER_MAX, where the area model stops being additive because the two
# deposits genuinely overlap.
#
# DOES THE ANCHOR PICK THE NUMBER?  Swept at this task's review, and no.  Its
# ALPHA (0.02 / 0.05 / 0.30), its SIZE (8x8 / 16x16) and its POSITION (either
# corner) leave f3e BIT-IDENTICAL at +77.4113% — the anchor enters only through
# the measured range, and every one of those variants measures the same range.
# Only its SPAN moves the reading, monotonically and in the CONSERVATIVE
# direction: [8,13] (the cards' own natural range, i.e. what an unanchored
# render measures) reads +79.269%, [8,14] +78.296%, the shipped [8,16]
# +77.411% and [7,20] +73.886%.  So a WIDER pinned range UNDERSTATES the
# defect, and the shipped span is the narrowest one that still brackets every
# span the sweep uses — it is not tuned to flatter the number.
#
# AND IS UNEQ_COVER_MAX HONEST?  The worry is the mirror image of the confound:
# a coverage guard set too loose would admit pixels where the composite
# genuinely (and correctly) occludes, and report that as defect.  It does not,
# and the two controls are what prove it: f3g and f3h probe up to the SAME
# coverage 0.90 and read -0.003% and -0.000%.  If additivity broke at high
# coverage as such, they would show it too.  Confirmed from the other side by
# the tHeadIn=1 mutation in the table below, which takes f3e's LOW arm to
# -0.001%: a saturation artefact would survive that perturbation, an occlusion
# term cannot.  Every reading in this family, both arms, is the composite.
#
# WHAT MAKES THESE FIVE CHECKS FAIL — the question this milestone has been
# burned five times for not asking.  Eight perturbations of
# compositePixelCoveragePartition() were built at M1.P3.T22 and rendered
# through this family (each into its own build tree; src/ was never modified);
# the seven that move something:
#
#   perturbation                                     f3e        f3h      f3i
#   the residual is never occluded (tHeadIn = 1)    +0.000 P   +0.000 P  0.6709 F
#   the residual carries 30% of its alpha           +76.8  F   +0.014 P  0.1806 F
#   tiles allocated OLDEST-first (T21's rejected)  +136.9  F   +19.7  F  0.5000 P
#   M1.P3.T9's rejected claimedArea divisor         +77.7  F    +5.98 F  0.5000 P
#   each tile over-occluded by 2x                   +53.5  F   -18.4  F  0.4130 F
#   the residual occluded by (1 - claimedArea)      +55.2  F   -27.8  F  0.0437 F
#   THE CONSERVATIVE RULE: a residual is occluded
#     by the DENSEST tile at the pixel              -52.3  F   +0.001 P  0.5000 P
#
# FOUR MORE BUILT INDEPENDENTLY AT THIS TASK'S REVIEW, same discipline:
#
#   perturbation                                     f3e        f3h      f3i
#   a UNIFORM +2% on the composite's output         +77.411 F  +0.078 P  0.5100 F
#   the residual sees the pooled tClaimed (pre-T20) +77.691 F  +5.984 F  0.5000 P
#   the residual allocated over the WHOLE mosaic    +80.251 F  +6.560 F  0.5000 P
#   kCompositeHeadTiles 16 -> 2 (partial T21 revert)+76.174 F  +0.039 P  0.5000 P
#
# THE FIRST OF THOSE IS A SECOND BLIND SPOT, of a different kind from the
# never-occlude one, and it is why f3i is load-bearing twice over.  The oracle
# is a RATIO of two measurements of the same node, so it is invariant under any
# uniform scaling of the composite's output: a +2% error on every pixel leaves
# f3e/f3f/f3g/f3h BIT-IDENTICAL (+77.411% / +83.648% / +0.000% / +0.078%,
# every digit).  Only f3i, which compares against a HAND-KNOWN alpha rather
# than against another render, sees it (1.0e-02 against a 1.0e-05 gate); f3c/f3d
# see it too (+1.885%).  Any future edit that weakens f3i re-opens both holes.
# THE LAST is the limit of this family's resolution and is stated so it is not
# discovered later: halving the tile stack moves f3e by only 1.2 points, so
# f3e/f3f BOUND the over-read but do not finely resolve stack-depth changes —
# the POD staggered-parent sweep is what catches that one (it does; the full
# scatter suite fails on it).
#
# BACK TO THE FIRST TABLE: two of its rows matter more than the rest.  ITS
# LAST ROW — the conservative rule — is the one M1.P3.T23 is
# most likely to reach for — bound the SIGN rather than the magnitude, occlude
# every residual by the densest thing in front of it — and it does exactly
# what the brief warns about: it turns f3e's +77.4% over-read into a -52.3%
# UNDER-read on 244 of 738 pixels.  f3e is gated on |deviation|, both ways, so
# it FAILS on that too: the gate cannot be satisfied by trading the error's
# sign.  ITS FIRST ROW is the family's own blind spot — additivity is trivially
# true of a composite that never occludes anything, so f3e/f3f/f3g/f3h all go
# green under it — and f3i is the row that exists to catch it (f3c/f3d catch
# it independently, at +33.3%).
#
# THE TWO ARMS ARE TWO DIFFERENT TERMS, AND M1.P3.T23 MUST NOT READ THEM AS
# ONE.  f3e's high arm (+77.411%) is this task's target: invented alpha.  Its
# low arm (-2.011%) is NOT the same defect seen from the other side — the
# `overlap: spans disjoint` cell in f3f separates them cleanly, reading
# +0.000% high and -3.278% low, i.e. all deficit and no over-read at all.  That
# term is a residual whose own disc OVERHANGS the head tile it belongs to
# spilling onto a FOREIGN parent's tile and being occluded by it, which needs
# only a shared tile stack and not a shared bucket; it is in the direction the
# honest-alpha contract PERMITS, and it was unmeasured before this task.  So
# closing the +77% alone leaves f3e RED on the low arm.  The |deviation| gate is
# still the right one — it is what stops the over-read being traded for a
# deficit — but "f3e is still FAIL" is not by itself evidence that the target
# is still open: read the two arms, and the disjoint cell, separately.
#
# WHAT M1.P3.T23 MEASURED, AND WHY THIS FAMILY STAYS RED.  Five candidate
# composite rules were built; the three that survived POD screening were
# rendered through this family (each in its own build tree, behind a runtime
# switch whose OFF setting reproduced every reading in the harness row for
# row).  None beats the trade, and the reason is that the HIGH arm is not one
# term either.  Sorted by whether a composite rule can reach the cell at all:
#
#   * UNREACHABLE FROM THE FOUR PLANES.  `overlap 100% (coincident spans)`
#     reads +80.428% -- the largest cell here at the default K -- and is
#     BIT-IDENTICAL under a fifth plane carrying the co-located alpha.  When
#     two parents' spans coincide, both heads land in the SAME bucket and every
#     later part likewise, so the planes for {A, B} are NUMERICALLY IDENTICAL
#     to those of one parent at the pooled density: one (coverage, co-located
#     area, alpha) triple per bucket and one head tile.  That is f3g's argument
#     one level up.  Anything measured on this cell FROM THESE FOUR PLANES is
#     measuring accumulation, not composition.  MECHANISM CONFIRMED AT THE
#     REVIEW, and it is sharper than "the planes look the same": in this cell
#     every bucket carries EITHER new area OR co-located area and never both
#     (both cards occupy the same z span, so both heads claim in the first
#     bucket and every later part is co-located), so the C_k : D_k split is
#     DEGENERATE and a fifth plane that only refines that split is a strict
#     no-op.  Bit-identity here therefore does NOT generalise to a coincident
#     pair whose heads land in different buckets.
#     BUT NOT "AT ANY PLANE COUNT" -- that clause was withdrawn at the review.
#     The dropped term is a COVARIANCE (the composite forms E[a_res]*E[1-a_head]
#     where truth wants E[a_res*(1-a_head)]), and a plane carrying the second
#     moment sum w_i*a_i^2 supplies the within-bucket opacity spread that closes
#     it: subtracting s_res*s_head from the residual's per-tile transmittance --
#     the plan's own untried "split the tile stack by opacity band" -- is a
#     strict no-op wherever either spread is zero (T21's staggered exactness and
#     the dense ramp stay BIT-IDENTICAL) and takes the coincident two-parent
#     shape from +17.6..+86.3% to -5.8..+15.6%, exactly 0.000% at two parts.
#     Measured on hand-built planes, NOT shipped: it leaves the staggered/offset
#     cells unmoved (+51.7%), doubles f3c/f3d's deficit, and costs more planes
#     than the fifth.  What is permanent is the weaker claim: parent count per
#     bucket is unbounded, so no FIXED plane count recovers parent identity.
#   * REACHABLE, but only with the fifth plane AND at a price.  The cells whose
#     two heads land in DIFFERENT buckets do move: `overlap 25%` +52.251% ->
#     +5.464%, `overlap 0%` +20.906% -> +0.206%, base +77.411% -> +34.664%,
#     under the fifth plane plus allocating the residual's alpha by each tile's
#     own per-unit opacity.  The same rule takes the disjoint-span DEFICIT cell
#     from -3.278% to -38.636% and turns f3h -- an arrangement the composite is
#     EXACT on -- into a +6.510% FAIL.  A trade, and one that costs +14% of the
#     bucket planes at C=4 and +38-41% of the composite's time.
#   * THE FIFTH PLANE ALONE (allocation unchanged) moves this family by 2.7
#     points (+77.411% -> +74.702%) while taking f3c and f3d to EXACT.  So the
#     ruling that f3c/f3d are precision and f3e/f3f are correctness is not just
#     a scoping decision, it is two different mechanisms: one plane closes the
#     first pair outright and barely touches the second.
#
# SO DO NOT READ A CANDIDATE'S f3e MOVEMENT WITHOUT THE `Cells:` DETAIL, and do
# not expect the headline to reach zero: roughly half of it cannot be reached
# from here.  Also note the ALPHA scaling, measured at M1.P3.T23 through this
# same oracle: THIS FAMILY'S over-read scales WITH alpha, and its equal-density
# arm is mild at low alpha.  READ THAT AS A STATEMENT ABOUT THIS RIG ONLY.  It
# is NOT true of the node's other alpha<1 rig: scene (g)'s ramp (the g4 rig)
# reads +6.53% at alpha 0.10 and +5.17% at alpha 0.30 (K=2/4), +5.93% and
# +3.40% at the default K=16, i.e. it scales INVERSELY with alpha, and at
# alpha 0.90 it is a -3.25% DEFICIT.  Both were re-measured at T23's review and
# both reproduce; M1.P3.T23 read the g4-rig figures against THIS oracle, found
# different numbers and reported the record as unreproducible, which it is not.
# Low alpha IS part of the milestone's target -- on the g4 rig, not here.  The
# unequal-density arm here is not mild at low alpha either: at the 10:1 ratio
# alpha 0.30 reads +6.49%, thirteen times this gate.  Equal-
# density staggered twins (f3f's `ratio 1.00` cell) read +0.370% at alpha 0.10,
# +1.619% at 0.30, +2.931% at 0.50, +6.658% at 0.90 and +8.373% at 0.99, all at
# K=16, and the K sweep CONVERGES (plateau by K=32, e.g. alpha 0.50 reads
# +2.030/+2.437/+2.931/+3.070/+3.083/+3.070 at K=4/8/16/32/64/128).  At the
# 10:1 density ratio, K=16: +1.831% / +6.461% / +12.808% / +43.182% / +77.411%.
#
# f3g is the one row here that is an ATTRIBUTION control rather than a
# detector, and it is STRUCTURALLY so rather than by luck: the bucket planes
# are per-destination-pixel SUMS, so two cards at the same depth pool into one
# (coverage, alpha) entry and the composite cannot tell them from a single
# parent.  Nothing on the composite side can move it, and nothing did — it
# reads +0.000% under all eight perturbations tried, including one aimed at
# the fit/claim path rather than the residual.  That IS its finding (the
# over-read needs the residual chain that spreads a parent across buckets, and
# is not about the density ratio as such), but it provides no detection, and
# saying so is the point.

UNEQ_CARD_A = (84, 108, 124, 148)       # 40x40, 8 px apart in x
UNEQ_CARD_B = (132, 108, 172, 148)
UNEQ_ANCHOR = (2, 2, 18, 18)
UNEQ_ANCHOR_Z = (8.0, 16.0)             # brackets every span the family uses
# The range is measured from an ALPHA-WEIGHTED histogram whose outermost bins
# are clipped below 1e-4 of the frame's total alpha mass, so the anchor has to
# carry more than that to pin it: 16*16 px at 0.05 is 12.8 units against a
# worst-case clip of 0.63 for two alpha-0.99 cards, i.e. 20x margin.
UNEQ_ANCHOR_ALPHA = 0.05
UNEQ_FLOOR = 0.005                      # both cards must really be present
UNEQ_COVER_MAX = 0.90                   # stay out of the saturation regime
# The gate, in BOTH directions.  0.5% of the true alpha is well under the
# 1/255 an 8-bit view resolves at these levels, and it is the same "over
# +0.5%" threshold M1.P3.T20/T21 state their sweeps in.  Two-sided on purpose:
# a one-sided ceiling could be satisfied by trading the over-read for a
# deficit of the same size, which is not a fix.
UNEQ_TOL = 5.0e-03

_uneqAnchorCache = {}


def _uneqCard(box, zFront, zBack, alpha, tint):
    """A premultiplied volumetric card: a slab cropped to a rectangle."""
    color = (alpha * tint[0], alpha * tint[1], alpha * tint[2], alpha)
    return cropDeep(slab(zFront, zBack, color), box)


def _uneqAnchor():
    return _uneqCard(UNEQ_ANCHOR, UNEQ_ANCHOR_Z[0], UNEQ_ANCHOR_Z[1],
                     UNEQ_ANCHOR_ALPHA, (0.5, 0.5, 0.5))


def _uneqRender(settings, size, builders, tag):
    """Render one arrangement.  ``builders`` are called AFTER resetScript(),
    so every render of a cell builds its own graph from scratch."""
    resetScript()
    layers = [build() for build in builders]
    source = deepMerge(layers) if len(layers) > 1 else layers[0]
    node = makeDefocus(settings, source, size=size, focusDistance=30.0,
                       cocMode="manual")
    return render(settings, node, tag, box=formatBox())


def unequalDensityCell(settings, size, alphaA, alphaB, zA, zB, step=2):
    """merged vs soloA+soloB for one (density, overlap, size, K) cell."""
    def A():
        return _uneqCard(UNEQ_CARD_A, zA[0], zA[1], alphaA, (0.7, 0.5, 0.3))

    def B():
        return _uneqCard(UNEQ_CARD_B, zB[0], zB[1], alphaB, (0.3, 0.5, 0.7))

    # The anchor render depends only on the settings and the size, so it is
    # shared across the cells of a sweep rather than re-rendered per cell.
    # describe() rather than settings.k: the cache must not survive a change
    # to pre_merge/max_radius either.
    key = (settings.describe(), size)
    if key not in _uneqAnchorCache:
        _uneqAnchorCache[key] = _uneqRender(settings, size, [_uneqAnchor],
                                            "f_uneq_anchor")
    imgN = _uneqAnchorCache[key]
    imgA = _uneqRender(settings, size, [A, _uneqAnchor], "f_uneq_soloA")
    imgB = _uneqRender(settings, size, [B, _uneqAnchor], "f_uneq_soloB")
    imgM = _uneqRender(settings, size, [A, B, _uneqAnchor], "f_uneq_merged")

    x0, y0, x1, y1 = formatBox()
    high, highAt, highTriple = 0.0, None, None
    low, lowAt = 0.0, None
    over = under = probed = 0
    for y in range(y0, y1, step):
        rowN = imgN.row("A", y)
        rowA = imgA.row("A", y)
        rowB = imgB.row("A", y)
        rowM = imgM.row("A", y)
        for x in range(x0, x1, step):
            if rowN[x - imgN.x0] != 0.0:
                continue                        # the anchor reaches here
            a = rowA[x - imgA.x0]
            b = rowB[x - imgB.x0]
            if a < UNEQ_FLOOR or b < UNEQ_FLOOR:
                continue                        # not a two-card pixel
            # A solo card's alpha IS w*alpha (one parent, one density, which
            # the composite does exactly — f3g/f3h), so w = alpha_out/alpha_in.
            if a / alphaA + b / alphaB > UNEQ_COVER_MAX:
                continue                        # saturation regime
            probed += 1
            m = rowM[x - imgM.x0]
            rel = (m - (a + b)) / (a + b)
            if rel > high:
                high, highAt, highTriple = rel, (x, y), (a, b, m)
            if rel < low:
                low, lowAt = rel, (x, y)
            if rel > UNEQ_TOL:
                over += 1
            elif rel < -UNEQ_TOL:
                under += 1
    return dict(high=high, highAt=highAt, highTriple=highTriple,
                low=low, lowAt=lowAt, over=over, under=under, probed=probed)


def soloCardInterior(settings, size=4.0, alphas=(0.50, 0.10), inset=12):
    """The PREMISE f3e/f3f's oracle rests on, measured rather than assumed.

    ``merged == soloA + soloB`` is only a statement about the composite if
    each SOLO render is itself right — and a solo card is not a trivial case:
    it is a volumetric parent cut into parts, i.e. one head deposit and a
    residual chain, which is the very machinery f3e stresses.  So this reads a
    single card's alpha where its own blur covers the destination pixel
    completely, against the alpha it was built with.

    It is also what stops the family being satisfied DEGENERATELY.  The
    additive oracle is trivially true of a composite that never occludes a
    co-located deposit at all — measured: such a mutation takes f3e/f3f to
    +0.000% and PASSING — and this row is what catches that, reading 0.6709
    against 0.50 under the same mutation.  (f3c/f3d catch it too, at +33.3%;
    two independent detections of the same degenerate rule is the point.)

    ``size`` is small on purpose: the card has to be wider than its own blur
    for any pixel to reach full coverage, and a 40x40 card at size 4 has a
    16x16 core that does.
    """
    out = []
    for alpha in alphas:
        def card(alpha=alpha):
            return _uneqCard(UNEQ_CARD_A, 8.0, 12.0, alpha, (0.7, 0.5, 0.3))
        img = _uneqRender(settings, size, [card, _uneqAnchor],
                          "f_uneq_solo_interior")
        stats = channelStats(img, "A", insetBox(UNEQ_CARD_A, inset))
        out.append(dict(alpha=alpha, mean=stats.mean, min=stats.minimum,
                        max=stats.maximum, count=stats.count,
                        truth=alpha,
                        worst=max(abs(stats.minimum - alpha),
                                  abs(stats.maximum - alpha))))
    return out


def _uneqMeasured(cell):
    return "%+.3f%% high / %+.3f%% low" % (100.0 * cell["high"],
                                           100.0 * cell["low"])


def _uneqPopulation(cell):
    if cell["highTriple"] is None:
        return "%d probed px, none over the bound" % cell["probed"]
    a, b, m = cell["highTriple"]
    return ("%d/%d probed px over +%.1f%%, %d under; worst at %s: "
            "solo %.5f + %.5f = %.5f against merged %.5f"
            % (cell["over"], cell["probed"], 100.0 * UNEQ_TOL, cell["under"],
               cell["highAt"], a, b, a + b, m))


def unequalDensityChecks(settings):
    """f3e/f3f (the over-read) and f3g/f3h (the controls that attribute it)."""
    checks = []
    gate = "|merged - (soloA+soloB)| <= %.1f%% of it, per px" % (100.0 * UNEQ_TOL)

    # --- f3e: the single cell the milestone records, at the shipping K.
    base = unequalDensityCell(settings, 14.0, 0.99, 0.10,
                              (8.0, 12.0), (9.0, 13.0))
    checks.append(boolCheck(
        "f", "f3e unequal density, overlapping spans (alpha 0.99 beside 0.10, "
             "size 14)",
        max(base["high"], -base["low"]) <= UNEQ_TOL,
        _uneqMeasured(base), gate,
        population=_uneqPopulation(base),
        note="M1.P3.T22's gate, and M1.P3.T23's target — a PLAIN FAIL, not an "
             "xfail: alpha is INVENTED here, which the coverage-deficit spec "
             "forbids ('the saturation rule never scales alpha up to hide "
             "this'). Two 40x40 cards 8 px apart, spans [8,12] and [9,13], "
             "range-anchored so solo and merged share one bucket set and one "
             "kernel LUT (see the header above). Gated on |deviation| in BOTH "
             "directions: a conservative rule that occludes every residual by "
             "the densest tile turns this into a -52.3% UNDER-read (measured) "
             "and still fails here. READ THE TWO ARMS SEPARATELY: the HIGH arm "
             "is M1.P3.T23's target (invented alpha); the LOW arm is a "
             "different, permitted-direction term that f3f's 'spans disjoint' "
             "cell isolates at +0.000% high / -3.278% low, so closing the "
             "over-read alone leaves this row RED. M1.P3.T23 MEASURED FIVE "
             "CANDIDATE RULES AND NONE BEATS THE TRADE: about half of the high "
             "arm is f3f's `overlap 100%` cell, where both parents' parts pool "
             "into ONE bucket entry and ONE tile and no rule reading THESE "
             "FOUR PLANES can reach them (bit-identical under a fifth plane, "
             "because every bucket there carries either new or co-located area "
             "and never both, so that plane's split is degenerate). A second-"
             "moment plane DOES reach it (review, POD only) -- see the header "
             "above and the milestone Decisions"))

    # --- f3f: the sweep.  A gate that reads ONE number cannot show whether a
    # later change moved the defect or moved the rig, so every axis the defect
    # is known to depend on is swept: density ratio, depth overlap, size and K.
    # The whole f3e-f3i family costs ~12s of scene (f)'s ~47s; --full-sweep
    # widens each axis to the grid M1.P3.T22 measured, which costs ~26s more.
    ratioCells = [
        # ADDED AT THIS TASK'S REVIEW, and it is the cell that says the defect
        # is not confined to unequal density: two IDENTICAL alpha-0.99 fog
        # cards whose spans are staggered by one unit — as ordinary as comp
        # content gets — read +8.373% high on 641 of 1076 px, in the forbidden
        # direction.  It is measured here rather than only described in f3h's
        # note because it is the SAME composite-side term as the rest of this
        # sweep (mutation-measured: tHeadIn=1 takes it to +0.0001%), not
        # f3c/f3d's accumulation-time term, which no composite rule can move.
        ("ratio 1.00 (EQUAL density, STAGGERED spans)", 14.0, None,
         0.99, 0.99, (8.0, 12.0), (9.0, 13.0)),
        ("ratio 0.99/0.50", 14.0, None, 0.99, 0.50, (8.0, 12.0), (9.0, 13.0)),
        ("ratio 0.50/0.10", 14.0, None, 0.50, 0.10, (8.0, 12.0), (9.0, 13.0)),
        ("ratio 0.10/0.99 (thin card IN FRONT)", 14.0, None, 0.10, 0.99,
         (8.0, 12.0), (9.0, 13.0)),
    ]
    overlapCells = [
        ("overlap 100% (coincident spans)", 14.0, None, 0.99, 0.10,
         (8.0, 12.0), (8.0, 12.0)),
        ("overlap 25%", 14.0, None, 0.99, 0.10, (8.0, 12.0), (11.0, 15.0)),
        ("overlap 0% (spans touch)", 14.0, None, 0.99, 0.10,
         (8.0, 12.0), (12.0, 16.0)),
        # PROMOTED OUT OF --full-sweep AT THIS TASK'S REVIEW.  It is the one
        # cell that separates f3e's two arms: no bucket pools these two
        # parents, so the over-read term is exactly absent (+0.000% high) and
        # what is left is the deficit term alone (-3.278% low).  M1.P3.T23
        # needs it on EVERY run, not behind an option, or it cannot tell
        # "the target is closed" from "f3e is still red".
        ("overlap: spans disjoint (isolates the DEFICIT term)", 14.0, None,
         0.99, 0.10, (8.0, 10.0), (12.0, 14.0)),
    ]
    sizeCells = [
        ("size 6", 6.0, None, 0.99, 0.10, (8.0, 12.0), (9.0, 13.0)),
        ("size 20", 20.0, None, 0.99, 0.10, (8.0, 12.0), (9.0, 13.0)),
    ]
    kCells = [
        ("K 4", 14.0, 4, 0.99, 0.10, (8.0, 12.0), (9.0, 13.0)),
        ("K 64", 14.0, 64, 0.99, 0.10, (8.0, 12.0), (9.0, 13.0)),
    ]
    if getattr(settings, "fullSweep", False):
        ratioCells += [
            ("ratio 0.99/0.70", 14.0, None, 0.99, 0.70,
             (8.0, 12.0), (9.0, 13.0)),
            ("ratio 0.99/0.30", 14.0, None, 0.99, 0.30,
             (8.0, 12.0), (9.0, 13.0)),
            ("ratio 0.99/0.03", 14.0, None, 0.99, 0.03,
             (8.0, 12.0), (9.0, 13.0)),
            ("ratio 0.90/0.10", 14.0, None, 0.90, 0.10,
             (8.0, 12.0), (9.0, 13.0)),
            ("ratio 0.30/0.05", 14.0, None, 0.30, 0.05,
             (8.0, 12.0), (9.0, 13.0)),
            ("ratio 0.03/0.99", 14.0, None, 0.03, 0.99,
             (8.0, 12.0), (9.0, 13.0)),
        ]
        overlapCells += [
            ("overlap 50%", 14.0, None, 0.99, 0.10,
             (8.0, 12.0), (10.0, 14.0)),
            ("overlap: thin card inside the span", 14.0, None, 0.99, 0.10,
             (8.0, 12.0), (10.0, 10.05)),
            ("overlap: spans disjoint, further apart", 14.0, None, 0.99, 0.10,
             (8.0, 9.0), (14.0, 15.0)),
        ]
        sizeCells += [
            ("size 3", 3.0, None, 0.99, 0.10, (8.0, 12.0), (9.0, 13.0)),
            ("size 10", 10.0, None, 0.99, 0.10, (8.0, 12.0), (9.0, 13.0)),
        ]
        kCells += [
            # NOT "K 2": DepthBuckets clamps to kMinBuckets = 4, so a K=2 cell
            # is a bit-identical duplicate of the K 4 row and reporting it as a
            # K=2 datapoint would invent an axis point the node cannot reach.
            # The K sweep this family measures is 4 -> 128, not 2 -> 128.
            ("K 8", 14.0, 8, 0.99, 0.10, (8.0, 12.0), (9.0, 13.0)),
            ("K 32", 14.0, 32, 0.99, 0.10, (8.0, 12.0), (9.0, 13.0)),
            ("K 128", 14.0, 128, 0.99, 0.10, (8.0, 12.0), (9.0, 13.0)),
        ]

    worstCell = ("(base, f3e)", base)
    badCells = 0 if max(base["high"], -base["low"]) <= UNEQ_TOL else 1
    totalOver = base["over"]
    totalProbed = base["probed"]
    details = ["base %s" % _uneqMeasured(base)]
    for label, size, k, aA, aB, zA, zB in (ratioCells + overlapCells
                                           + sizeCells + kCells):
        cellSettings = settings if k is None else settings.derive(k=k)
        cell = unequalDensityCell(cellSettings, size, aA, aB, zA, zB)
        worst = max(cell["high"], -cell["low"])
        if worst > UNEQ_TOL:
            badCells += 1
        if worst > max(worstCell[1]["high"], -worstCell[1]["low"]):
            worstCell = (label, cell)
        totalOver += cell["over"]
        totalProbed += cell["probed"]
        details.append("%s %s (%d/%d px)"
                       % (label, _uneqMeasured(cell), cell["over"],
                          cell["probed"]))

    cellCount = 1 + len(ratioCells) + len(overlapCells) + len(sizeCells) \
        + len(kCells)
    checks.append(boolCheck(
        "f", "f3f unequal density, swept over ratio x overlap x size x K",
        badCells == 0,
        "worst cell %s [%s]" % (_uneqMeasured(worstCell[1]), worstCell[0]),
        gate + ", every cell",
        population="%d/%d cells over the bound; %d/%d probed px over +%.1f%%"
                   % (badCells, cellCount, totalOver, totalProbed,
                      100.0 * UNEQ_TOL),
        note="the point of a sweep rather than one pinned number is that it "
             "TRACKS the defect as M1.P3.T23 moves it — the four axes are the "
             "ones it is known to depend on. Cells: " + "; ".join(details)))

    # --- f3g/f3h: the two controls that attribute f3e/f3f.  Neither of them
    # may be quietly relaxed: each states an arrangement the composite is
    # EXACT on, and they are what proves the rig (the range anchor, the
    # additive oracle) is sound rather than the measurement being of the
    # harness itself.  WHAT THEY DO NOT SAY (corrected at this task's review):
    # they do NOT establish that the over-read needs UNEQUAL density.  f3h
    # pins COINCIDENT spans; stagger the same two equal-density cards by one
    # depth unit and the over-read is back at +8.373% (f3f's `ratio 1.00`
    # cell).  What the pair does establish is that it needs a MULTI-PART
    # parent whose residual chain meets another parent's deposits — f3g,
    # single-bucket, is exact at a 10x density ratio.
    thin = unequalDensityCell(settings, 14.0, 0.99, 0.10,
                              (10.0, 10.05), (10.0, 10.05))
    checks.append(boolCheck(
        "f", "f3g control: unequal density, single-bucket cards (no residual "
             "chain)",
        max(thin["high"], -thin["low"]) <= UNEQ_TOL,
        _uneqMeasured(thin), gate,
        population=_uneqPopulation(thin),
        note="unequal density ALONE is not the trigger: two thin cards at one "
             "depth are one head deposit each, and the composite is exact on "
             "them. So f3e/f3f measure the pooling of a multi-part parent's "
             "residual chain with another parent's deposits, not the alpha "
             "ratio as such"))

    equal = unequalDensityCell(settings, 14.0, 0.99, 0.99,
                               (8.0, 12.0), (8.0, 12.0))
    checks.append(boolCheck(
        "f", "f3h control: EQUAL density, overlapping spans (f3c/f3d's own "
             "case)",
        max(equal["high"], -equal["low"]) <= UNEQ_TOL,
        _uneqMeasured(equal), gate,
        population=_uneqPopulation(equal),
        note="the arrangement f3c/f3d pin, measured through THIS oracle: "
             "overlapping multi-part spans alone are not the trigger either. "
             "NOTE WHAT IT DOES NOT SAY: the spans here COINCIDE, and equal "
             "density is exact only then — the same two cards at alpha 0.99 "
             "with STAGGERED spans [8,12]/[9,13] read +8.373% high on 641 of "
             "1076 px (alpha 0.50 twins read +2.931%), in the forbidden "
             "direction. MECHANISM CORRECTED AT THIS TASK'S REVIEW: M1.P3.T22 "
             "attributed that to f3c/f3d's accumulation-time pooling and "
             "deferred it on that basis, but it is NOT that term — it is the "
             "same composite-side tile allocation as f3e's over-read. "
             "Mutation-measured: tHeadIn=1 takes the staggered cell to "
             "+0.0001% (f3c/f3d do NOT go to zero under it, they go to "
             "+33.333%), and the pooled-tClaimed and whole-mosaic rules move "
             "it to +7.058% and +4.731%. So it is reachable by a composite "
             "rule, it is ordinary content, and it belongs in M1.P3.T23's "
             "scope; it is now measured every run as f3f's 'ratio 1.00' cell"))

    # --- f3i: the oracle's own premise, and the family's non-degeneracy
    # guard.  Without it, a composite that simply never occludes a co-located
    # deposit satisfies f3e/f3f/f3g/f3h outright (mutation-measured: all four
    # PASS, f3e at +0.000%) — additivity is trivially true when nothing
    # occludes anything.  This row reads 0.6709 against 0.50 under that same
    # mutation, so the family as a whole cannot be satisfied that way.
    worstSolo = 0.0
    soloRows = []
    for solo in soloCardInterior(settings):
        worstSolo = max(worstSolo, solo["worst"])
        soloRows.append("alpha %.2f -> %.7f (min %.7f max %.7f, %d px)"
                        % (solo["alpha"], solo["mean"], solo["min"],
                           solo["max"], solo["count"]))
    checks.append(boolCheck(
        "f", "f3i a SOLO card's own interior alpha is the alpha it was built "
             "with (f3e's premise)",
        worstSolo <= 1.0e-05,
        "worst |measured - alpha| %.3e" % worstSolo,
        "<= 1.0e-05, at every probed alpha",
        population="; ".join(soloRows),
        note="one card, blur fully inside it, so the destination pixel is "
             "covered exactly once: a volumetric parent's head deposit plus "
             "its whole residual chain must read the parent's alpha. f3e/f3f "
             "compare two renders against each other and are blind to a rule "
             "that is wrong in BOTH; this one is absolute"))
    return checks


# --- scenes (g) and (l): a receding ground plane ------------------------------
#
# z(y) = C / (yHorizon - y) is the depth of a real ground plane under a level
# camera, and it has a property no ad-hoc ramp has: it makes the CoC ramp
# exactly LINEAR in y.
#
#   radius(y) = size * |1 - S/z(y)| = (size*S/C) * |y - yFocus|,
#   yFocus = yHorizon - C/S
#
# so the "steepness" M1.P3.T16 specifies scene (g) in — CoC-radius pixels per
# scanline — is a single constant `size*S/C` that the scene sets directly, the
# radius at every row is known analytically (which is what the non-vacuity
# probes check against), and the radius field has ZERO curvature, so the
# scatter's own varying-radius energy term cannot be mistaken for a bucket
# artefact.
#
# The horizon sits above the frame so z stays finite on every row, and the focal
# plane crosses the middle scanline so the plane genuinely recedes THROUGH
# focus, as the scene list requires.

GROUND_Y_HORIZON = 300.0
GROUND_Y_FOCUS = 128.0
GROUND_FOCUS = 10.0
GROUND_C = GROUND_FOCUS * (GROUND_Y_HORIZON - GROUND_Y_FOCUS)
GROUND_Z_EXPR = "%.6f/(%.1f-y)" % (GROUND_C, GROUND_Y_HORIZON)
GROUND_COLOR = (0.40, 0.55, 0.70, 1.0)      # premultiplied: colour <= alpha


def groundSlope(size):
    """CoC-radius pixels per scanline, for a Manual-mode ``size``."""
    return size * GROUND_FOCUS / GROUND_C


def groundRadius(size, y):
    return groundSlope(size) * abs(y - GROUND_Y_FOCUS)


# The global kernel-radius grid, mirrored from src/DeepCDefocusKernel.h
# (M1.P3.T19).  Used only to WRITE THE NOTES on scene (l)'s checks — nothing is
# gated on it — so a mirror is honest here in a way it would not be in a gate.
KERNEL_COARSE_FROM_PX = 16.0
KERNEL_FINE_LAST_INDEX = 993
KERNEL_FINE_SCALE = 512.0
KERNEL_FINE_ORIGIN = 1025


def kernelGridRadius(index):
    if index <= 0:
        return 0.0
    if index <= KERNEL_FINE_LAST_INDEX:
        return KERNEL_FINE_SCALE / (KERNEL_FINE_ORIGIN - index)
    return KERNEL_COARSE_FROM_PX + (index - KERNEL_FINE_LAST_INDEX) * 0.5


def kernelGridIndex(radius):
    if not radius > 0.25:
        return 0
    if not radius > 0.5:
        return 1
    if radius >= KERNEL_COARSE_FROM_PX:
        return KERNEL_FINE_LAST_INDEX + int(
            round((radius - KERNEL_COARSE_FROM_PX) * 2.0))
    u = KERNEL_FINE_SCALE / radius
    k = max(32, min(1023, int(math.floor(u))))
    thresh = 2.0 * k * (k + 1) / (2.0 * k + 1.0)
    return KERNEL_FINE_ORIGIN - (k if u <= thresh else k + 1)


def _rowsFromBinEdge(size, y):
    """Signed distance, in scanlines, from row ``y`` to the nearest
    ``DiscKernelLUT`` bin edge.

    The grid is no longer uniform (M1.P3.T19): below 16px the nodes are
    ``512/n``, so the edges sit at the harmonic midpoints between neighbouring
    nodes rather than at ``n*0.5 + 0.25``.  Scene (l) claims its affected rows
    sit on those edges; this measures the claim instead of asserting it, so the
    note cannot go stale if the artefact ever moves.  Positive means the row's
    radius is past the edge (already in the wider bin).
    """
    slope = groundSlope(size)
    if not slope:
        return 0.0
    radius = groundRadius(size, y)
    index = kernelGridIndex(radius)
    node = kernelGridRadius(index)
    below = 0.5 * (node + kernelGridRadius(index - 1)) if index > 0 else node
    above = 0.5 * (node + kernelGridRadius(index + 1))
    edge = below if abs(radius - below) <= abs(radius - above) else above
    return (radius - edge) / slope


def groundPlane(color=GROUND_COLOR):
    return depthRampLayer(constant2d(color), GROUND_Z_EXPR)


def groundPlaneRow(y, color=GROUND_COLOR):
    """The same plane masked down to ONE scanline.

    ``depthRampLayer()`` drops zero-alpha pixels, so every other row emits no
    deep sample at all and the render is a single row's bokeh — whose extent is
    a direct measurement of ``radius(y)``.  That is the non-vacuity guard
    scenes (g) and (l) need: it proves the per-pixel depth ramp actually reached
    the node and that the CoC really spans the range the bucket arithmetic
    below is claimed over, rather than the whole scene quietly rendering sharp.
    """
    return depthRampLayer(rectangle2d((0, y, FORMAT_W, y + 1), color),
                          GROUND_Z_EXPR)


def _extent(image, channel, fixed, box, axis, threshold=1.0e-6):
    x0, y0, x1, y1 = box
    if axis == "x":
        hits = [x for x in range(x0, x1)
                if image.at(channel, x, fixed) > threshold]
    else:
        hits = [y for y in range(y0, y1)
                if image.at(channel, fixed, y) > threshold]
    return (hits[0], hits[-1]) if hits else None


def extentX(image, channel, y, box, threshold=1.0e-6):
    return _extent(image, channel, y, box, "x", threshold)


def extentY(image, channel, x, box, threshold=1.0e-6):
    return _extent(image, channel, x, box, "y", threshold)


def _spanWidth(span):
    return (span[1] - span[0] + 1) if span else 0


# --- scene (g) ---------------------------------------------------------------

def sceneG(settings):
    """Banding: a ground plane receding continuously through the focal plane.

    THE RAMP IS DELIBERATELY STEEP (0.5 CoC-radius pixels per scanline, 64 px at
    the frame edges).  The bucket deficit scales with how many buckets one
    destination pixel's CoC neighbourhood straddles, not with K alone, so a
    shallow ramp understates exactly the effect M1.P3.T17 has to judge.  At this
    slope a pixel at radius 32 gathers over 64 scanlines, i.e. 32 CoC-pixels of
    spread, against an 8-px bucket step at K=16 — four buckets, sixteen at K=64.

    The input is an OPAQUE constant-colour plane, so the correct output is a
    perfectly flat field at alpha 1: every deviation reported here is the
    node's, and there is no reference render to argue with.  Two mechanisms
    produce deviations and this scene separates them:

      * BUCKETING — fragments whose disc weights sum to 1 at a destination pixel
        but land in different buckets.  It is K-dependent by construction, so
        `max |profile(K) - profile(K=64)|` isolates it (g2).
      * THE NEAR-FOCUS ROWS — at this scene's 0.5 CoC-px-per-scanline slope the
        first six rows either side of focus step through radius 0, 0.5, 1.0,
        1.5 ... one scanline at a time, i.e. the CoC field itself changes by a
        whole 0.5 px per row.  A flat opaque surface loses
        (S_r(0) - S_r'(0))/2 there no matter how fine the kernel grid is —
        refining the grid cannot make adjacent rows share a kernel when the
        content puts them 0.5 px apart in radius.  It is K-invariant and
        content-driven (measured: without the exclusion g2 partition reads an
        identical 7.407e-02 at BOTH K=8 and K=16, and g3 partition reads the
        same 7.407e-02 at K=64), and scene (l) measures the same transition
        properly, on a 40x shallower slope.  Those rows are therefore excluded
        from every reading here.

    ON THAT EXCLUSION (re-measured at M1.P3.T19, which removed the reason the
    exclusion was originally given — kernel-bin quantisation; and AGAIN at
    M1.P3.T17 on the post-T19 plugin, both framings, since the bake-off's brief
    required the unexcluded reading).  Without it: g1 9.916e-03 / 5.084e-03 /
    1.899e-03 at K=8/16/64 (with: 1.048e-02 / 4.446e-03 / 2.799e-07); the
    deleted candidate 2.563e-03 / 1.946e-02 / 7.050e-02 (with: 2.823e-03 /
    2.153e-02 / 7.789e-02); g2 7.407e-02 at BOTH K=8 and K=16 (with: 4.909e-02
    / 4.657e-02); g3 3.191e-02 / 2.883e-02 / 7.407e-02 (with: 3.191e-02 /
    2.883e-02 / 3.576e-07).  Nothing crosses an XFAIL hard bound either way,
    the verdict at T17 was the same both ways, but the focus rows contribute
    one identical 7.407e-02 figure to g2 at every K and to g3 at K=64 — i.e.
    they swamp exactly the two metrics built to ISOLATE the bucketing.  The
    exclusion stays; only its justification changed.

    g4 (added at M1.P3.T17) runs the same ramp at alpha < 1, where the fragment
    split is not a no-op and the saturation clamp does not hide the positive
    half of the error.  It WAS the largest residual the bucket-composite
    bake-off found on the composite it kept; M1.P3.T20 fixed the mechanism
    behind it (0.1607 -> 0.0543, K-divergence gone) and re-pinned it, and
    M1.P3.T21 -- fixing the upward error T20 traded for that -- took it to
    0.0325 and re-pinned it again.  The T20 fix is why g1/g2/g3 are no longer
    XFAILs: see the note above g1.
    """
    checks = []
    size = 86.0
    slope = groundSlope(size)                       # 0.5 px radius per scanline
    edgeRadius = slope * GROUND_Y_FOCUS             # 64 px at rows 0 and 255

    # Interior: a destination row is only fully fed when no MISSING source row
    # (outside the frame) could have reached it.  A source at row y' has radius
    # slope*|y'-128| and reaches row y'+radius, so the deepest row that a
    # missing y'<0 source could have touched is exactly slope*128 = edgeRadius.
    # Same in x, where a missing column carries the same worst-case radius.
    bound = int(math.ceil(edgeRadius)) + 2
    # ...minus the small-CoC band, which belongs to scene (l).
    band = int(math.ceil(2.5 / slope)) + 1
    lowBox = (bound, bound, FORMAT_W - bound, int(GROUND_Y_FOCUS) - band)
    highBox = (bound, int(GROUND_Y_FOCUS) + band, FORMAT_W - bound,
               FORMAT_H - bound)
    rowCount = (lowBox[3] - lowBox[1]) + (highBox[3] - highBox[1])
    colCount = lowBox[2] - lowBox[0]
    lowRows = lowBox[3] - lowBox[1]

    def profileRow(index):
        """Concatenated-profile index -> the scanline it came from."""
        return (lowBox[1] + index if index < lowRows
                else highBox[1] + index - lowRows)

    # --- g0: the ramp reached the node, and the CoC is the analytic one.
    # THREE rows at THREE different radii, not two rows equidistant from focus:
    # rows `bound` and `FORMAT_H - bound` carry the same radius, so a mutant
    # that pinned the whole plane to one of those depths would satisfy both and
    # the "ramp" would be unproven. The spread of radii here is the ramp.
    for probeY in (bound, 160, FORMAT_H - bound):
        resetScript()
        expected = groundRadius(size, probeY)
        pad = int(math.ceil(expected)) + 8
        probeBox = (-pad, probeY - pad, FORMAT_W + pad, probeY + pad)
        image = render(settings,
                       makeDefocus(settings, groundPlaneRow(probeY), size=size,
                                   focusDistance=GROUND_FOCUS,
                                   cocMode="manual"),
                       "g_probe_y%d" % probeY, box=probeBox)
        span = extentY(image, "A", FORMAT_W // 2, probeBox)
        measured = (_spanWidth(span) - 1) / 2.0
        checks.append(boolCheck(
            "g", "g0 CoC at row %d matches the analytic ramp" % probeY,
            span is not None and abs(measured - expected) <= 1.5,
            "%.2f px" % measured, "%.2f +/- 1.5 px" % expected,
            note="single-scanline probe; slope %.4f px/row, %.1f px at the "
                 "frame edge" % (slope, edgeRadius)))

    # --- g1: the K sweep on the shipped bucket composite.
    #
    # This used to be a K x combine table, rendered under both candidates in one
    # pass so M1.P3.T17 could inherit it.  T17 ran, decided from these pixels,
    # and deleted the loser; the readings it decided on are in the milestone
    # Decisions (the deleted candidate went -0.28% / -2.15% / -7.79% at
    # K=8/16/64 here, DIVERGING in K, against the surviving rule's -1.05% /
    # -0.44% / -2.8e-05%, and was over 1/255 on 112 of 112 interior rows at
    # both K=8 and K=16 against 51 and 23).  What is left is the surviving
    # rule's own residual, which is still a documented one.
    #
    # THE XFAILs ON g1/g2/g3 ARE RETIRED (M1.P3.T20).  They existed for the
    # bucket composite's deficit on this ramp, and that deficit is GONE: the
    # opaque plane's readings drop by five to six decades once a co-located rear
    # deposit is attenuated by its own head's transmittance instead of by the
    # pooled mean over the whole claimed area.  Before -> after, same scene, same
    # plugin otherwise:
    #
    #   g1  K=8  1.048e-02 -> 1.703e-08     g2  K=8  4.909e-02 -> 1.848e-06
    #       K=16 4.446e-03 -> 5.801e-08         K=16 4.657e-02 -> 7.153e-07
    #       K=64 2.799e-07 -> 2.182e-08     g3  K=8  3.191e-02 -> 1.907e-06
    #                                           K=16 2.883e-02 -> 2.980e-07
    #                                           K=64 3.576e-07 -> 1.192e-07
    #
    # WHY AN OPAQUE PLANE SHOWED THE ALPHA<1 DEFECT AT ALL, since the fragment
    # split is a no-op at alpha 1: it is the SATURATION that makes the residual
    # path live here.  Where a destination pixel's new area and co-located area
    # sum past 1 the bucket's alpha clamps to 1, so `local = aCov/cov` comes out
    # at 1/(C_k + D_k) < 1 and the rest of the alpha becomes a residual — which
    # the pooled `tClaimed` then over-occluded exactly as it did at alpha < 1.
    # (The K=64 columns barely move because at K=64 few fragments share a bucket,
    # which is the same convergence g4 now shows.)
    #
    # They are plain checks again on purpose: an `expectedFailure` that no longer
    # describes a residual is an unbounded licence to fail, and would let a later
    # regression land anywhere below the old hard bound as an XFAIL.  Scene (g)'s
    # own stated criterion — "no visible seams at bucket boundaries at K=16" —
    # is now met outright, at every K, by four decades.
    profiles = {}
    seams = {}
    for k in (8, 16, 64):
        cell = settings.derive(k=k)
        resetScript()
        image = render(cell,
                       makeDefocus(cell, groundPlane(), size=size,
                                   focusDistance=GROUND_FOCUS,
                                   cocMode="manual"),
                       "g_ramp_k%d" % k)
        profile = (rowMeans(image, "A", lowBox)
                   + rowMeans(image, "A", highBox))
        redProfile = (rowMeans(image, "R", lowBox)
                      + rowMeans(image, "R", highBox))
        profiles[k] = profile
        mean = sum(profile) / len(profile)
        worstStep, medianStep, stepAt = stepProfile(profile)
        seams[k] = (worstStep, medianStep, stepAt)
        # The colour must track alpha exactly: this input is a single
        # premultiplied colour, so R == 0.40*A everywhere or the composite
        # has desynced the premultiplied pair (a defect shape this milestone
        # has now hit three times).
        worstRatio = max(abs(redProfile[i] - GROUND_COLOR[0] * profile[i])
                         for i in range(len(profile)))
        checks.append(tolCheck(
            "g", "g1 K=%-2d interior flat-field alpha |a-1|" % k,
            abs(mean - 1.0), 1.0e-03,
            population="%d rows x %d px, |y-128| >= %d"
                       % (rowCount, colCount, band),
            note="mean %.6f (%+.3f%%) min %.6f (y=%d) max %.6f; worst "
                 "row step %.3e at y=%d (median %.3e); colour:alpha "
                 "residual %.2e"
                 % (mean, (mean - 1.0) * 100.0, min(profile),
                    profileRow(profile.index(min(profile))), max(profile),
                    worstStep, profileRow(stepAt), medianStep, worstRatio)))

    # --- g2: the part of the banding that is ATTRIBUTABLE TO BUCKETING.
    # A seam the eye can see is a step of about 1/255 in the 8-bit result, so
    # that is the gate; K=64 is the reference because it is the finest bucketing
    # the scene renders, and the difference cancels every K-invariant term
    # (kernel-bin quantisation, the varying-radius scatter residual).
    reference = profiles[64]
    for k in (8, 16):
        deltas = [abs(a - b) for a, b in zip(profiles[k], reference)]
        worst = max(deltas)
        overCount = sum(1 for d in deltas if d > 1.0 / 255.0)
        checks.append(tolCheck(
            "g", "g2 K=%-2d bucket-attributable banding vs K=64" % k,
            worst, 1.0 / 255.0,
            population="%d/%d interior rows over 1/255" % (overCount, rowCount),
            note="max |rowMean(K=%d) - rowMean(K=64)|; worst row y=%d "
                 "(radius %.2f px)"
                 % (k, profileRow(deltas.index(worst)),
                    groundRadius(size, profileRow(deltas.index(worst))))))

    # --- g3: the scene's OWN stated criterion — "no visible seams at bucket
    # boundaries at K=16; compare K=8 vs K=64".  g1 (a mean) and g2 (a
    # difference against K=64) both average or cancel a seam away; a seam is a
    # LOCALISED step in the row profile, and until this check existed the
    # milestone's banding scene never gated the thing it is named after.  The
    # median step is carried alongside so the reading can be read as "a spike
    # against a flat neighbourhood" rather than "the profile is that noisy".
    for k in (8, 16, 64):
        worstStep, medianStep, stepAt = seams[k]
        row = profileRow(stepAt)
        checks.append(tolCheck(
            "g", "g3 K=%-2d worst seam (row-to-row step in the profile)" % k,
            worstStep, 1.0 / 255.0,
            population="%d interior rows; median step %.3e"
                       % (rowCount, medianStep),
            note="worst step at y=%d (radius %.2f px), %.0fx the median; "
                 "1/255 is the step an 8-bit view resolves"
                 % (row, groundRadius(size, row),
                    (worstStep / medianStep) if medianStep > 0.0 else 0.0)))

    # --- g4: THE SAME RAMP AT alpha < 1.  Found at M1.P3.T17, RULED ON at
    # M1.P3.T20, and RE-PINNED here in the same change as the fix.
    #
    # WHAT IT MEASURED, AND WHAT IS LEFT.  An ordinary semi-transparent surface
    # receding through focus used to lose 12.5 / 16.1 / 9.2% of its alpha at
    # alpha 0.99 / 0.90 / 0.50 at K=16, and to DIVERGE in K (-5.0 / -9.2 /
    # -13.7 / -19.6 / -26.6% at K=8/16/32/64/128 at alpha 0.5).  M1.P3.T20
    # rebuilt the composite's transmittance bookkeeping — a co-located deposit
    # is now attenuated by ITS OWN head's sub-area transmittance rather than by
    # the pooled mean over everything claimed, and a residual's occlusion is
    # subtracted from that mean in proportion to the area it covers instead of
    # multiplying the whole of it.  The same rig now reads:
    #
    #   alpha  K=2      K=4      K=8      K=16     K=32     K=64     K=128
    #   1.00   -0.000   -0.000   -0.000   -0.000   -0.000   -0.000   -0.000
    #   0.99   -4.363   -4.363   -5.552   -4.886   -3.923   -3.539   -5.036
    #   0.90   -3.290   -3.290   -5.583   -5.426   -4.784   -4.665   -6.055
    #   0.50   +1.373   +1.373   -0.090   -0.128   +0.104   -0.022   -1.007
    #
    # i.e. the K-DIVERGENCE IS GONE (alpha 0.50 went from -26.6% at K=128 to
    # -1.0%, and its worst reading anywhere is now +1.4%), and what is left is
    # bounded and roughly K-flat.
    #
    # RE-PINNED AT M1.P3.T21, 0.0543 -> 0.0325.  T20 bought the ramp with an
    # upward error on staggered multi-part parents (up to +21.1% on hand-built
    # planes, the honest-alpha contract's forbidden direction) because it
    # carried ONE head tile and had to discard one whenever a bucket both
    # claimed area and continued a chain.  T21 carries a STACK of them
    # (kCompositeHeadTiles = 16) and allocates a residual across it by area from
    # the newest end.  That closes the upward error to +0.000% on the same
    # sweep AND takes another 40% off this reading, because a dense ramp's
    # buckets each leave two tiles as well.
    #
    # THE REMAINING TERM IS A DIFFERENT MECHANISM, and the control that says so
    # is in the unit suite ("the depth-ramp mosaic ...", M1.P3.T20/T21).  On
    # hand-built planes with no kernel, no holdout, no flatten and no
    # quantisation, a ramp whose fragments each occupy their OWN bucket pair is
    # EXACT at every bucket count, N, alpha AND split fraction -- where the
    # pre-T20 composite read -4.11 / -17.44 / -21.63% at N=2/16/64 for alpha
    # 0.90.  Pack the pairs ADJACENTLY, as a real ramp does, and a deficit comes
    # back at exactly this scale, because bucket k then pools fragment k's head
    # and fragment k-1's rear, whose per-unit opacities are
    # partitionAlpha(alpha, 1-frac) and partitionAlpha(alpha, frac) --
    # different for every split fraction but 0.5 -- and the composite's
    # C_k : D_k area split cannot separate them.  That is harness f3c/f3d's
    # mechanism: information lost at ACCUMULATION, not at composition, and no
    # per-bucket composite rule can undo it.
    #
    # SINCE M1.P3.T21 THAT TERM HAS A CLOSED FORM, which is what makes it a
    # residual rather than a mystery: the composite hands both sub-layers the
    # mean m = (a0 + a1)/2, so each fragment's tile reads 1 - (1-m)^2 instead of
    # 1 - (1-a0)(1-a1) = alpha, and AM-GM makes that a DEFICIT for every
    # fraction but 0.5.  At alpha 0.90 it is -4.107% at frac 0.25 AND at 0.75,
    # at every N -- pinned in the unit suite against the closed form, not
    # against a re-run.  (T20 read -2.42/-3.69/-4.00% and -5.79/-4.53/-4.21%
    # there: N-dependent and asymmetric in the fraction, because the single tile
    # mixed this term with the mosaic error T21 removed.)  This scene's ramp
    # gives every scanline its own split fraction, so it cannot reach zero.
    #
    # CORRECTED AT M1.P3.T20's REVIEW.  T20 first attributed this to fragments
    # carrying DIFFERENT split fractions from one another.  It is not that: the
    # unit suite's own cells hold the fraction CONSTANT across every fragment
    # and still read a deficit.  Only frac == 0.5 is exact.
    #
    # PINNED AS A BAND, not as a ceiling, and K IS FIXED AT 16 here rather than
    # taken from --k: a one-sided bound would be satisfied by every improvement
    # AND by a --k that moved it, and neither is what this check is for.
    # MUTATION-TESTED at M1.P3.T20 by rendering this very scene through seven
    # separate mutations of compositePixelCoveragePartition(), and RE-TESTED at
    # M1.P3.T21 against the mutations the head-tile stack makes available:
    #
    #   full pre-T20 revert                                   0.1607
    #   always carry the chain's tile forward                 0.1315
    #   scale the co-located residual's alpha by 0.75         0.1196
    #   merge the two candidate tiles by area                 0.0913
    #   claim area = cov instead of fit                       0.0703
    #   multiplicative `tClaimed *= (1 - resLocal)` again     0.0598
    #   the excess share does not attenuate tHead             0.0491
    #   ---- added at M1.P3.T21 ----
    #   M1.P3.T20's single head tile (i.e. this fix reverted) 0.0543
    #   allocate the residual OLDEST tile first (FIFO)        0.0718  (+8 FAILs)
    #   the residual never overflows onto this bucket's claim 0.0325  NOT CAUGHT
    #   the partly-covered frontier tile does not split       0.0325  NOT CAUGHT
    #
    # The band stays 0.004: the nearest mutation is now 0.0218 away (a straight
    # revert of this fix) and the nearest of the old seven 0.0166, so every one
    # of the eleven lands outside it.  The last two are recorded as NOT CAUGHT
    # here rather than left unstated: this check does not bound them.
    #
    # CORRECTED AT M1.P3.T21's REVIEW, which re-ran both against the unit suite.
    # "the residual never overflows onto this bucket's claim" is caught, by 9
    # assertions, as recorded.  "the partly-covered frontier tile does not
    # split" is caught by NOTHING: forcing the whole-tile branch (which is what
    # the stack-overflow path itself does) leaves all 175 429 unit assertions
    # passing AND leaves this check at 0.0325.  The branch is not dead -- it
    # fires 14 600 times in the unit suite -- so the suite exercises it without
    # constraining it.  What IS caught, by 3 assertions on T9's pinned
    # behind-focus residue, is the OTHER formulation the source names: scaling
    # `resLocal` by the covered share instead of splitting (61.00% -> 70.53%).
    # The unguarded direction is DOWNWARD (the whole-tile branch over-occludes
    # the uncovered ring), which the honest-alpha contract permits, so this is a
    # gap in coverage rather than an unbounded hazard -- but it is a gap, and
    # "caught by 1 assertion" was not measured.
    #
    # An eighth mutation from T20 -- registering the `excess` share as its own
    # head tile, which the isolated arithmetic argues FOR -- reads 0.0825 here
    # and takes g1/g2/g3 back to 1.082e-02 / 4.954e-02 / 3.200e-02, i.e. the
    # fit-only rule is confirmed by pixels and not only by argument.
    #
    # ("claim area = cov" was recorded by T20 as caught HERE and nowhere else.
    # Re-run at the review it is also caught by g1 (9.980e-03 against a
    # 1.0e-03 gate), g2 (4.906e-02) and g3 (3.191e-02) -- four rendered checks,
    # not one.  It does still survive the whole unit suite.)
    #
    # THIS CHECK CANNOT PASS, BY CONSTRUCTION: it is a band around a residual,
    # so any change to the reading -- an improvement included -- turns it FAIL.
    # Whoever moves it next must RE-PIN it in the same commit, exactly as
    # M1.P3.T20 and M1.P3.T21 did.
    G4_PIN, G4_BAND = 0.0325, 0.004
    g4K = 16
    g4Alpha = 0.90
    g4Colour = tuple(c * g4Alpha for c in GROUND_COLOR[:3]) + (g4Alpha,)
    g4Cell = settings.derive(k=g4K)
    resetScript()
    fogImage = render(g4Cell,
                      makeDefocus(g4Cell, groundPlane(color=g4Colour),
                                  size=size, focusDistance=GROUND_FOCUS,
                                  cocMode="manual"),
                      "g_ramp_alpha%g" % g4Alpha)
    fogProfile = (rowMeans(fogImage, "A", lowBox)
                  + rowMeans(fogImage, "A", highBox))
    fogMean = sum(fogProfile) / len(fogProfile)
    fogDeficit = 1.0 - fogMean / g4Alpha
    fogBad = sum(1 for v in fogProfile
                 if abs(v - g4Alpha) > g4Alpha / 255.0)
    checks.append(boolCheck(
        "g", "g4 K=%d interior flat-field alpha at alpha %.2f (the alpha<1 "
             "residual)" % (g4K, g4Alpha),
        False,
        "%.4f low (%.6f vs %.2f)" % (fogDeficit, fogMean, g4Alpha),
        "0.0000 (xfail %.4f +/- %.4f)" % (G4_PIN, G4_BAND),
        population="%d/%d interior rows over A/255" % (fogBad, rowCount),
        note="min %.6f (y=%d) max %.6f; the opaque twin (g1, same rig, K=%d) "
             "reads %+.3f%% — what is left is ONE BUCKET POOLING A HEAD AND A "
             "REAR AT UNEQUAL PER-UNIT OPACITY (f3c/f3d's mechanism; any split "
             "fraction but 0.5), not the tClaimed mosaic term M1.P3.T20 removed"
             % (min(fogProfile),
                profileRow(fogProfile.index(min(fogProfile))),
                max(fogProfile), g4K,
                (sum(profiles[g4K]) / len(profiles[g4K]) - 1.0) * 100.0),
        expectedFailure=True, hardTol=G4_BAND,
        hardValue=abs(fogDeficit - G4_PIN)))

    # --- g5: THE LOW-ALPHA ARM OF THE SAME RAMP (M1.P3.T24).  g4 pins one
    # alpha (0.90, a DEFICIT); below alpha ~0.5 the SAME rig reads HIGH --
    # invented alpha, the forbidden direction, at the shipping default K --
    # and until this check existed nothing bounded it.  ALL FIGURES HERE ARE
    # g4-RIG FIGURES (this scene's ground ramp), NOT the f3e/f3f two-card
    # family: M1.P3.T23 read this arm against that other rig, got +0.370%,
    # and reported the record transposed -- the seventh wrong-rig instance in
    # this milestone.  The full sweep on this rig (interior-mean excursion
    # mean/alpha - 1, this plugin, M1.P3.T24):
    #
    #   alpha    K=4      K=8     K=16     K=32     K=64    K=128
    #   0.10   +6.526   +6.156   +5.926   +5.803   +5.664   +5.388
    #   0.30   +5.166   +4.073   +3.399   +3.039   +2.648   +1.901
    #   0.50   +3.757   +1.987   +0.897   +0.319   -0.287   -1.409
    #   0.90   +0.819   -1.698   -3.253   -4.084   -4.935   -6.576
    #   0.99   -0.943   -1.855   -2.536   -2.970   -3.714   -5.497
    #
    # THE MECHANISM IS THE SCATTER'S, NOT THE COMPOSITE'S (M1.P3.T24), which
    # corrects two recorded attributions at once: it is NOT f3c/f3d's
    # accumulation-time pooling "seen from its positive side" (M1.P3.T21) and
    # NOT the covariance term the second-moment plane reaches (M1.P3.T23's
    # review direction).  Each fragment's disc is normalised to sum 1 over its
    # OWN kernel, and on a steep CoC gradient the adjoint sum at a DESTINATION
    # pixel is not 1: nearer-focus rows arrive with denser discs than
    # farther rows lose, so the deposited weight itself sums to ~1.07 here
    # (this scene's deliberately steep 0.5 CoC-px/scanline slope).  The
    # alpha-0.01 control below reads that number directly -- rendered
    # +7.124 / +7.063 / +7.037% at K=4/16/64, K-FLAT because the scatter is K-
    # independent -- and the alpha dependence is the composite honestly
    # `over`-attenuating the spurious excess by tClaimed ~ (1 - alpha): fully
    # visible as alpha -> 0, absorbed entirely by the area clamp at alpha = 1
    # (which is why g1 reads 1e-08 on the SAME weights).  The unit suite pins
    # the decomposition on a faithful 1-column model of this rig ("the g4
    # rig's low-alpha over-read...", M1.P3.T24): at alpha 0.001 the composite
    # reproduces sum(w) - 1 to 0.1 points at every K, and RENORMALISING the
    # weights per pixel flips every low-alpha cell to a small DEFICIT
    # (alpha 0.10: -0.25/-0.70/-0.87% at K=4/16/64), i.e. the composite's own
    # low-alpha term is in the PERMITTED direction and the whole forbidden-
    # direction excursion enters at scatter time.
    #
    # WHY IT IS ACCEPTED RATHER THAN FIXED, said with numbers:
    #   * no composite rule, at ANY plane count, can reach it: the same
    #     bucket planes arise from ~190 INDEPENDENT small cards at the same
    #     depths (each deposit shape is a legitimate lone fragment), and for
    #     those the over-composited truth is HIGHER than alpha, not equal to
    #     it -- one plane set, two truths, so no function of the planes (the
    #     second-moment plane included, computed from the same deposits) can
    #     be right on both.  "One receding surface" vs "many overlapping
    #     surfaces" is parent identity, which the planes do not carry.
    #   * the scatter-side fix is per-destination-pixel weight
    #     renormalisation, and that breaks content the node is exact on: two
    #     full-coverage fog layers at alpha 0.5 read exactly 0.75 today
    #     (f3h, and the `two 50% fog layers` identity) and 0.50 renormalised,
    #     because sum(w) = 2 there is GENUINE overlap.  Distinguishing the
    #     two needs the same parent identity.  A direction-gated version is
    #     the design's own deferred v2 `alpha-renormalize` toggle.
    #   * it is content-driven, scaling with the CoC gradient: RENDERED at
    #     alpha 0.01 / K=16 this ramp reads +7.06% at slope 0.5, +1.54% at
    #     slope 0.25 (size 43) and +0.34% at slope 0.125 (size 21.5),
    #     tracking the real-LUT adjoint sums +7.19/+1.63/+0.39% computed
    #     from DiscKernelLUT directly (T24 review; the chord model's -0.6%
    #     at slope 0.125 had the WRONG SIGN -- the real kernel's shallow-
    #     slope residue stays slightly high), so ordinary content (scene
    #     (l)'s 40x shallower ramp) sits orders of magnitude inside these
    #     pins.
    #
    # PINNED AS BANDS, K FIXED per cell, like g4.  Two-sided ON PURPOSE, so
    # the gate cannot be satisfied by trading the over-read for a deficit of
    # the same size.  MUTATION-TESTED IN BOTH DIRECTIONS at M1.P3.T24 by
    # rendering THIS SCENE through four perturbed builds (values are this
    # check's `excursion` at the mutated build; * = inside the band, i.e.
    # that cell alone does not catch that mutation):
    #
    #                                     a=.10/K16  a=.30/K16  a=.10/K4  a=.01/K16
    #   pin                                +0.0593    +0.0340    +0.0653   +0.0706
    #   UP:   excess term loses its
    #         tClaimed attenuation         +0.0620*   +0.0425    +0.0669*  +0.0709*
    #   UP:   tHeadIn = 1 (residual
    #         unattenuated)                +0.0877    +0.1256    +0.0871   +0.0734*
    #   DOWN: residual alpha scaled 0.75   -0.0705    -0.0854    -0.0511   -0.0634
    #   SCATTER: alpha deposits x 1.02     +0.0798    +0.0527    +0.0861   +0.0920
    #
    #   Every mutation flips at least one g5 cell (and g4) to FAIL; the DOWN
    #   row is the sign-trade case and all four cells catch it.  The
    #   alpha-0.01 control barely moves under COMPOSITE mutations BY DESIGN
    #   (tClaimed ~ 0.99 there, so the composite has almost nothing left to
    #   get wrong) -- its job is the SCATTER row, where it moves 5.4x its
    #   band, i.e. it bounds exactly the uniform-scaling class the
    #   f3e/f3f ratio oracle is structurally invariant to.
    #
    # THIS CHECK CANNOT PASS, BY CONSTRUCTION (a band around a residual);
    # whoever moves any of these readings must RE-PIN in the same commit,
    # exactly as g4's history demands.
    G5_BAND = 0.004
    g5Cells = [
        # (alpha, K, pin, role)
        (0.10, 16, +0.0593, "the shipping default K"),
        (0.30, 16, +0.0340, "the alpha the record left ungated"),
        (0.10, 4,  +0.0653, "the sweep's worst corner"),
        (0.01, 16, +0.0706, "mechanism control: reads sum(w)-1, the "
                            "scatter's own over-delivery, K-flat"),
    ]
    for g5Alpha, g5K, g5Pin, g5Role in g5Cells:
        g5Cell = settings.derive(k=g5K)
        g5Colour = tuple(c * g5Alpha for c in GROUND_COLOR[:3]) + (g5Alpha,)
        resetScript()
        g5Image = render(g5Cell,
                         makeDefocus(g5Cell, groundPlane(color=g5Colour),
                                     size=size, focusDistance=GROUND_FOCUS,
                                     cocMode="manual"),
                         "g_ramp_alpha%g_k%d" % (g5Alpha, g5K))
        g5Profile = (rowMeans(g5Image, "A", lowBox)
                     + rowMeans(g5Image, "A", highBox))
        g5Mean = sum(g5Profile) / len(g5Profile)
        g5Excursion = g5Mean / g5Alpha - 1.0
        g5High = sum(1 for v in g5Profile
                     if v - g5Alpha > g5Alpha / 255.0)
        checks.append(boolCheck(
            "g", "g5 K=%-2d alpha %.2f low-alpha over-read (the alpha arm; "
                 "g4 rig)" % (g5K, g5Alpha),
            False,
            "%+.4f (%.6f vs %.2f)" % (g5Excursion, g5Mean, g5Alpha),
            "0.0000 (xfail %+.4f +/- %.4f)" % (g5Pin, G5_BAND),
            population="%d/%d interior rows over A/255 HIGH" % (g5High,
                                                                rowCount),
            note="%s; min %.6f max %.6f (y=%d); POSITIVE is the forbidden "
                 "direction -- invented alpha, the scatter's weight "
                 "over-delivery on this rig's steep CoC slope, accepted and "
                 "bounded at M1.P3.T24 (see the block above)"
                 % (g5Role, min(g5Profile), max(g5Profile),
                    profileRow(g5Profile.index(max(g5Profile)))),
            expectedFailure=True, hardTol=G5_BAND,
            hardValue=abs(g5Excursion - g5Pin)))
    return checks


# --- scene (h) ---------------------------------------------------------------

def sceneH(settings):
    """Overlap normalization: two opaque same-depth cards overlapping in screen
    space => no alpha > 1, no brightened seam in the overlap band.

    Both cards carry the SAME premultiplied colour, so the union is one uniform
    rectangle and any double-count shows up directly as colour above the card's
    own value.  h0 pins that the two cards really do overlap (otherwise "the
    union reads 0.6" is free).

    **Read h1/h2 with h0b and h3.**  The scene list says *same-depth* cards, and
    at exactly the same depth the mandated per-pixel tidy pre-pass
    over-composites the two coincident samples before the scatter ever sees
    them: measured at M1.P3.T16's review, the two-card render is BIT-IDENTICAL
    to a single card covering the union (h0b), so h1/h2 cannot distinguish the
    overlap case from the trivial one-card case and cannot exercise the bucket
    saturation rule at all.  The mechanism the design names — "where same-bucket
    surfaces overlap in screen space, bucket alpha can exceed 1" — needs two
    surfaces the tidy pass and M1.P3.T13's collision merge both leave alone,
    i.e. two DIFFERENT kernels.  That is h3, and it is where this scene's
    discriminating power actually lives.  Note also that the saturation rule
    rescales colour AND alpha by 1/alpha together, so it hides a uniform
    double-count in colour just as it does in alpha; the seam step, which is
    local, is what survives it.
    """
    checks = []
    cardA = (40, 64, 168, 192)
    cardB = (88, 64, 216, 192)
    overlap = (cardB[0], cardA[1], cardA[2], cardA[3])
    union = (cardA[0], cardA[1], cardB[2], cardB[3])
    cardZ = 6.0
    colour = (0.60, 0.60, 0.60, 1.0)
    size, focus = 18.0, 10.0
    radius = size * abs(1.0 - focus / cardZ)            # 12 px
    pad = int(math.ceil(radius)) + 6
    paddedBox = (-pad, -pad, FORMAT_W + pad, FORMAT_H + pad)

    def layer(box, z=cardZ):
        return pointLayer(rectangle2d(box, colour), z,
                          keepZeroAlpha=False, premult=True)

    # --- h0: the cards really overlap. Each one ALONE must cover the whole
    # overlap band; if either did not, every reading below would be satisfied by
    # a single card and the scene would test nothing.
    for name, box in (("A", cardA), ("B", cardB)):
        resetScript()
        image = render(settings,
                       makeDefocus(settings, layer(box), size=0.0,
                                   cocMode="manual"),
                       "h_card%s" % name, box=formatBox())
        stats = channelStats(image, "A", insetBox(overlap, 2))
        checks.append(boolCheck(
            "h", "h0 card %s alone covers the overlap band" % name,
            stats.minimum > 0.999, "min alpha %.6f" % stats.minimum, "> 0.999",
            population="overlap band %dx%d px"
                       % (overlap[2] - overlap[0], overlap[3] - overlap[1])))

    # --- h0b: ...but "they overlap" is not "the scatter sees two surfaces".
    # At one depth the tidy pre-pass over-composites the coincident samples per
    # pixel before the scatter, so the whole two-card source collapses to one
    # opaque layer. Reported, not gated: it is a statement about what h1/h2 can
    # and cannot prove, and h3 is the check that does not have this problem.
    resetScript()
    twoCards = render(settings,
                      makeDefocus(settings, deepMerge([layer(cardA),
                                                       layer(cardB)]),
                                  size=size, focusDistance=focus,
                                  cocMode="manual"),
                      "h_twocards", box=paddedBox)
    resetScript()
    oneCard = render(settings,
                     makeDefocus(settings, layer(union), size=size,
                                 focusDistance=focus, cocMode="manual"),
                     "h_onecard", box=paddedBox)
    collapse = compareImages(twoCards, oneCard, box=paddedBox)
    checks.append(boolCheck(
        "h", "h0b same-depth pair vs ONE card covering the union", True,
        "%.4e" % collapse.maxAbs, "reported",
        population=collapse.population(),
        note="0 means the tidy pre-pass collapsed the pair before the scatter, "
             "so h1/h2 below are the one-card case and cannot exercise the "
             "bucket saturation rule; h3 is the check that can"))

    for name, blur in (("h1 sharp", 0.0), ("h2 defocused", size)):
        resetScript()
        source = deepMerge([layer(cardA), layer(cardB)])
        image = render(settings,
                       makeDefocus(settings, source, size=blur,
                                   focusDistance=focus, cocMode="manual"),
                       "h_overlap_%g" % blur, box=paddedBox)

        alpha = channelStats(image, "A", paddedBox)
        checks.append(tolCheck(
            "h", "%s: max alpha over the whole bbox, excess over 1" % name,
            max(0.0, alpha.maximum - 1.0), 1.0e-06,
            population="%d px" % alpha.count,
            note="max %.7f at (%d,%d)" % (alpha.maximum, alpha.maxAt[0],
                                          alpha.maxAt[1])))

        inset = int(math.ceil(radius)) + 3 if blur else 3
        interior = insetBox(union, inset)
        red = channelStats(image, "R", interior)
        checks.append(tolCheck(
            "h", "%s: brightened seam in the union interior (max R - 0.60)"
                 % name,
            max(0.0, red.maximum - colour[0]), 1.0e-06,
            population="union interior %dx%d px"
                       % (interior[2] - interior[0], interior[3] - interior[1]),
            note="min %.7f max %.7f mean %.7f; a double-counted overlap reads "
                 "high here even though alpha saturates at 1"
                 % (red.minimum, red.maximum, red.mean)))

        # A seam is a LOCAL step, so scan across the overlap boundary rather
        # than trusting the interior extremes alone.
        scanY = (union[1] + union[3]) // 2
        profile = [image.at("R", x, scanY)
                   for x in range(interior[0], interior[2])]
        worstStep, medianStep, stepAt = stepProfile(profile)
        checks.append(tolCheck(
            "h", "%s: worst step across the overlap boundary (scanline)" % name,
            worstStep, 1.0e-04,
            population="x=%d..%d at y=%d; overlap edges at x=%d and x=%d"
                       % (interior[0], interior[2], scanY, overlap[0],
                          overlap[2]),
            note="median step %.3e, worst at x=%d"
                 % (medianStep, interior[0] + stepAt)))

    # --- h3: the overlap the design's saturation rule is actually about — two
    # opaque surfaces overlapping in screen space that the scatter must see as
    # TWO deposits.  Different depths give them different CoC radii, hence
    # different DiscKernelLUT bins, which is the one predicate that defeats both
    # the tidy pre-pass (different z) and M1.P3.T13's collision merge
    # (`sameScatterKernel`), so both discs really are laid down and the bucket's
    # area planes really do have to resolve the double claim.
    farZ = 20.0
    radiusNear = size * abs(1.0 - focus / cardZ)        # 12 px
    radiusFar = size * abs(1.0 - focus / farZ)          # 9 px
    checks.append(boolCheck(
        "h", "h3a the two cards land in DIFFERENT kernel bins",
        round(radiusNear / 0.5) != round(radiusFar / 0.5),
        "r %.2f (bin %.1f) vs r %.2f (bin %.1f)"
        % (radiusNear, round(radiusNear / 0.5) * 0.5,
           radiusFar, round(radiusFar / 0.5) * 0.5), "different bins",
        note="same bin satisfies `sameScatterKernel`, which makes the pair "
             "eligible for M1.P3.T13's always-on collision merge; h3b-h3d "
             "would then be measuring a single deposit again"))

    resetScript()
    mixed = deepMerge([layer(cardA, cardZ), layer(cardB, farZ)])
    image = render(settings,
                   makeDefocus(settings, mixed, size=size,
                               focusDistance=focus, cocMode="manual"),
                   "h_diffkernel", box=paddedBox)
    alpha = channelStats(image, "A", paddedBox)
    checks.append(tolCheck(
        "h", "h3b different-kernel overlap: max alpha excess over 1",
        max(0.0, alpha.maximum - 1.0), 1.0e-06,
        population="%d px" % alpha.count,
        note="max %.7f at (%d,%d); both cards opaque and the same colour, so "
             "the correct union is a flat 0.60/1.0"
             % (alpha.maximum, alpha.maxAt[0], alpha.maxAt[1])))

    interior = insetBox(union, int(math.ceil(radiusNear)) + 3)
    red = channelStats(image, "R", interior)
    checks.append(tolCheck(
        "h", "h3c different-kernel overlap: brightened seam (max R - 0.60)",
        max(0.0, red.maximum - colour[0]), 2.0e-06,
        population="union interior %dx%d px"
                   % (interior[2] - interior[0], interior[3] - interior[1]),
        note="min %.7f max %.7f mean %.7f" % (red.minimum, red.maximum,
                                              red.mean)))
    scanY = (union[1] + union[3]) // 2
    profile = [image.at("R", x, scanY)
               for x in range(interior[0], interior[2])]
    worstStep, medianStep, stepAt = stepProfile(profile)
    checks.append(tolCheck(
        "h", "h3d different-kernel overlap: worst step across the boundary",
        worstStep, 1.0e-04,
        population="x=%d..%d at y=%d; overlap edges at x=%d and x=%d"
                   % (interior[0], interior[2], scanY, overlap[0], overlap[2]),
        note="median step %.3e, worst at x=%d"
             % (medianStep, interior[0] + stepAt)))
    return checks


# --- scene (i) ---------------------------------------------------------------

def sceneI(settings):
    """Sparse reveal / coverage deficit.

    NOT built with DeepMerge, deliberately: a DeepMerge of two full cards
    carries the occluded card's samples everywhere, so the defocused near card
    always has something behind it and the deficit can never appear.  Here a
    single ``DeepFromImage`` over a per-pixel depth gives EXACTLY ONE sample per
    pixel — near inside the silhouette, far outside, nothing hidden — which is
    what a renderer that terminates opaque hits actually emits.

    The expected result is the DOCUMENTED honest alpha dip, so the scene passes
    by matching the spec: the dip must be present (i2), confined to about one
    CoC radius inside the silhouette (i3), free of fabricated colour (i4), and
    absent from the DeepMerge-built twin (i5) — that last one is what proves the
    dip comes from the missing hidden samples rather than from the node.
    """
    checks = []
    silhouette = (64, 64, 192, 192)
    nearZ, farZ = 4.0, 20.0
    focus, size = 20.0, 6.0
    radius = size * abs(1.0 - focus / nearZ)            # 24 px
    nearColour, farColour = 0.80, 0.60
    inside = ("(x>=%d && x<%d && y>=%d && y<%d)"
              % (silhouette[0], silhouette[2], silhouette[1], silhouette[3]))

    def sparseSource():
        """One sample per pixel; no occluded data anywhere."""
        image = nuke.nodes.Expression(inputs=[constant2d((0.0, 0.0, 0.0, 0.0))])
        image["expr0"].setValue("%s ? %g : 0.0" % (inside, nearColour))
        image["expr1"].setValue("0.0")
        image["expr2"].setValue("%s ? 0.0 : %g" % (inside, farColour))
        image["expr3"].setValue("1.0")
        return depthRampLayer(image, "%s ? %g : %g" % (inside, nearZ, farZ))

    def mergedSource():
        """The same visible content WITH the occluded far card kept."""
        near = pointLayer(rectangle2d(silhouette, (nearColour, 0.0, 0.0, 1.0)),
                          nearZ, keepZeroAlpha=False, premult=True)
        far = pointLayer(constant2d((0.0, 0.0, farColour, 1.0)), farZ,
                         keepZeroAlpha=False, premult=True)
        return deepMerge([near, far])

    # --- i1: the two sources are indistinguishable when nothing is defocused.
    # They differ ONLY in hidden samples, so any difference below is that.
    resetScript()
    sparseSharp = render(settings, deepToImage(sparseSource(), volumetric=True),
                         "i_sparse_flat", box=formatBox())
    resetScript()
    mergedSharp = render(settings, deepToImage(mergedSource(), volumetric=True),
                         "i_merged_flat", box=formatBox())
    flatDiff = compareImages(sparseSharp, mergedSharp, box=formatBox())
    checks.append(tolCheck(
        "i", "i1 sparse and DeepMerge sources flatten identically",
        flatDiff.maxAbs, 2.0e-07, population=flatDiff.population(),
        note="they differ only in the occluded samples DeepMerge keeps; "
             "worst " + flatDiff.where()))

    dips = {}
    for name, build in (("sparse", sparseSource), ("DeepMerge", mergedSource)):
        resetScript()
        pad = int(math.ceil(radius)) + 6
        box = (-pad, -pad, FORMAT_W + pad, FORMAT_H + pad)
        image = render(settings,
                       makeDefocus(settings, build(), size=size,
                                   focusDistance=focus, cocMode="manual"),
                       "i_%s" % name, box=box)
        deep = insetBox(silhouette, int(math.ceil(radius)) + 3)
        edgeBand = channelStats(image, "A", (silhouette[0], deep[1],
                                             deep[0], deep[3]))
        core = channelStats(image, "A", deep)
        scanY = (silhouette[1] + silhouette[3]) // 2
        profile = [image.at("A", x, scanY)
                   for x in range(silhouette[0], silhouette[2])]
        dipWidth = 0
        for value in profile:
            if value >= 0.999:
                break
            dipWidth += 1
        # Blue is measured over the EDGE BAND — the band the dip occupies —
        # not over the deep interior.  Measured at M1.P3.T16's review: with the
        # far card given a CoC so it really can scatter inward, the dip band
        # reads 2.86e-01 of invented blue while the deep interior still reads
        # exactly 0, i.e. the interior-scoped form of this check survives the
        # one mutation it exists to catch.
        blue = channelStats(image, "B", (silhouette[0], deep[1],
                                         deep[0], deep[3]))
        dips[name] = (edgeBand, core, dipWidth, blue, image)

    sparseEdge, sparseCore, sparseWidth, sparseBlue, _ = dips["sparse"]
    mergedEdge, mergedCore, mergedWidth, _, _ = dips["DeepMerge"]

    # --- i2: the honest dip is PRESENT.
    checks.append(boolCheck(
        "i", "i2 honest alpha dip inside the silhouette (specified behaviour)",
        sparseEdge.minimum < 0.99,
        "min alpha %.6f" % sparseEdge.minimum, "< 0.99 (dip present)",
        population="edge band %d px wide inside the silhouette"
                   % (int(math.ceil(radius)) + 3),
        note="the design's honest-alpha rule: the node must NOT scale alpha up "
             "to hide a coverage deficit"))

    # --- i3: and it is ~one CoC wide, not the whole card.
    checks.append(boolCheck(
        "i", "i3 dip width matches the CoC radius",
        abs(sparseWidth - radius) <= 3.0,
        "%d px" % sparseWidth, "%.1f +/- 3 px" % radius,
        population="scanline y=%d, from the silhouette edge inward"
                   % ((silhouette[1] + silhouette[3]) // 2),
        note="deep interior alpha min %.6f mean %.6f (must be back at 1)"
             % (sparseCore.minimum, sparseCore.mean)))
    # 1e-04, not 1e-06: the DiscKernelLUT's per-entry normalisation residual is
    # ~5e-08 (milestone Decisions) and a radius-24 disc interior sums ~pi*r^2 =
    # 1810 of them, so ~9e-05 is the floor here and a tighter gate would be
    # measuring the LUT, not the coverage deficit.  It still discriminates: the
    # dip itself is 0.49 deep, four decades above this.
    checks.append(tolCheck(
        "i", "i3b silhouette interior beyond the CoC is unaffected",
        abs(sparseCore.minimum - 1.0), 1.0e-04,
        population="%d px" % sparseCore.count,
        note="min %.7f max %.7f; floor is the LUT normalisation residual over "
             "~%d deposits" % (sparseCore.minimum, sparseCore.maximum,
                               int(math.pi * radius * radius))))

    # --- i4: no fabricated colour. The far card is exactly in focus, so it
    # never scatters; every blue pixel inside the silhouette would be invented.
    checks.append(tolCheck(
        "i", "i4 no fabricated colour behind the dip (max blue in the dip band)",
        sparseBlue.maximum, 1.0e-06,
        population="%d px in the dip band" % sparseBlue.count,
        note="the far card is in focus and cannot scatter inward, and the "
             "silhouette has no hidden samples, so every blue pixel here would "
             "be invented exactly where the alpha dipped"))

    # --- i5: THE DISCRIMINATOR. The DeepMerge twin, which carries the occluded
    # samples, must NOT dip — otherwise the dip is a node artefact, not the
    # specified coverage deficit.
    checks.append(boolCheck(
        "i", "i5 DeepMerge twin (hidden samples present) shows NO dip",
        mergedEdge.minimum > 0.999,
        "min alpha %.6f" % mergedEdge.minimum, "> 0.999",
        population="same edge band; sparse reads %.6f there"
                   % sparseEdge.minimum,
        note="proves the dip is the missing hidden samples, not the node"))

    # --- i6/i7: pre_merge, set EXPLICITLY both ways on this scene.
    deltas = {}
    for tolerance in (settings.mergeTolerance, 2.0):
        rendered = []
        for preMerge in (True, False):
            cell = settings.derive(preMerge=preMerge, mergeTolerance=tolerance)
            resetScript()
            rendered.append(render(
                cell, makeDefocus(cell, sparseSource(), size=size,
                                  focusDistance=focus, cocMode="manual"),
                "i_premerge_%s_%g" % (preMerge, tolerance), box=formatBox()))
        deltas[tolerance] = compareImages(rendered[0], rendered[1],
                                          box=formatBox())
    for tolerance in sorted(deltas):
        diff = deltas[tolerance]
        checks.append(boolCheck(
            "i", "i6 pre_merge on vs off at merge_tolerance %.2f" % tolerance,
            True, "%.4e" % diff.maxAbs, "reported",
            population=diff.population(),
            note="0 is the CORRECT answer on this scene: it has exactly one "
                 "sample per pixel, so the pre-merge has nothing to group. "
                 "i7 is the reachability proof"))

    # --- i7: pre_merge REACHABILITY.  M1.P3.T12's review could not move a
    # single pixel with this knob on any content it tried, so it is settled
    # here with content built against the documented predicate rather than by
    # trying more scenes.
    #
    # The pre-merge groups adjacent same-pixel fragments when they share a
    # containing bucket, a FragmentKind and a holdout bracket, and their radii
    # are within merge_tolerance; the group then rasterises ONE disc, at its
    # front member's radius.  So the merge is lossless exactly when the grouped
    # radii round to the same DiscKernelLUT bin, and lossy when they do not —
    # which the 0.25px DEFAULT tolerance permits, because the bins are 0.5px
    # wide and their edges sit at n*0.5 + 0.25, so a pair 0.2px apart can still
    # straddle one.  (M1.P3.T16's first pass asserted the opposite — "anything
    # grouped at 0.25px rasterises the same disc, so the default is exactly
    # lossless" — and gated it on a pair 1.47px apart, which the default
    # tolerance never groups at all: the check read 0 because nothing merged,
    # not because merging was free.  Measured here instead.)
    #
    # Both layers are full-frame, so every pixel carries the pair; a corner
    # element at z=3 widens the frame's measured CoC range so the pair still
    # shares one containing ΔCoC bucket.
    SIZE, FOCUS = 20.0, 10.0

    def zForRadius(radiusPx):
        """Behind focus: r = size*(1 - focus/z)."""
        return FOCUS / (1.0 - radiusPx / SIZE)

    reachBox = (32, 32, 224, 224)

    def reachability(radiusA, radiusB, tolerance):
        rendered = []
        for preMerge in (True, False):
            cell = settings.derive(preMerge=preMerge, mergeTolerance=tolerance)
            resetScript()
            source = deepMerge([
                pointLayer(constant2d((0.30, 0.30, 0.30, 0.60)),
                           zForRadius(radiusA), keepZeroAlpha=False,
                           premult=False),
                pointLayer(constant2d((0.30, 0.30, 0.30, 0.60)),
                           zForRadius(radiusB), keepZeroAlpha=False,
                           premult=False),
                pointLayer(rectangle2d((0, 0, 24, 24), (0.5, 0.5, 0.5, 1.0)),
                           3.0, keepZeroAlpha=False, premult=True)])
            rendered.append(render(
                cell, makeDefocus(cell, source, size=SIZE,
                                  focusDistance=FOCUS, cocMode="manual"),
                "i_reach_%s_%g_%g" % (preMerge, radiusB - radiusA, tolerance),
                box=reachBox))
        return compareImages(rendered[0], rendered[1], box=reachBox)

    # 1.2 and 1.4 CoC px: 0.20 apart, i.e. inside the SHIPPING DEFAULT
    # tolerance, but on opposite sides of the 1.25 bin edge (bins 1.0 and 1.5).
    atDefault = reachability(1.2, 1.4, settings.mergeTolerance)
    checks.append(boolCheck(
        "i", "i7 pre_merge reaches the render at the DEFAULT merge_tolerance",
        atDefault.maxAbs > 1.0e-02,
        "%.4e" % atDefault.maxAbs,
        "> 1e-02 at merge_tolerance %.2f" % settings.mergeTolerance,
        population=atDefault.population(),
        note="two full-frame same-pixel layers at CoC radius 1.2 and 1.4 px — "
             "0.20 px apart, so within tolerance, but straddling the 1.25 px "
             "kernel-bin edge, so the group rasterises a different disc than "
             "the pair would"))
    # The control: identical content, tolerance 0, so nothing may group. It is
    # what makes i7 a statement about the merge rather than about the geometry.
    noGrouping = reachability(1.2, 1.4, 0.0)
    checks.append(tolCheck(
        "i", "i7b control: same content at merge_tolerance 0 cannot move",
        noGrouping.maxAbs, 1.0e-07, population=noGrouping.population(),
        note="pre_merge on vs off with nothing eligible to group; a non-zero "
             "reading here would mean i7's delta is not the merge"))
    # And the lossless half of the predicate, stated where it is actually true:
    # a pair inside the SAME bin.
    #
    # M1.P3.T19 MOVED THIS PAIR.  It used to be 5.0 and 5.2 px, which shared
    # bin 5.0 on the old uniform 0.5px grid; on the refined grid the nodes are
    # ~0.05px apart there (5.019608 and 5.224490), so that pair straddles four
    # of them and reads 9.0e-02 — a real reading of a real loss, but no longer a
    # reading of the LOSSLESS case this check exists for. Moved to 17.0/17.2,
    # which sit above kKernelCoarseFromPx where the grid is still the uniform
    # 0.5px one, both inside the [16.75, 17.25] bin.
    sameBin = reachability(17.0, 17.2, settings.mergeTolerance)
    checks.append(tolCheck(
        "i", "i7c ...and is lossless when the grouped radii share a kernel bin",
        sameBin.maxAbs, 1.0e-07, population=sameBin.population(),
        note="CoC radius 17.0 and 17.2 px: 0.20 apart like i7, but both inside "
             "the [16.75, 17.25] bin, so the grouped disc IS the pair's disc. "
             "i7d is the non-vacuity guard"))
    # ...and the guard that keeps i7c from passing because nothing GROUPED.
    # A lossless merge is unobservable by construction, so eligibility has to
    # be shown on a pair that is identical in every respect the pre-merge
    # tests -- same radius scale, same 0.20px separation, same tolerance,
    # same content -- and differs only in straddling a bin edge.
    straddle = reachability(17.2, 17.4, settings.mergeTolerance)
    checks.append(boolCheck(
        "i", "i7d guard: the same pair 0.2px higher, straddling a bin edge, DOES move",
        straddle.maxAbs > 1.0e-02,
        "%.4e" % straddle.maxAbs, "> 1e-02",
        population=straddle.population(),
        note="17.2 and 17.4 px straddle the 17.25 px edge (bins 17.0 and "
             "17.5), so this content at this tolerance really is eligible to "
             "group -- which is what makes i7c's zero a statement about "
             "losslessness rather than about nothing having merged"))
    return checks


# --- scene (j) ---------------------------------------------------------------

def sceneJ(settings):
    """Anamorphic: a pixel-aspect-2 format => bokeh elliptical by exactly the
    aspect, and the output bbox padded correspondingly in Y.

    The source is a SINGLE pixel, so the rendered support IS the kernel and the
    two extents are the kernel's two radii — no card geometry to subtract.
    """
    checks = []
    aspect = 2.0
    size, focus, cardZ = 10.0, 30.0, 10.0
    radius = size * abs(1.0 - focus / cardZ)            # 20 px
    padX = int(math.ceil(settings.maxRadius + 0.5 * 1.0))   # edge_softness 1.0
    box = (-140, -140, FORMAT_W + 140, FORMAT_H + 140)

    measured = {}
    for pixelAspect in (1.0, aspect):
        resetScript(pixelAspect=pixelAspect)
        rect = rectangle2d((128, 128, 129, 129), (1.0, 1.0, 1.0, 1.0))
        card = pointLayer(rect, cardZ, keepZeroAlpha=False, premult=True)
        # The 2D SOURCE's own format, not the root's: the bug this guards
        # against left the root at aspect 2 while a square-pixel Constant fed
        # the graph, so reading nuke.root()["format"] would have reported 2.000
        # and passed. rect.input(0) is that Constant (harness.rectangle2d).
        sourceFormat = rect.input(0)["format"].value()
        node = makeDefocus(settings, card, size=size, focusDistance=focus,
                           cocMode="manual")
        bbox = node.bbox()
        image = render(settings, node, "j_pa%g" % pixelAspect, box=box)
        measured[pixelAspect] = dict(
            formatAspect=sourceFormat.pixelAspect(),
            formatName=sourceFormat.name(),
            rootAspect=nuke.root()["format"].value().pixelAspect(),
            rx=(_spanWidth(extentX(image, "A", 128, box)) - 1) / 2.0,
            ry=(_spanWidth(extentY(image, "A", 128, box)) - 1) / 2.0,
            padX=-int(bbox.x()), padY=-int(bbox.y()),
            width=int(bbox.w()), height=int(bbox.h()))

    square, anamorphic = measured[1.0], measured[aspect]

    # --- j0: non-vacuity, read off the 2D SOURCE rather than the root. The 2D
    # sources are built from the CURRENT format, so this pins that the aspect
    # actually reached the node rather than the root carrying it while a
    # square-pixel Constant fed the graph (which is what happened before
    # harness.currentFormat() existed: bbox pad stayed at the square value and
    # the bokeh came out round). Verified at M1.P3.T16's review: under that bug
    # the root reads 2.000 and the source 1.000, so only the source reading is
    # a guard.
    checks.append(boolCheck(
        "j", "j0 the SOURCE really is a pixel-aspect-%g format" % aspect,
        abs(anamorphic["formatAspect"] - aspect) < 1e-06
        and abs(square["formatAspect"] - 1.0) < 1e-06,
        "%.3f on %s (square run %.3f on %s)"
        % (anamorphic["formatAspect"], anamorphic["formatName"],
           square["formatAspect"], square["formatName"]),
        "%.1f / 1.0" % aspect,
        note="root format reads %.3f / %.3f on the two runs — equal to the "
             "source only because the fix is in place"
             % (anamorphic["rootAspect"], square["rootAspect"])))

    checks.append(boolCheck(
        "j", "j1 X radius is unchanged by the pixel aspect",
        abs(anamorphic["rx"] - square["rx"]) <= 1.0,
        "%.2f px (square %.2f px)" % (anamorphic["rx"], square["rx"]),
        "equal +/- 1 px",
        note="analytic CoC radius %.2f px" % radius))

    checks.append(boolCheck(
        "j", "j2 bokeh is elliptical by exactly the pixel aspect",
        abs(anamorphic["ry"] - aspect * anamorphic["rx"]) <= 1.5,
        "ry %.2f px, ry/rx %.3f" % (anamorphic["ry"],
                                    anamorphic["ry"] / anamorphic["rx"]),
        "ry = %.1f * rx +/- 1.5 px" % aspect,
        note="square run ry/rx %.3f; the 1px anti-aliased rim is itself "
             "stretched in Y, which is what the 1.5px allowance is"
             % (square["ry"] / square["rx"])))

    checks.append(boolCheck(
        "j", "j3 bbox pad: Y = ceil(X pad * aspect)",
        anamorphic["padX"] == padX
        and anamorphic["padY"] == int(math.ceil(padX * aspect)),
        "padX %d padY %d" % (anamorphic["padX"], anamorphic["padY"]),
        "%d / %d" % (padX, int(math.ceil(padX * aspect))),
        population="bbox %dx%d (square run padX %d padY %d)"
                   % (anamorphic["width"], anamorphic["height"],
                      square["padX"], square["padY"]),
        note="max_radius %d + half an edge_softness" % settings.maxRadius))
    return checks


# --- scene (k) ---------------------------------------------------------------

def sceneK(settings):
    """Proxy + ray-distance.

    k1/k2: proxy 0.5 must halve the radius AND the max_radius clamp, so the
    proxy render is the same image at half scale rather than twice as blurred.
    k3-k5: ``depth_is_ray_distance`` on a wide-FOV corner pixel must reproduce
    the ground-truth Z exactly, and must be a no-op at the optical centre.
    """
    checks = []

    # --- proxy. The card is 8 px wide at full size so both renders have a card
    # to subtract; Nuke scales the Rectangle's box knob into proxy itself.
    cardBox = (124, 124, 132, 132)
    cardWidth = cardBox[2] - cardBox[0]
    size, focus, cardZ = 10.0, 30.0, 10.0
    fullRadius = size * abs(1.0 - focus / cardZ)        # 20 px

    for label, maxRadius, expected in (("k1 unclamped", settings.maxRadius,
                                        fullRadius),
                                       ("k2 max_radius clamped", 8, 8.0)):
        cell = settings.derive(maxRadius=maxRadius)
        radii = {}
        windows = {}
        for proxyScale in (None, 0.5):
            resetScript(proxyScale=proxyScale)
            scale = proxyScale or 1.0
            card = pointLayer(rectangle2d(cardBox, (1.0, 1.0, 1.0, 1.0)), cardZ,
                              keepZeroAlpha=False, premult=True)
            node = makeDefocus(cell, card, size=size, focusDistance=focus,
                               cocMode="manual")
            box = (-80, -80, FORMAT_W + 80, FORMAT_H + 80)
            image = render(cell, node, "k_proxy%g" % scale, box=box)
            # The crop box is in full-size coordinates; the render itself lands
            # in proxy coordinates, so scan the data window we actually got.
            window = (image.x0, image.y0, image.x0 + image.width,
                      image.y0 + image.height)
            windows[scale] = window
            span = extentX(image, "A", int(128 * scale), window)
            radii[scale] = (_spanWidth(span) - cardWidth * scale) / 2.0
        # k0: proxy really engaged — otherwise both renders are the same image
        # and "the radius halved" is a statement about nothing.
        checks.append(boolCheck(
            "k", "%s: proxy 0.5 really halves the data window" % label,
            abs((windows[0.5][2] - windows[0.5][0])
                - 0.5 * (windows[1.0][2] - windows[1.0][0])) <= 1.0,
            "%d px wide (full %d px)"
            % (windows[0.5][2] - windows[0.5][0],
               windows[1.0][2] - windows[1.0][0]), "half, +/- 1 px"))
        checks.append(boolCheck(
            "k", "%s: radius halves under proxy 0.5" % label,
            abs(radii[0.5] - 0.5 * radii[1.0]) <= 0.75,
            "%.2f px proxy vs %.2f px full" % (radii[0.5], radii[1.0]),
            "half, +/- 0.75 px",
            note="analytic full-size radius %.2f px (max_radius %d)"
                 % (expected, maxRadius)))
        checks.append(boolCheck(
            "k", "%s: full-size radius matches the analytic value" % label,
            abs(radii[1.0] - expected) <= 1.0,
            "%.2f px" % radii[1.0], "%.2f +/- 1 px" % expected))

    # --- ray distance. A 20 mm lens on a 36 mm filmback is ~84 degrees across,
    # so the corner pixel's radial filmback offset is large and the ray-length
    # -> Z correction is a third of the depth: exactly the case the toggle is
    # for, and one where getting it wrong is unmissable.
    resetScript()
    focalMm, filmbackMm = 20.0, 36.0
    rayDistance = 8.0
    manualSize, manualFocus = 20.0, 10.0

    def zAt(px, py):
        mmPerPxX = filmbackMm / FORMAT_W
        mmPerPxY = mmPerPxX                     # pixel aspect 1 in this scene
        dx = (px + 0.5 - 0.5 * FORMAT_W) * mmPerPxX
        dy = (py + 0.5 - 0.5 * FORMAT_H) * mmPerPxY
        rMm = math.sqrt(dx * dx + dy * dy)
        return rayDistance * focalMm / math.sqrt(focalMm * focalMm + rMm * rMm)

    def rayRender(px, py, depth, rayToggle, tag):
        resetScript()
        pixel = pointLayer(rectangle2d((px, py, px + 1, py + 1),
                                       (1.0, 1.0, 1.0, 1.0)),
                           depth, keepZeroAlpha=False, premult=True)
        node = makeDefocus(settings, pixel, size=manualSize,
                           focusDistance=manualFocus, cocMode="manual",
                           focal_length=focalMm, filmback_width=filmbackMm,
                           depth_is_ray_distance=rayToggle)
        return render(settings, node, tag, box=(-80, -80, FORMAT_W + 80,
                                                FORMAT_H + 80))

    cornerX, cornerY = 8, 8
    cornerZ = zAt(cornerX, cornerY)
    corrected = rayRender(cornerX, cornerY, rayDistance, True, "k_ray_on")
    truth = rayRender(cornerX, cornerY, cornerZ, False, "k_ray_truth")
    uncorrected = rayRender(cornerX, cornerY, rayDistance, False, "k_ray_off")
    box = (-80, -80, FORMAT_W + 80, FORMAT_H + 80)

    truthDiff = compareImages(corrected, truth, box=box)
    naiveDiff = compareImages(uncorrected, truth, box=box)
    checks.append(tolCheck(
        "k", "k3 ray-distance corner matches ground-truth Z",
        truthDiff.maxAbs, 1.0e-05, population=truthDiff.population(),
        note="ray length %.3f at pixel (%d,%d) -> Z %.5f (%.1f%% correction); "
             "worst %s; discrimination floor measured at M1.P3.T16's review: a "
             "Z 0.25%% off the truth already reads 4.4e-04 here, so 0.0 is a "
             "real match and not a blind gate"
             % (rayDistance, cornerX, cornerY, cornerZ,
                100.0 * (1.0 - cornerZ / rayDistance), truthDiff.where())))
    checks.append(boolCheck(
        "k", "k4 ...and the correction is doing real work (toggle off differs)",
        naiveDiff.maxAbs > 1.0e-02, "%.4e" % naiveDiff.maxAbs, "> 1e-02",
        population=naiveDiff.population(),
        note="radius %.2f px corrected vs %.2f px uncorrected"
             % (manualSize * abs(1.0 - manualFocus / cornerZ),
                manualSize * abs(1.0 - manualFocus / rayDistance))))

    centreX, centreY = FORMAT_W // 2, FORMAT_H // 2
    centreOn = rayRender(centreX, centreY, rayDistance, True, "k_ray_centre_on")
    centreOff = rayRender(centreX, centreY, rayDistance, False,
                          "k_ray_centre_off")
    centreDiff = compareImages(centreOn, centreOff, box=box)
    checks.append(tolCheck(
        "k", "k5 ray-distance is a no-op at the optical centre",
        centreDiff.maxAbs, 1.0e-06, population=centreDiff.population(),
        note="r_mm ~ 0 there, so the factor is 1; this is what makes k3 a test "
             "of the RADIAL term and not of a constant scale"))
    return checks


# --- scene (l) ---------------------------------------------------------------

def sceneL(settings):
    """Small-CoC transition: a shallow ramp crossing 0-2.5 px CoC.

    Same ground plane as scene (g) at a 40x shallower slope (0.0195 CoC px per
    scanline), so the frame walks the sharp-path threshold at 0.5 px and every
    DiscKernelLUT bin boundary below 2.5 px one scanline at a time.  The input
    is an opaque constant-colour plane, so the correct output is a flat field
    and any structure at all is chatter.

    HISTORY.  Until M1.P3.T19 l1/l2/l5 FAILED, and the failure was the node's:
    ``DiscKernelLUT::radiusToIndex`` was ``lround(radius/0.5)``, so where the
    ramp crossed a bin edge the two sides rasterised different discs and the
    destination row on the crossing was short by exactly half the difference in
    the two kernels' CENTRE-ROW weight — 0.799119 / 0.905153 / 0.948266 /
    0.973739 at the 0.5->1.0 / 1.0->1.5 / 1.5->2.0 / 2.0->2.5 crossings,
    predicted from the LUT alone and matched here to six decimals.  A
    one-scanline 20% dark line across an opaque surface.  T19 replaced the
    uniform grid with one whose step is ``c*r^2``, which makes that deficit
    uniform at ~1.26e-03 across the whole radius range; l1 went 2.009e-01 ->
    2.176e-03 and l2 2.009e-01 -> 1.950e-03.

    WHAT IS LEFT, and it is three OTHER mechanisms, not the LUT.  Each figure
    below is separated by a kernel-only model (the same scatter with NO buckets
    at all, run outside Nuke straight off the LUT) and by the K sweep:
      * the disc family's own C1 kink at r = 0.5, where the first neighbour
        shell enters (with edgeSoftness 1.0 every disc of radius <= 0.5 IS the
        delta, and at 0.5+ all four neighbours arrive at once).  Worth
        2.08e-03 on the y ramp (row y=100, radius 0.547 px — which is what l1
        reads at K=8, matching the kernel-only model to six decimals) and
        3.67e-03 on the radial one at (97,32), radius 0.530 px, which is what
        l5 gates on.  Both inside the 1/255 gate but not by much.  The Design
        reference's "add a sharp<->defocused blend zone" is the remedy if it
        ever needs tightening; T19 measured it and left it.
      * the bucket composite.  l1's REPORTED worst at the shipping K=16 is
        2.176e-03 at y=217, radius 1.739 px — a row the kernel-only model puts
        at exactly 1.000000, and which K=8 also reads as 1.000000.  It is
        K-dependent, so it is the composite, not the kernel; l3 is where it is
        tracked, and M1.P3.T17 owns it.
      * the CoC field's own extremum — l6.
    """
    checks = []
    size = 3.36
    slope = groundSlope(size)                   # 0.0195 px radius per scanline
    edgeRadius = slope * GROUND_Y_FOCUS         # 2.50 px
    interior = insetBox(formatBox(), int(math.ceil(edgeRadius)) + 6)

    # --- l0: the ramp is really there and really is small.
    probeY = 8
    expected = groundRadius(size, probeY)
    resetScript()
    probeBox = (-16, probeY - 16, FORMAT_W + 16, probeY + 16)
    probe = render(settings,
                   makeDefocus(settings, groundPlaneRow(probeY), size=size,
                               focusDistance=GROUND_FOCUS, cocMode="manual"),
                   "l_probe", box=probeBox)
    span = extentY(probe, "A", FORMAT_W // 2, probeBox)
    measured = (_spanWidth(span) - 1) / 2.0
    checks.append(boolCheck(
        "l", "l0 CoC at the frame edge is the analytic small radius",
        span is not None and abs(measured - expected) <= 1.0,
        "%.2f px" % measured, "%.2f +/- 1 px" % expected,
        note="slope %.5f px/row; the frame spans 0 to %.2f px of CoC"
             % (slope, edgeRadius)))

    # --- l1/l2: the flat field, at three K values. A bucket artefact moves with
    # K; a sharp-path/LUT-step artefact does not, and separating them is the
    # whole point of running the sweep.
    profiles = {}
    for k in (8, settings.k, 64):
        cell = settings.derive(k=k)
        resetScript()
        image = render(cell,
                       makeDefocus(cell, groundPlane(), size=size,
                                   focusDistance=GROUND_FOCUS,
                                   cocMode="manual"),
                       "l_ramp_k%d" % k)
        profiles[k] = rowMeans(image, "A", interior)

    profile = profiles[settings.k]
    worstRow = interior[1] + profile.index(min(profile))
    worstStep, medianStep, stepAt = stepProfile(profile)
    bad = [interior[1] + i for i, v in enumerate(profile) if abs(v - 1.0) > 1.0 / 255.0]
    checks.append(tolCheck(
        "l", "l1 flat-field alpha over the 0-%.1fpx CoC ramp |a-1|" % edgeRadius,
        max(abs(v - 1.0) for v in profile), 1.0 / 255.0,
        population="%d/%d interior rows over 1/255" % (len(bad), len(profile)),
        note="worst row y=%d (radius %.3f px) reads %.6f; per affected row, "
             "y(radius, alpha, rows-from-the-nearest kernel-grid bin edge) — "
             "MEASURED, not asserted: %s"
             % (worstRow, groundRadius(size, worstRow), min(profile),
                " ".join("y%d(r%.2f,a%.4f,%+.1f)"
                         % (y, groundRadius(size, y),
                            profile[y - interior[1]],
                            _rowsFromBinEdge(size, y))
                         for y in bad))))
    checks.append(tolCheck(
        "l", "l2 chatter: worst scanline-to-scanline step", worstStep,
        1.0 / 255.0,
        population="median step %.3e over %d rows" % (medianStep, len(profile)),
        note="worst step at y=%d" % (interior[1] + stepAt)))

    # --- l3: the K sweep.  While the LUT trough dominated, every K read the
    # same worst row to 1e-06 and that INVARIANCE was the evidence that the
    # trough was not the bucketing.  With the trough gone the sweep measures
    # something else and must be re-read.
    #
    # WHAT IT NOW MEASURES, per row, against a kernel-only model (the same
    # scatter with no buckets at all):
    #   * K=8's worst row IS the kernel-only floor — 0.997916 at y=100
    #     (radius 0.547 px), which the model reproduces to six decimals;
    #   * K=16's worst is 0.997824 at y=217 (radius 1.739 px), where the model
    #     and K=8 both read exactly 1.000000 — so it is already the composite,
    #     not the kernel;
    #   * K=64's worst is 0.992598 at y=103 (radius 0.488 px, the sharp<->disc
    #     threshold), where K=8 and K=16 both read 1.000000.
    # Two independent facts put all of that on the BUCKET COMPOSITE, not on
    # DiscKernelLUT: it appears only as K rises, and it was candidate-dependent
    # (the same K=64 row read 0.999666 under the composite M1.P3.T17 deleted
    # against 0.992598 under the one it kept, and the whole check read 2.186e-04
    # instead of 5.226e-03).  The old 0.5px grid hid it by rasterising every
    # radius in [0.25, 0.75] as the same delta, so no neighbour spilled across a
    # bucket boundary at all.
    #
    # THIS WHOLE SCENE FAVOURED THE DELETED CANDIDATE (l1 4.296e-04 against
    # 2.176e-03, l2 4.175e-04 / 1.950e-03, l3 2.186e-04 / 5.226e-03, l5
    # 2.935e-03 / 3.667e-03) and M1.P3.T17 kept the other one anyway — see the
    # Decisions entry for why, and for the alpha-0.5 and alpha-0.25 re-renders
    # that refuted the first explanation offered for it.  Below ~2.5 px BOTH of
    # the deleted rule's failure modes are quenched at once; its advantage
    # shrinks monotonically as the CoC grows and never exceeded ~1 8-bit code
    # value here.
    #
    # T19 did NOT make any pixel here worse: the worst row reads 0.799119 at
    # every K before this task and 0.9926 or better at every K after it.  What
    # changed is that the metric — spread of the minimum across K — is no
    # longer pinned by a K-invariant trough, so it now shows the composite.
    # M1.P3.T17 read it and did not move it: the composite it kept is the one
    # this check already measured.
    # RETIRED TO A PLAIN CHECK at M1.P3.T20's review.  M1.P3.T20 fixed the
    # bucket-composite term this measured and the reading went 5.226e-03 ->
    # 0.000e+00 (every K's worst row now reads the same 0.997916), but the check
    # was left carrying `expectedFailure=True, hardTol=8.0e-03` — so a full
    # revert of that fix reports XFAIL here rather than FAIL, which is exactly
    # the unbounded-licence-to-fail this suite's own policy forbids once the
    # residual an XFAIL describes is gone.  Verified by re-rendering this scene
    # against the pre-T20 plugin (5.226e-03) and against the `claimA = cov`
    # mutation (5.228e-03): both are FAILs now, XFAILs before.
    spread = max(abs(min(profiles[k]) - min(profiles[settings.k]))
                 for k in profiles)
    checks.append(tolCheck(
        "l", "l3 K-dependence of the residual (was the trough's K-invariance)",
        spread, 1.0e-06,
        population="K=%s" % "/".join(str(k) for k in sorted(profiles)),
        note="worst-row alpha " + " ".join("K%d:%.6f" % (k, min(profiles[k]))
                                           for k in sorted(profiles))
             + " — the K=64 outlier is one row at the 0.5px sharp<->disc "
               "threshold and belongs to the bucket composite (M1.P3.T17), "
               "not to DiscKernelLUT"))

    # --- l4: the control. The SAME radius held constant over the frame must be
    # flat to float precision, which is what makes l1/l2 a measurement of the
    # TRANSITION rather than of small radii in general — and proves those gates
    # are reachable rather than impossible.
    resetScript()
    controlZ = GROUND_C / (GROUND_Y_HORIZON - worstRow)
    control = render(settings,
                     makeDefocus(settings,
                                 pointLayer(constant2d((0.4, 0.55, 0.7, 1.0)),
                                            controlZ, keepZeroAlpha=False,
                                            premult=True),
                                 size=size, focusDistance=GROUND_FOCUS,
                                 cocMode="manual"),
                     "l_control")
    controlStats = channelStats(control, "A", interior)
    checks.append(tolCheck(
        "l", "l4 control: the SAME radius held constant is flat",
        max(abs(controlStats.minimum - 1.0), abs(controlStats.maximum - 1.0)),
        1.0e-06, population="%d px" % controlStats.count,
        note="constant z=%.4f, radius %.3f px — the same radius as l1's worst "
             "row" % (controlZ, groundRadius(size, worstRow))))

    # --- l5: the SAME ramp turned through 45 degrees and into a radial field.
    # l1's y-ramp crosses each bin edge along one axis only; a real defocus
    # field varies in both, and there the deficits compound. This is the number
    # a fix task should be written against, not l1's.
    #
    # The radial field is the only one of the three with an INTERIOR EXTREMUM
    # (its cone tip, where the CoC field peaks at edgeRadius and its gradient
    # reverses).  A scatter with a spatially varying normalised kernel
    # under-delivers at such a point by construction, over a neighbourhood
    # about one CoC radius wide, and that is a different mechanism from the bin
    # quantisation this scene was built for — so it is measured by l6 instead
    # of being averaged in here.  It is EXCLUDED, never discarded.
    apexX, apexY = FORMAT_W // 2, FORMAT_H // 2
    apexPad = int(math.ceil(edgeRadius)) + 1        # ~one CoC radius, + a pixel

    def _inApex(x, y):
        dx, dy = x - apexX, y - apexY
        return dx * dx + dy * dy <= apexPad * apexPad

    worstShape = None
    apexStats = None
    for label, expr, isRadial in (
            ("y ramp (l1)", "%.6f/(%.1f-y)" % (GROUND_C, GROUND_Y_HORIZON),
             False),
            ("diagonal ramp", "%.6f/(%.1f-(x+y)/2)"
                              % (GROUND_C, GROUND_Y_HORIZON), False),
            ("radial ramp", "%.6f/(%.1f-sqrt((x-%d)*(x-%d)+(y-%d)*(y-%d)))"
                            % (GROUND_C, GROUND_Y_HORIZON, apexX, apexX,
                               apexY, apexY), True)):
        resetScript()
        image = render(settings,
                       makeDefocus(settings,
                                   depthRampLayer(constant2d(GROUND_COLOR),
                                                  expr),
                                   size=size, focusDistance=GROUND_FOCUS,
                                   cocMode="manual"),
                       "l_shape_%s" % label.split()[0])
        stats = channelStats(image, "A", interior,
                             exclude=_inApex if isRadial else None)
        if isRadial:
            apexStats = channelStats(
                image, "A", interior,
                exclude=lambda x, y: not _inApex(x, y))
        if worstShape is None or stats.minimum < worstShape[1]:
            worstShape = (label, stats.minimum, stats.maximum, stats.minAt,
                          stats.count)
    checks.append(tolCheck(
        "l", "l5 worst small-CoC trough over 1D and 2D ramps |a-1|",
        abs(worstShape[1] - 1.0), 1.0 / 255.0,
        population="%d px measured, worst shape: %s at %s"
                   % (worstShape[4], worstShape[0], worstShape[3]),
        note="alpha min %.6f max %.6f; the radial ramp is measured OUTSIDE a "
             "%d px disc at its own field extremum (%d,%d), which l6 measures "
             "instead — see l6 for why that is a different mechanism, and note "
             "that WITHOUT that exclusion this check reads 9.937e-03 and FAILs "
             "on l6's residual. The worst reading here is the disc family's C1 "
             "kink at r=0.5 (radius 0.530 px at the reported pixel), not bin "
             "quantisation."
             % (worstShape[1], worstShape[2], apexPad, apexX, apexY)))

    # --- l6: the CoC field's own extremum, on the radial ramp.
    #
    # Two measurements make this a DIFFERENT mechanism from the bin trough
    # rather than an assertion that it is:
    #   * it is not the bucket composite either: a kernel-only model with NO
    #     buckets at all reproduces this reading exactly (0.990063), and the
    #     rendered value did not move between the two `combine` candidates
    #     (9.937e-03 under both, against l3's 5.226e-03 / 2.186e-04 split) —
    #     re-confirmed at M1.P3.T17, which is why the deletion left it alone;
    #   * it is pinned to a POSITION, not to a RADIUS.  A bin-quantisation
    #     artefact lives at whatever pixels carry the offending radius; this one
    #     stays at the field extremum when `size` is doubled, i.e. when the
    #     radius sitting there moves from 2.5px to 5.0px — an entirely
    #     different part of the kernel grid.  The control render below measures
    #     exactly that.
    #
    # WHERE IT COMES FROM, stated exactly, because the old grid DID read 1.0
    # here and this reading is therefore worse in absolute terms at these 49
    # pixels.  An EXACT per-radius kernel — no grid at all — gives 0.989234
    # here, and the shipped grid gives 0.990063: the dip is what a scatter with
    # a normalised, spatially varying kernel does at a maximum of the radius
    # field, not something quantisation adds.  (Continuum check: a cone of
    # slope `a` under-delivers `1 - 2a/3` at its tip; a = 0.01953 here gives
    # 0.9870, and doubling `size` to a = 0.03907 predicts 0.9740 against the
    # 0.970847 the control render below measures.)  The old grid read exactly
    # 1.0 only because it rasterised the whole extremum neighbourhood (radius
    # 2.40..2.50 px) with ONE disc — it flattened the field instead of tracking
    # it, and paid 2.009e-01 for that ninety scanlines away.  So this is a
    # residual T19 EXPOSED, not one it introduced, and it is accepted rather
    # than fixed: it will show up wherever a CoC field has an interior
    # extremum, at roughly 2/3 of the field's slope there.
    resetScript()
    doubleImage = render(settings,
                         makeDefocus(settings,
                                     depthRampLayer(
                                         constant2d(GROUND_COLOR),
                                         "%.6f/(%.1f-sqrt((x-%d)*(x-%d)+(y-%d)*(y-%d)))"
                                         % (GROUND_C, GROUND_Y_HORIZON, apexX,
                                            apexX, apexY, apexY)),
                                     size=2.0 * size,
                                     focusDistance=GROUND_FOCUS,
                                     cocMode="manual"),
                         "l_apex_double")
    doublePad = int(math.ceil(2.0 * edgeRadius)) + 1
    doubleInterior = insetBox(formatBox(), doublePad + 6)
    doubleApex = channelStats(
        doubleImage, "A", doubleInterior,
        exclude=lambda x, y: ((x - apexX) ** 2 + (y - apexY) ** 2
                              > doublePad * doublePad))
    checks.append(tolCheck(
        "l", "l6 the CoC field's own extremum (scatter, not quantisation)",
        abs(apexStats.minimum - 1.0), 1.0 / 255.0,
        expectedFailure=True, hardTol=1.5e-02,
        population="%d px inside the %d px extremum disc at (%d,%d)"
                   % (apexStats.count, apexPad, apexX, apexY),
        note="alpha min %.6f at %s; at size %.2f (extremum radius %.2f px "
             "instead of %.2f) the dip is still AT THE EXTREMUM and reads "
             "%.6f at %s — it tracks the field, not the kernel grid"
             % (apexStats.minimum, apexStats.minAt, 2.0 * size,
                2.0 * edgeRadius, edgeRadius, doubleApex.minimum,
                doubleApex.minAt)))
    return checks


SCENES = {
    "a": ("size=0 parity with DeepToImage", sceneA),
    "b": ("holdout in focus vs DeepHoldout2", sceneB),
    "c": ("energy conservation", sceneC),
    "d": ("sparse deep input", sceneD),
    "e": ("occlusion / defocused FG over sharp mid", sceneE),
    "f": ("volumetric holdout", sceneF),
    "g": ("banding on a receding ground plane", sceneG),
    "h": ("overlap normalization", sceneH),
    "i": ("sparse reveal / coverage deficit", sceneI),
    "j": ("anamorphic pixel aspect", sceneJ),
    "k": ("proxy + ray-distance", sceneK),
    "l": ("small-CoC transition", sceneL),
}
