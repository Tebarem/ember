/*
 * Glyph outlines of shaped text as paths (see glyph_path.h).
 */
#include "text/glyph_path.h"

#include "gpu/path.h"
#include "text/text_int.h"

#include <string.h>

static bool keep_layer(void *ctx, const embi_colr_op *op)
{
    (void)ctx;
    (void)op;
    return true;
}

/* Whether the glyph has a colour record (COLR): then it is drawn as its layers, and it has no outline to stroke. */
static bool has_colr_record(const embi_font *font, uint32_t gid, const int16_t *coords)
{
    const embi_colr_visitor v = {nullptr, keep_layer};
    return embi_font_colr_paint(font, gid, coords, 0, &v);
}

void embi_glyph_walk(const embi_text_layout *lay, const embi_text_vruns *vr, const embi_glyph_map *m,
                     const embi_glyph_sink *sink)
{
    embi_outline ol;
    memset(&ol, 0, sizeof ol);
    for (uint32_t vi = 0; vi < vr->items.len; vi++) {
        const embi_text_vrun *v = &vr->items.data[vi];
        const embi_text_run *run = &lay->runs.data[v->run];
        embi_face *face = run->face;
        const embi_font *font = face != nullptr ? embi_face_font(face) : nullptr;
        if (font == nullptr || font->metrics.units_per_em == 0 || !(run->size > 0.0f)) {
            continue;
        }
        /* A face with bitmap strikes is drawn from its bitmaps (draw.c), which have no outline to stroke. */
        if (m->draw && embi_font_has_bitmaps(font) && !font->metrics.has_colr) {
            continue;
        }
        const double k = (double)run->size / (double)font->metrics.units_per_em;
        const int16_t *coords = nullptr;
        double skew = 0.0;
        if (m->draw) {
            coords = embi_face_inst_coords(face, run->inst);
            if ((run->synth & EMBI_RUN_SYNTH_OBLIQUE) != 0) {
                skew = EMBI_OBLIQUE_SKEW;
            }
        }
        double pen = v->x;
        for (uint32_t gi = v->glyph_begin; gi < v->glyph_end; gi++) {
            const embi_text_glyph *g = &lay->glyphs.data[gi];
            if (embi_font_glyph_outline(font, g->gid, coords, &ol) && ol.quads.len > 0) {
                if (m->draw && font->metrics.has_colr && has_colr_record(font, g->gid, coords)) {
                    pen += g->advance;
                    continue;
                }
                const double gx = pen + g->x_offset, gy = g->y_offset;
                if (sink->glyph != nullptr) {
                    sink->glyph(sink->user, &ol, gx, gy, k);
                }
                /* A point of the outline in the mapped space: y down, skewed by the oblique. */
#define GP_X(px, py) (m->ox + m->sx * (gx + k * (px) + k * skew * (py)))
#define GP_Y(py) (m->oy - (gy + k * (py)))
                uint32_t ci = 0;
                bool open = false;
                for (uint32_t qi = 0; qi < ol.quads.len; qi++) {
                    const embi_quad *q = &ol.quads.data[qi];
                    if (ci < ol.contours.len && ol.contours.data[ci] == qi) {
                        if (open && sink->close != nullptr) {
                            sink->close(sink->user);
                        }
                        if (sink->move_to != nullptr) {
                            sink->move_to(sink->user, GP_X(q->p0.x, q->p0.y), GP_Y(q->p0.y));
                        }
                        ci++;
                        open = true;
                    }
                    if (sink->quad_to != nullptr) {
                        sink->quad_to(sink->user, GP_X(q->p1.x, q->p1.y), GP_Y(q->p1.y), GP_X(q->p2.x, q->p2.y),
                                      GP_Y(q->p2.y));
                    }
                }
                if (open && sink->close != nullptr) {
                    sink->close(sink->user);
                }
#undef GP_X
#undef GP_Y
            }
            pen += g->advance;
        }
    }
    embi_outline_free(&ol);
}

/* The path builder of embi_glyph_path: the path and the box of its control points. The path follows the exact outlines:
 * the pixel fitting that small filled text gets (baseline row, hinting) is not applied, so a thin stroke over small text may
 * sit up to half a pixel off centre on the fitted fill. */
typedef struct gp_build {
    embi_path *path;
    bool any;
    double x0, y0, x1, y1;
} gp_build;

static void gp_grow(gp_build *b, double x, double y)
{
    if (!b->any) {
        b->x0 = b->x1 = x;
        b->y0 = b->y1 = y;
        b->any = true;
        return;
    }
    b->x0 = x < b->x0 ? x : b->x0;
    b->x1 = x > b->x1 ? x : b->x1;
    b->y0 = y < b->y0 ? y : b->y0;
    b->y1 = y > b->y1 ? y : b->y1;
}

static void gp_move(void *user, double x, double y)
{
    gp_build *b = user;
    gp_grow(b, x, y);
    embi_path_move_to(b->path, (float)x, (float)y);
}

static void gp_quad(void *user, double cx, double cy, double x, double y)
{
    gp_build *b = user;
    gp_grow(b, cx, cy);
    gp_grow(b, x, y);
    embi_path_quad_to(b->path, (float)cx, (float)cy, (float)x, (float)y);
}

static void gp_close(void *user)
{
    gp_build *b = user;
    embi_path_close(b->path);
}

struct embi_path *embi_glyph_path(const embi_text_layout *lay, const embi_text_vruns *whole, uint32_t lo, uint32_t hi,
                                  float x, float y, float bounds[4])
{
    embi_text_vruns own;
    memset(&own, 0, sizeof own);
    const embi_text_vruns *vr = nullptr;
    if (lo == 0 && hi >= lay->text_len && whole != nullptr && whole->items.len > 0) {
        vr = whole;
    } else {
        if (hi > lay->text_len) {
            hi = lay->text_len;
        }
        if (lo < hi && embi_text_line(lay, lo, hi, &own)) {
            vr = &own;
        }
    }
    gp_build b;
    memset(&b, 0, sizeof b);
    b.path = vr != nullptr ? embi_path_create(0.0f) : nullptr;
    if (b.path != nullptr) {
        const embi_glyph_map m = {.ox = x, .oy = y, .sx = 1.0, .draw = true};
        const embi_glyph_sink sink = {.user = &b, .glyph = nullptr, .move_to = gp_move, .quad_to = gp_quad, .close = gp_close};
        embi_glyph_walk(lay, vr, &m, &sink);
    }
    embi_text_vruns_free(&own);
    if (b.path == nullptr || !b.any || embi_path_is_empty(b.path)) {
        if (b.path != nullptr) {
            embi_path_destroy(b.path);
        }
        return nullptr;
    }
    bounds[0] = (float)b.x0;
    bounds[1] = (float)b.y0;
    bounds[2] = (float)b.x1;
    bounds[3] = (float)b.y1;
    return b.path;
}

uint64_t embi_glyph_path_hash(const struct embi_path *p)
{
    return embi_path_hash((const embi_path *)p);
}
