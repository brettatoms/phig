#!/usr/bin/env python3
"""Regenerates tests/data/live_photo_still.jpg.

A committable stand-in for the still half of a Live Photo. Real Live Photos are
personal media and must not be fixtures, but the pairing code only ever tests
whether a still's MakerNote *contains* a video's Content Identifier — it never
parses Apple's MakerNote format, because libexif cannot. So a JPEG carrying any
MakerNote whose bytes include the identifier exercises every code path a real
Apple still would, and carries nothing personal.

The identifier matches tests/data/live_photo.mov, so the two pair.

Writes a minimal EXIF APP1 by hand rather than using a library: the payload is
four fields, and depending on libexif to write a MakerNote it cannot itself
parse would be a strange foundation for a test.

    python3 docs/plans/2026-08-09-038-make-still-fixture.py
"""

import pathlib
import struct
import sys

IDENTIFIER = b"AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE"
# A second identifier, and a still carrying both. One still claiming several
# videos must be refused just as firmly as several stills claiming one, and a
# fixture carrying a single identifier cannot express that case: copying one
# video twice gives both copies the *same* identifier, which an unrelated rule
# already rejects, so the test would pass without exercising anything.
IDENTIFIER_B = b"11111111-2222-3333-4444-555555555555"

TAG_EXIF_IFD_POINTER = 0x8769
TAG_MAKER_NOTE = 0x927C
TYPE_LONG = 4
TYPE_UNDEFINED = 7


def build_exif_app1(identifier: bytes) -> bytes:
    """An APP1 segment holding one MakerNote tag containing `identifier`.

    Layout, little-endian, offsets measured from the start of the TIFF header:

        0   TIFF header            "II*\\0", IFD0 at offset 8
        8   IFD0                   1 entry -> Exif IFD pointer
        26  Exif IFD               1 entry -> MakerNote
        44  MakerNote payload      the identifier bytes
    """
    tiff = b"II*\x00" + struct.pack("<I", 8)

    ifd0_size = 2 + 12 + 4  # count + one entry + next-IFD offset
    exif_ifd_offset = 8 + ifd0_size
    exif_ifd_size = 2 + 12 + 4
    maker_note_offset = exif_ifd_offset + exif_ifd_size

    ifd0 = struct.pack("<H", 1)
    ifd0 += struct.pack("<HHII", TAG_EXIF_IFD_POINTER, TYPE_LONG, 1, exif_ifd_offset)
    ifd0 += struct.pack("<I", 0)  # no IFD1

    exif_ifd = struct.pack("<H", 1)
    exif_ifd += struct.pack("<HHII", TAG_MAKER_NOTE, TYPE_UNDEFINED,
                            len(identifier), maker_note_offset)
    exif_ifd += struct.pack("<I", 0)

    payload = b"Exif\x00\x00" + tiff + ifd0 + exif_ifd + identifier
    # Segment length covers the length field itself but not the marker.
    return b"\xff\xe1" + struct.pack(">H", len(payload) + 2) + payload


def insert_app1(jpeg: bytes, app1: bytes) -> bytes:
    if jpeg[:2] != b"\xff\xd8":
        raise SystemExit("source is not a JPEG (no SOI marker)")
    # Straight after SOI, before any existing APP segment. libexif reads the
    # first APP1 it finds, so this takes precedence over whatever was there.
    return jpeg[:2] + app1 + jpeg[2:]


def main() -> None:
    root = pathlib.Path(__file__).resolve().parents[2]
    src = root / "tests" / "data" / "orient_1.jpg"
    dst = root / "tests" / "data" / "live_photo_still.jpg"
    if not src.exists():
        raise SystemExit(f"missing source fixture: {src}")

    raw = src.read_bytes()

    out = insert_app1(raw, build_exif_app1(IDENTIFIER))
    dst.write_bytes(out)
    if IDENTIFIER not in out:
        raise SystemExit("identifier missing from output — generator is broken")
    print(f"wrote {dst} ({len(out)} bytes), identifier {IDENTIFIER.decode()}")

    dst2 = root / "tests" / "data" / "live_photo_still_two_ids.jpg"
    both = IDENTIFIER + b"\x00" + IDENTIFIER_B
    out2 = insert_app1(raw, build_exif_app1(both))
    dst2.write_bytes(out2)
    if IDENTIFIER not in out2 or IDENTIFIER_B not in out2:
        raise SystemExit("two-identifier fixture is missing one of them")
    print(f"wrote {dst2} ({len(out2)} bytes), identifiers "
          f"{IDENTIFIER.decode()} + {IDENTIFIER_B.decode()}")


if __name__ == "__main__":
    sys.exit(main())
