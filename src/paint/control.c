/*
 * Painting of form controls (W5e): a single plain, platform-neutral look (Chromium-like flat
 * style). Text fields and textareas get a 1 px border, their text, the selection highlight, the blinking caret and the
 * composition underline; buttons a face; checkboxes and radios their box with a tick (EMBI_DI_CHECK) or dot; a select its
 * label and arrow (and a popup list while open); a listbox its rows; range a track and thumb; progress and meter their
 * bars. `appearance: none` drops all of the chrome, so authored styles show; the text still draws.
 *
 * The state (value, selection, caret, checkedness ...) comes from cfg->control (the bindings, via the embedder) or, without
 * it, from the element's attributes. The text of a control is shaped here, at paint time: the runs belong to the display
 * list (dl->own_runs). The same layout of the text answers where the caret goes for a pointer position
 * (embi_paint_control_offset) and where the caret is (embi_paint_control_caret).
 */
#include "paint/paint_int.h"

#include "dom/dom.h"
#include "layout/box.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* layout/layout_int.h declares it; including that header would clash with style.h names here. */
void embi_lay_text_style(const embi_style *s, embi_lay_tstyle *ts, embi_str *fams, uint32_t cap);

#define RGBA(r, g, b) (0xFF000000u | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))
#define C_WHITE 0xFFFFFFFFu
#define C_BORDER RGBA(118, 118, 118)
#define C_BORDER_DIS 0x4D767676u
#define C_FACE RGBA(239, 239, 239)
#define C_FACE_DIS 0x4DEFEFEFu
#define C_BLUE RGBA(0, 117, 255)
#define C_BLUE_DIS RGBA(176, 176, 176)
#define C_TRACK RGBA(195, 195, 195)
#define C_SEL_FOCUS RGBA(179, 215, 255)
#define C_SEL_BLUR RGBA(212, 212, 212)
#define C_PLACEHOLDER RGBA(117, 117, 117)

/* ------------------------------------------------------------------ state */

static bool attr_sb(const embi_element *el, const char *name, embi_sb *sb)
{
    const embi_attr *a = embi_el_get_attribute((embi_element *)(uintptr_t)el, (embi_str){name, (uint32_t)strlen(name)});
    if (a == nullptr) {
        return false;
    }
    embi_dstr_append_utf8(sb, a->value);
    return true;
}

static bool attr_num(const embi_element *el, const char *name, double *out)
{
    embi_sb sb = {0};
    bool ok = false;
    if (attr_sb(el, name, &sb) && sb.len > 0 && sb.len < 64) {
        char tmp[64];
        memcpy(tmp, sb.data, sb.len);
        tmp[sb.len] = '\0';
        char *end = nullptr;
        double v = strtod(tmp, &end);
        if (end != tmp && isfinite(v)) {
            *out = v;
            ok = true;
        }
    }
    embi_sb_free(&sb);
    return ok;
}

static bool lit_ieq(embi_str s, const char *lit)
{
    size_t n = strlen(lit);
    if (s.len != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = s.ptr[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c + 32);
        }
        if (c != lit[i]) {
            return false;
        }
    }
    return true;
}

/* The kind of the control element, from its tag and type. */
static uint8_t kind_of(const embi_element *el)
{
    embi_str nm = embi_atom_str(el->base.name);
    if (lit_ieq(nm, "textarea")) {
        return EMBI_PCTL_TEXTAREA;
    }
    if (lit_ieq(nm, "select")) {
        embi_sb sb = {0};
        double size = 0;
        bool list = attr_sb(el, "multiple", &sb) || (attr_num(el, "size", &size) && size > 1);
        embi_sb_free(&sb);
        return list ? EMBI_PCTL_LISTBOX : EMBI_PCTL_SELECT;
    }
    if (lit_ieq(nm, "progress")) {
        return EMBI_PCTL_PROGRESS;
    }
    if (lit_ieq(nm, "meter")) {
        return EMBI_PCTL_METER;
    }
    if (lit_ieq(nm, "button")) {
        return EMBI_PCTL_BUTTON;
    }
    if (!lit_ieq(nm, "input")) {
        return EMBI_PCTL_NONE;
    }
    embi_sb t = {0};
    uint8_t k = EMBI_PCTL_TEXT;
    if (attr_sb(el, "type", &t)) {
        embi_str ts = {t.data != nullptr ? t.data : "", t.len};
        if (lit_ieq(ts, "checkbox")) {
            k = EMBI_PCTL_CHECKBOX;
        } else if (lit_ieq(ts, "radio")) {
            k = EMBI_PCTL_RADIO;
        } else if (lit_ieq(ts, "range")) {
            k = EMBI_PCTL_RANGE;
        } else if (lit_ieq(ts, "button") || lit_ieq(ts, "submit") || lit_ieq(ts, "reset")) {
            k = EMBI_PCTL_BUTTON;
        } else if (lit_ieq(ts, "hidden") || lit_ieq(ts, "file") || lit_ieq(ts, "image") || lit_ieq(ts, "color")) {
            k = EMBI_PCTL_NONE;
        }
    }
    embi_sb_free(&t);
    return k;
}

/* Appends the text of the text nodes below n. */
static void text_below(const embi_node *n, embi_sb *sb)
{
    for (const embi_node *k = n->first_child; k != nullptr; k = k->next) {
        if (k->type == EMBI_NODE_TEXT) {
            embi_dstr_append_utf8(sb, ((const embi_chardata *)k)->data);
        } else if (k->type == EMBI_NODE_ELEMENT) {
            text_below(k, sb);
        }
    }
}

/* The label of an option without a state callback: the label attribute, else its text with white space collapsed. */
static void option_label_sb(const embi_element *opt, embi_sb *out)
{
    if (attr_sb(opt, "label", out) && out->len > 0) {
        return;
    }
    out->len = 0;
    embi_sb raw = {0};
    text_below(&opt->base, &raw);
    bool space = true;
    for (uint32_t i = 0; i < raw.len; i++) {
        char ch = raw.data[i];
        if (ch == ' ' || ch == 0x09 || ch == 0x0A || ch == 0x0D || ch == 0x0C) {
            if (!space) {
                embi_sb_append_char(out, ' ');
                space = true;
            }
        } else {
            embi_sb_append_char(out, ch);
            space = false;
        }
    }
    while (out->len > 0 && out->data[out->len - 1] == ' ') {
        out->len--;
    }
    embi_sb_free(&raw);
}

/* The state of el: from the callback when it knows the element, else from the attributes (the strings go into sb). */
static void ctl_state(embi_layout_control_fn fn, void *user, const embi_element *el, embi_paint_ctl *c, embi_sb *sb)
{
    memset(c, 0, sizeof *c);
    c->hot = -1;
    if (fn != nullptr && fn(user, el, c) && c->kind != EMBI_PCTL_NONE) {
        return;
    }
    memset(c, 0, sizeof *c);
    c->hot = -1;
    c->kind = kind_of(el);
    c->disabled = false;
    embi_sb probe = {0};
    c->disabled = attr_sb(el, "disabled", &probe);
    embi_sb_free(&probe);
    switch (c->kind) {
    case EMBI_PCTL_TEXT:
    case EMBI_PCTL_TEXTAREA: {
        embi_sb ty = {0};
        c->password = attr_sb(el, "type", &ty) && lit_ieq((embi_str){ty.data != nullptr ? ty.data : "", ty.len}, "password");
        embi_sb_free(&ty);
        if (c->kind == EMBI_PCTL_TEXT) {
            (void)attr_sb(el, "value", sb);
        } else {
            text_below(&el->base, sb);
        }
        c->text = sb->data;
        c->text_len = sb->len;
        c->caret = c->sel_a = c->sel_b = sb->len;
        if (sb->len == 0 && attr_sb(el, "placeholder", sb) && sb->len > 0) {
            /* an empty field shows its placeholder (the buffer holds only the placeholder then) */
            c->placeholder = sb->data;
            c->placeholder_len = sb->len;
            c->text = "";
            c->text_len = 0;
            c->caret = c->sel_a = c->sel_b = 0;
        }
        break;
    }
    case EMBI_PCTL_SELECT: {
        /* the label of the selected option, else of the first one */
        const embi_node *pick = nullptr;
        for (const embi_node *k = el->base.first_child; k != nullptr && pick == nullptr; k = k->next) {
            if (k->type == EMBI_NODE_ELEMENT && lit_ieq(embi_atom_str(k->name), "option")) {
                embi_sb sp = {0};
                bool sel = attr_sb((const embi_element *)k, "selected", &sp);
                embi_sb_free(&sp);
                if (sel) {
                    pick = k;
                }
            }
        }
        for (const embi_node *k = el->base.first_child; k != nullptr && pick == nullptr; k = k->next) {
            if (k->type == EMBI_NODE_ELEMENT && lit_ieq(embi_atom_str(k->name), "option")) {
                pick = k;
            }
        }
        if (pick != nullptr) {
            option_label_sb((const embi_element *)pick, sb);
            c->text = sb->data;
            c->text_len = sb->len;
        }
        break;
    }
    case EMBI_PCTL_CHECKBOX:
    case EMBI_PCTL_RADIO: {
        embi_sb ck = {0};
        c->checked = attr_sb(el, "checked", &ck);
        embi_sb_free(&ck);
        break;
    }
    case EMBI_PCTL_RANGE: {
        double mn = 0, mx = 100, v = 0;
        (void)attr_num(el, "min", &mn);
        (void)attr_num(el, "max", &mx);
        if (mx < mn) {
            mx = mn;
        }
        v = mn + (mx - mn) / 2;
        (void)attr_num(el, "value", &v);
        c->min = mn;
        c->max = mx;
        c->value = v < mn ? mn : (v > mx ? mx : v);
        break;
    }
    case EMBI_PCTL_PROGRESS: {
        double mx = 1, v = 0;
        (void)attr_num(el, "max", &mx);
        if (!(mx > 0)) {
            mx = 1;
        }
        c->max = mx;
        c->indeterminate_bar = !attr_num(el, "value", &v);
        c->value = v < 0 ? 0 : (v > mx ? mx : v);
        break;
    }
    case EMBI_PCTL_METER: {
        double mn = 0, mx = 1, v = 0;
        (void)attr_num(el, "min", &mn);
        (void)attr_num(el, "max", &mx);
        if (mx < mn) {
            mx = mn;
        }
        (void)attr_num(el, "value", &v);
        v = v < mn ? mn : (v > mx ? mx : v);
        double lo = mn, hi = mx, op = mn + (mx - mn) / 2;
        (void)attr_num(el, "low", &lo);
        (void)attr_num(el, "high", &hi);
        (void)attr_num(el, "optimum", &op);
        lo = lo < mn ? mn : (lo > mx ? mx : lo);
        hi = hi < lo ? lo : (hi > mx ? mx : hi);
        op = op < mn ? mn : (op > mx ? mx : op);
        c->min = mn;
        c->max = mx;
        c->value = v;
        c->low = lo;
        c->high = hi;
        c->optimum = op;
        break;
    }
    default:
        break;
    }
}

/* ------------------------------------------------------------------ geometry */

static bool appearance_none(const embi_style *s)
{
    return embi_pb_kw(s, EMBI_PROP_appearance) == EMBI_KW_none;
}

static bool has_author_bg(const embi_style *s)
{
    uint32_t rgba = 0;
    if (embi_pb_color(s, embi_style_value(s, EMBI_PROP_background_color), &rgba) && (rgba >> 24) != 0) {
        return true;
    }
    const embi_value *img = embi_style_value(s, EMBI_PROP_background_image);
    return img != nullptr && !(img->kind == EMBI_V_KEYWORD && img->kw == EMBI_KW_none);
}

static bool has_author_border(const embi_style *s)
{
    static const embi_prop_id side[4] = {EMBI_PROP_border_top_style, EMBI_PROP_border_right_style,
                                         EMBI_PROP_border_bottom_style, EMBI_PROP_border_left_style};
    for (int i = 0; i < 4; i++) {
        uint16_t kw = embi_pb_kw(s, side[i]);
        if (kw != 0 && kw != EMBI_KW_none && kw != EMBI_KW_hidden) {
            return true;
        }
    }
    return false;
}

/* The content box of a text control from its border box: the used border and padding, and with the chrome on at least the
 * 3 px (1 px border and 2 px padding) of the flat look. bdr and pad are left, top, right, bottom. */
static void text_content(const float bb[4], const float bdr[4], const float pad[4], bool chrome, float cb[4])
{
    float l = bdr[0] + pad[0], t = bdr[1] + pad[1], r = bdr[2] + pad[2], b = bdr[3] + pad[3];
    if (chrome) {
        l = fmaxf(l, 3.0f);
        r = fmaxf(r, 3.0f);
        t = fmaxf(t, 2.0f);
        b = fmaxf(b, 2.0f);
    }
    cb[0] = bb[0] + l;
    cb[1] = bb[1] + t;
    cb[2] = fmaxf(0.0f, bb[2] - l - r);
    cb[3] = fmaxf(0.0f, bb[3] - t - b);
}

/* ------------------------------------------------------------------ text layout */

typedef struct cl_piece {
    embi_lay_shaped *run; /* nullptr for an empty line */
    uint32_t ri;          /* the run's index in dl->runs once emitted (UINT32_MAX before) */
    uint32_t base;        /* byte offset of the run's text in the whole display text */
    uint32_t lo, hi;      /* the piece within the run */
    float y;              /* top, relative to the first line */
    bool last;            /* the last piece of its logical line */
} cl_piece;

typedef struct cl_layout {
    EMBI_VEC(cl_piece) pc;
    float lh, ca;         /* the line height and the ascent */
    embi_text *t;
    const embi_style *s;
} cl_layout;

static void cl_free(cl_layout *L, bool free_runs)
{
    if (free_runs) {
        for (uint32_t i = 0; i < L->pc.len; i++) {
            cl_piece *p = &L->pc.data[i];
            bool first_of_run = p->run != nullptr && (i == 0 || L->pc.data[i - 1].run != p->run);
            if (first_of_run) {
                embi_tshim_free(p->run);
            }
        }
    }
    embi_vec_free(&L->pc);
}

/* The display list takes over the shaped runs of the layout. */
static void cl_adopt(const cl_layout *L, embi_dl *d)
{
    for (uint32_t i = 0; i < L->pc.len; i++) {
        const cl_piece *p = &L->pc.data[i];
        if (p->run != nullptr && (i == 0 || L->pc.data[i - 1].run != p->run)) {
            embi_vec_push(&d->own_runs, (void *)p->run);
        }
    }
}

static embi_lay_shaped *shape_str(embi_text *t, const embi_style *s, const char *txt, uint32_t len)
{
    embi_lay_tstyle ts;
    embi_str fams[16];
    embi_lay_text_style(s, &ts, fams, 16);
    return embi_tshim_shape(t, txt, len, &ts);
}

static uint32_t cp_len(uint8_t b)
{
    return b < 0x80 ? 1 : (b >= 0xF0 ? 4 : (b >= 0xE0 ? 3 : (b >= 0xC0 ? 2 : 1)));
}

static void line_metrics(embi_text *t, const embi_style *s, float *lh, float *ca)
{
    embi_lay_shaped *r = shape_str(t, s, "x", 1);
    float a = 0, d = 0;
    if (r == nullptr || !embi_tshim_content_metrics(r, &a, &d)) {
        a = embi_pb_fontsize(s) * 0.9f;
        d = embi_pb_fontsize(s) * 0.25f;
    }
    embi_tshim_free(r);
    *lh = a + d;
    *ca = a;
}

/* Lays out txt (the display text) in lines: at the newlines and, when wrap is set, at break opportunities so that a line
 * fits in width. */
static void cl_build(cl_layout *L, embi_text *t, const embi_style *s, const char *txt, uint32_t len, float width, bool wrap)
{
    memset(L, 0, sizeof *L);
    L->t = t;
    L->s = s;
    line_metrics(t, s, &L->lh, &L->ca);
    uint32_t off = 0;
    float y = 0;
    for (;;) {
        uint32_t end = off;
        while (end < len && txt[end] != '\n') {
            end++;
        }
        uint32_t n = end - off;
        embi_lay_shaped *run = n > 0 ? shape_str(t, s, txt + off, n) : nullptr;
        if (run == nullptr) {
            cl_piece p = {nullptr, UINT32_MAX, off, 0, 0, y, true};
            embi_vec_push(&L->pc, p);
            y += L->lh;
        } else if (!wrap) {
            cl_piece p = {run, UINT32_MAX, off, 0, n, y, true};
            embi_vec_push(&L->pc, p);
            y += L->lh;
        } else {
            uint32_t lo = 0;
            uint32_t nb = embi_tshim_break_count(run);
            while (lo < n) {
                uint32_t best = 0;
                for (uint32_t i = 0; i < nb; i++) {
                    uint32_t bo = 0;
                    if (!embi_tshim_break_at(run, i, &bo) || bo <= lo || bo > n) {
                        continue;
                    }
                    /* trailing spaces hang: measure without them */
                    uint32_t me = bo;
                    while (me > lo && (txt[off + me - 1] == ' ' || txt[off + me - 1] == '\t')) {
                        me--;
                    }
                    if (embi_tshim_measure(run, lo, me) <= width) {
                        best = bo;
                    } else if (best != 0) {
                        break;
                    } else {
                        best = bo; /* a single unbreakable word: it overflows (cut by characters below) */
                        break;
                    }
                }
                if (best == 0) {
                    best = n;
                }
                if (embi_tshim_measure(run, lo, best) > width) {
                    /* cut by code points to fit (at least one) */
                    uint32_t k = lo + cp_len((uint8_t)txt[off + lo]);
                    while (k < best) {
                        uint32_t nk = k + cp_len((uint8_t)txt[off + k]);
                        if (nk > best || embi_tshim_measure(run, lo, nk) > width) {
                            break;
                        }
                        k = nk;
                    }
                    best = k;
                }
                cl_piece p = {run, UINT32_MAX, off, lo, best, y, best >= n};
                embi_vec_push(&L->pc, p);
                y += L->lh;
                lo = best;
            }
        }
        if (end >= len) {
            break;
        }
        off = end + 1;
    }
}

/* The piece and x offset of the display-text byte offset `at`. */
static void cl_locate(const cl_layout *L, uint32_t at, uint32_t *pi, float *x)
{
    uint32_t pick = L->pc.len - 1;
    for (uint32_t i = 0; i < L->pc.len; i++) {
        const cl_piece *p = &L->pc.data[i];
        uint32_t s0 = p->base + p->lo, e0 = p->base + p->hi;
        if (at < e0 || (at == e0 && p->last)) {
            if (at >= s0) {
                pick = i;
                break;
            }
        }
    }
    const cl_piece *p = &L->pc.data[pick];
    uint32_t rel = at > p->base + p->lo ? at - p->base : p->lo;
    if (rel > p->hi) {
        rel = p->hi;
    }
    *pi = pick;
    *x = p->run != nullptr && rel > p->lo ? embi_tshim_measure(p->run, p->lo, rel) : 0.0f;
}

/* The display-text offset nearest to the x within piece pi. */
static uint32_t cl_offset_in(const cl_layout *L, const char *txt, uint32_t pi, float x)
{
    const cl_piece *p = &L->pc.data[pi];
    if (p->run == nullptr || x <= 0) {
        return p->base + p->lo;
    }
    uint32_t best = p->lo;
    float bestd = x;
    uint32_t k = p->lo;
    while (k < p->hi) {
        k += cp_len((uint8_t)txt[p->base + k]);
        if (k > p->hi) {
            k = p->hi;
        }
        float w = embi_tshim_measure(p->run, p->lo, k);
        float d = fabsf(x - w);
        if (d < bestd) {
            bestd = d;
            best = k;
        }
    }
    return p->base + best;
}

/* ------------------------------------------------------------------ display text and its mapping */

typedef struct dtext {
    embi_sb sb;           /* the display text */
    bool bullets;         /* a password: one bullet per code point of the value */
    const char *val;      /* the value */
    uint32_t vlen;
    bool placeholder;     /* the placeholder is shown instead of the value */
} dtext;

static void dtext_make(dtext *d, const embi_paint_ctl *c)
{
    memset(d, 0, sizeof *d);
    d->val = c->text != nullptr ? c->text : "";
    d->vlen = c->text != nullptr ? c->text_len : 0;
    d->bullets = c->password;
    if (c->text_len == 0 && c->placeholder != nullptr && c->placeholder_len > 0 && !c->comp_on) {
        d->placeholder = true;
        embi_sb_append(&d->sb, (embi_str){c->placeholder, c->placeholder_len});
        return;
    }
    if (!d->bullets) {
        if (d->vlen > 0) {
            embi_sb_append(&d->sb, (embi_str){d->val, d->vlen});
        }
        return;
    }
    for (uint32_t i = 0; i < d->vlen;) {
        i += cp_len((uint8_t)d->val[i]);
        embi_sb_append(&d->sb, EMBI_STR("\xE2\x80\xA2"));
    }
}

/* The display offset of the value offset v. */
static uint32_t to_disp(const dtext *d, uint32_t v)
{
    if (d->placeholder) {
        return 0;
    }
    if (!d->bullets) {
        return v;
    }
    uint32_t n = 0;
    for (uint32_t i = 0; i < d->vlen && i < v;) {
        i += cp_len((uint8_t)d->val[i]);
        n++;
    }
    return n * 3;
}

/* The value offset of the display offset o. */
static uint32_t to_value(const dtext *d, uint32_t o)
{
    if (d->placeholder) {
        return 0;
    }
    if (!d->bullets) {
        return o > d->vlen ? d->vlen : o;
    }
    uint32_t n = o / 3, i = 0;
    while (n > 0 && i < d->vlen) {
        i += cp_len((uint8_t)d->val[i]);
        n--;
    }
    return i > d->vlen ? d->vlen : i;
}

/* ------------------------------------------------------------------ emitters */

static uint32_t run_index(embi_pst *p, embi_lay_shaped *run)
{
    uint32_t ri = p->d->runs.len;
    embi_vec_push(&p->d->runs, (const void *)run);
    return ri;
}

static void emit_text(embi_pst *p, cl_piece *pc, float x, float top, float baseline, float lh, uint32_t color, uint32_t xf,
                      uint32_t clip)
{
    if (pc->run == nullptr || pc->hi <= pc->lo) {
        return;
    }
    if (pc->ri == UINT32_MAX) {
        pc->ri = run_index(p, pc->run);
    }
    float w = embi_tshim_measure(pc->run, pc->lo, pc->hi);
    embi_ditem *it = embi_pb_item(p, EMBI_DI_TEXT, xf, clip);
    it->rect[0] = x;
    it->rect[1] = top;
    it->rect[2] = w;
    it->rect[3] = lh;
    it->aux = pc->ri;
    it->aux2 = pc->hi;
    it->aux3 = pc->lo;
    embi_ditem_set_chash(it, embi_tshim_content_hash(pc->run, pc->lo, pc->hi));
    it->f[0] = x;
    it->f[1] = baseline;
    it->colors[0] = color;
}

static uint32_t with_alpha(uint32_t c, float a)
{
    uint32_t na = (uint32_t)lroundf((float)(c >> 24) * a);
    return (c & 0x00FFFFFFu) | (na << 24);
}

static void box_border(embi_pst *p, const float r[4], const float radii[4], float w, uint32_t color, uint32_t xf, uint32_t clip)
{
    if ((color >> 24) == 0 || !(w > 0)) {
        return;
    }
    embi_ditem *it = embi_pb_item(p, EMBI_DI_BORDER, xf, clip);
    memcpy(it->rect, r, sizeof it->rect);
    memcpy(it->radii, radii, sizeof it->radii);
    for (int i = 0; i < 4; i++) {
        it->widths[i] = w;
        it->colors[i] = color;
        it->styles[i] = EMBI_DS_SOLID;
    }
}

static void radii_all(float v, float out[4])
{
    out[0] = out[1] = out[2] = out[3] = v;
}

/* The text content of a text control: lines, selection, composition and caret. cb is the content box. */
static void paint_text(embi_pst *p, const embi_style *s, const embi_paint_ctl *c, const float cb[4], uint32_t xf,
                       uint32_t clip, const embi_element *el)
{
    dtext d;
    dtext_make(&d, c);
    bool multi = c->kind == EMBI_PCTL_TEXTAREA;
    cl_layout L;
    cl_build(&L, p->text, s, d.sb.data != nullptr ? d.sb.data : "", d.sb.len, cb[2], multi && !c->nowrap);
    uint32_t color = 0xFF000000u;
    (void)embi_pb_color(s, embi_style_value(s, EMBI_PROP_color), &color);
    if (c->disabled) {
        color = with_alpha(color, 0.6f);
    }
    uint32_t pcolor = d.placeholder ? C_PLACEHOLDER : color;

    /* scroll to keep the caret visible */
    float sx = 0, sy = 0;
    uint32_t cpi = 0;
    float cx = 0;
    uint32_t caret_d = to_disp(&d, c->caret);
    cl_locate(&L, caret_d, &cpi, &cx);
    if (!multi && cx > cb[2] - 1.0f) {
        sx = cx - (cb[2] - 1.0f);
    }
    if (multi) {
        float bottom = L.pc.data[cpi].y + L.lh;
        if (bottom > cb[3]) {
            sy = bottom - cb[3];
        }
    }
    float oy = cb[1] - sy;
    if (!multi) {
        oy = cb[1] + (cb[3] - L.lh) * 0.5f;
    }
    float zero[4] = {0, 0, 0, 0};
    float cclip[4] = {cb[0], cb[1], cb[2], cb[3]};
    if (!multi && L.lh > cb[3]) {
        /* a single line is clipped on the sides only: a line taller than the content box shows over the padding */
        cclip[1] = oy;
        cclip[3] = L.lh;
    }
    uint32_t lclip = embi_pb_clip(p, cclip, zero, xf, clip);

    /* the selection highlight */
    if (!d.placeholder && c->sel_b > c->sel_a) {
        uint32_t a = to_disp(&d, c->sel_a), b = to_disp(&d, c->sel_b);
        uint32_t sel = c->focused ? C_SEL_FOCUS : C_SEL_BLUR;
        embi_style_engine *se = p->se;
        if (se != nullptr && el != nullptr) {
            const embi_style *ss = embi_style_generated(se, (embi_element *)(uintptr_t)el, EMBI_PE_SELECTION);
            uint32_t bg = 0;
            if (ss != nullptr && embi_pb_color(ss, embi_style_value(ss, EMBI_PROP_background_color), &bg) && (bg >> 24) != 0) {
                sel = bg;
            }
        }
        for (uint32_t i = 0; i < L.pc.len; i++) {
            cl_piece *pc = &L.pc.data[i];
            uint32_t s0 = pc->base + pc->lo, e0 = pc->base + pc->hi;
            uint32_t lo = a > s0 ? a : s0, hi = b < e0 ? b : e0;
            bool nl_sel = pc->last && b > e0 && a <= e0 && i + 1 < L.pc.len; /* the selected line break: a stub */
            if (hi <= lo && !nl_sel) {
                continue;
            }
            float x0 = pc->run != nullptr && lo > s0 ? embi_tshim_measure(pc->run, pc->lo, lo - pc->base) : 0.0f;
            float x1 = pc->run != nullptr && hi > s0 ? embi_tshim_measure(pc->run, pc->lo, hi - pc->base) : x0;
            if (nl_sel) {
                x1 += 4.0f;
            }
            float r[4] = {cb[0] - sx + x0, oy + pc->y, x1 - x0, L.lh};
            (void)embi_pb_fill(p, r, zero, sel, xf, lclip);
        }
    }
    /* the text */
    for (uint32_t i = 0; i < L.pc.len; i++) {
        cl_piece *pc = &L.pc.data[i];
        float top = oy + pc->y;
        if (top + L.lh < cb[1] || top > cb[1] + cb[3]) {
            continue;
        }
        emit_text(p, pc, cb[0] - sx, top, top + L.ca, L.lh, pcolor, xf, lclip);
    }
    /* the composition underline */
    if (c->comp_on && c->comp_b > c->comp_a) {
        uint32_t a = to_disp(&d, c->comp_a), b = to_disp(&d, c->comp_b);
        for (uint32_t i = 0; i < L.pc.len; i++) {
            cl_piece *pc = &L.pc.data[i];
            uint32_t s0 = pc->base + pc->lo, e0 = pc->base + pc->hi;
            uint32_t lo = a > s0 ? a : s0, hi = b < e0 ? b : e0;
            if (hi <= lo || pc->run == nullptr) {
                continue;
            }
            float x0 = embi_tshim_measure(pc->run, pc->lo, lo - pc->base);
            float x1 = embi_tshim_measure(pc->run, pc->lo, hi - pc->base);
            embi_ditem *it = embi_pb_item(p, EMBI_DI_DECORATION, xf, lclip);
            it->sub = EMBI_DECO_SOLID;
            it->rect[0] = cb[0] - sx + x0;
            it->rect[1] = oy + pc->y + L.ca + 1.5f;
            it->rect[2] = x1 - x0;
            it->rect[3] = 1.0f;
            it->colors[0] = color;
        }
    }
    /* the caret */
    if (c->focused && c->caret_on && !c->readonly && !c->disabled && c->sel_a == c->sel_b) {
        float r[4] = {cb[0] - sx + cx, oy + L.pc.data[cpi].y, 1.0f, L.lh};
        (void)embi_pb_fill(p, r, zero, color, xf, lclip);
    }
    cl_adopt(&L, p->d);
    cl_free(&L, false);
    embi_sb_free(&d.sb);
}

/* ------------------------------------------------------------------ chrome */

static void meter_colors(const embi_paint_ctl *c, uint32_t *fill)
{
    int rv = c->value < c->low ? 0 : (c->value > c->high ? 2 : 1);
    int ro = c->optimum < c->low ? 0 : (c->optimum > c->high ? 2 : 1);
    int diff = rv > ro ? rv - ro : ro - rv;
    *fill = diff == 0 ? RGBA(16, 162, 45) : (diff == 1 ? RGBA(255, 193, 7) : RGBA(220, 53, 69));
}

static void paint_rows(embi_pst *p, const embi_style *s, const embi_paint_ctl *c, const float area[4], uint32_t xf, uint32_t clip,
                       bool popup)
{
    uint32_t color = 0xFF000000u;
    (void)embi_pb_color(s, embi_style_value(s, EMBI_PROP_color), &color);
    float zero[4] = {0, 0, 0, 0};
    uint32_t lclip = embi_pb_clip(p, area, zero, xf, clip);
    float lh = 0, ca = 0;
    line_metrics(p->text, s, &lh, &ca);
    uint32_t first = popup ? 0 : c->first_row;
    for (uint32_t i = first; i < c->nopts; i++) {
        float y = area[1] + (float)(i - first) * EMBI_CTL_ROW_H;
        if (y >= area[1] + area[3]) {
            break;
        }
        const embi_paint_opt *o = &c->opts[i];
        bool hot = popup ? (int32_t)i == c->hot : o->selected;
        uint32_t tc = color;
        if (hot) {
            float r[4] = {area[0], y, area[2], EMBI_CTL_ROW_H};
            if (popup || c->focused) {
                (void)embi_pb_fill(p, r, zero, C_BLUE, xf, lclip);
                tc = C_WHITE;
            } else {
                (void)embi_pb_fill(p, r, zero, RGBA(207, 207, 207), xf, lclip);
            }
        }
        if (o->disabled) {
            tc = with_alpha(tc, 0.5f);
        }
        if (o->label_len > 0) {
            embi_lay_shaped *run = shape_str(p->text, s, o->label, o->label_len);
            if (run != nullptr) {
                cl_piece pc = {run, UINT32_MAX, 0, 0, o->label_len, 0, true};
                embi_vec_push(&p->d->own_runs, (void *)run);
                emit_text(p, &pc, area[0] + 4.0f, y, y + (EMBI_CTL_ROW_H - lh) * 0.5f + ca, lh, tc, xf, lclip);
            }
        }
    }
}

/* ------------------------------------------------------------------ range with styled parts */

/* The width of side i (left, top, right, bottom) of the border of a part of a control as its style gives it: a side whose
 * style is none or hidden has none, the rest snap to whole px as the layout's borders do. */
float embi_pb_part_border(const embi_style *s, int i)
{
    static const embi_prop_id wp[4] = {EMBI_PROP_border_left_width, EMBI_PROP_border_top_width, EMBI_PROP_border_right_width,
                                       EMBI_PROP_border_bottom_width};
    static const embi_prop_id sp[4] = {EMBI_PROP_border_left_style, EMBI_PROP_border_top_style, EMBI_PROP_border_right_style,
                                       EMBI_PROP_border_bottom_style};
    uint16_t st = embi_pb_kw(s, sp[i]);
    if (st == 0 || st == EMBI_KW_none || st == EMBI_KW_hidden) {
        return 0.0f;
    }
    float v = embi_style_px(s, wp[i]);
    if (isnan(v) || v <= 0.0f) {
        const embi_value *kv = embi_style_value(s, wp[i]);
        if (kv != nullptr && kv->kind == EMBI_V_KEYWORD) {
            return kv->kw == EMBI_KW_thin ? 1.0f : (kv->kw == EMBI_KW_thick ? 5.0f : 3.0f);
        }
        return 0.0f;
    }
    return v < 1.0f ? 1.0f : floorf(v);
}

float embi_pb_part_padding(const embi_style *s, int i)
{
    static const embi_prop_id pp[4] = {EMBI_PROP_padding_left, EMBI_PROP_padding_top, EMBI_PROP_padding_right,
                                       EMBI_PROP_padding_bottom};
    float v = embi_style_px(s, pp[i]);
    return isnan(v) || v < 0.0f ? 0.0f : v;
}

/* The part of a control that has no box of its own (the track and thumb of a range) with the computed style ps, as a block of
 * border box r: its shadows, background and border, under its own transform (ps's transform on r). */
static void paint_part(embi_pst *p, const embi_style *ps, const float r[4], uint32_t xf, uint32_t clip)
{
    embi_fmet m;
    memset(&m, 0, sizeof m);
    for (int i = 0; i < 4; i++) {
        m.bdr[i] = embi_pb_part_border(ps, i);
        m.pad[i] = embi_pb_part_padding(ps, i);
    }
    embi_frag f;
    memset(&f, 0, sizeof f);
    f.kind = EMBI_FK_BOX;
    f.w = r[2];
    f.h = r[3];
    f.met = &m;
    uint32_t pxf = embi_pb_box_xform(p, ps, r, xf);
    embi_pb_shadows(p, &f, ps, r, false, pxf, clip);
    embi_pb_background(p, &f, ps, r, pxf, clip);
    embi_pb_shadows(p, &f, ps, r, true, pxf, clip);
    embi_pb_border(p, &f, ps, r, pxf, clip);
}

/* The size of a part along an axis: its width or height as a length, 0 when it is auto (a part with no content). */
static float part_size(const embi_style *s, embi_prop_id prop)
{
    float v = embi_style_px(s, prop);
    return isnan(v) || v < 0.0f ? 0.0f : v;
}

typedef struct range_geo {
    float track[4]; /* the border box of the track */
    float thumb[4]; /* the border box of the thumb, before its own transform */
    float tl, tw;   /* the content box of the track along the axis: the thumb's left edge runs from tl to tl + tw - thumb w */
} range_geo;

/* The boxes of the parts of a range whose content box is cb: the track spans it and is centered on its height (a flex item of
 * the input in Blink), the thumb is a block in the flow of the track (its top is the track's content top plus its margin-top)
 * placed along the track by the value's fraction. */
static void range_geometry(const embi_style *ts, const embi_style *hs, const float cb[4], double frac, range_geo *g)
{
    float th = ts != nullptr ? part_size(ts, EMBI_PROP_height) : 0.0f;
    g->track[0] = cb[0];
    g->track[2] = cb[2];
    g->track[3] = th;
    g->track[1] = cb[1] + (cb[3] - th) * 0.5f;
    float bl = 0.0f, bt = 0.0f, br = 0.0f;
    if (ts != nullptr) {
        bl = embi_pb_part_border(ts, 0) + embi_pb_part_padding(ts, 0);
        bt = embi_pb_part_border(ts, 1) + embi_pb_part_padding(ts, 1);
        br = embi_pb_part_border(ts, 2) + embi_pb_part_padding(ts, 2);
    }
    g->tl = g->track[0] + bl;
    g->tw = fmaxf(0.0f, g->track[2] - bl - br);
    float w = hs != nullptr ? part_size(hs, EMBI_PROP_width) : 0.0f;
    float h = hs != nullptr ? part_size(hs, EMBI_PROP_height) : 0.0f;
    float mt = hs != nullptr ? embi_style_px(hs, EMBI_PROP_margin_top) : 0.0f;
    if (isnan(mt)) {
        mt = 0.0f;
    }
    if (frac < 0.0) {
        frac = 0.0;
    }
    if (frac > 1.0) {
        frac = 1.0;
    }
    g->thumb[0] = g->tl + (float)frac * (g->tw - w);
    g->thumb[1] = g->track[1] + bt + mt;
    g->thumb[2] = w;
    g->thumb[3] = h;
}

/* The styled parts of a range input (appearance: none): the pseudo-element styles of the track and the thumb. */
static bool range_parts(const embi_pst *p, const embi_element *el, const embi_style **ts, const embi_style **hs)
{
    *ts = nullptr;
    *hs = nullptr;
    if (p->se == nullptr || el == nullptr) {
        return false;
    }
    *ts = embi_style_generated(p->se, (embi_element *)(uintptr_t)el, EMBI_PE_SLIDER_TRACK);
    *hs = embi_style_generated(p->se, (embi_element *)(uintptr_t)el, EMBI_PE_SLIDER_THUMB);
    return *ts != nullptr || *hs != nullptr;
}

static void paint_chrome_pre(embi_pst *p, const embi_style *s, const embi_paint_ctl *c, const float bb[4], uint32_t xf,
                             uint32_t clip)
{
    float radii[4];
    float fs = embi_pb_fontsize(s);
    embi_pb_radii(p, s, bb[2], bb[3], fs, radii);
    bool bg = has_author_bg(s);
    switch (c->kind) {
    case EMBI_PCTL_TEXT:
    case EMBI_PCTL_TEXTAREA:
    case EMBI_PCTL_SELECT:
    case EMBI_PCTL_LISTBOX:
        if (!bg) {
            if (radii[0] == 0 && radii[1] == 0 && radii[2] == 0 && radii[3] == 0) {
                radii_all(c->kind == EMBI_PCTL_SELECT ? 4.0f : 2.0f, radii);
            }
            (void)embi_pb_fill(p, bb, radii, c->disabled ? C_FACE_DIS : C_WHITE, xf, clip);
        }
        break;
    case EMBI_PCTL_BUTTON:
        break; /* the face is the UA sheet's background-color: ButtonFace */
    default:
        break;
    }
}

void embi_pb_control_pre(embi_pst *p, const embi_frag *f, const embi_box *b, const embi_style *s, const float bb[4], uint32_t xf,
                         uint32_t clip)
{
    (void)f;
    if (b->el == nullptr || appearance_none(s) || embi_pb_hidden(s)) {
        return;
    }
    embi_paint_ctl c;
    embi_sb sb = {0};
    ctl_state(p->ctl_fn, p->ctl_user, b->el, &c, &sb);
    paint_chrome_pre(p, s, &c, bb, xf, clip);
    embi_sb_free(&sb);
}

void embi_pb_control(embi_pst *p, const embi_frag *f, const embi_box *b, const embi_style *s, const float bb[4], uint32_t xf,
                     uint32_t clip)
{
    if (b->el == nullptr || embi_pb_hidden(s)) {
        return;
    }
    embi_paint_ctl c;
    embi_sb sb = {0};
    ctl_state(p->ctl_fn, p->ctl_user, b->el, &c, &sb);
    bool chrome = !appearance_none(s);
    float fs = embi_pb_fontsize(s);
    float radii[4];
    embi_pb_radii(p, s, bb[2], bb[3], fs, radii);
    float pb[4], cb[4];
    embi_pb_boxes(f, bb, pb, cb);
    float bdr[4] = {embi_frag_bdr(f, 0), embi_frag_bdr(f, 1), embi_frag_bdr(f, 2), embi_frag_bdr(f, 3)};
    float pad[4] = {embi_frag_pad(f, 0), embi_frag_pad(f, 1), embi_frag_pad(f, 2), embi_frag_pad(f, 3)};
    float zero[4] = {0, 0, 0, 0};
    bool own_border = has_author_border(s);
    uint32_t bcol = c.disabled ? C_BORDER_DIS : C_BORDER;

    switch (c.kind) {
    case EMBI_PCTL_TEXT:
    case EMBI_PCTL_TEXTAREA: {
        if (chrome && !own_border) {
            float r2[4];
            memcpy(r2, radii, sizeof r2);
            if (r2[0] == 0 && r2[1] == 0 && r2[2] == 0 && r2[3] == 0) {
                radii_all(2.0f, r2);
            }
            box_border(p, bb, r2, 1.0f, bcol, xf, clip);
        }
        float tc[4];
        text_content(bb, bdr, pad, chrome, tc);
        paint_text(p, s, &c, tc, xf, clip, b->el);
        break;
    }
    case EMBI_PCTL_CHECKBOX:
    case EMBI_PCTL_RADIO: {
        if (!chrome || has_author_bg(s)) { /* an author background replaces the native look (as for the text fields) */
            break;
        }
        float d = fminf(bb[2], bb[3]);
        float r[4] = {bb[0] + (bb[2] - d) * 0.5f, bb[1] + (bb[3] - d) * 0.5f, d, d};
        bool radio = c.kind == EMBI_PCTL_RADIO;
        float rr[4];
        radii_all(radio ? d * 0.5f : 2.0f, rr);
        uint32_t accent = c.disabled ? C_BLUE_DIS : C_BLUE;
        bool on = c.checked || (!radio && c.indeterminate);
        if (radio) {
            (void)embi_pb_fill(p, r, rr, c.disabled ? C_FACE_DIS : C_WHITE, xf, clip);
            box_border(p, r, rr, 1.0f, on ? accent : bcol, xf, clip);
            if (c.checked) {
                float in = d * 0.25f;
                float dot[4] = {r[0] + in, r[1] + in, d - 2 * in, d - 2 * in};
                float dr[4];
                radii_all(dot[2] * 0.5f, dr);
                (void)embi_pb_fill(p, dot, dr, accent, xf, clip);
            }
        } else if (on) {
            (void)embi_pb_fill(p, r, rr, accent, xf, clip);
            if (c.indeterminate) {
                float dash[4] = {r[0] + d * 0.22f, r[1] + d * 0.44f, d * 0.56f, d * 0.12f};
                (void)embi_pb_fill(p, dash, zero, C_WHITE, xf, clip);
            } else {
                embi_ditem *it = embi_pb_item(p, EMBI_DI_CHECK, xf, clip);
                memcpy(it->rect, r, sizeof it->rect);
                it->colors[0] = C_WHITE;
            }
        } else {
            (void)embi_pb_fill(p, r, rr, c.disabled ? C_FACE_DIS : C_WHITE, xf, clip);
            box_border(p, r, rr, 1.0f, bcol, xf, clip);
        }
        break;
    }
    case EMBI_PCTL_SELECT: {
        if (chrome && !own_border) {
            float r2[4];
            radii_all(4.0f, r2);
            box_border(p, bb, r2, 1.0f, bcol, xf, clip);
        }
        uint32_t color = 0xFF000000u;
        (void)embi_pb_color(s, embi_style_value(s, EMBI_PROP_color), &color);
        if (c.disabled) {
            color = with_alpha(color, 0.6f);
        }
        float tc[4];
        text_content(bb, bdr, pad, chrome, tc);
        float arrow = chrome ? 14.0f : 0.0f;
        tc[2] = fmaxf(0.0f, tc[2] - arrow);
        if (chrome) {
            float ar[4] = {bb[0] + bb[2] - 12.0f, bb[1] + (bb[3] - 5.0f) * 0.5f, 8.0f, 5.0f};
            embi_pb_triangle(p, ar, EMBI_TRI_DOWN, color, xf, clip);
        }
        if (c.text_len > 0) {
            cl_layout L;
            cl_build(&L, p->text, s, c.text, c.text_len, tc[2], false);
            float zr[4] = {0, 0, 0, 0};
            uint32_t lclip = embi_pb_clip(p, tc, zr, xf, clip);
            float top = tc[1] + (tc[3] - L.lh) * 0.5f;
            for (uint32_t i = 0; i < L.pc.len; i++) {
                emit_text(p, &L.pc.data[i], tc[0], top, top + L.ca, L.lh, color, xf, lclip);
            }
            cl_adopt(&L, p->d);
            cl_free(&L, false);
        }
        if (c.open) {
            p->popup = true;
            p->popup_ctl = c;
            memcpy(p->popup_bb, bb, sizeof p->popup_bb);
            /* The popup is drawn over the page in the view's own space: the scroll containers around the select have moved
             * its box (scroll60), and a scroll moves it by its own rule (it flips above the select when there is no room). */
            float ux, uy;
            embi_pb_unscroll_delta(p, xf, &ux, &uy);
            p->popup_bb[0] += ux;
            p->popup_bb[1] += uy;
            embi_pb_scroll_rigid(p);
            p->popup_s = s;
        }
        break;
    }
    case EMBI_PCTL_LISTBOX: {
        if (chrome && !own_border) {
            float r2[4];
            radii_all(2.0f, r2);
            box_border(p, bb, r2, 1.0f, bcol, xf, clip);
        }
        float area[4];
        text_content(bb, bdr, pad, chrome, area);
        paint_rows(p, s, &c, area, xf, clip, false);
        break;
    }
    case EMBI_PCTL_RANGE: {
        double span = c.max - c.min;
        double frac = span > 0 ? (c.value - c.min) / span : 0.0;
        if (!chrome) {
            /* appearance: none: only what the author styles shows, the track and the thumb from the styles of
             * ::-webkit-slider-runnable-track and ::-webkit-slider-thumb */
            const embi_style *ts, *hs;
            if (range_parts(p, b->el, &ts, &hs)) {
                float content[4] = {bb[0] + bdr[0] + pad[0], bb[1] + bdr[1] + pad[1],
                                    fmaxf(0.0f, bb[2] - bdr[0] - pad[0] - bdr[2] - pad[2]),
                                    fmaxf(0.0f, bb[3] - bdr[1] - pad[1] - bdr[3] - pad[3])};
                range_geo g;
                range_geometry(ts, hs, content, frac, &g);
                if (ts != nullptr && !embi_pb_hidden(ts)) {
                    paint_part(p, ts, g.track, xf, clip);
                }
                if (hs != nullptr && !embi_pb_hidden(hs)) {
                    paint_part(p, hs, g.thumb, xf, clip);
                }
            }
            break;
        }
        float d = fminf(bb[3], 16.0f);
        float th = 4.0f;
        float tx = bb[0] + d * 0.5f, tw = fmaxf(0.0f, bb[2] - d);
        float ty = bb[1] + (bb[3] - th) * 0.5f;
        float rr[4];
        radii_all(th * 0.5f, rr);
        float track[4] = {bb[0] + 1.0f, ty, bb[2] - 2.0f, th};
        (void)embi_pb_fill(p, track, rr, C_TRACK, xf, clip);
        float px = tx + (float)frac * tw;
        float fill[4] = {track[0], ty, px - track[0], th};
        (void)embi_pb_fill(p, fill, rr, c.disabled ? C_BLUE_DIS : C_BLUE, xf, clip);
        float thumb[4] = {px - d * 0.5f, bb[1] + (bb[3] - d) * 0.5f, d, d};
        float tr[4];
        radii_all(d * 0.5f, tr);
        (void)embi_pb_fill(p, thumb, tr, c.disabled ? C_BLUE_DIS : C_BLUE, xf, clip);
        break;
    }
    case EMBI_PCTL_PROGRESS:
    case EMBI_PCTL_METER: {
        if (!chrome) {
            break;
        }
        float rr[4];
        radii_all(fminf(bb[3] * 0.5f, 8.0f), rr);
        (void)embi_pb_fill(p, bb, rr, RGBA(230, 230, 230), xf, clip);
        double span = c.max - c.min;
        double frac = span > 0 ? (c.value - c.min) / span : 0.0;
        if (frac < 0) {
            frac = 0;
        }
        if (frac > 1) {
            frac = 1;
        }
        uint32_t col = C_BLUE;
        float bar[4] = {bb[0], bb[1], bb[2] * (float)frac, bb[3]};
        if (c.kind == EMBI_PCTL_METER) {
            meter_colors(&c, &col);
        } else if (c.indeterminate_bar) {
            bar[0] = bb[0] + bb[2] * 0.35f;
            bar[2] = bb[2] * 0.3f;
        } else {
            bar[2] = bb[2] * (float)(c.max > 0 ? c.value / c.max : 0.0);
        }
        (void)embi_pb_fill(p, bar, rr, col, xf, clip);
        break;
    }
    default:
        break;
    }
    embi_sb_free(&sb);
}

/* The popup list of the open select, on top of the page (called once at the end of the build). */
void embi_pb_control_popup(embi_pst *p)
{
    if (!p->popup) {
        return;
    }
    const embi_paint_ctl *c = &p->popup_ctl;
    float h = (float)c->nopts * EMBI_CTL_ROW_H;
    if (c->nopts == 0 || !(h > 0)) {
        return;
    }
    float area[4] = {p->popup_bb[0], p->popup_bb[1] + p->popup_bb[3], p->popup_bb[2], h};
    if (area[1] + h > p->vh && p->popup_bb[1] - h >= 0) {
        area[1] = p->popup_bb[1] - h; /* flips above when there is no room below */
    }
    float rr[4];
    radii_all(2.0f, rr);
    (void)embi_pb_fill(p, area, rr, C_WHITE, 0, 0);
    paint_rows(p, p->popup_s, c, area, 0, 0, true);
    box_border(p, area, rr, 1.0f, C_BORDER, 0, 0);
}

/* ------------------------------------------------------------------ pointer and caret queries */

typedef struct hitctx {
    embi_paint_ctl c;
    embi_sb sb;
    dtext d;
    cl_layout L;
    float cb[4];
    bool ok;
} hitctx;

static void hit_free(hitctx *h)
{
    if (h->ok) {
        cl_free(&h->L, true);
    }
    embi_sb_free(&h->d.sb);
    embi_sb_free(&h->sb);
}

static bool hit_make(hitctx *h, const embi_layout_engine *e, const embi_element *el, const float bb[4])
{
    memset(h, 0, sizeof *h);
    const embi_box *bx = el != nullptr ? (const embi_box *)el->box : nullptr;
    if (bx == nullptr || bx->style == nullptr) {
        return false;
    }
    void *fu = nullptr;
    embi_layout_control_fn ff = embi_layout_control_hook(e, &fu);
    ctl_state(ff, fu, el, &h->c, &h->sb);
    if (h->c.kind != EMBI_PCTL_TEXT && h->c.kind != EMBI_PCTL_TEXTAREA) {
        embi_sb_free(&h->sb);
        return false;
    }
    float bdr[4], pad[4], mg[4];
    if (!embi_layout_box_edges(e, el, bdr, pad, mg)) {
        embi_sb_free(&h->sb);
        return false;
    }
    const embi_style *s = bx->style;
    text_content(bb, bdr, pad, !appearance_none(s), h->cb);
    dtext_make(&h->d, &h->c);
    bool multi = h->c.kind == EMBI_PCTL_TEXTAREA;
    cl_build(&h->L, embi_layout_text(e), s, h->d.sb.data != nullptr ? h->d.sb.data : "", h->d.sb.len, h->cb[2],
             multi && !h->c.nowrap);
    h->ok = true;
    return true;
}

bool embi_paint_control_offset(const embi_layout_engine *e, const embi_element *el, const float bb[4], float x, float y,
                               uint32_t *byte)
{
    hitctx h;
    if (!hit_make(&h, e, el, bb)) {
        return false;
    }
    bool multi = h.c.kind == EMBI_PCTL_TEXTAREA;
    uint32_t cpi = 0;
    float cx = 0;
    cl_locate(&h.L, to_disp(&h.d, h.c.caret), &cpi, &cx);
    float sx = 0, sy = 0;
    if (!multi && cx > h.cb[2] - 1.0f) {
        sx = cx - (h.cb[2] - 1.0f);
    }
    if (multi) {
        float bottom = h.L.pc.data[cpi].y + h.L.lh;
        if (bottom > h.cb[3]) {
            sy = bottom - h.cb[3];
        }
    }
    uint32_t pi = 0;
    if (multi) {
        float ry = y - (h.cb[1] - sy);
        int32_t li = (int32_t)floorf(ry / h.L.lh);
        if (li < 0) {
            li = 0;
        }
        if ((uint32_t)li >= h.L.pc.len) {
            li = (int32_t)h.L.pc.len - 1;
        }
        pi = (uint32_t)li;
    }
    uint32_t d = cl_offset_in(&h.L, h.d.sb.data != nullptr ? h.d.sb.data : "", pi, x - (h.cb[0] - sx));
    *byte = to_value(&h.d, d);
    hit_free(&h);
    return true;
}

bool embi_paint_control_caret(const embi_layout_engine *e, const embi_element *el, const float bb[4], float rect[4])
{
    hitctx h;
    if (!hit_make(&h, e, el, bb)) {
        return false;
    }
    bool multi = h.c.kind == EMBI_PCTL_TEXTAREA;
    uint32_t cpi = 0;
    float cx = 0;
    cl_locate(&h.L, to_disp(&h.d, h.c.caret), &cpi, &cx);
    float sx = 0, sy = 0;
    if (!multi && cx > h.cb[2] - 1.0f) {
        sx = cx - (h.cb[2] - 1.0f);
    }
    if (multi) {
        float bottom = h.L.pc.data[cpi].y + h.L.lh;
        if (bottom > h.cb[3]) {
            sy = bottom - h.cb[3];
        }
    }
    float top = multi ? h.cb[1] - sy + h.L.pc.data[cpi].y : h.cb[1] + (h.cb[3] - h.L.lh) * 0.5f;
    rect[0] = h.cb[0] - sx + cx;
    rect[1] = top;
    rect[2] = 1.0f;
    rect[3] = h.L.lh;
    hit_free(&h);
    return true;
}

bool embi_paint_range_track(const embi_layout_engine *e, embi_style_engine *se, const embi_element *el, const float bb[4],
                            float *left, float *width, float *thumb_w)
{
    const embi_box *bx = el != nullptr ? (const embi_box *)el->box : nullptr;
    if (se == nullptr || bx == nullptr || bx->style == nullptr || !appearance_none(bx->style)) {
        return false;
    }
    const embi_style *ts = embi_style_generated(se, (embi_element *)(uintptr_t)el, EMBI_PE_SLIDER_TRACK);
    const embi_style *hs = embi_style_generated(se, (embi_element *)(uintptr_t)el, EMBI_PE_SLIDER_THUMB);
    if (ts == nullptr && hs == nullptr) {
        return false;
    }
    float bdr[4], pad[4], mg[4];
    if (!embi_layout_box_edges(e, el, bdr, pad, mg)) {
        return false;
    }
    float content[4] = {bb[0] + bdr[0] + pad[0], bb[1] + bdr[1] + pad[1], fmaxf(0.0f, bb[2] - bdr[0] - pad[0] - bdr[2] - pad[2]),
                        fmaxf(0.0f, bb[3] - bdr[1] - pad[1] - bdr[3] - pad[3])};
    range_geo g;
    range_geometry(ts, hs, content, 0.0, &g);
    *left = g.tl;
    *width = g.tw;
    *thumb_w = g.thumb[2];
    return true;
}
