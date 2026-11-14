/*
 * Parser driver: owns the tokenizer and the tree builder, for whole documents (streaming) and for
 * fragments (innerHTML). The tokenizer sink hands each token to the tree builder, and a false from
 * the tree builder pauses the tokenizer (a script asked for it).
 */
#include "html/parser.h"
#include "html/treebuilder.h"
#include "html/tokenizer.h"

#include <string.h>

struct embi_html_parser {
    embi_html_parser_config cfg;
    embi_tb *tb;
    embi_html_tokenizer *tok;
};

static bool parser_sink(void *ctx, const embi_html_token *tok)
{
    embi_html_parser *p = ctx;
    if (embi_tb_stopped(p->tb)) {
        return false;
    }
    return embi_tb_token(p->tb, tok);
}

static void parser_error(void *ctx, const char *code, uint64_t offset, uint32_t line, uint32_t col)
{
    embi_html_parser *p = ctx;
    if (p->cfg.on_error) {
        p->cfg.on_error(p->cfg.error_ctx, code, offset, line, col);
    }
}

static embi_html_parser *parser_make(embi_document *doc, embi_element *context,
                                     const embi_html_parser_config *cfg)
{
    embi_html_parser *p = embi_calloc(1, sizeof *p);
    if (cfg) {
        p->cfg = *cfg;
    }
    p->tb = embi_tb_new(doc, context, cfg);
    p->tok = embi_html_tokenizer_create(parser_sink, parser_error, p);
    embi_tb_attach(p->tb, p->tok);
    return p;
}

embi_html_parser *embi_html_parser_new(embi_document *doc, const embi_html_parser_config *cfg)
{
    embi_html_parser *p = parser_make(doc, nullptr, cfg);
    embi_html_tokenizer_strip_bom(p->tok);
    return p;
}

size_t embi_html_parser_write(embi_html_parser *p, const void *bytes, size_t n)
{
    size_t k = embi_html_tokenizer_write(p->tok, bytes, n);
    if (!embi_tb_stopped(p->tb)) {
        embi_html_tokenizer_flush_text(p->tok);
        embi_tb_flush(p->tb);
    }
    return k;
}

embi_element *embi_html_parser_current(const embi_html_parser *p)
{
    return embi_tb_current(p->tb);
}

void embi_html_parser_resume(embi_html_parser *p)
{
    if (embi_tb_stopped(p->tb)) {
        return;
    }
    embi_html_tokenizer_resume(p->tok);
    embi_html_tokenizer_flush_text(p->tok);
    embi_tb_flush(p->tb);
}

bool embi_html_parser_paused(const embi_html_parser *p)
{
    return embi_html_tokenizer_paused(p->tok);
}

void embi_html_parser_end(embi_html_parser *p)
{
    if (embi_tb_stopped(p->tb)) {
        return;
    }
    embi_html_tokenizer_end(p->tok);
    embi_tb_flush(p->tb);
}

void embi_html_parser_destroy(embi_html_parser *p)
{
    if (!p) {
        return;
    }
    embi_html_tokenizer_destroy(p->tok);
    embi_tb_free(p->tb);
    embi_free(p, sizeof *p);
}

/* The tokenizer state the fragment algorithm starts in, from the context element (WHATWG 13.4). */
static void fragment_start_state(embi_html_tokenizer *t, const embi_element *ctx, bool scripting)
{
    if (ctx->base.extra != EMBI_NS_HTML) {
        return;
    }
    switch (ctx->base.name) {
    case EMBI_ATOM_title:
    case EMBI_ATOM_textarea:
        embi_html_tokenizer_set_state(t, EMBI_HTML_STATE_RCDATA);
        return;
    case EMBI_ATOM_style:
    case EMBI_ATOM_xmp:
    case EMBI_ATOM_iframe:
    case EMBI_ATOM_noembed:
    case EMBI_ATOM_noframes:
        embi_html_tokenizer_set_state(t, EMBI_HTML_STATE_RAWTEXT);
        return;
    case EMBI_ATOM_script:
        embi_html_tokenizer_set_state(t, EMBI_HTML_STATE_SCRIPT_DATA);
        return;
    case EMBI_ATOM_noscript:
        if (scripting) {
            embi_html_tokenizer_set_state(t, EMBI_HTML_STATE_RAWTEXT);
        }
        return;
    case EMBI_ATOM_plaintext:
        embi_html_tokenizer_set_state(t, EMBI_HTML_STATE_PLAINTEXT);
        return;
    default:
        return;
    }
}

embi_node *embi_html_parse_fragment(embi_element *context, embi_str input,
                                    const embi_html_parser_config *cfg)
{
    embi_document *doc = context->base.doc;
    embi_html_parser *p = parser_make(doc, context, cfg);
    fragment_start_state(p->tok, context, p->cfg.scripting);
    size_t off = 0;
    while (off < input.len) {
        size_t k = embi_html_tokenizer_write(p->tok, input.ptr + off, input.len - off);
        if (k == 0) {
            break;
        }
        off += k;
    }
    embi_html_tokenizer_end(p->tok);
    embi_node *frag = embi_tb_take_fragment(p->tb);
    embi_html_parser_destroy(p);
    return frag;
}
