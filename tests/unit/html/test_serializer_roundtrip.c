/*
 * HTML serializer: fixed expected strings for escaping, void elements, raw text parents, template
 * contents, foreign elements, namespaced attributes, noscript with and without scripting, non-BMP
 * and lone-surrogate text, DOCTYPE, outerHTML of elements, fragments and documents, a deep tree,
 * and a round trip. The serializer writes no leading LF for pre, textarea or listing: the
 * current algorithm does not round-trip the LF that the parser drops.
 */
#include "emb_test.h"

#include "base/base.h"
#include "dom/dom.h"
#include "html/parser.h"
#include "html/serializer.h"

#include <SDL3/SDL_stdinc.h>

#include <string.h>

static const char kInput[] =
    "<!DOCTYPE html><title>t &amp; x</title>"
    "<p class=\"a&quot;&amp;b\" title='q&lt;&gt;'>x &amp; &lt;y&gt;&nbsp;<br>z"
    "<pre>\n\nw</pre><script>a<b</script><!--c-->";

/* The parser drops the first LF of pre, so the tree holds "\nw" and that is what is written. */
static const char kExpected[] =
    "<!DOCTYPE html><html><head><title>t &amp; x</title></head><body>"
    "<p class=\"a&quot;&amp;b\" title=\"q&lt;&gt;\">x &amp; &lt;y&gt;&nbsp;<br>z</p>"
    "<pre>\nw</pre><script>a<b</script><!--c--></body></html>";

static void parse_doc(const char *input, size_t n, embi_document *doc, bool scripting)
{
    embi_html_parser_config cfg = {0};
    cfg.scripting = scripting;
    embi_html_parser *p = embi_html_parser_new(doc, &cfg);
    embi_html_parser_write(p, input, n);
    embi_html_parser_end(p);
    embi_html_parser_destroy(p);
}

/* Parses input as a whole document and serializes its children into out. */
static void parse_and_serialize(const char *input, size_t n, embi_sb *out)
{
    embi_document *doc = embi_document_new(true);
    parse_doc(input, n, doc, false);
    embi_html_serialize_children(&doc->base, false, out);
    embi_document_free(doc);
}

/* Does the buffer contain s? The buffer is not assumed to be NUL-terminated. */
static bool sb_contains(const embi_sb *sb, const char *s)
{
    size_t n = strlen(s);
    for (size_t i = 0; i + n <= sb->len; i++) {
        if (memcmp(sb->data + i, s, n) == 0) {
            return true;
        }
    }
    return false;
}

static bool sb_is(const embi_sb *sb, const char *s)
{
    size_t n = strlen(s);
    return sb->len == n && memcmp(sb->data, s, n) == 0;
}

/* The element that is the first child of body (or nullptr). */
static embi_node *body_of(embi_document *doc)
{
    embi_node *html = doc->base.first_child;
    for (embi_node *c = html ? html->first_child : nullptr; c; c = c->next) {
        if (c->type == EMBI_NODE_ELEMENT && c->name == EMBI_ATOM_body) {
            return c;
        }
    }
    return nullptr;
}

EMB_TEST(serializer_fixed_output)
{
    embi_sb out = {nullptr, 0, 0};
    parse_and_serialize(kInput, sizeof kInput - 1, &out);
    EMB_CHECK(out.len == sizeof kExpected - 1);
    EMB_CHECK(memcmp(out.data, kExpected, out.len) == 0);
    embi_sb_free(&out);
}

EMB_TEST(serializer_round_trip_is_stable)
{
    const char in[] = "<p class=\"a&quot;\">x &amp; y<br><pre>w</pre><textarea>t</textarea>"
                      "<script>a<b</script><!--c--><svg viewBox=\"0 0 1 1\"><circle/></svg>";
    embi_sb first = {nullptr, 0, 0};
    parse_and_serialize(in, sizeof in - 1, &first);
    embi_sb second = {nullptr, 0, 0};
    parse_and_serialize(first.data, first.len, &second);
    EMB_CHECK(second.len == first.len);
    EMB_CHECK(memcmp(first.data, second.data, first.len) == 0);
    embi_sb_free(&second);
    embi_sb_free(&first);
}

EMB_TEST(serializer_node_of_element_has_no_outer_wrapper)
{
    embi_document *doc = embi_document_new(true);
    parse_doc("<p>a<i>b</i>", strlen("<p>a<i>b</i>"), doc, false);
    embi_node *body = body_of(doc);
    EMB_REQUIRE(body != nullptr);
    embi_sb out = {nullptr, 0, 0};
    embi_html_serialize_node(body->first_child, false, &out);
    EMB_CHECK(sb_is(&out, "<p>a<i>b</i></p>"));
    embi_sb_free(&out);
    embi_document_free(doc);
}

EMB_TEST(serializer_outer_html_of_element_includes_its_tag)
{
    embi_document *doc = embi_document_new(true);
    parse_doc("<div id=\"x\"><b>y</b></div>", strlen("<div id=\"x\"><b>y</b></div>"), doc, false);
    embi_node *body = body_of(doc);
    EMB_REQUIRE(body != nullptr);
    embi_sb out = {nullptr, 0, 0};
    embi_html_serialize_node(body->first_child, false, &out);
    EMB_CHECK(sb_is(&out, "<div id=\"x\"><b>y</b></div>"));
    embi_sb_free(&out);
    embi_document_free(doc);
}

EMB_TEST(serializer_outer_html_of_void_element)
{
    embi_document *doc = embi_document_new(true);
    parse_doc("<p><br>", strlen("<p><br>"), doc, false);
    embi_node *body = body_of(doc);
    EMB_REQUIRE(body != nullptr);
    embi_node *p = body->first_child;
    embi_sb out = {nullptr, 0, 0};
    embi_html_serialize_node(p->first_child, false, &out);
    EMB_CHECK(sb_is(&out, "<br>"));
    embi_sb_free(&out);
    embi_document_free(doc);
}

EMB_TEST(serializer_outer_html_of_document_is_its_children)
{
    embi_document *doc = embi_document_new(true);
    parse_doc("<!DOCTYPE html><p>x", strlen("<!DOCTYPE html><p>x"), doc, false);
    embi_sb out = {nullptr, 0, 0};
    embi_html_serialize_node(&doc->base, false, &out);
    EMB_CHECK(sb_is(&out, "<!DOCTYPE html><html><head></head><body><p>x</p></body></html>"));
    embi_sb_free(&out);
    embi_document_free(doc);
}

EMB_TEST(serializer_outer_html_of_fragment_is_its_children)
{
    embi_document *doc = embi_document_new(true);
    embi_element *ctx = embi_new_element(doc, EMBI_NS_HTML, 0, 0, EMBI_ATOM_div);
    const char in[] = "<b>x</b>y";
    embi_node *frag = embi_html_parse_fragment(ctx, (embi_str){in, sizeof in - 1}, nullptr);
    embi_sb out = {nullptr, 0, 0};
    embi_html_serialize_node(frag, false, &out);
    EMB_CHECK(sb_is(&out, "<b>x</b>y"));
    embi_sb_free(&out);
    embi_destroy_detached(frag);
    embi_destroy_detached(&ctx->base);
    embi_document_free(doc);
}

EMB_TEST(serializer_template_writes_its_contents)
{
    embi_sb out = {nullptr, 0, 0};
    parse_and_serialize("<template><b>x</b></template>", strlen("<template><b>x</b></template>"), &out);
    EMB_CHECK(sb_is(&out, "<html><head><template><b>x</b></template></head><body></body></html>"));
    embi_sb_free(&out);
}

EMB_TEST(serializer_foreign_elements_use_local_names_and_self_close)
{
    embi_sb out = {nullptr, 0, 0};
    const char in[] = "<svg viewBox=\"0 0 1 1\"><circle/><foreignObject><p>x</p></foreignObject></svg>"
                      "<math><mi>a</mi></math>";
    parse_and_serialize(in, sizeof in - 1, &out);
    EMB_CHECK(sb_contains(&out, "<svg viewBox=\"0 0 1 1\"><circle></circle><foreignObject><p>x</p>"
                               "</foreignObject></svg><math><mi>a</mi></math>"));
    embi_sb_free(&out);
}

EMB_TEST(serializer_namespaced_attributes_use_prefixes)
{
    embi_sb out = {nullptr, 0, 0};
    parse_and_serialize("<svg><a xlink:href=\"u\" xml:lang=\"en\"></a></svg>", strlen("<svg><a xlink:href=\"u\" xml:lang=\"en\"></a></svg>"), &out);
    EMB_CHECK(sb_contains(&out, "<a xlink:href=\"u\" xml:lang=\"en\"></a>"));
    embi_sb_free(&out);
}

EMB_TEST(serializer_noscript_depends_on_scripting)
{
    /* An explicit body keeps the noscript in body, where scripting decides how it parses. */
    const char in[] = "<body><noscript><b>x</noscript>";
    embi_document *on = embi_document_new(true);
    parse_doc(in, sizeof in - 1, on, true);
    embi_sb out_on = {nullptr, 0, 0};
    embi_html_serialize_children(&on->base, true, &out_on);
    EMB_CHECK(sb_contains(&out_on, "<noscript><b>x</noscript>"));

    embi_document *off = embi_document_new(true);
    parse_doc(in, sizeof in - 1, off, false);
    embi_sb out_off = {nullptr, 0, 0};
    embi_html_serialize_children(&off->base, false, &out_off);
    EMB_CHECK(sb_contains(&out_off, "<noscript><b>x</b></noscript>"));

    embi_sb_free(&out_on);
    embi_sb_free(&out_off);
    embi_document_free(on);
    embi_document_free(off);
}

EMB_TEST(serializer_non_bmp_text_is_not_escaped_and_bad_bytes_become_fffd)
{
    /* U+1F600 as four UTF-8 bytes, then a UTF-8 encoded lone surrogate U+D800 (invalid UTF-8,
     * so the tokenizer makes U+FFFD of it). */
    const char in[] = "\xF0\x9F\x98\x80 \xED\xA0\x80";
    embi_sb out = {nullptr, 0, 0};
    parse_and_serialize(in, sizeof in - 1, &out);
    EMB_CHECK(sb_contains(&out, "\xF0\x9F\x98\x80 \xEF\xBF\xBD"));
    embi_sb_free(&out);
}

EMB_TEST(serializer_doctype_is_written)
{
    embi_sb out = {nullptr, 0, 0};
    parse_and_serialize("<!DOCTYPE html>", strlen("<!DOCTYPE html>"), &out);
    EMB_CHECK(strncmp(out.data, "<!DOCTYPE html>", strlen("<!DOCTYPE html>")) == 0);
    embi_sb_free(&out);
}

/* 100k nested divs: the serializer must not recurse, and the document must free cleanly. */
#define DEEP_DEPTH 100000u

EMB_TEST(serializer_deep_nesting_does_not_overflow)
{
    size_t n = (size_t)DEEP_DEPTH * 5u;
    char *in = SDL_malloc(n + 1);
    for (uint32_t i = 0; i < DEEP_DEPTH; i++) {
        memcpy(in + (size_t)i * 5u, "<div>", 5);
    }
    in[n] = '\0';
    embi_document *doc = embi_document_new(true);
    parse_doc(in, n, doc, false);
    SDL_free(in);
    embi_sb out = {nullptr, 0, 0};
    embi_html_serialize_children(body_of(doc), false, &out);
    /* Body holds all the divs: each one is "<div>" (5 bytes) plus "</div>" (6 bytes). */
    EMB_CHECK(out.len == (size_t)DEEP_DEPTH * 11u);
    embi_sb_free(&out);
    embi_document_free(doc);
}

EMB_TEST_MAIN()

/* A ProcessingInstruction node is written as <?target data?>. Built directly in the DOM. */
EMB_TEST(serializer_processing_instruction_dom_level)
{
    embi_document *doc = embi_document_new(true);
    embi_dstr data = embi_dstr_from_utf8(doc->slab, EMBI_STR("a=1 b"));
    embi_chardata *pi = embi_new_pi(doc, embi_atom_intern("tgt", 3), data);
    embi_dstr_free(data);
    embi_sb out = {nullptr, 0, 0};
    embi_html_serialize_node(&pi->base, false, &out);
    EMB_CHECK(sb_is(&out, "<?tgt a=1 b?>"));
    embi_sb_free(&out);
    embi_document_free(doc);
}

/* Parser-produced PIs serialize with ?> and survive a round trip. */
EMB_TEST(serializer_processing_instruction_parsed)
{
    const char in[] = "<body><?x data?><?y>";
    embi_sb first = {nullptr, 0, 0};
    parse_and_serialize(in, sizeof in - 1, &first);
    EMB_CHECK(sb_contains(&first, "<?x data?><?y ?>"));
    embi_sb second = {nullptr, 0, 0};
    parse_and_serialize(first.data, first.len, &second);
    EMB_CHECK(second.len == first.len && memcmp(first.data, second.data, first.len) == 0);
    embi_sb_free(&second);
    embi_sb_free(&first);
}
