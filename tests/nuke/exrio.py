"""Minimal uncompressed 32-bit-float scanline OpenEXR reader.

Nuke's bundled Python has neither numpy nor OpenImageIO (verified on
Nuke 17.0v3), and ``nuke.sample()`` returns NaN on a freshly built Op in
terminal mode, so the validation harness renders every image through a
``Write`` node configured as *32 bit float / no compression / raw* and reads
the pixels back with this parser.

Only the subset Nuke writes under that configuration is supported:
single-part, scanline, NO_COMPRESSION, FLOAT channels.  Anything else raises,
loudly, rather than silently mis-reading pixels a gate depends on.
"""

import array
import struct
import sys

EXR_MAGIC = 20000630

PIXELTYPE_UINT = 0
PIXELTYPE_HALF = 1
PIXELTYPE_FLOAT = 2


class Image(object):
    """A rectangular float image, indexed in absolute *Nuke* coordinates.

    ``planes`` maps an EXR channel name ("R", "G", "B", "A", ...) to a flat
    ``array('f')`` of ``width * height`` values in row-major order starting at
    ``(x0, y0)``.  EXR scanlines run top-down and Nuke's y axis runs bottom-up,
    so ``readExr()`` flips the rows on the way in: every coordinate this class
    exposes is the same y a Nuke knob or a ``Crop`` box would use.
    """

    def __init__(self, x0, y0, x1, y1, planes):
        self.x0 = x0
        self.y0 = y0
        self.x1 = x1          # inclusive, EXR dataWindow convention
        self.y1 = y1          # inclusive
        self.planes = planes

    @property
    def width(self):
        return self.x1 - self.x0 + 1

    @property
    def height(self):
        return self.y1 - self.y0 + 1

    def channels(self):
        return sorted(self.planes.keys())

    def contains(self, x, y):
        return self.x0 <= x <= self.x1 and self.y0 <= y <= self.y1

    def at(self, channel, x, y):
        """Value at absolute pixel (x, y); 0.0 outside the data window."""
        if not self.contains(x, y):
            return 0.0
        plane = self.planes.get(channel)
        if plane is None:
            return 0.0
        return plane[(y - self.y0) * self.width + (x - self.x0)]

    def row(self, channel, y):
        """The whole scanline as a list, or an all-zero list if out of range."""
        if not (self.y0 <= y <= self.y1) or channel not in self.planes:
            return [0.0] * self.width
        base = (y - self.y0) * self.width
        return list(self.planes[channel][base:base + self.width])


def _readString0(buf, off):
    end = buf.index(b"\x00", off)
    return buf[off:end].decode("utf-8"), end + 1


def _readChannelList(payload):
    """Parse a chlist attribute. Returns [(name, pixelType, xSamp, ySamp)]."""
    out = []
    off = 0
    while off < len(payload) and payload[off:off + 1] != b"\x00":
        name, off = _readString0(payload, off)
        pixelType, = struct.unpack_from("<i", payload, off)
        off += 4
        off += 4                                   # pLinear + 3 reserved bytes
        xSamp, ySamp = struct.unpack_from("<ii", payload, off)
        off += 8
        out.append((name, pixelType, xSamp, ySamp))
    return out


def readExr(path):
    """Read an uncompressed 32-bit float scanline EXR into an ``Image``."""
    with open(path, "rb") as fh:
        buf = fh.read()

    magic, = struct.unpack_from("<I", buf, 0)
    if magic != EXR_MAGIC:
        raise ValueError("%s is not an OpenEXR file" % path)
    versionField, = struct.unpack_from("<I", buf, 4)
    flags = versionField >> 8
    if flags & 0x02:
        raise ValueError("%s is tiled; the harness only reads scanline EXRs" % path)
    if flags & 0x10:
        raise ValueError("%s is multi-part; unsupported" % path)

    off = 8
    attrs = {}
    while True:
        name, off = _readString0(buf, off)
        if name == "":
            break
        attrType, off = _readString0(buf, off)
        size, = struct.unpack_from("<i", buf, off)
        off += 4
        attrs[name] = (attrType, buf[off:off + size])
        off += size

    if "dataWindow" not in attrs or "channels" not in attrs:
        raise ValueError("%s has no dataWindow/channels attribute" % path)

    x0, y0, x1, y1 = struct.unpack_from("<iiii", attrs["dataWindow"][1], 0)
    if "displayWindow" in attrs:
        _dx0, _dy0, _dx1, dispY1 = struct.unpack_from(
            "<iiii", attrs["displayWindow"][1], 0)
    else:
        dispY1 = y1
    compression = attrs.get("compression", ("compression", b"\x00"))[1][0]
    if compression != 0:
        raise ValueError("%s uses compression %d; the harness writes "
                         "uncompressed EXRs only" % (path, compression))

    chans = _readChannelList(attrs["channels"][1])
    for name, pixelType, xSamp, ySamp in chans:
        if pixelType != PIXELTYPE_FLOAT:
            raise ValueError("%s channel %s is not 32-bit float" % (path, name))
        if xSamp != 1 or ySamp != 1:
            raise ValueError("%s channel %s is subsampled" % (path, name))

    width = x1 - x0 + 1
    height = y1 - y0 + 1
    rowBytes = width * 4

    planes = {}
    for name, _pt, _xs, _ys in chans:
        planes[name] = array.array("f", [0.0]) * (width * height)

    numBlocks = height                             # NO_COMPRESSION => 1 line/block
    offsets = struct.unpack_from("<%dQ" % numBlocks, buf, off)

    # EXR y grows downward, Nuke's grows upward: nukeY = displayWindow.ymax -
    # exrY. Flip on the way in so every coordinate the harness handles is a
    # Nuke coordinate.
    nukeY0 = dispY1 - y1
    nukeY1 = dispY1 - y0

    swap = sys.byteorder != "little"
    for blockOffset in offsets:
        y, dataSize = struct.unpack_from("<ii", buf, blockOffset)
        pos = blockOffset + 8
        if dataSize != rowBytes * len(chans):
            raise ValueError("%s: unexpected scanline size %d at y=%d"
                             % (path, dataSize, y))
        dst = ((dispY1 - y) - nukeY0) * width
        for name, _pt, _xs, _ys in chans:          # chlist order == data order
            samples = array.array("f")
            samples.frombytes(buf[pos:pos + rowBytes])
            if swap:
                samples.byteswap()
            planes[name][dst:dst + width] = samples
            pos += rowBytes

    return Image(x0, nukeY0, x1, nukeY1, planes)
