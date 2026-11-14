#pragma once
/*
 * iframes (IF1): every iframe of a view's document shows a document of its own, held by a child view
 * (an emb_view that shares its root view's device and renderer and draws into a texture of its own). frames.c.
 *
 * A child view is a whole view: its own embi_doc, realm, event loop, style, layout, display lists, damage, scroll state,
 * input state and network loads. The root view updates its frames inside its own update (after its layout, so the iframes'
 * boxes are known: they are the frames' viewports), draws them into their textures before it draws itself, and its display
 * list shows each frame as an image item named "ember-frame:<iframe element>" that the renderer draws from the frame's
 * texture. Input is routed down to the frame under the pointer (input.c), keys to the focused frame.
 *
 * Windows are known to the other realms only by their window id (a new id for every document a view loads): the bindings'
 * remote window proxies hold the id, and postMessage looks the id up when it posts and again when the task runs.
 */
#include <ember/ember.h>

#include "base/base.h"
#include "paint/damage.h"
#include "paint/dl.h"

#include <stdbool.h>
#include <stdint.h>

#include <SDL3/SDL_gpu.h>

struct embi_element;
struct embi_bind_ctx;

/* A frame deeper than this (the top document is depth 0) gets an empty document. */
#define EMBI_FRAMES_MAX_DEPTH 8u
#define EMBI_FRAMES_TOUCHES 10u

typedef struct embi_frames embi_frames;

/* The input routing state of a view over its frames (input.c reads and writes it). */
typedef struct embi_frame_route {
    emb_view *hover;   /* the frame the mouse pointer is in (its own routing goes on below it), nullptr: none */
    emb_view *press;   /* the frame a button went down in: moves and releases go there until the last button is up */
    emb_view *focus;   /* the frame that has the keyboard (the parent's focused element is its iframe) */
    struct {
        uint64_t id;
        emb_view *kid;
    } touch[EMBI_FRAMES_TOUCHES]; /* the fingers that went down in a frame (kid nullptr: free slot) */
} embi_frame_route;

/* ------------------------------------------------------------------ lifetime and tree */

/* Makes v (a new child view) a frame of parent (its iframe is set by frames.c afterwards). False when out of memory. */
bool embi_frames_adopt(emb_view *v, emb_view *parent);
/* Destroys the frames of v (their views, recursively) and v's frame state. emb_view_destroy calls it first. */
void embi_frames_destroy(emb_view *v);
/* The parent view of a frame, nullptr for a top-level view. */
emb_view *embi_frames_parent(const emb_view *v);
/* The top-level view of v's tree (v itself at the top). */
emb_view *embi_frames_root(emb_view *v);
/* The iframe element of a frame in its parent's document; nullptr at the top or once the iframe left the document. */
struct embi_element *embi_frames_element(const emb_view *v);
/* The routing state of v (made on first use; nullptr when out of memory). */
embi_frame_route *embi_frames_route(emb_view *v);
/* Whether v has frames at all (a quick test for the input layer). */
bool embi_frames_any(const emb_view *v);
/* The i-th frame of v (in the order the frames were made), nullptr past the last. */
emb_view *embi_frames_child(const emb_view *v, uint32_t i);
/* Whether the frame is live: not a frame, or a frame whose iframe is still in its parent's document. */
bool embi_frames_live(const emb_view *v);

/* ------------------------------------------------------------------ the document's lifecycle */

/* A document is about to replace v's: the frames of the old one go, and the new one gets a new window id. */
void embi_frames_doc_begin(emb_view *v);
/* v's document has been parsed: its iframes are found, and later insertions, removals and src changes are watched. */
void embi_frames_doc_ready(emb_view *v);
/* v's document is complete (its load event has fired, or it had none to fire): the iframe of a frame gets its load event,
 * from a task of the parent's loop. */
void embi_frames_doc_complete(emb_view *v);
/* A navigation from inside a frame (location, a link): the frame loads url at the next update. False for a top-level view. */
bool embi_frames_navigate(emb_view *v, embi_str url);

/* ------------------------------------------------------------------ update and draw */

/* The frames step of v's update, after its layout: the frames of removed iframes are destroyed, new iframes get frames,
 * pending loads run, each frame's viewport follows its iframe's content box, and each frame is updated (embi_view_update).
 * Returns true when the picture of a frame changed (embi_frames_add_damage then damages its iframe). */
bool embi_frames_update(emb_view *v);
/* Adds to d the rects of the items of cur that show a frame whose picture changed in the last update. */
void embi_frames_add_damage(emb_view *v, embi_damage *d, const embi_dl *cur);
/* Draws the frames of v whose picture changed (and theirs, first) into their textures, recording into cmd. Called before v
 * draws itself, which samples them. */
void embi_frames_draw(emb_view *v, SDL_GPUCommandBuffer *cmd);
/* Whether a frame of v animates (wants the next frame). */
bool embi_frames_animating(emb_view *v);
/* The earliest due timer of v's frames (false when none), and whether one of them has a task waiting. */
bool embi_frames_next_due(emb_view *v, double *due_ms);
bool embi_frames_has_tasks(emb_view *v);

/* ------------------------------------------------------------------ input */

/* The frame whose iframe is the topmost hit at the viewport point (x, y) of v, with the point in that frame's viewport
 * (lx, ly). nullptr when the point is not in a frame. */
emb_view *embi_frames_at(emb_view *v, float x, float y, float *lx, float *ly);
/* Maps the point (x, y) of frame v's viewport into its root's viewport (the frames' boxes, their transforms taken as a
 * scale of the box). False when an iframe on the way has no box. v itself at the top. */
bool embi_frames_to_root(emb_view *v, float x, float y, float *rx, float *ry);
/* Maps the point (x, y) of a frame's parent viewport into the frame's viewport (for a captured drag, which may leave the box).
 * False when the iframe has no box. */
bool embi_frames_map(emb_view *kid, float x, float y, float *lx, float *ly);
/* A frame view is about to go: forgets it in the routing state of v (and of v's ancestors). */
void embi_frames_route_forget(emb_view *v, emb_view *kid);

/* ------------------------------------------------------------------ bindings */

/* Installs the frames hooks (contentWindow, parent, top, the remote windows' postMessage) in a realm of v. */
void embi_frames_bind(struct embi_bind_ctx *c, emb_view *v);
