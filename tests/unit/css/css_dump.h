#pragma once
/*
 * Canonical text dump of CSS syntax results, in the representation of the css-parsing-tests
 * README. The fixture runner compares our output with the expected JSON by dumping both sides
 * the same way. Header-only, test-only.
 */
#include "css/css.h"
#include "json_mini.h"

#include <string.h>

static inline void cd_lit(embi_sb *sb, const char *s)
{
    embi_sb_append(sb, (embi_str){s, (uint32_t)strlen(s)});
}

static inline void cd_str(embi_sb *sb, embi_str s)
{
    static const char hex[] = "0123456789abcdef";
    embi_sb_append_char(sb, '"');
    for (uint32_t k = 0; k < s.len; k++) {
        unsigned char c = (unsigned char)s.ptr[k];
        if (c == '"' || c == '\\') {
            embi_sb_append_char(sb, '\\');
            embi_sb_append_char(sb, (char)c);
        } else if (c < 0x20) {
            cd_lit(sb, "\\u00");
            embi_sb_append_char(sb, hex[c >> 4]);
            embi_sb_append_char(sb, hex[c & 15]);
        } else {
            embi_sb_append_char(sb, (char)c);
        }
    }
    embi_sb_append_char(sb, '"');
}

static inline void cd_cstr(embi_sb *sb, const char *s)
{
    cd_str(sb, (embi_str){s, (uint32_t)strlen(s)});
}

static inline void cd_num(embi_sb *sb, double v)
{
    if (v == 0) {
        v = 0; /* -0 and 0 print the same */
    }
    embi_sb_append_f64(sb, v);
}

static inline void cd_sep(embi_sb *sb, bool *first)
{
    if (!*first) {
        embi_sb_append_char(sb, ',');
    }
    *first = false;
}

static inline void cd_cv(embi_sb *sb, const embi_css_cv *c);

/* The parse errors the reference format shows after a string or url that EOF ended. The tokens
 * carry only the flag, so the marker is written here, next to the token. */
static inline void cd_eof_marker(embi_sb *sb, const embi_css_cv *c)
{
    if (c->tok.flag && c->tok.type == EMBI_CSS_STRING) {
        cd_lit(sb, ",[\"error\",\"eof-in-string\"]");
    } else if (c->tok.flag && c->tok.type == EMBI_CSS_URL) {
        cd_lit(sb, ",[\"error\",\"eof-in-url\"]");
    }
}

static inline void cd_cv_list(embi_sb *sb, embi_css_slice s)
{
    bool first = true;
    for (uint32_t k = 0; k < s.n; k++) {
        cd_sep(sb, &first);
        cd_cv(sb, &s.p[k]);
        cd_eof_marker(sb, &s.p[k]);
    }
}

static inline void cd_cv(embi_sb *sb, const embi_css_cv *c)
{
    const embi_css_token *t = &c->tok;
    char tmp[4];
    switch (t->type) {
    case EMBI_CSS_WS:
        cd_cstr(sb, " ");
        return;
    case EMBI_CSS_IDENT:
        cd_lit(sb, "[\"ident\",");
        cd_str(sb, t->value);
        break;
    case EMBI_CSS_FUNCTION:
        cd_lit(sb, "[\"function\",");
        cd_str(sb, t->value);
        if (c->nkids) {
            embi_sb_append_char(sb, ',');
            cd_cv_list(sb, embi_css_slice_make(c->kids, c->nkids));
        }
        break;
    case EMBI_CSS_AT_KEYWORD:
        cd_lit(sb, "[\"at-keyword\",");
        cd_str(sb, t->value);
        break;
    case EMBI_CSS_HASH:
        cd_lit(sb, "[\"hash\",");
        cd_str(sb, t->value);
        cd_lit(sb, t->flag ? ",\"id\"" : ",\"unrestricted\"");
        break;
    case EMBI_CSS_STRING:
        cd_lit(sb, "[\"string\",");
        cd_str(sb, t->value);
        break;
    case EMBI_CSS_BAD_STRING:
        cd_lit(sb, "[\"error\",\"bad-string\"]");
        return;
    case EMBI_CSS_URL:
        cd_lit(sb, "[\"url\",");
        cd_str(sb, t->value);
        break;
    case EMBI_CSS_BAD_URL:
        cd_lit(sb, "[\"error\",\"bad-url\"]");
        return;
    case EMBI_CSS_DELIM: {
        size_t k = embi_utf8_encode(t->cp, tmp);
        cd_str(sb, (embi_str){tmp, (uint32_t)k});
        return;
    }
    case EMBI_CSS_NUMBER:
    case EMBI_CSS_PERCENTAGE:
    case EMBI_CSS_DIMENSION:
        cd_lit(sb, t->type == EMBI_CSS_NUMBER        ? "[\"number\","
                   : t->type == EMBI_CSS_PERCENTAGE ? "[\"percentage\","
                                                     : "[\"dimension\",");
        cd_str(sb, t->repr);
        embi_sb_append_char(sb, ',');
        cd_num(sb, t->num);
        embi_sb_append_char(sb, ',');
        cd_cstr(sb, t->flag ? "integer" : "number");
        if (t->type == EMBI_CSS_DIMENSION) {
            embi_sb_append_char(sb, ',');
            cd_str(sb, t->value);
        }
        break;
    case EMBI_CSS_UNICODE_RANGE:
        cd_lit(sb, "[\"unicode-range\",");
        cd_num(sb, t->cp);
        embi_sb_append_char(sb, ',');
        cd_num(sb, t->cp_end);
        break;
    case EMBI_CSS_INCLUDE_MATCH:
        cd_cstr(sb, "~=");
        return;
    case EMBI_CSS_DASH_MATCH:
        cd_cstr(sb, "|=");
        return;
    case EMBI_CSS_PREFIX_MATCH:
        cd_cstr(sb, "^=");
        return;
    case EMBI_CSS_SUFFIX_MATCH:
        cd_cstr(sb, "$=");
        return;
    case EMBI_CSS_SUBSTRING_MATCH:
        cd_cstr(sb, "*=");
        return;
    case EMBI_CSS_COLUMN:
        cd_cstr(sb, "||");
        return;
    case EMBI_CSS_CDO:
        cd_cstr(sb, "<!--");
        return;
    case EMBI_CSS_CDC:
        cd_cstr(sb, "-->");
        return;
    case EMBI_CSS_COLON:
        cd_cstr(sb, ":");
        return;
    case EMBI_CSS_SEMICOLON:
        cd_cstr(sb, ";");
        return;
    case EMBI_CSS_COMMA:
        cd_cstr(sb, ",");
        return;
    case EMBI_CSS_LBRACKET:
        cd_lit(sb, "[\"[]\"");
        break;
    case EMBI_CSS_LPAREN:
        cd_lit(sb, "[\"()\"");
        break;
    case EMBI_CSS_LBRACE:
        cd_lit(sb, "[\"{}\"");
        break;
    case EMBI_CSS_RBRACKET:
        cd_lit(sb, "[\"error\",\"]\"]");
        return;
    case EMBI_CSS_RPAREN:
        cd_lit(sb, "[\"error\",\")\"]");
        return;
    case EMBI_CSS_RBRACE:
        cd_lit(sb, "[\"error\",\"}\"]");
        return;
    default:
        cd_cstr(sb, "?");
        return;
    }
    /* Array forms: the function or block contents follow. Function contents are already
     * written above; block contents are written here. */
    if (t->type == EMBI_CSS_LBRACKET || t->type == EMBI_CSS_LPAREN || t->type == EMBI_CSS_LBRACE) {
        if (c->nkids) {
            embi_sb_append_char(sb, ',');
            cd_cv_list(sb, embi_css_slice_make(c->kids, c->nkids));
        }
    }
    embi_sb_append_char(sb, ']');
}

static inline void cd_item(embi_sb *sb, const embi_css_item *it)
{
    {
        switch (it->kind) {
        case EMBI_CSS_ITEM_ERROR:
            cd_lit(sb, "[\"error\",");
            cd_cstr(sb, it->error);
            embi_sb_append_char(sb, ']');
            break;
        case EMBI_CSS_ITEM_QUALIFIED:
            cd_lit(sb, "[\"qualified rule\",[");
            cd_cv_list(sb, it->prelude);
            cd_lit(sb, "],[");
            cd_cv_list(sb, it->block);
            cd_lit(sb, "]]");
            break;
        case EMBI_CSS_ITEM_AT:
            cd_lit(sb, "[\"at-rule\",");
            cd_str(sb, it->name);
            cd_lit(sb, ",[");
            cd_cv_list(sb, it->prelude);
            cd_lit(sb, "],");
            if (it->has_block) {
                cd_lit(sb, "[");
                cd_cv_list(sb, it->block);
                cd_lit(sb, "]");
            } else {
                cd_lit(sb, "null");
            }
            embi_sb_append_char(sb, ']');
            break;
        case EMBI_CSS_ITEM_DECL:
            cd_lit(sb, "[\"declaration\",");
            cd_str(sb, it->name);
            cd_lit(sb, ",[");
            cd_cv_list(sb, it->prelude);
            cd_lit(sb, "],");
            cd_lit(sb, it->important ? "true" : "false");
            embi_sb_append_char(sb, ']');
            break;
        default:
            cd_cstr(sb, "?");
            break;
        }
    }
}

static inline void cd_items(embi_sb *sb, embi_css_items items)
{
    bool first = true;
    embi_sb_append_char(sb, '[');
    for (uint32_t k = 0; k < items.n; k++) {
        cd_sep(sb, &first);
        cd_item(sb, &items.p[k]);
    }
    embi_sb_append_char(sb, ']');
}

/* Dumps an expected value from the fixture JSON, in the same canonical form. */
static inline void cd_jm(embi_sb *sb, const jm_val *v)
{
    switch (v->type) {
    case JM_NULL:
        cd_lit(sb, "null");
        break;
    case JM_BOOL:
        cd_lit(sb, v->b ? "true" : "false");
        break;
    case JM_NUMBER:
        cd_num(sb, v->num);
        break;
    case JM_STRING:
        cd_str(sb, (embi_str){v->str, (uint32_t)v->len});
        break;
    case JM_ARRAY: {
        embi_sb_append_char(sb, '[');
        for (size_t k = 0; k < v->count; k++) {
            if (k) {
                embi_sb_append_char(sb, ',');
            }
            cd_jm(sb, &v->items[k]);
        }
        embi_sb_append_char(sb, ']');
        break;
    }
    default:
        cd_lit(sb, "{}");
        break;
    }
}
