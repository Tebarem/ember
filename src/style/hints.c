/*
 * Presentational hints (W2d, HTML Standard section 15.3). The legacy attributes of HTML elements
 * map to author-level declarations of zero specificity, below every author rule.
 *
 * Covered: bgcolor (table cells, table parts, body), body text, width and height (img, table,
 * cells, col, hr, iframe, video, embed, object; not canvas, whose size attributes only give its
 * ratio), align (block elements, headings, table parts, hr), valign (cells, rows, row groups, col),
 * table border, nowrap (cells), cellpadding (cells, from the nearest table), cellspacing (tables, as
 * border-spacing), dir, the font element's
 * color, face and size, and the center element.
 *
 * Not covered yet (listed so the gap is explicit): table and body
 * background, hspace and vspace, body link, vlink and alink, img and iframe border, hr size, width
 * and noshade, marginwidth and marginheight, and the legacy list and ol attributes.
 *
 * A cached declaration list is keyed by a hash of its text and the text is compared on every hit,
 * so a hash collision cannot apply the wrong declarations.
 */
#include "style_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool tag_in(const char *tag, const char *const *list, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        if (strcmp(tag, list[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* The declarations of one hint (property name and CSS value), cached by their text. */
static const embi_pdecl *hint_decls(embi_style_engine *e, const char *prop, embi_str value,
                                    uint32_t *count)
{
    uint32_t plen = (uint32_t)strlen(prop);
    uint32_t tlen = plen + 1 + value.len;
    char text[320];
    if (tlen > sizeof text) {
        *count = 0;
        return nullptr;
    }
    memcpy(text, prop, plen);
    text[plen] = '\x01';
    memcpy(text + plen + 1, value.ptr, value.len);
    uint64_t key = embi_hash_bytes(text, tlen, 0x4849u);
    uint64_t found = 0;
    if (embi_hashmap_get(&e->hints, key, &found)) {
        const embi_style_declrec *rec = (const embi_style_declrec *)(uintptr_t)found;
        if (rec->text.len == tlen && memcmp(rec->text.ptr, text, tlen) == 0) {
            *count = rec->n;
            return rec->d;
        }
    }
    embi_str kept = embi_style_copy_str(&e->arena, (embi_str){text, tlen});
    embi_str vtext = embi_style_copy_str(&e->arena, value);
    embi_css_decl d;
    memset(&d, 0, sizeof d);
    d.name = embi_atom_intern(prop, plen);
    d.value = embi_css_parse_cvs(&e->arena, vtext);
    uint32_t cnt = 0;
    const embi_pdecl *decls = embi_style_preparse(e, &d, 1, &cnt);
    embi_style_declrec *rec = embi_arena_alloc(&e->arena, sizeof *rec, 8);
    rec->d = decls;
    rec->n = cnt;
    rec->text = kept;
    embi_hashmap_put(&e->hints, key, (uint64_t)(uintptr_t)rec);
    *count = cnt;
    return decls;
}

static void add_hint(embi_style_engine *e, embi_sctx *sc, const char *prop, const char *value)
{
    uint32_t n = 0;
    embi_str v = {value, (uint32_t)strlen(value)};
    const embi_pdecl *d = hint_decls(e, prop, v, &n);
    embi_sel_spec zero = {0, 0, 0};
    embi_style_push_decls(sc, d, n, EMBI_ORIGIN_AUTHOR, 0, zero, 0, false);
}

/* A legacy length: leading digits (optionally followed by %). Writes CSS px or % text into buf.
 * Returns false when the value has no leading digit. */
static bool legacy_length(const char *s, uint32_t len, char *buf, uint32_t cap)
{
    uint32_t i = 0;
    while (i < len && s[i] >= '0' && s[i] <= '9') {
        i++;
    }
    if (i == 0) {
        return false;
    }
    bool pct = i < len && s[i] == '%';
    int w = SDL_snprintf(buf, cap, pct ? "%.*s%%" : "%.*spx", (int)i, s);
    return w > 0;
}

static bool is_color_legacy(const char *s, uint32_t len, char *buf, uint32_t cap)
{
    if (len == 0) {
        return false;
    }
    if (s[0] == '#') {
        int w = SDL_snprintf(buf, cap, "%.*s", (int)len, s);
        return w > 0;
    }
    /* A bare six-digit hex is a legacy color. */
    bool hex = len == 6;
    for (uint32_t i = 0; hex && i < len; i++) {
        char c = s[i];
        hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }
    if (hex) {
        int w = SDL_snprintf(buf, cap, "#%.*s", (int)len, s);
        return w > 0;
    }
    int w = SDL_snprintf(buf, cap, "%.*s", (int)len, s);
    return w > 0;
}

static const char *font_size_for(uint32_t n)
{
    switch (n) {
    case 1: return "x-small";
    case 2: return "small";
    case 3: return "medium";
    case 4: return "large";
    case 5: return "x-large";
    case 6: return "xx-large";
    case 7: return "xxx-large";
    default: return nullptr;
    }
}

/* The padding that the cellpadding attribute of the nearest table ancestor gives its cells, as a
 * "<n>px" length. False when there is no table ancestor or it has no such attribute. */
static bool table_cellpadding(embi_element *el, char *buf, uint32_t cap)
{
    for (embi_node *n = el->base.parent; n != nullptr && embi_is_element(n); n = n->parent) {
        embi_element *a = embi_as_element(n);
        if (a->base.extra != EMBI_NS_HTML ||
            !embi_str_ieq(embi_atom_str(a->base.name), EMBI_STR("table"))) {
            continue;
        }
        const embi_attr *pa = embi_el_get_attribute(a, EMBI_STR("cellpadding"));
        if (pa == nullptr) {
            return false;
        }
        size_t vcap = 0;
        char *vbuf = nullptr;
        embi_str v = embi_style_attr_utf8(&pa->value, &vcap, &vbuf);
        bool ok = legacy_length(v.ptr, v.len, buf, cap);
        embi_free(vbuf, vcap);
        return ok;
    }
    return false;
}

/* SVG presentation attributes (SVG 2 section 6.2, R4c): each maps to the property of the same name at author level 0, below
 * every author rule. The `transform` attribute is not here: its syntax is not CSS's, so the render tree reads it (the outermost svg, which
 * is a CSS box, gets it written out as CSS below). */
static const char *const svg_pres_attrs[] = {
    "alignment-baseline", "baseline-shift", "clip-path", "clip-rule", "color", "color-interpolation", "direction",
    "display", "dominant-baseline", "fill", "fill-opacity", "fill-rule", "filter", "font-family", "font-size",
    "font-stretch", "font-style", "font-variant", "font-weight", "image-rendering", "letter-spacing", "marker-end",
    "marker-mid", "marker-start", "mask", "opacity", "overflow", "paint-order", "pointer-events", "shape-rendering",
    "stop-color", "stop-opacity", "stroke", "stroke-dasharray", "stroke-dashoffset", "stroke-linecap",
    "stroke-linejoin", "stroke-miterlimit", "stroke-opacity", "stroke-width", "text-anchor", "text-decoration",
    "text-rendering", "transform-origin", "transform-box", "unicode-bidi", "vector-effect", "visibility", "word-spacing",
    "writing-mode", "mask-type",
};

/* The transform attribute of an outermost svg is a presentation attribute of the CSS box (SVG 2 section 8.5): the SVG list
 * syntax (unitless numbers, rotate(a cx cy), commas) is written out as CSS. Returns false when the list is not valid. */
static bool svg_ws_comma(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == ',';
}

static bool svg_transform_to_css(const char *s, size_t n, char *out, size_t cap)
{
    size_t w = 0;
    const char *p = s, *end = s + n;
#define PUT(...) do { int r_ = snprintf(out + w, cap - w, __VA_ARGS__); if (r_ < 0 || (size_t)r_ >= cap - w) return false; w += (size_t)r_; } while (0)
    while (p < end) {
        while (p < end && svg_ws_comma(*p)) {
            p++;
        }
        if (p >= end) {
            break;
        }
        const char *nm = p;
        while (p < end && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))) {
            p++;
        }
        size_t nl = (size_t)(p - nm);
        while (p < end && *p == ' ') {
            p++;
        }
        if (nl == 0 || p >= end || *p != '(') {
            return false;
        }
        p++;
        double v[6];
        int nv = 0;
        for (;;) {
            while (p < end && svg_ws_comma(*p)) {
                p++;
            }
            if (p < end && *p == ')') {
                p++;
                break;
            }
            if (p >= end || nv >= 6) {
                return false;
            }
            char tmp[48];
            size_t k = 0;
            while (p < end && k + 1 < sizeof tmp && (*p == '+' || *p == '-' || *p == '.' || *p == 'e' || *p == 'E' || (*p >= '0' && *p <= '9'))) {
                tmp[k++] = *p++;
            }
            tmp[k] = 0;
            char *ep;
            v[nv] = strtod(tmp, &ep);
            if (k == 0 || *ep != 0) {
                return false;
            }
            nv++;
        }
        if (nl == 6 && memcmp(nm, "matrix", 6) == 0 && nv == 6) {
            PUT("matrix(%.9g,%.9g,%.9g,%.9g,%.9g,%.9g) ", v[0], v[1], v[2], v[3], v[4], v[5]);
        } else if (nl == 9 && memcmp(nm, "translate", 9) == 0 && (nv == 1 || nv == 2)) {
            PUT("translate(%.9gpx,%.9gpx) ", v[0], nv == 2 ? v[1] : 0.0);
        } else if (nl == 5 && memcmp(nm, "scale", 5) == 0 && (nv == 1 || nv == 2)) {
            PUT("scale(%.9g,%.9g) ", v[0], nv == 2 ? v[1] : v[0]);
        } else if (nl == 6 && memcmp(nm, "rotate", 6) == 0 && (nv == 1 || nv == 3)) {
            if (nv == 3) {
                PUT("translate(%.9gpx,%.9gpx) rotate(%.9gdeg) translate(%.9gpx,%.9gpx) ", v[1], v[2], v[0], -v[1], -v[2]);
            } else {
                PUT("rotate(%.9gdeg) ", v[0]);
            }
        } else if (nl == 5 && memcmp(nm, "skewX", 5) == 0 && nv == 1) {
            PUT("skewX(%.9gdeg) ", v[0]);
        } else if (nl == 5 && memcmp(nm, "skewY", 5) == 0 && nv == 1) {
            PUT("skewY(%.9gdeg) ", v[0]);
        } else {
            return false;
        }
    }
#undef PUT
    return w > 0;
}

static void svg_hints(embi_style_engine *e, embi_sctx *sc, embi_element *el)
{
    embi_str ln = embi_atom_str(el->base.name);
    bool outer_svg = ln.len == 3 && memcmp(ln.ptr, "svg", 3) == 0 &&
                     (el->base.parent == nullptr || !embi_is_element(el->base.parent) ||
                      ((const embi_element *)el->base.parent)->base.extra != EMBI_NS_SVG);
    uint32_t na = embi_el_attr_count(el);
    for (uint32_t i = 0; i < na; i++) {
        const embi_attr *a = embi_el_attr_at(el, i);
        if (a->ns != EMBI_NS_NULL) {
            continue;
        }
        embi_str an = embi_atom_str(a->name);
        char name[32];
        if (an.len < 4 || an.len >= sizeof name) {
            continue;
        }
        memcpy(name, an.ptr, an.len);
        name[an.len] = 0;
        if (outer_svg && strcmp(name, "transform") == 0) {
            size_t tcap = 0;
            char *tbuf = nullptr;
            embi_str tv = embi_style_attr_utf8(&a->value, &tcap, &tbuf);
            char css[600];
            if (tv.len > 0 && tv.len < 300 && svg_transform_to_css(tv.ptr, tv.len, css, sizeof css)) {
                add_hint(e, sc, "transform", css);
            }
            embi_free(tbuf, tcap);
            continue;
        }
        if (!tag_in(name, svg_pres_attrs, sizeof svg_pres_attrs / sizeof svg_pres_attrs[0])) {
            continue;
        }
        size_t cap = 0;
        char *vbuf = nullptr;
        embi_str v = embi_style_attr_utf8(&a->value, &cap, &vbuf);
        if (v.len > 0 && v.len < 300) {
            char val[304];
            memcpy(val, v.ptr, v.len);
            val[v.len] = 0;
            add_hint(e, sc, name, val);
        }
        embi_free(vbuf, cap);
    }
}

void embi_style_gather_hints(embi_style_engine *e, embi_sctx *sc, embi_element *el)
{
    if (el->base.extra == EMBI_NS_SVG) {
        if (el->attrs != nullptr) {
            svg_hints(e, sc, el);
        }
        return;
    }
    if (el->base.extra != EMBI_NS_HTML) {
        return;
    }
    embi_str tag = embi_atom_str(el->base.name);
    char t[32];
    if (tag.len == 0 || tag.len >= sizeof t) {
        return;
    }
    memcpy(t, tag.ptr, tag.len);
    t[tag.len] = '\0';
    if (strcmp(t, "center") == 0) {
        add_hint(e, sc, "text-align", "center");
    }
    /* cellpadding of the nearest table applies to its cells (W3g: a cell reads its table's attribute). */
    if (strcmp(t, "td") == 0 || strcmp(t, "th") == 0) {
        char pad[96];
        if (table_cellpadding(el, pad, sizeof pad)) {
            add_hint(e, sc, "padding", pad);
        }
    }
    if (el->attrs == nullptr) {
        return;
    }

    static const char *const cell_or_table[] = {"body", "table", "thead", "tbody", "tfoot",
                                                "tr", "td", "th"};
    static const char *const sized[] = {"img", "table", "td", "th", "col", "colgroup", "hr",
                                        "iframe", "video", "embed", "object"};
    static const char *const aligned[] = {"div", "p", "h1", "h2", "h3", "h4", "h5", "h6",
                                          "caption", "td", "th", "thead", "tbody", "tfoot", "tr",
                                          "hr", "table", "col", "colgroup"};
    static const char *const valigned[] = {"td", "th", "tr", "thead", "tbody", "tfoot", "col",
                                           "colgroup"};
    static const char *const cells[] = {"td", "th"};
    static const char *const tables[] = {"table"};

    uint32_t na = embi_el_attr_count(el);
    for (uint32_t i = 0; i < na; i++) {
        const embi_attr *a = embi_el_attr_at(el, i);
        if (a->ns != EMBI_NS_NULL) {
            continue;
        }
        embi_str an = embi_atom_str(a->name);
        char name[32];
        if (an.len == 0 || an.len >= sizeof name) {
            continue;
        }
        memcpy(name, an.ptr, an.len);
        name[an.len] = '\0';

        /* Only the attributes that some hint reads are converted (most attributes are not). */
        static const char *const hint_attrs[] = {
            "bgcolor", "text", "width", "height", "align", "valign", "border", "nowrap", "dir",
            "color", "face", "size", "cellspacing",
        };
        if (!tag_in(name, hint_attrs, sizeof hint_attrs / sizeof hint_attrs[0])) {
            continue;
        }
        size_t cap = 0;
        char *vbuf = nullptr;
        embi_str v = embi_style_attr_utf8(&a->value, &cap, &vbuf);
        char out[96];
        if (strcmp(name, "bgcolor") == 0 && tag_in(t, cell_or_table, 8) &&
            is_color_legacy(v.ptr, v.len, out, sizeof out)) {
            add_hint(e, sc, "background-color", out);
        } else if (strcmp(name, "text") == 0 && strcmp(t, "body") == 0 &&
                   is_color_legacy(v.ptr, v.len, out, sizeof out)) {
            add_hint(e, sc, "color", out);
        } else if ((strcmp(name, "width") == 0 || strcmp(name, "height") == 0) &&
                   tag_in(t, sized, sizeof sized / sizeof sized[0]) &&
                   legacy_length(v.ptr, v.len, out, sizeof out)) {
            add_hint(e, sc, strcmp(name, "width") == 0 ? "width" : "height", out);
        } else if (strcmp(name, "align") == 0 && tag_in(t, aligned, sizeof aligned / sizeof aligned[0])) {
            if (embi_str_ieq(v, EMBI_STR("center")) && tag_in(t, tables, 1)) {
                add_hint(e, sc, "margin-left", "auto");
                add_hint(e, sc, "margin-right", "auto");
            } else if (embi_str_ieq(v, EMBI_STR("left")) || embi_str_ieq(v, EMBI_STR("right"))) {
                if (tag_in(t, tables, 1)) {
                    add_hint(e, sc, "float", embi_str_ieq(v, EMBI_STR("left")) ? "left" : "right");
                } else {
                    add_hint(e, sc, "text-align", embi_str_ieq(v, EMBI_STR("left")) ? "left" : "right");
                }
            } else if (embi_str_ieq(v, EMBI_STR("center")) || embi_str_ieq(v, EMBI_STR("justify"))) {
                add_hint(e, sc, "text-align", embi_str_ieq(v, EMBI_STR("center")) ? "center" : "justify");
            }
        } else if (strcmp(name, "valign") == 0 && tag_in(t, valigned, sizeof valigned / sizeof valigned[0])) {
            if (embi_str_ieq(v, EMBI_STR("top")) || embi_str_ieq(v, EMBI_STR("middle")) ||
                embi_str_ieq(v, EMBI_STR("bottom")) || embi_str_ieq(v, EMBI_STR("baseline"))) {
                char kw[16];
                int w = SDL_snprintf(kw, sizeof kw, "%.*s", (int)v.len, v.ptr);
                if (w > 0) {
                    for (int k = 0; k < w; k++) {
                        if (kw[k] >= 'A' && kw[k] <= 'Z') {
                            kw[k] = (char)(kw[k] - 'A' + 'a');
                        }
                    }
                    add_hint(e, sc, "vertical-align", kw);
                }
            }
        } else if (strcmp(name, "border") == 0 && strcmp(t, "table") == 0 &&
                   legacy_length(v.ptr, v.len, out, sizeof out)) {
            add_hint(e, sc, "border-width", out);
            add_hint(e, sc, "border-style", "solid");
        } else if (strcmp(name, "cellspacing") == 0 && strcmp(t, "table") == 0 &&
                   legacy_length(v.ptr, v.len, out, sizeof out)) {
            add_hint(e, sc, "border-spacing", out);
        } else if (strcmp(name, "nowrap") == 0 && tag_in(t, cells, 2)) {
            add_hint(e, sc, "white-space", "nowrap");
        } else if (strcmp(name, "dir") == 0 &&
                   (embi_str_ieq(v, EMBI_STR("ltr")) || embi_str_ieq(v, EMBI_STR("rtl")))) {
            add_hint(e, sc, "direction", embi_str_ieq(v, EMBI_STR("ltr")) ? "ltr" : "rtl");
        } else if (strcmp(t, "font") == 0) {
            if (strcmp(name, "color") == 0 && is_color_legacy(v.ptr, v.len, out, sizeof out)) {
                add_hint(e, sc, "color", out);
            } else if (strcmp(name, "face") == 0 && v.len > 0 && v.len < sizeof out) {
                int w = SDL_snprintf(out, sizeof out, "%.*s", (int)v.len, v.ptr);
                if (w > 0) {
                    add_hint(e, sc, "font-family", out);
                }
            } else if (strcmp(name, "size") == 0 && v.len == 1 && v.ptr[0] >= '1' && v.ptr[0] <= '7') {
                const char *kw = font_size_for((uint32_t)(v.ptr[0] - '0'));
                if (kw != nullptr) {
                    add_hint(e, sc, "font-size", kw);
                }
            }
        }
        embi_free(vbuf, cap);
    }
}
