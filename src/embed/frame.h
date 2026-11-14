#pragma once
/*
 * The frame step of a document (W6c): the HTML "update the rendering" steps that belong to the animations, run by the
 * view once per frame before the style update. frame.c.
 *
 * Order (HTML event loop): the due timers and tasks (and their microtasks), then update animations and send events (the
 * timeline moves to the frame time, the animations that are pending get their start time, the queued animation and
 * transition events go out), then the requestAnimationFrame callbacks, then the style and layout update (embi_doc_update).
 * After the update, embi_doc_frame_end readies the animations that the scripts and the style update created.
 */
#include "anim/frame.h"
#include "embed/doc.h"

/* The document's animator (made with the document, always present). */
embi_animator *embi_doc_animator(embi_doc *d);
/* Creates the animator of d over its style engine, with the host hooks that restyle the document and reach the realm. */
embi_animator *embi_frame_animator_new(embi_doc *d);
/* The steps before the style update, at the current clock. Safe to call on a document with no DOM. */
void embi_doc_frame(embi_doc *d);
/* The steps after the style update. */
void embi_doc_frame_end(embi_doc *d);
/* True while the document needs another frame: a running animation or transition, or a requestAnimationFrame callback
 * waiting. An idle page returns false. */
bool embi_doc_animating(embi_doc *d);
/* CF2: one step of work ahead of need, for a frame that had nothing to do while nothing animates (embi_view_update): once
 * the document has loaded and no task waits, the realm evaluates the animation prelude when the styles declare a CSS
 * transition or animation, or collects when half its allocation budget is spent (embi_bind_idle). Returns true when it did
 * something. */
bool embi_doc_idle(embi_doc *d);
