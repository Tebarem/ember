#include "emb_test.h"

#include "html/names.h"
#include "html/tokenizer.h"

#include <string.h>

/* Renders every token as one line and every parse error as "code@line:col", so tests can compare
 * whole streams with one string. */

typedef struct run_opts {
    embi_html_state state;
    const char *last_start;
    bool cdata;
    size_t chunk; /* 0 feeds the whole input at once */
} run_opts;

static void put(embi_sb *sb, const char *s)
{
    embi_sb_append(sb, (embi_str){s, (uint32_t)strlen(s)});
}

static void put_str(embi_sb *sb, embi_str s)
{
    if (s.len > 0) {
        embi_sb_append(sb, s);
    }
}

static void put_opt(embi_sb *sb, bool has, embi_str s)
{
    if (has) {
        put_str(sb, s);
    } else {
        put(sb, "-");
    }
}

static bool render_sink(void *ctx, const embi_html_token *tok)
{
    embi_sb *o = ctx;
    switch (tok->kind) {
    case EMBI_HTML_TOK_DOCTYPE:
        put(o, "D(");
        put_opt(o, tok->has_name, tok->name);
        put(o, "|");
        put_opt(o, tok->has_public_id, tok->public_id);
        put(o, "|");
        put_opt(o, tok->has_system_id, tok->system_id);
        put(o, tok->force_quirks ? "|1)\n" : "|0)\n");
        break;
    case EMBI_HTML_TOK_START_TAG:
        put(o, "<");
        put_str(o, tok->name);
        for (uint32_t i = 0; i < tok->attr_count; i++) {
            put(o, " ");
            put_str(o, tok->attrs[i].name);
            put(o, "=\"");
            put_str(o, tok->attrs[i].value);
            put(o, "\"");
        }
        put(o, tok->self_closing ? "/>\n" : ">\n");
        break;
    case EMBI_HTML_TOK_END_TAG:
        put(o, "</");
        put_str(o, tok->name);
        put(o, ">\n");
        break;
    case EMBI_HTML_TOK_COMMENT:
        put(o, "<!--");
        put_str(o, tok->data);
        put(o, "-->\n");
        break;
    case EMBI_HTML_TOK_PI:
        put(o, "<?");
        put_str(o, tok->name);
        put(o, " ");
        put_str(o, tok->data);
        put(o, "?>\n");
        break;
    case EMBI_HTML_TOK_CHARACTERS:
        put(o, "T:");
        put_str(o, tok->data);
        put(o, "\n");
        break;
    case EMBI_HTML_TOK_EOF:
        put(o, "EOF\n");
        break;
    }
    return true;
}

static void render_error(void *ctx, const char *code, uint64_t offset, uint32_t line, uint32_t col)
{
    embi_sb *e = ctx;
    (void)offset;
    char buf[128];
    SDL_snprintf(buf, sizeof buf, "%s@%u:%u\n", code, line, col);
    put(e, buf);
}

/* A context that carries both output buffers, so one tokenizer can report tokens and errors. */
typedef struct both {
    embi_sb toks;
    embi_sb errs;
} both;

static bool both_sink(void *ctx, const embi_html_token *tok)
{
    both *b = ctx;
    return render_sink(&b->toks, tok);
}

static void both_error(void *ctx, const char *code, uint64_t offset, uint32_t line, uint32_t col)
{
    both *b = ctx;
    render_error(&b->errs, code, offset, line, col);
}

/* Feeds the input in chunks of opts->chunk bytes and returns the rendered tokens and errors. */
static void tokenize(const char *in, size_t n, const run_opts *opts, both *out)
{
    embi_html_tokenizer *t = embi_html_tokenizer_create(both_sink, both_error, out);
    if (opts->last_start != nullptr) {
        embi_html_tokenizer_set_last_start_tag(t, (embi_str){opts->last_start,
                                                             (uint32_t)strlen(opts->last_start)});
    }
    embi_html_tokenizer_set_state(t, opts->state);
    embi_html_tokenizer_set_cdata_allowed(t, opts->cdata);
    size_t step = opts->chunk == 0 ? (n == 0 ? 1 : n) : opts->chunk;
    size_t off = 0;
    while (off < n) {
        size_t take = n - off < step ? n - off : step;
        size_t got = embi_html_tokenizer_write(t, in + off, take);
        EMB_CHECK(got == take);
        off += got;
    }
    embi_html_tokenizer_end(t);
    EMB_CHECK(!embi_html_tokenizer_paused(t));
    embi_html_tokenizer_destroy(t);
}

static void tokenize_str(const char *in, both *out)
{
    run_opts o = {EMBI_HTML_STATE_DATA, nullptr, false, 0};
    tokenize(in, strlen(in), &o, out);
}

static void both_free(both *b)
{
    embi_sb_free(&b->toks);
    embi_sb_free(&b->errs);
}

EMB_TEST(tag_and_attribute_basics)
{
    both b = {0};
    tokenize_str("<a href=\"x\" B=c checked><br/></a>", &b);
    EMB_CHECK_EQ_STR(embi_sb_cstr(&b.toks),
                     "<a href=\"x\" b=\"c\" checked=\"\">\n<br/>\n</a>\nEOF\n");
    EMB_CHECK_EQ_STR(embi_sb_cstr(&b.errs), "");
    both_free(&b);
}

EMB_TEST(character_references_numeric_and_named)
{
    both b = {0};
    tokenize_str("&amp;&lt;&#65;&#x42;&#x80;&amp", &b);
    EMB_CHECK_EQ_STR(embi_sb_cstr(&b.toks), "T:&<AB\xE2\x82\xAC&\nEOF\n");
    both_free(&b);
}

EMB_TEST(numeric_reference_edge_cases)
{
    both b = {0};
    tokenize_str("&#0;&#xD800;&#x110000;&#x81;", &b);
    EMB_CHECK_EQ_STR(embi_sb_cstr(&b.toks),
                     "T:\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xC2\x81\nEOF\n");
    both_free(&b);
}

EMB_TEST(longest_named_match_in_data)
{
    both b = {0};
    tokenize_str("&notit;&notin;", &b);
    /* "not" is a legacy name, so it decodes in data; "notin;" is a full name. */
    EMB_CHECK_EQ_STR(embi_sb_cstr(&b.toks), "T:\xC2\xACit;\xE2\x88\x89\nEOF\n");
    both_free(&b);
}

EMB_TEST(legacy_names_inside_attributes)
{
    both b = {0};
    tokenize_str("<a x=\"&amp=1\" y=\"&copy\" z=\"&notit;\">", &b);
    /* &amp followed by '=' stays literal; &copy at the end of a quoted value decodes; &not followed
     * by an alphanumeric stays literal. */
    EMB_CHECK_EQ_STR(embi_sb_cstr(&b.toks),
                     "<a x=\"&amp=1\" y=\"\xC2\xA9\" z=\"&notit;\">\nEOF\n");
    both_free(&b);
}

EMB_TEST(comment_forms)
{
    both b = {0};
    tokenize_str("<!--a-b--!><!---->", &b);
    EMB_CHECK_EQ_STR(embi_sb_cstr(&b.toks), "<!--a-b-->\n<!---->\nEOF\n");
    EMB_CHECK_EQ_STR(embi_sb_cstr(&b.errs), "incorrectly-closed-comment@1:11\n");
    both_free(&b);
}

EMB_TEST(doctype_public_and_system_identifiers)
{
    both b = {0};
    tokenize_str("<!DOCTYPE html PUBLIC \"-//W3C//DTD HTML 4.01//EN\" "
                 "\"http://www.w3.org/TR/html4/strict.dtd\">",
                 &b);
    EMB_CHECK_EQ_STR(embi_sb_cstr(&b.toks),
                     "D(html|-//W3C//DTD HTML 4.01//EN|http://www.w3.org/TR/html4/strict.dtd|0)\n"
                     "EOF\n");
    both_free(&b);

    both c = {0};
    tokenize_str("<!DOCTYPE>", &c);
    EMB_CHECK_EQ_STR(embi_sb_cstr(&c.toks), "D(-|-|-|1)\nEOF\n");
    EMB_CHECK_EQ_STR(embi_sb_cstr(&c.errs), "missing-doctype-name@1:10\n");
    both_free(&c);
}

EMB_TEST(bogus_comments_and_cdata_in_html)
{
    both b = {0};
    tokenize_str("<![CDATA[x]]><?pi?>", &b);
    EMB_CHECK_EQ_STR(embi_sb_cstr(&b.toks), "<!--[CDATA[x]]-->\n<?pi ?>\nEOF\n");
    both_free(&b);

    both c = {0};
    run_opts o = {EMBI_HTML_STATE_DATA, nullptr, true, 0};
    tokenize("<![CDATA[x]]>", 13, &o, &c);
    EMB_CHECK_EQ_STR(embi_sb_cstr(&c.toks), "T:x\nEOF\n");
    both_free(&c);
}

EMB_TEST(rcdata_end_tag_must_match_last_start_tag)
{
    both b = {0};
    run_opts o = {EMBI_HTML_STATE_RCDATA, "title", false, 0};
    const char *in = "a</titlex></title>";
    tokenize(in, strlen(in), &o, &b);
    EMB_CHECK_EQ_STR(embi_sb_cstr(&b.toks), "T:a</titlex>\n</title>\nEOF\n");
    both_free(&b);
}

EMB_TEST(duplicate_attribute_first_wins)
{
    both b = {0};
    tokenize_str("<a x=1 x=2>", &b);
    EMB_CHECK_EQ_STR(embi_sb_cstr(&b.toks), "<a x=\"1\">\nEOF\n");
    EMB_CHECK_EQ_STR(embi_sb_cstr(&b.errs), "duplicate-attribute@1:9\n");
    both_free(&b);
}

EMB_TEST(surrogate_input_is_reported_and_passed_through)
{
    both b = {0};
    run_opts o = {EMBI_HTML_STATE_DATA, nullptr, false, 0};
    tokenize("\xED\xA0\x80", 3, &o, &b);
    EMB_CHECK_EQ_STR(embi_sb_cstr(&b.toks), "T:\xED\xA0\x80\nEOF\n");
    EMB_CHECK_EQ_STR(embi_sb_cstr(&b.errs), "surrogate-in-input-stream@1:1\n");
    both_free(&b);
}

EMB_TEST(crlf_normalized_and_bom_passed_through_by_default)
{
    both b = {0};
    run_opts o = {EMBI_HTML_STATE_DATA, nullptr, false, 0};
    const char *in = "\xEF\xBB\xBF" "a\r\nb\rc";
    tokenize(in, strlen(in), &o, &b);
    EMB_CHECK_EQ_STR(embi_sb_cstr(&b.toks), "T:\xEF\xBB\xBF" "a\nb\nc\nEOF\n");
    both_free(&b);
}

EMB_TEST(bom_stripped_when_requested)
{
    const char *in = "\xEF\xBB\xBF" "a";
    both b = {0};
    embi_html_tokenizer *t = embi_html_tokenizer_create(both_sink, both_error, &b);
    embi_html_tokenizer_strip_bom(t);
    EMB_CHECK(embi_html_tokenizer_write(t, in, strlen(in)) == strlen(in));
    embi_html_tokenizer_end(t);
    embi_html_tokenizer_destroy(t);
    EMB_CHECK_EQ_STR(embi_sb_cstr(&b.toks), "T:a\nEOF\n");
    both_free(&b);
}

EMB_TEST(eof_inside_tag_reports_position)
{
    both b = {0};
    tokenize_str("<a href='x", &b);
    EMB_CHECK_EQ_STR(embi_sb_cstr(&b.toks), "EOF\n");
    EMB_CHECK_EQ_STR(embi_sb_cstr(&b.errs), "eof-in-tag@1:11\n");
    both_free(&b);
}

EMB_TEST(numeric_and_null_in_data_state)
{
    both b = {0};
    const char in[] = "a\0b";
    run_opts o = {EMBI_HTML_STATE_DATA, nullptr, false, 0};
    tokenize(in, 3, &o, &b);
    /* NUL in data is emitted as a character and reported. */
    static const char want[] = "T:a\0b\nEOF\n";
    EMB_CHECK(b.toks.len == sizeof want - 1);
    EMB_CHECK(memcmp(b.toks.data, want, sizeof want - 1) == 0);
    EMB_CHECK_EQ_STR(embi_sb_cstr(&b.errs), "unexpected-null-character@1:2\n");
    both_free(&b);
}

/* A rich document: tags, attributes, references, UTF-8, CRLF, script text and a comment. */
static const char k_rich[] =
    "<!DOCTYPE html><html a=\"&amp;&#x41;b\" c='d'><!-- x --><script>if (a<b) {}</script>"
    "h\xC3\xA9llo &notit; w\xC3\xB6rld &amp\r\n<br/><![CDATA[q]]></html>";

EMB_TEST(streaming_matches_whole_input_at_every_chunk_size)
{
    size_t n = sizeof k_rich - 1;
    both whole = {0};
    run_opts o = {EMBI_HTML_STATE_DATA, nullptr, false, 0};
    tokenize(k_rich, n, &o, &whole);
    EMB_CHECK(whole.toks.len > 0);

    static const size_t chunks[] = {1, 2, 3, 5, 7, 16, 64};
    for (size_t i = 0; i < sizeof chunks / sizeof chunks[0]; i++) {
        both part = {0};
        run_opts po = {EMBI_HTML_STATE_DATA, nullptr, false, chunks[i]};
        tokenize(k_rich, n, &po, &part);
        EMB_CHECK_EQ_STR(embi_sb_cstr(&part.toks), embi_sb_cstr(&whole.toks));
        EMB_CHECK_EQ_STR(embi_sb_cstr(&part.errs), embi_sb_cstr(&whole.errs));
        both_free(&part);
    }
    both_free(&whole);
}

typedef struct pause_ctx {
    embi_sb toks;
    int pauses_left;
} pause_ctx;

static bool pause_sink(void *ctx, const embi_html_token *tok)
{
    pause_ctx *p = ctx;
    render_sink(&p->toks, tok);
    if (tok->kind == EMBI_HTML_TOK_START_TAG && p->pauses_left > 0) {
        p->pauses_left--;
        return false;
    }
    return true;
}

EMB_TEST(pause_stops_at_the_boundary_and_resume_continues)
{
    pause_ctx p = {0};
    p.pauses_left = 1;
    embi_html_tokenizer *t = embi_html_tokenizer_create(pause_sink, nullptr, &p);
    const char *in = "<script></script>x";
    size_t n = strlen(in);
    size_t got = embi_html_tokenizer_write(t, in, n);
    EMB_CHECK(got == n);
    EMB_CHECK(embi_html_tokenizer_paused(t));
    /* Only the start tag has been delivered. */
    EMB_CHECK_EQ_STR(embi_sb_cstr(&p.toks), "<script>\n");
    embi_html_tokenizer_resume(t);
    EMB_CHECK(!embi_html_tokenizer_paused(t));
    embi_html_tokenizer_end(t);
    EMB_CHECK_EQ_STR(embi_sb_cstr(&p.toks), "<script>\n</script>\nT:x\nEOF\n");
    embi_html_tokenizer_destroy(t);
    embi_sb_free(&p.toks);
}

EMB_TEST(tokenizer_releases_all_memory)
{
    size_t before = embi_mem_live();
    for (int i = 0; i < 3; i++) {
        both b = {0};
        tokenize_str("<div a=b>text &amp; more</div><!-- c -->", &b);
        both_free(&b);
    }
    EMB_CHECK(embi_mem_live() == before);
}

/* Tags that fast_tag takes in one pass must read exactly like the state machine reads them (which
 * is what a one-byte feed forces, since the pass needs the whole tag): same tokens, same errors,
 * same positions for everything that follows, including after a tag that spans lines. */
EMB_TEST(fast_tags_read_like_the_state_machine)
{
    static const char *const cases[] = {
        "<DIV CLASS=A Id=\"x\" data-k='v w'>t</DIV>",
        "<a\n href=\"x\"\n\tb>\n\x01q\x7f&#1;z",
        "<a\r\nb=c>\x01y",
        "<p title=\"a &amp; b &lt;&gt; &quot;&#65;&#x42;&#233;&#x20AC;\" id=q>x</p>",
        "<a t=\"&nbsp;&foo;&amp\" u='&#0;&#x110000;'>x</a>",
        "<a b=c d=e/><br/><br /><a b = c d= \"e\" f =g>",
        "<a b=\"c\"d=e><a b=c\"d>",
        "<a b=c b=d B=e>x<a href=1 href=2>",
        "<input disabled value= ><input value=>x",
        "<a \"x=y><a x\"y=z><a x<y=z><a =x><a x='y' / ><a/b>",
        "<a t=\"\xC3\xA9\xE2\x82\xAC\">\xC3\xA9<\xC3\xA9>",
        "</a></A ></a x></a/><//a></ a>",
        ("<a a1=1 a2=2 a3=3 a4=4 a5=5 a6=6 a7=7 a8=8 a9=9 a10=10 a11=11 a12=12 a13=13 a14=14 "
         "a15=15 a16=16 a17=17 a18=18>x"),
        "<abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz>x",
        "<a abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz=1>",
        "<a t=\"unterminated",
        "<a t=\"x\" ",
        "<a t=\"x",
        "<script>if (a<b) {}</script><style>a{}</style><title>x&amp;</title><textarea>\n</textarea>",
        "<a>\n\n<b\n>\n<c d\n=\ne\n>\x01",
        /* Text that a tag follows is delivered as it is; text that only looks like it is not. */
        "text<b>more</b>tail<3 x</ y></>z&amp;<i>&lt;</i>end</",
        "a\nb<p>c\r\nd<q>e\x01f</q>g</p>h<",
    };
    for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        const char *in = cases[k];
        size_t n = strlen(in);
        both whole = {0};
        run_opts o = {EMBI_HTML_STATE_DATA, nullptr, false, 0};
        tokenize(in, n, &o, &whole);
        static const size_t chunks[] = {1, 2, 5};
        for (size_t i = 0; i < sizeof chunks / sizeof chunks[0]; i++) {
            both part = {0};
            run_opts po = {EMBI_HTML_STATE_DATA, nullptr, false, chunks[i]};
            tokenize(in, n, &po, &part);
            EMB_CHECK_EQ_STR(embi_sb_cstr(&part.toks), embi_sb_cstr(&whole.toks));
            EMB_CHECK_EQ_STR(embi_sb_cstr(&part.errs), embi_sb_cstr(&whole.errs));
            both_free(&part);
        }
        both_free(&whole);
    }
}

/* Spot checks of the fast path's output, so the comparison above cannot agree on a wrong answer. */
EMB_TEST(fast_tag_output)
{
    both b = {0};
    tokenize_str("<DIV CLASS=A Id=\"x\" data-k='v w'>t</DIV><p t=\"a &amp; &#65;\"/>", &b);
    EMB_CHECK_EQ_STR(embi_sb_cstr(&b.toks),
                     "<div class=\"A\" id=\"x\" data-k=\"v w\">\nT:t\n</div>\n<p t=\"a & A\"/>\nEOF\n");
    EMB_CHECK_EQ_STR(embi_sb_cstr(&b.errs), "");
    both_free(&b);

    /* Positions after a multi-line tag. */
    both c = {0};
    tokenize_str("<a\n b=\"c\nd\"\n>\x01", &c);
    EMB_CHECK_EQ_STR(embi_sb_cstr(&c.errs), "control-character-in-input-stream@4:2\n");
    both_free(&c);
}

/* Random markup soup: the fast paths and the state machine (a one-byte feed never gets a whole tag)
 * must agree on tokens, errors and positions for whatever the pieces add up to. */
EMB_TEST(fast_paths_agree_on_random_markup)
{
    static const char *const pieces[] = {
        "<", ">", "/", "=", "\"", "'", " ", "\n", "\t", "\r", "a", "B", "div", "href", "&amp;", "&lt",
        "&", ";", "x", "\xC3\xA9", "-", "!", "?", "#", "&#65;", "&#x;", "</", "<a ", "<b>", "</b>",
        "t=", "t=\"", "t='", "\f", "\x01", "0",
    };
    uint32_t seed = 12345u;
    for (int iter = 0; iter < 400; iter++) {
        char in[256];
        size_t n = 0;
        seed = seed * 1664525u + 1013904223u;
        uint32_t pieces_n = 1u + (seed >> 24) % 24u;
        for (uint32_t i = 0; i < pieces_n; i++) {
            seed = seed * 1664525u + 1013904223u;
            const char *p = pieces[(seed >> 16) % (sizeof pieces / sizeof pieces[0])];
            size_t pl = strlen(p);
            if (n + pl >= sizeof in) {
                break;
            }
            memcpy(in + n, p, pl);
            n += pl;
        }
        in[n] = '\0';
        both whole = {0};
        run_opts o = {EMBI_HTML_STATE_DATA, nullptr, false, 0};
        tokenize(in, n, &o, &whole);
        static const size_t chunks[] = {1, 7};
        for (size_t i = 0; i < sizeof chunks / sizeof chunks[0]; i++) {
            both part = {0};
            run_opts po = {EMBI_HTML_STATE_DATA, nullptr, false, chunks[i]};
            tokenize(in, n, &po, &part);
            EMB_CHECK_EQ_STR(embi_sb_cstr(&part.toks), embi_sb_cstr(&whole.toks));
            EMB_CHECK_EQ_STR(embi_sb_cstr(&part.errs), embi_sb_cstr(&whole.errs));
            if (strcmp(embi_sb_cstr(&part.toks), embi_sb_cstr(&whole.toks)) != 0) {
                SDL_Log("random markup #%d: %s", iter, in);
            }
            both_free(&part);
        }
        both_free(&whole);
    }
}

typedef struct atom_probe {
    bool has[8];
    embi_atom atom[8];
    embi_atom attr_atom[8];
    int n;
} atom_probe;

static bool atom_sink(void *ctx, const embi_html_token *tok)
{
    atom_probe *p = ctx;
    if ((tok->kind == EMBI_HTML_TOK_START_TAG || tok->kind == EMBI_HTML_TOK_END_TAG) && p->n < 8) {
        p->has[p->n] = tok->has_atom;
        p->atom[p->n] = tok->name_atom;
        p->attr_atom[p->n] = tok->attr_count ? tok->attrs[0].atom : 0;
        p->n++;
    }
    return true;
}

/* Atoms are for the tree builder: a plain tokenizer does not touch the atom table, and an
 * enabled one only looks names up (an unknown name has atom 0 and is not interned). */
EMB_TEST(tokenizer_passes_atoms_only_when_asked)
{
    static const char in[] = "<div class=x></div><zzqqtag zzqqattr=1></zzqqtag>";
    for (int want = 0; want < 2; want++) {
        atom_probe p = {0};
        embi_html_tokenizer *t = embi_html_tokenizer_create(atom_sink, nullptr, &p);
        embi_html_tokenizer_want_atoms(t, want != 0);
        embi_html_tokenizer_write(t, in, sizeof in - 1);
        embi_html_tokenizer_end(t);
        embi_html_tokenizer_destroy(t);
        EMB_CHECK_EQ_INT(p.n, 4);
        if (!want) {
            for (int i = 0; i < 4; i++) {
                EMB_CHECK(!p.has[i] && p.atom[i] == 0 && p.attr_atom[i] == 0);
            }
            continue;
        }
        EMB_CHECK(p.has[0] && p.atom[0] == embi_atom_find("div", 3) && p.atom[0] != 0);
        EMB_CHECK(p.attr_atom[0] == embi_atom_find("class", 5) && p.attr_atom[0] != 0);
        EMB_CHECK(p.has[1] && p.atom[1] == p.atom[0]);
        EMB_CHECK(p.has[2] && p.atom[2] == 0 && p.attr_atom[2] == 0);
        EMB_CHECK(p.has[3] && p.atom[3] == 0);
        EMB_CHECK(embi_atom_find("zzqqtag", 7) == 0);
        EMB_CHECK(embi_atom_find("zzqqattr", 8) == 0);
    }
}

/* The name cache survives embi_atom_shutdown only through embi_html_name_cache_reset. */
EMB_TEST(name_cache_reset_after_atom_shutdown)
{
    const char *n1 = "zzshutname1";
    const char *n2 = "zzshutname2";
    embi_atom a1 = embi_html_name_atom(n1, 11, true);
    embi_atom a2 = embi_html_name_atom(n2, 11, true);
    EMB_CHECK(a1 != 0 && a2 != 0 && a1 != a2);
    EMB_CHECK(embi_html_name_atom(n1, 11, false) == a1);
    embi_atom_shutdown();
    embi_html_name_cache_reset();
    /* Re-intern in the opposite order so the dynamic ids differ from before. */
    embi_atom b2 = embi_html_name_atom(n2, 11, true);
    embi_atom b1 = embi_html_name_atom(n1, 11, true);
    EMB_CHECK(b2 != b1);
    EMB_CHECK(b1 == embi_atom_find(n1, 11));
    EMB_CHECK(b2 == embi_atom_find(n2, 11));
    EMB_CHECK(embi_html_name_atom(n1, 11, false) == b1);
    EMB_CHECK(embi_html_name_atom(n2, 11, false) == b2);
}

EMB_TEST_MAIN()
