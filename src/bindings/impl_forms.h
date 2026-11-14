#pragma once
/*
 * Form controls, text editing and form submission of the bindings (W5e). Implemented in
 * impl_forms.c. The embedding layer (src/embed/input.c) sends the text input, key defaults and composition boundaries of a
 * focused text control here, and the loader's FormData constructor builds the entries of a form element.
 *
 * The state of a control (its dirty value and selection, its checkedness and the selectedness of an option) lives in the
 * block of its JS wrapper, which exists for every node that is connected to a document.
 */
#include "bindings/impl.h"

/* Applies an editing command to the text control target (input of a text type, or textarea): beforeinput is dispatched
 * first (cancelable; a canceled command changes nothing), then the value changes and input fires (an InputEvent with
 * inputType and data). input_type is the DOM inputType ("insertText", "insertCompositionText", "insertFromPaste",
 * "insertLineBreak", "deleteByCut", "deleteContentBackward", "deleteWordBackward", ...). data is the text to insert (empty
 * for deletions). composing is the isComposing flag: a composition text replaces the composition range of the last update.
 * Returns true when the value changed. Non-text targets are ignored (false). */
bool embi_forms_edit(embi_bind_ctx *c, embi_node *target, const char *input_type, embi_str data, bool composing);

/* A key of a focused editable control that moves the caret or deletes (Backspace, Delete, ArrowLeft/Right, Home, End,
 * ArrowUp/Down in textarea, and Ctrl+A for select all). The keys name the DOM key values; ctrl, shift, alt are the
 * modifiers. Returns true when the key was taken by the control (the caller then skips the page's default action). */
bool embi_forms_key(embi_bind_ctx *c, embi_node *target, const char *key, bool ctrl, bool shift, bool alt);

/* The composition of target starts or ends (the bracketing of insertCompositionText updates). */
void embi_forms_composition_start(embi_bind_ctx *c, embi_node *target);
void embi_forms_composition_end(embi_bind_ctx *c, embi_node *target);

/* Appends the selected text of a text control (copy and cut). Returns false when there is no selection or no text control. */
bool embi_forms_selected_text(embi_node *target, embi_sb *out);

/* Implicit submission: Enter in a text input of a form submits the form (through its default button or, with no submit
 * button, when the form has a single text field). Returns true when the form was submitted. */
bool embi_forms_implicit_submit(embi_bind_ctx *c, embi_node *target);

/* new FormData(form): a FormData of the entries of the form element a0 (a TypeError when a0 is not a form element; the
 * exception is pending then). Returns the FormData object, or EMBI_JS_EXCEPTION. */
embi_js_value embi_forms_formdata_construct(embi_bind_ctx *c, embi_js_value new_target, embi_js_value a0);

/* The activation behaviors of input (checkbox, radio, submit, reset, image) and button (submit, reset). Replaces the
 * button registration of impl_input.c: call after embi_impl_input_ready. */
void embi_forms_ready(void);

/* ---- W5e part 2: focus, commit, history, pointer and the painter's state ---- */
struct embi_paint_ctl;
/* The control got or lost the focus (called by the focus change of the input layer): blur fires the pending change event. */
void embi_forms_focus(embi_bind_ctx *c, embi_node *new_focus);
void embi_forms_blur(embi_bind_ctx *c, embi_node *old_focus);
/* Fires change on a text control whose value the user edited since the last change (Enter, blur, submission). */
void embi_forms_commit(embi_bind_ctx *c, embi_node *target);
/* Undo or redo the last edit of a text control (beforeinput and input with inputType historyUndo or historyRedo). */
bool embi_forms_undo(embi_bind_ctx *c, embi_node *target, bool redo);
/* The pointer on a form control. phase: 0 press, 1 move (with the button down), 2 release. bb is the control's border box and
 * (x, y) the pointer, both in document px; byte is the text offset under the pointer (text controls); clicks the click count.
 * Text controls place the caret or select (drag, word, all); a range moves its thumb; a select opens its popup or picks a
 * list row. Returns true when the control took the pointer. */
bool embi_forms_pointer(embi_bind_ctx *c, embi_node *target, int phase, const float bb[4], float x, float y, uint32_t byte,
                        uint32_t clicks, bool shift);
/* A press while the popup of select sel is open: a click on a row picks it (true), anywhere else closes the popup (false).
 * vh is the viewport height (the popup flips above the control when it does not fit below). */
bool embi_forms_popup_press(embi_bind_ctx *c, embi_node *sel, const float bb[4], float vh, float x, float y);
/* Pointer hover over the popup rows: true when the highlighted row changed (repaint). */
bool embi_forms_popup_hover(embi_bind_ctx *c, embi_node *sel, const float bb[4], float vh, float x, float y);
/* The blink phase of the caret of n at the time now_ms (SDL ticks): 0 draws the caret, 1 hides it; the caret restarts on each
 * edit or move. */
uint32_t embi_forms_caret_phase(const embi_node *n, uint64_t now_ms);
/* A counter that changes whenever the state of any control may have changed (values, selection, checkedness, focus, popups):
 * a view compares it with the value of its last paint to know that the display list is stale. */
uint32_t embi_forms_generation(void);
/* The state of a control for the painter (paint/paint.h embi_paint_control_fn): false for an element that is not a control. */
bool embi_forms_paint_state(const embi_node *n, struct embi_paint_ctl *out);
