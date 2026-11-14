// fuzz-modules: html dom loader
/* HTML tokenizer plus tree builder. Input: byte 0 = mode, byte 1 = chunk size or fragment context,
 * rest = markup. Modes (byte 0 & 3): 0 whole document in one write, 1 document in chunks (so tokens
 * and UTF-8 sequences straddle writes), 2 fragment against a context element, 3 byte stream decoded
 * with the sniffing algorithm (embi_html_decode_bytes) and then parsed. Bit 2 = scripting flag,
 * bit 3 = a script end tag pauses the parser (and the harness resumes it), bit 4 = serialize the
 * result and parse that again, bit 5 = parse errors go to a counting hook, bit 6 = run only the
 * standalone tokenizer instead (byte 1 bits 0..2 = start state, bits 3..4 = last start tag for the
 * appropriate-end-tag rule, bit 5 = pause on every 5th token, bit 6 = atoms, bit 7 = CDATA allowed). */
#define FUZZ_RESET_HOOKS() embi_html_name_cache_reset();
#include "dom/dom.h"
#include "html/input.h"
#include "html/names.h"
#include "html/parser.h"
#include "html/serializer.h"
#include "fuzz_common.h"

typedef struct run {
    unsigned scripts;
    unsigned errors;
    bool pause;
} run;

static embi_html_script_result on_script(void *ctx, embi_element *script)
{
    run *r = ctx;
    (void)script;
    r->scripts++;
    return r->pause ? EMBI_HTML_SCRIPT_PAUSE : EMBI_HTML_SCRIPT_CONTINUE;
}

static void on_error(void *ctx, const char *code, uint64_t offset, uint32_t line, uint32_t col)
{
    run *r = ctx;
    (void)code;
    (void)offset;
    (void)line;
    (void)col;
    r->errors++;
}

static void parse_doc(embi_document *doc, const uint8_t *p, size_t n, size_t chunk,
                      const embi_html_parser_config *cfg)
{
    embi_html_parser *hp = embi_html_parser_new(doc, cfg);
    size_t pos = 0;
    while (pos < n) {
        size_t take = chunk ? (n - pos < chunk ? n - pos : chunk) : n - pos;
        size_t done = 0;
        while (done < take) {
            size_t acc = embi_html_parser_write(hp, p + pos + done, take - done);
            done += acc;
            if (embi_html_parser_paused(hp)) {
                embi_html_parser_resume(hp);
            } else if (acc == 0) {
                break;
            }
        }
        pos += take;
    }
    if (embi_html_parser_paused(hp)) {
        embi_html_parser_resume(hp);
    }
    embi_html_parser_end(hp);
    embi_html_parser_destroy(hp);
}

typedef struct ctx_desc {
    const char *name;
    uint8_t ns;
} ctx_desc;

static const ctx_desc contexts[] = {
    {"div", EMBI_NS_HTML},      {"table", EMBI_NS_HTML},   {"tbody", EMBI_NS_HTML},
    {"tr", EMBI_NS_HTML},       {"td", EMBI_NS_HTML},      {"select", EMBI_NS_HTML},
    {"title", EMBI_NS_HTML},    {"textarea", EMBI_NS_HTML}, {"script", EMBI_NS_HTML},
    {"style", EMBI_NS_HTML},    {"plaintext", EMBI_NS_HTML}, {"noscript", EMBI_NS_HTML},
    {"template", EMBI_NS_HTML}, {"body", EMBI_NS_HTML},    {"html", EMBI_NS_HTML},
    {"head", EMBI_NS_HTML},     {"frameset", EMBI_NS_HTML}, {"colgroup", EMBI_NS_HTML},
    {"caption", EMBI_NS_HTML},  {"svg", EMBI_NS_SVG},      {"foreignObject", EMBI_NS_SVG},
    {"math", EMBI_NS_MATHML},   {"annotation-xml", EMBI_NS_MATHML}, {"mi", EMBI_NS_MATHML},
    {"p", EMBI_NS_HTML},        {"button", EMBI_NS_HTML},  {"option", EMBI_NS_HTML},
    {"iframe", EMBI_NS_HTML},   {"xmp", EMBI_NS_HTML},     {"form", EMBI_NS_HTML},
    {"a", EMBI_NS_HTML},        {"li", EMBI_NS_HTML},
};

static void parse_fragment(embi_document *doc, uint8_t which, embi_str text,
                           const embi_html_parser_config *cfg)
{
    const ctx_desc *cd = &contexts[which % (sizeof contexts / sizeof contexts[0])];
    embi_element *ctx = embi_new_element(doc, cd->ns, 0, 0,
                                         embi_atom_intern(cd->name, (uint32_t)strlen(cd->name)));
    embi_node *frag = embi_html_parse_fragment(ctx, text, cfg);
    embi_sb out = {0};
    embi_html_serialize_children(frag, cfg ? cfg->scripting : false, &out);
    embi_sb_free(&out);
    embi_normalize(frag);
    embi_destroy_detached(frag);
    embi_destroy_detached(&ctx->base);
}

typedef struct tok_run {
    unsigned tokens;
    bool pause;
} tok_run;

static bool tok_sink(void *ctx, const embi_html_token *t)
{
    tok_run *r = ctx;
    r->tokens++;
    unsigned sum = t->name.len + t->data.len + t->attr_count;
    for (uint32_t i = 0; i < t->attr_count; i++) {
        sum += t->attrs[i].name.len + t->attrs[i].value.len;
    }
    (void)sum;
    return !(r->pause && r->tokens % 5 == 0);
}

static void tok_error(void *ctx, const char *code, uint64_t offset, uint32_t line, uint32_t col)
{
    (void)ctx;
    (void)code;
    (void)offset;
    (void)line;
    (void)col;
}

static void run_tokenizer(fuzz_in in, uint8_t arg)
{
    static const char *const last_tags[] = {"", "script", "style", "textarea"};
    tok_run r = {0, (arg & 32) != 0};
    embi_html_tokenizer *t = embi_html_tokenizer_create(tok_sink, tok_error, &r);
    embi_html_tokenizer_set_state(t, (embi_html_state)((arg & 7) % 6));
    const char *lt = last_tags[(arg >> 3) & 3];
    embi_html_tokenizer_set_last_start_tag(t, (embi_str){lt, (uint32_t)strlen(lt)});
    embi_html_tokenizer_want_atoms(t, (arg & 64) != 0);
    embi_html_tokenizer_set_cdata_allowed(t, (arg & 128) != 0);
    embi_html_tokenizer_strip_bom(t);
    size_t pos = 0;
    while (pos < in.n) {
        size_t chunk = (arg & 1) ? 3 : in.n;
        size_t take = in.n - pos < chunk ? in.n - pos : chunk;
        size_t acc = embi_html_tokenizer_write(t, in.p + pos, take);
        pos += acc;
        if (embi_html_tokenizer_paused(t)) {
            embi_html_tokenizer_resume(t);
        } else if (acc == 0) {
            break;
        }
    }
    embi_html_tokenizer_flush_text(t);
    if (embi_html_tokenizer_paused(t)) {
        embi_html_tokenizer_resume(t);
    }
    embi_html_tokenizer_end(t);
    embi_html_tokenizer_destroy(t);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    FUZZ_GUARD_BEGIN();
    fuzz_in in = {data, size};
    uint8_t mode = fuzz_u8(&in);
    uint8_t arg = fuzz_u8(&in);
    run r = {0, 0, (mode & 8) != 0};
    embi_html_parser_config cfg = {
        .scripting = (mode & 4) != 0,
        .on_script = on_script,
        .script_ctx = &r,
        .on_error = (mode & 32) ? on_error : nullptr,
        .error_ctx = &r,
    };

    if (mode & 64) {
        run_tokenizer(in, arg);
        FUZZ_GUARD_END();
        return 0;
    }
    embi_document *doc = embi_document_new(true);
    embi_sb decoded = {0};
    embi_str text = fuzz_str(&in);
    const uint8_t *bytes = in.p;
    size_t nbytes = in.n;
    switch (mode & 3) {
    case 0:
        parse_doc(doc, bytes, nbytes, 0, &cfg);
        break;
    case 1:
        parse_doc(doc, bytes, nbytes, (size_t)(arg & 31) + 1, &cfg);
        break;
    case 2:
        parse_fragment(doc, arg, text, &cfg);
        break;
    default: {
        const embi_encoding *used = nullptr;
        embi_html_decode_bytes(bytes, nbytes, EMBI_STR(""), &decoded, &used);
        parse_doc(doc, (const uint8_t *)decoded.data, decoded.len, 0, &cfg);
        break;
    }
    }

    embi_sb out = {0};
    embi_html_serialize_node(&doc->base, cfg.scripting, &out);
    if ((mode & 16) && out.len > 0 && (mode & 3) != 2) {
        embi_document *doc2 = embi_document_new(true);
        parse_doc(doc2, (const uint8_t *)out.data, out.len, 0, &cfg);
        embi_sb out2 = {0};
        embi_html_serialize_node(&doc2->base, cfg.scripting, &out2);
        embi_sb_free(&out2);
        embi_document_free(doc2);
    }
    embi_sb_free(&out);
    embi_sb_free(&decoded);
    embi_document_free(doc);
    FUZZ_GUARD_END();
    return 0;
}
