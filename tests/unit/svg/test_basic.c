/*
 * The SVG module (R4c): path data parsing, shape geometry, the display list of inline svg, paint servers, use, clip and
 * mask groups, hit testing and SVG documents as images.
 */
#include "unit/paint/paint_harness.h"

#include "embed/svgcache.h"
#include "layout/xform.h"
#include "svg/svg_int.h"

#include <SDL3/SDL_timer.h>
#include <stdio.h>

static bool near(double a, double b)
{
    return fabs(a - b) < 1e-3;
}

/* ------------------------------------------------------------------ path data */

static void path_box(const char *d, bool *err, float box[4], bool *ok)
{
    embi_path *p = svg_parse_path(d, (uint32_t)strlen(d), 0.0f, nullptr, err);
    *ok = svgp_bbox(p, box);
    svgp_free(p);
}

EMB_TEST(path_data_basic)
{
    bool err, ok;
    float b[4];
    path_box("M10 10 L30 20 z", &err, b, &ok);
    EMB_CHECK(ok && !err && near(b[0], 10) && near(b[1], 10) && near(b[2], 20) && near(b[3], 10));
    path_box("m10,10 l20,10 h-5 v5", &err, b, &ok);
    EMB_CHECK(ok && !err && near(b[0], 10) && near(b[2], 20) && near(b[3], 15));
    /* numbers: exponents, signs as separators, leading dots */
    path_box("M0 0L1e1-5.5.5-3", &err, b, &ok);
    EMB_CHECK(ok && !err && near(b[2], 10) && near(b[1], -5.5));
    /* implicit lineto after a moveto */
    path_box("M0 0 10 0 10 10", &err, b, &ok);
    EMB_CHECK(ok && !err && near(b[2], 10) && near(b[3], 10));
    /* arc flags without separators */
    path_box("M0 0 a5 5 0 1010 0", &err, b, &ok);
    EMB_CHECK(ok && !err && near(b[2], 10));
    /* quadratic and cubic */
    path_box("M0 0 Q 5 10 10 0", &err, b, &ok);
    EMB_CHECK(ok && !err && near(b[3], 5)); /* the extremum of the quadratic is half the control height */
    path_box("M0 0 C 0 10 10 10 10 0 S 20 -10 20 0", &err, b, &ok);
    EMB_CHECK(ok && !err && b[2] >= 19.99f);
}

EMB_TEST(path_data_errors)
{
    bool err, ok;
    float b[4];
    /* rendered up to the error */
    path_box("M10 10 L20 20 L x 5", &err, b, &ok);
    EMB_CHECK(ok && err && near(b[2], 10));
    /* a path must start with a moveto */
    path_box("L10 10", &err, b, &ok);
    EMB_CHECK(err && !ok);
    /* unknown command */
    path_box("M0 0 L5 5 X 3 3", &err, b, &ok);
    EMB_CHECK(err && ok && near(b[2], 5));
    /* too few numbers */
    path_box("M0 0 L5 5 L 7", &err, b, &ok);
    EMB_CHECK(err && ok && near(b[2], 5));
    /* empty */
    path_box("", &err, b, &ok);
    EMB_CHECK(!ok);
}

EMB_TEST(transform_and_length)
{
    float m[6];
    EMB_CHECK(svg_parse_transform("translate(10 20) scale(2)", 25, m));
    EMB_CHECK(near(m[0], 2) && near(m[3], 2) && near(m[4], 10) && near(m[5], 20));
    EMB_CHECK(svg_parse_transform("rotate(90)", 10, m));
    EMB_CHECK(near(m[0], 0) && near(m[1], 1) && near(m[2], -1));
    EMB_CHECK(!svg_parse_transform("scale(", 6, m));
    EMB_CHECK(!svg_parse_transform("foo(1)", 6, m));
    svg_len l;
    EMB_CHECK(svg_parse_length("12.5mm", 6, &l) && l.unit == SVGL_MM);
    EMB_CHECK(near(svg_len_px(l, 16, 0), 12.5 * 96 / 25.4));
    EMB_CHECK(svg_parse_length("50%", 3, &l) && near(svg_len_px(l, 16, 200), 100));
    EMB_CHECK(!svg_parse_length("5foo", 4, &l));
    svg_par par = svg_parse_par("xMinYMax slice", 14);
    EMB_CHECK(par.align == 7 && par.slice);
    par = svg_parse_par("none", 4);
    EMB_CHECK(par.align == 0);
    float vb[4] = {0, 0, 10, 5};
    svg_viewbox_matrix(vb, 100, 100, svg_parse_par("xMidYMid meet", 13), m);
    EMB_CHECK(near(m[0], 10) && near(m[5], 25));
}

EMB_TEST(path_geometry_queries)
{
    bool err;
    embi_path *p = svg_parse_path("M0 0 H100 V50 H0 z", 18, 0.0f, nullptr, &err);
    EMB_CHECK(!err);
    EMB_CHECK(near(svgp_length(p), 300));
    double pt[2];
    EMB_CHECK(svgp_point_at(p, 150, pt, nullptr) && near(pt[0], 100) && near(pt[1], 50));
    EMB_CHECK(svgp_contains(p, 0, 50, 25));
    EMB_CHECK(!svgp_contains(p, 0, 150, 25));
    svgp_free(p);
    /* evenodd versus nonzero for a star */
    p = svg_parse_path("M50 0 L80 100 L0 40 L100 40 L20 100 z", 38, 0.0f, nullptr, &err);
    EMB_CHECK(svgp_contains(p, 0, 50, 50));
    EMB_CHECK(!svgp_contains(p, 1, 50, 50));
    svgp_free(p);
}

/* ------------------------------------------------------------------ display list of inline svg */

static const embi_ditem *path_item(const embi_dl *d, int nth)
{
    int k = -1;
    for (uint32_t i = 0; i < d->items.len; i++) {
        if (d->items.data[i].kind == EMBI_DI_PATH && ++k == nth) {
            return &d->items.data[i];
        }
    }
    return nullptr;
}

EMB_TEST(inline_rect_fill)
{
    ph h = ph_make("<svg width=100 height=50 viewBox=\"0 0 10 5\"><rect width=10 height=5 fill=\"#f00\"/></svg>", "");
    EMB_CHECK(ph_count(&h.dl, EMBI_DI_PATH) == 1);
    const embi_ditem *it = path_item(&h.dl, 0);
    EMB_REQUIRE(it != nullptr);
    EMB_CHECK(it->colors[0] == 0xFF0000FFu);
    EMB_CHECK(near(it->rect[2], 10) && near(it->rect[3], 5));
    /* the item's matrix scales the viewBox to the box */
    const float *m = h.dl.xforms.data[it->xform].m;
    EMB_CHECK(near(m[0], 10) && near(m[3], 10));
    EMB_CHECK(h.dl.paths.len == 1);
    ph_free(&h);
}

EMB_TEST(fill_stroke_properties)
{
    ph h = ph_make("<svg width=100 height=100><circle cx=50 cy=50 r=30 fill=none stroke=blue stroke-width=4 stroke-opacity=.5 />"
                   "<path d=\"M0 0 L10 10\" stroke=green stroke-dasharray=\"2 2\" fill=red /></svg>",
                   "");
    const embi_ditem *a = path_item(&h.dl, 0);
    EMB_REQUIRE(a != nullptr);
    EMB_CHECK((a->colors[0] >> 24) == 128 || (a->colors[0] >> 24) == 127);
    EMB_CHECK((a->colors[0] & 0xFFFFFF) == 0xFF0000u);
    EMB_CHECK((a->flags & EMBI_DF_STROKE) != 0 && near(a->f[0], 4) && a->rect[2] >= 64); /* a kept center line, bounds grown by the stroke */
    ph_free(&h);
}

EMB_TEST(presentation_attributes_and_css)
{
    ph h = ph_make("<svg width=20 height=20><rect id=r width=20 height=20 fill=red /></svg>", "#r{fill:#00f}");
    const embi_ditem *it = path_item(&h.dl, 0);
    EMB_REQUIRE(it != nullptr);
    EMB_CHECK(it->colors[0] == 0xFFFF0000u); /* css beats the attribute */
    ph_free(&h);
    h = ph_make("<svg width=20 height=20 style=\"color:#0f0\"><rect width=20 height=20 fill=currentColor /></svg>", "");
    it = path_item(&h.dl, 0);
    EMB_REQUIRE(it != nullptr);
    EMB_CHECK(it->colors[0] == 0xFF00FF00u);
    ph_free(&h);
}

EMB_TEST(gradients)
{
    ph h = ph_make("<svg width=100 height=100><defs><linearGradient id=g x1=0 x2=1><stop offset=0 stop-color=red />"
                   "<stop offset=1 stop-color=blue /></linearGradient>"
                   "<radialGradient id=q href=\"#g\" spreadMethod=reflect /></defs>"
                   "<rect width=100 height=100 fill=\"url(#g)\" />"
                   "<circle cx=50 cy=50 r=40 fill=\"url(#q)\" /></svg>",
                   "");
    const embi_ditem *a = path_item(&h.dl, 0), *b = path_item(&h.dl, 1);
    EMB_REQUIRE(a != nullptr && b != nullptr);
    EMB_CHECK(a->styles[0] == EMBI_GRAD_LINEAR_DL && a->aux3 >= 2);
    EMB_CHECK(b->styles[0] == EMBI_GRAD_RADIAL_DL && (b->flags & EMBI_DF_REPEAT) != 0 && b->aux3 >= 4);
    EMB_REQUIRE(a->aux3 >= 2);
    EMB_CHECK(h.dl.stops.data[a->aux2].color == 0xFF0000FFu);
    ph_free(&h);
}

EMB_TEST(use_symbol_and_opacity)
{
    ph h = ph_make("<svg width=100 height=100><defs><rect id=r width=10 height=10 fill=red />"
                   "<symbol id=s viewBox=\"0 0 10 10\"><circle cx=5 cy=5 r=5 fill=blue /></symbol></defs>"
                   "<use href=\"#r\" x=20 y=30 opacity=.5 />"
                   "<use href=\"#s\" x=50 y=50 width=20 height=20 /></svg>",
                   "");
    EMB_CHECK(ph_count(&h.dl, EMBI_DI_PATH) == 2);
    EMB_CHECK(ph_count(&h.dl, EMBI_DI_LAYER_BEGIN) == 1);
    const embi_ditem *a = path_item(&h.dl, 0);
    EMB_REQUIRE(a != nullptr);
    const float *m = h.dl.xforms.data[a->xform].m;
    EMB_CHECK(near(m[4], 20) && near(m[5], 30));
    ph_free(&h);
    /* a cycle terminates */
    h = ph_make("<svg width=10 height=10><g id=a><use href=\"#a\" /><rect width=5 height=5 /></g></svg>", "");
    EMB_CHECK(ph_count(&h.dl, EMBI_DI_PATH) == 1);
    ph_free(&h);
}

EMB_TEST(clip_path_and_mask)
{
    ph h = ph_make("<svg width=100 height=100><defs><clipPath id=c><rect width=50 height=50 /></clipPath>"
                   "<mask id=m><rect width=100 height=100 fill=white /></mask></defs>"
                   "<rect width=100 height=100 fill=red clip-path=\"url(#c)\" />"
                   "<rect width=100 height=100 fill=red mask=\"url(#m)\" /></svg>",
                   "");
    EMB_CHECK(ph_count(&h.dl, EMBI_DI_PATH) == 4);
    uint32_t iso = 0, mask = 0;
    for (uint32_t i = 0; i < h.dl.items.len; i++) {
        const embi_ditem *it = &h.dl.items.data[i];
        if (it->kind == EMBI_DI_LAYER_BEGIN) {
            iso += it->sub == EMBI_LS_ISO;
            mask += it->sub == EMBI_LS_SVGMASK;
        }
    }
    EMB_CHECK(iso == 2 && mask == 2);
    ph_free(&h);
}

/* A 24x24 icon (a heart with a stroke, as icon sets draw them) costs less than 2 KB beyond the DOM: the display list items,
 * the path and the transform and clip entries. */
EMB_TEST(icon_bytes)
{
    ph base = ph_make("<div>x</div>", "");
    size_t b0 = embi_dl_bytes(&base.dl);
    size_t m0 = embi_mem_live();
    ph h = ph_make("<svg width=24 height=24 viewBox=\"0 0 24 24\" fill=none stroke=currentColor stroke-width=2 stroke-linecap=round>"
                   "<path d=\"M20.84 4.61a5.5 5.5 0 0 0-7.78 0L12 5.67l-1.06-1.06a5.5 5.5 0 0 0-7.78 7.78l1.06 1.06L12 21.23"
                   "l7.78-7.78 1.06-1.06a5.5 5.5 0 0 0 0-7.78z\"/></svg>",
                   "");
    size_t dl_bytes = embi_dl_bytes(&h.dl) - b0;
    (void)m0;
    uint64_t path_bytes = 0;
    for (uint32_t i = 0; i < h.dl.paths.len; i++) {
        path_bytes += svgp_bytes((embi_path *)h.dl.paths.data[i].path);
        SDL_Log("  path %u: %u quadratics", i, svgp_segments((embi_path *)h.dl.paths.data[i].path));
    }
    SDL_Log("icon 24x24: list %llu bytes beyond an empty page, paths %llu bytes", (unsigned long long)dl_bytes, (unsigned long long)path_bytes);
    EMB_CHECK(dl_bytes + path_bytes < 2048);
    ph_free(&h);
    ph_free(&base);
}

EMB_TEST(markers)
{
    ph h = ph_make("<svg width=100 height=100><defs><marker id=m markerWidth=4 markerHeight=4 refX=2 refY=2 orient=auto>"
                   "<rect width=4 height=4 fill=red /></marker></defs>"
                   "<path d=\"M10 10 L50 10 L50 50\" fill=none stroke=black marker-start=\"url(#m)\" marker-mid=\"url(#m)\" "
                   "marker-end=\"url(#m)\" /></svg>",
                   "");
    /* the stroke and three markers (start, mid, end) */
    EMB_CHECK(ph_count(&h.dl, EMBI_DI_PATH) == 4);
    /* the end marker points down (the path's last segment): its matrix rotates by 90 degrees */
    const embi_ditem *e = path_item(&h.dl, 3);
    EMB_REQUIRE(e != nullptr);
    const float *m = h.dl.xforms.data[e->xform].m;
    EMB_CHECK(near(m[0], 0) && near(m[1], 1) && near(m[4], 52) && near(m[5], 48));
    ph_free(&h);
}

EMB_TEST(patterns)
{
    ph h = ph_make("<svg width=100 height=100><defs><pattern id=p width=10 height=10 patternUnits=userSpaceOnUse>"
                   "<rect width=5 height=5 fill=red /></pattern>"
                   "<pattern id=q width=.5 height=.5><circle cx=.25 cy=.25 r=.2 fill=blue /></pattern></defs>"
                   "<rect width=30 height=20 fill=\"url(#p)\" /><rect width=100 height=100 fill=\"url(#q)\" /></svg>",
                   "");
    /* the first: 3 x 2 tiles and the coverage of the shape as a mask; the second: 2 x 2 tiles and the mask */
    EMB_CHECK(ph_count(&h.dl, EMBI_DI_PATH) == 6 + 1 + 4 + 1);
    uint32_t iso = 0, mask = 0;
    for (uint32_t i = 0; i < h.dl.items.len; i++) {
        iso += h.dl.items.data[i].kind == EMBI_DI_LAYER_BEGIN && h.dl.items.data[i].sub == EMBI_LS_ISO;
        mask += h.dl.items.data[i].kind == EMBI_DI_LAYER_BEGIN && h.dl.items.data[i].sub == EMBI_LS_SVGMASK;
    }
    EMB_CHECK(iso == 2 && mask == 2);
    ph_free(&h);
}

EMB_TEST(text)
{
    ph h = ph_make("<svg width=200 height=60><text id=t x=50 y=30 text-anchor=middle fill=blue font-size=20>Hello <tspan dx=4>world</tspan></text></svg>", "");
    EMB_CHECK(ph_count(&h.dl, EMBI_DI_TEXT) == 2);
    int i = ph_find(&h.dl, 0, EMBI_DI_TEXT);
    EMB_REQUIRE(i >= 0);
    const embi_ditem *a = &h.dl.items.data[i];
    EMB_CHECK(a->colors[0] == 0xFFFF0000u && near(a->f[1], 30));
    /* centered on x = 50: the chunk spans both pieces, so the first starts left of 50 */
    EMB_CHECK(a->f[0] < 50 && a->f[0] > 0);
    ph_free(&h);
}

EMB_TEST(no_svg_no_cost)
{
    ph h = ph_make("<div>plain</div>", "");
    EMB_CHECK(h.dl.paths.len == 0);
    ph_free(&h);
}

/* ------------------------------------------------------------------ hit testing */

static const embi_element *hit_hook(const embi_layout_engine *e, const embi_element *svg, const float box[4], float x, float y)
{
    embi_svg_env env;
    memset(&env, 0, sizeof env);
    env.se = embi_layout_style_engine(e);
    env.dev_scale = 1;
    return embi_svg_hit(&env, svg, box, x, y);
}

EMB_TEST(hit_testing)
{
    embi_layout_set_svg_hit(hit_hook);
    ph h = ph_make("<svg id=s width=100 height=100><rect id=a width=50 height=50 fill=red />"
                   "<circle id=c cx=75 cy=75 r=20 fill=none stroke=black stroke-width=6 />"
                   "<rect id=n x=0 y=60 width=30 height=30 pointer-events=none /></svg>",
                   "");
    embi_element *a = ph_id(&h, "a"), *c = ph_id(&h, "c"), *s = ph_id(&h, "s");
    EMB_CHECK(embi_layout_hit_test(h.eng, h.doc, 10, 10) == a);
    EMB_CHECK(embi_layout_hit_test(h.eng, h.doc, 95, 75) == c); /* on the stroke (cx + r) */
    EMB_CHECK(embi_layout_hit_test(h.eng, h.doc, 75, 75) == s); /* inside the unfilled circle: the svg itself */
    EMB_CHECK(embi_layout_hit_test(h.eng, h.doc, 10, 70) == s); /* pointer-events: none */
    ph_free(&h);
    embi_layout_set_svg_hit(nullptr);
}

/* ------------------------------------------------------------------ SVG as an image */

EMB_TEST(svg_image)
{
    static const char doc[] = "<?xml version=\"1.0\"?><!-- c --><svg xmlns=\"http://www.w3.org/2000/svg\" width=\"40\" height=\"20\">"
                              "<style>.a{fill:#0f0}</style><rect class=\"a\" width=\"40\" height=\"20\"/></svg>";
    EMB_CHECK(embi_svg_sniff((const uint8_t *)doc, sizeof doc - 1));
    EMB_CHECK(!embi_svg_sniff((const uint8_t *)"<html><svg>", 11));
    EMB_CHECK(!embi_svg_sniff((const uint8_t *)"\x89PNG", 4));
    ph h = ph_make("<div>x</div>", "");
    embi_svg_image *img = embi_svg_image_load(h.style, (const uint8_t *)doc, sizeof doc - 1);
    EMB_REQUIRE(img != nullptr);
    float w, hh;
    bool ratio;
    embi_svg_image_size(img, &w, &hh, &ratio);
    EMB_CHECK(near(w, 40) && near(hh, 20) && ratio);
    embi_dl d;
    embi_dl_init(&d);
    embi_svg_env env;
    memset(&env, 0, sizeof env);
    env.dev_scale = 1;
    float dest[4] = {10, 10, 80, 40}, uv[4] = {0, 0, 1, 1};
    embi_svg_paint_image(&env, &d, img, dest, uv, 0xFFFFFFFFu, 0, 0);
    EMB_CHECK(ph_count(&d, EMBI_DI_PATH) == 1);
    const embi_ditem *it = path_item(&d, 0);
    EMB_REQUIRE(it != nullptr);
    EMB_CHECK(it->colors[0] == 0xFF00FF00u);
    const float *m = d.xforms.data[it->xform].m;
    EMB_CHECK(near(m[0], 2) && near(m[4], 10) && near(m[5], 10));
    EMB_CHECK(d.clips.len >= 1);
    embi_dl_free(&d);
    embi_svg_image_free(img);
    ph_free(&h);
}

/* <img src=x.svg> and a CSS background: the painter asks the host for the parsed image and paints vector items. */
static embi_svg_image *g_test_img;

static const embi_svg_image *test_lookup(void *user, embi_str url)
{
    (void)user;
    return url.len >= 5 && memcmp(url.ptr, "x.svg", 5) == 0 ? g_test_img : nullptr;
}

static bool test_size(void *user, embi_str url, float *w, float *h, bool *ratio)
{
    (void)user;
    if (url.len >= 5 && memcmp(url.ptr, "x.svg", 5) == 0) {
        bool r;
        embi_svg_image_size(g_test_img, w, h, &r);
        *ratio = r;
        return true;
    }
    return false;
}

EMB_TEST(img_and_background)
{
    static const char doc[] = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"20\" height=\"20\"><rect width=\"20\" height=\"20\" fill=\"#00c\"/></svg>";
    ph tmp = ph_make("<div>x</div>", "");
    g_test_img = embi_svg_image_load(tmp.style, (const uint8_t *)doc, sizeof doc - 1);
    EMB_REQUIRE(g_test_img != nullptr);
    embi_paint_set_svg_image_fn(test_lookup);
    embi_paint_cfg cfg = {nullptr, test_size, 0xFF1A73E8u, 800.0f, 600.0f, 1.0f};
    ph h = ph_make_cfg("<img src=\"x.svg\" width=40 height=40><div style=\"width:60px;height:60px;background:url(x.svg) no-repeat\"></div>", "", &cfg);
    EMB_CHECK(ph_count(&h.dl, EMBI_DI_IMAGE) == 0);
    EMB_CHECK(ph_count(&h.dl, EMBI_DI_PATH) == 2);
    const embi_ditem *a = path_item(&h.dl, 0);
    EMB_REQUIRE(a != nullptr);
    EMB_CHECK(a->colors[0] == 0xFFCC0000u);
    /* a background is painted at the image's natural size, the img is scaled from 20 to 40 */
    const float *m = h.dl.xforms.data[a->xform].m;
    EMB_CHECK(near(m[0], 1) && near(m[3], 1) && near(m[5], 45));
    const embi_ditem *b = path_item(&h.dl, 1);
    EMB_REQUIRE(b != nullptr);
    m = h.dl.xforms.data[b->xform].m;
    EMB_CHECK(near(m[0], 2) && near(m[3], 2) && near(m[5], 0));
    ph_free(&h);
    embi_paint_set_svg_image_fn(nullptr);
    embi_svg_image_free(g_test_img);
    ph_free(&tmp);
}


/* ------------------------------------------------------------------ review round 2 */

/* A reflected gradient doubles its stops: 40 of them must fit the table. */
EMB_TEST(gradient_many_stops_reflect)
{
    char html[4096];
    int n = snprintf(html, sizeof html, "<svg width=100 height=100><defs><linearGradient id=g spreadMethod=reflect>");
    for (int i = 0; i < 40; i++) {
        n += snprintf(html + n, sizeof html - (size_t)n, "<stop offset=\"%d%%\" stop-color=\"%s\"/>", i * 100 / 39, i & 1 ? "red" : "blue");
    }
    snprintf(html + n, sizeof html - (size_t)n, "</linearGradient></defs><rect width=100 height=100 fill=\"url(#g)\"/></svg>");
    ph h = ph_make(html, "");
    const embi_ditem *a = path_item(&h.dl, 0);
    EMB_REQUIRE(a != nullptr);
    EMB_CHECK(a->styles[0] == EMBI_GRAD_LINEAR_DL && a->aux3 >= 80 && a->aux3 <= 82);
    ph_free(&h);
}

/* A hidden shape with markers, empty references and a tiny pattern: nothing leaks and nothing is read out of bounds. */
EMB_TEST(hidden_markers_no_leak)
{
    const char *html = "<svg width=100 height=100><defs><marker id=m markerWidth=4 markerHeight=4><rect width=4 height=4 /></marker></defs>"
                       "<path d=\"M10 10 L50 10 L50 50\" visibility=hidden stroke=black marker-start=\"url(#m)\" marker-end=\"url(#m)\"/>"
                       "<polyline points=\"1,1 5,5 9,1\" visibility=hidden marker-mid=\"url(#m)\"/></svg>";
    ph warm = ph_make(html, "");
    ph_free(&warm);
    size_t live = embi_mem_live();
    ph h = ph_make(html, "");
    EMB_CHECK(ph_count(&h.dl, EMBI_DI_PATH) == 0);
    ph_free(&h);
    EMB_CHECK(embi_mem_live() == live);
}

EMB_TEST(empty_references)
{
    const char *html = "<svg width=100 height=100><defs><linearGradient id=g /></defs><use href=\"\"/><use xlink:href=\"\"/>"
                       "<rect width=10 height=10 fill=\"url()\" stroke=\"url(#)\" clip-path=\"url()\" mask=\"url(#nope)\"/>"
                       "<linearGradient id=h href=\"\"/><rect width=10 height=10 fill=\"url(#h)\"/>"
                       "<defs><pattern id=p href=\"\" width=1 height=1><rect width=1 height=1 /></pattern></defs>"
                       "<rect width=10 height=10 fill=\"url(#p)\"/></svg>";
    ph warm = ph_make(html, "");
    ph_free(&warm);
    size_t live = embi_mem_live();
    ph h = ph_make(html, "");
    ph_free(&h);
    EMB_CHECK(embi_mem_live() == live);
    embi_str id;
    EMB_CHECK(!svg_ref_id((embi_str){nullptr, 0}, &id));
}

EMB_TEST(pattern_tiny_tile)
{
    ph h = ph_make("<svg width=100 height=100><defs><pattern id=p width=0.00001 height=0.00001 patternUnits=userSpaceOnUse>"
                   "<rect width=1 height=1 /></pattern><pattern id=q width=1e-30 height=1e30 patternUnits=userSpaceOnUse><rect width=1 height=1 /></pattern></defs>"
                   "<rect width=100 height=100 fill=\"url(#p)\"/><rect width=100 height=100 fill=\"url(#q)\"/></svg>",
                   "");
    EMB_CHECK(ph_count(&h.dl, EMBI_DI_PATH) == 0); /* too many tiles: nothing is painted */
    ph_free(&h);
}

/* A use bomb (level k holds two uses of level k-1) with a bounding-box clip must finish quickly. */
EMB_TEST(use_bomb_bbox_budget)
{
    char *html = SDL_malloc(16384);
    int n = snprintf(html, 16384, "<svg width=100 height=100><defs><clipPath id=c clipPathUnits=objectBoundingBox><rect width=1 height=1 /></clipPath>"
                                  "<rect id=g0 width=5 height=5 />");
    for (int i = 1; i <= 26; i++) {
        n += snprintf(html + n, 16384 - (size_t)n, "<g id=g%d><use href=\"#g%d\"/><use href=\"#g%d\" x=1 /></g>", i, i - 1, i - 1);
    }
    snprintf(html + n, 16384 - (size_t)n, "</defs><g clip-path=\"url(#c)\" transform-origin=\"10px 10px\" transform=\"rotate(3)\"><use href=\"#g26\"/></g></svg>");
    uint64_t t0 = SDL_GetTicks();
    ph h = ph_make(html, "");
    uint64_t dt = SDL_GetTicks() - t0;
    EMB_CHECK(dt < 20000);
    ph_free(&h);
    SDL_free(html);
}

EMB_TEST(marker_vertices_after_closepath)
{
    svg_verts vs = {0};
    bool err;
    const char *d = "M10 10 L50 10 L50 50 Z M60 60 L80 80";
    embi_path *p = svg_parse_path(d, (uint32_t)strlen(d), 0.0f, &vs, &err);
    EMB_CHECK(!err && vs.n == 6); /* 4 for the closed subpath (start, 2 corners, close), 2 for the line */
    svgp_free(p);
    svg_verts_free(&vs);
    memset(&vs, 0, sizeof vs);
    d = "M10 10 L50 10 Z";
    p = svg_parse_path(d, (uint32_t)strlen(d), 0.0f, &vs, &err);
    EMB_CHECK(vs.n == 3);
    svgp_free(p);
    svg_verts_free(&vs);
}

EMB_TEST(marker_vertices_of_closed_shapes)
{
    /* a path that returns to its start before the z has no extra vertex for the zero-length closepath */
    svg_verts vs = {0};
    bool err;
    const char *d = "M0 0 L10 0 L10 10 L0 0 z";
    embi_path *p = svg_parse_path(d, (uint32_t)strlen(d), 0.0f, &vs, &err);
    EMB_CHECK(!err && vs.n == 4 && vs.v[0].seg == 0 && vs.v[3].seg == 2);
    /* the end vertex of a closed subpath is bisected like its start: both see the closing and the first segment */
    EMB_CHECK(vs.v[0].has_in && vs.v[0].has_out && vs.v[3].has_in && vs.v[3].has_out);
    svgp_free(p);
    svg_verts_free(&vs);
    /* a polygon is M p0 L p1 ... L pn-1 Z: n + 1 vertices, the last one back at p0 */
    memset(&vs, 0, sizeof vs);
    double xy[4] = {250, 150, 200, 150};
    svg_verts_from_points(&vs, xy, 2, true);
    EMB_CHECK(vs.n == 3 && vs.v[0].seg == 0 && vs.v[1].seg == 1 && vs.v[2].seg == 2);
    EMB_CHECK(vs.v[2].x == 250 && vs.v[2].y == 150 && vs.v[1].x == 200);
    svg_verts_free(&vs);
    memset(&vs, 0, sizeof vs);
    svg_verts_from_points(&vs, xy, 2, false);
    EMB_CHECK(vs.n == 2);
    svg_verts_free(&vs);
}

EMB_TEST(empty_clip_path_clips_everything)
{
    ph h = ph_make("<svg width=100 height=100><defs><clipPath id=c></clipPath><clipPath id=d><rect width=5 height=5 style=\"display:none\"/></clipPath>"
                   "<mask id=m></mask></defs>"
                   "<rect width=50 height=50 fill=red clip-path=\"url(#c)\"/><rect width=50 height=50 fill=red clip-path=\"url(#d)\"/>"
                   "<rect width=50 height=50 fill=red mask=\"url(#m)\"/><rect x=60 width=10 height=10 fill=blue /></svg>",
                   "");
    EMB_CHECK(ph_count(&h.dl, EMBI_DI_PATH) == 1);
    const embi_ditem *a = path_item(&h.dl, 0);
    EMB_REQUIRE(a != nullptr);
        EMB_CHECK(a->colors[0] == 0xFFFF0000u);
    ph_free(&h);
}

EMB_TEST(text_lists_and_anchor)
{
    /* x list: each character is placed alone */
    ph h = ph_make("<svg width=200 height=60><text x=\"10 40 80\" y=30 font-size=20>ABC</text></svg>", "");
    EMB_CHECK(ph_count(&h.dl, EMBI_DI_TEXT) == 3);
    int i = ph_find(&h.dl, 0, EMBI_DI_TEXT);
    EMB_REQUIRE(i >= 0);
    EMB_CHECK(near(h.dl.items.data[i].f[0], 10));
    i = ph_find(&h.dl, (uint32_t)i + 1, EMBI_DI_TEXT);
    EMB_CHECK(i >= 0 && near(h.dl.items.data[i].f[0], 40));
    ph_free(&h);
    /* rotate repeats its last value: the rotated pieces get their own transforms */
    h = ph_make("<svg width=200 height=60><text x=10 y=30 rotate=\"0 90\" font-size=20>ABC</text></svg>", "");
    EMB_CHECK(ph_count(&h.dl, EMBI_DI_TEXT) == 3);
    int k = ph_find(&h.dl, 0, EMBI_DI_TEXT);
    EMB_REQUIRE(k >= 0);
    k = ph_find(&h.dl, (uint32_t)k + 1, EMBI_DI_TEXT);
    EMB_REQUIRE(k >= 0);
    const float *m = h.dl.xforms.data[h.dl.items.data[k].xform].m;
    EMB_CHECK(near(m[0], 0) && near(m[1], 1));
    ph_free(&h);
    /* the trailing white space does not move a centered text */
    ph a = ph_make("<svg width=200 height=60><text x=100 y=30 text-anchor=middle font-size=20>Hello</text></svg>", "");
    ph b = ph_make("<svg width=200 height=60><text x=100 y=30 text-anchor=middle font-size=20>\n  Hello\n</text></svg>", "");
    int ia = ph_find(&a.dl, 0, EMBI_DI_TEXT), ib = ph_find(&b.dl, 0, EMBI_DI_TEXT);
    EMB_REQUIRE(ia >= 0 && ib >= 0);
    EMB_CHECK(near(a.dl.items.data[ia].f[0], b.dl.items.data[ib].f[0]));
    ph_free(&a);
    ph_free(&b);
    /* dominant-baseline: central moves the glyph origin down */
    ph c0 = ph_make("<svg width=200 height=60><text x=10 y=30 font-size=20>A</text></svg>", "");
    ph c1 = ph_make("<svg width=200 height=60><text x=10 y=30 font-size=20 dominant-baseline=central>A</text></svg>", "");
    int i0 = ph_find(&c0.dl, 0, EMBI_DI_TEXT), i1 = ph_find(&c1.dl, 0, EMBI_DI_TEXT);
    EMB_REQUIRE(i0 >= 0 && i1 >= 0);
    EMB_CHECK(c1.dl.items.data[i1].f[1] > c0.dl.items.data[i0].f[1] + 1.0f);
    ph_free(&c0);
    ph_free(&c1);
}

EMB_TEST(explicit_transform_origin)
{
    /* an explicit 50% 50% is the centre of the viewport (100x100), not the (0,0) default */
    ph h = ph_make("<svg width=100 height=100><rect width=10 height=10 style=\"transform:rotate(90deg);transform-origin:50% 50%\"/></svg>", "");
    const embi_ditem *a = path_item(&h.dl, 0);
    EMB_REQUIRE(a != nullptr);
    const float *m = h.dl.xforms.data[a->xform].m;
    EMB_CHECK(near(m[4], 100) && near(m[5], 0));
    ph_free(&h);
    h = ph_make("<svg width=100 height=100><rect width=10 height=10 style=\"transform:rotate(90deg)\"/></svg>", "");
    a = path_item(&h.dl, 0);
    EMB_REQUIRE(a != nullptr);
    m = h.dl.xforms.data[a->xform].m;
    EMB_CHECK(near(m[4], 0) && near(m[5], 0));
    ph_free(&h);
}

EMB_TEST(svg_image_fits_viewbox)
{
    static const char doc[] = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10\" height=\"10\" viewBox=\"0 0 10 10\">"
                              "<rect width=\"10\" height=\"10\" fill=\"#0f0\"/></svg>";
    ph h = ph_make("<div>x</div>", "");
    embi_svg_image *img = embi_svg_image_load(h.style, (const uint8_t *)doc, sizeof doc - 1);
    EMB_REQUIRE(img != nullptr);
    embi_dl d;
    embi_dl_init(&d);
    embi_svg_env env;
    memset(&env, 0, sizeof env);
    env.dev_scale = 1;
    float dest[4] = {0, 0, 40, 20}, uv[4] = {0, 0, 1, 1};
    embi_svg_paint_image(&env, &d, img, dest, uv, 0xFFFFFFFFu, 0, 0);
    const embi_ditem *it = path_item(&d, 0);
    EMB_REQUIRE(it != nullptr);
    const float *m = d.xforms.data[it->xform].m;
    /* letterboxed: scale 2 (not 4 x 2), centered horizontally */
    EMB_CHECK(near(m[0], 2) && near(m[3], 2) && near(m[4], 10) && near(m[5], 0));
    embi_dl_free(&d);
    embi_svg_image_free(img);
    ph_free(&h);
}

EMB_TEST(root_overflow_visible)
{
    ph h = ph_make("<svg width=10 height=10 style=\"overflow:visible\"><rect width=50 height=50 /></svg>", "");
    const embi_ditem *a = path_item(&h.dl, 0);
    EMB_REQUIRE(a != nullptr);
    /* no clip of the 10 x 10 viewport on the item */
    bool clipped = false;
    for (uint32_t c = a->clip; c != 0 && c < h.dl.clips.len; c = h.dl.clips.data[c].parent) {
        if (h.dl.clips.data[c].rect[2] == 10.0f && h.dl.clips.data[c].rect[3] == 10.0f) {
            clipped = true;
        }
    }
    EMB_CHECK(!clipped);
    ph_free(&h);
}

EMB_TEST(polyline_many_points)
{
    char *pts = SDL_malloc(60000);
    int n = snprintf(pts, 60000, "<svg width=100 height=100><polyline fill=none stroke=black points=\"");
    for (int i = 0; i < 5000; i++) {
        n += snprintf(pts + n, 60000 - (size_t)n, "%d,%d ", i % 100, (i * 7) % 100);
    }
    snprintf(pts + n, 60000 - (size_t)n, "\"/></svg>");
    ph h = ph_make(pts, "");
    const embi_ditem *a = path_item(&h.dl, 0);
    EMB_REQUIRE(a != nullptr);
    EMB_CHECK(svgp_segments((embi_path *)h.dl.paths.data[a->aux].path) >= 4999); /* more than the old 2048 point cap */
    ph_free(&h);
    SDL_free(pts);
}

/* A view with hundreds of distinct SVG icons does not keep them all. */
EMB_TEST(svgcache_evicts_least_recently_used)
{
    ph h = ph_make("<div>x</div>", "");
    embi_svgcache *c = embi_svgcache_create();
    char doc[1500];
    char url[32];
    for (int i = 0; i < 700; i++) {
        int n = snprintf(doc, sizeof doc, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"24\" height=\"24\"><rect width=\"%d\" height=\"5\"/><!--", i + 1);
        memset(doc + n, 'x', 900);
        n += 900;
        n += snprintf(doc + n, sizeof doc - (size_t)n, "--></svg>");
        int un = snprintf(url, sizeof url, "u%d.svg", i);
        EMB_CHECK(embi_svgcache_add(c, h.style, (embi_str){url, (uint32_t)un}, (const uint8_t *)doc, (size_t)n));
        if (i == 10) {
            (void)embi_svgcache_get(c, (embi_str){"u0.svg", 6});
        }
    }
    /* adding never frees an image (one fetched for a paint in progress stays valid): only a trim at a safe point evicts */
    EMB_CHECK(embi_svgcache_get(c, (embi_str){"u4.svg", 6}) != nullptr); /* (this get makes u4 the newest) */
    EMB_CHECK(embi_svgcache_bytes(c) > 512u * 1024u + 4096u);
    embi_svgcache_trim(c);
    EMB_CHECK(embi_svgcache_bytes(c) <= 512u * 1024u + 4096u);
    EMB_CHECK(embi_svgcache_get(c, (embi_str){"u699.svg", 8}) != nullptr);
    EMB_CHECK(embi_svgcache_get(c, (embi_str){"u5.svg", 6}) == nullptr);
    embi_svgcache_destroy(c);
    ph_free(&h);
}

EMB_TEST(use_instance_em_units)
{
    /* the instance inherits font-size from the use element: 2em is 40 there, 32 in place */
    ph h = ph_make("<svg width=200 height=100><defs><rect id=r width=\"2em\" height=10 /></defs><use href=\"#r\" style=\"font-size:20px\" /></svg>", "");
    const embi_ditem *a = path_item(&h.dl, 0);
    EMB_REQUIRE(a != nullptr);
    EMB_CHECK(near(a->rect[2], 40));
    ph_free(&h);
}

/* The UA sheet gives symbol and nested svg overflow:hidden: a use of a symbol clips to its width and height. */
EMB_TEST(symbol_and_nested_svg_clip)
{
    ph h = ph_make("<svg width=500 height=500><defs><symbol id=sym><rect width=400 height=400 fill=green /></symbol></defs>"
                   "<use href=\"#sym\" width=100 height=100 /><svg x=200 width=50 height=60><rect width=400 height=400 /></svg></svg>", "");
    EMB_CHECK(ph_count(&h.dl, EMBI_DI_PATH) == 2);
    bool a = false, b = false;
    for (uint32_t i = 0; i < h.dl.items.len; i++) {
        const embi_ditem *it = &h.dl.items.data[i];
        if (it->kind != EMBI_DI_PATH) {
            continue;
        }
        for (uint32_t c = it->clip; c != 0 && c < h.dl.clips.len; c = h.dl.clips.data[c].parent) {
            const float *r = h.dl.clips.data[c].rect;
            a |= r[2] == 100.0f && r[3] == 100.0f;
            b |= r[2] == 50.0f && r[3] == 60.0f;
        }
    }
    EMB_CHECK(a && b);
    ph_free(&h);
}

EMB_TEST(outer_svg_transform_attribute)
{
    /* the transform attribute of an outermost svg is the CSS transform of its box */
    ph h = ph_make("<div id=a><svg id=s width=100 height=100 transform=\"translate(10, 20) scale(2)\"></svg></div>", "");
    embi_element *s = ph_id(&h, "s");
    const embi_value *v = embi_style_value((const embi_style *)s->style, EMBI_PROP_transform);
    EMB_CHECK(v != nullptr && v->kind != EMBI_V_NONE && embi_lay_has_transform((const embi_style *)s->style));
    ph_free(&h);
}

EMB_TEST(clip_path_from_css_property)
{
    ph h = ph_make("<svg width=100 height=100><defs><clipPath id=c><rect width=10 height=10 /></clipPath></defs>"
                   "<rect width=50 height=50 fill=red style=\"clip-path:url(#c)\" /></svg>", "");
    uint32_t iso = 0;
    for (uint32_t i = 0; i < h.dl.items.len; i++) {
        iso += h.dl.items.data[i].kind == EMBI_DI_LAYER_BEGIN && h.dl.items.data[i].sub == EMBI_LS_ISO;
    }
    EMB_CHECK(iso == 1);
    ph_free(&h);
}

/* ------------------------------------------------------------------ attribute and sizing details */

/* An empty or blank gradientTransform counts as not specified: the value of the gradient it references shows through. */
EMB_TEST(gradient_empty_attribute_inherits_through_href)
{
    ph h = ph_make("<svg width=100 height=100><defs>"
                   "<linearGradient id=a gradientUnits=userSpaceOnUse gradientTransform=\"rotate(90)\" x2=100>"
                   "<stop offset=0 stop-color=red /><stop offset=1 stop-color=blue /></linearGradient>"
                   "<linearGradient id=b href=\"#a\" gradientTransform=\"\" />"
                   "<radialGradient id=c gradientUnits=userSpaceOnUse gradientTransform=\"scale(2)\" r=10>"
                   "<stop offset=0 stop-color=red /><stop offset=1 stop-color=blue /></radialGradient>"
                   "<radialGradient id=d href=\"#c\" gradientTransform=\" \" />"
                   "<linearGradient id=e gradientUnits=userSpaceOnUse gradientTransform=\"\" x2=100>"
                   "<stop offset=0 stop-color=red /><stop offset=1 stop-color=blue /></linearGradient></defs>"
                   "<rect width=100 height=100 fill=\"url(#b)\" /><rect width=100 height=100 fill=\"url(#d)\" />"
                   "<rect width=100 height=100 fill=\"url(#e)\" /></svg>",
                   "");
    const embi_ditem *a = path_item(&h.dl, 0), *b = path_item(&h.dl, 1), *e = path_item(&h.dl, 2);
    EMB_REQUIRE(a != nullptr && b != nullptr && e != nullptr);
    EMB_CHECK(a->styles[0] == EMBI_GRAD_LINEAR_DL && b->styles[0] == EMBI_GRAD_RADIAL_DL);
    const float *m = h.dl.xforms.data[a->xform].m;
    EMB_CHECK(near(m[0], 0) && near(m[1], 1) && near(m[2], -1) && near(m[3], 0)); /* rotate(90) from #a */
    m = h.dl.xforms.data[b->xform].m;
    EMB_CHECK(near(m[0], 2) && near(m[3], 2)); /* scale(2) from #c */
    m = h.dl.xforms.data[e->xform].m;
    EMB_CHECK(near(m[0], 1) && near(m[1], 0) && near(m[3], 1)); /* nothing to inherit: the identity */
    ph_free(&h);
}

/* With objectBoundingBox units an absolute length is its pixel value used as a fraction of the box (SVG 2 pservers). */
EMB_TEST(gradient_bounding_box_absolute_units)
{
    ph h = ph_make("<svg width=300 height=100><defs><linearGradient id=g x1=\"0\" x2=\"0.5in\" y2=\"10%\">"
                   "<stop offset=0 stop-color=red /><stop offset=1 stop-color=blue /></linearGradient></defs>"
                   "<rect width=200 height=100 fill=\"url(#g)\" /></svg>",
                   "");
    const embi_ditem *a = path_item(&h.dl, 0);
    EMB_REQUIRE(a != nullptr && a->styles[0] == EMBI_GRAD_LINEAR_DL);
    EMB_CHECK(near(a->f[0], 0) && near(a->f[2], 48) && near(a->f[3], 0.1)); /* 0.5in = 48 px, so 48 boxes wide */
    /* the gradient matrix is the box */
    const float *m = h.dl.xforms.data[a->xform].m;
    EMB_CHECK(near(m[0], 200) && near(m[3], 100));
    ph_free(&h);
}

/* The marker shorthand sets the three marker properties; an invalid value is dropped. */
EMB_TEST(marker_shorthand)
{
    const char *defs = "<svg width=100 height=100><defs><marker id=m markerWidth=4 markerHeight=4><rect width=4 height=4 /></marker></defs>";
    static const struct {
        const char *style;
        uint32_t paths;
    } k_cases[] = {
        {"marker:url(#m)", 4},                 /* the stroke and the start, mid and end markers */
        {"marker:url(#m);marker-mid:none", 3}, /* a longhand after it */
        {"marker:url(#m) url(#m)", 1},         /* invalid: one value only */
        {"marker:url(#m);marker:bogus", 4},    /* the invalid declaration is dropped, the valid one stays */
        {"marker-start:url(#m);marker:none", 1},
    };
    for (size_t i = 0; i < sizeof k_cases / sizeof k_cases[0]; i++) {
        char html[512];
        snprintf(html, sizeof html, "%s<path d=\"M10 10 L50 10 L50 50\" fill=none stroke=black style=\"%s\" /></svg>", defs,
                 k_cases[i].style);
        ph h = ph_make(html, "");
        if (ph_count(&h.dl, EMBI_DI_PATH) != k_cases[i].paths) {
            SDL_Log("    marker case %s: %u paths", k_cases[i].style, ph_count(&h.dl, EMBI_DI_PATH));
        }
        EMB_CHECK(ph_count(&h.dl, EMBI_DI_PATH) == k_cases[i].paths);
        ph_free(&h);
    }
}

/* Marker orient angles take deg (the default), grad, rad and turn. */
EMB_TEST(marker_orient_angle_units)
{
    double r;
    EMB_CHECK(svg_parse_angle("45", 2, &r) && near(r, 3.14159265358979 / 4));
    EMB_CHECK(svg_parse_angle(" 90deg ", 7, &r) && near(r, 3.14159265358979 / 2));
    EMB_CHECK(svg_parse_angle("100grad", 7, &r) && near(r, 3.14159265358979 / 2));
    EMB_CHECK(svg_parse_angle("-0.5turn", 8, &r) && near(r, -3.14159265358979));
    EMB_CHECK(svg_parse_angle("1RAD", 4, &r) && near(r, 1));
    EMB_CHECK(!svg_parse_angle("45foo", 5, &r));
    EMB_CHECK(!svg_parse_angle("4 5", 3, &r));
    EMB_CHECK(!svg_parse_angle("deg", 3, &r));
    static const char *k_orients[] = {"90", "90deg", "100grad", "1.5707963rad", "0.25turn"};
    for (size_t i = 0; i < sizeof k_orients / sizeof k_orients[0]; i++) {
        char html[512];
        snprintf(html, sizeof html,
                 "<svg width=100 height=100><defs><marker id=m markerWidth=4 markerHeight=4 orient=\"%s\">"
                 "<rect width=4 height=4 /></marker></defs>"
                 "<path d=\"M10 10 L50 10\" fill=none stroke=black marker-start=\"url(#m)\" /></svg>",
                 k_orients[i]);
        ph h = ph_make(html, "");
        const embi_ditem *e = path_item(&h.dl, 1);
        EMB_REQUIRE(e != nullptr);
        const float *m = h.dl.xforms.data[e->xform].m;
        EMB_CHECK(near(m[0], 0) && near(m[1], 1)); /* rotated by a quarter turn */
        ph_free(&h);
    }
}

/* A view element named by the fragment of an SVG image's URL gives the image its viewBox. */
EMB_TEST(svg_image_fragment_view)
{
    static const char doc[] = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\" viewBox=\"0 0 100 100\">"
                              "<view id=\"v\" viewBox=\"50 50 50 50\"/><rect width=\"100\" height=\"100\" fill=\"#0f0\"/></svg>";
    ph h = ph_make("<div>x</div>", "");
    embi_svg_image *img = embi_svg_image_load(h.style, (const uint8_t *)doc, sizeof doc - 1);
    EMB_REQUIRE(img != nullptr);
    float dest[4] = {0, 0, 100, 100}, uv[4] = {0, 0, 1, 1};
    static const embi_str k_frags[] = {{"v", 1}, {"nope", 4}, {nullptr, 0}};
    for (int pass = 0; pass < 3; pass++) {
        embi_svg_image_set_fragment(img, k_frags[pass]);
        embi_dl d;
        embi_dl_init(&d);
        embi_svg_env env;
        memset(&env, 0, sizeof env);
        env.dev_scale = 1;
        embi_svg_paint_image(&env, &d, img, dest, uv, 0xFFFFFFFFu, 0, 0);
        const embi_ditem *it = path_item(&d, 0);
        EMB_REQUIRE(it != nullptr);
        const float *m = d.xforms.data[it->xform].m;
        if (pass == 0) {
            EMB_CHECK(near(m[0], 2) && near(m[3], 2) && near(m[4], -100) && near(m[5], -100)); /* the view's 50 x 50 fills it */
        } else {
            EMB_CHECK(near(m[0], 1) && near(m[3], 1) && near(m[4], 0) && near(m[5], 0)); /* no such view: the root's */
        }
        embi_dl_free(&d);
    }
    embi_svg_image_free(img);
    ph_free(&h);
}

/* Whether some clip of the item (or its parents) is a w x h rectangle. */
static bool clipped_to(const embi_dl *d, const embi_ditem *it, float w, float h)
{
    for (uint32_t c = it->clip; c != 0 && c < d->clips.len; c = d->clips.data[c].parent) {
        if (near(d->clips.data[c].rect[2], w) && near(d->clips.data[c].rect[3], h)) {
            return true;
        }
    }
    return false;
}

/* overflow: auto clips an svg root and a nested svg as hidden does (svg content does not scroll). */
EMB_TEST(svg_overflow_auto_clips)
{
    ph h = ph_make("<svg width=10 height=10 style=\"overflow:auto\"><rect width=50 height=50 /></svg>", "");
    const embi_ditem *a = path_item(&h.dl, 0);
    EMB_REQUIRE(a != nullptr);
    EMB_CHECK(clipped_to(&h.dl, a, 10, 10));
    ph_free(&h);
    h = ph_make("<svg width=100 height=100><svg width=20 height=30 style=\"overflow:auto\"><rect width=50 height=50 /></svg>"
                "<svg x=40 width=20 height=30 style=\"overflow:visible\"><rect width=50 height=50 /></svg></svg>",
                "");
    a = path_item(&h.dl, 0);
    const embi_ditem *b = path_item(&h.dl, 1);
    EMB_REQUIRE(a != nullptr && b != nullptr);
    EMB_CHECK(clipped_to(&h.dl, a, 20, 30));
    EMB_CHECK(!clipped_to(&h.dl, b, 20, 30));
    ph_free(&h);
}

static uint32_t layer_count(const embi_dl *d, uint8_t sub)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < d->items.len; i++) {
        n += d->items.data[i].kind == EMBI_DI_LAYER_BEGIN && d->items.data[i].sub == sub;
    }
    return n;
}

/* Marker content inside the marker viewport needs no clip; content that crosses it is clipped through a mask group. */
EMB_TEST(marker_clip_only_when_needed)
{
    ph h = ph_make("<svg width=100 height=100><defs><marker id=m markerWidth=4 markerHeight=4><rect width=4 height=4 /></marker></defs>"
                   "<path d=\"M10 10 L50 10 L50 50\" fill=none stroke=black marker-start=\"url(#m)\" marker-end=\"url(#m)\" /></svg>",
                   "");
    EMB_CHECK(ph_count(&h.dl, EMBI_DI_PATH) == 3 && layer_count(&h.dl, EMBI_LS_SVGMASK) == 0);
    ph_free(&h);
    h = ph_make("<svg width=100 height=100><defs><marker id=m markerWidth=4 markerHeight=4><rect width=8 height=8 /></marker></defs>"
                "<path d=\"M10 10 L50 10 L50 50\" fill=none stroke=black marker-start=\"url(#m)\" marker-end=\"url(#m)\" /></svg>",
                "");
    EMB_CHECK(layer_count(&h.dl, EMBI_LS_SVGMASK) == 2); /* one per marker */
    ph_free(&h);
    /* a stroked child may reach past its box: it is clipped too */
    h = ph_make("<svg width=100 height=100><defs><marker id=m markerWidth=4 markerHeight=4><rect width=4 height=4 stroke=red /></marker></defs>"
                "<path d=\"M10 10 L50 10\" fill=none stroke=black marker-end=\"url(#m)\" /></svg>",
                "");
    EMB_CHECK(layer_count(&h.dl, EMBI_LS_SVGMASK) == 1);
    ph_free(&h);
    /* overflow: visible never clips */
    h = ph_make("<svg width=100 height=100><defs><marker id=m markerWidth=4 markerHeight=4 style=\"overflow:visible\"><rect width=8 height=8 /></marker></defs>"
                "<path d=\"M10 10 L50 10\" fill=none stroke=black marker-end=\"url(#m)\" /></svg>",
                "");
    EMB_CHECK(layer_count(&h.dl, EMBI_LS_SVGMASK) == 0);
    ph_free(&h);
}

/* An inline svg with a viewBox and no width or height is as wide as its container, and its height follows the ratio. */
EMB_TEST(svg_ratio_without_size)
{
    ph h = ph_make("<div style=\"width:300px\"><svg id=a viewBox=\"0 0 100 50\"></svg></div>"
                   "<div style=\"width:500px\"><svg id=b viewBox=\"0 0 100 50\" style=\"display:block\"></svg></div>"
                   "<div style=\"width:500px\"><svg id=c viewBox=\"0 0 100 50\" style=\"width:200px\"></svg></div>"
                   "<div style=\"width:500px\"><svg id=d viewBox=\"0 0 100 50\" style=\"height:20px\"></svg></div>"
                   "<div style=\"width:500px\"><svg id=e viewBox=\"0 0 100 50\" style=\"margin-left:100px;display:block\"></svg></div>",
                   "");
    embi_rectf r;
    EMB_REQUIRE(embi_layout_border_box(h.eng, ph_id(&h, "a"), &r));
    EMB_CHECK(near(r.w, 300) && near(r.h, 150));
    EMB_REQUIRE(embi_layout_border_box(h.eng, ph_id(&h, "b"), &r));
    EMB_CHECK(near(r.w, 500) && near(r.h, 250));
    EMB_REQUIRE(embi_layout_border_box(h.eng, ph_id(&h, "c"), &r));
    EMB_CHECK(near(r.w, 200) && near(r.h, 100)); /* a given width: the ratio gives the height */
    EMB_REQUIRE(embi_layout_border_box(h.eng, ph_id(&h, "d"), &r));
    EMB_CHECK(near(r.w, 40) && near(r.h, 20)); /* a given height: the ratio gives the width */
    EMB_REQUIRE(embi_layout_border_box(h.eng, ph_id(&h, "e"), &r));
    EMB_CHECK(near(r.w, 400) && near(r.h, 200)); /* the margin comes off the available width */
    ph_free(&h);
}

/* A pattern of thousands of tiles paints every tile (the walk draws one and the rest are copies). */
EMB_TEST(pattern_many_tiles)
{
    ph h = ph_make("<svg width=500 height=500><defs><pattern id=p width=10 height=10 patternUnits=userSpaceOnUse>"
                   "<rect width=5 height=5 fill=red /></pattern></defs><rect width=500 height=500 fill=\"url(#p)\" /></svg>",
                   "");
    EMB_CHECK(ph_count(&h.dl, EMBI_DI_PATH) == 2500 + 1); /* 50 x 50 tiles and the coverage mask */
    /* each copy is moved by its tile offset */
    bool seen = false;
    for (uint32_t i = 0; i < h.dl.items.len; i++) {
        const embi_ditem *it = &h.dl.items.data[i];
        if (it->kind == EMBI_DI_PATH && it->colors[0] == 0xFF0000FFu) {
            const float *m = h.dl.xforms.data[it->xform].m;
            seen |= near(m[4], 490) && near(m[5], 490);
        }
    }
    EMB_CHECK(seen);
    ph_free(&h);
}

/* ------------------------------------------------------------------ foreignObject */

static const embi_element *hit_hook_fo(const embi_layout_engine *e, const embi_element *svg, const float box[4], float x, float y)
{
    embi_svg_env env;
    memset(&env, 0, sizeof env);
    env.se = embi_layout_style_engine(e);
    env.dev_scale = 1;
    env.layout = e;
    return embi_svg_hit(&env, svg, box, x, y);
}

/* The item index of the first FILL with color rgba at or after from, or -1. */
static int fill_from(const embi_dl *d, uint32_t from, uint32_t rgba)
{
    for (uint32_t i = from; i < d->items.len; i++) {
        if (d->items.data[i].kind == EMBI_DI_FILL && d->items.data[i].colors[0] == rgba) {
            return (int)i;
        }
    }
    return -1;
}

/* The HTML content of a foreignObject is laid out in its x, y, width, height box, painted where the SVG walk meets the element
 * (under its transform, clipped to the viewport) and hit tested there. */
EMB_TEST(foreign_object_layout_paint_hit)
{
    embi_layout_set_svg_hit(hit_hook_fo);
    ph h = ph_make("<svg width=300 height=200><rect id=s width=300 height=200 fill=\"#f00\" />"
                   "<g transform=\"translate(10 20)\"><foreignObject id=f x=100 y=10 width=80 height=50>"
                   "<div id=d style=\"height:30px;background:#00f\"></div><div id=e style=\"height:100px;background:#0f0\"></div>"
                   "</foreignObject></g><rect x=250 width=50 height=50 fill=\"#ff0\" /></svg>",
                   "");
    embi_element *f = ph_id(&h, "f"), *d = ph_id(&h, "d"), *e = ph_id(&h, "e"), *s = ph_id(&h, "s");
    EMB_REQUIRE(f != nullptr && d != nullptr && e != nullptr);
    /* layout: the foreignObject is a block of its width and height; the divs fill its width */
    embi_rectf r;
    EMB_REQUIRE(embi_layout_border_box(h.eng, d, &r));
    EMB_CHECK(near(r.w, 80) && near(r.h, 30));
    /* paint: the divs come between the svg's two rects, in the foreignObject's user space, clipped to 80 x 50 */
    int red = ph_find(&h.dl, 0, EMBI_DI_PATH);
    int blue = fill_from(&h.dl, 0, 0xFFFF0000u), green = fill_from(&h.dl, 0, 0xFF00FF00u);
    EMB_REQUIRE(red >= 0 && blue > red && green > blue);
    int yellow = -1;
    for (uint32_t i = (uint32_t)green; i < h.dl.items.len; i++) {
        if (h.dl.items.data[i].kind == EMBI_DI_PATH) {
            yellow = (int)i;
        }
    }
    EMB_CHECK(yellow > green);
    const embi_ditem *bi = &h.dl.items.data[blue];
    const float *m = h.dl.xforms.data[bi->xform].m;
    EMB_CHECK(near(m[0], 1) && near(m[3], 1) && near(m[4] + bi->rect[0], 110) && near(m[5] + bi->rect[1], 30));
    bool clipped = false;
    const embi_ditem *gi = &h.dl.items.data[green];
    for (uint32_t c = gi->clip; c != 0 && c < h.dl.clips.len; c = h.dl.clips.data[c].parent) {
        clipped |= near(h.dl.clips.data[c].rect[2], 80) && near(h.dl.clips.data[c].rect[3], 50);
    }
    EMB_CHECK(clipped);
    /* hit: through the group's transform, the viewport clips the second div */
    EMB_CHECK(embi_layout_hit_test(h.eng, h.doc, 115, 35) == d);
    EMB_CHECK(embi_layout_hit_test(h.eng, h.doc, 115, 65) == e);
    EMB_CHECK(embi_layout_hit_test(h.eng, h.doc, 115, 85) == s); /* below the 50 px viewport: the rect under it */
    EMB_CHECK(embi_layout_hit_test(h.eng, h.doc, 50, 35) == s);
    ph_free(&h);
    /* not drawn in a use instance (the one fragment tree is drawn once, where the svg walk meets the element) */
    h = ph_make("<svg width=300 height=200><defs><g id=t><foreignObject width=50 height=50><div style=\"height:10px;background:#00f\"></div>"
                "</foreignObject></g></defs><use href=\"#t\" /></svg>", "");
    EMB_CHECK(fill_from(&h.dl, 0, 0xFFFF0000u) < 0);
    ph_free(&h);
    embi_layout_set_svg_hit(nullptr);
}

/* The pattern copies (their transform and clip maps) and the foreignObject boxes leave nothing behind. */
EMB_TEST(pattern_and_foreign_no_leak)
{
    const char *html = "<svg width=200 height=200><defs><pattern id=p width=10 height=10 patternUnits=userSpaceOnUse>"
                       "<g transform=\"rotate(5)\"><rect width=5 height=5 fill=red clip-path=\"url(#c)\" /></g></pattern>"
                       "<clipPath id=c><circle r=3 /></clipPath></defs><rect width=200 height=200 fill=\"url(#p)\" />"
                       "<foreignObject x=10 y=10 width=50 height=50><p>text <b>bold</b></p></foreignObject></svg>";
    ph warm = ph_make(html, "");
    ph_free(&warm);
    size_t live = embi_mem_live();
    ph h = ph_make(html, "");
    EMB_CHECK(ph_count(&h.dl, EMBI_DI_PATH) > 400);
    ph_free(&h);
    EMB_CHECK(embi_mem_live() == live);
}

/* ------------------------------------------------------------------ R4e: the R3 effects on SVG content */

/* The first LAYER_BEGIN of sub (at or after from), or nullptr. */
static const embi_ditem *first_layer(const embi_dl *d, uint8_t sub, uint32_t from)
{
    for (uint32_t i = from; i < d->items.len; i++) {
        if (d->items.data[i].kind == EMBI_DI_LAYER_BEGIN && d->items.data[i].sub == sub) {
            return &d->items.data[i];
        }
    }
    return nullptr;
}

/* The number of COMPOSITE groups with the blend mode. */
static uint32_t blend_groups(const embi_dl *d, uint32_t mode)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < d->items.len; i++) {
        const embi_ditem *it = &d->items.data[i];
        n += it->kind == EMBI_DI_LAYER_BEGIN && it->sub == EMBI_LS_COMPOSITE && it->colors[0] == mode;
    }
    return n;
}

EMB_TEST(r4e_blend_mode_and_isolation)
{
    ph h = ph_make("<svg width=100 height=100><rect width=50 height=50 style=\"mix-blend-mode:screen\"/><rect width=10 height=10 /></svg>", "");
    EMB_CHECK(blend_groups(&h.dl, EMBI_BM_SCREEN) == 1 && layer_count(&h.dl, EMBI_LS_COMPOSITE) == 1);
    EMB_CHECK(ph_count(&h.dl, EMBI_DI_PATH) == 2 && h.dl.fx_items == 1);
    ph_free(&h);
    /* an isolated group with nothing that blends below costs no layer */
    h = ph_make("<svg width=100 height=100><g style=\"isolation:isolate\"><rect width=50 height=50 /></g></svg>", "");
    EMB_CHECK(layer_count(&h.dl, EMBI_LS_COMPOSITE) == 0 && h.dl.fx_items == 0);
    ph_free(&h);
    /* with a blending descendant it is an isolated group, and so is a group reached through use */
    h = ph_make("<svg width=100 height=100><defs><rect id=r width=5 height=5 style=\"mix-blend-mode:multiply\"/></defs>"
                "<g style=\"isolation:isolate\"><rect width=50 height=50 style=\"mix-blend-mode:difference\"/></g>"
                "<g style=\"isolation:isolate\"><use href=\"#r\"/></g></svg>", "");
    EMB_CHECK(layer_count(&h.dl, EMBI_LS_COMPOSITE) == 4);
    EMB_CHECK(blend_groups(&h.dl, EMBI_BM_DIFFERENCE) == 1 && blend_groups(&h.dl, EMBI_BM_MULTIPLY) == 1);
    const embi_ditem *iso = first_layer(&h.dl, EMBI_LS_COMPOSITE, 0);
    EMB_REQUIRE(iso != nullptr);
    EMB_CHECK(iso->colors[0] == EMBI_BM_NORMAL && (iso->flags & EMBI_DF_ISOLATE) != 0);
    ph_free(&h);
    /* a blend mode inside a clip or mask content is not applied */
    h = ph_make("<svg width=100 height=100><defs><clipPath id=c><rect width=50 height=50 style=\"mix-blend-mode:screen\"/></clipPath></defs>"
                "<rect width=100 height=100 clip-path=\"url(#c)\"/></svg>", "");
    EMB_CHECK(blend_groups(&h.dl, EMBI_BM_SCREEN) == 0);
    ph_free(&h);
}

EMB_TEST(r4e_css_clip_path_shape_on_svg)
{
    ph h = ph_make("<svg width=100 height=100><rect width=50 height=50 style=\"clip-path:circle(20px)\"/>"
                   "<rect width=50 height=50 style=\"clip-path:fill-box\"/></svg>", "");
    EMB_CHECK(layer_count(&h.dl, EMBI_LS_COMPOSITE) == 2 && h.dl.cpaths.len == 2);
    const embi_ditem *g = first_layer(&h.dl, EMBI_LS_COMPOSITE, 0);
    EMB_REQUIRE(g != nullptr && g->colors[1] == 1);
    /* the circle is resolved against the stroke box (the rect): its center is at 25, 25 */
    EMB_CHECK(h.dl.cpaths.data[0].kind == EMBI_CP_PATH || h.dl.cpaths.data[0].kind == EMBI_CP_RRECT);
    EMB_CHECK(h.dl.cpaths.data[0].bounds[0] > 4.0f && h.dl.cpaths.data[0].bounds[2] < 46.0f);
    EMB_CHECK(h.dl.cpaths.data[1].kind == EMBI_CP_RRECT && near(h.dl.cpaths.data[1].rect[2], 50));
    ph_free(&h);
    /* a box with no area clips everything away */
    h = ph_make("<svg width=100 height=100><rect width=0 height=50 style=\"clip-path:fill-box\"/></svg>", "");
    EMB_CHECK(ph_count(&h.dl, EMBI_DI_PATH) == 0);
    ph_free(&h);
}

EMB_TEST(r4e_css_mask_image_on_svg_elements)
{
    ph h = ph_make("<svg width=100 height=100><rect width=50 height=50 style=\"mask-image:linear-gradient(red, transparent)\"/></svg>", "");
    const embi_ditem *g = first_layer(&h.dl, EMBI_LS_COMPOSITE, 0);
    EMB_REQUIRE(g != nullptr && g->colors[2] == 1);
    uint32_t gi = (uint32_t)(g - h.dl.items.data);
    const embi_ditem *ml = first_layer(&h.dl, EMBI_LS_MASK, gi);
    EMB_REQUIRE(ml != nullptr);
    uint32_t mi = (uint32_t)(ml - h.dl.items.data);
    EMB_REQUIRE(ml->aux > mi + 1 && h.dl.items.data[mi + 1].kind == EMBI_DI_GRADIENT);
    /* the reference box is the element's box: the gradient tile is 50 x 50 at the origin of its user space */
    EMB_CHECK(near(h.dl.items.data[mi + 1].rect[2], 50) && near(h.dl.items.data[mi + 1].rect[3], 50));
    EMB_CHECK(ph_count(&h.dl, EMBI_DI_PATH) == 1);
    ph_free(&h);
}

EMB_TEST(r4e_mask_references_of_svg_elements)
{
    /* a mask that does not resolve masks everything away; one in a tree that is not rendered is disabled */
    ph h = ph_make("<svg width=100 height=100><rect width=50 height=50 mask=\"url(#nope)\"/></svg>", "");
    EMB_CHECK(ph_count(&h.dl, EMBI_DI_PATH) == 0);
    ph_free(&h);
    h = ph_make("<svg style=\"display:none\"><mask id=m><rect width=10 height=10 fill=white /></mask></svg>"
                "<svg width=100 height=100><rect width=50 height=50 mask=\"url(#m)\"/></svg>", "");
    EMB_CHECK(ph_count(&h.dl, EMBI_DI_PATH) == 1 && layer_count(&h.dl, EMBI_LS_SVGMASK) == 0);
    ph_free(&h);
    /* an unresolved clip-path clips nothing */
    h = ph_make("<svg width=100 height=100><rect width=50 height=50 clip-path=\"url(#nope)\"/></svg>", "");
    EMB_CHECK(ph_count(&h.dl, EMBI_DI_PATH) == 1);
    ph_free(&h);
}

EMB_TEST(r4e_css_mask_and_clip_path_references_of_html_boxes)
{
    const char *defs = "<svg width=10 height=10><defs><mask id=m><rect width=20 height=20 fill=white /></mask>"
                       "<clipPath id=c><rect width=10 height=10 /></clipPath></defs></svg>";
    char html[1024];
    SDL_snprintf(html, sizeof html, "%s<div style=\"width:50px;height:50px;background:red;mask:url(#m)\"></div>", defs);
    ph h = ph_make(html, "");
    const embi_ditem *g = first_layer(&h.dl, EMBI_LS_COMPOSITE, 0);
    EMB_REQUIRE(g != nullptr);
    EMB_CHECK(g->colors[2] == 1);
    uint32_t gi = (uint32_t)(g - h.dl.items.data);
    const embi_ditem *ml = first_layer(&h.dl, EMBI_LS_MASK, gi);
    EMB_REQUIRE(ml != nullptr && ml->colors[0] == EMBI_MM_ALPHA);
    uint32_t mi = (uint32_t)(ml - h.dl.items.data);
    EMB_CHECK(ml->aux > mi + 1 && h.dl.items.data[mi + 1].kind == EMBI_DI_PATH); /* the rect of the mask is the layer */
    ph_free(&h);
    /* clip-path: url(#c) is the top mask layer, intersected with the mask-image layers */
    SDL_snprintf(html, sizeof html, "%s<div style=\"width:50px;height:50px;background:red;clip-path:url(#c);mask:url(#m)\"></div>", defs);
    h = ph_make(html, "");
    g = first_layer(&h.dl, EMBI_LS_COMPOSITE, 0);
    EMB_REQUIRE(g != nullptr && g->colors[2] == 2);
    gi = (uint32_t)(g - h.dl.items.data);
    ml = first_layer(&h.dl, EMBI_LS_MASK, gi);
    EMB_REQUIRE(ml != nullptr);
    EMB_CHECK(ml->colors[1] == EMBI_MC_INTERSECT && ml->colors[0] == EMBI_MM_ALPHA);
    ph_free(&h);
    /* a mask that names nothing is a transparent layer; a clip-path that names nothing clips nothing */
    SDL_snprintf(html, sizeof html, "%s<div style=\"width:50px;height:50px;background:red;mask:url(#nope)\"></div>", defs);
    h = ph_make(html, "");
    g = first_layer(&h.dl, EMBI_LS_COMPOSITE, 0);
    EMB_REQUIRE(g != nullptr && g->colors[2] == 1);
    gi = (uint32_t)(g - h.dl.items.data);
    ml = first_layer(&h.dl, EMBI_LS_MASK, gi);
    EMB_CHECK(ml != nullptr && ml->aux == (uint32_t)(ml - h.dl.items.data) + 1);
    ph_free(&h);
    SDL_snprintf(html, sizeof html, "%s<div style=\"width:50px;height:50px;background:red;clip-path:url(#nope)\"></div>", defs);
    h = ph_make(html, "");
    g = first_layer(&h.dl, EMBI_LS_COMPOSITE, 0);
    EMB_CHECK(g == nullptr || g->colors[2] == 0);
    EMB_CHECK(first_layer(&h.dl, EMBI_LS_MASK, 0) == nullptr);
    ph_free(&h);
    /* a mask element of a tree that is not rendered is disabled: no layer */
    h = ph_make("<svg style=\"display:none\"><mask id=m><rect width=20 height=20 fill=white /></mask></svg>"
                "<div style=\"width:50px;height:50px;background:red;mask:url(#m)\"></div>", "");
    g = first_layer(&h.dl, EMBI_LS_COMPOSITE, 0);
    EMB_CHECK(g == nullptr || g->colors[2] == 0);
    ph_free(&h);
}

EMB_TEST(r4e_text_stroke_shadow_and_visibility)
{
    ph h = ph_make("<svg width=200 height=60><text x=10 y=40 font-size=20 fill=red stroke=blue "
                   "style=\"text-shadow:2px 2px 0 #00f, 0 0 4px #0f0\">A</text></svg>", "");
    /* the sharp shadow, the blurred one in a filtered group, and the text: each with its stroke */
    EMB_CHECK(ph_count(&h.dl, EMBI_DI_TEXT) == 3 && ph_count(&h.dl, EMBI_DI_PATH) == 3);
    EMB_CHECK(layer_count(&h.dl, EMBI_LAYER_FILTERED) == 1 && h.dl.fx.len == 1);
    EMB_REQUIRE(h.dl.fx.len == 1);
    EMB_CHECK(h.dl.fx.data[0].kind == EMBI_FX_BLUR && near(h.dl.fx.data[0].sigma, 2.0));
    /* the last shadow is drawn first, below the others; the text is last */
    int i = ph_find(&h.dl, 0, EMBI_DI_TEXT), last = i;
    EMB_REQUIRE(i >= 0);
    EMB_CHECK(h.dl.items.data[i].colors[0] == 0xFF00FF00u && near(h.dl.items.data[i].f[0], 10));
    for (; i >= 0; i = ph_find(&h.dl, (uint32_t)i + 1, EMBI_DI_TEXT)) {
        last = i;
    }
    EMB_CHECK(h.dl.items.data[last].colors[0] == 0xFF0000FFu && near(h.dl.items.data[last].f[0], 10) && near(h.dl.items.data[last].f[1], 40));
    ph_free(&h);
    /* stroke only: no text item, the outline is stroked */
    h = ph_make("<svg width=200 height=60><text x=10 y=40 font-size=20 fill=none stroke=blue stroke-width=2>A</text></svg>", "");
    EMB_CHECK(ph_count(&h.dl, EMBI_DI_TEXT) == 0 && ph_count(&h.dl, EMBI_DI_PATH) == 1);
    int pi = ph_find(&h.dl, 0, EMBI_DI_PATH);
    EMB_REQUIRE(pi >= 0);
    EMB_CHECK((h.dl.items.data[pi].flags & EMBI_DF_STROKE) != 0 && near(h.dl.items.data[pi].f[0], 2));
    ph_free(&h);
    /* a visible tspan inside hidden text is drawn */
    h = ph_make("<svg width=200 height=60><text x=10 y=40 font-size=20 fill=red visibility=hidden>"
                "<tspan visibility=visible>X</tspan></text></svg>", "");
    EMB_CHECK(ph_count(&h.dl, EMBI_DI_TEXT) == 1);
    ph_free(&h);
}

/* The effects groups, the clip-path records and the mask layers leave nothing behind. */
EMB_TEST(r4e_no_leak)
{
    const char *html = "<svg width=100 height=100><defs><mask id=m><rect width=20 height=20 fill=white /></mask>"
                       "<clipPath id=c><rect width=10 height=10 /></clipPath></defs>"
                       "<g style=\"isolation:isolate\"><rect width=50 height=50 style=\"mix-blend-mode:screen;clip-path:circle(10px)\"/></g>"
                       "<text x=10 y=40 font-size=20 stroke=blue style=\"text-shadow:0 0 4px #0f0\">A</text></svg>"
                       "<div style=\"width:50px;height:50px;background:red;clip-path:url(#c);mask:url(#m)\"></div>";
    ph warm = ph_make(html, "");
    ph_free(&warm);
    size_t live = embi_mem_live();
    ph h = ph_make(html, "");
    EMB_CHECK(h.dl.fx_items > 0);
    ph_free(&h);
    EMB_CHECK(embi_mem_live() == live);
}

EMB_TEST_MAIN()
