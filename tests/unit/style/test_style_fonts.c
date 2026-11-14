/*
 * @font-face through the engine into a real font registry: families, the unicode-range descriptor
 * (valid, invalid and overlong), the format() filter, and once-only registration per sheet.
 *
 * This TU includes the CSS and style headers only. The text headers collide with the CSS
 * declarations, so the three registry entry points used here are declared by hand (same signatures
 * as src/text/text.h).
 */
#include "emb_test.h"

#include "html/parser.h"
#include "style/style.h"

#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_stdinc.h>

#include <string.h>

embi_fontdb *embi_fontdb_create(void);
void embi_fontdb_destroy(embi_fontdb *db);
uint32_t embi_fontdb_face_count(const embi_fontdb *db);

typedef struct loader_state {
    uint32_t calls;
    uint32_t bytes_served;
} loader_state;

static bool load_font(void *user, embi_str base, embi_str url, uint8_t **data, size_t *len)
{
    loader_state *ls = user;
    (void)base;
    (void)url;
    ls->calls++;
    size_t n = 0;
    void *buf = SDL_LoadFile(EMBER_SOURCE_DIR "/tests/fonts/NotoSans-Regular.ttf", &n);
    if (buf == nullptr) {
        return false;
    }
    *data = buf;
    *len = n;
    ls->bytes_served += (uint32_t)n;
    return true;
}

static embi_document *parse_doc(const char *html)
{
    embi_document *doc = embi_document_new(true);
    embi_html_parser *p = embi_html_parser_new(doc, nullptr);
    embi_html_parser_write(p, html, strlen(html));
    embi_html_parser_end(p);
    embi_html_parser_destroy(p);
    return doc;
}

static embi_css_sheet *parse_css(const char *css)
{
    embi_css_hooks hooks = {nullptr, embi_prop_decl_ok, nullptr};
    embi_css_parse_opts opts = {nullptr, &hooks, EMBI_STR("")};
    return embi_css_sheet_parse((embi_str){css, (uint32_t)strlen(css)}, &opts);
}

/* A unicode-range with 65 single code points: one more than the registry accepts. */
static char *overlong_range_css(size_t *len_out)
{
    char *buf = SDL_malloc(64 * 1024);
    size_t len = 0;
    len += (size_t)SDL_snprintf(buf + len, 64 * 1024 - len,
                                "@font-face { font-family: Big; src: url(c.ttf); unicode-range: ");
    for (uint32_t i = 0; i < 65; i++) {
        len += (size_t)SDL_snprintf(buf + len, 64 * 1024 - len, "%sU+%X", i ? ", " : "", 0x100 + i);
    }
    len += (size_t)SDL_snprintf(buf + len, 64 * 1024 - len, "; }\n");
    *len_out = len;
    return buf;
}

EMB_TEST(font_faces_register_through_the_loader)
{
    embi_fontdb *fonts = embi_fontdb_create();
    EMB_REQUIRE(fonts != nullptr);
    loader_state ls = {0, 0};
    embi_style_callbacks cb = {&ls, load_font};
    embi_style_engine *eng = embi_style_engine_create(fonts, nullptr, &cb);
    EMB_REQUIRE(eng != nullptr);
    uint32_t faces0 = embi_fontdb_face_count(fonts);

    size_t big_len = 0;
    char *big = overlong_range_css(&big_len);
    char *css = SDL_malloc(4096 + big_len);
    size_t len = (size_t)SDL_snprintf(
        css, 4096,
        "@font-face { font-family: Test Face; src: url(a.ttf) format(\"truetype\"); "
        "font-weight: 100 900; unicode-range: U+0020-007E; }\n"
        /* An invalid unicode-range is ignored: the face still registers, covering everything. */
        "@font-face { font-family: \"Quoted\"; src: url(b.ttf); unicode-range: U+0020 nope; }\n"
        /* A format() the registry does not read drops the source: no load call. */
        "@font-face { font-family: Wrong; src: url(d.woff3) format(\"woff3\"); }\n"
        "@font-face { font-family: Open Sans; src: local(\"Open Sans\"), url(e.ttf); }\n");
    memcpy(css + len, big, big_len);
    len += big_len;
    css[len] = '\0';
    embi_css_sheet *sheet = parse_css(css);
    embi_css_sheet *sheets[1] = {sheet};

    embi_document *doc = parse_doc("<p>x</p>");
    embi_document_set_author_sheets(eng, doc, sheets, 1);
    embi_style_resolve_document(eng, doc);
    /* Test Face, Quoted, Open Sans (through url after a failing local) and Big. */
    EMB_CHECK_EQ_INT(ls.calls, 4);
    EMB_CHECK_EQ_INT(embi_fontdb_face_count(fonts) - faces0, 4);

    /* The same sheet attached again registers nothing new. */
    embi_document_set_author_sheets(eng, doc, sheets, 1);
    embi_style_resolve_document(eng, doc);
    EMB_CHECK_EQ_INT(ls.calls, 4);

    embi_style_forget_document(eng, doc);
    embi_document_free(doc);
    embi_css_sheet_unref(sheet);
    embi_style_engine_destroy(eng);
    embi_fontdb_destroy(fonts);
    SDL_free(big);
    SDL_free(css);
}

EMB_TEST_MAIN()
