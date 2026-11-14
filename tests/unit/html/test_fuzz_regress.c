/*
 * Regressions found by the libFuzzer harnesses (tests/fuzz, task M9a). Each input is the minimized
 * reproducer, as a byte array.
 *
 * - "</p>" or "</br>" with an HTML integration point (svg foreignObject, desc, title) or a MathML
 *   text integration point (mi, mo, mn, ms, mtext) as the current node looped forever: the tree
 *   builder popped nothing and sent the token through the dispatcher again, which sent it to the
 *   foreign content rules again. The token must be reprocessed by the insertion mode in HTML content.
 *
 * A hang fails these tests through the ctest timeout; the checks cover the tree that results.
 */
#include "emb_test.h"

#include "base/base.h"
#include "dom/dom.h"
#include "html/parser.h"
#include "html/serializer.h"

#include <string.h>

/* fuzz_html_doc input 00 3e: "<svg><foreignObject\n></p >" */
static const uint8_t kHangSvg[] = {'<', 's', 'v', 'g', '>', '<', 'f', 'o', 'r', 'e', 'i', 'g', 'n',
                                   'O', 'b', 'j', 'e', 'c', 't', '\n', '>', '<', '/', 'p', ' ', '>'};
/* "<math><mi></p >\n" */
static const uint8_t kHangMath[] = {'<', 'm', 'a', 't', 'h', '>', '<', 'm', 'i', '>', '<', '/', 'p', ' ', '>', '\n'};

static bool parse_is(const uint8_t *p, size_t n, const char *want);

static bool parse_is(const uint8_t *p, size_t n, const char *want)
{
    /* The first parse interns the element names in the global atom table; that is not a leak. */
    static bool warm;
    if (!warm) {
        warm = true;
        (void)parse_is(p, n, want);
    }
    size_t live = embi_mem_live();
    embi_document *doc = embi_document_new(true);
    embi_html_parser *hp = embi_html_parser_new(doc, nullptr);
    embi_html_parser_write(hp, p, n);
    embi_html_parser_end(hp);
    embi_html_parser_destroy(hp);
    embi_sb out = {0};
    embi_html_serialize_children(&doc->base, false, &out);
    bool ok = out.len == strlen(want) && memcmp(out.data, want, out.len) == 0;
    if (!ok) {
        SDL_Log("    got '%.*s'", (int)out.len, out.data);
    }
    embi_sb_free(&out);
    embi_document_free(doc);
    return ok && embi_mem_live() == live;
}

EMB_TEST(end_p_in_html_integration_point_terminates)
{
    EMB_CHECK(parse_is(kHangSvg, sizeof kHangSvg,
                       "<html><head></head><body><svg><foreignObject><p></p></foreignObject></svg></body></html>"));
}

EMB_TEST(end_p_in_mathml_text_integration_point_terminates)
{
    EMB_CHECK(parse_is(kHangMath, sizeof kHangMath,
                       "<html><head></head><body><math><mi><p></p>\n</mi></math></body></html>"));
}

EMB_TEST(end_br_in_integration_points_terminates)
{
    static const char a[] = "<svg><desc></br></desc></svg>";
    static const char b[] = "<svg><title></br></title></svg>";
    static const char c[] = "<math><mtext></br></mtext></math>";
    static const char d[] = "<math><annotation-xml encoding=text/html></p></annotation-xml></math>";
    EMB_CHECK(parse_is((const uint8_t *)a, sizeof a - 1,
                       "<html><head></head><body><svg><desc><br></desc></svg></body></html>"));
    EMB_CHECK(parse_is((const uint8_t *)b, sizeof b - 1,
                       "<html><head></head><body><svg><title><br></title></svg></body></html>"));
    EMB_CHECK(parse_is((const uint8_t *)c, sizeof c - 1,
                       "<html><head></head><body><math><mtext><br></mtext></math></body></html>"));
    EMB_CHECK(parse_is((const uint8_t *)d, sizeof d - 1,
                       "<html><head></head><body><math><annotation-xml encoding=\"text/html\"><p></p></annotation-xml></math></body></html>"));
}

EMB_TEST(end_p_in_nested_foreign_content_pops_to_the_html_element)
{
    static const char a[] = "<div><svg><g><circle></p></svg>";
    EMB_CHECK(parse_is((const uint8_t *)a, sizeof a - 1,
                       "<html><head></head><body><div><svg><g><circle></circle></g></svg><p></p></div></body></html>"));
}

EMB_TEST_MAIN()
