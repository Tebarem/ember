/*
 * Canvas text (R4b): shaping through the text system, alignment, baselines, metrics and the glyph outlines of strokeText.
 */
#include "canvas/canvas_int.h"

#include "font/font.h"
#include "text/text.h"

#include <string.h>

struct ctext_layout {
    embi_text *t;
    embi_text_layout lay;
    embi_text_vruns vr;
    bool have_vr;
    embi_str *fams;
    uint32_t nfams;
    char *famtext;
    size_t famlen;
    float size;
    double advance;
    bool rtl;
    /* The primary face's metrics in px */
    double ascent, descent, hanging;
    double em_ascent, em_descent; /* the em box split in the proportion of the font ascent and descent */
    const embi_font *font;
    char *text;
    uint32_t text_len;
};

static void split_families(ctext_layout *l, const char *family)
{
    size_t n = strlen(family);
    l->famlen = n + 1;
    l->famtext = embi_alloc(l->famlen);
    memcpy(l->famtext, family, n + 1);
    uint32_t count = 1;
    for (size_t i = 0; i < n; i++) {
        if (family[i] == ',') {
            count++;
        }
    }
    l->fams = embi_alloc(count * sizeof *l->fams);
    uint32_t k = 0;
    char *p = l->famtext;
    while (k < count) {
        char *c = strchr(p, ',');
        size_t len = c != nullptr ? (size_t)(c - p) : strlen(p);
        l->fams[k++] = (embi_str){p, (uint32_t)len};
        if (c == nullptr) {
            break;
        }
        p = c + 1;
    }
    l->nfams = k;
}

ctext_layout *ctext_shape(embi_text *t, const ctextcmd *tc)
{
    if (t == nullptr) {
        return nullptr;
    }
    ctext_layout *l = embi_calloc(1, sizeof *l);
    l->t = t;
    split_families(l, tc->font.family != nullptr ? tc->font.family : "sans-serif");
    l->size = tc->font.px;
    l->rtl = tc->rtl;
    /* Whitespace characters become spaces. */
    l->text_len = tc->len;
    l->text = embi_alloc((size_t)tc->len + 1);
    for (uint32_t i = 0; i < tc->len; i++) {
        char c = tc->text[i];
        l->text[i] = (c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r') ? ' ' : c;
    }
    l->text[tc->len] = '\0';

    embi_text_style st;
    memset(&st, 0, sizeof st);
    st.families = l->fams;
    st.family_count = l->nfams;
    st.size = tc->font.px;
    st.weight = tc->font.weight;
    st.font_style = tc->font.style;
    st.stretch = tc->font.stretch;
    st.letter_spacing = (float)tc->letter_spacing;
    st.word_spacing = (float)tc->word_spacing;
    st.direction = tc->rtl ? EMBI_DIR_RTL : EMBI_DIR_LTR;
    st.kerning = tc->kerning == 1 ? EMBI_KERN_NORMAL : (tc->kerning == 2 ? EMBI_KERN_NONE : EMBI_KERN_AUTO);
    st.ligatures = tc->letter_spacing == 0.0;
    st.synth = EMBI_SYNTH_ALL;
    st.caps = (tc->font.small_caps || tc->caps == 1 || tc->caps == 3) ? EMBI_CAPS_SMALL
              : (tc->caps == 2 || tc->caps == 4)                       ? EMBI_CAPS_ALL_SMALL
              : tc->caps == 5                                          ? EMBI_CAPS_UNICASE
              : tc->caps == 6                                          ? EMBI_CAPS_TITLING
                                                                       : EMBI_CAPS_NORMAL;
    st.optical_sizing = true;
    if (!embi_text_shape(t, l->text, l->text_len, &st, &l->lay)) {
        ctext_layout_free(l);
        return nullptr;
    }
    if (l->text_len > 0) {
        l->have_vr = embi_text_line(&l->lay, 0, l->text_len, &l->vr);
    }
    l->advance = l->lay.advance;
    /* The primary face: metrics rounded to whole px, as the layout does. */
    embi_style_match m = {l->fams, l->nfams, tc->font.weight, tc->font.style, tc->font.stretch};
    embi_face *face = nullptr;
    if (embi_fontdb_candidates(embi_text_fonts(t), &m, &face, 1) > 0 && face != nullptr) {
        l->font = embi_face_font(face);
    }
    if (l->font != nullptr && l->font->metrics.units_per_em != 0) {
        const embi_font_metrics *fm = &l->font->metrics;
        double k = (double)tc->font.px / (double)fm->units_per_em;
        double asc = fm->use_typo_metrics ? fm->typo_ascender : fm->hhea_ascender;
        double desc = fm->use_typo_metrics ? -fm->typo_descender : -fm->hhea_descender;
        l->ascent = round(asc * k);
        l->descent = round(desc * k);
        l->hanging = l->ascent * 0.8;
        l->em_ascent = asc + desc > 0.0 ? (double)tc->font.px * asc / (asc + desc) : l->ascent;
        l->em_descent = (double)tc->font.px - l->em_ascent;
    } else {
        l->ascent = tc->font.px * 0.9;
        l->descent = tc->font.px * 0.2;
        l->hanging = l->ascent * 0.8;
        l->em_ascent = l->ascent;
        l->em_descent = l->descent;
    }
    return l;
}

void ctext_layout_free(ctext_layout *l)
{
    if (l == nullptr) {
        return;
    }
    if (l->have_vr) {
        embi_text_vruns_free(&l->vr);
    }
    embi_text_layout_free(&l->lay);
    embi_free(l->fams, l->nfams * sizeof *l->fams);
    embi_free(l->famtext, l->famlen);
    embi_free(l->text, (size_t)l->text_len + 1);
    embi_free(l, sizeof *l);
}

double ctext_advance(const ctext_layout *l)
{
    return l->advance;
}

/* How far above the alphabetic baseline the baseline of the style sits. */
static double baseline_up(const ctext_layout *l, const ctextcmd *tc)
{
    switch (tc->baseline) {
    case CV_BASE_TOP: return l->ascent;
    case CV_BASE_HANGING: return l->hanging;
    case CV_BASE_MIDDLE: return (l->ascent - l->descent) * 0.5;
    case CV_BASE_IDEOGRAPHIC: return -l->descent;
    case CV_BASE_BOTTOM: return -l->descent;
    default: return 0.0;
    }
}

/* The x offset of the start of the text from the anchor, for the alignment (start/end follow the direction). */
static double align_dx(const ctextcmd *tc, double w)
{
    switch (tc->align) {
    case CV_ALIGN_LEFT: return 0.0;
    case CV_ALIGN_RIGHT: return -w;
    case CV_ALIGN_CENTER: return -w * 0.5;
    case CV_ALIGN_END: return tc->rtl ? 0.0 : -w;
    default: return tc->rtl ? -w : 0.0;
    }
}

void ctext_origin(const ctext_layout *l, const ctextcmd *tc, double *ox, double *oy, double *sx)
{
    double w = l->advance;
    double k = 1.0;
    if (tc->max_width > 0.0 && w > tc->max_width) {
        k = tc->max_width / w;
        w = tc->max_width;
    }
    *sx = k;
    *ox = tc->x + align_dx(tc, w);
    *oy = tc->y + baseline_up(l, tc);
}

typedef struct bbox {
    double minx, maxx, miny, maxy;
    bool any;
} bbox;

static void glyph_walk(const ctext_layout *l, bbox *bb, embi_cpath *path, double ox, double oy, double sx)
{
    if (!l->have_vr) {
        return;
    }
    embi_outline ol;
    memset(&ol, 0, sizeof ol);
    for (uint32_t vi = 0; vi < l->vr.items.len; vi++) {
        const embi_text_vrun *v = &l->vr.items.data[vi];
        const embi_text_run *run = &l->lay.runs.data[v->run];
        const embi_font *font = run->face != nullptr ? embi_face_font(run->face) : nullptr;
        if (font == nullptr || font->metrics.units_per_em == 0) {
            continue;
        }
        double k = (double)run->size / (double)font->metrics.units_per_em;
        double pen = v->x;
        for (uint32_t gi = v->glyph_begin; gi < v->glyph_end; gi++) {
            const embi_text_glyph *g = &l->lay.glyphs.data[gi];
            if (embi_font_glyph_outline(font, g->gid, nullptr, &ol) && ol.quads.len > 0) {
                double gx = pen + g->x_offset, gy = g->y_offset;
                if (bb != nullptr) {
                    double x0 = gx + ol.xmin * k, x1 = gx + ol.xmax * k;
                    double y0 = gy + ol.ymin * k, y1 = gy + ol.ymax * k;
                    if (!bb->any) {
                        bb->minx = x0;
                        bb->maxx = x1;
                        bb->miny = y0;
                        bb->maxy = y1;
                        bb->any = true;
                    } else {
                        bb->minx = x0 < bb->minx ? x0 : bb->minx;
                        bb->maxx = x1 > bb->maxx ? x1 : bb->maxx;
                        bb->miny = y0 < bb->miny ? y0 : bb->miny;
                        bb->maxy = y1 > bb->maxy ? y1 : bb->maxy;
                    }
                }
                if (path != nullptr) {
                    uint32_t ci = 0;
                    bool open = false;
                    for (uint32_t qi = 0; qi < ol.quads.len; qi++) {
                        const embi_quad *q = &ol.quads.data[qi];
                        if (ci < ol.contours.len && ol.contours.data[ci] == qi) {
                            if (open) {
                                embi_cpath_close(path);
                            }
                            embi_cpath_move_to(path, nullptr, ox + (gx + q->p0.x * k) * sx, oy - (gy + q->p0.y * k));
                            ci++;
                            open = true;
                        }
                        embi_cpath_quad_to(path, nullptr, ox + (gx + q->p1.x * k) * sx, oy - (gy + q->p1.y * k),
                                           ox + (gx + q->p2.x * k) * sx, oy - (gy + q->p2.y * k));
                    }
                    if (open) {
                        embi_cpath_close(path);
                    }
                }
            }
            pen += g->advance;
        }
    }
    embi_outline_free(&ol);
}

void ctext_outline(const ctext_layout *l, embi_cpath *dst, double ox, double oy, double sx)
{
    glyph_walk(l, nullptr, dst, ox, oy, sx);
}

void ctext_metrics(const ctext_layout *l, const ctextcmd *tc, embi_text_metrics *m)
{
    memset(m, 0, sizeof *m);
    double w = l->advance;
    m->width = w;
    double b = baseline_up(l, tc);
    double dx = align_dx(tc, w);
    bbox bb;
    memset(&bb, 0, sizeof bb);
    glyph_walk(l, &bb, nullptr, 0, 0, 1.0);
    if (bb.any) {
        m->left = -(dx + bb.minx);
        m->right = dx + bb.maxx;
        m->ascent = bb.maxy - b;
        m->descent = b - bb.miny;
    } else {
        m->left = -dx;
        m->right = dx;
        m->ascent = -b;
        m->descent = b;
    }
    m->font_ascent = l->ascent - b;
    m->font_descent = l->descent + b;
    m->em_ascent = l->em_ascent - b;
    m->em_descent = l->em_descent + b;
    m->hanging = l->hanging - b;
    m->alphabetic = -b;
    m->ideographic = -l->descent - b;
}

void ctext_draw(const ctext_layout *l, void *frame, uint32_t color, uint32_t xform, uint32_t clip)
{
    if (!l->have_vr) {
        return;
    }
    embi_text_draw(l->t, frame, &l->lay, &l->vr, 0.0f, 0.0f, color, xform, clip);
}
