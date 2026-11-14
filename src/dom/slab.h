#pragma once
/*
 * Refcounted size-class slab. Each document owns one; every DOM allocation (nodes, text and
 * attribute payloads, attribute blocks) comes from a slab and is freed back to it individually.
 *
 * Every block carries a 16-byte header just before its payload that records the owning slab and
 * the block size. embi_slab_free and embi_slab_owner need only the payload pointer. Small blocks
 * (<= 512 bytes including the header) come from 64 KiB chunks through per-class free lists. Larger
 * blocks are individual embi_alloc calls linked into the slab so the slab can free them.
 *
 * A slab is kept alive by a reference from its document and by a reference from every other
 * slab that holds a node or payload adopted from it (embi_slab_keep). Releasing the last reference
 * frees all chunks and blocks and then releases the slabs it keeps.
 */
#include "base/base.h"

typedef struct embi_slab embi_slab;

/* Returns a slab with one reference. */
embi_slab *embi_slab_new(void);
void embi_slab_retain(embi_slab *s);
/* Frees the slab when the last reference goes. */
void embi_slab_release(embi_slab *s);
/* dst keeps src alive until dst is freed. Idempotent. No-op when dst == src. */
void embi_slab_keep(embi_slab *dst, embi_slab *src);
/* Releases every slab that s keeps and forgets them. Used when the document that owns s is freed,
 * so that two documents holding each other's nodes do not keep each other alive forever. */
void embi_slab_drop_kept(embi_slab *s);
/* Returns uninitialized memory of at least `size` bytes. Never nullptr. The payload sits 16 bytes after
 * an embi_alloc block, so it is at least 8-byte aligned and is 16-byte aligned only where
 * embi_alloc is. */
[[nodiscard]] void *embi_slab_alloc(embi_slab *s, size_t size);
/* Frees a payload returned by embi_slab_alloc from any slab. nullptr is ignored. */
void embi_slab_free(void *p);
/* Usable payload bytes of a block (at least the size requested). */
size_t embi_slab_capacity(const void *p);
/* The slab that owns a payload. */
embi_slab *embi_slab_owner(const void *p);
/* Bytes and blocks outstanding in this slab, headers included. */
size_t embi_slab_live_bytes(const embi_slab *s);
size_t embi_slab_live_blocks(const embi_slab *s);
/* Number of slabs this slab keeps alive. */
size_t embi_slab_kept_count(const embi_slab *s);
