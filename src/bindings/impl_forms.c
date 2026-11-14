/*
 * Form controls, text editing and form submission (W5e): input, textarea, select, option, button
 * and form. The state of a control (its dirty value and selection in an embi_edit, its checkedness, the selectedness of an
 * option, its custom validity message and the composition range) is kept in the block of its JS wrapper (embi_bind_obj.owned,
 * freed by ctl_release). Connected nodes keep their wrappers, so the state lives as long as the node does.
 *
 * Painting state, pointer handling, history (undo and redo), change on blur and Enter, pattern and step validation, stepUp and
 * stepDown, and POST bodies (urlencoded, multipart, text/plain) are here too; src/paint/control.c draws a control from the
 * state embi_forms_paint_state gives.
 *
 * Limits (see forms.webidl): no file selection (files is always an empty FileList), the pattern attribute is checked by a
 * small backtracking matcher (no lookbehind, no named groups, no Unicode property escapes), the date and time input types
 * keep the value as text (no valueAsDate, no step), setRangeText is missing, cancelling a radio click does not restore the
 * group's previously checked radio, ArrowUp and ArrowDown have no goal column, and form.elements and select.options are
 * NodeList approximations.
 */
#include "bindings/impl_forms.h"

#include "bindings/activation.h"

#include "bindings/gen/forms.h"
#include "bindings/gen/html.h"
#include "bindings/impl_input.h"
#include "dom/event.h"
#include "platform/edit.h"
#include "num/num.h"
#include "paint/paint.h"
#include "platform/platform.h"
#include "style/style.h"

#include <stdatomic.h>
#include <string.h>

#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_timer.h>

/* ------------------------------------------------------------------ control state */

/* One entry of the undo or redo stack of a text control: the value and the selection before an edit. */
typedef struct embi_snap {
    embi_sb text;
    uint32_t anchor, focus;
} embi_snap;
typedef EMBI_VEC(embi_snap) embi_snapvec;
#define EMBI_UNDO_MAX 100u

typedef struct embi_ctl {
    embi_edit ed;          /* the value and the selection of a text control (mirrors the default value when not dirty) */
    embi_sb custom;        /* the setCustomValidity message (empty: no custom error) */
    uint32_t comp_from;    /* byte range of the composition text, valid while comp_on */
    uint32_t comp_to;
    bool comp_on;
    bool value_dirty;      /* the value was set by the user or a script: the value attribute no longer supplies it */
    bool checked;          /* checkedness, valid while checked_dirty */
    bool checked_dirty;
    bool prev_checked;     /* checkedness before the legacy-pre-activation of a checkbox or radio */
    bool selected;         /* option selectedness, valid while selected_dirty */
    bool selected_dirty;
    bool focused;          /* the control has the focus (the caret is drawn) */
    bool uncommitted;      /* the user changed the value since the last change event */
    bool dragging;         /* a pointer drag selects text or moves a range thumb */
    bool open;             /* select: the popup list is open */
    bool indeterminate;    /* checkbox indeterminate */
    int32_t hot;           /* select popup: the highlighted option */
    uint32_t first_row;    /* listbox: the first visible option */
    uint64_t caret_ms;     /* when the caret last moved or the control got the focus (the blink restarts) */
    embi_snapvec undo;
    embi_snapvec redo;
    embi_sb ptext;         /* the value as the painter reads it */
    embi_sb pph;           /* the placeholder as the painter reads it */
    embi_sb plabels;       /* select: the option labels, back to back */
    EMBI_VEC(embi_paint_opt) popts;
} embi_ctl;

static void snaps_free(embi_snapvec *v)
{
    for (uint32_t i = 0; i < v->len; i++) {
        embi_sb_free(&v->data[i].text);
    }
    embi_vec_free(v);
}

/* Pushes the current value and selection of e on a stack (dropping the oldest at the limit). */
static void snap_push(embi_snapvec *v, const embi_edit *e)
{
    if (v->len >= EMBI_UNDO_MAX) {
        embi_sb_free(&v->data[0].text);
        memmove(v->data, v->data + 1, (v->len - 1) * sizeof *v->data);
        v->len--;
    }
    embi_snap sn = {{0}, e->anchor, e->focus};
    if (e->text.len > 0) {
        embi_sb_append(&sn.text, (embi_str){e->text.data, e->text.len});
    }
    embi_vec_push(v, sn);
}

static void ctl_release(void *p)
{
    embi_ctl *s = p;
    embi_edit_free(&s->ed);
    embi_sb_free(&s->custom);
    snaps_free(&s->undo);
    snaps_free(&s->redo);
    embi_sb_free(&s->ptext);
    embi_sb_free(&s->pph);
    embi_sb_free(&s->plabels);
    embi_vec_free(&s->popts);
}

static atomic_uint g_forms_gen; /* bumped whenever the state of a control may change (the views repaint on it) */

uint32_t embi_forms_generation(void)
{
    return atomic_load_explicit(&g_forms_gen, memory_order_relaxed);
}

/* The state of n, created on first use. */
static embi_ctl *ctl_make(embi_bind_ctx *c, embi_node *n)
{
    atomic_fetch_add_explicit(&g_forms_gen, 1u, memory_order_relaxed);
    embi_bind_obj *bo = embi_impl_node_rec(c, n);
    if (bo == nullptr) {
        return nullptr;
    }
    if (bo->owned == nullptr) {
        embi_ctl *s = embi_calloc(1, sizeof *s);
        if (s == nullptr) {
            return nullptr;
        }
        bo->owned = s;
        bo->owned_size = sizeof *s;
        bo->release = ctl_release;
    }
    return bo->owned;
}

/* The state of n when it has one (no wrapper or no state: nullptr, and the defaults apply). */
static const embi_ctl *ctl_peek(const embi_node *n)
{
    const embi_bind_obj *bo = n->wrapper;
    return bo != nullptr ? bo->owned : nullptr;
}

/* ------------------------------------------------------------------ small helpers */

static embi_element *el_of(const embi_node *n)
{
    return (embi_element *)(uintptr_t)n;
}

static bool is_html(const embi_node *n, uint32_t name)
{
    return n != nullptr && embi_is_element(n) && n->extra == EMBI_NS_HTML && n->name == name;
}

static const embi_attr *attr_of(const embi_node *n, const char *name)
{
    return embi_el_get_attribute(el_of(n), (embi_str){name, (uint32_t)SDL_strlen(name)});
}

static bool has_attr(const embi_node *n, const char *name)
{
    return attr_of(n, name) != nullptr;
}

/* ASCII case-insensitive comparison of a DOM string with a literal. */
static bool dstr_ieq(embi_dstr d, const char *lit)
{
    uint32_t n = (uint32_t)SDL_strlen(lit);
    if (d.len != n) {
        return false;
    }
    for (uint32_t i = 0; i < n; i++) {
        uint16_t u = embi_dstr_unit(d, i);
        if (u >= 'A' && u <= 'Z') {
            u = (uint16_t)(u + 32);
        }
        if (u != (uint8_t)lit[i]) {
            return false;
        }
    }
    return true;
}

static bool dstr_eq(embi_dstr a, embi_dstr b)
{
    if (a.len != b.len) {
        return false;
    }
    for (uint32_t i = 0; i < a.len; i++) {
        if (embi_dstr_unit(a, i) != embi_dstr_unit(b, i)) {
            return false;
        }
    }
    return true;
}

/* The attribute as UTF-8 (appended to out). Returns false when the attribute is absent. */
static bool attr_utf8(const embi_node *n, const char *name, embi_sb *out)
{
    const embi_attr *a = attr_of(n, name);
    if (a == nullptr) {
        return false;
    }
    embi_dstr_append_utf8(out, a->value);
    return true;
}

static uint32_t utf16_len(const char *p, uint32_t n)
{
    uint32_t u = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t b = (uint8_t)p[i];
        if ((b & 0xC0u) == 0x80u) {
            continue;
        }
        u += b >= 0xF0u ? 2u : 1u;
    }
    return u;
}

static bool ascii_space(uint8_t b)
{
    return b == ' ' || b == '\t' || b == '\n' || b == '\r' || b == '\f';
}

/* The next node of a pre-order walk of root's subtree (root excluded from the parents it climbs to). */
static embi_node *next_in(embi_node *x, const embi_node *root)
{
    if (x->first_child != nullptr) {
        return x->first_child;
    }
    while (x != root) {
        if (x->next != nullptr) {
            return x->next;
        }
        x = x->parent;
        if (x == nullptr) {
            return nullptr;
        }
    }
    return nullptr;
}

/* The concatenated text of the Text descendants of n. */
static void text_of(const embi_node *n, embi_sb *out)
{
    for (embi_node *x = n->first_child; x != nullptr; x = next_in(x, n)) {
        if (x->type == EMBI_NODE_TEXT) {
            embi_dstr_append_utf8(out, ((const embi_chardata *)x)->data);
        }
    }
}

/* The text with ASCII whitespace runs collapsed to one space and the ends trimmed (option text and label). */
static void collapsed_text(const embi_node *n, embi_sb *out)
{
    embi_sb raw = {0};
    text_of(n, &raw);
    bool pending = false;
    uint32_t start = out->len;
    for (uint32_t i = 0; i < raw.len; i++) {
        uint8_t b = (uint8_t)raw.data[i];
        if (ascii_space(b)) {
            pending = out->len > start;
            continue;
        }
        if (pending) {
            embi_sb_append_char(out, ' ');
            pending = false;
        }
        embi_sb_append_char(out, (char)b);
    }
    embi_sb_free(&raw);
}

/* ------------------------------------------------------------------ input types */

enum {
    IT_TEXT,
    IT_SEARCH,
    IT_URL,
    IT_TEL,
    IT_EMAIL,
    IT_PASSWORD,
    IT_DATE,
    IT_MONTH,
    IT_WEEK,
    IT_TIME,
    IT_LOCAL,
    IT_NUMBER,
    IT_RANGE,
    IT_COLOR,
    IT_CHECKBOX,
    IT_RADIO,
    IT_FILE,
    IT_SUBMIT,
    IT_IMAGE,
    IT_RESET,
    IT_BUTTON,
    IT_HIDDEN,
};

static const struct {
    const char *name;
    int type;
} k_types[] = {
    {"text", IT_TEXT},         {"search", IT_SEARCH},     {"url", IT_URL},           {"tel", IT_TEL},
    {"email", IT_EMAIL},       {"password", IT_PASSWORD}, {"date", IT_DATE},         {"month", IT_MONTH},
    {"week", IT_WEEK},         {"time", IT_TIME},         {"datetime-local", IT_LOCAL}, {"number", IT_NUMBER},
    {"range", IT_RANGE},       {"color", IT_COLOR},       {"checkbox", IT_CHECKBOX}, {"radio", IT_RADIO},
    {"file", IT_FILE},         {"submit", IT_SUBMIT},     {"image", IT_IMAGE},       {"reset", IT_RESET},
    {"button", IT_BUTTON},     {"hidden", IT_HIDDEN},
};

static int input_type_of(const embi_node *n)
{
    const embi_attr *a = attr_of(n, "type");
    if (a == nullptr) {
        return IT_TEXT;
    }
    for (size_t i = 0; i < sizeof k_types / sizeof k_types[0]; i++) {
        if (dstr_ieq(a->value, k_types[i].name)) {
            return k_types[i].type;
        }
    }
    return IT_TEXT;
}

static const char *input_type_name(int t)
{
    for (size_t i = 0; i < sizeof k_types / sizeof k_types[0]; i++) {
        if (k_types[i].type == t) {
            return k_types[i].name;
        }
    }
    return "text";
}

static bool is_input(const embi_node *n)
{
    return is_html(n, EMBI_ATOM_input);
}

/* Types whose value is typed text (the value and the selection apply, and the newlines are stripped). */
static bool text_like(int t)
{
    return t == IT_TEXT || t == IT_SEARCH || t == IT_URL || t == IT_TEL || t == IT_EMAIL || t == IT_PASSWORD;
}

/* Types with a selection of the API (selectionStart and friends, setSelectionRange): HTML leaves out email and number. */
static bool has_selection(int t)
{
    return t == IT_TEXT || t == IT_SEARCH || t == IT_URL || t == IT_TEL || t == IT_PASSWORD;
}

/* Types edited as a single-line text field (typing, the caret and the selection, the clipboard, undo and select()): the
 * text-like types and number, as in Chromium. The number field keeps the text the user typed; its value is that text when
 * it is a valid floating-point number and the empty string otherwise (value_sb). */
static bool text_field(int t)
{
    return text_like(t) || t == IT_NUMBER;
}

/* The characters Chromium lets into a number field (typed, pasted or composed): the others are dropped. */
static bool number_char(char ch)
{
    return (ch >= '0' && ch <= '9') || ch == '.' || ch == ',' || ch == '-' || ch == '+' || ch == 'e' || ch == 'E';
}

/* Types whose value is kept in the value mode (the value attribute is the default, the IDL value is the dirty one). */
static bool value_mode(int t)
{
    switch (t) {
    case IT_CHECKBOX:
    case IT_RADIO:
    case IT_FILE:
    case IT_SUBMIT:
    case IT_IMAGE:
    case IT_RESET:
    case IT_BUTTON:
    case IT_HIDDEN:
        return false;
    default:
        return true;
    }
}

static bool number_like(int t)
{
    return t == IT_NUMBER || t == IT_RANGE;
}

/* Whether s is a valid floating-point number of the HTML rules (no leading plus, digits before or after the point). */
static bool float_ok(const char *s, uint32_t n)
{
    uint32_t i = 0;
    if (i < n && s[i] == '-') {
        i++;
    }
    uint32_t d0 = i;
    while (i < n && s[i] >= '0' && s[i] <= '9') {
        i++;
    }
    uint32_t ints = i - d0;
    uint32_t fracs = 0;
    if (i < n && s[i] == '.') {
        i++;
        uint32_t f0 = i;
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            i++;
        }
        fracs = i - f0;
        if (fracs == 0) {
            return false;
        }
    }
    if (ints == 0 && fracs == 0) {
        return false;
    }
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < n && (s[i] == '+' || s[i] == '-')) {
            i++;
        }
        uint32_t e0 = i;
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            i++;
        }
        if (i == e0) {
            return false;
        }
    }
    return i == n;
}

/* A number from the attribute name, or def when absent or not a valid number. *ok tells whether it was valid. */
static double attr_num(const embi_node *n, const char *name, double def, bool *ok)
{
    embi_sb sb = {0};
    bool present = attr_utf8(n, name, &sb);
    double v = def;
    bool good = present && sb.len > 0 && float_ok(sb.data, sb.len);
    if (good) {
        v = SDL_strtod(embi_sb_cstr(&sb), nullptr);
    }
    embi_sb_free(&sb);
    if (ok != nullptr) {
        *ok = good;
    }
    return v;
}

/* A non-negative integer attribute (maxlength, minlength). Returns false when absent or invalid. */
static bool attr_len(const embi_node *n, const char *name, uint32_t *out)
{
    embi_sb sb = {0};
    bool good = false;
    if (attr_utf8(n, name, &sb) && sb.len > 0) {
        uint64_t v = 0;
        good = true;
        for (uint32_t i = 0; i < sb.len; i++) {
            if (sb.data[i] < '0' || sb.data[i] > '9') {
                good = false;
                break;
            }
            if (v < 0xFFFFFFFFu) {
                v = v * 10u + (uint64_t)(sb.data[i] - '0');
            }
        }
        if (good) {
            *out = v > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)v;
        }
    }
    embi_sb_free(&sb);
    return good;
}

/* Value sanitization of the value algorithms, in place on UTF-8. */
/* Formats a double as a number string (the shortest form that reads back). */
static void fmt_number(double v, embi_sb *out)
{
    char buf[64];
    size_t k = embi_num_to_str(v, buf);
    embi_sb_append(out, (embi_str){buf, (uint32_t)k});
}

static void sanitize(embi_node *n, int t, embi_sb *s)
{
    if (text_like(t) || t == IT_URL || t == IT_EMAIL) {
        uint32_t w = 0;
        for (uint32_t i = 0; i < s->len; i++) {
            if (s->data[i] != '\n' && s->data[i] != '\r') {
                s->data[w++] = s->data[i];
            }
        }
        s->len = w;
    }
    if (t == IT_URL || t == IT_EMAIL) {
        uint32_t a = 0;
        uint32_t b = s->len;
        while (a < b && ascii_space((uint8_t)s->data[a])) {
            a++;
        }
        while (b > a && ascii_space((uint8_t)s->data[b - 1])) {
            b--;
        }
        if (a > 0 || b < s->len) {
            SDL_memmove(s->data, s->data + a, b - a);
            s->len = b - a;
        }
    }
    if (t == IT_NUMBER) {
        if (s->len > 0 && !float_ok(s->data, s->len)) {
            s->len = 0;
        }
    }
    if (t == IT_RANGE) {
        bool ok = s->len > 0 && float_ok(s->data, s->len);
        double mn = attr_num(n, "min", 0.0, nullptr);
        double mx = attr_num(n, "max", 100.0, nullptr);
        if (mx < mn) {
            mx = mn;
        }
        double v = ok ? SDL_strtod(embi_sb_cstr(s), nullptr) : (mn + mx) / 2.0;
        if (v < mn) {
            v = mn;
        }
        if (v > mx) {
            v = mx;
        }
        s->len = 0;
        fmt_number(v, s);
    }
    if (t == IT_COLOR) {
        bool ok = s->len == 7 && s->data[0] == '#';
        for (uint32_t i = 1; ok && i < 7; i++) {
            char ch = s->data[i];
            ok = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
        }
        if (ok) {
            for (uint32_t i = 1; i < 7; i++) {
                if (s->data[i] >= 'A' && s->data[i] <= 'F') {
                    s->data[i] = (char)(s->data[i] + 32);
                }
            }
        } else {
            s->len = 0;
            embi_sb_append(s, EMBI_STR("#000000"));
        }
    }
}

/* ------------------------------------------------------------------ tags and form association */

static bool is_textarea(const embi_node *n)
{
    return is_html(n, EMBI_ATOM_textarea);
}

static bool is_select(const embi_node *n)
{
    return is_html(n, EMBI_ATOM_select);
}

static bool is_option(const embi_node *n)
{
    return is_html(n, EMBI_ATOM_option);
}

static bool is_button(const embi_node *n)
{
    return is_html(n, EMBI_ATOM_button);
}

static bool is_form(const embi_node *n)
{
    return is_html(n, EMBI_ATOM_form);
}

/* The button type: "submit" (the default), "reset" or "button". */
static int button_kind(const embi_node *n)
{
    const embi_attr *a = attr_of(n, "type");
    if (a != nullptr) {
        if (dstr_ieq(a->value, "reset")) {
            return IT_RESET;
        }
        if (dstr_ieq(a->value, "button")) {
            return IT_BUTTON;
        }
    }
    return IT_SUBMIT;
}

/* Whether n is a submit button of the kind submission uses: input submit or image, or a button of type submit. */
static bool is_submit_button(const embi_node *n)
{
    if (is_input(n)) {
        int t = input_type_of(n);
        return t == IT_SUBMIT || t == IT_IMAGE;
    }
    return is_button(n) && button_kind(n) == IT_SUBMIT;
}

/* The form owner of a form-associated element: the form attribute's element, else the nearest ancestor form. */
static embi_node *owner_form(const embi_node *n)
{
    if (!embi_is_element(n)) {
        return nullptr;
    }
    const embi_attr *fa = attr_of(n, "form");
    if (fa != nullptr) {
        embi_sb id = {0};
        embi_dstr_append_utf8(&id, fa->value);
        embi_element *f = embi_get_element_by_id(n->doc, (embi_str){id.data, id.len});
        embi_sb_free(&id);
        return f != nullptr && is_form(&f->base) ? &f->base : nullptr;
    }
    for (embi_node *p = n->parent; p != nullptr; p = p->parent) {
        if (is_form(p)) {
            return p;
        }
    }
    return nullptr;
}

/* Whether n is a listed element of the form kind (the ones the form's elements collection and submission look at). */
static bool listed(const embi_node *n)
{
    return is_input(n) || is_select(n) || is_textarea(n) || is_button(n) || is_html(n, EMBI_ATOM_fieldset) ||
           is_html(n, EMBI_ATOM_output) || is_html(n, EMBI_ATOM_object);
}

/* Whether n is inside a datalist (its controls are not validated). */
static bool in_datalist(const embi_node *n)
{
    for (const embi_node *p = n->parent; p != nullptr; p = p->parent) {
        if (is_html(p, EMBI_ATOM_datalist)) {
            return true;
        }
    }
    return false;
}

/* The top of n's tree: the radio group is looked for below it. */
static embi_node *tree_root(embi_node *n)
{
    while (n->parent != nullptr) {
        n = n->parent;
    }
    return n;
}

/* ------------------------------------------------------------------ checkedness and selectedness */

static bool checked_of(const embi_node *n)
{
    const embi_ctl *s = ctl_peek(n);
    if (s != nullptr && s->checked_dirty) {
        return s->checked;
    }
    return has_attr(n, "checked");
}

static bool is_radio(const embi_node *n)
{
    return is_input(n) && input_type_of(n) == IT_RADIO;
}

/* The other radio buttons of n's group: same tree, same form owner, same non-empty name. */
typedef void (*radio_fn)(embi_bind_ctx *c, embi_node *radio, void *user);
static void radio_group_each(embi_bind_ctx *c, embi_node *n, radio_fn fn, void *user)
{
    const embi_attr *na = attr_of(n, "name");
    if (na == nullptr || na->value.len == 0) {
        return;
    }
    embi_node *f = owner_form(n);
    embi_node *root = tree_root(n);
    for (embi_node *x = root; x != nullptr; x = next_in(x, root)) {
        if (x == n || !is_radio(x)) {
            continue;
        }
        const embi_attr *xa = attr_of(x, "name");
        if (xa == nullptr || !dstr_eq(xa->value, na->value) || owner_form(x) != f) {
            continue;
        }
        fn(c, x, user);
    }
}

/* W8b: the checkedness or selectedness of n changed without a DOM change: the rules with :checked match again. */
static void state_restyle(embi_bind_ctx *c, embi_node *n)
{
    if (c != nullptr && c->host.style != nullptr && embi_is_element(n)) {
        embi_style_mark_state(c->host.style, (embi_element *)n);
    }
}

/* The same for every option of the select sel (a change of one option's selectedness can change the others'). */
static void select_restyle(embi_bind_ctx *c, embi_node *sel)
{
    for (embi_node *x = sel != nullptr ? sel->first_child : nullptr; x != nullptr; x = next_in(x, sel)) {
        if (is_option(x)) {
            state_restyle(c, x);
        }
    }
}

static void uncheck_one(embi_bind_ctx *c, embi_node *radio, void *user)
{
    (void)user;
    const bool was = checked_of(radio);
    embi_ctl *s = ctl_make(c, radio);
    if (s != nullptr) {
        s->checked = false;
        s->checked_dirty = true;
        if (was) {
            state_restyle(c, radio);
        }
    }
}

static void set_checked(embi_bind_ctx *c, embi_node *n, bool v)
{
    const bool was = checked_of(n);
    embi_ctl *s = ctl_make(c, n);
    if (s == nullptr) {
        return;
    }
    s->checked = v;
    s->checked_dirty = true;
    if (was != v) {
        state_restyle(c, n);
    }
    if (v && is_radio(n)) {
        radio_group_each(c, n, uncheck_one, nullptr);
    }
}

/* The option's selectedness as the spec's selectedness setting algorithm sees it (raw, before the single-select fallback). */
static bool option_raw(const embi_node *opt)
{
    const embi_ctl *s = ctl_peek(opt);
    if (s != nullptr && s->selected_dirty) {
        return s->selected;
    }
    return has_attr(opt, "selected");
}

static bool option_disabled(const embi_node *opt)
{
    if (has_attr(opt, "disabled")) {
        return true;
    }
    const embi_node *p = opt->parent;
    return is_html(p, EMBI_ATOM_optgroup) && has_attr(p, "disabled");
}

/* The options of a select, in tree order (inside optgroups too). Written to out (capacity cap); returns the count. */
static uint32_t select_options(embi_node *sel, embi_node **out, uint32_t cap)
{
    uint32_t n = 0;
    for (embi_node *x = sel->first_child; x != nullptr; x = next_in(x, sel)) {
        if (is_option(x)) {
            if (out != nullptr && n < cap) {
                out[n] = x;
            }
            n++;
        }
    }
    return n;
}

static bool select_multiple(const embi_node *sel)
{
    return has_attr(sel, "multiple");
}

/* The option a single select shows: the last raw-selected option, else the first non-disabled one. nullptr for none. */
static embi_node *single_selected(embi_node *sel)
{
    embi_node *picked = nullptr;
    embi_node *first_enabled = nullptr;
    for (embi_node *x = sel->first_child; x != nullptr; x = next_in(x, sel)) {
        if (!is_option(x)) {
            continue;
        }
        if (option_raw(x)) {
            picked = x;
        }
        if (first_enabled == nullptr && !option_disabled(x)) {
            first_enabled = x;
        }
    }
    return picked != nullptr ? picked : first_enabled;
}

/* The selected options of a select (one for a single select, all raw-selected ones for a multiple select). */
static bool option_selected_effective(embi_node *opt, embi_node *sel)
{
    if (sel == nullptr) {
        return option_raw(opt);
    }
    if (select_multiple(sel)) {
        return option_raw(opt);
    }
    return single_selected(sel) == opt;
}

static embi_node *nearest_select(embi_node *opt)
{
    for (embi_node *p = opt->parent; p != nullptr; p = p->parent) {
        if (is_select(p)) {
            return p;
        }
    }
    return nullptr;
}

static void option_value(const embi_node *opt, embi_sb *out)
{
    if (!attr_utf8(opt, "value", out)) {
        collapsed_text(opt, out);
    }
}

/* ------------------------------------------------------------------ value */

/* The default value of an input in the value mode: the value attribute, sanitized. */
static void default_value_sb(embi_node *n, int t, embi_sb *out)
{
    attr_utf8(n, "value", out);
    sanitize(n, t, out);
}

/* The value (UTF-8, appended to out) as scripts read it. */
static void value_sb(embi_node *n, embi_sb *out)
{
    if (is_textarea(n)) {
        const embi_ctl *s = ctl_peek(n);
        if (s != nullptr && s->value_dirty) {
            embi_str v = embi_edit_value(&s->ed);
            if (v.len > 0) {
                embi_sb_append(out, v);
            }
        } else {
            text_of(n, out);
        }
        return;
    }
    if (is_select(n)) {
        embi_node *opt = single_selected(n);
        if (select_multiple(n)) {
            opt = nullptr;
            for (embi_node *x = n->first_child; x != nullptr; x = next_in(x, n)) {
                if (is_option(x) && option_raw(x)) {
                    opt = x;
                    break;
                }
            }
        }
        if (opt != nullptr) {
            option_value(opt, out);
        }
        return;
    }
    if (!is_input(n)) {
        return;
    }
    int t = input_type_of(n);
    if (t == IT_CHECKBOX || t == IT_RADIO) {
        if (!attr_utf8(n, "value", out)) {
            embi_sb_append(out, EMBI_STR("on"));
        }
        return;
    }
    if (t == IT_FILE) {
        return; /* no file is ever selected by the engine */
    }
    if (!value_mode(t)) {
        attr_utf8(n, "value", out);
        return;
    }
    const embi_ctl *s = ctl_peek(n);
    if (s != nullptr && s->value_dirty) {
        embi_str v = embi_edit_value(&s->ed);
        /* A number field may hold text the user is still typing ("1e", "-"): its value is then the empty string. */
        if (t == IT_EMAIL || t == IT_URL) {
            /* typed text is sanitized as a set value is: no white space around it */
            while (v.len > 0 && ascii_space((uint8_t)v.ptr[0])) {
                v.ptr++;
                v.len--;
            }
            while (v.len > 0 && ascii_space((uint8_t)v.ptr[v.len - 1])) {
                v.len--;
            }
        }
        if (v.len > 0 && !(t == IT_NUMBER && !float_ok(v.ptr, v.len))) {
            embi_sb_append(out, v);
        }
        return;
    }
    default_value_sb(n, t, out);
}

/* The text a text field shows (appended to out): its value, except that number, email and url fields show the text the user
 * typed (not trimmed, even when it is not a valid number), which the caret offsets refer to. */
static void shown_sb(embi_node *n, embi_sb *out)
{
    const embi_ctl *s = ctl_peek(n);
    int st = is_input(n) ? input_type_of(n) : IT_TEXT;
    if ((st == IT_NUMBER || st == IT_EMAIL || st == IT_URL) && s != nullptr && s->value_dirty) {
        embi_str v = embi_edit_value(&s->ed);
        if (v.len > 0) {
            embi_sb_append(out, v);
        }
        return;
    }
    value_sb(n, out);
}

/* Whether a number field holds text that is not a valid number (badInput). */
static bool number_bad_input(const embi_node *n)
{
    const embi_ctl *s = ctl_peek(n);
    if (!is_input(n) || input_type_of(n) != IT_NUMBER || s == nullptr || !s->value_dirty) {
        return false;
    }
    embi_str v = embi_edit_value(&s->ed);
    return v.len > 0 && !float_ok(v.ptr, v.len);
}

/* Keeps the edit model in step with the value when the value is not dirty (the attribute supplies it), so the selection
 * stays meaningful. */
static void sync_ed(embi_bind_ctx *c, embi_node *n, embi_ctl *s)
{
    if (s->value_dirty) {
        return;
    }
    embi_sb cur = {0};
    value_sb(n, &cur);
    bool same = cur.len == s->ed.text.len &&
                (cur.len == 0 || memcmp(cur.data, s->ed.text.data, cur.len) == 0);
    if (!same) {
        (void)embi_edit_set_value(&s->ed, (embi_str){cur.data, cur.len});
    }
    embi_sb_free(&cur);
    (void)c;
}

/* The edit state of a text control, synced; nullptr when the control has none. */
static embi_ctl *text_state(embi_bind_ctx *c, embi_node *n)
{
    embi_ctl *s = ctl_make(c, n);
    if (s != nullptr) {
        sync_ed(c, n, s);
    }
    return s;
}

/* Sets the value as a script does (the value setters). Returns an error for the file input. */
static embi_dom_err set_value(embi_bind_ctx *c, embi_node *n, embi_str v)
{
    if (is_input(n) && input_type_of(n) == IT_FILE) {
        return EMBI_DOM_OK; /* a file input cannot be given a value by script */
    }
    if (is_select(n)) {
        embi_node *match = nullptr;
        for (embi_node *x = n->first_child; x != nullptr; x = next_in(x, n)) {
            if (!is_option(x)) {
                continue;
            }
            embi_sb ov = {0};
            option_value(x, &ov);
            bool eq = ov.len == v.len && (v.len == 0 || memcmp(ov.data, v.ptr, v.len) == 0);
            embi_sb_free(&ov);
            if (eq && match == nullptr) {
                match = x;
            }
        }
        for (embi_node *x = n->first_child; x != nullptr; x = next_in(x, n)) {
            if (is_option(x)) {
                embi_ctl *s = ctl_make(c, x);
                if (s != nullptr) {
                    s->selected = x == match;
                    s->selected_dirty = true;
                }
            }
        }
        select_restyle(c, n);
        return EMBI_DOM_OK;
    }
    if (is_textarea(n) || (is_input(n) && value_mode(input_type_of(n)))) {
        int t = is_input(n) ? input_type_of(n) : IT_TEXT;
        if (is_input(n) && t == IT_FILE) {
            return EMBI_DOM_OK;
        }
        embi_sb s = {0};
        if (v.len > 0) {
            embi_sb_append(&s, v);
        }
        if (is_input(n)) {
            sanitize(n, t, &s);
        }
        embi_ctl *st = ctl_make(c, n);
        if (st != nullptr) {
            (void)embi_edit_set_value(&st->ed, (embi_str){s.data, s.len});
            st->value_dirty = true;
            st->comp_on = false;
            st->uncommitted = false;
            snaps_free(&st->undo);
            snaps_free(&st->redo);
        }
        embi_sb_free(&s);
        return EMBI_DOM_OK;
    }
    /* The default mode (checkbox, radio, submit, hidden, ...): the value attribute itself. */
    (void)embi_el_set_attribute(el_of(n), EMBI_STR("value"), embi_bind_tmp_utf8(c, v));
    return EMBI_DOM_OK;
}

/* ------------------------------------------------------------------ pattern matching (the pattern attribute) */

/* A compact backtracking matcher for the subset of regular expressions that pattern attributes use: literals, ., classes with
 * ranges and negation, \d \w \s and their negations outside classes, escapes, groups (capturing or (?:)), alternation, and the
 * quantifiers * + ? {n} {n,} {n,m} (lazy marks are accepted). The match is anchored at both ends, as the attribute defines
 * (^(?:pattern)$). A pattern outside the subset does not compile; the control is then not constrained by it. */
enum { RX_CHAR, RX_ANY, RX_SET, RX_GROUP, RX_BOL, RX_EOL };
#define RX_NODES 192
#define RX_RANGES 192
#define RX_INF 0xFFFFFFFFu
typedef struct rx_node {
    uint8_t type;
    bool neg;
    uint16_t next;  /* the next element of the sequence (0: none) */
    uint16_t alt;   /* group: the first alternative's head */
    uint16_t altnx; /* an alternative head: the next alternative */
    uint32_t lo, hi;
    uint32_t cp;
    uint16_t set_at, set_n;
} rx_node;
typedef struct rx_prog {
    rx_node n[RX_NODES];
    uint32_t cnt;
    uint32_t rng[RX_RANGES][2];
    uint32_t nrng;
    const char *pat;
    uint32_t plen, pos;
    bool bad;
    const char *s;
    uint32_t slen;
    uint32_t steps;
    uint32_t depth;
} rx_prog;
typedef struct rx_cont {
    const struct rx_cont *up;
    uint16_t node;
    uint32_t cnt;
    uint32_t start;
    uint8_t kind; /* 0 end, 1 sequence, 2 repeat */
} rx_cont;

static uint32_t rx_dec(const char *s, uint32_t len, uint32_t at, uint32_t *n)
{
    uint8_t b = (uint8_t)s[at];
    uint32_t k = b < 0x80 ? 1 : (b >= 0xF0 ? 4 : (b >= 0xE0 ? 3 : (b >= 0xC0 ? 2 : 1)));
    if (at + k > len) {
        k = 1;
    }
    uint32_t cp = k == 1 ? b : (k == 2 ? (b & 0x1Fu) : (k == 3 ? (b & 0x0Fu) : (b & 0x07u)));
    for (uint32_t i = 1; i < k; i++) {
        cp = (cp << 6) | ((uint8_t)s[at + i] & 0x3Fu);
    }
    *n = k;
    return cp;
}

static uint16_t rx_new(rx_prog *r, uint8_t type)
{
    if (r->cnt >= RX_NODES) {
        r->bad = true;
        return 0;
    }
    rx_node *x = &r->n[r->cnt];
    memset(x, 0, sizeof *x);
    x->type = type;
    x->lo = x->hi = 1;
    return (uint16_t)r->cnt++;
}

static void rx_range(rx_prog *r, uint32_t lo, uint32_t hi)
{
    if (r->nrng >= RX_RANGES) {
        r->bad = true;
        return;
    }
    r->rng[r->nrng][0] = lo;
    r->rng[r->nrng][1] = hi;
    r->nrng++;
}

/* Adds the ranges of \d \w \s (the lower-case letter). Returns false for another letter. */
static bool rx_preset(rx_prog *r, char k)
{
    switch (k) {
    case 'd':
        rx_range(r, '0', '9');
        return true;
    case 'w':
        rx_range(r, '0', '9');
        rx_range(r, 'A', 'Z');
        rx_range(r, '_', '_');
        rx_range(r, 'a', 'z');
        return true;
    case 's':
        rx_range(r, 9, 13);
        rx_range(r, ' ', ' ');
        rx_range(r, 0xA0, 0xA0);
        return true;
    default:
        return false;
    }
}

static uint32_t rx_escape_cp(char k)
{
    switch (k) {
    case 'n':
        return 10;
    case 't':
        return 9;
    case 'r':
        return 13;
    case 'f':
        return 12;
    case 'v':
        return 11;
    case '0':
        return 0;
    default:
        return (uint32_t)(uint8_t)k;
    }
}

static uint16_t rx_alt(rx_prog *r);

static uint16_t rx_atom(rx_prog *r)
{
    if (r->pos >= r->plen) {
        r->bad = true;
        return 0;
    }
    char ch = r->pat[r->pos];
    if (ch == '(') {
        r->pos++;
        if (r->pos + 1 < r->plen && r->pat[r->pos] == '?') {
            if (r->pat[r->pos + 1] != ':') {
                r->bad = true; /* lookaheads and named groups are outside the subset */
                return 0;
            }
            r->pos += 2;
        }
        uint16_t g = rx_new(r, RX_GROUP);
        uint16_t head = rx_alt(r);
        if (r->bad || r->pos >= r->plen || r->pat[r->pos] != ')') {
            r->bad = true;
            return 0;
        }
        r->pos++;
        r->n[g].alt = head;
        return g;
    }
    if (ch == '[') {
        r->pos++;
        uint16_t x = rx_new(r, RX_SET);
        if (r->bad) {
            return 0;
        }
        r->n[x].set_at = (uint16_t)r->nrng;
        if (r->pos < r->plen && r->pat[r->pos] == '^') {
            r->n[x].neg = true;
            r->pos++;
        }
        while (r->pos < r->plen && r->pat[r->pos] != ']') {
            uint32_t lo;
            if (r->pat[r->pos] == '\\') {
                if (r->pos + 1 >= r->plen) {
                    r->bad = true;
                    return 0;
                }
                char k = r->pat[r->pos + 1];
                r->pos += 2;
                if (k == 'd' || k == 'w' || k == 's') {
                    (void)rx_preset(r, k);
                    continue;
                }
                if (k == 'D' || k == 'W' || k == 'S') {
                    r->bad = true;
                    return 0;
                }
                lo = rx_escape_cp(k);
            } else {
                uint32_t k = 0;
                lo = rx_dec(r->pat, r->plen, r->pos, &k);
                r->pos += k;
            }
            uint32_t hi = lo;
            if (r->pos + 1 < r->plen && r->pat[r->pos] == '-' && r->pat[r->pos + 1] != ']') {
                r->pos++;
                if (r->pat[r->pos] == '\\') {
                    if (r->pos + 1 >= r->plen) {
                        r->bad = true;
                        return 0;
                    }
                    hi = rx_escape_cp(r->pat[r->pos + 1]);
                    r->pos += 2;
                } else {
                    uint32_t k = 0;
                    hi = rx_dec(r->pat, r->plen, r->pos, &k);
                    r->pos += k;
                }
                if (hi < lo) {
                    r->bad = true;
                    return 0;
                }
            }
            rx_range(r, lo, hi);
        }
        if (r->pos >= r->plen) {
            r->bad = true;
            return 0;
        }
        r->pos++;
        r->n[x].set_n = (uint16_t)(r->nrng - r->n[x].set_at);
        return x;
    }
    if (ch == '.') {
        r->pos++;
        return rx_new(r, RX_ANY);
    }
    if (ch == '^' || ch == '$') {
        r->pos++;
        return rx_new(r, ch == '^' ? RX_BOL : RX_EOL);
    }
    if (ch == '\\') {
        if (r->pos + 1 >= r->plen) {
            r->bad = true;
            return 0;
        }
        char k = r->pat[r->pos + 1];
        r->pos += 2;
        if (k == 'd' || k == 'w' || k == 's' || k == 'D' || k == 'W' || k == 'S') {
            uint16_t x = rx_new(r, RX_SET);
            if (r->bad) {
                return 0;
            }
            r->n[x].set_at = (uint16_t)r->nrng;
            (void)rx_preset(r, (char)(k | 0x20));
            r->n[x].set_n = (uint16_t)(r->nrng - r->n[x].set_at);
            r->n[x].neg = k < 'a';
            return x;
        }
        if (k == 'b' || k == 'B' || (k >= '1' && k <= '9') || k == 'k' || k == 'p' || k == 'P' || k == 'u' || k == 'x' ||
            k == 'c') {
            r->bad = true;
            return 0;
        }
        uint16_t x = rx_new(r, RX_CHAR);
        r->n[x].cp = rx_escape_cp(k);
        return x;
    }
    if (ch == '*' || ch == '+' || ch == '?' || ch == ')' || ch == '|') {
        r->bad = true;
        return 0;
    }
    uint32_t k = 0;
    uint32_t cp = rx_dec(r->pat, r->plen, r->pos, &k);
    r->pos += k;
    uint16_t x = rx_new(r, RX_CHAR);
    r->n[x].cp = cp;
    return x;
}

static bool rx_num(rx_prog *r, uint32_t *v)
{
    uint32_t i = r->pos, n = 0;
    while (i < r->plen && r->pat[i] >= '0' && r->pat[i] <= '9') {
        n = n * 10u + (uint32_t)(r->pat[i] - '0');
        if (n > 1000) {
            n = 1000;
        }
        i++;
    }
    if (i == r->pos) {
        return false;
    }
    r->pos = i;
    *v = n;
    return true;
}

static uint16_t rx_seq(rx_prog *r)
{
    uint16_t head = 0, tail = 0;
    while (!r->bad && r->pos < r->plen && r->pat[r->pos] != '|' && r->pat[r->pos] != ')') {
        uint16_t a = rx_atom(r);
        if (r->bad) {
            return 0;
        }
        if (r->pos < r->plen) {
            char q = r->pat[r->pos];
            if (q == '*') {
                r->n[a].lo = 0;
                r->n[a].hi = RX_INF;
                r->pos++;
            } else if (q == '+') {
                r->n[a].lo = 1;
                r->n[a].hi = RX_INF;
                r->pos++;
            } else if (q == '?') {
                r->n[a].lo = 0;
                r->n[a].hi = 1;
                r->pos++;
            } else if (q == '{') {
                r->pos++;
                uint32_t lo = 0, hi = 0;
                if (!rx_num(r, &lo)) {
                    r->bad = true;
                    return 0;
                }
                hi = lo;
                if (r->pos < r->plen && r->pat[r->pos] == ',') {
                    r->pos++;
                    if (!rx_num(r, &hi)) {
                        hi = RX_INF;
                    }
                }
                if (r->pos < r->plen && r->pat[r->pos] == '}' && (hi == RX_INF || hi >= lo)) {
                    r->pos++;
                    r->n[a].lo = lo;
                    r->n[a].hi = hi;
                } else {
                    r->bad = true;
                    return 0;
                }
            }
            if (r->pos < r->plen && r->pat[r->pos] == '?' && (q == '*' || q == '+' || q == '?' || q == '}')) {
                r->pos++; /* lazy: irrelevant for a full match */
            }
        }
        if (head == 0) {
            head = a;
        } else {
            r->n[tail].next = a;
        }
        tail = a;
    }
    return head;
}

/* Alternatives, linked through altnx. An empty alternative is a group with no alternatives (it matches the empty string). */
static uint16_t rx_alt(rx_prog *r)
{
    uint16_t first = 0, last = 0;
    for (;;) {
        uint16_t h = rx_seq(r);
        if (r->bad) {
            return 0;
        }
        if (h == 0) {
            h = rx_new(r, RX_GROUP);
        }
        if (first == 0) {
            first = h;
        } else {
            r->n[last].altnx = h;
        }
        last = h;
        if (r->pos < r->plen && r->pat[r->pos] == '|') {
            r->pos++;
            continue;
        }
        break;
    }
    return first;
}

static bool rx_call(rx_prog *r, const rx_cont *k, uint32_t pos);
static bool rx_run(rx_prog *r, uint16_t n, uint32_t pos, const rx_cont *k);

static bool rx_rep(rx_prog *r, uint16_t n, uint32_t pos, uint32_t cnt, const rx_cont *k);

static bool rx_one(rx_prog *r, uint16_t n, uint32_t pos, const rx_cont *k)
{
    const rx_node *x = &r->n[n];
    switch (x->type) {
    case RX_BOL:
        return pos == 0 && rx_call(r, k, pos);
    case RX_EOL:
        return pos == r->slen && rx_call(r, k, pos);
    case RX_GROUP: {
        if (x->alt == 0) {
            return rx_call(r, k, pos);
        }
        uint16_t a = x->alt;
        for (;;) {
            if (rx_run(r, a, pos, k)) {
                return true;
            }
            if (r->n[a].altnx == 0) {
                break;
            }
            a = r->n[a].altnx;
        }
        return false;
    }
    default:
        break;
    }
    if (pos >= r->slen) {
        return false;
    }
    uint32_t len = 0;
    uint32_t cp = rx_dec(r->s, r->slen, pos, &len);
    bool ok;
    if (x->type == RX_CHAR) {
        ok = cp == x->cp;
    } else if (x->type == RX_ANY) {
        ok = cp != 10 && cp != 13 && cp != 0x2028 && cp != 0x2029;
    } else {
        ok = false;
        for (uint32_t i = 0; i < x->set_n; i++) {
            if (cp >= r->rng[x->set_at + i][0] && cp <= r->rng[x->set_at + i][1]) {
                ok = true;
                break;
            }
        }
        ok = ok != x->neg;
    }
    return ok && rx_call(r, k, pos + len);
}

#define RX_MAX_DEPTH 160u
#define RX_MAX_STEPS 400000u

/* Whether the single-character element x matches the code point at pos (len: its byte length). */
static bool rx_single(const rx_prog *r, const rx_node *x, uint32_t pos, uint32_t *len)
{
    if (pos >= r->slen) {
        return false;
    }
    uint32_t cp = rx_dec(r->s, r->slen, pos, len);
    if (x->type == RX_CHAR) {
        return cp == x->cp;
    }
    if (x->type == RX_ANY) {
        return cp != 10 && cp != 13 && cp != 0x2028 && cp != 0x2029;
    }
    bool ok = false;
    for (uint32_t i = 0; i < x->set_n; i++) {
        if (cp >= r->rng[x->set_at + i][0] && cp <= r->rng[x->set_at + i][1]) {
            ok = true;
            break;
        }
    }
    return ok != x->neg;
}

static bool rx_rep_in(rx_prog *r, uint16_t n, uint32_t pos, uint32_t cnt, const rx_cont *k);

/* Repeats element n (cnt iterations so far), then continues with k. Recursion depth is bounded: single-character atoms loop
 * greedily instead of recursing per character, and anything deeper than RX_MAX_DEPTH counts as "too costly: do not constrain". */
static bool rx_rep(rx_prog *r, uint16_t n, uint32_t pos, uint32_t cnt, const rx_cont *k)
{
    if (r->depth >= RX_MAX_DEPTH) {
        return true;
    }
    r->depth++;
    bool ok = rx_rep_in(r, n, pos, cnt, k);
    r->depth--;
    return ok;
}

static bool rx_rep_in(rx_prog *r, uint16_t n, uint32_t pos, uint32_t cnt, const rx_cont *k)
{
    const rx_node *x = &r->n[n];
    if (x->type == RX_CHAR || x->type == RX_ANY || x->type == RX_SET) {
        /* Greedy run, then back off one character at a time. */
        uint32_t p = pos, c = cnt;
        while (c < x->hi) {
            uint32_t len = 0;
            if (!rx_single(r, x, p, &len)) {
                break;
            }
            p += len;
            c++;
        }
        for (;;) {
            if (++r->steps > RX_MAX_STEPS) {
                return true; /* too costly: do not constrain */
            }
            if (c >= x->lo && rx_call(r, k, p)) {
                return true;
            }
            if (c == cnt) {
                return false;
            }
            uint32_t q = p - 1;
            while (q > pos && (((uint8_t)r->s[q]) & 0xC0u) == 0x80u) {
                q--;
            }
            p = q;
            c--;
        }
    }
    if (++r->steps > RX_MAX_STEPS) {
        return true; /* too costly: do not constrain */
    }
    if (cnt < x->hi) {
        rx_cont kk = {k, n, cnt + 1, pos, 2};
        if (rx_one(r, n, pos, &kk)) {
            return true;
        }
    }
    return cnt >= x->lo && rx_call(r, k, pos);
}

/* The sequence starting at element n, then k. */
static bool rx_run(rx_prog *r, uint16_t n, uint32_t pos, const rx_cont *k)
{
    if (n == 0) {
        return rx_call(r, k, pos);
    }
    rx_cont kk = {k, r->n[n].next, 0, 0, 1};
    return rx_rep(r, n, pos, 0, &kk);
}

static bool rx_call(rx_prog *r, const rx_cont *k, uint32_t pos)
{
    switch (k->kind) {
    case 0:
        return pos == r->slen;
    case 1:
        return rx_run(r, k->node, pos, k->up);
    default:
        if (pos == k->start && k->cnt > r->n[k->node].lo) {
            return false; /* an empty iteration beyond the minimum: stop looping */
        }
        return rx_rep(r, k->node, pos, k->cnt, k->up);
    }
}

/* Whether s matches the pattern as a whole. *valid is false when the pattern is outside the subset (nothing is said then). */
static bool pattern_match(const char *pat, uint32_t plen, const char *s, uint32_t slen, bool *valid)
{
    rx_prog *r = embi_calloc(1, sizeof *r);
    if (r == nullptr) {
        *valid = false;
        return true;
    }
    r->cnt = 1; /* node 0 is the "none" sentinel */
    r->pat = pat;
    r->plen = plen;
    uint16_t head = rx_alt(r);
    if (r->bad || r->pos != r->plen) {
        *valid = false;
        embi_free(r, sizeof *r);
        return true;
    }
    *valid = true;
    r->s = s;
    r->slen = slen;
    uint16_t g = rx_new(r, RX_GROUP);
    r->n[g].alt = head;
    rx_cont end = {nullptr, 0, 0, 0, 0};
    bool ok = r->bad ? true : rx_run(r, g, 0, &end);
    embi_free(r, sizeof *r);
    return ok;
}

/* ------------------------------------------------------------------ step */

/* The step and the step base of a number or range input. False when the control has no step constraint (step="any"). */
static bool step_of(const embi_node *n, double *step, double *base)
{
    embi_sb sb = {0};
    double st = 1.0;
    if (attr_utf8(n, "step", &sb)) {
        if (sb.len == 3 && SDL_strncasecmp(sb.data, "any", 3) == 0) {
            embi_sb_free(&sb);
            return false;
        }
        if (sb.len > 0 && float_ok(sb.data, sb.len)) {
            double v = SDL_strtod(embi_sb_cstr(&sb), nullptr);
            if (v > 0) {
                st = v;
            }
        }
    }
    embi_sb_free(&sb);
    bool ok = false;
    double b = attr_num(n, "min", 0.0, &ok);
    if (!ok) {
        b = attr_num(n, "value", 0.0, &ok);
        if (!ok) {
            b = 0.0;
        }
    }
    *step = st;
    *base = b;
    return true;
}

static bool step_mismatch(const embi_node *n, double x)
{
    double st, base;
    if (!step_of(n, &st, &base)) {
        return false;
    }
    double q = (x - base) / st;
    double r = q - SDL_floor(q + 0.5);
    double tol = 1e-9 * (SDL_fabs(q) > 1.0 ? SDL_fabs(q) : 1.0);
    return SDL_fabs(r) > tol;
}


/* ------------------------------------------------------------------ barring and validity */

/* Whether n is barred from constraint validation (never validated: disabled, read-only text, hidden, buttons that do not
 * submit, datalist descendants). */
static bool barred(const embi_node *n)
{
    if (embi_pf_disabled((const embi_element *)(uintptr_t)n) || in_datalist(n)) {
        return true;
    }
    if (is_input(n)) {
        int t = input_type_of(n);
        if (t == IT_HIDDEN || t == IT_RESET || t == IT_BUTTON || t == IT_IMAGE || t == IT_FILE) {
            return t == IT_FILE ? false : true;
        }
        if ((text_like(t) || number_like(t)) && has_attr(n, "readonly")) {
            return true;
        }
        return false;
    }
    if (is_textarea(n)) {
        return has_attr(n, "readonly");
    }
    if (is_button(n)) {
        return button_kind(n) != IT_SUBMIT;
    }
    return false;
}

typedef struct vflags {
    bool missing;
    bool type;
    bool too_long;
    bool too_short;
    bool under;
    bool over;
    bool custom;
    bool pattern;
    bool step;
    bool bad; /* badInput: a number field holds text that is not a valid number */
} vflags;

static bool email_ok(const char *s, uint32_t n)
{
    uint32_t at = n;
    uint32_t ats = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (ascii_space((uint8_t)s[i])) {
            return false;
        }
        if (s[i] == '@') {
            ats++;
            at = i;
        }
    }
    return ats == 1 && at > 0 && at + 1 < n;
}

static bool url_ok(const char *s, uint32_t n)
{
    uint32_t i = 0;
    if (n == 0 || !((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z'))) {
        return false;
    }
    for (i = 1; i < n && s[i] != ':'; i++) {
        char ch = s[i];
        bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '+' ||
                  ch == '-' || ch == '.';
        if (!ok) {
            return false;
        }
    }
    if (i >= n) {
        return false;
    }
    for (uint32_t k = 0; k < n; k++) {
        if (ascii_space((uint8_t)s[k])) {
            return false;
        }
    }
    return true;
}

static void validity_of(embi_node *n, vflags *f)
{
    *f = (vflags){0};
    if (barred(n)) {
        return; /* not a candidate for constraint validation: no flags */
    }
    embi_sb v = {0};
    value_sb(n, &v);
    const embi_ctl *s = ctl_peek(n);
    bool dirty = s != nullptr && s->value_dirty;
    uint32_t len16 = utf16_len(v.data != nullptr ? v.data : "", v.len);
    bool required = has_attr(n, "required");
    if (is_input(n)) {
        int t = input_type_of(n);
        if (t == IT_CHECKBOX) {
            f->missing = required && !checked_of(n);
        } else if (t == IT_RADIO) {
            bool any = checked_of(n);
            if (!any && required) {
                const embi_attr *na = attr_of(n, "name");
                if (na != nullptr && na->value.len > 0) {
                    embi_node *root = tree_root(n);
                    embi_node *f0 = owner_form(n);
                    for (embi_node *x = root; x != nullptr && !any; x = next_in(x, root)) {
                        if (is_radio(x) && x != n) {
                            const embi_attr *xa = attr_of(x, "name");
                            if (xa != nullptr && dstr_eq(xa->value, na->value) && owner_form(x) == f0 && checked_of(x)) {
                                any = true;
                            }
                        }
                    }
                }
            }
            f->missing = required && !any;
        } else if (value_mode(t) && t != IT_FILE) {
            f->missing = required && v.len == 0;
        }
        if (text_like(t) && dirty) {
            uint32_t max = 0;
            uint32_t min = 0;
            if (attr_len(n, "maxlength", &max) && len16 > max) {
                f->too_long = true;
            }
            if (attr_len(n, "minlength", &min) && v.len > 0 && len16 < min) {
                f->too_short = true;
            }
        }
        if (v.len > 0 && t == IT_EMAIL && !email_ok(v.data, v.len)) {
            f->type = true;
        }
        if (v.len > 0 && t == IT_URL && !url_ok(v.data, v.len)) {
            f->type = true;
        }
        if ((t == IT_TEXT || t == IT_SEARCH || t == IT_TEL || t == IT_URL || t == IT_EMAIL || t == IT_PASSWORD) && v.len > 0) {
            embi_sb pat = {0};
            if (attr_utf8(n, "pattern", &pat) && pat.len > 0) {
                bool valid = false;
                bool m = pattern_match(pat.data, pat.len, v.data, v.len, &valid);
                f->pattern = valid && !m;
            }
            embi_sb_free(&pat);
        }
        if (t == IT_NUMBER) {
            f->bad = number_bad_input(n);
        }
        if (number_like(t) && v.len > 0 && float_ok(v.data, v.len)) {
            double x = SDL_strtod(embi_sb_cstr(&v), nullptr);
            f->step = step_mismatch(n, x);
            bool ok_min = false;
            bool ok_max = false;
            double mn = attr_num(n, "min", 0.0, &ok_min);
            double mx = attr_num(n, "max", 0.0, &ok_max);
            if (ok_min && x < mn) {
                f->under = true;
            }
            if (ok_max && x > mx) {
                f->over = true;
            }
        }
    } else if (is_textarea(n)) {
        f->missing = required && v.len == 0;
        uint32_t max = 0;
        uint32_t min = 0;
        if (dirty && attr_len(n, "maxlength", &max) && len16 > max) {
            f->too_long = true;
        }
        if (dirty && attr_len(n, "minlength", &min) && v.len > 0 && len16 < min) {
            f->too_short = true;
        }
    } else if (is_select(n)) {
        bool any = false;
        if (select_multiple(n)) {
            for (embi_node *x = n->first_child; x != nullptr && !any; x = next_in(x, n)) {
                any = is_option(x) && option_raw(x);
            }
        } else {
            embi_node *o = single_selected(n);
            if (o != nullptr) {
                embi_sb ov = {0};
                option_value(o, &ov);
                any = ov.len > 0; /* the placeholder (an empty value) does not count */
                embi_sb_free(&ov);
            }
        }
        f->missing = required && !any;
    }
    if (s != nullptr && s->custom.len > 0) {
        f->custom = true;
    }
    embi_sb_free(&v);
}

static bool valid_of(embi_node *n)
{
    vflags f;
    validity_of(n, &f);
    return !(f.missing || f.type || f.too_long || f.too_short || f.under || f.over || f.custom || f.pattern || f.step ||
             f.bad);
}

/* The validation message of a control (the custom message when there is one). */
static void message_of(embi_node *n, embi_sb *out)
{
    if (valid_of(n)) {
        return;
    }
    const embi_ctl *s = ctl_peek(n);
    if (s != nullptr && s->custom.len > 0) {
        embi_sb_append(out, (embi_str){s->custom.data, s->custom.len});
        return;
    }
    vflags f;
    validity_of(n, &f);
    if (f.bad) { /* Chromium reports badInput before any other flag */
        embi_sb_append(out, EMBI_STR("Please enter a number."));
    } else if (f.missing) {
        embi_sb_append(out, EMBI_STR("Please fill out this field."));
    } else if (f.too_long) {
        embi_sb_append(out, EMBI_STR("The value is too long."));
    } else if (f.too_short) {
        embi_sb_append(out, EMBI_STR("The value is too short."));
    } else if (f.under) {
        embi_sb_append(out, EMBI_STR("The value must be greater than or equal to the minimum."));
    } else if (f.over) {
        embi_sb_append(out, EMBI_STR("The value must be less than or equal to the maximum."));
    } else if (f.step) {
        embi_sb_append(out, EMBI_STR("Please enter a valid value."));
    } else {
        embi_sb_append(out, EMBI_STR("Please match the requested format."));
    }
}

static bool will_validate(embi_node *n)
{
    return !barred(n) && (is_input(n) || is_textarea(n) || is_select(n) || is_button(n));
}

/* ------------------------------------------------------------------ JS values of the validity state */

static embi_js_value validity_new(embi_bind_ctx *c, embi_node *n)
{
    embi_js_value v = embi_bind_other_value(c, EMBI_IF_ValidityState, n, nullptr, 0);
    embi_bind_set_ref(embi_bind_obj_of(v), embi_bind_node_value(c, n));
    return v;
}

static embi_js_value filelist_new(embi_bind_ctx *c)
{
    return embi_bind_other_value(c, EMBI_IF_FileList, c, nullptr, 0);
}

/* ------------------------------------------------------------------ events */

/* Fires the input event of a control (an InputEvent with inputType, data and isComposing). */
static void fire_input_event(embi_bind_ctx *c, embi_node *target, const char *input_type, embi_str data, bool composing)
{
    embi_js_value pl = embi_bind_object_new(c);
    embi_bind_pin(c, pl);
    embi_bind_object_set_c(c, pl, "inputType", embi_bind_utf8_value(c, (embi_str){input_type, (uint32_t)SDL_strlen(input_type)}));
    if (data.len > 0) {
        embi_bind_object_set_c(c, pl, "data", embi_bind_utf8_value(c, data));
    }
    embi_bind_object_set_c(c, pl, "isComposing", embi_js_from_bool(composing));
    embi_js_value ev =
        embi_impl_event_make(c, EMBI_IF_InputEvent, "input", EMBI_EVF_TRUSTED | EMBI_EVF_BUBBLES | EMBI_EVF_COMPOSED, pl);
    if (ev != EMBI_JS_EXCEPTION) {
        (void)embi_impl_dispatch_value(c, embi_impl_node_rec(c, target), ev);
    } else {
        embi_js_exception_clear(c->rt);
    }
}

/* Fires a trusted plain Event that bubbles at a control: HTML fires "change" with bubbles true, "input" (outside text
 * editing) with bubbles and composed true, and "select" with bubbles true. Frameworks that delegate these events to the
 * root (Svelte 5 delegates change and input) see them only when they bubble. */
static void fire_bubbling(embi_bind_ctx *c, embi_node *target, const char *type)
{
    const bool composed = SDL_strcmp(type, "input") == 0;
    embi_js_value ev = embi_impl_event_make(c, EMBI_IF_Event, type,
                                            EMBI_EVF_TRUSTED | EMBI_EVF_BUBBLES | (composed ? EMBI_EVF_COMPOSED : 0u),
                                            EMBI_JS_UNDEFINED);
    if (ev == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(c->rt);
        return;
    }
    embi_impl_fire(c, embi_impl_node_rec(c, target), type, ev);
}

/* ------------------------------------------------------------------ text editing */

/* Whether n takes typed text now: a text field (text-like or number input) or a textarea that is neither disabled nor
 * read-only. */
static bool editable_now(const embi_node *n)
{
    if (is_textarea(n)) {
        return !embi_pf_disabled(el_of(n)) && !has_attr(n, "readonly");
    }
    if (!is_input(n)) {
        return false;
    }
    int t = input_type_of(n);
    return text_field(t) && !embi_pf_disabled(el_of(n)) && !has_attr(n, "readonly");
}

/* Whether n has a text selection the user and select() work with (the text fields and textarea). */
static bool selectable(const embi_node *n)
{
    if (is_textarea(n)) {
        return true;
    }
    return is_input(n) && text_field(input_type_of(n));
}

/* Whether the selection API applies to n (selectionStart, selectionEnd, selectionDirection, setSelectionRange): textarea and
 * the input types of has_selection. Email and number fields have a selection, but the API reads null and throws there. */
static bool selection_api(const embi_node *n)
{
    if (is_textarea(n)) {
        return true;
    }
    return is_input(n) && has_selection(input_type_of(n));
}

/* The maxlength of a control in UTF-16 units, 0 for none (maxlength does not apply to number). */
static uint32_t max16_of(const embi_node *n)
{
    if (is_input(n) && input_type_of(n) == IT_NUMBER) {
        return 0;
    }
    uint32_t m = 0;
    if (attr_len(n, "maxlength", &m)) {
        return m;
    }
    return 0;
}

static bool edit_run(embi_bind_ctx *c, embi_node *target, const char *input_type, embi_str data, bool composing);

bool embi_forms_edit(embi_bind_ctx *c, embi_node *target, const char *input_type, embi_str data, bool composing)
{
    if (data.len == 0 || !is_input(target) || input_type_of(target) != IT_NUMBER) {
        return edit_run(c, target, input_type, data, composing);
    }
    /* A number field takes digits, '.', ',', '-', '+', 'e' and 'E' only, as Chromium filters them: the other characters of
     * typed, pasted or composed text are dropped, and text with none of them is no edit (no beforeinput, no input). */
    embi_sb kept = {0};
    for (uint32_t i = 0; i < data.len; i++) {
        if (number_char(data.ptr[i])) {
            embi_sb_append_char(&kept, data.ptr[i]);
        }
    }
    bool changed = kept.len > 0 && edit_run(c, target, input_type, (embi_str){kept.data, kept.len}, composing);
    embi_sb_free(&kept);
    return changed;
}

static bool edit_run(embi_bind_ctx *c, embi_node *target, const char *input_type, embi_str data, bool composing)
{
    embi_bind_scope sc = embi_bind_enter(c);
    bool changed = false;
    if (!editable_now(target)) {
        (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
        return false;
    }
    embi_ctl *s0 = text_state(c, target);
    if (s0 == nullptr) {
        (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
        return false;
    }
    bool is_comp = composing || SDL_strcmp(input_type, "insertCompositionText") == 0;
    /* beforeinput: the data is NUL-terminated for the dispatch; nothing is passed for deletions. */
    embi_sb d = {0};
    if (data.len > 0) {
        embi_sb_append(&d, data);
    }
    bool proceed = embi_impl_input_before(c, target, input_type, d.len > 0 ? embi_sb_cstr(&d) : nullptr, is_comp);
    if (!proceed || !editable_now(target)) {
        embi_sb_free(&d);
        (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
        return false;
    }
    embi_ctl *s = text_state(c, target);
    if (s == nullptr) {
        embi_sb_free(&d);
        (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
        return false;
    }
    uint32_t max16 = max16_of(target);
    embi_sb text = {0};
    if (data.len > 0) {
        embi_sb_append(&text, data);
    }
    if (!is_textarea(target) && (is_comp || SDL_strcmp(input_type, "insertText") == 0 ||
                                 SDL_strcmp(input_type, "insertFromPaste") == 0)) {
        /* a single-line control keeps no line breaks, pasted or typed or composed */
        uint32_t w = 0;
        for (uint32_t i = 0; i < text.len; i++) {
            if (text.data[i] != 10 && text.data[i] != 13) {
                text.data[w++] = text.data[i];
            }
        }
        text.len = w;
    }
    bool pushed = false;
    if (!is_comp || !s->comp_on) {
        snap_push(&s->undo, &s->ed); /* one undo step per edit, and per whole composition */
        pushed = true;
        for (uint32_t i = 0; i < s->redo.len; i++) {
            embi_sb_free(&s->redo.data[i].text);
        }
        s->redo.len = 0;
    }
    if (is_comp) {
        /* A composition text replaces the range of the previous update of the same composition. */
        if (!s->comp_on) {
            uint32_t a16 = 0;
            uint32_t b16 = 0;
            embi_edit_get_selection(&s->ed, &a16, &b16, nullptr);
            s->comp_from = embi_edit_to_byte(&s->ed, a16);
            s->comp_to = embi_edit_to_byte(&s->ed, b16);
            s->comp_on = true;
        }
        embi_edit_set_selection(&s->ed, embi_edit_to16(&s->ed, s->comp_from), embi_edit_to16(&s->ed, s->comp_to),
                                EMBI_EDIT_DIR_FORWARD);
        changed = embi_edit_insert(&s->ed, (embi_str){text.data, text.len}, max16);
        s->comp_to = s->ed.focus; /* the caret is after the inserted text */
    } else if (SDL_strcmp(input_type, "insertText") == 0 || SDL_strcmp(input_type, "insertFromPaste") == 0 ||
               SDL_strcmp(input_type, "insertLineBreak") == 0 || SDL_strcmp(input_type, "insertReplacementText") == 0) {
        if (SDL_strcmp(input_type, "insertLineBreak") == 0 && !is_textarea(target)) {
            text.len = 0;
        }
        changed = embi_edit_insert(&s->ed, (embi_str){text.data, text.len}, max16);
        s->comp_on = false;
    } else if (SDL_strcmp(input_type, "deleteContentBackward") == 0 || SDL_strcmp(input_type, "deleteByCut") == 0) {
        changed = embi_edit_delete(&s->ed, false);
        s->comp_on = false;
    } else if (SDL_strcmp(input_type, "deleteContentForward") == 0) {
        changed = embi_edit_delete(&s->ed, true);
        s->comp_on = false;
    } else if (SDL_strcmp(input_type, "deleteWordBackward") == 0) {
        changed = embi_edit_delete_word(&s->ed, false);
        s->comp_on = false;
    } else if (SDL_strcmp(input_type, "deleteWordForward") == 0) {
        changed = embi_edit_delete_word(&s->ed, true);
        s->comp_on = false;
    }
    embi_sb_free(&text);
    embi_sb_free(&d);
    if (pushed && !changed && s->undo.len > 0) {
        embi_sb_free(&s->undo.data[s->undo.len - 1].text);
        s->undo.len--;
    }
    s->caret_ms = SDL_GetTicks();
    if (changed) {
        s->uncommitted = true;
        s->value_dirty = true;
        fire_input_event(c, target, input_type,
                         is_comp || SDL_strcmp(input_type, "insertText") == 0 ? data : (embi_str){nullptr, 0}, is_comp);
    }
    (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
    return changed;
}

void embi_forms_composition_start(embi_bind_ctx *c, embi_node *target)
{
    embi_ctl *s = is_textarea(target) || is_input(target) ? ctl_make(c, target) : nullptr;
    if (s != nullptr) {
        s->comp_on = false;
    }
}

void embi_forms_composition_end(embi_bind_ctx *c, embi_node *target)
{
    embi_ctl *s = is_textarea(target) || is_input(target) ? ctl_make(c, target) : nullptr;
    if (s != nullptr) {
        s->comp_on = false;
    }
}

bool embi_forms_selected_text(embi_node *target, embi_sb *out)
{
    if (!selectable(target)) {
        return false;
    }
    /* The selection needs the edit state, which the context creates on first use; the copy reads the synced text. */
    embi_bind_obj *bo = target->wrapper;
    if (bo == nullptr || bo->owned == nullptr) {
        return false;
    }
    embi_ctl *s = bo->owned;
    embi_str sel = embi_edit_selected(&s->ed);
    if (sel.len == 0) {
        return false;
    }
    embi_sb_append(out, sel);
    return true;
}

static bool select_key(embi_bind_ctx *c, embi_node *sel, const char *key, bool alt);
static bool range_key(embi_bind_ctx *c, embi_node *n, const char *key);
static void number_arrow(embi_bind_ctx *c, embi_node *n, int dir);

/* Moves or deletes by a key. */
bool embi_forms_key(embi_bind_ctx *c, embi_node *target, const char *key, bool ctrl, bool shift, bool alt)
{
    embi_bind_scope sc = embi_bind_enter(c);
    bool taken = false;
    bool multiline = is_textarea(target);
    if (is_select(target)) {
        taken = select_key(c, target, key, alt);
        (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
        return taken;
    }
    if (is_input(target) && input_type_of(target) == IT_RANGE) {
        taken = range_key(c, target, key);
        (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
        return taken;
    }
    if (!(multiline || selectable(target))) {
        (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
        return false;
    }
    bool disabled = embi_pf_disabled(el_of(target));
    bool locked = disabled || has_attr(target, "readonly");
    embi_ctl *ks = text_state(c, target);
    if (ks != nullptr) {
        ks->caret_ms = SDL_GetTicks();
    }
    if (ctrl && !alt && (SDL_strcmp(key, "z") == 0 || SDL_strcmp(key, "Z") == 0 || SDL_strcmp(key, "y") == 0 ||
                         SDL_strcmp(key, "Y") == 0)) {
        if (!locked) {
            bool redo = key[0] == 'y' || key[0] == 'Y' || shift;
            (void)embi_forms_undo(c, target, redo);
        }
        taken = true;
    } else if (SDL_strcmp(key, "a") == 0 || SDL_strcmp(key, "A") == 0) {
        if (ctrl && !alt && !shift) {
            embi_ctl *s = text_state(c, target);
            if (s != nullptr) {
                embi_edit_select_all(&s->ed);
            }
            taken = true;
        }
    } else if ((SDL_strcmp(key, "ArrowUp") == 0 || SDL_strcmp(key, "ArrowDown") == 0) && multiline) {
        embi_ctl *s = text_state(c, target);
        if (s != nullptr) {
            embi_edit_move(&s->ed, SDL_strcmp(key, "ArrowUp") == 0 ? EMBI_EDIT_LINE_UP : EMBI_EDIT_LINE_DOWN, shift);
        }
        taken = true;
    } else if ((SDL_strcmp(key, "ArrowUp") == 0 || SDL_strcmp(key, "ArrowDown") == 0) && is_input(target) &&
               input_type_of(target) == IT_NUMBER) {
        if (!locked) {
            number_arrow(c, target, SDL_strcmp(key, "ArrowUp") == 0 ? 1 : -1); /* the spin button's step */
        }
        taken = true;
    } else if ((SDL_strcmp(key, "ArrowUp") == 0 || SDL_strcmp(key, "ArrowDown") == 0) && !multiline) {
        embi_ctl *s = text_state(c, target);
        if (s != nullptr) {
            embi_edit_move(&s->ed, SDL_strcmp(key, "ArrowUp") == 0 ? EMBI_EDIT_TEXT_START : EMBI_EDIT_TEXT_END, shift);
        }
        taken = true;
    } else if (SDL_strcmp(key, "ArrowLeft") == 0 || SDL_strcmp(key, "ArrowRight") == 0 || SDL_strcmp(key, "Home") == 0 ||
               SDL_strcmp(key, "End") == 0) {
        embi_edit_motion m = EMBI_EDIT_CHAR_LEFT;
        if (SDL_strcmp(key, "ArrowLeft") == 0) {
            m = ctrl ? EMBI_EDIT_WORD_LEFT : EMBI_EDIT_CHAR_LEFT;
        } else if (SDL_strcmp(key, "ArrowRight") == 0) {
            m = ctrl ? EMBI_EDIT_WORD_RIGHT : EMBI_EDIT_CHAR_RIGHT;
        } else if (SDL_strcmp(key, "Home") == 0) {
            m = ctrl || !multiline ? EMBI_EDIT_TEXT_START : EMBI_EDIT_LINE_START;
        } else {
            m = ctrl || !multiline ? EMBI_EDIT_TEXT_END : EMBI_EDIT_LINE_END;
        }
        embi_ctl *s = text_state(c, target);
        if (s != nullptr) {
            embi_edit_move(&s->ed, m, shift);
        }
        taken = true;
    } else if (SDL_strcmp(key, "Backspace") == 0 || SDL_strcmp(key, "Delete") == 0) {
        if (!locked) {
            bool forward = SDL_strcmp(key, "Delete") == 0;
            const char *t = forward ? (ctrl ? "deleteWordForward" : "deleteContentForward")
                                    : (ctrl ? "deleteWordBackward" : "deleteContentBackward");
            (void)embi_forms_edit(c, target, t, (embi_str){nullptr, 0}, false);
        }
        taken = true;
    }
    (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
    return taken;
}


/* ------------------------------------------------------------------ commit, undo and redo */

/* The change event of a text control whose value the user changed since the last commit (blur, Enter, submission). */
void embi_forms_commit(embi_bind_ctx *c, embi_node *target)
{
    const embi_ctl *peek = ctl_peek(target);
    if (peek == nullptr || !peek->uncommitted) {
        return;
    }
    embi_ctl *s = ctl_make(c, target);
    if (s == nullptr) {
        return;
    }
    s->uncommitted = false;
    embi_bind_scope sc = embi_bind_enter(c);
    fire_bubbling(c, target, "change");
    (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
}

void embi_forms_blur(embi_bind_ctx *c, embi_node *old_focus)
{
    if (old_focus == nullptr || !(is_input(old_focus) || is_textarea(old_focus) || is_select(old_focus))) {
        return;
    }
    embi_forms_commit(c, old_focus);
    embi_ctl *s = ctl_make(c, old_focus);
    if (s != nullptr) {
        s->focused = false;
        s->open = false;
        s->dragging = false;
        s->comp_on = false;
    }
}

void embi_forms_focus(embi_bind_ctx *c, embi_node *new_focus)
{
    if (new_focus == nullptr || !(is_input(new_focus) || is_textarea(new_focus) || is_select(new_focus))) {
        return;
    }
    embi_ctl *s = ctl_make(c, new_focus);
    if (s != nullptr) {
        s->focused = true;
        s->caret_ms = SDL_GetTicks();
    }
}

bool embi_forms_undo(embi_bind_ctx *c, embi_node *target, bool redo)
{
    embi_bind_scope sc = embi_bind_enter(c);
    bool done = false;
    embi_ctl *s = editable_now(target) ? text_state(c, target) : nullptr;
    if (s != nullptr) {
        embi_snapvec *from = redo ? &s->redo : &s->undo;
        embi_snapvec *to = redo ? &s->undo : &s->redo;
        const char *it = redo ? "historyRedo" : "historyUndo";
        if (from->len > 0 && embi_impl_input_before(c, target, it, nullptr, false) && editable_now(target)) {
            snap_push(to, &s->ed);
            embi_snap sn = from->data[from->len - 1];
            from->len--;
            (void)embi_edit_set_value(&s->ed, (embi_str){sn.text.data, sn.text.len});
            s->ed.anchor = sn.anchor <= s->ed.text.len ? sn.anchor : s->ed.text.len;
            s->ed.focus = sn.focus <= s->ed.text.len ? sn.focus : s->ed.text.len;
            embi_sb_free(&sn.text);
            s->value_dirty = true;
            s->comp_on = false;
            s->uncommitted = true;
            s->caret_ms = SDL_GetTicks();
            fire_input_event(c, target, it, (embi_str){nullptr, 0}, false);
            done = true;
        }
    }
    (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
    return done;
}

/* ------------------------------------------------------------------ select, range, listbox */

/* The option labels of a select as the painter shows them. */
static void option_label(const embi_node *opt, embi_sb *out)
{
    embi_sb l = {0};
    if (attr_utf8(opt, "label", &l) && l.len > 0) {
        embi_sb_append(out, (embi_str){l.data, l.len});
    } else {
        collapsed_text(opt, out);
    }
    embi_sb_free(&l);
}

/* Makes option index idx (among the select's options) the selection of a single select, or toggles/sets it in a multiple one,
 * and fires input then change when the selection changed. */
static bool select_pick(embi_bind_ctx *c, embi_node *sel, int32_t idx, bool toggle)
{
    uint32_t n = select_options(sel, nullptr, 0);
    if (idx < 0 || (uint32_t)idx >= n) {
        return false;
    }
    embi_node **opts = embi_alloc(n * sizeof *opts);
    if (opts == nullptr) {
        return false;
    }
    (void)select_options(sel, opts, n);
    if (option_disabled(opts[idx])) {
        embi_free(opts, n * sizeof *opts);
        return false;
    }
    bool multi = select_multiple(sel);
    bool changed = false;
    for (uint32_t i = 0; i < n; i++) {
        bool was = option_selected_effective(opts[i], sel);
        bool now = multi ? (i == (uint32_t)idx ? (toggle ? !was : true) : (toggle ? was : false)) : i == (uint32_t)idx;
        if (was != now) {
            changed = true;
        }
        embi_ctl *os = ctl_make(c, opts[i]);
        if (os != nullptr) {
            os->selected = now;
            os->selected_dirty = true;
        }
    }
    embi_free(opts, n * sizeof *opts);
    if (changed) {
        select_restyle(c, sel);
    }
    if (changed) {
        embi_bind_scope sc = embi_bind_enter(c);
        fire_bubbling(c, sel, "input");
        fire_bubbling(c, sel, "change");
        (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
    }
    return changed;
}

/* The index among the select's options of the displayed (first selected) option, -1 for none. */
static int32_t select_index(embi_node *sel)
{
    int32_t i = 0;
    embi_node *picked = select_multiple(sel) ? nullptr : single_selected(sel);
    for (embi_node *x = sel->first_child; x != nullptr; x = next_in(x, sel)) {
        if (!is_option(x)) {
            continue;
        }
        if (select_multiple(sel) ? option_raw(x) : x == picked) {
            return i;
        }
        i++;
    }
    return -1;
}

/* The next enabled option from idx in direction dir (+1, -1), or idx when none. */
static int32_t select_step(embi_node *sel, int32_t idx, int32_t dir)
{
    uint32_t n = select_options(sel, nullptr, 0);
    embi_node **opts = n > 0 ? embi_alloc(n * sizeof *opts) : nullptr;
    int32_t out = idx;
    if (opts != nullptr) {
        (void)select_options(sel, opts, n);
        for (int32_t i = idx + dir; i >= 0 && (uint32_t)i < n; i += dir) {
            if (!option_disabled(opts[i])) {
                out = i;
                break;
            }
        }
        embi_free(opts, n * sizeof *opts);
    }
    return out;
}

static bool is_listbox(embi_node *sel)
{
    uint32_t size = 0;
    return select_multiple(sel) || (attr_len(sel, "size", &size) && size > 1);
}

static bool select_key(embi_bind_ctx *c, embi_node *sel, const char *key, bool alt)
{
    if (embi_pf_disabled(el_of(sel))) {
        return false;
    }
    embi_ctl *s = ctl_make(c, sel);
    if (s == nullptr) {
        return false;
    }
    uint32_t n = select_options(sel, nullptr, 0);
    bool down = SDL_strcmp(key, "ArrowDown") == 0, up = SDL_strcmp(key, "ArrowUp") == 0;
    bool home = SDL_strcmp(key, "Home") == 0, end = SDL_strcmp(key, "End") == 0;
    bool list = is_listbox(sel);
    if (s->open && !list) {
        if (down || up || home || end) {
            int32_t h = s->hot;
            if (h < 0) {
                h = select_index(sel);
            }
            if (home) {
                h = select_step(sel, -1, 1);
            } else if (end) {
                h = select_step(sel, (int32_t)n, -1);
            } else {
                h = select_step(sel, h, down ? 1 : -1);
            }
            s->hot = h;
            return true;
        }
        if (SDL_strcmp(key, " ") == 0 || SDL_strcmp(key, "Enter") == 0) {
            s->open = false;
            if (s->hot >= 0) {
                (void)select_pick(c, sel, s->hot, false);
            }
            return true;
        }
        if (SDL_strcmp(key, "Escape") == 0) {
            s->open = false;
            return true;
        }
        return false;
    }
    if (!list && (SDL_strcmp(key, " ") == 0 || ((down || up) && alt))) {
        s->open = true;
        s->hot = select_index(sel);
        return true;
    }
    if (down || up || home || end) {
        int32_t cur = select_index(sel);
        int32_t to = cur;
        if (home) {
            to = select_step(sel, -1, 1);
        } else if (end) {
            to = select_step(sel, (int32_t)n, -1);
        } else {
            to = select_step(sel, cur, down ? 1 : -1);
        }
        if (to != cur && to >= 0) {
            (void)select_pick(c, sel, to, false);
            s->hot = to;
            if (list) {
                uint32_t rows = 4;
                (void)attr_len(sel, "size", &rows);
                if (rows == 0) {
                    rows = 4;
                }
                if ((uint32_t)to < s->first_row) {
                    s->first_row = (uint32_t)to;
                } else if ((uint32_t)to >= s->first_row + rows) {
                    s->first_row = (uint32_t)to + 1u - rows;
                }
            }
        }
        return true;
    }
    return false;
}

/* The numeric range of a range input. */
static void range_bounds(const embi_node *n, double *mn, double *mx)
{
    *mn = attr_num(n, "min", 0.0, nullptr);
    *mx = attr_num(n, "max", 100.0, nullptr);
    if (*mx < *mn) {
        *mx = *mn;
    }
}

/* Sets the value of a range input to v (snapped to the step, clamped), firing input when it changed. */
static bool range_set(embi_bind_ctx *c, embi_node *n, double v, bool fire)
{
    double mn, mx;
    range_bounds(n, &mn, &mx);
    double st, base;
    if (step_of(n, &st, &base)) {
        v = base + SDL_floor((v - base) / st + 0.5) * st;
    }
    if (v < mn) {
        v = mn;
    }
    if (v > mx) {
        v = mx;
    }
    embi_sb cur = {0};
    value_sb(n, &cur);
    double old = cur.len > 0 ? SDL_strtod(embi_sb_cstr(&cur), nullptr) : (mn + mx) / 2.0;
    embi_sb_free(&cur);
    if (old == v) {
        return false;
    }
    embi_sb nv = {0};
    fmt_number(v, &nv);
    (void)set_value(c, n, (embi_str){nv.data, nv.len});
    embi_sb_free(&nv);
    embi_ctl *s = ctl_make(c, n);
    if (s != nullptr) {
        s->uncommitted = true;
    }
    if (fire) {
        embi_bind_scope sc = embi_bind_enter(c);
        fire_bubbling(c, n, "input");
        (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
    }
    return true;
}

static bool range_key(embi_bind_ctx *c, embi_node *n, const char *key)
{
    if (embi_pf_disabled(el_of(n))) {
        return false;
    }
    double mn, mx, st = 1.0, base = 0.0;
    range_bounds(n, &mn, &mx);
    if (!step_of(n, &st, &base)) {
        st = (mx - mn) / 100.0 > 0 ? (mx - mn) / 100.0 : 1.0;
    }
    embi_sb cur = {0};
    value_sb(n, &cur);
    double v = cur.len > 0 ? SDL_strtod(embi_sb_cstr(&cur), nullptr) : (mn + mx) / 2.0;
    embi_sb_free(&cur);
    double to = v;
    if (SDL_strcmp(key, "ArrowRight") == 0 || SDL_strcmp(key, "ArrowUp") == 0) {
        to = v + st;
    } else if (SDL_strcmp(key, "ArrowLeft") == 0 || SDL_strcmp(key, "ArrowDown") == 0) {
        to = v - st;
    } else if (SDL_strcmp(key, "PageUp") == 0) {
        to = v + (mx - mn) / 10.0;
    } else if (SDL_strcmp(key, "PageDown") == 0) {
        to = v - (mx - mn) / 10.0;
    } else if (SDL_strcmp(key, "Home") == 0) {
        to = mn;
    } else if (SDL_strcmp(key, "End") == 0) {
        to = mx;
    } else {
        return false;
    }
    if (range_set(c, n, to, true)) {
        embi_bind_scope sc = embi_bind_enter(c);
        embi_ctl *s = ctl_make(c, n);
        if (s != nullptr) {
            s->uncommitted = false;
        }
        fire_bubbling(c, n, "change");
        (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
    }
    return true;
}

/* The digits after the point of a number string, less its exponent (0 for an integer, at most 17). */
static int frac_digits(const char *p, uint32_t n)
{
    int d = 0;
    uint32_t i = 0;
    while (i < n && p[i] != '.' && p[i] != 'e' && p[i] != 'E') {
        i++;
    }
    if (i < n && p[i] == '.') {
        for (i++; i < n && p[i] >= '0' && p[i] <= '9'; i++) {
            d++;
        }
    }
    if (i < n && (p[i] == 'e' || p[i] == 'E')) {
        i++;
        bool neg = i < n && p[i] == '-';
        if (i < n && (p[i] == '-' || p[i] == '+')) {
            i++;
        }
        int e = 0;
        for (; i < n && p[i] >= '0' && p[i] <= '9' && e < 1000; i++) {
            e = e * 10 + (p[i] - '0');
        }
        d += neg ? e : -e;
    }
    return d < 0 ? 0 : (d > 17 ? 17 : d);
}

/* ArrowUp (dir 1) and ArrowDown (dir -1) in a number field step its value as Chromium's spin button does
 * (InputType::StepUpFromLayoutObject): step "any" steps by 1; a value that is not a number starts from 0, kept so that the
 * step lands within min and max; a value below min (up) or above max (down) goes to that limit and one at the far limit does
 * not move; a value off the step snaps to the step in the key's direction; the result stays within min and max (on the step).
 * The result is rounded to the decimals of the step, the step base and the value, as Chromium's decimal arithmetic gives
 * (0.1 + 0.2 is 0.3). When the value changed, input and then change fire, as in Chromium. */
static void number_arrow(embi_bind_ctx *c, embi_node *n, int dir)
{
    double st = 1.0;
    double base = 0.0;
    bool aligned = step_of(n, &st, &base);
    if (!aligned) {
        st = 1.0;
    }
    bool has_min = false;
    bool has_max = false;
    double mn = attr_num(n, "min", 0.0, &has_min);
    double mx = attr_num(n, "max", 0.0, &has_max);
    if (has_min && has_max && mx < mn) {
        return;
    }
    embi_sb cur = {0};
    value_sb(n, &cur);
    bool finite = cur.len > 0 && float_ok(cur.data, cur.len);
    double v = finite ? SDL_strtod(embi_sb_cstr(&cur), nullptr) : 0.0;
    if (!finite) {
        double next = st * (double)dir;
        if (has_min && v < mn - next) {
            v = mn - next;
        }
        if (has_max && v > mx - next) {
            v = mx - next;
        }
    }
    double to = v;
    if (dir > 0 && has_min && v < mn) {
        to = mn;
    } else if (dir < 0 && has_max && v > mx) {
        to = mx;
    } else if ((dir > 0 && has_max && v >= mx) || (dir < 0 && has_min && v <= mn)) {
        embi_sb_free(&cur);
        return;
    } else {
        if (aligned && step_mismatch(n, v)) {
            double q = (v - base) / st;
            to = base + (dir > 0 ? SDL_ceil(q) : SDL_floor(q)) * st;
        } else {
            to = v + st * (double)dir;
        }
        if (has_max) {
            double top = aligned ? base + SDL_floor((mx - base) / st + 1e-9) * st : mx;
            to = to > top ? top : to;
        }
        if (has_min) {
            double bot = aligned ? base + SDL_ceil((mn - base) / st - 1e-9) * st : mn;
            to = to < bot ? bot : to;
        }
    }
    int digits = finite ? frac_digits(cur.data, cur.len) : 0;
    static const char *const k_num_attrs[] = {"step", "min", "max", "value"};
    for (uint32_t i = 0; i < sizeof k_num_attrs / sizeof k_num_attrs[0]; i++) {
        embi_sb a = {0};
        if (attr_utf8(n, k_num_attrs[i], &a) && a.len > 0 && float_ok(a.data, a.len)) {
            int d = frac_digits(a.data, a.len);
            digits = d > digits ? d : digits;
        }
        embi_sb_free(&a);
    }
    digits = digits > 15 ? 15 : digits;
    if (digits > 0) {
        double p = SDL_pow(10.0, (double)digits);
        if (SDL_fabs(to * p) < 9007199254740992.0) {
            to = SDL_round(to * p) / p;
        }
    } else if (SDL_fabs(to) < 9007199254740992.0) {
        to = SDL_round(to);
    }
    embi_sb nv = {0};
    fmt_number(to, &nv);
    bool same = nv.len == cur.len && memcmp(nv.data, cur.data, nv.len) == 0;
    embi_sb_free(&cur);
    if (!same) {
        (void)set_value(c, n, (embi_str){nv.data, nv.len});
        embi_bind_scope sc = embi_bind_enter(c);
        fire_bubbling(c, n, "input");
        fire_bubbling(c, n, "change");
        (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
    }
    embi_sb_free(&nv);
}

/* ------------------------------------------------------------------ pointer */

/* The geometry of a select popup (and the hit row): the popup lies below the control, or above it when it does not fit. */
static void popup_rect(const embi_node *sel, embi_node **dummy, uint32_t nopts, const float bb[4], float vh, float out[4])
{
    (void)sel;
    (void)dummy;
    float h = (float)nopts * EMBI_CTL_ROW_H;
    out[0] = bb[0];
    out[1] = bb[1] + bb[3];
    out[2] = bb[2];
    out[3] = h;
    if (out[1] + h > vh && bb[1] - h >= 0) {
        out[1] = bb[1] - h;
    }
}

bool embi_forms_popup_press(embi_bind_ctx *c, embi_node *sel, const float bb[4], float vh, float x, float y)
{
    if (sel == nullptr || !is_select(sel)) {
        return false;
    }
    const embi_ctl *peek = ctl_peek(sel);
    if (peek == nullptr || !peek->open) {
        return false;
    }
    embi_ctl *s = ctl_make(c, sel);
    uint32_t n = select_options(sel, nullptr, 0);
    float r[4];
    popup_rect(sel, nullptr, n, bb, vh, r);
    bool inside = x >= r[0] && x < r[0] + r[2] && y >= r[1] && y < r[1] + r[3];
    if (inside && s != nullptr) {
        int32_t row = (int32_t)((y - r[1]) / EMBI_CTL_ROW_H);
        s->open = false;
        (void)select_pick(c, sel, row, false);
        return true;
    }
    if (s != nullptr) {
        s->open = false;
    }
    /* a press on the select itself only closes the popup (it must not reopen it) */
    return x >= bb[0] && x < bb[0] + bb[2] && y >= bb[1] && y < bb[1] + bb[3];
}

bool embi_forms_popup_hover(embi_bind_ctx *c, embi_node *sel, const float bb[4], float vh, float x, float y)
{
    const embi_ctl *peek = sel != nullptr && is_select(sel) ? ctl_peek(sel) : nullptr;
    if (peek == nullptr || !peek->open) {
        return false;
    }
    uint32_t n = select_options(sel, nullptr, 0);
    float r[4];
    popup_rect(sel, nullptr, n, bb, vh, r);
    if (x >= r[0] && x < r[0] + r[2] && y >= r[1] && y < r[1] + r[3]) {
        embi_ctl *s = ctl_make(c, sel);
        int32_t row = (int32_t)((y - r[1]) / EMBI_CTL_ROW_H);
        if (s != nullptr && s->hot != row) {
            s->hot = row;
            return true;
        }
    }
    return false;
}

bool embi_forms_pointer(embi_bind_ctx *c, embi_node *target, int phase, const float bb[4], float x, float y, uint32_t byte,
                        uint32_t clicks, bool shift)
{
    if (target == nullptr) {
        return false;
    }
    embi_bind_scope sc = embi_bind_enter(c);
    bool taken = false;
    if (selectable(target)) {
        embi_ctl *s = text_state(c, target);
        if (s != nullptr) {
            taken = true;
            if (phase == 0) {
                s->dragging = true;
                s->caret_ms = SDL_GetTicks();
                uint32_t b = byte <= s->ed.text.len ? byte : s->ed.text.len;
                if (clicks >= 3) {
                    embi_edit_select_all(&s->ed);
                } else if (clicks == 2) {
                    s->ed.anchor = s->ed.focus = b;
                    embi_edit_move(&s->ed, EMBI_EDIT_WORD_LEFT, false);
                    uint32_t a0 = s->ed.focus;
                    s->ed.focus = b;
                    s->ed.anchor = b;
                    embi_edit_move(&s->ed, EMBI_EDIT_WORD_RIGHT, false);
                    s->ed.anchor = a0;
                    s->ed.dir = EMBI_EDIT_DIR_FORWARD;
                } else if (shift) {
                    s->ed.focus = b;
                    s->ed.dir = b >= s->ed.anchor ? EMBI_EDIT_DIR_FORWARD : EMBI_EDIT_DIR_BACKWARD;
                } else {
                    s->ed.anchor = s->ed.focus = b;
                }
            } else if (phase == 1 && s->dragging) {
                uint32_t b = byte <= s->ed.text.len ? byte : s->ed.text.len;
                s->ed.focus = b;
                s->ed.dir = b >= s->ed.anchor ? EMBI_EDIT_DIR_FORWARD : EMBI_EDIT_DIR_BACKWARD;
                s->caret_ms = SDL_GetTicks();
            } else if (phase == 2) {
                s->dragging = false;
            }
        }
    } else if (is_input(target) && input_type_of(target) == IT_RANGE && !embi_pf_disabled(el_of(target))) {
        embi_ctl *s = ctl_make(c, target);
        if (s != nullptr) {
            taken = true;
            if (phase == 0 || (phase == 1 && s->dragging)) {
                s->dragging = true;
                double mn, mx;
                range_bounds(target, &mn, &mx);
                float d = bb[3] < 16.0f ? bb[3] : 16.0f;
                float left = bb[0] + d * 0.5f, span = bb[2] - d;
                float tl = 0.0f, tw = 0.0f, thw = 0.0f;
                if (embi_paint_range_track(embi_bind_layout(c), embi_bind_style(c), el_of(target), bb, &tl, &tw, &thw)) {
                    /* styled parts: the thumb's centre goes under the pointer, within what the styled thumb can travel */
                    left = tl + thw * 0.5f;
                    span = tw - thw;
                }
                double frac = span > 0 ? (double)(x - left) / (double)span : 0.0;
                frac = frac < 0 ? 0 : (frac > 1 ? 1 : frac);
                (void)range_set(c, target, mn + frac * (mx - mn), true);
            } else if (phase == 2 && s->dragging) {
                s->dragging = false;
                if (s->uncommitted) {
                    s->uncommitted = false;
                    fire_bubbling(c, target, "change");
                }
            }
        }
    } else if (is_select(target) && !embi_pf_disabled(el_of(target)) && phase == 0) {
        embi_ctl *s = ctl_make(c, target);
        if (s != nullptr) {
            taken = true;
            if (is_listbox(target)) {
                int32_t row = (int32_t)((y - bb[1] - 2.0f) / EMBI_CTL_ROW_H) + (int32_t)s->first_row;
                (void)select_pick(c, target, row, select_multiple(target));
            } else {
                s->open = !s->open;
                s->hot = select_index(target);
            }
        }
    }
    (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
    return taken;
}

/* The blink phase of the caret of the focused control: 0 shows the caret. */
uint32_t embi_forms_caret_phase(const embi_node *n, uint64_t now_ms)
{
    const embi_ctl *s = n != nullptr ? ctl_peek(n) : nullptr;
    if (s == nullptr) {
        return 0;
    }
    uint64_t dt = now_ms > s->caret_ms ? now_ms - s->caret_ms : 0;
    return (uint32_t)((dt / 500u) & 1u);
}

/* ------------------------------------------------------------------ state for the painter */

bool embi_forms_paint_state(const embi_node *n, embi_paint_ctl *out)
{
    if (n == nullptr || !embi_is_element(n) || n->extra != EMBI_NS_HTML) {
        return false;
    }
    embi_node *nn = (embi_node *)(uintptr_t)n;
    embi_ctl *s = n->wrapper != nullptr ? ((embi_bind_obj *)n->wrapper)->owned : nullptr;
    if (s == nullptr) {
        /* No state block: nothing was scripted, focused or edited, so for the controls that read text (fields and selects)
         * the painter's attribute fallback (value, placeholder, selected options) is exact, and no shared scratch buffer is
         * needed. The others carry no text and are answered below. */
        int t0 = is_input(n) ? input_type_of(n) : -1;
        bool plain = is_button(n) || t0 == IT_CHECKBOX || t0 == IT_RADIO || t0 == IT_RANGE || t0 == IT_SUBMIT ||
                     t0 == IT_RESET || t0 == IT_BUTTON;
        if (!plain) {
            return false;
        }
    }
    memset(out, 0, sizeof *out);
    out->hot = -1;
    out->disabled = embi_pf_disabled(el_of(n));
    out->readonly = has_attr(n, "readonly");
    embi_sb local_store = {0}; /* a range without a state block */
    embi_sb *store = s != nullptr ? &s->ptext : &local_store;
    store->len = 0;
    if (is_textarea(n) || (is_input(n) && input_type_of(n) != IT_FILE && input_type_of(n) != IT_IMAGE &&
                           input_type_of(n) != IT_HIDDEN && input_type_of(n) != IT_COLOR && value_mode(input_type_of(n)) &&
                           input_type_of(n) != IT_CHECKBOX && input_type_of(n) != IT_RADIO && input_type_of(n) != IT_RANGE &&
                           !(input_type_of(n) == IT_SUBMIT || input_type_of(n) == IT_RESET || input_type_of(n) == IT_BUTTON))) {
        int t = is_input(n) ? input_type_of(n) : IT_TEXT;
        out->kind = is_textarea(n) ? EMBI_PCTL_TEXTAREA : EMBI_PCTL_TEXT;
        out->password = t == IT_PASSWORD;
        out->nowrap = is_textarea(n) && has_attr(n, "wrap") && attr_of(n, "wrap")->value.len == 3 &&
                      dstr_ieq(attr_of(n, "wrap")->value, "off");
        shown_sb(nn, store);
        out->text = store->data != nullptr ? store->data : "";
        out->text_len = store->len;
        if (s != nullptr && selectable(n)) {
            uint32_t a = s->ed.anchor, b = s->ed.focus;
            if (a > store->len) {
                a = store->len;
            }
            if (b > store->len) {
                b = store->len;
            }
            out->sel_a = a < b ? a : b;
            out->sel_b = a < b ? b : a;
            out->caret = b;
            out->comp_on = s->comp_on;
            out->comp_a = s->comp_from < store->len ? s->comp_from : store->len;
            out->comp_b = s->comp_to < store->len ? s->comp_to : store->len;
        } else {
            out->caret = out->sel_a = out->sel_b = store->len;
        }
        embi_sb ph = {0};
        if (attr_utf8(n, "placeholder", &ph) && ph.len > 0) {
            s->pph.len = 0;
            embi_sb_append(&s->pph, (embi_str){ph.data, ph.len});
            out->placeholder = s->pph.data;
            out->placeholder_len = s->pph.len;
        }
        embi_sb_free(&ph);
    } else if (is_input(n) && (input_type_of(n) == IT_CHECKBOX || input_type_of(n) == IT_RADIO)) {
        out->kind = input_type_of(n) == IT_CHECKBOX ? EMBI_PCTL_CHECKBOX : EMBI_PCTL_RADIO;
        out->checked = checked_of(n);
        out->indeterminate = s != nullptr && s->indeterminate && out->kind == EMBI_PCTL_CHECKBOX;
    } else if (is_input(n) && input_type_of(n) == IT_RANGE) {
        out->kind = EMBI_PCTL_RANGE;
        double mn, mx;
        range_bounds(n, &mn, &mx);
        value_sb(nn, store);
        double v = store->len > 0 ? SDL_strtod(embi_sb_cstr(store), nullptr) : (mn + mx) / 2.0;
        out->min = mn;
        out->max = mx;
        out->value = v < mn ? mn : (v > mx ? mx : v);
        embi_sb_free(&local_store);
    } else if (is_input(n) && (input_type_of(n) == IT_SUBMIT || input_type_of(n) == IT_RESET || input_type_of(n) == IT_BUTTON)) {
        out->kind = EMBI_PCTL_BUTTON;
    } else if (is_button(n)) {
        out->kind = EMBI_PCTL_BUTTON;
    } else if (is_select(n)) {
        bool list = is_listbox(nn);
        out->kind = list ? EMBI_PCTL_LISTBOX : EMBI_PCTL_SELECT;
        uint32_t cnt = select_options(nn, nullptr, 0);
        if (s != nullptr) {
            s->popts.len = 0;
            s->plabels.len = 0;
            /* the labels are stored back to back; the option records point into plabels, so it is filled first */
            embi_node **os = cnt > 0 ? embi_alloc(cnt * sizeof *os) : nullptr;
            uint32_t *offs = cnt > 0 ? embi_alloc((cnt + 1u) * sizeof *offs) : nullptr;
            if (os != nullptr && offs != nullptr) {
                (void)select_options(nn, os, cnt);
                for (uint32_t i = 0; i < cnt; i++) {
                    offs[i] = s->plabels.len;
                    option_label(os[i], &s->plabels);
                }
                offs[cnt] = s->plabels.len;
                for (uint32_t i = 0; i < cnt; i++) {
                    embi_paint_opt po;
                    po.label = s->plabels.data != nullptr ? s->plabels.data + offs[i] : "";
                    po.label_len = offs[i + 1] - offs[i];
                    po.selected = option_selected_effective(os[i], nn);
                    po.disabled = option_disabled(os[i]);
                    embi_vec_push(&s->popts, po);
                    if (po.selected && !list && out->text == nullptr) {
                        out->text = po.label;
                        out->text_len = po.label_len;
                    }
                }
            }
            if (os != nullptr) {
                embi_free(os, cnt * sizeof *os);
            }
            if (offs != nullptr) {
                embi_free(offs, (cnt + 1u) * sizeof *offs);
            }
            out->opts = s->popts.data;
            out->nopts = s->popts.len;
            out->open = s->open;
            out->hot = s->hot;
            out->first_row = s->first_row;
        }
    } else {
        return false;
    }
    out->focused = s != nullptr && s->focused;
    out->caret_on = out->focused && embi_forms_caret_phase(n, SDL_GetTicks()) == 0;
    return true;
}

/* ------------------------------------------------------------------ form entries, submission and reset */

typedef struct entry_sink {
    void (*fn)(embi_bind_ctx *c, void *user, embi_str name, embi_str value);
    void *user;
} entry_sink;

/* The entry list of a form (the constructing the entry list algorithm): the listed controls of the form in tree order,
 * skipping disabled and unnamed ones, unchecked boxes, and buttons other than the submitter. */
static void each_entry(embi_bind_ctx *c, embi_node *form, embi_node *submitter, const entry_sink *sink)
{
    embi_node *root = tree_root(form);
    for (embi_node *x = root; x != nullptr; x = next_in(x, root)) {
        if (!listed(x) || owner_form(x) != form) {
            continue;
        }
        if (embi_pf_disabled(el_of(x)) || in_datalist(x)) {
            continue;
        }
        const embi_attr *na = attr_of(x, "name");
        if (na == nullptr || na->value.len == 0) {
            /* A select's options are entries with the select's name, which is checked below. */
            if (!is_select(x)) {
                continue;
            }
        }
        embi_sb name = {0};
        if (na != nullptr) {
            embi_dstr_append_utf8(&name, na->value);
        }
        if (is_select(x)) {
            if (name.len > 0) {
                for (embi_node *o = x->first_child; o != nullptr; o = next_in(o, x)) {
                    if (is_option(o) && option_selected_effective(o, x) && !option_disabled(o)) {
                        embi_sb v = {0};
                        option_value(o, &v);
                        sink->fn(c, sink->user, (embi_str){name.data, name.len}, (embi_str){v.data, v.len});
                        embi_sb_free(&v);
                    }
                }
            }
        } else if (is_textarea(x)) {
            embi_sb v = {0};
            value_sb(x, &v);
            sink->fn(c, sink->user, (embi_str){name.data, name.len}, (embi_str){v.data, v.len});
            embi_sb_free(&v);
        } else if (is_input(x)) {
            int t = input_type_of(x);
            if (t == IT_CHECKBOX || t == IT_RADIO) {
                if (checked_of(x)) {
                    embi_sb v = {0};
                    value_sb(x, &v);
                    sink->fn(c, sink->user, (embi_str){name.data, name.len}, (embi_str){v.data, v.len});
                    embi_sb_free(&v);
                }
            } else if (t == IT_SUBMIT || t == IT_IMAGE || t == IT_RESET || t == IT_BUTTON) {
                if (x == submitter && name.len > 0) {
                    embi_sb v = {0};
                    value_sb(x, &v);
                    sink->fn(c, sink->user, (embi_str){name.data, name.len}, (embi_str){v.data, v.len});
                    embi_sb_free(&v);
                }
            } else if (t != IT_FILE) {
                embi_sb v = {0};
                value_sb(x, &v);
                sink->fn(c, sink->user, (embi_str){name.data, name.len}, (embi_str){v.data, v.len});
                embi_sb_free(&v);
            }
        } else if (is_button(x)) {
            if (x == submitter && name.len > 0) {
                embi_sb v = {0};
                attr_utf8(x, "value", &v);
                sink->fn(c, sink->user, (embi_str){name.data, name.len}, (embi_str){v.data, v.len});
                embi_sb_free(&v);
            }
        }
        embi_sb_free(&name);
    }
}

static void push_entry(embi_bind_ctx *c, void *user, embi_str name, embi_str value)
{
    embi_js_value arr = *(embi_js_value *)user;
    uint32_t n = embi_bind_array_len(c, arr);
    embi_bind_array_set(c, arr, n, embi_bind_utf8_value(c, name));
    embi_bind_array_set(c, arr, n + 1u, embi_bind_utf8_value(c, value));
}

static void append_form_entry(embi_bind_ctx *c, void *user, embi_str name, embi_str value)
{
    (void)c;
    embi_sb *q = user;
    if (q->len > 0) {
        embi_sb_append_char(q, '&');
    }
    static const char hex[] = "0123456789ABCDEF";
    for (int pass = 0; pass < 2; pass++) {
        embi_str s = pass == 0 ? name : value;
        if (pass == 1) {
            embi_sb_append_char(q, '=');
        }
        for (uint32_t i = 0; i < s.len; i++) {
            uint8_t b = (uint8_t)s.ptr[i];
            bool keep = (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9') || b == '*' ||
                        b == '-' || b == '.' || b == '_';
            if (keep) {
                embi_sb_append_char(q, (char)b);
            } else if (b == ' ') {
                embi_sb_append_char(q, '+');
            } else {
                embi_sb_append_char(q, '%');
                embi_sb_append_char(q, hex[b >> 4]);
                embi_sb_append_char(q, hex[b & 15]);
            }
        }
    }
}

static bool no_validate(const embi_node *form, const embi_node *submitter)
{
    if (has_attr(form, "novalidate")) {
        return true;
    }
    return submitter != nullptr && has_attr(submitter, "formnovalidate");
}

/* Fires invalid on each invalid control of the form. Returns true when they are all valid. */
/* Fires the cancelable invalid event at n; true when it was not canceled. */
static bool fire_invalid(embi_bind_ctx *c, embi_node *n)
{
    embi_js_value ev = embi_impl_event_make(c, EMBI_IF_Event, "invalid", EMBI_EVF_CANCELABLE | EMBI_EVF_TRUSTED, EMBI_JS_UNDEFINED);
    if (ev == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(c->rt);
        return true;
    }
    (void)embi_bind_pin(c, ev);
    return embi_impl_dispatch_value(c, embi_impl_node_rec(c, n), ev);
}

/* reportValidity: invalid fires, and each invalid control that was not canceled reports its message as a console warning. */
static bool report_control(embi_bind_ctx *c, embi_node *n)
{
    if (!will_validate(n)) {
        return true;
    }
    if (valid_of(n)) {
        return true;
    }
    if (fire_invalid(c, n)) {
        embi_sb m = {0};
        message_of(n, &m);
        embi_bind_log(c, EMBI_BIND_LOG_WARN, (embi_str){m.data != nullptr ? m.data : "", m.len});
        embi_sb_free(&m);
    }
    return false;
}

static bool check_form(embi_bind_ctx *c, embi_node *form, embi_node *report_only)
{
    bool ok = true;
    embi_node *root = tree_root(form);
    for (embi_node *x = root; x != nullptr; x = next_in(x, root)) {
        if (!listed(x) || owner_form(x) != form || !will_validate(x)) {
            continue;
        }
        if (!valid_of(x)) {
            ok = false;
            if (report_only != nullptr) {
                (void)report_control(c, x);
            } else {
                embi_impl_fire(c, embi_impl_node_rec(c, x), "invalid", EMBI_JS_UNDEFINED);
            }
        }
    }
    return ok;
}

/* The encodings of the entry list of a submission, built in one pass: application/x-www-form-urlencoded, multipart/form-data
 * and text/plain. */
typedef struct subm_enc {
    embi_sb url;   /* urlencoded */
    embi_sb multi; /* multipart body */
    embi_sb plain; /* text/plain body */
    char boundary[56];
} subm_enc;

/* Appends s to out with every lone CR and lone LF replaced by CRLF. */
static void crlf_normalize(embi_str s, embi_sb *out)
{
    for (uint32_t i = 0; i < s.len; i++) {
        char ch = s.ptr[i];
        if (ch == '\r') {
            embi_sb_append(out, EMBI_STR("\r\n"));
            if (i + 1 < s.len && s.ptr[i + 1] == '\n') {
                i++;
            }
        } else if (ch == '\n') {
            embi_sb_append(out, EMBI_STR("\r\n"));
        } else {
            embi_sb_append_char(out, ch);
        }
    }
}

/* Boundary check pass: sets hit when the boundary occurs in a name or value. */
typedef struct bnd_probe {
    const char *boundary;
    bool hit;
} bnd_probe;

static bool contains_str(embi_str hay, const char *needle)
{
    uint32_t nl = (uint32_t)SDL_strlen(needle);
    for (uint32_t i = 0; i + nl <= hay.len; i++) {
        if (SDL_memcmp(hay.ptr + i, needle, nl) == 0) {
            return true;
        }
    }
    return false;
}

static void probe_entry(embi_bind_ctx *c, void *user, embi_str name, embi_str value)
{
    (void)c;
    bnd_probe *p = user;
    if (contains_str(name, p->boundary) || contains_str(value, p->boundary)) {
        p->hit = true;
    }
}

static void collect_entry(embi_bind_ctx *c, void *user, embi_str name, embi_str value)
{
    subm_enc *e = user;
    /* Every lone CR or LF becomes CRLF in names and values (the form-submission serializers). */
    embi_sb nn = {0}, nv = {0};
    crlf_normalize(name, &nn);
    crlf_normalize(value, &nv);
    name = (embi_str){nn.data != nullptr ? nn.data : "", nn.len};
    value = (embi_str){nv.data != nullptr ? nv.data : "", nv.len};
    append_form_entry(c, &e->url, name, value);
    /* multipart: quotes and line breaks in the name are percent-escaped */
    embi_sb_append(&e->multi, EMBI_STR("--"));
    embi_sb_append(&e->multi, (embi_str){e->boundary, (uint32_t)SDL_strlen(e->boundary)});
    embi_sb_append(&e->multi, EMBI_STR("\r\nContent-Disposition: form-data; name=\""));
    for (uint32_t i = 0; i < name.len; i++) {
        char ch = name.ptr[i];
        if (ch == '"') {
            embi_sb_append(&e->multi, EMBI_STR("%22"));
        } else if (ch == '\n') {
            embi_sb_append(&e->multi, EMBI_STR("%0A"));
        } else if (ch == '\r') {
            embi_sb_append(&e->multi, EMBI_STR("%0D"));
        } else {
            embi_sb_append_char(&e->multi, ch);
        }
    }
    embi_sb_append(&e->multi, EMBI_STR("\"\r\n\r\n"));
    embi_sb_append(&e->multi, value);
    embi_sb_append(&e->multi, EMBI_STR("\r\n"));
    embi_sb_append(&e->plain, name);
    embi_sb_append_char(&e->plain, '=');
    embi_sb_append(&e->plain, value);
    embi_sb_append(&e->plain, EMBI_STR("\r\n"));
    embi_sb_free(&nn);
    embi_sb_free(&nv);
}

/* The navigation of a submission: the action (the document URL when empty, formaction of the submitter first), the method
 * (formmethod first) and the body by the enctype (formenctype first). A GET carries the urlencoded entries in the query. */
static void navigate_submission(embi_bind_ctx *c, embi_node *form, embi_node *submitter, subm_enc *e)
{
    embi_sb action = {0};
    bool have = false;
    if (submitter != nullptr && attr_utf8(submitter, "formaction", &action) && action.len > 0) {
        have = true;
    }
    if (!have && (!attr_utf8(form, "action", &action) || action.len == 0)) {
        embi_str doc = embi_bind_url(c);
        action.len = 0;
        if (doc.len > 0) {
            embi_sb_append(&action, doc);
        }
        for (uint32_t i = 0; i < action.len; i++) {
            if (action.data[i] == '?' || action.data[i] == '#') {
                action.len = i;
                break;
            }
        }
    }
    const embi_attr *m = submitter != nullptr ? attr_of(submitter, "formmethod") : nullptr;
    if (m == nullptr || m->value.len == 0) {
        m = attr_of(form, "method");
    }
    bool post = m != nullptr && dstr_ieq(m->value, "post");
    if (m != nullptr && dstr_ieq(m->value, "dialog")) {
        embi_sb_free(&action);
        return;
    }
    const embi_attr *en = submitter != nullptr ? attr_of(submitter, "formenctype") : nullptr;
    if (en == nullptr || en->value.len == 0) {
        en = attr_of(form, "enctype");
    }
    int enc = 0; /* 0 urlencoded, 1 multipart, 2 plain */
    if (en != nullptr && dstr_ieq(en->value, "multipart/form-data")) {
        enc = 1;
    } else if (en != nullptr && dstr_ieq(en->value, "text/plain")) {
        enc = 2;
    }
    embi_sb raw = {0};
    embi_sb_append(&raw, (embi_str){action.data, action.len});
    embi_sb body = {0};
    const char *ctype = "";
    if (!post) {
        /* Mutate action URL: the query is replaced by the entries and the fragment stays after it. */
        uint32_t frag = raw.len, cut;
        for (uint32_t i = 0; i < raw.len; i++) {
            if (raw.data[i] == '#') {
                frag = i;
                break;
            }
        }
        cut = frag;
        for (uint32_t i = 0; i < frag; i++) {
            if (raw.data[i] == '?') {
                cut = i;
                break;
            }
        }
        embi_sb tail = {0};
        if (frag < raw.len) {
            embi_sb_append(&tail, (embi_str){raw.data + frag, raw.len - frag});
        }
        raw.len = cut;
        embi_sb_append_char(&raw, '?');
        embi_sb_append(&raw, (embi_str){e->url.data, e->url.len});
        if (tail.len > 0) {
            embi_sb_append(&raw, (embi_str){tail.data, tail.len});
        }
        embi_sb_free(&tail);
    } else if (enc == 1) {
        embi_sb_append(&e->multi, EMBI_STR("--"));
        embi_sb_append(&e->multi, (embi_str){e->boundary, (uint32_t)SDL_strlen(e->boundary)});
        embi_sb_append(&e->multi, EMBI_STR("--\r\n"));
        embi_sb_append(&body, (embi_str){e->multi.data, e->multi.len});
        ctype = "multipart/form-data; boundary=";
    } else if (enc == 2) {
        embi_sb_append(&body, (embi_str){e->plain.data, e->plain.len});
        ctype = "text/plain";
    } else {
        embi_sb_append(&body, (embi_str){e->url.data, e->url.len});
        ctype = "application/x-www-form-urlencoded";
    }
    embi_sb ct = {0};
    embi_sb_append(&ct, (embi_str){ctype, (uint32_t)SDL_strlen(ctype)});
    if (post && enc == 1) {
        embi_sb_append(&ct, (embi_str){e->boundary, (uint32_t)SDL_strlen(e->boundary)});
    }
    embi_impl_follow_form(c, post ? EMBI_STR("POST") : EMBI_STR("GET"), (embi_str){raw.data, raw.len},
                          (embi_str){body.data, body.len}, (embi_str){ct.data, ct.len});
    embi_sb_free(&ct);
    embi_sb_free(&body);
    embi_sb_free(&raw);
    embi_sb_free(&action);
}

/* The submission of a form: validation (unless skipped), the submit event (unless skipped), then the navigation. */
static bool submit_form(embi_bind_ctx *c, embi_node *form, embi_node *submitter, bool validate, bool fire_event)
{
    if (validate && !no_validate(form, submitter)) {
        if (!check_form(c, form, nullptr)) {
            return false;
        }
    }
    if (fire_event) {
        embi_js_value pl = embi_bind_object_new(c);
        embi_bind_pin(c, pl);
        embi_js_value sub = submitter != nullptr ? embi_bind_node_value(c, submitter) : EMBI_JS_NULL;
        embi_bind_object_set_c(c, pl, "submitter", sub);
        embi_js_value ev = embi_impl_event_make(c, EMBI_IF_SubmitEvent, "submit",
                                                EMBI_EVF_BUBBLES | EMBI_EVF_CANCELABLE, pl);
        if (ev == EMBI_JS_EXCEPTION) {
            embi_js_exception_clear(c->rt);
            return false;
        }
        if (!embi_impl_dispatch_value(c, embi_impl_node_rec(c, form), ev)) {
            return false;
        }
    }
    subm_enc enc;
    memset(&enc, 0, sizeof enc);
    {
        static atomic_uint boundary_seq;
        bnd_probe pr = {enc.boundary, false};
        const entry_sink probe = {probe_entry, &pr};
        for (int attempt = 0; attempt < 16; attempt++) {
            SDL_snprintf(enc.boundary, sizeof enc.boundary, "----EmberFormBoundary%08x%08x%04x", (unsigned)SDL_rand_bits(),
                         (unsigned)SDL_rand_bits(), (unsigned)(atomic_fetch_add(&boundary_seq, 1u) & 0xFFFFu));
            pr.hit = false;
            each_entry(c, form, submitter, &probe);
            if (!pr.hit) {
                break;
            }
        }
    }
    const entry_sink sink = {collect_entry, &enc};
    each_entry(c, form, submitter, &sink);
    navigate_submission(c, form, submitter, &enc);
    embi_sb_free(&enc.url);
    embi_sb_free(&enc.multi);
    embi_sb_free(&enc.plain);
    return true;
}

/* form.reset(): the reset event (cancelable), then every control returns to its default. */
static void reset_form(embi_bind_ctx *c, embi_node *form)
{
    embi_js_value ev = embi_impl_event_make(c, EMBI_IF_Event, "reset", EMBI_EVF_BUBBLES | EMBI_EVF_CANCELABLE,
                                            EMBI_JS_UNDEFINED);
    if (ev == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(c->rt);
        return;
    }
    if (!embi_impl_dispatch_value(c, embi_impl_node_rec(c, form), ev)) {
        return;
    }
    embi_node *root = tree_root(form);
    for (embi_node *x = root; x != nullptr; x = next_in(x, root)) {
        if (!listed(x) || owner_form(x) != form) {
            continue;
        }
        embi_ctl *s = ctl_peek(x) != nullptr ? ctl_make(c, x) : nullptr;
        if (s != nullptr) {
            s->value_dirty = false;
            s->checked_dirty = false;
            s->selected_dirty = false;
            s->comp_on = false;
        }
    }
}

/* Whether an input of this type counts as a field that blocks implicit submission (text-like types plus the date, time and
 * number types). */
static bool blocks_implicit(int t)
{
    return text_like(t) || t == IT_DATE || t == IT_MONTH || t == IT_WEEK || t == IT_TIME || t == IT_LOCAL ||
           t == IT_NUMBER;
}

bool embi_forms_implicit_submit(embi_bind_ctx *c, embi_node *target)
{
    embi_bind_scope sc = embi_bind_enter(c);
    bool done = false;
    embi_forms_commit(c, target); /* Enter commits the edit before the form is submitted */
    embi_node *form = owner_form(target);
    if (form != nullptr && is_input(target) && blocks_implicit(input_type_of(target)) && !embi_pf_disabled(el_of(target))) {
        embi_node *button = nullptr;
        embi_node *root = tree_root(form);
        for (embi_node *x = root; x != nullptr && button == nullptr; x = next_in(x, root)) {
            if (owner_form(x) == form && is_submit_button(x) && !embi_pf_disabled(el_of(x))) {
                button = x;
            }
        }
        if (button != nullptr) {
            embi_impl_HTMLElement_click(c, button);
            done = true;
        } else {
            uint32_t fields = 0;
            for (embi_node *x = root; x != nullptr; x = next_in(x, root)) {
                if (is_input(x) && owner_form(x) == form && blocks_implicit(input_type_of(x))) {
                    fields++;
                }
            }
            if (fields <= 1) {
                done = submit_form(c, form, nullptr, true, true);
            }
        }
    }
    (void)embi_bind_leave(sc, EMBI_JS_UNDEFINED);
    return done;
}

embi_js_value embi_forms_formdata_construct(embi_bind_ctx *c, embi_js_value new_target, embi_js_value a0)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_bind_obj *bo = embi_bind_obj_of(a0);
    if (bo == nullptr || !bo->is_node || bo->ptr == nullptr || !is_form(bo->ptr)) {
        (void)embi_js_throw_type_error(c->rt, "Failed to construct 'FormData': parameter 1 is not of type 'HTMLFormElement'.");
        return embi_bind_leave(sc, EMBI_JS_EXCEPTION);
    }
    embi_node *form = bo->ptr;
    embi_js_value arr = embi_bind_array_new(c, 0);
    embi_bind_pin(c, arr);
    const entry_sink sink = {push_entry, &arr};
    each_entry(c, form, nullptr, &sink);
    embi_js_value v = embi_bind_other_value(c, EMBI_IF_FormData, c, nullptr, 0);
    embi_bind_set_ref(embi_bind_obj_of(v), arr);
    embi_bind_adopt_new_target(c, v, new_target);
    return embi_bind_leave(sc, v);
}

/* ------------------------------------------------------------------ activation behaviors */

static bool input_applies(embi_bind_ctx *c, embi_node *el)
{
    (void)c;
    if (!is_input(el) || embi_pf_disabled(el_of(el))) {
        return false;
    }
    int t = input_type_of(el);
    return t == IT_CHECKBOX || t == IT_RADIO || t == IT_SUBMIT || t == IT_RESET || t == IT_IMAGE;
}

static void input_pre(embi_bind_ctx *c, embi_node *el, embi_bind_obj *event)
{
    (void)event;
    embi_ctl *s = ctl_make(c, el);
    if (s == nullptr) {
        return;
    }
    s->prev_checked = checked_of(el);
    int t = input_type_of(el);
    if (t == IT_CHECKBOX) {
        set_checked(c, el, !s->prev_checked);
    } else if (t == IT_RADIO) {
        set_checked(c, el, true);
    }
}

static void input_run(embi_bind_ctx *c, embi_node *el, embi_bind_obj *event)
{
    (void)event;
    int t = input_type_of(el);
    if (t == IT_CHECKBOX || t == IT_RADIO) {
        const embi_ctl *rs = ctl_peek(el);
        if (t == IT_RADIO && rs != nullptr && rs->prev_checked) {
            return; /* clicking the checked radio changes nothing: no input and no change */
        }
        fire_bubbling(c, el, "input");
        fire_bubbling(c, el, "change");
        return;
    }
    embi_node *form = owner_form(el);
    if (form == nullptr) {
        return;
    }
    if (t == IT_RESET) {
        reset_form(c, form);
    } else {
        (void)submit_form(c, form, el, true, true);
    }
}

static void input_canceled(embi_bind_ctx *c, embi_node *el, embi_bind_obj *event)
{
    (void)event;
    int t = input_type_of(el);
    if (t == IT_CHECKBOX || t == IT_RADIO) {
        const embi_ctl *s = ctl_peek(el);
        if (s != nullptr) {
            set_checked(c, el, s->prev_checked);
        }
    }
}

static bool button_applies(embi_bind_ctx *c, embi_node *el)
{
    (void)c;
    return is_button(el) && !embi_pf_disabled(el_of(el)) && button_kind(el) != IT_BUTTON;
}

static void button_run(embi_bind_ctx *c, embi_node *el, embi_bind_obj *event)
{
    (void)event;
    embi_node *form = owner_form(el);
    if (form == nullptr) {
        return;
    }
    if (button_kind(el) == IT_RESET) {
        reset_form(c, form);
    } else {
        (void)submit_form(c, form, el, true, true);
    }
}

static const embi_activation k_input = {
    .applies = input_applies,
    .pre = input_pre,
    .run = input_run,
    .canceled = input_canceled,
};

static const embi_activation k_button = {
    .applies = button_applies,
    .run = button_run,
};

/* W8b: :checked of checkbox and radio inputs and of options from their live state (css/selector.h embi_sel_checked_fn), -1
 * for any other element (the attributes decide). */
static int sel_checked(const embi_element *el)
{
    embi_node *n = (embi_node *)(uintptr_t)&el->base; /* read only: the helpers below take mutable nodes */
    if (is_input(n)) {
        const int t = input_type_of(n);
        return t == IT_CHECKBOX || t == IT_RADIO ? (checked_of(n) ? 1 : 0) : -1;
    }
    if (is_option(n)) {
        return option_selected_effective(n, nearest_select(n)) ? 1 : 0;
    }
    return -1;
}

void embi_forms_ready(void)
{
    embi_impl_input_ready();
    embi_sel_set_checked_fn(sel_checked);
    static atomic_int state; /* 0: not registered, 1: registering, 2: done */
    int expected = 0;
    if (atomic_compare_exchange_strong(&state, &expected, 1)) {
        (void)embi_bind_register_activation("input", &k_input);
        (void)embi_bind_register_activation("button", &k_button);
        atomic_store(&state, 2);
    } else {
        while (atomic_load(&state) != 2) {
        }
    }
}

/* ------------------------------------------------------------------ FileList and ValidityState */

uint32_t embi_impl_FileList_get_length(embi_bind_ctx *c, void *self)
{
    (void)c;
    (void)self;
    return 0;
}

embi_js_value embi_impl_FileList_item(embi_bind_ctx *c, void *self, uint32_t a0)
{
    (void)c;
    (void)self;
    (void)a0;
    return EMBI_JS_NULL;
}

#define VS_GET(name, expr)                                                                          \
    bool embi_impl_ValidityState_get_##name(embi_bind_ctx *c, void *self)                           \
    {                                                                                               \
        (void)c;                                                                                    \
        embi_node *n = self; /* the generated getter passes bo->ptr: the control */                 \
        if (n == nullptr) {                                                                         \
            return false;                                                                           \
        }                                                                                           \
        vflags f;                                                                                   \
        validity_of(n, &f);                                                                         \
        return (expr);                                                                              \
    }

VS_GET(valueMissing, f.missing)
VS_GET(typeMismatch, f.type)
VS_GET(patternMismatch, f.pattern)
VS_GET(tooLong, f.too_long)
VS_GET(tooShort, f.too_short)
VS_GET(rangeUnderflow, f.under)
VS_GET(rangeOverflow, f.over)
VS_GET(stepMismatch, f.step)
VS_GET(badInput, f.bad)
VS_GET(customError, f.custom)
VS_GET(valid, !(f.missing || f.type || f.too_long || f.too_short || f.under || f.over || f.custom || f.pattern || f.step ||
                 f.bad))

/* ------------------------------------------------------------------ shared member implementations */

static embi_dstr tmp_str(embi_bind_ctx *c, const embi_sb *sb)
{
    return embi_bind_tmp_utf8(c, (embi_str){sb->len > 0 ? sb->data : "", sb->len});
}

static bool set_custom(embi_bind_ctx *c, embi_node *n, embi_dstr msg)
{
    embi_ctl *s = ctl_make(c, n);
    if (s == nullptr) {
        return false;
    }
    embi_sb_clear(&s->custom);
    embi_sb_append(&s->custom, embi_bind_utf8(c, msg));
    return true;
}

static embi_dstr message_str(embi_bind_ctx *c, embi_node *n)
{
    embi_sb m = {0};
    message_of(n, &m);
    embi_dstr d = tmp_str(c, &m);
    embi_sb_free(&m);
    return d;
}

/* The selection of a text control, in UTF-16 units; zero for a type without one. */
static void sel_read(embi_bind_ctx *c, embi_node *n, uint32_t *a, uint32_t *b, int *dir)
{
    *a = 0;
    *b = 0;
    *dir = EMBI_EDIT_DIR_NONE;
    if (!selection_api(n)) {
        return;
    }
    embi_ctl *s = text_state(c, n);
    if (s != nullptr) {
        embi_edit_get_selection(&s->ed, a, b, dir);
    }
}

static void sel_write(embi_bind_ctx *c, embi_node *n, uint32_t a, uint32_t b, int dir)
{
    if (!selection_api(n)) {
        return;
    }
    embi_ctl *s = text_state(c, n);
    if (s != nullptr) {
        embi_edit_set_selection(&s->ed, a, b, dir);
    }
}

static embi_dstr dir_str(embi_bind_ctx *c, int dir)
{
    const char *d = dir == EMBI_EDIT_DIR_FORWARD ? "forward" : (dir == EMBI_EDIT_DIR_BACKWARD ? "backward" : "none");
    return embi_bind_tmp_utf8(c, (embi_str){d, (uint32_t)SDL_strlen(d)});
}

static int dir_of(embi_dstr d)
{
    if (dstr_ieq(d, "forward")) {
        return EMBI_EDIT_DIR_FORWARD;
    }
    if (dstr_ieq(d, "backward")) {
        return EMBI_EDIT_DIR_BACKWARD;
    }
    return EMBI_EDIT_DIR_NONE;
}

static embi_dom_err set_selection_range(embi_bind_ctx *c, embi_node *n, uint32_t a, uint32_t b, embi_dstr d)
{
    if (!selection_api(n)) {
        return embi_impl_throw(c, "InvalidStateError", "The input element does not support selection.");
    }
    sel_write(c, n, a, b, d.len > 0 ? dir_of(d) : EMBI_EDIT_DIR_NONE);
    return EMBI_DOM_OK;
}

/* The value as a DOM string of the scope. */
static embi_dstr value_dstr(embi_bind_ctx *c, embi_node *n)
{
    embi_sb v = {0};
    value_sb(n, &v);
    embi_dstr d = tmp_str(c, &v);
    embi_sb_free(&v);
    return d;
}

static embi_node *form_of(embi_node *n)
{
    return owner_form(n);
}

static embi_js_value nodelist_of(embi_bind_ctx *c, embi_node *root, bool (*pick)(embi_node *, embi_node *), embi_node *ctx)
{
    uint32_t count = 0;
    for (embi_node *x = root->first_child; x != nullptr; x = next_in(x, root)) {
        count += pick(x, ctx) ? 1u : 0u;
    }
    embi_node **items = count > 0 ? embi_alloc(count * sizeof *items) : nullptr;
    uint32_t i = 0;
    for (embi_node *x = root->first_child; x != nullptr && items != nullptr; x = next_in(x, root)) {
        if (pick(x, ctx)) {
            items[i++] = x;
        }
    }
    embi_js_value v = embi_impl_nodelist_static(c, items, count);
    if (items != nullptr) {
        embi_free(items, count * sizeof *items);
    }
    return v;
}

static bool pick_listed_form_member(embi_node *x, embi_node *form)
{
    return listed(x) && owner_form(x) == form;
}

static bool pick_option(embi_node *x, embi_node *sel)
{
    (void)sel;
    return is_option(x);
}

/* The listed elements of form, in tree order, for the form's elements collection. */
static embi_js_value form_elements(embi_bind_ctx *c, embi_node *form)
{
    embi_node *root = tree_root(form);
    uint32_t count = 0;
    for (embi_node *x = root; x != nullptr; x = next_in(x, root)) {
        count += pick_listed_form_member(x, form) ? 1u : 0u;
    }
    embi_node **items = count > 0 ? embi_alloc(count * sizeof *items) : nullptr;
    uint32_t i = 0;
    for (embi_node *x = root; x != nullptr && items != nullptr; x = next_in(x, root)) {
        if (pick_listed_form_member(x, form)) {
            items[i++] = x;
        }
    }
    embi_js_value v = embi_impl_nodelist_static(c, items, count);
    if (items != nullptr) {
        embi_free(items, count * sizeof *items);
    }
    return v;
}

/* ------------------------------------------------------------------ HTMLInputElement */

embi_dstr embi_impl_HTMLInputElement_get_type(embi_bind_ctx *c, embi_node *self)
{
    return embi_bind_tmp_utf8(c, (embi_str){input_type_name(input_type_of(self)), (uint32_t)SDL_strlen(input_type_name(input_type_of(self)))});
}

void embi_impl_HTMLInputElement_set_type(embi_bind_ctx *c, embi_node *self, embi_dstr a0)
{
    (void)embi_el_set_attribute(el_of(self), EMBI_STR("type"), embi_bind_tmp_copy(c, a0));
}

embi_dstr embi_impl_HTMLInputElement_get_value(embi_bind_ctx *c, embi_node *self)
{
    embi_sb v = {0};
    value_sb(self, &v);
    embi_dstr d = tmp_str(c, &v);
    embi_sb_free(&v);
    return d;
}

void embi_impl_HTMLInputElement_set_value(embi_bind_ctx *c, embi_node *self, embi_dstr a0)
{
    (void)set_value(c, self, embi_bind_utf8(c, a0));
}

bool embi_impl_HTMLInputElement_get_checked(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    return checked_of(self);
}

void embi_impl_HTMLInputElement_set_checked(embi_bind_ctx *c, embi_node *self, bool a0)
{
    set_checked(c, self, a0);
}

/* The input's selectionStart, selectionEnd and selectionDirection are nullable (HTML): null on the types without the
 * selection API (selection_api), where setting them throws InvalidStateError. The numbers are unsigned long? values, which
 * idlgen does not convert, so the attributes are any and the conversion is here (null and undefined are 0). */
static embi_dom_err sel_api_check(embi_bind_ctx *c, embi_node *n)
{
    if (!selection_api(n)) {
        return embi_impl_throw(c, "InvalidStateError", "The input element's type does not support selection.");
    }
    return EMBI_DOM_OK;
}

static bool sel_offset(embi_bind_ctx *c, embi_js_value v, uint32_t *out)
{
    *out = 0;
    if (embi_js_is_nullish(v)) {
        return true;
    }
    int64_t x = 0;
    if (!embi_bind_conv_int(c, v, 6, EMBI_CTX_SET, &x)) {
        return false;
    }
    *out = (uint32_t)x;
    return true;
}

embi_dom_err embi_impl_HTMLInputElement_get_selectionStart(embi_bind_ctx *c, embi_node *self, embi_js_value *out)
{
    uint32_t a, b;
    int d;
    sel_read(c, self, &a, &b, &d);
    *out = selection_api(self) ? embi_js_from_double((double)a) : EMBI_JS_NULL;
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_HTMLInputElement_set_selectionStart(embi_bind_ctx *c, embi_node *self, embi_js_value a0)
{
    uint32_t v = 0;
    if (!sel_offset(c, a0, &v)) {
        return EMBI_BIND_THREW;
    }
    if (sel_api_check(c, self) != EMBI_DOM_OK) {
        return EMBI_BIND_THREW;
    }
    uint32_t a, b;
    int d;
    sel_read(c, self, &a, &b, &d);
    sel_write(c, self, v, v > b ? v : b, d);
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_HTMLInputElement_get_selectionEnd(embi_bind_ctx *c, embi_node *self, embi_js_value *out)
{
    uint32_t a, b;
    int d;
    sel_read(c, self, &a, &b, &d);
    *out = selection_api(self) ? embi_js_from_double((double)b) : EMBI_JS_NULL;
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_HTMLInputElement_set_selectionEnd(embi_bind_ctx *c, embi_node *self, embi_js_value a0)
{
    uint32_t v = 0;
    if (!sel_offset(c, a0, &v)) {
        return EMBI_BIND_THREW;
    }
    if (sel_api_check(c, self) != EMBI_DOM_OK) {
        return EMBI_BIND_THREW;
    }
    uint32_t a, b;
    int d;
    sel_read(c, self, &a, &b, &d);
    sel_write(c, self, a < v ? a : v, v, d);
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_HTMLInputElement_get_selectionDirection(embi_bind_ctx *c, embi_node *self, embi_js_value *out)
{
    uint32_t a, b;
    int d;
    sel_read(c, self, &a, &b, &d);
    *out = selection_api(self) ? embi_bind_dstr_value(c, dir_str(c, d)) : EMBI_JS_NULL;
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_HTMLInputElement_set_selectionDirection(embi_bind_ctx *c, embi_node *self, embi_bind_nstr a0)
{
    if (sel_api_check(c, self) != EMBI_DOM_OK) {
        return EMBI_BIND_THREW;
    }
    uint32_t a, b;
    int d;
    sel_read(c, self, &a, &b, &d);
    sel_write(c, self, a, b, a0.is_null ? EMBI_EDIT_DIR_NONE : dir_of(a0.s));
    return EMBI_DOM_OK;
}

embi_node *embi_impl_HTMLInputElement_get_form(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    return form_of(self);
}

embi_js_value embi_impl_HTMLInputElement_get_files(embi_bind_ctx *c, embi_node *self)
{
    if (input_type_of(self) != IT_FILE) {
        return EMBI_JS_NULL;
    }
    return filelist_new(c);
}

bool embi_impl_HTMLInputElement_get_willValidate(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    return will_validate(self);
}

embi_js_value embi_impl_HTMLInputElement_get_validity(embi_bind_ctx *c, embi_node *self)
{
    return validity_new(c, self);
}

embi_dstr embi_impl_HTMLInputElement_get_validationMessage(embi_bind_ctx *c, embi_node *self)
{
    return message_str(c, self);
}

bool embi_impl_HTMLInputElement_checkValidity(embi_bind_ctx *c, embi_node *self)
{
    if (!will_validate(self)) {
        return true;
    }
    bool ok = valid_of(self);
    if (!ok) {
        embi_impl_fire(c, embi_impl_node_rec(c, self), "invalid", EMBI_JS_UNDEFINED);
    }
    return ok;
}

bool embi_impl_HTMLInputElement_reportValidity(embi_bind_ctx *c, embi_node *self)
{
    return report_control(c, self);
}

void embi_impl_HTMLInputElement_setCustomValidity(embi_bind_ctx *c, embi_node *self, embi_dstr a0)
{
    (void)set_custom(c, self, a0);
}

embi_dom_err embi_impl_HTMLInputElement_setSelectionRange(embi_bind_ctx *c, embi_node *self, uint32_t a0, uint32_t a1,
                                                          embi_dstr a2)
{
    return set_selection_range(c, self, a0, a1, a2);
}

bool embi_impl_HTMLInputElement_get_indeterminate(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    const embi_ctl *s = ctl_peek(self);
    return s != nullptr && s->indeterminate && input_type_of(self) == IT_CHECKBOX;
}

void embi_impl_HTMLInputElement_set_indeterminate(embi_bind_ctx *c, embi_node *self, bool a0)
{
    embi_ctl *s = ctl_make(c, self);
    if (s != nullptr) {
        s->indeterminate = a0;
    }
}

double embi_impl_HTMLInputElement_get_valueAsNumber(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    if (!number_like(input_type_of(self))) {
        return SDL_strtod("nan", nullptr);
    }
    embi_sb v = {0};
    value_sb(self, &v);
    double r = v.len > 0 && float_ok(v.data, v.len) ? SDL_strtod(embi_sb_cstr(&v), nullptr) : SDL_strtod("nan", nullptr);
    embi_sb_free(&v);
    return r;
}

void embi_impl_HTMLInputElement_set_valueAsNumber(embi_bind_ctx *c, embi_node *self, double a0)
{
    if (!number_like(input_type_of(self))) {
        (void)embi_impl_throw(c, "InvalidStateError", "This input element does not support valueAsNumber.");
        return;
    }
    embi_sb nv = {0};
    if (a0 == a0 && a0 - a0 == 0) {
        fmt_number(a0, &nv);
    }
    (void)set_value(c, self, (embi_str){nv.data, nv.len});
    embi_sb_free(&nv);
}

/* stepUp and stepDown: n steps from the value (the HTML stepUp algorithm for number and range). */
static embi_dom_err step_by(embi_bind_ctx *c, embi_node *n, int32_t amount, int dir)
{
    int t = input_type_of(n);
    double st, base;
    if (!number_like(t) || !step_of(n, &st, &base)) {
        return embi_impl_throw(c, "InvalidStateError", "This input element does not support stepping.");
    }
    bool has_min = false, has_max = false;
    double mn = attr_num(n, "min", 0.0, &has_min);
    double mx = attr_num(n, "max", 0.0, &has_max);
    if (has_min && has_max && mn > mx) {
        return EMBI_DOM_OK;
    }
    embi_sb cur = {0};
    value_sb(n, &cur);
    double v = cur.len > 0 && float_ok(cur.data, cur.len) ? SDL_strtod(embi_sb_cstr(&cur), nullptr) : 0.0;
    embi_sb_free(&cur);
    double delta = st * (double)amount * (double)dir;
    if (step_mismatch(n, v)) {
        double q = (v - base) / st;
        q = dir > 0 ? SDL_floor(q) + (amount > 0 ? (double)(amount - 1) : 0) : SDL_ceil(q) - (amount > 0 ? (double)(amount - 1) : 0);
        v = base + (q + (dir > 0 ? 1.0 : -1.0)) * st;
    } else {
        v += delta;
    }
    if (has_max) {
        double top = base + SDL_floor((mx - base) / st + 1e-9) * st;
        if (v > top) {
            v = top;
        }
    }
    if (has_min) {
        double bot = base + SDL_ceil((mn - base) / st - 1e-9) * st;
        if (v < bot) {
            v = bot;
        }
    }
    if (v != v) {
        return EMBI_DOM_OK;
    }
    embi_sb nv = {0};
    fmt_number(v, &nv);
    (void)set_value(c, n, (embi_str){nv.data, nv.len});
    embi_sb_free(&nv);
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_HTMLInputElement_stepUp(embi_bind_ctx *c, embi_node *self, int32_t a0)
{
    return step_by(c, self, a0, 1);
}

embi_dom_err embi_impl_HTMLInputElement_stepDown(embi_bind_ctx *c, embi_node *self, int32_t a0)
{
    return step_by(c, self, a0, -1);
}

/* select(): the whole value is selected and a select event fires. */
static void select_text(embi_bind_ctx *c, embi_node *n)
{
    if (!selectable(n)) {
        return;
    }
    embi_ctl *s = text_state(c, n);
    if (s != nullptr) {
        embi_edit_select_all(&s->ed);
        s->caret_ms = SDL_GetTicks();
    }
    fire_bubbling(c, n, "select");
}

void embi_impl_HTMLInputElement_select(embi_bind_ctx *c, embi_node *self)
{
    select_text(c, self);
}

void embi_impl_HTMLTextAreaElement_select(embi_bind_ctx *c, embi_node *self)
{
    select_text(c, self);
}

/* ------------------------------------------------------------------ HTMLTextAreaElement */

embi_dstr embi_impl_HTMLTextAreaElement_get_type(embi_bind_ctx *c, embi_node *self)
{
    (void)self;
    return embi_bind_tmp_utf8(c, EMBI_STR("textarea"));
}

embi_dstr embi_impl_HTMLTextAreaElement_get_defaultValue(embi_bind_ctx *c, embi_node *self)
{
    embi_sb v = {0};
    text_of(self, &v);
    embi_dstr d = tmp_str(c, &v);
    embi_sb_free(&v);
    return d;
}

void embi_impl_HTMLTextAreaElement_set_defaultValue(embi_bind_ctx *c, embi_node *self, embi_dstr a0)
{
    (void)c;
    /* The default value is the text content: the children become one text node (none when empty). */
    embi_node *node = a0.len > 0 ? &embi_new_text(self->doc, a0)->base : nullptr;
    (void)embi_replace_all(self, node);
}

embi_dstr embi_impl_HTMLTextAreaElement_get_value(embi_bind_ctx *c, embi_node *self)
{
    return value_dstr(c, self);
}

void embi_impl_HTMLTextAreaElement_set_value(embi_bind_ctx *c, embi_node *self, embi_dstr a0)
{
    (void)set_value(c, self, embi_bind_utf8(c, a0));
}

uint32_t embi_impl_HTMLTextAreaElement_get_textLength(embi_bind_ctx *c, embi_node *self)
{
    embi_sb v = {0};
    value_sb(self, &v);
    uint32_t n = utf16_len(v.data != nullptr ? v.data : "", v.len);
    embi_sb_free(&v);
    (void)c;
    return n;
}

uint32_t embi_impl_HTMLTextAreaElement_get_selectionStart(embi_bind_ctx *c, embi_node *self)
{
    uint32_t a, b;
    int d;
    sel_read(c, self, &a, &b, &d);
    return a;
}

void embi_impl_HTMLTextAreaElement_set_selectionStart(embi_bind_ctx *c, embi_node *self, uint32_t a0)
{
    uint32_t a, b;
    int d;
    sel_read(c, self, &a, &b, &d);
    sel_write(c, self, a0, a0 > b ? a0 : b, d);
}

uint32_t embi_impl_HTMLTextAreaElement_get_selectionEnd(embi_bind_ctx *c, embi_node *self)
{
    uint32_t a, b;
    int d;
    sel_read(c, self, &a, &b, &d);
    return b;
}

void embi_impl_HTMLTextAreaElement_set_selectionEnd(embi_bind_ctx *c, embi_node *self, uint32_t a0)
{
    uint32_t a, b;
    int d;
    sel_read(c, self, &a, &b, &d);
    sel_write(c, self, a < a0 ? a : a0, a0, d);
}

embi_dstr embi_impl_HTMLTextAreaElement_get_selectionDirection(embi_bind_ctx *c, embi_node *self)
{
    uint32_t a, b;
    int d;
    sel_read(c, self, &a, &b, &d);
    return dir_str(c, d);
}

void embi_impl_HTMLTextAreaElement_set_selectionDirection(embi_bind_ctx *c, embi_node *self, embi_dstr a0)
{
    uint32_t a, b;
    int d;
    sel_read(c, self, &a, &b, &d);
    sel_write(c, self, a, b, dir_of(a0));
}

embi_node *embi_impl_HTMLTextAreaElement_get_form(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    return form_of(self);
}

bool embi_impl_HTMLTextAreaElement_get_willValidate(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    return will_validate(self);
}

embi_js_value embi_impl_HTMLTextAreaElement_get_validity(embi_bind_ctx *c, embi_node *self)
{
    return validity_new(c, self);
}

embi_dstr embi_impl_HTMLTextAreaElement_get_validationMessage(embi_bind_ctx *c, embi_node *self)
{
    return message_str(c, self);
}

bool embi_impl_HTMLTextAreaElement_checkValidity(embi_bind_ctx *c, embi_node *self)
{
    return embi_impl_HTMLInputElement_checkValidity(c, self);
}

bool embi_impl_HTMLTextAreaElement_reportValidity(embi_bind_ctx *c, embi_node *self)
{
    return report_control(c, self);
}

void embi_impl_HTMLTextAreaElement_setCustomValidity(embi_bind_ctx *c, embi_node *self, embi_dstr a0)
{
    (void)set_custom(c, self, a0);
}

embi_dom_err embi_impl_HTMLTextAreaElement_setSelectionRange(embi_bind_ctx *c, embi_node *self, uint32_t a0, uint32_t a1,
                                                             embi_dstr a2)
{
    return set_selection_range(c, self, a0, a1, a2);
}

/* ------------------------------------------------------------------ HTMLSelectElement */

embi_dstr embi_impl_HTMLSelectElement_get_type(embi_bind_ctx *c, embi_node *self)
{
    return embi_bind_tmp_utf8(c, select_multiple(self) ? EMBI_STR("select-multiple") : EMBI_STR("select-one"));
}

embi_js_value embi_impl_HTMLSelectElement_get_options(embi_bind_ctx *c, embi_node *self)
{
    return nodelist_of(c, self, pick_option, self);
}

uint32_t embi_impl_HTMLSelectElement_get_length(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    return select_options(self, nullptr, 0);
}

int32_t embi_impl_HTMLSelectElement_get_selectedIndex(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    int32_t i = 0;
    embi_node *picked = select_multiple(self) ? nullptr : single_selected(self);
    for (embi_node *x = self->first_child; x != nullptr; x = next_in(x, self)) {
        if (!is_option(x)) {
            continue;
        }
        if (select_multiple(self) ? option_raw(x) : x == picked) {
            return i;
        }
        i++;
    }
    return -1;
}

void embi_impl_HTMLSelectElement_set_selectedIndex(embi_bind_ctx *c, embi_node *self, int32_t a0)
{
    int32_t i = 0;
    for (embi_node *x = self->first_child; x != nullptr; x = next_in(x, self)) {
        if (!is_option(x)) {
            continue;
        }
        embi_ctl *s = ctl_make(c, x);
        if (s != nullptr) {
            s->selected = i == a0;
            s->selected_dirty = true;
        }
        i++;
    }
    select_restyle(c, self);
}

embi_dstr embi_impl_HTMLSelectElement_get_value(embi_bind_ctx *c, embi_node *self)
{
    return value_dstr(c, self);
}

void embi_impl_HTMLSelectElement_set_value(embi_bind_ctx *c, embi_node *self, embi_dstr a0)
{
    (void)set_value(c, self, embi_bind_utf8(c, a0));
}

embi_node *embi_impl_HTMLSelectElement_get_form(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    return form_of(self);
}

bool embi_impl_HTMLSelectElement_get_willValidate(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    return will_validate(self);
}

embi_js_value embi_impl_HTMLSelectElement_get_validity(embi_bind_ctx *c, embi_node *self)
{
    return validity_new(c, self);
}

embi_dstr embi_impl_HTMLSelectElement_get_validationMessage(embi_bind_ctx *c, embi_node *self)
{
    return message_str(c, self);
}

bool embi_impl_HTMLSelectElement_checkValidity(embi_bind_ctx *c, embi_node *self)
{
    return embi_impl_HTMLInputElement_checkValidity(c, self);
}

bool embi_impl_HTMLSelectElement_reportValidity(embi_bind_ctx *c, embi_node *self)
{
    return report_control(c, self);
}

void embi_impl_HTMLSelectElement_setCustomValidity(embi_bind_ctx *c, embi_node *self, embi_dstr a0)
{
    (void)set_custom(c, self, a0);
}

/* ------------------------------------------------------------------ HTMLOptionElement */

embi_dstr embi_impl_HTMLOptionElement_get_label(embi_bind_ctx *c, embi_node *self)
{
    embi_sb v = {0};
    if (!attr_utf8(self, "label", &v)) {
        collapsed_text(self, &v);
    }
    embi_dstr d = tmp_str(c, &v);
    embi_sb_free(&v);
    return d;
}

void embi_impl_HTMLOptionElement_set_label(embi_bind_ctx *c, embi_node *self, embi_dstr a0)
{
    (void)embi_el_set_attribute(el_of(self), EMBI_STR("label"), embi_bind_tmp_copy(c, a0));
}

bool embi_impl_HTMLOptionElement_get_selected(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    return option_selected_effective(self, nearest_select(self));
}

void embi_impl_HTMLOptionElement_set_selected(embi_bind_ctx *c, embi_node *self, bool a0)
{
    embi_ctl *s = ctl_make(c, self);
    if (s != nullptr) {
        s->selected = a0;
        s->selected_dirty = true;
    }
    embi_node *sel = nearest_select(self);
    if (a0 && sel != nullptr && !select_multiple(sel)) {
        for (embi_node *x = sel->first_child; x != nullptr; x = next_in(x, sel)) {
            if (is_option(x) && x != self) {
                embi_ctl *o = ctl_make(c, x);
                if (o != nullptr) {
                    o->selected = false;
                    o->selected_dirty = true;
                }
            }
        }
    }
    if (sel != nullptr) {
        select_restyle(c, sel);
    } else {
        state_restyle(c, self);
    }
}

embi_dstr embi_impl_HTMLOptionElement_get_value(embi_bind_ctx *c, embi_node *self)
{
    embi_sb v = {0};
    option_value(self, &v);
    embi_dstr d = tmp_str(c, &v);
    embi_sb_free(&v);
    return d;
}

void embi_impl_HTMLOptionElement_set_value(embi_bind_ctx *c, embi_node *self, embi_dstr a0)
{
    (void)embi_el_set_attribute(el_of(self), EMBI_STR("value"), embi_bind_tmp_copy(c, a0));
}

embi_dstr embi_impl_HTMLOptionElement_get_text(embi_bind_ctx *c, embi_node *self)
{
    embi_sb v = {0};
    collapsed_text(self, &v);
    embi_dstr d = tmp_str(c, &v);
    embi_sb_free(&v);
    return d;
}

int32_t embi_impl_HTMLOptionElement_get_index(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    embi_node *sel = nearest_select(self);
    if (sel == nullptr) {
        return 0;
    }
    int32_t i = 0;
    for (embi_node *x = sel->first_child; x != nullptr; x = next_in(x, sel)) {
        if (is_option(x)) {
            if (x == self) {
                return i;
            }
            i++;
        }
    }
    return 0;
}

embi_node *embi_impl_HTMLOptionElement_get_form(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    embi_node *sel = nearest_select(self);
    return sel != nullptr ? owner_form(sel) : nullptr;
}

/* ------------------------------------------------------------------ HTMLButtonElement */

embi_dstr embi_impl_HTMLButtonElement_get_type(embi_bind_ctx *c, embi_node *self)
{
    int k = button_kind(self);
    const char *name = k == IT_RESET ? "reset" : (k == IT_BUTTON ? "button" : "submit");
    return embi_bind_tmp_utf8(c, (embi_str){name, (uint32_t)SDL_strlen(name)});
}

void embi_impl_HTMLButtonElement_set_type(embi_bind_ctx *c, embi_node *self, embi_dstr a0)
{
    (void)embi_el_set_attribute(el_of(self), EMBI_STR("type"), embi_bind_tmp_copy(c, a0));
}

embi_node *embi_impl_HTMLButtonElement_get_form(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    return form_of(self);
}

bool embi_impl_HTMLButtonElement_get_willValidate(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    return will_validate(self);
}

embi_js_value embi_impl_HTMLButtonElement_get_validity(embi_bind_ctx *c, embi_node *self)
{
    return validity_new(c, self);
}

embi_dstr embi_impl_HTMLButtonElement_get_validationMessage(embi_bind_ctx *c, embi_node *self)
{
    return message_str(c, self);
}

bool embi_impl_HTMLButtonElement_checkValidity(embi_bind_ctx *c, embi_node *self)
{
    return embi_impl_HTMLInputElement_checkValidity(c, self);
}

bool embi_impl_HTMLButtonElement_reportValidity(embi_bind_ctx *c, embi_node *self)
{
    return report_control(c, self);
}

void embi_impl_HTMLButtonElement_setCustomValidity(embi_bind_ctx *c, embi_node *self, embi_dstr a0)
{
    (void)set_custom(c, self, a0);
}

/* ------------------------------------------------------------------ HTMLFormElement */

embi_js_value embi_impl_HTMLFormElement_get_elements(embi_bind_ctx *c, embi_node *self)
{
    return form_elements(c, self);
}

uint32_t embi_impl_HTMLFormElement_get_length(embi_bind_ctx *c, embi_node *self)
{
    (void)c;
    uint32_t n = 0;
    embi_node *root = tree_root(self);
    for (embi_node *x = root; x != nullptr; x = next_in(x, root)) {
        if (listed(x) && owner_form(x) == self && !(is_input(x) && input_type_of(x) == IT_IMAGE)) {
            n++;
        }
    }
    return n;
}

embi_dom_err embi_impl_HTMLFormElement_submit(embi_bind_ctx *c, embi_node *self)
{
    (void)submit_form(c, self, nullptr, false, false);
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_HTMLFormElement_requestSubmit(embi_bind_ctx *c, embi_node *self, embi_node *a0)
{
    if (a0 != nullptr && !(is_submit_button(a0) && owner_form(a0) == self)) {
        return embi_impl_throw(c, "TypeError", "The submitter is not a submit button of this form.");
    }
    (void)submit_form(c, self, a0, true, true);
    return EMBI_DOM_OK;
}

void embi_impl_HTMLFormElement_reset(embi_bind_ctx *c, embi_node *self)
{
    reset_form(c, self);
}

bool embi_impl_HTMLFormElement_checkValidity(embi_bind_ctx *c, embi_node *self)
{
    return check_form(c, self, nullptr);
}

bool embi_impl_HTMLFormElement_reportValidity(embi_bind_ctx *c, embi_node *self)
{
    return check_form(c, self, self);
}
