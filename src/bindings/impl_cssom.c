/*
 * CSSStyleDeclaration (cssom.webidl): element.style (the declarations of the style attribute,
 * read and written through it, so a write marks the document dirty like any attribute change) and
 * the read-only declaration of getComputedStyle.
 *
 * The style attribute is parsed with the CSS Syntax tokenizer, so strings, balanced parentheses
 * and !important are handled exactly like the stylesheet parser. A declaration is kept as the
 * property name and the value text as written (shorthands stay shorthands: reading a longhand
 * that was only set through its shorthand answers the empty string).
 */
#include "bindings/impl.h"
#include "bindings/gen/cssom.h"
#include "bindings/gen/htmlcanvas.h"
#include "css/css.h"
#include "css/props.h"
#include "style/style.h"

#define STYLE_ATTR EMBI_STR("style")
enum { VIEW_INLINE = 0, VIEW_COMPUTED = 1 };

typedef struct decl {
    embi_str name;
    embi_str value;
    bool important;
} decl;

typedef struct decls {
    decl *items;
    uint32_t n;
} decls;

embi_js_value embi_impl_style_inline(embi_bind_ctx *c, embi_bind_obj *el)
{
    return embi_bind_view_new(c, EMBI_IF_CSSStyleDeclaration, el, VIEW_INLINE, nullptr, 0);
}

embi_js_value embi_impl_style_computed(embi_bind_ctx *c, embi_bind_obj *el)
{
    return embi_bind_view_new(c, EMBI_IF_CSSStyleDeclaration, el, VIEW_COMPUTED, nullptr, 0);
}

static embi_element *el_of(embi_bind_obj *self)
{
    return (embi_element *)embi_bind_owner_node(self);
}

static bool is_ws(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f';
}

static embi_str trim(embi_str s)
{
    uint32_t a = 0, b = s.len;
    while (a < b && is_ws(s.ptr[a])) {
        a++;
    }
    while (b > a && is_ws(s.ptr[b - 1])) {
        b--;
    }
    return (embi_str){s.ptr + a, b - a};
}

static bool is_custom(embi_str name)
{
    return name.len >= 2 && name.ptr[0] == '-' && name.ptr[1] == '-';
}

/* The property name in canonical form: ASCII lowercase, except for custom properties. */
static embi_str canon_name(embi_bind_ctx *c, embi_str name)
{
    name = trim(name);
    if (is_custom(name)) {
        return name;
    }
    char *buf = embi_bind_alloc(c, (size_t)name.len + 1);
    for (uint32_t i = 0; i < name.len; i++) {
        char ch = name.ptr[i];
        buf[i] = (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
    }
    return (embi_str){buf, name.len};
}

static char block_closer(uint8_t t)
{
    switch (t) {
    case EMBI_CSS_LBRACE: return '}';
    case EMBI_CSS_LBRACKET: return ']';
    default: return ')'; /* LPAREN and FUNCTION */
    }
}

static bool custom_cvs_ok(embi_str text, embi_css_slice v, bool top)
{
    for (uint32_t i = 0; i < v.n; i++) {
        const embi_css_cv *cv = &v.p[i];
        switch (cv->tok.type) {
        case EMBI_CSS_SEMICOLON:
            if (top) {
                return false;
            }
            break;
        case EMBI_CSS_RBRACE:
        case EMBI_CSS_RPAREN:
        case EMBI_CSS_RBRACKET:
        case EMBI_CSS_BAD_STRING:
        case EMBI_CSS_BAD_URL:
            return false;
        case EMBI_CSS_STRING:
        case EMBI_CSS_URL:
            if (cv->tok.flag) { /* ended by the end of the text */
                return false;
            }
            break;
        case EMBI_CSS_DELIM:
            if (top && cv->tok.cp == '!') {
                return false;
            }
            break;
        case EMBI_CSS_LBRACE:
        case EMBI_CSS_LBRACKET:
        case EMBI_CSS_LPAREN:
        case EMBI_CSS_FUNCTION:
            /* a block left open at the end of the text would swallow what follows it */
            if (cv->end == 0 || cv->end > text.len || text.ptr[cv->end - 1] != block_closer(cv->tok.type)) {
                return false;
            }
            if (!custom_cvs_ok(text, embi_css_slice_make(cv->kids, cv->nkids), false)) {
                return false;
            }
            break;
        default:
            break;
        }
    }
    return true;
}

/* A custom property value must not hold a top-level semicolon or !, unmatched or unclosed
 * brackets, or bad strings and urls: written into the style attribute it would split into other
 * declarations. */
static bool custom_value_ok(embi_bind_ctx *c, embi_str value)
{
    embi_css_slice v = embi_css_parse_cvs(&c->arena, value);
    return custom_cvs_ok(value, v, true);
}

/* Parses declaration text. The text is preprocessed like the CSS input stream, so that the token
 * offsets refer to the copy this function makes. */
static decls parse_decls(embi_bind_ctx *c, embi_str src)
{
    char *text = embi_bind_alloc(c, (size_t)src.len * 3 + 1);
    uint32_t n = 0;
    for (uint32_t i = 0; i < src.len; i++) {
        char ch = src.ptr[i];
        if (ch == '\r') {
            text[n++] = '\n';
            if (i + 1 < src.len && src.ptr[i + 1] == '\n') {
                i++;
            }
        } else if (ch == '\f') {
            text[n++] = '\n';
        } else if (ch == '\0') {
            text[n++] = (char)0xEF;
            text[n++] = (char)0xBF;
            text[n++] = (char)0xBD; /* U+FFFD */
        } else {
            text[n++] = ch;
        }
    }
    decls out = {nullptr, 0};
    embi_css_slice cvs = embi_css_parse_cvs(&c->arena, (embi_str){text, n});
    embi_css_items items = embi_css_consume_declarations(&c->arena, cvs);
    out.items = (decl *)embi_bind_alloc(c, (size_t)(items.n + 1) * sizeof(decl));
    for (uint32_t i = 0; i < items.n; i++) {
        const embi_css_item *it = &items.p[i];
        if (it->kind != EMBI_CSS_ITEM_DECL) {
            continue;
        }
        embi_str value = {"", 0};
        if (it->prelude.n) {
            const embi_css_cv *first = &it->prelude.p[0];
            const embi_css_cv *last = &it->prelude.p[it->prelude.n - 1];
            uint32_t s = first->tok.start;
            uint32_t e = last->end > last->tok.end ? last->end : last->tok.end;
            if (s <= e && e <= n) {
                value = trim((embi_str){text + s, e - s});
            }
        }
        decl d = {canon_name(c, it->name), value, it->important};
        /* a later declaration of the same property replaces the earlier one */
        bool replaced = false;
        for (uint32_t k = 0; k < out.n; k++) {
            if (embi_str_eq(out.items[k].name, d.name)) {
                out.items[k].value = d.value;
                out.items[k].important = d.important;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            out.items[out.n++] = d;
        }
    }
    return out;
}

/* The parsed declarations of the last style attribute value seen, kept on the context in one heap
 * block: [decl items][key text][name and value strings]. A read whose attribute text equals the
 * key skips the tokenizer, and a write primes the cache with what it wrote, so a series of
 * property writes parses the attribute once. */
typedef struct style_cache {
    size_t size;
    uint32_t n;
    uint32_t key_len;
    decl *items;
    char *key;
} style_cache;

void embi_impl_style_cache_free(embi_bind_ctx *c)
{
    style_cache *sc = c->style_cache;
    if (sc) {
        embi_free(sc, sc->size);
        c->style_cache = nullptr;
    }
}

static void cache_fill(embi_bind_ctx *c, embi_str key, const decls *d)
{
    size_t total = sizeof(style_cache) + (size_t)(d->n + 1) * sizeof(decl) + key.len + 1;
    for (uint32_t i = 0; i < d->n; i++) {
        total += (size_t)d->items[i].name.len + d->items[i].value.len;
    }
    total = (total + 7) & ~(size_t)7;
    style_cache *sc = embi_alloc(total);
    sc->size = total;
    sc->n = d->n;
    sc->key_len = key.len;
    sc->items = (decl *)(sc + 1);
    char *p = (char *)(sc->items + d->n + 1);
    sc->key = p;
    if (key.len) {
        SDL_memcpy(p, key.ptr, key.len);
    }
    p += key.len + 1;
    for (uint32_t i = 0; i < d->n; i++) {
        decl x = d->items[i];
        if (x.name.len) {
            SDL_memcpy(p, x.name.ptr, x.name.len);
        }
        x.name.ptr = p;
        p += x.name.len;
        if (x.value.len) {
            SDL_memcpy(p, x.value.ptr, x.value.len);
        }
        x.value.ptr = p;
        p += x.value.len;
        sc->items[i] = x;
    }
    /* d and key may point into the previous block */
    embi_impl_style_cache_free(c);
    c->style_cache = sc;
}

static decls inline_decls(embi_bind_ctx *c, embi_bind_obj *self)
{
    embi_element *el = el_of(self);
    if (!el) {
        return (decls){nullptr, 0};
    }
    const embi_attr *a = embi_el_get_attribute(el, STYLE_ATTR);
    if (!a || a->value.len == 0) {
        return (decls){nullptr, 0};
    }
    embi_str text = embi_bind_utf8(c, a->value);
    const style_cache *sc = c->style_cache;
    if (sc && sc->key_len == text.len && (text.len == 0 || SDL_memcmp(sc->key, text.ptr, text.len) == 0)) {
        /* a copy of the item array: callers edit it in place */
        decl *items = (decl *)embi_bind_alloc(c, (size_t)(sc->n + 1) * sizeof(decl));
        if (sc->n) {
            SDL_memcpy(items, sc->items, (size_t)sc->n * sizeof(decl));
        }
        return (decls){items, sc->n};
    }
    decls d = parse_decls(c, text);
    cache_fill(c, text, &d);
    return d;
}

static void write_decls(embi_bind_ctx *c, embi_bind_obj *self, const decls *d)
{
    embi_element *el = el_of(self);
    if (!el) {
        return;
    }
    embi_sb sb = {0};
    for (uint32_t i = 0; i < d->n; i++) {
        if (i) {
            embi_sb_append_char(&sb, ' ');
        }
        embi_sb_append(&sb, d->items[i].name);
        embi_sb_append(&sb, EMBI_STR(": "));
        embi_sb_append(&sb, d->items[i].value);
        if (d->items[i].important) {
            embi_sb_append(&sb, EMBI_STR(" !important"));
        }
        embi_sb_append_char(&sb, ';');
    }
    if (sb.len == 0 && !embi_el_has_attribute(el, STYLE_ATTR)) {
        embi_sb_free(&sb);
        return;
    }
    embi_dstr v = sb.len ? embi_bind_tmp_utf8(c, (embi_str){sb.data, sb.len}) : embi_dstr_empty();
    (void)embi_el_set_attribute(el, STYLE_ATTR, v);
    if (sb.len) {
        /* prime the cache with what was written (re-reading the text gives the same declarations) */
        cache_fill(c, (embi_str){sb.data, sb.len}, d);
    }
    embi_sb_free(&sb);
}

static int find_decl(const decls *d, embi_str name)
{
    for (uint32_t i = 0; i < d->n; i++) {
        if (embi_str_eq(d->items[i].name, name)) {
            return (int)i;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ computed values */

static embi_dstr computed_value(embi_bind_ctx *c, embi_bind_obj *self, embi_str name)
{
    embi_style_engine *se = embi_bind_style(c);
    embi_element *el = el_of(self);
    if (!se || !el) {
        return embi_dstr_empty();
    }
    embi_style_resolve_document(se, el->base.doc);
    const embi_style *s = embi_style_get(se, el, EMBI_PE_NONE);
    if (!s) {
        return embi_dstr_empty();
    }
    embi_sb sb = {0};
    bool ok;
    if (is_custom(name)) {
        ok = embi_style_custom_text(s, embi_atom_intern(name.ptr, name.len), &sb);
    } else {
        embi_prop_id id = embi_prop_lookup(name);
        ok = id != 0 && (embi_style_value_text(s, id, &sb) || embi_impl_computed_shorthand(s, name, &sb));
    }
    /* R4b: a canvas is a replaced element whose automatic size is its bitmap size, so the resolved width and height are
     * that size in px whatever the layout engine has done (the used value of an element with a box). */
#if EMBER_CANVAS
    if (ok && el->base.extra == EMBI_NS_HTML && el->base.name == EMBI_ATOM_canvas && sb.len == 4 &&
        SDL_memcmp(sb.data, "auto", 4) == 0 && (embi_str_eq(name, EMBI_STR("width")) || embi_str_eq(name, EMBI_STR("height")))) {
        uint32_t px = name.ptr[0] == 'w' ? embi_impl_HTMLCanvasElement_get_width(c, &el->base)
                                         : embi_impl_HTMLCanvasElement_get_height(c, &el->base);
        sb.len = 0;
        embi_sb_append_u64(&sb, px);
        embi_sb_append(&sb, EMBI_STR("px"));
    }
#endif
    embi_dstr d = ok && sb.len ? embi_bind_tmp_utf8(c, (embi_str){sb.data, sb.len}) : embi_dstr_empty();
    embi_sb_free(&sb);
    return d;
}

/* ------------------------------------------------------------------ the interface */

embi_dstr embi_impl_CSSStyleDeclaration_get_cssText(embi_bind_ctx *c, embi_bind_obj *self)
{
    if (self->aux == VIEW_COMPUTED) {
        return embi_dstr_empty();
    }
    decls d = inline_decls(c, self);
    embi_sb sb = {0};
    for (uint32_t i = 0; i < d.n; i++) {
        if (i) {
            embi_sb_append_char(&sb, ' ');
        }
        embi_sb_append(&sb, d.items[i].name);
        embi_sb_append(&sb, EMBI_STR(": "));
        embi_sb_append(&sb, d.items[i].value);
        if (d.items[i].important) {
            embi_sb_append(&sb, EMBI_STR(" !important"));
        }
        embi_sb_append_char(&sb, ';');
    }
    embi_dstr r = sb.len ? embi_bind_tmp_utf8(c, (embi_str){sb.data, sb.len}) : embi_dstr_empty();
    embi_sb_free(&sb);
    return r;
}

void embi_impl_CSSStyleDeclaration_set_cssText(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    if (self->aux == VIEW_COMPUTED) {
        return;
    }
    /* keep the valid declarations only, in their canonical form */
    decls d = parse_decls(c, embi_bind_utf8(c, a0));
    decls ok = {(decl *)embi_bind_alloc(c, (size_t)(d.n + 1) * sizeof(decl)), 0};
    for (uint32_t i = 0; i < d.n; i++) {
        embi_css_slice v = embi_css_parse_cvs(&c->arena, d.items[i].value);
        if (is_custom(d.items[i].name) ? custom_value_ok(c, d.items[i].value) : embi_prop_decl_ok(nullptr, d.items[i].name, v, d.items[i].important)) {
            ok.items[ok.n++] = d.items[i];
        }
    }
    write_decls(c, self, &ok);
}

uint32_t embi_impl_CSSStyleDeclaration_get_length(embi_bind_ctx *c, embi_bind_obj *self)
{
    if (self->aux == VIEW_COMPUTED) {
        return (uint32_t)embi_css_prop_count;
    }
    return inline_decls(c, self).n;
}

embi_js_value embi_impl_CSSStyleDeclaration_item(embi_bind_ctx *c, embi_bind_obj *self, uint32_t a0)
{
    if (self->aux == VIEW_COMPUTED) {
        if (a0 >= (uint32_t)embi_css_prop_count) {
            return EMBI_JS_UNDEFINED;
        }
        return embi_bind_utf8_value(c, (embi_str){embi_css_prop_names[a0], (uint32_t)SDL_strlen(embi_css_prop_names[a0])});
    }
    decls d = inline_decls(c, self);
    return a0 < d.n ? embi_bind_utf8_value(c, d.items[a0].name) : EMBI_JS_UNDEFINED;
}

embi_dstr embi_impl_CSSStyleDeclaration_getPropertyValue(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    embi_str name = canon_name(c, embi_bind_utf8(c, a0));
    if (self->aux == VIEW_COMPUTED) {
        return computed_value(c, self, name);
    }
    decls d = inline_decls(c, self);
    int at = find_decl(&d, name);
    return at >= 0 ? embi_bind_tmp_utf8(c, d.items[at].value) : embi_dstr_empty();
}

embi_dstr embi_impl_CSSStyleDeclaration_getPropertyPriority(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    if (self->aux == VIEW_COMPUTED) {
        return embi_dstr_empty();
    }
    embi_str name = canon_name(c, embi_bind_utf8(c, a0));
    decls d = inline_decls(c, self);
    int at = find_decl(&d, name);
    return (at >= 0 && d.items[at].important) ? embi_impl_lit("important") : embi_dstr_empty();
}

static void set_property(embi_bind_ctx *c, embi_bind_obj *self, embi_str name, embi_str value, bool important)
{
    name = canon_name(c, name);
    if (name.len == 0) {
        return;
    }
    decls d = inline_decls(c, self);
    int at = find_decl(&d, name);
    value = trim(value);
    if (value.len == 0) {
        if (at >= 0) {
            for (uint32_t i = (uint32_t)at; i + 1 < d.n; i++) {
                d.items[i] = d.items[i + 1];
            }
            d.n--;
            write_decls(c, self, &d);
        }
        return;
    }
    embi_css_slice v = embi_css_parse_cvs(&c->arena, value);
    if (is_custom(name) ? !custom_value_ok(c, value) : !embi_prop_decl_ok(nullptr, name, v, important)) {
        return;
    }
    decls nd = {(decl *)embi_bind_alloc(c, (size_t)(d.n + 1) * sizeof(decl)), d.n};
    for (uint32_t i = 0; i < d.n; i++) {
        nd.items[i] = d.items[i];
    }
    decl nw = {name, value, important};
    if (at >= 0) {
        nd.items[at] = nw;
    } else {
        nd.items[nd.n++] = nw;
    }
    write_decls(c, self, &nd);
}

void embi_impl_CSSStyleDeclaration_setProperty(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0,
                                               embi_dstr a1, embi_dstr a2)
{
    if (self->aux == VIEW_COMPUTED) {
        return;
    }
    embi_str prio = embi_bind_utf8(c, a2);
    bool important = false;
    if (prio.len) {
        if (!embi_str_ieq(prio, EMBI_STR("important"))) {
            return;
        }
        important = true;
    }
    set_property(c, self, embi_bind_utf8(c, a0), embi_bind_utf8(c, a1), important);
}

embi_dstr embi_impl_CSSStyleDeclaration_removeProperty(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    if (self->aux == VIEW_COMPUTED) {
        return embi_dstr_empty();
    }
    embi_str name = canon_name(c, embi_bind_utf8(c, a0));
    decls d = inline_decls(c, self);
    int at = find_decl(&d, name);
    if (at < 0) {
        return embi_dstr_empty();
    }
    embi_dstr old = embi_bind_tmp_utf8(c, d.items[at].value);
    for (uint32_t i = (uint32_t)at; i + 1 < d.n; i++) {
        d.items[i] = d.items[i + 1];
    }
    d.n--;
    write_decls(c, self, &d);
    return old;
}

embi_dstr embi_impl_CSSStyleDeclaration_cssprop_get(embi_bind_ctx *c, embi_bind_obj *self, int idx)
{
    embi_str name = {embi_css_prop_names[idx], (uint32_t)SDL_strlen(embi_css_prop_names[idx])};
    if (self->aux == VIEW_COMPUTED) {
        return computed_value(c, self, name);
    }
    decls d = inline_decls(c, self);
    int at = find_decl(&d, name);
    return at >= 0 ? embi_bind_tmp_utf8(c, d.items[at].value) : embi_dstr_empty();
}

void embi_impl_CSSStyleDeclaration_cssprop_set(embi_bind_ctx *c, embi_bind_obj *self, int idx, embi_dstr v)
{
    if (self->aux == VIEW_COMPUTED) {
        return;
    }
    embi_str name = {embi_css_prop_names[idx], (uint32_t)SDL_strlen(embi_css_prop_names[idx])};
    set_property(c, self, name, embi_bind_utf8(c, v), false);
}
