/*
 * W8b: a restyle for a state of an element that the DOM does not carry (style.h embi_style_mark_state). The form controls of
 * the bindings keep the checkedness of checkboxes and radios and the selectedness of options; when one changes they call
 * this so the rules with :checked match again (the TodoMVC toggle circle is `.toggle:checked + label`).
 */
#include "inval.h"
#include "style_int.h"

void embi_style_mark_state(embi_style_engine *e, embi_element *el)
{
    embi_docstate *ds = el->base.doc != nullptr ? embi_style_ds_find(e, el->base.doc) : nullptr;
    if (ds != nullptr) {
        embi_inval_state(ds, el);
    }
}

bool embi_style_mark_user_state(embi_style_engine *e, embi_element *el, embi_sel_pc pc)
{
    int which;
    switch (pc) {
    case EMBI_PC_HOVER:
        which = EMBI_USTATE_HOVER;
        break;
    case EMBI_PC_ACTIVE:
        which = EMBI_USTATE_ACTIVE;
        break;
    case EMBI_PC_FOCUS:
        which = EMBI_USTATE_FOCUS;
        break;
    case EMBI_PC_FOCUS_VISIBLE:
        which = EMBI_USTATE_FOCUS_VISIBLE;
        break;
    case EMBI_PC_FOCUS_WITHIN:
        which = EMBI_USTATE_FOCUS_WITHIN;
        break;
    default:
        return false;
    }
    embi_docstate *ds = el->base.doc != nullptr ? embi_style_ds_find(e, el->base.doc) : nullptr;
    if (ds != nullptr) {
        embi_inval_user_state(ds, el, which);
    }
    return true;
}
