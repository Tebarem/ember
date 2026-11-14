"""Generates the DEFLATE test vectors in this directory.

Run from anywhere:  python gen_vectors.py

For each case <name> it writes:
  <name>.zlib  zlib-wrapped stream (RFC 1950), as zlib.compress would produce it
  <name>.raw   raw DEFLATE stream (RFC 1951)
  <name>.out   the uncompressed bytes both streams must decode to

Cases:
  stored        level 0: stored blocks only
  fixed         Z_FIXED strategy: fixed Huffman blocks only
  dynamic       default level on 96 KiB of text: dynamic Huffman blocks, long matches
  huffman_only  Z_HUFFMAN_ONLY: dynamic blocks with no matches (all-zero distance code)
  rle           Z_RLE: run-length matches at distance 1
  empty         no input
  maxdist       hand-built: a stored block of 32768 random bytes, then a fixed block with a
                258-byte match at distance 32768 (the largest distance) and a distance-1 overlap
  tiny          a 300-byte text, compressed dynamically (used for truncation and bit-flip tests)

The random data uses a fixed-seed generator so the vectors are reproducible. Python's zlib is the
reference encoder; `maxdist` is written by the bit writer below because no encoder emits that
distance on purpose.
"""

import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))


class Lcg:
    """Fixed-seed 32-bit LCG (Numerical Recipes constants)."""

    def __init__(self, seed):
        self.state = seed & 0xFFFFFFFF

    def next(self):
        self.state = (self.state * 1664525 + 1013904223) & 0xFFFFFFFF
        return self.state >> 8


WORDS = [
    "the", "of", "and", "to", "in", "is", "that", "for", "font", "glyph", "outline",
    "contour", "point", "table", "offset", "length", "checksum", "stream", "block",
    "distance", "literal", "match", "window", "huffman", "code", "symbol", "tree",
]


def text_bytes(n, seed):
    rng = Lcg(seed)
    out = bytearray()
    while len(out) < n:
        out += WORDS[rng.next() % len(WORDS)].encode()
        out += b" " if rng.next() % 9 else b".\n"
    return bytes(out[:n])


def random_bytes(n, seed):
    rng = Lcg(seed)
    return bytes((rng.next() >> 3) & 0xFF for _ in range(n))


def compress(data, level, strategy=zlib.Z_DEFAULT_STRATEGY):
    """Returns (zlib_stream, raw_stream)."""
    z = zlib.compressobj(level, zlib.DEFLATED, -15, 8, strategy)  # raw DEFLATE
    raw = z.compress(data) + z.flush()
    z2 = zlib.compressobj(level, zlib.DEFLATED, 15, 8, strategy)
    zs = z2.compress(data) + z2.flush()
    assert zs[2:-4] == raw, "zlib body mismatch"
    return zs, raw


class BitWriter:
    """LSB-first bit writer. Huffman codes go in MSB-first (see huff())."""

    def __init__(self):
        self.out = bytearray()
        self.acc = 0
        self.n = 0

    def bits(self, value, count):
        for i in range(count):
            self.acc |= ((value >> i) & 1) << self.n
            self.n += 1
            if self.n == 8:
                self.out.append(self.acc)
                self.acc = 0
                self.n = 0

    def huff(self, code, length):
        for i in range(length - 1, -1, -1):
            self.bits((code >> i) & 1, 1)

    def getvalue(self):
        if self.n:
            self.out.append(self.acc)
            self.acc = 0
            self.n = 0
        return bytes(self.out)


def fixed_literal(w, sym):
    if sym < 144:
        w.huff(0x30 + sym, 8)
    elif sym < 256:
        w.huff(0x190 + sym - 144, 9)
    elif sym < 280:
        w.huff(sym - 256, 7)
    else:
        w.huff(0xC0 + sym - 280, 8)


def maxdist_raw():
    """A stored block with 32768 random bytes, then a fixed block: match(258, 32768),
    match(3, 1), end of block. Returns (raw_stream, expected_output)."""
    data = random_bytes(32768, 77)
    w = BitWriter()
    w.bits(0, 1)  # BFINAL = 0
    w.bits(0, 2)  # BTYPE = stored
    raw = bytearray(w.getvalue())
    raw += struct.pack("<HH", 32768, 0xFFFF ^ 32768) + data

    w = BitWriter()
    w.bits(1, 1)  # BFINAL = 1
    w.bits(1, 2)  # BTYPE = fixed
    fixed_literal(w, 285)  # length 258 (no extra bits)
    w.huff(29, 5)  # distance symbol 29: 24577..32768, 13 extra bits
    w.bits(32768 - 24577, 13)
    fixed_literal(w, 257)  # length 3
    w.huff(0, 5)  # distance symbol 0: distance 1
    fixed_literal(w, 256)  # end of block
    raw += w.getvalue()

    out = bytearray(data)
    for _ in range(258):  # copy from distance 32768
        out.append(out[len(out) - 32768])
    for _ in range(3):  # copy from distance 1
        out.append(out[len(out) - 1])
    return bytes(raw), bytes(out)


def write(name, data):
    with open(os.path.join(HERE, name), "wb") as f:
        f.write(data)


def main():
    cases = {
        "stored": (text_bytes(20000, 1), 0, zlib.Z_DEFAULT_STRATEGY),
        "fixed": (text_bytes(20000, 2), 6, zlib.Z_FIXED),
        "dynamic": (text_bytes(96 * 1024, 3), 9, zlib.Z_DEFAULT_STRATEGY),
        "huffman_only": (text_bytes(20000, 4), 6, zlib.Z_HUFFMAN_ONLY),
        "rle": (b"a" * 3000 + b"bc" * 1500 + b"d" * 500, 6, zlib.Z_RLE),
        "empty": (b"", 6, zlib.Z_DEFAULT_STRATEGY),
        "tiny": (text_bytes(300, 5), 9, zlib.Z_DEFAULT_STRATEGY),
    }
    for name, (data, level, strategy) in cases.items():
        zs, raw = compress(data, level, strategy)
        write(name + ".zlib", zs)
        write(name + ".raw", raw)
        write(name + ".out", data)

    raw, expected = maxdist_raw()
    write("maxdist.raw", raw)
    write("maxdist.out", expected)
    adler = struct.pack(">I", zlib.adler32(expected))
    write("maxdist.zlib", b"\x78\x9c" + raw + adler)

    verify()
    print("wrote and verified vectors in", HERE)


def verify():
    """Every stream must decode to its .out file with Python's zlib."""
    names = ["stored", "fixed", "dynamic", "huffman_only", "rle", "empty", "tiny", "maxdist"]
    for name in names:
        with open(os.path.join(HERE, name + ".out"), "rb") as f:
            expected = f.read()
        with open(os.path.join(HERE, name + ".zlib"), "rb") as f:
            zs = f.read()
        with open(os.path.join(HERE, name + ".raw"), "rb") as f:
            raw = f.read()
        assert zlib.decompress(zs) == expected, name + ".zlib"
        d = zlib.decompressobj(-15)
        assert d.decompress(raw) + d.flush() == expected, name + ".raw"


if __name__ == "__main__":
    main()
