/*
 * Script pause and resume in the streaming parser: a script end tag that returns PAUSE stops the
 * tokenizer, and the caller resumes it and writes the rest. The tree must not depend on how the
 * input was chunked or where the pauses fell.
 */
#include "emb_test.h"

#include "base/base.h"
#include "dom/dom.h"
#include "html/parser.h"
#include "html/serializer.h"

#include <string.h>

static const char kDoc[] =
    "<!DOCTYPE html><html><head><script>var a=1;</script></head>"
    "<body><p>one<script>var b=2;</script><p>two</p></body></html>";

static const char kTree[] =
    "<!DOCTYPE html><html><head><script>var a=1;</script></head>"
    "<body><p>one<script>var b=2;</script></p><p>two</p></body></html>";

typedef struct pause_probe {
    int pauses;
    bool always_pause;
    embi_html_parser *parser; /* when set, on_script inspects the stack through it */
    int calls;
    int popped;               /* calls where the script was already off the stack */
    bool script_namespace_svg;
} pause_probe;

static embi_html_script_result on_script(void *ctx, embi_element *script)
{
    pause_probe *pp = ctx;
    pp->calls++;
    if (pp->parser) {
        /* The script end tag has been processed: the script is off the stack, and the current
         * node is the element that the script was inserted into. */
        embi_element *cur = embi_html_parser_current(pp->parser);
        if (cur != script && script->base.parent == &cur->base) {
            pp->popped++;
        }
    }
    pp->script_namespace_svg = script->base.extra == EMBI_NS_SVG;
    if (!pp->always_pause) {
        return EMBI_HTML_SCRIPT_CONTINUE;
    }
    pp->pauses++;
    return EMBI_HTML_SCRIPT_PAUSE;
}

/* Writes n bytes in pieces of at most chunk bytes, resuming after every pause. Returns the
 * number of resumes that happened. */
static int feed(embi_html_parser *p, const char *s, size_t n, size_t chunk)
{
    size_t off = 0;
    int resumes = 0;
    int stalls = 0;
    while (off < n) {
        size_t want = n - off < chunk ? n - off : chunk;
        size_t k = embi_html_parser_write(p, s + off, want);
        off += k;
        if (embi_html_parser_paused(p)) {
            embi_html_parser_resume(p);
            resumes++;
            stalls = 0;
        } else if (k == 0) {
            if (++stalls > 1000) {
                break;
            }
        }
    }
    while (embi_html_parser_paused(p)) {
        embi_html_parser_resume(p);
        resumes++;
    }
    return resumes;
}

/* Parses kDoc in the given chunking and returns the serialized document in out. */
static void parse_chunked(size_t chunk, bool pausing, pause_probe *pp, embi_sb *out)
{
    embi_document *doc = embi_document_new(true);
    embi_html_parser_config cfg = {0};
    cfg.on_script = on_script;
    cfg.script_ctx = pp;
    embi_html_parser *p = embi_html_parser_new(doc, &cfg);
    pp->always_pause = pausing;
    feed(p, kDoc, sizeof kDoc - 1, chunk);
    embi_html_parser_end(p);
    embi_html_parser_destroy(p);
    embi_html_serialize_children(&doc->base, false, out);
    embi_document_free(doc);
}

EMB_TEST(pause_on_each_script_then_resume)
{
    pause_probe pp = {.pauses = 0, .always_pause = true};
    embi_sb out = {nullptr, 0, 0};
    parse_chunked(sizeof kDoc, true, &pp, &out);
    EMB_CHECK(pp.pauses == 2);
    EMB_CHECK(out.len == sizeof kTree - 1);
    EMB_CHECK(memcmp(out.data, kTree, out.len) == 0);
    embi_sb_free(&out);
}

EMB_TEST(chunking_does_not_change_the_tree)
{
    static const size_t chunks[] = {1, 2, 7, 64};
    for (size_t i = 0; i < sizeof chunks / sizeof chunks[0]; i++) {
        pause_probe pp = {.pauses = 0, .always_pause = true};
        embi_sb out = {nullptr, 0, 0};
        parse_chunked(chunks[i], true, &pp, &out);
        EMB_CHECK(pp.pauses == 2);
        EMB_CHECK(out.len == sizeof kTree - 1);
        EMB_CHECK(memcmp(out.data, kTree, out.len) == 0);
        embi_sb_free(&out);
    }
}

EMB_TEST(no_pause_gives_the_same_tree)
{
    pause_probe pp = {.pauses = 0, .always_pause = false};
    embi_sb out = {nullptr, 0, 0};
    parse_chunked(3, false, &pp, &out);
    EMB_CHECK(pp.pauses == 0);
    EMB_CHECK(out.len == sizeof kTree - 1);
    EMB_CHECK(memcmp(out.data, kTree, out.len) == 0);
    embi_sb_free(&out);
}

/* Parses one document with a parser that pauses at each script and records the stack at the
 * pause. */
static void parse_probed(const char *doc, size_t n, pause_probe *pp, embi_sb *out)
{
    embi_document *d = embi_document_new(true);
    embi_html_parser_config cfg = {0};
    cfg.on_script = on_script;
    cfg.script_ctx = pp;
    embi_html_parser *p = embi_html_parser_new(d, &cfg);
    pp->parser = p;
    pp->always_pause = true;
    feed(p, doc, n, n);
    embi_html_parser_end(p);
    embi_html_parser_destroy(p);
    embi_html_serialize_children(&d->base, false, out);
    embi_document_free(d);
}

EMB_TEST(script_is_popped_when_on_script_runs)
{
    static const char doc[] = "<body><p>x<script>a</script>y";
    pause_probe pp = {0};
    embi_sb out = {nullptr, 0, 0};
    parse_probed(doc, sizeof doc - 1, &pp, &out);
    EMB_CHECK(pp.calls == 1);
    EMB_CHECK(pp.popped == 1);
    EMB_CHECK(!pp.script_namespace_svg);
    embi_sb_free(&out);
}

EMB_TEST(svg_script_pauses_and_is_popped)
{
    static const char doc[] = "<svg><script>a</script></svg><p>z</p>";
    pause_probe pp = {0};
    embi_sb out = {nullptr, 0, 0};
    parse_probed(doc, sizeof doc - 1, &pp, &out);
    EMB_CHECK(pp.calls == 1);
    EMB_CHECK(pp.pauses == 1);
    EMB_CHECK(pp.popped == 1);
    EMB_CHECK(pp.script_namespace_svg);
    static const char want[] =
        "<html><head></head><body><svg><script>a</script></svg><p>z</p></body></html>";
    EMB_CHECK(out.len == sizeof want - 1);
    EMB_CHECK(memcmp(out.data, want, out.len) == 0);
    embi_sb_free(&out);
}

/* A write that reaches a script reports the pause, and the tokens after the script stay buffered
 * until resume (embi_html_parser.h). So the write accepts every byte it was given, and the rest
 * of the input after resume is accepted as nothing more. The tree is the same as without pauses. */
EMB_TEST(single_write_returns_bytes_accepted_at_pause)
{
    static const char doc[] = "<body><script>a</script><p>x</p>";
    size_t n = sizeof doc - 1;

    embi_document *d = embi_document_new(true);
    pause_probe pp = {.pauses = 0, .always_pause = true};
    embi_html_parser_config cfg = {0};
    cfg.on_script = on_script;
    cfg.script_ctx = &pp;
    embi_html_parser *p = embi_html_parser_new(d, &cfg);
    size_t k = embi_html_parser_write(p, doc, n);
    EMB_CHECK(embi_html_parser_paused(p));
    EMB_CHECK(k == n);
    EMB_CHECK(pp.pauses == 1);
    embi_html_parser_resume(p);
    EMB_CHECK(!embi_html_parser_paused(p));
    size_t k2 = embi_html_parser_write(p, doc + k, n - k);
    EMB_CHECK(k2 == n - k);
    embi_html_parser_end(p);
    embi_html_parser_destroy(p);

    embi_sb out = {nullptr, 0, 0};
    embi_html_serialize_children(&d->base, false, &out);
    static const char want[] =
        "<html><head></head><body><script>a</script><p>x</p></body></html>";
    EMB_CHECK(out.len == sizeof want - 1);
    EMB_CHECK(memcmp(out.data, want, out.len) == 0);
    embi_sb_free(&out);
    embi_document_free(d);
}

static bool sb_eq(const embi_sb *sb, const char *want)
{
    size_t n = strlen(want);
    return sb->len == n && memcmp(sb->data, want, n) == 0;
}

/* Serializes the document body's children as seen right now. */
static void snap(embi_document *d, embi_sb *out)
{
    out->data = nullptr;
    out->len = 0;
    out->cap = 0;
    embi_html_serialize_children(&d->base, false, out);
}

typedef struct snap_ctx {
    embi_document *doc;
    embi_sb seen;
    bool pause;
} snap_ctx;

static embi_html_script_result on_script_snap(void *ctx, embi_element *script)
{
    (void)script;
    snap_ctx *sc = ctx;
    embi_sb_free(&sc->seen); /* the hook can run more than once: keep only the latest snapshot */
    snap(sc->doc, &sc->seen);
    return sc->pause ? EMBI_HTML_SCRIPT_PAUSE : EMBI_HTML_SCRIPT_CONTINUE;
}

/* The DOM is current at every point control returns to the caller: inside the script hook, after
 * the write that paused, right after resume (text after the script), and after end. */
EMB_TEST(dom_is_current_after_resume)
{
    embi_document *d = embi_document_new(true);
    snap_ctx sc = {.doc = d, .pause = true};
    embi_html_parser_config cfg = {0};
    cfg.on_script = on_script_snap;
    cfg.script_ctx = &sc;
    embi_html_parser *p = embi_html_parser_new(d, &cfg);
    static const char doc[] = "<body>head<script>x</script>tail";
    size_t k = embi_html_parser_write(p, doc, sizeof doc - 1);
    EMB_CHECK(k == sizeof doc - 1);
    EMB_CHECK(embi_html_parser_paused(p));
    /* In the hook: text before the script and the script text are already in the tree. */
    EMB_CHECK(sb_eq(&sc.seen, "<html><head></head><body>head<script>x</script></body></html>"));
    embi_sb out;
    snap(d, &out);
    EMB_CHECK(sb_eq(&out, "<html><head></head><body>head<script>x</script></body></html>"));
    embi_sb_free(&out);
    embi_html_parser_resume(p);
    snap(d, &out);
    EMB_CHECK(sb_eq(&out, "<html><head></head><body>head<script>x</script>tail</body></html>"));
    embi_sb_free(&out);
    embi_html_parser_end(p);
    snap(d, &out);
    EMB_CHECK(sb_eq(&out, "<html><head></head><body>head<script>x</script>tail</body></html>"));
    embi_sb_free(&out);
    embi_sb_free(&sc.seen);
    embi_html_parser_destroy(p);
    embi_document_free(d);
}

EMB_TEST(dom_is_current_after_every_write)
{
    embi_document *d = embi_document_new(true);
    embi_html_parser *p = embi_html_parser_new(d, nullptr);
    embi_sb out;
    embi_html_parser_write(p, "<body>ab", 8);
    snap(d, &out);
    EMB_CHECK(sb_eq(&out, "<html><head></head><body>ab</body></html>"));
    embi_sb_free(&out);
    embi_html_parser_write(p, "c", 1);
    snap(d, &out);
    EMB_CHECK(sb_eq(&out, "<html><head></head><body>abc</body></html>"));
    embi_sb_free(&out);
    embi_html_parser_end(p);
    snap(d, &out);
    EMB_CHECK(sb_eq(&out, "<html><head></head><body>abc</body></html>"));
    embi_sb_free(&out);
    embi_html_parser_destroy(p);
    embi_document_free(d);
}

/* Several pauses in a row, each followed by text, with a snapshot straight after every resume. */
EMB_TEST(dom_is_current_after_each_of_many_resumes)
{
    embi_document *d = embi_document_new(true);
    snap_ctx sc = {.doc = d, .pause = true};
    embi_html_parser_config cfg = {0};
    cfg.on_script = on_script_snap;
    cfg.script_ctx = &sc;
    embi_html_parser *p = embi_html_parser_new(d, &cfg);
    static const char doc[] = "<body>a<script></script>b<script></script>c";
    embi_html_parser_write(p, doc, sizeof doc - 1);
    EMB_CHECK(embi_html_parser_paused(p));
    embi_html_parser_resume(p);
    EMB_CHECK(embi_html_parser_paused(p));
    embi_sb out;
    snap(d, &out);
    EMB_CHECK(sb_eq(&out, "<html><head></head><body>a<script></script>b<script></script></body></html>"));
    embi_sb_free(&out);
    embi_html_parser_resume(p);
    snap(d, &out);
    EMB_CHECK(sb_eq(&out, "<html><head></head><body>a<script></script>b<script></script>c</body></html>"));
    embi_sb_free(&out);
    embi_html_parser_end(p);
    embi_sb_free(&sc.seen);
    embi_html_parser_destroy(p);
    embi_document_free(d);
}

EMB_TEST_MAIN()
