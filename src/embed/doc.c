/*
 * The document side of a view (W4e, see doc.h): parse, stylesheets, style, layout, display list, scrolling and
 * scrollbars. This translation unit includes the CSS, style, layout and paint headers, so it must not include the
 * renderer headers (gpu/render.h and friends clash with them).
 */
#include "embed/doc.h"
#include "embed/script_int.h"
#include "embed/frame.h"
#include "embed/frames.h"
#include "embed/input_int.h"
#include "embed/netload.h"

#include "css/css.h"
#include "css/props.h"
#include "dom/dom.h"
#include "dom/dstr.h"
#include "embed/url.h"
#include "html/parser.h"
#include "layout/box.h"
#include "layout/fragment.h"
#include "layout/layout.h"
#include "layout/text_shim.h"
#include "paint/layer.h"
#include "paint/paint.h"
#include "paint/scroll.h"
#include "style/style.h"
#include "svg/svg.h"

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

#include <string.h>

#define EMBI_DOC_FOCUS_DEFAULT 0xFF1A73E8u

/* A resolved URL: the raw text and its absolute form, both in the document's arena. */
typedef struct res_ent {
    embi_str raw;
    embi_str abs;
} res_ent;

struct embi_doc {
    embi_text *text;
    embi_fontdb *fonts;
    embi_doc_cb cb;
    bool (*fetch_font)(void *user, embi_str url, uint8_t **data, size_t *len); /* W4i: @font-face sources */
    bool (*fetch_script)(void *user, embi_str url, uint8_t **data, size_t *len); /* N1d: the source of a script that arrived */
    uint32_t (*pending_count)(void *user);                                      /* N1d */
    void (*prefetch)(void *user, embi_str url);                                 /* N1d: start a download that will be needed */
    bool (*net_pending)(void *user, embi_str url);                              /* N1f: a network resource still downloading */
    void (*load_done)(void *user, const embi_doc_times *t, bool ok);            /* N1f: a suspended load finished */
    bool sheet_pending; /* N1d: a fetch of the sheet being parsed was pending: the sheet is not cached, it parses again on arrival */
    const struct embi_svg_image *(*svg_image)(void *user, embi_str url); /* R4c */
    embi_style_engine *style;
    embi_layout_engine *eng;
    embi_document *dom;
    char *base;      /* the document's base URL (SDL_malloc'd, "" when none) */
    uint32_t base_len;
    float vw, vh, dppx;
    bool dark;
    bool env_dirty, layout_dirty;
    uint64_t font_epoch; /* W4i: the system font index epoch the layout saw */
    bool font_fallback;  /* W4i: a layout since the last index change used a fallback font */
    const void *sb_hover_key; /* the scroller whose styled scrollbar thumb the pointer is over, and which bar (1 vertical, 2 horizontal) */
    int sb_hover_axis;
    struct scrollers *sc_cache; /* the scrollers of the last layout (scrollers_new refreshes it when the layout or an offset changed) */
    uint64_t sc_calls;
    const embi_frag *sc_root;
    float sc_root_off[2];
    embi_paint_cache *pcache; /* W6b: the last display list, for the incremental paint (embi_doc_paint_ex) */
    bool paint_pending;  /* W6b: the last update changed only paint (the fragments are the old ones) */
    embi_arena arena;     /* resolved URLs */
    embi_hashmap rmap;    /* hash of a raw URL -> res_ent */
    embi_sb title;
    bool no_script;       /* scripts are off for this document (the view's script flag) */
    embi_script *script;  /* W5c: the event loop, timers and scripts of the document (script.c) */
    embi_animator *anim;  /* W6c: the animations of the document (frame.c) */
    embi_css_sheet **author;  /* W7d: the document's own sheets (<style>, <link>), in tree order: references held */
    uint32_t nauthor;
    embi_css_sheet **adopted; /* W7d: document.adoptedStyleSheets, in order: references held */
    uint32_t nadopted;
    /* W6e: the document's <style> and <link> elements change after the parse (and a script reads styles during it). The sheets
     * are collected again when a style element or a stylesheet link is inserted, removed or edited; the sheets of the
     * elements that did not change are kept (scache: element, signature of what it parsed, the sheet with a reference). */
    bool sheets_stale;
    bool in_load; /* a load is running or suspended (the parse, the deferred scripts, the sheets): scripts see the sheets of the
                   * part of the document built so far */
    /* N1f: the state of a load that can be suspended: where it is, the parser, the input not parsed yet. */
    struct doc_load {
        bool active;       /* a load is running or suspended */
        bool busy;         /* inside a call of the parser (a script may start another load) */
        int phase;         /* LP_PARSE, LP_DEFER or LP_SHEETS */
        bool ended;        /* embi_html_parser_end was called */
        bool defers_begun; /* embi_script_parse_done was called */
        bool ok;           /* the layout of the finished load */
        embi_html_parser *parser;
        const char *src;   /* the input not parsed yet: the caller's buffer while the load runs, then our copy */
        size_t len, off;
        char *copy;
        embi_doc_times tm;
    } ld;
    /* The parsers of loads that a script replaced while the parser ran (a stack of nested replacements): each is stopped, and the
     * drive that was inside it frees it when the call returns. */
    embi_html_parser **orphans;
    uint32_t norphans, cap_orphans;
    /* The stylesheet links the parser inserted, as absolute URLs: a script waits for those that are still downloading. */
    char **sheet_urls;
    uint32_t nsheet_urls, cap_sheet_urls;
    uint32_t load_epoch;      /* bumped by every load and by every end of a load (load_abort): a drive that sees it change after
                               * running script knows its load was replaced, and stops without touching the state */
    struct sheet_ent {
        const embi_element *el;
        uint64_t sig;
        embi_css_sheet *sh;
    } *scache;
    uint32_t nscache;
};

static const embi_style *root_style(const embi_doc *d);
static void release_sheets(embi_css_sheet **v, uint32_t n);
static void observe_dom(embi_doc *d, embi_document *dom);
static void scache_free(embi_doc *d);
static void load_abort(embi_doc *d);
static void sheet_urls_clear(embi_doc *d);

/* ------------------------------------------------------------------ callbacks into the view */

void embi_doc_set_net(embi_doc *d, bool (*fetch_script)(void *user, embi_str url, uint8_t **data, size_t *len),
                      uint32_t (*pending_count)(void *user), void (*prefetch)(void *user, embi_str url),
                      bool (*net_pending)(void *user, embi_str url))
{
    if (d != nullptr) {
        d->prefetch = prefetch;
        d->fetch_script = fetch_script;
        d->pending_count = pending_count;
        d->net_pending = net_pending;
    }
}

void embi_doc_set_load_done(embi_doc *d, void (*fn)(void *user, const embi_doc_times *t, bool ok))
{
    if (d != nullptr) {
        d->load_done = fn;
    }
}

void embi_doc_set_font_fetch(embi_doc *d, bool (*fetch)(void *user, embi_str url, uint8_t **data, size_t *len))
{
    if (d != nullptr) {
        d->fetch_font = fetch;
    }
}

static bool load_font_cb(void *user, embi_str base, embi_str url, uint8_t **data, size_t *len)
{
    embi_doc *d = user;
    embi_str b = base.len > 0 ? base : (embi_str){d->base, d->base_len};
    size_t n = 0;
    char *abs = embi_url_resolve(b, url, &n);
    bool (*fn)(void *, embi_str, uint8_t **, size_t *) = d->fetch_font != nullptr ? d->fetch_font : d->cb.fetch;
    uint32_t p0 = d->pending_count != nullptr ? d->pending_count(d->cb.user) : 0;
    bool ok = fn != nullptr && fn(d->cb.user, (embi_str){abs, (uint32_t)n}, data, len);
    if (!ok && d->pending_count != nullptr && d->pending_count(d->cb.user) != p0) {
        embi_style_font_defer(d->style); /* not there yet: the sheet's faces are registered again when it arrives */
    }
    SDL_free(abs);
    return ok;
}

bool embi_input_control_state(void *user, const embi_element *el, void *out);

static const embi_svg_image *doc_svg_image(void *user, embi_str url)
{
    embi_doc *d = user;
    if (d->svg_image == nullptr) {
        return nullptr;
    }
    return d->svg_image(d->cb.user, embi_doc_resolve(d, url));
}

#if EMBER_SVG
static const embi_element *doc_svg_hit(const embi_layout_engine *e, const embi_element *svg, const float box[4], float x, float y)
{
    embi_svg_env env;
    memset(&env, 0, sizeof env);
    env.se = embi_layout_style_engine(e);
    env.text = embi_layout_text(e);
    env.dev_scale = 1.0f;
    env.layout = e; /* foreignObject content is hit through the layout's boxes */
    return embi_svg_hit(&env, svg, box, x, y);
}

/* The box of a foreignObject for layout (R4c): the SVG module resolves its lengths and transforms. */
static bool doc_svg_fo(embi_style_engine *se, const embi_element *svg, const embi_element *fo, float cw, float ch, float out[4])
{
    (void)svg;
    embi_svg_env env;
    memset(&env, 0, sizeof env);
    env.se = se;
    env.dev_scale = 1.0f;
    embi_svg_geom g = {&env, cw, ch};
    return embi_svg_foreign_box(&g, fo, out);
}
#endif

bool embi_doc_image_natural(embi_doc *d, embi_str raw, float *w, float *h)
{
    bool ratio;
    return d->cb.image_size != nullptr && d->cb.image_size(d->cb.user, embi_doc_resolve(d, raw), w, h, &ratio);
}

void embi_doc_set_svg_image(embi_doc *d, const struct embi_svg_image *(*fn)(void *user, embi_str url))
{
    d->svg_image = fn;
}

static bool image_size_cb(void *user, embi_str url, float *w, float *h, bool *has_ratio)
{
    embi_doc *d = user;
    if (d->cb.image_size == nullptr) {
        return false;
    }
    embi_str a = embi_doc_resolve(d, url);
    return d->cb.image_size(d->cb.user, a, w, h, has_ratio);
}

static embi_style_env make_env(const embi_doc *d)
{
    embi_style_env e;
    memset(&e, 0, sizeof e);
    e.width = d->vw;
    e.height = d->vh;
    e.dppx = d->dppx > 0.0f ? d->dppx : 1.0;
    e.dark = d->dark;
    e.hover = true;
    e.pointer = 2;
    e.update = 2;
    e.scripting = !d->no_script; /* the scripting media feature follows whether scripts run */
    return e;
}

/* ------------------------------------------------------------------ lifetime */

embi_doc *embi_doc_create(embi_text *text, embi_fontdb *fonts, const embi_doc_cb *cb)
{
    embi_doc *d = embi_alloc(sizeof *d);
    memset(d, 0, sizeof *d);
    d->text = text;
    d->fonts = fonts;
    if (cb != nullptr) {
        d->cb = *cb;
    }
    /* First, so that the realm thread it starts (FP2a) overlaps the style and layout engines below. */
    d->script = embi_script_new(d);
    d->vw = 800.0f;
    d->vh = 600.0f;
    d->dppx = 1.0f;
    d->base = SDL_strdup("");
    embi_paint_set_svg_image_fn(doc_svg_image);
#if EMBER_SVG
    embi_layout_set_svg_hit(doc_svg_hit);
    embi_layout_set_svg_fo(doc_svg_fo);
#endif
    embi_style_callbacks scb = {d, load_font_cb};
    embi_style_env env = make_env(d);
    d->style = embi_style_engine_create(fonts, &env, &scb);
    embi_layout_config cfg = {d, image_size_cb, 0.0f};
    d->eng = embi_layout_engine_create(text, d->style, &cfg);
    embi_layout_set_viewport(d->eng, d->vw, d->vh);
    embi_layout_set_images_stable(d->eng, true); /* image sizes are fetched once and cached: a size is stable per URL */
    embi_layout_set_control_hook(d->eng, embi_input_control_state, d); /* W5e: the painter reads form control state */
    embi_arena_init(&d->arena, 4096);
    embi_hashmap_init(&d->rmap);
    memset(&d->title, 0, sizeof d->title);
    d->anim = embi_frame_animator_new(d);
    return d;
}

void embi_doc_set_scripting(embi_doc *d, bool on)
{
    d->no_script = !on;
    if (!on) {
        embi_script_no_prewarm(d->script);
    }
}

bool embi_doc_scripting(const embi_doc *d)
{
    return !d->no_script;
}

/* Drops the document (not the engines): the layout and style records of it, then the DOM. */
static void scrollers_cache_free(embi_doc *d);

static void drop_document(embi_doc *d)
{
    embi_paint_cache_free(d->pcache);
    d->pcache = nullptr;
    scrollers_cache_free(d);
    d->paint_pending = false;
    if (d->dom != nullptr) {
        embi_layout_forget_document(d->eng, d->dom);
    }
}

void embi_doc_destroy(embi_doc *d)
{
    if (d == nullptr) {
        return;
    }
    load_abort(d); /* a suspended load: its parser and its input go before the DOM */
    /* The realm goes first: its wrappers refer to the DOM that is freed below. */
    embi_script_free(d->script);
    d->script = nullptr;
    embi_animator_free(d->anim);
    d->anim = nullptr;
    embi_paint_cache_free(d->pcache);
    d->pcache = nullptr;
    embi_layout_engine_destroy(d->eng);
    d->eng = nullptr;
    if (d->dom != nullptr) {
        embi_style_forget_document(d->style, d->dom);
        embi_document_free(d->dom);
        d->dom = nullptr;
    }
    embi_style_engine_destroy(d->style);
    scache_free(d);
    release_sheets(d->author, d->nauthor);
    release_sheets(d->adopted, d->nadopted);
    d->author = nullptr;
    d->adopted = nullptr;
    sheet_urls_clear(d);
    SDL_free(d->sheet_urls);
    SDL_free(d->orphans);
    embi_arena_free(&d->arena);
    embi_hashmap_free(&d->rmap);
    embi_sb_free(&d->title);
    SDL_free(d->base);
    embi_free(d, sizeof *d);
}

void embi_doc_set_viewport(embi_doc *d, float w, float h, float dppx, bool dark)
{
    if (w != d->vw || h != d->vh || dppx != d->dppx || dark != d->dark) {
        d->vw = w;
        d->vh = h;
        d->dppx = dppx;
        d->dark = dark;
        d->env_dirty = true;
        d->layout_dirty = true;
    }
}

void embi_doc_prime_env(embi_doc *d)
{
    if (d == nullptr || d->dom != nullptr || d->style == nullptr) {
        return;
    }
    /* No document yet: the engine holds no sheet of this view, so this costs the initial style only. The load's own
     * set_env then finds the environment equal and re-flattens nothing. */
    embi_style_env env = make_env(d);
    embi_style_engine_set_env(d->style, &env);
}

void embi_doc_viewport(const embi_doc *d, float *w, float *h, float *dppx)
{
    *w = d->vw;
    *h = d->vh;
    *dppx = d->dppx;
}

bool embi_doc_loaded(const embi_doc *d)
{
    return d->dom != nullptr;
}

/* ------------------------------------------------------------------ URLs */

embi_str embi_doc_resolve(embi_doc *d, embi_str raw)
{
    if (raw.len == 0) {
        return raw;
    }
    uint64_t key = embi_hash_bytes(raw.ptr, raw.len, 0x5eed);
    uint64_t v = 0;
    if (embi_hashmap_get(&d->rmap, key, &v)) {
        res_ent *e = (res_ent *)(uintptr_t)v;
        if (embi_str_eq(e->raw, raw)) {
            return e->abs;
        }
    }
    size_t n = 0;
    char *abs = embi_url_resolve((embi_str){d->base, d->base_len}, raw, &n);
    res_ent *e = embi_arena_alloc(&d->arena, sizeof *e, _Alignof(res_ent));
    char *rawc = embi_arena_alloc(&d->arena, raw.len + 1, 1);
    char *absc = embi_arena_alloc(&d->arena, n + 1, 1);
    SDL_memcpy(rawc, raw.ptr, raw.len);
    SDL_memcpy(absc, abs, n);
    SDL_free(abs);
    e->raw = (embi_str){rawc, raw.len};
    e->abs = (embi_str){absc, (uint32_t)n};
    if (!embi_hashmap_get(&d->rmap, key, &v)) {
        embi_hashmap_put(&d->rmap, key, (uint64_t)(uintptr_t)e);
    }
    return e->abs;
}

/* ------------------------------------------------------------------ DOM helpers */

static bool named(const embi_element *el, const char *name)
{
    return embi_str_ieq(embi_atom_str(el->base.name), (embi_str){name, (uint32_t)strlen(name)});
}

/* The value of attribute name as UTF-8 in sb (sb is cleared first). False when the attribute is absent. */
static bool attr_text(embi_element *el, const char *name, embi_sb *sb)
{
    embi_sb_clear(sb);
    const embi_attr *a = embi_el_get_attribute(el, (embi_str){name, (uint32_t)strlen(name)});
    if (a == nullptr) {
        return false;
    }
    embi_dstr_append_utf8(sb, a->value);
    return true;
}

/* Whether the space-separated token list of rel contains token (ASCII case-insensitive). */
static bool has_token(embi_str list, const char *token)
{
    size_t tl = strlen(token);
    uint32_t i = 0;
    while (i < list.len) {
        while (i < list.len && (list.ptr[i] == ' ' || list.ptr[i] == '\t' || list.ptr[i] == '\n' || list.ptr[i] == '\r')) {
            i++;
        }
        uint32_t s = i;
        while (i < list.len && !(list.ptr[i] == ' ' || list.ptr[i] == '\t' || list.ptr[i] == '\n' || list.ptr[i] == '\r')) {
            i++;
        }
        if (i - s == tl && embi_str_ieq((embi_str){list.ptr + s, i - s}, (embi_str){token, (uint32_t)tl})) {
            return true;
        }
    }
    return false;
}

/* The text of the element's child text nodes, in sb. */
static void child_text(embi_node *el, embi_sb *sb)
{
    for (embi_node *c = el->first_child; c != nullptr; c = c->next) {
        if (c->type == EMBI_NODE_TEXT) {
            embi_dstr_append_utf8(sb, embi_as_chardata(c)->data);
        }
    }
}

/* The first element named name in document order, or nullptr. */
static embi_element *find_named(embi_document *dom, const char *name)
{
    embi_node *root = &dom->base;
    for (embi_node *n = embi_next_in_tree(root, root); n != nullptr; n = embi_next_in_tree(root, n)) {
        if (embi_is_element(n) && named(embi_as_element(n), name)) {
            return embi_as_element(n);
        }
    }
    return nullptr;
}

/* ------------------------------------------------------------------ stylesheets */

typedef struct sheets {
    embi_css_sheet **v;
    uint32_t n, cap;
} sheets;

static void sheets_push(sheets *s, embi_css_sheet *sh)
{
    if (s->n == s->cap) {
        uint32_t cap = s->cap ? s->cap * 2 : 8;
        embi_css_sheet **v = SDL_realloc(s->v, cap * sizeof *v);
        EMBI_ASSERT(v != nullptr);
        s->v = v;
        s->cap = cap;
    }
    s->v[s->n++] = sh;
}

/* The @import loader of a sheet parse: the target is resolved against the importing sheet's URL and fetched through
 * the view like a linked sheet. The bytes and the resolved URL live until the parse ends (the parser copies what it
 * keeps), so the context owns them. */
typedef struct import_ctx {
    embi_doc *d;
    char *resolved;
    EMBI_VEC(uint8_t *) bufs;
} import_ctx;

static bool import_fetch(void *user, embi_str base, embi_str url, embi_str *resolved, const unsigned char **bytes,
                         size_t *n)
{
    import_ctx *c = user;
    embi_doc *d = c->d;
    if (d->cb.fetch == nullptr) {
        return false;
    }
    embi_str b = base.len > 0 ? base : (embi_str){d->base, d->base_len};
    size_t rn = 0;
    SDL_free(c->resolved);
    c->resolved = embi_url_resolve(b, url, &rn);
    *resolved = (embi_str){c->resolved, (uint32_t)rn};
    uint8_t *data = nullptr;
    size_t len = 0;
    uint32_t p0 = d->pending_count != nullptr ? d->pending_count(d->cb.user) : 0;
    if (!d->cb.fetch(d->cb.user, *resolved, &data, &len)) {
        if (d->pending_count != nullptr && d->pending_count(d->cb.user) != p0) {
            d->sheet_pending = true;
        }
        return false;
    }
    embi_vec_push(&c->bufs, data);
    *bytes = data;
    *n = len;
    return true;
}

static void import_done(import_ctx *c)
{
    for (uint32_t i = 0; i < c->bufs.len; i++) {
        SDL_free(c->bufs.data[i]);
    }
    embi_vec_free(&c->bufs);
    SDL_free(c->resolved);
}

static embi_css_sheet *parse_text(embi_doc *d, embi_str text, embi_str base)
{
    embi_css_hooks hooks = {nullptr, embi_prop_decl_ok, nullptr};
    import_ctx ic = {d, nullptr, {nullptr, 0, 0}};
    embi_css_loader loader = {&ic, import_fetch};
    embi_css_parse_opts opts = {&loader, &hooks, base};
    embi_css_sheet *sh = embi_css_sheet_parse(text, &opts);
    import_done(&ic);
    return sh;
}

/* Whether the sheet of el applies by its attributes: not disabled, and its type (when given) is text/css. tmp is
 * scratch. */
static bool sheet_enabled(embi_element *el, embi_sb *tmp)
{
    if (embi_el_get_attribute(el, EMBI_STR("disabled")) != nullptr) {
        return false;
    }
    if (attr_text(el, "type", tmp) && tmp->len > 0 &&
        !embi_str_ieq((embi_str){tmp->data, tmp->len}, EMBI_STR("text/css"))) {
        return false;
    }
    return true;
}

/* The text "@media <media> {<css>}", so the style engine evaluates the query against the document's environment.
 * False when media cannot be a query list (braces and semicolons would break the wrapper): the sheet then does not
 * apply, as an invalid media query does not. */
static bool media_wrap(embi_sb *out, embi_str media, const char *css, size_t n)
{
    for (uint32_t i = 0; i < media.len; i++) {
        if (media.ptr[i] == '{' || media.ptr[i] == '}' || media.ptr[i] == ';') {
            return false;
        }
    }
    embi_sb_clear(out);
    embi_sb_append(out, EMBI_STR("@media "));
    embi_sb_append(out, media);
    embi_sb_append(out, EMBI_STR(" {"));
    embi_sb_append(out, (embi_str){css, (uint32_t)n});
    embi_sb_append(out, EMBI_STR("}"));
    return true;
}

/* Collects the sheets of the document in tree order: each <style> element's text, and each
 * <link rel=stylesheet> fetched through the view. A disabled sheet, one whose type is not text/css, and one whose
 * media does not match the environment are left out (the media match is done by the style engine). */
typedef struct sheet_ent sheet_ent;

/* The sheet a style or link element parsed last time with the same signature, with a reference taken; nullptr if none. */
static embi_css_sheet *scache_find(const embi_doc *d, const embi_element *el, uint64_t sig)
{
    for (uint32_t i = 0; i < d->nscache; i++) {
        if (d->scache[i].el == el && d->scache[i].sig == sig) {
            embi_css_sheet_ref(d->scache[i].sh);
            return d->scache[i].sh;
        }
    }
    return nullptr;
}

typedef struct scache_new {
    sheet_ent *v;
    uint32_t n, cap;
} scache_new;

static void scache_add(scache_new *c, const embi_element *el, uint64_t sig, embi_css_sheet *sh)
{
    if (c->n == c->cap) {
        uint32_t cap = c->cap ? c->cap * 2 : 8;
        sheet_ent *v = SDL_realloc(c->v, cap * sizeof *v);
        EMBI_ASSERT(v != nullptr);
        c->v = v;
        c->cap = cap;
    }
    embi_css_sheet_ref(sh);
    c->v[c->n++] = (sheet_ent){el, sig, sh};
}

/* The absolute href of el when it is a <link> that loads a stylesheet now (rel stylesheet, not an alternate, enabled, with an
 * href), else nullptr. The caller frees it. */
static char *link_sheet_url(embi_doc *d, embi_element *el, size_t *n)
{
    embi_sb attr = {nullptr, 0, 0};
    char *abs = nullptr;
    if (named(el, "link") && attr_text(el, "rel", &attr) && has_token((embi_str){attr.data, attr.len}, "stylesheet") &&
        !has_token((embi_str){attr.data, attr.len}, "alternate") && sheet_enabled(el, &attr) &&
        attr_text(el, "href", &attr) && attr.len > 0) {
        abs = embi_url_resolve((embi_str){d->base, d->base_len}, (embi_str){attr.data, attr.len}, n);
    }
    embi_sb_free(&attr);
    return abs;
}

/* Calls fn(d, absolute href, arg) for each <link> of the tree that loads a stylesheet now. */
static void each_link_sheet(embi_doc *d, void (*fn)(embi_doc *d, embi_str abs, void *arg), void *arg)
{
    embi_node *root = &d->dom->base;
    for (embi_node *n = embi_next_in_tree(root, root); n != nullptr; n = embi_next_in_tree(root, n)) {
        if (!embi_is_element(n)) {
            continue;
        }
        size_t pn = 0;
        char *pabs = link_sheet_url(d, embi_as_element(n), &pn);
        if (pabs != nullptr) {
            fn(d, (embi_str){pabs, (uint32_t)pn}, arg);
            SDL_free(pabs);
        }
    }
}

/* The parser just inserted the run of siblings a..b: its stylesheet links start downloading at once (not when the sheets are
 * collected) and are remembered, because a script after them waits for them. Links made by scripts are not noted (they do not
 * block scripts). */
static void note_parser_links(embi_doc *d, embi_node *a, embi_node *b)
{
    for (embi_node *n = a; n != nullptr; n = n == b ? nullptr : n->next) {
        if (!embi_is_element(n)) {
            continue;
        }
        size_t pn = 0;
        char *pabs = link_sheet_url(d, embi_as_element(n), &pn);
        if (pabs == nullptr) {
            continue;
        }
        d->prefetch(d->cb.user, (embi_str){pabs, (uint32_t)pn});
        if (d->nsheet_urls == d->cap_sheet_urls) {
            d->cap_sheet_urls = d->cap_sheet_urls ? d->cap_sheet_urls * 2 : 8;
            d->sheet_urls = SDL_realloc(d->sheet_urls, d->cap_sheet_urls * sizeof *d->sheet_urls);
            EMBI_ASSERT(d->sheet_urls != nullptr);
        }
        d->sheet_urls[d->nsheet_urls++] = pabs;
    }
}

static void sheet_urls_clear(embi_doc *d)
{
    for (uint32_t i = 0; i < d->nsheet_urls; i++) {
        SDL_free(d->sheet_urls[i]);
    }
    d->nsheet_urls = 0;
}

static void link_prefetch(embi_doc *d, embi_str abs, void *arg)
{
    (void)arg;
    d->prefetch(d->cb.user, abs);
}

bool embi_doc_sheets_pending(embi_doc *d)
{
    if (d == nullptr || d->net_pending == nullptr) {
        return false;
    }
    bool pending = false;
    for (uint32_t i = 0; i < d->nsheet_urls;) {
        char *u = d->sheet_urls[i];
        if (d->net_pending(d->cb.user, (embi_str){u, (uint32_t)strlen(u)})) {
            pending = true;
            i++;
        } else { /* arrived or failed: it never blocks again */
            SDL_free(u);
            d->sheet_urls[i] = d->sheet_urls[--d->nsheet_urls];
        }
    }
    return pending;
}

static void collect_sheets(embi_doc *d, sheets *out)
{
    scache_new nc = {nullptr, 0, 0};
    embi_node *root = &d->dom->base;
    embi_sb buf, attr, media, wrapped;
    memset(&buf, 0, sizeof buf);
    memset(&attr, 0, sizeof attr);
    memset(&media, 0, sizeof media);
    memset(&wrapped, 0, sizeof wrapped);
    embi_str base = {d->base, d->base_len};
    if (d->prefetch != nullptr) {
        /* Every linked sheet starts downloading before the first one is waited for. */
        each_link_sheet(d, link_prefetch, nullptr);
    }
    for (embi_node *n = embi_next_in_tree(root, root); n != nullptr; n = embi_next_in_tree(root, n)) {
        if (!embi_is_element(n)) {
            continue;
        }
        embi_element *el = embi_as_element(n);
        if (named(el, "style")) {
            if (!sheet_enabled(el, &attr)) {
                continue;
            }
            embi_sb_clear(&buf);
            child_text(n, &buf);
            const char *css = buf.data != nullptr ? buf.data : "";
            size_t cn = buf.len;
            if (attr_text(el, "media", &media) && media.len > 0) {
                if (!media_wrap(&wrapped, (embi_str){media.data, media.len}, css, cn)) {
                    continue;
                }
                css = wrapped.data;
                cn = wrapped.len;
            }
            uint64_t sig = embi_hash_bytes(css, cn, 0x5717ull);
            embi_css_sheet *sh = scache_find(d, el, sig);
            d->sheet_pending = false;
            if (sh == nullptr) {
                sh = parse_text(d, (embi_str){css, (uint32_t)cn}, base);
            }
            if (sh != nullptr) {
                if (!d->sheet_pending) { /* an @import that has not arrived: parsed again when it does */
                    scache_add(&nc, el, sig, sh);
                }
                sheets_push(out, sh);
            }
        } else if (named(el, "link") && attr_text(el, "rel", &attr) && has_token((embi_str){attr.data, attr.len}, "stylesheet")) {
            /* rel="alternate stylesheet" is not applied until a user picks it (there is no such choice here). */
            if (has_token((embi_str){attr.data, attr.len}, "alternate") || !sheet_enabled(el, &attr)) {
                continue;
            }
            bool has_media = attr_text(el, "media", &media) && media.len > 0;
            /* The charset attribute is the environment encoding of the sheet (CSS Syntax 3 section 3.2): the label
             * used when neither a BOM, the protocol nor an @charset rule says. */
            embi_sb cset;
            memset(&cset, 0, sizeof cset);
            bool has_cset = attr_text(el, "charset", &cset) && cset.len > 0;
            embi_str env_label = has_cset ? (embi_str){cset.data, cset.len} : EMBI_STR("");
            if (!attr_text(el, "href", &attr) || attr.len == 0 || d->cb.fetch == nullptr) {
                embi_sb_free(&cset);
                continue;
            }
            size_t rn = 0;
            char *abs = embi_url_resolve(base, (embi_str){attr.data, attr.len}, &rn);
            uint8_t *bytes = nullptr;
            size_t blen = 0;
            uint64_t lsig = embi_hash_bytes(abs, rn, 0x11c4ull) ^ embi_hash_bytes(media.data != nullptr ? media.data : "", has_media ? media.len : 0, 0x3ull) ^
                            embi_hash_bytes(cset.data != nullptr ? cset.data : "", has_cset ? cset.len : 0, 0x7ull);
            embi_css_sheet *cached = scache_find(d, el, lsig);
            if (cached != nullptr) {
                scache_add(&nc, el, lsig, cached);
                sheets_push(out, cached);
            } else if (d->cb.fetch(d->cb.user, (embi_str){abs, (uint32_t)rn}, &bytes, &blen)) {
                embi_css_sheet *sh = nullptr;
                d->sheet_pending = false;
                if (!has_media) {
                    embi_css_hooks hooks = {nullptr, embi_prop_decl_ok, nullptr};
                    import_ctx ic = {d, nullptr, {nullptr, 0, 0}};
                    embi_css_loader loader = {&ic, import_fetch};
                    embi_css_parse_opts opts = {&loader, &hooks, (embi_str){abs, (uint32_t)rn}};
                    const char *enc = nullptr;
                    sh = embi_css_sheet_parse_bytes(bytes, blen, EMBI_STR(""), env_label, &opts, &enc);
                    import_done(&ic);
                } else {
                    /* The bytes are decoded first (BOM, @charset), as on the path without media, then wrapped. */
                    embi_arena dec;
                    embi_arena_init(&dec, 4096);
                    embi_str text = {nullptr, 0};
                    const char *enc = nullptr;
                    if (embi_css_decode_bytes(&dec, bytes, blen, EMBI_STR(""), env_label, &text, &enc) &&
                        media_wrap(&wrapped, (embi_str){media.data, media.len}, text.ptr != nullptr ? text.ptr : "",
                                   text.len)) {
                        sh = parse_text(d, (embi_str){wrapped.data, wrapped.len}, (embi_str){abs, (uint32_t)rn});
                    }
                    embi_arena_free(&dec);
                }
                if (sh != nullptr) {
                    if (!d->sheet_pending) {
                        scache_add(&nc, el, lsig, sh);
                    }
                    sheets_push(out, sh);
                }
                SDL_free(bytes);
            }
            embi_sb_free(&cset);
            SDL_free(abs);
        }
    }
    embi_sb_free(&buf);
    embi_sb_free(&attr);
    embi_sb_free(&media);
    embi_sb_free(&wrapped);
    /* The cache of this collection replaces the last one. */
    for (uint32_t i = 0; i < d->nscache; i++) {
        embi_css_sheet_unref(d->scache[i].sh);
    }
    SDL_free(d->scache);
    d->scache = nc.v;
    d->nscache = nc.n;
}

/* Collapses the ASCII white space of the title (runs become one space, and the ends are trimmed), as the window title
 * shows it. */
static void collapse_title(embi_sb *sb)
{
    uint32_t out = 0;
    bool pending = false, started = false;
    for (uint32_t i = 0; i < sb->len; i++) {
        char c = sb->data[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
            pending = started;
            continue;
        }
        if (pending) {
            sb->data[out++] = ' ';
            pending = false;
        }
        sb->data[out++] = c;
        started = true;
    }
    sb->len = out;
}

/* ------------------------------------------------------------------ load and update */

static void set_base(embi_doc *d, embi_str base_url)
{
    SDL_free(d->base);
    d->base = nullptr;
    d->base_len = 0;
    if (base_url.len == 0) {
        d->base = SDL_strdup("");
        return;
    }
    embi_str scheme;
    if (embi_url_scheme(base_url, &scheme)) {
        d->base = SDL_malloc(base_url.len + 1);
        SDL_memcpy(d->base, base_url.ptr, base_url.len);
        d->base[base_url.len] = '\0';
        d->base_len = base_url.len;
        return;
    }
    /* A bare path: its file: URL is the base. */
    size_t cap = embi_url_from_bare_path(base_url, nullptr, 0) + 1;
    d->base = SDL_malloc(cap);
    d->base_len = (uint32_t)embi_url_from_bare_path(base_url, d->base, cap);
}

static void release_sheets(embi_css_sheet **v, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        embi_css_sheet_unref(v[i]);
    }
    SDL_free(v);
}

/* Gives the style engine the document's sheets in cascade order: its own sheets, then the adopted ones (W7d). */
static void apply_sheets(embi_doc *d)
{
    uint32_t n = d->nauthor + d->nadopted;
    embi_css_sheet **all = n > 0 ? SDL_malloc(n * sizeof *all) : nullptr;
    for (uint32_t i = 0; i < d->nauthor; i++) {
        all[i] = d->author[i];
    }
    for (uint32_t i = 0; i < d->nadopted; i++) {
        all[d->nauthor + i] = d->adopted[i];
    }
    embi_document_set_author_sheets(d->style, d->dom, all, n);
    embi_style_env aenv = make_env(d);
    embi_animator_set_sheets(d->anim, all, n, &aenv);
    SDL_free(all);
}

void embi_doc_set_adopted(embi_doc *d, embi_css_sheet *const *list, uint32_t n)
{
    embi_css_sheet **v = n > 0 ? SDL_malloc(n * sizeof *v) : nullptr;
    for (uint32_t i = 0; i < n; i++) {
        v[i] = list[i];
        embi_css_sheet_ref(v[i]);
    }
    release_sheets(d->adopted, d->nadopted);
    d->adopted = v;
    d->nadopted = n;
    if (d->dom != nullptr) {
        apply_sheets(d);
        embi_doc_invalidate(d);
    }
}

static void scache_free(embi_doc *d)
{
    for (uint32_t i = 0; i < d->nscache; i++) {
        embi_css_sheet_unref(d->scache[i].sh);
    }
    SDL_free(d->scache);
    d->scache = nullptr;
    d->nscache = 0;
}

void embi_doc_sync_sheets(embi_doc *d)
{
    if (d == nullptr || !d->sheets_stale || d->dom == nullptr) {
        return;
    }
    d->sheets_stale = false;
    sheets sh;
    memset(&sh, 0, sizeof sh);
    collect_sheets(d, &sh);
    release_sheets(d->author, d->nauthor);
    d->author = sh.v;
    d->nauthor = sh.n;
    apply_sheets(d);
    if (d->in_load) {
        /* A script of the parse reads styles: the media environment is the viewport's. The load sets it again at its end. */
        embi_style_env env = make_env(d);
        embi_style_engine_set_env(d->style, &env);
    }
    d->layout_dirty = true;
}

/* ------------------------------------------------------------------ the load (begin, resume, finish) */

enum { LP_PARSE = 0, LP_DEFER, LP_SHEETS };
enum { LD_WAIT = 0, LD_DONE, LD_GONE };

static double ticks_ms_since(uint64_t t0)
{
    return (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
}

/* Ends a load, running or suspended: the parser goes (before the DOM it builds into), and so does the input copy. A load that
 * a script of its own parse replaced (the parser is inside a call) hands its parser to the drive that is still on the stack. */
static void load_abort(embi_doc *d)
{
    struct doc_load *L = &d->ld;
    if (L->parser != nullptr) {
        if (L->busy) {
            if (d->norphans == d->cap_orphans) {
                d->cap_orphans = d->cap_orphans ? d->cap_orphans * 2 : 4;
                d->orphans = SDL_realloc(d->orphans, d->cap_orphans * sizeof *d->orphans);
                EMBI_ASSERT(d->orphans != nullptr);
            }
            d->orphans[d->norphans++] = L->parser;
        } else {
            embi_html_parser_destroy(L->parser);
        }
    }
    SDL_free(L->copy);
    memset(L, 0, sizeof *L);
    sheet_urls_clear(d);
    d->in_load = false;
    d->load_epoch++;
}

bool embi_doc_load_suspended(const embi_doc *d)
{
    return d != nullptr && d->ld.active;
}

/* The drive stops here: the load waits for the network. The input not parsed yet is copied, since the caller's buffer is gone
 * when the thread returns to it. */
static int load_suspend(embi_doc *d)
{
    struct doc_load *L = &d->ld;
    if (L->copy == nullptr && L->off < L->len) {
        size_t n = L->len - L->off;
        L->copy = SDL_malloc(n);
        EMBI_ASSERT(L->copy != nullptr);
        memcpy(L->copy, L->src + L->off, n);
        L->src = L->copy;
        L->len = n;
        L->off = 0;
    }
    return LD_WAIT;
}

/* The load was replaced while a script of it ran: the parser it left behind is freed, and nothing else of it is touched. */
static int load_gone(embi_doc *d, embi_html_parser *p)
{
    for (uint32_t i = 0; i < d->norphans && p != nullptr; i++) {
        if (d->orphans[i] == p) {
            d->orphans[i] = d->orphans[--d->norphans];
            embi_html_parser_destroy(p);
            break;
        }
    }
    return LD_GONE;
}

/* Runs the load as far as it can without the network: the parse (a script that has not arrived suspends it), the deferred and
 * module scripts, the stylesheets (a pending one suspends the first layout), then style and layout. Begin and every arrival
 * call it; each step checks what it needs by state and returns LD_WAIT when that is not there yet. */
static int load_drive(embi_doc *d)
{
    struct doc_load *L = &d->ld;
    const uint32_t epoch = d->load_epoch;
    embi_script *sc = d->script;
    uint64_t t0 = SDL_GetPerformanceCounter();

    if (L->phase == LP_PARSE) {
        embi_html_parser *p = L->parser;
        for (;;) {
            if (embi_script_has_blocked(sc)) {
                L->busy = true;
                bool ran = embi_script_run_blocked(sc);
                if (d->load_epoch != epoch) {
                    return load_gone(d, p);
                }
                L->busy = false;
                if (!ran) {
                    L->tm.parse_ms += ticks_ms_since(t0);
                    return load_suspend(d);
                }
            }
            if (embi_html_parser_paused(p)) {
                L->busy = true;
                embi_html_parser_resume(p);
                if (d->load_epoch != epoch) {
                    return load_gone(d, p);
                }
                L->busy = false;
                continue;
            }
            if (L->off < L->len) {
                L->busy = true;
                size_t n = embi_html_parser_write(p, L->src + L->off, L->len - L->off);
                if (d->load_epoch != epoch) {
                    return load_gone(d, p);
                }
                L->busy = false;
                L->off += n;
                if (n == 0 && !embi_html_parser_paused(p)) {
                    L->off = L->len; /* cannot happen; never loop on it */
                }
                continue;
            }
            if (!L->ended) {
                L->ended = true;
                L->busy = true;
                embi_html_parser_end(p);
                if (d->load_epoch != epoch) {
                    return load_gone(d, p);
                }
                L->busy = false;
                continue; /* the end may have run a last script that blocks */
            }
            break;
        }
        embi_html_parser_destroy(p);
        L->parser = nullptr;
        L->phase = LP_DEFER;
    }

    if (L->phase == LP_DEFER) {
        if (!L->defers_begun) {
            embi_script_parse_done(sc); /* the module preloads start, readyState is interactive */
            if (d->load_epoch != epoch) { /* a readystatechange handler replaced the document: L is the new load's */
                return LD_GONE;
            }
            L->defers_begun = true;
        }
        bool done = embi_script_run_defers(sc); /* in document order; each waits for its bytes (or its module graph) */
        if (d->load_epoch != epoch) {
            return LD_GONE;
        }
        if (!done) {
            L->tm.parse_ms += ticks_ms_since(t0);
            return load_suspend(d);
        }
        embi_input_doc_ready(embi_doc_owner(d)); /* autofocus: the elements the parse made, and those inserted later */
        embi_frames_doc_ready(embi_doc_owner(d)); /* IF1: the iframes the parse made, and those inserted later */
        if (d->load_epoch != epoch) { /* a focus handler replaced the document */
            return LD_GONE;
        }
        L->tm.parse_ms += ticks_ms_since(t0);
        embi_script_stage_times(sc, &L->tm.script_realm_ms, &L->tm.script_compile_ms, &L->tm.script_run_ms);
        L->phase = LP_SHEETS;
        t0 = SDL_GetPerformanceCounter();
    }

    /* The stylesheets: every <style> parsed, every <link rel=stylesheet> fetched and parsed, and the sheets handed to the
     * style engine. A sheet (or an @import) that has not arrived keeps the first layout waiting: the load is suspended and the
     * view keeps showing what it showed before. A sheet that failed is ignored. */
    embi_document *dom = d->dom;
    sheets sh;
    memset(&sh, 0, sizeof sh);
    uint32_t p0 = d->pending_count != nullptr ? d->pending_count(d->cb.user) : 0;
    collect_sheets(d, &sh);
    if (d->pending_count != nullptr && d->pending_count(d->cb.user) != p0) {
        release_sheets(sh.v, sh.n);
        L->tm.sheets_ms += ticks_ms_since(t0);
        return load_suspend(d);
    }
    d->in_load = false;
    /* The document keeps its sheets (the style engine refs them too). A new document adopts nothing. */
    release_sheets(d->author, d->nauthor);
    d->author = sh.v;
    d->nauthor = sh.n;
    release_sheets(d->adopted, d->nadopted);
    d->adopted = nullptr;
    d->nadopted = 0;
    apply_sheets(d);
    d->sheets_stale = false;
    L->tm.sheets_ms += ticks_ms_since(t0);

    embi_sb_clear(&d->title);
    embi_element *te = find_named(dom, "title");
    if (te != nullptr) {
        child_text(&te->base, &d->title);
    }
    collapse_title(&d->title);
    embi_sb_cstr(&d->title);

    /* The media environment follows the viewport set by embi_doc_set_viewport. */
    embi_style_env env = make_env(d);
    embi_style_engine_set_env(d->style, &env);
    d->env_dirty = false;

    /* W4i: the load's layout sees the index of this epoch; an index that arrives during it is still detected by the next
     * update (the epoch moves on), while an index that is already ready costs no second layout. */
    d->font_epoch = embi_fonts_sys_epoch(d->fonts);
    uint64_t fb0 = embi_fonts_fallback_uses(d->fonts);
    t0 = SDL_GetPerformanceCounter();
    embi_style_resolve_document(d->style, dom);
    L->tm.style_ms = ticks_ms_since(t0);

    t0 = SDL_GetPerformanceCounter();
    embi_layout_set_viewport(d->eng, d->vw, d->vh);
    bool ok = embi_layout_document(d->eng, dom);
    L->tm.layout_ms = ticks_ms_since(t0);
    d->layout_dirty = false;
    d->font_fallback = d->font_fallback || embi_fonts_fallback_uses(d->fonts) != fb0;
    L->ok = ok;
    L->active = false;
    /* DOMContentLoaded and load are queued now that the document is laid out (they run from the loop, after this returns). */
    embi_script_finish_events(sc);
    return LD_DONE;
}

embi_doc_load_status embi_doc_load_begin(embi_doc *d, embi_str html, embi_str base_url, embi_doc_times *t)
{
    load_abort(d); /* a load that is suspended ends: its parser and its input go before the DOM they belong to */
    /* The old document's realm and its pending scripts and timers go before its DOM does. */
    embi_script_reset(d->script);
    embi_script_hint_markup(d->script, html);
    embi_animator_reset(d->anim);
    drop_document(d);
    if (d->dom != nullptr) {
        embi_style_forget_document(d->style, d->dom);
        embi_document_free(d->dom);
        d->dom = nullptr;
    }
    scache_free(d);
    /* The old document's sheets are not the new one's (the load collects its own). */
    release_sheets(d->author, d->nauthor);
    d->author = nullptr;
    d->nauthor = 0;
    release_sheets(d->adopted, d->nadopted);
    d->adopted = nullptr;
    d->nadopted = 0;
    /* The old resolved URLs and the old layout's runs are gone with the old document. */
    embi_arena_free(&d->arena);
    embi_arena_init(&d->arena, 4096);
    embi_hashmap_clear(&d->rmap);
    set_base(d, base_url);

    embi_document *dom = embi_document_new(true);
    /* The document is set before the parse: a script that runs during it sees the part of the DOM built so far. */
    d->dom = dom;
    d->sheets_stale = true; /* a script of the parse that reads styles gets the sheets of the part built so far */
    d->in_load = true;
    observe_dom(d, dom);
    embi_html_parser_config pcfg = {.scripting = !d->no_script,
                                    .on_script = d->no_script ? nullptr : embi_script_on_script,
                                    .script_ctx = d->script};
    struct doc_load *L = &d->ld;
    L->active = true;
    L->phase = LP_PARSE;
    L->parser = embi_html_parser_new(dom, &pcfg);
    L->src = html.ptr;
    L->len = html.len;

    int st = load_drive(d);
    if (st == LD_WAIT) {
        return EMBI_DOC_LOAD_SUSPENDED;
    }
    if (st == LD_GONE) {
        if (t != nullptr) {
            memset(t, 0, sizeof *t);
        }
        return EMBI_DOC_LOAD_GONE; /* a script started another load, which has its own result */
    }
    if (t != nullptr) {
        *t = d->ld.tm;
    }
    bool ok = d->ld.ok;
    SDL_free(d->ld.copy);
    memset(&d->ld, 0, sizeof d->ld);
    sheet_urls_clear(d); /* the load is over: nothing waits for a sheet any more */
    return ok ? EMBI_DOC_LOAD_DONE : EMBI_DOC_LOAD_FAILED;
}

bool embi_doc_load(embi_doc *d, embi_str html, embi_str base_url, embi_doc_times *t)
{
    return embi_doc_load_begin(d, html, base_url, t) != EMBI_DOC_LOAD_FAILED;
}

/* A resource the load may be waiting for arrived (or failed): the drive tries again from where it stopped. */
static void load_resume(embi_doc *d)
{
    if (!d->ld.active || d->ld.busy) {
        return; /* no load, or the parser is on the stack (a script pumped the loop): the drive goes on by itself */
    }
    int st = load_drive(d);
    if (st != LD_DONE) {
        return;
    }
    embi_doc_times tm = d->ld.tm;
    bool ok = d->ld.ok;
    SDL_free(d->ld.copy);
    memset(&d->ld, 0, sizeof d->ld);
    sheet_urls_clear(d); /* the load is over: nothing waits for a sheet any more */
    if (d->load_done != nullptr) {
        d->load_done(d->cb.user, &tm, ok);
    }
}

embi_animator *embi_doc_animator(embi_doc *d)
{
    return d->anim;
}

embi_style_engine *embi_doc_style_engine(embi_doc *d)
{
    return d->style;
}

embi_layout_engine *embi_doc_layout_engine(embi_doc *d)
{
    return d->eng;
}

bool embi_doc_body_has_load_handler(embi_doc *d)
{
    if (d == nullptr || d->dom == nullptr) {
        return false;
    }
    embi_element *body = find_named(d->dom, "body");
    embi_sb tmp = {0};
    bool present = body != nullptr && embi_doc_el_attr(body, "onload", &tmp);
    embi_sb_free(&tmp);
    return present;
}

void embi_doc_image_failed(embi_doc *d, embi_str url)
{
    if (d != nullptr && d->dom != nullptr && !d->no_script) {
        embi_script_image_event(d->script, url, false);
    }
}

/* text/text.h (the face registry) does not mix with the layout headers here: its one function this file needs. */
uint32_t embi_fontdb_face_count(const embi_fontdb *db);

void embi_doc_net_arrived(embi_doc *d, unsigned kinds, embi_str url, bool ok)
{
    if (d == nullptr || d->dom == nullptr) {
        return;
    }
    if (!ok) {
        kinds &= ~(unsigned)EMBI_NL_SHEET; /* a sheet that failed changes nothing on the page (a failed font: the face count below) */
    }
    bool skip_layout = false;
    if ((kinds & EMBI_NL_FONT) != 0) { /* the sheets whose @font-face sources were pending register their faces again */
        uint32_t faces = embi_fontdb_face_count(d->fonts);
        apply_sheets(d);
        /* The script bit is on every record (a script may wait for it): only the other kinds say what the page needs. A source
         * that failed and left no other face in its place changes nothing on the page, but waiters still hear of it. */
        skip_layout = !ok && (kinds & ~(unsigned)EMBI_NL_SCRIPT) == EMBI_NL_FONT && embi_fontdb_face_count(d->fonts) == faces;
    }
    if ((kinds & EMBI_NL_SHEET) != 0) {
        d->sheets_stale = true;
    }
    const uint32_t epoch = d->load_epoch;
    /* A load that is suspended has no layout yet: it lays out everything when it finishes (the runs cached by style are still
     * flushed when a font arrives, they may have been made without it for the previous document). */
    if (!skip_layout && (kinds & (EMBI_NL_IMAGE | EMBI_NL_FONT | EMBI_NL_SHEET)) != 0 &&
        (!d->ld.active || (kinds & EMBI_NL_FONT) != 0)) {
        /* Nothing in the DOM or the styles changed, so the incremental layout would keep its fragments: it starts over. */
        d->layout_dirty = true;
        if ((kinds & EMBI_NL_FONT) != 0) {
            embi_layout_flush_text(d->eng, d->dom);
        } else {
            embi_layout_invalidate_all(d->eng, d->dom);
        }
    }
    /* After the invalidation, so an onload handler reads the geometry with the image in. */
    if ((kinds & EMBI_NL_IMAGE) != 0 && !d->no_script) {
        embi_script_image_event(d->script, url, ok);
    }
    /* The scripts and module requests that waited for it, then the load itself. A script may have replaced the document. */
    if (d->load_epoch == epoch && d->script != nullptr) {
        embi_script_net_arrived(d->script, url);
    }
    if (d->load_epoch == epoch) {
        load_resume(d);
    }
}

void embi_doc_recheck_gate(embi_doc *d)
{
    if (d != nullptr && d->script != nullptr && d->dom != nullptr) {
        embi_script_recheck_gate(d->script);
    }
}

void embi_doc_invalidate(embi_doc *d)
{
    if (d != nullptr) {
        d->layout_dirty = true; /* the style engine restyles with the layout (a full restyle until W6b) */
    }
}

bool embi_doc_update(embi_doc *d, embi_doc_times *t)
{
    /* Due tasks and timers run first: a script may change the document, which the update below then lays out. Only a
     * change to the DOM (the observer of embi_doc_load) makes the layout dirty: a timer that touches nothing costs no
     * style, layout or paint. */
    (void)embi_script_pump(d->script);
    embi_doc_sync_sheets(d); /* W6e: a <style> or <link> inserted, removed or edited since the last update */
    /* W4i: when the system font index arrives (or is rebuilt), a document whose layout used a fallback (a family that was
     * not found, a code point from the bundled face) lays out again with the system faces. */
    uint64_t fepoch = embi_fonts_sys_epoch(d->fonts);
    if (fepoch != d->font_epoch) {
        d->font_epoch = fepoch;
        if (d->font_fallback) {
            d->layout_dirty = true;
            if (d->dom != nullptr) {
                embi_layout_invalidate_all(d->eng, d->dom); /* no style changed: the incremental layout would keep the fragments */
            }
            d->font_fallback = false;
        }
    }
    if (d->dom == nullptr || (!d->env_dirty && !d->layout_dirty)) {
        return false;
    }
    embi_doc_times tm;
    memset(&tm, 0, sizeof tm);
    if (d->env_dirty) {
        embi_style_env env = make_env(d);
        embi_style_engine_set_env(d->style, &env);
        embi_animator_set_env(d->anim, &env);
        d->env_dirty = false;
    }
    uint64_t t0 = SDL_GetPerformanceCounter();
    /* Layouts after the load (script-built DOM, timers, fetches, resizes) count too: the flag stays set until the next
     * index change restyles, because an incremental layout keeps fragments that used a fallback earlier. */
    uint64_t fb0 = embi_fonts_fallback_uses(d->fonts);
    embi_style_resolve_document(d->style, d->dom);
    tm.style_ms = (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
    t0 = SDL_GetPerformanceCounter();
    embi_layout_set_viewport(d->eng, d->vw, d->vh);
    bool laid = embi_layout_document(d->eng, d->dom);
    tm.layout_ms = (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
    d->layout_dirty = false;
    d->font_fallback = d->font_fallback || embi_fonts_fallback_uses(d->fonts) != fb0;
    if (t != nullptr) {
        *t = tm;
    }
    /* W6b: an update that kept the fragments (nothing changed, or only paint did) does not invalidate the display list on
     * screen; the paint-only one asks for a (partial) repaint. */
    embi_layout_incr_stats st;
    embi_layout_incr_stats_get(d->eng, d->dom, &st);
    if (!laid) {
        return false; /* no layout ran in this call: the kind of the last one says nothing about it */
    }
    d->paint_pending = d->paint_pending || st.last_kind == EMBI_LU_PAINT;
    return st.last_kind == EMBI_LU_SURGERY || st.last_kind == EMBI_LU_FULL;
}

bool embi_doc_take_paint_pending(embi_doc *d)
{
    bool v = d->paint_pending;
    d->paint_pending = false;
    return v;
}

uint64_t embi_doc_layout_serial(embi_doc *d)
{
    if (d == nullptr || d->dom == nullptr || d->eng == nullptr) {
        return 0;
    }
    uint64_t serial = 0;
    embi_element *const *els = nullptr;
    uint32_t n = 0;
    (void)embi_layout_paint_dirty(d->eng, d->dom, &serial, &els, &n);
    return serial;
}

uint64_t embi_doc_run_epoch(embi_doc *d)
{
    if (d == nullptr || d->dom == nullptr || d->eng == nullptr) {
        return 0;
    }
    return embi_layout_run_epoch(d->eng, d->dom);
}

/* ------------------------------------------------------------------ canvas */

static bool kw_is(const embi_value *v, uint16_t kw)
{
    return v->kind == EMBI_V_KEYWORD && v->kw == kw;
}

/* Whether the root element's used color scheme is dark: the environment prefers dark and the root's color-scheme lists
 * dark (the rule of the style engine's embi_style_scheme_dark, applied to the root). */
static bool root_scheme_dark(const embi_doc *d)
{
    const embi_style *s = d->dom != nullptr ? root_style(d) : nullptr;
    const embi_value *v = s != nullptr ? embi_style_value(s, EMBI_PROP_color_scheme) : nullptr;
    if (v == nullptr) {
        return false;
    }
    bool has_dark = false, has_light = false, only = false;
    if (v->kind == EMBI_V_KEYWORD) {
        has_dark = v->kw == EMBI_KW_dark;
        has_light = v->kw == EMBI_KW_light;
        only = v->kw == EMBI_KW_only;
    } else if (v->kind == EMBI_V_LIST) {
        for (uint32_t i = 0; i < v->list.n; i++) {
            has_dark = has_dark || kw_is(&v->list.items[i], EMBI_KW_dark);
            has_light = has_light || kw_is(&v->list.items[i], EMBI_KW_light);
            only = only || kw_is(&v->list.items[i], EMBI_KW_only);
        }
    }
    if (d->dark) {
        return has_dark;
    }
    return has_dark && !has_light && !only;
}

uint32_t embi_doc_canvas_rgba(const embi_doc *d)
{
    /* CSS Color Adjust: the canvas is the Canvas system color of the root element's used scheme. */
    uint16_t idx = 0;
    embi_color c, srgb;
    uint8_t px[4];
    if (root_scheme_dark(d) && embi_color_system(EMBI_STR("Canvas"), &idx) &&
        embi_color_system_value(idx, true, nullptr, &c) && embi_color_to_srgb(&c, &srgb) &&
        embi_color_to_srgb8(&srgb, px)) {
        return 0xFF000000u | ((uint32_t)px[2] << 16) | ((uint32_t)px[1] << 8) | (uint32_t)px[0];
    }
    return 0xFFFFFFFFu;
}

/* ------------------------------------------------------------------ paint */

double embi_doc_paint(embi_doc *d, embi_dl *out, float vw, float vh, uint32_t focus_rgba)
{
    return embi_doc_paint_ex(d, out, vw, vh, focus_rgba, false, false);
}

static void paint_cfg(embi_doc *d, embi_paint_cfg *cfg, float vw, float vh, uint32_t focus_rgba)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->user = d;
    cfg->image_size = image_size_cb;
    cfg->focus_rgba = focus_rgba != 0 ? focus_rgba : EMBI_DOC_FOCUS_DEFAULT;
    cfg->viewport_w = vw;
    cfg->viewport_h = vh;
    cfg->scale = d->dppx;
}

double embi_doc_paint_ex(embi_doc *d, embi_dl *out, float vw, float vh, uint32_t focus_rgba, bool cached, bool incremental)
{
    uint64_t t0 = SDL_GetPerformanceCounter();
    if (d->dom == nullptr) {
        embi_dl_reset(out);
    } else {
        embi_paint_cfg cfg;
        paint_cfg(d, &cfg, vw, vh, focus_rgba);
        if (cached) {
            if (d->pcache == nullptr) {
                d->pcache = embi_paint_cache_create();
            }
            embi_paint_document_incr(d->pcache, out, d->eng, d->dom, &cfg, incremental);
        } else {
            embi_paint_document(out, d->eng, d->dom, &cfg);
        }
    }
    return (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
}

int embi_doc_paint_scroll(embi_doc *d, embi_dl *list, uint32_t content_len, float vw, float vh, uint32_t focus_rgba, embi_scroll_damage *dmg, double *ms)
{
    uint64_t t0 = SDL_GetPerformanceCounter();
    int r = 0;
    if (d->dom != nullptr && d->pcache != nullptr) {
        embi_paint_cfg cfg;
        paint_cfg(d, &cfg, vw, vh, focus_rgba);
        r = embi_paint_document_scroll(d->pcache, list, content_len, d->eng, d->dom, &cfg, dmg);
    }
    *ms = (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
    return r;
}


/* ------------------------------------------------------------------ form controls (W5e) */

bool embi_doc_control_offset(const embi_doc *d, const embi_element *el, const float bb[4], float x, float y, uint32_t *byte)
{
    return d->dom != nullptr && embi_paint_control_offset(d->eng, el, bb, x, y, byte);
}

bool embi_doc_control_caret(const embi_doc *d, const embi_element *el, const float bb[4], float rect[4])
{
    return d->dom != nullptr && embi_paint_control_caret(d->eng, el, bb, rect);
}

/* ------------------------------------------------------------------ scrollers */

const void *embi_doc_root_key(const embi_doc *d)
{
    return d;
}

static embi_frag *root_frag(const embi_doc *d)
{
    if (d->dom == nullptr) {
        return nullptr;
    }
    return (embi_frag *)embi_layout_root(d->eng, d->dom);
}

/* The origin of a child fragment: the paint origin of its parent, plus its own position, less the parent's scroll
 * offset (paint.c enter() shifts the content of a scroller that way). The root's offset shifts the whole page the same
 * way, except a fixed box of the viewport, which paint keeps still, so the origins are paint coordinates, which are
 * viewport coordinates for the root. */
static void child_origin(const embi_frag *k, float ox, float oy, float *kx, float *ky)
{
    const embi_frag *p = k->parent;
    bool still = embi_lay_frag_vp_fixed(k);
    float sx = p != nullptr && !still ? embi_frag_sx(p) : 0.0f;
    float sy = p != nullptr && !still ? embi_frag_sy(p) : 0.0f;
    *kx = ox + k->x - sx;
    *ky = oy + k->y - sy;
}

/* A scroll container found by a walk: its fragment, its key (the element), its origin in paint coordinates, and the
 * rectangle of the point hit test (its own border box cut by the scroll containers around it, whose content clips it). */
typedef struct scroller {
    embi_frag *f;
    const void *key;
    float x, y;
    float hit[4]; /* x0, y0, x1, y1 */
    float off[2]; /* the scroll offset the position was computed with */
} scroller;

typedef struct scrollers {
    scroller *v; /* SDL_malloc'd, grown as needed */
    uint32_t n, cap;
} scrollers;

static void scrollers_push(scrollers *out, scroller s)
{
    if (out->n == out->cap) {
        uint32_t cap = out->cap ? out->cap * 2 : 16;
        scroller *v = SDL_realloc(out->v, (size_t)cap * sizeof *v);
        EMBI_ASSERT(v != nullptr);
        out->v = v;
        out->cap = cap;
    }
    out->v[out->n++] = s;
}

/* Collects every scroll container below f (pre-order: a container comes before the ones nested in it). clip is the
 * rectangle the content of f is visible in (the box of the nearest scroll container above, cut by its own ancestors),
 * so a container scrolled out of view of its parent cannot take a wheel over the parent's other content. Transforms
 * are not applied, so a scroller inside one is found where it would be without it. */
static void collect_scrollers(scrollers *out, embi_frag *f, float ox, float oy, const float clip[4])
{
    for (embi_frag *k = embi_frag_first(f); k != nullptr; k = k->next) {
        float kx = 0.0f, ky = 0.0f;
        child_origin(k, ox, oy, &kx, &ky);
        float inner[4] = {clip[0], clip[1], clip[2], clip[3]};
        if (k->kind == EMBI_FK_BOX && (k->flags & EMBI_FF_SCROLL) && k->box != nullptr && k->box->el != nullptr) {
            scroller s = {k, k->box->el, kx, ky, {kx, ky, kx + k->w, ky + k->h}, {embi_frag_sx(k), embi_frag_sy(k)}};
            for (int i = 0; i < 2; i++) {
                s.hit[i] = s.hit[i] > clip[i] ? s.hit[i] : clip[i];
                s.hit[i + 2] = s.hit[i + 2] < clip[i + 2] ? s.hit[i + 2] : clip[i + 2];
            }
            inner[0] = s.hit[0];
            inner[1] = s.hit[1];
            inner[2] = s.hit[2];
            inner[3] = s.hit[3];
            scrollers_push(out, s);
        }
        collect_scrollers(out, k, kx, ky, inner);
    }
}

/* The scrollers of the document (the viewport is not among them: its key is the document). */
static void scrollers_of(const embi_doc *d, scrollers *out)
{
    out->n = 0;
    embi_frag *root = root_frag(d);
    if (root != nullptr) {
        const float all[4] = {-1.0e30f, -1.0e30f, 1.0e30f, 1.0e30f};
        collect_scrollers(out, root, 0.0f, 0.0f, all);
    }
}

/* The scroller with key, or nullptr. */
static const scroller *find_scroller(const scrollers *s, const void *key)
{
    for (uint32_t i = 0; i < s->n; i++) {
        if (s->v[i].key == key) {
            return &s->v[i];
        }
    }
    return nullptr;
}

static bool point_in(const scroller *s, float x, float y)
{
    return x >= s->hit[0] && y >= s->hit[1] && x < s->hit[2] && y < s->hit[3];
}

/* The style of the document element, or nullptr. */
static const embi_style *root_style(const embi_doc *d)
{
    embi_element *el = embi_document_element(d->dom);
    return el != nullptr ? embi_style_get(d->style, el, EMBI_PE_NONE) : nullptr;
}

/* The scrollers of the document, from the cache the document keeps: the tree is walked again only when a layout ran since,
 * or the scroll offset of the root or of a cached scroller changed (a scroll moves the ones nested in it). The list belongs
 * to the document: it stays valid until the next call of this function, and is not freed by the caller. */
static scrollers *scrollers_new(const embi_doc *dc)
{
    embi_doc *d = (embi_doc *)(uintptr_t)dc;
    embi_frag *root = root_frag(d);
    uint64_t calls = embi_layout_calls(d->eng);
    float rx = root != nullptr ? embi_frag_sx(root) : 0.0f, ry = root != nullptr ? embi_frag_sy(root) : 0.0f;
    scrollers *list = d->sc_cache;
    bool ok = list != nullptr && d->sc_calls == calls && d->sc_root == root && d->sc_root_off[0] == rx && d->sc_root_off[1] == ry;
    for (uint32_t i = 0; ok && i < list->n; i++) {
        ok = embi_frag_sx(list->v[i].f) == list->v[i].off[0] && embi_frag_sy(list->v[i].f) == list->v[i].off[1];
    }
    if (ok) {
        return list;
    }
    if (list == nullptr) {
        list = SDL_calloc(1, sizeof *list);
        EMBI_ASSERT(list != nullptr);
        d->sc_cache = list;
    }
    scrollers_of(d, list);
    d->sc_calls = calls;
    d->sc_root = root;
    d->sc_root_off[0] = rx;
    d->sc_root_off[1] = ry;
    return list;
}

static void scrollers_cache_free(embi_doc *d)
{
    if (d->sc_cache != nullptr) {
        SDL_free(d->sc_cache->v);
        SDL_free(d->sc_cache);
        d->sc_cache = nullptr;
    }
}

static void scrollers_delete(scrollers *list)
{
    (void)list; /* the document's cache */
}

uint32_t embi_doc_emit_scrollbars(embi_doc *d, embi_dl *out, const embi_doc_sb *sbs, uint32_t n)
{
    if (d->dom == nullptr) {
        return 0;
    }
    /* Scrollers with a ::-webkit-scrollbar style have classic bars that show whenever they overflow (below); the others
     * have the overlay bars of the tracks (which fade). */
    bool styled = embi_style_uses_pseudo(d->style, d->dom, EMBI_PE_SCROLLBAR);
    if (n == 0 && !styled) {
        return 0;
    }
    uint32_t total = 0;
    embi_frag *root = root_frag(d);
    scrollers *list = scrollers_new(d);
    for (uint32_t i = 0; i < n; i++) {
        if (!(sbs[i].alpha > 0.0f)) {
            continue;
        }
        if (sbs[i].key == embi_doc_root_key(d)) {
            if (root == nullptr) {
                continue;
            }
            embi_sb_style st = embi_sb_style_from(root_style(d));
            total += embi_sb_emit(out, root, 0.0f, 0.0f, &st, sbs[i].alpha);
            continue;
        }
        const scroller *s = find_scroller(list, sbs[i].key);
        if (s == nullptr) {
            continue;
        }
        if (styled) {
            if (embi_style_generated(d->style, (embi_element *)(uintptr_t)s->key, EMBI_PE_SCROLLBAR) != nullptr) {
                continue; /* a styled or hidden bar: not an overlay one */
            }
        }
        const embi_style *es = embi_style_get(d->style, (embi_element *)s->key, EMBI_PE_NONE);
        embi_sb_style st = embi_sb_style_from(es);
        total += embi_sb_emit(out, s->f, s->x, s->y, &st, sbs[i].alpha);
    }
    if (styled) {
        for (uint32_t i = 0; i < list->n; i++) {
            const scroller *s = &list->v[i];
            int axis = d->sb_hover_key == s->key ? d->sb_hover_axis : 0;
            embi_sb_pe pe;
            if (embi_sb_pe_from(d->style, (embi_element *)(uintptr_t)s->key, axis, &pe) == EMBI_SBPE_STYLED) {
                total += embi_sb_emit_styled(out, s->f, s->x, s->y, &pe, s->hit);
            }
            embi_sb_pe_free(d->style, &pe);
        }
    }
    scrollers_delete(list);
    return total;
}

bool embi_doc_sbar_styled(embi_doc *d, const void *key)
{
    if (d->dom == nullptr || key == nullptr || key == embi_doc_root_key(d) ||
        !embi_style_uses_pseudo(d->style, d->dom, EMBI_PE_SCROLLBAR)) {
        return false;
    }
    embi_sb_pe pe;
    embi_sb_pe_kind kind = embi_sb_pe_from(d->style, (embi_element *)(uintptr_t)key, 0, &pe);
    embi_sb_pe_free(d->style, &pe);
    return kind != EMBI_SBPE_NONE;
}

/* Whether el is the element with key or inside it. */
static bool within_key(const embi_element *el, const void *key)
{
    for (const embi_node *n = el != nullptr ? &el->base : nullptr; n != nullptr; n = n->parent) {
        if ((const void *)n == key) {
            return true;
        }
    }
    return false;
}

bool embi_doc_sbar_at(embi_doc *d, float x, float y, const embi_element *hit, embi_doc_sbar *out)
{
    if (hit == nullptr) {
        return false;
    }
    if (d->dom == nullptr || !embi_style_uses_pseudo(d->style, d->dom, EMBI_PE_SCROLLBAR)) {
        return false;
    }
    scrollers *list = scrollers_new(d);
    bool found = false;
    for (uint32_t i = list->n; i-- > 0 && !found;) {
        const scroller *s = &list->v[i];
        if (!point_in(s, x, y) || !within_key(hit, s->key)) {
            continue; /* not there, or something else (a popup, a fixed box) is on top of the scroller at the point */
        }
        embi_sb_pe pe;
        embi_sb_geo g;
        if (embi_sb_pe_from(d->style, (embi_element *)(uintptr_t)s->key, 0, &pe) == EMBI_SBPE_STYLED &&
            embi_sb_geometry(s->f, s->x, s->y, &pe, &g)) {
            for (int axis = 0; axis < 2 && !found; axis++) {
                bool vertical = axis == 0;
                const float *bar = vertical ? g.v : g.h;
                if (!(vertical ? g.vert : g.horz) || x < bar[0] || x >= bar[0] + bar[2] || y < bar[1] || y >= bar[1] + bar[3]) {
                    continue;
                }
                float pos = vertical ? y : x;
                float t0 = vertical ? bar[1] : bar[0];
                float start = vertical ? g.vstart : g.hstart, len = vertical ? g.vlen : g.hlen;
                memset(out, 0, sizeof *out);
                out->key = s->key;
                out->vertical = vertical;
                out->track_start = t0;
                out->track_len = vertical ? g.vtrack : g.htrack;
                out->thumb_start = t0 + start;
                out->thumb_len = len;
                out->part = pos < t0 + start ? 2 : (pos >= t0 + start + len ? 3 : 1);
                found = true;
            }
        }
        embi_sb_pe_free(d->style, &pe);
    }
    scrollers_delete(list);
    return found;
}

bool embi_doc_sbar_drag(embi_doc *d, const void *key, bool vertical, float thumb_start, float vw, float vh)
{
    if (d->dom == nullptr) {
        return false;
    }
    scrollers *list = scrollers_new(d);
    const scroller *s = find_scroller(list, key);
    bool moved = false;
    if (s != nullptr) {
        embi_sb_pe pe;
        embi_sb_geo g;
        if (embi_sb_pe_from(d->style, (embi_element *)(uintptr_t)key, 0, &pe) == EMBI_SBPE_STYLED &&
            embi_sb_geometry(s->f, s->x, s->y, &pe, &g) && (vertical ? g.vert : g.horz)) {
            float travel = (vertical ? g.vtrack - g.vlen : g.htrack - g.hlen);
            float pos = thumb_start - (vertical ? g.v[1] : g.h[0]);
            float frac = travel > 0.0f ? pos / travel : 0.0f;
            frac = frac < 0.0f ? 0.0f : (frac > 1.0f ? 1.0f : frac);
            float target = frac * (vertical ? g.vmax : g.hmax);
            float cur = vertical ? embi_frag_sy(s->f) : embi_frag_sx(s->f);
            moved = embi_paint_scroll_by(d->eng, d->dom, s->f, vertical ? 0.0f : target - cur, vertical ? target - cur : 0.0f,
                                         false, vw, vh);
        }
        embi_sb_pe_free(d->style, &pe);
    }
    scrollers_delete(list);
    return moved;
}

bool embi_doc_sbar_page(embi_doc *d, const void *key, bool vertical, int dir, float vw, float vh)
{
    if (d->dom == nullptr) {
        return false;
    }
    scrollers *list = scrollers_new(d);
    const scroller *s = find_scroller(list, key);
    bool moved = false;
    if (s != nullptr) {
        /* a page is most of the visible part of the content (Chromium scrolls 7/8 of it) */
        float client = vertical ? s->f->h - embi_frag_bdr(s->f, 1) - embi_frag_bdr(s->f, 3) - embi_frag_scr(s->f, 1)
                                : s->f->w - embi_frag_bdr(s->f, 0) - embi_frag_bdr(s->f, 2) - embi_frag_scr(s->f, 0);
        float step = (float)dir * client * 0.875f;
        moved = embi_paint_scroll_by(d->eng, d->dom, s->f, vertical ? 0.0f : step, vertical ? step : 0.0f, false, vw, vh);
    }
    scrollers_delete(list);
    return moved;
}

bool embi_doc_sbar_set_hover(embi_doc *d, const void *key, int axis)
{
    if (axis == 0) {
        key = nullptr;
    }
    bool changed = d->sb_hover_key != key || d->sb_hover_axis != axis;
    d->sb_hover_key = key;
    d->sb_hover_axis = axis;
    return changed;
}

bool embi_doc_scroller_overflows(embi_doc *d, const void *key, float vw, float vh)
{
    if (d->dom == nullptr) {
        return false;
    }
    float mx = 0.0f, my = 0.0f;
    if (key == embi_doc_root_key(d)) {
        embi_scroll_max_root(root_frag(d), vw, vh, &mx, &my);
        return mx > 0.0f || my > 0.0f;
    }
    scrollers *list = scrollers_new(d);
    const scroller *s = find_scroller(list, key);
    if (s != nullptr) {
        embi_scroll_max(s->f, &mx, &my);
    }
    scrollers_delete(list);
    return mx > 0.0f || my > 0.0f;
}

/* ------------------------------------------------------------------ scroll state */

void embi_doc_root_offset(const embi_doc *d, float *x, float *y)
{
    embi_frag *root = root_frag(d);
    *x = root != nullptr ? embi_frag_sx(root) : 0.0f;
    *y = root != nullptr ? embi_frag_sy(root) : 0.0f;
}

bool embi_doc_scroller_offset(embi_doc *d, const void *key, float *x, float *y)
{
    if (key == embi_doc_root_key(d)) {
        embi_doc_root_offset(d, x, y);
        return true;
    }
    if (d->dom == nullptr) {
        return false;
    }
    scrollers *list = scrollers_new(d);
    const scroller *s = find_scroller(list, key);
    if (s != nullptr) {
        *x = embi_frag_sx(s->f);
        *y = embi_frag_sy(s->f);
    }
    bool found = s != nullptr;
    scrollers_delete(list);
    return found;
}

bool embi_doc_scroll_root_by(embi_doc *d, float dx, float dy, float vw, float vh)
{
    embi_frag *root = root_frag(d);
    if (root == nullptr) {
        return false;
    }
    return embi_paint_scroll_by(d->eng, d->dom, root, dx, dy, true, vw, vh);
}

bool embi_doc_scroll_root_to(embi_doc *d, float x, float y, float vw, float vh)
{
    float cx = 0.0f, cy = 0.0f;
    embi_doc_root_offset(d, &cx, &cy);
    return embi_doc_scroll_root_by(d, x - cx, y - cy, vw, vh);
}

bool embi_doc_root_smooth(embi_doc *d)
{
    if (d->dom == nullptr) {
        return false;
    }
    const embi_style *s = root_style(d);
    return s != nullptr && embi_scroll_is_smooth(s);
}

bool embi_doc_scroll_key_by(embi_doc *d, const void *key, float dx, float dy, float vw, float vh)
{
    if (key == embi_doc_root_key(d)) {
        return embi_doc_scroll_root_by(d, dx, dy, vw, vh);
    }
    if (d->dom == nullptr) {
        return false;
    }
    scrollers *list = scrollers_new(d);
    const scroller *s = find_scroller(list, key);
    bool changed = s != nullptr && embi_paint_scroll_by(d->eng, d->dom, s->f, dx, dy, false, vw, vh);
    scrollers_delete(list);
    return changed;
}

bool embi_doc_wheel(embi_doc *d, float x, float y, float dx, float dy, float vw, float vh, const void **key)
{
    if (d->dom == nullptr) {
        return false;
    }
    scrollers *list = scrollers_new(d);
    /* Containers that hold the point are tried from the innermost outwards (the last in pre-order is the innermost);
     * the first one that can move takes the wheel. The viewport is the last resort. */
    bool moved = false;
    for (uint32_t i = list->n; i-- > 0 && !moved;) {
        const scroller *s = &list->v[i];
        if (point_in(s, x, y) && embi_paint_scroll_by(d->eng, d->dom, s->f, dx, dy, false, vw, vh)) {
            *key = s->key;
            moved = true;
        }
    }
    scrollers_delete(list);
    if (!moved && embi_doc_scroll_root_by(d, dx, dy, vw, vh)) {
        *key = embi_doc_root_key(d);
        moved = true;
    }
    return moved;
}

void embi_doc_anim_start(embi_doc *d, embi_doc_anim *a, float x, float y, double now_ms, float vw, float vh)
{
    float cx = 0.0f, cy = 0.0f;
    embi_doc_root_offset(d, &cx, &cy);
    float mx = 0.0f, my = 0.0f;
    embi_frag *root = root_frag(d);
    if (root != nullptr) {
        embi_scroll_max_root(root, vw, vh, &mx, &my);
    }
    x = x < 0.0f ? 0.0f : (x > mx ? mx : x);
    y = y < 0.0f ? 0.0f : (y > my ? my : y);
    a->active = true;
    a->x0 = cx;
    a->y0 = cy;
    a->x1 = x;
    a->y1 = y;
    a->t0 = now_ms;
}

bool embi_doc_anim_step(embi_doc *d, embi_doc_anim *a, double now_ms, float vw, float vh)
{
    if (!a->active) {
        return false;
    }
    embi_scroll_anim s;
    s.x0 = a->x0;
    s.y0 = a->y0;
    s.x1 = a->x1;
    s.y1 = a->y1;
    s.t0 = a->t0;
    s.active = true;
    float x = a->x1, y = a->y1;
    bool running = embi_scroll_anim_sample(&s, now_ms, &x, &y);
    embi_doc_scroll_root_to(d, x, y, vw, vh);
    a->active = running;
    return running;
}

float embi_doc_fade_alpha(double last_ms, bool seen, double now_ms)
{
    embi_sb_fade f;
    f.last_ms = last_ms;
    f.seen = seen;
    return embi_sb_alpha(&f, now_ms);
}

/* ------------------------------------------------------------------ queries */

const char *embi_doc_title(const embi_doc *d)
{
    return d->title.data != nullptr ? d->title.data : "";
}

void embi_doc_counts(const embi_doc *d, uint32_t *boxes, uint32_t *frags)
{
    size_t bytes = 0;
    embi_layout_stats(d->eng, d->dom, boxes, frags, &bytes);
}

int embi_doc_group_mode(const embi_dl *d, uint32_t begin, uint32_t end)
{
    return (int)embi_layer_group_mode(d, begin, end);
}

/* ------------------------------------------------------------------ layout dump (debugging) */

static void dump_frag(const embi_frag *f, int depth)
{
    char tag[40] = "";
    if (f->box != nullptr && f->box->el != nullptr) {
        embi_str n = embi_atom_str(f->box->el->base.name);
        SDL_snprintf(tag, sizeof tag, "%.*s", (int)(n.len < 30 ? n.len : 30), n.ptr);
    }
    if (f->kind == EMBI_FK_TEXT) {
        SDL_Log("%*stext %s x=%.2f y=%.2f w=%.2f h=%.2f bytes[%u,%u)", depth * 2, "", tag, f->x, f->y, f->w, f->h,
                embi_frag_tbegin(f), embi_frag_tend(f));
        return;
    }
    SDL_Log("%*s%s %s x=%.2f y=%.2f w=%.2f h=%.2f bl=%.2f", depth * 2, "", f->kind == EMBI_FK_LINE ? "line" : "box", tag,
            f->x, f->y, f->w, f->h, embi_fl_chkc(f)->bl_first);
    for (const embi_frag *k = embi_frag_first(f); k != nullptr; k = k->next) {
        dump_frag(k, depth + 1);
    }
}

void embi_doc_dump_layout(const embi_doc *d)
{
    if (d != nullptr && d->dom != nullptr) {
        const embi_frag *r = embi_layout_root(d->eng, d->dom);
        if (r != nullptr) {
            dump_frag(r, 0);
        }
    }
}

/* ------------------------------------------------------------------ DOM changes made by scripts

 * Any change of the document's tree, attributes or character data (a script's work, or the parse itself) makes the layout
 * dirty. The style engine has its own observer for restyling; this one decides whether embi_doc_update has to run at all. */

/* W6e: whether the element is a <style> or a <link> (a link's rel is checked when the sheets are collected). */
static bool is_sheet_element(const embi_node *n)
{
    return n != nullptr && embi_is_element(n) &&
           (named(embi_as_element((embi_node *)(uintptr_t)n), "style") || named(embi_as_element((embi_node *)(uintptr_t)n), "link"));
}

/* Marks the sheets stale when the run of siblings a..b (or something below it) holds a <style> or a <link>. */
static void note_sheet_nodes(embi_doc *d, embi_node *a, embi_node *b)
{
    for (embi_node *n = a; n != nullptr && !d->sheets_stale; n = n == b ? nullptr : n->next) {
        for (embi_node *x = n; x != nullptr; x = embi_next_in_tree(n, x)) {
            if (is_sheet_element(x)) {
                d->sheets_stale = true;
                return;
            }
        }
    }
}

static void obs_children(void *ctx, embi_node *parent, embi_node *fa, embi_node *la, embi_node *fr, embi_node *lr)
{
    embi_doc *d = ctx;
    if (d->ld.busy && d->prefetch != nullptr && !embi_script_active(d->script)) {
        note_parser_links(d, fa, la); /* the parser built it, not a script */
    }
    if (!d->sheets_stale) {
        if (is_sheet_element(parent)) {
            d->sheets_stale = true;
        } else {
            note_sheet_nodes(d, fa, la);
            note_sheet_nodes(d, fr, lr);
        }
    }
    d->layout_dirty = true;
}

static void obs_attr(void *ctx, embi_element *el, uint8_t ns, embi_atom ns_uri, embi_atom local, embi_dstr old, bool had)
{
    (void)ns;
    (void)ns_uri;
    (void)local;
    (void)old;
    (void)had;
    embi_doc *d = ctx;
    if (!d->sheets_stale && is_sheet_element(&el->base)) {
        d->sheets_stale = true;
    }
    d->layout_dirty = true;
}

static void obs_chardata(void *ctx, embi_node *node)
{
    embi_doc *d = ctx;
    if (!d->sheets_stale && is_sheet_element(node->parent)) {
        d->sheets_stale = true;
    }
    d->layout_dirty = true;
}

static void observe_dom(embi_doc *d, embi_document *dom)
{
    embi_observer obs;
    memset(&obs, 0, sizeof obs);
    obs.ctx = d;
    obs.children_changed = obs_children;
    obs.attribute_changed = obs_attr;
    obs.character_data_changed = obs_chardata;
    embi_document_observe(dom, &obs);
}

/* ------------------------------------------------------------------ accessors for the script layer (script.c, W5c) */

embi_script *embi_doc_script(embi_doc *d)
{
    return d->script;
}

embi_document *embi_doc_dom(embi_doc *d)
{
    return d->dom;
}

embi_str embi_doc_url(embi_doc *d)
{
    if (d->base_len == 0) {
        return EMBI_STR("about:blank");
    }
    return (embi_str){d->base, d->base_len};
}

bool embi_doc_script_running(const embi_doc *d)
{
    return d != nullptr && embi_script_active(d->script);
}

void *embi_doc_owner(embi_doc *d)
{
    return d->cb.owner;
}

bool embi_doc_fetch_abs(embi_doc *d, embi_str url, uint8_t **data, size_t *len)
{
    if (d->fetch_script != nullptr) {
        return d->fetch_script(d->cb.user, url, data, len);
    }
    return d->cb.fetch != nullptr && d->cb.fetch(d->cb.user, url, data, len);
}

bool embi_doc_el_attr(embi_element *el, const char *name, embi_sb *out)
{
    return attr_text(el, name, out);
}

void embi_doc_el_text(embi_element *el, embi_sb *out)
{
    embi_sb_clear(out);
    child_text(&el->base, out);
}

bool embi_doc_next_due(embi_doc *d, double *due_ms)
{
    return embi_script_next_due(d->script, due_ms);
}
