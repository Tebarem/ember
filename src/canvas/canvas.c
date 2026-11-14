/*
 * Canvas 2D context (R4b): the drawing state, the state stack, and the recording of draw calls into commands. See canvas.h.
 */
#include "canvas/canvas_int.h"

#include "gpu/stroke.h"

#include <string.h>

/* ------------------------------------------------------------------ registry */

static SDL_InitState g_reg_state;
static SDL_Mutex *g_reg_mutex;
static embi_hashmap g_reg;

static bool reg_lock(void)
{
    if (SDL_ShouldInit(&g_reg_state)) {
        g_reg_mutex = SDL_CreateMutex();
        embi_hashmap_init(&g_reg);
        SDL_SetInitialized(&g_reg_state, true);
    }
    if (g_reg_mutex == nullptr) {
        return false;
    }
    SDL_LockMutex(g_reg_mutex);
    return true;
}

static void reg_unlock(void)
{
    SDL_UnlockMutex(g_reg_mutex);
}

embi_canvas *embi_canvas_find(const void *key)
{
    if (!reg_lock()) {
        return nullptr;
    }
    uint64_t v = 0;
    bool ok = embi_hashmap_get(&g_reg, (uint64_t)(uintptr_t)key, &v);
    reg_unlock();
    return ok ? (embi_canvas *)(uintptr_t)v : nullptr;
}

bool embi_canvas_paint_info(const void *key, uint32_t *version, uint32_t *w, uint32_t *h)
{
    if (!reg_lock()) {
        return false;
    }
    uint64_t v = 0;
    bool ok = embi_hashmap_get(&g_reg, (uint64_t)(uintptr_t)key, &v);
    if (ok) {
        const embi_canvas *cv = (const embi_canvas *)(uintptr_t)v;
        *version = cv->version;
        *w = cv->w;
        *h = cv->h;
    }
    reg_unlock();
    return ok;
}

/* ------------------------------------------------------------------ images, gradients, patterns */

bool embi_canvas_size_ok(uint32_t w, uint32_t h)
{
    return w <= CV_MAX_SIDE && h <= CV_MAX_SIDE && (uint64_t)w * h <= CV_MAX_AREA;
}

/* An image beyond the canvas limit is made empty (0 x 0): the size is script controlled, and the byte count below must not
 * wrap. */
embi_cimage *embi_cimage_new(uint32_t w, uint32_t h)
{
    static uint32_t next_id = 1;
    if ((uint64_t)w * h > EMBI_CANVAS_MAX_AREA) {
        w = h = 0;
    }
    embi_cimage *im = embi_calloc(1, sizeof *im);
    im->rc = 1;
    im->w = w;
    im->h = h;
    im->px = embi_calloc((size_t)w * h * 4 + 1, 1);
    im->id = next_id++;
    return im;
}

embi_cimage *embi_cimage_retain(embi_cimage *im)
{
    if (im != nullptr) {
        im->rc++;
    }
    return im;
}

void embi_cimage_release(embi_cimage *im)
{
    if (im == nullptr || --im->rc != 0) {
        return;
    }
    if (im->tex != nullptr) {
        cgpu_release_image(im);
    }
    embi_free(im->px, (size_t)im->w * im->h * 4 + 1);
    embi_free(im, sizeof *im);
}

embi_cgrad *embi_cgrad_new(int kind, const double g[6])
{
    embi_cgrad *p = embi_calloc(1, sizeof *p);
    p->rc = 1;
    p->kind = kind;
    memcpy(p->g, g, sizeof p->g);
    return p;
}

embi_cgrad *embi_cgrad_retain(embi_cgrad *g)
{
    if (g != nullptr) {
        g->rc++;
    }
    return g;
}

void embi_cgrad_release(embi_cgrad *g)
{
    if (g == nullptr || --g->rc != 0) {
        return;
    }
    embi_cgrad_release(g->snap);
    embi_vec_free(&g->stops);
    embi_free(g, sizeof *g);
}

void *embi_cgrad_wrapper(const embi_cgrad *g)
{
    return g->wrapper;
}

void embi_cgrad_set_wrapper(embi_cgrad *g, void *w)
{
    g->wrapper = w;
}

int embi_cgrad_add_stop(embi_cgrad *g, embi_canvas *cv, double offset, embi_str css)
{
    if (!(offset >= 0.0 && offset <= 1.0)) {
        return EMBI_CV_INDEX_SIZE;
    }
    uint8_t rgb[3];
    double a;
    if (!cs_parse_color(cv, css, rgb, &a)) {
        return EMBI_CV_SYNTAX;
    }
    cstop s = {(float)offset, {rgb[0] / 255.0f, rgb[1] / 255.0f, rgb[2] / 255.0f, (float)a}};
    /* Stops with equal offsets keep their order of insertion: insert after the last stop that is not greater. */
    uint32_t at = g->stops.len;
    while (at > 0 && g->stops.data[at - 1].off > s.off) {
        at--;
    }
    embi_vec_push(&g->stops, s);
    for (uint32_t i = g->stops.len - 1; i > at; i--) {
        g->stops.data[i] = g->stops.data[i - 1];
    }
    g->stops.data[at] = s;
    g->dirty = true;
    return EMBI_CV_OK;
}

/* The immutable copy the commands hold. */
static embi_cgrad *cgrad_snapshot(embi_cgrad *g)
{
    if (g->snap == nullptr || g->dirty) {
        embi_cgrad_release(g->snap);
        embi_cgrad *c = embi_cgrad_new(g->kind, g->g);
        for (uint32_t i = 0; i < g->stops.len; i++) {
            embi_vec_push(&c->stops, g->stops.data[i]);
        }
        g->snap = c;
        g->dirty = false;
    }
    return embi_cgrad_retain(g->snap);
}

embi_cpattern *embi_cpattern_new(embi_cimage *im, int repeat)
{
    embi_cpattern *p = embi_calloc(1, sizeof *p);
    p->rc = 1;
    p->im = embi_cimage_retain(im);
    p->repeat = repeat;
    p->m[0] = p->m[3] = 1.0;
    return p;
}

embi_cpattern *embi_cpattern_retain(embi_cpattern *p)
{
    if (p != nullptr) {
        p->rc++;
    }
    return p;
}

void embi_cpattern_release(embi_cpattern *p)
{
    if (p == nullptr || --p->rc != 0) {
        return;
    }
    embi_cimage_release(p->im);
    embi_free(p, sizeof *p);
}

void embi_cpattern_set_transform(embi_cpattern *p, const double m[6])
{
    memcpy(p->m, m, sizeof p->m);
}

void *embi_cpattern_wrapper(const embi_cpattern *p)
{
    return p->wrapper;
}

void embi_cpattern_set_wrapper(embi_cpattern *p, void *w)
{
    p->wrapper = w;
}

/* ------------------------------------------------------------------ state */

static char *dup_cstr(const char *s)
{
    size_t n = strlen(s);
    char *d = embi_alloc(n + 1);
    memcpy(d, s, n + 1);
    return d;
}

static void free_cstr(char *s)
{
    if (s != nullptr) {
        embi_free(s, strlen(s) + 1);
    }
}

void cv_element_style(const embi_canvas *cv, float *font_px, uint32_t *color, bool *rtl, float *root_px)
{
    *font_px = 10.0f;
    *color = 0xFF000000u;
    *rtl = false;
    *root_px = 16.0f;
    if (cv != nullptr && cv->env.element_style != nullptr && cv->key != nullptr) {
        cv->env.element_style(cv->env.user, cv->key, font_px, color, rtl, root_px);
    }
}

static void paint_free(cpaint *p)
{
    embi_cgrad_release(p->g);
    embi_cpattern_release(p->p);
    p->g = nullptr;
    p->p = nullptr;
}

static void paint_copy(cpaint *dst, const cpaint *src)
{
    *dst = *src;
    embi_cgrad_retain(dst->g);
    embi_cpattern_retain(dst->p);
}

void cv_state_free(cstate *s)
{
    paint_free(&s->fill);
    paint_free(&s->stroke);
    if (s->dash != nullptr) {
        embi_free(s->dash, s->ndash * sizeof *s->dash);
    }
    cfont_free(&s->font);
    free_cstr(s->letter_css);
    free_cstr(s->word_css);
    free_cstr(s->filter);
    memset(s, 0, sizeof *s);
}

static void state_copy(cstate *dst, const cstate *src)
{
    *dst = *src;
    paint_copy(&dst->fill, &src->fill);
    paint_copy(&dst->stroke, &src->stroke);
    if (src->dash != nullptr) {
        dst->dash = embi_alloc(src->ndash * sizeof *src->dash);
        memcpy(dst->dash, src->dash, src->ndash * sizeof *src->dash);
    }
    cfont_copy(&dst->font, &src->font);
    dst->letter_css = dup_cstr(src->letter_css);
    dst->word_css = dup_cstr(src->word_css);
    dst->filter = dup_cstr(src->filter);
}

void cv_state_init(embi_canvas *cv, cstate *s)
{
    memset(s, 0, sizeof *s);
    s->ctm[0] = s->ctm[3] = 1.0;
    s->fill.alpha = s->stroke.alpha = 1.0;
    s->alpha = 1.0;
    s->smooth = true;
    s->line_width = 1.0;
    s->miter = 10.0;
    s->letter_css = dup_cstr("0px");
    s->word_css = dup_cstr("0px");
    s->filter = dup_cstr("none");
    s->dir = 2;
    s->stretch = 4;
    float fpx, root;
    uint32_t col;
    bool rtl;
    cv_element_style(cv, &fpx, &col, &rtl, &root);
    if (!cs_parse_font(EMBI_STR("10px sans-serif"), fpx, root, &s->font)) {
        s->font.css = dup_cstr("10px sans-serif");
        s->font.family = dup_cstr("sans-serif");
        s->font.px = 10;
        s->font.weight = 400;
        s->font.stretch = 100;
    }
}

/* ------------------------------------------------------------------ lifetime */

embi_canvas *embi_canvas_create(const embi_canvas_env *env, const void *key, uint32_t w, uint32_t h, bool alpha)
{
    embi_canvas *cv = embi_calloc(1, sizeof *cv);
    if (env != nullptr) {
        cv->env = *env;
        if (cv->env.gpu == nullptr && cv->env.gpu_get != nullptr) {
            cv->env.gpu = cv->env.gpu_get(cv->env.user);
        }
    }
    cv->key = key;
    cv->w = w;
    cv->h = h;
    cv->alpha = alpha;
    cv->path = embi_cpath_new();
    cv_state_init(cv, &cv->st);
    embi_vec_push(&cv->clips, ((cclip){nullptr, false, 0}));
    cv->need_clear = true;
    cv->version = 1;
    cgpu_register(cv); /* a canvas of a view is flushed at frame time even when no script reads it back */
    if (reg_lock()) {
        embi_hashmap_put(&g_reg, (uint64_t)(uintptr_t)key, (uint64_t)(uintptr_t)cv);
        reg_unlock();
    }
    return cv;
}

void cv_paint_cmd(const cpaint *p, cpaint_cmd *out)
{
    memset(out, 0, sizeof *out);
    out->kind = p->kind;
    out->pm[0] = out->pm[3] = 1.0;
    if (p->kind == 0) {
        out->rgba[0] = p->rgb[0] / 255.0f;
        out->rgba[1] = p->rgb[1] / 255.0f;
        out->rgba[2] = p->rgb[2] / 255.0f;
        out->rgba[3] = (float)p->alpha;
    } else if (p->kind == 1) {
        out->g = cgrad_snapshot(p->g);
    } else {
        out->im = embi_cimage_retain(p->p->im);
        out->repeat = p->p->repeat;
        memcpy(out->pm, p->p->m, sizeof out->pm);
    }
}

void cv_paint_cmd_free(cpaint_cmd *p)
{
    embi_cgrad_release(p->g);
    embi_cimage_release(p->im);
    p->g = nullptr;
    p->im = nullptr;
}

void cv_cmd_free(ccmd *c)
{
    embi_cpath_release(c->path);
    cv_paint_cmd_free(&c->paint);
    if (c->line.dash != nullptr) {
        embi_free(c->line.dash, c->line.ndash * sizeof *c->line.dash);
    }
    embi_cimage_release(c->im);
    if (c->text.text != nullptr) {
        embi_free(c->text.text, c->text.len + 1);
        cfont_free(&c->text.font);
    }
    memset(c, 0, sizeof *c);
}

void cv_cmds_clear(embi_canvas *cv)
{
    for (uint32_t i = 0; i < cv->cmds.len; i++) {
        cv_cmd_free(&cv->cmds.data[i]);
    }
    cv->cmds.len = 0;
    cv->snaps.len = 0;
    /* The clips stay while a state still names one. */
    if (cv->stack.len == 0 && cv->st.clip == 0) {
        for (uint32_t i = 1; i < cv->clips.len; i++) {
            embi_cpath_release(cv->clips.data[i].path);
        }
        cv->clips.len = 1;
    }
}

void embi_canvas_destroy(embi_canvas *cv)
{
    if (cv == nullptr) {
        return;
    }
    if (reg_lock()) {
        uint64_t v;
        if (embi_hashmap_get(&g_reg, (uint64_t)(uintptr_t)cv->key, &v) && (embi_canvas *)(uintptr_t)v == cv) {
            embi_hashmap_remove(&g_reg, (uint64_t)(uintptr_t)cv->key);
        }
        if (embi_hashmap_len(&g_reg) == 0) {
            /* No canvas left: the table goes too, so a process that is done with canvases holds nothing of them. */
            embi_hashmap_free(&g_reg);
            embi_hashmap_init(&g_reg);
        }
        reg_unlock();
    }
    cgpu_unregister(cv);
    cv_cmds_clear(cv);
    for (uint32_t i = 1; i < cv->clips.len; i++) {
        embi_cpath_release(cv->clips.data[i].path);
    }
    embi_vec_free(&cv->clips);
    embi_vec_free(&cv->cmds);
    embi_vec_free(&cv->snaps);
    for (uint32_t i = 0; i < cv->stack.len; i++) {
        cv_state_free(&cv->stack.data[i]);
    }
    embi_vec_free(&cv->stack);
    cv_state_free(&cv->st);
    embi_cpath_release(cv->path);
    embi_free(cv, sizeof *cv);
}

/* Commands pile up between frames; past this many the canvas draws them right away (its own command buffer). */
#define CV_AUTO_FLUSH 4096u

void cv_touch(embi_canvas *cv)
{
    cv->version++;
    if (cv->env.gpu == nullptr && cv->gpu == nullptr) {
        cv_cmds_clear(cv); /* no pixels to draw into: nothing to keep */
    } else if (cv->cmds.len >= CV_AUTO_FLUSH) {
        cgpu_flush_canvas(cv, nullptr);
    }
    if (cv->env.dirty != nullptr) {
        cv->env.dirty(cv->env.user);
    }
}

void embi_canvas_reset(embi_canvas *cv)
{
    embi_canvas_resize(cv, cv->w, cv->h);
}

void embi_canvas_resize(embi_canvas *cv, uint32_t w, uint32_t h)
{
    cv_cmds_clear(cv);
    for (uint32_t i = 1; i < cv->clips.len; i++) {
        embi_cpath_release(cv->clips.data[i].path);
    }
    cv->clips.len = 1;
    for (uint32_t i = 0; i < cv->stack.len; i++) {
        cv_state_free(&cv->stack.data[i]);
    }
    cv->stack.len = 0;
    cv_state_free(&cv->st);
    cv_state_init(cv, &cv->st);
    embi_cpath_reset(cv->path);
    if (w != cv->w || h != cv->h) {
        cgpu_detach(cv); /* the texture has the old size */
    }
    cv->w = w;
    cv->h = h;
    cv->need_clear = true;
    cgpu_attach(cv); /* the detach above dropped it from the flush list */
    cv_touch(cv);
}

uint32_t embi_canvas_width(const embi_canvas *cv)
{
    return cv->w;
}

uint32_t embi_canvas_height(const embi_canvas *cv)
{
    return cv->h;
}

bool embi_canvas_has_alpha(const embi_canvas *cv)
{
    return cv->alpha;
}

const void *embi_canvas_key(const embi_canvas *cv)
{
    return cv->key;
}

/* ------------------------------------------------------------------ the state stack and transforms */

void embi_canvas_save(embi_canvas *cv)
{
    if (cv->stack.len >= 4096) {
        return;
    }
    cstate c;
    state_copy(&c, &cv->st);
    embi_vec_push(&cv->stack, c);
}

int embi_canvas_restore(embi_canvas *cv)
{
    if (cv->stack.len == 0) {
        return 0;
    }
    if (cv->stack.data[cv->stack.len - 1].is_layer) {
        return EMBI_CV_INVALID_STATE;
    }
    cv_state_free(&cv->st);
    cv->st = embi_vec_pop(&cv->stack);
    return 0;
}

void embi_canvas_begin_layer(embi_canvas *cv)
{
    if (cv->stack.len >= 4096) {
        return;
    }
    cstate c;
    state_copy(&c, &cv->st);
    c.is_layer = true;
    embi_vec_push(&cv->stack, c);
    cstate *s = &cv->st;
    s->alpha = 1.0;
    s->op = 0;
    s->sh_alpha = 0.0;
    s->sh_blur = s->sh_x = s->sh_y = 0.0;
    s->sh_rgb[0] = s->sh_rgb[1] = s->sh_rgb[2] = 0;
    free_cstr(s->filter);
    s->filter = dup_cstr("none");
}

int embi_canvas_end_layer(embi_canvas *cv)
{
    if (cv->stack.len == 0 || !cv->stack.data[cv->stack.len - 1].is_layer) {
        return EMBI_CV_INVALID_STATE;
    }
    cv_state_free(&cv->st);
    cv->st = embi_vec_pop(&cv->stack);
    cv->st.is_layer = false;
    return 0;
}

void embi_canvas_get_transform(const embi_canvas *cv, double m[6])
{
    memcpy(m, cv->st.ctm, sizeof cv->st.ctm);
}

void embi_canvas_set_transform(embi_canvas *cv, const double m[6])
{
    if (cm_finite(m, 6)) {
        memcpy(cv->st.ctm, m, sizeof cv->st.ctm);
    }
}

void embi_canvas_transform(embi_canvas *cv, const double m[6])
{
    if (cm_finite(m, 6)) {
        cm_mul(cv->st.ctm, m, cv->st.ctm);
    }
}

void embi_canvas_translate(embi_canvas *cv, double x, double y)
{
    if (isfinite(x) && isfinite(y)) {
        double m[6] = {1, 0, 0, 1, x, y};
        cm_mul(cv->st.ctm, m, cv->st.ctm);
    }
}

void embi_canvas_scale(embi_canvas *cv, double x, double y)
{
    if (isfinite(x) && isfinite(y)) {
        double m[6] = {x, 0, 0, y, 0, 0};
        cm_mul(cv->st.ctm, m, cv->st.ctm);
    }
}

void embi_canvas_rotate(embi_canvas *cv, double a)
{
    if (isfinite(a)) {
        double c = cos(a), s = sin(a);
        double m[6] = {c, s, -s, c, 0, 0};
        cm_mul(cv->st.ctm, m, cv->st.ctm);
    }
}

/* ------------------------------------------------------------------ simple attributes */

double embi_canvas_get_alpha(const embi_canvas *cv)
{
    return cv->st.alpha;
}

void embi_canvas_set_alpha(embi_canvas *cv, double a)
{
    if (isfinite(a) && a >= 0.0 && a <= 1.0) {
        cv->st.alpha = a;
    }
}

static const char *const k_ops[CV_OP_COUNT] = {
    "source-over", "source-in", "source-out", "source-atop", "destination-over", "destination-in", "destination-out",
    "destination-atop", "lighter", "copy", "xor", "multiply", "screen", "overlay", "darken", "lighten", "color-dodge",
    "color-burn", "hard-light", "soft-light", "difference", "exclusion", "hue", "saturation", "color", "luminosity", "clear",
};

const char *embi_canvas_get_composite(const embi_canvas *cv)
{
    return k_ops[cv->st.op];
}

void embi_canvas_set_composite(embi_canvas *cv, embi_str op)
{
    for (int i = 0; i < CV_OP_COUNT; i++) {
        if (op.len == strlen(k_ops[i]) && memcmp(op.ptr, k_ops[i], op.len) == 0) {
            cv->st.op = (uint8_t)i;
            return;
        }
    }
}

bool embi_canvas_get_smoothing(const embi_canvas *cv)
{
    return cv->st.smooth;
}

void embi_canvas_set_smoothing(embi_canvas *cv, bool on)
{
    cv->st.smooth = on;
}

static const char *const k_quality[3] = {"low", "medium", "high"};

const char *embi_canvas_get_quality(const embi_canvas *cv)
{
    return k_quality[cv->st.quality];
}

void embi_canvas_set_quality(embi_canvas *cv, embi_str q)
{
    for (int i = 0; i < 3; i++) {
        if (q.len == strlen(k_quality[i]) && memcmp(q.ptr, k_quality[i], q.len) == 0) {
            cv->st.quality = (uint8_t)i;
        }
    }
}

bool embi_canvas_set_color(embi_canvas *cv, int which, embi_str css)
{
    uint8_t rgb[3];
    double a;
    if (!cs_parse_color(cv, css, rgb, &a)) {
        return false;
    }
    cpaint *p = which == 0 ? &cv->st.fill : &cv->st.stroke;
    paint_free(p);
    p->kind = 0;
    memcpy(p->rgb, rgb, 3);
    p->alpha = a;
    return true;
}

void embi_canvas_set_gradient(embi_canvas *cv, int which, embi_cgrad *g)
{
    cpaint *p = which == 0 ? &cv->st.fill : &cv->st.stroke;
    embi_cgrad_retain(g);
    paint_free(p);
    p->kind = 1;
    p->g = g;
}

void embi_canvas_set_pattern(embi_canvas *cv, int which, embi_cpattern *pt)
{
    cpaint *p = which == 0 ? &cv->st.fill : &cv->st.stroke;
    embi_cpattern_retain(pt);
    paint_free(p);
    p->kind = 2;
    p->p = pt;
}

int embi_canvas_get_style(const embi_canvas *cv, int which, embi_sb *css, embi_cgrad **g, embi_cpattern **pt)
{
    const cpaint *p = which == 0 ? &cv->st.fill : &cv->st.stroke;
    if (p->kind == 1) {
        *g = p->g;
        return 1;
    }
    if (p->kind == 2) {
        *pt = p->p;
        return 2;
    }
    cs_color_string(p->rgb, p->alpha, css);
    return 0;
}

double embi_canvas_get_line_width(const embi_canvas *cv)
{
    return cv->st.line_width;
}

void embi_canvas_set_line_width(embi_canvas *cv, double w)
{
    if (isfinite(w) && w > 0.0) {
        cv->st.line_width = w;
    }
}

static const char *const k_caps[3] = {"butt", "round", "square"};
static const char *const k_joins[3] = {"miter", "round", "bevel"};

const char *embi_canvas_get_line_cap(const embi_canvas *cv)
{
    return k_caps[cv->st.cap];
}

void embi_canvas_set_line_cap(embi_canvas *cv, embi_str s)
{
    for (int i = 0; i < 3; i++) {
        if (s.len == strlen(k_caps[i]) && memcmp(s.ptr, k_caps[i], s.len) == 0) {
            cv->st.cap = (uint8_t)i;
        }
    }
}

const char *embi_canvas_get_line_join(const embi_canvas *cv)
{
    return k_joins[cv->st.join];
}

void embi_canvas_set_line_join(embi_canvas *cv, embi_str s)
{
    for (int i = 0; i < 3; i++) {
        if (s.len == strlen(k_joins[i]) && memcmp(s.ptr, k_joins[i], s.len) == 0) {
            cv->st.join = (uint8_t)i;
        }
    }
}

double embi_canvas_get_miter_limit(const embi_canvas *cv)
{
    return cv->st.miter;
}

void embi_canvas_set_miter_limit(embi_canvas *cv, double v)
{
    if (isfinite(v) && v > 0.0) {
        cv->st.miter = v;
    }
}

bool embi_canvas_set_line_dash(embi_canvas *cv, const double *v, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        if (!isfinite(v[i]) || v[i] < 0.0) {
            return false;
        }
    }
    if (cv->st.dash != nullptr) {
        embi_free(cv->st.dash, cv->st.ndash * sizeof *cv->st.dash);
        cv->st.dash = nullptr;
    }
    uint32_t total = (n & 1u) ? n * 2u : n;
    cv->st.ndash = total;
    if (total > 0) {
        cv->st.dash = embi_alloc(total * sizeof *cv->st.dash);
        for (uint32_t i = 0; i < total; i++) {
            cv->st.dash[i] = v[i % n];
        }
    }
    return true;
}

uint32_t embi_canvas_get_line_dash(const embi_canvas *cv, const double **v)
{
    *v = cv->st.dash;
    return cv->st.ndash;
}

double embi_canvas_get_dash_offset(const embi_canvas *cv)
{
    return cv->st.dash_off;
}

void embi_canvas_set_dash_offset(embi_canvas *cv, double v)
{
    if (isfinite(v)) {
        cv->st.dash_off = v;
    }
}

void embi_canvas_get_shadow_color(const embi_canvas *cv, embi_sb *css)
{
    cs_color_string(cv->st.sh_rgb, cv->st.sh_alpha, css);
}

void embi_canvas_set_shadow_color(embi_canvas *cv, embi_str css)
{
    uint8_t rgb[3];
    double a;
    if (cs_parse_color(cv, css, rgb, &a)) {
        memcpy(cv->st.sh_rgb, rgb, 3);
        cv->st.sh_alpha = a;
    }
}

double embi_canvas_get_shadow_blur(const embi_canvas *cv)
{
    return cv->st.sh_blur;
}

void embi_canvas_set_shadow_blur(embi_canvas *cv, double v)
{
    if (isfinite(v) && v >= 0.0) {
        cv->st.sh_blur = v;
    }
}

double embi_canvas_get_shadow_x(const embi_canvas *cv)
{
    return cv->st.sh_x;
}

double embi_canvas_get_shadow_y(const embi_canvas *cv)
{
    return cv->st.sh_y;
}

void embi_canvas_set_shadow_x(embi_canvas *cv, double v)
{
    if (isfinite(v)) {
        cv->st.sh_x = v;
    }
}

void embi_canvas_set_shadow_y(embi_canvas *cv, double v)
{
    if (isfinite(v)) {
        cv->st.sh_y = v;
    }
}

/* ------------------------------------------------------------------ text attributes */

static const char *const k_aligns[5] = {"start", "end", "left", "right", "center"};
static const char *const k_bases[6] = {"alphabetic", "top", "hanging", "middle", "ideographic", "bottom"};
static const char *const k_dirs[3] = {"ltr", "rtl", "inherit"};
static const char *const k_kern[3] = {"auto", "normal", "none"};

static int enum_index(const char *const *names, int n, embi_str v)
{
    for (int i = 0; i < n; i++) {
        if (v.len == strlen(names[i]) && memcmp(v.ptr, names[i], v.len) == 0) {
            return i;
        }
    }
    return -1;
}

static void sb_cstr(embi_sb *out, const char *s)
{
    embi_sb_append(out, (embi_str){s, (uint32_t)strlen(s)});
}

void embi_canvas_get_text_attr(const embi_canvas *cv, int which, embi_sb *out)
{
    const cstate *s = &cv->st;
    switch (which) {
    case 0: sb_cstr(out, s->font.css); break;
    case 1: sb_cstr(out, k_aligns[s->align]); break;
    case 2: sb_cstr(out, k_bases[s->baseline]); break;
    case 3: sb_cstr(out, k_dirs[s->dir]); break;
    case 4: sb_cstr(out, s->letter_css); break;
    case 5: sb_cstr(out, s->word_css); break;
    case 6: sb_cstr(out, k_kern[s->kerning]); break;
    case 7: sb_cstr(out, cs_font_stretch_name(s->stretch)); break;
    case 8: sb_cstr(out, cs_caps_name(s->caps)); break;
    case 9: sb_cstr(out, cs_rendering_name(s->rendering)); break;
    default: sb_cstr(out, s->filter); break;
    }
}

static void set_spacing(embi_canvas *cv, embi_str v, double *num, char **css)
{
    float fpx, root;
    uint32_t col;
    bool rtl;
    cv_element_style(cv, &fpx, &col, &rtl, &root);
    double px;
    if (!cs_parse_length(v, cv->st.font.px, root, &px)) {
        return;
    }
    *num = px;
    free_cstr(*css);
    /* The getter returns the specified length, trimmed; font-relative lengths are resolved again when the font changes. */
    while (v.len > 0 && (v.ptr[0] == ' ' || v.ptr[0] == '\t' || v.ptr[0] == '\n')) {
        v.ptr++;
        v.len--;
    }
    while (v.len > 0 && (v.ptr[v.len - 1] == ' ' || v.ptr[v.len - 1] == '\t' || v.ptr[v.len - 1] == '\n')) {
        v.len--;
    }
    char buf[96];
    size_t n = v.len < sizeof buf - 1 ? v.len : sizeof buf - 1;
    for (size_t k = 0; k < n; k++) {
        buf[k] = (v.ptr[k] >= 'A' && v.ptr[k] <= 'Z') ? (char)(v.ptr[k] + 32) : v.ptr[k];
    }
    buf[n] = '\0';
    *css = dup_cstr(buf);
}

/* After a font change: the spacings given in font-relative units follow the new font. */
static void respace(embi_canvas *cv, float root)
{
    cstate *s = &cv->st;
    double px;
    if (s->letter_css != nullptr && cs_parse_length((embi_str){s->letter_css, (uint32_t)strlen(s->letter_css)}, s->font.px, root, &px)) {
        s->letter_spacing = px;
    }
    if (s->word_css != nullptr && cs_parse_length((embi_str){s->word_css, (uint32_t)strlen(s->word_css)}, s->font.px, root, &px)) {
        s->word_spacing = px;
    }
}

void embi_canvas_set_text_attr(embi_canvas *cv, int which, embi_str v)
{
    cstate *s = &cv->st;
    int i;
    switch (which) {
    case 0: {
        float fpx, root;
        uint32_t col;
        bool rtl;
        cv_element_style(cv, &fpx, &col, &rtl, &root);
        cfont f;
        if (cs_parse_font(v, fpx, root, &f)) {
            cfont_free(&s->font);
            s->font = f;
            respace(cv, root);
        }
        break;
    }
    case 1:
        if ((i = enum_index(k_aligns, 5, v)) >= 0) {
            s->align = (uint8_t)i;
        }
        break;
    case 2:
        if ((i = enum_index(k_bases, 6, v)) >= 0) {
            s->baseline = (uint8_t)i;
        }
        break;
    case 3:
        if ((i = enum_index(k_dirs, 3, v)) >= 0) {
            s->dir = (uint8_t)i;
        }
        break;
    case 4: set_spacing(cv, v, &s->letter_spacing, &s->letter_css); break;
    case 5: set_spacing(cv, v, &s->word_spacing, &s->word_css); break;
    case 6:
        if ((i = enum_index(k_kern, 3, v)) >= 0) {
            s->kerning = (uint8_t)i;
        }
        break;
    case 7:
        for (i = 0; i < 9; i++) {
            const char *n = cs_font_stretch_name((uint8_t)i);
            if (v.len == strlen(n) && memcmp(v.ptr, n, v.len) == 0) {
                s->stretch = (uint8_t)i;
            }
        }
        break;
    case 8:
        for (i = 0; i < 7; i++) {
            const char *n = cs_caps_name((uint8_t)i);
            if (v.len == strlen(n) && memcmp(v.ptr, n, v.len) == 0) {
                s->caps = (uint8_t)i;
            }
        }
        break;
    case 9:
        for (i = 0; i < 4; i++) {
            const char *n = cs_rendering_name((uint8_t)i);
            if (v.len == strlen(n) && memcmp(v.ptr, n, v.len) == 0) {
                s->rendering = (uint8_t)i;
            }
        }
        break;
    default:
        if (v.len == 4 && memcmp(v.ptr, "none", 4) == 0) {
            free_cstr(s->filter);
            s->filter = dup_cstr("none");
        } else if (memchr(v.ptr, '(', v.len) != nullptr) {
            /* Stored for the getter; the filter list is not applied (needs R3a on the context). */
            free_cstr(s->filter);
            char *d = embi_alloc(v.len + 1);
            memcpy(d, v.ptr, v.len);
            d[v.len] = '\0';
            s->filter = d;
        }
        break;
    }
}

/* ------------------------------------------------------------------ recording */

static bool ctm_ok(const embi_canvas *cv)
{
    const double *m = cv->st.ctm;
    return cm_finite(m, 6) && cm_det(m) != 0.0;
}

static uint32_t snap_index(embi_canvas *cv)
{
    const cstate *s = &cv->st;
    csnap n;
    memset(&n, 0, sizeof n);
    memcpy(n.ctm, s->ctm, sizeof n.ctm);
    n.alpha = (float)s->alpha;
    n.op = s->op;
    if (s->op == CV_OP_CLEAR) {
        n.op = CV_OP_DEST_OUT;
        n.alpha = 1.0f;
        n.clear = 1;
    }
    n.smooth = s->smooth;
    n.quality = s->quality;
    n.sh_rgba[0] = s->sh_rgb[0] / 255.0f;
    n.sh_rgba[1] = s->sh_rgb[1] / 255.0f;
    n.sh_rgba[2] = s->sh_rgb[2] / 255.0f;
    n.sh_rgba[3] = (float)s->sh_alpha;
    n.sh_blur = (float)s->sh_blur;
    n.sh_x = (float)s->sh_x;
    n.sh_y = (float)s->sh_y;
    n.clip = s->clip;
    if (cv->snaps.len > 0 && memcmp(&cv->snaps.data[cv->snaps.len - 1], &n, sizeof n) == 0) {
        return cv->snaps.len - 1;
    }
    embi_vec_push(&cv->snaps, n);
    return cv->snaps.len - 1;
}

static ccmd *cmd_new(embi_canvas *cv, uint8_t kind)
{
    ccmd c;
    memset(&c, 0, sizeof c);
    c.kind = kind;
    c.snap = snap_index(cv);
    embi_vec_push(&cv->cmds, c);
    return &cv->cmds.data[cv->cmds.len - 1];
}

static void stroke_snapshot(const embi_canvas *cv, cstroke *l)
{
    const cstate *s = &cv->st;
    l->width = s->line_width;
    l->miter = s->miter;
    l->cap = s->cap;
    l->join = s->join;
    l->dash_off = s->dash_off;
    l->ndash = s->ndash;
    if (s->ndash > 0) {
        l->dash = embi_alloc(s->ndash * sizeof *l->dash);
        memcpy(l->dash, s->dash, s->ndash * sizeof *l->dash);
    }
}

/* The default path is shared with the commands that drew it: a change after that copies it first. */
static embi_cpath *own_path(embi_canvas *cv)
{
    if (cv->path->rc > 1) {
        embi_cpath *c = embi_cpath_clone(cv->path);
        embi_cpath_release(cv->path);
        cv->path = c;
    }
    return cv->path;
}

embi_cpath *embi_canvas_path(embi_canvas *cv)
{
    return own_path(cv);
}

void embi_canvas_begin_path(embi_canvas *cv)
{
    embi_cpath_release(cv->path);
    cv->path = embi_cpath_new();
}

static void record_path_cmd(embi_canvas *cv, uint8_t kind, embi_cpath *dev_path, bool evenodd, const cpaint *paint)
{
    if (!ctm_ok(cv)) {
        return;
    }
    ccmd *c = cmd_new(cv, kind);
    c->path = embi_cpath_retain(dev_path);
    c->evenodd = evenodd;
    if (paint != nullptr) {
        cv_paint_cmd(paint, &c->paint);
    }
    if (kind == CK_STROKE) {
        stroke_snapshot(cv, &c->line);
    }
    cv_touch(cv);
}

/* The path of a draw call in device space: the default path itself, or a Path2D under the CTM. */
static embi_cpath *device_path(embi_canvas *cv, embi_cpath *path, bool *owned)
{
    if (path == nullptr) {
        *owned = false;
        return cv->path;
    }
    embi_cpath *d = embi_cpath_new();
    embi_cpath_add(d, path, cv->st.ctm);
    *owned = true;
    return d;
}

void embi_canvas_fill(embi_canvas *cv, embi_cpath *path, bool evenodd)
{
    bool owned;
    embi_cpath *d = device_path(cv, path, &owned);
    if (!embi_cpath_empty(d)) {
        record_path_cmd(cv, CK_FILL, d, evenodd, &cv->st.fill);
    }
    if (owned) {
        embi_cpath_release(d);
    }
}

void embi_canvas_stroke(embi_canvas *cv, embi_cpath *path)
{
    bool owned;
    embi_cpath *d = device_path(cv, path, &owned);
    if (!embi_cpath_empty(d)) {
        record_path_cmd(cv, CK_STROKE, d, false, &cv->st.stroke);
    }
    if (owned) {
        embi_cpath_release(d);
    }
}

void embi_canvas_clip(embi_canvas *cv, embi_cpath *path, bool evenodd)
{
    bool owned;
    embi_cpath *d = device_path(cv, path, &owned);
    cclip c = {embi_cpath_clone(d), evenodd, cv->st.clip};
    if (owned) {
        embi_cpath_release(d);
    }
    embi_vec_push(&cv->clips, c);
    cv->st.clip = cv->clips.len - 1;
}

bool embi_canvas_in_path(embi_canvas *cv, embi_cpath *path, double x, double y, bool evenodd)
{
    if (!isfinite(x) || !isfinite(y)) {
        return false;
    }
    bool owned;
    embi_cpath *d = device_path(cv, path, &owned);
    bool r = embi_cpath_contains(d, x, y, evenodd);
    if (owned) {
        embi_cpath_release(d);
    }
    return r;
}

/* Converts a device-space path into a GPU path in user space (under the inverse CTM). */
static embi_path *user_gpu_path(const embi_cpath *p, const double inv[6])
{
    embi_path *g = embi_path_create(0.0f);
    const double *q = p->pts.data;
    for (uint32_t i = 0; i < p->ops.len; i++) {
        double t[6][2];
        switch (p->ops.data[i]) {
        case CP_M:
            cm_apply(inv, q[0], q[1], &t[0][0], &t[0][1]);
            embi_path_move_to(g, (float)t[0][0], (float)t[0][1]);
            q += 2;
            break;
        case CP_L:
            cm_apply(inv, q[0], q[1], &t[0][0], &t[0][1]);
            embi_path_line_to(g, (float)t[0][0], (float)t[0][1]);
            q += 2;
            break;
        case CP_Q:
            cm_apply(inv, q[0], q[1], &t[0][0], &t[0][1]);
            cm_apply(inv, q[2], q[3], &t[1][0], &t[1][1]);
            embi_path_quad_to(g, (float)t[0][0], (float)t[0][1], (float)t[1][0], (float)t[1][1]);
            q += 4;
            break;
        case CP_C:
            cm_apply(inv, q[0], q[1], &t[0][0], &t[0][1]);
            cm_apply(inv, q[2], q[3], &t[1][0], &t[1][1]);
            cm_apply(inv, q[4], q[5], &t[2][0], &t[2][1]);
            embi_path_cubic_to(g, (float)t[0][0], (float)t[0][1], (float)t[1][0], (float)t[1][1], (float)t[2][0],
                               (float)t[2][1]);
            q += 6;
            break;
        default:
            embi_path_close(g);
            break;
        }
    }
    return g;
}

typedef struct sk_wind {
    double x, y;
    int w;
} sk_wind;

static void sk_edge(sk_wind *k, double x0, double y0, double x1, double y1)
{
    if ((y0 <= k->y) != (y1 <= k->y)) {
        double xi = x0 + (k->y - y0) / (y1 - y0) * (x1 - x0);
        if (xi > k->x) {
            k->w += (y1 > y0) ? 1 : -1;
        }
    }
}

bool embi_canvas_in_stroke(embi_canvas *cv, embi_cpath *path, double x, double y)
{
    if (!isfinite(x) || !isfinite(y) || !ctm_ok(cv)) {
        return false;
    }
    bool owned;
    embi_cpath *d = device_path(cv, path, &owned);
    bool hit = false;
    double inv[6];
    if (!embi_cpath_empty(d) && cm_invert(cv->st.ctm, inv)) {
        embi_path *g = user_gpu_path(d, inv);
        embi_stroke_style st = {
            .width = (float)cv->st.line_width,
            .cap = (embi_stroke_cap)cv->st.cap,
            .join = (embi_stroke_join)cv->st.join,
            .miter_limit = (float)cv->st.miter,
        };
        float *dash = nullptr;
        if (cv->st.ndash > 0) {
            dash = embi_alloc(cv->st.ndash * sizeof *dash);
            for (uint32_t i = 0; i < cv->st.ndash; i++) {
                dash[i] = (float)cv->st.dash[i];
            }
            st.dash = dash;
            st.dash_count = cv->st.ndash;
            st.dash_offset = (float)cv->st.dash_off;
        }
        embi_path *s = embi_path_stroke(g, &st, 0.0f);
        if (s != nullptr) {
            double px, py; /* the point in user space */
            cm_apply(inv, x, y, &px, &py);
            uint32_t n = 0;
            const embi_qcurve *sg = embi_path_segments(s, &n);
            sk_wind k = {px, py, 0};
            for (uint32_t i = 0; i < n; i++) {
                /* The pieces of a stroke are closed quadratics' contours: flatten each quadratic in 8 chords. */
                double ax = sg[i].x0, ay = sg[i].y0;
                for (int j = 1; j <= 8; j++) {
                    double t = j / 8.0, u = 1 - t;
                    double bx = u * u * sg[i].x0 + 2 * u * t * sg[i].cx + t * t * sg[i].x1;
                    double by = u * u * sg[i].y0 + 2 * u * t * sg[i].cy + t * t * sg[i].y1;
                    sk_edge(&k, ax, ay, bx, by);
                    ax = bx;
                    ay = by;
                }
            }
            hit = k.w != 0;
            embi_path_destroy(s);
        }
        if (dash != nullptr) {
            embi_free(dash, cv->st.ndash * sizeof *dash);
        }
        embi_path_destroy(g);
    }
    if (owned) {
        embi_cpath_release(d);
    }
    return hit;
}

void embi_canvas_clear_rect(embi_canvas *cv, double x, double y, double w, double h)
{
    if (!isfinite(x) || !isfinite(y) || !isfinite(w) || !isfinite(h) || w == 0.0 || h == 0.0 || !ctm_ok(cv)) {
        return;
    }
    embi_cpath *p = embi_cpath_new();
    embi_cpath_rect(p, cv->st.ctm, x, y, w, h);
    record_path_cmd(cv, CK_CLEAR, p, false, nullptr);
    embi_cpath_release(p);
}

void embi_canvas_fill_rect(embi_canvas *cv, double x, double y, double w, double h)
{
    if (!isfinite(x) || !isfinite(y) || !isfinite(w) || !isfinite(h) || w == 0.0 || h == 0.0) {
        return;
    }
    embi_cpath *p = embi_cpath_new();
    embi_cpath_rect(p, cv->st.ctm, x, y, w, h);
    record_path_cmd(cv, CK_FILL, p, false, &cv->st.fill);
    embi_cpath_release(p);
}

void embi_canvas_stroke_rect(embi_canvas *cv, double x, double y, double w, double h)
{
    if (!isfinite(x) || !isfinite(y) || !isfinite(w) || !isfinite(h) || (w == 0.0 && h == 0.0)) {
        return;
    }
    embi_cpath *p = embi_cpath_new();
    /* A rectangle with one zero side is a closed contour that doubles back: it has joins and no caps. */
    embi_cpath_rect(p, cv->st.ctm, x, y, w, h);
    record_path_cmd(cv, CK_STROKE, p, false, &cv->st.stroke);
    embi_cpath_release(p);
}

/* ------------------------------------------------------------------ text */

static void text_snapshot(const embi_canvas *cv, ctextcmd *t, embi_str text, double x, double y, double mw)
{
    const cstate *s = &cv->st;
    t->text = embi_alloc((size_t)text.len + 1);
    memcpy(t->text, text.ptr, text.len);
    t->text[text.len] = '\0';
    t->len = text.len;
    t->x = x;
    t->y = y;
    t->max_width = mw;
    cfont_copy(&t->font, &s->font);
    t->align = s->align;
    t->baseline = s->baseline;
    t->dir = s->dir;
    t->kerning = s->kerning;
    t->stretch = s->stretch;
    t->caps = s->caps;
    t->letter_spacing = s->letter_spacing;
    t->word_spacing = s->word_spacing;
    t->rtl = s->dir == 1;
    if (s->dir == 2) {
        float fpx, root;
        uint32_t col;
        bool rtl;
        cv_element_style(cv, &fpx, &col, &rtl, &root); /* only the inherited direction needs the element's style */
        t->rtl = rtl;
    }
}

void embi_canvas_draw_text(embi_canvas *cv, embi_str utf8, double x, double y, double max_width, bool stroke)
{
    /* max_width is INFINITY when the script gave none; a NaN or a value of zero or less draws nothing. */
    if (!isfinite(x) || !isfinite(y) || isnan(max_width) || max_width <= 0.0 || !ctm_ok(cv) || cv->env.text == nullptr) {
        return;
    }
    ccmd *c = cmd_new(cv, CK_TEXT);
    c->stroke_text = stroke;
    cv_paint_cmd(stroke ? &cv->st.stroke : &cv->st.fill, &c->paint);
    if (stroke) {
        stroke_snapshot(cv, &c->line);
    }
    text_snapshot(cv, &c->text, utf8, x, y, max_width);
    cv_touch(cv);
}

bool embi_canvas_measure_text(embi_canvas *cv, embi_str utf8, embi_text_metrics *out)
{
    memset(out, 0, sizeof *out);
    if (cv->env.text == nullptr) {
        return false;
    }
    ctextcmd tc;
    memset(&tc, 0, sizeof tc);
    text_snapshot(cv, &tc, utf8, 0, 0, INFINITY);
    ctext_layout *l = ctext_shape(cv->env.text, &tc);
    bool ok = l != nullptr;
    if (ok) {
        ctext_metrics(l, &tc, out);
        ctext_layout_free(l);
    }
    embi_free(tc.text, tc.len + 1);
    cfont_free(&tc.font);
    return ok;
}

/* ------------------------------------------------------------------ images */

static void rect_norm(double r[4])
{
    if (r[2] < 0) {
        r[0] += r[2];
        r[2] = -r[2];
    }
    if (r[3] < 0) {
        r[1] += r[3];
        r[3] = -r[3];
    }
}

int embi_canvas_draw_image(embi_canvas *cv, embi_cimage *im, embi_canvas *src, const double s_in[4], const double d_in[4])
{
    uint32_t iw = im != nullptr ? im->w : src->w, ih = im != nullptr ? im->h : src->h;
    if (iw == 0 || ih == 0) {
        return EMBI_CV_INVALID_STATE;
    }
    if (im == nullptr && !embi_canvas_size_ok(iw, ih)) {
        return EMBI_CV_OK; /* a canvas beyond the size limit has no bitmap to draw */
    }
    double s[4], d[4];
    memcpy(s, s_in, sizeof s);
    memcpy(d, d_in, sizeof d);
    if (!cm_finite(s, 4) || !cm_finite(d, 4)) {
        return EMBI_CV_OK;
    }
    if (s[2] == 0.0 || s[3] == 0.0 || d[2] == 0.0 || d[3] == 0.0) {
        return EMBI_CV_OK;
    }
    rect_norm(s);
    rect_norm(d);
    /* Clip the source rectangle to the image and shrink the destination with it. */
    double sx0 = s[0], sy0 = s[1], sx1 = s[0] + s[2], sy1 = s[1] + s[3];
    double kx = d[2] / s[2], ky = d[3] / s[3];
    double cx0 = sx0 < 0 ? 0 : sx0, cy0 = sy0 < 0 ? 0 : sy0;
    double cx1 = sx1 > iw ? iw : sx1, cy1 = sy1 > ih ? ih : sy1;
    if (!(cx1 > cx0) || !(cy1 > cy0)) {
        return EMBI_CV_OK;
    }
    d[0] += (cx0 - sx0) * kx;
    d[1] += (cy0 - sy0) * ky;
    d[2] = (cx1 - cx0) * kx;
    d[3] = (cy1 - cy0) * ky;
    s[0] = cx0;
    s[1] = cy0;
    s[2] = cx1 - cx0;
    s[3] = cy1 - cy0;
    if (!ctm_ok(cv)) {
        return EMBI_CV_OK;
    }
    embi_cimage *snap = nullptr;
    if (src != nullptr && src != cv) {
        embi_canvas_flush(src); /* its texture is the picture as of this call */
    } else if (src == cv) {
        /* Drawing a canvas onto itself: copy what it holds now. */
        embi_canvas_flush(cv);
        snap = embi_canvas_snapshot(cv, 0, 0, cv->w, cv->h);
        im = snap;
        if (snap->w == 0) {
            embi_cimage_release(snap);
            return EMBI_CV_OK;
        }
    }
    embi_cpath *p = embi_cpath_new();
    embi_cpath_rect(p, cv->st.ctm, d[0], d[1], d[2], d[3]);
    ccmd *c = cmd_new(cv, CK_IMAGE);
    c->path = p;
    memcpy(c->s, s, sizeof c->s);
    memcpy(c->d, d, sizeof c->d);
    if (im != nullptr) {
        c->im = embi_cimage_retain(im);
    } else {
        c->src_cv = src;
        c->src_w = src->w;
        c->src_h = src->h;
    }
    embi_cimage_release(snap);
    cv_touch(cv);
    if (c->src_cv != nullptr) {
        embi_canvas_flush(cv); /* the source may change before the next frame */
    }
    return EMBI_CV_OK;
}

embi_cpattern *embi_canvas_pattern_from(embi_canvas *cv, embi_cimage *im, embi_canvas *src, int repeat)
{
    (void)cv;
    if (im != nullptr) {
        return embi_cpattern_new(im, repeat);
    }
    embi_cimage *snap = embi_canvas_snapshot(src, 0, 0, src->w, src->h); /* empty beyond the size limit */
    if (snap->w == 0 || snap->h == 0) {
        embi_cimage_release(snap);
        return nullptr;
    }
    embi_cpattern *p = embi_cpattern_new(snap, repeat);
    embi_cimage_release(snap);
    return p;
}

embi_cimage *embi_canvas_snapshot(embi_canvas *cv, int32_t x, int32_t y, uint32_t w, uint32_t h)
{
    embi_cimage *im = embi_cimage_new(w, h);
    if (im->w > 0 && im->h > 0) {
        (void)embi_canvas_read(cv, x, y, im->w, im->h, im->px);
    }
    return im;
}

bool embi_canvas_read(embi_canvas *cv, int32_t x, int32_t y, uint32_t w, uint32_t h, uint8_t *out)
{
    if (!cv_size_ok(w, h)) {
        return false;
    }
    memset(out, 0, (size_t)w * h * 4);
    if (cv->gpu == nullptr && cv->env.gpu == nullptr) {
        return false;
    }
    return cgpu_read(cv, x, y, w, h, out);
}

void embi_canvas_put(embi_canvas *cv, const uint8_t *px, uint32_t dw, uint32_t dh, int32_t x, int32_t y)
{
    if (dw == 0 || dh == 0) {
        return;
    }
    if ((uint64_t)dw * dh > EMBI_CANVAS_MAX_AREA) {
        return;
    }
    embi_cimage *im = embi_cimage_new(dw, dh);
    memcpy(im->px, px, (size_t)dw * dh * 4);
    ccmd *c = cmd_new(cv, CK_PUT);
    c->im = im;
    c->px = x;
    c->py = y;
    cv_touch(cv);
}

void embi_canvas_flush(embi_canvas *cv)
{
    if (cv->env.gpu != nullptr || cv->gpu != nullptr) {
        cgpu_flush_canvas(cv, nullptr);
    }
}

bool embi_canvas_to_png(embi_canvas *cv, embi_sb *out)
{
    if (cv->w == 0 || cv->h == 0 || !cv_size_ok(cv->w, cv->h)) {
        return false;
    }
    size_t n = (size_t)cv->w * cv->h * 4;
    uint8_t *px = embi_alloc(n);
    (void)embi_canvas_read(cv, 0, 0, cv->w, cv->h, px);
    embi_png_encode_rgba(px, cv->w, cv->h, out);
    embi_free(px, n);
    return true;
}
