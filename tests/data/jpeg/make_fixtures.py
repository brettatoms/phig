#!/usr/bin/env python3
"""Generates the JPEG decoder fixtures. Run from this directory; the outputs
are committed, so CI never runs this."""
import io
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
save(base(), "sub420.jpg", subsampling=2)
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
