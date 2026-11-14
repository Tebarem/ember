/*
 * Text (W4c section 3): one TEXT item per text fragment, with its color, the text decorations of the text's box
 * and of the boxes that decorate it, and the list markers drawn as shapes.
 *
 * Geometry. A text fragment's position is relative to its line, and the line's baseline is its first baseline
 * (bl_first), so the baseline of a text run is line top + bl_first. That is exact for text on the line's
 * baseline. Text shifted by vertical-align (sub, super, a length) is drawn on the line's baseline here, because
 * the fragment does not record its own shift (the fragment tier has no room for it).
 *
 * Decorations (CSS Text Decoration 3). A decoration line is drawn across the inline content of the element that sets
 * it, and it propagates to the in-flow inline descendants of that element, down to the next atomic box. So the
 * decorations of a text run are those set on its own box and on its inline ancestors, and on the first block-level
 * ancestor (the block container whose content the run is in); the walk stops there. Each line is drawn with the
 * style and the color of the nearest box that sets that line (the decorating element). The metrics come from the
 * font of the run (its underline and strikeout position and thickness, embi_tshim_deco_metrics); a font without
 * them gets the size-based defaults (thickness fontsize / 14, underline 0.1 em below the baseline, line-through 0.3 em
 * above it). text-decoration-skip-ink is not applied yet (it is `none` in this build, see W4c).
 *
 * List markers. The disc, circle, square and disclosure markers are shapes at the sizes of Chromium's list markers
 * (their bullet size from the font's ascent): the marker text is still laid out for its width, and this file draws
 * the shape in place of the glyph. Other markers stay text.
 */
#include "paint/paint_int.h"

#include <math.h>
#include <string.h>

#define BLACK_RGBA 0xFF000000u

/* The decoration lines of a text-decoration-line value: 1 underline, 2 overline, 4 line-through. */
static uint32_t deco_mask(const embi_style *s)
{
    const embi_value *v = embi_style_value(s, EMBI_PROP_text_decoration_line);
    if (v == nullptr) {
        return 0;
    }
    uint32_t n = 0, mask = 0;
    const embi_value *items = embi_value_items(v, &n);
    for (uint32_t i = 0; i < n; i++) {
        if (items[i].kind != EMBI_V_KEYWORD) {
            continue;
        }
        switch (items[i].kw) {
        case EMBI_KW_underline:
            mask |= 1u;
            break;
        case EMBI_KW_overline:
            mask |= 2u;
            break;
        case EMBI_KW_line_through:
            mask |= 4u;
            break;
        default:
            break;
        }
    }
    return mask;
}

static uint8_t deco_style_of(const embi_style *s)
{
    switch (embi_pb_kw(s, EMBI_PROP_text_decoration_style)) {
    case EMBI_KW_double:
        return EMBI_DECO_DOUBLE;
    case EMBI_KW_dotted:
        return EMBI_DECO_DOTTED;
    case EMBI_KW_dashed:
        return EMBI_DECO_DASHED;
    case EMBI_KW_wavy:
        return EMBI_DECO_WAVY;
    default:
        return EMBI_DECO_SOLID;
    }
}

/* The thickness `auto` gives: the font's value (font_px), else fs / 14, at least 1 px. */
static float deco_thickness_auto(float fs, float font_px)
{
    float t = font_px > 0.0f ? font_px : fs / 14.0f;
    return t >= 1.0f ? t : 1.0f;
}

/* The thickness of a decoration line set by style s: the explicit length, else the font's value (font_px). */
static float deco_thickness(const embi_pst *p, const embi_style *s, float fs, float font_px)
{
    float t = font_px;
    if (!(t > 0.0f)) {
        t = fs / 14.0f;
    }
    const embi_value *tv = embi_style_value(s, EMBI_PROP_text_decoration_thickness);
    if (tv != nullptr && (tv->kind == EMBI_V_LENGTH || tv->kind == EMBI_V_PERCENT || tv->kind == EMBI_V_CALC)) {
        bool ok = false;
        float px = embi_pb_len(p, tv, fs, fs, &ok);
        if (ok && px > 0.0f) {
            t = px;
        }
    }
    return t >= 1.0f ? t : 1.0f;
}

/* The decoration color of the decorating style s: text-decoration-color, where currentcolor is the color of s itself
 * (CSS Text Decoration 3), else the text color of s. */
static uint32_t deco_color(const embi_style *s)
{
    uint32_t text = BLACK_RGBA;
    if (!embi_pb_color(s, embi_style_value(s, EMBI_PROP_color), &text)) {
        text = BLACK_RGBA;
    }
    uint32_t c = text;
    if (!embi_pb_color(s, embi_style_value(s, EMBI_PROP_text_decoration_color), &c)) {
        c = text;
    }
    return c;
}

/* The underline position mode of style s: true for `under` (below the descenders). */
static bool underline_under(const embi_style *s)
{
    return embi_pb_kw(s, EMBI_PROP_text_underline_position) == EMBI_KW_under;
}

/* True when text-underline-offset is a length, percentage or calc (not auto). */
static bool underline_offset_set(const embi_style *s)
{
    const embi_value *v = embi_style_value(s, EMBI_PROP_text_underline_offset);
    return v != nullptr && v->kind != EMBI_V_KEYWORD;
}

/* text-underline-offset in px (0 for auto). */
static float underline_offset(const embi_pst *p, const embi_style *s, float fs)
{
    const embi_value *v = embi_style_value(s, EMBI_PROP_text_underline_offset);
    if (v == nullptr || v->kind == EMBI_V_KEYWORD) {
        return 0.0f;
    }
    bool ok = false;
    float px = embi_pb_len(p, v, fs, fs, &ok);
    return ok ? px : 0.0f;
}

static bool box_in(const embi_box *d, const embi_box *a)
{
    for (; d != nullptr; d = d->parent) {
        if (d == a) {
            return true;
        }
    }
    return false;
}

/* text-decoration-inset (CSS Text Decoration 4): moves the start and end of the decoration lines that the box D sets.
 * A positive inset trims, a negative one extends. It applies at the start of D's first line fragment and the end of its
 * last one (box-decoration-break: slice), or at both ends of every fragment (clone). A percentage is of D's decoration
 * length (the fragment's under clone, the sum of all fragments under slice). x and w are the segment of the text
 * fragment f (page px); w comes back 0 when the inset trims it away. */
static void deco_inset(const embi_pst *p, const embi_frag *f, const embi_box *D, float rect_x, float *x, float *w)
{
    const embi_style *ds = D != nullptr ? D->style : nullptr;
    const embi_value *v = ds != nullptr ? embi_style_value(ds, EMBI_PROP_text_decoration_inset) : nullptr;
    bool one_kw = v != nullptr && v->kind == EMBI_V_KEYWORD; /* the lone keyword `auto` */
    if (v == nullptr || (!one_kw && (v->kind != EMBI_V_LIST || v->list.n != 2)) || f == nullptr || f->parent == nullptr ||
        f->parent->kind != EMBI_FK_LINE) {
        return;
    }
    const embi_value *vs = one_kw ? v : &v->list.items[0], *ve = one_kw ? v : &v->list.items[1];
    const embi_frag *line = f->parent;
    const embi_frag *blk = line->parent;
    bool clone = embi_pb_kw(ds, EMBI_PROP_box_decoration_break) == EMBI_KW_clone;
    float line_ox = rect_x - f->x;
    float ex0 = 1e30f, ex1 = -1e30f, total = 0.0f, line_pad = 0.0f, before = -1.0f;
    const embi_frag *first_line = nullptr, *last_line = nullptr;
    for (const embi_frag *l = blk != nullptr ? embi_frag_first(blk) : line; l != nullptr; l = blk != nullptr ? l->next : nullptr) {
        if (l->kind != EMBI_FK_LINE) {
            continue;
        }
        float lo = 1e30f, hi = -1e30f, pad = 0.0f;
        for (const embi_frag *c = embi_frag_first(l); c != nullptr; c = c->next) {
            if (c->kind == EMBI_FK_BOX && c->inl != 0 && c->box == D) {
                /* the padding and border of D's own pieces count in its length (its sides that are not broken) */
                pad += embi_frag_pad(c, 0) + embi_frag_pad(c, 2) + embi_frag_bdr(c, 0) + embi_frag_bdr(c, 2);
            }
            if (c->kind == EMBI_FK_TEXT && c->box != nullptr && box_in(c->box, D)) {
                lo = fminf(lo, c->x);
                hi = fmaxf(hi, c->x + c->w);
            }
        }
        if (hi < lo) {
            continue;
        }
        if (first_line == nullptr) {
            first_line = l;
        }
        last_line = l;
        total += hi - lo + pad;
        if (l == line) {
            before = total - (hi - lo + pad); /* the length of D's fragments on the lines before this one */
            ex0 = line_ox + lo;
            ex1 = line_ox + hi;
            line_pad = pad;
        }
    }
    if (ex1 < ex0) {
        return;
    }
    float fs = embi_pb_fontsize(ds);
    float base = clone ? ex1 - ex0 + line_pad : total;
    bool ok = false;
    float a = 0.0f, b = 0.0f;
    float auto_inset = deco_thickness_auto(fs, 0.0f); /* `auto`: the UA trims the ends by one line thickness */
    a = auto_inset;
    b = auto_inset;
    if (vs->kind != EMBI_V_KEYWORD) {
        a = embi_pb_len(p, vs, base, fs, &ok);
        a = ok ? a : 0.0f;
    }
    if (ve->kind != EMBI_V_KEYWORD) {
        b = embi_pb_len(p, ve, base, fs, &ok);
        b = ok ? b : 0.0f;
    }
    bool rtl = embi_pb_kw(ds, EMBI_PROP_direction) == EMBI_KW_rtl;
    bool start_on = clone || line == first_line, end_on = clone || line == last_line;
    float left = rtl ? b : a, right = rtl ? a : b;
    bool left_on = rtl ? end_on : start_on, right_on = rtl ? start_on : end_on;
    float x0 = *x, x1 = *x + *w;
    if (!clone && before >= 0.0f) {
        /* slice: a positive inset larger than a line's fragment carries into the next lines (from either end) */
        float after = total - before - (ex1 - ex0 + line_pad);
        float cut_s = (rtl ? b : a) - before, cut_e = (rtl ? a : b) - after;
        if (!start_on && cut_s > 0.0f) {
            if (rtl) {
                x1 = fminf(x1, ex1 - cut_s);
            } else {
                x0 = fmaxf(x0, ex0 + cut_s);
            }
        }
        if (!end_on && cut_e > 0.0f) {
            if (rtl) {
                x0 = fmaxf(x0, ex0 + cut_e);
            } else {
                x1 = fminf(x1, ex1 - cut_e);
            }
        }
    }
    if (left_on) {
        if (left >= 0.0f) {
            x0 = fmaxf(x0, ex0 + left);
        } else if (x0 <= ex0 + 0.01f) {
            x0 += left;
        }
    }
    if (right_on) {
        if (right >= 0.0f) {
            x1 = fminf(x1, ex1 - right);
        } else if (x1 >= ex1 - 0.01f) {
            x1 -= right;
        }
    }
    *x = x0;
    *w = x1 > x0 ? x1 - x0 : 0.0f;
}

/* How far the dotted or dashed pattern of the decoration of box D has advanced when it reaches x (page px) in the text
 * fragment f: the distance from the start of D's first text fragment on the line, so one decorating box draws one
 * continuous pattern across its bidi runs. 0 when f is not on a line. */
static float deco_phase(const embi_frag *f, const embi_box *D, float rect_x, float x, float xend, float *more)
{
    if (f == nullptr || D == nullptr || f->parent == nullptr || f->parent->kind != EMBI_FK_LINE) {
        return 0.0f;
    }
    float lo = 1e30f, hi = -1e30f;
    for (const embi_frag *c = embi_frag_first(f->parent); c != nullptr; c = c->next) {
        if (c->kind == EMBI_FK_TEXT && c->box != nullptr && box_in(c->box, D)) {
            lo = fminf(lo, c->x);
            hi = fmaxf(hi, c->x + c->w);
        }
    }
    float start = rect_x - f->x + lo;
    *more = xend < rect_x - f->x + hi - 0.01f ? 1.0f : 0.0f;
    return x > start ? x - start : 0.0f;
}

/* The length in bytes of the space or spacer character at i in [i, end) of the shaped text s (text-decoration-skip-spaces:
 * spaces, tabs, no-break spaces and the fixed width spaces U+2000-200A, U+202F, U+205F, U+3000, U+1680), else 0. */
static uint32_t deco_space_len(const embi_lay_shaped *s, uint32_t i, uint32_t end)
{
    unsigned char c0 = (unsigned char)embi_tshim_byte(s, i);
    if (c0 == 0x20 || c0 == 0x09) {
        return 1;
    }
    if (c0 == 0xC2 && i + 1 < end && (unsigned char)embi_tshim_byte(s, i + 1) == 0xA0) {
        return 2;
    }
    if (i + 2 < end) {
        unsigned char c1 = (unsigned char)embi_tshim_byte(s, i + 1), c2 = (unsigned char)embi_tshim_byte(s, i + 2);
        if ((c0 == 0xE1 && c1 == 0x9A && c2 == 0x80) || (c0 == 0xE2 && c1 == 0x80 && ((c2 >= 0x80 && c2 <= 0x8A) || c2 == 0xAF)) ||
            (c0 == 0xE2 && c1 == 0x81 && c2 == 0x9F) || (c0 == 0xE3 && c1 == 0x80 && c2 == 0x80)) {
            return 3;
        }
    }
    return 0;
}

/* The bytes [*lo, *hi) of the fragment c's text that are not leading or trailing spaces; false when it is all spaces. */
static bool deco_ink_range(const embi_frag *c, uint32_t *lo, uint32_t *hi)
{
    const embi_lay_shaped *s = c->box != nullptr ? c->box->lt : nullptr;
    if (s == nullptr) {
        return false;
    }
    uint32_t b = embi_frag_tbegin(c), e = embi_frag_tend(c), i = b, last = b;
    bool have = false;
    while (i < e) {
        uint32_t n = deco_space_len(s, i, e);
        if (n == 0) {
            n = 1;
            while (i + n < e && ((unsigned char)embi_tshim_byte(s, i + n) & 0xC0) == 0x80) {
                n++;
            }
            if (!have) {
                *lo = i;
                have = true;
            }
            last = i + n;
        }
        i += n;
    }
    *hi = last;
    return have;
}

typedef struct deco_seg {
    float x0, x1;
} deco_seg;

#define DECO_SEG_MAX 24

/* The segments of [*x0, *x1] (page px) of the fragment f that text-decoration-skip-spaces leaves decorated. The initial
 * value `start end` skips the spaces and spacers at the start and the end of the line; `all` skips every one (and the
 * letter-spacing after the last character of a run); `none` skips nothing. */
static uint32_t deco_skip_spaces(const embi_pst *p, const embi_frag *f, const embi_box *b, float rect_x, float x0, float x1,
                                 deco_seg out[DECO_SEG_MAX])
{
    out[0].x0 = x0;
    out[0].x1 = x1;
    if (f == nullptr || f->parent == nullptr || f->parent->kind != EMBI_FK_LINE || b->lt == nullptr) {
        return 1;
    }
    bool skip_start = true, skip_end = true, all = false;
    const embi_value *mv = embi_style_value(b->style, EMBI_PROP_text_decoration_skip_spaces);
    if (mv != nullptr) {
        uint32_t n = 0;
        const embi_value *items = embi_value_items(mv, &n);
        bool st = false, en = false, any = false;
        for (uint32_t i = 0; i < n; i++) {
            if (items[i].kind != EMBI_V_KEYWORD) {
                continue;
            }
            any = true;
            st = st || items[i].kw == EMBI_KW_start;
            en = en || items[i].kw == EMBI_KW_end;
            all = all || items[i].kw == EMBI_KW_all;
        }
        if (any) {
            skip_start = st;
            skip_end = en;
        }
    }
    if (!all && !skip_start && !skip_end) {
        return 1;
    }
    uint32_t b0 = embi_frag_tbegin(f), b1 = embi_frag_tend(f);
    if (all) {
        float fs = embi_pb_fontsize(b->style), ls = 0.0f;
        const embi_value *lv = embi_style_value(b->style, EMBI_PROP_letter_spacing);
        bool ok = false;
        if (lv != nullptr && lv->kind != EMBI_V_KEYWORD) {
            ls = embi_pb_len(p, lv, fs, fs, &ok);
            ls = ok ? ls : 0.0f;
        }
        uint32_t ns = 0, i = b0;
        while (i < b1 && ns < DECO_SEG_MAX) {
            while (i < b1 && deco_space_len(b->lt, i, b1) != 0) {
                i += deco_space_len(b->lt, i, b1);
            }
            if (i >= b1) {
                break;
            }
            uint32_t j = i;
            while (j < b1 && deco_space_len(b->lt, j, b1) == 0) {
                j++;
            }
            float sx0 = rect_x + embi_tshim_measure(b->lt, b0, i);
            float sx1 = rect_x + embi_tshim_measure(b->lt, b0, j) - (ls > 0.0f ? ls : 0.0f);
            sx0 = fmaxf(sx0, x0);
            sx1 = fminf(sx1, x1);
            if (sx1 > sx0) {
                out[ns].x0 = sx0;
                out[ns].x1 = sx1;
                ns++;
            }
            i = j;
        }
        return ns;
    }
    bool before = false, after = false, seen = false;
    for (const embi_frag *c = embi_frag_first(f->parent); c != nullptr; c = c->next) {
        if (c == f) {
            seen = true;
            continue;
        }
        bool ink = false;
        if (c->kind == EMBI_FK_TEXT && c->box != nullptr) {
            uint32_t lo, hi;
            ink = deco_ink_range(c, &lo, &hi);
        } else if (c->kind == EMBI_FK_BOX && c->inl == 0) {
            ink = true; /* an atomic inline or other content */
        }
        if (ink) {
            if (seen) {
                after = true;
            } else {
                before = true;
            }
        }
    }
    uint32_t lo, hi;
    if (!deco_ink_range(f, &lo, &hi)) {
        bool skip = (!before && skip_start) || (!after && skip_end);
        return skip ? 0 : 1;
    }
    if (!before && skip_start && lo > b0) {
        out[0].x0 = fmaxf(out[0].x0, rect_x + embi_tshim_measure(b->lt, b0, lo));
    }
    if (!after && skip_end && hi < b1) {
        float d = embi_tshim_measure(b->lt, hi, b1);
        out[0].x1 = fminf(out[0].x1, rect_x + (embi_tshim_measure(b->lt, b0, b1) - d));
    }
    return out[0].x1 > out[0].x0 ? 1 : 0;
}

typedef struct deco_ent {
    uint8_t line; /* 0 underline, 1 overline, 2 line-through */
    const embi_style *style;
    const embi_box *box;
} deco_ent;

#define DECO_MAX 16

/* A solid, opaque decoration line that starts exactly where a recent line of the same color, position and thickness ends (the
 * lines of adjacent spans) makes that earlier line one pixel longer, over the start of its own extent, so the antialiased
 * edges of the two fractional rects do not leave a seam between them. The earlier line is emitted before the text of the later
 * span, so that text still paints over the extension. */
static void deco_overlap_prev(embi_pst *p, float x, float y, float h, uint8_t style, uint32_t color, uint32_t xf, uint32_t clip)
{
    if (style != EMBI_DECO_SOLID || (color >> 24) != 0xFFu) {
        return;
    }
    uint32_t n = (uint32_t)p->d->items.len;
    for (uint32_t k = 0; k < 4 && k < n; k++) {
        embi_ditem *q = &p->d->items.data[n - 1 - k];
        if (q->kind != EMBI_DI_DECORATION) {
            continue;
        }
        if (q->sub == style && q->colors[0] == color && q->xform == xf && q->clip == clip && q->rect[1] == y &&
            q->rect[3] == h && fabsf(q->rect[0] + q->rect[2] - x) < 0.001f) {
            q->rect[2] += 1.0f;
            return;
        }
    }
}


/* Draws the decoration lines of one run: one entry per decorating box and line type (CSS Text Decoration 3 section 2.1: the
 * lines of an ancestor and of a descendant are all drawn, on top of each other). The metrics are the run's font (b->lt);
 * each line's size comes from its own style. */
static void emit_decorations(embi_pst *p, const embi_frag *f, const embi_box *b, const deco_ent *ents, uint32_t nents,
                             uint32_t which, const float rect[4], float baseline, uint32_t xf, uint32_t clip, float dx, float dy,
                             bool has_color, uint32_t color_override)
{
    float fs = embi_pb_fontsize(b->style);
    float ul_pos = -0.1f * fs, ul_thick = 0.0f, st_pos = 0.3f * fs, st_size = 0.0f;
    bool have = embi_tshim_deco_metrics(b->lt, &ul_pos, &ul_thick, &st_pos, &st_size);
    if (!have) {
        ul_pos = -0.1f * fs;
        ul_thick = 0.0f;
        st_pos = 0.3f * fs;
        st_size = 0.0f;
    }
    for (uint32_t ei = nents; ei-- > 0;) { /* ancestors first, so a descendant's line is on top */
        int line = ents[ei].line;
        if ((which & (1u << line)) == 0) {
            continue;
        }
        const embi_style *ds = ents[ei].style;
        const embi_box *dbox = ents[ei].box;
        float fs_d = embi_pb_fontsize(ds);
        /* The position and thickness come from the decorating box's font: a smaller or larger font of a descendant (a
         * superscript) does not move the line. Scaled by the font sizes, the family being the same. */
        float km = 1.0f;
        if (fs > 0.0f && dbox != nullptr) {
            /* An inline decorating box uses its own font; a block's line is drawn by the anonymous inline box of the block that
             * holds the text, so that block's font gives the metrics. */
            float fm = fs_d;
            if (dbox->kind != EMBI_BK_INLINE) {
                const embi_box *q = b->parent;
                while (q != nullptr && (q->kind == EMBI_BK_INLINE || q->kind == EMBI_BK_TEXT)) {
                    q = q->parent;
                }
                fm = q != nullptr && q->style != nullptr ? embi_pb_fontsize(q->style) : fs;
            }
            km = fm / fs;
        }
        float x = rect[0], w = rect[2];
        deco_inset(p, f, dbox, rect[0], &x, &w);
        deco_seg segs[DECO_SEG_MAX];
        uint32_t nseg = w > 0.0f ? deco_skip_spaces(p, f, b, rect[0], x, x + w, segs) : 0;
        if (nseg == 0) {
            continue;
        }
        float top, t;
        if (line == 0) {
            /* underline: the font's underline position (the top edge, below the baseline), or below the descenders
             * with `under`; text-underline-offset moves it further down. */
            float base = -ul_pos * km;
            if (underline_under(ds) && base < 0.2f * fs_d) {
                base = 0.2f * fs_d;
            }
            t = deco_thickness(p, ds, fs_d, have ? ul_thick * km : 0.0f);
            if (underline_offset_set(ds) && embi_pb_kw(ds, EMBI_PROP_text_underline_position) != EMBI_KW_from_font) {
                /* a length offset is measured from the alphabetic baseline (CSS Text Decoration 4 section 3.5); with
                 * from-font it is measured from the font's underline position */
                base = underline_under(ds) && 0.2f * fs_d > 0.0f ? 0.2f * fs_d : 0.0f;
            }
            top = baseline + base + underline_offset(p, ds, fs_d);
        } else if (line == 1) {
            /* overline: its bottom edge is where the default-thickness line ends, so a thicker line grows upward. */
            float t0 = deco_thickness_auto(fs_d, have ? ul_thick * km : 0.0f);
            t = deco_thickness(p, ds, fs_d, have ? ul_thick * km : 0.0f);
            top = rect[1] + t0 - t;
        } else {
            /* line-through: centered on the font's strikeout stroke, so a thicker line grows both ways. */
            float t0 = deco_thickness_auto(fs_d, have ? st_size * km : 0.0f);
            t = deco_thickness(p, ds, fs_d, have ? st_size * km : 0.0f);
            top = baseline - (have ? st_pos * km : 0.3f * fs_d + t0 * 0.5f) + (t0 - t) * 0.5f;
        }
        /* Decoration lines are snapped to whole device pixels (position and thickness), so they are not blurred by
         * antialiasing and overlapping lines add up like the browsers' do. */
        {
            float t1 = roundf(t);
            t = t1 >= 1.0f ? t1 : 1.0f;
            top = roundf(top + dy) - dy;
        }
        uint32_t c = has_color ? color_override : deco_color(ds);
        for (uint32_t si = 0; si < nseg; si++) {
            float sx0 = segs[si].x0 + dx, sw = segs[si].x1 - segs[si].x0;
            deco_overlap_prev(p, sx0, top + dy, t, deco_style_of(ds), c, xf, clip);
            embi_ditem *dl = embi_pb_item(p, EMBI_DI_DECORATION, xf, clip);
            dl->rect[0] = sx0;
            dl->rect[1] = top + dy;
            dl->rect[2] = sw;
            dl->rect[3] = t;
            dl->sub = deco_style_of(ds);
            dl->colors[0] = c;
            float more = 0.0f;
            dl->f[0] = deco_phase(f, dbox, rect[0], segs[si].x0, segs[si].x1, &more);
            dl->f[1] = more;
        }
    }
}

/* One text item: the run's bytes [tb, te) with their baseline origin moved by (dx, dy). */
static void emit_text_item(embi_pst *p, const float rect[4], uint32_t ri, uint32_t tb, uint32_t te, float baseline,
                           uint32_t color, uint32_t xf, uint32_t clip, float dx, float dy)
{
    embi_ditem *it = embi_pb_item(p, EMBI_DI_TEXT, xf, clip);
    memcpy(it->rect, rect, sizeof it->rect);
    it->rect[0] += dx;
    it->rect[1] += dy;
    it->aux = ri;
    it->aux2 = te;
    it->aux3 = tb;
    embi_ditem_set_chash(it, embi_tshim_content_hash((const embi_lay_shaped *)p->d->runs.data[ri], tb, te));
    it->f[0] = rect[0] + dx;
    it->f[1] = baseline + dy;
    it->colors[0] = color;
}

/* -webkit-text-stroke (Compatibility section 6): a stroke along the glyph outlines, approximated by eight copies of the text in
 * the stroke color moved by half the stroke width (in the four axis and four diagonal directions), drawn under the fill. The
 * width keywords are 1, 3 and 5 px, as in browsers. */
static void emit_text_stroke(embi_pst *p, const embi_style *s, const float rect[4], uint32_t ri, uint32_t tb, uint32_t te,
                             float baseline, uint32_t color, uint32_t xf, uint32_t clip)
{
    const embi_value *wv = embi_style_value(s, EMBI_PROP__webkit_text_stroke_width);
    if (wv == nullptr) {
        return;
    }
    float w = 0.0f;
    if (wv->kind == EMBI_V_KEYWORD) {
        w = wv->kw == EMBI_KW_thin ? 1.0f : wv->kw == EMBI_KW_medium ? 3.0f : wv->kw == EMBI_KW_thick ? 5.0f : 0.0f;
    } else {
        bool ok = false;
        float fs = embi_pb_fontsize(s);
        w = embi_pb_len(p, wv, fs, fs, &ok);
        w = ok ? w : 0.0f;
    }
    if (!(w > 0.0f)) {
        return;
    }
    uint32_t c = color;
    if (!embi_pb_color(s, embi_style_value(s, EMBI_PROP__webkit_text_stroke_color), &c)) {
        c = color;
    }
    if ((c >> 24) == 0) {
        return;
    }
    const float h = 0.5f * w, k = 0.70710678f * h;
    const float d[8][2] = {{-h, 0.0f}, {h, 0.0f}, {0.0f, -h}, {0.0f, h}, {-k, -k}, {k, -k}, {-k, k}, {k, k}};
    for (uint32_t i = 0; i < 8; i++) {
        emit_text_item(p, rect, ri, tb, te, baseline, c, xf, clip, d[i][0], d[i][1]);
    }
}

/* ::first-line (CSS Pseudo-Elements 4 section 3): the color of the first formatted line of a block container. The text of a
 * fragment that sits on the first line of its block (or of a block that starts an ancestor block, whose first line it is too)
 * takes the color of the nearest ::first-line style, unless its own element sets a color of its own (its color differs from
 * the block's). Only the color is applied; properties that change the line metrics (font-size, ...) are not (a known gap). */
static bool first_line_color(const embi_pst *p, const embi_frag *f, uint32_t own, uint32_t *out)
{
    if (p->se == nullptr) {
        return false;
    }
    const embi_frag *line = f->parent;
    while (line != nullptr && line->kind != EMBI_FK_LINE) {
        line = line->parent;
    }
    if (line == nullptr || line->parent == nullptr) {
        return false;
    }
    const embi_frag *blk = line->parent;
    for (const embi_frag *k = embi_frag_first(blk); k != nullptr && k != line; k = k->next) {
        if (k->kind == EMBI_FK_LINE) {
            return false; /* an earlier line */
        }
    }
    for (const embi_frag *q = blk; q != nullptr && q->kind == EMBI_FK_BOX && q->box != nullptr; q = q->parent) {
        if (q->box->el != nullptr && (q->box->flags & EMBI_BF_ANON) == 0 && q->box->kind == EMBI_BK_BLOCK) {
            const embi_style *fs = embi_style_generated(p->se, q->box->el, EMBI_PE_FIRST_LINE);
            if (fs != nullptr) {
                const embi_value *cv = embi_style_value(fs, EMBI_PROP_color);
                uint32_t c = 0, bc = 0;
                if (cv != nullptr && embi_pb_color(fs, cv, &c)) {
                    const embi_style *bs = q->box->style;
                    if (bs != nullptr && embi_pb_color(bs, embi_style_value(bs, EMBI_PROP_color), &bc) && bc != own) {
                        return false; /* the text's element sets its own color */
                    }
                    *out = c;
                    return true;
                }
            }
        }
        /* The first line passes only through in-flow block containers: not absolutely positioned boxes, atomic inlines, tables,
         * flex or grid containers (their items are formatted on their own), and a float only when it is inside an inline
         * box (which is on the first line and passes its first-line style on to it). */
        if ((q->box->flags & (EMBI_BF_ABSPOS | EMBI_BF_ATOMIC)) != 0 || q->box->kind != EMBI_BK_BLOCK ||
            ((q->box->flags & EMBI_BF_FLOAT) != 0 && (q->box->parent == nullptr || q->box->parent->kind != EMBI_BK_INLINE))) {
            break;
        }
        const embi_frag *par = q->parent;
        if (par == nullptr || par->kind != EMBI_FK_BOX || par->box == nullptr || par->box->kind != EMBI_BK_BLOCK) {
            break;
        }
        const embi_frag *k = embi_frag_first(par);
        while (k != nullptr && k != q && k->kind == EMBI_FK_BOX && k->box != nullptr &&
               (k->box->flags & (EMBI_BF_FLOAT | EMBI_BF_ABSPOS)) != 0) {
            k = k->next;
        }
        if (k != q) {
            break; /* q does not start its parent */
        }
    }
    return false;
}

void embi_pb_text(embi_pst *p, const embi_frag *f, const embi_box *b, const float rect[4], float baseline, float line_baseline,
                  uint32_t xf, uint32_t clip)
{
    const embi_style *s = b->style;
    if (s == nullptr || b->lt == nullptr || embi_pb_hidden(s)) {
        return;
    }
    uint32_t tb = embi_frag_tbegin(f), te = embi_frag_tend(f);
    if (te <= tb) {
        return;
    }
    uint32_t color = BLACK_RGBA;
    if (!embi_pb_color(s, embi_style_value(s, EMBI_PROP_color), &color)) {
        color = BLACK_RGBA;
    }
    {
        /* -webkit-text-fill-color: the fill of the glyphs; the decorations keep the text color. */
        uint32_t fill = color;
        const embi_value *fv = embi_style_value(s, EMBI_PROP__webkit_text_fill_color);
        if (fv != nullptr && embi_pb_color(s, fv, &fill)) {
            color = fill;
        }
    }
    if (p->se != nullptr) {
        uint32_t fc = 0;
        if (first_line_color(p, f, color, &fc)) {
            color = fc;
        }
    }
    uint32_t ri = p->d->runs.len;
    embi_vec_push(&p->d->runs, (const void *)b->lt);
    const embi_value *shv = embi_style_value(s, EMBI_PROP_text_shadow);
    bool shadows = shv != nullptr && shv->kind == EMBI_V_SHADOWS && shv->shadows.n > 0;

    /* The decorating boxes: the text's own box and its ancestors up to a float, an absolutely positioned box or an
     * atomic inline (included: it decorates its own content, but the decorations of its ancestors are not propagated
     * into it). Decorations propagate to every in-flow block descendant too (CSS Text Decoration 3 section 2.1). Each
     * line takes the nearest box that sets it. */
    uint32_t mask = 0;
    deco_ent ents[DECO_MAX];
    uint32_t nents = 0;
    bool own = true; /* every decorating box so far is the text's own element (or the text box): the shifted baseline */
    for (const embi_box *q = b; q != nullptr; q = q->parent) {
        if (q->style != nullptr) {
            uint32_t m = deco_mask(q->style);
            for (int line = 0; line < 3; line++) {
                bool dup = false;
                for (uint32_t k = 0; k < nents; k++) {
                    dup = dup || (ents[k].line == line && ents[k].style == q->style);
                }
                if ((m & (1u << line)) != 0 && !dup && nents < DECO_MAX) {
                    /* a text box shares its element's style: the decorating box is the element */
                    ents[nents].line = (uint8_t)line;
                    ents[nents].style = q->style;
                    ents[nents].box = q->kind == EMBI_BK_TEXT && q->parent != nullptr && q->parent->style == q->style ? q->parent : q;
                    nents++;
                }
            }
            if (m != 0 && q != b && q != b->parent) {
                own = false;
            }
            mask |= m;
        }
        if ((q->flags & (EMBI_BF_FLOAT | EMBI_BF_ABSPOS | EMBI_BF_ATOMIC)) != 0) {
            break;
        }
    }
    float deco_base = own ? baseline : line_baseline;
    if (shadows) {
        /* text-shadow (CSS Text Decoration 3 section 4.1): the first shadow is on top, so they are drawn last to first, and
         * all of them below the text and its decorations. Each shadow is the text and its decoration lines in the shadow's
         * color, moved by its offset; a blur is a filtered group (effects.c). */
        for (uint32_t k = shv->shadows.n; k-- > 0;) {
            const embi_shadow *sh = &shv->shadows.p[k];
            uint32_t sc = color;
            if (!embi_pb_color(s, &sh->color, &sc)) {
                sc = color;
            }
            if ((sc >> 24) == 0) {
                continue;
            }
            float fs = embi_pb_fontsize(s);
            bool ok = false;
            float ox = embi_pb_len(p, &sh->x, fs, fs, &ok);
            ox = ok ? ox : 0.0f;
            float oy = embi_pb_len(p, &sh->y, fs, fs, &ok);
            oy = ok ? oy : 0.0f;
            float blur = embi_pb_len(p, &sh->blur, fs, fs, &ok);
            blur = ok && blur > 0.0f ? blur : 0.0f;
            float sigma = blur * 0.5f;
            if (sigma < 0.4f) {
                if (mask != 0) {
                    emit_decorations(p, f, b, ents, nents, 3u, rect, deco_base, xf, clip, ox, oy, true, sc);
                }
                emit_text_item(p, rect, ri, tb, te, baseline, sc, xf, clip, ox, oy);
                if (mask != 0) {
                    emit_decorations(p, f, b, ents, nents, 4u, rect, deco_base, xf, clip, ox, oy, true, sc);
                }
                continue;
            }
            /* A real Gaussian blur (R3a): the shadow's text and decoration lines are one filtered group, which replay
             * renders into a layer and blurs with sigma = blur radius / 2. */
            embi_dfx op;
            memset(&op, 0, sizeof op);
            op.kind = EMBI_FX_BLUR;
            op.sigma = sigma;
            uint32_t first = p->d->fx.len;
            embi_vec_push(&p->d->fx, op);
            uint32_t gb = embi_pb_fx_begin(p, first, 1, 1.0f, xf, clip);
            if (mask != 0) {
                emit_decorations(p, f, b, ents, nents, 3u, rect, deco_base, xf, clip, ox, oy, true, sc);
            }
            emit_text_item(p, rect, ri, tb, te, baseline, sc, xf, clip, ox, oy);
            if (mask != 0) {
                emit_decorations(p, f, b, ents, nents, 4u, rect, deco_base, xf, clip, ox, oy, true, sc);
            }
            embi_pb_fx_end(p, gb);
        }
    }
    /* Underlines and overlines are painted below the text, line-through above it (CSS Text Decoration 3 section 3). */
    if (mask != 0) {
        emit_decorations(p, f, b, ents, nents, 3u, rect, deco_base, xf, clip, 0.0f, 0.0f, false, 0);
    }
    emit_text_stroke(p, s, rect, ri, tb, te, baseline, color, xf, clip);
    emit_text_item(p, rect, ri, tb, te, baseline, color, xf, clip, 0.0f, 0.0f);
    {
        /* Glyphs that overlap each other (letter-spacing below -0.1em; a smaller overlap is hairline and folds) must be composited as a group by an opacity layer, which
         * would otherwise fold the alpha into the one text item and darken the overlaps (opacity-overlapping-letters). */
        const embi_value *lsv = embi_style_value(s, EMBI_PROP_letter_spacing);
        bool ok = false;
        float fs = embi_pb_fontsize(s);
        if (lsv != nullptr && lsv->kind != EMBI_V_KEYWORD && embi_pb_len(p, lsv, fs, fs, &ok) < -0.1f * fs && ok) {
            p->d->items.data[p->d->items.len - 1].flags |= EMBI_DF_NOFOLD;
        }
    }
    if (mask != 0) {
        emit_decorations(p, f, b, ents, nents, 4u, rect, deco_base, xf, clip, 0.0f, 0.0f, false, 0);
    }
}

/* ------------------------------------------------------------------ list markers as shapes */

enum {
    MK_NONE = 0,
    MK_DISC,
    MK_CIRCLE,
    MK_SQUARE,
    MK_OPEN,   /* disclosure-open: a triangle pointing down */
    MK_CLOSED, /* disclosure-closed: pointing right, or left in rtl */
};

static bool str_is(embi_str s, const char *lit)
{
    size_t n = strlen(lit);
    return s.len == n && memcmp(s.ptr, lit, n) == 0;
}

static uint8_t marker_shape_of(const embi_style *s)
{
    const embi_value *v = embi_style_value(s, EMBI_PROP_list_style_type);
    if (v == nullptr) {
        return MK_NONE;
    }
    if (v->kind == EMBI_V_KEYWORD) {
        switch (v->kw) {
        case EMBI_KW_disc:
            return MK_DISC;
        case EMBI_KW_circle:
            return MK_CIRCLE;
        case EMBI_KW_square:
            return MK_SQUARE;
        default:
            return MK_NONE;
        }
    }
    if (v->kind == EMBI_V_IDENT) {
        embi_str n = embi_atom_str(v->ident);
        if (str_is(n, "disclosure-open")) {
            return MK_OPEN;
        }
        if (str_is(n, "disclosure-closed")) {
            return MK_CLOSED;
        }
    }
    return MK_NONE;
}

bool embi_pb_marker_shape(embi_pst *p, const embi_frag *f, const embi_box *b, const float rect[4], float baseline,
                          uint32_t xf, uint32_t clip)
{
    if (b->kind != EMBI_BK_MARKER || b->style == nullptr) {
        return false;
    }
    uint8_t shape = marker_shape_of(b->style);
    if (shape == MK_NONE) {
        return false;
    }
    /* A marker's shape is drawn once, by its first fragment; the other fragments (none in practice) paint nothing. */
    if (embi_frag_tbegin(f) != 0 || embi_pb_hidden(b->style)) {
        return true;
    }
    uint32_t color = BLACK_RGBA;
    if (!embi_pb_color(b->style, embi_style_value(b->style, EMBI_PROP_color), &color)) {
        color = BLACK_RGBA;
    }
    /* Chromium's bullet size: a third of the ascent, rounded (LayoutListMarker). The ascent is 0.905 em (Arial, the
     * default face); the size is the same for every shape. */
    float fs = embi_pb_fontsize(b->style);
    float asc = floorf(fs * 0.905f + 0.5f);
    float d = floorf((asc * 2.0f / 3.0f + 1.0f) / 2.0f);
    if (!(d >= 1.0f)) {
        d = 1.0f;
    }
    /* The marker's glyph area ends with one space: the shape sits before it, centered on the x-height. */
    float space = fs * 0.25f;
    float right = rect[0] + rect[2] - space;
    float left = right - d;
    if (left < rect[0]) {
        left = rect[0];
    }
    float cy = baseline - fs * 0.3f;
    float top = cy - d * 0.5f;
    if (shape == MK_DISC || shape == MK_SQUARE) {
        float box[4] = {left, top, d, d};
        float radii[4] = {0, 0, 0, 0};
        if (shape == MK_DISC) {
            radii[0] = radii[1] = radii[2] = radii[3] = d * 0.5f;
        }
        (void)embi_pb_fill(p, box, radii, color, xf, clip);
    } else if (shape == MK_CIRCLE) {
        embi_ditem *it = embi_pb_item(p, EMBI_DI_BORDER, xf, clip);
        it->rect[0] = left;
        it->rect[1] = top;
        it->rect[2] = d;
        it->rect[3] = d;
        it->radii[0] = it->radii[1] = it->radii[2] = it->radii[3] = d * 0.5f;
        for (int i = 0; i < 4; i++) {
            it->widths[i] = 1.0f;
            it->colors[i] = color;
            it->styles[i] = EMBI_DS_SOLID;
        }
    } else {
        /* A disclosure triangle, about half the em, centered where the bullet is. */
        float tw = fs * 0.5f;
        float cx = left + d * 0.5f;
        float box[4] = {cx - tw * 0.5f, cy - tw * 0.5f, tw, tw};
        uint8_t dir = EMBI_TRI_DOWN;
        if (shape == MK_CLOSED) {
            const embi_value *dv = embi_style_value(b->style, EMBI_PROP_direction);
            bool rtl = dv != nullptr && dv->kind == EMBI_V_KEYWORD && dv->kw == EMBI_KW_rtl;
            dir = rtl ? EMBI_TRI_LEFT : EMBI_TRI_RIGHT;
        }
        embi_pb_triangle(p, box, dir, color, xf, clip);
    }
    return true;
}
