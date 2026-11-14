/* Owns the static atom name table (from atoms.inc). Everything else in the DOM uses the enum. */
#define EMBI_DOM_ATOM_TABLE 1
#include "dom/dom.h"

/* The base atom table interns the static names (src/base/static_atoms.inc, generated together with
 * atoms.inc) before anything else, so EMBI_ATOM_* ids hold in any init order. embi_dom_init is
 * optional: it only checks that the ids match. */
static bool atom_matches(uint32_t id)
{
    const char *s = embi_dom_atom_names[id];
    return embi_str_eq(embi_atom_str(id), (embi_str){s, (uint32_t)SDL_strlen(s)});
}

void embi_dom_init(void)
{
    /* Cheap assertion (called per document): the first and last static ids. The atoms test
     * verifies every id. A mismatch means atoms.inc and static_atoms.inc are out of sync. */
    if (!atom_matches(1) || !atom_matches(EMBI_ATOM_STATIC_COUNT - 1u)) {
        embi_log_error("dom: static atom ids do not match atoms.inc; regenerate with atomgen");
        abort();
    }
}
