#!/usr/bin/env python3
"""Fixtures ImageMagick cannot write: JPEG files with an EXIF Orientation tag, and a GIF whose
frames use local color tables. Called by make_fixtures.sh. Standard library only; nothing is
downloaded.

  make_fixtures_extra.py exif <in.jpg> <out.jpg> <orientation 1-8>
      Inserts an APP1 Exif segment right after SOI. Odd orientations use little-endian TIFF
      ("II"), even ones big-endian ("MM"), so both byte orders are covered. The pixels are
      unchanged; the tag tells the decoder how to rotate or flip them.

  make_fixtures_extra.py lct <out.gif>
      A 16x16 GIF89a with a global color table of 2 colors and two frames, each with its own
      local color table of 4 colors. Frame pixels are LZW-coded with a CLEAR before every pixel,
      so the code width stays at 3 bits and no dictionary growth is needed.
"""

import struct
import sys


def exif_segment(orientation: int) -> bytes:
    le = orientation % 2 == 1
    bo = "<" if le else ">"
    tiff = (b"II" if le else b"MM") + struct.pack(bo + "HI", 42, 8)
    # One IFD entry: tag 0x0112 (Orientation), type 3 (SHORT), count 1, value in the first 2 bytes.
    entry = struct.pack(bo + "HHI", 0x0112, 3, 1) + struct.pack(bo + "HH", orientation, 0)
    tiff += struct.pack(bo + "H", 1) + entry + struct.pack(bo + "I", 0)
    payload = b"Exif\x00\x00" + tiff
    return b"\xff\xe1" + struct.pack(">H", len(payload) + 2) + payload


def write_exif(src: str, dst: str, orientation: int) -> None:
    if not 1 <= orientation <= 8:
        raise SystemExit("orientation must be 1-8")
    data = open(src, "rb").read()
    if data[:2] != b"\xff\xd8":
        raise SystemExit("not a JPEG")
    out = data[:2] + exif_segment(orientation) + data[2:]
    open(dst, "wb").write(out)


def lzw_clear_each_pixel(indices, min_code: int = 2) -> bytes:
    """LZW-codes the indices with a CLEAR before every pixel (code width stays min_code + 1)."""
    clear = 1 << min_code
    eoi = clear + 1
    width = min_code + 1
    codes = []
    for p in indices:
        codes.append(clear)
        codes.append(p)
    codes.append(eoi)
    acc = 0
    nbits = 0
    out = bytearray()
    for c in codes:
        acc |= c << nbits
        nbits += width
        while nbits >= 8:
            out.append(acc & 0xFF)
            acc >>= 8
            nbits -= 8
    if nbits:
        out.append(acc & 0xFF)
    return bytes(out)


def sub_blocks(data: bytes) -> bytes:
    out = bytearray()
    for i in range(0, len(data), 255):
        chunk = data[i : i + 255]
        out.append(len(chunk))
        out += chunk
    out.append(0)
    return bytes(out)


def write_lct_gif(dst: str) -> None:
    w = h = 16
    gct = bytes([200, 10, 10, 10, 200, 10])  # global: red, green (unused by the frames)
    out = bytearray(b"GIF89a")
    out += struct.pack("<HHBBB", w, h, 0x80 | 0x00, 0, 0)  # GCT present, 2 entries
    out += gct
    frames = [
        # Local palettes: four colors each; the indices cover all four entries.
        (bytes([200, 10, 10, 10, 200, 10, 10, 10, 200, 240, 240, 20]), 10),
        (bytes([10, 10, 200, 240, 240, 20, 200, 10, 200, 20, 20, 20]), 20),
    ]
    for palette, delay in frames:
        # Graphic Control Extension: delay in hundredths, no transparency.
        out += b"\x21\xf9\x04\x00" + struct.pack("<H", delay) + b"\x00\x00"
        out += b"\x2c" + struct.pack("<HHHHB", 0, 0, w, h, 0x80 | 0x01)  # LCT, 4 entries
        out += palette
        indices = [((x // 4) + (y // 4)) % 4 for y in range(h) for x in range(w)]
        out += b"\x02"  # LZW minimum code size
        out += sub_blocks(lzw_clear_each_pixel(indices, 2))
    out += b"\x3b"
    open(dst, "wb").write(out)


def main(argv):
    if len(argv) == 5 and argv[1] == "exif":
        write_exif(argv[2], argv[3], int(argv[4]))
    elif len(argv) == 3 and argv[1] == "lct":
        write_lct_gif(argv[2])
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
