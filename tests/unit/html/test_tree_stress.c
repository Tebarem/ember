/*
 * Tree builder stress: deep nesting of formatting elements must not turn the adoption agency
 * algorithm, the list of active formatting elements or the stack searches quadratic. Each case
 * parses a document with tens of thousands of nested elements and has a generous time bound
 * (several times the measured cost, so it only trips on an accidental O(n^2) or worse).
 */
#include "emb_test.h"

#include "base/base.h"
#include "dom/dom.h"
#include "html/parser.h"

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

#include <stdio.h>
#include <string.h>

#define STRESS_SECONDS 20.0
/* Sanitizer builds run several times slower, and ctest runs them in parallel. */
#define STRESS_SLOWDOWN (EMB_TEST_ASAN ? 10.0 : 1.0)

static double parse_timed(const char *s, size_t n, embi_document *doc)
{
    uint64_t t0 = SDL_GetPerformanceCounter();
    embi_html_parser *p = embi_html_parser_new(doc, nullptr);
    embi_html_parser_write(p, s, n);
    embi_html_parser_end(p);
    embi_html_parser_destroy(p);
    return (double)(SDL_GetPerformanceCounter() - t0) / (double)SDL_GetPerformanceFrequency();
}

static size_t count_elements(const embi_node *root)
{
    size_t c = 0;
    const embi_node *n = root;
    while (n) {
        c += n->type == EMBI_NODE_ELEMENT;
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
    return c;
}

static void append(char **buf, size_t *len, size_t *cap, const char *s)
{
    size_t n = strlen(s);
    if (*len + n + 1 > *cap) {
        size_t nc = *cap ? *cap * 2 : 1024;
        while (nc < *len + n + 1) {
            nc *= 2;
        }
        char *nb = embi_alloc(nc);
        if (*buf) {
            memcpy(nb, *buf, *len);
            embi_free(*buf, *cap);
        }
        *buf = nb;
        *cap = nc;
    }
    memcpy(*buf + *len, s, n + 1);
    *len += n;
}

/* Runs one case: open_fmt is repeated n times, then middle, then close_fmt n times. */
static void run_case(const char *name, const char *open_fmt, const char *middle,
                     const char *close, unsigned n, size_t min_elements)
{
    char *buf = nullptr;
    size_t len = 0;
    size_t cap = 0;
    append(&buf, &len, &cap, "<body>");
    char piece[96];
    for (unsigned i = 0; i < n; i++) {
        snprintf(piece, sizeof piece, open_fmt, i);
        append(&buf, &len, &cap, piece);
    }
    append(&buf, &len, &cap, middle);
    for (unsigned i = 0; i < n; i++) {
        append(&buf, &len, &cap, close);
    }
    embi_document *doc = embi_document_new(true);
    double secs = parse_timed(buf, len, doc);
    size_t els = count_elements(&doc->base);
    printf("  %-28s %6u levels, %8zu bytes, %7zu elements, %.3f s\n", name, n, len, els, secs);
    EMB_CHECK(secs < STRESS_SECONDS * STRESS_SLOWDOWN);
    EMB_CHECK(els >= min_elements);
    embi_document_free(doc);
    embi_free(buf, cap);
}

EMB_TEST(deep_identical_formatting)
{
    run_case("identical <b> then </b>", "<b>", "x", "</b>", 40000, 40000);
}

EMB_TEST(deep_formatting_distinct_attributes)
{
    /* Distinct attributes defeat Noah's Ark, so the list of active formatting elements grows. */
    run_case("<b id=N> then </b>", "<b id=\"i%u\">", "x", "</b>", 20000, 20000);
}

EMB_TEST(formatting_closed_across_deep_blocks)
{
    /* Every </b> runs the adoption agency algorithm with a furthest block far above it. */
    run_case("<b><div> then </b>", "<b><div>", "x", "</b>", 5000, 10000);
}

EMB_TEST(deep_div_nesting)
{
    run_case("<div> then </div>", "<div>", "x", "</div>", 100000, 100000);
}

EMB_TEST(mixed_formatting_misnested)
{
    run_case("<a><b><i> then </a></b></i>", "<b><i>", "x", "</b></i>", 20000, 40000);
}

/* Total length (UTF-16 units) of all text nodes under root, including template contents. */
static size_t text_units(const embi_node *root)
{
    size_t c = 0;
    const embi_node *n = root;
    while (n) {
        if (n->type == EMBI_NODE_TEXT) {
            c += ((const embi_chardata *)n)->data.len;
        }
        if (n->type == EMBI_NODE_ELEMENT && n->name == EMBI_ATOM_template &&
            n->extra == EMBI_NS_HTML) {
            c += text_units(embi_template_contents((embi_element *)n));
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
    return c;
}

/* Long flat text inside a container, written whole or in chunks. Text appends must be linear. */
static void flat_text_case(const char *name, const char *open, const char *close, size_t chunk)
{
    enum { WORDS = 200000 }; /* "word " x 200000 = 1 MB */
    size_t wl = strlen("word ");
    char *buf = nullptr;
    size_t len = 0;
    size_t cap = 0;
    append(&buf, &len, &cap, "<!doctype html><body>");
    append(&buf, &len, &cap, open);
    for (unsigned i = 0; i < WORDS; i++) {
        append(&buf, &len, &cap, "word ");
    }
    append(&buf, &len, &cap, close);
    embi_document *doc = embi_document_new(true);
    uint64_t t0 = SDL_GetPerformanceCounter();
    embi_html_parser *p = embi_html_parser_new(doc, nullptr);
    for (size_t off = 0; off < len; off += chunk) {
        size_t n = len - off < chunk ? len - off : chunk;
        embi_html_parser_write(p, buf + off, n);
    }
    embi_html_parser_end(p);
    embi_html_parser_destroy(p);
    double secs = (double)(SDL_GetPerformanceCounter() - t0) / (double)SDL_GetPerformanceFrequency();
    size_t units = text_units(&doc->base);
    printf("  %-28s chunk %8zu, %8zu bytes, %8zu text units, %.3f s\n", name, chunk, len, units,
           secs);
    EMB_CHECK(secs < STRESS_SECONDS * STRESS_SLOWDOWN);
    EMB_CHECK(units >= WORDS * wl);
    embi_document_free(doc);
    embi_free(buf, cap);
}

static void flat_all_chunks(const char *name, const char *open, const char *close)
{
    flat_text_case(name, open, close, (size_t)1 << 30);
    flat_text_case(name, open, close, 4096);
}

EMB_TEST(flat_text_in_cell)
{
    flat_all_chunks("td text", "<table><tr><td>", "</td></tr></table>");
}

EMB_TEST(flat_text_in_template)
{
    flat_all_chunks("template text", "<template>", "</template>");
}

EMB_TEST(flat_text_in_caption)
{
    flat_all_chunks("caption text", "<table><caption>", "</caption></table>");
}

EMB_TEST(flat_text_in_svg)
{
    flat_all_chunks("svg text", "<svg><text>", "</text></svg>");
}

EMB_TEST(flat_text_in_select)
{
    flat_all_chunks("select text", "<select><option>", "</option></select>");
}

EMB_TEST(flat_text_after_body)
{
    flat_all_chunks("after body text", "</body>", "");
}

EMB_TEST(flat_text_in_table_foster)
{
    flat_all_chunks("table text (foster)", "<table>", "</table>");
}

EMB_TEST(many_siblings)
{
    run_case("<p> siblings", "<p>%u", "", "", 100000, 100000);
}
/* One byte per write into one text node: each write must leave the DOM current without copying
 * the whole node back, so this stays linear (1 MB must take well under a second). */
EMB_TEST(one_byte_writes_into_one_text_node)
{
    enum { N = 1 << 20 };
    embi_document *doc = embi_document_new(true);
    embi_html_parser *p = embi_html_parser_new(doc, nullptr);
    embi_html_parser_write(p, "<body>", 6);
    uint64_t t0 = SDL_GetPerformanceCounter();
    for (size_t i = 0; i < N; i++) {
        char c = (char)('a' + (i % 26));
        embi_html_parser_write(p, &c, 1);
    }
    embi_html_parser_end(p);
    double dt = (double)(SDL_GetPerformanceCounter() - t0) / (double)SDL_GetPerformanceFrequency();
    embi_html_parser_destroy(p);
    printf("one-byte writes: %.3f s\n", dt);
    EMB_CHECK(dt < 1.0 * STRESS_SLOWDOWN);
    embi_node *body = &embi_document_element(doc)->base;
    body = body->last_child;
    EMB_CHECK(body && body->first_child && body->first_child == body->last_child);
    EMB_CHECK(((embi_chardata *)body->first_child)->data.len == N);
    embi_document_free(doc);
}

EMB_TEST_MAIN()
