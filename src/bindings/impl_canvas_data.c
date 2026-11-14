/*
 * The picture objects of canvas.webidl (R4b): CanvasGradient, CanvasPattern, TextMetrics, ImageData, ImageBitmap and
 * createImageBitmap, and the context members that make or consume them (gradients, patterns, drawImage and the pixel
 * methods). Pictures cross into src/canvas as straight RGBA8 embi_cimage values.
 */
#include "bindings/impl_canvas.h"

#include "bindings/gen/dom.h"
#include "bindings/gen/window.h"
#include "bindings/impl_loader.h"
#include "image/image.h"
#include "js/typedarray.h"
#include "loader/loader.h"

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------ boxes */

typedef struct rbox {
    void *p;
} rbox;

static void grad_release(void *owned)
{
    embi_cgrad_release(((rbox *)owned)->p);
}

static void pat_release(void *owned)
{
    embi_cpattern_release(((rbox *)owned)->p);
}

static void bmp_release(void *owned)
{
    rbox *b = owned;
    embi_cimage_release(b->p);
    b->p = nullptr;
}

static embi_js_value ref_wrap(embi_bind_ctx *c, uint16_t iface, void *p, void (*rel)(void *), embi_bind_obj *owner)
{
    rbox *b = embi_alloc(sizeof *b);
    b->p = p;
    embi_js_value v = embi_bind_other_value(c, iface, p, b, sizeof *b);
    embi_bind_obj *bo = embi_bind_obj_of(v);
    bo->release = rel;
    bo->owner = owner;
    return v;
}

embi_js_value cv_gradient_value(embi_bind_ctx *c, embi_cgrad *g)
{
    return ref_wrap(c, EMBI_IF_CanvasGradient, embi_cgrad_retain(g), grad_release, nullptr);
}

embi_js_value cv_pattern_value(embi_bind_ctx *c, embi_cpattern *p)
{
    return ref_wrap(c, EMBI_IF_CanvasPattern, embi_cpattern_retain(p), pat_release, nullptr);
}

/* ------------------------------------------------------------------ gradients */

embi_dom_err embi_impl_CanvasGradient_addColorStop(embi_bind_ctx *c, embi_bind_obj *self, double a0, embi_dstr a1)
{
    if (!isfinite(a0)) {
        return cv_throw(c, EMBI_CV_TYPE, "The provided double value is non-finite.");
    }
    embi_canvas *cv = self->owner != nullptr ? cv_of(self->owner) : nullptr;
    int e = embi_cgrad_add_stop(self->ptr, cv, a0, embi_bind_utf8(c, a1));
    return cv_throw(c, e, e == EMBI_CV_INDEX_SIZE ? "The offset provided is outside the range [0, 1]."
                                                  : "The color provided could not be parsed.");
}

static embi_js_value new_gradient(embi_bind_ctx *c, embi_bind_obj *self, int kind, const double g[6])
{
    for (int i = 0; i < (kind == EMBI_CG_LINEAR ? 4 : (kind == EMBI_CG_RADIAL ? 6 : 3)); i++) {
        if (!isfinite(g[i])) {
            (void)embi_js_throw_type_error(c->rt, "The provided double value is non-finite.");
            return EMBI_JS_EXCEPTION;
        }
    }
    if (kind == EMBI_CG_RADIAL && (g[2] < 0.0 || g[5] < 0.0)) {
        (void)cv_throw(c, EMBI_CV_INDEX_SIZE, "The radius provided is negative.");
        return EMBI_JS_EXCEPTION;
    }
    return ref_wrap(c, EMBI_IF_CanvasGradient, embi_cgrad_new(kind, g), grad_release, self);
}

embi_dom_err embi_impl_CanvasRenderingContext2D_createLinearGradient(embi_bind_ctx *c, embi_bind_obj *self, double a0,
                                                                     double a1, double a2, double a3, embi_js_value *out)
{
    const double g[6] = {a0, a1, a2, a3, 0, 0};
    *out = new_gradient(c, self, EMBI_CG_LINEAR, g);
    return *out == EMBI_JS_EXCEPTION ? EMBI_BIND_THREW : EMBI_DOM_OK;
}

embi_dom_err embi_impl_CanvasRenderingContext2D_createRadialGradient(embi_bind_ctx *c, embi_bind_obj *self, double a0,
                                                                     double a1, double a2, double a3, double a4,
                                                                     double a5, embi_js_value *out)
{
    const double g[6] = {a0, a1, a2, a3, a4, a5};
    *out = new_gradient(c, self, EMBI_CG_RADIAL, g);
    return *out == EMBI_JS_EXCEPTION ? EMBI_BIND_THREW : EMBI_DOM_OK;
}

embi_dom_err embi_impl_CanvasRenderingContext2D_createConicGradient(embi_bind_ctx *c, embi_bind_obj *self, double a0,
                                                                    double a1, double a2, embi_js_value *out)
{
    const double g[6] = {a0, a1, a2, 0, 0, 0};
    *out = new_gradient(c, self, EMBI_CG_CONIC, g);
    return *out == EMBI_JS_EXCEPTION ? EMBI_BIND_THREW : EMBI_DOM_OK;
}

embi_dom_err embi_impl_CanvasPattern_setTransform(embi_bind_ctx *c, embi_bind_obj *self, embi_dict_DOMMatrix2DInit a0)
{
    double m16[16];
    if (!embi_svg_matrix_from_init(c, &a0, m16)) {
        return EMBI_BIND_THREW;
    }
    const double m[6] = {m16[0], m16[1], m16[4], m16[5], m16[12], m16[13]};
    embi_cpattern_set_transform(self->ptr, m);
    return EMBI_DOM_OK;
}

/* ------------------------------------------------------------------ image sources */

#define CV_IMG_CACHE_MAX 32u
#define CV_IMG_CACHE_BYTES ((size_t)64 << 20)

/* The picture of an img element (src resolved against the document URL), a retained shared copy of the embedder's decoded
 * pixels, or nullptr when it is not drawable. The copy is made once per URL and image generation; every drawImage of the
 * same sprite then names the same picture and its GPU texture. */
static embi_cimage *img_picture(embi_bind_ctx *c, embi_element *el)
{
    cvhost *ho = c->canvas_host;
    if (ho == nullptr || ho->base.image_pixels == nullptr) {
        return nullptr;
    }
    const embi_attr *a = embi_el_get_attribute(el, EMBI_STR("src"));
    if (a == nullptr || a->value.len == 0) {
        return nullptr;
    }
    embi_sb src = {0};
    embi_dstr_append_utf8(&src, a->value);
    embi_str base_s = c->host.url != nullptr ? c->host.url(c->host.user) : EMBI_STR("about:blank");
    embi_url base, u;
    embi_cimage *res = nullptr;
    if (embi_url_parse(base_s, nullptr, &base)) {
        if (embi_url_parse((embi_str){src.data, src.len}, &base, &u)) {
            embi_sb href = {0};
            embi_url_get(&u, EMBI_URL_HREF, &href);
            embi_url_free(&u);
            bool cacheable = ho->base.image_gen != nullptr;
            if (cacheable) {
                uint32_t gen = ho->base.image_gen(ho->base.user);
                if (gen != ho->img_gen) {
                    cvh_images_clear(ho);
                    ho->img_gen = gen;
                }
                for (uint32_t i = 0; i < ho->imgs.len; i++) {
                    if (ho->imgs.data[i].len == href.len && memcmp(ho->imgs.data[i].url, href.data, href.len) == 0) {
                        res = embi_cimage_retain(ho->imgs.data[i].im);
                        break;
                    }
                }
            }
            if (res == nullptr) {
                const uint8_t *px;
                uint32_t w, h;
                if (ho->base.image_pixels(ho->base.user, (embi_str){href.data, href.len}, &px, &w, &h) && w > 0 && h > 0 &&
                    embi_canvas_size_ok(w, h)) {
                    res = embi_cimage_new(w, h);
                    memcpy(res->px, px, (size_t)w * h * 4);
                    if (cacheable) {
                        ho->img_gen = ho->base.image_gen(ho->base.user); /* the fetch above may have moved it */
                        size_t bytes = (size_t)w * h * 4;
                        while (ho->imgs.len > 0 && (ho->imgs.len >= CV_IMG_CACHE_MAX || ho->img_bytes + bytes > CV_IMG_CACHE_BYTES)) {
                            cvimg *old = &ho->imgs.data[0];
                            ho->img_bytes -= (size_t)old->im->w * old->im->h * 4;
                            embi_free(old->url, old->len + 1);
                            embi_cimage_release(old->im);
                            memmove(old, old + 1, (ho->imgs.len - 1) * sizeof *old);
                            ho->imgs.len--;
                        }
                        char *url = embi_alloc(href.len + 1);
                        memcpy(url, href.data, href.len);
                        url[href.len] = '\0';
                        embi_vec_push(&ho->imgs, ((cvimg){url, (uint32_t)href.len, embi_cimage_retain(res)}));
                        ho->img_bytes += bytes;
                    }
                }
            }
            embi_sb_free(&href);
        }
        embi_url_free(&base);
    }
    embi_sb_free(&src);
    return res;
}

typedef struct picture {
    embi_cimage *im;   /* retained (a copy for an element, the bitmap's own for an ImageBitmap), or nullptr */
    embi_canvas *src;  /* a canvas source */
    uint32_t w, h;
} picture;

/* Classifies an image source. 1: usable; 0: nothing to draw (not loaded); -1: an exception is pending. */
static int pic_from(embi_bind_ctx *c, embi_js_value v, picture *out, const char *what)
{
    memset(out, 0, sizeof *out);
    embi_bind_obj *bo = embi_js_is_object(v) ? embi_bind_obj_of(v) : nullptr;
    if (bo == nullptr) {
        (void)embi_js_throw_type_error(c->rt, "%s: The provided value is not of type '(CSSImageValue or HTMLImageElement or SVGImageElement or HTMLVideoElement or HTMLCanvasElement or ImageBitmap or OffscreenCanvas or VideoFrame)'.", what);
        return -1;
    }
    if (bo->iface == EMBI_IF_ImageBitmap) {
        rbox *b = bo->owned;
        if (b == nullptr || b->p == nullptr) {
            (void)cv_throw(c, EMBI_CV_INVALID_STATE, "The image source is detached.");
            return -1;
        }
        out->im = embi_cimage_retain(b->p);
        out->w = out->im->w;
        out->h = out->im->h;
        return 1;
    }
    if (bo->is_node && embi_bind_iface_is(bo->iface, EMBI_IF_HTMLCanvasElement)) {
        embi_element *el = bo->ptr;
        uint32_t w, h;
        embi_canvas *src = embi_canvas_find(el);
        if (src != nullptr) {
            w = embi_canvas_width(src);
            h = embi_canvas_height(src);
        } else {
            w = embi_impl_HTMLCanvasElement_get_width(c, &el->base);
            h = embi_impl_HTMLCanvasElement_get_height(c, &el->base);
        }
        if (w == 0 || h == 0) {
            (void)cv_throw(c, EMBI_CV_INVALID_STATE, "The canvas has a width or height of zero.");
            return -1;
        }
        out->w = w;
        out->h = h;
        if (src != nullptr) {
            out->src = src;
        } else {
            out->im = embi_cimage_new(w, h); /* never drawn on: transparent black */
        }
        return 1;
    }
    if (bo->is_node && embi_bind_iface_is(bo->iface, EMBI_IF_HTMLImageElement)) {
        embi_cimage *im = img_picture(c, bo->ptr);
        if (im == nullptr) {
            return 0;
        }
        out->im = im;
        out->w = im->w;
        out->h = im->h;
        return 1;
    }
    if (bo->is_node && embi_bind_iface_is(bo->iface, EMBI_IF_SVGImageElement)) {
        return 0; /* SVG images are not decoded for the canvas (labelled not-yet): not drawable, nothing is drawn */
    }
    (void)embi_js_throw_type_error(c->rt, "%s: The provided value is not of a supported image type.", what);
    return -1;
}

static void pic_free(picture *p)
{
    if (p->im != nullptr) {
        embi_cimage_release(p->im);
    }
}

static embi_dom_err draw_image(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value img, const double s[4],
                               const double d[4], bool has_src)
{
    picture p;
    int r = pic_from(c, img, &p, "Failed to execute 'drawImage' on 'CanvasRenderingContext2D'");
    if (r < 0) {
        return EMBI_BIND_THREW;
    }
    if (r == 0) {
        return EMBI_DOM_OK;
    }
    double sr[4] = {0, 0, (double)p.w, (double)p.h};
    if (has_src) {
        memcpy(sr, s, sizeof sr);
    }
    double dr[4] = {d[0], d[1], d[2], d[3]};
    int e = embi_canvas_draw_image(cv_of(self), p.im, p.src, sr, dr);
    pic_free(&p);
    return cv_throw(c, e, "The image argument is a canvas element with a width or height of 0.");
}

embi_dom_err embi_impl_CanvasRenderingContext2D_drawImage_0(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value a0,
                                                            double a1, double a2)
{
    picture p;
    int r = pic_from(c, a0, &p, "Failed to execute 'drawImage' on 'CanvasRenderingContext2D'");
    if (r < 0) {
        return EMBI_BIND_THREW;
    }
    if (r == 0) {
        return EMBI_DOM_OK;
    }
    double s[4] = {0, 0, (double)p.w, (double)p.h};
    double d[4] = {a1, a2, (double)p.w, (double)p.h};
    int e = embi_canvas_draw_image(cv_of(self), p.im, p.src, s, d);
    pic_free(&p);
    return cv_throw(c, e, "The image source is not drawable.");
}

embi_dom_err embi_impl_CanvasRenderingContext2D_drawImage_1(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value a0,
                                                            double a1, double a2, double a3, double a4)
{
    const double d[4] = {a1, a2, a3, a4};
    return draw_image(c, self, a0, nullptr, d, false);
}

embi_dom_err embi_impl_CanvasRenderingContext2D_drawImage_2(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value a0,
                                                            double a1, double a2, double a3, double a4, double a5,
                                                            double a6, double a7, double a8)
{
    const double s[4] = {a1, a2, a3, a4};
    const double d[4] = {a5, a6, a7, a8};
    return draw_image(c, self, a0, s, d, true);
}

embi_dom_err embi_impl_CanvasRenderingContext2D_createPattern(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value a0,
                                                              embi_js_value a1, embi_js_value *out)
{
    int repeat = EMBI_CP_REPEAT;
    /* DOMString? repetition: null is the empty string (repeat), undefined is the string "undefined" (a SyntaxError). */
    if (!embi_js_is_null(a1)) {
        embi_dstr ds;
        if (!embi_bind_conv_str(c, a1, "Failed to execute 'createPattern' on 'CanvasRenderingContext2D'", false, &ds)) {
            *out = EMBI_JS_NULL;
            return EMBI_BIND_THREW;
        }
        embi_str s = embi_bind_utf8(c, ds);
        if (s.len == 0 || (s.len == 6 && memcmp(s.ptr, "repeat", 6) == 0)) {
            repeat = EMBI_CP_REPEAT;
        } else if (s.len == 8 && memcmp(s.ptr, "repeat-x", 8) == 0) {
            repeat = EMBI_CP_REPEAT_X;
        } else if (s.len == 8 && memcmp(s.ptr, "repeat-y", 8) == 0) {
            repeat = EMBI_CP_REPEAT_Y;
        } else if (s.len == 9 && memcmp(s.ptr, "no-repeat", 9) == 0) {
            repeat = EMBI_CP_NO_REPEAT;
        } else {
            *out = EMBI_JS_NULL;
            return cv_throw(c, EMBI_CV_SYNTAX, "The provided type is not a valid repetition.");
        }
    }
    picture p;
    int r = pic_from(c, a0, &p, "Failed to execute 'createPattern' on 'CanvasRenderingContext2D'");
    if (r < 0) {
        return EMBI_BIND_THREW;
    }
    if (r == 0) {
        *out = EMBI_JS_NULL;
        return EMBI_DOM_OK;
    }
    embi_cpattern *pt = embi_canvas_pattern_from(cv_of(self), p.im, p.src, repeat);
    pic_free(&p);
    if (pt == nullptr) {
        *out = EMBI_JS_NULL;
        return EMBI_DOM_OK;
    }
    *out = ref_wrap(c, EMBI_IF_CanvasPattern, pt, pat_release, self);
    return EMBI_DOM_OK;
}

/* ------------------------------------------------------------------ TextMetrics */

embi_js_value cv_metrics_value(embi_bind_ctx *c, const embi_text_metrics *m)
{
    embi_text_metrics *copy = embi_alloc(sizeof *copy);
    *copy = *m;
    return embi_bind_other_value(c, EMBI_IF_TextMetrics, copy, copy, sizeof *copy);
}

#define TM_FIELD(NAME, FIELD)                                                                                     \
    double embi_impl_TextMetrics_get_##NAME(embi_bind_ctx *c, embi_bind_obj *self)                               \
    {                                                                                                            \
        (void)c;                                                                                                 \
        return ((const embi_text_metrics *)self->ptr)->FIELD;                                                    \
    }

TM_FIELD(width, width)
TM_FIELD(actualBoundingBoxLeft, left)
TM_FIELD(actualBoundingBoxRight, right)
TM_FIELD(fontBoundingBoxAscent, font_ascent)
TM_FIELD(fontBoundingBoxDescent, font_descent)
TM_FIELD(actualBoundingBoxAscent, ascent)
TM_FIELD(actualBoundingBoxDescent, descent)
TM_FIELD(emHeightAscent, em_ascent)
TM_FIELD(emHeightDescent, em_descent)
TM_FIELD(hangingBaseline, hanging)
TM_FIELD(alphabeticBaseline, alphabetic)
TM_FIELD(ideographicBaseline, ideographic)

/* ------------------------------------------------------------------ ImageData */

typedef struct idata {
    uint32_t w, h;
    embi_bind_obj *bo; /* the wrapper, whose ref holds the Uint8ClampedArray */
} idata;

static embi_js_value new_clamped(embi_bind_ctx *c, size_t n)
{
    embi_js_runtime *rt = c->rt;
    embi_js_value ctor =
        embi_js_binary_intrinsic(rt, (enum embi_js_bin_intr)(EMBI_JS_BI_TA_CTOR_BASE + EMBI_JS_TA_UINT8C));
    embi_js_value len = embi_js_from_double((double)n);
    return embi_js_construct(rt, ctor, 1, &len, ctor);
}

/* The bytes of a Uint8ClampedArray value; false for any other value. */
static bool clamped_bytes(embi_js_value v, uint8_t **p, size_t *n)
{
    *p = nullptr;
    *n = 0;
    if (!embi_js_is_object(v) || !embi_js_is_typedarray_obj(embi_js_as_object(v))) {
        return false;
    }
    embi_js_varray *vw = embi_js_view_of(embi_js_as_object(v));
    if (embi_js_bin_num(vw->elems[EMBI_JS_VIEW_KIND]) != EMBI_JS_TA_UINT8C) {
        return false;
    }
    size_t count = 0;
    if (embi_js_view_len(vw, 1, &count) && count > 0) {
        *n = count;
        *p = embi_js_view_base(vw);
    }
    return true;
}

/* A new ImageData wrapper of w x h over data (an existing Uint8ClampedArray, or nullptr to make a zeroed one). */
static embi_js_value imagedata_new(embi_bind_ctx *c, uint32_t w, uint32_t h, embi_js_value data)
{
    if ((uint64_t)w * h > EMBI_CANVAS_MAX_AREA) {
        (void)embi_impl_throw(c, "IndexSizeError", "The requested image size exceeds the supported range.");
        return EMBI_JS_EXCEPTION;
    }
    if (embi_js_is_empty(data)) {
        data = new_clamped(c, (size_t)w * h * 4);
        if (data == EMBI_JS_EXCEPTION) {
            return data;
        }
    }
    idata *d = embi_alloc(sizeof *d);
    d->w = w;
    d->h = h;
    embi_js_value v = embi_bind_other_value(c, EMBI_IF_ImageData, d, d, sizeof *d);
    d->bo = embi_bind_obj_of(v);
    embi_bind_set_ref(d->bo, data);
    return v;
}

embi_js_value embi_impl_ImageData_constructor(embi_bind_ctx *c, embi_js_value new_target, embi_js_value a0,
                                              embi_js_value a1, embi_js_value a2, embi_js_value a3)
{
    (void)a3;
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value result;
    uint8_t *bytes;
    size_t n;
    const char *ctx = "Failed to construct 'ImageData'";
    if (embi_js_is_object(a0) && embi_js_is_typedarray_obj(embi_js_as_object(a0))) {
        if (!clamped_bytes(a0, &bytes, &n)) {
            /* Not a Uint8ClampedArray: the (sw, sh, settings) overload, where the array converts to NaN (a zero width) and a
             * third argument that is not a dictionary is a TypeError. */
            if (!embi_js_is_undefined(a2)) {
                (void)embi_js_throw_type_error(c->rt, "%s: The input data is not a Uint8ClampedArray.", ctx);
            } else {
                (void)embi_impl_throw(c, "IndexSizeError", "The source width or height is zero.");
            }
            return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
        }
        size_t count = 0;
        embi_js_varray *vw = embi_js_view_of(embi_js_as_object(a0));
        (void)embi_js_view_len(vw, 1, &count);
        int64_t sw = 0, sh = 0;
        if (!embi_bind_conv_int(c, a1, 6, ctx, &sw)) {
            return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
        }
        bool has_sh = !embi_js_is_undefined(a2);
        if (has_sh && !embi_bind_conv_int(c, a2, 6, ctx, &sh)) {
            return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
        }
        if (count == 0 || count % 4 != 0) {
            (void)embi_impl_throw(c, "InvalidStateError", "The input data length is not a nonzero multiple of 4.");
            return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
        }
        if (sw == 0) {
            (void)embi_impl_throw(c, "IndexSizeError", "The source width is zero or not a number.");
            return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
        }
        size_t pixels = count / 4;
        if (pixels % (size_t)sw != 0) {
            (void)embi_impl_throw(c, "IndexSizeError", "The input data length is not a multiple of (4 * width).");
            return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
        }
        size_t h = pixels / (size_t)sw;
        if (has_sh && (uint64_t)sh != h) {
            (void)embi_impl_throw(c, "IndexSizeError", "The input data byte length is not equal to (4 * width * height).");
            return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
        }
        result = imagedata_new(c, (uint32_t)sw, (uint32_t)h, a0);
    } else {
        int64_t sw = 0, sh = 0;
        if (!embi_bind_conv_int(c, a0, 6, ctx, &sw) || !embi_bind_conv_int(c, a1, 6, ctx, &sh)) {
            return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
        }
        if (sw == 0 || sh == 0) {
            (void)embi_impl_throw(c, "IndexSizeError", "The source width or height is zero.");
            return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
        }
        result = imagedata_new(c, (uint32_t)sw, (uint32_t)sh, EMBI_JS_EMPTY);
    }
    if (result != EMBI_JS_EXCEPTION) {
        embi_bind_adopt_new_target(c, result, new_target);
    }
    return embi_bind_leave(sc, result);
}

uint32_t embi_impl_ImageData_get_width(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    return ((const idata *)self->ptr)->w;
}

uint32_t embi_impl_ImageData_get_height(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    return ((const idata *)self->ptr)->h;
}

embi_js_value embi_impl_ImageData_get_data(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    return self->ref;
}

embi_dstr embi_impl_ImageData_get_colorSpace(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    (void)self;
    return embi_impl_lit("srgb");
}

embi_dstr embi_impl_ImageData_get_pixelFormat(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    (void)self;
    return embi_impl_lit("rgba-unorm8");
}

static uint32_t abs_u32(int32_t v)
{
    return v < 0 ? (uint32_t)(-(int64_t)v) : (uint32_t)v;
}

embi_dom_err embi_impl_CanvasRenderingContext2D_createImageData_0(embi_bind_ctx *c, embi_bind_obj *self, int32_t a0,
                                                                  int32_t a1, embi_dict_ImageDataSettings a2,
                                                                  embi_js_value *out)
{
    (void)self;
    (void)a2;
    if (a0 == 0 || a1 == 0) {
        return cv_throw(c, EMBI_CV_INDEX_SIZE, "The source width or height is zero.");
    }
    *out = imagedata_new(c, abs_u32(a0), abs_u32(a1), EMBI_JS_EMPTY);
    return *out == EMBI_JS_EXCEPTION ? EMBI_BIND_THREW : EMBI_DOM_OK;
}

embi_dom_err embi_impl_CanvasRenderingContext2D_createImageData_1(embi_bind_ctx *c, embi_bind_obj *self, void *a0,
                                                                  embi_js_value *out)
{
    (void)self;
    const idata *d = a0;
    *out = imagedata_new(c, d->w, d->h, EMBI_JS_EMPTY);
    return *out == EMBI_JS_EXCEPTION ? EMBI_BIND_THREW : EMBI_DOM_OK;
}

embi_dom_err embi_impl_CanvasRenderingContext2D_getImageData(embi_bind_ctx *c, embi_bind_obj *self, int32_t a0,
                                                             int32_t a1, int32_t a2, int32_t a3,
                                                             embi_dict_ImageDataSettings a4, embi_js_value *out)
{
    (void)a4;
    if (a2 == 0 || a3 == 0) {
        return cv_throw(c, EMBI_CV_INDEX_SIZE, "The source width or height is zero.");
    }
    int64_t x = a0, y = a1;
    if (a2 < 0) {
        x += a2;
    }
    if (a3 < 0) {
        y += a3;
    }
    x = x < INT32_MIN ? INT32_MIN : (x > INT32_MAX ? INT32_MAX : x);
    y = y < INT32_MIN ? INT32_MIN : (y > INT32_MAX ? INT32_MAX : y);
    uint32_t w = abs_u32(a2), h = abs_u32(a3);
    embi_js_value v = imagedata_new(c, w, h, EMBI_JS_EMPTY);
    if (v == EMBI_JS_EXCEPTION) {
        return EMBI_BIND_THREW;
    }
    uint8_t *bytes;
    size_t n;
    if (clamped_bytes(embi_bind_obj_of(v)->ref, &bytes, &n) && n == (size_t)w * h * 4) {
        (void)embi_canvas_read(cv_of(self), (int32_t)x, (int32_t)y, w, h, bytes);
    }
    *out = v;
    return EMBI_DOM_OK;
}

static embi_dom_err put_image(embi_bind_ctx *c, embi_bind_obj *self, void *img, int32_t dx, int32_t dy, int64_t sx,
                              int64_t sy, int64_t sw, int64_t sh)
{
    const idata *d = img;
    uint8_t *bytes;
    size_t n;
    if (!clamped_bytes(d->bo->ref, &bytes, &n) || n != (size_t)d->w * d->h * 4) {
        return cv_throw(c, EMBI_CV_INVALID_STATE, "The image data is detached.");
    }
    if (sw < 0) {
        sx += sw;
        sw = -sw;
    }
    if (sh < 0) {
        sy += sh;
        sh = -sh;
    }
    if (sx < 0) {
        sw += sx;
        sx = 0;
    }
    if (sy < 0) {
        sh += sy;
        sy = 0;
    }
    if (sx + sw > (int64_t)d->w) {
        sw = (int64_t)d->w - sx;
    }
    if (sy + sh > (int64_t)d->h) {
        sh = (int64_t)d->h - sy;
    }
    if (sw <= 0 || sh <= 0) {
        return EMBI_DOM_OK;
    }
    uint8_t *sub = embi_alloc((size_t)sw * (size_t)sh * 4);
    for (int64_t y = 0; y < sh; y++) {
        memcpy(sub + (size_t)y * (size_t)sw * 4, bytes + (((size_t)(sy + y)) * d->w + (size_t)sx) * 4, (size_t)sw * 4);
    }
    embi_canvas_put(cv_of(self), sub, (uint32_t)sw, (uint32_t)sh, (int32_t)(dx + sx), (int32_t)(dy + sy));
    embi_free(sub, (size_t)sw * (size_t)sh * 4);
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_CanvasRenderingContext2D_putImageData_0(embi_bind_ctx *c, embi_bind_obj *self, void *a0,
                                                               int32_t a1, int32_t a2)
{
    const idata *d = a0;
    return put_image(c, self, a0, a1, a2, 0, 0, d->w, d->h);
}

embi_dom_err embi_impl_CanvasRenderingContext2D_putImageData_1(embi_bind_ctx *c, embi_bind_obj *self, void *a0,
                                                               int32_t a1, int32_t a2, int32_t a3, int32_t a4,
                                                               int32_t a5, int32_t a6)
{
    return put_image(c, self, a0, a1, a2, a3, a4, a5, a6);
}

/* ------------------------------------------------------------------ ImageBitmap */

uint32_t embi_impl_ImageBitmap_get_width(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    rbox *b = self->owned;
    return b != nullptr && b->p != nullptr ? ((embi_cimage *)b->p)->w : 0;
}

uint32_t embi_impl_ImageBitmap_get_height(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    rbox *b = self->owned;
    return b != nullptr && b->p != nullptr ? ((embi_cimage *)b->p)->h : 0;
}

void embi_impl_ImageBitmap_close(embi_bind_ctx *c, embi_bind_obj *self)
{
    (void)c;
    rbox *b = self->owned;
    if (b != nullptr && b->p != nullptr) {
        embi_cimage_release(b->p);
        b->p = nullptr;
    }
}

static embi_js_value bitmap_wrap(embi_bind_ctx *c, embi_cimage *im)
{
    return ref_wrap(c, EMBI_IF_ImageBitmap, im, bmp_release, nullptr);
}

embi_js_value embi_impl_Window_createImageBitmap(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value a0, embi_js_value a1,
                                                 embi_js_value a2, embi_js_value a3, embi_js_value a4, embi_js_value a5)
{
    (void)self;
    (void)a5;
    embi_bind_scope sc = embi_bind_enter(c);
    embi_bind_obj *bo = embi_js_is_object(a0) ? embi_bind_obj_of(a0) : nullptr;
    embi_cimage *src = nullptr;
    const uint8_t *blob_p = nullptr;
    size_t blob_n = 0;
    embi_str blob_type = {0};
    if (bo != nullptr && bo->iface == EMBI_IF_ImageData) {
        const idata *d = bo->ptr;
        uint8_t *bytes;
        size_t n;
        src = embi_cimage_new(d->w, d->h);
        if (src->w == d->w && src->h == d->h && clamped_bytes(bo->ref, &bytes, &n) && n == (size_t)d->w * d->h * 4) {
            memcpy(src->px, bytes, n);
        }
    } else if (embi_js_is_object(a0) && ldr_blob_view(a0, &blob_p, &blob_n, &blob_type)) {
        embi_pixels px;
        if (!embi_image_decode(blob_p, blob_n, EMBI_IMAGE_DEFAULT_MAX_PIXELS, true, nullptr, nullptr, &px)) {
            (void)cv_throw(c, EMBI_CV_INVALID_STATE, "The source image could not be decoded.");
            return embi_bind_leave(sc, embi_bind_promise_from_pending(c));
        }
        src = embi_cimage_new(px.w, px.h);
        if (src->w == px.w && src->h == px.h) {
            memcpy(src->px, px.px, (size_t)px.w * px.h * 4);
        }
        embi_pixels_free(&px);
    } else {
        picture p;
        int r = pic_from(c, a0, &p, "Failed to execute 'createImageBitmap' on 'Window'");
        if (r < 0) {
            return embi_bind_leave(sc, embi_bind_promise_from_pending(c));
        }
        if (r == 0) {
            (void)cv_throw(c, EMBI_CV_INVALID_STATE, "The image source is not usable.");
            return embi_bind_leave(sc, embi_bind_promise_from_pending(c));
        }
        if (p.src != nullptr) {
            src = embi_canvas_snapshot(p.src, 0, 0, p.w, p.h);
        } else {
            src = embi_cimage_retain(p.im); /* pictures are immutable: the bitmap shares it */
        }
        pic_free(&p);
    }
    /* The optional source rectangle: (sx, sy, sw, sh) as numbers. */
    embi_cimage *out = src;
    if (!embi_js_is_undefined(a1) && !embi_js_is_object(a1) && !embi_js_is_undefined(a2) && !embi_js_is_undefined(a3) &&
        !embi_js_is_undefined(a4)) {
        double r[4];
        embi_js_value in[4] = {a1, a2, a3, a4};
        for (int i = 0; i < 4; i++) {
            if (!embi_bind_conv_double(c, in[i], false, "Failed to execute 'createImageBitmap' on 'Window'", &r[i])) {
                embi_cimage_release(src);
                return embi_bind_leave(sc, embi_bind_promise_from_pending(c));
            }
        }
        if (r[2] == 0.0 || r[3] == 0.0) {
            embi_cimage_release(src);
            (void)embi_js_throw_range_error(c->rt, "The crop rect width or height is 0.");
            return embi_bind_leave(sc, embi_bind_promise_from_pending(c));
        }
        /* The rectangle is script controlled: reject what cannot be a bitmap before any integer conversion. */
        if (!isfinite(r[0]) || !isfinite(r[1]) || !(fabs(r[2]) <= (double)EMBI_CANVAS_MAX_SIDE) ||
            !(fabs(r[3]) <= (double)EMBI_CANVAS_MAX_SIDE) || fabs(r[0]) > 2147483647.0 || fabs(r[1]) > 2147483647.0) {
            embi_cimage_release(src);
            (void)embi_js_throw_range_error(c->rt, "The crop rect is not a supported size.");
            return embi_bind_leave(sc, embi_bind_promise_from_pending(c));
        }
        int32_t sx = (int32_t)floor(r[0]), sy = (int32_t)floor(r[1]);
        uint32_t sw = (uint32_t)fabs(r[2]), sh = (uint32_t)fabs(r[3]);
        if (sw == 0 || sh == 0 || !embi_canvas_size_ok(sw, sh)) {
            embi_cimage_release(src);
            (void)embi_js_throw_range_error(c->rt, "The crop rect is not a supported size.");
            return embi_bind_leave(sc, embi_bind_promise_from_pending(c));
        }
        if (r[2] < 0.0) {
            sx -= (int32_t)sw;
        }
        if (r[3] < 0.0) {
            sy -= (int32_t)sh;
        }
        out = embi_cimage_new(sw, sh);
        for (uint32_t y = 0; y < sh; y++) {
            int64_t yy = (int64_t)sy + y;
            if (yy < 0 || yy >= src->h) {
                continue;
            }
            for (uint32_t x = 0; x < sw; x++) {
                int64_t xx = (int64_t)sx + x;
                if (xx >= 0 && xx < src->w) {
                    memcpy(out->px + ((size_t)y * sw + x) * 4, src->px + ((size_t)yy * src->w + (size_t)xx) * 4, 4);
                }
            }
        }
        embi_cimage_release(src);
    }
    embi_js_value bmp = bitmap_wrap(c, out);
    return embi_bind_leave(sc, embi_bind_promise_resolved(c, bmp));
}

