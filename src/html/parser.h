#pragma once
/*
 * HTML parser API: streaming document parsing and fragment parsing, WHATWG 13.2.6 and 13.4.
 * Internal module API. The bindings (W5) call it.
 *
 * Document parsing: create a parser for a document, write UTF-8 bytes in any chunking, and call
 * embi_html_parser_end. A byte of a leading UTF-8 BOM is dropped. When a script asks for a pause,
 * embi_html_parser_write returns the number of bytes it accepted. Call
 * embi_html_parser_resume and write the rest again. Buffered tokens are processed by resume.
 *
 * Fragment parsing: parses against a context element. Nodes are built in the context element's
 * own document (never in a scratch document), and the result is a DocumentFragment owned by the
 * caller. Scripts in a fragment never run, so on_script is not called and no pause happens.
 *
 * Processing instructions: "<?target data>" and "<?target data?>" with a target of ASCII letters,
 * digits, '-' and '_' (starting with a letter or '_') become ProcessingInstruction nodes, in every
 * insertion mode, at the same place a comment would go. Targets "xml" and "xml-stylesheet" (any
 * case) and invalid targets stay bogus comments.
 *
 * Customizable select: the contents of <selectedcontent> are filled once, at end of input (or
 * the end of a fragment), from the selected option. They are not updated afterwards, and not
 * refreshed when the DOM changes after parsing. Script-driven changes to the selected option are
 * a later (bindings) concern.
 *
 * The tree builder reports no parse errors of its own. Tokenizer errors go to on_error.
 */
#include "dom/dom.h"
#include "html/tokenizer.h"

typedef enum embi_html_script_result {
    EMBI_HTML_SCRIPT_CONTINUE = 0,
    EMBI_HTML_SCRIPT_PAUSE,
    /* The script replaced the document the parser builds into: the parser pauses and never touches the document again (it only
     * has to be destroyed). */
    EMBI_HTML_SCRIPT_STOP,
} embi_html_script_result;

/* Called when a script end tag has been processed. The script element has already been popped
 * from the stack of open elements, and it should now run. Returning PAUSE suspends the parser
 * until embi_html_parser_resume. */
typedef embi_html_script_result (*embi_html_script_fn)(void *ctx, embi_element *script);

typedef struct embi_html_parser_config {
    bool scripting;                /* scripting flag: when true, noscript content is raw text */
    embi_html_script_fn on_script; /* may be nullptr */
    void *script_ctx;
    embi_html_error_fn on_error;   /* tokenizer parse errors, may be nullptr */
    void *error_ctx;
} embi_html_parser_config;

typedef struct embi_html_parser embi_html_parser;

/* cfg may be nullptr (scripting off, no hooks). */
embi_html_parser *embi_html_parser_new(embi_document *doc, const embi_html_parser_config *cfg);
/* Returns the number of bytes accepted. That is less than n only when the parser paused. */
size_t embi_html_parser_write(embi_html_parser *p, const void *bytes, size_t n);
void embi_html_parser_resume(embi_html_parser *p);
bool embi_html_parser_paused(const embi_html_parser *p);
/* The current node of the stack of open elements, or nullptr. Read-only; a test aid for seeing
 * the stack at a script pause (the script element is already popped when on_script runs). */
embi_element *embi_html_parser_current(const embi_html_parser *p);
/* Ends the input. Resume first when paused. */
void embi_html_parser_end(embi_html_parser *p);
void embi_html_parser_destroy(embi_html_parser *p);

/* Fragment parsing (innerHTML). Returns a new DocumentFragment in context's document. Free it
 * with embi_destroy_detached, or move it into the tree. cfg may be nullptr. */
embi_node *embi_html_parse_fragment(embi_element *context, embi_str input,
                                    const embi_html_parser_config *cfg);
