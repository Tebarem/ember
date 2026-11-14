#pragma once
/*
 * Shared setup for the layout unit tests: parses a document with its author CSS, resolves styles,
 * and lays it out at the default 800x600 viewport. Not a test itself (the name has no test_
 * prefix, so CMake does not build it as one).
 */
#include "emb_test.h"

#include "css/css.h"
#include "css/props.h"
#include "dom/dom.h"
#include "html/parser.h"
#include "layout/layout.h"
#include "layout/layout_int.h"
#include "layout/text_shim.h"
#include "style/style.h"

#include <SDL3/SDL_stdinc.h>
#include <string.h>

typedef struct lh {
    embi_text *text;
    embi_style_engine *style;
    embi_layout_engine *eng;
    embi_document *doc;
    embi_css_sheet *sheet;
} lh;

/* The bodies of the <style> blocks of html (n bytes), joined, as a NUL-terminated SDL_malloc
 * string (free with SDL_free). Extra CSS goes after them. */
/* Set by a test that wants the UA sheet's own body margin (8px) left in place. */
static bool lh_keep_ua_body_margin;

static inline char *lh_join_styles(const char *html, size_t n, const char *extra)
{
    /* The UA sheet gives body an 8px margin (as browsers do); these unit tests assert coordinates from
     * the viewport origin, so a first author rule zeroes it (a test's own body rule still wins). */
    static const char reset[] = "body{margin:0}\n";
    size_t cap = n + strlen(extra) + sizeof reset + 2, len = 0;
    char *out = SDL_malloc(cap);
    if (!lh_keep_ua_body_margin) {
        memcpy(out, reset, sizeof reset - 1);
        len = sizeof reset - 1;
    }
    const char *p = html;
    const char *end = html + n;
    while (p < end) {
        const char *open = SDL_strcasestr(p, "<style");
        if (open == nullptr || open >= end) {
            break;
        }
        const char *body = SDL_strchr(open, '>');
        if (body == nullptr || body >= end) {
            break;
        }
        body++;
        const char *close = SDL_strcasestr(body, "</style>");
        size_t blen = close != nullptr ? (size_t)(close - body) : (size_t)(end - body);
        memcpy(out + len, body, blen);
        len += blen;
        out[len++] = '\n';
        p = close != nullptr ? close + 8 : end;
    }
    size_t el = strlen(extra);
    memcpy(out + len, extra, el);
    len += el;
    out[len] = '\0';
    return out;
}

/* Parses html (n bytes) with its own <style> blocks followed by css, resolves styles and lays the
 * document out with the layout config cfg (nullptr for the defaults). */
static inline lh lh_make_cfg(const char *html, size_t n, const char *css,
                             const embi_layout_config *cfg)
{
    lh h;
    memset(&h, 0, sizeof h);
    h.text = embi_tshim_create();
    h.style = embi_style_engine_create(embi_tshim_fonts(h.text), nullptr, nullptr);
    h.doc = embi_document_new(true);
    embi_html_parser *p = embi_html_parser_new(h.doc, nullptr);
    embi_html_parser_write(p, html, n);
    embi_html_parser_end(p);
    embi_html_parser_destroy(p);
    char *all = lh_join_styles(html, n, css);
    embi_css_hooks hooks = {nullptr, embi_prop_decl_ok, nullptr};
    embi_css_parse_opts opts = {nullptr, &hooks, EMBI_STR("")};
    h.sheet = embi_css_sheet_parse((embi_str){all, (uint32_t)strlen(all)}, &opts);
    SDL_free(all);
    embi_css_sheet *sheets[1] = {h.sheet};
    embi_document_set_author_sheets(h.style, h.doc, sheets, 1);
    embi_style_resolve_document(h.style, h.doc);
    h.eng = embi_layout_engine_create(h.text, h.style, cfg);
    (void)embi_layout_document(h.eng, h.doc);
    return h;
}

static inline lh lh_make_n(const char *html, size_t n, const char *css)
{
    return lh_make_cfg(html, n, css, nullptr);
}

static inline lh lh_make(const char *html, const char *css)
{
    return lh_make_n(html, strlen(html), css);
}

/* Re-creates the layout engine with the given scrollbar thickness, and lays the document out. */
/* Sets the scroll offset of a fragment of the current layout (the offset lives in the side record). */
static inline void lh_scroll(lh *h, embi_frag *f, float sx, float sy)
{
    embi_layout_frag_set_scroll(h->eng, h->doc, f, sx, sy);
}

static inline void lh_set_scrollbar(lh *h, float thickness)
{
    embi_layout_engine_destroy(h->eng);
    embi_layout_config cfg = {nullptr, nullptr, thickness};
    h->eng = embi_layout_engine_create(h->text, h->style, &cfg);
    (void)embi_layout_document(h->eng, h->doc);
}

/* Appends the text of the boxes that belong to element el's pseudo box pe (or its ::marker when
 * pe is EMBI_PE_MARKER). */
static inline void lh_pseudo_text(const embi_box *b, const embi_element *el, uint8_t pe, embi_sb *sb)
{
    for (const embi_box *k = b != nullptr ? embi_box_first(b) : nullptr; k != nullptr; k = k->next) {
        if (k->el == el && k->pe == pe && (k->kind == EMBI_BK_TEXT || k->kind == EMBI_BK_MARKER) &&
            k->text != nullptr) {
            embi_sb_append(sb, (embi_str){k->text, k->text_len});
        }
        lh_pseudo_text(k, el, pe, sb);
    }
}

/* The first box below b (b included) with the given kind and pseudo kind, or nullptr. */
static inline const embi_box *lh_find_box(const embi_box *b, uint8_t kind, uint8_t pe)
{
    if (b == nullptr) {
        return nullptr;
    }
    if (b->kind == kind && b->pe == pe) {
        return b;
    }
    for (const embi_box *k = embi_box_first(b); k != nullptr; k = k->next) {
        const embi_box *r = lh_find_box(k, kind, pe);
        if (r != nullptr) {
            return r;
        }
    }
    return nullptr;
}

/* The number of boxes below b (b included) that belong to element el with pseudo kind pe. */
static inline uint32_t lh_count_pseudo(const embi_box *b, const embi_element *el, uint8_t pe)
{
    if (b == nullptr) {
        return 0;
    }
    uint32_t n = (b->el == el && b->pe == pe) ? 1 : 0;
    for (const embi_box *k = embi_box_first(b); k != nullptr; k = k->next) {
        n += lh_count_pseudo(k, el, pe);
    }
    return n;
}

/* The box tree root (the root element's principal box), or nullptr. */
static inline const embi_box *lh_root_box(lh *h)
{
    embi_element *root = embi_document_element(h->doc);
    return root != nullptr ? (const embi_box *)root->box : nullptr;
}

static inline void lh_free(lh *h)
{
    embi_layout_engine_destroy(h->eng);
    embi_style_forget_document(h->style, h->doc);
    embi_document_free(h->doc);
    embi_css_sheet_unref(h->sheet);
    embi_style_engine_destroy(h->style);
    embi_tshim_destroy(h->text);
    memset(h, 0, sizeof *h);
}

static inline embi_element *lh_id(lh *h, const char *id)
{
    return embi_get_element_by_id(h->doc, (embi_str){id, (uint32_t)strlen(id)});
}

/* Relayouts the document (the styles are kept). */
static inline void lh_relayout(lh *h)
{
    (void)embi_layout_document(h->eng, h->doc);
}
