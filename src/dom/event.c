/*
 * DOM event records and the dispatch algorithm (W5b). See event.h.
 */
#include "dom/event.h"

#include <string.h>

size_t embi_event_size(const embi_event *ev)
{
    return sizeof(embi_event) + ev->type_len;
}

embi_event *embi_event_new(embi_str type, uint32_t flags, double time_stamp)
{
    embi_event *ev = embi_calloc(1, sizeof(embi_event) + type.len);
    if (!ev) {
        return nullptr;
    }
    ev->flags = flags | EMBI_EVF_INITIALIZED;
    ev->time_stamp = time_stamp;
    ev->type_len = type.len;
    if (type.len) {
        memcpy(ev->type, type.ptr, type.len);
    }
    return ev;
}

embi_event *embi_event_retype(const embi_event *ev, embi_str type)
{
    /* The flags and the time stamp are kept, except the dispatch state, which is clear anyway. */
    embi_event *n = embi_event_new(type, ev->flags & ~(EMBI_EVF_DISPATCHING | EMBI_EVF_STOP_PROPAGATION |
                                                       EMBI_EVF_STOP_IMMEDIATE | EMBI_EVF_IN_PASSIVE),
                                   ev->time_stamp);
    if (n) {
        n->flags |= EMBI_EVF_INITIALIZED;
    }
    return n;
}

void embi_event_free(embi_event *ev)
{
    if (ev) {
        embi_free(ev, embi_event_size(ev));
    }
}

/* Appends item to the path, growing it. Returns false on allocation failure. */
static bool path_push(embi_event *ev, embi_event_item item)
{
    if (ev->path_len % 16 == 0 || ev->path == nullptr) {
        uint32_t cap = ev->path_len + 16;
        size_t old_size = ev->path_len * sizeof(embi_event_item);
        size_t new_size = (size_t)cap * sizeof(embi_event_item);
        embi_event_item *p = embi_realloc(ev->path, old_size, new_size);
        if (!p) {
            return false;
        }
        ev->path = p;
    }
    ev->path[ev->path_len++] = item;
    return true;
}

static void path_free(embi_event *ev)
{
    size_t cap_bytes = ev->path ? ((size_t)((ev->path_len + 15) / 16 * 16)) * sizeof(embi_event_item) : 0;
    if (ev->path) {
        embi_free(ev->path, cap_bytes);
    }
    ev->path = nullptr;
    ev->path_len = 0;
}

bool embi_event_dispatch(const embi_event_ops *ops, embi_event *ev, embi_event_item target)
{
    ev->flags |= EMBI_EVF_DISPATCHING;
    ev->target = target;
    ev->current = (embi_event_item){nullptr, 0};
    ev->phase = EMBI_PHASE_NONE;

    /* The path is fixed before any listener runs: the target, then its ancestors up to the root. */
    ev->path = nullptr;
    ev->path_len = 0;
    bool ok = path_push(ev, target);
    for (embi_event_item cur = target; ok;) {
        embi_event_item p = ops->parent(ops->ctx, cur, ev);
        if (!p.obj) {
            break;
        }
        ok = path_push(ev, p);
        cur = p;
    }
    if (!ok) {
        path_free(ev);
        ev->flags &= ~(EMBI_EVF_DISPATCHING | EMBI_EVF_STOP_PROPAGATION | EMBI_EVF_STOP_IMMEDIATE);
        return !(ev->flags & EMBI_EVF_CANCELED);
    }

    const uint32_t n = ev->path_len;
    /* The activation target: the target when it has an activation behavior, else the first ancestor
     * that has one when the event bubbles. */
    embi_event_item act = {nullptr, 0};
    if (ops->has_activation && ops->activation) {
        if (ops->has_activation(ops->ctx, target, ev)) {
            act = target;
        } else if (ev->flags & EMBI_EVF_BUBBLES) {
            for (uint32_t i = 1; i < n; i++) {
                if (ops->has_activation(ops->ctx, ev->path[i], ev)) {
                    act = ev->path[i];
                    break;
                }
            }
        }
        if (act.obj) {
            ops->activation(ops->ctx, act, ev, EMBI_ACT_PRE);
        }
    }
    /* Capturing pass: ancestors from the root down (path[n-1] .. path[1]), then the target. */
    for (uint32_t i = n; i > 1 && !(ev->flags & EMBI_EVF_STOP_PROPAGATION); i--) {
        ev->phase = EMBI_PHASE_CAPTURING;
        ev->current = ev->path[i - 1];
        ops->invoke(ops->ctx, ev->current, ev, true);
    }
    if (!(ev->flags & EMBI_EVF_STOP_PROPAGATION)) {
        ev->phase = EMBI_PHASE_AT_TARGET;
        ev->current = target;
        ops->invoke(ops->ctx, target, ev, true);
    }
    /* Bubbling pass: the target, then the ancestors when the event bubbles. */
    if (!(ev->flags & EMBI_EVF_STOP_PROPAGATION)) {
        ev->phase = EMBI_PHASE_AT_TARGET;
        ev->current = target;
        ops->invoke(ops->ctx, target, ev, false);
    }
    if (ev->flags & EMBI_EVF_BUBBLES) {
        for (uint32_t i = 1; i < n && !(ev->flags & EMBI_EVF_STOP_PROPAGATION); i++) {
            ev->phase = EMBI_PHASE_BUBBLING;
            ev->current = ev->path[i];
            ops->invoke(ops->ctx, ev->current, ev, false);
        }
    }

    ev->phase = EMBI_PHASE_NONE;
    ev->current = (embi_event_item){nullptr, 0};
    ev->flags &= ~(EMBI_EVF_DISPATCHING | EMBI_EVF_STOP_PROPAGATION | EMBI_EVF_STOP_IMMEDIATE);
    /* The path lives only during the dispatch; the binding reads it through the callbacks. */
    path_free(ev);
    if (act.obj) {
        ops->activation(ops->ctx, act, ev, (ev->flags & EMBI_EVF_CANCELED) ? EMBI_ACT_CANCELED : EMBI_ACT_RUN);
    }
    return !(ev->flags & EMBI_EVF_CANCELED);
}
