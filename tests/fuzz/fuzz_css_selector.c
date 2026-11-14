// fuzz-modules: css html dom loader
/* Selector parsing and matching. Input: byte 0 = mode, rest = selector text. A NUL byte in the text
 * splits a second text off (used as the nesting parent). Modes (byte 0 & 7): 0 selector list,
 * 1 relative selector list, 2 An+B, 3 nested selector with a parsed parent list, 4 selector index
 * and candidate walk. Bit 3 adds a namespace map (default and "svg" prefixes). The parsed list is
 * matched against every element of a fixed document that covers forms, tables, lang, dir, ids,
 * classes, attributes, templates and foreign content; the matching context has state hooks that
 * answer from the element address, so the state pseudo-classes get both outcomes. */
#define FUZZ_RESET_HOOKS() embi_html_name_cache_reset(); embi_sel_lower_cache_reset();
#include "css/selector.h"
#include "dom/dom.h"
#include "html/names.h"
#include "html/parser.h"
#include "fuzz_common.h"

static const char doc_html[] =
    "<!doctype html><html lang=en-US dir=ltr><head><title>t</title><style>x{}</style></head>"
    "<body id=b class='a b c'><div id=d1 class=a data-x=\"1 2\" title=Foo><p class='b' lang=fr>text"
    "<span id=s1 class=c dir=rtl>x</span><em></em></p><input type=checkbox checked disabled>"
    "<input type=text value=v required><select><optgroup label=g><option selected>o1<option>o2"
    "</optgroup></select><a href=\"#\" class=\"a\">link</a><button disabled>b</button>"
    "<table><caption>c</caption><thead><tr><th>h<tbody><tr><td>1<td>2<tr><td>3</table>"
    "<ul><li>1<li class=b>2<li>3</ul><details open><summary>s</summary>d</details>"
    "<template><i class=in-template>t</i></template><svg id=sv class=a><circle r=1 /><foreignObject>"
    "<div class=fo></div></foreignObject></svg><math><mi>x</mi></math><textarea>t</textarea>"
    "<progress value=1 max=2></progress><meter value=1></meter><fieldset disabled><input></fieldset>"
    "<img src=x alt=y><video controls></video><dialog open></dialog><div hidden></div>"
    "<p></p><div><!--c--></div></div></body></html>";

static bool state_cb(void *user, embi_element *el, embi_sel_pc pc)
{
    uintptr_t salt = (uintptr_t)user;
    return (((uintptr_t)el >> 4) + (uintptr_t)pc + salt) % 3 == 0;
}

static void match_all(embi_document *doc, const embi_sel_list *l, bool with_hooks)
{
    if (!l) {
        return;
    }
    embi_sel_hooks hooks = {(void *)(uintptr_t)l->n, state_cb};
    embi_sel_ctx ctx;
    embi_sel_ctx_init(&ctx, nullptr, with_hooks ? &hooks : nullptr);
    embi_sel_ctx scoped;
    embi_element *scope = nullptr;
    unsigned idx = 0;
    for (embi_node *n = embi_next_in_tree(&doc->base, &doc->base); n; n = embi_next_in_tree(&doc->base, n)) {
        if (n->type != EMBI_NODE_ELEMENT) {
            continue;
        }
        idx++;
        if (idx == 7) {
            scope = (embi_element *)n;
        }
        (void)embi_sel_match_list(l, (embi_element *)n, &ctx);
        for (uint32_t i = 0; i < l->n; i++) {
            (void)embi_sel_match(&l->items[i], (embi_element *)n, EMBI_PE_NONE, &ctx, nullptr);
        }
    }
    embi_sel_ctx_free(&ctx);
    if (scope) {
        embi_sel_ctx_init(&scoped, scope, with_hooks ? &hooks : nullptr);
        for (embi_node *n = embi_next_in_tree(&doc->base, &doc->base); n; n = embi_next_in_tree(&doc->base, n)) {
            if (n->type == EMBI_NODE_ELEMENT) {
                (void)embi_sel_match_list(l, (embi_element *)n, &scoped);
            }
        }
        embi_sel_ctx_free(&scoped);
    }
}

typedef struct cand {
    unsigned hits;
} cand;

static void on_cand(void *user, uint64_t payload)
{
    (void)payload;
    ((cand *)user)->hits++;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    FUZZ_GUARD_BEGIN();
    fuzz_in in = {data, size};
    uint8_t mode = fuzz_u8(&in);
    embi_arena a = {0};
    embi_arena_init(&a, 4096);

    /* Split at the first NUL. */
    const uint8_t *nul = in.n ? memchr(in.p, 0, in.n) : nullptr;
    embi_str first = {in.n ? (const char *)in.p : "", (uint32_t)(nul ? (size_t)(nul - in.p) : in.n)};
    embi_str second = {"", 0};
    if (nul) {
        second = (embi_str){(const char *)nul + 1, (uint32_t)(in.n - (size_t)(nul - in.p) - 1)};
    }

    embi_sel_nsmap map[2] = {{0, embi_atom_intern("http://www.w3.org/1999/xhtml", 28)},
                             {embi_atom_intern("svg", 3), embi_atom_intern("http://www.w3.org/2000/svg", 26)}};
    embi_sel_env env = {(mode & 8) ? map : nullptr, (mode & 8) ? 2u : 0u, nullptr};

    embi_document *doc = embi_document_new(true);
    embi_html_parser *hp = embi_html_parser_new(doc, nullptr);
    embi_html_parser_write(hp, doc_html, sizeof doc_html - 1);
    embi_html_parser_end(hp);
    embi_html_parser_destroy(hp);

    switch (mode & 7) {
    case 0:
        match_all(doc, embi_sel_parse_text(&a, first, &env), (mode & 16) != 0);
        break;
    case 1: {
        embi_css_slice cvs = embi_css_parse_cvs(&a, first);
        match_all(doc, embi_sel_parse_relative(&a, cvs, &env), (mode & 16) != 0);
        break;
    }
    case 2: {
        embi_css_slice cvs = embi_css_parse_cvs(&a, first);
        int64_t x = 0;
        int64_t y = 0;
        (void)embi_sel_parse_anb(cvs, &x, &y);
        break;
    }
    case 3: {
        const embi_sel_list *parent = embi_sel_parse_text(&a, first, &env);
        embi_sel_env nested = env;
        nested.nest_parent = parent;
        const embi_sel_list *child = embi_sel_parse_text(&a, second, &nested);
        match_all(doc, child, (mode & 16) != 0);
        break;
    }
    default: {
        const embi_sel_list *l = embi_sel_parse_text(&a, first, &env);
        embi_sel_index ix = {0};
        if (l) {
            for (uint32_t i = 0; i < l->n; i++) {
                embi_sel_index_add(&ix, &l->items[i], i);
            }
            cand c = {0};
            for (embi_node *n = embi_next_in_tree(&doc->base, &doc->base); n; n = embi_next_in_tree(&doc->base, n)) {
                if (n->type == EMBI_NODE_ELEMENT) {
                    embi_sel_index_candidates(&ix, (embi_element *)n, on_cand, &c);
                }
            }
        }
        embi_sel_index_free(&ix);
        break;
    }
    }
    embi_document_free(doc);
    embi_arena_free(&a);
    FUZZ_GUARD_END();
    return 0;
}
