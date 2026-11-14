#pragma once
/*
 * Platform logic of the input layer (W5d): the parts of input that need no event dispatch and
 * no view, so they can be tested alone. Focusable areas and sequential focus navigation, the hover chains and the
 * enter/leave sequences, click counts, the key names of SDL keys, and the answers of the dynamic and form
 * pseudo-classes (the selector state hook of css/selector.h). The DOM events themselves are dispatched by
 * src/bindings/impl_input.c, and the public entry points are in src/embed/input.c.
 */
#include "dom/dom.h"
#include "css/selector.h"

#include <stdbool.h>
#include <stdint.h>

#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_scancode.h>

/* ------------------------------------------------------------------ focus (focus.c) */

/* Whether el is a disabled form control: the disabled attribute, or inside a disabled fieldset (outside its first legend).
 * Only HTML elements. */
bool embi_pf_disabled(const embi_element *el);
/* Whether el is a focusable area: a form control that is not disabled (input type=hidden never is), a[href] and
 * area[href], iframe, summary, or any element with a valid tabindex attribute (an invalid value counts as absent;
 * tabindex -1 included: focusable by script, skipped by Tab). Only HTML elements. */
bool embi_pf_focusable(const embi_element *el);
/* The tabindex that orders el in sequential navigation: the parsed attribute when present, 0 for the default focusable
 * elements, -1 for the rest. */
int32_t embi_pf_tabindex(const embi_element *el);
/* The focus order of the subtree of root (root included): the elements with tabindex >= 0 that are focusable, positive
 * tabindex first in ascending order, then the zeros, each group in tree order. The array is allocated with embi_alloc;
 * free it with embi_pf_order_free. *len is the count (out may be nullptr when there are none). */
void embi_pf_focus_order(embi_node *root, embi_element ***out, uint32_t *len);
void embi_pf_order_free(embi_element **items, uint32_t len);
/* The element that Tab (reverse false) or Shift+Tab (reverse true) moves the focus to from current. Wraps around at the
 * ends. current may be nullptr (the first element forward, the last reverse). When current is not in the order (tabindex
 * -1, a click-focused element, one that is no longer focusable), the step starts from its place in the tree: forward takes
 * the first tabbable (tabindex 0) element after it in tree order, reverse the last one before it, and the order's start
 * or end when there is none. Returns nullptr when the order is empty. */
embi_element *embi_pf_focus_step(embi_node *root, const embi_node *current, bool reverse);
/* Drops elements from the focus order: return false to skip el (display:none, not rendered, ...). */
typedef bool (*embi_pf_keep_fn)(void *user, const embi_element *el);
/* embi_pf_focus_step over the elements keep accepts (keep may be nullptr). */
embi_element *embi_pf_focus_step_filtered(embi_node *root, const embi_node *current, bool reverse, embi_pf_keep_fn keep,
                                          void *user);
/* The focus fixup test: true when focused is set and no longer connected to its document (it was removed). The caller
 * then moves the focus to the body. */
bool embi_pf_focus_lost(const embi_node *focused);

/* ------------------------------------------------------------------ pointer (pointer.c) */

/* Longest ancestor chain the hover logic handles. A chain deeper than this loses its innermost elements (they are not
 * entered or left), so chains of the same tree stay aligned at the root. */
#define EMBI_PF_MAX_DEPTH 512u

/* The element chain of el: el first, then its ancestors up to the root, skipping non-element nodes. Writes at most cap
 * entries to out and returns the count. A chain longer than cap keeps its outermost cap elements (the innermost ones are
 * dropped), so two chains of one tree always end at the same root and embi_pf_crossings stays correct. */
uint32_t embi_pf_chain(const embi_node *el, embi_element **out, uint32_t cap);

/* The enter and leave sequence of a pointer that moved from old (the chain of the previous target) to new (the chain of
 * the next one). leave receives the elements that left, innermost first (mouseleave order); enter receives the elements
 * that were entered, outermost first (mouseenter order). Both are capped by cap. The returned pair is the counts. */
typedef struct embi_pf_crossing {
    uint32_t nleave, nenter;
} embi_pf_crossing;
embi_pf_crossing embi_pf_crossings(const embi_element *const *old_chain, uint32_t n_old,
                                   const embi_element *const *new_chain, uint32_t n_new,
                                   embi_element **leave, embi_element **enter, uint32_t cap);

/* Click counts (MouseEvent.detail). A press counts as the next click of a sequence when it is on the same button within
 * EMBI_PF_CLICK_MS of the previous press, within EMBI_PF_CLICK_PX of its place, on the same target. */
#define EMBI_PF_CLICK_MS 500.0
#define EMBI_PF_CLICK_PX 4.0f

typedef struct embi_pf_clicks {
    uint32_t count;
    uint32_t button;
    double last_ms;
    float x, y;
    const void *target;
} embi_pf_clicks;

/* Records a press and returns its click count (1 for the first press of a sequence). */
uint32_t embi_pf_click_press(embi_pf_clicks *s, double now_ms, float x, float y, uint32_t button, const void *target);

/* ------------------------------------------------------------------ clipboard (clipboard.c) */

/* The system clipboard as text. SDL's clipboard when the video subsystem is up (a window, or the offscreen/dummy video
 * driver), else a process-local text buffer so that copy and paste still work between the views of a headless process.
 * Both calls are thread-safe. */
/* Appends the clipboard text (UTF-8) to out. Returns false when the clipboard could not be read (nothing appended). An
 * empty clipboard is true with nothing appended. */
bool embi_pf_clipboard_get(embi_sb *out);
/* Replaces the clipboard text. Returns false when the system clipboard refused it. */
bool embi_pf_clipboard_set(embi_str text);
/* Whether the system clipboard (SDL) is in use, false when the process-local buffer is. */
bool embi_pf_clipboard_native(void);

/* ------------------------------------------------------------------ cursor (cursor.c) */

/* The SDL system cursor (an SDL_SystemCursor value) of a CSS cursor keyword ("pointer", "text", "not-allowed", ...). Returns
 * -1 for "auto" and "default" and for keywords with no system cursor (the default arrow is then used); "none" returns
 * -2 (hide the cursor). Case-insensitive. */
int32_t embi_pf_cursor_from_css(embi_str keyword);

/* ------------------------------------------------------------------ selector state (state.c) */

/* The dynamic state the pseudo-classes read. The view keeps one and updates it on each input. hover and active are the
 * elements under the pointer and pressed (the innermost; their ancestors match :hover and :active too). focus is the
 * focused element, focus_visible tells whether the focus ring applies (keyboard focus). */
typedef struct embi_pf_state {
    const embi_element *hover;
    const embi_element *active;
    const embi_element *focus;
    bool focus_visible;
} embi_pf_state;

/* The embi_sel_hooks state answer for a state pseudo-class. user is an embi_pf_state. The matcher (src/css/match.c)
 * calls it for hover, active, focus, focus-visible, focus-within, target, modal, popover-open, open, placeholder-shown,
 * read-only, read-write, valid, invalid, in-range and out-of-range; the other state classes (defined, enabled, disabled,
 * checked, indeterminate, required, optional, default) are decided by the matcher's attribute fallback. Use it as embi_sel_hooks { &state, embi_pf_sel_state }. */
bool embi_pf_sel_state(void *user, embi_element *el, embi_sel_pc pc);

/* The validity of a form control by its attributes: required, minlength, maxlength, email and url syntax, and min and
 * max for number and range. Returns false when the control is invalid. Form controls only; other elements are valid.
 * Value comes from the value attribute (the live value is W5e's). */
bool embi_pf_control_valid(const embi_element *el);

/* ------------------------------------------------------------------ keys (keys.c) */

/* The DOM key value of an SDL key: a one-character string for printable keys (the shift state chooses the case, with the
 * US symbols), a name for the others ("Enter", "ArrowLeft"). Keypad digits are digits only while SDL_KMOD_NUM is in mods;
 * with NumLock off they name the navigation key ("End", "ArrowDown", ...). Writes a NUL-terminated string into buf and
 * returns its length; "Unidentified" when the key has no name. CapsLock is applied only by embi_pf_key_name_layout. */
uint32_t embi_pf_key_name(SDL_Keycode key, uint16_t mods, char *buf, uint32_t cap);
/* The DOM key value of a key event on the current layout. base is the event's keycode (the unmodified key), resolved the
 * keycode SDL_GetKeyFromScancode returns for the scancode with the modifier state mods (Shift, AltGr and the layout's own
 * levels applied). A printable resolved key is the character itself (ASCII letters take their case from Shift xor
 * CapsLock); anything else is named by base like embi_pf_key_name. This is what the windowed mode uses; the US shift
 * table of embi_pf_key_name is the fallback for callers without a layout. */
uint32_t embi_pf_key_name_layout(SDL_Keycode base, SDL_Keycode resolved, uint16_t mods, char *buf, uint32_t cap);
/* The DOM code value of an SDL scancode ("KeyA", "Digit1", "Enter", "ShiftLeft"), "" when unknown. Returns the length. */
uint32_t embi_pf_code_name(SDL_Scancode sc, char *buf, uint32_t cap);
/* The DOM location of an SDL scancode (KeyboardEvent.location): 0 standard, 1 left, 2 right, 3 numpad. */
uint8_t embi_pf_key_location(SDL_Scancode sc);
