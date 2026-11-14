/*
 * Selectors Level 4 matching (W2b): right-to-left complex matching with an optional :has() anchor,
 * simple selectors and pseudo-classes, the :has() memo, the rule index, and attribute value tests.
 *
 * Element pointers are non-const because the DOM accessors that read classes and the id are
 * non-const. Nothing here mutates the tree.
 */
#include "css/selector.h"

#include <SDL3/SDL_stdinc.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ element helpers */

static inline bool is_html_el(const embi_element *el)
{
    return el->base.extra == EMBI_NS_HTML && el->base.doc->is_html;
}

static inline bool is_html_named(const embi_element *el, embi_atom name)
{
    return el->base.extra == EMBI_NS_HTML && el->base.name == name;
}

static inline bool quirks_on(const embi_element *el)
{
    return el->base.doc->quirks == EMBI_QUIRKS_FULL;
}

static embi_element *parent_el(const embi_element *el)
{
    embi_node *p = el->base.parent;
    return (p != nullptr && p->type == EMBI_NODE_ELEMENT) ? (embi_element *)p : nullptr;
}

static embi_element *prev_el(const embi_element *el)
{
    for (embi_node *n = el->base.prev; n != nullptr; n = n->prev) {
        if (n->type == EMBI_NODE_ELEMENT) {
            return (embi_element *)n;
        }
    }
    return nullptr;
}

static bool has_null_attr(const embi_element *el, embi_atom local)
{
    return embi_el_find_attr(el, EMBI_NS_NULL, 0, local) != nullptr;
}

static const embi_attr *null_attr(const embi_element *el, embi_atom local)
{
    return embi_el_find_attr(el, EMBI_NS_NULL, 0, local);
}

/* An attribute value as UTF-8, in a small buffer or a heap copy. */
typedef struct u8buf {
    char small[256];
    char *heap;
    size_t cap;
    size_t n;
} u8buf;

static const char *u8_load(u8buf *b, embi_dstr d)
{
    b->heap = nullptr;
    b->cap = 0;
    size_t need = embi_dstr_to_utf8(d, b->small, sizeof b->small);
    if (need <= sizeof b->small) {
        b->n = need;
        return b->small;
    }
    b->heap = embi_alloc(need);
    b->cap = need;
    embi_dstr_to_utf8(d, b->heap, need);
    b->n = need;
    return b->heap;
}

static void u8_free(u8buf *b)
{
    if (b->heap != nullptr) {
        embi_free(b->heap, b->cap);
        b->heap = nullptr;
    }
}

/* ------------------------------------------------------------------ byte comparisons */

static inline unsigned char fold(unsigned char c, bool ci)
{
    return (ci && c >= 'A' && c <= 'Z') ? (unsigned char)(c + 32) : c;
}

static bool bytes_eq(const char *a, size_t an, const char *b, size_t bn, bool ci)
{
    if (an != bn) {
        return false;
    }
    for (size_t k = 0; k < an; k++) {
        if (fold((unsigned char)a[k], ci) != fold((unsigned char)b[k], ci)) {
            return false;
        }
    }
    return true;
}

/* Does h have nd at offset off? */
static bool bytes_at(const char *h, size_t hn, size_t off, const char *nd, size_t nn, bool ci)
{
    if (off > hn || nn > hn - off) {
        return false;
    }
    return bytes_eq(h + off, nn, nd, nn, ci);
}

static bool bytes_contains(const char *h, size_t hn, const char *nd, size_t nn, bool ci)
{
    if (nn > hn) {
        return false;
    }
    for (size_t off = 0; off + nn <= hn; off++) {
        if (bytes_at(h, hn, off, nd, nn, ci)) {
            return true;
        }
    }
    return false;
}

static bool is_ascii_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

/* ~= : one whitespace-separated word equals needle. */
static bool bytes_word(const char *h, size_t hn, const char *nd, size_t nn, bool ci)
{
    size_t k = 0;
    while (k < hn) {
        while (k < hn && is_ascii_ws(h[k])) {
            k++;
        }
        size_t s = k;
        while (k < hn && !is_ascii_ws(h[k])) {
            k++;
        }
        if (k > s && bytes_eq(h + s, k - s, nd, nn, ci)) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ attributes */

static bool ns_matches(uint8_t ns, embi_atom ns_uri, uint8_t nsk, embi_atom want)
{
    switch (nsk) {
    case EMBI_NSK_NONE:
        return ns == EMBI_NS_NULL;
    case EMBI_NSK_URI:
        return embi_str_eq(embi_dom_ns_uri(ns, ns_uri), embi_atom_str(want));
    default:
        return true;
    }
}

static const embi_attr *attr_find(const embi_element *el, const embi_sel_simple *s)
{
    embi_atom local = is_html_el(el) ? s->lname : s->name;
    uint32_t cnt = embi_el_attr_count(el);
    for (uint32_t k = 0; k < cnt; k++) {
        const embi_attr *a = embi_el_attr_at(el, k);
        if (a->name != local) {
            continue;
        }
        if (ns_matches(a->ns, a->ns_uri, s->nsk, s->ns_uri)) {
            return a;
        }
    }
    return nullptr;
}

static bool attr_value_matches(const embi_element *el, const embi_attr *a, const embi_sel_simple *s)
{
    if (s->sub == EMBI_AO_EXISTS) {
        return true;
    }
    embi_str need = s->value;
    bool word_bad = s->sub == EMBI_AO_WORD && (need.len == 0);
    if (word_bad) {
        return false;
    }
    if (s->sub == EMBI_AO_WORD) {
        for (uint32_t k = 0; k < need.len; k++) {
            if (is_ascii_ws(need.ptr[k])) {
                return false;
            }
        }
    }
    if ((s->sub == EMBI_AO_PREFIX || s->sub == EMBI_AO_SUFFIX || s->sub == EMBI_AO_SUBSTR) &&
        need.len == 0) {
        return false;
    }

    bool ci;
    if (s->flags & EMBI_SAF_CS) {
        ci = false;
    } else if (s->flags & EMBI_SAF_CI) {
        ci = true;
    } else {
        ci = (s->flags & EMBI_SAF_HTML_CI) && is_html_el(el);
    }

    u8buf b;
    const char *v = u8_load(&b, a->value);
    size_t n = b.n;
    bool ok = false;
    switch (s->sub) {
    case EMBI_AO_EQ:
        ok = bytes_eq(v, n, need.ptr, need.len, ci);
        break;
    case EMBI_AO_WORD:
        ok = bytes_word(v, n, need.ptr, need.len, ci);
        break;
    case EMBI_AO_DASH:
        ok = bytes_eq(v, n, need.ptr, need.len, ci) ||
             (n > need.len && v[need.len] == '-' && bytes_at(v, n, 0, need.ptr, need.len, ci));
        break;
    case EMBI_AO_PREFIX:
        ok = bytes_at(v, n, 0, need.ptr, need.len, ci);
        break;
    case EMBI_AO_SUFFIX:
        ok = need.len <= n && bytes_at(v, n, n - need.len, need.ptr, need.len, ci);
        break;
    case EMBI_AO_SUBSTR:
        ok = bytes_contains(v, n, need.ptr, need.len, ci);
        break;
    default:
        ok = false;
        break;
    }
    u8_free(&b);
    return ok;
}

/* ------------------------------------------------------------------ simple selectors: names */

static bool elem_ns_matches(const embi_element *el, uint8_t nsk, embi_atom want)
{
    switch (nsk) {
    case EMBI_NSK_NONE:
        return el->base.extra == EMBI_NS_NULL;
    case EMBI_NSK_URI:
        return embi_str_eq(embi_el_namespace_uri(el), embi_atom_str(want));
    default:
        return true;
    }
}

static bool type_matches(const embi_element *el, const embi_sel_simple *s)
{
    if (!elem_ns_matches(el, s->nsk, s->ns_uri)) {
        return false;
    }
    if (s->kind == EMBI_SS_UNIVERSAL) {
        return true;
    }
    if (is_html_el(el)) {
        /* The lowercased selector name is compared with the stored name only. */
        return el->base.name == s->lname;
    }
    return el->base.name == s->name;
}

static bool id_matches(const embi_element *el, embi_atom name)
{
    embi_atom id = embi_el_id(el);
    if (id == 0) {
        return false;
    }
    if (id == name) {
        return true;
    }
    return quirks_on(el) && embi_str_ieq(embi_atom_str(id), embi_atom_str(name));
}

static bool class_matches(embi_element *el, embi_atom name)
{
    uint32_t n = 0;
    const embi_atom *cls = embi_el_classes(el, &n);
    for (uint32_t k = 0; k < n; k++) {
        if (cls[k] == name) {
            return true;
        }
        if (quirks_on(el) && embi_str_ieq(embi_atom_str(cls[k]), embi_atom_str(name))) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ nth and structure */

static bool anb_hit(int64_t a, int64_t b, int64_t pos)
{
    if (a == 0) {
        return pos == b;
    }
    int64_t d = pos - b;
    if (d % a != 0) {
        return false;
    }
    return d / a >= 0;
}

static int64_t sib_index(embi_element *el, bool of_type, bool from_end,
                         const embi_sel_list *list, embi_sel_ctx *ctx);

static bool embi_sel_match_list_ctx(const embi_sel_list *l, embi_element *el, embi_sel_ctx *ctx);

static int64_t sib_index(embi_element *el, bool of_type, bool from_end,
                         const embi_sel_list *list, embi_sel_ctx *ctx)
{
    int64_t idx = 1;
    for (embi_node *n = from_end ? el->base.next : el->base.prev; n != nullptr;
         n = from_end ? n->next : n->prev) {
        if (n->type != EMBI_NODE_ELEMENT) {
            continue;
        }
        embi_element *s = (embi_element *)n;
        if (of_type && (s->base.name != el->base.name || s->base.extra != el->base.extra)) {
            continue;
        }
        if (list != nullptr && !embi_sel_match_list_ctx(list, s, ctx)) {
            continue;
        }
        idx++;
    }
    return idx;
}

/* True when an element sibling exists in one direction (of the same type when of_type). Stops at the
 * first one, so first-child and friends cost O(1) on a long list of siblings. */
static bool sib_has_other(const embi_element *el, bool of_type, bool from_end)
{
    for (const embi_node *n = from_end ? el->base.next : el->base.prev; n != nullptr;
         n = from_end ? n->next : n->prev) {
        if (n->type != EMBI_NODE_ELEMENT) {
            continue;
        }
        const embi_element *s = (const embi_element *)n;
        if (of_type && (s->base.name != el->base.name || s->base.extra != el->base.extra)) {
            continue;
        }
        return true;
    }
    return false;
}

static bool nth_matches(const embi_sel_simple *s, embi_element *el, embi_sel_ctx *ctx)
{
    bool of_type = (s->flags & EMBI_SNF_OF_TYPE) != 0;
    bool from_end = (s->flags & EMBI_SNF_FROM_END) != 0;
    if (s->list != nullptr && !embi_sel_match_list_ctx(s->list, el, ctx)) {
        return false;
    }
    return anb_hit(s->a, s->b, sib_index(el, of_type, from_end, s->list, ctx));
}

static bool root_matches(const embi_element *el)
{
    const embi_node *p = el->base.parent;
    return p != nullptr && p->type == EMBI_NODE_DOCUMENT;
}

static bool empty_matches(const embi_element *el)
{
    for (const embi_node *c = el->base.first_child; c != nullptr; c = c->next) {
        if (c->type == EMBI_NODE_ELEMENT) {
            return false;
        }
        if (c->type == EMBI_NODE_TEXT && embi_as_chardata((embi_node *)c)->data.len > 0) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ form and state (fallbacks) */

static bool is_form_named(const embi_element *el, embi_atom name)
{
    return is_html_named(el, name);
}

/* The elements that can be disabled. */
static bool disableable(const embi_element *el)
{
    return is_html_el(el) &&
           (el->base.name == EMBI_ATOM_button || el->base.name == EMBI_ATOM_input ||
            el->base.name == EMBI_ATOM_select || el->base.name == EMBI_ATOM_textarea ||
            el->base.name == EMBI_ATOM_optgroup || el->base.name == EMBI_ATOM_option ||
            el->base.name == EMBI_ATOM_fieldset);
}

static bool attr_is_ci(const embi_element *el, embi_atom local, const char *text)
{
    const embi_attr *a = null_attr(el, local);
    if (a == nullptr) {
        return false;
    }
    return embi_dstr_ieq_utf8(a->value, (embi_str){text, (uint32_t)strlen(text)});
}

/* input[type] is text-like when the type is absent or one of the text types. */
static bool text_like_input(const embi_element *el)
{
    const embi_attr *t = null_attr(el, EMBI_ATOM_type);
    if (t == nullptr) {
        return true;
    }
    /* Types that are not text-like. Anything else, including an unknown value, is text. */
    static const char *const other[] = {"hidden", "checkbox", "radio",  "file",  "submit",
                                        "image",  "reset",    "button", "range", "color"};
    for (size_t k = 0; k < sizeof other / sizeof other[0]; k++) {
        if (embi_dstr_ieq_utf8(t->value, (embi_str){other[k], (uint32_t)strlen(other[k])})) {
            return false;
        }
    }
    return true;
}

/* Is under (a child of f, or f's descendant on the path to the subject) inside f's first legend
 * element child? A disabled fieldset does not disable its first legend's contents. */
static bool in_first_legend(const embi_element *f, const embi_node *under)
{
    for (const embi_node *c = f->base.first_child; c != nullptr; c = c->next) {
        if (c->type == EMBI_NODE_ELEMENT &&
            is_html_named((const embi_element *)c, EMBI_ATOM_legend)) {
            return c == under;
        }
    }
    return false;
}

/* A disabled fieldset ancestor disables the form-associated descendants, except inside its first
 * legend. */
static bool fieldset_disabled(embi_element *el)
{
    const embi_node *under = &el->base;
    for (embi_element *f = parent_el(el); f != nullptr; under = &f->base, f = parent_el(f)) {
        if (is_html_named(f, EMBI_ATOM_fieldset) && has_null_attr(f, EMBI_ATOM_disabled) &&
            !in_first_legend(f, under)) {
            return true;
        }
    }
    return false;
}

/* The disabled state without a hook. Only button, input, select, textarea and fieldset inherit
 * from a disabled fieldset. option takes its own attribute or its optgroup's. optgroup takes its
 * own attribute only. */
static bool disabled_fallback(embi_element *el)
{
    if (has_null_attr(el, EMBI_ATOM_disabled)) {
        return true;
    }
    if (is_html_named(el, EMBI_ATOM_option)) {
        embi_element *p = parent_el(el);
        return p != nullptr && is_html_named(p, EMBI_ATOM_optgroup) &&
               has_null_attr(p, EMBI_ATOM_disabled);
    }
    if (is_html_named(el, EMBI_ATOM_optgroup)) {
        return false;
    }
    return fieldset_disabled(el);
}

/* contenteditable: the nearest ancestor-or-self with a valid value decides. */
static bool editable_by_attr(embi_element *el)
{
    for (embi_element *e = el; e != nullptr; e = parent_el(e)) {
        const embi_attr *ce = null_attr(e, EMBI_ATOM_contenteditable);
        if (ce == nullptr) {
            continue;
        }
        if (embi_dstr_eq_utf8(ce->value, EMBI_STR("")) ||
            embi_dstr_ieq_utf8(ce->value, EMBI_STR("true")) ||
            embi_dstr_ieq_utf8(ce->value, EMBI_STR("plaintext-only"))) {
            return true;
        }
        if (embi_dstr_ieq_utf8(ce->value, EMBI_STR("false"))) {
            return false;
        }
        /* An invalid value inherits. */
    }
    return false;
}

static bool read_write_fallback(embi_element *el)
{
    if (is_html_el(el)) {
        if (is_form_named(el, EMBI_ATOM_textarea)) {
            return !has_null_attr(el, EMBI_ATOM_readonly) && !disabled_fallback(el);
        }
        if (is_form_named(el, EMBI_ATOM_input)) {
            return text_like_input(el) && !has_null_attr(el, EMBI_ATOM_readonly) &&
                   !disabled_fallback(el);
        }
    }
    return editable_by_attr(el);
}

/* A hyphenated name that is not one of the reserved names is a custom element name, which counts
 * as undefined without a registry. */
static bool is_custom_name(embi_str n)
{
    if (n.len == 0 || memchr(n.ptr, '-', n.len) == nullptr) {
        return false;
    }
    static const char *const reserved[] = {
        "annotation-xml", "color-profile",    "font-face",      "font-face-src",
        "font-face-uri",  "font-face-format", "font-face-name", "missing-glyph",
    };
    for (size_t k = 0; k < sizeof reserved / sizeof reserved[0]; k++) {
        if (n.len == strlen(reserved[k]) && memcmp(n.ptr, reserved[k], n.len) == 0) {
            return false;
        }
    }
    return true;
}

static _Atomic(embi_sel_checked_fn) g_checked_fn; /* W8b: the checkedness provider of the form controls */

void embi_sel_set_checked_fn(embi_sel_checked_fn fn)
{
    g_checked_fn = fn;
}

/* The answers without a state hook. */
static bool state_fallback(embi_element *el, embi_sel_pc pc)
{
    switch (pc) {
    case EMBI_PC_OPEN:
        if (is_html_named(el, EMBI_ATOM_details) || is_html_named(el, EMBI_ATOM_dialog)) {
            return has_null_attr(el, EMBI_ATOM_open);
        }
        return false;
    case EMBI_PC_DEFINED:
        if (!is_html_el(el)) {
            return true;
        }
        return !is_custom_name(embi_atom_str(el->base.name));
    case EMBI_PC_ENABLED:
        return disableable(el) && !disabled_fallback(el);
    case EMBI_PC_DISABLED:
        return disableable(el) && disabled_fallback(el);
    case EMBI_PC_CHECKED: {
        const embi_sel_checked_fn fn = g_checked_fn;
        const int live = fn != nullptr ? fn(el) : -1;
        if (live >= 0) {
            return live != 0;
        }
        if (is_html_named(el, EMBI_ATOM_input)) {
            return (attr_is_ci(el, EMBI_ATOM_type, "checkbox") ||
                    attr_is_ci(el, EMBI_ATOM_type, "radio")) &&
                   has_null_attr(el, EMBI_ATOM_checked);
        }
        return is_html_named(el, EMBI_ATOM_option) && has_null_attr(el, EMBI_ATOM_selected);
    }
    case EMBI_PC_INDETERMINATE:
        return is_html_named(el, EMBI_ATOM_progress) && !has_null_attr(el, EMBI_ATOM_value);
    case EMBI_PC_REQUIRED:
        return (is_form_named(el, EMBI_ATOM_input) || is_form_named(el, EMBI_ATOM_select) ||
                is_form_named(el, EMBI_ATOM_textarea)) &&
               has_null_attr(el, EMBI_ATOM_required);
    case EMBI_PC_OPTIONAL:
        return (is_form_named(el, EMBI_ATOM_input) || is_form_named(el, EMBI_ATOM_select) ||
                is_form_named(el, EMBI_ATOM_textarea)) &&
               !has_null_attr(el, EMBI_ATOM_required);
    case EMBI_PC_READ_WRITE:
        return read_write_fallback(el);
    case EMBI_PC_READ_ONLY:
        return !read_write_fallback(el);
    case EMBI_PC_DEFAULT:
        if (is_form_named(el, EMBI_ATOM_input)) {
            return (attr_is_ci(el, EMBI_ATOM_type, "checkbox") ||
                    attr_is_ci(el, EMBI_ATOM_type, "radio")) &&
                   has_null_attr(el, EMBI_ATOM_checked);
        }
        return is_form_named(el, EMBI_ATOM_option) && has_null_attr(el, EMBI_ATOM_selected);
    default:
        /* hover, active, focus, target, modal, popover-open, placeholder-shown, valid, invalid,
         * in-range and out-of-range need state the DOM alone does not carry. */
        return false;
    }
}

static bool is_state_pc(embi_sel_pc pc)
{
    return pc >= EMBI_PC_HOVER && pc <= EMBI_PC_OUT_OF_RANGE;
}

static bool pc_matches(embi_element *el, embi_sel_pc pc, embi_sel_ctx *ctx)
{
    switch (pc) {
    case EMBI_PC_ROOT:
        return root_matches(el);
    case EMBI_PC_EMPTY:
        return empty_matches(el);
    case EMBI_PC_FIRST_CHILD:
        return !sib_has_other(el, false, false);
    case EMBI_PC_LAST_CHILD:
        return !sib_has_other(el, false, true);
    case EMBI_PC_ONLY_CHILD:
        return !sib_has_other(el, false, false) && !sib_has_other(el, false, true);
    case EMBI_PC_FIRST_OF_TYPE:
        return !sib_has_other(el, true, false);
    case EMBI_PC_LAST_OF_TYPE:
        return !sib_has_other(el, true, true);
    case EMBI_PC_ONLY_OF_TYPE:
        return !sib_has_other(el, true, false) && !sib_has_other(el, true, true);
    case EMBI_PC_SCOPE: {
        embi_element *sc = ctx->scope != nullptr ? ctx->scope : embi_document_element(el->base.doc);
        return sc == el;
    }
    case EMBI_PC_LINK:
        return is_html_el(el) &&
               (el->base.name == EMBI_ATOM_a || el->base.name == EMBI_ATOM_area) &&
               has_null_attr(el, EMBI_ATOM_href);
    case EMBI_PC_VISITED:
        return false;
    default:
        break;
    }
    if (is_state_pc(pc)) {
        /* The hook owns only the classes that need live platform state; the attribute-based ones
         * (:defined, :required, :optional, :default, :indeterminate, :checked, :disabled with fieldset/optgroup ...) stay with
         * the matcher. */
        bool hooked = false;
        switch (pc) {
        case EMBI_PC_HOVER:
        case EMBI_PC_ACTIVE:
        case EMBI_PC_FOCUS:
        case EMBI_PC_FOCUS_VISIBLE:
        case EMBI_PC_FOCUS_WITHIN:
        case EMBI_PC_TARGET:
        case EMBI_PC_MODAL:
        case EMBI_PC_POPOVER_OPEN:
        case EMBI_PC_OPEN:
        case EMBI_PC_PLACEHOLDER_SHOWN:
        case EMBI_PC_VALID:
        case EMBI_PC_INVALID:
        case EMBI_PC_IN_RANGE:
        case EMBI_PC_OUT_OF_RANGE:
            hooked = true;
            break;
        default:
            break;
        }
        if (hooked && ctx->hooks != nullptr && ctx->hooks->state != nullptr) {
            return ctx->hooks->state(ctx->hooks->user, el, pc);
        }
        return state_fallback(el, pc);
    }
    return false;
}

/* ------------------------------------------------------------------ :lang() and :dir() */

/* A subtag of a language tag or range: "*" in a range is a wildcard. */
typedef struct lang_sub {
    const char *p;
    size_t n;
} lang_sub;

static size_t lang_split(const char *s, size_t n, lang_sub *out, size_t cap)
{
    size_t cnt = 0;
    size_t start = 0;
    for (size_t k = 0; k <= n && cnt < cap; k++) {
        if (k == n || s[k] == '-') {
            out[cnt].p = s + start;
            out[cnt].n = k - start;
            cnt++;
            start = k + 1;
        }
    }
    return cnt;
}

/* RFC 4647 extended filtering, which Selectors 4 uses for :lang(). The first subtag must match
 * (or be a wildcard); each later range subtag must occur in the language tag, in order. Wildcard
 * subtags match anything. */
static bool lang_range_matches(const char *v, size_t n, embi_str r)
{
    if (r.len == 0 || n == 0) {
        return false;
    }
    enum { MAX_RANGE = 16, MAX_TAG = 32 };
    lang_sub rs[MAX_RANGE + 1];
    lang_sub ls[MAX_TAG + 1];
    size_t rn = lang_split(r.ptr, r.len, rs, MAX_RANGE + 1);
    size_t ln = lang_split(v, n, ls, MAX_TAG + 1);
    if (rn > MAX_RANGE || ln > MAX_TAG) {
        return false; /* too long to compare: no match rather than a truncated one */
    }
    bool first_wild = rs[0].n == 1 && rs[0].p[0] == '*';
    if (!first_wild && !bytes_eq(rs[0].p, rs[0].n, ls[0].p, ls[0].n, true)) {
        return false;
    }
    size_t lp = 1;
    for (size_t i = 1; i < rn; i++) {
        if (rs[i].n == 1 && rs[i].p[0] == '*') {
            continue;
        }
        while (lp < ln && !bytes_eq(ls[lp].p, ls[lp].n, rs[i].p, rs[i].n, true)) {
            if (ls[lp].n == 1) {
                return false; /* RFC 4647 2.3.2: a singleton subtag ends the search */
            }
            lp++;
        }
        if (lp >= ln) {
            return false;
        }
        lp++;
    }
    return true;
}

static bool lang_matches(embi_element *el, const embi_sel_simple *s)
{
    for (embi_element *e = el; e != nullptr; e = parent_el(e)) {
        const embi_attr *a = null_attr(e, EMBI_ATOM_lang);
        if (a == nullptr) {
            /* XHTML documents carry the language in xml:lang (a name with a colon in the HTML parser's output). */
            a = embi_el_get_attribute(e, (embi_str){"xml:lang", 8});
        }
        if (a == nullptr) {
            continue;
        }
        u8buf b;
        const char *v = u8_load(&b, a->value);
        bool any = false;
        for (uint32_t k = 0; k < s->nstrs && !any; k++) {
            any = lang_range_matches(v, b.n, s->strs[k]);
        }
        u8_free(&b);
        return any;
    }
    return false;
}

typedef struct cp_range {
    uint32_t lo, hi;
} cp_range;

static bool in_ranges(uint32_t cp, const cp_range *r, size_t n)
{
    for (size_t k = 0; k < n; k++) {
        if (cp >= r[k].lo && cp <= r[k].hi) {
            return true;
        }
    }
    return false;
}

/* Strong right-to-left characters (bidi class R and AL). Combining marks (NSM), Arabic-Indic digits
 * (AN, EN), and Arabic and Hebrew punctuation of the neutral classes are not listed. */
static const cp_range k_rtl_ranges[] = {
    {0x05BE, 0x05BE},   {0x05C0, 0x05C0},   {0x05C3, 0x05C3},   {0x05C6, 0x05C6},
    {0x05D0, 0x05F4},                                          /* Hebrew letters and ligatures */
    {0x0608, 0x0608},   {0x060B, 0x060B},   {0x060D, 0x060D},   {0x061B, 0x064A},
    {0x066D, 0x066F},   {0x0671, 0x06D5},   {0x06E5, 0x06E6},   {0x06EE, 0x06EF},
    {0x06FA, 0x070D},   {0x070F, 0x0710},   {0x0712, 0x072F},   {0x074D, 0x07A5},
    {0x07B1, 0x07B1},                                          /* Arabic, Syriac, Thaana */
    {0x07C0, 0x07EA},   {0x07F4, 0x07F5},   {0x07FA, 0x07FA},   {0x07FE, 0x0815}, /* NKo, Samaritan */
    {0x081A, 0x081A},   {0x0824, 0x0824},   {0x0828, 0x0828},   {0x0830, 0x083E},
    {0x0840, 0x0858},   {0x085E, 0x085E},   {0x0860, 0x086A},   {0x0870, 0x088E}, /* Mandaic... */
    {0x08A0, 0x08C9},                                          /* Arabic extended */
    {0x200F, 0x200F},                                          /* RIGHT-TO-LEFT MARK */
    {0xFB1D, 0xFB1D},   {0xFB1F, 0xFB28},   {0xFB2A, 0xFD3D},   {0xFD50, 0xFDCF},
    {0xFDF0, 0xFDFC},   {0xFE70, 0xFEFC},                      /* presentation forms */
    {0x10800, 0x10FFF},                                        /* RTL supplementary scripts */
    {0x1E800, 0x1EFFF},
};

/* Characters without a strong direction: ASCII and Latin-1 punctuation, digits, symbols, combining
 * diacritics, general punctuation and symbol blocks, CJK punctuation, variation selectors, emoji and
 * tags. Every other code point is treated as strong left-to-right, which covers each LTR script
 * (Latin, Greek, Cyrillic, Indic, Thai, Ethiopic, CJK, and so on) without listing them. */
static const cp_range k_neutral_ranges[] = {
    {0x0000, 0x0040},   {0x005B, 0x0060},   {0x007B, 0x00A9},   {0x00AB, 0x00B4},
    {0x00B6, 0x00B9},   {0x00BB, 0x00BF},   {0x00D7, 0x00D7},   {0x00F7, 0x00F7},
    {0x02B9, 0x02BF},   {0x02C2, 0x02CF},   {0x02D2, 0x02DF},   {0x02E5, 0x02FF},
    {0x0300, 0x036F},                                          /* combining diacritics */
    {0x0374, 0x0375},   {0x037E, 0x037E},   {0x0384, 0x0385},   {0x0387, 0x0387},
    {0x0483, 0x0489},                                          /* Cyrillic combining */
    {0x0600, 0x0607},   {0x0609, 0x060A},   {0x060C, 0x060C},   {0x060E, 0x061A},
    {0x064B, 0x066C},   {0x0670, 0x0670},   {0x06D6, 0x06E4},   {0x06E7, 0x06ED},
    {0x06F0, 0x06F9},   {0x0711, 0x0711},   {0x0730, 0x074A},   {0x07A6, 0x07B0},
    {0x0591, 0x05BD},   {0x05BF, 0x05BF},   {0x05C1, 0x05C2},   {0x05C4, 0x05C5},
    {0x05C7, 0x05C7},                                          /* Hebrew points */
    {0x07EB, 0x07F3},   {0x07FD, 0x07FD},   {0x0816, 0x0819},   {0x081B, 0x0823},
    {0x0825, 0x0827},   {0x0829, 0x082D},   {0x0859, 0x085B},   {0x0897, 0x089F},
    {0x08CA, 0x08E1},   {0x08E3, 0x08FF},                      /* NKo, Samaritan, Mandaic, Arabic NSM */
    {0x2000, 0x200E},   {0x2010, 0x2070},   {0x2074, 0x207E},   {0x2080, 0x208E},
    {0x20A0, 0x20FF},   {0x2100, 0x2101},   {0x2190, 0x2BFF},   /* punctuation, symbols, arrows */
    {0x2E00, 0x2E7F},   {0x3000, 0x3004},   {0x3008, 0x3020},   {0x3030, 0x3030},
    {0x303D, 0x303F},   {0x30FB, 0x30FB},
    {0xD800, 0xF8FF},                                          /* surrogates and private use */
    {0xFB1E, 0xFB1E},   {0xFB29, 0xFB29},   {0xFD3E, 0xFD3F},   {0xFDFD, 0xFDFF},
    {0xFE00, 0xFE6F},                                          /* variation selectors, forms */
    {0xFEFF, 0xFF20},   {0xFF3B, 0xFF40},   {0xFF5B, 0xFF65},   {0xFFE0, 0xFFFF},
    {0x1D000, 0x1D24F}, {0x1F000, 0x1FAFF}, {0xE0000, 0xE0FFF},
};

static bool is_rtl_cp(uint32_t cp)
{
    return in_ranges(cp, k_rtl_ranges, sizeof k_rtl_ranges / sizeof k_rtl_ranges[0]);
}

static bool is_ltr_cp(uint32_t cp)
{
    if (cp == 0x200E) {
        return true; /* LEFT-TO-RIGHT MARK */
    }
    return !in_ranges(cp, k_neutral_ranges, sizeof k_neutral_ranges / sizeof k_neutral_ranges[0]);
}

/* The first strong character of a text node: LTR, RTL, or 0 when there is none. */
static int strong_dir(embi_dstr d)
{
    for (uint32_t i = 0; i < d.len; i++) {
        uint32_t cp = embi_dstr_unit(d, i);
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < d.len) {
            uint32_t lo = embi_dstr_unit(d, i + 1);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            }
        }
        if (is_rtl_cp(cp)) {
            return EMBI_DIR_RTL;
        }
        if (is_ltr_cp(cp)) {
            return EMBI_DIR_LTR;
        }
    }
    return 0;
}

/* The dir attribute in a defined state: ltr, rtl or auto (0 when absent or invalid). */
static int dir_attr_state(const embi_element *e)
{
    const embi_attr *a = null_attr(e, EMBI_ATOM_dir);
    if (a == nullptr) {
        return 0;
    }
    if (embi_dstr_ieq_utf8(a->value, EMBI_STR("ltr"))) {
        return EMBI_DIR_LTR;
    }
    if (embi_dstr_ieq_utf8(a->value, EMBI_STR("rtl"))) {
        return EMBI_DIR_RTL;
    }
    if (embi_dstr_ieq_utf8(a->value, EMBI_STR("auto"))) {
        return 3;
    }
    return 0;
}

/* dir=auto: the first strong character in the text of e, skipping nested bdi, script, style,
 * textarea and elements with their own valid dir. */
static int auto_dir(embi_element *e)
{
    embi_node *root = &e->base;
    embi_node *n = embi_next_in_tree(root, root);
    while (n != nullptr) {
        if (n->type == EMBI_NODE_ELEMENT) {
            embi_element *c = (embi_element *)n;
            if (is_html_named(c, EMBI_ATOM_bdi) || is_html_named(c, EMBI_ATOM_script) ||
                is_html_named(c, EMBI_ATOM_style) || is_html_named(c, EMBI_ATOM_textarea) ||
                dir_attr_state(c) != 0) {
                n = embi_next_skipping_children(root, n);
                continue;
            }
        } else if (n->type == EMBI_NODE_TEXT) {
            int d = strong_dir(embi_as_chardata(n)->data);
            if (d != 0) {
                return d;
            }
        }
        n = embi_next_in_tree(root, n);
    }
    return EMBI_DIR_LTR;
}

static int dir_of(embi_element *el)
{
    for (embi_element *e = el; e != nullptr; e = parent_el(e)) {
        int st = dir_attr_state(e);
        if (st == EMBI_DIR_LTR || st == EMBI_DIR_RTL) {
            return st;
        }
        if (st == 3 || is_html_named(e, EMBI_ATOM_bdi)) {
            return auto_dir(e); /* a bdi with a missing or invalid dir is auto */
        }
    }
    return EMBI_DIR_LTR;
}

/* ------------------------------------------------------------------ :has() memo */

typedef struct embi_sel_memo_node {
    const embi_sel_list *list;
    struct embi_sel_memo_node *next;
    bool result;
} embi_sel_memo_node;

static embi_sel_memo_node *memo_find(const embi_sel_ctx *ctx, const embi_element *e,
                                     const embi_sel_list *l)
{
    uint64_t v = 0;
    if (!embi_hashmap_get(&ctx->has_memo, (uint64_t)(uintptr_t)e, &v)) {
        return nullptr;
    }
    for (embi_sel_memo_node *m = (embi_sel_memo_node *)(uintptr_t)v; m != nullptr; m = m->next) {
        if (m->list == l) {
            return m;
        }
    }
    return nullptr;
}

static void memo_put(embi_sel_ctx *ctx, const embi_element *e, const embi_sel_list *l, bool res)
{
    uint64_t key = (uint64_t)(uintptr_t)e;
    uint64_t v = 0;
    embi_sel_memo_node *m = embi_alloc(sizeof *m);
    m->list = l;
    m->result = res;
    m->next = nullptr;
    if (embi_hashmap_get(&ctx->has_memo, key, &v)) {
        m->next = (embi_sel_memo_node *)(uintptr_t)v;
    }
    embi_hashmap_put(&ctx->has_memo, key, (uint64_t)(uintptr_t)m);
}

/* ------------------------------------------------------------------ complex matching */

static bool has_matches(const embi_sel_list *l, embi_element *e, embi_sel_ctx *ctx);

static bool compound_matches(const embi_sel_compound *c, embi_element *el, embi_sel_ctx *ctx)
{
    for (uint32_t k = 0; k < c->n; k++) {
        const embi_sel_simple *s = &c->ss[k];
        bool ok;
        switch (s->kind) {
        case EMBI_SS_TYPE:
        case EMBI_SS_UNIVERSAL:
            ok = type_matches(el, s);
            break;
        case EMBI_SS_ID:
            ok = id_matches(el, s->name);
            break;
        case EMBI_SS_CLASS:
            ok = class_matches(el, s->name);
            break;
        case EMBI_SS_ATTR: {
            const embi_attr *a = attr_find(el, s);
            ok = a != nullptr && attr_value_matches(el, a, s);
            break;
        }
        case EMBI_SS_PC:
            ok = pc_matches(el, (embi_sel_pc)s->sub, ctx);
            break;
        case EMBI_SS_NTH:
            ok = nth_matches(s, el, ctx);
            break;
        case EMBI_SS_IS:
        case EMBI_SS_WHERE:
            ok = embi_sel_match_list_ctx(s->list, el, ctx);
            break;
        case EMBI_SS_NOT:
            ok = !embi_sel_match_list_ctx(s->list, el, ctx);
            break;
        case EMBI_SS_HAS:
            ok = has_matches(s->list, el, ctx);
            break;
        case EMBI_SS_LANG:
            ok = lang_matches(el, s);
            break;
        case EMBI_SS_DIR:
            ok = dir_of(el) == (int)s->sub;
            break;
        default:
            ok = false;
            break;
        }
        if (!ok) {
            return false;
        }
    }
    return true;
}

/* Does the relation of a leading combinator hold between the anchor and el? */
static bool anchor_relation(uint8_t comb, const embi_element *anchor, embi_element *el)
{
    switch (comb) {
    case EMBI_SC_DESC:
        for (embi_element *p = parent_el(el); p != nullptr; p = parent_el(p)) {
            if (p == anchor) {
                return true;
            }
        }
        return false;
    case EMBI_SC_CHILD:
        return parent_el(el) == anchor;
    case EMBI_SC_NEXT:
        return prev_el(el) == anchor;
    case EMBI_SC_SUBSEQ:
        for (embi_element *s = prev_el(el); s != nullptr; s = prev_el(s)) {
            if (s == anchor) {
                return true;
            }
        }
        return false;
    default:
        return false;
    }
}

/* Per-call state of one complex match. After a number of steps, failures of (compound, element)
 * pairs are remembered, so chains of descendant or sibling combinators that cannot take the greedy
 * shortcut stay polynomial. */
typedef struct mstate {
    uint32_t steps;
    embi_hashmap fail; /* element -> bitmask of compound indexes (below 64) that failed */
} mstate;

#define MS_MEMO_AFTER 32u

static bool ms_failed(const mstate *ms, uint32_t i, const embi_element *el)
{
    uint64_t v = 0;
    return i < 64 && embi_hashmap_get(&ms->fail, (uint64_t)(uintptr_t)el, &v) &&
           ((v >> i) & 1u) != 0;
}

static void ms_record(mstate *ms, uint32_t i, const embi_element *el)
{
    if (i >= 64) {
        return;
    }
    uint64_t v = 0;
    uint64_t key = (uint64_t)(uintptr_t)el;
    (void)embi_hashmap_get(&ms->fail, key, &v);
    embi_hashmap_put(&ms->fail, key, v | (1ull << i));
}

/* May the search for compound i-1 stop at the nearest element that matches it? That holds for a run
 * of the same transitive combinator (descendant or subsequent-sibling) down to the start, because
 * the nearest match leaves a superset of candidates for everything to its left. */
static bool greedy_ok(const embi_sel_complex *c, uint32_t i, const embi_element *anchor)
{
    uint8_t k = c->comb[i];
    if (k != EMBI_SC_DESC && k != EMBI_SC_SUBSEQ) {
        return false;
    }
    for (uint32_t j = 1; j < i; j++) {
        if (c->comb[j] != k) {
            return false;
        }
    }
    return anchor == nullptr || c->comb[0] == k;
}

static bool match_rest(const embi_sel_complex *c, uint32_t i, embi_element *el, embi_sel_ctx *ctx,
                       const embi_element *anchor, mstate *ms);

/* Compound i-1 against cand, then everything to its left. */
static bool match_prev(const embi_sel_complex *c, uint32_t i, embi_element *cand, embi_sel_ctx *ctx,
                       const embi_element *anchor, mstate *ms)
{
    return compound_matches(&c->comp[i - 1], cand, ctx) &&
           match_rest(c, i - 1, cand, ctx, anchor, ms);
}

/* Compound i has matched el. Matches the combinators and compounds to its left. With an anchor
 * (relative selectors), compound 0 must relate to the anchor. */
static bool match_rest(const embi_sel_complex *c, uint32_t i, embi_element *el, embi_sel_ctx *ctx,
                       const embi_element *anchor, mstate *ms)
{
    if (i == 0) {
        return anchor == nullptr || anchor_relation(c->comb[0], anchor, el);
    }
    bool memo = ++ms->steps > MS_MEMO_AFTER;
    if (memo && ms_failed(ms, i, el)) {
        return false;
    }
    bool ok = false;
    switch (c->comb[i]) {
    case EMBI_SC_CHILD: {
        embi_element *p = parent_el(el);
        ok = p != nullptr && match_prev(c, i, p, ctx, anchor, ms);
        break;
    }
    case EMBI_SC_DESC: {
        bool greedy = greedy_ok(c, i, anchor);
        for (embi_element *p = parent_el(el); p != nullptr && !ok; p = parent_el(p)) {
            if (greedy) {
                if (compound_matches(&c->comp[i - 1], p, ctx)) {
                    ok = match_rest(c, i - 1, p, ctx, anchor, ms);
                    break;
                }
            } else {
                ok = match_prev(c, i, p, ctx, anchor, ms);
            }
        }
        break;
    }
    case EMBI_SC_NEXT: {
        embi_element *s = prev_el(el);
        ok = s != nullptr && match_prev(c, i, s, ctx, anchor, ms);
        break;
    }
    case EMBI_SC_SUBSEQ: {
        bool greedy = greedy_ok(c, i, anchor);
        for (embi_element *s = prev_el(el); s != nullptr && !ok; s = prev_el(s)) {
            if (greedy) {
                if (compound_matches(&c->comp[i - 1], s, ctx)) {
                    ok = match_rest(c, i - 1, s, ctx, anchor, ms);
                    break;
                }
            } else {
                ok = match_prev(c, i, s, ctx, anchor, ms);
            }
        }
        break;
    }
    default:
        break;
    }
    if (!ok && memo) {
        ms_record(ms, i, el);
    }
    return ok;
}

/* Matches complex selector c with el as its subject. */
static bool match_complex(const embi_sel_complex *c, embi_element *el, embi_sel_ctx *ctx,
                          const embi_element *anchor)
{
    if (!compound_matches(&c->comp[c->nc - 1], el, ctx)) {
        return false;
    }
    if (c->nc == 1 && anchor == nullptr) {
        return true;
    }
    mstate ms = {0, {0}};
    bool r = match_rest(c, c->nc - 1, el, ctx, anchor, &ms);
    embi_hashmap_free(&ms.fail);
    return r;
}

static bool embi_sel_match_list_ctx(const embi_sel_list *l, embi_element *el, embi_sel_ctx *ctx)
{
    if (l == nullptr) {
        return false;
    }
    for (uint32_t k = 0; k < l->n; k++) {
        const embi_sel_complex *c = &l->items[k];
        if (c->pseudo != EMBI_PE_NONE) {
            continue;
        }
        if (match_complex(c, el, ctx, nullptr)) {
            return true;
        }
    }
    return false;
}

bool embi_sel_match_list(const embi_sel_list *l, embi_element *el, embi_sel_ctx *ctx)
{
    return embi_sel_match_list_ctx(l, el, ctx);
}

/* :has(): some element related to e by one of the relative items. */
static bool has_matches(const embi_sel_list *l, embi_element *e, embi_sel_ctx *ctx)
{
    if (l == nullptr) {
        return false;
    }
    embi_sel_memo_node *memo = memo_find(ctx, e, l);
    if (memo != nullptr) {
        return memo->result;
    }
    bool res = false;
    for (uint32_t k = 0; k < l->n && !res; k++) {
        const embi_sel_complex *c = &l->items[k];
        uint8_t lead = c->comb[0];
        if (lead == EMBI_SC_DESC || lead == EMBI_SC_CHILD) {
            /* Every subject is a descendant of e. */
            for (embi_node *n = embi_next_in_tree(&e->base, &e->base); n != nullptr && !res;
                 n = embi_next_in_tree(&e->base, n)) {
                if (n->type == EMBI_NODE_ELEMENT &&
                    match_complex(c, (embi_element *)n, ctx, e)) {
                    res = true;
                }
            }
        } else {
            /* Subjects are following siblings of e, or their descendants. */
            for (embi_node *s = e->base.next; s != nullptr && !res; s = s->next) {
                if (s->type != EMBI_NODE_ELEMENT) {
                    continue;
                }
                for (embi_node *n = s; n != nullptr && !res; n = embi_next_in_tree(s, n)) {
                    if (n->type == EMBI_NODE_ELEMENT &&
                        match_complex(c, (embi_element *)n, ctx, e)) {
                        res = true;
                    }
                }
            }
        }
    }
    memo_put(ctx, e, l, res);
    return res;
}

/* ------------------------------------------------------------------ top level */

static bool bloom_admits(const embi_bloom *b, const embi_sel_complex *c)
{
    for (uint32_t k = 0; k < c->nreq; k++) {
        if (!embi_bloom_test(b, c->req[k])) {
            return false;
        }
    }
    return true;
}

bool embi_sel_match(const embi_sel_complex *c, embi_element *el, embi_sel_pseudo pe,
                    embi_sel_ctx *ctx, const embi_bloom *anc)
{
    if (c->pseudo != (uint8_t)pe) {
        return false;
    }
    if (anc != nullptr && !quirks_on(el) && !bloom_admits(anc, c)) {
        return false;
    }
    return match_complex(c, el, ctx, nullptr);
}

void embi_sel_ctx_init(embi_sel_ctx *ctx, embi_element *scope, const embi_sel_hooks *hooks)
{
    embi_sel_lower_cache_reset();
    ctx->scope = scope;
    ctx->hooks = hooks;
    ctx->has_memo = (embi_hashmap){0};
}

void embi_sel_ctx_reset(embi_sel_ctx *ctx)
{
    embi_hashmap_iter it = embi_hashmap_iter_begin(&ctx->has_memo);
    uint64_t key = 0;
    uint64_t val = 0;
    while (embi_hashmap_iter_next(&it, &key, &val)) {
        embi_sel_memo_node *m = (embi_sel_memo_node *)(uintptr_t)val;
        while (m != nullptr) {
            embi_sel_memo_node *next = m->next;
            embi_free(m, sizeof *m);
            m = next;
        }
    }
    embi_hashmap_clear(&ctx->has_memo);
}

void embi_sel_ctx_free(embi_sel_ctx *ctx)
{
    embi_sel_ctx_reset(ctx);
    embi_hashmap_free(&ctx->has_memo);
}

/* ------------------------------------------------------------------ rule index */

static void bucket_push(embi_hashmap *m, uint64_t key, uint64_t payload)
{
    uint64_t v = 0;
    embi_sel_bucket *b = nullptr;
    if (embi_hashmap_get(m, key, &v)) {
        b = (embi_sel_bucket *)(uintptr_t)v;
    } else {
        b = embi_calloc(1, sizeof *b);
        embi_hashmap_put(m, key, (uint64_t)(uintptr_t)b);
    }
    if (b->len == b->cap) {
        uint32_t ncap = b->cap == 0 ? 4 : b->cap * 2;
        b->items = embi_realloc(b->items, (size_t)b->cap * sizeof *b->items,
                                (size_t)ncap * sizeof *b->items);
        b->cap = ncap;
    }
    b->items[b->len++] = payload;
}

static void bucket_visit(const embi_hashmap *m, uint64_t key, embi_sel_visit_fn fn, void *user)
{
    uint64_t v = 0;
    if (!embi_hashmap_get(m, key, &v)) {
        return;
    }
    const embi_sel_bucket *b = (const embi_sel_bucket *)(uintptr_t)v;
    for (uint32_t k = 0; k < b->len; k++) {
        fn(user, b->items[k]);
    }
}

static void bucket_free_all(embi_hashmap *m)
{
    embi_hashmap_iter it = embi_hashmap_iter_begin(m);
    uint64_t key = 0;
    uint64_t val = 0;
    while (embi_hashmap_iter_next(&it, &key, &val)) {
        embi_sel_bucket *b = (embi_sel_bucket *)(uintptr_t)val;
        if (b->items != nullptr) {
            embi_free(b->items, (size_t)b->cap * sizeof *b->items);
        }
        embi_free(b, sizeof *b);
    }
    embi_hashmap_free(m);
}

void embi_sel_index_add(embi_sel_index *ix, const embi_sel_complex *c, uint64_t payload)
{
    const embi_sel_compound *sub = &c->comp[c->nc - 1];
    const embi_sel_simple *key = nullptr;
    static const uint8_t order[] = {EMBI_SS_ID, EMBI_SS_CLASS, EMBI_SS_TYPE, EMBI_SS_ATTR};
    for (size_t o = 0; o < sizeof order && key == nullptr; o++) {
        for (uint32_t k = 0; k < sub->n; k++) {
            if (sub->ss[k].kind == order[o]) {
                key = &sub->ss[k];
                break;
            }
        }
    }
    if (key == nullptr) {
        if (ix->universal.len == ix->universal.cap) {
            uint32_t ncap = ix->universal.cap == 0 ? 4 : ix->universal.cap * 2;
            ix->universal.items = embi_realloc(ix->universal.items,
                                               (size_t)ix->universal.cap * sizeof(uint64_t),
                                               (size_t)ncap * sizeof(uint64_t));
            ix->universal.cap = ncap;
        }
        ix->universal.items[ix->universal.len++] = payload;
        return;
    }
    switch (key->kind) {
    case EMBI_SS_ID:
        bucket_push(&ix->ids, embi_sel_lower_atom(key->name), payload);
        break;
    case EMBI_SS_CLASS:
        bucket_push(&ix->classes, embi_sel_lower_atom(key->name), payload);
        break;
    case EMBI_SS_TYPE:
        bucket_push(&ix->tags, key->lname, payload);
        break;
    default:
        bucket_push(&ix->attrs, key->lname, payload);
        break;
    }
}

/* Visits the bucket of each distinct lowercase name once. v holds raw names on entry and is
 * overwritten with their lowercase forms. Buckets are keyed by lowercase atoms, so case variants
 * ("Foo" and "foo") must count as one name. */
static int atom_cmp(const void *x, const void *y)
{
    embi_atom a = *(const embi_atom *)x;
    embi_atom b = *(const embi_atom *)y;
    return a < b ? -1 : (a > b ? 1 : 0);
}

static void visit_names(const embi_hashmap *m, embi_atom *v, uint32_t n, embi_sel_visit_fn fn,
                        void *user)
{
    for (uint32_t k = 0; k < n; k++) {
        v[k] = embi_sel_lower_atom(v[k]);
    }
    if (n > 8) {
        SDL_qsort(v, n, sizeof *v, atom_cmp);
        for (uint32_t k = 0; k < n; k++) {
            if (k == 0 || v[k] != v[k - 1]) {
                bucket_visit(m, v[k], fn, user);
            }
        }
        return;
    }
    for (uint32_t k = 0; k < n; k++) {
        bool dup = false;
        for (uint32_t j = 0; j < k && !dup; j++) {
            dup = v[j] == v[k];
        }
        if (!dup) {
            bucket_visit(m, v[k], fn, user);
        }
    }
}

void embi_sel_index_candidates(const embi_sel_index *ix, embi_element *el, embi_sel_visit_fn fn,
                               void *user)
{
    if (embi_el_id(el) != 0) {
        bucket_visit(&ix->ids, embi_sel_lower_atom(embi_el_id(el)), fn, user);
    }

    enum { SMALL = 32 };
    embi_atom small[SMALL];

    uint32_t n = 0;
    const embi_atom *cls = embi_el_classes(el, &n);
    embi_atom *buf = n <= SMALL ? small : embi_alloc((size_t)n * sizeof *buf);
    for (uint32_t k = 0; k < n; k++) {
        buf[k] = cls[k];
    }
    visit_names(&ix->classes, buf, n, fn, user);
    if (buf != small) {
        embi_free(buf, (size_t)n * sizeof *buf);
    }

    bucket_visit(&ix->tags, embi_sel_lower_atom(el->base.name), fn, user);

    uint32_t na = embi_el_attr_count(el);
    buf = na <= SMALL ? small : embi_alloc((size_t)na * sizeof *buf);
    for (uint32_t k = 0; k < na; k++) {
        buf[k] = embi_el_attr_at(el, k)->name;
    }
    visit_names(&ix->attrs, buf, na, fn, user);
    if (buf != small) {
        embi_free(buf, (size_t)na * sizeof *buf);
    }

    for (uint32_t k = 0; k < ix->universal.len; k++) {
        fn(user, ix->universal.items[k]);
    }
}

void embi_sel_index_free(embi_sel_index *ix)
{
    bucket_free_all(&ix->ids);
    bucket_free_all(&ix->classes);
    bucket_free_all(&ix->tags);
    bucket_free_all(&ix->attrs);
    if (ix->universal.items != nullptr) {
        embi_free(ix->universal.items, (size_t)ix->universal.cap * sizeof(uint64_t));
    }
    ix->universal = (embi_sel_bucket){nullptr, 0, 0};
}
