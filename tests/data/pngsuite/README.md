# PngSuite

Willem van Schaik's PngSuite (2017-07-19 release, http://www.schaik.com/pngsuite/), under the
license in `PngSuite.LICENSE` (free use, copy, modification and distribution).

- `*.png`: the suite. Files starting with `x` are deliberately corrupt and must fail to decode.
- `ref/<name>.rgba`: reference pixels for every valid image, generated with pypng 0.20220715.0
  (`Reader.asDirect`, palette and tRNS applied, no gamma correction): a little-endian u32 width,
  a u32 height, then width*height non-premultiplied RGBA8 pixels, row-major from the top. Samples
  are scaled to 8 bits as `(v * 255 + max / 2) / max`, where `max = 2^bitdepth - 1`, so 16-bit
  samples round to nearest.
