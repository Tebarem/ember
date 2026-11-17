#pragma once
/*
 * Incremental style (W6b): invalidation sets, dirty paths, partial restyle and the change
 * set a layout engine consumes. Internal to the style module; the public surface is the end of style.h.
 *
 * Invalidation. Every DOM change marks elements in the document's dirty map (EMBI_IVD_* bits). The bits say what the
 * partial restyle must do when it reaches the element: restyle it (SELF), restyle everything below it (DESC), restyle the
 * siblings after it (SIB, SIBDEEP with their subtrees), restyle only the next sibling (NEXT, NEXTDEEP with its subtree:
 * a + b), restyle all its children (KIDS, KIDSDEEP), or just look below it (SUB, which every ancestor of a marked element
 * carries). What a class, id or attribute change reaches is read from
 * the selectors (inval.c builds, per set of rules, a table from names to the relations in which a selector uses
 * them), so a class change on one element marks only what the rules can see of it.
 */
#include "style/style_int.h"

enum {
    EMBI_IVD_SELF = 0x01,     /* restyle this element */
    EMBI_IVD_DESC = 0x02,     /* restyle every descendant */
    EMBI_IVD_SUB = 0x04,      /* something below is marked */
    EMBI_IVD_SIB = 0x08,      /* restyle the following siblings */
    EMBI_IVD_SIBDEEP = 0x10,  /* restyle the following siblings and everything below them */
    EMBI_IVD_KIDS = 0x20,     /* restyle all children */
    EMBI_IVD_KIDSDEEP = 0x40, /* restyle all children and everything below them */
    EMBI_IVD_NEXT = 0x80,     /* restyle the next element sibling */
    EMBI_IVD_NEXTDEEP = 0x100, /* restyle the next element sibling and everything below it */
    EMBI_IVD_ANIM = 0x200,    /* WPTASAN: restyle this element, for its animations only (its own inputs did not change) */
};

/* What a name reaches (embi_ivset.names values, embi_ivset.anyattr). */
enum {
    EMBI_IVM_DESC = 0x01,
    EMBI_IVM_SIB = 0x02,
    EMBI_IVM_SIBDEEP = 0x04,
    EMBI_IVM_HAS = 0x08,
    EMBI_IVM_ALLSIB = 0x10,
    EMBI_IVM_NEXT = 0x20,     /* the next element sibling (a + b) */
    EMBI_IVM_NEXTDEEP = 0x40, /* the next element sibling and its descendants (a + b c) */
    EMBI_IVM_SELF = 0x80,     /* the element itself (the name is read in a subject compound) */
};

/* embi_ivset.flags */
enum {
    EMBI_IVF_SIBCOMB = 0x01,     /* some selector has a sibling combinator */
    EMBI_IVF_STRUCT = 0x02,      /* some selector reads the position among siblings (:first-child, :nth-child ...) */
    EMBI_IVF_STRUCT_ANC = 0x04,  /* ... outside its subject compound */
    EMBI_IVF_HAS = 0x08,         /* :has() */
    EMBI_IVF_HASSIB = 0x10,      /* :has() with a sibling relation */
    EMBI_IVF_EMPTY = 0x20,       /* :empty */
};

/* ------------------------------------------------------------------ inval.c */

void embi_inval_ds_free(embi_style_engine *e, embi_docstate *ds);
void embi_inval_children(embi_docstate *ds, embi_node *parent, embi_node *fa, embi_node *la, embi_node *fr,
                         embi_node *lr);
void embi_inval_attr(embi_docstate *ds, embi_element *el, uint8_t ns, embi_atom ns_uri, embi_atom local,
                     embi_dstr old, bool had);
void embi_inval_chardata(embi_docstate *ds, embi_node *node);
/* Forgets everything recorded about an element that leaves the document. */
void embi_style_purge_element(embi_docstate *ds, embi_element *el);
/* Marks el for a restyle of its own style (W6c, the animations). */
void embi_inval_mark_self(embi_docstate *ds, embi_element *el);
/* WPTASAN: el is restyled for its animations (EMBI_IVD_ANIM): like a SELF mark, but the restyle tells apply that the element's
 * own inputs are unchanged (embi_style_anim_own_change). */
void embi_inval_mark_anim(embi_docstate *ds, embi_element *el);
/* W8b: a state of el the DOM does not carry changed (checkedness, selectedness): as an attribute change for the rules that
 * read the attribute-derived state pseudo-classes. */
void embi_inval_state(embi_docstate *ds, embi_element *el);
/* CF1: the user-action state which (EMBI_USTATE_*) of el flipped: marks what the rules that read it reach from el. */
void embi_inval_user_state(embi_docstate *ds, embi_element *el, int which);
/* The bits of the dirty map for el (0 when none). */
uint16_t embi_inval_get(const embi_docstate *ds, const embi_element *el);

/* ------------------------------------------------------------------ inval_walk.c */

/* The comparison of two styles by content: 0 when no visible property differs, else an embi_style_change_class
 * (REFRESH, PAINT or LAYOUT). Pointer-equal styles give 0. */
uint8_t embi_style_diff(const embi_style *a, const embi_style *b);
/* Records that el's style slot goes from old to cur (called before the slot is replaced). */
void embi_style_note_change(embi_docstate *ds, embi_element *el, const embi_style *old, const embi_style *cur);
void embi_style_note_pseudo(embi_docstate *ds, embi_element *el, uint8_t cls);
/* The partial restyle of the marked paths. Leaves ds->full set when it needs the full procedure instead. */
void embi_style_partial_pass(embi_style_engine *e, embi_docstate *ds, embi_document *doc);
/* A full restyle after a partial one, compared with it (EMBER_VERIFY_INCREMENTAL). */
void embi_style_verify_pass(embi_style_engine *e, embi_docstate *ds, embi_document *doc);

/* ------------------------------------------------------------------ resolve.c */

embi_docstate *embi_style_ds_find(embi_style_engine *e, embi_document *doc);
uint64_t embi_style_mix(uint64_t h, uint64_t v);
uint64_t embi_style_identity_hash(embi_element *el);
const embi_style *embi_style_enter_element(embi_style_engine *e, embi_docstate *ds, embi_element *el,
                                           const embi_style *parent, bool is_root, uint64_t anc, uint64_t ident);
/* The restyle of el under a parent whose style changed in its custom properties only (changed: a bloom of their names,
 * embi_style_vbit), when el's own inputs are as before (no mark of its own: its rules match as they did) and its style
 * passes the parent's custom properties on and reads none of the changed ones (EMBI_STYLEF_CUSTOM_PASS): its values stay
 * and its custom map becomes the parent's, without a cascade. nullptr when that does not apply (the caller restyles el in
 * full). */
const embi_style *embi_style_enter_custom_only(embi_style_engine *e, embi_docstate *ds, embi_element *el,
                                               const embi_style *parent, uint64_t changed);
void embi_style_full_resolve(embi_style_engine *e, embi_docstate *ds, embi_document *doc);
double embi_style_root_font_px(embi_document *doc);
