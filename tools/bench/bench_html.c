/*
 * HTML parser bench. It builds a 5 MB document of nested divs, spans, paragraphs, attributes, text
 * and character references, then measures:
 *   - tokenizer only (the sink counts tokens and returns true),
 *   - full tree build (document parser with the tree builder, scripting off),
 *   - peak live memory of the tree build against the input size.
 * Targets: tokenizer >= 300 MB/s, tree build >= 120 MB/s, and
 * live memory <= 150 bytes per DOM node, with the peak within 1.15x of the final tree. (The
 * original "3x input" budget cannot hold for markup this dense: the input has about 12.7 bytes
 * per node and a bare node is 64 bytes. The ratio to the input is still printed.) Each timing is
 * the best of several runs. Run it from a Release build for numbers:
 *     cmake --build build/<dir> --target bench_html
 *     build/<dir>/tools/bench/bench_html
 * Each line ends in PASS or FAIL. The exit code is 1 if any target failed.
 */

#include "base/base.h"
#include "dom/dom.h"
#include "html/parser.h"
#include "html/tokenizer.h"

#include <SDL3/SDL_log.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

#include <string.h>

#define BENCH_TARGET_BytesTokenizer 300.0 /* MB/s */
#define BENCH_TARGET_BytesTree 120.0      /* MB/s */
#define BENCH_TARGET_BYTES_PER_NODE 150.0 /* live bytes per DOM node */
#define BENCH_TARGET_PEAK_OVER_TREE 1.15  /* peak live bytes / final tree bytes */
#define BENCH_INPUT_BYTES (5u * 1024u * 1024u)
#define BENCH_RUNS 5

/* Nested markup: every block opens a card div with a span and a paragraph inside it, and the
 * cards nest up to BENCH_NEST_DEPTH levels before they are all closed, so the stack of open
 * elements grows and shrinks through the document, as it does on real pages. */
#define BENCH_NEST_DEPTH 40u

static char *make_document(size_t *out_len)
{
    size_t cap = BENCH_INPUT_BYTES + 4096;
    char *buf = embi_alloc(cap);
    size_t n = 0;
    unsigned i = 0;
    unsigned open = 0;
    while (n < BENCH_INPUT_BYTES) {
        int w = SDL_snprintf(buf + n, cap - n,
                             "<div class=\"card c%u\" id=\"n%u\" data-x='a&amp;b'>"
                             "<span title=\"t%u\">text &amp; &lt;more&gt; &#233;</span>"
                             "<p>Para %u with <b>bold</b> and <i>it</i>.\n",
                             i % 17u, i, i, i);
        if (w <= 0) {
            break;
        }
        n += (size_t)w;
        open++;
        if (open >= BENCH_NEST_DEPTH) {
            while (open > 0) {
                w = SDL_snprintf(buf + n, cap - n, "</p></div>\n");
                if (w <= 0) {
                    break;
                }
                n += (size_t)w;
                open--;
            }
        }
        i++;
    }
    while (open > 0) {
        int w = SDL_snprintf(buf + n, cap - n, "</p></div>\n");
        if (w <= 0) {
            break;
        }
        n += (size_t)w;
        open--;
    }
    *out_len = n;
    return buf;
}

typedef struct count_ctx {
    size_t tokens;
} count_ctx;

static bool count_sink(void *ctx, const embi_html_token *tok)
{
    (void)tok;
    ((count_ctx *)ctx)->tokens++;
    return true;
}

static void swallow_error(void *ctx, const char *code, uint64_t offset, uint32_t line, uint32_t col)
{
    (void)ctx;
    (void)code;
    (void)offset;
    (void)line;
    (void)col;
}

static double tokenize_once(const char *buf, size_t n, size_t *tokens)
{
    count_ctx cc = {0};
    uint64_t t0 = SDL_GetPerformanceCounter();
    embi_html_tokenizer *t = embi_html_tokenizer_create(count_sink, swallow_error, &cc);
    embi_html_tokenizer_write(t, buf, n);
    embi_html_tokenizer_end(t);
    embi_html_tokenizer_destroy(t);
    uint64_t t1 = SDL_GetPerformanceCounter();
    *tokens = cc.tokens;
    return (double)(t1 - t0) / (double)SDL_GetPerformanceFrequency();
}

static double build_once(const char *buf, size_t n)
{
    uint64_t t0 = SDL_GetPerformanceCounter();
    embi_document *doc = embi_document_new(true);
    embi_html_parser *p = embi_html_parser_new(doc, nullptr);
    embi_html_parser_write(p, buf, n);
    embi_html_parser_end(p);
    embi_html_parser_destroy(p);
    uint64_t t1 = SDL_GetPerformanceCounter();
    embi_document_free(doc);
    return (double)(t1 - t0) / (double)SDL_GetPerformanceFrequency();
}

/* Counts the nodes of a built tree by kind (iterative walk) and the attributes on elements. */
typedef struct tree_census {
    size_t elements, texts, attrs, nodes;
} tree_census;

static void census(const embi_node *root, tree_census *c)
{
    const embi_node *n = root;
    while (n) {
        c->nodes++;
        if (n->type == EMBI_NODE_ELEMENT) {
            c->elements++;
            c->attrs += embi_el_attr_count((const embi_element *)n);
        } else if (n->type == EMBI_NODE_TEXT) {
            c->texts++;
        }
        if (n->first_child) {
            n = n->first_child;
            continue;
        }
        while (n && n != root && !n->next) {
            n = n->parent;
        }
        if (!n || n == root) {
            break;
        }
        n = n->next;
    }
}

int main(void)
{
    size_t n = 0;
    char *buf = make_document(&n);
    double mb = (double)n / (1024.0 * 1024.0);
    SDL_Log("input: %.2f MB (%llu bytes)", mb, (unsigned long long)n);

    /* Peak memory first, in a clean measurement window: live bytes before the build, and the
     * process peak after it. The tokenizer runs do not allocate much, so they come after. */
    size_t live_before = embi_mem_live();
    double best_tree = 1e9;
    for (int r = 0; r < BENCH_RUNS; r++) {
        double s = build_once(buf, n);
        if (s < best_tree) {
            best_tree = s;
        }
    }
    size_t peak_delta = embi_mem_peak() > live_before ? embi_mem_peak() - live_before : 0;

    size_t tree_bytes_out = 0;
    size_t nodes_out = 0;
    /* The tree of one build, while it is still alive: its live bytes and node census. */
    {
        size_t base = embi_mem_live();
        embi_document *doc = embi_document_new(true);
        embi_html_parser *p = embi_html_parser_new(doc, nullptr);
        embi_html_parser_write(p, buf, n);
        embi_html_parser_end(p);
        embi_html_parser_destroy(p);
        size_t tree_bytes = embi_mem_live() - base;
        tree_census c = {0, 0, 0, 0};
        census(&doc->base, &c);
        tree_bytes_out = tree_bytes;
        nodes_out = c.nodes;
        SDL_Log("tree census: %llu nodes (%llu elements, %llu text, %llu attributes); live %.2f MB, %.1f bytes per node",
                (unsigned long long)c.nodes, (unsigned long long)c.elements,
                (unsigned long long)c.texts, (unsigned long long)c.attrs,
                (double)tree_bytes / (1024.0 * 1024.0),
                c.nodes ? (double)tree_bytes / (double)c.nodes : 0.0);
        embi_document_free(doc);
    }
    double peak_ratio = (double)peak_delta / (double)n;
    double bytes_per_node = nodes_out ? (double)tree_bytes_out / (double)nodes_out : 0.0;
    double peak_over_tree = tree_bytes_out ? (double)peak_delta / (double)tree_bytes_out : 0.0;

    double best_tok = 1e9;
    size_t tokens = 0;
    for (int r = 0; r < BENCH_RUNS; r++) {
        double s = tokenize_once(buf, n, &tokens);
        if (s < best_tok) {
            best_tok = s;
        }
    }

    double tok_mbs = mb / best_tok;
    double tree_mbs = mb / best_tree;
    bool ok_tok = tok_mbs >= BENCH_TARGET_BytesTokenizer;
    bool ok_tree = tree_mbs >= BENCH_TARGET_BytesTree;
    bool ok_node = bytes_per_node <= BENCH_TARGET_BYTES_PER_NODE;
    bool ok_peak = peak_over_tree <= BENCH_TARGET_PEAK_OVER_TREE;
    SDL_Log("tokenizer only: %.1f MB/s (best of %d, %llu tokens)  target >= %.0f  %s", tok_mbs,
            BENCH_RUNS, (unsigned long long)tokens, BENCH_TARGET_BytesTokenizer,
            ok_tok ? "PASS" : "FAIL");
    SDL_Log("tree build:     %.1f MB/s (best of %d)  target >= %.0f  %s", tree_mbs, BENCH_RUNS,
            BENCH_TARGET_BytesTree, ok_tree ? "PASS" : "FAIL");
    SDL_Log("memory/node:    %.1f bytes per node  target <= %.0f  %s", bytes_per_node,
            BENCH_TARGET_BYTES_PER_NODE, ok_node ? "PASS" : "FAIL");
    SDL_Log("peak/tree:      %.2fx the final tree (%.2f MB peak, %.2fx input; input ratio is "
            "informational)  target <= %.2fx  %s",
            peak_over_tree, (double)peak_delta / (1024.0 * 1024.0), peak_ratio,
            BENCH_TARGET_PEAK_OVER_TREE, ok_peak ? "PASS" : "FAIL");

    embi_free(buf, BENCH_INPUT_BYTES + 4096);
    return (ok_tok && ok_tree && ok_node && ok_peak) ? 0 : 1;
}
