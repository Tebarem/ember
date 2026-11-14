/*
 * Input events and default actions of the bindings (W5d, bindings/impl_input.h): the UI events of the input layer, the
 * focus change algorithm, and the activation behaviors of a, label and button. Event payloads are plain objects of the
 * interface's fields, as impl_events.c reads them.
 */
#include "bindings/impl_input.h"

#include "bindings/activation.h"
#include "bindings/impl_forms.h"
#include "bindings/gen/dom.h"
#include "bindings/gen/events.h"
#include "js/object.h"
#include "js/ops.h"
#include "js/runtime.h"
#include "dom/dstr.h"
#include "dom/event.h"
#include "platform/platform.h"

#include <stdatomic.h>
#include <string.h>

#include <SDL3/SDL_stdinc.h>

#include <ember/ember_input.h>

/* ------------------------------------------------------------------ helpers */

static embi_js_value num(double d)
{
    return embi_js_number(d);
}

static embi_str cstr(const char *s)
{
    return (embi_str){s, s != nullptr ? (uint32_t)strlen(s) : 0};
}

/* A new event payload object, pinned to the current scope. */
static embi_js_value payload(embi_bind_ctx *c)
{
    return embi_bind_pin(c, embi_bind_object_new(c));
}

static void put(embi_bind_ctx *c, embi_js_value obj, const char *k, embi_js_value v)
{
    embi_bind_object_set_c(c, obj, k, v);
}

static void put_mods(embi_bind_ctx *c, embi_js_value p, uint32_t mods)
{
    put(c, p, "ctrlKey", embi_js_from_bool((mods & EMB_MOD_CTRL) != 0));
    put(c, p, "shiftKey", embi_js_from_bool((mods & EMB_MOD_SHIFT) != 0));
    put(c, p, "altKey", embi_js_from_bool((mods & EMB_MOD_ALT) != 0));
    put(c, p, "metaKey", embi_js_from_bool((mods & EMB_MOD_META) != 0));
}

/* Dispatches the event wrapper ev at target (node). Returns true when not canceled. */
static bool dispatch(embi_bind_ctx *c, embi_node *target, embi_js_value ev)
{
    if (ev == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(c->rt);
        return true;
    }
    embi_bind_obj *rec = embi_impl_node_rec(c, target);
    return embi_impl_dispatch_value(c, rec, ev);
}

/* A wrapper of an event of iface and type with the payload p. */
static embi_js_value make(embi_bind_ctx *c, uint16_t iface, const char *type, uint32_t flags, embi_js_value p)
{
    embi_js_value ev = embi_impl_event_make(c, iface, type, flags | EMBI_EVF_TRUSTED, p);
    if (ev != EMBI_JS_EXCEPTION) {
        embi_bind_pin(c, ev);
    }
    return ev;
}

/* ------------------------------------------------------------------ pointer, keyboard, input, composition */

/* The MouseEvent members of a pointer payload. */
static void put_mouse(embi_bind_ctx *c, embi_js_value pl, const embi_input_ptr *p)
{
    put(c, pl, "clientX", num(p->clientX));
    put(c, pl, "clientY", num(p->clientY));
    put(c, pl, "screenX", num(p->screenX));
    put(c, pl, "screenY", num(p->screenY));
    put(c, pl, "pageX", num(p->pageX));
    put(c, pl, "pageY", num(p->pageY));
    put(c, pl, "offsetX", num(p->offsetX));
    put(c, pl, "offsetY", num(p->offsetY));
    put(c, pl, "movementX", num(p->movementX));
    put(c, pl, "movementY", num(p->movementY));
    put(c, pl, "relatedTarget", p->related != nullptr ? embi_bind_node_value(c, p->related) : EMBI_JS_NULL);
    put(c, pl, "button", num(p->button));
    put(c, pl, "buttons", num(p->buttons));
    put(c, pl, "detail", num(p->detail));
    put_mods(c, pl, p->mods);
}

bool embi_impl_input_pointer(embi_bind_ctx *c, embi_node *target, bool pointer, const char *type,
                             const embi_input_ptr *p, bool bubbles, bool cancelable)
{
    embi_bind_scope sc = embi_bind_enter(c);
    uint32_t flags = (bubbles ? EMBI_EVF_BUBBLES : 0) | (cancelable ? EMBI_EVF_CANCELABLE : 0) | EMBI_EVF_COMPOSED;
    embi_js_value pl = payload(c);
    put_mouse(c, pl, p);
    if (pointer) {
        put(c, pl, "pointerId", num(p->pointerId));
        put(c, pl, "pointerType", embi_bind_utf8_value(c, cstr(p->pointerType ? p->pointerType : "mouse")));
        put(c, pl, "isPrimary", embi_js_from_bool(p->isPrimary));
        put(c, pl, "width", num(1));
        put(c, pl, "height", num(1));
        put(c, pl, "pressure", num(p->buttons != 0 ? 0.5 : 0));
    }
    uint16_t iface = pointer ? EMBI_IF_PointerEvent : EMBI_IF_MouseEvent;
    embi_js_value ev = make(c, iface, type, flags, pl);
    bool ok = dispatch(c, target, ev);
    embi_bind_leave(sc, EMBI_JS_UNDEFINED);
    return ok;
}

/* Chrome's legacy keyCode (Windows, US layout) of a key: by code for the layout-independent keys, by key for a letter
 * (the virtual key follows the layout). 0 when unknown. */
static uint32_t legacy_key_code(const char *key, const char *code)
{
    static const struct {
        const char *code;
        uint16_t vk;
    } tab[] = {
        {"Backspace", 8}, {"Tab", 9}, {"Enter", 13}, {"NumpadEnter", 13}, {"ShiftLeft", 16}, {"ShiftRight", 16},
        {"ControlLeft", 17}, {"ControlRight", 17}, {"AltLeft", 18}, {"AltRight", 18}, {"Pause", 19}, {"CapsLock", 20}, {"PrintScreen", 44},
        {"Escape", 27}, {"Space", 32}, {"PageUp", 33}, {"PageDown", 34}, {"End", 35}, {"Home", 36}, {"ArrowLeft", 37},
        {"ArrowUp", 38}, {"ArrowRight", 39}, {"ArrowDown", 40}, {"Insert", 45}, {"Delete", 46}, {"MetaLeft", 91},
        {"MetaRight", 92}, {"ContextMenu", 93}, {"NumpadMultiply", 106}, {"NumpadAdd", 107}, {"NumpadSubtract", 109},
        {"NumpadDecimal", 110}, {"NumpadDivide", 111}, {"NumLock", 144}, {"ScrollLock", 145}, {"Semicolon", 186},
        {"Equal", 187}, {"Comma", 188}, {"Minus", 189}, {"Period", 190}, {"Slash", 191}, {"Backquote", 192},
        {"BracketLeft", 219}, {"Backslash", 220}, {"BracketRight", 221}, {"Quote", 222}, {"IntlBackslash", 226},
    };
    if (key != nullptr && key[0] != '\0' && key[1] == '\0') {
        char ch = key[0];
        if (ch >= 'a' && ch <= 'z') {
            return (uint32_t)(ch - 'a' + 'A');
        }
        if (ch >= 'A' && ch <= 'Z') {
            return (uint32_t)ch;
        }
    }
    if (code == nullptr || code[0] == '\0') {
        return 0;
    }
    size_t n = strlen(code);
    if (n == 6 && strncmp(code, "Digit", 5) == 0 && code[5] >= '0' && code[5] <= '9') {
        return (uint32_t)(48 + code[5] - '0');
    }
    if (n == 4 && strncmp(code, "Key", 3) == 0 && code[3] >= 'A' && code[3] <= 'Z') {
        return (uint32_t)code[3];
    }
    if (n == 7 && strncmp(code, "Numpad", 6) == 0 && code[6] >= '0' && code[6] <= '9') {
        return (uint32_t)(96 + code[6] - '0');
    }
    if (code[0] == 'F' && n >= 2 && n <= 3 && code[1] >= '1' && code[1] <= '9') {
        unsigned f = (unsigned)(code[1] - '0');
        if (n == 3) {
            if (code[2] < '0' || code[2] > '9') {
                return 0;
            }
            f = f * 10 + (unsigned)(code[2] - '0');
        }
        return f >= 1 && f <= 24 ? 111 + f : 0;
    }
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++) {
        if (strcmp(tab[i].code, code) == 0) {
            return tab[i].vk;
        }
    }
    return 0;
}

/* The first code point of a UTF-8 string (0 for an empty or malformed one). */
static uint32_t first_code_point(const char *s)
{
    const unsigned char *u = (const unsigned char *)s;
    if (u[0] < 0x80) {
        return u[0];
    }
    int extra = u[0] >= 0xF0 ? 3 : u[0] >= 0xE0 ? 2 : u[0] >= 0xC0 ? 1 : -1;
    if (extra < 0) {
        return 0;
    }
    uint32_t cp = u[0] & (0x3Fu >> extra);
    for (int i = 1; i <= extra; i++) {
        if ((u[i] & 0xC0u) != 0x80u) {
            return 0;
        }
        cp = (cp << 6) | (u[i] & 0x3Fu);
    }
    return cp;
}

bool embi_impl_input_key(embi_bind_ctx *c, embi_node *target, const char *type, const embi_input_key *k)
{
    embi_bind_scope sc = embi_bind_enter(c);
    /* keydown and keyup both bubble and are cancelable. */
    uint32_t flags = EMBI_EVF_BUBBLES | EMBI_EVF_CANCELABLE | EMBI_EVF_COMPOSED;
    embi_js_value pl = payload(c);
    put(c, pl, "key", embi_bind_utf8_value(c, cstr(k->key ? k->key : "")));
    put(c, pl, "code", embi_bind_utf8_value(c, cstr(k->code ? k->code : "")));
    put(c, pl, "location", num(k->location));
    put(c, pl, "repeat", embi_js_from_bool(k->repeat));
    put(c, pl, "isComposing", embi_js_from_bool(k->isComposing));
    put_mods(c, pl, k->mods);
    /* The legacy codes: keydown and keyup carry the virtual key and no char code; keypress carries the character in
     * both (Enter is 13). */
    uint32_t kc;
    uint32_t cc = 0;
    if (strcmp(type, "keypress") == 0) {
        const char *kk = k->key != nullptr ? k->key : "";
        kc = cc = strcmp(kk, "Enter") == 0 ? 13u : first_code_point(kk);
    } else {
        kc = legacy_key_code(k->key, k->code);
    }
    put(c, pl, "keyCode", num(kc));
    put(c, pl, "charCode", num(cc));
    put(c, pl, "which", num(kc));
    embi_js_value ev = make(c, EMBI_IF_KeyboardEvent, type, flags, pl);
    bool ok = dispatch(c, target, ev);
    embi_bind_leave(sc, EMBI_JS_UNDEFINED);
    return ok;
}

bool embi_impl_input_before(embi_bind_ctx *c, embi_node *target, const char *input_type, const char *data,
                            bool is_composing)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value pl = payload(c);
    put(c, pl, "inputType", embi_bind_utf8_value(c, cstr(input_type)));
    put(c, pl, "data", data != nullptr ? embi_bind_utf8_value(c, cstr(data)) : EMBI_JS_NULL);
    put(c, pl, "isComposing", embi_js_from_bool(is_composing));
    embi_js_value ev = make(c, EMBI_IF_InputEvent, "beforeinput", EMBI_EVF_BUBBLES | EMBI_EVF_CANCELABLE | EMBI_EVF_COMPOSED, pl);
    bool ok = dispatch(c, target, ev);
    embi_bind_leave(sc, EMBI_JS_UNDEFINED);
    return ok;
}

void embi_impl_input_composition(embi_bind_ctx *c, embi_node *target, const char *type, const char *data)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value pl = payload(c);
    put(c, pl, "data", embi_bind_utf8_value(c, cstr(data ? data : "")));
    embi_js_value ev = make(c, EMBI_IF_CompositionEvent, type, EMBI_EVF_BUBBLES | EMBI_EVF_COMPOSED, pl);
    (void)dispatch(c, target, ev);
    embi_bind_leave(sc, EMBI_JS_UNDEFINED);
}

bool embi_impl_input_wheel(embi_bind_ctx *c, embi_node *target, const embi_input_wheel *w)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value pl = payload(c);
    put_mouse(c, pl, &w->ptr);
    put(c, pl, "deltaX", num(w->deltaX));
    put(c, pl, "deltaY", num(w->deltaY));
    put(c, pl, "deltaZ", num(w->deltaZ));
    put(c, pl, "deltaMode", num(w->deltaMode));
    embi_js_value ev = make(c, EMBI_IF_WheelEvent, "wheel", EMBI_EVF_BUBBLES | EMBI_EVF_CANCELABLE | EMBI_EVF_COMPOSED, pl);
    bool ok = dispatch(c, target, ev);
    embi_bind_leave(sc, EMBI_JS_UNDEFINED);
    return ok;
}

/* Whether the quadruple list of one record holds a live listener (or handler) of type. */
static bool list_has(embi_bind_ctx *c, embi_js_value list, embi_str type)
{
    if (!embi_js_is_object(list)) {
        return false;
    }
    uint32_t n = embi_bind_array_len(c, list);
    for (uint32_t i = 0; i + 3 < n; i += 4) {
        embi_js_value cb = embi_bind_array_get(c, list, i + 1);
        if (embi_js_is_null(cb) || embi_js_is_undefined(cb)) {
            continue; /* a cleared event handler attribute keeps its slot */
        }
        embi_dstr t = embi_dstr_empty();
        if (embi_bind_to_dstr(c, embi_bind_array_get(c, list, i), &t) && embi_dstr_eq_utf8(t, type)) {
            return true;
        }
    }
    return false;
}

bool embi_impl_input_listening(embi_bind_ctx *c, embi_node *target, const char *type)
{
    embi_str t = cstr(type);
    char on[40];
    bool have_on = t.len + 3 <= sizeof on;
    if (have_on) {
        on[0] = 'o';
        on[1] = 'n';
        memcpy(on + 2, type, t.len + 1);
    }
    embi_bind_scope sc = embi_bind_enter(c);
    bool found = false;
    for (embi_node *n = target; n != nullptr && !found; n = n->parent) {
        const embi_bind_obj *w = n->wrapper;
        if (w != nullptr && !embi_bind_stale(w) && list_has(c, w->listeners, t)) {
            found = true;
        } else if (!found && embi_is_element(n) && have_on && ((embi_element *)n)->attrs != nullptr &&
                   embi_el_get_attribute((embi_element *)n, (embi_str){on, t.len + 2}) != nullptr) {
            found = true;
        }
    }
    if (!found && c->gbo != nullptr) {
        found = list_has(c, c->gbo->listeners, t);
    }
    embi_bind_leave(sc, EMBI_JS_UNDEFINED);
    return found;
}

/* ------------------------------------------------------------------ focus */

static bool connected(const embi_node *n)
{
    return n != nullptr && (n->flags & EMBI_NF_CONNECTED) != 0;
}

/* One focus event at n (focus, blur, focusin or focusout) with relatedTarget related. */
static void focus_event(embi_bind_ctx *c, embi_node *n, const char *type, bool bubbles, embi_node *related)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value pl = payload(c);
    put(c, pl, "relatedTarget", related != nullptr ? embi_bind_node_value(c, related) : EMBI_JS_NULL);
    put(c, pl, "detail", num(0));
    embi_js_value ev = make(c, EMBI_IF_FocusEvent, type, bubbles ? EMBI_EVF_BUBBLES : 0, pl);
    (void)dispatch(c, n, ev);
    embi_bind_leave(sc, EMBI_JS_UNDEFINED);
}

void embi_impl_input_focus(embi_bind_ctx *c, embi_node *node)
{
    if (node != nullptr && !connected(node)) {
        return;
    }
    embi_node *old = c->active;
    if (old == node) {
        return;
    }
    if (old != nullptr) {
        /* Nothing has the focus while the old element's blur events run (document.activeElement is the body), so a
         * listener that moves the focus itself is a plain focus change that does not see this one half done. When it did
         * move the focus, that change stands and this one stops. */
        c->active = nullptr;
        if (connected(old)) {
            embi_forms_blur(c, old); /* W5e: the pending change event comes before blur */
            focus_event(c, old, "blur", false, node);
            if (c->active != nullptr) {
                return;
            }
            focus_event(c, old, "focusout", true, node);
            if (c->active != nullptr) {
                return;
            }
        }
    }
    if (node != nullptr && !connected(node)) {
        return; /* a listener removed the new element: the focus is nowhere */
    }
    c->active = node;
    if (node != nullptr) {
        embi_forms_focus(c, node); /* W5e */
        focus_event(c, node, "focus", false, old);
        if (c->active != node) {
            return;
        }
        focus_event(c, node, "focusin", true, old);
    }
}

void embi_impl_input_pre_remove(void *ctx, embi_node *node)
{
    embi_bind_ctx *c = ctx;
    if (c->moving || c->freed || !connected(c->active) || !connected(node) || !embi_contains(node, c->active)) {
        return;
    }
    embi_impl_input_focus(c, nullptr);
}

embi_node *embi_impl_input_focused(embi_bind_ctx *c)
{
    return connected(c->active) ? c->active : nullptr;
}

void embi_impl_input_host_focus(embi_bind_ctx *c, bool focused)
{
    if (!connected(c->active)) {
        return;
    }
    if (focused) {
        focus_event(c, c->active, "focus", false, nullptr);
        focus_event(c, c->active, "focusin", true, nullptr);
    } else {
        focus_event(c, c->active, "blur", false, nullptr);
        focus_event(c, c->active, "focusout", true, nullptr);
    }
}

/* ------------------------------------------------------------------ MouseEvent.movementX / movementY */

static double payload_num(embi_bind_ctx *c, embi_js_value obj, const char *k)
{
    if (embi_js_is_empty(obj) || !embi_js_is_object(obj)) {
        return 0;
    }
    embi_js_value v = embi_js_obj_get(c->rt, embi_js_as_object(obj), embi_js_key_c(c->rt, k), obj);
    if (v == EMBI_JS_EXCEPTION) {
        embi_js_exception_clear(c->rt);
        return 0;
    }
    double d = 0;
    if (embi_js_is_undefined(v) || !embi_js_to_number(c->rt, v, &d)) {
        embi_js_exception_clear(c->rt);
        return 0;
    }
    return d;
}

double embi_impl_MouseEvent_get_movementX(embi_bind_ctx *c, embi_bind_obj *self)
{
    return payload_num(c, self->ref, "movementX");
}

double embi_impl_MouseEvent_get_movementY(embi_bind_ctx *c, embi_bind_obj *self)
{
    return payload_num(c, self->ref, "movementY");
}

/* ------------------------------------------------------------------ per-context hooks, pointer capture */

#define EMBI_INPUT_HOOKS_MAX 64u

typedef struct hook_slot {
    embi_bind_ctx *c;
    embi_input_hooks h;
} hook_slot;

static hook_slot g_hooks[EMBI_INPUT_HOOKS_MAX];
static atomic_flag g_hooks_lock = ATOMIC_FLAG_INIT;

static void hooks_lock(void)
{
    while (atomic_flag_test_and_set_explicit(&g_hooks_lock, memory_order_acquire)) {
    }
}

static void hooks_unlock(void)
{
    atomic_flag_clear_explicit(&g_hooks_lock, memory_order_release);
}

bool embi_impl_input_attach(embi_bind_ctx *c, const embi_input_hooks *h)
{
    bool ok = false;
    hooks_lock();
    hook_slot *free_slot = nullptr;
    for (uint32_t i = 0; i < EMBI_INPUT_HOOKS_MAX; i++) {
        if (g_hooks[i].c == c) {
            g_hooks[i].h = *h;
            ok = true;
            free_slot = nullptr;
            break;
        }
        if (g_hooks[i].c == nullptr && free_slot == nullptr) {
            free_slot = &g_hooks[i];
        }
    }
    if (!ok && free_slot != nullptr) {
        free_slot->c = c;
        free_slot->h = *h;
        ok = true;
    }
    hooks_unlock();
    return ok;
}

void embi_impl_input_detach(embi_bind_ctx *c)
{
    hooks_lock();
    for (uint32_t i = 0; i < EMBI_INPUT_HOOKS_MAX; i++) {
        if (g_hooks[i].c == c) {
            g_hooks[i].c = nullptr;
            memset(&g_hooks[i].h, 0, sizeof g_hooks[i].h);
        }
    }
    hooks_unlock();
}

/* A copy of the hooks of c (all zero when none are attached). */
static embi_input_hooks hooks_of(embi_bind_ctx *c)
{
    embi_input_hooks h;
    memset(&h, 0, sizeof h);
    hooks_lock();
    for (uint32_t i = 0; i < EMBI_INPUT_HOOKS_MAX; i++) {
        if (g_hooks[i].c == c) {
            h = g_hooks[i].h;
            break;
        }
    }
    hooks_unlock();
    return h;
}

bool embi_impl_input_can_focus(const embi_element *el)
{
    return embi_pf_focusable(el);
}

embi_dom_err embi_impl_Element_setPointerCapture(embi_bind_ctx *c, embi_node *self, int32_t a0)
{
    embi_input_hooks h = hooks_of(c);
    if (h.capture_set == nullptr || !h.capture_set(h.user, a0, self)) {
        return EMBI_DOM_ERR_NOT_FOUND;
    }
    return EMBI_DOM_OK;
}

embi_dom_err embi_impl_Element_releasePointerCapture(embi_bind_ctx *c, embi_node *self, int32_t a0)
{
    embi_input_hooks h = hooks_of(c);
    if (h.capture_release == nullptr) {
        return EMBI_DOM_ERR_NOT_FOUND;
    }
    h.capture_release(h.user, a0, self); /* releases only when self holds the capture */
    return EMBI_DOM_OK;
}

bool embi_impl_Element_hasPointerCapture(embi_bind_ctx *c, embi_node *self, int32_t a0)
{
    embi_input_hooks h = hooks_of(c);
    return h.capture_has != nullptr && h.capture_has(h.user, a0, self);
}

/* ------------------------------------------------------------------ activation behaviors */

static bool has_attr(embi_node *el, const char *name)
{
    return embi_el_has_attribute((embi_element *)el, cstr(name));
}

static bool html_tag(const embi_node *n, uint32_t name)
{
    return n->extra == EMBI_NS_HTML && n->name == name && embi_is_element(n);
}

/* An SVG a element (R4c): a link through href, or xlink:href when href is absent (SVG 2 section 16.1). */
static bool svg_a(const embi_node *n)
{
    return n->extra == EMBI_NS_SVG && n->name == EMBI_ATOM_a && embi_is_element(n);
}

/* a[href] (and the SVG a with href or xlink:href): the navigation of the link goes to the sink with the raw href. */
static bool a_applies(embi_bind_ctx *c, embi_node *el)
{
    (void)c;
    if (svg_a(el)) {
        return has_attr(el, "href") || has_attr(el, "xlink:href");
    }
    return html_tag(el, EMBI_ATOM_a) && has_attr(el, "href");
}

static void a_run(embi_bind_ctx *c, embi_node *el, embi_bind_obj *event)
{
    (void)event;
    const embi_attr *a = embi_el_get_attribute((embi_element *)el, cstr("href"));
    if (a == nullptr && svg_a(el)) {
        a = embi_el_get_attribute((embi_element *)el, cstr("xlink:href"));
    }
    if (a == nullptr) {
        return;
    }
    embi_sb sb = {0};
    embi_dstr_append_utf8(&sb, a->value);
    embi_impl_follow_link(c, (embi_str){sb.data, sb.len});
    embi_sb_free(&sb);
}

static bool labelable(const embi_node *n)
{
    if (!embi_is_element(n) || n->extra != EMBI_NS_HTML) {
        return false;
    }
    return n->name == EMBI_ATOM_input || n->name == EMBI_ATOM_select || n->name == EMBI_ATOM_textarea ||
           n->name == EMBI_ATOM_button;
}

/* The first labelable descendant of n in tree order. */
static embi_node *first_labelable(embi_node *n)
{
    for (embi_node *k = n->first_child; k != nullptr; k = k->next) {
        if (labelable(k)) {
            return k;
        }
        embi_node *r = first_labelable(k);
        if (r != nullptr) {
            return r;
        }
    }
    return nullptr;
}

/* The labeled control: the element of the for attribute, else the first labelable descendant. */
static embi_node *label_control(embi_bind_ctx *c, embi_node *label)
{
    const embi_attr *f = embi_el_get_attribute((embi_element *)label, cstr("for"));
    if (f != nullptr) {
        embi_sb sb = {0};
        embi_dstr_append_utf8(&sb, f->value);
        embi_element *el = embi_get_element_by_id(embi_bind_document(c), (embi_str){sb.data, sb.len});
        embi_sb_free(&sb);
        return el != nullptr && labelable(&el->base) ? &el->base : nullptr;
    }
    return first_labelable(label);
}

/* Whether a node between target and label is interactive content (a click there is not the label's). */
static bool interactive_between(const embi_node *target, const embi_node *label)
{
    for (const embi_node *k = target; k != nullptr && k != label; k = k->parent) {
        if (!embi_is_element(k) || k->extra != EMBI_NS_HTML) {
            continue;
        }
        if (k->name == EMBI_ATOM_button || k->name == EMBI_ATOM_input || k->name == EMBI_ATOM_select ||
            k->name == EMBI_ATOM_textarea || k->name == EMBI_ATOM_a) {
            return true;
        }
    }
    return false;
}

static void click_node(embi_bind_ctx *c, embi_node *n)
{
    embi_bind_scope sc = embi_bind_enter(c);
    embi_js_value ev = make(c, EMBI_IF_MouseEvent, "click",
                            EMBI_EVF_BUBBLES | EMBI_EVF_CANCELABLE | EMBI_EVF_COMPOSED, EMBI_JS_UNDEFINED);
    (void)dispatch(c, n, ev);
    embi_bind_leave(sc, EMBI_JS_UNDEFINED);
}

static void label_run(embi_bind_ctx *c, embi_node *label, embi_bind_obj *event)
{
    embi_event *ev = event->ptr;
    const embi_node *target = ev != nullptr ? (const embi_node *)ev->target.obj : nullptr;
    if (target != nullptr && interactive_between(target, label)) {
        return; /* a click on a control inside the label belongs to the control */
    }
    embi_node *control = label_control(c, label);
    if (control != nullptr && !embi_pf_disabled((const embi_element *)control)) {
        if (embi_is_element(control) && embi_pf_focusable((const embi_element *)control)) {
            embi_impl_input_focus(c, control);
        }
        click_node(c, control);
    }
}

static bool label_applies(embi_bind_ctx *c, embi_node *el)
{
    (void)c;
    return html_tag(el, EMBI_ATOM_label);
}

static bool button_applies(embi_bind_ctx *c, embi_node *el)
{
    (void)c;
    return html_tag(el, EMBI_ATOM_button) && !has_attr(el, "disabled");
}

/* The button's own activation (submit and reset) is W5e's, with the form association; a plain button has none. */
static void button_run(embi_bind_ctx *c, embi_node *el, embi_bind_obj *event)
{
    (void)c;
    (void)el;
    (void)event;
}

static const embi_activation k_anchor = {.applies = a_applies, .run = a_run};
static const embi_activation k_label = {.applies = label_applies, .run = label_run};
static const embi_activation k_button = {.applies = button_applies, .run = button_run};

void embi_impl_input_ready(void)
{
    static atomic_int state; /* 0: not registered, 1: registering, 2: done */
    int expected = 0;
    if (atomic_compare_exchange_strong(&state, &expected, 1)) {
        (void)embi_bind_register_activation("a", &k_anchor);
        (void)embi_bind_register_activation("label", &k_label);
        (void)embi_bind_register_activation("button", &k_button);
        atomic_store(&state, 2);
    } else {
        while (atomic_load(&state) != 2) {
        }
    }
}
