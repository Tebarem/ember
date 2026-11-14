/*
 * CSS Syntax Level 3, section 5 (parsers) and the byte stream decoding of section 3.2.
 *
 * The whole input is tokenized and turned into a component value tree first. Blocks are context
 * free in CSS, so every parser below works on slices of that tree. A block's contents are then a
 * slice of its kids.
 */
#include "css/css.h"
#include "loader/loader.h"

#include <string.h>

typedef EMBI_VEC(embi_css_item) item_vec;

/* ------------------------------------------------------------------ small helpers */

static bool is_ws(const embi_css_cv *c)
{
    return c->tok.type == EMBI_CSS_WS;
}

static bool is_type(const embi_css_cv *c, embi_css_type t)
{
    return c->tok.type == t;
}

static bool is_ident_ci(const embi_css_cv *c, const char *lit)
{
    return c->tok.type == EMBI_CSS_IDENT &&
           embi_str_ieq(c->tok.value, (embi_str){lit, (uint32_t)strlen(lit)});
}

static bool is_delim(const embi_css_cv *c, uint32_t cp)
{
    return c->tok.type == EMBI_CSS_DELIM && c->tok.cp == cp;
}

static uint32_t skip_ws(embi_css_slice in, uint32_t i)
{
    while (i < in.n && is_ws(&in.p[i])) {
        i++;
    }
    return i;
}

static uint32_t find_semi(embi_css_slice in, uint32_t i)
{
    while (i < in.n && !is_type(&in.p[i], EMBI_CSS_SEMICOLON)) {
        i++;
    }
    return i;
}

static bool has_lbrace(embi_css_slice in, uint32_t i, uint32_t j)
{
    for (uint32_t k = i; k < j; k++) {
        if (is_type(&in.p[k], EMBI_CSS_LBRACE)) {
            return true;
        }
    }
    return false;
}

/* True when [a, b) has a top-level {} block and at least one other non-whitespace value. */
static bool brace_with_other(embi_css_slice in, uint32_t a, uint32_t b)
{
    uint32_t braces = 0;
    uint32_t others = 0;
    for (uint32_t k = a; k < b; k++) {
        if (is_ws(&in.p[k])) {
            continue;
        }
        if (is_type(&in.p[k], EMBI_CSS_LBRACE)) {
            braces++;
        } else {
            others++;
        }
        if (braces > 0 && braces + others >= 2) {
            return true;
        }
    }
    return false;
}

/* Source offset just past the last non-whitespace component value before index to. */
static uint32_t last_end(embi_css_slice in, uint32_t to)
{
    uint32_t k = to;
    while (k > 0 && is_ws(&in.p[k - 1])) {
        k--;
    }
    return k > 0 ? in.p[k - 1].end : 0;
}

static bool is_custom_name(embi_str name)
{
    return name.len >= 2 && name.ptr[0] == '-' && name.ptr[1] == '-';
}

static embi_css_slice sub(embi_css_slice in, uint32_t a, uint32_t b)
{
    return embi_css_slice_make(in.p + a, b - a);
}

static embi_css_item error_item(const char *why)
{
    embi_css_item it;
    memset(&it, 0, sizeof it);
    it.kind = EMBI_CSS_ITEM_ERROR;
    it.error = why;
    return it;
}

static void items_push(item_vec *v, embi_css_item it)
{
    embi_vec_push(v, it);
}

static embi_css_items items_finish(embi_arena *a, item_vec *v)
{
    embi_css_items r;
    r.n = v->len;
    r.p = embi_css_arena_copy(a, v->data, v->len, sizeof(embi_css_item));
    embi_vec_free(v);
    return r;
}

/* ------------------------------------------------------------------ consumers (section 5.4) */

/* Consume an at-rule. in.p[*ip] is the at-keyword. */
static embi_css_item consume_at(embi_css_slice in, uint32_t *ip)
{
    uint32_t s = *ip;
    embi_css_item it;
    memset(&it, 0, sizeof it);
    it.kind = EMBI_CSS_ITEM_AT;
    it.name = in.p[s].tok.value;
    it.start = in.p[s].tok.start;

    uint32_t ps = s + 1;
    uint32_t pe = in.n;
    uint32_t i = ps;
    while (i < in.n) {
        const embi_css_cv *c = &in.p[i];
        if (is_type(c, EMBI_CSS_SEMICOLON)) {
            pe = i;
            i++;
            break;
        }
        if (is_type(c, EMBI_CSS_LBRACE)) {
            pe = i;
            it.block = embi_css_slice_make(c->kids, c->nkids);
            it.has_block = true;
            i++;
            break;
        }
        i++;
    }
    it.prelude = sub(in, ps, pe);
    it.end = last_end(in, i);
    *ip = i;
    return it;
}

/* Consume a qualified rule starting at *ip. Returns false (and consumes everything) at EOF. */
static bool consume_qualified(embi_css_slice in, uint32_t *ip, embi_css_item *out)
{
    uint32_t s = *ip;
    for (uint32_t i = s; i < in.n; i++) {
        const embi_css_cv *c = &in.p[i];
        if (is_type(c, EMBI_CSS_LBRACE)) {
            memset(out, 0, sizeof *out);
            out->kind = EMBI_CSS_ITEM_QUALIFIED;
            out->prelude = sub(in, s, i);
            out->block = embi_css_slice_make(c->kids, c->nkids);
            out->start = in.p[s].tok.start;
            out->end = c->end;
            *ip = i + 1;
            return true;
        }
    }
    *ip = in.n;
    return false;
}

/* Consume a declaration from the whole slice (CSS Syntax 5.4.5). Leading whitespace is skipped.
 * The value runs to the end of the slice. A non-custom property whose value has a top-level {}
 * block next to other values is not a declaration: it returns false, and the block contents
 * parser may read the same tokens as a nested rule. */
static bool decl_consume(embi_css_slice in, embi_css_item *out)
{
    uint32_t ni = skip_ws(in, 0);
    if (ni >= in.n || !is_type(&in.p[ni], EMBI_CSS_IDENT)) {
        return false;
    }
    embi_str name = in.p[ni].tok.value;
    uint32_t i = skip_ws(in, ni + 1);
    if (i >= in.n || !is_type(&in.p[i], EMBI_CSS_COLON)) {
        return false;
    }
    uint32_t vs = i + 1;
    uint32_t ve = in.n;

    bool important = false;
    uint32_t j = ve;
    while (j > vs && is_ws(&in.p[j - 1])) {
        j--;
    }
    if (j > vs && is_ident_ci(&in.p[j - 1], "important")) {
        uint32_t k = j - 1;
        while (k > vs && is_ws(&in.p[k - 1])) {
            k--;
        }
        if (k > vs && is_delim(&in.p[k - 1], '!')) {
            important = true;
            ve = k - 1;
        }
    }
    if (!is_custom_name(name) && brace_with_other(in, vs, ve)) {
        return false;
    }

    memset(out, 0, sizeof *out);
    out->kind = EMBI_CSS_ITEM_DECL;
    out->name = name;
    out->important = important;
    out->prelude = sub(in, vs, ve);
    out->start = in.p[ni].tok.start;
    out->end = last_end(in, in.n);
    return true;
}

/* ------------------------------------------------------------------ list parsers (section 5.4) */

embi_css_items embi_css_consume_rules(embi_arena *a, embi_css_slice in, bool top_level)
{
    item_vec v = {0};
    uint32_t i = 0;
    while (i < in.n) {
        const embi_css_cv *c = &in.p[i];
        if (is_ws(c)) {
            i++;
            continue;
        }
        if (top_level && (is_type(c, EMBI_CSS_CDO) || is_type(c, EMBI_CSS_CDC))) {
            i++;
            continue;
        }
        if (is_type(c, EMBI_CSS_AT_KEYWORD)) {
            items_push(&v, consume_at(in, &i));
            continue;
        }
        embi_css_item it;
        if (consume_qualified(in, &i, &it)) {
            items_push(&v, it);
        } else {
            items_push(&v, error_item("invalid"));
        }
    }
    return items_finish(a, &v);
}

embi_css_items embi_css_consume_declarations(embi_arena *a, embi_css_slice in)
{
    item_vec v = {0};
    uint32_t i = 0;
    while (i < in.n) {
        const embi_css_cv *c = &in.p[i];
        if (is_ws(c) || is_type(c, EMBI_CSS_SEMICOLON)) {
            i++;
            continue;
        }
        if (is_type(c, EMBI_CSS_AT_KEYWORD)) {
            items_push(&v, consume_at(in, &i));
            continue;
        }
        uint32_t j = find_semi(in, i);
        embi_css_item it;
        if (is_type(c, EMBI_CSS_IDENT) && decl_consume(sub(in, i, j), &it)) {
            items_push(&v, it);
        } else {
            items_push(&v, error_item("invalid"));
        }
        i = j;
    }
    return items_finish(a, &v);
}

embi_css_items embi_css_consume_block_contents(embi_arena *a, embi_css_slice in)
{
    item_vec v = {0};
    uint32_t i = 0;
    /* Cached index of the next top-level semicolon at or after some earlier i. Without it a block
     * of N semicolon-free nested rules would rescan to the end N times. */
    uint32_t semi = 0;
    bool semi_valid = false;
    while (i < in.n) {
        const embi_css_cv *c = &in.p[i];
        if (is_ws(c) || is_type(c, EMBI_CSS_SEMICOLON)) {
            i++;
            continue;
        }
        if (is_type(c, EMBI_CSS_AT_KEYWORD)) {
            items_push(&v, consume_at(in, &i));
            continue;
        }
        if (!semi_valid || semi < i) {
            semi = find_semi(in, i);
            semi_valid = true;
        }
        uint32_t j = semi;
        embi_css_item it;
        if (is_type(c, EMBI_CSS_IDENT) && decl_consume(sub(in, i, j), &it)) {
            items_push(&v, it);
            i = j;
            continue;
        }
        /* Not a declaration. A qualified rule (a:hover {...}) is only possible when a '{' occurs
         * before the next semicolon; the scan never goes past it. Otherwise the item is an invalid
         * declaration or rule that ends at the semicolon. */
        if (has_lbrace(in, i, j) && consume_qualified(in, &i, &it)) {
            items_push(&v, it);
            continue;
        }
        items_push(&v, error_item("invalid"));
        i = j;
    }
    return items_finish(a, &v);
}

/* ------------------------------------------------------------------ single-item parsers */

embi_css_item embi_css_parse_rule(embi_arena *a, embi_css_slice in)
{
    (void)a;
    uint32_t i = skip_ws(in, 0);
    if (i >= in.n) {
        return error_item("empty");
    }
    embi_css_item it;
    if (is_type(&in.p[i], EMBI_CSS_AT_KEYWORD)) {
        it = consume_at(in, &i);
    } else if (!consume_qualified(in, &i, &it)) {
        return error_item("invalid");
    }
    if (skip_ws(in, i) < in.n) {
        return error_item("extra-input");
    }
    return it;
}

embi_css_item embi_css_parse_declaration(embi_arena *a, embi_css_slice in)
{
    (void)a;
    if (skip_ws(in, 0) >= in.n) {
        return error_item("empty");
    }
    embi_css_item it;
    if (!decl_consume(in, &it)) {
        return error_item("invalid");
    }
    return it;
}

const char *embi_css_parse_component_value(embi_css_slice in, const embi_css_cv **out)
{
    uint32_t i = skip_ws(in, 0);
    if (i >= in.n) {
        return "empty";
    }
    *out = &in.p[i];
    i = skip_ws(in, i + 1);
    if (i < in.n) {
        return "extra-input";
    }
    return nullptr;
}

/* ------------------------------------------------------------------ component value tree */

typedef struct cv_frame {
    embi_css_token opener;
    uint8_t close;
    uint32_t start; /* index in the scratch vector where the contents begin */
} cv_frame;

typedef EMBI_VEC(embi_css_cv) cv_vec;
typedef EMBI_VEC(cv_frame) frame_vec;

/* Closes the innermost open block. end is the offset just past its closing token, or the end of
 * the input when the block is still open at EOF. */
static void close_frame(embi_arena *a, cv_vec *scratch, frame_vec *stack, uint32_t end)
{
    cv_frame f = embi_vec_pop(stack);
    uint32_t nk = scratch->len - f.start;
    embi_css_cv node;
    memset(&node, 0, sizeof node);
    node.tok = f.opener;
    node.kids = embi_css_arena_copy(a, nk ? scratch->data + f.start : nullptr, nk,
                                    sizeof(embi_css_cv));
    node.nkids = nk;
    node.end = end;
    scratch->len = f.start;
    embi_vec_push(scratch, node);
}

embi_css_slice embi_css_parse_cvs(embi_arena *a, embi_str text)
{
    /* The token vector is dead once the tree exists, so it stays on the heap. Only the tree and
     * the token strings it refers to go into the arena. */
    embi_css_token_vec toks;
    embi_css_tokenize_vec(a, text, &toks);
    cv_vec scratch = {0};
    frame_vec stack = {0};
    uint32_t last_end_off = 0;
    if (toks.len >= 1024) {
        embi_vec_reserve(&scratch, toks.len / 2 + 16); /* the top level and the open blocks: well under the tokens */
    }

    for (uint32_t k = 0; k < toks.len; k++) {
        const embi_css_token *t = &toks.data[k];
        last_end_off = t->end;
        uint8_t close = 0;
        switch (t->type) {
        case EMBI_CSS_LBRACE:
            close = EMBI_CSS_RBRACE;
            break;
        case EMBI_CSS_LBRACKET:
            close = EMBI_CSS_RBRACKET;
            break;
        case EMBI_CSS_LPAREN:
        case EMBI_CSS_FUNCTION:
            close = EMBI_CSS_RPAREN;
            break;
        default:
            break;
        }
        if (close != 0) {
            cv_frame f;
            f.opener = *t;
            f.close = close;
            f.start = scratch.len;
            embi_vec_push(&stack, f);
            continue;
        }
        if (stack.len != 0 && t->type == embi_vec_at(&stack, stack.len - 1).close) {
            close_frame(a, &scratch, &stack, t->end);
            continue;
        }
        embi_css_cv node;
        memset(&node, 0, sizeof node);
        node.tok = *t;
        node.end = t->end;
        embi_vec_push(&scratch, node);
    }
    while (stack.len != 0) {
        close_frame(a, &scratch, &stack, last_end_off);
    }

    embi_css_slice r;
    r.n = scratch.len;
    r.p = embi_css_arena_copy(a, scratch.data, scratch.len, sizeof(embi_css_cv));
    embi_vec_free(&scratch);
    embi_vec_free(&stack);
    embi_vec_free(&toks);
    return r;
}

/* ------------------------------------------------------------------ byte stream decoding */

/* Canonical name for a label (ASCII whitespace trimmed, case-insensitive), or nullptr when the
 * label is not a WHATWG label at all. The label table is shared with the loader (W7a), which
 * owns the Encoding Standard tables. */
static const char *label_name(const unsigned char *p, size_t n)
{
    return embi_encoding_label_name(p, n);
}

/* @charset "X"; must match these exact bytes at the start, and X must be ASCII-compatible. A
 * UTF-16 label is not believed: the result is UTF-8, which stops detection (CSS Syntax 3.2). */
static const char *charset_prescan(const unsigned char *p, size_t n)
{
    static const char pre[] = "@charset \"";
    size_t k = sizeof pre - 1;
    if (n < k || memcmp(p, pre, k) != 0) {
        return nullptr;
    }
    size_t e = k;
    while (e < n && p[e] != '"') {
        e++;
    }
    if (e + 1 >= n || p[e + 1] != ';') {
        return nullptr;
    }
    const char *label = label_name(p + k, e - k);
    if (label == nullptr) {
        return nullptr;
    }
    if (strcmp(label, "utf-16le") == 0 || strcmp(label, "utf-16be") == 0) {
        return "utf-8";
    }
    return label;
}

/* Upper halves (0x80..0xFF) of the legacy single-byte encodings, per the WHATWG index tables. */
static const uint16_t tab_iso8859_2[128] = {
    0x0080, 0x0081, 0x0082, 0x0083, 0x0084, 0x0085, 0x0086, 0x0087,
    0x0088, 0x0089, 0x008A, 0x008B, 0x008C, 0x008D, 0x008E, 0x008F,
    0x0090, 0x0091, 0x0092, 0x0093, 0x0094, 0x0095, 0x0096, 0x0097,
    0x0098, 0x0099, 0x009A, 0x009B, 0x009C, 0x009D, 0x009E, 0x009F,
    0x00A0, 0x0104, 0x02D8, 0x0141, 0x00A4, 0x013D, 0x015A, 0x00A7,
    0x00A8, 0x0160, 0x015E, 0x0164, 0x0179, 0x00AD, 0x017D, 0x017B,
    0x00B0, 0x0105, 0x02DB, 0x0142, 0x00B4, 0x013E, 0x015B, 0x02C7,
    0x00B8, 0x0161, 0x015F, 0x0165, 0x017A, 0x02DD, 0x017E, 0x017C,
    0x0154, 0x00C1, 0x00C2, 0x0102, 0x00C4, 0x0139, 0x0106, 0x00C7,
    0x010C, 0x00C9, 0x0118, 0x00CB, 0x011A, 0x00CD, 0x00CE, 0x010E,
    0x0110, 0x0143, 0x0147, 0x00D3, 0x00D4, 0x0150, 0x00D6, 0x00D7,
    0x0158, 0x016E, 0x00DA, 0x0170, 0x00DC, 0x00DD, 0x0162, 0x00DF,
    0x0155, 0x00E1, 0x00E2, 0x0103, 0x00E4, 0x013A, 0x0107, 0x00E7,
    0x010D, 0x00E9, 0x0119, 0x00EB, 0x011B, 0x00ED, 0x00EE, 0x010F,
    0x0111, 0x0144, 0x0148, 0x00F3, 0x00F4, 0x0151, 0x00F6, 0x00F7,
    0x0159, 0x016F, 0x00FA, 0x0171, 0x00FC, 0x00FD, 0x0163, 0x02D9,
};
static const uint16_t tab_iso8859_5[128] = {
    0x0080, 0x0081, 0x0082, 0x0083, 0x0084, 0x0085, 0x0086, 0x0087,
    0x0088, 0x0089, 0x008A, 0x008B, 0x008C, 0x008D, 0x008E, 0x008F,
    0x0090, 0x0091, 0x0092, 0x0093, 0x0094, 0x0095, 0x0096, 0x0097,
    0x0098, 0x0099, 0x009A, 0x009B, 0x009C, 0x009D, 0x009E, 0x009F,
    0x00A0, 0x0401, 0x0402, 0x0403, 0x0404, 0x0405, 0x0406, 0x0407,
    0x0408, 0x0409, 0x040A, 0x040B, 0x040C, 0x00AD, 0x040E, 0x040F,
    0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417,
    0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E, 0x041F,
    0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427,
    0x0428, 0x0429, 0x042A, 0x042B, 0x042C, 0x042D, 0x042E, 0x042F,
    0x0430, 0x0431, 0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437,
    0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E, 0x043F,
    0x0440, 0x0441, 0x0442, 0x0443, 0x0444, 0x0445, 0x0446, 0x0447,
    0x0448, 0x0449, 0x044A, 0x044B, 0x044C, 0x044D, 0x044E, 0x044F,
    0x2116, 0x0451, 0x0452, 0x0453, 0x0454, 0x0455, 0x0456, 0x0457,
    0x0458, 0x0459, 0x045A, 0x045B, 0x045C, 0x00A7, 0x045E, 0x045F,
};
static const uint16_t tab_windows_1252[128] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
    0x00A0, 0x00A1, 0x00A2, 0x00A3, 0x00A4, 0x00A5, 0x00A6, 0x00A7,
    0x00A8, 0x00A9, 0x00AA, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00AF,
    0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6, 0x00B7,
    0x00B8, 0x00B9, 0x00BA, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x00BF,
    0x00C0, 0x00C1, 0x00C2, 0x00C3, 0x00C4, 0x00C5, 0x00C6, 0x00C7,
    0x00C8, 0x00C9, 0x00CA, 0x00CB, 0x00CC, 0x00CD, 0x00CE, 0x00CF,
    0x00D0, 0x00D1, 0x00D2, 0x00D3, 0x00D4, 0x00D5, 0x00D6, 0x00D7,
    0x00D8, 0x00D9, 0x00DA, 0x00DB, 0x00DC, 0x00DD, 0x00DE, 0x00DF,
    0x00E0, 0x00E1, 0x00E2, 0x00E3, 0x00E4, 0x00E5, 0x00E6, 0x00E7,
    0x00E8, 0x00E9, 0x00EA, 0x00EB, 0x00EC, 0x00ED, 0x00EE, 0x00EF,
    0x00F0, 0x00F1, 0x00F2, 0x00F3, 0x00F4, 0x00F5, 0x00F6, 0x00F7,
    0x00F8, 0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x00FD, 0x00FE, 0x00FF,
};

/* Table for a single-byte legacy encoding we decode, or nullptr. */
static const uint16_t *legacy_table(const char *enc)
{
    if (strcmp(enc, "iso-8859-2") == 0) {
        return tab_iso8859_2;
    }
    if (strcmp(enc, "iso-8859-5") == 0) {
        return tab_iso8859_5;
    }
    if (strcmp(enc, "windows-1252") == 0) {
        return tab_windows_1252;
    }
    return nullptr;
}

bool embi_css_decode_bytes(embi_arena *a, const unsigned char *p, size_t n, embi_str protocol,
                           embi_str env, embi_str *text, const char **enc)
{
    size_t skip = 0;
    const char *e = nullptr;
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) {
        e = "utf-8";
        skip = 3;
    } else if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) {
        e = "utf-16le";
        skip = 2;
    } else if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) {
        e = "utf-16be";
        skip = 2;
    }
    if (e == nullptr) {
        e = label_name((const unsigned char *)protocol.ptr, protocol.len);
    }
    if (e == nullptr) {
        e = charset_prescan(p, n);
    }
    if (e == nullptr) {
        e = label_name((const unsigned char *)env.ptr, env.len);
    }
    if (e == nullptr) {
        e = "utf-8";
    }
    *enc = e;
    if ((uint64_t)n >= UINT32_MAX) {
        embi_log_error("css: stylesheet of %llu bytes is too large", (unsigned long long)n);
        return false;
    }

    const unsigned char *body = p + skip;
    size_t bn = n - skip;
    if (strcmp(e, "utf-8") == 0) {
        text->ptr = (const char *)body;
        text->len = (uint32_t)bn;
        return true;
    }
    if (strcmp(e, "utf-16le") == 0 || strcmp(e, "utf-16be") == 0) {
        bool be = strcmp(e, "utf-16be") == 0;
        size_t units = (bn + 1) / 2;
        if (units > UINT32_MAX / 3) {
            /* Each unit becomes at most three UTF-8 bytes, which must still fit 32 bits. */
            embi_log_error("css: UTF-16 stylesheet of %llu bytes is too large", (unsigned long long)n);
            return false;
        }
        uint16_t *u = embi_arena_alloc(a, units * sizeof(uint16_t), 2);
        for (size_t k = 0; k < units; k++) {
            if (2 * k + 1 < bn) {
                unsigned lo = body[2 * k + (be ? 1 : 0)];
                unsigned hi = body[2 * k + (be ? 0 : 1)];
                u[k] = (uint16_t)((hi << 8) | lo);
            } else {
                u[k] = 0xFFFD;
            }
        }
        size_t m = embi_utf16_to_utf8(u, units, nullptr, 0);
        char *d = embi_arena_alloc(a, m + 1, 1);
        embi_utf16_to_utf8(u, units, d, m);
        d[m] = '\0';
        text->ptr = d;
        text->len = (uint32_t)m;
        return true;
    }
    const uint16_t *tab = legacy_table(e);
    if (tab != nullptr) {
        if (bn > (UINT32_MAX - 1) / 3) {
            embi_log_error("css: stylesheet of %llu bytes is too large", (unsigned long long)n);
            return false;
        }
        char *d = embi_arena_alloc(a, bn * 3 + 1, 1);
        size_t m = 0;
        for (size_t k = 0; k < bn; k++) {
            unsigned cp = body[k] < 0x80 ? body[k] : tab[body[k] - 0x80];
            m += embi_utf8_encode(cp, d + m);
        }
        d[m] = '\0';
        text->ptr = d;
        text->len = (uint32_t)m;
        return true;
    }
    /* Other recognized legacy encodings: no decoder. */
    return false;
}
