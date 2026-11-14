#pragma once
/*
 * Ember input API. Offscreen and windowed views share it: a windowed view translates
 * its SDL events into these calls, and an offscreen host calls them directly. Each call dispatches the DOM events of the
 * input (pointer, mouse, keyboard, focus, composition) on the view's document and runs the default actions that the
 * DOM defines for them (link and button activation, label forwarding, Tab navigation, the scroll keys) unless a
 * listener canceled the event.
 *
 * emb_view_wheel (ember.h) takes part: it dispatches a trusted, cancelable, bubbling WheelEvent ("wheel", deltaMode 0, the
 * deltas in CSS px as passed) at the element under (x, y), and scrolls only when no listener canceled it.
 *
 * Coordinates. Positions are CSS px of the viewport, the same space as emb_view_wheel. Times are not passed in: the
 * view clock stamps the events.
 *
 * Threading. Every call comes from the view's thread, like the rest of the API.
 *
 * Structs passed here start with a `size` field: set it to sizeof the struct.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ember/ember.h>

/* Mouse and pointer buttons, as the DOM numbers them (MouseEvent.button). */
typedef enum emb_button {
    EMB_BUTTON_PRIMARY = 0,
    EMB_BUTTON_AUXILIARY = 1, /* the wheel button */
    EMB_BUTTON_SECONDARY = 2,
    EMB_BUTTON_BACK = 3,
    EMB_BUTTON_FORWARD = 4,
} emb_button;

/* Modifier bits of the input calls. */
#define EMB_MOD_SHIFT 0x01u
#define EMB_MOD_CTRL 0x02u
#define EMB_MOD_ALT 0x04u
#define EMB_MOD_META 0x08u

/* The keyboard location of a key (KeyboardEvent.location). */
typedef enum emb_key_location {
    EMB_KEY_LOCATION_STANDARD = 0,
    EMB_KEY_LOCATION_LEFT,
    EMB_KEY_LOCATION_RIGHT,
    EMB_KEY_LOCATION_NUMPAD,
} emb_key_location;

/* A key press or release. key is the DOM key value (UTF-8, "a", "Enter", "ArrowLeft"), code the physical key
 * ("KeyA", "Digit1", "" when unknown). An empty key is derived from code when possible. */
typedef struct emb_key_event {
    uint32_t size;          /* sizeof(emb_key_event) */
    bool down;              /* true for keydown, false for keyup */
    bool repeat;            /* an auto-repeated keydown */
    uint32_t mods;          /* EMB_MOD_* */
    emb_key_location location;
    const char *key;        /* DOM key value, nullptr is "" */
    const char *code;       /* DOM code value, nullptr is "" */
} emb_key_event;

/* The state of an IME composition, as the input method reports it. text is the composition string (UTF-8, "" when the
 * composition ended without text); cursor and sel_len are in bytes of text. */
typedef struct emb_text_editing {
    uint32_t size;          /* sizeof(emb_text_editing) */
    const char *text;
    int32_t cursor;
    int32_t sel_len;
} emb_text_editing;

/* The navigation hook of the view: called when the document navigates away from itself, that is a link activation (a
 * click on a[href] that was not canceled, or a click() call, from a pointer or a script), or a location assignment. It
 * stays installed for the life of the view, so a script's click reaches it too. url is the absolute URL the navigation
 * resolves to (never empty; the hook is not called for an empty one); the embedder loads it or ignores it. Same-document
 * fragment links (#id) are not passed here: the view scrolls to the target and fires hashchange itself. */
typedef void (*emb_navigate_fn)(void *user, const char *url, size_t url_len);

/* Sets the navigation hook of the view (pass nullptr to remove it). */
EMB_API void emb_view_set_navigate_handler(emb_view *view, emb_navigate_fn fn, void *user);

/* A form submission. method is "GET" or "POST" (upper case), url the absolute action URL (for GET it carries the
 * query), body the request body of a POST (empty for GET) and content_type its media type ("application/x-www-form-urlencoded",
 * "multipart/form-data; boundary=..." or "text/plain"; empty for GET). The strings are valid for the call. */
typedef struct emb_navigation {
    uint32_t size;               /* sizeof(emb_navigation) */
    const char *method;
    const char *url;
    size_t url_len;
    const char *body;
    size_t body_len;
    const char *content_type;
} emb_navigation;
typedef void (*emb_navigate_ex_fn)(void *user, const emb_navigation *nav);
/* Sets the form-aware navigation hook. A form submission calls it instead of the plain hook; without it a submission goes
 * to the plain hook with the URL (a POST then loses its body). */
EMB_API void emb_view_set_navigate_ex_handler(emb_view *view, emb_navigate_ex_fn fn, void *user);

/* The mouse pointer moved to (x, y) CSS px. Dispatches pointer and mouse move, over, out, enter and leave events for
 * the elements under the pointer (the layout hit test; exact paint order and pointer-events handling are not implemented yet). */
EMB_API void emb_view_mouse_move(emb_view *view, float x, float y, uint32_t mods);
/* A mouse button was pressed (down true) or released at (x, y). Dispatches pointerdown or pointerup, mousedown or
 * mouseup, and click, auxclick or contextmenu as the DOM defines them (click counts through detail). */
EMB_API void emb_view_mouse_button(emb_view *view, float x, float y, emb_button button, bool down, uint32_t mods);
/* The pointer left the view (the mouse went out of the window): the hover target becomes none. Dispatches pointerout,
 * pointerleave, mouseout and mouseleave (relatedTarget null) for the elements that were under the pointer. */
EMB_API void emb_view_mouse_leave(emb_view *view);
/* A touch point (pointerType "touch"). id identifies the finger; phase: 0 down, 1 move, 2 up, 3 cancel. Each finger has
 * its own press target and is implicitly captured by it: pointermove, pointerup and pointercancel go to the element the
 * finger went down on, wherever it moves. A touch down fires pointerover and pointerenter first, an up or cancel fires
 * pointerout and pointerleave last. A touch down focuses the focusable element it lands on (a tap focuses a control); a
 * tap (the finger came up near where it went down) fires click at the common ancestor of the press and release elements.
 * There are no compatibility mouse events. */
EMB_API void emb_view_touch(emb_view *view, uint64_t id, float x, float y, int phase);
/* A key was pressed or released. Dispatches keydown or keyup on the focused element (the body when nothing has the
 * focus). A canceled keydown skips the default action (Tab navigation, activation, scrolling, the clipboard shortcuts).
 * For a key that produces a character (a one-character key without Ctrl or Meta, and Enter) an uncanceled keydown
 * is followed by the legacy keypress event; canceling it skips the default action too. */
EMB_API void emb_view_key(emb_view *view, const emb_key_event *ev);
/* Committed text from the keyboard or an input method (UTF-8). A composition in progress ends first. Dispatches
 * beforeinput on the focused element; the editing of text controls is done by the engine. */
EMB_API void emb_view_text_input(emb_view *view, const char *text, size_t len);
/* The composition of an input method changed. Dispatches compositionstart, compositionupdate and compositionend. */
EMB_API void emb_view_text_editing(emb_view *view, const emb_text_editing *ev);
/* The view gained (focused true) or lost the keyboard focus of the host. Dispatches focus and blur on the focused
 * element (not on the window: the window's own events are not affected). Losing the focus ends a composition in progress
 * (compositionend) and the press of a mouse button. */
EMB_API void emb_view_focus(emb_view *view, bool focused);

/* The clipboard commands of a host (a menu or a key binding that is not Ctrl/Cmd+C, X or V). */
typedef enum emb_clipboard_command {
    EMB_CLIPBOARD_COPY = 0,
    EMB_CLIPBOARD_CUT,
    EMB_CLIPBOARD_PASTE,
} emb_clipboard_command;
/* Runs a clipboard command on the focused element (the body when nothing has the focus). Dispatches the trusted, cancelable
 * copy, cut or paste event with a clipboardData that holds text/plain: for paste the text of the system clipboard, for copy
 * and cut empty so that a listener can fill it with setData. When the event is canceled, a copy or cut writes the
 * clipboardData text/plain to the system clipboard (and a paste does nothing more); when it is not, the default action
 * runs: copy and cut write the selected text of a text control, and cut removes it, paste inserts the clipboard text
 * (both through beforeinput; the engine does the editing itself). Ctrl+C, Ctrl+X, Ctrl+V and Ctrl+Insert, Shift+Delete and
 * Shift+Insert (Cmd instead of Ctrl on the Meta key) of emb_view_key run these after an uncanceled keydown. The system
 * clipboard is SDL's, or a process-local buffer when SDL's video is not initialized (embi_pf_clipboard_native). */
EMB_API void emb_view_clipboard(emb_view *view, emb_clipboard_command command);
