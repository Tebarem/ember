/*
 * Border images (CSS Backgrounds 3 section 6). The source (border-image-source) is cut into nine slices by
 * border-image-slice; border-image-width gives the thickness of the nine areas of the border image area (the border
 * box grown by border-image-outset). The corners are scaled to fit their areas; the top, bottom, left and right edges
 * and, with fill, the middle are stretched, or tiled by border-image-repeat (repeat, round or space). Each edge tiles
 * at the scale of its own band (top: its width over its slice, bottom: the bottom band's, and so on); the middle takes
 * the top band's scale horizontally and the left band's vertically.
 *
 * A repeated image region is one tiled image item whose tiles show the slice (a uv sub-rect) and are clipped to the region, so
 * a 1px slice repeated along a long edge is still one item. A gradient has no tiled item: each tile is a gradient item
 * whose geometry is mapped from the gradient's box (the border image area) to the tile, clipped to its area; past
 * BI_TILE_CAP such items in one region the rest of the tiles are not drawn (a safety cap). The border
 * radius does not clip a border image, and the outset is ink overflow: the items reach past the border box, and
 * nothing here changes layout.
 *
 * Not done: a cross-fade() source; box-decoration-break (a broken inline box paints each fragment as its own box, the
 * clone behaviour, whatever the property says); calc() in the slice values.
 */
#include "paint/paint_int.h"

#include <math.h>
#include <string.h>

#define BI_WHITE 0xFFFFFFFFu
/* The most gradient tiles one region draws (a safety cap against a tiny tile over a huge area). */
#define BI_TILE_CAP 4096u

enum { BI_STRETCH = 0, BI_REPEAT, BI_ROUND, BI_SPACE };

/* The image a border image paints: a url (its natural size, in image px) or a gradient over the border image area
 * (its size in CSS px). org is the page origin of the image. */
typedef struct bi_src {
    bool grad;
    embi_str url;
    float org[2];
    float iw, ih;
    bool nearest;
    uint8_t kind;      /* gradient: EMBI_GRAD_*_DL */
    bool repeat;       /* gradient: repeating */
    float geo[6];      /* gradient: geometry over the border image area, in page px */
    uint32_t first, n; /* gradient: the stops in the list */
} bi_src;

/* The tiles of one axis: the origins first + i * step for i in [0, n), each ext long. */
typedef struct bi_axis {
    uint32_t n;
    float first, step, ext;
} bi_axis;

/* The value of side i of a list (the value itself when it is not a list; the last item when the list is shorter). */
static const embi_value *bi_item(const embi_value *v, uint32_t i)
{
    if (v != nullptr && v->kind == EMBI_V_LIST && v->list.n > 0) {
        return &v->list.items[i < v->list.n ? i : v->list.n - 1];
    }
    return v;
}

/* Whether a border-image-slice value has the fill keyword (its fifth item). */
static bool bi_fill(const embi_value *v)
{
    if (v == nullptr || v->kind != EMBI_V_LIST || v->list.n != 5) {
        return false;
    }
    const embi_value *f = &v->list.items[4];
    return f->kind == EMBI_V_KEYWORD && f->kw == EMBI_KW_fill;
}

static uint8_t bi_mode(const embi_value *v)
{
    if (v != nullptr && v->kind == EMBI_V_KEYWORD) {
        switch (v->kw) {
        case EMBI_KW_repeat:
            return BI_REPEAT;
        case EMBI_KW_round:
            return BI_ROUND;
        case EMBI_KW_space:
            return BI_SPACE;
        default:
            break;
        }
    }
    return BI_STRETCH;
}

/* A count from a float, clamped to [0, BI_TILE_CAP + 1]: a NaN or an infinite value is not converted as is. */
static uint32_t bi_count(float v)
{
    if (!(v >= 0.0f)) {
        return 0;
    }
    if (v > (float)(BI_TILE_CAP + 1u)) {
        return BI_TILE_CAP + 1u;
    }
    return (uint32_t)v;
}

/* A slice of side i (top, right, bottom, left) in image px: a number, or a percentage of the image height (top and
 * bottom) or width (left and right), clamped to that image size on its own. */
static float bi_slice(const embi_value *v, uint32_t i, float iw, float ih)
{
    const embi_value *x = bi_item(v, i);
    if (x == nullptr) {
        return 0.0f;
    }
    float dim = i == 0 || i == 2 ? ih : iw;
    float px = 0.0f;
    if (x->kind == EMBI_V_PERCENT) {
        px = (float)(x->num * 0.01) * dim;
    } else if (x->kind == EMBI_V_NUMBER || x->kind == EMBI_V_INTEGER) {
        px = (float)x->num;
    }
    return px < dim ? px : dim;
}

/* The tiles of one axis of a region: the area [a0, a0 + len) is covered by tiles of length tile (the scaled size of the
 * source along the axis) in mode. Repeat centers a tile on the middle of the area and lets the ends clip; round fits a
 * whole number of tiles, stretched; space fits whole tiles with equal gaps before, between and after them (none when
 * not one fits). Stretch is one tile over the whole area. */
static bi_axis bi_tiles(uint8_t mode, float a0, float len, float tile)
{
    bi_axis a = {1, a0, len, len};
    if (mode == BI_STRETCH) {
        return a;
    }
    if (!(tile > 0.0f)) {
        a.n = 0;
        return a;
    }
    switch (mode) {
    case BI_REPEAT: {
        float c = a0 + len * 0.5f - tile * 0.5f;
        a.first = c - ceilf((c - a0) / tile) * tile;
        a.step = tile;
        a.ext = tile;
        a.n = bi_count(ceilf((a0 + len - a.first) / tile));
        break;
    }
    case BI_ROUND: {
        float k = roundf(len / tile);
        if (!(k >= 1.0f)) {
            k = 1.0f;
        }
        a.n = bi_count(k);
        a.step = len / k;
        a.ext = a.step;
        a.first = a0;
        break;
    }
    case BI_SPACE: {
        float k = floorf(len / tile);
        if (!(k >= 1.0f)) {
            a.n = 0;
            break;
        }
        float gap = (len - k * tile) / (k + 1.0f);
        a.n = bi_count(k);
        a.first = a0 + gap;
        a.step = tile + gap;
        a.ext = tile;
        break;
    }
    default:
        break;
    }
    return a;
}

/* Draws one tile piece: the tile T (page px) of the source rect S (image px), clipped to the region C. */
static void bi_piece(embi_pst *p, const bi_src *src, const float S[4], const float T[4], const float C[4], uint32_t xf,
                     uint32_t clip)
{
    float x0 = fmaxf(T[0], C[0]), y0 = fmaxf(T[1], C[1]);
    float x1 = fminf(T[0] + T[2], C[0] + C[2]), y1 = fminf(T[1] + T[3], C[1] + C[3]);
    if (!(x1 > x0) || !(y1 > y0)) {
        return;
    }
    float I[4] = {x0, y0, x1 - x0, y1 - y0};
    if (src->grad) {
        /* the gradient maps from its box to the tile: the piece's source point (x, y) lands at T's origin plus its offset
         * from S's origin, scaled per axis; the stops are fractions of the gradient and do not change */
        float kx = T[2] / S[2], ky = T[3] / S[3];
        float ox = src->org[0] + S[0], oy = src->org[1] + S[1];
        float g[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        g[0] = T[0] + (src->geo[0] - ox) * kx;
        g[1] = T[1] + (src->geo[1] - oy) * ky;
        if (src->kind == EMBI_GRAD_RADIAL_DL) {
            g[2] = src->geo[2] * kx;
            g[3] = src->geo[3] * ky;
        } else if (src->kind == EMBI_GRAD_LINEAR_DL) {
            g[2] = T[0] + (src->geo[2] - ox) * kx;
            g[3] = T[1] + (src->geo[3] - oy) * ky;
        } else {
            g[2] = src->geo[2];
            g[3] = src->geo[3];
        }
        embi_pb_gradient_at(p, src->kind, I, g, src->first, src->n, src->repeat, xf, clip);
        return;
    }
    float sx = S[2] / T[2], sy = S[3] / T[3];
    float uv[4] = {(S[0] + (x0 - T[0]) * sx) / src->iw, (S[1] + (y0 - T[1]) * sy) / src->ih,
                   (S[0] + (x1 - T[0]) * sx) / src->iw, (S[1] + (y1 - T[1]) * sy) / src->ih};
    embi_pb_image(p, src->url, I, uv, BI_WHITE, src->nearest, xf, clip);
}

/* Draws one region: d its destination rect, s its source rect (image px), mx, my the repeat modes and bx, by the tile
 * scales. Each axis is one tile over the region, or the tiles of its mode, each clipped to the region. A repeated image
 * is one tiled item (embi_pb_image_tile with the slice as its uv sub-rect); a stretched image is one image item and a
 * gradient is one item per tile. */
static void bi_draw(embi_pst *p, const bi_src *src, const float d[4], const float s[4], uint8_t mx, uint8_t my, float bx,
                    float by, uint32_t xf, uint32_t clip)
{
    if (!(d[2] > 0.0f) || !(d[3] > 0.0f) || !(s[2] > 0.0f) || !(s[3] > 0.0f)) {
        return;
    }
    bi_axis x = bi_tiles(mx, d[0], d[2], s[2] * bx);
    bi_axis y = bi_tiles(my, d[1], d[3], s[3] * by);
    if (!src->grad && (mx != BI_STRETCH || my != BI_STRETCH) && x.n > 0 && y.n > 0) {
        /* a repeated image: one tiled item over the region, each tile showing the slice (clipped to the region when drawn) */
        const float first[2] = {x.first, y.first}, size[2] = {x.ext, y.ext}, step[2] = {x.step, y.step};
        const float sub[4] = {s[0] / src->iw, s[1] / src->ih, (s[0] + s[2]) / src->iw, (s[1] + s[3]) / src->ih};
        embi_pb_image_tile(p, src->url, d, first, size, step, sub, BI_WHITE, src->nearest, xf, clip);
        return;
    }
    uint32_t drawn = 0;
    for (uint32_t j = 0; j < y.n; j++) {
        for (uint32_t i = 0; i < x.n; i++) {
            if (drawn++ >= BI_TILE_CAP) {
                return; /* the documented limit: the tiles past the cap are not drawn */
            }
            float T[4] = {x.first + (float)i * x.step, y.first + (float)j * y.step, x.ext, y.ext};
            bi_piece(p, src, s, T, d, xf, clip);
        }
    }
}

/* The scale of a band of width wa over slice sa when it has both, else the scale of the opposite band (wb over sb),
 * else the image at its own size (border-image-slice-fill-003: a zero border area repeats the image at 1:1). */
static float bi_band_scale(float wa, float sa, float wb, float sb)
{
    if (sa > 0.0f && wa > 0.0f) {
        return wa / sa;
    }
    if (sb > 0.0f && wb > 0.0f) {
        return wb / sb;
    }
    return 1.0f;
}

bool embi_pb_border_image(embi_pst *p, const embi_frag *f, const embi_style *s, const float bb[4], uint32_t xf,
                          uint32_t clip)
{
    if ((f->flags & EMBI_FF_TABLE) != 0 && embi_frag_coll(f) != nullptr) {
        return false; /* border-image has no effect on a table in the collapsing border model */
    }
    const embi_value *sv = embi_style_value(s, EMBI_PROP_border_image_source);
    if (sv == nullptr || sv->kind != EMBI_V_IMAGE || sv->image == nullptr) {
        return false;
    }
    float dppx = 1.0f;
    const embi_image *im = sv->image;
    if (im->kind == EMBI_IMG_IMAGE_SET) {
        im = embi_lay_image_resolve(im, &dppx);
    }
    im = embi_pb_simple_image(p, im);
    if (im == nullptr) {
        return false;
    }
    bi_src src;
    memset(&src, 0, sizeof src);
    if (im->kind == EMBI_IMG_URL) {
        bool ratio = false;
        if (p->cfg->image_size == nullptr || !p->cfg->image_size(p->cfg->user, im->url, &src.iw, &src.ih, &ratio) ||
            !(src.iw > 0.0f) || !(src.ih > 0.0f)) {
            return false; /* not loaded: the border styles paint instead */
        }
        /* An image-set option at resolution dppx: its natural size is divided by dppx, so the slice numbers (and the
         * sizes of the image) are in CSS px, and an image px is 1/dppx of one. */
        if (dppx > 0.0f && dppx != 1.0f) {
            src.iw /= dppx;
            src.ih /= dppx;
        }
        src.url = im->url;
    } else {
        src.grad = true;
    }
    src.nearest = embi_pb_kw(s, EMBI_PROP_image_rendering) == EMBI_KW_pixelated ||
                  embi_pb_kw(s, EMBI_PROP_image_rendering) == EMBI_KW_crisp_edges;
    float fs = embi_pb_fontsize(s);
    p->lh = embi_pb_lineheight(s);
    p->bg_f = f;
    p->bg_s = s;

    /* side i: 0 top, 1 right, 2 bottom, 3 left. The border widths of the layout (top, right, bottom, left). */
    float bw[4] = {embi_frag_bdr(f, 1), embi_frag_bdr(f, 2), embi_frag_bdr(f, 3), embi_frag_bdr(f, 0)};

    /* border-image-outset: a number of border widths or a length, outside the border box */
    const embi_value *ov = embi_style_value(s, EMBI_PROP_border_image_outset);
    float o[4];
    for (uint32_t i = 0; i < 4; i++) {
        const embi_value *x = bi_item(ov, i);
        o[i] = 0.0f;
        if (x != nullptr && x->kind == EMBI_V_NUMBER) {
            o[i] = (float)x->num * bw[i];
        } else if (x != nullptr) {
            bool ok = false;
            o[i] = embi_pb_len(p, x, 0.0f, fs, &ok);
            if (!ok || !(o[i] > 0.0f)) {
                o[i] = 0.0f;
            }
        }
    }
    /* the border image area: x, y, w, h. A gradient is sized to it. */
    float O[4] = {bb[0] - o[3], bb[1] - o[0], bb[2] + o[3] + o[1], bb[3] + o[0] + o[2]};
    if (src.grad) {
        if (!(O[2] > 0.0f) || !(O[3] > 0.0f)) {
            return false;
        }
        src.iw = O[2];
        src.ih = O[3];
        src.org[0] = O[0];
        src.org[1] = O[1];
        if (!embi_pb_grad_prepare(p, im->grad, s, O, fs, &src.kind, src.geo, &src.first, &src.n)) {
            return false;
        }
        src.repeat = im->grad->repeating;
    }

    /* border-image-slice: each side clamped to the image size on its own (opposite slices may then overlap) */
    const embi_value *slv = embi_style_value(s, EMBI_PROP_border_image_slice);
    float sl[4];
    for (uint32_t i = 0; i < 4; i++) {
        sl[i] = bi_slice(slv, i, src.iw, src.ih);
    }

    /* border-image-width: the thickness of each area. auto is the slice (a gradient is sized to the border image area,
     * so it has one too: border-image-image-type-003). */
    const embi_value *wv = embi_style_value(s, EMBI_PROP_border_image_width);
    float W[4];
    for (uint32_t i = 0; i < 4; i++) {
        const embi_value *x = bi_item(wv, i);
        bool vert = i == 0 || i == 2;
        float full = vert ? O[3] : O[2];
        W[i] = bw[i];
        if (x == nullptr) {
            continue;
        }
        if (x->kind == EMBI_V_NUMBER) {
            W[i] = (float)x->num * bw[i];
        } else if (x->kind == EMBI_V_PERCENT) {
            W[i] = (float)(x->num * 0.01) * full;
        } else if (x->kind == EMBI_V_KEYWORD && x->kw == EMBI_KW_auto) {
            W[i] = sl[i];
        } else {
            bool ok = false;
            float v = embi_pb_len(p, x, full, fs, &ok);
            if (ok) {
                W[i] = v;
            }
        }
        if (!(W[i] > 0.0f)) {
            W[i] = 0.0f;
        }
    }
    /* Widths that overlap are scaled down by one factor, so that the opposite pairs fit the border image area. */
    float vs = W[0] + W[2], hs = W[1] + W[3];
    float fk = 1.0f;
    if (vs > O[3] && vs > 0.0f) {
        fk = fminf(fk, O[3] / vs);
    }
    if (hs > O[2] && hs > 0.0f) {
        fk = fminf(fk, O[2] / hs);
    }
    if (fk < 1.0f) {
        for (uint32_t i = 0; i < 4; i++) {
            W[i] *= fk;
        }
    }

    const embi_value *rv = embi_style_value(s, EMBI_PROP_border_image_repeat);
    uint8_t rh = bi_mode(bi_item(rv, 0)), rvm = bi_mode(bi_item(rv, 1));

    /* The tile scale of each edge is that of its own band; the middle takes the top band's scale horizontally and the
     * left band's vertically. */
    float kTop = bi_band_scale(W[0], sl[0], W[2], sl[2]);
    float kBot = bi_band_scale(W[2], sl[2], W[0], sl[0]);
    float kLft = bi_band_scale(W[3], sl[3], W[1], sl[1]);
    float kRgt = bi_band_scale(W[1], sl[1], W[3], sl[3]);

    float X0 = O[0], Y0 = O[1];
    float X1 = O[0] + W[3], Y1 = O[1] + W[0];
    float X2 = O[0] + O[2] - W[1], Y2 = O[1] + O[3] - W[2];
    float iw = src.iw, ih = src.ih;
    float SX1 = sl[3], SX2 = iw - sl[1], SY1 = sl[0], SY2 = ih - sl[2];
    /* the middle is empty when the opposite slices reach across the image */
    float mw = SX2 - SX1 > 0.0f ? SX2 - SX1 : 0.0f;
    float mh = SY2 - SY1 > 0.0f ? SY2 - SY1 : 0.0f;

    /* the corners are never tiled: each is scaled to its area */
    const float cTL[4] = {X0, Y0, W[3], W[0]}, sTL[4] = {0.0f, 0.0f, sl[3], sl[0]};
    const float cTR[4] = {X2, Y0, W[1], W[0]}, sTR[4] = {SX2, 0.0f, sl[1], sl[0]};
    const float cBL[4] = {X0, Y2, W[3], W[2]}, sBL[4] = {0.0f, SY2, sl[3], sl[2]};
    const float cBR[4] = {X2, Y2, W[1], W[2]}, sBR[4] = {SX2, SY2, sl[1], sl[2]};
    bi_draw(p, &src, cTL, sTL, BI_STRETCH, BI_STRETCH, 1.0f, 1.0f, xf, clip);
    bi_draw(p, &src, cTR, sTR, BI_STRETCH, BI_STRETCH, 1.0f, 1.0f, xf, clip);
    bi_draw(p, &src, cBL, sBL, BI_STRETCH, BI_STRETCH, 1.0f, 1.0f, xf, clip);
    bi_draw(p, &src, cBR, sBR, BI_STRETCH, BI_STRETCH, 1.0f, 1.0f, xf, clip);

    /* the top and bottom edges: the horizontal repeat, each tile scaled by its own band; the stretched axis is the
     * band's thickness */
    const float dtop[4] = {X1, Y0, X2 - X1, W[0]}, stop[4] = {SX1, 0.0f, mw, sl[0]};
    const float dbot[4] = {X1, Y2, X2 - X1, W[2]}, sbot[4] = {SX1, SY2, mw, sl[2]};
    bi_draw(p, &src, dtop, stop, rh, BI_STRETCH, kTop, 1.0f, xf, clip);
    bi_draw(p, &src, dbot, sbot, rh, BI_STRETCH, kBot, 1.0f, xf, clip);

    /* the left and right edges: the vertical repeat, each tile scaled by its own band */
    const float dlft[4] = {X0, Y1, W[3], Y2 - Y1}, slft[4] = {0.0f, SY1, sl[3], mh};
    const float drgt[4] = {X2, Y1, W[1], Y2 - Y1}, srgt[4] = {SX2, SY1, sl[1], mh};
    bi_draw(p, &src, dlft, slft, BI_STRETCH, rvm, 1.0f, kLft, xf, clip);
    bi_draw(p, &src, drgt, srgt, BI_STRETCH, rvm, 1.0f, kRgt, xf, clip);

    /* the middle, with fill: the top band's scale across and the left band's along */
    if (bi_fill(slv)) {
        const float dmid[4] = {X1, Y1, X2 - X1, Y2 - Y1}, smid[4] = {SX1, SY1, mw, mh};
        bi_draw(p, &src, dmid, smid, rh, rvm, kTop, kLft, xf, clip);
    }
    return true;
}
