/*
 * The style engine (W2d): creation and teardown, the initial style, document attachment and
 * dirty tracking (W1 observers), and the public entry points that the rest of the module shares.
 */
#include "style_int.h"
#include "inval.h"

#include <string.h>

/* ------------------------------------------------------------------ dirty tracking */

static void obs_children(void *ctx, embi_node *parent, embi_node *fa, embi_node *la,
                         embi_node *fr, embi_node *lr)
{
    embi_inval_children(ctx, parent, fa, la, fr, lr);
}

static void obs_attr(void *ctx, embi_element *el, uint8_t ns, embi_atom ns_uri, embi_atom local,
                     embi_dstr old, bool had)
{
    embi_inval_attr(ctx, el, ns, ns_uri, local, old, had);
}

static void obs_chardata(void *ctx, embi_node *node)
{
    embi_inval_chardata(ctx, node);
}

static void obs_inserted(void *ctx, embi_node *node)
{
    (void)node;
    ((embi_docstate *)ctx)->dirty = true;
}

static void obs_removed(void *ctx, embi_node *node)
{
    embi_docstate *ds = ctx;
    ds->dirty = true;
    /* The detached subtree keeps no styles: they are released now, so a removed element can never
     * leave a reference behind (it is restyled if it is inserted again). */
    embi_style_release_subtree(ds->eng, ds, node);
}

static embi_docstate *ds_get(embi_style_engine *e, embi_document *doc)
{
    uint64_t v = 0;
    if (embi_hashmap_get(&e->docs, (uint64_t)(uintptr_t)doc, &v)) {
        return (embi_docstate *)(uintptr_t)v;
    }
    embi_docstate *ds = embi_calloc(1, sizeof *ds);
    ds->eng = e;
    ds->doc = doc;
    ds->dirty = true;
    ds->full = true;
    ds->root_px = 16.0;
    embi_observer obs = {
        .ctx = ds,
        .children_changed = obs_children,
        .attribute_changed = obs_attr,
        .character_data_changed = obs_chardata,
        .node_inserted = obs_inserted,
        .node_removed = obs_removed,
    };
    embi_document_observe(doc, &obs);
    ds->observed = true;
    embi_hashmap_put(&e->docs, (uint64_t)(uintptr_t)doc, (uint64_t)(uintptr_t)ds);
    return ds;
}

/* ------------------------------------------------------------------ retained sheets */

/* The engine keeps one reference on every sheet it has been given (raw token slices point into
 * them), released in embi_style_engine_destroy. A sheet is added once however often it is set. */
static bool is_retained(const embi_style_engine *e, const embi_css_sheet *s)
{
    for (uint32_t i = 0; i < e->nretained; i++) {
        if (e->retained[i] == s) {
            return true;
        }
    }
    return false;
}

/* Records s in the retained list. owned: the caller's reference becomes the engine's. */
static void retain_sheet(embi_style_engine *e, embi_css_sheet *s, bool owned)
{
    if (s == nullptr) {
        return;
    }
    if (is_retained(e, s)) {
        if (owned) {
            embi_css_sheet_unref(s);
        }
        return;
    }
    if (!owned) {
        embi_css_sheet_ref(s);
    }
    if (e->nretained == e->capretained) {
        uint32_t cap = e->capretained == 0 ? 16u : e->capretained * 2u;
        e->retained = embi_realloc(e->retained, sizeof(embi_css_sheet *) * e->capretained,
                                   sizeof(embi_css_sheet *) * cap);
        e->capretained = cap;
    }
    e->retained[e->nretained++] = s;
}

/* ------------------------------------------------------------------ initial style */

static void build_initial(embi_style_engine *e)
{
    embi_sctx *sc = &e->sc;
    sc->eng = e;
    sc->parent = nullptr;
    sc->init = nullptr;
    sc->pcustom = nullptr;
    sc->ccustom = nullptr;
    sc->root_px = 16.0;
    sc->is_root = false;
    sc->ncands = 0;
    sc->ncw = 0;
    sc->unsafe = false;
    sc->nsets = 0;
    sc->dark = false;
    sc->gen++;
    sc->dirty = 0;
    sc->ntargets = 0;
    sc->wm = EMBI_KW_horizontal_tb;
    sc->dir = EMBI_KW_ltr;

    static const uint16_t first[] = {
        EMBI_PROP_writing_mode, EMBI_PROP_direction, EMBI_PROP_color_scheme, EMBI_PROP_font_size,
    };
    for (uint32_t i = 0; i < sizeof first / sizeof first[0]; i++) {
        uint16_t p = first[i];
        embi_style_set_ovr(sc, p, embi_style_finalize_value(sc, p, e->initial_raw[p]));
    }
    const embi_layout *L = embi_layout_get();
    for (uint32_t p = 1; p < EMBI_PROP_COUNT; p++) {
        if (L->gid[p] == EMBI_STYLE_NO_GROUP || embi_style_touched(sc, (uint16_t)p)) {
            continue;
        }
        embi_style_set_ovr(sc, (uint16_t)p,
                           embi_style_finalize_value(sc, (uint16_t)p, e->initial_raw[p]));
    }
    /* The used initial width (medium) before the none style of the initial style zeroes the
     * stored ones: an element whose style is set but whose width is not reads this value. */
    e->w_med = embi_style_finalize_value(sc, EMBI_PROP_border_top_width,
                                         e->initial_raw[EMBI_PROP_border_top_width]);
    embi_style_fix_display(sc);
    embi_style_fix_widths(sc);
    e->init = embi_style_finish_groups(e, sc);
    e->init_built = true;
}

static void init_initial_raw(embi_style_engine *e)
{
    const embi_layout *L = embi_layout_get();
    for (uint32_t p = 1; p < EMBI_PROP_COUNT; p++) {
        e->initial_raw[p] = nullptr;
        if (L->gid[p] == EMBI_STYLE_NO_GROUP) {
            continue;
        }
        embi_value tmp;
        if (embi_value_initial(&e->arena, (embi_prop_id)p, &tmp)) {
            e->initial_raw[p] = embi_style_canon(e, &tmp);
        }
    }
}

/* ------------------------------------------------------------------ rebuilding */

/* The UA set is flattened on first use (FP2b): nothing reads it before the first style resolve, so the engine's
 * creation does not wait for the shared sheet (which a thread of the runtime may still be parsing). */
static void flatten_ua(embi_style_engine *e)
{
    if (e->ua_sheet == nullptr) {
        e->ua_sheet = embi_style_ua_sheet(); /* the engine's reference (and one user of the shared sheet) */
        if (e->ua_sheet == nullptr) {        /* no sheet (embi_style_ua_sheet failed): the UA set stays empty */
            e->ua_ready = true;
            return;
        }
        e->ua_used = true;
        retain_sheet(e, e->ua_sheet, true);
    }
    embi_style_flatten_sheets(e, &e->ua, &e->ua_sheet, 1, EMBI_ORIGIN_UA, &e->env);
    e->ua_ready = true;
}

void embi_style_ua_ensure(embi_style_engine *e)
{
    if (!e->ua_ready) {
        flatten_ua(e);
    }
}

static void reflatten_all(embi_style_engine *e)
{
    if (e->ua_ready) {
        flatten_ua(e);
    } /* else the first use flattens it for the environment of that time */
    embi_style_flatten_sheets(e, &e->user, e->user_sheets, e->nuser_sheets, EMBI_ORIGIN_USER,
                              &e->env);
}

static void rebuild_initial(embi_style_engine *e)
{
    const embi_style *old = e->init;
    build_initial(e);
    embi_style_unref(e, old);
}

/* ------------------------------------------------------------------ public API */

static embi_style_env default_env(void)
{
    embi_style_env env;
    memset(&env, 0, sizeof env);
    env.width = 800;
    env.height = 600;
    env.dppx = 1.0;
    env.scripting = true;
    env.hover = true;
    env.pointer = 2;
    env.update = 2;
    env.gamut = 0;
    return env;
}

embi_style_engine *embi_style_engine_create(embi_fontdb *fonts, const embi_style_env *env,
                                            const embi_style_callbacks *cb)
{
    embi_style_engine_init_layout();
    embi_style_engine *e = embi_calloc(1, sizeof *e);
    e->fonts = fonts;
    e->env = env != nullptr ? *env : default_env();
    if (cb != nullptr) {
        e->cb = *cb;
    }
    embi_arena_init(&e->arena, 64u * 1024u);
    embi_arena_init(&e->vscratch, 4096u);
    e->own_change = EMBI_OWN_ALL;
    init_initial_raw(e);
    build_initial(e);
    return e;
}

static bool env_equal(const embi_style_env *a, const embi_style_env *b)
{
    return a->width == b->width && a->height == b->height && a->dppx == b->dppx &&
           a->dark == b->dark && a->reduced_motion == b->reduced_motion &&
           a->contrast == b->contrast && a->hover == b->hover && a->pointer == b->pointer &&
           a->scripting == b->scripting && a->forced_colors == b->forced_colors &&
           a->gamut == b->gamut && a->hdr == b->hdr && a->update == b->update &&
           a->palette_light == b->palette_light && a->palette_dark == b->palette_dark;
}

bool embi_style_uses_state(const embi_style_engine *e)
{
    embi_style_ua_ensure((embi_style_engine *)e); /* the UA sheet's :hover and :focus rules count */
    return e->uses_state;
}

void embi_style_set_state_idle(embi_style_engine *e, bool (*idle)(void *user))
{
    e->state_idle = idle;
}

void embi_style_set_sel_hooks(embi_style_engine *e, const embi_sel_hooks *hooks)
{
    e->hooks.user = hooks != nullptr ? hooks->user : nullptr;
    e->hooks.state = hooks != nullptr ? hooks->state : nullptr;
    embi_style_cache_clear(e);
    embi_hashmap_iter it = embi_hashmap_iter_begin(&e->docs);
    uint64_t k = 0;
    uint64_t v = 0;
    while (embi_hashmap_iter_next(&it, &k, &v)) {
        embi_docstate *ds = (embi_docstate *)(uintptr_t)v;
        ds->dirty = true;
        ds->full = true;
    }
}

void embi_style_engine_set_env(embi_style_engine *e, const embi_style_env *env)
{
    if (env_equal(&e->env, env)) {
        return; /* nothing a rule or a computed value reads has changed */
    }
    e->env = *env;
    embi_style_cache_clear(e);
    reflatten_all(e);
    rebuild_initial(e);
    embi_hashmap_iter it = embi_hashmap_iter_begin(&e->docs);
    uint64_t k = 0;
    uint64_t v = 0;
    while (embi_hashmap_iter_next(&it, &k, &v)) {
        embi_docstate *ds = (embi_docstate *)(uintptr_t)v;
        embi_style_flatten_sheets(e, &ds->author, ds->sheets, ds->nsheets, EMBI_ORIGIN_AUTHOR,
                                  &e->env);
        ds->dirty = true;
        ds->full = true;
        ds->iv.valid = false;
    }
    e->iv.valid = false;
}

void embi_style_engine_set_user_sheets(embi_style_engine *e, embi_css_sheet *const *sheets,
                                       uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        retain_sheet(e, sheets[i], false);
    }
    embi_style_cache_clear(e);
    if (e->user_sheets != nullptr) {
        embi_free(e->user_sheets, sizeof(embi_css_sheet *) * (e->nuser_sheets > 0 ? e->nuser_sheets : 1));
    }
    e->user_sheets = embi_alloc(sizeof(embi_css_sheet *) * (n > 0 ? n : 1));
    if (n) {
        memcpy(e->user_sheets, sheets, sizeof(embi_css_sheet *) * n);
    }
    e->nuser_sheets = n;
    embi_style_flatten_sheets(e, &e->user, e->user_sheets, e->nuser_sheets, EMBI_ORIGIN_USER,
                              &e->env);
    embi_hashmap_iter it = embi_hashmap_iter_begin(&e->docs);
    uint64_t k = 0;
    uint64_t v = 0;
    while (embi_hashmap_iter_next(&it, &k, &v)) {
        ((embi_docstate *)(uintptr_t)v)->dirty = true;
        ((embi_docstate *)(uintptr_t)v)->full = true;
    }
    e->iv.valid = false;
}

void embi_style_font_defer(embi_style_engine *e)
{
    e->font_pending = true;
}

void embi_style_engine_set_font_base(embi_style_engine *e, embi_str base)
{
    e->font_base = embi_style_copy_str(&e->arena, base);
}

void embi_document_set_author_sheets(embi_style_engine *e, embi_document *doc,
                                     embi_css_sheet *const *sheets, uint32_t n)
{
    embi_docstate *ds = ds_get(e, doc);
    for (uint32_t i = 0; i < n; i++) {
        retain_sheet(e, sheets[i], false);
    }
    embi_style_cache_clear(e);
    if (ds->sheets != nullptr) {
        embi_free(ds->sheets, sizeof(embi_css_sheet *) * (ds->nsheets > 0 ? ds->nsheets : 1));
    }
    ds->sheets = embi_alloc(sizeof(embi_css_sheet *) * (n > 0 ? n : 1));
    if (n) {
        memcpy(ds->sheets, sheets, sizeof(embi_css_sheet *) * n);
    }
    ds->nsheets = n;
    embi_style_flatten_sheets(e, &ds->author, ds->sheets, ds->nsheets, EMBI_ORIGIN_AUTHOR,
                              &e->env);
    embi_style_register_fonts(e, ds->sheets, ds->nsheets, &e->env);
    ds->dirty = true;
    ds->full = true;
    ds->iv.valid = false;
}

void embi_style_forget_document(embi_style_engine *e, embi_document *doc)
{
    uint64_t v = 0;
    if (!embi_hashmap_get(&e->docs, (uint64_t)(uintptr_t)doc, &v)) {
        return;
    }
    embi_docstate *ds = (embi_docstate *)(uintptr_t)v;
    embi_style_forget_document_walk(e, doc);
    embi_style_cache_clear(e);
    if (ds->observed) {
        embi_document_unobserve(doc, ds);
    }
    embi_hashmap_remove(&e->docs, (uint64_t)(uintptr_t)doc);
    embi_style_sset_free(&ds->author);
    if (ds->sheets != nullptr) {
        embi_free(ds->sheets, sizeof(embi_css_sheet *) * (ds->nsheets > 0 ? ds->nsheets : 1));
    }
    embi_hashmap_free(&ds->pseudo);
    embi_inval_ds_free(e, ds);
    embi_free(ds, sizeof *ds);
}

/* Drops every sharing-cache reference. The cache is keyed by identity only, so it is cleared
 * whenever an input outside the key changes (sheets, environment, a document's styles). */
void embi_style_cache_clear(embi_style_engine *e)
{
    for (uint32_t i = 0; i < EMBI_STYLE_CACHE_SLOTS; i++) {
        if (e->cache[i].s != nullptr) {
            const embi_style *s = e->cache[i].s;
            e->cache[i].s = nullptr;
            e->cache[i].key = 0;
            embi_style_unref(e, s);
        }
    }
}

const embi_style *embi_style_initial(const embi_style_engine *e)
{
    return e->init;
}

void embi_style_engine_stats(const embi_style_engine *e, embi_style_stats *out)
{
    out->styles = e->nstyles;
    out->groups = e->ngroups;
    out->custom_maps = e->ncustom;
    out->bytes = e->style_bytes + e->group_bytes + e->custom_bytes;
    out->share_hits = e->cache_hits;
    out->share_misses = e->cache_misses;
    out->arena_bytes = 0;
    for (const embi_arena_block *b = e->arena.first; b != nullptr; b = b->next) {
        out->arena_bytes += b->cap + sizeof *b;
    }
}

uint32_t embi_style_group_count(void)
{
    return embi_layout_get()->ngroups;
}

void embi_style_engine_destroy(embi_style_engine *e)
{
    if (e == nullptr) {
        return;
    }
    /* Documents that are still attached are forgotten first: their observers are removed and
     * their styles released, so no document keeps a pointer into the engine. */
    uint32_t ndocs = (uint32_t)e->docs.len;
    embi_document **docs = ndocs > 0 ? embi_alloc(sizeof(embi_document *) * ndocs) : nullptr;
    uint32_t k = 0;
    embi_hashmap_iter it = embi_hashmap_iter_begin(&e->docs);
    uint64_t key = 0;
    uint64_t val = 0;
    while (embi_hashmap_iter_next(&it, &key, &val) && k < ndocs) {
        docs[k++] = (embi_document *)(uintptr_t)key;
    }
    for (uint32_t i = 0; i < k; i++) {
        embi_style_forget_document(e, docs[i]);
    }
    if (ndocs > 0) {
        embi_free(docs, sizeof(embi_document *) * ndocs);
    }
    embi_style_cache_clear(e);
    embi_hashmap_free(&e->docs);
    embi_style_unref(e, e->init);
    e->init = nullptr;
    embi_style_sset_free(&e->ua);
    embi_style_sset_free(&e->user);
    for (uint32_t i = 0; i < e->nretained; i++) {
        embi_css_sheet_unref(e->retained[i]);
    }
    if (e->ua_used) {
        embi_style_ua_unuse();
    }
    embi_free(e->retained, sizeof(embi_css_sheet *) * e->capretained);
    embi_free(e->font_sheets, sizeof(embi_css_sheet *) * e->capfont_sheets);
    if (e->user_sheets != nullptr) {
        embi_free(e->user_sheets, sizeof(embi_css_sheet *) * (e->nuser_sheets > 0 ? e->nuser_sheets : 1));
    }
    embi_free(e->sc.cands, sizeof(embi_cand) * e->sc.capcands);
    embi_free(e->sc.cw, sizeof(embi_cust_win) * e->sc.capcw);
    embi_hashmap_free(&e->face_done);
    embi_hashmap_free(&e->hints);
    embi_hashmap_free(&e->rcache);
    embi_hashmap_free(&e->vtab);
    embi_hashmap_free(&e->vcache);
    embi_sb_free(&e->vsb);
    embi_arena_free(&e->vscratch);
    embi_style_vblocks_free(e);
    embi_hashmap_free(&e->gtab);
    embi_hashmap_free(&e->stab);
    embi_hashmap_free(&e->ctab);
    embi_sel_ctx_free(&e->selctx);
    embi_hashmap_free(&e->iv.names);
    embi_hashmap_free(&e->iv.dfeat);
    embi_vec_free(&e->iv.fs);
    embi_sb_free(&e->sb);
    embi_arena_free(&e->arena);
    embi_free(e, sizeof *e);
}
