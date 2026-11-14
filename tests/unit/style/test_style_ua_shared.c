/*
 * FP2b: the UA stylesheet is parsed once per process and shared by every style engine. Checks that engines share one
 * parsed sheet, that the user count and the memory return to zero, that the flatten is lazy, that the sheet can be
 * taken from several threads at once, and that computed UA styles are the same for every engine.
 */
#include "emb_test.h"

#include "base/base.h"
#include "css/css.h"
#include "css/props.h"
#include "dom/dom.h"
#include "html/parser.h"
#include "style/style.h"

#include <string.h>

#include <SDL3/SDL_thread.h>

static embi_document *parse_doc(const char *html)
{
    embi_document *doc = embi_document_new(true);
    embi_html_parser *p = embi_html_parser_new(doc, nullptr);
    embi_html_parser_write(p, html, strlen(html));
    embi_html_parser_end(p);
    embi_html_parser_destroy(p);
    return doc;
}

/* The serialized computed value of a property of an element. */
static bool value_of(embi_style_engine *eng, embi_element *el, const char *prop, char *out, size_t cap)
{
    out[0] = '\0';
    if (el == nullptr) {
        return false;
    }
    const embi_style *s = embi_style_get(eng, el, EMBI_PE_NONE);
    embi_prop_id pid = embi_prop_lookup((embi_str){prop, (uint32_t)strlen(prop)});
    embi_sb sb = {nullptr, 0, 0};
    bool ok = s != nullptr && pid != EMBI_PROP_NONE && embi_style_value_text(s, pid, &sb);
    if (ok && sb.len < cap) {
        memcpy(out, sb.data, sb.len);
        out[sb.len] = '\0';
    } else {
        ok = false;
    }
    embi_sb_free(&sb);
    return ok;
}

/* The atom table and the other process-wide tables are filled by the first parse and kept: take the sheet once. */
static void warm_up(void)
{
    embi_style_ua_release(embi_style_ua_sheet());
    embi_style_engine *e = embi_style_engine_create(nullptr, nullptr, nullptr); /* the engine's layout tables */
    embi_document *d = parse_doc("<p>x</p>");
    embi_document_set_author_sheets(e, d, nullptr, 0);
    embi_style_resolve_document(e, d);
    embi_style_forget_document(e, d);
    embi_document_free(d);
    embi_style_engine_destroy(e);
}

EMB_TEST(engines_share_one_ua_sheet_and_release_it)
{
    warm_up();
    EMB_CHECK(embi_style_ua_users() == 0);
    size_t base = embi_mem_live();
    embi_style_engine *a = embi_style_engine_create(nullptr, nullptr, nullptr);
    embi_style_engine *b = embi_style_engine_create(nullptr, nullptr, nullptr);
    /* Lazy: creating an engine neither parses nor flattens the UA sheet. */
    EMB_CHECK(embi_style_ua_users() == 0);
    embi_style_ua_ensure(a);
    EMB_CHECK(embi_style_ua_users() == 1);
    embi_style_ua_ensure(b);
    EMB_CHECK(embi_style_ua_users() == 2);
    embi_style_ua_ensure(b); /* idempotent */
    EMB_CHECK(embi_style_ua_users() == 2);
    embi_css_sheet *s1 = embi_style_ua_sheet();
    embi_css_sheet *s2 = embi_style_ua_sheet();
    EMB_CHECK(s1 == s2); /* the same parsed sheet, not a copy per caller */
    embi_style_ua_release(s1);
    embi_style_ua_release(s2);
    EMB_CHECK(embi_style_ua_users() == 2);
    embi_style_engine_destroy(a);
    EMB_CHECK(embi_style_ua_users() == 1);
    embi_style_engine_destroy(b);
    EMB_CHECK(embi_style_ua_users() == 0);
    EMB_CHECK(embi_mem_live() == base);
}

EMB_TEST(engine_destroyed_before_any_use_never_parses_the_ua_sheet)
{
    warm_up();
    size_t base = embi_mem_live();
    embi_style_engine *e = embi_style_engine_create(nullptr, nullptr, nullptr);
    embi_style_engine_destroy(e);
    EMB_CHECK(embi_style_ua_users() == 0);
    EMB_CHECK(embi_mem_live() == base);
}

typedef struct grab {
    embi_css_sheet *sheet;
} grab;

static int SDLCALL grab_main(void *arg)
{
    grab *g = arg;
    g->sheet = embi_style_ua_sheet();
    return 0;
}

EMB_TEST(ua_sheet_taken_from_several_threads_is_one_sheet)
{
    warm_up();
    size_t base = embi_mem_live();
    enum { N = 4 };
    grab g[N];
    SDL_Thread *t[N];
    memset(g, 0, sizeof g);
    for (int i = 0; i < N; i++) {
        t[i] = SDL_CreateThread(grab_main, "ua-grab", &g[i]);
        EMB_REQUIRE(t[i] != nullptr);
    }
    for (int i = 0; i < N; i++) {
        SDL_WaitThread(t[i], nullptr);
    }
    EMB_CHECK(embi_style_ua_users() == N);
    for (int i = 0; i < N; i++) {
        EMB_CHECK(g[i].sheet != nullptr && g[i].sheet == g[0].sheet);
    }
    for (int i = 0; i < N; i++) {
        embi_style_ua_release(g[i].sheet);
    }
    EMB_CHECK(embi_style_ua_users() == 0);
    EMB_CHECK(embi_mem_live() == base);
}

EMB_TEST(ua_styles_are_the_same_for_every_engine_and_survive_destroy_order)
{
    const char *html = "<body><h1 id=h>x</h1><p id=p>y</p><ul><li id=l>z</li></ul><a id=a href=x>l</a></body>";
    embi_style_engine *a = embi_style_engine_create(nullptr, nullptr, nullptr);
    embi_style_engine *b = embi_style_engine_create(nullptr, nullptr, nullptr);
    embi_document *da = parse_doc(html);
    embi_document *db = parse_doc(html);
    embi_document_set_author_sheets(a, da, nullptr, 0);
    embi_document_set_author_sheets(b, db, nullptr, 0);
    embi_style_resolve_document(a, da);
    /* The first engine goes while the second still uses the shared sheet. */
    embi_style_forget_document(a, da);
    embi_document_free(da);
    embi_style_engine_destroy(a);
    embi_style_resolve_document(b, db);
    static const char *const ids[] = {"h", "p", "l", "a"};
    static const char *const props[] = {"display", "margin-top", "font-size", "font-weight", "list-style-type", "color"};
    embi_style_engine *c = embi_style_engine_create(nullptr, nullptr, nullptr);
    embi_document *dc = parse_doc(html);
    embi_document_set_author_sheets(c, dc, nullptr, 0);
    embi_style_resolve_document(c, dc);
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        for (size_t k = 0; k < sizeof props / sizeof props[0]; k++) {
            char vb[128], vc[128];
            embi_element *eb = embi_get_element_by_id(db, (embi_str){ids[i], (uint32_t)strlen(ids[i])});
            embi_element *ec = embi_get_element_by_id(dc, (embi_str){ids[i], (uint32_t)strlen(ids[i])});
            EMB_REQUIRE(eb != nullptr && ec != nullptr);
            EMB_CHECK(value_of(b, eb, props[k], vb, sizeof vb));
            EMB_CHECK(value_of(c, ec, props[k], vc, sizeof vc));
            EMB_CHECK(strcmp(vb, vc) == 0);
        }
    }
    {
        char v[64];
        EMB_CHECK(value_of(b, embi_get_element_by_id(db, EMBI_STR("h")), "display", v, sizeof v));
        EMB_CHECK(strcmp(v, "block") == 0);
        EMB_CHECK(value_of(b, embi_get_element_by_id(db, EMBI_STR("h")), "font-weight", v, sizeof v));
        EMB_CHECK(strcmp(v, "700") == 0 || strcmp(v, "bold") == 0);
    }
    embi_style_forget_document(b, db);
    embi_document_free(db);
    embi_style_forget_document(c, dc);
    embi_document_free(dc);
    embi_style_engine_destroy(c);
    embi_style_engine_destroy(b);
    EMB_CHECK(embi_style_ua_users() == 0);
}

EMB_TEST(env_change_before_first_use_flattens_the_ua_set_once_with_the_new_environment)
{
    embi_style_env env;
    memset(&env, 0, sizeof env);
    env.width = 400;
    env.height = 300;
    env.dppx = 1.0;
    env.scripting = true;
    env.hover = true;
    env.pointer = 2;
    env.update = 2;
    embi_style_engine *e = embi_style_engine_create(nullptr, &env, nullptr);
    env.width = 1200;
    embi_style_engine_set_env(e, &env);
    EMB_CHECK(embi_style_ua_users() == 0); /* still lazy */
    embi_document *d = parse_doc("<body><p id=p>y</p></body>");
    embi_document_set_author_sheets(e, d, nullptr, 0);
    embi_style_resolve_document(e, d);
    EMB_CHECK(embi_style_ua_users() == 1);
    char v[64];
    EMB_CHECK(value_of(e, embi_get_element_by_id(d, EMBI_STR("p")), "display", v, sizeof v));
    EMB_CHECK(strcmp(v, "block") == 0);
    /* A later change re-flattens the (now flattened) UA set without a second user. */
    env.width = 300;
    embi_style_engine_set_env(e, &env);
    EMB_CHECK(embi_style_ua_users() == 1);
    embi_style_forget_document(e, d);
    embi_document_free(d);
    embi_style_engine_destroy(e);
    EMB_CHECK(embi_style_ua_users() == 0);
}

EMB_TEST_MAIN()
