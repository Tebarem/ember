/*
 * Geometry queries (CSSOM View) over the fragment tree, and the fragment dump (W3a, section 2).
 * Positions in fragments are relative to the parent's border box, so the document position of a
 * fragment is the sum of the positions along its chain to the initial containing block.
 *
 * The functions here answer for one layout: they read the boxes of the last embi_layout_document
 * call, through the element's box slot. An element with no box (display: none, or not laid out
 * yet) gives false.
 */
#include "layout_int.h"

#include "css/props.h"

#include <math.h>
#include <string.h>

#include <SDL3/SDL_stdinc.h>

typedef struct embi_grect {
    float x0, y0, x1, y1;
} embi_grect;

/* The document position of a fragment's border-box origin. */
static void doc_origin(const embi_frag *f, float *x, float *y)
{
    float ox = 0, oy = 0;
    for (const embi_frag *p = f; p != nullptr; p = p->parent) {
        ox += p->x;
        oy += p->y;
    }
    *x = ox;
    *y = oy;
}

static embi_grect frag_rect(const embi_frag *f)
{
    float x = 0, y = 0;
    doc_origin(f, &x, &y);
    embi_grect r = {x, y, x + f->w, y + f->h};
    return r;
}

static void grow(embi_grect *u, const embi_grect *r, bool *any)
{
    if (!*any) {
        *u = *r;
        *any = true;
        return;
    }
    if (r->x0 < u->x0) {
        u->x0 = r->x0;
    }
    if (r->y0 < u->y0) {
        u->y0 = r->y0;
    }
    if (r->x1 > u->x1) {
        u->x1 = r->x1;
    }
    if (r->y1 > u->y1) {
        u->y1 = r->y1;
    }
}

/* Whether a fragment is one piece of the principal inline box of element el. An inline that is split
 * around block-level children has one box per piece, all of them with the same element. */
static bool is_inline_piece_of(const embi_frag *k, const embi_element *el)
{
    return k->kind == EMBI_FK_BOX && k->box != nullptr && k->box->el == el &&
           k->box->kind == EMBI_BK_INLINE && k->box->pe == 0;
}

/* Collects the fragments of the inline pieces of el below f, in tree order. */
static void collect(const embi_frag *f, const embi_element *el, embi_grect *u, bool *any,
                    uint32_t *count, embi_rectf *out, uint32_t cap)
{
    for (const embi_frag *k = f; k != nullptr; k = k->next) {
        if (is_inline_piece_of(k, el)) {
            embi_grect r = frag_rect(k);
            grow(u, &r, any);
            if (out != nullptr && *count < cap) {
                out[*count] = (embi_rectf){r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0};
            }
            (*count)++;
        }
        collect(embi_frag_first(k), el, u, any, count, out, cap);
    }
}

/* Collects the fragments of box b below f, in tree order. */
static void collect_box(const embi_frag *f, const embi_box *b, embi_grect *u, bool *any, uint32_t *count, embi_rectf *out,
                        uint32_t cap)
{
    for (const embi_frag *k = f; k != nullptr; k = k->next) {
        if (k->kind == EMBI_FK_BOX && k->box == b) {
            embi_grect r = frag_rect(k);
            grow(u, &r, any);
            if (out != nullptr && *count < cap) {
                out[*count] = (embi_rectf){r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0};
            }
            (*count)++;
        }
        collect_box(embi_frag_first(k), b, u, any, count, out, cap);
    }
}

/* The fragments of an element's principal box and its pieces: a block has one fragment. */
static uint32_t element_rects(const embi_layout_engine *e, const embi_element *el, embi_grect *u,
                              bool *any, embi_rectf *out, uint32_t cap)
{
    *any = false;
    uint32_t count = 0;
    if (el == nullptr || el->box == nullptr) {
        return 0;
    }
    const embi_box *box = (const embi_box *)el->box;
    const embi_frag *root = embi_layout_root(e, el->base.doc);
    if (root == nullptr) {
        return 0;
    }
    if (box->kind != EMBI_BK_INLINE && (box->tflags & EMBI_TF_SPLIT) != 0 && box->frag != nullptr) {
        /* A box that a multi-column container cut: one rectangle per column it crosses (CSSOM View: one per fragment). */
        collect_box(root, box, u, any, &count, out, cap);
        if (count != 0) {
            return count;
        }
    }
    if (box->kind != EMBI_BK_INLINE) {
        if (box->frag == nullptr) {
            return 0;
        }
        embi_grect r = frag_rect(box->frag);
        grow(u, &r, any);
        if (out != nullptr && cap > 0) {
            out[0] = (embi_rectf){r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0};
        }
        return 1;
    }
    collect(root, el, u, any, &count, out, cap);
    return count;
}

bool embi_layout_border_box(const embi_layout_engine *e, const embi_element *el, embi_rectf *out)
{
    embi_grect u = {0, 0, 0, 0};
    bool any = false;
    if (element_rects(e, el, &u, &any, nullptr, 0) == 0 || !any) {
        return false;
    }
    *out = (embi_rectf){u.x0, u.y0, u.x1 - u.x0, u.y1 - u.y0};
    return true;
}

uint32_t embi_layout_client_rects(const embi_layout_engine *e, const embi_element *el,
                                  embi_rectf *out, uint32_t cap)
{
    embi_grect u = {0, 0, 0, 0};
    bool any = false;
    return element_rects(e, el, &u, &any, out, cap);
}

/* The principal fragment of an element, or nullptr. */
static const embi_frag *principal(const embi_layout_engine *e, const embi_element *el)
{
    if (el == nullptr || el->box == nullptr) {
        return nullptr;
    }
    /* The box slot is shared by every engine that laid the document out: only an engine that holds
     * a layout of the document owns the boxes the slot points to. */
    if (embi_layout_root(e, el->base.doc) == nullptr) {
        return nullptr;
    }
    return ((const embi_box *)el->box)->frag;
}

/* The computed style of an element (nullptr when it has none). */
static const embi_style *estyle(const embi_layout_engine *e, const embi_element *el)
{
    return embi_style_get(e->style, (embi_element *)(uintptr_t)el, EMBI_PE_NONE);
}

static uint16_t kw_of(const embi_style *s, embi_prop_id p)
{
    const embi_value *v = s != nullptr ? embi_style_value(s, p) : nullptr;
    return v != nullptr && v->kind == EMBI_V_KEYWORD ? v->kw : 0;
}

static bool is_html_named(const embi_element *el, const char *lit)
{
    embi_str n = embi_atom_str(el->base.name);
    size_t len = strlen(lit);
    if (el->base.extra != EMBI_NS_HTML || n.len != len) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char ch = n.ptr[i];
        if (ch >= 'A' && ch <= 'Z') {
            ch = (char)(ch - 'A' + 'a');
        }
        if (ch != lit[i]) {
            return false;
        }
    }
    return true;
}

/* CSSOM View offsetParent: the nearest positioned ancestor; for a statically positioned element
 * also a td, th or table; and body. A fixed element has none. */
static const embi_element *offset_parent(const embi_layout_engine *e, const embi_element *el)
{
    const embi_style *es = estyle(e, el);
    if (es == nullptr) {
        return nullptr;
    }
    uint16_t pos = kw_of(es, EMBI_PROP_position);
    if (pos == EMBI_KW_fixed) {
        return nullptr;
    }
    bool el_static = pos == 0 || pos == EMBI_KW_static;
    for (const embi_node *n = el->base.parent; n != nullptr && embi_is_element(n); n = n->parent) {
        const embi_element *a = (const embi_element *)n;
        const embi_style *as = estyle(e, a);
        if (as == nullptr) {
            continue;
        }
        uint16_t apos = kw_of(as, EMBI_PROP_position);
        if (apos != 0 && apos != EMBI_KW_static) {
            return a;
        }
        if (el_static && (is_html_named(a, "td") || is_html_named(a, "th") || is_html_named(a, "table"))) {
            return a;
        }
        if (is_html_named(a, "body")) {
            return a;
        }
    }
    return nullptr;
}

bool embi_layout_offset(const embi_layout_engine *e, const embi_element *el,
                        const embi_element **parent, float *x, float *y, float *w, float *h)
{
    *parent = nullptr;
    const embi_frag *f = principal(e, el);
    if (f == nullptr) {
        return false;
    }
    float ex = 0, ey = 0;
    doc_origin(f, &ex, &ey);
    /* The size is the bounding box of all the pieces of an inline box (CSSOM View offsetWidth),
     * snapped against the rounded edges: round(pos + size) - round(pos). */
    float bw = f->w, bh = f->h;
    if (f->box != nullptr && f->box->kind == EMBI_BK_INLINE) {
        embi_grect u = {0, 0, 0, 0};
        bool any = false;
        if (element_rects(e, el, &u, &any, nullptr, 0) > 0 && any) {
            bw = u.x1 - u.x0;
            bh = u.y1 - u.y0;
        }
    }
    *w = SDL_roundf(ex + bw) - SDL_roundf(ex);
    *h = SDL_roundf(ey + bh) - SDL_roundf(ey);
    const embi_element *op = offset_parent(e, el);
    *parent = op;
    if (op == nullptr) {
        *x = SDL_roundf(ex);
        *y = SDL_roundf(ey);
        return true;
    }
    /* The body rule: an offsetParent body that is not positioned measures from the containing
     * block origin. */
    const embi_style *bs = estyle(e, op);
    uint16_t bpos = kw_of(bs, EMBI_PROP_position);
    if (is_html_named(op, "body") && (bpos == 0 || bpos == EMBI_KW_static)) {
        *x = SDL_roundf(ex);
        *y = SDL_roundf(ey);
        return true;
    }
    const embi_frag *pf = principal(e, op);
    if (pf == nullptr) {
        *x = SDL_roundf(ex);
        *y = SDL_roundf(ey);
        return true;
    }
    float px = 0, py = 0;
    doc_origin(pf, &px, &py);
    *x = SDL_roundf(ex - px - embi_frag_bdr(pf, 0));
    *y = SDL_roundf(ey - py - embi_frag_bdr(pf, 1));
    return true;
}

/* CSSOM View: the root element (the body in quirks mode) stands for the viewport in clientWidth,
 * clientHeight, scrollWidth and scrollHeight. */
static bool is_viewport_element(const embi_layout_engine *e, const embi_element *el)
{
    const embi_document *doc = el->base.doc;
    if (doc == nullptr) {
        return false;
    }
    embi_element *root = embi_document_element((embi_document *)(uintptr_t)doc);
    (void)e;
    if (doc->quirks != EMBI_QUIRKS_FULL) {
        return el == root;
    }
    return is_html_named(el, "body") && el->base.parent == (const embi_node *)root;
}

/* The overflow of the viewport (CSS Overflow 3 section 3.3): the root element's, or the body's when
 * the root's is visible on both axes. Returns the used mode per axis: 0 auto/visible (a bar when the
 * content overflows), 1 hidden or clip (never a bar), 2 scroll (always a bar). */
static void viewport_modes(const embi_layout_engine *e, const embi_document *doc, int *mx, int *my)
{
    embi_element *root = embi_document_element((embi_document *)(uintptr_t)doc);
    const embi_element *src = root;
    uint16_t ox = kw_of(estyle(e, root), EMBI_PROP_overflow_x);
    uint16_t oy = kw_of(estyle(e, root), EMBI_PROP_overflow_y);
    bool vis = (ox == 0 || ox == EMBI_KW_visible) && (oy == 0 || oy == EMBI_KW_visible);
    if (vis) {
        for (const embi_node *n = root->base.first_child; n != nullptr; n = n->next) {
            if (n->type == EMBI_NODE_ELEMENT && is_html_named((const embi_element *)n, "body")) {
                src = (const embi_element *)n;
                break;
            }
        }
        ox = kw_of(estyle(e, src), EMBI_PROP_overflow_x);
        oy = kw_of(estyle(e, src), EMBI_PROP_overflow_y);
    }
    *mx = ox == EMBI_KW_scroll ? 2 : (ox == EMBI_KW_hidden || ox == EMBI_KW_clip ? 1 : 0);
    *my = oy == EMBI_KW_scroll ? 2 : (oy == EMBI_KW_hidden || oy == EMBI_KW_clip ? 1 : 0);
}

bool embi_layout_client_size(const embi_layout_engine *e, const embi_element *el, float *w,
                             float *h)
{
    const embi_frag *f = principal(e, el);
    if (f == nullptr) {
        *w = 0;
        *h = 0;
        return false;
    }
    /* An inline box has no padding box of its own in CSSOM View: 0 by 0. */
    if (f->box != nullptr && f->box->kind == EMBI_BK_INLINE) {
        *w = 0;
        *h = 0;
        return true;
    }
    if (is_viewport_element(e, el)) {
        /* The viewport without the scrollbars it shows: a vertical one when the content is taller,
         * a horizontal one when it is wider. */
        const embi_frag *icb = embi_layout_root(e, el->base.doc);
        float t = e->scrollbar > 0 ? e->scrollbar : 0;
        int mx, my;
        viewport_modes(e, el->base.doc, &mx, &my);
        bool tall = my == 2 || (my == 0 && embi_fo(icb)->ov[3] > e->vh + 0.5f);
        bool wide = mx == 2 || (mx == 0 && embi_fo(icb)->ov[2] > e->vw + 0.5f);
        *w = SDL_roundf(e->vw - (tall ? t : 0));
        *h = SDL_roundf(e->vh - (wide ? t : 0));
        return true;
    }
    /* The padding box, without the scrollbar gutters. */
    *w = SDL_roundf(f->w - embi_frag_bdr(f, 0) - embi_frag_bdr(f, 2) - embi_frag_scr(f, 0));
    *h = SDL_roundf(f->h - embi_frag_bdr(f, 1) - embi_frag_bdr(f, 3) - embi_frag_scr(f, 1));
    return true;
}

bool embi_layout_box_edges(const embi_layout_engine *e, const embi_element *el, float bdr[4],
                           float pad[4], float mg[4])
{
    (void)e;
    const embi_frag *f = principal(e, el);
    /* The fragment of an inline box piece has no margins (its tier ends before them). */
    bool inl = f != nullptr && f->box != nullptr && f->kind == EMBI_FK_BOX && f->box->kind == EMBI_BK_INLINE;
    for (int i = 0; i < 4; i++) {
        bdr[i] = f != nullptr ? embi_frag_bdr(f, i) : 0.0f;
        pad[i] = f != nullptr ? embi_frag_pad(f, i) : 0.0f;
        mg[i] = f != nullptr && !inl ? embi_frag_mg(f, i) : 0.0f;
    }
    return f != nullptr;
}

bool embi_layout_scroll_size(const embi_layout_engine *e, const embi_element *el, float *w,
                             float *h)
{
    const embi_frag *f = principal(e, el);
    if (f == nullptr) {
        *w = 0;
        *h = 0;
        return false;
    }
    if (is_viewport_element(e, el)) {
        /* The viewport scrolling area, at least the viewport. */
        const embi_frag *icb = embi_layout_root(e, el->base.doc);
        *w = SDL_roundf(embi_fo(icb)->ov[2] > e->vw ? embi_fo(icb)->ov[2] : e->vw);
        *h = SDL_roundf(embi_fo(icb)->ov[3] > e->vh ? embi_fo(icb)->ov[3] : e->vh);
        return true;
    }
    /* The scrollable overflow measured from the padding box origin, and at least the client size. */
    float cw = f->w - embi_frag_bdr(f, 0) - embi_frag_bdr(f, 2) - embi_frag_scr(f, 0);
    float ch = f->h - embi_frag_bdr(f, 1) - embi_frag_bdr(f, 3) - embi_frag_scr(f, 1);
    float fov[4];
    embi_frag_ov(f, fov);
    float ow = fov[2] - embi_frag_bdr(f, 0);
    float oh = fov[3] - embi_frag_bdr(f, 1);
    *w = SDL_roundf(ow > cw ? ow : cw);
    *h = SDL_roundf(oh > ch ? oh : ch);
    return true;
}

/* ------------------------------------------------------------------ dump */

static void dump_frag(const embi_frag *f, uint32_t depth, float ox, float oy, embi_sb *sb)
{
    float fx = ox + f->x;
    float fy = oy + f->y;
    char line[512];
    for (uint32_t i = 0; i < depth; i++) {
        embi_sb_append_char(sb, ' ');
        embi_sb_append_char(sb, ' ');
    }
    const char *kind = f->kind == EMBI_FK_LINE ? "line" : (f->kind == EMBI_FK_TEXT ? "text" : "box");
    embi_str tag = {"", 0};
    char idbuf[64] = "";
    char cls[64] = "";
    if (f->box != nullptr && f->box->el != nullptr) {
        tag = embi_atom_str(f->box->el->base.name);
        const embi_attr *ia = embi_el_get_attribute(f->box->el, (embi_str){"id", 2});
        if (ia != nullptr) {
            embi_sb t = {0};
            embi_dstr_append_utf8(&t, ia->value);
            SDL_snprintf(idbuf, sizeof idbuf, " id=%.*s", (int)(t.len < 40 ? t.len : 40), t.data != nullptr ? t.data : "");
            embi_sb_free(&t);
        }
        const embi_attr *ca = embi_el_get_attribute(f->box->el, (embi_str){"class", 5});
        if (ca != nullptr) {
            embi_sb t = {0};
            embi_dstr_append_utf8(&t, ca->value);
            SDL_snprintf(cls, sizeof cls, " class=%.*s", (int)(t.len < 40 ? t.len : 40), t.data != nullptr ? t.data : "");
            embi_sb_free(&t);
        }
    }
    int n = SDL_snprintf(line, sizeof line, "%s <%.*s>%s%s at (%g, %g) %gx%g", kind, (int)tag.len,
                         tag.ptr != nullptr ? tag.ptr : "", idbuf, cls, (double)fx, (double)fy,
                         (double)f->w, (double)f->h);
    if (n > 0) {
        /* SDL_snprintf returns the length it wanted: a long tag name must not be read past line. */
        size_t shown = (size_t)n < sizeof line ? (size_t)n : sizeof line - 1;
        embi_sb_append(sb, (embi_str){line, (uint32_t)shown});
    }
    if (f->kind == EMBI_FK_TEXT && f->box != nullptr && f->box->lt != nullptr) {
        embi_sb_append(sb, EMBI_STR(" \""));
        uint32_t len = embi_frag_tend(f) - embi_frag_tbegin(f);
        if (len > 40) {
            len = 40;
        }
        embi_sb_append(sb, (embi_str){embi_tshim_bytes(f->box->lt) + embi_frag_tbegin(f), len});
        embi_sb_append(sb, EMBI_STR("\""));
    }
    embi_sb_append_char(sb, '\n');
    for (const embi_frag *k = embi_frag_first(f); k != nullptr; k = k->next) {
        dump_frag(k, depth + 1, fx, fy, sb);
    }
}

void embi_layout_dump(const embi_layout_engine *e, const embi_document *doc, embi_sb *sb)
{
    const embi_frag *root = embi_layout_root(e, doc);
    if (root == nullptr) {
        embi_sb_append(sb, EMBI_STR("(no layout)\n"));
        return;
    }
    dump_frag(root, 0, 0, 0, sb);
}
