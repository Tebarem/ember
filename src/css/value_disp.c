/*
 * The multi-keyword syntax of display (CSS Display 3 section 2): [ <display-outside> || <display-inside> ] |
 * <display-listitem>, mapped to the single keyword of the same meaning. The list-item forms that have no
 * single-keyword equivalent use internal keywords (-internal-inline-list-item, -internal-flow-root-list-item,
 * -internal-inline-flow-root-list-item); embi_css_display_kw_text gives their CSS form for serialization.
 */
#include "css/value_int.h"

enum { D_BLOCK = 1, D_INLINE, D_RUNIN };
enum { I_FLOW = 1, I_FLOWROOT, I_TABLE, I_FLEX, I_GRID, I_RUBY, I_MATH };

static bool ident_eq(embi_str s, const char *lit)
{
    uint32_t n = (uint32_t)strlen(lit);
    if (s.len != n) {
        return false;
    }
    for (uint32_t i = 0; i < n; i++) {
        char ch = s.ptr[i];
        if (ch >= 'A' && ch <= 'Z') {
            ch = (char)(ch + 32);
        }
        if (ch != lit[i]) {
            return false;
        }
    }
    return true;
}

bool embi_css_parse_display_multi(embi_css_slice s, uint16_t *kw)
{
    int outer = 0, inner = 0, nid = 0;
    bool li = false;
    for (uint32_t i = 0; i < s.n; i++) {
        const embi_css_cv *cv = &s.p[i];
        if (cv->tok.type == EMBI_CSS_WS) {
            continue;
        }
        if (cv->tok.type != EMBI_CSS_IDENT || ++nid > 3) {
            return false;
        }
        embi_str t = cv->tok.value;
        int o = ident_eq(t, "block") ? D_BLOCK : ident_eq(t, "inline") ? D_INLINE : ident_eq(t, "run-in") ? D_RUNIN : 0;
        int in = ident_eq(t, "flow")        ? I_FLOW
                 : ident_eq(t, "flow-root") ? I_FLOWROOT
                 : ident_eq(t, "table")     ? I_TABLE
                 : ident_eq(t, "flex")      ? I_FLEX
                 : ident_eq(t, "grid")      ? I_GRID
                 : ident_eq(t, "ruby")      ? I_RUBY
                 : ident_eq(t, "math")      ? I_MATH
                                            : 0;
        if (o != 0) {
            if (outer != 0) {
                return false;
            }
            outer = o;
        } else if (in != 0) {
            if (inner != 0) {
                return false;
            }
            inner = in;
        } else if (ident_eq(t, "list-item")) {
            if (li) {
                return false;
            }
            li = true;
        } else {
            return false;
        }
    }
    if (nid == 0 || outer == D_RUNIN) {
        return false; /* run-in is not supported: the declaration is dropped */
    }
    if (li) {
        if (inner != 0 && inner != I_FLOW && inner != I_FLOWROOT) {
            return false;
        }
        bool in_l = outer == D_INLINE;
        if (inner == I_FLOWROOT) {
            *kw = in_l ? EMBI_KW__internal_inline_flow_root_list_item : EMBI_KW__internal_flow_root_list_item;
        } else {
            *kw = in_l ? EMBI_KW__internal_inline_list_item : EMBI_KW_list_item;
        }
        return true;
    }
    if (outer == 0) {
        /* A lone <display-inside>: ruby and math are inline, the others block. */
        outer = inner == I_RUBY || inner == I_MATH ? D_INLINE : D_BLOCK;
    }
    bool in_l = outer == D_INLINE;
    switch (inner) {
    case 0:
    case I_FLOW:
    case I_MATH: /* MathML layout is not supported: math is a flow box of its outer type */
        *kw = in_l ? EMBI_KW_inline : EMBI_KW_block;
        return true;
    case I_FLOWROOT:
        *kw = in_l ? EMBI_KW_inline_block : EMBI_KW_flow_root;
        return true;
    case I_TABLE:
        *kw = in_l ? EMBI_KW_inline_table : EMBI_KW_table;
        return true;
    case I_FLEX:
        *kw = in_l ? EMBI_KW_inline_flex : EMBI_KW_flex;
        return true;
    case I_GRID:
        *kw = in_l ? EMBI_KW_inline_grid : EMBI_KW_grid;
        return true;
    case I_RUBY:
        *kw = in_l ? EMBI_KW_ruby : EMBI_KW_block;
        return true;
    default:
        return false;
    }
}

const char *embi_css_display_kw_text(uint16_t kw)
{
    switch (kw) {
    case EMBI_KW__internal_inline_list_item:
        return "inline list-item";
    case EMBI_KW__internal_flow_root_list_item:
        return "flow-root list-item";
    case EMBI_KW__internal_inline_flow_root_list_item:
        return "inline flow-root list-item";
    default:
        return nullptr;
    }
}
