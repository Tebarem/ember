/*
 * Web Storage backend (W7b). See storage.h for the contract and the file format notes.
 *
 * Layout: a store keeps its entries in one vector in insertion order, and a linear-probing index
 * (slot holds entry index + 1, 0 is empty) finds them by key. The index holds only live entries.
 * Removal deletes the index slot with backward-shift deletion (no tombstone in the index, so probe
 * runs stay short) and turns the vector entry into a tombstone (mem is nullptr). Tombstones are
 * dropped by a compaction once they outnumber the live entries, so removal is O(1) on average and
 * the vector never holds more than twice the live count. Set and get are O(1) on average.
 * Storage.key(i) is O(1) while the vector has no tombstone and walks it otherwise, so a script that
 * enumerates keys after many removals pays a scan per call until the next compaction.
 *
 * File format (little-endian): "EMLS", u32 version (1), u32 origin byte length, origin bytes,
 * u32 entry count, then per entry: u32 key units, key units (u16 each), u32 value units, value
 * units; then a u64 checksum of every byte before it (embi_hash_bytes, seed EMBI_STORAGE_SEED).
 */
#include "loader/storage.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_storage.h>
#include <SDL3/SDL_timer.h>

#include <string.h>

#define EMBI_STORAGE_MAGIC "EMLS"
#define EMBI_STORAGE_VERSION 1u
#define EMBI_STORAGE_SEED 0x4c53'5442u
#define EMBI_STORAGE_MAX_FILE ((uint64_t)1 << 30)
/* How long embi_storage_open waits for an asynchronous user storage container to become ready. */
#define EMBI_STORAGE_READY_MS 2000u
/* Tombstones below this count never trigger a compaction, so small stores stay simple. */
#define EMBI_STORAGE_COMPACT_MIN 16u
/* Bytes of a file that are not entries: magic, version, origin length, count, checksum. */
#define EMBI_STORAGE_FIXED_BYTES (4u + 4u + 4u + 4u + 8u)

typedef struct embi_st_entry {
    uint16_t *mem; /* key units, then value units; nullptr for a tombstone */
    uint32_t klen;
    uint32_t vlen;
    uint32_t hash;
} embi_st_entry;

struct embi_storage_area {
    embi_storage *owner; /* nullptr for a session area */
    char *origin;        /* local areas: the origin bytes, allocated max(origin_len, 1) */
    uint32_t origin_len;
    EMBI_VEC(embi_st_entry) entries; /* insertion order, tombstones included */
    uint32_t live;                   /* entries that are not tombstones */
    uint32_t *slots; /* index: entry index + 1, 0 = empty; slot_cap is a power of two or 0 */
    uint32_t slot_cap;
    uint64_t units;
    bool dirty;
    bool persistent;
};

struct embi_storage {
    SDL_Storage *sdl; /* nullptr: memory-only */
    EMBI_VEC(embi_storage_area *) areas; /* local areas only */
    bool dirty;                          /* any area has unsaved changes */
    uint64_t dirty_since;                /* SDL_GetTicks of the first unsaved change */
};

/* ------------------------------------------------------------------ keys and entries */

static const char k_no_bytes[1] = {0};

static uint32_t units_hash(const uint16_t *p, uint32_t n)
{
    const void *bytes = n != 0 ? (const void *)p : (const void *)k_no_bytes;
    return (uint32_t)embi_hash_bytes(bytes, (size_t)n * 2u, EMBI_STORAGE_SEED);
}

static bool units_eq(const uint16_t *a, uint32_t an, const uint16_t *b, uint32_t bn)
{
    if (an != bn) {
        return false;
    }
    return an == 0 || memcmp(a, b, (size_t)an * 2u) == 0;
}

static size_t entry_bytes(uint32_t klen, uint32_t vlen)
{
    size_t n = (size_t)klen + vlen;
    return (n != 0 ? n : 1) * sizeof(uint16_t);
}

static embi_st_entry entry_make(const uint16_t *k, uint32_t kl, const uint16_t *v, uint32_t vl,
                                uint32_t h)
{
    embi_st_entry e;
    e.mem = embi_alloc(entry_bytes(kl, vl));
    e.klen = kl;
    e.vlen = vl;
    e.hash = h;
    if (kl != 0) {
        memcpy(e.mem, k, (size_t)kl * 2u);
    }
    if (vl != 0) {
        memcpy(e.mem + kl, v, (size_t)vl * 2u);
    }
    return e;
}

/* Frees the entry's memory and leaves a tombstone (or an empty slot after a clear). */
static void entry_kill(embi_st_entry *e)
{
    if (e->mem != nullptr) {
        embi_free(e->mem, entry_bytes(e->klen, e->vlen));
    }
    e->mem = nullptr;
    e->klen = 0;
    e->vlen = 0;
    e->hash = 0;
}

/* ------------------------------------------------------------------ index */

static void index_insert(embi_storage_area *a, uint32_t idx)
{
    uint32_t mask = a->slot_cap - 1;
    uint32_t i = a->entries.data[idx].hash & mask;
    while (a->slots[i] != 0) {
        i = (i + 1) & mask;
    }
    a->slots[i] = idx + 1;
}

/* Rebuilds the index with cap slots (a power of two, at least twice the live count). Tombstones
 * are skipped. */
static void index_rebuild(embi_storage_area *a, uint32_t cap)
{
    if (a->slots != nullptr) {
        embi_free(a->slots, (size_t)a->slot_cap * sizeof(uint32_t));
    }
    a->slots = embi_calloc(cap, sizeof(uint32_t));
    a->slot_cap = cap;
    for (uint32_t i = 0; i < a->entries.len; i++) {
        if (a->entries.data[i].mem != nullptr) {
            index_insert(a, i);
        }
    }
}

/* Returns the entry index, or -1 when absent. */
static int64_t area_find(const embi_storage_area *a, const uint16_t *k, uint32_t kl, uint32_t h)
{
    if (a->slot_cap == 0) {
        return -1;
    }
    uint32_t mask = a->slot_cap - 1;
    uint32_t i = h & mask;
    for (;;) {
        uint32_t s = a->slots[i];
        if (s == 0) {
            return -1;
        }
        const embi_st_entry *e = &a->entries.data[s - 1];
        if (e->hash == h && units_eq(e->mem, e->klen, k, kl)) {
            return (int64_t)(s - 1);
        }
        i = (i + 1) & mask;
    }
}

/* Deletes the index slot at i and shifts the later members of its probe run back into the gap, so
 * every remaining key is still reachable from its home slot (backward-shift deletion). */
static void index_delete_slot(embi_storage_area *a, uint32_t i)
{
    uint32_t mask = a->slot_cap - 1;
    uint32_t j = i;
    for (;;) {
        a->slots[i] = 0;
        for (;;) {
            j = (j + 1) & mask;
            if (a->slots[j] == 0) {
                return;
            }
            uint32_t home = a->entries.data[a->slots[j] - 1].hash & mask;
            /* The member at j stays when its home lies cyclically in (i, j]. */
            bool stays = i <= j ? (i < home && home <= j) : (i < home || home <= j);
            if (!stays) {
                break;
            }
        }
        a->slots[i] = a->slots[j];
        i = j;
    }
}

/* The index slot that holds entry idx (idx must be live). */
static uint32_t index_slot_of(const embi_storage_area *a, uint32_t idx)
{
    uint32_t mask = a->slot_cap - 1;
    uint32_t i = a->entries.data[idx].hash & mask;
    while (a->slots[i] != idx + 1) {
        i = (i + 1) & mask;
    }
    return i;
}

/* ------------------------------------------------------------------ area mutation */

/* Drops the tombstones (insertion order is kept) and rebuilds the index for the live entries. */
static void area_compact(embi_storage_area *a)
{
    uint32_t w = 0;
    for (uint32_t i = 0; i < a->entries.len; i++) {
        if (a->entries.data[i].mem != nullptr) {
            if (w != i) {
                a->entries.data[w] = a->entries.data[i];
            }
            w++;
        }
    }
    a->entries.len = w;
    uint32_t cap = EMBI_STORAGE_COMPACT_MIN;
    while ((uint64_t)w * 2u > cap) {
        cap *= 2u;
    }
    index_rebuild(a, cap);
}

/* Inserts or replaces without a quota check (the quota is checked by embi_storage_area_set). */
static void area_store(embi_storage_area *a, const uint16_t *k, uint32_t kl, const uint16_t *v,
                       uint32_t vl)
{
    uint32_t h = units_hash(k, kl);
    int64_t idx = area_find(a, k, kl, h);
    if (idx >= 0) {
        embi_st_entry *e = &a->entries.data[idx];
        a->units -= (uint64_t)e->klen + e->vlen;
        embi_st_entry ne = entry_make(k, kl, v, vl, h); /* k and v may point into e->mem */
        entry_kill(e);
        *e = ne;
    } else {
        embi_st_entry ne = entry_make(k, kl, v, vl, h);
        embi_vec_reserve(&a->entries, a->entries.len + 1);
        a->entries.data[a->entries.len++] = ne;
        a->live++;
        if (a->slot_cap == 0 || (uint64_t)a->live * 2u > a->slot_cap) {
            uint32_t cap = EMBI_STORAGE_COMPACT_MIN;
            while ((uint64_t)a->live * 2u > cap) {
                cap *= 2u;
            }
            index_rebuild(a, cap);
        } else {
            index_insert(a, a->entries.len - 1);
        }
    }
    a->units += (uint64_t)kl + vl;
}

/* Removes the live entry idx: its index slot goes, its vector entry becomes a tombstone. */
static void area_erase(embi_storage_area *a, uint32_t idx)
{
    index_delete_slot(a, index_slot_of(a, idx));
    embi_st_entry *e = &a->entries.data[idx];
    a->units -= (uint64_t)e->klen + e->vlen;
    entry_kill(e);
    a->live--;
    uint32_t dead = a->entries.len - a->live;
    if (dead > EMBI_STORAGE_COMPACT_MIN && dead > a->live) {
        area_compact(a);
    }
}

static void area_clear_entries(embi_storage_area *a)
{
    for (uint32_t i = 0; i < a->entries.len; i++) {
        entry_kill(&a->entries.data[i]);
    }
    a->entries.len = 0;
    a->live = 0;
    if (a->slots != nullptr) {
        memset(a->slots, 0, (size_t)a->slot_cap * sizeof(uint32_t));
    }
    a->units = 0;
}

static void mark_dirty(embi_storage_area *a)
{
    if (!a->persistent) {
        return;
    }
    embi_storage *st = a->owner;
    a->dirty = true;
    if (!st->dirty) {
        st->dirty = true;
        st->dirty_since = SDL_GetTicks();
    }
}

static embi_storage_area *area_alloc(embi_storage *owner)
{
    embi_storage_area *a = embi_calloc(1, sizeof *a);
    a->owner = owner;
    return a;
}

static void area_free(embi_storage_area *a)
{
    area_clear_entries(a);
    embi_vec_free(&a->entries);
    if (a->slots != nullptr) {
        embi_free(a->slots, (size_t)a->slot_cap * sizeof(uint32_t));
    }
    if (a->origin != nullptr) {
        embi_free(a->origin, a->origin_len != 0 ? a->origin_len : 1);
    }
    embi_free(a, sizeof *a);
}

/* ------------------------------------------------------------------ file encoding */

/* A cursor over a buffer that is sized exactly beforehand (see file_bytes). */
typedef struct embi_st_writer {
    unsigned char *p;
    size_t pos;
} embi_st_writer;

static void wr_bytes(embi_st_writer *w, const void *p, size_t n)
{
    if (n != 0) {
        memcpy(w->p + w->pos, p, n);
        w->pos += n;
    }
}

static void wr_u32(embi_st_writer *w, uint32_t v)
{
    unsigned char b[4] = {(unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16),
                          (unsigned char)(v >> 24)};
    wr_bytes(w, b, sizeof b);
}

static void wr_u64(embi_st_writer *w, uint64_t v)
{
    unsigned char b[8];
    for (int i = 0; i < 8; i++) {
        b[i] = (unsigned char)(v >> (8 * i));
    }
    wr_bytes(w, b, sizeof b);
}

/* Writes n code units as little-endian pairs in one pass. */
static void wr_units(embi_st_writer *w, const uint16_t *p, uint32_t n)
{
    unsigned char *o = w->p + w->pos;
    for (uint32_t i = 0; i < n; i++) {
        o[2 * (size_t)i] = (unsigned char)(p[i] & 0xFFu);
        o[2 * (size_t)i + 1] = (unsigned char)(p[i] >> 8);
    }
    w->pos += (size_t)n * 2u;
}

typedef struct embi_st_reader {
    const unsigned char *p;
    size_t end;
    size_t pos;
} embi_st_reader;

static bool rd_u32(embi_st_reader *r, uint32_t *out)
{
    if (r->end - r->pos < 4) {
        return false;
    }
    const unsigned char *b = r->p + r->pos;
    *out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    r->pos += 4;
    return true;
}

static bool rd_u64(embi_st_reader *r, uint64_t *out)
{
    if (r->end - r->pos < 8) {
        return false;
    }
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= (uint64_t)r->p[r->pos + (size_t)i] << (8 * i);
    }
    r->pos += 8;
    *out = v;
    return true;
}

/* Reads n code units into a fresh buffer owned by the caller (nullptr when n is 0). */
static bool rd_units(embi_st_reader *r, uint32_t n, uint16_t **out)
{
    *out = nullptr;
    if (n > (r->end - r->pos) / 2) {
        return false;
    }
    if (n == 0) {
        return true;
    }
    uint16_t *units = embi_alloc((size_t)n * 2u);
    for (uint32_t i = 0; i < n; i++) {
        const unsigned char *b = r->p + r->pos + (size_t)i * 2u;
        units[i] = (uint16_t)(b[0] | (b[1] << 8));
    }
    r->pos += (size_t)n * 2u;
    *out = units;
    return true;
}

static void area_file_name(const embi_storage_area *a, char out[40])
{
    const void *bytes = a->origin_len != 0 ? (const void *)a->origin : (const void *)k_no_bytes;
    SDL_snprintf(out, 40, "ls-%016llx.bin",
                 (unsigned long long)embi_hash_bytes(bytes, a->origin_len, EMBI_STORAGE_SEED));
}

/* The exact size of the file image of a: the fixed part, the origin and every live entry. */
static size_t file_bytes(const embi_storage_area *a)
{
    size_t n = EMBI_STORAGE_FIXED_BYTES + a->origin_len;
    for (uint32_t i = 0; i < a->entries.len; i++) {
        const embi_st_entry *e = &a->entries.data[i];
        if (e->mem != nullptr) {
            n += 4u + 4u + ((size_t)e->klen + e->vlen) * 2u;
        }
    }
    return n;
}

static bool area_write(embi_storage *st, embi_storage_area *a)
{
    char name[40];
    char tmp[48];
    area_file_name(a, name);
    SDL_snprintf(tmp, sizeof tmp, "%s.tmp", name);

    size_t total = file_bytes(a);
    unsigned char *buf = embi_alloc(total);
    embi_st_writer w = {buf, 0};
    wr_bytes(&w, EMBI_STORAGE_MAGIC, 4);
    wr_u32(&w, EMBI_STORAGE_VERSION);
    wr_u32(&w, a->origin_len);
    wr_bytes(&w, a->origin, a->origin_len);
    wr_u32(&w, a->live);
    for (uint32_t i = 0; i < a->entries.len; i++) {
        const embi_st_entry *e = &a->entries.data[i];
        if (e->mem == nullptr) {
            continue;
        }
        wr_u32(&w, e->klen);
        wr_units(&w, e->mem, e->klen);
        wr_u32(&w, e->vlen);
        wr_units(&w, e->mem + e->klen, e->vlen);
    }
    /* w.pos is total - 8 here: the checksum covers every byte before it. */
    wr_u64(&w, embi_hash_bytes(buf, w.pos, EMBI_STORAGE_SEED));

    bool ok = SDL_WriteStorageFile(st->sdl, tmp, buf, total) &&
              SDL_RenameStoragePath(st->sdl, tmp, name);
    if (!ok) {
        embi_log_warn("storage: saving %s failed: %s", name, SDL_GetError());
    }
    embi_free(buf, total);
    return ok;
}

/* Parses a file image into a (empty) area. False when the image is not a valid store for this
 * origin. */
static bool area_parse(embi_storage_area *a, const unsigned char *buf, size_t n)
{
    if (n < EMBI_STORAGE_FIXED_BYTES || memcmp(buf, EMBI_STORAGE_MAGIC, 4) != 0) {
        return false;
    }
    embi_st_reader r = {buf, n - 8, 4};
    uint64_t stored = 0;
    embi_st_reader tail = {buf, n, n - 8};
    if (!rd_u64(&tail, &stored) || stored != embi_hash_bytes(buf, n - 8, EMBI_STORAGE_SEED)) {
        return false;
    }
    uint32_t version = 0;
    uint32_t origin_len = 0;
    if (!rd_u32(&r, &version) || version != EMBI_STORAGE_VERSION) {
        return false;
    }
    if (!rd_u32(&r, &origin_len) || origin_len != a->origin_len ||
        r.end - r.pos < origin_len ||
        (origin_len != 0 && memcmp(buf + r.pos, a->origin, origin_len) != 0)) {
        return false;
    }
    r.pos += origin_len;
    uint32_t count = 0;
    if (!rd_u32(&r, &count)) {
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint32_t klen = 0;
        uint32_t vlen = 0;
        uint16_t *kbuf = nullptr;
        uint16_t *vbuf = nullptr;
        if (!rd_u32(&r, &klen) || !rd_units(&r, klen, &kbuf)) {
            return false;
        }
        if (!rd_u32(&r, &vlen) || !rd_units(&r, vlen, &vbuf)) {
            if (kbuf != nullptr) {
                embi_free(kbuf, (size_t)klen * 2u);
            }
            return false;
        }
        bool dup = area_find(a, kbuf, klen, units_hash(kbuf, klen)) >= 0;
        if (!dup) {
            /* Concatenate key and value for the store call, which copies them. */
            uint16_t *both = embi_alloc(entry_bytes(klen, vlen));
            if (klen != 0) {
                memcpy(both, kbuf, (size_t)klen * 2u);
            }
            if (vlen != 0) {
                memcpy(both + klen, vbuf, (size_t)vlen * 2u);
            }
            area_store(a, both, klen, both + klen, vlen);
            embi_free(both, entry_bytes(klen, vlen));
        }
        if (kbuf != nullptr) {
            embi_free(kbuf, (size_t)klen * 2u);
        }
        if (vbuf != nullptr) {
            embi_free(vbuf, (size_t)vlen * 2u);
        }
        if (dup) {
            return false;
        }
    }
    return r.pos == r.end;
}

static void area_load(embi_storage *st, embi_storage_area *a)
{
    if (st->sdl == nullptr) {
        return;
    }
    char name[40];
    area_file_name(a, name);
    Uint64 size = 0;
    if (!SDL_GetStorageFileSize(st->sdl, name, &size)) {
        return; /* no file yet: the origin starts empty */
    }
    if (size > EMBI_STORAGE_MAX_FILE || size < EMBI_STORAGE_FIXED_BYTES) {
        embi_log_warn("storage: %s has an invalid size, starting empty", name);
        return;
    }
    size_t n = (size_t)size;
    unsigned char *buf = embi_alloc(n);
    if (!SDL_ReadStorageFile(st->sdl, name, buf, size)) {
        embi_log_warn("storage: reading %s failed: %s", name, SDL_GetError());
    } else if (!area_parse(a, buf, n)) {
        embi_log_warn("storage: %s is not a valid store, starting empty", name);
        area_clear_entries(a);
    }
    embi_free(buf, n);
}

/* ------------------------------------------------------------------ backend */

static embi_storage *storage_new(SDL_Storage *sdl)
{
    embi_storage *st = embi_calloc(1, sizeof *st);
    st->sdl = sdl;
    return st;
}

embi_storage *embi_storage_open(const char *org, const char *app)
{
    SDL_Storage *sdl = nullptr;
    if (org != nullptr && app != nullptr) {
        sdl = SDL_OpenUserStorage(org, app, 0);
        if (sdl == nullptr) {
            embi_log_warn("storage: no user storage for %s/%s (%s), using memory only", org, app,
                          SDL_GetError());
        } else {
            /* User storage can open asynchronously (console and mobile backends). Wait a bounded
             * time; when it never becomes ready, close it and run memory-only. */
            Uint64 deadline = SDL_GetTicks() + EMBI_STORAGE_READY_MS;
            while (!SDL_StorageReady(sdl) && SDL_GetTicks() < deadline) {
                SDL_Delay(1);
            }
            if (!SDL_StorageReady(sdl)) {
                embi_log_warn("storage: %s/%s is not ready after %u ms, using memory only", org,
                              app, EMBI_STORAGE_READY_MS);
                SDL_CloseStorage(sdl);
                sdl = nullptr;
            }
        }
    }
    return storage_new(sdl);
}

embi_storage *embi_storage_open_dir(const char *dir)
{
    if (dir == nullptr || !SDL_CreateDirectory(dir)) {
        return nullptr;
    }
    SDL_Storage *sdl = SDL_OpenFileStorage(dir);
    if (sdl == nullptr) {
        return nullptr;
    }
    return storage_new(sdl);
}

bool embi_storage_flush(embi_storage *st)
{
    if (st == nullptr || !st->dirty) {
        return true;
    }
    bool ok = true;
    for (uint32_t i = 0; i < st->areas.len; i++) {
        embi_storage_area *a = st->areas.data[i];
        if (a->dirty) {
            if (area_write(st, a)) {
                a->dirty = false;
            } else {
                ok = false;
            }
        }
    }
    st->dirty = !ok;
    if (!ok) {
        st->dirty_since = SDL_GetTicks(); /* retry one delay later, not on every tick */
    }
    return ok;
}

void embi_storage_tick(embi_storage *st)
{
    if (st != nullptr && st->dirty && SDL_GetTicks() - st->dirty_since >= EMBI_STORAGE_FLUSH_MS) {
        (void)embi_storage_flush(st);
    }
}

void embi_storage_close(embi_storage *st)
{
    if (st == nullptr) {
        return;
    }
    (void)embi_storage_flush(st);
    for (uint32_t i = 0; i < st->areas.len; i++) {
        area_free(st->areas.data[i]);
    }
    embi_vec_free(&st->areas);
    if (st->sdl != nullptr) {
        SDL_CloseStorage(st->sdl);
    }
    embi_free(st, sizeof *st);
}

bool embi_storage_is_persistent(const embi_storage *st)
{
    return st != nullptr && st->sdl != nullptr;
}

embi_storage_area *embi_storage_local(embi_storage *st, embi_str origin)
{
    for (uint32_t i = 0; i < st->areas.len; i++) {
        embi_storage_area *a = st->areas.data[i];
        if (a->origin_len == origin.len &&
            (origin.len == 0 || memcmp(a->origin, origin.ptr, origin.len) == 0)) {
            return a;
        }
    }
    embi_storage_area *a = area_alloc(st);
    a->origin = embi_alloc(origin.len != 0 ? origin.len : 1);
    if (origin.len != 0) {
        memcpy(a->origin, origin.ptr, origin.len);
    }
    a->origin_len = origin.len;
    a->persistent = st->sdl != nullptr;
    area_load(st, a);
    embi_vec_reserve(&st->areas, st->areas.len + 1);
    st->areas.data[st->areas.len++] = a;
    return a;
}

embi_storage_area *embi_storage_session_new(void)
{
    return area_alloc(nullptr);
}

void embi_storage_session_free(embi_storage_area *a)
{
    if (a != nullptr) {
        area_free(a);
    }
}

/* ------------------------------------------------------------------ areas */

uint32_t embi_storage_area_len(const embi_storage_area *a)
{
    return a->live;
}

/* The entry at logical position index (insertion order). Direct when there are no tombstones. */
static const embi_st_entry *area_nth_live(const embi_storage_area *a, uint32_t index)
{
    if (a->live == a->entries.len) {
        return &a->entries.data[index];
    }
    uint32_t seen = 0;
    for (uint32_t i = 0; i < a->entries.len; i++) {
        if (a->entries.data[i].mem != nullptr) {
            if (seen == index) {
                return &a->entries.data[i];
            }
            seen++;
        }
    }
    return nullptr;
}

bool embi_storage_area_key(const embi_storage_area *a, uint32_t index, embi_u16view *out)
{
    if (index >= a->live) {
        return false;
    }
    const embi_st_entry *e = area_nth_live(a, index);
    out->p = e->mem;
    out->len = e->klen;
    return true;
}

bool embi_storage_area_get(const embi_storage_area *a, embi_u16view key, embi_u16view *out)
{
    int64_t idx = area_find(a, key.p, key.len, units_hash(key.p, key.len));
    if (idx < 0) {
        return false;
    }
    const embi_st_entry *e = &a->entries.data[idx];
    out->p = e->mem + e->klen;
    out->len = e->vlen;
    return true;
}

embi_storage_status embi_storage_area_set(embi_storage_area *a, embi_u16view key,
                                          embi_u16view value)
{
    uint64_t old = 0;
    int64_t idx = area_find(a, key.p, key.len, units_hash(key.p, key.len));
    if (idx >= 0) {
        const embi_st_entry *e = &a->entries.data[idx];
        old = (uint64_t)e->klen + e->vlen;
    }
    uint64_t next = a->units - old + key.len + value.len;
    if (next > EMBI_STORAGE_QUOTA_UNITS) {
        return EMBI_STORAGE_QUOTA;
    }
    area_store(a, key.p, key.len, value.p, value.len);
    mark_dirty(a);
    return EMBI_STORAGE_OK;
}

void embi_storage_area_remove(embi_storage_area *a, embi_u16view key)
{
    int64_t idx = area_find(a, key.p, key.len, units_hash(key.p, key.len));
    if (idx >= 0) {
        area_erase(a, (uint32_t)idx);
        mark_dirty(a);
    }
}

void embi_storage_area_clear(embi_storage_area *a)
{
    if (a->entries.len != 0) {
        area_clear_entries(a);
        mark_dirty(a);
    }
}

uint64_t embi_storage_area_units(const embi_storage_area *a)
{
    return a->units;
}
