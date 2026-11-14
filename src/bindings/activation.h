#pragma once
/*
 * Activation behaviors of elements (W5b). W5d and W5e register one per
 * element type (a, button, label, input checkbox/radio, ...); embi_event_dispatch runs them for
 * click events (MouseEvent, type "click"): legacy-pre-activation before the listeners, then the
 * activation when the event was not canceled, else legacy-canceled-activation. The activation
 * target is the event target when it has a behavior, else the nearest ancestor with one when the
 * click bubbles.
 */
#include "bindings/bind.h"

typedef struct embi_activation {
    /* Optional: does this element (of the registered tag) have the behavior right now (an anchor needs
     * an href, a button must not be disabled, an input must be a checkbox or radio)? */
    bool (*applies)(embi_bind_ctx *c, embi_node *el);
    /* Optional: legacy-pre-activation (checkbox and radio toggle their checkedness here). */
    void (*pre)(embi_bind_ctx *c, embi_node *el, embi_bind_obj *event);
    /* The activation behavior itself. */
    void (*run)(embi_bind_ctx *c, embi_node *el, embi_bind_obj *event);
    /* Optional: legacy-canceled-activation (checkbox and radio restore their checkedness). */
    void (*canceled)(embi_bind_ctx *c, embi_node *el, embi_bind_obj *event);
} embi_activation;

/* Registers (or replaces) the behavior of HTML elements with this lowercase local name. The
 * structure must outlive its registration. Returns false when the table is full. Process-wide. */
bool embi_bind_register_activation(const char *tag, const embi_activation *a);
/* Removes all registrations (tests). */
void embi_bind_clear_activations(void);
