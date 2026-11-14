#pragma once
/*
 * HTML tokenizer, WHATWG HTML 13.2.5. Internal module. The tree builder (W1c) drives it.
 *
 * Input: UTF-8 bytes, streamed through embi_html_tokenizer_write and finished with
 * embi_html_tokenizer_end. Decoding is WTF-8: encoded surrogates (ED A0..BF xx) are accepted as
 * code points and reported as surrogate-in-input-stream, as the spec's input stream does. A BOM at
 * the start is dropped only when embi_html_tokenizer_strip_bom was called. CR and CR LF become LF.
 * Token strings are WTF-8 too.
 *
 * Output: tokens are passed to a sink callback. Strings in a token point into tokenizer buffers
 * and are valid only during the callback. Tag and attribute names are lowercased ASCII where the
 * spec lowercases them. Character runs are coalesced and flushed before any non-character token,
 * and at end of input. Duplicate attributes are dropped (the first one wins).
 *
 * Positions in errors are code point offsets from the start of the stream. Lines are 1-based and
 * columns count UTF-16 code units (a non-BMP character is two), matching the html5lib test data.
 *
 * Pausing: the sink returns false to pause. The tokenizer stops at the next state boundary, and
 * embi_html_tokenizer_paused reports it. Call embi_html_tokenizer_resume before writing more.
 *
 * Usage:
 *     t = embi_html_tokenizer_create(sink, on_error, ctx);
 *     embi_html_tokenizer_write(t, bytes, n);   // returns how many bytes were accepted
 *     embi_html_tokenizer_end(t);
 *     embi_html_tokenizer_destroy(t);
 */

#include "base/base.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* States the tree builder may select with embi_html_tokenizer_set_state. */
typedef enum embi_html_state {
    EMBI_HTML_STATE_DATA,
    EMBI_HTML_STATE_RCDATA,
    EMBI_HTML_STATE_RAWTEXT,
    EMBI_HTML_STATE_SCRIPT_DATA,
    EMBI_HTML_STATE_PLAINTEXT,
    EMBI_HTML_STATE_CDATA_SECTION,
} embi_html_state;

typedef enum embi_html_token_kind {
    EMBI_HTML_TOK_DOCTYPE,
    EMBI_HTML_TOK_START_TAG,
    EMBI_HTML_TOK_END_TAG,
    EMBI_HTML_TOK_COMMENT,
    EMBI_HTML_TOK_PI,
    EMBI_HTML_TOK_CHARACTERS,
    EMBI_HTML_TOK_EOF,
} embi_html_token_kind;

typedef struct embi_html_attr {
    embi_str name;
    embi_str value;
    embi_atom atom; /* the interned name when the tokenizer already looked it up, else 0 */
} embi_html_attr;

/* Field use by kind:
 *   DOCTYPE:      name (has_name), public_id (has_public_id), system_id (has_system_id),
 *                 force_quirks. A missing name or identifier is reported with its flag false.
 *   START_TAG:    name, attrs/attr_count, self_closing.
 *   END_TAG:      name.
 *   COMMENT:      data.
 *   PI:           name (the target), data. xml and xml-stylesheet targets become comments.
 *   CHARACTERS:   data (coalesced run).
 *   EOF:          nothing.
 */
typedef struct embi_html_token {
    embi_html_token_kind kind;
    embi_str name;
    embi_str data;
    bool has_name;
    embi_str public_id;
    bool has_public_id;
    embi_str system_id;
    bool has_system_id;
    bool force_quirks;
    bool self_closing;
    const embi_html_attr *attrs;
    uint32_t attr_count;
    /* START_TAG and END_TAG: has_atom is set when the tokenizer looked the name up in the atom
     * table (it never interns: that stays with the consumer). name_atom is the atom, or 0 when the
     * name was not interned yet; the same holds for embi_html_attr.atom. */
    bool has_atom;
    embi_atom name_atom;
} embi_html_token;

/* Return false to pause the tokenizer. */
typedef bool (*embi_html_sink_fn)(void *ctx, const embi_html_token *tok);
/* code is a stable parse-error name such as "eof-in-tag". on_error may be nullptr. */
typedef void (*embi_html_error_fn)(void *ctx, const char *code, uint64_t offset, uint32_t line,
                                   uint32_t col);

typedef struct embi_html_tokenizer embi_html_tokenizer;

embi_html_tokenizer *embi_html_tokenizer_create(embi_html_sink_fn sink,
                                                embi_html_error_fn on_error, void *ctx);
void embi_html_tokenizer_destroy(embi_html_tokenizer *t);

/* Tree builder hooks. */
void embi_html_tokenizer_set_state(embi_html_tokenizer *t, embi_html_state state);
/* Drops a U+FEFF at the very start of the input. Call before the first write. A document parser
 * that accepts bytes should call this; the tokenizer itself passes a BOM through as a character. */
void embi_html_tokenizer_strip_bom(embi_html_tokenizer *t);
/* Name of the last start tag, for "appropriate end tag". Copied. The tokenizer updates it itself
 * when it emits a start tag, so the tree builder calls this only to seed the value. */
void embi_html_tokenizer_set_last_start_tag(embi_html_tokenizer *t, embi_str name);
/* Lets the tokenizer look the names of the tags it parses quickly up in the atom table (never
 * interning) and pass the result in has_atom, name_atom and embi_html_attr.atom. Off by default,
 * since a standalone tokenizer has no use for atoms and would otherwise create the atom table. */
void embi_html_tokenizer_want_atoms(embi_html_tokenizer *t, bool on);
/* CDATA sections are only recognised in foreign content, so the tree builder turns this on there. */
void embi_html_tokenizer_set_cdata_allowed(embi_html_tokenizer *t, bool allowed);

/* Returns the number of bytes accepted. That is less than n only when the tokenizer paused. */
size_t embi_html_tokenizer_write(embi_html_tokenizer *t, const void *bytes, size_t n);
/* Clears a pause and processes input that is already buffered. */
/* Delivers the character run collected so far (a split of one run into two tokens), unless paused.
 * The parser calls it when control returns to its caller, so the DOM holds all text received. */
void embi_html_tokenizer_flush_text(embi_html_tokenizer *t);
void embi_html_tokenizer_resume(embi_html_tokenizer *t);
bool embi_html_tokenizer_paused(const embi_html_tokenizer *t);
/* Ends the input and emits the final tokens, including EOF. Call resume first if paused. */
void embi_html_tokenizer_end(embi_html_tokenizer *t);
