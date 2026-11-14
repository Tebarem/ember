#pragma once
/*
 * Observers that run in the frame (W6d): ResizeObserver and IntersectionObserver. The
 * MutationObserver needs no frame step: its records are delivered in a microtask queued by the DOM changes.
 *
 * The frame calls embi_observers_after_layout(c) once per frame, after style and layout are current and before paint
 * (the rendering step of src/embed/loop.c, W6c). It delivers the ResizeObserver notifications of this frame in the
 * depth-ordered loop of the spec (callbacks may change the DOM, which relays out before the next pass), and it computes
 * the IntersectionObserver observations; their entries are delivered as a task (through the timer hooks of bind.h,
 * synchronously when the context has none). Nothing else is needed: a context with no observers returns at once.
 *
 * Frame wiring (W6c owns it, src/embed/loop.c): call this after the frame's style and layout update (embi_doc_update),
 * before paint. The ResizeObserver re-layout inside the loop restyles and lays out through the bindings' engines
 * (embi_bind_style, embi_bind_layout), because bindings cannot call embi_doc_update (src/embed depends on bindings).
 * The IntersectionObserver applies the clipping by intermediate ancestors' overflow and the scroll offsets. The remaining
 * gap: an element root clips by its padding box (the spec's content box is not used).
 */
#include "bindings/bind.h"

void embi_observers_after_layout(embi_bind_ctx *c);

/* Context teardown (called by embi_bind_free before the wrappers are freed): disconnects this context's
 * MutationObservers from the document and clears the document's signal to them. */
void embi_observers_release(embi_bind_ctx *c);
