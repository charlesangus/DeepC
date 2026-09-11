"""Generates the committed validation-scene scripts under tests/nuke/*.nk.

Each scene (a)-(m) is built with the SAME construction helpers scenes.py
renders through (harness.py's makeDefocus/deepMerge/pointLayer/... and a
handful of scenes.py's own builders), so a script opened in the GUI shows
exactly the graph the headless harness measures — not a hand-drawn
approximation of it.

Several scenes sweep a harness parameter (K, pre_merge, proxy scale, ...)
across many cells; a saved script is one fixed graph, so each of those pins a
few representative cells and says so in its StickyNote.  Where a scene's
harness family contains a documented FAIL or XFAIL, the script pins that too
rather than only the green cells — see the CHECK_* notes below for exactly what
every script pins, and what it deliberately leaves to the headless run.

Run inside Nuke's terminal interpreter:

    NUKE_PATH=<plugin dir> Nuke17.0 -t tests/nuke/generate_scene_scripts.py
"""

import math
import os
import sys

import nuke

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import (                                            # noqa: E402
    FORMAT_H, FORMAT_W, Settings, constant2d, deepHoldout, deepMerge,
    deepToImage, depthRampLayer, makeDefocus, pointLayer, rectangle2d,
    resetScript, slab,
)
from scenes import (                                              # noqa: E402
    CHECKER_CELL_PX, GROUND_COLOR, GROUND_FOCUS, HALO_BG, HALO_FG, HALO_FOCUS,
    HALO_HOLDOUT_ALPHA, HALO_HOLDOUT_Z, HALO_NEAR_Z, HALO_RADIUS, HALO_SIZE,
    UNEQ_CARD_A, UNEQ_CARD_B,
    _uneqAnchor, _uneqCard, groundPlane, haloBackground, haloForeground,
    haloHoldout, haloSparse, overChecker, stripSource, texturedLayer,
)

OUT_DIR = os.path.dirname(os.path.abspath(__file__))


def stickyNote(text, xpos, ypos, width=560):
    node = nuke.nodes.StickyNote()
    node["label"].setValue(text)
    node.setXYpos(xpos, ypos)
    node["note_font_size"].setValue(14)
    return node


def diffNode(a, b, xpos, ypos):
    node = nuke.nodes.Merge2(inputs=[a, b], operation="difference")
    node.setXYpos(xpos, ypos)
    node["label"].setValue("reference vs measured")
    return node


CHECK_A = (
    "Scene (a) -- size=0 / all-in-focus parity\n\n"
    "Check: at size=0 the node must match stock DeepToImage (volumetric_"
    "composition ON) to within a couple of ULP -- NOT bit-exact, because the "
    "mandatory fractional two-bucket split cannot land a sample exactly on a "
    "bucket centre.\n\n"
    "Pinned here: the point-sample row (4 depth-separated textured layers), "
    "tolerance 2e-07 absolute. The harness also runs a coincident-depth row "
    "and an overlapping-volumetric-spans row (tolerance 2.4e-07) and a pair "
    "of non-occluding-holdout rows, none reproduced here."
)

CHECK_B = (
    "Scene (b) -- holdout with everything in focus\n\n"
    "Check: with a holdout connected and size=0, the node must match "
    "DeepHoldout2 (which flattens internally -- do not chain a DeepToImage "
    "after it, that reads back a uniform black frame).\n\n"
    "Pinned here: an opaque full-alpha card holdout (b1). The harness also "
    "runs a partial-alpha holdout (b2) and a second reference built from "
    "DeepMerge2's own holdout operation, neither reproduced here."
)

CHECK_C = (
    "Scene (c) -- energy conservation\n\n"
    "Check: a constant-colour, constant-depth DeepCConstant field must stay "
    "a flat field at any CoC, in the bbox interior, alpha == 1 exactly.\n\n"
    "Pinned here: an opaque slab at size=12, focus_distance=30 (c1/c2). The "
    "harness also reads total scattered alpha on a bounded card (c3) and a "
    "semi-transparent fog slab (c4) at the same size/focus, neither "
    "reproduced here."
)

CHECK_D = (
    "Scene (d) -- sparse deep input\n\n"
    "Check: zero-sample regions must render exactly black, including beyond "
    "the scatter radius once the card is defocused.\n\n"
    "Two branches share one card source: 'sharp' (size=0) and 'defocused' "
    "(size=12, focus_distance=30). Both must read bitwise zero outside the "
    "card (plus its scatter radius for the defocused branch)."
)

CHECK_E = (
    "Scene (e) -- occlusion\n\n"
    "Check: three depth-separated DeepCConstant cards merged with DeepMerge2 "
    "-- the defocused FG card must bloom outward over the sharp mid card, "
    "and the BG must not bleed through the opaque FG.\n\n"
    "(The Escape mid-cook-cancel clause in this scene's harness check is not "
    "exercisable headless and is reported SKIP there.)"
)

CHECK_F = (
    "Scene (f) -- volumetric holdout\n\n"
    "Check: a partial-alpha fog-slab holdout must attenuate a depth-graded "
    "series of probe strips by the analytic (1-alpha)^t, t the fractional "
    "depth across the fog span.\n\n"
    "Pinned here (top row): fog slab alpha 0.90 spanning z=[5, 25] at size=0, "
    "against the same probe strips rendered with no holdout for comparison. "
    "That is the harness's f1, and it PASSES (4.4e-08 against a 1e-06 gate).\n\n"
    "The bottom row pins the harness's f3e, which FAILS -- see its own note. "
    "The harness's other scene-(f) rows are not reproduced here: f2 (opaque-"
    "holdout erasure onset, XFAIL), f3/f3b (equal-density fog density, PASS), "
    "f3c/f3d (equal-density overlapping slabs, XFAIL), f3f (the unequal-"
    "density sweep; FAILS), f3g/f3h (pinned XFAILs: the coverage fill's "
    "background-radius mismatch, with f3g2/f3h2 as the background_depth "
    "controls that return them to exact) and f3i."
)

CHECK_F3E = (
    "f3e -- unequal-density over-read (a PLAIN FAIL, not an xfail)\n\n"
    "Two 40x40 cards 8 px apart, depth spans [8,12] at alpha 0.99 and [9,13] "
    "at alpha 0.10, defocused at size=14. Below saturation the node's area "
    "model is additive, so the merged render must equal the sum of the two "
    "cards rendered SEPARATELY. It does not: the harness reads +87.2% high "
    "on the worst probed pixel against a 0.5% gate (+77.4% before the "
    "coverage fill; the extra is the fill's background-radius mismatch, "
    "which f3g/f3h isolate), i.e. the node INVENTS alpha -- more than the "
    "two cards deposited.\n\n"
    "'soloA + soloB' is that oracle and 'oracle vs merged' is the error. The "
    "faint corner card is the RANGE ANCHOR: every branch here must measure the "
    "same depth range, or solo and merged would be bucketed differently and "
    "the identity would not be exact. Pixels its own blur reaches are excluded "
    "from the harness's reading, which is what the 'anchor alone' branch is "
    "for.\n\n"
    "The harness's percentage is over a filtered pixel set (anchor-free, both "
    "cards present, estimated coverage <= 0.90) and is not what the difference "
    "node displays; the difference node shows WHERE the over-read is."
)

CHECK_G = (
    "Scene (g) -- banding on a receding ground plane\n\n"
    "Check: an opaque ground plane receding continuously through focus must "
    "show no visible seams at bucket boundaries at K=16; compare K=8 vs "
    "K=64.\n\n"
    "Top row: three DeepCDefocus branches off the same OPAQUE ground-plane "
    "source, depth_layers=8/16/64 (16 is the shipping default). That is the "
    "harness's g1/g2/g3 and the scene's own stated criterion, and it PASSES -- "
    "the worst row-to-row seam is 3.9e-05 against the 1/255 an 8-bit view "
    "resolves.\n\n"
    "Bottom row: the SAME ramp at alpha < 1, which is where the residual is. "
    "Both branches are documented XFAILs in the harness and neither reads its "
    "input alpha back -- see their own note. The coverage fill does not reach "
    "them: it divides only where less than unit weight arrives, and on this "
    "ramp's interior the arrival is 1.06-1.09 everywhere."
)

CHECK_G_ALPHA = (
    "The alpha<1 arms of the same ramp (harness g4 and g5) -- both XFAIL\n\n"
    "Identical rig to the opaque branches above, at K=16, with the plane's "
    "premultiplied colour scaled to alpha 0.90 and alpha 0.10. The correct "
    "interior flat field is the input alpha exactly. It is not:\n\n"
    "  alpha 0.90 (g4): reads 0.8708, a 3.25% DEFICIT\n"
    "  alpha 0.10 (g5): reads 0.1059, a 5.93% OVER-READ\n\n"
    "The over-read grows as alpha falls (the harness also pins alpha 0.30 at "
    "+3.40% and alpha 0.01 at +7.07%, not reproduced here): it is the "
    "scatter's own over-delivery on this steep CoC slope, attenuated by "
    "(1 - alpha). The deficit is one bucket pooling a head and a rear at "
    "unequal per-unit opacity -- accumulation-time information loss, which "
    "no per-bucket composite rule can undo. Neither is a shortfall of "
    "ARRIVAL: the arrival plane reads 1.06-1.09 across this interior, so the "
    "deficit-only coverage fill never engages here, and cannot.\n\n"
    "Both harness checks are pinned BANDS around their reading, so any change "
    "to these numbers -- an improvement included -- must re-pin them."
)

CHECK_H = (
    "Scene (h) -- overlap normalization\n\n"
    "Check: two opaque, same-colour cards overlapping in screen space must "
    "not read alpha > 1 or show a brightened seam in the overlap band.\n\n"
    "The design's literal 'same-depth' framing collapses to a single layer "
    "before the scatter ever runs (the per-pixel tidy pass over-composites "
    "coincident-depth samples first), so this script pins the harness's "
    "actual discriminating case instead: two cards at DIFFERENT depths "
    "(z=6 and z=20) that land in different kernel bins, which is what "
    "reaches the bucket saturation rule."
)

CHECK_I = (
    "Scene (i) -- sparse reveal / coverage fill\n\n"
    "Check: an opaque near card over a distant card, built with exactly one "
    "sample per pixel (no hidden data anywhere), used to show an alpha dip "
    "about one CoC wide inside the silhouette when the near card is "
    "defocused (0.51 at its deepest). The coverage fill now closes it: the "
    "edge band must read alpha 1, filled with the NEAR card's own colour "
    "ratio and no fabricated colour from the far card.\n\n"
    "Two branches: 'sparse' (one sample per pixel; filled to 1 by the node) "
    "and 'DeepMerge twin' (the same visible content but with the occluded "
    "far card's samples kept; alpha 1 by data, and the far card's BLUE "
    "showing through the defocused edge, which the sparse branch must NOT "
    "reproduce). The harness's pre_merge reachability sweep (i6/i7) is not "
    "reproduced here."
)

CHECK_J = (
    "Scene (j) -- anamorphic pixel aspect\n\n"
    "Check: at pixel aspect 2, the bokeh must be elliptical by exactly that "
    "aspect, and the output bbox pad must scale correspondingly in Y.\n\n"
    "The source is a single pixel, so the rendered support IS the kernel. "
    "This script's format is pinned at pixel aspect 2.0; the harness "
    "compares this render against an equivalent pixel-aspect-1.0 run, not "
    "reproduced here."
)

CHECK_K = (
    "Scene (k) -- proxy + ray-distance\n\n"
    "Check: depth_is_ray_distance on a wide-FOV corner pixel must reproduce "
    "ground-truth Z (and be a no-op at the optical centre); separately, "
    "proxy 0.5 must halve the render's radii, not double the blur.\n\n"
    "Pinned here: the ray-distance check only (a 20mm lens on a 36mm "
    "filmback, corner pixel at (8,8)). Three branches at the same pixel: "
    "'ray on' (depth=ray length, corrected), 'ray off' (same depth, "
    "uncorrected) and 'ground truth' (depth set directly to the true Z) -- "
    "'ray on' should match 'ground truth' and visibly differ from 'ray "
    "off'. The proxy-scale sweep (k1/k2) needs the root in two different "
    "proxy states at once and is not reproducible as one static script."
)

CHECK_L = (
    "Scene (l) -- small-CoC transition\n\n"
    "Check: a shallow depth ramp crossing 0-2.5px CoC must show no chatter "
    "or banding at the sharp-path / kernel-LUT-step transitions.\n\n"
    "Same ground plane as scene (g), at a 40x shallower slope. Three "
    "DeepCDefocus branches, depth_layers=8/16/64 (16 is the shipping "
    "default) -- a bucket-composite artefact moves with K, a sharp-path/LUT "
    "artefact does not. The radial/diagonal ramp variants and the CoC-"
    "field-extremum check are not reproduced here."
)

CHECK_M = (
    "Scene (m) -- coverage fill: the silhouette halo\n\n"
    "Check: a defocused opaque card in front of the focal plane must keep "
    "alpha 1 across its silhouette whether or not the renderer wrote "
    "occluded samples behind it, and the coverage the node fills in must "
    "carry the FOREGROUND's colour.\n\n"
    "The card is 96x96 at z=%g with focus at z=%g and size %g, i.e. a CoC "
    "radius of %g px exactly; the background sits ON the focal plane, so it "
    "cannot scatter inward and any background colour inside the silhouette "
    "would be invented. FG and BG share G and B and differ only in R (FG "
    "%.2f, BG %.2f), so G/A and B/A are known analytically everywhere and "
    "R/A reads the FG:BG mix.\n\n"
    "Three branches:\n"
    "  control (m0): DeepMerge2 of card over background -- occluded samples "
    "PRESENT. Alpha 1 across the silhouette, colour ratio the source's; "
    "in the edge band the fill overshoots and the clamp brings the "
    "premultiplied pair back down together.\n"
    "  halo (m1): ONE sample per pixel, no background behind the card. Used "
    "to read alpha 0.27 at the card's corner and 0.52 at its edge (a 16 px "
    "halo); must now read 1 to within 1/255, and the filled band is FG "
    "colour (R/A 0.80).\n"
    "  card over nothing (m2): the bloom must be UNCHANGED by the fill -- "
    "pinned two-sided against the pre-fill plugin's profile across the "
    "right edge on y=128 (-1: 0.5199, 0: 0.4801, +7: 0.2130, +14: 0.0170, "
    "+16: exactly 0). This holds only because the scene is single-depth: "
    "the empty pixels' virtual background scatters at the farthest measured "
    "depth's CoC, which for one object is its own."
    % (HALO_NEAR_Z, HALO_FOCUS, HALO_SIZE, HALO_RADIUS, HALO_FG[0],
       HALO_BG[0])
)

CHECK_M_RAMP = (
    "Scene (m) -- coverage fill: the bands either side of the focal line "
    "(m3)\n\n"
    "Scene (g)'s ground plane at scene (g)'s own steep slope (size 86, focus "
    "10: 0.5 CoC px per scanline, 64 px at the frame edges), depth_layers "
    "16, read over EVERY interior row (66-189) INCLUDING the near-focus "
    "rows scene (g) excludes -- those are the rows this fill is for.\n\n"
    "  alpha 1 (m3a, must hold): every row mean and every pixel within "
    "1/255 of 1. Rows 126/130 -- the only rows whose arrival falls below 1 "
    "on this ramp (0.972) -- read 0.9723 before the fill and 1 now.\n"
    "  alpha 0.9 (m3b, must hold; one-sided): no row within 3 of focus "
    "reads below 0.9 - 1/255. Rows 126/130 read 0.874 before the fill, "
    "0.899 now.\n"
    "  alpha 0.9 (m3c, XFAIL band pin): the worst row within 8 of focus "
    "reads +0.0734 +/- 0.004 ABOVE 0.9 (rows 127/129, arrival 1.201). That "
    "surplus is the scatter's own over-delivery on a steep CoC gradient -- "
    "scene (g)'s g5 arm -- which a deficit-only fill never touches, by "
    "design. Physical range [0, +0.100], the alpha-clamp ceiling. The far "
    "field's -0.030 is g4's and keeps g4's pin; it is not re-pinned here.\n\n"
    "Colour:alpha ratio is asserted beside every alpha reading. Note that "
    "at alpha 0.9 the sample's own unpremultiplied red is 0.36, not 0.40: "
    "DeepFromImage premultiplies the plane's already-premultiplied colour "
    "once more, and the harness reads the source's ratio from a stock "
    "flatten rather than assuming it."
)

CHECK_M_HOLDOUT = (
    "Scene (m) -- coverage fill: fill and holdout commute (m4), and the "
    "renders for the eye (m5)\n\n"
    "The fill divides by the RAW arrival -- deposited before holdout "
    "visibility is folded in -- while every colour and alpha deposit "
    "carries that visibility. A full-frame %.1f-alpha card at z=%g, in "
    "front of everything, therefore halves the numerator and leaves the "
    "divisor alone: visibility is exactly %.1f at every depth, halving is "
    "exact in float, so the held render must be the unheld one halved "
    "wherever nothing saturates.\n\n"
    "  m4a: the flat card over nothing (m2's rig, K=16), with and without "
    "the holdout. Arrival is 1 everywhere, so held/unheld alpha is 0.5 at "
    "EVERY pixel that carries alpha, to n x 2^-24 with n the disc's tap "
    "count (5.4e-05); reads 1.7e-06. Colour scales by the same factor.\n"
    "  m4b: the alpha-1 ramp (m3's rig, K=16), with and without the same "
    "card. On rows 126/130 -- the two rows whose arrival is below 1 (0.972), "
    "the rows the fill moves -- held/unheld is 0.5 at every pixel to "
    "1.9e-06; reads exactly 0. That is the observable form of 'the fill "
    "multiplier is the same with and without the card'. On every OTHER "
    "row arrival exceeds 1 and the ratio is NOT 0.5: the unheld render "
    "clamps its surplus to alpha 1 while the held one, at half, never "
    "reaches the clamp and reads arrival/2 -- 0.6005 on rows 127/129, "
    "~0.536 across the far field. The area model's saturation sets that, "
    "not the fill (which never engages where arrival > 1); it is pinned "
    "two-sided (+/- 0.004) as documented behaviour.\n\n"
    "Folding visibility into the arrival deposit instead reads a ratio of "
    "1 on every one of these pixels: the holdout's attenuation "
    "renormalised straight back.\n\n"
    "  m5: the m1 halo and both m3 ramps are also written over this %d px "
    "checkerboard to --out-dir (default ~/deepc-validation/"
    "over_checker_*.exr), to be compared by eye against a pre-fill build's "
    "files from the same command. Where the board shows through is where "
    "alpha is short; across the measured interior (rows/cols 66-189) it is "
    "hidden completely under the halo and the alpha-1 ramp on a build "
    "whose fill works, and shows through the halo band and rows 126/130 on "
    "the pre-fill build."
    % (HALO_HOLDOUT_ALPHA, HALO_HOLDOUT_Z, HALO_HOLDOUT_ALPHA,
       CHECKER_CELL_PX)
)


def buildSceneA(settings):
    resetScript()
    source = deepMerge([texturedLayer(4.0, 0), texturedLayer(7.5, 1),
                        texturedLayer(11.0, 2), texturedLayer(17.0, 3)])
    source.setXYpos(0, 0)
    reference = deepToImage(source, volumetric=True)
    reference.setXYpos(-120, 120)
    reference["label"].setValue("reference: DeepToImage\nvolumetric ON")
    measured = makeDefocus(settings, source, size=0.0, cocMode="manual")
    measured.setXYpos(120, 120)
    measured["label"].setValue("measured: DeepCDefocus\nsize=0")
    diffNode(reference, measured, 0, 240)
    stickyNote(CHECK_A, 280, 0)


def buildSceneB(settings):
    resetScript()
    source = deepMerge([texturedLayer(5.0, 4), texturedLayer(16.0, 5)])
    source.setXYpos(0, 0)
    holdout = pointLayer(rectangle2d((48, 48, 208, 208), (0.0, 0.0, 0.0, 1.0)),
                         10.0, keepZeroAlpha=False, premult=True)
    holdout.setXYpos(200, 0)
    holdout["label"].setValue("holdout card")
    reference = deepHoldout(source, holdout)
    reference.setXYpos(-120, 140)
    reference["label"].setValue("reference: DeepHoldout2\n(already flat -- no "
                                "DeepToImage after it)")
    measured = makeDefocus(settings, source, holdout=holdout, size=0.0,
                           cocMode="manual")
    measured.setXYpos(120, 140)
    measured["label"].setValue("measured: DeepCDefocus\nsize=0, holdout wired")
    diffNode(reference, measured, 0, 260)
    stickyNote(CHECK_B, 340, 0)


def buildSceneC(settings):
    resetScript()
    source = slab(9.0, 9.5, (0.40, 0.55, 0.70, 1.0))
    source.setXYpos(0, 0)
    node = makeDefocus(settings, source, size=12.0, focusDistance=30.0,
                       cocMode="manual")
    node.setXYpos(0, 120)
    node["label"].setValue("expect: flat alpha==1, flat colour==input")
    stickyNote(CHECK_C, 220, 0)


def buildSceneD(settings):
    resetScript()
    card = pointLayer(rectangle2d((64, 64, 192, 192), (0.6, 0.4, 0.2, 1.0)),
                      10.0, keepZeroAlpha=False, premult=True)
    card.setXYpos(0, 0)
    sharp = makeDefocus(settings, card, size=0.0, cocMode="manual")
    sharp.setXYpos(-120, 120)
    sharp["label"].setValue("sharp: outside the card must be bitwise 0")
    blurred = makeDefocus(settings, card, size=12.0, focusDistance=30.0,
                          cocMode="manual")
    blurred.setXYpos(120, 120)
    blurred["label"].setValue("defocused: beyond the scatter radius must "
                              "still be bitwise 0")
    stickyNote(CHECK_D, 300, 0)


def buildSceneE(settings):
    resetScript()
    fgBox = (30, 30, 120, 220)
    midBox = (90, 60, 200, 200)
    size, focus = 8.0, 10.0
    fgZ, midZ, bgZ = 4.0, 10.0, 20.0

    fg = nuke.nodes.DeepCrop(inputs=[slab(fgZ, fgZ + 0.05, (1.0, 0.0, 0.0, 1.0))])
    fg["use_bbox"].setValue(True)
    fg["bbox"].setValue([float(v) for v in fgBox])
    fg["outside_bbox"].setValue(False)
    fg["use_znear"].setValue(False)
    fg["use_zfar"].setValue(False)
    fg.setXYpos(-160, 0)
    fg["label"].setValue("FG card (red)")

    mid = nuke.nodes.DeepCrop(inputs=[slab(midZ, midZ + 0.05, (0.0, 1.0, 0.0, 1.0))])
    mid["use_bbox"].setValue(True)
    mid["bbox"].setValue([float(v) for v in midBox])
    mid["outside_bbox"].setValue(False)
    mid["use_znear"].setValue(False)
    mid["use_zfar"].setValue(False)
    mid.setXYpos(0, 0)
    mid["label"].setValue("mid card (green), in focus")

    bg = slab(bgZ, bgZ + 0.05, (0.0, 0.0, 1.0, 1.0))
    bg.setXYpos(160, 0)
    bg["label"].setValue("BG (blue), full frame")

    source = deepMerge([fg, mid, bg])
    source.setXYpos(0, 120)
    node = makeDefocus(settings, source, size=size, focusDistance=focus,
                       cocMode="manual")
    node.setXYpos(0, 240)
    stickyNote(CHECK_E, 280, 0)


def buildSceneF(settings):
    resetScript()
    depths = [6.0, 8.5, 11.0, 13.5, 16.0, 18.5, 21.0, 23.5]
    source, _boxes = stripSource(depths)
    source.setXYpos(0, 0)
    holdout = slab(5.0, 25.0, (0.0, 0.0, 0.0, 0.90))
    holdout.setXYpos(200, 0)
    holdout["label"].setValue("fog holdout, alpha 0.90, z=[5, 25]")

    bare = makeDefocus(settings, source, size=0.0, cocMode="manual")
    bare.setXYpos(-140, 140)
    bare["label"].setValue("no holdout: every strip must read 1.0")

    held = makeDefocus(settings, source, holdout=holdout, size=0.0,
                       cocMode="manual")
    held.setXYpos(140, 140)
    held["label"].setValue("holdout wired: attenuation vs analytic "
                          "(1-alpha)^t")
    stickyNote(CHECK_F, 340, 0)

    cardA = _uneqCard(UNEQ_CARD_A, 8.0, 12.0, 0.99, (0.7, 0.5, 0.3))
    cardA.setXYpos(-300, 320)
    cardA["label"].setValue("card A: alpha 0.99, z=[8, 12]")
    cardB = _uneqCard(UNEQ_CARD_B, 9.0, 13.0, 0.10, (0.3, 0.5, 0.7))
    cardB.setXYpos(-140, 320)
    cardB["label"].setValue("card B: alpha 0.10, z=[9, 13]")
    anchor = _uneqAnchor()
    anchor.setXYpos(20, 320)
    anchor["label"].setValue("range anchor (see the note)")

    def uneqBranch(layers, label, xpos):
        node = makeDefocus(settings, deepMerge(layers) if len(layers) > 1
                           else layers[0], size=14.0, focusDistance=30.0,
                           cocMode="manual")
        node.setXYpos(xpos, 460)
        node["label"].setValue(label)
        return node

    uneqBranch([anchor], "anchor alone", -300)
    soloA = uneqBranch([cardA, anchor], "solo A", -140)
    soloB = uneqBranch([cardB, anchor], "solo B", 20)
    merged = uneqBranch([cardA, cardB, anchor], "merged", 180)

    oracle = nuke.nodes.Merge2(inputs=[soloA, soloB], operation="plus")
    oracle.setXYpos(-60, 560)
    oracle["label"].setValue("soloA + soloB")
    error = diffNode(oracle, merged, 60, 640)
    error["label"].setValue("oracle vs merged: every nonzero pixel here is "
                            "alpha the composite invented or lost")
    stickyNote(CHECK_F3E, 340, 320)


def buildSceneG(settings):
    resetScript()
    source = groundPlane()
    source.setXYpos(0, 0)
    xs = (-220, 0, 220)
    for k, xpos in zip((8, settings.k, 64), xs):
        cell = settings.derive(k=k)
        node = makeDefocus(cell, source, size=86.0, focusDistance=GROUND_FOCUS,
                           cocMode="manual")
        node.setXYpos(xpos, 140)
        node["label"].setValue("K=%d" % k)
    stickyNote(CHECK_G, 300, 0)

    for alpha, xpos in ((0.90, -220), (0.10, 0)):
        colour = tuple(c * alpha for c in GROUND_COLOR[:3]) + (alpha,)
        fog = groundPlane(color=colour)
        fog.setXYpos(xpos, 320)
        fog["label"].setValue("same ramp, alpha %.2f" % alpha)
        node = makeDefocus(settings, fog, size=86.0,
                           focusDistance=GROUND_FOCUS, cocMode="manual")
        node.setXYpos(xpos, 440)
        node["label"].setValue("K=%d, expect a flat %.2f" % (settings.k, alpha))
    stickyNote(CHECK_G_ALPHA, 300, 320)


def buildSceneH(settings):
    resetScript()
    colour = (0.60, 0.60, 0.60, 1.0)
    cardA = pointLayer(rectangle2d((40, 64, 168, 192), colour), 6.0,
                       keepZeroAlpha=False, premult=True)
    cardA.setXYpos(-140, 0)
    cardA["label"].setValue("card A, z=6")
    cardB = pointLayer(rectangle2d((88, 64, 216, 192), colour), 20.0,
                       keepZeroAlpha=False, premult=True)
    cardB.setXYpos(140, 0)
    cardB["label"].setValue("card B, z=20 (different kernel bin than A)")
    mixed = deepMerge([cardA, cardB])
    mixed.setXYpos(0, 120)
    node = makeDefocus(settings, mixed, size=18.0, focusDistance=10.0,
                       cocMode="manual")
    node.setXYpos(0, 240)
    node["label"].setValue("expect: max alpha <= 1, no brightened seam")
    stickyNote(CHECK_H, 320, 0)


def buildSceneI(settings):
    resetScript()
    silhouette = (64, 64, 192, 192)
    nearZ, farZ = 4.0, 20.0
    focus, size = 20.0, 6.0
    nearColour, farColour = 0.80, 0.60
    inside = ("(x>=%d && x<%d && y>=%d && y<%d)"
              % (silhouette[0], silhouette[2], silhouette[1], silhouette[3]))

    image = nuke.nodes.Expression(inputs=[constant2d((0.0, 0.0, 0.0, 0.0))])
    image["expr0"].setValue("%s ? %g : 0.0" % (inside, nearColour))
    image["expr1"].setValue("0.0")
    image["expr2"].setValue("%s ? 0.0 : %g" % (inside, farColour))
    image["expr3"].setValue("1.0")
    image.setXYpos(-160, 0)
    sparseSource = depthRampLayer(image, "%s ? %g : %g"
                                  % (inside, nearZ, farZ))
    sparseSource.setXYpos(-160, 60)
    sparseSource["label"].setValue("sparse: one sample per pixel, no hidden "
                                   "data")

    near = pointLayer(rectangle2d(silhouette, (nearColour, 0.0, 0.0, 1.0)),
                      nearZ, keepZeroAlpha=False, premult=True)
    near.setXYpos(60, 0)
    far = pointLayer(constant2d((0.0, 0.0, farColour, 1.0)), farZ,
                     keepZeroAlpha=False, premult=True)
    far.setXYpos(220, 0)
    mergedSource = deepMerge([near, far])
    mergedSource.setXYpos(140, 60)
    mergedSource["label"].setValue("DeepMerge twin: same visible content, "
                                  "occluded far samples KEPT")

    sparseNode = makeDefocus(settings, sparseSource, size=size,
                             focusDistance=focus, cocMode="manual")
    sparseNode.setXYpos(-160, 180)
    sparseNode["label"].setValue("edge band filled to alpha 1, FG colour")

    mergedNode = makeDefocus(settings, mergedSource, size=size,
                             focusDistance=focus, cocMode="manual")
    mergedNode.setXYpos(140, 180)
    mergedNode["label"].setValue("alpha 1 by data; far card's blue shows "
                                 "through the edge")

    stickyNote(CHECK_I, 340, 0)


def buildSceneJ(settings):
    resetScript(pixelAspect=2.0)
    rect = rectangle2d((128, 128, 129, 129), (1.0, 1.0, 1.0, 1.0))
    rect.setXYpos(0, 0)
    card = pointLayer(rect, 10.0, keepZeroAlpha=False, premult=True)
    card.setXYpos(0, 60)
    node = makeDefocus(settings, card, size=10.0, focusDistance=30.0,
                       cocMode="manual")
    node.setXYpos(0, 180)
    node["label"].setValue("expect: bokeh stretched 2x vertically")
    stickyNote(CHECK_J, 220, 0)


def buildSceneK(settings):
    resetScript()
    focalMm, filmbackMm = 20.0, 36.0
    rayDistance = 8.0
    manualSize, manualFocus = 20.0, 10.0
    cornerX, cornerY = 8, 8

    mmPerPx = filmbackMm / FORMAT_W
    dx = (cornerX + 0.5 - 0.5 * FORMAT_W) * mmPerPx
    dy = (cornerY + 0.5 - 0.5 * FORMAT_H) * mmPerPx
    rMm = math.sqrt(dx * dx + dy * dy)
    cornerZ = rayDistance * focalMm / math.sqrt(focalMm * focalMm + rMm * rMm)

    def pixel(depth, xpos):
        node = pointLayer(rectangle2d((cornerX, cornerY, cornerX + 1,
                                       cornerY + 1), (1.0, 1.0, 1.0, 1.0)),
                          depth, keepZeroAlpha=False, premult=True)
        node.setXYpos(xpos, 0)
        return node

    onSource = pixel(rayDistance, -220)
    onNode = makeDefocus(settings, onSource, size=manualSize,
                         focusDistance=manualFocus, cocMode="manual",
                         focal_length=focalMm, filmback_width=filmbackMm,
                         depth_is_ray_distance=True)
    onNode.setXYpos(-220, 120)
    onNode["label"].setValue("ray-distance ON, depth=%.1f (ray length)"
                            % rayDistance)

    offSource = pixel(rayDistance, 0)
    offNode = makeDefocus(settings, offSource, size=manualSize,
                          focusDistance=manualFocus, cocMode="manual",
                          focal_length=focalMm, filmback_width=filmbackMm,
                          depth_is_ray_distance=False)
    offNode.setXYpos(0, 120)
    offNode["label"].setValue("ray-distance OFF, same depth=%.1f "
                             "(uncorrected -- expect a visibly different "
                             "radius)" % rayDistance)

    truthSource = pixel(cornerZ, 220)
    truthNode = makeDefocus(settings, truthSource, size=manualSize,
                            focusDistance=manualFocus, cocMode="manual",
                            focal_length=focalMm, filmback_width=filmbackMm,
                            depth_is_ray_distance=False)
    truthNode.setXYpos(220, 120)
    truthNode["label"].setValue("ground truth: depth=%.5f (Z, not ray "
                               "length) -- must match 'ray-distance ON'"
                               % cornerZ)

    stickyNote(CHECK_K, 400, 0)


def buildSceneL(settings):
    resetScript()
    source = groundPlane()
    source.setXYpos(0, 0)
    xs = (-220, 0, 220)
    for k, xpos in zip((8, settings.k, 64), xs):
        cell = settings.derive(k=k)
        node = makeDefocus(cell, source, size=3.36, focusDistance=GROUND_FOCUS,
                           cocMode="manual")
        node.setXYpos(xpos, 140)
        node["label"].setValue("K=%d" % k)
    stickyNote(CHECK_L, 300, 0)


def buildSceneM(settings):
    resetScript()
    # The halo family: three branches off the same foreground card.
    fg = haloForeground()
    fg.setXYpos(-330, 0)
    fg["label"].setValue("FG card, z=%g, CoC radius %g px" % (HALO_NEAR_Z,
                                                              HALO_RADIUS))
    bg = haloBackground()
    bg.setXYpos(-170, 0)
    bg["label"].setValue("BG, full frame, ON the focal plane")
    control = deepMerge([fg, bg])
    control.setXYpos(-250, 100)
    control["label"].setValue("control: occluded BG samples PRESENT")
    controlNode = makeDefocus(settings, control, size=HALO_SIZE,
                              focusDistance=HALO_FOCUS, cocMode="manual")
    controlNode.setXYpos(-250, 220)
    controlNode["label"].setValue("m0: alpha 1 across the silhouette, "
                                  "source colour ratio")

    sparse = haloSparse()
    sparse.setXYpos(0, 100)
    sparse["label"].setValue("halo: ONE sample per pixel, no BG behind "
                             "the card")
    haloNode = makeDefocus(settings, sparse, size=HALO_SIZE,
                           focusDistance=HALO_FOCUS, cocMode="manual")
    haloNode.setXYpos(0, 220)
    haloNode["label"].setValue("m1: NO dip (was 0.27-0.52 deep), filled "
                               "band is FG colour")
    haloBoard = overChecker(haloNode)
    haloBoard.setXYpos(0, 320)
    haloBoard["label"].setValue("m5: the halo over the board (for the eye)")

    alone = haloForeground()
    alone.setXYpos(250, 100)
    alone["label"].setValue("the same card over NOTHING")
    bloomCell = settings.derive(k=16)
    bloomNode = makeDefocus(bloomCell, alone, size=HALO_SIZE,
                            focusDistance=HALO_FOCUS, cocMode="manual")
    bloomNode.setXYpos(250, 220)
    bloomNode["label"].setValue("m2: bloom UNCHANGED by the fill (pinned "
                                "profile, K=16)")
    stickyNote(CHECK_M, 480, 0)

    # m4a: the same card over nothing, held out by the half-alpha card.
    holdout = haloHoldout()
    holdout.setXYpos(-170, 320)
    holdout["label"].setValue("m4 holdout: full-frame alpha %.1f card at "
                              "z=%g, in front of everything"
                              % (HALO_HOLDOUT_ALPHA, HALO_HOLDOUT_Z))
    heldCard = makeDefocus(bloomCell, alone, size=HALO_SIZE,
                           focusDistance=HALO_FOCUS, cocMode="manual",
                           holdout=holdout)
    heldCard.setXYpos(250, 320)
    heldCard["label"].setValue("m4a: exactly half of m2 at every pixel "
                               "(arrival 1 everywhere)")

    # The ramp: scene (g)'s rig, both alphas, K=16.
    rampCell = settings.derive(k=16)
    rampNodes = {}
    for alpha, xpos, label in (
            (1.0, -170, "m3a: EVERY interior row within 1/255 of 1"),
            (0.90, 90, "m3b: no near-focus row short of 0.9 - 1/255; "
                       "m3c: near-focus surplus pinned +0.0734")):
        colour = tuple(c * alpha for c in GROUND_COLOR[:3]) + (alpha,)
        ramp = groundPlane(color=colour)
        ramp.setXYpos(xpos, 420)
        ramp["label"].setValue("scene (g)'s ramp, alpha %.2f" % alpha)
        node = makeDefocus(rampCell, ramp, size=86.0,
                           focusDistance=GROUND_FOCUS, cocMode="manual")
        node.setXYpos(xpos, 540)
        node["label"].setValue("K=16; " + label)
        rampNodes[alpha] = (ramp, node)
        board = overChecker(node)
        board.setXYpos(xpos, 640)
        board["label"].setValue("m5: alpha %.2f ramp over the board (for "
                                "the eye)" % alpha)
    stickyNote(CHECK_M_RAMP, 480, 420)

    # m4b: the alpha-1 ramp held out by the same card.
    heldRamp = makeDefocus(rampCell, rampNodes[1.0][0], size=86.0,
                           focusDistance=GROUND_FOCUS, cocMode="manual",
                           holdout=holdout)
    heldRamp.setXYpos(-330, 540)
    heldRamp["label"].setValue("m4b: half of m3a on rows 126/130 (arrival "
                               "0.972); arrival/2 elsewhere (0.6005 at "
                               "127/129)")
    stickyNote(CHECK_M_HOLDOUT, 480, 900)


SCENE_BUILDERS = [
    ("a", "scene_a_size0_parity.nk", buildSceneA),
    ("b", "scene_b_holdout_in_focus.nk", buildSceneB),
    ("c", "scene_c_energy_conservation.nk", buildSceneC),
    ("d", "scene_d_sparse_deep_input.nk", buildSceneD),
    ("e", "scene_e_occlusion.nk", buildSceneE),
    ("f", "scene_f_volumetric_holdout.nk", buildSceneF),
    ("g", "scene_g_banding.nk", buildSceneG),
    ("h", "scene_h_overlap_normalization.nk", buildSceneH),
    ("i", "scene_i_sparse_reveal.nk", buildSceneI),
    ("j", "scene_j_anamorphic.nk", buildSceneJ),
    ("k", "scene_k_proxy_ray_distance.nk", buildSceneK),
    ("l", "scene_l_small_coc_transition.nk", buildSceneL),
    ("m", "scene_m_coverage_halo.nk", buildSceneM),
]


def sanitizeScript(path):
    """Strip the two things Nuke's own writer ties to this machine.

    The shebang points at the local Nuke install and the Root ``name`` knob
    is the absolute save path; neither is meaningful once the file is opened
    from somewhere else, and both would otherwise embed this machine's paths
    in a committed file.
    """
    with open(path, "r") as f:
        lines = f.readlines()
    if lines and lines[0].startswith("#!"):
        lines = lines[1:]
    filename = os.path.basename(path)
    cleaned = []
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("name ") and path in line:
            cleaned.append(" name %s\n" % filename)
        else:
            cleaned.append(line)
    with open(path, "w") as f:
        f.writelines(cleaned)


def main():
    settings = Settings()
    for letter, filename, build in SCENE_BUILDERS:
        build(settings)
        path = os.path.join(OUT_DIR, filename)
        nuke.scriptSaveAs(path, overwrite=1)
        sanitizeScript(path)
        print("wrote %s" % path)


if __name__ == "__main__":
    main()
