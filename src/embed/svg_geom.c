/*
 * The SVG DOM geometry of a document (R4c): getBBox, getCTM, getScreenCTM, getTotalLength, getPointAtLength, isPointInFill
 * and isPointInStroke, answered by the SVG module and installed on the bind context of the document's script realm.
 */
#include "embed/doc.h"

#if EMBER_SVG
#include "bindings/svg_geometry.h"
#include "layout/layout.h"
#include "svg/svg.h"

static const embi_element *outer_svg(const embi_element *el)
{
    const embi_element *o = nullptr;
    for (const embi_node *n = &el->base; n != nullptr && embi_is_element(n); n = n->parent) {
        if (n->extra == EMBI_NS_SVG) {
            o = (const embi_element *)n;
        } else {
            break;
        }
    }
    return o;
}

static bool setup(embi_doc *d, const embi_node *node, embi_svg_env *env, embi_svg_geom *g, const embi_element **el, float *ox, float *oy)
{
    if (node == nullptr || !embi_is_element(node) || node->extra != EMBI_NS_SVG) {
        return false;
    }
    *el = (const embi_element *)node;
    memset(env, 0, sizeof *env);
    env->se = embi_doc_style_engine(d);
    env->text = embi_layout_text(embi_doc_layout_engine(d));
    env->dev_scale = 1.0f;
    g->env = env;
    g->vw = g->vh = 100.0f;
    *ox = *oy = 0.0f;
    const embi_element *o = outer_svg(*el);
    embi_rectf r;
    if (o != nullptr && embi_layout_border_box(embi_doc_layout_engine(d), o, &r)) {
        g->vw = r.w;
        g->vh = r.h;
        *ox = r.x;
        *oy = r.y;
    }
    env->vw = g->vw;
    env->vh = g->vh;
    return true;
}

#define SETUP                                                                                                      \
    embi_svg_env env;                                                                                              \
    embi_svg_geom g;                                                                                               \
    const embi_element *e;                                                                                         \
    float ox, oy;                                                                                                  \
    if (!setup(user, node, &env, &g, &e, &ox, &oy)) {                                                              \
        return false;                                                                                              \
    }                                                                                                              \
    (void)ox;                                                                                                      \
    (void)oy;

static bool g_bbox(void *user, const embi_node *node, double out[4])
{
    SETUP
    return embi_svg_bbox(&g, e, out);
}

static bool g_ctm(void *user, const embi_node *node, bool screen, double out[6])
{
    SETUP
    if (!embi_svg_ctm(&g, e, screen, out)) {
        return false;
    }
    if (screen) {
        out[4] += ox;
        out[5] += oy;
    }
    return true;
}

static bool g_len(void *user, const embi_node *node, double *out)
{
    SETUP
    return embi_svg_total_length(&g, e, out);
}

static bool g_at(void *user, const embi_node *node, double dist, double out[2])
{
    SETUP
    return embi_svg_point_at_length(&g, e, dist, out);
}

static bool g_fill(void *user, const embi_node *node, double x, double y)
{
    SETUP
    return embi_svg_point_in_fill(&g, e, x, y);
}

static bool g_stroke(void *user, const embi_node *node, double x, double y)
{
    SETUP
    return embi_svg_point_in_stroke(&g, e, x, y);
}

static const embi_svg_geometry_ops k_ops = {g_bbox, g_ctm, g_len, g_at, g_fill, g_stroke};

void embi_doc_install_svg_geometry(embi_bind_ctx *c, embi_doc *d)
{
    embi_svg_geometry_install(c, &k_ops, d);
}
#else
typedef int embi_svg_unit_empty; /* EMBER_SVG=OFF: nothing here (SZ3b) */
#endif
