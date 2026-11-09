#!/usr/bin/env python3
"""Generates the JPEG decoder fixtures. Run from this directory; the outputs
are committed, so CI never runs this."""
import io
import struct
from PIL import Image

W, H = 64, 48

def base(mode="RGB"):
    img = Image.new("RGB", (W, H))
    px = img.load()
    for y in range(H):
        for x in range(W):
            px[x, y] = ((x * 4) % 256, (y * 5) % 256, ((x + y) * 3) % 256)
    return img.convert(mode)

def save(img, name, **kw):
    img.save(name, "JPEG", quality=90, **kw)

save(base(), "baseline.jpg")
save(base(), "progressive.jpg", progressive=True)
save(base("L"), "grayscale.jpg")
save(base("CMYK"), "cmyk_adobe.jpg")          # PIL writes an Adobe APP14 marker for CMYK
save(base(), "sub422.jpg", subsampling=1)
save(base(), "sub444.jpg", subsampling=0)
for o in range(1, 9):
    exif = Image.Exif()
    exif[0x0112] = o
    save(base(), f"orient_{o}.jpg", exif=exif.tobytes())
full = open("baseline.jpg", "rb").read()
open("truncated.jpg", "wb").write(full[: len(full) * 2 // 3])
buf = io.BytesIO()
base().save(buf, "PNG")
open("png_named.jpg", "wb").write(buf.getvalue())


# EXIF layouts that decide whether OpenCV applies the orientation. The TIFF
# blocks are built by hand so tag order, counts and offsets are exact.
ORIENT, MAKE, XRES, WHITE_POINT, CHROMATICITIES = 0x0112, 0x010F, 0x011A, 0x013E, 0x013F
SHORT, ASCII, RATIONAL = 3, 2, 5


def tiff(entries, bo=">", count=None, cut=None, pad=0):
    """IFD0 at offset 8. An entry's payload is bytes (inline when it fits in 4,
    else placed after the IFD) or an int written as a raw offset."""
    head = (b"MM" if bo == ">" else b"II") + struct.pack(bo + "HI", 42, 8)
    data_start = 8 + 2 + 12 * len(entries) + 4
    body, data = b"", b""
    for tag, typ, n, payload in entries:
        if isinstance(payload, int):
            value = struct.pack(bo + "I", payload)
        elif len(payload) <= 4:
            value = payload.ljust(4, b"\0")
        else:
            value = struct.pack(bo + "I", data_start + len(data))
            data += payload
        body += struct.pack(bo + "HHI", tag, typ, n) + value
    count = len(entries) if count is None else count
    out = head + struct.pack(bo + "H", count) + body + b"\0\0\0\0" + data + b"\0" * pad
    return out if cut is None else out[:cut]


def orient(o, bo=">"):
    return (ORIENT, SHORT, 1, struct.pack(bo + "H", o))


def rationals(n, bo=">"):
    return b"".join(struct.pack(bo + "II", 72, 1) for _ in range(n))


def exif(t):
    return b"Exif\0\0" + t


def with_app1(name, payloads):
    """baseline.jpg with APP1 segments inserted after its JFIF APP0."""
    jpg = open("baseline.jpg", "rb").read()
    assert jpg[2:4] == b"\xff\xe0"
    at = 4 + struct.unpack(">H", jpg[4:6])[0]
    segs = b"".join(b"\xff\xe1" + struct.pack(">H", len(p) + 2) + p for p in payloads)
    open(name, "wb").write(jpg[:at] + segs + jpg[at:])


XMP = b"http://ns.adobe.com/xap/1.0/\0<x:xmpmeta xmlns:x='adobe:ns:meta/'/>"

# OpenCV 5 reads the Exif marker after the XMP one; OpenCV 4 reads only the XMP.
with_app1("exif_after_xmp.jpg", [XMP, exif(tiff([orient(6)]))])
# Little-endian TIFF.
with_app1("exif_intel.jpg", [exif(tiff([orient(6, bo="<")], bo="<"))])
# Valid string, rational and rational-array tags ahead of the orientation.
with_app1("exif_tags_before.jpg", [exif(tiff([
    (MAKE, ASCII, 6, b"Canon\0"),
    (XRES, RATIONAL, 1, rationals(1)),
    (WHITE_POINT, RATIONAL, 2, rationals(2)),
    (CHROMATICITIES, RATIONAL, 6, rationals(6)),
    orient(6),
]))])
# Each tag ahead of the orientation reads out of range, so OpenCV stops there.
with_app1("exif_make_out_of_range.jpg", [exif(tiff([(MAKE, ASCII, 6, 0x7000), orient(6)]))])
with_app1("exif_xres_out_of_range.jpg", [exif(tiff([(XRES, RATIONAL, 1, 0x7000), orient(6)]))])
# The second white-point rational is the first out of range.
t = tiff([(WHITE_POINT, RATIONAL, 2, 0), orient(6)])
with_app1("exif_white_point_out_of_range.jpg",
          [exif(tiff([(WHITE_POINT, RATIONAL, 2, len(t) - 8), orient(6)]))])
# The sixth chromaticity rational is the first out of range.
t = tiff([(CHROMATICITIES, RATIONAL, 6, 0), orient(6)], pad=64)
with_app1("exif_chromaticities_out_of_range.jpg",
          [exif(tiff([(CHROMATICITIES, RATIONAL, 6, len(t) - 40), orient(6)], pad=64))])
# The IFD claims more entries than it holds; the orientation before the cut stays.
with_app1("exif_count_overrun.jpg", [exif(tiff([orient(6)], count=4, cut=22))])
# Two Exif markers: the first orientation wins.
with_app1("exif_two_markers.jpg", [exif(tiff([orient(3)])), exif(tiff([orient(6)]))])
# Only the second Exif marker has an orientation: OpenCV 5 applies it, 4 does not.
with_app1("exif_second_marker_orients.jpg",
          [exif(tiff([(MAKE, ASCII, 6, b"Canon\0")])), exif(tiff([orient(6)]))])
