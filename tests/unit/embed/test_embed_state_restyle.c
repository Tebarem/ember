/*
 * CF1: the restyle of a user-action state change goes through the real input layer (state_flips and state_record in
 * src/embed/input.c): only the elements whose hover, active or focus state flipped are marked, and what the rules
 * reading that state reach from them. Each test drives a view with the pointer and the keyboard, under the style
 * engine's verification (every partial resolve is compared with a full one, counting mode), and checks the computed
 * styles a page script reads. Also: a style sheet rewritten with the same text keeps custom properties readable (the
 * interned custom property maps own their tokens). Test-only; skipped without a device.
 */
#include "input_rig.h"

#include "style/style.h"

#define SETUP(html)                                                                                                    \
    emb_runtime *rt = emb_runtime_create(nullptr);                                                                     \
    EMB_REQUIRE(rt != nullptr);                                                                                        \
    embi_gpu *g = nullptr;                                                                                             \
    emb_view *v = nullptr;                                                                                             \
    embi_incr_set_verify(2);                                                                                           \
    const uint32_t base_fail = embi_incr_verify_failures();                                                            \
    OPEN_HTML_OR_SKIP(v, rt, g, html)

#define DONE()                                                                                                         \
    do {                                                                                                               \
        EMB_CHECK(embi_incr_verify_failures() == base_fail);                                                           \
        rig_close(v, g, rt);                                                                                           \
        embi_incr_set_verify(0);                                                                                       \
    } while (0)

/* The pointer moves to (x, y) and the view's next frame runs its style pass. */
static void move(emb_view *v, float x, float y)
{
    emb_view_mouse_move(v, x, y, 0);
    (void)embi_view_update(v);
}

static void click_at(emb_view *v, float x, float y)
{
    emb_view_mouse_move(v, x, y, 0);
    emb_view_mouse_button(v, x, y, EMB_BUTTON_PRIMARY, true, 0);
    emb_view_mouse_button(v, x, y, EMB_BUTTON_PRIMARY, false, 0);
}

static void update(emb_view *v)
{
    (void)embi_view_update(v);
}

/* The computed property prop of the element with the id. */
static const char *cs(emb_view *v, const char *id, const char *prop)
{
    char js[256];
    SDL_snprintf(js, sizeof js, "getComputedStyle(document.getElementById('%s')).getPropertyValue('%s')", id, prop);
    return run(v, js);
}

#define CS_IS(id, prop, want) EMB_CHECK_EQ_STR(cs(v, (id), (prop)), (want))

static const char k_boxes_css[] =
    "<!doctype html><style>body{margin:0;color:rgb(0,0,0)} .w{position:absolute;left:0;width:200px;height:60px}"
    " #w1{top:0} #w2{top:100px} .x{height:30px} .x:hover{color:rgb(255,0,0)} .w:hover .k{color:rgb(0,0,255)}"
    " .w2:hover .x{background-color:rgb(0,255,0)}</style>";

/* The hovered element is removed, then the pointer moves; the hovered element is moved under another parent, then the
 * pointer moves (the chains the input layer kept no longer hold: the whole document is restyled). */
EMB_TEST(hovered_element_removed_or_reparented)
{
    char html[1024];
    SDL_snprintf(html, sizeof html,
                 "%s<body><div class=w id=w1><div class=x id=a>a</div><span class=k id=k1>k</span></div>"
                 "<div class='w w2' id=w2><div class=x id=b>b</div><span class=k id=k2>k</span></div>",
                 k_boxes_css);
    SETUP(html);
    move(v, 50, 10);
    CS_IS("a", "color", "rgb(255, 0, 0)");
    CS_IS("k1", "color", "rgb(0, 0, 255)");
    CS_IS("k2", "color", "rgb(0, 0, 0)");
    (void)run(v, "document.getElementById('a').remove(); true");
    update(v);
    move(v, 50, 40);
    CS_IS("k1", "color", "rgb(0, 0, 255)");
    move(v, 50, 110);
    CS_IS("b", "color", "rgb(255, 0, 0)");
    CS_IS("b", "background-color", "rgb(0, 255, 0)");
    CS_IS("k1", "color", "rgb(0, 0, 0)");
    CS_IS("k2", "color", "rgb(0, 0, 255)");
    /* b (hovered) moves under w1: w2 is no longer an ancestor of the hovered element */
    (void)run(v, "document.getElementById('w1').appendChild(document.getElementById('b')); true");
    update(v);
    move(v, 60, 110);
    move(v, 50, 10);
    CS_IS("w2", "color", "rgb(0, 0, 0)");
    move(v, 250, 190);
    CS_IS("b", "color", "rgb(0, 0, 0)");
    CS_IS("k1", "color", "rgb(0, 0, 0)");
    CS_IS("k2", "color", "rgb(0, 0, 0)");
    DONE();
}

/* The focus moves A -> B -> a button -> nothing, by click and by Tab, with :focus, :focus-visible and :focus-within
 * rules on the inputs, on their groups and on the form. */
EMB_TEST(focus_moves_with_focus_within_on_the_ancestors)
{
    SETUP("<!doctype html><style>body{margin:0;color:rgb(0,0,0)} input{display:block;margin:0;width:100px;height:20px;border:0;padding:0}"
          " #f:focus-within{color:rgb(1,2,3)} .g:focus-within .lab{color:rgb(0,0,255)} input:focus{background-color:"
          "rgb(255,0,0)} input:focus-visible{outline-color:rgb(0,255,0)} :focus-visible + .after{color:rgb(9,8,7)}"
          " .g{height:40px}</style>"
          "<form id=f><div class=g id=g1><input id=i1><span class=lab id=l1>l</span></div>"
          "<div class=g id=g2><input id=i2><span class='lab after' id=l2>l</span></div></form>"
          "<button id=out style='display:block;height:20px'>o</button>");
    click_at(v, 20, 10);
    update(v);
    EMB_CHECK_EQ_STR(run(v, "document.activeElement.id"), "i1");
    CS_IS("f", "color", "rgb(1, 2, 3)");
    CS_IS("l1", "color", "rgb(0, 0, 255)");
    CS_IS("l2", "color", "rgb(1, 2, 3)");
    CS_IS("i1", "background-color", "rgb(255, 0, 0)");
    rig_tap_key(v, "Tab", "Tab", 0);
    update(v);
    EMB_CHECK_EQ_STR(run(v, "document.activeElement.id"), "i2");
    CS_IS("l1", "color", "rgb(1, 2, 3)");
    CS_IS("l2", "color", "rgb(0, 0, 255)");
    CS_IS("i1", "background-color", "rgba(0, 0, 0, 0)");
    CS_IS("i2", "background-color", "rgb(255, 0, 0)");
    CS_IS("i2", "outline-color", "rgb(0, 255, 0)");
    rig_tap_key(v, "Tab", "Tab", 0);
    update(v);
    EMB_CHECK_EQ_STR(run(v, "document.activeElement.id"), "out");
    CS_IS("f", "color", "rgb(0, 0, 0)");
    CS_IS("l2", "color", "rgb(0, 0, 0)");
    click_at(v, 250, 190);
    update(v);
    CS_IS("f", "color", "rgb(0, 0, 0)");
    /* a script moves the focus, and the focused input is removed */
    (void)run(v, "document.getElementById('i1').focus(); true");
    update(v);
    CS_IS("l1", "color", "rgb(0, 0, 255)");
    (void)run(v, "document.getElementById('i1').remove(); true");
    update(v);
    CS_IS("f", "color", "rgb(0, 0, 0)");
    CS_IS("l1", "color", "rgb(0, 0, 0)");
    DONE();
}

/* A press while hovering: :active on the pressed element and on its ancestors, then the pointer leaves while the button
 * is down, then the release. */
EMB_TEST(press_while_hovering)
{
    char html[1024];
    SDL_snprintf(html, sizeof html,
                 "%s<style>.w:active .k{font-weight:700} #a:hover:active{color:rgb(5,5,5)} .w:active{color:"
                 "rgb(7,7,7)}</style><body><div class=w id=w1><div class=x id=a>a</div><span class=k id=k1>k</span>"
                 "</div><div class='w w2' id=w2><div class=x id=b>b</div><span class=k id=k2>k</span></div>",
                 k_boxes_css);
    SETUP(html);
    move(v, 50, 10);
    emb_view_mouse_button(v, 50, 10, EMB_BUTTON_PRIMARY, true, 0);
    update(v);
    CS_IS("a", "color", "rgb(5, 5, 5)");
    CS_IS("w1", "color", "rgb(7, 7, 7)");
    CS_IS("k1", "font-weight", "700");
    CS_IS("k2", "font-weight", "400");
    move(v, 50, 110); /* the press stays on a; the hover moves to b */
    CS_IS("a", "color", "rgb(7, 7, 7)");
    CS_IS("b", "color", "rgb(255, 0, 0)");
    CS_IS("k1", "font-weight", "700");
    emb_view_mouse_button(v, 50, 110, EMB_BUTTON_PRIMARY, false, 0);
    update(v);
    CS_IS("w1", "color", "rgb(0, 0, 0)");
    CS_IS("k1", "font-weight", "400");
    CS_IS("b", "color", "rgb(255, 0, 0)");
    DONE();
}

/* Rules that read the hover of a sibling or of a descendant: .a:hover + .b, .a:hover ~ .c, :has(.a:hover), and
 * :nth-child(1 of :hover) (S read on the siblings). */
EMB_TEST(hover_read_by_siblings_has_and_nth_of)
{
    SETUP("<!doctype html><style>body{margin:0;color:rgb(0,0,0)} .r{height:30px;width:200px} .a:hover + .b{color:rgb(255,0,0)}"
          " .a:hover ~ .c{color:rgb(0,0,255)} .p:has(.a:hover) .q{color:rgb(0,128,0)}"
          " ul{position:absolute;left:220px;top:0;width:70px;margin:0;padding:0;list-style:none} li{height:30px}"
          " li:nth-child(2 of :not(:hover)){font-weight:700} li:nth-last-child(1 of :is(.z, :not(:hover))) span{"
          "color:rgb(3,3,3)}</style>"
          "<div class=p id=p><div class='r a' id=a1>a</div><div class='r b' id=b1>b</div><div class='r c' id=c1>c</div>"
          "<div class='r a' id=a2>a</div><div class='r b c' id=b2>b</div><div class='r q' id=q2>q</div></div>"
          "<div class=p id=qs><span class=q id=q0>q</span></div>"
          "<ul><li id=l1><span id=s1>1</span></li><li id=l2><span id=s2>2</span></li><li id=l3><span id=s3>3</span>"
          "</li></ul>");
    move(v, 50, 10); /* a1 */
    CS_IS("b1", "color", "rgb(255, 0, 0)");
    CS_IS("c1", "color", "rgb(0, 0, 255)");
    CS_IS("b2", "color", "rgb(0, 0, 255)");
    CS_IS("q2", "color", "rgb(0, 128, 0)");
    CS_IS("q0", "color", "rgb(0, 0, 0)");
    move(v, 50, 100); /* a2 */
    CS_IS("b1", "color", "rgb(0, 0, 0)");
    CS_IS("c1", "color", "rgb(0, 0, 0)");
    CS_IS("b2", "color", "rgb(0, 0, 255)"); /* + .b and ~ .c both match: the later rule wins */
    CS_IS("q2", "color", "rgb(0, 128, 0)");
    move(v, 50, 40); /* b1: no .a hovered */
    CS_IS("b2", "color", "rgb(0, 0, 0)");
    CS_IS("q2", "color", "rgb(0, 0, 0)");
    /* li:nth-child(2 of :not(:hover)): the second li that is not hovered, so a hover on l1 moves the match from l2 to
     * l3, whose own state did not flip. li:nth-last-child(1 of :is(.z, :not(:hover))) span: the last such li's span. */
    CS_IS("l2", "font-weight", "700");
    CS_IS("s3", "color", "rgb(3, 3, 3)");
    move(v, 250, 10); /* li#l1 */
    CS_IS("l2", "font-weight", "400");
    CS_IS("l3", "font-weight", "700");
    CS_IS("s3", "color", "rgb(3, 3, 3)");
    move(v, 250, 70); /* li#l3 */
    CS_IS("l2", "font-weight", "700");
    CS_IS("l3", "font-weight", "400");
    CS_IS("s3", "color", "rgb(0, 0, 0)");
    CS_IS("s2", "color", "rgb(3, 3, 3)");
    (void)run(v, "document.getElementById('l3').className = 'z'; true");
    update(v);
    CS_IS("s2", "color", "rgb(0, 0, 0)");
    CS_IS("s3", "color", "rgb(3, 3, 3)");
    move(v, 150, 190);
    CS_IS("l2", "font-weight", "700");
    CS_IS("l3", "font-weight", "400");
    DONE();
}

/* A style sheet rewritten with the same text (a framework re-rendering a style element), a style attribute switched
 * between two values, a sheet replaced by different text and back, a constructed sheet replaced: the custom properties
 * and what var() reads of them stay right (run under ASan on the box). The interned maps own copies of their tokens; the
 * memory they were copied from (the sheets, which the engine keeps until it is destroyed, and the style attributes'
 * declarations in the engine arena) does not go away under them today, so this passes with round 1's shallow maps too. */
EMB_TEST(same_text_sheet_rewrite_keeps_custom_properties)
{
    SETUP("<!doctype html><style id=s>:root{--a:1px;--fn:calc(2px + 3px);--list:a b c} #t{padding-left:var(--a);"
          "margin-left:var(--fn)}</style><body><div id=t style='--b: 4px'>t</div>");
    CS_IS("t", "--a", "1px");
    CS_IS("t", "padding-left", "1px");
    CS_IS("t", "margin-left", "5px");
    for (int i = 0; i < 4; i++) {
        (void)run(v, "var s = document.getElementById('s'); s.textContent = s.textContent + ''; true");
        update(v);
        CS_IS("t", "--a", "1px");
        CS_IS("t", "--fn", "calc(2px + 3px)");
        CS_IS("t", "--list", "a b c");
        CS_IS("t", "padding-left", "1px");
        CS_IS("t", "margin-left", "5px");
        (void)run(v, i % 2 == 0 ? "document.getElementById('t').setAttribute('style', '--b: 5px'); true"
                                : "document.getElementById('t').setAttribute('style', '--b: 4px'); true");
        update(v);
        CS_IS("t", "--b", i % 2 == 0 ? "5px" : "4px");
    }
    (void)run(v, "document.getElementById('s').textContent = ':root{--a:7px} #t{padding-left:var(--a)}'; true");
    update(v);
    CS_IS("t", "padding-left", "7px");
    (void)run(v, "document.getElementById('s').textContent = ':root{--a:1px;--fn:calc(2px + 3px);--list:a b c} "
                 "#t{padding-left:var(--a);margin-left:var(--fn)}'; true");
    update(v);
    CS_IS("t", "--a", "1px");
    CS_IS("t", "padding-left", "1px");
    /* a constructed sheet replaced with the same text */
    (void)run(v, "var c = new CSSStyleSheet(); c.replaceSync(':root{--z:9px}'); document.adoptedStyleSheets = [c];"
                 " true");
    update(v);
    CS_IS("t", "--z", "9px");
    (void)run(v, "c.replaceSync(':root{--z:9px}'); true");
    update(v);
    CS_IS("t", "--z", "9px");
    DONE();
}

RIG_MAIN()
