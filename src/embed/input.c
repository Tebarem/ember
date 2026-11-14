/*
 * The public input API of a view (W5d, include/ember/ember_input.h): the pointer, touch, wheel, keyboard, text,
 * composition, clipboard and host focus calls. Each one finds the view's binding context, updates the view's input state
 * (the hover and press targets, the touch fingers, the pointer captures, the click counts, the composition), dispatches
 * the DOM events through src/bindings/impl_input.c and runs the default actions that are not canceled: focus on press,
 * Tab navigation, link and button activation through Enter and Space, the scroll keys, the wheel scroll and the clipboard
 * commands.
 *
 * The state lives in the view (emb_view.input) and is freed with it. The binding context comes from the script layer
 * (embi_script_bind), which makes the realm on the first call: a view whose page has no script pays for the realm only
 * once it takes input. The nodes the state holds (the hover, press and capture targets) are pinned in that context; the
 * state is reset before a document is replaced and destroyed before the view's document, so a pin never outlives its
 * context.
 *
 * Event order. A pointer change dispatches the pointer events first and then the compatibility mouse events, as browsers
 * do: pointerout, pointerleave, pointerover, pointerenter, pointermove, then mouseout, mouseleave, mouseover, mouseenter,
 * mousemove. Nodes removed by a listener are skipped (no event reaches a node that left the document). Events that no
 * listener on their path can see are not built at all (embi_impl_input_listening), except click, whose activation
 * behavior must run.
 *
 * Style invalidation. The hover, press and focus targets are mirrored into the selector hook's state (embi_pf_state), but
 * the document is restyled only when something reads that state (embi_input_set_state_consumed): a pointer move across
 * an element boundary of a page with no :hover rule costs no relayout.
 */
#include "embed/input_int.h"

#include "embed_int.h"
#include "embed/frames.h"
#include "embed/script_int.h"

#include "base/base.h"
#include "base/clock.h"
#include "bindings/bind.h"
#include "bindings/impl.h"
#include "bindings/impl_forms.h"
#include "bindings/impl_input.h"
#include "layout/layout.h"
#include "platform/platform.h"
#include "style/style.h"

#include <ember/ember_input.h>

#include <string.h>

#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_stdinc.h>

#define EMBI_IN_MAX EMBI_PF_MAX_DEPTH
#define EMBI_IN_TOUCHES 10u
#define EMBI_IN_CAPTURES 4u
/* A finger that moved farther than this (CSS px) from where it went down is a scroll or a drag, not a tap. */
#define EMBI_IN_TAP_SLOP 12.0f
/* Nodes looked at per inserted subtree when searching for an autofocus element. */
#define EMBI_IN_AUTOFOCUS_BUDGET 4096u

typedef struct embi_input embi_input;

/* The hover change of one pointer move or button event: the old and new targets, and the counts of the leave and enter
 * lists (in the scratch arrays). old_pin holds the old target's pin until the events are dispatched. */
typedef struct embi_hover_chg {
    embi_element *old, *leaf;
    uint32_t nleave, nenter;
    uint32_t old_pin;
    bool any;
} embi_hover_chg;

/* One finger on the screen. The press target is pinned while the finger is down and is where the finger's events go
 * (implicit pointer capture). */
typedef struct embi_touch {
    bool used;
    bool primary;
    bool moved;
    uint64_t id;
    int32_t pid;           /* pointerId (>= 2: 1 is the mouse) */
    embi_element *press;   /* pinned, may be nullptr (the finger went down over nothing) */
    uint32_t press_pin;
    float sx, sy;          /* where it went down */
} embi_touch;

/* An explicit pointer capture (setPointerCapture). */
typedef struct embi_capture {
    int32_t id;            /* 0: free */
    embi_element *el;      /* pinned */
    uint32_t pin;
} embi_capture;

struct embi_input {
    emb_view *view;
    embi_bind_ctx *pin_ctx;      /* the context the pins below belong to (nullptr when none) */
    embi_bind_ctx *hooks_ctx;    /* the context the pointer capture hooks are attached to */
    embi_element *hover;         /* the element under the pointer (pinned), nullptr when none */
    uint32_t hover_pin;
    embi_element *press;         /* the mouse press target (pinned) until the button is released */
    uint32_t press_pin;
    embi_hover_chg ch;
    embi_pf_clicks clicks;
    uint32_t click_pin;          /* pins clicks.target so that its address cannot be reused while it is remembered */
    uint32_t down;               /* the DOM button bits held (primary 1, secondary 2, auxiliary 4, back 8, forward 16) */
    uint32_t mods;
    float px, py;                /* the last pointer position, CSS px */
    embi_touch touch[EMBI_IN_TOUCHES];
    embi_capture cap[EMBI_IN_CAPTURES];
    bool composing;
    char *comp;                  /* the composition text (SDL_malloc'd), valid while composing */
    uint32_t comp_len;
    int32_t comp_cursor, comp_sel; /* the caret and the selection length of the composition, in bytes of comp */
    embi_node *comp_target;      /* the node the composition started on (pinned), nullptr when it has none */
    uint32_t comp_pin;
    bool comp_edit;              /* composition events were sent to comp_target (it was a text control) */
    embi_node *space_target;     /* the node a Space keydown went to (pinned) until the keyup */
    uint32_t space_pin;
    emb_navigate_fn nav;
    void *nav_user;
    /* A drag of the thumb of a styled scrollbar (the press went to the bar, not to the page). */
    bool sb_drag;
    bool sb_vertical;
    const void *sb_key;          /* the scroller whose thumb is dragged */
    float sb_grab;               /* the pointer's distance along the bar from the start of the thumb */
    uint32_t forms_gen;          /* W5e: the generation of the form state the display list was last built for */
    uint32_t caret_phase;        /* W5e: the blink phase of the caret at that time */
    emb_navigate_ex_fn nav_ex;   /* W5e: the form-aware hook */
    void *nav_ex_user;
    embi_pf_state st;
    embi_pf_state seen;          /* the state the styles were last made for (state_mark compares with it) */
    /* CF1: per target of seen (hover, active, focus): its pin while seen holds it, and a hash of the element chain it had
     * when it became the target. A later change restyles only the elements whose state flips when the old target is still
     * in the document with the same chain (seen_ok), else the whole document. */
    uint32_t seen_pin[3];
    uint64_t seen_hash[3];
    bool seen_ok[3];
    bool state_used;             /* an author rule reads :hover or :active: restyle when they change */
    embi_style_engine *hooked;   /* the style engine the selector hook is installed in (nullptr: none yet) */
    int32_t cur_kind;            /* the cursor kind the window shows (or, offscreen, the kind it would show), see cursor_track */
    bool cur_known;              /* cur_kind is valid (false after a reset: the next track sets it) */
    bool text_on;                /* the SDL text input is on for a text control */
    float caret[4];              /* the caret rectangle last given to the window */
    embi_input_selection_fn sel_fn;
    void *sel_user;
    /* Autofocus. */
    embi_document *obs_doc;      /* the document the insertion observer is registered on */
    bool autofocus_done;         /* an autofocus element took the focus, or the user did something */
    bool autofocus_queued;
    /* Scratch arrays of the hover and click logic (chains, leave and enter lists), kept here so that no event path puts
     * them on the stack. */
    embi_element **scratch_mem;  /* EMBI_IN_SCR * EMBI_IN_MAX pointers, allocated by the first use (ensure_scratch) */
};

/* The scratch arrays: 0 to 3 for the hover and click logic, 4 and 5 for the selector state. state_flips holds the old
 * chain in 4 and the new one in 5 while it marks; state_record, which runs after it, reuses 4 for the new chain's hash. */
#define EMBI_IN_SCR 6u

/* The k-th scratch array (EMBI_IN_MAX entries). */
#define SCR(in, k) ((in)->scratch_mem + (size_t)(k) * EMBI_IN_MAX)

/* ------------------------------------------------------------------ the per-view state */

/* Allocates the scratch arrays of the hover and click logic. False when memory is out (the call then does nothing). */
static bool ensure_scratch(embi_input *in)
{
    if (in->scratch_mem == nullptr) {
        in->scratch_mem = embi_calloc(EMBI_IN_SCR * EMBI_IN_MAX, sizeof(embi_element *));
    }
    return in->scratch_mem != nullptr;
}

static embi_input *state_of(emb_view *v)
{
    if (v->input == nullptr) {
        v->input = embi_calloc(1, sizeof(struct embi_input));
        if (v->input == nullptr) {
            return nullptr;
        }
        v->input->view = v;
    }
    return v->input;
}

embi_pf_state *embi_input_pf_state(emb_view *v)
{
    embi_input *in = state_of(v);
    return in != nullptr ? &in->st : nullptr;
}

void embi_input_set_state_consumed(emb_view *v, bool used)
{
    embi_input *in = v != nullptr ? state_of(v) : nullptr;
    if (in == nullptr) {
        return;
    }
    bool was = in->state_used;
    in->state_used = used;
    if (used && !was && v->doc != nullptr) {
        /* The rules see the current state from now on: the document is restyled, not only relaid out. */
        embi_style_mark_dirty(embi_doc_style_engine(v->doc), embi_doc_dom(v->doc));
        embi_doc_invalidate(v->doc);
        v->paint_dirty = true;
    }
}

bool embi_input_state_consumed(emb_view *v)
{
    return v != nullptr && v->input != nullptr && v->input->state_used;
}

void embi_input_set_selection_provider(emb_view *v, embi_input_selection_fn fn, void *user)
{
    embi_input *in = v != nullptr ? state_of(v) : nullptr;
    if (in != nullptr) {
        in->sel_fn = fn;
        in->sel_user = user;
    }
}

embi_element *embi_input_hover_element(emb_view *v)
{
    return (v != nullptr && v->input != nullptr) ? v->input->hover : nullptr;
}

static void unpin(embi_input *in, uint32_t *tok)
{
    if (*tok != 0 && in->pin_ctx != nullptr) {
        embi_bind_unpin_node(in->pin_ctx, *tok);
    }
    *tok = 0;
}

static uint32_t pin_node(embi_input *in, embi_bind_ctx *c, embi_node *n)
{
    in->pin_ctx = c;
    return embi_bind_pin_node(c, n);
}

/* Releases every pin and forgets the targets (their context must still be alive). */
static void drop_pins(embi_input *in)
{
    unpin(in, &in->hover_pin);
    unpin(in, &in->press_pin);
    unpin(in, &in->ch.old_pin);
    unpin(in, &in->click_pin);
    for (uint32_t k = 0; k < 3; k++) {
        unpin(in, &in->seen_pin[k]);
        in->seen_ok[k] = false;
    }
    unpin(in, &in->comp_pin);
    unpin(in, &in->space_pin);
    in->comp_target = nullptr;
    in->comp_edit = false;
    in->space_target = nullptr;
    for (uint32_t i = 0; i < EMBI_IN_TOUCHES; i++) {
        unpin(in, &in->touch[i].press_pin);
        memset(&in->touch[i], 0, sizeof in->touch[i]);
    }
    for (uint32_t i = 0; i < EMBI_IN_CAPTURES; i++) {
        unpin(in, &in->cap[i].pin);
        memset(&in->cap[i], 0, sizeof in->cap[i]);
    }
    in->hover = nullptr;
    in->press = nullptr;
    in->pin_ctx = nullptr;
}

static embi_text_area_call g_text_area; /* the last call, for the tests */

/* Text input on v's window: a frame has none, so its root's window takes it, with the caret rectangle in the root's
 * viewport (IF1). */
static void text_area(emb_view *v, bool on, float x, float y, float w, float h)
{
    emb_view *root = embi_frames_root(v);
    if (root != v) {
        /* Only the frame that has the keyboard (every parent up to the root focuses the frame below it) starts it. */
        for (emb_view *k = v, *p = embi_frames_parent(v); on && p != nullptr; k = p, p = embi_frames_parent(p)) {
            embi_frame_route *r = p->frames != nullptr ? embi_frames_route(p) : nullptr;
            if (r == nullptr || r->focus != k) {
                return;
            }
        }
        float rx = 0.0f, ry = 0.0f;
        if (on && !embi_frames_to_root(v, x, y, &rx, &ry)) {
            return;
        }
        x = rx;
        y = ry;
    }
    g_text_area = (embi_text_area_call){root, on, {x, y, w, h}};
    embi_window_text_input(root, on, x, y, w, h);
}

embi_text_area_call embi_input_last_text_area(void)
{
    return g_text_area;
}

static void drop_text(emb_view *v, embi_input *in)
{
    if (in->text_on) {
        text_area(v, false, 0, 0, 0, 0);
        in->text_on = false;
    }
    if (in->comp != nullptr) {
        SDL_free(in->comp);
        in->comp = nullptr;
    }
    in->comp_len = 0;
    in->composing = false;
    in->comp_cursor = 0;
    in->comp_sel = 0;
}

/* Removes what the input layer put into the document and the bindings (the insertion observer, the capture hooks). */
static void detach(emb_view *v, embi_input *in)
{
    if (in->obs_doc != nullptr) {
        embi_document_unobserve(in->obs_doc, in);
        in->obs_doc = nullptr;
    }
    if (in->hooks_ctx != nullptr) {
        embi_impl_input_detach(in->hooks_ctx);
        in->hooks_ctx = nullptr;
    }
    (void)v;
}

void embi_input_destroy(emb_view *v)
{
    if (v == nullptr || v->input == nullptr) {
        return;
    }
    embi_input *in = v->input;
    detach(v, in);
    drop_pins(in);
    drop_text(v, in);
    if (in->scratch_mem != nullptr) {
        embi_free(in->scratch_mem, EMBI_IN_SCR * EMBI_IN_MAX * sizeof(embi_element *));
    }
    embi_free(in, sizeof *in);
    v->input = nullptr;
}

void embi_input_reset(emb_view *v)
{
    if (v == nullptr || v->input == nullptr) {
        return;
    }
    embi_input *in = v->input;
    detach(v, in);
    drop_pins(in);
    drop_text(v, in);
    in->down = 0;
    in->ch.any = false;
    in->clicks = (embi_pf_clicks){0};
    in->st = (embi_pf_state){0};
    in->seen = (embi_pf_state){0};
    in->hooked = nullptr;
    in->cur_known = false;
    in->cur_kind = -1;
    in->px = 0;
    in->py = 0;
    in->autofocus_done = false;
    in->autofocus_queued = false;
}

bool embi_input_has_navigate(const emb_view *v)
{
    return v != nullptr && v->input != nullptr && v->input->nav != nullptr;
}

void embi_input_navigate(emb_view *v, embi_str url)
{
    embi_input *in = v != nullptr ? v->input : nullptr;
    if (in == nullptr || in->nav == nullptr || url.len == 0 || url.ptr == nullptr) {
        return;
    }
    in->nav(in->nav_user, url.ptr, url.len);
}

bool embi_input_navigate_form(emb_view *v, embi_str method, embi_str url, embi_str body, embi_str ctype)
{
    embi_input *in = v != nullptr ? v->input : nullptr;
    if (in == nullptr || in->nav_ex == nullptr || url.len == 0) {
        return false;
    }
    char *m = SDL_strndup(method.ptr, method.len);
    char *u = SDL_strndup(url.ptr, url.len);
    char *b = SDL_strndup(body.ptr != nullptr ? body.ptr : "", body.len);
    char *ct = SDL_strndup(ctype.ptr != nullptr ? ctype.ptr : "", ctype.len);
    if (m != nullptr && u != nullptr && b != nullptr && ct != nullptr) {
        /* body may hold NULs (multipart with binary): the length travels separately */
        if (body.len > 0 && body.ptr != nullptr) {
            SDL_free(b);
            b = SDL_malloc(body.len + 1);
            if (b != nullptr) {
                SDL_memcpy(b, body.ptr, body.len);
                b[body.len] = '\0';
            }
        }
        if (b != nullptr) {
            emb_navigation nav = {sizeof nav, m, u, url.len, b, body.len, ct};
            in->nav_ex(in->nav_ex_user, &nav);
        }
    }
    SDL_free(m);
    SDL_free(u);
    SDL_free(b);
    SDL_free(ct);
    return true;
}

/* ------------------------------------------------------------------ context, hit testing, helpers */

static bool live(const embi_node *n);
static void capture_hooks(embi_input *in, embi_bind_ctx *c);
static void comp_check(embi_input *in, embi_bind_ctx *c);

/* The binding context of the view's document (made on first use), with the input layer's hooks attached to it. */
static embi_bind_ctx *ctx_of(emb_view *v)
{
    if (v->doc == nullptr) {
        return nullptr;
    }
    embi_script *s = embi_doc_script(v->doc);
    embi_bind_ctx *c = s != nullptr ? embi_script_bind(s) : nullptr;
    if (c != nullptr) {
        embi_impl_input_ready();
        embi_forms_ready(); /* the form controls (W5e) replace the button behavior */
        embi_input *in = state_of(v);
        if (in != nullptr && in->hooks_ctx != c) {
            capture_hooks(in, c);
        }
    }
    return c;
}

/* The element under the viewport point (x, y), CSS px, after the layout is current. The point is in viewport coordinates:
 * the root's scroll offset is applied by the hit walk itself, as paint applies it, so nothing is added here. */
static embi_element *hit(emb_view *v, embi_bind_ctx *c, float x, float y)
{
    if (c == nullptr || embi_bind_layout(c) == nullptr) {
        return nullptr;
    }
    embi_view_layout(v);
    return embi_layout_hit_test(embi_bind_layout(c), embi_bind_document(c), x, y);
}

static bool live(const embi_node *n)
{
    return n != nullptr && (n->flags & EMBI_NF_CONNECTED) != 0;
}

static bool is_tag(const embi_node *n, uint32_t name)
{
    return n != nullptr && embi_is_element(n) && n->extra == EMBI_NS_HTML && n->name == name;
}

static bool has_attr(const embi_node *n, const char *name)
{
    return embi_el_has_attribute((embi_element *)(uintptr_t)n, (embi_str){name, (uint32_t)SDL_strlen(name)});
}

/* The type attribute of an input, lowercase ASCII into buf (cap bytes). Returns false when absent or too long. */
static bool input_type(const embi_node *n, char *buf, uint32_t cap)
{
    const embi_attr *a = embi_el_get_attribute((embi_element *)(uintptr_t)n, EMBI_STR("type"));
    if (a == nullptr || a->value.len == 0 || a->value.len >= cap) {
        return false;
    }
    for (uint32_t i = 0; i < a->value.len; i++) {
        uint16_t u = embi_dstr_unit(a->value, i);
        if (u >= 'A' && u <= 'Z') {
            u = (uint16_t)(u - 'A' + 'a');
        }
        buf[i] = u < 0x80 ? (char)u : '?';
    }
    buf[a->value.len] = '\0';
    return true;
}

static bool input_type_is(const embi_node *n, const char *t)
{
    char buf[32];
    return is_tag(n, EMBI_ATOM_input) && input_type(n, buf, sizeof buf) && SDL_strcmp(buf, t) == 0;
}

/* A text control: a textarea, or an input that takes typed text (no type, an unknown type, or a text-like type), that is
 * neither disabled nor read-only. Its editing is W5e's; the input layer gates beforeinput, composition and the SDL text
 * input on it. */
static bool text_control(const embi_node *n)
{
    if (n == nullptr || !embi_is_element(n) || n->extra != EMBI_NS_HTML) {
        return false;
    }
    if (has_attr(n, "disabled") || has_attr(n, "readonly")) {
        return false;
    }
    if (n->name == EMBI_ATOM_textarea) {
        return true;
    }
    if (n->name != EMBI_ATOM_input) {
        return false;
    }
    char buf[32];
    if (!input_type(n, buf, sizeof buf)) {
        return true; /* the default type is text */
    }
    static const char *const types[] = {"text", "search", "url", "tel", "email", "password", "number"};
    static const char *const others[] = {"hidden", "date", "month", "week", "time", "datetime-local", "range", "color",
                                         "checkbox", "radio", "file", "submit", "image", "reset", "button"};
    for (uint32_t i = 0; i < sizeof types / sizeof types[0]; i++) {
        if (SDL_strcmp(buf, types[i]) == 0) {
            return true;
        }
    }
    for (uint32_t i = 0; i < sizeof others / sizeof others[0]; i++) {
        if (SDL_strcmp(buf, others[i]) == 0) {
            return false;
        }
    }
    return true; /* an unknown type is the Text state */
}

/* The DOM bit of a button in MouseEvent.buttons (the button numbers of the DOM differ from the bits). */
static uint32_t button_bit(emb_button b)
{
    switch (b) {
    case EMB_BUTTON_PRIMARY:
        return 1u;
    case EMB_BUTTON_SECONDARY:
        return 2u;
    case EMB_BUTTON_AUXILIARY:
        return 4u;
    case EMB_BUTTON_BACK:
        return 8u;
    case EMB_BUTTON_FORWARD:
        return 16u;
    }
    return 0u;
}

/* The payload of a pointer event at (x, y): the client, screen and page coordinates; the target's offsets are set per
 * target by ptr_event. */
static embi_input_ptr payload(emb_view *v, const embi_input *in, float x, float y, int16_t button, uint32_t buttons,
                              int32_t detail, bool touch, int32_t id, bool primary)
{
    embi_input_ptr p;
    memset(&p, 0, sizeof p);
    float ox = 0, oy = 0;
    if (v->doc != nullptr) {
        embi_doc_root_offset(v->doc, &ox, &oy);
    }
    p.clientX = x;
    p.clientY = y;
    p.screenX = x;
    p.screenY = y;
    p.pageX = (double)x + (double)ox;
    p.pageY = (double)y + (double)oy;
    p.button = button;
    p.buttons = (uint16_t)buttons;
    p.detail = detail;
    p.mods = in->mods;
    p.pointerType = touch ? "touch" : "mouse";
    p.pointerId = id;
    p.isPrimary = primary;
    return p;
}

/* Sets the offsets of the payload for target el: the position in the target's border box (where it is painted; the padding
 * box is not separated yet). */
static void offsets(embi_bind_ctx *c, embi_element *el, embi_input_ptr *p)
{
    embi_layout_engine *e = embi_bind_layout(c);
    embi_rectf r;
    if (e != nullptr && embi_layout_painted_box(e, el, &r)) {
        p->offsetX = p->pageX - (double)r.x;
        p->offsetY = p->pageY - (double)r.y;
    } else {
        p->offsetX = p->clientX;
        p->offsetY = p->clientY;
    }
}

/* Dispatches one pointer or mouse event at el with the payload base. A node that left the document gets no event. An
 * event that no listener on its path could see is not built unless always is set (a click, whose activation behavior has
 * to run). Returns true when the event was not canceled (or was not dispatched). */
static bool ptr_event_ex(embi_bind_ctx *c, embi_element *el, const embi_input_ptr *base, bool pointer, const char *type,
                         bool bubbles, bool cancelable, bool always)
{
    if (c == nullptr || el == nullptr || !live(&el->base)) {
        return true;
    }
    if (!always && !embi_impl_input_listening(c, &el->base, type)) {
        return true;
    }
    embi_input_ptr p = *base;
    offsets(c, el, &p);
    return embi_impl_input_pointer(c, &el->base, pointer, type, &p, bubbles, cancelable);
}

static bool ptr_event(embi_bind_ctx *c, embi_element *el, const embi_input_ptr *base, bool pointer, const char *type,
                      bool bubbles, bool cancelable)
{
    return ptr_event_ex(c, el, base, pointer, type, bubbles, cancelable, false);
}

/* The common ancestor-or-self of a and b (both elements): the target of a click whose press and release differ. Walks the
 * two chains from the root end (linear). */
static embi_element *common_ancestor(embi_input *in, embi_element *a, embi_element *b)
{
    embi_element **ca = SCR(in, 0), **cb = SCR(in, 1);
    uint32_t na = embi_pf_chain(&a->base, ca, EMBI_IN_MAX);
    uint32_t nb = embi_pf_chain(&b->base, cb, EMBI_IN_MAX);
    embi_element *last = nullptr;
    uint32_t i = na, j = nb;
    while (i > 0 && j > 0 && ca[i - 1] == cb[j - 1]) {
        last = ca[i - 1];
        i--;
        j--;
    }
    return last;
}

/* Whether a click on n is swallowed: n is, or is inside, a disabled form control (no click reaches it, as in browsers). */
static bool click_blocked(const embi_node *n)
{
    for (; n != nullptr; n = n->parent) {
        if (embi_is_element(n) && embi_pf_disabled((const embi_element *)n)) {
            return true;
        }
    }
    return false;
}

/* Sets the mouse press target (pinned while the button is down). */
static void set_press(embi_input *in, embi_bind_ctx *c, embi_element *n)
{
    unpin(in, &in->press_pin);
    in->press = n;
    in->press_pin = (n != nullptr && c != nullptr) ? pin_node(in, c, &n->base) : 0;
}

/* The element the first finger on the screen pressed (the :active target when no mouse button is down). */
static embi_element *touch_active(const embi_input *in)
{
    for (uint32_t i = 0; i < EMBI_IN_TOUCHES; i++) {
        if (in->touch[i].used && in->touch[i].press != nullptr) {
            return in->touch[i].press;
        }
    }
    return nullptr;
}

/* ------------------------------------------------------------------ scrolling a focused element into view */

/* Scrolls the root so that el's border box is visible, with the least movement (an element taller or wider than the view
 * aligns its start). Nested scroll containers are not scrolled. A fixed box of the viewport is where it is painted
 * whatever the offset, so it never moves the root. */
static void scroll_into_view(emb_view *v, embi_bind_ctx *c, embi_element *el)
{
    embi_layout_engine *e = c != nullptr ? embi_bind_layout(c) : nullptr;
    embi_rectf r;
    if (e == nullptr || el == nullptr || !embi_layout_painted_box(e, el, &r)) {
        return;
    }
    float ox = 0, oy = 0;
    embi_doc_root_offset(v->doc, &ox, &oy);
    float vw = (float)v->css_w, vh = (float)v->css_h;
    float dx = 0, dy = 0;
    if (r.y < oy) {
        dy = r.y - oy;
    } else if (r.y + r.h > oy + vh) {
        dy = r.h > vh ? r.y - oy : (r.y + r.h) - (oy + vh);
    }
    if (r.x < ox) {
        dx = r.x - ox;
    } else if (r.x + r.w > ox + vw) {
        dx = r.w > vw ? r.x - ox : (r.x + r.w) - (ox + vw);
    }
    if (dx != 0 || dy != 0) {
        embi_view_scroll_root(v, dx, dy);
    }
}

/* ------------------------------------------------------------------ sync of the state with the document and the window */

/* Mirrors the input state into the window (the text input and the caret rectangle). */
static void text_sync(emb_view *v, embi_input *in, embi_bind_ctx *c, embi_node *f)
{
    bool on = f != nullptr && text_control(f);
    float r[4] = {0, 0, 0, 0};
    if (on && c != nullptr) {
        embi_layout_engine *e = embi_bind_layout(c);
        float ox = 0, oy = 0;
        embi_rectf box;
        embi_doc_root_offset(v->doc, &ox, &oy);
        if (e != nullptr && embi_layout_painted_box(e, (embi_element *)f, &box)) {
            r[0] = box.x - ox;
            r[1] = box.y - oy;
            r[2] = box.w;
            r[3] = box.h;
            float bb[4] = {r[0], r[1], r[2], r[3]};
            float cr[4];
            if (embi_doc_control_caret(v->doc, (const embi_element *)f, bb, cr)) {
                /* W5e: the input method gets the caret's rectangle, so its window sits by the text being composed */
                r[0] = cr[0];
                r[1] = cr[1];
                r[2] = cr[2];
                r[3] = cr[3];
            }
        }
    }
    bool moved = on && (r[0] != in->caret[0] || r[1] != in->caret[1] || r[2] != in->caret[2] || r[3] != in->caret[3]);
    if (on != in->text_on || moved) {
        text_area(v, on, r[0], r[1], r[2], r[3]);
        in->text_on = on;
        SDL_memcpy(in->caret, r, sizeof r);
    }
}

/* ------------------------------------------------------------------ form controls (W5e) */

bool embi_input_control_state(void *user, const struct embi_element *el, void *out)
{
    (void)user;
    return embi_forms_paint_state(&el->base, out);
}

/* The border box of el in viewport px. */
static bool control_box(emb_view *v, embi_bind_ctx *c, const embi_element *el, float bb[4])
{
    embi_layout_engine *e = embi_bind_layout(c);
    float ox = 0, oy = 0;
    embi_rectf box;
    embi_doc_root_offset(v->doc, &ox, &oy);
    if (e == nullptr || !embi_layout_painted_box(e, el, &box)) {
        return false;
    }
    bb[0] = box.x - ox;
    bb[1] = box.y - oy;
    bb[2] = box.w;
    bb[3] = box.h;
    return true;
}

/* The pointer on a form control: caret placement and drag selection in text fields, the range thumb, select and list rows.
 * phase 0 is the press (el is the element under the pointer), 1 a move with the button down and 2 the release (el is the
 * pressed element). */
static void control_pointer(emb_view *v, embi_bind_ctx *c, embi_element *el, int phase, float x, float y, uint32_t clicks,
                            bool shift)
{
    if (c == nullptr || el == nullptr || !live(&el->base) || v->doc == nullptr) {
        return;
    }
    if (!(is_tag(&el->base, EMBI_ATOM_input) || is_tag(&el->base, EMBI_ATOM_textarea) || is_tag(&el->base, EMBI_ATOM_select))) {
        return;
    }
    float bb[4];
    if (!control_box(v, c, el, bb)) {
        return;
    }
    uint32_t byte = 0;
    if (is_tag(&el->base, EMBI_ATOM_textarea) || is_tag(&el->base, EMBI_ATOM_input)) {
        (void)embi_doc_control_offset(v->doc, el, bb, x, y, &byte);
    }
    (void)embi_forms_pointer(c, &el->base, phase, bb, x, y, byte, clicks, shift);
}

/* A press while the focused select's popup is open: true when the popup took it. */
static bool popup_press(emb_view *v, embi_bind_ctx *c, float x, float y)
{
    embi_node *f = c != nullptr ? embi_impl_input_focused(c) : nullptr;
    float bb[4];
    if (f == nullptr || !is_tag(f, EMBI_ATOM_select) || !control_box(v, c, (const embi_element *)f, bb)) {
        return false;
    }
    return embi_forms_popup_press(c, f, bb, (float)v->css_h, x, y);
}

/* Called once per frame: whether the form state or the caret blink changed since the display list was built. */
bool embi_input_forms_tick(emb_view *v)
{
    embi_input *in = v != nullptr ? v->input : nullptr;
    if (in == nullptr) {
        return false;
    }
    bool dirty = false;
    uint32_t g = embi_forms_generation();
    if (g != in->forms_gen) {
        in->forms_gen = g;
        dirty = true;
    }
    embi_bind_ctx *c = ctx_of(v);
    embi_node *f = c != nullptr ? embi_impl_input_focused(c) : nullptr;
    if (f != nullptr && (is_tag(f, EMBI_ATOM_input) || is_tag(f, EMBI_ATOM_textarea))) {
        uint32_t ph = embi_forms_caret_phase(f, SDL_GetTicks());
        if (ph != in->caret_phase) {
            in->caret_phase = ph;
            dirty = true;
        }
    }
    return dirty;
}

static void capture_prune(embi_input *in)
{
    for (uint32_t i = 0; i < EMBI_IN_CAPTURES; i++) {
        if (in->cap[i].id != 0 && !live(&in->cap[i].el->base)) {
            unpin(in, &in->cap[i].pin);
            memset(&in->cap[i], 0, sizeof in->cap[i]);
        }
    }
}

/* Installs the selector state hook (css/selector.h) in the style engine of the view's document, once per document: the
 * rules that read :hover, :active or :focus* then see the input state. */
/* True when nothing is hovered, pressed or focused: no element's style can depend on the state targets. */
static bool state_idle(void *user)
{
    const embi_pf_state *s = user;
    return s->hover == nullptr && s->active == nullptr && s->focus == nullptr && !s->focus_visible;
}

static void hook_install(emb_view *v, embi_input *in)
{
    embi_style_engine *se = v->doc != nullptr ? embi_doc_style_engine(v->doc) : nullptr;
    if (se == nullptr || in->hooked == se) {
        return;
    }
    embi_sel_hooks h = {&in->st, embi_pf_sel_state};
    embi_style_set_sel_hooks(se, &h);
    embi_style_set_state_idle(se, state_idle);
    in->hooked = se;
}

/* Restyles the view's document: the style engine resolves it again (the selector state is not a DOM change it sees) and
 * the layout and the picture follow. */
static void mark_restyle(emb_view *v)
{
    if (v->doc != nullptr) {
        embi_style_mark_dirty(embi_doc_style_engine(v->doc), embi_doc_dom(v->doc));
        embi_doc_invalidate(v->doc);
    }
    v->paint_dirty = true;
}

/* The element chain of t (t first, the root last) in out: its length, or UINT32_MAX when it is deeper than EMBI_IN_MAX. */
static uint32_t elem_chain(const embi_element *t, embi_element **out)
{
    uint32_t n = 0;
    for (const embi_node *k = t != nullptr ? &t->base : nullptr; k != nullptr; k = k->parent) {
        if (embi_is_element(k)) {
            if (n == EMBI_IN_MAX) {
                return UINT32_MAX;
            }
            out[n++] = (embi_element *)(uintptr_t)k;
        }
    }
    return n;
}

static uint64_t chain_hash(embi_element *const *c, uint32_t n)
{
    uint64_t h = 0x243F6A8885A308D3ull ^ n;
    for (uint32_t i = 0; i < n; i++) {
        h = (h ^ (uint64_t)(uintptr_t)c[i]) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    return h;
}

/* Marks the elements of chain a that are not in chain b and those of b that are not in a: their state pc flipped. Two
 * chains of one tree share their outer part (the common ancestors, whose state stays). */
static void flip_chains(embi_style_engine *se, embi_element *const *a, uint32_t na, embi_element *const *b, uint32_t nb,
                        embi_sel_pc pc)
{
    while (na > 0 && nb > 0 && a[na - 1] == b[nb - 1]) {
        na--;
        nb--;
    }
    for (uint32_t i = 0; i < na; i++) {
        (void)embi_style_mark_user_state(se, a[i], pc);
    }
    for (uint32_t i = 0; i < nb; i++) {
        (void)embi_style_mark_user_state(se, b[i], pc);
    }
}

/* CF1: marks for restyle the elements whose selector state differs between seen and st (instead of the whole document):
 * for :hover, :active and :focus-within the elements of the old or the new target's chain that are in only one of them,
 * for :focus and :focus-visible the old and the new focus. False when that cannot be known (an old target left the
 * document or its chain changed since it became the target, a chain deeper than EMBI_IN_MAX, no memory): the caller then
 * restyles the whole document. */
static bool state_flips(emb_view *v, embi_input *in)
{
    if (v->doc == nullptr || !ensure_scratch(in)) {
        return false;
    }
    embi_style_engine *se = embi_doc_style_engine(v->doc);
    const embi_element *was[3] = {in->seen.hover, in->seen.active, in->seen.focus};
    const embi_element *now[3] = {in->st.hover, in->st.active, in->st.focus};
    embi_element **oc = SCR(in, 4), **nc = SCR(in, 5);
    /* Every old chain is checked before anything is marked. */
    for (uint32_t k = 0; k < 3; k++) {
        if (was[k] != now[k] && was[k] != nullptr) {
            if (!in->seen_ok[k] || !live(&was[k]->base)) {
                return false;
            }
            uint32_t n = elem_chain(was[k], oc);
            if (n == UINT32_MAX || chain_hash(oc, n) != in->seen_hash[k]) {
                return false;
            }
        }
        if (was[k] != now[k] && elem_chain(now[k], nc) == UINT32_MAX) {
            return false;
        }
    }
    static const embi_sel_pc chain_pc[3] = {EMBI_PC_HOVER, EMBI_PC_ACTIVE, EMBI_PC_FOCUS_WITHIN};
    for (uint32_t k = 0; k < 3; k++) {
        if (was[k] == now[k]) {
            continue;
        }
        uint32_t no = elem_chain(was[k], oc);
        uint32_t nn = elem_chain(now[k], nc);
        flip_chains(se, oc, no, nc, nn, chain_pc[k]);
    }
    /* :focus and :focus-visible are the focus itself; focus-visible also flips with the focus kept. */
    if (was[2] != now[2] || in->seen.focus_visible != in->st.focus_visible) {
        for (uint32_t i = 0; i < 2; i++) {
            embi_element *f = (embi_element *)(uintptr_t)(i == 0 ? was[2] : now[2]);
            if (f != nullptr && (i == 0 || was[2] != now[2])) {
                (void)embi_style_mark_user_state(se, f, EMBI_PC_FOCUS);
                (void)embi_style_mark_user_state(se, f, EMBI_PC_FOCUS_VISIBLE);
            }
        }
    }
    return true;
}

/* Records the targets of st that changed since seen: each is pinned (state_flips reads it at the next change) with the
 * hash of its chain. Without a binding context or memory, the target is not recorded and its next change restyles the
 * whole document. */
static void state_record(embi_input *in, embi_bind_ctx *c)
{
    const embi_element *was[3] = {in->seen.hover, in->seen.active, in->seen.focus};
    const embi_element *now[3] = {in->st.hover, in->st.active, in->st.focus};
    for (uint32_t k = 0; k < 3; k++) {
        if (was[k] == now[k]) {
            continue;
        }
        unpin(in, &in->seen_pin[k]);
        in->seen_ok[k] = now[k] == nullptr;
        in->seen_hash[k] = 0;
        if (now[k] == nullptr || c == nullptr || !ensure_scratch(in)) {
            continue;
        }
        uint32_t n = elem_chain(now[k], SCR(in, 4));
        if (n == UINT32_MAX) {
            continue;
        }
        in->seen_pin[k] = pin_node(in, c, (embi_node *)(uintptr_t)&now[k]->base);
        if (in->seen_pin[k] != 0) {
            in->seen_hash[k] = chain_hash(SCR(in, 4), n);
            in->seen_ok[k] = true;
        }
    }
}

/* Brings the selector state (hover, active, focus, focus-visible) to the given targets and marks the styles dirty when a
 * rule can read a change. A hover, press or focus change restyles only when an author rule reads the state
 * (embi_style_uses_state); a change of focus-visible always does, because the user agent's :focus-visible outline reads
 * it, and a mouse focus (focus-visible false before and after) costs nothing. Only the elements whose state flipped are
 * restyled (state_flips), the whole document when they are not known. The next frame's style pass sees the change. */
static void state_mark(emb_view *v, embi_input *in, embi_bind_ctx *c, const embi_element *hover,
                       const embi_element *active, const embi_element *focus)
{
    bool used = v->doc != nullptr && embi_style_uses_state(embi_doc_style_engine(v->doc));
    if (used != in->state_used) {
        embi_input_set_state_consumed(v, used);
    }
    in->st.hover = hover;
    in->st.active = active;
    in->st.focus = focus;
    bool hov = in->seen.hover != hover || in->seen.active != active;
    bool foc = in->seen.focus != focus;
    bool fv = in->seen.focus_visible != in->st.focus_visible;
    /* The user agent's :focus-visible outline is not an author rule (state_used ignores it), so a focus move while the ring
     * applies, on either side of the move, restyles the document: the old target loses its ring, the new one gets it. */
    bool ring = in->seen.focus_visible || in->st.focus_visible;
    if ((hov && in->state_used) || (foc && (in->state_used || ring)) || fv) {
        if (state_flips(v, in)) {
            embi_doc_invalidate(v->doc);
            v->paint_dirty = true;
        } else {
            mark_restyle(v);
        }
    }
    state_record(in, c);
    in->seen = in->st;
}

/* The selector state of the view's document, read from the input state and the focused node. */
static void state_now(emb_view *v, embi_input *in, embi_bind_ctx *c)
{
    embi_node *f = c != nullptr ? embi_impl_input_focused(c) : nullptr;
    const embi_element *focus = (f != nullptr && embi_is_element(f)) ? (const embi_element *)f : nullptr;
    const embi_element *active = in->press != nullptr ? in->press : touch_active(in);
    state_mark(v, in, c, in->hover, active, focus);
}

/* The SDL system cursor kind of the element's computed cursor (-1 arrow, -2 hidden): the last item of the list, since
 * only the system cursors exist and a url() cursor falls back to the keyword after it. auto is the text cursor over a
 * text control and the arrow elsewhere. */
static int32_t cursor_kind(emb_view *v, const embi_element *el)
{
    if (el == nullptr || v->doc == nullptr) {
        return -1;
    }
    const embi_style *s = embi_style_get(embi_doc_style_engine(v->doc), (embi_element *)(uintptr_t)el, EMBI_PE_NONE);
    if (s == nullptr) {
        return -1;
    }
    embi_sb text = {0};
    int32_t kind = -1;
    if (embi_style_value_text(s, EMBI_PROP_cursor, &text) && text.len > 0) {
        uint32_t b = 0;
        uint32_t depth = 0; /* a comma inside url(...) or a string does not split the list */
        char quote = 0;
        for (uint32_t i = 0; i < text.len; i++) {
            char ch = text.data[i];
            if (quote != 0) {
                if (ch == 0x5c && i + 1 < text.len) { /* a backslash escapes the next character */
                    i++;
                } else if (ch == quote) {
                    quote = 0;
                }
            } else if (ch == '"' || ch == 0x27) {
                quote = ch;
            } else if (ch == '(') {
                depth++;
            } else if (ch == ')' && depth > 0) {
                depth--;
            } else if (ch == ',' && depth == 0) {
                b = i + 1;
            }
        }
        uint32_t e = text.len;
        while (b < e && text.data[b] == ' ') {
            b++;
        }
        while (e > b && text.data[e - 1] == ' ') {
            e--;
        }
        embi_str tok = {text.data + b, e - b};
        bool auto_kw = tok.len == 4 && memcmp(tok.ptr, "auto", 4) == 0;
        if (auto_kw && text_control((const embi_node *)el)) {
            kind = embi_pf_cursor_from_css(EMBI_STR("text"));
        } else {
            kind = embi_pf_cursor_from_css(tok);
        }
    }
    embi_sb_free(&text);
    return kind;
}

/* Brings the cursor kind of the hover element up to date and tells the window when it changed. The kind is read from the
 * computed style, so the style must be current: lay says the layout runs first (the pointer paths, where a :hover rule may
 * have just changed the style); the frame passes false, since it has laid out already. Compared by kind, not by element, so
 * a style change under a still pointer (a :hover rule, a script) also updates the cursor. The window is set only for
 * windowed views; cur_kind is kept for both, which the tests read. */
static void frame_cursor(emb_view *v, int32_t kind);

static void cursor_track(emb_view *v, embi_input *in, bool lay)
{
    if (lay) {
        embi_view_layout(v);
    }
    int32_t k = cursor_kind(v, in->hover);
    if (!in->cur_known || k != in->cur_kind) {
        in->cur_known = true;
        in->cur_kind = k;
        if (v->mode == EMB_VIEW_WINDOWED) {
            embi_window_set_cursor(v, k);
        } else {
            frame_cursor(v, k); /* a frame under the root's pointer shows its cursor in the root's window */
        }
    }
}

/* Mirrors the hover, press and focus targets into the selector hook's state and the window's cursor. */
static void sync(emb_view *v, embi_input *in, embi_bind_ctx *c)
{
    comp_check(in, c);
    capture_prune(in);
    hook_install(v, in);
    state_now(v, in, c);
    cursor_track(v, in, true); /* after the state: the hover's :hover rules are in the style the cursor reads */
    text_sync(v, in, c, c != nullptr ? embi_impl_input_focused(c) : nullptr);
}

/* The focus default action of a press on n: the nearest focusable ancestor-or-self takes the focus, else the focus
 * leaves. */
static void focus_on_press(embi_input *in, embi_bind_ctx *c, embi_node *n)
{
    embi_node *target = nullptr;
    for (embi_node *k = n; k != nullptr; k = k->parent) {
        if (embi_is_element(k) && embi_pf_focusable((const embi_element *)k)) {
            target = k;
            break;
        }
    }
    embi_impl_input_focus(c, target);
    in->st.focus_visible = false;
    in->autofocus_done = true; /* the user chose where the focus is */
}

/* ------------------------------------------------------------------ pointer capture */

static embi_capture *capture_find(embi_input *in, int32_t id)
{
    for (uint32_t i = 0; i < EMBI_IN_CAPTURES; i++) {
        if (in->cap[i].id == id) {
            return &in->cap[i];
        }
    }
    return nullptr;
}

static embi_touch *touch_by_pid(embi_input *in, int32_t pid)
{
    for (uint32_t i = 0; i < EMBI_IN_TOUCHES; i++) {
        if (in->touch[i].used && in->touch[i].pid == pid) {
            return &in->touch[i];
        }
    }
    return nullptr;
}

/* gotpointercapture or lostpointercapture at el, for pointer id. */
static void capture_event(embi_input *in, embi_bind_ctx *c, embi_element *el, int32_t id, const char *type)
{
    emb_view *v = in->view;
    bool touch = id != 1;
    embi_input_ptr p = payload(v, in, in->px, in->py, -1, touch ? 1u : in->down, 0, touch, id, true);
    (void)ptr_event(c, el, &p, true, type, true, false);
}

/* Releases the capture of pointer id (the lostpointercapture event goes to the element that held it). */
static void capture_release(embi_input *in, embi_bind_ctx *c, int32_t id, bool fire)
{
    embi_capture *k = capture_find(in, id);
    if (k == nullptr) {
        return;
    }
    embi_element *el = k->el;
    uint32_t pin = k->pin;
    k->id = 0;
    k->el = nullptr;
    k->pin = 0;
    if (fire && c != nullptr) {
        capture_event(in, c, el, id, "lostpointercapture");
    }
    if (pin != 0 && in->pin_ctx != nullptr) {
        embi_bind_unpin_node(in->pin_ctx, pin);
    }
}

static void capture_set(embi_input *in, embi_bind_ctx *c, int32_t id, embi_element *el)
{
    embi_capture *k = capture_find(in, id);
    if (k != nullptr && k->el == el) {
        return;
    }
    if (k != nullptr) {
        capture_release(in, c, id, true);
    }
    k = capture_find(in, 0);
    if (k == nullptr) {
        return; /* too many captures at once */
    }
    k->id = id;
    k->el = el;
    k->pin = pin_node(in, c, &el->base);
    capture_event(in, c, el, id, "gotpointercapture");
}

static bool hook_capture_set(void *user, int32_t id, embi_node *el)
{
    embi_input *in = user;
    embi_bind_ctx *c = in->hooks_ctx;
    if (!live(el) || !embi_is_element(el) || c == nullptr) {
        return true; /* InvalidStateError in the DOM; not reported here */
    }
    if (id == 1) {
        if (in->down == 0) {
            return true; /* a mouse with no button down: ignored, as the DOM does */
        }
    } else if (touch_by_pid(in, id) == nullptr) {
        return false; /* no such active pointer: NotFoundError */
    }
    capture_set(in, c, id, (embi_element *)el);
    return true;
}

static void hook_capture_release(void *user, int32_t id, embi_node *el)
{
    embi_input *in = user;
    embi_capture *k = capture_find(in, id);
    if (k != nullptr && (embi_node *)k->el == el) {
        capture_release(in, in->hooks_ctx, id, true);
    }
}

static bool hook_capture_has(void *user, int32_t id, embi_node *el)
{
    embi_capture *k = capture_find((embi_input *)user, id);
    return k != nullptr && (embi_node *)k->el == el;
}

static void capture_hooks(embi_input *in, embi_bind_ctx *c)
{
    embi_input_hooks h = {
        .user = in,
        .capture_set = hook_capture_set,
        .capture_release = hook_capture_release,
        .capture_has = hook_capture_has,
    };
    if (embi_impl_input_attach(c, &h)) {
        in->hooks_ctx = c;
    }
}

/* ------------------------------------------------------------------ hover */

/* Makes leaf the hover target: computes the change (the leave and enter lists in the scratch arrays) and pins the new
 * target. The events are dispatched by hover_events, and the old target's pin is released by hover_end. A target that left
 * the document is dropped without events. */
static void hover_set(embi_bind_ctx *c, embi_input *in, embi_element *leaf)
{
    in->ch.any = false;
    in->ch.nleave = 0;
    in->ch.nenter = 0;
    in->ch.old = nullptr;
    in->ch.leaf = nullptr;
    in->ch.old_pin = 0;
    if (c == nullptr) {
        return;
    }
    if (in->hover != nullptr && !live(&in->hover->base)) {
        unpin(in, &in->hover_pin);
        in->hover = nullptr;
    }
    embi_element *old = in->hover;
    if (leaf == old) {
        return;
    }
    embi_element **oc = SCR(in, 0), **nc = SCR(in, 1);
    uint32_t no = old != nullptr ? embi_pf_chain(&old->base, oc, EMBI_IN_MAX) : 0;
    uint32_t nn = leaf != nullptr ? embi_pf_chain(&leaf->base, nc, EMBI_IN_MAX) : 0;
    embi_pf_crossing r = embi_pf_crossings((const embi_element *const *)oc, no, (const embi_element *const *)nc, nn,
                                           SCR(in, 2), SCR(in, 3), EMBI_IN_MAX);
    in->ch.old = old;
    in->ch.leaf = leaf;
    in->ch.nleave = r.nleave;
    in->ch.nenter = r.nenter;
    in->ch.any = true;
    in->ch.old_pin = in->hover_pin;
    in->hover = leaf;
    in->hover_pin = leaf != nullptr ? pin_node(in, c, &leaf->base) : 0;
}

/* The target of the mouse pointer at (x, y): the element holding its capture, else the hit element. */
static embi_element *mouse_target(emb_view *v, embi_bind_ctx *c, embi_input *in, float x, float y)
{
    embi_capture *k = capture_find(in, 1);
    if (k != nullptr && live(&k->el->base)) {
        return k->el;
    }
    embi_element *el = hit(v, c, x, y);
    if (el == nullptr && in->down != 0 && in->press != nullptr && live(&in->press->base) && embi_frames_parent(v) != nullptr) {
        el = in->press; /* IF1: a drag that left a frame's viewport goes on at the element it pressed (implicit capture) */
    }
    return el;
}

/* The out, leave, over and enter events of the change (pointer ones when pointer is true, else the mouse ones). base is the
 * payload of the event that caused the change; the button is 0 for these events. */
static void hover_events(embi_bind_ctx *c, embi_input *in, const embi_input_ptr *base, bool pointer)
{
    if (!in->ch.any) {
        return;
    }
    embi_input_ptr p = *base;
    p.button = 0;
    p.detail = 0;
    p.movementX = 0;
    p.movementY = 0;
    p.related = in->ch.leaf != nullptr ? &in->ch.leaf->base : nullptr;
    if (in->ch.old != nullptr) {
        (void)ptr_event(c, in->ch.old, &p, pointer, pointer ? "pointerout" : "mouseout", true, true);
    }
    for (uint32_t i = 0; i < in->ch.nleave; i++) {
        (void)ptr_event(c, SCR(in, 2)[i], &p, pointer, pointer ? "pointerleave" : "mouseleave", false, false);
    }
    p.related = in->ch.old != nullptr ? &in->ch.old->base : nullptr;
    if (in->ch.leaf != nullptr) {
        (void)ptr_event(c, in->ch.leaf, &p, pointer, pointer ? "pointerover" : "mouseover", true, true);
    }
    for (uint32_t i = 0; i < in->ch.nenter; i++) {
        (void)ptr_event(c, SCR(in, 3)[i], &p, pointer, pointer ? "pointerenter" : "mouseenter", false, false);
    }
}

static void hover_end(embi_input *in)
{
    unpin(in, &in->ch.old_pin);
    in->ch.any = false;
}

/* ------------------------------------------------------------------ public: navigation hook, mouse */

void emb_view_set_navigate_handler(emb_view *view, emb_navigate_fn fn, void *user)
{
    embi_input *in = view != nullptr ? state_of(view) : nullptr;
    if (in != nullptr) {
        in->nav = fn;
        in->nav_user = user;
    }
}

void emb_view_set_navigate_ex_handler(emb_view *view, emb_navigate_ex_fn fn, void *user)
{
    embi_input *in = view != nullptr ? state_of(view) : nullptr;
    if (in != nullptr) {
        in->nav_ex = fn;
        in->nav_ex_user = user;
    }
}

/* ------------------------------------------------------------------ styled scrollbars */

/* The pointer over the thumb of a styled scrollbar: the thumb takes its :hover style. */
static void sbar_hover(emb_view *v, float x, float y, const embi_element *target)
{
    if (v->doc == nullptr) {
        return;
    }
    embi_doc_sbar sb;
    const void *key = nullptr;
    int axis = 0;
    if (embi_doc_sbar_at(v->doc, x, y, target, &sb) && sb.part == 1) {
        key = sb.key;
        axis = sb.vertical ? 1 : 2;
    }
    if (embi_doc_sbar_set_hover(v->doc, key, axis)) {
        v->sbar_dirty = true;
    }
}

static void sbar_scrolled(emb_view *v, const void *key)
{
    embi_view_note_scroll(v, key);
    v->scroll_dirty = true;
}

/* A press on a styled scrollbar: the thumb starts a drag, the track scrolls a page toward the point. True when the bar took it. */
static bool sbar_press(emb_view *v, embi_input *in, float x, float y, const embi_element *target)
{
    embi_doc_sbar sb;
    if (v->doc == nullptr || !embi_doc_sbar_at(v->doc, x, y, target, &sb)) {
        return false;
    }
    if (sb.part == 1) {
        in->sb_drag = true;
        in->sb_vertical = sb.vertical;
        in->sb_key = sb.key;
        in->sb_grab = (sb.vertical ? y : x) - sb.thumb_start;
        if (embi_doc_sbar_set_hover(v->doc, sb.key, sb.vertical ? 1 : 2)) {
            v->sbar_dirty = true;
        }
    } else if (embi_doc_sbar_page(v->doc, sb.key, sb.vertical, sb.part == 2 ? -1 : 1, (float)v->css_w, (float)v->css_h)) {
        sbar_scrolled(v, sb.key);
    }
    return true;
}

/* The pointer moves while the thumb is dragged: the thumb follows it. True while a drag is on. */
static bool sbar_drag(emb_view *v, embi_input *in, float x, float y)
{
    if (!in->sb_drag || v->doc == nullptr) {
        return false;
    }
    float pos = (in->sb_vertical ? y : x) - in->sb_grab;
    if (embi_doc_sbar_drag(v->doc, in->sb_key, in->sb_vertical, pos, (float)v->css_w, (float)v->css_h)) {
        sbar_scrolled(v, in->sb_key);
    }
    return true;
}

static void mouse_move_here(emb_view *view, float x, float y, uint32_t mods)
{
    if (view == nullptr || embi_view_load_suspended(view)) {
        return;
    }
    embi_input *in = state_of(view);
    if (in == nullptr || !ensure_scratch(in)) {
        return;
    }
    embi_bind_ctx *c = ctx_of(view);
    in->mods = mods;
    embi_input_ptr base = payload(view, in, x, y, -1, in->down, 0, false, 1, true);
    base.movementX = (double)x - (double)in->px;
    base.movementY = (double)y - (double)in->py;
    in->px = x;
    in->py = y;
    if (sbar_drag(view, in, x, y)) {
        return; /* the thumb of a scrollbar is held: the page sees no move */
    }
    if (c != nullptr && (in->down & button_bit(EMB_BUTTON_PRIMARY)) != 0 && in->press != nullptr) {
        control_pointer(view, c, in->press, 1, x, y, 1, false); /* a drag in a text field or on a range */
    }
    if (c != nullptr) {
        embi_node *fs = embi_impl_input_focused(c);
        float pbb[4];
        if (fs != nullptr && is_tag(fs, EMBI_ATOM_select) && control_box(view, c, (const embi_element *)fs, pbb)) {
            (void)embi_forms_popup_hover(c, fs, pbb, (float)view->css_h, x, y);
        }
    }
    hover_set(c, in, mouse_target(view, c, in, x, y));
    sbar_hover(view, x, y, in->hover);
    hover_events(c, in, &base, true);
    (void)ptr_event(c, in->hover, &base, true, "pointermove", true, true);
    hover_events(c, in, &base, false);
    (void)ptr_event(c, in->hover, &base, false, "mousemove", true, true);
    hover_end(in);
    sync(view, in, c);
}

static void mouse_leave_here(emb_view *view)
{
    if (view == nullptr || view->input == nullptr || embi_view_load_suspended(view)) {
        return;
    }
    embi_input *in = view->input;
    if (!ensure_scratch(in)) {
        return;
    }
    embi_bind_ctx *c = ctx_of(view);
    if (capture_find(in, 1) != nullptr) {
        return; /* a captured pointer stays on its element when it leaves the window */
    }
    if (view->doc != nullptr && !in->sb_drag && embi_doc_sbar_set_hover(view->doc, nullptr, 0)) {
        view->sbar_dirty = true;
    }
    embi_input_ptr base = payload(view, in, in->px, in->py, -1, in->down, 0, false, 1, true);
    hover_set(c, in, nullptr);
    hover_events(c, in, &base, true);
    hover_events(c, in, &base, false);
    hover_end(in);
    sync(view, in, c);
}

static double (*g_clock)(void); /* test override of the click-count clock */

void embi_input_set_clock(double (*fn)(void))
{
    g_clock = fn;
}

static _Thread_local uint32_t g_host_clicks; /* the host's click count for the press being dispatched (0: none); per thread, as views are */

void emb_view_mouse_button_clicks(emb_view *view, float x, float y, emb_button button, bool down, uint32_t mods,
                                  uint32_t clicks)
{
    g_host_clicks = clicks;
    emb_view_mouse_button(view, x, y, button, down, mods);
    g_host_clicks = 0;
}

static void mouse_button_here(emb_view *view, float x, float y, emb_button button, bool down, uint32_t mods)
{
    if (view == nullptr || embi_view_load_suspended(view) || (unsigned)button > (unsigned)EMB_BUTTON_FORWARD) {
        return;
    }
    embi_input *in = state_of(view);
    if (in == nullptr || !ensure_scratch(in)) {
        return;
    }
    embi_bind_ctx *c = ctx_of(view);
    in->mods = mods;
    uint32_t bit = button_bit(button);
    double now = g_clock != nullptr ? g_clock() : embi_clock_ms();
    embi_input_ptr base = payload(view, in, x, y, (int16_t)button, in->down, 0, false, 1, true);
    hover_set(c, in, mouse_target(view, c, in, x, y));
    hover_events(c, in, &base, true);
    hover_events(c, in, &base, false);
    hover_end(in);
    embi_element *leaf = in->hover;
    in->px = x;
    in->py = y;
    if (down && button == EMB_BUTTON_PRIMARY && popup_press(view, c, x, y)) {
        sync(view, in, c); /* a row of the open select's list, or the close of the list */
        return;
    }
    if (down && button == EMB_BUTTON_PRIMARY && sbar_press(view, in, x, y, leaf)) {
        return; /* a styled scrollbar: the press is the bar's */
    }
    if (!down && button == EMB_BUTTON_PRIMARY && in->sb_drag) {
        in->sb_drag = false;
        sbar_hover(view, x, y, leaf);
        return;
    }
    if (down) {
        in->down |= bit;
        base.buttons = (uint16_t)in->down;
        embi_element *prev_click = (embi_element *)(uintptr_t)in->clicks.target;
        uint32_t count = embi_pf_click_press(&in->clicks, now, x, y, (uint32_t)button, leaf);
        if (g_host_clicks != 0) {
            count = in->clicks.count = g_host_clicks; /* the host's count (the OS's) wins; the counter follows it */
        }
        if (prev_click != leaf) {
            unpin(in, &in->click_pin);
            in->click_pin = (leaf != nullptr && c != nullptr) ? pin_node(in, c, &leaf->base) : 0;
        }
        bool ok = ptr_event(c, leaf, &base, true, "pointerdown", true, true);
        embi_input_ptr mp = base;
        mp.detail = (int32_t)count;
        /* A canceled pointerdown suppresses the compatibility mouse events, the focus included. */
        bool mok = ok ? ptr_event(c, leaf, &mp, false, "mousedown", true, true) : false;
        if (mok && leaf != nullptr) {
            focus_on_press(in, c, &leaf->base); /* any button focuses, as in browsers */
            if (button == EMB_BUTTON_PRIMARY) {
                control_pointer(view, c, leaf, 0, x, y, count, (mods & EMB_MOD_SHIFT) != 0);
            }
        }
        if (button == EMB_BUTTON_SECONDARY) {
            (void)ptr_event(c, leaf, &mp, false, "contextmenu", true, true);
        }
        set_press(in, c, leaf);
    } else {
        in->down &= ~bit;
        base.buttons = (uint16_t)in->down;
        uint32_t count = in->clicks.count != 0 ? in->clicks.count : 1;
        embi_element *press = in->press;
        if (button == EMB_BUTTON_PRIMARY && press != nullptr) {
            control_pointer(view, c, press, 2, x, y, count, false);
        }
        (void)ptr_event(c, leaf, &base, true, "pointerup", true, true);
        embi_input_ptr mp = base;
        mp.detail = (int32_t)count;
        (void)ptr_event(c, leaf, &mp, false, "mouseup", true, true);
        if (in->down == 0) {
            capture_release(in, c, 1, true); /* an implicit release when the last button goes up */
        }
        if (press != nullptr && leaf != nullptr && live(&press->base)) {
            embi_element *target = common_ancestor(in, press, leaf);
            if (target != nullptr && !click_blocked(&target->base)) {
                if (button == EMB_BUTTON_PRIMARY) {
                    (void)ptr_event_ex(c, target, &mp, false, "click", true, true, true);
                    if (count == 2) {
                        (void)ptr_event(c, target, &mp, false, "dblclick", true, true);
                    }
                } else {
                    (void)ptr_event(c, target, &mp, false, "auxclick", true, true);
                }
            }
        }
        set_press(in, c, nullptr);
    }
    sync(view, in, c);
}

/* ------------------------------------------------------------------ wheel */

/* The wheel in v alone: true when a listener canceled it or something scrolled. */
static bool wheel_here(emb_view *v, float x, float y, float dx, float dy)
{
    if (embi_view_load_suspended(v)) {
        return false; /* the document is half built and shows nothing yet */
    }
    embi_input *in = state_of(v);
    embi_bind_ctx *c = in != nullptr ? ctx_of(v) : nullptr;
    bool scroll = true;
    if (c != nullptr) {
        embi_element *leaf = hit(v, c, x, y);
        if (leaf != nullptr && embi_impl_input_listening(c, &leaf->base, "wheel")) {
            embi_input_wheel w;
            memset(&w, 0, sizeof w);
            w.ptr = payload(v, in, x, y, 0, in->down, 0, false, 1, true);
            offsets(c, leaf, &w.ptr);
            w.deltaX = (double)dx; /* pixels, positive down and right, as the scroll offsets move */
            w.deltaY = (double)dy;
            w.deltaMode = 0;
            scroll = embi_impl_input_wheel(c, &leaf->base, &w);
        }
        if (v->doc == nullptr) {
            return true; /* a listener loaded another document */
        }
    }
    bool moved = !scroll;
    if (scroll) {
        moved = embi_view_wheel(v, x, y, dx, dy);
    }
    if (in != nullptr) {
        sync(v, in, c);
    }
    return moved;
}

/* ------------------------------------------------------------------ touch */

static embi_touch *touch_find(embi_input *in, uint64_t id)
{
    for (uint32_t i = 0; i < EMBI_IN_TOUCHES; i++) {
        if (in->touch[i].used && in->touch[i].id == id) {
            return &in->touch[i];
        }
    }
    return nullptr;
}

/* The pointerenter events of el's chain (outermost first) or the pointerleave events (innermost first). */
static void touch_boundary(embi_bind_ctx *c, embi_input *in, embi_element *el, const embi_input_ptr *base, bool enter)
{
    embi_element **ch = SCR(in, 0);
    uint32_t n = embi_pf_chain(&el->base, ch, EMBI_IN_MAX);
    embi_input_ptr p = *base;
    p.related = nullptr;
    if (enter) {
        for (uint32_t i = n; i > 0; i--) {
            (void)ptr_event(c, ch[i - 1], &p, true, "pointerenter", false, false);
        }
    } else {
        for (uint32_t i = 0; i < n; i++) {
            (void)ptr_event(c, ch[i], &p, true, "pointerleave", false, false);
        }
    }
}

static void touch_here(emb_view *view, uint64_t id, float x, float y, int phase)
{
    if (view == nullptr || embi_view_load_suspended(view) || phase < 0 || phase > 3) {
        return;
    }
    embi_input *in = state_of(view);
    if (in == nullptr || !ensure_scratch(in)) {
        return;
    }
    embi_bind_ctx *c = ctx_of(view);
    in->px = x;
    in->py = y;
    embi_touch *t = touch_find(in, id);
    if (phase == 0) {
        if (t != nullptr) {
            return; /* a repeated down for a finger that is already down */
        }
        bool other = false;
        for (uint32_t i = 0; i < EMBI_IN_TOUCHES; i++) {
            if (in->touch[i].used) {
                other = true;
            } else if (t == nullptr) {
                t = &in->touch[i];
            }
        }
        if (t == nullptr) {
            return; /* more fingers than slots */
        }
        memset(t, 0, sizeof *t);
        t->used = true;
        t->id = id;
        t->pid = 2 + (int32_t)(id & 0x3fffffffu);
        t->primary = !other;
        t->sx = x;
        t->sy = y;
        embi_element *leaf = hit(view, c, x, y);
        if (c != nullptr && leaf != nullptr) {
            embi_input_ptr p = payload(view, in, x, y, 0, 1u, 0, true, t->pid, t->primary);
            t->press = leaf;
            t->press_pin = pin_node(in, c, &leaf->base);
            (void)ptr_event(c, leaf, &p, true, "pointerover", true, true);
            touch_boundary(c, in, leaf, &p, true);
            if (ptr_event(c, leaf, &p, true, "pointerdown", true, true) && live(&leaf->base)) {
                focus_on_press(in, c, &leaf->base); /* a tap focuses the control it lands on (no compatibility mouse events) */
            }
        }
        sync(view, in, c);
        return;
    }
    if (t == nullptr) {
        return;
    }
    embi_element *press = t->press;
    embi_capture *k = capture_find(in, t->pid);
    embi_element *target = (k != nullptr && live(&k->el->base)) ? k->el : press; /* implicit capture: the press target */
    if (phase == 1) {
        float dx = x - t->sx, dy = y - t->sy;
        if (dx * dx + dy * dy > EMBI_IN_TAP_SLOP * EMBI_IN_TAP_SLOP) {
            t->moved = true;
        }
        embi_input_ptr p = payload(view, in, x, y, -1, 1u, 0, true, t->pid, t->primary);
        (void)ptr_event(c, target, &p, true, "pointermove", true, true);
        sync(view, in, c);
        return;
    }
    /* up (2) or cancel (3): the event goes to the pinned press target even when the finger is over nothing else. */
    bool up = phase == 2;
    embi_input_ptr p = payload(view, in, x, y, 0, 0u, 0, true, t->pid, t->primary);
    if (!up) {
        p.button = -1;
    }
    (void)ptr_event(c, target, &p, true, up ? "pointerup" : "pointercancel", true, up);
    capture_release(in, c, t->pid, true);
    if (target != nullptr && live(&target->base)) {
        p.button = -1;
        (void)ptr_event(c, target, &p, true, "pointerout", true, true);
        touch_boundary(c, in, target, &p, false);
    }
    bool primary = t->primary;
    bool moved = t->moved;
    uint32_t pin = t->press_pin;
    t->press_pin = 0;
    memset(t, 0, sizeof *t); /* the slot is free before the click, so a listener's input call sees a consistent state */
    if (up && primary && !moved && press != nullptr && live(&press->base)) {
        embi_element *leaf = hit(view, c, x, y);
        embi_element *cl = leaf != nullptr ? common_ancestor(in, press, leaf) : nullptr;
        if (cl != nullptr && !click_blocked(&cl->base)) {
            embi_input_ptr cp = payload(view, in, x, y, 0, 0u, 1, true, 0, true);
            cp.pointerType = nullptr;
            (void)ptr_event_ex(c, cl, &cp, false, "click", true, true, true);
        }
    }
    if (pin != 0 && in->pin_ctx != nullptr) {
        embi_bind_unpin_node(in->pin_ctx, pin);
    }
    sync(view, in, c);
}

/* ------------------------------------------------------------------ keyboard */

/* The element that receives keys: the focused one, else the body (the document element when there is none). */
static embi_node *key_target(embi_bind_ctx *c)
{
    embi_node *f = embi_impl_input_focused(c);
    if (f != nullptr) {
        return f;
    }
    embi_document *doc = embi_bind_document(c);
    embi_element *html = embi_document_element(doc);
    if (html == nullptr) {
        return &doc->base;
    }
    for (embi_node *k = html->base.first_child; k != nullptr; k = k->next) {
        if (embi_is_element(k) && k->name == EMBI_ATOM_body) {
            return k;
        }
    }
    return &html->base;
}

/* A control that takes the arrow keys and the Space key for its own use (a text field, a textarea, a select). */
static bool editable(const embi_node *n)
{
    if (is_tag(n, EMBI_ATOM_textarea) || is_tag(n, EMBI_ATOM_select)) {
        return true;
    }
    return is_tag(n, EMBI_ATOM_input) && text_control(n);
}

/* Controls that Space activates on keyup. */
static bool space_activates(const embi_node *n)
{
    return is_tag(n, EMBI_ATOM_button) || (is_tag(n, EMBI_ATOM_input) && (input_type_is(n, "checkbox") ||
                                                                           input_type_is(n, "radio") ||
                                                                           input_type_is(n, "button") ||
                                                                           input_type_is(n, "submit") ||
                                                                           input_type_is(n, "reset")));
}

static bool is_link(const embi_node *n)
{
    if (n != nullptr && embi_is_element(n) && n->extra == EMBI_NS_SVG && n->name == EMBI_ATOM_a) {
        return has_attr(n, "href") || has_attr(n, "xlink:href"); /* R4c: an SVG link */
    }
    return is_tag(n, EMBI_ATOM_a) && has_attr(n, "href");
}

/* What the Tab filter reads: the layout (an element with no box, display:none or inside such a subtree, is skipped) and the
 * style (visibility: hidden and collapse are skipped; their visible children are not). */
typedef struct tab_filter {
    embi_layout_engine *layout;
    embi_style_engine *style;
} tab_filter;

/* R4c: an element inside an svg (an SVG link) has no box of its own. It is rendered when the nearest ancestor with a box (its
 * svg root, or a foreignObject's content) is, and no SVG element on the way up has display: none (the UA sheet gives none to
 * defs, symbol and the other containers that are never rendered directly). */
static bool svg_rendered(const tab_filter *t, const embi_element *el)
{
    embi_rectf r;
    for (const embi_node *n = &el->base; n != nullptr && embi_is_element(n); n = n->parent) {
        const embi_element *e = (const embi_element *)n;
        if (embi_layout_border_box(t->layout, e, &r)) {
            return true;
        }
        if (n->extra != EMBI_NS_SVG) {
            return false;
        }
        const embi_style *s = t->style != nullptr ? embi_style_get(t->style, (embi_element *)(uintptr_t)e, EMBI_PE_NONE) : nullptr;
        const embi_value *d = s != nullptr ? embi_style_value(s, EMBI_PROP_display) : nullptr;
        if (s == nullptr || (d != nullptr && d->kind == EMBI_V_KEYWORD && d->kw == EMBI_KW_none)) {
            return false;
        }
    }
    return false;
}

static bool rendered(void *user, const embi_element *el)
{
    const tab_filter *t = user;
    embi_rectf r;
    if (t->layout != nullptr && !embi_layout_border_box(t->layout, el, &r) &&
        (el->base.extra != EMBI_NS_SVG || !svg_rendered(t, el))) {
        return false;
    }
    if (t->style != nullptr) {
        const embi_style *s = embi_style_get(t->style, (embi_element *)(uintptr_t)el, EMBI_PE_NONE);
        const embi_value *vis = s != nullptr ? embi_style_value(s, EMBI_PROP_visibility) : nullptr;
        if (vis != nullptr && vis->kind == EMBI_V_KEYWORD &&
            (vis->kw == EMBI_KW_hidden || vis->kw == EMBI_KW_collapse)) {
            return false;
        }
    }
    return true;
}

/* Moves the focus one step along the tab order. */
static void tab_step(emb_view *v, embi_bind_ctx *c, embi_input *in, bool reverse)
{
    embi_view_layout(v);
    tab_filter tf = {embi_bind_layout(c), embi_doc_style_engine(v->doc)};
    embi_element *next = embi_pf_focus_step_filtered(&embi_bind_document(c)->base, embi_impl_input_focused(c), reverse,
                                                     rendered, &tf);
    if (next != nullptr) {
        embi_impl_input_focus(c, &next->base);
        in->st.focus_visible = true;
        scroll_into_view(v, c, next);
    }
}

/* The selected text of target for copy and cut: the registered provider's (W5e), nothing otherwise. */
static bool selected_text(embi_input *in, embi_node *target, embi_sb *out)
{
    if (in->sel_fn == nullptr) {
        return embi_forms_selected_text(target, out) && out->len > 0; /* the text controls (W5e) */
    }
    return in->sel_fn(in->sel_user, target, out) && out->len > 0;
}

/* One clipboard command on the key target. See emb_view_clipboard. */
static void clipboard_run(emb_view *v, embi_input *in, embi_bind_ctx *c, emb_clipboard_command cmd)
{
    if (c == nullptr) {
        return;
    }
    embi_node *target = key_target(c);
    if (cmd == EMB_CLIPBOARD_PASTE) {
        embi_sb text = {0};
        (void)embi_pf_clipboard_get(&text);
        embi_sb_append(&text, EMBI_STR("\0"));
        if (text.len > 0) {
            text.len--; /* a terminator after the text, for the C string below */
        }
        bool ok = embi_impl_input_clipboard_event(c, target, "paste", text.len > 0 ? text.data : nullptr, nullptr, nullptr);
        if (ok && live(target) && text_control(target) && text.len > 0) {
            (void)embi_forms_edit(c, target, "insertFromPaste", (embi_str){text.data, text.len}, false);
        }
        embi_sb_free(&text);
        return;
    }
    bool cut = cmd == EMB_CLIPBOARD_CUT;
    embi_sb out = {0};
    bool has = false;
    bool ok = embi_impl_input_clipboard_event(c, target, cut ? "cut" : "copy", nullptr, &out, &has);
    if (!ok) {
        /* A listener handled it: its clipboardData is what goes to the clipboard. */
        if (has) {
            (void)embi_pf_clipboard_set((embi_str){out.data, out.len});
        }
    } else if (live(target)) {
        embi_sb sel = {0};
        if (selected_text(in, target, &sel)) {
            (void)embi_pf_clipboard_set((embi_str){sel.data, sel.len});
            if (cut && text_control(target)) {
                (void)embi_forms_edit(c, target, "deleteByCut", (embi_str){nullptr, 0}, false);
            }
        }
        embi_sb_free(&sel);
    }
    embi_sb_free(&out);
    (void)v;
}

static void clipboard_here(emb_view *view, emb_clipboard_command command)
{
    if (view == nullptr || embi_view_load_suspended(view) || (unsigned)command > (unsigned)EMB_CLIPBOARD_PASTE) {
        return;
    }
    embi_input *in = state_of(view);
    if (in == nullptr) {
        return;
    }
    embi_bind_ctx *c = ctx_of(view);
    clipboard_run(view, in, c, command);
    sync(view, in, c);
}

/* The clipboard command of a key press with these modifiers, or -1. */
static int clipboard_key(const emb_key_event *ev)
{
    const char *k = ev->key != nullptr ? ev->key : "";
    bool ctrl = (ev->mods & (EMB_MOD_CTRL | EMB_MOD_META)) != 0;
    bool shift = (ev->mods & EMB_MOD_SHIFT) != 0;
    bool alt = (ev->mods & EMB_MOD_ALT) != 0;
    if (ctrl && !alt && !shift && k[0] != '\0' && k[1] == '\0') {
        switch (k[0]) {
        case 'c':
        case 'C':
            return EMB_CLIPBOARD_COPY;
        case 'x':
        case 'X':
            return EMB_CLIPBOARD_CUT;
        case 'v':
        case 'V':
            return EMB_CLIPBOARD_PASTE;
        default:
            return -1;
        }
    }
    if (ctrl && !alt && !shift && SDL_strcmp(k, "Insert") == 0) {
        return EMB_CLIPBOARD_COPY;
    }
    if (!ctrl && !alt && shift && SDL_strcmp(k, "Insert") == 0) {
        return EMB_CLIPBOARD_PASTE;
    }
    if (!ctrl && !alt && shift && SDL_strcmp(k, "Delete") == 0) {
        return EMB_CLIPBOARD_CUT;
    }
    return -1;
}

/* The default action of a keydown that was not canceled. */
static void key_default(emb_view *v, embi_bind_ctx *c, embi_input *in, embi_node *target, const emb_key_event *ev)
{
    const char *k = ev->key != nullptr ? ev->key : "";
    bool shift = (ev->mods & EMB_MOD_SHIFT) != 0;
    int clip = clipboard_key(ev);
    if (clip >= 0) {
        clipboard_run(v, in, c, (emb_clipboard_command)clip);
        return;
    }
    if (SDL_strcmp(k, "Tab") == 0) {
        /* Ctrl, Alt and Meta belong to the browser or the host: Tab with them does not move the focus. */
        if (c != nullptr && (ev->mods & (EMB_MOD_CTRL | EMB_MOD_ALT | EMB_MOD_META)) == 0) {
            tab_step(v, c, in, shift);
        }
        return;
    }
    if (SDL_strcmp(k, "Enter") == 0) {
        if (c != nullptr && (is_link(target) || is_tag(target, EMBI_ATOM_button) ||
                          input_type_is(target, "submit") || input_type_is(target, "button") ||
                          input_type_is(target, "reset") || input_type_is(target, "image"))) {
            embi_impl_HTMLElement_click(c, target);
        } else if (c != nullptr && is_tag(target, EMBI_ATOM_textarea)) {
            char nl = 10; /* a line break in a textarea (W5e) */
            (void)embi_forms_edit(c, target, "insertLineBreak", (embi_str){&nl, 1}, false);
        } else if (c != nullptr && is_tag(target, EMBI_ATOM_input)) {
            (void)embi_forms_implicit_submit(c, target);
        }
        return;
    }
    if (c != nullptr && target != nullptr &&
        embi_forms_key(c, target, k, (ev->mods & EMB_MOD_CTRL) != 0, shift, (ev->mods & EMB_MOD_ALT) != 0)) {
        return; /* Backspace, Delete, the caret keys and Ctrl+A of a text control (W5e) */
    }
    float page = (float)v->css_h * 0.875f;
    float step = EMBI_VIEW_KEY_PX;
    if (SDL_strcmp(k, " ") == 0) {
        if (space_activates(target) || editable(target)) {
            return;
        }
        embi_view_scroll_root(v, 0.0f, shift ? -page : page);
    } else if (SDL_strcmp(k, "ArrowDown") == 0 || SDL_strcmp(k, "ArrowUp") == 0 ||
               SDL_strcmp(k, "ArrowLeft") == 0 || SDL_strcmp(k, "ArrowRight") == 0 || SDL_strcmp(k, "PageDown") == 0 ||
               SDL_strcmp(k, "PageUp") == 0 || SDL_strcmp(k, "Home") == 0 || SDL_strcmp(k, "End") == 0) {
        if (editable(target)) {
            return; /* the caret keeps these keys (W5e) */
        }
        if (SDL_strcmp(k, "ArrowDown") == 0) {
            embi_view_scroll_root(v, 0.0f, step);
        } else if (SDL_strcmp(k, "ArrowUp") == 0) {
            embi_view_scroll_root(v, 0.0f, -step);
        } else if (SDL_strcmp(k, "ArrowLeft") == 0) {
            embi_view_scroll_root(v, -step, 0.0f);
        } else if (SDL_strcmp(k, "ArrowRight") == 0) {
            embi_view_scroll_root(v, step, 0.0f);
        } else if (SDL_strcmp(k, "PageDown") == 0) {
            embi_view_scroll_root(v, 0.0f, page);
        } else if (SDL_strcmp(k, "PageUp") == 0) {
            embi_view_scroll_root(v, 0.0f, -page);
        } else if (SDL_strcmp(k, "Home") == 0) {
            embi_view_scroll_root(v, 0.0f, -1.0e9f);
        } else {
            embi_view_scroll_root(v, 0.0f, 1.0e9f);
        }
    }
}

/* Whether a keydown produces a character (and so is followed by the legacy keypress): a key value of one code point, or
 * Enter, with no Ctrl or Meta. */
static bool produces_char(const emb_key_event *ev)
{
    const char *k = ev->key != nullptr ? ev->key : "";
    if ((ev->mods & (EMB_MOD_CTRL | EMB_MOD_META)) != 0 || k[0] == '\0') {
        return false;
    }
    if (SDL_strcmp(k, "Enter") == 0) {
        return true;
    }
    uint32_t n = 0;
    for (const char *p = k; *p != '\0'; p++) {
        if (((unsigned char)*p & 0xC0u) != 0x80u) {
            n++;
        }
    }
    return n == 1;
}

static void key_here(emb_view *view, const emb_key_event *ev)
{
    if (view == nullptr || embi_view_load_suspended(view) || ev == nullptr || ev->size < sizeof(emb_key_event)) {
        return;
    }
    embi_input *in = state_of(view);
    if (in == nullptr) {
        return;
    }
    embi_bind_ctx *c = ctx_of(view);
    in->mods = ev->mods;
    embi_node *target = c != nullptr ? key_target(c) : nullptr;
    embi_input_key k = {
        .key = ev->key != nullptr ? ev->key : "",
        .code = ev->code != nullptr ? ev->code : "",
        .location = (uint32_t)ev->location,
        .repeat = ev->repeat,
        .isComposing = in->composing,
        .mods = ev->mods,
    };
    if (ev->down) {
        in->autofocus_done = true; /* the user is using the keyboard: a late autofocus must not steal the focus */
        bool canceled = c != nullptr && target != nullptr && live(target) && !embi_impl_input_key(c, target, "keydown", &k);
        if (!canceled && c != nullptr && target != nullptr && live(target) && produces_char(ev) && !in->composing) {
            /* The legacy keypress follows an uncanceled keydown; canceling it cancels the default action too. */
            canceled = !embi_impl_input_key(c, target, "keypress", &k);
            target = live(target) ? target : key_target(c);
        }
        if (!canceled) {
            key_default(view, c, in, target, ev);
        }
        if (c != nullptr && SDL_strcmp(k.key, " ") == 0 && !ev->repeat) {
            /* The activation on the keyup needs the Space to have gone down on the same control. */
            unpin(in, &in->space_pin);
            in->space_target = nullptr;
            if (target != nullptr && live(target) && space_activates(target)) {
                in->space_target = target;
                in->space_pin = pin_node(in, c, target);
            }
        }
    } else {
        bool canceled = c != nullptr && target != nullptr && live(target) && !embi_impl_input_key(c, target, "keyup", &k);
        bool same = in->space_target != nullptr && in->space_target == target;
        if (SDL_strcmp(k.key, " ") == 0) {
            unpin(in, &in->space_pin);
            in->space_target = nullptr;
        }
        if (!canceled && c != nullptr && same && live(target) && SDL_strcmp(k.key, " ") == 0 && space_activates(target)) {
            embi_impl_HTMLElement_click(c, target);
        }
    }
    sync(view, in, c);
}

/* ------------------------------------------------------------------ text and composition */

static void set_comp(embi_input *in, const char *text, size_t len)
{
    if (in->comp != nullptr) {
        SDL_free(in->comp);
        in->comp = nullptr;
    }
    in->comp_len = 0;
    if (len > 0) {
        in->comp = SDL_malloc(len + 1);
        if (in->comp != nullptr) {
            SDL_memcpy(in->comp, text, len);
            in->comp[len] = '\0';
            in->comp_len = (uint32_t)len;
        }
    }
}

/* Ends the composition: compositionend (with data) goes to the node the composition started on, if it is still in the
 * document and it received the composition events, even when the focus has moved since. */
static void comp_end(embi_input *in, embi_bind_ctx *c, const char *data)
{
    embi_node *t = in->comp_target;
    uint32_t pin = in->comp_pin;
    bool fire = in->comp_edit && c != nullptr && live(t);
    in->comp_target = nullptr;
    in->comp_pin = 0;
    in->comp_edit = false;
    in->composing = false;
    set_comp(in, nullptr, 0);
    if (fire) {
        embi_impl_input_composition(c, t, "compositionend", data);
        embi_forms_composition_end(c, t);
    }
    if (pin != 0 && in->pin_ctx != nullptr) {
        embi_bind_unpin_node(in->pin_ctx, pin);
    }
}

/* The composition belongs to the node it started on: when the key target is another node (the focus moved, from a script
 * or the page), it ends there. */
static void comp_check(embi_input *in, embi_bind_ctx *c)
{
    if (in->composing && in->comp_target != nullptr && c != nullptr && key_target(c) != in->comp_target) {
        comp_end(in, c, "");
    }
}

static void text_input_here(emb_view *view, const char *text, size_t len)
{
    if (view == nullptr || embi_view_load_suspended(view) || text == nullptr) {
        return;
    }
    embi_input *in = state_of(view);
    if (in == nullptr) {
        return;
    }
    embi_bind_ctx *c = ctx_of(view);
    comp_check(in, c);
    if (c != nullptr) {
        embi_node *target = key_target(c);
        bool edit = text_control(target);
        bool was_composing = in->composing;
        if (was_composing) {
            /* The commit is the last insertCompositionText (isComposing true), then the composition ends. */
            char *end = SDL_malloc(len + 1);
            if (end != nullptr) {
                SDL_memcpy(end, text, len);
                end[len] = '\0';
                if (edit) {
                    (void)embi_forms_edit(c, target, "insertCompositionText", (embi_str){end, (uint32_t)len}, true);
                }
                comp_end(in, c, end);
                SDL_free(end);
            } else {
                comp_end(in, c, "");
            }
        }
        /* A text input without a composition is the editing of the focused control (W5e applies it and fires input).
         * Nothing else takes text, so beforeinput is not sent to a target that is not editable. */
        if (edit && len > 0 && !was_composing) {
            char *s = SDL_malloc(len + 1);
            if (s != nullptr) {
                SDL_memcpy(s, text, len);
                s[len] = '\0';
                (void)embi_forms_edit(c, target, "insertText", (embi_str){s, (uint32_t)len}, false);
                SDL_free(s);
            }
        }
    }
    sync(view, in, c);
}

static void text_editing_here(emb_view *view, const emb_text_editing *ev)
{
    if (view == nullptr || embi_view_load_suspended(view) || ev == nullptr || ev->size < sizeof(emb_text_editing)) {
        return;
    }
    embi_input *in = state_of(view);
    if (in == nullptr) {
        return;
    }
    embi_bind_ctx *c = ctx_of(view);
    const char *text = ev->text != nullptr ? ev->text : "";
    size_t len = strlen(text);
    comp_check(in, c);
    if (c != nullptr) {
        embi_node *target = key_target(c);
        bool edit = text_control(target);
        if (len > 0 && !in->composing) {
            in->composing = true;
            set_comp(in, text, len);
            in->comp_target = target;
            in->comp_pin = pin_node(in, c, target);
            in->comp_edit = edit;
            if (edit) {
                embi_impl_input_composition(c, target, "compositionstart", "");
                embi_forms_composition_start(c, target);
                (void)embi_forms_edit(c, target, "insertCompositionText", (embi_str){text, (uint32_t)len}, true);
                embi_impl_input_composition(c, target, "compositionupdate", text);
            }
        } else if (len > 0 && in->composing) {
            if (in->comp_len != len || SDL_memcmp(in->comp, text, len) != 0) {
                set_comp(in, text, len);
                if (edit) {
                    (void)embi_forms_edit(c, target, "insertCompositionText", (embi_str){text, (uint32_t)len}, true);
                    embi_impl_input_composition(c, target, "compositionupdate", text);
                }
            }
        } else if (len == 0 && in->composing) {
            comp_end(in, c, "");
        }
    } else {
        in->composing = len > 0;
        set_comp(in, text, len);
    }
    in->comp_cursor = ev->cursor;
    in->comp_sel = ev->sel_len;
    sync(view, in, c);
}

/* ------------------------------------------------------------------ host focus */

static void focus_here(emb_view *view, bool focused)
{
    if (view == nullptr || embi_view_load_suspended(view)) {
        return;
    }
    embi_input *in = state_of(view);
    if (in == nullptr) {
        return;
    }
    embi_bind_ctx *c = ctx_of(view);
    if (!focused && in->composing) {
        /* The input method cannot finish a composition in a window that lost the keyboard: it ends here. */
        comp_end(in, c, "");
    }
    if (c != nullptr) {
        embi_impl_input_host_focus(c, focused);
    }
    if (!focused) {
        /* A host that loses focus ends the press: no click follows a release outside the view. */
        set_press(in, c, nullptr);
        in->down = 0;
        capture_release(in, c, 1, true);
    }
    sync(view, in, c);
}

/* ------------------------------------------------------------------ autofocus */

/* The first element in tree order below root (root included) with the autofocus attribute that may take the focus. At most
 * budget nodes are looked at (an insertion of a huge subtree stays cheap). */
static embi_element *find_autofocus(embi_node *root, uint32_t budget)
{
    embi_node *n = root;
    while (n != nullptr && budget-- > 0) {
        if (embi_is_element(n) && has_attr(n, "autofocus") && embi_pf_focusable((const embi_element *)n)) {
            return (embi_element *)n;
        }
        if (n->first_child != nullptr) {
            n = n->first_child;
            continue;
        }
        while (n != nullptr && n != root && n->next == nullptr) {
            n = n->parent;
        }
        n = (n == nullptr || n == root) ? nullptr : n->next;
    }
    return nullptr;
}

/* Gives the focus to the first autofocus element of the document, once, when nothing has the focus. */
static void autofocus_run(emb_view *v)
{
    embi_input *in = v->input;
    if (in == nullptr || in->autofocus_done || v->doc == nullptr) {
        return;
    }
    in->autofocus_queued = false;
    embi_bind_ctx *c = ctx_of(v);
    if (c == nullptr) {
        return;
    }
    if (embi_impl_input_focused(c) != nullptr) {
        in->autofocus_done = true;
        return;
    }
    embi_element *el = find_autofocus(&embi_bind_document(c)->base, UINT32_MAX);
    if (el != nullptr) {
        in->autofocus_done = true;
        embi_impl_input_focus(c, &el->base);
        in->st.focus_visible = true; /* programmatic focus shows the ring, like focus() on a text control */
        scroll_into_view(v, c, el);
        sync(v, in, c);
    }
}

static void autofocus_task(void *user)
{
    autofocus_run(user);
}

static void queue_autofocus(emb_view *v, embi_input *in)
{
    if (in->autofocus_done || in->autofocus_queued || v->doc == nullptr) {
        return;
    }
    embi_script *s = embi_doc_script(v->doc);
    if (s != nullptr && embi_script_post(s, autofocus_task, v)) {
        in->autofocus_queued = true;
    }
}

/* DOM observer: an inserted subtree that holds an autofocus element queues the autofocus processing. */
static void obs_node_inserted(void *ctx, embi_node *node)
{
    embi_input *in = ctx;
    if (in->autofocus_done || in->autofocus_queued) {
        return;
    }
    if (find_autofocus(node, EMBI_IN_AUTOFOCUS_BUDGET) != nullptr) {
        queue_autofocus(in->view, in);
    }
}

void embi_input_doc_ready(emb_view *v)
{
    if (v == nullptr || v->doc == nullptr) {
        return;
    }
    embi_input *in = state_of(v);
    embi_document *dom = embi_doc_dom(v->doc);
    if (in == nullptr || dom == nullptr) {
        return;
    }
    hook_install(v, in); /* the new document's rules read the input state from now on */
    embi_document_unobserve(dom, in); /* exactly one registration per document, whatever happened before */
    embi_observer obs;
    memset(&obs, 0, sizeof obs);
    obs.ctx = in;
    obs.node_inserted = obs_node_inserted;
    embi_document_observe(dom, &obs);
    in->obs_doc = dom;
    if (find_autofocus(&dom->base, UINT32_MAX) != nullptr) {
        queue_autofocus(v, in);
    }
}

/* ------------------------------------------------------------------ cursor */

void embi_input_apply_cursor(emb_view *v, embi_str css_cursor)
{
    if (v == nullptr) {
        return;
    }
    embi_input *in = state_of(v);
    int32_t k = embi_pf_cursor_from_css(css_cursor);
    if (in != nullptr) {
        in->cur_known = true; /* the window now shows k: the tracked kind follows it */
        in->cur_kind = k;
    }
    if (v->mode == EMB_VIEW_WINDOWED) {
        embi_window_set_cursor(v, k);
    } else {
        frame_cursor(v, k);
    }
}

void embi_input_cursor_refresh(emb_view *v)
{
    embi_input *in = v != nullptr ? v->input : nullptr;
    if (in != nullptr && v->doc != nullptr) {
        cursor_track(v, in, false);
    }
}

int32_t embi_input_cursor_kind(emb_view *v)
{
    embi_input *in = v != nullptr ? v->input : nullptr;
    return in != nullptr && in->cur_known ? in->cur_kind : -1;
}

/* ------------------------------------------------------------------ state refresh, cursor query */

void embi_input_refresh_state(emb_view *v)
{
    embi_input *in = v != nullptr ? v->input : nullptr;
    if (in == nullptr || v->doc == nullptr) {
        return;
    }
    embi_bind_ctx *c = ctx_of(v); /* an existing script bind: no input has to have happened yet */
    if (c == nullptr) {
        return;
    }
    hook_install(v, in);
    state_now(v, in, c); /* the focus may have moved by script since the last input */
}

int32_t embi_input_cursor_at(emb_view *v, float x, float y)
{
    if (v == nullptr) {
        return -1;
    }
    embi_bind_ctx *c = ctx_of(v);
    return cursor_kind(v, hit(v, c, x, y));
}

/* ------------------------------------------------------------------ routing to the frames */

/* The public calls route the event to the document it belongs to. A pointer event goes to the
 * frame whose iframe is the topmost hit at the point, in the frame's viewport coordinates, and on to that frame's own frames.
 * A button pressed in a frame keeps the pointer there until the last button is up (implicit capture, even outside the box);
 * a drag that started in the parent stays in the parent. The parent's elements get their out and leave events when the
 * pointer goes into a frame, and the frame's when it comes out. Keys, text, composition and the clipboard go to the focused
 * frame: a press in a frame focuses its iframe in the parent and the frame's document; a press in the parent, or the
 * parent's focus moving off the iframe, takes the keyboard back. */

/* Whether v (or a frame below it that holds the pointer) has a button down or a pointer capture: the pointer stays there. */
static bool pointer_held(emb_view *v)
{
    if (v->input != nullptr && (v->input->down != 0 || capture_find(v->input, 1) != nullptr)) {
        return true;
    }
    embi_frame_route *r = v->frames != nullptr ? embi_frames_route(v) : nullptr;
    return r != nullptr && r->press != nullptr && pointer_held(r->press);
}

/* The pointer moved from frame r->hover (nullptr: v itself) to kid (nullptr: v itself): the one it left gets its leave
 * events, and the window's cursor is set again by the next track. */
static void hover_frame(emb_view *v, embi_frame_route *r, emb_view *kid)
{
    if (r->hover == kid) {
        return;
    }
    emb_view *old = r->hover;
    r->hover = kid;
    if (old != nullptr) {
        emb_view_mouse_leave(old);
    } else {
        mouse_leave_here(v); /* the parent's elements: out and leave */
    }
    if (v->input != nullptr) {
        v->input->cur_known = false;
    }
    if (kid != nullptr && kid->input != nullptr) {
        kid->input->cur_known = false;
    }
}

/* The frame that takes a pointer event at (x, y) of v (the point mapped into it), or nullptr for v itself. */
static emb_view *pointer_frame(emb_view *v, embi_frame_route *r, float x, float y, float *lx, float *ly)
{
    if (r == nullptr) {
        return nullptr;
    }
    if (r->press != nullptr) {
        if (embi_frames_live(r->press) && pointer_held(r->press)) {
            if (embi_frames_at(v, x, y, lx, ly) == r->press) {
                return r->press; /* over the frame: the exact mapping */
            }
            if (!embi_frames_map(r->press, x, y, lx, ly)) {
                *lx = x;
                *ly = y;
            }
            return r->press;
        }
        r->press = nullptr;
    }
    if (v->input != nullptr && (v->input->down != 0 || capture_find(v->input, 1) != nullptr)) {
        return nullptr; /* a drag or a capture of the parent's own */
    }
    return embi_frames_any(v) ? embi_frames_at(v, x, y, lx, ly) : nullptr;
}

/* Frame kid (and the frames focused below it) loses the keyboard. */
static void frame_blur(emb_view *kid)
{
    embi_frame_route *r = kid->frames != nullptr ? embi_frames_route(kid) : nullptr;
    if (r != nullptr && r->focus != nullptr) {
        emb_view *k = r->focus;
        r->focus = nullptr;
        frame_blur(k);
    }
    focus_here(kid, false);
    if (kid->input != nullptr && kid->input->text_on) {
        text_area(kid, false, 0, 0, 0, 0); /* the frame's text field keeps its focus, but not the window's text input */
        kid->input->text_on = false;
    }
}

/* A press in frame kid of v: the iframe takes the focus in v, and kid has the keyboard. */
static void frame_focus(emb_view *v, embi_frame_route *r, emb_view *kid)
{
    if (r->focus != kid) {
        if (r->focus != nullptr) {
            frame_blur(r->focus);
        }
        r->focus = kid;
        if (kid->input != nullptr) {
            kid->input->text_on = false; /* asked again for the window: a field focused in the frame starts text input */
        }
        focus_here(kid, true);
    }
    embi_element *el = embi_frames_element(kid);
    embi_input *in = state_of(v);
    embi_bind_ctx *c = in != nullptr ? ctx_of(v) : nullptr;
    if (c != nullptr && el != nullptr && embi_impl_input_focused(c) != &el->base) {
        embi_impl_input_focus(c, &el->base);
        in->st.focus_visible = false;
        in->autofocus_done = true;
        sync(v, in, c);
    }
}

/* The document that has the keyboard below v: v, or its focused frame while v's focus is still on that frame's iframe. */
static emb_view *key_view(emb_view *v)
{
    for (;;) {
        embi_frame_route *r = v->frames != nullptr ? embi_frames_route(v) : nullptr;
        emb_view *kid = r != nullptr ? r->focus : nullptr;
        if (kid == nullptr) {
            return v;
        }
        embi_element *el = embi_frames_element(kid);
        embi_bind_ctx *c = v->input != nullptr ? ctx_of(v) : nullptr;
        if (el == nullptr || c == nullptr || embi_impl_input_focused(c) != &el->base) {
            r->focus = nullptr; /* v's focus moved off the iframe (a script, Tab) */
            frame_blur(kid);
            return v;
        }
        v = kid;
    }
}

/* Whether frame v is where the root's pointer is (every parent up to the root hovers the frame below it). */
static bool hover_chain(emb_view *v)
{
    for (emb_view *p = embi_frames_parent(v); p != nullptr; v = p, p = embi_frames_parent(p)) {
        embi_frame_route *r = p->frames != nullptr ? embi_frames_route(p) : nullptr;
        if (r == nullptr || (r->hover != v && r->press != v)) {
            return false;
        }
    }
    return true;
}

static void frame_cursor(emb_view *v, int32_t kind)
{
    if (embi_frames_parent(v) != nullptr && hover_chain(v)) {
        emb_view *root = embi_frames_root(v);
        if (root->mode == EMB_VIEW_WINDOWED) {
            embi_window_set_cursor(root, kind);
        }
    }
}

void emb_view_mouse_move(emb_view *view, float x, float y, uint32_t mods)
{
    if (view == nullptr) {
        return;
    }
    embi_frame_route *r = view->frames != nullptr ? embi_frames_route(view) : nullptr;
    float lx = 0, ly = 0;
    emb_view *kid = pointer_frame(view, r, x, y, &lx, &ly);
    if (r != nullptr && (kid == nullptr || kid != r->press)) {
        hover_frame(view, r, kid);
    }
    if (kid != nullptr) {
        emb_view_mouse_move(kid, lx, ly, mods);
        return;
    }
    mouse_move_here(view, x, y, mods);
}

void emb_view_mouse_leave(emb_view *view)
{
    if (view == nullptr) {
        return;
    }
    embi_frame_route *r = view->frames != nullptr ? embi_frames_route(view) : nullptr;
    if (r != nullptr && r->press != nullptr && pointer_held(r->press)) {
        return; /* a drag in a frame keeps its target when the pointer leaves the window */
    }
    if (r != nullptr && r->hover != nullptr) {
        emb_view *old = r->hover;
        r->hover = nullptr;
        emb_view_mouse_leave(old);
        return;
    }
    mouse_leave_here(view);
}

void emb_view_mouse_button(emb_view *view, float x, float y, emb_button button, bool down, uint32_t mods)
{
    if (view == nullptr) {
        return;
    }
    embi_frame_route *r = view->frames != nullptr ? embi_frames_route(view) : nullptr;
    float lx = 0, ly = 0;
    emb_view *kid = pointer_frame(view, r, x, y, &lx, &ly);
    if (kid != nullptr) {
        if (r->press != kid) {
            hover_frame(view, r, kid);
        }
        if (down) {
            r->press = kid;
            frame_focus(view, r, kid);
        }
        emb_view_mouse_button(kid, lx, ly, button, down, mods);
        if (!down && r->press == kid && !pointer_held(kid)) {
            r->press = nullptr;
        }
        return;
    }
    if (down && r != nullptr && r->focus != nullptr) {
        emb_view *k = r->focus;
        r->focus = nullptr; /* a press in the parent takes the keyboard back (its own focus moves as usual) */
        frame_blur(k);
    }
    mouse_button_here(view, x, y, button, down, mods);
}

/* The wheel at (x, y) of v: the frame under the point first; what it does not take scrolls v's scrollers there. */
static bool wheel_route(emb_view *v, float x, float y, float dx, float dy)
{
    float lx = 0, ly = 0;
    emb_view *kid = embi_frames_any(v) ? embi_frames_at(v, x, y, &lx, &ly) : nullptr;
    if (kid != nullptr) {
        if (wheel_route(kid, lx, ly, dx, dy)) {
            return true;
        }
        return !embi_view_load_suspended(v) && embi_view_wheel(v, x, y, dx, dy);
    }
    return wheel_here(v, x, y, dx, dy);
}

void embi_input_wheel_at(emb_view *v, float x, float y, float dx, float dy)
{
    if (v != nullptr) {
        (void)wheel_route(v, x, y, dx, dy);
    }
}

void emb_view_touch(emb_view *view, uint64_t id, float x, float y, int phase)
{
    if (view == nullptr || phase < 0 || phase > 3) {
        return;
    }
    embi_frame_route *r = view->frames != nullptr ? embi_frames_route(view) : nullptr;
    int slot = -1;
    for (uint32_t i = 0; r != nullptr && i < EMBI_FRAMES_TOUCHES; i++) {
        if (r->touch[i].kid != nullptr && r->touch[i].id == id) {
            slot = (int)i;
        }
    }
    float lx = 0, ly = 0;
    emb_view *kid = nullptr;
    if (slot >= 0) {
        kid = r->touch[slot].kid;
        if (!embi_frames_map(kid, x, y, &lx, &ly)) {
            lx = x;
            ly = y;
        }
    } else if (phase == 0 && r != nullptr && embi_frames_any(view)) {
        kid = embi_frames_at(view, x, y, &lx, &ly);
        for (uint32_t i = 0; kid != nullptr && i < EMBI_FRAMES_TOUCHES; i++) {
            if (r->touch[i].kid == nullptr) {
                r->touch[i].id = id;
                r->touch[i].kid = kid;
                slot = (int)i;
                break;
            }
        }
        if (slot < 0) {
            kid = nullptr; /* no room: the parent takes it */
        }
        if (kid != nullptr) {
            frame_focus(view, r, kid);
        }
    }
    if (kid != nullptr) {
        emb_view_touch(kid, id, lx, ly, phase);
        if (phase >= 2) {
            r->touch[slot].kid = nullptr;
        }
        return;
    }
    touch_here(view, id, x, y, phase);
}

void emb_view_key(emb_view *view, const emb_key_event *ev)
{
    if (view != nullptr) {
        key_here(key_view(view), ev);
    }
}

void emb_view_text_input(emb_view *view, const char *text, size_t len)
{
    if (view != nullptr) {
        text_input_here(key_view(view), text, len);
    }
}

void emb_view_text_editing(emb_view *view, const emb_text_editing *ev)
{
    if (view != nullptr) {
        text_editing_here(key_view(view), ev);
    }
}

void emb_view_clipboard(emb_view *view, emb_clipboard_command command)
{
    if (view != nullptr) {
        clipboard_here(key_view(view), command);
    }
}

void emb_view_focus(emb_view *view, bool focused)
{
    if (view == nullptr) {
        return;
    }
    /* The view, then down the chain of focused frames. A focus or blur handler may move the focus: the chain is read
     * again at each step, and ends where it ends. */
    for (emb_view *v = view; v != nullptr;) {
        focus_here(v, focused);
        emb_view *leaf = key_view(v);
        if (leaf == v) {
            break;
        }
        embi_frame_route *r = embi_frames_route(v);
        v = r != nullptr ? r->focus : nullptr;
    }
}

void embi_input_frame_focus_sync(emb_view *v)
{
    if (v != nullptr && v->frames != nullptr) {
        (void)key_view(v);
    }
}
