# DEFLATE test vectors

Used by `tests/unit/codec/test_codec_inflate.c`.

Each case `<name>` has three files:

| File | Contents |
|---|---|
| `<name>.zlib` | zlib-wrapped stream (RFC 1950) |
| `<name>.raw` | raw DEFLATE stream (RFC 1951) |
| `<name>.out` | the bytes both streams decode to |

| Case | Block types covered |
|---|---|
| `stored` | stored blocks (level 0) |
| `fixed` | fixed Huffman (`Z_FIXED`) |
| `dynamic` | dynamic Huffman, long matches, 96 KiB |
| `huffman_only` | dynamic blocks with no matches (all-zero distance code) |
| `rle` | distance-1 runs (`Z_RLE`) |
| `empty` | empty input |
| `tiny` | 300-byte input, small enough for truncation at every offset and bit flips |
| `maxdist` | hand-built: stored block, then a fixed block with a 258-byte match at distance 32768 (the largest distance) and a distance-1 overlap |

## Regenerating

    python tests/data/deflate/gen_vectors.py

The script needs only the Python standard library. Python's `zlib` produces the compressed streams.
`maxdist` is written with a small bit writer in the script, because no encoder emits that distance
on purpose. Every file is checked when it is written: the zlib and raw streams must decode to the
`.out` file with Python's own `zlib`, so a bad vector fails the script.
