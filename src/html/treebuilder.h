#pragma once
/*
 * Tree builder, WHATWG 13.2.6. Internal to the html module: parser.c drives it, one token at a
 * time, from the tokenizer sink. It builds nodes with the dom module's raw link operations, which
 * skip the validity checks because the tree builder never inserts a node that would violate them.
 */
#include "html/parser.h"

typedef struct embi_tb embi_tb;

/* context == nullptr builds a document into doc. Otherwise it is a fragment parse against
 * context, and nodes go into context->base.doc. */
embi_tb *embi_tb_new(embi_document *doc, embi_element *context, const embi_html_parser_config *cfg);
/* The tokenizer whose state the tree builder switches (RCDATA, RAWTEXT, PLAINTEXT, script data)
 * and whose CDATA flag it sets. */
void embi_tb_attach(embi_tb *tb, embi_html_tokenizer *tok);
/* Processes one tokenizer token. Returns false when the parser must pause (script). */
bool embi_tb_token(embi_tb *tb, const embi_html_token *tok);
/* The current node (bottom of the stack is the first entry), or nullptr when the stack is empty.
 * Read-only; the parser tests use it to see the stack at a script pause. */
embi_element *embi_tb_current(const embi_tb *tb);
/* Fragment only: moves the root's children into a new DocumentFragment in the destination
 * document, and frees the root. */
embi_node *embi_tb_take_fragment(embi_tb *tb);
/* Writes buffered text into the tree. Call after each tokenizer write so the DOM is current. */
void embi_tb_flush(embi_tb *tb);
/* Whether a script hook answered EMBI_HTML_SCRIPT_STOP: the tree builder builds nothing more. */
bool embi_tb_stopped(const embi_tb *tb);
void embi_tb_free(embi_tb *tb);
