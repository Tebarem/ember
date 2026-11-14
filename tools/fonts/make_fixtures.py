#!/usr/bin/env python3
"""Writes the font fixtures of the system font tests (W4i), with no third-party packages.

Usage: python tools/fonts/make_fixtures.py   (from anywhere; it writes under the source root)

Outputs (deterministic: fixed timestamps, no randomness):
  tests/fonts/sysfonts/        a directory tree the system font index scans (recursion, .ttc, .otf, .woff, an ignored
                               .fon file and a .txt file that must not be indexed)
  tests/fonts/bitmap/          CBDT/CBLC and sbix bitmap colour fonts for tests/unit/font/test_font_bitmap*.c

Every font is a small TrueType font (glyf outlines: one rectangle per glyph) with the tables the parsers read. The
values the tests expect are written in the comments below and in the tests, and the layout is documented by the
functions that build each table.
"""

import os
import struct
import zlib

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SYS = os.path.join(ROOT, "tests", "fonts", "sysfonts")
BMP = os.path.join(ROOT, "tests", "fonts", "bitmap")

UPEM = 1000
ADVANCE = 600


def u16(v):
    return struct.pack(">H", v & 0xFFFF)


def i16(v):
    return struct.pack(">h", v)


def u32(v):
    return struct.pack(">I", v & 0xFFFFFFFF)


def pad4(b):
    return b + b"\x00" * ((4 - len(b) % 4) % 4)


def name_table(records):
    """records: (platform, encoding, language, name_id, text). Format 0; the strings are UTF-16BE for platform 3 and
    ASCII for platform 1."""
    strings = b""
    recs = b""
    for plat, enc, lang, nid, text in records:
        data = text.encode("utf-16-be") if plat == 3 else text.encode("ascii")
        recs += struct.pack(">HHHHHH", plat, enc, lang, nid, len(data), len(strings))
        strings += data
    header = struct.pack(">HHH", 0, len(records), 6 + 12 * len(records))
    return header + recs + strings


def os2_table(weight, width, fs_selection):
    """Version 4, 96 bytes: usWeightClass at 4, usWidthClass at 6, fsSelection at 62 (the rest zero)."""
    t = bytearray(96)
    struct.pack_into(">H", t, 0, 4)
    struct.pack_into(">H", t, 4, weight)
    struct.pack_into(">H", t, 6, width)
    struct.pack_into(">H", t, 62, fs_selection)
    return bytes(t)


def post_table():
    """Version 3.0, 32 bytes: italic angle 0."""
    return struct.pack(">I", 0x00030000) + b"\x00" * 28


def head_table(index_to_loc_long):
    t = bytearray(54)
    struct.pack_into(">I", t, 0, 0x00010000)
    struct.pack_into(">I", t, 12, 0x5F0F3CF5)
    struct.pack_into(">H", t, 18, UPEM)
    struct.pack_into(">hhhh", t, 36, 0, -200, ADVANCE, 800)
    struct.pack_into(">h", t, 50, 1 if index_to_loc_long else 0)
    return bytes(t)


def hhea_table(num_hmetrics):
    t = bytearray(36)
    struct.pack_into(">I", t, 0, 0x00010000)
    struct.pack_into(">H", t, 34, num_hmetrics)
    return bytes(t)


def maxp_table(num_glyphs):
    t = bytearray(32)
    struct.pack_into(">I", t, 0, 0x00010000)
    struct.pack_into(">H", t, 4, num_glyphs)
    return bytes(t)


def glyf_and_loca(boxes):
    """boxes: per glyph, None (empty) or (x0, y0, x1, y1). Simple glyphs of four on-curve points, long loca."""
    glyf = b""
    offsets = [0]
    for box in boxes:
        if box is not None:
            x0, y0, x1, y1 = box
            pts = [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]
            body = struct.pack(">hhhhh", 1, x0, y0, x1, y1)
            body += u16(3) + u16(0)  # endPtsOfContours[0] = 3, instructionLength = 0
            body += bytes([0x01]) * 4  # flags: on curve, two-byte deltas
            prev = (0, 0)
            xs = b""
            for x, _ in pts:
                xs += i16(x - prev[0])
                prev = (x, prev[1])
            ys = b""
            for _, y in pts:
                ys += i16(y - prev[1])
                prev = (prev[0], y)
            body += xs + ys
            glyf += pad4(body)
        offsets.append(len(glyf))
    loca = b"".join(u32(o) for o in offsets)
    return glyf, loca


def cmap_format4(mapping):
    """mapping: code point -> glyph id, all in the BMP. Segments of consecutive code points with consecutive ids."""
    cps = sorted(mapping)
    segs = []
    for cp in cps:
        gid = mapping[cp]
        if segs and cp == segs[-1][1] + 1 and gid - cp == segs[-1][2]:
            segs[-1][1] = cp
        else:
            segs.append([cp, cp, gid - cp])
    segs.append([0xFFFF, 0xFFFF, 1])
    n = len(segs)
    end = b"".join(u16(s[1]) for s in segs)
    start = b"".join(u16(s[0]) for s in segs)
    delta = b"".join(u16(s[2]) for s in segs)
    rng = b"".join(u16(0) for _ in segs)
    length = 14 + 2 * n + 2 + 2 * n + 2 * n + 2 * n
    # Format 4 header: format, length, language, segCountX2, searchRange, entrySelector, rangeShift (7 u16).
    sub = struct.pack(">HHHHHHH", 4, length, 0, 2 * n, 0, 0, 0) + end + u16(0) + start + delta + rng
    header = struct.pack(">HH", 0, 1) + struct.pack(">HHI", 3, 1, 12)
    return header + sub


def search_params(n):
    """searchRange, entrySelector and rangeShift of a table directory with n tables."""
    p = 1
    e = 0
    while p * 2 <= n:
        p *= 2
        e += 1
    return p * 16, e, n * 16 - p * 16


def checksum(padded):
    """The sum of the table's big-endian 32-bit words (the checksum of the table directory entry)."""
    return sum(struct.unpack(">%dI" % (len(padded) // 4), padded)) & 0xFFFFFFFF


def sfnt(tables, tag=0x00010000):
    """Assembles the tables (dict tag -> bytes) into one sfnt, with its table directory. Returns the bytes."""
    names = sorted(tables)
    n = len(names)
    search_range, entry, shift = search_params(n)
    head = struct.pack(">IHHHH", tag, n, search_range, entry, shift)
    offset = 12 + 16 * n
    directory = b""
    body = b""
    for name in names:
        data = tables[name]
        padded = pad4(data)
        directory += name.encode("ascii") + u32(checksum(padded)) + u32(offset) + u32(len(data))
        body += padded
        offset += len(padded)
    return head + directory + body


def font_tables(family, weight, width, fs, names_extra, mapping, boxes, extra=None, long_loca=True, sub="Regular"):
    """The tables of a TrueType font. names_extra: list of (platform, enc, lang, id, text) besides the family names.
    Name IDs: 1 family, 4 full name 'family sub', 6 PostScript name 'Family-Sub' (no spaces)."""
    records = [(3, 1, 0x0409, 1, family), (3, 1, 0x0409, 4, family + " " + sub),
               (3, 1, 0x0409, 6, family.replace(" ", "") + "-" + sub)] + list(names_extra)
    glyf, loca = glyf_and_loca(boxes)
    tables = {
        "head": head_table(long_loca),
        "hhea": hhea_table(len(boxes)),
        "maxp": maxp_table(len(boxes)),
        "OS/2": os2_table(weight, width, fs),
        "post": post_table(),
        "name": name_table(records),
        "cmap": cmap_format4(mapping),
        "glyf": glyf,
        "loca": loca,
        "hmtx": b"".join(u16(ADVANCE) + i16(0) for _ in boxes),
    }
    if extra:
        tables.update(extra)
    return tables


def box(i):
    """A rectangle glyph, distinct per glyph id (so that the tests can tell glyphs apart)."""
    w = 100 + 10 * i
    return (50, 0, 50 + w, 700)


def ascii_mapping(first_gid=1):
    """ASCII 0x20..0x7E to glyph ids first_gid.. (the glyph ids of the outlines list)."""
    return {cp: first_gid + (cp - 0x20) for cp in range(0x20, 0x7F)}


def ascii_boxes():
    return [None] + [box(i) for i in range(1, 96)]


def png(width, height, rgba):
    """A PNG of one colour, RGBA8, filter 0."""
    raw = b""
    row = bytes(rgba) * width
    for _ in range(height):
        raw += b"\x00" + row

    def chunk(kind, data):
        return u32(len(data)) + kind + data + u32(zlib.crc32(kind + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def ttc(faces):
    """faces: list of table dicts. A TTC (version 1.0) whose table directories and offsets are absolute."""
    header_len = 12 + 4 * len(faces)
    blobs = []
    offset = header_len
    dirs = []
    for tables in faces:
        names = sorted(tables)
        n = len(names)
        search_range, entry, shift = search_params(n)
        d = struct.pack(">IHHHH", 0x00010000, n, search_range, entry, shift)
        table_off = offset + 12 + 16 * n
        recs = b""
        body = b""
        for name in names:
            data = tables[name]
            recs += name.encode("ascii") + u32(0) + u32(table_off) + u32(len(data))
            padded = pad4(data)
            body += padded
            table_off += len(padded)
        dirs.append(d + recs)
        blobs.append(body)
        offset += 12 + 16 * n + len(body)
    out = b"ttcf" + u32(0x00010000) + u32(len(faces))
    face_offsets = []
    pos = header_len
    for dr, body in zip(dirs, blobs):
        face_offsets.append(pos)
        pos += len(dr) + len(body)
    for fo in face_offsets:
        out += u32(fo)
    for dr, body in zip(dirs, blobs):
        out += dr + body
    return out


def woff1(sfnt_bytes):
    """A WOFF 1.0 file whose tables are stored uncompressed (compLength == origLength)."""
    num = struct.unpack(">H", sfnt_bytes[4:6])[0]
    flavor = struct.unpack(">I", sfnt_bytes[0:4])[0]
    entries = []
    for i in range(num):
        tag, _, off, length = struct.unpack(">4sIII", sfnt_bytes[12 + 16 * i:28 + 16 * i])
        entries.append((tag, off, length))
    entries.sort()
    header_len = 44 + 20 * num
    body = b""
    dirs = b""
    pos = header_len
    for tag, off, length in entries:
        data = sfnt_bytes[off:off + length]
        dirs += tag + u32(pos) + u32(length) + u32(length) + u32(0)
        body += pad4(data)
        pos += len(pad4(data))
    total = header_len + len(body)
    hdr = b"wOFF" + u32(flavor) + u32(total) + u16(num) + u16(0) + u32(len(sfnt_bytes)) + u16(1) + u16(0)
    hdr += u32(0) + u32(0) + u32(0) + u32(0) + u32(0)
    return hdr + dirs + body


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def main():
    # ---- the system font tree ------------------------------------------------------------------------------------
    # Ember Sans, regular and bold: ASCII outlines, names as the test expects (see test_text_sysfont_index.c).
    sans_regular = sfnt(font_tables("Ember Sans", 400, 5, 0x0040, [], ascii_mapping(), ascii_boxes()))
    sans_bold = sfnt(font_tables("Ember Sans", 700, 5, 0x0020, [], ascii_mapping(), ascii_boxes(), sub="Bold"))
    write(os.path.join(SYS, "sans", "EmberSans-Regular.ttf"), sans_regular)
    write(os.path.join(SYS, "sans", "EmberSans-Bold.ttf"), sans_bold)

    # Ember Serif Italic, in a nested directory: typographic family 'Ember Serif Display' (ID 16), a Japanese
    # localized family name (ID 1, language 0x0411) and fsSelection italic. PostScript name EmberSerif-Italic.
    ja_name = "エンバーセリフ"  # エンバーセリフ
    serif_names = [(3, 1, 0x0411, 1, ja_name), (3, 1, 0x0409, 16, "Ember Serif Display")]
    serif = sfnt(font_tables("Ember Serif", 400, 5, 0x0001, serif_names, ascii_mapping(), ascii_boxes(), sub="Italic"))
    write(os.path.join(SYS, "deep", "er", "EmberSerif-Italic.ttf"), serif)

    # A TrueType outline in a .otf file name, covering four CJK ideographs only (U+4E00..U+4E03).
    cjk_map = {0x4E00 + i: 1 + i for i in range(4)}
    cjk_boxes = [None] + [box(i) for i in range(1, 5)]
    cjk = sfnt(font_tables("Ember CJK", 400, 5, 0x0040, [], cjk_map, cjk_boxes))
    write(os.path.join(SYS, "cjk", "EmberCJK.otf"), cjk)

    # Fonts named like the families the per-script preference tables of sysfont.c list, so that per-language Han
    # fallback and per-script fallback can be tested: the four CJK faces cover U+4E00..U+4E03 (and nothing else), the
    # Arabic face covers U+0627..U+062A. The tests compare family names.
    for fam in ("Noto Sans CJK JP", "Noto Sans CJK SC", "Noto Sans CJK TC", "Noto Sans CJK KR"):
        d = sfnt(font_tables(fam, 400, 5, 0x0040, [], cjk_map, cjk_boxes))
        write(os.path.join(SYS, "script", fam.replace(" ", "") + ".ttf"), d)
    ar_map = {0x0627 + i: 1 + i for i in range(4)}
    ar_boxes = [None] + [box(i) for i in range(1, 5)]
    write(os.path.join(SYS, "script", "NotoSansArabic.ttf"),
          sfnt(font_tables("Noto Sans Arabic", 400, 5, 0x0040, [], ar_map, ar_boxes)))

    # A TTC with two faces: Ember Collection A (400) and B (700).
    coll_a = font_tables("Ember Collection A", 400, 5, 0x0040, [], ascii_mapping(), ascii_boxes())
    coll_b = font_tables("Ember Collection B", 700, 5, 0x0020, [], ascii_mapping(), ascii_boxes(), sub="Bold")
    write(os.path.join(SYS, "collection.ttc"), ttc([coll_a, coll_b]))

    # A WOFF of an Ember Woff face (uncompressed tables), in a subdirectory.
    woff_src = sfnt(font_tables("Ember Woff", 400, 5, 0x0040, [], ascii_mapping(), ascii_boxes()))
    write(os.path.join(SYS, "woff", "EmberWoff.woff"), woff1(woff_src))

    # Files the index must skip: a Windows bitmap font (.fon) and a text file.
    write(os.path.join(SYS, "ignored", "EmberIgnored.fon"), b"MZ" + b"\x00" * 256)
    write(os.path.join(SYS, "ignored", "readme.txt"), b"Not a font.\n")

    # ---- the bitmap fonts ------------------------------------------------------------------------------------------
    write(os.path.join(BMP, "EmberBitmap-cbdt.ttf"), cbdt_font())
    write(os.path.join(BMP, "EmberBitmap-sbix.ttf"), sbix_font())
    write(os.path.join(BMP, "EmberBitmap-colr.ttf"), cbdt_font(with_colr=True))


def cbdt_font(with_colr=False):
    """CBLC/CBDT with three strikes for glyph 1 ('A'), and an empty glyph 2 ('B', with an outline so the face opens).

    Strike 16 ppem: index format 1, image format 17 (16x16 red PNG, small metrics: bearing 0,14, advance 16).
    Strike 32 ppem: index format 3 (16-bit offsets), image format 18 (32x32 green PNG, big metrics: bearing 0,28).
    Strike 64 ppem: index format 2 (one image size, the big metrics live in the subtable), image format 19 (64x64
    yellow PNG, metrics from the subtable: bearing 1,56, advance 64). All strikes map glyph 1 only."""
    png16 = png(16, 16, (255, 0, 0, 255))
    png32 = png(32, 32, (0, 255, 0, 255))

    # Strike 16: data region starts at CBDT offset 4 (after the version); one glyph.
    rec17 = struct.pack(">BBbbB", 16, 16, 0, 14, 16) + u32(len(png16)) + png16
    data16 = rec17
    # Index subtable (format 1): header (indexFormat 1, imageFormat 17, imageDataOffset 4), offsets for glyph 1..2.
    sub16 = u16(1) + u16(17) + u32(4) + u32(0) + u32(len(data16))

    # Strike 32: format 18 record (bigGlyphMetrics: height, width, bearings, advance, vertical 3 bytes)
    rec18 = struct.pack(">BBbbBbbB", 32, 32, 0, 28, 32, 0, 0, 0) + u32(len(png32)) + png32
    data32 = rec18
    # Strike 64: format 19 record (only the PNG length and data; the metrics are in the index subtable, format 2).
    png64 = png(64, 64, (255, 255, 0, 255))
    rec19 = u32(len(png64)) + png64
    data64 = rec19
    # Index subtable (format 3): header, u16 offsets for glyph 1..2 (0, len).
    cbdt = u32(0x00030000) + data16 + data32 + data64  # the data32 record starts at 4 + len(data16)
    # The strike 32 data is addressed through its own imageDataOffset: 4 + len(data16).
    sub32 = u16(3) + u16(18) + u32(4 + len(data16)) + u16(0) + u16(len(data32))
    # Index subtable (format 2): header, imageSize, bigMetrics (height, width, horiBearingX/Y, horiAdvance, vert x3).
    sub64 = (u16(2) + u16(19) + u32(4 + len(data16) + len(data32)) + u32(len(data64)) +
             struct.pack(">BBbbBbbB", 64, 64, 1, 56, 64, 0, 0, 0))

    # CBLC: header (version 0x00020000, 2 strikes), two BitmapSize records, then the index arrays.
    nsizes = 3
    arr_base = 8 + 48 * nsizes  # the index subtable arrays start after the records
    # Each strike has one index subtable entry: first 1, last 1, offset to its subtable from the array start.
    arr16 = u16(1) + u16(1) + u32(8)  # entry: sub at arr16_base + 8
    arr32 = u16(1) + u16(1) + u32(8)
    # Layout: [header][size16 record][size32 record][arr16 entry][sub16][arr32 entry][sub32]
    off_arr16 = arr_base
    off_sub16 = off_arr16 + 8
    off_arr32 = off_sub16 + len(sub16)
    off_sub32 = off_arr32 + 8
    off_arr64 = off_sub32 + len(sub32)

    def size_rec(arr_off, sub_len, ppem):
        # indexSubTableArrayOffset, indexTablesSize, numberOfIndexSubTables, colorRef, hori, vert, start, end,
        # ppemX, ppemY, bitDepth, flags.
        sbit = bytes(12)
        return (u32(arr_off) + u32(8 + sub_len) + u32(1) + u32(0) + sbit + sbit +
                u16(1) + u16(1) + bytes([ppem, ppem, 32, 1]))

    cblc_head = u32(0x00020000) + u32(nsizes)
    cblc = cblc_head + size_rec(off_arr16, len(sub16), 16) + size_rec(off_arr32, len(sub32), 32) +         size_rec(off_arr64, len(sub64), 64)
    # The array entry's offset is relative to the array start; the subtable follows the entry.
    cblc += u16(1) + u16(1) + u32(8) + sub16
    cblc += u16(1) + u16(1) + u32(8) + sub32
    cblc += u16(1) + u16(1) + u32(8) + sub64

    # Glyph outlines: 0 empty, 1 'A' a box, 2 'B' a box (the only mapping is 'A' -> 1).
    mapping = {ord("A"): 1, ord("B"): 2}
    boxes = [None, box(1), box(2)]
    tables = font_tables("Ember Bitmap", 400, 5, 0x0040, [], mapping, boxes)
    tables["CBLC"] = cblc
    tables["CBDT"] = cbdt
    if with_colr:
        # COLR version 0: glyph 1 ('A') is drawn by one layer, glyph 2, in palette entry 0 (opaque magenta). The face
        # also has CBDT strikes: COLR must win over the bitmaps.
        colr = (u16(0) + u16(1) + u32(14) + u32(20) + u16(1) +  # header: version, base records, offsets, layers
                u16(1) + u16(0) + u16(1) +                      # base glyph record: gid 1, first layer 0, 1 layer
                u16(2) + u16(0))                                # layer record: gid 2, palette entry 0
        cpal = (u16(0) + u16(1) + u16(1) + u16(1) + u32(14) +   # header: version, entries, palettes, colors, offset
                u16(0) +                                        # palette 0 starts at color record 0
                bytes([255, 0, 255, 255]))                      # BGRA
        tables["COLR"] = colr
        tables["CPAL"] = cpal
    return sfnt(tables)


def sbix_font():
    """sbix with one strike (ppem 16) and a PNG for glyph 1 only (glyph 0 and 2 empty)."""
    png16 = png(16, 16, (0, 0, 255, 255))
    numglyphs = 3
    # Glyph data: origin X/Y (bottom-left, relative to the baseline), graphic type 'png ', then the PNG.
    g1 = struct.pack(">hh", 2, -3) + b"png " + png16
    offsets = [0, 0, len(g1), len(g1)]  # glyph 0: empty, glyph 1: g1, glyph 2: empty
    strike_hdr = u16(16) + u16(72)
    # The strike header is 4 bytes and the offset array 4 * (numGlyphs + 1) = 16, so glyph data starts at 20.
    strike = strike_hdr + b"".join(u32(20 + o) for o in offsets)  # offsets relative to the strike start
    sbix = u16(1) + u16(1) + u32(1) + u32(12) + strike + g1
    mapping = {ord("A"): 1}
    boxes = [None, box(1), None]
    tables = font_tables("Ember Sbix", 400, 5, 0x0040, [], mapping, boxes)
    tables["sbix"] = sbix
    return sfnt(tables)


if __name__ == "__main__":
    main()
