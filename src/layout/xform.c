/*
 * Box transforms as 4x4 matrices (R3c, see xform.h). The composition rules are CSS Transforms 2: the individual
 * properties and the transform list multiply in the order translate, rotate, scale, transform, and the result acts
 * about transform-origin. The perspective property gives the children of its box the matrix
 * T(perspective-origin) * P(d) * T(-perspective-origin).
 */
#include "layout/xform.h"

#include "css/props.h"

#include <math.h>
#include <string.h>

static const double k_pi = 3.14159265358979323846;

void embi_m4_identity(embi_m4 *a)
{
    memset(a, 0, sizeof *a);
    a->m[0] = a->m[5] = a->m[10] = a->m[15] = 1.0;
}

void embi_m4_mul(const embi_m4 *a, const embi_m4 *b, embi_m4 *out)
{
    embi_m4 r;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            double s = 0.0;
            for (int k = 0; k < 4; k++) {
                s += a->m[i * 4 + k] * b->m[k * 4 + j];
            }
            r.m[i * 4 + j] = s;
        }
    }
    *out = r;
}

void embi_m4_point(const embi_m4 *a, double x, double y, double z, double out[4])
{
    for (int i = 0; i < 4; i++) {
        out[i] = a->m[i * 4] * x + a->m[i * 4 + 1] * y + a->m[i * 4 + 2] * z + a->m[i * 4 + 3];
    }
}

bool embi_m4_invertible(const embi_m4 *a)
{
    const double *m = a->m;
    /* The determinant by cofactors of the 2x2 minors of the first two and last two rows. */
    double s0 = m[0] * m[5] - m[1] * m[4], s1 = m[0] * m[6] - m[2] * m[4], s2 = m[0] * m[7] - m[3] * m[4];
    double s3 = m[1] * m[6] - m[2] * m[5], s4 = m[1] * m[7] - m[3] * m[5], s5 = m[2] * m[7] - m[3] * m[6];
    double c5 = m[10] * m[15] - m[11] * m[14], c4 = m[9] * m[15] - m[11] * m[13], c3 = m[9] * m[14] - m[10] * m[13];
    double c2 = m[8] * m[15] - m[11] * m[12], c1 = m[8] * m[14] - m[10] * m[12], c0 = m[8] * m[13] - m[9] * m[12];
    double det = s0 * c5 - s1 * c4 + s2 * c3 + s3 * c2 - s4 * c1 + s5 * c0;
    /* The scale is that of the linear part: a translation says nothing about whether the map collapses. */
    double scale = 0.0;
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            scale = fmax(scale, fabs(m[r * 4 + c]));
        }
    }
    return isfinite(det) && scale > 0.0 && fabs(det) > 1e-9 * scale * scale * scale;
}

void embi_m4_plane(const embi_m4 *a, double h[9])
{
    static const int rows[3] = {0, 1, 3};
    for (int i = 0; i < 3; i++) {
        h[i * 3] = a->m[rows[i] * 4];
        h[i * 3 + 1] = a->m[rows[i] * 4 + 1];
        h[i * 3 + 2] = a->m[rows[i] * 4 + 3];
    }
}

double embi_h3_det(const double h[9])
{
    return h[0] * (h[4] * h[8] - h[5] * h[7]) - h[1] * (h[3] * h[8] - h[5] * h[6]) +
           h[2] * (h[3] * h[7] - h[4] * h[6]);
}

bool embi_h3_invert(const double h[9], double out[9])
{
    double det = embi_h3_det(h);
    if (!(fabs(det) > 1e-30) || !isfinite(det)) {
        return false;
    }
    double r[9];
    r[0] = (h[4] * h[8] - h[5] * h[7]) / det;
    r[1] = (h[2] * h[7] - h[1] * h[8]) / det;
    r[2] = (h[1] * h[5] - h[2] * h[4]) / det;
    r[3] = (h[5] * h[6] - h[3] * h[8]) / det;
    r[4] = (h[0] * h[8] - h[2] * h[6]) / det;
    r[5] = (h[2] * h[3] - h[0] * h[5]) / det;
    r[6] = (h[3] * h[7] - h[4] * h[6]) / det;
    r[7] = (h[1] * h[6] - h[0] * h[7]) / det;
    r[8] = (h[0] * h[4] - h[1] * h[3]) / det;
    memcpy(out, r, sizeof r);
    return true;
}

void embi_h3_mul(const double a[9], const double b[9], double out[9])
{
    double r[9];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
        }
    }
    memcpy(out, r, sizeof r);
}

/* A rotation of deg degrees about the axis (x, y, z) (CSS Transforms 2, the quaternion form). */
static void m4_rotate(double x, double y, double z, double deg, embi_m4 *out)
{
    embi_m4_identity(out);
    double len = sqrt(x * x + y * y + z * z);
    if (!(len > 0.0)) {
        return;
    }
    x /= len;
    y /= len;
    z /= len;
    double t = deg * k_pi / 180.0;
    double s = sin(t / 2.0), c = cos(t / 2.0);
    double qx = x * s, qy = y * s, qz = z * s, qw = c;
    out->m[0] = 1.0 - 2.0 * (qy * qy + qz * qz);
    out->m[1] = 2.0 * (qx * qy - qz * qw);
    out->m[2] = 2.0 * (qx * qz + qy * qw);
    out->m[4] = 2.0 * (qx * qy + qz * qw);
    out->m[5] = 1.0 - 2.0 * (qx * qx + qz * qz);
    out->m[6] = 2.0 * (qy * qz - qx * qw);
    out->m[8] = 2.0 * (qx * qz - qy * qw);
    out->m[9] = 2.0 * (qy * qz + qx * qw);
    out->m[10] = 1.0 - 2.0 * (qx * qx + qy * qy);
}

/* ------------------------------------------------------------------ lengths */

typedef struct lenenv {
    float vw, vh;
    float fs;
    float rem;   /* the root element's font size */
} lenenv;

static double unit_px(void *user, uint8_t unit, double v)
{
    const lenenv *e = user;
    double vw = e->vw, vh = e->vh;
    switch (unit) {
    case EMBI_U_PX:
        return v;
    case EMBI_U_EM:
        return v * e->fs;
    case EMBI_U_REM:
        return v * e->rem;
    case EMBI_U_EX:
    case EMBI_U_CH:
        return v * e->fs * 0.5;
    case EMBI_U_CAP:
        return v * e->fs * 0.7;
    case EMBI_U_VW:
    case EMBI_U_SVW:
    case EMBI_U_LVW:
    case EMBI_U_DVW:
        return v * vw / 100.0;
    case EMBI_U_VH:
    case EMBI_U_SVH:
    case EMBI_U_LVH:
    case EMBI_U_DVH:
        return v * vh / 100.0;
    case EMBI_U_VMIN:
    case EMBI_U_SVMIN:
    case EMBI_U_LVMIN:
    case EMBI_U_DVMIN:
        return v * (vw < vh ? vw : vh) / 100.0;
    case EMBI_U_VMAX:
    case EMBI_U_SVMAX:
    case EMBI_U_LVMAX:
    case EMBI_U_DVMAX:
        return v * (vw > vh ? vw : vh) / 100.0;
    default:
        return 0.0;
    }
}

/* A length, percentage or calc() value in px against the 100% base ref. ok is false for other kinds. */
static double len_px(const lenenv *e, const embi_value *v, double ref, bool *ok)
{
    *ok = true;
    switch (v->kind) {
    case EMBI_V_LENGTH:
        return unit_px((void *)(uintptr_t)e, v->unit, v->num);
    case EMBI_V_PERCENT:
        return ref * v->num / 100.0;
    case EMBI_V_CALC: {
        embi_calc_env ce = {ref, unit_px, (void *)(uintptr_t)e};
        double r = embi_calc_eval(v->calc, &ce);
        if (isnan(r)) {
            *ok = false;
            return 0.0;
        }
        return r;
    }
    default:
        *ok = false;
        return 0.0;
    }
}

double embi_lay_len_px(const embi_value *v, double ref, float fs, float vw, float vh, float rem, bool *ok)
{
    lenenv e = {vw, vh, fs, rem > 0.0f ? rem : 16.0f};
    return len_px(&e, v, ref, ok);
}

static float font_size_px(const embi_style *s)
{
    const embi_value *v = s != nullptr ? embi_style_value(s, EMBI_PROP_font_size) : nullptr;
    if (v != nullptr && v->kind == EMBI_V_LENGTH && v->unit == EMBI_U_PX) {
        return (float)v->num;
    }
    return 16.0f;
}

/* The axis offset of a transform origin or a perspective origin: anchor 1 measures from the far edge. */
static double axis_px(const lenenv *e, const embi_pos_axis *ax, double size)
{
    bool ok = false;
    double off = len_px(e, &ax->off, size, &ok);
    if (!ok) {
        return 0.0;
    }
    return ax->anchor != 0 ? size - off : off;
}

/* ------------------------------------------------------------------ the transform functions */

/* The 4x4 of one transform function with the box's border box bb (for percentages). */
static void xform_fn(const lenenv *e, const embi_xform *x, const float bb[4], embi_m4 *f)
{
    const embi_value *a = x->args;
    uint32_t n = x->n;
    bool ok = false;
    embi_m4_identity(f);
    switch (x->fn) {
    case EMBI_XF_MATRIX:
        if (n >= 6) {
            /* matrix(a, b, c, d, e, f): x' = a x + c y + e, y' = b x + d y + f */
            f->m[0] = a[0].num;
            f->m[4] = a[1].num;
            f->m[1] = a[2].num;
            f->m[5] = a[3].num;
            f->m[3] = a[4].num;
            f->m[7] = a[5].num;
        }
        break;
    case EMBI_XF_MATRIX3D:
        if (n >= 16) {
            /* column-major in CSS: element (row r, column c) is the (c * 4 + r)-th argument */
            for (int r = 0; r < 4; r++) {
                for (int c = 0; c < 4; c++) {
                    f->m[r * 4 + c] = a[c * 4 + r].num;
                }
            }
        }
        break;
    case EMBI_XF_TRANSLATE:
    case EMBI_XF_TRANSLATE3D:
        f->m[3] = n >= 1 ? len_px(e, &a[0], bb[2], &ok) : 0.0;
        f->m[7] = n >= 2 ? len_px(e, &a[1], bb[3], &ok) : 0.0;
        f->m[11] = n >= 3 ? len_px(e, &a[2], 0.0, &ok) : 0.0;
        break;
    case EMBI_XF_TRANSLATEX:
        f->m[3] = len_px(e, &a[0], bb[2], &ok);
        break;
    case EMBI_XF_TRANSLATEY:
        f->m[7] = len_px(e, &a[0], bb[3], &ok);
        break;
    case EMBI_XF_TRANSLATEZ:
        f->m[11] = len_px(e, &a[0], 0.0, &ok);
        break;
    case EMBI_XF_SCALE:
    case EMBI_XF_SCALE3D: {
        double sx = a[0].num, sy = n >= 2 ? a[1].num : sx;
        double sz = (x->fn == EMBI_XF_SCALE3D && n >= 3) ? a[2].num : 1.0;
        f->m[0] = sx;
        f->m[5] = sy;
        f->m[10] = sz;
        break;
    }
    case EMBI_XF_SCALEX:
        f->m[0] = a[0].num;
        break;
    case EMBI_XF_SCALEY:
        f->m[5] = a[0].num;
        break;
    case EMBI_XF_SCALEZ:
        f->m[10] = a[0].num;
        break;
    case EMBI_XF_ROTATE:
    case EMBI_XF_ROTATEZ:
        m4_rotate(0.0, 0.0, 1.0, a[0].num, f);
        break;
    case EMBI_XF_ROTATEX:
        m4_rotate(1.0, 0.0, 0.0, a[0].num, f);
        break;
    case EMBI_XF_ROTATEY:
        m4_rotate(0.0, 1.0, 0.0, a[0].num, f);
        break;
    case EMBI_XF_ROTATE3D:
        if (n >= 4) {
            m4_rotate(a[0].num, a[1].num, a[2].num, a[3].num, f);
        }
        break;
    case EMBI_XF_SKEW:
        f->m[1] = tan(a[0].num * k_pi / 180.0);
        f->m[4] = n >= 2 ? tan(a[1].num * k_pi / 180.0) : 0.0;
        break;
    case EMBI_XF_SKEWX:
        f->m[1] = tan(a[0].num * k_pi / 180.0);
        break;
    case EMBI_XF_SKEWY:
        f->m[4] = tan(a[0].num * k_pi / 180.0);
        break;
    case EMBI_XF_PERSPECTIVE:
        /* perspective(d): -1/d in row 3, column 2. A keyword (none) is the identity; a length below 1px is clamped to
         * 1px (CSS Transforms 2, perspective()). */
        if (n >= 1 && a[0].kind != EMBI_V_KEYWORD) {
            double d = len_px(e, &a[0], 0.0, &ok);
            if (ok) {
                f->m[14] = -1.0 / (d < 1.0 ? 1.0 : d);
            }
        }
        break;
    default:
        break;
    }
}

static bool xform_active(const embi_value *v)
{
    return v != nullptr && !(v->kind == EMBI_V_KEYWORD && v->kw == EMBI_KW_none) && v->kind != EMBI_V_NONE;
}

/* Multiplies m by the functions of one transform list (applied in order, the last one first to a point). */
static void apply_list(const lenenv *e, const embi_value *v, const float bb[4], embi_m4 *m)
{
    if (v == nullptr || v->kind != EMBI_V_TRANSFORM) {
        return;
    }
    for (uint32_t i = 0; i < v->xf.n; i++) {
        embi_m4 f;
        xform_fn(e, &v->xf.p[i], bb, &f);
        embi_m4_mul(m, &f, m);
    }
}

bool embi_lay_has_transform(const embi_style *s)
{
    if (s == nullptr) {
        return false;
    }
    return xform_active(embi_style_value(s, EMBI_PROP_transform)) ||
           xform_active(embi_style_value(s, EMBI_PROP_translate)) ||
           xform_active(embi_style_value(s, EMBI_PROP_rotate)) ||
           xform_active(embi_style_value(s, EMBI_PROP_scale));
}

/* Writes T(ox, oy, oz) * m * T(-ox, -oy, -oz) to m. */
static void about_origin(embi_m4 *m, double ox, double oy, double oz)
{
    embi_m4 to, tb;
    embi_m4_identity(&to);
    embi_m4_identity(&tb);
    to.m[3] = ox;
    to.m[7] = oy;
    to.m[11] = oz;
    tb.m[3] = -ox;
    tb.m[7] = -oy;
    tb.m[11] = -oz;
    embi_m4_mul(&to, m, m);
    embi_m4_mul(m, &tb, m);
}

bool embi_lay_box_matrix(const embi_style *s, const float bb[4], float vw, float vh, float rem, embi_m4 *out)
{
    embi_m4_identity(out);
    if (!embi_lay_has_transform(s)) {
        return false;
    }
    lenenv e = {vw, vh, font_size_px(s), rem > 0.0f ? rem : 16.0f};
    /* CSS Transforms 2: translate, then rotate, then scale, then transform. */
    apply_list(&e, embi_style_value(s, EMBI_PROP_translate), bb, out);
    apply_list(&e, embi_style_value(s, EMBI_PROP_rotate), bb, out);
    apply_list(&e, embi_style_value(s, EMBI_PROP_scale), bb, out);
    apply_list(&e, embi_style_value(s, EMBI_PROP_transform), bb, out);

    double ox = bb[0] + bb[2] * 0.5, oy = bb[1] + bb[3] * 0.5, oz = 0.0;
    const embi_value *o = embi_style_value(s, EMBI_PROP_transform_origin);
    if (o != nullptr && o->kind == EMBI_V_POSITION && o->pos != nullptr) {
        ox = bb[0] + axis_px(&e, &o->pos->x, bb[2]);
        oy = bb[1] + axis_px(&e, &o->pos->y, bb[3]);
        if (o->pos->z.kind == EMBI_V_LENGTH) {
            bool ok = false;
            oz = len_px(&e, &o->pos->z, 0.0, &ok);
            if (!ok) {
                oz = 0.0;
            }
        }
    }
    about_origin(out, ox, oy, oz);
    return true;
}

bool embi_lay_perspective_matrix(const embi_style *s, const float bb[4], float vw, float vh, float rem, embi_m4 *out)
{
    embi_m4_identity(out);
    const embi_value *pv = s != nullptr ? embi_style_value(s, EMBI_PROP_perspective) : nullptr;
    if (pv == nullptr || pv->kind == EMBI_V_NONE || pv->kind == EMBI_V_KEYWORD) {
        return false;
    }
    lenenv e = {vw, vh, font_size_px(s), rem > 0.0f ? rem : 16.0f};
    bool ok = false;
    double d = len_px(&e, pv, 0.0, &ok);
    if (!ok) {
        return false;
    }
    out->m[14] = -1.0 / (d < 1.0 ? 1.0 : d);
    double ox = bb[0] + bb[2] * 0.5, oy = bb[1] + bb[3] * 0.5;
    const embi_value *o = embi_style_value(s, EMBI_PROP_perspective_origin);
    if (o != nullptr && o->kind == EMBI_V_POSITION && o->pos != nullptr) {
        ox = bb[0] + axis_px(&e, &o->pos->x, bb[2]);
        oy = bb[1] + axis_px(&e, &o->pos->y, bb[3]);
    }
    about_origin(out, ox, oy, 0.0);
    return true;
}

float embi_lay_root_font_px(const embi_frag *root)
{
    for (const embi_frag *k = root != nullptr ? embi_frag_first(root) : nullptr; k != nullptr; k = k->next) {
        if (k->kind == EMBI_FK_BOX && k->box != nullptr && k->box->el != nullptr) {
            return font_size_px(k->box->style);
        }
    }
    return root != nullptr && root->box != nullptr ? font_size_px(root->box->style) : 16.0f;
}

const struct embi_element *embi_lay_parent_el(const embi_box *b)
{
    if (b == nullptr || b->el == nullptr) {
        return nullptr;
    }
    if (b->pe != 0) {
        return b->el;
    }
    const embi_node *par = b->el->base.parent;
    return par != nullptr && par->type == EMBI_NODE_ELEMENT ? (const struct embi_element *)par : nullptr;
}

static uint16_t kw_of(const embi_style *s, embi_prop_id p)
{
    const embi_value *v = s != nullptr ? embi_style_value(s, p) : nullptr;
    return v != nullptr && v->kind == EMBI_V_KEYWORD ? v->kw : 0;
}

bool embi_lay_preserve3d_style(const embi_style *s)
{
    return kw_of(s, EMBI_PROP_transform_style) == EMBI_KW_preserve_3d;
}

bool embi_lay_backface_hidden(const embi_style *s)
{
    return kw_of(s, EMBI_PROP_backface_visibility) == EMBI_KW_hidden;
}

static bool not_none(const embi_style *s, embi_prop_id p)
{
    const embi_value *v = embi_style_value(s, p);
    return v != nullptr && v->kind != EMBI_V_NONE && !(v->kind == EMBI_V_KEYWORD && v->kw == EMBI_KW_none);
}

bool embi_lay_preserve3d_used(const embi_style *s)
{
    if (!embi_lay_preserve3d_style(s)) {
        return false;
    }
    /* Grouping properties force the used value flat (CSS Transforms 2, "grouping property values"). */
    uint16_t ox = kw_of(s, EMBI_PROP_overflow_x), oy = kw_of(s, EMBI_PROP_overflow_y);
    if ((ox != 0 && ox != EMBI_KW_visible && ox != EMBI_KW_clip) || (oy != 0 && oy != EMBI_KW_visible && oy != EMBI_KW_clip)) {
        return false;
    }
    const embi_value *op = embi_style_value(s, EMBI_PROP_opacity);
    if (op != nullptr && op->kind == EMBI_V_NUMBER && op->num < 1.0) {
        return false;
    }
    if (not_none(s, EMBI_PROP_filter) || not_none(s, EMBI_PROP_clip_path) || not_none(s, EMBI_PROP_mask_image)) {
        return false;
    }
    if (kw_of(s, EMBI_PROP_isolation) == EMBI_KW_isolate) {
        return false;
    }
    uint16_t mb = kw_of(s, EMBI_PROP_mix_blend_mode);
    if (mb != 0 && mb != EMBI_KW_normal) {
        return false;
    }
    return true;
}
