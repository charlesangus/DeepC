"""Validation scenes (a)-(l) from the M1 Design reference's scene list.

Every scene builds its graph from Python nodes (no committed ``.nk`` — that is
M1.P5.T3's job), renders through ``harness.render()`` and reports a number.
No scene reads or writes anything under ``src/``: a failure here is reported
with its magnitude and pixel population, never patched.
"""

import math

import nuke

from harness import (
    BUCKET_COMBINE, BUCKET_COMBINE_LABEL, FORMAT_H, FORMAT_W, RGBA, SKIP, Check,
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
            # holds as stated: the deviation is SIGNED and flips with the
            # candidate (T12 review measured -0.113%/-1.675% under
            # CoveragePartition against +0.074%/+0.967% under
            # FrontToBackOver), so T17 must not treat it as a common-mode term.
            note="documented different-split-fraction residual (Decisions "
                 "2026-07-26); signed, and NOT candidate-independent as that "
                 "entry states — see the T12 review"
                 if documented else "",
            expectedFailure=documented,
            hardTol=hardPct,
            hardValue=(abs(loss) / 100.0) if hardPct is not None else None))
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


def _rowsFromBinEdge(size, y):
    """Signed distance, in scanlines, from row ``y`` to the nearest
    ``DiscKernelLUT`` bin edge.

    ``DiscKernelLUT::radiusToIndex()`` is ``lround(radius / 0.5)``, so the
    kernel changes where ``radius = n*0.5 + 0.25``.  Scene (l) claims its
    affected rows sit on those edges; this measures the claim instead of
    asserting it, so the note cannot go stale if the artefact ever moves.
    Positive means the row's radius is past the edge (already in the wider
    bin).
    """
    slope = groundSlope(size)
    radius = groundRadius(size, y)
    edge = 0.5 * round((radius - 0.25) / 0.5) + 0.25
    return (radius - edge) / slope if slope else 0.0


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
      * KERNEL-BIN QUANTISATION — the DiscKernelLUT rounds radius onto a 0.5px
        grid, and where the ramp crosses a bin boundary the neighbouring
        scanlines rasterise different discs.  It is K-INVARIANT (measured), it
        is worst at the smallest radii, and it is scene (l)'s subject, so the
        rows within 2.5 px of focus are excluded from every reading here.
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

    # --- g1: the K x combine table M1.P3.T17 inherits.
    #
    # HARD BOUNDS on the XFAILs below.  The bucket-composite deficit this scene
    # measures is documented and is exactly what M1.P3.T17 must judge, so it is
    # XFAIL rather than FAIL — but an unbounded XFAIL would swallow a *new*
    # defect landing on top of the old one and hand T17 a corrupted table.  The
    # bounds are ~2-3x the worst reading on record (g1 7.8e-02, g2 9.6e-02,
    # g3 3.1e-02, all at M1.P3.T16); past them the reading is a regression, not
    # the residual, and the check goes back to a hard FAIL.
    G1_HARD, G2_HARD, G3_HARD = 1.5e-01, 1.5e-01, 1.0e-01
    profiles = {}
    seams = {}
    for combine in ("partition", "over"):
        for k in (8, 16, 64):
            cell = settings.derive(k=k, combine=combine)
            resetScript()
            image = render(cell,
                           makeDefocus(cell, groundPlane(), size=size,
                                       focusDistance=GROUND_FOCUS,
                                       cocMode="manual"),
                           "g_ramp_%s_k%d" % (combine, k))
            profile = (rowMeans(image, "A", lowBox)
                       + rowMeans(image, "A", highBox))
            redProfile = (rowMeans(image, "R", lowBox)
                          + rowMeans(image, "R", highBox))
            profiles[(combine, k)] = profile
            mean = sum(profile) / len(profile)
            worstStep, medianStep, stepAt = stepProfile(profile)
            seams[(combine, k)] = (worstStep, medianStep, stepAt)
            # The colour must track alpha exactly: this input is a single
            # premultiplied colour, so R == 0.40*A everywhere or the composite
            # has desynced the premultiplied pair (a defect shape this milestone
            # has now hit three times).
            worstRatio = max(abs(redProfile[i] - GROUND_COLOR[0] * profile[i])
                             for i in range(len(profile)))
            checks.append(tolCheck(
                "g", "g1 %s K=%-2d interior flat-field alpha |a-1|"
                     % (BUCKET_COMBINE_LABEL[cell.combine], k),
                abs(mean - 1.0), 1.0e-03,
                population="%d rows x %d px, |y-128| >= %d"
                           % (rowCount, colCount, band),
                note="mean %.6f (%+.3f%%) min %.6f (y=%d) max %.6f; worst "
                     "row step %.3e at y=%d (median %.3e); colour:alpha "
                     "residual %.2e"
                     % (mean, (mean - 1.0) * 100.0, min(profile),
                        profileRow(profile.index(min(profile))), max(profile),
                        worstStep, profileRow(stepAt), medianStep, worstRatio),
                expectedFailure=True, hardTol=G1_HARD))

    # --- g2: the part of the banding that is ATTRIBUTABLE TO BUCKETING.
    # A seam the eye can see is a step of about 1/255 in the 8-bit result, so
    # that is the gate; K=64 is the reference because it is the finest bucketing
    # the scene renders, and the difference cancels every K-invariant term
    # (kernel-bin quantisation, the varying-radius scatter residual).
    for combine in ("partition", "over"):
        reference = profiles[(combine, 64)]
        for k in (8, 16):
            profile = profiles[(combine, k)]
            deltas = [abs(a - b) for a, b in zip(profile, reference)]
            worst = max(deltas)
            over = sum(1 for d in deltas if d > 1.0 / 255.0)
            label = BUCKET_COMBINE_LABEL[BUCKET_COMBINE[combine]]
            checks.append(tolCheck(
                "g", "g2 %s K=%-2d bucket-attributable banding vs K=64"
                     % (label, k),
                worst, 1.0 / 255.0,
                population="%d/%d interior rows over 1/255" % (over, rowCount),
                note="max |rowMean(K=%d) - rowMean(K=64)|; worst row y=%d "
                     "(radius %.2f px)"
                     % (k, profileRow(deltas.index(worst)),
                        groundRadius(size, profileRow(deltas.index(worst)))),
                expectedFailure=True, hardTol=G2_HARD))

    # --- g3: the scene's OWN stated criterion — "no visible seams at bucket
    # boundaries at K=16; compare K=8 vs K=64".  g1 (a mean) and g2 (a
    # difference against K=64) both average or cancel a seam away; a seam is a
    # LOCALISED step in the row profile, and until this check existed the
    # milestone's banding scene never gated the thing it is named after.  The
    # median step is carried alongside so the reading can be read as "a spike
    # against a flat neighbourhood" rather than "the profile is that noisy".
    for combine in ("partition", "over"):
        for k in (8, 16, 64):
            worstStep, medianStep, stepAt = seams[(combine, k)]
            label = BUCKET_COMBINE_LABEL[BUCKET_COMBINE[combine]]
            row = profileRow(stepAt)
            checks.append(tolCheck(
                "g", "g3 %s K=%-2d worst seam (row-to-row step in the profile)"
                     % (label, k),
                worstStep, 1.0 / 255.0,
                population="%d interior rows; median step %.3e"
                           % (rowCount, medianStep),
                note="worst step at y=%d (radius %.2f px), %.0fx the median; "
                     "1/255 is the step an 8-bit view resolves"
                     % (row, groundRadius(size, row),
                        (worstStep / medianStep) if medianStep > 0.0 else 0.0),
                expectedFailure=True, hardTol=G3_HARD))
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
    # a pair inside the SAME bin. 5.0 and 5.2 px both round to bin 5.0.
    sameBin = reachability(5.0, 5.2, settings.mergeTolerance)
    checks.append(tolCheck(
        "i", "i7c ...and is lossless when the grouped radii share a kernel bin",
        sameBin.maxAbs, 1.0e-07, population=sameBin.population(),
        note="CoC radius 5.0 and 5.2 px: 0.20 apart like i7, but both round to "
             "the 5.0 px bin, so the grouped disc IS the pair's disc"))
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

    l1/l2 FAIL, and the failure is the node's.  ``DiscKernelLUT::radiusToIndex``
    is ``lround(radius/0.5)``, so where the ramp crosses a bin edge the two
    sides of the crossing rasterise different discs and the destination row on
    the crossing is short by exactly half the difference in the two kernels'
    CENTRE-ROW weight.  Predicted from the LUT alone (M1.P3.T16's review):
    0.799119 / 0.905153 / 0.948266 / 0.973739 at the 0.5->1.0 / 1.0->1.5 /
    1.5->2.0 / 2.0->2.5 crossings, which is what l1 measures to six decimals.
    The matching surplus one row the other side is destroyed by the saturation
    rule (``max`` over the profile is exactly 1.0), so the artefact is a net
    energy LOSS, not a zero-mean ripple, and colour tracks alpha exactly
    (R/A = 0.400000 everywhere) — a one-scanline 20% dark line across an opaque
    surface.  l5 shows a 2D ramp makes it worse still.
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
             "y(radius, alpha, rows-from-the-nearest DiscKernelLUT bin edge at "
             "radius = n*0.5 + 0.25) — MEASURED, not asserted: %s"
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

    spread = max(abs(min(profiles[k]) - min(profiles[settings.k]))
                 for k in profiles)
    checks.append(tolCheck(
        "l", "l3 the artefact is K-INVARIANT (so it is not the bucketing)",
        spread, 1.0e-06,
        population="K=%s" % "/".join(str(k) for k in sorted(profiles)),
        note="worst-row alpha " + " ".join("K%d:%.6f" % (k, min(profiles[k]))
                                           for k in sorted(profiles))))

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
    worstShape = None
    for label, expr in (
            ("y ramp (l1)", "%.6f/(%.1f-y)" % (GROUND_C, GROUND_Y_HORIZON)),
            ("diagonal ramp", "%.6f/(%.1f-(x+y)/2)"
                              % (GROUND_C, GROUND_Y_HORIZON)),
            ("radial ramp", "%.6f/(%.1f-sqrt((x-%d)*(x-%d)+(y-%d)*(y-%d)))"
                            % (GROUND_C, GROUND_Y_HORIZON, FORMAT_W // 2,
                               FORMAT_W // 2, FORMAT_H // 2, FORMAT_H // 2))):
        resetScript()
        image = render(settings,
                       makeDefocus(settings,
                                   depthRampLayer(constant2d(GROUND_COLOR),
                                                  expr),
                                   size=size, focusDistance=GROUND_FOCUS,
                                   cocMode="manual"),
                       "l_shape_%s" % label.split()[0])
        stats = channelStats(image, "A", interior)
        if worstShape is None or stats.minimum < worstShape[1]:
            worstShape = (label, stats.minimum, stats.maximum, stats.minAt)
    checks.append(tolCheck(
        "l", "l5 worst small-CoC trough over 1D and 2D ramps |a-1|",
        abs(worstShape[1] - 1.0), 1.0 / 255.0,
        population="%d px interior, worst shape: %s at %s"
                   % (channelStats(image, "A", interior).count, worstShape[0],
                      worstShape[3]),
        note="alpha min %.6f max %.6f; the y-ramp l1 gates on is the MILDEST "
             "of the three — a CoC field varying along both axes crosses the "
             "bin edge on a diagonal and the deficits compound"
             % (worstShape[1], worstShape[2])))
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
