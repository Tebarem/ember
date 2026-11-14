/*
 * Focusable areas and sequential focus navigation (W5d). The order is the HTML rule: elements
 * with a positive tabindex first, ascending, then the others in tree order. Elements with a negative tabindex are
 * focusable by script but skipped by Tab.
 */
#include "platform/platform.h"

#include "base/base.h"

#include <SDL3/SDL_stdinc.h>

typedef struct entry {
    int32_t ti;
    embi_element *el;
    uint64_t seq; /* tree-order number */
} entry;

typedef EMBI_VEC(entry) entry_vec;

static bool is_html(const embi_element *el)
{
    return el->base.extra == EMBI_NS_HTML;
}

static bool has_attr(const embi_element *el, const char *name)
{
    return embi_el_get_attribute((embi_element *)(uintptr_t)el, (embi_str){name, (uint32_t)SDL_strlen(name)}) != nullptr;
}

/* ASCII whitespace of the HTML integer parse. */
static bool ascii_space(uint16_t u)
{
    return u == ' ' || u == '\t' || u == '\n' || u == '\f' || u == '\r';
}

/* A tabindex attribute parsed as the HTML rules for integers: leading ASCII whitespace skipped, an optional sign, then
 * digits; anything after the digits is ignored. False when the attribute is absent or has no digits (an invalid value
 * is treated as absent). */
static bool parse_tabindex(const embi_element *el, int32_t *out)
{
    const embi_attr *a = embi_el_get_attribute((embi_element *)(uintptr_t)el, EMBI_STR("tabindex"));
    if (a == nullptr) {
        return false;
    }
    uint32_t i = 0;
    while (i < a->value.len && ascii_space(embi_dstr_unit(a->value, i))) {
        i++;
    }
    bool neg = false;
    if (i < a->value.len && (embi_dstr_unit(a->value, i) == '-' || embi_dstr_unit(a->value, i) == '+')) {
        neg = embi_dstr_unit(a->value, i) == '-';
        i++;
    }
    int64_t v = 0;
    bool any = false;
    for (; i < a->value.len; i++) {
        uint16_t u = embi_dstr_unit(a->value, i);
        if (u < '0' || u > '9') {
            break;
        }
        if (v < 2147483647) {
            v = v * 10 + (u - '0');
        }
        any = true;
    }
    if (!any) {
        return false;
    }
    if (v > 2147483647) {
        v = 2147483647;
    }
    *out = (int32_t)(neg ? -v : v);
    return true;
}

/* input type=hidden: never focusable. */
static bool hidden_input(const embi_element *el)
{
    if (el->base.name != EMBI_ATOM_input) {
        return false;
    }
    const embi_attr *a = embi_el_get_attribute((embi_element *)(uintptr_t)el, EMBI_STR("type"));
    if (a == nullptr || a->value.len != 6) {
        return false;
    }
    static const char hidden[] = "hidden";
    for (uint32_t i = 0; i < 6; i++) {
        uint16_t u = embi_dstr_unit(a->value, i);
        if (u >= 'A' && u <= 'Z') {
            u = (uint16_t)(u - 'A' + 'a');
        }
        if (u != (uint16_t)hidden[i]) {
            return false;
        }
    }
    return true;
}

static bool form_control_tag(uint32_t name)
{
    return name == EMBI_ATOM_button || name == EMBI_ATOM_input || name == EMBI_ATOM_select ||
           name == EMBI_ATOM_textarea;
}

/* Whether el is the first legend child of its parent: the form controls inside it stay enabled in a disabled fieldset. */
static bool first_legend(const embi_node *n)
{
    if (n->name != EMBI_ATOM_legend || n->parent == nullptr) {
        return false;
    }
    for (const embi_node *k = n->parent->first_child; k != nullptr; k = k->next) {
        if (embi_is_element(k) && k->name == EMBI_ATOM_legend) {
            return k == n;
        }
    }
    return false;
}

/* A disabled form control: a control with the disabled attribute, or one inside a disabled fieldset (unless it is inside
 * that fieldset's first legend). */
static bool disabled_control(const embi_element *el)
{
    if (!form_control_tag(el->base.name)) {
        return false;
    }
    if (has_attr(el, "disabled")) {
        return true;
    }
    const embi_node *below = &el->base;
    for (const embi_node *k = el->base.parent; k != nullptr; below = k, k = k->parent) {
        if (embi_is_element(k) && k->name == EMBI_ATOM_fieldset && k->extra == EMBI_NS_HTML &&
            has_attr((const embi_element *)k, "disabled") && !first_legend(below)) {
            return true;
        }
    }
    return false;
}

bool embi_pf_disabled(const embi_element *el)
{
    return el != nullptr && is_html(el) && disabled_control(el);
}

bool embi_pf_focusable(const embi_element *el)
{
    if (el != nullptr && el->base.extra == EMBI_NS_SVG) {
        /* An SVG element is focusable through a valid tabindex, and an SVG link (an a element with href or xlink:href, R4c)
         * is focusable as an HTML link is (SVG 2 section 15.10). */
        int32_t sti = 0;
        return parse_tabindex(el, &sti) ||
               (el->base.name == EMBI_ATOM_a && (has_attr(el, "href") || has_attr(el, "xlink:href")));
    }
    if (el == nullptr || !is_html(el)) {
        return false;
    }
    if (disabled_control(el) || hidden_input(el)) {
        return false;
    }
    int32_t ti = 0;
    if (parse_tabindex(el, &ti)) {
        return true; /* a valid tabindex makes any element focusable (an invalid one is as if absent) */
    }
    switch (el->base.name) {
    case EMBI_ATOM_a:
    case EMBI_ATOM_area:
        return has_attr(el, "href");
    case EMBI_ATOM_button:
    case EMBI_ATOM_input:
    case EMBI_ATOM_select:
    case EMBI_ATOM_textarea:
    case EMBI_ATOM_iframe:
    case EMBI_ATOM_summary:
        return true;
    default:
        return false;
    }
}

int32_t embi_pf_tabindex(const embi_element *el)
{
    int32_t v = 0;
    if (parse_tabindex(el, &v)) {
        return v;
    }
    return embi_pf_focusable(el) ? 0 : -1;
}

/* Collects the focusable elements of the subtree of root with tabindex >= 0, in tree order, without recursion (the walk
 * is by the node links, so a document of any depth is safe). keep (optional) drops elements. When cur is set, *cur_seq
 * receives the number of tree nodes that precede it, and each entry's seq is its own such number. */
static void collect(const embi_node *root, entry_vec *v, embi_pf_keep_fn keep, void *user, const embi_node *cur,
                    uint64_t *cur_seq)
{
    uint64_t seq = 0;
    const embi_node *n = root;
    while (n != nullptr) {
        if (n == cur && cur_seq != nullptr) {
            *cur_seq = seq;
        }
        if (embi_is_element(n)) {
            const embi_element *el = (const embi_element *)n;
            if (embi_pf_focusable(el)) {
                int32_t ti = embi_pf_tabindex(el);
                if (ti >= 0 && (keep == nullptr || keep(user, el))) {
                    entry e = {ti, (embi_element *)(uintptr_t)el, seq};
                    embi_vec_push(v, e);
                }
            }
        }
        seq++;
        if (n->first_child != nullptr) {
            n = n->first_child;
            continue;
        }
        while (n != nullptr && n != root && n->next == nullptr) {
            n = n->parent;
        }
        n = (n == nullptr || n == root) ? nullptr : n->next;
    }
}

/* The order key: positive tabindex ascending, then the zeros (which sort last). */
static uint64_t key_of(int32_t ti)
{
    return ti == 0 ? (uint64_t)1 << 40 : (uint64_t)ti;
}

/* A stable bottom-up merge sort by key_of. tmp has the same length as a. */
static void merge_sort(entry *a, entry *tmp, uint32_t n)
{
    for (uint32_t w = 1; w < n; w *= 2) {
        for (uint32_t lo = 0; lo < n; lo += 2 * w) {
            uint32_t mid = lo + w < n ? lo + w : n;
            uint32_t hi = lo + 2 * w < n ? lo + 2 * w : n;
            uint32_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) {
                tmp[k++] = key_of(a[j].ti) < key_of(a[i].ti) ? a[j++] : a[i++];
            }
            while (i < mid) {
                tmp[k++] = a[i++];
            }
            while (j < hi) {
                tmp[k++] = a[j++];
            }
        }
        for (uint32_t i = 0; i < n; i++) {
            a[i] = tmp[i];
        }
    }
}

/* The sorted order of the entries (allocated array, *n its length). */
static entry *sorted_entries(entry_vec *v, uint32_t *n_out)
{
    uint32_t n = v->len;
    *n_out = n;
    if (n == 0) {
        return nullptr;
    }
    entry *tmp = embi_alloc(n * sizeof(entry));
    merge_sort(v->data, tmp, n);
    embi_free(tmp, n * sizeof(entry));
    return v->data;
}

void embi_pf_focus_order(embi_node *root, embi_element ***out, uint32_t *len)
{
    entry_vec v = {0};
    collect(root, &v, nullptr, nullptr, nullptr, nullptr);
    uint32_t n = v.len;
    if (out != nullptr) {
        *out = nullptr;
    }
    if (len != nullptr) {
        *len = 0;
    }
    if (n == 0) {
        embi_vec_free(&v);
        return;
    }
    entry *tmp = embi_alloc(n * sizeof(entry));
    merge_sort(v.data, tmp, n);
    embi_free(tmp, n * sizeof(entry));
    embi_element **items = embi_alloc(n * sizeof(embi_element *));
    for (uint32_t i = 0; i < n; i++) {
        items[i] = v.data[i].el;
    }
    embi_vec_free(&v);
    if (out != nullptr) {
        *out = items;
    } else {
        embi_free(items, n * sizeof(embi_element *));
        items = nullptr;
    }
    if (len != nullptr) {
        *len = n;
    }
}

void embi_pf_order_free(embi_element **items, uint32_t len)
{
    if (items != nullptr) {
        embi_free(items, len * sizeof(embi_element *));
    }
}

embi_element *embi_pf_focus_step_filtered(embi_node *root, const embi_node *current, bool reverse, embi_pf_keep_fn keep,
                                          void *user)
{
    entry_vec v = {0};
    uint64_t cur_seq = 0;
    collect(root, &v, keep, user, current, &cur_seq);
    uint32_t n = 0;
    entry *items = sorted_entries(&v, &n);
    embi_element *next = nullptr;
    if (n > 0) {
        uint32_t at = n; /* n: current is not in the order */
        for (uint32_t i = 0; current != nullptr && i < n; i++) {
            if ((const embi_node *)items[i].el == current) {
                at = i;
                break;
            }
        }
        if (at < n) {
            next = reverse ? items[at == 0 ? n - 1 : at - 1].el : items[at + 1 == n ? 0 : at + 1].el;
        } else if (current == nullptr) {
            next = reverse ? items[n - 1].el : items[0].el;
        } else if (!reverse) {
            /* The current element is not tabbable (tabindex -1, or not focusable now): the first tabbable element after
             * it in tree order, else the start of the order. */
            for (uint32_t i = 0; i < n && next == nullptr; i++) {
                if (items[i].ti == 0 && items[i].seq > cur_seq) {
                    next = items[i].el;
                }
            }
            if (next == nullptr) {
                next = items[0].el;
            }
        } else {
            for (uint32_t i = n; i > 0 && next == nullptr; i--) {
                if (items[i - 1].ti == 0 && items[i - 1].seq < cur_seq) {
                    next = items[i - 1].el;
                }
            }
            if (next == nullptr) {
                next = items[n - 1].el;
            }
        }
    }
    embi_vec_free(&v);
    return next;
}

embi_element *embi_pf_focus_step(embi_node *root, const embi_node *current, bool reverse)
{
    return embi_pf_focus_step_filtered(root, current, reverse, nullptr, nullptr);
}

bool embi_pf_focus_lost(const embi_node *focused)
{
    return focused != nullptr && (focused->flags & EMBI_NF_CONNECTED) == 0;
}
