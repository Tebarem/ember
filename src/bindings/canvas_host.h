#pragma once
/*
 * The canvas hooks of a binding context (R4b): the embedder gives the GPU manager, the text system and the repaint and
 * image hooks once per context; HTMLCanvasElement.getContext('2d') builds its canvas from them. Without an install the
 * contexts of that realm draw nothing (no pixels), which is what a document without a view has.
 */
#include "bindings/bind.h"
#include "canvas/canvas.h"

#if EMBER_CANVAS
void embi_canvas_bind_install(embi_bind_ctx *c, const embi_canvas_env *env);
#endif
