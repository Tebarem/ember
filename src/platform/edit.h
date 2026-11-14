#pragma once
/*
 * The text editing model of the form controls (W5e): the value of an input or textarea as UTF-8
 * with a selection (anchor and focus as byte offsets on code point boundaries). Pure: no DOM, no view, no events. The
 * bindings (src/bindings/impl_forms.c) and the key and pointer defaults of the embedding layer drive it and fire the events.
 *
 * Offsets that scripts see (selectionStart, maxlength, the length of the value) are UTF-16 code units; the *16 functions
 * convert. maxlength counts UTF-16 code units too.
 */
#include "base/base.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct embi_edit {
    embi_sb text;    /* UTF-8 text; data is nullptr while it is empty */
    uint32_t anchor; /* byte offset of the selection anchor */
    uint32_t focus;  /* byte offset of the focus: the caret when the selection is collapsed */
    int dir;         /* EMBI_EDIT_DIR_*: the selectionDirection (kept while the selection is collapsed) */
} embi_edit;

/* The selection direction of HTML (selectionDirection). */
enum {
    EMBI_EDIT_DIR_NONE = 0,
    EMBI_EDIT_DIR_FORWARD,
    EMBI_EDIT_DIR_BACKWARD,
};

/* Caret movements. Extending keeps the anchor (Shift+arrow). */
typedef enum embi_edit_motion {
    EMBI_EDIT_CHAR_LEFT,
    EMBI_EDIT_CHAR_RIGHT,
    EMBI_EDIT_WORD_LEFT,
    EMBI_EDIT_WORD_RIGHT,
    EMBI_EDIT_LINE_START, /* to after the previous newline (or the start) */
    EMBI_EDIT_LINE_END,   /* to before the next newline (or the end) */
    EMBI_EDIT_TEXT_START,
    EMBI_EDIT_TEXT_END,
    EMBI_EDIT_LINE_UP,   /* to the same column (in code points) of the previous line, or the start of the text on the first */
    EMBI_EDIT_LINE_DOWN, /* the same on the next line, or the end of the text on the last */
} embi_edit_motion;

void embi_edit_free(embi_edit *e);

/* Replaces the whole value. The caret moves to the end and the selection collapses there. Returns false on allocation
 * failure (the value is then unchanged). */
bool embi_edit_set_value(embi_edit *e, embi_str v);
/* The value (UTF-8). ptr may be nullptr when len is 0. */
embi_str embi_edit_value(const embi_edit *e);

/* The length of the value in UTF-16 code units. */
uint32_t embi_edit_len16(const embi_edit *e);
/* Converts a byte offset to UTF-16 units, and a UTF-16 offset (clamped to the length, rounded down to a code point) to a
 * byte offset. */
uint32_t embi_edit_to16(const embi_edit *e, uint32_t byte);
uint32_t embi_edit_to_byte(const embi_edit *e, uint32_t u16);

/* The selection in UTF-16 units (start <= end) and its direction. */
void embi_edit_get_selection(const embi_edit *e, uint32_t *start16, uint32_t *end16, int *dir);
/* setSelectionRange: both ends are clamped to the value and start is ordered before end. dir is one of EMBI_EDIT_DIR_*;
 * the focus goes to end for forward and none, to start for backward. */
void embi_edit_set_selection(embi_edit *e, uint32_t start16, uint32_t end16, int dir);
/* Selects the whole value (the focus at the end, direction none as the select() method leaves it). */
void embi_edit_select_all(embi_edit *e);
bool embi_edit_has_selection(const embi_edit *e);
/* The selected text (UTF-8, possibly empty, ptr may be nullptr). */
embi_str embi_edit_selected(const embi_edit *e);

/* Replaces the selection (or inserts at the caret) with s. When max16 is nonzero the result may not exceed max16 UTF-16
 * units: s is cut at a code point boundary to fit (the selection is replaced first, so it counts as removed). The caret
 * goes after the inserted text. Returns true when the value changed. False on allocation failure. */
bool embi_edit_insert(embi_edit *e, embi_str s, uint32_t max16);
/* Deletes the selection, else one code point before (backward) or after (forward) the caret. Returns true when the value
 * changed. */
bool embi_edit_delete(embi_edit *e, bool forward);
/* Deletes the selection, else the word before or after the caret (Ctrl+Backspace, Ctrl+Delete). */
bool embi_edit_delete_word(embi_edit *e, bool forward);
/* Moves the caret (or extends the selection when extend is set). A character move with a selection collapses it to its
 * near end instead, as the platform text fields do. */
void embi_edit_move(embi_edit *e, embi_edit_motion m, bool extend);

/* Removes every LF and CR from the value (single-line controls: value sanitization). The selection is clamped to the new
 * value. Returns true when the value changed. */
bool embi_edit_strip_newlines(embi_edit *e);
