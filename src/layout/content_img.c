/*
 * Images of the content property (CSS Content 3 section 2): the <image> items of generated content and element
 * replacement. W4g4c.
 *
 * An element whose content is a single <image> is replaced by it (element-replacement-*: an img's src is ignored
 * then). An image-set() resolves to one of its options here, as its computed value would (CSS Images 4 section 2.5):
 * the option whose resolution is the device's (1x), else the lowest resolution above it, else the highest one below
 * it. Options of a type() other than an image type the engine decodes are skipped.
 *
 * An img's source comes from its srcset and src attributes the same way (HTML section 4.8.4.3): the pixel density
 * candidates (src is the 1x one unless srcset has one) at a device pixel ratio of 1. Width descriptors need sizes,
 * which is not supported: those candidates are skipped.
 */
#include "layout_int.h"

#include "css/props.h"

#include <string.h>

static bool type_ok(embi_str t)
{
    if (t.len == 0) {
        return true;
    }
    static const char *const ok[] = {"image/png", "image/jpeg", "image/gif", "image/svg+xml"};
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++) {
        size_t n = strlen(ok[i]);
        if (t.len == n && memcmp(t.ptr, ok[i], n) == 0) {
            return true;
        }
    }
    return false;
}

const embi_image *embi_lay_image_resolve(const embi_image *im, float *dppx)
{
    double scale = 1.0;
    for (int depth = 0; im != nullptr && im->kind == EMBI_IMG_IMAGE_SET && depth < 4; depth++) {
        const embi_image *best = nullptr;
        double bd = 0.0;
        for (uint32_t i = 0; i < im->n; i++) {
            const embi_image_opt *o = &im->opts[i];
            if (o->img == nullptr || !type_ok(o->type)) {
                continue;
            }
            double d = o->dppx;
            bool better = best == nullptr || (d >= 1.0 && (bd < 1.0 || d < bd)) || (d < 1.0 && bd < 1.0 && d > bd);
            if (better) {
                best = o->img;
                bd = d;
            }
        }
        im = best;
        scale *= bd > 0.0 ? bd : 1.0;
    }
    if (dppx != nullptr) {
        *dppx = (float)scale;
    }
    return im;
}

/* The image of one content item, nullptr when the item is not an image. */
static const embi_image *item_image(const embi_value *v, embi_image *tmp)
{
    if (v->kind == EMBI_V_URL) {
        memset(tmp, 0, sizeof *tmp);
        tmp->kind = EMBI_IMG_URL;
        tmp->url = v->str;
        return tmp;
    }
    if (v->kind == EMBI_V_IMAGE && v->image != nullptr) {
        return embi_lay_image_resolve(v->image, nullptr);
    }
    return nullptr;
}

bool embi_lay_content_image(const embi_value *content, uint32_t index, embi_image *out)
{
    if (content == nullptr || content->kind == EMBI_V_KEYWORD) {
        return false;
    }
    uint32_t n = 0;
    const embi_value *items = embi_value_items(content, &n);
    n = content->kind == EMBI_V_LIST && content->aux < n ? n - content->aux : n; /* the alt text items follow a '/' */
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        embi_image tmp;
        const embi_image *im = item_image(&items[i], &tmp);
        bool is_img = items[i].kind == EMBI_V_URL || items[i].kind == EMBI_V_IMAGE;
        if (!is_img) {
            continue;
        }
        if (k++ != index) {
            continue;
        }
        if (im == nullptr || (im->kind != EMBI_IMG_URL && im->kind != EMBI_IMG_GRADIENT)) {
            return false;
        }
        *out = *im;
        return true;
    }
    return false;
}

bool embi_lay_content_replaces(const embi_value *content, embi_image *out)
{
    if (content == nullptr || content->kind == EMBI_V_KEYWORD) {
        return false;
    }
    uint32_t n = 0;
    (void)embi_value_items(content, &n);
    n = content->kind == EMBI_V_LIST && content->aux < n ? n - content->aux : n;
    if (n != 1) {
        return false;
    }
    return embi_lay_content_image(content, 0, out);
}

/* ------------------------------------------------------------------ img srcset */

static bool srcset_ws(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f';
}

/* A pixel density descriptor ("1.5x"): its value, or 0 when d is another descriptor or invalid. */
static double density_of(const char *d, uint32_t n)
{
    if (n < 2 || d[n - 1] != 'x') {
        return 0.0;
    }
    double v = 0.0, scale = 0.0;
    uint32_t digits = 0;
    for (uint32_t i = 0; i + 1 < n; i++) {
        char ch = d[i];
        if (ch >= '0' && ch <= '9') {
            if (scale > 0.0) {
                v += (ch - '0') * scale;
                scale /= 10.0;
            } else {
                v = v * 10.0 + (ch - '0');
            }
            digits++;
        } else if (ch == '.' && scale == 0.0) {
            scale = 0.1;
        } else {
            return 0.0;
        }
    }
    return digits > 0 ? v : 0.0;
}

embi_str embi_lay_img_source(embi_str src, embi_str srcset, float *density)
{
    embi_str best = {nullptr, 0};
    double bd = 0.0;
    bool has_1x = false, has_w = false;
    uint32_t i = 0, n = srcset.len;
    const char *t = srcset.ptr;
    while (t != nullptr && i < n) {
        while (i < n && (srcset_ws(t[i]) || t[i] == ',')) {
            i++;
        }
        if (i >= n) {
            break;
        }
        uint32_t u0 = i;
        while (i < n && !srcset_ws(t[i])) {
            i++;
        }
        uint32_t u1 = i;
        double d = 1.0;
        bool ok = true;
        if (t[u1 - 1] == ',') {
            while (u1 > u0 && t[u1 - 1] == ',') {
                u1--;
            }
        } else {
            /* The descriptors, up to the next comma. */
            uint32_t nd = 0;
            for (;;) {
                while (i < n && srcset_ws(t[i])) {
                    i++;
                }
                if (i >= n || t[i] == ',') {
                    break;
                }
                uint32_t d0 = i;
                while (i < n && !srcset_ws(t[i]) && t[i] != ',') {
                    i++;
                }
                if (t[i - 1] == 'w') {
                    has_w = true;
                    ok = false;
                } else {
                    d = density_of(t + d0, i - d0);
                    ok = ok && d > 0.0 && nd == 0;
                }
                nd++;
            }
        }
        if (!ok || u1 == u0) {
            continue;
        }
        has_1x = has_1x || d == 1.0;
        bool better = best.ptr == nullptr || (d >= 1.0 && (bd < 1.0 || d < bd)) || (d < 1.0 && bd < 1.0 && d > bd);
        if (better) {
            best = (embi_str){t + u0, u1 - u0};
            bd = d;
        }
    }
    /* The src attribute is the 1x candidate, unless srcset has one (HTML section 4.8.4.3.10). */
    if ((src.len > 0 && !has_1x && !has_w) || best.ptr == nullptr) {
        best = src;
        bd = 1.0;
    }
    if (density != nullptr) {
        *density = (float)bd;
    }
    return best;
}
