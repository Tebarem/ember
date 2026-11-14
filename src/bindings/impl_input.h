#pragma once
/*
 * Input events and default actions of the bindings (W5d): the pointer, mouse, keyboard,
 * focus, input and composition events the input layer dispatches, the focus change algorithm (focus, blur, focusin,
 * focusout), and the activation behaviors of a, label and button. Implemented in impl_input.c. The embedding layer
 * (src/embed/input.c) decides what happens and calls these; the platform rules (focus order, hover chains) are in
 * src/platform.
 */
#include "bindings/impl.h"

/* A pointer or mouse event. The coordinates are CSS px of the viewport. pageX/Y are the document coordinates, offsetX/Y
 * the position in the target's padding box; the embedding layer computes them. related is the other element of an
 * enter, leave, over or out event (nullptr when none). button is -1 for a move (DOM). */
typedef struct embi_input_ptr {
    double clientX, clientY;
    double screenX, screenY;
    double pageX, pageY;
    double offsetX, offsetY;
    double movementX, movementY;
    embi_node *related;
    int16_t button;
    uint16_t buttons;   /* the DOM bit set of the buttons held (primary 1, secondary 2, auxiliary 4, back 8, forward 16) */
    int32_t detail;     /* click count */
    uint32_t mods;      /* EMB_MOD_* (ember_input.h) */
    const char *pointerType; /* "mouse", "pen" or "touch"; used by pointer events only */
    int32_t pointerId;
    bool isPrimary;
} embi_input_ptr;

/* A keyboard event: key and code are DOM values (UTF-8, NUL-terminated). */
typedef struct embi_input_key {
    const char *key;
    const char *code;
    uint32_t location;
    bool repeat;
    bool isComposing;
    uint32_t mods; /* EMB_MOD_* */
} embi_input_key;

/* Dispatches a trusted pointer event (pointer true: a PointerEvent of type, else a MouseEvent) at target and returns
 * true when it was not canceled. type is the DOM type ("pointerdown", "mouseover", "click", ...). */
bool embi_impl_input_pointer(embi_bind_ctx *c, embi_node *target, bool pointer, const char *type,
                             const embi_input_ptr *p, bool bubbles, bool cancelable);
/* A wheel event: the mouse payload (button 0 and buttons as for a move) plus the deltas. deltaMode 0 is pixels. */
typedef struct embi_input_wheel {
    embi_input_ptr ptr;
    double deltaX, deltaY, deltaZ;
    uint32_t deltaMode;
} embi_input_wheel;
/* Dispatches a trusted, cancelable, bubbling WheelEvent "wheel" at target. Returns true when not canceled. */
bool embi_impl_input_wheel(embi_bind_ctx *c, embi_node *target, const embi_input_wheel *w);

/* Whether dispatching an event of type at target could reach anything: a listener of that type on the target, an
 * ancestor, the document or the window, or a content attribute (onclick="...") of an element on the path. Lets the input
 * layer skip building the payload of events nobody can see (pointermove over a page with no handlers). Conservative: a
 * false answer is exact, a true one may be a listener that has since been removed. Activation behaviors (click) do not
 * count: always dispatch those. */
bool embi_impl_input_listening(embi_bind_ctx *c, embi_node *target, const char *type);

/* Dispatches a trusted keyboard event (keydown, keyup or the legacy keypress) at target. Returns true when not canceled. */
bool embi_impl_input_key(embi_bind_ctx *c, embi_node *target, const char *type, const embi_input_key *k);
/* Dispatches beforeinput (cancelable InputEvent) with the input type ("insertText", "deleteContentBackward", ...) and
 * the data (may be nullptr) and the isComposing flag. Returns true when not canceled. */
bool embi_impl_input_before(embi_bind_ctx *c, embi_node *target, const char *input_type, const char *data,
                            bool is_composing);
/* Dispatches a composition event (compositionstart, compositionupdate, compositionend) with the data. */
void embi_impl_input_composition(embi_bind_ctx *c, embi_node *target, const char *type, const char *data);

/* Dispatches a trusted, cancelable, bubbling ClipboardEvent (type "copy", "cut" or "paste") at target. Its clipboardData is
 * a DataTransfer whose "text/plain" is in_text (nullptr: empty). Returns true when the event was not canceled. When out is
 * not nullptr, the "text/plain" of clipboardData after the dispatch (what a listener put there with setData) is appended
 * to it and *has_text tells whether there was any (a listener may set an empty string on purpose). */
bool embi_impl_input_clipboard_event(embi_bind_ctx *c, embi_node *target, const char *type, const char *in_text,
                                     embi_sb *out, bool *has_text);

/* What the input layer lends to the bindings of one context (pointer capture). A context has none until the input layer of
 * its view attaches. The functions run on the view's thread. */
typedef struct embi_input_hooks {
    void *user;
    /* setPointerCapture: false when no pointer with this id is active (the call then throws NotFoundError). */
    bool (*capture_set)(void *user, int32_t pointer_id, embi_node *el);
    /* releasePointerCapture: releases when el holds the capture of the pointer. */
    void (*capture_release)(void *user, int32_t pointer_id, embi_node *el);
    bool (*capture_has)(void *user, int32_t pointer_id, embi_node *el);
} embi_input_hooks;
/* Attaches (or replaces) the hooks of a context; embi_impl_input_detach removes them. The input layer detaches before the
 * context is freed. Returns false when the process-wide table is full. */
bool embi_impl_input_attach(embi_bind_ctx *c, const embi_input_hooks *h);
void embi_impl_input_detach(embi_bind_ctx *c);
/* Whether el may take the focus now (for focus()): the platform layer's focusable-area rules (embi_pf_focusable). */
bool embi_impl_input_can_focus(const embi_element *el);

/* The focus change algorithm: the focused element becomes node (nullptr to blur everything). Fires blur and focusout on
 * the old element and focus and focusin on the new one, in that order, and sets document.activeElement. A node that is
 * not connected or not focusable is ignored. */
void embi_impl_input_focus(embi_bind_ctx *c, embi_node *node);
/* The focused element, or nullptr when none (the body is document.activeElement then). */
embi_node *embi_impl_input_focused(embi_bind_ctx *c);
/* The host window gained (true) or lost (false) the keyboard focus: focus and focusin, or blur and focusout, on the
 * focused element. The focused element itself does not change. */
void embi_impl_input_host_focus(embi_bind_ctx *c, bool focused);

/* The click of an element (HTMLElement.click): a synthetic, cancelable click that bubbles and runs the activation. */
void embi_impl_HTMLElement_click(embi_bind_ctx *c, embi_node *self);

/* Registers the activation behaviors of a, label and button (once per process). Call before any dispatch of input. */
void embi_impl_input_ready(void);

/* Follows a link: raw is the href as written. The URL is resolved against the document, then the navigation goes through
 * the context's navigate hook (the embedder's callback), or stays in the document for a fragment (the hash changes, the
 * hashchange event fires and the target scrolls into view). Implemented in impl_window.c. An empty result does nothing. */
void embi_impl_follow_link(embi_bind_ctx *c, embi_str raw);
/* W5e: a form submission to the embedder: method, the raw action (resolved against the document URL), the body and its content
 * type. Falls back to the plain navigation with the URL when the host has no form hook. */
void embi_impl_follow_form(embi_bind_ctx *c, embi_str method, embi_str raw, embi_str body, embi_str ctype);
