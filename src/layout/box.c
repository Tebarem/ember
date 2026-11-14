/*
 * Box tree construction (W3a, section 1). Walks the DOM of one document with its computed styles and
 * builds the boxes of the layout. The result is a pure function of (DOM subtree, styles), so it is
 * rebuilt from scratch on every layout.
 *
 * What is built here:
 *   - one box per element with a box (display from W2; display: none and contents are handled);
 *   - text boxes, one per text node run after white space processing (whitespace.c);
 *   - inline boxes split around block-level children (CSS 2.1 section 9.2.1.1);
 *   - anonymous blocks around inline runs next to blocks, and whitespace-only runs dropped there;
 *   - ::before, ::after and ::marker boxes with their generated text (counters.c);
 *   - replaced boxes with their natural sizes, and form controls with fixed sizes.
 *
 * Table objects (CSS Tables 3 section 3, W3g): each table-internal box gets its role (box.h, tr), and
 * the missing parts are generated: anonymous rows and cells inside a table, row group or row, and an
 * anonymous table around table-internal boxes that sit in a block container. Whitespace-only text is
 * removed between table parts.
 *
 * Not yet: anonymous flex and grid items beyond the text wrapping, and url() images in generated
 * content (R5).
 */
#ifndef EMBER_SVG
#error "EMBER_SVG must be defined to 0 or 1 (CMakeLists.txt adds it for every target)"
#endif
#include "layout_int.h"

#include "css/props.h"
#include "dom/dom.h"

#include <SDL3/SDL_stdinc.h>
#include <string.h>

/* An ordered list of boxes under construction. */
typedef struct embi_blist {
    embi_box *head, *tail;
} embi_blist;

/* White space state shared by the inline content of one block container. */
typedef struct embi_ws {
    bool prev_space;
    embi_box *trim; /* a text box ending in a collapsible space, removed at the end of the line */
    bool in_word;   /* the text so far ends inside a word (text-transform: capitalize across boxes) */
    bool zw_last;   /* the text so far ends in U+200B: a segment break at the start of the next text is removed */
} embi_ws;

/* What a child inherits from its parent while the box tree is built. */
typedef struct embi_bcx {
    const embi_style *inh; /* the nearest element style (inherited properties) */
    embi_ws *ws;
    bool blockify; /* a child of a flex or grid container: inline-level children become blocks */
    bool in_inline; /* a child of an inline box: table parts here get an anonymous inline-table, which is atomic */
} embi_bcx;

/* ASCII case-insensitive comparison with a lowercase literal. */
static bool is_lit(embi_str s, const char *lit)
{
    size_t n = strlen(lit);
    if (s.len != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char ch = s.ptr[i];
        if (ch >= 'A' && ch <= 'Z') {
            ch = (char)(ch - 'A' + 'a');
        }
        if (ch != lit[i]) {
            return false;
        }
    }
    return true;
}

static uint32_t list_item_atom(void)
{
    return embi_atom_intern("list-item", 9);
}

static void bl_push(embi_blist *l, embi_box *b)
{
    b->next = nullptr;
    if (l->tail != nullptr) {
        l->tail->next = b;
    } else {
        l->head = b;
    }
    l->tail = b;
}

static void bl_attach(embi_box *parent, const embi_blist *l)
{
    parent->first = l->head;
    for (embi_box *ch = l->head; ch != nullptr; ch = ch->next) {
        ch->parent = parent;
    }
}

/* What an element's name means to the box build, by (atom, namespace): the name is compared as text once per
 * name, not for every element of the page (and embi_atom_str takes the atom table lock). */
enum { TAG_OL = 1, TAG_INPUT = 2, TAG_BR = 4, TAG_BUTTON = 8, TAG_REPLACED = 16, TAG_WBR = 32, TAG_FIELDSET = 64 };

static uint8_t tag_compute(const embi_element *el)
{
    embi_str name = embi_atom_str(el->base.name);
    if (el->base.extra == EMBI_NS_SVG) {
        return is_lit(name, "svg") ? TAG_REPLACED : 0;
    }
    if (el->base.extra != EMBI_NS_HTML) {
        return 0;
    }
    uint8_t f = 0;
    f |= is_lit(name, "ol") ? TAG_OL : 0;
    f |= is_lit(name, "input") ? TAG_INPUT : 0;
    f |= is_lit(name, "br") ? TAG_BR : 0;
    f |= is_lit(name, "wbr") ? TAG_WBR : 0;
    f |= is_lit(name, "button") ? TAG_BUTTON : 0;
    f |= is_lit(name, "fieldset") ? TAG_FIELDSET : 0;
    if (is_lit(name, "img") || is_lit(name, "canvas") || is_lit(name, "video") || is_lit(name, "iframe") ||
        is_lit(name, "embed") || is_lit(name, "object") || is_lit(name, "input") || is_lit(name, "select") ||
        is_lit(name, "textarea") || is_lit(name, "progress") || is_lit(name, "meter")) {
        f |= TAG_REPLACED;
    }
    return f;
}

static uint8_t tag_flags(embi_lctx *c, const embi_element *el)
{
    embi_atom a = el->base.name;
    uint16_t ns = el->base.extra;
    uint32_t slot = ((uint32_t)a * 0x9E3779B1u >> 24) & 255u;
    if (c->ld->tagc[slot].a != a || c->ld->tagc[slot].ns != ns || a == 0) {
        c->ld->tagc[slot].a = a;
        c->ld->tagc[slot].ns = ns;
        c->ld->tagc[slot].f = tag_compute(el);
    }
    return c->ld->tagc[slot].f;
}

/* Sets the element of a box; remembers whether it is an HTML button (block.c reads it for every block, and the
 * element is cold memory by then). */
static void set_el(embi_lctx *c, embi_box *b, embi_element *el)
{
    b->el = el;
    if (el != nullptr && (tag_flags(c, el) & TAG_BUTTON) != 0) {
        b->flags2 |= EMBI_BF2_BUTTON | EMBI_BF2_CHROME;
    }
}

static embi_box *new_box(embi_lctx *c, uint8_t kind)
{
    /* A text run is allocated without the last 8 bytes (rw, rh, cq_w, cq_h), which it never reads; sanitizer
     * builds allocate them and poison them (embi_lay_unpoison_arena frees the poison with the tree). */
    bool text = kind == EMBI_BK_TEXT || kind == EMBI_BK_MARKER;
#ifdef EMBI_LAY_ASAN
    embi_box *b = EMBI_LAY_NEW(c, embi_box);
    memset(b, 0, sizeof *b);
    if (text) {
        ASAN_POISON_MEMORY_REGION((char *)b + EMBI_BSZ_TEXT, sizeof *b - EMBI_BSZ_TEXT);
    }
#else
    size_t sz = text ? EMBI_BSZ_TEXT : sizeof(embi_box);
    embi_box *b = (embi_box *)embi_arena_alloc(&c->ld->ba, sz, alignof(embi_box));
    memset(b, 0, sz);
#endif
    b->kind = kind;
    return b;
}

static uint16_t display_kw(const embi_style *s)
{
    const embi_value *d = embi_style_value(s, EMBI_PROP_display);
    if (d == nullptr || d->kind != EMBI_V_KEYWORD) {
        return EMBI_KW_block;
    }
    return d->kw;
}

static uint16_t keyword_of(const embi_style *s, embi_prop_id p)
{
    const embi_value *v = embi_style_value(s, p);
    return v != nullptr && v->kind == EMBI_V_KEYWORD ? v->kw : 0;
}

/* Appends the value of an attribute (UTF-8) to sb. Returns false when the attribute is absent. */
static bool attr_text(embi_element *el, const char *name, embi_sb *sb)
{
    embi_str q = {name, (uint32_t)strlen(name)};
    const embi_attr *a = embi_el_get_attribute(el, q);
    if (a == nullptr) {
        return false;
    }
    embi_dstr_append_utf8(sb, a->value);
    return true;
}

/* An integer attribute (leading white space and an optional sign allowed). */
static bool attr_int(embi_element *el, const char *name, int32_t *out)
{
    embi_sb sb = {0};
    if (!attr_text(el, name, &sb)) {
        embi_sb_free(&sb);
        return false;
    }
    const char *p = sb.data != nullptr ? sb.data : "";
    const char *end = p + sb.len;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f')) {
        p++;
    }
    bool neg = false;
    if (p < end && (*p == '-' || *p == '+')) {
        neg = *p == '-';
        p++;
    }
    bool any = false;
    int64_t v = 0;
    bool big = false;
    while (p < end && *p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        big = big || v > 2147483648LL;
        if (big) {
            v = 0;
        }
        any = true;
        p++;
    }
    embi_sb_free(&sb);
    /* HTML's rules for parsing integers: a value outside the 32-bit range is an error, so the attribute is ignored. */
    if (!any || big || (!neg && v > 2147483647LL)) {
        return false;
    }
    *out = (int32_t)(neg ? -v : v);
    return true;
}

/* ------------------------------------------------------------------ replaced elements */

/* Natural sizes of the form controls, as border boxes: what offsetWidth/offsetHeight give for the
 * control with the default styling (appearance: auto) in Chromium on Windows. MEASURED in Chromium
 * with the default font Arial 13.333px (and the default monospace font, Courier New 13.333px, for
 * the textarea); the text, textarea and select widths depend on the font, so they are right for
 * that font only. Ember's UA sheet gives input and textarea box-sizing: border-box (as the HTML
 * rendering section suggests) where Chromium computes content-box for text fields (169 content +
 * 4 padding + 4 border = 177), so a control with no specified size on an axis has these constants as
 * its border box either way, and with border-box sizing the content size is the constant minus the
 * box's border and padding (see embi_lay_replaced). Derivation, where there is one:
 *   - height 21 of the single-line text-like inputs and the file input: a 15px line (Arial
 *     13.333px, line-height normal) + 2 x 1px padding + 2 x 2px border.
 *   - checkbox and radio 13 x 13, range 129 x 16, progress 160 x 16: the themed glyph sizes.
 * Types and elements that are NOT in this table:
 *   - input button, submit and reset are block containers holding their label (box.c
 *     button_input): Chromium gives 58 x 21 for submit with the default label and 16 x 21 for an
 *     empty button, which is padding + border around the text.
 *   - input hidden has no box; input image has no size without a loaded image (0 x 0, R5 sizes it).
 *   - time, datetime-local, month and week are NOT MEASURED: they use the date size.
 *   - select is the size of an EMPTY select (22 x 19); W5 sizes a select by its options.
 */
typedef struct embi_ctl_size {
    const char *type;
    float w, h;
} embi_ctl_size;

static const embi_ctl_size ctl_sizes[] = {
    {"checkbox", 13, 13},        {"radio", 13, 13},      {"range", 129, 16},
    {"color", 50, 27},           {"text", 177, 21},      {"password", 177, 21},
    {"search", 177, 21},         {"url", 177, 21},       {"email", 177, 21},
    {"tel", 177, 21},            {"number", 177, 21},    {"date", 116, 21},
    {"time", 116, 21},           {"datetime-local", 116, 21},
    {"month", 116, 21},          {"week", 116, 21},      {"file", 253, 21},
    {"image", 0, 0},             {nullptr, 0, 0},
};

#define SELECT_W 22.0f
#define SELECT_H 19.0f
#define TEXTAREA_W 168.0f
#define TEXTAREA_H 36.0f
#define METER_W 80.0f
#define METER_H 16.0f
#define PROGRESS_W 160.0f
#define PROGRESS_H 16.0f

/* ---- W5e: intrinsic sizes of the text controls and the select, from size / cols / rows, the font and the options ---- */

/* The font size of a control in px (13.333 when the style has none). */
static float ctl_font_px(const embi_box *b)
{
    const embi_value *v = b->style != nullptr ? embi_style_value(b->style, EMBI_PROP_font_size) : nullptr;
    return v != nullptr && v->kind == EMBI_V_LENGTH && v->unit == EMBI_U_PX && v->num > 0 ? (float)v->num : 13.3333f;
}

/* The height of a line of "normal" line height, as browsers round it (15 at 13.333px). */
static float ctl_line_px(float fs)
{
    return floorf(fs * 1.125f + 0.5f);
}

/* A positive integer attribute, else def. */
static uint32_t ctl_count_attr(embi_element *el, const char *name, uint32_t def)
{
    int32_t v = 0;
    if (attr_int(el, name, &v) && v > 0) {
        return (uint32_t)v;
    }
    return def;
}

/* Whether the given size property of the box is min-content, max-content or fit-content. */
static bool ctl_axis_keyword(const embi_box *b, embi_prop_id id)
{
    const embi_value *v = b->style != nullptr ? embi_style_value(b->style, id) : nullptr;
    if (v == nullptr) {
        return false;
    }
    if (v->kind == EMBI_V_KEYWORD) {
        return v->kw == EMBI_KW_min_content || v->kw == EMBI_KW_max_content || v->kw == EMBI_KW_fit_content;
    }
    return v->kind == EMBI_V_FUNC && v->func.name != 0 && is_lit(embi_atom_str(v->func.name), "fit-content");
}

/* Whether the box's width or height is an intrinsic keyword (a select keeps its fixed sizes then, as before W5e). */
static bool ctl_intrinsic_keyword(const embi_box *b)
{
    return ctl_axis_keyword(b, EMBI_PROP_width) || ctl_axis_keyword(b, EMBI_PROP_height);
}

/* The content measure of an input (its value) or a textarea (its text): the widest line in px and the number of lines. */
/* The width of a short literal in the control's font (zero when it cannot be shaped). */
static void ctl_text_measure_str(embi_lctx *c, const embi_box *b, const char *lit, float *w, uint32_t *lines)
{
    *w = 0.0f;
    *lines = 1;
    if (b->style == nullptr || c->text == nullptr) {
        return;
    }
    embi_lay_tstyle ts;
    embi_str fams[16];
    embi_lay_text_style(b->style, &ts, fams, 16);
    embi_lay_shaped *run = embi_tshim_shape(c->text, lit, (uint32_t)strlen(lit), &ts);
    if (run != nullptr) {
        *w = embi_tshim_advance(run);
        embi_tshim_free(run);
    }
}

#define LFCH 10
#define CRCH 13
static void ctl_text_measure(embi_lctx *c, const embi_box *b, embi_element *el, bool area, float *widest, uint32_t *lines)
{
    *widest = 0.0f;
    *lines = 1;
    if (b->style == nullptr || c->text == nullptr) {
        return;
    }
    embi_sb t = {0};
    if (area) {
        for (embi_node *n = el->base.first_child; n != nullptr; n = n->next) {
            if (n->type == EMBI_NODE_TEXT) {
                embi_dstr_append_utf8(&t, embi_as_chardata(n)->data);
            }
        }
    } else {
        (void)attr_text(el, "value", &t);
    }
    embi_lay_tstyle ts;
    embi_str fams[16];
    embi_lay_text_style(b->style, &ts, fams, 16);
    uint32_t start = 0;
    for (uint32_t i = 0; i <= t.len; i++) {
        bool end = i == t.len;
        if (end || t.data[i] == LFCH || t.data[i] == CRCH) {
            uint32_t len = i - start;
            if (len > 0) {
                embi_lay_shaped *run = embi_tshim_shape(c->text, t.data + start, len, &ts);
                if (run != nullptr) {
                    float w = embi_tshim_advance(run);
                    *widest = w > *widest ? w : *widest;
                    embi_tshim_free(run);
                }
            }
            if (!end) {
                if (t.data[i] == CRCH && i + 1 < t.len && t.data[i + 1] == LFCH) {
                    i++;
                }
                (*lines)++;
            }
            start = i + 1;
        }
    }
    embi_sb_free(&t);
}

/* The row height of a select list box (the painter draws its rows at the same height: EMBI_CTL_ROW_H). */
#define CTL_ROW_H 24.0f

/* The text of an option as the select shows it: the label attribute, else the text with white space collapsed. */
static void option_text(embi_element *opt, embi_sb *out)
{
    if (attr_text(opt, "label", out) && out->len > 0) {
        return;
    }
    out->len = 0;
    bool space = true;
    for (embi_node *n = opt->base.first_child; n != nullptr;) {
        if (n->type == EMBI_NODE_TEXT) {
            embi_sb tmp = {0};
            embi_dstr_append_utf8(&tmp, embi_as_chardata(n)->data);
            for (uint32_t i = 0; i < tmp.len; i++) {
                char ch = tmp.data[i];
                if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f') {
                    if (!space) {
                        embi_sb_append_char(out, ' ');
                        space = true;
                    }
                } else {
                    embi_sb_append_char(out, ch);
                    space = false;
                }
            }
            embi_sb_free(&tmp);
        }
        if (n->first_child != nullptr && n->type == EMBI_NODE_ELEMENT) {
            n = n->first_child;
            continue;
        }
        while (n != nullptr && n != &opt->base && n->next == nullptr) {
            n = n->parent;
        }
        if (n == nullptr || n == &opt->base) {
            break;
        }
        n = n->next;
    }
    while (out->len > 0 && out->data[out->len - 1] == ' ') {
        out->len--;
    }
}

/* The widest option of a select in px. */
static float select_text_width(embi_lctx *c, const embi_box *b, embi_element *sel)
{
    if (b->style == nullptr || c->text == nullptr) {
        return 0.0f;
    }
    embi_lay_tstyle ts;
    embi_str fams[16];
    embi_lay_text_style(b->style, &ts, fams, 16);
    float widest = 0.0f;
    embi_node *n = sel->base.first_child;
    while (n != nullptr) {
        if (n->type == EMBI_NODE_ELEMENT) {
            /* display: none on an option or optgroup removes it (and what is inside it) from the select's sizing */
            const embi_style *os = embi_style_get(c->style, embi_as_element(n), EMBI_PE_NONE);
            if (os != nullptr && display_kw(os) == EMBI_KW_none) {
                while (n != nullptr && n != &sel->base && n->next == nullptr) {
                    n = n->parent;
                }
                if (n == nullptr || n == &sel->base) {
                    break;
                }
                n = n->next;
                continue;
            }
        }
        if (n->type == EMBI_NODE_ELEMENT && n->extra == EMBI_NS_HTML && is_lit(embi_atom_str(n->name), "option")) {
            embi_sb t = {0};
            option_text(embi_as_element(n), &t);
            if (t.len > 0) {
                embi_lay_shaped *run = embi_tshim_shape(c->text, t.data, t.len, &ts);
                if (run != nullptr) {
                    float w = embi_tshim_advance(run);
                    widest = w > widest ? w : widest;
                    embi_tshim_free(run);
                }
            }
            embi_sb_free(&t);
        }
        if (n->first_child != nullptr && n->type == EMBI_NODE_ELEMENT && !is_lit(embi_atom_str(n->name), "option")) {
            n = n->first_child;
            continue;
        }
        while (n != nullptr && n != &sel->base && n->next == nullptr) {
            n = n->parent;
        }
        if (n == nullptr || n == &sel->base) {
            break;
        }
        n = n->next;
    }
    return widest;
}

/* A form control: its size is a border box, and it has no aspect ratio. */
static void set_control(embi_box *b, float w, float h)
{
    b->rw = w;
    b->rh = h;
    b->flags |= EMBI_BF_REPLACED;
    b->flags2 |= EMBI_BF2_CONTROL;
}

static void set_replaced(embi_box *b, float w, float h, bool ratio)
{
    b->rw = w;
    b->rh = h;
    b->flags |= EMBI_BF_REPLACED;
    if (ratio) {
        b->flags |= EMBI_BF_HAS_RATIO;
    }
}

/* A length attribute of an svg root: a plain number, optionally with px. False when it is absent
 * or given as a percentage or in another unit. */
static bool svg_len_attr(embi_element *el, const char *name, float *out)
{
    embi_sb sb = {0};
    bool ok = false;
    if (attr_text(el, name, &sb) && sb.len > 0 && sb.len < 64) {
        char tmp[64];
        memcpy(tmp, sb.data, sb.len);
        tmp[sb.len] = '\0';
        char *end = nullptr;
        double v = SDL_strtod(tmp, &end);
        if (end != tmp && v >= 0 && isfinite((float)v)) {
            while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') {
                end++;
            }
            if (*end == '\0' || (end[0] == 'p' && end[1] == 'x' && end[2] == '\0')) {
                *out = (float)v;
                ok = true;
            }
        }
    }
    embi_sb_free(&sb);
    return ok;
}

/* The viewBox of an svg root: four numbers whose last two (the size) are positive. */
static bool svg_viewbox(embi_element *el, float *vw, float *vh)
{
    embi_sb sb = {0};
    bool ok = false;
    if (attr_text(el, "viewBox", &sb) && sb.len > 0 && sb.len < 256) {
        char tmp[256];
        memcpy(tmp, sb.data, sb.len);
        tmp[sb.len] = '\0';
        double v[4] = {0, 0, 0, 0};
        const char *p = tmp;
        bool good = true;
        for (int i = 0; i < 4 && good; i++) {
            while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n' || *p == '\r') {
                p++;
            }
            char *e = nullptr;
            v[i] = SDL_strtod(p, &e);
            if (e == p) {
                good = false;
            }
            p = e;
        }
        if (good && v[2] > 0 && v[3] > 0) {
            *vw = (float)v[2];
            *vh = (float)v[3];
            ok = true;
        }
    }
    embi_sb_free(&sb);
    return ok;
}

/* The lowercased type attribute of an input ("text" when absent, empty or too long). */
static void input_type(embi_element *el, char type[24])
{
    embi_sb sb = {0};
    memcpy(type, "text", 5);
    if (attr_text(el, "type", &sb) && sb.len > 0 && sb.len < 24) {
        for (uint32_t i = 0; i < sb.len; i++) {
            char ch = sb.data[i];
            type[i] = (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
        }
        type[sb.len] = '\0';
    }
    embi_sb_free(&sb);
}

/* Gives a replaced box its natural size. Returns false when the element is not replaced (or is
 * hidden as an input). */
static bool replaced_for(embi_lctx *c, embi_box *b, embi_element *el)
{
    embi_str name = embi_atom_str(el->base.name);
    uint16_t ns = el->base.extra;
    if (ns == EMBI_NS_SVG) {
        if (!is_lit(name, "svg")) {
            return false;
        }
        /* The size of an svg root: its width and height. A missing one comes from the viewBox aspect
         * ratio when there is one, else from the 300x150 default of replaced elements. */
        float w = 0, h = 0, vw = 0, vh = 0;
        bool nosize = false;
        bool has_w = svg_len_attr(el, "width", &w);
        bool has_h = svg_len_attr(el, "height", &h);
        bool has_vb = svg_viewbox(el, &vw, &vh);
        if (!has_w || !has_h) {
            if (has_vb) {
                float r = vw / vh;
                if (has_w) {
                    h = w / r;
                } else if (has_h) {
                    w = h * r;
                } else {
                    w = 300.0f;
                    h = 300.0f / r;
                    nosize = true;
                }
            } else {
                if (!has_w) {
                    w = 300.0f;
                }
                if (!has_h) {
                    h = 150.0f;
                }
            }
        }
        set_replaced(b, w, h, has_vb || (has_w && has_h));
        if (nosize) {
            b->flags2 |= EMBI_BF2_NOSIZE;
        }
        return true;
    }
    if (ns != EMBI_NS_HTML) {
        return false;
    }
    if (is_lit(name, "img")) {
        float w = 0, h = 0;
        bool has_ratio = false;
        embi_sb sb = {0}, ss = {0};
        bool has_src = attr_text(el, "src", &sb);
        bool has_set = attr_text(el, "srcset", &ss);
        if ((has_src || has_set) && c->image_size != nullptr) {
            c->ld->has_images = true;
            float dens = 1.0f;
            embi_str url = embi_lay_img_source((embi_str){sb.data != nullptr ? sb.data : "", sb.len},
                                               (embi_str){ss.data != nullptr ? ss.data : "", ss.len}, &dens);
            if (url.len == 0 || !c->image_size(c->image_user, url, &w, &h, &has_ratio)) {
                w = h = 0;
                has_ratio = false;
            } else if (dens > 0.0f && dens != 1.0f) {
                w /= dens; /* a srcset density candidate (HTML section 4.8.4.3) */
                h /= dens;
            }
        }
        embi_sb_free(&sb);
        embi_sb_free(&ss);
        if (w == 0 && h == 0) {
            int32_t aw = 0, ah = 0;
            if (attr_int(el, "width", &aw) && attr_int(el, "height", &ah)) {
                w = (float)aw;
                h = (float)ah;
                has_ratio = true;
            }
        }
        set_replaced(b, w, h, has_ratio);
        return true;
    }
    if (is_lit(name, "canvas")) {
        /* A missing, invalid or negative size attribute gives the default (HTML canvas). */
        int32_t w = 300, h = 150;
        if (attr_int(el, "width", &w) && w < 0) {
            w = 300;
        }
        if (attr_int(el, "height", &h) && h < 0) {
            h = 150;
        }
        set_replaced(b, (float)w, (float)h, true);
        return true;
    }
    if (is_lit(name, "video") || is_lit(name, "iframe") || is_lit(name, "embed") ||
        is_lit(name, "object")) {
        set_replaced(b, 300, 150, true);
        return true;
    }
    if (is_lit(name, "input")) {
        char type[24];
        input_type(el, type);
        if (strcmp(type, "hidden") == 0) {
            return false;
        }
        float w = 177, h = 21;
        for (const embi_ctl_size *e = ctl_sizes; e->type != nullptr; e++) {
            if (strcmp(e->type, type) == 0) {
                w = e->w;
                h = e->h;
                break;
            }
        }
        bool text_field = strcmp(type, "text") == 0 || strcmp(type, "password") == 0 || strcmp(type, "search") == 0 ||
                          strcmp(type, "url") == 0 || strcmp(type, "email") == 0 || strcmp(type, "tel") == 0 ||
                          strcmp(type, "number") == 0;
        if (text_field) {
            /* W5e: size characters wide (20 by default) at 8.45 px a character for the 13.333px default font, scaled with
             * the font size, plus the 8 px of padding and border; one line high plus 6 px. */
            float fs = ctl_font_px(b);
            uint32_t size = ctl_count_attr(el, "size", 20);
            w = floorf((float)size * 8.45f * (fs / 13.3333f) + 0.5f) + 8.0f;
            h = ctl_line_px(fs) + 6.0f;
            if (ctl_axis_keyword(b, EMBI_PROP_width)) {
                float tw;
                uint32_t nl;
                ctl_text_measure(c, b, el, false, &tw, &nl);
                w = ceilf(tw); /* the value's width (the box is border-box, as 11ch is in the reference) */
            }
        }
        set_control(b, w, h);
        if (text_field) {
            b->tflags |= EMBI_TF_CTLBASE;
        }
        return true;
    }
    if (is_lit(name, "select") && ctl_intrinsic_keyword(b)) {
        set_control(b, SELECT_W, SELECT_H);
        b->tflags |= EMBI_TF_CTLBASE;
        return true;
    }
    if (is_lit(name, "select")) {
        /* W5e: sized by its options: the widest label plus the arrow and padding (22 px for an empty select); a list box
         * (multiple, or size above 1) shows size rows (4 by default) and has room for a scroll bar. */
        float fs = ctl_font_px(b);
        float tw = select_text_width(c, b, el);
        embi_sb probe = {0};
        bool multiple = attr_text(el, "multiple", &probe);
        embi_sb_free(&probe);
        uint32_t size = ctl_count_attr(el, "size", 0);
        if (multiple || size > 1) {
            uint32_t rows = size > 1 ? size : 4;
            set_control(b, ceilf(tw) + 4.0f + 15.0f, (float)rows * CTL_ROW_H + 2.0f);
        } else {
            float h = ctl_line_px(fs) + 4.0f;
            set_control(b, ceilf(tw) + SELECT_W, h > SELECT_H ? h : SELECT_H);
            b->tflags |= EMBI_TF_CTLBASE;
        }
        return true;
    }
    if (is_lit(name, "textarea")) {
        /* W5e: cols characters wide (20) and rows lines high (2), in the monospace font (0.6 em a character). */
        float fs = ctl_font_px(b);
        uint32_t cols = ctl_count_attr(el, "cols", 20);
        uint32_t rows = ctl_count_attr(el, "rows", 2);
        float adv = 0.6f * fs;
        {
            float zw;
            uint32_t zl;
            ctl_text_measure_str(c, b, "0", &zw, &zl);
            if (zw > 0.0f) {
                adv = zw; /* the advance of "0" in the control's own font (what cols counts) */
            }
        }
        float w = floorf((float)cols * adv + 0.5f) + 8.0f;
        float h = (float)rows * ctl_line_px(fs) + 6.0f;
        if (ctl_axis_keyword(b, EMBI_PROP_width) || ctl_axis_keyword(b, EMBI_PROP_height)) {
            float tw;
            uint32_t nl;
            ctl_text_measure(c, b, el, true, &tw, &nl);
            if (ctl_axis_keyword(b, EMBI_PROP_width)) {
                w = floorf(tw + 0.5f) + 8.0f;
            }
            if (ctl_axis_keyword(b, EMBI_PROP_height)) {
                h = (float)nl * ctl_line_px(fs) + 6.0f;
            }
        }
        set_control(b, w, h);
        return true;
    }
    if (is_lit(name, "progress")) {
        set_control(b, PROGRESS_W, PROGRESS_H);
        return true;
    }
    if (is_lit(name, "meter")) {
        set_control(b, METER_W, METER_H);
        return true;
    }
    return false;
}

/* True for an element that is a replaced kind (before the box is made). */
static bool is_replaced_element(embi_lctx *c, embi_element *el)
{
    return (tag_flags(c, el) & TAG_REPLACED) != 0;
}

/* ------------------------------------------------------------------ generated content */

/* Appends the value of an attr() function (or its fallback) to sb. */
static void gen_attr(embi_element *el, const embi_value *fn, embi_sb *sb)
{
    if (fn->func.n == 0 || fn->func.args[0].kind != EMBI_V_IDENT) {
        return;
    }
    embi_str name = embi_atom_str(fn->func.args[0].ident);
    const embi_attr *a = embi_el_get_attribute(el, name);
    if (a != nullptr) {
        embi_dstr_append_utf8(sb, a->value);
        return;
    }
    if (fn->func.n >= 2 && fn->func.args[fn->func.n - 1].kind == EMBI_V_STRING) {
        embi_sb_append(sb, fn->func.args[fn->func.n - 1].str);
    } else if (fn->func.n >= 2 && fn->func.args[fn->func.n - 1].kind == EMBI_V_RAW) {
        /* The fallback is kept as written: a string gives its text, anything else generates nothing here. */
        embi_css_slice fb = fn->func.args[fn->func.n - 1].raw;
        if (fb.n == 1 && fb.p[0].tok.type == EMBI_CSS_STRING) {
            embi_sb_append(sb, fb.p[0].tok.value);
        }
    }
}

/* Appends the value of counter() or counters() to sb. */
static void gen_counter(embi_lctx *c, const embi_value *fn, embi_sb *sb)
{
    if (fn->func.n == 0 || fn->func.args[0].kind != EMBI_V_IDENT) {
        return;
    }
    embi_atom name = fn->func.args[0].ident;
    c->ld->ctr_used = true; /* W6b: the box tree depends on the counter state; no surgery on it (incr.c) */
    c->ld->ctr_read = true;
    bool plural = is_lit(embi_atom_str(fn->func.name), "counters");
    uint16_t style = 0;
    if (plural) {
        embi_str sep = {"", 0};
        if (fn->func.n >= 2 && fn->func.args[1].kind == EMBI_V_STRING) {
            sep = fn->func.args[1].str;
        }
        if (fn->func.n >= 3 && fn->func.args[2].kind == EMBI_V_IDENT) {
            style = embi_ctr_style_name(embi_atom_str(fn->func.args[2].ident));
        }
        bool first = true;
        for (uint32_t i = 0; i < c->ctr.st.len; i++) {
            const embi_ctr_inst *in = &c->ctr.st.data[i];
            if (in->name != name) {
                continue;
            }
            if (!first) {
                embi_sb_append(sb, sep);
            }
            first = false;
            embi_ctr_format(sb, in->value, style);
        }
        return;
    }
    if (fn->func.n >= 2 && fn->func.args[1].kind == EMBI_V_IDENT) {
        style = embi_ctr_style_name(embi_atom_str(fn->func.args[1].ident));
    }
    embi_ctr_format(sb, embi_ctr_value(&c->ctr, name), style);
}

/* The language that `quotes: auto` follows for the quotation marks of element el: its parent's (the quotation sits in the
 * parent's text, css-content/quotes-030). */
static embi_str quote_lang(embi_lctx *c, embi_element *el)
{
    embi_box tmp;
    memset(&tmp, 0, sizeof tmp);
    const embi_node *up = el != nullptr ? el->base.parent : nullptr;
    tmp.el = up != nullptr && embi_is_element(up) ? (embi_element *)up : el;
    return embi_lay_lang(c, &tmp);
}

/* quotes: match-parent takes the parent's quotes, with auto resolved for the parent (CSS Content 3 section 3.1): walks up
 * from el while the value is match-parent. *lang_el is the element whose language an auto value follows (quote_lang
 * reads its parent's). Returns the resolved value (auto when the root is reached). */
static const embi_value *resolve_quotes(embi_lctx *c, embi_element *el, const embi_value *q, embi_element **lang_el)
{
    *lang_el = el;
    for (int guard = 0; guard < 256 && q != nullptr && q->kind == EMBI_V_KEYWORD && q->kw == EMBI_KW_match_parent;
         guard++) {
        const embi_node *up = *lang_el != nullptr ? (*lang_el)->base.parent : nullptr;
        if (up == nullptr || !embi_is_element(up)) {
            return nullptr; /* the root: auto */
        }
        *lang_el = (embi_element *)up;
        const embi_style *ps = embi_style_get(c->style, *lang_el, EMBI_PE_NONE);
        q = ps != nullptr ? embi_style_value(ps, EMBI_PROP_quotes) : nullptr;
    }
    return q;
}

/* Appends one item of a content value (CSS Content 3). Returns false for items that generate
 * nothing here (images, or anything unknown). */
static bool content_item(embi_lctx *c, embi_element *el, const embi_value *quotes,
                         const embi_value *v, embi_sb *sb)
{
    switch (v->kind) {
    case EMBI_V_STRING:
        embi_sb_append(sb, v->str);
        return true;
    case EMBI_V_FUNC:
        if (is_lit(embi_atom_str(v->func.name), "attr")) {
            gen_attr(el, v, sb);
            return true;
        }
        if (is_lit(embi_atom_str(v->func.name), "counter") ||
            is_lit(embi_atom_str(v->func.name), "counters")) {
            gen_counter(c, v, sb);
            return true;
        }
        return false;
    case EMBI_V_KEYWORD:
        switch (v->kw) {
        case EMBI_KW_open_quote:
        case EMBI_KW_close_quote: {
            c->ld->ctr_used = true;
            c->ld->ctr_read = true;
            embi_element *lel = nullptr;
            const embi_value *rq = resolve_quotes(c, el, quotes, &lel);
            embi_ctr_quote(&c->ctr, rq, v->kw == EMBI_KW_open_quote, quote_lang(c, lel), sb);
            return true;
        }
        case EMBI_KW_no_open_quote:
            c->ld->ctr_used = true;
            c->ld->ctr_read = true;
            c->ctr.quote_depth++;
            return true;
        case EMBI_KW_no_close_quote:
            c->ld->ctr_used = true;
            c->ld->ctr_read = true;
            if (c->ctr.quote_depth > 0) {
                c->ctr.quote_depth--;
            }
            return true;
        default:
            return false;
        }
    default:
        return false;
    }
}

/* The text of a content value. Returns false when it generates no text (none, normal, images). */


/* The marker text of a list item (CSS Lists 3 section 3): the counter in its style, followed by
 * a separator. Returns false when list-style-type is none. */

/* The first component value of a descriptor that is not white space, or nullptr. */
static const embi_css_cv *desc_first(const embi_css_decl *d, uint32_t *at)
{
    for (uint32_t j = 0; j < d->value.n; j++) {
        if (d->value.p[j].tok.type != EMBI_CSS_WS) {
            *at = j;
            return &d->value.p[j];
        }
    }
    return nullptr;
}

/* An author counter style (CSS Counter Styles 3): follows system: extends to a predefined style, keeping the
 * prefix and suffix of the nearest rule that gives them. Other systems are not supported yet and use decimal, which
 * is also the fallback for an undefined name or an extends cycle (section 3.1.1). */
typedef struct cstyle_res {
    uint16_t kw;
    bool has_prefix, has_suffix;
    embi_str prefix, suffix;
} cstyle_res;

static void custom_cstyle(embi_lctx *c, embi_atom name, cstyle_res *out)
{
    out->kw = EMBI_KW_decimal;
    for (uint32_t depth = 0; depth < 16; depth++) {
        uint16_t pre = embi_ctr_style_name(embi_atom_str(name));
        if (depth > 0 && pre != EMBI_KW_INVALID) {
            out->kw = pre;
            return;
        }
        const embi_css_rule *r = c->ld != nullptr ? embi_style_counter_style(c->style, c->ld->doc, name) : nullptr;
        if (r == nullptr) {
            if (pre != EMBI_KW_INVALID) {
                out->kw = pre;
            }
            return;
        }
        embi_atom next = 0;
        for (uint32_t i = 0; i < r->ndecls; i++) {
            const embi_css_decl *d = &r->decls[i];
            embi_str dn = embi_atom_str(d->name);
            uint32_t at = 0;
            const embi_css_cv *v = desc_first(d, &at);
            if (v == nullptr) {
                continue;
            }
            if (is_lit(dn, "system") && v->tok.type == EMBI_CSS_IDENT && is_lit(v->tok.value, "extends")) {
                for (uint32_t j = at + 1; j < d->value.n; j++) {
                    if (d->value.p[j].tok.type == EMBI_CSS_IDENT) {
                        next = embi_atom_intern(d->value.p[j].tok.value.ptr, d->value.p[j].tok.value.len);
                        break;
                    }
                }
            } else if (is_lit(dn, "prefix") && !out->has_prefix && v->tok.type == EMBI_CSS_STRING) {
                out->has_prefix = true;
                out->prefix = v->tok.value;
            } else if (is_lit(dn, "suffix") && !out->has_suffix && v->tok.type == EMBI_CSS_STRING) {
                out->has_suffix = true;
                out->suffix = v->tok.value;
            }
        }
        if (next == 0 || next == name) {
            return; /* not an extends system: decimal */
        }
        name = next;
    }
}

static bool marker_text(embi_lctx *c, const embi_style *ms, embi_sb *sb)
{
    const embi_value *t = embi_style_value(ms, EMBI_PROP_list_style_type);
    if (t == nullptr) {
        return false;
    }
    if (t->kind == EMBI_V_STRING) {
        embi_sb_append(sb, t->str);
        return true;
    }
    uint16_t kw = EMBI_KW_decimal;
    cstyle_res cr = {EMBI_KW_decimal, false, false, {nullptr, 0}, {nullptr, 0}};
    if (t->kind == EMBI_V_KEYWORD) {
        if (t->kw == EMBI_KW_none) {
            return false;
        }
        kw = t->kw;
    } else if (t->kind == EMBI_V_IDENT) {
        /* A named counter style: an @counter-style rule, or a predefined one, or decimal. */
        custom_cstyle(c, t->ident, &cr);
        kw = cr.kw;
    }
    const embi_value *dir = embi_style_value(ms, EMBI_PROP_direction);
    bool rtl = dir != nullptr && dir->kind == EMBI_V_KEYWORD && dir->kw == EMBI_KW_rtl;
    if (kw == EMBI_CTR_DISCLOSURE_CLOSED && rtl) {
        kw = EMBI_CTR_DISCLOSURE_CLOSED_RTL;
    }
    bool symbol = kw == EMBI_KW_disc || kw == EMBI_KW_circle || kw == EMBI_KW_square ||
                  kw == EMBI_CTR_DISCLOSURE_OPEN || kw == EMBI_CTR_DISCLOSURE_CLOSED ||
                  kw == EMBI_CTR_DISCLOSURE_CLOSED_RTL;
    if (cr.has_prefix) {
        embi_sb_append(sb, cr.prefix);
    }
    if (!symbol && c->ld != nullptr) {
        c->ld->ctr_read = true; /* CF1: the text is the list item's number (a symbol does not depend on it) */
    }
    embi_ctr_format(sb, embi_ctr_value(&c->ctr, list_item_atom()), kw);
    if (cr.has_suffix) {
        embi_sb_append(sb, cr.suffix);
    } else {
        embi_sb_append(sb, symbol ? (embi_str){" ", 1} : (embi_str){". ", 2});
    }
    return true;
}

/* ------------------------------------------------------------------ box creation */

/* The table role of a display value (CSS Tables 3). */
static uint8_t table_role(uint16_t disp)
{
    switch (disp) {
    case EMBI_KW_table:
    case EMBI_KW_inline_table:
        return EMBI_TR_TABLE;
    case EMBI_KW_table_caption:
        return EMBI_TR_CAPTION;
    case EMBI_KW_table_column_group:
        return EMBI_TR_COLGROUP;
    case EMBI_KW_table_column:
        return EMBI_TR_COL;
    case EMBI_KW_table_header_group:
        return EMBI_TR_THEAD;
    case EMBI_KW_table_footer_group:
        return EMBI_TR_TFOOT;
    case EMBI_KW_table_row_group:
        return EMBI_TR_ROWGROUP;
    case EMBI_KW_table_row:
        return EMBI_TR_ROW;
    case EMBI_KW_table_cell:
        return EMBI_TR_CELL;
    default:
        return EMBI_TR_NONE;
    }
}

static bool is_row_group(uint8_t tr)
{
    return tr == EMBI_TR_THEAD || tr == EMBI_TR_TFOOT || tr == EMBI_TR_ROWGROUP;
}

/* A box whose children table_fixup arranges: a table, a row group or a row. */
static bool is_table_container(uint8_t tr)
{
    return tr == EMBI_TR_TABLE || is_row_group(tr) || tr == EMBI_TR_ROW;
}

/* A box that only belongs in a table: captions, columns, groups, rows and cells. */
static bool is_table_internal(const embi_box *b)
{
    return b->tr != EMBI_TR_NONE && b->tr != EMBI_TR_TABLE;
}

/* The box for an element or a generated box from its display. Sets the kind and the formatting
 * flags. The caller handles the replaced kinds and the children. Returns nullptr for none. */
static bool box_in_counters(const embi_lctx *c, const embi_box *b);

static embi_box *make_box(embi_lctx *c, embi_element *el, uint8_t pe, const embi_style *s,
                          uint16_t disp, const embi_bcx *cx)
{
    uint8_t kind = EMBI_BK_BLOCK;
    bool inline_outer = false;
    bool bfc = false;
    switch (disp) {
    case EMBI_KW_none:
        return nullptr;
    case EMBI_KW_inline:
    case EMBI_KW_ruby:
    case EMBI_KW_ruby_base:
    case EMBI_KW_ruby_text:
        kind = EMBI_BK_INLINE;
        inline_outer = true;
        break;
    case EMBI_KW_inline_block:
    case EMBI_KW__internal_inline_flow_root_list_item:
        inline_outer = true;
        bfc = true;
        break;
    case EMBI_KW_flow_root:
    case EMBI_KW__internal_flow_root_list_item:
        bfc = true;
        break;
    case EMBI_KW__internal_inline_list_item:
        kind = EMBI_BK_INLINE;
        inline_outer = true;
        break;
    case EMBI_KW_flex:
        kind = EMBI_BK_FLEX;
        bfc = true; /* a flex container establishes a formatting context (CSS Flexbox 1 section 3) */
        break;
    case EMBI_KW_inline_flex:
        kind = EMBI_BK_FLEX;
        inline_outer = true;
        bfc = true;
        break;
    case EMBI_KW_grid:
        kind = EMBI_BK_GRID;
        bfc = true; /* a grid container establishes an independent formatting context (CSS Grid 2 section 2) */
        break;
    case EMBI_KW_inline_grid:
        kind = EMBI_BK_GRID;
        inline_outer = true;
        bfc = true;
        break;
    case EMBI_KW_table:
        kind = EMBI_BK_TABLE;
        bfc = true;
        break;
    case EMBI_KW_inline_table:
        kind = EMBI_BK_TABLE;
        inline_outer = true;
        bfc = true;
        break;
    case EMBI_KW_table_row_group:
    case EMBI_KW_table_header_group:
    case EMBI_KW_table_footer_group:
    case EMBI_KW_table_row:
    case EMBI_KW_table_column_group:
    case EMBI_KW_table_column:
    case EMBI_KW_table_caption:
    case EMBI_KW_table_cell:
        kind = EMBI_BK_TABLE;
        bfc = true;
        break;
    default:
        /* block, list-item, and anything else block-level */
        break;
    }
    if (cx->blockify) {
        /* Flex and grid items are blockified and establish formatting contexts. */
        inline_outer = false;
        if (kind == EMBI_BK_INLINE) {
            kind = EMBI_BK_BLOCK;
        }
        bfc = true;
    }
    embi_box *b = new_box(c, kind);
    set_el(c, b, el);
    b->pe = pe;
    b->style = s;
    if (box_in_counters(c, b)) {
        b->flags2 |= EMBI_BF2_CTR; /* read by the surgery: the style of this box may be gone by then */
    }
    b->tr = table_role(disp);
    if (inline_outer) {
        b->flags |= EMBI_BF_INLINE_LEVEL | EMBI_BF_ATOMIC;
    }
    if (kind == EMBI_BK_INLINE) {
        b->flags |= EMBI_BF_INLINE_LEVEL;
        b->flags &= (uint16_t)~EMBI_BF_ATOMIC;
    }
    if (cx->blockify && (disp == EMBI_KW_inline || disp == EMBI_KW_inline_block || disp == EMBI_KW__internal_inline_list_item ||
                         disp == EMBI_KW__internal_inline_flow_root_list_item ||
                         disp == EMBI_KW_inline_flex || disp == EMBI_KW_inline_grid ||
                         disp == EMBI_KW_inline_table || disp == EMBI_KW_ruby)) {
        b->flags |= EMBI_BF_BLOCKIFIED;
    }
    const embi_lay_ssum *sm = embi_lay_ssum_of(c, s);
    uint16_t pos = sm->pos;
    uint16_t flt = sm->flt;
    if (sm->xf) {
        c->ld->has_xf = true; /* hit testing cannot prune by scrollable overflow then (hit.c) */
    }
    if (pos == EMBI_KW_sticky) {
        c->ld->has_sticky = true; /* position.c walks the fragments for sticky boxes only then */
    }
    if (pos == EMBI_KW_absolute || pos == EMBI_KW_fixed) {
        b->flags |= EMBI_BF_ABSPOS | EMBI_BF_BFC;
    }
    if (flt == EMBI_KW_left || flt == EMBI_KW_right || flt == EMBI_KW_inline_start ||
        flt == EMBI_KW_inline_end) {
        b->flags |= EMBI_BF_FLOAT | EMBI_BF_BFC;
    }
    /* Any overflow other than visible clips. hidden, scroll and auto make a scroll container (CSS
     * Overflow 3 section 2.1), which establishes a BFC. clip clips without being one. */
    uint16_t ox = sm->ox;
    uint16_t oy = sm->oy;
    bool clip_x = ox != 0 && ox != EMBI_KW_visible;
    bool clip_y = oy != 0 && oy != EMBI_KW_visible;
    bool scroll_x = clip_x && ox != EMBI_KW_clip;
    bool scroll_y = clip_y && oy != EMBI_KW_clip;
    if (clip_x || clip_y) {
        b->flags |= EMBI_BF_CLIP;
    }
    if (scroll_x || scroll_y) {
        b->flags |= EMBI_BF_SCROLL | EMBI_BF_BFC;
    }
    /* Layout containment makes the box an independent formatting context root: contain: layout,
     * paint, strict and content, and container-type: size or inline-size (which applies layout
     * containment, CSS Containment 2 section 3 and 3 section 3). Paint containment also clips. */
    {
        bool lay = sm->contain_lay, paint = sm->contain_paint;
        /* Containment does not apply to non-atomic inline boxes. */
        if (kind != EMBI_BK_INLINE && paint) {
            b->flags |= EMBI_BF_CLIP | EMBI_BF_BFC;
        }
        if (kind != EMBI_BK_INLINE && lay) {
            b->flags |= EMBI_BF_BFC;
        }
    }
    if (el != nullptr && pe == 0 && el->base.extra == EMBI_NS_HTML &&
        is_lit(embi_atom_str(el->base.name), "button")) {
        b->flags |= EMBI_BF_CENTER;
    }
    if (sm->mc && kind == EMBI_BK_BLOCK && (b->flags & EMBI_BF_ANON) == 0) {
        b->flags |= EMBI_BF_BFC; /* a multi-column container is a BFC (CSS Multi-column 1 section 2) */
        b->tflags |= EMBI_TF_MC;
    }
    if (bfc || (sm->clamp && kind == EMBI_BK_BLOCK)) {
        b->flags |= EMBI_BF_BFC; /* a line-clamp container is an independent formatting context (CSS Overflow 4 section 5.1) */
        if (sm->clamp && kind == EMBI_BK_BLOCK) {
            b->tflags |= EMBI_TF_CLAMP;
        }
    }
    return b;
}

/* Inline-level: text, an inline box, a break, a marker, an atomic box, or an out-of-flow box (which
 * does not split the inline content). */
static bool is_inline_level(const embi_box *b)
{
    if (b->flags & (EMBI_BF_FLOAT | EMBI_BF_ABSPOS)) {
        return true;
    }
    if (b->flags & EMBI_BF_INLINE_LEVEL) {
        return true;
    }
    return b->kind == EMBI_BK_INLINE || b->kind == EMBI_BK_TEXT || b->kind == EMBI_BK_BR ||
           b->kind == EMBI_BK_MARKER;
}

static bool is_block_level(const embi_box *b)
{
    return !is_inline_level(b);
}

/* Adds a box to the list of a block container, updating its white space state: a block-level box
 * ends the line, and so does any box with content. Inline boxes that only group children (their
 * pieces are added as they are built) do not touch the state. */
static bool out_of_flow(const embi_box *b);
static void push_content(embi_blist *l, embi_box *b, embi_bcx *cx)
{
    if (cx->in_inline && is_table_internal(b) && !out_of_flow(b)) {
        /* A table part under an inline box ends up in an anonymous inline-table: an atomic inline. */
        cx->ws->trim = nullptr;
        cx->ws->in_word = false;
        cx->ws->prev_space = false;
    } else if (is_block_level(b)) {
        cx->ws->prev_space = true;
        cx->ws->trim = nullptr;
        cx->ws->in_word = false;
        cx->ws->zw_last = false;
    } else if (b->kind != EMBI_BK_INLINE && (b->flags & (EMBI_BF_FLOAT | EMBI_BF_ABSPOS)) == 0) {
        /* Out-of-flow boxes (floats, abspos) neither end a word nor cancel the pending trim. */
        cx->ws->trim = nullptr;
        cx->ws->in_word = false; /* an atomic inline or a break ends the word */
        cx->ws->zw_last = false;
        if (b->kind == EMBI_BK_BR) {
            cx->ws->prev_space = true; /* spaces at the start of the next line are removed */
        } else if (b->kind != EMBI_BK_TEXT && b->kind != EMBI_BK_MARKER) {
            /* An atomic inline is not a space: a collapsible space after it stays (CSS Text 3 section 4.1.1). */
            cx->ws->prev_space = false;
        }
    }
    bl_push(l, b);
}

/* Out-of-flow boxes (floats and absolutely positioned boxes) do not take part in the anonymous wrapping. */
static bool out_of_flow(const embi_box *b)
{
    return (b->flags & (EMBI_BF_FLOAT | EMBI_BF_ABSPOS)) != 0;
}

/* The URL of a url() value (a content item or a list-style-image), or false for other values. */
static bool image_url(const embi_value *v, embi_str *url)
{
    if (v == nullptr) {
        return false;
    }
    if (v->kind == EMBI_V_URL) {
        *url = v->str;
        return true;
    }
    if (v->kind == EMBI_V_IMAGE && v->image != nullptr && v->image->kind == EMBI_IMG_URL) {
        *url = v->image->url;
        return true;
    }
    return false;
}

/* The natural size of a content image (a url(), a gradient or an image-set() of them; CSS Content 3 section 2). A
 * gradient has no natural size or ratio: it gets the 300x150 default object size of replaced elements (CSS 2.1 section
 * 10.3.2). False when the item is not an image or the image cannot be sized. */
static bool content_image_size(embi_lctx *c, const embi_value *v, float *w, float *h, bool *ratio)
{
    embi_image one;
    memset(&one, 0, sizeof one);
    const embi_image *im = nullptr;
    float dppx = 1.0f;
    if (v != nullptr && v->kind == EMBI_V_URL) {
        one.kind = EMBI_IMG_URL;
        one.url = v->str;
        im = &one;
    } else if (v != nullptr && v->kind == EMBI_V_IMAGE && v->image != nullptr) {
        im = embi_lay_image_resolve(v->image, &dppx);
    }
    if (im == nullptr) {
        return false;
    }
    if (im->kind == EMBI_IMG_GRADIENT) {
        *w = 300.0f;
        *h = 150.0f;
        *ratio = false;
        return true;
    }
    if (im->kind != EMBI_IMG_URL || c->image_size == nullptr) {
        return false;
    }
    c->ld->has_images = true;
    if (!c->image_size(c->image_user, im->url, w, h, ratio)) {
        return false;
    }
    /* An image-set() option's resolution scales its natural size (CSS Images 4 section 2.5). */
    if (dppx > 0.0f && dppx != 1.0f) {
        *w /= dppx;
        *h /= dppx;
    }
    return true;
}

/* A replaced box for an <image> item of generated content (CSS Content 3). Nothing is generated when
 * the image cannot be sized. */
static void image_box(embi_lctx *c, embi_blist *l, embi_element *el, const embi_style *s,
                      const embi_value *v, uint8_t pe, embi_bcx *cx)
{
    float w = 0, h = 0;
    bool ratio = false;
    if (!content_image_size(c, v, &w, &h, &ratio)) {
        return;
    }
    embi_box *b = new_box(c, EMBI_BK_REPLACED);
    b->flags |= EMBI_BF_INLINE_LEVEL | EMBI_BF_ATOMIC;
    b->pe = pe;
    set_el(c, b, el);
    b->style = s;
    set_replaced(b, w, h, ratio);
    push_content(l, b, cx);
}

/* The replaced marker of a list item's list-style-image, or nullptr when there is none (or the
 * image cannot be sized, in which case the marker falls back to list-style-type). */
static embi_box *marker_image(embi_lctx *c, embi_element *el, const embi_style *ts)
{
    embi_str url = {nullptr, 0};
    float w = 0, h = 0;
    bool ratio = false;
    if (!image_url(embi_style_value(ts, EMBI_PROP_list_style_image), &url) || c->image_size == nullptr) {
        return nullptr;
    }
    c->ld->has_images = true;
    if (!c->image_size(c->image_user, url, &w, &h, &ratio)) {
        return nullptr;
    }
    embi_box *b = new_box(c, EMBI_BK_REPLACED);
    b->flags |= EMBI_BF_INLINE_LEVEL | EMBI_BF_ATOMIC;
    b->pe = EMBI_PE_MARKER;
    set_el(c, b, el);
    b->style = ts;
    set_replaced(b, w, h, ratio);
    return b;
}

/* ------------------------------------------------------------------ building */

static void build_node(embi_lctx *c, embi_blist *l, embi_node *n, embi_bcx *cx);
static void build_children(embi_lctx *c, embi_blist *l, embi_element *el, const embi_style *s,
                           embi_bcx *cx);

/* The longest text a box holds. EMBI_TEXT_MAX, unless a test lowered it (embi_lay_set_text_max) so that a few
 * bytes reach the split. */
static uint32_t g_text_max = EMBI_TEXT_MAX;

void embi_lay_set_text_max(uint32_t max)
{
    g_text_max = max == 0 || max > EMBI_TEXT_MAX ? EMBI_TEXT_MAX : max;
}

/* Adds the text box (several for a text longer than EMBI_TEXT_MAX bytes: the byte ranges of its fragments have
 * 24 bits) for the processed UTF-8 text [text, text + len), len > 0. stable: the bytes live as long as the box
 * tree (the boxes point at them); else they are copied into the box arena. */
static void text_boxes(embi_lctx *c, embi_blist *l, const embi_style *s, const char *text, uint32_t len,
                       bool stable, uint8_t pe, embi_element *el, embi_bcx *cx)
{
    embi_box *b = nullptr;
    for (uint32_t off = 0; off < len;) {
        uint32_t n = len - off;
        const uint32_t max = g_text_max;
        if (n > max) {
            n = max;
            while (n > 0 && ((uint8_t)text[off + n] & 0xC0u) == 0x80u) {
                n--; /* do not cut a UTF-8 sequence */
            }
            if (n == 0) {
                n = max; /* a character longer than the limit, or malformed text of continuation bytes only: cut anyway, never stall */
            }
        }
        b = new_box(c, EMBI_BK_TEXT);
        b->flags |= EMBI_BF_INLINE_LEVEL;
        b->pe = pe;
        set_el(c, b, el);
        b->style = s;
        if (stable) {
            b->text = text + off;
        } else {
            char *copy = embi_arena_alloc(&c->ld->ba, n, 1);
            memcpy(copy, text + off, n);
            b->text = copy;
        }
        b->text_len = n;
        bl_push(l, b);
        off += n;
    }
    /* The run ends in a collapsible space when the state says the last output char is one. */
    bool ends_space = b->text[b->text_len - 1] == ' ' && cx->ws->prev_space;
    cx->ws->trim = ends_space ? b : nullptr;
}

/* text-transform: math-auto (MathML Core section 3.2.1 and the italic mappings of section 7.2): a text of a single character
 * is replaced by its mathematical italic form. */
static uint32_t math_italic(uint32_t c)
{
    if (c >= 0x41u && c <= 0x5Au) {
        return 0x1D434u + (c - 0x41u);
    }
    if (c >= 0x61u && c <= 0x7Au) {
        return c == 0x68u ? 0x210Eu : 0x1D44Eu + (c - 0x61u);
    }
    if (c >= 0x391u && c <= 0x3A9u && c != 0x3A2u) {
        return 0x1D6E2u + (c - 0x391u);
    }
    if (c >= 0x3B1u && c <= 0x3C9u) {
        return 0x1D6FCu + (c - 0x3B1u);
    }
    switch (c) {
    case 0x131u: return 0x1D6A4u;
    case 0x237u: return 0x1D6A5u;
    case 0x3F4u: return 0x1D6F3u;
    case 0x2207u: return 0x1D6FBu;
    case 0x2202u: return 0x1D715u;
    case 0x3F5u: return 0x1D716u;
    case 0x3D1u: return 0x1D717u;
    case 0x3F0u: return 0x1D718u;
    case 0x3D5u: return 0x1D719u;
    case 0x3F1u: return 0x1D71Au;
    case 0x3D6u: return 0x1D71Bu;
    default: return 0;
    }
}

static void math_auto(embi_sb *out)
{
    const char *p = out->data, *end = out->data + out->len;
    uint32_t c = embi_utf8_decode(&p, end);
    if (p != end) {
        return; /* more than one character */
    }
    uint32_t m = math_italic(c);
    if (m != 0) {
        out->len = 0;
        embi_sb_append_cp(out, m);
    }
}

/* EMBI_TT_CJY when the text may have a segment break next to a non-ASCII character and its writing system (from the
 * language of lang_el) is Chinese, Japanese or Yi; else 0. */
static uint16_t cjy_flag(embi_lctx *c, embi_str in, uint8_t wsm, embi_element *lang_el)
{
    if (lang_el == nullptr || in.len == 0 || (wsm != EMBI_WS_COLLAPSE && wsm != EMBI_WS_PRESERVE_SPACES) ||
        memchr(in.ptr, 10, in.len) == nullptr) {
        return 0;
    }
    bool wide = false;
    for (uint32_t i = 0; i < in.len && !wide; i++) {
        wide = (uint8_t)in.ptr[i] >= 0x80u;
    }
    if (!wide) {
        return 0;
    }
    embi_box lb;
    memset(&lb, 0, sizeof lb);
    lb.el = lang_el;
    return embi_lay_lang_cjy(embi_lay_lang(c, &lb)) ? (uint16_t)EMBI_TT_CJY : 0;
}

/* Adds a text box for the UTF-8 text `in` with the white space of style s. `stable`, when not null, is a
 * copy of `in` that lives as long as the box tree (the ASCII bytes of a DOM text node): a run that the white
 * space processing leaves unchanged points at it instead of keeping a copy of its own. A text longer than
 * EMBI_TEXT_MAX bytes becomes several boxes (the byte ranges of its fragments have 24 bits). */
static void text_box(embi_lctx *c, embi_blist *l, const embi_style *s, embi_str in, uint8_t pe,
                     embi_element *el, embi_bcx *cx, const char *stable, embi_element *lang_el)
{
    embi_sb out = {0};
    uint16_t tt = embi_lay_transform(s);
    bool word_before = cx->ws->in_word;
    uint8_t wsm = embi_lay_ws_mode(s);
    if (cx->ws->zw_last && wsm == EMBI_WS_COLLAPSE) {
        /* A segment break right after U+200B (the end of the previous text, across inline box boundaries) is removed
         * (CSS Text 3 section 4.1.2), with the spaces and tabs around it. */
        size_t k = 0;
        bool brk = false;
        while (k < in.len && (in.ptr[k] == ' ' || in.ptr[k] == 9 || in.ptr[k] == 13 || in.ptr[k] == 10)) {
            brk = brk || in.ptr[k] == 10;
            k++;
        }
        if (brk) {
            in.ptr += k;
            in.len -= k;
            stable = nullptr; /* the run no longer starts where the DOM text does */
        }
    }
    embi_lay_ws_process(&out, in, wsm, (uint16_t)(tt | cjy_flag(c, in, wsm, lang_el)), &cx->ws->prev_space, &cx->ws->in_word);
    if ((tt & EMBI_TT_BASE) == EMBI_KW_math_auto && out.len > 0) {
        math_auto(&out);
    }
    if (out.len > 0) {
        cx->ws->zw_last = out.len >= 3 && (uint8_t)out.data[out.len - 3] == 0xE2u && (uint8_t)out.data[out.len - 2] == 0x80u &&
                          (uint8_t)out.data[out.len - 1] == 0x8Bu;
    }
    if (((tt & EMBI_TT_BASE) == EMBI_KW_uppercase || (tt & EMBI_TT_BASE) == EMBI_KW_capitalize) && out.len > 0 && lang_el != nullptr) {
        embi_box lb;
        memset(&lb, 0, sizeof lb);
        lb.el = lang_el;
        embi_lay_case_tailor(&out, embi_lay_lang(c, &lb), (uint16_t)(tt & EMBI_TT_BASE), word_before);
    }
    if (out.len == 0) {
        embi_sb_free(&out);
        return;
    }
    const char *src = out.len == in.len && memcmp(out.data, in.ptr, out.len) == 0 ? stable : nullptr;
    text_boxes(c, l, s, src != nullptr ? src : out.data, out.len, src != nullptr, pe, el, cx);
    embi_sb_free(&out);
}

/* The ::marker box of a list item: it goes first among the item's children. */
static void marker_box(embi_lctx *c, embi_blist *l, embi_element *el, const embi_style *s,
                       embi_bcx *cx, bool force_inside)
{
    /* A list item that is itself a generated box (::before { display: list-item }) has no ::marker style of its own here. */
    const embi_style *ms = c->no_pseudo || c->ctr.cur_pe != 0 ? nullptr : embi_style_generated(c->style, el, EMBI_PE_MARKER);
    const embi_style *ts = ms != nullptr ? ms : s;
    /* An inline-level list item has no line start to hang an outside marker from: its marker is inline (as
     * browsers do; inline-list, inline-block-list). */
    bool outside = !force_inside && keyword_of(ts, EMBI_PROP_list_style_position) == EMBI_KW_outside;
    embi_box *img = marker_image(c, el, ts);
    if (img != nullptr) {
        if (outside) {
            img->flags |= EMBI_BF_OUTSIDE;
        }
        push_content(l, img, cx);
        return;
    }
    embi_sb sb = {0};
    if (!marker_text(c, ts, &sb) || sb.len == 0) {
        embi_sb_free(&sb);
        return;
    }
    if (force_inside && sb.data[sb.len - 1] == ' ') {
        /* The marker of an inline list item stays on the line of the item's first content: its trailing space is no
         * break opportunity (inline-list: the marker is one unit with what follows, as an inline-block would be). */
        sb.len--;
        embi_sb_append(&sb, EMBI_STR("\xC2\xA0"));
    }
    embi_box *b = new_box(c, EMBI_BK_MARKER);
    b->flags |= EMBI_BF_INLINE_LEVEL;
    b->pe = EMBI_PE_MARKER;
    set_el(c, b, el);
    b->style = ts;
    if (outside) {
        b->flags |= EMBI_BF_OUTSIDE;
    }
    char *copy = embi_arena_alloc(&c->ld->ba, sb.len, 1);
    memcpy(copy, sb.data, sb.len);
    b->text = copy;
    b->text_len = sb.len;
    embi_sb_free(&sb);
    push_content(l, b, cx);
}


/* ------------------------------------------------------------------ reversed counter initial values */

/* The default initial value of a reversed counter (CSS Lists 3 section 4.4.2, "Instantiating Counters"):
 * walk the elements and pseudo-elements that increment or set the counter in its scope, in tree order;
 * num accumulates the negated increments, and the first counter-set adds its value and ends the walk.
 * The result is num plus the last non-zero negated increment (lastNonZeroIncrementNegated), which
 * includes the increment of the element that sets the counter. */
typedef struct rev_walk {
    embi_lctx *c;
    embi_atom name;
    bool is_li;
    bool done;
    bool parent_has; /* the parent's inherited counters already hold this name */
    int64_t num;
    int64_t last_nz_neg;
    uint32_t budget;
} rev_walk;

/* One style's effect: returns false when the style instantiates the counter anew (a nested scope). */
static bool rev_apply(rev_walk *w, const embi_style *s, bool is_item, embi_element *el, bool is_root)
{
    int32_t num = 0;
    bool has_num = false;
    if (!is_root && embi_ctr_style_op(s, EMBI_PROP_counter_reset, w->name, &num, &has_num)) {
        return false;
    }
    int64_t inc = 0;
    bool incs = false;
    if (embi_ctr_style_op(s, EMBI_PROP_counter_increment, w->name, &num, &has_num)) {
        inc = has_num ? num : 1;
        incs = true;
    } else if (w->is_li && is_item) {
        inc = -1; /* the implicit list-item increment of a reversed list */
        incs = true;
    }
    bool sets = false;
    int64_t sv = 0;
    int32_t v = 0;
    if (embi_ctr_style_op(s, EMBI_PROP_counter_set, w->name, &num, &has_num)) {
        sets = true;
        sv = has_num ? num : 0;
    }
    if (w->is_li && is_item && el != nullptr && attr_int(el, "value", &v)) {
        sets = true; /* <li value> is counter-set: list-item <value> */
        sv = v;
    }
    if (!incs && !sets) {
        return true;
    }
    int64_t neg = -inc;
    if (neg != 0) {
        w->last_nz_neg = neg;
    }
    if (sets) {
        w->num += sv;
        w->done = true;
        return true;
    }
    w->num += neg;
    return true;
}

static void rev_pseudo(rev_walk *w, embi_element *el, embi_sel_pseudo pe)
{
    if (w->done) {
        return;
    }
    const embi_style *ps = w->c->no_pseudo ? nullptr : embi_style_generated(w->c->style, el, pe);
    if (ps == nullptr || display_kw(ps) == EMBI_KW_none) {
        return;
    }
    const embi_value *cv = embi_style_value(ps, EMBI_PROP_content);
    if (cv == nullptr || (cv->kind == EMBI_V_KEYWORD && (cv->kw == EMBI_KW_none || cv->kw == EMBI_KW_normal))) {
        return;
    }
    rev_apply(w, ps, false, nullptr, false);
}

/* Walks one element; false when the element instantiates the counter anew, which ends the scope at its level
 * (the siblings after it see the new instance). */
static bool rev_element(rev_walk *w, embi_element *el, bool is_root)
{
    if (w->done || w->budget == 0) {
        return true;
    }
    w->budget--;
    const embi_style *s = embi_style_get(w->c->style, el, EMBI_PE_NONE);
    if (s == nullptr) {
        return true;
    }
    uint16_t disp = display_kw(s);
    if (disp == EMBI_KW_none) {
        return true;
    }
    bool is_item = embi_lay_disp_is_li(disp);
    if (!rev_apply(w, s, is_item, el->base.extra == EMBI_NS_HTML ? el : nullptr, is_root)) {
        return false;
    }
    rev_pseudo(w, el, EMBI_PE_BEFORE);
    for (embi_node *k = el->base.first_child; k != nullptr; k = k->next) {
        if (embi_is_element(k)) {
            rev_element(w, embi_as_element(k), false);
        }
    }
    rev_pseudo(w, el, EMBI_PE_AFTER);
    return true;
}

/* The following siblings of el: the scope of a counter reaches them (CSS Lists 3 section 4.4.1). */
static void rev_siblings(rev_walk *w, embi_element *el)
{
    if (w->is_li && w->parent_has) {
        /* CSS Lists 3 section 4.4.1: a sibling's counter is not inherited when the parent's set has one of
         * that name, so the items after a nested list belong to the outer list (li-value-reversed-007); a
         * list without an enclosing list-item counter reaches its following siblings (li-value-reversed-006d). */
        return;
    }
    for (embi_node *k = el->base.next; k != nullptr; k = k->next) {
        if (embi_is_element(k) && !rev_element(w, embi_as_element(k), false)) {
            break;
        }
    }
}

int32_t embi_ctr_reversed_initial(embi_ctrs *cs, embi_atom name)
{
    embi_lctx *c = (embi_lctx *)cs->lctx;
    embi_element *el = (embi_element *)cs->cur_el;
    if (c == nullptr || el == nullptr) {
        return 0;
    }
    rev_walk w = {c, name, name == list_item_atom(), false, false, 0, 0, 20000};
    for (uint32_t i = 0; i < cs->sib_base && i < cs->st.len; i++) {
        if (cs->st.data[i].name == name) {
            w.parent_has = true;
            break;
        }
    }
    if (cs->cur_pe == 0) {
        if (rev_element(&w, el, true)) {
            rev_siblings(&w, el);
        }
    } else {
        /* A reset on a pseudo-element: its own changes, then (for ::before) the element's content. */
        const embi_style *ps = embi_style_generated(c->style, el, (embi_sel_pseudo)cs->cur_pe);
        bool open = ps == nullptr || rev_apply(&w, ps, false, nullptr, true);
        if (open && cs->cur_pe == EMBI_PE_BEFORE) {
            for (embi_node *k = el->base.first_child; open && k != nullptr; k = k->next) {
                if (embi_is_element(k)) {
                    rev_element(&w, embi_as_element(k), false);
                }
            }
            if (open) {
                rev_pseudo(&w, el, EMBI_PE_AFTER);
            }
        }
        if (open) {
            rev_siblings(&w, el);
        }
    }
    int64_t init = w.num + w.last_nz_neg;
    return init > INT32_MAX ? INT32_MAX : (init < INT32_MIN ? INT32_MIN : (int32_t)init);
}

/* A generated box (::before, ::after) of element el. */
static uint32_t ws_trim_bits(const embi_style *s);

static void pseudo_box_inner(embi_lctx *c, embi_blist *l, embi_element *el, embi_sel_pseudo pe,
                       embi_bcx *cx)
{
    const embi_style *ps = c->no_pseudo ? nullptr : embi_style_generated(c->style, el, pe);
    if (ps == nullptr) {
        return;
    }
    uint16_t disp = display_kw(ps);
    if (disp == EMBI_KW_none) {
        return;
    }
    const embi_value *cv = embi_style_value(ps, EMBI_PROP_content);
    if (cv == nullptr ||
        (cv->kind == EMBI_V_KEYWORD && (cv->kw == EMBI_KW_none || cv->kw == EMBI_KW_normal))) {
        return;
    }
    if (!embi_lay_ssum_of(c, ps)->ctr_none) {
        c->ld->ctr_used = true;
    }
    c->ctr.lctx = c;
    c->ctr.cur_el = el;
    c->ctr.cur_pe = (uint8_t)pe;
    const bool pseudo_li = disp != EMBI_KW_contents && embi_lay_disp_is_li(disp);
    embi_ctr_apply(&c->ctr, ps, pseudo_li, false, 0);
    c->ctr.cur_pe = 0;
    const embi_value *quotes = embi_style_value(ps, EMBI_PROP_quotes);

    /* display: contents (CSS Display 3 section 2.5): no box, the generated content goes into the parent's list
     * as if it were the element's own children (display-contents-before-after-002/003). */
    bool contents = disp == EMBI_KW_contents;
    embi_box *b = contents ? nullptr : make_box(c, el, (uint8_t)pe, ps, disp, cx);
    if (b == nullptr && !contents) {
        return;
    }
    embi_ws block_ws = {true, nullptr, false, false};
    embi_bcx kid = {ps, cx->ws, cx->blockify, cx->in_inline};
    if (!contents) {
        kid.blockify = false;
        kid.in_inline = false;
    }
    if (b != nullptr && b->kind == EMBI_BK_BLOCK && !(b->flags & EMBI_BF_INLINE_LEVEL)) {
        kid.ws = &block_ws;
    }
    /* white-space-trim on the pseudo-element (CSS Text 4 section 4.2): discard-before removes the collapsible space before
     * it, discard-after the ones after it. */
    uint32_t wtrim = ws_trim_bits(ps);
    if ((wtrim & 1u) != 0) {
        if (cx->ws->trim != nullptr && cx->ws->trim->text_len > 0) {
            cx->ws->trim->text_len--;
        }
        cx->ws->trim = nullptr;
        cx->ws->prev_space = true;
    }
    embi_blist kids = {nullptr, nullptr};
    if (pseudo_li && b != nullptr) {
        /* display: list-item on a generated box: it has a marker like an element (after-content-display-003); the counter
         * of the box is still the current one. */
        c->ctr.cur_pe = (uint8_t)pe;
        marker_box(c, &kids, el, ps, &kid, (b->flags & EMBI_BF_INLINE_LEVEL) != 0);
        c->ctr.cur_pe = 0;
    }
    embi_sb text = {0};
    uint32_t n = 0;
    const embi_value *items = embi_value_items(cv, &n);
    for (uint32_t i = 0; i < n; i++) {
        const embi_value *v = &items[i];
        if (v->kind == EMBI_V_URL || v->kind == EMBI_V_IMAGE) {
            if (text.len > 0) {
                text_box(c, &kids, ps, (embi_str){text.data, text.len}, (uint8_t)pe, el, &kid, nullptr, el);
                embi_sb_clear(&text);
            }
            image_box(c, &kids, el, ps, v, (uint8_t)pe, &kid);
            continue;
        }
        (void)content_item(c, el, quotes, v, &text);
    }
    if (text.len > 0) {
        text_box(c, &kids, ps, (embi_str){text.data, text.len}, (uint8_t)pe, el, &kid, nullptr, el);
    }
    embi_sb_free(&text);
    if (contents) {
        for (embi_box *k = kids.head; k != nullptr;) {
            embi_box *nx = k->next;
            push_content(l, k, cx);
            k = nx;
        }
        return;
    }
    bl_attach(b, &kids);
    push_content(l, b, cx);
    if ((wtrim & 2u) != 0) {
        cx->ws->prev_space = true;
    }
}

/* A generated box: its counters end with it for the following siblings as an element's do (CSS Lists 3 section 4.4.1;
 * counters-scope-001). */
static void pseudo_box(embi_lctx *c, embi_blist *l, embi_element *el, embi_sel_pseudo pe, embi_bcx *cx)
{
    uint32_t start = c->ctr.st.len;
    pseudo_box_inner(c, l, el, pe, cx);
    if (c->ctr.st.len > start) { /* most boxes create no counter: skip the atom lookup */
        embi_ctr_end_element(&c->ctr, start, c->ctr.sib_base, list_item_atom());
    }
}

static void build_text_node(embi_lctx *c, embi_blist *l, embi_chardata *cd, embi_bcx *cx)
{
    if (cx->inh == nullptr) {
        return;
    }
    /* The common text needs no processing: narrow ASCII bytes that white space collapsing leaves as they are. */
    const embi_lay_ssum *tm = !cd->data.wide && cd->data.len > 0 ? embi_lay_ssum_of(c, cx->inh) : nullptr;
    if (tm != nullptr &&
        embi_lay_ws_ascii_unchanged((const uint8_t *)cd->data.ptr, cd->data.len, tm->ws, tm->tt,
                                    &cx->ws->prev_space, &cx->ws->in_word)) {
        text_boxes(c, l, cx->inh, (const char *)cd->data.ptr, cd->data.len, true, 0, nullptr, cx);
        cx->ws->zw_last = false;
        return;
    }
    embi_sb sb = {0};
    embi_dstr_append_utf8(&sb, cd->data);
    /* A narrow DOM string of ASCII bytes is its own UTF-8 form (sb.len == the code units): the box can point at it.
     * The DOM text lives until the next mutation, which changes the style epoch and so drops this tree. */
    const char *stable = !cd->data.wide && sb.len == cd->data.len ? (const char *)cd->data.ptr : nullptr;
    text_box(c, l, cx->inh, (embi_str){sb.data, sb.len}, 0, nullptr, cx, stable,
             cd->base.parent != nullptr && embi_is_element(cd->base.parent) ? (embi_element *)cd->base.parent : nullptr);
    embi_sb_free(&sb);
}

/* Splits an inline box's children (already built into sub) around its block-level children
 * (CSS 2.1 section 9.2.1.1). Each piece is a copy of the inline box. The pieces go into l, in order,
 * and the first one is returned (nullptr when the inline has no pieces). */

static void group_table_children(embi_lctx *c, embi_box *parent, bool inline_parent);

/* Whether the inline box has a start or end edge (inline-direction margin, border or padding) that is not zero. */
static bool inline_edge(embi_lctx *c, embi_box *inl, bool start)
{
    embi_edges bdr, pad, mar;
    embi_lay_edges(c, inl, 0.0f, &bdr, &pad, &mar);
    const embi_value *dir = embi_style_value(inl->style, EMBI_PROP_direction);
    bool rtl = dir != nullptr && dir->kind == EMBI_V_KEYWORD && dir->kw == EMBI_KW_rtl;
    bool left = start != rtl;
    return left ? (bdr.l != 0.0f || pad.l != 0.0f || mar.l != 0.0f) : (bdr.r != 0.0f || pad.r != 0.0f || mar.r != 0.0f);
}

/* True for an inline box with a position other than static (a containing block for its abspos descendants). */
static bool positioned_inline(const embi_box *inl)
{
    const embi_value *v = inl->style != nullptr ? embi_style_value(inl->style, EMBI_PROP_position) : nullptr;
    return v != nullptr && v->kind == EMBI_V_KEYWORD && v->kw != EMBI_KW_static;
}

static embi_box *new_piece(embi_lctx *c, embi_box *inl)
{
    embi_box *p = new_box(c, EMBI_BK_INLINE);
    p->flags = inl->flags;
    set_el(c, p, inl->el);
    p->pe = inl->pe;
    p->style = inl->style;
    return p;
}

static embi_box *split_inline(embi_lctx *c, embi_blist *l, embi_box *inl, embi_blist *sub,
                              embi_bcx *cx)
{
    embi_box *first = nullptr;
    embi_box *cur = nullptr;
    embi_box *prev = nullptr; /* the last piece pushed: it gets NOEND when another piece follows */
    embi_box *cur_last = nullptr; /* the last child of cur, so that appends stay O(1) */
    embi_box *k = sub->head;
    /* A box that starts with a block-level child still has its start edges, on an empty piece before it; so does a
     * positioned one, whose first fragment starts its containing block for abspos (position-absolute-in-inline-003). */
    if (k != nullptr && is_block_level(k) && (inline_edge(c, inl, true) || positioned_inline(inl))) {
        embi_box *e = new_piece(c, inl);
        e->flags2 |= EMBI_BF2_NOEND;
        bl_push(l, e);
        first = e;
        prev = e;
    }
    bool last_block = false;
    while (k != nullptr) {
        embi_box *nx = k->next;
        k->next = nullptr;
        if (is_block_level(k)) {
            if (cur != nullptr) {
                bl_push(l, cur);
                if (first == nullptr) {
                    first = cur;
                }
                prev = cur;
                cur = nullptr;
            }
            last_block = true;
            bl_push(l, k);
            if (first == nullptr) {
                first = k;
            }
            cx->ws->prev_space = true;
            cx->ws->trim = nullptr;
            cx->ws->in_word = false;
            cx->ws->zw_last = false;
        } else {
            last_block = false;
            if (cur == nullptr) {
                cur = new_piece(c, inl);
                if (first != nullptr) {
                    cur->flags2 |= EMBI_BF2_NOSTART;
                }
                if (prev != nullptr) {
                    prev->flags2 |= EMBI_BF2_NOEND;
                }
                cur_last = nullptr;
            }
            if (cur_last == nullptr) {
                cur->first = k;
            } else {
                cur_last->next = k;
            }
            cur_last = k;
            k->parent = cur;
        }
        k = nx;
    }
    if (cur != nullptr) {
        bl_push(l, cur);
        if (first == nullptr) {
            first = cur;
        }
    } else if (last_block && inline_edge(c, inl, false)) {
        /* The end edges are on an empty piece after the last block. */
        embi_box *e = new_piece(c, inl);
        e->flags2 |= EMBI_BF2_NOSTART;
        if (prev != nullptr) {
            prev->flags2 |= EMBI_BF2_NOEND;
        }
        bl_push(l, e);
        if (first == nullptr) {
            first = e;
        }
    }
    return first;
}

/* ------------------------------------------------------------------ ::first-letter */

/* The byte length of the first-letter text at the start of text (CSS Pseudo 4: the punctuation before the first
 * letter, the letter with its combining marks, and the punctuation after it), 0 when there is none. */
static uint32_t fl_length(const char *text, uint32_t len)
{
    const char *p = text;
    const char *end = text + len;
    while (p < end) {
        const char *q = p;
        uint32_t cp = embi_utf8_decode(&q, end);
        if (!embi_lay_is_fl_punct(cp)) {
            break;
        }
        p = q;
    }
    if (p >= end) {
        return 0;
    }
    const char *q = p;
    uint32_t cp = embi_utf8_decode(&q, end);
    if (cp == ' ' || cp == 0x09 || cp == 0x0A || cp == 0x0D || cp == 0x0C || cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) ||
        cp == 0x202F || cp == 0x205F || cp == 0x3000) {
        return 0;
    }
    p = q;
    while (p < end) {
        q = p;
        cp = embi_utf8_decode(&q, end);
        if (!embi_lay_is_mark(cp)) {
            break;
        }
        p = q;
    }
    while (p < end) {
        q = p;
        cp = embi_utf8_decode(&q, end);
        if (!embi_lay_is_fl_punct(cp)) {
            break;
        }
        p = q;
    }
    return (uint32_t)(p - text);
}

/* Looks for the first formatted text in the sibling list starting at *head and wraps its first letter in a
 * ::first-letter box of style ps. Returns true when the search is over: the letter was wrapped, or something that
 * ends the first line's start (an atomic box, a break, a first letter that is already there) came first. tail is the
 * list's tail pointer when the list is a blist (nullptr for a box's children). */
static bool fl_search(embi_lctx *c, embi_element *el, const embi_style *ps, embi_box **head, embi_box *parent,
                      embi_blist *top)
{
    for (embi_box **slot = head; *slot != nullptr; slot = &(*slot)->next) {
        embi_box *k = *slot;
        if ((k->flags & (EMBI_BF_FLOAT | EMBI_BF_ABSPOS)) != 0 || k->kind == EMBI_BK_MARKER) {
            continue;
        }
        if (k->kind == EMBI_BK_TEXT) {
            if (k->text_len == 0) {
                continue;
            }
            uint32_t n = fl_length(k->text, k->text_len);
            if (n == 0) {
                return true;
            }
            embi_bcx cx = {ps, nullptr, false, false};
            uint16_t disp = display_kw(ps);
            if (disp == EMBI_KW_none) {
                return true;
            }
            embi_box *w = make_box(c, el, EMBI_PE_FIRST_LETTER, ps, disp, &cx);
            if (w == nullptr || (w->kind != EMBI_BK_INLINE && (w->flags & EMBI_BF_FLOAT) == 0)) {
                return true;
            }
            w->parent = parent;
            if (n < k->text_len) {
                embi_box *f = new_box(c, EMBI_BK_TEXT);
                f->flags |= EMBI_BF_INLINE_LEVEL;
                f->pe = EMBI_PE_FIRST_LETTER;
                set_el(c, f, el);
                f->style = ps;
                f->text = k->text;
                f->text_len = n;
                f->parent = w;
                w->first = f;
                k->text += n;
                k->text_len -= n;
                w->next = k;
                *slot = w;
            } else {
                w->next = k->next;
                k->next = nullptr;
                k->style = ps;
                k->parent = w;
                w->first = k;
                *slot = w;
                if (top != nullptr && top->tail == k) {
                    top->tail = w;
                }
            }
            return true;
        }
        if (k->kind == EMBI_BK_INLINE && (k->flags & EMBI_BF_ATOMIC) == 0) {
            if (k->pe == EMBI_PE_FIRST_LETTER) {
                return true;
            }
            if (fl_search(c, el, ps, &k->first, k, nullptr)) {
                return true;
            }
            continue;
        }
        if (k->kind == EMBI_BK_BLOCK && (k->flags & EMBI_BF_INLINE_LEVEL) == 0) {
            if (fl_search(c, el, ps, &k->first, k, nullptr)) {
                return true;
            }
            continue;
        }
        return true;
    }
    return false;
}

static void first_letter(embi_lctx *c, embi_element *el, embi_blist *kids)
{
    const embi_style *ps = embi_style_generated(c->style, el, EMBI_PE_FIRST_LETTER);
    if (ps != nullptr) {
        c->ld->has_fl = true; /* W6b: a first-letter box reaches into descendant blocks; no surgery on the tree (incr.c) */
        fl_search(c, el, ps, &kids->head, nullptr, kids);
    }
}

/* An element: one box (or several pieces for an inline split), added to l. */
/* input type=button, submit and reset: an inline-block container with the label as its text (the
 * value attribute, else "Submit" or "Reset"), its content centered (EMBI_BF_CENTER). Its size is the
 * label plus the UA padding and border, as in Chromium. An empty label is a zero width space, so the
 * button still has a line of text (Chromium gives the empty button the height of one line). Returns
 * false for other input types. */
static bool button_input(embi_lctx *c, embi_blist *l, embi_box *b, embi_element *el,
                         const embi_style *s, embi_bcx *cx, const char *type)
{
    bool submit = strcmp(type, "submit") == 0;
    bool reset = strcmp(type, "reset") == 0;
    if (!submit && !reset && strcmp(type, "button") != 0) {
        return false;
    }
    embi_sb label = {0};
    if (!attr_text(el, "value", &label)) {
        if (submit) {
            embi_sb_append(&label, EMBI_STR("Submit"));
        } else if (reset) {
            embi_sb_append(&label, EMBI_STR("Reset"));
        }
    }
    if (label.len == 0) {
        embi_sb_append(&label, EMBI_STR("\xE2\x80\x8B"));
    }
    embi_ws ws = {true, nullptr, false, false};
    embi_bcx kid = {s, &ws, false, false};
    embi_blist kids = {nullptr, nullptr};
    text_box(c, &kids, s, (embi_str){label.data, label.len}, 0, el, &kid, nullptr, el);
    embi_sb_free(&label);
    if (ws.trim != nullptr && ws.trim->text_len > 0) {
        ws.trim->text_len--;
    }
    b->flags |= EMBI_BF_CENTER;
    b->flags2 |= EMBI_BF2_CHROME; /* W5e: the painter draws the face */
    bl_attach(b, &kids);
    el->box = b;
    push_content(l, b, cx);
    return true;
}

/* The white-space-trim bits of style s: 1 discard-before, 2 discard-after, 4 discard-inner. */
static uint32_t ws_trim_bits(const embi_style *s)
{
    const embi_value *v = embi_style_value(s, EMBI_PROP_white_space_trim);
    if (v == nullptr) {
        return 0;
    }
    uint32_t n = 0, bits = 0;
    const embi_value *items = embi_value_items(v, &n);
    for (uint32_t i = 0; i < n; i++) {
        if (items[i].kind != EMBI_V_KEYWORD) {
            continue;
        }
        bits |= items[i].kw == EMBI_KW_discard_before ? 1u : items[i].kw == EMBI_KW_discard_after ? 2u :
                items[i].kw == EMBI_KW_discard_inner ? 4u : 0u;
    }
    return bits;
}

/* R4c: the box of a foreignObject element: a block that establishes a formatting context, whatever its display says (SVG 2
 * section 23.2 lays its content out as a CSS block in its viewport), with its children built as the content of a block. */
static void svg_fo_box(embi_lctx *c, embi_blist *l, embi_element *el, const embi_style *s)
{
    embi_bcx cx0 = {s, nullptr, false, false};
    embi_box *b = make_box(c, el, 0, s, EMBI_KW_flow_root, &cx0);
    if (b == nullptr) {
        return;
    }
    b->kind = EMBI_BK_BLOCK;
    b->flags &= (uint16_t)~(EMBI_BF_FLOAT | EMBI_BF_ABSPOS | EMBI_BF_INLINE_LEVEL | EMBI_BF_ATOMIC | EMBI_BF_BLOCKIFIED);
    b->flags |= EMBI_BF_BFC;
    b->flags2 |= EMBI_BF2_SVGFO;
    embi_ws ws = {true, nullptr, false, false};
    embi_bcx kid = {s, &ws, false, false};
    embi_blist kids = {nullptr, nullptr};
    build_children(c, &kids, el, s, &kid);
    if (ws.trim != nullptr && ws.trim->text_len > 0) {
        ws.trim->text_len--;
    }
    bl_attach(b, &kids);
    el->box = b;
    bl_push(l, b);
}

/* The foreignObject elements below the SVG element el (in tree order), not below another foreignObject and not under an
 * element with display: none (the UA sheet gives none to defs, symbol, clipPath, mask, marker, pattern and the gradients).
 * Whether the SVG walk reaches one (switch, use) is the walk's business: a box it never reaches is never painted. */
static void svg_fo_scan(embi_lctx *c, embi_blist *l, embi_element *el)
{
    if (c->bdepth >= c->max_depth) {
        return;
    }
    c->bdepth++;
    for (embi_node *n = el->base.first_child; n != nullptr; n = n->next) {
        if (!embi_is_element(n)) {
            continue;
        }
        embi_element *k = embi_as_element(n);
        if (k->base.extra != EMBI_NS_SVG) {
            continue;
        }
        const embi_style *s = embi_style_get(c->style, k, EMBI_PE_NONE);
        if (s == nullptr || embi_lay_ssum_of(c, s)->disp == EMBI_KW_none) {
            continue;
        }
        if (is_lit(embi_atom_str(k->base.name), "foreignobject")) {
            svg_fo_box(c, l, k, s);
        } else {
            svg_fo_scan(c, l, k);
        }
    }
    c->bdepth--;
}

/* Turns b into a replaced box when the content of its element is one <image> (element replacement, CSS Content 3
 * section 2). False, leaving b as it is, when the content is anything else or the image cannot be sized. */
static bool content_replaced(embi_lctx *c, embi_box *b, const embi_style *s, const embi_bcx *cx)
{
    const embi_value *cv = embi_style_value(s, EMBI_PROP_content);
    if (cv == nullptr || cv->kind == EMBI_V_KEYWORD) {
        return false;
    }
    embi_image im;
    if (!embi_lay_content_replaces(cv, &im)) {
        return false;
    }
    uint32_t n = 0;
    const embi_value *items = embi_value_items(cv, &n); /* the image is the first item (alt text follows it) */
    float w = 0, h = 0;
    bool ratio = false;
    if (n == 0 || !content_image_size(c, &items[0], &w, &h, &ratio)) {
        return false;
    }
    b->kind = EMBI_BK_REPLACED;
    b->first = nullptr;
    set_replaced(b, w, h, ratio);
    if (cx->blockify) {
        b->flags &= (uint16_t)~EMBI_BF_INLINE_LEVEL;
        b->flags |= EMBI_BF_BLOCKIFIED;
    }
    return true;
}

/* A fieldset with a layout-internal display (table-row, table-cell, table-caption, ...) generates a block box (HTML
 * section 15.3.13: it behaves as flow-root unless its outer display type is inline; table-display-on-fieldset). */
static uint16_t fieldset_display(uint16_t disp)
{
    switch (disp) {
    case EMBI_KW_table_row_group:
    case EMBI_KW_table_header_group:
    case EMBI_KW_table_footer_group:
    case EMBI_KW_table_row:
    case EMBI_KW_table_cell:
    case EMBI_KW_table_column_group:
    case EMBI_KW_table_column:
    case EMBI_KW_table_caption:
        return EMBI_KW_flow_root;
    default:
        return disp;
    }
}

static void build_element_body(embi_lctx *c, embi_blist *l, embi_element *el, embi_bcx *cx)
{
    const embi_style *s = embi_style_get(c->style, el, EMBI_PE_NONE);
    if (s == nullptr) {
        return;
    }
    const embi_lay_ssum sm0 = *embi_lay_ssum_of(c, s);
    uint16_t disp = sm0.disp;
    if (disp == EMBI_KW_none) {
        return;
    }
#if !EMBER_SVG
    if (el->base.extra == EMBI_NS_SVG) {
        return; /* SZ3b: without SVG support an SVG element generates no box and none of its descendants do */
    }
#endif
    bool is_html = el->base.extra == EMBI_NS_HTML;
    const uint8_t tf = tag_flags(c, el);
    if ((tf & TAG_FIELDSET) != 0) {
        disp = fieldset_display(disp);
    }
    if ((tf & TAG_REPLACED) != 0 && is_html && table_role(disp) != EMBI_TR_NONE && table_role(disp) != EMBI_TR_TABLE) {
        /* A replaced element with an internal table display (an img with display: table-cell) is an inline: it takes
         * part in the table's anonymous box generation like any other inline content (table-anonymous-objects-211). */
        disp = EMBI_KW_inline;
    }

    /* The counters of the element (CSS Lists 3 section 4), applied before its content. */
    bool is_li = embi_lay_disp_is_li(disp);
    int32_t li_value = 0;
    bool has_value = is_li && is_html && attr_int(el, "value", &li_value);
    /* An element that generates no box (display: contents) cannot set, reset or increment a counter (CSS Lists 3
     * section 4.1). */
    if (disp != EMBI_KW_contents && (is_li || has_value || !sm0.ctr_none)) {
        c->ld->ctr_used = true;
        c->ctr.lctx = c;
        c->ctr.cur_el = el;
        c->ctr.cur_pe = 0;
        embi_ctr_apply(&c->ctr, s, is_li, has_value, li_value);
    }
    if ((tf & TAG_OL) != 0) {
        c->ld->ctr_used = true;
        /* The start attribute is a hint for the counter-reset the UA sheet gives (ol, ol[reversed]); an author
         * counter-reset of another shape (a value, or reversed() without the attribute) replaces it. */
        int32_t start = 0;
        bool has_start = attr_int(el, "start", &start);
        embi_sb probe = {0};
        bool reversed = attr_text(el, "reversed", &probe);
        embi_sb_free(&probe);
        int32_t rnum = 0;
        bool rhas = false;
        embi_ctr_inst *top = nullptr;
        const embi_atom li_atom = list_item_atom();
        for (uint32_t i = c->ctr.st.len; i > 0 && top == nullptr; i--) {
            if (c->ctr.st.data[i - 1].name == li_atom) {
                top = &c->ctr.st.data[i - 1];
            }
        }
        if (has_start && top != nullptr &&
            embi_ctr_style_op(s, EMBI_PROP_counter_reset, list_item_atom(), &rnum, &rhas) && !rhas &&
            (top->dir < 0) == reversed) {
            int64_t sv = reversed ? (int64_t)start + 1 : (int64_t)start - 1;
            top->value = sv > 2100000000LL ? 2100000000 : (sv < -2100000000LL ? -2100000000 : (int32_t)sv);
        }
    }
    uint32_t child_mark = embi_ctr_mark(&c->ctr);
    uint32_t outer_sib = c->ctr.sib_base;
    c->ctr.sib_base = child_mark;

    if (disp == EMBI_KW_contents) {
        c->ld->has_contents = true; /* W6b: boxes of this element's text sit under another element's box (incr.c) */
        /* display: contents generates no box: the children join the parent's list. build_children
         * generates the ::before and ::after boxes itself. */
        embi_bcx kid = {s, cx->ws, cx->blockify, cx->in_inline};
        /* The children are in the parent's list, so for the counters they are siblings of the element's own siblings:
         * what they create stays for the siblings that follow (counter-order-display-contents). */
        c->ctr.sib_base = outer_sib;
        build_children(c, l, el, s, &kid);
        return;
    }

    embi_box *b = make_box(c, el, 0, s, disp, cx);
    if (b == nullptr) {
        embi_ctr_truncate(&c->ctr, child_mark);
        return;
    }
    if (c->root_style == nullptr) {
        c->root_style = s;
    }

    if ((tf & TAG_INPUT) != 0) {
        char type[24];
        input_type(el, type);
        if (button_input(c, l, b, el, s, cx, type)) {
            embi_ctr_truncate(&c->ctr, child_mark);
            return;
        }
    }

    /* Element replacement (CSS Content 3 section 2): content that is one <image> replaces the element's content (an
     * img's src too). */
    if (content_replaced(c, b, s, cx)) {
        el->box = b;
        push_content(l, b, cx);
        embi_ctr_truncate(&c->ctr, child_mark);
        return;
    }

    /* Replaced elements and form controls: no children, a natural size. */
    if (is_html || el->base.extra == EMBI_NS_SVG) {
        if (is_replaced_element(c, el) && replaced_for(c, b, el)) {
            b->kind = EMBI_BK_REPLACED;
            if (cx->blockify) {
                b->flags &= (uint16_t)~EMBI_BF_INLINE_LEVEL;
                b->flags |= EMBI_BF_BLOCKIFIED;
            }
            if (!is_html) {
                /* R4c: an svg root's foreignObject boxes are its children (laid out by embi_lay_replaced) */
                embi_blist fos = {nullptr, nullptr};
                svg_fo_scan(c, &fos, el);
                bl_attach(b, &fos);
            }
            el->box = b;
            push_content(l, b, cx);
            embi_ctr_truncate(&c->ctr, child_mark);
            return;
        }
    }

    /* <wbr>: a soft wrap opportunity, as a zero width space (CSS Text 3 section 5.1, HTML section 15.3.1). */
    if ((tf & TAG_WBR) != 0 && b->kind == EMBI_BK_INLINE && !cx->blockify) {
        text_box(c, l, s, (embi_str){"\xE2\x80\x8B", 3}, 0, nullptr, cx, nullptr, nullptr);
        embi_ctr_truncate(&c->ctr, child_mark);
        return;
    }

    /* <br>: an inline-level break with no children. */
    if ((tf & TAG_BR) != 0) {
        b->kind = EMBI_BK_BR;
        b->flags |= EMBI_BF_INLINE_LEVEL;
        el->box = b;
        push_content(l, b, cx);
        embi_ctr_truncate(&c->ctr, child_mark);
        return;
    }

    /* Inline box: its children are built in the parent's white space state, then split. */
    if (b->kind == EMBI_BK_INLINE) {
        embi_bcx kid = {s, cx->ws, cx->blockify, true};
        embi_blist sub = {nullptr, nullptr};
        /* white-space-trim (CSS Text 4 section 4.2): discard-before removes the collapsible space before the box,
         * discard-inner the ones at the start and the end inside it, discard-after the ones after it. */
        uint32_t wtrim = ws_trim_bits(s);
        if ((wtrim & 1u) != 0) {
            if (cx->ws->trim != nullptr && cx->ws->trim->text_len > 0) {
                cx->ws->trim->text_len--;
            }
            cx->ws->trim = nullptr;
            cx->ws->prev_space = true;
        }
        if ((wtrim & 4u) != 0) {
            cx->ws->prev_space = true;
        }
        embi_box *trim0 = cx->ws->trim;
        if (is_li) {
            marker_box(c, &sub, el, s, &kid, true);
        }
        pseudo_box(c, &sub, el, EMBI_PE_BEFORE, &kid);
        for (embi_node *k = el->base.first_child; k != nullptr; k = k->next) {
            build_node(c, &sub, k, &kid);
        }
        pseudo_box(c, &sub, el, EMBI_PE_AFTER, &kid);
        if ((wtrim & 4u) != 0 && cx->ws->trim != nullptr && cx->ws->trim != trim0 && cx->ws->trim->text_len > 0) {
            cx->ws->trim->text_len--;
            cx->ws->trim = nullptr;
            cx->ws->prev_space = false; /* the space was removed: a space after the box is not collapsed into it */
        }
        if ((wtrim & 2u) != 0) {
            cx->ws->prev_space = true;
        }
        bool any_block = false;
        bool any_tab = false;
        for (embi_box *k = sub.head; k != nullptr; k = k->next) {
            any_tab = any_tab || (is_table_internal(k) && !out_of_flow(k));
        }
        if (any_tab) {
            /* Table parts inside an inline box get an anonymous inline-table (CSS 2.1 section 17.2.1) before the
             * box is split around block-level children. */
            bl_attach(b, &sub);
            group_table_children(c, b, true);
            sub.head = b->first;
            sub.tail = b->first;
            while (sub.tail != nullptr && sub.tail->next != nullptr) {
                sub.tail = sub.tail->next;
            }
        }
        for (embi_box *k = sub.head; k != nullptr; k = k->next) {
            if (is_block_level(k)) {
                any_block = true;
                break;
            }
        }
        if (!any_block) {
            bl_attach(b, &sub);
            el->box = b;
            bl_push(l, b);
        } else {
            embi_box *first = split_inline(c, l, b, &sub, cx);
            if (first != nullptr) {
                el->box = first;
            }
        }
        embi_ctr_truncate(&c->ctr, child_mark);
        return;
    }

    /* Block containers, atomic inlines, flex, grid and table boxes: their own white space state. */
    embi_ws ws = {true, nullptr, false, false};
    embi_bcx kid = {s, &ws, false, false};
    if (b->kind == EMBI_BK_FLEX || b->kind == EMBI_BK_GRID) {
        kid.blockify = true;
    }
    embi_blist kids = {nullptr, nullptr};
    if (is_li) {
        marker_box(c, &kids, el, s, &kid, false);
    }
    build_children(c, &kids, el, s, &kid);
    if (ws.trim != nullptr && ws.trim->text_len > 0) {
        /* Trailing collapsible space at the end of the last line (CSS Text 3 section 4.1.2). */
        ws.trim->text_len--;
    }
    if (!c->no_pseudo && (b->kind == EMBI_BK_BLOCK || b->tr == EMBI_TR_CELL || b->tr == EMBI_TR_CAPTION)) {
        first_letter(c, el, &kids);
    }
    bl_attach(b, &kids);
    el->box = b;
    push_content(l, b, cx);
    embi_ctr_truncate(&c->ctr, child_mark);
}

/* One element: its counters, then its box and children. The body sets the sibling base of the
 * children; the wrapper restores the base of the element's own level afterwards. */
static void build_element(embi_lctx *c, embi_blist *l, embi_element *el, embi_bcx *cx)
{
    if (c->bdepth >= c->max_depth) {
        return; /* too deep: the element and its subtree get no box */
    }
    uint32_t saved = c->ctr.sib_base;
    uint32_t ctr_start = c->ctr.st.len;
    c->bdepth++;
    build_element_body(c, l, el, cx);
    c->bdepth--;
    if (c->ctr.st.len > ctr_start) { /* most elements create no counter: skip the atom lookup */
        embi_ctr_end_element(&c->ctr, ctr_start, saved, list_item_atom());
    }
    c->ctr.sib_base = saved;
}

static void build_node(embi_lctx *c, embi_blist *l, embi_node *n, embi_bcx *cx)
{
    if (embi_is_element(n)) {
        build_element(c, l, embi_as_element(n), cx);
    } else if (n->type == EMBI_NODE_TEXT) {
        build_text_node(c, l, embi_as_chardata(n), cx);
    }
}

/* The children of an element: ::before, the DOM children, ::after. */
static void build_children(embi_lctx *c, embi_blist *l, embi_element *el, const embi_style *s,
                           embi_bcx *cx)
{
    (void)s;
    pseudo_box(c, l, el, EMBI_PE_BEFORE, cx);
    for (embi_node *k = el->base.first_child; k != nullptr; k = k->next) {
        build_node(c, l, k, cx);
    }
    pseudo_box(c, l, el, EMBI_PE_AFTER, cx);
}

/* ------------------------------------------------------------------ anonymous wrapping */

static bool is_ws_text(const embi_box *b);

/* A run of inline-level boxes that holds nothing but an outside marker (and collapsible white space). */
static embi_box *marker_only_run(const embi_blist *run)
{
    embi_box *m = nullptr;
    for (embi_box *q = run->head; q != nullptr; q = q->next) {
        if ((q->flags & EMBI_BF_OUTSIDE) != 0 && m == nullptr) {
            m = q;
        } else if (!(q->kind == EMBI_BK_TEXT && (q->text_len == 0 || is_ws_text(q)))) {
            return nullptr;
        }
    }
    return m;
}

/* The block container that takes an outside marker followed by the block-level box nb: the marker hangs
 * beside the first line of the list item, which is the first line of its first block (CSS Lists 3
 * section 3.1, "the marker is placed ... on the first line box"), so no anonymous line is made for it.
 * A flex or grid container passes it on to its first in-flow item. nullptr when nb cannot take it. */
static embi_box *marker_host(embi_box *nb)
{
    if (nb->kind == EMBI_BK_FLEX || nb->kind == EMBI_BK_GRID) {
        embi_box *it = nb->first;
        while (it != nullptr && (out_of_flow(it) || (it->kind == EMBI_BK_TEXT && is_ws_text(it)))) {
            it = it->next;
        }
        if (it == nullptr || it->kind != EMBI_BK_BLOCK || (it->flags & EMBI_BF_INLINE_LEVEL) != 0) {
            return nullptr;
        }
        return it;
    }
    if (nb->kind != EMBI_BK_BLOCK || (nb->flags & EMBI_BF_INLINE_LEVEL) != 0 || nb->first == nullptr) {
        return nullptr;
    }
    return nb;
}

/* Wraps the runs of inline-level children of a block container in anonymous blocks, when the
 * container also has block-level children (CSS 2.1 section 9.2.1.1). A run of nothing but collapsed
 * white space is dropped. Without block-level children the container holds inline content (IFC). */
static void wrap_runs(embi_lctx *c, embi_box *parent)
{
    if (parent->first == nullptr) {
        return;
    }
    bool any_block = false;
    for (embi_box *k = parent->first; k != nullptr; k = k->next) {
        if (is_block_level(k)) {
            any_block = true;
            break;
        }
    }
    /* The children of a flex or grid container are all items: its inline runs become anonymous items
     * even when there is no block-level child (CSS Flexbox 1 section 4, CSS Grid 2 section 6), so it has
     * no inline content. */
    bool flex = parent->kind == EMBI_BK_FLEX || parent->kind == EMBI_BK_GRID;
    if (!any_block && !flex) {
        parent->flags |= EMBI_BF_IFC;
        return;
    }
    embi_blist out = {nullptr, nullptr};
    embi_box *k = parent->first;
    while (k != nullptr) {
        if (is_block_level(k) || out_of_flow(k)) {
            /* A block-level box stands alone. A float or abspos box is left as it is, so it is not
             * taken into an anonymous block. */
            embi_box *nx = k->next;
            k->next = nullptr;
            bl_push(&out, k);
            k = nx;
            continue;
        }
        embi_blist run = {nullptr, nullptr};
        bool visible = false;
        /* A float that follows inline content joins its run: it is placed in the line it comes in, beside that line's
         * text (CSS 2.1 9.5.1; floats-029). An absolute box still ends the run, and in a flex or grid
         * container a float is an item of its own. */
        while (k != nullptr && !is_block_level(k) && ((k->flags & EMBI_BF_ABSPOS) == 0) && (!flex || (k->flags & EMBI_BF_FLOAT) == 0)) {
            embi_box *nx = k->next;
            k->next = nullptr;
            if (!(k->kind == EMBI_BK_TEXT && k->text_len == 0) && (flex || (k->flags & EMBI_BF_FLOAT) == 0)) {
                visible = true;
            }
            bl_push(&run, k);
            k = nx;
        }
        if (!visible) {
            /* Only collapsed white space and floats: the white space goes, the floats stay where they are. */
            for (embi_box *q = run.head; q != nullptr;) {
                embi_box *qn = q->next;
                q->next = nullptr;
                if ((q->flags & EMBI_BF_FLOAT) != 0) {
                    bl_push(&out, q);
                }
                q = qn;
            }
            continue;
        }
        embi_box *mk = k != nullptr && !flex && is_block_level(k) && !out_of_flow(k) ? marker_only_run(&run) : nullptr;
        embi_box *host = mk != nullptr ? marker_host(k) : nullptr;
        if (host != nullptr) {
            mk->next = host->first;
            host->first = mk;
            mk->parent = host;
            continue;
        }
        embi_box *anon = new_box(c, EMBI_BK_BLOCK);
        anon->flags |= EMBI_BF_ANON | EMBI_BF_IFC;
        if (flex) {
            anon->flags |= EMBI_BF_BFC; /* a flex or grid item is a block container that establishes a BFC */
        }
        set_el(c, anon, parent->el);
        anon->style = parent->style;
        bl_attach(anon, &run);
        bl_push(&out, anon);
    }
    parent->first = out.head;
    for (embi_box *q = out.head; q != nullptr; q = q->next) {
        q->parent = parent;
    }
}

/* ------------------------------------------------------------------ table objects (W3g) */

/* Whitespace-only text: removed between the parts of a table. */
static bool is_ws_text(const embi_box *b)
{
    if (b->kind != EMBI_BK_TEXT) {
        return false;
    }
    for (uint32_t i = 0; i < b->text_len; i++) {
        char ch = b->text[i];
        if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r' && ch != '\f') {
            return false;
        }
    }
    return true;
}

/* An anonymous table box of a role (a table, row or cell). It takes the style of its parent, so its
 * non-inherited properties are the initial values (EMBI_BF_ANON). */
static embi_box *anon_table_box(embi_lctx *c, const embi_box *parent, uint8_t role)
{
    embi_box *a = new_box(c, EMBI_BK_TABLE);
    a->tr = role;
    a->flags |= EMBI_BF_ANON | EMBI_BF_BFC;
    set_el(c, a, parent->el);
    a->style = parent->style;
    return a;
}

/* The open anonymous row and cell of table_fixup. Each keeps its children in a list until it is
 * closed and attached. */
typedef struct tfix {
    embi_box *row;
    embi_blist row_kids;
    embi_box *cell;
    embi_blist cell_kids;
} tfix;

/* Closes the open anonymous cell into the list out (a row's children). */
static void tf_end_cell(tfix *s, embi_blist *out)
{
    if (s->cell == nullptr) {
        return;
    }
    bl_attach(s->cell, &s->cell_kids);
    bl_push(out, s->cell);
    s->cell = nullptr;
    s->cell_kids = (embi_blist){nullptr, nullptr};
}

/* Closes the open anonymous row (and its cell) into the list out (the children of the table). */
static void tf_end_row(tfix *s, embi_blist *out)
{
    tf_end_cell(s, &s->row_kids);
    if (s->row == nullptr) {
        return;
    }
    bl_attach(s->row, &s->row_kids);
    bl_push(out, s->row);
    s->row = nullptr;
    s->row_kids = (embi_blist){nullptr, nullptr};
}

/* What a child of a table part is for it: 0 a direct child, 1 a cell that goes into an anonymous row,
 * 2 a table-internal box that is dropped, 3 a box that goes into an anonymous cell. */
static int tf_class(uint8_t parent, const embi_box *k)
{
    /* A misparented table part other than a column goes into an anonymous cell (and row), which wraps it in an
     * anonymous table (CSS 2.1 section 17.2.1 rules 2.1 and 2.2; display-contents-003). */
    bool col = k->tr == EMBI_TR_COL || k->tr == EMBI_TR_COLGROUP;
    if (parent == EMBI_TR_ROW) {
        if (k->tr == EMBI_TR_CELL) {
            return 0;
        }
        return col ? 2 : 3;
    }
    if (parent == EMBI_TR_TABLE) {
        if (k->tr == EMBI_TR_CELL) {
            return 1;
        }
        if (is_table_internal(k)) {
            return 0; /* caption, column, column group, row group or row */
        }
        return 3;
    }
    /* A row group: rows are direct, cells go into anonymous rows, columns are dropped. */
    if (k->tr == EMBI_TR_ROW) {
        return 0;
    }
    if (k->tr == EMBI_TR_CELL) {
        return 1;
    }
    return col ? 2 : 3;
}

/* The children of a table, row group or row: missing rows and cells are generated (CSS Tables 3
 * section 3), whitespace-only text is removed, and out-of-flow boxes stay where they are. */
static void table_fixup(embi_lctx *c, embi_box *b)
{
    bool rowp = b->tr == EMBI_TR_ROW;
    if (is_row_group(b->tr)) {
        /* A row group inside a row group is not a valid part: its content joins the outer group (the rows and the
         * cells in a run go to the same anonymous row, as Chromium does; WPT table-row-group-nested-anonymous-001). */
        embi_blist flat = {nullptr, nullptr};
        embi_box *q = b->first;
        bool changed = false;
        while (q != nullptr) {
            embi_box *qn = q->next;
            q->next = nullptr;
            if (is_row_group(q->tr) && (q->flags & EMBI_BF_ABSPOS) == 0) {
                changed = true;
                for (embi_box *g = q->first; g != nullptr;) {
                    embi_box *gn = g->next;
                    g->next = nullptr;
                    bl_push(&flat, g);
                    g = gn;
                }
                q->first = nullptr;
            } else {
                bl_push(&flat, q);
            }
            q = qn;
        }
        if (changed) {
            bl_attach(b, &flat);
        } else {
            b->first = flat.head;
        }
    }
    embi_blist out = {nullptr, nullptr};
    tfix s;
    memset(&s, 0, sizeof s);
    embi_box *k = b->first;
    while (k != nullptr) {
        embi_box *nx = k->next;
        k->next = nullptr;
        if ((k->flags & EMBI_BF_ABSPOS) != 0) {
            /* A positioned box stays where it is; a float is in flow here and goes into an anonymous cell like any
             * other box that is not a table part. */
            bl_push(&out, k);
        } else if (is_ws_text(k)) {
            /* White space between two boxes that go into the same anonymous cell is part of its inline content. */
            embi_box *p = nx;
            while (p != nullptr && ((p->flags & EMBI_BF_ABSPOS) != 0 || is_ws_text(p))) {
                p = p->next;
            }
            if (s.cell != nullptr || (p != nullptr && tf_class(b->tr, p) == 3)) {
                /* Preserved white space ahead of the first box of an anonymous cell is part of the cell's content
                 * (table-anonymous-objects-211), and so is the white space after the last one, up to the next table
                 * part (table-anonymous-objects-199: pre-wrapped spaces keep the width of the cell); collapsible white
                 * space there is removed with the line anyway. */
                if (s.cell == nullptr) {
                    if (rowp) {
                        s.cell = anon_table_box(c, b, EMBI_TR_CELL);
                    } else {
                        if (s.row == nullptr) {
                            s.row = anon_table_box(c, b, EMBI_TR_ROW);
                        }
                        s.cell = anon_table_box(c, b, EMBI_TR_CELL);
                    }
                }
                bl_push(&s.cell_kids, k);
            }
        } else {
            int cls = tf_class(b->tr, k);
            if (cls == 0) {
                if (rowp) {
                    tf_end_cell(&s, &out);
                } else {
                    tf_end_row(&s, &out);
                }
                bl_push(&out, k);
            } else if (cls == 1) {
                if (s.row == nullptr) {
                    s.row = anon_table_box(c, b, EMBI_TR_ROW);
                }
                tf_end_cell(&s, &s.row_kids);
                bl_push(&s.row_kids, k);
            } else if (cls == 3) {
                if (rowp) {
                    if (s.cell == nullptr) {
                        s.cell = anon_table_box(c, b, EMBI_TR_CELL);
                    }
                    bl_push(&s.cell_kids, k);
                } else {
                    if (s.row == nullptr) {
                        s.row = anon_table_box(c, b, EMBI_TR_ROW);
                    }
                    if (s.cell == nullptr) {
                        s.cell = anon_table_box(c, b, EMBI_TR_CELL);
                    }
                    bl_push(&s.cell_kids, k);
                }
            } /* cls 2: dropped */
        }
        k = nx;
    }
    if (rowp) {
        tf_end_cell(&s, &out);
    } else {
        tf_end_row(&s, &out);
    }
    bl_attach(b, &out);
}

/* The table-internal children of a block container (not a table part) are wrapped in anonymous
 * tables (CSS Tables 3 section 3): a run of them, with the whitespace between them, becomes one
 * table. */
static void group_table_children(embi_lctx *c, embi_box *parent, bool inline_parent)
{
    bool any = false;
    for (embi_box *k = parent->first; k != nullptr; k = k->next) {
        if (is_table_internal(k) && !out_of_flow(k)) {
            any = true;
            break;
        }
    }
    if (!any) {
        return;
    }
    embi_blist out = {nullptr, nullptr};
    embi_blist kids = {nullptr, nullptr};
    embi_blist pend = {nullptr, nullptr}; /* whitespace after the last table part so far */
    embi_box *anon = nullptr;
    embi_box *k = parent->first;
    while (k != nullptr) {
        embi_box *nx = k->next;
        k->next = nullptr;
        if (is_table_internal(k) && !out_of_flow(k)) {
            if (anon == nullptr) {
                anon = anon_table_box(c, parent, EMBI_TR_TABLE);
                if (inline_parent) {
                    /* Under an inline box it is an anonymous inline-table (CSS 2.1 section 17.2.1). */
                    anon->flags |= EMBI_BF_INLINE_LEVEL | EMBI_BF_ATOMIC;
                }
            }
            bl_push(&kids, k);
        } else if (anon != nullptr && is_ws_text(k)) {
            /* whitespace between the table parts is removed; at the end of the run it stays outside the table */
            bl_push(&pend, k);
        } else {
            if (anon != nullptr) {
                bl_attach(anon, &kids);
                bl_push(&out, anon);
                anon = nullptr;
                kids = (embi_blist){nullptr, nullptr};
                for (embi_box *w = pend.head; w != nullptr;) {
                    embi_box *wn = w->next;
                    w->next = nullptr;
                    bl_push(&out, w);
                    w = wn;
                }
                pend = (embi_blist){nullptr, nullptr};
            }
            bl_push(&out, k);
        }
        k = nx;
        if (anon != nullptr && k != nullptr && is_table_internal(k) && !out_of_flow(k)) {
            pend = (embi_blist){nullptr, nullptr}; /* whitespace between two parts: dropped */
        }
    }
    if (anon != nullptr) {
        bl_attach(anon, &kids);
        bl_push(&out, anon);
        for (embi_box *w = pend.head; w != nullptr;) {
            embi_box *wn = w->next;
            w->next = nullptr;
            bl_push(&out, w);
            w = wn;
        }
    }
    parent->first = out.head;
    for (embi_box *q = out.head; q != nullptr; q = q->next) {
        q->parent = parent;
    }
}

/* Anonymous wrapping for every block container of the tree below b, and the table objects. */
static void finish_tree(embi_lctx *c, embi_box *b)
{
    if (embi_box_is_text(b)) {
        return; /* a text run has no children */
    }
    if ((b->flags & EMBI_BF_ANON) == 0 && b->style != nullptr && embi_lay_ssum_of(c, b->style)->calc_size) {
        b->flags2 |= EMBI_BF2_CALCSIZE;
    }
    if (b->kind == EMBI_BK_INLINE) {
        group_table_children(c, b, true);
    }
    if (b->kind == EMBI_BK_TABLE && is_table_container(b->tr)) {
        table_fixup(c, b);
    } else if ((b->kind == EMBI_BK_BLOCK || b->kind == EMBI_BK_TABLE || b->kind == EMBI_BK_FLEX ||
                b->kind == EMBI_BK_GRID) &&
               (!(b->flags & EMBI_BF_ANON) || b->kind == EMBI_BK_TABLE)) {
        /* Captions and cells are block containers; a column group keeps its columns, and the other
         * boxes of a table part never reach this point. */
        if (b->kind == EMBI_BK_BLOCK ||
            (b->kind == EMBI_BK_TABLE && (b->tr == EMBI_TR_CELL || b->tr == EMBI_TR_CAPTION))) {
            group_table_children(c, b, false);
        }
        if (b->kind != EMBI_BK_TABLE || (b->tr == EMBI_TR_CELL || b->tr == EMBI_TR_CAPTION)) {
            wrap_runs(c, b);
        }
    }
    for (embi_box *k = b->first; k != nullptr; k = k->next) {
        finish_tree(c, k);
    }
}

embi_box *embi_lay_build_root(embi_lctx *c, embi_element *root)
{
    const embi_style *s = embi_style_get(c->style, root, EMBI_PE_NONE);
    if (s == nullptr) {
        return nullptr;
    }
    embi_ws ws = {true, nullptr, false, false};
    embi_bcx cx = {s, &ws, false, false};
    embi_blist l = {nullptr, nullptr};
    build_element(c, &l, root, &cx);
    embi_box *b = l.head;
    if (b == nullptr) {
        return nullptr;
    }
    if (embi_box_is_text(b) || b->next != nullptr || b->el != root) {
        /* display: contents on the root element: its children are the content of the initial containing block, so
         * they get a block of their own to be the root box (the head of the list can be a text run, and there can
         * be several siblings). */
        embi_box *w = new_box(c, EMBI_BK_BLOCK);
        set_el(c, w, root);
        w->style = s;
        bl_attach(w, &l);
        b = w;
    }
    b->flags |= EMBI_BF_ROOT | EMBI_BF_BFC;
    /* The overflow of the root element, or of the body when the root's is visible, belongs to the viewport (CSS
     * Overflow 3 section 3.3): that element is not itself a scroll container or a clip, so a sticky box inside it
     * sticks to the viewport and its content is not clipped at its own edge. */
    {
        const embi_lay_ssum *rs = embi_lay_ssum_of(c, s);
        bool vis = (rs->ox == 0 || rs->ox == EMBI_KW_visible) && (rs->oy == 0 || rs->oy == EMBI_KW_visible);
        b->flags &= (uint16_t)~(EMBI_BF_SCROLL | EMBI_BF_CLIP);
        if (vis && !embi_box_is_text(b)) {
            for (embi_box *k = b->first; k != nullptr; k = k->next) {
                if (!embi_box_is_text(k) && k->el != nullptr && k->el->base.parent == (const embi_node *)root && k->kind != EMBI_BK_INLINE &&
                    is_lit(embi_atom_str(k->el->base.name), "body")) {
                    /* Its block formatting context came from the overflow, so it goes too (its margins collapse
                     * with its children's again). */
                    bool was_scroll = (k->flags & EMBI_BF_SCROLL) != 0;
                    k->flags &= (uint16_t)~(EMBI_BF_SCROLL | EMBI_BF_CLIP);
                    if (was_scroll && k->kind == EMBI_BK_BLOCK && k->style != nullptr &&
                        (k->flags & (EMBI_BF_FLOAT | EMBI_BF_ABSPOS)) == 0) {
                        const embi_lay_ssum *bs = embi_lay_ssum_of(c, k->style);
                        if (!bs->contain_lay && !bs->contain_paint && bs->disp == EMBI_KW_block) {
                            k->flags &= (uint16_t)~EMBI_BF_BFC;
                        }
                    }
                    break;
                }
            }
        }
    }
    finish_tree(c, b);
    return b;
}

void embi_lay_clear_boxes(embi_document *doc)
{
    embi_node *root = &doc->base;
    for (embi_node *k = embi_next_in_tree(root, root); k != nullptr;
         k = embi_next_in_tree(root, k)) {
        if (embi_is_element(k)) {
            embi_as_element(k)->box = nullptr;
        }
    }
}


/* ------------------------------------------------------------------ box-tree surgery (W6b, incr.c) */

/* True when a content value reads the counter state (counter(), counters(), quotes). */
static bool content_reads_counters(const embi_value *v)
{
    if (v == nullptr) {
        return false;
    }
    switch (v->kind) {
    case EMBI_V_FUNC:
        return is_lit(embi_atom_str(v->func.name), "counter") || is_lit(embi_atom_str(v->func.name), "counters");
    case EMBI_V_KEYWORD:
        return v->kw == EMBI_KW_open_quote || v->kw == EMBI_KW_close_quote || v->kw == EMBI_KW_no_open_quote ||
               v->kw == EMBI_KW_no_close_quote;
    case EMBI_V_LIST:
        for (uint32_t i = 0; i < v->list.n; i++) {
            if (content_reads_counters(&v->list.items[i])) {
                return true;
            }
        }
        return false;
    default:
        return false;
    }
}

/* True when the box takes part in the counters itself (decided from its style when it is built). */
static bool box_in_counters(const embi_lctx *c, const embi_box *b)
{
    if ((b->flags & EMBI_BF_ANON) != 0 || b->style == nullptr) {
        return false;
    }
    const embi_lay_ssum *sm = embi_lay_ssum_of(c, b->style);
    if (!sm->ctr_none || embi_lay_disp_is_li(sm->disp)) {
        return true;
    }
    if (b->pe != 0 && content_reads_counters(embi_style_value(b->style, EMBI_PROP_content))) {
        return true;
    }
    return b->el != nullptr && b->pe == 0 && is_lit(embi_atom_str(b->el->base.name), "ol");
}

/* True when nothing in the box subtree b takes part in the counters (list items, counter properties, counter() and quotes
 * in generated content): then the boxes after it do not depend on it and it can be built without the counter state. The
 * boxes are asked by their flag: their styles may be freed already when a surgery looks at the old subtree. */
static bool subtree_counter_free(const embi_box *b)
{
    if (b->kind == EMBI_BK_MARKER || (b->flags2 & EMBI_BF2_CTR) != 0) {
        return false;
    }
    if (embi_box_is_text(b)) {
        return true;
    }
    for (const embi_box *k = b->first; k != nullptr; k = k->next) {
        if (!subtree_counter_free(k)) {
            return false;
        }
    }
    return true;
}

/* True when the box subtree b holds the ::marker box of a list item that is not el or inside it: the marker of an item
 * goes into the first block of the item, so the subtree of that block cannot be built without the item. */
static bool subtree_has_outer_marker(const embi_box *b, const embi_element *el)
{
    if (b->kind == EMBI_BK_MARKER) {
        for (const embi_node *n = b->el != nullptr ? &b->el->base : nullptr; n != nullptr; n = n->parent) {
            if (n == &el->base) {
                return false;
            }
        }
        return true;
    }
    if (embi_box_is_text(b)) {
        return false;
    }
    for (const embi_box *k = b->first; k != nullptr; k = k->next) {
        if (subtree_has_outer_marker(k, el)) {
            return true;
        }
    }
    return false;
}

static bool subtree_has_abspos(const embi_box *b)
{
    if (embi_box_is_text(b)) {
        return false;
    }
    if ((b->flags & EMBI_BF_ABSPOS) != 0) {
        return true;
    }
    for (const embi_box *k = b->first; k != nullptr; k = k->next) {
        if (subtree_has_abspos(k)) {
            return true;
        }
    }
    return false;
}

/* Rebuilds the box subtree of el in place of old (its principal box, built before): the boxes of el and everything below
 * it are new, the boxes around it stay. Only block-level block containers whose parent is a plain block qualify: their
 * build reads nothing of what came before them (their own white space state), and what the parent did with them (the
 * anonymous wrapping of the inline runs around) does not change while their kind and flags stay the same. Returns false,
 * with the tree around the old box intact, when the new box does not fit where the old one was (the caller rebuilds
 * the whole tree then). The element slots of the subtree are overwritten either way.
 * The build runs without the counter state. CF1: when the tree reads no counter value (ld->ctr_read: no counter(), no
 * quotes, no numbered marker), that state is not observable and a subtree with list items or counter properties may be
 * rebuilt as long as its new boxes read none either; otherwise the old subtree and the new one must not take part in the
 * counters at all. */
bool embi_lay_rebuild_element(embi_lctx *c, embi_element *el, embi_box *old)
{
    embi_box *par = old->parent;
    if (par == nullptr || par->kind != EMBI_BK_BLOCK || (par->flags & EMBI_BF_IFC) != 0) {
        return false;
    }
    if (embi_box_is_text(old) || old->el != el || old->pe != 0 ||
        (old->flags & (EMBI_BF_ANON | EMBI_BF_INLINE_LEVEL | EMBI_BF_ROOT)) != 0 ||
        (old->flags2 & (EMBI_BF2_NOSTART | EMBI_BF2_NOEND)) != 0 || old->tr != EMBI_TR_NONE ||
        (old->kind != EMBI_BK_BLOCK && old->kind != EMBI_BK_FLEX && old->kind != EMBI_BK_GRID)) {
        return false;
    }
    /* CF1: an outside marker of the item above that sits in an anonymous block just before this box: whether the box
     * takes it instead (marker_host) depends on what is built in it, which the item's build decides */
    {
        embi_box *prev = nullptr;
        for (embi_box *k = par->first; k != nullptr && k != old; k = k->next) {
            prev = k;
        }
        if (prev != nullptr && (prev->flags & EMBI_BF_ANON) != 0 && subtree_has_outer_marker(prev, el)) {
            return false;
        }
    }
    const bool reads = c->ld->ctr_read;
    if (reads ? !subtree_counter_free(old) : subtree_has_outer_marker(old, el)) {
        return false; /* the counters after it depend on it, or it holds the marker of an item above it */
    }
    if (subtree_has_abspos(old)) {
        c->ld->abs_gone = true; /* the fragments of its out-of-flow boxes are not in the subtree: no partial relayout */
    }
    /* the slots of the elements below point into boxes that are about to be unlinked */
    {
        embi_node *top = &el->base;
        for (embi_node *n = top; n != nullptr; n = embi_next_in_tree(top, n)) {
            if (embi_is_element(n)) {
                embi_as_element(n)->box = nullptr;
            }
        }
    }
    const embi_style *inh = nullptr;
    if (el->base.parent != nullptr && embi_is_element(el->base.parent)) {
        inh = (const embi_style *)embi_as_element(el->base.parent)->style;
    }
    embi_ws ws = {true, nullptr, false, false};
    embi_bcx cx = {inh, &ws, false, false};
    embi_blist l = {nullptr, nullptr};
    /* the build must not touch the counters, ::first-letter or display: contents: it runs without their state */
    bool sc = c->ld->ctr_used, sr = c->ld->ctr_read, sf = c->ld->has_fl, sk = c->ld->has_contents;
    c->ld->ctr_used = c->ld->ctr_read = c->ld->has_fl = c->ld->has_contents = false;
    build_element(c, &l, el, &cx);
    bool touched = (reads ? c->ld->ctr_used : c->ld->ctr_read) || c->ld->has_fl || c->ld->has_contents;
    c->ld->ctr_used = sc || c->ld->ctr_used;
    c->ld->ctr_read = sr || c->ld->ctr_read;
    c->ld->has_fl = sf || c->ld->has_fl;
    c->ld->has_contents = sk || c->ld->has_contents;
    embi_box *nb = l.head;
    if (touched) {
        return false;
    }
    const uint16_t keep = EMBI_BF_FLOAT | EMBI_BF_ABSPOS | EMBI_BF_INLINE_LEVEL | EMBI_BF_ATOMIC | EMBI_BF_BLOCKIFIED | EMBI_BF_ROOT;
    if (nb == nullptr || nb->next != nullptr || nb->el != el || nb->pe != 0 || nb->kind != old->kind ||
        nb->tr != old->tr || ((nb->flags ^ old->flags) & keep) != 0) {
        return false;
    }
    /* The body of a root whose overflow is visible carries the viewport's overflow, not its own (embi_lay_build_root). */
    embi_element *rootel = embi_document_element(el->base.doc);
    if (rootel != nullptr && el->base.parent == &rootel->base && is_lit(embi_atom_str(el->base.name), "body")) {
        const embi_style *rs = embi_style_get(c->style, rootel, EMBI_PE_NONE);
        if (rs != nullptr) {
            const embi_lay_ssum *sm = embi_lay_ssum_of(c, rs);
            bool vis = (sm->ox == 0 || sm->ox == EMBI_KW_visible) && (sm->oy == 0 || sm->oy == EMBI_KW_visible);
            if (vis) {
                nb->flags &= (uint16_t)~(EMBI_BF_SCROLL | EMBI_BF_CLIP);
            }
        }
    }
    finish_tree(c, nb);
    /* splice it in where the old box was */
    nb->parent = par;
    nb->next = old->next;
    if (par->first == old) {
        par->first = nb;
    } else {
        embi_box *k = par->first;
        while (k != nullptr && k->next != old) {
            k = k->next;
        }
        if (k == nullptr) {
            return false;
        }
        k->next = nb;
    }
    el->box = nb;
    return true;
}
