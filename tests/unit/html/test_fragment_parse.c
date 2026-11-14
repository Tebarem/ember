/*
 * Fragment parsing (innerHTML): the context element selects the tokenizer state and the insertion
 * mode, the nodes are built in the context element's own document, and scripts never run.
 */
#include "emb_test.h"

#include "base/base.h"
#include "dom/dom.h"
#include "html/parser.h"
#include "html/serializer.h"

#include <string.h>

static embi_element *make_context(embi_document *doc, uint8_t ns, const char *name)
{
    embi_str local = {name, (uint32_t)strlen(name)};
    return embi_new_element(doc, ns, 0, 0, embi_atom_intern(local.ptr, local.len));
}

/* Parses input under context and returns the serialized children in out. The fragment is freed. */
static embi_node *frag_of(embi_element *ctx, const char *input, embi_html_parser_config *cfg,
                          embi_sb *out)
{
    embi_str in = {input, (uint32_t)strlen(input)};
    embi_node *frag = embi_html_parse_fragment(ctx, in, cfg);
    embi_html_serialize_children(frag, cfg ? cfg->scripting : false, out);
    return frag;
}

static bool sb_is(const embi_sb *sb, const char *s)
{
    size_t n = strlen(s);
    return sb->len == n && memcmp(sb->data, s, n) == 0;
}

EMB_TEST(fragment_in_div_builds_children)
{
    embi_document *doc = embi_document_new(true);
    embi_element *ctx = make_context(doc, EMBI_NS_HTML, "div");
    embi_sb out = {nullptr, 0, 0};
    embi_node *frag = frag_of(ctx, "<p>a<b>b</i>c", nullptr, &out);
    EMB_CHECK(sb_is(&out, "<p>a<b>bc</b></p>"));
    EMB_CHECK(frag->doc == ctx->base.doc);
    embi_sb_free(&out);
    embi_destroy_detached(frag);
    embi_destroy_detached(&ctx->base);
    embi_document_free(doc);
}

EMB_TEST(fragment_title_context_is_rcdata)
{
    embi_document *doc = embi_document_new(true);
    embi_element *ctx = make_context(doc, EMBI_NS_HTML, "title");
    embi_sb out = {nullptr, 0, 0};
    embi_node *frag = frag_of(ctx, "<b>x&amp;", nullptr, &out);
    EMB_CHECK(sb_is(&out, "&lt;b&gt;x&amp;"));
    embi_sb_free(&out);
    embi_destroy_detached(frag);
    embi_destroy_detached(&ctx->base);
    embi_document_free(doc);
}

EMB_TEST(fragment_svg_context_builds_svg_children)
{
    embi_document *doc = embi_document_new(true);
    embi_element *ctx = make_context(doc, EMBI_NS_SVG, "path");
    embi_sb out = {nullptr, 0, 0};
    embi_node *frag = frag_of(ctx, "<g>x</g>", nullptr, &out);
    EMB_CHECK(sb_is(&out, "<g>x</g>"));
    EMB_CHECK(frag->first_child != nullptr && frag->first_child->extra == EMBI_NS_SVG);
    embi_sb_free(&out);
    embi_destroy_detached(frag);
    embi_destroy_detached(&ctx->base);
    embi_document_free(doc);
}

static int g_script_calls;

static embi_html_script_result count_script(void *ctx, embi_element *script)
{
    (void)ctx;
    (void)script;
    g_script_calls++;
    return EMBI_HTML_SCRIPT_PAUSE;
}

EMB_TEST(fragment_scripts_never_run)
{
    embi_document *doc = embi_document_new(true);
    embi_element *ctx = make_context(doc, EMBI_NS_HTML, "div");
    embi_html_parser_config cfg = {0};
    cfg.scripting = true;
    cfg.on_script = count_script;
    embi_sb out = {nullptr, 0, 0};
    g_script_calls = 0;
    embi_node *frag = frag_of(ctx, "<script>1</script><p>z", &cfg, &out);
    EMB_CHECK(g_script_calls == 0);
    EMB_CHECK(sb_is(&out, "<script>1</script><p>z</p>"));
    embi_sb_free(&out);
    embi_destroy_detached(frag);
    embi_destroy_detached(&ctx->base);
    embi_document_free(doc);
}

EMB_TEST_MAIN()
