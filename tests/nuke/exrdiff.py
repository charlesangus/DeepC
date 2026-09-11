"""Cross-build pixel identity check for two EXRs, outside Nuke.

The harness's own ``compareImages`` (harness.py) does the same per-pixel
ULP comparison but imports ``nuke``, so it can't run standalone. This is
the same idea over ``exrio.readExr`` -- plain python3, no numpy, no nuke:

    python3 tests/nuke/exrdiff.py A.exr B.exr

Exits 0 only if every shared channel, over the intersection of the two
data windows, has max ULP distance 0. Exits 1 if any channel differs, 2
on a read error or if the images share no channels.
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from exrio import readExr                                        # noqa: E402


def ulps(a, b):
    """Distance in representable floats between two same-signed finite floats."""
    ia = struct.unpack("<i", struct.pack("<f", a))[0]
    ib = struct.unpack("<i", struct.pack("<f", b))[0]
    ia = -2147483648 - ia if ia < 0 else ia
    ib = -2147483648 - ib if ib < 0 else ib
    return abs(ia - ib)


def main(argv):
    if len(argv) != 2:
        sys.stderr.write("usage: exrdiff.py A.exr B.exr\n")
        return 2
    try:
        imgA, imgB = readExr(argv[0]), readExr(argv[1])
    except (IOError, OSError, ValueError) as exc:
        sys.stderr.write("exrdiff: %s\n" % exc)
        return 2

    channels = sorted(set(imgA.channels()) & set(imgB.channels()))
    if not channels:
        sys.stderr.write("exrdiff: no common channels\n")
        return 2
    x0, x1 = max(imgA.x0, imgB.x0), min(imgA.x1, imgB.x1)
    y0, y1 = max(imgA.y0, imgB.y0), min(imgA.y1, imgB.y1)

    worstUlps = 0
    for c in channels:
        maxAbs, maxUlps = 0.0, 0
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                va, vb = imgA.at(c, x, y), imgB.at(c, x, y)
                maxAbs = max(maxAbs, abs(va - vb))
                maxUlps = max(maxUlps, ulps(va, vb))
        worstUlps = max(worstUlps, maxUlps)
        print("%s: max|delta|=%.3e max_ulps=%d" % (c, maxAbs, maxUlps))
    print("summary: %d channel(s), worst_ulps=%d" % (len(channels), worstUlps))
    return 0 if worstUlps == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
