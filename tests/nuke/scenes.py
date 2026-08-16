"""Validation scenes (a)-(f) from the M1 Design reference's scene list.

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
    deepMerge, deepMergeHoldout, deepToImage, formatBox, insetBox,
    makeDefocus, pointLayer, rectangle2d, render, resetScript, slab, tolCheck,
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
    # is INVARIANT across K=4/8/16/64/128 and both bucket-combine candidates
    # (measured), so 1e-03 left two orders of magnitude of dead slack.
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
    front of it; that is the residual T18 judges, reported here, not fixed.
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
    # holdout interpolants, pre_merge on/off and merge_tolerance 0.25/2.0.
    # (It is 2.5e-01 under combine=FrontToBackOver, which the old gate also
    # caught; 1e-06 additionally catches anything smaller than a quarter.)
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
    checks.append(boolCheck(
        "f", "f2 opaque holdout erases unoccluded FG (M1.P3.T10)",
        not erased,
        "%d strip(s); %.3f depth units (%.0f%% of the bracket) erased in front"
        % (len(erased), erasedDepth, 100.0 * erasedDepth / bracket),
        "0 strips",
        population="%d/%d unoccluded strip px erased; %s"
                   % (erasedPixels, stripPixels,
                      ", ".join("z=%.3f -> %.4f" % e for e in erased) or "none"),
        note="holdout z=%.3f, bracket (range/K) = %.3f spanning [%.3f, %.3f]; "
             "%s; %s"
             % (holdoutZ, bracket, bracketLow, bracketLow + bracket,
                onsetNote, detail),
        expectedFailure=True))

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

    densityRows = [
        ("f3  fog density, size=0, separated slabs", separatedSlabs, 0.0,
         1.0e-05, 8, False),
        ("f3b fog density, defocused, separated slabs", separatedSlabs, 12.0,
         1.0e-05, settings.maxRadius + 4, False),
        ("f3c fog density, defocused, overlapping slabs", overlappingSlabs,
         12.0, 1.0e-05, settings.maxRadius + 4, True),
        ("f3d fog density, defocused, span + point (shared bucket, "
         "different split fractions)", spanPlusPoint, 12.0, 1.0e-05,
         settings.maxRadius + 4, True),
    ]
    for name, build, size, tol, inset, documented in densityRows:
        resetScript()
        node = makeDefocus(settings, build(), size=size, focusDistance=30.0,
                           cocMode="manual")
        img = render(settings, node, "f_density", box=formatBox())
        stats = channelStats(img, "A", insetBox(formatBox(), inset))
        loss = (exact - stats.mean) / exact * 100.0
        checks.append(boolCheck(
            "f", name, abs(stats.mean - exact) <= tol,
            "%.7f (%+.3f%%)" % (stats.mean, -loss), "0.75 +/- %.0e" % tol,
            population="interior flat field, %d px" % stats.count,
            # The Decisions entry of 2026-07-26 calls this a "~4%/layer LOSS"
            # and "identical under both bucket-combine candidates". Neither
            # holds as stated: the deviation is SIGNED and flips with the
            # candidate (T12 review measured -0.113%/-1.675% under
            # CoveragePartition against +0.074%/+0.967% under
            # FrontToBackOver), so T17 must not treat it as a common-mode term.
            note="documented different-split-fraction residual (Decisions "
                 "2026-07-26); signed, and NOT candidate-independent as that "
                 "entry states — see the T12 review"
                 if documented else "",
            expectedFailure=documented))
    return checks


SCENES = {
    "a": ("size=0 parity with DeepToImage", sceneA),
    "b": ("holdout in focus vs DeepHoldout2", sceneB),
    "c": ("energy conservation", sceneC),
    "d": ("sparse deep input", sceneD),
    "e": ("occlusion / defocused FG over sharp mid", sceneE),
    "f": ("volumetric holdout", sceneF),
}
