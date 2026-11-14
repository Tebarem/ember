/*
 * The value types of the SVG DOM (R4d): SVGNumber, SVGLength, SVGAngle, SVGPoint, SVGRect, SVGMatrix, SVGTransform,
 * SVGPreserveAspectRatio, and the geometry interfaces DOMPoint(ReadOnly) and DOMMatrix(ReadOnly).
 *
 * A value object holds an `sv` payload. A free value keeps its data in the payload. A value of an animated attribute
 * (SVGAnimatedLength.baseVal, ...) is bound to the attribute: every read parses it again and every write serializes it
 * (SB_ATTR). An item of a list is bound to the list (SB_ITEM), the matrix of a transform to the transform (SB_MATRIX).
 */
#include "bindings/svg_int.h"
#include "bindings/gen/window.h"
#include "css/css.h"
#include "css/props.h"
#include "num/num.h"
#include "style/style.h"

#include <float.h>
#include <math.h>
#include <stdint.h>

/* ------------------------------------------------------------------ small helpers */

#define P1 , double a0
#define P2 , double a0, double a1
#define P3 , double a0, double a1, double a2
#define P6 , double a0, double a1, double a2, double a3, double a4, double a5

static embi_str cstr(const char *s)
{
    return (embi_str){s, (uint32_t)SDL_strlen(s)};
}

static bool finite_d(double v)
{
    return v - v == 0.0;
}

static bool svg_ws(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f';
}

static void skip_ws(embi_str s, uint32_t *p)
{
    while (*p < s.len && svg_ws(s.ptr[*p])) {
        (*p)++;
    }
}

static embi_dom_err throw_type(embi_bind_ctx *c, const char *msg)
{
    (void)embi_js_throw_type_error(embi_bind_runtime(c), "%s", msg);
    return EMBI_BIND_THREW;
}

static void sb_num(embi_sb *sb, double v)
{
    char buf[64];
    if (v - v != 0.0) {
        v = 0; /* NaN and Infinity are not numbers of the attribute grammars */
    }
    size_t k = embi_num_to_str(v, buf);
    embi_sb_append(sb, (embi_str){buf, (uint32_t)k});
}

bool embi_sv_float_range_error(embi_bind_ctx *c, double v)
{
    if (v > FLT_MAX || v < -FLT_MAX) {
        (void)embi_js_throw_type_error(embi_bind_runtime(c), "The value is outside the range of a float.");
        return true;
    }
    return false;
}

/* A number at *pos: an optional sign, then digits (no leading whitespace). */
static bool scan_num(embi_str s, uint32_t *pos, double *out)
{
    uint32_t p = *pos;
    bool neg = false;
    if (p < s.len && (s.ptr[p] == '+' || s.ptr[p] == '-')) {
        neg = s.ptr[p] == '-';
        p++;
    }
    if (p >= s.len || !((s.ptr[p] >= '0' && s.ptr[p] <= '9') || s.ptr[p] == '.')) {
        return false;
    }
    size_t used = 0;
    double v = embi_num_parse_decimal(s.ptr + p, s.len - p, &used);
    if (used == 0 || !finite_d(v) || s.ptr[p + used - 1] == '.') {
        return false; /* "5." is not a number of the SVG grammar */
    }
    *out = neg ? -v : v;
    *pos = p + (uint32_t)used;
    return true;
}

static bool ieq_prefix(embi_str s, uint32_t p, const char *word)
{
    size_t n = SDL_strlen(word);
    if (s.len - p < n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char a = s.ptr[p + i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (a != word[i]) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ attributes */

static const char *const sa_names[SA_COUNT] = {
    "class", "href", "target", "x", "y", "width", "height", "rx", "ry", "cx", "cy", "r", "fx", "fy", "fr", "x1", "y1", "x2",
    "y2", "offset", "pathLength", "viewBox", "preserveAspectRatio", "refX", "refY", "markerWidth", "markerHeight",
    "markerUnits", "orient", "orient", "dx", "dy", "rotate", "textLength", "lengthAdjust", "startOffset", "method",
    "spacing", "side", "points", "points", "transform", "gradientTransform", "patternTransform", "requiredExtensions",
    "systemLanguage", "clipPathUnits", "gradientUnits", "spreadMethod", "maskUnits", "maskContentUnits", "patternUnits",
    "patternContentUnits",
};

const char *embi_sv_attr_name(uint32_t idx)
{
    return idx < SA_COUNT ? sa_names[idx] : "";
}

bool embi_sv_attr_text(embi_bind_ctx *c, embi_node *n, uint32_t idx, embi_str *out)
{
    const embi_attr *a = embi_el_get_attribute((embi_element *)n, cstr(embi_sv_attr_name(idx)));
    if (a == nullptr) {
        return false;
    }
    *out = embi_bind_utf8(c, a->value);
    return true;
}

void embi_sv_set_attr(embi_bind_ctx *c, embi_node *n, uint32_t idx, embi_str text)
{
    (void)embi_el_set_attribute((embi_element *)n, cstr(embi_sv_attr_name(idx)), embi_bind_tmp_utf8(c, text));
}

/* ------------------------------------------------------------------ payloads and bindings */

/* The payload of a value is allocated for its kind only (a DOMRect needs 48 bytes, not the 152 of an sv with its 16 double
 * matrix): never copy or read a whole sv from a heap payload, use sv_size of its kind. */
static size_t sv_payload(uint8_t kind)
{
    switch (kind) {
    case SV_PT:
    case SV_RECT:
        return sizeof(double[4]);
    case SV_MX:
        return sizeof(((sv *)nullptr)->u.mx);
    case SV_TF:
        return sizeof(((sv *)nullptr)->u.tf);
    case SV_PAR:
        return sizeof(((sv *)nullptr)->u.par);
    case SV_LIST:
        return sizeof(((sv *)nullptr)->u.list);
    default:
        return sizeof(double); /* SV_NUM, SV_LEN, SV_ANGLE, SV_STR */
    }
}

static size_t sv_size(uint8_t kind)
{
    return offsetof(sv, u) + sv_payload(kind);
}

sv *embi_sv_new(embi_bind_ctx *c, uint16_t iface, uint8_t kind, embi_js_value *out)
{
    size_t size = sv_size(kind);
    sv *p = embi_alloc(size);
    SDL_memset(p, 0, size);
    p->kind = kind;
    switch (kind) {
    case SV_LEN:
        p->unit = SVG_LT_NUMBER;
        break;
    case SV_ANGLE:
        p->unit = SVG_AT_UNSPECIFIED;
        break;
    case SV_PT:
        p->u.r[3] = 1;
        break;
    case SV_MX:
        p->u.mx.m[0] = p->u.mx.m[5] = p->u.mx.m[10] = p->u.mx.m[15] = 1;
        p->u.mx.is2d = true;
        break;
    case SV_TF:
        p->u.tf.m[0] = p->u.tf.m[3] = 1;
        break;
    case SV_PAR:
        p->u.par.align = 6; /* xMidYMid */
        p->u.par.mos = 1;   /* meet */
        break;
    default:
        break;
    }
    embi_js_value v = embi_bind_other_value(c, iface, p, p, size);
    p->self = embi_bind_obj_of(v);
    *out = v;
    return p;
}

embi_js_value embi_svg_new_value(embi_bind_ctx *c, uint16_t iface, uint8_t kind)
{
    embi_js_value v;
    (void)embi_sv_new(c, iface, kind, &v);
    return v;
}

embi_bind_obj *embi_sv_parent(const sv *p)
{
    if (embi_js_is_empty(p->self->ref)) {
        return nullptr;
    }
    embi_bind_obj *r = embi_bind_obj_of(p->self->ref);
    return (r && r->ptr) ? r : nullptr;
}

/* The element a bound value reads its context (font size, viewport) from. */
static embi_node *ctx_node(const sv *p)
{
    embi_bind_obj *par = embi_sv_parent(p);
    if (par == nullptr) {
        return nullptr;
    }
    if (p->bound == SB_ATTR) {
        return par->is_node ? par->ptr : nullptr;
    }
    if (p->bound == SB_ITEM) {
        return embi_bind_owner_node(par);
    }
    return nullptr;
}

void embi_sv_copy_data(sv *dst, const sv *src)
{
    if (dst->kind == SV_LIST || src->kind == SV_LIST) {
        return;
    }
    dst->unit = src->unit;
    size_t n = sv_payload(dst->kind) < sv_payload(src->kind) ? sv_payload(dst->kind) : sv_payload(src->kind);
    SDL_memcpy(&dst->u, &src->u, n);
}

uint16_t embi_sv_iface_of(uint8_t kind)
{
    switch (kind) {
    case SV_NUM:
        return EMBI_IF_SVGNumber;
    case SV_LEN:
        return EMBI_IF_SVGLength;
    case SV_PT:
        return EMBI_IF_SVGPoint;
    case SV_TF:
        return EMBI_IF_SVGTransform;
    case SV_MX:
        return EMBI_IF_SVGMatrix;
    case SV_RECT:
        return EMBI_IF_SVGRect;
    default:
        return EMBI_IF_NONE;
    }
}

/* ------------------------------------------------------------------ matrices (column-major 4x4: m[col * 4 + row]) */

static void mx_ident(double *m)
{
    SDL_memset(m, 0, 16 * sizeof(double));
    m[0] = m[5] = m[10] = m[15] = 1;
}

static void mx_from6(double *m, const double a[6])
{
    mx_ident(m);
    m[0] = a[0];
    m[1] = a[1];
    m[4] = a[2];
    m[5] = a[3];
    m[12] = a[4];
    m[13] = a[5];
}

static void mx_to6(const double *m, double a[6])
{
    a[0] = m[0];
    a[1] = m[1];
    a[2] = m[4];
    a[3] = m[5];
    a[4] = m[12];
    a[5] = m[13];
}

/* out = a * b (b is applied first). */
static void mx_mul(double *out, const double *a, const double *b)
{
    double r[16];
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            double s = 0;
            for (int k = 0; k < 4; k++) {
                s += a[k * 4 + row] * b[col * 4 + k];
            }
            r[col * 4 + row] = s;
        }
    }
    SDL_memcpy(out, r, sizeof r);
}

static bool mx_invert(const double *m, double *out)
{
    double inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
             m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
             m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
             m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
              m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
             m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
             m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
             m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
              m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
             m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
             m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
              m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
              m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
             m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
             m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
              m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
              m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (det == 0 || !finite_d(det)) {
        return false;
    }
    det = 1.0 / det;
    for (int i = 0; i < 16; i++) {
        out[i] = inv[i] * det;
    }
    return true;
}

static double deg2rad(double d)
{
    return d * (3.14159265358979323846 / 180.0);
}

static void mx_translate(double *out, const double *m, double tx, double ty, double tz)
{
    double t[16];
    mx_ident(t);
    t[12] = tx;
    t[13] = ty;
    t[14] = tz;
    mx_mul(out, m, t);
}

static void mx_scale3(double *out, const double *m, double sx, double sy, double sz)
{
    double s[16];
    mx_ident(s);
    s[0] = sx;
    s[5] = sy;
    s[10] = sz;
    mx_mul(out, m, s);
}

static void mx_rot_z(double *out, const double *m, double deg)
{
    double r[16], a = deg2rad(deg);
    mx_ident(r);
    r[0] = SDL_cos(a);
    r[1] = SDL_sin(a);
    r[4] = -SDL_sin(a);
    r[5] = SDL_cos(a);
    mx_mul(out, m, r);
}

static void mx_rot_y(double *out, const double *m, double deg)
{
    double r[16], a = deg2rad(deg);
    mx_ident(r);
    r[0] = SDL_cos(a);
    r[2] = -SDL_sin(a);
    r[8] = SDL_sin(a);
    r[10] = SDL_cos(a);
    mx_mul(out, m, r);
}

static void mx_rot_x(double *out, const double *m, double deg)
{
    double r[16], a = deg2rad(deg);
    mx_ident(r);
    r[5] = SDL_cos(a);
    r[6] = SDL_sin(a);
    r[9] = -SDL_sin(a);
    r[10] = SDL_cos(a);
    mx_mul(out, m, r);
}

static void mx_skew_x(double *out, const double *m, double deg)
{
    double s[16];
    mx_ident(s);
    s[4] = SDL_tan(deg2rad(deg));
    mx_mul(out, m, s);
}

static void mx_skew_y(double *out, const double *m, double deg)
{
    double s[16];
    mx_ident(s);
    s[1] = SDL_tan(deg2rad(deg));
    mx_mul(out, m, s);
}

static bool mx_is_identity(const double *m)
{
    double id[16];
    mx_ident(id);
    return SDL_memcmp(m, id, sizeof id) == 0;
}

/* ------------------------------------------------------------------ transforms (parse and serialize) */

static void tf_set_matrix6(sv *t, const double a[6])
{
    SDL_memcpy(t->u.tf.m, a, 6 * sizeof(double));
}

/* The matrix of a transform from its type and parameters. */
static void tf_set_translate(sv *t, double tx, double ty)
{
    const double m[6] = {1, 0, 0, 1, tx, ty};
    t->u.tf.type = SVG_TF_TRANSLATE;
    t->u.tf.angle = 0;
    tf_set_matrix6(t, m);
}

static void tf_set_scale(sv *t, double sx, double sy)
{
    const double m[6] = {sx, 0, 0, sy, 0, 0};
    t->u.tf.type = SVG_TF_SCALE;
    t->u.tf.angle = 0;
    tf_set_matrix6(t, m);
}

static void tf_set_rotate(sv *t, double deg, double cx, double cy)
{
    double a = deg2rad(deg), co = SDL_cos(a), si = SDL_sin(a);
    /* translate(cx cy) rotate(a) translate(-cx -cy) */
    const double m[6] = {co, si, -si, co, cx - co * cx + si * cy, cy - si * cx - co * cy};
    t->u.tf.type = SVG_TF_ROTATE;
    t->u.tf.angle = deg;
    t->u.tf.cx = cx;
    t->u.tf.cy = cy;
    tf_set_matrix6(t, m);
}

static void tf_set_skew_x(sv *t, double deg)
{
    const double m[6] = {1, 0, SDL_tan(deg2rad(deg)), 1, 0, 0};
    t->u.tf.type = SVG_TF_SKEWX;
    t->u.tf.angle = deg;
    tf_set_matrix6(t, m);
}

static void tf_set_skew_y(sv *t, double deg)
{
    const double m[6] = {1, SDL_tan(deg2rad(deg)), 0, 1, 0, 0};
    t->u.tf.type = SVG_TF_SKEWY;
    t->u.tf.angle = deg;
    tf_set_matrix6(t, m);
}

static void tf_set_matrix(sv *t, const double a[6])
{
    t->u.tf.type = SVG_TF_MATRIX;
    t->u.tf.angle = 0;
    tf_set_matrix6(t, a);
}

static void tf_serialize(embi_sb *sb, const sv *t)
{
    const double *m = t->u.tf.m;
    switch (t->u.tf.type) {
    case SVG_TF_TRANSLATE:
        embi_sb_append(sb, EMBI_STR("translate("));
        sb_num(sb, m[4]);
        embi_sb_append_char(sb, ' ');
        sb_num(sb, m[5]);
        break;
    case SVG_TF_SCALE:
        embi_sb_append(sb, EMBI_STR("scale("));
        sb_num(sb, m[0]);
        embi_sb_append_char(sb, ' ');
        sb_num(sb, m[3]);
        break;
    case SVG_TF_ROTATE:
        embi_sb_append(sb, EMBI_STR("rotate("));
        sb_num(sb, t->u.tf.angle);
        if (t->u.tf.cx != 0 || t->u.tf.cy != 0) {
            embi_sb_append_char(sb, ' ');
            sb_num(sb, t->u.tf.cx);
            embi_sb_append_char(sb, ' ');
            sb_num(sb, t->u.tf.cy);
        }
        break;
    case SVG_TF_SKEWX:
        embi_sb_append(sb, EMBI_STR("skewX("));
        sb_num(sb, t->u.tf.angle);
        break;
    case SVG_TF_SKEWY:
        embi_sb_append(sb, EMBI_STR("skewY("));
        sb_num(sb, t->u.tf.angle);
        break;
    default:
        embi_sb_append(sb, EMBI_STR("matrix("));
        for (int i = 0; i < 6; i++) {
            if (i) {
                embi_sb_append_char(sb, ' ');
            }
            sb_num(sb, m[i]);
        }
        break;
    }
    embi_sb_append_char(sb, ')');
}

/* Numbers separated by whitespace and commas inside a function's parentheses; returns the count (up to max), or -1. */
static int scan_args(embi_str s, uint32_t *pos, double *out, int max)
{
    uint32_t p = *pos;
    int n = 0;
    skip_ws(s, &p);
    if (p >= s.len || s.ptr[p] != '(') {
        return -1;
    }
    p++;
    for (;;) {
        skip_ws(s, &p);
        if (p < s.len && s.ptr[p] == ')') {
            p++;
            break;
        }
        if (n > 0 && p < s.len && s.ptr[p] == ',') {
            p++;
            skip_ws(s, &p);
        }
        double v;
        if (n >= max || !scan_num(s, &p, &v)) {
            return -1;
        }
        out[n++] = v;
    }
    *pos = p;
    return n;
}

/* One transform function at *pos; false when it is invalid. */
static bool tf_parse_one(embi_str s, uint32_t *pos, sv *t)
{
    uint32_t p = *pos;
    double a[6];
    int n;
    if (s.len - p >= 6 && SDL_memcmp(s.ptr + p, "matrix", 6) == 0) {
        p += 6;
        n = scan_args(s, &p, a, 6);
        if (n != 6) {
            return false;
        }
        tf_set_matrix(t, a);
    } else if (s.len - p >= 9 && SDL_memcmp(s.ptr + p, "translate", 9) == 0) {
        p += 9;
        n = scan_args(s, &p, a, 2);
        if (n < 1) {
            return false;
        }
        tf_set_translate(t, a[0], n == 2 ? a[1] : 0);
    } else if (s.len - p >= 5 && SDL_memcmp(s.ptr + p, "scale", 5) == 0) {
        p += 5;
        n = scan_args(s, &p, a, 2);
        if (n < 1) {
            return false;
        }
        tf_set_scale(t, a[0], n == 2 ? a[1] : a[0]);
    } else if (s.len - p >= 6 && SDL_memcmp(s.ptr + p, "rotate", 6) == 0) {
        p += 6;
        n = scan_args(s, &p, a, 3);
        if (n != 1 && n != 3) {
            return false;
        }
        tf_set_rotate(t, a[0], n == 3 ? a[1] : 0, n == 3 ? a[2] : 0);
    } else if (s.len - p >= 5 && SDL_memcmp(s.ptr + p, "skewX", 5) == 0) {
        p += 5;
        n = scan_args(s, &p, a, 1);
        if (n != 1) {
            return false;
        }
        tf_set_skew_x(t, a[0]);
    } else if (s.len - p >= 5 && SDL_memcmp(s.ptr + p, "skewY", 5) == 0) {
        p += 5;
        n = scan_args(s, &p, a, 1);
        if (n != 1) {
            return false;
        }
        tf_set_skew_y(t, a[0]);
    } else {
        return false;
    }
    *pos = p;
    return true;
}

/* ------------------------------------------------------------------ lengths and angles */

static uint16_t parse_len_unit(embi_str s, uint32_t *pos)
{
    uint32_t p = *pos;
    if (p < s.len && s.ptr[p] == '%') {
        *pos = p + 1;
        return SVG_LT_PERCENTAGE;
    }
    static const struct {
        const char *w;
        uint16_t unit;
    } units[] = {{"em", SVG_LT_EMS}, {"ex", SVG_LT_EXS}, {"px", SVG_LT_PX}, {"cm", SVG_LT_CM},
                 {"mm", SVG_LT_MM},  {"in", SVG_LT_IN},  {"pt", SVG_LT_PT}, {"pc", SVG_LT_PC}};
    for (uint32_t i = 0; i < sizeof units / sizeof units[0]; i++) {
        if (ieq_prefix(s, p, units[i].w)) {
            *pos = p + 2;
            return units[i].unit;
        }
    }
    return SVG_LT_NUMBER;
}

/* A length at *pos (no leading whitespace). */
static bool scan_len(embi_str s, uint32_t *pos, double *v, uint16_t *unit)
{
    uint32_t p = *pos;
    if (!scan_num(s, &p, v)) {
        return false;
    }
    *unit = parse_len_unit(s, &p);
    *pos = p;
    return true;
}

static bool parse_len_full(embi_str s, double *v, uint16_t *unit)
{
    uint32_t p = 0;
    skip_ws(s, &p);
    if (!scan_len(s, &p, v, unit)) {
        return false;
    }
    skip_ws(s, &p);
    return p == s.len;
}

static const char *len_unit_text(uint16_t unit)
{
    static const char *const t[SVG_LT_COUNT] = {"", "", "%", "em", "ex", "px", "cm", "mm", "in", "pt", "pc"};
    return unit < SVG_LT_COUNT ? t[unit] : "";
}

static void len_serialize(embi_sb *sb, const sv *p)
{
    sb_num(sb, p->u.num);
    embi_sb_append(sb, cstr(len_unit_text(p->unit)));
}

static const char *ang_unit_text(uint16_t unit)
{
    return unit == SVG_AT_DEG ? "deg" : unit == SVG_AT_RAD ? "rad" : unit == SVG_AT_GRAD ? "grad" : unit == SVG_AT_TURN ? "turn" : "";
}

static void ang_serialize(embi_sb *sb, const sv *p)
{
    sb_num(sb, p->u.num);
    embi_sb_append(sb, cstr(ang_unit_text(p->unit)));
}

static bool parse_ang_full(embi_str s, double *v, uint16_t *unit)
{
    uint32_t p = 0;
    skip_ws(s, &p);
    if (!scan_num(s, &p, v)) {
        return false;
    }
    *unit = SVG_AT_UNSPECIFIED;
    if (ieq_prefix(s, p, "deg")) {
        *unit = SVG_AT_DEG;
        p += 3;
    } else if (ieq_prefix(s, p, "grad")) {
        *unit = SVG_AT_GRAD;
        p += 4;
    } else if (ieq_prefix(s, p, "rad")) {
        *unit = SVG_AT_RAD;
        p += 3;
    } else if (ieq_prefix(s, p, "turn")) {
        *unit = SVG_AT_TURN;
        p += 4;
    }
    skip_ws(s, &p);
    return p == s.len;
}

/* ------------------------------------------------------------------ the length context (units) */

static int axis_of(uint32_t attr)
{
    switch (attr) {
    case SA_x: case SA_width: case SA_rx: case SA_cx: case SA_fx: case SA_x1: case SA_x2: case SA_refX:
    case SA_markerWidth: case SA_dx: case SA_startOffset: case SA_textLength:
        return 0;
    case SA_y: case SA_height: case SA_ry: case SA_cy: case SA_fy: case SA_y1: case SA_y2: case SA_refY:
    case SA_markerHeight: case SA_dy:
        return 1;
    default:
        return 2;
    }
}

/* The size of the viewport the percentages of an element refer to. */
static void viewport_size(embi_bind_ctx *c, embi_node *n, double *w, double *h)
{
    for (embi_node *p = n ? n->parent : nullptr; p != nullptr; p = p->parent) {
        if (p->type == EMBI_NODE_ELEMENT && p->extra == EMBI_NS_SVG &&
            embi_str_eq(embi_atom_str(p->name), EMBI_STR("svg"))) {
            embi_str t = {"", 0};
            if (embi_sv_attr_text(c, p, SA_viewBox, &t)) {
                double v[4];
                uint32_t q = 0;
                int k = 0;
                while (k < 4) {
                    while (q < t.len && (svg_ws(t.ptr[q]) || t.ptr[q] == ',')) {
                        q++;
                    }
                    if (!scan_num(t, &q, &v[k])) {
                        break;
                    }
                    k++;
                }
                if (k == 4 && v[2] > 0 && v[3] > 0) {
                    *w = v[2];
                    *h = v[3];
                    return;
                }
            }
            double ww, hh;
            uint16_t u1, u2;
            if (embi_sv_attr_text(c, p, SA_width, &t) && parse_len_full(t, &ww, &u1) && u1 != SVG_LT_PERCENTAGE &&
                embi_sv_attr_text(c, p, SA_height, &t) && parse_len_full(t, &hh, &u2) && u2 != SVG_LT_PERCENTAGE) {
                *w = ww;
                *h = hh;
                return;
            }
        }
    }
    double dpr = 1;
    embi_bind_viewport(c, w, h, &dpr);
}

/* The computed font-size of an element in px (16 without a style engine or an element). */
static double font_size_px(embi_bind_ctx *c, embi_node *n)
{
    embi_style_engine *se = embi_bind_style(c);
    if (se == nullptr || n == nullptr || n->type != EMBI_NODE_ELEMENT || n->doc != c->doc) {
        /* Only the context's own page document is attached to the engine: a script-made or inert document (DOMParser,
         * createHTMLDocument, template contents) is freed by the GC without a forget call, so the engine must never
         * learn it (it would keep a dangling pointer). Such a document resolves with the initial 16px. */
        return 16;
    }
    /* The engine learns a document when the embed layer hands it the sheets (embi_doc apply_sheets), which is after the
     * parse; a script that runs during the parse (the tests of the platform do) reads em and ex before that. Such a read
     * attaches the document with no author sheets, which is exactly what apply_sheets sets next: its call replaces them (and
     * marks the document dirty), so the sheets still take effect (test svg_em_reads_the_engine_and_leaves_later_sheets_alone).
     * A document the engine already manages is left as it is; the resolve is a no-op when nothing changed. */
    if (embi_style_document_epoch(se, n->doc) == 0) {
        embi_document_set_author_sheets(se, n->doc, nullptr, 0);
    }
    embi_style_resolve_document(se, n->doc);
    const embi_style *st = embi_style_get(se, (embi_element *)n, EMBI_PE_NONE);
    embi_prop_id id = embi_prop_lookup(EMBI_STR("font-size"));
    embi_sb sb = {0};
    double px = 16;
    if (st != nullptr && id != 0 && embi_style_value_text(st, id, &sb)) {
        uint32_t q = 0;
        embi_str t = {sb.data ? sb.data : "", sb.len};
        double v;
        if (scan_num(t, &q, &v) && t.len - q >= 2 && t.ptr[q] == 'p' && t.ptr[q + 1] == 'x') {
            px = v;
        }
    }
    embi_sb_free(&sb);
    return px;
}

/* The factor (user units per unit) of a unit; false when it needs a context that is missing. */
static bool unit_factor(embi_bind_ctx *c, embi_node *n, uint32_t attr, uint16_t unit, double *f)
{
    switch (unit) {
    case SVG_LT_NUMBER:
    case SVG_LT_PX:
        *f = 1;
        return true;
    case SVG_LT_CM:
        *f = 96.0 / 2.54;
        return true;
    case SVG_LT_MM:
        *f = 96.0 / 25.4;
        return true;
    case SVG_LT_IN:
        *f = 96;
        return true;
    case SVG_LT_PT:
        *f = 96.0 / 72.0;
        return true;
    case SVG_LT_PC:
        *f = 16;
        return true;
    case SVG_LT_EMS:
        *f = font_size_px(c, n);
        return true;
    case SVG_LT_EXS:
        *f = font_size_px(c, n) / 2;
        return true;
    case SVG_LT_PERCENTAGE: {
        if (n == nullptr) {
            *f = 1; /* a detached length has no viewport: the number stands for itself */
            return true;
        }
        double w, h;
        viewport_size(c, n, &w, &h);
        int ax = axis_of(attr);
        *f = (ax == 0 ? w : ax == 1 ? h : SDL_sqrt((w * w + h * h) / 2)) / 100.0;
        return true;
    }
    default:
        return false;
    }
}

static bool len_to_user(embi_bind_ctx *c, const sv *p, double *out)
{
    double f;
    if (!unit_factor(c, ctx_node(p), p->attr, p->unit, &f)) {
        return false;
    }
    *out = p->u.num * f;
    return true;
}

/* ------------------------------------------------------------------ preserveAspectRatio */

static const char *const par_aligns[] = {"", "none", "xMinYMin", "xMidYMin", "xMaxYMin", "xMinYMid",
                                         "xMidYMid", "xMaxYMid", "xMinYMax", "xMidYMax", "xMaxYMax"};

static void par_serialize(embi_sb *sb, const sv *p)
{
    uint8_t a = p->u.par.align;
    embi_sb_append(sb, cstr(par_aligns[a >= 1 && a <= 10 ? a : 6]));
    embi_sb_append(sb, p->u.par.mos == 2 ? EMBI_STR(" slice") : EMBI_STR(" meet"));
}

static bool par_parse(embi_str s, sv *p)
{
    uint32_t q = 0;
    skip_ws(s, &q);
    if (s.len - q >= 5 && SDL_memcmp(s.ptr + q, "defer", 5) == 0 && q + 5 < s.len && svg_ws(s.ptr[q + 5])) {
        q += 5;
        skip_ws(s, &q);
    }
    uint32_t b = q;
    while (q < s.len && !svg_ws(s.ptr[q])) {
        q++;
    }
    int align = 0;
    for (int i = 1; i <= 10; i++) {
        if (q - b == SDL_strlen(par_aligns[i]) && SDL_memcmp(s.ptr + b, par_aligns[i], q - b) == 0) {
            align = i;
        }
    }
    if (align == 0) {
        return false;
    }
    skip_ws(s, &q);
    int mos = 1;
    if (q < s.len) {
        uint32_t b2 = q;
        while (q < s.len && !svg_ws(s.ptr[q])) {
            q++;
        }
        if (q - b2 == 4 && SDL_memcmp(s.ptr + b2, "meet", 4) == 0) {
            mos = 1;
        } else if (q - b2 == 5 && SDL_memcmp(s.ptr + b2, "slice", 5) == 0) {
            mos = 2;
        } else {
            return false;
        }
        skip_ws(s, &q);
        if (q != s.len) {
            return false;
        }
    }
    p->u.par.align = (uint8_t)align;
    p->u.par.mos = (uint8_t)mos;
    return true;
}

/* ------------------------------------------------------------------ list items: parse and serialize */

void embi_sv_free_items(sv *items, uint32_t cap)
{
    if (items != nullptr) {
        embi_free(items, (size_t)cap * sizeof *items);
    }
}

/* Room for one more item after cnt: the capacity doubles, so parsing a list of n items is linear. */
static sv *items_grow(sv *items, uint32_t cnt, uint32_t *cap)
{
    if (cnt == *cap) {
        uint32_t ncap = *cap ? *cap * 2 : 8;
        items = embi_realloc(items, (size_t)*cap * sizeof *items, (size_t)ncap * sizeof *items);
        *cap = ncap;
    }
    SDL_memset(&items[cnt], 0, sizeof *items);
    return items;
}

static void item_init(sv *it, uint8_t kind)
{
    it->kind = kind;
    switch (kind) {
    case SV_LEN:
        it->unit = SVG_LT_NUMBER;
        break;
    case SV_PT:
        it->u.r[3] = 1;
        break;
    case SV_TF:
        it->u.tf.m[0] = it->u.tf.m[3] = 1;
        break;
    default:
        break;
    }
}

bool embi_sv_parse_list(embi_str s, uint8_t kind, uint32_t attr, sv **out, uint32_t *n, uint32_t *cap)
{
    sv *items = nullptr;
    uint32_t cnt = 0, alloc = 0;
    uint32_t p = 0;
    bool ok = true;
    (void)attr;
    skip_ws(s, &p);
    while (p < s.len) {
        if (cnt > 0 && s.ptr[p] == ',') {
            p++;
            skip_ws(s, &p);
            if (p >= s.len) {
                /* a trailing comma: invalid in a transform list, ignored elsewhere */
                ok = kind != SV_TF;
                break;
            }
        }
        items = items_grow(items, cnt, &alloc);
        sv *it = &items[cnt];
        item_init(it, kind);
        bool good = false;
        bool truncated = false;
        switch (kind) {
        case SV_NUM:
            good = scan_num(s, &p, &it->u.num);
            break;
        case SV_LEN:
            good = scan_len(s, &p, &it->u.num, &it->unit);
            break;
        case SV_PT: {
            double x, y;
            good = scan_num(s, &p, &x);
            if (good) {
                uint32_t q = p;
                while (q < s.len && (svg_ws(s.ptr[q]) || s.ptr[q] == ',')) {
                    q++;
                }
                if (q >= s.len) {
                    truncated = true; /* a missing y ends the list */
                } else if (scan_num(s, &q, &y)) {
                    it->u.r[0] = x;
                    it->u.r[1] = y;
                    p = q;
                } else {
                    good = false;
                }
            }
            break;
        }
        case SV_TF:
            good = tf_parse_one(s, &p, it);
            break;
        default:
            break;
        }
        if (truncated) {
            break;
        }
        if (!good) {
            ok = false;
            break;
        }
        cnt++;
        skip_ws(s, &p);
    }
    if (!ok || cnt == 0) {
        embi_sv_free_items(items, alloc);
        *out = nullptr;
        *n = 0;
        *cap = 0;
        return ok;
    }
    *out = items;
    *n = cnt;
    *cap = alloc;
    return true;
}

void embi_sv_serialize_item(embi_sb *sb, const sv *p)
{
    switch (p->kind) {
    case SV_NUM:
        sb_num(sb, p->u.num);
        break;
    case SV_LEN:
        len_serialize(sb, p);
        break;
    case SV_PT:
        sb_num(sb, p->u.r[0]);
        embi_sb_append_char(sb, ',');
        sb_num(sb, p->u.r[1]);
        break;
    case SV_TF:
        tf_serialize(sb, p);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ load and store */

static void len_default(embi_bind_ctx *c, embi_node *n, sv *p)
{
    (void)c;
    p->u.num = 0;
    p->unit = SVG_LT_NUMBER;
    if (n == nullptr) {
        return;
    }
    embi_str tag = embi_atom_str(n->name);
    uint32_t a = p->attr;
    if (embi_str_eq(tag, EMBI_STR("svg")) && (a == SA_width || a == SA_height)) {
        p->u.num = 100;
        p->unit = SVG_LT_PERCENTAGE;
    } else if (embi_str_eq(tag, EMBI_STR("marker")) && (a == SA_markerWidth || a == SA_markerHeight)) {
        p->u.num = 3;
    } else if (embi_str_eq(tag, EMBI_STR("radialGradient")) &&
               (a == SA_cx || a == SA_cy || a == SA_r || a == SA_fx || a == SA_fy)) {
        p->u.num = 50;
        p->unit = SVG_LT_PERCENTAGE;
    } else if (embi_str_eq(tag, EMBI_STR("linearGradient")) && (a == SA_x1 || a == SA_y1 || a == SA_y2)) {
        p->unit = SVG_LT_PERCENTAGE;
    } else if (embi_str_eq(tag, EMBI_STR("radialGradient")) && a == SA_fr) {
        p->unit = SVG_LT_PERCENTAGE;
    } else if (embi_str_eq(tag, EMBI_STR("linearGradient")) && a == SA_x2) {
        p->u.num = 100;
        p->unit = SVG_LT_PERCENTAGE;
    } else if (embi_str_eq(tag, EMBI_STR("mask")) && (a == SA_x || a == SA_y)) {
        p->u.num = -10;
        p->unit = SVG_LT_PERCENTAGE;
    } else if (embi_str_eq(tag, EMBI_STR("mask")) && (a == SA_width || a == SA_height)) {
        p->u.num = 120;
        p->unit = SVG_LT_PERCENTAGE;
    }
}

static void attr_load(embi_bind_ctx *c, sv *p, embi_node *n)
{
    embi_str t = {"", 0};
    bool has = embi_sv_attr_text(c, n, p->attr, &t);
    switch (p->kind) {
    case SV_LEN: {
        double v;
        uint16_t u;
        if (has && parse_len_full(t, &v, &u)) {
            p->u.num = v;
            p->unit = u;
        } else {
            len_default(c, n, p);
        }
        break;
    }
    case SV_ANGLE: {
        double v;
        uint16_t u;
        if (has && parse_ang_full(t, &v, &u)) {
            p->u.num = v;
            p->unit = u;
        } else {
            p->u.num = 0;
            p->unit = SVG_AT_UNSPECIFIED;
        }
        break;
    }
    case SV_RECT: {
        double v[4] = {0, 0, 0, 0};
        uint32_t q = 0;
        int k = 0;
        while (has && k < 4) {
            while (q < t.len && (svg_ws(t.ptr[q]) || t.ptr[q] == ',')) {
                q++;
            }
            if (!scan_num(t, &q, &v[k])) {
                break;
            }
            k++;
        }
        skip_ws(t, &q);
        if (has && k == 4 && q == t.len && v[2] >= 0 && v[3] >= 0) {
            SDL_memcpy(p->u.r, v, sizeof v);
        } else {
            SDL_memset(p->u.r, 0, sizeof p->u.r);
        }
        break;
    }
    case SV_PAR: {
        sv tmp;
        SDL_memset(&tmp, 0, sizeof tmp);
        tmp.kind = SV_PAR;
        tmp.u.par = p->u.par;
        if (has && par_parse(t, &tmp)) {
            p->u.par = tmp.u.par;
        } else {
            p->u.par.align = 6;
            p->u.par.mos = 1;
        }
        break;
    }
    default:
        break;
    }
}

static void attr_store(embi_bind_ctx *c, sv *p, embi_node *n)
{
    embi_sb sb = {0};
    switch (p->kind) {
    case SV_LEN:
        len_serialize(&sb, p);
        break;
    case SV_ANGLE:
        ang_serialize(&sb, p);
        break;
    case SV_RECT:
        for (int i = 0; i < 4; i++) {
            if (i) {
                embi_sb_append_char(&sb, ' ');
            }
            sb_num(&sb, p->u.r[i]);
        }
        break;
    case SV_PAR:
        par_serialize(&sb, p);
        break;
    default:
        break;
    }
    embi_sv_set_attr(c, n, p->attr, (embi_str){sb.data ? sb.data : "", sb.len});
    embi_sb_free(&sb);
}

void embi_sv_load(embi_bind_ctx *c, sv *p)
{
    if (p->bound == SB_FREE) {
        return;
    }
    embi_bind_obj *par = embi_sv_parent(p);
    if (par == nullptr) {
        return;
    }
    switch (p->bound) {
    case SB_ATTR:
        if (par->is_node) {
            attr_load(c, p, par->ptr);
        }
        break;
    case SB_ITEM:
        embi_svg_list_sync(c, par);
        break;
    case SB_MATRIX: {
        sv *t = par->ptr;
        embi_sv_load(c, t);
        mx_from6(p->u.mx.m, t->u.tf.m);
        p->u.mx.is2d = true;
        break;
    }
    default:
        break;
    }
}

void embi_sv_store(embi_bind_ctx *c, sv *p)
{
    if (p->bound == SB_FREE || p->ro) {
        return; /* an animVal never writes its attribute, whatever path reached here */
    }
    embi_bind_obj *par = embi_sv_parent(p);
    if (par == nullptr) {
        return;
    }
    switch (p->bound) {
    case SB_ATTR:
        if (par->is_node) {
            attr_store(c, p, par->ptr);
        }
        break;
    case SB_ITEM:
        embi_svg_list_commit(c, par);
        break;
    case SB_MATRIX: {
        sv *t = par->ptr;
        if (t->ro) {
            break;
        }
        double a[6];
        mx_to6(p->u.mx.m, a);
        tf_set_matrix(t, a);
        embi_sv_store(c, t);
        break;
    }
    default:
        break;
    }
}

static embi_dom_err ro_check(embi_bind_ctx *c, const sv *p)
{
    if (p->ro) {
        return embi_impl_throw(c, "NoModificationAllowedError", "The object is read-only.");
    }
    return EMBI_DOM_OK;
}

/* ------------------------------------------------------------------ SVGNumber */

embi_dom_err embi_impl_SVGNumber_get_value(embi_bind_ctx *c, embi_bind_obj *self, double *out)
{
    sv *p = self->ptr;
    embi_sv_load(c, p);
    *out = p->u.num;
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_SVGNumber_set_value(embi_bind_ctx *c, embi_bind_obj *self, double a0)
{
    sv *p = self->ptr;
    {
        embi_dom_err ro_e = ro_check(c, p);
        if (ro_e) {
            return ro_e;
        }
    }
    if (embi_sv_float_range_error(c, a0)) {
        return EMBI_BIND_THREW;
    }
    embi_sv_load(c, p);
    p->u.num = a0;
    embi_sv_store(c, p);
    return EMBI_DOM_OK;
}

/* ------------------------------------------------------------------ SVGLength */

uint16_t embi_impl_SVGLength_get_unitType(embi_bind_ctx *c, embi_bind_obj *self)
{
    sv *p = self->ptr;
    embi_sv_load(c, p);
    return p->unit;
}

embi_dom_err embi_impl_SVGLength_get_value(embi_bind_ctx *c, embi_bind_obj *self, double *out)
{
    sv *p = self->ptr;
    embi_sv_load(c, p);
    double v = 0;
    if (!len_to_user(c, p, &v)) {
        v = 0;
    }
    *out = v;
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_SVGLength_set_value(embi_bind_ctx *c, embi_bind_obj *self, double a0)
{
    sv *p = self->ptr;
    embi_dom_err e = ro_check(c, p);
    if (e) {
        return e;
    }
    embi_sv_load(c, p);
    double f;
    if (!unit_factor(c, ctx_node(p), p->attr, p->unit, &f) || f == 0) {
        return embi_impl_throw(c, "NotSupportedError", "The unit cannot be converted without a context.");
    }
    p->u.num = a0 / f;
    embi_sv_store(c, p);
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_SVGLength_get_valueInSpecifiedUnits(embi_bind_ctx *c, embi_bind_obj *self, double *out)
{
    sv *p = self->ptr;
    embi_sv_load(c, p);
    *out = p->u.num;
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_SVGLength_set_valueInSpecifiedUnits(embi_bind_ctx *c, embi_bind_obj *self, double a0)
{
    sv *p = self->ptr;
    embi_dom_err e = ro_check(c, p);
    if (e) {
        return e;
    }
    embi_sv_load(c, p);
    p->u.num = a0;
    embi_sv_store(c, p);
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_SVGLength_get_valueAsString(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr *out)
{
    sv *p = self->ptr;
    embi_sv_load(c, p);
    embi_sb sb = {0};
    len_serialize(&sb, p);
    *out = embi_bind_tmp_utf8(c, (embi_str){sb.data ? sb.data : "", sb.len});
    embi_sb_free(&sb);
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_SVGLength_set_valueAsString(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    sv *p = self->ptr;
    embi_dom_err e = ro_check(c, p);
    if (e) {
        return e;
    }
    embi_sv_load(c, p);
    double v;
    uint16_t u;
    if (!parse_len_full(embi_bind_utf8(c, a0), &v, &u)) {
        return embi_impl_throw(c, "SyntaxError", "The value is not a valid length.");
    }
    p->u.num = v;
    p->unit = u;
    embi_sv_store(c, p);
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_SVGLength_newValueSpecifiedUnits(embi_bind_ctx *c, embi_bind_obj *self, uint16_t a0, double a1)
{
    sv *p = self->ptr;
    embi_dom_err e = ro_check(c, p);
    if (e) {
        return e;
    }
    embi_sv_load(c, p);
    if (a0 < SVG_LT_NUMBER || a0 >= SVG_LT_COUNT) {
        return embi_impl_throw(c, "NotSupportedError", "The unit type is not valid.");
    }
    p->u.num = a1;
    p->unit = a0;
    embi_sv_store(c, p);
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_SVGLength_convertToSpecifiedUnits(embi_bind_ctx *c, embi_bind_obj *self, uint16_t a0)
{
    sv *p = self->ptr;
    embi_dom_err e = ro_check(c, p);
    if (e) {
        return e;
    }
    if (a0 < SVG_LT_NUMBER || a0 >= SVG_LT_COUNT) {
        return embi_impl_throw(c, "NotSupportedError", "The unit type is not valid.");
    }
    embi_sv_load(c, p);
    embi_node *n = ctx_node(p);
    double from, to;
    if (!unit_factor(c, n, p->attr, p->unit, &from) || !unit_factor(c, n, p->attr, a0, &to) || to == 0) {
        return embi_impl_throw(c, "NotSupportedError", "The unit cannot be converted without a context.");
    }
    p->u.num = p->u.num * from / to;
    p->unit = a0;
    embi_sv_store(c, p);
    return EMBI_DOM_OK;
}

/* ------------------------------------------------------------------ SVGAngle */

static double clamp_f(double v)
{
    return v > FLT_MAX ? FLT_MAX : v < -FLT_MAX ? -FLT_MAX : v;
}

static double ang_to_deg(const sv *p)
{
    double d = p->unit == SVG_AT_RAD    ? p->u.num * (180.0 / 3.14159265358979323846)
               : p->unit == SVG_AT_GRAD ? p->u.num * 0.9
               : p->unit == SVG_AT_TURN ? p->u.num * 360.0
                                        : p->u.num;
    return d;
}

uint16_t embi_impl_SVGAngle_get_unitType(embi_bind_ctx *c, embi_bind_obj *self)
{
    sv *p = self->ptr;
    embi_sv_load(c, p);
    return p->unit == SVG_AT_TURN ? 0 : p->unit;
}

embi_dom_err embi_impl_SVGAngle_get_value(embi_bind_ctx *c, embi_bind_obj *self, double *out)
{
    sv *p = self->ptr;
    embi_sv_load(c, p);
    *out = clamp_f(ang_to_deg(p));
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_SVGAngle_set_value(embi_bind_ctx *c, embi_bind_obj *self, double a0)
{
    sv *p = self->ptr;
    embi_dom_err e = ro_check(c, p);
    if (e) {
        return e;
    }
    embi_sv_load(c, p);
    if (p->unit == SVG_AT_RAD) {
        p->u.num = a0 * (3.14159265358979323846 / 180.0);
    } else if (p->unit == SVG_AT_GRAD) {
        p->u.num = a0 / 0.9;
    } else if (p->unit == SVG_AT_TURN) {
        p->u.num = a0 / 360.0;
    } else {
        p->u.num = a0;
        if (p->unit != SVG_AT_DEG) {
            p->unit = SVG_AT_UNSPECIFIED;
        }
    }
    embi_sv_store(c, p);
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_SVGAngle_get_valueInSpecifiedUnits(embi_bind_ctx *c, embi_bind_obj *self, double *out)
{
    sv *p = self->ptr;
    embi_sv_load(c, p);
    *out = clamp_f(p->u.num);
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_SVGAngle_set_valueInSpecifiedUnits(embi_bind_ctx *c, embi_bind_obj *self, double a0)
{
    sv *p = self->ptr;
    embi_dom_err e = ro_check(c, p);
    if (e) {
        return e;
    }
    embi_sv_load(c, p);
    p->u.num = a0;
    embi_sv_store(c, p);
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_SVGAngle_get_valueAsString(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr *out)
{
    sv *p = self->ptr;
    embi_sv_load(c, p);
    embi_sb sb = {0};
    ang_serialize(&sb, p);
    *out = embi_bind_tmp_utf8(c, (embi_str){sb.data ? sb.data : "", sb.len});
    embi_sb_free(&sb);
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_SVGAngle_set_valueAsString(embi_bind_ctx *c, embi_bind_obj *self, embi_dstr a0)
{
    sv *p = self->ptr;
    embi_dom_err e = ro_check(c, p);
    if (e) {
        return e;
    }
    embi_sv_load(c, p);
    double v;
    uint16_t u;
    if (!parse_ang_full(embi_bind_utf8(c, a0), &v, &u)) {
        return embi_impl_throw(c, "SyntaxError", "The value is not a valid angle.");
    }
    p->u.num = v;
    p->unit = u;
    embi_sv_store(c, p);
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_SVGAngle_newValueSpecifiedUnits(embi_bind_ctx *c, embi_bind_obj *self, uint16_t a0, double a1)
{
    sv *p = self->ptr;
    embi_dom_err e = ro_check(c, p);
    if (e) {
        return e;
    }
    embi_sv_load(c, p);
    if (a0 < SVG_AT_UNSPECIFIED || a0 > SVG_AT_GRAD) {
        return embi_impl_throw(c, "NotSupportedError", "The unit type is not valid.");
    }
    p->u.num = a1;
    p->unit = a0;
    embi_sv_store(c, p);
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_SVGAngle_convertToSpecifiedUnits(embi_bind_ctx *c, embi_bind_obj *self, uint16_t a0)
{
    sv *p = self->ptr;
    embi_dom_err e = ro_check(c, p);
    if (e) {
        return e;
    }
    if (a0 < SVG_AT_UNSPECIFIED || a0 > SVG_AT_GRAD) {
        return embi_impl_throw(c, "NotSupportedError", "The unit type is not valid.");
    }
    embi_sv_load(c, p);
    double deg = ang_to_deg(p);
    p->unit = a0;
    p->u.num = clamp_f(a0 == SVG_AT_RAD ? deg * (3.14159265358979323846 / 180.0) : a0 == SVG_AT_GRAD ? deg / 0.9 : deg);
    embi_sv_store(c, p);
    return EMBI_DOM_OK;
}

/* ------------------------------------------------------------------ points and rectangles */

static double min2(double a, double b)
{
    return a < b ? a : b;
}

static double max2(double a, double b)
{
    return a > b ? a : b;
}

/* The members of a point or a rectangle live in u.r[IDX]. A read refreshes the payload from what it is bound to, a write
 * refuses an animVal, changes the member and writes the payload back. The accessors of SVGPoint and SVGRect are the same
 * code: the generated glue has already converted their argument to a float. */
#define NUM_FIELD(IFACE, NAME, IDX)                                                                   \
    embi_dom_err embi_impl_##IFACE##_get_##NAME(embi_bind_ctx *c, embi_bind_obj *self, double *out)    \
    {                                                                                                 \
        sv *p = self->ptr;                                                                            \
        embi_sv_load(c, p);                                                                           \
        *out = p->u.r[IDX];                                                                           \
        return EMBI_DOM_OK;                                                                           \
    }                                                                                                 \
    embi_dom_err embi_impl_##IFACE##_set_##NAME(embi_bind_ctx *c, embi_bind_obj *self, double a0)      \
    {                                                                                                 \
        sv *p = self->ptr;                                                                            \
        embi_dom_err e = ro_check(c, p);                                                              \
        if (e) {                                                                                      \
            return e;                                                                                 \
        }                                                                                             \
        embi_sv_load(c, p);                                                                           \
        p->u.r[IDX] = a0;                                                                             \
        embi_sv_store(c, p);                                                                          \
        return EMBI_DOM_OK;                                                                           \
    }

#define NUM_GET(IFACE, NAME, EXPR)                                                                    \
    double embi_impl_##IFACE##_get_##NAME(embi_bind_ctx *c, embi_bind_obj *self)                       \
    {                                                                                                 \
        sv *p = self->ptr;                                                                            \
        embi_sv_load(c, p);                                                                           \
        const double *r = p->u.r;                                                                     \
        return (EXPR);                                                                                \
    }

NUM_FIELD(SVGPoint, x, 0)
NUM_FIELD(SVGPoint, y, 1)
NUM_FIELD(SVGRect, x, 0)
NUM_FIELD(SVGRect, y, 1)
NUM_FIELD(SVGRect, width, 2)
NUM_FIELD(SVGRect, height, 3)

NUM_FIELD(DOMPoint, x, 0)
NUM_FIELD(DOMPoint, y, 1)
NUM_FIELD(DOMPoint, z, 2)
NUM_FIELD(DOMPoint, w, 3)
NUM_GET(DOMPointReadOnly, x, r[0])
NUM_GET(DOMPointReadOnly, y, r[1])
NUM_GET(DOMPointReadOnly, z, r[2])
NUM_GET(DOMPointReadOnly, w, r[3])

NUM_FIELD(DOMRect, x, 0)
NUM_FIELD(DOMRect, y, 1)
NUM_FIELD(DOMRect, width, 2)
NUM_FIELD(DOMRect, height, 3)
NUM_GET(DOMRectReadOnly, x, r[0])
NUM_GET(DOMRectReadOnly, y, r[1])
NUM_GET(DOMRectReadOnly, width, r[2])
NUM_GET(DOMRectReadOnly, height, r[3])
NUM_GET(DOMRectReadOnly, top, min2(r[1], r[1] + r[3]))
NUM_GET(DOMRectReadOnly, right, max2(r[0], r[0] + r[2]))
NUM_GET(DOMRectReadOnly, bottom, max2(r[1], r[1] + r[3]))
NUM_GET(DOMRectReadOnly, left, min2(r[0], r[0] + r[2]))

static embi_js_value point_new(embi_bind_ctx *c, uint16_t iface, double x, double y, double z, double w)
{
    embi_js_value v;
    sv *p = embi_sv_new(c, iface, SV_PT, &v);
    p->u.r[0] = x;
    p->u.r[1] = y;
    p->u.r[2] = z;
    p->u.r[3] = w;
    return v;
}

embi_js_value embi_svg_dompoint_new(embi_bind_ctx *c, double x, double y, double z, double w)
{
    return point_new(c, EMBI_IF_DOMPoint, x, y, z, w);
}

embi_js_value embi_svg_svgpoint_new(embi_bind_ctx *c, double x, double y)
{
    return point_new(c, EMBI_IF_SVGPoint, x, y, 0, 1);
}

static embi_js_value rect_new(embi_bind_ctx *c, uint16_t iface, double x, double y, double w, double h)
{
    embi_js_value v;
    sv *p = embi_sv_new(c, iface, SV_RECT, &v);
    p->u.r[0] = x;
    p->u.r[1] = y;
    p->u.r[2] = w;
    p->u.r[3] = h;
    return v;
}

/* A DOMRect (getBoundingClientRect, getBBox, the items of a DOMRectList). */
embi_js_value embi_impl_rect_new(embi_bind_ctx *c, double x, double y, double w, double h)
{
    return rect_new(c, EMBI_IF_DOMRect, x, y, w, h);
}

/* ------------------------------------------------------------------ matrix payload helpers */

static embi_js_value new_mx6(embi_bind_ctx *c, uint16_t iface, const double m16[16], bool is2d)
{
    embi_js_value v;
    sv *p = embi_sv_new(c, iface, SV_MX, &v);
    SDL_memcpy(p->u.mx.m, m16, 16 * sizeof(double));
    p->u.mx.is2d = is2d;
    return embi_bind_pin(c, v);
}

embi_js_value embi_svg_dommatrix_new(embi_bind_ctx *c, const double m[6])
{
    double m16[16];
    mx_from6(m16, m);
    return new_mx6(c, EMBI_IF_DOMMatrix, m16, true);
}

/* The members of a DOMMatrix2DInit into a 4x4 matrix: an alias and its element must agree. False (a TypeError is
 * pending) when they do not. A DOMMatrix, an SVGMatrix or any object with the members passes as one. */
bool embi_svg_matrix_from_init(embi_bind_ctx *c, const embi_dict_DOMMatrix2DInit *d, double m[16])
{
    static const struct {
        int idx;
    } map[6] = {{0}, {1}, {4}, {5}, {12}, {13}};
    const double alias[6] = {d->a, d->b, d->c, d->d, d->e, d->f};
    const double elem[6] = {d->m11, d->m12, d->m21, d->m22, d->m41, d->m42};
    mx_ident(m);
    for (int i = 0; i < 6; i++) {
        bool ha = !(alias[i] != alias[i]);
        bool he = !(elem[i] != elem[i]);
        if (ha && he && alias[i] != elem[i]) {
            (void)embi_js_throw_type_error(embi_bind_runtime(c), "The matrix members disagree.");
            return false;
        }
        if (ha) {
            m[map[i].idx] = alias[i];
        } else if (he) {
            m[map[i].idx] = elem[i];
        }
    }
    return true;
}

/* ------------------------------------------------------------------ SVGMatrix */

/* a..f as floats (the generated glue converted a0); the other members, the operations and the *Self operations are the
 * DOMMatrix ones, which also read and write through the binding. */
#define SVGM_FIELD(NAME, IDX)                                                                                \
    embi_dom_err embi_impl_SVGMatrix_get_##NAME(embi_bind_ctx *c, embi_bind_obj *self, double *out)           \
    {                                                                                                        \
        sv *p = self->ptr;                                                                                   \
        embi_sv_load(c, p);                                                                                  \
        *out = p->u.mx.m[IDX];                                                                               \
        return EMBI_DOM_OK;                                                                                  \
    }                                                                                                        \
    embi_dom_err embi_impl_SVGMatrix_set_##NAME(embi_bind_ctx *c, embi_bind_obj *self, double a0)             \
    {                                                                                                        \
        sv *p = self->ptr;                                                                                   \
        embi_dom_err e = ro_check(c, p);                                                                     \
        if (e) {                                                                                             \
            return e;                                                                                        \
        }                                                                                                    \
        embi_sv_load(c, p);                                                                                  \
        p->u.mx.m[IDX] = a0;                                                                                 \
        embi_sv_store(c, p);                                                                                 \
        return EMBI_DOM_OK;                                                                                  \
    }

SVGM_FIELD(a, 0)
SVGM_FIELD(b, 1)
SVGM_FIELD(c, 4)
SVGM_FIELD(d, 5)
SVGM_FIELD(e, 12)
SVGM_FIELD(f, 13)

/* ------------------------------------------------------------------ SVGTransform */

uint16_t embi_impl_SVGTransform_get_type(embi_bind_ctx *c, embi_bind_obj *self)
{
    sv *p = self->ptr;
    embi_sv_load(c, p);
    return p->u.tf.type;
}

embi_js_value embi_impl_SVGTransform_get_matrix(embi_bind_ctx *c, embi_bind_obj *self)
{
    sv *t = self->ptr;
    embi_sv_load(c, t);
    embi_js_value v;
    sv *m = embi_sv_new(c, EMBI_IF_SVGMatrix, SV_MX, &v);
    m->bound = SB_MATRIX;
    m->ro = t->ro; /* the matrix of an animVal transform is read-only too */
    embi_bind_set_ref(m->self, embi_bind_obj_value(self));
    mx_from6(m->u.mx.m, t->u.tf.m);
    return v;
}

double embi_impl_SVGTransform_get_angle(embi_bind_ctx *c, embi_bind_obj *self)
{
    sv *p = self->ptr;
    embi_sv_load(c, p);
    return p->u.tf.angle;
}

embi_dom_err embi_impl_SVGTransform_setMatrix(embi_bind_ctx *c, embi_bind_obj *self, embi_dict_DOMMatrix2DInit a0)
{
    sv *t = self->ptr;
    {
        embi_dom_err ro_e = ro_check(c, t);
        if (ro_e) {
            return ro_e;
        }
    }
    double m[16];
    if (!embi_svg_matrix_from_init(c, &a0, m)) {
        return EMBI_BIND_THREW;
    }
    embi_sv_load(c, t);
    double a[6];
    mx_to6(m, a);
    tf_set_matrix(t, a);
    embi_sv_store(c, t);
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_SVGTransform_setTranslate(embi_bind_ctx *c, embi_bind_obj *self, double a0, double a1)
{
    sv *t = self->ptr;
    {
        embi_dom_err ro_e = ro_check(c, t);
        if (ro_e) {
            return ro_e;
        }
    }
    embi_sv_load(c, t);
    tf_set_translate(t, a0, a1);
    embi_sv_store(c, t);
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_SVGTransform_setScale(embi_bind_ctx *c, embi_bind_obj *self, double a0, double a1)
{
    sv *t = self->ptr;
    {
        embi_dom_err ro_e = ro_check(c, t);
        if (ro_e) {
            return ro_e;
        }
    }
    embi_sv_load(c, t);
    tf_set_scale(t, a0, a1);
    embi_sv_store(c, t);
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_SVGTransform_setRotate(embi_bind_ctx *c, embi_bind_obj *self, double a0, double a1, double a2)
{
    sv *t = self->ptr;
    {
        embi_dom_err ro_e = ro_check(c, t);
        if (ro_e) {
            return ro_e;
        }
    }
    embi_sv_load(c, t);
    tf_set_rotate(t, a0, a1, a2);
    embi_sv_store(c, t);
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_SVGTransform_setSkewX(embi_bind_ctx *c, embi_bind_obj *self, double a0)
{
    sv *t = self->ptr;
    {
        embi_dom_err ro_e = ro_check(c, t);
        if (ro_e) {
            return ro_e;
        }
    }
    embi_sv_load(c, t);
    tf_set_skew_x(t, a0);
    embi_sv_store(c, t);
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_SVGTransform_setSkewY(embi_bind_ctx *c, embi_bind_obj *self, double a0)
{
    sv *t = self->ptr;
    {
        embi_dom_err ro_e = ro_check(c, t);
        if (ro_e) {
            return ro_e;
        }
    }
    embi_sv_load(c, t);
    tf_set_skew_y(t, a0);
    embi_sv_store(c, t);
    return EMBI_DOM_OK;
}

embi_js_value embi_svg_new_transform_from_init(embi_bind_ctx *c, const embi_dict_DOMMatrix2DInit *init)
{
    double m[16];
    if (!embi_svg_matrix_from_init(c, init, m)) {
        return EMBI_JS_EXCEPTION;
    }
    embi_js_value v;
    sv *t = embi_sv_new(c, EMBI_IF_SVGTransform, SV_TF, &v);
    double a[6];
    mx_to6(m, a);
    tf_set_matrix(t, a);
    return v;
}

/* ------------------------------------------------------------------ SVGPreserveAspectRatio */

embi_dom_err embi_impl_SVGPreserveAspectRatio_get_align(embi_bind_ctx *c, embi_bind_obj *self, uint16_t *out)
{
    sv *p = self->ptr;
    embi_sv_load(c, p);
    *out = p->u.par.align;
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_SVGPreserveAspectRatio_set_align(embi_bind_ctx *c, embi_bind_obj *self, uint16_t a0)
{
    sv *p = self->ptr;
    embi_dom_err e = ro_check(c, p);
    if (e) {
        return e;
    }
    if (a0 < 1 || a0 > 10) {
        return throw_type(c, "The align value is not valid.");
    }
    embi_sv_load(c, p);
    p->u.par.align = (uint8_t)a0;
    embi_sv_store(c, p);
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_SVGPreserveAspectRatio_get_meetOrSlice(embi_bind_ctx *c, embi_bind_obj *self, uint16_t *out)
{
    sv *p = self->ptr;
    embi_sv_load(c, p);
    *out = p->u.par.mos;
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_SVGPreserveAspectRatio_set_meetOrSlice(embi_bind_ctx *c, embi_bind_obj *self, uint16_t a0)
{
    sv *p = self->ptr;
    embi_dom_err e = ro_check(c, p);
    if (e) {
        return e;
    }
    if (a0 < 1 || a0 > 2) {
        return throw_type(c, "The meetOrSlice value is not valid.");
    }
    embi_sv_load(c, p);
    p->u.par.mos = (uint8_t)a0;
    embi_sv_store(c, p);
    return EMBI_DOM_OK;
}

/* ------------------------------------------------------------------ DOMPoint, DOMPointReadOnly */

static embi_js_value dompoint_ctor(embi_bind_ctx *c, embi_js_value nt, uint16_t iface, double x, double y, double z, double w)
{
    embi_js_value v = point_new(c, iface, x, y, z, w);
    embi_bind_adopt_new_target(c, v, nt);
    return v;
}

embi_js_value embi_impl_DOMPoint_constructor(embi_bind_ctx *c, embi_js_value nt, double a0, double a1, double a2, double a3)
{
    return dompoint_ctor(c, nt, EMBI_IF_DOMPoint, a0, a1, a2, a3);
}

embi_js_value embi_impl_DOMPointReadOnly_constructor(embi_bind_ctx *c, embi_js_value nt, double a0, double a1, double a2,
                                                     double a3)
{
    return dompoint_ctor(c, nt, EMBI_IF_DOMPointReadOnly, a0, a1, a2, a3);
}

embi_js_value embi_impl_DOMPoint_fromPoint(embi_bind_ctx *c, embi_dict_DOMPointInit a0)
{
    return point_new(c, EMBI_IF_DOMPoint, a0.x, a0.y, a0.z, a0.w);
}

embi_js_value embi_impl_DOMPointReadOnly_fromPoint(embi_bind_ctx *c, embi_dict_DOMPointInit a0)
{
    return point_new(c, EMBI_IF_DOMPointReadOnly, a0.x, a0.y, a0.z, a0.w);
}

embi_js_value embi_impl_DOMPointReadOnly_matrixTransform(embi_bind_ctx *c, embi_bind_obj *self, embi_dict_DOMMatrix2DInit a0)
{
    sv *p = self->ptr;
    double m[16];
    if (!embi_svg_matrix_from_init(c, &a0, m)) {
        return EMBI_JS_EXCEPTION;
    }
    embi_sv_load(c, p);
    const double v[4] = {p->u.r[0], p->u.r[1], p->u.r[2], p->u.r[3]};
    double r[4];
    for (int row = 0; row < 4; row++) {
        r[row] = m[row] * v[0] + m[4 + row] * v[1] + m[8 + row] * v[2] + m[12 + row] * v[3];
    }
    return embi_svg_dompoint_new(c, r[0], r[1], r[2], r[3]);
}

embi_js_value embi_impl_DOMPointReadOnly_toJSON(embi_bind_ctx *c, embi_bind_obj *self)
{
    sv *p = self->ptr;
    embi_sv_load(c, p);
    embi_js_value o = embi_bind_pin(c, embi_bind_object_new(c));
    embi_bind_object_set_c(c, o, "x", embi_js_number(p->u.r[0]));
    embi_bind_object_set_c(c, o, "y", embi_js_number(p->u.r[1]));
    embi_bind_object_set_c(c, o, "z", embi_js_number(p->u.r[2]));
    embi_bind_object_set_c(c, o, "w", embi_js_number(p->u.r[3]));
    return o;
}

/* ------------------------------------------------------------------ DOMRect, DOMRectReadOnly */

static embi_js_value rect_ctor(embi_bind_ctx *c, embi_js_value nt, uint16_t iface, double x, double y, double w, double h)
{
    embi_js_value v = rect_new(c, iface, x, y, w, h);
    embi_bind_adopt_new_target(c, v, nt);
    return v;
}

embi_js_value embi_impl_DOMRectReadOnly_constructor(embi_bind_ctx *c, embi_js_value nt, double a0, double a1,
                                                    double a2, double a3)
{
    return rect_ctor(c, nt, EMBI_IF_DOMRectReadOnly, a0, a1, a2, a3);
}

embi_js_value embi_impl_DOMRect_constructor(embi_bind_ctx *c, embi_js_value nt, double a0, double a1,
                                            double a2, double a3)
{
    return rect_ctor(c, nt, EMBI_IF_DOMRect, a0, a1, a2, a3);
}

embi_js_value embi_impl_DOMRectReadOnly_toJSON(embi_bind_ctx *c, embi_bind_obj *self)
{
    sv *p = self->ptr;
    embi_sv_load(c, p);
    const double *r = p->u.r;
    embi_js_value o = embi_bind_object_new(c);
    embi_bind_pin(c, o);
    embi_bind_object_set_c(c, o, "x", embi_js_number(r[0]));
    embi_bind_object_set_c(c, o, "y", embi_js_number(r[1]));
    embi_bind_object_set_c(c, o, "width", embi_js_number(r[2]));
    embi_bind_object_set_c(c, o, "height", embi_js_number(r[3]));
    embi_bind_object_set_c(c, o, "top", embi_js_number(min2(r[1], r[1] + r[3])));
    embi_bind_object_set_c(c, o, "right", embi_js_number(max2(r[0], r[0] + r[2])));
    embi_bind_object_set_c(c, o, "bottom", embi_js_number(max2(r[1], r[1] + r[3])));
    embi_bind_object_set_c(c, o, "left", embi_js_number(min2(r[0], r[0] + r[2])));
    return o;
}

/* ------------------------------------------------------------------ DOMMatrix, DOMMatrixReadOnly */

static const char *const dm_names[16] = {"m11", "m12", "m13", "m14", "m21", "m22", "m23", "m24",
                                         "m31", "m32", "m33", "m34", "m41", "m42", "m43", "m44"};

static bool dm_all_default_3d(const double *m)
{
    return m[2] == 0 && m[3] == 0 && m[6] == 0 && m[7] == 0 && m[8] == 0 && m[9] == 0 && m[10] == 1 && m[11] == 0 &&
           m[14] == 0 && m[15] == 1;
}

static embi_js_value dm_new(embi_bind_ctx *c, uint16_t iface, const double m[16], bool is2d)
{
    return new_mx6(c, iface, m, is2d);
}

static embi_js_value dm_ctor(embi_bind_ctx *c, embi_js_value nt, uint16_t iface, embi_js_value init)
{
    embi_js_value v;
    sv *p = embi_sv_new(c, iface, SV_MX, &v);
    embi_bind_pin(c, v);
    if (!embi_js_is_undefined(init)) {
        if (embi_js_is_object(init)) {
            uint32_t n = embi_bind_array_len(c, init);
            if (n != 6 && n != 16) {
                (void)embi_js_throw_type_error(embi_bind_runtime(c), "The matrix needs 6 or 16 numbers.");
                return EMBI_JS_EXCEPTION;
            }
            double vals[16];
            for (uint32_t i = 0; i < n; i++) {
                double d = 0;
                if (!embi_js_to_number(embi_bind_runtime(c), embi_bind_array_get(c, init, i), &d)) {
                    return EMBI_JS_EXCEPTION;
                }
                vals[i] = d;
            }
            if (n == 6) {
                mx_from6(p->u.mx.m, vals);
                p->u.mx.is2d = true;
            } else {
                SDL_memcpy(p->u.mx.m, vals, sizeof vals);
                p->u.mx.is2d = false;
            }
        } else {
            embi_dstr d;
            if (!embi_bind_to_dstr(c, init, &d)) {
                return EMBI_JS_EXCEPTION;
            }
            embi_str s = embi_bind_utf8(c, d);
            uint32_t q = 0;
            skip_ws(s, &q);
            double acc[16];
            mx_ident(acc);
            while (q < s.len) {
                sv t;
                SDL_memset(&t, 0, sizeof t);
                item_init(&t, SV_TF);
                if (!tf_parse_one(s, &q, &t)) {
                    (void)embi_impl_throw(c, "SyntaxError", "The transform list is not valid.");
                    return EMBI_JS_EXCEPTION;
                }
                double m[16];
                mx_from6(m, t.u.tf.m);
                mx_mul(acc, acc, m);
                skip_ws(s, &q);
                if (q < s.len && s.ptr[q] == ',') {
                    q++;
                    skip_ws(s, &q);
                }
            }
            SDL_memcpy(p->u.mx.m, acc, sizeof acc);
            p->u.mx.is2d = true;
        }
    }
    embi_bind_adopt_new_target(c, v, nt);
    return v;
}

embi_js_value embi_impl_DOMMatrix_constructor(embi_bind_ctx *c, embi_js_value nt, embi_js_value a0)
{
    return dm_ctor(c, nt, EMBI_IF_DOMMatrix, a0);
}

embi_js_value embi_impl_DOMMatrixReadOnly_constructor(embi_bind_ctx *c, embi_js_value nt, embi_js_value a0)
{
    return dm_ctor(c, nt, EMBI_IF_DOMMatrixReadOnly, a0);
}

#define DM_FIELD(NAME, IDX)                                                                    \
    double embi_impl_DOMMatrixReadOnly_get_##NAME(embi_bind_ctx *c, embi_bind_obj *self)        \
    {                                                                                          \
        sv *p = self->ptr;                                                                     \
        embi_sv_load(c, p);                                                                    \
        return p->u.mx.m[IDX];                                                                 \
    }                                                                                          \
    embi_dom_err embi_impl_DOMMatrix_get_##NAME(embi_bind_ctx *c, embi_bind_obj *self, double *out) \
    {                                                                                          \
        sv *p = self->ptr;                                                                     \
        embi_sv_load(c, p);                                                                    \
        *out = p->u.mx.m[IDX];                                                                 \
        return EMBI_DOM_OK;                                                                    \
    }                                                                                          \
    embi_dom_err embi_impl_DOMMatrix_set_##NAME(embi_bind_ctx *c, embi_bind_obj *self, double a0) \
    {                                                                                          \
        sv *p = self->ptr;                                                                     \
        embi_dom_err e = ro_check(c, p);                                                       \
        if (e) {                                                                               \
            return e;                                                                          \
        }                                                                                      \
        embi_sv_load(c, p);                                                                    \
        p->u.mx.m[IDX] = a0;                                                                   \
        if (!dm_all_default_3d(p->u.mx.m)) {                                                   \
            p->u.mx.is2d = false;                                                              \
        }                                                                                      \
        embi_sv_store(c, p);                                                                   \
        return EMBI_DOM_OK;                                                                    \
    }

DM_FIELD(a, 0)
DM_FIELD(b, 1)
DM_FIELD(c, 4)
DM_FIELD(d, 5)
DM_FIELD(e, 12)
DM_FIELD(f, 13)
DM_FIELD(m11, 0)
DM_FIELD(m12, 1)
DM_FIELD(m13, 2)
DM_FIELD(m14, 3)
DM_FIELD(m21, 4)
DM_FIELD(m22, 5)
DM_FIELD(m23, 6)
DM_FIELD(m24, 7)
DM_FIELD(m31, 8)
DM_FIELD(m32, 9)
DM_FIELD(m33, 10)
DM_FIELD(m34, 11)
DM_FIELD(m41, 12)
DM_FIELD(m42, 13)
DM_FIELD(m43, 14)
DM_FIELD(m44, 15)

bool embi_impl_DOMMatrixReadOnly_get_is2D(embi_bind_ctx *c, embi_bind_obj *self)
{
    sv *p = self->ptr;
    embi_sv_load(c, p);
    return p->u.mx.is2d;
}

bool embi_impl_DOMMatrixReadOnly_get_isIdentity(embi_bind_ctx *c, embi_bind_obj *self)
{
    sv *p = self->ptr;
    embi_sv_load(c, p);
    return mx_is_identity(p->u.mx.m);
}

/* The operations work on a copy of the matrix (the RO methods return it as a new DOMMatrix, the Self methods write it
 * back, to the attribute or list item it is bound to, and return the matrix itself). */
typedef struct dm_op {
    double m[16];
    bool is2d;
} dm_op;

static void dmo_translate(dm_op *o, double tx, double ty, double tz)
{
    mx_translate(o->m, o->m, tx, ty, tz);
    if (tz != 0) {
        o->is2d = false;
    }
}

static void dmo_scale(dm_op *o, double sx, double sy, double sz, double ox, double oy, double oz)
{
    if (sy != sy) {
        sy = sx;
    }
    mx_translate(o->m, o->m, ox, oy, oz);
    mx_scale3(o->m, o->m, sx, sy, sz);
    mx_translate(o->m, o->m, -ox, -oy, -oz);
    if (sz != 1 || oz != 0) {
        o->is2d = false;
    }
}

static void dmo_rotate(dm_op *o, double rx, double ry, double rz)
{
    if (ry != ry && rz != rz) {
        rz = rx;
        rx = 0;
        ry = 0;
    }
    if (ry != ry) {
        ry = 0;
    }
    if (rz != rz) {
        rz = 0;
    }
    if (rx != 0 || ry != 0) {
        o->is2d = false;
    }
    if (rz != 0) {
        mx_rot_z(o->m, o->m, rz);
    }
    if (ry != 0) {
        mx_rot_y(o->m, o->m, ry);
    }
    if (rx != 0) {
        mx_rot_x(o->m, o->m, rx);
    }
}

static void dmo_rotate_vec(dm_op *o, double x, double y)
{
    if (x == 0 && y == 0) {
        return;
    }
    mx_rot_z(o->m, o->m, SDL_atan2(y, x) * (180.0 / 3.14159265358979323846));
}

static void dmo_invert(dm_op *o)
{
    double r[16];
    if (mx_invert(o->m, r)) {
        SDL_memcpy(o->m, r, sizeof r);
    } else {
        for (int i = 0; i < 16; i++) {
            o->m[i] = (double)NAN;
        }
        o->is2d = false;
    }
}

static dm_op dmo_of(embi_bind_ctx *c, embi_bind_obj *self)
{
    sv *p = self->ptr;
    embi_sv_load(c, p);
    dm_op o;
    SDL_memcpy(o.m, p->u.mx.m, sizeof o.m);
    o.is2d = p->u.mx.is2d;
    return o;
}

static embi_js_value dmo_result(embi_bind_ctx *c, const dm_op *o)
{
    return dm_new(c, EMBI_IF_DOMMatrix, o->m, o->is2d);
}

/* The Self operations: refuse an animVal, then write the result back and return the matrix itself. */
static embi_dom_err dmo_self_begin(embi_bind_ctx *c, embi_bind_obj *self, dm_op *o)
{
    embi_dom_err e = ro_check(c, self->ptr);
    if (e == EMBI_DOM_OK) {
        *o = dmo_of(c, self);
    }
    return e;
}

static embi_dom_err dmo_self_end(embi_bind_ctx *c, embi_bind_obj *self, const dm_op *o, embi_js_value *out)
{
    sv *p = self->ptr;
    SDL_memcpy(p->u.mx.m, o->m, sizeof o->m);
    p->u.mx.is2d = o->is2d;
    embi_sv_store(c, p);
    *out = embi_bind_obj_value(self);
    return EMBI_DOM_OK;
}

#define DM_OP_PAIR(NAME, SELFNAME, PARAMS, BODY)                                                              \
    embi_js_value embi_impl_DOMMatrixReadOnly_##NAME(embi_bind_ctx *c, embi_bind_obj *self PARAMS)             \
    {                                                                                                         \
        dm_op o = dmo_of(c, self);                                                                            \
        BODY;                                                                                                 \
        return dmo_result(c, &o);                                                                             \
    }                                                                                                         \
    embi_dom_err embi_impl_DOMMatrix_##SELFNAME(embi_bind_ctx *c, embi_bind_obj *self PARAMS, embi_js_value *out) \
    {                                                                                                         \
        dm_op o;                                                                                              \
        embi_dom_err e = dmo_self_begin(c, self, &o);                                                         \
        if (e) {                                                                                              \
            return e;                                                                                         \
        }                                                                                                     \
        BODY;                                                                                                 \
        return dmo_self_end(c, self, &o, out);                                                                \
    }

DM_OP_PAIR(translate, translateSelf, P3, dmo_translate(&o, a0, a1, a2))
DM_OP_PAIR(scale, scaleSelf, P6, dmo_scale(&o, a0, a1, a2, a3, a4, a5))
DM_OP_PAIR(rotate, rotateSelf, P3, dmo_rotate(&o, a0, a1, a2))
DM_OP_PAIR(rotateFromVector, rotateFromVectorSelf, P2, dmo_rotate_vec(&o, a0, a1))
DM_OP_PAIR(skewX, skewXSelf, P1, mx_skew_x(o.m, o.m, a0))
DM_OP_PAIR(skewY, skewYSelf, P1, mx_skew_y(o.m, o.m, a0))

/* The legacy scaleNonUniform of SVGMatrix (and WebKit/Blink's DOMMatrixReadOnly). */
embi_js_value embi_impl_DOMMatrixReadOnly_scaleNonUniform(embi_bind_ctx *c, embi_bind_obj *self, double a0, double a1)
{
    dm_op o = dmo_of(c, self);
    dmo_scale(&o, a0, a1, 1, 0, 0, 0);
    return dmo_result(c, &o);
}

embi_js_value embi_impl_DOMMatrixReadOnly_flipX(embi_bind_ctx *c, embi_bind_obj *self)
{
    dm_op o = dmo_of(c, self);
    mx_scale3(o.m, o.m, -1, 1, 1);
    return dmo_result(c, &o);
}

embi_js_value embi_impl_DOMMatrixReadOnly_flipY(embi_bind_ctx *c, embi_bind_obj *self)
{
    dm_op o = dmo_of(c, self);
    mx_scale3(o.m, o.m, 1, -1, 1);
    return dmo_result(c, &o);
}

embi_js_value embi_impl_DOMMatrixReadOnly_inverse(embi_bind_ctx *c, embi_bind_obj *self)
{
    dm_op o = dmo_of(c, self);
    dmo_invert(&o);
    return dmo_result(c, &o);
}

embi_dom_err embi_impl_DOMMatrix_invertSelf(embi_bind_ctx *c, embi_bind_obj *self, embi_js_value *out)
{
    dm_op o;
    embi_dom_err e = dmo_self_begin(c, self, &o);
    if (e) {
        return e;
    }
    dmo_invert(&o);
    return dmo_self_end(c, self, &o, out);
}

embi_js_value embi_impl_DOMMatrixReadOnly_multiply(embi_bind_ctx *c, embi_bind_obj *self, embi_dict_DOMMatrix2DInit a0)
{
    double b[16];
    if (!embi_svg_matrix_from_init(c, &a0, b)) {
        return EMBI_JS_EXCEPTION;
    }
    dm_op o = dmo_of(c, self);
    mx_mul(o.m, o.m, b);
    return dmo_result(c, &o);
}

embi_dom_err embi_impl_DOMMatrix_multiplySelf(embi_bind_ctx *c, embi_bind_obj *self, embi_dict_DOMMatrix2DInit a0,
                                              embi_js_value *out)
{
    double b[16];
    dm_op o;
    embi_dom_err e = dmo_self_begin(c, self, &o);
    if (e) {
        return e;
    }
    if (!embi_svg_matrix_from_init(c, &a0, b)) {
        return EMBI_BIND_THREW;
    }
    mx_mul(o.m, o.m, b);
    return dmo_self_end(c, self, &o, out);
}

embi_dom_err embi_impl_DOMMatrix_preMultiplySelf(embi_bind_ctx *c, embi_bind_obj *self, embi_dict_DOMMatrix2DInit a0,
                                                 embi_js_value *out)
{
    double b[16];
    dm_op o;
    embi_dom_err e = dmo_self_begin(c, self, &o);
    if (e) {
        return e;
    }
    if (!embi_svg_matrix_from_init(c, &a0, b)) {
        return EMBI_BIND_THREW;
    }
    mx_mul(o.m, b, o.m);
    return dmo_self_end(c, self, &o, out);
}

embi_js_value embi_impl_DOMMatrixReadOnly_transformPoint(embi_bind_ctx *c, embi_bind_obj *self, embi_dict_DOMPointInit a0)
{
    sv *p = self->ptr;
    embi_sv_load(c, p);
    const double *m = p->u.mx.m;
    const double v[4] = {a0.x, a0.y, a0.z, a0.w};
    double r[4];
    for (int row = 0; row < 4; row++) {
        r[row] = m[row] * v[0] + m[4 + row] * v[1] + m[8 + row] * v[2] + m[12 + row] * v[3];
    }
    return embi_svg_dompoint_new(c, r[0], r[1], r[2], r[3]);
}

embi_js_value embi_impl_DOMMatrixReadOnly_toJSON(embi_bind_ctx *c, embi_bind_obj *self)
{
    sv *p = self->ptr;
    embi_sv_load(c, p);
    embi_js_value o = embi_bind_pin(c, embi_bind_object_new(c));
    static const char *const names6[6] = {"a", "b", "c", "d", "e", "f"};
    static const int idx6[6] = {0, 1, 4, 5, 12, 13};
    for (int i = 0; i < 6; i++) {
        embi_bind_object_set_c(c, o, names6[i], embi_js_number(p->u.mx.m[idx6[i]]));
    }
    for (int i = 0; i < 16; i++) {
        embi_bind_object_set_c(c, o, dm_names[i], embi_js_number(p->u.mx.m[i]));
    }
    embi_bind_object_set_c(c, o, "is2D", embi_js_from_bool(p->u.mx.is2d));
    embi_bind_object_set_c(c, o, "isIdentity", embi_js_from_bool(mx_is_identity(p->u.mx.m)));
    return o;
}
