#pragma once
/*
 * Glyph outlines of shaped text as paths (module-internal, shared by the canvas text and the paint of
 * -webkit-text-stroke). The outlines are the quads of embi_font_glyph_outline, grouped by contour as the font gives them.
 * Coordinates are in local px with y down: the baseline origin of the text at (ox, oy), each glyph moved by its pen and its
 * offsets, and the outline scaled from font units by the run's size.
 */
#include "font/font.h"
#include "text/text.h"

#include <stdbool.h>
#include <stdint.h>

/* Where the outlines go. The callbacks may be nullptr (move_to, quad_to and close are called only when set). glyph is
 * called once per glyph that has an outline, before its contours, with the outline in font units and the glyph origin
 * (gx, gy) in px of the run (gy up, as the layout gives it) and k, the px per font unit of the glyph's run. */
typedef struct embi_glyph_sink {
    void *user;
    void (*glyph)(void *user, const embi_outline *ol, double gx, double gy, double k);
    void (*move_to)(void *user, double x, double y);
    void (*quad_to)(void *user, double cx, double cy, double x, double y);
    void (*close)(void *user);
} embi_glyph_sink;

/* The mapping of the outlines into local px. draw makes the outlines the way embi_text_draw draws them (the run's variation
 * instance and the skew of a synthetic oblique); without it the default instance and no skew are used. sx scales the
 * horizontal positions (1 for the canvas's untransformed text). */
typedef struct embi_glyph_map {
    double ox, oy;
    double sx;
    bool draw;
} embi_glyph_map;

/* Walks the glyph outlines of the visual runs vr of lay, in order, and feeds them to sink. Glyphs without an outline (spaces,
 * bitmap and colour glyphs that have none) are skipped. */
void embi_glyph_walk(const embi_text_layout *lay, const embi_text_vruns *vr, const embi_glyph_map *m,
                     const embi_glyph_sink *sink);

/* The outline of the bytes [lo, hi) of lay with the baseline origin at (x, y), as a path in local px (the path is for
 * stroking and filling, see gpu/path.h). whole is the visual order of all of lay, or nullptr: the range that is the whole
 * text uses it. bounds receives x0, y0, x1, y1, the box of the control points. nullptr when the range has no outline. The
 * caller owns the path until it is handed to a display list (embi_dl_add_path). */
struct embi_path *embi_glyph_path(const embi_text_layout *lay, const embi_text_vruns *whole, uint32_t lo, uint32_t hi,
                                  float x, float y, float bounds[4]);

/* The content hash of a path made by embi_glyph_path (equal outlines give equal hashes). */
uint64_t embi_glyph_path_hash(const struct embi_path *p);
