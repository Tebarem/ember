#pragma once
/*
 * Path geometry for the SVG module (R4c). The GPU headers (gpu/path.h, gpu/stroke.h) cannot share a translation unit with the
 * style and CSS headers, so everything the render tree does with a path goes through this header, and only geom.c includes
 * the GPU ones. A path is the gpu module's embi_path (quadratic segments, see gpu/path.h); its type name is the same.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct embi_path embi_path;

/* Builder. tol is the flattening tolerance in local px (0: the default). The commands return false for non-finite input. */
embi_path *svgp_new(float tol);
void svgp_free(embi_path *p);
bool svgp_move(embi_path *p, float x, float y);
bool svgp_line(embi_path *p, float x, float y);
bool svgp_quad(embi_path *p, float cx, float cy, float x, float y);
bool svgp_cubic(embi_path *p, float c1x, float c1y, float c2x, float c2y, float x, float y);
/* SVG endpoint arc; rotation in radians. */
bool svgp_arc(embi_path *p, float rx, float ry, float rot, bool large, bool sweep, float x, float y);
bool svgp_close(embi_path *p);
bool svgp_empty(const embi_path *p);

typedef struct svg_stroke {
    float width;
    uint8_t cap;        /* 0 butt, 1 round, 2 square */
    uint8_t join;       /* 0 miter, 1 round, 2 bevel */
    float miter_limit;
    const float *dash;  /* nullptr or ndash 0: solid */
    uint32_t ndash;
    float dash_offset;
} svg_stroke;

/* The fill path of a stroke (nullptr when there is nothing to draw). tol: local px. */
embi_path *svgp_stroke(const embi_path *p, const svg_stroke *st, float tol);
/* A copy of the path with every point mapped by m (CSS order a b c d e f); the new path has tolerance tol. */
embi_path *svgp_xform(const embi_path *p, const float m[6], float tol);
/* The bounding box of the curves (x, y, w, h), exact for quadratics. False when the path is empty. */
bool svgp_bbox(const embi_path *p, float out[4]);
/* A hash of the segments (equal paths hash equal). */
uint64_t svgp_hash(const embi_path *p);
/* Whether the point is inside the filled path (rule 0 nonzero, 1 evenodd). */
bool svgp_contains(const embi_path *p, int rule, double x, double y);
/* The length of the path's curves, summing every subpath (a closed subpath includes its closing line). */
double svgp_length(const embi_path *p);
/* The point at distance d along the path (clamped to its ends); the tangent angle in radians when angle is not nullptr. */
bool svgp_point_at(const embi_path *p, double d, double out[2], double *angle);
/* The number of quadratic segments. */
uint32_t svgp_segments(const embi_path *p);
/* Bytes held by the path. */
size_t svgp_bytes(const embi_path *p);
