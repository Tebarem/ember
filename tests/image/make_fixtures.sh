#!/bin/sh
# Regenerates the JPEG and GIF fixtures for the image decoder tests (tests/unit/image/).
#
# Run by hand, never by the build. Needs ImageMagick (`magick`, or set MAGICK to another binary).
# Every file is generated locally; nothing is downloaded. The committed outputs are what the tests
# read, so this script exists to document how they were made and to regenerate them.
#
#   jpg_*.jpg     JPEG: baseline 4:2:0 and 4:4:4, progressive, grayscale, CMYK (Adobe), restart
#                 markers, 1x1 and 17x13, EXIF orientations 1-8 (exif_o1..exif_o8), truncated.
#   gif*.gif      GIF: GIF87a, GIF89a interlaced, transparent, animated (first frame checked),
#                 local color table, odd size, truncated; gif_sdl_fallback.gif (fuzz regression, kept).
#   ref/*.png     Reference decodes by ImageMagick (straight RGBA8, 8 bits per channel). The
#                 JPEG references apply the EXIF orientation (-auto-orient), like the decoder.
#
# Needs Python 3 (PYTHON, default python) for the two fixtures ImageMagick cannot write.
# Usage: sh tests/image/make_fixtures.sh

set -eu
cd "$(dirname "$0")"
M=${MAGICK:-magick}
PYTHON=${PYTHON:-python}

mkdir -p ref
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Source content: deterministic plasma (seeded), 96x64 for the JPEG and GIF main cases.
$M -seed 7 -size 96x64 'plasma:rgb(255,40,0)-rgb(0,60,255)' "$TMP/src.png"
$M -seed 11 -size 40x24 'plasma:rgb(20,200,40)-rgb(250,250,0)' "$TMP/src40x24.png"
$M "$TMP/src40x24.png" -quality 90 "$TMP/jpg_src40x24.jpg"
$M -size 1x1 'xc:rgb(200,30,90)' "$TMP/px1.png"
$M "$TMP/src.png" -resize 17x13! "$TMP/src17x13.png"

# ---- JPEG
$M "$TMP/src.png" -quality 90 -sampling-factor 2x2 -interlace none jpg_baseline_420.jpg
$M "$TMP/src.png" -quality 90 -sampling-factor 1x1 -interlace none jpg_baseline_444.jpg
$M "$TMP/src.png" -quality 90 -sampling-factor 2x2 -interlace Plane jpg_progressive.jpg
$M "$TMP/src.png" -quality 90 -colorspace Gray -interlace none jpg_gray.jpg
$M "$TMP/src.png" -quality 90 -colorspace CMYK -interlace none jpg_cmyk_adobe.jpg
$M "$TMP/src.png" -quality 90 -define jpeg:restart-interval=1 -interlace none jpg_restart.jpg
$M "$TMP/px1.png" -quality 90 jpg_1x1.jpg
$M "$TMP/src17x13.png" -quality 90 jpg_17x13.jpg
for o in 1 2 3 4 5 6 7 8; do
    "$PYTHON" make_fixtures_extra.py exif "$TMP/jpg_src40x24.jpg" "jpg_exif_o$o.jpg" "$o"
done
# Truncated: the first half of the progressive file, and a cut inside the headers.
head -c 900 jpg_progressive.jpg > jpg_truncated.jpg
head -c 30 jpg_baseline_420.jpg > jpg_header_cut.jpg

# ---- GIF
$M "$TMP/src.png" GIF87:gif87a.gif
$M "$TMP/src.png" -interlace GIF gif89_interlaced.gif
$M -size 48x32 xc:none -fill 'rgb(230,20,20)' -draw 'circle 16,16 16,4' \
   -fill 'rgb(20,20,230)' -draw 'rectangle 28,8 44,28' gif89_transparent.gif
$M "$TMP/src17x13.png" gif89_odd_17x13.gif
$M -delay 10 -size 32x24 'plasma:rgb(255,0,0)-rgb(255,255,0)' \
   -size 32x24 'plasma:rgb(0,0,0)-rgb(255,255,255)' \
   -size 32x24 'plasma:rgb(0,255,0)-rgb(0,0,255)' gif89_anim.gif
"$PYTHON" make_fixtures_extra.py lct gif89_local_ct.gif
# Truncated: the first 150 bytes of the animation (cuts inside the first frame's data).
head -c 150 gif89_anim.gif > gif_truncated.gif
# gif_sdl_fallback.gif is NOT generated here. It is a fuzz-found input (test_image_decode_fuzz,
# gif89_anim, iteration 115 of a 2000-iteration run, seed 0x5EED1234): a GIF whose header SDL_image's
# animation decoder refuses, which made SDL_image recurse into its own GIF loader. It is a regression
# fixture and must not be regenerated.

# ---- Reference decodes (first frame for GIF, oriented for JPEG)
for f in jpg_*.jpg gif*.gif; do
    case "$f" in
        jpg_truncated.jpg|jpg_header_cut.jpg|gif_truncated.gif) continue ;;
    esac
    base=${f%.*}
    $M "$f[0]" -auto-orient -alpha on PNG32:"ref/$base.png"
done
