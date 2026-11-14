#pragma once
/*
 * Web Storage backend (W7b): per-origin localStorage persisted through SDL_storage, and memory-only
 * sessionStorage areas. Internal API: embi_ prefix. C only: this header and storage.c include no JS
 * header.
 *
 * Keys and values are UTF-16 code unit strings, as the DOM stores them, so lone surrogates survive a
 * round trip. The quota is 5 MiB per origin, counted as the UTF-16 code units of keys plus values.
 *
 * Persistence: each local area is one file in the storage root, named from a hash of its origin.
 * The file starts with the origin, so a renamed or foreign file is rejected, and ends with a 64-bit
 * checksum. Writes go to "<name>.tmp" and are renamed over the file, so a crash leaves the previous
 * version. Writes are write-behind: the first mutation starts a timer, and embi_storage_tick flushes
 * once EMBI_STORAGE_FLUSH_MS has passed. The embedder calls tick from its loop and flush at view or
 * runtime destruction.
 *
 * Ownership: an embi_storage owns its local areas. A session area belongs to its caller and is
 * freed with embi_storage_session_free. Pointers returned by get and key stay valid until the next
 * mutation of that area. The API is single-threaded (one runtime).
 */
#include "base/base.h"

/* Per-origin quota, in UTF-16 code units of keys plus values. */
#define EMBI_STORAGE_QUOTA_UNITS ((uint64_t)5u * 1024u * 1024u)
/* Write-behind delay from the first unsaved mutation. */
#define EMBI_STORAGE_FLUSH_MS 1000u

typedef struct embi_storage embi_storage;
typedef struct embi_storage_area embi_storage_area;

/* A borrowed UTF-16 string. p may be nullptr when len is 0. */
typedef struct embi_u16view {
    const uint16_t *p;
    uint32_t len;
} embi_u16view;

typedef enum embi_storage_status {
    EMBI_STORAGE_OK,
    EMBI_STORAGE_QUOTA, /* the write would exceed the origin quota; nothing changed */
} embi_storage_status;

/* Opens a backend. org and app name an SDL user storage container (the embedder's
 * emb_runtime_desc.storage_org and storage_app). When either is nullptr, or the container cannot be
 * opened, the backend is memory-only and nothing is persisted. Never returns nullptr except when out
 * of memory, which aborts. */
[[nodiscard]] embi_storage *embi_storage_open(const char *org, const char *app);
/* Opens a backend that keeps its files in dir (created when missing). For embedders and tests that
 * must not touch the user's storage location. Returns nullptr when dir cannot be created. */
[[nodiscard]] embi_storage *embi_storage_open_dir(const char *dir);
/* Flushes every dirty area, then frees the backend and its areas. */
void embi_storage_close(embi_storage *st);
/* Writes every dirty persistent area now. Returns false when a write failed (the error is logged and
 * the area stays dirty, so the next flush retries). */
bool embi_storage_flush(embi_storage *st);
/* Write-behind timer: flushes when a mutation has been unsaved for EMBI_STORAGE_FLUSH_MS. Cheap
 * when nothing is dirty. */
void embi_storage_tick(embi_storage *st);
/* Reports whether the backend writes to SDL storage (false for memory-only backends). */
bool embi_storage_is_persistent(const embi_storage *st);

/* The localStorage area of an origin (for example "app://bundle"). Created on first use, loaded
 * from its file then. Owned by st. Never returns nullptr. */
[[nodiscard]] embi_storage_area *embi_storage_local(embi_storage *st, embi_str origin);
/* A new sessionStorage area: memory-only, never persisted, owned by the caller. */
[[nodiscard]] embi_storage_area *embi_storage_session_new(void);
void embi_storage_session_free(embi_storage_area *a);

/* The number of entries (the Storage.length attribute). */
uint32_t embi_storage_area_len(const embi_storage_area *a);
/* The index-th key in insertion order (Storage.key). False when index is out of range. */
bool embi_storage_area_key(const embi_storage_area *a, uint32_t index, embi_u16view *out);
/* Looks up a key. False when absent. */
bool embi_storage_area_get(const embi_storage_area *a, embi_u16view key, embi_u16view *out);
/* Inserts or replaces. Returns EMBI_STORAGE_QUOTA when the new total would exceed the quota. */
embi_storage_status embi_storage_area_set(embi_storage_area *a, embi_u16view key,
                                          embi_u16view value);
/* Removes a key when present (Storage.removeItem). */
void embi_storage_area_remove(embi_storage_area *a, embi_u16view key);
/* Removes every key (Storage.clear). */
void embi_storage_area_clear(embi_storage_area *a);
/* Quota in use: the sum of key and value code units. */
uint64_t embi_storage_area_units(const embi_storage_area *a);
