// fuzz-modules: css dom loader
/* CSS syntax and stylesheets. Input: byte 0 = mode, rest = CSS text (or bytes).
 * Modes (byte 0 & 7): 0 stylesheet through embi_css_sheet_parse; 1 the same with the property
 * database hooks and an @import loader that serves slices of the input back (cycles, depth);
 * 2 embi_css_sheet_parse_bytes (BOM, @charset, UTF-16) with a protocol label from byte 1;
 * 3 the CSS Syntax entry points one by one (tokenize, rules, declarations, block contents, single
 * rule, single declaration, single component value); 4 the stylesheet model walked into selectors
 * and declaration values; 5 the tokenizer into a vec plus component values. */
#define FUZZ_RESET_HOOKS() embi_sel_lower_cache_reset();
#include "css/css.h"
#include "css/props.h"
#include "css/selector.h"
#include "fuzz_common.h"

typedef struct loader_state {
    const uint8_t *p;
    size_t n;
    unsigned fetches;
} loader_state;

static bool fetch_cb(void *user, embi_str base, embi_str url, embi_str *resolved,
                     const unsigned char **bytes, size_t *n)
{
    loader_state *s = user;
    (void)base;
    s->fetches++;
    if (s->fetches > 64 || s->n == 0) {
        return false;
    }
    /* A resolved name from the url's length: several spellings map to the same file. */
    static const char *const names[] = {"https://x/a.css", "https://x/b.css", "https://x/c.css"};
    const char *nm = names[url.len % 3];
    *resolved = (embi_str){nm, (uint32_t)strlen(nm)};
    size_t cut = s->n > 16 ? s->n - (url.len % 16) : s->n;
    *bytes = s->p;
    *n = cut;
    return true;
}

static bool selector_ok_cb(void *user, embi_css_slice selector)
{
    embi_arena *a = user;
    return embi_sel_parse(a, selector, nullptr) != nullptr;
}

static void touch_value(embi_arena *a, embi_prop_id prop, embi_css_slice v)
{
    embi_longhand lh[EMBI_LONGHAND_MAX];
    uint32_t n = embi_css_parse_decl(a, prop, v, lh, EMBI_LONGHAND_MAX);
    embi_sb sb = {0};
    for (uint32_t i = 0; i < n; i++) {
        embi_value_serialize(&sb, &lh[i].value);
    }
    embi_sb_free(&sb);
}

static void walk(embi_arena *a, const embi_css_rule *r, unsigned depth)
{
    if (depth > 64) {
        return;
    }
    if (r->kind == EMBI_CSS_RULE_STYLE || r->kind == EMBI_CSS_RULE_SCOPE) {
        (void)embi_sel_parse(a, r->prelude, nullptr);
        (void)embi_sel_parse_relative(a, r->prelude, nullptr);
    }
    for (uint32_t i = 0; i < r->ndecls; i++) {
        const embi_css_decl *d = &r->decls[i];
        embi_str nm = embi_atom_str(d->name);
        embi_prop_id id = embi_prop_lookup(nm);
        if (id != 0) {
            touch_value(a, id, d->value);
        }
    }
    if (r->sheet) {
        for (uint32_t i = 0; i < r->sheet->nrules && depth < 8; i++) {
            walk(a, &r->sheet->rules[i], depth + 1);
        }
    }
    for (uint32_t i = 0; i < r->nkids; i++) {
        walk(a, &r->kids[i], depth + 1);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    FUZZ_GUARD_BEGIN();
    fuzz_in in = {data, size};
    uint8_t mode = fuzz_u8(&in) & 7;
    embi_arena a = {0};
    embi_arena_init(&a, 4096);
    loader_state ls = {in.p, in.n, 0};
    embi_css_loader loader = {&ls, fetch_cb};
    embi_css_hooks hooks = {nullptr, embi_prop_decl_ok, selector_ok_cb};
    hooks.user = &a;
    embi_css_parse_opts opts = {&loader, &hooks, EMBI_STR("https://x/root.css")};
    embi_str text = fuzz_str(&in);

    switch (mode) {
    case 0: {
        embi_css_sheet *s = embi_css_sheet_parse(text, nullptr);
        embi_css_sheet_unref(s);
        break;
    }
    case 1: {
        embi_css_sheet *s = embi_css_sheet_parse(text, &opts);
        embi_css_sheet_ref(s);
        embi_css_sheet_unref(s);
        embi_css_sheet_unref(s);
        break;
    }
    case 2: {
        uint8_t proto = fuzz_u8(&in);
        static const char *const labels[] = {"", "utf-8", "utf-16le", "utf-16be", "windows-1252",
                                             "iso-8859-1", "gbk", "x-bogus"};
        const char *pl = labels[proto & 7];
        const char *el = labels[(proto >> 3) & 7];
        const char *enc = nullptr;
        embi_css_sheet *s = embi_css_sheet_parse_bytes(in.p, in.n, (embi_str){pl, (uint32_t)strlen(pl)},
                                                       (embi_str){el, (uint32_t)strlen(el)}, &opts, &enc);
        if (s) {
            embi_css_sheet_unref(s);
        }
        embi_str txt = {nullptr, 0};
        (void)embi_css_decode_bytes(&a, in.p, in.n, (embi_str){pl, (uint32_t)strlen(pl)}, (embi_str){el, (uint32_t)strlen(el)},
                                    &txt, &enc);
        break;
    }
    case 3: {
        (void)embi_css_tokenize(&a, text);
        embi_css_slice cvs = embi_css_parse_cvs(&a, text);
        (void)embi_css_consume_rules(&a, cvs, true);
        (void)embi_css_consume_rules(&a, cvs, false);
        (void)embi_css_consume_declarations(&a, cvs);
        (void)embi_css_consume_block_contents(&a, cvs);
        (void)embi_css_parse_rule(&a, cvs);
        (void)embi_css_parse_declaration(&a, cvs);
        const embi_css_cv *one = nullptr;
        (void)embi_css_parse_component_value(cvs, &one);
        break;
    }
    case 4: {
        embi_css_sheet *s = embi_css_sheet_parse(text, &opts);
        for (uint32_t i = 0; i < s->nrules; i++) {
            walk(&a, &s->rules[i], 0);
        }
        embi_css_sheet_unref(s);
        break;
    }
    default: {
        embi_css_token_vec v = {0};
        embi_css_tokenize_vec(&a, text, &v);
        embi_vec_free(&v);
        break;
    }
    }
    embi_arena_free(&a);
    FUZZ_GUARD_END();
    return 0;
}
