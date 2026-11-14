#pragma once
/*
 * Shared setup for the paint unit tests: parses a document with its author CSS, resolves styles, lays it out at
 * the default 800x600 viewport and builds its display list. Not a test itself (no test_ prefix).
 */
#include "emb_test.h"

#include "css/css.h"
#include "css/props.h"
#include "dom/dom.h"
#include "html/parser.h"
#include "layout/layout.h"
#include "layout/text_shim.h"
#include "paint/paint.h"
#include "style/style.h"

#include <SDL3/SDL_stdinc.h>
#include <string.h>

typedef struct ph {
    embi_text *text;
    embi_style_engine *style;
    embi_layout_engine *eng;
    embi_document *doc;
    embi_css_sheet *sheet;
    embi_dl dl;
} ph;

/* Joins the bodies of the <style> blocks of html with css (a body margin reset comes first, as in the layout
 * tests, so coordinates are from the viewport origin). Free with SDL_free. */
static inline char *ph_join_styles(const char *html, size_t n, const char *extra)
{
    static const char reset[] = "body{margin:0}\n";
    size_t cap = n + strlen(extra) + sizeof reset + 2, len = 0;
    char *out = SDL_malloc(cap);
    memcpy(out, reset, sizeof reset - 1);
    len = sizeof reset - 1;
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

/* Parses html, resolves its styles with css appended, lays it out and builds the display list with cfg. */
static inline ph ph_make_cfg(const char *html, const char *css, const embi_paint_cfg *cfg)
{
    ph h;
    memset(&h, 0, sizeof h);
    h.text = embi_tshim_create();
    h.style = embi_style_engine_create(embi_tshim_fonts(h.text), nullptr, nullptr);
    h.doc = embi_document_new(true);
    embi_html_parser *p = embi_html_parser_new(h.doc, nullptr);
    size_t n = strlen(html);
    embi_html_parser_write(p, html, n);
    embi_html_parser_end(p);
    embi_html_parser_destroy(p);
    char *all = ph_join_styles(html, n, css);
    embi_css_hooks hooks = {nullptr, embi_prop_decl_ok, nullptr};
    embi_css_parse_opts opts = {nullptr, &hooks, EMBI_STR("")};
    h.sheet = embi_css_sheet_parse((embi_str){all, (uint32_t)strlen(all)}, &opts);
    SDL_free(all);
    embi_css_sheet *sheets[1] = {h.sheet};
    embi_document_set_author_sheets(h.style, h.doc, sheets, 1);
    embi_style_resolve_document(h.style, h.doc);
    h.eng = embi_layout_engine_create(h.text, h.style, nullptr);
    (void)embi_layout_document(h.eng, h.doc);
    embi_dl_init(&h.dl);
    (void)embi_paint_document(&h.dl, h.eng, h.doc, cfg);
    return h;
}

/* Parses html and builds its display list with the default configuration (an 800 x 600 viewport). */
static inline ph ph_make(const char *html, const char *css)
{
    embi_paint_cfg cfg = {nullptr, nullptr, 0xFF1A73E8u, 800.0f, 600.0f, 1.0f};
    return ph_make_cfg(html, css, &cfg);
}

static inline void ph_free(ph *h)
{
    embi_dl_free(&h->dl);
    embi_layout_engine_destroy(h->eng);
    embi_style_forget_document(h->style, h->doc);
    embi_document_free(h->doc);
    embi_css_sheet_unref(h->sheet);
    embi_style_engine_destroy(h->style);
    embi_tshim_destroy(h->text);
    memset(h, 0, sizeof *h);
}

static inline embi_element *ph_id(ph *h, const char *id)
{
    return embi_get_element_by_id(h->doc, (embi_str){id, (uint32_t)strlen(id)});
}

/* The first item of kind k at or after index from, or -1. */
static inline int ph_find(const embi_dl *d, uint32_t from, uint8_t k)
{
    for (uint32_t i = from; i < d->items.len; i++) {
        if (d->items.data[i].kind == k) {
            return (int)i;
        }
    }
    return -1;
}

/* The number of items of kind k. */
static inline uint32_t ph_count(const embi_dl *d, uint8_t k)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < d->items.len; i++) {
        n += d->items.data[i].kind == k ? 1u : 0u;
    }
    return n;
}

/* The item index of the first FILL whose color is rgba, or -1. */
static inline int ph_fill_of(const embi_dl *d, uint32_t rgba)
{
    for (uint32_t i = 0; i < d->items.len; i++) {
        if (d->items.data[i].kind == EMBI_DI_FILL && d->items.data[i].colors[0] == rgba) {
            return (int)i;
        }
    }
    return -1;
}

/* CSS colors as the display list stores them (RGBA8, r in the low byte). */
#define PH_RED 0xFF0000FFu
#define PH_GREEN 0xFF00FF00u
#define PH_BLUE 0xFFFF0000u
#define PH_BLACK 0xFF000000u
#define PH_WHITE 0xFFFFFFFFu
