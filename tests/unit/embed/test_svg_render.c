/*
 * SVG on the GPU (R4c): a page with shapes, strokes, a gradient, a clip path, a mask, a use of a symbol and an SVG image is
 * rendered through an offscreen view and checked at pixels and against a golden image.
 */
#include "unit/embed/crisp_rig.h"

#include <SDL3/SDL_init.h>

static const char k_page[] =
    "<!doctype html><body style=\"margin:0;background:#fff\">"
    "<svg width=\"240\" height=\"120\" style=\"display:block\">"
    "<defs>"
    "<linearGradient id=\"g\" x1=\"0\" x2=\"1\"><stop offset=\"0\" stop-color=\"#f00\"/><stop offset=\"1\" stop-color=\"#00f\"/></linearGradient>"
    "<clipPath id=\"c\"><circle cx=\"30\" cy=\"90\" r=\"12\"/></clipPath>"
    "<mask id=\"m\"><rect x=\"70\" y=\"70\" width=\"20\" height=\"40\" fill=\"#fff\"/></mask>"
    "<symbol id=\"s\" viewBox=\"0 0 10 10\"><rect width=\"10\" height=\"10\" fill=\"#f80\"/></symbol>"
    "</defs>"
    "<rect x=\"10\" y=\"10\" width=\"40\" height=\"40\" fill=\"#f00\"/>"
    "<circle cx=\"100\" cy=\"30\" r=\"20\" fill=\"none\" stroke=\"#00f\" stroke-width=\"4\"/>"
    "<rect x=\"150\" y=\"10\" width=\"80\" height=\"40\" fill=\"url(#g)\"/>"
    "<rect x=\"10\" y=\"70\" width=\"40\" height=\"40\" fill=\"#0a0\" clip-path=\"url(#c)\"/>"
    "<rect x=\"70\" y=\"70\" width=\"40\" height=\"40\" fill=\"#000\" mask=\"url(#m)\"/>"
    "<use href=\"#s\" x=\"130\" y=\"70\" width=\"40\" height=\"40\"/>"
    "<path d=\"M190 70 L230 70 L210 110 z\" fill=\"#909\" stroke=\"#000\" stroke-width=\"2\" stroke-linejoin=\"round\"/>"
    "</svg>"
    "<img width=\"40\" height=\"40\" style=\"display:block\" src=\"data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' "
    "width='20' height='20'%3E%3Crect width='20' height='20' fill='%2300c'/%3E%3C/svg%3E\">"
    "</body>";

#define W 240
#define H 170

EMB_TEST(svg_scene)
{
    crisp_page pg;
    if (!crisp_render_html(k_page, sizeof k_page - 1, W, H, EMB_TEXT_AA_GRAY, &pg)) {
        EMB_SKIP("no GPU device");
    }
    const uint8_t *px = pg.px;
#define AT(x, y) (px + ((size_t)(y) * W + (size_t)(x)) * 4)
    /* a filled rect */
    EMB_CHECK(AT(30, 30)[0] > 240 && AT(30, 30)[1] < 12 && AT(30, 30)[2] < 12);
    EMB_CHECK(AT(5, 30)[0] > 240 && AT(5, 30)[1] > 240); /* outside: white */
    /* a stroked circle: the ring is blue, the inside and the outside white */
    EMB_CHECK(AT(120, 30)[2] > 240 && AT(120, 30)[0] < 12);
    EMB_CHECK(AT(100, 30)[0] > 240 && AT(100, 30)[1] > 240 && AT(100, 30)[2] > 240);
    /* a gradient: red on the left, blue on the right */
    EMB_CHECK(AT(153, 30)[0] > 200 && AT(153, 30)[2] < 60);
    EMB_CHECK(AT(227, 30)[2] > 200 && AT(227, 30)[0] < 60);
    EMB_CHECK(AT(190, 30)[0] > 90 && AT(190, 30)[0] < 170 && AT(190, 30)[2] > 90 && AT(190, 30)[2] < 170);
    /* a clip path: green inside the circle, white at the corner of the rect */
    EMB_CHECK(AT(30, 90)[1] > 140 && AT(30, 90)[0] < 20);
    EMB_CHECK(AT(12, 72)[0] > 240 && AT(12, 72)[1] > 240 && AT(12, 72)[2] > 240);
    /* a mask: the masked half is black, the other half white */
    EMB_CHECK(AT(80, 90)[0] < 12 && AT(80, 90)[1] < 12);
    EMB_CHECK(AT(100, 90)[0] > 240 && AT(100, 90)[1] > 240);
    /* use of a symbol, scaled by its viewBox */
    EMB_CHECK(AT(150, 90)[0] > 240 && AT(150, 90)[1] > 110 && AT(150, 90)[1] < 150 && AT(150, 90)[2] < 12);
    /* a filled and stroked triangle */
    EMB_CHECK(AT(210, 80)[0] > 120 && AT(210, 80)[0] < 160 && AT(210, 80)[1] < 12 && AT(210, 80)[2] > 120);
    /* an SVG image of an img element, scaled from 20x20 to 40x40 */
    EMB_CHECK(AT(20, 140)[2] > 190 && AT(20, 140)[0] < 12);
#undef AT
    EMB_CHECK(emb_gold_check("svg_scene", pg.px, W, H));
    crisp_free(&pg);
}


/* Radial gradient with reflect, a pattern, dashes, markers (auto orient), a stroked polyline with round joins and a nested
 * svg with a viewBox and preserveAspectRatio. */
static const char k_page2[] =
    "<!doctype html><body style=\"margin:0;background:#fff\">"
    "<svg width=\"240\" height=\"120\" style=\"display:block\">"
    "<defs>"
    "<radialGradient id=\"rg\" cx=\"0.5\" cy=\"0.5\" r=\"0.25\" spreadMethod=\"reflect\"><stop offset=\"0\" stop-color=\"#00f\"/><stop offset=\"1\" stop-color=\"#ff0\"/></radialGradient>"
    "<pattern id=\"p\" width=\"20\" height=\"20\" patternUnits=\"userSpaceOnUse\"><circle cx=\"10\" cy=\"10\" r=\"6\" fill=\"#f00\"/></pattern>"
    "<marker id=\"m\" markerWidth=\"8\" markerHeight=\"8\" refX=\"4\" refY=\"4\" orient=\"auto\"><rect width=\"8\" height=\"8\" fill=\"#0a0\"/></marker>"
    "</defs>"
    "<rect x=\"10\" y=\"10\" width=\"60\" height=\"60\" fill=\"url(#rg)\"/>"
    "<rect x=\"90\" y=\"10\" width=\"60\" height=\"60\" fill=\"url(#p)\"/>"
    "<line x1=\"10\" y1=\"90\" x2=\"110\" y2=\"90\" stroke=\"#000\" stroke-width=\"6\" stroke-dasharray=\"10 10\"/>"
    "<path d=\"M130 90 L170 90 L170 110\" fill=\"none\" stroke=\"#000\" stroke-width=\"2\" marker-start=\"url(#m)\" marker-mid=\"url(#m)\" marker-end=\"url(#m)\"/>"
    "<polyline points=\"190,20 210,50 230,20\" fill=\"none\" stroke=\"#808\" stroke-width=\"10\" stroke-linejoin=\"round\" stroke-linecap=\"round\"/>"
    "<svg x=\"190\" y=\"70\" width=\"40\" height=\"40\" viewBox=\"0 0 10 20\" preserveAspectRatio=\"xMinYMid meet\"><rect width=\"10\" height=\"20\" fill=\"#f80\"/></svg>"
    "</svg></body>";

#define H2 120

EMB_TEST(svg_scene2)
{
    crisp_page pg;
    if (!crisp_render_html(k_page2, sizeof k_page2 - 1, W, H2, EMB_TEXT_AA_GRAY, &pg)) {
        EMB_SKIP("no GPU device");
    }
    const uint8_t *px = pg.px;
#define AT(x, y) (px + ((size_t)(y) * W + (size_t)(x)) * 4)
    /* the radial gradient: blue at the centre; the reflect repeats yellow-to-blue outward, so the corner is not the end stop */
    EMB_CHECK(AT(40, 40)[2] > 200 && AT(40, 40)[0] < 60);
    EMB_CHECK(AT(40 + 15, 40)[0] > 150 && AT(40 + 15, 40)[2] < 120); /* near the first radius: yellowish */
    /* the pattern: a red disc at the tile centre, white at the tile corner */
    EMB_CHECK(AT(110, 30)[0] > 240 && AT(110, 30)[1] < 12 && AT(110, 30)[2] < 12);
    EMB_CHECK(AT(101, 21)[0] > 240 && AT(101, 21)[1] > 240 && AT(101, 21)[2] > 240);
    /* dashes: black on the dash, white in the gap */
    EMB_CHECK(AT(15, 90)[0] < 12 && AT(15, 90)[1] < 12);
    EMB_CHECK(AT(25, 90)[0] > 240 && AT(25, 90)[1] > 240);
    /* markers: green boxes at the vertices, the black stroke between them */
    EMB_CHECK(AT(130, 90)[1] > 140 && AT(130, 90)[0] < 20);
    EMB_CHECK(AT(170, 110)[1] > 140 && AT(170, 110)[0] < 20);
    EMB_CHECK(AT(150, 90)[0] < 12 && AT(150, 90)[1] < 12);
    /* the round-joined polyline: the apex is covered by the join, the outside is white */
    EMB_CHECK(AT(210, 52)[0] > 100 && AT(210, 52)[1] < 20);
    EMB_CHECK(AT(210, 66)[0] > 240 && AT(210, 66)[1] > 240);
    /* the nested svg: the 10 x 20 viewBox in 40 x 40 is scaled by 2 and sits at the left (xMin), 20 wide */
    EMB_CHECK(AT(200, 90)[0] > 240 && AT(200, 90)[1] > 110 && AT(200, 90)[1] < 150);
    EMB_CHECK(AT(225, 90)[0] > 240 && AT(225, 90)[1] > 240); /* outside the 20 px wide content */
#undef AT
    EMB_CHECK(emb_gold_check("svg_scene2", pg.px, W, H2));
    crisp_free(&pg);
}

int main(void)
{
    const bool video = SDL_Init(SDL_INIT_VIDEO);
    const int rc = emb_test_run_all();
    if (video) {
        SDL_Quit();
    }
    return rc;
}
