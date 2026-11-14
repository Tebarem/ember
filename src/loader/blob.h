#pragma once
/*
 * Blob core (W7c): immutable byte blobs with a media type, the Blob
 * slice rules, and the blob: URL registry that URL.createObjectURL and URL.revokeObjectURL use. C
 * only: no JS header. The JS-facing Blob and File are in src/bindings/impl_loader_blob.c.
 *
 * A blob is a window (off, size) over shared bytes, so slice does not copy. Blobs are reference
 * counted: embi_blob_release frees the bytes with the last holder.
 */
#include "base/base.h"

#include "loader/random.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct embi_blob {
    uint32_t refs;
    uint8_t *data;   /* shared bytes (embi_alloc), nullptr when the store is empty */
    size_t alloc;    /* bytes behind data (0 for a slice, whose root owns them) */
    struct embi_blob *root; /* a slice's owner (retained), nullptr for a blob that owns its bytes */
    size_t off;      /* the window of this blob */
    size_t size;
    embi_sb type;    /* the normalized media type, "" when none */
} embi_blob;

/* One piece of a blob's content: bytes (copied), or the bytes of another blob (copied). */
typedef struct embi_blob_part {
    const uint8_t *bytes; /* used when blob is nullptr */
    size_t len;
    const embi_blob *blob;
} embi_blob_part;

/* The media type rule of Blob (the type is lowercase, and empty when it has a code point outside U+0020..U+007E).
 * Appends the result to out. */
void embi_blob_type_normalize(embi_str in, embi_sb *out);
/* A blob of the parts concatenated in order, with the media type (normalized). Never nullptr except when out of
 * memory, which aborts. */
embi_blob *embi_blob_new(const embi_blob_part *parts, uint32_t nparts, embi_str type);
/* The Blob.slice of a blob: start and end are the (possibly negative) relative indices of the spec (the
 * caller passes the size for an omitted end). The slice shares the bytes and retains the root blob. */
embi_blob *embi_blob_slice(const embi_blob *b, int64_t start, int64_t end, embi_str type);
embi_blob *embi_blob_retain(embi_blob *b);
void embi_blob_release(embi_blob *b);
/* The bytes of the window, and their count. Empty blobs give nullptr. */
const uint8_t *embi_blob_bytes(const embi_blob *b, size_t *len);

/* The blob: URL registry of one origin set (the runtime's). Entries hold a reference to their blob. */
typedef struct embi_blob_url_entry {
    embi_sb url;          /* "blob:" + origin + "/" + uuid */
    embi_blob *blob;
} embi_blob_url_entry;

typedef struct embi_blob_urls {
    EMBI_VEC(embi_blob_url_entry) entries;
} embi_blob_urls;

/* Creates a blob URL for b under origin (the document's serialized origin, for example "app://bundle"),
 * from the random source rng. Writes the URL to out. origin is the caller's: the binding passes the origin of the
 * realm, which is the only origin a document can name. Memory aborts on failure, so this cannot fail. The registry
 * holds one entry per live URL (revoke removes it); resolve scans them, which is fine for the few URLs an app makes. */
void embi_blob_url_create(embi_blob_urls *reg, embi_rng *rng, embi_str origin, embi_blob *b, embi_sb *out);
/* The blob a blob URL names (the fragment is ignored), or nullptr. The blob is borrowed. */
embi_blob *embi_blob_url_resolve(const embi_blob_urls *reg, embi_str url);
/* Removes the entry of a blob URL (URL.revokeObjectURL). Other URLs are left alone. */
void embi_blob_url_revoke(embi_blob_urls *reg, embi_str url);
void embi_blob_urls_free(embi_blob_urls *reg);
