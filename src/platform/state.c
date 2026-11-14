/*
 * The dynamic and form pseudo-classes (W5d): the answers of the selector state hook
 * (css/selector.h embi_sel_hooks) for :hover, :active, :focus, :focus-visible, :focus-within, :disabled, :enabled,
 * :checked, :placeholder-shown, :read-only, :read-write, :valid and :invalid. The style engine wires the hook in W5d2.
 * Constraint validation covers the simple constraints (required, minlength, maxlength, email and url syntax, number
 * and range bounds); pattern is W5e's.
 */
#include "platform/platform.h"

#include "base/base.h"
#include "dom/dstr.h"

#include <string.h>

#include <SDL3/SDL_stdinc.h>

static bool ascii_eq(embi_str a, const char *b)
{
    uint32_t n = (uint32_t)SDL_strlen(b);
    if (a.len != n) {
        return false;
    }
    for (uint32_t i = 0; i < n; i++) {
        char c = a.ptr[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != b[i]) {
            return false;
        }
    }
    return true;
}

static bool has(const embi_element *el, const char *name)
{
    return embi_el_get_attribute((embi_element *)(uintptr_t)el, (embi_str){name, (uint32_t)SDL_strlen(name)}) != nullptr;
}

/* The attribute as UTF-8 in sb. False when it is absent. */
static bool attr_utf8(const embi_element *el, const char *name, embi_sb *sb)
{
    const embi_attr *a = embi_el_get_attribute((embi_element *)(uintptr_t)el, (embi_str){name, (uint32_t)SDL_strlen(name)});
    if (a == nullptr) {
        return false;
    }
    embi_dstr_append_utf8(sb, a->value);
    return true;
}

/* The known values of an input's type attribute (HTML "input type keywords"). */
static const char *const k_input_types[] = {"hidden", "text", "search", "tel", "url", "email", "password", "date",
                                            "month", "week", "time", "datetime-local", "number", "range", "color",
                                            "checkbox", "radio", "file", "submit", "image", "reset", "button"};

/* Whether the type of el is t. An absent type, and a value that is not one of the keywords, is the Text state. */
static bool type_is(const embi_element *el, const char *t)
{
    const embi_attr *a = embi_el_get_attribute((embi_element *)(uintptr_t)el, EMBI_STR("type"));
    bool is_text = SDL_strcmp(t, "text") == 0;
    if (a == nullptr) {
        return is_text;
    }
    embi_sb sb = {0};
    embi_dstr_append_utf8(&sb, a->value);
    embi_str v = {sb.data, sb.len};
    bool known = false;
    for (uint32_t i = 0; i < sizeof k_input_types / sizeof k_input_types[0] && !known; i++) {
        known = ascii_eq(v, k_input_types[i]);
    }
    bool r = known ? ascii_eq(v, t) : is_text;
    embi_sb_free(&sb);
    return r;
}

/* Input types that take a text value (the ones :read-write and :placeholder-shown apply to). */
static bool text_like(const embi_element *el)
{
    static const char *const types[] = {"text", "search", "url", "tel", "email", "password", "date", "month",
                                        "week", "time", "datetime-local", "number"};
    for (uint32_t i = 0; i < sizeof types / sizeof types[0]; i++) {
        if (type_is(el, types[i])) {
            return true;
        }
    }
    return false;
}

static bool form_candidate(uint32_t name)
{
    return name == EMBI_ATOM_button || name == EMBI_ATOM_input || name == EMBI_ATOM_select ||
           name == EMBI_ATOM_textarea || name == EMBI_ATOM_fieldset || name == EMBI_ATOM_optgroup ||
           name == EMBI_ATOM_option;
}

/* Whether el is a form control element that the constraint rules apply to (input, select, textarea). */
static bool constrained(const embi_element *el)
{
    return el->base.extra == EMBI_NS_HTML &&
           (el->base.name == EMBI_ATOM_input || el->base.name == EMBI_ATOM_select || el->base.name == EMBI_ATOM_textarea);
}

/* Counts the code points of UTF-8 text (bytes that do not continue a sequence). */
static uint32_t code_points(const embi_sb *sb)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < sb->len; i++) {
        if (((unsigned char)sb->data[i] & 0xC0u) != 0x80u) {
            n++;
        }
    }
    return n;
}

/* Whether the text is an HTML valid floating-point number: an optional '-', digits with an optional fraction (or a
 * fraction alone), and an optional exponent. No '+', no spaces, no "inf", "nan" or hexadecimal. */
static bool valid_float(const char *p, uint32_t n)
{
    uint32_t i = 0;
    if (i < n && p[i] == '-') {
        i++;
    }
    uint32_t int_digits = 0;
    while (i < n && p[i] >= '0' && p[i] <= '9') {
        i++;
        int_digits++;
    }
    if (i < n && p[i] == '.') {
        i++;
        uint32_t frac = 0;
        while (i < n && p[i] >= '0' && p[i] <= '9') {
            i++;
            frac++;
        }
        if (frac == 0) {
            return false;
        }
    } else if (int_digits == 0) {
        return false;
    }
    if (i < n && (p[i] == 'e' || p[i] == 'E')) {
        i++;
        if (i < n && (p[i] == '-' || p[i] == '+')) {
            i++;
        }
        uint32_t exp = 0;
        while (i < n && p[i] >= '0' && p[i] <= '9') {
            i++;
            exp++;
        }
        if (exp == 0) {
            return false;
        }
    }
    return i == n;
}

/* A number as the HTML valid floating-point number: the whole text must be one (the grammar is checked first, then
 * SDL_strtod converts). */
static bool parse_number(const embi_sb *sb, double *out)
{
    char buf[128];
    if (sb->len == 0 || sb->len >= sizeof buf || !valid_float(sb->data, sb->len)) {
        return false;
    }
    SDL_memcpy(buf, sb->data, sb->len);
    buf[sb->len] = '\0';
    char *end = nullptr;
    double v = SDL_strtod(buf, &end);
    if (end != buf + sb->len) {
        return false;
    }
    *out = v;
    return true;
}

/* A maxlength or minlength attribute: digits only, at most nine of them. Anything else (a sign, an exponent, a fraction,
 * an empty or overlong value) is ignored, as HTML ignores an invalid length. False when the attribute is absent or
 * ignored. */
static bool parse_length(const embi_element *el, const char *name, uint32_t *out)
{
    const embi_attr *a = embi_el_get_attribute((embi_element *)(uintptr_t)el, (embi_str){name, (uint32_t)SDL_strlen(name)});
    if (a == nullptr || a->value.len == 0 || a->value.len > 9) {
        return false;
    }
    uint32_t v = 0;
    for (uint32_t i = 0; i < a->value.len; i++) {
        uint16_t u = embi_dstr_unit(a->value, i);
        if (u < '0' || u > '9') {
            return false;
        }
        v = v * 10u + (uint32_t)(u - '0');
    }
    *out = v;
    return true;
}

/* The text content of n's subtree, appended as UTF-8. */
static void text_of(const embi_node *n, embi_sb *sb)
{
    if (n->type == EMBI_NODE_TEXT) {
        embi_dstr_append_utf8(sb, ((const embi_chardata *)n)->data);
        return;
    }
    for (const embi_node *k = n->first_child; k != nullptr; k = k->next) {
        text_of(k, sb);
    }
}

static bool value_empty_or_absent(const embi_element *el, embi_sb *sb)
{
    if (el->base.name == EMBI_ATOM_textarea) {
        /* A textarea's value is its text content. */
        text_of(&el->base, sb);
        return sb->len == 0;
    }
    (void)attr_utf8(el, "value", sb);
    return sb->len == 0;
}

bool embi_pf_control_valid(const embi_element *el)
{
    if (!constrained(el)) {
        return true;
    }
    if (has(el, "disabled")) {
        return true; /* barred from constraint validation */
    }
    embi_sb v = {0};
    bool ok = true;
    bool is_input = el->base.name == EMBI_ATOM_input;
    if (is_input && (type_is(el, "checkbox") || type_is(el, "radio"))) {
        if (has(el, "required") && !has(el, "checked")) {
            ok = false;
        }
        embi_sb_free(&v);
        return ok;
    }
    bool empty = value_empty_or_absent(el, &v);
    if (has(el, "required") && empty && el->base.name != EMBI_ATOM_select && !(is_input && type_is(el, "file"))) {
        ok = false;
    }
    if (ok && !empty && is_input) {
        uint32_t max_len = 0, min_len = 0;
        if (text_like(el) && parse_length(el, "maxlength", &max_len) && code_points(&v) > max_len) {
            ok = false;
        }
        if (ok && text_like(el) && parse_length(el, "minlength", &min_len) && code_points(&v) < min_len) {
            ok = false;
        }
        if (ok && type_is(el, "email")) {
            /* One address: local part, one '@', a non-empty domain without spaces. */
            uint32_t at = 0, count = 0;
            bool space = false;
            for (uint32_t i = 0; i < v.len; i++) {
                if (v.data[i] == '@') {
                    count++;
                    at = i;
                }
                if (v.data[i] == ' ' || v.data[i] == '\t' || v.data[i] == '\n') {
                    space = true;
                }
            }
            ok = count == 1 && at > 0 && at + 1 < v.len && !space;
        }
        if (ok && type_is(el, "url")) {
            /* A scheme (letters, then ':') and something after it. */
            uint32_t colon = 0;
            bool scheme = v.len > 0 && ((v.data[0] >= 'a' && v.data[0] <= 'z') || (v.data[0] >= 'A' && v.data[0] <= 'Z'));
            for (uint32_t i = 0; scheme && i < v.len && colon == 0; i++) {
                char c = v.data[i];
                if (c == ':') {
                    colon = i;
                } else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '+' ||
                             c == '-' || c == '.')) {
                    scheme = false;
                }
            }
            ok = scheme && colon > 0 && colon + 1 < v.len;
        }
        if (ok && (type_is(el, "number") || type_is(el, "range"))) {
            double x = 0;
            if (!parse_number(&v, &x)) {
                ok = false;
            } else {
                embi_sb m = {0};
                const embi_attr *mn2 = embi_el_get_attribute((embi_element *)(uintptr_t)el, EMBI_STR("min"));
                if (mn2 != nullptr) {
                    embi_dstr_append_utf8(&m, mn2->value);
                    double lim = 0;
                    if (parse_number(&m, &lim) && x < lim) {
                        ok = false;
                    }
                    m.len = 0;
                }
                const embi_attr *mx = embi_el_get_attribute((embi_element *)(uintptr_t)el, EMBI_STR("max"));
                if (ok && mx != nullptr) {
                    embi_dstr_append_utf8(&m, mx->value);
                    double lim = 0;
                    if (parse_number(&m, &lim) && x > lim) {
                        ok = false;
                    }
                }
                embi_sb_free(&m);
            }
        }
    }
    embi_sb_free(&v);
    return ok;
}

/* True when el is a (the element itself or one of its ancestors): el matches :hover, :active and :focus-within when the
 * pointer target or the focus lies inside it. */
static bool contains_target(const embi_element *el, const embi_element *target)
{
    if (target == nullptr) {
        return false;
    }
    for (const embi_node *k = (const embi_node *)target; k != nullptr; k = k->parent) {
        if (k == (const embi_node *)el) {
            return true;
        }
    }
    return false;
}

/* Whether el is read-only: not an editable control, or a text control with readonly or disabled. Anything that is not a
 * control is read-only (it is not editable). */
static bool read_only(const embi_element *el)
{
    if (el->base.extra != EMBI_NS_HTML) {
        return true;
    }
    if (el->base.name == EMBI_ATOM_textarea) {
        return has(el, "readonly") || has(el, "disabled");
    }
    if (el->base.name == EMBI_ATOM_input) {
        return !text_like(el) || has(el, "readonly") || has(el, "disabled");
    }
    return true;
}

bool embi_pf_sel_state(void *user, embi_element *el, embi_sel_pc pc)
{
    const embi_pf_state *s = user;
    switch (pc) {
    case EMBI_PC_HOVER:
        return contains_target(el, s->hover);
    case EMBI_PC_ACTIVE:
        return contains_target(el, s->active);
    case EMBI_PC_FOCUS:
        return s->focus == el;
    case EMBI_PC_FOCUS_VISIBLE:
        return s->focus == el && s->focus_visible;
    case EMBI_PC_FOCUS_WITHIN:
        return contains_target(el, s->focus);
    case EMBI_PC_DISABLED:
        return form_candidate(el->base.name) && has(el, "disabled");
    case EMBI_PC_ENABLED:
        return form_candidate(el->base.name) && !has(el, "disabled") &&
               (el->base.name != EMBI_ATOM_option || !has(el, "disabled"));
    case EMBI_PC_CHECKED:
        if (el->base.name == EMBI_ATOM_input && (type_is(el, "checkbox") || type_is(el, "radio"))) {
            return has(el, "checked");
        }
        return el->base.name == EMBI_ATOM_option && has(el, "selected");
    case EMBI_PC_OPEN:
        return el->base.extra == EMBI_NS_HTML && (el->base.name == EMBI_ATOM_details || el->base.name == EMBI_ATOM_dialog) &&
               has(el, "open");
    case EMBI_PC_PLACEHOLDER_SHOWN: {
        if (!has(el, "placeholder")) {
            return false;
        }
        if (el->base.name == EMBI_ATOM_textarea) {
            return el->base.first_child == nullptr;
        }
        if (el->base.name == EMBI_ATOM_input && text_like(el)) {
            embi_sb v = {0};
            bool empty = !attr_utf8(el, "value", &v) || v.len == 0;
            embi_sb_free(&v);
            return empty;
        }
        return false;
    }
    case EMBI_PC_READ_ONLY:
        return read_only(el);
    case EMBI_PC_READ_WRITE:
        return !read_only(el);
    case EMBI_PC_VALID:
        return constrained(el) && embi_pf_control_valid(el);
    case EMBI_PC_INVALID:
        return constrained(el) && !has(el, "disabled") && !embi_pf_control_valid(el);
    default:
        return false;
    }
}
