#pragma once
/*
 * MutationObserver core (W6d, DOM Standard "queue a mutation record", "transient registered observers").
 * Internal to the DOM and the bindings (include "dom/mutation.h").
 *
 * The DOM calls the embi_mut_* hooks after each change (tree.c, node.c, document.c). The hooks queue records and
 * never run script. The binding (src/bindings/impl_observers.c) owns the observer objects: it keeps each embi_mo in
 * its wrapper, pins the wrapper while the observer observes (embi_mo_observe returns true on the first observation),
 * releases the pin when embi_mo_disconnect returns true, creates the MutationRecord objects and delivers the queues
 * in a microtask. The microtask is requested through embi_mut_set_signal, which is called once per batch of
 * mutations.
 *
 * Holding. A node that a record, an observation or a transient registration refers to gets EMBI_NF_HELD and is listed
 * in the document's held list (embi_mut_hold). The DOM never frees a held subtree (embi_destroy_detached skips it).
 * The holds are not counted: embi_mut_release recomputes them from the live references (the queued records, the
 * registrations, the transient registrations, and the binding's own references reported through its callback), so a
 * held node is released as soon as nothing refers to it, and the detached roots of released nodes that have no wrapper
 * are freed. The binding calls embi_mut_release after each point where it drops references (delivery, takeRecords,
 * disconnect, unobserve); the DOM only queues records and never releases.
 *
 * Threading: single-threaded, like the DOM.
 */
#include "dom/dom.h"

typedef struct embi_mut embi_mut;
typedef struct embi_mo embi_mo;

/* Record types. */
enum {
    EMBI_MR_ATTRIBUTES = 1,
    EMBI_MR_CHARACTER_DATA,
    EMBI_MR_CHILD_LIST,
};

/* The options of one observation: MutationObserverInit with the defaults already applied. */
typedef struct embi_mo_opts {
    bool child_list;
    bool attributes;
    bool character_data;
    bool subtree;
    bool attr_old;      /* attributeOldValue */
    bool char_old;      /* characterDataOldValue */
    bool has_filter;    /* attributeFilter was given (possibly empty: then nothing matches) */
    uint32_t nfilter;
    embi_atom *filter;  /* owned copy of the attribute local names; nullptr when there are none */
} embi_mo_opts;

/* A queued mutation record. The arrays and old_value are owned by the record. */
typedef struct embi_mrec {
    uint8_t type;       /* EMBI_MR_* */
    bool has_old;       /* oldValue is not null: old_value holds it */
    uint8_t attr_ns;    /* attributes: namespace (embi_ns) */
    embi_atom attr_ns_uri;
    embi_atom attr_local;
    embi_node *target;
    embi_node *prev;    /* childList: previousSibling (nullptr: none) */
    embi_node *next;    /* childList: nextSibling (nullptr: none) */
    embi_node **added;
    uint32_t nadded;
    embi_node **removed;
    uint32_t nremoved;
    embi_dstr old_value;
} embi_mrec;

/* One observation (registered observer) of a node. */
typedef struct embi_mreg {
    embi_node *node;
    embi_mo_opts opts;
} embi_mreg;

/* A MutationObserver. Allocated by the binding (the wrapper owns it); the DOM core owns the arrays inside. */
struct embi_mo {
    embi_mut *mut;      /* the document state while observing, nullptr otherwise */
    embi_mreg *regs;    /* observations, one per target */
    uint32_t nregs, capregs;
    embi_mrec *recs;    /* queued records */
    uint32_t nrecs, caprecs;
    bool active;        /* listed in the document's active observers */
    uint32_t mark;      /* scratch: the gathering pass that last visited this observer */
    void *user;         /* binding: the record of the wrapper (set by the binding) */
    uint32_t pin;       /* binding: persistent handle plus one, 0 when not pinned */
    uint64_t seq;       /* binding: creation order (the notification order); set once by the constructor */
};

/* ------------------------------------------------------------------ document state */

/* The document's mutation state, created on demand. embi_mut_peek returns nullptr when there is none. */
embi_mut *embi_mut_get(embi_document *doc);
embi_mut *embi_mut_peek(const embi_document *doc);
/* True when some MutationObserver observes the document (the DOM skips all record work otherwise). */
bool embi_mut_watched(const embi_node *node);
/* The signal: called once when the first record of a batch is queued. The binding enqueues the microtask that calls
 * embi_mut_take_active. The signal runs inside a DOM change, so it must only queue work. */
void embi_mut_set_signal(embi_document *doc, void (*fn)(void *user), void *user);
/* A slot for the binding's own observer registry (ResizeObserver, IntersectionObserver). free_fn releases it when the
 * document is freed. */
void *embi_mut_ext(const embi_document *doc);
void embi_mut_set_ext(embi_document *doc, void *p, void (*free_fn)(void *p));
/* Frees the mutation state of the document (its records and observations; the observer objects stay with their
 * wrappers). Called by embi_document_free. */
void embi_mut_free(embi_document *doc);

/* Holds (see the header comment). embi_mut_hold marks n as referenced by the binding (an observation). embi_mut_touch
 * records that the binding dropped a reference, so the next embi_mut_release has work to do. */
void embi_mut_hold(embi_node *n);
void embi_mut_touch(embi_document *doc);
/* Reports one live reference of the binding to embi_mut_release. */
typedef void (*embi_mut_mark_fn)(void *mctx, embi_node *n);
/* Reports every reference of the binding (ctx is the binding's own pointer): calls mark once per node it refers to. */
typedef void (*embi_mut_refs_fn)(void *ctx, embi_mut_mark_fn mark, void *mctx);
/* Releases the holds that nothing refers to any more (see the header comment). extra may be nullptr. Does nothing unless
 * the document was touched since the last call. */
void embi_mut_release(embi_document *doc, embi_mut_refs_fn extra, void *ctx);

/* ------------------------------------------------------------------ observers */

/* Observes target with opts (copied). An observer belongs to one document (the one of its first observation; observing
 * a node of another document is a caller error, asserted). Returns true when this is the observer's first observation (the binding then pins
 * the wrapper). Re-observing a target replaces its options. */
bool embi_mo_observe(embi_mo *mo, embi_document *doc, embi_node *target, const embi_mo_opts *opts);
/* disconnect(): removes every observation, transient registration and queued record. Returns true when the observer was
 * observing (the binding then releases the pin). */
bool embi_mo_disconnect(embi_mo *mo);
/* Disconnects every observer of doc for which pred returns true. The binding uses it at context teardown, before the
 * wrappers (and so the observer states) are freed. */
void embi_mut_detach_if(embi_document *doc, bool (*pred)(embi_mo *mo, void *ctx), void *ctx);
bool embi_mo_observing(const embi_mo *mo);
/* takeRecords(): moves the queued records out (exact-size array, caller frees with embi_mrec_free_all). */
uint32_t embi_mo_take_records(embi_mo *mo, embi_mrec **out);
/* Frees n records and the array (the array was allocated with exactly n entries). */
void embi_mrec_free_all(embi_mrec *recs, uint32_t n);
/* The notification step: returns the observers with queued records in creation order (mo->seq, like the spec's list of
 * all MutationObservers); exact-size array, the caller frees it with embi_free(out, n * sizeof). Clears the active list
 * and, as the DOM Standard's notify step does, the transient registrations of EVERY observer (also of those without
 * records), and re-arms the signal. */
uint32_t embi_mut_take_active(embi_document *doc, embi_mo ***out);

/* ------------------------------------------------------------------ hooks for the DOM */

/* Nodes removed from parent (each one, in order), before the record is queued: adds the transient registrations of
 * the removed subtrees (always, also when the record itself is not queued). */
void embi_mut_removed(embi_node *parent, embi_node *const *nodes, uint32_t n);
/* One childList record for parent: added is the run first..last (siblings linked through next), removed is an array.
 * prev and next are the removed or inserted nodes' siblings (nullptr when none). Does nothing when there is no record
 * to queue. */
void embi_mut_childlist(embi_node *parent, embi_node *first_added, embi_node *last_added,
                        embi_node *const *removed, uint32_t nremoved, embi_node *prev, embi_node *next);
/* An attribute of el changed. old is the old value when had_old (an added attribute has none). */
void embi_mut_attribute(embi_element *el, uint8_t ns, embi_atom ns_uri, embi_atom local, embi_dstr old,
                        bool had_old);
/* Character data of node is about to change: old is its current value (copied at once). */
void embi_mut_chardata(embi_node *node, embi_dstr old, bool had_old);
