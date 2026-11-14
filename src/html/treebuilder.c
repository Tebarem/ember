/*
 * Tree builder, WHATWG 13.2.6. Every insertion mode, the stack of open elements, the list of
 * active formatting elements with Noah's Ark, the adoption agency algorithm, foster parenting,
 * the template insertion modes, foreign content and reset-the-insertion-mode.
 *
 * Tokens arrive one at a time from the tokenizer sink. Character tokens are split into segments
 * that are whitespace, U+0000, or other text, so each segment is handled as one unit (every mode
 * treats the characters of one segment the same way). Nodes are linked with the dom module's raw
 * operations: the tree builder only ever links fresh nodes or moves nodes inside the same tree,
 * so the validity checks of embi_pre_insert add nothing and would cost O(depth) per insertion.
 */
#include "html/treebuilder.h"
#include "html/names.h"
#include "dom/internal.h"

#include <string.h>

#define REPLACEMENT_CHAR "\xEF\xBF\xBD"
/* Names below this have an open-element count (see embi_tb.hcount). */
#define TB_NAME_MAX 1024u
/* Atoms used to index hcount directly must be static atoms below TB_NAME_MAX. Dynamic names
 * (interned from documents) are never freed from the global atom table: a hostile document with
 * millions of distinct tag or attribute names grows memory until the table is capped or refcounted
 * (known limitation, tracked for a later task). */
static_assert(EMBI_ATOM_template < TB_NAME_MAX, "hcount indexes template by atom id");

typedef enum tb_mode {
    M_INITIAL,
    M_BEFORE_HTML,
    M_BEFORE_HEAD,
    M_IN_HEAD,
    M_IN_HEAD_NOSCRIPT,
    M_AFTER_HEAD,
    M_IN_BODY,
    M_TEXT,
    M_IN_TABLE,
    M_IN_TABLE_TEXT,
    M_IN_CAPTION,
    M_IN_COLUMN_GROUP,
    M_IN_TABLE_BODY,
    M_IN_ROW,
    M_IN_CELL,
    M_IN_SELECT,
    M_IN_SELECT_IN_TABLE,
    M_IN_TEMPLATE,
    M_AFTER_BODY,
    M_IN_FRAMESET,
    M_AFTER_FRAMESET,
    M_AFTER_AFTER_BODY,
    M_AFTER_AFTER_FRAMESET,
} tb_mode;

typedef enum tk_kind {
    TK_CHAR,
    TK_START,
    TK_END,
    TK_COMMENT,
    TK_DOCTYPE,
    TK_EOF,
} tk_kind;

/* Character segment classes. A segment never mixes these. */
enum { SEG_WS, SEG_NUL, SEG_OTHER };

/* A token as the mode functions see it. raw is nullptr for synthetic start tags. */
typedef struct tk {
    tk_kind kind;
    const embi_html_token *raw;
    embi_atom name; /* START: always interned. END: 0 when the name was never interned. */
    embi_str text;  /* CHAR: segment. COMMENT: data. */
    int seg;        /* CHAR only */
} tk;

typedef struct loc {
    embi_node *parent;
    embi_node *before; /* nullptr appends */
} loc;

enum { CLEAR_TABLE, CLEAR_TBODY, CLEAR_ROW };
enum { SCOPE_DEFAULT, SCOPE_LIST_ITEM, SCOPE_BUTTON, SCOPE_TABLE, SCOPE_SELECT };

struct embi_tb {
    embi_document *doc;      /* destination document */
    embi_element *context;   /* fragment parsing: the context element */
    embi_element *root;      /* fragment parsing: the root html element, not in any tree */
    bool fragment;
    bool scripting;
    embi_html_script_fn on_script;
    void *script_ctx;
    embi_html_tokenizer *tok;
    tb_mode mode;
    tb_mode orig_mode;
    EMBI_VEC(int) tmodes;          /* template insertion modes */
    EMBI_VEC(embi_element *) stk;  /* stack of open elements, bottom first */
    EMBI_VEC(embi_element *) afe;  /* active formatting elements; nullptr is a marker */
    EMBI_VEC(uint32_t) afe_h;      /* parallel to afe: hash of name, namespace and attributes */
    embi_element *head;            /* head element pointer */
    embi_element *form;            /* form element pointer */
    bool frameset_ok;
    bool foster;                   /* foster parenting enabled */
    bool skip_lf;                  /* ignore the next token when it is a line feed */
    bool pause;                    /* set by a script end tag; returned by embi_tb_token */
    bool stop;                     /* a script hook said the document is gone (EMBI_HTML_SCRIPT_STOP) */
    embi_sb pending;               /* pending table character tokens, UTF-8 */
    bool pending_nonws;
    bool has_select;               /* a select was inserted: selectedcontent needs filling */
    bool cdata_flag;               /* the last value passed to set_cdata_allowed */
    /* Open HTML elements on the stack per local name atom (names below TB_NAME_MAX). Scope tests
     * use it to answer "no such element" without walking the stack, which keeps deep nesting
     * linear. Maintained by stk_count on every stack change. */
    uint32_t hcount[TB_NAME_MAX];
};

/* ------------------------------------------------------------------ small helpers */


static bool lit_eq(embi_str s, const char *lit)
{
    size_t n = strlen(lit);
    if (s.len != n) {
        return false;
    }
    return n == 0 || memcmp(s.ptr, lit, n) == 0;
}

static unsigned char ascii_lower(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + 32) : c;
}

static bool starts_ci(embi_str s, const char *p)
{
    size_t n = strlen(p);
    if (s.len < n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (ascii_lower((unsigned char)s.ptr[i]) != ascii_lower((unsigned char)p[i])) {
            return false;
        }
    }
    return true;
}

static bool eq_ci(embi_str s, const char *lit)
{
    return s.len == strlen(lit) && starts_ci(s, lit);
}

static bool is_ws_byte(unsigned char c)
{
    return c == 0x09 || c == 0x0A || c == 0x0C || c == 0x0D || c == 0x20;
}

static bool is_ascii(embi_str s)
{
    for (uint32_t i = 0; i < s.len; i++) {
        if ((unsigned char)s.ptr[i] >= 0x80) {
            return false;
        }
    }
    return true;
}

/* A dstr for UTF-8 text. ASCII text is viewed in place (the Latin-1 form of ASCII is the same
 * bytes). Other text is converted into *own, which the caller frees after the value is copied. */
static embi_dstr text_dstr(embi_slab *s, embi_str text, embi_dstr *own)
{
    *own = embi_dstr_empty();
    if (text.len == 0) {
        return embi_dstr_empty();
    }
    if (is_ascii(text)) {
        return (embi_dstr){text.ptr, text.len, false};
    }
    *own = embi_dstr_from_utf8(s, text);
    return *own;
}

static embi_dstr opt_dstr(embi_slab *s, bool has, embi_str v, embi_dstr *own)
{
    *own = embi_dstr_empty();
    return has ? text_dstr(s, v, own) : embi_dstr_empty();
}

/* Name lookups go through the per-thread cache in html/names.h. */
static embi_atom cached_atom(embi_str s, bool create)
{
    return embi_html_name_atom(s.ptr, s.len, create);
}

static embi_atom intern(embi_str s)
{
    return cached_atom(s, true);
}

static inline bool is_el(const embi_element *el, uint8_t ns, embi_atom name)
{
    return el != nullptr && el->base.extra == ns && el->base.name == name;
}

static inline bool is_html(const embi_element *el, embi_atom name)
{
    return is_el(el, EMBI_NS_HTML, name);
}

static inline bool is_html_ns(const embi_element *el)
{
    return el != nullptr && el->base.extra == EMBI_NS_HTML;
}

static bool is_mtip(const embi_element *el)
{
    if (!el || el->base.extra != EMBI_NS_MATHML) {
        return false;
    }
    embi_atom n = el->base.name;
    return n == EMBI_ATOM_mi || n == EMBI_ATOM_mo || n == EMBI_ATOM_mn || n == EMBI_ATOM_ms ||
           n == EMBI_ATOM_mtext;
}

/* HTML integration point (an SVG foreignObject/desc/title, or a MathML annotation-xml whose
 * encoding is text/html or application/xhtml+xml). */
static bool is_hip(const embi_element *el)
{
    if (!el) {
        return false;
    }
    if (el->base.extra == EMBI_NS_SVG) {
        embi_atom n = el->base.name;
        return n == EMBI_ATOM_foreignObject || n == EMBI_ATOM_desc || n == EMBI_ATOM_title;
    }
    if (el->base.extra == EMBI_NS_MATHML && el->base.name == EMBI_ATOM_annotation_xml) {
        uint32_t n = embi_el_attr_count(el);
        for (uint32_t i = 0; i < n; i++) {
            const embi_attr *a = embi_el_attr_at(el, i);
            if (a->ns != EMBI_NS_NULL || !embi_str_eq(embi_atom_str(a->name), EMBI_STR("encoding"))) {
                continue;
            }
            return embi_dstr_ieq_utf8(a->value, EMBI_STR("text/html")) ||
                   embi_dstr_ieq_utf8(a->value, EMBI_STR("application/xhtml+xml"));
        }
    }
    return false;
}

/* ------------------------------------------------------------------ stack of open elements */

static embi_element *cur(const embi_tb *tb)
{
    return tb->stk.len ? tb->stk.data[tb->stk.len - 1] : nullptr;
}

/* The adjusted current node: the context element when parsing a fragment with a single node. */
static embi_element *adjusted_current(const embi_tb *tb)
{
    if (tb->fragment && tb->stk.len == 1) {
        return tb->context;
    }
    return cur(tb);
}

/* Adds delta to the open-element count of el (HTML elements with a name below TB_NAME_MAX). */
static void stk_count(embi_tb *tb, const embi_element *el, int delta)
{
    if (el && is_html_ns(el) && el->base.name < TB_NAME_MAX) {
        tb->hcount[el->base.name] += (uint32_t)delta;
    }
}

static void push(embi_tb *tb, embi_element *el)
{
    embi_vec_push(&tb->stk, el);
    stk_count(tb, el, 1);
}

static void pop(embi_tb *tb)
{
    if (tb->stk.len) {
        stk_count(tb, tb->stk.data[tb->stk.len - 1], -1);
        (void)embi_vec_pop(&tb->stk);
    }
}

static int stk_find(const embi_tb *tb, const embi_element *el)
{
    for (int i = (int)tb->stk.len - 1; i >= 0; i--) {
        if (tb->stk.data[i] == el) {
            return i;
        }
    }
    return -1;
}

static bool in_stack(const embi_tb *tb, const embi_element *el)
{
    return stk_find(tb, el) >= 0;
}

static void stk_remove_at(embi_tb *tb, int idx)
{
    stk_count(tb, tb->stk.data[idx], -1);
    for (uint32_t i = (uint32_t)idx + 1; i < tb->stk.len; i++) {
        tb->stk.data[i - 1] = tb->stk.data[i];
    }
    tb->stk.len--;
}

static void stk_insert_at(embi_tb *tb, int idx, embi_element *el)
{
    stk_count(tb, el, 1);
    embi_vec_push(&tb->stk, el);
    for (uint32_t i = tb->stk.len - 1; i > (uint32_t)idx; i--) {
        tb->stk.data[i] = tb->stk.data[i - 1];
    }
    tb->stk.data[idx] = el;
}

/* Pops up to and including el. */
static void pop_until_el(embi_tb *tb, const embi_element *el)
{
    while (tb->stk.len) {
        embi_element *e = cur(tb);
        pop(tb);
        if (e == el) {
            break;
        }
    }
}

/* Pops up to and including the HTML element with this name. */
static void pop_until_html(embi_tb *tb, embi_atom name)
{
    while (tb->stk.len) {
        embi_element *e = cur(tb);
        pop(tb);
        if (is_html(e, name)) {
            break;
        }
    }
}

static bool is_heading(const embi_element *el)
{
    if (!is_html_ns(el)) {
        return false;
    }
    embi_atom n = el->base.name;
    return n == EMBI_ATOM_h1 || n == EMBI_ATOM_h2 || n == EMBI_ATOM_h3 || n == EMBI_ATOM_h4 ||
           n == EMBI_ATOM_h5 || n == EMBI_ATOM_h6;
}

static void pop_until_heading(embi_tb *tb)
{
    while (tb->stk.len) {
        embi_element *e = cur(tb);
        pop(tb);
        if (is_heading(e)) {
            break;
        }
    }
}

static bool has_template(const embi_tb *tb)
{
    return tb->hcount[EMBI_ATOM_template] != 0;
}

/* The form element pointer rules ask whether a template is open. A template context of a
 * fragment parse is one too, although the fragment's stack holds only the root html element. */
static bool form_in_template(const embi_tb *tb)
{
    return has_template(tb) || (tb->fragment && is_html(tb->context, EMBI_ATOM_template));
}

/* Special category (WHATWG 13.2.4.2). */
static bool is_special(const embi_element *el)
{
    if (!el) {
        return false;
    }
    embi_atom n = el->base.name;
    switch (el->base.extra) {
    case EMBI_NS_HTML:
        switch (n) {
        case EMBI_ATOM_address:
        case EMBI_ATOM_applet:
        case EMBI_ATOM_area:
        case EMBI_ATOM_article:
        case EMBI_ATOM_aside:
        case EMBI_ATOM_base:
        case EMBI_ATOM_basefont:
        case EMBI_ATOM_bgsound:
        case EMBI_ATOM_blockquote:
        case EMBI_ATOM_body:
        case EMBI_ATOM_br:
        case EMBI_ATOM_button:
        case EMBI_ATOM_caption:
        case EMBI_ATOM_center:
        case EMBI_ATOM_col:
        case EMBI_ATOM_colgroup:
        case EMBI_ATOM_dd:
        case EMBI_ATOM_details:
        case EMBI_ATOM_dir:
        case EMBI_ATOM_div:
        case EMBI_ATOM_dl:
        case EMBI_ATOM_dt:
        case EMBI_ATOM_embed:
        case EMBI_ATOM_fieldset:
        case EMBI_ATOM_figcaption:
        case EMBI_ATOM_figure:
        case EMBI_ATOM_footer:
        case EMBI_ATOM_form:
        case EMBI_ATOM_frame:
        case EMBI_ATOM_frameset:
        case EMBI_ATOM_h1:
        case EMBI_ATOM_h2:
        case EMBI_ATOM_h3:
        case EMBI_ATOM_h4:
        case EMBI_ATOM_h5:
        case EMBI_ATOM_h6:
        case EMBI_ATOM_head:
        case EMBI_ATOM_header:
        case EMBI_ATOM_hgroup:
        case EMBI_ATOM_hr:
        case EMBI_ATOM_html:
        case EMBI_ATOM_iframe:
        case EMBI_ATOM_img:
        case EMBI_ATOM_input:
        case EMBI_ATOM_keygen:
        case EMBI_ATOM_li:
        case EMBI_ATOM_link:
        case EMBI_ATOM_listing:
        case EMBI_ATOM_main:
        case EMBI_ATOM_marquee:
        case EMBI_ATOM_menu:
        case EMBI_ATOM_meta:
        case EMBI_ATOM_nav:
        case EMBI_ATOM_noembed:
        case EMBI_ATOM_noframes:
        case EMBI_ATOM_noscript:
        case EMBI_ATOM_object:
        case EMBI_ATOM_ol:
        case EMBI_ATOM_p:
        case EMBI_ATOM_param:
        case EMBI_ATOM_plaintext:
        case EMBI_ATOM_pre:
        case EMBI_ATOM_script:
        case EMBI_ATOM_search:
        case EMBI_ATOM_section:
        case EMBI_ATOM_select:
        case EMBI_ATOM_source:
        case EMBI_ATOM_style:
        case EMBI_ATOM_summary:
        case EMBI_ATOM_table:
        case EMBI_ATOM_tbody:
        case EMBI_ATOM_td:
        case EMBI_ATOM_template:
        case EMBI_ATOM_textarea:
        case EMBI_ATOM_tfoot:
        case EMBI_ATOM_th:
        case EMBI_ATOM_thead:
        case EMBI_ATOM_title:
        case EMBI_ATOM_tr:
        case EMBI_ATOM_track:
        case EMBI_ATOM_ul:
        case EMBI_ATOM_wbr:
        case EMBI_ATOM_xmp:
            return true;
        default:
            return false;
        }
    case EMBI_NS_MATHML:
        return n == EMBI_ATOM_mi || n == EMBI_ATOM_mo || n == EMBI_ATOM_mn || n == EMBI_ATOM_ms ||
               n == EMBI_ATOM_mtext || n == EMBI_ATOM_annotation_xml;
    case EMBI_NS_SVG:
        return n == EMBI_ATOM_foreignObject || n == EMBI_ATOM_desc || n == EMBI_ATOM_title;
    default:
        return false;
    }
}

/* Scope boundaries (WHATWG 13.2.4.2). */
static bool is_scope_boundary(const embi_element *el, int scope)
{
    uint8_t ns = (uint8_t)el->base.extra;
    embi_atom n = el->base.name;
    if (scope == SCOPE_SELECT) {
        return !(ns == EMBI_NS_HTML && (n == EMBI_ATOM_optgroup || n == EMBI_ATOM_option));
    }
    if (scope == SCOPE_TABLE) {
        return ns == EMBI_NS_HTML &&
               (n == EMBI_ATOM_html || n == EMBI_ATOM_table || n == EMBI_ATOM_template);
    }
    if (ns == EMBI_NS_HTML) {
        switch (n) {
        case EMBI_ATOM_applet:
        case EMBI_ATOM_caption:
        case EMBI_ATOM_html:
        case EMBI_ATOM_table:
        case EMBI_ATOM_td:
        case EMBI_ATOM_th:
        case EMBI_ATOM_marquee:
        case EMBI_ATOM_object:
        case EMBI_ATOM_template:
            return true;
        case EMBI_ATOM_ol:
        case EMBI_ATOM_ul:
            return scope == SCOPE_LIST_ITEM;
        case EMBI_ATOM_button:
            return scope == SCOPE_BUTTON;
        default:
            return false;
        }
    }
    if (ns == EMBI_NS_MATHML) {
        return n == EMBI_ATOM_mi || n == EMBI_ATOM_mo || n == EMBI_ATOM_mn || n == EMBI_ATOM_ms ||
               n == EMBI_ATOM_mtext || n == EMBI_ATOM_annotation_xml;
    }
    if (ns == EMBI_NS_SVG) {
        return n == EMBI_ATOM_foreignObject || n == EMBI_ATOM_desc || n == EMBI_ATOM_title;
    }
    return false;
}

static bool el_in_scope(const embi_tb *tb, const embi_element *target, int scope)
{
    for (int i = (int)tb->stk.len - 1; i >= 0; i--) {
        const embi_element *e = tb->stk.data[i];
        if (e == target) {
            return true;
        }
        if (is_scope_boundary(e, scope)) {
            return false;
        }
    }
    return false;
}

/* Has an HTML element with this name in scope. */
static bool name_in_scope(const embi_tb *tb, embi_atom name, int scope)
{
    if (name < TB_NAME_MAX && tb->hcount[name] == 0) {
        return false;
    }
    for (int i = (int)tb->stk.len - 1; i >= 0; i--) {
        const embi_element *e = tb->stk.data[i];
        if (is_html(e, name)) {
            return true;
        }
        if (is_scope_boundary(e, scope)) {
            return false;
        }
    }
    return false;
}

/* Has an HTML element with any of these names in scope. */
static bool names_in_scope(const embi_tb *tb, const embi_atom *names, uint32_t count, int scope)
{
    for (int i = (int)tb->stk.len - 1; i >= 0; i--) {
        const embi_element *e = tb->stk.data[i];
        for (uint32_t k = 0; k < count; k++) {
            if (is_html(e, names[k])) {
                return true;
            }
        }
        if (is_scope_boundary(e, scope)) {
            return false;
        }
    }
    return false;
}

static bool is_implied_member(embi_atom n)
{
    switch (n) {
    case EMBI_ATOM_dd:
    case EMBI_ATOM_dt:
    case EMBI_ATOM_li:
    case EMBI_ATOM_optgroup:
    case EMBI_ATOM_option:
    case EMBI_ATOM_p:
    case EMBI_ATOM_rb:
    case EMBI_ATOM_rp:
    case EMBI_ATOM_rt:
    case EMBI_ATOM_rtc:
        return true;
    default:
        return false;
    }
}

static bool is_thorough_member(embi_atom n)
{
    switch (n) {
    case EMBI_ATOM_caption:
    case EMBI_ATOM_colgroup:
    case EMBI_ATOM_tbody:
    case EMBI_ATOM_td:
    case EMBI_ATOM_tfoot:
    case EMBI_ATOM_th:
    case EMBI_ATOM_thead:
    case EMBI_ATOM_tr:
        return true;
    default:
        return is_implied_member(n);
    }
}

/* Generate implied end tags, except for HTML elements named except (0 for none). */
static void gen_implied(embi_tb *tb, embi_atom except, bool thorough)
{
    while (tb->stk.len) {
        embi_element *e = cur(tb);
        if (!is_html_ns(e)) {
            break;
        }
        embi_atom n = e->base.name;
        if (n == except) {
            break;
        }
        if (!(thorough ? is_thorough_member(n) : is_implied_member(n))) {
            break;
        }
        pop(tb);
    }
}

static void close_p(embi_tb *tb)
{
    gen_implied(tb, EMBI_ATOM_p, false);
    pop_until_html(tb, EMBI_ATOM_p);
}

static void clear_to(embi_tb *tb, int ctx)
{
    while (tb->stk.len) {
        embi_element *e = cur(tb);
        if (is_html(e, EMBI_ATOM_html) || is_html(e, EMBI_ATOM_template)) {
            break;
        }
        if (ctx == CLEAR_TABLE && is_html(e, EMBI_ATOM_table)) {
            break;
        }
        if (ctx == CLEAR_TBODY && (is_html(e, EMBI_ATOM_tbody) || is_html(e, EMBI_ATOM_tfoot) ||
                                   is_html(e, EMBI_ATOM_thead))) {
            break;
        }
        if (ctx == CLEAR_ROW && is_html(e, EMBI_ATOM_tr)) {
            break;
        }
        pop(tb);
    }
}

/* ------------------------------------------------------------------ list of active formatting elements */

static int afe_find(const embi_tb *tb, const embi_element *el)
{
    for (int i = (int)tb->afe.len - 1; i >= 0; i--) {
        if (tb->afe.data[i] == el) {
            return i;
        }
    }
    return -1;
}

/* Removes the entry at idx. bm, when not nullptr, is a bookmark index that moves with entries. */
static void afe_remove_at(embi_tb *tb, int idx, int *bm)
{
    for (uint32_t i = (uint32_t)idx + 1; i < tb->afe.len; i++) {
        tb->afe.data[i - 1] = tb->afe.data[i];
        tb->afe_h.data[i - 1] = tb->afe_h.data[i];
    }
    tb->afe.len--;
    tb->afe_h.len--;
    if (bm && idx < *bm) {
        (*bm)--;
    }
}

static void afe_remove_el(embi_tb *tb, const embi_element *el, int *bm)
{
    int i = afe_find(tb, el);
    if (i >= 0) {
        afe_remove_at(tb, i, bm);
    }
}

static uint32_t afe_hash(const embi_element *el);

static void afe_insert_at(embi_tb *tb, int idx, embi_element *el)
{
    embi_vec_push(&tb->afe, el);
    embi_vec_push(&tb->afe_h, 0u);
    for (uint32_t i = tb->afe.len - 1; i > (uint32_t)idx; i--) {
        tb->afe.data[i] = tb->afe.data[i - 1];
        tb->afe_h.data[i] = tb->afe_h.data[i - 1];
    }
    tb->afe.data[idx] = el;
    tb->afe_h.data[idx] = afe_hash(el);
}

static void afe_push_marker(embi_tb *tb)
{
    embi_vec_push(&tb->afe, nullptr);
    embi_vec_push(&tb->afe_h, 0u);
}

static void afe_clear(embi_tb *tb)
{
    while (tb->afe.len) {
        embi_element *e = tb->afe.data[tb->afe.len - 1];
        tb->afe.len--;
        tb->afe_h.len--;
        if (!e) {
            break;
        }
    }
}

/* The last formatting element named name after the last marker, or nullptr. */
static embi_element *afe_last_named(const embi_tb *tb, embi_atom name)
{
    for (int i = (int)tb->afe.len - 1; i >= 0; i--) {
        embi_element *e = tb->afe.data[i];
        if (!e) {
            break;
        }
        if (is_html(e, name)) {
            return e;
        }
    }
    return nullptr;
}

/* Two elements have the same attributes: same count, and every attribute of a has an equal
 * attribute in b (same namespace, prefix, local name and value). */
static bool attrs_equal(const embi_element *a, const embi_element *b)
{
    uint32_t na = embi_el_attr_count(a);
    if (na != embi_el_attr_count(b)) {
        return false;
    }
    for (uint32_t i = 0; i < na; i++) {
        const embi_attr *x = embi_el_attr_at(a, i);
        const embi_attr *y = embi_el_find_attr(b, x->ns, x->ns_uri, x->name);
        if (!y || y->prefix != x->prefix || !embi_dstr_eq(x->value, y->value)) {
            return false;
        }
    }
    return true;
}

/* A hash of an element's name, namespace and attribute set (independent of attribute order), so
 * that Noah's Ark compares attributes only for entries that can be equal. */
static uint32_t afe_hash(const embi_element *el)
{
    uint64_t h = ((uint64_t)el->base.name * 0x9E3779B97F4A7C15ull) ^ el->base.extra;
    uint32_t n = embi_el_attr_count(el);
    uint64_t sum = 0;
    for (uint32_t i = 0; i < n; i++) {
        const embi_attr *a = embi_el_attr_at(el, i);
        uint64_t x = embi_dstr_hash(a->value) ^ ((uint64_t)a->name * 0xC2B2AE3D27D4EB4Full) ^
                     ((uint64_t)a->prefix << 20) ^ ((uint64_t)a->ns << 40);
        x *= 0x9E3779B97F4A7C15ull;
        sum += x ^ (x >> 29);
    }
    h ^= sum + (uint64_t)n;
    h *= 0xD6E8FEB86659FD93ull;
    return (uint32_t)(h >> 32);
}

/* Pushes a formatting element, applying Noah's Ark: at most three identical entries after the
 * last marker. */
static void afe_push(embi_tb *tb, embi_element *el)
{
    int count = 0;
    uint32_t h = afe_hash(el);
    for (int i = (int)tb->afe.len - 1; i >= 0; i--) {
        embi_element *e = tb->afe.data[i];
        if (!e) {
            break;
        }
        if (tb->afe_h.data[i] == h && e->base.name == el->base.name &&
            e->base.extra == el->base.extra && attrs_equal(e, el)) {
            count++;
            if (count == 3) {
                afe_remove_at(tb, i, nullptr);
                break;
            }
        }
    }
    embi_vec_push(&tb->afe, el);
    embi_vec_push(&tb->afe_h, h);
}

static embi_element *clone_formatting(embi_document *doc, const embi_element *src)
{
    embi_element *n = embi_new_element(doc, EMBI_NS_HTML, 0, 0, src->base.name);
    uint32_t count = embi_el_attr_count(src);
    for (uint32_t i = 0; i < count; i++) {
        const embi_attr *a = embi_el_attr_at(src, i);
        embi_el_set_attr_ns_atom(n, a->ns, a->ns_uri, a->prefix, a->name, a->value);
    }
    return n;
}

/* ------------------------------------------------------------------ nodes and insertion */

static void set_token_attr_ns(embi_element *el, uint8_t ns, embi_str name, embi_dstr v);

static embi_element *make_el(embi_document *doc, uint8_t ns, embi_atom name,
                             const embi_html_token *raw)
{
    if (raw && ns == EMBI_NS_HTML && raw->attr_count <= 32) {
        /* The common element: its attribute block and values come from one allocation. */
        embi_attr_init init[32];
        for (uint32_t i = 0; i < raw->attr_count; i++) {
            const embi_html_attr *a = &raw->attrs[i];
            init[i].name = a->atom ? a->atom : intern(a->name);
            init[i].value = a->value;
        }
        embi_element *el = embi_new_element_attrs(doc, ns, name, raw->attr_count, init);
        if (name == EMBI_ATOM_template) {
            (void)embi_template_contents(el);
        }
        return el;
    }
    embi_element *el = embi_new_element(doc, ns, 0, 0, name);
    if (raw) {
        /* One block of the exact size: the tokenizer already dropped duplicates. */
        if (raw->attr_count > 0) {
            embi_el_attrs_reserve(el, raw->attr_count);
        }
        for (uint32_t i = 0; i < raw->attr_count; i++) {
            const embi_html_attr *a = &raw->attrs[i];
            embi_dstr own;
            embi_dstr v = text_dstr(doc->slab, a->value, &own);
            if (ns == EMBI_NS_HTML) {
                embi_el_set_attr_atom(el, a->atom ? a->atom : intern(a->name), v);
            } else {
                set_token_attr_ns(el, ns, a->name, v);
            }
            embi_dstr_free(own);
        }
    }
    if (ns == EMBI_NS_HTML && name == EMBI_ATOM_template) {
        (void)embi_template_contents(el);
    }
    return el;
}

static loc doc_loc(const embi_tb *tb)
{
    return (loc){&tb->doc->base, nullptr};
}

/* The template contents fragment of a template element. */
static embi_node *contents_of(embi_element *el)
{
    return embi_template_contents(el);
}

/* The appropriate place for inserting a node (WHATWG 13.2.6.1). override, when not nullptr, is
 * the target instead of the current node. */
static loc appropriate(embi_tb *tb, embi_element *override)
{
    embi_element *target = override ? override : cur(tb);
    if (!target) {
        return doc_loc(tb);
    }
    if (tb->foster && is_html_ns(target)) {
        embi_atom n = target->base.name;
        if (n == EMBI_ATOM_table || n == EMBI_ATOM_tbody || n == EMBI_ATOM_tfoot ||
            n == EMBI_ATOM_thead || n == EMBI_ATOM_tr) {
            int last_template = -1;
            int last_table = -1;
            for (int i = (int)tb->stk.len - 1; i >= 0; i--) {
                embi_element *e = tb->stk.data[i];
                if (last_template < 0 && is_html(e, EMBI_ATOM_template)) {
                    last_template = i;
                }
                if (last_table < 0 && is_html(e, EMBI_ATOM_table)) {
                    last_table = i;
                }
                if (last_template >= 0 && last_table >= 0) {
                    break;
                }
            }
            if (last_template >= 0 && (last_table < 0 || last_template > last_table)) {
                return (loc){contents_of(tb->stk.data[last_template]), nullptr};
            }
            if (last_table < 0) {
                return (loc){&tb->stk.data[0]->base, nullptr};
            }
            embi_element *table = tb->stk.data[last_table];
            if (table->base.parent) {
                return (loc){table->base.parent, &table->base};
            }
            return (loc){&tb->stk.data[last_table - 1]->base, nullptr};
        }
    }
    if (is_html(target, EMBI_ATOM_template)) {
        return (loc){contents_of(target), nullptr};
    }
    return (loc){&target->base, nullptr};
}

/* Links an unlinked or freshly created node at the location. A node that is already in a tree is
 * moved (detached first), and when it is the reference node, the insertion uses its successor. */
static void link_at(loc L, embi_node *node)
{
    embi_node *p = L.parent;
    embi_node *before = L.before;
    if (before == node) {
        before = node->next;
    }
    if (node->parent) {
        embi_detach(node);
    }
    if (before) {
        embi_tree_link_chain(p, node, node, before);
    } else if (node->first_child == nullptr) {
        /* The common case, a fresh leaf appended: the links written inline. */
        embi_node *last = p->last_child;
        node->parent = p;
        node->prev = last;
        node->next = nullptr;
        if (last) {
            last->next = node;
        } else {
            p->first_child = node;
        }
        p->last_child = node;
        node->flags |= (uint8_t)(p->flags & EMBI_NF_CONNECTED);
    } else {
        embi_tree_append_raw(p, node);
    }
    embi_document *d = p->doc;
    if (p->flags & EMBI_NF_CONNECTED) {
        embi_document_idmap_invalidate(d);
    }
    if (d->observers.len != 0) {
        embi_notify_inserted(node);
        embi_notify_children(p, node, node, nullptr, nullptr);
    }
}

static embi_element *insert_el(embi_tb *tb, uint8_t ns, embi_atom name, const embi_html_token *raw)
{
    loc L = appropriate(tb, nullptr);
    embi_element *el = make_el(L.parent->doc, ns, name, raw);
    link_at(L, &el->base);
    push(tb, el);
    return el;
}

/* A foreign element. The name gets its SVG case adjustment (applied in svg_tag_adjust). */
static embi_str svg_tag_adjust(embi_str name);

static embi_element *insert_foreign(embi_tb *tb, uint8_t ns, const tk *t)
{
    embi_str n = t->raw->name;
    if (ns == EMBI_NS_SVG) {
        n = svg_tag_adjust(n);
    }
    loc L = appropriate(tb, nullptr);
    embi_element *el = make_el(L.parent->doc, ns, intern(n), t->raw);
    link_at(L, &el->base);
    push(tb, el);
    return el;
}

/* Appends UTF-8 text to a text node in place. The node's block is grown geometrically, so a long
 * text built from many small pieces costs linear time and the DOM is always current. */
static void text_append(embi_chardata *cd, embi_str s)
{
    bool swide;
    uint32_t add = embi_utf8_units(s, &swide);
    bool wide = cd->data.wide || swide;
    uint32_t old_len = cd->data.len;
    uint32_t nlen = old_len + add;
    size_t unit = wide ? 2u : 1u;
    bool in_place = cd->data.ptr != nullptr && cd->data.wide == wide &&
                    !(cd->base.flags & EMBI_NF_INLINE_DATA) &&
                    embi_slab_capacity(cd->data.ptr) >= (size_t)nlen * unit;
    uint8_t *dst;
    if (in_place) {
        dst = (uint8_t *)cd->data.ptr;
    } else {
        size_t cap = (size_t)nlen * unit;
        if (old_len != 0) {
            cap *= 2;
        }
        if (cap < 16) {
            cap = 16;
        }
        dst = embi_slab_alloc(cd->base.doc->slab, cap);
        if (old_len != 0) {
            if (wide == cd->data.wide) {
                embi_copy_bytes(dst, cd->data.ptr, (size_t)old_len * unit);
            } else {
                const uint8_t *o = cd->data.ptr;
                uint16_t *w = (uint16_t *)dst;
                for (uint32_t i = 0; i < old_len; i++) {
                    w[i] = o[i];
                }
            }
        }
    }
    embi_utf8_write(dst + (size_t)old_len * unit, wide, s);
    if (!in_place) {
        embi_chardata_adopt(cd, (embi_dstr){dst, nlen, wide});
    } else {
        cd->data.len = nlen;
    }
    if (cd->base.doc->observers.len != 0) {
        embi_notify_char_data(&cd->base);
    }
}

/* The tree is always current: text is appended to its node in place, so there is nothing to
 * write back. Kept so every return path to the caller has one place to guarantee that. */
void embi_tb_flush(embi_tb *tb)
{
    (void)tb;
}

bool embi_tb_stopped(const embi_tb *tb)
{
    return tb->stop;
}

/* Character data: appends to the previous text node when it is the node before the location. */
static void insert_text(embi_tb *tb, embi_str s)
{
    if (s.len == 0) {
        return;
    }
    loc L = appropriate(tb, nullptr);
    if (L.parent->type == EMBI_NODE_DOCUMENT) {
        return;
    }
    embi_document *d = L.parent->doc;
    embi_node *prev = L.before ? L.before->prev : L.parent->last_child;
    if (prev && prev->type == EMBI_NODE_TEXT) {
        text_append((embi_chardata *)prev, s);
        return;
    }
    embi_chardata *t = embi_new_text_utf8(d, s);
    link_at(L, &t->base);
}

/* Inserts a comment token, or a processing instruction token (both are TK_COMMENT). */
static void insert_comment(embi_tb *tb, loc L, const tk *t)
{
    (void)tb;
    embi_document *d = L.parent->doc;
    embi_dstr own;
    embi_dstr v = text_dstr(d->slab, t->text, &own);
    embi_chardata *c;
    if (t->raw->kind == EMBI_HTML_TOK_PI) {
        c = embi_new_pi(d, intern(t->raw->name), v);
    } else {
        c = embi_new_comment(d, v);
    }
    embi_dstr_free(own);
    link_at(L, &c->base);
}

/* ------------------------------------------------------------------ attributes and names */

typedef struct name_pair {
    const char *from;
    const char *to;
} name_pair;

static const name_pair k_svg_tags[] = {
    {"altglyph", "altGlyph"},
    {"altglyphdef", "altGlyphDef"},
    {"altglyphitem", "altGlyphItem"},
    {"animatecolor", "animateColor"},
    {"animatemotion", "animateMotion"},
    {"animatetransform", "animateTransform"},
    {"clippath", "clipPath"},
    {"feblend", "feBlend"},
    {"fecolormatrix", "feColorMatrix"},
    {"fecomponenttransfer", "feComponentTransfer"},
    {"fecomposite", "feComposite"},
    {"feconvolvematrix", "feConvolveMatrix"},
    {"fediffuselighting", "feDiffuseLighting"},
    {"fedisplacementmap", "feDisplacementMap"},
    {"fedistantlight", "feDistantLight"},
    {"fedropshadow", "feDropShadow"},
    {"feflood", "feFlood"},
    {"fefunca", "feFuncA"},
    {"fefuncb", "feFuncB"},
    {"fefuncg", "feFuncG"},
    {"fefuncr", "feFuncR"},
    {"fegaussianblur", "feGaussianBlur"},
    {"feimage", "feImage"},
    {"femerge", "feMerge"},
    {"femergenode", "feMergeNode"},
    {"femorphology", "feMorphology"},
    {"feoffset", "feOffset"},
    {"fepointlight", "fePointLight"},
    {"fespecularlighting", "feSpecularLighting"},
    {"fespotlight", "feSpotLight"},
    {"fetile", "feTile"},
    {"feturbulence", "feTurbulence"},
    {"foreignobject", "foreignObject"},
    {"glyphref", "glyphRef"},
    {"lineargradient", "linearGradient"},
    {"radialgradient", "radialGradient"},
    {"textpath", "textPath"},
};

static const name_pair k_svg_attrs[] = {
    {"attributename", "attributeName"},
    {"attributetype", "attributeType"},
    {"basefrequency", "baseFrequency"},
    {"baseprofile", "baseProfile"},
    {"calcmode", "calcMode"},
    {"clippathunits", "clipPathUnits"},
    {"diffuseconstant", "diffuseConstant"},
    {"edgemode", "edgeMode"},
    {"filterunits", "filterUnits"},
    {"glyphref", "glyphRef"},
    {"gradienttransform", "gradientTransform"},
    {"gradientunits", "gradientUnits"},
    {"kernelmatrix", "kernelMatrix"},
    {"kernelunitlength", "kernelUnitLength"},
    {"keypoints", "keyPoints"},
    {"keysplines", "keySplines"},
    {"keytimes", "keyTimes"},
    {"lengthadjust", "lengthAdjust"},
    {"limitingconeangle", "limitingConeAngle"},
    {"markerheight", "markerHeight"},
    {"markerunits", "markerUnits"},
    {"markerwidth", "markerWidth"},
    {"maskcontentunits", "maskContentUnits"},
    {"maskunits", "maskUnits"},
    {"numoctaves", "numOctaves"},
    {"pathlength", "pathLength"},
    {"patterncontentunits", "patternContentUnits"},
    {"patterntransform", "patternTransform"},
    {"patternunits", "patternUnits"},
    {"pointsatx", "pointsAtX"},
    {"pointsaty", "pointsAtY"},
    {"pointsatz", "pointsAtZ"},
    {"preservealpha", "preserveAlpha"},
    {"preserveaspectratio", "preserveAspectRatio"},
    {"primitiveunits", "primitiveUnits"},
    {"refx", "refX"},
    {"refy", "refY"},
    {"repeatcount", "repeatCount"},
    {"repeatdur", "repeatDur"},
    {"requiredextensions", "requiredExtensions"},
    {"requiredfeatures", "requiredFeatures"},
    {"specularconstant", "specularConstant"},
    {"specularexponent", "specularExponent"},
    {"spreadmethod", "spreadMethod"},
    {"startoffset", "startOffset"},
    {"stddeviation", "stdDeviation"},
    {"stitchtiles", "stitchTiles"},
    {"surfacescale", "surfaceScale"},
    {"systemlanguage", "systemLanguage"},
    {"tablevalues", "tableValues"},
    {"targetx", "targetX"},
    {"targety", "targetY"},
    {"textlength", "textLength"},
    {"viewbox", "viewBox"},
    {"viewtarget", "viewTarget"},
    {"xchannelselector", "xChannelSelector"},
    {"ychannelselector", "yChannelSelector"},
    {"zoomandpan", "zoomAndPan"},
};

static embi_str pair_lookup(const name_pair *tbl, size_t n, embi_str key, bool *found)
{
    for (size_t i = 0; i < n; i++) {
        if (lit_eq(key, tbl[i].from)) {
            *found = true;
            return (embi_str){tbl[i].to, (uint32_t)strlen(tbl[i].to)};
        }
    }
    *found = false;
    return key;
}

static embi_str svg_tag_adjust(embi_str name)
{
    bool found;
    embi_str r = pair_lookup(k_svg_tags, sizeof k_svg_tags / sizeof k_svg_tags[0], name, &found);
    return r;
}

static embi_str svg_attr_adjust(embi_str name)
{
    bool found;
    return pair_lookup(k_svg_attrs, sizeof k_svg_attrs / sizeof k_svg_attrs[0], name, &found);
}

/* Matches "prefix" at the start of s and yields the rest. */
static bool has_prefix(embi_str s, const char *p, embi_str *rest)
{
    size_t n = strlen(p);
    if (s.len <= n || memcmp(s.ptr, p, n) != 0) {
        return false;
    }
    rest->ptr = s.ptr + n;
    rest->len = s.len - (uint32_t)n;
    return true;
}

static bool is_xlink_local(embi_str s)
{
    return lit_eq(s, "actuate") || lit_eq(s, "arcrole") || lit_eq(s, "href") ||
           lit_eq(s, "role") || lit_eq(s, "show") || lit_eq(s, "title") || lit_eq(s, "type");
}

/* Sets an attribute of a token. HTML elements take it in the null namespace. Foreign elements get
 * the MathML and SVG attribute adjustments and the foreign attribute adjustments (xlink, xml,
 * xmlns). */
static void set_token_attr_ns(embi_element *el, uint8_t ns, embi_str name, embi_dstr v)
{
    if (ns == EMBI_NS_HTML) {
        embi_el_set_attr_atom(el, intern(name), v);
        return;
    }
    if (ns == EMBI_NS_MATHML && lit_eq(name, "definitionurl")) {
        name = EMBI_STR("definitionURL");
    } else if (ns == EMBI_NS_SVG) {
        name = svg_attr_adjust(name);
    }
    uint8_t xns = EMBI_NS_NULL;
    embi_str prefix = {nullptr, 0};
    embi_str local = name;
    embi_str rest;
    if (lit_eq(name, "xmlns")) {
        xns = EMBI_NS_XMLNS;
    } else if (lit_eq(name, "xmlns:xlink")) {
        xns = EMBI_NS_XMLNS;
        prefix = EMBI_STR("xmlns");
        local = EMBI_STR("xlink");
    } else if (has_prefix(name, "xlink:", &rest) && is_xlink_local(rest)) {
        xns = EMBI_NS_XLINK;
        prefix = EMBI_STR("xlink");
        local = rest;
    } else if (has_prefix(name, "xml:", &rest) && (lit_eq(rest, "lang") || lit_eq(rest, "space"))) {
        xns = EMBI_NS_XML;
        prefix = EMBI_STR("xml");
        local = rest;
    }
    if (xns == EMBI_NS_NULL) {
        embi_el_set_attr_atom(el, intern(local), v);
    } else {
        embi_atom pa = prefix.len ? intern(prefix) : 0;
        embi_el_set_attr_ns_atom(el, xns, 0, pa, intern(local), v);
    }
}

/* Adds the attributes of a token that the element does not have yet (html and body start tags). */
static void add_missing_attrs(embi_element *el, const embi_html_token *raw)
{
    if (!raw) {
        return;
    }
    for (uint32_t i = 0; i < raw->attr_count; i++) {
        const embi_html_attr *a = &raw->attrs[i];
        embi_atom local = intern(a->name);
        if (embi_el_find_attr(el, EMBI_NS_NULL, 0, local)) {
            continue;
        }
        embi_dstr own;
        embi_dstr v = text_dstr(el->base.doc->slab, a->value, &own);
        embi_el_set_attr_atom(el, local, v);
        embi_dstr_free(own);
    }
}

static const embi_html_attr *find_token_attr(const embi_html_token *raw, const char *name)
{
    if (!raw) {
        return nullptr;
    }
    for (uint32_t i = 0; i < raw->attr_count; i++) {
        if (lit_eq(raw->attrs[i].name, name)) {
            return &raw->attrs[i];
        }
    }
    return nullptr;
}

static bool token_has_attr(const embi_html_token *raw, const char *name)
{
    return find_token_attr(raw, name) != nullptr;
}

static bool type_is_hidden(const embi_html_token *raw)
{
    const embi_html_attr *a = find_token_attr(raw, "type");
    return a && eq_ci(a->value, "hidden");
}

/* ------------------------------------------------------------------ mode plumbing */

static void process(embi_tb *tb, const tk *t);
static void mode_run(embi_tb *tb, const tk *t);
static void m_in_body(embi_tb *tb, const tk *t);
static void m_in_template(embi_tb *tb, const tk *t);
static void in_head_rules(embi_tb *tb, const tk *t);
static void reset_mode(embi_tb *tb);
static void template_end(embi_tb *tb);
static bool adoption(embi_tb *tb, embi_atom subject);
static void any_other_end(embi_tb *tb, const tk *t);

static void set_template_mode(embi_tb *tb, tb_mode m)
{
    if (tb->tmodes.len) {
        tb->tmodes.data[tb->tmodes.len - 1] = (int)m;
    }
    tb->mode = m;
}

static void tok_state(embi_tb *tb, embi_html_state s)
{
    if (tb->tok) {
        embi_html_tokenizer_set_state(tb->tok, s);
    }
}

/* Generic RCDATA and raw text elements (WHATWG 13.2.6.2). */
static void generic_text(embi_tb *tb, const tk *t, bool rcdata)
{
    insert_el(tb, EMBI_NS_HTML, t->name, t->raw);
    tok_state(tb, rcdata ? EMBI_HTML_STATE_RCDATA : EMBI_HTML_STATE_RAWTEXT);
    tb->orig_mode = tb->mode;
    tb->mode = M_TEXT;
}

static void script_start(embi_tb *tb, const tk *t)
{
    insert_el(tb, EMBI_NS_HTML, EMBI_ATOM_script, t->raw);
    tok_state(tb, EMBI_HTML_STATE_SCRIPT_DATA);
    tb->orig_mode = tb->mode;
    tb->mode = M_TEXT;
}

/* Reset the insertion mode appropriately (WHATWG 13.2.4.1). */
static void reset_mode(embi_tb *tb)
{
    bool last = false;
    for (int i = (int)tb->stk.len - 1; i >= 0; i--) {
        embi_element *node = tb->stk.data[i];
        if (i == 0) {
            last = true;
            if (tb->fragment) {
                node = tb->context;
            }
        }
        if (is_html(node, EMBI_ATOM_select)) {
            if (!last) {
                for (int j = i - 1; j >= 1; j--) {
                    embi_element *anc = tb->stk.data[j];
                    if (is_html(anc, EMBI_ATOM_template)) {
                        break;
                    }
                    if (is_html(anc, EMBI_ATOM_table)) {
                        tb->mode = M_IN_SELECT_IN_TABLE;
                        return;
                    }
                }
            }
            tb->mode = M_IN_SELECT;
            return;
        }
        if ((is_html(node, EMBI_ATOM_td) || is_html(node, EMBI_ATOM_th)) && !last) {
            tb->mode = M_IN_CELL;
            return;
        }
        if (is_html(node, EMBI_ATOM_tr)) {
            tb->mode = M_IN_ROW;
            return;
        }
        if (is_html(node, EMBI_ATOM_tbody) || is_html(node, EMBI_ATOM_thead) ||
            is_html(node, EMBI_ATOM_tfoot)) {
            tb->mode = M_IN_TABLE_BODY;
            return;
        }
        if (is_html(node, EMBI_ATOM_caption)) {
            tb->mode = M_IN_CAPTION;
            return;
        }
        if (is_html(node, EMBI_ATOM_colgroup)) {
            tb->mode = M_IN_COLUMN_GROUP;
            return;
        }
        if (is_html(node, EMBI_ATOM_table)) {
            tb->mode = M_IN_TABLE;
            return;
        }
        if (is_html(node, EMBI_ATOM_template)) {
            tb->mode = tb->tmodes.len ? (tb_mode)tb->tmodes.data[tb->tmodes.len - 1] : M_IN_BODY;
            return;
        }
        if (is_html(node, EMBI_ATOM_head) && !last) {
            tb->mode = M_IN_HEAD;
            return;
        }
        if (is_html(node, EMBI_ATOM_body)) {
            tb->mode = M_IN_BODY;
            return;
        }
        if (is_html(node, EMBI_ATOM_frameset)) {
            tb->mode = M_IN_FRAMESET;
            return;
        }
        if (is_html(node, EMBI_ATOM_html)) {
            tb->mode = tb->head ? M_AFTER_HEAD : M_BEFORE_HEAD;
            return;
        }
        if (last) {
            tb->mode = M_IN_BODY;
            return;
        }
    }
    tb->mode = M_IN_BODY;
}

static void template_end(embi_tb *tb)
{
    if (!has_template(tb)) {
        return;
    }
    gen_implied(tb, 0, true);
    pop_until_html(tb, EMBI_ATOM_template);
    afe_clear(tb);
    if (tb->tmodes.len) {
        tb->tmodes.len--;
    }
    reset_mode(tb);
}

/* Foreign content (WHATWG 13.2.6.5): the rules for SVG and MathML elements. */
static bool is_breakout(const tk *t)
{
    switch (t->name) {
    case EMBI_ATOM_b:
    case EMBI_ATOM_big:
    case EMBI_ATOM_blockquote:
    case EMBI_ATOM_body:
    case EMBI_ATOM_br:
    case EMBI_ATOM_center:
    case EMBI_ATOM_code:
    case EMBI_ATOM_dd:
    case EMBI_ATOM_div:
    case EMBI_ATOM_dl:
    case EMBI_ATOM_dt:
    case EMBI_ATOM_em:
    case EMBI_ATOM_embed:
    case EMBI_ATOM_h1:
    case EMBI_ATOM_h2:
    case EMBI_ATOM_h3:
    case EMBI_ATOM_h4:
    case EMBI_ATOM_h5:
    case EMBI_ATOM_h6:
    case EMBI_ATOM_head:
    case EMBI_ATOM_hr:
    case EMBI_ATOM_i:
    case EMBI_ATOM_img:
    case EMBI_ATOM_li:
    case EMBI_ATOM_listing:
    case EMBI_ATOM_menu:
    case EMBI_ATOM_meta:
    case EMBI_ATOM_nobr:
    case EMBI_ATOM_ol:
    case EMBI_ATOM_p:
    case EMBI_ATOM_pre:
    case EMBI_ATOM_ruby:
    case EMBI_ATOM_s:
    case EMBI_ATOM_small:
    case EMBI_ATOM_span:
    case EMBI_ATOM_strong:
    case EMBI_ATOM_strike:
    case EMBI_ATOM_sub:
    case EMBI_ATOM_sup:
    case EMBI_ATOM_table:
    case EMBI_ATOM_tt:
    case EMBI_ATOM_u:
    case EMBI_ATOM_ul:
    case EMBI_ATOM_var:
        return true;
    case EMBI_ATOM_font:
        return token_has_attr(t->raw, "color") || token_has_attr(t->raw, "face") ||
               token_has_attr(t->raw, "size");
    default:
        return false;
    }
}

/* The dispatch: a token goes to the insertion mode unless the adjusted current node is a
 * foreign element that the token does not belong to (WHATWG 13.2.6). */
static bool use_mode_rules(const embi_tb *tb, const tk *t)
{
    if (tb->stk.len == 0) {
        return true;
    }
    embi_element *a = adjusted_current(tb);
    if (is_html_ns(a)) {
        return true;
    }
    if (is_mtip(a)) {
        if (t->kind == TK_START && t->name != EMBI_ATOM_mglyph && t->name != EMBI_ATOM_malignmark) {
            return true;
        }
        if (t->kind == TK_CHAR) {
            return true;
        }
    }
    if (a->base.extra == EMBI_NS_MATHML && a->base.name == EMBI_ATOM_annotation_xml &&
        t->kind == TK_START && t->name == EMBI_ATOM_svg) {
        return true;
    }
    if (is_hip(a) && (t->kind == TK_START || t->kind == TK_CHAR)) {
        return true;
    }
    return t->kind == TK_EOF;
}

static void foreign(embi_tb *tb, const tk *t)
{
    switch (t->kind) {
    case TK_CHAR:
        if (t->seg == SEG_NUL) {
            insert_text(tb, EMBI_STR(REPLACEMENT_CHAR));
            return;
        }
        insert_text(tb, t->text);
        if (t->seg == SEG_OTHER) {
            tb->frameset_ok = false;
        }
        return;
    case TK_COMMENT:
        insert_comment(tb, appropriate(tb, nullptr), t);
        return;
    case TK_DOCTYPE:
        return;
    case TK_START:
        if (is_breakout(t)) {
            while (tb->stk.len) {
                embi_element *c = cur(tb);
                if (is_html_ns(c) || is_mtip(c) || is_hip(c)) {
                    break;
                }
                pop(tb);
            }
            if (is_html_ns(cur(tb))) {
                mode_run(tb, t);
            } else {
                process(tb, t);
            }
            return;
        }
        {
            uint8_t ns = adjusted_current(tb)->base.extra;
            insert_foreign(tb, ns, t);
            if (t->raw->self_closing) {
                pop(tb);
            }
        }
        return;
    case TK_END: {
        if (t->name == EMBI_ATOM_br || t->name == EMBI_ATOM_p) {
            while (tb->stk.len) {
                embi_element *b = cur(tb);
                if (is_html_ns(b) || is_mtip(b) || is_hip(b)) {
                    break;
                }
                pop(tb);
            }
            /* "Reprocess the token according to the rules given in the section corresponding to
             * the current insertion mode in HTML content": never back through the dispatcher.
             * The loop above stops at an HTML integration point (foreignObject, desc, title,
             * annotation-xml) or a MathML text integration point, where the dispatcher sends an
             * end tag to the foreign rules again, which pop nothing and call process() again:
             * an endless loop for "<svg><foreignObject></p>". */
            mode_run(tb, t);
            return;
        }
        embi_element *c = cur(tb);
        if (is_el(c, EMBI_NS_SVG, EMBI_ATOM_script) && lit_eq(t->raw->name, "script")) {
            pop(tb);
            if (!tb->fragment && tb->on_script) {
                embi_html_script_result res = tb->on_script(tb->script_ctx, c);
                tb->pause = res != EMBI_HTML_SCRIPT_CONTINUE;
                tb->stop = res == EMBI_HTML_SCRIPT_STOP;
            }
            return;
        }
        int i = (int)tb->stk.len - 1;
        for (;;) {
            if (i <= 0) {
                return;
            }
            embi_element *node = tb->stk.data[i];
            if (embi_str_ieq(embi_atom_str(node->base.name), t->raw->name)) {
                pop_until_el(tb, node);
                return;
            }
            i--;
            if (is_html_ns(tb->stk.data[i])) {
                mode_run(tb, t);
                return;
            }
        }
    }
    case TK_EOF:
        mode_run(tb, t);
        return;
    }
}

static void process(embi_tb *tb, const tk *t)
{
    if (use_mode_rules(tb, t)) {
        mode_run(tb, t);
    } else {
        foreign(tb, t);
    }
}

/* ------------------------------------------------------------------ "in head" and friends */

/* The "in head" rules, which also serve "in template", "after head" and "in body" for the head
 * start and end tags. */
static void in_head_rules(embi_tb *tb, const tk *t)
{
    switch (t->kind) {
    case TK_CHAR:
        if (t->seg == SEG_WS) {
            insert_text(tb, t->text);
            return;
        }
        break;
    case TK_COMMENT:
        insert_comment(tb, appropriate(tb, nullptr), t);
        return;
    case TK_DOCTYPE:
        return;
    case TK_START:
        switch (t->name) {
        case EMBI_ATOM_html:
            m_in_body(tb, t);
            return;
        case EMBI_ATOM_base:
        case EMBI_ATOM_basefont:
        case EMBI_ATOM_bgsound:
        case EMBI_ATOM_link:
        case EMBI_ATOM_meta:
            insert_el(tb, EMBI_NS_HTML, t->name, t->raw);
            pop(tb);
            return;
        case EMBI_ATOM_title:
            generic_text(tb, t, true);
            return;
        case EMBI_ATOM_noscript:
            if (tb->scripting) {
                generic_text(tb, t, false);
                return;
            }
            insert_el(tb, EMBI_NS_HTML, t->name, t->raw);
            tb->mode = M_IN_HEAD_NOSCRIPT;
            return;
        case EMBI_ATOM_noframes:
        case EMBI_ATOM_style:
            generic_text(tb, t, false);
            return;
        case EMBI_ATOM_script:
            script_start(tb, t);
            return;
        case EMBI_ATOM_template:
            insert_el(tb, EMBI_NS_HTML, EMBI_ATOM_template, t->raw);
            afe_push_marker(tb);
            tb->mode = M_IN_TEMPLATE;
            embi_vec_push(&tb->tmodes, (int)M_IN_TEMPLATE);
            return;
        case EMBI_ATOM_head:
            return;
        default:
            break;
        }
        break;
    case TK_END:
        if (t->name == EMBI_ATOM_template) {
            template_end(tb);
            return;
        }
        if (t->name == EMBI_ATOM_head) {
            pop(tb);
            tb->mode = M_AFTER_HEAD;
            return;
        }
        if (t->name == EMBI_ATOM_body || t->name == EMBI_ATOM_html || t->name == EMBI_ATOM_br) {
            break;
        }
        return;
    case TK_EOF:
        break;
    }
    /* Anything else: leave the head element. */
    pop(tb);
    tb->mode = M_AFTER_HEAD;
    process(tb, t);
}

/* ------------------------------------------------------------------ quirks mode (WHATWG 13.2.6.4.1) */

static const char *const k_quirk_public_exact[] = {
    "-//W3O//DTD W3 HTML Strict 3.0//EN//",
    "-/W3C/DTD HTML 4.0 Transitional/EN",
    "HTML",
};

static const char *const k_quirk_public_prefix[] = {
    "+//Silmaril//dtd html Pro v0r11 19970101//",
    "-//AS//DTD HTML 3.0 asWedit + extensions//",
    "-//AdvaSoft Ltd//DTD HTML 3.0 asWedit + extensions//",
    "-//IETF//DTD HTML 2.0 Level 1//",
    "-//IETF//DTD HTML 2.0 Level 2//",
    "-//IETF//DTD HTML 2.0 Strict Level 1//",
    "-//IETF//DTD HTML 2.0 Strict Level 2//",
    "-//IETF//DTD HTML 2.0 Strict//",
    "-//IETF//DTD HTML 2.0//",
    "-//IETF//DTD HTML 2.1E//",
    "-//IETF//DTD HTML 3.0//",
    "-//IETF//DTD HTML 3.2 Final//",
    "-//IETF//DTD HTML 3.2//",
    "-//IETF//DTD HTML 3//",
    "-//IETF//DTD HTML Level 0//",
    "-//IETF//DTD HTML Level 1//",
    "-//IETF//DTD HTML Level 2//",
    "-//IETF//DTD HTML Level 3//",
    "-//IETF//DTD HTML Strict Level 0//",
    "-//IETF//DTD HTML Strict Level 1//",
    "-//IETF//DTD HTML Strict Level 2//",
    "-//IETF//DTD HTML Strict Level 3//",
    "-//IETF//DTD HTML Strict//",
    "-//IETF//DTD HTML//",
    "-//Metrius//DTD Metrius Presentational//",
    "-//Microsoft//DTD Internet Explorer 2.0 HTML Strict//",
    "-//Microsoft//DTD Internet Explorer 2.0 HTML//",
    "-//Microsoft//DTD Internet Explorer 2.0 Tables//",
    "-//Microsoft//DTD Internet Explorer 3.0 HTML Strict//",
    "-//Microsoft//DTD Internet Explorer 3.0 HTML//",
    "-//Microsoft//DTD Internet Explorer 3.0 Tables//",
    "-//Netscape Comm. Corp.//DTD HTML//",
    "-//Netscape Comm. Corp.//DTD Strict HTML//",
    "-//O'Reilly and Associates//DTD HTML 2.0//",
    "-//O'Reilly and Associates//DTD HTML Extended 1.0//",
    "-//O'Reilly and Associates//DTD HTML Extended Relaxed 1.0//",
    "-//SoftQuad Software//DTD HoTMetaL PRO 6.0::19990601::extensions to HTML 4.0//",
    "-//SoftQuad//DTD HoTMetaL PRO 4.0::19971010::extensions to HTML 4.0//",
    "-//Spyglass//DTD HTML 2.0 Extended//",
    "-//Sun Microsystems Corp.//DTD HotJava HTML//",
    "-//Sun Microsystems Corp.//DTD HotJava Strict HTML//",
    "-//W3C//DTD HTML 3 1995-03-24//",
    "-//W3C//DTD HTML 3.2 Draft//",
    "-//W3C//DTD HTML 3.2 Final//",
    "-//W3C//DTD HTML 3.2//",
    "-//W3C//DTD HTML 3.2S Draft//",
    "-//W3C//DTD HTML 4.0 Frameset//",
    "-//W3C//DTD HTML 4.0 Transitional//",
    "-//W3C//DTD HTML Experimental 19960712//",
    "-//W3C//DTD HTML Experimental 970421//",
    "-//W3C//DTD W3 HTML//",
    "-//W3O//DTD W3 HTML 3.0//",
    "-//WebTechs//DTD Mozilla HTML 2.0//",
    "-//WebTechs//DTD Mozilla HTML//",
};

static const char *const k_quirk_missing_system[] = {
    "-//W3C//DTD HTML 4.01 Frameset//",
    "-//W3C//DTD HTML 4.01 Transitional//",
};

static const char *const k_limited_public[] = {
    "-//W3C//DTD XHTML 1.0 Frameset//",
    "-//W3C//DTD XHTML 1.0 Transitional//",
};

static const char *const k_limited_with_system[] = {
    "-//W3C//DTD HTML 4.01 Frameset//",
    "-//W3C//DTD HTML 4.01 Transitional//",
};

static bool starts_with_any(embi_str s, const char *const *list, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (starts_ci(s, list[i])) {
            return true;
        }
    }
    return false;
}

/* The document mode for a DOCTYPE token (the quirks conditions of the spec). */
static embi_quirks doctype_quirks(const embi_html_token *r)
{
    if (r->force_quirks || !r->has_name || !lit_eq(r->name, "html")) {
        return EMBI_QUIRKS_FULL;
    }
    bool has_pub = r->has_public_id;
    bool has_sys = r->has_system_id;
    embi_str pub = r->public_id;
    embi_str sys = r->system_id;
    if (has_pub) {
        for (size_t i = 0; i < sizeof k_quirk_public_exact / sizeof k_quirk_public_exact[0]; i++) {
            if (eq_ci(pub, k_quirk_public_exact[i])) {
                return EMBI_QUIRKS_FULL;
            }
        }
        if (starts_with_any(pub, k_quirk_public_prefix,
                            sizeof k_quirk_public_prefix / sizeof k_quirk_public_prefix[0])) {
            return EMBI_QUIRKS_FULL;
        }
    }
    if (has_sys && eq_ci(sys, "http://www.ibm.com/data/dtd/v11/ibmxhtml1-transitional.dtd")) {
        return EMBI_QUIRKS_FULL;
    }
    if (has_pub && !has_sys &&
        starts_with_any(pub, k_quirk_missing_system,
                        sizeof k_quirk_missing_system / sizeof k_quirk_missing_system[0])) {
        return EMBI_QUIRKS_FULL;
    }
    if (has_pub && starts_with_any(pub, k_limited_public,
                                   sizeof k_limited_public / sizeof k_limited_public[0])) {
        return EMBI_QUIRKS_LIMITED;
    }
    if (has_pub && has_sys &&
        starts_with_any(pub, k_limited_with_system,
                        sizeof k_limited_with_system / sizeof k_limited_with_system[0])) {
        return EMBI_QUIRKS_LIMITED;
    }
    return EMBI_QUIRKS_NONE;
}

static void m_initial(embi_tb *tb, const tk *t)
{
    switch (t->kind) {
    case TK_CHAR:
        if (t->seg == SEG_WS) {
            return;
        }
        break;
    case TK_COMMENT:
        insert_comment(tb, doc_loc(tb), t);
        return;
    case TK_DOCTYPE: {
        const embi_html_token *r = t->raw;
        embi_document *d = tb->doc;
        embi_dstr on;
        embi_dstr op;
        embi_dstr os;
        embi_dstr dn = opt_dstr(d->slab, r->has_name, r->name, &on);
        embi_dstr dp = opt_dstr(d->slab, r->has_public_id, r->public_id, &op);
        embi_dstr ds = opt_dstr(d->slab, r->has_system_id, r->system_id, &os);
        embi_doctype *dt = embi_new_doctype(d, dn, dp, ds);
        embi_dstr_free(on);
        embi_dstr_free(op);
        embi_dstr_free(os);
        link_at(doc_loc(tb), &dt->base);
        if (!tb->fragment) {
            d->quirks = doctype_quirks(r);
        }
        tb->mode = M_BEFORE_HTML;
        return;
    }
    default:
        break;
    }
    if (!tb->fragment) {
        tb->doc->quirks = EMBI_QUIRKS_FULL;
    }
    tb->mode = M_BEFORE_HTML;
    process(tb, t);
}

static void m_before_html(embi_tb *tb, const tk *t)
{
    switch (t->kind) {
    case TK_DOCTYPE:
        return;
    case TK_COMMENT:
        insert_comment(tb, doc_loc(tb), t);
        return;
    case TK_CHAR:
        if (t->seg == SEG_WS) {
            return;
        }
        break;
    case TK_START:
        if (t->name == EMBI_ATOM_html) {
            embi_element *el = make_el(tb->doc, EMBI_NS_HTML, EMBI_ATOM_html, t->raw);
            link_at(doc_loc(tb), &el->base);
            push(tb, el);
            tb->mode = M_BEFORE_HEAD;
            return;
        }
        break;
    case TK_END:
        if (t->name != EMBI_ATOM_head && t->name != EMBI_ATOM_body && t->name != EMBI_ATOM_html &&
            t->name != EMBI_ATOM_br) {
            return;
        }
        break;
    case TK_EOF:
        break;
    }
    embi_element *el = make_el(tb->doc, EMBI_NS_HTML, EMBI_ATOM_html, nullptr);
    link_at(doc_loc(tb), &el->base);
    push(tb, el);
    tb->mode = M_BEFORE_HEAD;
    process(tb, t);
}

static void m_before_head(embi_tb *tb, const tk *t)
{
    switch (t->kind) {
    case TK_CHAR:
        if (t->seg == SEG_WS) {
            return;
        }
        break;
    case TK_COMMENT:
        insert_comment(tb, appropriate(tb, nullptr), t);
        return;
    case TK_DOCTYPE:
        return;
    case TK_START:
        if (t->name == EMBI_ATOM_html) {
            m_in_body(tb, t);
            return;
        }
        if (t->name == EMBI_ATOM_head) {
            tb->head = insert_el(tb, EMBI_NS_HTML, EMBI_ATOM_head, t->raw);
            tb->mode = M_IN_HEAD;
            return;
        }
        break;
    case TK_END:
        if (t->name != EMBI_ATOM_head && t->name != EMBI_ATOM_body && t->name != EMBI_ATOM_html &&
            t->name != EMBI_ATOM_br) {
            return;
        }
        break;
    case TK_EOF:
        break;
    }
    tb->head = insert_el(tb, EMBI_NS_HTML, EMBI_ATOM_head, nullptr);
    tb->mode = M_IN_HEAD;
    process(tb, t);
}

static void m_in_head_noscript(embi_tb *tb, const tk *t)
{
    switch (t->kind) {
    case TK_DOCTYPE:
        return;
    case TK_START:
        switch (t->name) {
        case EMBI_ATOM_html:
            m_in_body(tb, t);
            return;
        case EMBI_ATOM_basefont:
        case EMBI_ATOM_bgsound:
        case EMBI_ATOM_link:
        case EMBI_ATOM_meta:
        case EMBI_ATOM_noframes:
        case EMBI_ATOM_style:
            in_head_rules(tb, t);
            return;
        case EMBI_ATOM_head:
        case EMBI_ATOM_noscript:
            return;
        default:
            break;
        }
        break;
    case TK_END:
        if (t->name == EMBI_ATOM_noscript) {
            pop(tb);
            tb->mode = M_IN_HEAD;
            return;
        }
        if (t->name != EMBI_ATOM_br) {
            return;
        }
        break;
    case TK_CHAR:
        if (t->seg == SEG_WS) {
            in_head_rules(tb, t);
            return;
        }
        break;
    case TK_COMMENT:
        in_head_rules(tb, t);
        return;
    case TK_EOF:
        break;
    }
    pop(tb);
    tb->mode = M_IN_HEAD;
    process(tb, t);
}

static void m_after_head(embi_tb *tb, const tk *t)
{
    switch (t->kind) {
    case TK_CHAR:
        if (t->seg == SEG_WS) {
            insert_text(tb, t->text);
            return;
        }
        break;
    case TK_COMMENT:
        insert_comment(tb, appropriate(tb, nullptr), t);
        return;
    case TK_DOCTYPE:
        return;
    case TK_START:
        switch (t->name) {
        case EMBI_ATOM_html:
            m_in_body(tb, t);
            return;
        case EMBI_ATOM_body:
            insert_el(tb, EMBI_NS_HTML, EMBI_ATOM_body, t->raw);
            tb->frameset_ok = false;
            tb->mode = M_IN_BODY;
            return;
        case EMBI_ATOM_frameset:
            insert_el(tb, EMBI_NS_HTML, EMBI_ATOM_frameset, t->raw);
            tb->mode = M_IN_FRAMESET;
            return;
        case EMBI_ATOM_base:
        case EMBI_ATOM_basefont:
        case EMBI_ATOM_bgsound:
        case EMBI_ATOM_link:
        case EMBI_ATOM_meta:
        case EMBI_ATOM_noframes:
        case EMBI_ATOM_script:
        case EMBI_ATOM_style:
        case EMBI_ATOM_template:
        case EMBI_ATOM_title:
            if (tb->head) {
                push(tb, tb->head);
                in_head_rules(tb, t);
                int hi = stk_find(tb, tb->head);
                if (hi >= 0) {
                    stk_remove_at(tb, hi);
                }
            } else {
                in_head_rules(tb, t);
            }
            return;
        case EMBI_ATOM_head:
            return;
        default:
            break;
        }
        break;
    case TK_END:
        if (t->name == EMBI_ATOM_template) {
            in_head_rules(tb, t);
            return;
        }
        if (t->name != EMBI_ATOM_body && t->name != EMBI_ATOM_html && t->name != EMBI_ATOM_br) {
            return;
        }
        break;
    case TK_EOF:
        break;
    }
    insert_el(tb, EMBI_NS_HTML, EMBI_ATOM_body, nullptr);
    tb->mode = M_IN_BODY;
    process(tb, t);
}

/* The list of active formatting elements is reconstructed before inserting content (13.2.4.3). */
static void reconstruct(embi_tb *tb)
{
    uint32_t n = tb->afe.len;
    if (n == 0) {
        return;
    }
    embi_element *last = tb->afe.data[n - 1];
    if (!last || in_stack(tb, last)) {
        return;
    }
    int i = (int)n - 1;
    while (i > 0) {
        embi_element *e = tb->afe.data[i - 1];
        if (!e || in_stack(tb, e)) {
            break;
        }
        i--;
    }
    for (; i < (int)tb->afe.len; i++) {
        embi_element *old = tb->afe.data[i];
        loc L = appropriate(tb, nullptr);
        embi_element *nel = clone_formatting(L.parent->doc, old);
        link_at(L, &nel->base);
        push(tb, nel);
        tb->afe.data[i] = nel;
    }
}

/* Start tags of "in body" (WHATWG 13.2.6.4.7). */
static void body_start(embi_tb *tb, const tk *t)
{
    const embi_html_token *raw = t->raw;
    switch (t->name) {
    case EMBI_ATOM_html:
        if (!has_template(tb)) {
            add_missing_attrs(tb->stk.data[0], raw);
        }
        return;
    case EMBI_ATOM_base:
    case EMBI_ATOM_basefont:
    case EMBI_ATOM_bgsound:
    case EMBI_ATOM_link:
    case EMBI_ATOM_meta:
    case EMBI_ATOM_noframes:
    case EMBI_ATOM_script:
    case EMBI_ATOM_style:
    case EMBI_ATOM_template:
        tb->frameset_ok = false;
        in_head_rules(tb, t);
        return;
    case EMBI_ATOM_title:
        in_head_rules(tb, t);
        return;
    case EMBI_ATOM_body:
        if (tb->stk.len < 2 || !is_html(tb->stk.data[1], EMBI_ATOM_body) || has_template(tb)) {
            return;
        }
        tb->frameset_ok = false;
        add_missing_attrs(tb->stk.data[1], raw);
        return;
    case EMBI_ATOM_frameset:
        if (tb->stk.len < 2 || !is_html(tb->stk.data[1], EMBI_ATOM_body) || !tb->frameset_ok) {
            return;
        }
        embi_detach(&tb->stk.data[1]->base);
        while (tb->stk.len > 1) {
            pop(tb);
        }
        insert_el(tb, EMBI_NS_HTML, EMBI_ATOM_frameset, raw);
        tb->mode = M_IN_FRAMESET;
        return;
    case EMBI_ATOM_address:
    case EMBI_ATOM_article:
    case EMBI_ATOM_aside:
    case EMBI_ATOM_blockquote:
    case EMBI_ATOM_center:
    case EMBI_ATOM_details:
    case EMBI_ATOM_dialog:
    case EMBI_ATOM_dir:
    case EMBI_ATOM_div:
    case EMBI_ATOM_dl:
    case EMBI_ATOM_fieldset:
    case EMBI_ATOM_figcaption:
    case EMBI_ATOM_figure:
    case EMBI_ATOM_footer:
    case EMBI_ATOM_header:
    case EMBI_ATOM_hgroup:
    case EMBI_ATOM_main:
    case EMBI_ATOM_menu:
    case EMBI_ATOM_nav:
    case EMBI_ATOM_ol:
    case EMBI_ATOM_p:
    case EMBI_ATOM_search:
    case EMBI_ATOM_section:
    case EMBI_ATOM_summary:
    case EMBI_ATOM_ul:
        if (name_in_scope(tb, EMBI_ATOM_p, SCOPE_BUTTON)) {
            close_p(tb);
        }
        insert_el(tb, EMBI_NS_HTML, t->name, raw);
        return;
    case EMBI_ATOM_h1:
    case EMBI_ATOM_h2:
    case EMBI_ATOM_h3:
    case EMBI_ATOM_h4:
    case EMBI_ATOM_h5:
    case EMBI_ATOM_h6:
        if (name_in_scope(tb, EMBI_ATOM_p, SCOPE_BUTTON)) {
            close_p(tb);
        }
        if (is_heading(cur(tb))) {
            pop(tb);
        }
        insert_el(tb, EMBI_NS_HTML, t->name, raw);
        return;
    case EMBI_ATOM_pre:
    case EMBI_ATOM_listing:
        if (name_in_scope(tb, EMBI_ATOM_p, SCOPE_BUTTON)) {
            close_p(tb);
        }
        insert_el(tb, EMBI_NS_HTML, t->name, raw);
        tb->skip_lf = true;
        tb->frameset_ok = false;
        return;
    case EMBI_ATOM_form: {
        if (tb->form && !form_in_template(tb)) {
            return;
        }
        bool in_tmpl = form_in_template(tb);
        if (name_in_scope(tb, EMBI_ATOM_p, SCOPE_BUTTON)) {
            close_p(tb);
        }
        embi_element *el = insert_el(tb, EMBI_NS_HTML, EMBI_ATOM_form, raw);
        if (!in_tmpl) {
            tb->form = el;
        }
        return;
    }
    case EMBI_ATOM_li:
        tb->frameset_ok = false;
        for (int i = (int)tb->stk.len - 1; i >= 0; i--) {
            embi_element *node = tb->stk.data[i];
            if (is_html(node, EMBI_ATOM_li)) {
                gen_implied(tb, EMBI_ATOM_li, false);
                pop_until_el(tb, node);
                break;
            }
            if (is_special(node) && !is_html(node, EMBI_ATOM_address) &&
                !is_html(node, EMBI_ATOM_div) && !is_html(node, EMBI_ATOM_p)) {
                break;
            }
        }
        if (name_in_scope(tb, EMBI_ATOM_p, SCOPE_BUTTON)) {
            close_p(tb);
        }
        insert_el(tb, EMBI_NS_HTML, EMBI_ATOM_li, raw);
        return;
    case EMBI_ATOM_dd:
    case EMBI_ATOM_dt:
        tb->frameset_ok = false;
        for (int i = (int)tb->stk.len - 1; i >= 0; i--) {
            embi_element *node = tb->stk.data[i];
            if (is_html(node, EMBI_ATOM_dd) || is_html(node, EMBI_ATOM_dt)) {
                gen_implied(tb, node->base.name, false);
                pop_until_el(tb, node);
                break;
            }
            if (is_special(node) && !is_html(node, EMBI_ATOM_address) &&
                !is_html(node, EMBI_ATOM_div) && !is_html(node, EMBI_ATOM_p)) {
                break;
            }
        }
        if (name_in_scope(tb, EMBI_ATOM_p, SCOPE_BUTTON)) {
            close_p(tb);
        }
        insert_el(tb, EMBI_NS_HTML, t->name, raw);
        return;
    case EMBI_ATOM_plaintext:
        if (name_in_scope(tb, EMBI_ATOM_p, SCOPE_BUTTON)) {
            close_p(tb);
        }
        insert_el(tb, EMBI_NS_HTML, EMBI_ATOM_plaintext, raw);
        tok_state(tb, EMBI_HTML_STATE_PLAINTEXT);
        return;
    case EMBI_ATOM_button:
        if (name_in_scope(tb, EMBI_ATOM_button, SCOPE_DEFAULT)) {
            gen_implied(tb, 0, false);
            pop_until_html(tb, EMBI_ATOM_button);
        }
        reconstruct(tb);
        insert_el(tb, EMBI_NS_HTML, EMBI_ATOM_button, raw);
        tb->frameset_ok = false;
        return;
    case EMBI_ATOM_a: {
        embi_element *old = afe_last_named(tb, EMBI_ATOM_a);
        if (old) {
            adoption(tb, EMBI_ATOM_a);
            afe_remove_el(tb, old, nullptr);
            int si = stk_find(tb, old);
            if (si >= 0) {
                stk_remove_at(tb, si);
            }
        }
        reconstruct(tb);
        embi_element *el = insert_el(tb, EMBI_NS_HTML, EMBI_ATOM_a, raw);
        afe_push(tb, el);
        return;
    }
    case EMBI_ATOM_b:
    case EMBI_ATOM_big:
    case EMBI_ATOM_code:
    case EMBI_ATOM_em:
    case EMBI_ATOM_font:
    case EMBI_ATOM_i:
    case EMBI_ATOM_s:
    case EMBI_ATOM_small:
    case EMBI_ATOM_strike:
    case EMBI_ATOM_strong:
    case EMBI_ATOM_tt:
    case EMBI_ATOM_u: {
        reconstruct(tb);
        embi_element *el = insert_el(tb, EMBI_NS_HTML, t->name, raw);
        afe_push(tb, el);
        return;
    }
    case EMBI_ATOM_nobr: {
        reconstruct(tb);
        if (name_in_scope(tb, EMBI_ATOM_nobr, SCOPE_DEFAULT)) {
            if (!adoption(tb, EMBI_ATOM_nobr)) {
                any_other_end(tb, t);
            }
            reconstruct(tb);
        }
        embi_element *el = insert_el(tb, EMBI_NS_HTML, EMBI_ATOM_nobr, raw);
        afe_push(tb, el);
        return;
    }
    case EMBI_ATOM_applet:
    case EMBI_ATOM_marquee:
    case EMBI_ATOM_object:
        reconstruct(tb);
        insert_el(tb, EMBI_NS_HTML, t->name, raw);
        afe_push_marker(tb);
        tb->frameset_ok = false;
        return;
    case EMBI_ATOM_table:
        if (tb->doc->quirks != EMBI_QUIRKS_FULL && name_in_scope(tb, EMBI_ATOM_p, SCOPE_BUTTON)) {
            close_p(tb);
        }
        insert_el(tb, EMBI_NS_HTML, EMBI_ATOM_table, raw);
        tb->frameset_ok = false;
        tb->mode = M_IN_TABLE;
        return;
    case EMBI_ATOM_area:
    case EMBI_ATOM_br:
    case EMBI_ATOM_embed:
    case EMBI_ATOM_img:
    case EMBI_ATOM_keygen:
    case EMBI_ATOM_wbr:
        reconstruct(tb);
        insert_el(tb, EMBI_NS_HTML, t->name, raw);
        pop(tb);
        tb->frameset_ok = false;
        return;
    case EMBI_ATOM_input:
        reconstruct(tb);
        insert_el(tb, EMBI_NS_HTML, EMBI_ATOM_input, raw);
        pop(tb);
        if (!type_is_hidden(raw)) {
            tb->frameset_ok = false;
        }
        return;
    case EMBI_ATOM_param:
    case EMBI_ATOM_source:
    case EMBI_ATOM_track:
        insert_el(tb, EMBI_NS_HTML, t->name, raw);
        pop(tb);
        return;
    case EMBI_ATOM_hr:
        if (name_in_scope(tb, EMBI_ATOM_p, SCOPE_BUTTON)) {
            close_p(tb);
        }
        insert_el(tb, EMBI_NS_HTML, EMBI_ATOM_hr, raw);
        pop(tb);
        tb->frameset_ok = false;
        return;
    case EMBI_ATOM_image: {
        tk t2 = *t;
        t2.name = EMBI_ATOM_img;
        process(tb, &t2);
        return;
    }
    case EMBI_ATOM_textarea:
        insert_el(tb, EMBI_NS_HTML, EMBI_ATOM_textarea, raw);
        tb->skip_lf = true;
        tok_state(tb, EMBI_HTML_STATE_RCDATA);
        tb->orig_mode = tb->mode;
        tb->frameset_ok = false;
        tb->mode = M_TEXT;
        return;
    case EMBI_ATOM_xmp:
        if (name_in_scope(tb, EMBI_ATOM_p, SCOPE_BUTTON)) {
            close_p(tb);
        }
        reconstruct(tb);
        tb->frameset_ok = false;
        generic_text(tb, t, false);
        return;
    case EMBI_ATOM_iframe:
        tb->frameset_ok = false;
        generic_text(tb, t, false);
        return;
    case EMBI_ATOM_noembed:
        generic_text(tb, t, false);
        return;
    case EMBI_ATOM_noscript:
        if (tb->scripting) {
            generic_text(tb, t, false);
            return;
        }
        break;
    case EMBI_ATOM_select:
        reconstruct(tb);
        insert_el(tb, EMBI_NS_HTML, EMBI_ATOM_select, raw);
        tb->has_select = true;
        tb->frameset_ok = false;
        if (tb->mode == M_IN_TABLE || tb->mode == M_IN_CAPTION || tb->mode == M_IN_TABLE_BODY ||
            tb->mode == M_IN_ROW || tb->mode == M_IN_CELL) {
            tb->mode = M_IN_SELECT_IN_TABLE;
        } else {
            tb->mode = M_IN_SELECT;
        }
        return;
    case EMBI_ATOM_optgroup:
    case EMBI_ATOM_option:
        if (is_html(cur(tb), EMBI_ATOM_option)) {
            pop(tb);
        }
        reconstruct(tb);
        insert_el(tb, EMBI_NS_HTML, t->name, raw);
        return;
    case EMBI_ATOM_rb:
    case EMBI_ATOM_rtc:
        if (name_in_scope(tb, EMBI_ATOM_ruby, SCOPE_DEFAULT)) {
            gen_implied(tb, 0, false);
        }
        insert_el(tb, EMBI_NS_HTML, t->name, raw);
        return;
    case EMBI_ATOM_rp:
    case EMBI_ATOM_rt:
        if (name_in_scope(tb, EMBI_ATOM_ruby, SCOPE_DEFAULT)) {
            gen_implied(tb, EMBI_ATOM_rtc, false);
        }
        insert_el(tb, EMBI_NS_HTML, t->name, raw);
        return;
    case EMBI_ATOM_math:
        reconstruct(tb);
        insert_foreign(tb, EMBI_NS_MATHML, t);
        if (raw->self_closing) {
            pop(tb);
        }
        return;
    case EMBI_ATOM_svg:
        reconstruct(tb);
        insert_foreign(tb, EMBI_NS_SVG, t);
        if (raw->self_closing) {
            pop(tb);
        }
        return;
    case EMBI_ATOM_caption:
    case EMBI_ATOM_col:
    case EMBI_ATOM_colgroup:
    case EMBI_ATOM_frame:
    case EMBI_ATOM_head:
    case EMBI_ATOM_tbody:
    case EMBI_ATOM_td:
    case EMBI_ATOM_tfoot:
    case EMBI_ATOM_th:
    case EMBI_ATOM_thead:
    case EMBI_ATOM_tr:
        return;
    default:
        break;
    }
    reconstruct(tb);
    insert_el(tb, EMBI_NS_HTML, t->name, raw);
}

/* The adoption agency algorithm (WHATWG 13.2.6.4.7). Returns false when the caller must run
 * "any other end tag" instead (there is no formatting element for the subject). */
static bool adoption(embi_tb *tb, embi_atom subject)
{
    embi_element *c = cur(tb);
    if (is_html(c, subject) && afe_find(tb, c) < 0) {
        pop(tb);
        return true;
    }
    for (int outer = 0; outer < 8; outer++) {
        embi_element *fe = afe_last_named(tb, subject);
        if (!fe) {
            return false;
        }
        int fe_idx = stk_find(tb, fe);
        if (fe_idx < 0) {
            afe_remove_el(tb, fe, nullptr);
            return true;
        }
        if (!el_in_scope(tb, fe, SCOPE_DEFAULT)) {
            return true;
        }
        int fb_idx = -1;
        for (int i = fe_idx + 1; i < (int)tb->stk.len; i++) {
            if (is_special(tb->stk.data[i])) {
                fb_idx = i;
                break;
            }
        }
        if (fb_idx < 0) {
            while ((int)tb->stk.len > fe_idx) {
                pop(tb);
            }
            afe_remove_el(tb, fe, nullptr);
            return true;
        }
        if (fe_idx == 0) {
            return true;
        }
        embi_element *fb = tb->stk.data[fb_idx];
        embi_element *common = tb->stk.data[fe_idx - 1];
        int bm = afe_find(tb, fe);
        embi_element *last = fb;
        int ni = fb_idx;
        for (int inner = 1;; inner++) {
            ni--;
            embi_element *node = tb->stk.data[ni];
            if (node == fe) {
                break;
            }
            if (inner > 3 && afe_find(tb, node) >= 0) {
                afe_remove_el(tb, node, &bm);
            }
            if (afe_find(tb, node) < 0) {
                stk_remove_at(tb, ni);
                continue;
            }
            int ai = afe_find(tb, node);
            embi_element *nel = clone_formatting(node->base.doc, node);
            tb->afe.data[ai] = nel;
            stk_count(tb, tb->stk.data[ni], -1);
            stk_count(tb, nel, 1);
            tb->stk.data[ni] = nel;
            if (last == fb) {
                bm = ai + 1;
            }
            embi_detach(&last->base);
            embi_tree_append_raw(&nel->base, &last->base);
            last = nel;
        }
        loc L = appropriate(tb, common);
        embi_detach(&last->base);
        link_at(L, &last->base);
        embi_element *nfe = clone_formatting(fb->base.doc, fe);
        while (fb->base.first_child) {
            embi_node *child = fb->base.first_child;
            embi_detach(child);
            embi_tree_append_raw(&nfe->base, child);
        }
        link_at((loc){&fb->base, nullptr}, &nfe->base);
        afe_remove_el(tb, fe, &bm);
        afe_insert_at(tb, bm, nfe);
        int fe_now = stk_find(tb, fe);
        if (fe_now >= 0) {
            stk_remove_at(tb, fe_now);
        }
        int fb_now = stk_find(tb, fb);
        stk_insert_at(tb, fb_now + 1, nfe);
    }
    return true;
}

/* "Any other end tag" (WHATWG 13.2.6.4.7). */
static void any_other_end(embi_tb *tb, const tk *t)
{
    for (int i = (int)tb->stk.len - 1; i >= 0; i--) {
        embi_element *node = tb->stk.data[i];
        if (is_html(node, t->name)) {
            gen_implied(tb, t->name, false);
            pop_until_el(tb, node);
            return;
        }
        if (is_special(node)) {
            return;
        }
    }
}

/* End tags of "in body" (WHATWG 13.2.6.4.7). */
static void body_end(embi_tb *tb, const tk *t)
{
    switch (t->name) {
    case EMBI_ATOM_template:
        in_head_rules(tb, t);
        return;
    case EMBI_ATOM_body:
        if (!name_in_scope(tb, EMBI_ATOM_body, SCOPE_DEFAULT)) {
            return;
        }
        tb->mode = M_AFTER_BODY;
        return;
    case EMBI_ATOM_html:
        if (!name_in_scope(tb, EMBI_ATOM_body, SCOPE_DEFAULT)) {
            return;
        }
        tb->mode = M_AFTER_BODY;
        process(tb, t);
        return;
    case EMBI_ATOM_address:
    case EMBI_ATOM_article:
    case EMBI_ATOM_aside:
    case EMBI_ATOM_blockquote:
    case EMBI_ATOM_button:
    case EMBI_ATOM_center:
    case EMBI_ATOM_details:
    case EMBI_ATOM_dialog:
    case EMBI_ATOM_dir:
    case EMBI_ATOM_div:
    case EMBI_ATOM_dl:
    case EMBI_ATOM_fieldset:
    case EMBI_ATOM_figcaption:
    case EMBI_ATOM_figure:
    case EMBI_ATOM_footer:
    case EMBI_ATOM_header:
    case EMBI_ATOM_hgroup:
    case EMBI_ATOM_listing:
    case EMBI_ATOM_main:
    case EMBI_ATOM_menu:
    case EMBI_ATOM_nav:
    case EMBI_ATOM_ol:
    case EMBI_ATOM_pre:
    case EMBI_ATOM_search:
    case EMBI_ATOM_section:
    case EMBI_ATOM_summary:
    case EMBI_ATOM_ul:
        if (!name_in_scope(tb, t->name, SCOPE_DEFAULT)) {
            return;
        }
        gen_implied(tb, 0, false);
        pop_until_html(tb, t->name);
        return;
    case EMBI_ATOM_form:
        if (!form_in_template(tb)) {
            embi_element *node = tb->form;
            tb->form = nullptr;
            if (!node || !el_in_scope(tb, node, SCOPE_DEFAULT)) {
                return;
            }
            gen_implied(tb, 0, false);
            int i = stk_find(tb, node);
            if (i >= 0) {
                stk_remove_at(tb, i);
            }
            return;
        }
        if (!name_in_scope(tb, EMBI_ATOM_form, SCOPE_DEFAULT)) {
            return;
        }
        gen_implied(tb, 0, false);
        pop_until_html(tb, EMBI_ATOM_form);
        return;
    case EMBI_ATOM_p:
        if (!name_in_scope(tb, EMBI_ATOM_p, SCOPE_BUTTON)) {
            tk s = {TK_START, nullptr, EMBI_ATOM_p, {nullptr, 0}, 0};
            body_start(tb, &s);
        }
        close_p(tb);
        return;
    case EMBI_ATOM_li:
        if (!name_in_scope(tb, EMBI_ATOM_li, SCOPE_LIST_ITEM)) {
            return;
        }
        gen_implied(tb, EMBI_ATOM_li, false);
        pop_until_html(tb, EMBI_ATOM_li);
        return;
    case EMBI_ATOM_dd:
    case EMBI_ATOM_dt:
        if (!name_in_scope(tb, t->name, SCOPE_DEFAULT)) {
            return;
        }
        gen_implied(tb, t->name, false);
        pop_until_html(tb, t->name);
        return;
    case EMBI_ATOM_h1:
    case EMBI_ATOM_h2:
    case EMBI_ATOM_h3:
    case EMBI_ATOM_h4:
    case EMBI_ATOM_h5:
    case EMBI_ATOM_h6: {
        const embi_atom hs[6] = {EMBI_ATOM_h1, EMBI_ATOM_h2, EMBI_ATOM_h3,
                                 EMBI_ATOM_h4, EMBI_ATOM_h5, EMBI_ATOM_h6};
        if (!names_in_scope(tb, hs, 6, SCOPE_DEFAULT)) {
            return;
        }
        gen_implied(tb, 0, false);
        pop_until_heading(tb);
        return;
    }
    case EMBI_ATOM_a:
    case EMBI_ATOM_b:
    case EMBI_ATOM_big:
    case EMBI_ATOM_code:
    case EMBI_ATOM_em:
    case EMBI_ATOM_font:
    case EMBI_ATOM_i:
    case EMBI_ATOM_nobr:
    case EMBI_ATOM_s:
    case EMBI_ATOM_small:
    case EMBI_ATOM_strike:
    case EMBI_ATOM_strong:
    case EMBI_ATOM_tt:
    case EMBI_ATOM_u:
        if (!adoption(tb, t->name)) {
            any_other_end(tb, t);
        }
        return;
    case EMBI_ATOM_applet:
    case EMBI_ATOM_marquee:
    case EMBI_ATOM_object:
        if (!name_in_scope(tb, t->name, SCOPE_DEFAULT)) {
            return;
        }
        gen_implied(tb, 0, false);
        pop_until_html(tb, t->name);
        afe_clear(tb);
        return;
    case EMBI_ATOM_br: {
        tk s = {TK_START, nullptr, EMBI_ATOM_br, {nullptr, 0}, 0};
        body_start(tb, &s);
        return;
    }
    default:
        break;
    }
    any_other_end(tb, t);
}

static void m_in_body(embi_tb *tb, const tk *t)
{
    switch (t->kind) {
    case TK_CHAR:
        if (t->seg == SEG_NUL) {
            return;
        }
        reconstruct(tb);
        insert_text(tb, t->text);
        if (t->seg == SEG_OTHER) {
            tb->frameset_ok = false;
        }
        return;
    case TK_COMMENT:
        insert_comment(tb, appropriate(tb, nullptr), t);
        return;
    case TK_DOCTYPE:
        return;
    case TK_START:
        body_start(tb, t);
        return;
    case TK_END:
        body_end(tb, t);
        return;
    case TK_EOF:
        if (tb->tmodes.len) {
            m_in_template(tb, t);
        }
        return;
    }
}

/* "Text" mode: raw text and RCDATA content of script, style, textarea and the like. */
static void m_text(embi_tb *tb, const tk *t)
{
    switch (t->kind) {
    case TK_CHAR:
        insert_text(tb, t->text);
        return;
    case TK_EOF:
        pop(tb);
        tb->mode = tb->orig_mode;
        process(tb, t);
        return;
    case TK_END: {
        embi_element *node = cur(tb);
        pop(tb);
        tb->mode = tb->orig_mode;
        if (t->name == EMBI_ATOM_script && is_html(node, EMBI_ATOM_script) && !tb->fragment &&
            tb->on_script != nullptr) {
            embi_html_script_result res = tb->on_script(tb->script_ctx, node);
            tb->pause = res != EMBI_HTML_SCRIPT_CONTINUE;
            tb->stop = res == EMBI_HTML_SCRIPT_STOP;
        }
        return;
    }
    case TK_START:
    case TK_COMMENT:
    case TK_DOCTYPE:
        return;
    }
}

/* Pending table character tokens are flushed when a non-character token arrives. */
static void flush_pending(embi_tb *tb)
{
    if (tb->pending.len == 0) {
        return;
    }
    embi_str s = {tb->pending.data, tb->pending.len};
    if (tb->pending_nonws) {
        bool saved = tb->foster;
        tb->foster = true;
        reconstruct(tb);
        insert_text(tb, s);
        tb->foster = saved;
        tb->frameset_ok = false;
    } else {
        insert_text(tb, s);
    }
    embi_sb_clear(&tb->pending);
    tb->pending_nonws = false;
}

static void m_in_table_text(embi_tb *tb, const tk *t)
{
    if (t->kind == TK_CHAR) {
        if (t->seg == SEG_NUL) {
            return;
        }
        embi_sb_append(&tb->pending, t->text);
        if (t->seg == SEG_OTHER) {
            tb->pending_nonws = true;
        }
        return;
    }
    flush_pending(tb);
    tb->mode = tb->orig_mode;
    process(tb, t);
}

static void m_in_table(embi_tb *tb, const tk *t)
{
    switch (t->kind) {
    case TK_CHAR: {
        embi_element *c = cur(tb);
        if (is_html(c, EMBI_ATOM_table) || is_html(c, EMBI_ATOM_tbody) ||
            is_html(c, EMBI_ATOM_template) || is_html(c, EMBI_ATOM_tfoot) ||
            is_html(c, EMBI_ATOM_thead) || is_html(c, EMBI_ATOM_tr)) {
            embi_sb_clear(&tb->pending);
            tb->pending_nonws = false;
            tb->orig_mode = tb->mode;
            tb->mode = M_IN_TABLE_TEXT;
            process(tb, t);
            return;
        }
        break;
    }
    case TK_COMMENT:
        insert_comment(tb, appropriate(tb, nullptr), t);
        return;
    case TK_DOCTYPE:
        return;
    case TK_START:
        switch (t->name) {
        case EMBI_ATOM_caption:
            clear_to(tb, CLEAR_TABLE);
            afe_push_marker(tb);
            insert_el(tb, EMBI_NS_HTML, EMBI_ATOM_caption, t->raw);
            tb->mode = M_IN_CAPTION;
            return;
        case EMBI_ATOM_colgroup:
            clear_to(tb, CLEAR_TABLE);
            insert_el(tb, EMBI_NS_HTML, EMBI_ATOM_colgroup, t->raw);
            tb->mode = M_IN_COLUMN_GROUP;
            return;
        case EMBI_ATOM_col:
            clear_to(tb, CLEAR_TABLE);
            insert_el(tb, EMBI_NS_HTML, EMBI_ATOM_colgroup, nullptr);
            tb->mode = M_IN_COLUMN_GROUP;
            process(tb, t);
            return;
        case EMBI_ATOM_tbody:
        case EMBI_ATOM_tfoot:
        case EMBI_ATOM_thead:
            clear_to(tb, CLEAR_TABLE);
            insert_el(tb, EMBI_NS_HTML, t->name, t->raw);
            tb->mode = M_IN_TABLE_BODY;
            return;
        case EMBI_ATOM_td:
        case EMBI_ATOM_th:
        case EMBI_ATOM_tr:
            clear_to(tb, CLEAR_TABLE);
            insert_el(tb, EMBI_NS_HTML, EMBI_ATOM_tbody, nullptr);
            tb->mode = M_IN_TABLE_BODY;
            process(tb, t);
            return;
        case EMBI_ATOM_table:
            if (!name_in_scope(tb, EMBI_ATOM_table, SCOPE_TABLE)) {
                return;
            }
            pop_until_html(tb, EMBI_ATOM_table);
            reset_mode(tb);
            process(tb, t);
            return;
        case EMBI_ATOM_style:
        case EMBI_ATOM_script:
        case EMBI_ATOM_template:
            in_head_rules(tb, t);
            return;
        case EMBI_ATOM_input:
            if (!type_is_hidden(t->raw)) {
                break;
            }
            insert_el(tb, EMBI_NS_HTML, EMBI_ATOM_input, t->raw);
            pop(tb);
            return;
        case EMBI_ATOM_form: {
            /* Deliberate deviation from the WHATWG text, which ignores a form here when a
             * template is on the stack: the vendored template.dat test 117
             * ("<template><table><form></table></template>") expects the form element to be
             * inserted into the table (and left off the form element pointer). */
            if (tb->form) {
                return;
            }
            embi_element *el = insert_el(tb, EMBI_NS_HTML, EMBI_ATOM_form, t->raw);
            if (!has_template(tb)) {
                tb->form = el;
            }
            pop(tb);
            return;
        }
        default:
            break;
        }
        break;
    case TK_END:
        switch (t->name) {
        case EMBI_ATOM_table:
            if (!name_in_scope(tb, EMBI_ATOM_table, SCOPE_TABLE)) {
                return;
            }
            pop_until_html(tb, EMBI_ATOM_table);
            reset_mode(tb);
            return;
        case EMBI_ATOM_body:
        case EMBI_ATOM_caption:
        case EMBI_ATOM_col:
        case EMBI_ATOM_colgroup:
        case EMBI_ATOM_html:
        case EMBI_ATOM_tbody:
        case EMBI_ATOM_td:
        case EMBI_ATOM_tfoot:
        case EMBI_ATOM_th:
        case EMBI_ATOM_thead:
        case EMBI_ATOM_tr:
            return;
        case EMBI_ATOM_template:
            in_head_rules(tb, t);
            return;
        default:
            break;
        }
        break;
    case TK_EOF:
        m_in_body(tb, t);
        return;
    }
    /* Anything else: process in "in body" with foster parenting. */
    tb->foster = true;
    m_in_body(tb, t);
    tb->foster = false;
}

static void close_caption(embi_tb *tb)
{
    gen_implied(tb, 0, false);
    pop_until_html(tb, EMBI_ATOM_caption);
    afe_clear(tb);
    tb->mode = M_IN_TABLE;
}

static void m_in_caption(embi_tb *tb, const tk *t)
{
    if (t->kind == TK_START) {
        switch (t->name) {
        case EMBI_ATOM_caption:
        case EMBI_ATOM_col:
        case EMBI_ATOM_colgroup:
        case EMBI_ATOM_tbody:
        case EMBI_ATOM_td:
        case EMBI_ATOM_tfoot:
        case EMBI_ATOM_th:
        case EMBI_ATOM_thead:
        case EMBI_ATOM_tr:
            if (!name_in_scope(tb, EMBI_ATOM_caption, SCOPE_TABLE)) {
                return;
            }
            close_caption(tb);
            process(tb, t);
            return;
        default:
            break;
        }
    } else if (t->kind == TK_END) {
        switch (t->name) {
        case EMBI_ATOM_caption:
            if (!name_in_scope(tb, EMBI_ATOM_caption, SCOPE_TABLE)) {
                return;
            }
            close_caption(tb);
            return;
        case EMBI_ATOM_table:
            if (!name_in_scope(tb, EMBI_ATOM_caption, SCOPE_TABLE)) {
                return;
            }
            close_caption(tb);
            process(tb, t);
            return;
        case EMBI_ATOM_body:
        case EMBI_ATOM_col:
        case EMBI_ATOM_colgroup:
        case EMBI_ATOM_html:
        case EMBI_ATOM_tbody:
        case EMBI_ATOM_td:
        case EMBI_ATOM_tfoot:
        case EMBI_ATOM_th:
        case EMBI_ATOM_thead:
        case EMBI_ATOM_tr:
            return;
        default:
            break;
        }
    }
    m_in_body(tb, t);
}

static void m_in_column_group(embi_tb *tb, const tk *t)
{
    switch (t->kind) {
    case TK_CHAR:
        if (t->seg == SEG_WS) {
            insert_text(tb, t->text);
            return;
        }
        break;
    case TK_COMMENT:
        insert_comment(tb, appropriate(tb, nullptr), t);
        return;
    case TK_DOCTYPE:
        return;
    case TK_START:
        if (t->name == EMBI_ATOM_html) {
            m_in_body(tb, t);
            return;
        }
        if (t->name == EMBI_ATOM_col) {
            insert_el(tb, EMBI_NS_HTML, EMBI_ATOM_col, t->raw);
            pop(tb);
            return;
        }
        if (t->name == EMBI_ATOM_template) {
            in_head_rules(tb, t);
            return;
        }
        break;
    case TK_END:
        if (t->name == EMBI_ATOM_colgroup) {
            if (!is_html(cur(tb), EMBI_ATOM_colgroup)) {
                return;
            }
            pop(tb);
            tb->mode = M_IN_TABLE;
            return;
        }
        if (t->name == EMBI_ATOM_col) {
            return;
        }
        if (t->name == EMBI_ATOM_template) {
            in_head_rules(tb, t);
            return;
        }
        break;
    case TK_EOF:
        m_in_body(tb, t);
        return;
    }
    if (!is_html(cur(tb), EMBI_ATOM_colgroup)) {
        return;
    }
    pop(tb);
    tb->mode = M_IN_TABLE;
    process(tb, t);
}

static bool table_body_in_scope(const embi_tb *tb)
{
    const embi_atom names[3] = {EMBI_ATOM_tbody, EMBI_ATOM_thead, EMBI_ATOM_tfoot};
    return names_in_scope(tb, names, 3, SCOPE_TABLE);
}

static void close_table_body(embi_tb *tb)
{
    clear_to(tb, CLEAR_TBODY);
    pop(tb);
    tb->mode = M_IN_TABLE;
}

static void m_in_table_body(embi_tb *tb, const tk *t)
{
    if (t->kind == TK_START) {
        switch (t->name) {
        case EMBI_ATOM_tr:
            clear_to(tb, CLEAR_TBODY);
            insert_el(tb, EMBI_NS_HTML, EMBI_ATOM_tr, t->raw);
            tb->mode = M_IN_ROW;
            return;
        case EMBI_ATOM_th:
        case EMBI_ATOM_td:
            clear_to(tb, CLEAR_TBODY);
            insert_el(tb, EMBI_NS_HTML, EMBI_ATOM_tr, nullptr);
            tb->mode = M_IN_ROW;
            process(tb, t);
            return;
        case EMBI_ATOM_caption:
        case EMBI_ATOM_col:
        case EMBI_ATOM_colgroup:
        case EMBI_ATOM_tbody:
        case EMBI_ATOM_tfoot:
        case EMBI_ATOM_thead:
            if (!table_body_in_scope(tb)) {
                return;
            }
            close_table_body(tb);
            process(tb, t);
            return;
        default:
            break;
        }
    } else if (t->kind == TK_END) {
        switch (t->name) {
        case EMBI_ATOM_tbody:
        case EMBI_ATOM_tfoot:
        case EMBI_ATOM_thead:
            if (!name_in_scope(tb, t->name, SCOPE_TABLE)) {
                return;
            }
            close_table_body(tb);
            return;
        case EMBI_ATOM_table:
            if (!table_body_in_scope(tb)) {
                return;
            }
            close_table_body(tb);
            process(tb, t);
            return;
        case EMBI_ATOM_body:
        case EMBI_ATOM_caption:
        case EMBI_ATOM_col:
        case EMBI_ATOM_colgroup:
        case EMBI_ATOM_html:
        case EMBI_ATOM_td:
        case EMBI_ATOM_th:
        case EMBI_ATOM_tr:
            return;
        default:
            break;
        }
    }
    m_in_table(tb, t);
}

static void close_row(embi_tb *tb)
{
    clear_to(tb, CLEAR_ROW);
    pop(tb);
    tb->mode = M_IN_TABLE_BODY;
}

static void m_in_row(embi_tb *tb, const tk *t)
{
    if (t->kind == TK_START) {
        switch (t->name) {
        case EMBI_ATOM_th:
        case EMBI_ATOM_td:
            clear_to(tb, CLEAR_ROW);
            insert_el(tb, EMBI_NS_HTML, t->name, t->raw);
            tb->mode = M_IN_CELL;
            afe_push_marker(tb);
            return;
        case EMBI_ATOM_caption:
        case EMBI_ATOM_col:
        case EMBI_ATOM_colgroup:
        case EMBI_ATOM_tbody:
        case EMBI_ATOM_tfoot:
        case EMBI_ATOM_thead:
        case EMBI_ATOM_tr:
            if (!name_in_scope(tb, EMBI_ATOM_tr, SCOPE_TABLE)) {
                return;
            }
            close_row(tb);
            process(tb, t);
            return;
        default:
            break;
        }
    } else if (t->kind == TK_END) {
        switch (t->name) {
        case EMBI_ATOM_tr:
            if (!name_in_scope(tb, EMBI_ATOM_tr, SCOPE_TABLE)) {
                return;
            }
            close_row(tb);
            return;
        case EMBI_ATOM_table:
            if (!name_in_scope(tb, EMBI_ATOM_tr, SCOPE_TABLE)) {
                return;
            }
            close_row(tb);
            process(tb, t);
            return;
        case EMBI_ATOM_tbody:
        case EMBI_ATOM_tfoot:
        case EMBI_ATOM_thead:
            if (!name_in_scope(tb, t->name, SCOPE_TABLE)) {
                return;
            }
            if (!name_in_scope(tb, EMBI_ATOM_tr, SCOPE_TABLE)) {
                return;
            }
            close_row(tb);
            process(tb, t);
            return;
        case EMBI_ATOM_body:
        case EMBI_ATOM_caption:
        case EMBI_ATOM_col:
        case EMBI_ATOM_colgroup:
        case EMBI_ATOM_html:
        case EMBI_ATOM_td:
        case EMBI_ATOM_th:
            return;
        default:
            break;
        }
    }
    m_in_table(tb, t);
}

/* Closes the current cell (td or th): generate implied end tags, pop to the cell, clear the
 * formatting list to the marker and return to "in row". */
static void close_cell(embi_tb *tb)
{
    gen_implied(tb, 0, false);
    while (tb->stk.len) {
        embi_element *e = cur(tb);
        pop(tb);
        if (is_html(e, EMBI_ATOM_td) || is_html(e, EMBI_ATOM_th)) {
            break;
        }
    }
    afe_clear(tb);
    tb->mode = M_IN_ROW;
}

static void m_in_cell(embi_tb *tb, const tk *t)
{
    if (t->kind == TK_START) {
        switch (t->name) {
        case EMBI_ATOM_caption:
        case EMBI_ATOM_col:
        case EMBI_ATOM_colgroup:
        case EMBI_ATOM_tbody:
        case EMBI_ATOM_td:
        case EMBI_ATOM_tfoot:
        case EMBI_ATOM_th:
        case EMBI_ATOM_thead:
        case EMBI_ATOM_tr:
            if (!name_in_scope(tb, EMBI_ATOM_td, SCOPE_TABLE) &&
                !name_in_scope(tb, EMBI_ATOM_th, SCOPE_TABLE)) {
                return;
            }
            close_cell(tb);
            process(tb, t);
            return;
        default:
            break;
        }
    } else if (t->kind == TK_END) {
        switch (t->name) {
        case EMBI_ATOM_td:
        case EMBI_ATOM_th:
            if (!name_in_scope(tb, t->name, SCOPE_TABLE)) {
                return;
            }
            gen_implied(tb, 0, false);
            pop_until_html(tb, t->name);
            afe_clear(tb);
            tb->mode = M_IN_ROW;
            return;
        case EMBI_ATOM_body:
        case EMBI_ATOM_caption:
        case EMBI_ATOM_col:
        case EMBI_ATOM_colgroup:
        case EMBI_ATOM_html:
            return;
        case EMBI_ATOM_table:
        case EMBI_ATOM_tbody:
        case EMBI_ATOM_tfoot:
        case EMBI_ATOM_thead:
        case EMBI_ATOM_tr:
            if (!name_in_scope(tb, t->name, SCOPE_TABLE)) {
                return;
            }
            close_cell(tb);
            process(tb, t);
            return;
        default:
            break;
        }
    }
    m_in_body(tb, t);
}

/* Is there a select element on the stack of open elements? Inside the in-select mode the stack
 * is the one of the innermost select, so this is the select scope test minus formatting
 * elements, which the customizable select rules let sit between a select and its options. */
static bool select_on_stack(const embi_tb *tb)
{
    for (uint32_t i = 0; i < tb->stk.len; i++) {
        if (is_html(tb->stk.data[i], EMBI_ATOM_select)) {
            return true;
        }
    }
    return false;
}

/* Is there a select on the stack above stack index idx? */
static bool select_above(const embi_tb *tb, int idx)
{
    for (int j = idx + 1; j < (int)tb->stk.len; j++) {
        if (is_html(tb->stk.data[j], EMBI_ATOM_select)) {
            return true;
        }
    }
    return false;
}

static void close_select(embi_tb *tb)
{
    pop_until_html(tb, EMBI_ATOM_select);
    reset_mode(tb);
}

static void m_in_select(embi_tb *tb, const tk *t)
{
    switch (t->kind) {
    case TK_CHAR:
        if (t->seg == SEG_NUL) {
            return;
        }
        reconstruct(tb);
        insert_text(tb, t->text);
        return;
    case TK_COMMENT:
        insert_comment(tb, appropriate(tb, nullptr), t);
        return;
    case TK_DOCTYPE:
        return;
    case TK_START:
        switch (t->name) {
        case EMBI_ATOM_html:
            m_in_body(tb, t);
            return;
        case EMBI_ATOM_option:
            if (is_html(cur(tb), EMBI_ATOM_option)) {
                pop(tb);
            }
            reconstruct(tb);
            insert_el(tb, EMBI_NS_HTML, EMBI_ATOM_option, t->raw);
            return;
        case EMBI_ATOM_optgroup:
            if (is_html(cur(tb), EMBI_ATOM_option)) {
                pop(tb);
            }
            if (is_html(cur(tb), EMBI_ATOM_optgroup)) {
                pop(tb);
            }
            insert_el(tb, EMBI_NS_HTML, EMBI_ATOM_optgroup, t->raw);
            return;
        case EMBI_ATOM_hr:
            if (is_html(cur(tb), EMBI_ATOM_option)) {
                pop(tb);
            }
            if (is_html(cur(tb), EMBI_ATOM_optgroup)) {
                pop(tb);
            }
            insert_el(tb, EMBI_NS_HTML, EMBI_ATOM_hr, t->raw);
            pop(tb);
            return;
        case EMBI_ATOM_select:
            /* Closes the select even with formatting elements open inside it. Without a select
             * on the stack (fragment case), the token is ignored. */
            if (select_on_stack(tb)) {
                close_select(tb);
            }
            return;
        case EMBI_ATOM_input:
            if (!select_on_stack(tb)) {
                return;
            }
            close_select(tb);
            process(tb, t);
            return;
        case EMBI_ATOM_textarea:
            if (!select_on_stack(tb)) {
                m_in_body(tb, t);
                return;
            }
            close_select(tb);
            process(tb, t);
            return;
        case EMBI_ATOM_script:
        case EMBI_ATOM_template:
            in_head_rules(tb, t);
            return;
        default:
            /* Customizable select: every other start tag is processed with the in body rules
             * (div, button, datalist and formatting elements inside a select are kept). */
            m_in_body(tb, t);
            return;
        }
    case TK_END:
        switch (t->name) {
        case EMBI_ATOM_optgroup:
            if (is_html(cur(tb), EMBI_ATOM_option) && tb->stk.len >= 2 &&
                is_html(tb->stk.data[tb->stk.len - 2], EMBI_ATOM_optgroup)) {
                pop(tb);
            }
            if (is_html(cur(tb), EMBI_ATOM_optgroup)) {
                pop(tb);
            }
            return;
        case EMBI_ATOM_option:
            if (is_html(cur(tb), EMBI_ATOM_option)) {
                pop(tb);
            }
            return;
        case EMBI_ATOM_select:
            if (!name_in_scope(tb, EMBI_ATOM_select, SCOPE_SELECT)) {
                return;
            }
            close_select(tb);
            return;
        case EMBI_ATOM_template:
            in_head_rules(tb, t);
            return;
        case EMBI_ATOM_a:
        case EMBI_ATOM_b:
        case EMBI_ATOM_big:
        case EMBI_ATOM_code:
        case EMBI_ATOM_em:
        case EMBI_ATOM_font:
        case EMBI_ATOM_i:
        case EMBI_ATOM_nobr:
        case EMBI_ATOM_s:
        case EMBI_ATOM_small:
        case EMBI_ATOM_strike:
        case EMBI_ATOM_strong:
        case EMBI_ATOM_tt:
        case EMBI_ATOM_u: {
            /* A formatting element outside the select is not reached from inside it: the
             * select is special, so the adoption agency would move it. The end tag is ignored. */
            embi_element *fe = afe_last_named(tb, t->name);
            if (fe) {
                int fi = stk_find(tb, fe);
                if (fi >= 0 && select_above(tb, fi)) {
                    return;
                }
            }
            if (adoption(tb, t->name)) {
                return;
            }
            any_other_end(tb, t);
            return;
        }
        default:
            /* The generic "any other end tag" steps: an end tag that reaches a select is ignored
             * because the select is special. */
            any_other_end(tb, t);
            return;
        }
    case TK_EOF:
        m_in_body(tb, t);
        return;
    }
}

static void m_in_select_in_table(embi_tb *tb, const tk *t)
{
    if (t->kind == TK_START) {
        switch (t->name) {
        case EMBI_ATOM_caption:
        case EMBI_ATOM_table:
        case EMBI_ATOM_tbody:
        case EMBI_ATOM_tfoot:
        case EMBI_ATOM_thead:
        case EMBI_ATOM_tr:
        case EMBI_ATOM_td:
        case EMBI_ATOM_th:
            pop_until_html(tb, EMBI_ATOM_select);
            reset_mode(tb);
            process(tb, t);
            return;
        default:
            break;
        }
    } else if (t->kind == TK_END) {
        switch (t->name) {
        case EMBI_ATOM_caption:
        case EMBI_ATOM_table:
        case EMBI_ATOM_tbody:
        case EMBI_ATOM_tfoot:
        case EMBI_ATOM_thead:
        case EMBI_ATOM_tr:
        case EMBI_ATOM_td:
        case EMBI_ATOM_th:
            if (!name_in_scope(tb, t->name, SCOPE_TABLE)) {
                return;
            }
            pop_until_html(tb, EMBI_ATOM_select);
            reset_mode(tb);
            process(tb, t);
            return;
        default:
            break;
        }
    }
    m_in_select(tb, t);
}

static void m_in_template(embi_tb *tb, const tk *t)
{
    switch (t->kind) {
    case TK_CHAR: {
        bool saved_fok = tb->frameset_ok;
        m_in_body(tb, t);
        tb->frameset_ok = saved_fok;
        return;
    }
    case TK_COMMENT:
    case TK_DOCTYPE:
        m_in_body(tb, t);
        return;
    case TK_START:
        switch (t->name) {
        case EMBI_ATOM_base:
        case EMBI_ATOM_basefont:
        case EMBI_ATOM_bgsound:
        case EMBI_ATOM_link:
        case EMBI_ATOM_meta:
        case EMBI_ATOM_noframes:
        case EMBI_ATOM_script:
        case EMBI_ATOM_style:
        case EMBI_ATOM_template:
        case EMBI_ATOM_title:
            in_head_rules(tb, t);
            return;
        case EMBI_ATOM_caption:
        case EMBI_ATOM_colgroup:
        case EMBI_ATOM_tbody:
        case EMBI_ATOM_tfoot:
        case EMBI_ATOM_thead:
            set_template_mode(tb, M_IN_TABLE);
            process(tb, t);
            return;
        case EMBI_ATOM_col:
            set_template_mode(tb, M_IN_COLUMN_GROUP);
            process(tb, t);
            return;
        case EMBI_ATOM_tr:
            set_template_mode(tb, M_IN_TABLE_BODY);
            process(tb, t);
            return;
        case EMBI_ATOM_td:
        case EMBI_ATOM_th:
            set_template_mode(tb, M_IN_ROW);
            process(tb, t);
            return;
        default:
            set_template_mode(tb, M_IN_BODY);
            process(tb, t);
            return;
        }
    case TK_END:
        if (t->name == EMBI_ATOM_template) {
            in_head_rules(tb, t);
        }
        return;
    case TK_EOF:
        if (!has_template(tb)) {
            return;
        }
        pop_until_html(tb, EMBI_ATOM_template);
        afe_clear(tb);
        if (tb->tmodes.len) {
            tb->tmodes.len--;
        }
        reset_mode(tb);
        process(tb, t);
        return;
    }
}

static void m_after_body(embi_tb *tb, const tk *t)
{
    switch (t->kind) {
    case TK_CHAR:
        if (t->seg == SEG_WS) {
            m_in_body(tb, t);
            return;
        }
        break;
    case TK_COMMENT:
        insert_comment(tb, (loc){&tb->stk.data[0]->base, nullptr}, t);
        return;
    case TK_DOCTYPE:
        return;
    case TK_START:
        if (t->name == EMBI_ATOM_html) {
            m_in_body(tb, t);
            return;
        }
        break;
    case TK_END:
        if (t->name == EMBI_ATOM_html) {
            if (!tb->fragment) {
                tb->mode = M_AFTER_AFTER_BODY;
            }
            return;
        }
        break;
    case TK_EOF:
        return;
    }
    tb->mode = M_IN_BODY;
    process(tb, t);
}

static void m_in_frameset(embi_tb *tb, const tk *t)
{
    switch (t->kind) {
    case TK_CHAR:
        if (t->seg == SEG_WS) {
            insert_text(tb, t->text);
        }
        return;
    case TK_COMMENT:
        insert_comment(tb, appropriate(tb, nullptr), t);
        return;
    case TK_DOCTYPE:
        return;
    case TK_START:
        switch (t->name) {
        case EMBI_ATOM_html:
            m_in_body(tb, t);
            return;
        case EMBI_ATOM_frameset:
            insert_el(tb, EMBI_NS_HTML, EMBI_ATOM_frameset, t->raw);
            return;
        case EMBI_ATOM_frame:
            insert_el(tb, EMBI_NS_HTML, EMBI_ATOM_frame, t->raw);
            pop(tb);
            return;
        case EMBI_ATOM_noframes:
            in_head_rules(tb, t);
            return;
        default:
            return;
        }
    case TK_END:
        if (t->name == EMBI_ATOM_frameset) {
            if (tb->stk.len == 1) {
                return;
            }
            pop(tb);
            if (!tb->fragment && !is_html(cur(tb), EMBI_ATOM_frameset)) {
                tb->mode = M_AFTER_FRAMESET;
            }
        }
        return;
    case TK_EOF:
        return;
    }
}

static void m_after_frameset(embi_tb *tb, const tk *t)
{
    switch (t->kind) {
    case TK_CHAR:
        if (t->seg == SEG_WS) {
            insert_text(tb, t->text);
        }
        return;
    case TK_COMMENT:
        insert_comment(tb, appropriate(tb, nullptr), t);
        return;
    case TK_DOCTYPE:
        return;
    case TK_START:
        if (t->name == EMBI_ATOM_html) {
            m_in_body(tb, t);
        } else if (t->name == EMBI_ATOM_noframes) {
            in_head_rules(tb, t);
        }
        return;
    case TK_END:
        if (t->name == EMBI_ATOM_html) {
            tb->mode = M_AFTER_AFTER_FRAMESET;
        }
        return;
    case TK_EOF:
        return;
    }
}

static void m_after_after_body(embi_tb *tb, const tk *t)
{
    switch (t->kind) {
    case TK_COMMENT:
        insert_comment(tb, doc_loc(tb), t);
        return;
    case TK_DOCTYPE:
        m_in_body(tb, t);
        return;
    case TK_CHAR:
        if (t->seg == SEG_WS) {
            m_in_body(tb, t);
            return;
        }
        break;
    case TK_START:
        if (t->name == EMBI_ATOM_html) {
            m_in_body(tb, t);
            return;
        }
        break;
    case TK_END:
        break;
    case TK_EOF:
        return;
    }
    tb->mode = M_IN_BODY;
    process(tb, t);
}

static void m_after_after_frameset(embi_tb *tb, const tk *t)
{
    switch (t->kind) {
    case TK_COMMENT:
        insert_comment(tb, doc_loc(tb), t);
        return;
    case TK_DOCTYPE:
        m_in_body(tb, t);
        return;
    case TK_CHAR:
        if (t->seg == SEG_WS) {
            m_in_body(tb, t);
        }
        return;
    case TK_START:
        if (t->name == EMBI_ATOM_html) {
            m_in_body(tb, t);
        } else if (t->name == EMBI_ATOM_noframes) {
            in_head_rules(tb, t);
        }
        return;
    case TK_END:
    case TK_EOF:
        return;
    }
}

static void mode_run(embi_tb *tb, const tk *t)
{
    switch (tb->mode) {
    case M_INITIAL:
        m_initial(tb, t);
        return;
    case M_BEFORE_HTML:
        m_before_html(tb, t);
        return;
    case M_BEFORE_HEAD:
        m_before_head(tb, t);
        return;
    case M_IN_HEAD:
        in_head_rules(tb, t);
        return;
    case M_IN_HEAD_NOSCRIPT:
        m_in_head_noscript(tb, t);
        return;
    case M_AFTER_HEAD:
        m_after_head(tb, t);
        return;
    case M_IN_BODY:
        m_in_body(tb, t);
        return;
    case M_TEXT:
        m_text(tb, t);
        return;
    case M_IN_TABLE:
        m_in_table(tb, t);
        return;
    case M_IN_TABLE_TEXT:
        m_in_table_text(tb, t);
        return;
    case M_IN_CAPTION:
        m_in_caption(tb, t);
        return;
    case M_IN_COLUMN_GROUP:
        m_in_column_group(tb, t);
        return;
    case M_IN_TABLE_BODY:
        m_in_table_body(tb, t);
        return;
    case M_IN_ROW:
        m_in_row(tb, t);
        return;
    case M_IN_CELL:
        m_in_cell(tb, t);
        return;
    case M_IN_SELECT:
        m_in_select(tb, t);
        return;
    case M_IN_SELECT_IN_TABLE:
        m_in_select_in_table(tb, t);
        return;
    case M_IN_TEMPLATE:
        m_in_template(tb, t);
        return;
    case M_AFTER_BODY:
        m_after_body(tb, t);
        return;
    case M_IN_FRAMESET:
        m_in_frameset(tb, t);
        return;
    case M_AFTER_FRAMESET:
        m_after_frameset(tb, t);
        return;
    case M_AFTER_AFTER_BODY:
        m_after_after_body(tb, t);
        return;
    case M_AFTER_AFTER_FRAMESET:
        m_after_after_frameset(tb, t);
        return;
    }
}

/* ------------------------------------------------------------------ selectedcontent */

/* The next node in tree order below root (root itself is excluded), or nullptr. */
static embi_node *next_in_subtree(embi_node *n, const embi_node *root)
{
    if (n->first_child) {
        return n->first_child;
    }
    while (n != root) {
        if (n->next) {
            return n->next;
        }
        n = n->parent;
    }
    return nullptr;
}

/* The customizable select rule: selectedcontent gets copies of the children of the selected
 * option. The selected option is the last option with a selected attribute, or else the first
 * option. This runs once, at the end of input, so the DOM after a parse is right; later option
 * changes do not update it yet. */
static void fill_one_select(embi_node *sel)
{
    embi_element *content = nullptr;
    embi_element *chosen = nullptr;
    embi_element *first = nullptr;
    for (embi_node *n = next_in_subtree(sel, sel); n; n = next_in_subtree(n, sel)) {
        if (n->type != EMBI_NODE_ELEMENT || n->extra != EMBI_NS_HTML) {
            continue;
        }
        embi_element *e = (embi_element *)n;
        if (n->name == EMBI_ATOM_selectedcontent && !content) {
            content = e;
        } else if (n->name == EMBI_ATOM_option) {
            if (!first) {
                first = e;
            }
            if (embi_el_find_attr(e, EMBI_NS_NULL, 0, EMBI_ATOM_selected)) {
                chosen = e;
            }
        }
    }
    if (!content) {
        return;
    }
    if (!chosen) {
        chosen = first;
    }
    while (content->base.first_child) {
        embi_node *old = content->base.first_child;
        embi_detach(old);
        embi_destroy_detached(old);
    }
    if (!chosen) {
        return;
    }
    for (embi_node *c = chosen->base.first_child; c; c = c->next) {
        embi_tree_append_raw(&content->base, embi_clone_node(c, true));
    }
}

static void fill_selectedcontent(embi_node *top)
{
    for (embi_node *n = next_in_subtree(top, top); n; n = next_in_subtree(n, top)) {
        if (n->type == EMBI_NODE_ELEMENT && n->extra == EMBI_NS_HTML && n->name == EMBI_ATOM_select) {
            fill_one_select(n);
        }
    }
}

/* ------------------------------------------------------------------ tokens in */

/* Splits a character token into segments of one class and processes each one. */
static void dispatch_chars(embi_tb *tb, const embi_html_token *raw, embi_str s)
{
    uint32_t i = 0;
    /* Fast path: in "in body" and "text" with an HTML current node, white space and other
     * characters are inserted the same way, so a run is processed as one segment (NULs split it).
     * This avoids one text-node merge per word. */
    if ((tb->mode == M_IN_BODY || tb->mode == M_TEXT) && tb->stk.len != 0 &&
        is_html_ns(adjusted_current(tb))) {
        while (i < s.len) {
            const char *nul = memchr(s.ptr + i, 0, s.len - i);
            uint32_t n = nul ? (uint32_t)(nul - (s.ptr + i)) : s.len - i;
            bool nonws = false;
            for (uint32_t j = 0; j < n; j++) {
                if (!is_ws_byte((unsigned char)s.ptr[i + j])) {
                    nonws = true;
                    break;
                }
            }
            if (n > 0) {
                if (tb->mode == M_IN_BODY) {
                    /* m_in_body for characters, without the dispatch layers. */
                    reconstruct(tb);
                    insert_text(tb, (embi_str){s.ptr + i, n});
                    if (nonws) {
                        tb->frameset_ok = false;
                    }
                } else {
                    tk t = {TK_CHAR, raw, 0, {s.ptr + i, n}, nonws ? SEG_OTHER : SEG_WS};
                    process(tb, &t);
                }
                i += n;
            }
            if (i < s.len) {
                tk t = {TK_CHAR, raw, 0, {s.ptr + i, 1}, SEG_NUL};
                process(tb, &t);
                i++;
            }
        }
        return;
    }
    while (i < s.len) {
        unsigned char c = (unsigned char)s.ptr[i];
        uint32_t n = 1;
        int seg;
        if (c == 0) {
            seg = SEG_NUL;
        } else if (is_ws_byte(c)) {
            seg = SEG_WS;
            while (i + n < s.len && is_ws_byte((unsigned char)s.ptr[i + n])) {
                n++;
            }
        } else {
            seg = SEG_OTHER;
            while (i + n < s.len) {
                unsigned char d = (unsigned char)s.ptr[i + n];
                if (d == 0 || is_ws_byte(d)) {
                    break;
                }
                n++;
            }
        }
        tk t = {TK_CHAR, raw, 0, {s.ptr + i, n}, seg};
        process(tb, &t);
        i += n;
    }
}

bool embi_tb_token(embi_tb *tb, const embi_html_token *raw)
{
    bool skip = tb->skip_lf;
    tb->skip_lf = false;
    if (raw->kind != EMBI_HTML_TOK_CHARACTERS) {
    }
    switch (raw->kind) {
    case EMBI_HTML_TOK_CHARACTERS: {
        embi_str s = raw->data;
        if (skip && s.len > 0 && s.ptr[0] == '\n') {
            s.ptr++;
            s.len--;
        }
        if (s.len > 0) {
            dispatch_chars(tb, raw, s);
        }
        break;
    }
    case EMBI_HTML_TOK_START_TAG: {
        tk t = {TK_START, raw, raw->name_atom ? raw->name_atom : intern(raw->name), {nullptr, 0}, 0};
        if (tb->mode == M_IN_BODY && tb->stk.len != 0 && is_html_ns(adjusted_current(tb))) {
            /* The mode rules apply (HTML current node): skip the dispatch layers. */
            body_start(tb, &t);
        } else {
            process(tb, &t);
        }
        break;
    }
    case EMBI_HTML_TOK_END_TAG: {
        tk t = {TK_END, raw, raw->has_atom ? raw->name_atom : cached_atom(raw->name, false), {nullptr, 0}, 0};
        if (tb->mode == M_IN_BODY && tb->stk.len != 0 && is_html_ns(adjusted_current(tb))) {
            body_end(tb, &t);
        } else {
            process(tb, &t);
        }
        break;
    }
    case EMBI_HTML_TOK_PI:
    case EMBI_HTML_TOK_COMMENT: {
        tk t = {TK_COMMENT, raw, 0, raw->data, 0};
        process(tb, &t);
        break;
    }
    case EMBI_HTML_TOK_DOCTYPE: {
        tk t = {TK_DOCTYPE, raw, 0, {nullptr, 0}, 0};
        process(tb, &t);
        break;
    }
    case EMBI_HTML_TOK_EOF: {
        tk t = {TK_EOF, raw, 0, {nullptr, 0}, 0};
        process(tb, &t);
        if (tb->has_select) {
            fill_selectedcontent(tb->fragment ? &tb->root->base : &tb->doc->base);
        }
        break;
    }
    }
    if (tb->stop) {
        tb->pause = false;
        return false; /* the document is gone: nothing below may read it */
    }
    if (tb->tok) {
        bool foreign_now = tb->stk.len != 0 && !is_html_ns(adjusted_current(tb));
        if (foreign_now != tb->cdata_flag) {
            tb->cdata_flag = foreign_now;
            embi_html_tokenizer_set_cdata_allowed(tb->tok, foreign_now);
        }
    }
    if (raw->kind == EMBI_HTML_TOK_EOF) {
    }
    bool pausing = tb->pause;
    tb->pause = false;
    return !pausing;
}

/* ------------------------------------------------------------------ lifetime */

embi_tb *embi_tb_new(embi_document *doc, embi_element *context, const embi_html_parser_config *cfg)
{
    embi_tb *tb = embi_calloc(1, sizeof *tb);
    tb->doc = doc;
    tb->context = context;
    tb->fragment = context != nullptr;
    tb->scripting = cfg != nullptr && cfg->scripting;
    tb->on_script = (cfg != nullptr && !tb->fragment) ? cfg->on_script : nullptr;
    tb->script_ctx = cfg != nullptr ? cfg->script_ctx : nullptr;
    tb->frameset_ok = true;
    tb->mode = M_INITIAL;
    if (tb->fragment) {
        embi_element *root = make_el(doc, EMBI_NS_HTML, EMBI_ATOM_html, nullptr);
        tb->root = root;
        push(tb, root);
        if (is_html(context, EMBI_ATOM_template)) {
            embi_vec_push(&tb->tmodes, (int)M_IN_TEMPLATE);
        }
        for (embi_node *n = &context->base; n; n = n->parent) {
            if (n->type == EMBI_NODE_ELEMENT && is_html((const embi_element *)n, EMBI_ATOM_form)) {
                tb->form = (embi_element *)n;
                break;
            }
        }
        reset_mode(tb);
    }
    return tb;
}

embi_element *embi_tb_current(const embi_tb *tb)
{
    return cur(tb);
}

void embi_tb_attach(embi_tb *tb, embi_html_tokenizer *tok)
{
    tb->tok = tok;
    embi_html_tokenizer_want_atoms(tok, true);
}

embi_node *embi_tb_take_fragment(embi_tb *tb)
{
    embi_node *frag = embi_new_fragment(tb->doc);
    embi_element *root = tb->root;
    if (!root) {
        return frag;
    }
    while (root->base.first_child) {
        embi_node *child = root->base.first_child;
        embi_detach(child);
        embi_tree_append_raw(frag, child);
    }
    embi_destroy_detached(&root->base);
    tb->root = nullptr;
    return frag;
}

void embi_tb_free(embi_tb *tb)
{
    if (!tb) {
        return;
    }
    embi_vec_free(&tb->tmodes);
    embi_vec_free(&tb->stk);
    embi_vec_free(&tb->afe);
    embi_vec_free(&tb->afe_h);
    embi_sb_free(&tb->pending);
    embi_free(tb, sizeof *tb);
}
