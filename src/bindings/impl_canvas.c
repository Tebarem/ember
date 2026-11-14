/*
 * HTMLCanvasElement and CanvasRenderingContext2D (canvas.webidl, R4b). The context wrapper's ptr is the embi_canvas of
 * src/canvas; its owned memory (a cvbox) holds the canvas and the host reference the canvas hooks call through, and its
 * release function destroys the canvas. The wrapper keeps the element wrapper alive (owner), and the element wrapper
 * keeps the context wrapper in a same-object slot, so getContext('2d') returns one object for the life of the element.
 */
#include "bindings/impl_canvas.h"

#include "bindings/canvas_host.h"
#include "css/css.h"
#include "css/props.h"
#include "style/style.h"

#include <math.h>
#include <string.h>

/* The same-object slot of the element wrapper that holds the 2d context (above every slot the generated code uses). */
#define CV_CTX_SLOT 60u
/* The same-object slots of the context wrapper that hold the objects assigned to fillStyle and strokeStyle. */
#define CV_STYLE_SLOT 0u

/* ------------------------------------------------------------------ the host of a binding context */

void cvh_images_clear(cvhost *h)
{
    for (uint32_t i = 0; i < h->imgs.len; i++) {
        embi_free(h->imgs.data[i].url, h->imgs.data[i].len + 1);
        embi_cimage_release(h->imgs.data[i].im);
    }
    h->imgs.len = 0;
    h->img_bytes = 0;
}

void cvh_unref(cvhost *h)
{
    if (h != nullptr && --h->rc == 0) {
        cvh_images_clear(h);
        embi_vec_free(&h->imgs);
        embi_free(h, sizeof *h);
    }
}

static void cvh_dirty(void *user)
{
    cvhost *h = user;
    if (h->base.dirty != nullptr) {
        h->base.dirty(h->base.user);
    }
}

static bool cvh_pixels(void *user, embi_str url, const uint8_t **px, uint32_t *w, uint32_t *h)
{
    cvhost *ho = user;
    return ho->base.image_pixels != nullptr && ho->base.image_pixels(ho->base.user, url, px, w, h);
}

/* The GPU of a new canvas when the host made none yet (a windowed view creates its device on first need). */
static embi_canvas_gpu *cvh_gpu(void *user)
{
    cvhost *ho = user;
    return ho->base.gpu_get != nullptr ? ho->base.gpu_get(ho->base.user) : nullptr;
}

/* The text of a computed property of an element, or an empty string. */
static bool computed_text(embi_bind_ctx *c, embi_element *el, embi_prop_id prop, embi_sb *sb)
{
    embi_style_engine *se = embi_bind_style(c);
    if (se == nullptr || el == nullptr) {
        return false;
    }
    embi_style_resolve_document(se, el->base.doc);
    const embi_style *s = embi_style_get(se, el, EMBI_PE_NONE);
    return s != nullptr && embi_style_value_text(s, prop, sb);
}

static float parse_px(const embi_sb *sb, float def)
{
    if (sb->len == 0) {
        return def;
    }
    char buf[48];
    size_t n = sb->len < sizeof buf - 1 ? sb->len : sizeof buf - 1;
    memcpy(buf, sb->data, n);
    buf[n] = '\0';
    char *end = nullptr;
    double v = SDL_strtod(buf, &end);
    return (end != buf && v > 0.0) ? (float)v : def;
}

static void cvh_style(void *user, const void *elp, float *font_px, uint32_t *color, bool *rtl, float *root_px)
{
    cvhost *h = user;
    if (h->c == nullptr || elp == nullptr) {
        return;
    }
    embi_element *el = (embi_element *)elp;
    embi_style_engine *se = embi_bind_style(h->c);
    uint64_t epoch = 0;
    if (se != nullptr) {
        embi_style_resolve_document(se, el->base.doc); /* a no-op while the document is clean */
        epoch = embi_style_document_epoch(se, el->base.doc);
        if (epoch != 0 && h->sty_el == el && h->sty_epoch == epoch) {
            *font_px = h->sty_font_px;
            *root_px = h->sty_root_px;
            *color = h->sty_color;
            *rtl = h->sty_rtl;
            return;
        }
    }
    embi_sb sb = {0};
    if (computed_text(h->c, el, EMBI_PROP_font_size, &sb)) {
        *font_px = parse_px(&sb, *font_px);
    }
    embi_sb_free(&sb);
    if (computed_text(h->c, el, EMBI_PROP_color, &sb)) {
        uint32_t rgba;
        if (embi_canvas_parse_color((embi_str){sb.data, sb.len}, &rgba)) {
            *color = rgba;
        }
    }
    embi_sb_free(&sb);
    memset(&sb, 0, sizeof sb);
    if (computed_text(h->c, el, EMBI_PROP_direction, &sb)) {
        *rtl = sb.len == 3 && memcmp(sb.data, "rtl", 3) == 0;
    }
    embi_sb_free(&sb);
    memset(&sb, 0, sizeof sb);
    embi_element *root = embi_document_element(el->base.doc);
    if (root != nullptr && computed_text(h->c, root, EMBI_PROP_font_size, &sb)) {
        *root_px = parse_px(&sb, *root_px);
    }
    embi_sb_free(&sb);
    if (epoch != 0) {
        h->sty_el = el;
        h->sty_epoch = epoch;
        h->sty_font_px = *font_px;
        h->sty_root_px = *root_px;
        h->sty_color = *color;
        h->sty_rtl = *rtl;
    }
}

void embi_canvas_bind_install(embi_bind_ctx *c, const embi_canvas_env *env)
{
    if (c->canvas_host != nullptr) {
        cvhost *old = c->canvas_host;
        old->c = nullptr;
        cvh_unref(old);
        c->canvas_host = nullptr;
    }
    if (env == nullptr) {
        return;
    }
    cvhost *h = embi_calloc(1, sizeof *h);
    h->rc = 1;
    h->c = c;
    h->base = *env;
    c->canvas_host = h;
}

void embi_impl_canvas_release(embi_bind_ctx *c)
{
    cvhost *h = c->canvas_host;
    c->canvas_host = nullptr;
    if (h != nullptr) {
        h->c = nullptr;
        cvh_unref(h);
    }
}

/* ------------------------------------------------------------------ errors */

embi_dom_err cv_throw(embi_bind_ctx *c, int code, const char *what)
{
    switch (code) {
    case EMBI_CV_INDEX_SIZE: return embi_impl_throw(c, "IndexSizeError", what);
    case EMBI_CV_INVALID_STATE: return embi_impl_throw(c, "InvalidStateError", what);
    case EMBI_CV_SYNTAX: return embi_impl_throw(c, "SyntaxError", what);
    case EMBI_CV_SECURITY: return embi_impl_throw(c, "SecurityError", what);
    case EMBI_CV_TYPE:
        (void)embi_js_throw_type_error(c->rt, "%s", what);
        return EMBI_BIND_THREW;
    default: return EMBI_DOM_OK;
    }
}

/* ------------------------------------------------------------------ the element */

/* The HTML "rules for parsing non-negative integers" on a content attribute; def when it is absent or invalid. */
static uint32_t dim_attr(embi_element *el, const char *name, uint32_t def)
{
    const embi_attr *a = embi_el_get_attribute(el, (embi_str){name, (uint32_t)strlen(name)});
    if (a == nullptr) {
        return def;
    }
    embi_sb sb = {0};
    embi_dstr_append_utf8(&sb, a->value);
    const char *p = sb.data;
    size_t n = sb.len, i = 0;
    while (i < n && (p[i] == ' ' || p[i] == '\t' || p[i] == '\n' || p[i] == '\f' || p[i] == '\r')) {
        i++;
    }
    uint32_t r = def;
    if (i < n && p[i] >= '0' && p[i] <= '9') {
        uint64_t v = 0;
        while (i < n && p[i] >= '0' && p[i] <= '9') {
            v = v * 10 + (uint64_t)(p[i] - '0');
            if (v > 2147483647u) {
                v = 0xFFFFFFFFFull;
                break;
            }
            i++;
        }
        r = v > 2147483647u ? def : (uint32_t)v;
    } else if (i < n && p[i] == '+') {
        i++;
        if (i < n && p[i] >= '0' && p[i] <= '9') {
            uint64_t v = 0;
            while (i < n && p[i] >= '0' && p[i] <= '9') {
                v = v * 10 + (uint64_t)(p[i] - '0');
                if (v > 2147483647u) {
                    v = 0xFFFFFFFFFull;
                    break;
                }
                i++;
            }
            r = v > 2147483647u ? def : (uint32_t)v;
        }
    }
    embi_sb_free(&sb);
    return r;
}

static uint32_t el_width(embi_element *el)
{
    return dim_attr(el, "width", 300);
}

static uint32_t el_height(embi_element *el)
{
    return dim_attr(el, "height", 150);
}

uint32_t embi_impl_HTMLCanvasElement_get_width(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    return el_width((embi_element *)self);
}

uint32_t embi_impl_HTMLCanvasElement_get_height(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    return el_height((embi_element *)self);
}

void embi_impl_HTMLCanvasElement_set_width(embi_bind_ctx *c, embi_node *self, uint32_t a0)
{
    (void)c;
    embi_bind_reflect_set_ulong((embi_element *)self, EMBI_STR("width"), a0 > 2147483647u ? 300u : a0);
    cv_sync_size((embi_element *)self, true);
}

void embi_impl_HTMLCanvasElement_set_height(embi_bind_ctx *c, embi_node *self, uint32_t a0)
{
    (void)c;
    embi_bind_reflect_set_ulong((embi_element *)self, EMBI_STR("height"), a0 > 2147483647u ? 150u : a0);
    cv_sync_size((embi_element *)self, true);
}

/* Resets the canvas of the element to its attribute size. force: even when the size is the same (the attribute was set). */
void cv_sync_size(embi_element *el, bool force)
{
    embi_canvas *cv = embi_canvas_find(el);
    if (cv == nullptr) {
        return;
    }
    uint32_t w = el_width(el), h = el_height(el);
    if (force || w != embi_canvas_width(cv) || h != embi_canvas_height(cv)) {
        embi_canvas_resize(cv, w, h);
    }
}

/* Called by the context's attribute observer. */
void embi_impl_canvas_attr_changed(embi_bind_ctx *c, embi_element *el, embi_atom local)
{
    (void)c;
    embi_str n = embi_atom_str(local);
    if (((n.len == 5 && memcmp(n.ptr, "width", 5) == 0) || (n.len == 6 && memcmp(n.ptr, "height", 6) == 0)) &&
        embi_canvas_find(el) != nullptr) {
        cv_sync_size(el, true);
    }
}

/* ------------------------------------------------------------------ getContext */

typedef struct cvbox {
    embi_canvas *cv;
    cvhost *host;
    bool will_read;
} cvbox;

static void cvbox_release(void *owned)
{
    cvbox *b = owned;
    embi_canvas_destroy(b->cv);
    cvh_unref(b->host);
}

/* The canvas of a context wrapper. */
embi_canvas *cv_of(embi_bind_obj *self)
{
    return self->ptr;
}

static bool dict_bool(embi_bind_ctx *c, embi_js_value opts, const char *name, bool def)
{
    embi_js_value v;
    if (!embi_js_is_object(opts) || !embi_bind_dict_get(c, opts, name, &v) || embi_js_is_undefined(v)) {
        return def;
    }
    return embi_js_to_boolean(v);
}

embi_js_value embi_impl_HTMLCanvasElement_getContext(embi_bind_ctx *c, embi_node *self, embi_dstr a0, embi_js_value a1)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_str id = embi_bind_utf8(c, a0);
    embi_element *el = (embi_element *)self;
    if (!(id.len == 2 && memcmp(id.ptr, "2d", 2) == 0)) {
        return embi_bind_leave(sc, EMBI_JS_NULL);
    }
    embi_bind_obj *rec = embi_impl_node_rec(c, self);
    embi_js_value *slot = embi_bind_same(rec, CV_CTX_SLOT);
    if (!embi_js_is_empty(*slot) && !embi_js_is_undefined(*slot) && !embi_js_is_null(*slot)) {
        return embi_bind_leave(sc, *slot);
    }
    cvhost *h = c->canvas_host;
    bool alpha = dict_bool(c, a1, "alpha", true);
    bool will_read = dict_bool(c, a1, "willReadFrequently", false);
    embi_canvas_env env;
    memset(&env, 0, sizeof env);
    if (h != nullptr) {
        env = h->base;
        env.user = h;
        env.dirty = cvh_dirty;
        env.image_pixels = cvh_pixels;
        env.image_gen = nullptr; /* the host keeps the image cache itself */
        env.element_style = cvh_style;
        env.gpu_get = h->base.gpu_get != nullptr ? cvh_gpu : nullptr; /* env.user is the host now: forward */
        h->rc++;
    }
    embi_canvas *cv = embi_canvas_create(&env, el, el_width(el), el_height(el), alpha);
    cvbox *box = embi_alloc(sizeof *box);
    box->cv = cv;
    box->host = h;
    box->will_read = will_read;
    embi_js_value v = embi_bind_other_value(c, EMBI_IF_CanvasRenderingContext2D, cv, box, sizeof *box);
    embi_bind_obj *bo = embi_bind_obj_of(v);
    bo->release = cvbox_release;
    bo->owner = rec;
    *slot = v;
    EMBI_JS_WB_STRUCT(rec->obj);
    return embi_bind_leave(sc, v);
}

embi_node *embi_impl_CanvasRenderingContext2D_get_canvas(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    return (embi_node *)embi_canvas_key(cv_of(self));
}

embi_js_value embi_impl_CanvasRenderingContext2D_getContextAttributes(embi_bind_ctx *c, embi_bind_obj *self)
{
    embi_canvas *cv = cv_of(self);
    cvbox *box = self->owned;
    embi_js_value o = embi_bind_object_new(c);
    embi_bind_object_set_c(c, o, "alpha", embi_js_from_bool(embi_canvas_has_alpha(cv)));
    embi_bind_object_set_c(c, o, "colorSpace", embi_bind_utf8_value(c, EMBI_STR("srgb")));
    embi_bind_object_set_c(c, o, "desynchronized", embi_js_from_bool(false));
    embi_bind_object_set_c(c, o, "willReadFrequently", embi_js_from_bool(box != nullptr && box->will_read));
    return o;
}

bool embi_impl_CanvasRenderingContext2D_isContextLost(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    (void)self;
    return false;
}

/* ------------------------------------------------------------------ toDataURL and toBlob */

static bool canvas_png(embi_element *el, embi_sb *png)
{
    embi_canvas *cv = embi_canvas_find(el);
    uint32_t w = el_width(el), h = el_height(el);
    if (w == 0 || h == 0 || !embi_canvas_size_ok(w, h)) {
        return false; /* no bitmap: toDataURL gives "data:," and toBlob a null blob */
    }
    if (cv == nullptr) {
        /* No context: a transparent bitmap of the element's size. */
        uint8_t *px = embi_calloc((size_t)w * h, 4);
        embi_png_encode_rgba(px, w, h, png);
        embi_free(px, (size_t)w * h * 4);
        return true;
    }
    return embi_canvas_to_png(cv, png);
}

embi_dom_err embi_impl_HTMLCanvasElement_toDataURL(embi_bind_ctx *c, embi_node *self, embi_dstr a0, embi_js_value a1,
                                                   embi_dstr *out)
{
    (void)a0;
    (void)a1;
    embi_element *el = (embi_element *)self;
    embi_sb png = {0};
    if (!canvas_png(el, &png)) {
        embi_sb_free(&png);
        *out = embi_impl_lit("data:,");
        return EMBI_DOM_OK;
    }
    embi_sb b64 = {0};
    embi_base64_encode((const unsigned char *)png.data, png.len, &b64);
    embi_sb url = {0};
    embi_sb_append(&url, EMBI_STR("data:image/png;base64,"));
    embi_sb_append(&url, (embi_str){b64.data, b64.len});
    *out = embi_bind_tmp_utf8(c, (embi_str){url.data, url.len});
    embi_sb_free(&png);
    embi_sb_free(&b64);
    embi_sb_free(&url);
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_HTMLCanvasElement_toBlob(embi_bind_ctx *c, embi_node *self, embi_js_value a0, embi_dstr a1,
                                                embi_js_value a2)
{
    (void)a1;
    (void)a2;
    if (!embi_js_is_callable(a0)) {
        (void)embi_js_throw_type_error(c->rt, "Failed to execute 'toBlob' on 'HTMLCanvasElement': The callback provided as parameter 1 is not a function.");
        return EMBI_BIND_THREW;
    }
    embi_element *el = (embi_element *)self;
    embi_sb png = {0};
    embi_js_value arg = EMBI_JS_NULL;
    if (canvas_png(el, &png)) {
        arg = ldr_blob_new(c, (const uint8_t *)png.data, png.len, EMBI_STR("image/png"));
    }
    embi_sb_free(&png);
    embi_js_value r = embi_js_call(c->rt, a0, EMBI_JS_UNDEFINED, 1, &arg);
    if (r == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(c->rt); /* an exception of the callback is reported, not thrown to the caller */
    }
    return EMBI_DOM_OK;
}

/* ------------------------------------------------------------------ state */

void embi_impl_CanvasRenderingContext2D_save(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    embi_canvas_save(cv_of(self));
}

embi_dom_err embi_impl_CanvasRenderingContext2D_restore(embi_bind_ctx *c, embi_bind_obj *self)
{
    return cv_throw(c, embi_canvas_restore(cv_of(self)), "restore() cannot close a layer opened by beginLayer().");
}

void embi_impl_CanvasRenderingContext2D_beginLayer(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value a0)
{
    (void)c;
    (void)a0;
    embi_canvas_begin_layer(cv_of(self));
}

embi_dom_err embi_impl_CanvasRenderingContext2D_endLayer(embi_bind_ctx *c, embi_bind_obj *self)
{
    return cv_throw(c, embi_canvas_end_layer(cv_of(self)), "endLayer() needs an open layer on top of the state stack.");
}

void embi_impl_CanvasRenderingContext2D_reset(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    embi_canvas_reset(cv_of(self));
}

void embi_impl_CanvasRenderingContext2D_scale(embi_bind_ctx *c, embi_bind_obj *self, double a0, double a1)
{
    (void)c;
    embi_canvas_scale(cv_of(self), a0, a1);
}

void embi_impl_CanvasRenderingContext2D_rotate(embi_bind_ctx *c, embi_bind_obj *self, double a0)
{
    (void)c;
    embi_canvas_rotate(cv_of(self), a0);
}

void embi_impl_CanvasRenderingContext2D_translate(embi_bind_ctx *c, embi_bind_obj *self, double a0, double a1)
{
    (void)c;
    embi_canvas_translate(cv_of(self), a0, a1);
}

void embi_impl_CanvasRenderingContext2D_transform(embi_bind_ctx *c, embi_bind_obj *self, double a0, double a1, double a2,
                                                  double a3, double a4, double a5)
{
    (void)c;
    const double m[6] = {a0, a1, a2, a3, a4, a5};
    embi_canvas_transform(cv_of(self), m);
}

embi_js_value embi_impl_CanvasRenderingContext2D_getTransform(embi_bind_ctx *c, embi_bind_obj *self)
{
    double m[6];
    embi_canvas_get_transform(cv_of(self), m);
    return embi_svg_dommatrix_new(c, m);
}

void embi_impl_CanvasRenderingContext2D_setTransform_0(embi_bind_ctx *c, embi_bind_obj *self, double a0, double a1,
                                                       double a2, double a3, double a4, double a5)
{
    (void)c;
    const double m[6] = {a0, a1, a2, a3, a4, a5};
    embi_canvas_set_transform(cv_of(self), m);
}

embi_dom_err embi_impl_CanvasRenderingContext2D_setTransform_1(embi_bind_ctx *c, embi_bind_obj *self,
                                                               embi_dict_DOMMatrix2DInit a0)
{
    double m16[16];
    if (!embi_svg_matrix_from_init(c, &a0, m16)) {
        return EMBI_BIND_THREW;
    }
    const double m[6] = {m16[0], m16[1], m16[4], m16[5], m16[12], m16[13]};
    embi_canvas_set_transform(cv_of(self), m);
    return EMBI_DOM_OK;
}

void embi_impl_CanvasRenderingContext2D_resetTransform(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    const double m[6] = {1, 0, 0, 1, 0, 0};
    embi_canvas_set_transform(cv_of(self), m);
}

/* ------------------------------------------------------------------ compositing and smoothing */

double embi_impl_CanvasRenderingContext2D_get_globalAlpha(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    return embi_canvas_get_alpha(cv_of(self));
}

void embi_impl_CanvasRenderingContext2D_set_globalAlpha(embi_bind_ctx *c, embi_bind_obj *self, double a0)
{
    (void)c;
    embi_canvas_set_alpha(cv_of(self), a0);
}

embi_dstr embi_impl_CanvasRenderingContext2D_get_globalCompositeOperation(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    return embi_impl_lit(embi_canvas_get_composite(cv_of(self)));
}

void embi_impl_CanvasRenderingContext2D_set_globalCompositeOperation(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    embi_canvas_set_composite(cv_of(self), embi_bind_utf8(c, a0));
}

bool embi_impl_CanvasRenderingContext2D_get_imageSmoothingEnabled(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    return embi_canvas_get_smoothing(cv_of(self));
}

void embi_impl_CanvasRenderingContext2D_set_imageSmoothingEnabled(embi_bind_ctx *c, embi_bind_obj *self, bool a0)
{
    (void)c;
    embi_canvas_set_smoothing(cv_of(self), a0);
}

embi_dstr embi_impl_CanvasRenderingContext2D_get_imageSmoothingQuality(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    return embi_impl_lit(embi_canvas_get_quality(cv_of(self)));
}

void embi_impl_CanvasRenderingContext2D_set_imageSmoothingQuality(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    embi_canvas_set_quality(cv_of(self), embi_bind_utf8(c, a0));
}

/* ------------------------------------------------------------------ fill and stroke styles */

static embi_js_value get_style(embi_bind_ctx *c, embi_bind_obj *self, int which)
{
    embi_canvas *cv = cv_of(self);
    embi_sb css = {0};
    embi_cgrad *g = nullptr;
    embi_cpattern *p = nullptr;
    int kind = embi_canvas_get_style(cv, which, &css, &g, &p);
    embi_js_value r = EMBI_JS_UNDEFINED;
    if (kind == 0) {
        r = embi_bind_utf8_value(c, (embi_str){css.data, css.len});
    } else {
        /* The object the script assigned, when the slot still holds the wrapper of this very gradient or pattern. */
        embi_js_value *slot = embi_bind_same(self, CV_STYLE_SLOT + (uint32_t)which);
        embi_bind_obj *so = embi_js_is_empty(*slot) ? nullptr : embi_bind_obj_of(*slot);
        void *want = kind == 1 ? (void *)g : (void *)p;
        if (so != nullptr && so->ptr == want) {
            r = *slot;
        } else if (kind == 1) {
            r = cv_gradient_value(c, g);
        } else {
            r = cv_pattern_value(c, p);
        }
    }
    embi_sb_free(&css);
    return r;
}

static embi_dom_err set_style(embi_bind_ctx *c, embi_bind_obj *self, int which, embi_js_value v)
{
    embi_canvas *cv = cv_of(self);
    embi_bind_obj *o = embi_js_is_object(v) ? embi_bind_obj_of(v) : nullptr;
    if (o != nullptr && o->iface == EMBI_IF_CanvasGradient) {
        embi_canvas_set_gradient(cv, which, o->ptr);
    } else if (o != nullptr && o->iface == EMBI_IF_CanvasPattern) {
        embi_canvas_set_pattern(cv, which, o->ptr);
    } else {
        embi_dstr d;
        if (!embi_bind_conv_str(c, v, "fillStyle", false, &d)) {
            return EMBI_BIND_THREW;
        }
        if (!embi_canvas_set_color(cv, which, embi_bind_utf8(c, d))) {
            return EMBI_DOM_OK;
        }
        o = nullptr;
    }
    embi_js_value *slot = embi_bind_same(self, CV_STYLE_SLOT + (uint32_t)which);
    *slot = o != nullptr ? v : EMBI_JS_UNDEFINED;
    EMBI_JS_WB_STRUCT(self->obj);
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_CanvasRenderingContext2D_get_strokeStyle(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value *out)
{
    *out = get_style(c, self, 1);
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_CanvasRenderingContext2D_set_strokeStyle(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value a0)
{
    return set_style(c, self, 1, a0);
}

embi_dom_err embi_impl_CanvasRenderingContext2D_get_fillStyle(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value *out)
{
    *out = get_style(c, self, 0);
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_CanvasRenderingContext2D_set_fillStyle(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value a0)
{
    return set_style(c, self, 0, a0);
}

/* ------------------------------------------------------------------ shadows and filter */

double embi_impl_CanvasRenderingContext2D_get_shadowOffsetX(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    return embi_canvas_get_shadow_x(cv_of(self));
}

void embi_impl_CanvasRenderingContext2D_set_shadowOffsetX(embi_bind_ctx *c, embi_bind_obj *self, double a0)
{
    (void)c;
    embi_canvas_set_shadow_x(cv_of(self), a0);
}

double embi_impl_CanvasRenderingContext2D_get_shadowOffsetY(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    return embi_canvas_get_shadow_y(cv_of(self));
}

void embi_impl_CanvasRenderingContext2D_set_shadowOffsetY(embi_bind_ctx *c, embi_bind_obj *self, double a0)
{
    (void)c;
    embi_canvas_set_shadow_y(cv_of(self), a0);
}

double embi_impl_CanvasRenderingContext2D_get_shadowBlur(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    return embi_canvas_get_shadow_blur(cv_of(self));
}

void embi_impl_CanvasRenderingContext2D_set_shadowBlur(embi_bind_ctx *c, embi_bind_obj *self, double a0)
{
    (void)c;
    embi_canvas_set_shadow_blur(cv_of(self), a0);
}

embi_dstr embi_impl_CanvasRenderingContext2D_get_shadowColor(embi_bind_ctx *c, embi_bind_obj *self)
{
    embi_sb sb = {0};
    embi_canvas_get_shadow_color(cv_of(self), &sb);
    embi_dstr d = embi_bind_tmp_utf8(c, (embi_str){sb.data, sb.len});
    embi_sb_free(&sb);
    return d;
}

void embi_impl_CanvasRenderingContext2D_set_shadowColor(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    embi_canvas_set_shadow_color(cv_of(self), embi_bind_utf8(c, a0));
}

/* ------------------------------------------------------------------ text attributes (indices of embi_canvas_get_text_attr) */

static embi_dstr text_attr_get(embi_bind_ctx *c, embi_bind_obj *self, int which)
{
    embi_sb sb = {0};
    embi_canvas_get_text_attr(cv_of(self), which, &sb);
    embi_dstr d = embi_bind_tmp_utf8(c, (embi_str){sb.data, sb.len});
    embi_sb_free(&sb);
    return d;
}

static void text_attr_set(embi_bind_ctx *c, embi_bind_obj *self, int which, embi_dstr v)
{
    embi_canvas_set_text_attr(cv_of(self), which, embi_bind_utf8(c, v));
}

#define TEXT_ATTR(NAME, IDX)                                                                                      \
    embi_dstr embi_impl_CanvasRenderingContext2D_get_##NAME(embi_bind_ctx *c, embi_bind_obj *self)               \
    {                                                                                                            \
        return text_attr_get(c, self, IDX);                                                                      \
    }                                                                                                            \
    void embi_impl_CanvasRenderingContext2D_set_##NAME(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)      \
    {                                                                                                            \
        text_attr_set(c, self, IDX, a0);                                                                         \
    }

TEXT_ATTR(font, 0)
TEXT_ATTR(textAlign, 1)
TEXT_ATTR(textBaseline, 2)
TEXT_ATTR(direction, 3)
TEXT_ATTR(letterSpacing, 4)
TEXT_ATTR(wordSpacing, 5)
TEXT_ATTR(fontKerning, 6)
TEXT_ATTR(fontStretch, 7)
TEXT_ATTR(fontVariantCaps, 8)
TEXT_ATTR(textRendering, 9)
TEXT_ATTR(filter, 10)

/* ------------------------------------------------------------------ line styles */

double embi_impl_CanvasRenderingContext2D_get_lineWidth(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    return embi_canvas_get_line_width(cv_of(self));
}

void embi_impl_CanvasRenderingContext2D_set_lineWidth(embi_bind_ctx *c, embi_bind_obj *self, double a0)
{
    (void)c;
    embi_canvas_set_line_width(cv_of(self), a0);
}

embi_dstr embi_impl_CanvasRenderingContext2D_get_lineCap(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    return embi_impl_lit(embi_canvas_get_line_cap(cv_of(self)));
}

void embi_impl_CanvasRenderingContext2D_set_lineCap(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    embi_canvas_set_line_cap(cv_of(self), embi_bind_utf8(c, a0));
}

embi_dstr embi_impl_CanvasRenderingContext2D_get_lineJoin(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    return embi_impl_lit(embi_canvas_get_line_join(cv_of(self)));
}

void embi_impl_CanvasRenderingContext2D_set_lineJoin(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    embi_canvas_set_line_join(cv_of(self), embi_bind_utf8(c, a0));
}

double embi_impl_CanvasRenderingContext2D_get_miterLimit(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    return embi_canvas_get_miter_limit(cv_of(self));
}

void embi_impl_CanvasRenderingContext2D_set_miterLimit(embi_bind_ctx *c, embi_bind_obj *self, double a0)
{
    (void)c;
    embi_canvas_set_miter_limit(cv_of(self), a0);
}

void embi_impl_CanvasRenderingContext2D_setLineDash(embi_bind_ctx *c, embi_bind_obj *self, embi_bind_seq a0)
{
    (void)c;
    (void)embi_canvas_set_line_dash(cv_of(self), a0.data, a0.len);
}

embi_bind_seq embi_impl_CanvasRenderingContext2D_getLineDash(embi_bind_ctx *c, embi_bind_obj *self)
{
    const double *v = nullptr;
    uint32_t n = embi_canvas_get_line_dash(cv_of(self), &v);
    embi_bind_seq s = {nullptr, n};
    if (n > 0) {
        s.data = embi_bind_alloc(c, (size_t)n * sizeof(double));
        memcpy(s.data, v, (size_t)n * sizeof(double));
    }
    return s;
}

double embi_impl_CanvasRenderingContext2D_get_lineDashOffset(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    return embi_canvas_get_dash_offset(cv_of(self));
}

void embi_impl_CanvasRenderingContext2D_set_lineDashOffset(embi_bind_ctx *c, embi_bind_obj *self, double a0)
{
    (void)c;
    embi_canvas_set_dash_offset(cv_of(self), a0);
}

/* ------------------------------------------------------------------ rects, paths, text */

void embi_impl_CanvasRenderingContext2D_clearRect(embi_bind_ctx *c, embi_bind_obj *self, double a0, double a1, double a2,
                                                  double a3)
{
    (void)c;
    embi_canvas_clear_rect(cv_of(self), a0, a1, a2, a3);
}

void embi_impl_CanvasRenderingContext2D_fillRect(embi_bind_ctx *c, embi_bind_obj *self, double a0, double a1, double a2,
                                                 double a3)
{
    (void)c;
    embi_canvas_fill_rect(cv_of(self), a0, a1, a2, a3);
}

void embi_impl_CanvasRenderingContext2D_strokeRect(embi_bind_ctx *c, embi_bind_obj *self, double a0, double a1,
                                                   double a2, double a3)
{
    (void)c;
    embi_canvas_stroke_rect(cv_of(self), a0, a1, a2, a3);
}

void embi_impl_CanvasRenderingContext2D_beginPath(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    embi_canvas_begin_path(cv_of(self));
}

/* The fill rule argument: "evenodd" is even-odd; anything else that is not "nonzero" is a TypeError in the enum rule, which
 * the DOMString argument leaves to us: it is thrown by the caller through the return value. */
static bool rule_evenodd(embi_bind_ctx *c, embi_dstr rule, bool *evenodd)
{
    embi_str s = embi_bind_utf8(c, rule);
    if (s.len == 7 && memcmp(s.ptr, "nonzero", 7) == 0) {
        *evenodd = false;
        return true;
    }
    if (s.len == 7 && memcmp(s.ptr, "evenodd", 7) == 0) {
        *evenodd = true;
        return true;
    }
    (void)embi_js_throw_type_error(c->rt, "The provided value '%.*s' is not a valid enum value of type CanvasFillRule.",
                                   (int)s.len, s.ptr);
    return false;
}

void embi_impl_CanvasRenderingContext2D_fill_0(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    bool eo;
    if (rule_evenodd(c, a0, &eo)) {
        embi_canvas_fill(cv_of(self), nullptr, eo);
    }
}

void embi_impl_CanvasRenderingContext2D_fill_1(embi_bind_ctx *c, embi_bind_obj *self, void *a0, embi_dstr a1)
{
    bool eo;
    if (rule_evenodd(c, a1, &eo)) {
        embi_canvas_fill(cv_of(self), a0, eo);
    }
}

void embi_impl_CanvasRenderingContext2D_stroke_0(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    embi_canvas_stroke(cv_of(self), nullptr);
}

void embi_impl_CanvasRenderingContext2D_stroke_1(embi_bind_ctx *c, embi_bind_obj *self, void *a0)
{
    (void)c;
    embi_canvas_stroke(cv_of(self), a0);
}

void embi_impl_CanvasRenderingContext2D_clip_0(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    bool eo;
    if (rule_evenodd(c, a0, &eo)) {
        embi_canvas_clip(cv_of(self), nullptr, eo);
    }
}

void embi_impl_CanvasRenderingContext2D_clip_1(embi_bind_ctx *c, embi_bind_obj *self, void *a0, embi_dstr a1)
{
    bool eo;
    if (rule_evenodd(c, a1, &eo)) {
        embi_canvas_clip(cv_of(self), a0, eo);
    }
}

embi_dom_err embi_impl_CanvasRenderingContext2D_isPointInPath_0(embi_bind_ctx *c, embi_bind_obj *self, double a0, double a1,
                                                                embi_dstr a2, bool *out)
{
    bool eo;
    *out = false;
    if (!rule_evenodd(c, a2, &eo)) {
        return EMBI_BIND_THREW;
    }
    *out = embi_canvas_in_path(cv_of(self), nullptr, a0, a1, eo);
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_CanvasRenderingContext2D_isPointInPath_1(embi_bind_ctx *c, embi_bind_obj *self, void *a0, double a1,
                                                                double a2, embi_dstr a3, bool *out)
{
    bool eo;
    *out = false;
    if (!rule_evenodd(c, a3, &eo)) {
        return EMBI_BIND_THREW;
    }
    *out = embi_canvas_in_path(cv_of(self), a0, a1, a2, eo);
    return EMBI_DOM_OK;
}

bool embi_impl_CanvasRenderingContext2D_isPointInStroke_0(embi_bind_ctx *c, embi_bind_obj *self, double a0, double a1)
{
    (void)c;
    return embi_canvas_in_stroke(cv_of(self), nullptr, a0, a1);
}

bool embi_impl_CanvasRenderingContext2D_isPointInStroke_1(embi_bind_ctx *c, embi_bind_obj *self, void *a0, double a1,
                                                          double a2)
{
    (void)c;
    return embi_canvas_in_stroke(cv_of(self), a0, a1, a2);
}

void embi_impl_CanvasRenderingContext2D_drawFocusIfNeeded(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value a0,
                                                          embi_js_value a1)
{
    /* Out of scope (R4b): focus rings are not drawn. */
    (void)c;
    (void)self;
    (void)a0;
    (void)a1;
}

void embi_impl_CanvasRenderingContext2D_fillText(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0, double a1,
                                                 double a2, double a3)
{
    embi_canvas_draw_text(cv_of(self), embi_bind_utf8(c, a0), a1, a2, a3, false);
}

void embi_impl_CanvasRenderingContext2D_strokeText(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0, double a1,
                                                   double a2, double a3)
{
    embi_canvas_draw_text(cv_of(self), embi_bind_utf8(c, a0), a1, a2, a3, true);
}

embi_js_value embi_impl_CanvasRenderingContext2D_measureText(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    embi_text_metrics m;
    if (!embi_canvas_measure_text(cv_of(self), embi_bind_utf8(c, a0), &m)) {
        memset(&m, 0, sizeof m);
    }
    return cv_metrics_value(c, &m);
}

/* ------------------------------------------------------------------ path building on the context (CTM applied) */

#define CTX_PATH(self, m)                                                                                         \
    embi_canvas *cv = cv_of(self);                                                                               \
    double m[6];                                                                                                 \
    embi_canvas_get_transform(cv, m);                                                                            \
    embi_cpath *p = embi_canvas_path(cv)

void embi_impl_CanvasRenderingContext2D_closePath(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    CTX_PATH(self, m);
    (void)m;
    embi_cpath_close(p);
}

void embi_impl_CanvasRenderingContext2D_moveTo(embi_bind_ctx *c, embi_bind_obj *self, double a0, double a1)
{
    (void)c;
    CTX_PATH(self, m);
    embi_cpath_move_to(p, m, a0, a1);
}

void embi_impl_CanvasRenderingContext2D_lineTo(embi_bind_ctx *c, embi_bind_obj *self, double a0, double a1)
{
    (void)c;
    CTX_PATH(self, m);
    embi_cpath_line_to(p, m, a0, a1);
}

void embi_impl_CanvasRenderingContext2D_quadraticCurveTo(embi_bind_ctx *c, embi_bind_obj *self, double a0, double a1,
                                                         double a2, double a3)
{
    (void)c;
    CTX_PATH(self, m);
    embi_cpath_quad_to(p, m, a0, a1, a2, a3);
}

void embi_impl_CanvasRenderingContext2D_bezierCurveTo(embi_bind_ctx *c, embi_bind_obj *self, double a0, double a1,
                                                      double a2, double a3, double a4, double a5)
{
    (void)c;
    CTX_PATH(self, m);
    embi_cpath_cubic_to(p, m, a0, a1, a2, a3, a4, a5);
}

embi_dom_err embi_impl_CanvasRenderingContext2D_arcTo(embi_bind_ctx *c, embi_bind_obj *self, double a0, double a1,
                                                      double a2, double a3, double a4)
{
    CTX_PATH(self, m);
    return cv_throw(c, embi_cpath_arc_to(p, m, a0, a1, a2, a3, a4), "The radius provided is negative.");
}

void embi_impl_CanvasRenderingContext2D_rect(embi_bind_ctx *c, embi_bind_obj *self, double a0, double a1, double a2,
                                             double a3)
{
    (void)c;
    CTX_PATH(self, m);
    embi_cpath_rect(p, m, a0, a1, a2, a3);
}

embi_dom_err embi_impl_CanvasRenderingContext2D_roundRect(embi_bind_ctx *c, embi_bind_obj *self, double a0, double a1,
                                                          double a2, double a3, embi_js_value a4)
{
    CTX_PATH(self, m);
    return cv_round_rect(c, p, m, a0, a1, a2, a3, a4);
}

embi_dom_err embi_impl_CanvasRenderingContext2D_arc(embi_bind_ctx *c, embi_bind_obj *self, double a0, double a1,
                                                    double a2, double a3, double a4, bool a5)
{
    CTX_PATH(self, m);
    return cv_throw(c, embi_cpath_arc(p, m, a0, a1, a2, a3, a4, a5), "The radius provided is negative.");
}

embi_dom_err embi_impl_CanvasRenderingContext2D_ellipse(embi_bind_ctx *c, embi_bind_obj *self, double a0, double a1,
                                                        double a2, double a3, double a4, double a5, double a6, bool a7)
{
    CTX_PATH(self, m);
    return cv_throw(c, embi_cpath_ellipse(p, m, a0, a1, a2, a3, a4, a5, a6, a7), "The radius provided is negative.");
}
