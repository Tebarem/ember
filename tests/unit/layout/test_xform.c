/*
 * 4x4 box matrices (R3c): the helpers of layout/xform.h and the matrices built from styles.
 */
#include "layout_harness.h"

#include "layout/xform.h"

#include <stdio.h>
#include <math.h>

static bool near(double a, double b)
{
    return fabs(a - b) < 1e-6;
}

EMB_TEST(identity_and_multiply)
{
    embi_m4 a, b, c;
    embi_m4_identity(&a);
    embi_m4_identity(&b);
    a.m[3] = 5;
    b.m[7] = 7;
    embi_m4_mul(&a, &b, &c);
    double p[4];
    embi_m4_point(&c, 1, 2, 3, p);
    EMB_CHECK(near(p[0], 6) && near(p[1], 9) && near(p[2], 3) && near(p[3], 1));
}

EMB_TEST(plane_homography_and_inverse)
{
    embi_m4 a;
    embi_m4_identity(&a);
    a.m[0] = 2;
    a.m[3] = 10;
    a.m[14] = -0.01; /* perspective: w = 1 - z/100 for z, here w picks up z only */
    double h[9], inv[9], r[9];
    embi_m4_plane(&a, h);
    EMB_CHECK(near(h[0], 2) && near(h[2], 10) && near(h[8], 1));
    EMB_REQUIRE(embi_h3_invert(h, inv));
    embi_h3_mul(h, inv, r);
    EMB_CHECK(near(r[0], 1) && near(r[4], 1) && near(r[8], 1) && near(r[1], 0) && near(r[3], 0));
    double sing[9] = {1, 2, 3, 2, 4, 6, 0, 0, 1};
    EMB_CHECK(!embi_h3_invert(sing, inv));
}

EMB_TEST(singular_matrix_is_not_invertible)
{
    embi_m4 a;
    embi_m4_identity(&a);
    EMB_CHECK(embi_m4_invertible(&a));
    a.m[10] = 0;
    EMB_CHECK(!embi_m4_invertible(&a));
    embi_m4_identity(&a);
    a.m[3] = 1e6; /* a big translation does not make a matrix singular */
    EMB_CHECK(embi_m4_invertible(&a));
}

static bool box_m(const char *style, embi_m4 *m)
{
    char html[512];
    snprintf(html, sizeof html, "<body><div id=a style='width:100px;height:50px;%s'></div></body>", style);
    lh h = lh_make(html, "");
    embi_element *el = lh_id(&h, "a");
    const embi_style *s = el != nullptr ? embi_style_get(h.style, el, EMBI_PE_NONE) : nullptr;
    float bb[4] = {0, 0, 100, 50};
    bool r = el != nullptr && s != nullptr && embi_lay_box_matrix(s, bb, 800, 600, 16.0f, m);
    lh_free(&h);
    return r;
}

EMB_TEST(translate3d_matrix)
{
    embi_m4 m;
    EMB_REQUIRE(box_m("transform:translate3d(10px,20px,30px)", &m));
    double p[4];
    embi_m4_point(&m, 0, 0, 0, p);
    EMB_CHECK(near(p[0], 10) && near(p[1], 20) && near(p[2], 30));
}

EMB_TEST(rotate_about_origin_keeps_the_origin)
{
    embi_m4 m;
    EMB_REQUIRE(box_m("transform:rotateY(90deg)", &m));
    double p[4];
    embi_m4_point(&m, 50, 25, 0, p); /* the center is the default origin */
    EMB_CHECK(near(p[0], 50) && near(p[1], 25));
    embi_m4_point(&m, 100, 25, 0, p);
    EMB_CHECK(near(p[0], 50) && near(fabs(p[2]), 50));
    EMB_CHECK(near(embi_h3_det((double[9]){0}), 0));
}

EMB_TEST(perspective_function_sets_w)
{
    embi_m4 m;
    EMB_REQUIRE(box_m("transform:perspective(100px) translateZ(50px)", &m));
    double p[4];
    embi_m4_point(&m, 50, 25, 0, p);
    EMB_CHECK(near(p[3], 0.5));
}

EMB_TEST(no_transform_gives_false)
{
    embi_m4 m;
    EMB_CHECK(!box_m("", &m));
}

EMB_TEST(preserve_3d_used_value)
{
    lh h = lh_make("<body><div id=a style='transform-style:preserve-3d'></div>"
                   "<div id=b style='transform-style:preserve-3d;opacity:.5'></div>"
                   "<div id=c style='transform-style:preserve-3d;overflow:hidden'></div></body>",
                   "");
    EMB_CHECK(embi_lay_preserve3d_used(embi_style_get(h.style, lh_id(&h, "a"), EMBI_PE_NONE)));
    EMB_CHECK(!embi_lay_preserve3d_used(embi_style_get(h.style, lh_id(&h, "b"), EMBI_PE_NONE)));
    EMB_CHECK(!embi_lay_preserve3d_used(embi_style_get(h.style, lh_id(&h, "c"), EMBI_PE_NONE)));
    lh_free(&h);
}

EMB_TEST(backface_hidden)
{
    lh h = lh_make("<body><div id=a style='backface-visibility:hidden'></div><div id=b></div></body>", "");
    EMB_CHECK(embi_lay_backface_hidden(embi_style_get(h.style, lh_id(&h, "a"), EMBI_PE_NONE)));
    EMB_CHECK(!embi_lay_backface_hidden(embi_style_get(h.style, lh_id(&h, "b"), EMBI_PE_NONE)));
    lh_free(&h);
}

EMB_TEST_MAIN()
