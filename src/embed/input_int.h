#pragma once
/*
 * Internal declarations of the input layer of a view (W5d): the per-view state (owned by the view, emb_view.input), its
 * teardown and reset, the navigation and the SDL text input the windowed mode drives. The public calls are in
 * include/ember/ember_input.h and implemented in input.c.
 */
#include <ember/ember.h>

#include "platform/platform.h"

#include <stdbool.h>

/* Drops the input state of a view and unpins its nodes. Called by emb_view_destroy before the document is freed. */
void embi_input_destroy(emb_view *v);
/* Forgets the pointer, press, focus and composition state of the view and unpins its nodes. Called before the document
 * is replaced (emb_view_load_html), while the old binding context is still alive. The navigation hook stays. */
void embi_input_reset(emb_view *v);
/* Test hook: replaces the clock (ms) used for click counting; nullptr restores the real clock. */
void embi_input_set_clock(double (*fn)(void));
/* The dynamic state the selector hook reads (hover, active, focus, focus-visible), kept current by the input calls. The
 * style engine wires it as the user of embi_sel_hooks (W5d2). Valid until the view is destroyed. */
embi_pf_state *embi_input_pf_state(emb_view *v);
/* Delivers a navigation of the document to the view's navigation hook (an absolute URL; an empty one is dropped). Called
 * by the bindings' navigate hook (script.c). */
void embi_input_navigate(emb_view *v, embi_str url);
/* Whether the embedder installed a navigation hook (emb_view_set_navigate_handler). */
bool embi_input_has_navigate(const emb_view *v);
/* The state of a form control for the painter (paint_cfg.control, W5e): asks the bindings (impl_forms.c). */
bool embi_input_control_state(void *user, const struct embi_element *el, void *out);
/* Whether the form state or the caret blink changed since the last frame, so the display list needs a rebuild (the damage then
 * covers only what differs). Called by the view's update. */
bool embi_input_forms_tick(emb_view *v);
/* A form submission (W5e): true when the form-aware hook took it. */
bool embi_input_navigate_form(emb_view *v, embi_str method, embi_str url, embi_str body, embi_str ctype);
/* Windowed views: the SDL text input is on while a text control has the focus, with the caret rectangle (view CSS px)
 * for the input method. Does nothing for offscreen views. Implemented in window.c. */
void embi_window_text_input(emb_view *v, bool on, float x, float y, float w, float h);

/* The wheel at (x, y) with the scroll deltas dx and dy (CSS px, positive down and right): dispatches the trusted, cancelable
 * wheel event at the element under the point and scrolls (embi_view_wheel) unless a listener canceled it. emb_view_wheel
 * calls it. */
void embi_input_wheel_at(emb_view *v, float x, float y, float dx, float dy);
/* IF1: when v's focus has left the iframe of the frame that had the keyboard, that frame (and the frames below it) lose it
 * now. The frames' update calls it. */
void embi_input_frame_focus_sync(emb_view *v);
/* The last text input request a view made of a window (start or stop, and the caret rectangle in that window's view): a
 * frame's go to its root's window. For the tests. */
typedef struct embi_text_area_call {
    emb_view *window;
    bool on;
    float rect[4];
} embi_text_area_call;
embi_text_area_call embi_input_last_text_area(void);
/* Called by the document once its parse is over: registers the insertion observer that spots autofocus elements and queues
 * the autofocus processing when the page has one. The processing runs on the view's event loop (after the load events were
 * queued): the first autofocus element in tree order takes the focus unless something has it already or the user acted. */
void embi_input_doc_ready(emb_view *v);
/* Tells the input layer that a style rule reads the dynamic state (:hover, :active, :focus, :focus-visible,
 * :focus-within): the document is restyled when the hover, press or focus target changes. Off by default, so that a page
 * without such rules pays nothing for the pointer moving between elements. The style engine's wiring of the selector hook
 * (W5d2) sets it from the stylesheets. Turning it on invalidates the style once. */
void embi_input_set_state_consumed(emb_view *v, bool used);
bool embi_input_state_consumed(emb_view *v);
/* The selected text of target for the copy and cut commands (W5e registers the text controls' selection): append it to out
 * as UTF-8 and return true, or return false when there is no selection. */
typedef bool (*embi_input_selection_fn)(void *user, embi_node *target, embi_sb *out);
void embi_input_set_selection_provider(emb_view *v, embi_input_selection_fn fn, void *user);
/* The element under the mouse pointer (the innermost), nullptr when none: what the style engine reads the CSS cursor
 * property from. */
embi_element *embi_input_hover_element(emb_view *v);
/* Sets the window cursor from a CSS cursor keyword ("pointer", "text", ...; an unknown keyword is the default arrow), and
 * records it as the kind the view shows. Windowed views only set the window. */
void embi_input_apply_cursor(emb_view *v, embi_str css_cursor);
/* Brings the cursor kind of the hover element up to date with the current computed style (no layout is run: view.c calls it
 * after the frame's layout). Windowed views set the window's cursor when the kind changed. */
void embi_input_cursor_refresh(emb_view *v);
/* The cursor kind the view shows (the value embi_window_set_cursor was last given), -1 when none is tracked yet. Tests read
 * it to check the cursor of the element under the pointer without a window. */
int32_t embi_input_cursor_kind(emb_view *v);
/* The SDL system cursor kind (windowed cursors: an SDL_SystemCursor value, -1 arrow, -2 hidden) that the computed cursor of
 * the element under (x, y) asks for, as the window would show it. Runs the layout when it is stale. */
int32_t embi_input_cursor_at(emb_view *v, float x, float y);
/* Brings the selector state of the view up to date with the focus (a script may have moved it since the last input event)
 * and marks the styles dirty when a rule can read the change. view.c calls it before each update. */
void embi_input_refresh_state(emb_view *v);
/* Sets the SDL system cursor of the window: an SDL_SystemCursor value, -1 for the default arrow, -2 to hide the cursor.
 * Cursors are created once per kind and kept until the window goes. Does nothing for offscreen views. window.c. */
void embi_window_set_cursor(emb_view *v, int32_t sdl_cursor);
