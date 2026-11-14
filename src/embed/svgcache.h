#pragma once
#ifndef EMBER_SVG
#error "EMBER_SVG must be defined to 0 or 1 (CMakeLists.txt adds it for every target)"
#endif
/*
 * The SVG images of a view (R4c): documents fetched for <img src=x.svg>, CSS url(x.svg) and the like, parsed once and kept
 * by URL. This header does not include the GPU or style headers, so the renderer's file (gfx.c) can use it.
 */
#include "base/base.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct embi_svg_image embi_svg_image;
typedef struct embi_style_engine embi_style_engine;
typedef struct embi_svgcache embi_svgcache;

#if !EMBER_SVG
/* EMBER_SVG=OFF (SZ3b): no SVG image is ever parsed or found; the cache is a null object. */
static inline embi_svgcache *embi_svgcache_create(void) { return nullptr; }
static inline void embi_svgcache_destroy(embi_svgcache *c) { (void)c; }
static inline const embi_svg_image *embi_svgcache_get(const embi_svgcache *c, embi_str url) { (void)c; (void)url; return nullptr; }
static inline bool embi_svgcache_add(embi_svgcache *c, embi_style_engine *se, embi_str url, const uint8_t *bytes, size_t len)
{
    (void)c; (void)se; (void)url; (void)bytes; (void)len;
    return false;
}
static inline bool embi_svgcache_size(const embi_svgcache *c, embi_str url, float *w, float *h, bool *has_ratio)
{
    (void)c; (void)url; (void)w; (void)h; (void)has_ratio;
    return false;
}
static inline size_t embi_svgcache_bytes(const embi_svgcache *c) { (void)c; return 0; }
static inline void embi_svgcache_trim(embi_svgcache *c) { (void)c; }
#else
embi_svgcache *embi_svgcache_create(void);
/* Frees the images; the style engine they were made with must still be alive. */
void embi_svgcache_destroy(embi_svgcache *c);
/* The image of an absolute URL, or nullptr when it is not an SVG image (or was never added). */
const embi_svg_image *embi_svgcache_get(const embi_svgcache *c, embi_str url);
/* If bytes are an SVG document, parses and stores them under url and returns true. */
bool embi_svgcache_add(embi_svgcache *c, embi_style_engine *se, embi_str url, const uint8_t *bytes, size_t len);
/* The natural size of the image of url. False when there is none. */
bool embi_svgcache_size(const embi_svgcache *c, embi_str url, float *w, float *h, bool *has_ratio);
size_t embi_svgcache_bytes(const embi_svgcache *c);
/* Drops the least recently used images until the cache is within its budget. Only where no paint holds an image of the
 * cache (adding never drops anything, so an image being painted stays alive while it adds nested ones). */
void embi_svgcache_trim(embi_svgcache *c);
#endif
