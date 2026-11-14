#pragma once
/*
 * DOM events (W5b): the event record and the dispatch algorithm of the
 * DOM Standard, free of JavaScript. The binding (src/bindings/impl_events.c) builds the event path
 * from the tree (the ops' parent), runs the listeners of each path item (the ops' invoke) and wraps
 * records as Event objects. Platform code (W5d, W5e) fires events through the binding's helpers,
 * which call embi_event_dispatch.
 *
 * Items of an event path are opaque to this module: an object pointer and a kind chosen by the
 * binding (0: an embi_node, 1: a binding record). The path is only valid during a dispatch.
 *
 * Memory: an event is one block (embi_event plus its type string) from embi_calloc. Free it with
 * embi_event_free. The binding owns the block through its wrapper.
 */
#include "base/base.h"

enum embi_event_phase {
    EMBI_PHASE_NONE = 0,
    EMBI_PHASE_CAPTURING = 1,
    EMBI_PHASE_AT_TARGET = 2,
    EMBI_PHASE_BUBBLING = 3,
};

enum {
    EMBI_EVF_BUBBLES = 1u << 0,
    EMBI_EVF_CANCELABLE = 1u << 1,
    EMBI_EVF_COMPOSED = 1u << 2,
    EMBI_EVF_TRUSTED = 1u << 3,
    EMBI_EVF_STOP_PROPAGATION = 1u << 4,
    EMBI_EVF_STOP_IMMEDIATE = 1u << 5,
    EMBI_EVF_CANCELED = 1u << 6,
    EMBI_EVF_DISPATCHING = 1u << 7,
    EMBI_EVF_INITIALIZED = 1u << 8,
    EMBI_EVF_IN_PASSIVE = 1u << 9,
};

typedef struct embi_event_item {
    void *obj;
    uint32_t kind;
} embi_event_item;

typedef struct embi_event {
    uint32_t flags;      /* EMBI_EVF_* */
    uint8_t phase;       /* embi_event_phase, valid during a dispatch */
    uint32_t type_len;   /* bytes of type */
    double time_stamp;   /* milliseconds on the view clock */
    embi_event_item target;  /* valid during a dispatch */
    embi_event_item current; /* valid during a dispatch */
    embi_event_item *path;   /* valid during a dispatch: path[0] is the target */
    uint32_t path_len;
    char type[]; /* UTF-8 event type, not NUL-terminated */
} embi_event;

/* A new event (not dispatched) of the UTF-8 type. flags are EMBI_EVF_* (BUBBLES, CANCELABLE,
 * COMPOSED, TRUSTED); the event is marked initialized. Returns nullptr on allocation failure. */
embi_event *embi_event_new(embi_str type, uint32_t flags, double time_stamp);
/* A copy of ev with another type (initEvent). The caller frees ev with embi_event_free. */
embi_event *embi_event_retype(const embi_event *ev, embi_str type);
/* Size of the block of ev, for embi_free. */
size_t embi_event_size(const embi_event *ev);
void embi_event_free(embi_event *ev);

static inline embi_str embi_event_type(const embi_event *ev)
{
    return (embi_str){ev->type, ev->type_len};
}

/* The listener and path callbacks of a dispatch. ctx is passed to every call. */
typedef struct embi_event_ops {
    void *ctx;
    /* The parent of an item in the event path, or an item with obj nullptr when there is none.
     * Called once per item while the path is built, before any listener runs. */
    embi_event_item (*parent)(void *ctx, embi_event_item item, const embi_event *ev);
    /* Runs the listeners of item for ev's type. capture selects the capturing pass (capture
     * listeners run) or the bubbling pass (the others). ev->phase and ev->current are set. Must
     * stop when EMBI_EVF_STOP_IMMEDIATE is set on ev. */
    void (*invoke)(void *ctx, embi_event_item item, embi_event *ev, bool capture);
    /* Optional (both or neither). has_activation: does item have an activation behavior for ev? It
     * must return false unless ev is an activation event (a MouseEvent click). The target is the
     * activation target when it has one; otherwise, for a bubbling event, the first ancestor on the
     * path that has one. activation runs with EMBI_ACT_PRE (legacy-pre-activation) before the
     * passes, then after the passes (once the dispatch state is reset) with EMBI_ACT_RUN when the
     * event was not canceled, else EMBI_ACT_CANCELED (legacy-canceled-activation). The item must
     * stay valid until the dispatch returns (the binding pins it in has_activation). */
    bool (*has_activation)(void *ctx, embi_event_item item, const embi_event *ev);
    void (*activation)(void *ctx, embi_event_item item, embi_event *ev, int phase);
} embi_event_ops;

enum { EMBI_ACT_PRE = 0, EMBI_ACT_RUN = 1, EMBI_ACT_CANCELED = 2 };

/* Dispatches ev at target (the DOM Standard's dispatch algorithm, without shadow trees): the
 * capturing pass from the root down to the target, the target's listeners, then the bubbling pass
 * up to the root when the event bubbles. stopPropagation skips the remaining items and
 * stopImmediatePropagation the remaining listeners. Returns true when the event was not canceled.
 * The dispatch flag, the stop flags, the phase and the current item are reset at the end; target
 * and path_len stay as they were (the path is freed). Preconditions (not dispatching, initialized)
 * are the caller's to check. */
bool embi_event_dispatch(const embi_event_ops *ops, embi_event *ev, embi_event_item target);

/* stopPropagation and stopImmediatePropagation (which also stops propagation). */
static inline void embi_event_stop_propagation(embi_event *ev)
{
    ev->flags |= EMBI_EVF_STOP_PROPAGATION;
}
static inline void embi_event_stop_immediate(embi_event *ev)
{
    ev->flags |= EMBI_EVF_STOP_PROPAGATION | EMBI_EVF_STOP_IMMEDIATE;
}
/* preventDefault: sets canceled when the event is cancelable and no passive listener runs. */
static inline void embi_event_prevent_default(embi_event *ev)
{
    if ((ev->flags & EMBI_EVF_CANCELABLE) && !(ev->flags & EMBI_EVF_IN_PASSIVE)) {
        ev->flags |= EMBI_EVF_CANCELED;
    }
}
